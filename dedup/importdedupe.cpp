#include "importdedupe.h"
#include "imagededuplicatewindow.h"
#include "dedupworker.h"
#include "deduputils.h"
#include "dedupenv.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

using importdedupe::Disp;
using importdedupe::FileOp;
using importdedupe::ImportDedupeSession;
using importdedupe::Level;
using importdedupe::PairDecision;
using importdedupe::ScannedFile;
using importdedupe::ScanResult;

namespace {

/* ==================================================================
 *                          基础工具
 * ================================================================== */

QString tsStr()
{
    return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
}

/*
 * 路径规范化键:所有"这个路径是不是同一个"的判断都用它。
 * Windows 上大小写不敏感,所以统一转小写;分隔符统一成 '/'。
 */
QString normKey(const QString &path)
{
    QString p = QDir::cleanPath(QDir::toNativeSeparators(path.trimmed()));
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return p.toLower();
}

QString joinPath(const QString &root, const QString &rel)
{
    return QDir::cleanPath(QDir::toNativeSeparators(root) + QLatin1Char('/') + rel);
}

bool isImageFile(const QString &absPath)
{
    const QString suffix = QFileInfo(absPath).suffix().toLower();
    return dedup::supportedExtensions().contains(suffix);
}

/*
 * 卷标识:只有同一个卷上的移动才能靠 QFile::rename 一步完成。
 * 盘符取前两个字符;UNC(\\server\share\...) 取 server+share。
 */
QString volumeKeyOf(const QString &path)
{
    QString p = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    if (p.startsWith(QStringLiteral("\\\\"))) {
        const QStringList parts = p.mid(2).split(QLatin1Char('\\'), Qt::SkipEmptyParts);
        if (parts.size() >= 2) return (parts[0] + QLatin1Char('/') + parts[1]).toLower();
        return p.toLower();
    }
    return p.left(2).toUpper();
}

enum class CmpResult { Equal, Different, Error };

/*
 * 逐字节比较两个文件(哈希只用来筛选,最终一致性以字节为准 —— 避免哈希碰撞误判)。
 */
CmpResult compareTwoFiles(const QString &a, const QString &b, QString *err)
{
    QFileInfo fa(a), fb(b);
    if (!fa.exists()) { if (err) *err = QStringLiteral("文件不存在: %1").arg(a); return CmpResult::Error; }
    if (!fb.exists()) { if (err) *err = QStringLiteral("文件不存在: %1").arg(b); return CmpResult::Error; }
    if (fa.size() != fb.size()) return CmpResult::Different;

    QFile f1(a), f2(b);
    if (!f1.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("无法读取: %1 (%2)").arg(a, f1.errorString());
        return CmpResult::Error;
    }
    if (!f2.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("无法读取: %1 (%2)").arg(b, f2.errorString());
        return CmpResult::Error;
    }

    constexpr qint64 kChunk = 1 << 20;   // 1MB
    QByteArray ba, bb;
    while (!f1.atEnd()) {
        ba = f1.read(kChunk);
        bb = f2.read(kChunk);
        if (ba.size() != bb.size()) return CmpResult::Different;
        if (ba.isEmpty()) break;
        if (std::memcmp(ba.constData(), bb.constData(), static_cast<size_t>(ba.size())) != 0)
            return CmpResult::Different;
    }
    return CmpResult::Equal;
}

/*
 * 冲突改名的候选:name.ext -> name_1.ext -> name_2.ext ...
 * 源文件没有扩展名时就变成 001 -> 001_1(需求里明确要求的行为)。
 */
QString withSuffixNumber(const QString &path, int n)
{
    QFileInfo fi(path);
    const QString dir = fi.absolutePath();
    const QString base = fi.completeBaseName();
    const QString suffix = fi.suffix();
    QString name = base + QLatin1Char('_') + QString::number(n);
    if (!suffix.isEmpty()) name += QLatin1Char('.') + suffix;
    return QDir(dir).filePath(name);
}

/* dir(或整条路径)里有没有名为 component 的一段(忽略大小写) */
bool pathHasComponent(const QString &path, const QString &component)
{
    const QString norm = QDir::cleanPath(QDir::toNativeSeparators(path));
    const QStringList parts = norm.split(QRegularExpression(QStringLiteral("[\\\\/]")),
                                         Qt::SkipEmptyParts);
    for (const QString &p : std::as_const(parts)) {
        if (p.compare(component, Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

/* 扫描时要跳过的目录:normal dedup 的隔离区以及脚本的备用回收目录 */
bool isSkippedDirName(const QString &name)
{
    if (name.compare(QStringLiteral("duplicate"), Qt::CaseInsensitive) == 0) return true;
    if (name.startsWith(QStringLiteral(".recycle_bin"), Qt::CaseInsensitive)) return true;
    /*
     * not_in_a 是"B 独有"文件的停放区,不属于待整理的画廊内容。
     * 不跳过的话,下一次 import 会把它当普通 B 文件重新比较:内容如果和 A 里某张
     * 一致,还会被判成"二进制重复"把 A 那张删掉、再把停放区的文件搬回作品目录,
     * 等于把上一次的隔离结果又翻出来。所以整棵 not_in_a 一律不扫。
     */
    if (name.compare(QStringLiteral("not_in_a"), Qt::CaseInsensitive) == 0) return true;
    /* 解压冲突暂存区:里面是还没决定归谁的文件,不能当作品内容整理 */
    if (name.compare(QStringLiteral(".temp"), Qt::CaseInsensitive) == 0) return true;
    return false;
}

/* ==================================================================
 *  阶段一之一:递归扫描(所有文件类型,不含隔离目录/符号链接/特征缓存)
 * ================================================================== */

void collectRootFiles(const QString &root,
                      bool fromA,
                      ScanResult &res,
                      const std::function<void(const QString &, Level)> &log,
                      const std::atomic<bool> &cancel)
{
    const QString cleanRoot = QDir::cleanPath(QDir::toNativeSeparators(root));
    QVector<ScannedFile> found;

    /*
     * 进度日志(节流到 2 秒一条)。
     *
     * 这一段原来是"扫完一棵树才说一句话":大目录要跑几分钟,日志窗口里
     * 一个字都没有 —— 用户唯一的感受就是"卡死了"。现在至少能看到
     * "已经收集了多少个文件",也能据此判断是不是还在动。
     */
    qint64 lastProgressMs = QDateTime::currentMSecsSinceEpoch();
    qint64 seen = 0;
    const QChar which = fromA ? QLatin1Char('A') : QLatin1Char('B');

    QDirIterator it(cleanRoot, QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (cancel.load()) return;
        const QString path = it.next();
        ++seen;
        const QFileInfo fi(path);

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs - lastProgressMs >= 2000) {
            lastProgressMs = nowMs;
            log(QStringLiteral("[阶段一] 扫描 %1: 已遍历 %2 个文件…").arg(which).arg(seen),
                Level::Detail);
        }

        // 隔离目录(duplicate / .recycle_bin*)里的东西不属于画廊内容
        const QString relToRoot = QDir(cleanRoot).relativeFilePath(fi.absoluteFilePath());
        {
            bool skip = false;
            const QStringList parts = QDir::fromNativeSeparators(relToRoot)
                                          .split(QLatin1Char('/'), Qt::SkipEmptyParts);
            for (int i = 0; i + 1 < parts.size(); ++i) {          // 最后一段是文件名
                if (isSkippedDirName(parts[i])) { skip = true; break; }
            }
            if (skip) { ++res.skippedQuarantine; continue; }
        }

        if (fi.isSymLink()) { ++res.skippedSymlinks; continue; }

        // 特征缓存文件不是画廊内容,别把它当成一个"文件"去比较/搬运
        if (fi.fileName().compare(QStringLiteral("image_features.tscch"),
                                  Qt::CaseInsensitive) == 0) {
            ++res.skippedCacheFiles;
            continue;
        }

        ScannedFile sf;
        sf.fromA   = fromA;
        sf.rel     = QDir::fromNativeSeparators(relToRoot);
        sf.abs     = fi.absoluteFilePath();
        sf.size    = fi.size();
        sf.mtimeMs = fi.lastModified().toMSecsSinceEpoch();
        found.push_back(std::move(sf));
    }

    /*
     * 组内元素顺序 = 扫描顺序,而扫描顺序必须是确定的:
     * 按相对路径的自然序(A 全部在前,B 全部在后)。
     * 这样"以组内第一个元素为优先项"的规则每次跑都得到同样的结果。
     */
    std::sort(found.begin(), found.end(), [](const ScannedFile &a, const ScannedFile &b) {
        return ImportDedupeSession::naturalKey(a.rel) < ImportDedupeSession::naturalKey(b.rel);
    });

    for (ScannedFile &sf : found)
        res.files.push_back(std::move(sf));

    log(QStringLiteral("[%1] 扫描 %2: 收集 %3 个文件")
            .arg(fromA ? QStringLiteral("A") : QStringLiteral("B"))
            .arg(cleanRoot)
            .arg(found.size()),
        Level::Info);
}

/* ==================================================================
 *  阶段一之二:大小分组 -> SHA-256 -> 逐字节校验 -> 同哈希组
 * ================================================================== */

void buildHashGroups(ScanResult &res,
                     const std::function<void(const QString &, Level)> &log,
                     const std::atomic<bool> &cancel)
{
    const int n = static_cast<int>(res.files.size());
    if (n <= 1) return;

    // (1) 先用大小粗筛:内容相同的文件大小必然相同,大小唯一的文件连哈希都不用算
    QHash<qint64, QVector<int>> bySize;
    bySize.reserve(n);
    for (int i = 0; i < n; ++i)
        bySize[res.files[i].size].push_back(i);

    QVector<int> toHash;
    for (auto it = bySize.constBegin(); it != bySize.constEnd(); ++it) {
        if (it.value().size() >= 2)
            toHash += it.value();
    }

    log(QStringLiteral("[阶段一] 共 %1 个文件;大小存在重复的候选 %2 个,开始计算 SHA-256")
            .arg(n).arg(toHash.size()),
        Level::Info);

    // (2) 哈希(并发;这里的 I/O 是最耗时的一段)
    if (!toHash.isEmpty()) {
        QVector<QString> paths;
        paths.reserve(toHash.size());
        for (int idx : std::as_const(toHash))
            paths.push_back(res.files[idx].abs);

        std::atomic<int> done{0};
        std::atomic<qint64> lastProgressMs{QDateTime::currentMSecsSinceEpoch()};
        const int totalToHash = paths.size();
        const QVector<QByteArray> digests = QtConcurrent::blockingMapped(
            paths, [&done, &cancel, &log, &lastProgressMs, totalToHash](const QString &p) -> QByteArray {
                if (cancel.load()) return QByteArray();
                bool ok = false;
                const QByteArray d = dedup::fileSHA256(p, &ok);
                const int d0 = done.fetch_add(1) + 1;

                /*
                 * 哈希是大目录里最慢的一段。每 2 秒报一次进度,
                 * 让"还在算"和"已经卡死"在日志里能区分开。
                 */
                const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                qint64 prev = lastProgressMs.load(std::memory_order_relaxed);
                if (nowMs - prev >= 2000
                    && lastProgressMs.compare_exchange_strong(prev, nowMs)) {
                    log(QStringLiteral("[阶段一] 计算 SHA-256: %1 / %2")
                            .arg(d0).arg(totalToHash),
                        Level::Detail);
                }
                return ok ? d : QByteArray();
            });

        for (int k = 0; k < toHash.size(); ++k) {
            if (cancel.load()) return;
            const QByteArray &d = digests[k];
            if (d.size() == 32)
                res.files[toHash[k]].hash = d;
            else
                ++res.hashFailures;
        }
    }

    // (3) 按 (大小, 哈希) 归组
    QHash<QByteArray, QVector<int>> byHash;
    for (int idx : std::as_const(toHash)) {
        const QByteArray &h = res.files[idx].hash;
        if (h.size() != 32) continue;      // 读不出来的文件不参与分组
        byHash[h].push_back(idx);          // toHash 已经是下标升序 -> 组内保持扫描顺序
    }

    // (4) 逐字节确认,顺便把"哈希撞了但内容不同"的拆开
    QVector<QVector<int>> groups;
    for (auto it = byHash.constBegin(); it != byHash.constEnd(); ++it) {
        if (cancel.load()) return;

        QVector<QVector<int>> pending;
        pending.push_back(it.value());

        while (!pending.isEmpty()) {
            QVector<int> cur = pending.takeLast();
            if (cur.size() < 2) continue;

            // 以第一个为基准逐字节校验(它就是"组内优先项")
            const int rep = cur[0];
            QVector<int> same{rep};
            QVector<int> different;
            for (int k = 1; k < cur.size(); ++k) {
                QString err;
                const CmpResult r = compareTwoFiles(res.files[rep].abs,
                                                    res.files[cur[k]].abs, &err);
                if (r == CmpResult::Equal)
                    same.push_back(cur[k]);
                else if (r == CmpResult::Different)
                    different.push_back(cur[k]);
                else {
                    // 读不出来:不把它并进任何组(保守:不做任何移动/删除)
                    ++res.hashFailures;
                    log(QStringLiteral("[阶段一] 无法比较,已跳过: %1 (%2)")
                            .arg(res.files[cur[k]].abs, err), Level::Warning);
                }
            }
            if (same.size() >= 2) groups.push_back(same);
            if (different.size() >= 2) {
                ++res.collisionSplits;
                log(QStringLiteral("[阶段一] 发现哈希碰撞:同一哈希下有 %1 个文件逐字节不同,已拆组")
                        .arg(different.size()), Level::Warning);
                pending.push_back(different);
            }
        }
    }

    // 组之间的顺序也确定化:按"组内最早出现的下标"排
    std::sort(groups.begin(), groups.end(), [](const QVector<int> &a, const QVector<int> &b) {
        return a.first() < b.first();
    });
    res.groups = groups;

    int dupFiles = 0;
    for (const QVector<int> &g : std::as_const(res.groups)) dupFiles += g.size() - 1;
    log(QStringLiteral("[阶段一] 找到 %1 个内容相同的分组(涉及 %2 个多余副本)")
            .arg(res.groups.size()).arg(dupFiles),
        Level::Info);
}

/* ==================================================================
 *  阶段一之三:同哈希组规则 -> 操作 + 处置
 * ================================================================== */

void planBinaryPhase(const ScanResult &scan,
                     QVector<FileOp> &ops,
                     QVector<Disp> &disp,
                     const std::function<void(const QString &, Level)> &log)
{
    auto makeOp = [&scan](FileOp::Kind kind, int fileIdx, const QString &target,
                          const QString &reason, const QString &origin) {
        FileOp op;
        op.kind    = kind;
        const ScannedFile &f = scan.files[fileIdx];
        op.source  = f.abs;
        op.target  = target;
        op.reason  = reason;
        op.origin  = origin;
        op.sourceSize    = f.size;
        op.sourceMtimeMs = f.mtimeMs;
        op.sourceHash    = f.hash;
        return op;
    };

    for (const QVector<int> &g : std::as_const(scan.groups)) {
        QVector<int> aMembers, bMembers;
        for (int idx : g)
            (scan.files[idx].fromA ? aMembers : bMembers).push_back(idx);

        const QString hashTag = QStringLiteral("binary:")
                                + QString::fromLatin1(scan.files[g.first()].hash.left(8).toHex());

        if (!aMembers.isEmpty() && !bMembers.isEmpty()) {
            /* ---- 交叉组:内容同时在 A、B 里 ---- */
            const ScannedFile &anchor = scan.files[aMembers.first()];

            if (anchor.rel.isEmpty()) {
                log(QStringLiteral("[阶段一] 跳过歧义分组(锚点相对路径为空): %1")
                        .arg(anchor.abs), Level::Error);
                continue;
            }
            const QString targetAbs = joinPath(scan.rootB, anchor.rel);

            /*
             * 组内优先项:优先选"已经在目标路径上"的 B 成员(免得白搬一次),
             * 否则用 B 侧第一个成员(结果数组顺序就是既有选择顺序)。
             */
            int survivor = -1;
            for (int b : std::as_const(bMembers)) {
                if (scan.files[b].rel.compare(anchor.rel, Qt::CaseInsensitive) == 0) {
                    survivor = b;
                    break;
                }
            }
            if (survivor < 0) survivor = bMembers.first();

            if (aMembers.size() > 1) {
                QStringList cands;
                for (int a : std::as_const(aMembers)) cands << scan.files[a].rel;
                log(QStringLiteral("[阶段一] 分组 %1 在 A 侧有多个候选路径(歧义):%2;"
                                   "按结果数组顺序取第一个作为 B 的目标结构:%3")
                        .arg(hashTag, cands.join(QStringLiteral(" | ")), anchor.rel),
                    Level::Warning);
            }

            // A 侧副本全部进回收站(内容已由 B 侧保留)
            for (int a : std::as_const(aMembers)) {
                ops.push_back(makeOp(FileOp::Kind::Trash, a,
                                     QString(),
                                     QStringLiteral("与 B 侧内容二进制完全一致,A 侧副本进回收站"),
                                     hashTag));
                disp[a] = Disp::Trash;
            }
            // B 侧多余副本进回收站
            for (int b : std::as_const(bMembers)) {
                if (b == survivor) continue;
                ops.push_back(makeOp(FileOp::Kind::Trash, b,
                                     QString(),
                                     QStringLiteral("同哈希组内的多余副本(整组只保留一份)"),
                                     hashTag));
                disp[b] = Disp::Trash;
            }
            // 幸存者落到 A 的相对路径上
            if (scan.files[survivor].rel.compare(anchor.rel, Qt::CaseInsensitive) == 0) {
                disp[survivor] = Disp::InPlaceAtTarget;
                log(QStringLiteral("[阶段一] 分组 %1:代表文件已在目标位置,不重复移动: %2")
                        .arg(hashTag, scan.files[survivor].abs), Level::Detail);
            } else {
                ops.push_back(makeOp(FileOp::Kind::Move, survivor, targetAbs,
                                     QStringLiteral("二进制匹配:按 A 的相对路径归并到 B"),
                                     hashTag));
                disp[survivor] = Disp::MoveToTarget;
            }
        } else if (!bMembers.isEmpty()) {
            /* ---- 仅存在于 B:整组只留一个代表,按 B 独有文件处理 ---- */
            const int survivor = bMembers.first();
            for (int b : std::as_const(bMembers)) {
                if (b == survivor) continue;
                ops.push_back(makeOp(FileOp::Kind::Trash, b,
                                     QString(),
                                     QStringLiteral("仅存在于 B 的同哈希组,组内只保留一份代表"),
                                     hashTag));
                disp[b] = Disp::Trash;
            }
            disp[survivor] = Disp::Unhandled;   // 稍后按"B 独有"进 not_in_a
        } else {
            /* ---- 仅存在于 A:保留一个代表于 A,其余进回收站(不得自动搬进 B) ---- */
            const int survivor = aMembers.first();
            for (int a : std::as_const(aMembers)) {
                if (a == survivor) continue;
                ops.push_back(makeOp(FileOp::Kind::Trash, a,
                                     QString(),
                                     QStringLiteral("仅存在于 A 的同哈希组,组内只保留一份代表"),
                                     hashTag));
                disp[a] = Disp::Trash;
            }
            disp[survivor] = Disp::KeepInA;
        }
    }
}

/* ==================================================================
 *  阶段二:把用户在窗口里的判定接进计划
 * ================================================================== */

void planDecisionPhase(const ScanResult &scan,
                       const QHash<QString, int> &indexByPath,
                       const QVector<PairDecision> &decisions,
                       QVector<FileOp> &ops,
                       QVector<Disp> &disp,
                       const std::function<void(const QString &, Level)> &log)
{
    auto makeOp = [&scan](FileOp::Kind kind, int fileIdx, const QString &target,
                          const QString &reason, const QString &origin, int seq) {
        FileOp op;
        op.kind    = kind;
        const ScannedFile &f = scan.files[fileIdx];
        op.source  = f.abs;
        op.target  = target;
        op.reason  = reason;
        op.origin  = origin;
        op.decisionSeq   = seq;
        op.sourceSize    = f.size;
        op.sourceMtimeMs = f.mtimeMs;
        op.sourceHash    = f.hash;
        return op;
    };

    for (const PairDecision &d : std::as_const(decisions)) {
        const int ia = indexByPath.value(normKey(d.pathA), -1);
        const int ib = indexByPath.value(normKey(d.pathB), -1);
        if (ia < 0 || ib < 0) {
            log(QStringLiteral("[阶段二] 判定结果里有没扫到的文件,已忽略该对: %1 / %2")
                    .arg(d.pathA, d.pathB), Level::Warning);
            continue;
        }
        if (!scan.files[ia].fromA || scan.files[ib].fromA) {
            log(QStringLiteral("[阶段二] 图片对不是 A-B 跨根组合,已忽略: %1 / %2")
                    .arg(d.pathA, d.pathB), Level::Error);
            continue;
        }

        const QString origin = QStringLiteral("pair:") + scan.files[ia].abs;
        const QString targetAbs = joinPath(scan.rootB, scan.files[ia].rel);

        const PairState st = static_cast<PairState>(d.state);
        const quint8 own = ownRemovalBits(st, quint8(d.removedMask));

        /*
         * 复合态:失效对上"只删一侧"。
         *
         * state 会停在 Invalid*(这样列表还能点回肇事那一对),删除只记在
         * removedMask 里。以前这里只 switch(state),落到 default 就当"无操作",
         * 用户那次删除被静默丢掉。现在补上 —— 但只 Trash,不做归位:
         * 另一侧已经被别的对处理过,不可能再"保留并搬到 A 的路径"。
         */
        if (isInvalidPairState(st)) {
            if (own & SideBitA) {
                ops.push_back(makeOp(FileOp::Kind::Trash, ia, QString(),
                                     QStringLiteral("人工比较(失效对上移除 A)"),
                                     origin, d.seq));
                disp[ia] = Disp::Trash;
            }
            if (own & SideBitB) {
                ops.push_back(makeOp(FileOp::Kind::Trash, ib, QString(),
                                     QStringLiteral("人工比较(失效对上移除 B)"),
                                     origin, d.seq));
                disp[ib] = Disp::Trash;
            }
            continue;
        }

        switch (st) {
        case PairState::RemovedB:
            /*
             * 保留 A 版本:A 的文件【原地不动】,只删 B 的重复件。
             *
             * 以前这里会把 A 提前搬到 B 的目标路径(joinPath(rootB, relA)),
             * 等于替用户执行了"导入"这一步 —— 而 deduplicate import 的约定是
             * "程序只负责删重复件、移动 B 里的文件,最后的导入由用户自己做"
             * (用户之后会把整个 A 目录覆盖进去)。所以 A 侧一律不动。
             */
            ops.push_back(makeOp(FileOp::Kind::Trash, ib, QString(),
                                 QStringLiteral("人工比较(remove B):判定与 A 重复"),
                                 origin, d.seq));
            disp[ib] = Disp::Trash;
            disp[ia] = Disp::KeepInA;   // 留在 A,也不再参与"未匹配"规则
            break;

        case PairState::RemovedA:
            /* 保留 B 版本:B 的文件落到 A 的相对路径上,A 的重复件进回收站 */
            ops.push_back(makeOp(FileOp::Kind::Trash, ia, QString(),
                                 QStringLiteral("人工比较(remove A):判定与 B 重复"),
                                 origin, d.seq));
            disp[ia] = Disp::Trash;
            ops.push_back(makeOp(FileOp::Kind::Move, ib, targetAbs,
                                 QStringLiteral("人工比较(remove A):保留 B 版本并归位到 A 的相对路径"),
                                 origin, d.seq));
            disp[ib] = Disp::MoveToTarget;
            break;

        case PairState::RemovedBoth:
            ops.push_back(makeOp(FileOp::Kind::Trash, ia, QString(),
                                 QStringLiteral("人工比较(remove both)"),
                                 origin, d.seq));
            disp[ia] = Disp::Trash;
            ops.push_back(makeOp(FileOp::Kind::Trash, ib, QString(),
                                 QStringLiteral("人工比较(remove both)"),
                                 origin, d.seq));
            disp[ib] = Disp::Trash;
            break;

        default:
            /*
             * jump(跳过)/ 未处理的 Pending / Invalid*(文件已被别的对处理掉):
             * 这里不产生任何操作,两边都留给"未匹配"规则去决定。
             */
            break;
        }
    }
}

/* ==================================================================
 *  阶段三之一:未匹配文件(A 独有留 A;B 独有进 not_in_a)
 * ================================================================== */

void planUnmatchedPhase(const ScanResult &scan,
                        QVector<FileOp> &ops,
                        QVector<Disp> &disp,
                        const std::function<void(const QString &, Level)> &log)
{
    for (int i = 0; i < scan.files.size(); ++i) {
        if (disp[i] != Disp::Unhandled) continue;
        const ScannedFile &f = scan.files[i];

        if (f.fromA) {
            // A 独有:保留在 A,由用户之后自行决定要不要补进 B
            disp[i] = Disp::KeepInA;
            continue;
        }

        const QString target = ImportDedupeSession::notInATargetPath(f.abs, scan.rootB);
        if (normKey(target) == normKey(f.abs)) {
            // 已经在某个 not_in_a 目录里了:保持现有层级,不再嵌套
            disp[i] = Disp::KeepInA;
            log(QStringLiteral("[阶段三] B 独有文件已在 not_in_a 内,保持原位: %1")
                    .arg(f.abs), Level::Detail);
            continue;
        }

        FileOp op;
        op.kind    = FileOp::Kind::Move;
        op.source  = f.abs;
        op.target  = target;
        op.reason  = QStringLiteral("B 独有(在 A 中没有对应内容),归入 not_in_a");
        op.origin  = QStringLiteral("unmatched");
        op.sourceSize    = f.size;
        op.sourceMtimeMs = f.mtimeMs;
        op.sourceHash    = f.hash;
        ops.push_back(op);
        disp[i] = Disp::MoveToTarget;
    }
}

/* ==================================================================
 *  计划收尾:冲突裁决 + 目标路径统一分配
 * ================================================================== */

bool fileContentMatchesHash(const QString &path, const FileOp &op)
{
    QFileInfo fi(path);
    if (!fi.exists()) return false;
    if (op.sourceSize >= 0 && fi.size() != op.sourceSize) return false;
    if (op.sourceHash.size() != 32) return false;
    bool ok = false;
    const QByteArray h = dedup::fileSHA256(path, &ok);
    return ok && h == op.sourceHash;
}

/*
 * 把计划里的操作整理成最终形态:
 *   1) 同一个文件被多个来源安排成不同操作时,按 decisionSeq 取"最后一次意图";
 *      无法裁决(序号相同却互相矛盾)时,跳过该文件的全部破坏性操作并报错。
 *   2) 送达回收站的排在前面(它们会把目标位置腾出来)。
 *   3) 每个移动操作分配一个"现在不存在、计划里也没被预定"的目标名;
 *      真冲突(不同内容)按需求加 _num 后缀并把细节写进日志。
 */
bool finalizePlan(const ScanResult &scan,
                  QVector<FileOp> &ops,
                  const std::function<void(const QString &, Level)> &log)
{
    Q_UNUSED(scan);

    /* ---- 1) 按源文件聚合,解决互相矛盾的操作 ---- */
    QHash<QString, QVector<int>> bySource;
    for (int i = 0; i < ops.size(); ++i)
        bySource[normKey(ops[i].source)].push_back(i);

    QVector<bool> alive(ops.size(), true);
    for (auto it = bySource.constBegin(); it != bySource.constEnd(); ++it) {
        const QVector<int> &v = it.value();
        if (v.size() < 2) continue;

        // 完全相同的操作(同类型同目标)直接去重
        auto sameAs = [&ops](int i, int j) {
            return ops[i].kind == ops[j].kind
                   && normKey(ops[i].target) == normKey(ops[j].target);
        };

        int maxSeq = 0;
        for (int i : v) maxSeq = std::max(maxSeq, ops[i].decisionSeq);

        QVector<int> winners;
        for (int i : v) if (ops[i].decisionSeq == maxSeq) winners.push_back(i);

        bool allSame = true;
        for (int k = 1; k < winners.size(); ++k)
            if (!sameAs(winners[0], winners[k])) { allSame = false; break; }

        if (allSame) {
            /*
             * 同一个文件被多个 pair 判成了同一件事(典型:A1 同时和 B1、B2 配成对,
             * 用户两次都选 remove B)。只留一条,否则第二条会在执行阶段撞上
             * "源文件已经不在了"而白报一个错。
             */
            const int keep = winners.first();
            for (int i : v) alive[i] = false;
            alive[keep] = true;
            if (v.size() > 1) {
                log(QStringLiteral("[计划] 文件被重复登记为同一操作,已去重: %1")
                        .arg(ops[keep].source), Level::Detail);
            }
            continue;
        }

        // 真的矛盾:按最后一次操作裁决
        if (maxSeq > 0) {
            QStringList dropped;
            for (int i : v) {
                if (i == winners[0]) continue;
                dropped << QStringLiteral("%1(%2)")
                               .arg(ops[i].kind == FileOp::Kind::Trash
                                        ? QStringLiteral("回收站")
                                        : QStringLiteral("移动到 ") + ops[i].target,
                                    ops[i].reason);
            }
            const QString keptText =
                (ops[winners[0]].kind == FileOp::Kind::Trash
                     ? QStringLiteral("回收站")
                     : QStringLiteral("移动到 ") + ops[winners[0]].target)
                + QStringLiteral("(%1)").arg(ops[winners[0]].reason);
            log(QStringLiteral("[计划] 冲突裁决: %1 同时被安排了多个操作 [%2];"
                               "按用户最后一次操作(序号 %3)保留 %4,其余忽略")
                    .arg(ops[winners[0]].source, dropped.join(QStringLiteral(" / ")))
                    .arg(maxSeq).arg(keptText),
                Level::Warning);
            for (int i : v) alive[i] = false;
            alive[winners[0]] = true;
        } else {
            log(QStringLiteral("[计划] 存在无法裁决的互相矛盾操作,已跳过该文件的全部破坏性操作: %1")
                    .arg(ops[winners[0]].source), Level::Error);
            for (int i : v) alive[i] = false;
        }
    }

    QVector<FileOp> keptOps;
    for (int i = 0; i < ops.size(); ++i)
        if (alive[i]) keptOps.push_back(ops[i]);

    /* ---- 2) 回收站操作排在前面 ---- */
    std::stable_sort(keptOps.begin(), keptOps.end(), [](const FileOp &a, const FileOp &b) {
        if (a.kind != b.kind) return a.kind == FileOp::Kind::Trash;
        return normKey(a.source) < normKey(b.source);
    });

    /* ---- 3) 移动操作的目标分配 ---- */
    QSet<QString> vacated;          // 所有会被腾空的路径
    for (const FileOp &op : std::as_const(keptOps))
        vacated.insert(normKey(op.source));

    QSet<QString> reserved;         // 计划已经点名的目标
    QVector<FileOp> finalOps;
    finalOps.reserve(keptOps.size());

    for (const FileOp &rawOp : std::as_const(keptOps)) {
        FileOp op = rawOp;

        if (op.kind == FileOp::Kind::Trash) {
            finalOps.push_back(op);
            continue;
        }

        // 已经在目标位置上:不重复移动
        if (normKey(op.source) == normKey(op.target)) {
            log(QStringLiteral("[计划] 已在目标位置,跳过移动: %1").arg(op.source), Level::Detail);
            continue;
        }

        const QString targetKey = normKey(op.target);
        const bool onDisk = QFileInfo::exists(op.target);
        const bool taken  = reserved.contains(targetKey);

        if (!onDisk && !taken) {
            reserved.insert(targetKey);
            finalOps.push_back(op);
            continue;
        }

        // 目标被"计划内会被移走"的文件占着:等它腾地方即可,不必改名
        if (onDisk && !taken && vacated.contains(targetKey)
            && targetKey != normKey(op.source)) {
            reserved.insert(targetKey);
            log(QStringLiteral("[计划] 目标当前被占,但占位文件也在本次计划里被移走,按顺序执行: %1")
                    .arg(op.target), Level::Detail);
            finalOps.push_back(op);
            continue;
        }

        // 目标已经有完全相同的内容:不制造第二份,把源文件按重复副本处理
        if (onDisk && !taken && fileContentMatchesHash(op.target, op)) {
            log(QStringLiteral("[计划] 目标位置已存在相同内容,源文件按重复副本处理(回收站): %1 -> %2")
                    .arg(op.source, op.target), Level::Warning);
            op.kind   = FileOp::Kind::Trash;
            op.reason += QStringLiteral(" (目标已存在相同内容,按重复副本处理)");
            op.target.clear();
            finalOps.push_back(op);
            continue;
        }

        // 真冲突:加 _num 后缀,保留扩展名
        QString cand;
        int n = 1;
        do {
            cand = withSuffixNumber(op.target, n++);
        } while (QFileInfo::exists(cand) || reserved.contains(normKey(cand)));

        const QString conflictReason =
            taken ? QStringLiteral("计划内另一个任务已经点名了这个目标路径")
                  : QStringLiteral("目标路径已被不同内容的文件占据");
        log(QStringLiteral("[计划] 路径冲突: 源=%1 | 原计划目标=%2 | 冲突=%3 | 实际采用=%4 | 原因=%5")
                .arg(op.source, op.target,
                     taken ? QStringLiteral("(计划内已预定)") : op.target,
                     cand, conflictReason),
            Level::Warning);

        op.target = cand;
        reserved.insert(normKey(cand));
        finalOps.push_back(op);
    }

    ops = finalOps;
    return true;
}

/* ==================================================================
 *  执行:回收站 / 移动(含跨盘安全复制)
 * ================================================================== */

/*
 * 执行前核对源文件:必须还在、大小没变、内容没变。
 * 计划到执行之间用户可能手动改过文件(尤其是阶段一之后隔了很久才关窗),
 * 这时候"它还是不是原来那个重复文件"就不确定了 —— 跳过比乱搬安全。
 */
bool verifySourceUnchanged(const FileOp &op, QString *err)
{
    QFileInfo fi(op.source);
    if (!fi.exists()) {
        *err = QStringLiteral("源文件不存在(可能已被外部程序移动或删除)");
        return false;
    }
    if (op.sourceSize >= 0 && fi.size() != op.sourceSize) {
        *err = QStringLiteral("文件大小已变化(计划时 %1 字节,现在 %2 字节)")
                   .arg(op.sourceSize).arg(fi.size());
        return false;
    }
    const qint64 nowMtime = fi.lastModified().toMSecsSinceEpoch();
    if (op.sourceMtimeMs > 0 && nowMtime != op.sourceMtimeMs) {
        if (op.sourceHash.size() == 32) {
            bool ok = false;
            const QByteArray h = dedup::fileSHA256(op.source, &ok);
            if (!ok || h != op.sourceHash) {
                *err = QStringLiteral("文件内容已变化(修改时间与内容都与计划时不一致)");
                return false;
            }
            // 只是时间戳被摸过,内容一致 -> 继续
        } else {
            *err = QStringLiteral("文件修改时间已变化(计划时 %1,现在 %2)")
                       .arg(op.sourceMtimeMs).arg(nowMtime);
            return false;
        }
    }
    return true;
}

/*
 * 把文件从 from 移到 to。
 *
 * 同卷:优先 QFile::rename(原子、零拷贝)。
 *   rename 失败(被占用 / 跨卷挂载点 / 路径过长等)时不直接报错,而是退到
 *   "复制 + 校验 + 删源"的安全流程 —— 关键是【目标副本没有校验成功之前绝不删源】。
 */
bool moveFileSafely(const QString &from, const QString &to,
                    QString *err, QStringList *notes, bool *usedCopyFallback = nullptr)
{
    if (usedCopyFallback) *usedCopyFallback = false;
    if (normKey(from) == normKey(to)) return true;

    if (!QFile::exists(from)) {
        *err = QStringLiteral("源文件不存在: %1").arg(from);
        return false;
    }

    const QString toDir = QFileInfo(to).absolutePath();
    if (!QDir().mkpath(toDir)) {
        *err = QStringLiteral("无法创建目标目录: %1").arg(toDir);
        return false;
    }
    if (QFileInfo::exists(to)) {
        *err = QStringLiteral("目标已存在,拒绝覆盖: %1").arg(to);
        return false;
    }

    if (volumeKeyOf(from) == volumeKeyOf(to) && QFile::rename(from, to))
        return true;

    /* ---- 安全复制路径(跨卷 / rename 失败) ---- */
    if (usedCopyFallback) *usedCopyFallback = true;
    const QString tmp = to + QStringLiteral(".importdedupe.tmp");
    if (QFileInfo::exists(tmp) && !QFile::remove(tmp)) {
        *err = QStringLiteral("无法清理残留的临时文件: %1").arg(tmp);
        return false;
    }

    {
        QFile in(from), out(tmp);
        if (!in.open(QIODevice::ReadOnly)) {
            *err = QStringLiteral("无法读取源文件: %1 (%2)").arg(from, in.errorString());
            return false;
        }
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            *err = QStringLiteral("无法创建临时文件: %1 (%2)").arg(tmp, out.errorString());
            return false;
        }
        constexpr qint64 kChunk = 4 << 20;
        while (!in.atEnd()) {
            const QByteArray buf = in.read(kChunk);
            if (buf.isEmpty()) break;
            if (out.write(buf) != buf.size()) {
                *err = QStringLiteral("写入临时文件失败(磁盘空间不足?): %1 (%2)")
                           .arg(tmp, out.errorString());
                out.close();
                QFile::remove(tmp);
                return false;
            }
        }
        out.flush();
        out.close();
        in.close();
        if (in.error() != QFileDevice::NoError) {
            *err = QStringLiteral("读取源文件出错: %1 (%2)").arg(from, in.errorString());
            QFile::remove(tmp);
            return false;
        }
    }

    // 校验:大小 + 内容哈希都要一致,才允许删源
    {
        QFileInfo tf(tmp), sf(from);
        if (tf.size() != sf.size()) {
            *err = QStringLiteral("复制校验失败(大小不一致): %1").arg(tmp);
            QFile::remove(tmp);
            return false;
        }
    }
    {
        bool ok1 = false, ok2 = false;
        const QByteArray h1 = dedup::fileSHA256(from, &ok1);
        const QByteArray h2 = dedup::fileSHA256(tmp, &ok2);
        if (!ok1 || !ok2 || h1 != h2) {
            *err = QStringLiteral("复制校验失败(内容不一致): %1").arg(tmp);
            QFile::remove(tmp);
            return false;
        }
    }

    if (!QFile::rename(tmp, to)) {
        *err = QStringLiteral("临时文件改名到最终目标失败: %1 -> %2").arg(tmp, to);
        QFile::remove(tmp);
        return false;
    }

    if (!QFile::remove(from)) {
        // 目标副本已经建好并且校验过了,数据是安全的;源没删掉只是留下一个副本
        if (notes) {
            *notes << QStringLiteral("目标副本已就绪但源文件删除失败,请手工处理: %1").arg(from);
        }
    }
    return true;
}

/* ==================================================================
 *  JSONL 操作日志(计划 + 每条操作的执行状态)
 * ================================================================== */

class PlanLogWriter
{
public:
    bool open(const QString &path, QString *err)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        m_file.setFileName(path);
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            if (err) *err = m_file.errorString();
            return false;
        }
        m_open = true;
        return true;
    }

    void writeLine(const QJsonObject &obj)
    {
        if (!m_open) return;
        const QByteArray line = QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";
        m_file.write(line);
        m_file.flush();
    }

    void close() { if (m_open) { m_file.close(); m_open = false; } }

private:
    QFile m_file;
    bool  m_open = false;
};

/*
 * 执行整份计划。返回 (成功数, 跳过数, 失败数)。
 * 单条失败不影响其它互不依赖的操作 —— 逐个记录、逐个继续。
 */
void executeOps(const QVector<FileOp> &ops,
                PlanLogWriter &planLog,
                int &okCount, int &skipCount, int &failCount,
                const std::function<void(const QString &, Level)> &log)
{
    okCount = skipCount = failCount = 0;

    /* 计划里所有当源用的路径(用于判断"目标被占是不是暂时的") */
    QSet<QString> planSources;
    for (const FileOp &op : ops) planSources.insert(normKey(op.source));
    QSet<QString> vacated;

    auto record = [&](int index, const FileOp &op, const QString &status,
                      const QString &detail) {
        QJsonObject o;
        o.insert(QStringLiteral("type"), QStringLiteral("result"));
        o.insert(QStringLiteral("i"), index);
        o.insert(QStringLiteral("kind"),
                 op.kind == FileOp::Kind::Trash ? QStringLiteral("trash") : QStringLiteral("move"));
        o.insert(QStringLiteral("source"), op.source);
        o.insert(QStringLiteral("target"), op.target);
        o.insert(QStringLiteral("status"), status);
        o.insert(QStringLiteral("detail"), detail);
        o.insert(QStringLiteral("time"), tsStr());
        planLog.writeLine(o);
    };

    /* ---------- 第一步:回收站(先把位置腾出来) ---------- */
    for (int i = 0; i < ops.size(); ++i) {
        const FileOp &op = ops[i];
        if (op.kind != FileOp::Kind::Trash) continue;

        QString err;
        if (!verifySourceUnchanged(op, &err)) {
            ++skipCount;
            record(i, op, QStringLiteral("skipped"), err);
            log(QStringLiteral("[执行] 跳过(安全检查未通过) %1 | 原因: %2 | 原计划: %3")
                    .arg(op.source, err, op.reason), Level::Error);
            continue;
        }

        if (dedupenv::moveToTrash(op.source)) {
            ++okCount;
            vacated.insert(normKey(op.source));
            record(i, op, QStringLiteral("ok"), QStringLiteral("已移入系统回收站"));
            log(QStringLiteral("[执行] 已移入系统回收站: %1 | %2")
                    .arg(op.source, op.reason), Level::Detail);
        } else {
            ++failCount;
            record(i, op, QStringLiteral("failed"),
                   QStringLiteral("系统回收站操作失败(不退化为永久删除)"));
            log(QStringLiteral("[执行] 回收站操作失败,源文件保持原样: %1 | 原因: %2")
                    .arg(op.source, op.reason), Level::Error);
        }
    }

    /* ---------- 第二步:移动(需要等待目标位置腾空时延后一轮) ---------- */
    QVector<int> pending;
    for (int i = 0; i < ops.size(); ++i)
        if (ops[i].kind == FileOp::Kind::Move) pending.push_back(i);

    auto doMove = [&](int index, FileOp op, bool allowResuffix) -> bool {
        QString err;
        QStringList notes;
        bool usedCopyFallback = false;
        if (!verifySourceUnchanged(op, &err)) {
            ++skipCount;
            record(index, op, QStringLiteral("skipped"), err);
            log(QStringLiteral("[执行] 跳过(安全检查未通过) %1 | 原因: %2 | 原计划目标: %3")
                    .arg(op.source, err, op.target), Level::Error);
            return true;   // 这一条已经处理完(跳过),继续别的
        }

        if (QFileInfo::exists(op.target)) {
            if (allowResuffix) {
                // 执行期才发现的冲突(计划时没算到):按需求补 _num 后缀
                QString cand;
                int n = 1;
                do { cand = withSuffixNumber(op.target, n++); }
                while (QFileInfo::exists(cand));
                log(QStringLiteral("[执行] 目标在计划后被占用,改用安全替代名: 源=%1 | 原目标=%2 | 采用=%3")
                        .arg(op.source, op.target, cand), Level::Warning);
                op.target = cand;
            } else {
                return false;   // 交给下一轮(等占位文件被搬走)
            }
        }

        if (moveFileSafely(op.source, op.target, &err, &notes, &usedCopyFallback)) {
            ++okCount;
            vacated.insert(normKey(op.source));
            record(index, op, QStringLiteral("ok"),
                   usedCopyFallback ? QStringLiteral("已移动(跨卷:复制+校验后删除源文件)")
                                    : QStringLiteral("已移动"));
            log(QStringLiteral("[执行] 已移动: %1 -> %2 | %3%4")
                    .arg(op.source, op.target, op.reason,
                         usedCopyFallback
                             ? QStringLiteral(" [跨卷:先复制并校验,确认目标完整后才删源]")
                             : QString()),
                Level::Detail);
            for (const QString &nt : std::as_const(notes))
                log(QStringLiteral("[执行] %1").arg(nt), Level::Warning);
        } else {
            ++failCount;
            record(index, op, QStringLiteral("failed"), err);
            log(QStringLiteral("[执行] 移动失败,源文件保持原样: %1 -> %2 | 原因: %3")
                    .arg(op.source, op.target, err), Level::Error);
        }
        return true;
    };

    for (int round = 0; !pending.isEmpty(); ++round) {
        QVector<int> stillPending;
        bool progress = false;

        for (int index : std::as_const(pending)) {
            const FileOp &op = ops[index];

            // 目标上还压着一个"本次计划里迟早要被搬走"的文件:等它先走
            const QString tKey = normKey(op.target);
            if (QFileInfo::exists(op.target)
                && planSources.contains(tKey) && !vacated.contains(tKey)) {
                stillPending.push_back(index);
                continue;
            }

            if (doMove(index, op, /*allowResuffix=*/round > 0))
                progress = true;
            else
                stillPending.push_back(index);
        }

        if (!progress && !stillPending.isEmpty()) {
            // 互相等待(理论上不会发生):最后兜底,允许改后缀,避免死循环
            for (int index : std::as_const(stillPending))
                doMove(index, ops[index], /*allowResuffix=*/true);
            pending.clear();
            break;
        }
        pending = stillPending;
    }
}

} // namespace

