#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QMouseEvent>
#include <QSet>
#include <QSettings>
#include <QTextStream>
#include <QTimer>

#include "appservice.h"
#include "boxmanager.h"
#include "floatingboxmanager.h"
#include "floatingboxwidget.h"

// ---------------------------------------------------------------------------
// overlap_diag —— 三个浮窗时的"仍然重叠"回归探针。
//
// 【为什么必须是探针】
//
// 三条缺陷都只在**三个浮窗**、窗口接连展开/收起时才暴露，
// 两个浮窗的单锚点布局怎么摆都碰不到：
//
//   [A] 二次推动把前一次让开的位移抹掉。
//       位移的语义有过一次错配 —— computePushDown 给的是"相对窗口**当前
//       稳定位置**还要往下挪多少"，而执行端一度把它当成"相对原位的绝对
//       偏移"用。于是 C 先被 A 推开一段、又被 B 二次推开时，第二次的
//       目标位置反而比第一次更靠上，C 不降反升，与 B 叠在一起。
//
//   [B] 多锚点收起时把别人推开的窗口一起收回原位。
//       A 展开推下 C；B 也展开（C 已经在下面，B 不需要再推它）；
//       此时 A 收起 —— C 如果直接滑回原位，而 B 还展开着占着那个位置，
//       两者就重叠。这条曾经是代码里写明的"已知简化"。
//
//   [C] 高度动画与位移动画争抢 geometry。
//       鼠标移动过快时，乙还在被甲往下推、动画没结束时就被展开。
//       曾经高度动画写整个 geometry（连位置一起写），会把位移动画接管掉，
//       乙永远停在半路；同时下一轮推挤又按这个中间位置去量丙，最终重新叠上。
//
//   [D] "滑回原位"的完成标记没有在再次被推开时取消。
//       第三个浮窗正要回原位，半路又被仍展开的第二个推下去；旧的
//       m_restClearPending 留到了新的下推动画结束，于是误清掉原位基线。
//       最后所有浮窗都收起时，第三个不再认为自己被推开过，便永远停在
//       中间位置回不到最初的原位。[D][E][F] 覆盖这条完整链路。
//
// 判据就是最朴素的一条：同一块屏幕上，任意两个**展开的**浮窗都不该在
// 纵向重叠（横向本来就并排的除外，本探针三个窗口横向完全重合）。
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

// 两个矩形在横纵两个方向是否**真的**交叠。
// 边界相接（a.bottom()+1 == b.top()）不算重叠 —— 那正是让位后的合法状态。
static bool overlaps(const QRect &a, const QRect &b)
{
    return a.left() < b.right() && b.left() < a.right()
        && a.top() < b.bottom() && b.top() < a.bottom();
}

static QString rectText(const QRect &r)
{
    return QStringLiteral("y=%1..%2 (h=%3)")
        .arg(r.top()).arg(r.bottom()).arg(r.height());
}

