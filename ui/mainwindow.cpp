#include "mainwindow.h"

#include "boxlistwidget.h"
#include "floatingboxmanager.h"
#include "itemlistwidget.h"
#include "previewdialog.h"
#include "trayicon.h"

#include "appearancedialog.h"

#include "appservice.h"
#include "boxmanager.h"
#include "corenames.h"
#include "deskscanner.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

// ---------------------------------------------------------------------------
// 实现说明
//
// * 空状态防御：本窗口几乎每个动作都依赖"当前有选中的盒子"。所有入口
//   （收纳 / 拖入）都先判 currentBox().path 是否为空，为空则给一句人话提示，
//   绝不让操作落到空路径上。
//
// * 失败清单：需求明确要求批量失败时逐条列出原因。故 reportMoveResult 在
//   有任何 Failed 时弹 QMessageBox，把中文原因一条条铺开，而不是只报个数字。
//
// * 刷新策略（相对早期版本的改变）：
//   以前是"谁操作完谁记得调刷新"，于是 collectEntries / onUndoLast /
//   onRestoreRequested 里各有一份 refreshBoxes + refreshItems。现在统一改为
//   **订阅 AppService 的信号** —— 状态一变就广播，主窗口与将来的 N 个浮窗
//   各自刷新，不再有"漏调一处导致界面不一致"的可能。
//
//   代价是 refreshBoxes 的 preferSelect 参数在信号路径里拿不到（信号只带
//   boxPath，不带"希望选中谁"）。处理方式见 onBoxContentsChanged 的注释。
//
// * 浮窗入口：左栏盒列表的右键菜单提供「在桌面显示浮窗 / 关闭浮窗」。
//   本窗口不自己 new 浮窗，只调 FloatingBoxManager —— 它的注释里说明了
//   为什么要把浮窗的拥有关系收敛到一处。
// ---------------------------------------------------------------------------

MainWindow::MainWindow(AppService *service,
                       FloatingBoxManager *floating,
                       QWidget *parent)
    : QMainWindow(parent)
    , m_service(service)
    , m_floating(floating)
{
    Q_ASSERT(m_service);    // 注入空指针是编程错误，不是运行时状况
    Q_ASSERT(m_floating);

    setWindowTitle(tr("DeskTidy —— 桌面收纳盒"));
    resize(900, 600);

    buildUi();
    connectServiceSignals();

    // 启动：确保根目录存在 -> 扫描盒子 -> 恢复上次选中的盒。
    QString err;
    if (!BoxManager::ensureRoot(CoreNames::boxRoot(), &err)) {
        // 根目录建不出来不是致命错误（可能只是权限），照常启动让主人看到界面。
        updateStatus(tr("无法创建收纳根目录：%1").arg(err));
    }

    refreshBoxes(m_service->settings()->lastBoxName());
    refreshItems();
    updateUndoButton();

    if (m_boxList->count() == 0) {
        updateStatus(tr("还没有收纳盒。先点「新建收纳盒」建一个，再点「收纳桌面」。"));
    } else {
        updateStatus(tr("就绪。桌面共 %1 项待整理。")
                         .arg(DeskScanner::scan(CoreNames::desktopRoot(),
                                                CoreNames::publicDesktopRoot(),
                                                m_service->settings()->excludedNames(),
                                                CoreNames::boxRoot())
                                  .size()));
    }
}

MainWindow::~MainWindow()
{
    // 不需要 delete 任何 core 对象：AppService 由 main.cpp 拥有，
    // 且声明在窗口之前，会晚于本窗口析构。子控件由 Qt 父子关系自动回收。
}

void MainWindow::setTray(TrayIcon *tray)
{
    m_tray = tray;
}

// ---------------------------------------------------------------------------
// 关闭行为：有托盘则隐藏常驻，无托盘则老实退出
// ---------------------------------------------------------------------------
void MainWindow::closeEvent(QCloseEvent *event)
{
    // 托盘可用 = 程序还在后台有入口，关掉窗口不等于关掉程序。
    // 这正是"常驻"的语义：主窗口只是控制中心，浮窗才是常驻在桌面上的东西。
    if (m_tray && m_tray->isAvailable()) {
        event->ignore();    // 不真的关闭（否则 Qt 会走销毁流程）
        hide();

        // 只在第一次告知"程序还在后台"。
        // 之后每次都弹的话，本来贴心的提示就成了骚扰。
        m_tray->showBackgroundHintOnce();
        return;
    }

    // ⚠️ 没有托盘时**必须主动退出**，不能只 accept 事件了事。
    //
    // 这是本阶段最容易写错的一处：main.cpp 关掉了 setQuitOnLastWindowClosed，
    // 所以"窗口全关"不再自动结束进程。如果这里只是 accept，主窗口会消失
    // 而进程留在后台 —— 任务栏、Alt+Tab、托盘里都找不到它，一个既占资源
    // 又关不掉的幽灵进程，只能靠任务管理器杀。
    //
    // 拿不到系统托盘并非理论情况：远程桌面会话、部分精简版系统、
    // 组策略禁用托盘都会走到这条路上。
    //
    // 退出前做一次与托盘退出等价的收尾：浮窗几何兜底落盘。
    // 不能直接 quit 了事 —— 那样去抖定时器里还没写下去的位置就丢了。
    // closeAll 留给 Qt 的析构流程处理（浮窗归 FloatingBoxManager，
    // 它声明在窗口之后析构，那时才轮到它退场）。
    m_floating->saveAllGeometry();

    event->accept();
    QApplication::quit();
}

