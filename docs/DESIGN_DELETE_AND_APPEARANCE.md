# DeskTidy 删除收纳盒 + 浮窗外观自定义 —— 技术设计方案

> 状态：**已实施**（实施记录见文末）
> 影响范围：新增 1 个 ui 文件、改 8 个文件、改 2 份构建清单

---

## 0. 本方案要解决的问题

DeskTidy 第一版做完了「收纳」与「常驻浮窗」，但留下两个明显的缺口：

1. **收纳盒只能建、不能删。** 主人建错名字、或者某个盒已经不需要了，只能自己去资源管理器里删目录 —— 而那样做**盒里的文件会跟着一起没**，正是这个工具最想避免的事。
2. **浮窗长相是写死的。** 所有人所有盒都长一个样：列表模式、不透明、固定图标大小。桌面工具的外观是最影响「愿不愿意一直摆着」的因素。

---

## 1. 不做什么（边界）

| 不做 | 原因 |
|---|---|
| 删除盒子时**永久删除**盒内文件 | 违反项目铁律。本方案只做「还原回桌面」+「回收站兜底」 |
| 把删盒动作并入撤销栈 | 文件已经回桌面了，效果上等价于一次完整还原，再叠一层撤销语义会让「撤销」到底撤销什么变得含糊 |
| 浮窗外观做成**全局**设置 | 需求方明确要求每盒一套。不同的盒用途不同（"工作"想小图标省地方，"游戏"想大图标好看） |
| 浮窗圆角 / 半透明玻璃效果 | 需要 `Qt::WA_TranslucentBackground`，牵涉无边框窗口重绘时序，与第一版「刻意不做圆角」的结论一致。**透明度**用窗口整体不透明度实现，不引这套 |
| 外观设置里做**自定义颜色/字体** | 需求只提了透明度 + 排列方式 + 显示方式。加颜色会连带做取色器与配色协调，是另一个功能 |
| 删除根目录本身 | 只删单个盒。根目录留着，否则「还没收纳过」的状态会被破坏 |

---

## 2. 现状核实结果（逐条对源码确认）

### 2.1 删除相关

| 事实 | 出处 |
|---|---|
| `BoxManager` 只有 4 个自由函数，**零删除代码** | `core/boxmanager.h:23,26,32,38` |
| `Collector` 是全应用唯一执行文件移动的地方，且**没有删除函数** | `core/collector.h:10-13` |
| `AppService::restorePaths` 已能一次还原一批路径，**可直接复用** | `core/appservice.cpp:116-163` |
| `restorePaths` 内部 `moveFinished` 的 actionLabel **写死为「还原到桌面」** | `core/appservice.cpp:159-160` |
| `restorePaths` emit `boxContentsChanged(QString())`（空串 = 全部刷新） | `core/appservice.cpp:157` |
| `restorePaths` **不碰撤销栈** | 全函数无 `m_undo` 引用 |
| `FloatingBoxManager::closeBox` 幂等，并摘掉 `Settings::openBoxNames` 里的记录 | `ui/floatingboxmanager.cpp:135-153` |
| `FloatingBoxManager` 已有「盒目录消失就自动关浮窗」的自愈逻辑 | `ui/floatingboxmanager.cpp:202-247` |
| 主窗口左栏右键菜单每次**现场构造** QMenu，插新项无副作用 | `ui/mainwindow.cpp:577-612` |
| `MainWindow::refreshBoxes()` 是 **private** | `ui/mainwindow.h:98` |
| 盒目录存在性判断走 `QFileInfo::isDir()` | `ui/floatingboxmanager.cpp:261-266` |

**`Collector::restore` 的字段方向（本方案最容易搞反的一处，务必按此实现）：**

> **入参沿用 `collect()` 的方向**，不是"相反"：
> - `sourcePath` = 当初被收走的位置（桌面原位）→ restore 用它推**落点目录 + 落点文件名**
> - `finalPath` = 当前实际所在位置（盒内）→ restore **从这个路径搬走**
> - **只有返回值**才反过来（`sourcePath` = 被撤销的位置，`finalPath` = 落点）
>
> 依据：`collector.h:37-52` 的警告块、`appservice.h:83-88` 的复述、实现见 `collector.cpp:230,236-253`。
> 这个坑项目已经踩过一次（`docs/DESIGN_FLOATING_BOXES.md` 的实施记录里有）。

### 2.2 ⚠️ 关于回收站：**以真机实测为准，不要信"文档说法"**

实测探针 `tools/trash_probe2.cpp` 在**本机 Qt 6.11.1 / Windows 11** 上的结果：

