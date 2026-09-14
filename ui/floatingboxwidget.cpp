#include "floatingboxwidget.h"

#include "itemlistwidget.h"
#include "previewdialog.h"

#include "appservice.h"
#include "boxmanager.h"
#include "corenames.h"
#include "deskscanner.h"
#include "opener.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QBitmap>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QEasingCurve>
#include <QEnterEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScreen>
#include <QSizeGrip>
#include <QTimer>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
// 实现说明
//
// * 窗口标志：Qt::Tool 让它不占任务栏、不进 Alt+Tab；FramelessWindowHint 去掉
//   系统标题栏（我们要自绘）；WindowStaysOnTopHint 来自配置且可关。
//
// * 关于圆角：**靠窗口遮罩实现，不用透明窗口属性**。
//
//   走过的弯路值得记下来，因为它很反直觉：
//
//   最初的做法是"开 Qt::WA_TranslucentBackground + 样式表 border-radius +
//   每层子控件自己补底色"。结果是「标题栏整块透明、只剩文字」，而且越修越糟
//   （中间一度把底色从样式表改到 palette，反而连操作条和手柄行也一起透了）。
//
//   根子在于把两件不相干的事绑在了一起：
//     - 圆角 = **二值裁剪**（这块区域算不算窗口）
//     - 半透明 = **alpha 混合**（这块区域画出来要多透明）
//   WA_TranslucentBackground 是给后者用的，它会让父窗口不再绘制背景，
//   于是每一层子控件都得自己画底色 —— 而"子控件自己画底色"在 Windows 上
//   并不可靠，实测下来底色会一层层地丢。
//
//   而圆角只需要前者。setMask() 设的窗口遮罩就能做到：被裁掉的区域，
//   系统根本不绘制窗口内容，直接透出桌面。窗口**保持不透明**，
//   底色由样式表按最常规的路径绘制，两边互不干扰。
//
//   所以现在的做法是：
//     - 窗口不开透明属性，样式表给白底 + 边框（常规路径，可靠）
//     - 四角由 updateRoundedMask() 裁掉，随尺寸变化在 resizeEvent 里重算
//
// * 拖动与列表拖拽的隔离：见 floatingboxwidget.h 里 FloatingBoxTitleBar 的注释。
//
// * 几何落盘去抖：见 scheduleGeometrySave。
//
// * 外观（透明度 + 视图模式）：见 applyAppearance。其中透明度**必须**用
//   setWindowOpacity 而不是样式表，理由写在该函数上方（列表的拖拽高亮会
//   清空整条样式表，用样式表做透明度会被拖拽操作冲掉）。
//
// * 外观的配置写入不在这里做：浮窗只发 appearanceChangeRequested，
//   由 FloatingBoxManager 统一写配置并广播。两个入口（浮窗右键、主窗口设置）
//   因此必然表现一致 —— 若各写各的，必然有一处漏发信号。
// ---------------------------------------------------------------------------

namespace {

// 浮窗的默认尺寸与最小尺寸。默认偏小（像一个桌面小组件），但保证三个中文
// 文件名不会被挤到只剩省略号。
constexpr int kDefaultWidth  = 260;
constexpr int kDefaultHeight = 300;
constexpr int kMinimumWidth  = 220;
constexpr int kMinimumHeight = 120;
constexpr int kTitleBarHeight = 28;

// 除标题栏外两段"装饰"的高度。算"图标模式需要多大"时要把它们算进去，
// 否则算出来的目标高度会少一截，图标照样被挤。
// 数值与 buildUi 里各段实际高度对应（操作条含上下 4px 内边距、手柄行 14px + 余量）。
constexpr int kActionBarHeight = 26;
constexpr int kGripRowHeight   = 16;

// 浮窗四角的圆角半径。
//
// 8px 是桌面小部件的常见取值：肉眼能看出是圆角，但不至于圆到像按钮。
// 它同时是"父窗口透明区域"的尺寸 —— 四个角上各有一小块 8×8 的透明区，
// 那就是圆角效果的来源（原理见文件头的实现说明）。
constexpr int kCornerRadius = 8;

// 淡入 / 淡出 / 透明度过渡的时长。
//
// 180ms 这个量级是刻意的：桌面小工具的动画应当"快到你注意不到它存在，
// 但拿掉就立刻感觉生硬"。再快（<100ms）看不出是动画，再慢（>250ms）
// 就开始有"等它"的感觉了 —— 而浮窗是要被频繁开关的东西。
constexpr int kFadeDurationMs = 180;

// 卷起 / 展开的时长。
//
// 比淡入稍长：高度变化是"形体在变"，比"明暗在变"更抢眼，用同一个时长
// 会显得仓促。220ms 仍在"不觉得在等"的范围内。
constexpr int kRollDurationMs = 220;

// 被其他浮窗推开（或滑回原位）时的平移时长。
//
// 比卷起略短（200 vs 220）：让位是"给别人腾地方"，应当比自己变形更利落；
// 两者接近则看着像同一套动作，不会觉得"两个东西各动各的"。
//
// 也不能更短：这是个纯位移，没有大小变化来提示"它在动"，
// 太快（<120ms）会被看成一帧跳变，反而失去了"平滑让开"的意义 ——
// 而这一步的全部目的就是让主人看出它是"被让开"而不是"瞬移"。
constexpr int kLayoutSlideDurationMs = 200;

// 双击标题栏卷起时的可见高度：就是标题栏本身的高度。
// 留 0 不额外加边距 —— 卷起后应当正好是一条。
int rolledUpHeight()
{
    return kTitleBarHeight + 2;   // +2 给上下边框留位置
}

// 弹模态框 / 菜单期间把某个布尔标志置真，作用域结束时复位。
//
// 为什么做成 RAII 而不是"进入前置真、退出后置假"两句：
// 这些 exec() 调用点之间都有 return（用户取消、条目为空、提前失败），
// 手写复位必然漏掉其中一条。而漏复位的后果不是"闪一下"，是
// **这个浮窗从此再也不响应悬停**，且没有任何报错 —— 极难查。
struct ModalGuard
{
    bool *flag;
    explicit ModalGuard(bool *f) : flag(f) { *flag = true; }
    ~ModalGuard() { *flag = false; }
};

// 统一的扁平小按钮样式。三处按钮（标题栏两个 + 操作条两个）共用一份，
// 避免各写一遍导致悬停色不一致这种细节破绽。
QString flatButtonStyle()
{
    return QStringLiteral(
        "QPushButton { border: none; background: transparent; padding: 2px 8px;"
        " border-radius: 3px; color: #3C4043; }"
        "QPushButton:hover { background: #E0E0E0; }"
        "QPushButton:pressed { background: #D0D0D0; }"
        "QPushButton:disabled { color: #B0B0B0; background: transparent; }");
}

// 标题栏上的图标按钮（卷起 / 关闭）更紧凑。
QString titleBarButtonStyle(bool isClose)
{
    return QStringLiteral(
               "QPushButton { border: none; background: transparent;"
               " padding: 0px; margin: 0px; border-radius: 3px;"
               " color: #5F6368; font-size: 13px; }"
               "QPushButton:hover { background: %1; color: %2; }"
               "QPushButton:pressed { background: %3; }")
        .arg(isClose ? QStringLiteral("#E81123") : QStringLiteral("#E0E0E0"),
             isClose ? QStringLiteral("#FFFFFF") : QStringLiteral("#202124"),
             isClose ? QStringLiteral("#C50F1F") : QStringLiteral("#D0D0D0"));
}

} // namespace

// ===========================================================================
// FloatingBoxTitleBar
// ===========================================================================

FloatingBoxTitleBar::FloatingBoxTitleBar(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(kTitleBarHeight);
    setCursor(Qt::ArrowCursor);
    // 标题栏要能收到鼠标事件做拖动，但它本身不是列表，
    // 不需要焦点 —— 抢焦点会让主窗口的快捷键失焦。
    setFocusPolicy(Qt::NoFocus);

    // 接住从桌面拖过来的文件。
    //
    // ⚠️ 这一句是"卷起的浮窗也能被拖拽唤醒"的前提。卷起时列表是隐藏的，
    // 从桌面拖来的拖放流只有标题栏能收到；不开 acceptDrops，Qt 根本不会
    // 把 dragEnterEvent 派发给它。
    //
    // 注意这里**不是**要接管拖放：标题栏从不 accept 这个事件，
    // 也不处理 drop。它只是把"有东西拖到我跟前了"这件事转告给浮窗
    //（用于触发展开），展开之后列表显出来，真正的落点判定照旧由列表负责。
    setAcceptDrops(true);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 0, 4, 0);
    layout->setSpacing(6);

    m_title = new QLabel(this);
    m_title->setStyleSheet(QStringLiteral("QLabel { font-weight: bold; color: #202124; }"));

    // 数量单独一个小标签、弱化显示，形如「3 项」。
    // 之所以不拼进标题文字里：拼进去就写死了格式，日后想改成图标+数字
    // 或者加"共"字都要动字符串拼接逻辑；分开后各管各的。
    m_count = new QLabel(this);
    m_count->setStyleSheet(QStringLiteral("QLabel { color: #80868B; font-size: 11px; }"));

    m_rollUpBtn = new QPushButton(QStringLiteral("—"), this);
    m_rollUpBtn->setFixedSize(22, 20);
    m_rollUpBtn->setStyleSheet(titleBarButtonStyle(false));
    m_rollUpBtn->setToolTip(tr("卷起 / 展开（也可以双击标题栏）"));
    m_rollUpBtn->setFocusPolicy(Qt::NoFocus);

    // 锁图标。
    //
    // 用文字符号而不是图片资源：本项目没有 .qrc，为一个小图标引入资源系统
    // 不划算。这两个字符在 Windows 的默认字体里有，且是等宽的方框形，
    // 与旁边的「—」「✕」观感一致。
    //
    // ⚠️ 它必须是标题栏的**直接子控件**，不能放进卷起时才显示的容器里 ——
    // 卷起状态下标题栏是整个浮窗唯一可见的部分，锁图标若跟着藏起来，
    // 主人就再也没法解锁了（只能先把窗口展开，而解锁往往正是为了
    // 阻止它被推开/被盖住，展开一次可能又被别人推走）。
    m_lockBtn = new QPushButton(QStringLiteral("🔓"), this);
    m_lockBtn->setFixedSize(22, 20);
    m_lockBtn->setStyleSheet(titleBarButtonStyle(false));
    m_lockBtn->setToolTip(tr("钉住：不被其他浮窗推开，并固定在最底层"));
    m_lockBtn->setFocusPolicy(Qt::NoFocus);

    m_closeBtn = new QPushButton(QStringLiteral("✕"), this);
    m_closeBtn->setFixedSize(22, 20);
    m_closeBtn->setStyleSheet(titleBarButtonStyle(true));
    m_closeBtn->setToolTip(tr("关闭这个浮窗（收纳盒与文件都不受影响）"));
    m_closeBtn->setFocusPolicy(Qt::NoFocus);

    layout->addWidget(m_title);
    layout->addWidget(m_count);
    layout->addStretch(1);
    layout->addWidget(m_lockBtn);
    layout->addWidget(m_rollUpBtn);
    layout->addWidget(m_closeBtn);

    connect(m_lockBtn,  &QPushButton::clicked, this, &FloatingBoxTitleBar::lockClicked);
    connect(m_rollUpBtn, &QPushButton::clicked, this, &FloatingBoxTitleBar::rollUpClicked);
    connect(m_closeBtn,  &QPushButton::clicked, this, &FloatingBoxTitleBar::closeClicked);
}

void FloatingBoxTitleBar::setLockedLook(bool locked)
{
    m_lockedLook = locked;

    // 锁着时用实心锁 + 主题色，没锁时用空心锁 + 灰色。
    //
    // 只靠字符本身区分不够：🔒 与 🔓 在小字号下很接近，扫一眼分不出来。
    // 换个颜色能让"锁着没有"在余光里就能看出来。
    if (locked) {
        m_lockBtn->setText(QStringLiteral("🔒"));
        m_lockBtn->setStyleSheet(
            QStringLiteral("QPushButton { border: none; background: transparent;"
                           " padding: 0px; margin: 0px; border-radius: 3px;"
                           " color: #1A73E8; font-size: 13px; }"
                           "QPushButton:hover { background: #E0E0E0; }"));
        m_lockBtn->setToolTip(tr("已钉住：不会被其他浮窗推开，且固定在最底层。点一下取消。"));
    } else {
        m_lockBtn->setText(QStringLiteral("🔓"));
        m_lockBtn->setStyleSheet(titleBarButtonStyle(false));
        m_lockBtn->setToolTip(tr("钉住：不被其他浮窗推开，并固定在最底层"));
    }
}

void FloatingBoxTitleBar::setTitle(const QString &boxName)
{
    m_title->setText(boxName);
}

void FloatingBoxTitleBar::setCountText(const QString &text)
{
    m_count->setText(text);
}

void FloatingBoxTitleBar::mousePressEvent(QMouseEvent *event)
{
    // 只响应左键拖动。右键留给右键菜单（由父窗口的 contextMenuEvent 处理）。
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    // 记录鼠标相对窗口左上角的偏移。注意要相对于**顶层窗口**而不是标题栏自身，
    // 否则算出来的位置会把标题栏的高度也算进偏移里，拖动时窗口会往上跳。
    QWidget *topLevel = window();
    m_dragOffset = event->globalPosition().toPoint() - topLevel->frameGeometry().topLeft();
    m_dragging   = true;
    event->accept();
}

void FloatingBoxTitleBar::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging || !(event->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    window()->move(event->globalPosition().toPoint() - m_dragOffset);
    event->accept();
}

void FloatingBoxTitleBar::mouseReleaseEvent(QMouseEvent *event)
{
    m_dragging = false;
    QWidget::mouseReleaseEvent(event);
}

void FloatingBoxTitleBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        emit doubleClicked();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

// ---------------------------------------------------------------------------
// 拖放流：只用来当"鼠标带着东西来了"的替身信号
// ---------------------------------------------------------------------------
//
// 【为什么标题栏要处理拖放】
//
// 从桌面拖文件过来时，Windows 走的是**拖放事件流**，不会产生 enterEvent ——
// 所以悬停展开那套逻辑（挂在 enterEvent 上）在这条路径上完全不会触发。
// 而卷起状态下列表、操作条、手柄行全被隐藏了，整个浮窗只剩标题栏可见，
// 于是拖放流也只有标题栏收得到。
//
// 不处理的后果很具体：主人从桌面拖一个文件想放进卷起的浮窗，
// 拖到跟前它死活不展开，也就没有地方可以放 —— 而"把桌面文件拖进来"
// 恰恰是这个浮窗最主要的用途之一。
//
// 【为什么这里不 accept、也不处理 drop】
//
// 标题栏的职责只是"报信"，不是"接管"。拖放能不能放、放到哪里，
// 一律由内嵌的 ItemListWidget 按原来的规则决定 —— 那是经过实测验证的
// 路径（见 preferreddropeffect.h 的硬约束）。若标题栏在这里 accept 了，
// 事件就不会继续传给列表，等于把已验证的投放逻辑顶掉了。
// 所以这里**只 emit 信号、不动事件**。

void FloatingBoxTitleBar::dragEnterEvent(QDragEnterEvent *event)
{
    // 只有拖的是文件才管：拖一段文字、一个网页链接过来不该把窗口炸开。
    // 这里用的是"有没有 URL"这个与列表一致的判据。
    if (event->mimeData() && event->mimeData()->hasUrls()) {
        emit dragHovered();
    }
    // ⚠️ 刻意不 accept：让事件继续冒泡，落点判定仍归列表。
    QWidget::dragEnterEvent(event);
}

void FloatingBoxTitleBar::dragLeaveEvent(QDragLeaveEvent *event)
{
    emit dragLeft();
    QWidget::dragLeaveEvent(event);
}

