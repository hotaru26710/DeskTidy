#include "itemlistwidget.h"

#include <QBrush>
#include <QColor>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QLocale>
#include <QMimeData>
#include <QSignalBlocker>
#include <QSize>
#include <QTextStream>
#include <QUrl>

// ---------------------------------------------------------------------------
// 实现说明
//
// * 图标：QFileIconProvider 会向 shell 查询真实图标，exe 与 .lnk 因此能显示出
//   各自的本来面目。注意 provider **不给 .url 解析 IconFile**，见 setItems
//   里那段说明 —— 那是"网址快捷方式全变白纸"的原因。
//
// * 拖拽反馈：Qt 的拖拽事件里必须显式 accept，否则 dropEvent 根本不会到来。
//   高亮外观集中在 setDragHighlight 里改，避免三处事件各写一遍样式串导致不一致。
//
// * 双击语义：默认是「还原回桌面」而非「打开」，由 m_options.doubleClick 决定。
//   主窗口不传 Options，行为与改造前一致。
//
// * 视图模式（列表 / 图标网格）：同样走 Options opt-in，且**默认配置下
//   applyAppearance 直接早退**，所以主窗口那条路径根本不会碰到视图属性。
//   图标网格必须成套地设 movement/resizeMode/wordWrap/gridSize 等，
//   少设一个就有对应的破绽，逐条理由见 applyViewMode。
//
// * 拖出：只在 draggableOut 为真时才放开（浮窗用）。拖出后必须检查源文件
//   是否还在，理由见 preferreddropeffect.h 的"硬约束"。
// ---------------------------------------------------------------------------

namespace {

// 与 ui/preferreddropeffect.cpp 里 kRestoreMimeType 必须一致：
// 它是"这次拖拽来自 DeskTidy 浮窗"的标记，也是让 Windows 把拖入
// 当成"移动"而非"复制"的触发器。
const char *kRestoreMimeType = "application/x-desktidy-restore";

// 从一个 .url 文件里读出 IconFile= 指向的图标路径；没有就返回空串。
//
// 【为什么需要自己读】
//
// .url 是 INI 格式的**纯文本**文件，真实图标写在 IconFile= 那一行
//（Steam 生成的游戏快捷方式都是这样，指向 steam/games 下的高清 .ico）。
// 但 QFileIconProvider 只看后缀、按类型关联给一个通用图标，
// 所以十几个 .url 会长得一模一样 —— 主人看到的就是"全变白纸"。
//
// 实测（主人盒里 11 个 Steam 快捷方式）：
//     provider 给出的不同图标数 = 1 / 11
//     读 IconFile 取到的不同图标数 = 11 / 11
//
// 【为什么手写解析而不是用 QSettings】
//
// QSettings 确实能读 INI，但它对"含中文与空格的 Windows 路径"的处理
// 依赖调用方的编码设置（setIniCodec 那套），而且会把值里的一些字符
// （分号、反斜杠）当转义处理 —— 拿它读一个**路径**容易出岔子。
// 这里只要一行 "IconFile=<路径>"，手写反而更可控、更好读。
//
// 【为什么不加缓存】
//
// 图标模式下列表会重建（切视图、收纳后刷新），于是同一个 .url 会被读多次。
// 但读一个几百字节的文本文件比一次 shell 图标查询便宜得多 ——
// 真正的重开销在 QFileIconProvider 那边，这里省不出什么。
// 为它加一层缓存又要处理失效（IconFile 指向的文件被删），不值。
QString urlIconFile(const QString &urlPath)
{
    QFile f(urlPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }

    QTextStream ts(&f);
    // .url 由 Windows 写成 UTF-8（含 UTF-8 BOM），显式指定编码免得
    // 中文路径在非中文 locale 下被读成乱码。
    ts.setEncoding(QStringConverter::Utf8);

    while (!ts.atEnd()) {
        const QString line = ts.readLine().trimmed();
        if (line.startsWith(QStringLiteral("IconFile="), Qt::CaseInsensitive)) {
            return line.mid(9).trimmed();
        }
    }
    return QString();
}

// 浮窗里显示文件名时去掉扩展名；文件夹和以点开头、没有正式扩展名的
// 隐藏文件保持原名。这里只改展示文字，真实路径始终走 DesktopEntry::filePath。
QString itemDisplayName(const DesktopEntry &entry, bool hideExtensions)
{
    if (!hideExtensions || entry.isDir)
        return entry.name;

    const QString baseName = QFileInfo(entry.name).completeBaseName();
    return baseName.isEmpty() ? entry.name : baseName;
}

} // namespace

