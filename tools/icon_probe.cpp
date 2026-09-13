#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QImage>
#include <QTextStream>

#include "appservice.h"
#include "boxmanager.h"
#include "itemlistwidget.h"

// ---------------------------------------------------------------------------
// icon_probe —— 验证「盒内条目的图标不会互相串号」。
//
// 【守的是什么 bug】
//
// 主人实测报的：**每次存入新东西，收纳盒里的文件图标就错乱**。
// 根因是 setItems 里那个"按后缀缓存图标"的策略：
//     const QString key = QFileInfo(entry.name).suffix().toLower();
// 它假设"同后缀 = 同图标"，但 QFileIconProvider::icon(文件) 给的是
// **每个文件各自**的图标 —— .exe 有各自的嵌入图标，.lnk 指向各自的目标。
// 于是同后缀的文件互相串号，而"谁先被扫到"由目录遍历顺序决定；
// 每次收纳都重建列表 -> 图标跟着重排 -> 看起来就是"错乱"。
//
// 【判据为什么这么定】
//
// ⚠️ 这里换过三版判据，前两版都是错的，记下来免得再走：
//
//   v1 比 QIcon::cacheKey() —— 全错。它标识 QIcon **对象实例**，
//      同一个文件取两次就是两个不同的 key，于是全部条目假报"不符"。
//
//   v2 把"列表里的图标"与"另取一次 QFileIconProvider::icon()"逐像素比
//      —— 仍然错，而且错得很有迷惑性：列表模式报 .exe 不符、
//      大图标模式报**全部**不符。查下来是两个原因，都不是产品的错：
//        * 大图标模式：产品会先要一张大位图再重包成单尺寸 QIcon
//          （防糊，见 setItems 里的说明），而我的参照没走这一步，
//          于是 shown=64x64 而 expect=256x256 —— 尺寸都对不上，
//          比的是两条不同的渲染路径。
//        * 列表模式：尺寸与 availableSizes 都完全一致，差的是像素。
//      另外单独测过（tools/icondiag4）：同一个 exe 取两次，16/32/64
//      三个尺寸下像素**完全相同**（0 差异）；两个不同 exe 之间是
//      1024/1024 全不同。所以图标内容本身没问题。
//
//   结论：拿"另一条路径取到的图标"当参照，测的是**我的参照对不对**，
//   不是产品对不对。所以 v3 换成下面这个**自洽**的判据：
//
//   **对盒内每一个文件，产品给的图标都必须等于"把同一个文件单独喂给
//   同一个 ItemListWidget"时它给的图标。**
//   两边走的是完全相同的代码路径，任何差异都只能是"串号"造成的。
//   这样既对等，又正好命中要守的那个 bug。
//
// 另加一条**身份**检查：图标内容不同的两个文件，它们在列表里的图标
// 也必须不同。这一条是直接照着主人看到的现象写的。
// ---------------------------------------------------------------------------

static int gPass = 0;
static int gFail = 0;

static void check(bool ok, const QString &what, QTextStream &out)
{
    if (ok) { ++gPass; out << "  [PASS] " << what << "\n"; }
    else    { ++gFail; out << "  [FAIL] " << what << "\n"; }
    out.flush();
}

// 建一个列表控件（走产品的真实代码路径），喂给它给定的条目。
static ItemListWidget *makeList(const QList<DesktopEntry> &items,
                                BoxAppearance::ViewMode mode,
                                QWidget *parent = nullptr)
{
    ItemListWidget::Options opts;
    opts.doubleClick  = ItemListWidget::DoubleClickAction::Open;
    opts.draggableOut = true;
    opts.viewMode     = mode;
    opts.iconSize     = 0;

    auto *w = new ItemListWidget(opts, parent);
    w->setItems(items);
    return w;
}

