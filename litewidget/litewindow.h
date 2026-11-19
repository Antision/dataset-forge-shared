#ifndef LiteWindow_H
#define LiteWindow_H

/*
 * LiteWindow - 普通窗口,长得就是 LiteWidget2 最大化之后的样子。
 *
 *   +------------------------------------+
 *   | title            [_] [口] [x]      |  <- 整宽标题栏(22px)
 *   +------------------------------------+
 *   |                                    |
 *   |              content               |
 *   |                                    |
 *   +------------------------------------+
 *
 * 和 LiteWidget2 的分工:
 *   · LiteWidget2 = 凸起条 + 不规则轮廓,只给"贴着别的东西弹出的一小块"
 *     用(控件弹层、wiki 渲染窗)。它没有标题文字,拖动区是一条不规则的把手;
 *   · LiteWindow  = 真正的窗体:整宽标题栏(带标题文字)、三颗标准按钮、
 *     四边都能拉,用于大图预览、筛选、日志这类"用户会一直摆在那儿"的窗口。
 *
 * 视觉沿用 LiteWidget2 的那套:外框 1px(浅灰)+ 内框 4px(条灰)+ body,
 * 圆角 2px;最大化时圆角归零、铺满工作区。
 *
 * 交互:标题栏拖动 / 双击最大化 / 拖到屏幕顶 +8px 最大化 / 最大化时下拖 24px
 * 还原 / 四边 6px 缩放(最小 320x160)/ Esc 关闭 / 关闭按钮 hide() + closed()。
 *
 * 几何记忆:和 LiteWidget2 一样,进程内按 `geometryKey()`(默认类名)记住
 * 尺寸和位置,show 时恢复(夹到当前屏幕),关掉重开不会复原。不落盘。
 */

#include "litewidget.h"

#include <QSize>
#include <QString>

class QVBoxLayout;
class WindowGlyphButton;

class LiteWindow : public LiteWidget
{
    Q_OBJECT
public:
    explicit LiteWindow(QWidget* parent = nullptr);
    ~LiteWindow() override;

    void setContentWidget(QWidget* widget);
    QWidget* contentWidget() const { return m_content; }
    QVBoxLayout* contentLayout() const;

    QRect titleBarRect() const;
    QRect closeRect() const;
    QRect toggleRect() const;
    QRect minimizeRect() const;

    /// 最大化状态由自己维护,不读系统状态。
    bool isMaximizedFlag() const { return m_maximized; }
    void setMaximizedFlag(bool on);
    /// 上一次还原时的矩形(没最大化过就是空的)。
    QRect restoreGeometry() const { return m_restoreGeometry; }

    void setGeometryKey(const QString& key);
    QString geometryKey() const;
    bool hasStoredGeometry() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void closed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    enum ResizeEdge {
        EdgeNone = 0,
        EdgeLeft = 1,
        EdgeRight = 2,
        EdgeTop = 4,
        EdgeBottom = 8
    };

    void layoutChildren();
    void updateChrome();
    void requestClose();
    void applyMaximized(bool on, const QRect& available);
    void restoreFromMaximized(const QPoint& globalCursor);

    /// 轮廓(最大化时是直角矩形,否则是 2px 圆角),inset 按垂直距离内缩。
    QPainterPath silhouette(qreal inset) const;

    int resizeEdgesAt(const QPoint& pos) const;
    void updateResizeCursor(int edges);
    void beginResize(int edges, const QPoint& global);
    void performResize(const QPoint& global);

    QWidget* m_content = nullptr;
    WindowGlyphButton* m_closeButton = nullptr;
    WindowGlyphButton* m_toggleButton = nullptr;
    WindowGlyphButton* m_minimizeButton = nullptr;

    bool m_maximized = false;
    QRect m_restoreGeometry;

    bool m_resizing = false;
    int m_resizeEdges = EdgeNone;
    QRect m_resizeStartGeometry;
    QPoint m_resizeStartGlobal;

    bool m_dragging = false;
    QPoint m_dragOffset;      // 按下时:光标 - 窗口左上角
    QPoint m_pressGlobal;     // 按下时的全局坐标(判断"撕下"的距离)
    QSize m_dragSize;         // 按下时的尺寸;拖动期间每帧重新 assert
    bool m_tornOff = false;

    // 跨屏 DPI 保护(和 LiteWidget2 同一套办法)。
    QSize m_stableSize;
    qreal m_stableDpr = 0.0;
    bool m_sizeLocked = false;

    QString m_geometryKey;
};

#endif // LiteWindow_H
