#ifndef SVGICON_H
#define SVGICON_H

/**
 * @file svgicon.h
 * @brief Lucide(ISC 许可)线条 SVG 图标的运行时着色渲染。
 *
 * 资源是 24x24 viewBox 的线条图标(fill:none,stroke:currentColor)。
 * QtSvg 不支持运行时改 currentColor,所以渲染前把源文本里的 "currentColor"
 * 换成目标色——文件只有几百字节,读资源的开销可以忽略;渲染结果缓存在
 * QPixmapCache 里,同一图标反复构建菜单时不会重复渲染。
 *
 * glow > 0 时在图标底下叠几圈低透明度的重影,得到柔和外发光
 * (快速添加窗的 对勾/加号/关闭 三颗按钮用)。
 *
 * 需要 `QT += svg`(common.pri 已经带上,消费方不用重复加)。
 */

#include <QColor>
#include <QFile>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QPixmapCache>
#include <QRectF>
#include <QString>
#include <QSvgRenderer>

#include <cmath>

namespace svgicon {

/// 图标在资源里的路径(":/icons/check.svg")到 QColor 的着色渲染。
/// `glow` 是每圈重影的半径(px);0 表示不发光。结果按参数缓存。
/// 发光时画布按 glow 放大留边(否则光晕会被裁掉),图标本身仍是 `size`。
inline QPixmap tinted(const QString& path,const QColor& color,
                      const QSize& size=QSize(16,16),qreal glow=0.0)
{
    const QString key=QStringLiteral("svgicon|%1|%2|%3x%4|%5")
        .arg(path,color.name(QColor::HexArgb)).arg(size.width()).arg(size.height()).arg(glow);
    QPixmap cached;
    if(QPixmapCache::find(key,&cached))
        return cached;

    QFile file(path);
    QString source;
    if(file.open(QIODevice::ReadOnly))
        source=QString::fromUtf8(file.readAll());
    if(!source.isEmpty())
        source.replace(QLatin1String("currentColor"),color.name(QColor::HexRgb));

    // 光晕最多画到 3*glow,画布四周留出同样的边。
    const int margin = glow>0 ? int(std::ceil(glow*3.0)) : 0;
    QSvgRenderer renderer(source.toUtf8());
    QPixmap pixmap(size+QSize(margin*2,margin*2));
    pixmap.fill(Qt::transparent);
    if(renderer.isValid()){
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing,true);
        const QRectF area(margin,margin,size.width(),size.height());
        if(glow>0){
            // 三圈 × 12 方位的低透明度重影:叠加后是柔和的光晕,比
            // QGraphicsBlurEffect(需要 scene)轻得多。0.12 而不是 0.08:
            // 常态下也要看得出来(只在 hover 才亮的那版被当成"没有发光")。
            for(int ring=1;ring<=3;++ring){
                const qreal radius=ring*glow;
                for(int step=0;step<12;++step){
                    const qreal angle=2*M_PI*step/12.0;
                    painter.setOpacity(0.09);
                    renderer.render(&painter,
                        area.translated(radius*std::cos(angle),radius*std::sin(angle)));
                }
            }
            painter.setOpacity(1.0);
        }
        renderer.render(&painter,area);
    }
    QPixmapCache::insert(key,pixmap);
    return pixmap;
}

/// 同 tinted(),包成 QIcon(菜单动作用)。
inline QIcon icon(const QString& path,const QColor& color,
                  const QSize& size=QSize(16,16),qreal glow=0.0)
{
    return QIcon(tinted(path,color,size,glow));
}

/// 菜单图标的默认灰白色。
inline QIcon menuIcon(const QString& path,const QSize& size=QSize(15,15))
{
    return icon(path,QColor(200,200,210),size);
}

} // namespace svgicon

#endif // SVGICON_H