// ---------------------------------------------------------------------------
// 订阅状态中枢
// ---------------------------------------------------------------------------
void MainWindow::connectServiceSignals()
{
    // 撤销栈一变，按钮的可用性与文案就要跟着变。
    connect(m_service, &AppService::undoStateChanged,
            this, &MainWindow::updateUndoButton);

    // 盒内容变了。传空串表示"不知道具体哪个盒"（撤销/还原场景），此时全刷。
    connect(m_service, &AppService::boxContentsChanged,
            this, [this](const QString &boxPath) {
                Q_UNUSED(boxPath);

                // 关于重复刷新的取舍：
                // AppService 的信号是发给"所有消费者"的（主窗口 + N 个浮窗），
                // 它只知道 boxPath，不可能知道主窗口"希望刷新后选中哪个盒"。
                // 所以这里**不再传 preferSelect**，改用另一个办法保住选中项：
                // refreshBoxes("") 会走 BoxListWidget::setBoxes 内部自带的
                // "记住刷新前选中项并尽量恢复"逻辑（见 boxlistwidget.cpp）。
                //
                // 选择"统一走信号"而不是"信号 + 显式刷新各来一份"，是为了
                // 保证一次操作只刷新一次 —— 否则每加一个消费者，刷新次数就翻倍。
                //
                // ⚠️ 这里**不**再显式调 refreshItems()：
                // BoxListWidget::setBoxes 末尾会补发一次 boxSelectionChanged
                // （见 boxlistwidget.cpp:68），那条信号会走到
                // onBoxSelectionChanged -> refreshItems()，右栏已经刷过了。
                // 若在此处再加一句，右栏就会重新扫两遍目录 ——
                // 盒里文件多时这是实打实的浪费。
                refreshBoxes(QString());
            });

    // 一次移动结束，把结果铺到状态栏（有失败项时还会弹清单）。
    connect(m_service, &AppService::moveFinished,
            this, &MainWindow::reportMoveResult);
}

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *layout  = new QVBoxLayout(central);
    layout->setContentsMargins(8, 8, 8, 4);
    layout->setSpacing(8);

    // ---- 顶部工具栏 ----
    auto *topBar = new QHBoxLayout;
    topBar->setSpacing(8);

    m_newBoxBtn   = new QPushButton(tr("新建收纳盒"), central);
    m_collectBtn  = new QPushButton(tr("收纳桌面"), central);
    m_undoBtn     = new QPushButton(tr("撤销上次收纳"), central);
    m_settingsBtn = new QPushButton(tr("设置"), central);

    // 「收纳桌面」是主操作，视觉上给它主色，让眼睛第一眼就落在这里。
    m_collectBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: #1A73E8; color: white; border: none;"
        " border-radius: 4px; padding: 6px 16px; font-weight: bold; }"
        "QPushButton:hover { background: #1765CC; }"
        "QPushButton:pressed { background: #14539F; }"
        "QPushButton:disabled { background: #C6D4E6; color: #F1F3F4; }"));

    for (QPushButton *btn : {m_newBoxBtn, m_undoBtn, m_settingsBtn}) {
        btn->setStyleSheet(QStringLiteral(
            "QPushButton { padding: 6px 14px; border: 1px solid #DADCE0;"
            " border-radius: 4px; background: #FFFFFF; }"
            "QPushButton:hover { background: #F1F3F4; }"
            "QPushButton:pressed { background: #E8EAED; }"
            "QPushButton:disabled { color: #BDC1C6; background: #F8F9FA; }"));
    }

    topBar->addWidget(m_newBoxBtn);
    topBar->addWidget(m_collectBtn);
    topBar->addWidget(m_undoBtn);
    topBar->addStretch(1);
    topBar->addWidget(m_settingsBtn);
    layout->addLayout(topBar);

    connect(m_newBoxBtn,   &QPushButton::clicked, this, &MainWindow::onNewBox);
    connect(m_collectBtn,  &QPushButton::clicked, this, &MainWindow::onCollectDesktop);
    connect(m_undoBtn,     &QPushButton::clicked, this, &MainWindow::onUndoLast);
    connect(m_settingsBtn, &QPushButton::clicked, this, &MainWindow::onOpenSettings);

    // ---- 左右分栏 ----
    m_splitter = new QSplitter(Qt::Horizontal, central);
    m_boxList  = new BoxListWidget(m_splitter);
    // 主窗口用默认 Options：双击 = 还原，不可拖出。行为与改造前一致。
    m_itemList = new ItemListWidget(m_splitter);

    m_splitter->addWidget(m_boxList);
    m_splitter->addWidget(m_itemList);
    // 左栏窄、右栏宽：盒子名短，条目名长。
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({260, 620});
    m_splitter->setChildrenCollapsible(false);   // 别让主人不小心把某一栏拖没了

    layout->addWidget(m_splitter, 1);
    setCentralWidget(central);

    connect(m_boxList, &BoxListWidget::boxSelectionChanged,
            this, &MainWindow::onBoxSelectionChanged);
    connect(m_itemList, &ItemListWidget::restoreRequested,
            this, &MainWindow::onRestoreRequested);
    connect(m_itemList, &ItemListWidget::filesDropped,
            this, &MainWindow::onFilesDropped);

    // 左栏右键菜单：浮窗开关入口。
    // 用 CustomContextMenu 而不是重写 contextMenuEvent —— 这是 QListWidget 的
    // 标准做法，且能拿到"点在哪个条目上"的坐标，不必自己去算 itemAt。
    m_boxList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_boxList, &BoxListWidget::customContextMenuRequested,
            this, &MainWindow::onBoxListContextMenu);

    // 浮窗开关状态变了：刷新左栏，让列表项上的浮窗标记跟上。
    connect(m_floating, &FloatingBoxManager::boxWindowToggled,
            this, [this](const QString &boxName, bool open) {
                Q_UNUSED(boxName);
                Q_UNUSED(open);
                // 本阶段列表项不做视觉标记，但刷新一次能让选中态保持正确。
                // 若将来要在盒名后加"（浮窗中）"之类的后缀，就在这里改。
                refreshBoxes(QString());
            });

    // 浮窗右键菜单里的「在控制中心中显示」：把主窗口提到前面并选中该盒。
    connect(m_floating, &FloatingBoxManager::revealInControlCenterRequested,
            this, [this](const QString &boxName) {
                showNormal();
                raise();
                activateWindow();
                refreshBoxes(boxName);
                refreshItems();
                updateStatus(tr("已定位到收纳盒「%1」。").arg(boxName));
            });

    // 浮窗右键菜单里的「更多设置…」：打开外观对话框，并预先选中那个盒。
    // 不自己 new 对话框、也不自己写配置 —— 与左栏右键走同一个槽，
    // 于是两个入口的默认选中规则、读写路径、广播效果必然一致。
    connect(m_floating, &FloatingBoxManager::openAppearanceDialogRequested,
            this, &MainWindow::onOpenAppearanceDialog);

    // 外观变了：刷新左栏即可。
    // 具体"哪个盒变成什么样"由 FloatingBoxManager 直接驱动对应浮窗，
    // 主窗口这边没有需要同步的显示项（列表项当前不带外观标记）。
    // 留这条连接是为了：日后若在盒名后加"（大图标）"之类的后缀，
    // 改这一处就够，不必再去追所有可能改外观的地方。
    connect(m_floating, &FloatingBoxManager::boxAppearanceChanged,
            this, [this](const QString &boxName) {
                Q_UNUSED(boxName);
                refreshBoxes(QString());
            });

    // ---- 状态栏 ----
    m_statusLabel = new QLabel(this);
    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->setSizeGripEnabled(true);
}

