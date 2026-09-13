#include "opener.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QUrl>

// ---------------------------------------------------------------------------
// Opener 实现。只读模块：不得出现任何写文件系统的调用。
// ---------------------------------------------------------------------------

namespace Opener {

bool openPath(const QString &path, QString *error)
{
    // 先确认存在。openUrl 对不存在的路径在某些关联下会静默失败，
    // 与其让主人对着"点了没反应"发呆，不如直接给一句人话。
    const QFileInfo info(path);
    if (path.isEmpty() || !info.exists()) {
        if (error) {
            *error = QCoreApplication::translate(
                "Opener", "文件已不存在，可能已被移动或删除。");
        }
        return false;
    }

    // fromLocalFile 处理中文路径与空格；跨盘符也是全路径，无需特殊照顾。
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(info.absoluteFilePath()))) {
        if (error) {
            *error = QCoreApplication::translate(
                         "Opener",
                         "系统没有为「%1」注册可用的打开方式。")
                         .arg(info.fileName());
        }
        return false;
    }

    return true;
}

} // namespace Opener