| 用例 | 返回值 | 原位置 | 回收站条目增量 |
|---|---|---|---|
| 单文件 | `true` | 消失 | +1 |
| **带 3 个文件 + 1 层子目录的目录** | **`true`** | **消失** | **+1** |

`QFile::supportsMoveToTrash()` 返回 `true`。回收站内路径形如 `F:/$RECYCLE.BIN/<SID>/$RXXXXXX`。

**结论：不需要引 `windows.h`、不需要 `SHFileOperationW`、不需要额外链接 `shell32`。用 `QFile::moveToTrash` 即可，且目录是递归进回收站的。**

> 网上与部分 AI 会说「Windows 上 `moveToTrash` 不支持目录」，那是过时或错误说法。本项目以实测为准。**但这条结论是绑定在 Qt 6.11.1 上的**，若将来升级 Qt 大版本需重测。

### 2.3 外观相关

| 事实 | 出处 |
|---|---|
| `ItemListWidget::Options` 现在只有 `doubleClick` / `draggableOut` 两个字段 | `ui/itemlistwidget.h:48-52` |
| 头文件有硬约束：**新增能力一律走 opt-in，默认值必须保持改造前行为** | `ui/itemlistwidget.h:13-15` |
| `ItemListWidget` 现在**只有列表模式**，从未调用 `setViewMode`/`setIconSize`/`setGridSize` | grep 全文件零命中 |
| 唯一写死的尺寸是拖拽缩略图 `pixmap(32,32)` | `ui/itemlistwidget.cpp:188` |
| 图标按后缀缓存，懒建 `QFileIconProvider` | `ui/itemlistwidget.cpp:87-121` |
| ⚠️ `setDragHighlight` 会用 `setStyleSheet(QString())` **清空列表样式表** | `ui/itemlistwidget.cpp:270` |
| 浮窗样式表硬编码 3 处：主窗体 / 标题栏 / 按钮 | `floatingboxwidget.cpp:246,258,109-125` |
| 尺寸常量 `kDefaultWidth=260` `kDefaultHeight=300` `kMinimumWidth=220` `kMinimumHeight=120` `kTitleBarHeight=28` | `floatingboxwidget.cpp:50-54` |
| 浮窗右键菜单现场构造 | `ui/floatingboxwidget.cpp:636` 起 |
| `MainWindow::onOpenSettings()` 约 40 行，对话框**在函数体内现场构造**，非独立类 | `ui/mainwindow.cpp:531-572` |
| `PreviewDialog` 是「独立可复用对话框」的现成先例 | `ui/previewdialog.h:23-34` |

**⚠️ 卷起逻辑与尺寸的纠缠（改尺寸最大的风险）**：

```
applyRollUpState(true)   (:578-602)
    卷起时 setMinimumHeight(rolledUpHeight()) + setMaximumHeight(rolledUpHeight())
    —— 高度被 min/max **锁死**

applyRollUpState(false)  (:603-622)
    展开时恢复 min = kMinimumHeight、max = QWIDGETSIZE_MAX，
    再用 m_expandedHeight（或 kDefaultHeight）resize

resizeEvent              (:776-793)
    if (!m_rolledUp && height() > rolledUpHeight()) m_expandedHeight = height();
    然后 scheduleGeometrySave()

applySavedGeometry       (:721-745)
    restoreGeometry(blob) 之后，**必须重新施加一次卷起状态** ——
    因为 blob 存的是展开尺寸，会把卷起的 min/max 限制顶开
```

**结论：任何「改尺寸」的操作都必须**：
1. 先判断 `m_rolledUp`，卷起时**只改宽度、不碰高度**（否则会被 min/max 顶掉）
2. 改完调 `scheduleGeometrySave()` 落盘
3. 若要同时改 min 尺寸，注意卷起状态下 `setMinimumHeight` 正被占用

### 2.4 Settings 现状

现有键（`settings.cpp:18-28`）：

```cpp
"filters/excludedNames"      // 排除名单
"ui/lastBoxName"             // 上次选中的盒
"floating/openBoxes"         // 哪些盒开了浮窗
"floating/alwaysOnTop"       // 置顶（全局，非每盒）
"ui/trayHintShown"           // 托盘提示是否已展示
"floating/geometry/%1"       // 每盒几何
"floating/rolledUp/%1"       // 每盒卷起状态
```

`floatRolledUp` 那一对是「每盒一份配置」的**现成模板**，且它有一个好约定（`settings.cpp:215-219`）：

```cpp
// 展开是默认状态，不必落键 —— 少一个键就少一份可能失真的状态。
if (rolledUp) ini.setValue(key, true); else ini.remove(key);
```

