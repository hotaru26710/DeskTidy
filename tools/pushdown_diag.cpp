#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QMouseEvent>
#include <QSet>
#include <QTextStream>
#include <QTimer>

#include "appservice.h"
#include "boxmanager.h"
#include "floatingboxmanager.h"
#include "floatingboxwidget.h"

// ---------------------------------------------------------------------------
// pushdown_diag —— 浮窗"互相让位 + 平滑移动 + 钉住"的行为验证。
//
// 【为什么必须是探针】
//
// 推动的**几何计算**（WindowLayout::computePushDown）已经被 64 项单测
// 覆盖了，但那些只保证"算得对"。本步真正新引入的风险全在**接线**上：
//   * 展开那一刻高度动画才刚起步，喂给计算的是不是展开后的尺寸？
//   * 位移是**滑**过去的，还是瞬移？
//   * 卷起时有没有滑回原位？
//   * 被推开的位移有没有**偷偷落盘**？
//   * 钉住的窗口会不会被推？
// 这五条都只能在真实窗口 + 真事件循环里观察，单测一条也够不着。
//
// 【怎么驱动"展开"】
//
// 走双击标题栏这条真实路径，而不是直接调 applyRollUpState ——
// 后者会绕过 emit rollUpStateChanged 那一句，等于把要验的接线剪断了。
// 发鼠标事件时必须 Press + DblClick + Release 三个都发：
// 只发前两个的话标题栏的 m_dragging 会永久停在真上，
// 之后所有悬停判定都被挡住（这个坑在 hover_diag 里踩过一次）。
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

// 手工投递一个 enter 事件。
//
// ⚠️ 与 hover_diag 里同样的局限：绕过窗口系统，只验"收到事件之后的逻辑"。
// 本探针用它的目的是确认"钉住时即使收到了 enter 也不展开"。
static void sendEnterTo(QWidget *w)
{
    const QPointF local(20, 8);
    const QPointF global(w->mapToGlobal(local.toPoint()));
    QEnterEvent ev(local, local, global);
    QApplication::sendEvent(w, &ev);
}

namespace {

// 一个被测浮窗的抓手。
struct Win
{
    FloatingBoxWidget *w = nullptr;

