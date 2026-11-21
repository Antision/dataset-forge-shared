#include "litewindow.h"

#include "litewidget2geometry.h"
#include "windowglyphbutton.h"

#include <QCloseEvent>
#include <QGuiApplication>
#include <QHash>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QRegion>
#include <QScreen>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

constexpr int kResizeBorder = 6;
constexpr int kSnapDistance = 8;      // 拖到屏幕顶 +8px 内最大化
constexpr int kTearOffDistance = 24;  // 最大化状态下拖 24px 还原

/// 进程内的窗口几何记忆(key -> 上次的几何)。不落盘:重启回到推荐尺寸。
QHash<QString, QRect> s_savedGeometry;

} // namespace

LiteWindow::LiteWindow(QWidget* parent)
    : LiteWidget(parent)
{
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setMouseTracking(true);
    setMinimumSize(320, 160);
    setWindowTitle(QStringLiteral("LiteWindow"));

    // 基类那层"1px 边框里的内容容器"用不上:本类的内容在标题栏下面,自己摆。
    delete takeContentWidget();
    if (QLayout* outer = layout())
        outer->setContentsMargins(0, 0, 0, 0);

    m_content = new QWidget(this);
    m_content->setObjectName(QStringLiteral("litewindowContent"));
    m_content->setAutoFillBackground(false);

    m_closeButton = new WindowGlyphButton(WindowGlyphButton::Glyph::Close, this);
    m_toggleButton = new WindowGlyphButton(WindowGlyphButton::Glyph::Maximize, this);
    m_minimizeButton = new WindowGlyphButton(WindowGlyphButton::Glyph::Minimize, this);
    connect(m_closeButton, &QPushButton::clicked, this, &LiteWindow::requestClose);
    connect(m_toggleButton, &QPushButton::clicked, this,
            [this] { setMaximizedFlag(!m_maximized); });
    connect(m_minimizeButton, &QPushButton::clicked, this, &QWidget::showMinimized);

    // 这里不要 resize:一 resize 就置上 WA_Resized,show() 时就不会再按
    // sizeHint() 定尺寸,派生类重写的 sizeHint() 全部失效(filterWidget 是
    // 430x780 的竖窗,结果每次开出来都是 640x480 的横窗)。第一次显示时在
    // showEvent 里按 sizeHint 定。
    layoutChildren();
}

LiteWindow::~LiteWindow() = default;

void LiteWindow::setContentWidget(QWidget* widget)
{
    if (widget == m_content)
        return;
    delete m_content;
    m_content = widget;
    if (m_content) {
        m_content->setParent(this);
        m_content->show();
    }
    layoutChildren();
}

QVBoxLayout* LiteWindow::contentLayout() const
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

QRect LiteWindow::titleBarRect() const
{
    return QRect(0, 0, width(), litewidget2::kStripHeight);
}

QRect LiteWindow::closeRect() const
{
    return QRect(width() - litewidget2::kCloseWidth, 0,
                 litewidget2::kCloseWidth, litewidget2::kStripHeight);
}

QRect LiteWindow::toggleRect() const
{
    return closeRect().translated(-litewidget2::kCloseWidth, 0);
}

QRect LiteWindow::minimizeRect() const
{
    return toggleRect().translated(-litewidget2::kCloseWidth, 0);
}

QSize LiteWindow::sizeHint() const
{
    QSize content;
    if (m_content) {
        if (QLayout* contentLayout = m_content->layout())
            content = contentLayout->sizeHint();
        else
            content = m_content->sizeHint();
    }
    if (!content.isValid() || content.isEmpty())
        return QSize(640, 480);
    return QSize(content.width() + 2 * litewidget2::kContentInset,
                 content.height() + litewidget2::kStripHeight + litewidget2::kContentInset);
}

QSize LiteWindow::minimumSizeHint() const
{
    QSize content;
    if (m_content) {
        if (QLayout* contentLayout = m_content->layout())
            content = contentLayout->minimumSize();
        else
            content = m_content->minimumSizeHint();
    }
    if (!content.isValid())
        content = QSize(0, 0);
    return QSize(qMax(320, content.width() + 2 * litewidget2::kContentInset),
                 qMax(160, content.height() + litewidget2::kStripHeight
                               + litewidget2::kContentInset));
}

