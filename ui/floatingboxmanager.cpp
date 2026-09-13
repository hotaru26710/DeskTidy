#include "floatingboxmanager.h"

#include "floatingboxwidget.h"

#include "appservice.h"
#include "boxmanager.h"
#include "corenames.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>

#include <algorithm>
#include <utility>      // std::as_const

namespace {

// "先收回、再重推"之间的等待时长。
//
// 必须大于一次让位动画（kLayoutSlideDurationMs = 200ms），否则重推时
// 读到的还是动画中间的位置，算出来的位移会偏。
// 留 60ms 余量覆盖定时器精度与事件循环排队。
//
// 为什么不干脆做成"动画结束再重推"的信号回调：那需要把 manager 和
// 浮窗的内部动画状态耦合起来，而这里只需要一个"差不多走完了"的时刻。
// 多等 60ms 对主人来说完全无感（他刚点完一个开关）。
constexpr int kRelayoutSettleMs = 260;

} // namespace

// ---------------------------------------------------------------------------
// 实现说明
//
// * 对象拥有关系：m_widgets 里的浮窗**归本类所有**。销毁一律走 deleteLater，
//   不直接 delete —— 因为销毁常常是在浮窗自己发来的信号处理函数里触发的
//   （closeRequested → closeBox），此时直接 delete 等于在对象自己的栈帧里
//   把它删掉，之后的代码碰到任何成员都是未定义行为。deleteLater 把销毁
//   推迟到事件循环下一轮，安全。
//
// * 为什么盒名是唯一键而不是路径：同一时刻一个盒最多一个浮窗（产品的明确选择）。
//   用盒名做键还能让配置里的开关记录天然去重。
//
// * 刷新转发：AppService 的信号是广播给所有消费者的（主窗口 + N 个浮窗）。
//   本类只负责把 boxContentsChanged 转给"相关的"浮窗 ——
//   带 boxPath 的意义就在这里：A 盒收纳完不该让 B、C 重新扫盘。
// ---------------------------------------------------------------------------

FloatingBoxManager::FloatingBoxManager(AppService *service, QObject *parent)
    : QObject(parent)
    , m_service(service)
{
    Q_ASSERT(m_service);    // 注入空指针是编程错误，不是运行时状况

    // 订阅盒内容变化。除了转发刷新，这里还兼做"盒目录被删"的检测。
    connect(m_service, &AppService::boxContentsChanged,
            this, &FloatingBoxManager::onBoxContentsChanged);
}

FloatingBoxManager::~FloatingBoxManager()
{
    // 浮窗是 QWidget，没有 parent 关系挂在 QObject 树上（它们的 parent 是 nullptr，
    // 因为顶层窗口不该有父窗口 —— 有父窗口就会跟着父窗口一起被销毁或限制在父窗口内）。
    // 所以这里必须自己清一遍，否则退出时会泄漏。
    closeAll();
}

// ---------------------------------------------------------------------------
// 启动恢复
// ---------------------------------------------------------------------------
void FloatingBoxManager::restoreOpenBoxes()
{
    const QStringList saved = m_service->settings()->openBoxNames();
    if (saved.isEmpty()) {
        return;
    }

    // 这一段是"批量恢复"，期间建的浮窗都跳过淡入。
    //
    // 为什么：启动时可能一次恢复五个浮窗，五个同时淡入会像程序在闪，
    // 而且它们本来就是"上次退出时就在那儿"的东西 —— 静悄悄出现才对。
    // 用 RAII 的置位/复位而不是在函数末尾手动清，是为了函数中途 return 时也安全。
    struct BatchGuard
    {
        bool *flag;
        explicit BatchGuard(bool *f) : flag(f) { *flag = true; }
        ~BatchGuard() { *flag = false; }
    } guard(&m_batchRestoring);

    // 先扫一遍真实存在的盒子，再拿它去对照配置。
    // 方向很重要：以**磁盘**为准去过滤配置，而不是反过来。
    // 若照配置无脑建浮窗，主人删掉一个盒之后会看到一堆指向空目录的浮窗。
    const QList<StorageBox> boxes = BoxManager::listBoxes(CoreNames::boxRoot());

    QHash<QString, QString> nameToPath;
    for (const StorageBox &box : boxes) {
        nameToPath.insert(box.name, box.path);
    }

    QStringList restored;
    for (const QString &name : saved) {
        const auto it = nameToPath.constFind(name);
        if (it == nameToPath.constEnd()) {
            // 盒目录已被删掉：丢弃该条目，不建浮窗。
            continue;
        }
        openBox(name, it.value());
        restored << name;
    }

    // 把过滤后的名单写回配置，清掉失效条目 ——
    // 否则每次启动都要重新过滤一遍这些死名字，且配置会越积越脏。
    if (restored.size() != saved.size()) {
        m_service->settings()->setOpenBoxNames(restored);
    }
}

