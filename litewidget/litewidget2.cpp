#include "litewidget2.h"

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
#include <windowsx.h>
#endif

namespace {
constexpr int kResizeBorder = 6;
constexpr int kSnapDistance = 8;                    // 拖到顶边 +8px 内最大化
constexpr int kTearOffDistance = 24;                // 下拖 24px 撕下
using litewidget2::kBodyColor;
using litewidget2::kContentInset;
using litewidget2::kOutlineColor;
using litewidget2::kStripColor;

/// 进程内的窗口几何记忆(key -> 上次的几何)。不落盘:重启程序回到推荐尺寸。
QHash<QString, QRect> s_savedGeometry;
} // namespace

LiteWidget2::LiteWidget2(QWidget* parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setMouseTracking(true);
    setMinimumSize(320, 160);
    setWindowTitle(QStringLiteral("LiteWidget2"));

    m_content = new QWidget(this);
    m_content->setObjectName(QStringLiteral("litewidget2Content"));
    m_content->setAutoFillBackground(false);

    m_closeButton = new WindowGlyphButton(WindowGlyphButton::Glyph::Close, this);
    m_toggleButton = new WindowGlyphButton(WindowGlyphButton::Glyph::Restore, this);
    m_minimizeButton = new WindowGlyphButton(WindowGlyphButton::Glyph::Minimize, this);
    for (WindowGlyphButton* button : {m_closeButton, m_toggleButton, m_minimizeButton}) {
        button->setFixedSize(litewidget2::kCloseWidth, litewidget2::kStripHeight);
    }
    connect(m_closeButton, &QPushButton::clicked, this, &LiteWidget2::requestClose);
    connect(m_toggleButton, &QPushButton::clicked, this, [this] { setMaximizedFlag(false); });
    connect(m_minimizeButton, &QPushButton::clicked, this, &QWidget::showMinimized);
    // 最大化才出现的两颗,普通状态下藏起来(凸起条里只有关闭)。
    m_toggleButton->hide();
    m_minimizeButton->hide();

    // 不加 WS_THICKFRAME:系统的边框会在跨屏 DPI 切换时被反复扣一圈,窗口越拖
    // 越小。边缘缩放改成自己做(见 resizeEdgesAt / performResize)。
    m_content->installEventFilter(this);
    m_content->setMouseTracking(true);

    resize(480, 320);
    layoutChildren();
}

LiteWidget2::~LiteWidget2() = default;

void LiteWidget2::setContentWidget(QWidget* widget)
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

QVBoxLayout* LiteWidget2::contentLayout() const
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

QRect LiteWidget2::stripRect() const { return litewidget2::stripRect(rect(), m_maximized); }
QRect LiteWidget2::handleRect() const { return litewidget2::handleRect(rect(), m_maximized); }
QRect LiteWidget2::closeRect() const { return litewidget2::closeRect(rect(), m_maximized); }
QRect LiteWidget2::toggleRect() const { return litewidget2::toggleRect(rect(), m_maximized); }
QRect LiteWidget2::minimizeRect() const { return litewidget2::minimizeRect(rect(), m_maximized); }
QRect LiteWidget2::bodyRect() const { return litewidget2::bodyRect(rect()); }

QSize LiteWidget2::sizeHint() const
{
    // 推荐尺寸 = 内容自己算出来的尺寸 + 轮廓让出来的边(条高 + 两圈内框)。
    // 以前这里写死 560x420,所有 LiteWidget2 窗口都被撑成一个尺寸:内容少的
    // 一片空白,内容多的挤到行叠行。
    QSize content;
    if (m_content) {
        if (QLayout* layout = m_content->layout())
            content = layout->sizeHint();
        else
            content = m_content->sizeHint();
    }
    if (!content.isValid() || content.isEmpty())
        return QSize(560, 420);
    return QSize(content.width() + 2 * kContentInset,
                 content.height() + litewidget2::kStripHeight + 2 * kContentInset);
}

