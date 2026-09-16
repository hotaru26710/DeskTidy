#ifndef FLOATINGBOXWIDGET_H
#define FLOATINGBOXWIDGET_H

// ---------------------------------------------------------------------------
// ui/floatingboxwidget.h —— 桌面常驻浮窗：一个收纳盒在桌面上的化身。
//
// 【它是什么】
// 每个开着的收纳盒对应桌面上一个小窗口，主人不用打开控制中心就能：
//   * 看见盒里装了什么（内嵌 ItemListWidget，系统真实图标）；
//   * 双击条目直接用系统默认方式打开（浮窗是"使用"界面，不是"管理"界面）；
//   * 把条目拖出去，拖到哪就还原到哪；
//   * 把桌面上的文件拖进来，归入本盒；
//   * 点一下就能把桌面收进**本盒**；
//   * 撤销上一次收纳；
//   * 卷起成一条标题栏，给桌面腾地方。
//
// 【关键设计决定】
//
// 1) 浮窗「收纳桌面」收进的是**它自己代表的那个盒**，不是主窗口当前选中的盒。
//    这一点必须靠 AppService::collectInto(entries, boxPath, boxName) 显式传目标盒
//    来保证。若沿用"当前选中盒"的语义，主窗口选着 A 盒时在 B 盒浮窗上点收纳，
//    文件会跑进 A 盒 —— 主人看到的和发生的对不上，是功能性错误而非风格问题。
//
// 2) 双击语义与主窗口**刻意不同**：主窗口双击=还原（管理界面），
//    浮窗双击=打开（使用界面）。靠 ItemListWidget::Options 选择，两边都符合直觉。
//
// 3) 撤销按钮必须**点名是哪个盒**：撤销栈全局唯一、只保留最近一次收纳，
//    三个浮窗同时开着时，B 盒浮窗上的撤销按下去撤的可能是 A 盒那批。
//    所以文案分两种：「撤销本盒上次收纳（N 项）」与「撤销「工作」的收纳（N 项）」。
//
// 4) 浮窗**不做销毁决策**：点关闭只发 closeRequested，由 FloatingBoxManager
//    决定何时 delete。这样"谁拥有对象谁负责生命周期"这条线始终清楚，
//    不会出现浮窗自己 delete 自己之后 manager 的哈希表还存着野指针。
//
// 5) 几何落盘**必须去抖**：拖动窗口会连续触发 moveEvent，每像素写一次 INI
//    会明显卡顿（QSettings 每次都重新构造并 sync 到磁盘）。
//
// 6) 外观（透明度 + 视图模式）**每盒一份**，存于 Settings。
//    透明度用 QWidget::setWindowOpacity 而不是样式表 —— 见实现文件里的
//    决定性理由（列表的拖拽高亮会清空整条样式表）。
// ---------------------------------------------------------------------------

#include <QBitmap>
#include <QByteArray>
#include <QList>
#include <QPoint>
#include <QString>
#include <QWidget>

// DesktopEntry 是 collectEntries 的按值参数类型，必须完整定义 ——
// 前置声明在这里不够用：QList<T> 的实例化需要 T 是完整类型。
#include "coretypes.h"

class AppService;
class FloatingHoverOverlay;
class ItemListWidget;

class QLabel;
class QMenu;
class QPropertyAnimation;
class QPushButton;
class QSizeGrip;
class QTimer;
class QVBoxLayout;

// 自绘标题栏。
//
// 之所以抽成独立子控件而不是在主 widget 里判断"鼠标是否落在标题栏矩形内"：
// 无边框窗口需要自己实现拖动，而拖动只能由鼠标事件驱动。若在主 widget 上
// 统一处理鼠标，就必须区分"这次按下是想拖窗口"还是"想在列表里框选/拖出" ——
// 列表自身的拖拽会与窗口拖动在同一套事件里打架，判定条件写起来很脆。
// 把标题栏做成独立控件后，鼠标事件天然按控件分发，互不干扰。
class FloatingBoxTitleBar : public QWidget
{
    Q_OBJECT

public:
    explicit FloatingBoxTitleBar(QWidget *parent = nullptr);

    // 标题文字由浮窗设置（形如「临时」），数量单独一个弱化的小标签。
    void setTitle(const QString &boxName);
    void setCountText(const QString &text);

    // 锁图标的外观（开/关）。
    //
    // 由浮窗在 setLocked 时调用，而不是让标题栏自己去读配置 ——
    // 标题栏是个纯展示控件，让它碰配置会让"状态从哪来"多出一条路径。
    void setLockedLook(bool locked);

    // 应用全局主题颜色。标题栏是独立控件，但颜色统一由浮窗下发，
    // 避免标题栏自己读取配置导致两条状态来源。
    void setTheme(const AppTheme &theme);

    // 当前是否钉住。浮窗需要它来同步右键菜单的勾选态。
    bool isLockedLook() const { return m_lockedLook; }

    // 当前是否正被拖动。
    //
    // 悬停自动展开需要问这一句：拖动时鼠标"在窗口上"是假象 ——
    // 人其实是想把它搬走，此时展开会让窗口在手指底下变形。
    // 而且拖动过程中鼠标很容易甩出窗口边界，那会误触发 leave → 自动卷起，
    // 表现为"拖到一半窗口自己缩成一条线"。
    //
    // 做成公开访问器而不是发信号：浮窗需要的是"进 leave 的那一刻它是不是真的在拖"
    // 这个**瞬时状态**，而不是"拖动开始过"这个事件。
    // 用查询比用信号少一份需要自己维护同步的标志。
    bool isDragging() const { return m_dragging; }

signals:
    void doubleClicked();            // 双击标题栏 = 卷起/展开
    void closeClicked();
    void rollUpClicked();

