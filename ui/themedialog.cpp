#include "themedialog.h"

#include "floatingboxmanager.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace {

const BoxAppearance::ViewMode kViewModeOrder[] = {
    BoxAppearance::ViewMode::List,
    BoxAppearance::ViewMode::SmallIcon,
    BoxAppearance::ViewMode::MediumIcon,
    BoxAppearance::ViewMode::LargeIcon,
};

const BoxAppearance::FeedbackStrength kStrengthOrder[] = {
    BoxAppearance::FeedbackStrength::Subtle,
    BoxAppearance::FeedbackStrength::Standard,
    BoxAppearance::FeedbackStrength::Strong,
};

const BoxAppearance::AnimationSpeed kSpeedOrder[] = {
    BoxAppearance::AnimationSpeed::Relaxed,
    BoxAppearance::AnimationSpeed::Standard,
    BoxAppearance::AnimationSpeed::Fast,
};

template <typename T, size_t N>
int indexOfPreset(const T (&values)[N], T needle)
{
    for (size_t i = 0; i < N; ++i) {
        if (values[i] == needle)
            return int(i);
    }
    return 0;
}

bool appearanceEqual(const BoxAppearance &a, const BoxAppearance &b)
{
    return a.viewMode == b.viewMode
           && a.iconSize == b.iconSize
           && a.opacity == b.opacity
           && a.hoverEffect == b.hoverEffect
           && a.feedbackStrength == b.feedbackStrength
           && a.animationSpeed == b.animationSpeed
           && a.hoverExpandDelayMs == b.hoverExpandDelayMs
           && a.hoverCollapseDelayMs == b.hoverCollapseDelayMs
           && a.cornerRadius == b.cornerRadius
           && a.cornerSmoothing == b.cornerSmoothing;
}

bool themeEqual(const AppTheme &a, const AppTheme &b)
{
    return a.windowBackground == b.windowBackground
           && a.surface == b.surface
           && a.titleBar == b.titleBar
           && a.text == b.text
           && a.mutedText == b.mutedText
           && a.border == b.border
           && a.hover == b.hover
           && a.pressed == b.pressed
           && a.primary == b.primary
           && a.primaryHover == b.primaryHover
           && a.primaryPressed == b.primaryPressed
           && a.onPrimary == b.onPrimary
           && a.danger == b.danger
           && a.dangerPressed == b.dangerPressed
           && a.windowOpacity == b.windowOpacity
           && appearanceEqual(a.floatDefaults, b.floatDefaults);
}

QString colorButtonStyle(const QColor &color)
{
    const QString foreground = color.lightness() > 150
                                   ? QStringLiteral("#202124")
                                   : QStringLiteral("#FFFFFF");
    return QStringLiteral("QPushButton {"
                          "background-color: %1;"
                          "color: %2;"
                          "border: 1px solid #9AA0A6;"
                          "border-radius: 5px;"
                          "padding: 5px 10px;"
                          "font-weight: 600;"
                          "}"
                          "QPushButton:hover { border-color: %2; }")
        .arg(color.name(QColor::HexRgb), foreground);
}

QPainterPath roundedPath(const QRectF &rect, qreal radius, int smoothing)
{
    QPainterPath path;
    const qreal maxRadius = std::min(rect.width(), rect.height()) / 2.0;
    radius = std::max<qreal>(0.0, std::min(radius, maxRadius));

    if (radius <= 0.0) {
        path.addRect(rect);
        return path;
    }

    if (smoothing <= 0) {
        path.addRoundedRect(rect, radius, radius);
        return path;
    }

    // 标准档接近圆角矩形的常见三次曲线；平滑档把控制点继续推向外侧，
    // 让预览能清楚看出两种圆角手感不同。
    const qreal k = smoothing == 1 ? 0.55228475 : 0.78;
    const qreal cx = radius * k;
    const qreal cy = radius * k;

    path.moveTo(rect.left() + radius, rect.top());
    path.lineTo(rect.right() - radius, rect.top());
    path.cubicTo(rect.right() - radius + cx, rect.top(),
                 rect.right(), rect.top() + radius - cy,
                 rect.right(), rect.top() + radius);
    path.lineTo(rect.right(), rect.bottom() - radius);
    path.cubicTo(rect.right(), rect.bottom() - radius + cy,
                 rect.right() - radius + cx, rect.bottom(),
                 rect.right() - radius, rect.bottom());
    path.lineTo(rect.left() + radius, rect.bottom());
    path.cubicTo(rect.left() + radius - cx, rect.bottom(),
                 rect.left(), rect.bottom() - radius + cy,
                 rect.left(), rect.bottom() - radius);
    path.lineTo(rect.left(), rect.top() + radius);
    path.cubicTo(rect.left(), rect.top() + radius - cy,
                 rect.left() + radius - cx, rect.top(),
                 rect.left() + radius, rect.top());
    path.closeSubpath();
    return path;
}

} // namespace

