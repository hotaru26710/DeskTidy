#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include "appservice.h"
#include "boxmanager.h"
#include "collector.h"
#include "corenames.h"
#include "deskscanner.h"
#include "undostack.h"

// ---------------------------------------------------------------------------
// e2e_probe —— 端到端验证探针（验收工具，不是产品代码）。
//
// 为什么需要它：单元测试只覆盖了 CoreNames 的纯逻辑，而"搬文件"这条最危险的
// 路径始终没被真实验证过。本探针在一个隔离的临时目录里模拟桌面，
// 走一遍完整流程：创建盒 -> 扫描 -> 收纳 -> 校验 -> 重名避让 -> 撤销 -> 还原冲突。
//
// 目的是在不触碰主人真实桌面的前提下，确认：
//   1) 文件真的被移动了，且内容没损坏；
//   2) 重名时自动加序号而不是覆盖（这是"绝不覆盖"底线的实测）；
//   3) 撤销能把文件原样送回来；
//   4) 还原时若桌面已有同名文件，同样不覆盖。
//
// 运行前请确保 F:/QtProject/DeskTidy/_e2e_tmp 处于干净状态；
// 探针自身不负责搭建初始文件，由调用方脚本准备。
// ---------------------------------------------------------------------------

static int gPass = 0;
static int gFail = 0;

static void check(bool ok, const QString &what)
{
    QTextStream out(stdout);
    if (ok) {
        ++gPass;
        out << "  [PASS] " << what << "\n";
    } else {
        ++gFail;
        out << "  [FAIL] " << what << "\n";
    }
    out.flush();
}

