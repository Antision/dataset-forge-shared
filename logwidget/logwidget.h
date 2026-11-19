#ifndef LOGWIDGET_H
#define LOGWIDGET_H

/*
 * LogWidget - 两个项目共用的日志窗口。
 *
 * 来自 gallery_info 的 LogWidget(下载/去重/clear file 都指着它),搬到 common 后
 * tag_quick_editor 也用它,替代原来那个只有 QPlainTextEdit + clear 按钮的
 * LogWindow。原来那份没有任何分级、没有提示音、关掉之后再也不弹,用户看不到
 * 出错的日志。
 *
 * 窗口本身是 LiteWindow(整宽标题栏 + 最小化/最大化/关闭三颗按钮,
 * 和日志窗口"一直摆在那儿"的定位匹配);内容和按钮都在标题栏下面。
 *
 * 它的定位永远是"某个窗口的附属窗口":构造时给 owner 当 parent —— 结果是一个
 * 独立窗口,但
 *   · 永远压在 owner 上面,不会被主界面或别的程序盖住;
 *   · owner 隐藏/销毁时它跟着隐藏/销毁(不需要谁去 detach);
 *   · 收到 Warning/Error 时自己 show() 回来(用户手动关掉过的也会回来)。
 *
 * 配色走 QPlainTextEdit 的 QTextCharFormat,按钮状态走 designer 里写好的
 * QPushButton[class="warning"] / QPushButton[class="error"] 属性选择器。
 * 一旦进入 error 就不会再自发退回 warning —— 错误比"警告"更值得注意。
 * 按 OK 清除 class 属性回到正常态(可以反复点,不需要 disable)。
 *
 * 提示音按级别分开(见 appaudio::Sound):
 *   Warning -> universfield-warning-notification
 *   Error   -> universfield-error-notification
 * 两种声音各自节流,Error 不会因为刚响过 Warning 就被吞掉。
 *
 * 消费方需要在 .pro 里 `QT += multimedia` 和 `include(common/common.pri)`。
 */

#include "litewindow.h"

namespace Ui {
class LogWidget;
}

class LogWidget : public LiteWindow
{
    Q_OBJECT

public:
    /// 日志级别:决定文字颜色,以及要不要响提示音、把窗口顶到最前。
    enum class Level {
        Info,     // 普通信息
        Detail,   // 次要信息(跳过已有文件、重试之类)
        Good,     // 成功
        Warning,  // 可以做但需要注意(服务器上没有该文件、写盘失败...)
        Error     // 其它失败
    };

    /// OK 按钮的状态色。
    enum class OkState {
        Normal,
        Warning,
        Error
    };

    explicit LogWidget(QWidget *parent = nullptr);
    ~LogWidget();

    /// 追加一行彩色日志(自动带时间戳与换行)。任何线程都可以调。
    void append(const QString &text, Level level = Level::Info);
    void clearLog();

    /// 设置 OK 按钮的样式状态;Warning 不会覆盖已经处于的 Error。
    void setOkState(OkState state);
    OkState okState() const { return m_okState; }

    /// 标题栏文字(默认 "Log");gi 的下载日志窗口用 "Download Log"。
    void setTitle(const QString& title);

private slots:
    void on_btn_clear_clicked();
    void on_btn_ok_clicked();

private:
    /// 改动态属性后必须 unpolish/polish 一次,样式才会重算。
    void applyOkClass(const QString &cls);
    /*
     * Warning / Error 的"叫醒"流程:响提示音 + 把窗口弄回屏幕上 + 顶到最前。
     * append 可能被工作线程调(各种日志 sink),所以真正干活的那份必须在
     * 主线程跑 —— 本体就是这个函数,跨线程时用队列连接把自己投递回去。
     */
    Q_INVOKABLE void notifyImportant(Level level);

    Ui::LogWidget *ui;
    OkState m_okState = OkState::Normal;
    /*
     * 节流用的时间戳(毫秒),两种级别各记一份。
     * 刷屏时最多 Warning 3 秒响一次 / Error 1.5 秒响一次,不然提示音会连成一片;
     * 窗口的"显示 + 置前"不受节流限制 —— 那不需要代价,而漏看一条警告代价很大。
     */
    qint64 m_lastWarningMs = 0;
    qint64 m_lastErrorMs = 0;
};

#endif // LOGWIDGET_H
