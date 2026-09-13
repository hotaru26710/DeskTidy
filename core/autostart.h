#ifndef AUTOSTART_H
#define AUTOSTART_H

// ---------------------------------------------------------------------------
// AutoStart —— Windows 开机自启开关（当前用户 HKCU\...\Run）。
//
// 【职责】
//   只负责读写"当前用户登录时启动 DeskTidy"这一个系统设置，UI 不直接碰
//   注册表。这样以后若改成启动文件夹快捷方式或加安装包逻辑，只需要动这里。
//
// 【为什么不存进 Settings 的 INI】
//   注册表里那个值本身就是开关的唯一真相。再在 ini 里存一份 bool，一旦
//   用户在任务管理器里禁用启动项，两边就会打架，界面显示的也不是实际状态。
//
// 【平台】
//   Windows 上读写 HKCU Run 键；其他平台 isSupported() 返回 false，
//   调用方应把选项置灰，setEnabled() 也只返回 false，不做假动作。
// ---------------------------------------------------------------------------

#include <QString>

class AutoStart
{
public:
    // 当前平台是否支持本类管理的开机自启方式。
    static bool isSupported();

    // 当前程序是否已登记为开机自启。
    //
    // 判断标准不是"注册表里有键"，而是"键指向当前这个可执行文件"。
    // 开发时从构建目录勾选、之后又移动到正式目录时，旧路径不会让新程序
    // 误显示为已启用；重新勾选一次即可覆盖为当前位置。
    static bool isEnabled();

    // 当前登记的启动命令是否带静默参数。只在 isEnabled() 为 true 时有意义。
    static bool isSilent();

    // 命令行参数：带它启动时不显示控制中心，只恢复浮窗并驻留托盘。
    // 单独暴露出来，main.cpp 解析与注册表写入使用同一个字符串，避免拼错。
    static QString silentStartArgument();

    // 打开/关闭开机自启。silent=true 时写入静默启动参数。
    // 返回 false 表示写注册表失败。
    static bool setEnabled(bool enabled, bool silent = false);
};

#endif // AUTOSTART_H
