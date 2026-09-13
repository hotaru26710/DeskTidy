# DeskTidy 桌面常驻浮窗 —— 技术设计方案

> 状态：**待需求方确认**（有 3 处需拍板，见 §14）
> 影响范围：新增 3 个模块、新增 2 个 core 文件、改 6 个文件、改 2 份构建清单、推翻 1 条既有设计决定

---

## 0. 本方案要解决的问题

DeskTidy 目前是「开一次、整理一次、关掉」的工具，收纳盒里的东西**平时完全不可见**。本方案把「收纳盒」从控制中心里的一份列表，变成**桌面上看得见、点得着、能一直摆着的浮窗**。

---

## 1. 不做什么（边界）

| 不做 | 原因 |
|---|---|
| 开机自启 | 需求已明确不做 |
| 图标网格布局（Fences 那种） | 要自绘、处理缩放分页，另一个量级 |
| 多盒合成一个浮窗 | 需求方已选「每盒一个独立小窗」 |
| 浮窗内新建/删除盒 | 盒的生命周期仍归控制中心 |
| 记录图标桌面原坐标 | 独立的一块（`IFolderView` 内存映射） |
| 浮窗内重命名/删除条目 | 违反「不删除任何文件」铁律 |
| **接管桌面图标图层** | Fences 是 hook 桌面 ListView 的；我们只在桌面**之上**浮一层窗口。观感接近，但不动系统桌面一根汗毛 |

---

## 2. 现状核实结果（逐条对源码确认）

| 事实 | 出处 |
|---|---|
| `UndoStack` 是值语义普通类，非 QObject、无信号；`push()` 覆盖式只留一批；`undo()` 只要一条成功就清栈 | `core/undostack.h:19-45`、`undostack.cpp:18-81` |
| `UndoStack` 与 `Settings` 唯二实例以裸指针挂 MainWindow，private，无对外访问器 | `ui/mainwindow.h:83-84`、`mainwindow.cpp:45-46`、`:76-80` |
| **`MainWindow` 一个自定义信号都没有** | `ui/mainwindow.h` 全文无 `signals:` |
| 刷新是硬编码直调 | `mainwindow.cpp:342-343`、`:374-375`、`:403-405`、`:245-247`、`:219` |
| `reportMoveResult` 前半段统计**纯计算零依赖**，后半段硬编码 `updateStatus()` 与 `QMessageBox box(this)` | `mainwindow.cpp:458-506` |
| `PreviewDialog` 签名 `(entries, boxName, QWidget *parent = nullptr)`，**不依赖 MainWindow** | `ui/previewdialog.h:29-31` |
| `ItemListWidget` 双击发 `restoreRequested`；`setDragDropMode(DropOnly)`；图标经 `QFileIconProvider` 按后缀缓存；路径存 `Qt::UserRole` | `itemlistwidget.h:41`、`itemlistwidget.cpp:36`、`:55-83`、`:59` |
| `DesktopEntry` **无「可执行/快捷方式」标识** | `core/coretypes.h:27-35` |
| `main.cpp` 未设 `setQuitOnLastWindowClosed`（默认 true）；MainWindow 是栈上对象 | `main.cpp:55`、`:65-66` |
| `setQuitOnLastWindowClosed` 定义在 **`QGuiApplication`** 上 | `QtGui/qguiapplication.h:123` |
| 浮窗/托盘/置顶/无边框代码**全零命中** | 全仓库 grep |
| 托盘在 Widgets 模块内，**无需新增 Qt 模块** | `DeskTidy.pro:1`、`CMakeLists.txt:20` |

**语义陷阱**：`onUndoLast` 撤的是**全局最近一次任意盒**的收纳（`mainwindow.cpp:391-396`）。浮窗上放撤销按钮而不改文案必然误导，详见 §9。

---

## 3. 核心设计决定

### 3.1 UndoStack 所有权：引入 `AppService`

| 方案 | 问题 |
|---|---|
| A. 改单例 | 全局可变单例无法测试隔离；core 层现在全是干净静态命名空间，塞单例破坏一致性 |
| B. 浮窗回调 MainWindow | **破坏分层**；且**语义错位**——浮窗点收纳，收的却是主窗口当前选中的盒 |
| **C. 中立 `AppService`（采纳）** | 多一层间接，但语义正确 |

```cpp
// core/appservice.h —— 应用级会话状态（core 层唯一有信号的类）
class AppService : public QObject
{
    Q_OBJECT
public:
    explicit AppService(QObject *parent = nullptr);

    Settings *settings() { return &m_settings; }

    bool      canUndo() const;
    int       undoCount() const;
    QString   undoBoxName() const;
    QDateTime undoTime() const;

    // 收纳：目标盒由调用方显式传入（不是"当前选中的盒"）
    QList<MoveRecord> collectInto(const QList<DesktopEntry> &entries,
                                  const QString &boxPath,
                                  const QString &boxName);
    QList<MoveRecord> undoLast();
    QList<MoveRecord> restorePaths(const QStringList &paths,
                                   const QString &targetDir = QString());

    struct MoveSummary {
        int ok = 0, skipped = 0, failed = 0;
        QStringList detailLines;
        QString headline() const;
    };
    static MoveSummary summarize(const QList<MoveRecord> &records,
                                 const QString &actionLabel);

signals:
    void undoStateChanged();
    void boxContentsChanged(const QString &boxPath);   // 空串 = 全部刷新
    void moveFinished(const QList<MoveRecord> &records, const QString &actionLabel);

private:
    UndoStack m_undo;        // 值成员，全应用唯一
    Settings  m_settings;
};
```

**三个具体判断**：

1. **`Settings` 收进来：收。** 事实无状态，但有两个消费者，统一入口更清楚。
2. **`UndoStack` 保持纯值语义、不改 QObject：正确。** 它没有任何需要异步通知的东西。**通知是 AppService 的职责**——是 AppService 在 push/undo 后 emit，不是 UndoStack 自己发。把信号加进去是把「状态存储」和「状态广播」混在一起。
3. **`summarize()` 抽成 static 纯函数：抽。** 现在这段（`mainwindow.cpp:465-485`）是纯计算却埋在 MainWindow 里。抽出来它是本方案**唯一能自动化测试的新逻辑**。

### 3.2 生命周期与注入

MainWindow 是栈上对象，浮窗动态创建销毁，AppService 必须活得比所有窗口久。

```cpp
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // ... 元信息与字体设置不变 ...

    app.setQuitOnLastWindowClosed(false);   // 见 §3.3，必须在建窗口前

    AppService service;                  // 【关键】声明在 window 之前
    MainWindow window(&service);         // 逆序析构 => service 后死
    window.show();

    return app.exec();
}
```

> ⚠️ **声明顺序即生命周期**：`service` 必须声明在 `window` 之前。若有人调换这两行，会在退出时崩溃。**建议在该处加注释锁定约束。**

**浮窗持有者**：`FloatingBoxManager`（QObject）统一持有 `QHash<盒名, FloatingBoxWidget*>`，负责启动重建、开关、几何落盘、盒被删时销毁。MainWindow 与 TrayIcon 都不直接 new 浮窗。

### 3.3 `setQuitOnLastWindowClosed` 的坑

默认 `true`。加浮窗+托盘后会**同时破坏两件事**：关主窗口时若没有浮窗开着，进程直接退出；关掉最后一个浮窗时同理。

**另一个更隐蔽的坑**：`Qt::Tool`（`qnamespace.h:219` = `Popup | Dialog`）**不计入 `lastWindowClosed` 判定**。即使浮窗开着，主窗口一隐藏就可能触发退出。行为依赖平台，**不能靠猜**。

**方案**：`app.setQuitOnLastWindowClosed(false)`，退出只有托盘菜单一条路径 → `qApp->quit()`。

配套：主窗口 `closeEvent` 改为 `event->ignore(); hide();`，首次隐藏时 `showMessage()` 提示一次（记在 Settings，避免每次烦人）；托盘退出时先 `prepareForQuit()`（关浮窗、落盘）再 quit。

> ⚠️ **需实测**：确认「关掉主窗口+所有浮窗后进程仍在，内存不涨」。

### 3.4 打开文件：新增 `core/opener`

**不放 `Collector`**——它的定位注释写得很死（`collector.h:10-13`），塞进去会污染语义边界。

```cpp
// core/opener.h —— 用系统默认关联打开（只读，不动文件）
namespace Opener {
bool openPath(const QString &path, QString *error = nullptr);
}
```

**为什么用 `QDesktopServices::openUrl` 而非 `ShellExecuteW`**：前者是 Qt 封装，后者要引 `windows.h` 并处理 `HINSTANCE <= 32` 的错误码语义（很反直觉）。本工具不需要指定 verb / 工作目录 / SW_SHOW。**选前者。**

> ⚠️ **安全提示**：`openUrl` 会**真的运行 `.exe`**。需求方选的就是「服从系统默认关联」，这是刻意的；但 tooltip 要让用户看得出「双击 = 用系统默认方式打开」。

---

## 4. 浮窗类设计

### 4.1 类结构

```cpp
// ui/floatingboxwidget.h —— 一个浮窗 = 一个收纳盒的桌面化身
class FloatingBoxWidget : public QWidget
{
    Q_OBJECT
public:
    FloatingBoxWidget(AppService *service, const QString &boxName,
                      const QString &boxPath, QWidget *parent = nullptr);
    QString boxName() const;
    QString boxPath() const;
    void refreshItems();
    void applySavedGeometry(const QByteArray &blob);

signals:
    void closeRequested(const QString &boxName);
    void geometryChanged(const QString &boxName, const QByteArray &blob);

protected:
    void closeEvent(QCloseEvent *) override;        // 转成 closeRequested
    void moveEvent(QMoveEvent *) override;          // 几何去抖
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;   // 自绘拖动
    void mouseMoveEvent(QMouseEvent *) override;

private slots:
    void onOpenRequested(const QString &path);      // 双击 -> Opener::openPath
    void onCollectClicked();                        // 收进"我代表的这个盒"
    void onUndoClicked();
    void onRollUpClicked();
    void onRestoreRequested(const QString &path);   // 右键菜单

private:
    AppService     *m_service = nullptr;
    QString         m_boxName, m_boxPath;
    QLabel         *m_titleLabel = nullptr;
    QPushButton    *m_rollUpBtn = nullptr, *m_closeBtn = nullptr;
    QPushButton    *m_collectBtn = nullptr, *m_undoBtn = nullptr;
    ItemListWidget *m_itemList = nullptr;
    QPoint          m_dragOffset;
    bool            m_dragging = false, m_rolledUp = false;
    int             m_expandedHeight = 0;
    QTimer         *m_geometryDebounce = nullptr;
    QByteArray      m_pendingGeometry;
};
```

### 4.2 复用 `ItemListWidget`（内嵌实例）

它已把「`QFileIconProvider` 取系统图标 + 按后缀缓存」做好（`itemlistwidget.cpp:55-83`），另写必然出现两套图标逻辑；且它已是「只发信号、不动文件」的纯展示控件（`itemlistwidget.h:13`）。

**必须改造三处，原则是「新增能力用参数 opt-in，默认行为一个字不改」**：

| 现状 | 改法 |
|---|---|
| 双击发 `restoreRequested` | 加 `enum class DoubleClickAction { Restore, Open }` 构造参数，**默认 `Restore`** |
| `setDragDropMode(DropOnly)` | 改为 `DragDrop` + 实现 `startDrag`，同样用参数控制 |
| tooltip 硬编码「双击还原回桌面」 | 文案随 `DoubleClickAction` 变化 |

### 4.3 无边框标题栏

`setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)`，失去系统标题栏，需自绘：

```
┌──────────────────────────────────────────┐
│ ⠿ 临时           3 项      [−] [✕]        │  ← 自绘标题栏 28px
├──────────────────────────────────────────┤
│ [收纳桌面] [撤销上次收纳（3 项）]           │  ← 操作条 26px
├──────────────────────────────────────────┤
│  report.docx / data.txt / photos          │  ← ItemListWidget
└──────────────────────────────────────────┘
   ↑ 右下角 QSizeGrip
```

- **拖动**：**只在标题栏区域内**响应鼠标拖动，否则会与列表拖拽/框选打架。
- **调整大小**：角落放 `QSizeGrip`。
- **卷起（Roll-up）**：记录 `m_expandedHeight`，隐藏操作条与列表、`resize(width(), 标题栏高)`。双击标题栏切换。成本很低，**第一期就做**。
- **置顶取舍**：`WindowStaysOnTopHint` 会盖住**全屏视频和游戏**，会招抱怨。**推荐**：默认置顶 + 右键菜单可关。

> ⚠️ **需实测**：三个 flag 在 Win11 上的组合效果（是否真不占任务栏、是否进 Alt+Tab）。Qt 文档对此含糊。

---

## 5. 收纳与打开的语义

### 5.1 浮窗「收纳桌面」收进自己代表的盒

`collectInto(entries, boxPath, boxName)` 的**目标盒由调用方显式传入**，正好绕开 `onCollectDesktop()` 对「当前选中盒」的依赖。

