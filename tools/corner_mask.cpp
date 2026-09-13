#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QRegion>
#include <QTextStream>
#include <QTimer>

#include "appservice.h"
#include "boxmanager.h"
#include "floatingboxmanager.h"
#include "floatingboxwidget.h"

// ---------------------------------------------------------------------------
// corner_mask —— 通过读窗口 mask 验证圆角，不抓屏。
//
// 【为什么放弃抓屏】
// 前两个版本（corner_diag / corner_dump）都想用 QScreen::grabWindow 抓屏后
// 读像素来判断圆角，结果绕了很久也没定论：
//   * 抓到的区域与 frameGeometry 报的位置对不上（差 28px）；
//   * 屏幕背景本身是浅灰蓝的渐变，与窗口白底的亮度差不大，
//     靠亮度阈值找窗口边界的启发式方法直接失效（扫描到 (0,0)）。
//
// 绕远路是因为一开始就选错了判据。"圆角有没有生效"的**真值**只有一个：
// 窗口的 mask。Qt 就是拿它来裁剪窗口可见区域的 —— mask 上为 0 的像素
// 根本不会被画到屏幕上。所以直接读 mask，比"画出来再检验画得对不对"
// 既准确又不受桌面背景干扰。
//
// 局限：mask 只证明"裁剪区域是对的"，不证明"子控件没在裁剪区外多画东西"。
// 但那件事由 mask 本身保证 —— 窗口级裁剪对子控件一样有效。
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
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyCornerMask"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyCornerMask"));

    QTextStream out(stdout);
    out << "=== 圆角验证（读窗口 mask）===\n\n";

    const QString root = QStringLiteral("F:/QtProject/DeskTidy/_cornermask");
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

    // 等窗口真正显示并完成一次布局
    for (int i = 0; i < 20; ++i) {
        QEventLoop tick;
        QTimer::singleShot(50, &tick, &QEventLoop::quit);
        tick.exec();
    }

    FloatingBoxWidget *fw = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
            if (fb->boxName() == QStringLiteral("圆角")) { fw = fb; break; }
        }
    }
    if (!fw) { out << "找不到浮窗\n"; return 1; }

    out << "浮窗尺寸: " << fw->width() << "x" << fw->height() << "\n";
    out << "WA_TranslucentBackground = "
        << (fw->testAttribute(Qt::WA_TranslucentBackground) ? "true" : "false") << "\n\n";

    // QWidget::mask() 返回 QRegion 而不是 QBitmap —— setMask(QBitmap) 会被
    // 转成区域存储（顺带说明：抗锯齿在转换时会被丢掉，区域是二值的）。
    // 所以这里直接用 QRegion::contains 判定。
    const QRegion mask = fw->mask();
    check(!mask.isEmpty(),
          QStringLiteral("窗口设了 mask（不设的话四角永远是方的）"), out);
    if (mask.isEmpty()) {
        out << "\n=== 结果：" << gPass << " 通过 / " << gFail << " 失败 ===\n";
        return 1;
    }

    const QRect maskBounds = mask.boundingRect();
    const int w = fw->width();
    const int h = fw->height();
    out << "mask 包围盒: " << maskBounds.width() << "x" << maskBounds.height()
        << "   窗口尺寸: " << w << "x" << h << "\n\n";

    auto visibleAt = [&](int x, int y) { return mask.contains(QPoint(x, y)); };

    const int r = 2;    // 取最角上的一点

    out << "采样点可见性（窗口最角上的 2px 处，圆角生效时应当不可见）:\n";
    const bool tl = visibleAt(r, r);
    const bool tr = visibleAt(w - 1 - r, r);
    const bool bl = visibleAt(r, h - 1 - r);
    const bool br = visibleAt(w - 1 - r, h - 1 - r);
    out << "  左上=" << (tl ? "可见" : "已裁") << "  右上=" << (tr ? "可见" : "已裁")
        << "  左下=" << (bl ? "可见" : "已裁") << "  右下=" << (br ? "可见" : "已裁") << "\n\n";

    // 边中点应当仍然可见（圆角只切四个角，不该把边也切掉）
    const bool mt = visibleAt(w / 2, 0);
    const bool mb = visibleAt(w / 2, h - 1);
    const bool ml = visibleAt(0, h / 2);
    const bool mr = visibleAt(w - 1, h / 2);
    out << "四条边中点应当仍可见:\n";
    out << "  上=" << (mt ? "可见" : "已裁") << "  下=" << (mb ? "可见" : "已裁")
        << "  左=" << (ml ? "可见" : "已裁") << "  右=" << (mr ? "可见" : "已裁") << "\n\n";

    out << QStringLiteral("沿左上角对角线采样（圆角处应从 已裁 渐变为 可见）:\n  ");
    for (int d = 0; d <= 12; ++d) {
        out << d << QLatin1Char(':') << (visibleAt(d, d) ? QLatin1Char('V') : QLatin1Char('.'))
            << QLatin1Char(' ');
    }
    out << "\n\n";

    check(!tl && !tr && !bl && !br,
          QStringLiteral("四个角都被裁掉（圆角生效）"), out);
    check(mt && mb && ml && mr,
          QStringLiteral("四条边中点仍可见（圆角没把边也切了）"), out);
    check(visibleAt(10, 10),
          QStringLiteral("距角 10px 处可见（裁掉的范围就是圆角大小，没多切）"), out);

    // 尺寸变化后 mask 必须跟着重算 —— 这是 mask 方案最大的维护点。
    //
    // ⚠️ 断言方式：不能拿"新包围盒 != 旧包围盒"当判据。
    // 浮窗的尺寸是从配置恢复的，若上一次运行恰好把它留在 320x360，
    // 这一轮 resize(320,360) 就成了空操作，旧断言必然失败 —— 那测的是
    // "配置里存了什么"，不是"mask 会不会重算"。
    //
    // 正确的判据是"mask 的包围盒 == 窗口当前尺寸"：只要这两者始终一致，
    // 就说明每次尺寸变化 mask 都跟上了。再挑一个与当前尺寸不同的目标值，
    // 确保 resize 真的发生了。
    const QSize target = (fw->size() == QSize(340, 380))
                             ? QSize(300, 320)
                             : QSize(340, 380);
    fw->resize(target);
    for (int i = 0; i < 8; ++i) {
        QEventLoop tick;
        QTimer::singleShot(30, &tick, &QEventLoop::quit);
        tick.exec();
    }
    const QRegion afterMask = fw->mask();
    const QRect afterRect = afterMask.boundingRect();

    out << "改尺寸到 " << target.width() << "x" << target.height()
        << "  窗口=" << fw->width() << "x" << fw->height()
        << "  mask 包围盒=" << afterRect.width() << "x" << afterRect.height() << "\n";

    check(!afterMask.isEmpty()
              && afterRect.width() == fw->width()
              && afterRect.height() == fw->height(),
          QStringLiteral("mask 包围盒与窗口尺寸一致（尺寸变化后 mask 跟上了）"), out);

    check(!afterMask.contains(QPoint(2, 2))
              && !afterMask.contains(QPoint(fw->width() - 3, fw->height() - 3)),
          QStringLiteral("改尺寸后新 mask 的四角同样被裁（圆角没丢）"), out);

    mgr.closeAll();
    QDir(root).removeRecursively();

    out << "\n=== 结果：" << gPass << " 通过 / " << gFail << " 失败 ===\n";
    out.flush();
    return gFail == 0 ? 0 : 1;
}
