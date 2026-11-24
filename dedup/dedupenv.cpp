#include "dedupenv.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QUrl>

#include <string>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#include <Windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace {

std::function<void(DedupSound)>& soundHandler()
{
    static std::function<void(DedupSound)> handler;
    return handler;
}

QString& logDirOverride()
{
    static QString dir;
    return dir;
}

std::function<void(const QString&)>& revealHandler()
{
    static std::function<void(const QString&)> handler;
    return handler;
}

std::function<bool(const QString&)>& trashHandler()
{
    static std::function<bool(const QString&)> handler;
    return handler;
}

/// A directory we can really write to: exists() alone lies on a full or
/// read-only drive, so write one byte and remove it again.
bool dirIsWritable(const QString& path)
{
    if(path.isEmpty())
        return false;
    QDir dir(path);
    if(!dir.exists()&&!dir.mkpath(QStringLiteral(".")))
        return false;
    QTemporaryFile probe(dir.filePath(QStringLiteral(".write_probe_XXXXXX")));
    probe.setAutoRemove(true);
    if(!probe.open())
        return false;
    const bool ok=probe.write("1",1)==1;
    probe.close();
    return ok;
}

/// One probe per directory and process: probing creates a temporary file, and
/// doing it again for every pair leaves noise in the user's directories.
bool dirIsWritableOnce(const QString& path)
{
    static QMutex mtx;
    static QHash<QString,bool> cache;
    QMutexLocker lock(&mtx);
    const auto it=cache.constFind(path);
    if(it!=cache.constEnd())
        return it.value();
    const bool ok=dirIsWritable(path);
    cache.insert(path,ok);
    return ok;
}

} // namespace

namespace dedupenv {

void setSoundHandler(std::function<void(DedupSound)> handler)
{
    soundHandler()=std::move(handler);
}

void playSound(DedupSound sound)
{
    if(soundHandler())
        soundHandler()(sound);
}

QString logDir()
{
    if(!logDirOverride().isEmpty())
        return logDirOverride();
    const QString portable=QCoreApplication::applicationDirPath()+QStringLiteral("/logs");
    if(QDir().mkpath(portable))
        return portable;
    return QString();
}

void setLogDir(const QString& dir)
{
    logDirOverride()=dir;
}

QString writableCacheDir(const QString& preferred)
{
    const QString cleaned=QDir::cleanPath(QDir::toNativeSeparators(preferred.trimmed()));
    if(!cleaned.isEmpty()&&cleaned!=QLatin1String(".")
        &&dirIsWritableOnce(cleaned))
        return cleaned;

    const QString portable=
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("cache"));
    if(dirIsWritableOnce(portable))
        return portable;
    return QString();
}

QString cacheSubDir(const QString& preferred,const QString& subDir)
{
    const QString root=writableCacheDir(preferred);
    if(root.isEmpty())
        return QString();
    const QString cleanedSub=QDir::cleanPath(subDir);
    if(cleanedSub.isEmpty()||cleanedSub==QLatin1String("."))
        return root;
    QDir dir(root);
    if(!dir.mkpath(cleanedSub))
        return QString();
    return dir.filePath(cleanedSub);
}

QString resolveExistingPathForReveal(const QString& filePath)
{
    if(filePath.isEmpty())
        return QString();
    QFileInfo info(filePath);
    if(info.exists())
        return info.absoluteFilePath();

    // The dedup module moves rejects into <dir>/duplicate/, so a path that no
    // longer exists is usually found there, under its own name or with the
    // `_1` suffix the move adds when the name is taken.
    const QDir dir=info.absoluteDir();
    const QString duplicateDir=dir.absoluteFilePath(QStringLiteral("duplicate"));
    const QString sameName=QDir(duplicateDir).absoluteFilePath(info.fileName());
    if(QFileInfo::exists(sameName))
        return sameName;

    const QString base=info.completeBaseName();
    const QString suffix=info.suffix();
    QDir duplicates(duplicateDir);
    if(duplicates.exists()){
        const QString pattern=suffix.isEmpty()
                                  ?QStringLiteral("%1_*").arg(base)
                                  :QStringLiteral("%1_*.%2").arg(base,suffix);
        const QStringList hits=duplicates.entryList(QStringList{pattern},QDir::Files,QDir::Name);
        if(!hits.isEmpty())
            return duplicates.absoluteFilePath(hits.first());
    }
    if(QFileInfo::exists(duplicateDir))
        return duplicateDir;
    return info.absoluteFilePath();
}