void FloatingBoxTitleBar::dropEvent(QDropEvent *event)
{
    // 在这一层什么都不做，交给列表（它才是真正的投放目标）。
    //
    // 但要把"拖拽悬停"状态收掉：主人可能直接把东西丢在标题栏上，
    // 那样不会再有 dragLeave，状态会一直卡在"拖着东西"上，
    // 导致这个浮窗再也不自动卷起。
    emit dragLeft();
    QWidget::dropEvent(event);
}

// ===========================================================================
// FloatingBoxWidget
// ===========================================================================

FloatingBoxWidget::FloatingBoxWidget(AppService *service,
                                     const QString &boxName,
                                     const QString &boxPath,
                                     QWidget *parent)
    : QWidget(parent)
    , m_service(service)
    , m_boxName(boxName)
    , m_boxPath(boxPath)
{
    Q_ASSERT(m_service);    // 注入空指针是编程错误，不是运行时状况

    setWindowTitle(boxName);
    // Tool: 不占任务栏、不进 Alt+Tab。Frameless: 自绘标题栏。
    // 置顶标志在 applyAlwaysOnTop 里按配置加，所以这里先不带 StaysOnTop。
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
    setMinimumSize(kMinimumWidth, kMinimumHeight);
    resize(kDefaultWidth, kDefaultHeight);

    // 浮窗是常驻的展示性窗口，不需要抢焦点；但列表要能接受键盘操作，
    // 所以只把窗口本身设为不抢焦点，子控件保持默认。
    setAttribute(Qt::WA_ShowWithoutActivating, false);

    buildUi();
    connectServiceSignals();

    // ---- 透明度动画对象 ----
    // 在这里建一次、之后反复复用（见头文件里 m_opacityAnim 的说明）。
    // targetObject 给 this，propertyName 是 QWidget 的 "windowOpacity" ——
    // 这个属性名是 Qt 定的字符串，写错不会编译报错、只会"动画没反应"，
    // 所以单独注释标一下。
    m_animationsOn = m_service->settings()->animationsEnabled();

    m_opacityAnim = new QPropertyAnimation(this, "windowOpacity", this);
    m_opacityAnim->setDuration(kFadeDurationMs);
    // OutCubic：快起慢收。符合"窗口出现"的物理直觉 ——
    // 起手要快要果断，收尾轻一点，不要"啪"地砸到位。
    m_opacityAnim->setEasingCurve(QEasingCurve::OutCubic);

    // 高度动画。
    //
    // ⚠️ 动的是 "geometry" 而不是某个 "height" 属性 —— QWidget 没有单列的
    // height 属性可以动画（size/geometry 才有）。动 geometry 时宽度保持不变，
    // 所以效果等价于"只动高度"。
    //
    // 缓动曲线与透明度一致（OutCubic）：卷起/展开和淡入淡出是一套观感，
    // 用不同的曲线会显得两块动画各演各的。
    //
    // 时长比透明度略长一点：高度变化是"形体改变"，比"明暗变化"更抢眼，
    // 用同一个时长会显得太仓促。
    m_heightAnim = new QPropertyAnimation(this, "size", this);
    m_heightAnim->setDuration(kRollDurationMs);
    m_heightAnim->setEasingCurve(QEasingCurve::OutCubic);

    // 位置动画（被其他浮窗推开时的平移）。
    //
    // ⚠️ 高度动画动的是 "size"、这里动的是 "pos"：两者严格正交，可以
    // 同时跑。历史上高度动画动的是 "geometry"（连位置一起写），被推开的
    // 浮窗若恰好也在卷起/展开，两个动画就会争抢同一个属性，表现为窗口
    // 停在让位中途、与旁边窗口重叠。改成只动 size 后两个动画不再打架。
    //
    // 时长比高度略短（200 vs 220）：让位是"给别人腾地方"，应当比
    // "自己变形"更利落一点；两者接近则看着像同一套动作。
    m_posAnim = new QPropertyAnimation(this, "pos", this);
    m_posAnim->setDuration(kLayoutSlideDurationMs);
    m_posAnim->setEasingCurve(QEasingCurve::OutCubic);

    // ⚠️⚠️ m_posAnimating 必须覆盖**整个动画区间**，所以挂在 stateChanged 上。
    //
    // 目标是"动画期间所有 moveEvent 都不落盘"。若只在起止两端手动置位，
    // 中间那些帧仍然会各自 scheduleGeometrySave() —— 而去抖是
    // "最后一次调用之后 500ms"，动画只有 200ms，于是动画结束后那次落盘
    // 用的正是被推开的位置。表现是"重启后浮窗停在上次被别人推开的地方"，
    // 而且只在"恰好被推开过、且期间没有别的落盘触发"时才复现，极难查。
    //
    // 用 stateChanged 而不是 finished：动画被 stop() 打断时
    // finished **不会**发，标志就会永久卡在真上 —— 那更糟，
    // 等于这个浮窗从此再也不记录任何几何变化。
    //
    // 这里用**独立**的 m_posAnimating 而不是复用一个 m_layoutAdjusting：
    // 后者在 applyRollUpState 的展开分支里另有一对置位/清除，
    // 而"展开"与"被推开"恰恰是同一时刻发生的 —— 那对清除会把动画期间的
    // 守卫抹掉，症状正是本步最想避免的"被推开的位置被落了盘"。
    connect(m_posAnim, &QPropertyAnimation::stateChanged,
            this, [this](QAbstractAnimation::State newState,
                         QAbstractAnimation::State oldState) {
                Q_UNUSED(oldState);
                m_posAnimating = (newState == QAbstractAnimation::Running);
            });

    // 滑回原位结束时清掉基线。
    //
    // ⚠️ 这条连接**只挂这一次**，靠 m_restClearPending 决定本次要不要清 ——
    // 不能改成"每次滑回时 connect 一个 lambda"：finished 是累积连接的，
    // 挂 N 次就会有 N 个回调，而这个动作会被反复触发
    //（连续收起、展开几个浮窗）。
    //
    // 为什么清基线要等到动画结束：见 slideByForLayout 里的说明 ——
    // 提前清会让"滑回途中又被推开"的场景把中途位置误记成原位。
    connect(m_posAnim, &QPropertyAnimation::finished, this, [this]() {
        if (m_restClearPending) {
            m_restClearPending = false;
            m_hasRestPos = false;
            m_layoutOffset = 0;     // 基线没了，累计偏移一起归零
        }
    });

    // 置顶按配置来（默认开）。
    // ⚠️ 此处只设标志、不 show()：构造函数尚未返回，控件还不该出现在屏幕上。
    // applyAlwaysOnTop 里的 show() 是给"运行中切换置顶"场景用的，
    // 构造期走 setWindowFlags 即可（标志在首次 show 时自然生效）。
    if (m_service->settings()->alwaysOnTop()) {
        setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);
    }

    // 卷起状态从配置恢复。
    // ⚠️ 顺序：先把 m_rolledUp 置为 false，调 applyRollUpState(true) 让它
    // 走"记下当前高度"那条分支，再把标志置真。若先置真，applyRollUpState
    // 会因为"已经是卷起状态"而跳过记录，展开时就回不到原高度了。
    m_rolledUp = false;
    if (m_service->settings()->floatRolledUp(m_boxName)) {
        applyRollUpState(true);
        m_rolledUp = true;
    }

    // 恢复成卷起态。这里**不需要**任何"这是手动收的还是自动收的"的区分 ——
    // 曾经有过那个区分（m_manualRolledUp），但它会形成死循环，已删除。
    // 现在手动卷起的窗口重启后照样能被悬停展开，这正是期望行为。
    // 详见 onToggleRollUp 里的说明。
    // 跨进程能持久化的只有"卷起"这个结果本身（上面那一段）。

    refreshItems();
    updateUndoButton();
}

void FloatingBoxWidget::buildUi()
{
    // ---- 圆角：靠窗口遮罩，不靠透明底 ----
    //
    // ⚠️⚠️ 这里**刻意不开** Qt::WA_TranslucentBackground。
    //
    // 一开始为了圆角顺手把它开了，结果是「标题栏整块透明、只剩文字」，
    // 越修越糟 —— 最后发现根子就在这一句。
    //
    // 原因：
    //   * WA_TranslucentBackground 的语义是"窗口**半透明**"（alpha 混合），
    //     它会让父窗口**不再绘制背景**，于是每一层子控件都得自己画底色。
    //     而在 Windows 上，"子控件自己画底色"这件事并不可靠 ——
    //     实测标题栏、操作条、手柄行的底色都会丢，露出下面的东西。
    //   * 而我们要的其实只是"**四角裁掉**" —— 那是二值裁剪，不是半透明。
    //     二值裁剪用 setMask() 就够了：被 mask 裁掉的区域，系统根本不会
    //     绘制窗口内容，直接透出桌面。
    //
    // 换句话说：**圆角和半透明是两件事，不该绑在一起做。**
    // 只做圆角的话，窗口保持不透明，底色由样式表正常绘制（这是最常规、
    // 最可靠的路径），四角由 mask 裁掉 —— 两者各司其职，互不干扰。
    //
    // 圆角由 updateRoundedMask() 实现，它在 resizeEvent 里随尺寸重算。

    // 常规的不透明窗口：底色 + 边框 + 圆角轮廓全部交给样式表。
    // 这是 Qt 里最标准的做法，没有透明窗口那些坑。
    setStyleSheet(QStringLiteral(
        "FloatingBoxWidget {"
        "  background: #FFFFFF;"
        "  border: 1px solid #C0C0C0;"
        "}"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ---- 标题栏 ----
    m_titleBar = new FloatingBoxTitleBar(this);
    m_titleBar->setTitle(m_boxName);
    // 标题栏底色略深于正文，并用一条底边线与列表分开 —— 这是"这是一条标题"
    // 最省事的视觉表达，不需要额外控件。
    //
    // ⚠️ 只给**顶部**两个圆角：底部与操作条接壤，圆了会在接缝处露出透明底，
    // 看起来像浮窗中间被咬了一口。
    //
    // 底色用样式表 —— 窗口现在是不透明的，样式表是最常规可靠的路径。
    // （中途曾因为开了 WA_TranslucentBackground 而把底色改到 palette 上，
    //   那是被透明窗口逼出来的权宜之计；不开那个属性就不需要了。）
    m_titleBar->setStyleSheet(QStringLiteral(
        "FloatingBoxTitleBar {"
        "  background: #F1F3F4;"
        "  border-bottom: 1px solid #DADCE0;"
        "  border-top-left-radius: %1px;"
        "  border-top-right-radius: %1px;"
        "}").arg(kCornerRadius));
    layout->addWidget(m_titleBar);

    connect(m_titleBar, &FloatingBoxTitleBar::closeClicked,
            this, &FloatingBoxWidget::fadeOutThenClose);
    connect(m_titleBar, &FloatingBoxTitleBar::rollUpClicked,
            this, &FloatingBoxWidget::onToggleRollUp);
    connect(m_titleBar, &FloatingBoxTitleBar::doubleClicked,
            this, &FloatingBoxWidget::onToggleRollUp);

    // 锁图标 -> 请求写配置并广播。
    //
    // 浮窗不自己写配置：钉住还会影响"谁推谁"，那需要 manager 重新组装
    // others 列表，浮窗没这个视野。走信号回去是最短的正确路径。
    connect(m_titleBar, &FloatingBoxTitleBar::lockClicked, this, [this]() {
        emit lockChangeRequested(m_boxName, !m_locked);
    });

    // ---- 从桌面拖东西过来 ----
    //
    // 这两条走的是与 enterEvent 完全不同的路径：拖放期间 Windows 不产生
    // enter/leave，只有 dragEnter/dragLeave。不接的话，"拖文件到卷起的
    // 浮窗上"这条最常用的用法根本触发不了展开。详见标题栏里的说明。
    connect(m_titleBar, &FloatingBoxTitleBar::dragHovered, this, [this]() {
        // ⚠️⚠️ 顺序很要命：必须**先判断、后置标志**。
        //
        // 踩过：一开始把 m_dragHoverActive = true 写在最前面，而
        // hoverInteractionBlocked() 又把它算作"交互中"之一，
        // 于是 canAutoExpand() 立刻返回 false —— 这个标志把自己挡住了。
        // 表现是"事件收到了、信号也发了，窗口就是不展开"，且完全不报错。
        // 所以标志只在"这次展开已经做完了"之后才置上，它的语义是
        // "有个拖拽正悬在这儿、并且已经按它展开过了"，不是"事件来过了"。
        if (canAutoExpand()) {
            // 立即展开 —— 这里**刻意不走 250ms 延迟**。
            //
            // 与"鼠标扫过"不同，拖动是一个明确的意图：人已经带着文件到跟前了，
            // 再让他悬停等待是多余的。而且拖拽状态下鼠标是被系统抓取的，
            // 多等 250ms 只会让手感发黏。
            m_hoverExpanded = true;
            m_rolledUp = false;
            applyRollUpState(false);
        }

        // 无论刚才展没展开，从现在起这个标志都要立起来：它接下来要负责
        // 挡住"投放途中窗口自己缩回去"（见 hoverInteractionBlocked）。
        m_dragHoverActive = true;

        // 拖拽期间那个"离开就卷"的计时必须停掉。
        // 拖着东西在窗口边缘晃的时候，dragLeave / dragEnter 会交替触发，
        // 留着一个已在跑的收起计时会让窗口在主人手底下忽开忽合。
        if (m_hoverCollapseTimer) {
            m_hoverCollapseTimer->stop();
        }
    });

    connect(m_titleBar, &FloatingBoxTitleBar::dragLeft, this, [this]() {
        m_dragHoverActive = false;

        // 拖离了但没放下 —— 按正常规则收回去（规格已拍板）。
        //
        // 复用悬停那一套计时与判定，不另写一份：收起该不该发生、
        // 鼠标是不是又回来了，这些规则已经在 shouldAutoCollapse 里，
        // 复制一份必然会有一处忘记同步。
        if (m_hoverExpanded) {
            if (m_hoverCollapseTimer) {
                m_hoverCollapseTimer->start();
            }
        }
    });

    // ---- 操作条 ----
    // 卷起时整条隐藏（连同下面的列表），所以给它单独的容器便于一次性显隐。
    m_actionBar = new QWidget(this);
    // 显式给底色：这是个纯容器，不设的话它会露出父窗口的白底 ——
    // 观感上没问题，但列表是 item 级交替色，操作条与它之间会缺一道分界。
    m_actionBar->setStyleSheet(QStringLiteral(
        "QWidget#actionBar { background: #FFFFFF; }"));
    m_actionBar->setObjectName(QStringLiteral("actionBar"));
    auto *actionLayout = new QHBoxLayout(m_actionBar);
    actionLayout->setContentsMargins(6, 4, 6, 4);
    actionLayout->setSpacing(6);

    m_collectBtn = new QPushButton(tr("收纳桌面"), m_actionBar);
    // 主操作按钮单独一份样式（主色填充），不能与 flatButtonStyle 拼接 ——
    // 拼接会让后出现的通用 QPushButton 规则覆盖掉主色背景，
    // 结果是按钮变成透明底、看不出可点。
    m_collectBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: #1A73E8; color: white; border: none;"
        " border-radius: 3px; padding: 3px 10px; }"
        "QPushButton:hover { background: #1765CC; }"
        "QPushButton:pressed { background: #14539F; }"
        "QPushButton:disabled { background: #C6D4E6; color: #F1F3F4; }"));
    m_collectBtn->setToolTip(tr("把桌面上的条目收进「%1」").arg(m_boxName));

    m_undoBtn = new QPushButton(tr("暂无可撤销"), m_actionBar);
    m_undoBtn->setStyleSheet(flatButtonStyle());

    actionLayout->addWidget(m_collectBtn);
    actionLayout->addWidget(m_undoBtn, 1);
    layout->addWidget(m_actionBar);

    connect(m_collectBtn, &QPushButton::clicked, this, &FloatingBoxWidget::onCollectDesktop);
    connect(m_undoBtn,    &QPushButton::clicked, this, &FloatingBoxWidget::onUndoClicked);

    // ---- 列表 ----
    // 浮窗专用 Options：双击=打开（不是还原），且允许拖出。
    ItemListWidget::Options opts;
    opts.doubleClick  = ItemListWidget::DoubleClickAction::Open;
    opts.draggableOut = true;
    opts.hideExtensions = true;

    m_itemList = new ItemListWidget(opts, this);
    // ⚠️ 列表的底色**不能用样式表设**。
    //
    // 原因：ItemListWidget::setDragHighlight() 在拖拽结束时执行
    // setStyleSheet(QString()) —— 那是**清空整条样式表**，不是"还原到之前的"。
    // 用样式表设的白底会被任何一次拖入/拖出冲掉，表现为"拖一个文件进去，
    // 浮窗底部突然透出一片透明"。这个坑在透明度那处也踩过一次（见文件头）。
    //
    // 改用调色盘：setStyleSheet 不碰 palette，拖拽高亮怎么清都影响不到它。
    QPalette listPal = m_itemList->palette();
    listPal.setColor(QPalette::Base, QColor(0xFF, 0xFF, 0xFF));
    listPal.setColor(QPalette::Window, QColor(0xFF, 0xFF, 0xFF));
    m_itemList->setPalette(listPal);
    m_itemList->setAutoFillBackground(true);
    layout->addWidget(m_itemList, 1);

    connect(m_itemList, &ItemListWidget::openRequested,
            this, &FloatingBoxWidget::onOpenRequested);
    connect(m_itemList, &ItemListWidget::dragOutFinished,
            this, &FloatingBoxWidget::onDragOutFinished);

    // 拖出期间屏蔽悬停判定 —— QDrag::exec() 是系统级鼠标抓取，
    // 浮窗在它跑的那段时间里收不到正常的 enter/leave。
    // 详见 ItemListWidget::dragOutStarted 的说明。
    connect(m_itemList, &ItemListWidget::dragOutStarted, this, [this]() {
        m_dragOutActive = true;
        // 顺手把两个悬停定时器停掉：拖动开始的那一刻鼠标已经在往外走，
        // 一个待触发的收起计时若留到拖拽结束后才炸，会在主人松手之后
        // 突然把窗口卷起来，看起来像"拖完文件窗口自己关了"。
        stopHoverTimers();
    });
    connect(m_itemList, &ItemListWidget::filesDropped,
            this, &FloatingBoxWidget::onFilesDropped);

    // ---- 右下角尺寸手柄 ----
    // 无边框窗口没有系统边框可拖，必须自己放一个 QSizeGrip，
    // 否则主人完全没法调整浮窗大小。
    // 单独一行容器是为了卷起时能连同它一起隐藏（卷起后调整大小没有意义）。
    m_gripRow = new QWidget(this);
    // 与操作条同理：窗口透明底之下，容器必须自带底色，否则露洞。
    // 手柄行在最底部，是圆角的一部分 —— 给它底部两个圆角，
    // 这样右下角的尺寸手柄也落在圆角轮廓内。
    m_gripRow->setObjectName(QStringLiteral("gripRow"));
    m_gripRow->setStyleSheet(QStringLiteral(
        "QWidget#gripRow {"
        "  background: #FFFFFF;"
        "  border-bottom-left-radius: %1px;"
        "  border-bottom-right-radius: %1px;"
        "}").arg(kCornerRadius));
    auto *gripLayout = new QHBoxLayout(m_gripRow);
    gripLayout->setContentsMargins(0, 0, 0, 0);
    gripLayout->addStretch(1);

    m_sizeGrip = new QSizeGrip(m_gripRow);
    m_sizeGrip->setFixedSize(14, 14);
    gripLayout->addWidget(m_sizeGrip, 0, Qt::AlignBottom | Qt::AlignRight);
    layout->addWidget(m_gripRow, 0);

    // ---- 几何去抖定时器 ----
    m_geometryDebounce = new QTimer(this);
    m_geometryDebounce->setSingleShot(true);
    m_geometryDebounce->setInterval(500);
    connect(m_geometryDebounce, &QTimer::timeout,
            this, &FloatingBoxWidget::onGeometryDebounceTimeout);

    // ---- 悬停自动展开的两个定时器 ----
    //
    // 都 singleShot：表达的是"某个时刻之后做一件事"，不是周期性任务。
    // start() 重开会自然重置计时 —— 这正是"每次进入都重新计时"要的语义。
    //
    // 延迟值取自头文件里的 static constexpr 常量，而不是在这里写死数字：
    // 单元测试要引用它们来断言"延迟确实是 250/400"，
    // 两处各写一遍必然漂移（改了实现忘了改测试，测试仍然全绿 —— 比没有测试更糟）。
    m_hoverExpandTimer = new QTimer(this);
    m_hoverExpandTimer->setSingleShot(true);
    m_hoverExpandTimer->setInterval(kHoverExpandDelayMs);
    connect(m_hoverExpandTimer, &QTimer::timeout,
            this, &FloatingBoxWidget::onHoverExpandTimeout);

    m_hoverCollapseTimer = new QTimer(this);
    m_hoverCollapseTimer->setSingleShot(true);
    m_hoverCollapseTimer->setInterval(kHoverCollapseDelayMs);
    connect(m_hoverCollapseTimer, &QTimer::timeout,
            this, &FloatingBoxWidget::onHoverCollapseTimeout);

    // ---- 尺寸手柄拖拽期间要挡住悬停判定 ----
    //
    // QSizeGrip 内部的鼠标事件不会冒泡到浮窗上，但**鼠标会被 grab**：
    // 拉右下角时鼠标很容易滑出窗口边界，此时 Qt 仍把事件发给 grip（它是抓取者），
    // 而浮窗这边**收不到** leave —— 可一旦用户把鼠标挪回窗口内又挪出去，
    // 边界判定就可能失衡。更现实的问题是：拉大窗口的过程中窗口边界一直在变，
    // 鼠标"在不在窗口上"这件事本身就不稳定。
    //
    // 所以直接按"grip 是否按着"来屏蔽，比去猜边界可靠得多。
    //
    // ⚠️ 这里曾经写过 connect(m_sizeGrip, &QSizeGrip::pressed, ...) ——
    // QSizeGrip **根本没有** pressed/released 信号（它把鼠标事件全自己吞了），
    // 编译期就会报 "is not a member of 'QSizeGrip'"。
    // 它是 QWidget，所以正确做法是装事件过滤器，从 MouseButtonPress /
    // MouseButtonRelease 上取状态。用过滤器而不是继承一个子类：
    // 这个状态只在本文件内用一次，为它多开一个类不划算。
    m_sizeGrip->installEventFilter(this);
}

