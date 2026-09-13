#include <QApplication>
#include <QBitmap>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QRegion>
#include <QSet>
#include <QTextStream>
#include <QTimer>

#include "appservice.h"
#include "boxmanager.h"
#include "floatingboxmanager.h"
#include "floatingboxwidget.h"

// ---------------------------------------------------------------------------
// mask_anim_diag —— 验证"高度动画期间不再逐帧重设圆角遮罩"这条优化。
//
// 【为什么需要这个探针】
// setMask() 不是一次普通的成员赋值：它要把遮罩转成原生窗口区域
//（Windows 上是 SetWindowRgn 那一路）。离屏实测它是**固定开销**，
// 与区域复杂度无关 —— 一个平凡矩形也要 ~800us（见 tools/mask_perf.cpp）。
// 而卷起/展开会在 220ms 里每帧改一次高度，于是原实现每帧都在 resizeEvent
// 里重画位图 + setMask，等于把 ~940us 钉在动画的每一帧上。
//
// 优化后动画期间只用"够大就行"的遮罩（原生区域 = 遮罩 ∩ 窗口矩形，
// 多出来的部分看不见），只在"窗口长过现有遮罩"时才重设一次。
//
// 【判据】
// 本探针逐帧同时记录三个数：窗口高度、mask 包围盒高度、该帧是不是动画中。
// 于是能直接读出三个事实，且都不依赖计时：
//   1) 动画中 mask 的高度**不跟着窗口走**（恒定）-> 说明没逐帧重设；
//   2) 动画中 mask 始终**不小于**窗口 -> 说明没有裁掉任何真实内容；
//   3) 静止态 mask 与窗口**严格等大** -> 圆角在落定后精确复位。
//
// 第 2 条是这次优化唯一可能真实伤人的地方（遮罩比窗口矮 = 窗口底部
// 被硬切掉一块，用户会看到内容消失）。所以它必须由探针守死。
// ---------------------------------------------------------------------------

static int gPass = 0;
static int gFail = 0;

static void check(bool ok, const QString &what, QTextStream &out)
{
    if (ok) { ++gPass; out << "  [PASS] " << what << "\n"; }
    else    { ++gFail; out << "  [FAIL] " << what << "\n"; }
    out.flush();
}

static void pump(int ms)
{
    QEventLoop t;
    QTimer::singleShot(ms, &t, &QEventLoop::quit);
    t.exec();
}

