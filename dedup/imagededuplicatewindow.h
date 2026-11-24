#ifndef IMAGEDEDUPLICATEWINDOW_H
#define IMAGEDEDUPLICATEWINDOW_H

#include "deduppch.h"
#include "ui_imagededuplicatewindow.h"
#include "DedupImageDisplayWidget.h"
#include "dedupworker.h"
#include "lrucache.h"
#include "dedupenv.h"
#include "duplicatepairlistview.h"
#include "foldercompare.h"
#include "foldergroup.h"

#include <deque>
#include <QSet>
#include <QQueue>
#include <functional>

class FolderCompareWidget;

/*
 * PairState / isInvalidPairState 定义在 pairstate_math.h(纯头文件,可单测)。
 * 那里还定义了"叠加态"的位运算规则,筛选功能依赖它。
 */
#include "pairstate_math.h"

struct PairInfo {
    int idxA = -1;
    int idxB = -1;
    int pHashDistance = INT_MAX;
    int dHashDistance = INT_MAX;
    /*
     * ORB 几何一致率 = RANSAC 内点 / Lowe 命中(0..1)。
     * 它同时是判据(m_orbMinInlierRatio)和界面上显示的"ORB similarity",
     * 所以用户看到 0.89 就知道这一对为什么被判成重复。
     */
    double orbMatchRatio = 0.0;
    /* RANSAC 内点数,判据的另一半;用于日志/提示,不参与显示百分比 */
    int orbInliers = 0;

    bool pixelRatioComputed = false; // NEW: pixel diff ratio has been computed
    double pixelDiffRatio = 1.0; // 0..1 (computed ratio)
    /*
     * pixel diff 连续失败次数。
     * 失败(矩阵取不到/分配失败)时先不锁死结果,给几次重试机会,
     * 免得一次偶发失败就把这一对永久标成"pix diff = 1"。
     */
    int pixelDiffFailures = 0;

    /*
     * 对齐后的显示图(预取窗口缓存)。
     * 只对“预取窗口内”的pair有效(向前最近的Pending对、向后最近的非Invalid对),
     * 一旦滑出窗口就会被立刻释放,所以不会把所有pair的全分辨率图都囤在内存里。
     * 切换pair时直接取这两个,命中就不用解码、不用重新对齐。
     */
    QImage displayPairA;
    QImage displayPairB;
    bool displayPairReady = false;

    PairState state = PairState::Pending;
    /*
     * true = 这一对是程序自动处理掉的,不是用户在界面上点的。
     *
     * 4.2 的归属统计必须把它排除:统计的是"人工决策带来的偏移",
     * 把程序自己的动作算进去,等于让程序跟自己的结果比大小。
     *
     * 现状:像素完全相同那批已经改成"用户按 auto remove"处理(见
     * imagededuplicatewindow.cpp 里的说明),所以目前没有代码把它置 true ——
     * 留着是因为统计口径必须能区分这两种来源,以后再有自动批量处理的对
     * 也必须在这里打标记。
     */
    bool autoResolved = false;
    /*
     * 叠加态:本对移除过哪一侧(位1=A、位2=B)。
     *
     * 只有 state 是 Invalid* 时才会用到它 —— "因为别的对失效(invalid d)
     * + 本次筛选把另一张也删了(removed e)"。
     * 以前这种对只能二选一:记成 InvalidA 就丢了"e 已被删"的事实
     * (4.2 的 deleted_from_* 会少算一次用户动作),记成 RemovedB 又丢了失效原因
     * (点它会跳不回肇事的那一对)。现在两者都留着,显示成
     * "InvalidA + RemovedB"。
     *
     * 有效对不需要叠加:RemovedA/RemovedB/RemovedBoth 已经能表达"删了哪一侧"。
     */
    quint8 removedMask = 0;

    /*
     * 第二条叠加轴:哪一侧已经被【别的对】移走(位1=A、位2=B)。
     *
     * 和 removedMask 的区别:removedMask 记"本对删了谁",只在 Invalid* 时有意义;
     * invalidMask 对所有状态都有意义 —— Skipped / Removed* 的对照样可能因为
     * 别的对删了其中一张而失效,而 PairState 表达不了。
     * 见到它就必须当"失效"处理(restore / 批量 / 菜单 都走 pairLooksInvalid)。
     */
    quint8 invalidMask = 0;

    mutable std::mutex stateMutex;   // 新增：保护上述可变状态

    /*
     * 记录“是谁把A/B搞成Invalid的”:
     * 点击Invalid项时可以直接跳到那个pair。0表示没有记录。
     */
    int invalidCauseForA = 0;   // pairIndex+1
    int invalidCauseForB = 0;

    /*
     * 本对最后一次动作的先后序号(由 ImageDeduplicateWindow::m_decisionSeq 递增分配)。
     *
     * 为什么需要它:导入整理把文件操作推迟到窗口关闭之后统一执行,于是可能出现
     * "同一个文件被两个不同 pair 判成了互相冲突的两件事"的情况,例如
     *   pair1(A1,B1) -> RemoveB : 保留 A1 并计划把它搬进 B
     *   pair2(A1,B2) -> RemoveBoth: 把 A1 也送去回收站
     * 两者不能同时执行。有了序号就能按"用户最后一次的意图"裁决,而不是随便挑一个。
     */
    int decisionSeq = 0;
};

/* 把PairState翻译成可读字符串(列表显示用) */
const char *pairStateName(PairState state);
/* PairState的整数值(列表只存int) */
inline int pairStateToInt(PairState s) { return static_cast<int>(s); }

struct ActionRecord {
    enum Type { None, RemoveA, RemoveB, RemoveBoth, Skip } type = None;
    QString movedA_from;
    QString movedA_to;
    QString movedB_from;
    QString movedB_to;
};

class ImageDeduplicateWindow : public QWidget {
    Q_OBJECT
public:
    explicit ImageDeduplicateWindow(QWidget* parent = nullptr);
    ~ImageDeduplicateWindow() override;

    /* ============ 文件操作模式(导入整理复用本窗口的关键开关) ============ */
    /*
     * Immediate:默认行为。remove/restore 立刻改 FileEntry 并把物理 rename
     *            交给 m_fileOpPool 串行执行(和加这个开关之前完全一致)。
     * Deferred :导入整理用。remove/restore 只更新比较状态和"待执行计划"的语义,
     *            绝不碰磁盘、也不改 currentPath —— 文件必须留在原地,
     *            否则窗口自己就没法继续读图比较了(是"先移走再想办法恢复"
     *            的反面)。真正的移动/回收站由会话结束后的统一计划执行。
     */
    enum class FileOpMode { Immediate, Deferred };

    void setFileOpMode(FileOpMode mode) { m_fileOpMode = mode; }
    FileOpMode fileOpMode() const { return m_fileOpMode; }

    /*
     * 会话代:每次 load*(普通去重/导入整理/remove cover/clear file)都会 +1。
     *
     * 窗口是 main() 里的唯一长寿命对象,四类会话共用。外部会话在 load 之后
     * 记下这个号,closeEvent 收到 importSessionClosed 时先比对 —— 号变了说明
     * 窗口已经被另一个会话换过数据,这次信号里的 importPairResults() 不是
     * 自己的判定,必须整段忽略。以前没有这层校验,一个信号被三个消费者
     * (remove cover / clear file / import dedupe)共用,谁的 active 标记先
     * 命中谁就去读快照,可能把别家的判定当成自己的计划执行。
     */
    quint64 sessionSerial() const { return m_sessionSerial; }

    /*
     * ===== 任务队列(四类会话共用本窗口) =====
     *
     * 调用方不再直接 load*,而是 enqueueSession:
     *   · 窗口空闲 -> 立即执行(和以前一样);
     *   · 窗口忙   -> 排队,当前任务结束(closeEvent,含"下一个任务"按钮)
     *                 之后自动启动队首。
     * start 是个自包含的 lambda(load* + show/raise),窗口只负责调度。
     */
    void enqueueSession(const QString &label, std::function<void()> start);
    /* 当前是否有任务在窗口里(load 之后 ~ closeEvent 之前) */
    bool sessionActive() const { return m_sessionActive; }

    /*
     * 导入整理会话结束时,导出的"每对图片的最终判定"。
     * 只在 Deferred 模式下有意义:文件都还在原地,pathA/pathB 就是扫描时的路径。
     * state 是 PairState 的整数值;decisionSeq 见 PairInfo::decisionSeq。
     */
    struct ImportPairResult {
        QString pathA;
        QString pathB;
        int     state = 0;
        int     decisionSeq = 0;
        /*
         * 本对删过哪几侧(位掩码,见 pairstate_math.h 的 SideBitA/SideBitB)。
         *
         * 必须导出:失效对上"只删一侧"产生的是复合态(如 InvalidB + 删过 A),
         * state 只剩 InvalidB,计划生成器按 state 推导时会把这个删除静默丢掉。
         * 消费方统一用 ownRemovalBits(state, removedMask) 得到"本对真正删过
         * 哪几侧",不要只 switch(state)。
         */
        int     removedMask = 0;
    };
    QVector<ImportPairResult> importPairResults();

    /* 候选图片的扫描 + 配对是否已经完成(导入整理用它判断"关窗时有没有结果") */
    bool importScanDone() const { return m_importScanDone.load(std::memory_order_relaxed); }

    /*
     * 导入整理模式下关闭窗口前的询问钩子,返回三种结果之一:
     *   Execute       允许关闭,会话按当前判定(含二进制阶段)执行整理计划
     *   KeepOpen      取消本次关闭,继续在窗口里比较
     *   AbortSession  关闭窗口,并放弃整次导入整理 —— 计划一条都不执行
     */
    enum class ImportCloseResult { Execute, KeepOpen, AbortSession };

    /*
     * 注入关闭询问钩子。ImportDedupeSession 用它弹出"执行整理/继续比较/放弃本次整理"。
     * 返回 AbortSession 时窗口会发 importSessionAborted(),由会话丢弃计划。
     */
    void setImportCloseHandler(std::function<ImportCloseResult()> handler)
    {
        m_importCloseHandler = std::move(handler);
    }

    // load folders (can be called multiple times). Will reset state.

    void load(const auto &folders)
    {
        beginLoad(folders, /*importMode=*/false);
    }

    /*
     * 导入整理专用载入:和 load() 的唯一区别是进入 Deferred 模式并标记会话开始,
     * 这样窗口关闭时才会通知会话去执行计划。
     */
    void loadImportSession(const auto &folders)
    {
        beginLoad(folders, /*importMode=*/true);
    }

    /*
     * remove cover 专用:只显示给定的 (A=内容, B=封面) 对。
     *
     * 不走 buildPairsFromFiles 的 pHash/dHash 阈值 —— 这些对是调用方按目录结构
     * 挑出来的,即使 pHash 距离很大也必须让用户看到(比如封面是内容的裁剪+遮罩)。
     * 和 loadImportSession 一样是 Deferred:关窗前不碰磁盘。
     */
    void loadPairSession(const QVector<QPair<QString, QString>> &pairs);