    // 主人点了锁图标。请求切换钉住状态。
    //
    // 发信号而不是自己改状态：真正的开关要写配置、还要通知 manager
    //（钉住与否影响"谁推谁"），这些都超出标题栏的职责。
    void lockClicked();

    // 有东西被拖到标题栏上方（还没放下）。
    //
    // 为什么标题栏要管这件事：**卷起时只有标题栏是可见的**，列表、操作条、
    // 手柄行全部被隐藏了。而从桌面拖文件过来时，Windows 走的是拖放事件流，
    // **根本不会产生 enterEvent** —— 浮窗收不到、列表也收不到（它是隐藏的），
    // 只有光标底下这个标题栏能收到 dragEnterEvent。
    //
    // 不接这个信号的后果：主人从桌面拖一个文件想放进卷起的浮窗，
    // 拖到跟前发现它死活不展开，也就没地方可放 —— 而"拖进来"正是
    // 这个浮窗最主要的用途之一。
    void dragHovered();
    void dragLeft();

    // 拖动窗口开始 / 结束。
    //
    // 与 isDragging() 那条查询并存的理由：悬停光影需要在**拖动一开始**就退出
    //（拖动期间鼠标很容易被甩出窗口边界，靠 enter/leave 记账会留下卡住的光晕），
    // 而这件事需要一个瞬时事件。isDragging() 只能被反过来问，问不出"刚开始"。
    // 结束时浮窗再按"鼠标是否还在窗口里"决定要不要恢复光影。
    void dragStarted();
    void dragFinished();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

    // 接住"从桌面拖过来"的拖放流，用来触发悬停展开。
    //
    // 只读不写：这里**不改**任何拖放语义（接不接受、放不放得下），
    // 那些由内嵌的 ItemListWidget 按原来的规则决定。
    // 标题栏只是借这个事件流当"鼠标来了"的替身信号。
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    QLabel      *m_title = nullptr;
    QLabel      *m_count = nullptr;
    QPushButton *m_lockBtn   = nullptr;
    QPushButton *m_rollUpBtn = nullptr;
    QPushButton *m_closeBtn  = nullptr;

    // 锁图标的当前外观。
    bool m_lockedLook = false;

    AppTheme m_theme;

    // 拖动窗口用：记录"按下时鼠标相对窗口左上角的偏移"，
    // 移动时用「全局鼠标位置 - 偏移」算出新窗口位置。
    // 这样拖动时窗口不会跳到鼠标正下方，手感才对。
    QPoint m_dragOffset;
    bool   m_dragging = false;
};

class FloatingBoxWidget : public QWidget
{
    Q_OBJECT

public:
    // service 由 main.cpp 拥有，生命周期长于本浮窗；boxName/boxPath 为本浮窗
    // 代表的那个盒。不接受 nullptr service —— 少了它浮窗没有任何数据来源。
    FloatingBoxWidget(AppService *service,
                      const QString &boxName,
                      const QString &boxPath,
                      QWidget *parent = nullptr);

    QString boxName() const { return m_boxName; }
    QString boxPath() const { return m_boxPath; }

    // ---- 悬停自动展开的延迟常量 ----

    // 鼠标停在浮窗上多久才展开。
    //
    // 250ms 是"主人确实想把鼠标停在这儿"与"只是扫过去"之间的分界：
    // 再短（<150ms）的话，鼠标从屏幕一角划到另一角会一路把碰到的浮窗全炸开；
    // 再长（>400ms）就有"它怎么不理我"的迟滞感了。
    static constexpr int kHoverExpandDelayMs = 250;

    // 鼠标离开浮窗后多久才卷起。
    //
    // 刻意比进入延迟长（400 vs 250）：鼠标从列表区域移向标题栏上的按钮时，
    // 会短暂掠过窗口边缘，中途可能触发一次 leave。若两个延迟一样长，
    // 这点时间差不够人走回来，表现为"想去点关闭，窗口先自己卷了"。
    // 进入快、离开慢，是这类"自动让路"交互的通用配方。
    static constexpr int kHoverCollapseDelayMs = 400;

    // 悬停自动展开总开关（由 FloatingBoxManager 在全局设置变化时同步进来，
    // 也在构造时由 manager 显式设一次）。
    //
    // 浮窗自己不去读 Settings：与 applyAppearance 同理 ——
    // 两个入口（浮窗右键菜单、控制中心）各读一次，必然有一处漏发通知，
    // 表现是"从一个入口关了，另一个入口的窗口还开着"。
    void setHoverExpandEnabled(bool on);
    bool isHoverExpandEnabled() const { return m_hoverExpandEnabled; }

    // 重新扫描盒子目录并填充列表。
    // 列表数据一律现扫磁盘（BoxManager::listBoxItems）而不是缓存在内存里：
    // 主人的盒目录随时可能在资源管理器里被改动，缓存必然失真。
    void refreshItems();

    // 恢复上次保存的几何（QWidget::saveGeometry 的 blob）。
    // blob 为空表示首次打开：用默认尺寸并放在屏幕上一个合理位置。
    void applySavedGeometry(const QByteArray &blob);

    // 当前几何的 blob，供 FloatingBoxManager 落盘。
    QByteArray currentGeometryBlob() const;

    // 撤稿按钮随全局撤销栈状态更新（由 manager 在 undoStateChanged 时调用）。
    void updateUndoButton();

    // 应用一套外观设置（透明度 + 视图模式 + 必要时调整尺寸）。
    //
    // 由 FloatingBoxManager 在"外观被改"与"浮窗刚创建"两个时点调用 ——
    // 浮窗自己不去读配置：那会让"什么时候该重读"散落多处，
    // 且两个入口（浮窗右键、主窗口设置）改完后的通知路径会不一致。
    void applyAppearance(const BoxAppearance &appearance);