namespace {

// 一帧采样：窗口高度 + 当时 mask 包围盒高度。
struct Frame {
    int windowH = 0;
    int maskH   = 0;
};

// 本轮采样结果。
struct Trace {
    QList<Frame> frames;
    // 动画中段的样本（既不是起点也不是终点的那些）。
    QSet<int> midMaskHeights;
    QSet<int> midWindowHeights;
    bool everClipped = false;       // mask 比窗口矮 = 会裁掉内容
};

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyMaskAnimDiag"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyMaskAnimDiag"));

    QTextStream out(stdout);
    out << "=== 高度动画期间的圆角遮罩行为 ===\n\n";

    const QString root = QStringLiteral("F:/QtProject/DeskTidy/_maskanim");
    QDir(root).removeRecursively();
    QDir().mkpath(root);

    AppService service;
    QString err;
    BoxManager::ensureRoot(root + QStringLiteral("/boxes"), &err);

    StorageBox box;
    BoxManager::createBox(root + QStringLiteral("/boxes"), QStringLiteral("遮罩"), &box, &err);
    {
        QFile f(box.path + QStringLiteral("/a.txt"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) { f.write("x"); f.close(); }
    }

    FloatingBoxManager mgr(&service);
    mgr.openBox(QStringLiteral("遮罩"), box.path);
    for (int i = 0; i < 20; ++i)
        pump(50);

    FloatingBoxWidget *fw = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
            if (fb->boxName() == QStringLiteral("遮罩")) { fw = fb; break; }
        }
    }
    if (!fw) { out << "找不到浮窗\n"; return 1; }

    QWidget *titleBar = nullptr;
    for (QWidget *w : fw->findChildren<QWidget *>()) {
        if (QString::fromLatin1(w->metaObject()->className())
                .contains(QStringLiteral("TitleBar"))) {
            titleBar = w;
            break;
        }
    }
    if (!titleBar) { out << "找不到标题栏\n"; return 1; }

    auto doubleClickTitle = [&]() {
        const QPointF local(10, 10);
        const QPointF global(titleBar->mapToGlobal(QPoint(10, 10)));
        QMouseEvent press(QEvent::MouseButtonPress, local, global,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent dbl(QEvent::MouseButtonDblClick, local, global,
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(titleBar, &press);
        QApplication::sendEvent(titleBar, &dbl);
    };

    // 采样：每 4ms 抓一次（窗口高度，mask 包围盒高度）。
    // 采样本身可能落在动画帧之间，但动画有 220ms、约 14 帧，
    // 4ms 的间隔足以把"mask 到底跟不跟着窗口走"看清楚。
    auto trace = [&](const QString &label, int durationMs) -> Trace {
        Trace t;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < durationMs) {
            // 用 0ms 定时器把采样排到当前这一轮事件处理**之后**，
            // 这样读到的一定是 resizeEvent 里那次遮罩更新之后的值。
            QEventLoop t0;
            QTimer::singleShot(0, &t0, &QEventLoop::quit);
            t0.exec();

            Frame f;
            f.windowH = fw->height();
            f.maskH   = fw->mask().boundingRect().height();
            t.frames << f;
            if (f.maskH < f.windowH) t.everClipped = true;

            pump(4);
        }
        out << "  " << label << " 采样 " << t.frames.size() << " 帧\n";
        return t;
    };

    // 只保留"动画中段"的样本：既明显离开起点、也没到终点。
    // 起点/终点静止态的 mask 本来就该与窗口等大，混进来会掩盖真相。
    auto analyzeMid = [&](Trace &t, int startH, int endH) {
        const int lo = qMin(startH, endH) + 8;
        const int hi = qMax(startH, endH) - 8;
        for (const Frame &f : t.frames) {
            if (f.windowH > lo && f.windowH < hi) {
                t.midMaskHeights.insert(f.maskH);
                t.midWindowHeights.insert(f.windowH);
            }
        }
    };

    auto streamSet = [&](const QSet<int> &s) {
        QList<int> v = s.values();
        std::sort(v.begin(), v.end());
        QString r;
        for (int i = 0; i < v.size(); ++i) {
            if (i) r += QStringLiteral(", ");
            r += QString::number(v[i]);
        }
        return r.isEmpty() ? QStringLiteral("(空)") : r;
    };

    const int expandedH = fw->height();
    out << "浮窗展开高度 = " << expandedH << "\n";
    out << "静止态 mask 高度 = " << fw->mask().boundingRect().height() << "\n\n";

    check(expandedH > 100,
          QStringLiteral("前提：浮窗初始是展开态（%1 > 100）").arg(expandedH), out);

    // ================= [1] 卷起 =================
    out << QStringLiteral("[1] 卷起（%1 -> 标题栏高）\n").arg(expandedH);
    doubleClickTitle();
    Trace up = trace(QStringLiteral("卷起"), 450);
    analyzeMid(up, expandedH, fw->height());

    out << "  动画中段：窗口高度出现过 " << up.midWindowHeights.size()
        << " 种，mask 高度出现过 " << up.midMaskHeights.size() << " 种\n";
    out << "  动画中段窗口高度集合: " << streamSet(up.midWindowHeights) << "\n";
    out << "  动画中段 mask 高度集合: " << streamSet(up.midMaskHeights) << "\n";
    out << "  卷起后：窗口=" << fw->height()
        << "  mask 包围盒=" << fw->mask().boundingRect().height() << "\n";

    check(up.midWindowHeights.size() >= 3,
          QStringLiteral("前提：确实采到了动画中段（窗口高度有 %1 种中间值）")
              .arg(up.midWindowHeights.size()), out);
    check(!up.everClipped,
          QStringLiteral("★ 全称断言：动画期间 mask 从未比窗口矮（否则底部内容会被裁掉）"), out);
    check(up.midMaskHeights.size() == 1,
          QStringLiteral("★ 卷起动画中 mask 高度恒定（%1 种）—— 没有逐帧重设遮罩，"
                         "而基线会重设 %2 次")
              .arg(up.midMaskHeights.size())
              .arg(up.midWindowHeights.size() + 2), out);
    check(fw->isRolledUp(), QStringLiteral("卷起状态成立"), out);
    check(fw->mask().boundingRect().height() == fw->height(),
          QStringLiteral("★ 卷起落定后 mask 与窗口严格等大（圆角精确复位）"), out);

    // ================= [2] 展开 =================
    out << QStringLiteral("\n[2] 展开（标题栏高 -> %1）\n").arg(expandedH);
    const int rolledH = fw->height();
    doubleClickTitle();
    Trace down = trace(QStringLiteral("展开"), 450);
    analyzeMid(down, rolledH, fw->height());

    out << "  动画中段：窗口高度出现过 " << down.midWindowHeights.size()
        << " 种，mask 高度出现过 " << down.midMaskHeights.size() << " 种\n";
    out << "  动画中段窗口高度集合: " << streamSet(down.midWindowHeights) << "\n";
    out << "  动画中段 mask 高度集合: " << streamSet(down.midMaskHeights) << "\n";
    out << "  展开后：窗口=" << fw->height()
        << "  mask 包围盒=" << fw->mask().boundingRect().height() << "\n";

    check(down.midWindowHeights.size() >= 3,
          QStringLiteral("前提：确实采到了动画中段（窗口高度有 %1 种中间值）")
              .arg(down.midWindowHeights.size()), out);
    check(!down.everClipped,
          QStringLiteral("★ 全称断言：动画期间 mask 从未比窗口矮（否则底部内容会被裁掉）"), out);
    check(down.midMaskHeights.size() == 1,
          QStringLiteral("★ 展开动画中 mask 高度恒定（%1 种）—— 同上")
              .arg(down.midMaskHeights.size()), out);
    check(!fw->isRolledUp() && fw->height() == expandedH,
          QStringLiteral("展开回到原高度（%1）").arg(expandedH), out);
    check(fw->mask().boundingRect().height() == fw->height(),
          QStringLiteral("★ 展开落定后 mask 与窗口严格等大（圆角精确复位）"), out);

    // ================= [3] 一次 setMask 值多少 =================
    //
    // 直接在这扇**真实可见的原生窗口**上量。要防 Qt 自己的去重
    //（同一个区域重复设等于没设，实测 0.0us），所以两张内容不同的位图交替设。
    {
        const int w = fw->width();
        const int h = fw->height();
        auto makeMask = [&](bool notch) {
            QBitmap bm(w, h);
            bm.fill(Qt::color0);
            QPainter p(&bm);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setBrush(Qt::color1);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(QRectF(0, 0, w, notch ? h - 1 : h).adjusted(0.5, 0.5, -0.5, -0.5),
                              8, 8);
            p.end();
            return bm;
        };
        const QBitmap a = makeMask(false);
        const QBitmap b = makeMask(true);

        const int reps = 20;
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < reps; ++i) {
            fw->setMask(i % 2 ? a : b);
            QApplication::processEvents();
        }
        const qint64 totalUs = timer.nsecsElapsed() / 1000;
        out << "\n[3] 真实原生窗口上 setMask 的代价\n";
        out << "  " << reps << " 次交替设不同区域，合计 " << totalUs
            << " us，平均 " << (totalUs / reps) << " us/次\n";
    }

    mgr.closeAll();
    QDir(root).removeRecursively();

    out << "\n=== 结果：" << gPass << " 通过 / " << gFail << " 失败 ===\n";
    out.flush();
    return gFail == 0 ? 0 : 1;
}