/* ==================================================================
 *                       ImportDedupeSession
 * ================================================================== */

namespace importdedupe {

ImportDedupeSession::ImportDedupeSession(QObject *parent)
    : QObject(parent)
{
}

ImportDedupeSession::~ImportDedupeSession()
{
    /*
     * 退出时:
     *   - 让后台线程尽快看到取消标记并收尾(join 保证不会再有线程碰本对象);
     *   - 把窗口上的关窗钩子摘掉 —— 它捕获了 this,本对象没了以后不能再被调用;
     *   - 绝对不在这里执行计划:用户还没"关窗确认",而且日志窗口可能已经先被销毁,
     *     悄悄改磁盘比"什么都不做"危险得多。
     */
    m_cancel.store(true);
    joinThreads();
    if (m_window) m_window->setImportCloseHandler(nullptr);
}

void ImportDedupeSession::joinThreads()
{
    if (m_scanThread.joinable()) m_scanThread.join();
    if (m_execThread.joinable()) m_execThread.join();
}

void ImportDedupeSession::log(const QString &line, Level level)
{
    /*
     * 先落盘再发信号:信号是排队投递的,万一接收方(UI)正卡在别处,
     * 至少文本日志里已经能看到"走到哪一步了"。
     */
    writeSessionLog(line);
    emit logMessage(line, static_cast<int>(level));
}

void ImportDedupeSession::openSessionLog()
{
    const QString base = ::dedupenv::logDir();
    if (base.isEmpty()) {
        m_sessionLogPath.clear();
        return;
    }

    const QString dir = base + QStringLiteral("/import_dedupe");
    if (!QDir().mkpath(dir)) {
        m_sessionLogPath.clear();
        return;
    }

    /*
     * 文件名带毫秒:同一秒内连点两次也不会互相覆盖(排查卡死时两份日志混在
     * 一起比没有日志更糟)。
     */
    m_sessionLogPath = dir + QStringLiteral("/import_")
                       + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz")
                       + QStringLiteral(".log");

    std::lock_guard<std::mutex> lk(m_sessionLogMutex);
    QFile f(m_sessionLogPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        m_sessionLogPath.clear();
        return;
    }
    f.write("gallery_info import-dedupe session log\n");
}

void ImportDedupeSession::writeSessionLog(const QString &line)
{
    if (m_sessionLogPath.isEmpty())
        return;

    std::lock_guard<std::mutex> lk(m_sessionLogMutex);
    if (m_sessionLogPath.isEmpty())
        return;

    QFile f(m_sessionLogPath);
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return;
    const QByteArray bytes = (QDateTime::currentDateTime().toString("HH:mm:ss.zzz ")
                              + line + QLatin1Char('\n')).toUtf8();
    f.write(bytes);
    f.flush();   // 卡死时进程可能被强杀,不缓冲
}

void ImportDedupeSession::hopToUi(const std::function<void()> &fn)
{
    QMetaObject::invokeMethod(this, [fn]() { fn(); }, Qt::QueuedConnection);
}

QString ImportDedupeSession::naturalKey(const QString &text)
{
    /*
     * 与 dedupe_move_to_trash.py 的 natural_key 同语义:
     * 把字符串切成"数字段 / 非数字段",数字按数值比较(所以 a2 < a10),
     * 同一位置上数字段排在文本段前面(对应 Python 的 (0,int) / (1,str))。
     *
     * 实现:数字段零填充成固定宽度,整体拼成一个可以按字典序比较的字符串 ——
     * '0' + 数字 与 '1' + 文本,前缀字符保证"数字在前"。
     * 这样排序确定、可复现,不必真的造一棵比较树。
     */
    static const QRegularExpression re(QStringLiteral("(\\d+)"));
    QString key;
    int last = 0;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const int pos = m.capturedStart();
        if (pos > last)
            key += QLatin1Char('1') + text.mid(last, pos - last).toLower();
        key += QLatin1Char('0') + m.captured(1).rightJustified(20, QLatin1Char('0'));
        last = m.capturedEnd();
    }
    if (last < text.size())
        key += QLatin1Char('1') + text.mid(last).toLower();
    return key;
}