```
1. DeskScanner::scan(用户桌面, 公共桌面, settings->excludedNames(), boxRoot)
2. 空 -> 「桌面已经很干净了」
3. PreviewDialog dlg(found, m_boxName, parent)
4. accepted -> service->collectInto(dlg.selectedEntries(), m_boxPath, m_boxName)
5. AppService emit boxContentsChanged + undoStateChanged + moveFinished
6. 浮窗与主窗口各自刷新
```

`PreviewDialog` 只依赖传入的 `QWidget *parent`（`previewdialog.h:31`），浮窗做 parent 没问题。

> ⚠️ **需实测**：`PreviewDialog` 是**模态**的（`exec()`）。以 `Qt::Tool` 浮窗为模态 parent 在 Windows 上可能出现「对话框跑到浮窗后面」。**规避**：改以 MainWindow 为 parent，或 parent 传 `nullptr` + `activateWindow()`。**需实测哪种最好。**

### 5.2 浮窗双击 = 打开（语义差异是刻意的）

| 窗口 | 双击 | 理由 |
|---|---|---|
| 主窗口 | **还原回桌面** | 主窗口是「管理」界面 |
| 浮窗 | **用系统默认方式打开** | 浮窗是「使用」界面 |

两处 tooltip 必须写清楚，浮窗列表上方应有一句极短提示。

---

## 6. 拖出还原 —— 风险最高的一处

### 6.2 核心风险：资源管理器会怎么处理这个 drop？（**已实测，结论已更新**）

**⚠️ 本节曾经的不确定性已由两轮实测消除。以下是实测结论，不再是推测。**

**第一轮实测（默认行为）**：直接把文件拖到资源管理器文件夹，`exec()` 返回 `1`（`CopyAction`），源文件仍在，目标处出现副本。
→ **结论：Windows 默认按"复制"处理。需求方要的"拖到哪就还原到哪"用默认方式做不到。**

**第二轮实测（注入 Preferred Drop Effect）**：通过自定义 `QWindowsMimeConverter` 子类，在 Qt 把 `QMimeData` 翻译成 Windows `IDataObject` 时额外挂上 `CFSTR_PREFERREDDROPEFFECT = DROPEFFECT_MOVE`，再拖拽：

| 验证点 | 实测结果 |
|---|---|
| `exec()` 返回值 | `2`（`MoveAction`） |
| 源文件 | 已消失 |
| **目标文件夹** | **文件确实在这里**（31 字节） |
| 桌面根目录 | 无散落 |

→ **结论：注入方案有效，「拖到哪就还原到哪」可以完整实现。**

**关键实现细节**（`tools/drag_probe2.cpp` 是可运行的验证样例）：

```cpp
class PreferredDropEffectMime : public QWindowsMimeConverter
{
    // Qt -> Windows：把"移动"意图翻译成原生格式
    bool canConvertFromMime(const FORMATETC &fmt, const QMimeData *) const override
    { return fmt.cfFormat == m_registeredFormat; }

    bool convertFromMime(const FORMATETC &fmt, const QMimeData *,
                         STGMEDIUM *pmedium) const override
    {
        if (fmt.cfFormat != m_registeredFormat || !pmedium) return false;
        DWORD effect = DROPEFFECT_MOVE;              // ← 关键
        HGLOBAL h = GlobalAlloc(GMEM_SHARE | GMEM_MOVEABLE, sizeof(DWORD));
        if (!h) return false;
        void *p = GlobalLock(h);
        if (!p) { GlobalFree(h); return false; }
        memcpy(p, &effect, sizeof(DWORD));
        GlobalUnlock(h);
        pmedium->tymed = TYMED_HGLOBAL; pmedium->hGlobal = h;
        pmedium->pUnkForRelease = nullptr;
        return true;
    }

    // 只在我们的私有 mime 出现时才附加该格式，避免污染普通拖拽
    QList<FORMATETC> formatsForMime(const QString &mimeType, const QMimeData *) const override
    {
        if (mimeType != QStringLiteral("application/x-desktidy-restore")) return {};
        FORMATETC f = {}; f.cfFormat = m_registeredFormat;
        f.dwAspect = DVASPECT_CONTENT; f.lindex = -1; f.tymed = TYMED_HGLOBAL;
        return { f };
    }
    // 其余三个纯虚函数返回 false / 空即可
};

// 启动时注册一次：
m_registeredFormat = QWindowsMimeConverter::registerMimeType(
                         QStringLiteral("Preferred Drop Effect"));
```

> `QWindowsMimeConverter` 的头文件虽带 `_P_H`（私有）后缀，但类本身是 `Q_GUI_EXPORT` 导出的，
> 且**无需链接额外库**（就在 `Qt6Gui` 内）。这是目前唯一可行的公开途径。

### 6.2.1 ⚠️ 该方案的硬约束（必须遵守，否则会丢文件）

方案成立的**前提是：由 Windows 资源管理器完成实际移动**。我们只是告诉了它"这是移动操作"。
这导致两条必须在实现中守住的规矩：

**约束一：拖到"非资源管理器"目标会失败。**
若用户把文件拖到一个不接受文件拖放的地方（某些 Qt 程序、浏览器空白区等），
对方不会执行移动，而我们的标记又让系统以为"会有人处理"。
**实测中已出现过这一现象：文件既不在源、也不在目标、也不在回收站——凭空消失。**

**约束二：绝不能在 `exec()` 返回后自己再去动源文件。**
资源管理器已经移走了，再动就是操作一个不存在的路径。

**兜底逻辑（必须实现）**：`exec()` 返回后**检查源文件是否仍存在**——

```cpp
const Qt::DropAction result = drag->exec(Qt::MoveAction, Qt::MoveAction);
const bool sourceStillThere = QFile::exists(draggedPath);

if (!sourceStillThere) {
    // 资源管理器成功移走，静默刷新列表即可
    emit itemMovedOut();
} else {
    // 对方没接手（或只复制），源仍在盒里 —— 必须告诉用户，不能让文件"看起来丢了"
    emit moveNotCompleted(draggedPath);
}
```

这条兜底是**防"文件凭空消失"这个最坏情况的最后一道闸**，不能省。

### 6.3 规避方案：标记 + 事后处理

**不依赖 shell 帮我们移动，用私有 mime 标记把控制权拿回来**：

```cpp
void ItemListWidget::startDrag(Qt::DropActions)
{
    QListWidgetItem *item = currentItem();
    if (!item) return;
    const QString path = item->data(Qt::UserRole).toString();

    auto *mime = new QMimeData;
    mime->setUrls({ QUrl::fromLocalFile(path) });
    // 【关键标记】私有 mime 类型，标识这是一次 DeskTidy 还原拖拽
    mime->setData(QStringLiteral("application/x-desktidy-restore"), path.toUtf8());

    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->setPixmap(item->icon().pixmap(32, 32));

    m_draggedPath = path;
    const Qt::DropAction result = drag->exec(Qt::MoveAction, Qt::MoveAction);
    m_draggedPath.clear();

    if (result == Qt::MoveAction)        emit externalMoveHappened();     // 情形 B
    else if (result == Qt::IgnoreAction) { /* 丢在空白处，什么都不做 */ }
    else                                 emit copyInsteadOfMoveDetected(path); // 情形 A
}
```

**兜底（重要）**：无论 A 还是 B，**都不自动删除盒里的源文件**——与「绝不删除」铁律一致。区别只在提示：

- 情形 B：静默刷新。
- 情形 A：明确提示「文件已复制到目标位置，但仍保留在收纳盒中。如需移除，请用右键菜单的『还原回桌面』。」

**代价**：情形 A 下用户会得到副本 + 盒里留一份，需手动收拾。**但这比自动删源安全得多。**

### 6.4 一条走不通的路（说明为什么）

「拖拽结束后由我们调 `Collector::restore` 移到目标目录」——**走不通**：`QDrag::exec()` 返回值**不含目标目录**，Qt 只给动作类型（Copy/Move/Ignore）。要拿目标路径只能自己实现原生 OLE `IDropTarget`（工作量爆炸）。**故第一期不承诺「拖到哪就真的移动到哪」。**

> ⚠️ **必须让需求方知情**：需求方选的是「拖到哪就还原到哪」。情形 B 下恰好完全满足；情形 A 下退化为「复制 + 提示」。**建议先做 §12 阶段 0 的实验再定案。**

---

## 7. 刷新同步机制

```
                    ┌─────────────────────────┐
                    │      AppService          │
                    │  (core, 唯一有信号的)     │
                    └───────────┬─────────────┘
                                │ · undoStateChanged()
                                │ · boxContentsChanged(boxPath)
                                │ · moveFinished(records, label)
              ┌─────────────────┼─────────────────┐
              ▼                 ▼                 ▼
    ┌──────────────┐   ┌──────────────┐  ┌──────────────┐
    │  MainWindow  │   │ FloatingBox  │  │ FloatingBox  │
    │ 刷新左右栏    │   │  (临时)       │  │  (工作)       │
    │ 更新撤销按钮  │   │ 只关心自己的  │  │ 只关心自己的  │
    └──────────────┘   └──────────────┘  └──────────────┘
```

用 `Qt::AutoConnection`（同线程直连），**不用 QueuedConnection**（会让界面慢一拍）。

| 信号 | MainWindow | FloatingBoxWidget |
|---|---|---|
| `undoStateChanged()` | `updateUndoButton()` | 更新自己的撤销按钮 |
| `boxContentsChanged(boxPath)` | `refreshBoxes()` + `refreshItems()` | `if (boxPath == m_boxPath \|\| boxPath.isEmpty()) refreshItems()` |
| `moveFinished(...)` | `updateStatus(headline())` + 有失败项弹清单 | 同上（parent 是自己） |

**带 `boxPath` 参数是为了避免无谓刷新**：三个浮窗开着时，A 盒收纳完不该让 B、C 重新扫盘。

**主窗口隐藏后刷新不出错**：隐藏的窗口仍是活对象，信号照常送达；Qt 对不可见控件不做布局计算。真正要小心的是**盒目录被外部删掉**——`listBoxItems` 会返回空（`boxmanager.h:37`），此时应让浮窗自动关闭自己（盒都没了），由 `FloatingBoxManager` 在信号里检查并销毁。

---

## 8. 托盘图标

**独立成 `ui/trayicon.h/.cpp`**。理由：MainWindow 已 506 行；且托盘生命周期与主窗口**不是一回事**（主窗口隐藏时托盘要活着）。

```cpp
class TrayIcon : public QObject
{
    Q_OBJECT
public:
    TrayIcon(AppService *service, MainWindow *window,
             FloatingBoxManager *floating, QObject *parent = nullptr);
    bool isAvailable() const;
    void syncFloatingMenu(const QStringList &openBoxNames);
signals:
    void quitRequested();
private slots:
    void onActivated(QSystemTrayIcon::ActivationReason);
    void rebuildMenu();
private:
    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_menu = nullptr;
    QMenu *m_floatingMenu = nullptr;   // 「浮窗」子菜单：每盒一个可勾选项
};
```

菜单结构：`打开控制中心 / ─── / 浮窗 ▸ (☑临时 ☐工作 ☐下载暂存) / ─── / 退出`

**退出流程**：`saveAllGeometry()` → 关闭所有浮窗 → `qApp->quit()`。
**双击托盘 = 唤出主窗口**（`restore()` + `activateWindow()` + `raise()`），Windows 通行做法。

**图标来源**：当前 `main.cpp` **没有任何程序图标**。建议先用 `QStyle::standardIcon(QStyle::SP_DesktopIcon)` 顶上，避免为图标单开 `.qrc` 资源（会牵扯构建清单再加一项）。

---

## 9. 撤销按钮的文案（必须改）

撤销栈**全局唯一**。三个浮窗上都放撤销按钮，用户会以为「按的是我这个盒的撤销」。

**设计决定：不动语义，但把文案改到不可能误解。**

| 场景 | 现状（`mainwindow.cpp:194`） | 新文案 |
|---|---|---|
| 不可撤销 | `撤销上次收纳` | `暂无可撤销` |
| 可撤销（主窗口） | `撤销上次收纳（3 项）` | `撤销上次收纳（3 项 · 临时）` |
| 可撤销（浮窗，本盒） | — | `撤销本盒上次收纳（3 项）` |
| 可撤销（浮窗，**别的盒**） | — | `撤销「工作」的收纳（3 项）` ← **点名** |

Tooltip 统一写全：「把上一次收纳进「临时」的 3 项还原回桌面（发生于 14:32:07）／注意：撤销的是**全程序最近一次**收纳。」

确认框（`mainwindow.cpp:391-396` 同步改）：「确定把上次收纳进「临时」的 3 项还原回桌面吗？（这是全程序最近一次收纳；若你刚在别的盒里收纳过，撤销的会是那一次。）」

---

## 10. 持久化方案

### 10.1 新增配置键（加在 `settings.cpp:17-19` 键名常量区）

```cpp
const QString kKeyFloatingBoxes  = QStringLiteral("floating/openBoxes");
const QString kKeyFloatGeometry  = QStringLiteral("floating/geometry/%1");
const QString kKeyFloatRolledUp  = QStringLiteral("floating/rolledUp/%1");
const QString kKeyAlwaysOnTop    = QStringLiteral("floating/alwaysOnTop");
const QString kKeyTrayHintShown  = QStringLiteral("ui/trayHintShown");
```

