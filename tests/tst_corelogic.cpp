#include <QtTest/QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "corenames.h"

// ---------------------------------------------------------------------------
// tst_corelogic —— DeskTidy 核心纯逻辑单测。
//
// 只测 CoreNames：它是整个工具里唯一"看起来简单、实际最容易出隐蔽 Bug"的一层，
// 而且完全不碰文件系统之外的东西，测起来零成本。
//
// 之所以不测 Collector 的真实移动：那会真的搬动主人磁盘上的文件，
// 性价比和安全性都不划算 —— 它的正确性由构建后的人工验收覆盖。
//
// 三个被测点对应的真实风险：
//   * uniqueTargetPath —— "绝不覆盖"这条底线的唯一保证，序号算错就等于毁文件；
//   * sanitizeBoxName  —— 盒名非法会导致建目录失败或产生诡异路径；
//   * isPathInside     —— 判错会让收纳盒把自己吞进去，或误伤用户文件。
// ---------------------------------------------------------------------------

class TestCoreLogic : public QObject
{
    Q_OBJECT

private slots:

    // --- 路径推导：只要求返回非空且合理，不硬编码具体盘符 ---

    void desktopRootIsUsable()
    {
        const QString p = CoreNames::desktopRoot();
        QVERIFY2(!p.isEmpty(), "桌面路径不应为空");
        QVERIFY2(QDir::isAbsolutePath(p), "桌面路径应为绝对路径");
    }

    void publicDesktopRootIsUsable()
    {
        const QString p = CoreNames::publicDesktopRoot();
        QVERIFY2(!p.isEmpty(), "公共桌面路径不应为空");
        QVERIFY2(QDir::isAbsolutePath(p), "公共桌面路径应为绝对路径");
    }

    void boxRootIsUnderHomeAndNotCreated()
    {
        const QString p = CoreNames::boxRoot();
        QVERIFY2(!p.isEmpty(), "收纳盒根路径不应为空");
        QVERIFY2(p.endsWith(QStringLiteral("DeskTidy")),
                 "收纳盒根目录名应为 DeskTidy");

        // boxRoot 只做拼接，不应有创建目录的副作用。
        // 这里只断言"主页之下"，不断言它不存在 —— 因为它可能真的已被程序创建过。
        QVERIFY2(QDir::cleanPath(p).startsWith(QDir::cleanPath(QDir::homePath()),
                                               Qt::CaseInsensitive),
                 "收纳盒根目录应位于用户主目录之下");
    }

    // --- uniqueTargetPath：本套测试的重头戏 ---

