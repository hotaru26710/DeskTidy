#ifndef ITEMLISTWIDGET_H
#define ITEMLISTWIDGET_H

// ---------------------------------------------------------------------------
// ui/itemlistwidget.h —— 「盒内条目列表」，供主窗口与浮窗共用。
//
// 职责：
//   1) 展示盒内容（图标取系统真实图标，exe/快捷方式才有辨识度）；
//   2) 双击：主窗口语义是"还原回桌面"，浮窗语义是"用系统默认方式打开"；
//   3) 接收从桌面拖进来的文件，发给上层归入当前盒；
//   4) （浮窗专用）支持把条目**拖出**，拖到桌面/资源管理器即还原。
//
// 【关于默认行为】本控件为浮窗新增了能力，但一律走 Options 里 opt-in，
// **默认值与改造前完全一致**：双击=还原、不可拖出、DropOnly。
// 主窗口不传 Options，于是它的行为一个字都没变 —— 这是回归闸门阶段的硬要求。
//
// 本控件**不执行任何文件操作**：还原、收纳、打开都只是发信号，
// 由上层调 Collector / Opener 完成。
// ---------------------------------------------------------------------------

#include <QListWidget>
#include <QList>
#include <QStringList>

#include "coretypes.h"          // DesktopEntry 为按值参数，须完整类型

class QFileIconProvider;
class QMimeData;

class ItemListWidget : public QListWidget
{
    Q_OBJECT

public:
    // 双击行为。
    //   Restore —— 还原回桌面（主窗口用，也是默认值）
    //   Open    —— 用系统默认关联打开（浮窗用）
    //
    // 之所以同一控件要支持两种语义：主窗口是"管理"界面，双击的意图是
    // "把它放回去"；浮窗是"使用"界面，双击的意图是"我要用它"。
    // 两者都符合各自场景下的直觉，强行统一反而会让其中一边别扭。
    enum class DoubleClickAction {
        Restore,
        Open
    };

    // 场景化配置。默认构造即"主窗口行为"。
    struct Options
    {
        DoubleClickAction doubleClick   = DoubleClickAction::Restore;
        bool              draggableOut  = false;   // 是否允许把条目拖出

        // 外观（浮窗专用）。默认值 = 改造前行为（列表模式），
        // 主窗口不传 Options 时这一组全走默认，连 setViewMode 都不会被调到。
        BoxAppearance::ViewMode viewMode = BoxAppearance::ViewMode::List;
        int                     iconSize = 0;      // 0 = 跟随 viewMode 推导
    };

    // 保持原签名：等价于 Options{}，即改造前的行为。
    explicit ItemListWidget(QWidget *parent = nullptr);

    // 浮窗用这个构造。
    explicit ItemListWidget(const Options &options, QWidget *parent = nullptr);

    // 重建列表。文件图标经 QFileIconProvider 取系统图标并缓存，
    // 避免同一目录反复刷新时重复走 shell 查询（那是这一栏唯一的重开销）。
    void setItems(const QList<DesktopEntry> &items);

    // 应用外观中的**视图部分**（视图模式 + 图标尺寸）。
    //
    // 与构造时的 Options 相比，这个可以在运行中调用 —— 浮窗右键菜单切视图走这里。
    // 透明度不在这里处理：它是窗口级的（QWidget::setWindowOpacity），
    // 而且必须避开列表的样式表（见 floatingboxwidget.cpp 的说明）。
    void applyAppearance(const BoxAppearance &appearance);

    // 选中项的完整路径清单。
    QStringList selectedPaths() const;

signals:
    // 双击某条且 doubleClick == Restore：请求把它还原回桌面原目录。
    void restoreRequested(const QString &path);

    // 双击某条且 doubleClick == Open：请求用系统默认方式打开它。
    void openRequested(const QString &path);

    // 拖入若干路径：请求归入当前盒。
    void filesDropped(const QStringList &paths);