class ThemePreview final : public QWidget
{
public:
    explicit ThemePreview(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(190);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setTheme(const AppTheme &theme)
    {
        m_theme = theme;
        m_theme.normalize();
        update();
    }

    QSize sizeHint() const override
    {
        return QSize(540, 210);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), m_theme.windowBackground);

        const QRectF outer = QRectF(rect()).adjusted(10, 10, -10, -10);
        const QPainterPath shell = roundedPath(
            outer, m_theme.floatDefaults.cornerRadius + 4,
            m_theme.floatDefaults.cornerSmoothing);

        painter.fillPath(shell, m_theme.surface);
        painter.setPen(QPen(m_theme.border, 1.0));
        painter.drawPath(shell);

        const QRectF titleRect(outer.left() + 1.0, outer.top() + 1.0,
                               outer.width() - 2.0, 34.0);
        painter.save();
        painter.setClipPath(shell);
        painter.fillRect(titleRect, m_theme.titleBar);
        painter.restore();
        painter.setPen(m_theme.text);
        painter.drawText(titleRect.adjusted(12, 0, -12, 0),
                         Qt::AlignVCenter | Qt::AlignLeft,
                         tr("中控预览 · 窗口透明度 %1%").arg(m_theme.windowOpacity));

        const QRectF bodyRect(outer.left() + 18.0, outer.top() + 52.0,
                              std::max<qreal>(120.0, outer.width() - 260.0),
                              82.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_theme.hover);
        painter.drawRoundedRect(bodyRect, 6, 6);
        painter.setPen(m_theme.text);
        painter.drawText(bodyRect.adjusted(14, 10, -14, 0),
                         Qt::AlignTop | Qt::AlignLeft, tr("内容区域"));

        painter.setPen(m_theme.mutedText);
        painter.drawText(bodyRect.adjusted(14, 34, -14, 0),
                         Qt::AlignTop | Qt::AlignLeft,
                         tr("文字、边框和按钮会使用当前配色"));

        const QRectF primaryRect(bodyRect.left(), bodyRect.bottom() + 14.0, 92.0, 30.0);
        const QPainterPath primaryPath = roundedPath(primaryRect, 6, 1);
        painter.fillPath(primaryPath, m_theme.primary);
        painter.setPen(m_theme.onPrimary);
        painter.drawText(primaryRect, Qt::AlignCenter, tr("主色按钮"));

        const qreal floatWidth = std::min<qreal>(210.0, outer.width() * 0.38);
        const QRectF floatRect(outer.right() - floatWidth - 20.0,
                               outer.top() + 56.0, floatWidth, 104.0);
        QColor floatSurface = m_theme.surface;
        floatSurface.setAlphaF(std::clamp(m_theme.floatDefaults.opacity / 100.0, 0.2, 1.0));
        const QPainterPath floatPath = roundedPath(
            floatRect, m_theme.floatDefaults.cornerRadius,
            m_theme.floatDefaults.cornerSmoothing);
        painter.fillPath(floatPath, floatSurface);
        painter.setPen(QPen(m_theme.border, 1.0));
        painter.drawPath(floatPath);

        painter.setPen(m_theme.text);
        painter.drawText(floatRect.adjusted(12, 10, -12, 0),
                         Qt::AlignTop | Qt::AlignLeft, tr("全局浮窗"));
        painter.setPen(m_theme.mutedText);
        painter.drawText(floatRect.adjusted(12, 36, -12, 0),
                         Qt::AlignTop | Qt::AlignLeft,
                         tr("透明度 %1%").arg(m_theme.floatDefaults.opacity));
        painter.drawText(floatRect.adjusted(12, 58, -12, 0),
                         Qt::AlignTop | Qt::AlignLeft,
                         tr("圆角 %1 px · 平滑 %2")
                             .arg(m_theme.floatDefaults.cornerRadius)
                             .arg(m_theme.floatDefaults.cornerSmoothing));
    }