// ---------------------------------------------------------------------------
// 开关浮窗
// ---------------------------------------------------------------------------
void FloatingBoxManager::openBox(const QString &boxName, const QString &boxPath)
{
    if (boxName.isEmpty() || boxPath.isEmpty()) {
        return;
    }

    // 已经开着：安全空操作，不重复建窗、不发信号。
    if (m_widgets.contains(boxName)) {
        // 但仍要把它提到前面 —— 主人点了"在桌面显示浮窗"却发现它被别的窗口
        // 盖住，会以为没生效。
        if (FloatingBoxWidget *existing = m_widgets.value(boxName)) {
            existing->raise();
        }
        return;
    }

    // 盒目录不在了就不建 —— 浮窗没有内容可显示，建出来只会让人困惑。
    if (!boxDirectoryExists(boxPath)) {
        return;
    }

    auto *widget = new FloatingBoxWidget(m_service, boxName, boxPath);

    // 恢复上次几何；没存过则用默认位置。
    widget->applySavedGeometry(m_service->settings()->floatGeometry(boxName));

    // 应用外观。放在 applySavedGeometry **之后**：
    // 几何恢复会把窗口尺寸还原成上次的大小，而外观（尤其图标模式）
    // 可能要求更大的尺寸 —— 顺序反了会被几何恢复覆盖掉。
    widget->applyAppearance(m_service->settings()->floatAppearance(boxName));

    connect(widget, &FloatingBoxWidget::closeRequested,
            this, &FloatingBoxManager::closeBox);

    connect(widget, &FloatingBoxWidget::geometryChanged,
            this, [this](const QString &name, const QByteArray &blob) {
                m_service->settings()->setFloatGeometry(name, blob);
            });

    connect(widget, &FloatingBoxWidget::revealInControlCenterRequested,
            this, &FloatingBoxManager::revealInControlCenterRequested);

    connect(widget, &FloatingBoxWidget::openAppearanceDialogRequested,
            this, &FloatingBoxManager::openAppearanceDialogRequested);

    // 右键菜单改外观 -> 统一走 applyAppearance 写配置并广播。
    connect(widget, &FloatingBoxWidget::appearanceChangeRequested,
            this, &FloatingBoxManager::applyAppearance);

    // 右键菜单改「悬停自动展开」-> 统一走 setHoverExpandEnabled 写配置并广播。
    connect(widget, &FloatingBoxWidget::hoverExpandChangeRequested,
            this, &FloatingBoxManager::setHoverExpandEnabled);

    // ⚠️ 新浮窗必须**立刻**同步一次悬停开关，不能等全局设置下次变化。
    //
    // 浮窗构造里把 m_hoverExpandEnabled 默认成了 true，而配置可能是 false
    //（主人关过）。不同步的话，新开的浮窗会自作主张地自动展开 ——
    // 表现为"设置里明明是关的，这个新窗口怎么还乱弹"，而且要等到主人
    // 再动一次那个开关才会自愈。
    //
    // 与动画开关不同，这里不能靠"构造时自己读配置"：那正是两个入口
    // 各读各的开始（见 setHoverExpandEnabled 的说明）。
    widget->setHoverExpandEnabled(m_service->settings()->hoverExpandEnabled());

    // 外观变更广播 -> 本浮窗重读并应用。
    //
    // 为什么在这里连、而不是让浮窗自己持有 manager 指针：
    // 浮窗只认识 AppService，反向持有 manager 会形成循环引用
    // （manager 拥有浮窗、浮窗又指回 manager）。
    // 由 manager 主动连到它自己创建的 widget 上，依赖方向始终是单向的，
    // 而且与上面三个信号的连接方式完全一致。
    connect(this, &FloatingBoxManager::boxAppearanceChanged,
            widget, [this, widget](const QString &changedBox) {
                // ⚠️ 这个判断不能省：三个浮窗同时开着时，
                // A 盒改外观不该让 B、C 白跑一遍重读配置 + 重设视图属性。
                if (changedBox != widget->boxName()) {
                    return;
                }
                widget->applyAppearance(
                    m_service->settings()->floatAppearance(changedBox));
            });

    m_widgets.insert(boxName, widget);

    // ---- 让位：展开时推开下方，卷起时收回 ----
    //
    // 直连（不用队列）：applyRollUpState 末尾 emit 时，本轮的高度动画
    // 与 min/max 都已经就位，expandedGeometry() 能拿到正确的终值。
    // 走队列连接的话会推迟到事件循环下一轮，那时窗口可能已经被
    // 主人又点了一下 —— 算出来的位移基于一个过期的状态。
    connect(widget, &FloatingBoxWidget::rollUpStateChanged,
            this, [this](const QString &name, bool rolledUp) {
                relayoutAround(m_widgets.value(name), !rolledUp);
            });

    // ---- 钉住 ----
    //
    // 走 manager 中转，而不是让浮窗自己写配置：钉住会改变"谁推谁"，
    // 而"谁推谁"只有 manager 有视野去重算。
    connect(widget, &FloatingBoxWidget::lockChangeRequested,
            this, &FloatingBoxManager::setBoxLocked);

    // 创建时按配置同步一次钉住状态。
    //
    // 否则新开的浮窗会一直用构造时的默认值（没钉住），
    // 表现为"我明明锁过这个盒，重开之后锁又没了"。
    widget->setLocked(m_service->settings()->floatLocked(boxName));

    // ---- 淡入的三步，顺序不能变 ----
    //
    // 1) prepareFadeIn：把不透明度先置 0。**必须在 show 之前** ——
    //    否则窗口会先以终值显示一帧，再跳回 0 重新淡入，看着像"闪一下才开始"。
    //
    // 2) show()。
    //
    // 3) fadeIn：从 0 动到终值。**必须在 show 之后** ——
    //    QPropertyAnimation 作用于 windowOpacity，而对尚未显示的窗口
    //    设不透明度是无效的（原生窗口还没建），动画会白跑一趟，
    //    窗口一显示就是终值，表现是"淡入完全没用"。
    //
    // 1 与 3 的时机要求正好相反，所以拆成了两个方法而不是合成一个。
    //
    // skip 只在"启动批量恢复"时为真（见 m_batchRestoring）：一次开出
    // 五个浮窗各淡入一次会像程序在闪，它们静悄悄出现才对。
    widget->prepareFadeIn(m_batchRestoring);
    widget->show();
    widget->fadeIn(m_batchRestoring);

    persistOpenBoxes();
    emit boxWindowToggled(boxName, true);
}

