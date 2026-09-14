#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QEnterEvent>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QRegion>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QVariantAnimation>

#include "appservice.h"
#include "boxmanager.h"
#include "floatingboxmanager.h"
#include "floatingboxwidget.h"
#include "floatinghoveroverlay.h"

// ---------------------------------------------------------------------------
// hover_feedback_diag —— 悬停「光影描边」触感的动态行为验证。
//
// 【为什么必须有这个探针】
//
// 触感的三条核心承诺都只能在**真事件循环**里验证：
//   1) 连续 / 快速进出后进度要收敛到 0 或 1，不能卡在中途；
//   2) 快速跨多个浮窗时每个浮窗各自收敛，不互相串味；
//   3) 整个过程**不得**改动窗口的尺寸 / 位置 / 透明度 / 遮罩。
//
// 单测（tst_floatinglogic）只能验证配置项的编解码与归一化，对上面三条
// 一个字都说明不了。所以这里用 QApplication::sendEvent 手工投递
// QEnterEvent / QEvent::Leave，再用 QEventLoop + QTimer 推进真实时间，
// 让 QVariantAnimation 自然跑完。
//
// 【⚠️ 已知局限 —— 不要把它当成"触感已经验收"】
//
// 手工投递事件绕过了窗口系统。本探针验证的是"收到 enter/leave 之后的
// 逻辑对不对"，而不是"真实鼠标移动会不会产生 enter/leave"，更不是
// "光晕好不好看、强度档位是否合适"。那些只能人工把鼠标移上去看。
//
// 【触感为什么不会干扰浮窗推动】
//
// 光影严格约束在 FloatingHoverOverlay 这个**子控件**内部：它既不改顶层
// 窗口的几何 / 透明度，也不调用 setMask()。本探针第 [5] / [6] 段就是
// 把这条约束钉成断言 —— 一旦有人日后把光晕改成"直接画在窗口上"，
// 那两段会立刻变红。
// ---------------------------------------------------------------------------

static int gPass = 0;
static int gFail = 0;

static void check(bool ok, const QString &what, QTextStream &out)
{
    if (ok) { ++gPass; out << "  [PASS] " << what << "\n"; }
    else    { ++gFail; out << "  [FAIL] " << what << "\n"; }
    out.flush();
}

static void pump(int ms)
{
    QEventLoop t;
    QTimer::singleShot(ms, &t, &QEventLoop::quit);
    t.exec();
}

// 把光标移进窗口内部。
//
// ⚠️ 必须真的移动光标：FloatingBoxWidget::refreshHoverGlow() 用的是
// "光标位置是否落在 rect() 内"这个几何判断，而不是 underMouse()。
// 只发事件不动光标，refreshHoverGlow 会算出 inside == false。
static void moveCursorInto(const QWidget *w)
{
    QCursor::setPos(w->mapToGlobal(QPoint(30, 12)));
    pump(40);
}

static void moveCursorAway(const QWidget *w)
{
    QCursor::setPos(w->mapToGlobal(QPoint(w->width() + 700, w->height() + 500)));
    pump(40);
}

static void sendEnter(QWidget *w)
{
    const QPointF local(30, 12);
    const QPointF global(w->mapToGlobal(local.toPoint()));
    QEnterEvent ev(local, local, global);
    QApplication::sendEvent(w, &ev);
}

static void sendLeave(QWidget *w)
{
    QEvent ev(QEvent::Leave);
    QApplication::sendEvent(w, &ev);
}

static FloatingHoverOverlay *overlayOf(FloatingBoxWidget *fw)
{
    return fw ? fw->findChild<FloatingHoverOverlay *>() : nullptr;
}

// 把覆盖层离屏渲染出来，用于回答"到底画没画"。
// 覆盖层是子控件、背景透明，所以未绘制时整张图 alpha 应全为 0。
static QImage renderOverlay(FloatingHoverOverlay *ov)
{
    QImage img(ov->size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    ov->render(&img);
    return img;
}

static bool imageHasVisiblePixels(const QImage &img)
{
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(line[x]) > 0)
                return true;
        }
    }
    return false;
}

static bool approx(qreal a, qreal b, qreal eps = 0.002)
{
    return qAbs(a - b) <= eps;
}

namespace {

// 造 N 个浮窗的夹具：独立配置目录、独立 AppService，保证状态干净可重复。
struct Fixture
{
    AppService           service;
    QString              root;
    FloatingBoxManager  *mgr = nullptr;
    QStringList          names;
    QHash<QString, QString> paths;
    bool                 ok = false;

