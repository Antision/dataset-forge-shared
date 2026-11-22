#include "litewidget.h"

#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
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

void liteCenterOnFirstShow(QWidget* window)
{
    if (!window)
        return;
    // A caller that placed the window itself (the menus and the add widget move
    // to the cursor before showing) keeps its position.
    if (window->testAttribute(Qt::WA_Moved))
        return;

    QScreen* screen = window->screen();
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect();

    QRect frame(window->pos(), window->size());
    if (QWidget* owner = window->parentWidget()) {
        // Owned windows are placed by the system at the owner *widget's* origin,
        // so a dialog owned by a page deep in the editor column opened at the
        // bottom of the main window - half off-screen. The owner window's centre
        // is where these windows belong.
        frame.moveCenter(owner->window()->frameGeometry().center());
    } else if (available.isValid()) {
        frame.moveCenter(available.center());
    }

    if (available.isValid()) {
        frame.setWidth(qMin(frame.width(), available.width()));
        frame.setHeight(qMin(frame.height(), available.height()));
        frame.moveLeft(qBound(available.left(), frame.left(),
                              qMax(available.left(), available.right() - frame.width() + 1)));
        frame.moveTop(qBound(available.top(), frame.top(),
                             qMax(available.top(), available.bottom() - frame.height() + 1)));
    }
    window->setGeometry(frame);
}

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

QWidget* LiteWidget::takeContentWidget()
{
    if (!m_content)
        return nullptr;
    m_outerLayout->removeWidget(m_content);
    QWidget* taken = m_content;
    m_content = nullptr;
    taken->setParent(nullptr);
    return taken;
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