    // 当前生效的外观。右键菜单据此打勾、并作为"改一项"的基准值。
    BoxAppearance appearance() const { return m_appearance; }

    // 应用全局主题颜色。中控主题只控制全局颜色；单盒外观覆盖仍由
    // applyAppearance 管理，两者互不夺权。
    void setTheme(const AppTheme &theme);

    // ---- 总在最前 ----

    // 应用（或取消）窗口置顶。会重建原生窗口，因此内部要恢复
    // 不透明度与圆角遮罩 —— 见实现处的注释。
    //
    // 之所以是公开的：右键菜单会调它，探针也要能直接驱动这条路径
    // （它是"切换置顶"这件事的唯一边界，不该只有菜单能碰）。
    void applyAlwaysOnTop(bool on);

    // 当前是否置顶。与配置分开读：配置是"主人想要什么"，
    // 这个是"窗口现在实际是什么"，两者在置顶切换的瞬间可能不一致。
    bool isAlwaysOnTop() const;

    // ---- 动画 ----

    // 为淡入做准备：把窗口不透明度先置 0。
    //
    // ⚠️ 必须在 show() **之前**调用，与 fadeIn 配对使用：
    //   准备（置 0）-> show() -> fadeIn（从 0 动到终值）
    //
    // 为什么拆成两步而不是让 fadeIn 自己置 0：fadeIn 必须在 show 之后才能
    // 起动画（对未显示的窗口设不透明度无效），但"置 0"必须在 show **之前** ——
    // 否则窗口会先以终值显示一帧，再跳回 0 重新淡入，看起来是"闪一下才开始"。
    // 两件事的时机要求相反，所以只能拆开。
    //
    // skip=true（启动批量恢复）时不做任何事：那种情况不需要淡入，
    // 窗口直接以终值出现。
    void prepareFadeIn(bool skip = false);

    // 淡入。窗口 show() 之后调用，与 prepareFadeIn 配对。
    //
    // 终值取的是**当前外观的透明度**而不是 1.0：浮窗可能配置成 70% 半透明，
    // 若淡入到 1.0，动画结束的瞬间就会被 applyAppearance 拽回 0.7，
    // 主人会看到一次突兀的跳变。
    void fadeIn(bool skip = false);

    // 淡出后再请求关闭。
    //
    // 为什么不直接 emit closeRequested：manager 收到信号就 hide + deleteLater，
    // 浮窗会瞬间消失，淡出根本没有机会播放。
    // 所以改成"先播动画，动画结束时再报告"，把时序拉长到动画那么久。
    //
    // 防重入：淡出过程中再点一次关闭不会启动第二个动画、也不会发两次信号。
    void fadeOutThenClose();

    // 界面动画总开关变化时由 FloatingBoxManager 通知。
    void setAnimationsEnabled(bool on);

    // ---- 浮窗之间互相让位（由 FloatingBoxManager 驱动）----
    //
    // 【为什么位移的计算在 manager 而不在这里】
    // 算"谁该往下挪多少"必须看到**全部**浮窗，而单个浮窗看不到别人
    //（它不持有 manager 指针 —— 那会形成循环引用：manager 拥有浮窗）。
    // 所以本类只负责"被要求滑动到某个位置"，几何计算全在 manager。
    // 这与 WindowLayout 的分工一致：那边是纯几何，这边是纯执行。
    //
    // 传 dy 而不是绝对坐标：浮窗自己知道"我被推开之前的原位在哪"
    //（m_restPos），由它自己算目标位置，比让 manager 替它推坐标更不容易错 ——
    // manager 若自己记一份各窗口的原位，就和浮窗里的那份重复了，
    // 两份状态迟早会不一致。
    //
    // ⚠️ dy 是**增量**，不是相对原位的绝对偏移。它的来源是
    // WindowLayout::computePushDown，而那个函数算的是"相对它拿到的矩形
    //（= 窗口当前稳定位置）还需要往下挪多少"。
    //
    // 早先这里直接写成"目标 = 原位 + dy"，只在"这个窗口第一次被推"时
    // 巧合正确 —— 一旦它先被 A 推开一段、又被 B 二次推开，
    // 第二次的 dy 是相对"已推开后的位置"算出来的，却当成相对原位用，
    // 于是目标位置反而往回缩，与 B 叠在一起（三浮窗 / 悬停快速扫过时常见）。
    // 现在内部维护累计偏移 m_layoutOffset，把每次 dy 加上去，
    // 累加值与 computePushDown 的语义才对得上。
    //
    // dy == 0 表示"滑回原位"（卷起时用），同时把累计偏移清零。
    void slideByForLayout(int dy);

    // 当前是否正处于"被推开"的状态（有基线）。
    //
    // manager 用它判断"这一轮还有没有需要收回的窗口"。
    bool isPushedAside() const { return m_hasRestPos; }

    // 本浮窗**展开后**会占据的矩形（顶层窗口坐标）。
    //
    // 存在的理由：manager 需要在浮窗"刚开始展开"的那一刻就算出它展开后
    // 会挡住谁，但那一刻高度**动画**才刚起步，frameGeometry() 还是卷起时
    // 那条 30px 的细线 —— 拿它去算，结果是"展开不会推开任何人"，
    // 而且不报错，只是功能静默失效。
    //
    // 所以这里返回动画的**终值**尺寸而不是当前尺寸：
    // 正在做高度动画就返回动画的 endValue，否则返回当前几何。
    QRect expandedGeometry() const;

