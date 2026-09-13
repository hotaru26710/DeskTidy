#include "autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QString>

#ifdef Q_OS_WIN
#include <QSettings>
#endif

#ifdef Q_OS_WIN
namespace {

const char kRunKey[] =
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
// 任务管理器里的"启动"页会在下面这个键里放一个 12 字节的启用/禁用标记。
// 只写 Run 键而不管它，曾经被系统禁用过的程序就会继续不启动。
const char kStartupApprovedRunKey[] =
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
const char kValueName[] = "DeskTidy";

// 写注册表时始终给路径加引号，避免 "C:\Program Files\..." 这种带空格
// 的安装目录被 Windows 拆成多个参数。
QString currentAppCommand(bool silent)
{
    const QString appPath = QCoreApplication::applicationFilePath();
    if (appPath.isEmpty()) {
        return QString();
    }

    QString command = QLatin1Char('"') + QDir::toNativeSeparators(appPath) + QLatin1Char('"');
    if (silent) {
        command += QLatin1Char(' ') + AutoStart::silentStartArgument();
    }
    return command;
}

// 从 Run 值里取出可执行文件路径。标准写法是带引号的完整路径；
// 这里也兼容主人手工写过的无引号短路径，但带空格的无引号路径本身有歧义，
// 本程序不会写出那种值。
QString commandExecutable(const QString &command)
{
    const QString text = command.trimmed();
    if (text.isEmpty()) {
        return QString();
    }

    if (text.startsWith(QLatin1Char('"'))) {
        const int end = text.indexOf(QLatin1Char('"'), 1);
        return end > 1 ? text.mid(1, end - 1) : QString();
    }

    const int space = text.indexOf(QLatin1Char(' '));
    return space >= 0 ? text.left(space) : text;
}

bool samePath(const QString &a, const QString &b)
{
    const QString cleanA = QDir::cleanPath(QDir::fromNativeSeparators(a.trimmed()));
    const QString cleanB = QDir::cleanPath(QDir::fromNativeSeparators(b.trimmed()));
    if (cleanA.isEmpty() || cleanB.isEmpty()) {
        return false;
    }
    return cleanA.compare(cleanB, Qt::CaseInsensitive) == 0;
}

// StartupApproved\Run 的值是 REG_BINARY：常见格式首字节 0x02 = 启用，
// 0x03 = 禁用。不存在该值时 Windows 按"启用"处理。
bool disabledByStartupApproved()
{
    QSettings approved(QString::fromLatin1(kStartupApprovedRunKey),
                       QSettings::NativeFormat);
    const QVariant value = approved.value(QString::fromLatin1(kValueName));
    if (!value.isValid()) {
        return false;
    }

    const QByteArray bytes = value.toByteArray();
    return !bytes.isEmpty()
        && static_cast<unsigned char>(bytes.at(0)) == 0x03;
}

// 本程序重新打开自启时，必须把任务管理器留下的禁用标记清掉，
// 否则 Run 键虽然写回去了，Windows 仍然会跳过这个程序。
bool clearStartupApproved()
{
    QSettings approved(QString::fromLatin1(kStartupApprovedRunKey),
                       QSettings::NativeFormat);
    approved.remove(QString::fromLatin1(kValueName));
    approved.sync();
    return approved.status() == QSettings::NoError;
}

} // namespace
#endif // Q_OS_WIN

bool AutoStart::isSupported()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

QString AutoStart::silentStartArgument()
{
    return QStringLiteral("--silent-autostart");
}

bool AutoStart::isEnabled()
{
#ifdef Q_OS_WIN
    QSettings run(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
    const QString command = run.value(QString::fromLatin1(kValueName)).toString();
    if (command.trimmed().isEmpty()) {
        return false;
    }

    if (!samePath(commandExecutable(command), QCoreApplication::applicationFilePath())) {
        return false;
    }
    return !disabledByStartupApproved();
#else
    return false;
#endif
}

bool AutoStart::isSilent()
{
#ifdef Q_OS_WIN
    QSettings run(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
    const QString command = run.value(QString::fromLatin1(kValueName)).toString();
    if (command.trimmed().isEmpty()) {
        return false;
    }

    const QStringList args = QProcess::splitCommand(command);
    return args.contains(AutoStart::silentStartArgument());
#else
    return false;
#endif
}

bool AutoStart::setEnabled(bool enabled, bool silent)
{
#ifdef Q_OS_WIN
    QSettings run(QString::fromLatin1(kRunKey), QSettings::NativeFormat);

    if (enabled) {
        const QString command = currentAppCommand(silent);
        if (command.isEmpty()) {
            return false;
        }
        run.setValue(QString::fromLatin1(kValueName), command);
    } else {
        run.remove(QString::fromLatin1(kValueName));
    }

    run.sync();
    bool ok = run.status() == QSettings::NoError;

    // 打开时清掉旧禁用标记；关闭时也一并清理，避免留下孤儿状态。
    ok = clearStartupApproved() && ok;
    return ok;
#else
    Q_UNUSED(enabled);
    Q_UNUSED(silent);
    return false;
#endif
}