QString ImportDedupeSession::notInATargetPath(const QString &bFilePath, const QString &rootB)
{
    const QFileInfo fi(bFilePath);
    if (pathHasComponent(fi.absolutePath(), QStringLiteral("not_in_a")))
        return fi.absoluteFilePath();       // 已经在 not_in_a 里:不再嵌套

    /*
     * 统一放到 <B根>/not_in_a/<相对 B 根的路径>,而不是"就近创建"。
     *
     * 以前是 <文件所在目录>/not_in_a/<文件名>,于是同一个作品里会在各个子目录
     * 各生成一个 not_in_a,既碎又没法整体导入(还得先把它们收拢到一处)。
     * 现在所有"B 独有"的文件都集中在 B 根的一个 not_in_a 下,并保留原来的相对
     * 路径 —— 结构信息不丢,用户之后要人工处理时也知道它原本属于哪个作品。
     */
    const QString root = QDir::cleanPath(QDir::toNativeSeparators(rootB));
    if (root.isEmpty() || !QDir(root).exists())
        return fi.absoluteFilePath();

    QString rel = QDir(root).relativeFilePath(fi.absoluteFilePath());
    rel.replace('\\', '/');
    if (rel.isEmpty() || rel == QLatin1String(".")
        || rel.startsWith(QLatin1String("../"))) {
        // 不在 B 根之下(理论上不会发生):原位不动,别把它搬到莫名其妙的地方
        return fi.absoluteFilePath();
    }

    return QDir(root).filePath(QStringLiteral("not_in_a/") + rel);
}