    // 本浮窗在"让位动画全部落定之后"会处的矩形（顶层窗口坐标）。
    //
    // 与 expandedGeometry 的分工：
    //   * expandedGeometry 关心**高度**（用高度动画终值做推挤计算）；
    //   * 这里关心**位置**（用位置动画终值做推挤计算）。
    //
    // 为什么不能直接用 frameGeometry()：连续快速触发让位（悬停展开时鼠标
    // 扫过多个浮窗）会让上一轮位移动画还没跑完、下一轮计算就来了。那时
    // frameGeometry() 只是动画的中间帧，拿它算出来的位移会偏，而偏的方向
    // 恰好让窗口叠在一起。改用终值算，两轮计算才落在同一个坐标系里。
    QRect layoutStableGeometry() const;

    // 当前是否处于卷起（只留标题栏）状态。
    //
    // manager 用它决定"要不要为这个浮窗算让位"：卷起的浮窗只占一条细线，
    // 挡不住谁，不该触发推开。
    bool isRolledUp() const { return m_rolledUp; }

    // ---- 钉住（锁定）----
    //
    // 钉住 = "这个窗口别动我"。三件事一起生效：
    //   * 不参与浮窗间让位（不被推开，也不推开别人）；
    //     ——"谁推谁不推"由 manager 在组装 others 列表时决定，
    //       本类没有全局视野，也不该有；
    //   * **悬停自动展开与自动卷起整个停用**（见 hoverInteractionBlocked）；
    //   * 压到最底层（applyAlwaysOnTop(false)），别人可以盖住它。
    //
    // ⚠️ 钉住**不**影响手动操作：双击标题栏、右键菜单、标题栏按钮照常可用。
    // 锁的是"自动"，不是"主人自己"—— 否则他就没法在不解锁的情况下
    // 看一眼盒里有什么了，那不叫保护，叫添乱。
    //
    // 上锁那一刻会停掉两个悬停定时器并清掉 m_hoverExpanded，
    // 于是窗口**停在当下这个样子**（展开的保持展开、卷起的保持卷起），
    // 而不是解锁后又自己收回去。详见 setLocked 的注释。
    //
    // 由 manager 在创建时与变更时同步（浮窗自己不读配置，与 applyAppearance 同理）。
    void setLocked(bool locked);
    bool isLocked() const { return m_locked; }

signals:
    // 主人点了关闭按钮。浮窗自己不做销毁决策，交给 manager。
    void closeRequested(const QString &boxName);

    // 几何变化（已去抖），需要落盘。
    void geometryChanged(const QString &boxName, const QByteArray &blob);

    // 请求在控制中心里选中本盒（右键菜单用，由 manager 转给主窗口）。
    void revealInControlCenterRequested(const QString &boxName);

    // 请求打开本盒的外观设置对话框（右键菜单的「更多设置…」）。
    // 本阶段只发信号 —— 对话框是另一个阶段的事，浮窗不需要知道它长什么样。
    void openAppearanceDialogRequested(const QString &boxName);

    // 主人从右键菜单改了一项外观，请求写回配置。
    //
    // 浮窗**不自己写配置**：外观有两个入口（浮窗右键、主窗口设置），
    // 若两边各自写，必然有一处漏发广播信号，表现是"从一个入口改完，
    // 另一个入口还显示旧值"。统一交给 FloatingBoxManager。
    void appearanceChangeRequested(const QString &boxName,
                                   const BoxAppearance &appearance);

    // 主人从右键菜单改了「悬停自动展开」这一项（勾选态）。
    //
    // 与 appearanceChangeRequested 同一个路数：浮窗**不自己写配置**，
    // 交给 FloatingBoxManager 写 + 广播。悬停自动展开有两个入口
    //（浮窗右键菜单、控制中心），各写各的必然有一处漏发通知，
    // 表现是"从一个入口关了，另一个入口的勾还打着、别的浮窗也还在自动展开"。
    void hoverExpandChangeRequested(bool enabled);

    // 主人点了标题栏上的锁图标，请求写回配置。
    //
    // 与 appearanceChangeRequested 同一个路数：浮窗**不自己写配置**。
    // 而且钉住还会改变"谁推谁"，那是 manager 才有资格决定的事 ——
    // 浮窗自己写配置的话，manager 那侧的状态就永远慢一拍。
    void lockChangeRequested(const QString &boxName, bool locked);

    // 本浮窗的卷起/展开状态刚刚变了（rolledUp 为真表示刚卷起）。
    //
    // FloatingBoxManager 据此重算"让位" —— 展开时要推开下方的，
    // 卷起时要把之前被推开的收回去。
    //
    // 为什么用信号而不是让 manager 去轮询或包在 onToggleRollUp 外面：
    // 触发卷起的路径有**四条**（双击标题栏、点「—」按钮、右键菜单、
    // 悬停自动展开/收起）。在每条路径外面各调一次 relayout 必然漏掉一条，
    // 而漏掉的表现是"某种方式展开时不会推开下面的浮窗" —— 很难想到去查。
    // 发在 applyRollUpState 里是唯一能保证全覆盖的位置。
    void rollUpStateChanged(const QString &boxName, bool rolledUp);

protected:
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

    // 只为一件事存在：盯住右下角 QSizeGrip 的按下/抬起。
    //
    // QSizeGrip 把鼠标事件全自己吞了、也没有 pressed/released 信号，
    // 想拿这个状态只能装过滤器。见 buildUi() 里的说明。
    bool eventFilter(QObject *watched, QEvent *event) override;

    // 悬停自动展开的两个入口事件。
    //
    // 用整个浮窗的 enter/leave 而不是只挂标题栏：规格明确要求"鼠标放在
    // 浮窗栏位上就展开"，而主人想看清盒里有什么时，鼠标往往已经在列表上了。
    //
    // ⚠️ Qt6 的进入事件签名是 enterEvent(QEnterEvent *)，
    // 不再是 Qt5 的 enterEvent(QEvent *)。写错不会编译报错（那是另一个重载），
    // 只会静默地永远不被调用 —— 表现是"悬停完全没反应"。
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private slots:
    void onCollectDesktop();
    void onUndoClicked();
    void onToggleRollUp();
    void onOpenRequested(const QString &path);
    void onDragOutFinished(const QString &path, bool sourceStillExists);
    void onFilesDropped(const QStringList &paths);
    void onGeometryDebounceTimeout();