    Fixture(const QString &tag, int boxCount)
    {
        root = QStringLiteral("F:/QtProject/DeskTidy/_hoverfbdiag_") + tag;
        QDir(root).removeRecursively();
        QDir().mkpath(root);

        QString err;
        BoxManager::ensureRoot(root + QStringLiteral("/boxes"), &err);

        for (int i = 0; i < boxCount; ++i) {
            const QString name = QStringLiteral("触感%1").arg(i + 1);
            StorageBox box;
            if (!BoxManager::createBox(root + QStringLiteral("/boxes"), name, &box, &err))
                return;

            QFile f(box.path + QStringLiteral("/a.txt"));
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) { f.write("x"); f.close(); }

            names << name;
            paths.insert(name, box.path);
        }

        ok = (names.size() == boxCount);
    }

    ~Fixture()
    {
        // 先销毁 manager（它拥有所有浮窗），再删临时盒目录。
        // 顺序反过来的话浮窗可能在退出时又往已删除的目录写一次。
        delete mgr;
        QDir(root).removeRecursively();
    }

    FloatingBoxWidget *widgetFor(const QString &name) const
    {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (auto *fb = qobject_cast<FloatingBoxWidget *>(w)) {
                if (fb->boxName() == name)
                    return fb;
            }
        }
        return nullptr;
    }

    QList<FloatingBoxWidget *> openAll()
    {
        QList<FloatingBoxWidget *> out;
        for (const QString &n : names) {
            mgr->openBox(n, paths.value(n));
            pump(120);
            if (FloatingBoxWidget *w = widgetFor(n))
                out << w;
        }
        return out;
    }
};

