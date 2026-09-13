#include "collector.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

#include "corenames.h"

// ---------------------------------------------------------------------------
// Collector 实现 —— 全应用唯一执行真实文件移动的地方。
//
// 本文件里每一次 QFile::rename / copy / remove 都直接关系到主人的数据安全，
// 修改前请先读完这段：
//
//   1) 目标路径永远来自 CoreNames::uniqueTargetPath，它在返回前已确认该路径
//      不存在。因此**不存在任何覆盖写路径**，QFile::copy 也不会静默覆盖。
//   2) copy 成功但 remove 源失败时，必须把已落地的副本删掉再报错，
//      否则同一个文件会同时存在于桌面和收纳盒，主人无从判断哪个是真的。
//   3) 目录跨卷不做递归复制：递归到一半失败会留下半棵目录树，
//      比直接报"不在同一分区"糟得多。
//   4) 单条失败绝不中断整批，原因写进 error 由 UI 汇总展示。
// ---------------------------------------------------------------------------

namespace Collector {

namespace {

// 把 Qt 的失败状态映射成主人能看懂的中文。
// QFile 本身不提供 errno，只能按路径特征做有限推断 —— 宁可给一个偏保守
// 的通用措辞，也不要编造一个可能误导主人的具体原因。
QString describeFileError(const QString &path, bool isDir)
{
    const QFileInfo info(path);

    if (!info.exists()) {
        return QCoreApplication::translate(
            "Collector", "源%1已不存在，可能已被移动或删除。")
            .arg(isDir ? QCoreApplication::translate("Collector", "文件夹")
                       : QCoreApplication::translate("Collector", "文件"));
    }

    if (!info.isWritable()) {
        return QCoreApplication::translate(
            "Collector", "没有写入权限，或%1处于只读状态。")
            .arg(isDir ? QCoreApplication::translate("Collector", "文件夹")
                       : QCoreApplication::translate("Collector", "文件"));
    }

    return QCoreApplication::translate(
        "Collector", "操作失败：文件可能正被其他程序占用，或权限不足。");
}

// 判断两个路径是否位于同一个卷（盘符 / 挂载点）。
// 仅用于给跨卷失败提供可读原因；不参与控制流决策。
bool sameVolume(const QString &lhs, const QString &rhs)
{
    const QString a = QDir::toNativeSeparators(QDir::cleanPath(lhs));
    const QString b = QDir::toNativeSeparators(QDir::cleanPath(rhs));
    return !a.isEmpty() && !b.isEmpty()
           && a.left(2).compare(b.left(2), Qt::CaseInsensitive) == 0;
}

// 移动单个**文件**：先 rename，跨卷时回落 copy + remove。
// 返回 true 表示文件已在 to 落地且源已消失。
bool moveFile(const QString &from, const QString &to, QString *error)
{
    if (QFile::rename(from, to))
        return true;

    // 到这里通常是 EXDEV（跨卷），也可能是目标被占用等。
    // 无论如何先试 copy 回落 —— 成功即达成目的。
    QDir().mkpath(QFileInfo(to).absolutePath());

    if (!QFile::copy(from, to)) {
        if (error) {
            *error = sameVolume(from, to)
                         ? QCoreApplication::translate(
                               "Collector", "复制到收纳盒失败：文件可能被占用或权限不足。")
                         : QCoreApplication::translate(
                               "Collector", "复制到收纳盒失败：目标磁盘空间不足或不可写。");
        }
        return false;
    }

    if (!QFile::remove(from)) {
        // 关键回滚：副本已落地但源没删掉，必须清掉副本，
        // 否则同一文件出现两份且状态不明。清理失败也只能如实报告。
        if (!QFile::remove(to)) {
            if (error) {
                *error = QCoreApplication::translate(
                             "Collector",
                             "复制成功但无法删除源文件，且清理残留副本失败："
                             "该文件现在同时存在于桌面与收纳盒，请手动确认。");
            }
            return false;
        }
        if (error) {
            *error = QCoreApplication::translate(
                         "Collector", "复制成功但无法删除源文件，已回滚，桌面内容未改变。");
        }
        return false;
    }

    return true;
}

// 执行一次"移动"（文件或目录），结果写进 record。
// 这是收纳与还原共用的底层动作。
void performMove(const QString &from, const QString &to, bool isDir, MoveRecord *record)
{
    QDir().mkpath(QFileInfo(to).absolutePath());

    if (QFile::rename(from, to)) {
        record->state = MoveState::Succeeded;
        return;
    }

    if (isDir) {
        // 目录跨卷不递归复制，理由见文件头注释第 3 条。
        record->state = MoveState::Failed;
        record->error = sameVolume(from, to)
                            // 同卷还失败，多半是被占用或权限问题；
                            // 先问 describeFileError 要一个更贴合现场的原因。
                            ? describeFileError(from, true)
                            : QCoreApplication::translate(
                                  "Collector", "目标与源不在同一磁盘分区，无法移动文件夹。");
        return;
    }

    QString err;
    if (moveFile(from, to, &err)) {
        record->state = MoveState::Succeeded;
        return;
    }

    record->state = MoveState::Failed;
    // moveFile 给的是"复制/删除阶段"的具体原因，更贴近事实，优先采用；
    // 它没给出原因时才回落到按源文件状态推断的通用说明。
    record->error = err.isEmpty() ? describeFileError(from, false) : err;
}

} // namespace

QList<MoveRecord> collect(const QList<DesktopEntry> &entries, const QString &boxPath)
{
    QList<MoveRecord> records;
    records.reserve(entries.size());

    int okCount = 0, skipCount = 0, failCount = 0;

    for (const DesktopEntry &entry : entries) {
        MoveRecord record;
        record.sourcePath = entry.filePath;
        record.origin     = entry.origin;
        record.size       = entry.size;
        record.isDir      = entry.isDir;
        record.time       = QDateTime::currentDateTime();

        const QFileInfo srcInfo(entry.filePath);

        // 扫描与执行之间主人可能已经动过这个文件，先确认再动手。
        if (!srcInfo.exists()) {
            record.state     = MoveState::Skipped;
            record.finalPath = entry.filePath;
            record.error     = QCoreApplication::translate(
                                   "Collector", "源文件已不存在，可能已被移动或删除。");
            ++skipCount;
            records.append(record);
            continue;
        }

        // 目标路径由 uniqueTargetPath 保证不存在 —— 这是"绝不覆盖"的唯一来源。
        // 目录重名与文件重名走同一套避让规则，无需分支。
        const QString target = CoreNames::uniqueTargetPath(boxPath, entry.name);
        record.finalPath = target;

        performMove(entry.filePath, target, entry.isDir, &record);

        switch (record.state) {
        case MoveState::Succeeded:
            ++okCount;
            break;
        case MoveState::Skipped:
            ++skipCount;
            break;
        case MoveState::Failed:
            ++failCount;
            break;
        }

        records.append(record);
    }

    // 汇总一行给日志；逐条失败再单独告警，便于主人排查。
    qInfo().noquote() << QStringLiteral("收纳完成：成功 %1 / 跳过 %2 / 失败 %3 / 共 %4 条")
                             .arg(okCount).arg(skipCount).arg(failCount).arg(records.size());

    for (const MoveRecord &record : records) {
        if (record.state == MoveState::Failed) {
            qWarning().noquote() << QStringLiteral("收纳失败：%1 -> %2：%3")
                                        .arg(QDir::toNativeSeparators(record.sourcePath),
                                             QDir::toNativeSeparators(record.finalPath),
                                             record.error);
        }
    }

    return records;
}

QList<MoveRecord> restore(const QList<MoveRecord> &records)
{
    QList<MoveRecord> result;
    result.reserve(records.size());

    int okCount = 0, skipCount = 0, failCount = 0;

    for (const MoveRecord &original : records) {
        // 只还原真正动过的条目；失败/跳过的本来就没搬，碰它们反而危险。
        if (original.state != MoveState::Succeeded)
            continue;

        MoveRecord record;
        // 还原场景下两个路径字段语义对调：
        //   sourcePath = 当前所在位置（收纳盒里）
        //   finalPath  = 还原后的落点（桌面）
        record.sourcePath = original.finalPath;
        record.origin     = original.origin;
        record.size       = original.size;
        record.isDir      = original.isDir;
        record.time       = QDateTime::currentDateTime();

        const QFileInfo srcInfo(original.finalPath);
        if (!srcInfo.exists()) {
            record.state     = MoveState::Skipped;
            record.finalPath = original.sourcePath;
            record.error     = QCoreApplication::translate(
                                   "Collector", "收纳盒中已找不到该项目，无法还原。");
            ++skipCount;
            result.append(record);
            continue;
        }

        // 主人可能在桌面重新建了同名文件，落点同样要重新避让，绝不覆盖。
        const QString targetDir  = QFileInfo(original.sourcePath).absolutePath();
        const QString targetName = QFileInfo(original.sourcePath).fileName();
        const QString target     = CoreNames::uniqueTargetPath(targetDir, targetName);
        record.finalPath = target;

        performMove(original.finalPath, target, original.isDir, &record);

        switch (record.state) {
        case MoveState::Succeeded:
            ++okCount;
            break;
        case MoveState::Skipped:
            ++skipCount;
            break;
        case MoveState::Failed:
            ++failCount;
            break;
        }

        result.append(record);
    }

    qInfo().noquote() << QStringLiteral("撤销完成：成功 %1 / 跳过 %2 / 失败 %3 / 共 %4 条")
                             .arg(okCount).arg(skipCount).arg(failCount).arg(result.size());

    for (const MoveRecord &record : result) {
        if (record.state == MoveState::Failed) {
            qWarning().noquote() << QStringLiteral("还原失败：%1 -> %2：%3")
                                        .arg(QDir::toNativeSeparators(record.sourcePath),
                                             QDir::toNativeSeparators(record.finalPath),
                                             record.error);
        }
    }

    return result;
}

} // namespace Collector
