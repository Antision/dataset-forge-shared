#include "dupmove.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace dupmove {
namespace {

QString uniqueInDir(const QString &dir, const QString &fileName)
{
    const QFileInfo fi(fileName);
    const QString base = fi.completeBaseName();
    const QString suffix = fi.suffix().isEmpty() ? QString()
                                                 : QLatin1Char('.') + fi.suffix();
    QString candidate = QDir(dir).filePath(fileName);
    if (!QFileInfo::exists(candidate))
        return candidate;

    for (int i = 1; i < 10000; ++i) {
        candidate = QDir(dir).filePath(QStringLiteral("%1_%2%3").arg(base).arg(i).arg(suffix));
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
    return QString();
}

} // namespace

QString uniquePath(const QString &dir, const QString &fileName, const QSet<QString> &reserved)
{
    const QFileInfo fi(fileName);
    const QString base = fi.completeBaseName();
    const QString suffix = fi.suffix().isEmpty() ? QString()
                                                 : QLatin1Char('.') + fi.suffix();

    auto taken = [&](const QString &path) {
        return reserved.contains(QDir::cleanPath(path).toLower()) || QFileInfo::exists(path);
    };

    QString candidate = QDir(dir).filePath(fileName);
    if (!taken(candidate))
        return candidate;

    for (int i = 1; i < 10000; ++i) {
        candidate = QDir(dir).filePath(QStringLiteral("%1_%2%3").arg(base).arg(i).arg(suffix));
        if (!taken(candidate))
            return candidate;
    }
    return QString();
}

Result moveIntoDuplicate(const QString &filePath, const QString &duplicateDir)
{
    Result r;
    if (!QFileInfo(filePath).isFile()) {
        r.error = QStringLiteral("源文件不存在: %1").arg(filePath);
        return r;
    }
    if (!QDir().mkpath(duplicateDir)) {
        r.error = QStringLiteral("无法创建 duplicate 目录: %1").arg(duplicateDir);
        return r;
    }

    const QString target = uniquePath(duplicateDir, QFileInfo(filePath).fileName());
    if (target.isEmpty()) {
        r.error = QStringLiteral("无法为 %1 生成不冲突的文件名").arg(filePath);
        return r;
    }
    if (QFile::exists(target)) {
        r.error = QStringLiteral("目标已存在,拒绝覆盖: %1").arg(target);
        return r;
    }
    if (!QFile::rename(filePath, target)) {
        r.error = QStringLiteral("移动失败: %1 -> %2").arg(filePath, target);
        return r;
    }

    r.ok = true;
    r.targetPath = target;
    return r;
}

Result restoreFromDuplicate(const QString &pathInDuplicate, const QString &originalPath)
{
    Result r;
    if (!QFileInfo(pathInDuplicate).isFile()) {
        r.error = QStringLiteral("duplicate 里找不到文件: %1").arg(pathInDuplicate);
        return r;
    }

    const QFileInfo orig(originalPath);
    const QString dir = orig.absolutePath();
    if (!QDir().mkpath(dir)) {
        r.error = QStringLiteral("无法创建目录: %1").arg(dir);
        return r;
    }
    // 原名可用就还原原名,否则 _num(不覆盖)
    const QString target = uniquePath(dir, orig.fileName());
    if (target.isEmpty()) {
        r.error = QStringLiteral("无法为 %1 生成不冲突的文件名").arg(originalPath);
        return r;
    }
    if (!QFile::rename(pathInDuplicate, target)) {
        r.error = QStringLiteral("恢复失败: %1 -> %2").arg(pathInDuplicate, target);
        return r;
    }

    r.ok = true;
    r.targetPath = target;
    return r;
}

} // namespace dupmove
