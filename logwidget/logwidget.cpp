#include "logwidget.h"
#include "ui_logwidget.h"

#include "audio.h"

#include <QDateTime>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QStyle>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QThread>

namespace {

// 各日志级别的文字颜色(和 designer 里按钮的 warning/error 配色保持一致)
QColor levelColor(LogWidget::Level level)
{
    switch (level) {
    case LogWidget::Level::Detail:  return QColor(0x7a, 0x7a, 0x7a);
    case LogWidget::Level::Good:    return QColor(0x7b, 0xd8, 0x8f);
    case LogWidget::Level::Warning: return QColor(0xeb, 0xc7, 0x5e);
    case LogWidget::Level::Error:   return QColor(0xff, 0x77, 0x77);
    case LogWidget::Level::Info:
    default:                          return QColor(0xc8, 0xc8, 0xc8);
    }
}

} // namespace

LogWidget::LogWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::LogWidget)
{
    ui->setupUi(this);

    // 日志只读,但保留选中/复制的能力
    ui->textEdit->setReadOnly(true);
    ui->textEdit->setUndoRedoEnabled(false);
    ui->textEdit->setMaximumBlockCount(20000);   // 防止长时间运行把内存吃满

    setOkState(OkState::Normal);
}

LogWidget::~LogWidget()
{
    delete ui;
}

void LogWidget::setTitle(const QString &title)
{
    setWindowTitle(title);
}

void LogWidget::append(const QString &text, Level level)
{
    QTextCharFormat fmt;
    fmt.setForeground(levelColor(level));

    // 行首的时间戳用更暗的灰色,方便一眼区分正文
    QTextCharFormat stampFmt;
    stampFmt.setForeground(QColor(0x60, 0x60, 0x60));

    QTextCursor cursor(ui->textEdit->document());
    cursor.movePosition(QTextCursor::End);

    cursor.insertText(QStringLiteral("[") + QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))
                          + QStringLiteral("] "),
                      stampFmt);
    cursor.insertText(text, fmt);
    cursor.insertBlock();

    // 手动滚动到底部(QPlainTextEdit 不会自动跟随)
    QScrollBar *bar = ui->textEdit->verticalScrollBar();
    bar->setValue(bar->maximum());

    // Warning / Error 不该被错过:OK 按钮亮起、响提示音、把窗口抢救到最前。
    // 留着灯不放是因为"有一行出错了"这件事在滚屏之后光看正文会漏掉,用户点一下
    // OK 才算看到(setOkState 有属性比较,重复设置不会重新 polish)。
    if (level == Level::Warning || level == Level::Error) {
        setOkState(level == Level::Error ? OkState::Error : OkState::Warning);
        notifyImportant(level);
    }
}

void LogWidget::notifyImportant(Level level)
{
    /*
     * append 有一堆调用方,其中一部分在日志 sink 里跑(工作线程):
     * 弹窗和放声音都只能在主线程做,不在主线程就把自己投递回去。
     */
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, level] { notifyImportant(level); },
                                  Qt::QueuedConnection);
        return;
    }

    /*
     * 先把窗口弄回屏幕上。
     *
     * 用户关掉日志窗口只是 close()(= hide),以前这里只有 raise(),对一个隐藏窗口
     * 等于什么都没做 —— 表现就是"关掉之后再报警告就不弹了,只剩声音"。
     * 现在:最小化就还原,被关掉就重新 show 出来。
     *
     * 注意不要用 activateWindow():那会抢走用户正在打字的焦点。
     */
    if (isMinimized())
        showNormal();
    else if (!isVisible())
        show();
    raise();

    /*
     * 声音:按级别各用一条,各自节流 —— 批量失败时 Warning 3 秒最多一次,
     * Error 1.5 秒最多一次,但 Error 不会因为刚刚响过 Warning 就被吞掉。
     */
    const bool isError = (level == Level::Error);
    qint64 &lastMs = isError ? m_lastErrorMs : m_lastWarningMs;
    const qint64 minGapMs = isError ? 1500 : 3000;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - lastMs < minGapMs) return;
    lastMs = now;

    appaudio::playSound(isError ? appaudio::Sound::Error : appaudio::Sound::Warning);
}

void LogWidget::clearLog()
{
    ui->textEdit->clear();
}

void LogWidget::setOkState(OkState state)
{
    // error 是最高优先级,进去之后不会被后来的 warning 顶掉
    if (m_okState == OkState::Error && state == OkState::Warning)
        return;

    m_okState = state;
    switch (state) {
    case OkState::Warning: applyOkClass(QStringLiteral("warning")); break;
    case OkState::Error:   applyOkClass(QStringLiteral("error"));   break;
    case OkState::Normal:
    default:               applyOkClass(QString());                 break;
    }
}

void LogWidget::applyOkClass(const QString &cls)
{
    if (!ui->btn_ok) return;

    const QVariant want = cls.isEmpty() ? QVariant() : QVariant(cls);
    if (ui->btn_ok->property("class") == want) return;

    ui->btn_ok->setProperty("class", want);
    if (QStyle *st = ui->btn_ok->style()) {
        st->unpolish(ui->btn_ok);
        st->polish(ui->btn_ok);
    }
    ui->btn_ok->update();
}

void LogWidget::on_btn_clear_clicked()
{
    clearLog();
}

void LogWidget::on_btn_ok_clicked()
{
    // 只是确认"我看到了",把按钮颜色恢复成正常态,窗口不关闭
    setOkState(OkState::Normal);
}