void FloatingBoxManager::closeBox(const QString &boxName)
{
    FloatingBoxWidget *widget = m_widgets.take(boxName);
    if (!widget) {
        return;     // 已经关着：安全空操作
    }

    // 关闭前兜底存一次几何，防止去抖定时器还没触发就被销毁。
    m_service->settings()->setFloatGeometry(boxName, widget->currentGeometryBlob());

    // deleteLater 而非 delete：本函数常常是在浮窗自己发出的
    // closeRequested 信号处理中被调用的，此时直接 delete 会在对象自己的
    // 栈帧里把它销毁掉，后续任何成员访问都是未定义行为。
    widget->hide();
    widget->deleteLater();

    persistOpenBoxes();
    emit boxWindowToggled(boxName, false);
}

bool FloatingBoxManager::isBoxOpen(const QString &boxName) const
{
    return m_widgets.contains(boxName);
}

QStringList FloatingBoxManager::openBoxNames() const
{
    return m_widgets.keys();
}

// ---------------------------------------------------------------------------
// 外观
// ---------------------------------------------------------------------------
void FloatingBoxManager::applyAppearance(const QString &boxName,
                                         const BoxAppearance &appearance)
{
    if (boxName.isEmpty()) {
        return;
    }

    m_service->settings()->setFloatAppearance(boxName, appearance);

    // 广播。浮窗是否开着都发 —— 关着的盒也要更新配置，
    // 下次打开时 openBox 会读到新值。广播是为了让已经开着的那一个立即跟上。
    //
    // 注意：发起改动的那个浮窗自己也会收到这次广播，于是它会把同一份外观
    // 再应用一遍（第一次是右键菜单里基于 m_appearance 改的，这一次是从
    // 配置读回来）。"应用两遍"在这里是**无害的**（幂等：设同样的透明度、
    // 同样的视图属性），换来的是"不需要区分发起者"这个简单结构 ——
    // 要区分就得让信号带一个 source 参数，那条路径上的复杂度
    // 远大于重设一遍属性的开销。
    emit boxAppearanceChanged(boxName);
}