    // 悬停延迟到点：展开 / 卷起。
    void onHoverExpandTimeout();
    void onHoverCollapseTimeout();

private:
    void buildUi();

    // 按 m_theme 重新套用所有颜色相关样式。尺寸、透明度和动画状态不变。
    void applyThemeToUi();
    void connectServiceSignals();

    // 卷起的实际执行。rolledUp 为真时隐藏操作条与列表、把高度收到只剩标题栏。
    void applyRollUpState(bool rolledUp);
    void persistRolledUp(bool rolledUp);

    // ---- 悬停自动展开 ----

    // 现在是否可以自动展开。把所有"不该自动展开"的情形收在一处判断。
    //
    // 为什么集中判断而不是在每个触发点各写一遍：这些条件（开关关了、
    // 主人手动卷起过、已经是展开态、动画在跑、正在拖动/拉大小/菜单弹着）
    // 会在 enter 与定时器回调**两处**都要问，分散写必然漂移，
    // 而症状是"偶尔会莫名其妙展开一下"，极难复现。
    bool canAutoExpand() const;

    // 现在是否应该把浮窗收起来（离开定时器到点后调用）。
    bool shouldAutoCollapse() const;

    // 停掉两个悬停定时器。进 enter 时要用它停掉离开定时器 ——
    // 否则刚回到窗口上就被上一次离开的定时器卷了回去，是经典的闪烁 bug。
    void stopHoverTimers();

    // 排除"鼠标暂时不在窗口上，但人并没有离开"的那些时刻。
    //
    // 六种情形：拖动窗口、拉尺寸手柄、右键菜单 exec 中、模态对话框 exec 中、
    // 文件拖出的 QDrag::exec() 中、高度动画进行中。
    // 每一种的具体理由都写在实现处 —— 它们是本步最容易出错的地方。
    bool hoverInteractionBlocked() const;

    // 一键收纳/拖入共用的执行路径：都走 collectInto。
    void collectEntries(const QList<DesktopEntry> &entries);

    // 本浮窗是否就是当前撤销栈里那一批的盒。
    bool undoBelongsToThisBox() const;

    // 几何去抖：每次调用都重开定时器，实现"最后一次变化后 500ms 才落盘"。
    void scheduleGeometrySave();

    // ---- 让位动画 ----

    // 平滑滑动到 target（顶层窗口坐标）。
    //
    // 动画期间会把 m_layoutAdjusting 置真，**覆盖整个区间**（起止成对，
    // 靠动画的 stateChanged）—— 详见 moveEvent 里的说明：
    // 只在 move() 那一行前后包一下是不够的，因为动画每一帧都会触发 moveEvent。
    void animatePosTo(const QPoint &target);

    // 不走动画直接跳到位。动画被关掉时、以及构造/恢复几何时用它。
    void setPosImmediately(const QPoint &target);

    // 按当前外观算"装得下吗"，装不下就放大窗口。
    //
    // ⚠️ 只在需要**变大**时才动尺寸：主人可能特意拉大过窗口，
    // 自动缩小会抹掉他手动调的尺寸。代价是切回小图标后右侧有留白 ——
    // 这是刻意的取舍（见设计方案 §4.5）。
    void resizeForAppearance();

    // 当前外观（图标模式）需要的**高度**是多少。列表模式返回 0（不需要额外空间）。
    //
    // 抽成纯计算是为了让 applyRollUpState 的展开分支能在**启动动画之前**
    // 就知道最终该到多高 —— 否则动画跑到一半 resizeForAppearance 又来改高度，
    // 两者会打架。
    int heightNeededForAppearance() const;

    // 按当前尺寸重算圆角遮罩。
    //
    // 为什么需要 mask 而不是只靠样式表的 border-radius：
    // 子控件的 border-radius 只影响**它自己背景的绘制范围**，
    // 而子控件是独立绘制在父窗口之上的 —— 父窗口的圆角管不到它们。
    // 结果就是：标题栏自带的顶部圆角能生效（它自己画成圆角），
    // 但底部的列表/手柄行会把方角画到父窗口的圆角外面，
    // 表现为"上面两个角是圆的、下面两个角是方的"。
    //
    // mask 是窗口级的裁剪，一刀切掉四个角，与子控件怎么画无关。
    // 代价：mask 是位图裁剪，**尺寸一变就必须重算**（见 resizeEvent），
    // 而且边缘是硬切、没有抗锯齿 —— 用 QPainter 的抗锯齿路径画 mask 可以
    // 缓解，8px 半径下肉眼基本看不出锯齿。
    //
    // ⚠️ 这一句是"精确重建"：它一律按当前窗口尺寸重画并调 setMask()，
    // 不做任何复用判断。理由见实现里的说明（applyAlwaysOnTop 重建过原生窗口）。
    void updateRoundedMask();

    // 按给定尺寸生成圆角遮罩并下发。内部会刷新下面的缓存。
    void applyRoundedMask(int w, int h);

    // 高度动画期间用的"够大就行"遮罩：保证遮罩**不小于**窗口即可，
    // 已经覆盖时一个字节都不重算。缩小时完全复用旧遮罩，
    // 把每帧一次 setMask()（原生窗口区域调用，离屏实测约 800us/次）省掉。
    //
    // 安全性：原生窗口区域 = 遮罩 ∩ 窗口矩形，所以"遮罩比窗口大"是无害的；
    // 反过来（遮罩比窗口小）会裁掉真实内容，这个函数绝不允许出现那种情况。
    void ensureRoundedMaskCovers(int minHeight);

