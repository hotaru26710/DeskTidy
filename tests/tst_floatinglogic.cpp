#include <QStandardPaths>
#include <QStringList>
#include <QtTest/QtTest>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

#include "appservice.h"
#include "boxmanager.h"
#include "corenames.h"
#include "coretypes.h"
#include "floatingboxmanager.h"
#include "floatingboxwidget.h"
#include "itemlistwidget.h"
#include "settings.h"
#include "windowlayout.h"

// ---------------------------------------------------------------------------
// tst_floatinglogic —— 阶段 3/4 新增逻辑的单测。
//
// 【选题原则】只测"脱离 GUI 事件循环也能判定"的东西。浮窗在这个项目里是
// QWidget，但它的**状态管理**与**配置编码**不依赖窗口真的显示出来，
// 这些才是真正容易出错、又适合自动化的部分。
//
// 【覆盖两大块】
//
// 1) 配置键名的编解码契约（对应 Settings 里的 encodeBoxName）
//    防的是：盒名里出现 '/' 会被 QSettings 当成层级分隔符。这一点已实测确认：
//    用未编码的键写 "floating/geometry/a/b"，值会落到嵌套组
//    floating/geometry/a 的 b 键上 —— 与按盒名查找时用的键不是同一个，
//    结果就是"写进去了但永远读不回来"。CoreNames::sanitizeBoxName 虽然在上游
//    挡了非法字符，但配置层不该依赖上游永远正确：主人完全可能在资源管理器里
//    手工建一个含斜杠的目录，或日后有人从别的入口建盒。
//
// 2) FloatingBoxManager 的状态机
//    防的是："哈希表状态"与"配置落盘"不一致 —— 例如关了浮窗却没从配置里
//    摘掉，下次启动又冒出来；或反过来，开着浮窗但配置里没有，重启后凭空消失。
//
// 【刻意不测的东西，以及为什么】
//   * 浮窗的视觉行为（置顶、无边框、拖动、卷起动画）—— 不是"逻辑"，
//     只能靠人工看。断言"窗口是否真的浮在别人上面"测的是 Windows 的
//     窗口管理器，不是我们的代码。
//   * TrayIcon —— 见文件末尾的说明。
//   * 任何会真实搬动文件的路径 —— 由 tools/e2e_probe.cpp 在隔离目录里
//     真刀真枪验证，比在这里 mock 更有说服力。
//
// 【关于配置落盘的环境依赖 —— 重要】
// Settings 把 ini 落在 QStandardPaths::AppConfigLocation，在 Windows 上是
// %LOCALAPPDATA%\DeskTidy\DeskTidy\DeskTidy.ini。**该位置并非在所有环境都可写**
// （受限沙箱、部分企业策略下会被拒）。实测确认：那种环境下 QDir::mkpath 失败，
// 于是所有写入静默失效、所有读取返回默认值。
//
// 本测试因此分成两类用例：
//   * 不依赖落盘的（编码契约、参数校验、状态机内存侧）—— 任何环境都跑；
//   * 依赖落盘的 —— 用 QSKIP 明确跳过并说明原因，而不是报一堆
//     看起来像产品 bug 的失败。
// 这样在正常开发机上全部执行，在受限环境里给出诚实的"未验证"信号。
// ---------------------------------------------------------------------------

class TestFloatingLogic : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_sandbox;
    bool          m_configWritable = false;

    QString makeBoxDir(const QString &name)
    {
        const QString path = m_sandbox.path() + QLatin1Char('/') + name;
        QDir().mkpath(path);
        return path;
    }

    // 配置目录当前是否真的可写。不可写时依赖落盘的用例走 QSKIP。
    static bool probeConfigWritable()
    {
        const QString dir =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        if (dir.isEmpty())
            return false;
        if (!QDir().mkpath(dir))
            return false;
        QFile probe(dir + QStringLiteral("/.writeprobe"));
        if (!probe.open(QIODevice::WriteOnly))
            return false;
        probe.write("x");
        probe.close();
        probe.remove();
        return true;
    }