ItemListWidget::ItemListWidget(QWidget *parent)
    : ItemListWidget(Options{}, parent)
{
    // 委托给下面的主构造函数：把"默认值"这件事收敛到 Options 的定义处，
    // 避免两个构造函数各写一份默认行为，日后改一处漏一处。
}

ItemListWidget::ItemListWidget(const Options &options, QWidget *parent)
    : QListWidget(parent)
    , m_options(options)
{
    setAlternatingRowColors(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);   // 支持多选，便于批量还原
    setAcceptDrops(true);                                     // 接收从桌面拖入

    // 拖出能力按需放开。默认 DropOnly —— 与改造前完全一致，
    // 主窗口的列表不能拖出（它靠双击还原，拖出会让语义混乱）。
    setDragDropMode(m_options.draggableOut ? QAbstractItemView::DragDrop
                                           : QAbstractItemView::DropOnly);

    setToolTip(tr("双击任意条目 = %1\n"
                  "也可以直接把桌面上的文件拖到这里，归入当前收纳盒")
                   .arg(m_options.doubleClick == DoubleClickAction::Open
                            ? tr("用系统默认方式打开")
                            : tr("把它还原回桌面原目录")));

    // 构造时就把视图设好：浮窗创建出来就该是正确的显示方式，
    // 而不是等到主人第一次切视图才生效。
    //
    // 这里不复用 applyAppearance(BoxAppearance)：那个函数的入参语义是
    // "一整套外观"，而构造期我们手上只有 Options 里的两项。
    // 直接调 applyViewMode 更直白 —— 但**必须先过一遍默认值早退**，
    // 否则主窗口（默认 Options）会白跑一遍属性设置，与"行为逐字不变"
    // 那条硬要求的精神相违。
    if (m_options.viewMode != BoxAppearance::ViewMode::List || m_options.iconSize != 0) {
        applyViewMode();
    }

    connect(this, &QListWidget::itemDoubleClicked,
            this, &ItemListWidget::onItemDoubleClicked);
}

void ItemListWidget::applyAppearance(const BoxAppearance &appearance)
{
    const bool viewChanged = (m_options.viewMode != appearance.viewMode)
                             || (m_options.iconSize != appearance.iconSize);

    m_options.viewMode = appearance.viewMode;
    m_options.iconSize = appearance.iconSize;

    // ---- 回归保障 ----
    // 主窗口用默认 Options 构造，不该被动过任何视图属性 ——
    // 它这条路径连 setViewMode 都不该被调到。
    //
    // ⚠️ 判断条件必须是"从未应用过"而不是"值等于默认"，这是一个真 bug 的修正：
    //
    // 早先写的是 `if (viewMode == List && iconSize == 0) return;`，按**值**判断。
    // 从大图标切回列表时，新值恰好就是默认值，于是这次实实在在的切换
    // 被当成了"没配置过"而直接返回 —— setViewMode 没调回 ListMode、
    // gridSize 没清空、setMovement/setWordWrap 等属性全都停在图标模式的值上。
    // 表现是"点了「列表」，界面还是网格，行高还被撑得很怪"。
    //
    // 按 m_appearanceApplied 判断才能区分这两种情况：
    //   构造期（从未应用过）      -> 什么都不做，主窗口行为零改动
    //   运行期切回默认值          -> 必须真的复位
    if (!m_appearanceApplied && !viewChanged) {
        m_appearanceApplied = true;
        return;
    }

    m_appearanceApplied = true;
    applyViewMode();
}

