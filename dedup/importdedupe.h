#ifndef IMPORTDEDUPE_H
#define IMPORTDEDUPE_H

#include "deduppch.h"

#include <QObject>
#include <QString>
#include <QVector>
#include <QSet>
#include <QHash>
#include <memory>
#include <mutex>
#include <atomic>

class ImageDeduplicateWindow;

/*
 * ============================================================================
 *  导入去重 / 画廊整理会话
 * ============================================================================
 *
 * 场景:E-H(B)缺文件时从 Pawchive(A)补,两边的目录结构不一样。
 * 以前的做法是"先把 B 里与 A 重复的删掉,再把 A 里缺的搬过去",于是大量文件
 * 先删后建、白折腾一遍磁盘。这个会话把整个流程做成一次操作:
 *
 *   阶段一(二进制)  递归扫描 A、B 的【所有】文件(图片/PSD/视频/其它都算),
 *                    按 大小 -> SHA-256 -> 逐字节校验 找出内容完全相同的文件,
 *                    再按"同哈希组"规则决定每组只留一份实体、以及它该落在 B 的哪个
 *                    相对路径(A 的相对路径就是目标结构的基准)。
 *
 *   阶段二(人工)    二进制没匹配上的【图片】交给现有的 ImageDeduplicateWindow
 *                    (延迟执行模式)做视觉比较,由用户决定保留哪一边。窗口里
 *                    不能改磁盘 —— 否则 previous/jump 这些导航动作会带着写盘。
 *
 *   阶段三(收尾)    A 独有 -> 留在 A;B 独有 -> 挪到所在目录的 not_in_a/;
 *                    计划内部统一分配不冲突的目标名,冲突的加 _num 后缀。
 *
 * 所有真实文件操作都推迟到【窗口关闭之后】由 executePlan() 统一执行:
 * 先做安全检查(源还在不在、内容变没变、目标有没有冲突、有没有互相矛盾的操作),
 * 再按 回收站 -> 移动 的顺序执行,失败的单条记录、单条跳过,不牵连其它文件。
 *
 * 日志:通过 logMessage 信号发给 MainWindow 的 LogWidget(首次错误自动弹出),
 *       同时把计划与每条操作的执行结果写进一份 JSONL 操作日志(可事后核对)。
 */
namespace importdedupe {

/* 日志级别(取值与 MainWindow::appendDownloadLog / ktb::LogLevel 对齐) */
enum class Level : int {
    Info    = 0,
    Detail  = 1,
    Good    = 2,
    Warning = 3,
    Error   = 4
};

/* 计划里的一条文件操作 */
struct FileOp {
    enum class Kind {
        Trash,   // 移到系统回收站(永不永久删除)
        Move     // 移动(必要时跨盘:复制 + 校验 + 删源)
    };

    Kind    kind = Kind::Trash;
    QString source;
    QString target;      // Move 专用
    QString reason;      // 人话说明(写进日志与计划文件)
    QString origin;      // 来源标记:"binary:<hash16>" / "pair:<A路径>"
    int     decisionSeq = 0;   // 冲突裁决用(见 PairInfo::decisionSeq)

    /* 计划时的源文件快照,执行前用它核对"文件有没有被外部改过" */
    qint64     sourceSize    = -1;
    qint64     sourceMtimeMs = -1;
    QByteArray sourceHash;     // 可能为空(没算过哈希的文件)
};

/*
 * 一个被扫描到的文件(相对路径 + 内容指纹)。
 * 放在命名空间里(而不是会话类的私有嵌套类型)是为了让 .cpp 里的自由函数
 * —— 扫描 / 哈希 / 分组 / 校验 —— 能直接拿它当参数,不必把实现揉进类里。
 */
struct ScannedFile {
    bool       fromA = true;
    QString    rel;        // 相对各自根的路径,统一用 '/'
    QString    abs;
    qint64     size = 0;
    qint64     mtimeMs = 0;
    QByteArray hash;       // 32 字节 SHA-256;没参与哈希的文件为空
};

/* 阶段一(二进制扫描)的结果:扫描线程写一次,之后只读 */
struct ScanResult {
    QString rootA, rootB;
    QVector<ScannedFile> files;      // A 的全部在前、B 的全部在后,各自按自然序
    QVector<QVector<int>> groups;    // 同哈希组;元素顺序 = files 下标顺序

