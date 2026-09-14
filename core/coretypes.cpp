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
           && cornerRadius == 8;
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
