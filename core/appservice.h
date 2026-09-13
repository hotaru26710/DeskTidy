#ifndef APPSERVICE_H
#define APPSERVICE_H

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include "coretypes.h"
#include "settings.h"
#include "undostack.h"

// ---------------------------------------------------------------------------
// AppService —— 应用级唯一的状态中枢（core 层唯一带信号的类）。
//
// 【为什么需要它】
// 在此之前，UndoStack 与 Settings 的唯一实例以裸指针私有挂在 MainWindow 上，
// 且没有任何对外访问器。一旦加上"桌面常驻浮窗"，就出现两个问题：
//   1) 每个浮窗若各自 new 一个 UndoStack，撤销状态必然各自为政 —— 浮窗 A 收纳后，
//      浮窗 B 的撤销按钮仍不可用，主窗口的按钮也可能撤到一个过期批次（数据正确性问题）；
//   2) 界面刷新原本是硬编码直调（各处手工调 refreshBoxes/updateUndoButton），
//      多窗口场景下这套手动同步模型必然失效。
// AppService 把"状态"和"通知"收拢到一处，主窗口与 N 个浮窗都只是它的消费者。
//
// 【为什么 UndoStack 不改成 QObject、不加信号】
// 这是有意的设计决定：UndoStack 保持纯值语义、零 Qt 元对象依赖，
// 于是它可以被单测直接构造、拷贝、验证，不需要 moc，也不引入全局状态。
// 通知是 AppService 的职责 —— 由 AppService 在 push/undo 之后 emit，
// 而不是让存储层自己广播。把信号加进 UndoStack 会把"状态存储"与
// "状态广播"两件事混在一个类里，存储层从此背上 Qt 事件循环的包袱。
//
// 【生命周期】
// 由 main.cpp 在栈上创建，且**必须声明在 MainWindow 之前**（逆序析构保证
// AppService 活得比所有窗口久）。浮窗动态创建销毁，一律只持有它的裸指针。
//
// 【分层】
// 本类在 core 层，**不认识 ui**：它只发信号、只返回 MoveRecord，
// 不知道谁会接、接的人怎么刷新界面。符合项目"ui 只调 core"的铁律。
// ---------------------------------------------------------------------------

class AppService : public QObject
{
    Q_OBJECT

public:
    explicit AppService(QObject *parent = nullptr);

    // ---- 配置访问 ----
    // 供各 UI 读取排除项等。返回指针而非拷贝：Settings 事实无状态
    // （每次读写都重新构造 QSettings 访问同一个 ini），共享一个实例没有一致性问题。
    Settings       *settings()       { return &m_settings; }
    const Settings *settings() const { return &m_settings; }

    // ---- 撤销状态查询（转发给内部 UndoStack）----
    bool      canUndo() const;
    int       undoCount() const;
    QString   undoBoxName() const;
    QDateTime undoTime() const;

    // ---- 收纳 ----
    // 把 entries 收纳进 boxPath 代表的盒。
    //
    // **目标盒由调用方显式传入**，而不是"读主窗口当前选中的盒"。
    // 这一点是浮窗能正确工作的关键：浮窗代表某个特定盒，点收纳就该收进
    // 它自己那个盒；若沿用"当前选中盒"的语义，主窗口选中的是 A 盒时
    // 在 B 盒浮窗上点收纳，文件会跑进 A 盒 —— 用户看到的和发生的对不上。
    //
    // 内部执行 Collector::collect，成功后把结果推入撤销栈并发信号。
    QList<MoveRecord> collectInto(const QList<DesktopEntry> &entries,
                                  const QString &boxPath,
                                  const QString &boxName);

    // 撤销最近一次收纳。不可撤销时返回空列表（不发信号）。
    QList<MoveRecord> undoLast();

    // 还原指定的若干路径到目标目录。
    //
    // targetDir 为空（默认）时回各自的"原目录"—— 但单条还原场景下，
    // 调用方往往只知道文件当前在盒里，不知道它当初来自哪个桌面目录，
    // 此时应显式传入桌面路径。
    //
    // 实现细节（字段方向，来自 Collector::restore 的**实现**，非其头文件注释）：
    //   Collector::restore 的入参沿用的是 collect 的语义 ——
    //     sourcePath = 当初被收走的位置（桌面）
    //     finalPath  = 当前所在位置（收纳盒内）
    //   而它**返回**的记录语义是对调的（sourcePath = 被撤销的位置，finalPath = 落点）。
    //   本函数构造入参时按前者填，请勿照着头文件那句含糊的注释写反。
    QList<MoveRecord> restorePaths(const QStringList &paths,
                                   const QString &targetDir = QString());

