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
           && opacity == 100;
}
