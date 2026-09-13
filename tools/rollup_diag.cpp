#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QMouseEvent>
#include <QTextStream>
#include <QTimer>

#include "appservice.h"
#include "boxmanager.h"
#include "floatingboxmanager.h"
#include "floatingboxwidget.h"

// ---------------------------------------------------------------------------
// rollup_diag —— 采样卷起 / 展开的高度曲线。
//
// 照 fade_diag 的模式写，因为它那套"每 30ms 采一次值、看曲线形状"的
// 做法对高度同样适用，而且能一次性回答三个问题：
//   1) 动画到底有没有跑（高度是不是逐帧变化，而不是"啪"地跳到位）
//   2) 终值对不对（展开应当回到记录的高度，卷起应当到标题栏高）
//   3) 动画跑完之后 m_expandedHeight 有没有被污染
//
// 第 3 条是这一步最危险的坑：resizeEvent 在动画每一帧都会触发，
// 而展开动画期间 m_rolledUp 已经是 false，"记住展开高度"那条守卫拦不住它。
// 若没有 m_rollAnimating 兜着，m_expandedHeight 会被写成动画的中间值，
// 表现为"展开一次比一次矮"。
//
// 【判据说明】
// m_expandedHeight 是私有的，探针读不到。这里用一个**行为等价**的办法验证：
//   记下展开时的高度 H1 -> 卷起 -> 再展开 -> 应当还是 H1。
//   若 m_expandedHeight 被污染，第二次展开会明显矮于 H1。
// 这比读私有成员更接近用户真实感知。
// ---------------------------------------------------------------------------

static int gPass = 0;
static int gFail = 0;

static void check(bool ok, const QString &what, QTextStream &out)
{
    if (ok) { ++gPass; out << "  [PASS] " << what << "\n"; }
    else    { ++gFail; out << "  [FAIL] " << what << "\n"; }
    out.flush();
}

