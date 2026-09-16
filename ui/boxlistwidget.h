#ifndef BOXLISTWIDGET_H
#define BOXLISTWIDGET_H

// ---------------------------------------------------------------------------
// ui/boxlistwidget.h —— 左栏「收纳盒列表」。
//
// 只负责展示与选中：盒子数据一律来自 BoxManager::listBoxes 扫描根目录的结果，
// 本控件既不创建也不删除任何目录（创建归 BoxManager，本控件只发起调用）。
//
// 之所以做成独立控件而不是在主窗口里裸用 QListWidget：空状态提示逻辑
// （"还没有收纳盒"）需要跟着数据变化自动显隐，封装进来后主窗口只调 setBoxes 即可，
// 不必在每次刷新时重复维护提示标签的可见性。
// ---------------------------------------------------------------------------

#include <QListWidget>
#include <QList>

#include "coretypes.h"          // StorageBox 为按值参数/成员，须完整类型

class QLabel;

class BoxListWidget : public QListWidget
{
    Q_OBJECT

public:
    explicit BoxListWidget(QWidget *parent = nullptr);

    // 重建整个列表。会保留"刷新前选中的盒名"并尽量恢复选中，
    // 否则每次收纳完刷新都会把选中态弹回第一项，操作手感很差。
    void setBoxes(const QList<StorageBox> &boxes);

    // 当前选中的盒子。无选中时返回 name 为空串的 StorageBox。
    StorageBox currentBox() const;

    // 按名字选中某盒（用于"新建后自动选中"和"恢复上次使用的盒"）。
    // 找到并选中返回 true，未找到返回 false 且不改变当前选中。
    bool selectBoxByName(const QString &name);

    // 应用全局主题颜色。
    void setTheme(const AppTheme &theme);

signals:
    // 选中项变化时发出，供主窗口联动刷新右栏。
    void boxSelectionChanged(const QString &boxName);

protected:
    // 窗口尺寸变化时让空状态提示重新铺满视口。
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onCurrentItemChanged();

private:
    void updateEmptyHint();

private:
    QLabel *m_emptyHint = nullptr;  // 覆盖在列表上的空状态提示
};

#endif // BOXLISTWIDGET_H