void ItemListWidget::setTheme(const AppTheme &theme)
{
    m_theme = theme;
    m_theme.normalize();

    QPalette pal = palette();
    pal.setColor(QPalette::Base, m_theme.surface);
    pal.setColor(QPalette::Window, m_theme.surface);
    pal.setColor(QPalette::Text, m_theme.text);
    pal.setColor(QPalette::WindowText, m_theme.text);
    pal.setColor(QPalette::Highlight, m_theme.primary);
    pal.setColor(QPalette::HighlightedText, m_theme.onPrimary);
    pal.setColor(QPalette::AlternateBase, m_theme.windowBackground);
    pal.setColor(QPalette::Disabled, QPalette::Text, m_theme.mutedText);
    setPalette(pal);

    for (int row = 0; row < count(); ++row) {
        QListWidgetItem *item = this->item(row);
        if (!item)
            continue;
        const bool isDir = item->data(Qt::UserRole + 1).toBool();
        item->setForeground(QBrush(isDir ? m_theme.primary : m_theme.text));
    }

    if (m_dragActive) {
        m_dragActive = false;
        setStyleSheet(m_baseStyleSheet);   // 先回到真正的基础样式，再按新主题重画高亮
        setDragHighlight(true);
    } else {
        setStyleSheet(m_baseStyleSheet);
    }
}

void ItemListWidget::applyViewMode()
{
    const bool iconMode = m_options.viewMode != BoxAppearance::ViewMode::List;

    if (iconMode) {
        // 图标像素走 BoxAppearance 的推导，不在这里重新算一遍 ——
        // 两处各算一份的话，日后改了档位表就会漏改一处，
        // 表现为"设置里显示大图标、实际还是中图标"。
        BoxAppearance effective;
        effective.viewMode = m_options.viewMode;
        effective.iconSize = m_options.iconSize;
        const int px = effective.effectiveIconSize();

        setViewMode(QListView::IconMode);

        // ---- 以下六项每一个都不能省，理由逐条写在旁边 ----

        // 不设 Static 的话默认是 Free：主人能把图标拖着乱放，而位置没有任何
        // 地方保存，一次刷新（收纳完、撤销完都会刷新）就全乱了 —— 观感上像是
        // "我刚才摆的位置被程序吃了"。
        setMovement(QListView::Static);

        // 不设 Adjust 的话窗口拉大后不重排，内容全挤在左边一列。
        setResizeMode(QListView::Adjust);

        // 不设的话长文件名会把格子撑破、与相邻条目重叠成一团。
        setWordWrap(true);

        // 图标模式下所有格子同尺寸，Qt 可以省掉逐项测量。条目多时差别明显。
        setUniformItemSizes(true);

        setIconSize(QSize(px, px));
        // 格子比图标本身大一圈：下方要留出文件名，四周留一点呼吸感。
        // 这里的 +32/+40 与 floatingboxwidget.cpp 里 resizeForAppearance
        // 算窗口尺寸时用的是同一组值 —— 改一处必须改另一处。
        setGridSize(QSize(px + 32, px + 40));
        setSpacing(4);
    } else {
        // 切回列表：**必须把上面设过的全部复位**，否则残留的网格会让列表模式
        // 变得很怪（行高被撑开、条目之间莫名留白）。
        setViewMode(QListView::ListMode);
        setMovement(QListView::Static);
        setResizeMode(QListView::Fixed);
        setWordWrap(false);
        setUniformItemSizes(true);
        // 空 QSize 表示"不启用网格"，这是 Qt 的约定，不是漏写。
        setGridSize(QSize());
        setSpacing(0);
        setIconSize(QSize(16, 16));
    }

    // 拖拽缩略图（startDrag 里的 pixmap(32,32)）刻意不随视图模式变：
    // 它是跟着鼠标走的小图，与列表里显示多大无关，32×32 在任何模式下都合适。

    // ---- 重建条目图标 ----
    // 必须重来一遍：setIconSize 只会把现有图标**拉伸**到新尺寸，
    // 而 setItems 里那条"向 QIcon 要大图"的分支只在重建条目时才走到。
    // 不重建的话，切到大图标看到的是把小图放大后的糊图。
    //
    // 用 setItems 自己缓存的那份条目数据重来，不去列表里反解 ——
    // 列表项里只存了路径，反解不出大小与修改时间，重建后 tooltip 会丢掉信息。
    //
    // 先拷一份再传：setItems 内部会把入参赋回 m_lastItems，
    // 直接传 m_lastItems 就是自己赋自己。QList 的自赋值是安全的，
    // 但显式拷一份让这里的意图一眼可见，不依赖读者的隐式知识。
    if (!m_lastItems.isEmpty()) {
        const QList<DesktopEntry> snapshot = m_lastItems;
        setItems(snapshot);
    }
}