### 10.2 几何用 `saveGeometry()` blob，每盒一键

**为什么不用 x/y/w/h**：`saveGeometry()` 会一并存**屏幕标识与 DPI 上下文**，多显示器拔插后 Qt 能自己把窗口挪回可见区；手写 x/y **存不了这个信息，浮窗会跑到已拔掉的显示器上永远看不见**。代价是 blob 在 ini 里不可读——可接受。

**键名含盒名的问题**：`QSettings` 的键分隔符是 `/`，盒名虽已被 `sanitizeBoxName` 挡掉非法字符（`corenames.h:48-53`）仍需防御。**处理**：`toUtf8().toBase64(Base64UrlEncoding)` 后再拼。

### 10.3 读写时机

| 时机 | 动作 |
|---|---|
| 开浮窗 | 盒名加入 `openBoxes` |
| 点 ✕ | 移出 `openBoxes`；**保留 geometry 键**（下次还在原位） |
| 移动/缩放 | **去抖 500ms** 后写 geometry |
| 卷起/展开 | 立即写 `rolledUp/<盒名>` |
| 托盘退出 | `saveAllGeometry()` 兜底，防去抖中最后一次改动丢失 |

**去抖是必须做的，不是优化项**：拖动会连续触发 `moveEvent`，每像素写一次 INI 会明显卡顿。

### 10.4 盒目录被删后的残留

**核心原则与现有哲学一致**（`settings.h:12-15`：盒子=真实目录，文件系统即真相）：

- **启动时**：读 `openBoxes`，逐个对照 `BoxManager::listBoxes()`，**没有就丢弃该条目**，不为已消失的盒创建浮窗。
- **运行中**：`FloatingBoxManager` 检查盒目录是否还在，不在就销毁浮窗 + 移出配置。
- **孤儿 geometry 键**：**不主动清理**（需每次启动全量遍历，成本 > 收益；且重建同名盒能复用原位置，算个小惊喜）。**这是刻意取舍。**

---

## 11. 构建清单变更（两处必须同步）

**`DeskTidy.pro`** —— `SOURCES` 加 `core/appservice.cpp`、`core/opener.cpp`、`ui/floatingboxwidget.cpp`、`ui/floatingboxmanager.cpp`、`ui/trayicon.cpp`；`HEADERS` 加对应 5 个 `.h`。

**`CMakeLists.txt`** —— `DESKTIDY_CORE_SOURCES` 加 `core/appservice.cpp`、`core/opener.cpp`；`DESKTIDY_CORE_HEADERS` 加 `core/appservice.h`、`core/opener.h`；`DESKTIDY_UI_SOURCES` 加三个新 `ui/*.cpp`；`DESKTIDY_UI_HEADERS` 加对应三个 `.h`。

**无需新增 Qt 模块**：`QSystemTrayIcon`/`QMenu` 在 `Qt6::Widgets`；`QDesktopServices` 在 `Qt6::Gui`（均已 link）。

---

## 12. 改动清单与风险排序

| # | 改动 | 文件 | 风险 |
|---|---|---|---|
| 1 | 新增 `core/opener` | 新 | **低**（纯新增） |
| 2 | 新增 `summarize()` | 新 `core/appservice` | **低**（平移，可单测） |
| 3 | 浮窗配置访问器 | `core/settings.h/.cpp` | **低**（纯追加） |
| 4 | 构建清单同步 | `.pro` + `CMakeLists.txt` | **低**（漏改立刻暴露） |
| 5 | `AppService` 骨架 | 新 | **中** |
| 6 | MainWindow 改用注入的 service | `ui/mainwindow.h/.cpp`、`main.cpp` | **中**（要删析构里的 `delete`） |
| 7 | `ItemListWidget` 加 opt-in 参数 | `ui/itemlistwidget.h/.cpp` | **中**（默认值保现有行为） |
| 8 | `FloatingBoxWidget` | 新 | **中**（无边框细节多） |
| 9 | `FloatingBoxManager` | 新 | **中** |
| 10 | 托盘 + 主窗口关窗改隐藏 | 新 + `mainwindow.cpp` + `main.cpp` | **高**（最易出现「关不掉」或「意外退出」） |
| 11 | 拖出还原最终方案 | `itemlistwidget.cpp`、`floatingboxwidget.cpp` | **最高**（§6 不确定性全在这） |

**推进顺序**：

```
阶段 0：实验（不写产品代码，1 小时内）
  最小 Qt 程序：拖出一个真实文件到资源管理器文件夹，观察
    (a) 源文件还在不在？ (b) 目标是副本还是移动？ (c) exec() 返回值？
  => 直接决定 §6 最终方案

阶段 1：core 层（低风险，可单测）
  opener / appservice(含 summarize) / settings 新访问器 / 构建清单
  ✅ 编译通过 + summarize 单测全过

阶段 2：改造现有 UI 接入 AppService
  main.cpp 注入 / MainWindow 改用 service / ItemListWidget 加 opt-in 参数
  ✅ 跑现有 tests + e2e_probe，必须全过【回归闸门】

阶段 3：浮窗本体（先不做拖出）
  ✅ 手动验开关/移动/卷起/重启后位置恢复

阶段 4：托盘与常驻
  ✅ 关主窗口不退出、关所有浮窗不退出、托盘退出才退出（任务管理器确认）

阶段 5：拖出还原（依赖阶段 0 结论）
```

**回归闸门**：**阶段 2 结束必须跑 `verify.bat`**（21 单测 + 29 端到端）。需清醒：`e2e_probe` **只测 core**，所以**主窗口行为只能靠手动验**。

---

## 13. 测试策略

**能进 `tests/` 单测的（建议全做）**：

| 测什么 | 要点 |
|---|---|
| `summarize()` | 空列表、全成功、混合状态、`sourcePath` 为空时的展示名回落 |
| 浮窗几何序列化 | `QTemporaryDir` + 真 `QSettings`，验证存取一致、盒名含特殊字符的键名编码 |
| `openBoxes` 解析与清理 | 已消失盒名被过滤、顺序稳定、空值不留脏键 |
| `Opener::openPath` **失败分支** | 文件不存在时返回 `false` 且 `error` 非空。**不断言成功分支**（会真启动程序） |

**只能人工验收**：浮窗不占任务栏 / 真置顶 / Alt+Tab 是否出现 / 无边框拖动 / `QSizeGrip` 可用 / 托盘退出真结束进程 / 关主窗口后进程仍在 / 多屏拔插后位置 / 双击 exe 能启动 / **拖出的 A/B 行为**。

**`tools/e2e_probe.cpp` 不建议扩展**：它现在是 `QCoreApplication`（`e2e_probe.pro:1` 只 `QT += core`），加 GUI 会破坏它「秒级纯逻辑验证」的定位。

**改为新增 `tools/e2e_floating.cpp`**（`QT += core gui widgets`），验证可自动化的部分：建临时盒放文件 → 建 `FloatingBoxWidget` 并 `show()` → 断言 `isVisible()`、flag 含 `WindowStaysOnTopHint`、标题正确 → 模拟收纳 → 断言列表条目数变化（**这是信号链路是否接通的直接验证**）→ 模拟撤销 → 断言按钮文字与可用性。**价值很高**，能自动捕捉「信号没接上」这类最难发现的错误。

---

## 14. 待需求方拍板

### 问题 1（**最重要**）：拖出还原的语义要不要打折？

§6 表明能否实现取决于 shell 取 `Copy` 还是 `Move`，**必须实测**。若实测是 Copy，只能做到「复制到目标 + 提示源仍在盒里」，**不能**自动删源（违反铁律）。要么接受降级，要么放弃拖出（改右键菜单还原）。

**建议**：先做阶段 0 实验（1 小时）再定。**不要在不知道 A/B 的情况下动手写这部分。**

### 问题 2：浮窗「总在最前」要不要做成可关闭的？

`WindowStaysOnTopHint` 会盖住**全屏视频和游戏**。

- A：永远置顶不给开关（最简单，符合原话）
- **B（建议）**：默认置顶 + 右键菜单可关（多约 20 行，避免「看电影被挡」的抱怨）

### 问题 3：托盘图标用什么图？

当前 `main.cpp` **没有设置任何程序图标**。

- **A（建议第一期）**：`QStyle::standardIcon(QStyle::SP_DesktopIcon)`（零资源，但辨识度低）
- B：加 `.ico` + `.qrc`（两份构建清单再加 `RESOURCES`，且要设计图标）

---

## 15. 不确定的地方（诚实清单）

> **更新：第 1 条（拖放行为）已由两轮实测解决，见 §6.2。以下保留其余待实测项。**

1. ~~**拖放到资源管理器的实际行为**~~ —— **已解决**。实测确认默认是复制；注入 `Preferred Drop Effect = MOVE` 后可实现真正的移动。**同时发现一条硬约束**：拖到非资源管理器目标可能导致文件消失，必须实现"检查源文件是否仍在"的兜底（§6.2.1）。
2. **三个窗口 flag 在 Win11 上的组合行为** + `setQuitOnLastWindowClosed(false)` 后托盘是否足以维持进程。**必须实测。**
3. **模态 `PreviewDialog` 以 `Qt::Tool` 为 parent 的层级表现**（§5.1）。**需实测。**
4. **`QSizeGrip` 在 `FramelessWindowHint` 下的显示**，可能需手动 `resize()`。**需实测。**
5. **多屏 + 混合 DPI 下 `saveGeometry()` blob 的可靠性**（历史上出过问题）。**需实测。**
6. **`QSystemTrayIcon` 在 Win11 溢出区是否容易被找到**——不影响正确性但影响可用性，用户可能找不到「退出」。**可能需首次 `showMessage()` 引导。**

---

## 附：设计决定速查表

| 决定 | 选择 | 理由 |
|---|---|---|
| 状态所有权 | 新增 `AppService`（core） | 浮窗需与主窗口平级的入口；「收进哪个盒」必须由调用方指定 |
| `UndoStack` 改造 | **不改**，保持纯值语义 | 通知是 AppService 的职责 |
| `Settings` 归属 | 收进 AppService | 两个消费者，统一入口 |
| AppService 生命周期 | `main.cpp` 栈上，**声明在 MainWindow 之前** | 逆序析构保证活得更久 |
| 退出条件 | `setQuitOnLastWindowClosed(false)`，只有托盘退出 | 不依赖窗口类型与平台细节 |
| 打开文件 | 新增 `core/opener`，用 `QDesktopServices` | 不放 Collector |
| 浮窗内列表 | **复用** `ItemListWidget` + opt-in 参数 | 避免两套图标逻辑 |
| 主窗口双击 | **不变**，仍是还原 | 语义差异刻意保留，不动现有行为 |
| 浮窗双击 | 打开 | 浮窗是「使用」界面 |
| 浮窗内收纳 | 收进**自己代表的盒** | 绕开「当前选中盒」依赖 |
| 撤销语义 | **保持全局唯一**，改文案消歧义 | 每盒一栈是另一个功能 |
| 几何持久化 | `saveGeometry()` blob，每盒一键 | 能处理多屏拔插 |
| 几何写入 | **去抖 500ms** | 否则拖动卡顿 |
| 孤儿配置 | 不主动清理 | 成本 > 收益 |
| 拖出还原 | **已实测通过**：注入 `Preferred Drop Effect = MOVE` 后资源管理器执行真正的移动 | 两轮实测，见 §6.2 |
| e2e 探针 | 不改现有，**新增** `e2e_floating` | 保持 core 探针的纯逻辑定位 |

---

## 实施记录（方案落地后的实际情况）

> 本节是方案执行完之后补写的，用于对照"计划"与"实际"。
> 上面的正文保持原样（那是决策时的思考），这里是结果。

### 各阶段完成情况

| 阶段 | 状态 | 验证方式 |
|---|---|---|
| 0 实验 | ✅ 完成 | 两轮真机拖放实验，结论见 §6.2 |
| 1 core 层 | ✅ 完成 | 编译 0 警告 + 17 项新单测 |
| 2 UI 接入 | ✅ 完成 | **回归闸门**：21 + 17 单测、29 端到端全过 |
| 3 浮窗本体 | ✅ 完成 | 编译 0 警告 + 实机启动 + 窗口样式逐项校对 + 信号链路 18/18 |
| 4 托盘常驻 | ✅ 完成 | 实机验证三件事（见下） |
| 5 拖出还原 | ✅ 完成 | 依赖阶段 0 的实测结论实现，含源文件存在性兜底 |

### 实机验证结果（阶段 4 的"必须实测"项）

用 Windows API 直接检查，不是靠推断：

| 验证项 | 实测结果 |
|---|---|
| 无系统标题栏 | ✅ `WS_CAPTION = False` |
| 不占任务栏 | ✅ `WS_EX_TOOLWINDOW = True`、`WS_EX_APPWINDOW = False` |
| 置顶生效 | ✅ `WS_EX_TOPMOST = True` |
| 托盘图标真的注册 | ✅ 找到 `Qt6111TrayIconMessageWindowClass` / `QTrayIconMessageWindow` |
| **关主窗口后进程仍在** | ✅ 在，且浮窗继续可见 |
| **关掉所有浮窗后进程仍不退** | ✅ 仍不退 —— 这条是 `setQuitOnLastWindowClosed(false)` 存在的理由 |
| 浮窗随程序重启恢复 | ✅ 自动重建（读 `openBoxes` 配置） |
| 几何持久化 | ✅ 配置里存成 `geometry\<base64盒名> = @ByteArray(...)` |

