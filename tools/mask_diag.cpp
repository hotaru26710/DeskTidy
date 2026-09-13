#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QMouseEvent>
#include <QRegion>
#include <QTextStream>
#include <QTimer>

#include "appservice.h"
#include "boxmanager.h"
#include "floatingboxmanager.h"
#include "floatingboxwidget.h"

// ---------------------------------------------------------------------------
// mask_diag —— 诊断"浮窗顶部全透明、点不到"。
//
// 用户报告：浮窗顶部变成全透明，有时候点不到。
//
// 关键事实：**被窗口 mask 裁掉的区域不接收鼠标事件**。所以如果 mask 把
// 标题栏那一行裁掉了，表现就是"看得见内容、但点不动"。
//
// 本程序把 mask 的**逐行可见像素数**打出来：
//   * 正常：第 0 行因圆角略少，第 8 行起满宽，最后几行又略少
//   * 异常：某整行可见像素为 0 —— 那就是点不到的根因
//
// 同时对照打印"控件实际占用的行"，看 mask 与控件布局是否对得上。
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyMaskDiag"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyMaskDiag"));

    QTextStream out(stdout);
    out << "=== 浮窗 mask 逐行可见性诊断 ===\n\n";

    const QString root = QStringLiteral("F:/QtProject/DeskTidy/_maskdiag");
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

    FloatingBoxManager mgr(&service);
    mgr.openBox(QStringLiteral("诊断"), box.path);
    for (int i = 0; i < 15; ++i) {
        QEventLoop t; QTimer::singleShot(40, &t, &QEventLoop::quit); t.exec();
    }

    FloatingBoxWidget *fw = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
            if (fb->boxName() == QStringLiteral("诊断")) { fw = fb; break; }
        }
    }
    if (!fw) { out << "找不到浮窗\n"; return 1; }

    auto analyze = [&](const QString &label) {
        const QRegion m = fw->mask();
        const int w = fw->width();
        const int h = fw->height();

        out << "[" << label << "]  窗口 " << w << "x" << h;
        if (m.isEmpty()) {
            out << "   mask 为空（未设 mask，全部可点）\n\n";
            return;
        }
        const QRect bb = m.boundingRect();
        out << "   mask 包围盒 " << bb.width() << "x" << bb.height()
            << " 位置(" << bb.x() << "," << bb.y() << ")\n";

        // 逐行统计，最多打印 24 行
        const int lines = qMin(h, 24);
        const double step = double(h) / lines;
        int zeroRows = 0;
        for (int i = 0; i < lines; ++i) {
            const int y = qBound(0, int(i * step), h - 1);
            int visible = 0;
            int firstX = -1, lastX = -1;
            for (int x = 0; x < w; ++x) {
                if (m.contains(QPoint(x, y))) {
                    ++visible;
                    if (firstX < 0) firstX = x;
                    lastX = x;
                }
            }
            const double ratio = w > 0 ? double(visible) / w : 0.0;
            QString note;
            if (visible == 0) {
                note = QStringLiteral("   <<< 整行被裁，这一行收不到点击");
                ++zeroRows;
            } else if (ratio < 0.97) {
                note = QStringLiteral("   (边缘行，圆角所致)");
            }
            out << "    y=" << QString::number(y).rightJustified(4)
                << "  可见 " << QString::number(visible).rightJustified(4) << "/" << w
                << "  x∈[" << firstX << "," << lastX << "]" << note << "\n";
        }
        out << "    => 整行被裁的行数: " << zeroRows << "\n\n";
    };

    analyze(QStringLiteral("展开"));

    // 找标题栏，看它占哪些行
    out << "控件占用行（用于对照）:\n";
    for (QObject *child : fw->children()) {
        if (auto *w2 = qobject_cast<QWidget *>(child)) {
            if (w2->isVisible()) {
                out << "    " << w2->metaObject()->className()
                    << "  y=" << w2->y() << ".." << (w2->y() + w2->height() - 1)
                    << "  x=" << w2->x() << ".." << (w2->x() + w2->width() - 1) << "\n";
            }
        }
    }
    out << "\n";

    // -----------------------------------------------------------------------
    // 用**真实路径**切到卷起：双击标题栏。
    //
    // 这里刻意不用 resize() 手工模拟 —— applyRollUpState 会先设 min/max 高度
    // 再 resize，而且中间夹着 setVisible 引起的布局重算。手工 resize 走不到
    // 那条路径，测出来的 mask 是"理想情况"，复现不了用户看到的问题。
    // -----------------------------------------------------------------------
    out << "[卷起] 走真实路径（双击标题栏）\n";
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
    if (titleBar) {
        // 直接投递双击事件给标题栏，等价于用户双击
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(10, 10),
                          QPointF(titleBar->mapToGlobal(QPoint(10, 10))),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(10, 10),
                        QPointF(titleBar->mapToGlobal(QPoint(10, 10))),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(titleBar, &press);
        QApplication::sendEvent(titleBar, &dbl);
    } else {
        out << "  找不到标题栏，改为直接 resize 到 30 高\n";
        fw->resize(fw->width(), 30);
    }

    for (int i = 0; i < 20; ++i) {
        QEventLoop t; QTimer::singleShot(40, &t, &QEventLoop::quit); t.exec();
    }

    analyze(QStringLiteral("卷起后"));

    out << "\n  卷起后子控件可见性（应当只剩标题栏可见）:\n";
    for (QObject *child : fw->children()) {
        if (auto *w2 = qobject_cast<QWidget *>(child)) {
            const QString cls = QString::fromLatin1(w2->metaObject()->className());
            out << "    " << cls << "  可见=" << (w2->isVisible() ? "是" : "否")
                << "  y=" << w2->y() << " h=" << w2->height() << "\n";
        }
    }
    out << "\n";

    // -----------------------------------------------------------------------
    // 检查每个可见子控件的"是否自己画背景"。
    //
    // WA_TranslucentBackground 开了之后父窗口不画背景，所以每一层子控件
    // 都必须自己负责自己的底色。谁没画，谁的位置就会露出透明 ——
    // 用户说的"顶部全透明"正是这种症状，而"顶部"就是标题栏。
    //
    // autoFillBackground 与 palette 是最可靠的判据（样式表设的 background
    // 不会反映在 autoFillBackground 上，但样式表本身也是一种"会画"的证据）。
    // -----------------------------------------------------------------------
    out << "[背景自足性检查]  窗口开了透明底，每层子控件都得自己画背景：\n";
    for (QObject *child : fw->children()) {
        auto *w2 = qobject_cast<QWidget *>(child);
        if (!w2) continue;
        const QString cls = QString::fromLatin1(w2->metaObject()->className());
        const bool hasSS = !w2->styleSheet().isEmpty();
        const bool autoFill = w2->autoFillBackground();
        const QColor winColor = w2->palette().color(QPalette::Window);
        out << "    " << cls
            << "  样式表=" << (hasSS ? "有" : "无")
            << "  autoFillBackground=" << (autoFill ? "是" : "否")
            << "  palette.Window=" << winColor.name()
            << (hasSS || autoFill ? QStringLiteral("   OK")
                                  : QStringLiteral("   <<< 不画背景，会露透明底"))
            << "\n";
    }
    out << "\n";

    mgr.closeAll();
    QDir(root).removeRecursively();
    out.flush();
    return 0;
}