bool FloatingBoxWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_sizeGrip) {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
            // 按下手柄 = 正在拉尺寸。这期间窗口边界一直在动，
            // 悬停判定必然不准，直接整体屏蔽。
            m_sizeGripActive = true;
            break;
        case QEvent::MouseButtonRelease:
            m_sizeGripActive = false;
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FloatingBoxWidget::connectServiceSignals()
{
    // 盒内容变了：
    //   boxPath 为空串 = "不知道具体哪个盒"（撤销/还原场景），全部刷新；
    //   否则只有 path 匹配的那个浮窗需要刷新。
    // 带 boxPath 的意义：三个浮窗开着时，A 盒收纳完不该让 B、C 重新扫盘。
    connect(m_service, &AppService::boxContentsChanged,
            this, [this](const QString &boxPath) {
                if (boxPath.isEmpty() || boxPath == m_boxPath) {
                    refreshItems();
                }
            });

    // 撤销栈一变，按钮可用性与文案就要跟着变。
    connect(m_service, &AppService::undoStateChanged,
            this, &FloatingBoxWidget::updateUndoButton);
}

void FloatingBoxWidget::refreshItems()
{
    if (m_boxPath.isEmpty()) {
        m_itemList->setItems({});
        m_titleBar->setCountText(tr("0 项"));
        return;
    }

    const QList<DesktopEntry> items = BoxManager::listBoxItems(m_boxPath);
    m_itemList->setItems(items);
    m_titleBar->setCountText(tr("%1 项").arg(items.size()));
}

// ---------------------------------------------------------------------------
// 收纳：收进**本浮窗代表的盒**，不是主窗口当前选中的盒
// ---------------------------------------------------------------------------
void FloatingBoxWidget::onCollectDesktop()
{
    const QList<DesktopEntry> found = DeskScanner::scan(
        CoreNames::desktopRoot(),
        CoreNames::publicDesktopRoot(),
        m_service->settings()->excludedNames(),
        CoreNames::boxRoot());

    if (found.isEmpty()) {
        // ⚠️ 所有 exec() 型弹窗都要用这个守卫：模态框一弹出来，鼠标在系统看来
        // 就不在浮窗上了，Qt 会发 leaveEvent —— 不挡的话主人每点一次
        // 「收纳桌面」，浮窗自己先卷起来，确认框还盖在一条细线上。
        // 守卫是 RAII 的，弹窗中途 return 也不会漏复位。
        ModalGuard guard(&m_modalDialogOpen);
        QMessageBox::information(this, tr("桌面已经很干净了"),
                                 tr("没有找到需要收纳的条目。"));
        return;
    }

    // ⚠️ 需实机验证：PreviewDialog 是模态的（exec()），以 Qt::Tool 浮窗为 parent
    // 时，在 Windows 上可能出现"对话框跑到浮窗后面"或被浮窗遮挡的层级问题。
    // 本处按设计方案的推荐用 parent = this。
    // 若实测发现层级不对，退路是改为 parent = nullptr 并手动 activateWindow()：
    //     PreviewDialog dlg(found, m_boxName, nullptr);
    //     dlg.activateWindow(); dlg.raise();
    //     if (dlg.exec() != QDialog::Accepted) return;
    PreviewDialog dlg(found, m_boxName, this);
    {
        // 模态期间挡掉悬停判定 —— 理由同 onCollectDesktop 里那个空列表分支。
        ModalGuard guard(&m_modalDialogOpen);
        if (dlg.exec() != QDialog::Accepted) {
            return;
        }
    }

    collectEntries(dlg.selectedEntries());
}

void FloatingBoxWidget::collectEntries(const QList<DesktopEntry> &entries)
{
    if (entries.isEmpty()) {
        return;
    }

    // 只调 service：搬文件、推撤销栈、发信号都在它内部完成。
    // 界面刷新由 connectServiceSignals 里那两条连接驱动，
    // 此处**不再**手工调 refreshItems —— 否则同一次操作会刷新两遍。
    m_service->collectInto(entries, m_boxPath, m_boxName);
}

// ---------------------------------------------------------------------------
// 撤销
// ---------------------------------------------------------------------------
bool FloatingBoxWidget::undoBelongsToThisBox() const
{
    return m_service->canUndo() && m_service->undoBoxName() == m_boxName;
}

void FloatingBoxWidget::updateUndoButton()
{
    if (!m_undoBtn) {
        return;
    }

    const bool can = m_service->canUndo();
    m_undoBtn->setEnabled(can);

    if (!can) {
        m_undoBtn->setText(tr("暂无可撤销"));
        m_undoBtn->setToolTip(tr("还没有可撤销的收纳。"));
        return;
    }

    const int     count   = m_service->undoCount();
    const QString boxName = m_service->undoBoxName();

    // 文案必须点名是哪个盒 —— 撤销栈全局唯一，只保留最近一次收纳。
    // 三个浮窗同时开着时，B 盒浮窗上的撤销按下去撤的可能是 A 盒那批；
    // 若只写"撤销上次收纳"，主人会以为撤的是自己这个盒，是误导。
    if (undoBelongsToThisBox()) {
        m_undoBtn->setText(tr("撤销本盒（%1 项）").arg(count));
    } else {
        m_undoBtn->setText(tr("撤销「%1」（%2 项）").arg(boxName).arg(count));
    }

    m_undoBtn->setToolTip(
        tr("把上一次收纳进「%1」的 %2 项还原回桌面（发生于 %3）\n"
           "注意：撤销的是**全程序最近一次**收纳。")
            .arg(boxName)
            .arg(count)
            .arg(m_service->undoTime().toString(QStringLiteral("HH:mm:ss"))));
}

void FloatingBoxWidget::onUndoClicked()
{
    if (!m_service->canUndo()) {
        return;
    }

    const int     count   = m_service->undoCount();
    const QString boxName = m_service->undoBoxName();

    // 确认框同样要点名，并且明确告知"这是全程序最近一次"。
    const QString question =
        undoBelongsToThisBox()
            ? tr("确定把收纳进本盒的 %1 项还原回桌面吗？").arg(count)
            : tr("确定把收纳进「%1」的 %2 项还原回桌面吗？\n\n"
                 "注意：这是全程序最近一次收纳，不是本盒的。")
                  .arg(boxName)
                  .arg(count);

    QMessageBox::StandardButton answer = QMessageBox::No;
    {
        ModalGuard guard(&m_modalDialogOpen);
        answer = QMessageBox::question(this, tr("撤销收纳"), question,
                                       QMessageBox::Yes | QMessageBox::No,
                                       QMessageBox::No);
    }
    if (answer != QMessageBox::Yes) {
        return;
    }

    // 执行、清栈、发信号都在 service 内完成；刷新由信号驱动。
    m_service->undoLast();
}

// ---------------------------------------------------------------------------
// 双击打开
// ---------------------------------------------------------------------------
void FloatingBoxWidget::onOpenRequested(const QString &path)
{
    if (path.isEmpty()) {
        return;
    }

    QString err;
    if (Opener::openPath(path, &err)) {
        return;
    }

    // 只在真的失败、要弹警告框时才挡悬停 —— openPath 本身是普通调用，
    // 不弹窗时没什么需要屏蔽的。
    ModalGuard guard(&m_modalDialogOpen);
    QMessageBox::warning(this, tr("打开失败"),
                         err.isEmpty() ? tr("无法用系统默认方式打开该项目。") : err);
}

// ---------------------------------------------------------------------------
// 拖出结束 —— 防"文件凭空消失"的兜底
// ---------------------------------------------------------------------------
void FloatingBoxWidget::onDragOutFinished(const QString &path, bool sourceStillExists)
{
    // 拖放结束（无论成没成），解除屏蔽。
    //
    // ⚠️ 必须在**最开头**就解除，而不是在某个分支里 —— 本函数下面有多个
    // 提前 return（刷新后返回、弹框后返回），写在后面必然有路径漏掉，
    // 而漏掉的后果是这个浮窗从此永不自动展开/收起，还查不出原因。
    m_dragOutActive = false;

    // 拖完之后鼠标多半已经不在窗口上了（人把文件拖到别处去了）。
    // 若这次是悬停展开的，该收就收 —— 但要重新起计时，
    // 因为拖拽期间那两个定时器已经被停掉了。
    if (m_hoverExpanded && !hoverInteractionBlocked() && !m_rolledUp) {
        if (m_hoverCollapseTimer) {
            m_hoverCollapseTimer->start();
        }
    }

    // 为什么必须判断这个：
    // 拖出靠的是"告诉 Windows 这是一次移动操作"，真正搬文件的是资源管理器。
    // 若主人把文件拖到一个**不接受文件拖放**的目标上（某些程序、浏览器空白区），
    // 对方不会执行移动，而我们的标记又让系统以为"会有人处理" ——
    // 结果是文件既不在源、也不在目标、也不在回收站，**凭空消失**。
    // 实测中确实出现过这一现象。
    //
    // 所以判据必须以文件系统的实际状态为准，而不是 QDrag::exec() 的返回值：
    // 返回值可能仍报 MoveAction，只看它会误判成"已还原"。
    if (!sourceStillExists) {
        // 资源管理器成功把它移走了 —— 静默刷新列表即可，不需要打扰主人。
        refreshItems();
        return;
    }

    // 源仍在盒里：对方没接手，或者只是复制了一份。
    // 必须明确告知，绝不能让主人以为文件已经还原了。
    ModalGuard guard(&m_modalDialogOpen);
    QMessageBox::information(
        this, tr("未完成移动"),
        tr("文件仍保留在收纳盒中，没有移动任何东西。\n\n"
           "原因：目标位置不接受文件拖放（例如拖到了某个程序窗口或浏览器的空白处）。\n\n"
           "如需把「%1」移出去，请改用右键菜单里的「还原到桌面」，"
           "或者把它拖到资源管理器的文件夹里。")
            .arg(QFileInfo(path).fileName()));
}

// ---------------------------------------------------------------------------
// 拖入：归入本盒
// ---------------------------------------------------------------------------
void FloatingBoxWidget::onFilesDropped(const QStringList &paths)
{
    // 组装成 DesktopEntry。来源按文件真实所在的桌面目录判定 ——
    // 只有当它确实来自某个桌面时才标桌面来源，否则标 UserDesktop 即可
    // （这些条目大概率是别的文件夹拖进来的，origin 在还原时的语义另有处理）。
    QList<DesktopEntry> entries;
    entries.reserve(paths.size());
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.exists()) {
            continue;   // 拖拽过程中源已被删/移走
        }

        DesktopEntry e;
        e.filePath = info.absoluteFilePath();
        e.name     = info.fileName();
        e.isDir    = info.isDir();
        e.size     = e.isDir ? 0 : info.size();
        e.modified = info.lastModified();
        e.origin   = CoreNames::isPathInside(e.filePath, CoreNames::publicDesktopRoot())
                         ? EntryOrigin::PublicDesktop
                         : EntryOrigin::UserDesktop;
        entries << e;
    }

    if (entries.isEmpty()) {
        return;
    }

    // 不做二次确认：主人已经在主动拖了，再弹一个框只会碍事。
    collectEntries(entries);
}

