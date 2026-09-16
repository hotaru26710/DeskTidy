#ifndef THEMEDIALOG_H
#define THEMEDIALOG_H

// ---------------------------------------------------------------------------
// ui/themedialog.h -- 中控全局主题设置对话框。
//
// 这里编辑的是“中控颜色 + 所有浮窗的默认外观”。单个浮窗若要偏离全局，
// 必须从浮窗右键进入各自的“浮窗外观”对话框，本对话框不会写任何每盒键。
//
// 应用动作统一交给 FloatingBoxManager::setTheme()：它会负责持久化并向
// 中控和所有未覆盖的浮窗广播。对话框不直接碰 Settings，避免出现
// “配置文件已经变了，但正在显示的窗口没跟着变”的状态分裂。
// ---------------------------------------------------------------------------

#include <QDialog>
#include <QVector>

#include "coretypes.h"

class FloatingBoxManager;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class ThemePreview;

class ThemeDialog : public QDialog
{
public:
    // current：调用时的全局主题快照。
    // floating：唯一的主题读写入口，不归本对话框所有；允许为空时仅可预览。
    ThemeDialog(const AppTheme &current,
                FloatingBoxManager *floating,
                QWidget *parent = nullptr);

    // 当前编辑中的主题（已归一化；点击“应用”后会立即通过 manager 落盘）。
    AppTheme theme() const;
    AppTheme editedTheme() const;

    // 相对上一次成功应用的主题是否有未保存修改。
    bool changed() const;

    // 嵌入统一设置中心时，由外层“保存”按钮调用。
    bool applyChanges();

private:
    struct ColorButton
    {
        QColor AppTheme::*field = nullptr;
        QPushButton *button = nullptr;
    };

    void buildUi();
    void syncControlsFromTheme();
    void collectThemeFromControls();
    void updateColorButtons();
    void updateControlState();
    void refreshPreview();
    void editColor(QColor AppTheme::*field);

    bool applyTheme();

    void onControlsChanged();
    void onResetToDefault();
    void onApply();
    void onSaveAndClose();

private:
    FloatingBoxManager *m_floating = nullptr;

    AppTheme m_theme;
    AppTheme m_appliedTheme;
    bool m_syncing = false;

    QVector<ColorButton> m_colorButtons;

    QSlider *m_windowOpacitySlider = nullptr;
    QLabel *m_windowOpacityLabel = nullptr;

    QComboBox *m_viewCombo = nullptr;
    QCheckBox *m_followIconSizeCheck = nullptr;
    QSpinBox *m_iconSizeSpin = nullptr;
    QSlider *m_opacitySlider = nullptr;
    QLabel *m_opacityLabel = nullptr;

    QComboBox *m_hoverEffectCombo = nullptr;
    QComboBox *m_strengthCombo = nullptr;
    QComboBox *m_speedCombo = nullptr;
    QComboBox *m_expandDelayCombo = nullptr;
    QComboBox *m_collapseDelayCombo = nullptr;
    QComboBox *m_cornerCombo = nullptr;
    QComboBox *m_cornerSmoothingCombo = nullptr;

    ThemePreview *m_preview = nullptr;

    QPushButton *m_resetBtn = nullptr;
    QPushButton *m_applyBtn = nullptr;
    QPushButton *m_saveCloseBtn = nullptr;
};

#endif // THEMEDIALOG_H