    /*
     * ===== clear file 专用:A = 压缩包解压内容,B = 散图 =====
     *
     * 和 load()/loadImportSession() 的区别:
     *   · 只建 A↔B 的对(同组内一律不比较):多个压缩包解压出的多个根目录之间
     *     也不该互相比较,所以用 compareSide 而不是 rootInputFolder 隔离;
     *   · A 侧强制放在左(用户看到的左边永远是压缩包里的那张);
     *   · 排序先按 A 侧的解压根目录分组,组内 pHash -> pix -> ORB -> dHash;
     *   · Deferred:关窗 = 提交,由 clear file 会话统一删到回收站;
     *   · 不做文件夹主流程里的"完全重复自动移走 / 资源文件去重"(那两个会直接改磁盘)。
     *
     * folders 里每一项都是一次扫描的输入目录:
     *   A 组 = 解压根(可多个),B 组 = 压缩包所在目录 + fileFilter(只收直属文件)。
     */
    struct ZipCompareFolder {
        QString folder;
        QString regexFilter;
        int     internal = 0;        // InternalDedup::None
        bool    cache = false;
        bool    reuseCache = false;
        int     compareSide = 0;     // 0 = A(压缩包内容),1 = B(散图)
        /*
         * B 侧必须是 false:散图和 zip 解压出来的图片在同一个目录树里
         * (attachments/ 与 attachments/<zip名>/),递归收集会把解压内容
         * 当成散图再比一遍,还会把它们的记录写进 attachments 的 tscch。
         */
        bool    recursive = true;
        std::function<bool(const QString &)> fileFilter;
    };
    void loadZipCompareSession(const QVector<ZipCompareFolder> &folders);

    // runtime control for pixel-diff behavior
    void setEnablePixelDiff(bool enable);   // 控制是否计算 pixel diff（ratio）

    /*
     * m_mutex 重入保护的RAII标记(见 .cpp 里的详细说明)。
     * 内部函数在“调用方已经持有 m_mutex”时会跳过重复加锁,
     * 避免同一线程递归加锁 —— Windows 的 SRWLock 上那样会直接死锁。
     * 用“深度”而不是bool,保证多层嵌套也正确。
     */
    class MMutexScope {
    public:
        explicit MMutexScope(int &depth);
        ~MMutexScope();
    private:
        int &m_depth;
    };

signals:
    /* 窗口里的日志同步一份给主窗口的 LogWidget(level: 0=Info,3=Warning) */
    void logMessage(const QString &line, int level);
    /* 窗口真的被关掉了(不是隐藏)：主窗口据此把半选路径的日志窗口一起收起来 */
    void windowClosed();
    /* 导入整理会话的窗口关闭了(此时文件操作还没有执行,由会话负责统一执行) */
    void importSessionClosed();
    /*
     * 用户在"是否执行整理"里选了"放弃":窗口已经关闭,会话必须丢弃计划。
     * 和 importSessionClosed 分开,否则会话无从区分"执行"和"放弃"。
     */
    void importSessionAborted();
    /*
     * 导入整理/clear file 这类外部会话"真的开始了"(beginLoad 已经 +1 会话代)。
     *
     * 为什么需要这个信号:以前会话在 enqueueSession 的 lambda 里 load* 之后
     * 自己读 sessionSerial() 记下来。可 lambda 有两种不执行的理由 ——
     * 窗口忙时被排队、或者会话在 load 途中就被关掉 —— 一旦漏记,
     * 关窗回调里的 `serial == 0 就 return` 会把会话永远钉在"等用户"上
     * (实测:窗口扫完/关掉后,导入整理再也点不动,只能重启)。
     * 由窗口在 beginLoad 里主动播报,会话侧就不会漏。
     */
    void importSessionStarted(quint64 serial);

protected:
    // override to capture space press for drag enable
    void keyPressEvent(QKeyEvent *ev) override;
    // void keyReleaseEvent(QKeyEvent *ev) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    /*
     * 导入整理模式下,关闭窗口是"执行计划"的触发点,所以这里要先过一遍
     * m_importCloseHandler(用户可能被问"还有 N 对没处理,真的要关吗")。
     * 普通去重模式下不做任何额外动作,保持原来的关闭行为。
     */
    void closeEvent(QCloseEvent *ev) override;

private slots:
    // Designer slots
    void on_btn_previous_clicked();
    void on_btn_rm_a_clicked();
    void on_btn_rm_b_clicked();
    void on_btn_rm_both_clicked();
    void on_btn_jmp_clicked();
    void on_btn_auto_rm_clicked();
    void on_btn_clear_clicked();
    void on_sb_pixdiff_valueChanged(double v);
    void on_sb_area_valueChanged(double v);

    // internal
    void computeAllPixelDiffsAndResort(int epoch);//外部加锁
    void onWorkerScanProgress(int done, int total);
    void onWorkerScanFinished(const QVector<FileEntry>& files,int epoch);
    void onWorkerLog(const QString &msg);

    void onDisplayImagePainted(quint64 serial);
    void onDisplayLeftClicked();
    void onDisplayRightClicked();

    void on_btn_stop_clicked();

    /* ===== DuplicatePairListView 交互 ===== */
    void onPairActivated(int pairIndex);
    void onInvalidPairActivated(int pairIndex, bool preferA);
    void onPairListLazyLoad(const QVector<int> &pairIndices);
    void onPairListBatchAction(const QVector<int> &pairIndices, int actionType);
    /* 右键菜单:只作用于右键的那一对 */
    void onPairListPairAction(int pairIndex, int actionType);

    void on_btn_jump_this_file_clicked();

    void on_btn_next_clicked();

    /* 结束当前任务并开始队首;没有活跃任务时直接启动队首 */
    void on_btn_next_task_clicked();

private:
    /*
     * load()/loadImportSession() 的公共实现。
     * 唯一多出来的就是 importMode:它决定 FileOpMode 以及"关窗要不要通知会话"。
     * 注意这里不能调用 nowStr() —— 它是 .cpp 里的静态函数,而本函数是模板
     * (在调用方 TU 里实例化),所以直接用 QDateTime。
     */
    template <typename FolderList>
    void beginLoad(const FolderList &folders, bool importMode)
    {
        ++m_sessionSerial;         // 会话代:消费者据此判断"窗口还是不是我那次 load"
        /*
         * 先播报会话代,再做后面的重活。**只对导入整理播报**。
         *
         * 顺序很关键:外部会话要在这个号确定的第一时间记下它,才能在
         * "窗口刚 load 完就被关掉"这种极端时序里认出关窗信号。
         * 以前的写法是 load* 返回之后由调用方读 sessionSerial(),
         * 只要 lambda 因为排队/异常没能走到那一行,号就永远是 0 —— 关窗回调
         * 直接 return,会话停在"等用户"状态,再也点不动。
         *
         * 为什么不能对普通去重也播报:导入整理可能正排在窗口队列里等着,
         * 这时候来一次普通去重(或 remove cover),它的会话代就会被当成
         * "本次导入的会话代"记下来,关窗时把别人的判定当成自己的计划执行 ——
         * 那是会动磁盘的错。normal load 的关闭走 windowClosed,不关这边的事。
         */
        if (importMode)
            emit importSessionStarted(m_sessionSerial);
        m_sessionActive = true;    // 队列据此判断"窗口忙不忙"
        m_explicitPairs.clear();   // 普通/导入会话不带显式文件对
        m_fileOpMode = importMode ? FileOpMode::Deferred : FileOpMode::Immediate;
        m_importSessionActive = importMode;
        m_importScanDone.store(false, std::memory_order_relaxed);

        std::unique_lock<std::shared_mutex> lk(m_mutex);
        m_dataEpoch.fetch_add(1, std::memory_order_relaxed);
        qDebug()<<"m_dataEpoch已递增,当前值:"<<m_dataEpoch;
        clearState();
        logLine(QString("[%1] Starting scan of %2 folders...%3")
                    .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"))
                    .arg(folders.size())
                    .arg(importMode ? QStringLiteral(" [import-dedupe: all file operations deferred]")
                                    : QString()));
        setProgressPhaseScanning();   // 第一层:绿色,表示pHash/ORB扫描进度
        m_worker->startScan(folders,m_dataEpoch);// epoch随着该函数调用而递增
    }

    Ui::ImageDeduplicateWindow ui;
    DedupImageDisplayWidget *m_display = nullptr;
    DuplicatePairListView *m_pairList = nullptr;

    /* ===== 导入整理(Deferred)模式的状态 ===== */
    FileOpMode m_fileOpMode = FileOpMode::Immediate;
    /* 本次 load 是不是导入整理会话(决定 closeEvent 要不要通知会话) */
    bool m_importSessionActive = false;
    /* 会话代:每次 load* +1,见 sessionSerial() */
    quint64 m_sessionSerial = 0;

    /* ===== 任务队列(见 enqueueSession) ===== */
    struct SessionRequest {
        QString label;
        std::function<void()> start;
    };
    QQueue<SessionRequest> m_sessionQueue;
    QString m_currentSessionLabel;
    /* load* 之后置 true,closeEvent 置 false 并 pump 队首 */
    bool    m_sessionActive = false;
    void pumpSessionQueue();
    void updateSessionQueueUi();
    /*
     * clear file 的 A/B 组会话(见 loadZipCompareSession)。
     * 只影响建对/摆位/排序,不影响 Deferred 那套"关窗才执行"的机制。
     */
    bool m_zipCompareMode = false;
    /* 关窗前的询问钩子,见 setImportCloseHandler() */
    std::function<ImportCloseResult()> m_importCloseHandler = nullptr;
    /* remove cover 的显式 A/B 对(路径);非空时 buildPairsFromFiles 不参与 */
    QVector<QPair<QString, QString>> m_explicitPairs;
    /* 候选图片扫描 + 配对是否完成(worker 扫描结束时置位) */
    std::atomic<bool> m_importScanDone{false};
    /* decisionSeq 的分配器(见 PairInfo::decisionSeq) */
    std::atomic<int> m_decisionSeq{0};

    /* 列表选中同步时的抑制标志(程序性跳转不要反过来触发跳转) */
    bool m_suppressListActivation = false;

    /* ===== 进度条(两层) ===== */
    /*
     * 第一层:绿色chunk = pHash/ORB 扫描进度;
     * 第二层:设置 state="pix_diff_caculating" 属性后,背景变绿、chunk变黄绿,
     *         用已算完的pair数占比表示 pixel diff 阶段进度。
     */
    void setProgressPhaseScanning();
    void setProgressPhasePixelDiff();
    /* 在UI线程上刷新进度(供后台线程调用,内部会排队到UI线程) */
    void requestProgressRefresh(bool scanning, int done, int total);
    /* 只应在UI线程调用:设置进度条的值/state属性并刷新样式 */
    void applyProgressValue(const QString &state, int done, int total);

