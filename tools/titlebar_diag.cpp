#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QPixmap>
#include <QTextStream>
#include <QTimer>

#include "appservice.h"
#include "boxmanager.h"
#include "floatingboxmanager.h"
#include "floatingboxwidget.h"

// ---------------------------------------------------------------------------
// titlebar_diag —— 用 QWidget::grab() 读标题栏自己的渲染结果。
//
// 【为什么这次用 grab() 而不是抓屏】
// 抓屏在前面绕过两圈：坐标对不上、桌面背景干扰。而这次要回答的问题很局部 ——
// "标题栏这块控件自己画出来是什么颜色" —— 那 grab() 就是最直接的答案：
// 它渲染控件本身，不含任何窗口管理器的装饰，也不受桌面背景影响。
//
// grab() 的局限（必须知道）：它抓不到"控件外面的透明区域"，所以**不能**
// 用来验证圆角。圆角已经由 corner_mask 用 mask 验过了，这里只查底色。
//
// 判据：标题栏区域里，"非文字"的部分（即避开标签与按钮的那些像素）应当是
// 标题栏的浅灰 #F1F3F4，而不是全透明/纯黑/纯白。
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyTitleDiag"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyTitleDiag"));

    QTextStream out(stdout);
    out << "=== 标题栏渲染检查 ===\n\n";

    const QString root = QStringLiteral("F:/QtProject/DeskTidy/_titlediag");
    QDir(root).removeRecursively();
    QDir().mkpath(root);

    AppService service;
    QString err;
    BoxManager::ensureRoot(root + QStringLiteral("/boxes"), &err);

    StorageBox box;
    BoxManager::createBox(root + QStringLiteral("/boxes"), QStringLiteral("标题"), &box, &err);
    {
        QFile f(box.path + QStringLiteral("/a.txt"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) { f.write("x"); f.close(); }
    }

    FloatingBoxManager mgr(&service);
    mgr.openBox(QStringLiteral("标题"), box.path);
    for (int i = 0; i < 15; ++i) {
        QEventLoop t; QTimer::singleShot(40, &t, &QEventLoop::quit); t.exec();
    }

    FloatingBoxWidget *fw = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
            if (fb->boxName() == QStringLiteral("标题")) { fw = fb; break; }
        }
    }
    if (!fw) { out << "找不到浮窗\n"; return 1; }

    // 找标题栏
    QWidget *tb = nullptr;
    for (QObject *child : fw->children()) {
        if (auto *w2 = qobject_cast<QWidget *>(child)) {
            if (QString::fromLatin1(w2->metaObject()->className())
                    .contains(QStringLiteral("TitleBar"))) {
                tb = w2;
                break;
            }
        }
    }
    if (!tb) { out << "找不到标题栏\n"; return 1; }

    out << "标题栏: " << tb->metaObject()->className()
        << "  " << tb->width() << "x" << tb->height() << "\n";
    out << "  样式表内容: " << (tb->styleSheet().isEmpty()
                                  ? QStringLiteral("(空)")
                                  : tb->styleSheet()) << "\n";
    out << "  autoFillBackground = " << (tb->autoFillBackground() ? "是" : "否") << "\n";
    out << "  palette.Window = " << tb->palette().color(QPalette::Window).name() << "\n\n";

    // 渲染标题栏自身
    const QPixmap pm = tb->grab();
    if (pm.isNull()) { out << "grab 失败\n"; return 1; }
    const QImage img = pm.toImage();
    out << "渲染结果: " << img.width() << "x" << img.height() << "\n\n";

    // 沿标题栏中间那一行采样（避开顶部的圆角与底部的分隔线）
    const int y = img.height() / 2;
    out << "沿 y=" << y << " 采样（每 20px 一个点）:\n  ";
    QHash<QString, int> histogram;
    for (int x = 0; x < img.width(); x += 20) {
        const QColor c = img.pixelColor(x, y);
        out << x << ":" << c.name() << "  ";
        histogram[c.name()] += 1;
    }
    out << "\n\n";

    out << "颜色直方图（出现次数最多的应当是标题栏底色 #f1f3f4）:\n";
    QList<QPair<QString, int>> sorted;
    for (auto it = histogram.constBegin(); it != histogram.constEnd(); ++it)
        sorted.append({it.key(), it.value()});
    std::sort(sorted.begin(), sorted.end(),
              [](const QPair<QString, int> &a, const QPair<QString, int> &b) {
                  return a.second > b.second;
              });
    for (const auto &p : sorted)
        out << "  " << p.first << "  x" << p.second << "\n";

    // 结论
    out << "\n判定：\n";
    const bool hasTitleBg = histogram.contains(QStringLiteral("#f1f3f4"));
    out << "  标题栏底色 #f1f3f4 是否出现: " << (hasTitleBg ? "是" : "否");
    if (!hasTitleBg) {
        out << "   <<< 底色没画出来 —— 这就是「顶部透明」的根因";
    }
    out << "\n";

    // 顺手检查每层子控件的 autoFillBackground 与样式表，列出"哪些层不画背景"
    out << "\n所有子控件的背景来源:\n";
    for (QObject *child : fw->children()) {
        auto *w2 = qobject_cast<QWidget *>(child);
        if (!w2) continue;
        out << "  " << w2->metaObject()->className()
            << "  样式表=" << (w2->styleSheet().isEmpty() ? "无" : "有")
            << "  autoFill=" << (w2->autoFillBackground() ? "是" : "否")
            << "  paletteWin=" << w2->palette().color(QPalette::Window).name() << "\n";
    }

    // -----------------------------------------------------------------------
    // 整个浮窗的 ASCII 渲染图。
    //
    // 本会话的模型看不了图，但看得了一屏字符。这张图能一眼看出"标题栏那块
    // 有没有底色"—— 有底色会渲染成一整条同字符的行，没底色则与列表区
    // 无法区分（或呈透明/黑）。
    // -----------------------------------------------------------------------
    out << "\n整个浮窗的渲染字符画（标出了标题栏的范围）:\n\n";
    const QPixmap full = fw->grab();
    const QImage fi = full.toImage();
    const int cols = 60;
    const int rows = qMax(1, fi.height() * cols / qMax(1, fi.width()) / 2);

    auto toCh = [](const QColor &c) -> QChar {
        const int lum = (c.red() * 299 + c.green() * 587 + c.blue() * 114) / 1000;
        if (lum > 235) return QLatin1Char('.');   // 很亮
        if (lum > 200) return QLatin1Char('-');   // 浅灰（标题栏底色）
        if (lum > 150) return QLatin1Char('+');
        if (lum > 80)  return QLatin1Char('*');
        return QLatin1Char('#');                  // 很暗（文字）
    };

    const int titleRows = qMax(1, tb->height() * rows / qMax(1, fi.height()));
    for (int ry = 0; ry < rows; ++ry) {
        QString line;
        for (int rx = 0; rx < cols; ++rx) {
            const int px = rx * fi.width() / cols;
            const int py = ry * fi.height() / rows;
            line += toCh(fi.pixelColor(qBound(0, px, fi.width() - 1),
                                       qBound(0, py, fi.height() - 1)));
        }
        out << (ry < titleRows ? QStringLiteral("T|") : QStringLiteral(" |"))
            << line << "\n";
    }
    out << "\n（T| 开头的行是标题栏区域，应当能看到连续的底色字符与若干文字字符）\n";

    mgr.closeAll();
    QDir(root).removeRecursively();
    out.flush();
    return hasTitleBg ? 0 : 1;
}
