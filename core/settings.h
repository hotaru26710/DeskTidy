#ifndef SETTINGS_H
#define SETTINGS_H

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "coretypes.h"      // BoxAppearance 按值返回/传参，须完整类型

// ---------------------------------------------------------------------------
// Settings —— 全局配置的唯一入口（INI 落盘）。
//
// 存这些东西：
//   * 排除名称列表（手工指定"永不收纳"的名字）；
//   * 上次使用的盒名；
//   * 常驻浮窗相关：哪些盒开了浮窗、各浮窗的几何、卷起状态、是否置顶；
//   * 托盘提示是否已展示过。
//
// 为什么**盒名清单**不进来：盒子本质就是 %USERPROFILE%\DeskTidy\ 下的真实目录，
// 文件系统即真相。若在配置里再存一份盒名清单，就出现了两份真相，
// 主人在资源管理器里手工建/删一个盒子后配置立刻失真。
// 因此盒子一律靠扫描目录重建（见 BoxManager::listBoxes）。
// 浮窗那几项存的是"状态"（哪个盒开着浮窗、窗口多大）而不是"有哪些盒"，不冲突。
//
// 开机自启同样**不**进这份 INI：它是 Windows 注册表里的系统状态，由
// core/autostart.h 直接读写。再存一份 bool 会和任务管理器里的启用/禁用状态打架。
//
// 落盘位置（Windows 实测）：
//   %LOCALAPPDATA%\DeskTidy\DeskTidy\DeskTidy.ini
//   即 C:\Users\<用户>\AppData\Local\DeskTidy\DeskTidy\DeskTidy.ini
//
// ⚠️ 注意**不是** %APPDATA%（Roaming）。虽然常量名叫 AppConfigLocation，
// 但 Qt6 在 Windows 上把它映射到本地配置目录。本工具也不需要漫游语义：
// 配置里存的是浮窗位置这类与具体机器强相关的状态，跟着用户漫游反而添乱。
//
// 中间那层重复的 \DeskTidy\ 来自 organizationName + applicationName
// （main.cpp 里两者都设成了 "DeskTidy"）。
// ---------------------------------------------------------------------------

class Settings
{
public:
    Settings();

    // ---- 收纳过滤 ----

    // 永不收纳的名称清单（大小写不敏感匹配），如主人自己指定的"重要文件"。
    QStringList excludedNames() const;
    void        setExcludedNames(const QStringList &names);

    // ---- 界面状态 ----

    // 上次使用的盒名，用于下次打开时预选中，减少重复选择。
    QString lastBoxName() const;
    void    setLastBoxName(const QString &name);

    // ---- 常驻浮窗 ----

    // 哪些盒子当前开了浮窗。只存盒名集合 —— 浮窗内容本身仍靠扫描目录重建，
    // 避免落第二份真相。
    // 注意：盒目录可能已被主人删掉，读取方需自行对照 BoxManager::listBoxes()
    // 丢弃已消失的条目（对照逻辑不放在这里，Settings 不认识 BoxManager）。
    QStringList openBoxNames() const;
    void        setOpenBoxNames(const QStringList &names);

    // 单个浮窗的几何，存 QWidget::saveGeometry() 的 blob。
    //
    // 为什么用 blob 而不是 x/y/w/h：blob 里含**屏幕标识与 DPI 上下文**，
    // 多显示器拔插后 Qt 能把窗口自动挪回可见区；手写坐标没有这个信息，
    // 显示器一拔浮窗就跑到不存在的屏幕上，再也找不回来。
    // 代价是 ini 里这串东西人看不懂 —— 可接受。
    QByteArray floatGeometry(const QString &boxName) const;
    void       setFloatGeometry(const QString &boxName, const QByteArray &blob);

    // 浮窗是否处于卷起（只留标题栏）状态。
    bool floatRolledUp(const QString &boxName) const;
    void setFloatRolledUp(const QString &boxName, bool rolledUp);

    // ---- 浮窗外观（每盒一份）----
    //
    // 三项（视图模式 / 图标尺寸 / 透明度）打包在 BoxAppearance 里整体读写，
    // 不提供逐项访问器：散着传容易漏传一项，表现是"改了没反应"这种难查的问题。
    //
    // 读写都遵循"只存非默认值"的约定（同 floatRolledUp）：
    // 默认外观不落任何键，少一个键就少一份可能失真的状态。
    //
    // 读取时所有值都会被夹到合法范围（枚举越界退回 List、透明度夹进
    // [BoxAppearance::kMinOpacity, 100]）—— 配置文件是可以被手工编辑的，
    // 不能假设它一定合法。
    BoxAppearance floatAppearance(const QString &boxName) const;
    void          setFloatAppearance(const QString &boxName, const BoxAppearance &appearance);

