#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QElapsedTimer>
#include <QTextStream>
#include <QTimer>

#include "appservice.h"
#include "boxmanager.h"
#include "floatingboxmanager.h"
#include "floatingboxwidget.h"

// ---------------------------------------------------------------------------
// heightlock_diag —— 验证"卷起时的 min/max 高度锁会不会顶掉高度动画"。
//
// 【为什么必须先验证这个】
// applyRollUpState 里是这么收起来的：
//     setMinimumHeight(rolledUpHeight());     // 锁死下限
//     setMaximumHeight(rolledUpHeight());     // 锁死上限
//     resize(width(), rolledUpHeight());
//
// 一旦 min == max，resize() 就只能得到那一个高度。若动画也走 resize()，
// 它的每一帧都会被顶回 30 —— 表现是"动画完全没有效果"，而不是报错。
// 这类"看起来没跑"的故障最难查，所以在写动画之前先把它摸清楚。
//
// 本程序对比三种情形下的 resize 效果：
//   [A] 正常状态（无锁）        -> resize 应当生效
//   [B] 锁死 min=max=30        -> resize 应当被顶掉
//   [C] 只锁 min，放开 max      -> resize 应当生效（这就是动画要用的姿势）
// ---------------------------------------------------------------------------

static QTextStream *g_out = nullptr;

static void say(const QString &s)
{
    *g_out << s << "\n";
    g_out->flush();
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyHeightLock"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyHeightLock"));

    QTextStream out(stdout);
    g_out = &out;

    say(QStringLiteral("=== 卷起高度锁对 resize 的影响 ===\n"));

    const QString root = QStringLiteral("F:/QtProject/DeskTidy/_heightlock");
    QDir(root).removeRecursively();
    QDir().mkpath(root);

    AppService service;
    QString err;
    BoxManager::ensureRoot(root + QStringLiteral("/boxes"), &err);

    StorageBox box;
    BoxManager::createBox(root + QStringLiteral("/boxes"), QStringLiteral("锁"), &box, &err);
    {
        QFile f(box.path + QStringLiteral("/a.txt"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) { f.write("x"); f.close(); }
    }

    FloatingBoxManager mgr(&service);
    mgr.openBox(QStringLiteral("锁"), box.path);
    for (int i = 0; i < 15; ++i) {
        QEventLoop t; QTimer::singleShot(40, &t, &QEventLoop::quit); t.exec();
    }

    FloatingBoxWidget *fw = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
            if (fb->boxName() == QStringLiteral("锁")) { fw = fb; break; }
        }
    }
    if (!fw) { say(QStringLiteral("找不到浮窗")); return 1; }

    auto settle = [&]() {
        for (int i = 0; i < 6; ++i) {
            QEventLoop t; QTimer::singleShot(30, &t, &QEventLoop::quit); t.exec();
        }
    };

    auto report = [&](const QString &label, int wanted) {
        say(QStringLiteral("  [%1] 请求 resize 到 %2  ->  实际高度 %3  %4")
                .arg(label)
                .arg(wanted)
                .arg(fw->height())
                .arg(fw->height() == wanted ? QStringLiteral("[生效]")
                                            : QStringLiteral("<<< 被顶掉了")));
    };

    // ---- [A] 无锁 ----
    say(QStringLiteral("[A] 正常状态（min/max 都是默认值）"));
    say(QStringLiteral("    当前 minHeight=%1  maxHeight=%2")
            .arg(fw->minimumHeight()).arg(fw->maximumHeight()));
    fw->resize(fw->width(), 200);
    settle();
    report(QStringLiteral("A"), 200);

    // ---- [B] 锁死 min == max == 30 ----
    say(QStringLiteral("\n[B] 锁死 min=max=30（applyRollUpState 卷起分支的做法）"));
    fw->setMinimumHeight(30);
    fw->setMaximumHeight(30);
    fw->resize(fw->width(), 30);
    settle();
    say(QStringLiteral("    锁上之后高度 = %1").arg(fw->height()));
    fw->resize(fw->width(), 200);
    settle();
    report(QStringLiteral("B"), 200);

    // ---- [C] 只锁 min，放开 max ----
    say(QStringLiteral("\n[C] 只锁 min=30、max 放开（动画要用的姿势）"));
    fw->setMinimumHeight(30);
    fw->setMaximumHeight(QWIDGETSIZE_MAX);
    settle();
    fw->resize(fw->width(), 200);
    settle();
    report(QStringLiteral("C"), 200);

    // ---- [D] 动画进行中逐帧 resize 会不会被顶掉 ----
    //
    // 这是最接近真实场景的一步：模拟动画每帧调 resize。
    say(QStringLiteral("\n[D] 模拟动画逐帧 resize（min=30, max 放开）"));
    fw->setMinimumHeight(30);
    fw->setMaximumHeight(QWIDGETSIZE_MAX);
    fw->resize(fw->width(), 30);
    settle();
    say(QStringLiteral("    起点高度 = %1").arg(fw->height()));

    const int from = 30;
    const int to = 260;
    QList<int> observed;
    for (int step = 0; step <= 10; ++step) {
        const int h = from + (to - from) * step / 10;
        fw->resize(fw->width(), h);
        settle();
        observed << fw->height();
    }
    QString chain;
    for (int h : observed) chain += QString::number(h) + QLatin1Char(' ');
    say(QStringLiteral("    逐帧请求 30..260，实际高度序列: ") + chain);
    say(QStringLiteral("    终点高度 = %1  %2")
            .arg(fw->height())
            .arg(fw->height() >= 250 ? QStringLiteral("[动画能跑]")
                                     : QStringLiteral("<<< 动画会被顶掉")));

    mgr.closeAll();
    QDir(root).removeRecursively();

    say(QStringLiteral("\n=== 结论 ==="));
    say(QStringLiteral("  若 [B] 被顶掉、[C][D] 生效，则动画的做法就是："));
    say(QStringLiteral("  动画开始时 setMaximumHeight(QWIDGETSIZE_MAX) 解锁，"));
    say(QStringLiteral("  动画结束后再按需锁回去。"));
    out.flush();
    return 0;
}
