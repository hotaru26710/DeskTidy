#include <QApplication>
#include <QBitmap>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPainter>
#include <QRegion>
#include <QTextStream>
#include <QTimer>
#include <QWidget>

#include <functional>

// ---------------------------------------------------------------------------
// mask_perf —— 量"圆角遮罩每帧重建"的真实开销。
//
// 浮窗的高度动画（卷起 / 展开，220ms）每一帧都会触发 resizeEvent，
// resizeEvent 里调 updateRoundedMask()：重建整窗 QBitmap + 抗锯齿画圆角
// + setMask()。setMask 在 Windows 上要转成 Win32 区域交给系统裁剪窗口。
// 本文件回答的就是：这一串每帧占多少，值不值得改、怎么改。
//
// 对照组：
//   A 现状      —— 每帧重建整窗位图 + setMask(QBitmap)
//   B 分组缓存  —— 只缓存"上圆角带 / 下圆角带"位图，中间那条是纯矩形，
//                  每帧拼一个 QRegion 再 setMask(QRegion)
//   C 纯 resize —— 不碰 mask，量出 resize 本身的地板价
//
// 另外做**等价性校验**：对一大批 (宽,高) 比较 A 与 B 得到的区域**集合**是否
// 完全相同（用互相减去的空集判定，而不是比矩形分解方式 —— 分解方式不影响
// 渲染，集合必须一模一样）。
// ---------------------------------------------------------------------------

constexpr int kCornerRadius = 8;

QRect rect_of(int w, int h)
{
    return QRect(0, 0, w, h);
}

// 与 ui/floatingboxwidget.cpp::updateRoundedMask 完全同一套画法。
QBitmap buildFullMask(int w, int h)
{
    QBitmap mask(w, h);
    mask.fill(Qt::color0);

    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setBrush(Qt::color1);
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(QRectF(rect_of(w, h)).adjusted(0.5, 0.5, -0.5, -0.5),
                            kCornerRadius, kCornerRadius);
    painter.end();
    return mask;
}

// 组 B 用到的两条"圆角带"。带高取得比半径大一点，把抗锯齿的过渡行也包进去。
constexpr int kBandHeight = kCornerRadius + 2;

QBitmap buildBand(int w, bool top)
{
    QBitmap bmp(w, kBandHeight);
    bmp.fill(Qt::color0);

    QPainter painter(&bmp);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setBrush(Qt::color1);
    painter.setPen(Qt::NoPen);

    // 画的是同一个圆角矩形，只是把画布挪到"只露出上/下那几行"的位置。
    // 上下两带都用整块矩形做参照，保证抗锯齿的半像素相位与整窗位图一致。
    const QRectF full = QRectF(rect_of(w, 2 * kBandHeight)).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.drawRoundedRect(top ? full : full.translated(0, -kBandHeight),
                            kCornerRadius, kCornerRadius);
    painter.end();
    return bmp;
}

QRegion compositeRegion(int w, int h, const QBitmap &topBand, const QBitmap &bottomBand)
{
    if (h < 2 * kBandHeight)
        return QRegion(buildFullMask(w, h));   // 太矮就退回整窗位图

    QRegion region(topBand);
    region += QRegion(QRect(0, kBandHeight, w, h - 2 * kBandHeight));
    QRegion bottom(bottomBand);
    bottom.translate(0, h - kBandHeight);
    region += bottom;
    return region;
}

// 集合相等（忽略矩形分解方式）
bool sameRegionSet(const QRegion &a, const QRegion &b)
{
    return a.subtracted(b).isEmpty() && b.subtracted(a).isEmpty();
}

static QTextStream out(stdout);

void report(const QString &label, qint64 usTotal, int n)
{
    out << QStringLiteral("  %1 %2 us/次\n")
               .arg(label, -30)
               .arg(double(usTotal) / n, 0, 'f', 1);
    out.flush();
}

// 每次尺寸都在变的窗口：A 与 B 的对照都在它的 resizeEvent 里走
class MaskWindow : public QWidget
{
public:
    enum Mode { NoMask, FullBitmap, Bands } mode = NoMask;

    QBitmap topBand;
    QBitmap bottomBand;