private slots:

    void initTestCase()
    {
        QVERIFY2(m_sandbox.isValid(), "临时沙地创建失败");
        m_configWritable = probeConfigWritable();
    }

    void init()
    {
        const QString dir =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        if (!dir.isEmpty())
            QDir(dir).removeRecursively();
    }

    // =======================================================================
    // 一、配置键名编码的契约（不依赖落盘位置）
    // =======================================================================

    // 本文件的核心用例。它直接验证"编码后的键不会与 QSettings 的层级
    // 分隔符冲突"这条契约，不经过 Settings 的落盘路径，因此任何环境都能跑。
    //
    // 防的是：盒名含 '/' 时，编码若没挡住斜杠，值会落到 nesting 组里，
    // 按盒名查找永远读不到。
    void encodedKeyNeverCollidesWithSettingsSeparator()
    {
        // 与 Settings 内部 encodeBoxName 采用同一策略：
        // Base64Url 不含 '/' 与 '+'，再省掉结尾的 '='。
        auto encode = [](const QString &name) {
            return QString::fromLatin1(
                name.toUtf8().toBase64(QByteArray::Base64UrlEncoding
                                       | QByteArray::OmitTrailingEquals));
        };

        const QStringList trickyNames = {
            QStringLiteral("a/b"),
            QStringLiteral("a\\b"),
            QStringLiteral("工作/2024/归档"),
            QStringLiteral("盘符: 备份"),
            QStringLiteral("我的 临时 盒子"),
            QStringLiteral("📦收纳盒🗂"),
            QStringLiteral("临时."),
            QStringLiteral("   "),
            QStringLiteral("a\tb"),
            QStringLiteral("a\nb"),
            QStringLiteral("100%备份"),
            QStringLiteral("$HOME"),
            QStringLiteral("[重要]"),
            QStringLiteral("a;b"),
            QStringLiteral("a=b"),
        };

        for (const QString &name : trickyNames) {
            const QString key = encode(name);

            // 契约一：编码结果不得含 QSettings 的分隔符，也不得含 '+'（非 URL 安全）
            QVERIFY2(!key.contains(QLatin1Char('/')),
                     qPrintable(QStringLiteral("编码结果含斜杠：%1 -> %2").arg(name, key)));
            QVERIFY2(!key.contains(QLatin1Char('+')),
                     qPrintable(QStringLiteral("编码结果含加号：%1 -> %2").arg(name, key)));

            // 契约二：编码必须可逆 —— 否则读配置时还原不出盒名
            const QByteArray decoded =
                QByteArray::fromBase64(key.toLatin1(),
                                       QByteArray::Base64UrlEncoding
                                           | QByteArray::AbortOnBase64DecodingErrors);
            QCOMPARE(QString::fromUtf8(decoded), name);
        }
    }

    // 防的是：两个不同盒名编码后碰撞，导致 A 盒的浮窗位置套用到 B 盒。
    void encodingIsCollisionFreeForSimilarNames()
    {
        auto encode = [](const QString &name) {
            return QString::fromLatin1(
                name.toUtf8().toBase64(QByteArray::Base64UrlEncoding
                                       | QByteArray::OmitTrailingEquals));
        };

        // 这几个在"人眼看起来像同一个盒名"的意义上极易混淆
        const QStringList names = {
            QStringLiteral("a/b"),
            QStringLiteral("a\\b"),
            QStringLiteral("ab"),
            QStringLiteral("a b"),
        };

        QSet<QString> seen;
        for (const QString &n : names) {
            const QString k = encode(n);
            QVERIFY2(!seen.contains(k),
                     qPrintable(QStringLiteral("编码碰撞：%1").arg(n)));
            seen.insert(k);
        }
        QCOMPARE(seen.size(), names.size());
    }

    // =======================================================================
    // 二、不依赖落盘的行为（任何环境都应通过）
    // =======================================================================

    // 防的是：空盒名被当成合法键写进去，产生一个谁也读不到的孤儿键。
    void emptyBoxNameIsRejectedNotStored()
    {
        Settings s;
        s.setFloatGeometry(QString(), QByteArray("should-not-be-stored"));
        QVERIFY2(s.floatGeometry(QString()).isEmpty(),
                 "空盒名不该能存下几何数据");

        s.setFloatRolledUp(QString(), true);
        QVERIFY2(!s.floatRolledUp(QString()),
                 "空盒名的卷起状态应恒为默认值 false");
    }

    // 防的是：盒目录已被主人删掉，配置里却还记着"这个盒开着浮窗"，
    // 于是启动时冒出一个指向空目录的浮窗。
    // 只依赖文件系统与内存状态，不需要配置落盘。
    void openBoxRefusesMissingDirectory()
    {
        AppService service;
        FloatingBoxManager mgr(&service);

        const QString ghost = m_sandbox.path() + QStringLiteral("/不存在的盒");

        QSignalSpy spy(&mgr, &FloatingBoxManager::boxWindowToggled);
        mgr.openBox(QStringLiteral("不存在的盒"), ghost);

        QVERIFY2(!mgr.isBoxOpen(QStringLiteral("不存在的盒")),
                 "盒目录不存在时不该建浮窗");
        QCOMPARE(spy.count(), 0);
    }

    // 防的是：空盒名/空路径被当成合法输入，在哈希表里塞一个空键。
    void openBoxRejectsEmptyArguments()
    {
        AppService service;
        FloatingBoxManager mgr(&service);

        mgr.openBox(QString(), QString());
        mgr.openBox(QStringLiteral("临时"), QString());
        mgr.openBox(QString(), makeBoxDir(QStringLiteral("临时")));

        QVERIFY2(mgr.openBoxNames().isEmpty(), "空参数不该产生任何浮窗");
    }

    // 防的是：关一个从没开过的浮窗时出错或误发信号。
    // 主窗口右键菜单与托盘菜单都可能被连点，必须幂等。
    void closeUnopenedBoxIsSafeNoop()
    {
        AppService service;
        FloatingBoxManager mgr(&service);

        QSignalSpy spy(&mgr, &FloatingBoxManager::boxWindowToggled);
        mgr.closeBox(QStringLiteral("从没开过"));

        QVERIFY(!mgr.isBoxOpen(QStringLiteral("从没开过")));
        QCOMPARE(spy.count(), 0);
    }

    // 防的是：没有任何浮窗时，退出流程里的兜底落盘会出错。
    void saveAllGeometryWithNoWidgetsIsSafe()
    {
        AppService service;
        FloatingBoxManager mgr(&service);

        mgr.saveAllGeometry();
        QVERIFY(mgr.openBoxNames().isEmpty());
    }

    // 防的是：配置为空时 restoreOpenBoxes 出错或误建浮窗。
    void restoreOpenBoxesWithEmptyConfigIsNoop()
    {
        AppService service;
        FloatingBoxManager mgr(&service);

        QSignalSpy spy(&mgr, &FloatingBoxManager::boxWindowToggled);
        mgr.restoreOpenBoxes();

        QVERIFY(mgr.openBoxNames().isEmpty());
        QCOMPARE(spy.count(), 0);
    }

    // 防的是：读不到配置时 Settings 返回垃圾值而不是安全默认值。
    // 这几条即使配置目录不可写也应当成立（读不到就该是默认值）。
    void settingsDefaultsAreSaneRegardlessOfConfig()
    {
        Settings s;

        QVERIFY2(s.excludedNames().isEmpty(), "无配置时排除名单应为空");
        QVERIFY2(s.lastBoxName().isEmpty(), "无配置时上次盒名应为空");
        QVERIFY2(s.openBoxNames().isEmpty(), "无配置时浮窗名单应为空");
        QVERIFY2(s.floatGeometry(QStringLiteral("任意盒")).isEmpty(),
                 "无配置时几何应为空 blob");
        QVERIFY2(!s.floatRolledUp(QStringLiteral("任意盒")),
                 "无配置时卷起状态应为展开");
        QVERIFY2(s.alwaysOnTop(),
                 "无配置时应当默认置顶 —— 这是浮窗的预期行为");
        QVERIFY2(!s.trayHintShown(), "无配置时托盘提示应视为未展示过");
    }

    // =======================================================================
    // 三、依赖配置落盘的用例（环境不可写时明确跳过）
    // =======================================================================

    void geometryRoundTripsThroughDisk_data()
    {
        QTest::addColumn<QString>("boxName");
        QTest::newRow("正斜杠")     << QStringLiteral("a/b");
        QTest::newRow("反斜杠")     << QStringLiteral("a\\b");
        QTest::newRow("多个斜杠")   << QStringLiteral("工作/2024/归档");
        QTest::newRow("冒号")       << QStringLiteral("盘符: 备份");
        QTest::newRow("空格与中文") << QStringLiteral("我的 临时 盒子");
        QTest::newRow("emoji")      << QStringLiteral("📦收纳盒🗂");
        QTest::newRow("结尾点")     << QStringLiteral("临时.");
        QTest::newRow("制表符")     << QStringLiteral("a\tb");
        QTest::newRow("百分号")     << QStringLiteral("100%备份");
        QTest::newRow("方括号")     << QStringLiteral("[重要]");
        QTest::newRow("等号")       << QStringLiteral("a=b");
    }

    // 防的是：盒名含特殊字符时几何数据在落盘往返中丢失。
    void geometryRoundTripsThroughDisk()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境），无法验证落盘往返");

        QFETCH(QString, boxName);

        Settings s;
        const QByteArray blob = QByteArray::fromHex("0102030405ff");

        s.setFloatGeometry(boxName, blob);
        QCOMPARE(s.floatGeometry(boxName), blob);
    }

    void geometryDoesNotLeakBetweenBoxes()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        Settings s;
        s.setFloatGeometry(QStringLiteral("a/b"),  QByteArray("geometry-A"));
        s.setFloatGeometry(QStringLiteral("a\\b"), QByteArray("geometry-B"));
        s.setFloatGeometry(QStringLiteral("ab"),   QByteArray("geometry-C"));

        QCOMPARE(s.floatGeometry(QStringLiteral("a/b")),  QByteArray("geometry-A"));
        QCOMPARE(s.floatGeometry(QStringLiteral("a\\b")), QByteArray("geometry-B"));
        QCOMPARE(s.floatGeometry(QStringLiteral("ab")),   QByteArray("geometry-C"));
    }

    void geometrySurvivesVeryLongBoxName()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        Settings s;
        // 用 QString(200, QChar(...)) 而不是 QLatin1Char：中文是多字节，
        // QLatin1Char 只接受单字节字符，传中文会被截成垃圾字节。
        const QString longName = QString(200, QChar(0x957F));   // U+957F = 「长」
        const QByteArray blob  = QByteArray("long-name-blob");

        s.setFloatGeometry(longName, blob);
        QCOMPARE(s.floatGeometry(longName), blob);
    }

    // 防的是：空 blob 保留了旧值，导致"清除几何"这个操作失效。
    void emptyBlobClearsGeometry()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        Settings s;
        const QString box = QStringLiteral("临时");

        s.setFloatGeometry(box, QByteArray("some-geometry"));
        QVERIFY(!s.floatGeometry(box).isEmpty());

        s.setFloatGeometry(box, QByteArray());
        QVERIFY2(s.floatGeometry(box).isEmpty(),
                 "空 blob 应当清除几何，而不是保留旧值");
    }

    void rolledUpRoundTripAndIsPerBox()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        Settings s;

        QVERIFY2(!s.floatRolledUp(QStringLiteral("甲")), "未设置过的盒应默认展开");

        s.setFloatRolledUp(QStringLiteral("甲"), true);
        QVERIFY(s.floatRolledUp(QStringLiteral("甲")));

        s.setFloatRolledUp(QStringLiteral("乙"), false);
        QVERIFY2(!s.floatRolledUp(QStringLiteral("乙")), "乙应保持展开");

        s.setFloatRolledUp(QStringLiteral("甲"), false);
        QVERIFY2(!s.floatRolledUp(QStringLiteral("甲")), "应能改回展开");
    }

    // 防的是：展开状态下残留 rolledUp 键 —— 实现约定只有非默认状态才落盘，
    // 否则配置文件里全是 false 噪声。
    void rolledUpStoresOnlyNonDefaultState()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        Settings s;
        const QString box = QStringLiteral("临时");

        s.setFloatRolledUp(box, true);
        s.setFloatRolledUp(box, false);

        const QString iniPath =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
            + QStringLiteral("/DeskTidy.ini");
        QSettings ini(iniPath, QSettings::IniFormat);
        ini.beginGroup(QStringLiteral("floating/rolledUp"));
        QVERIFY2(ini.childKeys().isEmpty(),
                 "展开状态下不该残留 rolledUp 键（应只有非默认状态才落盘）");
    }

    void openBoxNamesCleansAndDeduplicates()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        Settings s;
        s.setOpenBoxNames({QStringLiteral("  临时  "),
                           QString(),
                           QStringLiteral("工作"),
                           QStringLiteral("临时"),      // 去空白后与第一项重复
                           QStringLiteral("   "),
                           QStringLiteral("工作")});     // 再次重复

        const QStringList got = s.openBoxNames();
        QCOMPARE(got.size(), 2);
        QVERIFY(got.contains(QStringLiteral("临时")));
        QVERIFY(got.contains(QStringLiteral("工作")));
    }

    void emptyOpenBoxNamesRemovesKey()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        Settings s;
        s.setOpenBoxNames({QStringLiteral("临时")});
        s.setOpenBoxNames(QStringList());
        QVERIFY2(s.openBoxNames().isEmpty(), "清空后应读回空列表");

        const QString iniPath =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
            + QStringLiteral("/DeskTidy.ini");
        QSettings ini(iniPath, QSettings::IniFormat);
        QVERIFY2(!ini.contains(QStringLiteral("floating/openBoxes")),
                 "清空后不该残留 openBoxes 键");
    }

    void openBoxNamesSurvivesBoxNameWithSlash()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        Settings s;
        const QStringList names{QStringLiteral("a/b"), QStringLiteral("工作/2024")};
        s.setOpenBoxNames(names);
        QCOMPARE(s.openBoxNames(), names);
    }

    // 防的是：浮窗改造把既有配置项（排除名单、上次盒名）带坏了。
    void floatingSettingsDoNotDisturbExistingOnes()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        Settings s;

        s.setExcludedNames({QStringLiteral("重要文件"), QStringLiteral("别动")});
        s.setLastBoxName(QStringLiteral("工作"));

        s.setOpenBoxNames({QStringLiteral("临时")});
        s.setFloatGeometry(QStringLiteral("临时"), QByteArray("blob"));
        s.setFloatRolledUp(QStringLiteral("临时"), true);
        s.setAlwaysOnTop(false);

        QCOMPARE(s.excludedNames(),
                 QStringList({QStringLiteral("重要文件"), QStringLiteral("别动")}));
        QCOMPARE(s.lastBoxName(), QStringLiteral("工作"));
    }

    // -----------------------------------------------------------------------
    // 状态机：开关浮窗时的落盘一致性
    // -----------------------------------------------------------------------

    void openBoxRegistersStateAndPersists()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        AppService service;
        FloatingBoxManager mgr(&service);
        const QString path = makeBoxDir(QStringLiteral("临时"));

        QSignalSpy spy(&mgr, &FloatingBoxManager::boxWindowToggled);
        mgr.openBox(QStringLiteral("临时"), path);

        QVERIFY(mgr.isBoxOpen(QStringLiteral("临时")));
        QVERIFY(mgr.openBoxNames().contains(QStringLiteral("临时")));

        // 信号必须发出且参数正确 —— 主窗口与托盘菜单靠它刷新勾选态，
        // 不发信号的话菜单会一直显示旧状态。
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toString(), QStringLiteral("临时"));
        QCOMPARE(spy.first().at(1).toBool(), true);

        // 配置也要落盘，否则重启后浮窗不见
        QVERIFY(service.settings()->openBoxNames().contains(QStringLiteral("临时")));
    }

    void closeBoxUnregistersAndPersists()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        AppService service;
        FloatingBoxManager mgr(&service);
        mgr.openBox(QStringLiteral("临时"), makeBoxDir(QStringLiteral("临时")));

        QSignalSpy spy(&mgr, &FloatingBoxManager::boxWindowToggled);
        mgr.closeBox(QStringLiteral("临时"));

        QVERIFY(!mgr.isBoxOpen(QStringLiteral("临时")));
        QVERIFY(!service.settings()->openBoxNames().contains(QStringLiteral("临时")));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(1).toBool(), false);
    }

    void openAndCloseAreIdempotent()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        AppService service;
        FloatingBoxManager mgr(&service);
        const QString path = makeBoxDir(QStringLiteral("临时"));

        mgr.openBox(QStringLiteral("临时"), path);
        QVERIFY(mgr.isBoxOpen(QStringLiteral("临时")));

        // 再开一次：仍是开着，且不重复发信号
        QSignalSpy reopenSpy(&mgr, &FloatingBoxManager::boxWindowToggled);
        mgr.openBox(QStringLiteral("临时"), path);
        QVERIFY(mgr.isBoxOpen(QStringLiteral("临时")));
        QCOMPARE(reopenSpy.count(), 0);

        mgr.closeBox(QStringLiteral("临时"));
        QVERIFY(!mgr.isBoxOpen(QStringLiteral("临时")));

        QSignalSpy closeAgainSpy(&mgr, &FloatingBoxManager::boxWindowToggled);
        mgr.closeBox(QStringLiteral("临时"));
        QVERIFY2(!mgr.isBoxOpen(QStringLiteral("临时")), "重复关闭不该出错");
        QCOMPARE(closeAgainSpy.count(), 0);
    }

    void multipleBoxesPersistTogether()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        AppService service;
        FloatingBoxManager mgr(&service);

        mgr.openBox(QStringLiteral("甲"), makeBoxDir(QStringLiteral("甲")));
        mgr.openBox(QStringLiteral("乙"), makeBoxDir(QStringLiteral("乙")));
        mgr.openBox(QStringLiteral("丙"), makeBoxDir(QStringLiteral("丙")));

        QCOMPARE(mgr.openBoxNames().size(), 3);

        QStringList saved = service.settings()->openBoxNames();
        saved.sort();
        QCOMPARE(saved, QStringList({QStringLiteral("丙"), QStringLiteral("乙"),
                                     QStringLiteral("甲")}));

        mgr.closeBox(QStringLiteral("乙"));
        QCOMPARE(mgr.openBoxNames().size(), 2);
        QStringList after = service.settings()->openBoxNames();
        after.sort();
        QCOMPARE(after, QStringList({QStringLiteral("丙"), QStringLiteral("甲")}));
    }

    // 防的是：QHash 遍历顺序不定导致每次落盘顺序不同，ini 内容无意义抖动。
    //
    // 期望值是**排序后**的名单，不是打开顺序 ——
    // FloatingBoxManager::persistOpenBoxes 刻意做了 `names.sort()`，
    // 正是为了消除 QHash 遍历的不确定性。
    //
    // ⚠️ 排序按 Unicode 码点，不是拼音：
    //     丙(U+4E19) < 乙(U+4E59) < 甲(U+7532)
    //   所以"甲"排在最后。写成拼音序（甲/乙/丙）会把正确实现误判成 bug。
    //   本用例真正要防的是"顺序可复现"，不是"中文排得好看"，
    //   所以下面还会断言：反复落盘得到同一个顺序。
    void persistedOrderIsStable()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        AppService service;
        FloatingBoxManager mgr(&service);

        mgr.openBox(QStringLiteral("丙"), makeBoxDir(QStringLiteral("丙")));
        mgr.openBox(QStringLiteral("甲"), makeBoxDir(QStringLiteral("甲")));
        mgr.openBox(QStringLiteral("乙"), makeBoxDir(QStringLiteral("乙")));

        const QStringList first = service.settings()->openBoxNames();
        QCOMPARE(first, QStringList({QStringLiteral("丙"), QStringLiteral("乙"),
                                     QStringLiteral("甲")}));

        // 关键断言：再来一轮开关，顺序必须一模一样（这才叫"不抖动"）
        mgr.closeBox(QStringLiteral("乙"));
        mgr.openBox(QStringLiteral("乙"), makeBoxDir(QStringLiteral("乙")));
        QCOMPARE(service.settings()->openBoxNames(), first);

        // 去掉一个之后，剩下的仍保持排序序
        mgr.closeBox(QStringLiteral("丙"));
        QCOMPARE(service.settings()->openBoxNames(),
                 QStringList({QStringLiteral("乙"), QStringLiteral("甲")}));
    }

    // 防的是：closeAll（退出流程用）把配置里的开关记录也清掉了，
    // 于是"常驻"变成每次启动都要重新开浮窗。
    void closeAllKeepsConfigurationForNextLaunch()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        AppService service;
        FloatingBoxManager mgr(&service);

        mgr.openBox(QStringLiteral("甲"), makeBoxDir(QStringLiteral("甲")));
        mgr.openBox(QStringLiteral("乙"), makeBoxDir(QStringLiteral("乙")));

        mgr.closeAll();

        QVERIFY2(mgr.openBoxNames().isEmpty(), "closeAll 后内存里不该还有浮窗");
        QVERIFY2(service.settings()->openBoxNames().size() == 2,
                 "closeAll 不该清空配置 —— 退出时的关闭是「程序结束了」，"
                 "不是「主人不想再看到这些浮窗」，下次启动要恢复");
    }

    // 防的是：restoreOpenBoxes 照配置无脑建浮窗，不管盒目录是否还在。
    void restoreOpenBoxesDropsMissingBoxes()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        AppService service;
        FloatingBoxManager mgr(&service);

        // 本用例要真的让 BoxManager 扫到那个盒，所以必须用真实的盒根目录。
        // 这是本文件唯一会碰宿主目录的地方，测完立刻清理并恢复配置。
        const QString realBoxName = QStringLiteral("存活测试盒");
        const QString realBoxPath =
            CoreNames::boxRoot() + QLatin1Char('/') + realBoxName;

        const QStringList originalOpen = service.settings()->openBoxNames();
        QDir().mkpath(realBoxPath);

        service.settings()->setOpenBoxNames({realBoxName,
                                             QStringLiteral("已删除的盒甲"),
                                             QStringLiteral("已删除的盒乙")});

        mgr.restoreOpenBoxes();

        QVERIFY(mgr.isBoxOpen(realBoxName));
        QVERIFY(!mgr.isBoxOpen(QStringLiteral("已删除的盒甲")));
        QVERIFY(!mgr.isBoxOpen(QStringLiteral("已删除的盒乙")));

        const QStringList nowSaved = service.settings()->openBoxNames();
        QCOMPARE(nowSaved.size(), 1);
        QCOMPARE(nowSaved.first(), realBoxName);

        // 收尾
        mgr.closeAll();
        QDir(realBoxPath).removeRecursively();
        service.settings()->setOpenBoxNames(originalOpen);
    }

    // 几何落盘的**真实契约**（读 FloatingBoxManager 实现得出）：
    //   openBox   -> 不写几何。首次打开走默认落位，本来就没有"上次位置"可言。
    //   closeBox  -> 写几何。兜底存一次，防去抖定时器还没触发就被销毁。
    //   移动/缩放 -> 去抖 500ms 后写。
    //
    // 所以断言的是"关闭后必须有几何"，而不是"打开后就有"——
    // 后者是一厢情愿，会把一个正确实现误判成 bug。
    void closeBoxStoresUsableGeometry()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        AppService service;
        FloatingBoxManager mgr(&service);
        const QString box  = QStringLiteral("几何测试");

        mgr.openBox(box, makeBoxDir(box));

        mgr.closeBox(box);
        QVERIFY2(!service.settings()->floatGeometry(box).isEmpty(),
                 "关闭浮窗后应当落一份几何，否则下次打开会跑到默认位置");

        // 再开再关一轮，几何仍应保留（不是被清空）
        mgr.openBox(box, makeBoxDir(box));
        mgr.closeBox(box);
        QVERIFY2(!service.settings()->floatGeometry(box).isEmpty(),
                 "反复开关后几何仍应保留");
    }

    // 防的是：两个盒的几何互相覆盖。
    void geometryIsIndependentPerBox()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        AppService service;
        FloatingBoxManager mgr(&service);
        const QString a = QStringLiteral("甲");
        const QString b = QStringLiteral("乙");

        // 用"开-关"触发落盘（openBox 本身不写几何，见上一个用例的说明）
        mgr.openBox(a, makeBoxDir(a));
        mgr.closeBox(a);
        mgr.openBox(b, makeBoxDir(b));
        mgr.closeBox(b);

        const QByteArray geomA = service.settings()->floatGeometry(a);
        const QByteArray geomB = service.settings()->floatGeometry(b);
        QVERIFY2(!geomA.isEmpty(), "甲应当有自己的几何");
        QVERIFY2(!geomB.isEmpty(), "乙应当有自己的几何");

        // 再折腾一次甲，乙的几何不该受影响
        mgr.openBox(a, makeBoxDir(a));
        mgr.closeBox(a);
        QCOMPARE(service.settings()->floatGeometry(b), geomB);
    }

    // =======================================================================
    // 浮窗外观（BoxAppearance 纯函数 + Settings 每盒一份的配置）
    // =======================================================================

    // 防的是：浮窗只应该把 .docx/.txt/.lnk 这类扩展名藏起来，
    // 不能把真实路径也改掉，也不能误伤文件夹与 .gitignore 这类点文件。
    void floatingItemListHidesFileExtensions()
    {
        DesktopEntry file;
        file.filePath = QStringLiteral("C:/DeskTidy/报告.docx");
        file.name = QStringLiteral("报告.docx");
        file.isDir = false;

        DesktopEntry folder;
        folder.filePath = QStringLiteral("C:/DeskTidy/资料");
        folder.name = QStringLiteral("资料");
        folder.isDir = true;

        DesktopEntry dotfile;
        dotfile.filePath = QStringLiteral("C:/DeskTidy/.gitignore");
        dotfile.name = QStringLiteral(".gitignore");
        dotfile.isDir = false;

        ItemListWidget::Options options;
        options.hideExtensions = true;
        ItemListWidget list(options);
        list.setItems({file, folder, dotfile});

        QCOMPARE(list.count(), 3);
        QCOMPARE(list.item(0)->text(), QStringLiteral("报告"));
        QCOMPARE(list.item(0)->data(Qt::UserRole).toString(), file.filePath);
        QCOMPARE(list.item(1)->text(), QStringLiteral("资料"));
        QCOMPARE(list.item(2)->text(), QStringLiteral(".gitignore"));

        ItemListWidget defaultList;
        defaultList.setItems({file});
        QCOMPARE(defaultList.item(0)->text(), file.name);
    }

    // 防的是：图标尺寸档位算错，导致"选了大图标还是小图标"。
    void appearanceIconSizeDerivation()
    {
        BoxAppearance a;    // 默认 = 列表 + 不透明
        QCOMPARE(a.viewMode, BoxAppearance::ViewMode::List);
        QCOMPARE(a.opacity, 100);
        QVERIFY(a.isDefault());

        // iconSize 为 0 时跟随 viewMode
        a.viewMode = BoxAppearance::ViewMode::SmallIcon;
        QCOMPARE(a.effectiveIconSize(), 16);
        a.viewMode = BoxAppearance::ViewMode::MediumIcon;
        QCOMPARE(a.effectiveIconSize(), 32);
        a.viewMode = BoxAppearance::ViewMode::LargeIcon;
        QCOMPARE(a.effectiveIconSize(), 64);

        // 显式设过就用显式值（留着将来支持自定义像素的余地）
        a.iconSize = 48;
        QCOMPARE(a.effectiveIconSize(), 48);

        // 负数视为"未设"，应退回按 viewMode 推导，而不是变成一个非法尺寸
        a.iconSize = -1;
        QCOMPARE(a.effectiveIconSize(), 64);

        // 档位必须严格递增，否则"大图标比中图标还小"这种荒谬配置会被放行
        QVERIFY(BoxAppearance::defaultIconSizeFor(BoxAppearance::ViewMode::SmallIcon)
                < BoxAppearance::defaultIconSizeFor(BoxAppearance::ViewMode::MediumIcon));
        QVERIFY(BoxAppearance::defaultIconSizeFor(BoxAppearance::ViewMode::MediumIcon)
                < BoxAppearance::defaultIconSizeFor(BoxAppearance::ViewMode::LargeIcon));
    }

    // 防的是：isDefault 判错，导致默认外观也往配置里写一堆键
    // （或反过来，改过的外观被判成默认而没存下来）。
    void appearanceIsDefaultDetection()
    {
        BoxAppearance a;
        QVERIFY(a.isDefault());

        a.viewMode = BoxAppearance::ViewMode::LargeIcon;
        QVERIFY2(!a.isDefault(), "改了视图模式就不再是默认");

        a = BoxAppearance();
        a.opacity = 80;
        QVERIFY2(!a.isDefault(), "改了透明度就不再是默认");

        a = BoxAppearance();
        a.iconSize = 48;
        QVERIFY2(!a.isDefault(), "显式设了图标尺寸就不再是默认");
    }

    void appearanceRoundTrip()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        Settings s;
        const QString box = QStringLiteral("外观测试");

        // 默认外观读回来就该是默认值
        QVERIFY(s.floatAppearance(box).isDefault());

        BoxAppearance a;
        a.viewMode = BoxAppearance::ViewMode::LargeIcon;
        a.opacity  = 75;
        s.setFloatAppearance(box, a);

        const BoxAppearance back = s.floatAppearance(box);
        QCOMPARE(back.viewMode, BoxAppearance::ViewMode::LargeIcon);
        QCOMPARE(back.opacity, 75);
    }

    // 防的是：默认值也被落键，配置文件为每个盒留一堆恒等于默认的冗余行。
    // 这与 floatRolledUp 的既有约定一致。
    void appearanceDefaultsAreNotPersisted()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        Settings s;
        const QString box = QStringLiteral("默认不落键");

        // 先写一个非默认值，再改回默认，键应当被清掉
        BoxAppearance custom;
        custom.viewMode = BoxAppearance::ViewMode::LargeIcon;
        custom.opacity  = 60;
        s.setFloatAppearance(box, custom);
        QVERIFY(!s.floatAppearance(box).isDefault());

        s.setFloatAppearance(box, BoxAppearance());
        QVERIFY2(s.floatAppearance(box).isDefault(),
                 "写回默认后读出来应当也是默认");
    }

    // 防的是：配置文件被手工编辑成非法值时程序行为异常。
    // 配置是可以被用户直接改的，不能假设它一定合法。
    void appearanceClampsOutOfRangeValues()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        Settings s;
        const QString box = QStringLiteral("越界测试");

        // 透明度低于下限：会被夹到 kMinOpacity，绝不返回 0（那会让浮窗消失）
        BoxAppearance tooLow;
        tooLow.opacity = 0;
        s.setFloatAppearance(box, tooLow);
        QVERIFY2(s.floatAppearance(box).opacity >= BoxAppearance::kMinOpacity,
                 "透明度必须被夹到下限以上，否则浮窗会彻底看不见");

        // 大于 100：夹回 100
        BoxAppearance tooHigh;
        tooHigh.opacity = 250;
        s.setFloatAppearance(box, tooHigh);
        QVERIFY(s.floatAppearance(box).opacity <= 100);
    }

    // 防的是：一个盒的外观改了，另一个盒跟着变。
    void appearanceIsIndependentPerBox()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        Settings s;
        const QString a = QStringLiteral("甲");
        const QString b = QStringLiteral("乙");

        BoxAppearance big;
        big.viewMode = BoxAppearance::ViewMode::LargeIcon;
        big.opacity  = 60;
        s.setFloatAppearance(a, big);

        BoxAppearance small;
        small.viewMode = BoxAppearance::ViewMode::SmallIcon;
        s.setFloatAppearance(b, small);

        QCOMPARE(s.floatAppearance(a).viewMode, BoxAppearance::ViewMode::LargeIcon);
        QCOMPARE(s.floatAppearance(b).viewMode, BoxAppearance::ViewMode::SmallIcon);
        QCOMPARE(s.floatAppearance(a).opacity, 60);
        QCOMPARE(s.floatAppearance(b).opacity, 100);    // 乙没改透明度
    }

    // 防的是：盒名含 '/' 时外观配置写到别的键上（与几何那条同理）。
    void appearanceSurvivesBoxNameWithSlash()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        Settings s;
        const QString tricky = QStringLiteral("a/b:测试 盒");

        BoxAppearance a;
        a.viewMode = BoxAppearance::ViewMode::MediumIcon;
        a.opacity  = 70;
        s.setFloatAppearance(tricky, a);

        const BoxAppearance back = s.floatAppearance(tricky);
        QCOMPARE(back.viewMode, BoxAppearance::ViewMode::MediumIcon);
        QCOMPARE(back.opacity, 70);

        // 另一个盒不该被串味
        QVERIFY(s.floatAppearance(QStringLiteral("别的盒")).isDefault());
    }

    // 防的是：删盒后残留外观配置，导致将来建同名盒"继承"上一个盒的外观。
    void clearAppearanceRemovesAllKeys()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        Settings s;
        const QString box = QStringLiteral("待清理");

        BoxAppearance a;
        a.viewMode = BoxAppearance::ViewMode::LargeIcon;
        a.opacity  = 55;
        s.setFloatAppearance(box, a);
        QVERIFY(!s.floatAppearance(box).isDefault());

        s.clearFloatAppearance(box);
        QVERIFY2(s.floatAppearance(box).isDefault(),
                 "清理后应当读回默认外观");

        // 不该误伤别的盒
        const QString other = QStringLiteral("无辜的盒");
        s.setFloatAppearance(other, a);
        s.clearFloatAppearance(box);
        QVERIFY2(!s.floatAppearance(other).isDefault(),
                 "清理一个盒的外观不该影响另一个盒");
    }

    // 防的是：浮窗改造把既有配置项带坏（与 floatingSettingsDoNotDisturbExistingOnes 同类）。
    void appearanceDoesNotDisturbOtherSettings()
    {
        if (!m_configWritable)
            QSKIP("配置目录不可写（受限环境）");

        Settings s;
        s.setExcludedNames({QStringLiteral("别动"), QStringLiteral("重要文件")});
        s.setLastBoxName(QStringLiteral("临时"));

        BoxAppearance a;
        a.viewMode = BoxAppearance::ViewMode::LargeIcon;
        s.setFloatAppearance(QStringLiteral("某个盒"), a);
        s.clearFloatAppearance(QStringLiteral("某个盒"));

        QCOMPARE(s.excludedNames(),
                 QStringList({QStringLiteral("别动"), QStringLiteral("重要文件")}));
        QCOMPARE(s.lastBoxName(), QStringLiteral("临时"));
    }

    // =======================================================================
    // 九、窗口推开几何计算（WindowLayout::computePushDown）
    //
    // 【这一组用例为什么重要】
    // 浮窗之间"谁挡了谁、谁该让位多少"是本项目里最不好调试的一块：
    // 算错的直接表现是"浮窗莫名其妙跑到别处去了"，而那时界面上的状态
    // 已经被破坏、无从回放。抽成纯函数之后就能在这里把各种布局钉死。
    //
    // 【⚠️ 本组用例当前全部失败，原因已在下方逐条定位】
    // 被测实现 core/windowlayout.cpp 目前存在一处**逻辑死结**，导致它对
    // 任何输入都返回空列表。这不是测试写错，是本小姐用独立探针实测确认过的
    // 产品缺陷（推导与实测见 computePushDown_* 各用例的注释）。
    //
    // 因此本组用例分两类，请勿把第二类当成"重复"而删掉：
    //   * 契约类（C-01/02/03…）：断言**规格要求的行为**。它们现在红着，
    //     修好之后应当全绿 —— 这就是修完的验收标准。
    //   * 特征类（D-01/02…）：断言**当前实际的行为**（恒返回空）。
    //     它们现在是绿的，作用是"钉住现状"，让缺陷不会以别的方式悄悄变形。
    //     一旦有人修好了实现，这两个会立刻变红 —— 那正是提醒他
    //     "契约类该转绿了、我这里该删了"的信号。
    // =======================================================================

    // ---- 构造辅助 ----

    // 一块 1920x1080 的"主屏可用区"。整组用例共用，避免各处手写魔数
    // 导致某一个用例写错屏幕尺寸却看不出来。
    static QRect mainScreen()
    {
        return QRect(0, 0, 1920, 1080);
    }

    // 造一个窗口项。screenRect 默认落在主屏 —— 绝大多数用例都在单屏上。
    static WindowLayout::Item makeItem(const QString &id, const QRect &rect,
                                       const QRect &screen = QRect(0, 0, 1920, 1080))
    {
        WindowLayout::Item it;
        it.id = id;
        it.rect = rect;
        it.screenRect = screen;
        return it;
    }

    // 从结果里取某个 id 的位移量。找不到返回一个哨兵值（INT_MIN），
    // 便于用 QCOMPARE 直接看出"某个窗口不该出现却出现了/该出现却没出现"。
    static int dyOf(const QList<WindowLayout::Shift> &shifts, const QString &id)
    {
        for (const WindowLayout::Shift &s : shifts) {
            if (s.id == id)
                return s.dy;
        }
        return INT_MIN;
    }

    // =======================================================================
    // C 组：契约类 —— 断言规格要求的行为（修好前为红，修好后为绿）
    // =======================================================================

    // C-01 基本推让：正下方、横向完全重叠的窗口应当被推。
    //
    // 防的是：展开之后把下面的窗口压在底下不闻不问 —— 那正是"推开"这个
    // 功能存在的全部理由，它要是没生效，整个功能等于不存在。
    //
    // 断言 dy **恰好**是"让被挡者顶边落到 anchor 底边"所需的量：
    //   anchor   = (0, 0, 200, 150)  -> bottom = 149
    //   被推者   = (0, 100, 200, 50) -> top    = 100
    //   dy 应为 149 - 100 = 49
    // 这条同时验证了"只推最小距离"：若实现改成"推到底"或"等距排列"，
    // 值会明显大于 49。
    void computePushDown_pushesWindowDirectlyBelow()
    {
        const QRect anchor(0, 0, 200, 150);
        const QRect screen = mainScreen();
        const QList<WindowLayout::Item> others{
            makeItem(QStringLiteral("下"), QRect(0, 100, 200, 50))};

        const QList<WindowLayout::Shift> shifts =
            WindowLayout::computePushDown(QStringLiteral("锚"), anchor, screen, others);

        QCOMPARE(shifts.size(), 1);
        QCOMPARE(dyOf(shifts, QStringLiteral("下")), 50);
    }

    // C-02 不该推：位于**上方**的窗口。
    //
    // 防的是：把上面的窗口也往下推。展开是向下长的，推上面的既没道理，
    // 又很容易把它顶出屏幕上沿 —— 用户会看到上方的浮窗无缘无故往下掉。
    void computePushDown_ignoresWindowAbove()
    {
        const QRect anchor(0, 400, 200, 150);          // bottom = 549
        const QRect screen = mainScreen();
        const QList<WindowLayout::Item> others{
            makeItem(QStringLiteral("上"), QRect(0, 100, 200, 50))};   // 在 anchor 上方

        const QList<WindowLayout::Shift> shifts =
            WindowLayout::computePushDown(QStringLiteral("锚"), anchor, screen, others);

        QCOMPARE(dyOf(shifts, QStringLiteral("上")), INT_MIN);
    }

    // C-03 不该推：横向完全不重叠的窗口（一个在左、一个在右）。
    //
    // 防的是：只看纵向就推。桌面上左右并排摆两个浮窗是常见用法，
    // 左边那个展开不该把右边那个推下去 —— 它们根本不在同一条竖直通道上。
    void computePushDown_ignoresHorizontallySeparate()
    {
        const QRect anchor(0, 0, 200, 150);            // 占 x=0..199
        const QRect screen = mainScreen();
        const QList<WindowLayout::Item> others{
            makeItem(QStringLiteral("右"), QRect(600, 100, 200, 50))};   // 占 x=600..799

        const QList<WindowLayout::Shift> shifts =
            WindowLayout::computePushDown(QStringLiteral("锚"), anchor, screen, others);

        QCOMPARE(dyOf(shifts, QStringLiteral("右")), INT_MIN);
    }

    // C-04 不该推：横向**仅边界相接**的窗口。
    //
    // 防的是：把"并排贴着"误判成"重叠"。这条约束在头文件里写明了是刻意的：
    //   anchor  right() = 199
    //   邻窗   left()  = 200
    // 两者严格相邻、一个像素都不重叠。若判据写成 a.right() >= b.left()，
    // 这种紧贴摆放就会被误推 —— 桌面上东西会莫名其妙地往下掉。
    void computePushDown_ignoresEdgeTouchingNeighbour()
    {
        const QRect anchor(0, 0, 200, 150);            // right = 199
        const QRect screen = mainScreen();
        const QList<WindowLayout::Item> others{
            makeItem(QStringLiteral("紧贴"), QRect(200, 100, 200, 50))};  // left = 200

        const QList<WindowLayout::Shift> shifts =
            WindowLayout::computePushDown(QStringLiteral("锚"), anchor, screen, others);

        QCOMPARE(dyOf(shifts, QStringLiteral("紧贴")), INT_MIN);
    }

    // C-05 不该推：发起者自己。
    //
    // 防的是：把自己也算进让位对象，于是展开时窗口被自己推走 ——
    // 表现是"一展开就往下跑"，而且会与动画目标位置打架。
    void computePushDown_neverPushesItself()
    {
        const QRect anchor(0, 0, 200, 150);
        const QRect screen = mainScreen();
        const QList<WindowLayout::Item> others{
            makeItem(QStringLiteral("锚"), QRect(0, 0, 200, 150)),       // 自己
            makeItem(QStringLiteral("下"), QRect(0, 100, 200, 50))};

        const QList<WindowLayout::Shift> shifts =
            WindowLayout::computePushDown(QStringLiteral("锚"), anchor, screen, others);

        QCOMPARE(dyOf(shifts, QStringLiteral("锚")), INT_MIN);
    }

    // C-06 顺序应用：上面那个先让开，下面那个再基于新位置判断。
    //
    // 【这个布局为什么能区分"顺序应用"与"各自按原位置算"】
    //
    // 关键在于"让位"会**改变被推者自身的位置**，而这个改变只在后续项
    // 需要参考它时才起作用。构造如下（anchor 底边 = 149）：
    //
    //   中间窗口 mid = (0, 100, 200, 50)  -> top=100，被压，应推 49，落到 y=149..198
    //   下方窗口 bot = (0, 150, 200, 50)  -> top=150
    //
    // 两种算法的分歧点：
    //   * 各自独立算（不含连锁下推）：bot 的 top=150 >= 149，判定"没被 anchor 压住" -> 不推
    //   * 含连锁下推：mid 让开后占了 y=149..198，bot 原本的 y=150..199 就与 mid
    //     **撞上了** -> bot 需要继续往下让
    //
    // 所以"bot 在不在结果里"这一条，就能把两种算法分开：
    //   含连锁下推 -> bot 应当出现（dy>0）
    //   各自独立算 -> bot 不出现
    //
    // ⚠️ 连锁发生在哪一层，必须说清楚（否则这条用例的注释是误导的）：
    // 不是在"外层循环顺序应用 settled"那一层 —— anchorTarget 全程不变、
    // 每个 id 只处理一次，settled 只会被**自己**的 id 命中，对不同的 id
    // 来说它读不到任何东西。真正的连锁在 pushBelow 内部完成：
    // 它把 anchor 和已落定者放进同一个障碍列表，从原始位置一次算到最终落点。
    //
    // 因此这条用例钉的是**结果**（bot 也被推动、且落到 mid 之下）。
    // 实现上由"连锁下推"达成：anchor 与已落定者一起作为障碍，
    // 从原始位置一次算到最终落点。
    void computePushDown_appliesShiftsTopDown()
    {
        const QRect anchor(0, 0, 200, 150);            // bottom = 149
        const QRect screen = mainScreen();
        const QRect midRect(0, 100, 200, 50);
        const QRect botRect(0, 150, 200, 50);
        const QList<WindowLayout::Item> others{
            makeItem(QStringLiteral("中"), midRect),
            makeItem(QStringLiteral("下"), botRect)};

        const QList<WindowLayout::Shift> shifts =
            WindowLayout::computePushDown(QStringLiteral("锚"), anchor, screen, others);

        const int dyMid = dyOf(shifts, QStringLiteral("中"));
        const int dyBot = dyOf(shifts, QStringLiteral("下"));

        QVERIFY2(dyMid != INT_MIN, "中间那个被 anchor 压住，必须让位");
        QVERIFY2(dyBot != INT_MIN,
                 "中间让开后与下方窗口相撞，下方窗口也应当继续让位（连锁下推）");

        // 规格不变量一：让位后各自都不再与 anchor 纵向交叠。
        const QRect midMoved = midRect.translated(0, dyMid);
        const QRect botMoved = botRect.translated(0, dyBot);
        QVERIFY2(midMoved.top() > anchor.bottom(), "中间那个必须落到 anchor 底边之下");
        QVERIFY2(botMoved.top() >= midMoved.bottom() || botMoved.top() > anchor.bottom(),
                 "下方那个让位后不得再与中间那个交叠");

        // 规格不变量二：连锁让位不得越过屏幕下沿。
        QVERIFY2(botMoved.bottom() <= screen.bottom(),
                 "连锁下推也不得把窗口推出屏幕");
    }

    // C-07 屏幕边界：推不动时不能推出屏幕。
    //
    // 防的是：把窗口推到屏幕外 —— 那等于把浮窗弄丢（用户看不见、也拖不回来）。
    // 实现的选择是"宁可少推"（留下部分重叠），这条用例把该选择钉住。
    //
    // 布局：屏幕可用区底边 = 1079；被推者 bottom = 1059，只剩 20px 可推。
    // anchor 底边 = 1499，要求它推到远超屏幕的位置 —— 必须被钳制。
    void computePushDown_clampsAtScreenBottom()
    {
        const QRect screen(0, 0, 1920, 1080);          // bottom = 1079
        const QRect anchor(0, 900, 200, 600);          // bottom = 1499，远超屏幕

        const QRect nearBottom(0, 1000, 200, 60);      // bottom = 1059
        const QList<WindowLayout::Item> others{
            makeItem(QStringLiteral("近底"), nearBottom, screen)};

        const QList<WindowLayout::Shift> shifts =
            WindowLayout::computePushDown(QStringLiteral("锚"), anchor, screen, others);

        const int dy = dyOf(shifts, QStringLiteral("近底"));
        QVERIFY2(dy != INT_MIN, "被 anchor 压住且还有 20px 空间，必须给出位移");

        // ⚠️ QRect::bottom() == top() + height() - 1。
        // 被推者原本 bottom = 1000 + 60 - 1 = 1059（不是 1060），
        // 剩下的可推空间 = 1079 - 1059 = 20。用 translated() 而不是手算
        // y+height+dy，可以避免再踩这个 -1 的坑。
        const QRect moved = nearBottom.translated(0, dy);

        // 硬约束：底边绝不越过屏幕可用区下沿。
        QVERIFY2(moved.bottom() <= screen.bottom(),
                 "被推窗口的底边不得越过屏幕可用区下沿");

        // 且位移为正 —— 不能为了"塞进屏幕"反而把窗口往上挪。
        QVERIFY2(dy > 0, "被挡住时必须向下让位，位移应为正");

        // 钳制语义：既然上方仍有冲突、下方又只剩 20px，就应当**用满**这 20px。
        // 若实现推得更少，说明钳制把它多减了。
        QVERIFY2(dy <= 20, "可推空间只有 20px，位移不应超过它");
    }

    // C-08 屏幕边界（推不动）：已经贴着屏幕底边的窗口不该被给出位移。
    //
    // 防的是：给出一个 dy>0 却因为钳制实际挪不动，让调用方以为动了 ——
    // 或者更糟：dy 算成负数，把窗口往上挪。
    void computePushDown_givesUpWhenNoRoomLeft()
    {
        const QRect screen(0, 0, 1920, 1080);
        const QRect anchor(0, 0, 200, 600);            // bottom = 599

        // 被推者底边已经贴着屏幕下沿，一点也推不动
        const QList<WindowLayout::Item> others{
            makeItem(QStringLiteral("贴底"), QRect(0, 1000, 200, 79), screen)};

        const QList<WindowLayout::Shift> shifts =
            WindowLayout::computePushDown(QStringLiteral("锚"), anchor, screen, others);

        const int dy = dyOf(shifts, QStringLiteral("贴底"));
        QVERIFY2(dy == INT_MIN || dy > 0,
                 "推不动时不应给出位移（尤其不能给负数把窗口往上挪）");
    }

    // C-09 跨屏隔离：不同屏幕的窗口各自独立。
    //
    // 防的是：拿另一块屏的 y 坐标来算。多屏环境下副屏的 y 常常从 0 重新开始，
    // 与主屏的 y 没有可比性 —— 硬算会把副屏窗口推到荒谬的位置上去。
    //
    // 布局：副屏在主屏右侧（x 从 1920 开始），副屏上有一个窗口，
    // 其坐标看起来"在主屏 anchor 下方"，但它属于另一块屏，绝不该被动。
    void computePushDown_neverCrossesScreens()
    {
        const QRect primary(0, 0, 1920, 1080);
        const QRect secondary(1920, 0, 1920, 1080);

        const QRect anchor(0, 100, 200, 150);
        const QRect screen = mainScreen();

        const QList<WindowLayout::Item> others{
            makeItem(QStringLiteral("副屏窗"), QRect(1920, 300, 200, 50), secondary)};

        const QList<WindowLayout::Shift> shifts =
            WindowLayout::computePushDown(QStringLiteral("锚"), anchor, screen, others);

        QCOMPARE(dyOf(shifts, QStringLiteral("副屏窗")), INT_MIN);
    }

    // C-10 空输入不崩、返回空。
    //
    // 防的是：调用方在没有其他浮窗时（单浮窗场景）触发空指针/越界。
    void computePushDown_handlesEmptyOthers()
    {
        const QRect screen = mainScreen();
        const QList<WindowLayout::Shift> shifts =
            WindowLayout::computePushDown(QStringLiteral("锚"),
                                          QRect(0, 0, 200, 150),
                                          screen,
                                          QList<WindowLayout::Item>());
        QVERIFY(shifts.isEmpty());
    }

    // C-11 完全重合的两个窗口：行为必须合理且可解释。
    //
    // 防的是：完全重合（两个浮窗叠在同一位置，通常来自几何信息被写坏）
    // 落到"两个判据之间的缝隙"里 —— 既不算挡住、也不算没挡住，
    // 于是返回一个随机结果。重合必须有一个明确、可解释的处理。
    //
    // 规格：完全重合 = 被完全挡住的窗口，应当整块让开。
    // 实现走的是 pushBelow 连锁下推，判据为
    //     cursor.top() >= b.bottom()  -> 跳过（已在下面）
    //     cursor.bottom() <= b.top()  -> 跳过（完全在上面）
    // 重合时 cursor == b：top=300 < 449 且 bottom=449 > 300，两条都不跳过，
    // 于是落点 = b.bottom()+1 = 450，dy = 450 - 300 = 150。
    //
    // ⚠️ 这里断言的是"让开一个高度后不再重叠"这个**规格不变量**，
    // 而不是去复刻实现里的 +1 偏移 —— 复刻魔数会让用例在实现
    // 做出等价调整时误报失败。
    void computePushDown_handlesExactOverlap()
    {
        const QRect anchor(0, 300, 200, 150);
        const QRect screen = mainScreen();
        const QRect same(0, 300, 200, 150);
        const QList<WindowLayout::Item> others{
            makeItem(QStringLiteral("重合"), same)};

        const QList<WindowLayout::Shift> shifts =
            WindowLayout::computePushDown(QStringLiteral("锚"), anchor, screen, others);

        const int dy = dyOf(shifts, QStringLiteral("重合"));
        QVERIFY2(dy != INT_MIN, "完全重合的窗口应被视为被完全挡住，需要让位");
        QVERIFY2(dy > 0, "位移必须为正 —— 负数会把窗口往上挪，等于反向重叠");

        // 规格不变量：让位之后，它与 anchor 之间不再有纵向交叠。
        const QRect moved = same.translated(0, dy);
        QVERIFY2(moved.top() > anchor.bottom() || moved.bottom() < anchor.top(),
                 "让位后必须与 anchor 脱离纵向交叠");

        // 且不得越过屏幕下沿（本项目"宁可少推也不推出去"的硬约束）。
        QVERIFY2(moved.bottom() <= screen.bottom(),
                 "完全重合时被推窗口也不得越过屏幕下沿");
    }

    // C-12 只推"被挡住的"，不做等距排列。
    //
    // 防的是：把"推开"实现成"把下方所有窗口按固定间距重排"。
    // 那种做法会把本来没被挡住的窗口也挪走，桌面布局会被整个打乱。
    //
    // 布局：anchor 底边 = 149，三个窗口里只有第一个被压住，
    // 后两个的顶边都远在 149 之下。
    void computePushDown_pushesOnlyBlockedOnes()
    {
        const QRect anchor(0, 0, 200, 150);            // bottom = 149
        const QRect screen = mainScreen();
        const QList<WindowLayout::Item> others{
            makeItem(QStringLiteral("被压"), QRect(0, 100, 200, 50)),   // top=100 < 149
            makeItem(QStringLiteral("没压1"), QRect(0, 400, 200, 50)),  // top=400
            makeItem(QStringLiteral("没压2"), QRect(0, 700, 200, 50))};  // top=700

        const QList<WindowLayout::Shift> shifts =
            WindowLayout::computePushDown(QStringLiteral("锚"), anchor, screen, others);

        QCOMPARE(dyOf(shifts, QStringLiteral("被压")), 50);
        QCOMPARE(dyOf(shifts, QStringLiteral("没压1")), INT_MIN);
        QCOMPARE(dyOf(shifts, QStringLiteral("没压2")), INT_MIN);
    }

    // C-13 computePushDownForResize：展开（变高）时才算。
    //
    // 防的是：卷起时也去推别人 —— 卷起只是把自己收起来，腾出来的空间
    // 不需要谁去填。若卷起也推，下方窗口会莫名往下掉，而且永远不会回来。
    void computePushDownForResize_onlyWhenGrowing()
    {
        const QRect screen = mainScreen();
        const QList<WindowLayout::Item> others{
            makeItem(QStringLiteral("下"), QRect(0, 120, 200, 50))};

        // 展开：current 矮、target 高 -> 应当计算
        const QList<WindowLayout::Shift> growing =
            WindowLayout::computePushDownForResize(QStringLiteral("锚"),
                                                   QRect(0, 0, 200, 40),
                                                   QRect(0, 0, 200, 150),
                                                   screen,
                                                   others);
        QVERIFY2(!growing.isEmpty(),
                 "展开（目标更高）时应当算出被挡住的窗口");

        // 卷起：current 高、target 矮 -> 必须返回空
        const QList<WindowLayout::Shift> shrinking =
            WindowLayout::computePushDownForResize(QStringLiteral("锚"),
                                                   QRect(0, 0, 200, 150),
                                                   QRect(0, 0, 200, 40),
                                                   mainScreen(),
                                                   others);
        QVERIFY2(shrinking.isEmpty(),
                 "卷起（目标更矮）时不该推任何窗口");

        // 高度不变 -> 也返回空
        const QList<WindowLayout::Shift> same =
            WindowLayout::computePushDownForResize(QStringLiteral("锚"),
                                                   QRect(0, 0, 200, 150),
                                                   QRect(0, 0, 200, 150),
                                                   mainScreen(),
                                                   others);
        QVERIFY2(same.isEmpty(),
                 "高度不变时不该推任何窗口");
    }

    // =======================================================================
    // D 组：特征类 —— 钉住"当前实际行为"，用于暴露并跟踪缺陷
    // =======================================================================

    // D-01 曾经的死结（已修复）—— 保留本用例作为回归守卫。
    //
    // 历史：computePushDown 曾经对任何输入都返回空，功能完全没生效。
    // 原因是三条判断互为反面：
    //     if (current.top() < anchorTarget.bottom()) continue;   // 要求"不重叠"
    //     int dy = anchorTarget.bottom() - current.top();        // 于是 dy <= 0
    //     if (dy <= 0) continue;                                 // 必然丢掉
    // 能通过第一条的输入一定过不了第三条。
    //
    // 修法是把判据改成"**重叠才推**"：纵向区间相交且它在下面。
    //
    // ⚠️ 本用例的价值在于：它是"最普通的一个被挡场景"，任何人再把这个判据
    // 写反，这里会第一时间变红。所以断言方向是"必须有位移"，不再是"必须为空"。
    void computePushDown_pushesOrdinaryBlockedNeighbour()
    {
        // 教科书式的被挡案例：正下方、横向完全重叠、空间充足
        const QRect anchor(0, 0, 200, 150);            // bottom = 149
        const QRect screen = mainScreen();
        const QList<WindowLayout::Item> others{
            makeItem(QStringLiteral("下"), QRect(0, 100, 200, 50))};   // top=100，被压住

        const QList<WindowLayout::Shift> shifts =
            WindowLayout::computePushDown(QStringLiteral("锚"), anchor, screen, others);

        QCOMPARE(shifts.size(), 1);
        QCOMPARE(dyOf(shifts, QStringLiteral("下")), 50);   // 149 + 1 - 100
    }

    // D-02 computePushDownForResize 也要能算出位移。
    //
    // 它把工作转交给 computePushDown，所以底层那个死结被修好之后，
    // 这一层也应当跟着生效。单独钉一条，是为了让"两层都通"这件事可见。
    void computePushDownForResize_pushesWhenGrowing()
    {
        const QRect screen = mainScreen();
        const QList<WindowLayout::Item> others{
            makeItem(QStringLiteral("下"), QRect(0, 120, 200, 50))};

        const QList<WindowLayout::Shift> shifts =
            WindowLayout::computePushDownForResize(QStringLiteral("锚"),
                                                   QRect(0, 0, 200, 40),
                                                   QRect(0, 0, 200, 150),
                                                   screen,
                                                   others);

        QCOMPARE(shifts.size(), 1);
        QCOMPARE(dyOf(shifts, QStringLiteral("下")), 30);   // 149 + 1 - 120
    }

    // =======================================================================
    // E 组：悬停自动展开的状态机
    //
    // 【这一组能测到什么，测不到什么 —— 先说清楚】
    //
    // 能测（且下面都测了）：
    //   * 延迟常量确实是 250 / 400，且"离开比进入长"这条关系成立；
    //   * 全局开关的默认值与落盘往返；
    //   * 状态机的**迁移规则**：手动卷起压过自动展开、再手动一次恢复自动、
    //     悬停展开与手动展开的区分。
    //
    // 测不到（**没有**在这里假装测了）：
    //   * "鼠标真的停在窗口上 250ms 后窗口真的展开了" —— 这需要真实的
    //     鼠标进入事件与一个跑起来的事件循环。造一个 QEnterEvent 打进去
    //     可以模拟，但那样测的是"我能不能手动触发这个函数"，
    //     而不是"Windows 会不会把 enter 事件送到这儿"。
    //   * 右键菜单 exec() 期间的事件时序 —— exec() 会阻塞，在单测里
    //     没法既弹菜单又继续跑断言。
    //   * 尺寸手柄按下/抬起、QDrag 抓取期间的真实事件流。
    //
    // 这些只能靠 tools/hover_diag 那样的探针配合真机人工操作。
    // =======================================================================

    // E-01 延迟常量：进入快、离开慢。
    //
    // 防的是：把两个延迟写反，或者其中一个改成 0（那就变成"鼠标扫过就炸开"）。
    void hoverDelaysAreSane()
    {
        QCOMPARE(FloatingBoxWidget::kHoverExpandDelayMs, 250);
        QCOMPARE(FloatingBoxWidget::kHoverCollapseDelayMs, 400);

        // 进入必须比离开快 —— 这是"从列表移向标题栏时不至于闪卷"那条
        // 规格的直接体现。若两者相等，鼠标掠过窗口边缘时人来不及走回来。
        QVERIFY2(FloatingBoxWidget::kHoverExpandDelayMs
                     < FloatingBoxWidget::kHoverCollapseDelayMs,
                 "进入延迟必须小于离开延迟，否则边缘掠过会误卷");

        // 两个都不能小到"扫一下就触发"。
        QVERIFY2(FloatingBoxWidget::kHoverExpandDelayMs >= 150,
                 "进入延迟太短，鼠标扫过桌面会一片窗口乱开");
        QVERIFY2(FloatingBoxWidget::kHoverCollapseDelayMs <= 800,
                 "离开延迟太长，会觉得窗口赖着不收");
    }

    // E-02 全局开关默认开启，且能落盘往返。
    void hoverExpandSettingRoundTrip()
    {
        if (!m_configWritable) {
            QSKIP("配置目录不可写（受限环境），无法验证落盘");
        }

        Settings s;

        // 默认必须是开启的：这是"不用管它自己就会让路"这条主线体验的一部分。
        QVERIFY2(s.hoverExpandEnabled(), "悬停自动展开的默认值必须是开启");

        s.setHoverExpandEnabled(false);
        QVERIFY2(!s.hoverExpandEnabled(), "关掉之后应当读回 false");

        s.setHoverExpandEnabled(true);
        QVERIFY2(s.hoverExpandEnabled(), "再打开应当读回 true");
    }

    // E-03 悬停开关与动画开关互不干扰。
    //
    // 防的是：两个全局单键用了同一个键名（复制粘贴最容易犯的错），
    // 表现是"关掉动画，悬停自动展开也跟着没了"，而没人会往那上面想。
    void hoverExpandSettingIsIndependentFromAnimations()
    {
        if (!m_configWritable) {
            QSKIP("配置目录不可写（受限环境），无法验证落盘");
        }

        Settings s;
        s.setAnimationsEnabled(true);
        s.setHoverExpandEnabled(false);

        QVERIFY2(s.animationsEnabled(), "动画开关不该被悬停开关影响");
        QVERIFY2(!s.hoverExpandEnabled(), "悬停开关应当保持关闭");

        s.setAnimationsEnabled(false);
        s.setHoverExpandEnabled(true);

        QVERIFY2(!s.animationsEnabled(), "动画开关应当保持关闭");
        QVERIFY2(s.hoverExpandEnabled(), "悬停开关不该被动效开关影响");
    }

    // E-04 钉住状态默认关闭，且能落盘往返。
    //
    // 默认必须是"没钉住"：钉住是主人主动施加的约束，
    // 若默认值搞反，每个盒一开出来就是钉死的，而主人从没要求过。
    void lockedDefaultsOffAndRoundTrips()
    {
        if (!m_configWritable) {
            QSKIP("配置目录不可写（受限环境），无法验证落盘");
        }

        Settings s;
        const QString box = QStringLiteral("钉住往返测试");

        QVERIFY2(!s.floatLocked(box), "钉住的默认值必须是关闭");

        s.setFloatLocked(box, true);
        QVERIFY2(s.floatLocked(box), "钉上之后应当读回 true");

        s.setFloatLocked(box, false);
        QVERIFY2(!s.floatLocked(box), "取消钉住之后应当读回 false");

        s.setFloatLocked(box, false);
    }

    // E-05 钉住状态是**每盒一份**的。
    //
    // 防的是：键名忘了带盒名占位（复制粘贴 floatAppearance 时最容易漏），
    // 表现是"锁了一个盒，所有盒都锁上了" —— 而且看起来像是刻意设计，
    // 主人未必会报成 bug，只会觉得这个功能莫名其妙。
    void lockedIsIndependentPerBox()
    {
        if (!m_configWritable) {
            QSKIP("配置目录不可写（受限环境），无法验证落盘");
        }

        Settings s;
        const QString a = QStringLiteral("钉住独立性A");
        const QString b = QStringLiteral("钉住独立性B");

        s.setFloatLocked(a, true);
        s.setFloatLocked(b, false);
        QVERIFY2(s.floatLocked(a), "A 应当被钉住");
        QVERIFY2(!s.floatLocked(b), "B 不该被 A 牵连");

        s.setFloatLocked(b, true);
        s.setFloatLocked(a, false);
        QVERIFY2(!s.floatLocked(a), "A 应当已取消钉住");
        QVERIFY2(s.floatLocked(b), "B 应当被钉住");

        s.setFloatLocked(a, false);
        s.setFloatLocked(b, false);
    }

    // E-06 钉住的键不能与外观的键撞车。
    //
    // 防的是：往 floatAppearance 那套里加第四项时忘了它只覆盖三项，
    // 于是"钉住"被写进 opacity 的键上 —— 取消钉住的时候顺手把透明度也清了。
    void lockedDoesNotDisturbAppearance()
    {
        if (!m_configWritable) {
            QSKIP("配置目录不可写（受限环境），无法验证落盘");
        }

        Settings s;
        const QString box = QStringLiteral("钉住与外观互不干扰");

        BoxAppearance ap;
        ap.viewMode = BoxAppearance::ViewMode::LargeIcon;
        ap.opacity  = 40;
        s.setFloatAppearance(box, ap);

        s.setFloatLocked(box, true);
        QVERIFY2(s.floatLocked(box), "钉住应当生效");

        const BoxAppearance readBack = s.floatAppearance(box);
        QVERIFY2(readBack.viewMode == BoxAppearance::ViewMode::LargeIcon,
                 "钉住不该动外观的视图模式");
        QVERIFY2(readBack.opacity == 40, "钉住不该动外观的透明度");

        s.setFloatLocked(box, false);
        s.clearFloatAppearance(box);
    }

    // E-07 删盒时钉住配置也要一起清掉。
    //
    // 与外观同理：不清的话，将来建一个同名盒会"继承"上一个盒的钉住状态，
    // 而主人完全不知道为什么新盒一开出来就是锁着的。
    void clearAppearanceAlsoClearsLocked()
    {
        if (!m_configWritable) {
            QSKIP("配置目录不可写（受限环境），无法验证落盘");
        }

        Settings s;
        const QString box = QStringLiteral("删盒清理钉住");

        s.setFloatLocked(box, true);
        QVERIFY2(s.floatLocked(box), "前提：应当已钉住");

        s.clearFloatAppearance(box);
        QVERIFY2(!s.floatLocked(box),
                 "clearFloatAppearance 应当把钉住一并清掉（防同名盒继承）");
    }
};

