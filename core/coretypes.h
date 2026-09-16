#ifndef CORETYPES_H
#define CORETYPES_H

#include <QColor>
#include <QString>
#include <QDateTime>

// ---------------------------------------------------------------------------
// coretypes.h —— DeskTidy 共享数据契约（唯一权威）。
//
// 扫描器 / 盒子管理 / 收纳执行 / 撤销栈 / UI 全部只认这里定义的类型，
// 禁止各模块各自重造同名结构。
//
// 设计底线（来自需求确认）：
//   * 本工具绝不删除任何文件，也不覆盖任何已存在文件；
//   * 所谓"收纳"= 把文件从桌面真实移动到 %USERPROFILE%\DeskTidy\<盒名>\；
//   * 冲突一律自动改名加序号，并留下日志。
// ---------------------------------------------------------------------------

// 单个桌面条目的来源位置，决定还原时的去向。
enum class EntryOrigin
{
    UserDesktop,    // %USERPROFILE%\Desktop
    PublicDesktop   // %PUBLIC%\Desktop（写入失败不中断，只记失败）
};

// 桌面上的一个条目（文件 / 目录 / 快捷方式）。
struct DesktopEntry
{
    QString     filePath;   // 完整路径
    QString     name;       // 文件名（含扩展名）
    bool        isDir    = false;
    qint64      size     = 0;       // 目录为 0
    QDateTime   modified;
    EntryOrigin origin   = EntryOrigin::UserDesktop;
};

// 一个收纳盒：就是 %USERPROFILE%\DeskTidy\ 下的一个子目录。
// 注意：盒子列表不落配置，每次靠扫描根目录重建，文件系统即真相。
struct StorageBox
{
    QString name;       // 盒名（即目录名）
    QString path;       // 盒目录全路径
    int     itemCount = 0;
};

// 收纳动作的执行结果状态。
enum class MoveState
{
    Succeeded,  // 移动成功（可能已因重名改名）
    Skipped,    // 未执行（源已不存在等）
    Failed      // 执行失败，error 记录中文可读原因
};

// 一次收纳中，单个条目的处理记录。撤销栈按此反向还原。
struct MoveRecord
{
    QString     sourcePath;     // 原位置（桌面上的全路径）
    QString     finalPath;      // 收纳后的实际全路径（重名时已加序号）
    MoveState   state = MoveState::Succeeded;
    QString     error;          // Failed 时的中文可读原因
    EntryOrigin origin = EntryOrigin::UserDesktop;
    qint64      size = 0;       // 目录为 0
    bool        isDir = false;
    QDateTime   time;
};

// ---------------------------------------------------------------------------
// 一个浮窗的外观设置。**每盒一份**，存于 Settings。
//
// 之所以做成结构体而不是散着几个字段：它要在「设置对话框 -> Settings ->
// 浮窗」之间整体传递，散着传很容易漏传一项，而表现是"改了没反应"这种
// 最难查的问题。
// ---------------------------------------------------------------------------
struct BoxAppearance
{
    // 视图模式。
    //
    // ⚠️ 数值**刻意不与 QListView::ViewMode 绑定**：那是 Qt 的实现细节，
    // 而存进配置的应该是本工具自己的语义。将来若换一种渲染方式
    // （比如自绘图网格），存量配置的含义不会跟着漂移，只需改映射函数。
    enum class ViewMode
    {
        List = 0,       // 列表（默认，与改造前行为一致）
        SmallIcon = 1,  // 小图标网格
        MediumIcon = 2, // 中图标网格
        LargeIcon = 3   // 大图标网格
    };

    // 悬停触感：鼠标进入浮窗时的光影反馈。
    //
    // 只用浮窗内部的透明覆盖层 + 单个 QVariantAnimation 实现，
    // 不改变窗口尺寸 / 位置 / 透明度，因此不会干扰浮窗推动逻辑。
    enum class HoverEffect
    {
        Off  = 0,   // 关闭触感
        Glow = 1    // 光影描边（默认）
    };

    // 触感反馈强度：决定描边 / 高光的 alpha 与线宽。
    enum class FeedbackStrength
    {
        Subtle   = 0,   // 轻微
        Standard = 1,   // 标准（默认）
        Strong   = 2    // 明显
    };