### 实施中修正的、方案里没预料到的问题

1. **`collector.h` 里 `restore()` 的注释写反了字段语义**。原文说"sourcePath/finalPath 语义与收纳时相反"，
   读起来像是**入参**也对调；实际**入参沿用 `collect()` 的方向**，只有**返回值**才对调。
   照那句注释写会把文件搬向错误方向。已改准并展开说明。
2. **配置文件路径的注释写错**。文档与代码注释都写 `%APPDATA%\DeskTidy\`，
   实际 Qt6 在 Windows 上把 `AppConfigLocation` 映射到 **`%LOCALAPPDATA%\DeskTidy\DeskTidy\`**。已修正。
3. **双重复刷新**：阶段 2 发现 `refreshBoxes` 之后会经 `BoxListWidget::setBoxes` 主动补发
   `boxSelectionChanged`，若信号处理里再显式调 `refreshItems()` 就会重扫两遍目录。已去掉显式调用。
4. **缺 include / 缺 `signals:` 段**：`trayicon.h` 用嵌套枚举却只前置声明、`quitRequested` 声明遗漏。
   编译器直接拦住，已补。

### 与方案不同之处

- **托盘**：方案里说 MainWindow 与 TrayIcon 都不直接 new 浮窗 —— 实际实现一致，都通过
  `FloatingBoxManager`。托盘用 **setter 注入**而非构造参数，因为托盘构造需要主窗口指针、
  主窗口关闭判断需要托盘指针，放同一构造函数会成为循环依赖。
- **浮窗外观**：方案提到可以用 `WA_TranslucentBackground` 做圆角。**第一次实现用了它，
  后来整个删掉**。详见下面"圆弧与透明背景"一节 —— 那是一条走不通的路。
- **悬停自动展开**：方案 §4.3 提到 Fences 的悬停展开，实施记录初版写的是"实际没做"。
  **后续步骤正在补**（分步交付中），当前状态见下面"分步扩展"一节。

---

## 分步扩展（v1 之后的增量需求）

> 这一节按用户提出的顺序记录每一次扩展。每一步都要求"做完即验收"，
> 所以每一步末尾都附**诚实清单**：哪些是真验证过的，哪些只是"看着对"。

### 步骤 1：删除收纳盒 + 自定义浮窗外观

需求原文：「这一版没法删除收纳盒，添加这个功能，同时添加自定义桌面浮窗外观的选项，
可以调整透明度，排列方式和显示方式（如大图标，小图标等）」

- `BoxAppearance`（`core/coretypes.h`）：视图模式四档（列表 / 小图标 / 中图标 / 大图标）、
  图标尺寸、透明度；`kMinOpacity = 20`。
- 持久化：每盒一组键 `floating/viewMode|iconSize|opacity/<base64盒名>`，
  **只存非默认值** —— 默认外观不会在配置里留下任何痕迹。
- `AppService::deleteBox()`：先把盒内条目还原到目标目录（**绝不覆盖**，重名自动改名），
  再把空壳目录送进回收站。返回 `BoxDeletionResult{restored, trashedItems, error}`。
- 验证：`delete_probe` 16 项、`viewreset_probe` 15 项、`appearance` 单测 9 项。

**实测澄清的一处误判**：先前的调研结论说 `QFile::moveToTrash` 不支持目录。
实测**推翻**了这个说法 —— 对"3 个文件 + 1 个子目录"的目录调用，返回 `true`、
源目录消失、回收站计数 +1。所以不需要 `windows.h` / `SHFileOperationW`。

### 步骤 2：圆角 + 卷起/展开动画 + 记忆展开尺寸

- 圆角用 `setMask()`（`updateRoundedMask()`，从 `resizeEvent` 触发），
  半径 `kCornerRadius = 8`。**没用** `WA_TranslucentBackground`。
- 卷起/展开：`animateHeightTo()` / `setHeightImmediately()`，
  时长 `kRollDurationMs = 220`；卷起后高度 = 标题栏高（30）。
- 记忆展开尺寸：`m_expandedHeight`。

### 步骤 2.5：修掉"浮窗顶部全透明且点不到"

这是一次**由圆角引入的回归**，值得单独记一笔。

- **现象**：加了圆角之后浮窗顶部变全透明，而且那一块点不到。
- **根因**：为了让圆角外的区域透明，给窗口设了 `Qt::WA_TranslucentBackground`。
  这个属性会让**父窗口不再绘制自己的背景**，于是每个子控件都必须自己画背景。
  Windows 上这套组合不可靠。
- **走过的弯路**：试过给 4 个子控件设 palette + `autoFillBackground`、
  并把 stylesheet 里的 `background` 摘掉 —— 结果**更糟**，
  用户反馈「现在只有内容那一块是白色的，其他都变透明了」。
- **最终解法**：**把 `WA_TranslucentBackground` 整个删掉**，圆角只靠 `setMask()`，
  背景一律交给 stylesheet。用户确认「好了」。
- **教训**：`setMask()` 已经能把四角裁掉，`WA_TranslucentBackground` 是多余的；
  为了一个多余的属性去重构全部背景绘制，是纯粹的负收益。

### 步骤 3：浮窗互相让位（`core/windowlayout`）

需求原文包含"如果展开浮窗时下面挡到其他浮窗了，自动调整下面浮窗的位置，平滑移动"。
这一步只做**几何计算**，尚未接到真实窗口上。

- 新模块 `core/windowlayout.h/.cpp`：纯几何，**不引用任何 QWidget**，
  只吃 `QRect`、吐"谁该往下挪多少"。抽成纯函数的理由有三条，
  写在头文件顶部注释里（其中一条是"用单元测试覆盖各种布局比手工摆窗口便宜得多"）。
- 四条约束：只推下方、只推最小距离、不跨屏、不推出屏幕。
- 两条在设计过程中**被明确拍板**的语义：
  - **连锁下推**：被推者让开后若压住了排在它下面的窗口，那个窗口继续让。
  - **严格不重叠**：被推者顶边落在障碍物底边**之下 1 像素**
    （`QRect::bottom()` 含尾，所以判据写作 `b.bottom() + 1`）。

#### 这一步踩过的三个坑（都留了注释和回归用例）

1. **判据写反成了死结**。原写法先要求"不重叠"再算 `dy = anchor.bottom() - top`，
   于是 `dy` 必然 ≤ 0，下一秒又被 `if (dy <= 0) continue` 丢掉。
   两个条件互为反面 —— 能过前者的输入一定过不了后者，
   表现是**函数恒返回空且不报错**，功能完全没生效。
2. **拿窗口矩形去比屏幕矩形**。判断"是不是同一块屏"时用了
   `sameScreen(item.screenRect, anchorTarget)`，而 `anchorTarget` 是窗口矩形，
   与屏幕矩形**永远不可能相等** → 所有窗口都被判成异屏跳过，
   表现同样是"什么都不做"。修法是给函数加 `anchorScreen` 参数，两边都拿屏幕矩形比。
3. **连锁的位移被算两遍**。写成"先搬到 anchor 之下，再从那个位置往下推"，
   结果搬过去的位置本身已经压住了已落定者，`pushBelow` 又把这段重叠加了一遍 ——
   落点比应有的位置多出**整整一个窗口高度**。修法是把 `anchorTarget`
   与已落定者放进**同一个障碍列表**，从原始位置一次算到最终落点。

还有一个**只在测试里**的坑：`QRect::bottom() == top() + height() - 1`。
用例里写 `1000 + 60 + dy` 当作底边，比真实底边大 1，导致边界断言差一格假失败。

#### 验证

- `tests/tst_floatinglogic.cpp` 新增 C 组 15 条用例（含上述三个坑的回归守卫），
  该目标**总计 64 项全过**。
- 新增独立探针 `tools/windowlayout_diag`（加入 `verify.bat` 第 10 阶段，11 项全过）。
  它和单测**不重复**：单测钉每条约束的边界（"恰好推多少"），
  探针钉**整局布局的不变量** —— 让位后两两不重叠、没人出屏、没人被向上推、
  发起者不推自己。前者防写错，后者防"三条约束各自都对、合起来互相打架"。
  探针设计了 5 个场景，第 5 个是"一列四窗从下往上逐个展开"，
  专门查连续操作后布局会不会一路往下漂。

**探针第一次跑就抓到一个失败，但那是探针自己写错了**：
循环里先把位移应用到 `items` 上，再把它传给 `checkInvariants`，
而后者内部**又应用了一次** —— 同一批位移算两遍，窗口被推出屏幕，
报出来却像"实现越界"。调整成"先校验、后落地"就对了。
（留这条记录是因为：探针的假失败和产品的真失败长得一模一样，
不写下来下次还会浪费同样的时间去查实现。）

#### 步骤 3 的诚实清单

1. **`computePushDown` 还没有接进真实浮窗**。目前它是"算得对但没人调用"的状态，
   第 5 步才会接。所以"展开时自动推开下面的浮窗"**当前在界面上不可见**。
2. **平滑移动未验证**。探测器和单测只管几何目标值，
   至于窗口是"瞬移过去"还是"滑过去"，要看接入时怎么用动画。
3. **`moveEvent` 与位置持久化的交互未验证**。被推的窗口如果照常触发
   `scheduleGeometrySave()`，那就把"临时让位"当成了用户意图存进配置 ——
   下次启动会带着被推开的位置醒来。接入时必须用 `m_layoutAdjusting` 类守卫跳过。
4. **振荡风险未实测**。理论上"单次触发 + 以展开前位置为基线"不会来回推，
   但真实多窗口下反复 hover 展开/收起是否稳定，**没有实测过**。
5. **多显示器仍然只有单屏可测**。跨屏隔离是靠单元测试断言保证的，
   不是靠真机双屏跑出来的。


### 步骤 4：悬停自动展开 / 离开自动卷起

需求原文：「最好是不需要手动展开卷起，可以做当鼠标放置在浮窗栏位上的时候自动展开，
不放在上面时自动卷起，然后记住展开时的浮窗尺寸」。

#### 规格（已拍板）

| 项 | 决定 |
|---|---|
| 触发区域 | **整个浮窗**（不是只标题栏） |
| 进入延迟 | 250 ms —— 鼠标扫过桌面时不至于一片窗口噼里啪啦乱开 |
| 离开延迟 | 400 ms —— 比进入长，从列表移向标题栏时不至于闪卷 |
| 手动卷起 | 悬停**仍能**自动展开（**已修订**，见下） |
| 状态持久化 | 自动卷起的状态**要落盘**（沿用 `floatRolledUp`，每盒一份） |
| 开关 | 浮窗右键菜单一项 + 控制中心全局开关，两个入口都通 |
| 关动画时 | 高度**瞬变**到位，悬停展开本身仍然生效 |

#### 必须压制的六种冲突

自动展开会在这些时刻被误触发，每一处都得显式挡掉：

1. **拖动窗口**。`FloatingBoxTitleBar::m_dragging` 为真时鼠标"在窗口上"是假象 ——
   实际上人是想把它搬走。此时展开会让窗口在手指底下变形。
2. **`QSizeGrip` 拖拽**。拉右下角时鼠标可能短暂移出窗口，误判成 leave → 卷起 →
   窗口在拉伸过程中缩成一条线。
3. **右键菜单打开期间**。`QMenu::exec()` 是模态事件循环，菜单弹出后鼠标已经不在
   浮窗上，会立刻触发 leave。不挡的话每次右键都会把浮窗卷起来。
4. **模态对话框**（`PreviewDialog`）。同理。
5. **文件拖出**（`QDrag`）。`QDrag::exec()` 是**系统级鼠标抓取**，期间浮窗收不到
   正常的 enter/leave，状态会错乱。
6. **卷起动画进行中**。动画期间再触发一次会让两段动画互相打断，
   `m_expandedHeight` 可能被写脏（这个坑在步骤 2 已经踩过一次）。

#### 与既有状态机的接缝

- 新增 `m_hoverExpanded`：**悬浮展开**造成的展开态，与"主人手动展开"区分开。
  离开时只有它会被自动收回；主人手动展开的窗口，鼠标离开**不**自动卷起
  （否则主人特意展开来看，鼠标一移开就没了，很恼人）。
- ~~`m_manualRolledUp`：主人手动卷起的标记。它为真时悬停不展开。~~
  **已删除** —— 见下面「规格修订：取消『手动卷起优先』」一节。
- 两个定时器：`m_hoverExpandTimer`（250 ms）、`m_hoverCollapseTimer`（400 ms）。
  **进入时必须停掉离开定时器**，否则刚进来又被上一次的离开事件卷回去。
- 两个标志都必须进 `resizeEvent` 的高度记录守卫 —— 理由是现成的：
  `m_rollAnimating` 当年就是为了同一件事加的。

#### 验证方式

- 单测（`tst_floatinglogic`）：定时器延迟常量、开关配置往返。
  状态机迁移不在这里测 —— 它需要真事件循环，交给 `hover_diag`。
- 新探针 `hover_diag`：真的发 `QEnterEvent` / `QEvent::Leave` /
  `QDragEnterEvent`，推进事件循环等过延迟，断言高度确实变化。

**依赖步骤 3**：展开时要调用 `WindowLayout::computePushDown` 推开下方浮窗，
并且**被推的位移不能落盘**（`m_layoutAdjusting` 守卫）。

#### 步骤 4 尚未做的事

- 平滑移动（`QPropertyAnimation` 逐帧挪位）留给步骤 5，本步只做几何目标值。
- 多显示器未测（仍然只有单屏）。

---

#### 步骤 4 实施记录

##### 实际怎么做的

改动分布在 7 个文件，核心全部落在 `ui/floatingboxwidget.cpp`：

| 文件 | 改了什么 |
|---|---|
| `core/settings.h/.cpp` | 新增全局单键 `ui/hoverExpand`，`hoverExpandEnabled()` / `setHoverExpandEnabled()`，默认开启 |
| `ui/floatingboxwidget.h` | 两个延迟常量、`setHoverExpandEnabled`、`enterEvent`/`leaveEvent`/`eventFilter`、`hoverExpandChangeRequested` 信号、6 个新成员 |
| `ui/floatingboxwidget.cpp` | 悬停状态机、六种冲突的屏蔽、右键菜单项、各种 `exec()` 的守卫 |
| `ui/floatingboxmanager.h/.cpp` | `setHoverExpandEnabled` 写配置 + 遍历广播；新浮窗在 `openBox` 里同步一次 |
| `ui/mainwindow.cpp` | 设置对话框加「鼠标悬停时自动展开浮窗」勾选框 |
| `ui/itemlistwidget.h/.cpp` | 新增 `dragOutStarted()` 信号，把 `QDrag::exec()` 的区间框出来 |
| `tests/tst_floatinglogic.cpp` | 新增 E 组 3 条用例（延迟常量、开关往返、与动画开关互不干扰） |

关键实现选择：

- **延迟常量放在头文件**做 `static constexpr`，单测直接引用。两处各写一遍数字
  必然漂移 —— 改了实现忘了改测试，测试仍然全绿，比没有测试更糟。
- **`hoverInteractionBlocked()` 把六种情形收在一处**：`enterEvent`、`leaveEvent`、
  两个定时器回调共四处都要问同一组问题，分散写必然漂移，而症状是
  "偶尔会莫名其妙展开一下"，极难复现。
- **`ModalGuard` 用 RAII**：这些 `exec()` 调用点之间都有 `return`（用户取消、
  条目为空、提前失败），手写"进入置真、退出置假"必然漏掉其中一条。
  漏复位的后果不是"闪一下"，是**这个浮窗从此再也不响应悬停**，且没有任何报错。
- **新浮窗的开关初值给 `false`**：`setHoverExpandEnabled` 在值没变时提前返回，
  若初值是 `true`、而配置是 `false`，那次同步会被自己挡掉 —— 表现是
  "设置里明明是关的，新开的浮窗怎么还乱弹"。

##### 踩到的坑

1. **`QSizeGrip` 根本没有 `pressed` / `released` 信号。**
   最初写的是 `connect(m_sizeGrip, &QSizeGrip::pressed, ...)`，编译期就会报
   `is not a member of 'QSizeGrip'`。翻 Qt 6.11 的头文件确认：这个类连
   `Q_SIGNALS` 段都没有，鼠标事件全被它自己吞掉。
   正确做法是装**事件过滤器**（`installEventFilter` + 重写 `eventFilter`），
   从 `MouseButtonPress` / `MouseButtonRelease` 上取状态。
   没为它继承一个子类，是因为这个状态只在本文件用一次，多开一个类不划算。

2. **`QCursor` 需要显式 include。**
   在 `shouldAutoCollapse()` 里用 `QCursor::pos()` 判断"鼠标是不是又回来了"，
   而它可能经由 `<QMouseEvent>` 传递进来。依赖传递包含是脆的 ——
   哪个 Qt 版本调整了头文件结构就会突然编译失败，所以补了显式 include。

3. **一个需求上没说清、由实现补齐的决策**：`m_manualRolledUp` **不跨进程
   持久化**。配置里那个 `floatRolledUp` 键分辨不出"主人手动收的"与
   "上次悬停离开时自动收的"。选择的理由是"每次启动都从自动模式开始"：
   主人重启后鼠标移到浮窗上它照常展开，而不是"我什么都没做它却不肯展开"。
   代价是手动收起的窗口重启后悬停又会展开 —— 可接受，因为鼠标离开后
   它自己就收回去了。这个取舍写在构造函数的注释里。

##### 验证到什么程度（实施者交付时的自述）

**编译：0 warning / 0 error。** 但要说清楚是哪一次：

- 第一次完整构建（`qmake` + `mingw32-make -j8`）产出的 `build-qmake/build.log`
  显示 22 个 `.cpp` 全部编译通过、链接出 `DeskTidy.exe`，日志里 `error`/`warning`
  零命中。那一次**已经包含** `floatingboxwidget`、`floatingboxmanager`、
  `itemlistwidget`、`settings` 的悬停改动，以及事件过滤器那处修正。
- ⚠️ 但**在那之后**又改了两处：`mainwindow.cpp`（设置对话框加勾选框）与
  `floatingboxwidget.cpp` 构造函数里那段注释、以及补 `#include <QCursor>`。
  **这三处没有重新构建过** —— 实施这一步的会话里没有可用的 shell 工具，
  无法再跑一次 `mingw32-make`。所以严格地说：

  | 部分 | 构建状态 |
  |---|---|
  | 核心悬停状态机、事件过滤器、manager、settings、itemlistwidget | ✅ 实际构建通过 |
  | `mainwindow.cpp` 的勾选框、`#include <QCursor>` | ⚠️ **只做了人工审阅，未重新编译** |

  这三处改动都是简单的（一个 `new QCheckBox` + 两行 `setChecked`/`addWidget` +
  一行 `m_floating->setHoverExpandEnabled(...)`；一个标准头文件 include），
  手工核对下来语法与符号都存在（`QCheckBox` 已在 17 行 include、
  `hoverExpandEnabled` 已在 Settings 里定义、`<QCursor>` 是 QtGui 的公开头），
  但**"应该能编译"不等于"编译过了"**，需要补跑一次构建确认。

