#include "windowlayout.h"

#include <QHash>

#include <algorithm>

// ---------------------------------------------------------------------------
// WindowLayout 实现。纯计算：不引用任何 QWidget，只处理 QRect。
// ---------------------------------------------------------------------------

namespace WindowLayout {

namespace {

// 两个矩形在**水平方向**上是否有重叠。
//
// 边界相接（a.right() == b.left()）不算重叠：那样两个窗口是"并排贴着的"，
// 上面那个展开不该把旁边那个推走 —— 那会让桌面上的东西莫名其妙地往下掉。
bool overlapsHorizontally(const QRect &a, const QRect &b)
{
    return a.left() < b.right() && b.left() < a.right();
}

// 两个矩形是否属于同一块屏幕。
//
// 判据用"屏幕可用区域的左上角"而不是指针：纯函数拿不到 QScreen，
// 而且同型号屏幕的顺序在不同运行里也可能变，用几何位置更稳定。
bool sameScreen(const QRect &a, const QRect &b)
{
    return a.topLeft() == b.topLeft() && a.size() == b.size();
}

} // namespace

QList<Shift> computePushDown(const QString &anchorId,
                             const QRect &anchorTarget,
                             const QRect &anchorScreen,
                             const QList<Item> &others)
{
    QList<Shift> shifts;

    // 按 y 排序后再处理，并且**顺序应用**：
    // 上面那个先让开，下面那个再基于"上面已经让开后的位置"判断。
    //
    // 若一次性基于原始位置算完，会出现这种情况：
    //   A 在 B 上方 10px，B 在 C 上方 10px，A 展开压住 B 也压住 C。
    //   各自按原始位置算增量 -> B 挪 50，C 也挪 50（因为它也被 A 压住）
    //   -> B 挪完与 C 撞上。顺序应用则不会有这个问题。
    QList<Item> sorted = others;
    std::sort(sorted.begin(), sorted.end(), [](const Item &a, const Item &b) {
        if (a.screenRect.topLeft() != b.screenRect.topLeft()) {
            if (a.screenRect.x() != b.screenRect.x())
                return a.screenRect.x() < b.screenRect.x();
            return a.screenRect.y() < b.screenRect.y();
        }
        if (a.rect.y() != b.rect.y()) return a.rect.y() < b.rect.y();
        if (a.rect.x() != b.rect.x()) return a.rect.x() < b.rect.x();
        return a.id < b.id;
    });

    // 已经"落定"的窗口位置，供后面的项判断时参考。
    // 键是 id，值是应用位移之后的矩形。
    QHash<QString, QRect> settled;

    // 障碍列表在一整轮循环里**只分配一份**：下标 0 恒为 anchorTarget，
    // 之后按"落定顺序"追加被推开的窗口。
    //
    // 原先每个 item 都现场重建一份 { anchorTarget } ∪ settled ——
    // 每个 item 一次 O(n) 分配，整轮就是 O(n²) 的堆压力，而这个函数会在
    // 展开/卷起的动画里被反复调用。
    //
    // 复用是等价的：pushBelow 只取"所有障碍里最深的那条底边"，
    // 结果与遍历顺序无关；而 append 的时机与 settled.insert 严格一致，
    // 所以两者在任何时刻都是同一个集合。
    QList<QRect> blockers;
    blockers.reserve(sorted.size());
    blockers.append(anchorTarget);

    // 把 rect 向下推到"不再与 blockers 中任何一个相交"为止。
    //
    // 这是"连锁下推"的核心：被推者让开之后，可能正好压住排在它下面的
    // 另一个窗口；那个窗口同样得让。反复取"所有冲突里最深的那条底边"
    // 直到无冲突，一次循环就是一级连锁。
    //
    // 返回需要往下挪的距离（>=0）。0 表示本来就互不干涉。
    const auto pushBelow = [](const QRect &rect, const QList<QRect> &blockers) {
        QRect cursor = rect;
        int total = 0;

        // 每轮至少消掉一个冲突，且 cursor 单调下移，所以循环必然终止。
        // 上限只是防御性的：正常布局里连锁深度远小于 blockers 数量。
        for (int guard = 0; guard <= blockers.size(); ++guard) {
            int deepest = cursor.top();     // 需要落到的最终顶边
            bool hit = false;

            for (const QRect &b : blockers) {
                if (!overlapsHorizontally(cursor, b)) continue;
                if (cursor.top() >= b.bottom()) continue;   // 已经在它下面

                // 只有"彼此纵向确实交叠"才算冲突：cursor 的顶边在 b 底边之上，
                // 且 cursor 的底边在 b 顶边之下。注意"完全重合"满足这一条
                // （cursor == b），所以重合会被当作冲突处理 —— 这正是期望行为。
                if (cursor.bottom() <= b.top()) continue;

                deepest = qMax(deepest, b.bottom() + 1);
                hit = true;
            }

            if (!hit) break;

            const int dy = deepest - cursor.top();
            if (dy <= 0) break;

            cursor.moveTop(deepest);
            total += dy;
        }

        return total;
    };

    for (const Item &item : sorted) {
        if (item.id == anchorId) {
            continue;   // 不推自己
        }

        // 只处理与发起者**同一块屏幕**的窗口。
        //
        // ⚠️ 比的是两边的**屏幕矩形**，不是窗口矩形 ——
        // 拿 anchorTarget（窗口矩形）去跟 item.screenRect（屏幕矩形）比
        // 是永远不相等的，那样写会让所有窗口都被判成异屏而跳过，
        // 表现是这个函数"什么都不做"，且不报错。这一点踩过。
        if (!sameScreen(item.screenRect, anchorScreen)) {
            continue;
        }

        // 取"当前位置"，但要考虑前面几项已经让开的位移 —— 这就是顺序应用。
        QRect current = settled.contains(item.id) ? settled.value(item.id) : item.rect;

        // ---- 它需不需要动？两种来源，缺一不可 ----
        //
        // 来源一：被 anchor **直接**挡住。
        // 来源二：**间接**被挡住 —— 本来没被 anchor 压到，但前面某一项让开
        //         之后正好压住了它（上下紧挨着的两个浮窗就是这种）。
        //
        // ⚠️ 一开始只在"直接挡住"时才往下算，于是 C-06 那种布局里
        // 下方窗口永远不动：它的顶边正好等于 anchor 底边，判据认为它
        // "没被挡住"直接 continue 掉了，连锁根本无从发生。
        // 所以下面这些 continue 只用来排除**根本不可能参与**的窗口，
        // 不做"是否被挡"的最终裁决 —— 裁决交给 pushBelow。
        //
        // ⚠️ 另外，这里的判据曾经写反过，导致整个函数恒返回空、功能完全没生效：
        //     if (current.top() < anchorTarget.bottom()) continue;   // 要求"不重叠"
        //     int dy = anchorTarget.bottom() - current.top();        // 于是 dy <= 0
        //     if (dy <= 0) continue;                                 // 必然丢掉
        // 两个条件互为反面，构成死结 —— 能通过前者的输入一定过不了后者。
        if (current.top() < anchorTarget.top()) {
            continue;   // 在上方或与之平齐 —— 展开是向下长的，不推上面
        }
        if (!overlapsHorizontally(current, anchorTarget)) {
            continue;   // 不在同一条竖直通道上，压不到它
        }

        // ---- 连锁下推 ----
        //
        // 把 anchor 和已落定者**放在同一个障碍列表里**，从原始位置一次算到
        // 最终落点。落点若就是原地（dy == 0），说明它既没被 anchor 挡住、
        // 也没被谁挤到 —— 直接跳过，不产生噪声位移。
        //
        // ⚠️ 这里踩过一次"重复计算"：
        // 一开始写成 pushBelow(current.translated(0, dyToAnchor), blockers)，
        // 也就是"先搬到 anchor 之下，再从那个位置往下推"。结果位移被算了两遍 ——
        // 搬过去的位置本身已经压住了已落定者，pushBelow 又把这段重叠加了
        // 一遍，落点比应有的位置多出整整一个窗口高度。
        //
        // ⚠️ 还踩过一次"重复计入 anchor"：anchorTarget 本身已经在
        // overlapsHorizontally 那一关用过了，但若把它排除在 blockers 之外，
        // 被推者就只会被已落定者推、不会被 anchor 推，连锁的起点就没了。
        int dy = pushBelow(current, blockers);

        // ⚠️ 边界钳制：不推出屏幕下沿。
        //
        // 推到屏幕外等于把窗口弄丢 —— 用户看不到、也拖不回来。
        // 宁可少推（结果是有点重叠），也不能推出去。
        // 这里是**全局上限**：连锁下推算出来的最终落点如果越过屏幕下沿，
        // 就一律压回"底边正好贴住屏幕下沿"。
        // 用 availableGeometry 而不是 geometry：要避开任务栏。
        const int maxBottom = item.screenRect.bottom();
        if (current.bottom() + dy > maxBottom) {
            dy = maxBottom - current.bottom();
        }

        if (dy <= 0) {
            continue;   // 不需要推，或已经贴着屏幕底边推不动了
        }

        Shift shift;
        shift.id = item.id;
        shift.dy = dy;
        shifts.append(shift);

        const QRect settledRect = current.translated(0, dy);
        settled.insert(item.id, settledRect);
        blockers.append(settledRect);
    }

    return shifts;
}

QList<Shift> computePushDownForResize(const QString &anchorId,
                                      const QRect &anchorCurrent,
                                      const QRect &anchorTarget,
                                      const QRect &anchorScreen,
                                      const QList<Item> &others)
{
    // 变矮（卷起）时不推任何人。
    //
    // 卷起只是把自己收起来，腾出来的空间不需要谁去填 —— 让下面的窗口
    // 自己弹回原位是另一件事（而且很容易做成"抖一下"）。
    // 这里只处理"长高"这一种方向。
    if (anchorTarget.height() <= anchorCurrent.height()) {
        return QList<Shift>();
    }

    // 长高时，把"当前挡住的部分"也算进去：
    // 用当前矩形与目标矩形的并集作为实际占位 —— 因为动画期间旧位置也还占着。
    QRect occupied = anchorTarget;
    occupied.setTop(qMin(anchorCurrent.top(), anchorTarget.top()));

    return computePushDown(anchorId, occupied, anchorScreen, others);
}

} // namespace WindowLayout
