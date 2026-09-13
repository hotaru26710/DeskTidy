#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QEnterEvent>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QMimeData>
#include <QMouseEvent>
#include <QSet>
#include <QTextStream>
#include <QTimer>

#include "appservice.h"
#include "boxmanager.h"
#include "floatingboxmanager.h"
#include "floatingboxwidget.h"
#include "itemlistwidget.h"

// ---------------------------------------------------------------------------
// hover_diag —— 悬停自动展开 / 离开自动卷起的行为验证。
//
// 【为什么必须有这个探针】
//
// 悬停展开的逻辑全挂在"定时器到点"和"鼠标进出事件"上，这两样都必须有
// **真事件循环**才会跑。单测里直接调函数看返回值的写法，对"250ms 之后
// 到底有没有展开"一个字都说明不了 —— 它最多能证明那个常量等于 250。
//
// 所以这里用 QApplication::sendEvent 手工投递 QEnterEvent / QEvent::Leave，
// 再用 QEventLoop + QTimer 推进**真实时间**，让悬停定时器自然到点。
//
// 【⚠️ 已知局限 —— 不要把它当成"悬停功能已验证"】
//
// 手工投递事件**绕过了窗口系统**。本探针验证的是"收到事件之后逻辑对不对"，
// 而不是"真实鼠标移动会不会产生 enter/leave"。后者必须人工把鼠标移上去看。
// 延迟手感（250/400ms 是否合适）同理，探针测不出来。
//
// 【状态机（当前规格）】
//
// 只有两条规则，都很短：
//   * 悬停进入 -> 停 250ms -> 展开（"悬停展开"，记下 m_hoverExpanded）
//   * 离开     -> 停 400ms -> **只有悬停展开的**才卷起
//
// 由此推出：
//   手动卷起 -> 悬停照样能展开（那次展开是悬停的，离开时会自动收回）
//   手动展开 -> 离开不卷（m_hoverExpanded 是 false）
//
// ⚠️ 曾经还有第三条"手动卷起就屏蔽悬停展开"（m_manualRolledUp），
// 已被主人实测推翻并删除：它会形成一个解不开的循环（见 [1] 段说明）。
// 本探针的 [1]/[1b] 两段就是钉这条新规格的，别改回旧方向。
//
// 【夹具】
//
// 每个 Fixture 用**独立的 AppService + 独立配置目录**，并显式清掉
// floatRolledUp 与 floatGeometry，保证从确定状态起步。
// 不这么做的话上一个夹具的状态会串进来，断言变成自比式的、恒真、什么也不验。
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

// 把光标真的移开窗口。
//
// ⚠️ 这一步不能省：shouldAutoCollapse() 里有一条"鼠标又回来了就不收"的检查
// （那是**正确**行为）。光标还停在窗口上时收起会被正确挡掉，
// 探针就会把一个正确行为报成失败。
static void moveCursorAway(const QWidget *w)
{
    QCursor::setPos(w->geometry().bottomRight() + QPoint(600, 600));
    pump(80);
}

static void sendEnter(QWidget *w)
{
    const QPointF local(20, 8);
    const QPointF global(w->mapToGlobal(local.toPoint()));
    QEnterEvent ev(local, local, global);
    QApplication::sendEvent(w, &ev);
}

static void sendLeave(QWidget *w)
{
    QEvent ev(QEvent::Leave);
    QApplication::sendEvent(w, &ev);
}

namespace {

const int kEnter  = FloatingBoxWidget::kHoverExpandDelayMs;
const int kLeave  = FloatingBoxWidget::kHoverCollapseDelayMs;
const int kMargin = 450;    // 比高度动画（220ms）宽裕

// 造一个浮窗：独立配置目录，保证每次状态干净。
struct Fixture
{
    AppService  service;
    QString     root;
    StorageBox  box;
    FloatingBoxManager *mgr = nullptr;
    FloatingBoxWidget  *fw  = nullptr;
    bool ok = false;

    explicit Fixture(const QString &tag)
    {
        root = QStringLiteral("F:/QtProject/DeskTidy/_hoverdiag_") + tag;
        QDir(root).removeRecursively();
        QDir().mkpath(root);

        QString err;
        BoxManager::ensureRoot(root + QStringLiteral("/boxes"), &err);
        BoxManager::createBox(root + QStringLiteral("/boxes"),
                              QStringLiteral("悬停"), &box, &err);
        {
            QFile f(box.path + QStringLiteral("/a.txt"));
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) { f.write("x"); f.close(); }
        }

