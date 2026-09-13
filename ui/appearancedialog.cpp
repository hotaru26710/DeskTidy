#include "appearancedialog.h"

#include "floatingboxmanager.h"
#include "itemlistwidget.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
// AppearanceDialog 实现。
//
// 三条贯穿全文件的约定：
//
// 1) 【数据流单向】控件 -> m_appearance（collectAppearanceFromControls）
//    与 m_appearance -> 控件（syncControlsFromAppearance）严格分开。
//    两边若互相触发，改一次下拉框会引发一串递归刷新。
//    防抖靠 m_syncing 标志：程序化改控件值期间，onXxxChanged 直接返回。
//
// 2) 【预览区是真的 ItemListWidget，不是画出来的示意图】
//    画示意图意味着"预览的样子"和"浮窗真正的样子"是两份实现，
//    迟早会不一致，而用户正是照着预览做决定的。用真控件就没有这个问题。
//    代价是它得喂假条目 —— 见 buildPreviewEntries。
//
// 3) 【透明度下限卡在 20%】
//    0% 会让浮窗彻底看不见，而主人没有入口把它调回来（连浮窗都点不到），
//    这是不可恢复的故障。滑块物理下限与 BoxAppearance::kMinOpacity 一致，
//    浮窗那边还有第二道兜底（见 floatingboxwidget.cpp 的 applyAppearance）。
// ---------------------------------------------------------------------------

namespace {

// 视图模式下拉框的条目顺序与 BoxAppearance::ViewMode 的枚举值对应。
//
// 这里只存**枚举值**，不存文案 —— 文案在 buildUi 里用 tr() 字面量逐条写。
// 之所以不在这里放一张"枚举 -> 文案"的表：那样在 addItem 时只能写
// tr(variable)，而 lupdate 提取不出非字面量的字符串，日后真的要做多语言时
// 这几个词会漏翻，且没有任何报错提示 —— 属于"当时看着更简洁、将来莫名其妙
// 少几个翻译"的典型陷阱。
//
// 顺序即下拉框顺序，也是 ViewMode 的数值顺序（List / Small / Medium / Large），
// 所以下面的下标转换是安全的。
const BoxAppearance::ViewMode kViewModeOrder[] = {
    BoxAppearance::ViewMode::List,
    BoxAppearance::ViewMode::SmallIcon,
    BoxAppearance::ViewMode::MediumIcon,
    BoxAppearance::ViewMode::LargeIcon,
};

constexpr int kViewModeCount = int(sizeof(kViewModeOrder) / sizeof(kViewModeOrder[0]));

} // namespace

AppearanceDialog::AppearanceDialog(const QList<StorageBox> &boxes,
                                   const QString &initialBoxName,
                                   FloatingBoxManager *floating,
                                   QWidget *parent)
    : QDialog(parent)
    , m_floating(floating)
    , m_boxes(boxes)
{
    setWindowTitle(tr("浮窗外观"));
    resize(480, 520);
    setModal(true);

    // 没有盒可设置时走空状态界面。放在构造里统一判断，
    // 免得每个调用点都要自己先查一遍再决定弹不弹。
    if (m_boxes.isEmpty()) {
        buildEmptyUi();
        return;
    }

    buildUi();

    // 预选盒：优先用调用方指定的，没有就用第一个。
    // 不假设 initialBoxName 一定存在于 m_boxes 里 —— 调用方可能在对话框
    // 构造之前刚好把那个盒删了，此时静默退到第一个而不是弹错。
    int initial = 0;
    if (!initialBoxName.isEmpty()) {
        for (int i = 0; i < m_boxes.size(); ++i) {
            if (m_boxes.at(i).name == initialBoxName) {
                initial = i;
                break;
            }
        }
    }

    // ⚠️ 这一次 setCurrentIndex 必须屏蔽槽函数。
    // 此刻连接已经建好了（连接都在 buildUi 里），不屏蔽就会触发 onBoxChanged ——
    // 而它做的事（读外观、刷新预览）紧接着下面三行又要完整做一遍，
    // 等于构造时白读一次配置、白建一遍预览列表。
    m_syncing = true;
    m_boxCombo->setCurrentIndex(initial);
    m_syncing = false;

    // 读一次当前盒的外观填进界面。用 manager 而不是 Settings：
    // manager 会对"没有专门配置的盒"返回默认外观，语义更完整。
    m_appearance = m_floating ? m_floating->appearanceOf(m_boxes.at(initial).name)
                              : BoxAppearance();
    syncControlsFromAppearance();
    refreshPreview();
    updateButtonsEnabled();
}

