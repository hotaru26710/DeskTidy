#include "settingsdialog.h"

#include "appservice.h"
#include "autostart.h"
#include "floatingboxmanager.h"
#include "settings.h"
#include "themedialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {

QLabel *makeHeading(const QString &title, const QString &subtitle, QWidget *parent)
{
    auto *label = new QLabel(QStringLiteral("<div style='font-size:18px;font-weight:700;'>%1</div>"
                                            "<div style='font-size:12px;margin-top:4px;'>%2</div>")
                                 .arg(title.toHtmlEscaped(), subtitle.toHtmlEscaped()),
                             parent);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setObjectName(QStringLiteral("pageHeading"));
    return label;
}

QFrame *makeCard(QWidget *parent)
{
    auto *card = new QFrame(parent);
    card->setObjectName(QStringLiteral("settingsCard"));
    return card;
}

QLabel *makeCardTitle(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("cardTitle"));
    return label;
}

QLabel *makeHint(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("pageHint"));
    label->setWordWrap(true);
    return label;
}

} // namespace

SettingsDialog::SettingsDialog(AppService *service,
                               FloatingBoxManager *floating,
                               QWidget *parent)
    : QDialog(parent)
    , m_service(service)
    , m_floating(floating)
{
    setWindowTitle(tr("DeskTidy 设置"));
    resize(1040, 720);
    setMinimumSize(860, 600);

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(16);

    m_sidebar = new QListWidget(this);
    m_sidebar->setObjectName(QStringLiteral("settingsSidebar"));
    m_sidebar->addItem(tr("常规"));
    m_sidebar->addItem(tr("主题与外观"));
    m_sidebar->addItem(tr("启动与后台"));
    m_sidebar->addItem(tr("关于"));
    m_sidebar->setFixedWidth(176);
    m_sidebar->setCurrentRow(0);
    root->addWidget(m_sidebar);

    auto *right = new QVBoxLayout;
    right->setSpacing(12);

    m_pages = new QStackedWidget(this);
    m_pages->addWidget(buildGeneralPage());
    m_pages->addWidget(buildThemePage());
    m_pages->addWidget(buildStartupPage());
    m_pages->addWidget(buildAboutPage());
    right->addWidget(m_pages, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Save)->setText(tr("保存设置"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    right->addWidget(buttons);

    root->addLayout(right, 1);

    connect(m_sidebar, &QListWidget::currentRowChanged,
            this, &SettingsDialog::switchPage);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::saveAll);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    applyDialogTheme();
}

QWidget *SettingsDialog::buildGeneralPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    layout->addWidget(makeHeading(tr("常规"),
                                  tr("控制收纳规则和界面动画，保持桌面清爽但不打扰。"),
                                  page));

    auto *behavior = makeCard(page);
    auto *behaviorLayout = new QVBoxLayout(behavior);
    behaviorLayout->setContentsMargins(16, 14, 16, 16);
    behaviorLayout->setSpacing(10);
    behaviorLayout->addWidget(makeCardTitle(tr("界面与浮窗"), behavior));

    m_animationsCheck = new QCheckBox(tr("启用界面动画"), behavior);
    m_animationsCheck->setChecked(m_service->settings()->animationsEnabled());
    m_animationsCheck->setToolTip(tr("关闭后，浮窗出现、卷起和触感反馈会立即切换。"));
    behaviorLayout->addWidget(m_animationsCheck);

    m_hoverExpandCheck = new QCheckBox(tr("鼠标移到浮窗上时自动展开"), behavior);
    m_hoverExpandCheck->setChecked(m_service->settings()->hoverExpandEnabled());
    m_hoverExpandCheck->setToolTip(tr("只控制自动展开；悬停光影触感仍可在主题外观里单独设置。"));
    behaviorLayout->addWidget(m_hoverExpandCheck);
    layout->addWidget(behavior);

    auto *exclude = makeCard(page);
    auto *excludeLayout = new QVBoxLayout(exclude);
    excludeLayout->setContentsMargins(16, 14, 16, 16);
    excludeLayout->setSpacing(10);
    excludeLayout->addWidget(makeCardTitle(tr("不要收纳的内容"), exclude));
    excludeLayout->addWidget(makeHint(tr("每行写一个名称。匹配的桌面文件或文件夹不会被收纳；"
                                         "根目录和 DeskTidy 自身已经自动排除。"),
                                      exclude));
    m_excludeEditor = new QPlainTextEdit(exclude);
    m_excludeEditor->setPlainText(m_service->settings()->excludedNames().join(QLatin1Char('\n')));
    m_excludeEditor->setPlaceholderText(tr("例如：\n重要项目\n正在处理的文档"));
    m_excludeEditor->setMinimumHeight(170);
    excludeLayout->addWidget(m_excludeEditor);
    layout->addWidget(exclude, 1);

    return page;
}

