#include "DedupImageDisplayWidget.h"
#include "dedupenv.h"

DedupImageDisplayWidget::DedupImageDisplayWidget(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
}

quint64 DedupImageDisplayWidget::setPixmap(const QPixmap &pix) {
    bool hadPrevious = !m_pix.isNull();

    // 记录旧 viewport（以便在尺寸一致或可恢复时能保持）
    Viewport prevVp = { m_scale, m_pan };

    m_pix = pix;
    ++m_serial;      // 换图序号:窗口靠它判断"我要等的那张图是否已经上屏"
    // 不要用 m_pix.toImage().hasAlphaChannel() —— 那会把整张大图再转回 QImage。
    // QPixmap::hasAlphaChannel() 直接问底层格式,是常数开销。
    m_pixHasAlpha = m_pix.hasAlphaChannel();

    // 更新 fit 边界，但不要强制重置 m_scale
    adjustScaleToFit();

    if (!hadPrevious) {
        // 首次加载图片：初始化 scale/pan 到适配
        m_pan = QPointF(0,0);
        // ensure scale at least min
        m_scale = qBound(m_minScale, m_scale, m_maxScale);
    } else {
        // 有先前图片时：尽量保留前一个视口
        // 但是需要 clamp scale 到新的 min/max
        m_scale = qBound(m_minScale, prevVp.scale, m_maxScale);
        m_pan = prevVp.pan;
        // 防止切换时图片完全跑出视区；轻微 clamp 保证可见性
        clampViewport();
    }

    update();
    return m_serial;
}

void DedupImageDisplayWidget::setStatusBadge(const QString &text, bool busy) {
    if (m_badgeText == text && m_badgeBusy == busy)
        return;
    m_badgeText = text;
    m_badgeBusy = busy;
    /*
     * 只重绘,不动 m_serial —— 角标/高亮的局部重绘必须和"换图"区分开,
     * 否则窗口会以为"等的那张图已经上屏"而提前解锁按钮。
     */
    update();
}

void DedupImageDisplayWidget::setScanningVisual(bool on) {
    if (m_scanning == on)
        return;
    m_scanning = on;
    /*
     * 不碰 m_serial:这只是"换个画法",不是换图。
     * 动序号会让窗口误判"等的那张图已经上屏"。
     */
    update();
}

void DedupImageDisplayWidget::drawScanningOverlay(QPainter &p) const
{
    if (!m_scanning)
        return;

    // 整块压一层很淡的暖色:画面"变浅",和平时一眼能区分开
    p.fillRect(rect(), QColor(255, 214, 130, 46));

    // 粗边框:即使画面本身是黑的/白的,边框也一定看得见
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 190, 80), 3));
    p.drawRect(rect().adjusted(1, 1, -2, -2));
}

void DedupImageDisplayWidget::preserveViewportFrom(const DedupImageDisplayWidget *other) {
    if(!other) return;
    m_scale = other->m_scale;
    m_pan = other->m_pan;
    update();
}


void DedupImageDisplayWidget::clampViewport() {
    if (m_pix.isNull()) return;

    // 计算显示尺寸
    QSizeF s = QSizeF(m_pix.size()) * m_scale;
    // 当前显示矩形
    double left = (width() - s.width()) / 2.0 + m_pan.x();
    double top  = (height() - s.height()) / 2.0 + m_pan.y();

    // 我们允许图片部分移出视区，但防止它完全移出：
    // 保证 displayedRect 的任意一边与 widget 边有重叠（至少 1 px）
    QRectF dr(left, top, s.width(), s.height());

    // 如果图片完全在右侧/下侧/左侧/上侧之外，则把 pan 调整回使图片与视区有 1px 重叠
    if (dr.right() < 1.0) {
        m_pan.setX(m_pan.x() + (1.0 - dr.right()));
    }
    if (dr.bottom() < 1.0) {
        m_pan.setY(m_pan.y() + (1.0 - dr.bottom()));
    }
    if (dr.left() > (width() - 1.0)) {
        m_pan.setX(m_pan.x() - (dr.left() - (width() - 1.0)));
    }
    if (dr.top() > (height() - 1.0)) {
        m_pan.setY(m_pan.y() - (dr.top() - (height() - 1.0)));
    }

    // 额外 clamp：保证 scale 在范围内
    m_scale = qBound(m_minScale, m_scale, m_maxScale);
}