QString AppearanceDialog::selectedBoxName() const
{
    const StorageBox box = currentBox();
    return box.name;
}

StorageBox AppearanceDialog::currentBox() const
{
    if (!m_boxCombo)
        return StorageBox();

    const int index = m_boxCombo->currentIndex();
    if (index < 0 || index >= m_boxes.size())
        return StorageBox();

    return m_boxes.at(index);
}

void AppearanceDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);

    // ---- 表单区：选盒 + 显示方式 + 透明度 ----
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_boxCombo = new QComboBox(this);
    for (const StorageBox &box : m_boxes) {
        // 带上条目数，与主窗口左栏的写法一致 —— 主人在两个地方看到的
        // 盒名应当长得一样，否则会怀疑是不是同一个盒。
        m_boxCombo->addItem(tr("%1  (%2)").arg(box.name).arg(box.itemCount));
    }
    form->addRow(tr("收纳盒："), m_boxCombo);

    m_viewCombo = new QComboBox(this);
    // 文案逐条字面量写，保证 lupdate 能提取（见文件头 kViewModeOrder 的说明）。
    // 顺序必须与 kViewModeOrder 严格一致。
    m_viewCombo->addItem(tr("列表"));
    m_viewCombo->addItem(tr("小图标"));
    m_viewCombo->addItem(tr("中图标"));
    m_viewCombo->addItem(tr("大图标"));
    form->addRow(tr("显示方式："), m_viewCombo);

    // 透明度：滑块 + 右侧实时百分比。
    // 外面套一个 QWidget 是为了让"滑块 + 数字"作为一整格放进表单，
    // 直接 addRow 两个控件会让它们分列两行。
    auto *opacityRow    = new QWidget(this);
    auto *opacityLayout = new QHBoxLayout(opacityRow);
    opacityLayout->setContentsMargins(0, 0, 0, 0);

    m_opacitySlider = new QSlider(Qt::Horizontal, opacityRow);
    m_opacitySlider->setRange(BoxAppearance::kMinOpacity, 100);
    m_opacitySlider->setSingleStep(5);
    m_opacitySlider->setPageStep(10);
    m_opacitySlider->setTickPosition(QSlider::TicksBelow);
    m_opacitySlider->setTickInterval(20);

    m_opacityLabel = new QLabel(opacityRow);
    // 固定宽度：不固定的话数字从 100% 变成 85% 时整行会左右抽动。
    m_opacityLabel->setMinimumWidth(48);
    m_opacityLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    opacityLayout->addWidget(m_opacitySlider, 1);
    opacityLayout->addWidget(m_opacityLabel);
    form->addRow(tr("透明度："), opacityRow);

    root->addLayout(form);

    // 说明下限的来历。不写的话主人会疑惑"为什么拉不到底"。
    m_hintLabel = new QLabel(
        tr("透明度最低 20%：再低会让浮窗淡到看不见，届时连调回来的入口都没有。"),
        this);
    m_hintLabel->setWordWrap(true);
    m_hintLabel->setStyleSheet(QStringLiteral("color: #5F6368;"));
    root->addWidget(m_hintLabel);

    // ---- 预览区 ----
    auto *previewBox    = new QGroupBox(tr("预览"), this);
    auto *previewLayout = new QVBoxLayout(previewBox);
    previewLayout->setContentsMargins(8, 8, 8, 8);

    ItemListWidget::Options opts;
    // 预览只是给人看的，双击不该真的去打开什么文件 —— 用 Restore 还是 Open
    // 都无所谓，因为下面会把鼠标事件整个屏蔽掉，双击根本到不了控件。
    opts.doubleClick  = ItemListWidget::DoubleClickAction::Open;
    opts.draggableOut = false;

    m_preview = new ItemListWidget(opts, previewBox);
    // 关键：预览区不接受任何鼠标交互。
    // 它内嵌的是真控件，不屏蔽的话主人能在里面选中、双击、甚至试着拖出条目，
    // 而那些信号本对话框一个都没接 —— 表现就是"点了没反应"，像是坏了。
    m_preview->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_preview->setFocusPolicy(Qt::NoFocus);
    previewLayout->addWidget(m_preview, 1);

    root->addWidget(previewBox, 1);

    // ---- 按钮区 ----
    // 「应用」不关闭对话框：外观是调出来看的，调一档关一次没法连续比较。
    auto *buttons = new QDialogButtonBox(this);
    m_resetBtn = buttons->addButton(tr("恢复默认"), QDialogButtonBox::ResetRole);
    m_applyBtn = buttons->addButton(tr("应用"), QDialogButtonBox::ApplyRole);
    buttons->addButton(tr("关闭"), QDialogButtonBox::RejectRole);
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_resetBtn, &QPushButton::clicked, this, &AppearanceDialog::onResetToDefault);
    connect(m_applyBtn, &QPushButton::clicked, this, &AppearanceDialog::onApply);

    connect(m_boxCombo, &QComboBox::currentIndexChanged,
            this, &AppearanceDialog::onBoxChanged);
    connect(m_viewCombo, &QComboBox::currentIndexChanged,
            this, &AppearanceDialog::onViewModeChanged);
    connect(m_opacitySlider, &QSlider::valueChanged,
            this, &AppearanceDialog::onOpacityChanged);

    // 预览用的假条目只造一次。
    // 刻意不扫真实盒目录：一是盒可能本来就是空的（那时预览会一片空白，
    // 看不出任何效果），二是切下拉框时扫盘会卡。
    m_previewEntries.clear();
    for (const QString &name : {QStringLiteral("示例文档.txt"),
                                QStringLiteral("报告.docx"),
                                QStringLiteral("照片")}) {
        DesktopEntry entry;
        // 指向一个几乎必然存在的目录，让图标至少是"文件夹/通用文件"而不是问号。
        // 指向不存在的路径在部分 Windows 版本上会退化成空白图标。
        entry.filePath = QDir::homePath() + QLatin1Char('/') + name;
        entry.name     = name;
        entry.isDir    = (name == QStringLiteral("照片"));
        entry.size     = 2048;
        entry.modified = QDateTime::currentDateTime();
        m_previewEntries << entry;
    }
}

