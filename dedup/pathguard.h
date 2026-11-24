#ifndef PATHGUARD_H
#define PATHGUARD_H

/*
 * 输入路径的防御性校验(纯函数,有单测)。
 *
 * "去重时输入多个路径"这件事本身就是危险的:同一个文件如果同时属于两个输入目录,
 * 它会被扫描两次、depth 被两个根各算一次、完全重复的自动归属两边打架 ——
 * 而这些动作都会真的搬文件。所以在入口处拦住,而不是进去之后再补救。
 */

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace pathguard {

/* 规范化:统一分隔符、去尾斜杠、小写(Windows 路径大小写不敏感) */
inline QString norm(const QString &p)
{
    QString s = QDir::cleanPath(QDir::toNativeSeparators(p.trimmed()));
    s.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (s.endsWith(QLatin1Char('/')) && s.size() > 1) s.chop(1);
    return s.toLower();
}

/* child 是否等于 parent,或位于 parent 之下。按路径分隔符判边界,不是字符串前缀 */
inline bool isUnder(const QString &child, const QString &parent)
{
    const QString c = norm(child);
    const QString p = norm(parent);
    if (c.isEmpty() || p.isEmpty()) return false;
    if (p == QLatin1String(".")) return false;   // 空路径被 cleanPath 成 "." 时不要当根
    return c == p || c.startsWith(p + QLatin1Char('/'));
}

/*
 * 多个输入路径之间不允许互相包含。
 * 返回空串 = 通过;否则返回可直接展示给用户的原因(含出问题的那两条路径)。
 */
inline QString checkNoNested(const QStringList &paths)
{
    for (int i = 0; i < paths.size(); ++i) {
        if (paths[i].trimmed().isEmpty()) continue;
        for (int j = i + 1; j < paths.size(); ++j) {
            if (paths[j].trimmed().isEmpty()) continue;
            const QString a = paths[i].trimmed();
            const QString b = paths[j].trimmed();
            if (norm(a).isEmpty() || norm(b).isEmpty()) continue;
            if (isUnder(a, b)) {
                return QStringLiteral("输入目录存在包含关系,会重复扫描同一批文件:\n\n"
                                      "%1\n    包含于 %2\n\n"
                                      "请只保留最外层目录,或把嵌套的那一层移出去。").arg(a, b);
            }
            if (isUnder(b, a)) {
                return QStringLiteral("输入目录存在包含关系,会重复扫描同一批文件:\n\n"
                                      "%1\n    包含于 %2\n\n"
                                      "请只保留最外层目录,或把嵌套的那一层移出去。").arg(b, a);
            }
        }
    }
    return QString();
}

} // namespace pathguard

#endif // PATHGUARD_H
