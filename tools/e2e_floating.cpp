#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QPushButton>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QtMath>

#include "appservice.h"
#include "boxmanager.h"
#include "corenames.h"
#include "coretypes.h"
#include "floatingboxwidget.h"
#include "floatingboxmanager.h"
#include "itemlistwidget.h"

// ---------------------------------------------------------------------------
// e2e_floating —— 浮窗信号链路的自动化验证探针（验收工具，不是产品代码）。
//
// 【为什么需要它】
// 浮窗的"观感"（是否真置顶、拖标题栏跟不跟手、卷起动画顺不顺）只能靠人工看，
// 自动化验不了。但有一类 bug 自动化能抓、而且人工极难发现：
//
//   信号没接上 —— 收纳完成了，但浮窗列表还是旧的；
//                 撤销栈变了，但浮窗上按钮还灰着。
//
// 这类问题的表现是"界面看起来正常，只是数据不对"，人工点几下未必发现，
// 而它恰恰是多窗口架构（AppService + 主窗口 + N 个浮窗）最容易出错的接缝。
//
// 本探针把那条链路跑一遍：
//   建盒 -> 放文件 -> 建浮窗 -> 断言初始条目数
//        -> 通过 AppService 收纳 -> 断言浮窗自动刷新（这是核心）
//        -> 断言撤销按钮状态跟着变
//        -> 去掉一个文件 -> 断言刷新
//
// 全程在隔离目录里真实搬动文件，不碰主人的真实桌面与收纳盒。
// ---------------------------------------------------------------------------

static int gPass = 0;
static int gFail = 0;

static void check(bool ok, const QString &what)
{
    QTextStream out(stdout);
    if (ok) {
        ++gPass;
        out << "  [PASS] " << what << "\n";
    } else {
        ++gFail;
        out << "  [FAIL] " << what << "\n";
    }
    out.flush();
}

// 等界面动画跑完。
//
// ⚠️ 为什么必须有这个：浮窗的淡入/透明度过渡是 180ms 的 QPropertyAnimation。
// 只调 processEvents() 推一轮事件，动画才走了几毫秒 —— 这时去读
// windowOpacity() 会读到 0（刚开始）或 0.81（半路），于是断言以
// "透明度不对"的面貌失败，而真相只是探针跑得比动画快。
//
// 等待策略：轮询"值是否已经稳定"。比写死 sleep 更省时间，也比只 sleep
// 一次更可靠（动画时长将来若调整，这里不用跟着改）。
//
// ⚠️ 稳定判据取"连续 4 次读数相同"而不是 2 次：
// 曾出现过偶发失败（单独跑 34/34 全过、混在 verify.bat 里跑就红一条）。
// 原因是 180ms 的动画在负载高时会走得慢，两个轮询点之间可能只前进了
// 很微小的一步 —— 用 <0.001 判"没变"就会提前认为动画停了，
// 而此时实际值还差着最后一截。多要两次连续稳定能挡掉这种抖动。
static void waitForAnimations(int maxMs = 1500)
{
    QElapsedTimer timer;
    timer.start();

    double last = -1.0;
    int stableCount = 0;

    while (timer.elapsed() < maxMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

        QEventLoop loop;
        QTimer::singleShot(20, &loop, &QEventLoop::quit);
        loop.exec();

        // 收集当前所有顶层浮窗的透明度，看是否稳定
        double sum = 0.0;
        int count = 0;
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
                sum += fb->windowOpacity();
                ++count;
            }
        }
        const double current = count > 0 ? sum / count : 0.0;

        if (qAbs(current - last) < 1e-6) {
            if (++stableCount >= 4)
                return;         // 连续四次完全没变，认定动画停了
        } else {
            stableCount = 0;
        }
        last = current;
    }
}

// 从浮窗里把那个列表控件找出来（按类型递归找第一个 ItemListWidget）。
// 探针不参与产品设计，所以用这种"翻控件树"的方式拿内部状态，
// 免得为了可测性给产品类开一堆测试专用的访问器。
template <typename T>
static T *findChildOfType(QWidget *root)
{
    if (!root)
        return nullptr;
    const QList<T *> found = root->findChildren<T *>();
    return found.isEmpty() ? nullptr : found.first();
}

