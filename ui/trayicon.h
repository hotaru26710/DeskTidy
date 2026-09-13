#ifndef TRAYICON_H
#define TRAYICON_H

// ---------------------------------------------------------------------------
// ui/trayicon.h —— 系统托盘图标：程序常驻期间唯一的"总入口"。
//
// 【为什么独立成类，而不塞进 MainWindow】
// 两个理由，第二个是硬性的：
//   1) MainWindow 已经近 600 行，再堆托盘菜单会很难读；
//   2) **托盘的生命周期与主窗口不是一回事**。本阶段的核心目标就是
//      "关掉主窗口不退出程序"，也就是说主窗口隐藏甚至之后被销毁时，
//      托盘必须还活着、还得能唤回窗口、还得能退出。把托盘挂成主窗口的
//      子对象会让这个关系变得含糊 —— 主窗口一析构，托盘跟着没了，
//      用户就再也找不到退出入口。
//
// 【为什么托盘在本程序里是"必须"而不是"可选"】
// main.cpp 里关掉了 setQuitOnLastWindowClosed，也就是"关掉所有窗口也不退出"。
// 这个决定只有配合托盘才成立：否则用户关掉主窗口后，程序会留在后台，
// 而任务栏、Alt+Tab 里都找不到它 —— 既占着资源又关不掉，是最坏的结果。
// 所以 isAvailable() 必须被调用方认真对待：
//   * 主窗口据此决定"关闭是真退出还是只隐藏"（见 MainWindow::closeEvent）；
//   * 拿不到托盘时，一切退回传统行为（关窗即退出），绝不硬撑常驻。
//
// 【本类管什么】
//   * 托盘图标、右键菜单、双击/单击唤回主窗口；
//   * 「浮窗」子菜单：列出所有收纳盒，勾选态 = 该盒浮窗是否开着；
//   * 「退出」：发出 quitRequested，由 main.cpp 执行真正的退出流程。
//
// 【本类不管什么】
//   * 不管浮窗的生死（那是 FloatingBoxManager 的事，本类只调它的开关接口）；
//   * 不管文件（本类不碰磁盘上的任何文件）；
//   * 不做退出决策（只发信号，退出顺序由 main.cpp 统一编排）。
// ---------------------------------------------------------------------------

#include <QObject>
#include <QString>
#include <QStringList>
#include <QSystemTrayIcon>   // 槽函数签名用到嵌套枚举 ActivationReason，
                         // 前置声明不够 —— 编译器必须看到完整类型定义

class AppService;
class MainWindow;
class FloatingBoxManager;

class QAction;
class QMenu;

class TrayIcon : public QObject
{
    Q_OBJECT

public:
    // service / window / floating 均由 main.cpp 拥有，生命周期长于本对象。
    // window 允许为 nullptr（理论上不会有，但构造期注入顺序可能让调用方
    // 先建托盘后设窗口），此时"打开控制中心"退化为空操作而不是崩溃。
    TrayIcon(AppService *service,
             MainWindow *window,
             FloatingBoxManager *floating,
             QObject *parent = nullptr);
    ~TrayIcon() override;

    // 系统是否真的提供托盘。**调用方必须据此决定关闭行为**：
    // 为假时"关主窗口只隐藏"会让程序变成关不掉的幽灵进程，
    // 此时应退回传统的"关窗即退出"。
    bool isAvailable() const;

    // 主窗口被隐藏时调用。内部依据 Settings::trayHintShown 决定是否真的提示，
    // 且**只提示一次** —— 每次都弹气泡会很快变成骚扰。
    //
    // 之所以要做这个提示：主人点了关闭，窗口消失了，但进程还在后台跑。
    // 不给任何交代的话，他会以为程序已经关了 —— 直到下次看任务管理器才发现。
    void showBackgroundHintOnce();

    // 浮窗开关状态变化后刷新菜单勾选态。
    // 连接 FloatingBoxManager::boxWindowToggled 用。
    void syncFloatingMenu();

signals:
    // 主人点了托盘菜单的「退出」。
    //
    // 本类只发信号、不自己退出：退出是一段有顺序的编排
    // （先把浮窗几何兜底落盘 -> 再关浮窗 -> 最后 quit），
    // 且顺序错了会丢数据或留下销毁到一半的对象。
    // 把它交给 main.cpp 统一编排，是为了让"退出流程"只有一个地方可读。
    void quitRequested();

private slots:
    void onActivated(QSystemTrayIcon::ActivationReason reason);

    // 菜单每次弹出前重建 —— 见实现里的理由（盒列表必须现扫）。
    void rebuildMenu();

    void onOpenControlCenter();
    void onQuit();

private:
    // 把主窗口恢复到前台：反最小化 -> 显示 -> 置顶一瞬 -> 激活。
    void bringWindowToFront();

    // 造一个"☑ 盒名"样式的可勾选菜单项。
    QAction *makeBoxAction(const QString &boxName, const QString &boxPath, bool open);

private:
    AppService         *m_service  = nullptr;   // 由 main.cpp 注入
    MainWindow         *m_window   = nullptr;   // 由 main.cpp 注入，不归本类所有
    FloatingBoxManager *m_floating = nullptr;   // 由 main.cpp 注入，不归本类所有

    QSystemTrayIcon *m_tray        = nullptr;
    QMenu           *m_menu        = nullptr;

    // 「浮窗」子菜单。**只在 rebuildMenu 内部有效** —— clear() 会销毁上一次
    // 造出来的子菜单，所以这个指针每次重建后都会变，绝不能跨调用缓存或判空。
    QMenu           *m_floatingMenu = nullptr;

    // 系统托盘是否可用。构造时探测一次即可 —— 运行中托盘服务状态变化
    // 属于极罕见情况，为它挂 QSystemTrayIcon 的信号会让逻辑复杂很多，
    // 收益却几乎为零。此处刻意不做动态跟随。
    bool m_available = false;
};

#endif // TRAYICON_H
