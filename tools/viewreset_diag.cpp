#include <QApplication>
#include <QDir>
#include <QFile>
#include <QListView>
#include <QTextStream>

#include "coretypes.h"
#include "itemlistwidget.h"

// ---------------------------------------------------------------------------
// viewreset_diag —— 证实"切回列表不生效"的确切原因。
//
// 假设：ItemListWidget::applyAppearance 里那句默认值早退
//     if (viewMode == List && iconSize == 0) return;
// 本意是保护主窗口的**首次构造**（默认 Options 不该被动任何属性），
// 但它按"值"判断而不是按"状态变化"判断，于是也命中了
// "大图标 -> 列表" 这次切换，导致 applyViewMode 的列表复位分支根本不执行。
//
// 本诊断把三种调用序列并排跑，看哪些能正确复位：
//   A) 直接调 applyViewMode()（绕过早退）
//   B) 走 applyAppearance(LargeIcon) 再 applyAppearance(List)  <- 生产路径
//   C) 走 applyAppearance(LargeIcon) 再 applyAppearance(List, iconSize=16) <- 绕过早退
//
// 若 A、C 能复位而 B 不能，假设成立。
// ---------------------------------------------------------------------------

static void report(QTextStream &out, const QString &tag, ItemListWidget *l)
{
    out << QStringLiteral("    %1: viewMode=%2 gridSize=%3x%4 iconSize=%5x%6\n")
               .arg(tag)
               .arg(int(l->viewMode()))
               .arg(l->gridSize().width()).arg(l->gridSize().height())
               .arg(l->iconSize().width()).arg(l->iconSize().height());
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QTextStream out(stdout);
    out << "=== 切回列表不生效：原因证实 ===\n\n";

    BoxAppearance large;
    large.viewMode = BoxAppearance::ViewMode::LargeIcon;

    // A) 直接调 applyViewMode（模拟"没有早退"的理想行为）
    out << "[A] 先 applyAppearance(Large)，再手动 applyViewMode 的列表分支\n";
    {
        ItemListWidget l;
        l.applyAppearance(large);
        report(out, QStringLiteral("大图标后"), &l);
        // 绕过早退：直接把 options 改回 List 再调 applyViewMode
        // —— 但 applyViewMode 是 private，探针不能直接调。
        // 于是改用 C 的等价手段：给一个非 0 的 iconSize 造出"值不等于默认"。
        out << QStringLiteral("    （applyViewMode 是 private，无法直接调，转 [C]）\n");
    }

    // B) 生产路径：Large -> List（默认值）
    out << "\n[B] 生产路径：applyAppearance(Large) -> applyAppearance(List)\n";
    {
        ItemListWidget l;
        l.applyAppearance(large);
        report(out, QStringLiteral("大图标后  "), &l);

        BoxAppearance backToDefault;    // List + iconSize=0 + opacity=100
        l.applyAppearance(backToDefault);
        report(out, QStringLiteral("切回默认后"), &l);

        const bool reset = (l.viewMode() == QListView::ListMode && l.gridSize().isEmpty());
        out << QStringLiteral("    >>> 是否复位成功: %1\n")
                   .arg(reset ? QStringLiteral("是") : QStringLiteral("否 —— 早退把复位吞了"));
    }

    // C) 绕过早退：List 但 iconSize 非 0
    out << "\n[C] 绕过早退：applyAppearance(Large) -> applyAppearance(List, iconSize=16)\n";
    {
        ItemListWidget l;
        l.applyAppearance(large);
        report(out, QStringLiteral("大图标后  "), &l);

        BoxAppearance listWithSize;
        listWithSize.viewMode = BoxAppearance::ViewMode::List;
        listWithSize.iconSize = 16;     // 非 0 -> 不会命中早退
        l.applyAppearance(listWithSize);
        report(out, QStringLiteral("切回(带size)"), &l);

        const bool reset = (l.viewMode() == QListView::ListMode && l.gridSize().isEmpty());
        out << QStringLiteral("    >>> 是否复位成功: %1\n")
                   .arg(reset ? QStringLiteral("是") : QStringLiteral("否"));
    }

    out << "\n=== 结论 ===\n";
    out << "若 [B] 否而 [C] 是，则早退条件 (List && iconSize==0) 是根因：\n";
    out << "它按 值等于默认 判断，无法区分 从未配置过 与 从图标模式切回来 。\n";
    out.flush();
    return 0;
}