QString ItemListWidget::doubleClickHint() const
{
    return m_options.doubleClick == DoubleClickAction::Open
               ? tr("双击用系统默认方式打开")
               : tr("双击还原回桌面");
}

void ItemListWidget::setItems(const QList<DesktopEntry> &items)
{
    const QSignalBlocker blocker(this);
    clear();

    // 留下这份数据，供"切换视图模式后原地重建"使用（见 applyViewMode）。
    m_lastItems = items;

    // 懒建：只有真的要显示条目时才创建 provider（省掉空盒时的一次 shell 初始化）。
    if (!m_iconProvider) {
        m_iconProvider = new QFileIconProvider();
    }

    // 同后缀复用同一个图标：一个盒子里十几个 .txt 只查一次 shell。
    //
    // ⚠️⚠️ 这个缓存**已被证明是错的，整个删除**（主人实测：每次收纳新东西
    // 都会让盒里的图标错乱）。别再写回来。
    //
    // 错在哪：它假设"同后缀 = 同图标"，但 QFileIconProvider::icon(文件)
    // 返回的是**每个文件各自**的图标 —— 不止后缀决定，还与文件的嵌入图标、
    // .lnk 指向的目标、以及文件类型关联有关。
    // 实测（tools/icondiag）：两个同后缀的 .txt，QIcon::cacheKey() **不同**。
    // 于是第二个文件会拿到第一个的图标，而"谁先被扫到"取决于目录遍历顺序 ——
    // 每次收纳都重建一次列表，图标就跟着重排一次，表现正是"图标错乱"。
    //
    // 【为什么敢直接删掉缓存，不再换一种缓存键】
    // 下手前实测过耗时（tools/iconperf，各规模取真实 64×64 位图）：
    //     50 个文件 4ms / 100 个 6ms / 200 个 10ms（缓存版 1/3/5ms）
    // 省下的是个位数毫秒，而重建列表本身发生在收纳/拖动之后，
    // 离"能感觉到卡"（~100ms）差一个数量级。为了几毫秒换图标错乱不划算。
    //
    // 顺带记一个测耗时时踩的坑：一开始只调 prov.icon() 然后读 cacheKey()，
    // 三档全是 0ms —— 那不是快，是 QFileIconProvider::icon() 惰性，
    // 真正的 shell 查询发生在**要位图**的那一刻。必须 pixmap() 取出来才测得准。

    // 双击提示只算一次，每个条目的 tooltip 都用它。
    const QString hint = doubleClickHint();

    for (const DesktopEntry &entry : items) {
        auto *item = new QListWidgetItem(
            itemDisplayName(entry, m_options.hideExtensions), this);
        item->setData(Qt::UserRole, entry.filePath);
        item->setData(Qt::UserRole + 1, entry.isDir);

        // 大小与时间放进 tooltip：列表本身保持干净，信息按需可见。
        const QString sizeText = entry.isDir
                                     ? tr("文件夹")
                                     : QLocale().formattedDataSize(entry.size);
        item->setToolTip(tr("%1\n大小：%2\n修改时间：%3\n%4")
                             .arg(entry.filePath,
                                  sizeText,
                                  entry.modified.isValid()
                                      ? entry.modified.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                                      : tr("未知"),
                                  hint));

        // 图标：每个文件单独取，不复用。
        QIcon icon;
        bool haveIcon = false;

        // ---- 先处理 .url：QFileIconProvider 对它**不解析 IconFile** ----
        //
        // 现象（主人实测）：盒里 11 个 Steam 游戏快捷方式全显示成同一个
        // "白纸"图标，分不出是哪个游戏。
        //
        // 原因：.url 是个 INI 文本文件，图标写在里面的 IconFile= 那一行
        //（Steam 生成的这些还都是几十到几百 KB 的高清 .ico）。
        // 但 QFileIconProvider 只看**后缀**，按 .url 的类型关联给一个通用图标，
        // 十几个 .url 因此长得一模一样。
        // 而 .lnk 没有这个问题 —— shell 会解析它的目标，所以那 53 个快捷方式
        // 图标本来就是各自的。
        //
        // 实测（tools/urlcheck，主人盒里的真实数据）：
        //     provider 给出的不同图标数 = 1 / 11
        //     从 .ico  取到的不同图标数 = 11 / 11
        // 所以这里自己把 IconFile= 读出来。
        if (!entry.isDir && entry.filePath.endsWith(QStringLiteral(".url"), Qt::CaseInsensitive)) {
            const QString iconFile = urlIconFile(entry.filePath);
            if (!iconFile.isEmpty() && QFileInfo::exists(iconFile)) {
                icon = QIcon(iconFile);
                haveIcon = !icon.isNull();
            }
            // 读不到就往下走通用逻辑，退化成原来的图标 ——
            // 绝不能因为 IconFile 失效就让条目变成没有图标。
        }

        if (!haveIcon) {
            icon = entry.isDir
                       ? m_iconProvider->icon(QFileIconProvider::Folder)
                       : m_iconProvider->icon(QFileInfo(entry.filePath));
        }

        // ⚠️ 大图标模式下直接 setIcon 会得到一张糊图：
        // QFileIconProvider 给的是系统默认尺寸（通常 32×32 或更小）的图标，
        // 而 QListWidget 在 IconMode 下会把它按 setIconSize 拉伸到 64×64。
        // 这里向 QIcon 要一个**足够大的** pixmap 再建图标 ——
        // QIcon 内部对多尺寸资源（.ico / 系统图标缓存）会挑合适的那一档，
        // 比让 Qt 事后拉伸清楚得多。
        const int wantPx = m_options.viewMode == BoxAppearance::ViewMode::List
                               ? 0
                               : qMax(m_options.iconSize,
                                      BoxAppearance::defaultIconSizeFor(m_options.viewMode));

        // ⚠️⚠️ 必须**深拷贝**成一份独立的 QIcon，不能直接把 provider 的返回值
        // 交出去 —— 这是主人报的"每存一次新东西图标就错乱"的真正根因。
        //
        // 现象（实测 tools/icon_probe）：四个不同的 exe
        //（notepad/calc/cmd/charmap）在列表里**全显示同一个图标**，
        // 而且是**第一个**那个的图标；同一个循环里直接问 provider，
        // 它给的却是四个互不相同的正确图标。
        //
        // 原因：QFileIconProvider 在 Windows 上背后是系统的 shell 图标缓存，
        // 它返回的 QIcon 是**隐式共享**的。把这样的 QIcon 直接交给
        // QListWidgetItem 之后，多个条目会指向同一份底层数据；
        // 后续的取值/渲染就都被并到同一个图标上。
        // 列表每次刷新都重建一遍，所以"谁排第一谁说了算" ——
        // 每次收纳新文件都会重排，观感就是图标在乱跳。
        //
        // 修法：把位图取出来重新包一个 QIcon，切断共享。
        // 下面这一句对两种模式都成立：
        //   * 列表模式 wantPx == 0 时取 0 尺寸不行，所以退化成"要一张
        //     与系统默认档相当的图"（会挑到 16 或 32 那一档）；
        //   * 图标模式取 wantPx，顺便解决防糊的问题。
        const int copyPx = wantPx > 0 ? wantPx
                                      : BoxAppearance::defaultIconSizeFor(
                                            BoxAppearance::ViewMode::List);
        item->setIcon(QIcon(icon.pixmap(copyPx, copyPx)));

        item->setForeground(QBrush(entry.isDir ? m_theme.primary : m_theme.text));
    }
}

