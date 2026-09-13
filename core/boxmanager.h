#ifndef BOXMANAGER_H
#define BOXMANAGER_H

#include <QList>
#include <QString>

#include "coretypes.h"

// ---------------------------------------------------------------------------
// BoxManager —— 收纳盒（= 根目录下的真实子目录）的扫描与创建。
//
// 核心约定：**盒子列表不落配置**，每次由 listBoxes 扫描根目录重建。
// 这样主人在资源管理器里手工建/删/改名一个盒子，控制中心刷新一下就能看到，
// 永远不会出现"配置说有、磁盘上没有"的失真。
//
// 本模块只做目录层面的增删查，不搬动任何桌面文件（那是 Collector 的事）。
// ---------------------------------------------------------------------------

namespace BoxManager {

// 扫描 root 下的一级子目录，作为收纳盒清单返回。
// root 不存在时返回**空列表且不创建它** —— 让"还没收纳过"保持为无副作用状态。
QList<StorageBox> listBoxes(const QString &root);

// 确保收纳根目录存在（QDir::mkpath）。失败时把中文可读原因写进 error。
bool ensureRoot(const QString &root, QString *error);

// 在 root 下创建（或复用）名为 rawName 的盒子。
// rawName 先经 CoreNames::sanitizeBoxName 规范化；同名目录**已存在不算错误**，
// 直接把该目录作为盒子返回（幂等），省掉 UI 侧的预先判重。
// out 非空时填入盒子信息（name/path/itemCount）。
bool createBox(const QString &root, const QString &rawName,
               StorageBox *out, QString *error);

// 列出某个盒子内的直接子项，供 UI 展示"这个盒子里装了什么"。
// 复用 DesktopEntry 结构，其中 origin 在本场景无意义，统一填 UserDesktop。
// 返回按名称排序后的清单；boxPath 不存在时返回空列表。
QList<DesktopEntry> listBoxItems(const QString &boxPath);

} // namespace BoxManager

#endif // BOXMANAGER_H
