#ifndef APPEARANCEDIALOG_H
#define APPEARANCEDIALOG_H

// ---------------------------------------------------------------------------
// ui/appearancedialog.h —— 浮窗外观设置对话框。
//
// 【为什么是独立类，而不是塞进 MainWindow::onOpenSettings】
//   1) 两者管的东西不同：那个对话框编辑的是**全局**排除名单，
//      这里编辑的是**每个盒各一份**的外观。混在一起用户会以为外观也是全局的。
//   2) 入口有两个（主窗口盒列表右键、浮窗右键的「更多设置…」），
//      现场构造在某个函数里的对话框没法被两边共用。
//   3) 项目已有 ui/previewdialog.* 这个"独立可复用对话框"的先例，照它写。
//
// 【本对话框不写配置、不碰文件】
//   一切读写都经 FloatingBoxManager：它内部完成"写 Settings + 广播"两件事。
//   之所以不让对话框直接碰 Settings，是因为主人的改动必须让**已经开着的浮窗
//   立刻跟着变**，而广播只有 manager 会发。绕过去必然出现"改完没反应"。
//
// 【「应用」按钮不关闭对话框】
//   外观是"调出来看看"的东西，调一档关一次窗口会让人没法连续比较。
// ---------------------------------------------------------------------------

#include <QDialog>
#include <QList>

#include "coretypes.h"          // StorageBox / BoxAppearance 按值使用，须完整类型

class FloatingBoxManager;
class FloatingHoverOverlay;
class ItemListWidget;

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;

class AppearanceDialog : public QDialog
{
    Q_OBJECT

public:
    // boxes：当前全部收纳盒（现扫磁盘得到，本对话框不再自己扫）。
    // initialBoxName：打开时预选哪个盒；传空则用列表第一个。
    // floating：外观的唯一读写入口，不归本对话框所有。
    //
    // 允许 boxes 为空（一个盒都没有）：此时界面进入全禁用状态并给出提示，
    // 而不是让调用方自己去判断该不该弹。
    AppearanceDialog(const QList<StorageBox> &boxes,
                     const QString &initialBoxName,
                     FloatingBoxManager *floating,
                     QWidget *parent = nullptr);

    // 主人最终选中的盒名。无盒时为空串。
    QString selectedBoxName() const;

protected:
    // 只管预览区的两件事：
    //   * QEvent::Resize —— 把光影覆盖层重新铺满预览控件（它不参与布局）；
    //   * Enter / Leave  —— 按鼠标进出驱动覆盖层的亮起与消退。
    // 用它而不是继承/改造 ItemListWidget：对话框只需要知道"鼠标在不在预览上"，
    // 为此多开一个控件子类不划算。
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onBoxChanged(int index);
    void onViewModeChanged();
    void onOpacityChanged(int value);
    void onTactileChanged();
    void onApply();
    void onResetToDefault();
    void onFollowGlobalTheme();

private:
    void buildUi();
    void buildEmptyUi();

    // 把 m_appearance 反映到各控件上（切盒、恢复默认后都要走一遍）。
    // 与"控件 -> m_appearance"的方向严格分开，避免两边互相触发变成死循环。
    void syncControlsFromAppearance();

    // 从各控件读回一份 BoxAppearance。
    void collectAppearanceFromControls();

    // 按当前设置刷新预览区（改视图/透明度时实时调用，不需要点应用）。
    void refreshPreview();

    // 把触感相关的五项（开关/强度/速度/圆角）同步给预览区的光影覆盖层。
    // 与 refreshPreview 分开：那个管预览的内容与视图，这个只管光影层。
    void syncPreviewGlow();

    // 触感关闭时把「反馈强度」置灰 —— 那一刻它没有任何作用，
    // 留着可点会让人以为调了没反应。
    void updateTouchControlsEnabled();

    // 当前选中盒；无盒或未选中时 name 为空串。
    StorageBox currentBox() const;

    // 当前盒是否已经有非默认外观（决定「恢复默认」按钮可用性）。
    void updateButtonsEnabled();

private:
    FloatingBoxManager *m_floating = nullptr;   // 由调用方注入，不归本对话框所有

    QList<StorageBox> m_boxes;
    BoxAppearance     m_appearance;             // 当前编辑中的一份（尚未应用的也在里面）

    // 一次性预置给预览区显示的假条目。
    // 不扫真实盒目录：对话框要在盒为空时也能用，而且扫盘会让切下拉框变卡。
    QList<DesktopEntry> m_previewEntries;

    QComboBox      *m_boxCombo    = nullptr;
    QComboBox      *m_viewCombo   = nullptr;
    QSlider        *m_opacitySlider = nullptr;
    QLabel         *m_opacityLabel  = nullptr;
    QLabel         *m_hintLabel     = nullptr;
    ItemListWidget *m_preview       = nullptr;

    // ---- 触感与动画（每盒一份）----
    QCheckBox      *m_hoverEffectCheck  = nullptr;
    QComboBox      *m_strengthCombo     = nullptr;
    QComboBox      *m_speedCombo        = nullptr;
    QComboBox      *m_expandDelayCombo  = nullptr;
    QComboBox      *m_collapseDelayCombo = nullptr;
    QComboBox      *m_cornerCombo       = nullptr;
    QComboBox      *m_cornerSmoothingCombo = nullptr;
    QLabel         *m_touchHintLabel    = nullptr;

    // 预览区的光影覆盖层（不参与布局），以及它挂靠的那个容器。
    // 容器负责收 Enter/Leave —— 预览控件本身是鼠标穿透的，收不到。
    FloatingHoverOverlay *m_previewGlow = nullptr;
    QWidget              *m_previewHost = nullptr;

    QPushButton    *m_followGlobalBtn = nullptr;
    QPushButton    *m_resetBtn = nullptr;
    QPushButton    *m_applyBtn = nullptr;

    AppTheme m_theme;

    // 防止"程序化改控件值"反过来触发 onXxxChanged 造成重复刷新或死循环。
    bool m_syncing = false;

    // 刚点过「应用」的那 900ms 内为真：让按钮保持禁用、文案停在"已应用"，
    // 使这次反馈能被看清。放在成员里而不是靠按钮自身的 enabled 状态，
    // 是因为 updateButtonsEnabled() 每次都会重算可用性、会把那个状态覆盖掉。
    bool m_justApplied = false;
};

#endif // APPEARANCEDIALOG_H