    /*
     * 第二层进度用的原子计数(避免在已持锁的后台任务里重复加锁)。
     * done     = 已经算完pixel diff的pair数;
     * expected = “实际被受理去算”的pair数。
     * 之所以不能拿 m_pairs.size() 当总数:有些pair在入队时就已经算好了会被跳过,
     * 也有pair会被去重队列过滤,拿全表大小当分母进度就永远到不了100%。
     */
    std::atomic<int> m_pixDiffDoneCount{0};
    /* 半选模式的 pix diff 阶段只启动一次(扫描末尾启动、方向定完不要重复启动) */
    bool m_pixDiffPhaseStarted = false;
    std::atomic<int> m_pixDiffExpectedCount{0};
    /* 后台线程进度上报的合并缓冲:只保留最新值 + 最多一个待处理的刷新事件 */
    std::atomic<int>  m_progressDone{0};
    std::atomic<int>  m_progressTotal{0};
    std::atomic<int>  m_progressState{0};            // 0=扫描阶段 1=pixel diff阶段
    std::atomic<bool> m_progressRefreshScheduled{false};

    /*
     * 内部去重现在【按输入目录各自设置】(每个目录的 cb_internal_x:
     * 不勾/半勾/全勾 = 0/1/2),所以这里不再有全局开关。
     * 规则见 FileEntry::internalMode 与 buildPairsFromFiles 里的配对判断。
     */
    /*
     * prefetch: 计算 overlay / pixdiff / 显示图的窗口半径(0 = 不预取,1 = 每个方向各1个)。
     * 这里是唯一的内存/预取速度权衡开关 —— 每多一个pair就多两份全分辨率的
     * “对齐后显示图”。查重与对比一律使用原始分辨率,不做任何降采样。
     */
    int m_prefetchWindow = 2;

    /* ===== 显示图缓存(预取窗口) ===== */
    /*
     * 只缓存“预取窗口”内的对齐后图像(窗口规则见 collectPrefetchWindowPairs):
     *   - displayPair 优先用缓存(零解码零缩放),未命中才现场构造并塞进缓存;
     *   - 窗口滑动时,落在窗口外的条目会被立刻释放。
     * 这样既保留了“按pair缓存显示图”的加速效果,又不会让pixel diff阶段
     * 把所有pair的全分辨率图都囤在内存里。窗口大小就是 m_prefetchWindow。
     */

    /* 显示图就存在 PairInfo::displayPairA/B 里;这个QHash只是pairIndex->是否有缓存的索引 */
    QHash<int, bool> m_displayCacheIndex;
    std::mutex m_displayCacheMutex;                 // 独立锁,别和m_mutex/stateMutex混在一起

    /*
     * 正在后台预取中的 pair:下标 -> 派发时刻(毫秒,m_displayCacheMutex 保护)。
     *
     * 不能只记"在不在飞",还要记"飞了多久" —— 原因见 2026-11-11 的现场:
     * sortPairsFromIndex 会重排 m_pairs,而正在飞的任务手里拿的是【旧下标】。
     * 旧实现把 in-flight 集合按 old->new 重映射,任务结束时却按旧下标 remove,
     * 于是重映射后的新下标永远留在这个集合里 —— 这一对之后每次预取都被
     * "已经在飞"挡掉,永远不上屏,而且没有任何失败日志。
     * 现在:
     *   · 重排时直接清空这个集合(旧任务的下标已经没有意义,让它自然退出);
     *   · 即使还有别的路径漏了,超过 kDisplayPrefetchStaleMs 的标记会被当作
     *     陈旧标记重新派发,不会把一对图永久卡死。
     */
    static constexpr qint64 kDisplayPrefetchStaleMs = 20000;
    QHash<int, qint64> m_displayPrefetchInFlight;

    /* 取缓存的显示图;命中返回true */
    bool getDisplayCache(int pairIndex, QImage &outA, QImage &outB);
    /* 写入缓存的显示图(写进PairInfo::displayPairA/B) */
    void putDisplayCache(int pairIndex, const QImage &imgA, const QImage &imgB);
    /* 标记/取消标记"该pair正在后台预取中"(用于避免同一对重复派发) */
    bool markDisplayPrefetchInFlight(int pairIndex);
    void clearDisplayPrefetchInFlight(int pairIndex);
    /* 按窗口裁剪:保留 keepPairs 里的,其余立刻释放 */
    void pruneDisplayCache(const QSet<int> &keepPairs);
    /* 清空整个显示图缓存(自己加 m_mutex) */
    void clearDisplayCache();
    /*
     * 只清 PairInfo 里的显示图,不碰 m_mutex。
     * 调用方必须已持有 m_mutex(共享/排他皆可) —— clearState() 就是这种情况。
     */
    void clearDisplayPairImagesLocked();
    /*
     * 收集预取窗口内的pair下标(调用方必须已持有 m_mutex 共享锁):
     *   向前 m_prefetchWindow 个 Pending(下一步要处理的);
     *   向后 m_prefetchWindow 个非Invalid(刚才处理过的,previous要能秒开)。
     * 两端的文件都必须还在原地。
     */
    QVector<int> collectPrefetchWindowPairs(int centerIndex) const;
    /* 收集窗口内所有“可显示”的pair下标;现在直接复用上面的规则 */
    QVector<int> collectDisplayWindowPairs(int centerIndex) const;
    /*
     * 后台为窗口内的pair准备显示图。
     *
     * windowPairs 由调用方在持 m_mutex 时算好并传进来 ——
     * 这个函数内部绝对不去碰 m_mutex / currentIndexMutex,
     * 因为它可能被 displayPairByIndex 在持有 currentIndexMutex 的情况下调用,
     * 一旦在这里再去拿 m_mutex/cacheMutex 就会和后台排序线程形成锁序反转而死锁。
     * 后台任务自己会重新拿 m_mutex 做快照。
     */
    void prefetchDisplayWindow(const QVector<int> &windowPairs, int epoch);
    /*
     * 后台准备单个 pair 的显示图(解码 + 原始分辨率对齐 + 转 QImage + 写缓存),
     * 完成后回 UI 线程换图(applyDisplayImagesFromCache)。
     * 同一对重复调用会被 in-flight 标记挡住,不会重复派发。
     * 线程安全,可在任意线程调用(内部只启动任务,不阻塞)。
     */
    void prepareDisplayPairAsync(int pairIndex, int epoch);

    // data
    std::vector<FileEntry> m_files;
    std::vector<std::shared_ptr<PairInfo>> m_pairs;

    std::shared_mutex m_mutex;
    std::mutex cacheMutex;//缓存锁,访问cachedFileIndices和cachedFileIndices_forPixCompute时上锁
    std::mutex currentIndexMutex;
    int m_currentIndex = -1; // index into m_pairs

    /*
     * 切图时目标 pair 的显示图还没就绪:先停在这里,等后台加载完再真正切换。
     * 否则屏幕还留着上一对的图,而 remove A/B 已经作用在新 pair 上 ——
     * 用户看到的和实际删的不是同一对(会误删)。-1 表示没有待切换目标。
     * 只在 UI 线程读写。
     */
    int  m_pendingDisplayIndex = -1;
    int  m_pendingDisplayEpoch = -1;
    /* 加载超时过的 pair:允许"强制放行"(显示空白),避免坏图把人永久卡住 */
    QSet<int> m_displayLoadFailedIndices;

    /*
     * 当前显示的这一对是否处于"图片无法显示"状态(显示区空白 + 角标提示)。
     *
     * 这个状态必须和 m_currentIndex 一起维护:
     *   · displayPair 命中缓存 / 后台补图成功 -> false
     *   · 解码失败 / 3 秒没提交任何图 / 两侧文件都没了 -> true
     * onDisplayImagePainted 靠它决定角标里要不要加"图片无法显示";
     * 否则空白图的 paint 会把角标刷回普通的 "A · pair N / M",
     * 用户就分不清"这张图是黑的"还是"这一对没有图"。
     * 只在 UI 线程读写。
     */
    bool m_currentPairImageUnavailable = false;
    /*
     * 显示区已经清空,但后台还在加载(超过 3 秒没上屏,还没到"失败"的程度)。
     * 和 unavailable 互斥:文案一个是"加载中",一个是"图片无法显示"。
     * 线程池排队/磁盘慢的时候会走到这里,不能当成坏图。
     */
    bool m_currentPairImageLoading = false;

    // display / viewport cache
    QPixmap m_pixA, m_pixB;

    std::set<int> cachedFileIndices;//displayPair的预加载缓存,requestComputePixelForPair也会首先检查缓存是否存在于此文件夹中
    /*
     * pix 计算的矩阵缓存。
     *
     * 原来只有 5 个槽位,而 computeAllPixelDiffsAndResort 的并发是
     * min(核数, 内存预算) —— 12MP 图在 16 核机器上能开到 8~10 个 worker,
     * 每个任务要 2 张图。槽位比"活跃任务数×2"还小,就会互相淘汰:
     * 同一张图刚解码完、还没被另一个 worker 用上就被挤掉,于是重新解码一遍。
     * 表现就是"图越多越慢",而且日志里 LRU 淘汰和"预加载"交替刷。
     *
     * 现在的约束:容量 >= 并发上限 × 2(见 kPixDiffMaxWorkers),
     * 单张 32MP 的 BGR 约 93MB,16 个槽位最坏 1.5GB —— 但只有在
     * 全部槽位都装着 32MP 图且有并发时才会达到,普通图远低于此。
     */
    LRUCache<int,int,16> cachedFileIndices_forPixCompute;
    bool findInCache(int index) {
        auto it = cachedFileIndices.find(index);
        if(it!=cachedFileIndices.end()){
            return true;
        }else
            return cachedFileIndices_forPixCompute.hasKey(index);
    }
    /*
     * 保证fileIdx的矩阵已经加载。命中cacheMutex保护的任一缓存就直接返回true。
     * mode: 0 = 放到预加载缓存(会被fileUnloadMat统一释放)
     *       1 = 放到pix计算用的LRU
     * excludeA/excludeB: 这两个下标绝不能从LRU淘汰出去(它们正在被使用)
     *
     * 【锁】本函数自己管理 cacheMutex,调用方【不要】持有 cacheMutex。
     * 原因:解码一张 32MP 图要 ~1 秒,原来整个过程都在 cacheMutex 里,
     * 把预取/像素计算/预加载全部串成了一根线。现在锁只包住两段很短的查表/登记,
     * 解码只占该文件自己的 matMutex,不同文件的解码才能真正并行。
     *
     * 【outMat】可选。非空时在其中返回矩阵句柄(cv::Mat 是引用计数,拿到之后
     * 即使缓存把它淘汰/释放,数据也依然有效)。
     * 句柄是在持有 cacheMutex 的临界区里取的 —— 这一点是必须的:
     * 否则"ensure 返回"到"调用方自己去读 m_mat"之间存在窗口,
     * 别的线程可能刚好把这个下标从 LRU 里淘汰掉并 releaseMat,
     * 调用方就会读到空矩阵 —— pixel diff 会把它误判成"计算失败(ratio=1)",
     * 显示图预取则会直接放弃这一对。所以凡是"ensure 完还要用矩阵"的地方,
     * 都必须用 outMat 拿句柄,不要自己去读 m_files[].m_mat。
     */
    bool ensureMatCached(int fileIdx, int mode, int excludeA = -1, int excludeB = -1,
                         cv::Mat *outMat = nullptr);
    /* 往pix计算LRU里塞一个下标,必要时按LRU淘汰(避开预加载缓存里和exclude的) */
    void putPixComputeCache(int fileIdx, int excludeA = -1, int excludeB = -1);
    /* 清空pix计算LRU,并真正释放其中不再被预加载缓存引用的矩阵 */
    void releaseAllPixComputeCache(int excludeA = -1, int excludeB = -1);
    /* 记录曾经放进pix计算LRU的下标(用于精确释放) */
    std::set<int> trackedPixCacheIndices;