QSize LiteWidget2::minimumSizeHint() const
{
    QSize content;
    if (m_content) {
        if (QLayout* layout = m_content->layout())
            content = layout->minimumSize();
        else
            content = m_content->minimumSizeHint();
    }
    if (!content.isValid())
        content = QSize(0, 0);
    return QSize(qMax(320, content.width() + 2 * kContentInset),
                 qMax(160, content.height() + litewidget2::kStripHeight + 2 * kContentInset));
}

void LiteWidget2::setGeometryKey(const QString& key)
{
    m_geometryKey = key;
}

QString LiteWidget2::geometryKey() const
{
    if (!m_geometryKey.isEmpty())
        return m_geometryKey;
    return QString::fromLatin1(metaObject()->className());
}

bool LiteWidget2::hasStoredGeometry() const
{
    return s_savedGeometry.contains(geometryKey());
}

void LiteWidget2::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_maximized)
        return;

    QScreen* screen = this->screen();
    if (!screen)
        screen = QGuiApplication::primaryScreen();

    // 下限跟着内容走:窗口被拖得比内容还小的时候,原本靠布局撑住的行会叠在
    // 一起(服饰窗那种多行的最明显)。上限是当前屏幕,免得小屏上开不出来。
    QSize minimum = minimumSizeHint();
    if (screen)
        minimum = minimum.boundedTo(screen->availableGeometry().size());
    if (minimum != minimumSize())
        setMinimumSize(minimum);

    const auto it = s_savedGeometry.constFind(geometryKey());
    if (it == s_savedGeometry.constEnd() || !it->isValid())
        return;

    QRect geometry = *it;
    // 按"上次那块屏"来夹:新窗口可能被系统摆到光标所在的另一块屏上,用当前屏
    // 判断会误以为这块几何在屏幕外,于是干脆不恢复。
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

void LiteWidget2::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    // 最大化 / 最小化时记下来的不是用户调的那个尺寸。
    if (m_maximized || isMinimized())
        return;
    if (geometry().isValid())
        s_savedGeometry.insert(geometryKey(), geometry());
}

void LiteWidget2::layoutChildren()
{
    if (m_content) {
        // 顶边在普通状态也要让出内框(4px),否则面板的深色底会把上边框盖掉;
        // 最大化时标题栏在窗口内部,不需要留。
        const int top = litewidget2::kStripHeight + (m_maximized ? 0 : kContentInset);
        m_content->setGeometry(kContentInset, top,
                               width() - 2 * kContentInset,
                               height() - top - kContentInset);
        // 面板自己会铺一层深色底(消费方的 QSS),不裁的话左下/右下会被画成直角,
        // 把 body 的圆角盖掉。内缩两圈后内角已是直角,半径给 0。
        const QPainterPath clip = litewidget2::contentPath(
            QRectF(QPointF(0, 0), QSizeF(m_content->size())),
            qMax(0.0, qreal(litewidget2::kBodyRadius - kContentInset)));
        m_content->setMask(QRegion(clip.toFillPolygon().toPolygon()));
    }
    if (m_closeButton) {
        m_closeButton->setFixedSize(closeRect().size());
        m_closeButton->setGeometry(closeRect());
    }
    if (m_toggleButton) {
        m_toggleButton->setFixedSize(toggleRect().size());
        m_toggleButton->setGeometry(toggleRect());
    }
    if (m_minimizeButton) {
        m_minimizeButton->setFixedSize(minimizeRect().size());
        m_minimizeButton->setGeometry(minimizeRect());
    }
    if (m_closeButton)
        m_closeButton->raise();
    if (m_toggleButton)
        m_toggleButton->raise();
    if (m_minimizeButton)
        m_minimizeButton->raise();
}

void LiteWidget2::updateChrome()
{
    if (m_toggleButton) {
        m_toggleButton->setGlyph(m_maximized ? WindowGlyphButton::Glyph::Restore
                                             : WindowGlyphButton::Glyph::Maximize);
        m_toggleButton->setVisible(m_maximized);
    }
    if (m_minimizeButton)
        m_minimizeButton->setVisible(m_maximized);
    layoutChildren();
    update();
}

