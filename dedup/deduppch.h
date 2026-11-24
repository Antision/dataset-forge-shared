#ifndef DEDUP_DEDUPPCH_H
#define DEDUP_DEDUPPCH_H

/**
 * @file deduppch.h
 * @brief What the dedup module needs from its surroundings (MERGE-01).
 *
 * The module used to include gallery_info's `pch.h`, which dragged the whole
 * program's Qt surface (and its QSql/QNetwork modules) into a library that is
 * meant to be shared. This header is the module's own answer to the same
 * question: the Qt types the code here actually uses, plus the two imaging
 * dependencies (OpenCV and libjpeg-turbo).
 *
 * It is deliberately not added to any precompiled header: tag_quick_editor
 * compiles every source with its own PCH, and pulling OpenCV into that one
 * would slow down every file in the program for a window that is opened once
 * in a while.
 *
 * Include this first in a dedup source file. `gi` still force-includes its own
 * pch.h; both have include guards, and including this one after it is free.
 */

/*
 * The two programs' PCHs leave macros behind that a template-heavy module
 * cannot live with, and a PCH is parsed before anything here can react. Clear
 * them first, before any Qt header below:
 *
 *  - `new`: tag_quick_editor defines it as DEBUG_NEW for the debug CRT's leak
 *    tracking (`_CRTDBG_MAP_ALLOC`). Its own code writes `new Foo(...)` and is
 *    fine; Qt and OpenCV use *placement* new (`new (n) Node{...}`), which the
 *    macro rewrites into a syntax error inside qcache.h.
 *  - `min` / `max`: Windows.h brings them in; this module uses std::min/max.
 */
#ifdef new
#undef new
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <QApplication>
#include <QAtomicInt>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QDrag>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QGraphicsOpacityEffect>
#include <QHash>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidgetItem>
#include <QLocale>
#include <QMainWindow>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QMimeData>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointF>
#include <QProcess>
#include <QProgressDialog>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRunnable>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QSplitter>
#include <QTextEdit>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantList>
#include <QVariantMap>
#include <QWheelEvent>
#include <QWidget>
#include <QtConcurrent>
#include <QtMath>

// Windows.h: ask for no min/max macros and no rarely used API.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "fonts.h"      // appFontFamily() - the shared font family

#include <opencv2/opencv.hpp>
#include <jpeglib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#endif // DEDUP_DEDUPPCH_H
