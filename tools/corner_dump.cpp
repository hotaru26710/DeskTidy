#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QPixmap>
#include <QScreen>
#include <QTextStream>
#include <QTimer>

#include "appservice.h"
#include "boxmanager.h"
#include "floatingboxmanager.h"
#include "floatingboxwidget.h"

// ---------------------------------------------------------------------------
// corner_dump —— 把浮窗区域的像素 dump 成 ASCII 图。
//
// corner_diag 给出的四角像素读数自相矛盾（"上中"读到的颜色既不是标题栏灰、
// 也不是页面白），说明有可能是抓屏本身就抓错了区域。
//
// 这个程序把整个窗口区域的像素降采样成一张 ASCII 图打印出来 ——
// 本会话的模型看不了真图，但看得了一屏字符。圆角在 ASCII 图上表现为
// 四个角是"· "（背景），而边框内是"##"（窗口）。
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyCornerDump"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyCornerDump"));

    QTextStream out(stdout);
    out << "=== 浮窗区域像素 ASCII dump ===\n\n";

    const QString root = QStringLiteral("F:/QtProject/DeskTidy/_cornerdump");
    QDir(root).removeRecursively();
    QDir().mkpath(root);

    AppService service;
    QString err;
    BoxManager::ensureRoot(root + QStringLiteral("/boxes"), &err);

    StorageBox box;
    BoxManager::createBox(root + QStringLiteral("/boxes"), QStringLiteral("圆角"), &box, &err);
    {
        QFile f(box.path + QStringLiteral("/a.txt"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) { f.write("x"); f.close(); }
    }

    FloatingBoxManager mgr(&service);
    mgr.openBox(QStringLiteral("圆角"), box.path);

    // ⚠️ 等窗口**位置**稳定，不只是等动画。
    //
    // 这是本探针第一版踩的坑：只 sleep 了 700ms 就抓屏，抓到的是
    // 窗口被平台层移动到最终位置**之前**的区域 —— 读回来一片桌面灰蓝，
    // 于是把"抓错了地方"误判成"圆角没生效"，白查了两轮。
    //
    // 判据：连续几次读到的 frameGeometry 完全相同，才算位置稳定。
    QRect stable = QRect();
    int stableCount = 0;
    for (int i = 0; i < 60 && stableCount < 4; ++i) {
        QEventLoop tick;
        QTimer::singleShot(50, &tick, &QEventLoop::quit);
        tick.exec();

        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
                if (fb->boxName() != QStringLiteral("圆角")) continue;
                const QRect g = fb->frameGeometry();
                if (g == stable && fb->windowOpacity() >= 0.99) {
                    ++stableCount;
                } else {
                    stable = g;
                    stableCount = 0;
                }
                break;
            }
        }
    }
    out << "位置稳定判据：连续 " << stableCount << " 次读到 " << stable.x() << "," << stable.y()
        << " " << stable.width() << "x" << stable.height() << "\n\n";

    FloatingBoxWidget *fw = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
            if (fb->boxName() == QStringLiteral("圆角")) { fw = fb; break; }
        }
    }
    if (!fw) { out << "找不到浮窗\n"; return 1; }

    const QRect rect = fw->frameGeometry();
    out << "frameGeometry = " << rect.x() << "," << rect.y()
        << " " << rect.width() << "x" << rect.height() << "\n";
    out << "mask 是否为空 = " << (fw->mask().isEmpty() ? "是（没设 mask）" : "否（mask 已设）") << "\n\n";

    QScreen *screen = QApplication::primaryScreen();
    const QPixmap shot = screen->grabWindow(0, rect.x(), rect.y(),
                                            rect.width(), rect.height());
    if (shot.isNull()) { out << "抓屏失败\n"; return 1; }
    const QImage img = shot.toImage();

    // 降采样成字符画：宽 65 字符，高按比例
    const int cols = 65;
    const int rows = qMax(1, img.height() * cols / qMax(1, img.width()) / 2);

    auto toChar = [](const QColor &c) -> QChar {
        const int lum = (c.red() * 299 + c.green() * 587 + c.blue() * 114) / 1000;
        if (lum > 235) return QLatin1Char('.');   // 很亮（白底）
        if (lum > 200) return QLatin1Char('-');   // 亮（浅灰标题栏）
        if (lum > 150) return QLatin1Char('+');   // 中（边框/桌面）
        if (lum > 80)  return QLatin1Char('*');   // 暗
        return QLatin1Char('#');                  // 很暗
    };

    out << "字符画（. = 亮/白, - = 浅灰, + = 中, * = 暗, # = 很暗）:\n\n";
    for (int ry = 0; ry < rows; ++ry) {
        QString line;
        for (int rx = 0; rx < cols; ++rx) {
            const int px = rx * img.width() / cols;
            const int py = ry * img.height() / rows;
            line += toChar(img.pixelColor(qBound(0, px, img.width() - 1),
                                          qBound(0, py, img.height() - 1)));
        }
        out << line << "\n";
    }

    out << "\n四个角的具体像素（各取 3x3 平均）：\n";
    auto cornerAt = [&](int x0, int y0) {
        int rr = 0, gg = 0, bb = 0, n = 0;
        for (int dy = 0; dy < 3; ++dy) {
            for (int dx = 0; dx < 3; ++dx) {
                const int x = qBound(0, x0 + dx, img.width() - 1);
                const int y = qBound(0, y0 + dy, img.height() - 1);
                const QColor c = img.pixelColor(x, y);
                rr += c.red(); gg += c.green(); bb += c.blue(); ++n;
            }
        }
        return QColor(rr / n, gg / n, bb / n);
    };

    auto show = [&](const char *label, const QColor &c) {
        out << "  " << label << " rgb(" << c.red() << "," << c.green() << "," << c.blue() << ")\n";
    };
    show("左上(1,1)          ", cornerAt(1, 1));
    show("右上(w-4,1)        ", cornerAt(img.width() - 4, 1));
    show("左下(1,h-4)        ", cornerAt(1, img.height() - 4));
    show("右下(w-4,h-4)      ", cornerAt(img.width() - 4, img.height() - 4));
    show("正中(w/2,h/2)      ", cornerAt(img.width() / 2, img.height() / 2));
    show("标题栏(w/2,10)     ", cornerAt(img.width() / 2, 10));

    // ---------------------------------------------------------------------
    // 补充诊断：窗口到底画在哪。
    //
    // 前面那张 ASCII 图的前几行是桌面色，说明"抓屏矩形左上角"与"窗口左上角"
    // 不重合。为了定位，这里沿垂直方向扫一列像素，找出窗口内容的起始 y ——
    // 判据是"从这一行起，连续多行都是亮色（窗口白底）"。
    // ---------------------------------------------------------------------
    out << "\n沿 x=130 这一列，从上往下找窗口内容的起始行：\n";
    int contentTop = -1;
    for (int y = 0; y + 8 < img.height(); ++y) {
        bool allBright = true;
        for (int dy = 0; dy < 8; ++dy) {
            const QColor c = img.pixelColor(130, y + dy);
            const int lum = (c.red() * 299 + c.green() * 587 + c.blue() * 114) / 1000;
            if (lum < 200) { allBright = false; break; }
        }
        if (allBright) { contentTop = y; break; }
    }
    out << "  内容起始行 y = " << contentTop;
    if (contentTop >= 0) {
        out << "  →  窗口实际顶部 ≈ " << (rect.y() + contentTop);
        out << "（frameGeometry 报的是 " << rect.y() << "）";
    }
    out << "\n";

    // ---------------------------------------------------------------------
    // 坐标不可靠，改为"在整块屏幕上扫描找窗口"。
    //
    // 上一版发现抓到的图里，窗口只在 y>=28 处出现，说明 grabWindow 的
    // 原点与 frameGeometry 的原点不重合（差 28px，恰好是标题栏高度，
    // 怀疑是窗口框架补偿）。与其继续猜偏移量，不如用最笨也最可靠的办法：
    // 抓一整块比窗口大得多的区域，然后在里面**扫描找窗口的边界**。
    //
    // 判据：窗口左边界 = 第一列"从上往下有连续 N 个亮像素"的位置。
    // ---------------------------------------------------------------------
    const int pad = 60;
    const QRect wide(rect.x() - pad, rect.y() - pad,
                     rect.width() + pad * 2, rect.height() + pad * 2);
    const QPixmap bigShot = screen->grabWindow(0, wide.x(), wide.y(),
                                               wide.width(), wide.height());
    if (bigShot.isNull()) { out << "大范围抓屏失败\n"; return 0; }
    const QImage big = bigShot.toImage();
    out << "\n大范围抓屏: " << wide.x() << "," << wide.y() << " "
        << wide.width() << "x" << wide.height() << "\n";

    // 扫描找窗口左上角：找一个像素，它的右侧 20 像素、下方 20 像素都是亮的
    int foundX = -1, foundY = -1;
    for (int y = 0; y + 24 < big.height() && foundY < 0; ++y) {
        for (int x = 0; x + 24 < big.width(); ++x) {
            bool ok = true;
            for (int d = 0; d < 24 && ok; ++d) {
                const QColor ch = big.pixelColor(x + d, y);
                const QColor cv = big.pixelColor(x, y + d);
                const int lh = (ch.red() * 299 + ch.green() * 587 + ch.blue() * 114) / 1000;
                const int lv = (cv.red() * 299 + cv.green() * 587 + cv.blue() * 114) / 1000;
                if (lh < 190 || lv < 190) ok = false;
            }
            if (ok) { foundX = x; foundY = y; break; }
        }
    }
    out << "扫描到的窗口左上角（相对大图）: " << foundX << "," << foundY;
    if (foundX >= 0) {
        out << "  →  屏幕坐标 " << (wide.x() + foundX) << "," << (wide.y() + foundY);
    }
    out << "\n";

    // 用扫描到的位置重新截一块，做圆角判定
    if (foundX >= 0 && foundY >= 0 && foundX + rect.width() <= big.width()
        && foundY + rect.height() <= big.height()) {
        const QImage exact = big.copy(foundX, foundY, rect.width(), rect.height());
        out << "\n用扫描到的位置重新采样四角：\n";
        auto sample = [&](const char *label, int x0, int y0) {
            int rr = 0, gg = 0, bb = 0, n = 0;
            for (int dy = 0; dy < 3; ++dy) {
                for (int dx = 0; dx < 3; ++dx) {
                    const QColor c = exact.pixelColor(qBound(0, x0 + dx, exact.width() - 1),
                                                      qBound(0, y0 + dy, exact.height() - 1));
                    rr += c.red(); gg += c.green(); bb += c.blue(); ++n;
                }
            }
            const QColor c(rr / n, gg / n, bb / n);
            out << "  " << label << " rgb(" << c.red() << "," << c.green() << "," << c.blue() << ")\n";
            return c;
        };
        const QColor tl = sample("左上(1,1)     ", 1, 1);
        const QColor tr = sample("右上(w-4,1)   ", exact.width() - 4, 1);
        const QColor bl = sample("左下(1,h-4)   ", 1, exact.height() - 4);
        const QColor br = sample("右下(w-4,h-4) ", exact.width() - 4, exact.height() - 4);
        sample("正中(w/2,h/2) ", exact.width() / 2, exact.height() / 2);

        const bool cornersSame = (tl == tr && tl == bl && tl == br);
        out << "\n  >>> 四角是否一致: " << (cornersSame ? "是" : "否") << "\n";
    }

    mgr.closeAll();
    QDir(root).removeRecursively();
    out.flush();
    return 0;
}