void DedupImageDisplayWidget::fitToWidget(bool center) {
    if (m_pix.isNull()) return;
    QSize img = m_pix.size();
    if (img.width() <= 0 || img.height() <= 0) return;

    // 计算适配缩放（与 adjustScaleToFit 相同的逻辑，但这里我们 *设置* scale）
    double sx = double(width()) / img.width();
    double sy = double(height()) / img.height();
    double target = qMin(sx, sy);

    // 更新 minScale（保持一致性）
    m_minScale = target;

    // 把 scale 设置为刚好 fit（并 clamp）
    m_scale = qBound(m_minScale, target, m_maxScale);

    // 可选：重置平移到居中（默认 true）
    if (center) {
        m_pan = QPointF(0,0);
    }

    // 防止图片完全跑出视区（小调整）
    clampViewport();

    update();
}
void DedupImageDisplayWidget::adjustScaleToFit() {
    if (m_pix.isNull()) return;
    QSize img = m_pix.size();
    if (img.width() <= 0 || img.height() <= 0) return;

    double sx = double(width()) / img.width();
    double sy = double(height()) / img.height();
    double newMin = qMin(sx, sy);

    // 不要无条件覆盖 m_scale —— 只更新 m_minScale 并 clamp 当前 scale
    m_minScale = newMin;
    if (m_scale < m_minScale) m_scale = m_minScale;
    // 不把 m_scale 直接设为 m_minScale（避免覆盖用户缩放）
}


QRectF DedupImageDisplayWidget::displayedRect() const {
    if (m_pix.isNull()) return QRectF();
    QSizeF s = QSizeF(m_pix.size()) * m_scale;
    double x = (width() - s.width()) / 2.0 + m_pan.x();
    double y = (height() - s.height()) / 2.0 + m_pan.y();
    return QRectF(x, y, s.width(), s.height());
}

// helper: 生成并缓存棋盘格 pixmap
static QPixmap makeCheckerboardPixmap(int tileSize = 10) {
    static QHash<int, QPixmap> cache;
    if (cache.contains(tileSize)) return cache.value(tileSize);

    int w = tileSize * 2;
    int h = tileSize * 2;
    QPixmap px(w, h);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing, false);

    QColor light(220, 220, 220);
    QColor dark(160, 160, 160);

    p.fillRect(0, 0, tileSize, tileSize, light);
    p.fillRect(tileSize, tileSize, tileSize, tileSize, light);
    p.fillRect(tileSize, 0, tileSize, tileSize, dark);
    p.fillRect(0, tileSize, tileSize, tileSize, dark);

    p.end();
    cache.insert(tileSize, px);
    return px;
}

void DedupImageDisplayWidget::paintEvent(QPaintEvent *ev) {
    Q_UNUSED(ev);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    if (m_pix.isNull()) {
        p.fillRect(rect(), palette().window());
        drawScanningOverlay(p);     // 没有图的时候也要能看出"正在扫描"
        drawStatusBadge(p);
        emit imagePainted(m_serial);
        return;
    }

    /*
     * 只做一次 blit —— 缩放和 overlay 合成已经在 ensureRenderCache() 里
     * 按参数指纹缓存好了(见头文件里的说明)。
     * 缓存正常时这里连一次缩放都不会发生,highlight 定时器/拖动重绘都不再重新采样大图。
     */
    if (!ensureRenderCache()) {
        p.fillRect(rect(), palette().window());
        drawScanningOverlay(p);
        drawStatusBadge(p);
        emit imagePainted(m_serial);
        return;
    }

    p.drawPixmap(0, 0, m_renderCache);

    drawScanningOverlay(p);   // 先蒙层,角标再画在它上面(保证"扫描中"字样清晰)
    drawStatusBadge(p);

    emit imagePainted(m_serial);
}

/*
 * 左下角状态角标。
 *
 * 为什么要画在显示区里,而不是只靠侧栏/标题:
 *   · 侧栏在"切换中"是被清空的,空的框看起来像坏了,不像"正在加载";
 *   · 标题栏容易被忽略,而且窗口最大化时注意力都在图中间;
 *   · 最关键的是"相邻两对的图长得一模一样"时,画面根本没变化 ——
 *     用户必须有一个与图片内容无关的信号,才能知道到底切没切。
 * 所以这里常驻显示 pair 号,切换期间换成高亮的"切换中"。
 */
void DedupImageDisplayWidget::drawStatusBadge(QPainter &p) const
{
    if (m_badgeText.isEmpty())
        return;

    QFont f(appFontFamily());
    f.setPixelSize(13);
    f.setBold(true);
    p.setFont(f);

    const QFontMetrics fm(f);
    const int padX = 10;
    const int padY = 6;
    const int textW = fm.horizontalAdvance(m_badgeText);
    const int textH = fm.height();
    QRect box(12, height() - textH - padY * 2 - 12,
              textW + padX * 2, textH + padY * 2);
    if (box.width() > width() - 24)
        box.setWidth(std::max(60, width() - 24));

    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(m_badgeBusy ? QColor(60, 40, 0, 225) : QColor(0, 0, 0, 165));
    p.drawRoundedRect(box, 6, 6);

    p.setPen(m_badgeBusy ? QColor(255, 200, 90) : QColor(215, 215, 215));
    p.drawText(box, Qt::AlignCenter,
               fm.elidedText(m_badgeText, Qt::ElideMiddle, box.width() - padX));
    p.setRenderHint(QPainter::Antialiasing, false);
}


