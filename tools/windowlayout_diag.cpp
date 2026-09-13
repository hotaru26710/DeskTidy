#include <QCoreApplication>
#include <QList>
#include <QRect>
#include <QString>
#include <QTextStream>

#include "windowlayout.h"

// ---------------------------------------------------------------------------
// windowlayout_diag —— 浮窗"互相让位"的几何校验。
//
// 【为什么要有这个探针】
//
// computePushDown 的约束有四条：只推下方、只推最小距离、不跨屏、不推出屏幕。
// 单测已经逐条钉过了，但单测的布局是为了"区分两种算法"精心构造的，
// 而真实使用中最怕的是另一种情况：每条约束单独看都满足，合起来却互相打架 ——
// 比如"只推最小距离"和"连锁下推"叠加之后，把某个窗口推到了屏幕外；
// 或者连锁传导到自己身上，把发起者推走。
//
// 所以这里换一套判据：**不看每个 dy 是多少，只看终局布局是否合法**。
// 输入是一批真实比例的浮窗，逐步展开每一个，每步都检查全局不变量。
//
// 【不变量】
//   1) 任何两个窗口都不再纵向重叠（横向不重叠的不算）；
//   2) 没有任何窗口超出所在屏幕的可用区域（上、下、左、右四边）；
//   3) 没有窗口被向上推（dy 恒为正，或干脆不出现）；
//   4) 发起者自己绝不出现在结果里。
//
// 这四条任何一条被破坏，用户都会看到"浮窗跑到奇怪的地方去了"。
// ---------------------------------------------------------------------------

static int gPass = 0;
static int gFail = 0;

static void check(bool ok, const QString &what, QTextStream &out)
{
    if (ok) { ++gPass; out << "  [PASS] " << what << "\n"; }
    else    { ++gFail; out << "  [FAIL] " << what << "\n"; }
    out.flush();
}

