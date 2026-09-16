#include "coretypes.h"

#include <algorithm>

// ---------------------------------------------------------------------------
// coretypes 实现。
//
// 这里只放**依赖数据契约本身的纯计算**（BoxAppearance 的推导函数）。
// 之所以单独开一个 .cpp 而不是写进 coretypes.h 内联：
//   * coretypes.h 是"数据契约"，被几乎每个翻译单元包含，
//     塞进实现会拖慢全项目编译；
//   * 这几个函数是要被单测直接调的，放 .cpp 里才有稳定的符号可链接。
// ---------------------------------------------------------------------------

int BoxAppearance::defaultIconSizeFor(ViewMode mode)
{
    switch (mode) {
    case ViewMode::List:
        // 列表模式不用这个尺寸 —— 走的是 QListWidget 默认行高。
        // 返回一个合理值而不是 0，免得调用方拿到 0 去构造 QSize 出问题。
        return 16;
    case ViewMode::SmallIcon:
        return 16;
    case ViewMode::MediumIcon:
        // 与现有拖拽缩略图的 32×32 一致，作为视觉基准。
        return 32;
    case ViewMode::LargeIcon:
        return 64;
    }
    return 16;      // 编译器要求覆盖所有分支，这里只是兜底
}

int BoxAppearance::effectiveIconSize() const
{
    // 显式设过就用显式值。做成"可显式覆盖"而不是"只能从 viewMode 推"，
    // 是为了将来想支持自定义像素时不必改配置格式。
    if (iconSize > 0)
        return iconSize;

    return defaultIconSizeFor(viewMode);
}

bool BoxAppearance::isDefault() const
{
    return viewMode == ViewMode::List
           && iconSize == 0
           && opacity == 100
           && hoverEffect == HoverEffect::Glow
           && feedbackStrength == FeedbackStrength::Standard
           && animationSpeed == AnimationSpeed::Standard
           && hoverExpandDelayMs == 250
           && hoverCollapseDelayMs == 400
           && cornerRadius == 8
           && cornerSmoothing == 1;
}

namespace {

// 通用：把一个整数夹到某个预设数组里数值最接近的那一档。
// 距离相同（正好落在两档正中间）时取较小的一档 —— 保持确定性。
int nearestPreset(const int *presets, int count, int raw)
{
    int best = presets[0];
    int bestDist = std::abs(raw - best);
    for (int i = 1; i < count; ++i) {
        const int dist = std::abs(raw - presets[i]);
        if (dist < bestDist) {
            bestDist = dist;
            best = presets[i];
        }
    }
    return best;
}

// 通用：把一个整数夹到 [0, count-1]，用于修复非法枚举值。
template <typename Enum>
Enum clampEnum(int raw, int count)
{
    if (raw < 0)
        return static_cast<Enum>(0);
    if (raw >= count)
        return static_cast<Enum>(count - 1);
    return static_cast<Enum>(raw);
}

} // namespace

BoxAppearance::HoverEffect BoxAppearance::normalizeHoverEffect(int raw)
{
    // 只有 Off / Glow 两档，"最近的预设"就是往有效区间里夹。
    return clampEnum<HoverEffect>(raw, 2);
}

BoxAppearance::FeedbackStrength BoxAppearance::normalizeFeedbackStrength(int raw)
{
    return clampEnum<FeedbackStrength>(raw, 3);
}

BoxAppearance::AnimationSpeed BoxAppearance::normalizeAnimationSpeed(int raw)
{
    return clampEnum<AnimationSpeed>(raw, 3);
}

int BoxAppearance::normalizeHoverExpandDelayMs(int raw)
{
    return nearestPreset(kHoverExpandDelaysMs, 3, raw);
}

int BoxAppearance::normalizeHoverCollapseDelayMs(int raw)
{
    return nearestPreset(kHoverCollapseDelaysMs, 3, raw);
}

int BoxAppearance::normalizeCornerRadius(int raw)
{
    return nearestPreset(kCornerRadii, 4, raw);
}

