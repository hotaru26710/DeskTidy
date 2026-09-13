#include "boxlistwidget.h"

#include <QLabel>
#include <QResizeEvent>
#include <QSignalBlocker>

// ---------------------------------------------------------------------------
// 实现说明
//
// * 存储约定：Qt::UserRole 存盒名、Qt::UserRole+1 存盒目录全路径。
//   只存名字的话，主窗口还要自己拼路径，等于把 CoreNames::boxRoot 的规则
//   复制到 UI 层，一旦根目录规则变了就会两处不一致。
//
// * 空状态：提示标签是列表的子控件（不是布局成员），靠手动定位居中覆盖。
//   用 QStackedLayout 会更"正统"，但这里只有"有数据/没数据"两态，
//   引入额外布局栈得不偿失。
// ---------------------------------------------------------------------------

BoxListWidget::BoxListWidget(QWidget *parent)
    : QListWidget(parent)
{
    setAlternatingRowColors(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setUniformItemSizes(false);
    setToolTip(tr("收纳盒只是 %1 下的普通文件夹，\n"
                  "在资源管理器里手工新建或删除也会同步显示。")
                   .arg(QStringLiteral("%USERPROFILE%\\DeskTidy")));

    // 空状态提示：作为子控件浮在列表之上，由 updateEmptyHint 控制显隐。
    m_emptyHint = new QLabel(tr("还没有收纳盒，点上方「新建收纳盒」"), this);
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setWordWrap(true);
    m_emptyHint->setStyleSheet(QStringLiteral("color: #9AA0A6; background: transparent;"));
    m_emptyHint->setAttribute(Qt::WA_TransparentForMouseEvents);  // 别挡住列表的点击
    m_emptyHint->setVisible(false);

    connect(this, &QListWidget::currentItemChanged,
            this, &BoxListWidget::onCurrentItemChanged);
}

void BoxListWidget::setBoxes(const QList<StorageBox> &boxes)
{
    // 记住刷新前的选中项，刷新后尽量恢复 —— 否则每次收纳完右栏都会跳回第一个盒子。
    const QString keepName = currentBox().name;

    // 重建期间屏蔽信号，避免中间态触发一串无意义的联动刷新。
    const QSignalBlocker blocker(this);

    clear();
    for (const StorageBox &box : boxes) {
        // 显示成「盒名 (N)」，N 为盒内条目数，让主人在左栏就能看出哪个盒快满了。
        auto *item = new QListWidgetItem(tr("%1  (%2)").arg(box.name).arg(box.itemCount), this);
        item->setData(Qt::UserRole, box.name);
        item->setData(Qt::UserRole + 1, box.path);
        item->setToolTip(box.path);
    }

    updateEmptyHint();

    // 恢复选中：优先 keepName，其次第一项。
    if (!keepName.isEmpty() && selectBoxByName(keepName)) {
        // selectBoxByName 内部已处理选中态。
    } else if (count() > 0) {
        setCurrentRow(0);
    }

    // 手动补发一次：上面被 blocker 屏蔽了，主窗口需要知道最终选中变成了谁。
    emit boxSelectionChanged(currentBox().name);
}

StorageBox BoxListWidget::currentBox() const
{
    StorageBox box;
    const QListWidgetItem *item = currentItem();
    if (!item) {
        return box;     // name/path 均为空串，调用方据此判断"无选中"
    }
    box.name = item->data(Qt::UserRole).toString();
    box.path = item->data(Qt::UserRole + 1).toString();

    // itemCount 在条目文本里，这里不反解析文本（脆弱）；
    // 调用方若需要计数会用 BoxManager::listBoxes 重新扫描，故此处保持 0。
    return box;
}

bool BoxListWidget::selectBoxByName(const QString &name)
{
    if (name.isEmpty()) {
        return false;
    }
    for (int row = 0; row < count(); ++row) {
        QListWidgetItem *item = this->item(row);
        if (item && item->data(Qt::UserRole).toString() == name) {
            setCurrentItem(item);
            return true;
        }
    }
    return false;
}

void BoxListWidget::onCurrentItemChanged()
{
    emit boxSelectionChanged(currentBox().name);
}

void BoxListWidget::updateEmptyHint()
{
    const bool empty = (count() == 0);
    m_emptyHint->setVisible(empty);
    if (empty) {
        // 覆盖整个视口并居中；列表此时没有条目，不必担心遮挡内容。
        m_emptyHint->setGeometry(rect());
    }
}

void BoxListWidget::resizeEvent(QResizeEvent *event)
{
    QListWidget::resizeEvent(event);
    // 跟随尺寸变化重新铺满，否则窗口拉大后提示会偏在一角。
    if (m_emptyHint && m_emptyHint->isVisible()) {
        m_emptyHint->setGeometry(rect());
    }
}