private:
    AppTheme m_theme;
};

ThemeDialog::ThemeDialog(const AppTheme &current,
                         FloatingBoxManager *floating,
                         QWidget *parent)
    : QDialog(parent)
    , m_floating(floating)
    , m_theme(current)
    , m_appliedTheme(current)
{
    setWindowTitle(tr("主题设置"));
    setModal(true);
    setSizeGripEnabled(true);
    resize(660, 800);

    m_theme.normalize();
    m_appliedTheme.normalize();

    buildUi();
    syncControlsFromTheme();
}

AppTheme ThemeDialog::theme() const
{
    return m_theme;
}

AppTheme ThemeDialog::editedTheme() const
{
    return m_theme;
}

bool ThemeDialog::changed() const
{
    return !themeEqual(m_theme, m_appliedTheme);
}

void ThemeDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);

    auto *scopeHint = new QLabel(
        tr("这里改的是全局外观，会影响所有没有单独设置过外观的浮窗。\n"
           "如果只想改某一个浮窗，请在桌面的浮窗上点右键。"), this);
    scopeHint->setWordWrap(true);
    scopeHint->setStyleSheet(QStringLiteral("font-weight: 600; padding: 4px 2px;"));
    root->addWidget(scopeHint);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    root->addWidget(scroll, 1);

    auto *content = new QWidget(scroll);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(12);
    scroll->setWidget(content);

    struct ColorFieldDefinition
    {
        QString label;
        QColor AppTheme::*field;
        QString tooltip;
    };

    const QVector<ColorFieldDefinition> colorFields = {
        { tr("窗口背景"), &AppTheme::windowBackground, tr("整个中控窗口最底层的颜色") },
        { tr("卡片颜色"), &AppTheme::surface, tr("按钮、列表和设置卡片的主要内容颜色") },
        { tr("标题栏"), &AppTheme::titleBar, tr("中控和浮窗顶部标题栏的颜色") },
        { tr("文字"), &AppTheme::text, tr("正文和主要按钮上的文字颜色") },
        { tr("提示文字"), &AppTheme::mutedText, tr("说明、状态和次要信息的颜色") },
        { tr("分隔线"), &AppTheme::border, tr("卡片边框和区域之间的分隔线颜色") },
        { tr("鼠标经过"), &AppTheme::hover, tr("鼠标经过按钮或列表项时的颜色") },
        { tr("点击时"), &AppTheme::pressed, tr("按钮或列表项被按下时的颜色") },
        { tr("强调色"), &AppTheme::primary, tr("主要按钮、选中状态和重点提示的颜色") },
        { tr("强调色·鼠标经过"), &AppTheme::primaryHover, tr("强调色按钮在鼠标经过时更深或更亮") },
        { tr("强调色·点击时"), &AppTheme::primaryPressed, tr("强调色按钮被按下时的颜色") },
        { tr("强调色上的文字"), &AppTheme::onPrimary, tr("放在强调色区域上的文字颜色") },
        { tr("删除/警告"), &AppTheme::danger, tr("删除、恢复失败等需要留意的颜色") },
        { tr("删除/警告·点击时"), &AppTheme::dangerPressed, tr("删除/警告按钮被按下时的颜色") },
    };

    auto *colorGroup = new QGroupBox(tr("界面颜色"), content);
    auto *colorGrid = new QGridLayout(colorGroup);
    colorGrid->setHorizontalSpacing(10);
    colorGrid->setVerticalSpacing(8);
    colorGrid->setColumnStretch(1, 1);
    colorGrid->setColumnStretch(3, 1);

    for (int i = 0; i < colorFields.size(); ++i) {
        const ColorFieldDefinition &field = colorFields.at(i);
        const int pair = i % 2;
        const int row = i / 2;
        const int column = pair * 2;

        auto *label = new QLabel(field.label + tr("："), colorGroup);
        auto *button = new QPushButton(colorGroup);
        button->setMinimumWidth(110);
        button->setToolTip(field.tooltip);
        connect(button, &QPushButton::clicked, this,
                [this, field]() { editColor(field.field); });

        colorGrid->addWidget(label, row, column);
        colorGrid->addWidget(button, row, column + 1);
        m_colorButtons.append({ field.field, button });
    }

    auto *windowOpacityRow = new QWidget(colorGroup);
    auto *windowOpacityLayout = new QHBoxLayout(windowOpacityRow);
    windowOpacityLayout->setContentsMargins(0, 0, 0, 0);
    m_windowOpacitySlider = new QSlider(Qt::Horizontal, windowOpacityRow);
    m_windowOpacitySlider->setRange(40, 100);
    m_windowOpacitySlider->setSingleStep(5);
    m_windowOpacitySlider->setPageStep(10);
    m_windowOpacitySlider->setTickPosition(QSlider::TicksBelow);
    m_windowOpacitySlider->setTickInterval(20);
    m_windowOpacitySlider->setToolTip(tr("中控主窗口整体透明度，最低 40%"));
    m_windowOpacityLabel = new QLabel(windowOpacityRow);
    m_windowOpacityLabel->setMinimumWidth(48);
    m_windowOpacityLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    windowOpacityLayout->addWidget(m_windowOpacitySlider, 1);
    windowOpacityLayout->addWidget(m_windowOpacityLabel);

    const int windowOpacityRowIndex = colorGrid->rowCount();
    colorGrid->addWidget(new QLabel(tr("窗口透明度："), colorGroup),
                         windowOpacityRowIndex, 0);
    colorGrid->addWidget(windowOpacityRow, windowOpacityRowIndex, 1, 1, 3);
    contentLayout->addWidget(colorGroup);

    auto *floatGroup = new QGroupBox(tr("全局浮窗默认外观"), content);
    auto *floatLayout = new QVBoxLayout(floatGroup);
    auto *floatHint = new QLabel(
        tr("这些选项只改变尚未单独设置外观的浮窗；已经在浮窗右键中保存过外观的盒子会继续使用自己的设置。"),
        floatGroup);
    floatHint->setWordWrap(true);
    floatLayout->addWidget(floatHint);

    auto *floatForm = new QFormLayout;
    floatForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    floatLayout->addLayout(floatForm);

    m_viewCombo = new QComboBox(floatGroup);
    m_viewCombo->addItem(tr("列表"));
    m_viewCombo->addItem(tr("小图标"));
    m_viewCombo->addItem(tr("中图标"));
    m_viewCombo->addItem(tr("大图标"));
    floatForm->addRow(tr("显示方式："), m_viewCombo);

    auto *iconRow = new QWidget(floatGroup);
    auto *iconLayout = new QHBoxLayout(iconRow);
    iconLayout->setContentsMargins(0, 0, 0, 0);
    m_followIconSizeCheck = new QCheckBox(tr("跟随显示方式"), iconRow);
    m_iconSizeSpin = new QSpinBox(iconRow);
    m_iconSizeSpin->setRange(8, 512);
    m_iconSizeSpin->setSuffix(tr(" px"));
    iconLayout->addWidget(m_followIconSizeCheck);
    iconLayout->addWidget(m_iconSizeSpin, 1);
    floatForm->addRow(tr("图标大小："), iconRow);

    auto *opacityRow = new QWidget(floatGroup);
    auto *opacityLayout = new QHBoxLayout(opacityRow);
    opacityLayout->setContentsMargins(0, 0, 0, 0);
    m_opacitySlider = new QSlider(Qt::Horizontal, opacityRow);
    m_opacitySlider->setRange(BoxAppearance::kMinOpacity, 100);
    m_opacitySlider->setSingleStep(5);
    m_opacitySlider->setPageStep(10);
    m_opacitySlider->setTickPosition(QSlider::TicksBelow);
    m_opacitySlider->setTickInterval(20);
    m_opacityLabel = new QLabel(opacityRow);
    m_opacityLabel->setMinimumWidth(48);
    m_opacityLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    opacityLayout->addWidget(m_opacitySlider, 1);
    opacityLayout->addWidget(m_opacityLabel);
    floatForm->addRow(tr("浮窗透明度："), opacityRow);

    m_hoverEffectCombo = new QComboBox(floatGroup);
    m_hoverEffectCombo->addItem(tr("关闭"));
    m_hoverEffectCombo->addItem(tr("光影描边"));
    floatForm->addRow(tr("悬停触感："), m_hoverEffectCombo);

    m_strengthCombo = new QComboBox(floatGroup);
    m_strengthCombo->addItem(tr("轻微"));
    m_strengthCombo->addItem(tr("标准"));
    m_strengthCombo->addItem(tr("明显"));
    floatForm->addRow(tr("反馈强度："), m_strengthCombo);

    m_speedCombo = new QComboBox(floatGroup);
    m_speedCombo->addItem(tr("舒缓"));
    m_speedCombo->addItem(tr("标准"));
    m_speedCombo->addItem(tr("快速"));
    floatForm->addRow(tr("动画速度："), m_speedCombo);

    m_expandDelayCombo = new QComboBox(floatGroup);
    for (const int ms : BoxAppearance::kHoverExpandDelaysMs)
        m_expandDelayCombo->addItem(tr("%1 ms").arg(ms));
    floatForm->addRow(tr("展开延迟："), m_expandDelayCombo);

    m_collapseDelayCombo = new QComboBox(floatGroup);
    for (const int ms : BoxAppearance::kHoverCollapseDelaysMs)
        m_collapseDelayCombo->addItem(tr("%1 ms").arg(ms));
    floatForm->addRow(tr("收起延迟："), m_collapseDelayCombo);

    m_cornerCombo = new QComboBox(floatGroup);
    for (const int px : BoxAppearance::kCornerRadii)
        m_cornerCombo->addItem(tr("%1 px").arg(px));
    floatForm->addRow(tr("圆角大小："), m_cornerCombo);

    m_cornerSmoothingCombo = new QComboBox(floatGroup);
    m_cornerSmoothingCombo->addItem(tr("锐利"));
    m_cornerSmoothingCombo->addItem(tr("标准"));
    m_cornerSmoothingCombo->addItem(tr("更平滑"));
    floatForm->addRow(tr("圆角平滑度："), m_cornerSmoothingCombo);

    contentLayout->addWidget(floatGroup);

    auto *previewGroup = new QGroupBox(tr("实时预览"), content);
    auto *previewLayout = new QVBoxLayout(previewGroup);
    m_preview = new ThemePreview(previewGroup);
    previewLayout->addWidget(m_preview);
    contentLayout->addWidget(previewGroup);
    contentLayout->addStretch(1);

    auto *buttons = new QDialogButtonBox(this);
    m_resetBtn = buttons->addButton(tr("恢复默认"), QDialogButtonBox::ResetRole);
    m_applyBtn = buttons->addButton(tr("应用"), QDialogButtonBox::ApplyRole);
    m_saveCloseBtn = buttons->addButton(tr("保存并关闭"), QDialogButtonBox::AcceptRole);
    buttons->addButton(tr("取消"), QDialogButtonBox::RejectRole);
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_resetBtn, &QPushButton::clicked, this, &ThemeDialog::onResetToDefault);
    connect(m_applyBtn, &QPushButton::clicked, this, &ThemeDialog::onApply);
    connect(m_saveCloseBtn, &QPushButton::clicked, this, &ThemeDialog::onSaveAndClose);

    connect(m_viewCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { onControlsChanged(); });
    connect(m_followIconSizeCheck, &QCheckBox::toggled,
            this, [this](bool follow) {
                if (m_iconSizeSpin)
                    m_iconSizeSpin->setEnabled(!follow);
                onControlsChanged();
            });
    connect(m_iconSizeSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int) { onControlsChanged(); });
    connect(m_opacitySlider, &QSlider::valueChanged,
            this, [this](int) { onControlsChanged(); });
    connect(m_windowOpacitySlider, &QSlider::valueChanged,
            this, [this](int) { onControlsChanged(); });
    connect(m_hoverEffectCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { onControlsChanged(); });
    connect(m_strengthCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { onControlsChanged(); });
    connect(m_speedCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { onControlsChanged(); });
    connect(m_expandDelayCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { onControlsChanged(); });
    connect(m_collapseDelayCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { onControlsChanged(); });
    connect(m_cornerCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { onControlsChanged(); });
    connect(m_cornerSmoothingCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { onControlsChanged(); });
}