// ---------------------------------------------------------------------------
// 卷起 / 展开
// ---------------------------------------------------------------------------
void FloatingBoxWidget::onToggleRollUp()
{
    m_rolledUp = !m_rolledUp;

    // ---- 手动操作与自动展开的关系 ----
    //
    // ⚠️ 这里原先有一套"手动卷起优先"的逻辑（m_manualRolledUp），
    // **已经被主人实测推翻并删除**，别再写回来。
    //
    // 旧逻辑：手动卷起 = "我现在不想看见内容"，于是把悬停自动展开一并屏蔽，
    // 直到主人再手动展开一次才解除。听起来合理，但它有一个**解不开的循环**：
    //   手动卷起 -> 窗口是收起的 -> 悬停不展开 -> 主人只剩双击这一条路
    //   而他还得先想到要去双击
    // 表现就是"手动卷起一次之后，自动展开再也回不来了"。
    //
    // 新逻辑（当前实现）：**不需要那层标志**。
    //   手动卷起 -> 悬停仍能自动展开
    //   手动展开 -> 鼠标离开不自动卷起
    // 这两条合起来其实只有一句话：**"离开自动卷起"只对悬停展开的窗口生效**。
    // 而那正是 m_hoverExpanded 已经表达的语义 —— 手动展开时把它清成 false，
    // 手动卷起时它本来就是 false。多一层 m_manualRolledUp 不但没有增加表达力，
    // 还制造了上面那个死循环。

    // 手动动过之后，悬停那套状态要一起归零：
    //   * 手动展开时清 m_hoverExpanded —— 否则鼠标一离开，这个
    //     主人手动展开的窗口会被自动收起（正是 m_hoverExpanded 要防的事）；
    //   * 手动收起时也要清，避免残留一个"悬停展开过"的标记。
    m_hoverExpanded = false;

    // 两个悬停定时器都停掉：主人刚刚明确表达了意图，
    // 不该让一个待触发的自动动作紧接着把它推翻。
    stopHoverTimers();

    applyRollUpState(m_rolledUp);
    persistRolledUp(m_rolledUp);
}

void FloatingBoxWidget::applyRollUpState(bool rolledUp)
{
    if (rolledUp) {
        // 记住当前高度 —— 展开时要恢复成"卷起前那个高度"，
        // 而不是某个写死的默认值，否则主人调过的大小会被抹掉。
        //
        // 仅在"当前不是卷起状态"时才记。否则重复调用（构造时恢复一次、
        // applySavedGeometry 里又补一次）会把第一次收起后的那条细线高度
        // 当成"展开高度"记下来，展开时窗口就再也回不到原来的大小了。
        //
        // 再加一条"不在高度动画中"：动画的每一帧都会触发 resizeEvent，
        // 而展开动画期间 m_rolledUp 已经是 false —— 守卫拦不住它，
        // 会让 m_expandedHeight 被写成动画的中间值。详见 resizeEvent 的注释。
        if (!m_rolledUp && !m_rollAnimating && height() > rolledUpHeight()) {
            m_expandedHeight = height();
        }

        // 子控件**立即**隐藏，不等动画。
        //
        // 若等动画结束再隐藏，卷起过程中列表会一直露在外面被"压扁"，
        // 观感上像是窗口在缩而不是在收。
        m_actionBar->setVisible(false);
        m_itemList->setVisible(false);
        if (m_gripRow) {
            m_gripRow->setVisible(false);
        }

        // ⚠️ min/max 锁的时机：
        //   - 下限**立即**设：卷起过程中高度不该低于标题栏高。
        //   - 上限**不在这里设**：设了 min == max 就把高度焊死了，
        //     动画每帧都会被顶回 30（实测见 tools/heightlock_diag.cpp）。
        //     它的落点交给动画的 finished（见 animateHeightTo），
        //     或者下面的"不可动画"分支。
        setMinimumHeight(rolledUpHeight());

        animateHeightTo(rolledUpHeight());

        // 动画没能跑起来时（窗口未显示 / 动画被关 / 终值已相等），
        // 上限必须当场补上 —— 否则卷起态可以被主人往下拉，
        // 拉出来的是一块没有内容的空白。
        if (!m_heightAnim || m_heightAnim->state() != QAbstractAnimation::Running) {
            setMaximumHeight(rolledUpHeight());
        }

        if (m_titleBar) {
            m_titleBar->setToolTip(tr("已卷起。双击或点「—」展开。"));
        }
    } else {
        // ⚠️⚠️ 展开分支有两个顺序陷阱，都在实测中踩到过，必须一起处理。
        //
        // 【陷阱一】setMinimumHeight 会让窗口立即被拉高，触发一次 resizeEvent。
        //   卷起态高度是 30，而 kMinimumHeight 是 120 —— 一旦把下限设回 120，
        //   Qt 会**立刻**把窗口拉到 120，触发 resizeEvent。
        //   那次 resize 发生在动画开始之前（m_rollAnimating 还是 false），
        //   于是"记住展开高度"的逻辑会把 120 写进 m_expandedHeight 自己把自己
        //   的记忆覆盖掉；而动画的目标高度又是从这个变量算的 ——
        //   结果展开只能到 120，且**每一轮都这样**，像是"展开功能坏了"。
        //
        //   两道防线：
        //     a) 先把 rememberedHeight 取出来，后面 min/max 引起多少次 resize
        //        都不影响这一轮的目标值；
        //     b) 下面那个 m_layoutAdjusting 标志让这次 resize 不去改记录。
        //
        // 【陷阱二】动画启动前的那一串 min/max 与 setVisible 都会触发 resize，
        //   它们的中间高度（120、230…）都不是"主人调过的尺寸"。
        //   用同一个标志一并挡掉。
        const int rememberedHeight = m_expandedHeight;

        m_layoutAdjusting = true;       // 下面这段尺寸变动不许写 m_expandedHeight

        setMinimumHeight(kMinimumHeight);
        setMaximumHeight(QWIDGETSIZE_MAX);

        // 展开：子控件立即显示。
        // 与卷起同理，延迟显示会让展开过程看起来像"一块空白被拉开"。
        m_actionBar->setVisible(true);
        m_itemList->setVisible(true);
        if (m_gripRow) {
            m_gripRow->setVisible(true);
        }

        m_layoutAdjusting = false;

        // 恢复卷起前的高度；若那个值已经不合法（比如从没展开过），
        // 回落到默认高度，避免把窗口恢复成一条细线。
        const int target = rememberedHeight > rolledUpHeight() ? rememberedHeight
                                                                : kDefaultHeight;

        // ⚠️ 顺序：先算外观校正后的目标高度，再启动动画。
        //
        // 原先的写法是 resize() 然后 resizeForAppearance() —— 后者会在这基础上
        // 判断"要不要继续放大"。改成动画之后，那两步都必须发生在动画**之前**，
        // 否则会出现"动画刚跑到一半，resizeForAppearance 又把高度改掉"的打架。
        //
        // 做法：让 resizeForAppearance 先按目标高度算出它需要的尺寸，
        // 谁大用谁，最后一次性动画到那个值。
        const int appearanceHeight = heightNeededForAppearance();
        const int finalTarget = qMax(target, appearanceHeight);

        animateHeightTo(finalTarget);

        if (m_titleBar) {
            m_titleBar->setToolTip(QString());
        }
    }

    // 尺寸变了要落盘（卷起状态本身另存一份，见 persistRolledUp）。
    scheduleGeometrySave();

    // 通知 manager 重算让位。
    //
    // ⚠️ 放在这里而不是各个调用点：卷起/展开有四条触发路径
    //（双击、按钮、右键菜单、悬停自动），在每条外面各调一次必然漏，
    // 而漏掉的表现是"某种方式展开时不会推开下面的浮窗"。
    //
    // 放在函数**末尾**：上面那些 resize/动画都已经就位，
    // manager 读 frameGeometry() 时才是这一轮最终的目标尺寸。
    // 放在开头的话它拿到的是变化前的尺寸，算出来的位移是错的。
    emit rollUpStateChanged(m_boxName, rolledUp);
}

void FloatingBoxWidget::persistRolledUp(bool rolledUp)
{
    m_service->settings()->setFloatRolledUp(m_boxName, rolledUp);
}

// ---------------------------------------------------------------------------
// 悬停自动展开 / 离开自动卷起
//
// 【这一整块最需要说清楚的设计】
//
// 1) 为什么要区分"谁把它展开的"（m_hoverExpanded）
//
//    离开时只有**悬停展开的**窗口才自动收回。若不区分，主人手动双击展开、
//    想仔细看看盒里有什么，鼠标一移开窗口就自己卷了 —— 那个操作等于白做。
//    反过来若一律不收回，鼠标扫过的浮窗会全部永久展开，自动展开反而成了负担。
//
// 2) 为什么不需要"手动卷起优先"（曾经的 m_manualRolledUp，已删除）
//
//    曾经有过一层"手动卷起就把自动展开屏蔽掉"的逻辑。它的动机是
//    "主人手动收起 = 他现在不想看见内容"。听起来合理，但**解不开**：
//    屏蔽之后，主人唯一的解除方式是再手动展开一次，而他得先想到要去双击；
//    在此之前悬停是死的。主人实测报的就是这个 ——"手动卷起一次之后，
//    自动展开再也回不来了"。
//
//    现在的规矩只有一条：**"离开自动卷起"只对悬停展开的窗口生效**。
//    手动卷起 -> 悬停照样展开（那次展开就是悬停的，离开时会自动收回去）；
//    手动展开 -> 离开不卷。两句话各自成立，而且不需要额外状态。
//
// 3) 为什么每个触发点都要走 hoverInteractionBlocked()
//
//    鼠标"不在窗口上"和"人离开了"是**两回事**。拖窗口、拉尺寸、开菜单、
//    弹模态框、拖文件出去，这些情况下鼠标都会跑到窗口外面，但人根本没走。
//    不挡的话，每右键一次窗口就自己卷起来、每拖一次文件浮窗就缩成一条线。
//    这些是自动展开功能最容易造成的破坏，所以判断收在一处，
//    enter/leave 与两个定时器回调都过它。
//
//    另有一条**性质不同**的：被钉住（m_locked）。它不是"事件收不到"的
//    权宜之计，而是主人明确的意图 ——"这个窗口别动我"。所以它排在最前面，
//    而且同时关掉自动展开与自动卷起两个方向。
//    注意它**不**影响手动操作：双击、右键菜单、标题栏按钮照常可用 ——
//    锁的是"自动"，不是"主人自己"。
// ---------------------------------------------------------------------------

bool FloatingBoxWidget::hoverInteractionBlocked() const
{
    // 被钉住：悬停自动展开与自动卷起**整个停用**。
    //
    // ⚠️ 放在最前面，因为它是唯一一条"主人明确要求别动我"的理由 ——
    // 其余几条都是"鼠标其实还在，只是事件收不到"的权宜之计，
    // 只有这条是意图本身。
    //
    // 为什么钉住要连悬停一起关掉（而不是只关"被推动"）：
    // 主人给一个浮窗上锁，表达的是"这个窗口就摆在现在这个样子，别碰它"。
    // 若锁定后鼠标一移上去它还是自己弹开、移开又自己卷起，
    // 那"锁"就没锁住任何他能看见的东西 —— 他锁的多半正是**展开态**，
    // 结果一悬停它先变矮再展开，观感上就是"锁了个寂寞"。
    //
    // 与"被推动"的区别：推动是别人引发的、锁住的是**别人对我的影响**；
    // 悬停展开是我自己引发的、锁住的是**我自己会动作**。
    // 两件都要锁，才叫"别动我"。
    if (m_locked) {
        return true;
    }

    // 拖动窗口：人想把它搬走，"鼠标在窗口上"是假象。
    //
    // ⚠️ 额外查一次"左键是不是真的还按着"。
    //
    // m_dragging 由 Press 置真、Release 置假。若中途丢了 Release
    //（拖到窗口外松手、系统弹窗抢走事件、远程桌面断线……），这个标志会
    // **永久**停在真上 —— 而它的后果不是"拖动行为有点怪"，是这个浮窗
    // 从此再也不会自动展开/收起，且完全看不出原因。
    // 主人报的"手动卷起后再也展开不了"一度就长得像这个症状，
    // 所以这里宁可多问一句物理按键状态。
    //
    // 用 QGuiApplication::mouseButtons() 而不是查某个事件：
    // 它反映的是全局的按键状态，不依赖我们有没有收到某个特定事件。
    if (m_titleBar && m_titleBar->isDragging()
        && (QGuiApplication::mouseButtons() & Qt::LeftButton)) {
        return true;
    }

    // 拉右下角尺寸手柄。
    if (m_sizeGripActive) {
        return true;
    }

    // 右键菜单正弹着。
    //
    // ⚠️ QMenu::exec() 是**模态事件循环**：菜单一弹出，鼠标在系统看来就已经
    // 不在浮窗上了，Qt 会立刻给浮窗发一个 leaveEvent。
    // 不挡的话，每次右键打开菜单，这个浮窗就先自己卷起来 —— 菜单还开着，
    // 底下的窗口却缩成一条线，主人点的"卷起/展开"项会作用在一个已经卷起的
    // 窗口上，行为完全对不上。
    if (m_contextMenuOpen) {
        return true;
    }

    // 模态对话框（收纳预览、确认框、错误提示）弹着。同理。
    if (m_modalDialogOpen) {
        return true;
    }

    // 文件正在被拖出去。
    //
    // ⚠️ QDrag::exec() 是**系统级鼠标抓取**：它接管消息循环直到拖放结束，
    // 期间浮窗收不到正常的 enter/leave 序列。若拖到一半浮窗自己卷起来，
    // 拖拽的源控件（列表）会被隐藏，drop 事件的目标判定与拖拽反馈全乱。
    if (m_dragOutActive) {
        return true;
    }

    // 有东西正被拖到我头上。
    //
    // ⚠️ 与上面几条不同源：上面是"鼠标其实还在，只是事件收不到"，
    // 这条是"鼠标带着文件悬在这儿"。它挡的不是误判，而是
    // "投放途中窗口自己缩起来" —— 一缩，列表就被隐藏，drop 没了接收者，
    // 主人正要放下的那个文件就掉进虚空（既没进盒、也没回桌面）。
    if (m_dragHoverActive) {
        return true;
    }

    // 高度动画正跑着。
    //
    // 两段动画互相打断的后果不是"看起来卡一下"，而是 m_expandedHeight 被写脏：
    // 展开动画跑到一半时卷起，resizeEvent 会把中间高度记成"展开高度"，
    // 之后这个浮窗就永久只能展开到半截高。步骤 2 已经踩过一次这个坑，
    // 详见 m_rollAnimating 的说明。
    if (m_rollAnimating) {
        return true;
    }

    return false;
}