// 给浮窗换一份外观并等它同步完（applyAppearance 内部会刷新覆盖层）。
void applyLook(FloatingBoxWidget *fw, const BoxAppearance &app, int settleMs = 60)
{
    fw->applyAppearance(app);
    pump(settleMs);
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyHoverFeedbackDiag"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyHoverFeedbackDiag"));

    // 每次运行先清掉本探针自己的配置 ini。
    //
    // 为什么不靠"每个夹具用不同的盒名"：多个夹具共用同一个组织名 / 应用名，
    // 就共用同一份 ini。不清的话前一个夹具写下的几何与外观会漏进后一个，
    // 断言就变成"拿被污染的状态自比"，看着在测、其实什么也没验。
    // 组织名是本探针专用的，绝不会碰到主人的真实配置。
    {
        const QString cfgDir =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        if (!cfgDir.isEmpty())
            QDir(cfgDir).removeRecursively();
    }

    QTextStream out(stdout);
    out << "=== 浮窗悬停触感（光影描边）诊断 ===\n\n";

    // =======================================================================
    out << "[1] 连续进出：进度必须收敛，不能卡在中途\n";
    //
    // 主人快速在窗口边缘划进划出时，每个 enter/leave 都会重启动画。
    // 若 applyTargetProgress() 里少了"先 stop 再 start"，前一段动画会
    // 和新的抢同一个属性，进度就会抖甚至卡住。
    // =======================================================================
    {
        Fixture fx(QStringLiteral("settle"), 1);
        if (!fx.ok) { check(false, QStringLiteral("夹具创建成功"), out); return 1; }
        fx.mgr = new FloatingBoxManager(&fx.service);

        FloatingBoxWidget *fw = fx.openAll().value(0);
        if (!fw) { check(false, QStringLiteral("浮窗打开成功"), out); return 1; }

        FloatingHoverOverlay *ov = overlayOf(fw);
        check(ov != nullptr, QStringLiteral("找到触感覆盖层"), out);
        if (!ov) return 1;

        check(approx(ov->hoverProgress(), 0.0),
              QStringLiteral("初始进度为 0（未悬停不画）"), out);

        // 连着快速进出 3 次，每次只停 20ms —— 远短于 120ms 的动画时长。
        for (int i = 0; i < 3; ++i) {
            moveCursorInto(fw);
            sendEnter(fw);
            pump(20);
            moveCursorAway(fw);
            sendLeave(fw);
            pump(20);
        }
        pump(400);
        out << "  3 次快速进出后进度 = " << ov->hoverProgress() << "\n";
        check(approx(ov->hoverProgress(), 0.0),
              QStringLiteral("★ 连续快速进出后收敛到 0（没有卡在中间）"), out);

        // 最后稳稳停在里面，应当收敛到 1。
        moveCursorInto(fw);
        sendEnter(fw);
        pump(400);
        out << "  稳定悬停后进度 = " << ov->hoverProgress() << "\n";
        check(approx(ov->hoverProgress(), 1.0),
              QStringLiteral("★ 稳定悬停后收敛到 1"), out);

        // 真的画出来了：离屏渲染应当有可见像素。
        const QImage lit = renderOverlay(ov);
        check(imageHasVisiblePixels(lit),
              QStringLiteral("悬停时覆盖层确实有绘制（离屏渲染有可见像素）"), out);

        moveCursorAway(fw);
        sendLeave(fw);
        pump(400);
        const QImage dark = renderOverlay(ov);
        check(!imageHasVisiblePixels(dark),
              QStringLiteral("★ 消退后覆盖层不再绘制任何像素"), out);
    }
    out << "\n";

    // =======================================================================
    out << "[2] 快速跨多个浮窗：各自收敛，不互相串味\n";
    // =======================================================================
    {
        Fixture fx(QStringLiteral("multi"), 3);
        if (!fx.ok) { check(false, QStringLiteral("三浮窗夹具创建成功"), out); return 1; }
        fx.mgr = new FloatingBoxManager(&fx.service);

        const QList<FloatingBoxWidget *> fws = fx.openAll();
        if (fws.size() != 3) { check(false, QStringLiteral("三个浮窗都打开了"), out); return 1; }

        // 拉开距离，避免靠在一起时窗口系统给的 enter/leave 互相干扰。
        int x = 120;
        for (FloatingBoxWidget *w : fws) {
            w->move(x, 140);
            x += 320;
            pump(60);
        }

        QList<FloatingHoverOverlay *> ovs;
        for (FloatingBoxWidget *w : fws)
            ovs << overlayOf(w);
        for (int i = 0; i < ovs.size(); ++i)
            check(ovs[i] != nullptr, QStringLiteral("浮窗 #%1 有覆盖层").arg(i + 1), out);

        // 光标在三个窗口之间高速横扫 5 轮。
        for (int round = 0; round < 5; ++round) {
            for (FloatingBoxWidget *w : fws) {
                moveCursorInto(w);
                sendEnter(w);
                pump(12);
                sendLeave(w);
                pump(12);
            }
        }

        // 最后停在第三个上，其余两个应当完全消退。
        FloatingBoxWidget *last = fws.last();
        moveCursorInto(last);
        sendEnter(last);
        pump(500);

        for (int i = 0; i < fws.size(); ++i) {
            const qreal want = (fws[i] == last) ? 1.0 : 0.0;
            out << "  浮窗 #" << (i + 1) << " 进度 = " << ovs[i]->hoverProgress()
                << "（期望 " << want << "）\n";
            check(approx(ovs[i]->hoverProgress(), want),
                  QStringLiteral("★ 快速跨窗后浮窗 #%1 收敛到 %2")
                      .arg(i + 1).arg(want, 0, 'g', 2), out);
        }
    }
    out << "\n";

    // =======================================================================
    out << "[3] 关闭触感（HoverEffect::Off）：不绘制\n";
    // =======================================================================
    {
        Fixture fx(QStringLiteral("off"), 1);
        if (!fx.ok) { check(false, QStringLiteral("夹具创建成功"), out); return 1; }
        fx.mgr = new FloatingBoxManager(&fx.service);

        FloatingBoxWidget *fw = fx.openAll().value(0);
        if (!fw) { check(false, QStringLiteral("浮窗打开成功"), out); return 1; }
        FloatingHoverOverlay *ov = overlayOf(fw);
        if (!ov) { check(false, QStringLiteral("找到覆盖层"), out); return 1; }

        BoxAppearance off;
        off.hoverEffect = BoxAppearance::HoverEffect::Off;
        applyLook(fw, off);

        moveCursorInto(fw);
        sendEnter(fw);
        pump(400);

        out << "  关闭触感并悬停 400ms 后进度 = " << ov->hoverProgress() << "\n";
        check(approx(ov->hoverProgress(), 0.0),
              QStringLiteral("★ 关闭触感时进度恒为 0"), out);
        check(!imageHasVisiblePixels(renderOverlay(ov)),
              QStringLiteral("★ 关闭触感时覆盖层一个像素都不画"), out);

        // 再切回开启，应当立刻恢复能力（不必重建浮窗）。
        applyLook(fw, BoxAppearance{});
        pump(300);
        check(approx(ov->hoverProgress(), 1.0),
              QStringLiteral("切回光影后光标还在窗口上，光晕立刻恢复"), out);
    }
    out << "\n";

    // =======================================================================
    out << "[4] 全局关动画：立即出现 / 立即消失，无中间值\n";
    //
    // 产品规格：「启用界面动画」是最高开关。关掉之后触感不再播放过渡，
    // 而是瞬间到位 —— 对弱机 / 远程桌面这类场景，这是唯一正确的取舍。
    // =======================================================================
    {
        Fixture fx(QStringLiteral("noanim"), 1);
        if (!fx.ok) { check(false, QStringLiteral("夹具创建成功"), out); return 1; }
        fx.mgr = new FloatingBoxManager(&fx.service);

        FloatingBoxWidget *fw = fx.openAll().value(0);
        if (!fw) { check(false, QStringLiteral("浮窗打开成功"), out); return 1; }
        FloatingHoverOverlay *ov = overlayOf(fw);
        if (!ov) { check(false, QStringLiteral("找到覆盖层"), out); return 1; }

        fw->setAnimationsEnabled(false);
        // 故意挑最慢的档位：若实现还依赖时长，这里就会露馅。
        BoxAppearance look;
        look.animationSpeed = BoxAppearance::AnimationSpeed::Relaxed;
        applyLook(fw, look);

        moveCursorInto(fw);
        sendEnter(fw);

        // 不 pump 就采样：关动画时必须已经到位。
        out << "  关动画后 enter 立刻采样进度 = " << ov->hoverProgress() << "\n";
        check(approx(ov->hoverProgress(), 1.0),
              QStringLiteral("★ 关动画时进入立即到 1（无中间值）"), out);

        auto *anim = ov->findChild<QVariantAnimation *>();
        check(anim && anim->state() != QAbstractAnimation::Running,
              QStringLiteral("★ 关动画时动画对象处于停止态（不空转）"), out);

        moveCursorAway(fw);
        sendLeave(fw);
        out << "  关动画后 leave 立刻采样进度 = " << ov->hoverProgress() << "\n";
        check(approx(ov->hoverProgress(), 0.0),
              QStringLiteral("★ 关动画时离开立即到 0（无中间值）"), out);

        fw->setAnimationsEnabled(true);
        pump(30);
        check(approx(ov->hoverProgress(), 0.0),
              QStringLiteral("重新开启动画后仍停在 0"), out);
    }
    out << "\n";

    // =======================================================================
    out << "[5] 100 次进出：窗口尺寸 / 位置 / 透明度 / 遮罩必须纹丝不动\n";
    //
    // 这是本设计最重要的一条约束。触感若碰了上述任何一项，浮窗之间的
    // 推动与让位逻辑就会重新开始打架（那正是历史上出重叠 bug 的根源）。
    //
    // 这一段直接驱动覆盖层进度而不是真的搬鼠标：跑 100 轮，每轮都经过
    // 完整的动画 + 重绘路径，但不去动主人的真实光标。
    // =======================================================================
    {
        Fixture fx(QStringLiteral("stable"), 1);
        if (!fx.ok) { check(false, QStringLiteral("夹具创建成功"), out); return 1; }
        fx.mgr = new FloatingBoxManager(&fx.service);

        FloatingBoxWidget *fw = fx.openAll().value(0);
        if (!fw) { check(false, QStringLiteral("浮窗打开成功"), out); return 1; }
        FloatingHoverOverlay *ov = overlayOf(fw);
        if (!ov) { check(false, QStringLiteral("找到覆盖层"), out); return 1; }

        // 先让它彻底静下来，再取基准。
        pump(200);
        const QSize  size0  = fw->size();
        const QPoint pos0   = fw->pos();
        const qreal  op0    = fw->windowOpacity();
        const QRegion mask0 = fw->mask();

        out << "  基准：size=" << size0.width() << "x" << size0.height()
            << " pos=(" << pos0.x() << "," << pos0.y() << ")"
            << " opacity=" << op0 << " maskNull=" << (mask0.isEmpty() ? "yes" : "no") << "\n";

        for (int i = 0; i < 100; ++i) {
            ov->setActive(true);
            pump(3);
            ov->setActive(false);
            pump(3);
        }
        pump(400);

        const bool sizeSame  = (fw->size() == size0);
        const bool posSame   = (fw->pos() == pos0);
        const bool opSame    = approx(fw->windowOpacity(), op0);
        const bool maskSame  = (fw->mask() == mask0);

        out << "  100 轮后：size=" << fw->size().width() << "x" << fw->size().height()
            << " pos=(" << fw->pos().x() << "," << fw->pos().y() << ")"
            << " opacity=" << fw->windowOpacity()
            << " maskNull=" << (fw->mask().isEmpty() ? "yes" : "no") << "\n";

        check(sizeSame, QStringLiteral("★ 100 次进出后窗口尺寸不变"), out);
        check(posSame,  QStringLiteral("★ 100 次进出后窗口位置不变"), out);
        check(opSame,   QStringLiteral("★ 100 次进出后窗口透明度不变"), out);
        check(maskSame, QStringLiteral("★ 100 次进出后窗口遮罩不变（悬停期间绝不 setMask）"), out);
        check(approx(ov->hoverProgress(), 0.0),
              QStringLiteral("100 轮后光晕彻底收敛"), out);

        // 覆盖层自己应当始终铺满客户区，且位于最上层。
        check(ov->geometry() == fw->rect(),
              QStringLiteral("覆盖层始终铺满浮窗客户区"), out);
    }
    out << "\n";

    // =======================================================================
    out << "[6] 快速跨窗：不得改写几何、不得重新引入重叠\n";
    //
    // 把三个浮窗摆成一排、互不重叠，然后让光标在它们之间高速横扫。
    // 如果触感偷偷动了窗口几何（或触发了让位），这里立刻会看到几何漂移
    // 或两两相交。
    // =======================================================================
    {
        Fixture fx(QStringLiteral("overlap"), 3);
        if (!fx.ok) { check(false, QStringLiteral("三浮窗夹具创建成功"), out); return 1; }
        fx.mgr = new FloatingBoxManager(&fx.service);

        const QList<FloatingBoxWidget *> fws = fx.openAll();
        if (fws.size() != 3) { check(false, QStringLiteral("三个浮窗都打开了"), out); return 1; }

        // 拉开成一排。
        int x = 120;
        for (FloatingBoxWidget *w : fws) {
            w->move(x, 160);
            x += 320;
            pump(80);
        }
        pump(400);

        QList<QRect> before;
        for (FloatingBoxWidget *w : fws)
            before << w->frameGeometry();

        // 高速横扫 6 轮。
        for (int round = 0; round < 6; ++round) {
            for (FloatingBoxWidget *w : fws) {
                moveCursorInto(w);
                sendEnter(w);
                pump(10);
                sendLeave(w);
                pump(10);
            }
        }
        moveCursorAway(fws.last());
        pump(500);

        bool geometryStable = true;
        for (int i = 0; i < fws.size(); ++i) {
            if (fws[i]->frameGeometry() != before[i]) {
                geometryStable = false;
                out << "  浮窗 #" << (i + 1) << " 几何漂移："
                    << before[i].x() << "," << before[i].y() << " -> "
                    << fws[i]->frameGeometry().x() << "," << fws[i]->frameGeometry().y() << "\n";
            }
        }
        check(geometryStable, QStringLiteral("★ 快速跨窗过程中所有浮窗几何纹丝不动"), out);

        bool anyOverlap = false;
        for (int i = 0; i < fws.size() && !anyOverlap; ++i) {
            for (int j = i + 1; j < fws.size(); ++j) {
                if (fws[i]->frameGeometry().intersects(fws[j]->frameGeometry())) {
                    anyOverlap = true;
                    out << "  浮窗 #" << (i + 1) << " 与 #" << (j + 1) << " 重叠\n";
                    break;
                }
            }
        }
        check(!anyOverlap, QStringLiteral("★ 快速跨窗后没有重新引入重叠"), out);
    }
    out << "\n";

    out << "=== 结果：" << gPass << " 通过 / " << gFail << " 失败 ===\n";
    out << "\n[说明] 本探针验证的是「收到 enter/leave 之后的逻辑」以及\n";
    out << "       「触感是否改动了窗口几何」。真实鼠标移动是否产生这些事件、\n";
    out << "       光晕的观感与强度档位是否合适，需要人工把鼠标移上去确认。\n";
    out.flush();

    return gFail == 0 ? 0 : 1;
}