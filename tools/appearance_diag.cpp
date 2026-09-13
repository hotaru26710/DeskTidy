#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>

#include "appservice.h"
#include "boxmanager.h"
#include "corenames.h"
#include "floatingboxmanager.h"
#include "floatingboxwidget.h"
#include "itemlistwidget.h"
#include "settings.h"

// ---------------------------------------------------------------------------
// appearance_diag —— 逐环诊断"改外观为什么没生效"。
//
// e2e_floating 里有一组外观断言失败：透明度设 70% 后实际仍是 1.0。
// 而同一组里"切回列表"却通过了 —— 说明广播是通的，问题在中间某一环。
//
// 这个程序把链路拆开逐环打印，定位到底断在哪：
//   1) setFloatAppearance 写配置 -> floatAppearance 读回来对不对
//   2) manager.applyAppearance 有没有 emit
//   3) 浮窗有没有收到广播
//   4) 收到之后 applyAppearance 有没有真的调 setWindowOpacity
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyAppDiag"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyAppDiag"));

    QTextStream out(stdout);
    out << "=== 外观链路逐环诊断 ===\n\n";

    const QString root = QStringLiteral("F:/QtProject/DeskTidy/_appdiag");
    QDir(root).removeRecursively();
    QDir().mkpath(root);

    AppService service;
    QString err;
    BoxManager::ensureRoot(root + QStringLiteral("/boxes"), &err);

    StorageBox box;
    BoxManager::createBox(root + QStringLiteral("/boxes"), QStringLiteral("诊断"), &box, &err);
    {
        QFile f(box.path + QStringLiteral("/a.txt"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) { f.write("x"); f.close(); }
    }

    // ---- 环 1：Settings 读写 ----
    out << "[环1] Settings::setFloatAppearance -> floatAppearance\n";
    BoxAppearance want;
    want.viewMode = BoxAppearance::ViewMode::LargeIcon;
    want.opacity  = 70;

    service.settings()->setFloatAppearance(QStringLiteral("诊断"), want);
    const BoxAppearance got = service.settings()->floatAppearance(QStringLiteral("诊断"));
    out << "  写入 opacity=70, viewMode=3(LargeIcon)\n";
    out << "  读回 opacity=" << got.opacity
        << ", viewMode=" << int(got.viewMode) << "\n";
    out << "  => " << ((got.opacity == 70 && got.viewMode == BoxAppearance::ViewMode::LargeIcon)
                         ? "OK" : "★ 断在这里") << "\n\n";

    // ---- 环 2/3/4：manager 广播 -> 浮窗应用 ----
    out << "[环2] manager.openBox + applyAppearance\n";
    FloatingBoxManager mgr(&service);

    int broadcastCount = 0;
    QObject::connect(&mgr, &FloatingBoxManager::boxAppearanceChanged,
                     [&](const QString &name) {
                         ++broadcastCount;
                         out << "      [信号] boxAppearanceChanged(" << name << ")\n";
                     });

    mgr.openBox(QStringLiteral("诊断"), box.path);
    QCoreApplication::processEvents();

    FloatingBoxWidget *fw = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
            if (fb->boxName() == QStringLiteral("诊断")) { fw = fb; break; }
        }
    }
    if (!fw) { out << "  找不到浮窗，诊断中止\n"; return 1; }

    out << "  浮窗创建后：opacity=" << fw->windowOpacity() << "\n";
    out << "  （配置里是大图标 + 70%，openBox 时应已应用）\n\n";

    out << "[环3] 调 mgr.applyAppearance 设成 70%\n";
    mgr.applyAppearance(QStringLiteral("诊断"), want);
    QCoreApplication::processEvents();

    out << "  广播发出次数: " << broadcastCount << "\n";
    out << "  浮窗 opacity: " << fw->windowOpacity() << "\n";
    out << "  => " << ((qAbs(fw->windowOpacity() - 0.7) < 0.01) ? "OK" : "★ 断在这里") << "\n\n";

    // ---- 环 4：直接调浮窗自己的 applyAppearance ----
    out << "[环4] 绕过广播，直接调 fw->applyAppearance\n";
    BoxAppearance direct;
    direct.viewMode = BoxAppearance::ViewMode::LargeIcon;
    direct.opacity  = 70;
    fw->applyAppearance(direct);
    QCoreApplication::processEvents();
    out << "  浮窗 opacity: " << fw->windowOpacity() << "\n";
    out << "  => " << ((qAbs(fw->windowOpacity() - 0.7) < 0.01)
                         ? "OK（说明浮窗本身没问题，问题在广播链路）"
                         : "★ 浮窗自身的 setWindowOpacity 也没生效") << "\n\n";

    // ---- 检查列表控件 ----
    const QList<ItemListWidget *> lists = fw->findChildren<ItemListWidget *>();
    if (!lists.isEmpty()) {
        ItemListWidget *l = lists.first();
        out << "[附] 列表控件状态\n";
        out << "  viewMode=" << int(l->viewMode()) << "（0=List,1=Icon）\n";
        out << "  iconSize=" << l->iconSize().width() << "x" << l->iconSize().height() << "\n";
        out << "  gridSize=" << l->gridSize().width() << "x" << l->gridSize().height() << "\n";
    }

    mgr.closeAll();
    QDir(root).removeRecursively();

    out << "\n=== 诊断结束 ===\n";
    out.flush();
    return 0;
}
