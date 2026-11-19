#ifndef LITEWIDGET2GEOMETRY_H
#define LITEWIDGET2GEOMETRY_H

/*
 * litewidget2geometry - LiteWidget2 的轮廓几何(纯函数,不依赖任何控件)。
 *
 * 普通状态:右上角一条凸起的 strip,左侧用 cos^2 曲线过渡到 body 顶边:
 *
 *      +---------------+                    <- strip 顶边(y = 0)
 *      |   handle | x  |\
 *      |          |    | \                  <- cos^2 过渡(宽度按窗口宽度算)
 *      +----------+----+  \
 *   +---------------------+---------------+  <- body 顶边(y = kStripHeight)
 *   |                                     |
 *   |                  body               |
 *   +-------------------------------------+  <- 底边(y = H)
 *
 * 最大化状态:凸起条变成一条横贯整宽的标题栏(无文字,右侧三颗按钮),
 * 轮廓就是一个普通圆角矩形。
 *
 * 比例:过渡区 + 平地 = 窗口宽度 * (1 - 0.618)。过渡区固定为
 * kStripHeight * pi/2(中心切线 45°),窗口变宽时只有平地延长。
 *
 * 边框分三层,全部靠填充差画(不用描边,避免非整数 DPI 下上边框被摊薄):
 *   1. 轮廓本身                    -> 浅灰外框(kStroke 逻辑像素)
 *   2. 内缩 kStroke 的轮廓          -> 条灰内框(kInnerStroke 逻辑像素)
 *   3. body 区域(内缩两圈)         -> body 底色
 *
 * 内缩不是简单地把矩形缩小:kStroke 是"垂直于边"的宽度,曲线段的采样点要
 * 沿法线偏移,否则 45° 的过渡段上那条 1px 边框看上去只有别的边的 70%。
 */

#include <QColor>
#include <QPainterPath>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QVector>

namespace litewidget2 {

/// strip 的高度(也是 body 顶边的 y 坐标)。
constexpr int kStripHeight = 22;
/// 圆角半径;2px 足够,再大就显得软。
constexpr int kBodyRadius = 2;
/// strip / 标题栏右上角的圆角半径。
constexpr int kTabRadius = 2;
/// 最外层浅灰边框的宽度(逻辑像素)。
constexpr int kStroke = 1;
/// 内层条灰边框的宽度(逻辑像素)。1px 在深色底上几乎看不出来,给 4px。
constexpr int kInnerStroke = 4;

// ---- 配色(所有 lite 窗口共用,只在这里改) ----
/// 最外层浅灰边框。185 太浅,和浅色桌面撞色;140 在深浅底上都看得见。
inline const QColor kOutlineColor(140, 146, 154);
/// 凸起条 / 标题栏,同时也是内框色。
inline const QColor kStripColor(0x26, 0x26, 0x2c, 250);
/// 凸起条 / 标题栏上面那层"玻璃":半透明,让标题栏透出后面的东西;边框和内框
/// 仍然用不透明的 kStripColor,所以轮廓的锐度不变。
inline const QColor kStripGlassColor(0x26, 0x26, 0x2c, 150);
/// body 底色。
inline const QColor kBodyColor(0x1b, 0x1b, 0x1f, 250);
/// 内容面板相对窗口边缘让出来的距离(外框 + 内框)。
inline constexpr int kContentInset = kStroke + kInnerStroke;
/// 拖动把手的宽度(关闭按钮左边那一段,普通状态下只占 strip 的一格)。
constexpr int kHandleWidth = 36;
/// 每个窗口按钮的宽度(关闭 / 还原 / 最小化共用)。
constexpr int kCloseWidth = 36;
/// 过渡区 + 平地 占窗口宽度的比例(1 - 0.618)。
constexpr qreal kTabWidthRatio = 0.382;
/// cos^2 过渡的宽度:取"中心点切线斜率 = 1"(45°)的比例。
/// y = h*cos^2(pi t/2) 在 t=0.5 的斜率是 -h*pi/(2w),令其为 1 得 w = h*pi/2。
constexpr int kSlopeWidth = 35;   // = round(22 * pi / 2)

/// 曲线段的边框内缩增益。1px 的斜线会被抗锯齿摊到两个像素上,看起来比直边
/// 淡一半;中段加粗到 1.75 倍,两端按 sin^2 渐回 1,和直边严丝合缝。
constexpr qreal kSlopeInsetGain = 1.75;

/// 平地部分的最小宽度(把手 + 关闭)。
int flatWidth();
/// 余弦平方过渡的水平跨度;固定 = kSlopeWidth(中心切线 45°)。
int slopeWidth(int windowWidth);
/// 凸起部分总宽(过渡 + 平地),总宽按窗口宽度 0.382 走。
int tabWidth(int windowWidth);

/// 凸起条里"平地"那一段(把手 + 关闭),贴右边缘;最大化时返回整条标题栏。
QRect stripRect(const QRect& window, bool maximized = false);
/// 拖动区:普通状态从过渡区左端到关闭按钮左边;最大化时是标题栏左侧空区。
QRect handleRect(const QRect& window, bool maximized = false);
/// 关闭按钮(两种状态下都在最右一格)。
QRect closeRect(const QRect& window, bool maximized = false);
/// 还原按钮(只有最大化时才显示),在关闭按钮左边一格。
QRect toggleRect(const QRect& window, bool maximized = false);
/// 最小化按钮(只有最大化时才显示),在还原按钮左边一格。
QRect minimizeRect(const QRect& window, bool maximized = false);
/// body 矩形(y 从 kStripHeight 起)。
QRect bodyRect(const QRect& window);

/**
 * @brief cos^2 过渡的采样点,从左到右排列。
 *
 * t = (x - x0) / sw,y(x) = h * cos^2(pi * t / 2):
 * 最左点 y = kStripHeight(接 body 顶边),最右点 y = 0(接 strip 顶边),
 * 两端切线水平。样本数至少 24(LW-03),默认 32。
 *
 * inset > 0 时每个点沿"指向形状内部"的法线偏移 inset,用来做等宽的边框。
 */
QVector<QPointF> slopePoints(const QRectF& window, int samples = 32,
                             qreal inset = 0.0);

/**
 * @brief 整个窗口的轮廓(普通 = 凸起条形状,最大化 = 圆角矩形)。
 *
 * @param inset 向内收缩的距离,按"垂直于边"计算:直边直接减 inset,cos^2
 *              过渡段沿法线偏移,所以过渡段的边框和直边一样宽。
 */
QPainterPath outlinePath(const QRectF& window, bool maximized = false,
                         qreal inset = 0.0);

/**
 * @brief body 的填充形状(底部两角圆角,顶部平直)。
 *
 * @param inset     左右下三边内缩的距离。
 * @param insetTop  顶边是否也内缩。普通状态要(顶边就是窗口外轮廓,需要
 *                  外框 + 内框);最大化时标题栏在窗口内部,顶边不内缩,
 *                  否则标题栏和 body 之间会多出一条灰线。
 */
QPainterPath bodyPath(const QRectF& window, int inset, bool insetTop = true);

/**
 * @brief 内容面板的裁剪形状(左上/左下/右下圆角,右上直角)。
 *
 * 面板自己会按旧配色画深色底(parent 的 QSS),不裁的话会把 body 的圆角
 * 盖成方的。
 */
QPainterPath contentPath(const QRectF& contentRect, qreal radius = kBodyRadius);

} // namespace litewidget2

#endif // LITEWIDGET2GEOMETRY_H