StorageBox MainWindow::currentBox() const
{
    if (!m_boxList) {
        return StorageBox{};
    }
    return m_boxList->currentBox();
}

void MainWindow::refreshBoxes(const QString &preferSelect)
{
    const QList<StorageBox> boxes = BoxManager::listBoxes(CoreNames::boxRoot());
    m_boxList->setBoxes(boxes);

    // 优先恢复指定盒（新建后 / 启动时恢复上次），否则交给 setBoxes 的保留逻辑。
    if (!preferSelect.isEmpty()) {
        if (!m_boxList->selectBoxByName(preferSelect)) {
            // 上一次用的盒子已被主人在资源管理器里删掉：忽略即可，不是错误。
        }
    }
}

void MainWindow::refreshItems()
{
    const StorageBox box = currentBox();
    if (box.path.isEmpty()) {
        m_itemList->setItems({});
        m_collectBtn->setEnabled(false);
        return;
    }
    m_collectBtn->setEnabled(true);
    m_itemList->setItems(BoxManager::listBoxItems(box.path));
}

void MainWindow::updateUndoButton()
{
    // 数据来源改为 AppService；文案逻辑保持原样（文案改进是后续阶段的事）。
    const bool can = m_service->canUndo();
    m_undoBtn->setEnabled(can);
    m_undoBtn->setText(can
                           ? tr("撤销上次收纳（%1 项）").arg(m_service->undoCount())
                           : tr("撤销上次收纳"));
    if (can) {
        m_undoBtn->setToolTip(tr("把上一次收纳进「%1」的 %2 项还原回桌面\n（发生于 %3）")
                                  .arg(m_service->undoBoxName())
                                  .arg(m_service->undoCount())
                                  .arg(m_service->undoTime().toString(
                                      QStringLiteral("HH:mm:ss"))));
    } else {
        m_undoBtn->setToolTip(tr("暂无可撤销的收纳。"));
    }
}

