#ifndef DESKSCANNER_H
#define DESKSCANNER_H

#include <QList>
#include <QString>
#include <QStringList>

#include "coretypes.h"

// ---------------------------------------------------------------------------
// DeskScanner —— 只读的"桌面上有什么"枚举器。
//
// 本模块**只读不写**：它的输出是给 UI 预览和执行器消费的候选清单，
// 真正的移动一律由 Collector 独占完成。
//
// 过滤规则（缺一不可，否则第一次使用就会翻车）：
//   * 只扫直接子项，不递归 —— 桌面下的大文件夹不该被整锅端走；
//   * 名称命中 excludedNames（大小写不敏感）的跳过；
//   * 路径落在 boxRootPath 之内的跳过 —— 防止把收纳盒自己收进收纳盒；
//   * 硬排除名为 "DeskTidy" 的项，以及任何 "DeskTidy.lnk" 快捷方式，
//     否则主人第一次点"收纳桌面"就会把本工具自己搬走。
// ---------------------------------------------------------------------------

namespace DeskScanner {

// 扫描两个桌面目录的直接子项。
// userDesktop / publicDesktop 任一不存在时**静默跳过**（公共桌面在部分机器上被策略禁用），
// 不报错、不创建目录。
// boxRootPath 用于自我收纳防护；excludedNames 为用户自定义排除项。
// 返回按名称排序后的条目清单。
QList<DesktopEntry> scan(const QString &userDesktop,
                         const QString &publicDesktop,
                         const QStringList &excludedNames,
                         const QString &boxRootPath);

} // namespace DeskScanner

#endif // DESKSCANNER_H