    // 动画落定后把遮罩精确复位到当前尺寸。已经精确匹配时不做任何事。
    void settleRoundedMask();

    // 右键菜单里的「外观」子菜单。
    // 抽出来是因为 contextMenuEvent 已经很长，而这一段自成一块。
    void buildAppearanceMenu(QMenu *parentMenu);

    // ---- 悬停光影触感 ----

    // 把当前外观里的触感四项（开关 / 强度 / 速度 / 圆角）同步到覆盖层。
    // 由 applyAppearance 调用；覆盖层自己不读配置。
    void syncHoverOverlay();

    // 按"鼠标是否真的落在窗口里 + 此刻是否处于交互中"重新决定光晕开关。
    //
    // 光影的进出**不走 canAutoExpand / shouldAutoCollapse**：那两个是
    // "自动展开"的判定（关掉自动展开后它们一律为假），而触感是独立的视觉反馈，
    // 即使全局关掉自动展开也应当照常亮。所以单独一份判定。
    //
    // 用几何包含关系而不是 underMouse()：拖动 / 菜单 / 尺寸调整期间，
    // Qt 内部的 enter/leave 记账恰恰是不可信的时候（与 shouldAutoCollapse 同源）。
    void refreshHoverGlow();

    // 光影层铺满客户区并抬到最上层。缩放窗口时调用。
    void layoutHoverOverlay();

    // 会临时打断光影的交互：拖窗口 / 右键菜单 / 拉尺寸 / 拖文件出去。
    // 这几条与 hoverInteractionBlocked 里的对应项同源，但**不含**"被钉住"
    // 与"悬停自动展开开关"—— 那两条是"别自动动窗口"，不该把视觉反馈也关掉。
    bool hoverGlowBlocked() const;

    // 把 m_appearance.cornerRadius 同步到标题栏 / 手柄行的样式与窗口遮罩。
    // 只有在圆角真的变了、或窗口尺寸/原生窗口重建时才调 setMask()，
    // 悬停期间绝不调用。
    void applyCornerRadiusToUi();

    // ---- 透明度动画 ----

    // 平滑地把窗口透明度动画到 target（0.0–1.0）。
    //
    // **所有改透明度的地方都必须走它**，不要再直接调 setWindowOpacity。
    // 原因：淡入动画进行到一半时若有人直接 setWindowOpacity，
    // 那一句会把动画打到一半的值硬设回去 —— 动画还以为是自己在控制，
    // 结果就是"透明度闪一下才到位"。
    //
    // durationMs < 0 表示用默认时长（kFadeDurationMs）。
    // 动画被关掉时内部自动退化为 setOpacityImmediately。
    void animateOpacityTo(double target, int durationMs = -1);

    // 不走动画，直接设值。
    // 用于两处：构造/首次显示时的初始化，以及动画被主人关掉的情况。
    void setOpacityImmediately(double target);

    // 当前外观对应的目标透明度（已按 kMinOpacity 夹紧）。
    // 淡入的终值与"改外观时的终值"都取它 —— 两处共用一份，
    // 免得日后改了夹紧规则却漏改一处，表现是"淡入结束时闪一下"。
    double targetOpacityFromAppearance() const;

    // ---- 高度动画（卷起 / 展开）----

    // 平滑地把窗口高度动画到 targetHeight。
    //
    // ⚠️ 做这件事有两个必须处理的坑，都在实现里：
    //
    // 1) **min/max 高度锁会把动画顶掉**。
    //    applyRollUpState 的卷起分支设了 setMinimumHeight == setMaximumHeight，
    //    一旦 min == max，resize() 只能得到那一个值 —— 动画每帧都会被顶回 30，
    //    表现是"完全没效果"，而且不报错。
    //    实测见 tools/heightlock_diag.cpp。
    //    所以动画开始前必须 setMaximumHeight(QWIDGETSIZE_MAX) 解锁。
    //
    // 2) **resizeEvent 会在动画每一帧污染 m_expandedHeight**。
    //    onToggleRollUp 是先改 m_rolledUp 再调 applyRollUpState，所以展开
    //    动画期间 m_rolledUp 已经是 false，"记住展开高度"那条守卫拦不住它 ——
    //    从 30 涨到 400 的过程中，m_expandedHeight 会被依次写成 137、268……
    //    动画一旦被打断（切外观、拖窗口、用户又双击），展开尺寸就永久停在中间值。
    //    所以动画期间必须把 m_rollAnimating 置真，让 resizeEvent 跳过记录。
    void animateHeightTo(int targetHeight);

    // 不走动画，直接设高度。动画关闭时、构造期、恢复几何时用它。
    void setHeightImmediately(int targetHeight);

private:
    AppService *m_service = nullptr;    // 由 main.cpp 注入，不归浮窗所有
    QString     m_boxName;
    QString     m_boxPath;

    // 当前生效的外观。默认值即"改造前行为"（不透明 + 列表）。
    BoxAppearance m_appearance;

    // 当前全局主题颜色。只由 manager 下发。
    AppTheme m_theme;

    FloatingBoxTitleBar *m_titleBar   = nullptr;

    // 悬停光影覆盖层。不在布局里 —— 它必须铺满**整个客户区**（含标题栏），
    // 而任何布局容器都会被拆成若干行，画不出完整的一圈描边。
    // 由 layoutHoverOverlay() 手动定位、resizeEvent 里跟随。
    FloatingHoverOverlay *m_hoverOverlay = nullptr;

