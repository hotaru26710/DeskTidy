#include "trayicon.h"

#include "floatingboxmanager.h"
#include "mainwindow.h"

#include "appservice.h"
#include "boxmanager.h"
#include "corenames.h"
#include "settings.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QMenu>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QWidget>

// ---------------------------------------------------------------------------
// TrayIcon 实现。
//
// * 菜单**每次弹出都重建**，而不是建一次然后手工同步勾选态。
//   理由：收纳盒的真相在磁盘上 —— 主人随时可能在控制中心里新建一个盒，
//   甚至在资源管理器里手工建一个。缓存 menu 内容就必然要维护"什么时候失效"，
//   而重建一个只有几项的菜单开销可以忽略。这与项目里"文件系统即真相"的
//   一贯做法一致（见 BoxManager 的文件头说明）。
//
// * 图标用系统标准图标顶上，**刻意不引入 .ico 与 .qrc**：
//   当前项目没有任何资源文件，为一个托盘图标新开资源系统会牵动两份构建清单，
//   还要设计图形。等到确实要发布时再补一个正经图标更划算。
// ---------------------------------------------------------------------------

TrayIcon::TrayIcon(AppService *service,
                   MainWindow *window,
                   FloatingBoxManager *floating,
                   QObject *parent)
    : QObject(parent)
    , m_service(service)
    , m_window(window)
    , m_floating(floating)
{
    Q_ASSERT(m_service);
    Q_ASSERT(m_floating);

    // 探测托盘可用性。这个结果会经 isAvailable() 影响主窗口的关闭行为，
    // 所以必须在构造时就确定，不能等到第一次点关闭时才问。
    m_available = QSystemTrayIcon::isSystemTrayAvailable();

    if (!m_available) {
        // 关键分支：拿不到托盘就不要创建图标，也不要让它出现在界面上。
        // 调用方（MainWindow::closeEvent）会据此走"关窗即退出"的传统路径，
        // 避免出现"程序在后台却没有入口"的死局。
        return;
    }

    m_tray = new QSystemTrayIcon(this);

    // 系统标准图标，零资源文件。SP_DesktopIcon 语义上也贴切（桌面收纳）。
    const QIcon fallback =
        QApplication::style()->standardIcon(QStyle::SP_DesktopIcon);
    m_tray->setIcon(fallback);
    m_tray->setToolTip(tr("DeskTidy —— 桌面收纳盒"));

    m_menu = new QMenu();

    // 菜单弹出前重建：保证盒列表与勾选态永远是最新的。
    // 注意子菜单不在这里建 —— rebuildMenu 每次都会 clear() 后重建，
    // 构造函数里提前建一个只会立刻被回收，徒增困惑。
    connect(m_menu, &QMenu::aboutToShow, this, &TrayIcon::rebuildMenu);

    m_tray->setContextMenu(m_menu);

    connect(m_tray, &QSystemTrayIcon::activated,
            this, &TrayIcon::onActivated);

    // 浮窗开关变化时同样要刷新勾选态 —— 否则主窗口里开了浮窗，
    // 托盘菜单上还是空的，两个入口的状态就对不上了。
    connect(m_floating, &FloatingBoxManager::boxWindowToggled,
            this, &TrayIcon::syncFloatingMenu);

    m_tray->show();
}

TrayIcon::~TrayIcon()
{
    // m_menu 没有父对象（QMenu 挂到 QSystemTrayIcon 上不会自动接管所有权），
    // 所以这里显式回收，避免退出时泄漏。
    delete m_menu;
    m_menu = nullptr;
}

bool TrayIcon::isAvailable() const
{
    return m_available;
}

void TrayIcon::showBackgroundHintOnce()
{
    if (!m_available || !m_tray) {
        return;
    }

    // 只提示一次。配置住在 Settings 里而不是成员变量里，
    // 因为"提示过没有"要跨进程记住 —— 否则每次启动都会再弹一回。
    Settings *settings = m_service->settings();
    if (settings->trayHintShown()) {
        return;
    }

    m_tray->showMessage(tr("DeskTidy 仍在后台运行"),
                        tr("程序已最小化到托盘，桌面上的收纳盒浮窗会继续保留。\n"
                           "要完全退出，请右键单击托盘图标并选择「退出」。"),
                        QSystemTrayIcon::Information,
                        6000);

    settings->setTrayHintShown(true);
}

void TrayIcon::syncFloatingMenu()
{
    if (!m_available) {
        return;
    }

    // 菜单正开着的时候不要去重建它 —— clear() 会把当前弹出的那些 QAction
    // 连同子菜单一起销毁，用户正点着的那一项会突然消失。
    // 这种情况下什么都不做即可：菜单关闭前 aboutToShow 不会再来，
    // 但关闭后再开就会走 rebuildMenu，状态自然是新的。
    if (m_menu && m_menu->isVisible()) {
        return;
    }

    rebuildMenu();
}