QWidget *SettingsDialog::buildThemePage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    layout->addWidget(makeHeading(tr("主题与外观"),
                                  tr("统一调整中控和所有未单独定制浮窗的颜色、透明度与手感。"),
                                  page));

    m_themePage = new ThemeDialog(m_floating->theme(), m_floating, page);
    m_themePage->setWindowFlags(Qt::Widget);
    m_themePage->setModal(false);
    m_themePage->setSizeGripEnabled(false);
    if (QDialogButtonBox *box = m_themePage->findChild<QDialogButtonBox *>())
        box->hide();
    layout->addWidget(m_themePage, 1);
    return page;
}

QWidget *SettingsDialog::buildStartupPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    layout->addWidget(makeHeading(tr("启动与后台"),
                                  tr("登录 Windows 后如何启动 DeskTidy，以及是否显示控制中心。"),
                                  page));

    auto *card = makeCard(page);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(16, 14, 16, 16);
    cardLayout->setSpacing(12);
    cardLayout->addWidget(makeCardTitle(tr("开机自动启动"), card));

    const bool supported = AutoStart::isSupported();
    const bool enabled = supported && AutoStart::isEnabled();

    m_autoStartCheck = new QCheckBox(tr("登录 Windows 后自动启动 DeskTidy"), card);
    m_autoStartCheck->setChecked(enabled);
    m_autoStartCheck->setEnabled(supported);
    cardLayout->addWidget(m_autoStartCheck);

    auto *modeRow = new QHBoxLayout;
    modeRow->addSpacing(24);
    modeRow->addWidget(new QLabel(tr("启动方式："), card));
    m_autoStartModeCombo = new QComboBox(card);
    m_autoStartModeCombo->addItem(tr("普通启动（显示控制中心）"));
    m_autoStartModeCombo->addItem(tr("静默启动（只显示浮窗）"));
    m_autoStartModeCombo->setCurrentIndex(
        enabled ? (AutoStart::isSilent() ? 1 : 0)
                : (m_service->settings()->autoStartSilent() ? 1 : 0));
    modeRow->addWidget(m_autoStartModeCombo, 1);
    cardLayout->addLayout(modeRow);

    m_autoStartModeCombo->setEnabled(supported && m_autoStartCheck->isChecked());
    connect(m_autoStartCheck, &QCheckBox::toggled, m_autoStartModeCombo,
            [this](bool on) {
                if (m_autoStartModeCombo)
                    m_autoStartModeCombo->setEnabled(AutoStart::isSupported() && on);
            });

    cardLayout->addWidget(makeHint(tr("静默启动不会显示控制中心，只在桌面恢复浮窗并驻留托盘。"),
                                   card));
    layout->addWidget(card);
    layout->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildAboutPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    layout->addWidget(makeHeading(tr("关于 DeskTidy"),
                                  tr("一个不删除文件的桌面收纳盒。需要时打开，不需要时安静待命。"),
                                  page));

    auto *card = makeCard(page);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(18, 16, 18, 18);
    cardLayout->setSpacing(10);
    cardLayout->addWidget(makeCardTitle(tr("DeskTidy"), card));
    cardLayout->addWidget(makeHint(tr("文件只会被移动到 %USERPROFILE%\\DeskTidy 下；"
                                      "重名会自动避让，不会覆盖已有文件。"),
                                   card));
    cardLayout->addWidget(makeHint(tr("删除收纳盒时，文件会先回到桌面；无法还原的内容会进入回收站。"),
                                   card));
    layout->addWidget(card);
    layout->addStretch(1);
    return page;
}