BoxAppearance FloatingBoxManager::appearanceOf(const QString &boxName) const
{
    // 直接转发给 Settings：那里已经处理了"没配置过就返回默认值"
    // 与"值越界就夹到合法范围"两件事。在这里再兜一层只会让
    // 两处的默认值定义有机会漂移。
    return m_service->settings()->floatAppearance(boxName);
}

// ---------------------------------------------------------------------------
// 界面动画总开关
// ---------------------------------------------------------------------------
void FloatingBoxManager::setAnimationsEnabled(bool on)
{
    m_service->settings()->setAnimationsEnabled(on);

    // 逐个通知已经开着的浮窗。
    //
    // 为什么不发广播信号再让浮窗自己连：可以那么做，但这一项与"外观"
    // 那种"改一个盒只影响一个盒"的广播语义不同 —— 它是全局的，
    // 每个浮窗都要收到。直接遍历比"发一次广播 + 每个浮窗都写一遍
    // 名字匹配判断"更直白，也少一处需要写对的东西。
    for (FloatingBoxWidget *widget : std::as_const(m_widgets)) {
        if (widget) {
            widget->setAnimationsEnabled(on);
        }
    }
}

// ---------------------------------------------------------------------------
// 悬停自动展开总开关
// ---------------------------------------------------------------------------
void FloatingBoxManager::setHoverExpandEnabled(bool on)
{
    m_service->settings()->setHoverExpandEnabled(on);

    // 逐个通知已经开着的浮窗。理由与 setAnimationsEnabled 完全相同。
    for (FloatingBoxWidget *widget : std::as_const(m_widgets)) {
        if (widget) {
            widget->setHoverExpandEnabled(on);
        }
    }
}

// ---------------------------------------------------------------------------
// 钉住
// ---------------------------------------------------------------------------
bool FloatingBoxManager::isBoxLocked(const QString &boxName) const
{
    return m_service->settings()->floatLocked(boxName);
}

