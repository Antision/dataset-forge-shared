#ifndef DEDUP_DEDUPENV_H
#define DEDUP_DEDUPENV_H

/**
 * @file dedupenv.h
 * @brief The handful of services the dedup module needs from its host program.
 *
 * MERGE-01: the module used to call gallery_info's `utils::` helpers and
 * `appaudio::playSound()` directly, which is exactly what kept it from moving
 * into `common`. Everything program-specific is concentrated here:
 *
 *  - prompt sounds: an injectable handler (no audio backend in common);
 *  - log / cache directories: writable defaults, a program may override them;
 *  - shell helpers (reveal in Explorer, move to trash, dark title bar): the
 *    built-in Windows implementations are good enough on their own, and a
 *    program that already has a better one (gallery_info has a faster trash
 *    path) installs it as a handler.
 *
 * Everything here is GUI-thread only.
 */

#include <QString>

#include <functional>

/// The prompt sounds the window asks for. Values only; how they are played is
/// the host's business.
enum class DedupSound {
    SingleAction,
    BatchAction,
    Error,
    NothingRemoved
};

namespace dedupenv {

// -- prompt sounds ---------------------------------------------------------

/// Installs the host's sound backend (nullptr: silence). Not owned.
void setSoundHandler(std::function<void(DedupSound)> handler);
void playSound(DedupSound sound);

// -- directories -----------------------------------------------------------

/// Root for this module's trace logs. Defaults to `<appDir>/logs`; empty when
/// that cannot be created (callers must skip logging silently).
QString logDir();
void setLogDir(const QString& dir);

/// `preferred` when it is really writable, otherwise `<appDir>/cache`; empty
/// when neither works. "Really" means a one-byte file was written and removed
/// — a directory that exists on a full or read-only drive is not writable.
QString writableCacheDir(const QString& preferred);
/// `writableCacheDir(preferred)/subDir`, created; empty on failure.
QString cacheSubDir(const QString& preferred,const QString& subDir);

// -- shell helpers ---------------------------------------------------------

/// The file itself, or the copy the dedup module moved into `<dir>/duplicate`
/// (`name_1.ext`, ...), or the duplicate directory, or the original path.
QString resolveExistingPathForReveal(const QString& filePath);
/// Reveals `filePath` in Explorer. Uses the installed handler when there is
/// one, otherwise the built-in Shell API path (CJK + spaces safe).
void revealFileInExplorer(const QString& filePath);
void setRevealHandler(std::function<void(const QString&)> handler);

/// Moves a file or directory to the recycle bin. Uses the installed handler
/// when there is one, otherwise SHFileOperationW.
bool moveToTrash(const QString& path);
void setTrashHandler(std::function<bool(const QString&)> handler);

/// Dark title bar for a native window handle (`HWND` as void* on purpose: this
/// header must not drag Windows.h into every translation unit that includes it).
void enableDarkTitleBar(void* nativeHandle);

} // namespace dedupenv

#endif // DEDUP_DEDUPENV_H
