#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

// ---------------------------------------------------------------------------
// trash_probe2 —— 严格验证「目录能否整体进回收站」。
//
// 第一轮探针给出了"可以用"的结论，但有一处不够严谨：
// 它把 "目录消失" 当成了 "进了回收站" 的证据。而 QFile::moveToTrash
// 对目录若返回 false 且**什么都不做**，目录也该还在；返回 true 却只删掉，
// 则会误判成"进了回收站"。两种失败模式没被区分开。
//
// Qt 官方文档对 Windows 上 moveToTrash 的目录支持表述含糊，社区说法不一，
// 所以这里做严格验证：造一个带多个文件的目录，移走之后**真的去回收站目录里找**。
//
// 全程在隔离目录操作，不碰主人的任何文件。
// ---------------------------------------------------------------------------

static QTextStream *g_out = nullptr;

static void say(const QString &s)
{
    *g_out << s << "\n";
    g_out->flush();
}

// 在回收站里找指定名字的项。回收站的物理路径形如
//   <盘符>:\$RECYCLE.BIN\<SID>\
// 但里面存的是被改过的名字（$R 开头的文件、$I 开头的元数据），
// 所以按名字直接找不可靠 —— 改为统计目录数量变化。
static int countRecycleEntries(const QString &drive)
{
    QDir bin(drive + QStringLiteral(":/$RECYCLE.BIN"));
    if (!bin.exists())
        return -1;

    int total = 0;
    // 每个用户 SID 一个子目录
    for (const QFileInfo &sid : bin.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot
                                                      | QDir::Hidden)) {
        QDir sidDir(sid.absoluteFilePath());
        // $R 开头的才是真实内容
        for (const QFileInfo &e : sidDir.entryInfoList(QDir::AllEntries
                                                           | QDir::NoDotAndDotDot
                                                           | QDir::Hidden)) {
            if (e.fileName().startsWith(QStringLiteral("$R")))
                ++total;
        }
    }
    return total;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    g_out = &out;

    say(QStringLiteral("=== 目录进回收站：严格验证 ===\n"));

    // 用 F 盘（项目所在盘）做实验
    const QString drive = QStringLiteral("F");
    const QString testRoot = QStringLiteral("F:/QtProject/DeskTidy/_trash_probe2");
    QDir(testRoot).removeRecursively();
    QDir().mkpath(testRoot);

    const int before = countRecycleEntries(drive);
    say(QStringLiteral("回收站当前条目数：%1").arg(before));

    // ---- 用例 A：单文件 ----
    {
        const QString f = testRoot + QStringLiteral("/single.txt");
        QFile file(f);
        file.open(QIODevice::WriteOnly | QIODevice::Text);
        file.write("single");
        file.close();

        QString inTrash;
        const bool ok = QFile::moveToTrash(f, &inTrash);
        const int after = countRecycleEntries(drive);
        say(QStringLiteral("\n[A] 单文件"));
        say(QStringLiteral("    返回值=%1  原位置还在=%2  回收站增量=%3")
                .arg(ok ? QStringLiteral("true") : QStringLiteral("false"),
                     QFile::exists(f) ? QStringLiteral("是") : QStringLiteral("否"),
                     QString::number(after - before)));
        say(QStringLiteral("    回收站内路径=%1")
                .arg(inTrash.isEmpty() ? QStringLiteral("(空)") : inTrash));
    }

    // ---- 用例 B：带多个文件的目录（关键用例）----
    const int beforeDir = countRecycleEntries(drive);
    {
        const QString d = testRoot + QStringLiteral("/box_dir");
        QDir().mkpath(d);
        for (int i = 0; i < 3; ++i) {
            QFile f(d + QStringLiteral("/file%1.txt").arg(i));
            f.open(QIODevice::WriteOnly | QIODevice::Text);
            f.write("content");
            f.close();
        }
        // 再加一层子目录，验证递归
        QDir().mkpath(d + QStringLiteral("/nested"));
        QFile nf(d + QStringLiteral("/nested/deep.txt"));
        nf.open(QIODevice::WriteOnly | QIODevice::Text);
        nf.write("deep");
        nf.close();

        say(QStringLiteral("\n[B] 带 3 个文件 + 1 层子目录的目录"));
        say(QStringLiteral("    移动前：目录存在=%1  内部条目=%2")
                .arg(QDir(d).exists() ? QStringLiteral("是") : QStringLiteral("否"))
                .arg(QDir(d).entryList(QDir::AllEntries | QDir::NoDotAndDotDot
                                       | QDir::Hidden).size()));

        QString inTrash;
        const bool ok = QFile::moveToTrash(d, &inTrash);
        const int afterDir = countRecycleEntries(drive);

        say(QStringLiteral("    返回值=%1").arg(ok ? QStringLiteral("true") : QStringLiteral("false")));
        say(QStringLiteral("    原位置还在=%1")
                .arg(QDir(d).exists() ? QStringLiteral("是") : QStringLiteral("否")));
        say(QStringLiteral("    回收站增量=%1").arg(afterDir - beforeDir));
        say(QStringLiteral("    回收站内路径=%1")
                .arg(inTrash.isEmpty() ? QStringLiteral("(空)") : inTrash));

        // 判定：三种可能
        say(QStringLiteral("\n    >>> 判定："));
        if (ok && !QDir(d).exists() && afterDir > beforeDir) {
            say(QStringLiteral("        SUPPORTED —— 目录整体进回收站，含子文件。"
                               "可直接用 QFile::moveToTrash。"));
        } else if (!ok && QDir(d).exists()) {
            say(QStringLiteral("        NOT-SUPPORTED —— 返回 false 且什么都没做。"
                               "需要改用 SHFileOperationW + FOF_ALLOWUNDO。"));
        } else if (ok && !QDir(d).exists() && afterDir <= beforeDir) {
            say(QStringLiteral("        DANGER —— 返回 true、目录消失，但回收站没增加。"
                               "可能是永久删除，绝不能用于用户文件！"));
        } else {
            say(QStringLiteral("        UNCLEAR —— 行为不符预期，需人工检查。"));
        }
    }

    // ---- 收尾 ----
    QDir(testRoot).removeRecursively();
    say(QStringLiteral("\n=== 实验结束 ==="));
    return 0;
}