bool FloatingBoxWidget::canAutoExpand() const
{
    // 总开关关了 = 这个能力整个不存在。
    if (!m_hoverExpandEnabled) {
        return false;
    }

    // 已经展开着：没什么可做的。
    // 这一条同时挡掉了"鼠标在窗口上停留时反复触发 enter"的重复动作。
    //
    // ⚠️ 这里原先还有一条"主人手动卷起过就别来烦他"（m_manualRolledUp），
    // 已随"手动卷起优先"一起删除 —— 那个标志制造了一个解不开的循环，
    // 详见 onToggleRollUp 里的说明。现在手动卷起的窗口照样会被悬停展开，
    // 这正是主人要的。
    if (!m_rolledUp) {
        return false;
    }

    // 五种"鼠标可能不在窗口上但人没走"的情形。
    if (hoverInteractionBlocked()) {
        return false;
    }

    // 窗口不可见时不要动高度：动画在不可见窗口上不会推进，
    // 而且构造期（窗口还没 show）恢复卷起状态时正好会经过这里。
    if (!isVisible()) {
        return false;
    }

    return true;
}

bool FloatingBoxWidget::shouldAutoCollapse() const
{
    // 不是悬停展开的 —— 那是主人自己展开的，不替他收。
    if (!m_hoverExpanded) {
        return false;
    }

    if (hoverInteractionBlocked()) {
        return false;
    }

    // 已经卷起了，没什么可做的。
    if (m_rolledUp) {
        return false;
    }

    if (!isVisible()) {
        return false;
    }

    // 鼠标又回到窗口上了（离开定时器还没到点，人就回来了）——
    // 这属于"其实是误判的 leave"，不该收。
    //
    // ⚠️ 判断用 rect().contains(mapFromGlobal(QCursor::pos())) 而不是
    // underMouse()：underMouse() 依赖 Qt 内部的 enter/leave 记账，
    // 而这里恰恰是在怀疑那套记账（菜单/拖拽期间它本来就不可靠）。
    // 直接比几何位置是唯一不依赖那套状态的做法。
    if (rect().contains(mapFromGlobal(QCursor::pos()))) {
        return false;
    }

    return true;
}

void FloatingBoxWidget::stopHoverTimers()
{
    if (m_hoverExpandTimer) {
        m_hoverExpandTimer->stop();
    }
    if (m_hoverCollapseTimer) {
        m_hoverCollapseTimer->stop();
    }
}

void FloatingBoxWidget::setHoverExpandEnabled(bool on)
{
    if (m_hoverExpandEnabled == on) {
        return;
    }
    m_hoverExpandEnabled = on;

    // 刚被关掉：把悬停相关的定时器全停掉。
    //
    // 不停的话，关开关的那一刻若正好有一个展开定时器在跑，它到点后仍会把
    // 窗口展开 —— 主人会看到"我明明关了，它还是弹了一下"。
    // 注意这里**不**去收回已经悬停展开的窗口：那是另一个语义
    //（关的是"以后还自动不自动"，不是"现在把已经开的收起来"），
    // 而且立刻抖动一下窗口更像 bug。下一次离开时 shouldAutoCollapse 会正常收。
    if (!on) {
        stopHoverTimers();
        return;
    }

    // ---- 刚被打开：补一次判断 ----
    //
    // ⚠️ 这个补判是必须的，主人报过"重新激活后只有刚激活那一下生效"。
    //
    // 根因：展开计时器是在 enterEvent 里启动的，而**打开开关这个动作本身
    // 不产生 enterEvent**。主人的真实操作顺序是：
    //     鼠标挪到浮窗上（卷起态，此时开关是关的，enterEvent 进来了但
    //     canAutoExpand() 因为开关关着直接返回，计时器没起）
    //     -> 发现没反应 -> 打开开关 -> 鼠标**一直没动过**
    // 此时鼠标已经在窗口上，窗口系统不会再发一次 enter（它认为鼠标本来
    // 就在那儿），于是没有任何时机去启动那个计时器 —— 表现就是"打开了也
    // 还是不理我"，只有把鼠标移开再移回来才恢复。
    //
    // 所以要在这里主动问一句"鼠标现在是不是就在我身上"，是的话当场开始计时。
    //
    // 判据用几何位置而不是 underMouse()：与 shouldAutoCollapse 里同源 ——
    // underMouse() 依赖 Qt 内部的 enter/leave 记账，而此刻恰恰可能因为
    // 菜单/拖拽等原因而不准。直接比矩形包含关系最可靠。
    if (!canAutoExpand()) {
        return;
    }
    if (!rect().contains(mapFromGlobal(QCursor::pos()))) {
        return;
    }
    if (m_hoverExpandTimer) {
        m_hoverExpandTimer->start();
    }
}

void FloatingBoxWidget::enterEvent(QEnterEvent *event)
{
    QWidget::enterEvent(event);

    // ⚠️⚠️ 进窗口的第一件事：**停掉离开定时器**。
    //
    // 这是整个悬停交互里最容易出的一个 bug，而且是经典形态：
    // 鼠标在窗口边缘进进出出（或从列表移向标题栏时擦过边界）会先发 leave、
    // 再发 enter。leave 启动了 400ms 的收起计时，enter 若不把它停掉，
    // 400ms 后窗口就被那次**已经过期的** leave 卷了起来 ——
    // 而此刻鼠标好端端地在窗口上。
    // 表现是"鼠标放在上面，窗口过一会儿自己卷了"，且时有时无。
    if (m_hoverCollapseTimer) {
        m_hoverCollapseTimer->stop();
    }

    // 鼠标回来了但它本来就是展开的（或者不该自动展开）：什么都不做。
    if (!canAutoExpand()) {
        return;
    }

    // 开始计时：停满 kHoverExpandDelayMs 才真正展开。
    // 用 start() 而不是"先判断没在跑再 start"：重开会重置计时，
    // 正是"每次进入都重新计时"要的语义。
    if (m_hoverExpandTimer) {
        m_hoverExpandTimer->start();
    }
}

void FloatingBoxWidget::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);

    // 刚离开时那个展开计时就不该继续了 —— 鼠标已经走了，
    // 到点后把窗口展开是很莫名其妙的（人都不在那儿了）。
    if (m_hoverExpandTimer) {
        m_hoverExpandTimer->stop();
    }

    // 不该自动收的情形（不是悬停展开的 / 交互中 / 已经卷起）：直接返回。
    // 这里不能写 shouldAutoCollapse()，因为它还要检查"鼠标是不是又回来了"，
    // 而此刻我们**刚收到 leave**，鼠标理论上确实在外面 —— 但菜单、拖拽
    // 这些情形下这个前提不成立，所以先过一遍交互屏蔽，再起计时。
    if (!m_hoverExpanded || hoverInteractionBlocked() || m_rolledUp) {
        return;
    }

    // 起收起计时。真正收不收在 onHoverCollapseTimeout 里再判一次 ——
    // 那时鼠标的位置才是可信的。
    if (m_hoverCollapseTimer) {
        m_hoverCollapseTimer->start();
    }
}

void FloatingBoxWidget::onHoverExpandTimeout()
{
    // 到点了再确认一次：这 250ms 里主人可能已经拖走了窗口、关掉了开关，
    // 或者鼠标早就离开了（那种情况下 leaveEvent 已经停掉了本定时器，
    // 走不到这里）。多这一问不多余 —— 条件变了的路径确实存在。
    if (!canAutoExpand()) {
        return;
    }

    m_hoverExpanded = true;

    // ⚠️ 这里**不**走 onToggleRollUp。
    //
    // onToggleRollUp 会 persistRolledUp()，而规格要求"自动卷起的状态要落盘"
    // 落的是**卷起**那一侧。自动展开是临时行为：主人退出程序时浮窗是展开的，
    // 下次启动就该是展开的，不该因为"上次退出前鼠标恰好扫过它"而记住什么。
    // 展开态的持久化由收起那一侧负责。
    m_rolledUp = false;
    applyRollUpState(false);
}

void FloatingBoxWidget::onHoverCollapseTimeout()
{
    if (!shouldAutoCollapse()) {
        return;
    }

    m_hoverExpanded = false;
    m_rolledUp = true;
    applyRollUpState(true);

    // 与展开不同，**收起要落盘**（规格明确要求）。
    //
    // 为什么两个方向不对称：收起是"这个窗口安静待着"的状态，重新启动后
    // 保持安静是符合预期的（桌面整洁）；而展开是"我现在要看"，属于当下动作。
    // 顺带这也让"自动展开"不会在配置里留下任何痕迹 —— 主人翻 ini 只会看到
    // 一个 rolledUp=true，与他自己双击收起的结果一模一样，符合直觉。
    persistRolledUp(true);
}

// ---------------------------------------------------------------------------
// 外观
// ---------------------------------------------------------------------------
void FloatingBoxWidget::applyAppearance(const BoxAppearance &appearance)
{
    m_appearance = appearance;

    // ---- 透明度 ----
    // 用 setWindowOpacity 而不是样式表。三条理由，第二条是决定性的：
    //
    // 1) 作用域正确：整个浮窗（标题栏、边框、列表）一起变淡才自然。
    //
    // 2) ⚠️ 不能碰列表的样式表：ItemListWidget::setDragHighlight() 在拖拽结束时
    //    会执行 setStyleSheet(QString()) —— **整条样式表被清空**。
    //    若透明度靠列表样式表实现，每次把文件拖进/拖出浮窗都会把透明度冲掉，
    //    表现为"拖一下就突然变回不透明"，而且极难联想到根因。
    //
    // 3) 不污染几何：不改窗口尺寸、不触发 resizeEvent，不与卷起/尺寸逻辑打架。
    //
    // 下限用 kMinOpacity（20%）而不是 Settings 层兜的 1%：
    // 两层都兜是因为"浮窗彻底看不见"是**不可恢复**的故障 ——
    // 主人会找不到自己的浮窗，连改回来的入口都没有。
    //
    // ⚠️ 走 animateOpacityTo 而不是直接 setWindowOpacity：
    // 从 100% 调到 70% 时直接 set 会"啪"地跳一下，动画让它滑过去。
    // 更重要的是，这样它与淡入共用同一个动画对象 ——
    // 否则"淡入进行中有人改外观"会让那一句把动画打到一半的值硬设回去，
    // 表现为透明度闪一下才到位。两处统一走同一个动画，冲突自然消失。
    animateOpacityTo(targetOpacityFromAppearance());

    // ---- 视图 ----
    if (m_itemList) {
        m_itemList->applyAppearance(appearance);
    }

    // ---- 尺寸 ----
    resizeForAppearance();
}

// ---------------------------------------------------------------------------
// 透明度动画
// ---------------------------------------------------------------------------
double FloatingBoxWidget::targetOpacityFromAppearance() const
{
    // 夹紧规则与 applyAppearance 完全一致，抽成一份共用：
    // 若两处各写一遍，日后改了夹紧范围就会漏改一处，
    // 而症状是"淡入结束时闪一下"（终值与静态值对不上），极难定位。
    return qBound(BoxAppearance::kMinOpacity / 100.0,
                  m_appearance.opacity / 100.0,
                  1.0);
}

void FloatingBoxWidget::setOpacityImmediately(double target)
{
    // 先停掉可能正在跑的动画：否则动画的下一帧立刻会把这次设值覆盖掉，
    // 表现为"设了但没生效"。
    if (m_opacityAnim && m_opacityAnim->state() == QAbstractAnimation::Running) {
        m_opacityAnim->stop();
    }
    setWindowOpacity(target);
}

void FloatingBoxWidget::animateOpacityTo(double target, int durationMs)
{
    // ---- 窗口还没显示：直接设值，不要起动画 ----
    //
    // ⚠️ 这条不是优化，是防一次可见的闪烁。
    // openBox 的顺序是：构造 → applyAppearance（会走到这里）→ prepareFadeIn
    // → show → fadeIn。
    // 若在 show 之前就把动画跑起来，动画会在一个尚未成为原生窗口的对象上
    // 瞬间跑完并把 windowOpacity 写成了终值；接着 show() 让窗口以**终值**
    // 显示出来 —— 下一句 fadeIn 又把它从头动一遍，于是主人看到的是
    // "先亮一下、再从头淡入"。既难看又显得程序有毛病。
    //
    // 未显示的窗口本来就设不了有效的不透明度，所以这里直接设值即可。
    // 淡入的起始值由 prepareFadeIn 在 show 之前单独置 0。
    if (!isVisible()) {
        setOpacityImmediately(target);
        return;
    }

    // 动画被主人关掉：退化成立即生效，行为与加动画之前逐字一致。
    if (!m_animationsOn || !m_opacityAnim) {
        setOpacityImmediately(target);
        return;
    }

    // 终值没变且没在跑：什么都不用做。
    // 省掉一次无意义的动画 —— 这在新开浮窗时很常见
    //（openBox 会连着调 applyAppearance，而那多半就是"设成当前值"）。
    //
    // ⚠️ 用 qAbs(...) < 阈值 而不是 qFuzzyCompare：
    // qFuzzyCompare 的相对误差算法在其中一个操作数为 0 时不可靠
    //（按它的定义，比较 0 与 0 恰好成立，但比较 0 与一个极小数也会成立），
    // 而本函数确实会拿 0.0 当终值用（淡出）。绝对误差比较在这里更稳。
    constexpr double kOpacityEpsilon = 0.001;
    if (qAbs(windowOpacity() - target) < kOpacityEpsilon
        && m_opacityAnim->state() != QAbstractAnimation::Running) {
        return;
    }

    m_opacityAnim->stop();      // 重设起止值之前先停，避免用旧终值再跑一帧
    m_opacityAnim->setDuration(durationMs > 0 ? durationMs : kFadeDurationMs);
    m_opacityAnim->setStartValue(windowOpacity());
    m_opacityAnim->setEndValue(target);
    m_opacityAnim->start();
}

void FloatingBoxWidget::setAnimationsEnabled(bool on)
{
    if (m_animationsOn == on) {
        return;
    }
    m_animationsOn = on;

    // ⚠️ 正在淡出时把动画关掉，必须补发一次 closeRequested。
    //
    // 因为 fadeOutThenClose 是靠动画的 finished 信号去报告关闭的，
    // 而下面那句 stop() 会让 finished **永远不来** —— 结果就是
    // 主人点关闭的同时去设置里关掉动画，浮窗卡在半透明状态关不掉了
    //（manager 那边还认为它开着，再点关闭也不会重新走淡出，因为 m_closing 已置真）。
    //
    // 这个组合不常见，但后果是"关不掉的窗口"，值得专门兜一手。
    const bool closingWasPending = m_closing;

    // 刚被关掉时，若正好有动画在跑就立刻跳到终值收尾 ——
    // 否则一个"应该已经关掉动画"的程序还在播动画，主人会觉得设置没生效。
    if (!on && m_opacityAnim
        && m_opacityAnim->state() == QAbstractAnimation::Running) {
        m_opacityAnim->stop();
        setWindowOpacity(targetOpacityFromAppearance());
    }

    if (closingWasPending) {
        // 清掉标志再发信号：不清的话，主人反复开关这个设置会重复 emit，
        // 而 manager 的 closeBox 第二次拿到的是空指针
        //（第一次已经把它从哈希表里 take 走了）。
        m_closing = false;
        emit closeRequested(m_boxName);
    }
}