**逻辑推理（不是实测）**：状态机的迁移规则是从代码逐条推出来的，
包括"手动卷起 → 悬停不展开 → 再手动展开 → 恢复自动"这条链。
E 组的 3 条单测覆盖了延迟常量与配置往返，但**没有**覆盖迁移本身 ——
理由是那些迁移依赖真实的 `enterEvent`/定时器到点，见下面的诚实清单。

##### 验证到什么程度（上级复核后更新）

⚠️ **上面那份清单是实施者的自述，写在他交付的时刻。其中若干条在后续
复核里已经被推翻或补上 —— 以本节为准。**

复核做了三件事：编译、跑 `verify.bat`、写 `hover_diag` 探针真正驱动事件。

**`verify.bat` 已扩到 12 个阶段，RESULT: ALL PASSED**（退出码 0）：

| 阶段 | 结果 |
|---|---|
| 2 浮窗单测 | **67 项**全过（原 64，+E 组 3 条） |
| 11 悬停（新增） | **14 项**全过 |

**`hover_diag` 实测到的行为** —— 这是本步第一次真正观察到功能生效：

| 场景 | 实测 |
|---|---|
| 卷起态 + 悬停 700ms | 高度 30 → **300**（展开了） |
| 展开态 + 离开 850ms | 高度 300 → **30**（卷起了） |
| 再悬停一次 | 高度 30 → **300**（可重复，不是"只能展开一次"） |
| 手动卷起后悬停 | 保持 30（手动优先） |
| 手动展开后离开 | 保持 300（不替主人收） |
| 总开关关闭时悬停 | 不变；打开后立即恢复 |

**上表中已被复核推翻的条目**：

- 第 7 条（`mainwindow.cpp` 没编译）→ **已编译**，0 warning / 0 error。
- 第 8 条（没跑 verify.bat、E 组未执行）→ **已跑**，67 项全过。
- 第 1 条（悬停展开完全没验证）→ **已由 `hover_diag` 覆盖核心路径**（见上表）。
  但"真实鼠标移动会不会产生 enter/leave"**仍然没验证** —— 探针是手工
  投递事件、绕过窗口系统的。这一条如实保留。
- 第 3 条（E 组没覆盖状态迁移）→ 状态迁移改由 `hover_diag` 覆盖，
  因为它才需要真事件循环。

##### 复核发现：探针自己的两个假失败

两次都花了时间去查"是不是产品错了"，结论都是探针写错。
记下来是因为**探针的假失败与产品的真失败长得一模一样**：

1. **夹具串状态。** `hover_diag` 的四个夹具共用同一配置目录，前一个夹具
   存下的 `floatRolledUp` / 几何 blob 会被下一个继承。[2] 段于是以
   **120 高**（而不是默认 300）起步，`PASS` 的那条断言是自比式的、
   恒真、什么都没验证。修法：每个夹具显式清 `floatRolledUp` 与
   `floatGeometry`，并加一条**绝对量级**前提断言（`height() >= 200`）。
   → 教训：自比式断言没有前提约束时等于没写。
2. **同名额窗口抓错。** 遍历 `topLevelWidgets()` 找"名字对且可见"的窗口，
   会抓到上一个夹具尚未析构完的同名窗口。修法：打开前快照窗口集合，
   **只认新增的那个**。

##### 仍未验证 / 不确定的地方（复核后诚实清单）

1. **真实鼠标从未参与验证。** `hover_diag` 用 `QApplication::sendEvent`
   手工投递事件，**绕过了窗口系统**。它证明的是"收到事件之后逻辑对不对"，
   **不是** "真实鼠标移动会产生这些事件"。必须人工把鼠标移上去确认。
2. **延迟手感（250 / 400 ms）没验证。** 探针只能证明"等这个时长后状态确实
   变了"，证明不了"这个时长用起来舒服"。
3. **右键菜单 / 模态框 / 文件拖出 / QSizeGrip 拖拽这四种屏蔽没有实测。**
   代码里都在（`hoverInteractionBlocked()` 逐条写明理由），逻辑是直线代码，
   但探针无法在 `exec()` 内部投递事件。**这四条需要人工各试一次。**
4. **`m_manualRolledUp` 不跨进程持久化。** 配置里 `floatRolledUp` 分辨不出
   "手动收的"与"悬停离开时自动收的"，实现选了"每次启动都从自动模式开始"。
   代价：主人手动卷起的窗口，重启后鼠标移上去又会展开。
   （实施者自行决定，此处明示。）
5. **`resizeEvent` 的记录守卫没有为悬停增加条件** —— 这是刻意判断：
   悬停展开走 `applyRollUpState(false)`，会被 `m_rollAnimating` 与
   `m_layoutAdjusting` 覆盖。推理成立，但**没有实测** "悬停展开后
   `m_expandedHeight` 有没有被写脏"。不过 `hover_diag` [3] 段
   "展开→收起→再展开" 三轮都回到 300，间接支持这个推理。
6. **多显示器未测**，同前几步。
7. **高度动画期间 mask 是否逐帧正确**没专门验（步骤 2 只验了
   "尺寸变化后 mask 会跟上"）。

##### 结论

**功能本身已由 `hover_diag` 观察到生效**（悬停展开、离开卷起、可重复、
手动优先、开关生效五条核心行为），12 阶段验收全过。

仍然需要人工确认的只有两类：**真实鼠标事件是否触发**、
**四种屏蔽（菜单/模态/拖出/拉尺寸）在真机上是否牢靠**。

#### 步骤 4 修：主人实测报回来的两个 bug

上面那套验收全绿之后，主人实际用了一下，报回两个问题。
**两个都是探针没覆盖到的路径** —— 这本身就说明"22 项全过"不等于"能用"。

##### bug A：重新打开「悬停自动展开」开关后，只有刚打开那一下生效

- **现象**：关掉再打开开关，鼠标停在浮窗上没反应；把鼠标移开再移回来才恢复。
- **根因**：展开计时器是在 `enterEvent` 里启动的，而**打开开关这个动作
  本身不产生 enterEvent**。主人的真实操作顺序是：
  鼠标挪到浮窗上（此时开关是关的，`enterEvent` 进来了但 `canAutoExpand()`
  因为开关关着直接返回，计时器没起）→ 发现没反应 → 打开开关 →
  **鼠标一直没动过**。此时窗口系统认为鼠标本来就在窗口上，不会再发 enter，
  于是没有任何时机去启动那个计时器。
- **修法**：`setHoverExpandEnabled(true)` 时主动补一次判断 ——
  若鼠标此刻已经在浮窗矩形内（用 `rect().contains(mapFromGlobal(QCursor::pos()))`
  判断，与 `shouldAutoCollapse` 同源，不依赖 Qt 的 enter/leave 记账），
  当场开始计时。
- **为什么原来的探针没抓到**：`hover_diag` 的 [4] 段在重新打开开关之后
  **又补发了一次 `sendEnter`**，于是"即使实现有 bug 也会展开"，用例恒过、
  把问题盖住了。新增的 [5] 段刻意**不发 enter、也不动鼠标**，
  只调开关，才复现出来。
  → 教训：夹具的"顺手补一下前置条件"会把 bug 藏起来。