void LiteWindow::setGeometryKey(const QString& key)
{
    m_geometryKey = key;
}

QString LiteWindow::geometryKey() const
{
    if (!m_geometryKey.isEmpty())
        return m_geometryKey;
    return QString::fromLatin1(metaObject()->className());
}

bool LiteWindow::hasStoredGeometry() const
{
    return s_savedGeometry.contains(geometryKey());
}

void LiteWindow::layoutChildren()
{
    if (m_content) {
        const int inset = litewidget2::kContentInset;
        const int top = litewidget2::kStripHeight;
        m_content->setGeometry(inset, top, width() - 2 * inset,
                               height() - top - inset);
        // 面板多半会铺自己的深色底(QSS),不裁会把左下/右下画成直角。
        const QPainterPath clip = litewidget2::contentPath(
            QRectF(QPointF(0, 0), QSizeF(m_content->size())), 0.0);
        m_content->setMask(QRegion(clip.toFillPolygon().toPolygon()));
    }
    if (m_closeButton) {
        m_closeButton->setFixedSize(closeRect().size());
        m_closeButton->setGeometry(closeRect());
        m_closeButton->raise();
    }
    if (m_toggleButton) {
        m_toggleButton->setFixedSize(toggleRect().size());
        m_toggleButton->setGeometry(toggleRect());
        m_toggleButton->raise();
    }
    if (m_minimizeButton) {
        m_minimizeButton->setFixedSize(minimizeRect().size());
        m_minimizeButton->setGeometry(minimizeRect());
        m_minimizeButton->raise();
    }
}

void LiteWindow::updateChrome()
{
    if (m_toggleButton) {
        m_toggleButton->setGlyph(m_maximized ? WindowGlyphButton::Glyph::Restore
                                             : WindowGlyphButton::Glyph::Maximize);
        m_toggleButton->show();
    }
    if (m_minimizeButton)
        m_minimizeButton->show();
    layoutChildren();
    update();
}

QPainterPath LiteWindow::silhouette(qreal inset) const
{
    const QRectF r = QRectF(rect()).adjusted(inset, inset, -inset, -inset);
    const qreal radius = m_maximized
                             ? 0.0
                             : qMax(0.0, qreal(litewidget2::kBodyRadius) - inset);
    QPainterPath path;
    path.addRoundedRect(r, radius, radius);
    return path;
}

void LiteWindow::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 三层填充:浅灰外框 → 条灰内框 → body,和 LiteWidget2 同一套。
    painter.fillPath(silhouette(0), litewidget2::kOutlineColor);
    painter.fillPath(silhouette(litewidget2::kStroke), litewidget2::kStripColor);
    painter.fillPath(litewidget2::bodyPath(QRectF(rect()), litewidget2::kContentInset, false),
                     litewidget2::kBodyColor);

    if (!windowTitle().isEmpty()) {
        const int left = litewidget2::kContentInset + 10;
        const int right = minimizeRect().left() - 6;
        const QRect textRect(left, 0, qMax(0, right - left), litewidget2::kStripHeight);
        if (textRect.width() > 8) {
            QFont titleFont = font();
            if (titleFont.pointSizeF() > 0.0)
                titleFont.setPointSizeF(qMax(8.0, titleFont.pointSizeF() - 1.5));
            painter.setFont(titleFont);
            painter.setPen(QColor(0xc9, 0xc9, 0xc9));
            const QString elided = painter.fontMetrics().elidedText(
                windowTitle(), Qt::ElideRight, textRect.width());
            painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, elided);
        }
    }
}

void LiteWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    const qreal dpr = devicePixelRatioF();
    if (!m_sizeLocked && qFuzzyCompare(dpr, m_stableDpr == 0.0 ? dpr : m_stableDpr)) {
        m_stableSize = size();
        m_stableDpr = dpr;
    }
    layoutChildren();
}

void LiteWindow::changeEvent(QEvent* event)
{
    const bool dpiChange = event->type() == QEvent::ScreenChangeInternal
                           || event->type() == QEvent::DevicePixelRatioChange;
    if (!dpiChange) {
        QWidget::changeEvent(event);
        return;
    }

    const QSize keep = m_dragging ? m_dragSize : m_stableSize;
    const bool restore = !m_maximized && keep.isValid() && keep.width() > 0;
    m_sizeLocked = true;
    QWidget::changeEvent(event);
    m_sizeLocked = false;

    if (restore && size() != keep)
        resize(keep);
    m_stableDpr = devicePixelRatioF();
    if (!restore)
        m_stableSize = size();
    update();
}