盒名转键名的编码：`encodeBoxName()`（`settings.cpp:41-46`），**匿名命名空间的私有静态函数**，Base64Url 编码（因为 QSettings 用 `/` 做层级分隔）。它只在 `settings.cpp` 内可见 —— **新访问器写在同一个文件里就能直接调，不要把它提升出去**。

---

## 3. 实施顺序：先做外观，再做删除

理由：

1. **风险不对称。** 外观是纯新增 + 纯配置，最坏情况是「长得不对」。删除功能最坏情况是**「文件没了」** —— 量级完全不同。
2. **删除功能依赖外观的一部分成果。** 删盒流程要「关掉对应浮窗」，会碰到 `FloatingBoxManager`；外观功能同样要改它（加外观变更广播）。先做外观能把这块改动的「手感」摸熟。
3. **先做的能被后做的当回归基线。** 外观做完跑一遍全量测试，删除功能再改时一旦测试变红就能立刻定位。

---

## 4. 功能 2：浮窗外观自定义

### 4.1 配置项设计

新增三个**每盒一份**的键，沿用 `floatRolledUp` 的模式：

```cpp
const QString kKeyFloatViewModeFmt = "floating/viewMode/%1";
const QString kKeyFloatIconSizeFmt = "floating/iconSize/%1";
const QString kKeyFloatOpacityFmt  = "floating/opacity/%1";
```

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `viewMode` | int（枚举） | `0` = List | 0=List 1=SmallIcon 2=MediumIcon 3=LargeIcon |
| `iconSize` | int（像素） | `0` = 跟随 viewMode | 仅在非 List 模式下有意义 |
| `opacity` | int 0–100 | `100` | 存百分比而非 double |

**为什么 opacity 用 int 0–100 而不是 double**：`QSettings` 的 INI 后端写 double 会有浮点格式化往返误差，而透明度不需要那个精度。转换只在 UI 层做。

**「只存非默认值」约定**：100% 不透明、List 模式都不落键。理由与 `floatRolledUp` 相同：少一个键就少一份可能失真的状态。

> **0% 的处理**：0% 完全不可见 = 用户再也找不到自己的浮窗。所以 0 与「重置为默认」等价对待，实际写入用 `qBound(1, ...)` 兜在 1%。

### 4.2 数据契约：`BoxAppearance`

在 `core/coretypes.h` 加一个值结构体（与 `StorageBox` 同级）：

```cpp
struct BoxAppearance
{
    // 数值与 QListView::ViewMode 不绑定 —— 那是 Qt 的实现细节，
    // 存进配置的应该是本工具自己的语义，将来换渲染方式也不用迁移配置。
    enum class ViewMode { List = 0, SmallIcon = 1, MediumIcon = 2, LargeIcon = 3 };

    ViewMode viewMode = ViewMode::List;
    int      iconSize = 0;      // 0 = 跟随 viewMode 推导
    int      opacity  = 100;    // 百分比 1..100

    static int defaultIconSizeFor(ViewMode mode);
    int effectiveIconSize() const;
    bool isDefault() const;
};
```

**图标尺寸档位**：

| ViewMode | 图标像素 |
|---|---|
| List | （不适用） |
| SmallIcon | 16 |
| MediumIcon | 32 |
| LargeIcon | 64 |

### 4.3 `ItemListWidget` 支持多视图模式

**选择：加 `Options` 字段（opt-in），不改成独立方法** —— 与项目既有硬约束一致，编译期就能保证主窗口不受影响。

**⚠️ 切换 `IconMode` 必须设的属性（漏一个就出问题）**：

| 属性 | 值 | 不设会怎样 |
|---|---|---|
| `setMovement` | `Static` | 图标能被拖乱，且顺序存不下来，刷新即乱 |
| `setResizeMode` | `Adjust` | 窗口拉大后不重排，右侧留白 |
| `setWordWrap` | `true` | 长文件名把条目撑爆 |
| `setUniformItemSizes` | `true` | 性能退化 |
| `setGridSize` | 按图标算 | 条目紧贴、看不出分组 |
| `setSpacing` | 小值 | 同上 |
| **切回列表时复位以上全部** | — | 残留网格让列表模式变得很怪 |

**回归保障：默认配置（List + iconSize==0）下早退、什么都不做** —— 主窗口那条路径连 `setViewMode` 都不会被调到。

**拖拽缩略图 `pixmap(32,32)` 保持不变**：它是拖动时跟着鼠标的小图，32×32 在任何视图模式下都合适。

### 4.4 透明度实现

**用 `QWidget::setWindowOpacity()`，不用样式表。**

理由（三条，第二条是决定性的）：