##### bug B：从桌面拖文件靠近浮窗时，浮窗不展开

- **现象**：拖桌面图标想存进卷起的浮窗，拖到跟前它死活不展开，
  于是没地方可放。
- **根因（两层）**：
  1. 拖放走的**不是** `enterEvent` 那条路 —— 拖拽期间 Windows 只发
     `dragEnter`/`dragMove`/`dragLeave`，**不发 enter/leave**。
     于是悬停展开那套逻辑（挂在 `enterEvent` 上）在这条路径上完全哑火。
  2. 而且**卷起时列表、操作条、手柄行全被隐藏了**，整个浮窗只剩标题栏可见，
     所以拖放流也只有标题栏收得到 —— 而标题栏原先没开 `acceptDrops`，
     Qt 根本不会把 `dragEnterEvent` 派发给它。
- **修法**：标题栏 `setAcceptDrops(true)`，新增 `dragEnterEvent` /
  `dragLeaveEvent` / `dropEvent`，只用于**报信**（发 `dragHovered` /
  `dragLeft` 信号），**不 accept、不处理 drop** —— 落点判定仍归列表，
  不能把已经过实测验证的投放逻辑顶掉。
  浮窗收到 `dragHovered` 时**立即展开**（刻意不走 250ms 延迟：人已经带着
  文件到跟前了，再让他等是多余的）；收到 `dragLeft` 时按正常规则计时收回。
- **修的过程中踩到的坑（自阻塞）**：第一版把 `m_dragHoverActive = true`
  写在 lambda 最前面，而 `hoverInteractionBlocked()` 又把它算作"交互中"之一，
  于是紧接着的 `canAutoExpand()` 立刻返回 false —— **这个标志把自己挡住了**。
  表现是"事件收到了、信号也发了、窗口就是不展开"，且完全不报错。
  修法：先判断、后置标志。
  → 诊断办法：在 `dragEnterEvent` 里临时插一句 `qDebug` 打印，
  一眼看出事件确实送达、问题在后面的守卫链上。比反复读代码猜快得多。

##### 修复后的验证

`hover_diag` 从 14 项扩到 **22 项**，全过；`verify.bat` 12 阶段全过、无回归。
新增的三段：

| 段 | 验证 |
|---|---|
| [5] | 只调开关、不发 enter、鼠标不动 → 高度 30 → **300** |
| [6] | 拖出条目期间（`dragOutStarted` 已发）离开 → 保持 300 不收起 |
| [7] | 给标题栏投 `QDragEnterEvent` → 高度 30 → **300**；再投 `QDragLeaveEvent` → 300 → **30** |

**仍然没验证的**：这两条修复都靠手工投递事件验证，**真实鼠标拖拽**是否
产生同样的 `dragEnter`/`dragLeave` 序列没有实测。特别是"拖到标题栏上方时
Windows 会不会把 drag 事件发给一个 `Qt::Tool` 无边框窗口"这件事，
依赖窗口的拖放注册，**必须人工拖一次才知道**。

#### 步骤 4 修 2：取消「手动卷起优先」（规格修订）

主人继续实测，报了第三个问题：**手动卷起一次之后，自动展开和自动卷起
再也无法触发**。这个问题的根因是原规格本身有缺陷，不是实现写错。

**原规格**：手动卷起 = "我现在不想看见内容"，于是把悬停自动展开一并屏蔽
（`m_manualRolledUp`），直到主人**再手动展开一次**才解除。

**为什么它必然坏 —— 那是一个解不开的循环**：

```
手动卷起 -> m_manualRolledUp = true -> 悬停不展开
         -> 窗口保持卷起 -> 主人想恢复自动，只能再手动展开一次
         -> 而他还得先想到要去双击标题栏
```

屏蔽的**解除条件**（手动展开）恰好被屏蔽本身挡住了入口。
主人的感受就是"卷起一次之后就废了"。而且它是**不可恢复**的：
一旦进入这个状态，无论怎么悬停都没用。

**新规格**（主人拍板）：

| 操作 | 悬停展开 | 离开自动卷起 |
|---|---|---|
| 手动卷起后 | ✅ 可以 | —（本来就是卷起的） |
| 手动展开后 | —（已展开） | ❌ 不卷 |
| 悬停展开后 | — | ✅ 卷 |

**实现**：`m_manualRolledUp` **整个删除**。你不需要它 ——
上表其实只有一句话：**"离开自动卷起"只对悬停展开的窗口生效**，
而那正是 `m_hoverExpanded` 已经表达的语义：
手动展开时它被清成 false，手动卷起时它本来就是 false。
多一层标志不但没有增加表达力，还制造了上面那个死循环。

**验证**：`hover_diag` 从 22 项扩到 **25 项**，全过。新增两段：

| 段 | 验证 |
|---|---|
| [1] | 手动卷起 → 悬停展开（30 → **300**）→ 离开自动卷起（300 → **30**） |
| [1b] | 上面这串**重复三轮**，每轮都必须成功 —— 专门钉"不会卡死" |

`[1b]` 是重点：`[1]` 只跑一轮不足以说明状态机没有单向陷进去，
三轮交替才是"可重复"的证明。结果三轮都是 `30 → 300 → 30`。

##### 顺带查出并加固的两个隐患

修这个问题时顺带发现两处，都不是主人直接报的，但都会造成同样的
"悬停忽然不工作了、且看不出原因"：

1. **标题栏的 `m_dragging` 可能永久卡在真。**
   它由 Press 置真、Release 置假。若中途丢了 Release（拖到窗口外松手、
   系统弹窗抢走事件、远程桌面断线），这个标志再也不会被清 ——
   而 `hoverInteractionBlocked()` 一看到它就返回真，于是这个浮窗
   **从此不可能自动展开或收起**。
   加固：判据改成 `isDragging() && (QGuiApplication::mouseButtons() & Qt::LeftButton)`，
   多问一句物理按键状态。丢掉事件时能自愈。

2. **`hover_diag` 的夹具漏发 `MouseButtonRelease`。**
   双击夹具只发了 Press + DblClick，于是 `m_dragging` 一直是真，
   后面所有悬停用例全部失败 —— 而且**表现得和主人报的那个 bug 一模一样**。
   查了半天才发现是夹具的问题。这条记在这里是因为：
   夹具的假失败和产品的真失败长得一样，而这次差点把产品代码改坏。

##### 诚实清单

- 新规格靠 `hover_diag` 手工投递事件验证，**真实鼠标**下的表现仍需人工确认。
- 特别是"手动卷起 → 鼠标移上去 → 应当展开"这条，是本轮的核心改动，
  **请务必手工试一次**。

#### 步骤 4 之后仍未做的事

- 平滑移动 → 见步骤 5。
- `computePushDown` 接进真实浮窗 → 见步骤 5。

### 步骤 5：接入推开逻辑 + 平滑移动 + 锁定按钮

把步骤 3 做好的 `WindowLayout::computePushDown`（纯几何、已 64 项单测覆盖）
真正接到浮窗上，并且**推动要有动画**，让主人看出是"被让开"而不是"瞬移"。

#### 规格（已拍板）

| 项 | 决定 |
|---|---|
| 计算时机 | **只在展开 / 卷起这两个时机各算一次**，不做实时跟随 |
| 推动动画 | 必须有。被推的窗口**平滑滑过去**，不是瞬移 |
| 卷起时 | 被推开的窗口**自动滑回原位**（不是停在原处） |
| 主人手动拖过的窗口 | 也参与推动（但见下面的"锁定"） |
| **锁定** | 标题栏加锁图标；锁定的浮窗**不参与推动**，且**固定在最底层**（别人可以盖住它） |
| 锁定持久化 | 每盒一份，重启后保持 |

#### 为什么"只在展开/卷起时算一次"

实时跟随（拖动中也持续重算）看着更"物理"，但它有两个必然的坏结果：

1. **振荡**。A 展开推 B 下移 → B 的新位置又触发一轮计算 → 可能推回 A 或推 C，
   来回几次窗口在自己抖。步骤 3 的 `computePushDown` 是**一次算完**的纯函数，
   它假设输入是稳定布局；实时重算会把这个假设打破。
2. **和手动拖动抢位置**。主人正拖着窗口，程序同时按几何算出来的位置挪它，
   手感直接崩掉。

所以：**在状态真正变化的那一刻算一次，然后把结果用动画播出去。**

#### 「滑回原位」需要记基线

被推开的位移是**临时的**，不能当成主人的意图（否则下次启动浮窗位置全乱）。
需要为每个浮窗记住"被推开之前它在哪"（`m_restPos`，rest position）：

- 展开时：`computePushDown` 算出 `dy` → 若该窗口还没有 `m_restPos`，
  把**当前位置**记成基线 → 然后 `move()` 到 `restPos + dy`。
- 卷起时：所有被推过的窗口 `move()` 回各自的 `m_restPos`，然后清掉基线。
- 基线存在内存里即可，**不落盘**（它是会话内的临时布局）。

#### 落盘的两处必须挡住

1. **`moveEvent` 里不能落盘被推的位移。**
   现有实现是 `moveEvent` 无条件 `scheduleGeometrySave()`。
   被推开的位移若照常落盘，就等于把"临时让位"当成了主人的意图 ——
   下次启动会带着被推开的位置醒来。
   用已有的 `m_layoutAdjusting` 标志挡住（它的注释里早就预告了这件事）。

2. **动画期间每一帧 `moveEvent` 都会触发**，所以标志必须覆盖整个动画区间
   （用 `QPropertyAnimation` 的 `stateChanged` 或起止两端成对设置），
   不能只在 `move()` 那一行前后包一下。

#### 锁定的语义

锁定 = "**这个窗口别动我**"。它一次关掉三件事：

1. 锁定后**不参与推动**：既不被别人推开（`computePushDown` 的 `others` 里排除它），
   也不推开别人（它自己展开时不推任何人）。
   ——注意 `computePushDown` 是纯函数，**排除的动作由调用方做**，
   不要把"锁定"这个概念漏进 `core/` 层（那里不应该知道 UI 的锁定按钮）。
2. 锁定后**悬停自动展开与自动卷起整个停用**（主人后补的要求，见下）。
3. 锁定后**固定在最底层**：`applyAlwaysOnTop(false)` 且不允许再置顶；
   别的浮窗可以盖住它。

外加：

- 锁图标放标题栏（卷起/关闭按钮旁边），**卷起时也要可见** ——
  否则卷起状态下没法解锁。
- 状态每盒一份存 `Settings`，键的形状仿照 `floatAppearance` 那套
  （只存非默认值）。

#### 锁定也关掉悬停（主人后补要求 + 三条拍板）

原规格只把锁定接在"让位"上。主人实测后要求：**锁定时同时关闭自动展开与卷起**。

**为什么这条是必须的**：主人给浮窗上锁，表达的是"这个窗口就摆在现在这个样子，
别碰它"。若锁定后鼠标移上去它照样弹开、移开又自己卷起，那么"锁"没锁住任何
他能看见的东西 —— 而他锁的多半**正是展开态**，结果一悬停它先变矮再展开，
观感上就是"锁了个寂寞"。

与"参与推动"的区别值得点明：
- 让位是**别人引发的**，锁住的是"别人对我的影响"；
- 悬停展开是**自己引发的**，锁住的是"我自己会动作"。

两件都锁，才叫"别动我"。

**三条拍板**：

| 项 | 决定 |
|---|---|
| 上锁时若正处于悬停展开态 | **保持当下状态不动**（展开的保持展开、卷起的保持卷起），不先收起来 |
| 锁定后手动操作 | **仍然可用**。双击标题栏、右键菜单、标题栏按钮照常工作 |
| 实现位置 | `hoverInteractionBlocked()` 的最前面，一条判断同时关掉两个方向 |

**实现要点**（两条容易漏的）：

- **两个方向必须分开验**。自动展开走 `canAutoExpand()`、
  自动卷起走 `shouldAutoCollapse()`，是两条不同的路径。
  只在其中一条加判断的话，另一种行为会静默残留 ——
  比如只在 `canAutoExpand` 里判锁定，于是"鼠标移开时它自己卷起来"照旧发生，
  而那恰恰是主人上锁时最不想看到的。
- **上锁时要停掉两个悬停定时器并清 `m_hoverExpanded`**。
  清标记是为了让窗口"停在当下这个样子"：若留着它，解锁后的下一次 leave
  会立刻把窗口收回去，与"保持不动"的规格冲突。
  停定时器是为了防"解锁后一个过期的收起计时突然生效"。
- **不影响手动路径**：手动双击、右键菜单都不经过 `hoverInteractionBlocked()`，
  所以天然可用 —— 这正是"锁的是自动，不是主人自己"。

#### 验证方式

- 单测（`tst_floatinglogic`）：锁定配置往返、`m_restPos` 记录的纯逻辑部分。
- 新探针 `pushdown_diag`：
  - 开两个上下相邻的浮窗，展开上面那个 → 断言下面那个**位置变了**（y 增大）
  - 断言它是**滑过去**的（采样位置曲线，中间值既不是起点也不是终点）
  - 卷起上面那个 → 断言下面那个**滑回原位**
  - **断言被推的位置没有落盘**（读 Settings 里的 geometry blob 对比）
  - 锁定下面那个 → 展开上面 → 断言它**没被动**
