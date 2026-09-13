#ifndef WINDOWLAYOUT_H
#define WINDOWLAYOUT_H

#include <QList>
#include <QRect>
#include <QString>

// ---------------------------------------------------------------------------
// WindowLayout —— 浮窗之间"互相让位"的纯几何计算。
//
// 这里**不碰任何窗口**：输入是一组矩形，输出是"某几个矩形该移到哪"。
// 真正去 move() 窗口是调用方（FloatingBoxWidget / FloatingBoxManager）的事。
//
// 为什么硬要把这件事抽成纯函数：
//   1) 它是本工具箱里最容易出隐蔽错误的计算 —— 要考虑屏幕分组、边界钳制、
//      "只推被挡住的"、"只推最小距离"三条约束，任何一条写错都表现为
//      "浮窗莫名其妙跑到别处去了"，很难对着界面调试；
//   2) 抽出来之后可以用单元测试覆盖各种布局（上下相邻、左右并排、
//      跨屏、贴屏幕底边），这些场景手工摆很麻烦；
//   3) 项目已有的 AppService::summarize 就是这个路数，保持一致。
//
// 关键约定：
//   * **只推下方**：只处理 y 更大（在下面）且横向有重叠的窗口；
//   * **只推最小距离**：恰好让被挡的那个露出多少就推多少，不做"等距排列"；
//   * **严格不重叠**：被推者的顶边落在障碍物底边之**下 1 像素**
//     （QRect::bottom() 是含尾的，所以判据是 b.bottom() + 1）；
//   * **连锁下推**：被推者让开后若压住了排在它下面的另一个窗口，
//     那个窗口同样继续让 —— 一级一级往下传导，直到没人再被压住；
//   * **不跨屏比较**：不同屏幕的坐标各自独立，绝不用另一块屏的 y 去算；
//   * **不推出屏幕**：推到屏幕外等于把窗口弄丢，宁可少推。
// ---------------------------------------------------------------------------

namespace WindowLayout {

// 一个待参与计算的窗口。
struct Item
{
    QString id;         // 调用方用来对应回真实窗口（通常传盒名）
    QRect   rect;       // **屏幕坐标**下的窗口矩形（用 frameGeometry）
    QRect   screenRect; // 该窗口所在屏幕的**可用区域**（要避开任务栏）
};

// 一次位移：把 id 对应的窗口纵向移动 dy 像素（正数向下）。
struct Shift
{
    QString id;
    int     dy = 0;
};

// 场景：anchor 要占据 anchorTarget 这块矩形（通常是它"展开后"的尺寸），
// 请算出其余窗口里哪些会被挡住、各自需要向下挪多少。
//
// anchorId：发起者自己的 id，它**不参与**位移计算（我们不推自己）。
// anchorTarget：发起者展开后的目标矩形。
// anchorScreen：发起者所在屏幕的**可用区域**（避开任务栏）。
//               ⚠️ 这个参数不是可选的细节：判断"另一个窗口是不是在同一块屏上"
//               必须拿两边的**屏幕矩形**比，而不能拿窗口矩形比 —— 窗口矩形
//               与屏幕矩形永远不可能相等，那样写会让所有窗口都被判成异屏而跳过，
//               表现是"这个函数什么都不做"，而且不报错。
// others：其余所有窗口（可以跨屏，函数内部会按屏幕分组）。
//
// 返回：需要位移的项。**原地不动的窗口不会出现在结果里**（dy 恒为正）。
//
// 关于"顺序应用"：结果已经保证了内部自洽（被推者之间也不重叠），
// 调用方直接按 id 把每个窗口移动对应的 dy 即可，不需要再自己排序 ——
// 排序与传导在函数内部就做完了。
QList<Shift> computePushDown(const QString &anchorId,
                             const QRect &anchorTarget,
                             const QRect &anchorScreen,
                             const QList<Item> &others);

// 上面那个函数的"展开/收起版"：只在 anchorTarget 比 anchorCurrent 更高时才算。
//
// 之所以单独提供：卷起时（变得更矮）不该推任何人，而调用方很容易忘了判断。
// 与其让每处调用都写一遍 if，不如在入口处一次性收口。
QList<Shift> computePushDownForResize(const QString &anchorId,
                                      const QRect &anchorCurrent,
                                      const QRect &anchorTarget,
                                      const QRect &anchorScreen,
                                      const QList<Item> &others);

} // namespace WindowLayout

#endif // WINDOWLAYOUT_H