void LiteWindow::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_maximized)
        return;

    QScreen* screen = this->screen();
    if (!screen)
        screen = QGuiApplication::primaryScreen();

    // 第一次显示、调用方也没显式 resize 过:按 sizeHint() 定尺寸。
    // (必须在 setMinimumSize 之前:那里可能顺手 resize,把 WA_Resized 置上。)
    if (!testAttribute(Qt::WA_Resized) && !hasStoredGeometry()) {
        QSize hint = sizeHint();
        if (screen)
            hint = hint.boundedTo(screen->availableGeometry().size());
        if (hint.isValid() && !hint.isEmpty())
            resize(hint);
    }

    QSize minimum = minimumSizeHint();
    if (screen)
        minimum = minimum.boundedTo(screen->availableGeometry().size());
    if (minimum != minimumSize())
        setMinimumSize(minimum);

    const auto it = s_savedGeometry.constFind(geometryKey());
    if (it == s_savedGeometry.constEnd() || !it->isValid())
        return;

    QRect geometry = *it;
    // 按"上次那块屏"来夹:新窗口可能被系统摆到光标所在的另一块屏上。
    QScreen* target = QGuiApplication::screenAt(geometry.center());
    if (!target)
        target = screen;
    if (target) {
        const QRect available = target->availableGeometry();
        geometry.setWidth(qMin(geometry.width(), available.width()));
        geometry.setHeight(qMin(geometry.height(), available.height()));
        geometry.moveLeft(qBound(available.left(), geometry.left(),
                                 qMax(available.left(), available.right() - geometry.width() + 1)));
        geometry.moveTop(qBound(available.top(), geometry.top(),
                                qMax(available.top(), available.bottom() - geometry.height() + 1)));
    }
    setGeometry(geometry);
}

void LiteWindow::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    if (m_maximized || isMinimized())
        return;
    if (geometry().isValid())
        s_savedGeometry.insert(geometryKey(), geometry());
}

void LiteWindow::closeEvent(QCloseEvent* event)
{
    event->accept();
    emit closed();
}

void LiteWindow::requestClose()
{
    emit closed();
    hide();
}

void LiteWindow::setMaximizedFlag(bool on)
{
    if (on == m_maximized)
        return;
    if (on) {
        QScreen* screen = this->screen();
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        if (screen)
            applyMaximized(true, screen->availableGeometry());
    } else {
        m_maximized = false;
        if (m_restoreGeometry.isValid())
            setGeometry(m_restoreGeometry);
        updateChrome();
    }
}

void LiteWindow::applyMaximized(bool on, const QRect& available)
{
    if (on) {
        if (!m_maximized)
            m_restoreGeometry = geometry();
        m_maximized = true;
        setGeometry(available);
    } else {
        m_maximized = false;
        if (m_restoreGeometry.isValid())
            setGeometry(m_restoreGeometry);
    }
    updateChrome();
}

void LiteWindow::restoreFromMaximized(const QPoint& globalCursor)
{
    QRect restored = m_restoreGeometry;
    if (!restored.isValid())
        restored = QRect(QPoint(0, 0), sizeHint());

    const qreal relativeX = width() > 0 ? qreal(m_dragOffset.x()) / qreal(width()) : 0.5;
    m_maximized = false;
    setGeometry(restored);
    const int x = globalCursor.x() - qRound(relativeX * restored.width());
    const int y = globalCursor.y() - litewidget2::kStripHeight / 2;
    move(x, y);
    m_dragOffset = globalCursor - pos();
    m_dragSize = size();
    updateChrome();
}

int LiteWindow::resizeEdgesAt(const QPoint& pos) const
{
    if (m_maximized)
        return EdgeNone;
    const int border = kResizeBorder;
    int edges = EdgeNone;
    if (pos.x() < border)
        edges |= EdgeLeft;
    if (pos.x() >= width() - border)
        edges |= EdgeRight;
    if (pos.y() < border)
        edges |= EdgeTop;
    if (pos.y() >= height() - border)
        edges |= EdgeBottom;
    return edges;
}

