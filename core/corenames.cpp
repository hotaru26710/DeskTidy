#include "corenames.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

// ---------------------------------------------------------------------------
// CoreNames 实现。全文件**不得**出现任何创建/删除/移动文件系统的调用：
// 这里的函数之所以要能被单测直接调用，前提就是它们没有副作用。
// ---------------------------------------------------------------------------

namespace CoreNames {

namespace {

// Windows 文件名保留字符。冒号在 NTFS 上还兼作数据流分隔符，一并剔除。
// 用 QString（而非 QLatin1String）承载，才能直接用 contains(QChar) 判定。
const QString kIllegalChars = QStringLiteral("\\/:*?\"<>|");

// Windows 保留设备名。即便带扩展名（CON.txt）在 NTFS 上同样非法，
// 所以这里按"主干名"比较。
bool isReservedDeviceName(const QString &baseName)
{
    static const QStringList reserved = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9")
    };
    return reserved.contains(baseName.toUpper());
}

// 把 dir 与 name 拼成全路径并做 cleanPath 归一（消除 . / .. 与重复分隔符）。
QString joinPath(const QString &dir, const QString &name)
{
    if (dir.isEmpty())
        return QDir::cleanPath(name);
    return QDir::cleanPath(dir + QLatin1Char('/') + name);
}

} // namespace

QString desktopRoot()
{
    const QString viaShell = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (!viaShell.isEmpty())
        return QDir::cleanPath(viaShell);

    // 兜底：某些精简/服务账号环境下 DesktopLocation 会返回空。
    return QDir::cleanPath(QDir::homePath() + QStringLiteral("/Desktop"));
}

QString publicDesktopRoot()
{
    // 公共桌面并不在 QStandardPaths 的枚举里（它是机器级而非用户级），
    // 只能读环境变量。PUBLIC 缺失时按 Windows 惯例退化到 C:\Users\Public\Desktop；
    // 该路径通常不存在，扫描器会静默跳过，不会造成误伤。
    QString publicDir = qEnvironmentVariable("PUBLIC");
    if (publicDir.isEmpty())
        publicDir = QStringLiteral("C:/Users/Public");

    return QDir::cleanPath(publicDir + QStringLiteral("/Desktop"));
}

QString boxRoot()
{
    return QDir::cleanPath(QDir::homePath() + QStringLiteral("/DeskTidy"));
}

QString uniqueTargetPath(const QString &dir, const QString &fileName)
{
    // 空文件名无从谈起，原样返回交由调用方发现异常。
    if (fileName.isEmpty())
        return joinPath(dir, fileName);

    const QString first = joinPath(dir, fileName);
    if (!QFileInfo::exists(first))
        return first;

    // 拆出主干与扩展名。隐藏文件（.gitignore）整个名字都是主干，
    // 否则会被误判成"主干部为空、扩展名为 gitignore"，
    // 从而生成 ". (2)gitignore" 这种畸形名字。
    QString stem = fileName;
    QString suffix;     // 含前导点，如 ".docx"；无扩展名时为空

    const int dot = fileName.lastIndexOf(QLatin1Char('.'));
    if (dot > 0 && dot < fileName.size() - 1) {
        stem   = fileName.left(dot);
        suffix = fileName.mid(dot);
    }

    // 从 2 开始递增探测。理论上必然终止，但加一个上限防御：
    // 若目录里真堆了上万个同名文件，宁可返回一个可能冲突的路径交给
    // rename 失败处理（记 Failed），也不能让 UI 无限卡住。
    constexpr int kMaxProbe = 10000;
    for (int index = 2; index <= kMaxProbe; ++index) {
        const QString candidate = QStringLiteral("%1 (%2)%3").arg(stem).arg(index).arg(suffix);
        const QString full = joinPath(dir, candidate);
        if (!QFileInfo::exists(full))
            return full;
    }

    return first;
}

QString sanitizeBoxName(const QString &raw)
{
    QString name = raw;

    // 逐个剔除非法字符。用 QChar 遍历以正确处理中文与代理对。
    QString cleaned;
    cleaned.reserve(name.size());
    for (const QChar &ch : name) {
        if (kIllegalChars.contains(ch))
            continue;
        cleaned.append(ch);
    }
    name = cleaned;

    // 去掉首尾空白；再去掉结尾的点 —— Windows 会静默丢弃目录名末尾的点，
    // 留着会导致"配置里的名字"与"磁盘上的名字"不一致。
    name = name.trimmed();
    while (name.endsWith(QLatin1Char('.')))
        name.chop(1);
    name = name.trimmed();

    if (name.isEmpty())
        name = QCoreApplication::translate("CoreNames", "收纳盒");

    // 保留设备名规避：CON -> CON_，避免创建目录时被系统拒绝。
    if (isReservedDeviceName(name))
        name += QLatin1Char('_');

    return name;
}

bool isPathInside(const QString &child, const QString &parent)
{
    if (child.isEmpty() || parent.isEmpty())
        return false;

    QString c = QDir::cleanPath(child);
    QString p = QDir::cleanPath(parent);

    // 统一分隔符，避免 C:/a 与 C:\a 比不相等。
    c.replace(QLatin1Char('\\'), QLatin1Char('/'));
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));

    // 去掉结尾斜杠，让补分隔符的逻辑有唯一形态。
    while (c.endsWith(QLatin1Char('/')) && c.size() > 1)
        c.chop(1);
    while (p.endsWith(QLatin1Char('/')) && p.size() > 1)
        p.chop(1);

    if (c.compare(p, Qt::CaseInsensitive) == 0)
        return true;    // 相等也算"之内"

    // 补上结尾分隔符再比前缀，否则 C:/a 会被误判为 C:/ab 的父目录。
    if (!p.endsWith(QLatin1Char('/')))
        p += QLatin1Char('/');

    return c.startsWith(p, Qt::CaseInsensitive);
}

} // namespace CoreNames
