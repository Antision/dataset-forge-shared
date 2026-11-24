#ifndef DUPMOVE_H
#define DUPMOVE_H

/*
 * 文件级"移入 duplicate / 从 duplicate 恢复"的通用实现(不依赖窗口,可离屏测试)。
 * 规则:目标已存在时加 _1/_2 后缀,绝不覆盖;失败返回 error,不谎报成功。
 */
#include <QSet>
#include <QString>

namespace dupmove {

struct Result {
    bool    ok = false;
    QString targetPath;   // 成功后的真实路径
    QString error;
};

/* dir 下给 fileName 找一个不冲突的名字(存在即 _1、_2 …) */
QString uniquePath(const QString &dir, const QString &fileName,
                   const QSet<QString> &reserved = QSet<QString>());

/* 把 filePath 移进 duplicateDir 子目录(不存在则创建) */
Result moveIntoDuplicate(const QString &filePath, const QString &duplicateDir);

/* 把 duplicate 里的文件恢复到 originalPath;原名被占则加 _num,不覆盖 */
Result restoreFromDuplicate(const QString &pathInDuplicate, const QString &originalPath);

} // namespace dupmove

#endif // DUPMOVE_H