1. **作用域正确**：整个浮窗（含标题栏、边框）一起变淡才自然。
2. **⚠️ 不能碰列表的样式表**：`ItemListWidget::setDragHighlight()` 会在拖拽结束时执行 `setStyleSheet(QString())` —— **整条样式表被清空**。若透明度靠列表样式表实现，每次拖拽进出都会把透明度冲掉。**这是本方案里最容易踩的坑。**
3. **不污染几何**：不改窗口尺寸，不触发 `resizeEvent`，不与卷起/尺寸逻辑打架。

**下限 0.2 的双重保险**：Settings 层兜 1%，应用层再兜 20%。两层都兜是因为「浮窗彻底看不见」是**不可恢复**的故障。UI 滑块物理下限也是 20%。

### 4.5 尺寸自动调整（与卷起状态的纠缠）

**核心约束**：卷起状态下高度被 `setMinimumHeight/MaximumHeight` 锁死，此刻 `resize()` 改高度会被顶掉。

```cpp
void FloatingBoxWidget::resizeForAppearance()
{
    // 卷起状态：高度是锁死的，此刻不能碰高度；且此时图标不可见，直接返回。
    if (m_rolledUp)
        return;

    // 图标模式下按 3 列 × 2 行算最小够用尺寸
    if (m_appearance.viewMode != BoxAppearance::ViewMode::List) {
        targetW = qMax(width(), cellW * 3 + 16);
        targetH = qMax(height(), chrome + cellH * 2);
    }

    // 只在"确实需要变大"时才 resize —— 无脑 resize 会覆盖主人特意调过的尺寸
    if (targetW > width() || targetH > height()) {
        resize(...);
        scheduleGeometrySave();
    }
}
```

**插入位置**：`applyAppearance()` 末尾；另需在 `applyRollUpState(false)` 展开分支末尾补一句 —— 否则「卷起状态下改大图标 → 展开」会维持旧尺寸。

**⚠️ 为什么不做「缩小」**：主人可能特意拉大过窗口，自动缩小会抹掉他手动调的尺寸，而且「被程序改了尺寸」很招人烦。**只在需要变大时才动尺寸**。代价是切回小图标后右侧有留白 —— 这是刻意的取舍。

### 4.6 两个入口的共享：改动如何广播

**照抄现有的信号中继模式** —— `FloatingBoxManager::boxWindowToggled` 就是现成先例。

```cpp
// floatingboxmanager.h 追加
signals:
    void boxAppearanceChanged(const QString &boxName);
public:
    void applyAppearance(const QString &boxName, const BoxAppearance &appearance);
    BoxAppearance appearanceOf(const QString &boxName) const;
```

**收在 manager 里而不是让两个入口各自写配置**：入口有两个，各写一份必然有一处漏发信号，表现是"从一个入口改完，另一个入口还显示旧值"。

**⚠️ 依赖方向问题**：`FloatingBoxWidget` 只持有 `AppService*`，不持有 manager。让浮窗反向持有会形成循环引用。

**选 B 方案**：不改浮窗构造签名，由 manager 在 `openBox` 里连接（manager 有 widget 指针）。**与现有三个信号的连接方式一致，不引入反向依赖。**

### 4.7 外观对话框：新建独立类

**选择：新建 `ui/appearancedialog.h/.cpp`。**

理由：
1. `onOpenSettings` 已 40 行，塞进去会膨胀到 150 行以上，且混了两件不相干的事（排除规则是**全局**的，外观是**每盒**的）
2. 外观对话框需要"选哪个盒"，是独立的一块交互
3. 项目已有 `PreviewDialog` 先例

**交互**：
- 顶部 `QComboBox` 选盒
- `QComboBox` 选视图模式（列表/小图标/中图标/大图标）
- **透明度滑块范围 20–100**（不是 0–100），旁边显示"85%"
- **预览区**：内嵌一个 `ItemListWidget`，填假条目
- 底部：「恢复默认」「应用」「关闭」——**「应用」不关闭对话框**，方便连续试

### 4.8 浮窗右键菜单入口

在 `contextMenuEvent` 插入「外观」子菜单，含「显示方式」（四个互斥可选项，用 `QActionGroup`）与「透明度」（几个档位，菜单里放滑块交互别扭）与「更多设置…」。

> **⚠️ `QActionGroup` 与动态构造菜单**：菜单是现场构造的，`QActionGroup` 的父子关系要设对，否则反复打开菜单会泄漏。列为待实测项。

**「排列方式」的说明**：需求原话是「排列方式（如大图标，小图标等）」。括号里的例子说明**指的是视图模式**。**目录内容的排序规则**（按名称/时间/大小）不在本方案内。

---

## 5. 功能 1：删除收纳盒

### 5.1 语义与三步流程

