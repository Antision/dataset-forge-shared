#ifndef PAIRCONTEXTMENU_H
#define PAIRCONTEXTMENU_H

/*
 * 图片对右键菜单的**排版工具**(纯排版,不含业务判断)。
 *
 * 只做三件事:
 *   1) 用普通 QMenu(原生 action),这样滚动、键盘、Esc、屏幕边缘翻转都是系统行为;
 *   2) 段与段之间画一条分隔线(sections 里每个非空段之前加 QMenu::separator),
 *      把 Remove A / Remove B / Remove Both / Jump / 其它分组;
 *   3) 统一套一份和主界面一致的 QSS(颜色取自 imagededuplicatewindow.ui 里的 QMenu 段)。
 *
 * 为什么不用 QWidgetAction 自绘面板:
 *   · 自绘按钮只支持单行纯文本,QPushButton 又不吃 HTML,想写"原因"只好塞 <div>,
 *     结果原样显示成文本;
 *   · 也没有原生菜单的悬停/键盘/贴边翻转行为。
 * 现在"为什么不能点"直接拼在条目文字后面的括号里,一行说完。
 */

#include <QPoint>
#include <QString>
#include <QVector>

#include <functional>

class QWidget;

namespace pairmenu {

struct Entry {
    QString text;
    bool    enabled = true;
    std::function<void()> trigger;   // 点中之后执行(菜单关闭后才执行)
};

/* 一段 = 菜单里连续的一组条目;段与段之间画分隔线 */
using Section = QVector<Entry>;

/*
 * 在 globalPos 弹出菜单。
 * anchor 只用来定位/继承父窗口(可以为空);sections 全空时什么都不弹。
 * 触发器在 menu.exec() 返回之后才执行 —— 弹菜单是嵌套事件循环,
 * 在它里面搬文件/重建列表等于在自己脚下抽地板。
 */
void exec(QWidget *anchor, const QPoint &globalPos, const QVector<Section> &sections);

} // namespace pairmenu

#endif // PAIRCONTEXTMENU_H