void AppearanceDialog::buildEmptyUi()
{
    auto *root = new QVBoxLayout(this);

    auto *label = new QLabel(
        tr("还没有收纳盒。\n\n先在控制中心里建一个收纳盒，再回来设置它的浮窗外观。"),
        this);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    root->addWidget(label, 1);

    auto *buttons = new QDialogButtonBox(this);
    buttons->addButton(tr("关闭"), QDialogButtonBox::RejectRole);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // 全禁用状态由"控件都是 nullptr"自然达成 —— 下面的槽函数
    // 都以 m_boxCombo 非空为前提，构造期也不会有人调用它们。
}

// ---------------------------------------------------------------------------
// 控件 <-> 数据
// ---------------------------------------------------------------------------

void AppearanceDialog::syncControlsFromAppearance()
{
    // 程序化改控件值期间屏蔽槽函数，否则 setCurrentIndex 会触发
    // onViewModeChanged，而后者又会去读控件、刷新预览 —— 一圈下来
    // 不但白做工，还会把"正在从配置读入"这件事搞得难以追踪。
    m_syncing = true;

    if (m_viewCombo) {
        for (int i = 0; i < kViewModeCount; ++i) {
            if (kViewModeOrder[i] == m_appearance.viewMode) {
                m_viewCombo->setCurrentIndex(i);
                break;
            }
        }
    }

    if (m_opacitySlider) {
        // 兜一道下限：配置若被手工改成 0（ini 是可以手编的），
        // 界面要显示 20 而不是 0 —— 否则滑块会停在物理下限、
        // 数字却写着 0%，看着像坏了。
        const int value = qBound(BoxAppearance::kMinOpacity, m_appearance.opacity, 100);
        m_opacitySlider->setValue(value);
        m_opacityLabel->setText(tr("%1%").arg(value));
    }

    m_syncing = false;
}

void AppearanceDialog::collectAppearanceFromControls()
{
    if (m_viewCombo) {
        const int index = m_viewCombo->currentIndex();
        if (index >= 0 && index < kViewModeCount)
            m_appearance.viewMode = kViewModeOrder[index];
    }

    if (m_opacitySlider) {
        m_appearance.opacity = m_opacitySlider->value();
    }

    // iconSize 保持 0（跟随 viewMode 推导）。
    // 本对话框不暴露"自定义像素"，所以不去动它 —— 若主人之前用别的方式
    // 设过显式值，这里强行清零会悄悄改掉他的设置。
}

// ---------------------------------------------------------------------------
// 槽函数
// ---------------------------------------------------------------------------

void AppearanceDialog::onBoxChanged(int index)
{
    if (m_syncing || !m_floating)
        return;
    if (index < 0 || index >= m_boxes.size())
        return;

    // 切盒时**丢弃未应用的改动**，直接读新盒的已存外观。
    //
    // 为什么不做"先把改动应用到旧盒再切"：那会让"切过去看一眼"变成
    // 一次静默的写入，主人只是想比较两个盒，却把前一个盒改掉了。
    // 想生效就点「应用」，这是明确的手势。
    m_appearance = m_floating->appearanceOf(m_boxes.at(index).name);
    syncControlsFromAppearance();
    refreshPreview();
    updateButtonsEnabled();
}

