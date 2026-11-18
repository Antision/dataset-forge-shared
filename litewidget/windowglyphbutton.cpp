#include "windowglyphbutton.h"

#include <QEnterEvent>
#include <QPainter>
#include <QPainterPath>

namespace {
const QColor kNormal(0xc9, 0xc9, 0xcf);
const QColor kHover(0xff, 0xff, 0xff);
const QColor kPressed(0x00, 0xd9, 0x6b);
const QColor kCloseHover(200, 40, 40, 90);   // rgba(200,40,40,0.35)
const QColor kClosePressed(190, 32, 32, 215); // 深一档的红:按下时整块变红
const QColor kClosePressedGlyph(255, 225, 225);
constexpr qreal kCornerRadius = 2.0;
const QColor kNeutralHover(255, 255, 255, 22);
constexpr qreal kGlyphSize = 10.0;
// × 比别的图形显眼:同样的外框看着更大,单独缩一档。
constexpr qreal kCloseGlyphSize = 8.0;
constexpr qreal kLineWidth = 1.5;
} // namespace

WindowGlyphButton::WindowGlyphButton(Glyph glyph, QWidget* parent)
    : QPushButton(parent)
    , m_glyph(glyph)
{
    setCursor(Qt::ArrowCursor);
    setFlat(true);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover, true);
    setStyleSheet(QStringLiteral("QPushButton{border:none;background:transparent;}"));
}

WindowGlyphButton::WindowGlyphButton(QWidget* parent)
    : WindowGlyphButton(Glyph::Close, parent)
{
}

void WindowGlyphButton::setGlyph(Glyph glyph)
{
    if (m_glyph == glyph)
        return;
    m_glyph = glyph;
    update();
}

QSize WindowGlyphButton::sizeHint() const
{
    return QSize(30, 24);
}

QSize WindowGlyphButton::minimumSizeHint() const
{
    return QSize(30, 24);
}

void WindowGlyphButton::enterEvent(QEnterEvent* event)
{
    QPushButton::enterEvent(event);
    update();
}

void WindowGlyphButton::leaveEvent(QEvent* event)
{
    QPushButton::leaveEvent(event);
    update();
}

void WindowGlyphButton::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const bool hovered = underMouse();
    const bool pressed = isDown();
    const bool close = (m_glyph == Glyph::Close);

    if (hovered || pressed) {
        QColor background;
        if (close)
            background = pressed ? kClosePressed : kCloseHover;
        else
            background = pressed ? QColor(255, 255, 255, 46) : kNeutralHover;

        // 贴着条边铺满,不留内边距 —— 留白会让按钮下面空出一块。右上角跟着
        // 凸起条的圆角收一下,免得把窗口轮廓的圆角糊成直角。
        const QRectF r(rect());
        QPainterPath backgroundPath;
        backgroundPath.moveTo(r.left(), r.top());
        if (close) {
            backgroundPath.lineTo(r.right() - kCornerRadius, r.top());
            backgroundPath.quadTo(QPointF(r.right(), r.top()),
                                  QPointF(r.right(), r.top() + kCornerRadius));
        } else {
            backgroundPath.lineTo(r.right(), r.top());
        }
        backgroundPath.lineTo(r.right(), r.bottom());
        backgroundPath.lineTo(r.left(), r.bottom());
        backgroundPath.closeSubpath();
        painter.fillPath(backgroundPath, background);
    }

    // 10x10 的图形居中;线条 + 圆头,和位图时代的观感一致但不发虚。
    const qreal glyphSize = close ? kCloseGlyphSize : kGlyphSize;
    const QRectF box(QPointF((width() - glyphSize) / 2.0, (height() - glyphSize) / 2.0),
                     QSizeF(glyphSize, glyphSize));
    const QPointF center = box.center();

    QPainterPath path;
    switch (m_glyph) {
    case Glyph::Close:
        path.moveTo(box.topLeft());
        path.lineTo(box.bottomRight());
        path.moveTo(box.bottomLeft());
        path.lineTo(box.topRight());
        break;
    case Glyph::Maximize:
        path.addRect(box.adjusted(0.75, 0.75, -0.75, -0.75));
        break;
    case Glyph::Restore: {
        // 前面一个方框 + 右上角露出的"后窗"两条边。
        path.addRect(QRectF(box.left(), box.top() + 3.0, 7.0, 7.0).adjusted(0.75, 0.75, -0.75, -0.75));
        path.moveTo(box.left() + 3.0, box.top() + 3.0);
        path.lineTo(box.left() + 3.0, box.top() + 0.75);
        path.lineTo(box.right() - 0.75, box.top() + 0.75);
        path.lineTo(box.right() - 0.75, box.top() + 7.0);
        path.lineTo(box.left() + 7.0, box.top() + 7.0);
        break;
    }
    case Glyph::Minimize:
        path.moveTo(QPointF(box.left(), center.y() + 3.0));
        path.lineTo(QPointF(box.right(), center.y() + 3.0));
        break;
    }

    // 关闭按钮按下是红的(别的按钮才是绿的):绿 × 放在关闭键上像是在"确认"。
    QColor color = kNormal;
    if (pressed)
        color = close ? kClosePressedGlyph : kPressed;
    else if (hovered)
        color = close ? QColor(0xff, 0xd0, 0xd0) : kHover;

    if (hovered && !pressed) {
        // 轻微外发光:同一路径用更宽的半透明笔再描一遍。
        QPen glow(color);
        glow.setWidthF(kLineWidth + 2.5);
        glow.setCapStyle(Qt::RoundCap);
        glow.setColor(QColor(color.red(), color.green(), color.blue(), 46));
        painter.setPen(glow);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }

    QPen pen(color);
    pen.setWidthF(kLineWidth);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::MiterJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
}
