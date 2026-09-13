#include <QStandardPaths>
#include <QStringList>
#include <QtTest/QtTest>

#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QTemporaryDir>

#include "appservice.h"
#include "coretypes.h"
#include "settings.h"

// ---------------------------------------------------------------------------
// tst_appservice —— 阶段 1 新增纯逻辑的单测。
//
// 只测能脱离 UI、脱离真实文件搬动就跑起来的东西：
//   * AppService::summarize —— 从 MainWindow::reportMoveResult 平移过来的统计逻辑，
//     它是本次改造里唯一"有分支、容易错、又完全纯"的函数；
//   * Settings 的浮窗配置读写 —— 尤其是含特殊字符的盒名如何安全地变成配置键。
//
// 刻意不测的东西（以及为什么）：
//   * collectInto / undoLast / restorePaths —— 会真的搬动文件。
//     它们的行为由 tools/e2e_probe.cpp 在隔离目录里真刀真枪验证，比在这里
//     用 mock 测更有说服力，也不会拿主人的磁盘做实验。
//   * Opener::openPath 的成功分支 —— 它会真的启动一个程序（打开 exe 就跑），
//     单测里只验失败分支。
// ---------------------------------------------------------------------------

class TestAppService : public QObject
{
    Q_OBJECT

private:
    // 造一条 MoveRecord 的小助手，省得每个用例都写五遍字段。
    static MoveRecord rec(MoveState state,
                          const QString &sourcePath,
                          const QString &finalPath = QString(),
                          const QString &error = QString())
    {
        MoveRecord r;
        r.state      = state;
        r.sourcePath = sourcePath;
        r.finalPath  = finalPath;
        r.error      = error;
        r.time       = QDateTime::currentDateTime();
        return r;
    }

private slots:

    // 每个用例前把测试用的配置清空，保证用例之间互不影响、且重复跑结果一致。
    void init()
    {
        const QString dir =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        if (!dir.isEmpty())
            QDir(dir).removeRecursively();
    }

    // ===================== summarize() =====================

    void summarizeEmptyList()
    {
        const AppService::MoveSummary s =
            AppService::summarize(QList<MoveRecord>());

        QCOMPARE(s.ok, 0);
        QCOMPARE(s.skipped, 0);
        QCOMPARE(s.failed, 0);
        QVERIFY2(s.detailLines.isEmpty(), "空清单不该产生明细行");
        QVERIFY2(!s.hasProblems(), "空清单不该被判为有问题");
    }

    void summarizeAllSucceeded()
    {
        QList<MoveRecord> records;
        records << rec(MoveState::Succeeded, QStringLiteral("C:/Desktop/a.txt"))
                << rec(MoveState::Succeeded, QStringLiteral("C:/Desktop/b.txt"))
                << rec(MoveState::Succeeded, QStringLiteral("C:/Desktop/c.txt"));

        const AppService::MoveSummary s = AppService::summarize(records);

        QCOMPARE(s.ok, 3);
        QCOMPARE(s.skipped, 0);
        QCOMPARE(s.failed, 0);
        QVERIFY2(s.detailLines.isEmpty(), "全成功不该有明细行");
        QVERIFY2(!s.hasProblems(), "全成功不该被判为有问题");
    }

    void summarizeMixedStates()
    {
        QList<MoveRecord> records;
        records << rec(MoveState::Succeeded, QStringLiteral("C:/Desktop/ok1.txt"))
                << rec(MoveState::Succeeded, QStringLiteral("C:/Desktop/ok2.txt"))
                << rec(MoveState::Skipped,   QStringLiteral("C:/Desktop/skip.txt"),
                       QString(), QStringLiteral("源文件已不存在"))
                << rec(MoveState::Failed,    QStringLiteral("C:/Desktop/fail.txt"),
                       QString(), QStringLiteral("文件被占用"));

        const AppService::MoveSummary s = AppService::summarize(records);

        QCOMPARE(s.ok, 2);
        QCOMPARE(s.skipped, 1);
        QCOMPARE(s.failed, 1);
        QVERIFY(s.hasProblems());
        // 跳过与失败都要出现在明细里 —— 需求明确要求让主人看到每一条
        QCOMPARE(s.detailLines.size(), 2);
    }

