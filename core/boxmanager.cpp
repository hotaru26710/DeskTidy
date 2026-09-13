#include "boxmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <algorithm>

#include "corenames.h"

// ---------------------------------------------------------------------------
// BoxManager 实现。
//
// 只操作 %USERPROFILE%\DeskTidy\ 这一层目录结构，不搬动任何桌面文件。
// 唯一容易被忽略的约定：listBoxes 对不存在的根目录**不创建**，
// 保持"还没收纳过"为无副作用状态。
// ---------------------------------------------------------------------------

namespace BoxManager {

namespace {

// 统计一个目录的直接子项数量。取不到时返回 0（比抛错更符合 UI 期待）。
int countChildren(const QString &dirPath)
{
    QDir dir(dirPath);
    if (!dir.exists())
        return 0;
    return dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot,
                         QDir::NoSort).size();
}

// 由完整路径填出 DesktopEntry，供 listBoxItems 复用。
DesktopEntry makeEntry(const QFileInfo &info)
{
    DesktopEntry entry;
    entry.filePath = QDir::cleanPath(info.absoluteFilePath());
    entry.name     = info.fileName();
    entry.isDir    = info.isDir();
    entry.size     = info.isDir() ? 0 : info.size();
    entry.modified = info.lastModified();
    // 盒子内的条目没有"桌面来源"概念，统一填 UserDesktop 以满足结构要求。
    entry.origin   = EntryOrigin::UserDesktop;
    return entry;
}

} // namespace

QList<StorageBox> listBoxes(const QString &root)
{
    QList<StorageBox> boxes;

    QDir rootDir(root);
    // 根目录不存在意味着主人从未收纳过 —— 这不是错误，也绝不代为创建。
    if (!rootDir.exists())
        return boxes;

    const QFileInfoList children =
        rootDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);

    boxes.reserve(children.size());
    for (const QFileInfo &info : children) {
        StorageBox box;
        box.name      = info.fileName();
        box.path      = QDir::cleanPath(info.absoluteFilePath());
        box.itemCount = countChildren(box.path);
        boxes.append(box);
    }

    std::sort(boxes.begin(), boxes.end(),
              [](const StorageBox &lhs, const StorageBox &rhs) {
                  return QString::compare(lhs.name, rhs.name, Qt::CaseInsensitive) < 0;
              });

    return boxes;
}

bool ensureRoot(const QString &root, QString *error)
{
    if (root.isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("BoxManager", "收纳根目录路径为空。");
        return false;
    }

    QDir dir;
    if (dir.mkpath(root))
        return true;

    if (error) {
        *error = QCoreApplication::translate(
                     "BoxManager",
                     "无法创建收纳根目录：%1。请检查磁盘空间与写入权限。")
                     .arg(QDir::toNativeSeparators(root));
    }
    return false;
}

bool createBox(const QString &root, const QString &rawName,
               StorageBox *out, QString *error)
{
    const QString name = CoreNames::sanitizeBoxName(rawName);
    const QString path = QDir::cleanPath(root + QLatin1Char('/') + name);

    QDir rootDir(root);
    if (!rootDir.exists()) {
        // 根目录缺失时一并补建，省掉调用方额外的 ensureRoot 步骤。
        if (!ensureRoot(root, error))
            return false;
    }

    const QFileInfo info(path);
    if (info.exists()) {
        // 同名已存在：不做错误处理，直接把既有目录当盒子返回。
        // 这样 UI 侧"输入盒名 -> 开始收纳"这条路径天然幂等，不必预先判重。
        if (!info.isDir()) {
            if (error) {
                *error = QCoreApplication::translate(
                             "BoxManager",
                             "同名文件已存在且不是文件夹：%1。")
                             .arg(name);
            }
            return false;
        }
    } else if (!rootDir.mkpath(name)) {
        if (error) {
            *error = QCoreApplication::translate(
                         "BoxManager",
                         "无法创建收纳盒「%1」。请检查磁盘空间与写入权限。")
                         .arg(name);
        }
        return false;
    }

    if (out) {
        out->name      = name;
        out->path      = path;
        out->itemCount = countChildren(path);
    }
    return true;
}

QList<DesktopEntry> listBoxItems(const QString &boxPath)
{
    QList<DesktopEntry> items;

    QDir dir(boxPath);
    if (!dir.exists())
        return items;

    const QFileInfoList children =
        dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                          QDir::NoSort);

    items.reserve(children.size());
    for (const QFileInfo &info : children)
        items.append(makeEntry(info));

    std::sort(items.begin(), items.end(),
              [](const DesktopEntry &lhs, const DesktopEntry &rhs) {
                  return QString::compare(lhs.name, rhs.name, Qt::CaseInsensitive) < 0;
              });

    return items;
}

} // namespace BoxManager