// ---------------------------------------------------------------------------
// 高度动画（卷起 / 展开）
// ---------------------------------------------------------------------------
void FloatingBoxWidget::setHeightImmediately(int targetHeight)
{
    // 先停掉可能正在跑的高度动画：否则它的下一帧立刻会把这次设值覆盖掉，
    // 表现为"设了但没生效"。
    if (m_heightAnim && m_heightAnim->state() == QAbstractAnimation::Running) {
        m_heightAnim->stop();
    }
    m_rollAnimating = false;

    resize(width(), targetHeight);

    // 没有动画时遮罩本该由 resizeEvent 精确重建；但 resize() 在目标高度恰好
    // 等于当前高度时不会触发 resizeEvent，遮罩可能还停在上一段动画那张
    // "偏大"的版本上（表现是底边两角变直角）。补一次收尾。
    settleRoundedMask();
}

void FloatingBoxWidget::animateHeightTo(int targetHeight)
{
    // ---- 不能动画的情形，一律退化为立即生效 ----
    //
    // ⚠️ 三条早退都必须有。它们对应三种"动画永远跑不完"的处境，
    // 而卷起/展开与淡出不同 —— 淡出跑不完只是窗口关不掉，
    // 高度动画跑不完则是**窗口卡在半高**，而且 m_rollAnimating 会一直是真，
    // 后续所有的高度记录都被跳过，展开高度再也记不住。
    //
    //   1) 窗口没显示：动画在不可见窗口上不会推进（与 animateOpacityTo 同理）。
    //      构造期恢复卷起状态走的就是这条路。
    //   2) 动画被主人关掉：直接用终值。
    //   3) 终值没变：省掉一次无意义的动画。
    if (!isVisible() || !m_animationsOn || !m_heightAnim) {
        setHeightImmediately(targetHeight);
        return;
    }
    if (height() == targetHeight) {
        return;
    }

    // ---- 解锁 ----
    //
    // ⚠️ 这一步是整个高度动画成立的前提。
    // 卷起时 applyRollUpState 设了 min == max == 30，此时 resize() 会被
    // 顶回 30 —— 动画每帧都白跑，而且不报错。实测见 tools/heightlock_diag.cpp。
    //
    // 这里只放上限，不碰下限：下限仍由调用方（applyRollUpState）决定，
    // 因为"最小能到多少"是业务约束（比如展开态不能小于 kMinimumHeight），
    // 而"最大能到多少"在动画期间必须是自由的。
    setMaximumHeight(QWIDGETSIZE_MAX);

    m_heightAnim->stop();       // 重设起止值前先停，避免用旧终值再跑一帧

    // ---- 置标志，防污染 ----
    //
    // ⚠️ 必须在 start() 之前置真：动画的第一帧就会触发 resizeEvent，
    // 晚一步就会漏掉第一帧的记录。
    m_rollAnimating = true;

    const QSize startSize = size();
    m_heightAnim->setDuration(kRollDurationMs);
    m_heightAnim->setStartValue(startSize);
    m_heightAnim->setEndValue(QSize(startSize.width(), targetHeight));

    // finished 只连一次（动画对象是全生命周期复用的，常连会累积重复回调）。
    // ---- 预置遮罩，让动画的每一帧都不必碰原生窗口区域 ----
    //
    // 一次生成"这段动画可能出现的最大高度"的圆角遮罩并下发，之后整段动画
    // 的 resizeEvent 都会命中 ensureRoundedMaskCovers 的复用分支。
    //
    // 遮罩比窗口大是**安全**的：原生窗口区域的语义是"遮罩 ∩ 窗口矩形"，
    // 多出来的部分落在窗口之外，看不见；反过来才会裁掉真实内容。
    // 代价是动画期间窗口底边暂时是直角（圆角被推到窗口下方裁掉了），
    // 220ms 的形体变化里肉眼基本抓不住，且落定后立刻精确复位。
    ensureRoundedMaskCovers(qMax(startSize.height(), targetHeight));

    connect(m_heightAnim, &QPropertyAnimation::finished, this, [this, targetHeight]() {
        // 动画结束：清标志，让 resizeEvent 恢复记录展开高度。
        m_rollAnimating = false;

        // 把高度**精确**落到终值。
        // 动画的最后一帧理论上就是终值，但经过插值可能差一两个像素；
        // 而卷起态的高度必须是精确的（后续 setMinimumHeight/MaximumHeight
        // 会拿它当基准），差一点会让"卷起后还是能拉出一点缝"。
        resize(width(), targetHeight);

        // 高度定下来了，把动画期间那张"偏大"的遮罩换成与窗口严格等大的版本。
        // （resize() 若真的改了尺寸，resizeEvent 里已经精确重建过，这里是空操作。）
        settleRoundedMask();

        // 尺寸定下来了，落一次盘。
        // 动画期间的 resizeEvent 每次都会重开去抖定时器，所以真正的落盘
        // 一直推迟到动画结束 —— 这里主动补一次，免得主人紧接着就退出程序。
        scheduleGeometrySave();
    }, Qt::SingleShotConnection);

    m_heightAnim->start();
}

// ---------------------------------------------------------------------------
// 淡入 / 淡出
// ---------------------------------------------------------------------------
void FloatingBoxWidget::prepareFadeIn(bool skip)
{
    // 批量恢复不淡入：窗口直接以终值出现，什么都不必准备。
    if (skip) {
        return;
    }

    // 只在"确实会淡入"时才置 0。
    // 动画关掉的情况下置 0 是危险的：若后续因为任何原因没能走到 fadeIn，
    // 窗口就永远停在全透明 —— 一个看不见又关不掉的窗口。
    // 所以这里把"能不能动画"的判断也一并做了，让它与 fadeIn 的条件严格一致。
    if (!m_animationsOn || !m_opacityAnim) {
        return;
    }

    // 直接设值，不走 setOpacityImmediately —— 后者会去碰动画状态，
    // 而此刻无论如何都不该有动画在跑（窗口还没显示过）。
    setWindowOpacity(0.0);
}

void FloatingBoxWidget::fadeIn(bool skip)
{
    const double target = targetOpacityFromAppearance();

    // skip：启动恢复一次开出多个浮窗时用。
    // 五个浮窗同时淡入会显得很乱（像程序在闪），而且它们本来就是
    // "上次退出时就在那儿"的，静悄悄出现才对。
    //
    // 注意这里**也要**显式设终值：prepareFadeIn 在 skip 时直接返回、
    // 没动过不透明度，所以窗口此刻是构造时的 1.0（或上次留下的值），
    // 而外观可能要求 0.7 —— 不设的话半透明浮窗会以全不透明出现。
    if (skip) {
        setOpacityImmediately(target);
        return;
    }

    // 动画关掉时同样落到终值即可（prepareFadeIn 也没置 0）。
    if (!m_animationsOn || !m_opacityAnim) {
        setOpacityImmediately(target);
        return;
    }

    // 到这里不透明度应当是 prepareFadeIn 在 show 之前置好的 0。
    // animateOpacityTo 内部用 setStartValue(windowOpacity()) 取当前值当起手值，
    // 所以正常路径下动画就是从 0 滑到终值。
    //
    // 若日后有人只调 fadeIn 而忘了配对调 prepareFadeIn，起手值会变成 1.0
    //（构造后的默认）而不是 0 —— 动画仍能跑完、终值仍正确，
    // 只是少了一段"从无到有"的淡入。这叫"漏调也不出可见故障"，
    // 比依赖调用方守规矩更稳。
    animateOpacityTo(target);
}

void FloatingBoxWidget::fadeOutThenClose()
{
    // 防重入：已经在淡出了就什么都不做。
    // 不防的话，动画期间连点两次关闭会 emit 两次 closeRequested，
    // 而 manager 的 closeBox 第二次拿到的是空指针（第一次已 take），
    // 虽然它做了空指针保护不会崩，但语义上是"关了两遍"，不干净。
    if (m_closing) {
        return;
    }
    m_closing = true;

    // 动画关掉时直接报告关闭，行为与加动画之前逐字一致 ——
    // 这是回归保障的落点之一。
    //
    // ⚠️ 窗口不可见时也必须直接报告。因为 animateOpacityTo 对未显示的窗口
    // 会退化成"直接设值、不起动画"（见那里的说明），而本函数是**靠动画的
    // finished 信号**去发 closeRequested 的 —— 动画没跑，信号就永远不来，
    // 浮窗会变成一个关不掉的东西（manager 那边一直以为它还开着）。
    // 这条路径在"程序退出时 closeAll 逐个关闭"等场景下会真实走到。
    if (!m_animationsOn || !m_opacityAnim || !isVisible()) {
        emit closeRequested(m_boxName);
        return;
    }

    // ⚠️ 已经全透明时直接报告关闭。
    // 这种情况下 animateOpacityTo 会走"终值没变就不用动画"的早退分支，
    // 于是动画根本不会跑、finished 也不会来 —— 又是一个"关不掉"的坑。
    // 全透明只在"淡入还没开始就被要求关闭"这类极端时序里出现，
    // 但代价只是三行判断，不值得留一个关不掉的窗口给别人踩。
    constexpr double kFullyTransparent = 0.01;
    if (windowOpacity() < kFullyTransparent) {
        emit closeRequested(m_boxName);
        return;
    }

    // 动画结束时才报告关闭。这样 manager 收到信号去 hide + deleteLater 时，
    // 淡出已经播完了。
    //
    // 用 SingleShotConnection 让它只触发一次，省掉"手动断开连接"的顾虑 ——
    // 动画对象是全生命周期复用的，常连会让下一次淡入又挂上一个重复的回调。
    connect(m_opacityAnim, &QPropertyAnimation::finished,
            this, [this]() { emit closeRequested(m_boxName); },
            Qt::SingleShotConnection);

    animateOpacityTo(0.0);
}

int FloatingBoxWidget::heightNeededForAppearance() const
{
    // 列表模式下不需要为图标预留空间。
    if (m_appearance.viewMode == BoxAppearance::ViewMode::List) {
        return 0;
    }

    const int px    = m_appearance.effectiveIconSize();
    const int cellH = px + 40;      // 与 ItemListWidget::applyViewMode 的 gridSize 对应
    const int chrome = kTitleBarHeight + kActionBarHeight + kGripRowHeight + 8;

    // 按 2 行算"够用"的高度：够放下图标而不出现滚动条，
    // 又不至于把桌面占掉一大块。
    return chrome + cellH * 2;
}

void FloatingBoxWidget::resizeForAppearance()
{
    // 卷起状态：高度被 setMinimumHeight/MaximumHeight 锁死，此刻 resize 改高度
    // 会被直接顶掉；而且这时图标根本不可见，调整没有任何意义。
    //
    // 展开时补上这一次 —— 见 applyRollUpState 的展开分支。
    if (m_rolledUp) {
        return;
    }

    // 列表模式下不需要为图标预留空间，什么都不做。
    if (m_appearance.viewMode == BoxAppearance::ViewMode::List) {
        return;
    }

    const int px    = m_appearance.effectiveIconSize();
    const int cellW = px + 32;      // 与 ItemListWidget::applyViewMode 的 gridSize 对应

    // 按 3 列算"够用"的宽度。
    const int targetW = cellW * 3 + 16;
    const int targetH = heightNeededForAppearance();

    // 下限也要跟着图标放大，否则大图标模式下窗口最小只能拉到 220 宽，
    // 一个格子都放不下，界面会变成一团糟。
    setMinimumWidth(qMax(kMinimumWidth, cellW + 16));

    // ⚠️ 只放大、不缩小。
    // 主人可能特意把窗口拉大过，自动缩小会抹掉他手动调的尺寸，
    // 而且"窗口被程序改了大小"本身就很招人烦。
    // 代价：切回小图标后右侧会留白 —— 这是刻意的取舍，不是遗漏。
    if (targetW > width() || targetH > height()) {
        resize(qMax(targetW, width()), qMax(targetH, height()));
        // resize 会触发 resizeEvent，那里已经会 scheduleGeometrySave()，
        // 所以这里不用再手动调一次落盘。
    }
}

// ---------------------------------------------------------------------------
// 右键菜单
// ---------------------------------------------------------------------------
void FloatingBoxWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);

    // ---- 总在最前 ----
    // 做成可关是刻意的：置顶会盖住全屏视频和游戏，主人要有办法让它让位。
    QAction *onTop = menu.addAction(tr("总在最前"));
    onTop->setCheckable(true);
    onTop->setChecked(m_service->settings()->alwaysOnTop());
    connect(onTop, &QAction::triggered, this, [this](bool checked) {
        m_service->settings()->setAlwaysOnTop(checked);
        applyAlwaysOnTop(checked);
        // 重新 show 之后窗口可能失去之前的卷起/尺寸状态表现，刷新一次标题栏计数。
        refreshItems();
    });

    // ---- 卷起 / 展开 ----
    QAction *rollUp = menu.addAction(m_rolledUp ? tr("展开") : tr("卷起"));
    connect(rollUp, &QAction::triggered, this, &FloatingBoxWidget::onToggleRollUp);

    // ---- 钉住 ----
    //
    // 与标题栏那个锁图标是同一个开关的两个入口。两个入口都保留：
    // 图标是"随手就能点"，菜单项是"能看见它当前是开还是关"（勾选态），
    // 而一个 🔒/🔓 字符在余光里分不出状态。
    QAction *pin = menu.addAction(tr("钉住（不参与让位，固定在最底层）"));
    pin->setCheckable(true);
    pin->setChecked(m_locked);
    connect(pin, &QAction::triggered, this, [this](bool checked) {
        emit lockChangeRequested(m_boxName, checked);
    });

    // ---- 悬停自动展开 ----
    //
    // 紧跟在「卷起 / 展开」之后：两者都是"窗口什么时候该展开"这件事，
    // 放一起读起来才连贯。它勾的是**全局开关**（所有浮窗共用一份配置），
    // 而不是本窗口的某个私有状态 —— 后者是"手动卷起"，没有菜单项，
    // 靠手动卷起/展开本身表达。
    QAction *hover = menu.addAction(tr("悬停自动展开"));
    hover->setCheckable(true);
    hover->setChecked(m_service->settings()->hoverExpandEnabled());
    hover->setToolTip(tr("鼠标停在浮窗上自动展开，移开自动卷起（对所有浮窗生效）"));
    connect(hover, &QAction::triggered, this, [this](bool checked) {
        // 写配置后**必须经 manager 广播**，否则控制中心那边的勾选状态不会更新，
        // 而且另外几个开着的浮窗仍然按旧值工作 —— 这正是"两个入口"最容易
        // 出现不一致的地方（与外观的 appearanceChangeRequested 同一个理由）。
        emit hoverExpandChangeRequested(checked);
    });

    // ---- 外观 ----
    // 放在「卷起」之后、「还原选中条目」之前：前者是窗口自身的状态，
    // 后者开始就是针对盒内文件的操作了，外观插在中间属于同类相聚。
    buildAppearanceMenu(&menu);

    menu.addSeparator();

    // ---- 还原选中条目 ----
    // 浮窗的双击是"打开"，所以"还原"必须另给一个入口，否则主人没有别的办法
    // 把东西从浮窗里放回桌面（拖出虽然也行，但拖到哪不总是可控）。
    const QStringList selected = m_itemList->selectedPaths();
    QAction *restore = menu.addAction(selected.size() > 1
                                          ? tr("还原选中的 %1 项到桌面").arg(selected.size())
                                          : tr("还原到桌面"));
    restore->setEnabled(!selected.isEmpty());
    connect(restore, &QAction::triggered, this, [this, selected]() {
        if (selected.isEmpty()) {
            return;
        }
        // 显式传桌面路径：单条还原场景下，这条记录当初来自用户桌面还是
        // 公共桌面已无从得知（origin 只在收纳那一刻存在于内存里）。
        // 保守还原到用户桌面 —— 文件不会丢，主人在桌面上照样看得到。
        m_service->restorePaths(selected, CoreNames::desktopRoot());
    });

    if (m_rolledUp) {
        // 卷起时列表是隐藏的，"还原选中项"没有操作对象，禁用掉更诚实。
        restore->setEnabled(false);
    }

    menu.addSeparator();

    // ---- 在控制中心中显示 ----
    QAction *reveal = menu.addAction(tr("在控制中心中显示"));
    connect(reveal, &QAction::triggered, this, [this]() {
        emit revealInControlCenterRequested(m_boxName);
    });

    // ---- 弹出菜单本身 ----
    //
    // ⚠️⚠️ 这两行标志是本步最容易被忽略、后果却最尴尬的一处。
    //
    // QMenu::exec() 会进入一个**模态事件循环**。菜单一弹出来，鼠标在系统看来
    // 就已经不在浮窗上了 —— Qt 立刻给浮窗发一个 leaveEvent。
    // 不挡的话，每一次右键都会先把浮窗卷起来：菜单还开着，底下的窗口却缩成
    // 一条线；而菜单里「展开」那一项的文案是按弹菜单**之前**的状态生成的，
    // 点下去会让窗口展开成一个已经卷起的窗口 —— 状态彻底对不上。
    //
    // 用 RAII 而不是"exec 前设真、exec 后设假"两句：QMenu::exec 虽然不抛异常，
    // 但中间那一段有 return 的风险（日后有人加个提前返回就漏了复位），
    // 而漏复位的后果是**这个浮窗从此再也不自动展开/收起**，非常难查。
    ModalGuard guard(&m_contextMenuOpen);

    menu.exec(event->globalPos());
}