```
Step 0  前置检查（盒目录是否还在）
Step 1  把盒内所有条目还原回桌面
Step 2  还原不回去的（连同盒目录本身）丢进回收站
Step 3  关掉该盒的浮窗、清掉配置
Step 4  刷新主窗口左栏
```

**每一步失败的走向**：

| 步骤 | 失败情形 | 走向 |
|---|---|---|
| Step 0 | 盒目录已不存在 | **不算失败**，直接返回 ok（主人的目的已达成） |
| Step 1 | 个别条目 Failed | **不中断**。计入 `trashedItems`，交给 Step 2 |
| Step 2 | `moveToTrash` 返回 false | **整体失败**，`error` 非空，**不执行 Step 3/4** |
| Step 3 | 该盒没开浮窗 | `closeBox` 幂等，静默通过 |

**为什么 Step 2 失败就不能继续**：盒目录还在磁盘上，下次扫描还会被列出来。此时若关了浮窗、刷了列表，主人会看到「盒消失了」，但下次打开程序它又回来了 —— **状态分裂，比直接报错难查得多**。

### 5.2 回收站那一步（关键设计点）

**两种做法的对比**：

| 做法 | 过程 | 结果 |
|---|---|---|
| A. 先挑出来再删空目录 | 逐个失败文件 `moveToTrash`，剩下的空目录再丢 | 回收站里 **N 个文件 + 1 个目录**，从属关系**断了** |
| **B. 整个目录一次丢**（选它） | 直接 `moveToTrash(boxPath)` | 回收站里 **1 个目录**，失败文件保持层级 |

**选 B 的理由**：
1. **还原体验完整** —— 失败的文件往往是一堆东西，保持层级才找得回来
2. **少一次出错机会** —— A 要遍历 N 次，每次都可能失败
3. **精准兜底** —— 成功还原的文件已经不在盒里了，所以这个目录里**只剩下还原失败的那些**，整体丢正是我们想要的

**⚠️ 空盒也要走回收站，不要改成 `QDir::rmdir`**：`rmdir` 是**永久删除**，与项目基调不一致。丢回收站成本极低，换来的是"万一还有隐藏文件没被扫到"的兜底。

### 5.3 业务逻辑放 `AppService`（推翻了原始分工建议）

**这是个真问题，两边理由都成立**：

**支持放 `AppService`**：它是「状态与通知的唯一入口」，且 `AppService` 本来就持有 `Settings`，清外观配置也顺手。

**反对**：`AppService` 现在只认识 `Collector`，加删除要引入 `BoxManager`、`QFile`、`QDir`，依赖面变宽。

**关键的第三个考虑（它决定了一切）**：`deleteBox` 需要构造 `MoveRecord` 调 `Collector::restore`，而那套**字段方向正是项目踩过坑的地方**。

- 若放 `BoxManager`，就得**重写一份字段方向的构造逻辑** —— 等于把踩过的坑再埋一颗雷
- 放 `AppService`，可以直接复用 `restorePathsQuiet`，字段方向**只有一份实现**

**最终判断：放 `AppService`。避免重复实现那个已知的坑，比保持类的职责纯粹更重要。**

### 5.4 确认对话框设计

**盒为空时** —— 普通确认框。

**盒非空时** —— 要求输入盒名（GitHub 删仓库那种）：

```
收纳盒「临时」里有 12 个项目。

删除后，这 12 个项目会被还原回桌面
（若桌面已有同名文件，会自动加序号，不会覆盖）。
还原不回去的会连同盒目录一起移入回收站，可以还原。

⚠️ 此操作不可撤销。

请输入盒名「临时」以确认：
```

**交互细节**：
1. 「删除」按钮**默认禁用**，输入与盒名**完全相等**（区分大小写）才启用
2. `setWindowModality(Qt::WindowModal)`
3. **默认焦点在「取消」上** —— 回车不该是危险操作
4. 盒名用 `%1` 拼，不写死

**为什么非空要输盒名**：盒非空时删除**要移动真实文件**，中途出问题用户看到的是"东西乱了"。手输盒名强制主人确认删对了盒 —— 防"删错盒"这个最贵的错误。

### 5.5 `moveFinished` 的重复弹框问题

**问题**：`restorePaths` 内部 emit `moveFinished(records, "还原到桌面")`，而 MainWindow 订阅了它 → 有失败项就弹框。于是删盒流程会**弹两次框**，其中一个标题还莫名其妙。

**处理方案：新增私有的"不发信号的还原"**：

```cpp
// core/appservice.h (private)
// 与 restorePaths 相同，但**不发任何信号**。
QList<MoveRecord> restorePathsQuiet(const QStringList &paths, const QString &targetDir);
```

`restorePaths` 改为薄包装（调 quiet + 发两个信号）。