void LiteWidget2::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 三层填充:浅灰外框 → 条灰内框 → body。不用描边,所以 150%/200% 缩放下
    // 四条边的粗细一致(描边会被摊到两个设备像素上,上边框看着就细一半)。
    const QRectF outer = QRectF(rect());

    // 内缩量按"垂直于边"算,cos^2 过渡段才和直边一样宽。
    painter.fillPath(litewidget2::outlinePath(outer, m_maximized, 0), kOutlineColor);   // 1
    painter.fillPath(litewidget2::outlinePath(outer, m_maximized, litewidget2::kStroke),
                     kStripColor);                                                     // 2
    painter.fillPath(litewidget2::bodyPath(outer, kContentInset, !m_maximized), kBodyColor); // 3

    // 4. 玻璃标题栏:把顶部这一条(平的 + cos² 过渡)的像素**换成**半透明色。
    //    不能叠上去:alpha 叠在已经是不透明的条灰上,结果还是不透明,只是变亮。
    //    Source 只作用在裁剪区里,所以 1px 外框和左右下的内框带仍然是不透明的。
    painter.save();
    painter.setClipPath(litewidget2::outlinePath(outer, m_maximized, litewidget2::kStroke));
    painter.setClipRect(QRectF(0, 0, outer.width(), litewidget2::kStripHeight),
                        Qt::IntersectClip);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(QRectF(0, 0, outer.width(), litewidget2::kStripHeight),
                     litewidget2::kStripGlassColor);
    painter.restore();
}

void LiteWidget2::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    // 只在 DPI 没变时记录"用户设定的逻辑尺寸"。跨屏触发的那次 resize 不会被
    // 记进来,否则保护值本身会跟着一起缩。
    const qreal dpr = devicePixelRatioF();
    if (!m_sizeLocked && qFuzzyCompare(dpr, m_stableDpr == 0.0 ? dpr : m_stableDpr)) {
        m_stableSize = size();
        m_stableDpr = dpr;
    }
    layoutChildren();
}

void LiteWidget2::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const int edges = resizeEdgesAt(event->pos());
        if (edges != EdgeNone) {
            beginResize(edges, event->globalPosition().toPoint());
            event->accept();
            return;
        }
    }
    if ((event->button() == Qt::LeftButton || event->button() == Qt::RightButton)
        && handleRect().contains(event->pos())) {
        m_dragging = true;
        m_dragButton = event->button();
        m_pressGlobal = event->globalPosition().toPoint();
        m_dragOffset = m_pressGlobal - pos();
        m_dragSize = size();
        m_tornOff = false;
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void LiteWidget2::mouseMoveEvent(QMouseEvent* event)
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
        // 最大化后继续下拖超过 24px 才"撕下",否则保持最大化(避免手抖还原)。
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

    // 用 setGeometry 而不是 move:跨屏时系统可能顺手改掉尺寸,这里每次都把
    // 按下时的尺寸一起写回去,永远不会"越拖越小"。
    setGeometry(QRect(target, m_dragSize.isValid() ? m_dragSize : size()));
    event->accept();
}

void LiteWidget2::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_resizing) {
        m_resizing = false;
        m_resizeEdges = EdgeNone;
        updateResizeCursor(EdgeNone);
        event->accept();
        return;
    }
    if (!m_dragging) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;
    const Qt::MouseButton button = m_dragButton;
    m_dragButton = Qt::NoButton;
    // 右键在把手上:单击最小化,拖完松开也最小化(LW-04)。
    if (button == Qt::RightButton)
        showMinimized();
    event->accept();
}

void LiteWidget2::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        requestClose();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void LiteWidget2::closeEvent(QCloseEvent* event)
{
    // Alt+F4 等系统路径也要通知管理者,和关闭按钮保持同一语义。
    event->accept();
    emit closed();
}

void LiteWidget2::changeEvent(QEvent* event)
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