    QWidget             *m_actionBar  = nullptr;    // 「收纳桌面」「撤销」那一行
    ItemListWidget      *m_itemList   = nullptr;
    QWidget             *m_gripRow    = nullptr;    // 右下角尺寸手柄所在的容器行
    QSizeGrip           *m_sizeGrip   = nullptr;
    QPushButton         *m_collectBtn = nullptr;
    QPushButton         *m_undoBtn    = nullptr;

    bool m_rolledUp       = false;
    int  m_expandedHeight = 0;      // 卷起前的高度，展开时恢复用

    // 卷起/展开的高度动画是否正在跑。
    //
    // ⚠️ 这个标志的存在理由（本轮加动画时发现的真陷阱）：
    // resizeEvent 里有一段"记住展开高度"的逻辑，守卫条件是
    //     if (!m_rolledUp && height() > rolledUpHeight()) m_expandedHeight = height();
    // 而 onToggleRollUp 是**先改 m_rolledUp、再调 applyRollUpState**。
    // 于是展开动画期间 m_rolledUp 已经是 false，动画的**每一帧**都会把
    // 当前中间高度写进 m_expandedHeight —— 从 30 涨到 400 的过程中，
    // 它会被依次写成 137、268、399……
    // 一旦动画因任何原因中断（切外观、拖窗口、用户又双击），
    // m_expandedHeight 就永久停在那个中间值，下次展开只有半截高。
    // 所以 resizeEvent 的记录逻辑必须把"正在做高度动画"排除掉。
    bool m_rollAnimating = false;

    // 是否正在为"避让其他浮窗"而被动移动位置。
    //
    // 用途：被推开的位移是**临时布局**，不该落盘 —— 否则下次启动浮窗位置全乱。
    // moveEvent 里会据此跳过 scheduleGeometrySave。
    //
    // ⚠️ 它必须覆盖**整个动画区间**，不能只在 move() 前后包一下：
    // 位置动画每一帧都会触发一次 moveEvent，只在两端置位的话，
    // 中间那些帧仍然会各自排一次落盘 —— 而去抖定时器是"最后一次之后 500ms"，
    // 动画时长只有 ~200ms，于是动画结束时的落盘用的正是被推开的位置。
    // 表现是"关掉程序再开，浮窗停在上次被别人推开的地方"，而且极难复现
    //（要恰好推开过一次、且期间没有别的落盘触发）。
    bool m_layoutAdjusting = false;

    // 位置（让位）动画是否正在跑。
    //
    // ⚠️ 为什么需要**两个**标志，而不是共用 m_layoutAdjusting：
    //
    // m_layoutAdjusting 有两个写入方 —— 本类自己的"不让 m_expandedHeight
    // 被写脏"（applyRollUpState 的展开分支会成对置位/清除），以及让位动画。
    // 展开分支在结尾无条件写假，而**同一个时刻让位动画很可能正在跑**
    //（展开与推开就是同一件事的两面）。于是那一下会把动画期间的守卫抹掉，
    // 中间几帧又各自排了落盘，最终落的正是被推开的位置 ——
    // 恰好把本步最想避免的那件事放了过去。
    //
    // 所以拆成两个：m_layoutAdjusting 表示"这一段代码不希望别人记录
    // 展开高度"，m_posAnimating 表示"位置动画正在跑、帧不要落盘"。
    // moveEvent 只看 m_posAnimating，两者互不干扰。
    bool m_posAnimating = false;

    // ---- 让位（被推开）的状态 ----

    // "被推开之前它在哪"。
    //
    // 被推时若还没有基线就先记下当前位置，再动到 restPos + dy；
    // 卷起时滑回这里，然后把基线清掉。
    //
    // **只存内存、不落盘**：它是会话内的临时布局，重启后浮窗各自回到
    // 主人真实摆放的位置（各自由 applySavedGeometry 恢复）。
    //
    // ⚠️ 有没有基线看 m_hasRestPos，**不要**去问 m_restPos 本身。
    // QPoint 没有 isValid()（只有 QRect/QSize 有），第一版就是照 QRect 的
    // 习惯写了 m_restPos.isValid()，编译直接报
    // "'const class QPoint' has no member named 'isValid'"。
    // 也不能拿某个哨兵坐标当"无效"（比如 (-1,-1)）—— 多显示器下负坐标
    // 是完全合法的真实位置，那样判会把副屏上的浮窗误判成"没有基线"。
    QPoint m_restPos;
    bool   m_hasRestPos = false;

    // 相对 m_restPos 的**累计**目标偏移（>= 0）。
    //
    // 每次被推开都加上新的增量，而不是被新值覆盖 —— 见 slideByForLayout
    // 的说明：computePushDown 给的 dy 是相对"当前稳定位置"的增量，
    // 覆盖式写入会在二次推动时把窗口拉回去。
    // 回到原位（slideByForLayout(0)）与清基线时一起归零。
    int    m_layoutOffset = 0;

    // 本次"滑回原位"结束时要不要清掉 m_restPos。
    //
    // 这个意图必须能被后来的"再次被推开"取消：若回家动画尚未结束又被推走，
    // 旧 bool 若留着，新推动画结束时会误清基线（见 slideByForLayout）。
    //
    // 为什么清基线要推迟到动画结束：见 slideByForLayout 里的说明 ——
    // 提前清会让"滑回途中又被推开"的场景把中途位置误记成原位，
    // 于是那个浮窗每被推一次就永久偏一点。
    //
    // 为什么用一个标志而不是每次 connect 一个 lambda：
    // `QPropertyAnimation::finished` 是**累积**连接的，挂 N 次就有 N 个回调，
    // 而滑回原位会被反复触发（连续收起、展开几个浮窗）。
    // 所以连接在构造函数里只挂一次，靠这个标志决定本次要不要动手。
    bool m_restClearPending = false;