void MainWindow::updateStatus(const QString &text)
{
    if (m_statusLabel) {
        m_statusLabel->setText(text);
    }
}

void MainWindow::onBoxSelectionChanged(const QString &boxName)
{
    if (!boxName.isEmpty()) {
        m_service->settings()->setLastBoxName(boxName);
    }
    refreshItems();
}

// ---------------------------------------------------------------------------
// 新建收纳盒
// ---------------------------------------------------------------------------
void MainWindow::onNewBox()
{
    bool ok = false;
    const QString raw = QInputDialog::getText(this,
                                              tr("新建收纳盒"),
                                              tr("给收纳盒起个名字："),
                                              QLineEdit::Normal,
                                              tr("临时"),
                                              &ok);
    if (!ok || raw.trimmed().isEmpty()) {
        return;     // 主人取消，什么都不做
    }

    QString err;
    StorageBox box;
    if (!BoxManager::createBox(CoreNames::boxRoot(), raw, &box, &err)) {
        QMessageBox::warning(this, tr("新建收纳盒失败"), err);
        return;
    }

    // 新建盒子走的是"目录层面"的变更，AppService 不知道（它只管移动文件），
    // 所以这里仍显式刷新，并指定新盒为选中项。
    refreshBoxes(box.name);     // 新建后自动选中
    refreshItems();
    updateStatus(tr("已就绪收纳盒「%1」。").arg(box.name));
}

// ---------------------------------------------------------------------------
// 收纳桌面
// ---------------------------------------------------------------------------
void MainWindow::onCollectDesktop()
{
    const StorageBox box = currentBox();
    if (box.path.isEmpty()) {
        QMessageBox::information(this,
                                 tr("还没有选中收纳盒"),
                                 tr("请先在左侧选中一个收纳盒，或点「新建收纳盒」建一个。"));
        return;
    }

    const QList<DesktopEntry> found = DeskScanner::scan(CoreNames::desktopRoot(),
                                                        CoreNames::publicDesktopRoot(),
                                                        m_service->settings()->excludedNames(),
                                                        CoreNames::boxRoot());
    if (found.isEmpty()) {
        QMessageBox::information(this, tr("桌面已经很干净了"),
                                 tr("没有找到需要收纳的条目。"));
        updateStatus(tr("桌面已经很干净了，没有可收纳的条目。"));
        return;
    }

    PreviewDialog dlg(found, box.name, this);
    if (dlg.exec() != QDialog::Accepted) {
        updateStatus(tr("已取消收纳。"));
        return;
    }

    collectEntries(dlg.selectedEntries(), box.path, box.name);
}

// ---------------------------------------------------------------------------
// 拖拽归入：不做二次确认（主人已经在主动拖了），失败仍照常报告
// ---------------------------------------------------------------------------
void MainWindow::onFilesDropped(const QStringList &paths)
{
    const StorageBox box = currentBox();
    if (box.path.isEmpty()) {
        QMessageBox::information(this,
                                 tr("还没有选中收纳盒"),
                                 tr("请先在左侧选中一个收纳盒，再拖入文件。"));
        return;
    }

    // 组装成 DesktopEntry：来源按文件真实所在的桌面目录判定，
    // 这样将来还原时能回到正确的桌面（用户桌面 or 公共桌面）。
    QList<DesktopEntry> entries;
    entries.reserve(paths.size());
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.exists()) {
            continue;       // 拖拽过程中源已被删/移走
        }
        DesktopEntry e;
        e.filePath = info.absoluteFilePath();
        e.name     = info.fileName();
        e.isDir    = info.isDir();
        e.size     = e.isDir ? 0 : info.size();
        e.modified = info.lastModified();
        e.origin   = CoreNames::isPathInside(e.filePath, CoreNames::publicDesktopRoot())
                         ? EntryOrigin::PublicDesktop
                         : EntryOrigin::UserDesktop;
        entries << e;
    }

    if (entries.isEmpty()) {
        updateStatus(tr("拖入的项已不存在，未做任何操作。"));
        return;
    }

    collectEntries(entries, box.path, box.name);
}

// ---------------------------------------------------------------------------
// 收纳的公共执行路径
// ---------------------------------------------------------------------------
void MainWindow::collectEntries(const QList<DesktopEntry> &entries,
                                const QString &boxPath,
                                const QString &boxName)
{
    if (entries.isEmpty()) {
        updateStatus(tr("没有选中任何条目，未做任何操作。"));
        return;
    }

    // 只调 service：搬文件、推撤销栈、发信号都在它内部完成。
    // 界面刷新与结果上报由 connectServiceSignals 里那三条连接驱动，
    // 此处**不再**手工调 refreshBoxes / refreshItems / updateUndoButton，
    // 否则同一次操作会刷新两遍（信号一次、显式一次）。
    m_service->collectInto(entries, boxPath, boxName);
}