**额外好处**：`restorePathsQuiet` 是"字段方向只写一份"的**唯一落点**，`restorePaths` 和 `deleteBox` 都复用它。

### 5.6 边界情况处理

| 情形 | 处理 |
|---|---|
| 盒为空 | 普通确认框；跳过 Step 1；直接丢空目录进回收站 |
| 盒正被浮窗显示 | 正常流程。**注意顺序**：先删文件再关浮窗 |
| 盒目录已被外部删除 | Step 0 检测到，**不算失败** |
| 盒名含特殊字符 | 配置键走 `encodeBoxName`；确认框用 `%1` 拼 |
| 还原目标不可写 | 全部进回收站兜底 → 汇报"0 项还原、N 项进回收站"。**不静默** |
| 盒内文件正被占用 | 该条 Failed → 进回收站。Windows 上占用中的文件通常**也进不了回收站**，此时整个目录 `moveToTrash` 会失败 → **整体失败**，提示关闭占用程序后重试 |
| 盒内有子目录 | 子目录作为**一个条目**还原，不用递归展开 |
| 删除过程中程序被杀 | **不引入事务**：已还原的在桌面、没还原的还在盒里，状态是自洽的 |

---

## 6. 改动清单（按风险从低到高）

### 功能 2（外观）—— 先做

| # | 改动 | 文件 | 风险 |
|---|---|---|---|
| 1 | `BoxAppearance` 结构体 | `core/coretypes.h` | 低 |
| 2 | 三个配置访问器 + `clearBoxAppearance` | `core/settings.h/.cpp` | 低 |
| 3 | `Options` 加字段 + `applyViewMode()` | `ui/itemlistwidget.h/.cpp` | 中 |
| 4 | 浮窗应用外观（透明度 + 尺寸） | `ui/floatingboxwidget.h/.cpp` | 中 |
| 5 | `applyAppearance`/`appearanceOf` + 信号 | `ui/floatingboxmanager.h/.cpp` | 中 |
| 6 | 外观对话框 | 新增 `ui/appearancedialog.h/.cpp` | 中 |
| 7 | 两个入口 | `floatingboxwidget.cpp`、`mainwindow.cpp` | 中 |
| 8 | 构建清单 | `.pro` + `CMakeLists.txt` | 低 |

### 功能 1（删除）

| # | 改动 | 文件 | 风险 |
|---|---|---|---|
| 9 | `restorePathsQuiet` 抽取 | `core/appservice.h/.cpp` | 中 |
| 10 | `deleteBox` + `BoxDeletionResult` | `core/appservice.h/.cpp` | 中 |
| 11 | 左栏右键菜单加「删除收纳盒…」 | `ui/mainwindow.cpp` | 中 |
| 12 | 确认对话框 | `ui/mainwindow.cpp` | 中 |
| 13 | 编排（关浮窗 + 清配置 + 刷新） | `ui/mainwindow.cpp` | **高** |

---

## 7. 构建清单（必须同步）

新增文件：`ui/appearancedialog.h` / `ui/appearancedialog.cpp`。

`DeskTidy.pro` 与 `CMakeLists.txt` **两处都要加**。

---

## 8. 测试策略

**能进 `tests/` 的**：`BoxAppearance` 纯函数、Settings 外观往返（含盒名含 `/`、默认值不落键）、`clearBoxAppearance`、`deleteBox` 的空盒/有文件/重名避让/盒目录不存在。

**⚠️ 回收站不进球测**：`moveToTrash` 会真的往主人系统回收站里丢东西，有副作用且无法自动清理。

**建议**：
- 单测里**只测到"还原"这一步**
- 回收站那一步用 `QFile::supportsMoveToTrash()` 做能力探测，不可用就 `QSKIP`
- 真正的回收站行为靠 `tools/trash_probe2.cpp` 人工跑

**扩展探针**：`e2e_probe.cpp` 加删盒用例；`e2e_floating.cpp` 加外观变更信号链路用例。

**只能人工验收的**：各视图模式的实际观感、卷起 + 改外观的组合、透明度预览手感、切大图标后尺寸是否合适、删除确认框的输入校验体验、占用中的文件所在目录能否进回收站。

---

## 9. 不确定的地方（诚实清单）

1. **`moveToTrash` 对"目录内有被占用文件"的行为** —— 未测。直接决定"整体失败"这条分支会不会被触发。
2. **`moveToTrash` 对网络驱动器 / U 盘** —— 未测。预期失败。
3. **`setWindowOpacity` 与 `Qt::FramelessWindowHint` 的组合** —— 需实机确认有无重绘残影。
4. **`IconMode` 下 `setGridSize` 与高 DPI 缩放的配合** —— 未验证。
5. **`setResizeMode(Adjust)` 在条目多时的重排性能** —— 未测。
6. **`QActionGroup` 与动态构造菜单的配合** —— 反复打开菜单可能泄漏，需实测。
7. **`resizeForAppearance()` 的"只放大不缩小"策略** —— 刻意取舍，实际用起来是否别扭只能靠真实使用感受判断。

