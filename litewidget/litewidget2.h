#ifndef LITEWIDGET2_H
#define LITEWIDGET2_H

/*
 * LiteWidget2 - 带右上角凸起条的圆角无边框窗口(common)。
 *
 * 形状见 litewidget2geometry.h:右上角一条 strip(把手 + 关闭),左侧用
 * cos^2 曲线过渡到 body 顶边。过渡区 + 平地按窗口宽度的 0.382 走,窗口越宽
 * 这一段越长。圆角只有 2px。
 *
 * 边框三层:最外浅灰 1px → 内圈条灰 1px → body 底色。全部用填充差画,
 * 不用描边 —— 描边在 150%/200% 缩放下会被摊到两个设备像素上,看上去比
 * 别的边细。
 *
 * 交互(LW-04):
 *   · 左键按住把手(普通状态从过渡区左端到关闭按钮左侧)拖动窗口;
 *     拖到屏幕顶部 +8px 内自动最大化,继续下拖 24px 撕下来恢复原尺寸;
 *   · 右键在把手上一律最小化(按下拖动时窗口跟着走,松开即最小化);
 *   · 最大化后凸起条变成整宽标题栏:左侧可拖,右侧依次是最小化 / 还原 /
 *     关闭三颗按钮(没有标题文字);
 *   · 边缘 6px 内可缩放:左右下三边、底部两角,以及 body 的顶边(横带
 *     y≈kStripHeight,strip 自己不参与);最小 320x160。**不用 WS_THICKFRAME**,
 *     自己做缩放 —— 系统的边框会让跨屏 DPI 切换反复扣掉一圈,窗口越拖越小;
 *   · 最大化状态由自己维护(自绘窗口不参与系统 Aero Snap / IsZoomed)。
 *
 * 关闭:关闭按钮 / Esc → hide() 并 emit closed();deleteLater 由管理者决定。
 */

#include <QRect>
#include <QWidget>

class QVBoxLayout;
class WindowGlyphButton;

class LiteWidget2 : public QWidget
{
    Q_OBJECT
public:
    explicit LiteWidget2(QWidget* parent = nullptr);
    ~LiteWidget2() override;

    /// 接管 widget 的所有权,放进 body 内部(让出内外两层边框与顶部条)。
    void setContentWidget(QWidget* widget);
    QWidget* contentWidget() const { return m_content; }
    /// 内容区的布局;没有时自动建一个(无边距、无间距)。
    QVBoxLayout* contentLayout() const;

    WindowGlyphButton* closeButton() const { return m_closeButton; }
    WindowGlyphButton* toggleButton() const { return m_toggleButton; }
    WindowGlyphButton* minimizeButton() const { return m_minimizeButton; }

    /// 测试用:凸起条、把手、三颗按钮、body 的窗口内矩形。
    QRect stripRect() const;
    QRect handleRect() const;
    QRect closeRect() const;
    QRect toggleRect() const;
    QRect minimizeRect() const;
    QRect bodyRect() const;

    /// 最大化状态由自己维护,不读系统状态。
    bool isMaximizedFlag() const { return m_maximized; }
    void setMaximizedFlag(bool on);
    /// 最大化时记下来的还原矩形(未最大化时为空)。
    QRect restoreGeometry() const { return m_restoreGeometry; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    /**
     * @brief 运行时记住这个窗口的尺寸/位置(只在本次进程里,不落盘)。
     *
     * key 默认取类名;同一个类有几种完全不同的内容时(比如 colorsWidget 的
     * 颜色窗 / 服饰窗)由调用方给不同的 key。
     */
    void setGeometryKey(const QString& key);
    QString geometryKey() const;
    /// 有没有记住的几何(没有就按 sizeHint 开,有就照上次的恢复)。
    bool hasStoredGeometry() const;

signals:
    void closed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    enum ResizeEdge {
        EdgeNone = 0,
        EdgeLeft = 1,
        EdgeRight = 2,
        EdgeTop = 4,
        EdgeBottom = 8
    };
    int resizeEdgesAt(const QPoint& pos) const;
    void updateResizeCursor(int edges);
    void beginResize(int edges, const QPoint& global);
    void performResize(const QPoint& global);

    void applyMaximized(bool on, const QRect& available);
    void restoreFromMaximized(const QPoint& globalCursor);
    void layoutChildren();
    void updateChrome();
    void requestClose();

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
    Qt::MouseButton m_dragButton = Qt::NoButton;
    QPoint m_dragOffset;      // 按下时:光标 - 窗口左上角
    QPoint m_pressGlobal;     // 按下时的全局坐标(判断"撕下"的距离)
    QSize m_dragSize;         // 按下时的尺寸;拖动期间每次都重新 assert
    bool m_tornOff = false;

    // 跨屏 DPI 保护:Windows/Qt 处理 WM_DPICHANGED 时会按"带边框几何"换算窗口
    // 矩形,来回跨屏一次就少一圈,最后缩到最小尺寸。这里记住"用户设定的逻辑
    // 尺寸",DPI 一变就把它复原回去。
    QSize m_stableSize;
    QString m_geometryKey;
    qreal m_stableDpr = 0.0;
    bool m_sizeLocked = false;
};

#endif // LITEWIDGET2_H