    // 位置动画对象。与高度/透明度动画分开第三个实例，理由同前两者：
    // 它动的是不同的属性，而且会与高度动画**同时**跑
    //（展开时一边长高一边被推走），共用一个实例必然互相打断。
    QPropertyAnimation *m_posAnim = nullptr;

    // ---- 钉住 ----

    // 是否被主人钉住。见 Settings::floatLocked。
    // 由 manager 同步（浮窗不读配置）。
    bool m_locked = false;

    QTimer *m_geometryDebounce = nullptr;

    // ---- 悬停自动展开 ----

    // 进入定时器：鼠标停在浮窗上满 kHoverExpandDelayMs 后展开。
    // 离开定时器：鼠标离开浮窗满 kHoverCollapseDelayMs 后卷起。
    //
    // 两个都 singleShot：它们表达的是"某个时刻之后做一件事"，
    // 不是周期性任务。用 start() 重开会自然重置计时，正好符合"每次进入都重新计时"。
    QTimer *m_hoverExpandTimer   = nullptr;
    QTimer *m_hoverCollapseTimer = nullptr;

    // 当前这次展开是**悬停**造成的（而不是主人手动双击展开的）。
    //
    // ⚠️ 这个区分是本步的核心，不是可有可无的细节：
    // 离开时**只有它**会被自动收回。主人手动展开的窗口，鼠标一移开就自动卷起
    // 是很恼人的 —— 他明明是想仔细看看盒里有什么。
    // 反过来，悬停展开的窗口若不收回，鼠标扫过的每一个浮窗都会永久留在展开态，
    // 自动展开就成了"只会开不会关"的负担。
    bool m_hoverExpanded = false;

    // 悬停自动展开的总开关，由 manager 同步。
    //
    // ⚠️ 初值是 **false** 而不是 true，这一点和"配置默认开启"不矛盾：
    // 浮窗在**被 manager 配置之前**不该自作主张地自动展开 —— 构造期窗口
    // 还没 show，任何悬停动作都没有意义；而 openBox 紧接着就会用真实配置
    // 覆盖它。若初值给 true，setHoverExpandEnabled(false) 会因为
    // "值没变"而提前返回（见实现），配置里的"关闭"反而同步不进来。
    bool m_hoverExpandEnabled = false;

    // ---- 交互中标志 ----
    //
    // 这四个都表示"鼠标现在可能不在窗口上，但人并没有离开"。
    // 每一次的具体理由写在 hoverInteractionBlocked 里。
    //
    // 用独立的布尔而不是一个枚举/计数：它们的生命周期互不嵌套
    //（不会出现"菜单里又拖窗口"这种组合），一个 bool 一个语义最省事。
    bool m_contextMenuOpen = false;     // 右键菜单 exec() 期间
    bool m_modalDialogOpen = false;     // PreviewDialog / QMessageBox 等 exec() 期间
    bool m_dragOutActive   = false;     // QDrag::exec() 期间（系统级鼠标抓取）
    bool m_sizeGripActive  = false;     // 拉右下角尺寸手柄期间

    // 有东西从外面被拖到本浮窗上方（还没放下）。
    //
    // ⚠️ 这一条与上面四条**不同源**：上面四条都是"鼠标其实还在，只是收不到
    // 事件"；这一条是"鼠标带着一个文件悬在我头上"。它要挡的不是误判，
    // 而是"正在投放的过程中窗口自己缩起来"—— 那会把主人正要放下的
    // 东西弄丢（缩起后列表被隐藏，drop 就没有接收者了）。
    bool m_dragHoverActive  = false;

    // 最近一次真正下发的圆角平滑度。
    // 与半径分开缓存，便于只在实际改变外观时重建窗口遮罩。
    int m_appliedCornerSmoothing = -1;

    // 圆角变化时若鼠标正悬停，先记录待刷新，等触感退场后再调用 setMask()。
    bool m_maskRefreshPending = false;

    // 最近一次真正下发的圆角半径。
    // 用途：applyAppearance 会在切视图 / 改透明度时被反复调用，而圆角是
    // 那一堆项里**唯一**要重设窗口遮罩的。没有这个比较，每次改透明度都会
    // 白跑一次 setMask()（原生窗口区域调用，离屏实测约 800us/次）。
    int m_appliedCornerRadius = -1;

    // 最近一次下发给原生窗口的圆角遮罩与它的尺寸。
    //
    // 存的目的是回答"这张遮罩能不能直接复用"：高度动画期间窗口变矮时，
    // 旧遮罩（更大）依然完整覆盖窗口，可以原地留着、不必逐帧重设。
    // 见 ensureRoundedMaskCovers / settleRoundedMask。
    QBitmap m_maskBitmap;
    QSize   m_maskSize;

    // ---- 动画状态 ----

    // 透明度动画对象。**只建一次、反复复用**：
    // 每次 new 一个虽然靠 Qt 父子关系也能回收，但反复 new/delete 是白费，
    // 而浮窗会被频繁开关，动画对象是热路径上的东西。
    QPropertyAnimation *m_opacityAnim = nullptr;

    // 高度动画对象。与透明度动画分开两个实例 —— 它们动的是不同属性、
    // 也会同时跑（比如展开的同时正在淡入），共用一个会互相打断。
    // 同样只建一次、反复复用。
    QPropertyAnimation *m_heightAnim = nullptr;

    // 界面动画是否启用。构造时从 Settings 读；之后由 manager 在
    // "主窗口改了这个开关"时通过 setAnimationsEnabled 同步过来。
    bool m_animationsOn = true;

    // 淡出是否已经开始。防重入用：淡出过程中再点一次关闭，
    // 既不启动第二个动画，也不重复 emit closeRequested
    //（重复 emit 会让 manager 对着一个已经在销毁队列里的对象再走一遍关闭流程）。
    bool m_closing = false;
};

#endif // FLOATINGBOXWIDGET_H
