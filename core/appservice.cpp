#include "appservice.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "boxmanager.h"
#include "collector.h"

// ---------------------------------------------------------------------------
// AppService 实现。
//
// 三条约定：
//   1) 本类不直接动文件 —— 一律经 Collector，它是唯一的写文件入口；
//   2) 状态变化后必须发信号，让所有消费者自己刷新（不再有"谁手工调刷新"这回事）；
//   3) 中文文案一律 translate，为将来的 .ts 留口子。
// ---------------------------------------------------------------------------

AppService::AppService(QObject *parent)
    : QObject(parent)
{
}

// ---------------------------------------------------------------------------
// 撤销状态查询：纯转发。
// 之所以不把 UndoStack 直接暴露出去：那会让调用方绕过信号机制去读状态，
// 于是"状态变了要刷新界面"这件事又变回手工同步。
// ---------------------------------------------------------------------------

bool AppService::canUndo() const
{
    return m_undo.canUndo();
}

int AppService::undoCount() const
{
    return m_undo.undoCount();
}

QString AppService::undoBoxName() const
{
    return m_undo.undoBoxName();
}

QDateTime AppService::undoTime() const
{
    return m_undo.undoTime();
}

// ---------------------------------------------------------------------------
// 收纳
// ---------------------------------------------------------------------------

QList<MoveRecord> AppService::collectInto(const QList<DesktopEntry> &entries,
                                          const QString &boxPath,
                                          const QString &boxName)
{
    if (entries.isEmpty() || boxPath.isEmpty())
        return QList<MoveRecord>();

    const QList<MoveRecord> records = Collector::collect(entries, boxPath);

    // 只有真搬动了东西才推入撤销栈。全失败/全跳过时 push 空批次
    // 会让 canUndo 变成 false（UndoStack::push 只留成功项），
    // 但那样会白白清掉主人上一批可撤销的收纳 —— 所以这里先判断。
    bool anySucceeded = false;
    for (const MoveRecord &record : records) {
        if (record.state == MoveState::Succeeded) {
            anySucceeded = true;
            break;
        }
    }

    if (anySucceeded) {
        m_undo.push(records);
        emit undoStateChanged();
    }

    // 盒内条目数变了，左栏计数与浮窗内容都要跟着更新。
    emit boxContentsChanged(boxPath);

    emit moveFinished(records,
                      QCoreApplication::translate("AppService", "收纳到「%1」")
                          .arg(boxName));

    return records;
}

// ---------------------------------------------------------------------------
// 撤销
// ---------------------------------------------------------------------------

QList<MoveRecord> AppService::undoLast()
{
    if (!m_undo.canUndo())
        return QList<MoveRecord>();

    const QList<MoveRecord> records = m_undo.undo();

    emit undoStateChanged();

    // 传空串 = 全部刷新。撤销栈里可能装着任意一个盒的批次，
    // 而 UndoStack 不告诉我们它撤的是哪个盒（盒名只是供显示的字符串），
    // 与其猜，不如让所有窗口自己重扫 —— 撤销是低频操作，这点开销无所谓。
    emit boxContentsChanged(QString());

    emit moveFinished(records,
                      QCoreApplication::translate("AppService", "撤销收纳"));

    return records;
}

// ---------------------------------------------------------------------------
// 还原指定路径
// ---------------------------------------------------------------------------

