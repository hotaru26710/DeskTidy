#ifndef FLOATINGHOVEROVERLAY_H
#define FLOATINGHOVEROVERLAY_H

// ---------------------------------------------------------------------------
// ui/floatinghoveroverlay.h —— 浮窗内部的悬停「光影描边」触感层。
//
// 【它是什么】
// 一层**不参与布局、不接收鼠标、不抢焦点**的透明子控件，铺满浮窗客户区，
// 只在鼠标悬停时沿窗口边缘画一圈蓝色外描边 + 内层白色高光。
//
// 【为什么这么做】
// 触感必须"鼠标一进去就有反馈"，但浮窗本身有一堆会改变窗口形态的动画
// （卷起/展开、互相让位、淡入淡出）。若把光晕做在窗口自己身上（改样式表、
// 改窗口透明度、动态重建遮罩），就会和这些机制抢同一批属性，最直接的后果
// 是浮窗推动逻辑被干扰、出现重叠。
//
// 因此触感严格约束在**一层覆盖控件内部**：
//   * 不动窗口尺寸 / 位置 / 透明度；
//   * 不动窗口遮罩（setMask 只在必要时刻由浮窗自己调用）；
//   * 只有一个 QVariantAnimation 驱动进度，动画跑完即停，无常驻计时器；
//   * 不启用顶层 Qt::WA_TranslucentBackground。
//
// 【与全局开关的关系】
// 「启用界面动画」是最高开关：关掉时触感立即出现 / 立即消失，不播放过渡。
// 「鼠标悬停自动展开」只控制展开行为，与本层无耦合 —— 即使关掉自动展开，
// 光影触感依然独立生效。
// ---------------------------------------------------------------------------

#include <QWidget>

#include "coretypes.h"

class QVariantAnimation;

class FloatingHoverOverlay : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(qreal hoverProgress READ hoverProgress WRITE setHoverProgress)

public:
    explicit FloatingHoverOverlay(QWidget *parent = nullptr);

    void setHoverEffect(BoxAppearance::HoverEffect effect);
    void setStrength(BoxAppearance::FeedbackStrength strength);
    void setAnimationSpeed(BoxAppearance::AnimationSpeed speed);
    void setAnimationsEnabled(bool on);
    void setCornerRadius(int radius);

    // 鼠标进入 / 离开时调用。true = 光晕亮起，false = 平滑消退。
    void setActive(bool active);

    qreal hoverProgress() const { return m_progress; }
    bool  isActive() const { return m_active; }

    // Q_PROPERTY 的写入端；公开是为了动画能通过属性系统驱动它。
    void setHoverProgress(qreal progress);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    // 当前速度档下的触感时长（毫秒）。
    int durationMs() const;

    // 按当前 m_active 推导目标进度并启动动画（或立即跳转）。
    void applyTargetProgress();

    BoxAppearance::HoverEffect       m_effect = BoxAppearance::HoverEffect::Glow;
    BoxAppearance::FeedbackStrength  m_strength = BoxAppearance::FeedbackStrength::Standard;
    BoxAppearance::AnimationSpeed    m_speed = BoxAppearance::AnimationSpeed::Standard;
    bool  m_animationsOn = true;
    int   m_cornerRadius = 8;

    bool  m_active = false;
    qreal m_progress = 0.0;

    // 只建一次、反复复用。动画对象是热路径上的东西，反复 new/delete 是白费。
    QVariantAnimation *m_anim = nullptr;
};

#endif // FLOATINGHOVEROVERLAY_H