void FloatingBoxManager::setBoxLocked(const QString &boxName, bool locked)
{
    m_service->settings()->setFloatLocked(boxName, locked);

    FloatingBoxWidget *widget = m_widgets.value(boxName);
    if (!widget) {
        // 浮窗没开着也要写配置：主人可能先锁上、之后才打开浮窗。
        return;
    }

    widget->setLocked(locked);

    // 钉住状态变了，当前的让位关系要重算一次。
    //
    // 两个方向都有意义：
    //   * 刚锁上：它可能正压着别人，也可能自己正被别人推着（现在该回原位）。
    //   * 刚解锁：它可能正盖着别人，现在该把别人推开。
    //
    // 做法是把**所有**被推开的窗口先收回原位，再让当前没被钉住的、
    // 处于展开态的浮窗按布局顺序重新推一遍。这样无论锁还是解锁，
    // 结果都只由"当前状态"决定，与之前发生过什么无关 ——
    // 否则会残留上一次协调的位移，越积越乱。
    //
    // ⚠️ 收回是**异步动画**。所以这里不能马上接着算推动 ——
    // 那时窗口还在半路上，layoutItemsExcept 读到的 frameGeometry()
    // 是动画中间值，算出来的位移会偏。先等一轮动画走完
    //（kLayoutSlideDurationMs 是 200ms，留 260ms 余量），
    // 再重新协调。用 singleShot 而不是阻塞等待：阻塞会把整个界面冻住。
    for (FloatingBoxWidget *w : std::as_const(m_widgets)) {
        if (w && w->isPushedAside()) {
            w->slideByForLayout(0);
        }
    }

    QTimer::singleShot(kRelayoutSettleMs, this, [this]() {
        // ⚠️ 延时期间主人可能已经关掉了某些浮窗、或又改了锁定状态。
        // 所以这一刻重新取一次名单，而不是捕获上面那份快照。
        const QList<FloatingBoxWidget *> ordered = widgetsInLayoutOrder();
        for (FloatingBoxWidget *w : ordered) {
            if (w && !w->isRolledUp()) {
                relayoutAround(w, true);
            }
        }
    });
}

// ---------------------------------------------------------------------------
// 浮窗之间的空间协调
// ---------------------------------------------------------------------------
QList<FloatingBoxWidget *> FloatingBoxManager::widgetsInLayoutOrder() const
{
    QList<FloatingBoxWidget *> list;
    list.reserve(m_widgets.size());
    for (FloatingBoxWidget *w : std::as_const(m_widgets)) {
        if (w) {
            list.append(w);
        }
    }

    // 排序：先屏幕，再 y，再 x，最后盒名。
    //
    // ⚠️ 第一键必须是屏幕。多显示器时坐标是各自独立的 —— 副屏的 y=100
    // 与主屏的 y=100 毫无关系。若只按 y 排，两块屏的浮窗会被交错排在一起，
    // "推开下方"就会算出把主屏的浮窗推到副屏去的荒谬结果。
    //
    // 盒名作为最后一键是为了**完全确定性**：两个浮窗恰好同屏同位置时
    //（刚恢复配置、还没摆开），仍然有一个稳定顺序，不会每次跑出不同结果。
    std::sort(list.begin(), list.end(),
              [](FloatingBoxWidget *a, FloatingBoxWidget *b) {
                  if (!a || !b) return a != nullptr;

                  QScreen *sa = a->screen();
                  QScreen *sb = b->screen();
                  if (sa != sb) {
                      // 屏幕之间也要有个确定顺序：按屏幕几何左上角排。
                      // 直接用指针比大小在不同运行里结果不同，不可取。
                      const QRect ga = sa ? sa->geometry() : QRect();
                      const QRect gb = sb ? sb->geometry() : QRect();
                      if (ga.topLeft() != gb.topLeft()) {
                          if (ga.x() != gb.x()) return ga.x() < gb.x();
                          return ga.y() < gb.y();
                      }
                  }

                  const QRect ra = a->frameGeometry();
                  const QRect rb = b->frameGeometry();
                  if (ra.y() != rb.y()) return ra.y() < rb.y();
                  if (ra.x() != rb.x()) return ra.x() < rb.x();
                  return a->boxName() < b->boxName();
              });

    return list;
}

