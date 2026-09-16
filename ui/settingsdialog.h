#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

// ---------------------------------------------------------------------------
// ui/settingsdialog.h -- 统一设置中心。
//
// 左侧是模块导航，右侧是内容页：
//   常规 / 主题与外观 / 启动与后台 / 关于
//
// 主题页复用 ThemeDialog 的编辑与预览逻辑，只是在统一设置中心里隐藏其
// 自己的按钮区，由本对话框底部的“保存”统一提交，避免两个保存入口。
// ---------------------------------------------------------------------------

#include <QDialog>

class AppService;
class FloatingBoxManager;
class ThemeDialog;

class QCheckBox;
class QComboBox;
class QListWidget;
class QPlainTextEdit;
class QStackedWidget;

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    SettingsDialog(AppService *service,
                   FloatingBoxManager *floating,
                   QWidget *parent = nullptr);

private slots:
    void switchPage(int row);
    void saveAll();

private:
    QWidget *buildGeneralPage();
    QWidget *buildThemePage();
    QWidget *buildStartupPage();
    QWidget *buildAboutPage();

    void applyDialogTheme();

private:
    AppService *m_service = nullptr;
    FloatingBoxManager *m_floating = nullptr;

    QListWidget *m_sidebar = nullptr;
    QStackedWidget *m_pages = nullptr;

    QCheckBox *m_animationsCheck = nullptr;
    QCheckBox *m_hoverExpandCheck = nullptr;
    QPlainTextEdit *m_excludeEditor = nullptr;

    QCheckBox *m_autoStartCheck = nullptr;
    QComboBox *m_autoStartModeCombo = nullptr;

    ThemeDialog *m_themePage = nullptr;
};

#endif // SETTINGSDIALOG_H
