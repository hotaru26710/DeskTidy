#ifndef COLLECTOR_H
#define COLLECTOR_H

#include <QList>
#include <QString>

#include "coretypes.h"

// ---------------------------------------------------------------------------
// Collector —— 全应用**唯一执行真实文件移动**的地方。
//
// 与 FileManager 的 Executor 同一定位：除本文件外，任何模块（含 UI）
// 一律不得直接调用 QFile::rename / QFile::copy / QFile::remove。
//
// 安全底线：
//   * 目标路径一律由 CoreNames::uniqueTargetPath 求得，因此**永不覆盖**已存在文件；
//   * 单个条目失败绝不中断整批 —— 搬 100 个文件时第 37 个被占用，
//     不该让前 36 个白搬（回滚本身又要再移动一次，还可能再失败，得不偿失）；
//   * 跨卷(EXDEV)时 rename 必然失败，回落 copy + remove；
//     目录跨卷不做递归复制，直接记 Failed 并说明原因（递归复制半途失败会留下
//     难以收拾的残骸，风险远大于收益）。
// ---------------------------------------------------------------------------

namespace Collector {

// 把 entries 逐个收纳进 boxPath。
//
// 前置条件：boxPath 已存在（调用方负责 BoxManager::ensureRoot + createBox）。
// 返回与输入同序的结果清单，每条都填好
// sourcePath / finalPath / state / origin / size / isDir / time；
// Failed 与 Skipped 的条目在 error 中给出中文可读原因。
QList<MoveRecord> collect(const QList<DesktopEntry> &entries, const QString &boxPath);

// 撤销：把入参 records 中 state==Succeeded 的条目，从**盒内位置**移回**桌面原位**。
// 移回时同样经 uniqueTargetPath，避免覆盖主人已在桌面上重新建好的同名文件。
//
// ⚠️ 字段语义（极易搞反，务必看清）—— 入参与返回值**不是**同一套语义：
//
//   【入参】沿用与 collect() 完全相同的方向：
//       sourcePath = 桌面原位（这个文件本来该在哪）
//       finalPath  = 收纳盒内的当前位置（它现在在哪）
//     也就是说：还原时真正要动的是 finalPath 那个文件，落点是 sourcePath 的父目录。
//     若照"语义相反"去理解入参、把两个字段填反，会把文件搬向错误方向。
//
//   【返回】描述"这次还原发生了什么"，此时才与 collect() 相反：
//       sourcePath = 被撤销的收纳位置（盒内，文件从这儿被搬离）
//       finalPath  = 还原后的落点（桌面）
//     这样 reportMoveResult 取 sourcePath 的文件名展示时，
//     收纳与还原两种场景下拿到的都是"被搬离的那一侧"，主人认得出。
//
// 注意落点是从 sourcePath 的父目录推出来的；若入参给出的 sourcePath 不在任何
// 已知桌面目录下，仍会按它推父目录，调用方需自行保证语义正确。
QList<MoveRecord> restore(const QList<MoveRecord> &records);

} // namespace Collector

#endif // COLLECTOR_H