bool DedupImageDisplayWidget::ensureRenderCache() const
{
    const QRectF targetF = displayedRect();
    if (targetF.isEmpty()) return false;

    const QRect target = targetF.toRect();
    const QSize sz = size();
    const qreal dpr = devicePixelRatioF();
    const qint64 pixKey = m_pix.cacheKey();

    /*
     * 参数指纹命中就不用重新合成。
     * 平移/缩放/尺寸/换图/overlay 开关都会改变指纹,所以不需要在
     * setPixmap/setViewport/fitToWidget/resizeEvent 里逐处手工失效 —— 少一个漏点。
     */
    if (m_renderCacheValid
        && m_rcPixKey == pixKey
        && m_rcSize == sz
        && qFuzzyCompare(m_rcScale + 1.0, m_scale + 1.0)
        && m_rcPan == m_pan
        && qFuzzyCompare(m_rcDpr + 1.0, dpr + 1.0)) {
        return true;
    }

    // 缓存按设备像素比分配,HiDPI 下才不会糊
    const QSize cacheSize(qCeil(sz.width() * dpr), qCeil(sz.height() * dpr));
    if (m_renderCache.isNull() || m_renderCache.size() != cacheSize) {
        m_renderCache = QPixmap(cacheSize);
    }
    m_renderCache.setDevicePixelRatio(dpr);

    QPainter rp(&m_renderCache);
    rp.setRenderHint(QPainter::Antialiasing, false);
    rp.fillRect(QRect(QPoint(0, 0), sz), palette().window());
    rp.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (m_pixHasAlpha) {
        const int tileSize = 10;
        QPixmap checker = makeCheckerboardPixmap(tileSize);
        QBrush brush(checker);
        rp.save();
        rp.setBrushOrigin(target.topLeft());
        rp.fillRect(target, brush);
        rp.restore();
    }

    rp.drawPixmap(target, m_pix);
    rp.end();

    m_rcPixKey      = pixKey;
    m_rcSize        = sz;
    m_rcScale       = m_scale;
    m_rcPan         = m_pan;
    m_rcDpr         = dpr;
    m_renderCacheValid = true;
    return true;
}

void DedupImageDisplayWidget::zoomToScaleAtPoint(double newScale, const QPointF &pos) {
    if (m_pix.isNull()) return;
    // clamp
    newScale = qBound(m_minScale, newScale, m_maxScale);

    // 以 pos 为焦点进行缩放
    QPointF mpos = pos;
    QPointF before = (mpos - QPointF(width()/2.0, height()/2.0) - m_pan) / m_scale;
    double oldScale = m_scale;
    m_scale = newScale;
    QPointF after = (mpos - QPointF(width()/2.0, height()/2.0) - m_pan) / m_scale;
    m_pan += (after - before) * m_scale;
    update();
}

void DedupImageDisplayWidget::performLargeZoomOrRestoreAt(const QPointF &pos) {
    if (m_pix.isNull()) return;

    if (!m_isLargeZoomed) {
        // 保存当前视口以便恢复
        m_prevViewport = { m_scale, m_pan };

        // 目标放大策略（把 widget 显示约 256×256 图像像素，或至少 1:1，或当前 * 8）
        double byPixelGoalW = double(width()) / 256.0;
        double byPixelGoalH = double(height()) / 256.0;
        double byPixelGoal = qMin(byPixelGoalW, byPixelGoalH); // 宽高任一小的一方作为目标
        double candidate = qMax(m_scale * 8.0, qMax(1.0, byPixelGoal));
        candidate = qBound(m_minScale, candidate, m_maxScale);
        zoomToScaleAtPoint(candidate, pos);
        m_isLargeZoomed = true;
    } else {
        // 恢复之前保存的视口
        m_scale = qBound(m_minScale, m_prevViewport.scale, m_maxScale);
        m_pan = m_prevViewport.pan;
        m_isLargeZoomed = false;
        update();
    }
}