void ImportDedupeSession::start(const QString &rootA, const QString &rootB,
                                ImageDeduplicateWindow *window)
{
    /*
     * 上一次会话可能是个"死会话"(后台线程早就退了 / 窗口也早就不在它手里),
     * 但 m_phase 还挂在非 Idle 上。先自检复位,否则用户会一直看到
     * "上一次导入整理还没结束",而窗口里一个任务都没有 —— 只能重启程序。
     */
    if (recoverStaleSession())
        log(QStringLiteral("检测到上一次导入整理已经没有任何后台线程/窗口在推进,已自动复位"),
            Level::Warning);

    if (isActive()) {
        log(QStringLiteral("已有导入整理任务在进行中,请等它结束后再试"), Level::Warning);
        return;
    }

    openSessionLog();

    m_rootA = QDir::cleanPath(QDir::toNativeSeparators(rootA));
    m_rootB = QDir::cleanPath(QDir::toNativeSeparators(rootB));
    m_window = window;

    m_cancel.store(false);
    m_scanResultPending.store(false);
    m_windowSessionSerial = 0;
    m_lastMoves.store(0);
    m_lastTrashes.store(0);
    m_lastOk.store(0);
    m_lastSkipped.store(0);
    m_lastFailed.store(0);
    m_lastPlanLogPath.clear();

    auto data = std::make_shared<SessionData>();
    data->rootA = m_rootA;
    data->rootB = m_rootB;
    data->rootALower = normKey(m_rootA);
    data->rootBLower = normKey(m_rootB);
    m_data = data;

    m_phase = Phase::Scanning;
    log(QStringLiteral("==== 导入整理开始 ===="), Level::Info);
    log(QStringLiteral("Path A(结构基准)= %1").arg(m_rootA), Level::Info);
    log(QStringLiteral("Path B(来源/整理目标)= %1").arg(m_rootB), Level::Info);
    if (!m_sessionLogPath.isEmpty())
        log(QStringLiteral("会话日志: %1").arg(QDir::toNativeSeparators(m_sessionLogPath)),
            Level::Detail);
    startScan();
}

