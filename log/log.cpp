#include "log.h"

#include <QCoreApplication>
#include <QDebug>

#include <mutex>

namespace {
std::mutex g_logTargetMutex;
std::function<void(const QString&, teLogLevel)> g_logTarget;
}

void teSetLogTarget(std::function<void(const QString&, teLogLevel)> target)
{
    std::lock_guard<std::mutex> lg(g_logTargetMutex);
    g_logTarget = std::move(target);
}

void teLog(const QString& message, teLogLevel level)
{
    qDebug().noquote() << message;

    QCoreApplication* app = QCoreApplication::instance();
    if (!app)
        return;

    // teLog() may be called from the image-loader worker threads, so the widget
    // update has to be marshalled onto the GUI thread.
    QMetaObject::invokeMethod(app, [message, level] {
        std::function<void(const QString&, teLogLevel)> target;
        {
            std::lock_guard<std::mutex> lg(g_logTargetMutex);
            target = g_logTarget;
        }
        if (target)
            target(message, level);
    }, Qt::QueuedConnection);
}

void teLog(const QString& message)
{
    teLog(message, teLogLevel::Info);
}
