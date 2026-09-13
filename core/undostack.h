#ifndef UNDOSTACK_H
#define UNDOSTACK_H

#include <QDateTime>
#include <QList>
#include <QString>

#include "coretypes.h"

// ---------------------------------------------------------------------------
// UndoStack —— 只保留"上一次"收纳批次的撤销点。
//
// 需求明确只要这一个粒度：主人点错一次"收纳桌面"能整体退回来即可。
// 做成完整操作历史需要持久化 + 冲突仲裁，对一个"不常驻"的小工具属于过度设计。
//
// 纯内存对象，不落盘 —— 程序关闭即失效，与"不常驻、不开机自启"的定位一致。
// ---------------------------------------------------------------------------

class UndoStack
{
public:
    UndoStack() = default;

    // 记录上一批收纳结果。只保留 state==Succeeded 的条目：
    // 失败/跳过的条目本来就没动过文件，记进来反而会让撤销做无用功。
    // 传入空批次时等同清空（上一批全失败，没有可撤销内容）。
    void push(const QList<MoveRecord> &records);

    bool canUndo() const;
    int  undoCount() const;

    // 供 UI 显示"撤销上次收纳（N 项 · 盒名 · 时间）"。
    QString   undoBoxName() const;
    QDateTime undoTime() const;

    // 调用 Collector::restore 执行还原。
    // **成功后清空**栈，防止重复撤销导致把文件又搬回去一次。
    // 若还原全部失败则保留栈，让主人可以重试。
    QList<MoveRecord> undo();

private:
    QList<MoveRecord> m_records;    // 上一次收纳中成功的条目
    QString           m_boxName;    // 上一次收纳进的盒子名（取自盒目录名）
    QDateTime         m_time;       // 入栈时刻
};

#endif // UNDOSTACK_H
