#ifndef COMMON_LOG_H
#define COMMON_LOG_H

/**
 * @file log.h
 * @brief The application log every module writes to.
 *
 * It lived in tag_quick_editor's func.h, which made it the one thing the shared
 * tag layer could not use after it moved into common. Both programs now get the
 * same entry point: a line goes to qDebug() and, if a target is installed, to
 * the GUI log window on the GUI thread.
 */

#include <QString>
#include <functional>

/**
 * @brief How important a log line is.
 *
 * The log window colours the line by it, keeps a single "something went wrong"
 * light, and raises itself with a sound for Warning/Error. Most of the program's
 * lines are plain progress information, so teLog() stays the default; only the
 * lines a user would actually want to be interrupted for get tewarn()/teerror().
 */
enum class teLogLevel {
    Info,
    Detail,
    Good,
    Warning,
    Error
};

/// Registers the receiver of application log lines. The target is always
/// invoked on the GUI thread, so it may touch widgets. Pass an empty
/// std::function to detach it (e.g. while the log window is being destroyed).
void teSetLogTarget(std::function<void(const QString&, teLogLevel)> target);
/// Appends a line to the application log. Safe to call from any thread.
void teLog(const QString& message);
/// Same, with a level (see teLogLevel).
void teLog(const QString& message, teLogLevel level);

/// Convenience wrappers kept so existing call sites stay readable.
#define telog(a) teLog(QString(a))
/// A line the user should notice: failed to write a caption, a download broke...
#define tewarn(a) teLog(QString(a), teLogLevel::Warning)
#define teerror(a) teLog(QString(a), teLogLevel::Error)

#endif // COMMON_LOG_H
