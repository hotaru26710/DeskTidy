#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>
#include <QTimer>

#include "appservice.h"
#include "boxmanager.h"
#include "floatingboxmanager.h"
#include "floatingboxwidget.h"
#include "settings.h"

// ---------------------------------------------------------------------------
// fade_diag —— 实测淡入动画的时间曲线。
//
// 为什么需要：e2e_floating 里那几条"默认浮窗不透明"的断言在加了淡入动画后
// 失败（读到 0 或 0.81），因为探针只 processEvents 一轮，而动画要 180ms。
//
// 但"探针跑太快"只是**一种**解释。另一种是"淡入根本没跑完/卡住了"——
// 后者是真 bug。这个程序把透明度随时间的变化打出来，一眼就能分辨：
//   * 若最后稳定在终值 -> 动画正常，探针该等
//   * 若一直停在 0 或某个中间值 -> 动画真有问题
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyFadeDiag"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyFadeDiag"));

    QTextStream out(stdout);
    out << "=== 淡入动画时间曲线 ===\n\n";

    const QString root = QStringLiteral("F:/QtProject/DeskTidy/_fadediag");
    QDir(root).removeRecursively();
    QDir().mkpath(root);

    AppService service;
    QString err;
    BoxManager::ensureRoot(root + QStringLiteral("/boxes"), &err);

    StorageBox box;
    BoxManager::createBox(root + QStringLiteral("/boxes"), QStringLiteral("淡入"), &box, &err);

    out << "animationsEnabled() 初始值 = "
        << (service.settings()->animationsEnabled() ? "true" : "false") << "\n\n";

    FloatingBoxManager mgr(&service);
    mgr.openBox(QStringLiteral("淡入"), box.path);

    FloatingBoxWidget *fw = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
            if (fb->boxName() == QStringLiteral("淡入")) { fw = fb; break; }
        }
    }
    if (!fw) { out << "找不到浮窗\n"; return 1; }

    out << "openBox 之后立刻读: opacity = " << fw->windowOpacity() << "\n";
    out << "（期望接近 0 —— 淡入的起点）\n\n";

    out << "随时间采样（每 30ms 一次，共 12 次）：\n";
    QElapsedTimer timer;
    timer.start();

    for (int i = 0; i < 12; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);

        // 用 QEventLoop + 定时器精确等 30ms —— processEvents 不保证时长
        QEventLoop loop;
        QTimer::singleShot(30, &loop, &QEventLoop::quit);
        loop.exec();

        out << "  t=" << QString::number(timer.elapsed()).rightJustified(4)
            << "ms   opacity = " << fw->windowOpacity() << "\n";
        out.flush();
    }

    out << "\n最终值 = " << fw->windowOpacity() << "\n";
    out << "=> " << ((qAbs(fw->windowOpacity() - 1.0) < 0.01)
                       ? "动画正常跑完，稳定在终值 1.0"
                       : "FAIL-MARKER 没有回到终值，动画可能有问题") << "\n";

    // ---- 再测一次：关闭动画开关后应当瞬间到位 ----
    out << "\n--- 关掉动画开关后重新打开一个浮窗 ---\n";
    service.settings()->setAnimationsEnabled(false);
    mgr.setAnimationsEnabled(false);

    StorageBox box2;
    BoxManager::createBox(root + QStringLiteral("/boxes"), QStringLiteral("无动画"), &box2, &err);
    mgr.openBox(QStringLiteral("无动画"), box2.path);
    QCoreApplication::processEvents();

    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
            if (fb->boxName() == QStringLiteral("无动画")) {
                out << "  立刻读 opacity = " << fb->windowOpacity() << "\n";
                out << "  => " << ((qAbs(fb->windowOpacity() - 1.0) < 0.01)
                                     ? "OK（无动画时瞬间到位）"
                                     : "FAIL-MARKER 关掉动画后仍是中间值") << "\n";
                break;
            }
        }
    }

    mgr.closeAll();
    QDir(root).removeRecursively();

    out << "\n=== 诊断结束 ===\n";
    out.flush();
    return 0;
}