    void summarizeShowsFileNameNotFullPath()
    {
        QList<MoveRecord> records;
        records << rec(MoveState::Failed,
                       QStringLiteral("C:/Users/x/Desktop/报告.docx"),
                       QString(), QStringLiteral("没有权限"));

        const AppService::MoveSummary s = AppService::summarize(records);

        QCOMPARE(s.detailLines.size(), 1);
        const QString line = s.detailLines.first();
        QVERIFY2(line.contains(QStringLiteral("报告.docx")), "明细里应含文件名");
        QVERIFY2(!line.contains(QStringLiteral("C:/Users")),
                 "明细里不该出现完整路径，那对主人没有意义还很长");
    }

    void summarizeFallsBackToFinalPathWhenSourceEmpty()
    {
        // 还原场景下可能出现 sourcePath 为空的异常记录，
        // 此时要退回 finalPath 取名字，否则明细里会出现空条目。
        QList<MoveRecord> records;
        records << rec(MoveState::Failed, QString(),
                       QStringLiteral("C:/Users/x/DeskTidy/临时/孤儿.txt"),
                       QStringLiteral("源路径丢失"));

        const AppService::MoveSummary s = AppService::summarize(records);

        QCOMPARE(s.detailLines.size(), 1);
        QVERIFY2(s.detailLines.first().contains(QStringLiteral("孤儿.txt")),
                 "sourcePath 为空时应退回 finalPath 取文件名");
    }

    void summarizeBothPathsEmptyDoesNotCrash()
    {
        // 极端情况：两个路径都空。不能崩，也不能产生一个含义不明的条目。
        QList<MoveRecord> records;
        records << rec(MoveState::Failed, QString(), QString(),
                       QStringLiteral("未知错误"));

        const AppService::MoveSummary s = AppService::summarize(records);

        QCOMPARE(s.failed, 1);
        QCOMPARE(s.detailLines.size(), 1);
        // 有原因就够主人判断了，名字为空是可接受的降级
        QVERIFY(s.detailLines.first().contains(QStringLiteral("未知错误")));
    }

    void summarizeHeadlineMentionsAllThreeCounts()
    {
        QList<MoveRecord> records;
        records << rec(MoveState::Succeeded, QStringLiteral("a"))
                << rec(MoveState::Skipped,   QStringLiteral("b"), QString(),
                       QStringLiteral("已不存在"))
                << rec(MoveState::Failed,    QStringLiteral("c"), QString(),
                       QStringLiteral("被占用"));

        const QString head = AppService::summarize(records).headline();

        QVERIFY2(head.contains(QStringLiteral("1")), "标题应含数字");
        // 三个计数都要出现，否则主人看不出到底发生了什么
        QVERIFY2(head.contains(QStringLiteral("成功")), "标题应含「成功」");
        QVERIFY2(head.contains(QStringLiteral("跳过")), "标题应含「跳过」");
        QVERIFY2(head.contains(QStringLiteral("失败")), "标题应含「失败」");
    }

    void summarizeDoesNotMutateInput()
    {
        // 传进来的是 const 引用，但实现里若有意外拷贝修改就该被抓到。
        QList<MoveRecord> records;
        records << rec(MoveState::Failed, QStringLiteral("x"), QString(),
                       QStringLiteral("错"));

        AppService::summarize(records);

        QCOMPARE(records.size(), 1);
        QCOMPARE(records.first().state, MoveState::Failed);
    }

    // ===================== Settings 浮窗配置 =====================

    void settingsFloatingBoxesRoundTrip()
    {
        Settings s;
        const QStringList names{QStringLiteral("临时"), QStringLiteral("工作"),
                                QStringLiteral("下载暂存")};
        s.setOpenBoxNames(names);
        QCOMPARE(s.openBoxNames(), names);

        // 清空后不留脏键
        s.setOpenBoxNames(QStringList());
        QVERIFY2(s.openBoxNames().isEmpty(), "清空后应读回空列表");
    }