int BoxAppearance::normalizeCornerSmoothing(int raw)
{
    return nearestPreset(kCornerSmoothings, 3, raw);
}

bool AppTheme::isDefault() const
{
    const AppTheme d;
    return windowBackground == d.windowBackground
           && surface == d.surface
           && titleBar == d.titleBar
           && text == d.text
           && mutedText == d.mutedText
           && border == d.border
           && hover == d.hover
           && pressed == d.pressed
           && primary == d.primary
           && primaryHover == d.primaryHover
           && primaryPressed == d.primaryPressed
           && onPrimary == d.onPrimary
           && danger == d.danger
           && dangerPressed == d.dangerPressed
           && windowOpacity == d.windowOpacity
           && floatDefaults.viewMode == d.floatDefaults.viewMode
           && floatDefaults.iconSize == d.floatDefaults.iconSize
           && floatDefaults.opacity == d.floatDefaults.opacity
           && floatDefaults.hoverEffect == d.floatDefaults.hoverEffect
           && floatDefaults.feedbackStrength == d.floatDefaults.feedbackStrength
           && floatDefaults.animationSpeed == d.floatDefaults.animationSpeed
           && floatDefaults.hoverExpandDelayMs == d.floatDefaults.hoverExpandDelayMs
           && floatDefaults.hoverCollapseDelayMs == d.floatDefaults.hoverCollapseDelayMs
           && floatDefaults.cornerRadius == d.floatDefaults.cornerRadius
           && floatDefaults.cornerSmoothing == d.floatDefaults.cornerSmoothing;
}

void AppTheme::normalize()
{
    const AppTheme d;
    const auto normalizedColor = [](const QColor &color, const QColor &fallback) {
        return color.isValid() ? color : fallback;
    };

    windowBackground = normalizedColor(windowBackground, d.windowBackground);
    surface          = normalizedColor(surface, d.surface);
    titleBar         = normalizedColor(titleBar, d.titleBar);
    text             = normalizedColor(text, d.text);
    mutedText        = normalizedColor(mutedText, d.mutedText);
    border           = normalizedColor(border, d.border);
    hover            = normalizedColor(hover, d.hover);
    pressed          = normalizedColor(pressed, d.pressed);
    primary          = normalizedColor(primary, d.primary);
    primaryHover     = normalizedColor(primaryHover, d.primaryHover);
    primaryPressed   = normalizedColor(primaryPressed, d.primaryPressed);
    onPrimary        = normalizedColor(onPrimary, d.onPrimary);
    danger           = normalizedColor(danger, d.danger);
    dangerPressed    = normalizedColor(dangerPressed, d.dangerPressed);
    windowOpacity    = std::max(40, std::min(windowOpacity, 100));

    BoxAppearance &a = floatDefaults;
    a.viewMode = static_cast<BoxAppearance::ViewMode>(
        std::max(0, std::min(static_cast<int>(a.viewMode), 3)));
    a.iconSize = std::max(0, std::min(a.iconSize, 512));
    a.opacity = std::max(BoxAppearance::kMinOpacity, std::min(a.opacity, 100));
    a.hoverEffect = BoxAppearance::normalizeHoverEffect(static_cast<int>(a.hoverEffect));
    a.feedbackStrength = BoxAppearance::normalizeFeedbackStrength(static_cast<int>(a.feedbackStrength));
    a.animationSpeed = BoxAppearance::normalizeAnimationSpeed(static_cast<int>(a.animationSpeed));
    a.hoverExpandDelayMs = BoxAppearance::normalizeHoverExpandDelayMs(a.hoverExpandDelayMs);
    a.hoverCollapseDelayMs = BoxAppearance::normalizeHoverCollapseDelayMs(a.hoverCollapseDelayMs);
    a.cornerRadius = BoxAppearance::normalizeCornerRadius(a.cornerRadius);
    a.cornerSmoothing = BoxAppearance::normalizeCornerSmoothing(a.cornerSmoothing);
}