// ---------------------------------------------------------------------------
// 单个还原（右栏双击）
// ---------------------------------------------------------------------------
void MainWindow::onRestoreRequested(const QString &path)
{
    if (path.isEmpty()) {
        return;
    }

    // 关键取舍：单个还原时，这条记录**来自用户桌面还是公共桌面已无从得知** ——
    // "来源"只在收纳那一刻存在于内存里，程序重启或收了第二批后就丢了，
    // 而需求已明确不做持久化。故此处保守地还原到用户桌面（显式传桌面路径）。
    //
    // 后果：从公共桌面收进来的项，单个还原会落到用户桌面而非原位。
    // 这是可接受的 —— 文件没丢、没被改名、主人在桌面上照样能看到它。
    // 真正的"批"还原走撤销栈，那里保留了 origin，不受此限制。
    //
    // 注意：字段方向的构造已收进 AppService::restorePaths（那里有详细注释
    // 说明 Collector::restore 的入参语义），本处不再自己拼 MoveRecord，
    // 免得两个地方各写一份、日后改一处漏一处。
    m_service->restorePaths({path}, CoreNames::desktopRoot());

    // 刷新与上报同样由信号驱动，见 connectServiceSignals。
}

// ---------------------------------------------------------------------------
// 撤销上次收纳
// ---------------------------------------------------------------------------
void MainWindow::onUndoLast()
{
    if (!m_service->canUndo()) {
        return;
    }

    const int count = m_service->undoCount();
    const QString boxName = m_service->undoBoxName();

    const auto answer = QMessageBox::question(
        this,
        tr("撤销上次收纳"),
        tr("确定把上次收纳进「%1」的 %2 项还原回桌面吗？").arg(boxName).arg(count),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }

    // 执行、清栈、发信号都在 service 内完成。
    // 左栏计数由 boxContentsChanged 驱动刷新（撤销传空串 = 全部刷新），
    // 所以这里不再需要原先那句 refreshBoxes(boxName)。
    m_service->undoLast();
}

// ---------------------------------------------------------------------------
// 设置：只编辑"排除名称"清单
// ---------------------------------------------------------------------------
void MainWindow::onOpenSettings()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("设置"));
    // 比原来的 360 高一点：新加了"启用界面动画"这一行，不留余量会把
    // 排除清单的编辑框挤得只剩两行可见。
    // 后来又加了"悬停自动展开"一行，再放 20px 余量。
    dlg.resize(460, 420);

    auto *layout = new QVBoxLayout(&dlg);

    // ---- 界面动画总开关 ----
    // 放在排除清单**之前**：它是更基础的显示偏好，
    // 而排除清单是"这个工具具体怎么工作"的规则，前者更靠上更自然。
    auto *anim = new QCheckBox(tr("启用界面动画"), &dlg);
    anim->setChecked(m_service->settings()->animationsEnabled());
    anim->setToolTip(tr("关闭后，浮窗的出现与透明度变化会立即生效，不再有过渡效果。"));
    layout->addWidget(anim);

    // ---- 悬停自动展开 ----
    // 紧跟在动画开关之后：两者都是"浮窗怎么表现"的全局偏好，同类相邻。
    //
    // 与浮窗右键菜单里那一项是**同一个配置键**，两处都能改、改完都会广播 ——
    // 这正是"两个入口"必须收在 manager 里的理由（见 setHoverExpandEnabled）。
    auto *hover = new QCheckBox(tr("鼠标悬停时自动展开浮窗"), &dlg);
    hover->setChecked(m_service->settings()->hoverExpandEnabled());
    hover->setToolTip(tr("鼠标停在浮窗上约 0.25 秒后自动展开，移开后约 0.4 秒自动卷起。\n"
                         "手动卷起过的浮窗不会被自动展开（再手动展开一次即可恢复）。"));
    layout->addWidget(hover);

    layout->addSpacing(8);

    auto *hint = new QLabel(tr("以下名称的桌面项永远不会被收纳（每行一个，大小写不敏感）。\n"
                               "本工具的收纳根目录与自身快捷方式已被硬性排除，无需在此重复填写。"),
                            &dlg);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *editor = new QPlainTextEdit(&dlg);
    editor->setPlainText(m_service->settings()->excludedNames().join(QLatin1Char('\n')));
    editor->setPlaceholderText(tr("例如：\n重要项目\n进行中的文档"));
    layout->addWidget(editor, 1);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    box->button(QDialogButtonBox::Save)->setText(tr("保存"));
    box->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    layout->addWidget(box);

    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    QStringList names;
    const QStringList lines = editor->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            names << trimmed;
        }
    }
    m_service->settings()->setExcludedNames(names);

    // 动画开关交给 manager 写 + 广播 —— 主窗口不持有浮窗，通知不到它们。
    m_floating->setAnimationsEnabled(anim->isChecked());

    // 悬停自动展开同理，而且它与浮窗右键菜单共享同一个配置键，
    // 两处改完都必须经过 manager 广播，否则两边勾选状态会不一致。
    m_floating->setHoverExpandEnabled(hover->isChecked());

    updateStatus(tr("设置已保存，共 %1 条排除项。").arg(names.size()));
}