/* ---------------- 阶段一 ---------------- */

void ImportDedupeSession::startScan()
{
    joinThreads();
    auto data = m_data;

    /*
     * 先置"存活"再起线程。
     *
     * 反过来的话,std::thread 构造返回到线程真正开始执行之间有一个极小的窗口,
     * 这期间 m_scanRunning 还是 false —— 如果用户正好又点了一次导入整理,
     * recoverStaleSession() 会把"刚起步"误判成死会话。窗口极小,但代价是
     * 一次正在跑的扫描被掐掉,不值得赌。
     */
    m_scanRunning.store(true, std::memory_order_relaxed);
    try {
        m_scanThread = std::thread([this, data]() {
            /* 出口复位(正常返回 / 异常展开都走析构),见 recoverStaleSession() */
            struct ClearOnExit {
                std::atomic<bool> &flag;
                explicit ClearOnExit(std::atomic<bool> &f) : flag(f) {}
                ~ClearOnExit() { flag.store(false); }
            } running(m_scanRunning);
            scanThreadMain(data);
        });
    } catch (...) {
        m_scanRunning.store(false);
        throw;
    }
}

void ImportDedupeSession::abortScanOnUi()
{
    hopToUi([this]() {
        if (m_phase != Phase::Scanning)
            return;                       // 已经被 abortSession / finishSession 收过
        log(QStringLiteral("[阶段一] 扫描被取消,本次导入整理结束"), Level::Warning);
        finishSession(false, QStringLiteral("二进制扫描被取消"));
    });
}