---

## 10. 设计决定速查表

| 决定 | 选择 | 理由 |
|---|---|---|
| 删盒语义 | 先还原、失败的整体进回收站 | 绝不永久删除 |
| 回收站实现 | `QFile::moveToTrash` | 实测支持目录且递归 |
| 失败文件+目录怎么进回收站 | **整个目录一次丢** | 保持层级、少一次出错机会 |
| 删盒业务逻辑 | 放 `AppService` | 复用字段方向，避免重埋坑 |
| 删盒是否发 `moveFinished` | **不发**（新增 `restorePathsQuiet`） | 否则重复弹框且标题误导 |
| 删盒失败时后续步骤 | **不执行** | 否则状态分裂 |
| 确认强度 | 空盒普通确认；非空要输盒名 | 非空的要动真实文件 |
| 删除按钮默认状态 | **禁用**，输入匹配才启用 | 防手滑 |
| 默认焦点 | 「取消」 | 回车不该是危险操作 |
| 外观粒度 | **每盒一套** | 需求方明确要求 |
| 透明度存储 | int 0–100 | 避免 INI 里 double 的浮点误差 |
| 透明度实现 | `setWindowOpacity` | 样式表会被 `setDragHighlight` 清空 |
| 透明度防呆 | 三层下限（Settings 1% / apply 20% / UI 滑块 20%） | "浮窗看不见"不可恢复 |
| 视图模式 | `Options` 加字段（opt-in） | 编译期保证主窗口不受影响 |
| 尺寸自动调整 | **只放大、不缩小** | 不覆盖主人手动调的尺寸 |
| 尺寸调整与卷起 | 卷起时**直接返回** | 高度被 min/max 锁死 |
| 两个入口的共享 | manager + 信号中继 | 照抄 `boxWindowToggled` |
| 浮窗订阅方式 | 由 manager 在 `openBox` 里连接 | 不动浮窗构造签名，不引入反向依赖 |
| 外观对话框 | **新建独立类** | `onOpenSettings` 已臃肿；有 `PreviewDialog` 先例 |

---

## 11. 实施顺序与验证闸门

```
阶段 A：core 层（低风险，可单测）
  A1  BoxAppearance 结构体
  A2  Settings 访问器
  A3  restorePathsQuiet 抽取 + restorePaths 改包装   -> 全量回归【闸门】

阶段 B：列表控件与浮窗（中风险）
  B1  ItemListWidget::Options + applyViewMode()     -> 全量回归【闸门】
  B2  FloatingBoxWidget::applyAppearance + resizeForAppearance
  B3  FloatingBoxManager::applyAppearance + 信号

阶段 C：入口与对话框
  C1  AppearanceDialog
  C2  浮窗右键菜单子菜单
  C3  主窗口设置入口
  C4  构建清单同步
  ✅ 功能 2 完整可用

阶段 D：删除功能（高风险）
  D1  AppService::deleteBox                          -> 新增单测
  D2  e2e_probe 加删盒用例
  D3  左栏右键菜单入口
  D4  确认对话框
  D5  编排                                            -> 全量回归【闸门】
```

**回归闸门必须跑 `verify.bat`**：现有 78 项单测 + 29 项 core 端到端 + 18 项浮窗链路，一项都不许红。

---

## 附：本方案引用的关键源码位置

| 用途 | 位置 |
|---|---|
| `Collector::restore` 字段方向警告 | `core/collector.h:37-52` |
| 字段方向的实现证据 | `core/collector.cpp:230,236-253` |
| `restorePaths` 的字段构造 | `core/appservice.cpp:132-147` |
| `restorePaths` 写死的 actionLabel | `core/appservice.cpp:159-160` |
| 回收站实测探针 | `tools/trash_probe2.cpp` |
| 列表 opt-in 硬约束 | `ui/itemlistwidget.h:13-15` |
| 样式表被清空的坑 | `ui/itemlistwidget.cpp:270` |
| 卷起与 min/max 锁定 | `ui/floatingboxwidget.cpp:578-602` |
| `applySavedGeometry` 的顺序注意 | `ui/floatingboxwidget.cpp:739-744` |
| 每盒配置的模板 | `core/settings.cpp:198-221` |
| 盒名编码函数 | `core/settings.cpp:41-46` |
| 浮窗开关的中继模式 | `ui/floatingboxmanager.h:73` |
| 主窗口左栏右键菜单 | `ui/mainwindow.cpp:577-612` |