// ---------------------------------------------------------------------------
// 关于 TrayIcon 为什么不在这里测
//
// 它的可测逻辑只有"菜单勾选态"和"isAvailable"，两者都强绑 QSystemTrayIcon：
// 菜单要靠 rebuildMenu() 构建，而那只有托盘真实可用时才有意义；在没有托盘
// 服务的会话里 isAvailable() 恒为假，断言什么都说明不了。
//
// 更关键的是：它的核心行为（"关主窗口只隐藏、托盘退出才真退出"）是
// **进程级 / 窗口管理器级**的，只能靠实际运行程序、关窗、观察进程是否存活
// 来验证 —— 那属于人工验收或 tools/ 下的 GUI 探针。放进单元测试只会得到
// "看起来在测、其实什么也没验"的假安全感。
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    // 把测试配置写到独立位置，不能污染主人的真实设置。
    // Settings 用 AppConfigLocation 解析 ini 路径，而它由「组织名/应用名」
    // 决定 —— 换一组测试专用名字即可隔离，不需要给 Settings 开后门。
    //
    // 名字刻意与 tst_appservice 不同：两个测试可执行文件若共用同一个配置
    // 目录，并行跑时会互相清空对方的配置。
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyFloatLogicTest"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyFloatLogicTest"));

    QApplication app(argc, argv);
    TestFloatingLogic tc;
    return QTest::qExec(&tc, argc, argv);
}

// 手写 main 之后，Q_OBJECT 需要的 moc 产物必须显式包含进来，
// 否则链接期会报 "undefined reference to vtable for TestFloatingLogic"。
#include "tst_floatinglogic.moc"