    /*
     * 冻结进度计账(原子,后台线程会读)。
     *
     * 主阶段的并发 worker 全 join 完之后就把它置 true:那一刻进度条的分子分母已经相等,
     * 显示的应该就是 100%。后面"收尾补算"是对失败任务的修复,**不是**新工作 ——
     * 如果继续让它们去改 expected/done,进度条就会在 99%/100% 之间反复抽搐,
     * 而补算本身可能跑几十秒(每对都要重新解码)。
     */
    std::atomic<bool> m_pixDiffProgressFrozen{false};

    /*
     * 批量阶段(computeAllPixelDiffsAndResort 的并发段)是否正在跑。
     *
     * 这个阶段的任务数在开工前就全知道了,所以分母由它一次性设好(见
     * m_pixDiffExpectedCount);每个任务自己 **不能** 再去 +1 ——
     * 否则分母跟着分子一起涨,进度永远显示 ~99%,不管实际做了多少(这就是
     * "卡在 99% 很久"的真正原因)。此标志期间的所有计算都不再改分母。
     */
    std::atomic<bool> m_pixDiffBulkPhase{false};

    /*
     * "显示图还没就绪"这条日志上一次记的是哪一对。
     * 同一对只记一次:期间用户每按一次键都会重走这条路径,以前就是一路刷屏,
     * 看起来像"程序在疯狂报错"。
     */
    int m_displayNotReadyLoggedIndex = -1;

    bool m_showingA = true;

    /*
     * "我在等第几号 pixmap 上屏"(0 = 没在等)。
     *
     * DedupImageDisplayWidget::setPixmap 返回自增序号,paintEvent 再把当前序号
     * 一起发回来。只有两边对上,才能确定"屏幕上这一帧就是我刚换上去的那张图"。
     * 以前用的是 m_pendingPaintCount 计数器 + "任何一次 paint 都算上屏",
     * 于是角标/局部重绘也会把按钮解锁、把"切换中"清掉 —— 保护机制形同虚设。
     */
    quint64 m_awaitingPaintSerial = 0;

    /*
     * "这一对的信息还没回填"标记(只在 UI 线程读写)。
     *
     * 切图时先把侧栏清空、按钮锁住(屏幕上此刻还是上一对的图,任何数字/按钮都
     * 可能作用在错的图上),等新图真正 paint 出来(onDisplayImagePainted)才
     * 回填侧栏并解锁。cacheHit 的快速切图也走同一条路 —— 大图 + 连续高速切换时
     * 卡的就是这一帧 paint,不锁的话用户完全可能在"图还是旧的"那一瞬间按下 remove。
     */
    bool m_switchInfoPending = false;

    // thresholds (tweakable)
    int m_pHashThreshold = 6;
    int m_dHashThreshold = 1;
    /*
     * ===== ORB 判重的两个判据 =====
     * 旧版是"描述子命中率 >= 0.18",只做 Lowe ratio test、不做几何校验,
     * 结果是:草稿/成品这种线条重绘过的对只有 0.106(漏),
     * 而同页 UI 截图那种"命中一堆但几何上互相矛盾"的能到 0.09(差点误中)。
     *
     * 现在改成几何校验(相似变换 + RANSAC),两个条件"与":
     *   · 内点数 >= m_orbMinInliers       —— 挡住只有十几个点凑成一条线的退化解
     *   · 内点/命中 >= m_orbMinInlierRatio —— 挡住命中很多但互相矛盾的
     * 数值来自实测,见 deduputils.h 里的 OrbVerifyOptions 注释。
     */
    int    m_orbMinInliers     = 30;
    double m_orbMinInlierRatio = 0.55;
    double m_pixdiffThreshold = 0.00000001;
    double m_areaThreshold = 0.000001;

    // worker (assume this exists and scans; uses std::thread internally)
    class DedupWorker *m_worker = nullptr;

    struct TaskQueue {
        std::queue<std::shared_ptr<PairInfo>> q;
        std::mutex mtx;
        std::condition_variable cv;
        bool finished = false;
    };

    TaskQueue m_queue;
    QThreadPool *m_pixelThreadPool = nullptr;
    /*
     * 显示图预取专用池(2 线程)。必须是窗口自己的池:析构里 waitForDone 才管得住
     * 在飞的预取任务(以前用全局池,窗口没了任务还在碰 m_files)。
     */
    QThreadPool *m_prefetchPool = nullptr;
    /* 析构开始后不再接受新的后台派发(见 ~ImageDeduplicateWindow) */
    std::atomic<bool> m_tearingDown{false};
    // track queued/processing pixel tasks
    std::mutex m_pixelQueuedMutex;
    std::unordered_set<std::shared_ptr<PairInfo>> m_pixelQueued;
    // runtime flags controlled by MainWindow's checkboxes
    std::atomic<bool> m_enablePixelDiff{true};
    std::atomic<int> m_dataEpoch{0};

    // helpers
    void clearState();//应外部加锁
    void sortPairsFromIndex(int startIndex,int epoch);
    /*
     * 目录组(A、B 两张图各自的**直属父目录**组成的目录对)的排序权重。
     * 调用方必须已持有 m_mutex。
     *
     * 键 = pairDirSortKey()(规范化方式与 dirPairKeyOf() 相同,但粒度是
     * 直属父目录,不是顶层子目录),值里带 covered/avgBest,
     * 排序时用 score() 从大到小。评分规则见 foldergroup.h(纯函数,有单测)。
     */
    QHash<QString, foldergroup::Stat> folderGroupStats() const;
    /* 跳过(jump)过的目录对 -> 目录对键集合(方向无关) */
    QSet<QString> jumpedFolderPairKeys() const;
    /*
     * 把"被 jump 的目录对"之间的图片对从 m_pairs 里剔掉,返回剔除数量。
     *
     * 语义:FolderCompareWidget 里按了 jump = 这两个目录之间**不做任何比较**,
     * 不是"留到后面当散图处理"。以前它们会以"没有合集方向"的身份尾插到列表末尾,
     * 用户还是得逐对看完 —— 那等于 jump 白按了。
     *
     * 调用方必须已持有 m_mutex(排他)。只在目录决策收尾时调用一次。
     */
    int dropPairsBetweenJumpedFolders();
    /* 目录组排序的日志去重签名(同样的顺序不重复刷) */
    QString m_lastFolderRankLog;
    void buildPairsFromFiles(int epoch); // forms m_pairs & m_pairOrder using pHash/dHash criteria 应外部加锁
    /* remove cover:按 m_explicitPairs 直接建 pair(不做阈值筛选) 应外部加锁 */
    void buildPairsFromExplicitList(int epoch);
    std::vector<int> collectPrefetchPairIndices(int centerIndex);
    /*
     * 跳到指定pair并刷新主显示区。
     * listScrollMode: 传给列表的选中同步。
     *   用户直接在列表里点选 -> ScrollNever(别动他的滚动位置);
     *   按钮/快捷键/批量后自动前进 -> ScrollAlways(目标可能离得很远,需要滚过去)。
     * 应外部加锁(含index锁)。
     */
    void displayPairByIndex(int index,int epoch,
                            DuplicatePairListView::ScrollMode listScrollMode
                                = DuplicatePairListView::ScrollAlways);
    void displayPair(std::shared_ptr<PairInfo> pairptr,int epoch);
    /*
     * 后台把"当前显示的pair"的显示图准备好之后,回 UI 线程把图换上。
     * pairIndex 对不上 m_currentIndex(用户已经切走)就直接丢弃。
     */
    void applyDisplayImagesFromCache(int pairIndex, int epoch);
    /*
     * 显示图确实拿不到(解码失败/文件不存在/超时)时的唯一收尾口。
     *
     * 铁律:显示区必须是【空白】,不能留着上一对的图。
     * 以前超时分支只是"贴侧栏 + 解锁按钮",屏幕还是上一对 ——
     * 用户看着 A 按下 remove,删掉的却是 currentIndex 指向的 B。
     *
     * 由后台任务通过 QueuedConnection 调用;detail 会进日志。
     * 只在 UI 线程调用。
     */
    void onDisplayPairLoadFailed(int pairIndex, int epoch, const QString &detail);
    /*
     * 把显示区清空 + 角标/侧栏标成"图片无法显示"或"加载中"。
     * 调用方必须已经持 m_mutex 共享锁(displayPair 里就是这么调的),
     * 或者在 UI 线程且确定没有后台正在改 m_files(3 秒兜底/失败回调)。
     * 不等待 paint:清空是同步生效的,后面那次 paint 只会帮忙确认状态。
     *
     * stillLoading=true:后台任务还在跑,只是超过 3 秒没上屏 ——
     * 画面同样必须清空(不能留上一对),但文案是"加载中",不能吓唬用户去删图。
     */
    void showPairImagesUnavailable(int pairIndex, const QString &detail,
                                   bool stillLoading = false);
    /*
     * 超时/失败日志里带上这一对的 A/B 路径。
     * 只在 timer 回调里用(自己拿 m_mutex 共享锁),不能从已经持锁的上下文调用。
     */
    QString displayPairPathsForLog(int pairIndex);
    /*
     * 同步"预加载文件缓存"(cachedFileIndices):窗口内缺的加载、窗口外多的释放。
     * 调用方必须已持有 cacheMutex。
     * 真正的加载/释放本来就是丢给 m_pixelThreadPool 的,所以这段可以整体挪到后台做。
     */
    void syncFileCacheForWindowLocked(const std::unordered_set<int> &neededFileIndices, int epoch);
    /*
     * 每次切图递增:被推迟到后台的文件缓存同步靠它判断"自己是不是已经过时了",
     * 过时的直接跳过,免得用一个旧的窗口集合去加载/释放。
     */
    std::atomic<int> m_fileCacheSyncSeq{0};
    void computePixelForPair(std::shared_ptr<PairInfo> pairptr,int epoch);
    void requestComputePixelForPair(std::shared_ptr<PairInfo> pairptr,int epoch);
    void computePixelForPairCore(std::shared_ptr<PairInfo> pairptr,int epoch);
    /*
     * pixel diff 失败的统一收尾:累加失败计数、写日志(界面上看得见)、
     * 决定"继续重试"还是"锁成 1 停止重试"。返回 true 表示已锁成 1。
     * 以前失败一次就直接落 computed=true + ratio=1,于是那一对永远显示 1 且不再重算。
     */
    bool handlePixelDiffFailure(std::shared_ptr<PairInfo> pairptr, const QString &reason);
    /*
     * pixel diff 出结果之后把结果送回 UI(后台线程调用,内部走队列投递):
     * 刷新列表里对应那一行 + (若这一对正在显示)信息面板。
     * 以前只有"当前显示的那一对"会被刷新,列表行会一直停在 computing...。
     */
    void notifyPixelDiffUpdated(std::shared_ptr<PairInfo> pairptr, int epoch);
    int findNextPending(int fromOrderIndex = -1);//应外部加锁
    void applyActionToPair(std::shared_ptr<PairInfo> pairptr, ActionRecord::Type type,
                           QVector<int>* changedRows = nullptr);//应外部加锁
    /* 把pair恢复到Pending:把已移到duplicate的文件搬回来,状态设回Pending */
    /*
     * 把一对恢复成 Pending(并把当初移进 duplicate 的文件搬回来)。
     *
     * 只对"自己做过决定"的对有效 —— Invalid 的对不能走这里:
     * 它的文件是被【另一对】移走的,在这里恢复会把那一对的 remove 一起撤销
     * (文件回原位、相关对重新变 Pending),而那一对的状态还停在 RemovedA/B,
     * 磁盘和列表从此对不上。返回 false 表示被拒绝。
     */
    bool restorePairToPending(std::shared_ptr<PairInfo> pairptr);//应外部加锁
    void markRelatedPairsValidState(const FileEntry& file,std::shared_ptr<PairInfo> currentPair, bool valid, QVector<int>* changedRows = nullptr);//将所有其它包含file的图片对标记为invalid(当valid=false时)