static void pump(int ms)
{
    QEventLoop t;
    QTimer::singleShot(ms, &t, &QEventLoop::quit);
    t.exec();
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyRollDiag"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyRollDiag"));

    QTextStream out(stdout);
    out << "=== 卷起 / 展开高度曲线 ===\n\n";

    const QString root = QStringLiteral("F:/QtProject/DeskTidy/_rolldiag");
    QDir(root).removeRecursively();
    QDir().mkpath(root);

    AppService service;
    QString err;
    BoxManager::ensureRoot(root + QStringLiteral("/boxes"), &err);

    StorageBox box;
    BoxManager::createBox(root + QStringLiteral("/boxes"), QStringLiteral("卷起"), &box, &err);
    {
        QFile f(box.path + QStringLiteral("/a.txt"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) { f.write("x"); f.close(); }
    }

    FloatingBoxManager mgr(&service);
    mgr.openBox(QStringLiteral("卷起"), box.path);
    pump(1200);

    FloatingBoxWidget *fw = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
            if (fb->boxName() == QStringLiteral("卷起")) { fw = fb; break; }
        }
    }
    if (!fw) { out << "找不到浮窗\n"; return 1; }

    out << "初始高度 = " << fw->height() << "\n\n";

    // 标题栏指针：双击它来触发卷起（走真实路径）
    QWidget *titleBar = nullptr;
    for (QObject *child : fw->children()) {
        if (auto *w2 = qobject_cast<QWidget *>(child)) {
            if (QString::fromLatin1(w2->metaObject()->className())
                    .contains(QStringLiteral("TitleBar"))) {
                titleBar = w2;
                break;
            }
        }
    }
    if (!titleBar) { out << "找不到标题栏\n"; return 1; }

    auto doubleClickTitle = [&]() {
        const QPointF local(10, 10);
        const QPointF global(titleBar->mapToGlobal(QPoint(10, 10)));
        QMouseEvent press(QEvent::MouseButtonPress, local, global,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent dbl(QEvent::MouseButtonDblClick, local, global,
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(titleBar, &press);
        QApplication::sendEvent(titleBar, &dbl);
    };

    auto sampleCurve = [&](const QString &label) -> QList<int> {
        QList<int> heights;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 500) {
            pump(25);
            heights << fw->height();
        }
        QString chain;
        for (int i = 0; i < heights.size(); i += 2)
            chain += QString::number(heights[i]) + QLatin1Char(' ');
        out << "  " << label << " 高度序列: " << chain << "\n";
        return heights;
    };

    // ---- [1] 卷起 ----
    const int expandedH = fw->height();
    out << "[1] 卷起（双击标题栏）\n";
    doubleClickTitle();
    const QList<int> rollCurve = sampleCurve(QStringLiteral("卷起"));
    const int rolledH = fw->height();
    out << "  终点高度 = " << rolledH << "（期望约 30）\n";

    // 曲线里应当有"既不是起点也不是终点"的中间值，那才叫动画
    int distinct = 0;
    for (int i = 1; i < rollCurve.size(); ++i)
        if (rollCurve[i] != rollCurve[i - 1]) ++distinct;
    check(distinct >= 2,
          QStringLiteral("卷起是渐变的（出现 %1 次高度变化，不是一步到位）").arg(distinct), out);
    check(rolledH <= 35,
          QStringLiteral("卷起后高度落到标题栏高（%1）").arg(rolledH), out);

    // ---- [2] 展开 ----
    out << "\n[2] 展开（再双击）\n";
    doubleClickTitle();
    const QList<int> expandCurve = sampleCurve(QStringLiteral("展开"));
    const int backH = fw->height();
    out << "  终点高度 = " << backH << "（期望回到 " << expandedH << "）\n";

    distinct = 0;
    for (int i = 1; i < expandCurve.size(); ++i)
        if (expandCurve[i] != expandCurve[i - 1]) ++distinct;
    check(distinct >= 2,
          QStringLiteral("展开是渐变的（出现 %1 次高度变化）").arg(distinct), out);
    check(backH == expandedH,
          QStringLiteral("展开后回到原来的高度（%1）").arg(backH), out);

    // ---- [3] 反复卷起展开，高度不应衰减 ----
    //
    // 这是 m_expandedHeight 有没有被动画污染的**行为级判据**：
    // 若记录被动画的中间值覆盖，第二次展开就会明显矮于第一次，
    // 而且会一轮比一轮矮。
    out << "\n[3] 反复卷起展开 3 轮（查展开高度有没有衰减）\n";
    QList<int> eachRound;
    for (int round = 0; round < 3; ++round) {
        doubleClickTitle();     // 卷起
        pump(500);
        doubleClickTitle();     // 展开
        pump(500);
        eachRound << fw->height();
        out << "  第 " << (round + 1) << " 轮展开后高度 = " << fw->height() << "\n";
    }
    bool allEqual = true;
    for (int h : eachRound)
        if (h != expandedH) allEqual = false;
    check(allEqual,
          QStringLiteral("三轮展开都回到同一高度（没有逐轮变矮）"), out);

    // ---- [4] 关掉动画后行为应当与改造前一致 ----
    out << "\n[4] 关掉动画开关后的行为\n";
    mgr.setAnimationsEnabled(false);
    pump(200);

    doubleClickTitle();         // 卷起
    pump(250);
    const int instantRolled = fw->height();
    out << "  关动画后卷起高度 = " << instantRolled << "\n";

    doubleClickTitle();         // 展开
    pump(250);
    const int instantBack = fw->height();
    out << "  关动画后展开高度 = " << instantBack << "\n";

    check(instantRolled <= 35,
          QStringLiteral("关动画时卷起仍然到位（%1）").arg(instantRolled), out);
    check(instantBack == expandedH,
          QStringLiteral("关动画时展开仍然回到原高度（%1）").arg(instantBack), out);

    // 关动画后卷起态的 min/max 必须仍然锁死 —— 否则能拉出空白
    doubleClickTitle();         // 再卷起，检查锁
    pump(250);
    const bool locked = (fw->minimumHeight() == fw->maximumHeight());
    out << "  关动画卷起后 min=" << fw->minimumHeight()
        << " max=" << fw->maximumHeight() << "\n";
    check(locked, QStringLiteral("关动画时卷起后 min==max（拉不出空白）"), out);
    doubleClickTitle();
    pump(250);

    mgr.closeAll();
    QDir(root).removeRecursively();

    out << "\n=== 结果：" << gPass << " 通过 / " << gFail << " 失败 ===\n";
    out.flush();
    return gFail == 0 ? 0 : 1;
}