namespace {
struct Win
{
    FloatingBoxWidget *w = nullptr;
    bool ok() const { return w != nullptr; }
    QRect rect() const { return w->frameGeometry(); }
};
} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyOverlapDiag"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyOverlapDiag"));
    QSettings().clear();   // 探针必须可重复；上次运行残留的卷起/几何状态不许带进来

    QTextStream out(stdout);
    out << "=== 三浮窗二次推动 / 多锚点收起：重叠回归 ===\n\n";

    const QString root = QStringLiteral("F:/QtProject/DeskTidy/_overlapdiag");
    QDir(root).removeRecursively();
    QDir().mkpath(root);

    AppService service;
    QString err;
    BoxManager::ensureRoot(root + QStringLiteral("/boxes"), &err);

    StorageBox boxA, boxB, boxC;
    BoxManager::createBox(root + QStringLiteral("/boxes"), QStringLiteral("甲"), &boxA, &err);
    BoxManager::createBox(root + QStringLiteral("/boxes"), QStringLiteral("乙"), &boxB, &err);
    BoxManager::createBox(root + QStringLiteral("/boxes"), QStringLiteral("丙"), &boxC, &err);
    for (const StorageBox &b : {boxA, boxB, boxC}) {
        QFile f(b.path + QStringLiteral("/a.txt"));
        if (f.open(QIODevice::WriteOnly)) { f.write("x"); f.close(); }
    }

    // 探针里三个窗口叠着，置顶会让"谁盖住谁"影响观感判断（本探针只验坐标）。
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

    Win a{openAndFind(QStringLiteral("甲"), boxA.path)};
    Win b{openAndFind(QStringLiteral("乙"), boxB.path)};
    Win c{openAndFind(QStringLiteral("丙"), boxC.path)};
    if (!a.ok() || !b.ok() || !c.ok()) {
        out << "浮窗没建出来（甲=" << a.ok() << " 乙=" << b.ok()
            << " 丙=" << c.ok() << "）\n";
        QDir(root).removeRecursively();
        return 1;
    }

    auto titleBarOf = [](FloatingBoxWidget *fw) -> QWidget * {
        for (QObject *child : fw->children()) {
            if (auto *w = qobject_cast<QWidget *>(child)) {
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

    auto expandIfRolled = [&](Win win) {
        if (win.w->isRolledUp()) { doubleClickTitle(win.w); pump(850); }
    };
    auto collapseIfExpanded = [&](Win win) {
        if (!win.w->isRolledUp()) { doubleClickTitle(win.w); pump(850); }
    };

    // 把三个窗口恢复到"都卷起、都没有让位基线、摆到指定位置"的干净起点。
    //
    // ⚠️ 必须先清基线再 move()。若某个窗口还处于"被推开"状态，它的基线
    // 指着旧原位，下一次滑回会被拉回那里，把刚摆好的布局冲掉。
    auto resetTo = [&](int aY, int bY, int cY) {
        collapseIfExpanded(a);
        collapseIfExpanded(b);
        collapseIfExpanded(c);
        if (a.w->isPushedAside()) a.w->slideByForLayout(0);
        if (b.w->isPushedAside()) b.w->slideByForLayout(0);
        if (c.w->isPushedAside()) c.w->slideByForLayout(0);
        pump(800);
        a.w->move(300, aY);
        b.w->move(300, bY);
        c.w->move(300, cY);
        pump(400);
    };

    // =======================================================================
    out << "\n[A] 二次推动：C 先被甲推开、又被乙推开，不该叠回乙身上\n";
    // =======================================================================
    // 摆位：甲 y=100、乙 y=200、丙 y=300（都卷起）。
    //   甲展开 (100..399)  -> 乙被推到 400、丙被推到 430
    //   乙展开 (400..699)  -> 丙还要再让 270
    // 位移若是"相对原位"的覆盖式写法，丙会落到 300+270=570，
    // 正好埋在乙 (400..699) 里面；累加式写法才会落到 700。
    resetTo(100, 200, 300);
    const int baseH = a.rect().height();
    out << "  卷起时高度 = " << baseH << "\n";
    expandIfRolled(a);
    const QRect bAfterA = b.rect();
    const QRect cAfterA = c.rect();
    out << "  甲展开后：乙 " << rectText(bAfterA)
        << "，丙 " << rectText(cAfterA) << "\n";
    check(!overlaps(bAfterA, cAfterA),
          QStringLiteral("前提：甲展开时乙、丙都让开了，彼此不叠"), out);

    expandIfRolled(b);
    const QRect bExpanded = b.rect();
    const QRect cAfterB = c.rect();
    out << "  乙再展开后：乙 " << rectText(bExpanded)
        << "，丙 " << rectText(cAfterB) << "\n";
    check(!overlaps(bExpanded, cAfterB),
          QStringLiteral("★ 丙经过了两次推动，仍不在乙身上重叠"), out);

    // =======================================================================
    out << "\n[B] 多锚点收起：甲收起后，丙不该滑回乙正占着的位置\n";
    // =======================================================================
    // 摆位：甲 y=100、乙 y=200、丙 y=450（都卷起）。
    //   甲展开 (100..399)  -> 乙到 400；丙原位 450 没被甲压到，不动
    //   乙展开 (400..699)  -> 丙被推到 700
    //   甲收起            -> 丙如果直接回原位 450，就与仍展开的乙重叠
    resetTo(100, 200, 450);
    expandIfRolled(a);
    expandIfRolled(b);
    const QRect bExpanded2 = b.rect();
    const QRect cBeforeCollapse = c.rect();
    out << "  甲、乙都展开时：乙 " << rectText(bExpanded2)
        << "，丙 " << rectText(cBeforeCollapse) << "\n";
    check(!overlaps(bExpanded2, cBeforeCollapse),
          QStringLiteral("前提：乙展开时丙已经让开"), out);

    collapseIfExpanded(a);
    pump(400);
    const QRect bAfterCollapse = b.rect();
    const QRect cAfterCollapse = c.rect();
    out << "  甲收起后：乙 " << rectText(bAfterCollapse)
        << "，丙 " << rectText(cAfterCollapse) << "\n";
    check(!overlaps(bAfterCollapse, cAfterCollapse),
          QStringLiteral("★ 甲收起后丙没有滑回乙正占着的位置"), out);

    // =======================================================================
    out << "\n[C] 快速移动：乙还在被甲推着时就展开，最终也不该和丙重叠\n";
    // =======================================================================
    // 这是"鼠标移动过快"的时序：甲刚展开、乙的让位动画才跑了 30ms，
    // 乙马上又被展开。若锚点目标位置取的是当前中间帧，乙自己的落点会算偏，
    // 它再把丙推开的距离也会偏小，两个动画落定后就重新叠上。
    resetTo(100, 200, 350);
    doubleClickTitle(a.w);      // 甲开始展开，乙、丙开始让位
    pump(30);                   // 故意不等动画结束
    doubleClickTitle(b.w);      // 乙在被推途中立刻展开
    pump(1400);                 // 等所有高度/位置动画落定
    const QRect aFast = a.rect();
    const QRect bFast = b.rect();
    const QRect cFast = c.rect();
    out << "  快速展开落定后：甲 " << rectText(aFast)
        << "，乙 " << rectText(bFast)
        << "，丙 " << rectText(cFast) << "\n";
    check(!overlaps(aFast, bFast),
          QStringLiteral("★ 快速连续展开后甲、乙不重叠"), out);
    check(!overlaps(bFast, cFast),
          QStringLiteral("★ 快速连续展开后乙、丙不重叠"), out);

    // =======================================================================
    out << "\n[D] 第三个浮窗展开再收回：应回到展开前那个稳定位置\n";
    // =======================================================================
    // 前两个已经展开并把第三个推到了合法位置。第三个自己再展开、再收回，
    // 它应当回到"展开前已经被前两个推到的位置"，不能继续往下漂。
    resetTo(100, 200, 450);
    expandIfRolled(a);
    expandIfRolled(b);
    pump(500);
    const QRect cBeforeThirdExpand = c.rect();
    expandIfRolled(c);
    collapseIfExpanded(c);
    pump(700);
    const QRect cAfterThirdCollapse = c.rect();
    out << "  第三个展开前：丙 " << rectText(cBeforeThirdExpand)
        << "；收回后：丙 " << rectText(cAfterThirdCollapse) << "\n";
    check(cAfterThirdCollapse.top() == cBeforeThirdExpand.top(),
          QStringLiteral("★ 第三个收回后回到展开前的稳定位置"), out);

    // =======================================================================
    out << "\n[E] 第三个浮窗快速展开又收回：也不能漂到别处\n";
    // =======================================================================
    // 模拟鼠标扫过：第三个刚开始展开就立刻收回，此时高度/位置动画都还没落定。
    resetTo(100, 200, 450);
    expandIfRolled(a);
    expandIfRolled(b);
    pump(500);
    const QRect cBeforeFastToggle = c.rect();
    doubleClickTitle(c.w);      // 第三个开始展开
    pump(30);                   // 不等落定
    doubleClickTitle(c.w);      // 立刻收回
    pump(1200);
    const QRect cAfterFastToggle = c.rect();
    out << "  快速展开收回前：丙 " << rectText(cBeforeFastToggle)
        << "；收回后：丙 " << rectText(cAfterFastToggle) << "\n";
    check(cAfterFastToggle.top() == cBeforeFastToggle.top(),
          QStringLiteral("★ 快速展开再收回后仍回到展开前位置"), out);

    // =======================================================================
    out << "\n[F] 第三个收回后，前两个再收回：所有浮窗都回原位\n";
    // =======================================================================
    // 完整检查第三个浮窗的让位基线没有被它自己的展开/收回清掉：
    // 甲、乙后来也收起时，丙必须从被推位置回到最初的原位。
    resetTo(100, 200, 450);
    expandIfRolled(a);
    expandIfRolled(b);
    pump(500);
    expandIfRolled(c);
    collapseIfExpanded(c);
    pump(500);
    collapseIfExpanded(a);
    collapseIfExpanded(b);
    pump(900);
    const QRect cAfterAllCollapse = c.rect();
    out << "  甲、乙也收回后：丙 " << rectText(cAfterAllCollapse) << "\n";
    check(cAfterAllCollapse.top() == 450,
          QStringLiteral("★ 所有浮窗收回后丙回到最初原位 y=450"), out);

    // =======================================================================
    out << "\n=== 结果：" << gPass << " 通过 / " << gFail << " 失败 ===\n";
    out.flush();

    mgr.closeAll();
    QDir(root).removeRecursively();
    return gFail == 0 ? 0 : 1;
}