    /* ===== 文件移动异步化(remove/恢复 的物理 rename 不再阻塞 UI 线程) ===== */
    /*
     * 一次待执行的文件移动。UI 线程只做"乐观更新"(改 FileEntry 的 currentPath/
     * inDuplicate 和 pair 状态),物理 rename 由单线程池串行执行 ——
     * 单线程保证同一文件的操作严格按入队顺序执行,不会互相踩。
     */
    struct PendingFileOp {
        int fileIdx = -1;
        QString from;        // 执行时文件应在的位置(也是失败回滚的依据)
        QString to;          // 目标位置
        bool restore = false;    // true: duplicate -> 原位置;false: 原位置 -> duplicate
        int epoch = 0;           // 数据代:过期任务的结果直接丢弃
        // 失败回滚 pair 状态用的上下文(仅当状态还停留在乐观值时才回滚)
        std::shared_ptr<PairInfo> pair;
        int optimisticState = -1;    // 入队时乐观写入的 PairState 整数值,-1 表示不用回滚状态
        int rollbackState = -1;      // 失败时写回的状态(入队前的旧状态)
    };
    std::deque<PendingFileOp> m_fileOpQueue;    // FIFO 队列(由 m_fileOpMutex 保护)
    std::set<int> m_fileOpInFlight;             // 有在途操作的 fileIdx(修正 exists() 判断)
    /*
     * 已经被在途操作"预定"的目标路径。
     * moveToDuplicate 生成唯一文件名时要查磁盘是否存在,可文件还没真的搬过去,
     * 只查磁盘会让两个文件算出同一个目标名。这里把已预定的路径也纳入冲突判断。
     */
    std::set<QString> m_fileOpReservedTargets;
    mutable std::mutex m_fileOpMutex;//mutable:hasPendingFileOp 是 const 方法,要在 const 上下文里查在途表
    QThreadPool *m_fileOpPool = nullptr;        // maxThreadCount=1,串行执行
    bool m_fileOpWorkerActive = false;          // 是否已有执行线程在消耗队列(由 m_fileOpMutex 保护)
    /*
     * 单对动作已经提交、正在等后台 rename 落定的那一对(nullptr = 没在等)。
     *
     * 为什么要有它:remove 是乐观更新(state 先改成 Removed*,文件在后台搬),
     * 以前不管搬没搬成都会立刻 findNextPending 前进。万一 rename 失败
     * (最常见的是显示图/缩略图线程正打开着这个文件,Windows 上就是共享冲突),
     * handleFileOpResult 会把 state 回滚成 Pending —— 而用户已经走到后面去了,
     * 列表上方就留下一个"看着是 Pending、其实已经处理过"的孤儿对。
     * 这就是 2026-11-20 用户报的"find next pending 会跳 pair"。
     *
     * 现在:凡是产生了文件操作的单对动作,都用这个标记把"前进"挂起;
     * 落盘成功才走,失败就留在原地(播 Error 音 + 日志),由用户重试或跳过。
     */
    std::shared_ptr<PairInfo> m_awaitFileOpPair;
    /*
     * 每一对还有几个文件操作没落定(remove both = 2)。
     * 只在 UI 线程读写:enqueueFileOp / handleFileOpResult 都在 UI 线程。
     */
    QHash<const PairInfo *, int> m_pairFileOpsInFlight;
    /* 该文件是否有还没执行完的移动操作(供 exists() 类决策使用) */
    bool hasPendingFileOp(int fileIdx) const;
    /* 把一个操作入队并(必要时)唤醒执行线程 */
    void enqueueFileOp(const PendingFileOp &op);
    /* 后台串行执行:物理 rename + 结果回投 UI 线程 */
    void executeFileOps();
    /* UI 线程处理执行结果:失败时回滚 FileEntry / pair 状态并报错 */
    void handleFileOpResult(const PendingFileOp &op, bool ok,
                            const QString &err, const QStringList &notes);

    /*
     * 把文件移到 duplicate 目录 / 从 duplicate 恢复。
     * 这两个函数现在只负责"规划 + 乐观更新 FileEntry + 入队",物理 rename 由后台池执行,
     * 所以 UI 线程不会再被大文件的跨盘移动卡住。
     * 返回 false 只表示"连计划都做不了"(参数不合法);返回 true 表示已受理,
     * 真正的失败通过 handleFileOpResult 异步回滚并记录日志。
     * ctxPair/optimisticState/rollbackState 用于失败时把 pair 状态回滚。
     * 调用方必须已持有 m_mutex。
     */
    bool moveToDuplicate(int fileIdx, std::shared_ptr<PairInfo> ctxPair,
                         int optimisticState = -1, int rollbackState = -1);
    bool restoreFromDuplicate(int fileIdx, std::shared_ptr<PairInfo> ctxPair,
                              int optimisticState = -1, int rollbackState = -1);
    /*
     * 追加一行日志(窗口里的 pte_log + 同步给 MainWindow 的 LogWidget)。
     *
     * level 与 LogWidget::Level 对齐:0=Info 1=Detail 2=Good 3=Warning 4=Error,
     * 决定日志窗口里的颜色、OK 按钮状态以及要不要响提示音。
     *
     * isAuto 只负责加 "[auto]" 前缀,**不再代表"这条是警告"**:
     * 自动移重复、放弃筛选、批量处理完成都是正常流程,以前它们被一律当成
     * Warning(level 3),于是日志窗口一有风吹草动就弹出来 + 叫一声 + 顶到最前,
     * 用户的原话是"太敏感了,正常操作也报警告"。真正该叫醒用户的
     * (文件移动失败、图片读不出来、像素比较失败)现在显式传 level。
     */
    void logLine(const QString &s, bool isAuto = false, int level = 0);
    void updateInfoPanelForPair(std::shared_ptr<PairInfo> p, bool showingAisLeft);

    /*
     * 侧栏里 8 个"随当前对变化"的字段(文字 + 颜色)。
     *
     * 为什么要先算好、晚点再贴:贴的时机必须是"新图真正上屏"
     * (onDisplayImagePainted),否则会出现"旧图 + 新数字"。但 paintEvent 里
     * 不能去拿 m_mutex —— 后台 pix diff / 排序会长期持有排他锁,
     * 上一版用 try_lock 抢不到就放弃回填,结果侧栏直接空掉(黑),
     * 而且之后再没有新的 paint,只有切 A/B 才会重新填。
     *
     * 现在:在【持锁的上下文】里(displayPair / toggleAB)先把文字算成纯字符串,
     * paint 回调里只做 setText —— 完全不碰 m_pairs / m_files,不需要任何锁。
     */
    struct InfoField { QString text; QString color; };   // color 为空 = 不改颜色
    QVector<InfoField> makeInfoFields(const std::shared_ptr<PairInfo> &p, bool showingAisLeft) const;
    void applyInfoFields(const QVector<InfoField> &fields);
    QVector<InfoField> m_pendingInfo;
    bool m_pendingInfoValid = false;

    /*
     * 切图等待期:清空侧栏信息 + 锁定七个操作按钮。
     *
     * 只"禁止切换 currentIndex"是不够的:屏幕上还是上一对的图,用户没反应过来
     * 就会把上一对当成下一对来操作(按下 remove 的那一瞬间下一对加载好了,
     * 删的就成了另一对的文件)。所以在等待期间必须给出强提示 ——
     * 侧栏清空、按钮全部置灰,直到新图真正 paint 出来才恢复。
     */
    void clearSideInfoForLoading();
    /*
     * 侧栏最下面那一行(lb_diff_rate 平时不用)拿来做状态提示:
     *   "切换中: pair 12 / 340"        —— 新图还没上屏
     *   "加载超时: pair 12"            —— 兜底放弃
     *   ""                            —— 正常状态,清空
     *
     * 侧栏清空时如果只剩一片空白,看起来像坏了;给一行明确的文字,
     * 用户才知道"这是在等图",而不是"程序出问题了"。
     */
    void setPairLoadingHint(const QString &text);
    /*
     * 开始一次切换时的可见提示(侧栏那一行 + 显示区左下角角标)。
     *
     * 两个地方都要有,因为它们的"可见性"互补:
     *   · 侧栏只有 300px 宽,眼睛在图中间时根本不看那边;
     *   · 角标画在图面上,但用户可能已经放大拖到别的位置。
     * 关键是这两个提示都与"图片内容"无关 —— 相邻两对图长得一模一样时,
     * 画面本身给不出"切没切"的信号,只能靠它们。
     */
    void beginPairSwitchHint(int pairIndex);
    /*
     * 角标上显示的"第几对 / 共几对"。
     * 存在成员里而不是每次去读 m_pairs.size():onDisplayImagePainted 跑在
     * paintEvent 里,不能碰 m_pairs(后台排序可能正在整体搬它)。
     */
    int m_displayPairNumber = 0;
    int m_displayPairTotal  = 0;
    void setPairActionButtonsEnabled(bool on);
    bool m_pairActionButtonsLocked = false;
    /* 锁定前的可用状态(解锁时原样恢复,别把筛选模式里的置灰冲掉) */
    std::vector<std::pair<QPointer<QPushButton>, bool>> m_pairActionButtonsPrev;
    void updateWindowTitleAndBackground(bool showingAisLeft);
    void toggleAB(); // switch shown pixmap, preserve viewport 应外部加锁