    void prepareBands(int w)
    {
        topBand = buildBand(w, true);
        bottomBand = buildBand(w, false);
    }

protected:
    void resizeEvent(QResizeEvent *) override
    {
        if (mode == FullBitmap) {
            setMask(buildFullMask(width(), height()));
        } else if (mode == Bands) {
            setMask(compositeRegion(width(), height(), topBand, bottomBand));
        }
    }
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DeskTidyMaskPerf"));
    QCoreApplication::setApplicationName(QStringLiteral("DeskTidyMaskPerf"));

    out << "=== 圆角遮罩每帧开销 ===\n\n";

    // ---- 等价性校验：A 与 B 必须给出完全相同的区域集合 ----
    out << "[等价性] A(整窗位图) vs B(分组拼区域)\n";
    int mismatches = 0, cases = 0;
    const int widths[] = { 220, 221, 259, 260, 261, 341, 600 };
    const int heights[] = { 30, 31, 32, 33, 120, 121, 300, 301, 480, 900 };
    for (int w : widths) {
        const QBitmap top = buildBand(w, true);
        const QBitmap bot = buildBand(w, false);
        for (int h : heights) {
            ++cases;
            const QRegion a(buildFullMask(w, h));
            const QRegion b = compositeRegion(w, h, top, bot);
            if (!sameRegionSet(a, b)) {
                ++mismatches;
                out << "    [不一致] " << w << "x" << h << "\n";
            }
        }
    }
    out << "    共 " << cases << " 组，不一致 " << mismatches << " 组\n\n";

    // ---- 计时 ----
    MaskWindow winA, winB;
    winA.resize(260, 300);
    winB.resize(260, 300);
    winA.show();
    winB.show();
    for (int i = 0; i < 10; ++i) {
        QEventLoop tick; QTimer::singleShot(20, &tick, &QEventLoop::quit); tick.exec();
    }

    const int w = 260;
    const int baseH = 300;
    const int N = 400;

    winA.prepareBands(w);
    winB.prepareBands(w);

    auto run = [&](const QString &label, MaskWindow::Mode mode, const std::function<void(MaskWindow &, int)> &body) {
        MaskWindow &win = (mode == MaskWindow::Bands) ? winB : winA;
        win.mode = mode;
        for (int i = 0; i < 30; ++i) body(win, i);          // 预热
        qint64 best = -1;
        for (int round = 0; round < 3; ++round) {
            QElapsedTimer t; t.start();
            for (int i = 0; i < N; ++i) body(win, i);
            const qint64 us = t.nsecsElapsed() / 1000;
            if (best < 0 || us < best) best = us;
        }
        report(label, best, N);
    };

    out << "[计时] 每帧尺寸在 300 / 301 之间来回切（模拟高度动画）\n";

    run(QStringLiteral("A 整窗位图 + setMask"), MaskWindow::FullBitmap,
        [&](MaskWindow &win, int i) { win.setMask(buildFullMask(w, baseH + (i % 2))); });

    run(QStringLiteral("B 拼区域 + setMask"), MaskWindow::Bands,
        [&](MaskWindow &win, int i) {
            win.setMask(compositeRegion(w, baseH + (i % 2), win.topBand, win.bottomBand));
        });

    winA.clearMask();   // C 组要的是「没有 mask」的地板价
    run(QStringLiteral("C 只 resize（无 mask）"), MaskWindow::NoMask,
        [&](MaskWindow &win, int i) { win.resize(w, baseH + (i % 2)); });

    run(QStringLiteral("A' resize + 整窗位图"), MaskWindow::FullBitmap,
        [&](MaskWindow &win, int i) { win.resize(w, baseH + (i % 2)); });

    run(QStringLiteral("B' resize + 拼区域"), MaskWindow::Bands,
        [&](MaskWindow &win, int i) { win.resize(w, baseH + (i % 2)); });

    // -----------------------------------------------------------------------
    // 离屏对照：WA_DontShowOnScreen 的窗口没有原生句柄，setMask 不会走到
    // 系统那一侧，量到的就是"我们自己的 CPU 开销"（位图绘制 / 区域构造）。
    // 可见窗口那组数字里混着 Windows 合成器的往返时间，噪声极大，
    // 只有这一组才说明算法本身的代价。
    // -----------------------------------------------------------------------
    out << QStringLiteral("\n[计时] 离屏（无原生窗口，只剩 CPU 开销）\n");