// ---------------------------------------------------------------------------
// 左栏右键菜单：浮窗开关入口
// ---------------------------------------------------------------------------
void MainWindow::onBoxListContextMenu(const QPoint &pos)
{
    // 用 itemAt 而不是 currentBox()：右键点在哪个盒上就该操作哪个盒，
    // 而不是"当前高亮的那一个"。若主人没先左键选中就直接右键，
    // 用 currentBox() 会操作到另一个盒上，是很隐蔽的错。
    QListWidgetItem *item = m_boxList->itemAt(pos);
    if (!item) {
        return;     // 点在空白处：不弹菜单
    }

    // 右键的同时把选中移过去，让"菜单操作的对象"和"界面上高亮的那一项"一致，
    // 否则主人会看到菜单说的是 A 盒、列表高亮的却是 B 盒。
    m_boxList->setCurrentItem(item);
    const StorageBox box = m_boxList->currentBox();
    if (box.path.isEmpty()) {
        return;
    }

    QMenu menu(this);
    menu.addAction(tr("收纳盒：%1").arg(box.name))->setEnabled(false);
    menu.addSeparator();

    const bool isOpen = m_floating->isBoxOpen(box.name);
    QAction *toggle = menu.addAction(isOpen ? tr("关闭浮窗") : tr("在桌面显示浮窗"));
    connect(toggle, &QAction::triggered, this, [this, box, isOpen]() {
        if (isOpen) {
            m_floating->closeBox(box.name);
            updateStatus(tr("已关闭「%1」的桌面浮窗。").arg(box.name));
        } else {
            m_floating->openBox(box.name, box.path);
            updateStatus(tr("已在桌面显示「%1」的浮窗。").arg(box.name));
        }
    });

    // 外观设置。放在浮窗开关之后：先决定"要不要显示"，再决定"长什么样"，
    // 顺序符合操作直觉；而且没开浮窗的盒也能先设好外观。
    QAction *appearance = menu.addAction(tr("外观设置…"));
    connect(appearance, &QAction::triggered, this, [this, box]() {
        onOpenAppearanceDialog(box.name);
    });

    // 删除收纳盒。**放在菜单最底部**（Windows 惯例：破坏性操作排最后），
    // 且前面加一道分隔线，避免与上面的日常操作挨在一起被顺手点到。
    menu.addSeparator();
    QAction *remove = menu.addAction(tr("删除收纳盒…"));
    connect(remove, &QAction::triggered, this, [this, box]() {
        onDeleteBox(box);
    });

    menu.exec(m_boxList->viewport()->mapToGlobal(pos));
}

// ---------------------------------------------------------------------------
// 删除收纳盒
//
// 这是全应用唯一会"让文件离开收纳盒"的破坏性操作，因此流程刻意做得重：
//   1) 先统计盒里有多少东西 —— 决定确认强度，也是给主人的第一个交代；
//   2) 确认（空盒普通确认；**非空要求手输盒名**）；
//   3) 交给 AppService::deleteBox（还原回桌面 + 兜底进回收站）；
//   4) 失败就明确报错、什么都不改；
//   5) 成功才收尾：关浮窗 -> 清外观配置 -> 刷新左栏 -> 汇报。
//
// 第 5 步的顺序不能反。若先关了浮窗再删，而删除又失败，主人会看到
// "浮窗没了但盒子还在"——状态分裂，比直接报错难查得多。
// ---------------------------------------------------------------------------
void MainWindow::onDeleteBox(const StorageBox &box)
{
    // ---- 1. 统计 ----
    // 用 listBoxItems 而不是 count()：前者带 QDir::Hidden，隐藏文件也算数。
    // 少算了会让主人以为"这个盒是空的"从而放松警惕，而删除时那些文件
    // 会一起进回收站 —— 数字必须与实际会动到的东西一致。
    const int itemCount = BoxManager::listBoxItems(box.path).size();

    // ---- 2. 确认 ----
    if (!confirmBoxDeletion(box, itemCount)) {
        updateStatus(tr("已取消删除「%1」。").arg(box.name));
        return;
    }

    // ---- 3. 执行 ----
    const AppService::BoxDeletionResult result =
        m_service->deleteBox(box.path, CoreNames::desktopRoot());

    // ---- 4. 失败：明确报错，什么都不改 ----
    if (!result.ok()) {
        QMessageBox::warning(this, tr("删除失败"), result.error);

        // 即使失败也可能已经有一部分文件被还原到桌面了（还原成功、
        // 但盒目录丢不进回收站）。这不回滚 —— 那些文件此刻好好躺在桌面上，
        // 再搬回去只会让状态更乱。但必须让主人知道，否则他会以为"什么都没发生"。
        if (result.restored > 0) {
            QMessageBox::information(
                this, tr("部分文件已还原"),
                tr("在放弃删除之前，已经把 %1 项还原到了桌面。\n\n"
                   "收纳盒本身**没有**被删除，其余内容仍在里面。")
                    .arg(result.restored));
        }

        updateStatus(tr("删除「%1」失败。").arg(box.name));
        return;
    }

    // ---- 5. 成功才收尾 ----
    // 先关浮窗：盒目录已经不在磁盘上了，留着浮窗只会显示一个空壳
    // （它的自愈逻辑也会把它关掉，但显式关掉语义更清楚）。
    m_floating->closeBox(box.name);

    // 清掉该盒的外观配置。不清的话，将来建一个同名盒会"继承"上一个盒的外观，
    // 那个盒早已不存在，主人会觉得莫名其妙。
    m_service->settings()->clearFloatAppearance(box.name);

    // 左栏刷新。删盒是"目录层面"的变更，AppService 不知道（它只管移动文件），
    // 所以这里必须显式刷一次，否则删掉的盒还挂在列表里。
    refreshBoxes();

    // ---- 汇报 ----
    reportBoxDeletion(box, result);
}