void ThemeDialog::syncControlsFromTheme()
{
    m_syncing = true;

    updateColorButtons();

    if (m_viewCombo) {
        m_viewCombo->setCurrentIndex(
            indexOfPreset(kViewModeOrder, m_theme.floatDefaults.viewMode));
    }

    if (m_followIconSizeCheck && m_iconSizeSpin) {
        const bool follow = m_theme.floatDefaults.iconSize <= 0;
        m_followIconSizeCheck->setChecked(follow);
        m_iconSizeSpin->setEnabled(!follow);
        m_iconSizeSpin->setValue(follow ? m_theme.floatDefaults.effectiveIconSize()
                                        : m_theme.floatDefaults.iconSize);
    }

    if (m_windowOpacitySlider)
        m_windowOpacitySlider->setValue(qBound(40, m_theme.windowOpacity, 100));

    if (m_opacitySlider) {
        m_opacitySlider->setValue(qBound(BoxAppearance::kMinOpacity,
                                         m_theme.floatDefaults.opacity, 100));
    }

    if (m_hoverEffectCombo) {
        m_hoverEffectCombo->setCurrentIndex(
            m_theme.floatDefaults.hoverEffect == BoxAppearance::HoverEffect::Glow ? 1 : 0);
    }

    if (m_strengthCombo) {
        m_strengthCombo->setCurrentIndex(
            indexOfPreset(kStrengthOrder, m_theme.floatDefaults.feedbackStrength));
    }
    if (m_speedCombo) {
        m_speedCombo->setCurrentIndex(
            indexOfPreset(kSpeedOrder, m_theme.floatDefaults.animationSpeed));
    }
    if (m_expandDelayCombo) {
        m_expandDelayCombo->setCurrentIndex(
            indexOfPreset(BoxAppearance::kHoverExpandDelaysMs,
                          m_theme.floatDefaults.hoverExpandDelayMs));
    }
    if (m_collapseDelayCombo) {
        m_collapseDelayCombo->setCurrentIndex(
            indexOfPreset(BoxAppearance::kHoverCollapseDelaysMs,
                          m_theme.floatDefaults.hoverCollapseDelayMs));
    }
    if (m_cornerCombo) {
        m_cornerCombo->setCurrentIndex(
            indexOfPreset(BoxAppearance::kCornerRadii,
                          m_theme.floatDefaults.cornerRadius));
    }
    if (m_cornerSmoothingCombo) {
        m_cornerSmoothingCombo->setCurrentIndex(
            indexOfPreset(BoxAppearance::kCornerSmoothings,
                          m_theme.floatDefaults.cornerSmoothing));
    }

    m_syncing = false;
    updateControlState();
    refreshPreview();
}