    /* ===== DuplicatePairListView 相关 ===== */
    /* 用当前m_pairs重建整个列表 */
    void rebuildPairList();//应外部加锁
    /*
     * 同步列表选中到m_currentIndex(调用方必须已持有currentIndexMutex)。
     * scrollMode: 见 DuplicatePairListView::ScrollMode。
     *   - 程序性跳转(按钮/快捷键/批量后跳转到下一个Pending)用 ScrollAlways;
     *   - 只是"选中态需要跟上"时用 ScrollNever,别去动用户的滚动位置。
     */
    void syncPairListSelectionNoLock(DuplicatePairListView::ScrollMode scrollMode
                                         = DuplicatePairListView::ScrollAlways);
    /* 同步列表选中到m_currentIndex(自己加currentIndexMutex) */
    void syncPairListSelection(DuplicatePairListView::ScrollMode scrollMode
                                   = DuplicatePairListView::ScrollAlways);
    /* 所有pair都处理完时的收尾:m_currentIndex停在越界位置(调用方须持有currentIndexMutex) */
    void showNoMorePairs();//应外部加锁(含currentIndexMutex)
    /*
     * 只刷新列表里某个pair那一行(状态/pixdiff/指标)。
     * 单个pair的按钮/快捷键操作走这条路。
     */
    void refreshPairListRow(int pairIndex);
    /*
     * 只刷新"这次状态变化真正影响到的那几行"(被操作的一对 + 因它而 Invalid/恢复的行),
     * 不整表重建。
     *
     * 为什么需要它:finalizePairStateChange() 里的 rebuildPairList() 是 O(全表),
     * 35000 对的会话里每点一次按钮都要重排一遍模型 —— 这是单键操作里唯一的
     * O(N) 工作,也是"点一下卡半天"最可疑的来源。行级刷新是 O(受影响行数),
     * 通常个位数,而且不会把已经加载好的缩略图连同滚动位置一起丢掉。
     */
    void finalizePairStateChangeRows(const QVector<int> &rows,
                                     DuplicatePairListView::ScrollMode scrollMode);

    /* 把 m_currentIndex 往后/往前挪一个"有效(非 Invalid)"的对;不含业务动作 */
    void goToNextPair();
    void goToPreviousPair();
    /*
     * 全局导航快捷键(2026-11-22 按用户要求统一):
     *   Ctrl+A          -> 选中右侧 pair 列表的**全部行**(筛选里 = 全部筛出来的对)
     *   ← / →           -> 上一对 / 下一对(永远作用于中间的大图,和焦点在哪无关)
     *   Shift+← / →     -> 上一个 / 下一个目录对比区域
     *
     * 返回 true 表示"这个按键已经被消费",调用方不要再处理。
     * 用途:窗口自己的 keyPressEvent、图片控件/列表的事件过滤器共用同一份判定,
     * 避免"焦点在按钮上就不认快捷键"这种按控件分裂的行为。
     */
    bool handleNavigationShortcut(QKeyEvent *ke);
    /* 选中右侧列表的全部行(Ctrl+A);焦点在哪都不影响,选择集永远给列表 */
    void selectAllPairsInList();
    /*
     * 目录去重模式的 next 变体:跳到下一个 Pending 且"直属父目录对"和
     * 当前这一对不一样的对 —— 也就是下一个目录对比区域。
     * 普通的 next 只往后挪一对,同区域里还剩一串同目录对时要按很多次;
     * 摆位/排序是按目录对优先的,所以"区域变了"用目录键判定即可。
     */
    void goToNextFolderRegionPair();

    /*
     * 反向的"目录区域跳转":回到上一个目录对比区域的**最靠下的一条有效对**
     * (只要求有效,不要求 Pending —— 回看就是要看已处理过的那批)。
     * 那个区域一对有效对都没有(全被删/失效)时退回到它最靠下的那一行。
     * 快捷键 Shift+←,右键只在 previous 按钮上(落点规则见 folderregionjump.h)。
     */
    void goToLastFolderRegionPair();
    /*
     * 对"当前显示的那一对"施加动作,然后前进到下一个待处理对。
     * 四个单键操作(remove A/B/both/jump)共用这一条路径,带慢操作耗时日志。
     */
    void applyActionToCurrentPair(ActionRecord::Type type);
    /*
     * 按钮 / 快捷键的总入口:按列表的选择集决定是"单对"还是"批量"。
     *
     *   · 选中的对 > 1  -> 对整个选区批量处理(和右键批量菜单同一个实现),
     *                      播"批量"提示音(bubble pop);
     *   · 否则          -> 原来的 applyActionToCurrentPair(只处理当前那一对),
     *                      播"单对"提示音(pop)。
     *
     * 以前 a/s/d/空格 和四个操作按钮只认 m_currentIndex,用户在列表里
     * 框选了一片再按 remove A,只有屏幕上那一对被处理,选区白选了。
     */
    void dispatchPairAction(ActionRecord::Type type);
    /*
     * 目录方向还没定(FolderCompareWidget 开着)时,禁止一切会改状态/搬文件的操作。
     *
     * 这期间 A/B 谁在左、完全重复归谁都没确定,现在删掉的文件很可能正是
     * 用户下一步选"保留 B"时想要的那一份。返回 true 表示"已经拦下并提示过了"。
     */
    bool blockedByFolderDecision(const QString &what);

    /*
     * 操作按钮(btn_rm_a / btn_rm_b / btn_rm_both / btn_jmp)的**右键**菜单。
     *
     * 左键语义不变(remove A/B/both、jump 这一对),右键只给**这个按钮自己的**
     * 范围变体 —— 不是一个"什么都有"的大菜单:
     *
     *   btn_rm_a    : 筛选:与 A 这张图相似的所有配对 | Remove A 整个目录(目录名)
     *   btn_rm_b    : 筛选:与 B 这张图相似的所有配对 | Remove B 整个目录(目录名)
     *   btn_rm_both : 筛选:A 和 B 的所有配对(结束后 remove both) | Remove Both 两个目录
     *   btn_jmp     : Jump This File | Jump These Folders
     *
     * 共用的底层还是 startSimilarityFilter / removeFolders / jumpForPair,
     * 只是入口放在按钮上,手已经在按钮上时不必再跑去列表里找那一行。
     */
    void setupActionButtonContextMenus();
    /* 按钮右键:role 0/1/2 = rm_a/rm_b/rm_both,3 = jump,4 = next 的目录区域跳转 */
    void showPairButtonMenu(QWidget *button, int role, const QPoint &globalPos);

    /*
     * 一次右键菜单需要的全部快照。**在锁内取好**,弹菜单期间不再碰 m_pairs ——
     * 菜单是嵌套事件循环,持锁弹菜单会让后台排序/像素线程全堵在锁上。
     *
     * 【为什么还存 pair 指针】
     * 光存下标是不够的:菜单 exec() 是嵌套事件循环,期间后台 sortPairsFromIndex
     * 可以拿排他锁整表重排 m_pairs,等用户点下菜单项时,同一个下标已经指向
     * 另一对 —— "Remove A 整个目录"这种动作会被放大成整批不相干目录的误删。
     * 所以 trigger 执行时必须用 shared_ptr 反查【当前】下标(见
     * indexOfPairPointer),而不是直接用弹菜单那一刻的下标。
     */
    struct PairMenuCtx {
        int index = -1;
        std::shared_ptr<PairInfo> pair;        // 右键的那一对(反查用)
        QVector<std::shared_ptr<PairInfo>> pairTargets;   // 批量目标(反查用)
        int state = 0;
        int cause = -1;                 // 导致失效的那一对 invalidCauseForA-1
        QString pathA, pathB;
        QString dirA, dirB;             // 两张图各自的直接父目录(小写规范路径)
        QString nameDirA, nameDirB;
        bool hasA = false, hasB = false;
        bool aGone = false, bGone = false;   // 这一侧已经进 duplicate
        bool inFilter = false, waitingFolder = false, filterSupported = false;
        bool invalid = false, folderOk = false, multi = false;
        bool aAlive = false, bAlive = false;   // 这一侧还在原处(可以下 remove 决定)
        bool canRestore = false;               // 本对删过图 / 没被牵连失效 -> 允许恢复
        int invalidMask = 0;            // 哪一侧被别的对移走(见 PairInfo::invalidMask)
        QVector<int> targets;
    };
    PairMenuCtx collectPairMenuCtx(int pairIndex, const QVector<int> &multiTargets);
    /*
     * 按 pair 指针反查【当前】下标(菜单 trigger 专用)。
     *
     * 菜单弹着的时候后台排序可能已经把 m_pairs 整表重排,弹菜单那一刻记下的
     * 下标已经不可信;shared_ptr 指向的 pair 对象本身不会变,所以用指针反查。
     * 返回 -1 表示这一对已经不在 m_pairs 里(被剔除/换代),调用方必须放弃动作。
     * 内部自己拿 m_mutex 共享锁,调用时【不要】持锁。
     */
    int indexOfPairPointer(const std::shared_ptr<PairInfo> &p);
    QVector<int> indicesOfPairPointers(const QVector<std::shared_ptr<PairInfo>> &ps);

    /*
     * 列表行的右键菜单:所有选项**平铺成一列**,用分隔线分成
     * Remove A / Remove B / Remove Both / Jump / 状态 / 打开 / (失效时)肇事对 七段。
     * pairIndex < 0 表示"当前显示的那一对";multiTargets 多于一个时按批量处理。
     */
    void showPairActionsMenu(int pairIndex, const QVector<int> &multiTargets,
                             QWidget *anchor, const QPoint &globalPos);
    void onPairListContextMenuRequested(int pairIndex, const QVector<int> &targets,
                                        const QPoint &globalPos);
    /*
     * Jump 的作用域(pairIndex 可以是列表里任意一对,不必是当前显示的那一对)。
     *   File     : 跳"包含这张图"的全部未处理对。
     *              当前显示的那一对按【屏幕上是哪一张】决定(A/B 可用空格切换),
     *              其它行没有"当前显示"的概念,固定用 A。
     *   TwoFiles : 跳"包含 A 或 B 任意一张"的全部未处理对 ——
     *              一次就能把"这张图和这张图都不要再看了"表达清楚,
     *              不用先切到 A 跳一次、再切到 B 跳一次。
     *   Folders  : 跳这两张图各自【直接父目录】之间的全部未处理对。
     */
    enum class JumpScope { File, TwoFiles, Folders };
    void jumpForPair(int pairIndex, JumpScope scope);