// ---------------------------------------------------------------------------
// 让位（推开下方浮窗）
// ---------------------------------------------------------------------------
//
// 【为什么只在"展开/卷起"这两个时机各算一次，而不是实时跟随】
//
// 实时跟随（拖动中也持续重算）看着更"物理"，但它有两个必然的坏结果：
//
// 1) **振荡**。A 展开推 B 下移 → B 的新位置又触发一轮计算 → 可能推回 A
//    或推 C，来回几次窗口在自己抖。WindowLayout::computePushDown 是
//    **一次算完**的纯函数，它假设输入是一个稳定布局；实时重算会把这个
//    假设打破，而它内部"连锁下推"的逻辑在抖动输入下会放大误差。
//
// 2) **和手动拖动抢位置**。主人正拖着窗口，程序同时按几何算出来的位置
//    挪它 —— 手感直接崩掉，而且他会觉得"这窗口不听话"。
//
// 所以：在状态**真正变化的那一刻**算一次，把结果用动画播出去。
//
// 【为什么位移算在 manager 而不是浮窗里】
// 算"谁该往下挪多少"必须看到**全部**浮窗（要按屏幕分组、按 y 排序、
// 还要排除被钉住的）。单个浮窗没有这个视野 —— 它不持有 manager 指针
//（那会形成循环引用，见头文件里的说明）。所以几何计算收在这里，
// 浮窗只负责"被要求滑到某个位置"。

QList<WindowLayout::Item> FloatingBoxManager::layoutItemsExcept(FloatingBoxWidget *anchor) const
{
    QList<WindowLayout::Item> items;

    for (FloatingBoxWidget *w : std::as_const(m_widgets)) {
        if (!w || w == anchor) {
            continue;
        }

        // 被钉住的浮窗不参与推动 —— 别人推不动它。
        //
        // ⚠️ 这个过滤必须在**这里**做，不能塞进 WindowLayout::computePushDown。
        // 那是 core 层的纯几何函数，只认识矩形；让它知道 UI 上有个"锁"按钮，
        // 等于把 UI 概念漏进了 core，之后 core 的单元测试也得被迫造锁的状态。
        if (w->isLocked()) {
            continue;
        }

        // 不可见的浮窗不占地方。正常流程下 m_widgets 里都是显示着的，
        // 但关闭动画（淡出）期间窗口可能已经 hide 而对象还在，
        // 那段时间里它不该继续把别人挡住。
        if (!w->isVisible()) {
            continue;
        }

        QScreen *scr = w->screen();
        if (!scr) {
            continue;
        }

        WindowLayout::Item item;
        item.id = w->boxName();
        // ⚠️ 用 frameGeometry 而不是 geometry：无边框窗口两者相同，
        // 但将来若加了阴影/边框边距，只有 frameGeometry 才是屏幕上
        // 真正占位的那块。WindowLayout 的注释里也是这么约定的。
        //
        // ⚠️ 位置取的是"让位动画落定后的终值"（layoutStableGeometry），
        // 而不是当前这一帧。连续快速让位时上一轮滑动还没跑完，用中间帧
        // 计算会让位移偏掉、把窗口叠在一起（见该函数的注释）。
        item.rect = w->layoutStableGeometry();
        // 用 availableGeometry 而不是 geometry：要避开任务栏，
        // 否则会把浮窗推到任务栏底下（那里看着是空的，实际点不到）。
        item.screenRect = scr->availableGeometry();
        items.append(item);
    }

    return items;
}

