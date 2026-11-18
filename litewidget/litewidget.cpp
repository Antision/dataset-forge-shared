#include "litewidget.h"

#include <QPainter>
#include <QVBoxLayout>

namespace {
const QColor kBorderColor(74, 74, 82);     // 深灰边框
const QColor kBodyColor(27, 27, 31, 250);  // 与 LiteWidget2 的 body 同色
constexpr int kBorder = 1;
} // namespace

LiteWidget::LiteWidget(QWidget* parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setAutoFillBackground(false);

    m_outerLayout = new QVBoxLayout(this);
    m_outerLayout->setContentsMargins(kBorder, kBorder, kBorder, kBorder);
    m_outerLayout->setSpacing(0);

    m_content = new QWidget(this);
    m_content->setAutoFillBackground(false);
    m_outerLayout->addWidget(m_content);
}

LiteWidget::~LiteWidget() = default;

void LiteWidget::setContentWidget(QWidget* widget)
{
    if (widget == m_content)
        return;
    if (m_content) {
        m_outerLayout->removeWidget(m_content);
        delete m_content;
    }
    m_content = widget;
    if (m_content) {
        m_content->setParent(this);
        m_outerLayout->addWidget(m_content);
    }
}

QVBoxLayout* LiteWidget::contentLayout() const
{
    if (!m_content)
        return nullptr;
    if (auto* existing = qobject_cast<QVBoxLayout*>(m_content->layout()))
        return existing;
    auto* layout = new QVBoxLayout(m_content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    return layout;
}

void LiteWidget::setNoActivate(bool on)
{
    if (m_noActivate == on)
        return;
    m_noActivate = on;
    setAttribute(Qt::WA_ShowWithoutActivating, on);
    setAttribute(Qt::WA_X11DoNotAcceptFocus, on);
}

void LiteWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), kBodyColor);
    QPen pen(kBorderColor);
    pen.setWidth(kBorder);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}
