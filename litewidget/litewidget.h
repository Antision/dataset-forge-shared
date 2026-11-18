#ifndef LITEWIDGET_H
#define LITEWIDGET_H

/*
 * LiteWidget - 最简无边框顶层窗口:深灰 1px 边框 + 内容。
 *
 * 没有标题栏、没有圆角、没有关闭按钮、没有拖动把手 —— 显示 / 隐藏 / 关闭全由
 * 管理者决定(hide() 即视为关闭,它自己不会产生"关闭"语义)。它存在的理由是
 * 二级按钮的菜单(SC-02)、重命名对话框(DS-05)、溯源浮窗(MERGE-10)这类
 * "贴在别的东西旁边的一小块内容",不需要 LiteWidget2 那条凸起条。
 *
 * 约定:
 *   · 构造时不 show();尺寸由内容决定,管理者可以显式 resize()/move();
 *   · setContentWidget() 接管所有权,之后再设一次会删掉上一个;
 *   · 默认 Qt::Tool:不出现在任务栏,也不会被主窗口盖住;
 *   · 需要"鼠标穿透的预览态"时用 setNoActivate(true) 或直接设
 *     WA_TransparentForMouseEvents(菜单自己管,这里只提供开关)。
 */

#include <QWidget>

class QVBoxLayout;

class LiteWidget : public QWidget
{
    Q_OBJECT
public:
    explicit LiteWidget(QWidget* parent = nullptr);
    ~LiteWidget() override;

    /// 接管 widget 的所有权并把它放进 1px 边框里面。
    void setContentWidget(QWidget* widget);
    QWidget* contentWidget() const { return m_content; }
    /// 内容区的布局;内容 widget 还没有布局时自动建一个(无边距)。
    QVBoxLayout* contentLayout() const;

    /// true = 显示时不激活、不接受焦点(菜单预览态用)。
    void setNoActivate(bool on);
    bool noActivate() const { return m_noActivate; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QWidget* m_content = nullptr;
    QVBoxLayout* m_outerLayout = nullptr;
    bool m_noActivate = false;
};

#endif // LITEWIDGET_H
