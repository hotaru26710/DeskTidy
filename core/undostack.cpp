#include "undostack.h"

#include <QDir>
#include <QFileInfo>

#include "collector.h"

// ---------------------------------------------------------------------------
// UndoStack 实现。
//
// 只保一批。主人点错一次"收纳桌面"能整体退回来，这就是需求的全部。
//
// 清栈时机的取舍：undo() 只有在**至少成功还原一条**时才清栈。
// 若整批还原全失败（比如收纳盒所在盘被拔了），栈必须留着，
// 否则主人插回硬盘后连重试的入口都没有了。
// ---------------------------------------------------------------------------

void UndoStack::push(const QList<MoveRecord> &records)
{
    QList<MoveRecord> succeeded;
    succeeded.reserve(records.size());

    for (const MoveRecord &record : records) {
        // 只收成功项：失败/跳过意味着文件没动过，记进来会让撤销做无用功，
        // 还会把 canUndo 变成 true 却什么都撤不回来。
        if (record.state == MoveState::Succeeded)
            succeeded.append(record);
    }

    m_records = succeeded;
    m_time    = QDateTime::currentDateTime();

    // 盒名从任意一条成功记录的落点反推：落点必然在 <根>\<盒名>\ 之下。
    // 取不到则留空，UI 显示时会省略这一项。
    m_boxName.clear();
    if (!m_records.isEmpty()) {
        const QString boxDir = QFileInfo(m_records.first().finalPath).absolutePath();
        m_boxName = QFileInfo(boxDir).fileName();
    }
}

bool UndoStack::canUndo() const
{
    return !m_records.isEmpty();
}

int UndoStack::undoCount() const
{
    return m_records.size();
}

QString UndoStack::undoBoxName() const
{
    return m_boxName;
}

QDateTime UndoStack::undoTime() const
{
    return m_time;
}

QList<MoveRecord> UndoStack::undo()
{
    if (m_records.isEmpty())
        return QList<MoveRecord>();

    const QList<MoveRecord> restored = Collector::restore(m_records);

    // 只要有一条成功落地，这批操作就算已经退回去了，栈必须清掉 ——
    // 否则主人再点一次撤销，会把刚还原到桌面的文件又搬回收纳盒。
    for (const MoveRecord &record : restored) {
        if (record.state == MoveState::Succeeded) {
            m_records.clear();
            m_boxName.clear();
            m_time = QDateTime();
            break;
        }
    }

    return restored;
}
