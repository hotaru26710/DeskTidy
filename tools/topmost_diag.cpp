#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QRegion>
#include <QTextStream>
#include <QTimer>

#include "appservice.h"
#include "boxmanager.h"
#include "floatingboxmanager.h"
#include "floatingboxwidget.h"

// ---------------------------------------------------------------------------
// topmost_diag —— 验证"切换总在最前之后，圆角遮罩是否还在、位置对不对"。
//
// 用户报告：浮窗顶部变成全透明、有时候点不到。
//
// 关键背景：
//   * mask 决定"哪一块区域算窗口" —— 被裁掉的地方既不显示、也不接收鼠标；
//   * applyAlwaysOnTop 里 setWindowFlags() 会**重建原生窗口**；
//   * mask 是挂在原生窗口上的属性，重建后可能残留成**错位的旧遮罩**。
//
// 这个程序在切换置顶**前后**各量一次 mask 的包围盒与覆盖率，
// 只要两者不一致，就说明那条路径确实会把遮罩弄坏。
// ---------------------------------------------------------------------------

static int gPass = 0;
static int gFail = 0;

static void check(bool ok, const QString &what, QTextStream &out)
{
    if (ok) { ++gPass; out << "  [PASS] " << what << "\n"; }
    else    { ++gFail; out << "  [FAIL] " << what << "\n"; }
    out.flush();
}

// 返回某个窗口 mask 的"每行可见比例"的指纹，用于前后比对
static QString maskFingerprint(QWidget *w)
{
    const QRegion m = w->mask();
    if (m.isEmpty())
        return QStringLiteral("<无 mask>");

    const int width = w->width();
    const int height = w->height();
    QString fp = QStringLiteral("%1x%2/mask%3x%4:")
                     .arg(width).arg(height)
                     .arg(m.boundingRect().width())
                     .arg(m.boundingRect().height());

    // 采样 5 行
    for (int i = 0; i < 5; ++i) {
        const int y = qBound(0, height * i / 4, height - 1);
        int visible = 0;
        for (int x = 0; x < width; ++x)
            if (m.contains(QPoint(x, y))) ++visible;
        fp += QStringLiteral("%1,").arg(visible);
    }
    return fp;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyTopmostDiag"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyTopmostDiag"));

    QTextStream out(stdout);
    out << "=== 切换置顶前后的遮罩一致性 ===\n\n";

    const QString root = QStringLiteral("F:/QtProject/DeskTidy/_topmostdiag");
    QDir(root).removeRecursively();
    QDir().mkpath(root);

    AppService service;
    QString err;
    BoxManager::ensureRoot(root + QStringLiteral("/boxes"), &err);

    StorageBox box;
    BoxManager::createBox(root + QStringLiteral("/boxes"), QStringLiteral("置顶"), &box, &err);
    {
        QFile f(box.path + QStringLiteral("/a.txt"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) { f.write("x"); f.close(); }
    }

    FloatingBoxManager mgr(&service);
    mgr.openBox(QStringLiteral("置顶"), box.path);
    for (int i = 0; i < 15; ++i) {
        QEventLoop t; QTimer::singleShot(40, &t, &QEventLoop::quit); t.exec();
    }

    FloatingBoxWidget *fw = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
            if (fb->boxName() == QStringLiteral("置顶")) { fw = fb; break; }
        }
    }
    if (!fw) { out << "找不到浮窗\n"; return 1; }

    const QSize sizeBefore = fw->size();
    const QString fpBefore = maskFingerprint(fw);
    out << "切换前: 尺寸=" << sizeBefore.width() << "x" << sizeBefore.height() << "\n";
    out << "        mask 指纹 = " << fpBefore << "\n\n";

    // 走真实路径：右键菜单里那一项调的就是这个
    out << "切换「总在最前」...\n";
    fw->applyAlwaysOnTop(!fw->isAlwaysOnTop());
    for (int i = 0; i < 15; ++i) {
        QEventLoop t; QTimer::singleShot(40, &t, &QEventLoop::quit); t.exec();
    }

    const QSize sizeAfter = fw->size();
    const QString fpAfter = maskFingerprint(fw);
    out << "切换后: 尺寸=" << sizeAfter.width() << "x" << sizeAfter.height() << "\n";
    out << "        mask 指纹 = " << fpAfter << "\n\n";

    check(sizeAfter == sizeBefore,
          QStringLiteral("切换置顶后尺寸没变"), out);
    check(fpAfter == fpBefore,
          QStringLiteral("切换置顶后 mask 与切换前完全一致（遮罩没坏）"), out);

    // 再看 mask 有没有"整行被裁"——那才是点不到的根因
    const QRegion m = fw->mask();
    int zeroRows = 0;
    if (!m.isEmpty()) {
        for (int y = 0; y < fw->height(); ++y) {
            int visible = 0;
            for (int x = 0; x < fw->width(); ++x)
                if (m.contains(QPoint(x, y))) { ++visible; break; }
            if (visible == 0) ++zeroRows;
        }
    }
    check(zeroRows == 0,
          QStringLiteral("没有整行被裁掉（整行被裁 = 那一行收不到点击）"), out);

    // 卷起状态下再来一遍 —— 用户报告的是"顶部"点不到，卷起时整个窗口就是标题栏
    out << "\n--- 卷起状态下再切一次置顶 ---\n";
    fw->resize(fw->width(), 30);
    for (int i = 0; i < 10; ++i) {
        QEventLoop t; QTimer::singleShot(40, &t, &QEventLoop::quit); t.exec();
    }
    const QString fpRolled = maskFingerprint(fw);
    out << "卷起后 mask 指纹 = " << fpRolled << "\n";

    fw->applyAlwaysOnTop(!fw->isAlwaysOnTop());
    for (int i = 0; i < 15; ++i) {
        QEventLoop t; QTimer::singleShot(40, &t, &QEventLoop::quit); t.exec();
    }
    const QString fpRolledAfter = maskFingerprint(fw);
    out << "再切置顶后指纹 = " << fpRolledAfter << "\n";

    check(fpRolledAfter == fpRolled,
          QStringLiteral("卷起状态下切换置顶，遮罩同样保持一致"), out);

    const QRegion mr = fw->mask();
    int zeroRowsRolled = 0;
    if (!mr.isEmpty()) {
        for (int y = 0; y < fw->height(); ++y) {
            bool any = false;
            for (int x = 0; x < fw->width(); ++x)
                if (mr.contains(QPoint(x, y))) { any = true; break; }
            if (!any) ++zeroRowsRolled;
        }
    }
    check(zeroRowsRolled == 0,
          QStringLiteral("卷起状态下也没有整行被裁"), out);

    mgr.closeAll();
    QDir(root).removeRecursively();

    out << "\n=== 结果：" << gPass << " 通过 / " << gFail << " 失败 ===\n";
    out.flush();
    return gFail == 0 ? 0 : 1;
}
