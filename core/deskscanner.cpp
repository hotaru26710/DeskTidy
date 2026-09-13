#include "deskscanner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

#include "corenames.h"

// ---------------------------------------------------------------------------
// DeskScanner 实现。只读模块：不得出现任何写文件系统的调用。
// ---------------------------------------------------------------------------

namespace DeskScanner {

namespace {

// 本工具自身的名称。硬排除它，否则主人第一次点"收纳桌面"
// 就会把 DeskTidy 自己的快捷方式（甚至整个根目录）搬进收纳盒里。
const QString kSelfName = QStringLiteral("DeskTidy");

// 判断某个名字是否被主人列入排除清单（大小写不敏感）。
//
// ⚠️ 语义必须与原先逐条 compare(Qt::CaseInsensitive) 完全一致，所以键用的是
// toCaseFolded() 而不是 toLower() —— Qt 的 CaseInsensitive 比较内部走的就是
// 大小写折叠，两者对同一对字符串给出相同的相等结论（toLower 在个别字符上
// 与折叠并不等价，用它换掉会悄悄改变判定）。
//
// 改动的理由只是复杂度：原先对每个文件名都把整份排除清单线性扫一遍，
// 是"文件数 × 排除数"次字符串比较。折叠一次、装进 QSet 之后，
// 每个名字只剩一次折叠 + 一次哈希查找。
bool isNameExcluded(const QString &name, const QSet<QString> &excludedFolded)
{
    return excludedFolded.contains(name.toCaseFolded());
}

// 判断某个名字是否属于"本工具自身"，需要无条件排除。
// 直接以名字判定即可：快捷方式名为 "DeskTidy.lnk"，根目录名为 "DeskTidy"。
bool isSelfArtifact(const QString &name)
{
    if (name.compare(kSelfName, Qt::CaseInsensitive) == 0)
        return true;
    if (name.compare(kSelfName + QStringLiteral(".lnk"), Qt::CaseInsensitive) == 0)
        return true;
    // 兼容 "DeskTidy - 快捷方式.lnk" 这种被系统改过名的形态。
    return name.contains(kSelfName, Qt::CaseInsensitive)
           && name.endsWith(QStringLiteral(".lnk"), Qt::CaseInsensitive);
}

// 桌面自身的系统文件，由 Windows 维护，收走会破坏桌面视图配置。
// 这里带 Hidden 枚举正是为了看见它们 —— 看见了才能明确排除掉。
bool isDesktopSystemFile(const QString &name)
{
    return name.compare(QStringLiteral("desktop.ini"), Qt::CaseInsensitive) == 0;
}

// 扫描单个桌面目录，把合格条目追加进 out。
void scanOne(const QString &dirPath, EntryOrigin origin,
             const QSet<QString> &excludedFolded, const QString &boxRootPath,
             QList<DesktopEntry> *out)
{
    // 空路径必须先挡掉：QDir("") 会解析成"当前工作目录"，
    // 那样会把程序自己的运行目录当桌面扫，把编译产物之类的无关文件卷进来。
    // 正常流程不会传空（publicDesktopRoot 永远返回有效路径），
    // 但这是"绝不误伤"底线上不该存在的缝隙。
    if (dirPath.trimmed().isEmpty())
        return;

    // 目录不存在就静默跳过。公共桌面在不少机器上被组策略禁掉，
    // 那不是错误，不该弹窗打扰主人。
    QDir dir(dirPath);
    if (!dir.exists())
        return;

    const QFileInfoList children =
        dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                          QDir::NoSort);

    for (const QFileInfo &info : children) {
        const QString name = info.fileName();

        if (isSelfArtifact(name))
            continue;

        if (isDesktopSystemFile(name))
            continue;

        if (isNameExcluded(name, excludedFolded))
            continue;

        // 自我收纳防护：万一 DeskTidy 根目录被主人放到了桌面上，
        // 也不能把它当普通文件夹搬走（那会把已收纳的内容整体埋掉）。
        if (CoreNames::isPathInside(info.absoluteFilePath(), boxRootPath))
            continue;

        DesktopEntry entry;
        entry.filePath = QDir::cleanPath(info.absoluteFilePath());
        entry.name     = name;
        entry.isDir    = info.isDir();
        entry.size     = info.isDir() ? 0 : info.size();
        entry.modified = info.lastModified();
        entry.origin   = origin;

        out->append(entry);
    }
}

} // namespace

QList<DesktopEntry> scan(const QString &userDesktop,
                         const QString &publicDesktop,
                         const QStringList &excludedNames,
                         const QString &boxRootPath)
{
    QList<DesktopEntry> result;

    // 排除清单折叠一次，两个桌面目录共用 —— 原先每个文件名都要重扫一遍清单。
    QSet<QString> excludedFolded;
    excludedFolded.reserve(excludedNames.size());
    for (const QString &excluded : excludedNames) {
        excludedFolded.insert(excluded.toCaseFolded());
    }

    scanOne(userDesktop, EntryOrigin::UserDesktop,
            excludedFolded, boxRootPath, &result);
    scanOne(publicDesktop, EntryOrigin::PublicDesktop,
            excludedFolded, boxRootPath, &result);

    // UI 里是一张平铺列表，不排序会让主人每次看到的顺序都不一样。
    std::sort(result.begin(), result.end(),
              [](const DesktopEntry &lhs, const DesktopEntry &rhs) {
                  return QString::compare(lhs.name, rhs.name, Qt::CaseInsensitive) < 0;
              });

    return result;
}

} // namespace DeskScanner