    // ---- 删除收纳盒 ----

    struct BoxDeletionResult
    {
        int     restored     = 0;   // 成功还原到目标目录的条目数
        int     trashedItems = 0;   // 未能还原、随盒目录一起进回收站的条目数
        QString error;              // 非空 = 整体失败，盒**未被删除**

        bool ok() const { return error.isEmpty(); }
    };

    // 删除一个收纳盒。
    //
    // 语义：盒内文件先**还原回 targetDir**（重名自动加序号，绝不覆盖）；
    // 还原不回去的（跨卷、被占用等）连同盒目录本身一起**进回收站** ——
    // 回收站可以还原，所以并不违反"绝不永久删除文件"这条底线。
    //
    // 为什么放在 AppService 而不是 BoxManager：本函数要构造 MoveRecord 调
    // Collector::restore，而那套字段方向（见上面 restorePaths 那段说明）
    // 是全项目最容易搞反、并且**已经踩过一次坑**的地方。
    // 放在这里可以直接复用 restorePathsQuiet，字段方向只有一份实现；
    // 换个模块重写一份，等于把踩过的坑再埋一颗雷。
    //
    // 本函数**不发任何信号**：删盒是"盒层面"的操作，调用方要的是
    // "盒没了、刷新列表"，而不是"有一批文件移动了"。若发 moveFinished，
    // 主窗口会弹出一个标题为"还原到桌面"的失败框，与删盒自己的汇报重复。
    //
    // 调用方拿到 ok() 之后还需自己收尾：关浮窗、清外观配置、刷新列表。
    BoxDeletionResult deleteBox(const QString &boxPath, const QString &targetDir);

    // ---- 结果统计 ----
    // 从 MainWindow::reportMoveResult 的纯计算部分平移过来，做成 static 纯函数：
    //   * 零依赖，可被单测直接调用（这是本次改造里唯一能自动化测试的新逻辑）；
    //   * 各窗口（主窗口、N 个浮窗）各自决定怎么展示，统计口径统一在一处。
    struct MoveSummary
    {
        int         ok      = 0;
        int         skipped = 0;
        int         failed  = 0;
        QStringList detailLines;    // 形如 "· 文件名 —— 失败原因"

        bool hasProblems() const { return skipped > 0 || failed > 0; }

        // 形如 "成功 3 项，跳过 0 项，失败 1 项"
        QString headline() const;
    };

    static MoveSummary summarize(const QList<MoveRecord> &records);

signals:
    // 撤销栈内容发生变化 —— 主窗口与所有浮窗据此同步撤销按钮的可用性与文案。
    void undoStateChanged();

    // 某个盒的内容变了。传空串表示"不知道具体是哪个盒，全部刷新"
    // （撤销场景即如此：撤销栈里可能装着任意盒的批次）。
    //
    // 之所以带上 boxPath 而不是让所有窗口无脑刷新：三个浮窗同时开着时，
    // A 盒收纳完不该让 B、C 重新扫盘。
    void boxContentsChanged(const QString &boxPath);

    // 一次移动操作结束（收纳/撤销/还原），携带完整记录供各窗口自行展示。
    void moveFinished(const QList<MoveRecord> &records, const QString &actionLabel);

private:
    // 与 restorePaths 相同的还原，但**不发任何信号**。
    //
    // 为什么要拆出这一层：删盒流程需要拿到原始的 MoveRecord 明细
    // （统计"还原了几项、哪些没还原成"），但不需要那两个广播 ——
    // 广播会让主窗口弹出一个标题为"还原到桌面"的失败对话框，
    // 与删盒自己的汇报重复，标题还莫名其妙。
    //
    // 副产物（也是它存在的主要理由）：上面那套字段方向的构造逻辑
    // 从此只有这一份实现，restorePaths 与 deleteBox 都复用它。
    QList<MoveRecord> restorePathsQuiet(const QStringList &paths,
                                        const QString &targetDir);

private:
    UndoStack m_undo;       // 值成员：全应用唯一，保证撤销状态不分裂
    Settings  m_settings;   // 值成员：事实无状态，共享实例无一致性问题
};

#endif // APPSERVICE_H