void ImportDedupeSession::scanThreadMain(std::shared_ptr<SessionData> data)
{
    auto logFn = [this](const QString &line, Level lv) { log(line, lv); };

    try {
        auto res = std::make_shared<ScanResult>();
        res->rootA = data->rootA;
        res->rootB = data->rootB;

        collectRootFiles(data->rootA, true, *res, logFn, m_cancel);
        if (m_cancel.load()) { abortScanOnUi(); return; }
        collectRootFiles(data->rootB, false, *res, logFn, m_cancel);
        if (m_cancel.load()) { abortScanOnUi(); return; }

        buildHashGroups(*res, logFn, m_cancel);
        if (m_cancel.load()) { abortScanOnUi(); return; }

        const int nA = static_cast<int>(std::count_if(
            res->files.cbegin(), res->files.cend(),
            [](const ScannedFile &f) { return f.fromA; }));

        /* 阶段一的分组规则 -> 操作 + 处置 */
        QVector<FileOp> ops;
        QVector<Disp> disp(res->files.size(), Disp::Unhandled);
        planBinaryPhase(*res, ops, disp, logFn);
        if (m_cancel.load()) { abortScanOnUi(); return; }

        /*
         * 自检(需求 7.2 里那条"同哈希副本有没有漏掉"):一个内容相同的组
         * 最后只允许留下一份实体 —— 除了进回收站的那些,其余成员的处置
         * (保留在 A / 搬到目标 / 已在目标)都意味着还会留下一份。
         * 数量不是 1 就说明规则算错了,必须报出来(但不动手改,由用户判断)。
         */
        for (const QVector<int> &g : std::as_const(res->groups)) {
            int survivors = 0;
            for (int idx : g)
                if (disp[idx] != Disp::Trash) ++survivors;
            if (survivors != 1) {
                QStringList paths;
                for (int idx : g) paths << res->files[idx].abs;
                logFn(QStringLiteral("[自检] 同哈希组 %1 最终会留下 %2 份实体(应为 1): %3")
                          .arg(QString::fromLatin1(res->files[g.first()].hash.left(8).toHex()))
                          .arg(survivors)
                          .arg(paths.join(QStringLiteral(" | "))),
                      Level::Error);
            }
        }

        /* 视觉比较候选:阶段一已经安排掉的文件不再进窗口 */
        QSet<QString> excluded;
        for (int i = 0; i < res->files.size(); ++i) {
            if (disp[i] == Disp::Trash || disp[i] == Disp::MoveToTarget
                || disp[i] == Disp::InPlaceAtTarget) {
                excluded.insert(normKey(res->files[i].abs));
            }
        }

        QStringList candA, candB;
        for (int i = 0; i < res->files.size(); ++i) {
            if (excluded.contains(normKey(res->files[i].abs))) continue;
            if (!isImageFile(res->files[i].abs)) continue;      // 非图片不参与视觉比较
            (res->files[i].fromA ? candA : candB) << res->files[i].abs;
        }

        logFn(QStringLiteral("[阶段一] 二进制阶段安排: 计划操作 %1 条;"
                             "需要人工比较的图片 A=%2 张 / B=%3 张")
                  .arg(ops.size()).arg(candA.size()).arg(candB.size()),
              Level::Info);
        logFn(QStringLiteral("[阶段一] A 共 %1 个文件,B 共 %2 个文件(跳过符号链接 %3、"
                             "隔离目录 %4、特征缓存 %5、读取失败 %6)")
                  .arg(nA).arg(res->files.size() - nA)
                  .arg(res->skippedSymlinks).arg(res->skippedQuarantine)
                  .arg(res->skippedCacheFiles).arg(res->hashFailures),
              Level::Detail);

        data->scan        = res;
        data->binaryOps   = ops;
        data->disp        = disp;
        data->excludedLower = excluded;
        data->candidateA  = candA;
        data->candidateB  = candB;

        m_scanResultPending.store(true, std::memory_order_relaxed);
        hopToUi([this, data]() { onScanFinished(data); });
    } catch (const std::exception &e) {
        log(QStringLiteral("[阶段一] 扫描时发生异常: %1").arg(QString::fromUtf8(e.what())),
            Level::Error);
        hopToUi([this]() {
            finishSession(false, QStringLiteral("二进制扫描失败(异常)"));
        });
    } catch (...) {
        log(QStringLiteral("[阶段一] 扫描时发生未知异常"), Level::Error);
        hopToUi([this]() {
            finishSession(false, QStringLiteral("二进制扫描失败(未知异常)"));
        });
    }
}

void ImportDedupeSession::onScanFinished(std::shared_ptr<SessionData> data)
{
    /* 结果已经送到 UI,自检不再需要把它当成"投递中" */
    m_scanResultPending.store(false, std::memory_order_relaxed);
    if (m_cancel.load()) return;
    /*
     * 会话可能已经被自检/放弃复位掉了(phase 不再是 Scanning)。
     * 这时 scan 结果已经没有主人,继续往下走会莫名其妙地把窗口打开。
     */
    if (m_phase != Phase::Scanning) return;

    /*
     * 阶段一的结果已经出来了。视觉比较候选为空时没必要开窗口打扰用户
     * (所有内容都已经由二进制阶段安排好了),直接执行计划。
     *
     * 注意这里必须是【或】而不是【与】:
     *   窗口里 A/B 两组的 internal 都是 None(组内不比较),只会建 A<->B 的跨组对。
     *   所以只要有一侧候选为空,就一对都建不出来 —— 窗口会扫一遍两个根目录、
     *   把另一侧所有候选图的 pHash/dHash/ORB 全算完,然后显示 0 对。
     *
     * 实际踩到的例子:A 里 86 张全部和 B 二进制一致(A 侧全进回收站,candA=0),
     * B 侧还有一批"A 里没有"的图(candB=若干)-> 旧条件判成"要比较" ->
     * 窗口把 B 的图整套特征重算一遍,一条决策都用不上。
     */
    const bool noPairPossible = data->candidateA.isEmpty() || data->candidateB.isEmpty();

    if (!m_window || noPairPossible) {
        if (!m_window && !noPairPossible) {
            log(QStringLiteral("ImageDeduplicateWindow 未就绪,无法进行人工比较;"
                               "本次只执行二进制阶段的整理计划"), Level::Warning);
        } else if (!noPairPossible) {
            log(QStringLiteral("[阶段二] 没有需要人工比较的图片,直接执行整理计划"), Level::Info);
        } else {
            log(QStringLiteral("[阶段二] 不需要人工比较(A=%1 张 / B=%2 张,"
                               "只有一侧有候选,建不出跨组对),直接执行整理计划")
                    .arg(data->candidateA.size()).arg(data->candidateB.size()),
                Level::Info);
        }
        executeOnly();
        return;
    }

    openWindowForUser(data);
}