// ---------------------------------------------------------------------------
// 删除确认
//
// 盒子非空时**要求手输盒名**，不是走形式：
// 非空删除会移动真实文件，中途出问题（跨卷、被占用）主人看到的是"东西乱了"。
// 手输盒名强制他确认自己删对了盒 —— 防的是"删错盒"这个最贵的错误。
//
// 空盒不要求：它不移动任何文件，只是把一个空目录丢进回收站，可完全还原，
// 要人输名字属于为难人。
// ---------------------------------------------------------------------------
bool MainWindow::confirmBoxDeletion(const StorageBox &box, int itemCount)
{
    // ---- 空盒：普通确认框 ----
    // 空盒不移动任何文件，只是把一个空目录丢进回收站（还能还原），
    // 要人输名字属于为难人。
    if (itemCount <= 0) {
        QMessageBox ask(this);
        ask.setIcon(QMessageBox::Warning);
        ask.setWindowTitle(tr("删除收纳盒"));
        ask.setWindowModality(Qt::WindowModal);
        ask.setText(tr("确定要删除收纳盒「%1」吗？").arg(box.name));
        ask.setInformativeText(tr("这个盒子是空的。\n"
                                  "盒目录会被移入回收站，之后可以还原。"));

        // 用 addButton 而不是 setStandardButtons + setButtonText：
        // 后者在 Qt6 里已弃用（会报 -Wdeprecated-declarations），
        // 本项目的规矩是零警告。
        QPushButton *delBtn  = ask.addButton(tr("删除"), QMessageBox::AcceptRole);
        QPushButton *cancelBtn = ask.addButton(tr("取消"), QMessageBox::RejectRole);
        ask.setDefaultButton(cancelBtn);    // 回车不该是危险操作
        Q_UNUSED(delBtn);

        ask.exec();
        return ask.clickedButton() == delBtn;
    }

    // ---- 非空盒：要求手输盒名 ----
    //
    // 为什么不用 QMessageBox 硬塞输入框：往它内部布局 addWidget 依赖 Qt 的
    // 实现细节（按钮行、图标列的位置），换个 Qt 版本就可能错位。
    // 自己搭一个 QDialog 只多十几行，但每一处位置都是明确的。
    QDialog dlg(this);
    dlg.setWindowTitle(tr("删除收纳盒"));
    dlg.setWindowModality(Qt::WindowModal);
    dlg.resize(460, 300);

    auto *layout = new QVBoxLayout(&dlg);

    auto *title = new QLabel(
        tr("<b>收纳盒「%1」里有 %2 个项目。</b>").arg(box.name).arg(itemCount), &dlg);
    title->setWordWrap(true);
    layout->addWidget(title);

    auto *detail = new QLabel(
        tr("删除后，这些项目会被<b>还原回桌面</b>\n"
           "（若桌面已有同名文件，会自动加序号，不会覆盖）。\n\n"
           "还原不回去的会连同盒目录一起移入回收站，之后可以还原。\n\n"
           "⚠️ 此操作不可撤销。"),
        &dlg);
    detail->setWordWrap(true);
    layout->addWidget(detail);

    layout->addStretch();

    auto *prompt = new QLabel(tr("请输入盒名「%1」以确认：").arg(box.name), &dlg);
    layout->addWidget(prompt);

    auto *input = new QLineEdit(&dlg);
    input->setPlaceholderText(box.name);
    layout->addWidget(input);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("删除"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    layout->addWidget(buttons);

    QPushButton *okBtn = buttons->button(QDialogButtonBox::Ok);
    okBtn->setEnabled(false);       // 未输入前禁用，防手滑

    // 输入与盒名**完全相等**才放行。区分大小写：盒名本身已经过
    // sanitizeBoxName 规范化，严格匹配能有效挡住"随手敲两个字符就回车"。
    connect(input, &QLineEdit::textChanged, okBtn,
            [okBtn, &box](const QString &text) {
                okBtn->setEnabled(text == box.name);
            });
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    // 默认焦点给输入框（主人本来就要在这里打字），
    // 而「取消」是 Esc 的默认行为 —— 回车因为按钮被禁用而无效，是安全的。
    input->setFocus();

    return dlg.exec() == QDialog::Accepted;
}

// ---------------------------------------------------------------------------
// 删除结果汇报
// ---------------------------------------------------------------------------
void MainWindow::reportBoxDeletion(const StorageBox &box,
                                   const AppService::BoxDeletionResult &result)
{
    if (result.trashedItems == 0) {
        // 全都还原回桌面了，没有东西进回收站 —— 不打扰，状态栏交代一句就够。
        updateStatus(tr("已删除收纳盒「%1」，%2 项已还原到桌面。")
                         .arg(box.name).arg(result.restored));
        return;
    }

    // 有东西没还原成、进了回收站：必须说清楚，否则主人会以为文件丢了。
    // 用 Information 而不是 Warning —— 这不是错误，是设计好的兜底路径。
    QMessageBox done(this);
    done.setIcon(QMessageBox::Information);
    done.setWindowTitle(tr("收纳盒已删除"));
    done.setText(tr("已删除收纳盒「%1」。").arg(box.name));
    done.setInformativeText(
        tr("已还原 %1 项到桌面。\n\n"
           "另有 %2 项无法还原（可能正被其他程序占用，或目标与源不在\n"
           "同一磁盘分区），已连同盒目录一起移入回收站，可以从回收站取回。")
            .arg(result.restored).arg(result.trashedItems));
    done.setStandardButtons(QMessageBox::Ok);
    done.exec();

    updateStatus(tr("已删除收纳盒「%1」：还原 %2 项，%3 项进了回收站。")
                     .arg(box.name).arg(result.restored).arg(result.trashedItems));
}

void MainWindow::onOpenAppearanceDialog(const QString &boxName)
{
    // 盒清单现扫磁盘，不缓存 —— 与左栏刷新用的是同一个数据源，
    // 于是"列表里看到的"和"对话框里能选的"必然一致。
    // 若改成缓存，刚在主窗口建完盒就打开对话框会看不到它。
    const QList<StorageBox> boxes = BoxManager::listBoxes(CoreNames::boxRoot());

    // 没指定盒时退到当前选中的那个。两者都没有时传空串，
    // 由对话框自己退到列表第一项 —— 空状态判断收在对话框里，调用方不必操心。
    QString initial = boxName;
    if (initial.isEmpty()) {
        initial = currentBox().name;
    }

    AppearanceDialog dlg(boxes, initial, m_floating, this);
    dlg.exec();

    // 对话框内每次「应用」都已即时生效（经 manager 广播），
    // 所以关闭后不需要再统一刷一次外观。这里只更新状态栏，给一个"刚才做了什么"的交代。
    const QString applied = dlg.selectedBoxName();
    if (!applied.isEmpty()) {
        updateStatus(tr("「%1」的浮窗外观已按需更新。").arg(applied));
    }
}

// ---------------------------------------------------------------------------
// 结果汇报
// ---------------------------------------------------------------------------
void MainWindow::reportMoveResult(const QList<MoveRecord> &records, const QString &actionLabel)
{
    // 统计口径统一由 AppService::summarize 提供（浮窗也用同一份），
    // 本处只负责"怎么展示"。
    const AppService::MoveSummary summary = AppService::summarize(records);

    updateStatus(tr("%1：成功 %2 项，跳过 %3 项，失败 %4 项。")
                     .arg(actionLabel)
                     .arg(summary.ok)
                     .arg(summary.skipped)
                     .arg(summary.failed));

    // 有失败/跳过项时逐条列出原因 —— 需求明确要求，不能让主人只看到一个数字。
    if (summary.hasProblems()) {
        // 用 setDetailedText 而不是自己拼多行正文：Qt 会把它折叠进"详细信息"，
        // 失败项多时不会撑出一个占满屏幕的对话框。
        QMessageBox box(this);
        box.setIcon(summary.failed > 0 ? QMessageBox::Warning : QMessageBox::Information);
        box.setWindowTitle(actionLabel);
        box.setText(tr("成功 %1 项，跳过 %2 项，失败 %3 项。\n\n明细：")
                        .arg(summary.ok)
                        .arg(summary.skipped)
                        .arg(summary.failed));
        box.setDetailedText(summary.detailLines.join(QLatin1Char('\n')));
        box.setStandardButtons(QMessageBox::Ok);
        box.exec();
    }
}