QStringList ItemListWidget::selectedPaths() const
{
    QStringList paths;
    const QList<QListWidgetItem *> items = selectedItems();
    paths.reserve(items.size());
    for (const QListWidgetItem *item : items) {
        paths << item->data(Qt::UserRole).toString();
    }
    return paths;
}

void ItemListWidget::onItemDoubleClicked(QListWidgetItem *item)
{
    if (!item) {
        return;
    }
    // 仅发信号，不在 UI 层动文件 —— 移动一律经 Collector，打开一律经 Opener。
    const QString path = item->data(Qt::UserRole).toString();
    if (m_options.doubleClick == DoubleClickAction::Open) {
        emit openRequested(path);
    } else {
        emit restoreRequested(path);
    }
}

// ---------------------------------------------------------------------------
// 拖出
// ---------------------------------------------------------------------------
void ItemListWidget::startDrag(Qt::DropActions supportedActions)
{
    Q_UNUSED(supportedActions);

    // 双保险：正常情况下 DropOnly 不会走到这里，但显式判一次更稳，
    // 免得日后有人改了 dragDropMode 就意外获得拖出能力。
    if (!m_options.draggableOut) {
        return;
    }

    QListWidgetItem *item = currentItem();
    if (!item) {
        return;
    }

    const QString path = item->data(Qt::UserRole).toString();
    if (path.isEmpty()) {
        return;
    }

    auto *mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(path)});
    // 私有标记：既标识"这是 DeskTidy 的还原拖拽"，也是
    // PreferredDropEffect 转换器生效的触发器（只有它出现才会挂上
    // "请执行移动"那个原生格式）。见 preferreddropeffect.h。
    mime->setData(QLatin1String(kRestoreMimeType), path.toUtf8());

    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    // 拖动缩略图用条目自己的图标，主人能一眼看出拖的是哪个东西。
    drag->setPixmap(item->icon().pixmap(32, 32));

    // 只请求 MoveAction：配合 PreferredDropEffect，资源管理器会真的把文件
    // 搬过去（实测：exec() 返回 MoveAction、源文件消失、目标处有文件）。
    //
    // ⚠️ 用 emit 把 exec() 的整个区间框出来：消费方（浮窗）要据此屏蔽
    // 悬停自动卷起 —— 这段时间是系统级鼠标抓取，浮窗收不到正常的
    // enter/leave，不屏蔽的话拖到一半窗口会自己缩起来。
    // 两个信号之间的代码路径没有任何提前 return（下面全是直线代码），
    // 所以不会出现"报了开始、没报结束"把标志永久卡住的情况。
    emit dragOutStarted();

    drag->exec(Qt::MoveAction, Qt::MoveAction);

    // ⚠️ 防"文件凭空消失"的兜底，必须做：
    // 拖到不接受文件拖放的目标上时，Windows 以为"有人会处理"，可能谁都没动。
    // 此时源文件仍在盒里，而返回值可能仍报 MoveAction —— 只看返回值会误判成
    // "已还原"，于是列表刷新后条目消失、文件却哪个目录都找不到。
    // 以文件系统的实际状态为准，才是可靠的判据。
    const bool stillExists = QFile::exists(path);
    emit dragOutFinished(path, stillExists);

    // 注意：这里**绝不**自己去移动或删除源文件。真正搬文件的是资源管理器，
    // 我们再动一次就是操作一个可能已不存在的路径。
}