    int skippedSymlinks   = 0;
    int skippedCacheFiles = 0;
    int skippedQuarantine = 0;       // duplicate / .recycle_bin* 目录里的文件
    int hashFailures      = 0;
    int collisionSplits   = 0;       // 逐字节校验拆出来的"哈希碰撞"组
};

/* 用户在窗口里的最终判定(UI 线程快照给执行线程) */
struct PairDecision {
    QString pathA, pathB;
    int state = 0;
    int seq = 0;
    /*
     * 本对删过哪几侧(位掩码,见 pairstate_math.h)。
     * 失效对上的"只删一侧"是复合态:state 留在 Invalid*,删除只记在这里;
     * 计划生成器必须用 ownRemovalBits 一起判断,否则这次删除会被静默丢掉。
     */
    int removedMask = 0;
};

/* 执行阶段逐条操作的执行结果(写进 JSONL 操作日志) */
struct OpOutcome {
    int     index = -1;
    QString status;      // ok / skipped / failed
    QString detail;
    QString finalTarget;
};

/*
 * 单个文件在计划里的处置状态。
 * 它是"阶段一 → 阶段二 → 阶段三"之间传递结果的载体:
 * 只有 Unhandled 的文件才会落到"未匹配"规则(以及视觉比较候选)里。
 */
enum class Disp {
    Unhandled,        // 还没有任何处置
    Trash,            // 计划送系统回收站
    MoveToTarget,     // 计划移动到 target(要么搬进 B 的目标路径,要么进 not_in_a)
    InPlaceAtTarget,  // 内容已经在目标位置上,不需要动
    KeepInA           // 保留在 A(不产生操作)
};

class ImportDedupeSession : public QObject
{
    Q_OBJECT

public:
    /* 会话所处阶段(Idle = 没有正在进行的会话) */
    enum class Phase { Idle, Scanning, AwaitingUser, Executing };

    explicit ImportDedupeSession(QObject *parent = nullptr);
    ~ImportDedupeSession() override;

    /*
     * 开始一次导入整理。rootA = 结构基准(Pawchive),rootB = 来源兼目标(E-H)。
     * window 由 main() 创建、MainWindow 持有,这里只借用(不接管生命周期)。
     * 路径合法性由调用方(MainWindow)先校验。
     */
    void start(const QString &rootA, const QString &rootB, ImageDeduplicateWindow *window);

    /* 会话还没结束(扫描中 / 等用户在窗口里做决定 / 正在执行计划) */
    bool isActive() const { return m_phase != Phase::Idle; }
    /*
     * 后台线程是不是还活着(扫描/执行)。
     *
     * 以前 isActive() 只看 m_phase:线程因为取消/异常提前退出时没有人改 phase,
     * 这个会话就永远"活着" —— 界面上表现为"上一次导入整理还没结束"，
     * 可去重窗口里一个任务都没有,只能重启程序。
     */
    bool hasLiveWorker() const {
        return m_scanRunning.load(std::memory_order_relaxed)
            || m_execRunning.load(std::memory_order_relaxed);
    }
    /*
     * 会话状态自检。返回 true 表示发现了一个"没有对应后台线程 / 没有对应窗口"
     * 的死会话,并且已经把它复位成 Idle(调用方可以安全地重新 start)。
     *
     * 触发条件(全部要求"确实没人替它干活"):
     *   Scanning  但没有扫描线程
     *   Executing 但没有执行线程
     *   AwaitingUser,窗口已经不在任何会话里(既没在跑本次导入,也没有排队)
     */
    bool recoverStaleSession();
    /* 正在做耗时的后台工作(扫描或执行) */
    bool isBusy() const {
        const Phase p = m_phase;
        return p == Phase::Scanning || p == Phase::Executing;
    }
    /*
     * 关窗询问的行为:
     *   Ask      真弹框问"执行整理 / 继续比较 / 放弃本次整理";
     *   Execute  不问,直接执行(无人值守/测试);
     *   Abort    不问,直接放弃(测试"放弃"这条路径)。
     */
    enum class ClosePrompt { Ask, Execute, Abort };
    void setClosePrompt(ClosePrompt p) { m_closePrompt.store(static_cast<int>(p)); }
    /* 测试/无人值守用:关掉"还有未处理图片对"的确认框,直接按当前结果执行 */
    void setInteractiveCloseConfirm(bool on) {
        setClosePrompt(on ? ClosePrompt::Ask : ClosePrompt::Execute);
    }