void ThemeDialog::collectThemeFromControls()
{
    if (m_windowOpacitySlider)
        m_theme.windowOpacity = m_windowOpacitySlider->value();

    auto &appearance = m_theme.floatDefaults;

    if (m_viewCombo) {
        const int index = m_viewCombo->currentIndex();
        if (index >= 0 && index < int(sizeof(kViewModeOrder) / sizeof(kViewModeOrder[0])))
            appearance.viewMode = kViewModeOrder[index];
    }

    if (m_followIconSizeCheck && m_iconSizeSpin) {
        appearance.iconSize = m_followIconSizeCheck->isChecked()
                                  ? 0
                                  : m_iconSizeSpin->value();
    }

    if (m_opacitySlider)
        appearance.opacity = m_opacitySlider->value();

    if (m_hoverEffectCombo) {
        appearance.hoverEffect = m_hoverEffectCombo->currentIndex() == 0
                                     ? BoxAppearance::HoverEffect::Off
                                     : BoxAppearance::HoverEffect::Glow;
    }

    if (m_strengthCombo) {
        const int index = m_strengthCombo->currentIndex();
        if (index >= 0 && index < int(sizeof(kStrengthOrder) / sizeof(kStrengthOrder[0])))
            appearance.feedbackStrength = kStrengthOrder[index];
    }

    if (m_speedCombo) {
        const int index = m_speedCombo->currentIndex();
        if (index >= 0 && index < int(sizeof(kSpeedOrder) / sizeof(kSpeedOrder[0])))
            appearance.animationSpeed = kSpeedOrder[index];
    }

    if (m_expandDelayCombo) {
        const int index = m_expandDelayCombo->currentIndex();
        if (index >= 0 && index < int(sizeof(BoxAppearance::kHoverExpandDelaysMs)
                                      / sizeof(BoxAppearance::kHoverExpandDelaysMs[0])))
            appearance.hoverExpandDelayMs = BoxAppearance::kHoverExpandDelaysMs[index];
    }

    if (m_collapseDelayCombo) {
        const int index = m_collapseDelayCombo->currentIndex();
        if (index >= 0 && index < int(sizeof(BoxAppearance::kHoverCollapseDelaysMs)
                                      / sizeof(BoxAppearance::kHoverCollapseDelaysMs[0])))
            appearance.hoverCollapseDelayMs = BoxAppearance::kHoverCollapseDelaysMs[index];
    }

    if (m_cornerCombo) {
        const int index = m_cornerCombo->currentIndex();
        if (index >= 0 && index < int(sizeof(BoxAppearance::kCornerRadii)
                                      / sizeof(BoxAppearance::kCornerRadii[0])))
            appearance.cornerRadius = BoxAppearance::kCornerRadii[index];
    }

    if (m_cornerSmoothingCombo) {
        const int index = m_cornerSmoothingCombo->currentIndex();
        if (index >= 0 && index < int(sizeof(BoxAppearance::kCornerSmoothings)
                                      / sizeof(BoxAppearance::kCornerSmoothings[0])))
            appearance.cornerSmoothing = BoxAppearance::kCornerSmoothings[index];
    }

    m_theme.normalize();
}

