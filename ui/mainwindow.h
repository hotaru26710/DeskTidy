#ifndef MAINWINDOW_H
#define MAINWINDOW_H

// ---------------------------------------------------------------------------
// ui/mainwindow.h —— DeskTidy 控制中心主窗口。
//
// 装配方式：顶部工具栏 + 左右分栏 + 底部状态栏，单窗口单页面。
//
// 【职责】主窗口是**操作编排者**：它决定"什么时候收、收进哪个盒"，
// 但自己不持有状态、也不搬文件 ——
//   * 状态（撤销栈、配置）住在 AppService 里，由 main.cpp 拥有并注入；
//     主窗口只是它的消费者之一，将来的各个浮窗是与它平级的另一个消费者；
//   * 文件移动一律委托 Collector（全应用唯一写文件入口，见 collector.h）。
//
// 这一点是相对早期版本的改变：以前 UndoStack / Settings 由主窗口
// new 出来并私有持有，于是"再来一个窗口"就无从共享状态。现在改为注入。
//
// 【数据流】
//   左栏选中盒子 -> 右栏刷新该盒内容
//   顶部「收纳桌面」-> DeskScanner 扫描 -> PreviewDialog 确认
//                  -> AppService::collectInto（内部走 Collector）
//                  -> 信号驱动刷新 -> 状态栏统计
//
// 【刷新模型】不再手工逐处调刷新，而是订阅 AppService 的信号：
//   undoStateChanged   -> updateUndoButton
//   boxContentsChanged -> refreshBoxes + refreshItems
//   moveFinished       -> reportMoveResult
// 这样"一次操作刷新一次"，且浮窗接进来后行为一致。
// ---------------------------------------------------------------------------

#include <QMainWindow>
#include <QList>
#include <QStringList>

#include "coretypes.h"          // DesktopEntry/StorageBox/MoveRecord 按值传递，须完整类型
// reportBoxDeletion 的形参是 AppService::BoxDeletionResult（嵌套类型）——
// 前置声明不够用，编译器必须看到完整的类定义。
#include "appservice.h"

class BoxListWidget;
class ItemListWidget;
class FloatingBoxManager;
class TrayIcon;

class QPushButton;
class QCloseEvent;
class QSplitter;
class QLabel;
class QFrame;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // service / floating 均由 main.cpp 拥有，生命周期长于本窗口
    // （main.cpp 里的声明顺序保证逆序析构）。不接受 nullptr。
    //
    // floating 做成注入而不是主窗口自己 new：浮窗的拥有关系若散落在
    // "主窗口右键菜单"和"托盘菜单"两个入口，就会出现谁 new 谁 delete 的纠纷。
    // 统一交给 FloatingBoxManager，主窗口只是它的一个调用方。
    explicit MainWindow(AppService *service,
                        FloatingBoxManager *floating,
                        QWidget *parent = nullptr);
    ~MainWindow() override;

    // 托盘用 setter 注入而不是走构造函数参数。
    //
    // 理由：托盘构造时需要拿到主窗口指针（它要能唤回窗口），
    // 若把托盘也塞进构造函数，就变成"建窗口要先有托盘、建托盘要先有窗口"
    // 的循环依赖，只能靠其中一个参数传 nullptr 再补，反而更绕。
    // 用 setter 后顺序是干净的：建窗口 -> 建托盘（传窗口）-> 设回窗口。
    //
    // 允许为 nullptr：托盘不可用时本来就没有托盘对象。
    void setTray(TrayIcon *tray);

protected:
    // 关窗行为在本阶段被改变：有托盘时只隐藏，让程序留在后台常驻。
    //
    // ⚠️ 没有托盘时必须照常退出 —— 否则用户关掉窗口后程序还在后台，
    // 而任务栏与 Alt+Tab 里都找不到入口，等于造出一个关不掉的幽灵进程。
    void closeEvent(QCloseEvent *event) override;

private slots:
    // 顶部动作
    void onNewBox();
    void onCollectDesktop();
    void onUndoLast();
    void onOpenSettings();

    // 栏目联动
    void onBoxSelectionChanged(const QString &boxName);
    void onRestoreRequested(const QString &path);
    void onFilesDropped(const QStringList &paths);

    // 左栏右键菜单：在桌面显示 / 关闭浮窗
    void onBoxListContextMenu(const QPoint &pos);

    // 打开浮窗外观设置对话框。
    // 由浮窗右键信号传入盒名；为空时退到当前选中的盒。
    void onOpenAppearanceDialog(const QString &boxName = QString());

    // 删除收纳盒。**这是全应用唯一的破坏性操作**，流程见实现处的长注释。
    void onDeleteBox(const StorageBox &box);

private:
    void buildUi();
    void applyThemeToUi();
    void refreshBoxes(const QString &preferSelect = QString());
    void refreshItems();
    void updateUndoButton();
    void updateStatus(const QString &text);

    // 删除前的确认。空盒走普通确认框；非空盒要求手输盒名。
    // 返回 true 表示主人确认删除。
    bool confirmBoxDeletion(const StorageBox &box, int itemCount);

    // 删除结束后向主人汇报（还原了几项、多少项进了回收站）。
    void reportBoxDeletion(const StorageBox &box,
                           const AppService::BoxDeletionResult &result);

    // 订阅 AppService 的信号。集中在一处建立，便于看清"谁驱动了什么刷新"。
    void connectServiceSignals();

    // 报告一批收纳/还原结果，并在有失败项时弹出失败清单（需求明确要求）。
    void reportMoveResult(const QList<MoveRecord> &records, const QString &actionLabel);

    // 把一批桌面条目收进指定盒的公共路径（顶部按钮与拖拽共用）。
    void collectEntries(const QList<DesktopEntry> &entries, const QString &boxPath,
                        const QString &boxName);

    // 当前选中盒；无选中时 name/path 为空串。
    StorageBox currentBox() const;

private:
    AppService *m_service = nullptr;    // 由 main.cpp 注入，不归本窗口所有

    // 浮窗管理者。本窗口只通过它开关浮窗、订阅它的状态变化，
    // 不直接持有任何 FloatingBoxWidget（生命周期全归它管）。
    FloatingBoxManager *m_floating = nullptr;

    // 托盘。不归本窗口所有（由 main.cpp 持有），这里只用来：
    //   * 判断"关闭时该隐藏还是该退出"；
    //   * 首次隐藏时提示一次"程序仍在后台"。
    // 可能为 nullptr —— 拿不到系统托盘时就是这种情况。
    TrayIcon *m_tray = nullptr;

    BoxListWidget *m_boxList   = nullptr;
    ItemListWidget *m_itemList = nullptr;
    QSplitter     *m_splitter  = nullptr;

    QFrame        *m_toolbarCard = nullptr;
    QFrame        *m_boxCard     = nullptr;
    QFrame        *m_itemCard    = nullptr;

    QLabel        *m_appTitleLabel = nullptr;
    QLabel        *m_appSubtitleLabel = nullptr;
    QLabel        *m_boxSectionTitle = nullptr;
    QLabel        *m_itemSectionTitle = nullptr;

    QPushButton   *m_newBoxBtn  = nullptr;
    QPushButton   *m_collectBtn = nullptr;
    QPushButton   *m_undoBtn    = nullptr;
    QPushButton   *m_settingsBtn = nullptr;

    QLabel        *m_statusLabel = nullptr;
};

#endif // MAINWINDOW_H