    // 删掉某个盒的全部外观配置。
    // 删收纳盒时调用：不清的话，将来建一个同名盒会"继承"上一个盒的外观。
    void clearFloatAppearance(const QString &boxName);

    // ---- 浮窗「钉住」（每盒一份）----
    //
    // 钉住 = "这个窗口别动我"。三件事一起生效：
    //   1) 不参与浮窗间让位（别人推不动它，它展开时也不推别人）；
    //   2) **悬停自动展开与自动卷起整个停用** —— 鼠标移上去不弹开、
    //      移开也不收起，它就停在主人上锁时那个样子；
    //   3) 固定在最底层（别的浮窗可以盖住它）。
    //
    // 用途：主人可能有一个常驻在屏幕角落的浮窗，不希望它被别的浮窗挤走、
    // 也不希望鼠标扫过时它自己开开合合。这两类诉求都指向"把它钉死"，
    // 所以合成一个开关而不是拆成两个 —— 拆开的话主人要同时理解
    // "让位"和"悬停"两套概念，而他的心智模型里只有一个"锁"字。
    //
    // ⚠️ 钉住**不**影响手动操作：双击标题栏、右键菜单、标题栏按钮照常可用。
    // 锁的是"自动"，不是"主人自己"。
    //
    // 沿用 floatRolledUp 的"只存非默认值"约定：默认（没钉住）不落键。
    bool floatLocked(const QString &boxName) const;
    void setFloatLocked(const QString &boxName, bool locked);

    // 浮窗是否总在最前。做成可关，因为置顶会盖住全屏视频与游戏。
    bool alwaysOnTop() const;
    void setAlwaysOnTop(bool on);

    // "程序已进托盘"的提示是否展示过 —— 只提示一次，避免每次都烦主人。
    bool trayHintShown() const;
    void setTrayHintShown(bool shown);

    // ---- 界面动画 ----

    // 是否启用界面动画。**全局设置**（不像外观那样每盒一份），所有浮窗共用。
    //
    // 做成可关的理由有二：
    //   1) 有人就是不喜欢动画，觉得磨叽；也有人机器性能吃紧。
    //   2) 万一动画在某个平台/显卡驱动上引发显示异常，
    //      主人能一键关掉继续用，而不是只能卸载重装。
    //
    // 与每盒外观的"只存非默认值"约定**刻意不同**，这里默认值也落键：
    //   * 外观是每盒一份，一个个存默认值会随盒数量堆积成一片冗余；
    //     而本项是全局单键，不存在堆积问题。
    //   * "主人明确关掉过"这件事本身值得被记住并如实写下来，
    //     排查"为什么我这没有动画"时，看一眼 ini 就知道是配置还是代码问题。
    bool animationsEnabled() const;
    void setAnimationsEnabled(bool on);

    // ---- 悬停自动展开 ----

    // 鼠标在浮窗上停留一会儿就自动展开、移开就自动卷起。**全局设置**，默认开启。
    //
    // 与 animationsEnabled 归为同一类：全局单键、默认值也落键（理由同上）。
    //
    // 为什么默认是开：这是本工具"不用管它自己就会让路"这条主线体验的一部分 ——
    // 默认关闭等于这个功能不存在，没人会主动去菜单里找一个自己没见过的开关。
    //
    // 注意：打开本开关之后，**手动卷起的浮窗照样会被悬停展开**。
    // 曾经有过"手动卷起就屏蔽自动展开"的一层（m_manualRolledUp），
    // 但那会形成一个解不开的循环（手动卷起后再也没有触发自动展开的机会），
    // 已被主人实测推翻并删除。详见 FloatingBoxWidget::onToggleRollUp 的说明。
    //
    // 于是本开关是**全局唯一**的总闸：想给所有浮窗都不要自动展开，就关它。
    // 要给**单个**浮窗免掉自动展开，用 floatLocked（钉住）——
    // 见那里的说明，钉住同时还会关掉让位。
    bool hoverExpandEnabled() const;
    void setHoverExpandEnabled(bool on);

    // ---- 开机自启方式 ----

    // 开机自启是否采用静默方式。
    //
    // 这项只决定"下次开启自启时注册表里的命令带不带 --silent-autostart"；
    // 自启本身是否开启仍以 core/autostart.h 读写的注册表为准。
    // 关闭自启后也保留这项偏好，方便下次重新勾选时沿用上次选择。
    bool autoStartSilent() const;
    void setAutoStartSilent(bool silent);

private:
    // QSettings 不可拷贝，且每次读写都重新构造开销很低，故按需即时创建。
    // 声明为 mutable 以便 const 访问器内部构造。
    mutable QString m_iniPath;  // 已解析的 ini 全路径，避免重复解析
};

#endif // SETTINGS_H
