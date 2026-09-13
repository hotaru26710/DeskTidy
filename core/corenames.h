#ifndef CORENAMES_H
#define CORENAMES_H

#include <QString>

// ---------------------------------------------------------------------------
// CoreNames —— 纯路径/命名的"计算"层，本文件内**不做任何文件 IO**。
//
// 之所以把所有路径推导与改名规则抽成一个无副作用的命名空间，是为了让
// uniqueTargetPath / sanitizeBoxName / isPathInside 这三个最容易出隐蔽 Bug
// 的函数可以被单元测试直接调用，而不必真的去搬动主人的文件。
//
// 关键约定（来自需求确认）：
//   * 收纳根目录固定为 %USERPROFILE%\DeskTidy，不放桌面 —— 否则桌面整洁打对折；
//   * 重名一律"加序号"而非覆盖，序号插在扩展名之前：报告.docx -> 报告 (2).docx；
//   * 盒名要经 sanitizeBoxName 规范化后才能落到文件系统上。
// ---------------------------------------------------------------------------

namespace CoreNames {

// 当前用户桌面 %USERPROFILE%\Desktop。
// 取不到时回落到 home/Desktop，保证调用方永远拿到一个可用路径。
QString desktopRoot();

// 公共桌面 %PUBLIC%\Desktop。
// 桌面在视觉上是"两个目录合并显示"的，只收用户桌面会让主人觉得没收拾干净，
// 所以这里同样要返回公共桌面路径。环境变量缺失时按惯例退化到 C:\Users\Public\Desktop。
QString publicDesktopRoot();

// 收纳盒根目录 %USERPROFILE%\DeskTidy。
// 注意：本函数只做拼接，**不创建目录**（创建是 BoxManager::ensureRoot 的职责）。
QString boxRoot();

// 在 dir 下为 fileName 找一个"当前不存在"的目标全路径。
//
// 规则：
//   * dir/fileName 不存在  -> 原样返回 dir/fileName；
//   * 已存在              -> 插入序号，扩展名保持不动：
//                            报告.docx -> 报告 (2).docx -> 报告 (3).docx
//   * 无扩展名            -> 序号直接追加： 素材 -> 素材 (2)
//   * 名字以点开头(隐藏文件) -> 整个名字视为"主干"，点不算扩展名分隔符：
//                            .gitignore -> .gitignore (2)
//
// 这是本模块最重要的函数：它是"绝不覆盖任何已存在文件"这条底线的唯一保证，
// 因此**永不返回一个已存在的路径**。调用方无需再做存在性检查。
QString uniqueTargetPath(const QString &dir, const QString &fileName);

// 把用户输入的盒名规范化为合法 Windows 目录名：
//   * 去掉非法字符 \ / : * ? " < > |；
//   * 去掉首尾空白以及结尾的点（Windows 不允许目录名以点结尾）；
//   * 规避保留设备名（CON/PRN/AUX/NUL/COM1-9/LPT1-9），加下划线后缀；
//   * 结果为空则回退为 "收纳盒"。
QString sanitizeBoxName(const QString &raw);

// 判断 child 是否位于 parent 之内（child == parent 也算"之内"）。
// 用于防止"把收纳盒自己又收进收纳盒"这种自我吞噬。
// Windows 路径大小写不敏感、分隔符可能混用，故先 cleanPath 统一再比较；
// 比较时补上结尾分隔符，避免 C:\a 被误判为 C:\ab 的父目录。
bool isPathInside(const QString &child, const QString &parent);

} // namespace CoreNames

#endif // CORENAMES_H