void DedupImageDisplayWidget::wheelEvent(QWheelEvent *ev) {
    if (m_pix.isNull()) { ev->ignore(); return; }

    // 优先使用真实 Ctrl 键状态或外部 m_ctrlEnabled（兼容）
    bool ctrlActive = m_ctrlPressed || (ev->modifiers() & Qt::ControlModifier);

    if (ctrlActive) {
        // Ctrl 下滚轮用于切图（向外部发送 delta）
        int d = ev->angleDelta().y();
        emit wheelScrolled(d);
        return;
    } else {
        // 否则滚轮用于缩放。提高灵敏度：用指数函数根据 delta 调整
        double delta = double(ev->angleDelta().y());
        double factor = std::pow(1.003, delta);
        QPointF mpos = ev->position();
        QPointF before = (mpos - QPointF(width()/2.0, height()/2.0) - m_pan) / m_scale;
        double newScale = m_scale * factor;
        newScale = qBound(m_minScale, newScale, m_maxScale);
        m_scale = newScale;
        QPointF after = (mpos - QPointF(width()/2.0, height()/2.0) - m_pan) / m_scale;
        m_pan += (after - before) * m_scale;
        update();
    }
}

void DedupImageDisplayWidget::mousePressEvent(QMouseEvent *ev) {
    if (ev->button() == Qt::LeftButton) {
        m_lastMouse = ev->pos();
        m_dragging = false;
        m_maybeClick = true;
    }
    // 右键在 release 时处理（区分拖拽/短点击）
}

void DedupImageDisplayWidget::mouseMoveEvent(QMouseEvent *ev) {
    // 拖拽仅在 m_ctrlEnabled（外部启用）或 space/ctrl 按下时有效
    if(ev->buttons() & Qt::LeftButton){

        bool dragAllowed = !m_ctrlPressed;
        if (dragAllowed) {
            QPoint dp = ev->pos() - m_lastMouse;
            if (!m_dragging) {
                if (std::abs(dp.x()) > 4 || std::abs(dp.y()) > 4) {
                    m_dragging = true;
                    m_maybeClick = false;
                }
            }
            if (m_dragging) {
                m_pan += QPointF(dp);
                m_lastMouse = ev->pos();
                update();
            }
        }
    }
}

void DedupImageDisplayWidget::mouseReleaseEvent(QMouseEvent *ev) {
    if (ev->button() == Qt::LeftButton) {
        if (m_dragging) {
            m_dragging = false;
            m_maybeClick = false;
            return;
        }
        // short click (distance small)
        if (ev->modifiers() & Qt::ControlModifier) {
            emit leftClicked(); // 本地操作 — 记录为本地短切换
        } else {
            emit toggleRequested();
        }
        return;
    } else if (ev->button() == Qt::RightButton) {
        // 右键：执行放大或恢复
        performLargeZoomOrRestoreAt(ev->position());
        // 不自动 emit rightClicked()（如果你仍需右键信号再加 emit）
        return;
    }
}

void DedupImageDisplayWidget::resizeEvent(QResizeEvent *ev) {
    QWidget::resizeEvent(ev);
    adjustScaleToFit();
    // 当控件尺寸变化时，如果处于 large-zoom 状态，保持比例但 clamp
    m_scale = qBound(m_minScale, m_scale, m_maxScale);
    update();
}

void DedupImageDisplayWidget::keyPressEvent(QKeyEvent *ev) {
    if (ev->isAutoRepeat()) {
        QWidget::keyPressEvent(ev);
        return;
    }
    if (ev->key() == Qt::Key_Control) {
        m_ctrlPressed = true;
    } else if (ev->key() == Qt::Key_Space) {
        m_spacePressed = true;
        // 为了使 space drag 更可靠，强制获取焦点（避免 keyRelease 丢失情况）
        if (!hasFocus())
            setFocus(Qt::MouseFocusReason);
        QWidget::keyPressEvent(ev);
    } else {
        QWidget::keyPressEvent(ev);
    }
    update();
}

void DedupImageDisplayWidget::keyReleaseEvent(QKeyEvent *ev) {
    if (ev->isAutoRepeat()) {
        QWidget::keyReleaseEvent(ev);
        return;
    }
    if (ev->key() == Qt::Key_Control) {
        m_ctrlPressed = false;
    } else if (ev->key() == Qt::Key_Space) {
        m_spacePressed = false;
    } else {
        QWidget::keyReleaseEvent(ev);
    }
    update();
}

void DedupImageDisplayWidget::focusOutEvent(QFocusEvent *ev) {
    // 如果控件失去焦点（例如外部窗口干扰），清理按键状态，避免“粘键”问题
    Q_UNUSED(ev);
    m_ctrlPressed = false;
    m_spacePressed = false;
    QWidget::focusOutEvent(ev);
    update();
}
