#include "animations.h"

#include <QCursor>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QPropertyAnimation>
#include <QTimer>
#include <QWidget>

void createFloatingLabel(QWidget*parent,const QString &text)
{
    Q_UNUSED(parent);
    /*
     * 顶层穿透窗口,不再是 parent 的子控件。
     *
     * 以前 label 是子控件 + WA_TransparentForMouseEvents:那只对普通鼠标
     * 事件有效 —— 拖放目标查找(QWidget::childAt)照样会命中它,而它对
     * Drag* 一律 ignore,恰好它又生成在光标正下方,一次 ignore 就会把
     * 整条拖放链粘住(同 DatasetWidget 那次的根因)。
     * 改成独立顶层窗口 + Qt::WindowTransparentForInput:
     * Windows 上映射为 WS_EX_LAYERED|WS_EX_TRANSPARENT,系统级命中测试
     * 直接跳过它 —— 鼠标、拖放统统穿透,也不抢焦点(WA_ShowWithoutActivating)。
     */
    QLabel *label = new QLabel(nullptr);
    label->setText(text);
    label->setStyleSheet(
        "background-color: rgba(0, 160, 160, 60);"
        "color: white;"
        "padding: 2px 2px;"
        "border-radius: 4px;"
        );
    label->setWindowFlags(Qt::FramelessWindowHint
                          | Qt::WindowStaysOnTopHint
                          | Qt::Tool
                          | Qt::WindowTransparentForInput);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    label->setAttribute(Qt::WA_ShowWithoutActivating);
    label->setAttribute(Qt::WA_TranslucentBackground);
    /*
     * 文本很长时必须手动 adjustSize:
     * 下面要用 label 的尺寸定位(还差 show() 的自动布局一步),
     * 不调的话 long text 时拿到的还是默认大小,标签不会被文本撑开。
     */
    label->adjustSize();
    // 顶层窗口直接用全局坐标:光标位置往左上偏 1/4 尺寸,别盖住落点
    label->move(QCursor::pos() - QPoint{label->width()/4, label->height()/4});
    label->show();

    // 透明效果
    auto *opacityEffect = new QGraphicsOpacityEffect(label);
    label->setGraphicsEffect(opacityEffect);
    opacityEffect->setOpacity(1.0);

    // 1秒后启动动画
    QTimer::singleShot(1000, label, [=]() {
        // 位移动画（向上漂浮）
        QPropertyAnimation *posAnim =
            new QPropertyAnimation(label, "pos");
        posAnim->setDuration(1000);
        posAnim->setStartValue(label->pos());
        posAnim->setEndValue(label->pos() + QPoint(0, -40));
        posAnim->setEasingCurve(QEasingCurve::OutCubic);

        // 透明度动画
        QPropertyAnimation *opacityAnim =
            new QPropertyAnimation(opacityEffect, "opacity");
        opacityAnim->setDuration(1000);
        opacityAnim->setStartValue(1.0);
        opacityAnim->setEndValue(0.0);

        posAnim->start(QAbstractAnimation::DeleteWhenStopped);
        opacityAnim->start(QAbstractAnimation::DeleteWhenStopped);

        // 动画结束后销毁控件
        QObject::connect(opacityAnim, &QPropertyAnimation::finished,
                         label, &QLabel::deleteLater);
    });
}
