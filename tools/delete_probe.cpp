#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>

#include "appservice.h"
#include "boxmanager.h"
#include "corenames.h"

// ---------------------------------------------------------------------------
// delete_probe —— 删除收纳盒的端到端实机验证。
//
// 为什么单独写一个而不复用单测：单测里用的是 QTemporaryDir，而 deleteBox
// 会把盒目录丢进真实回收站 —— 临时目录收不回来。这里刻意用项目下的隔离
// 目录做，跑完还知道自己收拾。
//
// 验证的是 core 层的 deleteBox（UI 只是在它外面包了确认框，逻辑都在这）。
// ---------------------------------------------------------------------------

static int gPass = 0;
static int gFail = 0;

static void check(bool ok, const QString &what)
{
    QTextStream out(stdout);
    if (ok) { ++gPass; out << "  [PASS] " << what << "\n"; }
    else    { ++gFail; out << "  [FAIL] " << what << "\n"; }
    out.flush();
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    // 用独立组织名，避免碰主人的真实配置。
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyDelProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyDelProbe"));

    const QString root   = QStringLiteral("F:/QtProject/DeskTidy/_del_probe");
    const QString boxDir = root + QStringLiteral("/boxes");
    const QString target = root + QStringLiteral("/target");

    QDir(root).removeRecursively();
    QDir().mkpath(boxDir);
    QDir().mkpath(target);

    out << "=== 删除收纳盒：实机验证 ===\n\n";

    AppService service;

    // ---- 用例 1：有文件的盒 ----
    out << "[1] 有文件的盒：文件应被还原回目标目录\n";
    const QString box1 = boxDir + QStringLiteral("/DeskTidyDelProbe-甲");
    QDir().mkpath(box1);
    for (const QString &n : {QStringLiteral("one.txt"), QStringLiteral("two.txt")}) {
        QFile f(box1 + QLatin1Char('/') + n);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) { out << "    (警告：无法创建 " << n << ")\n"; }
        f.write("payload");
        f.close();
    }

    const AppService::BoxDeletionResult r1 = service.deleteBox(box1, target);
    check(r1.ok(), QStringLiteral("返回成功"));
    check(r1.restored == 2, QStringLiteral("还原 2 项（实际 %1）").arg(r1.restored));
    check(QFile::exists(target + QStringLiteral("/one.txt")),
          QStringLiteral("one.txt 到了目标目录"));
    check(QFile::exists(target + QStringLiteral("/two.txt")),
          QStringLiteral("two.txt 到了目标目录"));
    check(!QDir(box1).exists(), QStringLiteral("盒目录已消失"));

    // ---- 用例 2：重名避让 ----
    out << "\n[2] 目标目录有同名文件时绝不覆盖\n";
    {
        QFile pre(target + QStringLiteral("/same.txt"));
        if (!pre.open(QIODevice::WriteOnly | QIODevice::Text)) { out << "    (警告：无法创建 same.txt)\n"; }
        pre.write("ORIGINAL");
        pre.close();
    }
    const QString box2 = boxDir + QStringLiteral("/DeskTidyDelProbe-乙");
    QDir().mkpath(box2);
    {
        QFile f(box2 + QStringLiteral("/same.txt"));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) { out << "    (警告：无法创建 same.txt)\n"; }
        f.write("FROM_BOX");
        f.close();
    }

    const AppService::BoxDeletionResult r2 = service.deleteBox(box2, target);
    check(r2.ok(), QStringLiteral("返回成功"));
    {
        QFile f(target + QStringLiteral("/same.txt"));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) { out << "    (警告：无法读取 same.txt)\n"; }
        const QString content = QString::fromUtf8(f.readAll());
        f.close();
        check(content == QStringLiteral("ORIGINAL"),
              QStringLiteral("原文件内容未被覆盖（仍是 ORIGINAL）"));
    }
    check(QFile::exists(target + QStringLiteral("/same (2).txt")),
          QStringLiteral("还原的文件被改名为 same (2).txt"));

    // ---- 用例 3：子目录 ----
    out << "\n[3] 子目录应连内部结构一起还原\n";
    const QString box3 = boxDir + QStringLiteral("/DeskTidyDelProbe-丙");
    QDir().mkpath(box3 + QStringLiteral("/photos/nested"));
    {
        QFile f(box3 + QStringLiteral("/photos/nested/deep.txt"));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) { out << "    (警告：无法创建 deep.txt)\n"; }
        f.write("deep");
        f.close();
    }
    const AppService::BoxDeletionResult r3 = service.deleteBox(box3, target);
    check(r3.ok(), QStringLiteral("返回成功"));
    check(QFile::exists(target + QStringLiteral("/photos/nested/deep.txt")),
          QStringLiteral("子目录连同内部文件都到了目标目录"));

    // ---- 用例 4：空盒 ----
    out << "\n[4] 空盒删除\n";
    const QString box4 = boxDir + QStringLiteral("/DeskTidyDelProbe-丁");
    QDir().mkpath(box4);
    const AppService::BoxDeletionResult r4 = service.deleteBox(box4, target);
    check(r4.ok(), QStringLiteral("返回成功"));
    check(r4.restored == 0, QStringLiteral("还原 0 项"));
    check(!QDir(box4).exists(), QStringLiteral("空盒目录已消失"));

    // ---- 用例 5：盒目录不存在 ----
    out << "\n[5] 盒目录不存在时不算失败\n";
    const AppService::BoxDeletionResult r5 =
        service.deleteBox(boxDir + QStringLiteral("/根本没有这个盒"), target);
    check(r5.ok(), QStringLiteral("返回成功（目的已达成，不该报错）"));

    // ---- 用例 6：不发信号 ----
    out << "\n[6] 不发 moveFinished / boxContentsChanged 信号\n";
    int moveFinishedCount = 0;
    int contentsCount = 0;
    QObject::connect(&service, &AppService::moveFinished,
                     [&](const QList<MoveRecord> &, const QString &) { ++moveFinishedCount; });
    QObject::connect(&service, &AppService::boxContentsChanged,
                     [&](const QString &) { ++contentsCount; });

    const QString box6 = boxDir + QStringLiteral("/DeskTidyDelProbe-戊");
    QDir().mkpath(box6);
    {
        QFile f(box6 + QStringLiteral("/x.txt"));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) { out << "    (警告：无法创建 x.txt)\n"; }
        f.write("x");
        f.close();
    }
    service.deleteBox(box6, target);
    check(moveFinishedCount == 0, QStringLiteral("没有发出 moveFinished"));
    check(contentsCount == 0, QStringLiteral("没有发出 boxContentsChanged"));

    // ---- 收尾 ----
    QDir(root).removeRecursively();

    out << "\n=== 结果：" << gPass << " 通过 / " << gFail << " 失败 ===\n";
    out << "注意：回收站里会留下 5 个 DeskTidyDelProbe-* 目录，请自行清理。\n";
    out.flush();
    return gFail == 0 ? 0 : 1;
}
