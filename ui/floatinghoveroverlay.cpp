#include "floatinghoveroverlay.h"

#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QRectF>
#include <QVariantAnimation>

namespace {

// 三档反馈强度的固定参数。整套效果只用固定的主题蓝 + 白高光两种颜色，
// 不引入用户可配的颜色 —— 颜色一多，跨主题 / 跨 DPI 的观感就不可控了。
struct FeedbackStyle
{
    int   outerAlpha;       // 外描边（主题蓝）最大 alpha
    int   innerAlpha;       // 内层高光（白）最大 alpha
    qreal lineWidth;        // 描边线宽（像素）
};

FeedbackStyle styleFor(BoxAppearance::FeedbackStrength strength)
{
    switch (strength) {
    case BoxAppearance::FeedbackStrength::Subtle:
        return FeedbackStyle{ 90, 50, 1.0 };
    case BoxAppearance::FeedbackStrength::Strong:
        return FeedbackStyle{ 210, 130, 2.0 };
    case BoxAppearance::FeedbackStrength::Standard:
        break;
    }
    return FeedbackStyle{ 150, 90, 1.5 };
}

// 主题蓝。与界面主色保持一致，避免触感像是外挂上去的。
const QColor kGlowColor(0x1A, 0x73, 0xE8);

} // namespace

FloatingHoverOverlay::FloatingHoverOverlay(QWidget *parent)
    : QWidget(parent)
{
    // 纯展示层：绝不接收鼠标 / 焦点，否则会把列表的悬停、点击、拖拽全吃掉。
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(false);

    m_anim = new QVariantAnimation(this);
    m_anim->setStartValue(0.0);
    m_anim->setEndValue(1.0);
    connect(m_anim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) { setHoverProgress(value.toReal()); });
    // 动画结束后不再做任何事：QVariantAnimation 自身回到 Stopped，
    // 没有常驻计时器、没有周期性重绘，静止时 CPU 占用为零。
}

void FloatingHoverOverlay::setHoverEffect(BoxAppearance::HoverEffect effect)
{
    if (m_effect == effect)
        return;

    m_effect = effect;

    // 关掉触感时立刻收起光晕，别留一圈残影在那儿。
    if (m_effect == BoxAppearance::HoverEffect::Off) {
        if (m_anim)
            m_anim->stop();
        setHoverProgress(0.0);
        return;
    }

    applyTargetProgress();
}

void FloatingHoverOverlay::setStrength(BoxAppearance::FeedbackStrength strength)
{
    if (m_strength == strength)
        return;

    m_strength = strength;
    if (m_progress > 0.0)
        update();
}

void FloatingHoverOverlay::setAnimationSpeed(BoxAppearance::AnimationSpeed speed)
{
    if (m_speed == speed)
        return;

    m_speed = speed;
    // 速度只影响后续动画的时长；正在跑的那一段不强行改时长，
    // 否则鼠标快速划过时进度会跳变。
}

void FloatingHoverOverlay::setAnimationsEnabled(bool on)
{
    if (m_animationsOn == on)
        return;

    m_animationsOn = on;
    applyTargetProgress();
}

void FloatingHoverOverlay::setCornerRadius(int radius)
{
    if (m_cornerRadius == radius)
        return;

    m_cornerRadius = radius;
    if (m_progress > 0.0)
        update();
}

void FloatingHoverOverlay::setActive(bool active)
{
    if (m_active == active)
        return;

    m_active = active;
    applyTargetProgress();
}

void FloatingHoverOverlay::setHoverProgress(qreal progress)
{
    progress = qBound(0.0, progress, 1.0);
    if (qFuzzyCompare(m_progress + 1.0, progress + 1.0))
        return;

    m_progress = progress;
    update();
}

int FloatingHoverOverlay::durationMs() const
{
    switch (m_speed) {
    case BoxAppearance::AnimationSpeed::Relaxed:
        return 160;
    case BoxAppearance::AnimationSpeed::Fast:
        return 80;
    case BoxAppearance::AnimationSpeed::Standard:
        break;
    }
    return 120;
}

void FloatingHoverOverlay::applyTargetProgress()
{
    // 触感关闭时永远不亮。
    if (m_effect == BoxAppearance::HoverEffect::Off) {
        if (m_anim)
            m_anim->stop();
        setHoverProgress(0.0);
        return;
    }

    const qreal target = m_active ? 1.0 : 0.0;

    // 全局动画被关掉时"立即出现 / 立即消失"，不播放过渡。
    if (!m_animationsOn || !m_anim) {
        if (m_anim)
            m_anim->stop();
        setHoverProgress(target);
        return;
    }

    // 已经在目标上就不必再跑一遍动画。
    if (qFuzzyCompare(m_progress + 1.0, target + 1.0)) {
        m_anim->stop();
        return;
    }

    m_anim->stop();
    m_anim->setStartValue(m_progress);
    m_anim->setEndValue(target);
    m_anim->setDuration(durationMs());
    // 亮起用缓出（先快后慢，立刻有"被碰到"的反馈），
    // 消退用缓入（慢慢淡出，不显得突兀）。
    m_anim->setEasingCurve(target > m_progress ? QEasingCurve::OutCubic
                                               : QEasingCurve::InCubic);
    m_anim->start();
}

void FloatingHoverOverlay::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    if (m_effect == BoxAppearance::HoverEffect::Off || m_progress <= 0.0)
        return;

    const FeedbackStyle style = styleFor(m_strength);
    const int outerAlpha = int(style.outerAlpha * m_progress + 0.5);
    const int innerAlpha = int(style.innerAlpha * m_progress + 0.5);
    if (outerAlpha <= 0 && innerAlpha <= 0)
        return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);

    const qreal radius = qMax<qreal>(0.0, qreal(m_cornerRadius));
    const qreal outerWidth = style.lineWidth;

    if (outerAlpha > 0) {
        QPen pen(kGlowColor);
        pen.setColor(QColor(kGlowColor.red(), kGlowColor.green(), kGlowColor.blue(),
                            outerAlpha));
        pen.setWidthF(outerWidth);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);

        const QRectF outer = QRectF(rect()).adjusted(outerWidth / 2.0, outerWidth / 2.0,
                                                     -outerWidth / 2.0, -outerWidth / 2.0);
        painter.drawRoundedRect(outer, radius, radius);
    }

    if (innerAlpha > 0) {
        // 内层高光比外描边更细，并再向内收一点，形成"边缘被光描了一圈"的层次。
        const qreal innerWidth = qMax<qreal>(1.0, outerWidth * 0.75);
        const qreal inset = outerWidth + innerWidth / 2.0;

        QPen pen(QColor(255, 255, 255, innerAlpha));
        pen.setWidthF(innerWidth);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);

        const QRectF inner = QRectF(rect()).adjusted(inset, inset, -inset, -inset);
        if (inner.isValid())
            painter.drawRoundedRect(inner, qMax<qreal>(0.0, radius - outerWidth),
                                    qMax<qreal>(0.0, radius - outerWidth));
    }
}
