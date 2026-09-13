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
// corner_diag —— 用读像素的方式验证圆角是否真的生效。
//
// 【为什么用像素而不是"看截图"】
// 本会话的模型看不了图。而"圆角有没有生效"是个纯视觉问题，光靠读代码
// 只能确认样式表写对了，确认不了四角是不是真的透明了 —— 比如
// WA_TranslucentBackground 没起作用时，样式表的 border-radius 依然会被
// 父窗口的底色填满四个角，代码看着一点问题没有，实际是四个黑方块。
//
// 所以这里抓浮窗那一块屏幕，直接读四个角与四条边中点的像素：
//   * 四角应当是"桌面背景"的颜色（不是浮窗的白色）
//   * 四边中点应当是浮窗的白/灰（不是桌面背景）
// 这个判据不依赖任何 Qt 内部状态，是最终的、可见的结果。
//
// 局限：它无法区分"圆角透明"与"四角恰好也是白色"。所以脚本会把
// 四角像素的 RGB 打出来供人工核对，并在桌面色与浮窗白接近时给出提示。
// ---------------------------------------------------------------------------

static int gPass = 0;
static int gFail = 0;

static void check(bool ok, const QString &what, QTextStream &out)
{
    if (ok) { ++gPass; out << "  [PASS] " << what << "\n"; }
    else    { ++gFail; out << "  [FAIL] " << what << "\n"; }
    out.flush();
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyCornerDiag"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyCornerDiag"));

    QTextStream out(stdout);
    out << "=== 圆角实效验证（读屏幕像素）===\n\n";

    const QString root = QStringLiteral("F:/QtProject/DeskTidy/_cornerdiag");
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

    // 等淡入动画跑完，否则抓到的是半透明的窗口，颜色对不上
    QEventLoop loop;
    QTimer::singleShot(600, &loop, &QEventLoop::quit);
    loop.exec();

    FloatingBoxWidget *fw = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
            if (fb->boxName() == QStringLiteral("圆角")) { fw = fb; break; }
        }
    }
    if (!fw) { out << "找不到浮窗\n"; return 1; }

    out << "浮窗几何: " << fw->frameGeometry().x() << "," << fw->frameGeometry().y()
        << "  " << fw->frameGeometry().width() << "x" << fw->frameGeometry().height() << "\n";
    out << "WA_TranslucentBackground = "
        << (fw->testAttribute(Qt::WA_TranslucentBackground) ? "true" : "false") << "\n\n";

    // 抓浮窗所占的那块屏幕
    const QRect rect = fw->frameGeometry();
    QScreen *screen = QApplication::primaryScreen();
    if (!screen) { out << "没有屏幕\n"; return 1; }

    const QPixmap shot = screen->grabWindow(0, rect.x(), rect.y(),
                                            rect.width(), rect.height());
    if (shot.isNull()) { out << "抓屏失败\n"; return 1; }
    const QImage img = shot.toImage();
    out << "抓到图像: " << img.width() << "x" << img.height() << "\n\n";

    // 四角与四边中点的采样点。往里缩 1px 避开边框线本身。
    const int r = 7;    // 圆角半径 8，取 7 保证落在圆角弧线**之外**
    const int w = img.width();
    const int h = img.height();

    const QColor topLeft     = img.pixelColor(r, r);
    const QColor topRight    = img.pixelColor(w - 1 - r, r);
    const QColor bottomLeft  = img.pixelColor(r, h - 1 - r);
    const QColor bottomRight = img.pixelColor(w - 1 - r, h - 1 - r);

    const QColor midTop    = img.pixelColor(w / 2, 2);
    const QColor midBottom = img.pixelColor(w / 2, h - 3);
    const QColor midLeft   = img.pixelColor(2, h / 2);

    auto name = [](const QColor &c) {
        return QStringLiteral("rgb(%1,%2,%3)").arg(c.red()).arg(c.green()).arg(c.blue());
    };

    out << "四角像素（期望是桌面背景色，不是白色）:\n";
    out << "  左上 " << name(topLeft)    << "\n";
    out << "  右上 " << name(topRight)   << "\n";
    out << "  左下 " << name(bottomLeft) << "\n";
    out << "  右下 " << name(bottomRight)<< "\n\n";

    out << "边中点像素（期望是浮窗的白色/浅灰）:\n";
    out << "  上中 " << name(midTop)    << "\n";
    out << "  下中 " << name(midBottom) << "\n";
    out << "  左中 " << name(midLeft)   << "\n\n";

    // 判据：四角颜色一致（它们都是桌面背景），且与窗口内部的白色不同。
    const bool cornersAgree = (topLeft == topRight)
                              && (topLeft == bottomLeft)
                              && (topLeft == bottomRight);

    // 浮窗内部取一点作为"窗口色"
    const QColor inner = img.pixelColor(w / 2, h / 2);
    out << "窗口内部像素: " << name(inner) << "\n\n";

    check(cornersAgree,
          QStringLiteral("四个角颜色一致（说明都被圆角切掉了，剩下的是同一片桌面）"),
          out);
    check(inner != topLeft,
          QStringLiteral("四角颜色与窗口内部不同（说明角上确实不是窗口的一部分）"),
          out);

    if (qAbs(inner.red() - topLeft.red()) < 12) {
        out << "\n⚠️ 提示：窗口内部与四角颜色接近，本判据分辨力有限，"
               "请人工看一眼确认是不是真的圆角。\n";
    }

    out << "\n=== 结果：" << gPass << " 通过 / " << gFail << " 失败 ===\n";

    mgr.closeAll();
    QDir(root).removeRecursively();
    out.flush();
    return gFail == 0 ? 0 : 1;
}