// 把列表里某项的图标渲染成图。
//
// ⚠️ 渲染尺寸对所有项取**同一个值**，而且不依赖该项自己报的尺寸 ——
// 否则尺寸本身就成了变量，比出来的差异分不清是内容还是缩放。
static QImage renderOf(const QIcon &ic, int px)
{
    return ic.pixmap(px, px).toImage();
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyIconProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyIconProbe"));

    QTextStream out(stdout);
    out << "=== 盒内条目图标不会串号 ===\n\n";

    const QString root = QStringLiteral("F:/QtProject/DeskTidy/_iconprobe");
    QDir(root).removeRecursively();
    QDir().mkpath(root);

    AppService service;
    QString err;
    BoxManager::ensureRoot(root + QStringLiteral("/boxes"), &err);

    StorageBox box;
    BoxManager::createBox(root + QStringLiteral("/boxes"), QStringLiteral("图标"), &box, &err);

    // 样本要包含"**后缀决定不了图标**"的文件，否则测不出串号。
    //
    // 踩过：第一版只造 a.txt/b.txt/c.txt，即使是旧的错误缓存也能全对 ——
    // 因为同后缀的普通文件图标本来就一样，探针恒过、什么也没证明
    //（实测：注入旧 bug 后仍然全 PASS）。
    // 真正会串号的是 .exe（各有嵌入图标）与无后缀文件（缓存键都是空串）。
    const QStringList plainNames{
        QStringLiteral("a.txt"), QStringLiteral("b.txt"), QStringLiteral("c.txt"),
        QStringLiteral("x.md"),  QStringLiteral("y.md"),
        QStringLiteral("noext1"), QStringLiteral("noext2"), QStringLiteral("noext3")};
    for (const QString &n : plainNames) {
        QFile f(box.path + QLatin1Char('/') + n);
        if (f.open(QIODevice::WriteOnly)) { f.write("x"); f.close(); }
    }

    // 真实的 exe：图标各不相同，是串号最明显的样本。
    // 用真文件而不是伪造的 —— 嵌入图标由 shell 提供，伪造不出来。
    const QString sysDir = QStringLiteral("C:/Windows/System32");
    const QStringList exeSources{
        QStringLiteral("notepad.exe"), QStringLiteral("calc.exe"),
        QStringLiteral("cmd.exe"), QStringLiteral("charmap.exe")};
    int copiedExe = 0;
    for (const QString &exe : exeSources) {
        if (QFile::copy(sysDir + QLatin1Char('/') + exe,
                        box.path + QLatin1Char('/') + exe)) {
            ++copiedExe;
        }
    }
    out << "样本：普通文件 " << plainNames.size()
        << " 个 + 真实 exe " << copiedExe << " 个\n";

    // ---- .url 样本：每个带一个**各不相同**的 IconFile ----
    //
    // 守的是另一个 bug（主人报的"盒里图标全变白纸"）：
    // QFileIconProvider 不解析 .url 里的 IconFile=，只按后缀给通用图标，
    // 于是十几个网址快捷方式长得一模一样。
    //
    // ⚠️ IconFile 必须指向**真正的 .ico**，不能指向 .exe。
    // 踩过：第一版样本用了 System32 下的 exe 当素材，结果 4 个 .url
    // 全部报"图标与 IconFile 不一致"，看着像产品没修好 ——
    // 实际是 QIcon 根本不解析 exe 的嵌入图标（它只认图像文件），
    // QIcon("xxx.exe") 返回 null，于是走了回退分支。
    // 而主人盒里的 IconFile 指向的正是 Steam 的 .ico，那条路是通的。
    //
    // 所以这里用 Steam 目录下的真实 .ico（主人机器上就有 72 个），
    // 与真实场景完全一致。
    const QString steamIcoDir = QStringLiteral("C:/Program Files (x86)/Steam/steam/games");
    QStringList icoPool;
    {
        QDir d(steamIcoDir);
        const QFileInfoList icos = d.entryInfoList({QStringLiteral("*.ico")},
                                                   QDir::Files, QDir::Size);
        for (const QFileInfo &fi : icos) {
            icoPool << fi.absoluteFilePath();
            if (icoPool.size() >= 4) break;   // 四个够区分了
        }
    }

    int madeUrl = 0;
    if (icoPool.size() >= 2) {
        for (int i = 0; i < icoPool.size(); ++i) {
            QFile f(box.path + QStringLiteral("/链接%1.url").arg(i + 1));
            if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) continue;
            QTextStream ts(&f);
            ts.setEncoding(QStringConverter::Utf8);
            ts << "[InternetShortcut]\r\n"
               << "URL=steam://rungameid/" << (1000 + i) << "\r\n"
               << "IconFile=" << icoPool.at(i) << "\r\n"
               << "IconIndex=0\r\n";
            ts.flush();
            f.close();
            ++madeUrl;
        }
    } else {
        out << "⚠️ 找不到可用的 .ico 素材（" << steamIcoDir
            << "），.url 检查会退化\n";
    }
    out << "样本：.url " << madeUrl << " 个（各指向不同的 .ico）\n\n";

    const QList<DesktopEntry> all = BoxManager::listBoxItems(box.path);
    out << "盒内条目数 = " << all.size() << "\n\n";

    // 诊断：对照实验 —— 只放 4 个 exe（不放普通文件）时，图标对不对？
    //
    // 变量只剩一个：前面那 8 个普通文件是否污染了后面 exe 的图标。
    // 单独跑一次纯 exe 的列表，与整批的结果对比即可分辨。
    out << "===== 对照：只放 exe（不含普通文件）=====\n";
    {
        QList<DesktopEntry> exeOnly;
        for (const DesktopEntry &e : all) {
            if (QFileInfo(e.filePath).suffix().compare(QStringLiteral("exe"),
                                                       Qt::CaseInsensitive) == 0) {
                exeOnly << e;
            }
        }
        ItemListWidget *solo = makeList(exeOnly, BoxAppearance::ViewMode::List);
        {
            // 并排对照：同一个程序里，把"控件给的"与"直接问 provider 的"
            // 放在一起打印。这样不用跨程序猜，一眼看出是哪边不对。
            QFileIconProvider ref;
            for (int i = 0; i < solo->count(); ++i) {
                const QString path = solo->item(i)->data(Qt::UserRole).toString();
                const QImage fromWidget = renderOf(solo->item(i)->icon(), 16);
                const QImage fromProv = ref.icon(QFileInfo(path)).pixmap(16, 16).toImage();
                quint64 hw = 1469598103934665603ULL, hp = hw;
                for (int y = 0; y < fromWidget.height(); ++y)
                    for (int x = 0; x < fromWidget.width(); ++x) {
                        hw ^= static_cast<quint64>(fromWidget.pixel(x, y)); hw *= 1099511628211ULL;
                        hp ^= static_cast<quint64>(fromProv.pixel(x, y));   hp *= 1099511628211ULL;
                    }
                // 把 entry 的关键字段也打出来 —— 图标取法取决于 isDir。
                const DesktopEntry &src = exeOnly.at(i);
                out << "    " << solo->item(i)->text()
                    << "  控件[" << QString::number(hw, 16) << "]"
                    << "  provider[" << QString::number(hp, 16) << "]"
                    << "  相同=" << (fromWidget == fromProv ? "是" : "否")
                    << "  isDir=" << (src.isDir ? "true" : "false")
                    << "  size=" << src.size
                    << "\n";
            }
        }
        delete solo;
    }
    out << "\n";

    for (BoxAppearance::ViewMode mode : {BoxAppearance::ViewMode::List,
                                         BoxAppearance::ViewMode::LargeIcon}) {
        const QString modeName = (mode == BoxAppearance::ViewMode::List)
                                     ? QStringLiteral("列表模式")
                                     : QStringLiteral("大图标模式");
        const int px = BoxAppearance::defaultIconSizeFor(mode);
        out << "===== " << modeName << " =====\n";

        // 整批一次（产品的真实用法）。
        ItemListWidget *batch = makeList(all, mode);
        check(batch->count() == all.size(),
              QStringLiteral("列表条目数与磁盘一致"), out);

        // ---- 判据：逐个文件单独喂进去，图标必须与整批时一致 ----
        //
        // 两边走**完全相同**的代码路径，所以任何差异都只能来自"串号"。
        int mismatched = 0;
        int nullIcons = 0;

        for (int i = 0; i < batch->count(); ++i) {
            QListWidgetItem *item = batch->item(i);
            const QString path = item->data(Qt::UserRole).toString();

            if (item->icon().isNull()) {
                ++nullIcons;
                out << "  [FAIL] " << item->text() << " 没有图标\n";
                continue;
            }

            // 单独喂一个条目进去。
            const QList<DesktopEntry> one{all.at(i)};
            ItemListWidget *solo = makeList(one, mode);
            const QImage a = renderOf(item->icon(), px);
            const QImage b = renderOf(solo->item(0)->icon(), px);
            delete solo;

            if (a.isNull() || b.isNull() || a.size() != b.size() || a != b) {
                ++mismatched;
                out << "  [FAIL] " << item->text()
                    << " 在整批里的图标与它单独存在时不一致（被串号了）\n";
            }
        }

        check(nullIcons == 0, QStringLiteral("每个条目都有图标"), out);
        check(mismatched == 0,
              QStringLiteral("★ 整批与单独一致（没有互相串号）"), out);

        // ---- 身份检查：**不同程序**的图标不该显示成同一个 ----
        //
        // 这条直接照着主人看到的现象写。
        //
        // ⚠️ 参照物不能用"另取一次 QFileIconProvider"。
        // 踩过：v2 判据就是栽在这里，换成这条时又差点栽第二遍 ——
        // 参照走的是"多尺寸 QIcon 直接渲染"，而产品在图标模式下会先重包，
        // 两条路径对同一个文件给出的像素本来就可能不同；
        // 拿它当"该不该不同"的判据，会把纯路径差异报成串号。
        //（实测：那条判据让 calc.exe 与其余三个 exe 全部假报"相同"，
        //  导出图片一看，两边画的都是计算器图标，产品根本没错。）
        //
        // 所以参照物改用**文件自身的事实**：这里放的是四个不同的程序
        //（notepad / calc / cmd / charmap），它们在任何正常系统上都不该
        // 显示成同一个图标。这个判据不依赖 Qt 的渲染细节。
        //
        // 注意 calc.exe 在 Win11 上是 UWP 启动器，图标形状与其它几个明显不同，
        // 正好是个好的样本。
        int exePairs = 0;
        int exeSame = 0;
        {
            QList<int> exeRows;
            for (int i = 0; i < batch->count(); ++i) {
                const QString p = batch->item(i)->data(Qt::UserRole).toString();
                if (QFileInfo(p).suffix().compare(QStringLiteral("exe"),
                                                  Qt::CaseInsensitive) == 0) {
                    exeRows << i;
                }
            }
            out << "  盒内 exe 条目 " << exeRows.size() << " 个\n";

            for (int a = 0; a < exeRows.size(); ++a) {
                for (int b = a + 1; b < exeRows.size(); ++b) {
                    ++exePairs;
                    const QImage ia = renderOf(batch->item(exeRows[a])->icon(), px);
                    const QImage ib = renderOf(batch->item(exeRows[b])->icon(), px);
                    if (ia == ib) {
                        ++exeSame;
                        out << "  [FAIL] " << batch->item(exeRows[a])->text() << " 与 "
                            << batch->item(exeRows[b])->text()
                            << " 是两个不同程序，却显示了同一个图标\n";
                    }
                }
            }

            // 诊断：把每个 exe 在列表里的图标尺寸/可用尺寸打出来，
            // 并**当场再查一次**这个条目自己的路径。
            for (int r : exeRows) {
                const QIcon ic = batch->item(r)->icon();
                QString av;
                for (const QSize &s : ic.availableSizes())
                    av += QStringLiteral("%1x%2 ").arg(s.width()).arg(s.height());
                out << "    " << batch->item(r)->text()
                    << "  render=" << renderOf(ic, px).width()
                    << "  avail=[" << av.trimmed() << "]\n";
            }
        }
        check(exePairs > 0,
              QStringLiteral("样本里有多个 exe（否则这条没意义）"), out);
        check(exeSame == 0,
              QStringLiteral("★ 不同程序的图标确实互不相同"), out);

        // ---- .url 检查：各指向不同图标的 .url 不该显示成同一个 ----
        //
        // 守的是主人报的"盒里图标全变白纸"：QFileIconProvider 不解析
        // .url 里的 IconFile=，十几个网址快捷方式会长得一模一样。
        //
        // ⚠️ 判据同样是"文件自身的事实"：这些 .url 各自指向**不同的**
        // 图标文件，所以它们本来就该显示成不同的图标。
        // 不去问 provider 在不在意这件事 —— 它恰恰是不在意的那一方。
        {
            QList<QString> urlIconTargets;   // 期望的图标来源路径
            QList<int> urlRows;
            for (int i = 0; i < batch->count(); ++i) {
                const QString p = batch->item(i)->data(Qt::UserRole).toString();
                if (!p.endsWith(QStringLiteral(".url"), Qt::CaseInsensitive)) continue;
                urlRows << i;

                // 自己把 IconFile= 读出来当期望值
                QFile f(p);
                QString target;
                if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    QTextStream ts(&f);
                    ts.setEncoding(QStringConverter::Utf8);
                    while (!ts.atEnd()) {
                        const QString line = ts.readLine().trimmed();
                        if (line.startsWith(QStringLiteral("IconFile="),
                                            Qt::CaseInsensitive)) {
                            target = line.mid(9).trimmed();
                            break;
                        }
                    }
                }
                urlIconTargets << target;
            }

            out << "  盒内 .url 条目 " << urlRows.size() << " 个\n";

            // (a) 每个 .url 显示的图标必须等于它 IconFile 指向的那个文件
            int urlWrong = 0;
            for (int k = 0; k < urlRows.size(); ++k) {
                const QString target = urlIconTargets.at(k);
                if (target.isEmpty() || !QFile::exists(target)) {
                    out << "  [FAIL] " << batch->item(urlRows.at(k))->text()
                        << " 的 IconFile 取不到：" << target << "\n";
                    ++urlWrong;
                    continue;
                }
                const QImage shown = renderOf(batch->item(urlRows.at(k))->icon(), px);
                const QImage want = renderOf(QIcon(target), px);
                if (shown.isNull() || want.isNull() || shown != want) {
                    ++urlWrong;
                    out << "  [FAIL] " << batch->item(urlRows.at(k))->text()
                        << " 显示的图标与它 IconFile 指定的不一致\n";
                }
            }
            check(urlRows.size() > 1,
                  QStringLiteral("样本里有多个 .url（否则这条没意义）"), out);
            check(urlWrong == 0,
                  QStringLiteral("★ 每个 .url 显示的都是它自己指定的图标"), out);

            // (b) 指向不同图标的 .url，显示也必须不同
            int urlPairs = 0;
            int urlSame = 0;
            for (int a = 0; a < urlRows.size(); ++a) {
                for (int b = a + 1; b < urlRows.size(); ++b) {
                    ++urlPairs;
                    const QImage ia = renderOf(batch->item(urlRows.at(a))->icon(), px);
                    const QImage ib = renderOf(batch->item(urlRows.at(b))->icon(), px);
                    if (ia == ib) {
                        ++urlSame;
                        out << "  [FAIL] " << batch->item(urlRows.at(a))->text()
                            << " 与 " << batch->item(urlRows.at(b))->text()
                            << " 指向不同的图标，却显示成同一个\n";
                    }
                }
            }
            check(urlSame == 0,
                  QStringLiteral("★ 指向不同图标的 .url 显示得也不同（不再是白纸）"), out);
        }

        delete batch;
        out << "\n";
    }

    out << "=== 结果：" << gPass << " 通过 / " << gFail << " 失败 ===\n";
    out << "\n[说明] 本探针验证「图标身份没有串号」，\n";
    out << "       验证不了「图标画出来好不好看」—— 那需要人眼看。\n";
    out.flush();

    QDir(root).removeRecursively();
    return gFail == 0 ? 0 : 1;
}
