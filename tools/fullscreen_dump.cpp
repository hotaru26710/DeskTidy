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
// fullscreen_dump —— 抓整块屏幕，画成字符画，并标出浮窗应在的位置。
//
// 【为什么退到这一步】
// 前几版都在"抓一小块然后读像素"，但读到的颜色（#00629c、#1a223a）
// 看起来根本不像桌面壁纸，怀疑一开始就抓错了地方或者抓到了别的东西。
//
// 与其继续猜测抓哪儿，不如把**整个屏幕**画成字符画 —— 浮窗该在的位置
// 用方括号标出来，一眼就能看出：
//   * 屏幕上到底有没有这个窗口
//   * 它在字符画里的哪个位置
//   * 它的标题栏那一条是什么颜色
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyFullDump"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyFullDump"));

    QTextStream out(stdout);
    out << "=== 全屏字符画（标出浮窗应在的位置）===\n\n";

    const QString root = QStringLiteral("F:/QtProject/DeskTidy/_fulldump");
    QDir(root).removeRecursively();
    QDir().mkpath(root);

    AppService service;
    QString err;
    BoxManager::ensureRoot(root + QStringLiteral("/boxes"), &err);

    StorageBox box;
    BoxManager::createBox(root + QStringLiteral("/boxes"), QStringLiteral("全屏"), &box, &err);
    {
        QFile f(box.path + QStringLiteral("/a.txt"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) { f.write("x"); f.close(); }
    }

    FloatingBoxManager mgr(&service);
    mgr.openBox(QStringLiteral("全屏"), box.path);

    // 挪到一个容易辨认的位置
    FloatingBoxWidget *fw = nullptr;
    for (int i = 0; i < 20 && !fw; ++i) {
        QEventLoop t; QTimer::singleShot(40, &t, &QEventLoop::quit); t.exec();
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
                if (fb->boxName() == QStringLiteral("全屏")) { fw = fb; break; }
            }
        }
    }
    if (!fw) { out << "找不到浮窗\n"; return 1; }

    fw->move(200, 200);
    fw->raise();
    for (int i = 0; i < 25; ++i) {
        QEventLoop t; QTimer::singleShot(40, &t, &QEventLoop::quit); t.exec();
    }

    const QRect g = fw->frameGeometry();
    out << "浮窗 frameGeometry = (" << g.x() << "," << g.y() << ") "
        << g.width() << "x" << g.height() << "\n";
    out << "浮窗 isVisible=" << (fw->isVisible() ? "是" : "否")
        << "  windowOpacity=" << fw->windowOpacity() << "\n\n";

    QScreen *screen = QApplication::primaryScreen();
    if (!screen) { out << "无屏幕\n"; return 1; }
    out << "屏幕几何: " << screen->geometry().width() << "x"
        << screen->geometry().height() << "\n\n";

    const QPixmap shot = screen->grabWindow(0);
    if (shot.isNull()) { out << "全屏抓屏失败\n"; return 1; }
    const QImage img = shot.toImage();
    out << "抓到整屏: " << img.width() << "x" << img.height() << "\n\n";

    // 字符画：宽 100，高按比例（字符高宽比约 2:1，所以纵向折半）
    const int cols = 100;
    const int rows = qMax(1, img.height() * cols / qMax(1, img.width()) / 2);

    auto toCh = [](const QColor &c) -> QChar {
        const int lum = (c.red() * 299 + c.green() * 587 + c.blue() * 114) / 1000;
        if (lum > 235) return QLatin1Char('@');   // 很亮（白）
        if (lum > 200) return QLatin1Char('.');   // 亮
        if (lum > 150) return QLatin1Char('-');   // 中亮
        if (lum > 90)  return QLatin1Char('+');   // 中
        if (lum > 40)  return QLatin1Char('*');   // 暗
        return QLatin1Char('#');                  // 很暗
    };

    // 标出浮窗区域：把屏幕坐标换算成字符画坐标
    auto inWindow = [&](int px, int py) {
        return g.contains(px, py);
    };

    out << "（[ ] 圈出的是浮窗应在的区域；@=白 .=亮 -=中亮 +=中 *=暗 #=很暗）\n\n";
    for (int ry = 0; ry < rows; ++ry) {
        QString line;
        for (int rx = 0; rx < cols; ++rx) {
            const int px = rx * img.width() / cols;
            const int py = ry * img.height() / rows;
            const QChar ch = toCh(img.pixelColor(qBound(0, px, img.width() - 1),
                                                 qBound(0, py, img.height() - 1)));
            if (inWindow(px, py)) {
                line += QStringLiteral("[") + ch + QStringLiteral("]");
            } else {
                line += QStringLiteral(" ") + ch + QStringLiteral(" ");
            }
        }
        out << line << "\n";
    }

    // 直接把浮窗左上角那一带的原始像素打出来
    out << "\n浮窗左上角 (x,y) 起 12x4 的原始像素：\n";
    for (int dy = 0; dy < 4; ++dy) {
        out << "  y=" << (g.y() + dy) << ": ";
        for (int dx = 0; dx < 12; ++dx) {
            const int px = g.x() + dx;
            const int py = g.y() + dy;
            if (px < img.width() && py < img.height())
                out << img.pixelColor(px, py).name() << " ";
        }
        out << "\n";
    }

    mgr.closeAll();
    QDir(root).removeRecursively();
    out.flush();
    return 0;
}