QList<MoveRecord> AppService::restorePathsQuiet(const QStringList &paths,
                                                const QString &targetDir)
{
    if (paths.isEmpty())
        return QList<MoveRecord>();

    QList<MoveRecord> plan;
    plan.reserve(paths.size());

    for (const QString &path : paths) {
        if (path.isEmpty())
            continue;

        const QFileInfo info(path);

        MoveRecord record;
        // 字段方向严格按 Collector::restore 的**实现**填（见头文件注释）：
        //   sourcePath = 位置信息的来源，restore 内部只用它推目标目录与文件名；
        //   finalPath  = 文件当前实际所在位置，restore 从这个路径搬走。
        //
        // targetDir 给定时（单条还原：文件在盒里，要回桌面），
        // sourcePath 就用"目标目录 + 原文件名"合成，于是 restore 会把文件
        // 搬到 targetDir 下，并在重名时自动避让。
        // targetDir 为空时（撤销场景）用原路径，restore 会取它的父目录作为落点。
        record.finalPath  = info.absoluteFilePath();
        record.sourcePath = targetDir.isEmpty()
                                ? info.absoluteFilePath()
                                : QDir(targetDir).absoluteFilePath(info.fileName());
        record.isDir      = info.isDir();
        record.size       = info.isDir() ? 0 : info.size();
        record.state      = MoveState::Succeeded;   // restore 只处理 Succeeded
        record.time       = QDateTime::currentDateTime();

        plan.append(record);
    }

    if (plan.isEmpty())
        return QList<MoveRecord>();

    // 注意：这里**不发任何信号**。这是刻意的 —— 见头文件里对本函数的说明。
    // 上面那段字段方向的构造是全应用唯一的一份实现，
    // restorePaths（公开、发信号）与 deleteBox（发自己的汇报）都复用它。
    return Collector::restore(plan);
}

QList<MoveRecord> AppService::restorePaths(const QStringList &paths,
                                           const QString &targetDir)
{
    // 薄包装：拿到静默结果后补发两个广播。
    // 之所以把广播留在这里而不是塞进 restorePathsQuiet，
    // 是因为删盒流程需要"不发信号"的那个版本（否则会弹出一个标题为
    // "还原到桌面"的失败对话框，与删盒自己的汇报重复且误导）。
    const QList<MoveRecord> records = restorePathsQuiet(paths, targetDir);

    if (records.isEmpty())
        return records;

    emit boxContentsChanged(QString());     // 不知道影响哪个盒，全部刷新

    emit moveFinished(records,
                      QCoreApplication::translate("AppService", "还原到桌面"));

    return records;
}

// ---------------------------------------------------------------------------
// 删除收纳盒
// ---------------------------------------------------------------------------