    // 动画速度档位：统一驱动触感 / 卷起展开 / 让位三类动画。
    // 注意：淡入淡出（透明度）固定 180ms，不受本档位影响。
    enum class AnimationSpeed
    {
        Relaxed  = 0,   // 舒缓
        Standard = 1,   // 标准（默认）
        Fast     = 2    // 快速
    };

    ViewMode viewMode = ViewMode::List;
    int      iconSize = 0;      // 0 = 跟随 viewMode 推导；>0 为显式像素
    int      opacity  = 100;    // 百分比 1..100（100 = 完全不透明）

    HoverEffect     hoverEffect     = HoverEffect::Glow;
    FeedbackStrength feedbackStrength = FeedbackStrength::Standard;
    AnimationSpeed   animationSpeed   = AnimationSpeed::Standard;
    int hoverExpandDelayMs   = 250;
    int hoverCollapseDelayMs = 400;
    int cornerRadius         = 8;
    int cornerSmoothing      = 1;   // 0=锐利 1=标准 2=更平滑

    // 由 viewMode 推导的图标像素（iconSize 为 0 时使用）。
    static int defaultIconSizeFor(ViewMode mode);

    // 实际生效的图标像素：iconSize > 0 用显式值，否则跟随 viewMode。
    int effectiveIconSize() const;

    // 是否等于默认外观。
    // 用途有二：配置层据此决定"要不要落键"（少一个键少一份失真可能），
    // UI 层据此决定"恢复默认"按钮的可用性。
    bool isDefault() const;

    // 透明度下限。低于此值浮窗会淡到几乎看不见，
    // 而"主人找不到自己的浮窗"是不可恢复的故障，故设一道硬闸。
    static constexpr int kMinOpacity = 20;

    // ---- 安全预设 ----------------------------------------------------------
    // 界面只暴露这几档，配置层的非法 / 越界值一律归一化到最近的预设，
    // 这样手工改配置文件也不会把浮窗搞成不可用的样子。
    inline static constexpr int kHoverExpandDelaysMs[3]   = { 150, 250, 400 };
    inline static constexpr int kHoverCollapseDelaysMs[3] = { 250, 400, 600 };
    inline static constexpr int kCornerRadii[4]           = { 4, 8, 12, 16 };
    inline static constexpr int kCornerSmoothings[3]      = { 0, 1, 2 };

    static HoverEffect      normalizeHoverEffect(int raw);
    static FeedbackStrength normalizeFeedbackStrength(int raw);
    static AnimationSpeed   normalizeAnimationSpeed(int raw);
    static int              normalizeHoverExpandDelayMs(int raw);
    static int              normalizeHoverCollapseDelayMs(int raw);
    static int              normalizeCornerRadius(int raw);
    static int              normalizeCornerSmoothing(int raw);
};

// ---------------------------------------------------------------------------
// 全局主题：中控窗口与所有浮窗默认外观的唯一来源。
//
// 单个浮窗若没有自己的覆盖配置，就完整使用 floatDefaults；若主人从浮窗
// 右键保存过外观，则该盒保留一份完整覆盖，不被后续全局主题改动覆盖。
// ---------------------------------------------------------------------------
struct AppTheme
{
    QColor windowBackground = QColor(248, 249, 250);
    QColor surface          = QColor(255, 255, 255);
    QColor titleBar         = QColor(241, 243, 244);
    QColor text             = QColor(32, 33, 36);
    QColor mutedText        = QColor(95, 99, 104);
    QColor border           = QColor(218, 220, 224);
    QColor hover            = QColor(241, 243, 244);
    QColor pressed          = QColor(232, 234, 237);
    QColor primary          = QColor(26, 115, 232);
    QColor primaryHover     = QColor(23, 101, 204);
    QColor primaryPressed   = QColor(20, 83, 159);
    QColor onPrimary        = QColor(255, 255, 255);
    QColor danger           = QColor(232, 17, 35);
    QColor dangerPressed    = QColor(197, 15, 31);

    // 中控主窗口的整窗透明度，百分比 40..100（100 = 完全不透明）。
    // 设下限是为了避免主人把控制中心调到几乎看不见而无法恢复。
    int windowOpacity = 100;

    BoxAppearance floatDefaults;

    bool isDefault() const;
    void normalize();
};

#endif // CORETYPES_H