    /* 最近一次计划的统计(报告/测试用) */
    int     lastPlannedMoves() const   { return m_lastMoves.load(std::memory_order_relaxed); }
    int     lastPlannedTrashes() const { return m_lastTrashes.load(std::memory_order_relaxed); }
    QString lastPlanLogPath() const    { return m_lastPlanLogPath; }
    /* 执行结果:成功/跳过/失败的条数 */
    int lastExecutedOk() const      { return m_lastOk.load(std::memory_order_relaxed); }
    int lastExecutedSkipped() const { return m_lastSkipped.load(std::memory_order_relaxed); }
    int lastExecutedFailed() const  { return m_lastFailed.load(std::memory_order_relaxed); }

    /*
     * 纯函数形式的"自然排序键"(与 dedupe_move_to_trash.py 的 natural_key 同语义:
     * a2 < a10)。扫描顺序与同哈希组的元素顺序都用它,保证结果可复现。
     * 暴露出来是为了让测试能独立核对顺序规则。
     */
    static QString naturalKey(const QString &text);
    /* B 独有文件的目标路径:所在目录下的 not_in_a/;已经在 not_in_a 里就原地不动 */
    /*
     * "B 独有"文件的去处:<B根>/not_in_a/<相对 B 根的路径>。
     * 已经在 not_in_a 之下时返回原路径(不再嵌套)。
     */
    static QString notInATargetPath(const QString &bFilePath, const QString &rootB);

signals:
    /* 一行日志(level 见 importdedupe::Level) */
    void logMessage(const QString &line, int level);
    /* 会话彻底结束:ok 表示没有任何错误,failed 条数为 0 */
    void sessionFinished(bool ok, const QString &summary);
    /*
     * 用户在关窗询问里选了"放弃本次整理":计划一条都没执行,磁盘没有被修改。
     * 和 sessionFinished(false) 分开 —— 主动放弃不是"出错",不该弹红色错误提示。
     */
    void sessionAborted(const QString &summary);

private:
    /* ---- UI 线程 / 后台线程之间传递的一次会话上下文 ---- */
    struct SessionData {
        QString rootA, rootB, rootALower, rootBLower;
        std::shared_ptr<const ScanResult> scan;
        /* 阶段一(二进制)生成的操作与每个文件的处置 */
        QVector<FileOp> binaryOps;
        QVector<Disp>   disp;
        /* 视觉比较候选(绝对路径);阶段一被安排掉的文件不在里面 */
        QStringList candidateA, candidateB;
        QSet<QString> excludedLower;    // 规范化路径(小写)
        /* 用户判定(UI 线程在窗口关闭时快照) */
        QVector<PairDecision> decisions;
    };

    void log(const QString &line, Level level = Level::Info);
    /*
     * 会话日志同时落盘一份。
     *
     * 为什么需要:导入整理的日志原来只进 MainWindow 的"Download Log"窗口,
     * 而用户看到弹出来的是"去重日志"(它只接 ImageDeduplicateWindow 的信号),
     * 于是"弹了个空窗口、什么都没发生"。现在两边都写,并且从开始扫之前
     * 就落一份文本日志(<logDir>/import_dedupe/import_YYYYMMDD_HHmmss.log),
     * 卡住时不用再靠猜:每一步都有时间戳。
     */
    void openSessionLog();
    void writeSessionLog(const QString &line);

