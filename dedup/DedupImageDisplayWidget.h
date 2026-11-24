#ifndef DEDUPIMAGEDISPLAYWIDGET_H
#define DEDUPIMAGEDISPLAYWIDGET_H
#include "deduppch.h"

class DedupImageDisplayWidget : public QWidget {
    Q_OBJECT
public:
    explicit DedupImageDisplayWidget(QWidget *parent = nullptr);

    /*
     * 换图。返回值是这一张图的"上屏序号"(自增)。
     *
     * 为什么需要序号:窗口要判断"上一次 onDisplayImagePainted 到底画的是
     * 不是我刚换上去的那张图"。仅凭"paint 事件来过"是不够的 ——
     * 角标/高亮的局部重绘也会触发 paintEvent,如果按"有 paint 就算上屏",
     * 会在新图还没到的时候提前解锁按钮、清掉"切换中"角标。
     * 换图时序号+1,窗口记住自己要等的是哪一号;两边对上才算真的上屏。
     */
    quint64 setPixmap(const QPixmap &pix);

    /*
     * 左下角状态角标:空串 = 不画。
     *
     * busy=true 时用高亮色("切换中 · pair 12 / 340"),否则用暗色
     * (常驻显示"pair 12 / 340")。常驻的用途是:两张相邻的图长得一模一样时,
     * 用户没法靠"画面变没变"判断到底切没切 —— 看角标上的号码就行。
     */
    void setStatusBadge(const QString &text, bool busy = false);

    /*
     * "正在扫描"提示:整块画面压一层半透明暖色 + 四周画一圈粗边框。
     *
     * 为什么不用 QSS 边框:本控件是自己 paintEvent 全权绘制的
     * (setAttribute(Qt::WA_OpaquePaintEvent)),样式表里的 border/background
     * 对普通 QWidget 不起作用,必须在 paint 里画。
     * 目的是让用户在"拖入图片后正在比对"的这段时间里,一眼看出窗口状态变了,
     * 别在扫描期间又拖一张新图进来。
     */
    void setScanningVisual(bool on);
    bool scanningVisual() const { return m_scanning; }

    quint64 paintedSerial() const { return m_serial; }
    void clearLargeZoomedFlag(){m_isLargeZoomed=false;}
    // preserve scale/pan from other display
    void preserveViewportFrom(const DedupImageDisplayWidget* other);

    struct Viewport { double scale; QPointF pan; };
    Viewport getViewport() const { return { m_scale, m_pan }; }
    void setViewport(const Viewport &v) { m_scale = v.scale; m_pan = v.pan; update(); }
    void fitToWidget(bool center = true);
signals:
    void imagePainted(quint64 serial);   // 参数 = 这一帧画的是第几号 pixmap
    void leftClicked();
    void rightClicked();
    void toggleRequested();            // 短点击（在 dragEnabled 模式下） -> 切换 A/B
    void wheelScrolled(int delta);     // delta >0 向前，<0 向后

protected:
    void paintEvent(QPaintEvent *ev) override;
    void wheelEvent(QWheelEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void mouseMoveEvent(QMouseEvent *ev) override;
    void mouseReleaseEvent(QMouseEvent *ev) override;
    void resizeEvent(QResizeEvent *ev) override;

    // 新增：处理键盘状态（Ctrl / Space）
    void keyPressEvent(QKeyEvent *ev) override;
    void keyReleaseEvent(QKeyEvent *ev) override;

    // 新增：当控件失去焦点时清理按键状态（防止 keyRelease 丢失）
    void focusOutEvent(QFocusEvent *ev) override;

private:
    QPixmap m_pix;
    bool m_pixHasAlpha = false;      // NEW: 当前 pixmap 是否包含 alpha 通道
    quint64 m_serial = 0;            // 换图序号(见 setPixmap 的说明)
    QString m_badgeText;             // 左下角状态角标
    bool    m_badgeBusy = false;
    bool    m_scanning = false;      // 正在扫描(见 setScanningVisual)
    double m_scale = 1.0;
    QPointF m_pan{0,0};
    bool m_dragging = false;
    QPoint m_lastMouse;
    bool m_maybeClick=false;

    /*
     * ===== 渲染缓存 =====
     *
     * paintEvent 原来每次重绘都现场做两件很贵的事(12.6MP 实测):
     *   1) drawPixmap 把 12.6MP 的图平滑缩放到控件大小     ~7ms;
     *
     * 现在把"缩放后的显示图"预先合成到一张和控件一样大的 pixmap,
     * paintEvent 只剩一次 drawPixmap。
     *
     * 失效判定不用手工到处插 invalidation(容易漏),而是在 paint 时比对一个
     * 参数指纹:任何一个参数变了就重新合成。指纹比对本身是常数开销。
     */
    mutable QPixmap m_renderCache;
    mutable bool     m_renderCacheValid = false;
    mutable qint64   m_rcPixKey      = 0;
    mutable double   m_rcScale       = 0.0;
    mutable QPointF  m_rcPan;
    mutable QSize    m_rcSize;
    mutable qreal    m_rcDpr         = 1.0;

    /* 按需(重新)合成渲染缓存;返回 false 表示当前不需要绘制 */
    bool ensureRenderCache() const;
    /* 左下角状态角标(见 .cpp 里的说明) */
    void drawStatusBadge(QPainter &p) const;
    /* "正在扫描"蒙层 + 边框(见 setScanningVisual) */
    void drawScanningOverlay(QPainter &p) const;
    // 新增：键盘状态（按键 press/release 可能丢失 -> 也在 focusOutEvent 清理）
    bool m_ctrlPressed = false;
    bool m_spacePressed = false;

    // 缩放边界
    double m_minScale = 0.01;
    double m_maxScale = 64.0;

    // 右键放大/恢复相关
    bool m_isLargeZoomed = false;
    Viewport m_prevViewport{};             // 保存右键放大前的视口，用于恢复

    void adjustScaleToFit();
    QRectF displayedRect() const;

    // helper for zooming (保证以 pos 为焦点进行缩放，同时做 clamp)
    void zoomToScaleAtPoint(double newScale, const QPointF &pos);

    // 右键专用：大量放大（在 pos 位置）；若已 large-zoom 则恢复 saved viewport
    void performLargeZoomOrRestoreAt(const QPointF &pos);
    void clampViewport();

};
#endif // DEDUPIMAGEDISPLAYWIDGET_H