void SettingsDialog::switchPage(int row)
{
    if (!m_pages || row < 0 || row >= m_pages->count())
        return;
    m_pages->setCurrentIndex(row);
}

void SettingsDialog::saveAll()
{
    QStringList names;
    const QStringList lines = m_excludeEditor->toPlainText().split(QLatin1Char('\n'),
                                                                   Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty())
            names << trimmed;
    }

    m_service->settings()->setExcludedNames(names);
    m_floating->setAnimationsEnabled(m_animationsCheck->isChecked());
    m_floating->setHoverExpandEnabled(m_hoverExpandCheck->isChecked());

    const bool silent = m_autoStartModeCombo->currentIndex() == 1;
    m_service->settings()->setAutoStartSilent(silent);

    bool autoStartSaved = true;
    if (AutoStart::isSupported()) {
        autoStartSaved = AutoStart::setEnabled(m_autoStartCheck->isChecked(), silent);
    }

    if (m_themePage && m_themePage->changed())
        m_themePage->applyChanges();

    if (!autoStartSaved) {
        QMessageBox::warning(this, tr("开机自启未生效"),
                             tr("无法更新 Windows 开机启动项，请确认当前用户有权限写入注册表。"));
    }

    accept();
}

void SettingsDialog::applyDialogTheme()
{
    AppTheme theme = m_floating->theme();
    theme.normalize();

    setStyleSheet(QStringLiteral(
        "QDialog { background: %1; color: %2; }"
        "QListWidget#settingsSidebar {"
        "  background: %3; border: 1px solid %4; border-radius: 14px;"
        "  padding: 6px; outline: 0;"
        "}"
        "QListWidget#settingsSidebar::item {"
        "  height: 42px; border-radius: 9px; padding-left: 12px; color: %2;"
        "}"
        "QListWidget#settingsSidebar::item:hover { background: %5; }"
        "QListWidget#settingsSidebar::item:selected { background: %6; color: %7; }"
        "QLabel#pageHeading { color: %2; }"
        "QLabel#pageHint { color: %8; font-size: 12px; }"
        "QLabel#cardTitle { color: %2; font-size: 14px; font-weight: 600; }"
        "QFrame#settingsCard {"
        "  background: %3; border: 1px solid %4; border-radius: 14px;"
        "}"
        "QGroupBox {"
        "  background: %3; border: 1px solid %4; border-radius: 12px;"
        "  margin-top: 12px; padding: 14px 12px 12px 12px; font-weight: 600;"
        "}"
        "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px;"
        "  background: %3; color: %2; }"
        "QPlainTextEdit, QComboBox, QSpinBox, QLineEdit {"
        "  background: %9; color: %2; border: 1px solid %4;"
        "  border-radius: 9px; padding: 7px 9px;"
        "}"
        "QPlainTextEdit:focus, QComboBox:focus, QSpinBox:focus, QLineEdit:focus {"
        "  border-color: %6;"
        "}"
        "QPushButton { padding: 7px 14px; border: 1px solid %4; border-radius: 9px;"
        "  background: %3; color: %2; }"
        "QPushButton:hover { background: %5; }"
        "QPushButton:pressed { background: %10; }"
        "QDialogButtonBox QPushButton { min-width: 92px; }")
        .arg(theme.windowBackground.name(QColor::HexRgb),
             theme.text.name(QColor::HexRgb),
             theme.surface.name(QColor::HexRgb),
             theme.border.name(QColor::HexRgb),
             theme.hover.name(QColor::HexRgb),
             theme.primary.name(QColor::HexRgb),
             theme.onPrimary.name(QColor::HexRgb),
             theme.mutedText.name(QColor::HexRgb),
             theme.windowBackground.name(QColor::HexRgb),
             theme.pressed.name(QColor::HexRgb)));
}