void ImportDedupeSession::openWindowForUser(std::shared_ptr<SessionData> data)
{
    m_phase = Phase::AwaitingUser;

    /*
     * 窗口是 main() 里创建的栈对象,可能比本会话先被销毁(main 的栈展开顺序)。
     * 用 destroyed 信号把指针置空,后面所有对 m_window 的使用都先判空。
     */
    ImageDeduplicateWindow *win = m_window;
    connect(win, &QObject::destroyed, this, [this]() { m_window = nullptr; });

    // 窗口关闭 = 本次会话的判定到此为止 -> 收尾并执行计划
    // (先断开上一次会话可能留下的连接,避免一个窗口多次会话时收到多份通知)
    disconnect(win, &ImageDeduplicateWindow::importSessionClosed, this, nullptr);
    connect(win, &ImageDeduplicateWindow::importSessionClosed, this,
            [this]() { onWindowClosed(true); });

    // 关窗询问里选了"放弃":丢弃计划,一条都不执行
    disconnect(win, &ImageDeduplicateWindow::importSessionAborted, this, nullptr);
    connect(win, &ImageDeduplicateWindow::importSessionAborted, this,
            [this]() { abortSession(); });

    /*
     * 关窗确认:三个选项。
     *   - 执行整理:按当前结果(含二进制阶段)执行;
     *   - 继续比较:留在窗口里;
     *   - 放弃本次整理:什么都不执行,磁盘保持原样 —— 包括二进制阶段已经判定为
     *     "内容完全相同"的那些文件。它们同样只是计划里的 Trash/Move 条目,
     *     真正的磁盘操作从来只在关了窗之后统一跑。
     */
    m_window->setImportCloseHandler([this]() -> ImageDeduplicateWindow::ImportCloseResult {
        using Result = ImageDeduplicateWindow::ImportCloseResult;

        // 无人值守 / 测试:不问,直接按预设走
        switch (static_cast<ClosePrompt>(m_closePrompt.load())) {
        case ClosePrompt::Execute: return Result::Execute;
        case ClosePrompt::Abort:   return Result::AbortSession;
        case ClosePrompt::Ask:     break;
        }

        QMessageBox box(m_window);
        box.setWindowTitle(QStringLiteral("导入整理"));
        box.setIcon(QMessageBox::Question);
        box.setText(QStringLiteral("关闭窗口后如何处理本次整理?"));
        box.setInformativeText(QStringLiteral(
            "执行整理:\n"
            "  - 二进制阶段与人工判定的结果会立刻生效\n"
            "  - 仍未处理的图片按\"未匹配\"处理(A 侧留在 A,B 侧移入 not_in_a)\n"
            "  - 所有删除都是移到系统回收站,不会永久删除\n\n"
            "放弃本次整理:\n"
            "  - 一条文件操作都不执行(A 侧的二进制重复、B 侧的 not_in_a 移动都取消)\n"
            "  - 磁盘保持当前状态,窗口里的判定一并丢弃"));
        QPushButton *go = box.addButton(QStringLiteral("执行整理"), QMessageBox::AcceptRole);
        QPushButton *abort = box.addButton(QStringLiteral("放弃本次整理"),
                                           QMessageBox::DestructiveRole);
        box.addButton(QStringLiteral("继续比较"), QMessageBox::RejectRole);
        box.setDefaultButton(go);
        box.exec();

        if (box.clickedButton() == go)    return Result::Execute;
        if (box.clickedButton() == abort) return Result::AbortSession;
        return Result::KeepOpen;
    });

    // 候选目录:只扫这一段,而且用排除谓词把阶段一已安排掉的文件挡在外面
    struct ImportFolderOption {
        QString folder;
        QString regexFilter;
        int     internal = 0;      // InternalDedup::None:同一目录内部不比较(只做 A-B 跨根比较)
        bool    cache    = false;
        bool    reuseCache = false;
        std::function<bool(const QString &)> fileFilter;
    };

    QSet<QString> excluded = data->excludedLower;
    QVector<ImportFolderOption> folders;
    ImportFolderOption fa;
    fa.folder = data->rootA;
    fa.internal = 0;
    /*
     * 读缓存 + 写缓存。
     *
     * 读:这两个根目录里通常已经有 image_features.tscch(普通去重、"cache image
     *     features"按钮、半选去重都会写),里面的 pHash/dHash/ORB 全是现成的。
     *     不读的话,窗口每次都要把候选目录里所有图片完整解码一遍 ——
     *     实测 20 张普通 jpg 就是 8.5s 对 15ms。
     *
     * 写:以前是"只读不写",理由是关窗后大量文件会被搬走/回收,写下的记录
     *     马上就指向不存在的路径。现在这条理由不成立了 ——
     *     scanFolders 已经能靠 SHA-256 把"丢了记录的旧路径"和"新增文件"配对,
     *     文件被搬走之后特征会跟着记录搬到新位置(见"移位复用")。
     *     所以算出来的东西就该存下来,否则下次还得再解码一遍。
     *     写回是 mergeOnly(这一趟带 fileFilter,只覆盖候选子集),不会剔除别人的记录。
     */
    fa.cache = true;
    fa.reuseCache = true;
    fa.fileFilter = [excluded](const QString &p) { return excluded.contains(normKey(p)); };
    folders.push_back(fa);

    ImportFolderOption fb = fa;
    fb.folder = data->rootB;
    folders.push_back(fb);

    /*
     * 把"会用哪两份 tscch"写进日志。
     *
     * 这条链路最容易被误解成"导入整理不碰缓存":实际上窗口扫描走的是
     * DedupWorker::scanFolders,只要 cache/reuseCache 为真就会读、算完还会
     * 就近写回(mergeOnly,只覆盖本次候选子集)。
     *
     * 但"就近"不等于"就在这个目录":scanFolders 开着 useNestedCaches,
     * 会从本目录向上逐级找已经存在的 tscch,把记录合并进去 —— 所以
     * 扫描 <B>/fanbox free 时,如果 <B> 本来就有一份 image_features.tscch,
     * 新记录会写进 <B> 那份,fanbox free 里不会再冒出一个新文件。
     * 用户核对时请看这两行给出的路径以及"上层已有缓存"的实际位置。
     */
    log(QStringLiteral("[阶段二] 特征缓存(读+写): %1 | %2"
                       "(扫描时会向上找已存在的 tscch 并合并进去,"
                       "所以子目录里可能不会出现新文件)")
            .arg(QDir::toNativeSeparators(cacheio::cacheFilePathForFolder(data->rootA)),
                 QDir::toNativeSeparators(cacheio::cacheFilePathForFolder(data->rootB))),
        Level::Info);

    log(QStringLiteral("[阶段二] 把 A=%1 张 / B=%2 张待比较图片交给去重窗口"
                       "(二进制阶段已确认的文件不参与视觉比较)")
            .arg(data->candidateA.size()).arg(data->candidateB.size()),
        Level::Info);

    /*
     * 窗口可能正被别的会话(clear file / remove cover / 普通去重)占着 ->
     * 交给窗口的任务队列排队,当前任务结束后自动启动。
     */
    ImageDeduplicateWindow *w = m_window;
    /* 任务尚未启动:serial=0 表示"还没收到窗口的回执",见 importSessionStarted */
    m_windowSessionSerial = 0;

    /*
     * 关键:会话代由窗口在 beginLoad 里主动播报,不再靠在 lambda 里
     * "load 完再读一次"。lambda 可能因为排队/异常走不到那一行,
     * 而这里一旦漏记,关窗回调就会永远忽略关窗信号(实测把导入整理锁死)。
     */
    disconnect(w, &ImageDeduplicateWindow::importSessionStarted, this, nullptr);
    connect(w, &ImageDeduplicateWindow::importSessionStarted, this,
            [this](quint64 serial) {
                m_windowSessionSerial = serial;
                log(QStringLiteral("[阶段二] 去重窗口已开始本次会话(serial=%1)").arg(serial),
                    Level::Detail);
            });

    const QString label = QStringLiteral("导入整理: %1 ↔ %2")
                              .arg(QFileInfo(data->rootA).fileName(),
                                   QFileInfo(data->rootB).fileName());
    w->enqueueSession(label, [this, w, folders]() {
        w->loadImportSession(folders);
        /*
         * showNormal:窗口可能被用户最小化过。对最小化窗口调 show() 不会还原,
         * 于是"任务已经跑起来了"在桌面上完全看不见 —— 用户只会看到一个
         * 空日志窗口,以为程序卡死。这里明确要求还原 + 置前。
         */
        w->showNormal();
        w->raise();
        w->activateWindow();
    });
}

void ImportDedupeSession::onWindowClosed(bool takeDecisionsFromWindow)
{
    if (m_phase != Phase::AwaitingUser) return;

    /*
     * 下面两个"会话代"检查只对【要从窗口里取判定】的路径有意义。
     *
     * executeOnly()(没有图片需要人工比较)是直接调 onWindowClosed(false) 的,
     * 那种情况窗口根本没 load 过,serial 必然是 0 —— 以前把检查放在函数最前面,
     * 等于让"没有视觉候选"这条最省事的路每次都卡死(phase 永远停在 AwaitingUser,
     * 第二次点导入整理就报"上一次还没结束")。取判定时才需要认门。
     */
    if (takeDecisionsFromWindow) {
        /*
         * 没有收到窗口回执(serial=0)却收到了关窗信号。
         *
         * 常见且正常的情况:本次导入还在窗口队列里排着(前一个会话是
         * remove cover/clear file,它们关闭时也会发这个信号)。这时【不能】
         * 拿这个信号当自己的收尾 —— 判定根本不是我们的。
         *
         * 真出问题的那种(以前会永久死锁)由 recoverStaleSession() 兜底:
         * 下一次点"导入整理"时会发现"phase 还在等用户,窗口早就空闲",
         * 复位后重新开始。这里只留一条日志,不做任何文件决定。
         */
        if (m_windowSessionSerial == 0) {
            log(QStringLiteral("[阶段二] 收到关窗信号,但本次导入还没有登记窗口会话"
                               "(任务可能仍在排队),先忽略"),
                Level::Detail);
            return;
        }

        /*
         * 三槽共听 importSessionClosed(clear file / remove cover / 导入整理),
         * 而窗口是唯一长寿命对象。sessionSerial 对不上 = 窗口在等待期间被
         * 别的会话 load 过,importPairResults() 里是别人的判定 ——
         * 拿它去 planDecisionPhase 会把完全无关的路径当成本次整理的决定。
         * 只退出等待状态,不产生任何操作。
         */
        if (m_window && m_window->sessionSerial() != m_windowSessionSerial) {
            log(QStringLiteral("[阶段二] 比较窗口已被其它会话接管,本次人工判定已放弃"),
                Level::Warning);
            m_phase = Phase::Idle;
            m_data.reset();
            m_windowSessionSerial = 0;
            return;
        }
    }

    std::shared_ptr<SessionData> data = m_data;
    if (!data) return;

    // 1) 把用户在窗口里的判定快照下来
    if (m_window && takeDecisionsFromWindow) {
        const QVector<ImageDeduplicateWindow::ImportPairResult> results =
            m_window->importPairResults();
        data->decisions.clear();
        data->decisions.reserve(results.size());
        for (const auto &r : std::as_const(results)) {
            PairDecision d;
            d.pathA = r.pathA;
            d.pathB = r.pathB;
            d.state = r.state;
            d.seq   = r.decisionSeq;
            d.removedMask = r.removedMask;
            data->decisions.push_back(d);
        }
        if (!m_window->importScanDone()) {
            log(QStringLiteral("[阶段二] 注意:窗口关闭时图片扫描还没完成,"
                               "未产生任何人工判定;未匹配的 B 文件将按规则进入 not_in_a"),
                Level::Warning);
        } else {
            int decided = 0;
            for (const PairDecision &d : std::as_const(data->decisions))
                if (d.state != static_cast<int>(PairState::Pending)) ++decided;
            log(QStringLiteral("[阶段二] 窗口关闭:共 %1 对候选,其中已处理 %2 对")
                    .arg(data->decisions.size()).arg(decided),
                Level::Info);
        }

        // 窗口恢复普通模式:计划执行由会话负责,窗口不再参与
        m_window->setImportCloseHandler(nullptr);
        m_window->setFileOpMode(ImageDeduplicateWindow::FileOpMode::Immediate);
    } else if (m_window) {
        log(QStringLiteral("[阶段二] 本次没有图片需要人工比较,窗口里的历史结果不作为判定依据"),
            Level::Detail);
    }

    startExecution(data);
}