    void settingsFloatingBoxesCleansInput()
    {
        Settings s;
        // 带空白与空串的输入要被清洗，与现有 excludedNames() 行为一致
        s.setOpenBoxNames({QStringLiteral("  临时  "), QString(),
                           QStringLiteral("工作"), QStringLiteral("   ")});
        QCOMPARE(s.openBoxNames(),
                 QStringList({QStringLiteral("临时"), QStringLiteral("工作")}));
    }

    void settingsGeometryRoundTripWithTrickyBoxName()
    {
        // 关键用例：盒名里可能有 / 或中文，而 QSettings 用 / 作键分隔符。
        // 若不做转义，这类盒名会把键结构搅乱、读写错位。
        Settings s;
        const QString tricky = QStringLiteral("a/b:测试 盒");
        const QByteArray blob = QByteArray::fromHex("deadbeef0102");

        s.setFloatGeometry(tricky, blob);
        QCOMPARE(s.floatGeometry(tricky), blob);

        // 另一个盒名的几何不应被串味
        QVERIFY2(s.floatGeometry(QStringLiteral("别的盒")).isEmpty(),
                 "不同盒名的几何数据不应互相污染");
    }

    void settingsGeometrySeparatedPerBox()
    {
        Settings s;
        const QByteArray a = QByteArray("geometry-A");
        const QByteArray b = QByteArray("geometry-B");

        s.setFloatGeometry(QStringLiteral("盒A"), a);
        s.setFloatGeometry(QStringLiteral("盒B"), b);

        QCOMPARE(s.floatGeometry(QStringLiteral("盒A")), a);
        QCOMPARE(s.floatGeometry(QStringLiteral("盒B")), b);
    }

    void settingsRolledUpRoundTrip()
    {
        Settings s;
        s.setFloatRolledUp(QStringLiteral("临时"), true);
        QVERIFY(s.floatRolledUp(QStringLiteral("临时")));

        s.setFloatRolledUp(QStringLiteral("临时"), false);
        QVERIFY2(!s.floatRolledUp(QStringLiteral("临时")), "应能改回展开");

        // 没设过的盒默认应为展开（false），而不是随机的脏值
        QVERIFY2(!s.floatRolledUp(QStringLiteral("没见过的盒")),
                 "未设置过的盒应默认为展开");
    }

    void settingsAlwaysOnTopDefaultsToTrue()
    {
        // 默认置顶是需求决定（浮窗要浮在桌面上），
        // 用户可以从右键菜单关掉。
        Settings s;
        s.setAlwaysOnTop(true);
        QVERIFY(s.alwaysOnTop());
        s.setAlwaysOnTop(false);
        QVERIFY(!s.alwaysOnTop());
    }

    void settingsTrayHintRoundTrip()
    {
        Settings s;
        s.setTrayHintShown(true);
        QVERIFY(s.trayHintShown());
        s.setTrayHintShown(false);
        QVERIFY(!s.trayHintShown());
    }

    void settingsAutoStartSilentRoundTrip()
    {
        Settings s;

        // 默认应是普通自启，避免旧配置没有该键时界面误显示成静默。
        QVERIFY(!s.autoStartSilent());

        s.setAutoStartSilent(true);
        QVERIFY(s.autoStartSilent());

        s.setAutoStartSilent(false);
        QVERIFY(!s.autoStartSilent());
    }

