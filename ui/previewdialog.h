#ifndef PREVIEWDIALOG_H
#define PREVIEWDIALOG_H

// ---------------------------------------------------------------------------
// ui/previewdialog.h —— 收纳前的预览确认对话框。
//
// 这是"真实移动文件"这件事上唯一的人工闸门：主人点「收纳桌面」后不会立刻搬东西，
// 而是先看到"将要发生什么"并逐条勾选。默认全选（大多数情况就是全收），
// 但保留取消勾选的能力，避免把正在用的文件一并收走。
//
// 对话框内不做任何文件操作，只负责收集"主人同意搬哪些"。
// ---------------------------------------------------------------------------

#include <QDialog>
#include <QList>

#include "coretypes.h"          // DesktopEntry 为按值参数/成员，须完整类型

class QListWidget;
class QLabel;
class QPushButton;

class PreviewDialog : public QDialog
{
    Q_OBJECT

public:
    // entries 为待收纳候选清单；boxName 用于在说明文字里点名去向。
    PreviewDialog(const QList<DesktopEntry> &entries,
                  const QString &boxName,
                  QWidget *parent = nullptr);

    // 主人勾选的条目（顺序与传入顺序一致）。
    QList<DesktopEntry> selectedEntries() const;

private slots:
    void selectAll();
    void selectNone();
    void onConfirm();

private:
    void buildUi(const QString &boxName);
    void buildEmptyUi();
    void refreshSummary();
    void updateConfirmEnabled();

private:
    QList<DesktopEntry> m_entries;      // 全部候选（勾选态存在各 item 的 CheckState 里）
    QListWidget        *m_list    = nullptr;
    QLabel             *m_summary = nullptr;
    QPushButton        *m_confirm = nullptr;
};

#endif // PREVIEWDIALOG_H