void FloatingBoxWidget::buildAppearanceMenu(QMenu *parentMenu)
{
    if (!parentMenu) {
        return;
    }

    QMenu *appearanceMenu = parentMenu->addMenu(tr("外观"));

    // ---- 显示方式（互斥四项）----
    QMenu *viewMenu = appearanceMenu->addMenu(tr("显示方式"));
    auto *viewGroup = new QActionGroup(viewMenu);

    // ⚠️ QActionGroup 的 parent 必须给**子菜单自身**，不能给 this。
    // 菜单是每次右键现场构造的栈上对象，随 menu 一起销毁；
    // 若 group 挂在 this（浮窗）上，每次打开菜单都会攒下一个再也用不到的
    // group，而它管理的那些 QAction 早已随菜单销毁 —— 反复右键就是持续泄漏。
    viewGroup->setExclusive(true);

    struct ViewChoice
    {
        BoxAppearance::ViewMode mode;
        QString                 label;
    };
    const ViewChoice choices[] = {
        {BoxAppearance::ViewMode::List,       tr("列表")},
        {BoxAppearance::ViewMode::SmallIcon,  tr("小图标")},
        {BoxAppearance::ViewMode::MediumIcon, tr("中图标")},
        {BoxAppearance::ViewMode::LargeIcon,  tr("大图标")},
    };

    for (const ViewChoice &choice : choices) {
        QAction *act = viewMenu->addAction(choice.label);
        act->setCheckable(true);
        act->setChecked(m_appearance.viewMode == choice.mode);
        viewGroup->addAction(act);

        const BoxAppearance::ViewMode mode = choice.mode;
        connect(act, &QAction::triggered, this, [this, mode]() {
            // 读当前外观 -> 只改这一项 -> 整份写回。
            // 之所以要"读改写"而不是直接构造一份新的：外观是三项打包的，
            // 直接构造会把另外两项（透明度、显式图标尺寸）重置掉。
            BoxAppearance next = m_appearance;
            next.viewMode = mode;
            // 切换视图模式时把显式图标尺寸清掉，让它重新跟随模式推导 ——
            // 否则从小图标切到大图标会一直沿用旧的小尺寸，看着像没生效。
            next.iconSize = 0;

            emit appearanceChangeRequested(m_boxName, next);
        });
    }

    // ---- 透明度（档位，不用滑块）----
    // 菜单里放滑块交互很别扭（拖动时菜单会关掉），档位够用。
    QMenu *opacityMenu = appearanceMenu->addMenu(tr("透明度"));
    auto *opacityGroup = new QActionGroup(opacityMenu);
    opacityGroup->setExclusive(true);

    // 最低 60%：菜单里给的是"常用档"，不是全量范围。
    // 想要 20% 那种极端值走「更多设置…」里的滑块（那里有实时预览）。
    const int levels[] = {100, 90, 80, 70, 60};

    // 当前值不在档位里（例如在设置对话框里拖到了 85%）时，
    // 单独补一项显示它并打勾 —— 否则菜单里一个勾都没有，
    // 主人会以为"透明度没设置成功"。
    bool currentListed = false;
    for (int level : levels) {
        if (m_appearance.opacity == level) {
            currentListed = true;
            break;
        }
    }
    if (!currentListed && m_appearance.opacity > 0) {
        QAction *current = opacityMenu->addAction(tr("%1%（当前）").arg(m_appearance.opacity));
        current->setCheckable(true);
        current->setChecked(true);
        opacityGroup->addAction(current);
        // 不给它连 triggered：点自己当前的值不该有副作用，
        // 而 QActionGroup 已经保证它会一直是勾选态。
        opacityMenu->addSeparator();
    }

    for (int level : levels) {
        QAction *act = opacityMenu->addAction(tr("%1%").arg(level));
        act->setCheckable(true);
        act->setChecked(m_appearance.opacity == level);
        opacityGroup->addAction(act);

        connect(act, &QAction::triggered, this, [this, level]() {
            BoxAppearance next = m_appearance;
            next.opacity = level;
            emit appearanceChangeRequested(m_boxName, next);
        });
    }

    appearanceMenu->addSeparator();

    // ---- 更多设置 ----
    QAction *more = appearanceMenu->addAction(tr("更多设置…"));
    connect(more, &QAction::triggered, this, [this]() {
        emit openAppearanceDialogRequested(m_boxName);
    });
}

// ---------------------------------------------------------------------------
// 窗口标志：总在最前
// ---------------------------------------------------------------------------
bool FloatingBoxWidget::isAlwaysOnTop() const
{
    // 直接看窗口标志，而不是读配置 —— 配置是"主人想要什么"，
    // 标志是"窗口现在实际是什么"。置顶切换的那一瞬间两者会短暂不一致。
    return windowFlags().testFlag(Qt::WindowStaysOnTopHint);
}

void FloatingBoxWidget::applyAlwaysOnTop(bool on)
{
    Qt::WindowFlags flags = windowFlags();

    if (on) {
        flags |= Qt::WindowStaysOnTopHint;
    } else {
        flags &= ~Qt::WindowStaysOnTopHint;
    }

    if (flags == windowFlags()) {
        return;     // 没变化就不要走下面那套 hide/show，免得窗口闪一下
    }

    setWindowFlags(flags);

    // ⚠️ Qt 的已知行为：setWindowFlags 会**隐藏**窗口，必须重新 show 才能恢复。
    // 而且重新 show 之后窗口的激活状态会丢，所以补一次 raise 让它在最前面，
    // 否则改了置顶设置的瞬间浮窗会消失、主人以为程序崩了。
    show();
    raise();

    // ⚠️ 重新 show 会重建原生窗口，而不透明度是挂在原生窗口上的属性 ——
    // 重建之后它会被重置成 1.0，半透明的浮窗会在切换置顶时突然变实。
    // 所以这里把当前该有的透明度重新落一遍（不走动画：这是一次"恢复现场"，
    // 不是一次"变化"，让主人看到一个渐变的淡入会很莫名其妙）。
    setOpacityImmediately(targetOpacityFromAppearance());

    // 圆角遮罩重算一次。
    //
    // 实测（tools/topmost_diag.cpp）：本机 Qt 6.11.1 上 setWindowFlags
    // **不会**弄坏 mask —— 切换前后指纹完全一致，退掉这一句探针也全过。
    // 所以这不是某个已知 bug 的修复，而是一道防御：mask 是原生窗口的属性，
    // 而 setWindowFlags 会重建原生窗口，将来 Qt 版本或平台行为变了都可能
    // 让它失配。重算一次的开销是一次位图绘制，可以忽略。
    updateRoundedMask();

    update();
}

// ---------------------------------------------------------------------------
// 浮窗之间互相让位（由 FloatingBoxManager 驱动）
// ---------------------------------------------------------------------------
void FloatingBoxWidget::slideByForLayout(int dy)
{
    if (dy == 0) {
        // ---- 滑回原位 ----
        //
        // 没有基线说明它本来就没被推开过，什么都不用做。
        // 这一条同时挡掉了"卷起时对每个浮窗都调一次"带来的无谓动作。
        if (!m_hasRestPos) {
            return;
        }

        const QPoint back = m_restPos;

        // 目标回到原位，累计偏移随之归零。必须赶在 animatePosTo 之前清：
        // 若滑回途中又来了新的"被推开"请求，slideByForLayout 要能从这个 0
        // 起累加，算出来的目标才是"原位 + 新位移"，而不是被旧偏移顶出去。
        m_layoutOffset = 0;

        // ⚠️ 基线要等到**真的滑回去之后**才能清，不能在这里清。
        //
        // 若在这里清，而滑回动画还没跑完时又来了一个"被推开"请求
        //（主人手快，收起一个又展开另一个），那时基线已经清掉，
        // 于是会把**动画中途的位置**当成原位记下来 ——
        // 正是上面警告的那种"每推一次就永久偏一点"的漂移。
        //
        // 所以交给动画结束的 finished 去清（那条连接在构造函数里挂一次，
        // 用 m_restClearPending 决定"这次要不要清"，而不是反复 connect ——
        // finished 是累积连接的，每挂一次就多一个回调）。
        m_restClearPending = true;
        animatePosTo(back);

        // 动画没跑起来就自己收尾。
        //
        // ⚠️ 两种情况都会走到这里，都必须覆盖：
        //   1) 立即生效路径（窗口不可见 / 动画被主人关了）；
        //   2) animatePosTo 里那句"pos() == target 就返回" ——
        //      此刻它已经**停在**原位了，没有动画会结束，
        //      于是 finished 永远不来，m_restClearPending 会卡在真上。
        //      卡住的后果不是这次出错，而是**下一次**滑回时标志已经是真，
        //      看起来"已经挂上了"，基线就一直清不掉 ——
        //      那个浮窗从此再也回不到原位。
        if (!m_posAnim || m_posAnim->state() != QAbstractAnimation::Running) {
            m_restClearPending = false;
            m_hasRestPos = false;
        }
        return;
    }

    // ---- 被推开 ----
    //
    // 记基线。**只在还没有基线时记** —— 这是整个机制的关键：
    // 若每次被推都重新记一遍当前位置，那么第二次被推时记下的
    // 已经是"被推开后的位置"，卷起时就回不到真正原位了，
    // 表现为浮窗每被推一次就永久往下掉一截，几轮之后跑出屏幕。
    if (!m_hasRestPos) {
        m_restPos = pos();
        m_hasRestPos = true;
        m_layoutOffset = 0;
    }

    // ⚠️ 取消上一轮"滑回原位"遗留的清理意图。
    //
    // 时序：窗口正在滑回原位（m_restClearPending=true），动画没结束就又被
    // 另一个展开的浮窗推下去。此时 animatePosTo 会 stop() 旧的回家动画并
    // 启动新的下推动画；stop() 不发 finished，所以旧 pending 会一直挂着。
    // 若不在新推动开始时取消它，新的下推动画结束时就会误以为"这次是回家
    // 结束"，把 m_hasRestPos 清掉。后果正是主人现在遇到的：第三个浮窗
    // 被推下去后，最后所有浮窗收回，它却不再被当作 pushedAside，永远停在
    // 中间位置回不去原位。
    m_restClearPending = false;

    // ⚠️ 累加，而不是覆盖 —— 这里正是"二次推开会重叠"的修复点。
    //
    // dy 来自 computePushDown，语义是"相对它拿到的矩形（= 窗口当前**稳定**
    // 位置）还要往下挪多少"，而稳定位置 = m_restPos + m_layoutOffset。
    // 所以新目标偏移 = 旧偏移 + dy。
    //
    // 写成"目标 = m_restPos + dy"（覆盖）时，第一次推没事（此时偏移为 0），
    // 第二次就会把已经推开的那一段抹掉：窗口不降反升，与第二个锚点叠在一起。
    m_layoutOffset += dy;

    animatePosTo(m_restPos + QPoint(0, m_layoutOffset));
}

// 展开后会占据的矩形。
//
// 与 manager 的分工：manager 要用它算"展开时挡住了谁"，而那一刻
// 高度动画才刚启动，当前几何还是卷起时的细线。
QRect FloatingBoxWidget::expandedGeometry() const
{
    // 高度动画正在跑：它的 endValue 就是这一轮的终值尺寸。
    //
    // 用 endValue 而不是"自己按外观重算一遍高度"：重算要复制
    // applyRollUpState 展开分支里那一串 rememberedHeight/appearanceHeight
    // 的取值逻辑，两处必然漂移 —— 而漂移的表现是"推开的位置差一点点"，
    // 肉眼几乎看不出来但会一直存在。
    if (m_heightAnim && m_heightAnim->state() == QAbstractAnimation::Running) {
        QRect rect = frameGeometry();
        rect.setSize(m_heightAnim->endValue().toSize());
        return rect;
    }

    return frameGeometry();
}

// 让位动画全部落定后会处的矩形。
//
// 与 expandedGeometry 的区别：那个只取高度动画的终值；这里还要把**位置**
// 动画的终值算进来。连续快速让位时 frameGeometry() 只是中间帧，
// 用它算位移会让第二轮偏掉（见头文件的说明）。
QRect FloatingBoxWidget::layoutStableGeometry() const
{
    QRect rect = frameGeometry();

    // 高度动画在跑：用终值高度 —— 被推窗口自身的高度决定它占多少垂直空间。
    if (m_heightAnim && m_heightAnim->state() == QAbstractAnimation::Running) {
        rect.setSize(m_heightAnim->endValue().toSize());
    }

    // 位置动画在跑：用终值位置 —— 这一轮落定后它才会停在那里。
    if (m_posAnim && m_posAnim->state() == QAbstractAnimation::Running) {
        rect.moveTopLeft(m_posAnim->endValue().toPoint());
    }

    return rect;
}

void FloatingBoxWidget::setPosImmediately(const QPoint &target)
{
    // 先停掉可能在跑的位置动画：否则它的下一帧立刻会把这次设值覆盖掉，
    // 表现为"设了但没生效"。与 setHeightImmediately 同理。
    if (m_posAnim && m_posAnim->state() == QAbstractAnimation::Running) {
        m_posAnim->stop();
    }
    // stop() 会经 stateChanged 把 m_posAnimating 清掉；这里再兜一次，
    // 防止动画对象尚未创建（构造期）时该标志没有写入方。
    m_posAnimating = false;

    move(target);
}

