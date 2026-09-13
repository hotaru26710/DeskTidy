#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>

// ---------------------------------------------------------------------------
// trash_probe —— 验证 QFile::moveToTrash 在本机可用。
//
// 为什么要先验：删收纳盒的兜底方案依赖"把整个盒目录丢进回收站"。
// 如果这个 API 在本机不可用（某些精简系统、网络驱动器、或回收站被禁用），
// 整个兜底设计就得换一套。写产品代码前先确认它能工作。
//
// 只在隔离目录里操作，不碰主人的任何文件。
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    out << "=== QFile::moveToTrash 可用性验证 ===\n\n";

    out << "supportsMoveToTrash(): "
        << (QFile::supportsMoveToTrash() ? "true" : "false") << "\n\n";

    // 造一个隔离的测试文件
    const QString dir = QStringLiteral("F:/QtProject/DeskTidy/_trash_probe");
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);

    const QString testFile = dir + QStringLiteral("/should_be_trashed.txt");
    {
        QFile f(testFile);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            out << "无法创建测试文件，实验中止\n";
            return 1;
        }
        f.write("this file is expected to end up in the recycle bin\n");
        f.close();
    }

    out << "测试文件已创建: " << testFile << "\n";
    out << "移动前存在: " << (QFile::exists(testFile) ? "是" : "否") << "\n\n";

    QString pathInTrash;
    const bool ok = QFile::moveToTrash(testFile, &pathInTrash);

    out << "moveToTrash 返回值: " << (ok ? "true" : "false") << "\n";
    out << "回收站内路径: " << (pathInTrash.isEmpty() ? "(未提供)" : pathInTrash) << "\n";
    out << "移动后原位置还在: " << (QFile::exists(testFile) ? "是" : "否") << "\n\n";

    if (ok && !QFile::exists(testFile)) {
        out << ">>> 结论：可用。删除收纳盒的兜底方案成立。\n";
    } else {
        out << ">>> 结论：不可用或行为异常，兜底方案需要换。\n";
    }

    // 顺手验证目录也能进回收站（删盒要丢的是整个目录）
    out << "\n--- 追加验证：整个目录能否丢进回收站 ---\n";
    const QString subDir = dir + QStringLiteral("/a_box_dir");
    QDir().mkpath(subDir);
    QFile inner(subDir + QStringLiteral("/inner.txt"));
    if (inner.open(QIODevice::WriteOnly | QIODevice::Text)) {
        inner.write("inner content");
        inner.close();
    }

    QString dirInTrash;
    const bool dirOk = QFile::moveToTrash(subDir, &dirInTrash);
    out << "目录存在: " << (QDir(subDir).exists() ? "是" : "否") << "\n";
    out << "目录 moveToTrash 返回值: " << (dirOk ? "true" : "false") << "\n";
    out << "回收站内路径: " << dirInTrash << "\n";

    if (dirOk && !QDir(subDir).exists()) {
        out << ">>> 结论：目录也能整体进回收站（含子文件）。\n";
    } else {
        out << ">>> 注意：目录进回收站的行为与文件不同，需要另行处理。\n";
    }

    // 清理隔离目录
    QDir(dir).removeRecursively();

    out << "\n=== 实验结束 ===\n";
    out.flush();
    return 0;
}