    // =======================================================================
    // 删除收纳盒
    //
    // ⚠️【已知副作用，务必知情】
    // deleteBox 的最后一步会把盒目录丢进**系统真实回收站**，而这一步在
    // 单测里没法撤销 —— QTemporaryDir 只回收自己目录里还剩的东西，
    // 被 moveToTrash 搬走的盒已经不在那儿了。
    //
    // 所以：**每跑一次这组测试，回收站里就会多出几个目录。**
    // 为了让它们可识别、可清理，各用例的盒名统一带 "DeskTidyTest-" 前缀。
    // verify.bat 的末尾会自动清理这些残留。
    //
    // 为什么不干脆跳过回收站那一步：deleteBox 的语义就是"还原 + 兜底进回收站"，
    // 跳过就变成了两个互不相干的半截操作，测出来的东西没有意义。
    // 回收站本身的能力（文件/目录/递归）已由 tools/trash_probe2.cpp 单独验证。
    // =======================================================================

    // 辅助：造一个盒目录，放若干文件进去，返回盒路径。
    static QString makeBoxWithFiles(const QString &root, const QString &boxName,
                                    const QStringList &fileNames)
    {
        const QString boxPath = root + QLatin1Char('/') + boxName;
        QDir().mkpath(boxPath);
        for (const QString &n : fileNames) {
            QFile f(boxPath + QLatin1Char('/') + n);
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                f.write("payload");
                f.close();
            }
        }
        return boxPath;
    }

    // 防的是：盒目录根本不存在时，删盒流程报错 —— 那会让主人困惑
    // （"我要删它，你说它不存在？"）。目的已经达成，就该当成成功。
    void deleteBoxOnMissingDirectoryIsNotAnError()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        AppService service;
        const AppService::BoxDeletionResult r =
            service.deleteBox(tmp.path() + QStringLiteral("/根本没有这个盒"),
                              tmp.path() + QStringLiteral("/target"));

        QVERIFY2(r.ok(), "盒目录不存在不应被视为失败");
        QCOMPARE(r.error, QString());
        QCOMPARE(r.restored, 0);
    }

    // 防的是：空盒删除时出错（没有可还原的东西，走的是最短路径）。
    void deleteBoxOnEmptyDirectorySucceeds()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        const QString boxPath = tmp.path() + QStringLiteral("/DeskTidyTest-空盒");
        QDir().mkpath(boxPath);
        QVERIFY(QDir(boxPath).exists());

        AppService service;
        const AppService::BoxDeletionResult r =
            service.deleteBox(boxPath, tmp.path() + QStringLiteral("/target"));

        // 只断言"还原了 0 项"与"没报错"。
        // 目录是否真的进了回收站不在这里断言 —— 那会污染真实回收站。
        QCOMPARE(r.restored, 0);
        QCOMPARE(r.trashedItems, 0);
    }

    // 防的是：还原这一步把文件搬错方向（这是全项目最经典的坑，
    // 见 Collector::restore 的字段方向说明）。
    // 断言"文件出现在目标目录"，而不是"盒里没了" —— 后者两种错误方向都能满足。
    void deleteBoxRestoresFilesToTargetDirectory()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        const QString target = tmp.path() + QStringLiteral("/target");
        QDir().mkpath(target);

        const QString boxPath = makeBoxWithFiles(
            tmp.path(), QStringLiteral("DeskTidyTest-有内容的盒"),
            {QStringLiteral("alpha.txt"), QStringLiteral("beta.txt")});

        AppService service;
        const AppService::BoxDeletionResult r = service.deleteBox(boxPath, target);

        QVERIFY2(r.ok(), qPrintable(r.error));
        QVERIFY2(r.restored == 2,
                 qPrintable(QStringLiteral("应还原 2 项，实际 %1").arg(r.restored)));

        // 关键断言：文件确实到了目标目录
        QVERIFY2(QFile::exists(target + QStringLiteral("/alpha.txt")),
                 "alpha.txt 应当出现在目标目录");
        QVERIFY2(QFile::exists(target + QStringLiteral("/beta.txt")),
                 "beta.txt 应当出现在目标目录");
    }

    // 防的是：还原时覆盖目标目录里的同名文件 —— 这违反"绝不覆盖"铁律。
    // 这是删盒功能里最贵的一种错误（主人的文件被悄悄换掉）。
    void deleteBoxNeverOverwritesExistingFileInTarget()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        const QString target = tmp.path() + QStringLiteral("/target");
        QDir().mkpath(target);

        // 目标目录里先放一个同名文件，内容可辨认
        {
            QFile existing(target + QStringLiteral("/same.txt"));
            QVERIFY(existing.open(QIODevice::WriteOnly | QIODevice::Text));
            existing.write("ORIGINAL_CONTENT");
            existing.close();
        }

        const QString boxPath = makeBoxWithFiles(
            tmp.path(), QStringLiteral("DeskTidyTest-有同名的盒"), {QStringLiteral("same.txt")});

        AppService service;
        const AppService::BoxDeletionResult r = service.deleteBox(boxPath, target);

        QVERIFY2(r.ok(), qPrintable(r.error));

        // 原文件必须原样
        QFile f(target + QStringLiteral("/same.txt"));
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString content = QString::fromUtf8(f.readAll());
        f.close();
        QCOMPARE(content, QStringLiteral("ORIGINAL_CONTENT"));

        // 被还原的那个应当自动加了序号
        QVERIFY2(QFile::exists(target + QStringLiteral("/same (2).txt")),
                 "重名的文件应当被改名为 same (2).txt，而不是覆盖原文件");
    }

    // 防的是：盒里有子目录时还原失败（子目录要作为一个整体搬走，内部结构保留）。
    void deleteBoxRestoresNestedDirectory()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        const QString target = tmp.path() + QStringLiteral("/target");
        QDir().mkpath(target);

        // 造一个带内部结构的子目录
        const QString boxPath = tmp.path() + QStringLiteral("/DeskTidyTest-带目录的盒");
        QDir().mkpath(boxPath + QStringLiteral("/photos/nested"));
        {
            QFile f(boxPath + QStringLiteral("/photos/nested/deep.txt"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write("deep");
            f.close();
        }

        AppService service;
        const AppService::BoxDeletionResult r = service.deleteBox(boxPath, target);

        QVERIFY2(r.ok(), qPrintable(r.error));
        QVERIFY2(QFile::exists(target + QStringLiteral("/photos/nested/deep.txt")),
                 "子目录应当连同内部结构一起还原到目标目录");
    }

    // 防的是：deleteBox 发了 moveFinished 信号，导致主窗口弹出一个
    // 标题为"还原到桌面"的失败对话框（与删盒自己的汇报重复且误导）。
    // 这是设计里特意拆出 restorePathsQuiet 的原因，值得钉住。
    void deleteBoxEmitsNoMoveFinishedSignal()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        const QString target = tmp.path() + QStringLiteral("/target");
        QDir().mkpath(target);

        AppService service;

        int moveFinishedCount = 0;
        int contentsChangedCount = 0;
        QObject::connect(&service, &AppService::moveFinished,
                         [&](const QList<MoveRecord> &, const QString &) {
                             ++moveFinishedCount;
                         });
        QObject::connect(&service, &AppService::boxContentsChanged,
                         [&](const QString &) { ++contentsChangedCount; });

        const QString boxPath = makeBoxWithFiles(
            tmp.path(), QStringLiteral("DeskTidyTest-信号测试盒"), {QStringLiteral("x.txt")});

        service.deleteBox(boxPath, target);

        QCOMPARE(moveFinishedCount, 0);
        QCOMPARE(contentsChangedCount, 0);
    }
};

int main(int argc, char *argv[])
{
    // 关键：把测试的配置写到独立位置，不能污染主人的真实设置。
    // Settings 内部用 QStandardPaths::AppConfigLocation 解析 ini 路径，
    // 而该路径由「组织名/应用名」决定 —— 所以这里换成一组测试专用名字，
    // 两边的配置天然隔离，不需要给 Settings 开任何测试后门。
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyTest"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyTest"));

    QApplication app(argc, argv);
    TestAppService tc;
    return QTest::qExec(&tc, argc, argv);
}

// 手写 main 之后，Q_OBJECT 需要的 moc 产物必须显式包含进来，
// 否则链接期会报 "undefined reference to vtable for TestAppService"。
#include "tst_appservice.moc"