// ---------------------------------------------------------------------------
// 菜单
// ---------------------------------------------------------------------------
void TrayIcon::rebuildMenu()
{
    if (!m_menu) {
        return;
    }

    // 每次清空重建。clear() 会连带销毁上一次 addMenu 造出来的子菜单
    // （它以本菜单为 parent），所以 m_floatingMenu 在这里是**失效指针**，
    // 必须在下面重新赋值后才能用 —— 不能拿它当"是否初始化过"的标志。
    m_menu->clear();
    m_floatingMenu = m_menu->addMenu(tr("浮窗"));

    // 盒列表**现扫磁盘**，不缓存。主人可能刚在控制中心里新建了一个盒，
    // 甚至在资源管理器里手工建了一个 —— 缓存必然失真。
    const QList<StorageBox> boxes = BoxManager::listBoxes(CoreNames::boxRoot());

    if (boxes.isEmpty()) {
        // 没有盒子时给一个禁用占位项，而不是让子菜单空着。
        // 空菜单在 Windows 上表现为"点了没反应"，主人会以为程序坏了。
        QAction *placeholder = m_floatingMenu->addAction(tr("（还没有收纳盒）"));
        placeholder->setEnabled(false);
    } else {
        for (const StorageBox &box : boxes) {
            const bool open = m_floating->isBoxOpen(box.name);
            m_floatingMenu->addAction(makeBoxAction(box.name, box.path, open));
        }
    }

    m_menu->addSeparator();

    QAction *openCenter = m_menu->addAction(tr("打开控制中心"));
    connect(openCenter, &QAction::triggered, this, &TrayIcon::onOpenControlCenter);

    m_menu->addSeparator();

    QAction *quit = m_menu->addAction(tr("退出"));
    connect(quit, &QAction::triggered, this, &TrayIcon::onQuit);
}

QAction *TrayIcon::makeBoxAction(const QString &boxName,
                                 const QString &boxPath,
                                 bool open)
{
    QAction *action = m_floatingMenu->addAction(boxName);
    action->setCheckable(true);
    action->setChecked(open);

    // 用盒名与盒路径的拷贝捕获，不引用外部变量 —— 菜单项的生命周期
    // 可能比本次 rebuildMenu 调用长。
    connect(action, &QAction::triggered, this, [this, boxName, boxPath](bool checked) {
        if (checked) {
            m_floating->openBox(boxName, boxPath);
        } else {
            m_floating->closeBox(boxName);
        }
        // 勾选态由 QAction 自己维护；boxWindowToggled 会把两边同步回来。
    });

    return action;
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------
void TrayIcon::onActivated(QSystemTrayIcon::ActivationReason reason)
{
    switch (reason) {
    case QSystemTrayIcon::Trigger:
        // 单击。Windows 用户习惯单击唤起，这里跟随平台习惯。
        // 代价是单击无法用于"显示气泡"，但那本来也不是本程序的需求。
        bringWindowToFront();
        break;

    case QSystemTrayIcon::DoubleClick:
        // 双击是 Windows 上更传统的做法。和单击一样处理 ——
        // 不合并成 fallthrough 是刻意的：两者都可能独立触发
        // （某些平台只送 DoubleClick 不送 Trigger，反之亦然），
        // 各自处理最省心，重复调一次 bringWindowToFront 也无副作用。
        bringWindowToFront();
        break;

    default:
        // Context（右键，会弹菜单）、MiddleClick、Unknown 一律不处理。
        break;
    }
}

void TrayIcon::onOpenControlCenter()
{
    bringWindowToFront();
}

void TrayIcon::onQuit()
{
    // 本类不做退出编排 —— 只发信号。
    // 真正的顺序（先落盘几何、再关浮窗、最后 quit）由 main.cpp 统一编排，
    // 因为那涉及 FloatingBoxManager 与 QApplication 两个本类不该插手的对象。
    emit quitRequested();
}

void TrayIcon::bringWindowToFront()
{
    if (!m_window) {
        return;
    }

    // 顺序有讲究：
    //   1) 先去掉最小化状态，否则 show() 出来的还是最小化的窗口；
    //   2) show() 兼顾"之前被 hide() 过"（关闭按钮走的就是隐藏）与"本来就可见"；
    //   3) raise + activateWindow 才能真的抢到前台焦点 —— 只 show() 的话，
    //      Windows 可能只让窗口在任务栏闪一下而不切过去。
    m_window->setWindowState(m_window->windowState() & ~Qt::WindowMinimized);
    m_window->show();
    m_window->raise();
    m_window->activateWindow();
}