void AppearanceDialog::onViewModeChanged()
{
    if (m_syncing)
        return;

    collectAppearanceFromControls();
    refreshPreview();
    updateButtonsEnabled();
}

void AppearanceDialog::onOpacityChanged(int value)
{
    if (m_syncing)
        return;

    if (m_opacityLabel)
        m_opacityLabel->setText(tr("%1%").arg(value));

    collectAppearanceFromControls();
    refreshPreview();
    updateButtonsEnabled();
}

void AppearanceDialog::onApply()
{
    const StorageBox box = currentBox();
    if (box.name.isEmpty() || !m_floating)
        return;

    collectAppearanceFromControls();

    // 交给 manager：它写 Settings 并发 boxAppearanceChanged，
    // 于是已经开着的浮窗会立刻跟着变。
    m_floating->applyAppearance(box.name, m_appearance);

    // 反馈：按钮短暂变文字再变回来。
    // 不做这个的话点了"应用"界面毫无动静，主人会怀疑到底生效没有
    // —— 尤其是浮窗此时正被别的窗口盖住、看不到变化的时候。
    //
    // ⚠️ 顺序：先置 m_justApplied，再调 updateButtonsEnabled。
    // 反过来的话 updateButtonsEnabled 会把按钮立刻重新启用，
    // "已应用"还没显示完就被撤掉了（按钮亮着但字写着已应用，很怪）。
    if (m_applyBtn) {
        m_justApplied = true;
        m_applyBtn->setText(tr("已应用"));
        QTimer::singleShot(900, this, [this]() {
            if (!m_applyBtn)
                return;
            m_justApplied = false;
            m_applyBtn->setText(tr("应用"));
            // 到点后再统一算一次可用性，把这段时间里可能发生的
            // 切盒 / 恢复默认一并考虑进来。
            updateButtonsEnabled();
        });
    }

    updateButtonsEnabled();
}

void AppearanceDialog::onResetToDefault()
{
    // 恢复默认 = 默认构造一份（列表 + 不透明）。
    // 注意这里**不动控件以外的任何东西**，也不立刻写入配置 ——
    // 仍然要主人点「应用」才落盘，与其它改动保持同一套手势。
    // 否则会出现"我只想看看默认长什么样，结果直接改掉了"。
    m_appearance = BoxAppearance();

    syncControlsFromAppearance();
    refreshPreview();
    updateButtonsEnabled();
}

// ---------------------------------------------------------------------------
// 预览与按钮状态
// ---------------------------------------------------------------------------

void AppearanceDialog::refreshPreview()
{
    if (!m_preview)
        return;

    // 顺序要紧：**先喂条目，再套外观**。
    //
    // ItemListWidget::applyViewMode 切到图标模式时会重建列表项（图标尺寸变了
    // 必须重建，否则看到的是把小图拉大的糊图），而重建的数据来自它自己缓存的
    // m_lastItems —— 那个缓存只有 setItems 才会填。
    //
    // 若反过来先套外观，第一次调用时缓存还是空的，重建落空；虽然紧接着的
    // setItems 会把内容补上、最终结果正确，但白跑一趟重建。
    // 先喂条目就没有这个空转，意图也更直白：内容先有，外观后套。
    m_preview->setItems(m_previewEntries);
    m_preview->applyAppearance(m_appearance);

    // 透明度：预览区本身保持不透明（它嵌在对话框里，变淡了反而看不清），
    // 所以只用文字提示当前档位，不做视觉模拟。
    // 真实的透明度效果请点「应用」后在桌面浮窗上看 —— 那才是所见即所得。
}

void AppearanceDialog::updateButtonsEnabled()
{
    const StorageBox box = currentBox();
    const bool hasBox = !box.name.isEmpty();

    if (m_resetBtn) {
        // 已经是默认外观时禁用：点了也没变化，留着会让人以为按钮坏了。
        m_resetBtn->setEnabled(hasBox && !m_appearance.isDefault());
    }

    if (m_applyBtn) {
        // 刚点过应用的那 900ms 内保持禁用，让"已应用"这段反馈能完整显示出来。
        // 不禁用的话主人会连点好几下，每次都是无害的重复写入 ——
        // 但看起来会像是"点了没反应，所以我再点一下试试"。
        m_applyBtn->setEnabled(hasBox && !m_justApplied);
    }
}