    // 拖出结束。
    //
    // sourceStillExists 是**防文件凭空消失的兜底**：拖到不接受文件拖放的
    // 目标上时，Windows 以为"有人会处理"，可能谁都没动，文件就没了。
    // 上层必须据此判断：
    //   false —— 资源管理器成功把它移走了，静默刷新即可；
    //   true  —— 对方没接手，文件仍在盒里，必须明确告知主人。
    // 详见 ui/preferreddropeffect.h 的"硬约束"一节。
    void dragOutFinished(const QString &path, bool sourceStillExists);

    // 拖出**开始**（QDrag::exec() 即将进入系统级鼠标抓取）。
    //
    // 为什么要单独报一个"开始"：浮窗的悬停自动卷起需要知道这段时间里
    // 鼠标"不在窗口上"是假象。而 QDrag::exec() 会接管整个消息循环，
    // 从这一句进去到 dragOutFinished 出来之间，浮窗收不到正常的
    // enter/leave 序列 —— 只能靠这两个信号把区间框出来。
    //
    // 不框的后果：拖文件拖到一半，浮窗判定"鼠标走了"自己卷起来，
    // 源列表被隐藏，拖拽的 drop 目标判定和拖拽反馈全乱。
    void dragOutStarted();

protected:
    // 拖入三件套。dragEnter/dragMove 只改外观给出"可以放"的反馈，
    // 真正的落点判断集中在 dropEvent。
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

    // 拖出。仅在 m_options.draggableOut 为真时会被 Qt 调用到
    // （否则控件是 DropOnly，根本不会发起拖拽）。
    void startDrag(Qt::DropActions supportedActions) override;

private slots:
    void onItemDoubleClicked(QListWidgetItem *item);

private:
    // 统一拖拽高亮外观的开关，避免三处事件里各写一遍样式串。
    void setDragHighlight(bool on);

    // 按 m_options 里的 viewMode/iconSize 实际改控件的视图属性。
    // 只由 applyAppearance 调用 —— 单独暴露出去会让"什么时候该切视图"
    // 这件事散落各处，日后加一种视图模式必然漏掉某个调用点。
    void applyViewMode();

    // 从拖拽数据里提取本地文件路径（排除目录之外的 URL 类型）。
    static QStringList localPathsFromMime(const QMimeData *mime);

    // 条目 tooltip 里那句动作提示随 doubleClick 变化，
    // 否则浮窗里会写着"双击还原回桌面"而实际执行的是打开。
    QString doubleClickHint() const;

private:
    Options            m_options;

    // 上一次 setItems 收到的条目。
    //
    // 缓存它是为了支持"切换视图模式后原地重建"：图标尺寸变了必须重建条目
    // （见 applyViewMode 末尾的说明），而重建需要完整的 DesktopEntry ——
    // 列表项里只存了路径，反解不出大小与修改时间，重建后 tooltip 会丢信息。
    //
    // 代价是这份数据可能与磁盘不同步（主人在资源管理器里删了文件）。
    // 可接受：任何真正的刷新（收纳、撤销、开浮窗）都会重新调 setItems 覆盖它，
    // 而这份缓存只在"切视图"这一瞬间被用到。
    QList<DesktopEntry> m_lastItems;

    QFileIconProvider *m_iconProvider = nullptr;   // 系统图标提供者（懒建，只建一次）
    bool               m_dragActive   = false;     // 拖拽高亮当前是否生效

    // 是否已经应用过一次外观。
    //
    // 用途：applyAppearance 需要判断"这次调用是否真的改变了视图配置"。
    //
    // ⚠️ 不能按**值**判断（曾经就是这么写的，是个真 bug）：
    // 从大图标切回列表时，新的 viewMode 恰好等于枚举默认值 List、iconSize 恰好是 0，
    // 于是"值是默认就早退"会把这次切换当成"没配置过"，直接返回 ——
    // 结果是 setViewMode 没被调回 ListMode、gridSize 也没被清空，
    // 界面停在图标网格上，行高还被残留的网格撑开。
    //
    // 按"是否应用过"判断才能区分"从未配置"（构造期，应当什么都不做）
    // 与"切回默认值"（运行期，必须真的复位）。
    bool m_appearanceApplied = false;
};

#endif // ITEMLISTWIDGET_H