void setRevealHandler(std::function<void(const QString&)> handler)
{
    revealHandler()=std::move(handler);
}

void revealFileInExplorer(const QString& filePath)
{
    if(revealHandler()){
        revealHandler()(filePath);
        return;
    }
    const QString target=resolveExistingPathForReveal(filePath);
    if(target.isEmpty())
        return;
    const QFileInfo info(target);

#ifdef Q_OS_WIN
    // SHOpenFolderAndSelectItems hands the ITEMIDLIST straight to Explorer:
    // no command line, so spaces and CJK path segments stay intact (the old
    // `explorer.exe /select,` fallback opened Documents for those).
    if(info.exists()){
        const HRESULT hrInit=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        const bool weInitialized=SUCCEEDED(hrInit);
        if(SUCCEEDED(hrInit)||hrInit==RPC_E_CHANGED_MODE){
            bool ok=false;
            const std::wstring native=
                QDir::toNativeSeparators(info.absoluteFilePath()).toStdWString();
            PIDLIST_ABSOLUTE pidl=ILCreateFromPathW(native.c_str());
            if(pidl){
                ok=SUCCEEDED(SHOpenFolderAndSelectItems(pidl,0,nullptr,0));
                ILFree(pidl);
            }
            if(weInitialized)
                CoUninitialize();
            if(ok)
                return;
        }
        const QString arg=QStringLiteral("/select,%1").arg(QDir::toNativeSeparators(target));
        if(QProcess::startDetached(QStringLiteral("explorer.exe"),QStringList{arg}))
            return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.isDir()?info.absoluteFilePath()
                                                            :info.absolutePath()));
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.isDir()?info.absoluteFilePath()
                                                            :info.absolutePath()));
#endif
}

void setTrashHandler(std::function<bool(const QString&)> handler)
{
    trashHandler()=std::move(handler);
}

bool moveToTrash(const QString& path)
{
    if(trashHandler())
        return trashHandler()(path);
    QFileInfo info(path);
    if(!info.exists())
        return false;

#ifdef Q_OS_WIN
    // SHFileOperationW wants a double-null-terminated list of native paths; a
    // forward-slash path makes it report failure while still deleting, which
    // reads as "failed" to every caller.
    std::wstring native=QDir::toNativeSeparators(path).toStdWString();
    std::vector<wchar_t> from(native.begin(),native.end());
    from.push_back(L'\0');
    from.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.wFunc=FO_DELETE;
    op.pFrom=from.data();
    // No error UI: the callers inspect the result and log it; a modal system
    // box here would block the UI thread with nothing the user can choose.
    op.fFlags=FOF_ALLOWUNDO|FOF_NOCONFIRMATION|FOF_SILENT|FOF_NOERRORUI;
    const int result=SHFileOperationW(&op);
    if(result!=0||op.fAnyOperationsAborted){
        // The return value is not fully reliable: some paths report an error
        // after the file really went to the bin. The source still existing is
        // the honest answer.
        if(!QFileInfo::exists(info.absoluteFilePath()))
            return true;
        return false;
    }
    return true;
#else
    const QString trash=QStandardPaths::writableLocation(QStandardPaths::TrashLocation)
                        +QStringLiteral("/files");
    QDir().mkpath(trash);
    const QString target=QDir(trash).filePath(info.fileName());
    return QDir().rename(path,target);
#endif
}

void enableDarkTitleBar(void* nativeHandle)
{
#ifdef Q_OS_WIN
    if(!nativeHandle)
        return;
    BOOL dark=TRUE;
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 10 1809+).
    DwmSetWindowAttribute(static_cast<HWND>(nativeHandle),20,&dark,sizeof(dark));
#else
    Q_UNUSED(nativeHandle);
#endif
}

} // namespace dedupenv