void FloatingBoxManager::relayoutAround(FloatingBoxWidget *anchor, bool expanded)
{
    if (!anchor) {
        return;
    }

    // 被钉住的浮窗不推别人。
    //
    // 与"别人推不动它"是同一条规则的两面：钉住 = 退出这套空间协调，
    // 既不接受位移也不施加位移。只做一半的话会出怪事 ——
    // 一个钉住的浮窗展开时把下面所有窗口推走，自己却纹丝不动，
    // 主人会以为锁坏了。
    if (anchor->isLocked()) {
        return;
    }

    // ---- 卷起：把之前被推开的收回原位 ----
    //
    // 这里**不**调 computePushDown。卷起只是自己变矮，腾出来的空间
    // 不需要谁去精确填补 —— 谁被推过谁就滑回自己的原位即可，
    // 而那正是各浮窗自己的 m_restPos 记得最准的东西。
    // 用几何重算反而会引入误差（比如某个浮窗在此期间被主人手动挪过，
    // 重算会把它按几何摆到另一个地方，而不是它原本待的地方）。
    //
    // ⚠️ 历史：这里曾经有个"已知的简化" —— 多个锚点同时展开时，
    // 收起其中一个会把另一个推开的窗口也一起收回原位，导致重叠。
    //
    // 例：A 展开推下 C；B 也展开（C 已经在下面，B 不需要再推它）；
    // 此时 A 收起 —— C 滑回原位，可 B 还展开着、本来就压在那个位置。
    // 结果是 C 与 B 叠在一起，要等下一次展开/收起才自愈。
    //
    // 现在改成"先归位、再让仍然展开的浮窗重推一遍"（见下面这段）：
    // C 先回到原位，紧接着被 B 重新推到 B 下方，不再重叠。
    // 不需要维护"谁推的 C"那张依赖图 —— 当前仍展开的窗口的真实占位
    // 就是答案，直接重算即可。
    if (!expanded) {
        for (FloatingBoxWidget *w : std::as_const(m_widgets)) {
            if (w && w != anchor && w->isPushedAside()) {
                w->slideByForLayout(0);     // 0 == "滑回原位"
            }
        }

        // 归位之后再让**仍然展开着**的其他浮窗重推一遍。
        //
        // 这一步不能省：上面那句只是"滑回原位"，而原位此刻可能仍然被
        // 另一个展开的浮窗占着。归位目标已经写进位移动画，而
        // layoutStableGeometry() 会读动画终值，所以这里重算时看到的是
        // C 归位后的位置，能正确把它再推到 B 下方。
        //
        // 顺序按 y 排：让靠上的锚点先推，与 computePushDown 内部
        // "上面先让开、下面再基于已让开的位置判断"保持一致，结果才稳定。
        QList<FloatingBoxWidget *> stillExpanded;
        for (FloatingBoxWidget *w : std::as_const(m_widgets)) {
            if (!w || w == anchor) {
                continue;
            }
            if (w->isRolledUp() || w->isLocked() || !w->isVisible()) {
                continue;   // 卷起的/钉住的/不可见的不占地方，也不推别人
            }
            stillExpanded.append(w);
        }
        std::sort(stillExpanded.begin(), stillExpanded.end(),
                  [](FloatingBoxWidget *a, FloatingBoxWidget *b) {
                      const QRect ra = a->frameGeometry();
                      const QRect rb = b->frameGeometry();
                      if (ra.y() != rb.y()) return ra.y() < rb.y();
                      if (ra.x() != rb.x()) return ra.x() < rb.x();
                      return a->boxName() < b->boxName();
                  });

        for (FloatingBoxWidget *w : std::as_const(stillExpanded)) {
            relayoutAround(w, true);
        }
        return;
    }

    // ---- 展开：算一次，推下方 ----
    QScreen *anchorScreen = anchor->screen();
    if (!anchorScreen) {
        return;
    }

    // ⚠️ 锚点的目标矩形要用**展开后**的尺寸。
    //
    // 麻烦之处：applyRollUpState(false) 是先启动高度**动画**再 emit 的，
    // 所以此刻 frameGeometry() 还是卷起时那条 30px 的细线。
    // 直接拿它去算等于"展开不会推开任何人"（细线挡不住谁）。
    //
    // 解决办法：不用当前几何，而是问浮窗"这轮让位动画落定后会在哪、
    // 会有多高"。layoutStableGeometry() 同时取高度与位置动画的终值，
    // 所以即使锚点自己还在被别的浮窗推着，也能算出正确目标。
    const QRect anchorTarget = anchor->layoutStableGeometry();

    const QList<WindowLayout::Shift> shifts =
        WindowLayout::computePushDown(anchor->boxName(),
                                      anchorTarget,
                                      anchorScreen->availableGeometry(),
                                      layoutItemsExcept(anchor));

    for (const WindowLayout::Shift &shift : shifts) {
        FloatingBoxWidget *w = m_widgets.value(shift.id);
        if (w) {
            w->slideByForLayout(shift.dy);
        }
    }
}