namespace {

const QRect kScreen(0, 0, 1920, 1080);      // 主屏可用区（bottom = 1079）

WindowLayout::Item makeItem(const QString &id, const QRect &r)
{
    WindowLayout::Item it;
    it.id = id;
    it.rect = r;
    it.screenRect = kScreen;
    return it;
}

// 两个矩形是否纵向重叠（横向也必须重叠才算"真的叠在一起"）。
bool overlaps(const QRect &a, const QRect &b)
{
    const bool horizontally = a.left() < b.right() && b.left() < a.right();
    if (!horizontally) return false;
    return a.top() < b.bottom() && b.top() < a.bottom();
}

// 把一批位移应用到窗口上。
QList<WindowLayout::Item> applyShifts(const QList<WindowLayout::Item> &items,
                                      const QList<WindowLayout::Shift> &shifts)
{
    QList<WindowLayout::Item> result = items;
    for (const WindowLayout::Shift &s : shifts) {
        for (WindowLayout::Item &it : result) {
            if (it.id == s.id) {
                it.rect.translate(0, s.dy);
                break;
            }
        }
    }
    return result;
}

// 检查全部四条不变量。returns 失败条数。
int checkInvariants(const QList<WindowLayout::Item> &items,
                    const QString &anchorId,
                    const QList<WindowLayout::Shift> &shifts,
                    const QString &label,
                    QTextStream &out)
{    int bad = 0;

    // 不变量 3：没有向上推、也没有零位移的噪声项。
    for (const WindowLayout::Shift &s : shifts) {
        if (s.dy <= 0) {
            out << "  [FAIL] " << label << "：" << s.id
                << " 的 dy = " << s.dy << "（应当恒为正）\n";
            ++bad;
        }
    }

    // 不变量 4：发起者自己不被推。
    for (const WindowLayout::Shift &s : shifts) {
        if (s.id == anchorId) {
            out << "  [FAIL] " << label << "：发起者自己出现在了位移结果里\n";
            ++bad;
        }
    }

    const QList<WindowLayout::Item> after = applyShifts(items, shifts);

    // 不变量 1：两两不再重叠。
    for (int i = 0; i < after.size(); ++i) {
        for (int j = i + 1; j < after.size(); ++j) {
            if (overlaps(after[i].rect, after[j].rect)) {
                out << "  [FAIL] " << label << "：让位后 " << after[i].id
                    << " 与 " << after[j].id << " 仍然重叠\n";
                ++bad;
            }
        }
    }

    // 不变量 2：都在屏幕内。
    for (const WindowLayout::Item &it : after) {
        const QRect s = it.screenRect;
        if (it.rect.top() < s.top() || it.rect.bottom() > s.bottom()
            || it.rect.left() < s.left() || it.rect.right() > s.right()) {
            out << "  [FAIL] " << label << "：" << it.id
                << " 超出屏幕 " << it.rect.top() << "," << it.rect.bottom() << "\n";
            ++bad;
        }
    }

    return bad;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QTextStream out(stdout);
    out << "=== 浮窗让位几何校验 ===\n\n";
    out << "屏幕可用区：" << kScreen.width() << "x" << kScreen.height() << "\n\n";

    // -----------------------------------------------------------------------
    // 场景 1：三个上下紧挨着的浮窗，展开最上面那个。
    //
    // 这是"连锁下推"最典型的现场，也是真实使用里最容易撞上的布局 ——
    // 用户把浮窗一个个往下排，展开第一个时下面两个都得让。
    // -----------------------------------------------------------------------
    {
        out << "场景 1：竖直三连，展开最上面的\n";

        const QList<WindowLayout::Item> items{
            makeItem(QStringLiteral("甲"), QRect(100, 100, 260, 60)),   // 收起状态
            makeItem(QStringLiteral("乙"), QRect(100, 160, 260, 60)),   // 紧贴其下
            makeItem(QStringLiteral("丙"), QRect(100, 220, 260, 60))};  // 再紧贴

        // 甲展开到 300 高 -> bottom = 399
        const QList<WindowLayout::Shift> shifts =
            WindowLayout::computePushDown(QStringLiteral("甲"),
                                          QRect(100, 100, 260, 300),
                                          kScreen,
                                          items);

        out << "  位移数 = " << shifts.size() << "\n";
        for (const WindowLayout::Shift &s : shifts)
            out << "    " << s.id << " 下移 " << s.dy << "\n";

        check(shifts.size() == 2, QStringLiteral("乙、丙都被推动（连锁传导到底）"), out);

        const int bad = checkInvariants(items, QStringLiteral("甲"), shifts,
                                        QStringLiteral("场景1"), out);
        check(bad == 0, QStringLiteral("场景 1 全局不变量成立"), out);
        out << "\n";
    }

    // -----------------------------------------------------------------------
    // 场景 2：窗口会撞到屏幕下沿。
    //
    // 甲展开后要求乙挪到很下面，但乙已经很靠底了。实现的选择是
    // "宁可少推也不推出去"，这里验证那个选择确实被遵守 —— 以及
    // 此时全局不变量仍然成立（允许残留重叠，但绝不允许出屏）。
    // -----------------------------------------------------------------------
    {
        out << "场景 2：被推者贴近屏幕下沿\n";

        const QList<WindowLayout::Item> items{
            makeItem(QStringLiteral("甲"), QRect(100, 700, 260, 60)),
            makeItem(QStringLiteral("乙"), QRect(100, 1000, 260, 79))}; // bottom = 1078

        const QList<WindowLayout::Shift> shifts =
            WindowLayout::computePushDown(QStringLiteral("甲"),
                                          QRect(100, 700, 260, 320),    // bottom = 1019
                                          kScreen,
                                          items);

        out << "  位移数 = " << shifts.size() << "\n";
        for (const WindowLayout::Shift &s : shifts)
            out << "    " << s.id << " 下移 " << s.dy << "\n";

        // 乙 bottom = 1078，只剩 1px 空间；必须给出一个"不越界"的结果。
        for (const WindowLayout::Shift &s : shifts) {
            if (s.id == QStringLiteral("乙")) {
                check(1078 + s.dy <= kScreen.bottom(),
                      QStringLiteral("贴底的乙没有被推出屏幕"), out);
            }
        }

        // 这里不检查"两两不重叠"：空间不够时残留重叠是明确接受的行为。
        int bad = 0;
        for (const WindowLayout::Shift &s : shifts) {
            if (s.dy <= 0) ++bad;
            if (s.id == QStringLiteral("甲")) ++bad;
        }
        check(bad == 0, QStringLiteral("场景 2 位移方向与发起者都正确"), out);
        out << "\n";
    }

    // -----------------------------------------------------------------------
    // 场景 3：横向并排的浮窗不该互相影响。
    //
    // 防的是"按 y 排序后无脑推"—— 那样会把旁边那一列也推下去，
    // 桌面布局整个乱掉。
    // -----------------------------------------------------------------------
    {
        out << "场景 3：横向并排，纵向坐标相近\n";

        const QList<WindowLayout::Item> items{
            makeItem(QStringLiteral("甲"), QRect(100, 100, 260, 60)),
            makeItem(QStringLiteral("乙"), QRect(400, 120, 260, 60)),   // 右边一列
            makeItem(QStringLiteral("丙"), QRect(100, 160, 260, 60))};  // 甲正下方

        const QList<WindowLayout::Shift> shifts =
            WindowLayout::computePushDown(QStringLiteral("甲"),
                                          QRect(100, 100, 260, 300),
                                          kScreen,
                                          items);

        out << "  位移数 = " << shifts.size() << "\n";
        int dyB = 0;
        for (const WindowLayout::Shift &s : shifts) {
            out << "    " << s.id << " 下移 " << s.dy << "\n";
            if (s.id == QStringLiteral("乙")) dyB = s.dy;
        }

        check(dyB == 0, QStringLiteral("右边的乙完全没被推动"), out);
        check(shifts.size() == 1, QStringLiteral("只有正下方的丙被推动"), out);

        const int bad = checkInvariants(items, QStringLiteral("甲"), shifts,
                                        QStringLiteral("场景3"), out);
        check(bad == 0, QStringLiteral("场景 3 全局不变量成立"), out);
        out << "\n";
    }

    // -----------------------------------------------------------------------
    // 场景 4：完全重合。
    //
    // 异常布局，但必须有个明确行为：整块让开。
    // -----------------------------------------------------------------------
    {
        out << "场景 4：两个浮窗坐标完全重合\n";

        const QList<WindowLayout::Item> items{
            makeItem(QStringLiteral("甲"), QRect(300, 300, 260, 300)),
            makeItem(QStringLiteral("乙"), QRect(300, 300, 260, 300))};

        const QList<WindowLayout::Shift> shifts =
            WindowLayout::computePushDown(QStringLiteral("甲"),
                                          QRect(300, 300, 260, 300),
                                          kScreen,
                                          items);

        out << "  位移数 = " << shifts.size() << "\n";
        for (const WindowLayout::Shift &s : shifts)
            out << "    " << s.id << " 下移 " << s.dy << "\n";

        check(shifts.size() == 1, QStringLiteral("重合的那个被整块推开"), out);

        const int bad = checkInvariants(items, QStringLiteral("甲"), shifts,
                                        QStringLiteral("场景4"), out);
        check(bad == 0, QStringLiteral("场景 4 全局不变量成立"), out);
        out << "\n";
    }

    // -----------------------------------------------------------------------
    // 场景 5：逐级展开一整列，检查反复推让之后不会累积出越界。
    //
    // 这是"多次操作后布局是否还能自洽"的检验 —— 单次计算正确不代表
    // 连续几次之后还正确（例如每步都按当前位置算，可能一路往下漂）。
    // -----------------------------------------------------------------------
    {
        out << "场景 5：一列四窗，从下往上逐个展开\n";

        QList<WindowLayout::Item> items{
            makeItem(QStringLiteral("A"), QRect(500, 100, 260, 60)),
            makeItem(QStringLiteral("B"), QRect(500, 160, 260, 60)),
            makeItem(QStringLiteral("C"), QRect(500, 220, 260, 60)),
            makeItem(QStringLiteral("D"), QRect(500, 280, 260, 60))};

        int totalShifts = 0;
        int bad = 0;

        // 从下往上展开：D 先变高，再 C，再 B，最后 A。
        const QStringList order{QStringLiteral("D"), QStringLiteral("C"),
                                QStringLiteral("B"), QStringLiteral("A")};

        for (const QString &id : order) {
            // 找到它，把它撑到 240 高（模拟展开）
            QRect target;
            for (const WindowLayout::Item &it : items) {
                if (it.id == id) { target = it.rect; break; }
            }
            target.setHeight(240);

            const QList<WindowLayout::Shift> shifts =
                WindowLayout::computePushDown(id, target, kScreen, items);

            totalShifts += shifts.size();

            // ⚠️ 先校验（此时 items 还是"位移前"的），再把位移落下去。
            // 反过来写会让 checkInvariants 内部再应用一次位移 —— 同一批
            // 位移算两遍，窗口被推到屏幕外，报出来的却是"实现越界"。
            // 第一次跑就踩了这个坑，场景 5 假失败。
            bad += checkInvariants(items, id, shifts,
                                   QStringLiteral("展开") + id, out);

            items = applyShifts(items, shifts);

            // 把发起者自己也撑高（调用方会做的事）
            for (WindowLayout::Item &it : items) {
                if (it.id == id) { it.rect.setHeight(240); break; }
            }
        }

        out << "  累计位移数 = " << totalShifts << "\n";
        for (const WindowLayout::Item &it : items)
            out << "    " << it.id << " -> y=" << it.rect.top()
                << " bottom=" << it.rect.bottom() << "\n";

        check(bad == 0, QStringLiteral("连续五次展开后不变量依然成立"), out);

        // 全部展开之后，四个窗口占 4*240 = 960 高，屏幕可用 1080 —— 放得下，
        // 所以应当没有任何重叠。
        bool anyOverlap = false;
        for (int i = 0; i < items.size(); ++i)
            for (int j = i + 1; j < items.size(); ++j)
                if (overlaps(items[i].rect, items[j].rect)) anyOverlap = true;
        check(!anyOverlap, QStringLiteral("空间够时最终布局无任何重叠"), out);
        out << "\n";
    }

    out << "=== 结果：" << gPass << " 通过 / " << gFail << " 失败 ===\n";
    out.flush();

    return gFail == 0 ? 0 : 1;
}
