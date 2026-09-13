#ifndef PREFERREDDROPEFFECT_H
#define PREFERREDDROPEFFECT_H

// ---------------------------------------------------------------------------
// ui/preferreddropeffect.h —— 让 Windows 把"从浮窗拖出条目"当成**移动**而非复制。
//
// 【为什么需要这个东西】
// 浮窗里的条目支持拖出到桌面/资源管理器，语义是"还原"（把文件搬走）。
// 但实测确认：直接把文件拖到资源管理器文件夹，Windows **默认按复制处理** ——
// 目标处出现副本，源文件仍留在收纳盒里（QDrag::exec() 返回 CopyAction）。
// 那样就变成"多了一份副本"，与主人期望的"还原"不是一回事。
//
// 【为什么默认是复制】
// Windows shell 依据剪贴板格式 CFSTR_PREFERREDDROPEFFECT 判定默认动作，
// 而 Qt 的 QMimeData 没有暴露设置它的接口。QWindowsMimeConverter 虽然是
// 私有头（文件名带 _P_H），但类本身是 Q_GUI_EXPORT 导出的公开符号，
// 于是可以自己实现一个转换器，在 Qt 把 QMimeData 翻译成 Windows
// IDataObject 时**额外挂上这个格式**，把 DROPEFFECT_MOVE 塞进去。
//
// 【实测结论】（tools/drag_probe2.cpp 是可运行的验证样例）
//   不注入：exec() 返回 CopyAction=1，源文件仍在，目标处出现副本。
//   注入后：exec() 返回 MoveAction=2，源文件消失，文件确实落在目标文件夹。
//
// 【⚠️ 硬约束：本方案成立的唯一前提是"由资源管理器完成实际移动"】
// 我们只是告诉了 shell"这是一次移动操作"，真正搬文件的仍然是 Windows。
// 由此推出两条必须由调用方遵守的规矩：
//
//   1) 拖到**不接受文件拖放**的目标（某些程序、浏览器空白区等）会失败 ——
//      对方不会执行移动，而我们的标记又让系统以为"会有人处理"，结果是
//      文件既不在源、也不在目标、也不在回收站，**凭空消失**。
//      实测中已经出现过这一现象。
//      因此调用方必须在 exec() 返回后 **检查源文件是否仍然存在**：
//      源没了 = 移动成功；源还在 = 对方没接手，必须提示用户文件仍在盒里。
//
//   2) exec() 返回后**绝不自己去动源文件** —— 资源管理器已经搬走了，
//      再动就是操作一个不存在的路径。
//
// 这两条兜底逻辑写在 ItemListWidget::startDrag 与
// FloatingBoxWidget 对 dragOutFinished 的处理里，不要在别处另起炉灶。
//
// 【平台】
// 只有 Windows 需要这套机制（其他平台的拖拽默认就是移动语义）。
// 非 Windows 平台本模块是空实现，保证代码可移植。
// ---------------------------------------------------------------------------

namespace PreferredDropEffect {

// 把转换器注册到 Qt 的 Windows 拖放系统。**程序启动时调用一次即可**。
//
// 重复调用是安全的（内部用函数内 static 保证只注册一次）。
// 非 Windows 平台上本函数什么也不做。
void ensureRegistered();

} // namespace PreferredDropEffect

#endif // PREFERREDDROPEFFECT_H