// ---------------------------------------------------------------------------
// 退出流程
// ---------------------------------------------------------------------------
void FloatingBoxManager::saveAllGeometry()
{
    for (auto it = m_widgets.constBegin(); it != m_widgets.constEnd(); ++it) {
        if (FloatingBoxWidget *widget = it.value()) {
            m_service->settings()->setFloatGeometry(it.key(), widget->currentGeometryBlob());
        }
    }
}

void FloatingBoxManager::closeAll()
{
    // 注意：这里**不**调 persistOpenBoxes() 去清空配置。
    // 退出时的关闭语义是"程序结束了"，不是"主人不想再看到这些浮窗"，
    // 下次启动应当恢复。若在这里清空，浮窗开关就变成了每次都要重设，
    // 那与"常驻"的定位矛盾。
    const QStringList names = m_widgets.keys();
    for (const QString &name : names) {
        FloatingBoxWidget *widget = m_widgets.take(name);
        if (!widget) {
            continue;
        }
        m_service->settings()->setFloatGeometry(name, widget->currentGeometryBlob());

        // 关掉信号连接：析构过程中浮窗可能还会发信号回来，
        // 而那时本类已经在销毁自己了。
        widget->disconnect(this);
        widget->hide();
        delete widget;      // 析构路径上不再走事件循环，直接 delete 即可
    }
}

// ---------------------------------------------------------------------------
// 盒内容变化 / 盒目录被删的检测
// ---------------------------------------------------------------------------
void FloatingBoxManager::onBoxContentsChanged(const QString &boxPath)
{
    if (m_widgets.isEmpty()) {
        return;
    }

    // 空串表示"不知道具体哪个盒"（撤销/还原场景），此时检查所有浮窗。
    if (boxPath.isEmpty()) {
        const QStringList names = m_widgets.keys();
        for (const QString &name : names) {
            FloatingBoxWidget *widget = m_widgets.value(name);
            if (!widget) {
                continue;
            }

            // 盒目录被主人在资源管理器里删掉了：浮窗已经没有意义，销毁它。
            // 这条是设计方案 §10.4 的"运行中"分支 —— 属于"自愈"，
            // 不能让浮窗留着一个空壳，那会让人以为盒还在。
            if (!boxDirectoryExists(widget->boxPath())) {
                closeBox(name);
            }
        }
        return;
    }

    // 具体某个盒变了：只刷新它自己的浮窗。
    // 三个浮窗同时开着时，A 盒收纳完不该让 B、C 重新扫盘。
    //
    // 哈希表的键是盒名而信号带的是路径，所以这里要按路径反查。
    // 浮窗数量在个位数，线性扫一遍远比维护"路径 -> 盒名"第二张表划算。
    FloatingBoxWidget *widget = nullptr;
    for (auto it = m_widgets.constBegin(); it != m_widgets.constEnd(); ++it) {
        if (it.value() && it.value()->boxPath() == boxPath) {
            widget = it.value();
            break;
        }
    }

    if (widget) {
        if (boxDirectoryExists(widget->boxPath())) {
            widget->refreshItems();
        } else {
            closeBox(widget->boxName());
        }
    }
}

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
void FloatingBoxManager::persistOpenBoxes()
{
    // 顺序按盒名排序后写入。不排序的话 QHash 的遍历顺序不确定，
    // 每次落盘 ini 的内容都不一样，diff 起来全是噪声。
    QStringList names = m_widgets.keys();
    names.sort(Qt::CaseInsensitive);
    m_service->settings()->setOpenBoxNames(names);
}

bool FloatingBoxManager::boxDirectoryExists(const QString &boxPath)
{
    if (boxPath.isEmpty()) {
        return false;
    }
    return QFileInfo(boxPath).isDir();
}
