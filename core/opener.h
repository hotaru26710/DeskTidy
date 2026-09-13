#ifndef OPENER_H
#define OPENER_H

#include <QString>

// ---------------------------------------------------------------------------
// Opener —— 用系统默认关联打开一个文件/文件夹/快捷方式。
//
// 与 Collector 的职责边界（务必分清，不得混淆）：
//   * Collector —— **移动**文件。全应用唯一允许调用 QFile::rename/copy/remove 的地方。
//   * Opener    —— **打开**文件。只读操作，一个字节都不搬、不删、不改。
//
// 之所以单独成模块而不塞进 Collector：Collector 的定位注释写得很死
// （"唯一执行真实文件移动的地方"），把"打开"混进去会让职责边界糊掉，
// 日后有人看到 Collector 里有 openUrl 就会以为这里可以做别的事。
//
// ⚠️ 安全提示：openPath 会**真的运行 .exe**。
// 这是刻意的 —— 需求方明确选择"服从系统默认关联"，即双击一个 exe 就是要运行它。
// 调用方（浮窗）必须在 tooltip 里让主人看得出"双击 = 用系统默认方式打开"，
// 不能让主人在不知情的情况下启动了程序。
// ---------------------------------------------------------------------------

namespace Opener {

// 用系统默认关联打开 path。
//
// 成功返回 true；失败返回 false 并把**中文可读原因**写进 error（error 可为 nullptr）。
//
// 失败情形：
//   * 路径不存在（文件可能刚被主人手动移走/删除）；
//   * 系统没有为该类型注册任何关联程序（openUrl 返回 false）。
//
// 实现上刻意用 QDesktopServices::openUrl 而非 Win32 的 ShellExecuteW：
// 后者要引 windows.h，且返回值 HINSTANCE <= 32 表示失败（把"错误码"和
// "成功句柄"挤在同一个返回值里），语义反直觉、极易写错。本工具不需要
// 指定 verb / 工作目录 / SW_SHOW 这些精细控制，Qt 的封装刚好够用。
bool openPath(const QString &path, QString *error = nullptr);

} // namespace Opener

#endif // OPENER_H