    /* ============ 阶段一:二进制扫描(后台线程) ============ */
    void startScan();
    void scanThreadMain(std::shared_ptr<SessionData> data);
    /*
     * 扫描线程因为取消而提前退出时,回到 UI 线程把"扫描中"状态收掉。
     * 以前这里是裸 return:线程没了、phase 还停在 Scanning,会话就此变成
     * 一个点不动的僵尸(见 recoverStaleSession 的注释)。
     */
    void abortScanOnUi();
    void hopToUi(const std::function<void()> &fn);
    void onScanFinished(std::shared_ptr<SessionData> data);

    /* ============ 阶段二:把候选图片交给 ImageDeduplicateWindow ============ */
    void openWindowForUser(std::shared_ptr<SessionData> data);
    /*
     * 窗口关闭后的收尾。
     * takeDecisionsFromWindow=false 用于"没有图片需要人工比较"的路径:
     * 这时窗口里残留的是【上一次会话】的结果,不能拿来当本次判定。
     */
    void onWindowClosed(bool takeDecisionsFromWindow);
    /*
     * 用户在关窗询问里选了"放弃":丢弃计划,什么都不执行,磁盘保持原样。
     * 注意二进制阶段的"重复文件"也只是记在内存里的计划(Trash/Move 操作列表),
     * 真正的回收站/移动从来都只发生在 executeOps() 里 —— 所以放弃就是"不执行"。
     */
    void abortSession();
    void executeOnly();      // 没有候选图片:跳过窗口,直接执行计划

    /* ============ 阶段三:生成并执行计划(后台线程) ============ */
    void startExecution(std::shared_ptr<SessionData> data);
    void execThreadMain(std::shared_ptr<SessionData> data);
    void finishSession(bool ok, const QString &summary);

    Phase m_phase = Phase::Idle;

    /*
     * 后台线程存活标记(线程入口置位、出口复位,RAII 保证异常也能复位)。
     * 和 m_phase 配合判断"会话是不是已经死了",见 recoverStaleSession()。
     */
    std::atomic<bool> m_scanRunning{false};
    std::atomic<bool> m_execRunning{false};
    /*
     * 扫描线程已经把结果投递给 UI(hopToUi(onScanFinished))。
     *
     * 线程退出到 UI 真正处理这条排队消息之间有一小段"没人跑、phase 还是
     * Scanning"的空档;没有这个标记的话,那期间点一下导入整理就会把
     * 几分钟的扫描结果当成死会话扔掉。
     */
    std::atomic<bool> m_scanResultPending{false};

    /* 会话文本日志(见 openSessionLog) */
    QString    m_sessionLogPath;
    std::mutex m_sessionLogMutex;

    QString m_rootA, m_rootB;
    ImageDeduplicateWindow *m_window = nullptr;
    /*
     * 把图片交给窗口时窗口的 sessionSerial(见 ImageDeduplicateWindow::sessionSerial)。
     * 关窗回调先比对:号变了说明窗口已被别的会话(clear file / remove cover /
     * 普通去重)换过数据,importPairResults() 不是本次导入整理的判定,必须放弃。
     */
    quint64 m_windowSessionSerial = 0;

    std::shared_ptr<SessionData> m_data;   // 只在 UI 线程用

    std::atomic<bool> m_cancel{false};
    /* 关窗询问的方式,见 ClosePrompt */
    std::atomic<int>  m_closePrompt{static_cast<int>(ClosePrompt::Ask)};

    std::atomic<int>  m_lastMoves{0};
    std::atomic<int>  m_lastTrashes{0};
    std::atomic<int>  m_lastOk{0};
    std::atomic<int>  m_lastSkipped{0};
    std::atomic<int>  m_lastFailed{0};
    QString           m_lastPlanLogPath;

    std::thread m_scanThread;
    std::thread m_execThread;

    void joinThreads();
};

} // namespace importdedupe

#endif // IMPORTDEDUPE_H
