// ---------------------------------------------------------------------------
// main.cpp —— DeskTidy 程序入口。
//
// 职责有五件事：
//   1) 设置应用元信息（QSettings 的落盘位置依赖 organizationName/applicationName，
//      设错会导致配置写到别处，故必须在构造 Settings 之前完成）；
//   2) 挑选一套中文显示效果可靠的字体 —— 界面全中文，用默认字体在部分 Windows
//      环境会发虚、字重不均；
//   3) 关掉"最后一个窗口关闭即退出"，改由托盘菜单独占退出入口 —— 见下文注释；
//   4) 创建 AppService / FloatingBoxManager / TrayIcon 并按正确的析构顺序装配；
//   5) 起主窗口、恢复上次开着的浮窗、进入事件循环。
//
// Qt6 已默认启用高 DPI 缩放，故不再设置 Qt5 时代的
// AA_EnableHighDpiScaling / AA_UseHighDpiPixmaps 属性（那套在 Qt6 已废弃）。
// ---------------------------------------------------------------------------

#include "ui/floatingboxmanager.h"
#include "ui/mainwindow.h"
#include "ui/preferreddropeffect.h"
#include "ui/trayicon.h"

#include "appservice.h"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QStringList>

namespace {

// 从中文显示效果最好的几款字体里挑第一个系统真实装了的。
// 之所以给一串候选而不是写死一个：Windows 版本差异较大，
// 写死 "Microsoft YaHei" 在少数精简版系统上会回落到难看的默认字形。
QFont pickUiFont()
{
    const QStringList candidates = {
        QStringLiteral("Microsoft YaHei UI"),
        QStringLiteral("Microsoft YaHei"),
        QStringLiteral("PingFang SC"),          // 万一将来在 macOS 上编译
        QStringLiteral("Noto Sans CJK SC"),     // Linux
    };

    const QStringList families = QFontDatabase::families();
    for (const QString &name : candidates) {
        if (families.contains(name, Qt::CaseInsensitive)) {
            QFont font(name);
            font.setPointSize(10);
            return font;
        }
    }

    // 一个都没有：用系统默认字体，但保证字号合适。
    QFont font;
    font.setPointSize(10);
    return font;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // 这几项必须早于任何 Settings 构造 —— 它决定 INI 落到 %APPDATA%\DeskTidy\。
    app.setOrganizationName(QStringLiteral("DeskTidy"));
    app.setApplicationName(QStringLiteral("DeskTidy"));
    app.setApplicationDisplayName(QStringLiteral("DeskTidy"));
    app.setApplicationVersion(QStringLiteral("1.0"));

    app.setFont(pickUiFont());

    // 让 Windows 把"从浮窗拖出条目"当成移动而非复制。
    // 不注册的话资源管理器只会复制一份，源文件仍留在盒里，与需求不符。
    // 见 ui/preferreddropeffect.h —— 这是实测验证过的方案。
    PreferredDropEffect::ensureRegistered();

    // ⚠️ 关掉"最后一个窗口关闭就退出程序"。
    //
    // 默认值是 true。加上浮窗与托盘之后它会同时破坏两件事：
    //   1) 关掉主窗口时若一个浮窗都没开，进程直接退出，"常驻"名存实亡；
    //   2) 关掉最后一个浮窗时同理，主人会莫名其妙地丢掉整个程序。
    // 更隐蔽的是：Qt::Tool 类型的窗口（浮窗正是）**不计入** lastWindowClosed
    // 判定，所以哪怕浮窗还开着，主窗口一隐藏也可能触发退出 —— 这行为依赖
    // 平台细节，不能靠猜。
    //
    // 彻底关掉它之后，退出就只剩一条路径：托盘菜单 -> quitRequested -> qApp->quit()。
    // 与之配套的是 MainWindow::closeEvent 的"有托盘则隐藏"分支。
    // 注意：这条决定**以托盘可用为前提**，所以那边对"没有托盘"的情况
    // 专门做了照常退出的分支，否则会造出关不掉的幽灵进程。
    app.setQuitOnLastWindowClosed(false);

    // ⚠️ 声明顺序即生命周期：service、floating 必须声明在 window 之前，
    // 而 tray 必须声明在 window 与 floating **之后**。
    //
    // 栈上对象按声明逆序析构：
    //   tray    先析构 —— 它引用 window 与 floating，必须最先退场；
    //   window  其次   —— 它的析构里可能还会读 service/floating 的状态；
    //   floating、service 最后 —— 保证活到最后。
    //
    // floating 持有的浮窗更是要在主窗口关掉之后继续存在 —— 这是"常驻"的基础。
    // 若有人调换这几行，退出时会崩溃 —— 这是一处极易被"顺手整理"搞坏的地方。
    AppService         service;
    FloatingBoxManager floating(&service);
    MainWindow         window(&service, &floating);

    // 托盘在窗口之后建（它需要窗口指针来"唤回控制中心"），
    // 再用 setter 设回窗口（窗口需要它来判断关闭行为）。
    // 这样避开了"建窗口要先有托盘、建托盘要先有窗口"的循环依赖。
    TrayIcon           tray(&service, &window, &floating);
    window.setTray(&tray);

    window.show();

    // 主窗口显示之后再恢复浮窗：浮窗的默认落位依赖屏幕几何，
    // 且先恢复浮窗会让它们在主窗口之前闪一下，观感上是"先冒出一堆小窗再出主界面"。
    floating.restoreOpenBoxes();

    // 退出编排。
    //
    // 顺序不能反：closeAll 之后再落盘，窗口已经销毁，几何就写不出来了。
    // saveAllGeometry 放在最前面，是为了兜住"去抖定时器还没触发就被 quit"
    // 导致的最后一次拖动位置丢失。
    QObject::connect(&tray, &TrayIcon::quitRequested, &app, [&floating, &app]() {
        floating.saveAllGeometry();
        floating.closeAll();
        app.quit();
    });

    return app.exec();
}