void LiteWidget2::requestClose()
{
    hide();
    emit closed();
}

void LiteWidget2::setMaximizedFlag(bool on)
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
        const QRect restored = m_restoreGeometry;
        m_maximized = false;
        if (restored.isValid())
            setGeometry(restored);
        updateChrome();
    }
}

void LiteWidget2::applyMaximized(bool on, const QRect& available)
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

void LiteWidget2::restoreFromMaximized(const QPoint& globalCursor)
{
    QRect restored = m_restoreGeometry;
    if (!restored.isValid())
        restored = QRect(QPoint(0, 0), sizeHint());

    // 保持光标在窗口里的相对横向位置,撕下来才不会跳到一边。
    const qreal relativeX = width() > 0 ? qreal(m_dragOffset.x()) / qreal(width()) : 0.5;
    m_maximized = false;
    setGeometry(restored);
    const int x = globalCursor.x() - qRound(relativeX * restored.width());
    const int y = globalCursor.y() - litewidget2::kStripHeight / 2;
    move(x, y);
    m_dragOffset = globalCursor - pos();
    // 撕下来以后拖动用的是新尺寸,不能继续沿用最大化时的那个。
    m_dragSize = size();
    updateChrome();
}

int LiteWidget2::resizeEdgesAt(const QPoint& pos) const
{
    if (m_maximized)
        return EdgeNone;
    const int border = kResizeBorder;
    int edges = EdgeNone;
    if (pos.x() < border)
        edges |= EdgeLeft;
    if (pos.x() >= width() - border)
        edges |= EdgeRight;
    if (pos.y() >= height() - border)
        edges |= EdgeBottom;
    // 顶边只有 body 的平顶那一段参与;strip(含过渡区)不参与。
    const int flatTopEnd = width() - litewidget2::tabWidth(width());
    if (pos.y() >= litewidget2::kStripHeight - border
        && pos.y() <= litewidget2::kStripHeight + border
        && pos.x() < flatTopEnd) {
        edges |= EdgeTop;
    }
    return edges;
}

void LiteWidget2::updateResizeCursor(int edges)
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

void LiteWidget2::beginResize(int edges, const QPoint& global)
{
    m_resizing = true;
    m_resizeEdges = edges;
    m_resizeStartGeometry = geometry();
    m_resizeStartGlobal = global;
}

void LiteWidget2::performResize(const QPoint& global)
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

bool LiteWidget2::eventFilter(QObject* watched, QEvent* event)
{
    // 内容面板盖住了除 3px 边框以外的整个窗口;边缘 6px 的缩放带有一半落在
    // 面板上,所以面板的鼠标事件要先过一遍这里。
    if (watched == m_content) {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                const QPoint pos = m_content->mapTo(this, mouse->position().toPoint());
                const int edges = resizeEdgesAt(pos);
                if (edges != EdgeNone) {
                    beginResize(edges, mouse->globalPosition().toPoint());
                    mouse->accept();
                    return true;
                }
            }
            break;
        }
        case QEvent::MouseMove: {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (m_resizing) {
                performResize(mouse->globalPosition().toPoint());
                mouse->accept();
                return true;
            }
            const QPoint pos = m_content->mapTo(this, mouse->position().toPoint());
            updateResizeCursor(resizeEdgesAt(pos));
            break;
        }
        case QEvent::MouseButtonRelease: {
            if (m_resizing) {
                m_resizing = false;
                m_resizeEdges = EdgeNone;
                updateResizeCursor(EdgeNone);
                event->accept();
                return true;
            }
            break;
        }
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool LiteWidget2::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG") {
        MSG* msg = static_cast<MSG*>(message);
        if (msg->message == WM_NCCALCSIZE && msg->wParam) {
            // 无边框:整窗都是客户区。最大化也是自己设 geometry,不走系统状态,
            // 所以不需要收窄工作区。
            *result = 0;
            return true;
        }
    }
#endif
    return QWidget::nativeEvent(eventType, message, result);
}
