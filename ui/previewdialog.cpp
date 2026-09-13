#include "previewdialog.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
// 实现说明
//
// * 勾选态存在 QListWidgetItem 的 CheckState 上，而不是另开一个 QList<bool>：
//   让状态跟着条目走，控件被清空重建时不会与外部数组错位。
//
// * 来源列显示「用户桌面 / 公共桌面」：公共桌面的写入在部分机器上会因权限失败，
//   提前让主人看见"这一条来自公共桌面"，失败时就不会觉得莫名其妙。
//
// * 无候选时退化成一个纯提示框（只剩「关闭」）：这种状态下不该出现"确认收纳"，
//   否则点了也没东西可搬，属于空动作按钮。
// ---------------------------------------------------------------------------

PreviewDialog::PreviewDialog(const QList<DesktopEntry> &entries,
                             const QString &boxName,
                             QWidget *parent)
    : QDialog(parent)
    , m_entries(entries)
{
    setWindowTitle(tr("收纳预览"));
    setModal(true);

    if (m_entries.isEmpty()) {
        buildEmptyUi();
    } else {
        buildUi(boxName);
    }

    resize(560, 420);
}

void PreviewDialog::buildUi(const QString &boxName)
{
    auto *layout = new QVBoxLayout(this);

    // 顶部说明：把"不删除任何文件"这句话放在最显眼处，消除主人的顾虑。
    auto *intro = new QLabel(tr("以下 %1 项将从桌面移动到收纳盒「%2」，"
                                "不会删除任何文件。")
                                 .arg(m_entries.size())
                                 .arg(boxName),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    m_list = new QListWidget(this);
    m_list->setAlternatingRowColors(true);
    for (const DesktopEntry &entry : m_entries) {
        const QString sizeText = entry.isDir
                                     ? tr("文件夹")
                                     : QLocale().formattedDataSize(entry.size);
        const QString originText = (entry.origin == EntryOrigin::PublicDesktop)
                                       ? tr("公共桌面")
                                       : tr("用户桌面");

        auto *item = new QListWidgetItem(
            tr("%1     %2    %3").arg(entry.name, sizeText, originText), m_list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);       // 默认全选：多数情况下就是全收
        item->setToolTip(entry.filePath);
    }
    layout->addWidget(m_list, 1);

    m_summary = new QLabel(this);
    layout->addWidget(m_summary);

    // 全选 / 全不选 + 确认 / 取消。确认按钮设为默认，回车即确认。
    auto *btnRow = new QHBoxLayout;
    auto *allBtn  = new QPushButton(tr("全选"), this);
    auto *noneBtn = new QPushButton(tr("全不选"), this);
    btnRow->addWidget(allBtn);
    btnRow->addWidget(noneBtn);
    btnRow->addStretch(1);

    auto *box = new QDialogButtonBox(this);
    m_confirm = box->addButton(tr("确认收纳"), QDialogButtonBox::AcceptRole);
    m_confirm->setDefault(true);
    box->addButton(tr("取消"), QDialogButtonBox::RejectRole);
    btnRow->addWidget(box);
    layout->addLayout(btnRow);

    connect(allBtn,  &QPushButton::clicked, this, &PreviewDialog::selectAll);
    connect(noneBtn, &QPushButton::clicked, this, &PreviewDialog::selectNone);
    connect(box, &QDialogButtonBox::accepted, this, &PreviewDialog::onConfirm);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_list, &QListWidget::itemChanged, this, [this] {
        refreshSummary();
        updateConfirmEnabled();
    });

    refreshSummary();
    updateConfirmEnabled();
}

void PreviewDialog::buildEmptyUi()
{
    auto *layout = new QVBoxLayout(this);

    auto *hint = new QLabel(tr("桌面上没有可收纳的条目。"), this);
    hint->setAlignment(Qt::AlignCenter);
    hint->setStyleSheet(QStringLiteral("color: #5F6368; font-size: 11pt;"));
    layout->addStretch(1);
    layout->addWidget(hint);
    layout->addStretch(1);

    auto *box = new QDialogButtonBox(this);
    box->addButton(tr("关闭"), QDialogButtonBox::RejectRole);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(box);
}

void PreviewDialog::selectAll()
{
    if (!m_list) {
        return;
    }
    const QSignalBlocker blocker(m_list);   // 批量改时先静音，末尾统一刷新一次
    for (int row = 0; row < m_list->count(); ++row) {
        m_list->item(row)->setCheckState(Qt::Checked);
    }
    refreshSummary();
    updateConfirmEnabled();
}

void PreviewDialog::selectNone()
{
    if (!m_list) {
        return;
    }
    const QSignalBlocker blocker(m_list);
    for (int row = 0; row < m_list->count(); ++row) {
        m_list->item(row)->setCheckState(Qt::Unchecked);
    }
    refreshSummary();
    updateConfirmEnabled();
}

void PreviewDialog::refreshSummary()
{
    if (!m_summary) {
        return;
    }
    m_summary->setText(tr("已选择 %1 / %2 项")
                           .arg(selectedEntries().size())
                           .arg(m_entries.size()));
}

void PreviewDialog::updateConfirmEnabled()
{
    if (!m_confirm) {
        return;
    }
    // 一项都没勾时禁止确认，避免"点了确认但什么都没发生"的困惑。
    m_confirm->setEnabled(!selectedEntries().isEmpty());
}

void PreviewDialog::onConfirm()
{
    if (selectedEntries().isEmpty()) {
        return;     // 双保险：按钮理论上已置灰
    }
    accept();
}

QList<DesktopEntry> PreviewDialog::selectedEntries() const
{
    QList<DesktopEntry> picked;
    if (!m_list) {
        return picked;      // 空状态对话框：没有任何可选项
    }

    // 按 m_entries 的顺序取，保证结果稳定可预测（不依赖控件的行序）。
    for (int i = 0; i < m_entries.size(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        if (item && item->checkState() == Qt::Checked) {
            picked << m_entries.at(i);
        }
    }
    return picked;
}