        // ⚠️ 显式清掉"卷起"与"几何"两项配置，别继承上一次的状态。
        //
        // 这里踩了两层：
        //   1) 没清 rolledUp 时，浮窗带着 rolledUp=true 以**卷起态**出现，
        //      于是 [2] 里"离开后高度 == expandedH"拿到的 expandedH 是 30，
        //      断言恒真 —— 用例显示 PASS 却什么都没验证。
        //   2) 清了 rolledUp 但**没清几何**时，浮窗从上一次存下来的
        //      geometry blob 里恢复出一个 120 高的窗口（不是默认的 300），
        //      于是"手动展开后回到原高度"看起来像"展开后缩到了 120"，
        //      长得**非常像**产品 bug，实际是夹具串了状态。
        // 两个夹具共用同一个配置目录，所以每一项都必须显式清。
        service.settings()->setFloatRolledUp(QStringLiteral("悬停"), false);
        service.settings()->setFloatGeometry(QStringLiteral("悬停"), QByteArray());

        // 把"总在最前"关掉：探针里多个窗口叠加时，置顶会让光标位置判定
        // 变得不可预测。这一条只影响探针，不代表产品行为。
        service.settings()->setAlwaysOnTop(false);

        mgr = new FloatingBoxManager(&service);

        // ⚠️ 先记下"打开之前就已经存在的浮窗"，之后只认**新增的**那一个。
        //
        // 踩过：一开始直接遍历 topLevelWidgets 找"名字对且可见"的，
        // 于是会抓到**上一个夹具遗留下来的同名额窗口**（前一个 Fixture 的
        // 析构在某些时序下没那么快生效）。表现是新用例作用在旧窗口上，
        // 读到的初始高度是 30、断言全是空的 —— 更糟的是它长得像产品 bug。
        // 判据必须是"这次新建的"，而不是"名字碰巧一样"。
        QSet<QWidget *> before;
        for (QWidget *w : QApplication::topLevelWidgets()) {
            before.insert(w);
        }