QStringList ItemListWidget::localPathsFromMime(const QMimeData *mime)
{
    QStringList paths;
    if (!mime || !mime->hasUrls()) {
        return paths;
    }
    const QList<QUrl> urls = mime->urls();
    paths.reserve(urls.size());
    for (const QUrl &url : urls) {
        // 只接本地文件，过滤掉 http:// 之类的网络 URL（那些没有本地路径可搬）。
        if (url.isLocalFile()) {
            paths << url.toLocalFile();
        }
    }
    return paths;
}

void ItemListWidget::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();   // 不 accept 的话后续 dropEvent 不会送达
        setDragHighlight(true);
    } else {
        event->ignore();
    }
}

void ItemListWidget::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void ItemListWidget::dragLeaveEvent(QDragLeaveEvent *event)
{
    setDragHighlight(false);
    QListWidget::dragLeaveEvent(event);
}

void ItemListWidget::dropEvent(QDropEvent *event)
{
    setDragHighlight(false);

    const QStringList paths = localPathsFromMime(event->mimeData());
    if (paths.isEmpty()) {
        event->ignore();
        return;
    }

    event->acceptProposedAction();
    emit filesDropped(paths);   // 落地动作交给上层 + Collector
}

void ItemListWidget::setDragHighlight(bool on)
{
    if (m_dragActive == on) {
        return;
    }
    m_dragActive = on;

    // 用样式表切换边框与底色做反馈：够醒目，且不需要额外控件。
    if (on) {
        m_baseStyleSheet = styleSheet();
        QColor highlightBg = m_theme.primary;
        highlightBg.setAlpha(36);
        setStyleSheet(QStringLiteral("QListWidget { border: 2px dashed %1;"
                                     " background: rgba(%2,%3,%4,%5); }")
                          .arg(m_theme.primary.name(QColor::HexArgb))
                          .arg(highlightBg.red())
                          .arg(highlightBg.green())
                          .arg(highlightBg.blue())
                          .arg(highlightBg.alpha()));
    } else {
        setStyleSheet(m_baseStyleSheet);
    }
}