- 推动动画不能只断言"终值对" —— 那只证明它到了，不证明它是滑过去的。
  必须像 `rollup_diag` 那样采样中间帧。

#### 步骤 5 实施记录

##### 改动清单

| 文件 | 改了什么 |
|---|---|
| `core/settings.h/.cpp` | 新增每盒一份的 `floatLocked` / `setFloatLocked`（键 `floating/locked/%1`，只存非默认值）；`clearFloatAppearance` 顺带清掉它 |
| `ui/floatingboxwidget.h/.cpp` | 新增 `slideByForLayout` / `isPushedAside` / `expandedGeometry` / `isRolledUp` / `setLocked` / `isLocked`；新增 `m_restPos`、`m_posAnim`、`m_posAnimating`；`moveEvent` 加落盘守卫；标题栏加锁按钮；右键菜单加「钉住」；新增 `lockChangeRequested` 与 `rollUpStateChanged` 信号 |
| `ui/floatingboxmanager.h/.cpp` | 新增 `relayoutAround` / `layoutItemsExcept` / `setBoxLocked` / `isBoxLocked`；`openBox` 里接上两条新信号并同步钉住状态 |
| `tests/tst_floatinglogic.cpp` | 新增 E-04 ~ E-07 四条钉住配置用例 |
| `tools/pushdown_diag.cpp/.pro` | 新增探针（六个场景） |
| `verify.bat` | 新增第 12 阶段，总阶段数 12 → 13 |

##### 实现要点

**推动的接线在 `FloatingBoxWidget::rollUpStateChanged` 信号上。**
触发卷起/展开的路径有四条（双击标题栏、点「—」、右键菜单、悬停自动），
`emit` 放在 `applyRollUpState` **末尾**，是唯一能保证全覆盖的位置 ——
在四条路径外面各调一次必然漏一条，而漏掉的表现是
"某种方式展开时不会推开下面的浮窗"，很难想到去查。
放在末尾而不是开头也有讲究：那时 resize/动画都已就位，
`expandedGeometry()` 才能拿到本轮的终值。

**`expandedGeometry()` 是这一步的关键补丁。**
`applyRollUpState(false)` 先启动高度动画、再 emit，所以 manager 收到信号时
`frameGeometry()` 还是卷起时那条 30px 细线 —— 拿它去算等于
"展开不会推开任何人"，而且**不报错**。修法是让浮窗报告动画的 `endValue()`。

##### 踩到的四个坑

1. **`m_layoutAdjusting` 有两个写入方，会互相清除。**
   它原本只服务于"展开时别把 `m_expandedHeight` 写脏"，
   而"展开"与"被推开"恰恰是同一时刻发生的 —— 展开分支结尾无条件写假，
   正好把让位动画期间的落盘守卫抹掉。**症状正是本步最想避免的那件事**
   （被推开的位置被落了盘），而且只在"展开的同时正在被推"时才出现。
   修法：拆出独立的 `m_posAnimating`，由位置动画的 `stateChanged` 独占维护。

2. **构造期同步钉住状态会打乱淡入。**
   `setLocked(true)` 内部走 `applyAlwaysOnTop(false)`，而那个函数会
   `show()` —— 但 manager 是在 `prepareFadeIn` **之前**同步钉住的，
   于是窗口先以全不透明显示一帧，再被置 0 重新淡入，表现为"开启时闪一下"。
   修法：未显示时只设窗口标志、不 show（与构造期设置顶标志同一个路数）。

3. **`animatePosTo` 里"先判终值后 stop"的顺序有漏洞。**
   若此刻有一段动画正朝别处跑、而新目标恰好等于**当前**位置，
   那句早退会直接返回并让旧动画继续跑完 —— 窗口停在旧目标上。
   在让位场景里的表现是"某个窗口该收回原位，却仍停在被推开的位置"。
   修法：先 `stop()` 再判终值。

4. **只挡 `moveEvent` 是不够的，落盘还有两条旁路。**
   写完 `moveEvent` 的守卫之后复查才发现：`saveAllGeometry()`（退出时调用）
   与去抖定时器的回调都直接调 `currentGeometryBlob()`，**不经过 `moveEvent`**。
   于是"主人恰好在让位动画没跑完时退出程序"仍然会把被推开的位置写进配置。
   修法：把判断下沉到 `currentGeometryBlob()` —— 只要 `m_restPos` 有效，
   就按**原位**生成 blob。做法是临时把窗口挪回原位、取 blob、再挪回来
   （`saveGeometry()` 没有"按给定位置序列化"的重载，
   而自己拼 blob 的字节格式等于去依赖 Qt 的私有布局，更脆）。
   这个函数只在落盘时调用，不在热路径上。

   顺带：这个临时挪动本身也会触发 `moveEvent`，所以借 `m_posAnimating`
   把那段挪动期间的落盘一并挡掉 —— 否则"为了取原位而挪动"反而又排了一次
   落盘，而且读到的正是挪回来之后的中间态，比不修还糟。

##### 已知的语义简化（刻意，非遗漏）

**多个锚点同时展开时，收起其中一个会把另一个推开的窗口也一起收回原位。**

例：A 展开推下 C；B 也展开（它不再需要推 C，C 已经在下面了）；
此时 A 收起 —— C 会滑回原位，尽管 B 还展开着、本来仍需要压着 C。
结果是 C 与 B 叠在一起，直到下一次有人展开/收起才会修正。

没有为它加"谁推的 C"这套记账，理由：
需要给每个被推者记一份"被推的"，而推动是连锁的（A 推 B、B 又推 C），
归因会变成一张依赖图；而收尾很廉价 —— 主人接着点任意一个浮窗就会自愈；
真机上多锚点同时展开本来也罕见。已写进 `relayoutAround` 的注释。

##### 验证到什么程度

⚠️ **本次会话没有可用的 shell 工具，代码没有真正编译过。**
这是最重要的限制，不能含糊：所有改动都只经过**人工静态审阅**
（逐个核对了成员声明与使用、函数签名、include、枚举名、`std::as_const`
的迭代类型、`QPropertyAnimation` 的属性名字符串），
但**没有跑过一次 `qmake` / `mingw32-make`**。
交付方需要先跑一次构建确认。

新增的 `pushdown_diag` 探针**同样从未执行过**。它覆盖六个场景：

| 段 | 断言 |
|---|---|
| [1] | 展开上面的浮窗 → 下面的 y 变大；且采样轨迹里有 ≥3 个中间值（证明是**滑**过去的，不是瞬移） |
| [2] | 卷起 → 下面的滑回原位，且基线已清 |
| [3] | 展开并等过落盘去抖 → 解码配置里的 blob，断言记的是**原位**而不是被推开的位置 || [4] | 钉住下面的 → 展开上面的，断言它没被动 |
| [5] | 钉住状态落盘、且不牵连别的盒 |
| [6] | 钉住的浮窗自己展开时也不推别人 |

##### 仍未验证 / 不确定的地方

1. **没有编译过**（见上）。这是交付前必须补的第一件事。
2. **探针没有跑过**，六个场景是否全过未知。
3. **多显示器仍然只有单屏可测**（一直如此）。跨屏隔离只有单测覆盖。
4. **"动画看起来顺不顺"没法自动验证。** 探针只能证明轨迹里有中间帧，
   证明不了 200ms / OutCubic 的手感。需要人工看。
5. **钉住 = 压到最底层依赖窗口置顶标志**，而切换置顶会**重建原生窗口**。
   它与让位动画、圆角 mask 的交互**没有实测** —— 钉住的那一刻若正在播动画，
   原生窗口重建会不会打断动画，未知。
6. **多锚点同时展开的收尾有已知简化**（见 `relayoutAround` 里的说明）：
   收起其中一个会把另一个推开的窗口也收回原位。这是刻意的取舍，已写进注释。
7. **连续快速双击展开/收起**（动画未跑完就反向）没有测过。
   `m_restPos` 的存取是同步的，理论上安全，但没有实测。
8. **三个以上浮窗的连锁推动在真机上的观感**需要人工看。

#### 步骤 5 的诚实清单（当初预写的，保留以对照）

- 多显示器仍然只有单屏可测（一直如此）。
- "锁定后固定在最底层"依赖窗口置顶标志，**切换置顶会重建原生窗口**，
  与动画、圆角 mask 的交互需要实测。
- 三个以上浮窗的连锁推动在真机上的观感（会不会看着乱）需要人工看。

#### 步骤 5 复核记录（上级验证后补）

实施者交付时**明确报告"没有编译过"**（它的会话里没有可用的 shell）。
复核确实发现它编译不过，另有一处跨模块回归。以下是复核做了什么。

##### 修掉的两个问题

1. **`QPoint` 没有 `isValid()`。** 实施者照 `QRect` 的习惯写了
   `m_restPos.isValid()` 作为"有没有基线"的判据，编译直接报 5 处
   `'const class QPoint' has no member named 'isValid'`。
   改成显式的 `bool m_hasRestPos`。
   ⚠️ **不能**用哨兵坐标（如 `(-1,-1)`）代替 —— 多显示器下负坐标是完全
   合法的真实位置，那样判会把副屏上的浮窗误判成"没有基线"。

2. **9 个探针的 `.pro` 缺 `windowlayout.cpp`，导致链接失败。**
   `floatingboxmanager` 现在会调 `WindowLayout::computePushDown`，
   而 `windowlayout.cpp` 是步骤 3 才新增的翻译单元 ——
   那些早就写好的探针 `.pro` 里自然没有它。
   表现是 `undefined reference to WindowLayout::computePushDown`，
   而且 `verify.bat` 第 5 阶段（`fade_diag`）直接挂掉。
   已给 9 个文件统一补上：`appearance_diag` / `corner_diag` / `corner_dump` /
   `fade_diag` / `fullscreen_dump` / `mask_diag` / `screen_probe` /
   `titlebar_diag` / `topmost_diag`。
   → 教训：**新增一个被广泛依赖的翻译单元时，要回头检查所有 .pro**。
   `DeskTidy.pro` / `CMakeLists.txt` 同步了不够，那一堆探针各有一份源文件清单。

##### 验证结果

`verify.bat` 扩到 **13 个阶段，RESULT: ALL PASSED**（退出码 0）：

| 阶段 | 结果 |
|---|---|
| 2 浮窗单测 | **71 项**（原 67，+钉住配置 4 条） |
| 12 推开/滑动/钉住（新增） | **13 项** |

`pushdown_diag` 实测到的行为：

| 场景 | 实测 |
|---|---|
| 展开上面 → 下面被推 | y 200 → **400** |
| **是不是滑过去的** | 采样 44 帧，其中 **8 帧是中间值**（不是瞬移） |
| 卷起上面 → 下面滑回 | y 400 → **200**，且基线已清 |
| **展开时报出的目标高度** | **300**（不是 30px 细线 —— 这是本步最关键的接线） |
| 被推的位置**没落盘** | 配置里记的是 **200**（原位），不是 400 |
| 钉住的窗口不被推 | y 保持 200 |
| 钉住的窗口自己展开也不推别人 | 对方 y 保持 200 |
| **四轮推收是否漂移** | 四轮都是 `200 → 200 → 400`，**无累积漂移** |

最后一条是复核时**自己加**的：实施者的探针没验"反复推收会不会逐轮往下
掉一截"，而那正是基线记账写错时的典型症状 —— 前一两轮看着正常，
几轮之后浮窗跑出屏幕。

##### 复核修掉的两处**探针自身**的错

跟前面几步一样，探针的假失败与产品的真失败长得一样，记下来免得重蹈：

1. **问 `expandedGeometry()` 的时机不对。** 它回答的是"你展开后会有多高"，
   而复核一开始在浮窗**还卷着**的时候问，得到 30，打印出来像功能坏了。
   必须在**展开动画刚起步那一刻**问才是有效判据 ——
   而那也正是 manager 真正去问它的时刻。
2. **没先收回基线就重摆窗口。** [7] 段里直接 `move()` 摆位置，
   但某个窗口此刻还处于"被推开"状态，它的基线指着旧原位，
   下一次滑回时被拉回**那个旧原位**，把刚摆好的布局冲掉 ——
   表现是"我明明挪回来了，它自己又跳回去"。
   摆位置前必须让两者的让位状态都归零。

##### 仍未验证的项（复核后诚实清单）

1. **动画手感没人看过。** "推动是滑过去的"已被 8 帧中间值证明，
   但 200ms / OutCubic 看起来舒不舒服，只能人眼判断。
2. **钉住 = 压到最底层依赖 `setWindowFlags`，而它会重建原生窗口。**
   与让位动画、圆角 mask 的交互**没有实测**。
   钉住那一刻若正在播动画会不会中断，未知。
3. **`currentGeometryBlob()` 里"临时挪回原位取 blob 再挪回来"会不会闪一下。**
   实施者的判断是 `Qt::Tool` 窗口在事件循环返回前不重绘，应该看不出来 ——
   但**这是推理，不是实测**。
4. **多锚点同时展开的收尾有已知简化**：收起一个会把另一个推开的也一起
   收回原位（没有做"谁推的"归因记账）。已写进代码注释，
   代价是多点一下任意浮窗就会自愈。**这是刻意的取舍，不是遗漏。**
