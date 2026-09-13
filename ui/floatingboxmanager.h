#ifndef FLOATINGBOXMANAGER_H
#define FLOATINGBOXMANAGER_H

// ---------------------------------------------------------------------------
// ui/floatingboxmanager.h —— 所有浮窗的生命周期与集合状态管理者。
//
// 【为什么需要它，而不是让主窗口自己 new 浮窗】
// 浮窗的拥有关系一旦散落在多个入口（主窗口右键菜单、托盘菜单、启动恢复），
// 就会出现"谁 new 的谁 delete"的纠纷：主窗口关掉时它的子浮窗跟着销毁，
// 但托盘可能还在引用；或者浮窗自己 delete 了自己，而某个哈希表里还留着野指针。
// 把"谁拥有浮窗"收敛到一个对象，这些纠纷就不存在了 ——
// 浮窗只发 closeRequested，销毁决策一律由本类做。
//
// 【本类管什么】
//   * 开 / 关某个盒的浮窗；
//   * 把开关状态与几何落进 Settings；
//   * 启动时按配置重建（盒目录已消失的条目直接丢弃）；
//   * 把 AppService 的刷新信号转发给相关浮窗；
//   * 退出前把几何兜底落盘。
//
// 【本类不管什么】
//   * 不搬文件（那是 Collector / AppService 的事）；
//   * 不创建或删除收纳盒目录（那是 BoxManager 的事）；
//   * 不做菜单（主窗口与托盘各自做，只通过信号通知状态变化）。
//
// 【生命周期】与 AppService 一样由 main.cpp 在栈上创建，且声明在 MainWindow
// 之前 —— 浮窗都是本类的成员，本类活得比主窗口久，主窗口关掉时浮窗照常存在
// （这是"常驻"的基础）。
// ---------------------------------------------------------------------------

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

#include "windowlayout.h"

// BoxAppearance 在信号与槽里按值传递，必须完整定义 ——
// 前置声明不足：Qt 的信号槽连接要在编译期知道参数类型的完整信息。
#include "coretypes.h"

class AppService;
class FloatingBoxWidget;

class FloatingBoxManager : public QObject
{
    Q_OBJECT

public:
    explicit FloatingBoxManager(AppService *service, QObject *parent = nullptr);
    ~FloatingBoxManager() override;

    // 启动时调用：读 Settings::openBoxNames()，为**仍存在**的盒重建浮窗。
    //
    // 盒目录已不存在的条目要丢弃：盒子本质是磁盘上的真实目录，
    // 配置里那份开关记录只是"状态"，不能当作"这个盒还在"的证据。
    // 若照配置无脑建浮窗，主人删掉一个盒之后会看到一堆指向空目录的浮窗。
    void restoreOpenBoxes();

    // 打开 / 关闭某个盒的浮窗。开已开的、关已关的都是安全空操作。
    // closeBox 会立即销毁浮窗对象（deleteLater），不留悬空指针。
    void openBox(const QString &boxName, const QString &boxPath);
    void closeBox(const QString &boxName);

    bool        isBoxOpen(const QString &boxName) const;
    QStringList openBoxNames() const;

    // ---- 钉住（每盒一份）----
    //
    // 写配置 + 通知对应浮窗。与 applyAppearance 同构：浮窗不自己读配置，
    // 因为"钉住"有两个入口（标题栏锁图标、右键菜单），各写各的必然有一处
    // 漏发通知，表现是"从一个入口锁了，另一个入口还显示没锁"。
    void setBoxLocked(const QString &boxName, bool locked);
    bool isBoxLocked(const QString &boxName) const;

    // ---- 浮窗之间的空间协调 ----
    //
    // 遍历所有已开浮窗，**按屏幕位置从上到下、从左到右排序**。
    //
    // 为什么必须排序而不是直接暴露 QHash：QHash 的遍历顺序是**不确定的**
    //（与插入顺序无关，还会随扩容变化）。而"推开下方浮窗"的逻辑需要按
    // 纵向次序处理，拿到一个每次都不一样的顺序会让结果不可复现 ——
    // 同一组浮窗，这次推 A 下次推 B，用户看到的是"位置在乱跳"。
    //
    // 排序键：先按所在屏幕（多显示器时不同屏幕的 y 不可比），
    // 再按 y，再按 x，最后按盒名兜底（保证完全同位置时也有确定顺序）。
    QList<FloatingBoxWidget *> widgetsInLayoutOrder() const;

    // ---- 外观（每盒一份）----
    //
    // 写配置 + 广播，收在这里而不是让两个入口（浮窗右键、主窗口设置）
    // 各自写配置：各写一份必然有一处漏发信号，表现是"从一个入口改完，
    // 另一个入口还显示旧值"这种最难查的不一致。
    void applyAppearance(const QString &boxName, const BoxAppearance &appearance);