---

## 实施记录（方案落地后的实际情况）

> 本节在功能做完后补写，用于对照"计划"与"实际"。
> 上面的正文保持原样（那是决策时的思考），这里是结果。

### 各阶段完成情况

| 阶段 | 状态 | 验证方式 |
|---|---|---|
| A core 层 | ✅ | 编译 0 警告 + 回归闸门全绿 |
| B 列表与浮窗 | ✅ | 编译 0 警告 + 新增 9 项外观单测 |
| C 对话框与入口 | ✅ | 编译 0 警告 + 回归全绿 |
| D 删除入口 | ✅ | 编译 0 警告 + 新增 6 项删盒单测 |

### 测试规模变化

| 测试目标 | 之前 | 现在 |
|---|---|---|
| CoreNames 纯函数 | 21 | 21 |
| AppService（含删盒） | 17 | **23** |
| 浮窗逻辑（含外观） | 40 | **49** |
| core 端到端探针 | 29 | 29 |
| 浮窗信号链路探针 | 18 | 18 |
| **合计** | 125 | **140** |

### 实施中修正的、方案里没预料到的问题

1. **`deleteBox` 的单元测试会污染主人的真实回收站。**
   `deleteBox` 的最后一步是真把盒目录 `moveToTrash`，而 `QTemporaryDir` 收不回来
   （东西已经不在它目录下了）。跑一次就在回收站里留 5 个垃圾目录。

   **处理**：测试盒名统一加 `DeskTidyTest-` 前缀便于识别，并在 `verify.bat` 末尾
   加了一步自动清理（只清这个前缀的项）。测试里那段注释也改成了实话 ——
   原先写的"只断言到还原那一步"是不准确的。

2. **`QFileIconProvider` 给的图标是系统默认尺寸（≤32px），在 64px 大图标模式下会被拉伸成糊图。**
   这是阶段 B 的实施者自己发现并处理的：在 `setItems` 里加了一条
   "需要放大时向 `QIcon` 要足够大的 pixmap"的分支，并在切换视图后重建条目让它生效。

3. **`QMessageBox::setButtonText` 在 Qt6 已弃用**（`-Wdeprecated-declarations`）。
   删盒确认框改用 `addButton` + `clickedButton()`。本项目规矩是零警告，不能留。

4. **往 `QMessageBox` 内部布局塞输入框很脆。**
   非空盒的确认框最初打算用 `QMessageBox` + `layout()->addWidget(QLineEdit)`，
   但那依赖 Qt 的内部布局结构（按钮行与图标列的位置），换版本就可能错位。
   **改成自己搭 `QDialog`**，多十几行但每处位置都明确。

5. **`mainwindow.h` 需要完整包含 `appservice.h`。**
   `reportBoxDeletion` 的形参是 `AppService::BoxDeletionResult`（嵌套类型），
   前置声明不够用 —— 编译器必须看到完整的类定义。已改为 include。

### 与方案不同之处

- **外观对话框的预览**：方案里写"内嵌一个 `ItemListWidget` 填假条目"，实际一致。
  实施时额外加了 `WA_TransparentForMouseEvents` + `NoFocus` 屏蔽交互 ——
  不屏蔽的话主人能在预览里选中/双击/试着拖出条目，而那些信号本对话框一个都没接，
  表现是"点了没反应"像是坏了。
- **透明度不做视觉模拟**：预览嵌在对话框里，本身变淡反而看不清。只显示数字，
  真实效果点「应用」后在桌面浮窗上看。
- **「应用」按钮的反馈**：最初实现里禁用按钮后又被 `updateButtonsEnabled()` 启用回来，
  导致按钮亮着但字写着"已应用"。改用独立的 `m_justApplied` 标志。

### 仍未验证的项（诚实清单）

1. **`moveToTrash` 对"目录内有被占用文件"的行为** —— 未测。直接决定"整体失败"
   这条分支会不会被触发。Windows 各版本对回收站的处理不完全一致。
2. **`moveToTrash` 对网络驱动器 / U 盘** —— 未测。预期失败（不支持回收站）。
3. **`setWindowOpacity` 与 `Qt::FramelessWindowHint` 的组合** —— 未在本项目实测。
4. **外观对话框的实际观感**（480×520 是否合适、预览区高度够不够）。
5. **切到大图标后浮窗尺寸是否真的够放下 6 个图标** —— `cellW * 3` / `cellH * 2`
   是推算值，需要肉眼确认。
6. **`QActionGroup` 与动态构造菜单的配合** —— 反复打开浮窗右键菜单是否泄漏。
7. **多显示器拔插后几何恢复** —— 没有第二块屏幕可测。