void ThemeDialog::updateColorButtons()
{
    for (const ColorButton &entry : m_colorButtons) {
        if (!entry.button || !entry.field)
            continue;

        const QColor color = m_theme.*(entry.field);
        entry.button->setText(color.name(QColor::HexRgb).toUpper());
        entry.button->setStyleSheet(colorButtonStyle(color));
    }
}

void ThemeDialog::updateControlState()
{
    if (m_windowOpacityLabel) {
        m_windowOpacityLabel->setText(
            tr("%1%").arg(m_windowOpacitySlider ? m_windowOpacitySlider->value()
                                                  : m_theme.windowOpacity));
    }

    if (m_opacityLabel) {
        m_opacityLabel->setText(
            tr("%1%").arg(m_opacitySlider ? m_opacitySlider->value()
                                          : m_theme.floatDefaults.opacity));
    }

    const bool glowEnabled = m_theme.floatDefaults.hoverEffect
                             == BoxAppearance::HoverEffect::Glow;
    if (m_strengthCombo)
        m_strengthCombo->setEnabled(glowEnabled);

    const bool dirty = changed();
    if (m_applyBtn) {
        m_applyBtn->setEnabled(m_floating && dirty);
        if (dirty)
            m_applyBtn->setText(tr("应用"));
    }
    if (m_saveCloseBtn)
        m_saveCloseBtn->setEnabled(m_floating != nullptr);
    if (m_resetBtn)
        m_resetBtn->setEnabled(!m_theme.isDefault());
}