void ImportDedupeSession::abortSession()
{
    /*
     * 只有"等用户在窗口里做决定"这个阶段才谈得上放弃。
     * 扫描中窗口还开不起来、执行中窗口已经关了,其它阶段进来的话直接忽略,
     * 免得把正在跑的后台线程晾在那儿。
     */
    if (m_phase != Phase::AwaitingUser) return;

    /*
     * 这里【没有】任何东西需要撤销:整个会话的承诺就是"关窗之前不动磁盘"。
     * 二进制阶段所谓的"重复文件"只是 data->binaryOps 里的一条条计划
     * (Trash / Move),真正的回收站与移动全部发生在 executeOps() ——
     * 放弃就是不进那个函数。B 侧的 not_in_a 移动同理。
     *
     * m_cancel 仍然置位:窗口关闭过程中可能还有收尾补算/异步任务在排队,
     * 让它们立刻退,别在会话已经结束后再去读 m_data。
     */
    m_cancel.store(true);
    m_data.reset();
    m_phase = Phase::Idle;

    log(QStringLiteral("[阶段二] 用户选择放弃本次导入整理:一条文件操作都不执行,"
                       "磁盘保持原样(二进制阶段也只做了扫描,没有动过文件)"),
        Level::Warning);

    emit sessionAborted(QStringLiteral("导入整理已放弃:没有修改任何文件"));
}

void ImportDedupeSession::executeOnly()
{
    // 没有视觉比较环节:不带任何人工判定,直接进执行
    m_phase = Phase::AwaitingUser;
    onWindowClosed(false);
}

/* ---------------- 阶段三 ---------------- */

void ImportDedupeSession::startExecution(std::shared_ptr<SessionData> data)
{
    m_phase = Phase::Executing;
    joinThreads();
    /* 同 startScan():先置存活再起线程,避免"刚起步"被自检误判 */
    m_execRunning.store(true, std::memory_order_relaxed);
    try {
        m_execThread = std::thread([this, data]() {
            struct ClearOnExit {
                std::atomic<bool> &flag;
                explicit ClearOnExit(std::atomic<bool> &f) : flag(f) {}
                ~ClearOnExit() { flag.store(false); }
            } running(m_execRunning);
            execThreadMain(data);
        });
    } catch (...) {
        m_execRunning.store(false);
        throw;
    }
}

void ImportDedupeSession::execThreadMain(std::shared_ptr<SessionData> data)
{
    auto logFn = [this](const QString &line, Level lv) { log(line, lv); };

    try {
        const ScanResult &scan = *data->scan;

        // 路径 -> 下标(只在这里建一次)
        QHash<QString, int> indexByPath;
        indexByPath.reserve(scan.files.size() * 2);
        for (int i = 0; i < scan.files.size(); ++i)
            indexByPath.insert(normKey(scan.files[i].abs), i);

        QVector<FileOp> ops = data->binaryOps;
        QVector<Disp> disp = data->disp;

        planDecisionPhase(scan, indexByPath, data->decisions, ops, disp, logFn);
        if (m_cancel.load()) return;

        planUnmatchedPhase(scan, ops, disp, logFn);
        if (m_cancel.load()) return;

        /* 给还没算过哈希的操作源补一次哈希(执行前的"内容未变"核对要用,冲突判断也要用) */
        for (FileOp &op : ops) {
            if (op.sourceHash.size() == 32) continue;
            if (op.sourceSize < 0) {
                const QFileInfo fi(op.source);
                if (fi.exists()) { op.sourceSize = fi.size(); op.sourceMtimeMs = fi.lastModified().toMSecsSinceEpoch(); }
            }
            bool ok = false;
            const QByteArray h = dedup::fileSHA256(op.source, &ok);
            if (ok) op.sourceHash = h;
        }
        if (m_cancel.load()) return;

        finalizePlan(scan, ops, logFn);

        int moves = 0, trashes = 0;
        for (const FileOp &op : std::as_const(ops)) {
            if (op.kind == FileOp::Kind::Move) ++moves; else ++trashes;
        }
        m_lastMoves.store(moves);
        m_lastTrashes.store(trashes);

        logFn(QStringLiteral("[计划] 最终操作数: 移动 %1 条 / 回收站 %2 条")
                  .arg(moves).arg(trashes),
              Level::Info);

        /* ---- 操作日志(JSONL):计划 + 每条的执行状态,可事后核对 ---- */
        PlanLogWriter planLog;
        // 用户要求:日志不写 C 盘,统一 M:/Temp/gallery_info
        const QString logBase = ::dedupenv::logDir();
        const QString logDir = logBase.isEmpty() ? QString()
                                                 : logBase + QStringLiteral("/import_dedupe");
        const QString logPath = logDir.isEmpty() ? QString()
                                : logDir + QStringLiteral("/import_dedupe_")
                                      + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")
                                      + QStringLiteral(".jsonl");
        QString logErr;
        if (!logPath.isEmpty() && planLog.open(logPath, &logErr)) {
            m_lastPlanLogPath = logPath;
            QJsonObject header;
            header.insert(QStringLiteral("type"), QStringLiteral("plan"));
            header.insert(QStringLiteral("version"), 1);
            header.insert(QStringLiteral("time"), tsStr());
            header.insert(QStringLiteral("rootA"), data->rootA);
            header.insert(QStringLiteral("rootB"), data->rootB);
            header.insert(QStringLiteral("moves"), moves);
            header.insert(QStringLiteral("trashes"), trashes);
            QJsonArray arr;
            for (int i = 0; i < ops.size(); ++i) {
                const FileOp &op = ops[i];
                QJsonObject o;
                o.insert(QStringLiteral("i"), i);
                o.insert(QStringLiteral("kind"),
                         op.kind == FileOp::Kind::Trash ? QStringLiteral("trash")
                                                        : QStringLiteral("move"));
                o.insert(QStringLiteral("source"), op.source);
                o.insert(QStringLiteral("target"), op.target);
                o.insert(QStringLiteral("reason"), op.reason);
                o.insert(QStringLiteral("origin"), op.origin);
                o.insert(QStringLiteral("seq"), op.decisionSeq);
                arr.append(o);
            }
            header.insert(QStringLiteral("ops"), arr);
            planLog.writeLine(header);
            log(QStringLiteral("[计划] 操作日志: %1").arg(logPath), Level::Info);
        } else {
            log(QStringLiteral("[计划] 无法写入操作日志 %1: %2").arg(logPath, logErr),
                Level::Warning);
        }

        /* ---- 执行 ---- */
        log(QStringLiteral("==== 开始执行整理(所有删除均为系统回收站) ===="), Level::Info);
        int ok = 0, skipped = 0, failed = 0;
        executeOps(ops, planLog, ok, skipped, failed, logFn);
        planLog.close();

        m_lastOk.store(ok);
        m_lastSkipped.store(skipped);
        m_lastFailed.store(failed);

        const QString summary =
            QStringLiteral("导入整理结束: 操作成功 %1 条 / 跳过 %2 条 / 失败 %3 条(移动 %4,回收站 %5)")
                .arg(ok).arg(skipped).arg(failed).arg(moves).arg(trashes);

        if (failed > 0 || skipped > 0) {
            log(summary, Level::Error);
        } else {
            log(summary, Level::Good);
        }

        /*
         * 只有"一条都没失败、也没被跳过"才报告成功。
         * 跳过意味着某个文件的安全检查没通过(被外部改过/不见了),用户必须知道
         * 这次整理并不是完整完成了 —— 不能把部分失败说成成功。
         */
        const bool allOk = (failed == 0 && skipped == 0);
        hopToUi([this, allOk, summary]() { finishSession(allOk, summary); });
    } catch (const std::exception &e) {
        log(QStringLiteral("[执行] 执行计划时发生异常: %1").arg(QString::fromUtf8(e.what())),
            Level::Error);
        hopToUi([this]() { finishSession(false, QStringLiteral("执行计划时发生异常")); });
    } catch (...) {
        log(QStringLiteral("[执行] 执行计划时发生未知异常"), Level::Error);
        hopToUi([this]() { finishSession(false, QStringLiteral("执行计划时发生未知异常")); });
    }
}

void ImportDedupeSession::finishSession(bool ok, const QString &summary)
{
    m_phase = Phase::Idle;
    emit sessionFinished(ok, summary);
}

bool ImportDedupeSession::recoverStaleSession()
{
    if (m_phase == Phase::Idle)
        return false;

    bool stale = false;
    QString why;

    switch (m_phase) {
    case Phase::Scanning:
        if (!m_scanRunning.load(std::memory_order_relaxed)
            && !m_scanResultPending.load(std::memory_order_relaxed)) {
            stale = true;
            why = QStringLiteral("阶段一(二进制扫描)已经没有线程在跑");
        }
        break;

    case Phase::Executing:
        if (!m_execRunning.load(std::memory_order_relaxed)) {
            stale = true;
            why = QStringLiteral("阶段三(执行计划)已经没有线程在跑");
        }
        break;

    case Phase::AwaitingUser: {
        /*
         * 等用户 = 等去重窗口关闭。判断标准只有一条:窗口到底还在不在
         * 替本次导入干活。
         *   · 窗口正忙(本次导入的扫描/列表还在) -> 正常,别动它;
         *   · 窗口已经空闲 -> 说明它早就关过/换过会话,这一等永远不会回来
         *     (这就是实测到的那次:窗口扫完关掉,会话还挂着 AwaitingUser);
         *   · 窗口被别的会话 load 过(serial 对不上) -> 本次判定已经作废。
         */
        if (m_window && m_window->sessionActive()) {
            if (m_windowSessionSerial != 0
                && m_window->sessionSerial() != m_windowSessionSerial) {
                stale = true;
                why = QStringLiteral("去重窗口已被别的会话接管");
            }
        } else {
            stale = true;
            why = QStringLiteral("去重窗口已经不在本次导入整理手里");
        }
        break;
    }

    default:
        break;
    }

    if (!stale)
        return false;

    log(QStringLiteral("[自检] 上一次导入整理卡在 %1,但%2 —— 自动复位,可以重新开始")
            .arg(m_phase == Phase::Scanning ? QStringLiteral("阶段一(扫描)")
                 : m_phase == Phase::AwaitingUser ? QStringLiteral("阶段二(等用户)")
                                                  : QStringLiteral("阶段三(执行)"))
            .arg(why),
        Level::Warning);

    m_cancel.store(true);
    joinThreads();          // 线程若只是刚退出,join 立刻返回
    m_cancel.store(false);
    m_phase = Phase::Idle;
    m_data.reset();
    m_windowSessionSerial = 0;
    return true;
}

} // namespace importdedupe