5. **连续快速点击展开/收起**（动画没跑完就反向）没测。
6. **三个以上浮窗的连锁推动观感**需要人工看。
7. **多显示器**仍然只有单屏可测 —— 从头到尾都是。
8. **真实鼠标的悬停 + 推开叠加**（悬停展开时把下面的推走）没有真机验证；
   `pushdown_diag` 走的是双击路径。

#### 步骤 5 补：锁定同时关闭悬停（验证记录）

主人后续要求"锁定功能开启时同时关闭自动展开与卷起"，已实现并验证。

**改了一处产品代码**：`hoverInteractionBlocked()` 最前面加 `if (m_locked) return true;`。
这一条同时覆盖两个方向，因为 `canAutoExpand()` 与 `shouldAutoCollapse()`
都以它为闸门。

**另加两处收尾**（都在 `setLocked` 里）：`stopHoverTimers()` + 清 `m_hoverExpanded`，
让窗口"停在当下这个样子"，而不是解锁后又被一个过期的收起计时卷走。

**验证**：`pushdown_diag` 从 13 项扩到 **18 项**，新增 `[6b]` 段，
**两个方向分开验**（这是关键 —— 只验一个方向的话，另一个会静默残留）：

| 场景 | 实测 |
|---|---|
| 钉住 + 卷起 + 悬停 750ms | 高度保持 **30**（不展开） |
| 钉住 + 展开 + 离开 1000ms | 高度保持 **300**（不卷起） |
| 钉住 + 手动双击卷起 | 高度 300 → 30（**手动仍然可用**） |

`verify.bat` 13 阶段全过，退出码 0。

**未验证**：`[6b]` 用的是手工投递的 `QEnterEvent`/`Leave`（绕过窗口系统），
所以"真实鼠标移到一个钉住的浮窗上会不会有任何动作"仍需人工确认。
另外"上锁那一刻它正好在动画中间"（展开动画没跑完就上锁）没测。

### 步骤 6：修「盒内图标错乱」

主人实测报的现象：**每次存入新东西，收纳盒里的文件图标就错乱**
（图标是错的、显示成通用白纸图标）。两个界面都有，切大小图标模式时也有。

**这里有两处独立的原因，都修了。** 第二处是排查第一处时顺带发现的，
而且它才是"每次收纳都错乱"的直接原因。

#### 原因一：图标缓存按**后缀**做键（错得离谱）

`setItems` 里原来是：

```cpp
const QString key = entry.isDir ? "<dir>" : QFileInfo(entry.name).suffix().toLower();
```

它假设"同后缀 = 同图标"。但 `QFileIconProvider::icon(文件)` 返回的是
**每个文件各自**的图标 —— 还取决于嵌入图标、`.lnk` 指向的目标、类型关联。
`.exe` 尤其明显：每个程序都有自己的嵌入图标。

于是同后缀的文件互相借用图标，而"谁先被扫到"由目录遍历顺序决定
（`listBoxItems` 用的是 `QDir::NoSort`，顺序本就不保证）。
列表每次收纳/撤销都会重建，所以图标跟着重排 —— 观感正是"错乱"。

**修法**：整个删掉缓存，每个文件单独取。

**删之前先量了代价**（`tools/iconperf`，取真实 64×64 位图）：

| 文件数 | 每文件单独取 | 按后缀缓存 | 差值 |
|---|---|---|---|
| 50 | 4 ms | 1 ms | 3 ms |
| 100 | 6 ms | 3 ms | 3 ms |
| 200 | 10 ms | 5 ms | 5 ms |

200 个文件也只差 5ms，离"能感觉到卡"（~100ms）差一个数量级。
为几毫秒换图标错乱不值。

> 测耗时时踩的坑：第一版只调 `prov.icon()` 然后读 `cacheKey()`，
> 三档全是 0ms。那不是快 —— `QFileIconProvider::icon()` 是**惰性**的，
> 真正的 shell 查询发生在**要位图**的那一刻。必须 `pixmap()` 取出来才测得准。

#### 原因二：`QFileIconProvider` 返回的 QIcon 被**隐式共享**（真凶）

删掉缓存后图标**仍然**错乱。实测症状非常明确（`tools/icon_probe`）：

```
四个不同的 exe，在列表里全显示同一个图标，而且是**第一个**那个的图标；
同一个循环里直接问 provider，它给的却是四个互不相同的正确图标。
```

```
calc.exe      控件[c9c32a…]  provider[c9c32a…]  相同=是
charmap.exe   控件[c9c32a…]  provider[735563…]  相同=否   ← 拿到了 calc 的
cmd.exe       控件[c9c32a…]  provider[a73a30…]  相同=否   ← 同样是 calc 的
notepad.exe   控件[c9c32a…]  provider[c53e71…]  相同=否   ← 同样是 calc 的
```

`QFileIconProvider` 在 Windows 上背后是系统的 shell 图标缓存，
它返回的 `QIcon` 是隐式共享的。直接把这样的 `QIcon` 交给
`QListWidgetItem`，多个条目会指向同一份底层数据。
列表每次刷新都重建 → 谁排第一谁说了算 → 每次收纳都会重排。

**修法**：把位图取出来重新包一个 `QIcon`，切断共享：

```cpp
item->setIcon(QIcon(icon.pixmap(copyPx, copyPx)));
```

这一句同时对两种模式成立：图标模式下 `copyPx = wantPx`（顺带解决防糊），
列表模式下退化成系统默认档（16/32）。

#### 验证

新增探针 `tools/icon_probe`，接入 `verify.bat` 第 13 阶段（**10 项全过**）。
`verify.bat` 扩到 **14 个阶段，RESULT: ALL PASSED**。

探针的判据是**图标身份**，不是"有没有图标"—— 因为串号时列表里
**仍然有一个图标**，只是错的那个，光看存在性是发现不了的：

| 检查 | 说明 |
|---|---|
| 整批 vs 单独一致 | 同一个文件单独喂进去与整批时，图标必须相同（对等比较，命中串号） |
| 不同程序的图标互不相同 | 四个真实 exe（notepad/calc/cmd/charmap）不得显示成同一个图标 |
| 两种视图模式都测 | 产品在列表模式与大图标模式下走**不同的取图路径**，只测一种会漏 |

样本必须包含"**后缀决定不了图标**"的文件（`.exe`、无后缀文件）。
第一版只造了 `a.txt/b.txt/c.txt`——那种样本即使配着旧 bug 也能全对
（同后缀的普通文件图标本来就一样），**注入旧 bug 实测仍然全 PASS**，
等于什么都没测。改用真实 exe 后才咬得住。

#### 排查过程中探针自己踩的三个坑

这次卡了很久，因为**探针的判据反复在说谎**，每次都表现得像产品 bug。
三条都记在 `icon_probe.cpp` 注释里：

1. **不能比 `QIcon::cacheKey()`**。它标识 QIcon **对象实例**而非图像内容，
   同一个文件取两次就是两个不同的 key → 全部条目假报"不符"。
2. **不能拿"另取一次 provider"当参照**。参照走的是"多尺寸 QIcon 直接渲染"，
   而产品在图标模式下会先重包，两条路径对同一文件给出的像素本就可能不同。
   这条判据让 `calc.exe` 与其余三个 exe 全部假报"相同"。
   导出图片一看，两边画的都是计算器图标 —— 产品根本没错。
   → 参照物应改用**文件自身的事实**（这是四个不同的程序），
   而不是另一条渲染路径。
3. **不能假设 `listBoxItems` 的顺序稳定**（它是 `QDir::NoSort`）。
   拿 `all.at(i)` 去配对 `batch->item(i)` 在跨调用时可能错位。

#### 仍未验证的

1. **真实桌面上的 `.lnk` 快捷方式未覆盖。** 探针用的是 `.exe` 与普通文件。
   快捷方式的图标来自它的目标，理论上同一机制，但**没实测**。
2. **列表里显示的观感没人看过。** 探针证明"图标身份对得上"，
   证明不了"画出来清不清楚、大小图标档位合不合适"——那需要人眼看。
3. **图标数量很多时（几百个）的实际刷新手感未测。** 量过程（见上表），
   但那是脱离 UI 的裸耗时，真实刷新还包含列表重建。
4. **网络盘 / U 盘上的文件取图标**未测（shell 行为可能不同）。

### 步骤 7：修「网址快捷方式全变白纸」

主人接着报：盒里有一批文件图标是**白纸**。查下来是 `.url`（网址快捷方式）。

#### 现象与定位

对主人三个真实盒子做了一次**只读**体检（71 项），结果很干净：

- **没有任何一个是系统通用图标**（0 / 71）—— 所以"白纸"不是"读不到图标"；
- 但 **11 个 `.url` 的数字完全一致**：
  `不透明742/1024、颜色数81`（32px）/ `不透明203/256、颜色数32`（16px）。

**十一个不同的游戏，图标逐像素完全相同** —— 那才是主人看到的"白纸"：
不是系统兜底图标，而是**按 `.url` 后缀给的浏览器关联图标**，对所有 `.url` 都长这样。

#### 根因

`.url` 是 INI 格式的**纯文本**文件，图标写在文件**里面**：

```ini
[InternetShortcut]
URL=steam://rungameid/1230140
IconFile=C:\Program Files (x86)\Steam\steam\games\8995027a….ico
IconIndex=0
```

而 `QFileIconProvider` 只看**后缀**、按类型关联给一个通用图标，
**完全不解析文件内容**。于是十几个 `.url` 长得一模一样。

`.lnk` 没有这个问题 —— shell 会解析它的目标，所以那 53 个快捷方式图标
本来就是各自的（体检数据也证实：53 个各不相同）。

#### 修法

自己把 `IconFile=` 读出来，用那个文件建图标。实测对比
（主人盒里 11 个真实 Steam 快捷方式，`.ico` 全部存在，几十到几百 KB）：

| 取法 | 不同图标数 |
|---|---|
| `QFileIconProvider` | **1 / 11** |
| 读 `IconFile=` | **11 / 11** |

读不到、或 `IconFile` 指向的文件已不在时**回退**到原来的通用逻辑 ——
绝不能因为一个失效的 IconFile 就让条目变成没有图标。

手写解析而不用 `QSettings`：它读 INI 时对"含中文与空格的 Windows 路径"
依赖编码设置，还会把值里的分号反斜杠当转义 —— 拿它读**路径**容易出岔子。
这里只要一行 `IconFile=<路径>`，手写更可控。

#### 验证

`icon_probe` 从 10 项扩到 **16 项**，`verify.bat` 14 阶段全过。新增两条：

| 检查 | 说明 |
|---|---|
| 每个 `.url` 显示的是它 IconFile 指定的图标 | 与那个 `.ico` 渲染出来的像素比对 |
| 指向不同图标的 `.url` 显示得也不同 | 直接照着"全变白纸"这个现象写 |

另外对主人**真实盒子**跑了一次只读复检：

```
盒「游戏」 11 个 .url
  → 不同图标 11 / 11
仍是系统通用图标 = 0
```

**全程只读，没有修改任何文件**（改完核对过三个盒的项数：26 / 16 / 29，与改前一致）。

#### 这一步踩的坑

**探针样本用错了素材。** 第一版拿 `System32` 下的 `.exe` 当 `IconFile` 目标，
结果 4 个 `.url` 全部报"图标与 IconFile 不一致"，看着像产品没修好。
分段诊断后确认是 `QIcon("xxx.exe")` **返回 null** ——
`QIcon` 只认图像文件（`.ico`/`.png`），不解析 exe 的嵌入图标。
而主人盒里的 `IconFile` 指向的正是 Steam 的 `.ico`，那条路本来是通的。
换成 Steam 目录下的真实 `.ico` 后立刻全绿。

→ 教训：造样本时要让样本与**真实数据形态一致**。
主人报的是 11 个 Steam 游戏，样本就该用 Steam 的 `.ico`，
而不是"手边随便找个有图标的文件"。

#### 仍未验证的

1. **`.lnk` 里图标指向失效的情况**（目标被删/移动）没测 —— 本次范围只到 `.url`。
2. **`.url` 的 `IconFile` 指向 `.exe` 时会退化**（`QIcon` 不解析 exe 嵌入图标），
   此时走回退分支、显示通用图标。主人盒里没有这种数据，**未实测**。
3. **图标画出来清不清楚**仍然只能人眼看。


### 仍未验证的项（诚实清单）

1. **`PreviewDialog` 以 `Qt::Tool` 浮窗为 parent 时的层级表现** —— 模态框理论上可能跑到浮窗后面。
   代码里已标注退路（改 `parent = nullptr` + `activateWindow()`）。**需人工点一次「收纳桌面」确认。**
2. **运行中切换「总在最前」的观感** —— 功能可用（实测中被误触切换过并生效），
   但切换瞬间是否闪烁、是否丢激活态，需人工看。
3. **多显示器拔插后几何恢复** —— `saveGeometry()` 的 blob 理论上含屏幕与 DPI 上下文，
   但没有第二块屏幕可测。
4. **托盘图标在 Win11 溢出区的可见性** —— 图标确实注册了，但 Win11 默认折叠进溢出弹窗，
   用户可能一时找不到「退出」。