void ThemeDialog::refreshPreview()
{
    if (m_preview)
        m_preview->setTheme(m_theme);
}

void ThemeDialog::editColor(QColor AppTheme::*field)
{
    if (!field)
        return;

    const QColor chosen = QColorDialog::getColor(m_theme.*field, this, tr("选择颜色"));
    if (!chosen.isValid())
        return;

    m_theme.*field = chosen;
    updateColorButtons();
    onControlsChanged();
}

bool ThemeDialog::applyChanges()
{
    return applyTheme();
}

bool ThemeDialog::applyTheme()
{
    collectThemeFromControls();

    if (!m_floating)
        return false;

    m_floating->setTheme(m_theme);
    m_appliedTheme = m_theme;
    if (m_applyBtn)
        m_applyBtn->setText(tr("已应用"));
    updateControlState();
    return true;
}

void ThemeDialog::onControlsChanged()
{
    if (m_syncing)
        return;

    collectThemeFromControls();
    updateControlState();
    refreshPreview();
}

void ThemeDialog::onResetToDefault()
{
    m_theme = AppTheme();
    syncControlsFromTheme();
}

void ThemeDialog::onApply()
{
    applyTheme();
}

void ThemeDialog::onSaveAndClose()
{
    if (applyTheme())
        accept();
}