    /*
     * "Jump Pendings":把选中的这些对里【仍然是 Pending 的】标记为 Skipped。
     *
     * 和 batch 的 SetSkipped 的区别(也是它存在的理由):
     *   batch 会对 RemovedA/B/RemovedBoth 也套用 Skip —— applyActionToPair 的
     *   Skip 分支会顺手把已经搬进 duplicate 的文件【搬回来】(restoreIfNeeded),
     *   等于把用户之前的 remove 决定抹掉。
     * 这里只碰 Pending:已经 remove 过的对原样保留,一个文件都不动。
     */
    Q_INVOKABLE void jumpPendingPairs(const QVector<int> &pairIndices);

    /*
     * folder 级 remove(右键菜单 "Remove A/B/Both:这两个目录之间的全部未处理对")。
     *
     * 作用域和 Jump These Folders 完全一致:两张图各自的**直接父目录**之间的
     * 全部未处理对。区别只是动作是"删"而不是"跳过"。
     * side: 0 = 删掉这些对里落在 A 侧目录的图,1 = 落 B 侧目录的图,2 = 两侧都删。
     */
    void removeFolders(int pairIndex, int side);

    /* ================= "筛选出与这张图相似的所有配对" ================= */
    /*
     * 设计目的只有一个:**一次把"这张图 + 和它相似的所有版本"整族删掉**。
     *
     * 场景:看见一张不想要的图 a,它在若干对里各有一个"相似版本"(a-b、a-c、a-d…)。
     * 直接 remove both 只带走 a-b 里的 b,剩下的 a-c、a-d 因为 a 没了而变成
     * Invalid,再也没法处理(这就是"删不干净")。
     *
     * 所以筛选的语义(2026-11-22 按用户要求重做)是:
     *
     *   1) **进入时立刻对原对执行进入时选的那个动作**(remove A / remove B / remove both)。
     *      不再"等退出时统一删锚点"—— 那种延迟让原对一直挂着 Pending,看得见却动不了。
     *      锚点(被删掉的那张图)随即在所有兄弟对里失效,而那些兄弟对的**另一侧**
     *      正是要收拾的相似版本。
     *   2) 列表 = 所有"含锚点、且另一侧还活着"的**其它**对(原对不再列进来:它已经
     *      按用户的选择处理完了)。每行的锚点钉在 A 位,唯一要做的动作是删掉 B。
     *   3) **只看另一侧还在不在**,不看这一对的状态:InvalidA(锚点被删)、
     *      RemovedB、甚至已经 RemovedBoth 的对,只要还有一张活的相似图,就要能被处理。
     *      否则"整族删除"必然留尾巴。
     *   4) 没有任何其它成员(或那些对的另一侧都已经删掉)时**不进筛选**:
     *      直接对原对执行动作、提示一句、跳到下一个 Pending —— 一个孤立对没必要
     *      进一个只有一个可处理项的"筛选界面"。
     *
     * 锚点本身已经在 duplicate/ 里的情况(筛一张已经删掉的图)同样成立:
     * 不再重复删它,只收拾还活着的相似版本。见 filterMemberUsable()。
     */
    enum FilterEntryMode {
        FilterFromA = 0,    // 从 remove A 进:锚点是 A 侧那张图
        FilterFromB = 1,    // 从 remove B 进:锚点是 B 侧那张图
        FilterFromBoth = 2  // 从 remove both 进:A、B 两张都是锚点
    };

    struct FilterMember {
        int  pairIndex = -1;   // m_pairs 下标
        int  anchorFile = -1;  // 这一行里被钉在 A 位的图片
        bool swapped = false;  // A/B 是不是我们换过来的(退出时要换回去)
    };

    /*
     * 进入筛选。返回 true = 真的进了筛选界面;
     * false = 没进(已经在筛选里 / 目录方向未定 / Deferred / 没有其它成员)——
     * 最后一种情况下原对的动作**已经执行**,并已前进到下一个 Pending。
     * pairIndex 是右键/当前那一对;mode 见 FilterEntryMode。
     */
    bool startSimilarityFilter(int pairIndex, FilterEntryMode mode);
    /*
     * Shift+A / Shift+S / Shift+D 的入口:以**当前显示的那一对**为锚点进入筛选
     * (分别对应 FilterFromB / FilterFromBoth / FilterFromA)。
     *
     * 为什么只认 m_currentIndex、不看列表多选:
     * 筛选的锚点是"一张图"(filter both 是两张),整个列表会被重组成
     * "包含这张图的配对";多选在这里没有意义 —— 那和 remove A/B/both 的
     * "对选中的每一对都做一次"正好是相反的语义。
     */
    void startFilterFromCurrentPair(FilterEntryMode mode);
    /*
     * 一对能不能进筛选(anchorFile 为要钉住的那张图)。**调用方必须持有 p->stateMutex。**
     * 唯一条件:**另一侧还在**(还有一张相似图可删)。不看这一对的状态 ——
     * 失效、已删过一侧、甚至 RemovedBoth,只要另一侧活着就都得列出来。
     */
    bool filterMemberUsable(const std::shared_ptr<PairInfo> &p, int anchorFile) const;
    /* 交换一对的 A/B(连同 state / invalidCause / removedMask 一起翻) */
    void swapPairSides(const std::shared_ptr<PairInfo> &p);
    /* 筛选状态下 remove B / remove both 的实现(只动"另一侧") */
    void filterApplyAction(ActionRecord::Type type);
    /*
     * 筛选状态下"只对某一行"的动作(右键菜单/单行)入口。
     *
     * 为什么不复用 onPairListPairAction 的正文:那条路径会 applyActionToPair,
     * 而 applyActionToPair 把 RemoveA 当成"改主意"(先把另一侧恢复回来)。
     * 筛选里"另一侧"正是刚被删掉的相似图,走那条路会把它搬回来。
     *
     * 允许的动作:
     *   RemoveB / RemoveBoth -> 删掉另一侧(锚点已经删了,两者等价)
     *   Skip                 -> 跳过这一行:撤销本对删过的另一侧,标成 Skipped
     *   None                 -> 恢复为 Pending(撤销本行删掉的另一侧)
     * RemoveA 不开放:锚点已经处理过,要改主意请 finished 退出后在原对上改。
     */
    void filterPairAction(int pairIndex, ActionRecord::Type type);
    /*
     * 筛选状态下的批量动作(框选多行 -> 右键 / Ctrl+A 后按 a/s/空格)。
     * 只处理"属于筛选集合"的目标,其余计入 skipped;结束时统一刷一次列表。
     */
    void filterBatchAction(const QVector<int> &pairIndices, ActionRecord::Type type);
    /*
     * 删掉第 pos 个成员里的"另一侧"(钉住之后在 B 位)。
     * navigate:按钮/快捷键路径要自动跳到下一个可处理项;右键路径不动光标。
     */
    bool filterRemoveOtherSide(int pos, bool navigate);
    /*
     * 跳过第 pos 个成员:撤销本对删过的另一侧,状态标成 Skipped。
     * 和"恢复为 Pending"的区别只在收尾状态 —— 用户按空格的意思是"这对不看"，
     * 不是"放回待处理队列"。返回 false 表示这一行不在筛选集合里。
     */
    bool filterSkipMember(int pos, bool navigate);
    /*
     * "jump 这一对"的底层:撤销【本对】删过的那一侧(把文件从 duplicate 搬回来),
     * 再把状态标成 Skipped(保留 invalidMask,于是显示成 "Skipped + InvalidA")。
     *
     * 和 restorePairToPending 的唯一区别是收尾状态。两者都必须能被"删过另一侧的
     * 失效对"调用 —— 这正是用户报的"removed B 的对要 jump,只能先恢复 Pending"
     * 的那条路。调用时【不要】持有 p->stateMutex:内部自己拿,而且要回调
     * markRelatedPairsValidState 去修正兄弟对。
     */
    bool markPairSkippedUndoingOwnRemoval(const std::shared_ptr<PairInfo> &p);
    /*
     * 只移除一对的某一侧,绕过 remove A/B 的"先恢复另一侧"语义。
     * changedRows(可选):把"被牵连改了状态的其它行"收集进去 ——
     * 行级刷新(updatePairRow)需要它,否则兄弟行要等下一次整表重建才更新。
     */
    bool removeSideOfPair(const std::shared_ptr<PairInfo> &p, bool sideA,
                          QVector<int> *changedRows = nullptr);
    /* 退出筛选:交换回 A/B、恢复列表与按钮、回到原位(文件动作早在进入时就做完了) */
    void finishSimilarityFilter();
    /* 当前下标在筛选集合里的位置;-1 表示不在 */
    int filterMemberPos(int pairIndex) const;
    /* 筛选集合里从 pos 往后/往前找下一个"还能处理"的成员;没有返回 -1 */
    int nextActionableMember(int fromPos, int step) const;
    /*
     * 重建筛选列表(只含成员)。成员数量小,直接整表重建比逐行算"谁被牵连失效"
     * 更省事也更不容易漏:删掉某一侧会把引用同一张图的兄弟行标成 Invalid。
     */
    void refreshFilterList();
    void updateFilterUi();   // 标题 / 按钮文案

    struct PairFilterSession {
        bool active = false;
        FilterEntryMode mode = FilterFromA;
        /* 本次的锚点(A 侧 / B 侧 / 两侧各一张)。都是"进入时已经删掉"的图 */
        QVector<int> anchorFiles;
        /* 进入筛选时的那一对(退出要回到它;动作在进入时就已经落在它身上) */
        std::shared_ptr<PairInfo> originPair;
        int originPairIndex = -1;
        /* 只含"其它"对:原对不在里面(reason 见上面的大段注释) */
        std::vector<FilterMember> members;
    };
    PairFilterSession m_filter;