    // 读某盒当前外观。盒子没有专门配置时返回默认外观（不透明 + 列表）。
    BoxAppearance appearanceOf(const QString &boxName) const;

    // ---- 界面动画总开关 ----

    // 写配置 + 通知所有已开浮窗。
    //
    // 收在这里而不是让主窗口自己遍历浮窗：主窗口**不持有**浮窗
    //（它们归本类所有，这正是本类存在的意义），它没有别的办法通知到它们。
    void setAnimationsEnabled(bool on);

    // ---- 悬停自动展开总开关 ----
    //
    // 写配置 + 通知所有已开浮窗。与 setAnimationsEnabled 完全同构：
    // 它是全局项，每个浮窗都要收到，直接遍历比"发广播 + 每个浮窗各自
    // 写一遍匹配判断"更直白。
    //
    // 新增浮窗时的同步在 openBox 里做（构造完立刻 setHoverExpandEnabled），
    // 否则新开的浮窗会一直用构造时的默认值，表现为"改了设置，
    // 新开的浮窗还是不听话"。
    void setHoverExpandEnabled(bool on);

    // 退出前把所有浮窗几何落盘。
    // 防的是"去抖定时器还没触发就被 quit"导致最后一次拖动位置丢失。
    void saveAllGeometry();

    // 关闭全部浮窗。退出流程用。
    // 注意：它**不**清空 Settings 里的 openBoxNames —— 退出时的关闭是
    // "程序结束了"，不是"主人不想再看到这些浮窗"，下次启动要恢复。
    void closeAll();

signals:
    // 某个盒的浮窗开关状态变了。主窗口右键菜单与托盘菜单据此打勾。
    void boxWindowToggled(const QString &boxName, bool open);

    // 请求主窗口把控制中心调到前面并选中该盒（浮窗右键菜单的"在控制中心中显示"）。
    void revealInControlCenterRequested(const QString &boxName);

    // 请求主窗口打开本盒的外观设置对话框（浮窗右键菜单的"更多设置…"）。
    void openAppearanceDialogRequested(const QString &boxName);

    // 某个盒的外观变了。浮窗据此重读并应用 ——
    // 两个入口改完都会经过这里，所以两边表现必然一致。
    void boxAppearanceChanged(const QString &boxName);

private slots:
    // AppService::boxContentsChanged 的处理。
    // 除了转发刷新，还兼做"盒目录被外部删掉"的检测：目录没了就销毁浮窗。
    void onBoxContentsChanged(const QString &boxPath);

private:
    // 把当前开着的浮窗名单写回配置。
    void persistOpenBoxes();

    // ---- 让位（推开下方浮窗）----

    // 某个浮窗的高度刚刚变了（展开或卷起），重算并应用让位。
    //
    // 只在"展开/卷起"这两个时机各算一次，不做实时跟随 ——
    // 理由见实现处的长注释（振荡 + 和手动拖动抢位置）。
    //
    // anchor：发起者。若它自己是被钉住的，则整件事不成立（它不推别人）。
    // expanded：它为真表示"展开"（要推人），为假表示"卷起"（要收回让位）。
    void relayoutAround(FloatingBoxWidget *anchor, bool expanded);

    // 收集"除 anchor 外、参与让位的浮窗"。
    //
    // 排除两种：被钉住的（规格：钉住的不参与推动）、以及不可见的
    //（关掉的浮窗不该占地方）。
    // ⚠️ "排除钉住的"这件事必须由调用方做，不能塞进
    // WindowLayout::computePushDown —— 它是 core 层的纯几何函数，
    // 不该知道 UI 上有个"锁"按钮。
    QList<WindowLayout::Item> layoutItemsExcept(FloatingBoxWidget *anchor) const;

    // 盒目录是否真实存在（用于丢弃失效条目与检测运行中被删）。
    static bool boxDirectoryExists(const QString &boxPath);

private:
    AppService *m_service = nullptr;    // 由 main.cpp 注入，不归本类所有

    // 盒名 -> 浮窗。盒名是唯一键：同一时刻一个盒最多一个浮窗。
    QHash<QString, FloatingBoxWidget *> m_widgets;

    // 是否正在批量恢复浮窗（restoreOpenBoxes 执行期间）。
    //
    // 用途：让 openBox 知道"这一次是启动恢复"从而跳过淡入。
    // 单独用一个成员而不是给 openBox 加参数：openBox 是公开接口，
    // 加一个只有内部一个调用点用得到的参数，会让所有其他调用方
    // 都要去理解这个跟自己无关的语义。
    bool m_batchRestoring = false;
};

#endif // FLOATINGBOXMANAGER_H