void FloatingBoxWidget::animatePosTo(const QPoint &target)
{
    // ---- 不能动画的情形，一律退化为立即生效 ----
    //
    // 与 animateHeightTo 同样三条，理由也一样：动画在不可见窗口上不会推进、
    // 主人关掉了动画、动画对象没了。前两条若不退化，m_posAnimating
    // 会永远停在真上（动画永远跑不到停止），这个浮窗之后再也不记录
    // 任何几何变化 —— 主人拖动它、退出、重启，位置回到很久以前。
    if (!isVisible() || !m_animationsOn || !m_posAnim) {
        setPosImmediately(target);
        return;
    }

    // ⚠️ 先 stop()，再判"是不是已经在目标位置"。
    //
    // 顺序反过来的话有个漏洞：若此刻有一段动画正朝**别处**跑，
    // 而新目标恰好等于**当前**位置，那句早退会直接返回，
    // 旧动画继续跑它的 —— 结果窗口停在旧目标上，而不是新目标。
    // 表现在让位场景里就是"某个窗口该收回原位，却仍然停在被推开的位置"。
    m_posAnim->stop();      // 重设起止值前先停，避免用旧终值再跑一帧

    if (pos() == target) {
        return;
    }

    m_posAnim->setStartValue(pos());
    m_posAnim->setEndValue(target);
    m_posAnim->start();
    // m_posAnimating 由上面连的 stateChanged 负责置位与清除 ——
    // 不在这里手动置，是为了保证"置位/清除"只有一处发生，
    // 否则 stop() 打断动画时两处逻辑会打架。
}

void FloatingBoxWidget::setLocked(bool locked)
{
    if (m_locked == locked) {
        return;
    }
    m_locked = locked;

    if (m_titleBar) {
        m_titleBar->setLockedLook(locked);
    }

    // 上锁时把两个悬停定时器停掉、并清掉"这次是悬停展开的"这个标记。
    //
    // 【定时器为什么必须停】
    // 上锁的那一刻，鼠标可能正停在浮窗上、展开计时器正在跑。不停的话它
    // 到点后仍会去展开 —— 而 hoverInteractionBlocked() 会在**回调里**再判
    // 一次，所以实际不会展开（这一条是安全的）。
    // 真正要处理的是**离开计时器**：若上锁时它已经在跑，解锁之后
    // 那个过期的计时到点就可能把窗口收起来，主人看到的是
    // "我刚解锁，它自己就卷了"。停掉最省心。
    //
    // 【m_hoverExpanded 为什么要清】
    // 主人上锁时窗口多半正悬停展开着。若留着这个标记，解锁后的下一次
    // leave 会立刻把它收回去 —— 而上锁期间他显然希望它**停在这个样子**。
    // 清掉它 = 把当前状态当成"主人手动摆成的样子"，之后不再自动收回。
    // 这与规格"锁定时保持当下状态不动"是一致的。
    stopHoverTimers();
    m_hoverExpanded = false;

    // ---- 钉住 = 压到最底层 ----
    //
    // 取消钉住时**不**自动恢复置顶：主人可能是先钉住、后来取消，
    // 但他当初的"总在最前"偏好是他自己的设置，不该被这个开关代管。
    // 所以只有"钉住"这一个方向动窗口层级，取消时保持原样
    //（他会看到窗口停在底层，想置顶就去右键菜单点一下）。
    if (!locked) {
        return;
    }

    // ⚠️ 构造期不能走 applyAlwaysOnTop：它会调 show()。
    //
    // manager 在 openBox 里、prepareFadeIn **之前**就会同步一次钉住状态；
    // 若这里把窗口 show 出来，淡入的三步顺序就被打乱了 ——
    // 窗口先以完全不透明显示一帧，再被 prepareFadeIn 置 0 重新淡入，
    // 表现是"开启时闪一下"。这与构造期设置顶标志是同一个坑，
    // 那里的做法也是只设标志、不 show。
    const bool notShownYet = !isVisible();
    if (notShownYet) {
        setWindowFlags(windowFlags() & ~Qt::WindowStaysOnTopHint);
        return;
    }

    applyAlwaysOnTop(false);
}

// ---------------------------------------------------------------------------
// 圆角遮罩
// ---------------------------------------------------------------------------
void FloatingBoxWidget::applyRoundedMask(int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }

    // 用一个抗锯齿的位图 mask，而不是直接 Region。
    // 直接 QRegion 是硬边、四角会有明显锯齿；8px 半径下用位图画能磨平。
    QBitmap mask(w, h);
    mask.fill(Qt::color0);          // 先全透明

    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setBrush(Qt::color1);   // 再用"不透明"刷出圆角矩形
    painter.setPen(Qt::NoPen);

    // 内缩半像素：抗锯齿的边界会落在半个像素上，不内缩的话最外一圈
    // 会被削掉一像素，圆角看着比 8px 小一点。
    painter.drawRoundedRect(QRectF(0, 0, w, h).adjusted(0.5, 0.5, -0.5, -0.5),
                            kCornerRadius, kCornerRadius);
    painter.end();

    m_maskBitmap = mask;
    m_maskSize   = QSize(w, h);
    setMask(mask);
}

void FloatingBoxWidget::updateRoundedMask()
{
    // 尺寸还没定（构造期 width/height 可能还是默认的 640x480）时先跳过，
    // resizeEvent 会在真正显示前再调一次。
    if (width() <= 0 || height() <= 0) {
        return;
    }

    // 精确重建，并与当前窗口严格等大。
    //
    // ⚠️ 这里刻意**不做"尺寸没变就跳过"**：applyAlwaysOnTop 会 setWindowFlags，
    // 那会重建原生窗口，而遮罩是原生窗口的属性 —— 那条路径需要无条件重设一次
    // 作为防御（见该函数末尾的说明）。走缓存跳过会把这层防御悄悄抹掉。
    applyRoundedMask(width(), height());
}

void FloatingBoxWidget::settleRoundedMask()
{
    // 动画落定后调用。已经严格等大就什么都不用做；
    // 否则（动画期间用的是"偏大"的遮罩）在这里补一次精确重建。
    //
    // 与 updateRoundedMask 的差别只有一个前提：这里的场景不涉及重建原生窗口，
    // 所以"已经精确匹配"时允许直接返回。
    if (width() <= 0 || height() <= 0) {
        return;
    }
    if (!m_maskBitmap.isNull() && m_maskSize == size()) {
        return;
    }
    applyRoundedMask(width(), height());
}

void FloatingBoxWidget::ensureRoundedMaskCovers(int minHeight)
{
    const int w = width();
    const int h = qMax(height(), minHeight);
    if (w <= 0 || h <= 0) {
        return;
    }

    // 现有遮罩已经完整覆盖窗口 -> 原地复用，不重画、不 setMask()。
    if (!m_maskBitmap.isNull() && m_maskSize.width() == w
        && m_maskSize.height() >= h) {
        return;
    }

    // 必须重设：宽度变了，或窗口长过了现有遮罩。
    // 绝不能生成比窗口矮的遮罩 —— 那会把窗口底部一块真实内容裁掉。
    applyRoundedMask(w, h);
}

// ---------------------------------------------------------------------------
// 几何
// ---------------------------------------------------------------------------
void FloatingBoxWidget::applySavedGeometry(const QByteArray &blob)
{
    if (blob.isEmpty()) {
        // 首次打开：放在屏幕上一个合理位置 —— 靠右上角、留出边距，
        // 避开常见的任务栏（底部）与主窗口默认位置（居中）。
        if (QScreen *screen = QApplication::primaryScreen()) {
            const QRect avail = screen->availableGeometry();
            const int x = avail.right() - kDefaultWidth - 40;
            const int y = avail.top() + 60;
            move(qMax(avail.left(), x), qMax(avail.top(), y));
        }
        return;
    }

    // restoreGeometry 会一并校验屏幕是否还存在（blob 里含屏幕标识），
    // 多显示器拔插后 Qt 能把窗口挪回可见区 —— 这正是选 blob 而非 x/y/w/h 的理由。
    restoreGeometry(blob);

    // ⚠️ 顺序要紧：blob 里存的是**展开状态**下的尺寸，而卷起状态的窗口
    // 有固定的最小/最大高度限制，restoreGeometry 会把那条限制顶开、
    // 让卷起的浮窗变回一个大方块。故恢复几何之后必须重新施加一次卷起状态。
    if (m_rolledUp) {
        applyRollUpState(true);
    }
}

QByteArray FloatingBoxWidget::currentGeometryBlob() const
{
    // ⚠️ 正处于"被推开"状态时，落盘的必须是**原位**，不是当前位置。
    //
    // moveEvent 那边已经挡住了动画期间的主动落盘，但还有两条路绕过了它：
    //   * 退出时 manager 会调 saveAllGeometry() —— 若主人恰好在让位动画
    //     没跑完时退出程序（或动画刚结束、位置就停在被推开处），
    //     那次落盘写下的就是被推开的位置；
    //   * 去抖定时器到点时窗口可能已经被推到新位置。
    // 两条都在这里一次性收口：只要还留着基线，就按原位生成 blob。
    //
    // 做法是"临时把窗口挪回原位、取 blob、再挪回来"。
    // 看着笨，但 saveGeometry() 没有"按给定位置序列化"的重载，
    // 而重写一份 blob 的序列化格式等于去依赖 Qt 的私有布局（更脆）。
    // 这个函数只在落盘时被调用（去抖之后、退出时各一次），不在热路径上。
    if (!m_hasRestPos) {
        return saveGeometry();
    }

    FloatingBoxWidget *self = const_cast<FloatingBoxWidget *>(this);
    const QPoint actual = self->pos();

    // ⚠️ 用 m_posAnimating 临时挡住 moveEvent 里的落盘。
    //
    // 下面两次 move() 会各触发一次 moveEvent，那里会 scheduleGeometrySave()。
    // 不挡的话，这个"只为取 blob 而做的临时挪动"反而把去抖定时器重启了一次，
    // 于是又排一次落盘 —— 而且那次落盘读到的位置正好是**我们挪回原位后
    // 又挪回来**的中间态，比不修还糟。
    //
    // 借 m_posAnimating 而不是新加一个标志：它的语义正是
    // "这次移动是程序自己做的，别当成主人的意图"，与这里的诉求完全一致。
    // const 函数里改成员，所以上面取了非 const 的 self。
    const bool savedFlag = self->m_posAnimating;
    self->m_posAnimating = true;

    self->move(m_restPos);
    const QByteArray blob = self->saveGeometry();
    self->move(actual);

    self->m_posAnimating = savedFlag;

    return blob;
}

void FloatingBoxWidget::scheduleGeometrySave()
{
    if (!m_geometryDebounce) {
        return;
    }

    // 重开定时器即实现去抖：拖动会连续触发 moveEvent，
    // 每次都 start() 会把计时推后，只有停下来 500ms 后才真正落盘。
    // 不去抖的话每像素写一次 INI，拖动会明显卡顿（QSettings 每次都要
    // 重新构造并 sync 到磁盘）。
    m_geometryDebounce->start();
}

void FloatingBoxWidget::onGeometryDebounceTimeout()
{
    emit geometryChanged(m_boxName, currentGeometryBlob());
}

void FloatingBoxWidget::moveEvent(QMoveEvent *event)
{
    QWidget::moveEvent(event);

    // ⚠️ 被动移动（被其他浮窗推开）**不落盘**。
    //
    // 被推开的位移是"临时让位"，不是主人的意图。若照常落盘，
    // 下次启动浮窗会带着被推开的位置醒来 —— 而那时推开它的那个浮窗
    // 可能根本没开（或者位置完全不同），于是桌面上的布局整个错位，
    // 且主人完全不知道为什么窗口跑到了那里（他记得自己没动过）。
    //
    // m_posAnimating 由位置动画的 stateChanged 维护，覆盖整个动画区间 ——
    // 只在 move() 前后包一下是不够的，原因见头文件里那个成员的说明。
    //
    // 用 m_posAnimating 而不是 m_layoutAdjusting：后者还有一个写入方
    //（applyRollUpState 的展开分支），两者会互相清除。详见头文件里的说明。
    //
    // 注意：主人**手动拖动**时这个标志是假，落盘照常 ——
    // 挡的只是程序自己造成的位移，不是主人的真实操作。
    if (m_posAnimating) {
        return;
    }

    scheduleGeometrySave();
}

void FloatingBoxWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);

    // 圆角遮罩必须跟着尺寸重算 —— mask 是按当前像素尺寸生成的位图，
    // 尺寸一变它就失配（表现为圆角错位或干脆消失）。
    //
    // ⚠️ 唯一的例外是高度动画（m_rollAnimating）。卷起/展开会连续 220ms
    // 每帧改一次高度，而 setMask() 是"把遮罩转成原生窗口区域"的平台调用：
    // 离屏实测约 800us/次，**与区域复杂度无关**（连一个平凡矩形也要 813us，
    // 见 tools/mask_perf.cpp）—— 那笔开销是每帧钉在 resize 路径上的。
    //
    // 所以动画期间退成"够用就行"：只在窗口长过现有遮罩时才重设，
    // 缩小方向一次都不碰。animateHeightTo 启动前已经把遮罩预先撑到
    // 这段动画可能出现的最大高度，正常情况下一帧都不会走到重设分支；
    // 动画结束时由 finished 回调 settleRoundedMask() 把圆角精确复位。
    if (m_rollAnimating) {
        ensureRoundedMaskCovers(height());
    } else {
        updateRoundedMask();
    }

    // 记住"展开状态下的高度"，供卷起后展开时恢复。
    //
    // 为什么用新尺寸而不是旧尺寸：拖动右下角改大小时，我们要的就是改完之后
    // 那个高度。用 oldSize 会永远慢一拍（记的是上一次的），
    // 主人拉到满意大小后立刻卷起再展开，会回到前一个尺寸。
    //
    // 排除三种"不是主人本意"的高度变化：
    //   1) 卷起状态本身引起的 resize（我们自己收起来的，
    //      记下来会让展开时恢复成一条细线的高度）；
    //   2) 高度动画进行中的每一帧（见 m_rollAnimating 的说明 ——
    //      不排除的话，动画被打断时展开高度会永久停在中间值）；
    //   3) 卷起/展开流程里那串"切换 min/max 与子控件可见性"引起的 resize
    //      （见 m_layoutAdjusting 的说明 —— 不排除的话，展开时 setMinimumHeight
    //       触发的那次 resize 会把记录覆盖成最小高度，展开就再也回不到原尺寸）。
    //
    // ⚠️ 悬停展开走的是 applyRollUpState(false)，会自动进 (2)(3) 两条的覆盖 ——
    // 唯一漏网的是"悬停展开动画结束时的最后一帧 resize"（那时 m_rollAnimating
    // 已被 finished 清掉）。那一帧的高度**就是**展开高度，记下来是对的，
    // 所以不需要为悬停再加守卫条件。
    if (!m_rolledUp && !m_rollAnimating && !m_layoutAdjusting
        && event->size().height() > rolledUpHeight()) {
        m_expandedHeight = event->size().height();
    }

    scheduleGeometrySave();
}

void FloatingBoxWidget::closeEvent(QCloseEvent *event)
{
    // 浮窗不做销毁决策：只报告"主人想关我"，由 FloatingBoxManager 决定何时 delete。
    // 直接 accept 会让 Qt 隐藏窗口，但对象还活着、manager 的哈希表也还存着它，
    // 状态就分裂了。故先发信号，让 manager 走它自己的关闭流程。
    //
    // 走 fadeOutThenClose 而不是直接 emit：让关闭也有淡出。
    // 它内部处理了防重入与"动画关掉时直接发信号"两条路径。
    fadeOutThenClose();

    // ⚠️ 仍然 ignore：窗口的隐藏与销毁一律等 manager 的 closeBox 来做。
    // 若这里 accept，Qt 会立刻隐藏窗口，淡出动画就再也看不到了。
    event->ignore();
}