static QString readAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    const QString s = QString::fromUtf8(f.readAll());
    f.close();
    return s.trimmed();
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // 测试根目录刻意放在构建目录之外：放在 build/ 下面会把编译产物
    // 混进"模拟桌面"，让扫描结果失真（这个坑本探针踩过一次）。
    const QString testRoot = QStringLiteral("F:/QtProject/DeskTidy/_e2e_tmp");
    const QString desk     = testRoot + QStringLiteral("/FakeDesktop");
    const QString boxes    = testRoot + QStringLiteral("/Boxes");
    // 公共桌面给一个确定不存在的路径：既验证"不存在则静默跳过"，
    // 也避免空串被 QDir 解析成当前工作目录而误扫。
    const QString noPublic = testRoot + QStringLiteral("/NoSuchPublicDesktop");

    QTextStream out(stdout);
    out << "=== DeskTidy 端到端验证 ===\n\n";

    // ---- 阶段 1：创建收纳盒 ----
    out << "[1] 创建收纳盒\n";
    QString err;
    check(BoxManager::ensureRoot(boxes, &err), QStringLiteral("ensureRoot 创建根目录"));

    StorageBox box;
    check(BoxManager::createBox(boxes, QStringLiteral("临时"), &box, &err),
          QStringLiteral("createBox 创建收纳盒"));
    check(QDir(box.path).exists(), QStringLiteral("盒目录真实存在"));

    StorageBox again;
    check(BoxManager::createBox(boxes, QStringLiteral("临时"), &again, &err),
          QStringLiteral("重复创建同名盒应幂等不报错"));
    check(again.path == box.path, QStringLiteral("幂等返回同一路径"));

    const QList<StorageBox> listed = BoxManager::listBoxes(boxes);
    check(listed.size() == 1,
          QStringLiteral("listBoxes 扫到 1 个盒（实际 %1）").arg(listed.size()));

    // ---- 阶段 2：扫描模拟桌面 ----
    out << "\n[2] 扫描模拟桌面\n";
    const QList<DesktopEntry> entries =
        DeskScanner::scan(desk, noPublic, QStringList(), boxes);
    check(entries.size() == 4,
          QStringLiteral("扫到 4 个条目（实际 %1）").arg(entries.size()));

    // 空路径防线：传空串不应把当前工作目录当桌面扫
    const QList<DesktopEntry> emptyScan =
        DeskScanner::scan(QString(), QString(), QStringList(), boxes);
    check(emptyScan.isEmpty(), QStringLiteral("空路径扫描返回空（不会误扫工作目录）"));

    // ---- 阶段 3：收纳 ----
    out << "\n[3] 执行收纳\n";
    const QList<MoveRecord> records = Collector::collect(entries, box.path);
    int ok = 0;
    for (const MoveRecord &r : records)
        if (r.state == MoveState::Succeeded) ++ok;
    check(ok == 4, QStringLiteral("4 项全部收纳成功（实际 %1）").arg(ok));

    check(QDir(desk).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
          QStringLiteral("模拟桌面已清空"));
    check(QFile::exists(box.path + QStringLiteral("/report.docx")),
          QStringLiteral("报告.docx 已进盒"));
    check(QFile::exists(box.path + QStringLiteral("/photos/pic.txt")),
          QStringLiteral("文件夹随子文件一起搬入（内容未丢）"));

    check(readAll(box.path + QStringLiteral("/report.docx")) == QStringLiteral("alpha"),
          QStringLiteral("报告.docx 内容完好"));
    check(readAll(box.path + QStringLiteral("/data.txt")) == QStringLiteral("beta"),
          QStringLiteral("数据.txt 内容完好"));
    check(readAll(box.path + QStringLiteral("/photos/pic.txt")) == QStringLiteral("inner"),
          QStringLiteral("子文件内容完好"));

    // ---- 阶段 4：重名避让（真实文件系统上的验证）----
    out << "\n[4] 重名避让（绝不覆盖）\n";
    QFile dup(desk + QStringLiteral("/report.docx"));
    if (dup.open(QIODevice::WriteOnly)) {
        dup.write("SECOND");
        dup.close();
    }
    const QList<DesktopEntry> dupEntries =
        DeskScanner::scan(desk, noPublic, QStringList(), boxes);
    check(dupEntries.size() == 1, QStringLiteral("桌面重新出现 1 个同名文件"));

    const QList<MoveRecord> dupRecords = Collector::collect(dupEntries, box.path);
    check(dupRecords.size() == 1 && dupRecords.first().state == MoveState::Succeeded,
          QStringLiteral("重名文件收纳成功"));

    check(QFile::exists(box.path + QStringLiteral("/report (2).docx")),
          QStringLiteral("新文件被改名为report (2).docx"));
    check(readAll(box.path + QStringLiteral("/report.docx")) == QStringLiteral("alpha"),
          QStringLiteral("原 report.docx 未被覆盖（内容仍是 alpha）"));
    check(readAll(box.path + QStringLiteral("/report (2).docx")) == QStringLiteral("SECOND"),
          QStringLiteral("新文件内容正确（SECOND）"));
    check(QFile::exists(box.path + QStringLiteral("/data.txt")),
          QStringLiteral("已有文件未被误伤"));

    // ---- 阶段 5：撤销 ----
    out << "\n[5] 撤销上次收纳\n";
    UndoStack undo;
    undo.push(dupRecords);
    check(undo.canUndo(), QStringLiteral("撤销栈可用"));
    check(undo.undoCount() == 1, QStringLiteral("待撤销 1 项"));

    undo.undo();
    check(QFile::exists(desk + QStringLiteral("/report.docx")),
          QStringLiteral("文件已回到桌面"));
    check(readAll(desk + QStringLiteral("/report.docx")) == QStringLiteral("SECOND"),
          QStringLiteral("还原后内容正确"));
    check(!QFile::exists(box.path + QStringLiteral("/report (2).docx")),
          QStringLiteral("盒内已无该文件"));
    check(!undo.canUndo(), QStringLiteral("撤销成功后栈已清空（防重复撤销）"));

    // ---- 阶段 6：还原时不覆盖桌面上已存在的同名文件 ----
    out << "\n[6] 还原冲突避让\n";
    if (QFile::exists(box.path + QStringLiteral("/data.txt"))) {
        QFile pre(desk + QStringLiteral("/data.txt"));
        if (pre.open(QIODevice::WriteOnly)) {
            pre.write("DESKTOP_VERSION");
            pre.close();
        }
        MoveRecord mr;
        mr.sourcePath = desk + QStringLiteral("/data.txt");
        mr.finalPath  = box.path + QStringLiteral("/data.txt");
        mr.isDir      = false;
        mr.time       = QDateTime::currentDateTime();
        QList<MoveRecord> one;
        one << mr;
        Collector::restore(one);

        check(readAll(desk + QStringLiteral("/data.txt")) == QStringLiteral("DESKTOP_VERSION"),
              QStringLiteral("桌面原有文件未被还原操作覆盖"));
        check(QFile::exists(desk + QStringLiteral("/data (2).txt")),
              QStringLiteral("还原的文件被自动改名为data (2).txt"));
    }

    // ---- 阶段 7：删除收纳盒 ----
    //
    // ⚠️ 本阶段会真的把测试盒目录丢进系统回收站，**无法撤销**。
    //    因此盒名统一带 "e2eDelTest-" 前缀，便于在回收站里一眼认出并手工清理。
    //
    // 为什么这一阶段值得单独测：删盒把三件危险的事串在一起 ——
    //   还原（字段方向搞反就会把文件搬到错误位置）、
    //   重名避让（绝不覆盖）、
    //   回收站兜底（兜不住就等于丢文件）。
    // 任何一环错了，主人的数据都可能落到意想不到的地方。
    out << "\n[7] 删除收纳盒\n";

    AppService service;

    // 用独立的"目标目录"代替真实桌面：删盒会把文件还原到这儿。
    // 探针绝不能碰主人的真实桌面，所以 targetDir 显式传入。
    const QString restoreDir = testRoot + QStringLiteral("/RestoreTarget");
    QDir().mkpath(restoreDir);

    // ---- 前置能力探测：本环境能不能把目录丢进回收站？----
    //
    // 这一步不是走过场。deleteBox 的最后一步是 QFile::moveToTrash，
    // 而它在**受限权限的进程**里会静默返回 false（实测：受限沙箱下
    // F: 与 C: 一律返回 false，其内部文件/目录一切正常）。
    // 若不先探测，"环境不支持回收站"会被误报成一堆产品缺陷，
    // 而真正该关注的还原逻辑（本阶段最危险的部分）反而被淹没在噪声里。
    //
    // 探测方法：真建一个目录试一次。试成的那个顺手留在回收站里；
    // 试不成的自己删掉（它没进回收站，删掉不违反任何底线）。
    bool canTrash = false;
    {
        const QString probeDir = testRoot + QStringLiteral("/trashprobe");
        QDir(probeDir).removeRecursively();
        QDir().mkpath(probeDir);
        canTrash = QFile::moveToTrash(probeDir);
        if (!canTrash)
            QDir(probeDir).removeRecursively();
    }

    if (!canTrash) {
        out << "  [SKIP] 本环境无法把目录移入回收站（权限受限），"
               "删除盒相关的「目录消失」类断言跳过；\n"
               "         还原逻辑仍会完整验证 —— 那才是本阶段最容易出错的部分。\n";
    }

    // --- 7.1 空盒 ---
    // 防的是：空盒被当成"没东西可还原所以失败"。空盒是合法状态，
    // 删除它必须干净成功，且 restored 计数为 0。
    {
        StorageBox emptyBox;
        BoxManager::createBox(boxes, QStringLiteral("e2eDelTest-空盒"), &emptyBox, &err);
        const QString emptyPath = emptyBox.path;
        check(QDir(emptyPath).exists(), QStringLiteral("空盒已建好"));

        const AppService::BoxDeletionResult r = service.deleteBox(emptyPath, restoreDir);
        check(r.restored == 0,
              QStringLiteral("空盒 restored 计数为 0（实际 %1）").arg(r.restored));
        if (canTrash) {
            check(r.ok(), QStringLiteral("空盒删除成功（ok() 为真）"));
            check(!QDir(emptyPath).exists(),
                  QStringLiteral("空盒目录已从原位置消失（进了回收站）"));
        }
    }

    // --- 7.2 有文件的盒：还原方向是否正确 ---
    // 防的是**本功能最大的风险**：MoveRecord 的 sourcePath/finalPath 方向搞反。
    // 搞反了的话文件不会消失，但会被搬到莫名其妙的位置（甚至原地打转），
    // 而 deleteBox 依然可能返回 ok()。
    {
        StorageBox b;
        BoxManager::createBox(boxes, QStringLiteral("e2eDelTest-有文件"), &b, &err);
        const QString bPath = b.path;

        // 直接往盒里写文件（模拟"已经收纳进来的东西"）
        for (const QString &name : {QStringLiteral("one.txt"), QStringLiteral("two.txt")}) {
            QFile f(bPath + QLatin1Char('/') + name);
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                f.write("payload-") ;
                f.write(name.toUtf8());
                f.close();
            }
        }
        check(QDir(bPath).entryList(QDir::Files).size() == 2,
              QStringLiteral("盒内有 2 个文件"));

        const AppService::BoxDeletionResult r = service.deleteBox(bPath, restoreDir);
        check(r.restored == 2,
              QStringLiteral("restored 计数为 2（实际 %1）").arg(r.restored));

        // 关键断言：文件必须出现在**目标目录**里。
        // 这一条直接验出字段方向有没有搞反，且不依赖回收站能力。
        check(QFile::exists(restoreDir + QStringLiteral("/one.txt")),
              QStringLiteral("one.txt 已还原到目标目录"));
        check(QFile::exists(restoreDir + QStringLiteral("/two.txt")),
              QStringLiteral("two.txt 已还原到目标目录"));
        check(readAll(restoreDir + QStringLiteral("/one.txt")) == QStringLiteral("payload-one.txt"),
              QStringLiteral("还原后的文件内容完好"));
        if (canTrash) {
            check(r.ok(), QStringLiteral("有文件的盒删除成功"));
            check(!QDir(bPath).exists(), QStringLiteral("盒目录已消失"));
        }
    }

    // --- 7.3 重名避让：绝不覆盖 ---
    // 防的是：还原时把目标目录里已有的同名文件覆盖掉。
    // 这是"绝不覆盖"铁律在删盒路径上的落点。
    {
        const QString conflictDir = testRoot + QStringLiteral("/ConflictTarget");
        QDir().mkpath(conflictDir);

        // 目标目录里先放一个同名文件，内容可辨认
        {
            QFile pre(conflictDir + QStringLiteral("/same.txt"));
            if (pre.open(QIODevice::WriteOnly | QIODevice::Text)) {
                pre.write("ORIGINAL");
                pre.close();
            }
        }

        StorageBox b;
        BoxManager::createBox(boxes, QStringLiteral("e2eDelTest-重名"), &b, &err);
        const QString bPath = b.path;
        {
            QFile f(bPath + QStringLiteral("/same.txt"));
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                f.write("FROM_BOX");
                f.close();
            }
        }

        const AppService::BoxDeletionResult r = service.deleteBox(bPath, conflictDir);
        check(r.restored == 1, QStringLiteral("重名场景还原 1 项"));

        check(readAll(conflictDir + QStringLiteral("/same.txt")) == QStringLiteral("ORIGINAL"),
              QStringLiteral("目标目录原有文件未被覆盖（内容仍是 ORIGINAL）"));
        check(QFile::exists(conflictDir + QStringLiteral("/same (2).txt")),
              QStringLiteral("还原的文件被自动改名为 same (2).txt"));
        check(readAll(conflictDir + QStringLiteral("/same (2).txt")) == QStringLiteral("FROM_BOX"),
              QStringLiteral("改名后的文件内容正确（FROM_BOX）"));
        if (canTrash) {
            check(r.ok(), QStringLiteral("重名场景删除成功"));
        }
    }

    // --- 7.4 子目录连同内部结构一起还原 ---
    // 防的是：还原只处理顶层文件、把子目录丢了（那目录就会随盒进回收站）。
    {
        const QString subDir = testRoot + QStringLiteral("/SubTarget");
        QDir().mkpath(subDir);

        StorageBox b;
        BoxManager::createBox(boxes, QStringLiteral("e2eDelTest-子目录"), &b, &err);
        const QString bPath = b.path;

        QDir().mkpath(bPath + QStringLiteral("/folder/nested"));
        {
            QFile f(bPath + QStringLiteral("/folder/nested/deep.txt"));
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                f.write("deep-content");
                f.close();
            }
        }

        const AppService::BoxDeletionResult r = service.deleteBox(bPath, subDir);
        check(r.restored == 1,
              QStringLiteral("子目录计为 1 个条目（实际 %1）").arg(r.restored));
        check(QFile::exists(subDir + QStringLiteral("/folder/nested/deep.txt")),
              QStringLiteral("子目录连同内部文件一起还原（结构未丢）"));
        check(readAll(subDir + QStringLiteral("/folder/nested/deep.txt"))
                  == QStringLiteral("deep-content"),
              QStringLiteral("子目录内文件内容完好"));
        if (canTrash) {
            check(r.ok(), QStringLiteral("含子目录的盒删除成功"));
        }
    }

    // --- 7.5 盒目录已不存在：不算失败 ---
    // 防的是：主人先在资源管理器里删了目录、再点"删除收纳盒"，
    // 结果弹一个"找不到该盒"的错误框 —— 那既困惑又没意义。
    {
        const QString ghost = boxes + QStringLiteral("/e2eDelTest-根本不存在");
        const AppService::BoxDeletionResult r = service.deleteBox(ghost, restoreDir);
        check(r.ok(), QStringLiteral("对不存在的盒调用 deleteBox 返回成功（不是错误）"));
        check(r.restored == 0 && r.trashedItems == 0,
              QStringLiteral("不存在的盒两项计数均为 0"));
    }

    // --- 7.6 不发 moveFinished 信号 ---
    // 防的是：删盒触发了标题为"还原到桌面"的失败对话框，
    // 与删盒自己的汇报重复、且主人根本不知道自己在做"还原"。
    {
        int moveFinishedCount = 0;
        QObject::connect(&service, &AppService::moveFinished,
                         [&moveFinishedCount](const QList<MoveRecord> &, const QString &) {
                             ++moveFinishedCount;
                         });

        StorageBox b;
        BoxManager::createBox(boxes, QStringLiteral("e2eDelTest-静默"), &b, &err);
        {
            QFile f(b.path + QStringLiteral("/quiet.txt"));
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                f.write("quiet");
                f.close();
            }
        }

        service.deleteBox(b.path, restoreDir);

        check(moveFinishedCount == 0,
              QStringLiteral("删除盒**没有**发出 moveFinished 信号（实际发了 %1 次）")
                  .arg(moveFinishedCount));
    }

    // --- 汇总 ---
    out << "\n=== 结果：" << gPass << " 通过 / " << gFail << " 失败 ===\n";
    if (canTrash) {
        out << "注意：阶段 7 在系统回收站留下了若干 e2eDelTest-* 目录，需手工清理。\n";
    } else {
        out << "注意：本环境无法访问回收站，阶段 7 未产生回收站残留；\n"
               "      但也因此未验证「盒目录真的进了回收站」这一步。\n";
    }
    out.flush();
    return gFail == 0 ? 0 : 1;
}