AppService::BoxDeletionResult AppService::deleteBox(const QString &boxPath,
                                                    const QString &targetDir)
{
    BoxDeletionResult result;

    // ---------- Step 0：前置检查 ----------
    // 盒目录已经不在了（主人可能在资源管理器里自己删过）。
    // 这**不是失败** —— "把它从列表里去掉"本来就是调用方的目的，
    // 报错反而会让主人困惑："我要删它，你说它不存在？"
    if (!QFileInfo(boxPath).isDir())
        return result;      // ok() == true，各项计数为 0

    // ---------- Step 1：还原 ----------
    // 取盒内**直接子项**，不递归：子目录作为一个整体条目还原，
    // 它内部的结构由目录移动一并带过去。
    //
    // 必须用 listBoxItems 而不是自己 QDir 枚举 —— 它带 QDir::Hidden，
    // 隐藏文件也会被列出来。漏了的话那些文件既不还原、又不会计入统计，
    // 最后随着目录一起进回收站，主人事后才发现少东西。
    const QList<DesktopEntry> items = BoxManager::listBoxItems(boxPath);

    QStringList inBoxPaths;
    inBoxPaths.reserve(items.size());
    for (const DesktopEntry &entry : items)
        inBoxPaths << entry.filePath;

    if (!inBoxPaths.isEmpty()) {
        // 用 Quiet 版本：它不发信号。
        // 若走公开的 restorePaths，会 emit moveFinished("还原到桌面")，
        // 主窗口据此弹出一个标题为"还原到桌面"的失败对话框 —— 与删盒
        // 自己的汇报重复，且主人根本不知道自己在做"还原"。
        const QList<MoveRecord> records = restorePathsQuiet(inBoxPaths, targetDir);

        // 统计。三种状态的含义见 collector.cpp：
        //   Succeeded —— 已经搬到目标目录了
        //   Failed    —— 没搬成，文件**仍在盒里**，即将随目录进回收站
        //   Skipped   —— 盒里已经找不到这个文件了（被外部动过），没什么可丢的
        for (const MoveRecord &r : records) {
            switch (r.state) {
            case MoveState::Succeeded:
                ++result.restored;
                break;
            case MoveState::Failed:
                ++result.trashedItems;
                break;
            case MoveState::Skipped:
                break;      // 不计入任何一边：文件本来就不在盒里
            }
        }
    }

    // ---------- Step 2：回收站兜底 ----------
    // 即使盒子已经空了也要走这一步 —— 空目录同样要进回收站。
    //
    // 为什么不改成 QDir::rmdir 删空目录：那是**永久删除**，
    // 与本项目"只往回收站丢、绝不永久删"的基调不一致；
    // 而丢回收站只多花一次调用，还顺带兜住"万一有隐藏文件没被扫到"。
    //
    // 为什么整个目录一次丢、而不是把失败的文件逐个挑出来丢：
    //   逐个丢会得到"N 个散落条目 + 1 个空目录"，它们在回收站里的
    //   从属关系断了，主人还原时得自己拼回去。
    //   而成功还原的文件此时**已经不在盒里**了，所以这一个目录里
    //   剩下的正是"还原失败的那些"，整体丢就是精准兜底，不多不少。
    if (!QFile::moveToTrash(boxPath)) {
        // 连目录都丢不进回收站：这是真失败，**不能**假装删成功。
        //
        // 关键：这里必须整体失败、且**不继续后续收尾**。
        // 因为盒目录还在磁盘上，下次扫描还会被列出来；此时若关了浮窗、
        // 刷了列表，主人会看到"盒消失了"，但下次打开程序它又回来了 ——
        // 状态分裂，比直接报错难查得多。
        result.error = QCoreApplication::translate(
                           "AppService",
                           "无法把收纳盒「%1」移入回收站。\n\n"
                           "可能原因：目录正被其他程序占用（里面的文件正被打开），"
                           "或所在磁盘不支持回收站（例如网络驱动器、U 盘）。\n\n"
                           "未做任何删除，收纳盒保持原样。")
                           .arg(QFileInfo(boxPath).fileName());

        // 已经还原到目标目录的文件不回滚 —— 它们此刻好好地躺在桌面上，
        // 搬回去再失败一次只会让状态更乱。调用方应把这一点告诉主人。
        return result;
    }

    return result;      // ok() == true，调用方据此收尾（关浮窗 / 清配置 / 刷新）
}

// ---------------------------------------------------------------------------
// 结果统计
// ---------------------------------------------------------------------------

QString AppService::MoveSummary::headline() const
{
    return QCoreApplication::translate("AppService",
                                       "成功 %1 项，跳过 %2 项，失败 %3 项")
        .arg(ok)
        .arg(skipped)
        .arg(failed);
}

AppService::MoveSummary AppService::summarize(const QList<MoveRecord> &records)
{
    MoveSummary summary;

    for (const MoveRecord &record : records) {
        // 展示名取"主人认得出的那个名字"：收纳时源在桌面、还原时源在盒内，
        // 两种情况下 sourcePath 都指向文件被搬离的那一侧，故统一取它的文件名。
        // sourcePath 为空（异常记录）时退回到 finalPath，避免清单里出现空条目。
        // —— 此逻辑与改造前 MainWindow::reportMoveResult 的行为保持一致。
        const QString shown =
            QFileInfo(record.sourcePath.isEmpty() ? record.finalPath
                                                  : record.sourcePath)
                .fileName();

        switch (record.state) {
        case MoveState::Succeeded:
            ++summary.ok;
            break;

        case MoveState::Skipped:
            ++summary.skipped;
            // 跳过也值得让主人知道是哪一条，故一并列入清单。
            summary.detailLines
                << QCoreApplication::translate("AppService", "· %1 —— %2")
                       .arg(shown, record.error);
            break;

        case MoveState::Failed:
            ++summary.failed;
            summary.detailLines
                << QCoreApplication::translate("AppService", "· %1 —— %2")
                       .arg(shown, record.error);
            break;
        }
    }

    return summary;
}
