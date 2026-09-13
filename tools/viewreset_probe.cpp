#include <QApplication>
#include <QDir>
#include <QFile>
#include <QListView>
#include <QTextStream>

#include "appservice.h"
#include "boxmanager.h"
#include "corenames.h"
#include "floatingboxmanager.h"
#include "floatingboxwidget.h"
#include "itemlistwidget.h"

// ---------------------------------------------------------------------------
// viewreset_probe —— 验证「从图标模式切回列表模式」是否真的复位。
//
// 【为什么单独写这个】
// 这是一个真实 bug 的守门探针。原来的 applyAppearance 按**值**判断早退：
//     if (viewMode == List && iconSize == 0) return;
// 于是"从大图标切回列表"这次实实在在的切换，因为新值恰好等于默认值，
// 被当成了"没配置过"而直接返回 —— setViewMode 没调回 ListMode，
// gridSize 也没清空，界面停在图标网格上、行高还被撑开。
//
// 单测覆盖不到它：那需要真的构造 QWidget 并观察它的内部属性，
// 而本探针正是干这个的。
//
// 三个序列对照（B 是生产路径，最关键）：
//   [A] 构造即默认         -> 不该被动过任何属性
//   [B] 大图标 -> 列表      -> 必须复位（这是 bug 的现场）
//   [C] 大图标 -> 小图标 -> 列表 -> 同样必须复位
// ---------------------------------------------------------------------------

static int gPass = 0;
static int gFail = 0;

static void check(bool ok, const QString &what)
{
    QTextStream out(stdout);
    if (ok) { ++gPass; out << "  [PASS] " << what << "\n"; }
    else    { ++gFail; out << "  [FAIL] " << what << "\n"; }
    out.flush();
}

static ItemListWidget *listOf(FloatingBoxWidget *fw)
{
    const QList<ItemListWidget *> found = fw->findChildren<ItemListWidget *>();
    return found.isEmpty() ? nullptr : found.first();
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyViewProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyViewProbe"));

    QTextStream out(stdout);
    out << "=== 视图切换复位验证 ===\n\n";

    const QString root = QStringLiteral("F:/QtProject/DeskTidy/_view_probe");
    QDir(root).removeRecursively();
    QDir().mkpath(root);

    AppService service;
    QString err;
    BoxManager::ensureRoot(root + QStringLiteral("/boxes"), &err);

    StorageBox box;
    BoxManager::createBox(root + QStringLiteral("/boxes"), QStringLiteral("视图"), &box, &err);
    {
        QFile f(box.path + QStringLiteral("/a.txt"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) { f.write("x"); f.close(); }
    }

    FloatingBoxWidget fw(&service, box.name, box.path);
    fw.show();
    QCoreApplication::processEvents();

    ItemListWidget *list = listOf(&fw);
    if (!list) { out << "找不到列表控件\n"; return 1; }

    // ---- [A] 构造即默认：不该被动过 ----
    out << "[A] 构造后（默认外观）\n";
    check(list->viewMode() == QListView::ListMode,
          QStringLiteral("初始是列表模式"));
    check(list->gridSize().isEmpty(),
          QStringLiteral("初始没有网格（gridSize 为空）"));

    // ---- [B] 大图标 -> 列表（bug 的现场）----
    out << "\n[B] 切到大图标，再切回列表\n";

    BoxAppearance big;
    big.viewMode = BoxAppearance::ViewMode::LargeIcon;
    list->applyAppearance(big);
    QCoreApplication::processEvents();

    check(list->viewMode() == QListView::IconMode,
          QStringLiteral("切到大图标后确实是图标模式"));
    check(list->iconSize().width() == 64,
          QStringLiteral("图标尺寸是 64（实际 %1）").arg(list->iconSize().width()));
    const QSize iconGrid = list->gridSize();
    check(!iconGrid.isEmpty(),
          QStringLiteral("图标模式下有网格（%1x%2）")
              .arg(iconGrid.width()).arg(iconGrid.height()));

    // 关键：切回列表
    BoxAppearance plain;    // 默认 = 列表 + iconSize 0
    list->applyAppearance(plain);
    QCoreApplication::processEvents();

    check(list->viewMode() == QListView::ListMode,
          QStringLiteral("★ 切回列表后 viewMode 真的复位了"));
    check(list->gridSize().isEmpty(),
          QStringLiteral("★ 切回列表后 gridSize 被清空了（残留会撑开行高）"));
    check(list->iconSize().width() == 16,
          QStringLiteral("★ 切回列表后图标尺寸回到 16（实际 %1）")
              .arg(list->iconSize().width()));
    check(list->movement() == QListView::Static,
          QStringLiteral("movement 已复位"));
    check(!list->wordWrap(),
          QStringLiteral("wordWrap 已复位（列表模式不该换行）"));

    // ---- [C] 三级跳 ----
    out << "\n[C] 大图标 -> 中图标 -> 小图标 -> 列表\n";

    BoxAppearance mid;
    mid.viewMode = BoxAppearance::ViewMode::MediumIcon;
    list->applyAppearance(mid);
    QCoreApplication::processEvents();
    check(list->iconSize().width() == 32,
          QStringLiteral("中图标是 32（实际 %1）").arg(list->iconSize().width()));

    BoxAppearance small;
    small.viewMode = BoxAppearance::ViewMode::SmallIcon;
    list->applyAppearance(small);
    QCoreApplication::processEvents();
    check(list->iconSize().width() == 16,
          QStringLiteral("小图标是 16（实际 %1）").arg(list->iconSize().width()));

    list->applyAppearance(plain);
    QCoreApplication::processEvents();
    check(list->viewMode() == QListView::ListMode,
          QStringLiteral("★ 三级跳后仍能正确复位到列表"));
    check(list->gridSize().isEmpty(),
          QStringLiteral("★ gridSize 仍被清空"));

    // ---- [D] 反复横跳不应累积问题 ----
    out << "\n[D] 反复切换 5 轮\n";
    bool stable = true;
    for (int i = 0; i < 5; ++i) {
        list->applyAppearance(big);
        QCoreApplication::processEvents();
        if (list->viewMode() != QListView::IconMode) { stable = false; break; }
        list->applyAppearance(plain);
        QCoreApplication::processEvents();
        if (list->viewMode() != QListView::ListMode || !list->gridSize().isEmpty()) {
            stable = false;
            break;
        }
    }
    check(stable, QStringLiteral("★ 反复切换 5 轮后状态仍正确（无累积漂移）"));

    // ---- 收尾 ----
    fw.close();
    QDir(root).removeRecursively();

    out << "\n=== 结果：" << gPass << " 通过 / " << gFail << " 失败 ===\n";
    out.flush();
    return gFail == 0 ? 0 : 1;
}