// 数一数浮窗标题栏上显示的条目数（从 QLabel 里捞）。
static QString titleTextOf(QWidget *box)
{
    if (!box)
        return QString();
    const QList<QLabel *> labels = box->findChildren<QLabel *>();
    QString all;
    for (QLabel *l : labels)
        all += l->text() + QLatin1Char('|');
    return all;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // 用独立的组织名，避免探针污染主人的真实配置。
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyFloatProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyFloatProbe"));

    QTextStream out(stdout);
    out << "=== DeskTidy 浮窗信号链路验证 ===\n\n";

    // ---- 清空本探针自己的配置，保证结果与运行历史无关 ----
    //
    // 为什么必须做：外观用例是"写配置 -> 读回 -> 断言"。
    // 若上一轮跑剩的值恰好与这一轮要写的一致，断言会在
    // "配置其实根本没写进去"的情况下也通过 —— 结果随历史漂移。
    // 清干净之后，任何"读回等于刚写的值"都只能是真写进去了。
    {
        const QString cfgDir =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        if (!cfgDir.isEmpty())
            QDir(cfgDir).removeRecursively();
    }

    const QString testRoot = QStringLiteral("F:/QtProject/DeskTidy/_float_tmp");
    const QString desk     = testRoot + QStringLiteral("/FakeDesktop");
    const QString boxes    = testRoot + QStringLiteral("/Boxes");

    QDir(testRoot).removeRecursively();
    QDir().mkpath(desk);
    QDir().mkpath(boxes);

    // 造两个"桌面文件"
    for (const QString &name : {QStringLiteral("alpha.txt"), QStringLiteral("beta.txt")}) {
        QFile f(desk + QLatin1Char('/') + name);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
            f.write("payload");
    }
    check(QDir(desk).entryList(QDir::Files).size() == 2,
          QStringLiteral("准备 2 个模拟桌面文件"));

    // ---- 建盒 ----
    AppService service;
    QString err;
    BoxManager::ensureRoot(boxes, &err);

    StorageBox box;
    BoxManager::createBox(boxes, QStringLiteral("浮窗测试"), &box, &err);
    check(QDir(box.path).exists(), QStringLiteral("收纳盒目录已创建"));

    // ---- 把两个文件先收进去，让盒里有内容 ----
    QList<DesktopEntry> entries;
    for (const QString &name : {QStringLiteral("alpha.txt"), QStringLiteral("beta.txt")}) {
        DesktopEntry e;
        e.filePath = desk + QLatin1Char('/') + name;
        e.name     = name;
        entries << e;
    }
    const QList<MoveRecord> recs = service.collectInto(entries, box.path,
                                                       QStringLiteral("浮窗测试"));
    check(recs.size() == 2, QStringLiteral("收纳 2 项完成"));

    // ---- 建浮窗 ----
    out << "\n[1] 创建浮窗\n";
    FloatingBoxWidget *fw = new FloatingBoxWidget(&service, QStringLiteral("浮窗测试"),
                                                  box.path);
    fw->show();
    QCoreApplication::processEvents();

    check(fw->isVisible(), QStringLiteral("浮窗已显示"));
    check(fw->boxName() == QStringLiteral("浮窗测试"), QStringLiteral("浮窗绑定了正确的盒名"));

    ItemListWidget *list = findChildOfType<ItemListWidget>(fw);
    check(list != nullptr, QStringLiteral("浮窗内嵌了条目列表控件"));

    if (!list) {
        out << "\n!!! 找不到列表控件，后续断言无法进行\n";
        out.flush();
        return 1;
    }

    check(list->count() == 2,
          QStringLiteral("浮窗列表初始显示 2 项（实际 %1）").arg(list->count()));

    // ---- 核心验证：信号链路 ----
    out << "\n[2] 收纳新文件后浮窗是否自动刷新（核心）\n";

    // 桌面上再放一个新文件，收纳它，然后**不手动调任何刷新**，
    // 只看浮窗有没有靠信号自己更新。
    //
    // 注意必须用花括号把 QFile 的生命周期限住：文件句柄没关的话，
    // Collector 搬它会失败（"复制成功但无法删除源文件"），
    // 于是探针会误报成"信号没接通" —— 这个坑我踩过一次。
    {
        QFile extra(desk + QStringLiteral("/gamma.txt"));
        if (extra.open(QIODevice::WriteOnly | QIODevice::Text))
            extra.write("payload");
        extra.close();
    }

    DesktopEntry ge;
    ge.filePath = desk + QStringLiteral("/gamma.txt");
    ge.name     = QStringLiteral("gamma.txt");
    QList<DesktopEntry> one;
    one << ge;

    const int before = list->count();
    service.collectInto(one, box.path, QStringLiteral("浮窗测试"));
    QCoreApplication::processEvents();

    check(list->count() == before + 1,
          QStringLiteral("浮窗列表自动从 %1 项变为 %2 项（信号链路已接通）")
              .arg(before).arg(list->count()));

    // 标题栏上的数量也应跟着变
    const QString title = titleTextOf(fw);
    check(title.contains(QStringLiteral("3")),
          QStringLiteral("标题栏数量同步更新为 3（标题文本：%1）").arg(title.simplified()));

    // ---- 撤销按钮状态 ----
    out << "\n[3] 撤销栈变化是否驱动按钮\n";
    check(service.canUndo(), QStringLiteral("AppService 报告可撤销"));

    const QList<QPushButton *> btns = fw->findChildren<QPushButton *>();
    bool foundUndoBtn = false;
    bool undoEnabled  = false;
    for (QPushButton *b : btns) {
        if (b->text().contains(QStringLiteral("撤销"))) {
            foundUndoBtn = true;
            undoEnabled  = b->isEnabled();
        }
    }
    check(foundUndoBtn, QStringLiteral("浮窗上有撤销按钮"));
    check(undoEnabled, QStringLiteral("撤销按钮已启用（undoStateChanged 已送达）"));

    // ---- 撤销后刷新 ----
    out << "\n[4] 撤销后浮窗是否刷新\n";
    const int beforeUndo = list->count();
    service.undoLast();
    QCoreApplication::processEvents();

    check(list->count() == beforeUndo - 1,
          QStringLiteral("撤销后浮窗列表从 %1 项变为 %2 项")
              .arg(beforeUndo).arg(list->count()));
    check(!service.canUndo(), QStringLiteral("撤销后栈已空"));

    // ---- 管理器生命周期 ----
    out << "\n[5] FloatingBoxManager 生命周期\n";
    FloatingBoxManager mgr(&service);
    mgr.openBox(QStringLiteral("浮窗测试"), box.path);
    check(mgr.isBoxOpen(QStringLiteral("浮窗测试")), QStringLiteral("管理器报告浮窗已打开"));
    check(mgr.openBoxNames().contains(QStringLiteral("浮窗测试")),
          QStringLiteral("管理器列出已打开的盒"));

    mgr.closeBox(QStringLiteral("浮窗测试"));
    check(!mgr.isBoxOpen(QStringLiteral("浮窗测试")), QStringLiteral("关闭后管理器不再报告该盒"));

    // 幂等性：重复关不应崩
    mgr.closeBox(QStringLiteral("浮窗测试"));
    check(true, QStringLiteral("重复关闭是安全空操作（未崩溃）"));

    // ---- 阶段 6：外观设置是否驱动已开浮窗 ----
    //
    // 防的是"改了没反应"这一类最难查的问题：设置写进了配置，
    // 但已经开着的浮窗没重读 —— 界面看着正常，只是显示还是旧样子。
    // 两个入口（浮窗右键、主窗口设置）都经过 manager 的同一个方法，
    // 所以这里测通了，两个入口就都对。
    //
    // ⚠️ 必须用 mgr.openBox() 建浮窗，**不能**自己 new。
    // 原因：外观广播的连接是在 FloatingBoxManager::openBox() 里为每个浮窗
    // 建立的（与 geometryChanged 那几条连接并列）。自己 new 出来的浮窗
    // 没有这条连接，manager 广播再多它也收不到 —— 那样测出来的"失败"
    // 是探针用错路径，不是产品的问题。
    //
    // 另外注意阶段 5 末尾已经把「浮窗测试」的浮窗关掉了，所以这里要重新开。
    //
    // 阶段 1 那个裸浮窗（fw）绑的也是同一个盒名，先把它的可见性收起来，
    // 免得 findFloating 在"同名浮窗"之间拿错对象。它在阶段 1~4 的使命已经完成。
    fw->hide();
    QCoreApplication::processEvents();
    out << "\n[6] 外观设置驱动浮窗\n";

    // ---- 前置能力探测：本环境的配置能不能落盘？----
    //
    // 这一步不是走过场。外观的整条链路是
    //   applyAppearance -> Settings 写 ini -> 读回 -> 应用到浮窗
    // 而 Settings 在 ini 路径不可写时会**静默 no-op**（它有
    // `if (m_iniPath.isEmpty()) return;` 的前置守卫，QSettings 写失败
    // 本身也不抛异常）。在受限权限的进程里 %LOCALAPPDATA% 可能写不进去，
    // 于是外观改了却读回默认值 —— 那看起来和"产品有 bug"一模一样。
    //
    // ⚠️ 探测必须用**两个不同的值**做往返，不能只写一次再读回一次：
    //   上一轮跑过的配置可能残留着恰好相同的值，于是"读回等于写入值"
    //   会被误判成"可写"（这个坑我踩过一次，导致结果随运行历史漂移）。
    //   写入两个不同的值并要求读回也跟着变，才能证明真的落盘了。
    bool configWritable = false;
    {
        const QString probeBox  = QStringLiteral("__cfgprobe__");
        const QString probeBox2 = QStringLiteral("__cfgprobe2__");

        BoxAppearance apA;
        apA.viewMode = BoxAppearance::ViewMode::LargeIcon;
        apA.opacity  = 70;
        service.settings()->setFloatAppearance(probeBox, apA);

        BoxAppearance apB;
        apB.viewMode = BoxAppearance::ViewMode::SmallIcon;
        apB.opacity  = 35;
        service.settings()->setFloatAppearance(probeBox2, apB);

        const BoxAppearance backA = service.settings()->floatAppearance(probeBox);
        const BoxAppearance backB = service.settings()->floatAppearance(probeBox2);

        // 两个盒必须各自读回自己那份 —— 这同时顺带验证了 per-box 隔离。
        configWritable = (backA.viewMode == BoxAppearance::ViewMode::LargeIcon
                          && backA.opacity == 70
                          && backB.viewMode == BoxAppearance::ViewMode::SmallIcon
                          && backB.opacity == 35);

        // 探测痕迹清掉，别污染后续用例
        service.settings()->setFloatAppearance(probeBox, BoxAppearance());
        service.settings()->setFloatAppearance(probeBox2, BoxAppearance());
    }

    if (!configWritable) {
        out << "  [SKIP] 本环境的配置目录不可写（权限受限），外观落盘链路无法验证；\n"
               "         浮窗本身的构造/刷新等断言仍会跑。\n";
    }

    // 怎么拿到浮窗对象：FloatingBoxManager 把 m_widgets 藏成 private 且
    // 没有"按盒名取浮窗"的公开接口（这是对的 —— 暴露它等于允许调用方
    // 绕过生命周期管理）。探针不参与产品设计，所以用 QApplication 的
    // 顶层窗口列表来找，等价且不改产品代码。
    //
    // ⚠️ 必须**优先返回可见的**那个。原因：阶段 1 是探针自己 new 的一个裸浮窗
    // （用来验证"不走 manager 也能响应 AppService 信号"），它绑的也是
    // 「浮窗测试」这个盒名；而阶段 6 是 mgr.openBox 建的生产浮窗，同名。
    // 两个同名浮窗同时存在于顶层窗口列表里时，若只按盒名匹配，拿到的可能是
    // 那个不受 manager 广播影响的裸浮窗 —— 于是断言会以"外观没生效"的
    // 面貌失败，而真正的问题只是探针找错了对象。
    auto findFloating = [](const QString &boxName) -> FloatingBoxWidget * {
        FloatingBoxWidget *fallback = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
                if (fb->boxName() != boxName)
                    continue;
                if (fb->isVisible())
                    return fb;
                if (!fallback)
                    fallback = fb;
            }
        }
        return fallback;
    };

    // 另开一个盒做对照，用于验证"只影响目标盒"。
    // 同样走 mgr.openBox —— 见上面关于"必须用生产路径创建浮窗"的说明。
    StorageBox boxB;
    BoxManager::createBox(boxes, QStringLiteral("对照盒"), &boxB, &err);
    mgr.openBox(QStringLiteral("对照盒"), boxB.path);

    // 阶段 5 末尾把「浮窗测试」的浮窗关掉了，这里重新打开，
    // 让下面能验证"改外观后已开浮窗立即跟上"。
    mgr.openBox(QStringLiteral("浮窗测试"), box.path);

    // 等淡入动画跑完再断言透明度 —— 否则会读到 0（起点）或中间值，
    // 把"探针跑太快"误报成"透明度不对"。见 waitForAnimations 的说明。
    waitForAnimations();

    // --- 6.1 默认外观：不改变行为 ---
    // 防的是：新加的"应用外观"逻辑在没配置过的情况下也去动窗口属性，
    // 把默认观感改坏了（浮窗变半透明 / 列表莫名变成图标网格）。
    {
        FloatingBoxWidget *target = findFloating(QStringLiteral("浮窗测试"));
        check(target != nullptr, QStringLiteral("能找到目标浮窗对象"));
        if (target) {
            check(qFuzzyCompare(target->windowOpacity(), 1.0),
                  QStringLiteral("默认浮窗不透明（opacity=%1）").arg(target->windowOpacity()));
            ItemListWidget *l = findChildOfType<ItemListWidget>(target);
            check(l && l->viewMode() == QListView::ListMode,
                  QStringLiteral("默认浮窗是列表模式"));
        }
    }

    // --- 6.2 改外观：已开浮窗立即跟上 ---
    if (configWritable) {
        BoxAppearance ap;
        ap.viewMode = BoxAppearance::ViewMode::LargeIcon;
        ap.opacity  = 70;
        mgr.applyAppearance(QStringLiteral("浮窗测试"), ap);
        // 透明度变化现在也是动画（180ms），必须等它跑完再断言。
        waitForAnimations();

        FloatingBoxWidget *target = findFloating(QStringLiteral("浮窗测试"));
        check(target != nullptr, QStringLiteral("改外观后仍能找到目标浮窗"));
        if (target) {
            // 透明度：70% -> 0.7
            check(qAbs(target->windowOpacity() - 0.7) < 0.01,
                  QStringLiteral("透明度立即变为 0.7（实际 %1）").arg(target->windowOpacity()));

            ItemListWidget *l = findChildOfType<ItemListWidget>(target);
            check(l && l->viewMode() == QListView::IconMode,
                  QStringLiteral("视图立即切到 IconMode（实际 %1）")
                      .arg(l ? int(l->viewMode()) : -1));
            check(l && l->iconSize() == QSize(64, 64),
                  QStringLiteral("大图标档的图标尺寸是 64×64（实际 %1×%2）")
                      .arg(l ? l->iconSize().width() : -1)
                      .arg(l ? l->iconSize().height() : -1));
            check(l && l->gridSize() == QSize(64 + 32, 64 + 40),
                  QStringLiteral("网格尺寸与图标尺寸配套（%1×%2）")
                      .arg(l ? l->gridSize().width() : -1)
                      .arg(l ? l->gridSize().height() : -1));

            // 防的是：图标模式默认 Movement=Free，主人能拖着图标乱放，
            // 而位置没地方保存，一次刷新就全乱 —— 必须锁成 Static。
            check(l && l->movement() == QListView::Static,
                  QStringLiteral("图标模式锁定了 Movement=Static（防图标被拖乱）"));
        }
    }

    // --- 6.3 只影响目标盒 ---
    // 防的是：外观写成了全局单值，改一个盒把所有盒都改了。
    {
        FloatingBoxWidget *other = findFloating(QStringLiteral("对照盒"));
        check(other != nullptr, QStringLiteral("对照盒浮窗仍在"));
        if (other) {
            check(qFuzzyCompare(other->windowOpacity(), 1.0),
                  QStringLiteral("对照盒透明度未被牵连（仍为 1.0，实际 %1）")
                      .arg(other->windowOpacity()));
            ItemListWidget *l = findChildOfType<ItemListWidget>(other);
            check(l && l->viewMode() == QListView::ListMode,
                  QStringLiteral("对照盒仍是列表模式（未被牵连）"));
        }
    }

    // --- 6.4 切回列表 ---
    // 防的是：切回列表时残留的网格/图标尺寸没复位，
    // 表现是列表模式下行高被撑开、条目之间莫名留白。
    if (configWritable) {
        BoxAppearance ap;
        ap.viewMode = BoxAppearance::ViewMode::List;
        ap.opacity  = 100;
        mgr.applyAppearance(QStringLiteral("浮窗测试"), ap);
        waitForAnimations();

        FloatingBoxWidget *target = findFloating(QStringLiteral("浮窗测试"));
        if (target) {
            ItemListWidget *l = findChildOfType<ItemListWidget>(target);
            check(l && l->viewMode() == QListView::ListMode,
                  QStringLiteral("切回列表后 viewMode 是 ListMode（实际 %1）")
                      .arg(l ? int(l->viewMode()) : -1));
            check(l && l->gridSize().isEmpty(),
                  QStringLiteral("切回列表后网格被清空（防行高被残留网格撑开）"));
            check(qFuzzyCompare(target->windowOpacity(), 1.0),
                  QStringLiteral("切回后透明度恢复 1.0"));
        }
    }

    // --- 6.5 透明度下限 ---
    // 防的是：主人把透明度拉到极低，浮窗淡到几乎看不见 ——
    // 而"找不到自己的浮窗"是不可恢复的故障（连改回来的入口都没了）。
    if (configWritable) {
        BoxAppearance ap;
        ap.viewMode = BoxAppearance::ViewMode::List;
        ap.opacity  = 1;        // 远低于 kMinOpacity(20)
        mgr.applyAppearance(QStringLiteral("浮窗测试"), ap);
        waitForAnimations();

        FloatingBoxWidget *target = findFloating(QStringLiteral("浮窗测试"));
        if (target) {
            const double floorValue = BoxAppearance::kMinOpacity / 100.0;

            // ⚠️ 容差必须大于 8 位 alpha 的量化步长（1/255 ≈ 0.0039）。
            //
            // 原因：Windows 的 windowOpacity 最终落到一个 BYTE 上，
            // setWindowOpacity(0.2) 会被量化成 alpha=50，读回来是
            // 50/255 = 0.196078 —— 比 0.2 小 0.0039。
            // 若容差取 0.001 就会把这条**正常的量化误差**误报成"突破了下限"。
            // 这里取 2/255，既容得下量化又仍能抓住"真的低于下限"。
            const double quantum = 2.0 / 255.0;
            check(target->windowOpacity() >= floorValue - quantum,
                  QStringLiteral("透明度不会低于下限 %1（实际 %2，量化步长 %3）")
                      .arg(floorValue).arg(target->windowOpacity())
                      .arg(quantum, 0, 'f', 6));
        }
    }

    // 浮窗对象归 FloatingBoxManager 所有（它按盒名持有并负责销毁），
    // 探针不 delete 它们 —— 让 mgr 在析构时自己收拾，那才是生产路径。
    mgr.closeAll();

    // 阶段 1 那个浮窗是探针自己 new 的（用来验证"不走 manager 的裸浮窗
    // 也能响应 AppService 信号"），它不属于 mgr，得自己收。
    delete fw;

    // ---- 汇总 ----
    out << "\n=== 结果：" << gPass << " 通过 / " << gFail << " 失败 ===\n";
    if (!configWritable) {
        out << "注意：本环境配置目录不可写，外观落盘链路未验证；\n"
               "      请在正常权限下重跑以覆盖该部分。\n";
    }
    out.flush();

    QDir(testRoot).removeRecursively();
    return gFail == 0 ? 0 : 1;
}