    void uniqueTargetReturnsOriginalWhenFree()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        const QString dst = CoreNames::uniqueTargetPath(tmp.path(),
                                                       QStringLiteral("报告.docx"));
        QCOMPARE(QDir::cleanPath(dst),
                 QDir::cleanPath(tmp.path() + QStringLiteral("/报告.docx")));
    }

    void uniqueTargetInsertsIndexBeforeExtension()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString dir = tmp.path();

        // 占位：报告.docx
        QFile f(dir + QStringLiteral("/报告.docx"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();

        const QString dst = CoreNames::uniqueTargetPath(dir, QStringLiteral("报告.docx"));
        QCOMPARE(QFileInfo(dst).fileName(), QStringLiteral("报告 (2).docx"));
    }

    void uniqueTargetAdvancesIndexMonotonically()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString dir = tmp.path();

        QFile f(dir + QStringLiteral("/报告.docx"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();

        // 序号必须逐个递进：(2) -> (3) -> (4)
        QCOMPARE(QFileInfo(CoreNames::uniqueTargetPath(dir, QStringLiteral("报告.docx"))).fileName(),
                 QStringLiteral("报告 (2).docx"));

        QFile f2(dir + QStringLiteral("/报告 (2).docx"));
        QVERIFY(f2.open(QIODevice::WriteOnly));
        f2.close();

        QCOMPARE(QFileInfo(CoreNames::uniqueTargetPath(dir, QStringLiteral("报告.docx"))).fileName(),
                 QStringLiteral("报告 (3).docx"));

        QFile f3(dir + QStringLiteral("/报告 (3).docx"));
        QVERIFY(f3.open(QIODevice::WriteOnly));
        f3.close();

        QCOMPARE(QFileInfo(CoreNames::uniqueTargetPath(dir, QStringLiteral("报告.docx"))).fileName(),
                 QStringLiteral("报告 (4).docx"));
    }

    void uniqueTargetHandlesNameWithoutExtension()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString dir = tmp.path();

        QFile f(dir + QStringLiteral("/素材"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();

        QCOMPARE(QFileInfo(CoreNames::uniqueTargetPath(dir, QStringLiteral("素材"))).fileName(),
                 QStringLiteral("素材 (2)"));
    }

    void uniqueTargetTreatsLeadingDotAsStemNotExtension()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString dir = tmp.path();

        // .gitignore 这类点开头的名字不能被当成"扩展名是 gitignore、主干为空"
        QFile f(dir + QStringLiteral("/.gitignore"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();

        QCOMPARE(QFileInfo(CoreNames::uniqueTargetPath(dir, QStringLiteral(".gitignore"))).fileName(),
                 QStringLiteral(".gitignore (2)"));
    }

    void uniqueTargetNeverReturnsExistingPath()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString dir = tmp.path();

        // 连做 5 轮：每轮取一个目标路径、真的把它创建出来，
        // 下一轮必须绕开所有已存在的候选。这是"绝不覆盖"底线的直接验证。
        for (int i = 0; i < 5; ++i) {
            const QString dst = CoreNames::uniqueTargetPath(dir, QStringLiteral("数据.txt"));
            QVERIFY2(!QFile::exists(dst),
                     qPrintable(QStringLiteral("返回了已存在的路径: %1").arg(dst)));

            QFile f(dst);
            QVERIFY2(f.open(QIODevice::WriteOnly),
                     qPrintable(QStringLiteral("无法创建: %1").arg(dst)));
            f.close();
        }

        // 5 轮下来应产生 1 个原名 + 4 个带序号的名字
        const int count = QDir(dir).entryList(QDir::Files).size();
        QCOMPARE(count, 5);
    }

    // --- sanitizeBoxName ---

    void sanitizeStripsIllegalCharacters()
    {
        const QString s = CoreNames::sanitizeBoxName(QStringLiteral("工/作:区*域?"));
        QVERIFY2(!s.contains(QLatin1Char('/')), "不应残留 /");
        QVERIFY2(!s.contains(QLatin1Char(':')), "不应残留 :");
        QVERIFY2(!s.contains(QLatin1Char('*')), "不应残留 *");
        QVERIFY2(!s.contains(QLatin1Char('?')), "不应残留 ?");
        QVERIFY2(!s.isEmpty(), "清洗后不应为空");
    }

    void sanitizeHandlesBackslashAndQuotes()
    {
        const QString s = CoreNames::sanitizeBoxName(QStringLiteral("a\\b\"c<d>e|f"));
        QVERIFY2(!s.contains(QLatin1Char('\\')), "不应残留反斜杠");
        QVERIFY2(!s.contains(QLatin1Char('"')),  "不应残留双引号");
        QVERIFY2(!s.contains(QLatin1Char('<')),  "不应残留 <");
        QVERIFY2(!s.contains(QLatin1Char('>' )), "不应残留 >");
        QVERIFY2(!s.contains(QLatin1Char('|')),  "不应残留 |");
    }

    void sanitizeTrimsWhitespaceAndTrailingDots()
    {
        const QString s = CoreNames::sanitizeBoxName(QStringLiteral("  临时..." ));
        QCOMPARE(s, QStringLiteral("临时"));
    }

    void sanitizeEmptyInputFallsBack()
    {
        QCOMPARE(CoreNames::sanitizeBoxName(QString()), QStringLiteral("收纳盒"));
        QCOMPARE(CoreNames::sanitizeBoxName(QStringLiteral("   ")), QStringLiteral("收纳盒"));
        // 全是非法字符时，清洗完也应回退而不是返回空串
        QCOMPARE(CoreNames::sanitizeBoxName(QStringLiteral("///")), QStringLiteral("收纳盒"));
    }

    void sanitizeAvoidsReservedDeviceNames()
    {
        // CON/NUL 在 Windows 上不能作为目录名，必须被改写
        const QString con = CoreNames::sanitizeBoxName(QStringLiteral("CON"));
        QVERIFY2(con.compare(QStringLiteral("CON"), Qt::CaseInsensitive) != 0,
                 "CON 应被改写");
        QVERIFY2(!con.isEmpty(), "改写后不应为空");

        const QString nul = CoreNames::sanitizeBoxName(QStringLiteral("nul"));
        QVERIFY2(nul.compare(QStringLiteral("nul"), Qt::CaseInsensitive) != 0,
                 "nul 应被改写");
    }

    void sanitizeKeepsOrdinaryChineseNameIntact()
    {
        // 正常名字不该被乱改，这是最容易过度清洗的地方
        QCOMPARE(CoreNames::sanitizeBoxName(QStringLiteral("工作")), QStringLiteral("工作"));
        QCOMPARE(CoreNames::sanitizeBoxName(QStringLiteral("临时文件")), QStringLiteral("临时文件"));
    }

    // --- isPathInside ---

    void isPathInsideDetectsEqualsAndChildren()
    {
        QVERIFY(CoreNames::isPathInside(QStringLiteral("C:/a"), QStringLiteral("C:/a")));
        QVERIFY(CoreNames::isPathInside(QStringLiteral("C:/a/b"), QStringLiteral("C:/a")));
        QVERIFY(CoreNames::isPathInside(QStringLiteral("C:/a/b/c"), QStringLiteral("C:/a")));
    }

    void isPathInsideRejectsSiblingWithSharedPrefix()
    {
        // 这条是最经典的坑：C:/ab 不是 C:/a 的子路径
        QVERIFY2(!CoreNames::isPathInside(QStringLiteral("C:/ab"), QStringLiteral("C:/a")),
                 "C:/ab 不应被判为 C:/a 的子路径");
        QVERIFY2(!CoreNames::isPathInside(QStringLiteral("C:/abc/d"), QStringLiteral("C:/a")),
                 "C:/abc/d 不应被判为 C:/a 的子路径");
    }

    void isPathInsideIsCaseInsensitiveAndSeparatorAgnostic()
    {
        // Windows 上大小写不敏感
        QVERIFY(CoreNames::isPathInside(QStringLiteral("C:/USERS/x"), QStringLiteral("c:/users")));
        // 分隔符混用、结尾多余斜杠都要能处理
        QVERIFY(CoreNames::isPathInside(QStringLiteral("C:\\a\\b"), QStringLiteral("C:/a/")));
    }

    void isPathInsideRejectsUnrelatedPaths()
    {
        QVERIFY(!CoreNames::isPathInside(QStringLiteral("C:/a"), QStringLiteral("C:/b")));
        QVERIFY(!CoreNames::isPathInside(QStringLiteral("D:/a"), QStringLiteral("C:/a")));
        // 反向：父不是子的子
        QVERIFY(!CoreNames::isPathInside(QStringLiteral("C:/a"), QStringLiteral("C:/a/b")));
    }
};

QTEST_MAIN(TestCoreLogic)
#include "tst_corelogic.moc"
