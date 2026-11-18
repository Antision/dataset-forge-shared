#include "litewidget2geometry.h"

#include <QtGlobal>
#include <QtMath>

namespace litewidget2 {
namespace {

constexpr qreal kPi = 3.14159265358979323846;

/// dy/dx of the cos^2 transition at t (0 = body top, 1 = strip top).
qreal slopeTangent(qreal t, qreal height, qreal width)
{
    // y = h * cos^2(pi t / 2)  =>  dy/dt = -h * pi/2 * sin(pi t)
    return -height * kPi / (2.0 * width) * std::sin(kPi * t);
}

/// One Hermite segment as a cubic: control points a third of the way along the
/// end tangents. `dx` may be negative when the path runs right to left.
void appendSegment(QPainterPath& path, const QPointF& a, qreal slopeA,
                   const QPointF& b, qreal slopeB)
{
    const qreal dx = b.x() - a.x();
    path.cubicTo(QPointF(a.x() + dx / 3.0, a.y() + slopeA * dx / 3.0),
                 QPointF(b.x() - dx / 3.0, b.y() - slopeB * dx / 3.0),
                 b);
}

/// Appends the sampled cos^2 curve between its two ends.
void appendSlope(QPainterPath& path, const QVector<QPointF>& points,
                 qreal slopeWidthPx, bool forward)
{
    const int count = points.size();
    if (count < 2)
        return;
    const qreal height = qreal(kStripHeight);
    const qreal width = qMax(1.0, slopeWidthPx);
    auto tangentAt = [&](int i) {
        return slopeTangent(qreal(i) / qreal(count - 1), height, width);
    };
    if (forward) {
        for (int i = 0; i + 1 < count; ++i)
            appendSegment(path, points[i], tangentAt(i), points[i + 1], tangentAt(i + 1));
    } else {
        for (int i = count - 1; i > 0; --i)
            appendSegment(path, points[i], tangentAt(i), points[i - 1], tangentAt(i - 1));
    }
}

} // namespace

int flatWidth()
{
    return kHandleWidth + kCloseWidth;
}

int tabWidth(int windowWidth)
{
    const int desired = qRound(windowWidth * kTabWidthRatio);
    const int minimum = flatWidth() + kSlopeWidth;
    return qBound(minimum, desired, qMax(minimum, windowWidth));
}

int slopeWidth(int windowWidth)
{
    Q_UNUSED(windowWidth);
    return kSlopeWidth;
}

QRect stripRect(const QRect& window, bool maximized)
{
    if (maximized)
        return QRect(window.left(), window.top(), window.width(), kStripHeight);
    const int flat = tabWidth(window.width()) - kSlopeWidth;
    return QRect(window.right() - flat + 1, window.top(), flat, kStripHeight);
}

QRect closeRect(const QRect& window, bool maximized)
{
    // 普通状态下一直画到内框底边(条高 + kInnerStroke):按钮的悬停/按下底色
    // 正好填满凸起条的可见高度,下面不会剩一圈"空着"的条。最大化时标题栏
    // 和 body 之间没有内框,按钮就是条高。
    const int height = kStripHeight + (maximized ? 0 : kInnerStroke);
    return QRect(window.right() - kCloseWidth + 1, window.top(), kCloseWidth, height);
}

QRect toggleRect(const QRect& window, bool maximized)
{
    Q_UNUSED(maximized);
    const QRect close = closeRect(window);
    return QRect(close.left() - kCloseWidth, window.top(), kCloseWidth, kStripHeight);
}

QRect minimizeRect(const QRect& window, bool maximized)
{
    Q_UNUSED(maximized);
    const QRect toggle = toggleRect(window);
    return QRect(toggle.left() - kCloseWidth, window.top(), kCloseWidth, kStripHeight);
}

QRect handleRect(const QRect& window, bool maximized)
{
    if (maximized) {
        const int right = minimizeRect(window).left();
        return QRect(window.left(), window.top(), qMax(0, right - window.left()), kStripHeight);
    }
    const int left = window.right() - tabWidth(window.width()) + 1;
    const int right = closeRect(window).left();
    return QRect(left, window.top(), qMax(0, right - left), kStripHeight);
}

QRect bodyRect(const QRect& window)
{
    return QRect(window.left(), window.top() + kStripHeight, window.width(),
                 window.height() - kStripHeight);
}

QVector<QPointF> slopePoints(const QRectF& window, int samples, qreal inset)
{
    if (samples < 24)
        samples = 24;
    const int windowWidth = qRound(window.width());
    const qreal x0 = window.right() - tabWidth(windowWidth);
    const qreal width = qMax(1, slopeWidth(windowWidth));
    const qreal height = qreal(kStripHeight);
    QVector<QPointF> points;
    points.reserve(samples);
    for (int i = 0; i < samples; ++i) {
        const qreal t = qreal(i) / qreal(samples - 1);
        const qreal cosine = std::cos(kPi * t / 2.0);
        QPointF p(x0 + t * width, window.top() + height * cosine * cosine);
        if (inset > 0.0) {
            // 切向量 (1, dy/dx);朝形状内部(下侧)的法线是 (-dy/dx, 1) 归一化。
            // 两端 dy/dx = 0,法线正好竖直,和相邻的水平直边严丝合缝。
            const qreal sine = std::sin(kPi * t);
            const qreal slope = -height * kPi / (2.0 * width) * sine;
            const qreal length = std::sqrt(1.0 + slope * slope);
            const qreal gain = 1.0 + (kSlopeInsetGain - 1.0) * sine * sine;
            p += QPointF(-slope / length, 1.0 / length) * (inset * gain);
        }
        points.append(p);
    }
    return points;
}

QPainterPath outlinePath(const QRectF& r, bool maximized, qreal inset)
{
    const qreal left = r.left() + inset;
    const qreal top = r.top() + inset;
    const qreal right = r.right() - inset;
    const qreal bottom = r.bottom() - inset;
    const qreal body = qMax(0.0, qreal(kBodyRadius) - inset);
    const qreal tab = qMax(0.0, qreal(kTabRadius) - inset);
    const qreal strip = qreal(kStripHeight);

    QPainterPath path;
    if (maximized) {
        path.addRoundedRect(QRectF(left, top, right - left, bottom - top), body, body);
        return path;
    }

    // 左边(左下圆角上方)为起点,顺时针走一圈。
    path.moveTo(left, bottom - body);
    if (body >= 0.5)
        path.arcTo(QRectF(left, bottom - 2 * body, 2 * body, 2 * body), 180, 90);
    path.lineTo(right - body, bottom);
    if (body >= 0.5)
        path.arcTo(QRectF(right - 2 * body, bottom - 2 * body, 2 * body, 2 * body), 270, 90);
    // 右边直上,穿过 body 顶边进入 strip。
    path.lineTo(right, top + tab);
    if (tab >= 0.5)
        path.arcTo(QRectF(right - 2 * tab, top, 2 * tab, 2 * tab), 0, 90);
    // strip 顶边向左,然后 cos^2 过渡落到 body 顶边。
    path.lineTo(right - flatWidth(), top);
    appendSlope(path, slopePoints(r, 32, inset), slopeWidth(qRound(r.width())), false);
    // body 顶边向左,左上圆角,回到起点。
    path.lineTo(left + body, top + strip);
    if (body >= 0.5)
        path.arcTo(QRectF(left, top + strip, 2 * body, 2 * body), 90, 90);
    path.lineTo(left, bottom - body);
    path.closeSubpath();
    return path;
}

QPainterPath bodyPath(const QRectF& window, int inset, bool insetTop)
{
    const qreal left = window.left() + inset;
    const qreal right = window.right() - inset;
    const qreal top = window.top() + kStripHeight + (insetTop ? inset : 0);
    const qreal bottom = window.bottom() - inset;
    const qreal radius = qMax(0.0, qreal(kBodyRadius - inset));

    QPainterPath path;
    path.moveTo(left, top);
    path.lineTo(right, top);
    if (radius < 0.5) {
        path.lineTo(right, bottom);
        path.lineTo(left, bottom);
    } else {
        path.lineTo(right, bottom - radius);
        path.arcTo(QRectF(right - 2 * radius, bottom - 2 * radius, 2 * radius, 2 * radius),
                   0, -90);
        path.lineTo(left + radius, bottom);
        path.arcTo(QRectF(left, bottom - 2 * radius, 2 * radius, 2 * radius), 270, -90);
        path.lineTo(left, top);
    }
    path.closeSubpath();
    return path;
}

QPainterPath contentPath(const QRectF& r, qreal radius)
{
    const qreal left = r.left();
    const qreal top = r.top();
    const qreal right = r.right();
    const qreal bottom = r.bottom();
    const qreal rad = qMax(0.0, radius);

    QPainterPath path;
    path.moveTo(left + rad, top);
    path.lineTo(right, top);
    path.lineTo(right, bottom - rad);
    if (rad >= 0.5)
        path.arcTo(QRectF(right - 2 * rad, bottom - 2 * rad, 2 * rad, 2 * rad), 0, -90);
    else
        path.lineTo(right, bottom);
    path.lineTo(left + rad, bottom);
    if (rad >= 0.5)
        path.arcTo(QRectF(left, bottom - 2 * rad, 2 * rad, 2 * rad), 270, -90);
    else
        path.lineTo(left, bottom);
    path.lineTo(left, top + rad);
    if (rad >= 0.5)
        path.arcTo(QRectF(left, top, 2 * rad, 2 * rad), 180, -90);
    else
        path.lineTo(left, top);
    path.closeSubpath();
    return path;
}

} // namespace litewidget2
