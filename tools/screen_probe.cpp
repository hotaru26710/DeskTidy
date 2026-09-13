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
// screen_probe —— 抓真实屏幕，看标题栏那块**在屏幕上**到底是什么颜色。
//
// 【为什么必须回到抓屏】
// 上一版用 QWidget::grab() 渲染浮窗，看到标题栏是一片黑，于是以为找到了问题
// 并去改了背景绘制方式 —— 改完还是黑的。后来才想清楚：
// **grab() 渲染的是控件的 paintEvent 结果，不经过窗口合成**。
// 而 WA_TranslucentBackground 的效果正是发生在合成阶段。所以 grab() 看到的
// "黑"很可能只是"透明区域在无合成上下文里的默认值"，与屏幕上的真实观感
// 完全是两回事 —— 用错误的判据得出了错误的结论。
//
// 【这次怎么保证抓对位置】
// 之前抓屏失败是因为拿 frameGeometry() 去 grabWindow，两者原点差 28px。
// 这次改用**另一个已知可靠的来源**：把浮窗自己移动到屏幕上一个固定位置
// （(100,100)），然后去抓那个位置。位置是自己设的，不存在对不上的问题。
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyScreenProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyScreenProbe"));

    QTextStream out(stdout);
    out << "=== 屏幕上标题栏的实际颜色 ===\n\n";

    const QString root = QStringLiteral("F:/QtProject/DeskTidy/_screenprobe");
    QDir(root).removeRecursively();
    QDir().mkpath(root);

    AppService service;
    QString err;
    BoxManager::ensureRoot(root + QStringLiteral("/boxes"), &err);

    StorageBox box;
    BoxManager::createBox(root + QStringLiteral("/boxes"), QStringLiteral("屏幕"), &box, &err);
    {
        QFile f(box.path + QStringLiteral("/a.txt"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) { f.write("x"); f.close(); }
    }

    FloatingBoxManager mgr(&service);
    mgr.openBox(QStringLiteral("屏幕"), box.path);
    for (int i = 0; i < 15; ++i) {
        QEventLoop t; QTimer::singleShot(40, &t, &QEventLoop::quit); t.exec();
    }

    FloatingBoxWidget *fw = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
            if (fb->boxName() == QStringLiteral("屏幕")) { fw = fb; break; }
        }
    }
    if (!fw) { out << "找不到浮窗\n"; return 1; }

    // 把它挪到一个固定的、肯定在屏幕内的位置。
    // 之所以自己指定：这样"抓屏区域"与"窗口位置"用的是同一个来源，不会错位。
    const int X = 100;
    const int Y = 100;
    fw->move(X, Y);
    for (int i = 0; i < 12; ++i) {
        QEventLoop t; QTimer::singleShot(40, &t, &QEventLoop::quit); t.exec();
    }
    // 淡入动画也会影响读到的颜色，等它跑完
    for (int i = 0; i < 15; ++i) {
        QEventLoop t; QTimer::singleShot(40, &t, &QEventLoop::quit); t.exec();
        if (fw->windowOpacity() >= 0.99) break;
    }

    out << "浮窗移到 (" << X << "," << Y << ")"
        << "  尺寸 " << fw->width() << "x" << fw->height()
        << "  不透明度 " << fw->windowOpacity() << "\n";
    out << "frameGeometry = " << fw->frameGeometry().x() << ","
        << fw->frameGeometry().y() << "\n\n";

    QScreen *screen = QApplication::primaryScreen();
    if (!screen) { out << "无屏幕\n"; return 1; }

    // 抓一块比窗口略大的区域
    const int pad = 20;
    const QPixmap shot = screen->grabWindow(0, X - pad, Y - pad,
                                            fw->width() + pad * 2,
                                            fw->height() + pad * 2);
    if (shot.isNull()) { out << "抓屏失败\n"; return 1; }
    const QImage img = shot.toImage();
    out << "抓屏区域 (" << (X - pad) << "," << (Y - pad) << ")  " << img.width() << "x" << img.height() << "\n\n";

    // 窗口在抓屏图里的偏移就是 pad
    auto px = [&](int wx, int wy) {
        return img.pixelColor(qBound(0, wx + pad, img.width() - 1),
                              qBound(0, wy + pad, img.height() - 1));
    };

    out << "沿窗口内部 x=130 这一列，从上往下每 2px 采样:\n";
    for (int y = 0; y < qMin(60, fw->height()); y += 2) {
        const QColor c = px(130, y);
        const int lum = (c.red() * 299 + c.green() * 587 + c.blue() * 114) / 1000;
        QString tag;
        if (y < 28) {
            tag = QStringLiteral("  <- 标题栏区域");
            if (lum > 200) tag += QStringLiteral("  OK（亮灰底）");
            else           tag += QStringLiteral("  <<< 底色不对，应当是浅灰(~240)");
        }
        out << "  y=" << QString::number(y).rightJustified(3) << "  " << c.name()
            << "  亮度=" << QString::number(lum).rightJustified(3) << tag << "\n";
    }

    // 窗口外一点的像素（作为桌面参考色）
    out << "\n窗口外（x=130, y=-10 即窗口上方）: " << px(130, -10).name() << "\n";
    out << "窗口外（x=130, y=h+10）:          " << px(130, fw->height() + 10).name() << "\n";

    mgr.closeAll();
    QDir(root).removeRecursively();
    out.flush();
    return 0;
}