    /* ================= 半选 internal:目录级"合集优先"判定 ================= */
    /*
     * 扫描完成后(持有 m_mutex)对每个 internalMode==AcrossTopDirs 的输入目录
     * 做一次顶层子目录两两比较:能自动定方向的写进 m_folderKeep;
     * 需要人工的排进 m_folderManualViews,扫描结束后弹 FolderCompareWidget。
     */
    /* 是否处于"目录去重模式"(存在半勾选的输入目录) */
    bool isFolderCompareMode() const;
    void analyzeInternalFolders();
    /* 4.1:合集侧保留,把子集侧与合集完全重复的图片直接移入 duplicate */
    void autoMoveExactDuplicates();
    /*
     * 像素完全相同(pix diff == 0)但字节不同的对,【不】在这里自动搬 ——
     * 它们留在比较列表里由用户的 auto remove 处理(留小的 / 勾 Ignore Size 则
     * 固定删 B),这样"留哪一份"由用户拍板,处理结果也计入人工归属统计。
     * 详见 imagededuplicatewindow.cpp 里原 autoRemovePixelIdenticalPairs 位置的说明。
     */
    /*
     * 人工定完方向后,把"现在有方向"的字节级完全重复对从列表剔除并搬走。
     * (建对时人工目录对还没有方向,那时它们是照常建对的)
     */
    int pruneDirectedExactDuplicates();
    /* 资源文件(PSD/ZIP/GIF/MP4 等)只做二进制去重;辅助文件忽略。返回移走数量 */
    int  dedupeResourceFiles();
    /*
     * 4.2:去重收尾 —— 按两侧移出数量决定是否把完全重复文件恢复回子集侧。
     *
     * 触发时机(唯一标准):【窗口关闭】或【进入下一轮】。
     * 刻意不在"所有 pair 都处理完"时做:那一刻只说明列表里没有 Pending 了,
     * 用户完全可能还没做完(回看、筛选、改主意),提前定案会把后续操作锁死
     * (m_autoDupFinalized 只跑一次)。和 deduplicate_import 的判断一致 ——
     * 懒得多处理就直接关窗,此时照样要收尾。
     *
     * uiRefresh=false 用于关窗/换数据集:此时列表马上要销毁或清空,不必重建刷新。
     */
    void finalizeExactDuplicateOwnership(bool uiRefresh = true);
    /* 依次展示人工队列;队列空时把决定应用到 pair 排序上 */
    void presentNextFolderCompare();
    /* 目录方向全部确定后,才真正建图片对 / 移完全重复 / 显示第一对 */
    void finishFolderDecisionsAndBuildPairs();
    /* 记录一对目录的保留侧(dirKeep 是合集/保留侧) */
    void recordFolderDecision(const QString &dirKeep, const QString &dirSub);
    /* 撤销一对目录的保留侧(用户回退到上一对、准备重新选择时用) */
    void clearFolderDecision(const QString &dirA, const QString &dirB);
    /* 两个文件分属同一半勾选目录的不同顶层子目录时,返回该目录对的保留侧;否则空 */
    QString folderKeepDirFor(int fileIdxA, int fileIdxB) const;
    /* 让"合集侧"排在 A 位(仅普通模式;Deferred/导入整理不改 A/B 语义) */
    void applyFolderOrdering();
    QString topDirPathOfFile(int fileIdx) const;
    /*
     * 文件所在目录的**直属父目录**(小写规范化路径)。
     * 排序分组键(pairDirSortKey)和区域跳转用它;与 jumpForPair 里
     * 原来那份 parentDirOf lambda 是同一口径,收拢成一份实现避免漂移。
     * 路径为空/下标越界返回空串。
     */
    QString parentDirPathOfFile(int fileIdx) const;
    /* 每个顶层目录的递归图片数(摆位用的最后一级指标) */
    QHash<QString, int> m_dirImageCount;
    /* 目录级质量胜负分:按重复对累计(质量 > 大小 > 像素) */
    QHash<QString, int> m_dirScore;
    /*
     * 单张相似对的 A/B 摆位(提示词 §6):
     *   目录级质量分 > 压缩质量 > 文件大小 > 像素数 > 路径深度 > 目录递归图片数。
     *
     * 两个地方共用它:
     *   · buildPairsFromFiles() 建对时决定初始 A/B;
     *   · autoMoveExactDuplicates() 在"目录方向没定"时决定保留哪一侧。
     */
    bool preferFileAsA(int ia, int ib) const;
    /*
     * 半选 internal 模式下图片对的"分组排序键" = A、B 两张图**各自直属父目录**
     * 组成的目录对规范化键(方向无关)。
     * 2026-10-28 起从"顶层子目录对"改成"直属父目录对":排序聚簇的区域和
     * Jump These Folders / "跳转到下一个目录对比区域"必须是同一个粒度,
     * 否则跳过去的"下一个区域"在列表里还和当前区域连在一起。
     * 空串表示至少一侧拿不到路径(理论到不了),排在最前。
     */
    QString pairDirSortKey(int fileIdxA, int fileIdxB) const;
    /*
     * 半选 internal:目录方向确定并建好 pair 之后,把剩下的相似对的 pix diff 一次算完
     * 再排序。调用方必须在"未持 m_mutex"的状态下调用(内部会自建后台线程)。
     */
    void startPixelDiffPhaseForFolderMode(int epoch);

    QHash<QString, QString> m_folderKeep;      // key = 排序后的 "dirA|dirB"(小写) -> 保留目录
    /*
     * 本次扫描内已经算过的 ORB 几何校验结果(key = (小下标<<32)|大下标)。
     *
     * 半选模式下 analyzeInternalFolders() 为了判"哪个目录是合集"已经对目录间的
     * 图片两两跑过 verifyOrbPair(800x800 双向 kNN + RANSAC,单对几毫秒),
     * 紧接着 buildPairsFromFiles() 又会把同一批图再算一遍 —— 纯重复劳动。
     * 这里把第一次的结果留下来给建对复用(只读,建对阶段是并行的)。
     * 换数据集时必须清空(文件下标失效),见 clearState()。
     */
    QHash<quint64, dedup::OrbVerifyResult> m_orbVerifyCache;
    QVector<foldercompare::CompareView> m_folderManualViews;
    QVector<QPair<QString, QString>>    m_folderManualDirs;
    /* 队列里被 jump 掉的下标(左侧列表要显示 [jump]) */
    QSet<int>                           m_folderManualSkipped;
    int m_folderManualIndex = 0;
    /* true = 还在等 FolderCompareWidget 的决定,此时不建图片对、不开显示 */
    bool m_waitingFolderDecision = false;
    int  m_folderPendingEpoch = -1;
    /*
     * 完全重复(字节相同)的图片不进图片比较列表：它们不需要人看。
     * 建对时只收集候选(buildPairsFromFiles)，扫描完按合集方向直接移入 duplicate。
     */
    struct ExactDupMove {
        QString dirKey;        // 目录对的 key
        bool    keepIsA = true; // keep 侧是否是对子里的第一个
        int     keepFileIdx = -1;   // 合集/保留侧的文件(留在原地)
        int     movedFileIdx = -1;  // 子集侧的文件(被移进 duplicate)
        bool    moved = false;      // 是否真的移走了(移动失败=没有)
    };
    QVector<ExactDupMove>   m_exactDupMoves;
    QVector<QPair<int,int>> m_exactDupCandidates;   // (a,b) 文件下标，字节完全相同
    bool m_autoDupFinalized = false;
    /*
     * 完全重复归属改判的第二步:把原保留侧驱逐进 duplicate。
     *
     * 这一步【不能】在 finalizeExactDuplicateOwnership 里直接入队 ——
     * restoreFromDuplicate 返回 true 只代表"操作已受理",物理 rename 在后台
     * 串行执行。两步各自入队时,若回迁物理失败而驱逐成功,子集那份仍在
     * duplicate、原保留那份也进了 duplicate,这套作品从工作区整体消失。
     * 现在驱逐被挂到回迁的物理成功回调上(handleFileOpResult):
     *   · 登记键 = 被回迁的 fileIdx,值 = 要驱逐的 keepFileIdx;
     *   · 回迁物理成功 -> 这里弹出并执行驱逐;
     *   · 回迁物理失败 / 数据换代(clearState 清空)-> 放弃驱逐,保留原状。
     * 只在 UI 线程读写(handleFileOpResult 全部经 QueuedConnection 回 UI)。
     */
    QHash<int,int> m_pendingEvictAfterRestore;
    FolderCompareWidget *m_folderCompareWidget = nullptr;
    /*
     * 一次状态变化后的整体收尾:整表重建 + 同步选中项 + 强制重绘。
     * 调用方必须已持有 m_mutex 与 currentIndexMutex。
     * 用于批量操作(auto remove / 右键批量)这种会连带改变很多pair的场景。
     * scrollMode 透传给 syncPairListSelectionNoLock —— 批量操作时请用 ScrollNever,
     * 因为紧接着会由"跳到下一个Pending"自己决定要不要滚。
     */
    void finalizePairStateChange(DuplicatePairListView::ScrollMode scrollMode
                                     = DuplicatePairListView::ScrollAlways);
    /*
     * 批量操作收尾:如果m_currentIndex指向的pair已经不在Pending了(它自己或被连带处理掉了),
     * 就跳到下一个Pending并把主显示区刷新过去;全部处理完则走showNoMorePairs。
     * 这样批量remove后不需要"再手动重复点一次"才能前进。
     * 调用方必须已持有 m_mutex 与 currentIndexMutex。
     */
    void advanceToNextPendingAfterBatch();
    /*
     * ===== 单对动作:等磁盘落盘再前进(2026-11-20) =====
     *
     * 动作提交后调用。如果这一对产生了文件操作(remove/恢复真的入队了),
     * 就把 m_awaitFileOpPair 指到它并返回 true —— 调用方【不要】再 findNextPending。
     * 没有文件操作(例如这一侧早就在 duplicate 里、moveToDuplicate 直接受理)返回 false,
     * 调用方按老流程立刻前进。
     */
    bool holdAdvanceUntilFileOpsSettle(const std::shared_ptr<PairInfo> &pair);
    /*
     * 该对的文件操作全部落定且用户还停在那一对时,前进到下一个 Pending。
     * 用户已经手工点走/关窗则不打扰(只清标记)。
     * 内部自己加锁(m_mutex -> currentIndexMutex),调用方不要再持锁。
     */
    void advanceToNextPendingIfOn(const std::shared_ptr<PairInfo> &pair);
    /* 找 file 所属的 pair 下标(用于日志),-1 表示没找到 */
    int pairIndexOf(const std::shared_ptr<PairInfo> &pair) const;
    /* 找“导致该pair的A/B变成Invalid”的pair下标,-1表示没找到 */
    int findInvalidCausePair(int pairIndex, bool preferA);
    /*
     * 批量把一些pair依次设置为某个动作(前面的先处理)。
     * type为ActionRecord::None表示“恢复为Pending”。
     * 每处理一个前都会重新检查它当时的状态(前面的处理可能让它变成Invalid或变回Pending)。
     */
    void applyBatchAction(const QVector<int> &pairIndices, ActionRecord::Type type);
    /* 懒加载列表缩略图(只在GUI线程调用,后台算) */
    void loadPairListThumbnails(const QVector<int> &pairIndices);

    QString pairRootFolder(int fileIdx) const;

    // alignment helper: returns aligned A & B (larger size) using INTER_LINEAR
    void makeAlignedPair(const cv::Mat &A, const cv::Mat &B, cv::Mat &outA, cv::Mat &outB) const;

    void fileUnloadMat(int fileIdx,int epoch);
    void fileLoadMat(int fileIdx,int epoch);
};

#endif // IMAGEDEDUPLICATEWINDOW_H