        mgr->openBox(QStringLiteral("悬停"), box.path);
        pump(1200);

        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (before.contains(w)) {
                continue;   // 不是这次新建的
            }
            if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
                if (fb->boxName() == QStringLiteral("悬停") && fb->isVisible()) {
                    fw = fb;
                    break;
                }
            }
        }
        if (fw) {
            fw->setHoverExpandEnabled(true);
            ok = true;
        }
    }

    ~Fixture()
    {
        delete mgr;
        QDir(root).removeRecursively();
    }

    // 关掉再重开，并按配置里的 rolledUp 决定它以什么状态出现。
    //
    // 返回新窗口；找不到返回 nullptr。
    // 同样只认"这次新建的"，理由见构造函数里那段说明。
    FloatingBoxWidget *reopen()
    {
        mgr->closeBox(QStringLiteral("悬停"));
        pump(500);

        QSet<QWidget *> before;
        for (QWidget *w : QApplication::topLevelWidgets()) {
            before.insert(w);
        }

        mgr->openBox(QStringLiteral("悬停"), box.path);
        pump(1200);

        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (before.contains(w)) {
                continue;
            }
            if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
                if (fb->boxName() == QStringLiteral("悬停") && fb->isVisible()) {
                    fb->setHoverExpandEnabled(true);
                    return fb;
                }
            }
        }
        return nullptr;
    }

    // 标题栏双击（手动卷起 / 手动展开的真实用户路径）。
    QWidget *titleBar() const
    {
        for (QObject *child : fw->children()) {
            if (auto *w = qobject_cast<QWidget *>(child)) {
                if (QString::fromLatin1(w->metaObject()->className())
                        .contains(QStringLiteral("TitleBar"))) {
                    return w;
                }
            }
        }
        return nullptr;
    }

    void doubleClickTitleBar() const
    {
        QWidget *tb = titleBar();
        if (!tb) return;
        const QPointF local(10, 10);
        const QPointF global(tb->mapToGlobal(QPoint(10, 10)));
        QMouseEvent press(QEvent::MouseButtonPress, local, global,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent dbl(QEvent::MouseButtonDblClick, local, global,
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        // ⚠️ Release 不能省。标题栏的 mousePressEvent 会把 m_dragging 置真，
        // 只有 Release 才清掉它；而 m_dragging 为真时 hoverInteractionBlocked()
        // 返回真，悬停展开就永远不生效。
        //
        // 踩过：一开始只发 Press + DblClick，于是每次双击之后
        // "titleDrag=1" 一直挂着，后面所有悬停用例全部失败 ——
        // 而且表现得**特别像产品 bug**（"手动卷起后再也展开不了"，
        // 正是主人报的那个现象）。实际是我的夹具漏了半个事件序列。
        QMouseEvent release(QEvent::MouseButtonRelease, local, global,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(tb, &press);
        QApplication::sendEvent(tb, &dbl);
        QApplication::sendEvent(tb, &release);
    }
};

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyHoverDiag"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyHoverDiag"));

    QTextStream out(stdout);
    out << "=== 悬停自动展开 / 离开自动卷起 ===\n\n";
    out << "延迟常量：进入 " << kEnter << " ms / 离开 " << kLeave << " ms\n";
    out << "等待窗口：进入后等 " << (kEnter + kMargin)
        << " ms，离开后等 " << (kLeave + kMargin) << " ms\n\n";

    // =======================================================================
    out << "[1] 手动卷起的窗口，悬停**仍应**展开（新规格）\n";
    //
    // ⚠️ 这一段的方向被主人推翻过一次，别改回去。
    //
    // 旧规格是"手动卷起优先"：手动收起 = 主人不想看见内容，于是把悬停展开
    // 一并屏蔽，直到再手动展开一次才解除。
    // 问题在于那个解除条件**做不到**：手动卷起之后窗口是收起的，
    // 悬停又不展开，主人只剩"双击标题栏"这一条路 —— 而他还得先想到要去双击。
    // 表现就是"手动卷起一次之后，自动展开再也回不来了"（主人实测报的）。
    //
    // 新规格（主人拍板）：
    //   手动卷起 -> 悬停**仍能**自动展开
    //   手动展开 -> 鼠标离开**不**自动卷起
    // 也就是说"自动卷起"只对"悬停展开的窗口"生效 —— 这正好就是
    // m_hoverExpanded 的语义，于是"手动卷起优先"那一层标志整个不需要了。
    // =======================================================================
    {
        Fixture fx(QStringLiteral("manual"));
        if (!fx.ok) { check(false, QStringLiteral("夹具浮窗创建成功"), out); return 1; }

        moveCursorAway(fx.fw);
        const int expandedH = fx.fw->height();

        fx.doubleClickTitleBar();
        pump(700);
        const int rolledH = fx.fw->height();
        out << "  展开高度 " << expandedH << " -> 手动卷起后 " << rolledH << "\n";
        check(rolledH < expandedH / 2, QStringLiteral("手动卷起生效"), out);

        sendEnter(fx.fw);
        pump(kEnter + kMargin);
        const int afterHover = fx.fw->height();
        out << "  悬停 " << (kEnter + kMargin) << " ms 后 = " << afterHover << "\n";
        check(afterHover > rolledH,
              QStringLiteral("★ 手动卷起后悬停仍能展开"), out);

        // 展开之后离开，应当自动卷回去（这次是悬停展开的）。
        moveCursorAway(fx.fw);
        sendLeave(fx.fw);
        pump(kLeave + kMargin);
        out << "  离开 " << (kLeave + kMargin) << " ms 后 = " << fx.fw->height() << "\n";
        check(fx.fw->height() < afterHover,
              QStringLiteral("★ 悬停展开的窗口离开后仍会自动卷起"), out);
    }
    out << "\n";

    // =======================================================================
    out << "[1b] ★回归：手动卷起 / 悬停展开 可以反复循环，不会卡死\n";
    //
    // 这一段专门钉主人报的那个 bug 的另一半："再也无法触发自动展开和卷起"。
    // [1] 只验证了一轮，不足以说明状态机没有单向陷进去 ——
    // 反复交替手动与自动三次，每一轮都必须回到可用的状态。
    // =======================================================================
    {
        Fixture fx(QStringLiteral("loop"));
        if (!fx.ok) { check(false, QStringLiteral("夹具浮窗创建成功"), out); return 1; }

        moveCursorAway(fx.fw);
        const int baseH = fx.fw->height();
        check(baseH > 100, QStringLiteral("前提：初始是展开态"), out);

        bool allOk = true;
        for (int round = 1; round <= 3; ++round) {
            // ⚠️ 每一轮都必须从**展开态**起步，否则双击的方向是反的。
            //
            // 踩过两次：
            //   1) 第一轮结束时窗口是卷起的（上一步刚自动卷回去），
            //      第二轮开头那个"手动卷起"双击实际把它**展开**了 ——
            //      于是 r1=300，用例报"手动卷起失败"，看起来像产品没卷起。
            //   2) 想用"悬停展开再离开"来复位 —— 结果离开那一下又把它卷回去了，
            //      复位动作自己把自己撤销了。
            // 双击是 toggle，不能假设方向；直接用双击把状态摆到展开。
            if (fx.fw->height() < 100) {
                fx.doubleClickTitleBar();
                pump(700);
            }
            if (fx.fw->height() < 100) {
                allOk = false;
                out << "    第 " << round << " 轮：无法回到展开态，跳过\n";
                continue;
            }

            // 手动卷起
            fx.doubleClickTitleBar();
            pump(700);
            const int r1 = fx.fw->height();
            if (r1 >= 60) { allOk = false; out << "    第 " << round << " 轮手动卷起失败\n"; }

            // 悬停展开（这一条就是旧实现会失败的地方）
            sendEnter(fx.fw);
            pump(kEnter + kMargin);
            const int r2 = fx.fw->height();
            if (r2 <= r1) { allOk = false; out << "    第 " << round << " 轮悬停展开失败\n"; }

            // 离开自动卷起
            moveCursorAway(fx.fw);
            sendLeave(fx.fw);
            pump(kLeave + kMargin);
            const int r3 = fx.fw->height();
            if (r3 >= r2) { allOk = false; out << "    第 " << round << " 轮离开卷起失败\n"; }

            out << "    第 " << round << " 轮：手动卷起 " << r1
                << " -> 悬停展开 " << r2 << " -> 离开卷起 " << r3 << "\n";
        }
        check(allOk, QStringLiteral("★ 三轮「手动卷起→悬停展开→离开卷起」全部成立"), out);
    }
    out << "\n";

    // =======================================================================
    out << "[2] 手动展开的窗口，离开后**不**自动收起\n";
    //
    // 新规格的另一半：自动卷起只对"悬停展开的窗口"生效。
    // 主人自己双击展开来看盒里有什么，鼠标一移开就被收起来是很恼人的。
    //
    // 判据就是 m_hoverExpanded —— 手动展开时它被清成 false，
    // 于是 shouldAutoCollapse() 直接返回 false。
    // =======================================================================
    {
        Fixture fx(QStringLiteral("manualexpand"));
        if (!fx.ok) { check(false, QStringLiteral("夹具浮窗创建成功"), out); return 1; }

        moveCursorAway(fx.fw);
        const int expandedH = fx.fw->height();
        // 前提断言：夹具必须是**展开态**，否则下面的断言恒真、等于没测。
        // 这里刻意用绝对值（> 100）而不是"== 某个变量"，就是为了让
        // "夹具取态错了"这件事本身被发现，而不是被静默吞掉。
        check(expandedH > 100, QStringLiteral("夹具以展开态起步（前提）"), out);

        fx.doubleClickTitleBar();      // -> 卷起
        pump(700);
        check(fx.fw->height() < 60, QStringLiteral("手动卷起后是卷起态"), out);

        fx.doubleClickTitleBar();      // -> 展开（清 m_hoverExpanded）
        pump(700);
        out << "  手动展开后 = " << fx.fw->height() << "（期望 " << expandedH << "）\n";
        check(fx.fw->height() == expandedH, QStringLiteral("手动卷起再展开回到原高度"), out);
        // 第二条前提：展开后的高度必须是"像个正常浮窗"的量级。
        // 上面那条用的是自比，夹具一旦以 120 起步它就恒真、什么也证明不了。
        check(fx.fw->height() >= 200,
              QStringLiteral("展开后是正常尺寸（前提，防夹具串状态）"), out);

        moveCursorAway(fx.fw);
        out << "  挪开光标后 = " << fx.fw->height() << "\n";
        sendLeave(fx.fw);
        pump(kLeave + kMargin);
        out << "  离开 " << (kLeave + kMargin) << " ms 后 = " << fx.fw->height() << "\n";
        check(fx.fw->height() == expandedH,
              QStringLiteral("手动展开的窗口离开后不自动收起"), out);
    }
    out << "\n";

    // =======================================================================
    out << "[3] 核心路径：悬停展开 -> 离开自动卷起 -> 再悬停又展开\n";
    //
    // 用配置里的 rolledUp=true 造一个卷起态。
    // （新规格下其实"手动卷起"也能造出来，[1] 段就是这么干的；
    //  这里换一条路径，顺带把"配置恢复的卷起态同样可被悬停展开"也覆盖到。）
    // =======================================================================
    {
        Fixture fx(QStringLiteral("core"));
        if (!fx.ok) { check(false, QStringLiteral("夹具浮窗创建成功"), out); return 1; }

        moveCursorAway(fx.fw);
        check(fx.fw->height() > 100, QStringLiteral("浮窗默认以展开态出现（前提）"), out);

        fx.mgr->closeBox(QStringLiteral("悬停"));
        pump(500);
        fx.service.settings()->setFloatRolledUp(QStringLiteral("悬停"), true);
        FloatingBoxWidget *fw2 = fx.reopen();
        if (!fw2) { check(false, QStringLiteral("卷起态浮窗重建成功"), out); return 1; }

        const int startH = fw2->height();
        out << "  重建后高度 = " << startH << "（配置 rolledUp=true，期望约 30）\n";
        check(startH < 60,
              QStringLiteral("配置里的 rolledUp=true 使浮窗以卷起态出现"), out);

        moveCursorAway(fw2);
        sendEnter(fw2);
        pump(kEnter + kMargin);
        const int hoverH = fw2->height();
        out << "  悬停 " << (kEnter + kMargin) << " ms 后 = " << hoverH << "\n";
        check(hoverH > startH,
              QStringLiteral("★ 悬停把卷起的浮窗展开了"), out);

        // 离开 -> 应当自动卷起
        moveCursorAway(fw2);
        sendLeave(fw2);
        pump(kLeave + kMargin);
        const int afterLeaveH = fw2->height();
        out << "  离开 " << (kLeave + kMargin) << " ms 后 = " << afterLeaveH << "\n";
        check(afterLeaveH < hoverH,
              QStringLiteral("★ 离开后自动卷起"), out);

        // 再悬停一次 -> 又展开（防止"只能展开一次"）
        sendEnter(fw2);
        pump(kEnter + kMargin);
        out << "  再悬停后 = " << fw2->height() << "\n";
        check(fw2->height() > afterLeaveH,
              QStringLiteral("★ 再次悬停又能展开（可重复）"), out);
    }
    out << "\n";

    // =======================================================================
    out << "[4] 总开关关闭时，悬停完全不动作\n";
    // =======================================================================
    {
        Fixture fx(QStringLiteral("toggle"));
        if (!fx.ok) { check(false, QStringLiteral("夹具浮窗创建成功"), out); return 1; }

        moveCursorAway(fx.fw);

        fx.mgr->closeBox(QStringLiteral("悬停"));
        pump(500);
        fx.service.settings()->setFloatRolledUp(QStringLiteral("悬停"), true);
        FloatingBoxWidget *fw2 = fx.reopen();
        if (!fw2) { check(false, QStringLiteral("卷起态浮窗重建成功"), out); return 1; }

        fw2->setHoverExpandEnabled(false);
        const int before = fw2->height();
        out << "  关开关后高度 = " << before << "\n";

        moveCursorAway(fw2);
        sendEnter(fw2);
        pump(kEnter + kMargin);
        out << "  悬停 " << (kEnter + kMargin) << " ms 后 = " << fw2->height() << "\n";
        check(fw2->height() == before,
              QStringLiteral("开关关闭时悬停不展开"), out);

        // 打开开关，同一个窗口应当立刻可以被悬停展开。
        fw2->setHoverExpandEnabled(true);
        sendEnter(fw2);
        pump(kEnter + kMargin);
        out << "  打开开关后再悬停 = " << fw2->height() << "\n";
        check(fw2->height() > before,
              QStringLiteral("开关打开后悬停展开恢复正常"), out);
    }
    out << "\n";

    // =======================================================================
    out << "[5] ★回归：重新打开开关时鼠标**已经**停在浮窗上\n";
    //
    // 这是主人报的 bug 的精确复现。
    //
    // [4] 段之所以没抓到它：那里在重新打开开关之后又补发了一次 sendEnter，
    // 于是"即使实现有 bug 也会展开"，用例恒过、把问题盖住了。
    // 真实场景是：主人把鼠标挪到浮窗上 -> 发现没反应 -> 去设置里打开开关
    // -> **鼠标一直没动过**。此时窗口系统不会再发 enter（鼠标本来就在上面），
    // 所以没有任何时机去启动那个展开计时器。
    //
    // 期望行为（已拍板）：重新打开开关时立即补一次判断 ——
    // 若鼠标此刻已经在浮窗上，当场开始计时，不必把鼠标移开再移回来。
    // =======================================================================
    {
        Fixture fx(QStringLiteral("reenable"));
        if (!fx.ok) { check(false, QStringLiteral("夹具浮窗创建成功"), out); return 1; }

        fx.mgr->closeBox(QStringLiteral("悬停"));
        pump(500);
        fx.service.settings()->setFloatRolledUp(QStringLiteral("悬停"), true);
        FloatingBoxWidget *fw2 = fx.reopen();
        if (!fw2) { check(false, QStringLiteral("卷起态浮窗重建成功"), out); return 1; }

        const int rolledH = fw2->height();
        out << "  卷起态高度 = " << rolledH << "\n";

        // 关掉开关，并把鼠标**真的移到浮窗上**（不是 sendEnter 假装）。
        fw2->setHoverExpandEnabled(false);
        const QPoint inside = fw2->mapToGlobal(QPoint(fw2->width() / 2, 8));
        QCursor::setPos(inside);
        pump(150);
        out << "  鼠标已移到浮窗上，高度 = " << fw2->height() << "\n";
        check(fw2->height() == rolledH, QStringLiteral("开关关闭时鼠标停在上面也不展开"), out);

        // 关键：只打开开关，**不发任何 enter**，鼠标也不动。
        fx.mgr->setHoverExpandEnabled(true);
        pump(kEnter + kMargin);
        const int afterEnable = fw2->height();
        out << "  仅打开开关后（未动鼠标）等待 "
            << (kEnter + kMargin) << " ms = " << afterEnable << "\n";
        check(afterEnable > rolledH,
              QStringLiteral("★ 重开开关后立即生效（鼠标已在其上）"), out);
    }
    out << "\n";

    // =======================================================================
    out << "[6] 在浮窗内把条目拖出去时不该卷起（dragOutStarted 路径）\n";
    //
    // 与 [7] 的"从桌面拖进来"是相反方向的两条路径，别混：
    //   本段 = 从盒里往外拖（QDrag::exec() 是系统级鼠标抓取）
    //   [7]  = 从桌面往里拖（只有 dragEnter/dragLeave，没有 enter/leave）
    // 两条都必须在"拖拽期间不收起"，但用的机制不同。
    //
    // 用 m_dragOutActive 的公开等价物不好造，这里直接驱动
    // ItemListWidget::dragOutStarted 信号，验证屏障确实生效。
    // =======================================================================
    {
        Fixture fx(QStringLiteral("dragicon"));
        if (!fx.ok) { check(false, QStringLiteral("夹具浮窗创建成功"), out); return 1; }

        fx.mgr->closeBox(QStringLiteral("悬停"));
        pump(500);
        fx.service.settings()->setFloatRolledUp(QStringLiteral("悬停"), true);
        FloatingBoxWidget *fw2 = fx.reopen();
        if (!fw2) { check(false, QStringLiteral("卷起态浮窗重建成功"), out); return 1; }

        // 先悬停展开它，这样才有"可被收起"的状态。
        moveCursorAway(fw2);
        sendEnter(fw2);
        pump(kEnter + kMargin);
        const int expandedH = fw2->height();
        out << "  悬停展开后 = " << expandedH << "\n";
        check(expandedH > 100, QStringLiteral("前提：悬停确实展开了"), out);

        // 找到内嵌的条目列表并发一次 dragOutStarted。
        ItemListWidget *list = nullptr;
        for (QObject *c : fw2->children()) {
            if (auto *l = qobject_cast<ItemListWidget *>(c)) { list = l; break; }
        }
        if (!list) { check(false, QStringLiteral("找到内嵌条目列表"), out); return 1; }

        emit list->dragOutStarted();
        pump(50);

        // 拖动期间鼠标移出窗口 -> 不该收起
        moveCursorAway(fw2);
        sendLeave(fw2);
        pump(kLeave + kMargin);
        out << "  拖动期间离开 " << (kLeave + kMargin) << " ms 后 = " << fw2->height() << "\n";
        check(fw2->height() == expandedH,
              QStringLiteral("★ 拖动图标期间不自动卷起"), out);
    }
    out << "\n";

    // =======================================================================
    out << "[7] ★回归：从桌面拖文件过来时，卷起的浮窗应当展开\n";
    //
    // 主人报的问题：拖动桌面图标想存进浮窗时，靠近浮窗它不展开。
    //
    // 根因：拖放走的**不是** enterEvent 那条路 —— Windows 在拖拽期间
    // 只发 dragEnter/dragMove/dragLeave，不发 enter/leave。
    // 而卷起时列表是隐藏的、整个浮窗只剩标题栏可见，所以拖放流只有
    // 标题栏收得到。标题栏原先没开 acceptDrops，于是这条路径彻底哑火。
    //
    // 这个用例直接给标题栏投 QDragEnterEvent，模拟"拖着文件进入标题栏"。
    // =======================================================================
    {
        Fixture fx(QStringLiteral("dragin"));
        if (!fx.ok) { check(false, QStringLiteral("夹具浮窗创建成功"), out); return 1; }

        fx.mgr->closeBox(QStringLiteral("悬停"));
        pump(500);
        fx.service.settings()->setFloatRolledUp(QStringLiteral("悬停"), true);
        FloatingBoxWidget *fw2 = fx.reopen();
        if (!fw2) { check(false, QStringLiteral("卷起态浮窗重建成功"), out); return 1; }

        const int rolledH = fw2->height();
        out << "  卷起态高度 = " << rolledH << "\n";
        check(rolledH < 60, QStringLiteral("前提：浮窗是卷起的"), out);

        // 找标题栏。
        QWidget *tb = nullptr;
        for (QObject *c : fw2->children()) {
            if (auto *w = qobject_cast<QWidget *>(c)) {
                if (QString::fromLatin1(w->metaObject()->className())
                        .contains(QStringLiteral("TitleBar"))) {
                    tb = w; break;
                }
            }
        }
        if (!tb) { check(false, QStringLiteral("找到标题栏"), out); return 1; }

        check(tb->acceptDrops(),
              QStringLiteral("标题栏接受拖放（不开的话根本收不到事件）"), out);

        // 造一个带本地文件 URL 的拖放事件。
        QMimeData *mime = new QMimeData;
        mime->setUrls({QUrl::fromLocalFile(QStringLiteral("C:/Windows/notepad.exe"))});
        QDragEnterEvent dragEnter(QPoint(20, 8), Qt::CopyAction, mime,
                                  Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tb, &dragEnter);

        pump(kEnter + kMargin);
        const int afterDragIn = fw2->height();
        out << "  拖入后等待 " << (kEnter + kMargin) << " ms = " << afterDragIn << "\n";
        check(afterDragIn > rolledH,
              QStringLiteral("★ 拖着文件靠近时浮窗展开了"), out);

        // 拖走不放下 -> 应当按正常规则卷回去
        QDragLeaveEvent dragLeave;
        QApplication::sendEvent(tb, &dragLeave);
        moveCursorAway(fw2);
        pump(kLeave + kMargin);
        out << "  拖走后等待 " << (kLeave + kMargin) << " ms = " << fw2->height() << "\n";
        check(fw2->height() < afterDragIn,
              QStringLiteral("★ 拖走（未放下）后按正常规则卷回"), out);

        delete mime;
    }
    out << "\n";

    out << "=== 结果：" << gPass << " 通过 / " << gFail << " 失败 ===\n";
    out << "\n[说明] 本探针验证的是「收到 enter/leave 之后的逻辑」。真实鼠标\n";
    out << "       移动是否产生这些事件、以及 250/400ms 的延迟是否顺手，\n";
    out << "       探针测不出来，需要人工把鼠标移上去确认。\n";
    out.flush();

    return gFail == 0 ? 0 : 1;
}
