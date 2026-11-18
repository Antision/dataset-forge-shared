#ifndef WINDOWGLYPHBUTTON_H
#define WINDOWGLYPHBUTTON_H

/*
 * WindowGlyphButton - 主窗口和 LiteWidget2 共用的窗口按钮(关闭 / 最大化 /
 * 还原 / 最小化)。
 *
 * 不再用 PNG:三种状态(常态 / 悬停 / 按下)全部在 paintEvent 里画,1.5px
 * 线条,10x10 图形居中。这样在 150% / 200% 缩放下不会像位图那样发虚,也不
 * 用在 qrc 里给每个按钮准备两份图。
 *
 * 尺寸:默认 sizeHint 30x24;主窗口把它放进菜单栏、LiteWidget2 放进 strip,
 * 两边都只需要 setFixedWidth(30) / setFixedHeight(条高)。
 */

#include <QIcon>
#include <QPushButton>

class WindowGlyphButton : public QPushButton
{
    Q_OBJECT
public:
    enum class Glyph {
        Close,
        Maximize,
        Restore,
        Minimize
    };
    Q_ENUM(Glyph)

    explicit WindowGlyphButton(Glyph glyph, QWidget* parent = nullptr);
    explicit WindowGlyphButton(QWidget* parent = nullptr);

    void setGlyph(Glyph glyph);
    Glyph glyph() const { return m_glyph; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    Glyph m_glyph = Glyph::Close;
};

#endif // WINDOWGLYPHBUTTON_H