    MaskWindow offA, offB;
    offA.setAttribute(Qt::WA_DontShowOnScreen, true);
    offB.setAttribute(Qt::WA_DontShowOnScreen, true);
    offA.resize(260, 300);
    offB.resize(260, 300);
    offA.show();
    offB.show();
    offA.prepareBands(w);
    offB.prepareBands(w);
    for (int i = 0; i < 20; ++i) { QCoreApplication::processEvents(); }

    auto runOff = [&](const QString &label, MaskWindow::Mode mode, const std::function<void(MaskWindow &, int)> &body) {
        MaskWindow &win = (mode == MaskWindow::Bands) ? offB : offA;
        win.mode = mode;
        for (int i = 0; i < 30; ++i) body(win, i);
        qint64 best = -1;
        for (int round = 0; round < 5; ++round) {
            QElapsedTimer t; t.start();
            for (int i = 0; i < N; ++i) body(win, i);
            const qint64 us = t.nsecsElapsed() / 1000;
            if (best < 0 || us < best) best = us;
        }
        report(label, best, N);
    };

    runOff(QStringLiteral("A' resize + 整窗位图"), MaskWindow::FullBitmap,
           [&](MaskWindow &win, int i) { win.resize(w, baseH + (i % 2)); });

    runOff(QStringLiteral("B' resize + 拼区域"), MaskWindow::Bands,
           [&](MaskWindow &win, int i) { win.resize(w, baseH + (i % 2)); });

    offA.clearMask();
    runOff(QStringLiteral("C' 只 resize（无 mask）"), MaskWindow::NoMask,
           [&](MaskWindow &win, int i) { win.resize(w, baseH + (i % 2)); });

    // 只看"造 mask"本身：完全不动窗口
    {
        qint64 bestA = -1, bestB = -1;
        for (int round = 0; round < 5; ++round) {
            QElapsedTimer t; t.start();
            for (int i = 0; i < N; ++i) { const QBitmap b = buildFullMask(w, baseH + (i % 2)); Q_UNUSED(b) }
            const qint64 us = t.nsecsElapsed() / 1000;
            if (bestA < 0 || us < bestA) bestA = us;

            t.restart();
            for (int i = 0; i < N; ++i) {
                const QRegion r = compositeRegion(w, baseH + (i % 2), offB.topBand, offB.bottomBand);
                Q_UNUSED(r)
            }
            const qint64 us2 = t.nsecsElapsed() / 1000;
            if (bestB < 0 || us2 < bestB) bestB = us2;
        }
        report(QStringLiteral("A 只造整窗位图"), bestA, N);
        report(QStringLiteral("B 只拼区域"), bestB, N);
    }

    // setMask 本身有没有"与区域复杂度无关"的固定开销？拿一个平凡矩形试。
    {
        MaskWindow offC;
        offC.setAttribute(Qt::WA_DontShowOnScreen, true);
        offC.resize(260, 300);
        offC.show();
        for (int i = 0; i < 20; ++i) { QCoreApplication::processEvents(); }

        qint64 best = -1;
        for (int round = 0; round < 5; ++round) {
            QElapsedTimer t; t.start();
            for (int i = 0; i < N; ++i) { offC.setMask(QRect(0, 0, w, baseH + (i % 2))); }
            const qint64 us = t.nsecsElapsed() / 1000;
            if (best < 0 || us < best) best = us;
        }
        report(QStringLiteral("D 只 setMask(平凡矩形)"), best, N);

        best = -1;
        const QRegion fixedRegion = compositeRegion(w, baseH, offB.topBand, offB.bottomBand);
        for (int round = 0; round < 5; ++round) {
            QElapsedTimer t; t.start();
            for (int i = 0; i < N; ++i) { offC.setMask(fixedRegion); }
            const qint64 us = t.nsecsElapsed() / 1000;
            if (best < 0 || us < best) best = us;
        }
        report(QStringLiteral("E 反复设同一个区域"), best, N);
    }
    out << QStringLiteral("\n[规模] A 的区域矩形数 = %1，B 的区域矩形数 = %2，窗口 260x300\n")
               .arg(QRegion(buildFullMask(260, 300)).rectCount())
               .arg(compositeRegion(260, 300, winB.topBand, winB.bottomBand).rectCount());

    winA.hide();
    winB.hide();
    out.flush();
    return mismatches == 0 ? 0 : 1;
}