    bool ok() const { return w != nullptr; }
    int  y() const { return w->frameGeometry().y(); }
    int  h() const { return w->height(); }
};

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyPushDiag"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyPushDiag"));

    QTextStream out(stdout);
    out << "=== 浮窗互相让位 / 平滑移动 / 钉住 ===\n\n";

    const QString root = QStringLiteral("F:/QtProject/DeskTidy/_pushdiag");
    QDir(root).removeRecursively();
    QDir().mkpath(root);

    AppService service;
    QString err;
    BoxManager::ensureRoot(root + QStringLiteral("/boxes"), &err);

    // 造两个盒。
    StorageBox boxA, boxB;
    BoxManager::createBox(root + QStringLiteral("/boxes"), QStringLiteral("上"), &boxA, &err);
    BoxManager::createBox(root + QStringLiteral("/boxes"), QStringLiteral("下"), &boxB, &err);
    for (const StorageBox &b : {boxA, boxB}) {
        QFile f(b.path + QStringLiteral("/a.txt"));
        if (f.open(QIODevice::WriteOnly)) { f.write("x"); f.close(); }
    }

    // 关掉置顶：探针里两个窗口叠着，置顶会让"谁盖住谁"影响观感判断
    //（本探针不验层级，只验坐标）。
    service.settings()->setAlwaysOnTop(false);

    FloatingBoxManager mgr(&service);

    auto openAndFind = [&](const QString &name, const QString &path) -> FloatingBoxWidget * {
        QSet<QWidget *> before;
        for (QWidget *w : QApplication::topLevelWidgets()) before.insert(w);

        mgr.openBox(name, path);
        pump(900);

        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (before.contains(w)) continue;
            if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
                if (fb->boxName() == name && fb->isVisible()) return fb;
            }
        }
        return nullptr;
    };

    Win up{openAndFind(QStringLiteral("上"), boxA.path)};
    Win dn{openAndFind(QStringLiteral("下"), boxB.path)};
    if (!up.ok() || !dn.ok()) {
        out << "浮窗没建出来（up=" << up.ok() << " dn=" << dn.ok() << "）\n";
        QDir(root).removeRecursively();
        return 1;
    }

    // ⚠️ 手动把两个窗口摆成"上下相邻且横向重叠"。
    //
    // 不能靠默认位置碰运气：默认位置是级联的，未必重叠。
    // 而本探针要验的正是"上面展开把下面推走"，前提必须成立。
    up.w->move(300, 100);
    dn.w->move(340, 200);
    pump(200);

    out << "起始布局：上 y=" << up.y() << " h=" << up.h()
        << " / 下 y=" << dn.y() << " h=" << dn.h() << "\n\n";

    // 找标题栏（双击它来展开）。
    auto titleBarOf = [](FloatingBoxWidget *fw) -> QWidget * {
        for (QObject *c : fw->children()) {
            if (auto *w = qobject_cast<QWidget *>(c)) {
                if (QString::fromLatin1(w->metaObject()->className())
                        .contains(QStringLiteral("TitleBar"))) {
                    return w;
                }
            }
        }
        return nullptr;
    };

    auto doubleClickTitle = [&](FloatingBoxWidget *fw) {
        QWidget *tb = titleBarOf(fw);
        if (!tb) return;
        const QPointF local(10, 10);
        const QPointF global(tb->mapToGlobal(QPoint(10, 10)));
        QMouseEvent press(QEvent::MouseButtonPress, local, global,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent dbl(QEvent::MouseButtonDblClick, local, global,
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent rel(QEvent::MouseButtonRelease, local, global,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(tb, &press);
        QApplication::sendEvent(tb, &dbl);
        QApplication::sendEvent(tb, &rel);
    };

    // =======================================================================
    out << "[1] 展开上面的浮窗 -> 下面的应当被推开（位置变大）\n";
    // =======================================================================
    // 先让"上"卷起，这样双击是"展开"。
    doubleClickTitle(up.w);
    pump(800);
    const bool collapsed = up.w->isRolledUp();
    out << "  上面的卷起态 = " << (collapsed ? "是" : "否")
        << "，高度 " << up.h() << "\n";
    check(collapsed, QStringLiteral("前提：上面的浮窗已卷起"), out);

    const int dnBefore = dn.y();
    out << "  下面的初始 y = " << dnBefore << "\n";

    // ⚠️ expandedGeometry() 必须在**展开的那一刻**问，不能在这里问。
    //
    // 踩过：一开始在这一行就取它，那时浮窗还是卷起的、高度动画也没跑，
    // 于是它老老实实返回当前几何（30px 的细线），打印出来是
    // "上面展开后高度 = 30" —— 看着像功能坏了，其实是问早了。
    // 这个函数回答的是"你展开后会有多高"，前提是**你已经决定要展开**。

    // 展开。同时采样"下"的 y 轨迹，用来判"是不是滑过去的"。
    QList<int> trace;
    doubleClickTitle(up.w);
    {
        // 展开刚触发、高度动画刚起步 —— 这正是 manager 算让位的那一刻。
        // 此刻问 expandedGeometry()，它应当报出**展开后**的高度（300），
        // 而不是当前那条 30px 的细线。
        const int targetH = up.w->expandedGeometry().height();
        out << "  展开动画刚起步时，上面报出的目标高度 = " << targetH
            << "（若是 30 说明 manager 会拿到卷起尺寸，功能会静默失效）\n";
        check(targetH > 200,
              QStringLiteral("★ 展开时报出的是展开后尺寸（不是 30px 细线）"), out);

        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 900) {
            pump(20);
            trace << dn.y();
        }
    }

    const int dnAfter = dn.y();
    out << "  下面的最终 y = " << dnAfter << "（位移 " << (dnAfter - dnBefore) << "）\n";
    check(dnAfter > dnBefore, QStringLiteral("★ 下面的浮窗被推下去了"), out);

    // 平滑性：轨迹里必须有既不是起点也不是终点的中间值。
    //
    // 只断言终值是**不够的** —— 那只能证明它到了，不证明它是滑过去的。
    // 瞬移的实现同样会通过"终值"断言。
    int distinctMiddles = 0;
    for (int v : trace) {
        if (v != dnBefore && v != dnAfter) ++distinctMiddles;
    }
    out << "  采样 " << trace.size() << " 帧，其中中间值 " << distinctMiddles << " 个\n";
    check(distinctMiddles >= 3,
          QStringLiteral("★ 是**滑**过去的（轨迹里有中间帧，不是瞬移）"), out);

    // =======================================================================
    out << "\n[2] 卷起上面的浮窗 -> 下面的应当滑回原位\n";
    // =======================================================================
    const int dnPushed = dn.y();
    doubleClickTitle(up.w);
    pump(900);
    const int dnBack = dn.y();
    out << "  卷起后下面的 y = " << dnBack
        << "（被推时 " << dnPushed << "，原位 " << dnBefore << "）\n";
    check(dnBack == dnBefore,
          QStringLiteral("★ 下面的浮窗滑回了原位"), out);
    check(false == dn.w->isPushedAside(),
          QStringLiteral("回原位后基线已清（否则下轮会越推越远）"), out);

    // =======================================================================
    out << "\n[3] 被推开的位移**不能**落盘\n";
    // =======================================================================
    // 做法：展开 -> 等落盘去抖（500ms）跑完 -> 读配置里的 blob 解码出的位置。
    // 若实现漏挡了，这里读到的就是被推开的位置。
    //
    // 用 saveGeometry/restoreGeometry 来解码：blob 里的字段是私有的，
    // 但"恢复到一个临时窗口上看它落在哪"是公开且等价的判据。
    doubleClickTitle(up.w);       // 展开（推下去）
    pump(1400);                   // 覆盖动画 + 去抖窗口

    const QByteArray blob = dn.w->currentGeometryBlob();
    {
        // 用"恢复到一个临时窗口上看它落在哪"来解码 blob。
        //
        // blob 的字段格式是 Qt 私有的，但 saveGeometry/restoreGeometry
        // 是一对公开且互相可逆的操作 —— 拿它当解码器，判据才不依赖
        // 二进制布局的细节（那种判据会在 Qt 版本一升就碎）。
        //
        // ⚠️ 这个窗口**不显示**（restoreGeometry 对未显示的窗口同样有效，
        // 它只设几何）。显示出来会多一个挡视线的窗口，还可能抢焦点。
        QWidget decode;
        decode.restoreGeometry(blob);
        const int decodedY = decode.geometry().y();
        out << "  配置里记录的 y = " << decodedY
            << "（原位 " << dnBefore << "，被推时 " << dnPushed << "）\n";
        check(decodedY == dnBefore,
              QStringLiteral("★ 落盘的是原位，不是被推开的位置"), out);
    }

    // 收尾：卷回。
    doubleClickTitle(up.w);
    pump(900);

    // =======================================================================
    out << "\n[4] 钉住下面的浮窗 -> 上面的展开不该推动它\n";
    // =======================================================================
    mgr.setBoxLocked(QStringLiteral("下"), true);
    pump(300);
    out << "  下面的是否已钉住 = " << (dn.w->isLocked() ? "是" : "否") << "\n";
    check(dn.w->isLocked(), QStringLiteral("前提：下面的浮窗已钉住"), out);

    // 钉住后它会回到原位（setBoxLocked 里会重算协调）。
    pump(500);
    const int dnLockedY = dn.y();

    doubleClickTitle(up.w);       // 展开
    pump(900);
    const int dnAfterPushAttempt = dn.y();
    out << "  展开上面的之后，下面的 y = " << dnAfterPushAttempt
        << "（钉住时 y = " << dnLockedY << "）\n";
    check(dnAfterPushAttempt == dnLockedY,
          QStringLiteral("★ 钉住的浮窗没有被推开"), out);

    doubleClickTitle(up.w);       // 卷回
    pump(900);

    // =======================================================================
    out << "\n[5] 钉住状态要能落盘（每盒一份）\n";
    // =======================================================================
    check(service.settings()->floatLocked(QStringLiteral("下")),
          QStringLiteral("配置里「下」是钉住的"), out);
    check(!service.settings()->floatLocked(QStringLiteral("上")),
          QStringLiteral("配置里「上」没被牵连"), out);

    // =======================================================================
    out << "\n[6] 钉住的浮窗自己展开时，也不该推别人\n";
    // =======================================================================
    {
        // 先把"下"解锁、把两者摆回相邻，用来确认基线可用。
        mgr.setBoxLocked(QStringLiteral("下"), false);
        pump(600);
        up.w->move(300, 100);
        dn.w->move(340, 200);
        pump(300);

        // 重新钉住"上"它自己，然后让它展开。
        mgr.setBoxLocked(QStringLiteral("上"), true);
        pump(600);

        const int dnY0 = dn.y();
        doubleClickTitle(up.w);   // 展开"上"（它自己是钉住的）
        pump(900);
        const int dnY1 = dn.y();
        out << "  钉住的「上」展开后，「下」的 y: " << dnY0 << " -> " << dnY1 << "\n";
        check(dnY1 == dnY0,
              QStringLiteral("★ 钉住的浮窗展开时也不推别人"), out);
    }

    // =======================================================================
    out << "\n[6b] ★回归：钉住时悬停**不**自动展开、也**不**自动卷起\n";
    // =======================================================================
    //
    // 主人要求："锁定功能开启时同时关闭自动展开与卷起"。
    //
    // 这两个方向要分开验，因为它们是两条不同的代码路径：
    //   * 自动展开 = canAutoExpand() -> hoverInteractionBlocked()
    //   * 自动卷起 = shouldAutoCollapse() -> hoverInteractionBlocked()
    // 只验一个方向的实现，很可能漏掉另一个 —— 比如只在 canAutoExpand
    // 里加了锁定判断，于是"鼠标移开时它自己卷起来"照样发生，
    // 而那正是主人上锁时最不希望看见的（他锁的多半正是展开态）。
    {
        mgr.setBoxLocked(QStringLiteral("上"), false);
        mgr.setBoxLocked(QStringLiteral("下"), false);
        pump(500);

        // ---- 方向一：卷起的窗口，钉住后悬停不该展开 ----
        if (!dn.w->isRolledUp()) { doubleClickTitle(dn.w); pump(800); }
        check(dn.w->isRolledUp(), QStringLiteral("前提：下面的是卷起的"), out);

        mgr.setBoxLocked(QStringLiteral("下"), true);
        pump(400);

        const int rolledH = dn.h();
        // 鼠标真的移到它上面（不是只发事件），因为展开路径里也看位置。
        QCursor::setPos(dn.w->frameGeometry().topLeft() + QPoint(40, 8));
        pump(60);
        sendEnterTo(dn.w);
        pump(FloatingBoxWidget::kHoverExpandDelayMs + 500);
        const int afterHoverH = dn.h();
        out << "  钉住且卷起，悬停 " << (FloatingBoxWidget::kHoverExpandDelayMs + 500)
            << " ms 后高度 = " << afterHoverH << "（卷起时 " << rolledH << "）\n";
        check(afterHoverH == rolledH,
              QStringLiteral("★ 钉住的浮窗悬停时**不**自动展开"), out);

        // ---- 方向二：展开的窗口，钉住后离开不该卷起 ----
        mgr.setBoxLocked(QStringLiteral("下"), false);
        pump(400);
        doubleClickTitle(dn.w);      // 手动展开（手动操作不受锁定影响）
        pump(800);
        const int expandedH = dn.h();
        check(expandedH > rolledH, QStringLiteral("前提：下面的已手动展开"), out);

        mgr.setBoxLocked(QStringLiteral("下"), true);
        pump(400);

        // 把鼠标挪开并投 leave。
        QCursor::setPos(dn.w->frameGeometry().bottomRight() + QPoint(600, 600));
        pump(80);
        {
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(dn.w, &leave);
        }
        pump(FloatingBoxWidget::kHoverCollapseDelayMs + 600);
        const int afterLeaveH = dn.h();
        out << "  钉住且展开，离开 " << (FloatingBoxWidget::kHoverCollapseDelayMs + 600)
            << " ms 后高度 = " << afterLeaveH << "（展开时 " << expandedH << "）\n";
        check(afterLeaveH == expandedH,
              QStringLiteral("★ 钉住的浮窗离开后**不**自动卷起"), out);

        // ---- 手动操作仍应可用（锁的是自动，不是主人）----
        doubleClickTitle(dn.w);
        pump(800);
        check(dn.h() < expandedH,
              QStringLiteral("★ 钉住不影响手动双击卷起"), out);

        mgr.setBoxLocked(QStringLiteral("下"), false);
        pump(500);
    }

    // =======================================================================
    out << "\n[7] ★回归：反复推开/收回不会逐轮漂移\n";
    // =======================================================================
    //
    // 防的是基线记账写错的那种失败：若每次被推都把"当前位置"误当成原位，
    // 浮窗每推一次就永久往下掉一截，几轮之后跑出屏幕 ——
    // 而且前一两轮看着完全正常，极难在手工点几下时发现。
    //
    // 这里跑四轮，每轮结束都必须精确回到 200。
    {
        mgr.setBoxLocked(QStringLiteral("上"), false);
        mgr.setBoxLocked(QStringLiteral("下"), false);
        pump(400);

        // ⚠️ 先把两边都"收回原位"，再摆位置。
        //
        // 踩过：直接 move() 而不先收基线，若某个窗口此刻正处于"被推开"
        // 状态（m_hasRestPos 为真），它的基线还指着旧的原位；
        // 下一次滑回时会被动画拉回**那个旧原位**，把刚摆好的布局冲掉 ——
        // 表现是"我明明把它挪回来了，它自己又跳回去"。
        // 摆位置之前必须让两者的让位状态都归零。
        if (up.w->isPushedAside()) { up.w->slideByForLayout(0); }
        if (dn.w->isPushedAside()) { dn.w->slideByForLayout(0); }
        pump(600);

        up.w->move(300, 100);
        dn.w->move(340, 200);
        pump(400);

        const int y0 = dn.y();
        bool stable = true;
        for (int round = 1; round <= 4; ++round) {
            // 每轮都从"上已展开、下在 y0"的确定状态起步。
            if (up.w->isRolledUp()) { doubleClickTitle(up.w); pump(800); }
            if (dn.y() != y0) { dn.w->move(340, y0); pump(300); }

            doubleClickTitle(up.w);   // 卷起
            pump(800);
            const int back = dn.y();

            doubleClickTitle(up.w);   // 再展开 -> 推下去
            pump(800);
            const int pushed = dn.y();

            out << "    第 " << round << " 轮：原位 " << y0
                << " -> 收回 " << back << " -> 推开 " << pushed << "\n";
            if (back != y0)   { stable = false; out << "      本轮回位不准\n"; }
            if (pushed <= y0) { stable = false; out << "      本轮没有被推开\n"; }
        }
        check(stable, QStringLiteral("★ 四轮推收之后位置没有累积漂移"), out);
    }

    out << "\n=== 结果：" << gPass << " 通过 / " << gFail << " 失败 ===\n";
    out << "\n[说明] 本探针验证的是**坐标与落盘**，验证不了\"动画看起来顺不顺\"\n";
    out << "       —— 那需要人眼看。多显示器同样没有覆盖。\n";
    out.flush();

    mgr.closeAll();
    QDir(root).removeRecursively();
    return gFail == 0 ? 0 : 1;
}