void LiteWindow::updateResizeCursor(int edges)
{
    Qt::CursorShape shape = Qt::ArrowCursor;
    switch (edges) {
    case EdgeLeft:
    case EdgeRight:
        shape = Qt::SizeHorCursor;
        break;
    case EdgeTop:
    case EdgeBottom:
        shape = Qt::SizeVerCursor;
        break;
    case EdgeLeft | EdgeTop:
    case EdgeRight | EdgeBottom:
        shape = Qt::SizeFDiagCursor;
        break;
    case EdgeRight | EdgeTop:
    case EdgeLeft | EdgeBottom:
        shape = Qt::SizeBDiagCursor;
        break;
    default:
        break;
    }
    setCursor(shape);
    if (m_content)
        m_content->setCursor(shape);
}

void LiteWindow::beginResize(int edges, const QPoint& global)
{
    m_resizing = true;
    m_resizeEdges = edges;
    m_resizeStartGeometry = geometry();
    m_resizeStartGlobal = global;
}

void LiteWindow::performResize(const QPoint& global)
{
    if (!m_resizing)
        return;
    const QPoint delta = global - m_resizeStartGlobal;
    QRect rect = m_resizeStartGeometry;
    const int minWidth = minimumWidth();
    const int minHeight = minimumHeight();

    if (m_resizeEdges & EdgeLeft)
        rect.setLeft(qMin(rect.right() - minWidth + 1, rect.left() + delta.x()));
    if (m_resizeEdges & EdgeRight)
        rect.setRight(qMax(rect.left() + minWidth - 1, rect.right() + delta.x()));
    if (m_resizeEdges & EdgeTop)
        rect.setTop(qMin(rect.bottom() - minHeight + 1, rect.top() + delta.y()));
    if (m_resizeEdges & EdgeBottom)
        rect.setBottom(qMax(rect.top() + minHeight - 1, rect.bottom() + delta.y()));

    if (rect != geometry())
        setGeometry(rect);
}

void LiteWindow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const int edges = resizeEdgesAt(event->pos());
        if (edges != EdgeNone) {
            beginResize(edges, event->globalPosition().toPoint());
            event->accept();
            return;
        }
        if (titleBarRect().contains(event->pos())) {
            m_dragging = true;
            m_pressGlobal = event->globalPosition().toPoint();
            m_dragOffset = m_pressGlobal - pos();
            m_dragSize = size();
            m_tornOff = false;
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void LiteWindow::mouseMoveEvent(QMouseEvent* event)
{
    if (m_resizing) {
        performResize(event->globalPosition().toPoint());
        event->accept();
        return;
    }
    if (!m_dragging) {
        updateResizeCursor(resizeEdgesAt(event->pos()));
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QPoint global = event->globalPosition().toPoint();
    if (m_maximized) {
        if (m_tornOff || global.y() - m_pressGlobal.y() <= kTearOffDistance) {
            event->accept();
            return;
        }
        restoreFromMaximized(global);
        m_tornOff = true;
        event->accept();
        return;
    }

    const QPoint target = global - m_dragOffset;
    QScreen* screen = QGuiApplication::screenAt(global);
    if (screen && target.y() <= screen->availableGeometry().top() + kSnapDistance) {
        m_restoreGeometry = geometry();
        applyMaximized(true, screen->availableGeometry());
        event->accept();
        return;
    }

    // 位置和尺寸一起写:跨屏时系统可能顺手改掉尺寸,这里每帧把按下时的尺寸
    // 也 assert 回去,窗口不会"越拖越小"。
    setGeometry(QRect(target, m_dragSize.isValid() ? m_dragSize : size()));
    event->accept();
}

void LiteWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_resizing) {
        m_resizing = false;
        m_resizeEdges = EdgeNone;
        updateResizeCursor(EdgeNone);
        event->accept();
        return;
    }
    if (m_dragging) {
        m_dragging = false;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void LiteWindow::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && titleBarRect().contains(event->pos())) {
        setMaximizedFlag(!m_maximized);
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void LiteWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        requestClose();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool LiteWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG") {
        MSG* msg = static_cast<MSG*>(message);
        if (msg->message == WM_NCCALCSIZE && msg->wParam) {
            *result = 0;   // 无边框:整窗都是客户区
            return true;
        }
    }
#endif
    return QWidget::nativeEvent(eventType, message, result);
}
