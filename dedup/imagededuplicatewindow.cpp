#include "imagededuplicatewindow.h"
#include "deduppch.h"
#include "deduputils.h"
#include "dedupenv.h"
#include "foldercomparewidget.h"
#include "folderscope.h"   // 目录级 remove / jump 的作用域判定(纯函数,有单测)
#include "folderregionjump.h"   // 目录对比区域之间跳转的落点规则(纯函数,有单测)
#include "paircontextmenu.h" // 平铺分栏的右键菜单(列表行与操作按钮共用)
#include "animations.h"
#include "dupmove.h"
#include <QCryptographicHash>
#include <QDirIterator>

#include <QCloseEvent>   // closeEvent(导入整理:关窗 = 执行计划)
#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <QThread>

#include <cstdio>
#include <chrono>

using namespace std;
using namespace cv;
using namespace dedup;

// small BK-tree for uint64_t with hammingDistance metric
struct BKNode {
    uint64_t val;
    std::vector<int> indices; // file indices that share this hash
    std::unordered_map<int, BKNode*> children; // distance -> child
    BKNode(uint64_t v, int idx) : val(v) { indices.push_back(idx); }
};

class BKTree {
public:
    BKTree() : root(nullptr) {}
    ~BKTree() { clear(root); }

    void insert(uint64_t val, int idx) {
        if (!root) { root = new BKNode(val, idx); return; }
        BKNode* cur = root;
        while (true) {
            int d = hammingDistance(cur->val, val);
            if (d == 0) {
                cur->indices.push_back(idx);
                return;
            }
            auto it = cur->children.find(d);
            if (it == cur->children.end()) {
                cur->children[d] = new BKNode(val, idx);
                return;
            } else {
                cur = it->second;
            }
        }
    }

    // find all indices whose value has hamming distance <= maxDist
    void query(uint64_t val, int maxDist, std::vector<int> &out) {
        queryRec(root, val, maxDist, out);
    }

private:
    BKNode* root;
    void queryRec(BKNode* node, uint64_t val, int maxDist, std::vector<int> &out) {
        if (!node) return;
        int d = hammingDistance(node->val, val);
        if (d <= maxDist) {
            out.insert(out.end(), node->indices.begin(), node->indices.end());
        }
        // children whose edge distance in [d - maxDist, d + maxDist]
        int lo = d - maxDist;
        int hi = d + maxDist;
        for (auto &kv : node->children) {
            int ed = kv.first;
            if (ed >= lo && ed <= hi) queryRec(kv.second, val, maxDist, out);
        }
    }

    void clear(BKNode* node) {
        if (!node) return;
        for (auto &kv : node->children) clear(kv.second);
        delete node;
    }
};

static inline QString nowStr() {
    return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
}

/*
 * ============================ m_mutex 重入保护 ============================
 * m_mutex 是 std::shared_mutex,同一线程递归加锁(即使全是共享锁)在
 * Windows 的 SRWLock 上是未定义行为,会直接死锁。而这里有一条很深的调用链:
 *
 *   displayPairByIndex (持 currentIndexMutex)
 *     -> displayPair
 *          -> getDisplayCache / putDisplayCache   (原本各自要拿 m_mutex)
 *     -> prefetchDisplayWindow
 *          -> pruneDisplayCache                   (原本要拿 m_mutex)
 *
 * 这些调用方本来就持有 m_mutex,再去拿一次就是自锁。
 * 下面用“线程本地标记”记录当前线程是否已经持有 m_mutex,
 * 让这些内部函数按需跳过重复加锁(和递归锁RecursiveMutex同等效果)。
 *
 * 注意:这个标记只在“持有 m_mutex 的作用域”里设置/清除,
 *      不改变对外语义 —— 没持有 m_mutex 的调用方仍然会正常加锁。
 */
ImageDeduplicateWindow::MMutexScope::MMutexScope(int &depth) : m_depth(depth) { ++m_depth; }
ImageDeduplicateWindow::MMutexScope::~MMutexScope() { --m_depth; }


namespace {
thread_local int g_iwdMMutexDepth = 0;

inline bool iwdHoldsMMutex() { return g_iwdMMutexDepth > 0; }
}

/*
 * 用法:放在函数体最前面。
 *   没持有 m_mutex -> 正常加共享锁;
 *   已经持有(同一线程) -> 跳过加锁,只把深度+1(用深度而不是bool,保证嵌套时也正确)。
 *   每个函数内只用一次,所以变量名固定即可。
 */
#define IWD_WITH_MMUTEX_SHARED()                                                  \
    std::shared_lock<std::shared_mutex> iwdSharedLk(m_mutex, std::defer_lock);    \
    if (!iwdHoldsMMutex()) iwdSharedLk.lock();                                    \
    ImageDeduplicateWindow::MMutexScope iwdMScope(g_iwdMMutexDepth)

/*
 * 格式"质量档位"。和 conflictwindow / datasetwindow 里的同名逻辑保持一致。
 *
 * 只用 DQT 估计值(以及无损格式的 101)。以前这里还会拿"细节区高频零系数比"
 * 压分,但那个量是成对的相对值、单文件不可比,已整体摘除。
 */
static int qualityRank(const FileEntry& file) {
    QString fmtStr = file.format.toLower();
    if (fmtStr == "png") return 101;                 // 无损
    if (fmtStr.startsWith("jp")&&(fmtStr.endsWith("g")||fmtStr.endsWith("e")))
        return file.quality;
    if (fmtStr == "gif") return 40;
    if (fmtStr == "bmp") return 101;
    if (fmtStr == "webp") {
        // 无损webp是101,有损webp是1..100(由 webpquality 解析VP8量化索引得到)
        // 解析失败(损坏/截断/动画的后续帧)才会落到中性档位
        if (file.quality < 0) return kUnknownQualityRank;
        return file.quality;
    }
    return 0;
}
// small helper to wrap a std::function into QRunnable
namespace {
/*
 * 后台任务里未捕获的异常:写进 abort_trace.log(和崩溃现场同一份日志),绝不外抛。
 *
 * 为什么需要它:QThreadPool 遇到 QRunnable::run() 抛出的异常会 catch(...) 后调
 * qTerminate() -> std::terminate -> abort()。而 MSVC 的 std::set_terminate 只在
 * 调用它的那个线程生效(main.cpp 里装的 handler 管不到线程池),所以现场只能看到
 * "abort() has been called",拿不到 what()。这里在任务边界上先把它记下来。
 */
void recordTaskException(const char *where, const char *what)
{
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));

    // 用户要求:日志不写 C 盘,统一 M:/Temp/gallery_info
    const QString dir = ::dedupenv::logDir();
    if (!dir.isEmpty()) {
        const QString path = dir + QStringLiteral("/abort_trace.log");
        if (FILE *f = std::fopen(QDir::toNativeSeparators(path).toLocal8Bit().constData(), "a")) {
            std::fprintf(f, "\n[Task exception] %s | %s | thread=%llu | %s\n",
                         ts.toUtf8().constData(), where,
                         static_cast<unsigned long long>(
                             reinterpret_cast<quintptr>(QThread::currentThreadId())),
                         what ? what : "(null)");
            std::fclose(f);
        }
    }
    qDebug().noquote() << QStringLiteral("[Task exception] %1 | %2 | thread=%3")
                              .arg(QString::fromUtf8(where),
                                   QString::fromUtf8(what ? what : "(null)"))
                              .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()));
}
} // namespace

class FunctionRunnable : public QRunnable {
public:
    FunctionRunnable(std::function<void()> fn) : m_fn(std::move(fn)) { setAutoDelete(true); }
    void run() override {
        /*
         * 任务里的异常绝不能逃出 run():Qt 会 qTerminate() -> abort(),整个进程死。
         * 这些任务都是缓存/预取/像素计算,失败一次只影响自己,下次会自动重试。
         * (实测:auto remove 把文件搬进 duplicate/ 后,预取任务按旧路径 loadMat()
         *  读不到文件,两个线程同时中招,各弹一个 "abort() has been called")
         */
        try {
            m_fn();
        } catch (const std::exception &e) {
            recordTaskException("QRunnable", e.what());
        } catch (...) {
            recordTaskException("QRunnable", "non-std exception");
        }
    }
private:
    std::function<void()> m_fn;
};

/*
 * pixel diff 任务的内存参数(用于限制批量扫描的并发数)。
 *
 * 实测(bench10,32MP / 4626x7016 四通道 PNG):
 *   单个任务的瞬时峰值增量 ≈ 714MB,优化后 ≈ 370MB。
 *   这些是 normalizeToBGR / makeAlignedPair / computePixelDiff 里
 *   一步步新分配的整图(93MB 一张),不是缓存。
 * 每像素按 12 字节估算(3ch 原图 + 3ch 原图 + 3ch 差值 + 1ch 灰度 等)。
 */
static const SIZE_T kPixelDiffBytesPerPixel   = 12;
static const SIZE_T kPixelDiffMemoryBudgetMB  = 1500;   // 批量扫描允许的瞬时内存预算
/*
 * 并发上限。
 *
 * 不能只按核数开:每个任务要解码 2 张原图,而 pix 计算的矩阵缓存
 * (cachedFileIndices_forPixCompute)有固定槽位。worker 数超过槽位/2 就会
 * 互相淘汰、同一张图反复解码 —— 那时加线程只会更慢。
 * 16 个槽位 -> 最多 8 个 worker。
 */
static const size_t kPixDiffMaxWorkers = 8;
/*
 * 一对图连续失败多少次之后才把 pixel diff 落成 1(哨兵)并停止重试。
 * 失败通常是内存不够(分配异常)或文件暂时读不到,先给几次机会,
 * 免得一次偶发失败就把这一对永久标成"差异 100%"。
 */
static const int kPixelDiffMaxFailures = 3;

/*
 * 批量扫描结束后,"没算成"的那些最多再单线程补算多少对。
 * 上限是为了防极端情况(比如整批都因为内存失败)把收尾阶段拖成几十分钟 ——
 * 超过的等用户切到对应图片时再算。
 */
static const int kPixelDiffMaxSerialRetries = 60;

/*
 * 收尾补算的时间预算(毫秒)。
 *
 * 补算是"对失败任务的修复",每对都要重新解码(大图 ~1 秒)。
 * 只按条数(60 对)收口不够:60 张 32MP × 2 次解码 = 好几分钟,
 * 而这段时间进度条早就满格了,用户只会觉得"卡在 99%/100% 不动"。
 * 超时的部分不清空、不丢弃 —— 用户切到那一对时会自动补算。
 */
static const qint64 kPixelDiffRepairBudgetMs = 20000;

ImageDeduplicateWindow::ImageDeduplicateWindow(QWidget *parent)
    : QWidget(parent)
{
    ui.setupUi(this);
    // in ctor after ui.setupUi(this);
    setFocusPolicy(Qt::StrongFocus);
    ::dedupenv::enableDarkTitleBar((void*)winId());//暗色模式

    m_pixelThreadPool = new QThreadPool(this);
    int ideal = QThread::idealThreadCount();
    int poolSize = std::max(1, std::min(4, ideal / 2));
    m_pixelThreadPool->setMaxThreadCount(poolSize);

    /*
     * 显示图预取专用线程池。
     *
     * 以前这里用的是 QtConcurrent::run(全局线程池)。那个池不归窗口管:窗口析构时
     * 既不等它、也没法取消 —— 一张大图的预取还在解码(32MP 接近 1 秒),窗口先没了,
     * 任务再往 m_files / m_mutex 里写就是踩已释放的内存(实测 AV 落在
     * cv::Mat::operator= -> release)。窗口是 main() 里的长寿对象,平时不炸,
     * 但只要程序退出时正好有一对图在预取,就会崩在退出路径上。
     *
     * 换成窗口自己的池以后,析构里 waitForDone() 就能保证"任务全部退出"再拆成员。
     * 刻意【不】和 m_pixelThreadPool 共用:那个池是像素计算的,一批大图能把
     * 它占满,显示图预取跟着排队就会一直不上屏。
     */
    m_prefetchPool = new QThreadPool(this);
    m_prefetchPool->setMaxThreadCount(2);

    /*
     * 文件移动专用单线程池:remove/恢复 的物理 rename 在这里串行执行。
     * 之所以要"单线程":同一文件可能先后入队 [移到duplicate] -> [恢复回去] 这种
     * 相互反向的操作,必须严格按入队顺序执行才能保证最终状态是对的;
     * 多线程并行会打乱顺序。串行同时也把"移动错误"的处理变成单点。
     */
    m_fileOpPool = new QThreadPool(this);
    m_fileOpPool->setMaxThreadCount(1);

    // m_display = findChild<DedupImageDisplayWidget*>("img_display_widget");
    m_display = ui.img_display_widget;
    if (!m_display) {
        qFatal("img_display_widget must be promoted to DedupImageDisplayWidget in Designer!");
    }

    // connect display signals
    connect(m_display, &DedupImageDisplayWidget::imagePainted, this, &ImageDeduplicateWindow::onDisplayImagePainted);
    connect(m_display, &DedupImageDisplayWidget::leftClicked, this, &ImageDeduplicateWindow::onDisplayLeftClicked);
    connect(m_display, &DedupImageDisplayWidget::rightClicked, this, &ImageDeduplicateWindow::onDisplayRightClicked);
    connect(m_display, &DedupImageDisplayWidget::toggleRequested, this, &ImageDeduplicateWindow::onDisplayRightClicked);
    connect(m_display, &DedupImageDisplayWidget::wheelScrolled,this,&ImageDeduplicateWindow::toggleAB);

    // worker
    m_worker = new DedupWorker(&m_dataEpoch,this);
    qRegisterMetaType<QVector<FileEntry>>("QVector<FileEntry>");
    connect(m_worker, &DedupWorker::scanProgress, this, &ImageDeduplicateWindow::onWorkerScanProgress);
    connect(m_worker, &DedupWorker::scanFinished, this, &ImageDeduplicateWindow::onWorkerScanFinished);
    connect(m_worker, &DedupWorker::logMessage, this, &ImageDeduplicateWindow::onWorkerLog);

    // default UI ranges
    ui.sb_pixdiff->setRange(0.0, 1.0);
    ui.sb_pixdiff->setSingleStep(0.0001);
    ui.sb_pixdiff->setValue(m_pixdiffThreshold);
    ui.sb_area->setRange(0.0, 1.0);
    ui.sb_area->setSingleStep(0.0001);
    ui.sb_area->setValue(m_areaThreshold);

    // if (!ui.info_widget->layout()) ui.info_widget->setLayout(new QVBoxLayout(ui.info_widget));

    connect(ui.btn_clear, &QPushButton::clicked, this, &ImageDeduplicateWindow::on_btn_clear_clicked);

    ui.lb_path_a->installEventFilter(this);
    ui.lb_path_b->installEventFilter(this);
    // 只读日志框会吃掉 ←/→(见 eventFilter),这里接上让 previous/next 始终可用
    if (ui.pte_log) ui.pte_log->installEventFilter(this);
    /* 大图控件的过滤器:导航键(Ctrl+A / ← / →)必须和焦点无关,见 handleNavigationShortcut */
    if (m_display) m_display->installEventFilter(this);

    // 操作按钮的右键菜单(每个按钮只给自己的范围变体),左键语义不变
    setupActionButtonContextMenus();
    /*
     * 按钮 tooltip 只在 setupActionButtonContextMenus 里写一份(含左键语义 +
     * 右键入口 + 快捷键)。这里以前又 setToolTip 覆盖了一次,previous 那份只写了
     * "回到上一对",右键入口和 Shift+← 就被覆盖没了 —— 两处写同一句迟早对不上。
     */

    // ===== 图片对列表(DuplicatePairListView) =====
    m_pairList = ui.pair_list_view;
    if (m_pairList) {
        /*
         * 列表也挂导航键过滤器:不挂的话,"焦点在列表里按 →"会被 QListView 的
         * 内建导航吃掉,大图不翻页(见 handleNavigationShortcut)。
         */
        m_pairList->installEventFilter(this);
        connect(m_pairList, &DuplicatePairListView::pairActivated,
                this, &ImageDeduplicateWindow::onPairActivated);
        connect(m_pairList, &DuplicatePairListView::invalidPairActivated,
                this, &ImageDeduplicateWindow::onInvalidPairActivated);
        // 懒加载用QueuedConnection:列表可能在持有m_mutex时登记可见行,
        // 排队执行可以等锁释放后再去读m_pairs,避免自锁。
        static const int kVecIntType = qRegisterMetaType<QVector<int>>("QVector<int>");
        Q_UNUSED(kVecIntType);
        connect(m_pairList, &DuplicatePairListView::lazyLoadRequested,
                this, &ImageDeduplicateWindow::onPairListLazyLoad,
                Qt::QueuedConnection);
        connect(m_pairList, &DuplicatePairListView::contextMenuRequested,
                this, &ImageDeduplicateWindow::onPairListContextMenuRequested);
    } else {
        qWarning() << "imagededuplicatewindow.ui: pair_list_view must be promoted to "
                      "DuplicatePairListView!";
    }

    ui.splitter->setSizes({800, 200,200});
    ui.splitter->setStretchFactor(0, 2);
    ui.splitter->setStretchFactor(1, 0);
    ui.splitter->setStretchFactor(2, 1);

    /*
     * 右侧信息面板的宽度必须只由 splitter 决定,不能被里面的 label 文本带着走。
     *
     * 现象:切图片/切 pair 时中间的 img_display_widget 会忽宽忽窄。
     * 原因:lb_path_a / lb_path_b 显示的是完整文件路径,而且开着 wordWrap;
     * 路径里没有空格,QTextLayout 认为"一个词"就有整条路径那么宽,于是
     * QLabel 的 minimumSizeHint 宽度 = 路径像素宽度。QSplitter 不能把面板压到
     * 最小宽度以下,只能去挤中间那一栏 —— 每换一对图就重新排一次版。
     *
     * 把水平方向的 size policy 设成 Ignored:布局不再把标签的宽度当约束,
     * 标签拿到的是分栏给它的宽度,文本在里面换行(路径按字符断行)。
     */
    for (QLabel *lb : {ui.lb_path_a, ui.lb_path_b}) {
        if (!lb) continue;
        lb->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        lb->setMinimumWidth(0);
    }
    // 侧栏自身留一个下限,免得被拖到只剩几个像素看不出内容
    if (ui.sideWidget)
        ui.sideWidget->setMinimumWidth(160);

    std::unique_lock<std::shared_mutex> lk(m_mutex);
    clearState();

    /* 任务队列按钮:初始为空 -> "没有待处理任务" + 禁用 */
    updateSessionQueueUi();
}

ImageDeduplicateWindow::~ImageDeduplicateWindow()
{
    /*
     * 从这里开始不再接受新的后台派发。
     * 析构期间虽然不会处理事件,但收尾逻辑(restoreFromDuplicate 等)理论上还可能
     * 走到 displayPairByIndex -> prepareDisplayPairAsync;那样任务会在
     * "已经 waitForDone 之后"才进池,然后去碰正在销毁的 m_files。
     */
    m_tearingDown.store(true);

    /*
     * 导入整理会话还没结束就销毁窗口(典型:用户直接退出程序,没点关闭窗口那个 X)。
     * 此时"关窗执行计划"的时机永远不会到来 —— 明确说一句,免得用户以为
     * 整理已经生效。这里【不】自作主张补执行:日志窗口可能已经先被销毁,
     * 而且没有用户确认就动用户的文件不合适。
     */
    if (m_importSessionActive) {
        qWarning() << "ImageDeduplicateWindow: 导入整理会话未结束,窗口已销毁;"
                      "本次整理计划不会执行(文件未被修改)";
        m_importSessionActive = false;
    }

    if (m_worker) m_worker->cancel();

    /*
     * 去重收尾(4.2):直接退出程序、没点窗口上的 X 时,closeEvent 不会来 ——
     * 这里补上同一个口径的收尾,免得"关窗算结束、退出程序却不算"。
     * m_autoDupFinalized 保证和 closeEvent 重复触发时只做一次;
     * 回迁/移走的 rename 在下面 waitForDone() 之前入队,由那次等待保证落盘。
     */
    {
        std::shared_lock<std::shared_mutex> lk(m_mutex);
        MMutexScope mScope(g_iwdMMutexDepth);
        std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
        finalizeExactDuplicateOwnership(/*uiRefresh=*/false);
    }

    if (m_pixelThreadPool) {
        m_pixelThreadPool->waitForDone();
    }
    /*
     * 显示图预取:必须等它跑完才能拆 m_files。
     * 这个池是窗口自己的(不是全局池),waitForDone 才有意义;
     * 顺序放在像素池之后无所谓,但一定要在下面 fileOpPool 之前 ——
     * 预取只读文件,不会和移动队列互相等。
     */
    if (m_prefetchPool) {
        /*
         * 这一行不是噪声:它证明"关窗那一刻确实有预取在飞"。
         * 以前这些任务跑在全局池上,这一句等不到它们,析构完任务还在往 m_files 里写 ——
         * 就是那个 cv::Mat::release 的 AV。真出了这类问题,日志里能看到数量。
         */
        {
            std::lock_guard<std::mutex> lk(m_displayCacheMutex);
            if (!m_displayPrefetchInFlight.isEmpty()) {
                qDebug() << "[~ImageDeduplicateWindow] 析构时仍有"
                         << m_displayPrefetchInFlight.size()
                         << "个显示图预取任务,等它们退出";
            }
        }
        m_prefetchPool->waitForDone();
    }
    /*
     * 等文件移动队列跑完再退出。
     * 不只是"优雅":队列里的 rename 还没执行时,UI 上已经乐观更新过 FileEntry,
     * 直接退出会让"界面上显示已移到duplicate"和磁盘实际状态对不上 ——
     * 这是用户的文件,必须落盘到一致状态。
     * 队列是单线程串行的,waitForDone 会等它把剩余操作做完。
     */
    if (m_fileOpPool) {
        m_fileOpPool->waitForDone();
    }
}
void ImageDeduplicateWindow::setEnablePixelDiff(bool enable) {
    m_enablePixelDiff.store(enable);
    // 可选：当关闭 pixel diff 时，清理待计算队列以释放内存/取消工作
    if (!enable) {
        // 不主动改变 m_pairs[].pixelRatioComputed，避免改变 UI 语义；但清空排队列表
        std::lock_guard<std::mutex> queue_mtx(m_pixelQueuedMutex);
        m_pixelQueued.clear();
    }
}

void ImageDeduplicateWindow::keyPressEvent(QKeyEvent *event)
{
    /*
     * ===== 全局导航键:先于一切业务键处理 =====
     *
     *   Ctrl+A  -> 选中右侧列表的全部行
     *   ← / →   -> 上一对 / 下一对(永远作用于中间的大图)
     *   Shift+← / Shift+→ -> 上/下一个目录对比区域
     *
     * 这三组键的落点和焦点无关:焦点可能在某个操作按钮上、在图片控件上、
     * 也可能在列表里。用户的心智模型是"Ctrl+A 永远是全选右侧列表,
     * 左右键永远是翻大图",不该因为刚才点了哪个控件就失效。
     */
    if (handleNavigationShortcut(event))
        return;

    /*
     * 业务键只认【纯按键】和【纯 Shift】。
     *
     * 这里必须严格判等,不能只判 "有没有 Shift":以前 `Key_A` 分支只看了 shift,
     * 于是 Ctrl+A(焦点在窗口/按钮上时冒泡到这里)会被当成 remove B ——
     * 用户想做全选,结果删掉了一张图。Ctrl+S / Ctrl+D 同理。
     */
    const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;
    const bool shift = (mods == Qt::ShiftModifier);
    const bool plain = (mods == Qt::NoModifier);

    /*
     * Shift + a/s/d = 以当前这一对为锚点进入筛选(见 startFilterFromCurrentPair):
     *   Shift+A -> filter B  (锚点是 B 这张图)
     *   Shift+S -> filter both(A、B 都是锚点)
     *   Shift+D -> filter A  (锚点是 A 这张图)
     * 和 a/s/d(remove B / remove both / remove A)一一对应,只是"改成先筛一遍"。
     */
    if (event->key() == Qt::Key_Space && plain)
    {
        on_btn_jmp_clicked();
    }
    else if (event->key() == Qt::Key_A)
    {
        if (shift)      startFilterFromCurrentPair(FilterEntryMode::FilterFromB);
        else if (plain) on_btn_rm_b_clicked();
    }
    else if (event->key() == Qt::Key_D)
    {
        if (shift)      startFilterFromCurrentPair(FilterEntryMode::FilterFromA);
        else if (plain) on_btn_rm_a_clicked();
    }
    else if (event->key() == Qt::Key_S)
    {
        if (shift)      startFilterFromCurrentPair(FilterEntryMode::FilterFromBoth);
        else if (plain) on_btn_rm_both_clicked();
    }
    else if (event->key() == Qt::Key_Q && plain)
    {
        // Q:保持原来的语义(目录模式跳这两个目录,普通模式跳包含当前图的对)
        on_btn_jump_this_file_clicked();
    }
    /*
     * Shift + Backspace:把选中的行恢复为 Pending。
     *
     * 列表是 ExtendedSelection,所以不用特判多选:选一行恢复一行,选多行批量恢复。
     * 提示音由 applyBatchAction 按"实际处理了几行"来放(单行 pop / 多行 bubble),
     * 全被跳过时不响 —— 那说明想法没落地,不该给"成功"的反馈。
     */
    else if (event->key() == Qt::Key_Backspace && shift)
    {
        QVector<int> rows;
        if (m_pairList)
            rows = m_pairList->selectedPairIndices();
        if (rows.isEmpty() && m_currentIndex >= 0)
            rows.push_back(m_currentIndex);
        if (rows.isEmpty())
            return;
        onPairListBatchAction(rows, static_cast<int>(PairBatchAction::SetPending));
    }
    else
    {
        QWidget::keyPressEvent(event); // 保持默认行为
    }
}

bool ImageDeduplicateWindow::handleNavigationShortcut(QKeyEvent *ke)
{
    if (!ke || ke->type() != QEvent::KeyPress)
        return false;

    const Qt::KeyboardModifiers mods = ke->modifiers() & ~Qt::KeypadModifier;

    if (ke->key() == Qt::Key_A && mods == Qt::ControlModifier) {
        selectAllPairsInList();
        return true;
    }

    /*
     * ←/→ 只认纯按键。Shift+←/→ 是目录区域跳转。
     *
     * 焦点在列表里时 QListView 原本会把这两个键当内部导航吃掉(IconMode 里左右
     * 移动选中项,ListMode 下没意义却仍被消费),焦点在图片控件/按钮上时又依赖
     * 事件冒泡才能到窗口 —— 两条路都不稳。统一在这里判定,列表/图片控件用
     * 事件过滤器把这两个键送进来(见 eventFilter),于是"按右键一定是下一对"。
     */
    if (ke->key() == Qt::Key_Right) {
        if (mods == Qt::NoModifier)    { goToNextPair(); return true; }
        if (mods == Qt::ShiftModifier) { goToNextFolderRegionPair(); return true; }
        return false;
    }
    if (ke->key() == Qt::Key_Left) {
        if (mods == Qt::NoModifier)    { goToPreviousPair(); return true; }
        if (mods == Qt::ShiftModifier) { goToLastFolderRegionPair(); return true; }
        return false;
    }
    return false;
}

void ImageDeduplicateWindow::selectAllPairsInList()
{
    /*
     * Ctrl+A:选中右侧 pair 列表的全部行。
     *
     * 为什么不让 QListView 自己处理:那要求焦点**恰好在列表上**。用户的实际流程是
     * "Shift+S 进筛选 -> Ctrl+A 全选筛出来的对 -> s/a 批量移除",中间焦点常常停在
     * 刚点过的按钮或大图上,Ctrl+A 到不了列表,反而冒泡到窗口被当成 remove B。
     *
     * 筛选状态下列表里只有筛出来的那几对,所以"全选"天然就是"选中全部筛选结果",
     * 不需要额外区分。
     */
    if (!m_pairList || !m_pairList->model())
        return;

    const int rows = m_pairList->model()->rowCount();
    if (rows <= 0) {
        createFloatingLabel(this, QStringLiteral("列表里没有可选的配对"));
        return;
    }

    m_pairList->selectAll();
    /*
     * selectAll() 只改选择集、不动 currentIndex —— 这正是要的:
     * 大图显示的仍然是当前那一对,按 s/a 时 dispatchPairAction 看到的是"多选"
     * 从而走批量路径。
     */
    logLine(QStringLiteral("[%1] Ctrl+A: 选中列表全部 %2 行%3")
                .arg(nowStr()).arg(rows)
                .arg(m_filter.active ? QStringLiteral("(筛选结果)") : QString()));
}

void ImageDeduplicateWindow::closeEvent(QCloseEvent *ev)
{
    /*
     * ===== 关窗 = 本轮会话结束:立刻让批量 pix diff 收手 =====
     *
     * clear file 的节奏是"关窗 -> 马上把判定为重复的图移进回收站 -> 下一个文件夹"。
     * 而 pix diff 的批量阶段是 detached 线程 + 自己的 worker,靠 m_dataEpoch 判
     * "我是不是已经过时";epoch 以前只在**下一次** load() 时才 +1,于是从关窗到
     * 下一轮开扫这段时间里,那批 worker 还会继续读 m_files 里那些马上就要被删掉
     * 的文件 —— 日志刷"像素比较失败: 矩阵不可用",而且每对都要重试几次。
     *
     * 所以关窗就把 epoch 推走:在飞的 worker 回到 core 顶部看到 epoch 不匹配直接
     * 返回,排队中的任务全部作废。窗口要复用的话 load() 会再 +1,不影响下一轮。
     */
    auto stopPixelDiffPhase = [this]() {
        m_dataEpoch.fetch_add(1, std::memory_order_relaxed);
        std::unique_lock<std::mutex> lkQ(m_queue.mtx);
        std::queue<std::shared_ptr<PairInfo>>().swap(m_queue.q);
    };

    /*
     * 本轮任务到此结束:标记空闲 + 下一轮事件循环启动队首。
     *
     * 必须延后到下一轮:此处还在 closeEvent 内部,紧接着
     * QWidget::closeEvent() 会把窗口 hide 掉;如果现在就 show() 下一个任务
     * 的窗口,会被这一次 hide 顺手盖掉(clear file 的 continueAfterWindow
     * 用 QTimer::singleShot(0) 也是同一个原因)。
     */
    auto finishSessionAndPump = [this]() {
        m_sessionActive = false;
        m_currentSessionLabel.clear();
        updateSessionQueueUi();
        QTimer::singleShot(0, this, [this]() { pumpSessionQueue(); });
    };

    /*
     * 目录比较窗口是独立顶层窗口:不关掉它,主窗口关了程序也不会退出
     * (Qt 默认 quitOnLastWindowClosed 还有窗口活着)。
     * 这里直接销毁,不触发 closedByUser —— 关窗途中不该再去建图片对、起线程。
     */
    if (m_folderCompareWidget) {
        FolderCompareWidget *w = m_folderCompareWidget;
        m_folderCompareWidget = nullptr;
        w->blockSignals(true);
        w->close();
        w->deleteLater();
    }

    /*
     * 导入整理模式:关闭 = "本次会话结束,可以执行计划了"。
     * 先让会话问一句(它需要区分"执行整理 / 继续比较 / 放弃本次整理"),
     * 被否决就继续留在窗口里 —— 注意收尾要放在这个确认之后,取消关窗不该把账结掉。
     */
    if (m_importSessionActive && m_importCloseHandler) {
        switch (m_importCloseHandler()) {
        case ImportCloseResult::KeepOpen:
            ev->ignore();
            return;

        case ImportCloseResult::AbortSession:
            /*
             * 放弃:会话的计划一条都不执行,窗口本身照常关掉。
             *
             * 这里【不能】走下面的 importSessionClosed —— 那个信号的含义是
             * "用户做完了,去执行计划"。所以要先把会话相关的状态全部复位,
             * 再单独发 importSessionAborted,让会话把 m_data 丢掉。
             *
             * 也不做 finalizeExactDuplicateOwnership:导入整理是 Deferred 模式,
             * 这个函数在这里本来就是空转(m_exactDupMoves 为空),但"放弃"的语义
             * 要求连"可能动磁盘"的收尾入口都不要碰。
             */
            stopPixelDiffPhase();
            m_importSessionActive = false;
            m_importCloseHandler = nullptr;
            m_fileOpMode = FileOpMode::Immediate;
            emit importSessionAborted();
            emit windowClosed();
            finishSessionAndPump();
            QWidget::closeEvent(ev);
            return;

        case ImportCloseResult::Execute:
            break;
        }
    }

    /*
     * 去重收尾(4.2):窗口关闭 = 本轮去重到此结束。
     *
     * 这是判"去重结束"的标准之一(另一个是进入下一轮,见 clearState()),
     * 刻意不以"所有 pair 都处理完"为准:那只说明列表里暂时没有 Pending,
     * 用户完全可以懒得多处理直接关窗 —— 剩下的 Pending 不参与统计,但收尾必须照做。
     * 和 deduplicate_import 的判断一致。
     *
     * 收尾要读还活着的 m_pairs / m_files,所以必须在任何清理之前;回迁/移走是
     * 排队串行执行的,越早入队越可能在关窗后真正落到磁盘上。
     * uiRefresh=false:列表马上就不存在了,不必重建。
     */
    {
        std::shared_lock<std::shared_mutex> lk(m_mutex);
        MMutexScope mScope(g_iwdMMutexDepth);
        std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
        finalizeExactDuplicateOwnership(/*uiRefresh=*/false);
    }

    /* 收尾已经按这一轮的数据算完了 —— 现在可以让后台的 pix diff 停下来了 */
    stopPixelDiffPhase();

    /*
     * 普通去重模式:关闭就是关闭,不掺和任何文件操作。
     */
    if (!m_importSessionActive) {
        emit windowClosed();
        finishSessionAndPump();
        QWidget::closeEvent(ev);
        return;
    }

    /*
     * 先落标记再发信号:信号槽里会话会立刻读 importPairResults() 并启动执行,
     * 这段时间里如果再收到一次 close 不会重复开一次会话。
     */
    m_importSessionActive = false;
    emit importSessionClosed();
    emit windowClosed();
    finishSessionAndPump();
    QWidget::closeEvent(ev);
}

QVector<ImageDeduplicateWindow::ImportPairResult> ImageDeduplicateWindow::importPairResults()
{
    QVector<ImportPairResult> out;

    std::shared_lock<std::shared_mutex> lk(m_mutex);
    out.reserve(static_cast<int>(m_pairs.size()));
    for (const std::shared_ptr<PairInfo> &p : m_pairs) {
        if (!p) continue;

        ImportPairResult r;
        std::lock_guard<std::mutex> lkState(p->stateMutex);
        if (p->idxA >= 0 && p->idxA < (int)m_files.size())
            r.pathA = m_files[p->idxA].currentPath;
        if (p->idxB >= 0 && p->idxB < (int)m_files.size())
            r.pathB = m_files[p->idxB].currentPath;
        r.state = static_cast<int>(p->state);
        r.decisionSeq = p->decisionSeq;
        r.removedMask = int(p->removedMask);
        out.push_back(r);
    }
    return out;
}

/* ==================================================================
 *              remove cover:显式 A/B 对会话(Deferred)
 * ================================================================== */
/* ==================================================================
 *  任务队列:四类会话共用同一个窗口对象时的调度
 * ================================================================== */

void ImageDeduplicateWindow::enqueueSession(const QString &label,
                                            std::function<void()> start)
{
    if (!start) return;

    m_sessionQueue.enqueue(SessionRequest{label, std::move(start)});
    updateSessionQueueUi();

    /*
     * 空闲:立刻启动。忙:排队,closeEvent 会把 m_sessionActive 置 false
     * 并在下一轮事件循环 pump 队首。
     *
     * 这里不能无条件 pump:当前任务可能正处在 closeEvent 内部(clear file
     * 的消费者会在槽里再次入队下一个文件夹),那时窗口还没真正关完。
     */
    if (!m_sessionActive)
        pumpSessionQueue();
}

void ImageDeduplicateWindow::pumpSessionQueue()
{
    if (m_sessionActive) {
        updateSessionQueueUi();
        return;
    }
    if (m_sessionQueue.isEmpty()) {
        m_currentSessionLabel.clear();
        updateSessionQueueUi();
        return;
    }

    const SessionRequest req = m_sessionQueue.dequeue();
    m_sessionActive = true;
    m_currentSessionLabel = req.label;
    updateSessionQueueUi();

    /*
     * req.start 里会 load*(内部拿 m_mutex 排他锁)并 show 窗口。
     * 本函数不持锁,直接调用安全。
     *
     * 但 start 也可能【什么都没做成】—— 目录不存在、扫描入口抛异常、
     * 又或者窗口在 start 内部就被关掉。以前这些情况会让 m_sessionActive
     * 永远停在 true:队列空了、按钮还写着"结束当前任务",外部会话
     * (导入整理)则一直等一个不会来的关窗信号。所以这里兜一层:
     * 一旦 start 抛出,把本轮的"忙"标记还回去,并跳到下一个任务。
     */
    try {
        req.start();
    } catch (const std::exception &e) {
        qWarning() << "[session queue] start failed:" << e.what();
        logLine(QStringLiteral("任务启动失败(%1),已跳过:%2")
                    .arg(QString::fromUtf8(e.what()), req.label));
        m_sessionActive = false;
        m_currentSessionLabel.clear();
        updateSessionQueueUi();
        QTimer::singleShot(0, this, [this]() { pumpSessionQueue(); });
    } catch (...) {
        qWarning() << "[session queue] start failed with unknown exception";
        logLine(QStringLiteral("任务启动失败(未知异常),已跳过:%1").arg(req.label));
        m_sessionActive = false;
        m_currentSessionLabel.clear();
        updateSessionQueueUi();
        QTimer::singleShot(0, this, [this]() { pumpSessionQueue(); });
    }
}

void ImageDeduplicateWindow::updateSessionQueueUi()
{
    if (!ui.btn_next_task) return;

    const int queued = m_sessionQueue.size();

    if (!m_sessionActive && queued == 0) {
        ui.btn_next_task->setText(QStringLiteral("没有待处理任务"));
        ui.btn_next_task->setToolTip(QString());
        ui.btn_next_task->setEnabled(false);
        return;
    }

    QString text;
    if (m_sessionActive) {
        text = queued > 0
                   ? QStringLiteral("结束当前任务 · 队列 %1").arg(queued)
                   : QStringLiteral("结束当前任务");
    } else {
        text = QStringLiteral("开始下一个任务 (%1)").arg(queued);
    }

    /*
     * 按钮在右下角很窄,长任务名会把它撑爆 —— elide 截断,完整内容进 tooltip。
     */
    const QFontMetrics fm(ui.btn_next_task->font());
    const int avail = qMax(60, ui.btn_next_task->width() - 12);
    ui.btn_next_task->setText(fm.elidedText(text, Qt::ElideRight, avail));

    QString tip;
    if (!m_currentSessionLabel.isEmpty())
        tip = QStringLiteral("当前: %1").arg(m_currentSessionLabel);
    if (queued > 0)
        tip += (tip.isEmpty() ? QString() : QStringLiteral("\n"))
               + QStringLiteral("队列中还有 %1 个任务").arg(queued);
    ui.btn_next_task->setToolTip(tip);
    ui.btn_next_task->setEnabled(true);
}

void ImageDeduplicateWindow::on_btn_next_task_clicked()
{
    if (m_sessionActive) {
        /*
         * 结束当前任务 = 关窗:closeEvent 走提交语义(clear file 的判定就是
         * 在那里交给会话执行的),然后在下一轮事件循环 pump 队首。
         * 如果当前会话的 closeHandler 选择"继续比较"(KeepOpen),close()
         * 会被忽略、队列停住 —— 那是用户自己的选择。
         */
        close();
        return;
    }
    pumpSessionQueue();
}

void ImageDeduplicateWindow::loadPairSession(const QVector<QPair<QString, QString>> &pairs)
{
    QStringList paths;
    paths.reserve(pairs.size() * 2);
    for (const auto &p : pairs) {
        if (!p.first.isEmpty())  paths << p.first;
        if (!p.second.isEmpty()) paths << p.second;
    }
    paths.removeDuplicates();

    /*
     * 和 loadImportSession 同一套语义:Deferred + 会话标记,
     * 关窗时通过 importSessionClosed 通知会话去执行删除计划。
     */
    ++m_sessionSerial;   // 新会话:外部消费者据此识别"窗口已经不是我的了"
    m_sessionActive = true;   // 队列据此判断"窗口忙不忙"
    m_fileOpMode = FileOpMode::Deferred;
    m_importSessionActive = true;
    m_importScanDone.store(false, std::memory_order_relaxed);

    std::unique_lock<std::shared_mutex> lk(m_mutex);
    m_dataEpoch.fetch_add(1, std::memory_order_relaxed);
    clearState();

    // clearState 之后设置:扫描结束时 onWorkerScanFinished 要用它建 pair
    m_explicitPairs = pairs;

    logLine(QString("[%1] Starting remove-cover session: %2 pairs / %3 files "
                    "[all file operations deferred]")
                .arg(nowStr()).arg(pairs.size()).arg(paths.size()));
    setProgressPhaseScanning();

    m_worker->startScanFiles(paths, m_dataEpoch.load(std::memory_order_relaxed));
}

/* ==================================================================
 *              clear file:A=压缩包内容 / B=散图
 * ================================================================== */
void ImageDeduplicateWindow::loadZipCompareSession(const QVector<ZipCompareFolder> &folders)
{
    m_explicitPairs.clear();

    ++m_sessionSerial;   // 新会话:外部消费者据此识别"窗口已经不是我的了"
    m_sessionActive = true;   // 队列据此判断"窗口忙不忙"
    m_fileOpMode = FileOpMode::Deferred;
    m_importSessionActive = true;
    m_importScanDone.store(false, std::memory_order_relaxed);

    std::unique_lock<std::shared_mutex> lk(m_mutex);
    m_dataEpoch.fetch_add(1, std::memory_order_relaxed);
    clearState();
    /*
     * clearState 会把 m_zipCompareMode 复位(它属于"上一轮的数据集"状态),
     * 所以必须在这里再打开一次。
     */
    m_zipCompareMode = true;

    int dirsA = 0, dirsB = 0;
    for (const ZipCompareFolder &f : folders) {
        if (f.compareSide == 0) ++dirsA;
        else                    ++dirsB;
    }

    logLine(QStringLiteral("[%1] clear file 会话: A(压缩包内容) %2 个目录 / B(散图) %3 个目录 "
                           "[all file operations deferred]")
                .arg(nowStr()).arg(dirsA).arg(dirsB));
    setProgressPhaseScanning();

    m_worker->startScan(folders, m_dataEpoch.load(std::memory_order_relaxed));
}

void ImageDeduplicateWindow::buildPairsFromExplicitList(int epoch){
    Q_UNUSED(epoch);

    m_pairs.clear();
    if (m_explicitPairs.isEmpty() || m_files.empty())
        return;

    auto norm = [](const QString &p) {
        // Windows 路径大小写不敏感
        return QDir::cleanPath(p).toLower();
    };

    QHash<QString, int> indexByPath;
    indexByPath.reserve(static_cast<int>(m_files.size()));
    for (int i = 0; i < (int)m_files.size(); ++i)
        indexByPath.insert(norm(m_files[i].currentPath), i);

    for (const auto &pr : std::as_const(m_explicitPairs)) {
        const int a = indexByPath.value(norm(pr.first), -1);
        const int b = indexByPath.value(norm(pr.second), -1);
        if (a < 0 || b < 0 || a == b) continue;

        const FileEntry &fa = m_files[a];
        const FileEntry &fb = m_files[b];

        auto pi = std::make_shared<PairInfo>();
        pi->idxA = a;
        pi->idxB = b;
        pi->pHashDistance = dedup::hammingDistance(fa.pHash, fb.pHash);
        pi->dHashDistance = dedup::hammingDistance(fa.dHash, fb.dHash);
        if (!fa.orbDescriptors.empty() && !fb.orbDescriptors.empty()) {
            const dedup::OrbVerifyResult r = dedup::verifyOrbPair(
                fa.orbDescriptors, fa.orbPoints, fb.orbDescriptors, fb.orbPoints);
            pi->orbMatchRatio = r.inlierRatio;
            pi->orbInliers    = r.inliers;
        }
        m_pairs.push_back(pi);
    }

    // 最像的排前面,用户先看最可能删的那几对
    std::sort(m_pairs.begin(), m_pairs.end(),
              [](const std::shared_ptr<PairInfo> &x, const std::shared_ptr<PairInfo> &y) -> bool
    {
        if (!x) return false;
        if (!y) return true;
        if (x->pHashDistance != y->pHashDistance)
            return x->pHashDistance < y->pHashDistance;
        if (x->dHashDistance != y->dHashDistance)
            return x->dHashDistance < y->dHashDistance;
        return x->idxA < y->idxA;
    });
}

void ImageDeduplicateWindow::clearState()
{
    /*
     * 进入下一轮(同一个窗口被 load() 复用)之前,先把上一轮的账结掉:
     * 4.2 的完全重复归属要按上一轮的人工决策来定,一旦 m_files/m_pairs 被清空就
     * 再也算不出来了。
     *
     * 调用方(load/构造函数)已经持有 m_mutex 排他锁,所以这里只调不能自己加锁的
     * 版本;这些回迁/移走的 rename 会排在下面 waitForDone() 之前入队,
     * 由那一次等待保证真正执行完。
     */
    if (!m_exactDupMoves.isEmpty() && !m_autoDupFinalized) {
        logLine(QStringLiteral("[%1] 新一轮开始: 先按上一轮的人工决策收尾完全重复归属")
                    .arg(nowStr()));
        finalizeExactDuplicateOwnership(/*uiRefresh=*/false);
    }

    // 阻断"等待显示图加载"的切换状态(新数据集/停止后不该再有 pending)
    m_pendingDisplayIndex = -1;
    m_pendingDisplayEpoch = -1;
    /*
     * 上一轮"等落盘再前进"的挂起状态作废:m_files/m_pairs 马上要重建,
     * 旧 op.pair 已经不属于新数据集了。
     */
    m_awaitFileOpPair.reset();
    m_pairFileOpsInFlight.clear();
    m_displayLoadFailedIndices.clear();
    /* 目录组排序的日志签名:换数据集必须重新打一次 */
    m_lastFolderRankLog.clear();
    /* clear file 的 A/B 组模式属于"这一轮的数据集",换数据集必须复位 */
    m_zipCompareMode = false;
    /* 文件下标换了,上一轮算的 ORB 结果全部失效 */
    m_orbVerifyCache.clear();

    /*
     * 换了数据集:筛选会话直接作废。
     * 这里不做"swap 回 A/B"——PairInfo 马上跟着 m_pairs 一起丢掉,换回去没有意义;
     * 但按钮文案和列表的筛选标记必须复位,否则新数据集一进来 remove A 还写着 finished。
     */
    if (m_filter.active) {
        logLine(QStringLiteral("[%1] 数据集重置:放弃进行中的筛选").arg(nowStr()), true);
        m_filter = PairFilterSession();
    }
    updateFilterUi();

    /*
     * 先等在途的文件移动跑完。
     * 物理移动是用户已经做出的决定,不能因为"换了数据集"就丢掉 ——
     * 而且也必须保证下面清空 m_files 时,队列/在途表已经是一致的。
     * 这里的等待是有界的(只有队列里剩下的那几次 rename)。
     */
    if (m_fileOpPool) m_fileOpPool->waitForDone();

    if (m_pixelThreadPool) {
        m_pixelThreadPool->waitForDone();  // 等待所有像素任务完成
    }
    {
        std::lock_guard<std::mutex> lkOp(m_fileOpMutex);
        m_fileOpQueue.clear();
        m_fileOpInFlight.clear();
        m_fileOpReservedTargets.clear();
        m_fileOpWorkerActive = false;
    }
    /*
     * 完全重复归属改判的"延后驱逐"登记(见 finalizeExactDuplicateOwnership):
     * 键值都是上一轮的 fileIdx,换代后全部失效 —— 直接放弃,别在新数据集上
     * 执行驱逐。回迁的物理结果回调即使晚到,epoch 检查也会拦住。
     */
    m_pendingEvictAfterRestore.clear();
    {
        std::unique_lock<std::mutex> lkQ(m_queue.mtx);
        std::queue<std::shared_ptr<PairInfo>>().swap(m_queue.q);
    }
    {
        std::lock_guard<std::mutex> lkQ(m_pixelQueuedMutex);
        m_pixelQueued.clear();
    }
    {
        std::lock_guard<std::mutex> lkC(cacheMutex);//缓存锁,访问cachedFileIndices和cachedFileIndices_forPixCompute时上锁
        cachedFileIndices.clear();
        cachedFileIndices_forPixCompute.clear();
        trackedPixCacheIndices.clear();
    }
    m_files.clear();   // FileEntry析构,cv::Mat引用计数归零,内存真正释放
    qDebug()<<"ImageDeduplicateWindow::clearState";
    // 本函数被调用时,m_mutex 已经被调用方(load/构造函数)以排他锁持有,
    // 所以这里只能调 *Locked 版本,不能再自己去拿 m_mutex。
    clearDisplayPairImagesLocked();
    m_pairs.clear();
    {
        std::lock_guard<std::mutex> lkCache(m_displayCacheMutex);
        m_displayCacheIndex.clear();
    }
    if (m_pairList) m_pairList->clearPairs();   // 重置图片对列表
    m_pixDiffDoneCount.store(0);
    m_pixDiffExpectedCount.store(0);
    m_pixDiffPhaseStarted = false;
    m_pixA = QPixmap();
    m_pixB = QPixmap();
    m_showingA = true;
    m_awaitingPaintSerial = 0;
    m_currentPairImageUnavailable = false;
    m_currentPairImageLoading = false;
    if (m_display) m_display->setStatusBadge(QString());
    /*
     * 上一轮可能在"切图等待上屏"的中间被停止/换数据集,按钮还锁着。
     * 这里必须解开,否则新一轮一开始所有操作按钮都是灰的,而且再也等不到
     * 那次会解锁的 paint。
     */
    m_switchInfoPending = false;
    m_pendingInfoValid = false;
    m_pendingInfo.clear();
    setPairActionButtonsEnabled(true);
    m_currentIndex = -1;
    ui.pte_log->clear();
    auto lay = ui.info_widget->layout();
    if (lay) {
        for (int i = 0; i < lay->count(); ++i) {
            QLayoutItem *it = lay->itemAt(i);
            if (QLabel *label = qobject_cast<QLabel*>(it->widget())) {
                label->clear();
            }
        }
    }
}
void ImageDeduplicateWindow::on_btn_stop_clicked()
{
    m_worker->cancel();

    /*
     * 先把后台任务等停,再独占着清状态 —— 两个顺序都不能反:
     *   - 后台的解码/像素任务都会去申请 m_mutex,持着排他锁等它们就是死锁;
     *   - 而 clearState() 会清空 m_files/m_pairs,不持锁就是"边清边被后台读",
     *     原来这里就是这么调的(clearState 的注释写着"应外部加锁")。
     * m_dataEpoch 一起递增:那个 detached 的像素/排序线程靠它判定自己已经过时,
     * 否则它拿完锁还会继续用已经被清空的下标去访问 m_files/m_pairs。
     */
    if (m_pixelThreadPool) m_pixelThreadPool->waitForDone();
    if (m_fileOpPool)      m_fileOpPool->waitForDone();

    std::unique_lock<std::shared_mutex> lk(m_mutex);
    MMutexScope mScope(g_iwdMMutexDepth);
    m_dataEpoch.fetch_add(1, std::memory_order_relaxed);
    clearState();
}



void ImageDeduplicateWindow::onWorkerScanProgress(int done, int total)
{
    requestProgressRefresh(/*scanning=*/true, done, total);
    if (done == total || done % 10 == 0)
        ui.pte_log->appendPlainText(QString("Scan: %1 / %2").arg(done).arg(total));
}

void ImageDeduplicateWindow::onWorkerLog(const QString &msg)
{
    ui.pte_log->appendPlainText(msg);
}

void ImageDeduplicateWindow::computeAllPixelDiffsAndResort(int epoch)
{
    // 先快照当前已经在 pixel 队列里的任务，避免和 displayPairByIndex() 重复提交
    std::unordered_set<std::shared_ptr<PairInfo>> queuedSnapshot;
    {
        std::lock_guard<std::mutex> lkQ(m_pixelQueuedMutex);
        queuedSnapshot = m_pixelQueued;
    }

    // 先把待计算任务快照出来
    std::vector<std::shared_ptr<PairInfo>> tasks;
    {
        std::shared_lock<std::shared_mutex> lk(m_mutex);
        if(epoch!=m_dataEpoch)
            return;
        tasks.reserve(m_pairs.size());

        /*
         * pix diff 覆盖**全部相似对**,不区分有没有目录方向。
         *
         * 目录方向决定的是"A/B 谁在左、完全重复移哪一侧",它不影响"哪些对算相似"。
         * 没有方向的目录对(相似率不够、被用户 jump)同样要参与:
         *   · 列表排序要用 pix 做组内相似度键;
         *   · "像素完全相同"的对照样要能被 auto remove 认出来(见
         *     on_btn_auto_rm_clicked 的 pixelIdentical 分支:留小的那份)。
         * 只有真正算不动的(内存不够)才留到切图时补算。
         */
        for (auto &pi : m_pairs) {
            if (!pi) continue;

            // 已经算完的直接跳过
            {
                std::lock_guard<std::mutex> lkState(pi->stateMutex);
                if (pi->pixelRatioComputed) continue;
            }

            // 已经被 displayPairByIndex() 或其他逻辑送入队列的，跳过，避免重复提交
            if (queuedSnapshot.find(pi) != queuedSnapshot.end()) {
                continue;
            }

            tasks.push_back(pi);
        }
    }

    if (tasks.empty()) {
        // -1 = "从当前显示的下一对开始";函数内部自己按 m_mutex -> currentIndexMutex 加锁
        sortPairsFromIndex(/*startIndex=*/-1, epoch);
        return;
    }

    /*
     * ===== 进度条分母:开工前一次定死 =====
     *
     * 这一步是"卡在 99%"的根本修复。以前分母是"已经被 worker 领走的任务数"
     * (每个任务在自己开头 +1),而分子是"做完的任务数" —— 两者永远只差
     * "在飞的那几个"(并发数)。于是 35000 对跑了 5 分钟还是 99%:
     *   · 真正的进度 10% 时,显示的是 994/1000 = 99%;
     *   · 真正的进度 90% 时,显示的是 31500/31506 = 99.98%。
     * 换句话说这个进度条从来就没有反映过真实进度,它开工几秒后就到 99% 了。
     *
     * 现在:总数 = 这一轮本来就要算的对数(待建任务的 tasks + 已经在别的线程
     * 里算着的 queuedSnapshot),开工前一次性写进分母;任务自己不再 +1。
     */
    const int totalToDo = (int)(tasks.size() + queuedSnapshot.size());
    {
        m_pixDiffDoneCount.store(0);
        m_pixDiffExpectedCount.store(totalToDo);
        m_pixDiffBulkPhase.store(true);
        // 立刻把进度条拉回 0/N —— 让"这一阶段刚开始"对用户可见
        requestProgressRefresh(/*scanning=*/false, 0, std::max(1, totalToDo));
    }

    for (auto &t : tasks) {
        m_queue.q.push(std::move(t));
    }
    m_queue.finished = true;

    /*
     * 并发数不能按 CPU 核数来 —— 这个任务链是**内存**瓶颈,不是 CPU 瓶颈。
     *
     * 实测(32MP / 4626x7016,bench10):
     *   单个 pixel diff 任务的瞬时峰值增量 ≈ 714MB(优化后约 370MB),
     *   因为 normalizeToBGR / makeAlignedPair / computePixelDiff 每一步都要
     *   新分配一整张 93MB 的图。
     * 按 hardware_concurrency() 开线程(16 核)就是 11GB,分配必然失败;
     * 失败会被 normalizeToBGR 的 catch(...) 静默吞掉,上层就把这一对
     * 永久记成 "pix diff = 1"(实测用户遇到的就是这个)。
     *
     * 所以这里按"内存预算"限制并发:预算默认 1.5GB,每任务按
     * 最大图片像素数 x 12 字节估算(32MP ≈ 370MB),上下限 [1, 4]。
     */
    const unsigned hc = std::thread::hardware_concurrency();
    SIZE_T maxPixels = 0;
    {
        std::shared_lock<std::shared_mutex> lk(m_mutex);
        for (const auto &pi : tasks) {
            if (!pi) continue;
            const int ia = pi->idxA, ib = pi->idxB;
            for (int f : {ia, ib}) {
                if (f < 0 || f >= (int)m_files.size()) continue;
                const QSize sz = m_files[f].size;
                maxPixels = std::max<SIZE_T>(maxPixels,
                                             (SIZE_T)sz.width() * (SIZE_T)sz.height());
            }
        }
    }
    /*
     * 尺寸未知(m_files[].size 为 0)时必须给一个保守估计。
     *
     * 否则 perTaskMB 会被算成 1 MB → memCap = 1500 → 并发只受 CPU 上限约束,
     * 8 个 worker 各要几百 MB 同时开,分配直接失败 —— 日志里
     * "最大图 0.0 MP, 单任务约 1 MB" 后面跟着的"矩阵不可用"就是这么来的。
     * 扫描阶段解码失败的文件 size 就是 0(见 dedupworker.cpp)。
     */
    static const SIZE_T kUnknownSizePixels = (SIZE_T)8 * 1000 * 1000;   // 按 8 MP 兜底
    const bool sizeUnknown = (maxPixels == 0);
    if (sizeUnknown) maxPixels = kUnknownSizePixels;

    const SIZE_T perTaskMB = std::max<SIZE_T>(1, maxPixels * kPixelDiffBytesPerPixel / (1024 * 1024));
    const SIZE_T budgetMB   = kPixelDiffMemoryBudgetMB;
    const size_t memCap     = (size_t)std::max<SIZE_T>(1, budgetMB / perTaskMB);
    /*
     * CPU 上限只给一半核数。
     *
     * 这些 worker 是"纯计算、无 IO 等待"的,按核数开满(12 核开 8 个)之后,
     * UI 线程、显示图解码(m_pixelThreadPool 4 线程)、缩略图加载全都在抢 CPU,
     * 表现就是"进度条在走,但按什么键都没反应 / 图一直不上屏"。
     * 留一半核给交互和上屏,总耗时几乎不变。
     */
    const size_t cpuCap     = (size_t)std::max<unsigned>(1u, (hc ? hc : 4u) / 2u);
    const size_t workerCount = std::max<size_t>(
        1,
        std::min<size_t>({ tasks.size(), cpuCap, memCap, kPixDiffMaxWorkers })
        );

    logLine(QString("[%1] 像素比较: %2 个任务, 最大图 %3 MP%4, 单任务约 %5 MB, "
                    "并发 %6(CPU 上限 %7 / 内存预算 %8 MB)")
                .arg(nowStr()).arg(tasks.size())
                .arg(maxPixels / 1000000.0, 0, 'f', 1)
                .arg(sizeUnknown ? QStringLiteral("(尺寸未知,按 8MP 估算)") : QString())
                .arg(perTaskMB).arg(workerCount).arg(cpuCap).arg(budgetMB));

    auto worker = [this,epoch]() {
        /*
         * 这一层是 std::thread,不是 QThreadPool:异常逃出线程入口同样会 terminate。
         * computePixelForPair 内部已有 try/catch,这里兜住队列操作等边角路径。
         */
        try {
            for (;;) {
                std::shared_ptr<PairInfo> task;

                {
                    std::unique_lock<std::mutex> lk(m_queue.mtx);
                    m_queue.cv.wait(lk, [&] {
                        return !m_queue.q.empty() || m_queue.finished;
                    });

                    if (m_queue.q.empty()) {
                        return;
                    }

                    task = std::move(m_queue.q.front());
                    m_queue.q.pop();
                }

                computePixelForPair(task,epoch);
            }
        } catch (const std::exception &e) {
            recordTaskException("pixelDiffWorker", e.what());
        } catch (...) {
            recordTaskException("pixelDiffWorker", "non-std exception");
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i) {
        workers.emplace_back(worker);
    }

    QElapsedTimer bulkTimer;
    bulkTimer.start();

    m_queue.cv.notify_all();

    for (auto &t : workers) {
        if (t.joinable()) t.join();
    }

    /*
     * 并发阶段结束。
     *
     * 1) 先关掉批量标志:后面的收尾补算走"冻结"路径,不再碰分母。
     * 2) 把 done/expected 拉平 —— 个别任务可能因为 epoch 变化/异常没走到 tick,
     *    不能让进度条永远差那几条(以前就是这个 + 分母错误一起造成的 99%)。
     * 3) 按满格刷一次,然后冻结计账,补算不再影响进度条。
     */
    m_pixDiffBulkPhase.store(false);
    {
        const int done = m_pixDiffDoneCount.load();
        const int notTicked = m_pixDiffExpectedCount.load() - done;
        /*
         * 会话已经结束(关窗把 epoch 推走了)时不要打这行:
         * 那时候 worker 是"被叫停"而不是"算完了",报"0 对/0 秒"只会让
         * clear file 日志里多一条看起来像出错的记录。
         */
        if (epoch == m_dataEpoch.load(std::memory_order_relaxed)) {
            logLine(QString("[%1] 像素比较并发阶段完成: %2 对, 用时 %3 秒, 并发 %4%5")
                        .arg(nowStr()).arg(done).arg(bulkTimer.elapsed() / 1000.0, 0, 'f', 1)
                        .arg(workerCount)
                        .arg(notTicked > 0
                                 ? QStringLiteral("(有 %1 个任务未上报完成,已按完成计入)").arg(notTicked)
                                 : QString()));
            requestProgressRefresh(/*scanning=*/false, done, std::max(1, done));
        }

        m_pixDiffExpectedCount.store(done);
        m_pixDiffProgressFrozen.store(true);
    }

    /*
     * 收尾补算:并发那一轮里如果有没算成的(典型原因就是内存紧张 ——
     * 并发 N 个任务各要几百 MB,分配失败就被当成"算不了"),
     * 这里用单线程再补一轮:单线程的瞬时内存占用最小,最不容易再失败。
     *
     * 这一轮是"结果正确性"的最后一道保险:没有它的话,失败的那几对
     * 只能等用户下次切到它们时才补算(在界面上表现为一直 "computing...")。
     *
     * 但它【不能】无限期拖着:每对都要重新解码(大图 ~1 秒),几千对失败就是几十分钟。
     * 所以按时间预算收口(见 kPixelDiffRepairBudgetMs),剩下的等用户切到再算。
     */
    {
        std::vector<std::shared_ptr<PairInfo>> retry;
        {
            std::shared_lock<std::shared_mutex> lk(m_mutex);
            if (epoch == m_dataEpoch) {
                std::lock_guard<std::mutex> lkQ(m_pixelQueuedMutex);
                for (auto &pi : m_pairs) {
                    if (!pi) continue;
                    std::lock_guard<std::mutex> lkState(pi->stateMutex);
                    if (pi->pixelRatioComputed) continue;
                    if (m_pixelQueued.find(pi) != m_pixelQueued.end()) continue;
                    retry.push_back(pi);
                }
            }
        }
        if (!retry.empty() && epoch == m_dataEpoch) {
            logLine(QString("[%1] 有 %2 对图片的像素比较没算成,单线程补算(最多 %3 对 / %4 秒)")
                        .arg(nowStr()).arg(retry.size()).arg(kPixelDiffMaxSerialRetries)
                        .arg(kPixelDiffRepairBudgetMs / 1000));
            QElapsedTimer repairTimer;
            repairTimer.start();
            int done = 0;
            int skipped = 0;
            for (auto &pi : retry) {
                if (epoch != m_dataEpoch) break;
                if (done >= kPixelDiffMaxSerialRetries
                    || repairTimer.elapsed() > kPixelDiffRepairBudgetMs) {
                    skipped = (int)retry.size() - done;
                    break;
                }
                computePixelForPair(pi, epoch);
                ++done;
            }
            if (skipped > 0) {
                logLine(QString("[%1] 补算提前收口:已补 %2 对,还剩 %3 对未算"
                                "(切到那些图片时会自动补算)")
                            .arg(nowStr()).arg(done).arg(skipped));
            }
        }
    }

    // 冻结解除:之后用户在会话里的单对请求(切图)照常计数,不会倒退(见 tick)
    m_pixDiffProgressFrozen.store(false);

    // 在后台线程里完成重排(函数内部自己按 m_mutex -> currentIndexMutex 加锁)
    QElapsedTimer sortTimer;
    sortTimer.start();
    sortPairsFromIndex(/*startIndex=*/-1, epoch);
    if (epoch == m_dataEpoch) {
        logLine(QString("[%1] 像素比较收尾排序完成: 用时 %2 秒(总耗时 %3 秒)")
                    .arg(nowStr()).arg(sortTimer.elapsed() / 1000.0, 0, 'f', 2)
                    .arg(bulkTimer.elapsed() / 1000.0, 0, 'f', 1));
        logLine("计算完了全部像素并且排序完毕");
    }

    // pixel diff 算完并重排后,m_pairs里每一项的位置都可能变了;
    // 这里在GUI线程重建列表并重新同步选中项(本函数运行在后台线程,不能直接碰UI)。
    QPointer<ImageDeduplicateWindow> self(this);
    QTimer::singleShot(0, this, [self, epoch]() {
        if (!self) return;
        if (epoch != self->m_dataEpoch) return;

        /*
         * 这里**不再**补一轮"像素完全相同"的自动移除。
         *
         * 扫描阶段只能靠 SHA256 找出字节相同的对;字节不同、但解码后逐像素
         * 完全一致的(同一张图重新编码过)以前在这里被批量搬走。按新规则,
         * 这类对留哪一份要看文件大小,和合集方向可能相反,所以一律留在比较
         * 列表里,由用户按 auto remove 处理(见 on_btn_auto_rm_clicked 的
         * pixelIdentical 分支)。它们和其它对一起参与正常排序(半选模式是
         * 目录组 -> pHash -> pix),不会消失,一键 auto remove 就整批处理掉。
         */

        /*
         * ===== 重排后回到"第一个 Pending" =====
         *
         * 重排是整表重排:排序键从 pHash 换成了 pix,列表顺序整个变了,
         * 重排前停在哪一行、"当前这一对"是谁,都已经没有参考意义 ——
         * 继续钉在原来那一对上,用户看到的是"排序后随机停在中间某一对",
         * 想从头处理还得手动滚回顶部。
         *
         * 所以这里明确回到"列表里第一个还没处理的 pair"(等价于用户按 Home 之后
         * 再按 next)。如果确实一对 Pending 都没有,才退回原来的"保持当前对"。
         */
        int target = -1;
        {
            // 排序后pair下标全部变了,显示图缓存里的内容不再对应,直接清掉重建
            self->clearDisplayCache();

            std::unique_lock<std::shared_mutex> lk(self->m_mutex);
            std::lock_guard<std::mutex> lkIndex(self->currentIndexMutex);
            self->rebuildPairList();

            /*
             * 筛选状态下不能跳:列表此刻只显示筛选集合里的那几对,
             * 跳到集合外的 pair 会把当前对和选中行直接撕开(而且 sortPairsFromIndex
             * 在筛选期间本来就跳过整表重排,顺序没变,也没有"回到开头"的必要)。
             */
            if (!self->m_filter.active) {
                for (int i = 0; i < (int)self->m_pairs.size(); ++i) {
                    const auto &p = self->m_pairs[size_t(i)];
                    if (!p) continue;
                    std::lock_guard<std::mutex> lkState(p->stateMutex);
                    if (p->state == PairState::Pending) { target = i; break; }
                }
            }
            if (target < 0)
                self->syncPairListSelectionNoLock();
        }

        if (target >= 0 && target < (int)self->m_pairs.size()) {
            /*
             * 诊断(定位"重排后不回到第一个 Pending"用):
             * displayPairByIndex 在显示图没就绪时是**故意不切**的(见它的注释),
             * 会等后台加载完再回调。所以要区分"没跳"是没调用,还是在等图。
             */
            self->logLine(QString("[%1] 重排后回到第一个 Pending: pair %2").arg(nowStr()).arg(target));
            // displayPairByIndex 自己会拿共享锁、切 currentIndex、同步列表选中并预取窗口
            self->displayPairByIndex(target, epoch);
            return;
        }
        self->logLine(QString("[%1] 重排后没有 Pending 了(保持当前对)").arg(nowStr()));

        // 没有 Pending(全部处理完了):保持原来的行为 —— 按当前下标重建显示窗口
        int center = -1;
        QVector<int> displayWindow;
        {
            std::shared_lock<std::shared_mutex> lk(self->m_mutex);
            std::lock_guard<std::mutex> lkIndex(self->currentIndexMutex);
            if (self->m_currentIndex >= 0 && self->m_currentIndex < (int)self->m_pairs.size()) {
                center = self->m_currentIndex;
                displayWindow = self->collectDisplayWindowPairs(center);
            }
        }
        if (center >= 0)
            self->prefetchDisplayWindow(displayWindow, epoch);
    });
}


void ImageDeduplicateWindow::onWorkerScanFinished(const QVector<FileEntry>& files,int epoch)
{
    /*
     * 扫描完成到"FolderCompare 弹出/第一对图出现"之间全都在 UI 线程上同步跑,
     * 而且整段都握着 m_mutex 的排他锁 —— 用户看到的就是"扫完了,界面卡住不动"。
     * 这里按阶段计时,卡住时能直接看出是哪一段(目录级判定 / 建对 / 摆位 / 重建列表)。
     */
    QElapsedTimer stageTimer;
    stageTimer.start();
    qint64 tAnalyzeMs = 0, tBuildMs = 0, tPostMs = 0, tRebuildMs = 0;

    bool pendingFolderPost = false;   // 需要"摆位 -> 移完全重复 -> 资源去重"这套收尾
    {
        std::unique_lock<std::shared_mutex> lk(m_mutex);
        if(epoch!=m_dataEpoch)
            return;
        // 扫描结果已经落地,导入整理会话可以据此判断"关窗时到底有没有比较结果"
        m_importScanDone.store(true, std::memory_order_relaxed);
        m_files.clear();
        {
            QVector<FileEntry>& signal_arg_files = const_cast<QVector<FileEntry>&>(files);
            for(int i=0;i<files.size();++i)
                m_files.push_back(std::move(signal_arg_files[i]));
            for (auto &fe : m_files) {
                if (fe.originalPath.isEmpty()) fe.originalPath = fe.currentPath;
                if (fe.currentPath.isEmpty()) fe.currentPath = fe.originalPath;
            }
        }
        logLine(QString("[%1] Scan finished: %2 files").arg(nowStr()).arg(m_files.size()));

        /*
         * remove cover 给的是显式 A/B 对:必须原样建 pair,
         * 不能走 buildPairsFromFiles 的 pHash/dHash 阈值(否则"裁剪+遮罩"的封面会被筛掉)。
         */
        /*
         * 半选 internal:先做目录级"合集优先"判定,再建图片对。
         * 判定结果影响 pair 的 A/B 排序与 auto remove 的方向(见 folderKeepDirFor)。
         */
        analyzeInternalFolders();
        tAnalyzeMs = stageTimer.restart();

        if (m_explicitPairs.isEmpty()) {
            /*
             * 半选模式:相似图片对**先建好**,不管目录方向定没定。
             *
             * 用户选目录只是决定"哪一侧当合集",它影响的是 A/B 摆位和完全重复的
             * 归属,而不是"哪些对算相似"。所以现在的顺序是:
             *   建对 -> 算 pix diff -> (需要时)人工定方向 -> 摆位/移重复
             * 以前反过来(方向定完才建对),代价是 FolderCompareWidget 弹出来的时候
             * 后面一对图都还没有,用户选完还要再等一轮扫描 + 像素比较。
             */
            if (!m_folderManualViews.isEmpty()) {
                m_waitingFolderDecision = true;
                m_folderPendingEpoch = epoch;
            }
            buildPairsFromFiles(epoch);
            // 摆位/移重复都要等锁放开后再做(applyFolderOrdering 自己拿排他锁)
            pendingFolderPost = true;
        } else {
            buildPairsFromExplicitList(epoch);
        }
    }
    tBuildMs = stageTimer.restart();

    /*
     * 4.1 的收尾必须按这个顺序:
     *   1) 先把"合集侧"摆到 A 位(applyFolderOrdering)
     *   2) 再移完全重复 —— autoMoveExactDuplicates 的前提就是"A 是合集侧",
     *      顺序反了它会因为 topDirPathOfFile(ia) != keep 把每一对都跳过,
     *      表现就是"完全重复一张都没移",日志里却什么都没说。
     * 3) 资源文件走二进制比较,单独一条通道。
     */
    /*
     * 方向还没定的时候先别摆位、别移完全重复:
     * 摆位要用 m_folderKeep,移重复更要求"A 就是合集侧",这时候都还没有依据。
     * 这两件事统一在 finishFolderDecisionsAndBuildPairs() 里做。
     */
    /*
     * clear file 模式跳过这一段:它只做 A↔B 的视觉比较,完全重复与资源文件
     * 早在会话的二进制阶段就按 SHA-256 处理掉了;再让这两个函数(它们会直接
     * 把文件移进 duplicate/)跑一遍,会在用户还没看到列表之前就改磁盘。
     */
    if (pendingFolderPost && !m_waitingFolderDecision && !m_zipCompareMode) {
        applyFolderOrdering();

        int resMoved = 0;
        {
            std::unique_lock<std::shared_mutex> lk(m_mutex);
            MMutexScope mScope(g_iwdMMutexDepth);
            autoMoveExactDuplicates();
            resMoved = dedupeResourceFiles();
        }
        if (resMoved > 0) {
            logLine(QStringLiteral("[%1] 资源文件: %2 个重复已移入 duplicate")
                        .arg(nowStr()).arg(resMoved));
        }
    }
    tPostMs = stageTimer.restart();

    /*
     * 注意:displayPair / displayPairByIndex / rebuildPairList 内部都会去拿 m_mutex,
     * 所以下面这一段必须在“排他锁已经释放”之后执行,
     * 否则就是同线程"排他锁下再拿共享锁",Windows SRWLock 上会死锁。
     */
    rebuildPairList();

    if (!m_pairs.empty()) {
        std::lock_guard<std::mutex> lk(currentIndexMutex);
        displayPairByIndex(0,epoch);
    } else {
        logLine("No candidate duplicate pairs found.");
    }
    // 选中第一组(在锁外做,避免和列表的懒加载请求互相抢锁)
    syncPairListSelection();
    tRebuildMs = stageTimer.restart();

    /*
     * 扫描后处理的分段耗时。
     *
     * 这一段(old: "扫完之后卡上好一会")全部在 UI 线程同步执行,而且前面三段
     * 还握着 m_mutex 排他锁 —— 期间界面完全不响应。有了这条日志就不用再猜:
     *   · 目录级判定大 -> analyzeInternalFolders 里的 ORB 目录两两匹配
     *   · 建对      大 -> buildPairsFromFiles 的 BK树/ORB 候选 + ORB 精比
     *   · 摆位/移重复大 -> 完全没有(它只排队 rename),除非磁盘极慢
     *   · 重建列表  大 -> 列表模型构建/首次显示
     */
    if (tAnalyzeMs + tBuildMs + tPostMs + tRebuildMs > 500) {
        logLine(QStringLiteral("[%1] [耗时] 扫描后处理 %2ms(目录级判定 %3 / 建对 %4 / 摆位+移重复 %5 / 重建列表 %6)")
                    .arg(nowStr())
                    .arg(tAnalyzeMs + tBuildMs + tPostMs + tRebuildMs)
                    .arg(tAnalyzeMs).arg(tBuildMs).arg(tPostMs).arg(tRebuildMs), true);
    }

    // 有需要人工定方向的目录对 -> 扫描结束后弹 FolderCompareWidget 队列
    if (m_waitingFolderDecision) {
        logLine(QStringLiteral("[%1] 目录方向未确定: 已建好 %2 对相似图片,等待 Folder Compare 选择")
                    .arg(nowStr()).arg(m_pairs.size()));
        /*
         * 用户在 FolderCompare 里选的时候,pix diff 就在后台跑。
         * 这些 pix 不是白算的:目录方向只决定 A/B 摆位和完全重复归属,
         * 而"组内按相似度排序""像素完全相同也对"都要用到它。
         */
        startPixelDiffPhaseForFolderMode(epoch);
        QTimer::singleShot(0, this, [this]() { presentNextFolderCompare(); });
        return;
    } else if (!m_folderKeep.isEmpty()) {
        createFloatingLabel(this, QStringLiteral("目录判定: 自动定方向 %1 对,无人工项")
                                      .arg(m_folderKeep.size()));
    }

    // 扫描阶段结束 -> 切到第二层(黄绿色chunk),开始pixel diff计算
    // 注意:分母不是 m_pairs.size()。有些pair在入队时就已经算好会被跳过,
    // 拿全表大小当分母进度就永远到不了100%。这里从0开始,
    // 每受理一个真正要算的pair就 +1(见 computePixelForPair / requestComputePixelForPair)。
    m_pixDiffDoneCount.store(0);
    m_pixDiffExpectedCount.store(0);
    setProgressPhasePixelDiff();

    // --- 安全地收集需要计算 pixelRatio 的 pair indices（在锁内），但在锁外实际调用 requestComputePixelForPair ---
    QPointer<ImageDeduplicateWindow> self(this);
    std::thread([self, epoch]() {
        if (!self) return;
        // detached std::thread 的异常同样会走到 terminate,这里兜住并记录
        try {
            self->computeAllPixelDiffsAndResort(epoch);
        } catch (const std::exception &e) {
            recordTaskException("computeAllPixelDiffsAndResort", e.what());
        } catch (...) {
            recordTaskException("computeAllPixelDiffsAndResort", "non-std exception");
        }
    }).detach();

}


/*
 * 把pix计算用LRU整体清掉,并且“真的”释放那些不再被预加载缓存引用的矩阵。
 * 直接调用LRUCache::clear()只会清表,矩阵会变成没人管的内存(直到下次clearState)。
 * trackedPixCacheIndices 记录了曾经放进LRU的下标,用它来精确释放。
 */
void ImageDeduplicateWindow::releaseAllPixComputeCache(int excludeA, int excludeB)
{
    std::vector<int> victims(trackedPixCacheIndices.begin(), trackedPixCacheIndices.end());
    trackedPixCacheIndices.clear();
    cachedFileIndices_forPixCompute.clear();

    for (int victim : victims) {
        if (victim == excludeA || victim == excludeB) continue;
        if (cachedFileIndices.find(victim) != cachedFileIndices.end()) continue;
        if (victim < 0 || victim >= (int)m_files.size()) continue;
        std::lock_guard<std::mutex> lkVictim(m_files[victim].matMutex);
        if (m_files[victim].isMatLoaded)
            m_files[victim].releaseMat();
    }
}

static inline uint32_t fnv1a32_hash_descriptor(const cv::Mat &descRow, int maxBytes = -1) {
    const uint8_t *p = descRow.ptr<uint8_t>();
    int len = descRow.cols;
    if (maxBytes > 0 && maxBytes < len) len = maxBytes;
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}
void ImageDeduplicateWindow::sortPairsFromIndex(int startIndex, int epoch)
{
    qDebug() << QStringLiteral("[ImageDeduplicateWindow::sortPairsFromIndex]:收到排序任务,当前epoch:%1,传入epoch:%2")
                    .arg(m_dataEpoch.load())
                    .arg(epoch);

    /*
     * ============================ 锁(很重要) ============================
     *
     * 这个函数会【就地重排 m_pairs】,而且它是在后台线程里跑的
     * (computeAllPixelDiffsAndResort 那个 detached std::thread)。
     * 以前这里只拿了 currentIndexMutex 就直接 std::sort(m_pairs) —— 而 UI 线程
     * (auto remove / remove A/B/both、批量菜单)只拿【共享】m_mutex 就在遍历 m_pairs,
     * 两者并发时,排序中被搬来搬去的 shared_ptr 会被读到:
     * 轻则拿到错的对,重则 shared_ptr 的引用计数被读脏 -> 直接崩。
     * 用户反馈的"pix diff 算完、点 auto remove 程序异常"就是撞在这个窗口上。
     *
     * 所以:
     *   1) 先独占 m_mutex(重排必须排他);
     *   2) 再拿 currentIndexMutex 读 m_currentIndex;
     *   3) 顺序必须是 m_mutex -> currentIndexMutex —— 和 UI 线程的
     *      on_btn_auto_rm_clicked / on_btn_rm_* 完全一致,反过来会锁序反转;
     *   4) 登记 MMutexScope:下面的内部调用(requestComputePixelForPair 等)
     *      会按需申请共享锁,不登记就是"持排他锁再拿共享锁",SRWLock 上直接死锁。
     */
    /*
     * 筛选期间不许重排:m_filter.members 存的是 m_pairs 下标,
     * 一旦整表重排,筛选集合会指向完全不同的对。pix diff 收尾的那次重排
     * 赶在筛选中间到达时直接跳过(退出筛选后也不会补排,排序只影响浏览顺序)。
     */
    if (m_filter.active) {
        logLine(QStringLiteral("[%1] 筛选进行中,跳过本次整表重排").arg(nowStr()));
        return;
    }

    std::unique_lock<std::shared_mutex> lk(m_mutex);
    MMutexScope mScope(g_iwdMMutexDepth);
    std::lock_guard<std::mutex> lkIndex(currentIndexMutex);

    if (epoch != m_dataEpoch.load()) {
        qDebug() << "[ImageDeduplicateWindow::sortPairsFromIndex]: stale epoch, abort";
        return;
    }

    const int n = static_cast<int>(m_pairs.size());
    if (n <= 1) return;

    /*
     * ============ 一律整表排序(startIndex 参数只为兼容保留) ============
     *
     * 以前只排 m_currentIndex 之后那一段,本意是"已经看过的部分别再动",
     * 但实际后果是:排序键(pix)在会话里是不断到齐的,于是"当前对之前"的那一段
     * 永远停在一个过时的顺序里,而之后的一段是新顺序 —— 同一张表里两种口径,
     * 用户回看前面的对时会觉得"排序根本没生效/顺序是乱的"。
     *
     * 现在全部重排,代价是 O(N log N) 一次(35000 对约几百毫秒,且大多在后台线程),
     * 换来的是整表顺序始终一致。当前正在看的那一对靠 shared_ptr 找回新下标(见下面),
     * 所以"重排前看到的是谁,重排后还是谁",不会跳到别处。
     */
    startIndex = 0;

    const int viewIndex = m_currentIndex;

    auto collectNeededFilesFromPairs = [this](const std::vector<int>& pairIndices) -> std::unordered_set<int>
    {
        std::unordered_set<int> result;
        result.reserve(pairIndices.size() * 2);

        for (int pairIdx : pairIndices) {
            if (pairIdx < 0 || pairIdx >= (int)m_pairs.size()) continue;

            const auto &pi = m_pairs[pairIdx];
            if (!pi) continue;

            result.insert(pi->idxA);
            result.insert(pi->idxB);
        }
        return result;
    };

    auto collectPrefetchPairSet = [this](int centerIndex) -> std::unordered_set<int>
    {
        std::unordered_set<int> result;
        const auto indices = collectPrefetchPairIndices(centerIndex);
        result.reserve(indices.size());
        for (int idx : indices) {
            result.insert(idx);
        }
        return result;
    };

    // 排序前：记录当前窗口里的 pair 和 file
    std::unordered_set<int> beforePairs = collectPrefetchPairSet(viewIndex);
    std::unordered_set<int> beforeFiles = collectNeededFilesFromPairs(
        std::vector<int>(beforePairs.begin(), beforePairs.end())
        );

    // 当前正在显示的 pair，建议始终保留它的文件缓存
    if (viewIndex >= 0 && viewIndex < n) {
        const auto &cur = m_pairs[viewIndex];
        if (cur) {
            beforeFiles.insert(cur->idxA);
            beforeFiles.insert(cur->idxB);
        }
    }

    /*
     * ============================ 排序键快照(很重要) ============================
     *
     * 绝对不能拿 m_pairs 里的活对象直接喂 std::sort:
     *
     *   pixelDiffRatio / pixelRatioComputed 是 pixel diff 任务线程写的,而那个线程
     *   写回时**只持 pair 自己的 stateMutex,不持 m_mutex**(见 computePixelForPairCore
     *   末尾的写回块)。本函数虽然持着 m_mutex 排他锁,但拦不住它 ——
     *   于是 std::sort 排到一半,比率被改了,同一个 cmp(a,b) 前后会给出相反答案。
     *
     *   MSVC Debug 的 std::sort 会检查比较器是否自洽(_DEBUG_LT_PRED):
     *   只要 pred(a,b) 和 pred(b,a) 同时为真,就 "invalid comparator" + abort()。
     *   这正是"扫完 pix diff / 点 auto remove 后弹 abort()"的根因:
     *   displayPairByIndex 每次切图都会往 m_pixelThreadPool 投新的 pixel 任务,
     *   与本函数的排序完全可能重叠;数据量越大、任务越多,撞上的概率越高,
     *   小测试集几秒算完就永远复现不了。
     *
     * 修法:先把排序键读进局部快照(读字段时持 stateMutex,保证读到的是完整值),
     * 排序只比较快照,最后按快照顺序写回 m_pairs。快照一旦生成就不会再变,
     * 比较器天然满足严格弱序。
     */
    struct SortEntry {
        std::shared_ptr<PairInfo> p;
        int    pHashDistance = 0;
        int    dHashDistance = 0;
        double orbMatchRatio = 0.0;
        double pix           = 1.0;
        QString pathA, pathB;
        /* 半选 internal: A、B 各自**直属父目录**对的规范化键(A↔B 与 B↔A 相同) */
        QString dirKey;
        /* 这对目录有没有"合集方向"(自动判定或人工选择) */
        bool   hasKeep = false;
        /* 这个目录组的排序键(大 = 更该先处理),见 foldergroup::Stat::rank() */
        qint64 groupRank = 0;
        int    idxA = 0, idxB = 0;
        /* 排序前它在 m_pairs 里的下标 —— 用来算 old -> new 的置换 */
        int    oldIndex = -1;
    };

    /*
     * 半选 internal 模式的排序契约(与普通模式不同,不能混在一起):
     *
     *   1) 第一键 = 目录对(A、B 各自直属父目录对的字典序)。这样列表里
     *      "这一对目录之间的相似图"会连成一片,用户处理完这一段再进入下一对
     *      目录,不会被三个目录的图片来回打断。
     *      2026-10-28 起粒度从"顶层子目录对"细化到"直属父目录对":
     *      D/fa/sub1 ↔ D/fb/sub1 和 D/fa/sub2 ↔ D/fb/sub1 由此分成两个组,
     *      与 Jump These Folders、"下一个目录对比区域"的口径一致
     *      (见 pairDirSortKey 的注释)。
     *   2) 组内才比相似度:**pHash -> 像素差异 -> ORB -> dHash**
     *      (pHash/pix/dHash 越小越像,ORB 越大越像)。
     *      2026-10-28 起两种模式的相似度口径统一成这个顺序 —— 以前半选模式把
     *      pix 放在 pHash 前面,和普通模式不一致;pix 更容易被局部改动拉高,
     *      拿它当第一键会让"几乎一样"的对排到后面去。
     *
     * 普通模式没有"目录对"这个概念(单目录 internal 全部属于同一组),
     * 直接按上面的相似度顺序排。
     */
    const bool folderMode = isFolderCompareMode();

    /*
     * 分组键是第一键,所以半选模式必须整表排序。
     * 只排 currentIndex 之后那段会把同一对目录的对拆到两处,分组就失去意义了。
     * (普通模式现在也是整表排序,见上面的说明。)
     * 代价是当前显示的对可能换位置 —— 下面按 shared_ptr 找回新下标再同步。
     */

    /*
     * 目录组之间的先后:不再是目录名字典序,而是"这一组把子集侧解释得有多充分"。
     * 典型场景见 foldergroup.h —— 大量 A-C(滤镜变体)会把真正该先做的 B-C 压到后面。
     * 打分只用扫描阶段就有的 pHash/dHash/ORB,所以整轮会话里顺序是稳定的,
     * pix diff 算完再排一次也不会让分组跳来跳去。
     */
    QHash<QString, qint64> groupRankOf;
    if (folderMode) {
        const auto stats = folderGroupStats();
        groupRankOf.reserve(stats.size());
        for (auto it = stats.constBegin(); it != stats.constEnd(); ++it)
            groupRankOf.insert(it.key(), it.value().rank());

        // 同样的顺序不重复刷日志(排序在切图/批量后还会被调用)
        QString sig;
        sig.reserve(stats.size() * 48);
        for (auto it = stats.constBegin(); it != stats.constEnd(); ++it) {
            sig += it.key();
            sig += QLatin1Char('#');
            sig += QString::number(it.value().rank());
            sig += QLatin1Char(';');
        }
        if (sig != m_lastFolderRankLog) {
            m_lastFolderRankLog = sig;
            QVector<QPair<qint64, QString>> rank;
            rank.reserve(stats.size());
            for (auto it = stats.constBegin(); it != stats.constEnd(); ++it)
                rank.push_back({it.value().rank(), it.key()});
            std::sort(rank.begin(), rank.end(),
                      [](const QPair<qint64, QString> &a, const QPair<qint64, QString> &b) {
                          if (a.first != b.first) return a.first > b.first;
                          return a.second < b.second;
                      });
            QStringList parts;
            const int show = std::min<int>(6, rank.size());
            for (int i = 0; i < show; ++i) {
                const auto &st = stats.value(rank[i].second);
                const QStringList dirs = rank[i].second.split(QLatin1Char('\n'));
                QString nameA = dirs.value(0);
                QString nameB = dirs.value(1);
                nameA = nameA.mid(nameA.lastIndexOf(QLatin1Char('/')) + 1);
                nameB = nameB.mid(nameB.lastIndexOf(QLatin1Char('/')) + 1);
                parts << QStringLiteral("%1↔%2 覆盖%3张/均值%4(档%5)")
                             .arg(nameA, nameB)
                             .arg(st.covered)
                             .arg(st.avgBest, 0, 'f', 3)
                             .arg(st.rank() - qint64(st.covered) * 1000);
            }
            if (rank.size() > show)
                parts << QStringLiteral("…共 %1 组").arg(rank.size());
            logLine(QStringLiteral("[%1] 目录组排序: %2")
                        .arg(nowStr(), parts.join(QStringLiteral(", "))));
        }
    }

    std::vector<SortEntry> entries;
    entries.reserve(static_cast<size_t>(n - startIndex));

    for (int i = startIndex; i < n; ++i) {
        const std::shared_ptr<PairInfo> &p = m_pairs[i];
        if (!p) continue;

        SortEntry e;
        e.p = p;
        {
            std::lock_guard<std::mutex> lkState(p->stateMutex);
            e.pHashDistance = p->pHashDistance;
            e.dHashDistance = p->dHashDistance;
            e.orbMatchRatio = p->orbMatchRatio;
            e.pix = p->pixelRatioComputed ? p->pixelDiffRatio : 1.0;
            e.idxA = p->idxA;
            e.idxB = p->idxB;
        }
        // 路径只用来打破平局;顺带做范围检查 —— 越界会让 Debug STL 直接断言
        if (e.idxA >= 0 && e.idxA < (int)m_files.size()) e.pathA = m_files[e.idxA].currentPath;
        if (e.idxB >= 0 && e.idxB < (int)m_files.size()) e.pathB = m_files[e.idxB].currentPath;
        if (folderMode) {
            e.dirKey = pairDirSortKey(e.idxA, e.idxB);
            e.hasKeep = !folderKeepDirFor(e.idxA, e.idxB).isEmpty();
            e.groupRank = groupRankOf.value(e.dirKey, 0);
        }
        if (m_zipCompareMode) {
            /*
             * clear file 的分组键 = A 侧(压缩包内容)的扫描根目录。
             * 用 rootInputFolder 而不是"直属父目录":一个压缩包解压后内部可能
             * 还有多层目录,那些层级不该把同一个包的图拆成好几段。
             */
            if (e.idxA >= 0 && e.idxA < (int)m_files.size())
                e.dirKey = m_files[e.idxA].rootInputFolder.toLower();
            else
                e.dirKey.clear();
        }
        e.oldIndex = i;
        entries.push_back(std::move(e));
    }

    if ((int)entries.size() != n - startIndex) {
        // 理论到不了:buildPairsFromFiles 只 push 有效 pair。真到了就保持原顺序,
        // 绝不带着错位写回 m_pairs(那会让 idxA/idxB 与 m_files 对不上,后续全是越界)。
        logLine(QString("[%1] 排序中止: 排序键快照数量异常(%2 != %3)")
                    .arg(nowStr()).arg(entries.size()).arg(n - startIndex), true, 3);
        return;
    }

    /*
     * 排序前记下"当前这一对"的指针:整表重排之后,按下标找回来的是别的对。
     * 用指针找新位置,用户正在看的那一组不会突然变成别的内容。
     *
     * 以前只在半选模式这么做(普通模式只排尾部、当前下标不会变);
     * 现在两种模式都整表排序,所以两种模式都必须按指针跟回去 ——
     * 不跟的话列表会"跳到一个中间的索引位置"(那个下标现在属于别的对)。
     */
    std::shared_ptr<PairInfo> curPair;
    if (viewIndex >= 0 && viewIndex < n)
        curPair = m_pairs[viewIndex];

    const bool zipMode = m_zipCompareMode;
    auto cmp = [folderMode, zipMode](const SortEntry &a, const SortEntry &b) -> bool
    {
        if (zipMode) {
            /*
             * clear file:先按 A 组的解压根目录分组,组内
             * pHash(小=更像) -> 像素差异(小=更像) -> ORB(大=更像) -> dHash。
             * 和半选/普通模式的组内顺序完全一致,用户不用重新适应。
             */
            if (a.dirKey != b.dirKey)
                return a.dirKey < b.dirKey;

            if (a.pHashDistance != b.pHashDistance)
                return a.pHashDistance < b.pHashDistance;

            if (a.pix != b.pix)
                return a.pix < b.pix;

            if (a.orbMatchRatio != b.orbMatchRatio)
                return a.orbMatchRatio > b.orbMatchRatio;

            if (a.dHashDistance != b.dHashDistance)
                return a.dHashDistance < b.dHashDistance;
        } else if (folderMode) {
            /*
             * 0) 组间按"这一组把子集侧解释得有多充分"排(大 = 先做)。
             *
             * 这是第一键,**不再**先按"有没有合集方向"分组。
             *
             * 原来第一键是 hasKeep(有方向的优先),第二键是目录名字典序。
             * 两个都会被"体量"带偏:
             *   · 字典序:"A 给 C 加了滤镜"这种几百张的组总是排在前头;
             *   · hasKeep:滤镜变体集因为"张数更多、覆盖了 C 的全部图片",会被
             *     目录判定当成合集(A 是超集),于是它反而**抢**在真正的 B-C 前面。
             * 现在的口径:覆盖张数 × 相似质量决定先后(见 foldergroup.h)。
             * 真正有合集关系的组通常覆盖率高,自然排在前面,不需要额外的特权。
             *
             * 2) 分数相同时才让"有方向"的组优先(它可以直接自动搬重复文件),
             *    再相同就退回目录对字典序,保证顺序确定。
             */
            if (a.groupRank != b.groupRank)
                return a.groupRank > b.groupRank;

            if (a.hasKeep != b.hasKeep)
                return a.hasKeep;

            if (a.dirKey != b.dirKey)
                return a.dirKey < b.dirKey;

            /*
             * 2) 组内按相似度:**pHash(小=更像) -> 像素差异(小=更像) -> ORB(大=更像)**。
             *
             * 顺序和普通模式保持一致(2026-10-28 调整):
             * pHash 是"整张图的指纹",先按它分层更稳定;pix diff 更容易被
             * 少量像素噪声/局部改动拉高,放在第二键。
             * 以前这里把 pix 放在 pHash 前面,和普通模式的口径不一致。
             */
            if (a.pHashDistance != b.pHashDistance)
                return a.pHashDistance < b.pHashDistance;

            if (a.pix != b.pix)
                return a.pix < b.pix;

            if (a.orbMatchRatio != b.orbMatchRatio)
                return a.orbMatchRatio > b.orbMatchRatio;

            if (a.dHashDistance != b.dHashDistance)
                return a.dHashDistance < b.dHashDistance;
        } else {
            // 普通模式:pHash -> pix -> ORB -> dHash(与半选模式的组内顺序一致)
            if (a.pHashDistance != b.pHashDistance)
                return a.pHashDistance < b.pHashDistance;

            if (a.pix != b.pix)
                return a.pix < b.pix;

            /*
             * ORB 匹配率**越大越相似**,所以要降序。
             * 这里原来是 `<`(升序),等于把"最不相似的排最前" —— 与"按相似度排序"
             * 的语义正好相反,是个实实在在的排序 bug(2026-10-26 修正)。
             */
            if (a.orbMatchRatio != b.orbMatchRatio)
                return a.orbMatchRatio > b.orbMatchRatio;

            if (a.dHashDistance != b.dHashDistance)
                return a.dHashDistance < b.dHashDistance;
        }

        if (a.pathA != b.pathA)
            return a.pathA < b.pathA;

        if (a.pathB != b.pathB)
            return a.pathB < b.pathB;

        if (a.idxA != b.idxA) return a.idxA < b.idxA;
        return a.idxB < b.idxB;
    };

    std::sort(entries.begin(), entries.end(), cmp);

    // 按快照排好的顺序写回。m_pairs 只在这里被搬运一次,不再边比较边读。
    for (int i = 0; i < (int)entries.size(); ++i)
        m_pairs[startIndex + i] = entries[i].p;

    /*
     * 记下 old -> new 的置换(只有被排序的那一段会变)。
     *
     * 重排本身不会算错 pixel diff(任务是按 shared_ptr 派发、结果写回 pair 对象),
     * 但下面这些"按 pair 下标记账"的 UI 副表会全部指向别的对:
     *   · m_displayCacheIndex   —— 显示图缓存的下标集合
     *   · m_displayPrefetchInFlight —— 正在后台生成的显示图
     *   · m_pendingDisplayIndex —— "正在等哪一对上屏"
     *   · m_displayLoadFailedIndices —— 加载失败过的下标
     * 不跟着搬的话,重排后当前这一对会被当成"没缓存"而重新解码一次
     * (大图 1 秒),pruneDisplayCache 还会把窗口里那对的图清掉。
     */
    std::vector<int> newIndexByOld(n, -1);
    for (int i = 0; i < (int)entries.size(); ++i)
        newIndexByOld[entries[i].oldIndex] = startIndex + i;

    auto remapPairIndex = [&](int oldIdx) -> int {
        if (oldIdx < 0 || oldIdx >= n) return oldIdx;
        const int ni = newIndexByOld[oldIdx];
        return ni >= 0 ? ni : oldIdx;    // -1 = 没被重排(在 startIndex 之前),位置不变
    };

    // 整表重排后把"当前这一对"跟回去(仍持 currentIndexMutex,同步改是安全的)
    if (curPair) {
        for (int i = 0; i < n; ++i) {
            if (m_pairs[i] == curPair) {
                m_currentIndex = i;
                break;
            }
        }
    }

    // 按 pair 下标记账的 UI 副表:整体搬过去(不是清空,所以不会重复解码)
    {
        std::lock_guard<std::mutex> lkCache(m_displayCacheMutex);
        if (!m_displayCacheIndex.isEmpty()) {
            QHash<int, bool> remapped;
            remapped.reserve(m_displayCacheIndex.size());
            for (auto it = m_displayCacheIndex.constBegin();
                 it != m_displayCacheIndex.constEnd(); ++it) {
                remapped.insert(remapPairIndex(it.key()), it.value());
            }
            m_displayCacheIndex = remapped;
        }
        /*
         * 正在飞的任务不能按 old->new 重映射。
         *
         * 任务体里捕获的是【派发时的下标】,结束 guard 也是按那个下标 remove。
         * 如果这里把集合里的 6 改成 3,任务结束时 remove(6) 什么也没删,
         * 3 就永远留在集合里 —— 这一对之后的每次预取都被"已经在飞"挡掉,
         * 没有缓存、没有失败日志、8 秒超时后也不会再有人重试。
         * 2026-11-11 现场抓到的 m_displayPrefetchInFlight={3}、displayPairReady=0
         * 就是这个状态。
         *
         * 直接清空:旧任务的下标已经没意义,让它自然退出;
         * 后续 displayPairByIndex 会按新下标重新派发(最多多解码一次)。
         */
        m_displayPrefetchInFlight.clear();
    }
    if (m_pendingDisplayIndex >= 0)
        m_pendingDisplayIndex = remapPairIndex(m_pendingDisplayIndex);
    if (!m_displayLoadFailedIndices.isEmpty()) {
        QSet<int> remapped;
        for (int v : std::as_const(m_displayLoadFailedIndices))
            remapped.insert(remapPairIndex(v));
        m_displayLoadFailedIndices = remapped;
    }

    /*
     * 窗口也要按【新】下标去取。
     * 以前这里沿用排序前的 viewIndex —— 排序后同一个下标已经是别的对了,
     * 于是"排序后窗口"圈到的是别人家的邻居:该预取的没预取,当前对的图反而
     * 可能被 pruneDisplayCache 当成"窗口外"清掉。
     */
    const int afterViewIndex = remapPairIndex(viewIndex);

    qDebug() << "void ImageDeduplicateWindow::sortPairsFromIndex: pair 排序完毕,当前epoch:" << epoch;

    if (epoch != m_dataEpoch.load()) {
        qDebug() << "[ImageDeduplicateWindow::sortPairsFromIndex]: epoch changed after sort, abort cache sync";
        return;
    }

    // 排序后：重新计算当前窗口里的 pair 和 file(用重映射后的下标)
    std::unordered_set<int> afterPairs = collectPrefetchPairSet(afterViewIndex);
    std::unordered_set<int> afterFiles = collectNeededFilesFromPairs(
        std::vector<int>(afterPairs.begin(), afterPairs.end())
        );

    if (afterViewIndex >= 0 && afterViewIndex < n) {
        const auto &cur = m_pairs[afterViewIndex];
        if (cur) {
            afterFiles.insert(cur->idxA);
            afterFiles.insert(cur->idxB);
        }
    }

    // 1) 新进入窗口的 pair：请求 pixel diff 计算
    for (int pairIdx : afterPairs) {
        if (beforePairs.find(pairIdx) != beforePairs.end()) continue;
        if (pairIdx < 0 || pairIdx >= n) continue;

        auto p = m_pairs[pairIdx];
        if (!p) continue;

        {
            std::lock_guard<std::mutex> lkState(p->stateMutex);
            if (p->state != PairState::Pending) continue;
        }

        requestComputePixelForPair(p,epoch);
    }

    // 3) 新增/移出的文件缓存：按差集加载/释放
    {
        std::lock_guard<std::mutex> lk_c(cacheMutex);

        std::vector<int> toLoad;
        std::vector<int> toUnload;

        for (int fileIdx : afterFiles) {
            if (cachedFileIndices.find(fileIdx) == cachedFileIndices.end()) {
                toLoad.push_back(fileIdx);
            }
        }

        for (int fileIdx : cachedFileIndices) {
            if (afterFiles.find(fileIdx) == afterFiles.end()) {
                toUnload.push_back(fileIdx);
            }
        }

        for (int fileIdx : toLoad) {
            fileLoadMat(fileIdx, epoch);
        }

        for (int fileIdx : toUnload) {
            fileUnloadMat(fileIdx, epoch);
        }
    }
}

/* SHA256 是否真的算过(全 0 = 这次没算,不能用来判断"内容相同") */
static bool shaIsNonZero(const FileEntry &fe)
{
    static const char zero[32]{};
    return std::memcmp(fe.SHA256, zero, 32) != 0;
}

void ImageDeduplicateWindow::buildPairsFromFiles(int epoch)
{
    m_pairs.clear();
    m_exactDupCandidates.clear();
    m_exactDupMoves.clear();
    m_autoDupFinalized = false;

    const int nFiles = static_cast<int>(m_files.size());
    if (nFiles <= 1) return;

    // (1) ensure hashes / sizes present (we already computed pHash & dHash & ORB in worker per earlier change)
    for (int i = 0; i < nFiles; ++i) {
        FileEntry &fe = m_files[i];
        if (fe.size.width() == 0 || fe.size.height() == 0) {
            cv::Mat img = dedup::readImageBGR(dedup::readDataFromPath(fe.currentPath));
            if (!img.empty()) fe.size = QSize(img.cols, img.rows);
            qDebug()<<"[ImageDeduplicateWindow::buildPairsFromFiles]:picture do not have size";
        }
    }

    // (2) build BK-tree for pHash and for dHash
    BKTree treeP, treeD;
    for (int i = 0; i < nFiles; ++i) {
        treeP.insert(m_files[i].pHash, i);
        treeD.insert(m_files[i].dHash, i);
    }
    // 参数（可调）
    const int SAMPLE_DESCRIPTORS_PER_IMAGE = 200; // 精确索引每张图采样的描述子数
    const int MAX_BUCKET_BYTES = 8; // 精确索引用描述子前8字节
    const int TOPK_CANDIDATES = 30; // 对每张图取 top K 候选进行精比对

    /*
     * ===== 描述子倒排索引 =====
     *
     * 老实现只有"前 8 字节精确哈希"一张表。它对【同一像素网格、同一尺度】的重复有效
     * —— 实测"纯裁切 60% 面积"的照片和原图有 49 条描述子前缀完全相同(ORB 描述子在
     * 同一尺度下逐位相同),这就是"高度裁切的原图能被认出来"的原因。
     *
     * 但它对"换过分辨率 / 被旋转过"的版本失效:同一张图裁切+缩放+转3° 之后只剩 2 条
     * 命中;而用户那两对(分辨率不同 + 轻微旋转)是 0 条 —— 一条候选都进不来,
     * 后面那套 ORB 精比根本没机会跑。另外这些命中是"全有全无",一旦按
     * 1/4 采样(step = rows/200)就很容易一个都采不到。
     *
     * 所以补 4 张 32 位位带表:描述子按 8 字节一段切成 4 段,每段单独入一张表。
     * 只要某一段的 32 位恰好没被翻转(位翻转是散开的,单段全中概率约 3%~7%),
     * 这一对就能拿到票。票数只用来"决定谁值得精比",判重永远由几何校验定,
     * 所以这里的误报只花时间、不影响正确性。
     *
     * 表 0 = 精确前缀(高精度,同尺度裁切靠它);表 1..4 = 位带(容忍旋转/换分辨率)。
     * 用"排好序的扁平数组"而不是 unordered_map:35000 张图约 3500 万条,
     * map 每个节点几十字节会把内存吃到 GB 级,扁平数组一条只要 8 字节。
     * 布局:高 40 位是 key32,低 24 位是图片下标(所以图片数上限 1677 万)。
     */
    constexpr int kBandTables = 4;
    /* 每段 32 位(4 字节),段间步长 8 字节:取 0/8/16/24 起头,刚好盖住全部 32 字节 */
    constexpr int kBandStride = 8;
    const int kTableCount = 1 + kBandTables;
    /*
     * 索引条目预算。位带表默认【全量索引】而不是跨步采样:
     * 实测草稿/成品那一对全量 800 条能拿 63 票,按 1/4 跨步采样会掉到 0 票
     * (匹配上的描述子在行号上挨得近,跨步采样有大概率一条都采不到)。
     * 35000 张图 ≈ 3400 条/图 × 8 字节 ≈ 900MB,所以给一个预算上限,
     * 超过时按比例减少位带采样数,保证不会把内存吃光。
     */
    const qint64 kIndexEntryBudget = 64000000;   // 约 510MB

    int bandSample = dedup::kOrbFeatureCount;
    if (qint64(nFiles) * (qint64(SAMPLE_DESCRIPTORS_PER_IMAGE) + qint64(bandSample) * kBandTables)
        > kIndexEntryBudget) {
        const qint64 budget = kIndexEntryBudget / std::max<qint64>(1, nFiles)
                              - SAMPLE_DESCRIPTORS_PER_IMAGE;
        bandSample = int(std::clamp<qint64>(budget / kBandTables, 200, bandSample));
    }

    constexpr quint64 kIdxMask = 0xFFFFFFu;
    auto packKey = [](quint32 key, int img) -> quint64 {
        return (quint64(key) << 24) | (quint64(img) & kIdxMask);
    };
    /* 描述子里第 byteOffset 字节起的 4 字节,当成 32 位无符号数 */
    auto bandKey = [](const cv::Mat &row, int byteOffset) -> quint32 {
        const uchar *p = row.ptr<uchar>(0) + byteOffset;
        return (quint32(p[0]) << 24) | (quint32(p[1]) << 16)
             | (quint32(p[2]) << 8)  |  quint32(p[3]);
    };

    /* 注意:不能写成 indexTables(size_t(kTableCount)) —— 那是最经典的 vexing parse,
     * 编译器会把它当成"声明一个名叫 indexTables 的函数",后面所有下标全部报错。 */
    std::vector<std::vector<quint64>> indexTables;
    indexTables.resize(size_t(kTableCount));
    if (nFiles < (1 << 24)) {
        for (int i = 0; i < nFiles; ++i) {
            const cv::Mat &desc = m_files[i].orbDescriptors;
            if (desc.empty() || desc.rows <= 0 || desc.cols < kBandTables * kBandStride) continue;

            const int rows = desc.rows;
            const int stepExact = std::max(1, rows / SAMPLE_DESCRIPTORS_PER_IMAGE);
            for (int r = 0; r < rows; r += stepExact) {
                const cv::Mat row = desc.row(r);
                indexTables[0].push_back(packKey(fnv1a32_hash_descriptor(row, MAX_BUCKET_BYTES), i));
            }
            const int stepBand = std::max(1, rows / bandSample);
            for (int r = 0; r < rows; r += stepBand) {
                const cv::Mat row = desc.row(r);
                for (int t = 0; t < kBandTables; ++t)
                    indexTables[size_t(1 + t)].push_back(packKey(bandKey(row, t * kBandStride), i));
            }
        }
        for (auto &t : indexTables) {
            std::sort(t.begin(), t.end());
            t.erase(std::unique(t.begin(), t.end()), t.end());
        }
    }

    /*
     * ============================ 候选对 ============================
     *
     * 老实现:对每张图的每个描述子做一次 lower_bound(表 0 取 200 个、4 张位带表
     * 各取 800 个),也就是每张图 3400 次二分查找;每次都是 log2(几百万) 次
     * 随机访存。实测 200 张图 = 350ms(约 1.8ms/图),3 万张接近 1 分钟 ——
     * 而且全程在 UI 线程上、握着 m_mutex 排他锁。
     *
     * 但"哪两张图共享描述子"本身就是一个自连接:5 张表都排好序之后,线性扫一遍,
     * 同一个 key 下的图片两两投一票即可。复杂度从
     *      O(图数 × 描述子数 × 表数 × log(条目数))
     * 降到
     *      O(条目数) + O(票数)
     * 而且和查询顺序无关,不需要每张图再维护一个 hitCount 哈希表。
     *
     * 票数口径与老实现差一个固定倍数(老实现 A->B、B->A 各查一次,同一对描述子
     * 会被计两次),top-K 的排序结果不变。
     */
    constexpr quint64 kPairKeyMask = 0xFFFFFFu;   // 低 24 位放图片下标(与 packKey 一致)
    std::unordered_map<quint64, int> pairVotes;
    pairVotes.reserve(size_t(nFiles) * 4);

    for (const auto &table : indexTables) {
        size_t i = 0;
        const size_t m = table.size();
        while (i < m) {
            const quint64 key = table[i] >> 24;
            size_t j = i + 1;
            while (j < m && (table[j] >> 24) == key) ++j;
            if (j - i >= 2) {
                /* 组内条目按 (key, 图片下标) 排序,所以下标天然升序 -> a < b */
                for (size_t x = i; x < j; ++x) {
                    const int a = int(table[x] & kPairKeyMask);
                    for (size_t y = x + 1; y < j; ++y) {
                        const int b = int(table[y] & kPairKeyMask);
                        if (a == b) continue;
                        ++pairVotes[(quint64(a) << 24) | quint64(b)];
                    }
                }
            }
            i = j;
        }
    }

    /* 把票表拆到"每张图一份",顺便按票数取 top-K(BK 树给的那几个另外加) */
    std::vector<std::vector<std::pair<int, int>>> partnersOf;
    partnersOf.resize(size_t(nFiles));
    for (const auto &kv : pairVotes) {
        const int a = int(kv.first >> 24);
        const int b = int(kv.first & kPairKeyMask);
        partnersOf[size_t(a)].emplace_back(kv.second, b);
        partnersOf[size_t(b)].emplace_back(kv.second, a);
    }
    pairVotes.clear();
    pairVotes.rehash(0);

    /*
     * 候选对容器。
     *
     * orb 在下面**并行**算:ORB 几何校验是这一步最贵的操作
     * (800x800 双向 kNN + RANSAC,实测 Debug 构建下单对 5~20ms),
     * 而候选对的量级是"文件数 × 30"—— 几万对串行就是几分钟。
     */
    struct CandPair {
        int a = -1, b = -1;
        int pDist = 0, dDist = 0;
        bool phashOk = false, dhashOk = false;
        int votes = 0;
        dedup::OrbVerifyResult orb;
    };
    std::vector<CandPair> cands;
    cands.reserve(size_t(nFiles) * 8);

    std::unordered_set<uint64_t> seenPairs;   // (min<<32 | max)

    auto considerPair = [&](int ia, int ib, int votes) {
        if (ia == ib) return;
        const int a = std::min(ia, ib), b = std::max(ia, ib);

        /*
         * ===== clear file 的 A/B 组:只跨组比较 =====
         * A = 压缩包解压内容,B = 散图。同一个组内(包括多个压缩包解压出的
         * 多个根目录之间)一律不建对 —— 那些图本来就是同一个作品的不同部分,
         * 互相"去重"没有意义,只会把列表塞满。
         */
        if (m_files[a].compareSide >= 0 && m_files[a].compareSide == m_files[b].compareSide)
            return;

        /*
         * ===== 内部去重规则(现在是每个输入目录各自的 internal 模式)=====
         * 同一个输入目录内的两个文件:
         *   None(0)          -> 不比较(目录之间仍然照常比较);
         *   AcrossTopDirs(1) -> 只有落在【不同顶层子目录】里才比较
         *                       (半勾选:各顶层子目录之间互相去重,单个子目录内部不去重);
         *   All(2)           -> 全都比较。
         * internalMode 在扫描时按目录的 cb_internal_x 填进 FileEntry。
         */
        if (m_files[a].rootInputFolder == m_files[b].rootInputFolder) {
            const int mode = m_files[a].internalMode;
            if (mode == static_cast<int>(InternalDedup::None))
                return;
            if (mode == static_cast<int>(InternalDedup::AcrossTopDirs)) {
                const QString subA = dedup::topLevelSubDirOf(m_files[a].rootInputFolder,
                                                             m_files[a].currentPath);
                const QString subB = dedup::topLevelSubDirOf(m_files[b].rootInputFolder,
                                                             m_files[b].currentPath);
                if (subA.compare(subB, Qt::CaseInsensitive) == 0)
                    return;
            }
        }

        const uint64_t key = ((uint64_t)a << 32) | uint64_t(b);
        if (!seenPairs.insert(key).second) return;   // 已经被别的图带上来了

        CandPair c;
        c.a = a;
        c.b = b;
        c.votes = votes;
        c.pDist = dedup::hammingDistance(m_files[a].pHash, m_files[b].pHash);
        c.dDist = dedup::hammingDistance(m_files[a].dHash, m_files[b].dHash);
        c.phashOk = (c.pDist <= m_pHashThreshold);
        c.dhashOk = (c.dDist <= m_dHashThreshold);
        cands.push_back(std::move(c));
    };

    for (int i = 0; i < nFiles; ++i) {
        /* 1) pHash / dHash 的 BK 树邻居(这些对**已经定案**,ORB 只用来填显示值) */
        std::vector<int> candidatesP, candidatesD;
        treeP.query(m_files[i].pHash, m_pHashThreshold, candidatesP);
        treeD.query(m_files[i].dHash, m_dHashThreshold, candidatesD);
        for (int c : candidatesP) considerPair(i, c, 0);
        for (int c : candidatesD) considerPair(i, c, 0);

        /* 2) 描述子倒排索引的 top-K(按票数) */
        auto &partners = partnersOf[size_t(i)];
        if (partners.empty()) continue;
        if (int(partners.size()) > TOPK_CANDIDATES) {
            std::partial_sort(partners.begin(), partners.begin() + TOPK_CANDIDATES,
                              partners.end(),
                              [](const std::pair<int,int> &x, const std::pair<int,int> &y) {
                                  if (x.first != y.first) return x.first > y.first;  // 票多的优先
                                  return x.second < y.second;                       // 票同按下标,保证可复现
                              });
            partners.resize(size_t(TOPK_CANDIDATES));
        } else {
            std::sort(partners.begin(), partners.end(),
                      [](const std::pair<int,int> &x, const std::pair<int,int> &y) {
                          if (x.first != y.first) return x.first > y.first;
                          return x.second < y.second;
                      });
        }
        for (const auto &pv : partners) considerPair(i, pv.second, pv.first);
    }
    partnersOf.clear();
    partnersOf.shrink_to_fit();

    /* 候选对总数(建对日志里报出来,方便判断"卡"是候选太多还是单对太慢) */
    const int candTotal = int(cands.size());

    /* ===== ORB 几何校验(并行) ===== */
    int orbCompareCount = 0;      // 真正跑了 kNN + RANSAC 的次数
    int orbCacheReuse = 0;        // 复用目录级判定结果的次数
    int orbSkippedByGeometry = 0;
    int orbOnlyHits = 0;
    qint64 orbCompareMs = 0;

    {
        dedup::OrbVerifyOptions vopt;
        vopt.minInliers     = m_orbMinInliers;
        vopt.minInlierRatio = m_orbMinInlierRatio;

        std::atomic<size_t> nextTask{0};
        std::atomic<int> skipped{0};
        std::atomic<int> done{0};
        std::atomic<int> cacheHits{0};

        auto runOne = [&](size_t k) {
            CandPair &c = cands[k];
            dedup::OrbVerifyResult orbRes;
            if (!m_files[c.a].orbDescriptors.empty() && !m_files[c.b].orbDescriptors.empty()) {
                /*
                 * ORB 精比对所有候选都跑。
                 *
                 * 原始尺寸/长宽比、pHash/dHash 只能作为低成本证据,不能反过来
                 * 否决 ORB:大图裁切/重编码后的同图,原始面积比和 pHash 距离
                 * 都可能很差,但特征已经统一缩到长边 1024,几何校验照样成立。
                 * 尺度合理性由 verifyOrbPair 自己的 scale 校验负责。
                 *
                 * 特征点太少(纯色/极简图)时匹配率没有统计意义:
                 * 十几个点也可能"全配上"给出很高的比率,那是噪声不是证据。
                 */
                const bool enoughKeypoints = (m_files[c.a].orbDescriptors.rows >= 20
                                              && m_files[c.b].orbDescriptors.rows >= 20);
                if (enoughKeypoints) {
                    /*
                     * 半选模式:目录级合集判定刚对同一对图跑过一遍 ORB,
                     * 结果在 m_orbVerifyCache 里(只读并发访问,写入只发生在
                     * analyzeInternalFolders 的单线程阶段)。
                     */
                    const quint64 ck = (quint64(std::min(c.a, c.b)) << 32)
                                       | quint64(std::max(c.a, c.b));
                    const auto hit = m_orbVerifyCache.constFind(ck);
                    if (hit != m_orbVerifyCache.constEnd()) {
                        orbRes = hit.value();
                        cacheHits.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        orbRes = dedup::verifyOrbPair(m_files[c.a].orbDescriptors,
                                                      m_files[c.a].orbPoints,
                                                      m_files[c.b].orbDescriptors,
                                                      m_files[c.b].orbPoints, vopt);
                        done.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    skipped.fetch_add(1, std::memory_order_relaxed);
                }
            }
            c.orb = orbRes;
        };

        if (!cands.empty()) {
            /*
             * 线程数:UI 线程此刻在等这一炉算完,所以可以吃到全核;但仍然留一个核
             * 给事件循环/其它后台池,并且上限 8 —— 再多没有收益(内存带宽先饱和)。
             */
            const unsigned hc = std::thread::hardware_concurrency();
            const int want = std::clamp(int(hc ? hc : 4u) - 1, 1, 8);
            const int threads = std::min<int>(want, int(cands.size()));

            QElapsedTimer phaseT;
            phaseT.start();   // 默认构造的 QElapsedTimer 是"无效"的,不 start 就会拿到垃圾值
            if (threads <= 1) {
                for (size_t k = 0; k < cands.size(); ++k) runOne(k);
            } else {
                std::vector<std::thread> pool;
                pool.reserve(size_t(threads));
                for (int t = 0; t < threads; ++t) {
                    pool.emplace_back([&]() {
                        try {
                            for (;;) {
                                const size_t k = nextTask.fetch_add(1, std::memory_order_relaxed);
                                if (k >= cands.size()) break;
                                runOne(k);
                            }
                        } catch (const std::exception &e) {
                            recordTaskException("buildPairsOrb", e.what());
                        } catch (...) {
                            recordTaskException("buildPairsOrb", "non-std exception");
                        }
                    });
                }
                for (auto &th : pool) th.join();
            }
            orbCompareMs = phaseT.elapsed();
            orbCompareCount = done.load();
            orbCacheReuse  = cacheHits.load();
            orbSkippedByGeometry = skipped.load();
        }
    }

    /* ===== 用 ORB 结果定案 ===== */
    for (const CandPair &c : cands) {
        const int a = c.a, b = c.b;
        const dedup::OrbVerifyResult &orbRes = c.orb;

        // decide duplicate if ANY criterion satisfied (pHash OR dHash OR ORB)
        const bool orb_ok = orbRes.matched;
        if (orb_ok && !c.phashOk && !c.dhashOk)
            ++orbOnlyHits;    // 只有 ORB 能认出来的对(裁剪/加边框),用它判断预筛是否伤召回

        if (!(c.phashOk || c.dhashOk || orb_ok))
            continue;

        /*
         * 完全重复(字节相同)不建图片对:这种对没有"选哪张"的问题,
         * 扫描完由 autoMoveExactDuplicates() 按合集方向直接移入 duplicate。
         *
         * SHA256 全 0 表示"这次没算"(没勾 cache),不能当成内容相同。
         *
         * 只在"立即执行"的普通/半选去重里这么做:
         * 导入整理(Deferred)的完全重复由它自己的二进制阶段处理,
         * 那里还需要这些对留在列表里参与计划生成。
         */
        const bool immediateMode =
            (m_fileOpMode == FileOpMode::Immediate) && !m_importSessionActive;
        if (immediateMode && shaIsNonZero(m_files[a])
            && m_files[a].sameContentAs(m_files[b])) {
            /*
             * 只有"目录方向已经确定"的完全重复才跳过建对、直接搬走。
             * 没有合集关系时,留哪一份是用户的事 —— 正常建对进列表。
             */
            if (!folderKeepDirFor(a, b).isEmpty()) {
                m_exactDupCandidates.append(qMakePair(a, b));
                continue;
            }
        }

        std::shared_ptr<PairInfo> pi = std::make_shared<PairInfo>();
        pi->idxA = a;
        pi->idxB = b;
        /*
         * clear file:A(压缩包里的那张)永远放左边。
         * 用户在这里的固定预期是"左边是压缩包内容、右边是散图",
         * 删的几乎总是右边;A/B 随机摆会让 remove 按错边。
         */
        if (m_zipCompareMode && m_files[pi->idxA].compareSide != 0)
            std::swap(pi->idxA, pi->idxB);
        pi->pHashDistance = c.pDist;
        pi->dHashDistance = c.dDist;
        pi->orbMatchRatio = orbRes.inlierRatio;
        pi->orbInliers    = orbRes.inliers;
        m_pairs.push_back(pi);
    }

    /*
     * ===== 单张相似图片对的 A/B 摆位(提示词 §6)=====
     * 只决定"这一对里谁放 A 位",不是给图片对列表排序,也不产生任何 remove。
     *   优先合集关系(合集侧放 A);否则按
     *   压缩质量估计 > 文件大小 > 像素数 > 路径深度 > 所属顶层目录递归图片数。
     */
    {
        bool folderMode = false;
        for (const FileEntry &fe : std::as_const(m_files)) {
            if (fe.internalMode == static_cast<int>(InternalDedup::AcrossTopDirs)) {
                folderMode = true;
                break;
            }
        }

        if (folderMode) {
            /*
             * ===== 目录级质量优先级(半选 cb_internal_x 的动态优先级)=====
             * 按"重复对"给每个顶层目录累计胜负:
             *   每对里质量高的 +1;质量相同比文件大小;再相同比像素数。
             * 结果写进 m_dirScore,由 preferFileAsA() 统一使用 ——
             * 摆位和"完全重复移哪一侧"必须是同一套判断,否则列表里看到 A 在左、
             * 实际却把 A 移走了。
             */
            m_dirScore.clear();
            for (const auto &pi : std::as_const(m_pairs)) {
                if (!pi) continue;
                const int ia = pi->idxA, ib = pi->idxB;
                if (ia < 0 || ia >= (int)m_files.size() || ib < 0 || ib >= (int)m_files.size())
                    continue;
                const QString scoreDirA = topDirPathOfFile(ia);
                const QString scoreDirB = topDirPathOfFile(ib);
                if (scoreDirA.isEmpty() || scoreDirB.isEmpty() || scoreDirA == scoreDirB)
                    continue;

                const FileEntry &fa = m_files[ia];
                const FileEntry &fb = m_files[ib];
                int cmp = 0;
                if (fa.quality != fb.quality) {
                    cmp = (fa.quality > fb.quality) ? 1 : -1;
                } else if (fa.filesize != fb.filesize) {
                    cmp = (fa.filesize > fb.filesize) ? 1 : -1;
                } else {
                    const qint64 pa = qint64(fa.size.width()) * fa.size.height();
                    const qint64 pb = qint64(fb.size.width()) * fb.size.height();
                    if (pa != pb) cmp = (pa > pb) ? 1 : -1;
                }
                if (cmp > 0) ++m_dirScore[scoreDirA];
                else if (cmp < 0) ++m_dirScore[scoreDirB];
            }
            if (!m_dirScore.isEmpty()) {
                QStringList parts;
                for (auto it = m_dirScore.constBegin(); it != m_dirScore.constEnd(); ++it)
                    parts << QStringLiteral("%1=%2").arg(QFileInfo(it.key()).fileName()).arg(it.value());
                logLine(QStringLiteral("[%1] 目录质量评分(按重复对): %2")
                            .arg(nowStr(), parts.join(QStringLiteral(", "))));
            }

            m_dirImageCount.clear();
            for (int i = 0; i < (int)m_files.size(); ++i) {
                const QString d = topDirPathOfFile(i);
                if (!d.isEmpty()) ++m_dirImageCount[d];
            }

            int swapped = 0;
            for (const auto &pi : std::as_const(m_pairs)) {
                if (!pi) continue;
                const int ia = pi->idxA, ib = pi->idxB;
                if (ia < 0 || ia >= (int)m_files.size() || ib < 0 || ib >= (int)m_files.size())
                    continue;

                const QString keep = folderKeepDirFor(ia, ib);
                const bool aShouldKeep = keep.isEmpty()
                                             ? preferFileAsA(ia, ib)
                                             : (topDirPathOfFile(ia) == keep);
                if (!aShouldKeep) {
                    std::swap(pi->idxA, pi->idxB);
                    ++swapped;
                }
            }
            if (swapped)
                logLine(QStringLiteral("[%1] 图片对摆位: %2 对按质量/合集方向调整 A/B")
                            .arg(nowStr()).arg(swapped));
        }
    }

    /*
     * 扫描阶段的初始排序(第二次排序在 pix diff 算完之后,见 sortPairsFromIndex)。
     *
     * 半选 internal:第一键是"目录对",让同一对目录之间的相似图连成一片;
     *               此时 pix diff 还没算,组内只能按 pHash/dHash/ORB 这类已有指标排。
     * 普通模式    :按 A 侧文件下标(= 扫描顺序 ≈ 路径序),目的是让 pix diff
     *               的缓存命中率更高(相邻 pair 复用同一批已解码文件)。
     */
    const bool folderModeForSort = isFolderCompareMode();
    /* 半选模式:组间顺序按"子集侧被解释的充分程度"打分(见 foldergroup.h) */
    QHash<QString, qint64> groupRankForSort;
    if (folderModeForSort) {
        const auto stats = folderGroupStats();
        groupRankForSort.reserve(stats.size());
        for (auto it = stats.constBegin(); it != stats.constEnd(); ++it)
            groupRankForSort.insert(it.key(), it.value().rank());
    }
    auto cmpFilePath = [this, folderModeForSort, &groupRankForSort]
                       (const std::shared_ptr<PairInfo>& pa,
                        const std::shared_ptr<PairInfo>& pb) -> bool
    {
        /*
         * clear file:A 侧的解压根目录是第一键(同一次解压出来的图连成一片),
         * 组内按 pHash / ORB / dHash 排(pix 还没算)。
         * 此时 folderModeForSort 一定是 false —— 两种模式互斥。
         */
        if (m_zipCompareMode) {
            auto rootOf = [this](const std::shared_ptr<PairInfo> &p) -> QString {
                if (!p || p->idxA < 0 || p->idxA >= (int)m_files.size())
                    return QString();
                return m_files[p->idxA].rootInputFolder.toLower();
            };
            const QString ka = rootOf(pa);
            const QString kb = rootOf(pb);
            if (ka != kb)
                return ka < kb;

            if (pa->pHashDistance != pb->pHashDistance)
                return pa->pHashDistance < pb->pHashDistance;
            if (pa->orbMatchRatio != pb->orbMatchRatio)
                return pa->orbMatchRatio > pb->orbMatchRatio;
            if (pa->dHashDistance != pb->dHashDistance)
                return pa->dHashDistance < pb->dHashDistance;
        }

        if (folderModeForSort) {
            const QString ka = pairDirSortKey(pa->idxA, pa->idxB);
            const QString kb = pairDirSortKey(pb->idxA, pb->idxB);
            if (ka != kb) {
                const qint64 sa = groupRankForSort.value(ka, 0);
                const qint64 sb = groupRankForSort.value(kb, 0);
                if (sa != sb)
                    return sa > sb;
                return ka < kb;
            }

            // 组内按已有指标排序:pHah(小=更像) -> ORB(大=更像) -> dHash(小=更像)
            // 注意:pix diff 此刻还没算,不要在这里读它(全都是默认值 1.0,没有意义)。
            if (pa->pHashDistance != pb->pHashDistance)
                return pa->pHashDistance < pb->pHashDistance;
            if (pa->orbMatchRatio != pb->orbMatchRatio)
                return pa->orbMatchRatio > pb->orbMatchRatio;
            if (pa->dHashDistance != pb->dHashDistance)
                return pa->dHashDistance < pb->dHashDistance;
        }

        int idxaA = pa->idxA;
        int idxbA = pb->idxA;
        if (idxaA != idxbA)
            return idxaA < idxbA;

        int idxaB = pa->idxB;
        int idxbB = pb->idxB;
        if (idxaB != idxbB)
            return idxaB < idxbB;
        return false;   // 完全等价:必须给返回值,否则 std::sort 的比较器是 UB
    };
    std::sort(m_pairs.begin(), m_pairs.end(), cmpFilePath);
    if (folderModeForSort)
        logLine(QStringLiteral("[%1] 图片对排序: 按目录对分组(%2 组)")
                    .arg(nowStr())
                    .arg([this]() {
                        QSet<QString> keys;
                        for (const auto &pi : std::as_const(m_pairs)) {
                            if (!pi) continue;
                            keys.insert(pairDirSortKey(pi->idxA, pi->idxB));
                        }
                        return keys.size();
                    }()));
    /*
     * 建对阶段的工作量:卡顿时不用再猜。
     * orb 精比次数 × 平均耗时就是"扫完之后卡住"的大头;
     * "几何预筛跳过"和"ORB 独占命中"用来判断这道预筛有没有伤召回
     * (独占命中 = pHash/dHash 都不接近、只有 ORB 判出来的对)。
     */
    logLine(QStringLiteral("[%1] 建对: %2 个文件 -> %3 对相似图片(完全重复候选 %4 对);"
                           "候选对 %5 个,ORB 精比 %6 次 + 复用 %7 次(阶段耗时 %8ms,并行),"
                           "几何预筛跳过 %9 次,ORB 独占命中 %10 对")
                .arg(nowStr()).arg(nFiles).arg(m_pairs.size()).arg(m_exactDupCandidates.size())
                .arg(candTotal).arg(orbCompareCount).arg(orbCacheReuse).arg(orbCompareMs)
                .arg(orbSkippedByGeometry).arg(orbOnlyHits));

    qDebug() << "void ImageDeduplicateWindow::buildPairsFromFiles: pairs按照文件索引顺序排序完毕,接下来计算pixdiff时缓存命中率提高,当前epoch:" << epoch;
    qDebug()<<"Print All Pairs State:";
    qDebug()<<"m_currentIndex:"<<m_currentIndex;

    for(int i =0;i<m_pairs.size();++i){
        shared_ptr<PairInfo> pair = m_pairs[i];
        qDebug()<<QStringLiteral("Pairs %1{idxA:%2,idxB:%3} :").arg(i).arg(pair->idxA).arg(pair->idxB);
    }
    // sortPairsFromIndex(0,epoch);
    // qDebug()<<"已完成像素计算前排序";
}

bool ImageDeduplicateWindow::ensureMatCached(int fileIdx, int mode, int excludeA, int excludeB,
                                             cv::Mat *outMat)
{
    if (fileIdx < 0 || fileIdx >= (int)m_files.size()) return false;

    FileEntry &fe = m_files[fileIdx];

    // ---- 1) 查表(极短临界区):两个缓存都不认、或认得且矩阵确实在,都可以直接定论 ----
    {
        std::lock_guard<std::mutex> lk_c(cacheMutex);
        if (findInCache(fileIdx)) {
            std::lock_guard<std::mutex> lkMat(fe.matMutex);
            if (fe.isMatLoaded) {
                // 顺手把句柄取出来(和淘汰互斥,见声明处的说明)
                if (outMat) *outMat = fe.m_mat;
                return true;
            }
        }
    }

    /*
     * 2) 走到这里说明:两个缓存表都没有它,或者表里记着但矩阵已经被释放过(状态不一致)。
     * 不一致时必须把这个下标从表里摘干净,否则下面登记时 put 会以为"已经在缓存里"而跳过,
     * 新的矩阵就永远登记不上(下次还要重新解码)。
     *
     * 【只摘这一个下标】以前这里调的是 releaseAllPixComputeCache() —— 一次不一致
     * 就把整个 pix LRU(16 槽位)全部清空并释放矩阵,后面每一对都要重新解码
     * (大图一张接近 1 秒)。批量阶段里这种不一致很常见(预取/搬迁/释放交替),
     * 结果是"越跑越慢,进度条卡在 99%"。
     */
    {
        std::lock_guard<std::mutex> lk_c(cacheMutex);
        if (findInCache(fileIdx)) {
            cachedFileIndices.erase(fileIdx);
            cachedFileIndices_forPixCompute.erase(fileIdx);
            trackedPixCacheIndices.erase(fileIdx);
            qDebug() << QStringLiteral("[ImageDeduplicateWindow]:缓存状态不一致,只重载 %1").arg(fileIdx);
        }
    }

    /*
     * 3) 解码。
     *
     * 【性能】这里【只】持有这个文件自己的 matMutex,不再持有 cacheMutex。
     * 原来整个解码都在 cacheMutex 里做 —— 一张 32MP(4626x7016) 实测要 ~1 秒,
     * 于是预取、像素计算、预加载三类任务全部排在同一个锁上,4 个线程等价于 1 个线程,
     * 预取永远追不上用户按键的速度。拆开之后不同文件的解码可以真正并行。
     *
     * 同一个文件被两个线程同时要求加载也没关系:matMutex 保证只有第一个真的解码,
     * 第二个进来看到 isMatLoaded 直接跳过。
     */
    {
        std::lock_guard<std::mutex> lkMat(fe.matMutex);
        if (!fe.isMatLoaded) {
            if (!fe.loadMat()) return false;
        }
    }

    /*
     * 4) 登记 + 取句柄,同一个临界区。
     *
     * 取句柄必须和"登记"在同一个锁里:如果分两步(先 ensure 返回,调用方再去读 m_mat),
     * 中间就存在一个窗口 —— 别的线程此刻往 LRU 里塞新文件,可能刚好把这个下标
     * 淘汰掉并 releaseMat,调用方读到的就是空矩阵。
     * 那个空矩阵会让 pixel diff 落到"计算失败"分支、被永久记成 ratio=1,
     * 显示图预取也会直接放弃这一对(表现为图一直不上屏)。
     */
    {
        std::lock_guard<std::mutex> lk_c(cacheMutex);
        if (mode == 0) {
            cachedFileIndices.insert(fileIdx);
        } else {
            putPixComputeCache(fileIdx, excludeA, excludeB);
        }

        std::lock_guard<std::mutex> lkMat(fe.matMutex);
        if (fe.m_mat.empty()) return false;   // 理论上到不了,防御:没矩阵就别登记成"已缓存"
        if (outMat) *outMat = fe.m_mat;
    }
    return true;
}

void ImageDeduplicateWindow::putPixComputeCache(int fileIdx, int excludeA, int excludeB)
{
    std::optional<int> back = cachedFileIndices_forPixCompute.put(fileIdx, fileIdx);
    trackedPixCacheIndices.insert(fileIdx);

    // 处理被LRU淘汰出来的下标:只有它确实不在预加载缓存里、也不是正在使用的A/B时才能释放
    if (back.has_value()) {
        const int victim = back.value();
        trackedPixCacheIndices.erase(victim);
        /*
         * 越界保护:LRU 是按"文件下标"存的,数据换代(重扫)后旧下标可能已经超出
         * m_files 范围。这里的 m_files[victim] 在 Debug STL 下会直接断言 abort,
         * 所以先拦一道 —— 淘汰只是缓存优化,放弃这一次完全无害。
         */
        if (victim < 0 || victim >= (int)m_files.size())
            return;
        if (victim != excludeA && victim != excludeB &&
            cachedFileIndices.find(victim) == cachedFileIndices.end()) {
            std::lock_guard<std::mutex> lkVictim(m_files[victim].matMutex);
            if (m_files[victim].isMatLoaded) {
                m_files[victim].releaseMat();
                qDebug() << QStringLiteral("[ImageDeduplicateWindow]:LRU淘汰并释放 %1").arg(victim);
            }
        } else {
            qDebug() << QStringLiteral("[ImageDeduplicateWindow]:LRU淘汰 %1,但它在预加载缓存/正在使用,跳过释放").arg(victim);
        }
    }
}

void ImageDeduplicateWindow::fileLoadMat(int fileIdx,int epoch){
    m_pixelThreadPool->start(new FunctionRunnable([fileIdx, this,epoch](){
        std::shared_lock<std::shared_mutex>lk(m_mutex);
        if (epoch != m_dataEpoch.load(std::memory_order_relaxed))
            return;
        // 注意:不要在这里持 cacheMutex —— ensureMatCached 自己管理锁,
        // 解码那段只持该文件的 matMutex,多个文件的预加载才能真正并行。
        if (ensureMatCached(fileIdx, 0))
            qDebug()<<"预加载"<<fileIdx<<"的图片矩阵";
    }));
}
void ImageDeduplicateWindow::fileUnloadMat(int fileIdx,int epoch){
    m_pixelThreadPool->start(new FunctionRunnable([fileIdx,this,epoch](){
        std::shared_lock<std::shared_mutex>lk(m_mutex);
        if (int currentEpoch = m_dataEpoch.load(std::memory_order_relaxed);epoch != m_dataEpoch.load(std::memory_order_relaxed)){
            qDebug()<<QStringLiteral("epoch不匹配,线程%1,当前%2").arg(currentEpoch);
            return;
        }
        // 越界保护:重扫后旧下标可能已经失效,别让 m_files[fileIdx] 在 Debug STL 下断言
        if (fileIdx < 0 || fileIdx >= (int)m_files.size())
            return;
        lock_guard<std::mutex>lk_c(cacheMutex);
        if (cachedFileIndices.find(fileIdx) == cachedFileIndices.end())
            return;   // 不在预加载缓存里,不归这里释放

        lock_guard<std::mutex>lk_m(m_files[fileIdx].matMutex);
        // 如果它还在pix计算的LRU里,保留(那边还要用)
        if (cachedFileIndices_forPixCompute.hasKey(fileIdx)) {
            cachedFileIndices.erase(fileIdx);
            return;
        }
        if(m_files[fileIdx].releaseMat()){
            cachedFileIndices.erase(fileIdx);
            qDebug()<<"释放"<<fileIdx<<"的预加载图片矩阵";
        }
    }));
}

void ImageDeduplicateWindow::syncFileCacheForWindowLocked(const std::unordered_set<int> &neededFileIndices, int epoch)
{
    // 调用方必须已持有 cacheMutex(见声明处注释)
    std::vector<int> toLoad;
    std::vector<int> toUnload;

    for (int fileIdx : neededFileIndices) {
        if (cachedFileIndices.find(fileIdx) == cachedFileIndices.end()) {
            toLoad.push_back(fileIdx);
        }
    }

    for (int fileIdx : cachedFileIndices) {
        if (neededFileIndices.find(fileIdx) == neededFileIndices.end()) {
            toUnload.push_back(fileIdx);
        }
    }

    // fileLoadMat/fileUnloadMat 只是往线程池派活(不在本线程解码),所以在锁里调用没问题
    for (int fileIdx : toLoad) {
        fileLoadMat(fileIdx, epoch);
    }

    for (int fileIdx : toUnload) {
        fileUnloadMat(fileIdx, epoch);
    }
}
QVector<int> ImageDeduplicateWindow::collectPrefetchWindowPairs(int centerIndex) const
{
    /*
     * 预取窗口(调用方必须已持有 m_mutex 的共享锁)。
     *
     * 去重是“顺序向后处理 Pending 图片对”,偶尔点 previous 回看上一个被处理过的对,
     * 所以两个方向要非对称:
     *   向前:只收 Pending(即将要处理的)—— 用下标+1去收没用,因为中间夹着一堆
     *         已经被处理掉的Invalid对,那就永远命不中真正下一组要处理的;
     *   向后:收“非Invalid”的(可能已经被处理成 Removed Skipped,点previous要能秒开)。
     *
     * 两端的文件都必须还在磁盘上(inDuplicate的已经搬到duplicate目录了,没法显示)。
     */
    QVector<int> result;
    const int n = static_cast<int>(m_pairs.size());
    if (centerIndex < 0 || centerIndex >= n) return result;
    if (m_prefetchWindow <= 0) return result;

    result.reserve(m_prefetchWindow * 2 + 1);

    // 文件是否还在、能不能读出来显示
    auto fileUsable = [this](int fileIdx) -> bool {
        if (fileIdx < 0 || fileIdx >= (int)m_files.size()) return false;
        const FileEntry &fe = m_files[fileIdx];
        if (fe.currentPath.isEmpty()) return false;
        /*
         * 注意:不能因为 inDuplicate 就认为不可用!
         * 被移除的文件是搬到 <原目录>/duplicate/ 下的,currentPath 已经指向新位置,
         * 文件依然存在、依然能读出来显示 —— 这正是“点 previous 回看刚处理过的
         * Remove B 那一对”能秒开的前提。
         * 之前这里写了 if (fe.inDuplicate) return false,导致回看时永远命不中缓存,
         * 只能一路往前找更早的 Pending 对。
         *
         * 有在途移动操作时也一样:文件正在被搬,磁盘上这一瞬间可能两边都不在,
         * 但"应该认为它可用" —— 让预取照常试一次,搬完就能读出来了。
         */
        if (hasPendingFileOp(fileIdx)) return true;
        return QFile::exists(fe.currentPath);
    };

    auto stateOf = [this](int pairIdx) {
        std::lock_guard<std::mutex> lkState(m_pairs[pairIdx]->stateMutex);
        return m_pairs[pairIdx]->state;
    };

    /*
     * 一对图只要还有一侧能读出来就值得预取:
     *   - Pending 的对当然两侧都在;
     *   - 已经处理过的对(RemovedA/B)一侧已经搬到 duplicate/ 里了,
     *     但 displayPair 依然能显示(另一侧正常,搬走那侧读 duplicate 里的原文件)。
     * 所以这里用 || 而不是 &&,否则点 previous 回到刚处理过的对时永远命不中缓存。
     */
    auto usablePair = [&](int pairIdx) -> bool {
        if (pairIdx < 0 || pairIdx >= n) return false;
        if (!m_pairs[pairIdx]) return false;
        return fileUsable(m_pairs[pairIdx]->idxA) || fileUsable(m_pairs[pairIdx]->idxB);
    };

    /*
     * 中心pair自己排在最前面。
     *
     * 这一步很关键:窗口原来只收"前后邻居",不含centerIndex本身。
     * 于是每次跳到一对新图,它自己的显示图都不在缓存里 ——
     * displayPair 只能在 UI 线程上现场解码+原始分辨率对齐,大图就是几百毫秒到几秒的卡死。
     * 把它自己也算进窗口之后,缓存预热和邻居是同一批任务,下一次回到这一对(previous/
     * 列表点选)就能直接命中。
     * 放在最前还有第二个好处:prefetchDisplayWindow 会先把中心pair派出去,
     * 中心pair的场景总是最急需的。
     */
    if (usablePair(centerIndex))
        result.push_back(centerIndex);

    /*
     * 筛选模式:窗口按**筛选列表的顺序**取邻居,而不是按下标。
     * 筛选集合里的对在 m_pairs 里是散开的,按下标取会把不相干的预取进来、
     * 真正下一个要处理的那一对反而没预热 —— 表现就是每切一页都"Loading pair N"。
     */
    if (m_filter.active) {
        const int centerPos = filterMemberPos(centerIndex);
        if (centerPos >= 0) {
            for (int k = 1; k <= m_prefetchWindow; ++k) {
                const int fwd = centerPos + k;
                if (fwd < (int)m_filter.members.size()) {
                    const int pi = m_filter.members[fwd].pairIndex;
                    if (usablePair(pi)) result.push_back(pi);
                }
                const int bwd = centerPos - k;
                if (bwd >= 0) {
                    const int pi = m_filter.members[bwd].pairIndex;
                    if (usablePair(pi)) result.push_back(pi);
                }
            }
        }
        return result;
    }

    // 向前:找最近的 m_prefetchWindow 个 Pending 对
    int foundForward = 0;
    for (int i = centerIndex + 1; i < n && foundForward < m_prefetchWindow; ++i) {
        if (!usablePair(i)) continue;
        if (stateOf(i) != PairState::Pending) continue;
        result.push_back(i);
        ++foundForward;
    }

    // 向后:找最近的 m_prefetchWindow 个非Invalid对(含已被处理过的)
    int foundBackward = 0;
    for (int i = centerIndex - 1; i >= 0 && foundBackward < m_prefetchWindow; --i) {
        if (!usablePair(i)) continue;
        /*
         * 失效判定用 pairLooksInvalid:被 jump 掉但对里有一侧已被别的对移走的对
         * (Skipped + InvalidB)同样不该进预取窗口 —— 它的图画不出来。
         */
        {
            const auto &pf = m_pairs[i];
            if (!pf) continue;
            std::lock_guard<std::mutex> lkState(pf->stateMutex);
            if (pairLooksInvalid(pf->state, pf->invalidMask)) continue;
        }
        result.push_back(i);
        ++foundBackward;
    }

    return result;
}

std::vector<int> ImageDeduplicateWindow::collectPrefetchPairIndices(int centerIndex)
{
    // 现在和显示图预取共用同一个窗口规则,保持两边行为一致
    const QVector<int> windowPairs = collectPrefetchWindowPairs(centerIndex);
    std::vector<int> result;
    result.reserve(windowPairs.size());
    for (int idx : windowPairs)
        result.push_back(idx);
    return result;
}
void ImageDeduplicateWindow::displayPairByIndex(int index,int epoch,
                                                DuplicatePairListView::ScrollMode listScrollMode)
{
    // 调用方可能已经持有 m_mutex(例如 onWorkerScanFinished),这里按需加锁,避免自锁
    IWD_WITH_MMUTEX_SHARED();

    /*
     * 切图耗时埋点。
     * 切图变慢的原因散在好几段(显示图是否命中缓存、列表重建/滚动、后台预取派发),
     * 光看"[displayPair] 直接命中缓存"是判断不出来到底卡在哪一段的。
     * 这里分段计时,只在总耗时超过阈值时打一行,正常情况完全静默、零干扰。
     */
    QElapsedTimer switchTimer;
    switchTimer.start();
    qint64 tDisplay = 0, tList = 0, tPrefetch = 0;

    std::shared_ptr<PairInfo> currentPair;
    {
        if (index < 0 || index >= (int)m_pairs.size()) {
            logLine("[ImageDeduplicateWindow::displayPairByIndex]:Out of Range", false, 3);
            return;
        }
        currentPair = m_pairs[index];
    }

    /*
     * ============ 显示图没就绪时,不切 currentIndex ============
     *
     * 以前是"先把 currentIndex 切过去,图没命中就交给后台,屏幕上留上一对"。
     * 大图解码要几百毫秒到 1 秒,用户连按 next 之后再按 remove B,删的已经是
     * 新 pair 的 B,而眼睛里还是上一对的图 —— 直接删错文件。
     *
     * 现在:目标图不在缓存时先不切,记下 pending 并让后台加载;
     * 加载完成后 applyDisplayImagesFromCache 会重新调用本函数真正切换。
     * 期间 currentIndex 和屏幕上的图始终是同一对。
     *
     * 坏图兜底:8 秒还没加载出来就放弃这一次,把 index 记进
     * m_displayLoadFailedIndices;用户再切到它时直接放行(显示空白),不会永久卡死。
     */
    {
        QImage tmpA, tmpB;
        const bool ready = getDisplayCache(index, tmpA, tmpB);

        if (!ready && !m_displayLoadFailedIndices.contains(index)) {
            /*
             * 先清掉上一对残留的"等待上屏"序号,并同步置起切换标记。
             *
             * 不清的话,下面 beginPairSwitchHint 触发的角标重绘会带着【上一对】
             * 的序号进 paintEvent,而 onDisplayImagePainted 只比对序号 ——
             * 一次与换图无关的重绘就足以把"切换中"清掉、按钮解锁。
             * 此时屏幕/侧栏仍是上一对、m_currentIndex 也没切,用户按下 remove
             * 作用的就是看不见的那一对。快速连按(尤其上一对刚显示"加载中/
             * 无法显示")时序号残留非常常见,这正是"保护没有立刻生效"的根因。
             * 置 m_switchInfoPending 让 toggleAB 等路径的语义与"真切换"一致。
             */
            m_awaitingPaintSerial = 0;
            m_switchInfoPending = true;
            m_pendingDisplayIndex = index;
            m_pendingDisplayEpoch = epoch;
            dedup::traceShift(QStringLiteral("WIN 切到 pair %1:显示图未就绪 -> 先不切(current 仍是 %2)")
                                  .arg(index + 1).arg(m_currentIndex + 1));
            if (m_pairList) m_pairList->setDisplaySwitchPending(true);

            setWindowTitle(QStringLiteral("Loading pair %1 ...").arg(index));

            // 顺手把邻近的超前预取上,减少后续切换的等待
            prefetchDisplayWindow(collectDisplayWindowPairs(index), epoch);
            prepareDisplayPairAsync(index, epoch);

            /*
             * 强提示:侧栏清空 + 七个操作按钮置灰。
             * 图还没上屏之前,屏幕上/侧栏里的一切都属于"上一对",任何操作都可能
             * 作用在错误的图片上 —— 与其让用户去辨认,不如直接不让点。
             */
            clearSideInfoForLoading();
            beginPairSwitchHint(index);
            setPairActionButtonsEnabled(false);

            /*
             * 同一对只记一次:等待期间用户每按一次键都会重走这条路径,
             * 以前就是一路刷屏(一次扫描能刷出几十上百条),
             * 看起来像"程序在疯狂报错",而不是"在等一张图"。
             */
            if (m_displayNotReadyLoggedIndex != index) {
                m_displayNotReadyLoggedIndex = index;
                logLine(QString("[%1] pair %2 的显示图还没就绪,切换暂停(避免图与操作对象不一致)")
                            .arg(nowStr()).arg(index));
            }

            QTimer::singleShot(8000, this, [this, index, epoch]() {
                if (epoch != m_dataEpoch) return;
                if (m_pendingDisplayIndex != index) return;

                m_pendingDisplayIndex = -1;
                m_pendingDisplayEpoch = -1;
                m_displayLoadFailedIndices.insert(index);

                /*
                 * 8 秒还没加载出来:按"这一对图片加载失败"处理,直接切过去显示空白。
                 *
                 * 以前这里是"留在上一对 + 贴侧栏 + 解锁按钮" ——
                 * 屏幕是上一对、currentIndex 是这一对,按钮还解开了,
                 * 用户按下 remove 删的就是看不见的那一对。现在把决定权交回给
                 * displayPair:failed 标记会让它走 showPairImagesUnavailable(),
                 * 显示区清空、侧栏是这一对、角标写"图片无法显示",
                 * 三处指向同一对,不会再出现"看着 A 操作 B"。
                 *
                 * 注意不要在这里手工拿锁更新侧栏:displayPairByIndex 会走完整流程
                 * (列表选中、预取、文件缓存同步),手工抄一半反而容易漏。
                 */
                logLine(QString("[%1] pair %2 显示图 8 秒内没有加载出来,按加载失败处理(显示空白)\n     %3")
                            .arg(nowStr()).arg(index)
                            .arg(displayPairPathsForLog(index)), true, 3);
                /*
                 * 不在外面拿 currentIndexMutex:displayPairByIndex 内部会去拿 m_mutex,
                 * 而后台排序线程是"先 m_mutex 再 currentIndexMutex" ——
                 * 反过来拿就是锁序反转。它自己在共享 m_mutex 下写 m_currentIndex,
                 * 已经挡住了排序线程的排他锁。
                 */
                displayPairByIndex(index, epoch);
            });
            return;
        }
    }

    m_pendingDisplayIndex = -1;
    m_pendingDisplayEpoch = -1;

    /*
     * ============ 真正切换之前:先"上锁 + 清空侧栏" ============
     *
     * 这里不再只在"显示图没命中缓存"时才清栏/锁按钮。原因:
     *   · 命中缓存、但图很大(32MP)或用户连续高速切 pair 时,setPixmap + 下一次
     *     paintEvent 的合成仍然要几十~几百毫秒,这段时间里屏幕上是上一对的图;
     *   · 而 m_currentIndex 此刻已经指向新的一对 —— 用户此时按 a/d/s(或点按钮),
     *     删的就是"看不见的那一对"。
     * 所以只要发生切换就锁,解锁统一放在 onDisplayImagePainted(真正上屏之后),
     * 侧栏信息也在那一刻才回填(现在就填会和旧图一起显示,反而更误导)。
     */
    if (index != m_currentIndex) {
        m_switchInfoPending = true;
        clearSideInfoForLoading();
        beginPairSwitchHint(index);
        setPairActionButtonsEnabled(false);
    }

    // 先设好当前下标,displayPair 要靠它去查显示图缓存
    m_currentIndex = index;
    dedup::traceShift(QStringLiteral("WIN current -> pair %1").arg(index + 1));
    if (m_pairList) m_pairList->setDisplaySwitchPending(false);
    m_displayNotReadyLoggedIndex = -1;   // 切成功一次,"还没就绪"的日志去重状态就复位

    /*
     * 复位移除"上一次等待上屏"的序号。
     *
     * 3 秒兜底要靠它判断"这一次到底有没有提交过新图":
     *   · displayPair 命中缓存 / 明确清空 -> setPixmap 返回新序号,这里非 0;
     *   · 未命中且还在后台解码 -> 这里保持 0,说明控件里还是上一对的 pixmap。
     * 不先清掉的话,残留的旧序号会让兜底误以为"已经提交过了",继续把上一对留在屏幕上。
     */
    m_awaitingPaintSerial = 0;

    displayPair(currentPair,epoch);
    tDisplay = switchTimer.elapsed();

    /*
     * 侧栏在这里主动刷一次。
     *
     * 以前只有两条路会填侧栏:"切 A/B"(toggleAB) 和 "pixdiff 算完"(notifyPixelDiffUpdated)。
     * 所以正常切 pair 时侧栏会一直停在上一对的数字上,要再点一下图片才更新 ——
     * 而切图本身是异步的,pixdiff 回调里还有个 "当前对是不是这一对" 的判据,
     * 晚到的结果会被丢掉。这里在图已经切过去之后无条件重填一次,
     * 内容是纯 FileEntry 字段,不依赖图片有没有解码完。
     */
    if (m_currentIndex >= 0 && m_currentIndex < (int)m_pairs.size()) {
        updateInfoPanelForPair(m_pairs[m_currentIndex], m_showingA);
    }

    /*
     * 兜底解锁:正常路径是 onDisplayImagePainted 贴侧栏 + 解锁。
     * 但如果这一次 displayPair 没能提交任何重绘(缓存刚被 prune、后台也没补上来),
     * paint 就永远不会来 —— 侧栏会一直空着、按钮一直灰着。
     * 3 秒后强制收尾。
     *
     * 收尾前必须先看 m_awaitingPaintSerial:为 0 表示控件里仍然是上一对的图,
     * 这时只"贴数字 + 解锁"就是最危险的状态(看着旧的、操作新的)。
     * 正确做法是把显示区清空,并标明这一对图片无法显示。
     */
    if (m_switchInfoPending) {
        QTimer::singleShot(3000, this, [this, index, epoch]() {
            if (epoch != m_dataEpoch) return;
            if (m_currentIndex != index) return;        // 用户已经切到别处,交给新的一次
            if (!m_switchInfoPending) return;           // 已经正常上屏解锁了

            if (m_awaitingPaintSerial == 0) {
                /*
                 * 没有任何新图被提交:屏幕上一定还是上一对。
                 *
                 * 先分清"还在加载"还是"已经失败":
                 * 线程池排队/磁盘慢的时候,超过 3 秒没上屏并不等于图坏了,
                 * 这时标成"图片无法显示"会让人误以为该删。
                 * 在飞的预取任务还在 -> 标"加载中";否则才是真的失败。
                 */
                bool stillLoading = false;
                {
                    std::lock_guard<std::mutex> lk(m_displayCacheMutex);
                    stillLoading = m_displayPrefetchInFlight.contains(index);
                }
                if (!stillLoading)
                    m_displayLoadFailedIndices.insert(index);

                showPairImagesUnavailable(index,
                    (stillLoading
                        ? QString("[%1] pair %2 的显示图 3 秒内还没上屏,已先清空显示区(后台仍在加载)")
                              .arg(nowStr()).arg(index)
                        : QString("[%1] pair %2 的显示图 3 秒内没有上屏,已清空显示区(不再显示上一对)")
                              .arg(nowStr()).arg(index))
                        + QStringLiteral("\n     ") + displayPairPathsForLog(index),
                    /*stillLoading=*/stillLoading);
                return;
            }

            /*
             * 已经提交过新图(pixmap 已在控件里),只是 paint 还没来
             * (最小化/被遮挡/主线程忙)。控件内容是对的,照旧贴侧栏并解锁。
             */
            if (m_pendingInfoValid) {
                applyInfoFields(m_pendingInfo);
                m_pendingInfoValid = false;
            }
            m_switchInfoPending = false;
            setPairActionButtonsEnabled(true);
            logLine(QString("[%1] pair %2 的显示图 3 秒内没有绘制完成,已强制收尾(控件内已是这一对)")
                        .arg(nowStr()).arg(index), true, 1);
        });
    }

    /*
     * 同步列表选中。
     * listScrollMode = ScrollNever 用于"用户在列表里点了一下"这种场景:
     * 他点的这一行本来就在眼前,不需要(也不应该)把滚动位置重新对到这一行。
     * 其余程序性跳转保持 ScrollAlways,但 syncCurrentIndex 内部也只在目标行
     * 已经滚出视野时才真正滚动。
     */
    syncPairListSelectionNoLock(listScrollMode);
    tList = switchTimer.elapsed() - tDisplay;

    /*
     * 为“显示图滑动窗口”里的pair在后台准备对齐后的图像。
     * 窗口在这里(已持m_mutex)算好再传进去,prefetchDisplayWindow内部不再拿m_mutex,
     * 避免和后台排序线程的锁序反转。
     */
    {
        QVector<int> displayWindow = collectDisplayWindowPairs(index);
        prefetchDisplayWindow(displayWindow, epoch);

        /*
         * 兜底:当前这一对如果没有显示图缓存、也没有进入预取窗口
         * (m_prefetchWindow<=0、或窗口规则把它排除了),单独补一次。
         * displayPair 未命中时是异步加载,不补这一次的话屏幕上会一直停留在上一对图。
         * prepareDisplayPairAsync 自带 in-flight 去重,重复调用不会造成重复工作。
         */
        QImage tmpA, tmpB;
        if (!getDisplayCache(index, tmpA, tmpB))
            prepareDisplayPairAsync(index, epoch);
    }
    tPrefetch = switchTimer.elapsed() - tDisplay - tList;

    // 智能收集：当前项前后各最多 m_prefetchWindow 个 Pending pair
    std::vector<int> prefetchPairIndices = collectPrefetchPairIndices(index);

    // 当前显示 pair 也要保留，避免它被误清
    std::unordered_set<int> keepPairSet;
    keepPairSet.insert(index);
    for (int idx : prefetchPairIndices) {
        keepPairSet.insert(idx);
    }

    // 需要保留/预加载的文件索引
    std::unordered_set<int> neededFileIndices;
    {
        auto addPairFiles = [&](int pairIdx) {
            if (pairIdx < 0 || pairIdx >= (int)m_pairs.size()) return;
            const auto &pair = m_pairs[pairIdx];
            if (!pair) return;

            neededFileIndices.insert(pair->idxA);
            neededFileIndices.insert(pair->idxB);
        };

        // 当前 pair 一定要保留文件缓存
        addPairFiles(index);

        // 邻近 Pending pair 也纳入预加载
        for (int pairIdx : prefetchPairIndices) {
            addPairFiles(pairIdx);
        }
    }

    // 只对新进入窗口的 Pending pair 请求 pixel diff 计算
    for (int pairIdx : prefetchPairIndices) {
        std::shared_ptr<PairInfo> p;
        {
            if (pairIdx < 0 || pairIdx >= (int)m_pairs.size()) continue;
            p = m_pairs[pairIdx];
        }
        if (!p) continue;

        // collectPrefetchPairIndices 已经保证它是 Pending + 文件有效
        requestComputePixelForPair(p,epoch);
    }

    // 当前显示 pair 也可以补一次请求，保证当前 UI 有 pixel diff 结果
    if (currentPair) {
        requestComputePixelForPair(currentPair,epoch);
    }

    /*
     * 同步 file cache:窗口内缺的加载,窗口外多的释放。
     *
     * 【性能】这里以前直接 lock_guard(cacheMutex) 把整段拿在 UI 线程上。
     * 但 cacheMutex 会被后台的 ensureMatCached 长时间持有 —— 解码一张 32MP(4626x7016)
     * 要几百毫秒,而且是一整段都在锁里。于是"切图"要多等几百毫秒(实测 655ms)
     * 纯粹花在等这把锁上,这就是那一行 [切图耗时] 里"其余"的来源。
     *
     * 这段做的事只是"比对两个集合,然后往线程池派活"(真正的加载/释放本来就在后台),
     * 没有任何必须在 UI 线程完成的理由。所以:
     *   1) 先 try_lock 走快路径(平时锁是空闲的,一次十几微秒);
     *   2) 抢不到就整段丢到线程池里做,UI 线程绝不等待。
     * 同时用 m_fileCacheSyncSeq 给每次切图编号:排在队列里的旧任务发现已经有更新的同步,
     * 就直接跳过,免得用一个过期的窗口集合去卸载当前需要的文件。
     */
    {
        const int syncSeq = ++m_fileCacheSyncSeq;
        std::unique_lock<std::mutex> lkSync(cacheMutex, std::try_to_lock);
        if (lkSync.owns_lock()) {
            syncFileCacheForWindowLocked(neededFileIndices, epoch);
            lkSync.unlock();
        } else {
            QPointer<ImageDeduplicateWindow> self(this);
            m_pixelThreadPool->start(new FunctionRunnable([self, neededFileIndices, epoch, syncSeq]() {
                if (!self) return;
                if (syncSeq != self->m_fileCacheSyncSeq.load(std::memory_order_relaxed))
                    return;   // 已经有更新的切图同步在排队,这次过时了
                std::lock_guard<std::mutex> lk_c(self->cacheMutex);
                // 拿到锁期间可能又有新切图,再确认一次
                if (syncSeq != self->m_fileCacheSyncSeq.load(std::memory_order_relaxed))
                    return;
                self->syncFileCacheForWindowLocked(neededFileIndices, epoch);
            }));
        }
    }

    /*
     * 切图分段耗时(只在慢的时候打一行,正常静默)。
     *
     * 现在这几段都应该是个位数毫秒 —— 解码/对齐/写缓存全部在后台,
     * UI 线程不等待任何锁。所以如果这一行又出现了,说明有新的瓶颈:
     *   display 大  -> 显示图缓存查询/上屏变慢(正常应该 <5ms)
     *   list    大  -> 列表选中/滚动/重建太慢
     *   prefetch 大 -> 派发预取时的锁竞争
     *   其余     大  -> 窗口清理 + 文件缓存同步(这条路已经 try_lock,不该再等锁)
     * 另外注意:图片本身是后台加载的,加载完成的时间不在这里,而在
     * "[displayPair] pair=N 后台加载完成,已上屏" 那一行。
     */
    {
        const qint64 total = switchTimer.elapsed();
        if (total > 30) {
            qDebug() << QStringLiteral("[切图耗时] pair=%1 总%2ms | 显示%3ms 列表%4ms 预取派发%5ms 其余%6ms")
                            .arg(index).arg(total)
                            .arg(tDisplay).arg(tList).arg(tPrefetch)
                            .arg(total - tDisplay - tList - tPrefetch);
        }
    }
}
int ImageDeduplicateWindow::findNextPending(int fromOrderIndex)
{
    /*
     * 筛选模式:整体 findNextPending 会跳到筛选集合外面去,所以只在集合内找。
     * 集合里没有可处理项时返回 m_pairs.size(),调用方按"没有更多"处理
     * (筛选阶段不会走到 showNoMorePairs,因为退出时机由用户点 finished 决定)。
     */
    if (m_filter.active) {
        int pos = filterMemberPos(fromOrderIndex >= 0 ? fromOrderIndex : m_currentIndex);
        if (pos < 0) pos = -1;
        const int next = nextActionableMember(pos, +1);
        if (next >= 0)
            return m_filter.members[next].pairIndex;
        return (int)m_pairs.size();
    }

    if (fromOrderIndex < 0 || fromOrderIndex >= (int)m_pairs.size())
        fromOrderIndex = m_currentIndex;

    // 1. 先向后搜索
    for (int i = fromOrderIndex + 1; i < (int)m_pairs.size(); ++i) {
        std::lock_guard<std::mutex> lkState(m_pairs[i]->stateMutex);
        if (m_pairs[i]->state == PairState::Pending) {
            return i;
        }
    }

    // 2. 向后没有找到，再向前搜索
    for (int i = fromOrderIndex - 1; i >= 0; --i) {
        std::lock_guard<std::mutex> lkState(m_pairs[i]->stateMutex);
        if (m_pairs[i]->state == PairState::Pending) {
            return i;
        }
    }

    // 3. 前后都没有Pending，返回 m_pairs.size()（越界位置）
    return (int)m_pairs.size();
}

/* ==================================================================
 *        单对动作的"等落盘再前进"(见 holdAdvanceUntilFileOpsSettle)
 * ================================================================== */

int ImageDeduplicateWindow::pairIndexOf(const std::shared_ptr<PairInfo> &pair) const
{
    if (!pair) return -1;
    for (int i = 0; i < (int)m_pairs.size(); ++i) {
        if (m_pairs[i] == pair) return i;
    }
    return -1;
}

bool ImageDeduplicateWindow::holdAdvanceUntilFileOpsSettle(const std::shared_ptr<PairInfo> &pair)
{
    /*
     * 这一对确实往队列里放了 rename(计数在 enqueueFileOp 里加)。
     * 有 -> 挂起前进,等 handleFileOpResult 收尾;没有 -> 调用方照旧立刻前进。
     */
    if (!pair) return false;
    if (m_pairFileOpsInFlight.value(pair.get(), 0) <= 0) return false;
    m_awaitFileOpPair = pair;
    return true;
}

void ImageDeduplicateWindow::advanceToNextPendingIfOn(const std::shared_ptr<PairInfo> &pair)
{
    /*
     * 落盘成功了,现在才允许往前走。
     *
     * 这里再确认一次"用户还停在这一对":等待期间他可能已经点了列表里别的行,
     * 或者按了 previous。那种情况下不应该用一次成功把他拽到下一个 Pending ——
     * 只把挂起标记清掉(调用方已清),什么都不做。
     *
     * 调用方(handleFileOpResult)没有持锁,这里按标准锁序自己拿:
     * m_mutex(共享) -> currentIndexMutex。
     */
    if (!pair) return;

    std::shared_lock<std::shared_mutex> lk(m_mutex);
    MMutexScope mScope(g_iwdMMutexDepth);
    std::lock_guard<std::mutex> lkIndex(currentIndexMutex);

    if (m_currentIndex < 0 || m_currentIndex >= (int)m_pairs.size()) return;
    if (m_pairs[m_currentIndex] != pair) return;

    const int nextIndex = findNextPending();
    if (nextIndex >= 0 && nextIndex < (int)m_pairs.size()) {
        displayPairByIndex(nextIndex, m_dataEpoch);
        logLine(QString("[%1] 落盘成功,前进到 pair %2").arg(nowStr()).arg(nextIndex));
    } else {
        showNoMorePairs();
    }
}

/* ==================================================================
 *                            进度条(两层)
 * ================================================================== */

void ImageDeduplicateWindow::applyProgressValue(const QString &state, int done, int total)
{
    if (!ui.progressBar) return;

    /*
     * 这里只改范围和值,不做 unpolish/polish。
     * 每次 setValue 都 unpolish+polish 会触发整流样式重算,一张图一次会把事件循环拖住,
     * 结果就是“进度条好像只在扫完整个文件夹时才跳一下”。
     * 配色只在阶段切换时(见 setProgressPhaseScanning / setProgressPhasePixelDiff)刷一次。
     */
    if (total > 0) {
        if (ui.progressBar->maximum() != total)
            ui.progressBar->setRange(0, total);
        ui.progressBar->setValue(qBound(0, done, total));
    } else {
        if (ui.progressBar->maximum() != 100)
            ui.progressBar->setRange(0, 100);
        ui.progressBar->setValue(qBound(0, done, 100));
    }
    Q_UNUSED(state);
}

void ImageDeduplicateWindow::setProgressPhaseScanning()
{
    m_pixDiffDoneCount.store(0);
    m_pixDiffExpectedCount.store(0);
    // 防御:上一轮如果因为异常在"冻结/批量"状态下中断,新的一轮必须从可计账状态开始
    m_pixDiffProgressFrozen.store(false);
    m_pixDiffBulkPhase.store(false);
    m_pixDiffPhaseStarted = false;

    if (!ui.progressBar) return;
    m_progressState.store(0);

    // 去掉pix_diff属性 -> QSS恢复第一层配色(绿chunk + 透明背景)
    ui.progressBar->setProperty("state", QVariant());
    ui.progressBar->setRange(0, 100);
    ui.progressBar->setValue(0);
    ui.progressBar->style()->unpolish(ui.progressBar);
    ui.progressBar->style()->polish(ui.progressBar);
    ui.progressBar->update();
}

void ImageDeduplicateWindow::setProgressPhasePixelDiff()
{
    if (!ui.progressBar) return;
    m_progressState.store(1);

    // 打上 pix_diff_caculating 属性:背景变绿,chunk变黄绿(第二层)
    // 分母从0开始会随“受理任务数”增长(applyProgressValue 里会同步更新range)
    const int total = std::max(1, m_pixDiffExpectedCount.load());
    ui.progressBar->setProperty("state", "pix_diff_caculating");
    ui.progressBar->setRange(0, total);
    ui.progressBar->setValue(qBound(0, m_pixDiffDoneCount.load(), total));
    ui.progressBar->style()->unpolish(ui.progressBar);
    ui.progressBar->style()->polish(ui.progressBar);
    ui.progressBar->update();
}

void ImageDeduplicateWindow::requestProgressRefresh(bool scanning, int done, int total)
{
    const int newState = scanning ? 0 : 1;

    // 阶段切换:立刻做一次完整的样式刷新
    if (m_progressState.load() != newState) {
        if (QThread::currentThread() != this->thread()) {
            QMetaObject::invokeMethod(this, [this, scanning, done, total]() {
                if (scanning) setProgressPhaseScanning();
                else          setProgressPhasePixelDiff();
                applyProgressValue(QString(), done, total);
            }, Qt::QueuedConnection);
        } else {
            if (scanning) setProgressPhaseScanning();
            else          setProgressPhasePixelDiff();
            applyProgressValue(QString(), done, total);
        }
        return;
    }

    if (QThread::currentThread() != this->thread()) {
        /*
         * 后台线程高频上报:只记录“最新的进度值”,并且最多投递一个刷新事件。
         * 这样事件队列不会被几千个刷新事件塞满,UI线程也不会忙着处理积压 ——
         * 进度条就能真正一张一张地跟着走。
         */
        m_progressDone.store(done, std::memory_order_relaxed);
        m_progressTotal.store(total, std::memory_order_relaxed);

        bool expected = false;
        if (m_progressRefreshScheduled.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            QMetaObject::invokeMethod(this, [this]() {
                m_progressRefreshScheduled.store(false, std::memory_order_release);
                applyProgressValue(QString(),
                                   m_progressDone.load(std::memory_order_relaxed),
                                   m_progressTotal.load(std::memory_order_relaxed));
            }, Qt::QueuedConnection);
        }
        return;
    }

    applyProgressValue(QString(), done, total);
}

/* ==================================================================
 *                        DuplicatePairListView 支持
 * ================================================================== */

/* 把PairInfo + 两端文件信息打包成列表要显示的一条快照 */
static DuplicatePairItem buildPairListItem(int pairIndex,
                                           const std::shared_ptr<PairInfo> &p,
                                           const std::vector<FileEntry> &files)
{
    DuplicatePairItem item;
    item.pairIndex = pairIndex;
    if (!p) return item;

    item.idxA = p->idxA;
    item.idxB = p->idxB;
    item.pHashDistance = p->pHashDistance;
    item.dHashDistance = p->dHashDistance;
    item.orbMatchRatio = p->orbMatchRatio;
    item.pixelDiffComputed = p->pixelRatioComputed;
    item.pixelDiffRatio = p->pixelDiffRatio;
    item.state = static_cast<int>(p->state);
    item.removedMask = int(p->removedMask);
    item.invalidMask = int(p->invalidMask);

    if (p->idxA >= 0 && p->idxA < (int)files.size()) {
        const FileEntry &a = files[p->idxA];
        item.pathA = a.currentPath;
        item.rootA = a.rootInputFolder;
        item.sizeA = a.size;
        item.filesizeA = a.filesize;
        item.formatA = a.format;
        item.qualityA = a.quality;
        /*
         * 列表行用纯 DQT 分。视觉指标是整解码才有的,几千行逐行算是不可接受的;
         * 要看精确对比,点开那一对,pair info 会用现算的 det 重算一次。
         */
        item.qualityRankA = qualityRank(a);
        item.inDuplicateA = a.inDuplicate;
        /*
         * 缩略图直接从 FileEntry 带过来。
         *
         * 【关键】这里以前是空的,于是每次 rebuildPairList(任何一次状态变化、
         * 每次单键处理都要整表重建)都会把所有已经加载好的缩略图全部抹掉:
         * 列表上刚显示出来的预览图瞬间变回"文件名占位",必须等视图重新
         * paint 登记懒加载 -> 60ms 定时器 -> 再请求一轮才回来。
         * 表现出来就是"预览图经常不显示,要手动滚一下才出来,而且一出来就是一大片"。
         *
         * QImage 拷贝是引用计数,几乎零成本,所以这里直接带上不会拖慢重建。
         */
        item.thumbA = a.thumbnail;
    }
    if (p->idxB >= 0 && p->idxB < (int)files.size()) {
        const FileEntry &b = files[p->idxB];
        item.pathB = b.currentPath;
        item.rootB = b.rootInputFolder;
        item.sizeB = b.size;
        item.filesizeB = b.filesize;
        item.formatB = b.format;
        item.qualityB = b.quality;
        item.qualityRankB = qualityRank(b);
        item.inDuplicateB = b.inDuplicate;
        item.thumbB = b.thumbnail;
    }
    return item;
}

void ImageDeduplicateWindow::rebuildPairList()
{
    if (!m_pairList) return;

    QVector<DuplicatePairItem> items;

    /*
     * 筛选模式:列表只显示筛选集合里的那几对。
     * 行 -> pairIndex 是模型里的哈希映射(runForPairIndex),所以这里塞的是
     * 真实的 m_pairs 下标,updatePairRow/refreshPairListRow 都不用改。
     */
    if (m_filter.active && !m_filter.members.empty()) {
        items.reserve((int)m_filter.members.size());
        for (const FilterMember &fm : m_filter.members) {
            const int i = fm.pairIndex;
            if (i < 0 || i >= (int)m_pairs.size() || !m_pairs[i]) continue;
            std::lock_guard<std::mutex> lkState(m_pairs[i]->stateMutex);
            DuplicatePairItem item = buildPairListItem(i, m_pairs[i], m_files);
            item.filterAnchor = fm.anchorFile;
            items.push_back(item);
        }
        m_pairList->setPairs(items);
        return;
    }

    items.reserve(static_cast<int>(m_pairs.size()));
    for (int i = 0; i < (int)m_pairs.size(); ++i) {
        const std::shared_ptr<PairInfo> &p = m_pairs[i];
        if (!p) {
            /*
             * 理论到不了这里(buildPairsFromFiles 只 push 有效 pair)。
             * 真遇到也不能跳过 —— 跳过会让 items 的行号与 m_pairs 下标错位,
             * 之后所有 updatePairRow(pairIndex) 都会刷到别的行上。
             * 塞一个空占位,行号保持一一对应。
             */
            DuplicatePairItem placeholder;
            placeholder.pairIndex = i;
            items.push_back(placeholder);
            continue;
        }
        std::lock_guard<std::mutex> lkState(p->stateMutex);
        items.push_back(buildPairListItem(i, p, m_files));
    }
    m_pairList->setPairs(items);
}

void ImageDeduplicateWindow::syncPairListSelectionNoLock(DuplicatePairListView::ScrollMode scrollMode)
{
    if (!m_pairList) return;

    const int index = m_currentIndex;
    const int pairCount = static_cast<int>(m_pairs.size());

    m_suppressListActivation = true;
    m_pairList->syncCurrentIndex(index, pairCount, scrollMode);
    m_suppressListActivation = false;
}

void ImageDeduplicateWindow::syncPairListSelection(DuplicatePairListView::ScrollMode scrollMode)
{
    if (!m_pairList) return;
    std::lock_guard<std::mutex> lk(currentIndexMutex);
    syncPairListSelectionNoLock(scrollMode);
}

void ImageDeduplicateWindow::finalizePairStateChange(DuplicatePairListView::ScrollMode scrollMode)
{
    /*
     * 一次状态变化后的统一收尾(调用方已持有 m_mutex 与 currentIndexMutex)。
     *
     * 必须整表重建,不能只刷新被操作的那一行:
     *   移除A/B会把引用了同一个文件的**其它pair**连带改成 InvalidA/InvalidB,
     *   还会写 invalidCauseForA/B(点击Invalid项要跳到"肇事"的那一对)。
     *   只刷一行的话这些行在模型里还是 Pending,表现就是:
     *     - 状态依旧显示 Pending;
     *     - 点击它不会触发 invalidPairActivated(视图是按模型里的state决定发哪个信号的);
     *     - previous 回看时那一对看起来也是 Pending。
     * pair数量在千级,整表重建开销可以忽略,换来的是一致性。
     */
    rebuildPairList();
    // rebuildPairList -> setPairs 会把选中重置到第0行,这里必须把选中同步回真实的
    // m_currentIndex,否则批量操作后列表会"跳回当前显示的pair对"。
    syncPairListSelectionNoLock(scrollMode);
    // 模型reset之后再显式让整个列表重绘一次,避免已经画好的行不刷新
    if (m_pairList && m_pairList->viewport())
        m_pairList->viewport()->update();
}

void ImageDeduplicateWindow::finalizePairStateChangeRows(const QVector<int> &rows,
                                                         DuplicatePairListView::ScrollMode scrollMode)
{
    /*
     * 行级收尾:只刷新"这次动作真正改到的行"。
     *
     * 和 finalizePairStateChange() 的区别只在刷新范围 —— 语义上的收尾(同步选中、
     * 重绘视图)完全一样。单键操作走这条路,因为一次 remove A/B 通常只影响
     * "这一对 + 引用同一个文件的少数几对";35000 对的会话里整表重建是 O(N),
     * 每点一次都要重排一遍模型、丢掉所有已加载缩略图,这不该发生在点一个按钮上。
     */
    QVector<int> uniq = rows;
    std::sort(uniq.begin(), uniq.end());
    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());

    for (int i : std::as_const(uniq))
        refreshPairListRow(i);

    syncPairListSelectionNoLock(scrollMode);
    if (m_pairList && m_pairList->viewport())
        m_pairList->viewport()->update();
}

void ImageDeduplicateWindow::advanceToNextPendingAfterBatch()
{
    /*
     * 批量操作(右键批量 / 一键自动去重)之后调用。
     *
     * 在此之前的状态是:批量可能把 m_currentIndex 指向的这对也一并处理掉了
     * (它自己就在选中范围内,或者它的 A/B 被别的 pair 移除而连带成了 Invalid)。
     * 老代码只做 finalizePairStateChange(),列表会把这一对重新选中/显示出来,
     * 用户看到的还是一对已经处理完的图 —— 必须再手动点一次(甚至点那个已经无效的
     * 操作)才能前进到下一对,体验很糟。
     *
     * 这里统一判断一次:当前这对如果已经不是 Pending,就直接跳到下一个 Pending。
     */
    const int pairCount = static_cast<int>(m_pairs.size());

    bool currentStillPending = false;
    if (m_currentIndex >= 0 && m_currentIndex < pairCount) {
        std::lock_guard<std::mutex> lkState(m_pairs[m_currentIndex]->stateMutex);
        currentStillPending = (m_pairs[m_currentIndex]->state == PairState::Pending);
    }

    if (currentStillPending) {
        // 当前这对还没被处理:保持不动,只把选中/滚动同步到位
        syncPairListSelectionNoLock(DuplicatePairListView::ScrollAlways);
        return;
    }

    const int nextIndex = findNextPending();
    if (nextIndex >= 0 && nextIndex < pairCount) {
        displayPairByIndex(nextIndex, m_dataEpoch);
    } else {
        showNoMorePairs();
    }
}

void ImageDeduplicateWindow::refreshPairListRow(int pairIndex)
{
    /*
     * 只刷新一行(按钮/快捷键处理完单个pair后调用)。
     * 调用方可能已经持有 m_mutex,所以走带守卫的按需加锁。
     */
    if (!m_pairList) return;
    if (pairIndex < 0 || pairIndex >= (int)m_pairs.size()) return;

    DuplicatePairItem item;
    {
        IWD_WITH_MMUTEX_SHARED();
        std::lock_guard<std::mutex> lkState(m_pairs[pairIndex]->stateMutex);
        item = buildPairListItem(pairIndex, m_pairs[pairIndex], m_files);
    }

    m_pairList->updatePairRow(pairIndex, item.state, item.removedMask, item.invalidMask,
                              item.pixelDiffComputed, item.pixelDiffRatio);
    m_pairList->updatePairRowMetrics(pairIndex, item.pHashDistance,
                                     item.dHashDistance, item.orbMatchRatio);
}

QString ImageDeduplicateWindow::pairRootFolder(int fileIdx) const
{
    if (fileIdx < 0 || fileIdx >= (int)m_files.size()) return QString();
    return m_files[fileIdx].rootInputFolder;
}

int ImageDeduplicateWindow::findInvalidCausePair(int pairIndex, bool preferA)
{
    if (pairIndex < 0 || pairIndex >= (int)m_pairs.size()) return -1;

    std::lock_guard<std::mutex> lkState(m_pairs[pairIndex]->stateMutex);
    const int causeA = m_pairs[pairIndex]->invalidCauseForA - 1;   // 存的是index+1
    const int causeB = m_pairs[pairIndex]->invalidCauseForB - 1;

    auto valid = [this](int idx) {
        return idx >= 0 && idx < (int)m_pairs.size();
    };

    if (preferA) {
        if (valid(causeA)) return causeA;
        if (valid(causeB)) return causeB;
    } else {
        if (valid(causeB)) return causeB;
        if (valid(causeA)) return causeA;
    }
    return -1;
}

void ImageDeduplicateWindow::loadPairListThumbnails(const QVector<int> &pairIndices)
{
    if (!m_pairList || pairIndices.isEmpty()) return;

    /*
     * 缩略图统一放在 FileEntry::thumbnail 里(唯一来源),不在PairInfo里另存一份。
     * 判断顺序:
     *   1) FileEntry里已经有合法缩略图 -> 直接推给列表,不读盘;
     *   2) 没有 -> 读盘生成,并写回 FileEntry(别的pair引用同一张图时下次直接命中)。
     */
    QVector<int> pairIdxList;   // 要填的pair
    QVector<int> fileIdxA;      // -1表示用FileEntry里已有的缩略图,不用读盘
    QVector<int> fileIdxB;
    /*
     * 同时记下请求时这一对到底是哪两张图。
     * 缩略图是异步生成的,期间可能发生重排(像素差异算完会sort),
     * 那时 pairIndex 指向的已经是另一对了 —— 不校验就会把A的缩略图贴到B那一行,
     * 表现出来就是“预览图和点开后的图片对完全不是同一对”。
     */
    QVector<int> expectIdxA;
    QVector<int> expectIdxB;

    {
        std::shared_lock<std::shared_mutex> lk(m_mutex);
        for (int pairIndex : pairIndices) {
            if (pairIndex < 0 || pairIndex >= (int)m_pairs.size()) continue;
            int idxA = -1, idxB = -1;
            {
                std::lock_guard<std::mutex> lkState(m_pairs[pairIndex]->stateMutex);
                idxA = m_pairs[pairIndex]->idxA;
                idxB = m_pairs[pairIndex]->idxB;
            }

            auto decide = [this](int fileIdx) -> int {
                if (fileIdx < 0 || fileIdx >= (int)m_files.size()) return -1;
                const FileEntry &fe = m_files[fileIdx];
                const bool hasThumb = !fe.thumbnail.isNull() &&
                                      fe.thumbnail.width()  <= dedup::kThumbnailMaxSize &&
                                      fe.thumbnail.height() <= dedup::kThumbnailMaxSize;
                return hasThumb ? -1 : fileIdx;   // -1 = 已有缩略图,不用读
            };

            pairIdxList.push_back(pairIndex);
            fileIdxA.push_back(decide(idxA));
            fileIdxB.push_back(decide(idxB));
            expectIdxA.push_back(idxA);
            expectIdxB.push_back(idxB);
        }
    }

    if (pairIdxList.isEmpty()) return;

    // ===== 1) 已经有缩略图的直接推给列表,不等异步 =====
    {
        std::shared_lock<std::shared_mutex> lk(m_mutex);
        for (int i = 0; i < pairIdxList.size(); ++i) {
            const int pairIndex = pairIdxList[i];
            // 校验这一对的组成没变(重排过就丢弃)
            {
                if (pairIndex < 0 || pairIndex >= (int)m_pairs.size()) continue;
                std::lock_guard<std::mutex> lkState(m_pairs[pairIndex]->stateMutex);
                if (m_pairs[pairIndex]->idxA != expectIdxA[i] ||
                    m_pairs[pairIndex]->idxB != expectIdxB[i])
                    continue;
            }
            if (fileIdxA[i] < 0) {
                const int idx = expectIdxA[i];
                if (idx >= 0 && idx < (int)m_files.size())
                    m_pairList->setThumbnail(pairIndex, true, m_files[idx].thumbnail);
            }
            if (fileIdxB[i] < 0) {
                const int idx = expectIdxB[i];
                if (idx >= 0 && idx < (int)m_files.size())
                    m_pairList->setThumbnail(pairIndex, false, m_files[idx].thumbnail);
            }
        }
    }

    // ===== 2) 剩下的读盘生成 =====
    QStringList pathsA, pathsB;
    {
        std::shared_lock<std::shared_mutex> lk(m_mutex);
        for (int i = 0; i < pairIdxList.size(); ++i) {
            if (fileIdxA[i] >= 0) pathsA.push_back(m_files[fileIdxA[i]].currentPath);
            if (fileIdxB[i] >= 0) pathsB.push_back(m_files[fileIdxB[i]].currentPath);
        }
    }
    if (pathsA.isEmpty() && pathsB.isEmpty()) return;

    QPointer<ImageDeduplicateWindow> self(this);
    QtConcurrent::run([self, pairIdxList, fileIdxA, fileIdxB, expectIdxA, expectIdxB,
                       pathsA, pathsB]() {
        if (!self) return;

        QVector<QImage> imagesA;
        imagesA.reserve(pathsA.size());
        for (const QString &path : pathsA)
            imagesA.push_back(dedup::makeThumbnailFromFile(path, dedup::kThumbnailMaxSize));

        QVector<QImage> imagesB;
        imagesB.reserve(pathsB.size());
        for (const QString &path : pathsB)
            imagesB.push_back(dedup::makeThumbnailFromFile(path, dedup::kThumbnailMaxSize));

        QTimer::singleShot(0, self, [self, pairIdxList, fileIdxA, fileIdxB,
                                     expectIdxA, expectIdxB, imagesA, imagesB]() {
            if (!self) return;
            std::shared_lock<std::shared_mutex> lk(self->m_mutex);

            int ai = 0, bi = 0;
            for (int i = 0; i < pairIdxList.size(); ++i) {
                const int pairIndex = pairIdxList[i];

                /*
                 * 关键:投递前校验这一对还是当初那一对。
                 * 期间可能发生过重排(像素差异算完会重新排序),那时 pairIndex
                 * 已经指向别的图片对了,再贴上去就是"预览图错位"。
                 */
                {
                    if (pairIndex < 0 || pairIndex >= (int)self->m_pairs.size()) {
                        if (fileIdxA[i] >= 0) ++ai;
                        if (fileIdxB[i] >= 0) ++bi;
                        continue;
                    }
                    std::lock_guard<std::mutex> lkState(self->m_pairs[pairIndex]->stateMutex);
                    if (self->m_pairs[pairIndex]->idxA != expectIdxA[i] ||
                        self->m_pairs[pairIndex]->idxB != expectIdxB[i]) {
                        if (fileIdxA[i] >= 0) ++ai;
                        if (fileIdxB[i] >= 0) ++bi;
                        continue;   // 已经换人了,丢弃这两张缩略图
                    }
                }

                if (fileIdxA[i] >= 0 && ai < imagesA.size()) {
                    const int fileIdx = expectIdxA[i];
                    if (fileIdx >= 0 && fileIdx < (int)self->m_files.size()) {
                        self->m_files[fileIdx].thumbnail = imagesA[ai];
                        if (self->m_pairList)
                            self->m_pairList->setThumbnail(pairIndex, true, imagesA[ai]);
                    }
                    ++ai;
                }
                if (fileIdxB[i] >= 0 && bi < imagesB.size()) {
                    const int fileIdx = expectIdxB[i];
                    if (fileIdx >= 0 && fileIdx < (int)self->m_files.size()) {
                        self->m_files[fileIdx].thumbnail = imagesB[bi];
                        if (self->m_pairList)
                            self->m_pairList->setThumbnail(pairIndex, false, imagesB[bi]);
                    }
                    ++bi;
                }
            }
        });
    });
}

bool ImageDeduplicateWindow::restorePairToPending(std::shared_ptr<PairInfo> pairptr)
{
    if (!pairptr) return false;

    const bool hasA = (pairptr->idxA >= 0 && pairptr->idxA < (int)m_files.size());
    const bool hasB = (pairptr->idxB >= 0 && pairptr->idxB < (int)m_files.size());

    std::lock_guard<std::mutex> lkState(pairptr->stateMutex);

    /*
     * "恢复为 Pending"分两件事:
     *   1) 撤销【本对】自己做过的删除(ownRemovalBits:有效对看 state,失效对看 removedMask);
     *   2) 没有本对删过的图时,只把 Skipped 这个标记抹掉(见下面 own == 0 的分支)。
     *
     * 第 1 条不能替别的对做主:纯失效对在这里恢复等于把肇事那一对的 remove
     * 一起撤销(文件回原位、兄弟对重新变 Pending),而那一对仍写着 RemovedA/B。
     *
     * 复合态(RemovedA + InvalidB)恢复后只能回到 InvalidB:
     * A 是本对删的,搬回来; B 是别的对删的,一动不动,只是本对不再"删过 A"。
     * 旧实现整对拒绝,于是这种对的 A 再也拿不回来。
     */
    const quint8 own = ownRemovalBits(pairptr->state, pairptr->removedMask);
    if (own == 0) {
        /*
         * 没有本对删过的图 —— 但这里必须分成三种情况。
         *
         * 以前一律拒绝,于是 Skipped 的对再也改不回 Pending(用户报的:
         * "skipped pair 无法恢复为 pending"):跳过的对从来没删过任何文件,
         * 它需要的是"把 Skipped 这个标记抹掉",一行磁盘操作都不用做。
         *
         *   1) Pending:本来就是待处理,无事可做,算成功(批量里别计成失败);
         *   2) Skipped:撤销跳过标记 -> invalidMask 为 0 就是 Pending,
         *      否则是"某一侧确实被别的对移走了",收尾成对应的 Invalid*(别装成 Pending);
         *   3) Invalid*(且本对一只都没删):这是被别的对牵连失效的,
         *      在这里恢复等于替肇事那一对做主(文件会搬回、兄弟对重变 Pending,
         *      而肇事对还写着 Removed*),继续拒绝并指路。
         */
        if (pairptr->state == PairState::Pending)
            return true;

        if (pairptr->state == PairState::Skipped) {
            pairptr->state = invalidStateFromMask(pairptr->invalidMask);
            pairptr->removedMask = 0;
            logLine(QStringLiteral("[%1] 跳过的一对已恢复(收尾状态 %2;没有文件需要搬回)")
                        .arg(nowStr(), QString::fromUtf8(pairStateName(pairptr->state))), true);
            return true;
        }

        const int causeA = pairptr->invalidCauseForA - 1;
        const int causeB = pairptr->invalidCauseForB - 1;
        const int cause = (causeA >= 0) ? causeA : causeB;
        logLine(QStringLiteral("[%1] 拒绝把失效对恢复为 Pending(它没有本对删过的图,"
                               "是被别的对牵连失效的);请到 pair %2 上修改")
                    .arg(nowStr()).arg(cause >= 0 ? QString::number(cause)
                                                  : QStringLiteral("(未知)")), true);
        return false;
    }

    /*
     * 只恢复本对删过的那一侧。
     *
     * optimisticState 传 -1:一次恢复失败时【不】回滚这一对的状态 ——
     * 一对的两个文件可能只有一个恢复失败,回滚成"已处理"反而更不贴合实际;
     * 保持原样让用户能看到它、再决定怎么处理。
     * 失败的文件级回滚和"其它pair的Invalid标记"由 handleFileOpResult 负责。
     */
    if (hasA && (own & SideBitA)) {
        const int idx = pairptr->idxA;
        if (restoreFromDuplicate(idx, pairptr))
            markRelatedPairsValidState(m_files[idx], pairptr, true);
    }
    if (hasB && (own & SideBitB)) {
        const int idx = pairptr->idxB;
        if (restoreFromDuplicate(idx, pairptr))
            markRelatedPairsValidState(m_files[idx], pairptr, true);
    }

    /*
     * 收尾状态由"还有哪一侧是别的对删掉的"决定,和 invalidMask 对齐:
     *   invalidMask == 0 -> 真的回到 Pending;
     *   否则            -> 仍然 Invalid*(B 还是不在,这一对依然不能操作)。
     * invalidCause 保留着,点它还能跳回肇事那一对。
     */
    pairptr->removedMask = 0;
    pairptr->state = invalidStateFromMask(pairptr->invalidMask);
    return true;
}

void ImageDeduplicateWindow::applyBatchAction(const QVector<int> &pairIndices,
                                              ActionRecord::Type type)
{
    if (blockedByFolderDecision(QStringLiteral("批量操作")))
        return;
    if (m_filter.active) {
        // 批量会跨到筛选列表以外,和"只收拾这几对"的语义冲突
        createFloatingLabel(this, QStringLiteral("筛选状态下不能用批量操作"));
        logLine(QStringLiteral("[%1] 筛选状态下忽略批量操作").arg(nowStr()));
        return;
    }

    QVector<int> ordered = pairIndices;
    std::sort(ordered.begin(), ordered.end());

    int applied = 0;
    int skippedInvalid = 0;
    for (int pairIndex : std::as_const(ordered)) {
        if (pairIndex < 0 || pairIndex >= (int)m_pairs.size()) continue;

        std::shared_ptr<PairInfo> p;
        {
            std::shared_lock<std::shared_mutex> lk(m_mutex);
            if (pairIndex >= (int)m_pairs.size()) continue;
            p = m_pairs[pairIndex];
        }
        if (!p) continue;

        // 注意:前面的处理可能已经把这个pair变成Invalid(或变回Pending),
        // 所以每一个都要在应用前重新判断一次,依次处理。
        bool invalid = false;
        bool sideAlive = false;   // 失效对:这次要删的那一侧还在不在
        bool removable = false;   // 这一轮到底要不要下"只删一侧"的动作
        {
            std::lock_guard<std::mutex> lkState(p->stateMutex);
            /*
             * 用 pairLooksInvalid 而不是 isInvalidPairState:
             * Skipped / Removed* 也可能"某一侧已经被别的对移走"(invalidMask)。
             */
            invalid = pairLooksInvalid(p->state, p->invalidMask);

            int fileIdx = -1;
            if (type == ActionRecord::RemoveA)      { fileIdx = p->idxA; removable = true; }
            else if (type == ActionRecord::RemoveB) { fileIdx = p->idxB; removable = true; }
            if (fileIdx >= 0 && fileIdx < (int)m_files.size()) {
                sideAlive = !m_files[fileIdx].inDuplicate
                            && !m_files[fileIdx].currentPath.isEmpty();
            }
        }

        if (invalid) {
            /*
             * 失效对不等于"什么都不能做"。
             *   · remove A/B:删掉【还活着的那一侧】是独立决定,记成复合态
             *     (InvalidB + RemovedA)。要删的那一侧已经没了才算跳过。
             *   · 恢复为 Pending:只撤销本对删过的图,由 restorePairToPending 自己判断;
             *     本对一只都没删 -> 它会返回 false,计入 skipped。
             *   · remove both / jump:拒绝(会碰上已经被别人处理掉的那一侧)。
             */
            if (removable) {
                if (!sideAlive) { ++skippedInvalid; continue; }
                bool ok = false;
                {
                    std::lock_guard<std::mutex> lkState(p->stateMutex);
                    ok = removeSideOfPair(p, type == ActionRecord::RemoveA);
                }
                if (ok) ++applied;
                else    ++skippedInvalid;
                continue;
            }
            if (type == ActionRecord::None) {
                if (restorePairToPending(p)) ++applied;
                else                         ++skippedInvalid;
                continue;
            }
            ++skippedInvalid;
            continue;
        }

        if (type == ActionRecord::None) {
            if (!restorePairToPending(p))
                continue;
        } else {
            std::lock_guard<std::mutex> lkState(p->stateMutex);
            applyActionToPair(p, type);
        }
        ++applied;
    }

    // 前面pair的状态变化会影响后面pair的Invalid状态,整体刷一遍。
    // 批量操作时用 ScrollNever:滚动位置交给下面的 advanceToNextPendingAfterBatch 决定,
    // 免得整表重建+同步选中把用户滚了半天的列表又拽回原来那一对。
    /*
     * 批量操作的提示音(bubble pop):和单对的 pop 明显不同,
     * 用户一听就知道"刚才那一下是按选区处理的、不是只动了屏幕上这一对"。
     * 一对都没处理成(全被跳过)就不响 —— 那说明想法没落地,不该给"成功"的反馈。
     */
    if (applied > 0)
        dedupenv::playSound(DedupSound::BatchAction);
    {
        std::shared_lock<std::shared_mutex> lk(m_mutex);
        /*
         * 登记"本线程已持有 m_mutex":下面 finalizePairStateChange /
         * advanceToNextPendingAfterBatch -> displayPairByIndex -> displayPair
         * 内部都会走 IWD_WITH_MMUTEX_SHARED()。不登记的话它们会以为自己没持锁,
         * 在已经持共享锁的情况下再拿一次 —— SRWLock 上共享锁重入虽然通常可行,
         * 但一旦某条路径改成排他就会直接死锁,所以统一用深度标记挡住。
         */
        MMutexScope mScope(g_iwdMMutexDepth);
        std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
        finalizePairStateChange(DuplicatePairListView::ScrollNever);
        // 当前显示的这对如果也被批量处理掉了,直接前进到下一个 Pending,
        // 而不是把一对已经处理完的图留在屏幕上让用户"再点一次才能走"。
        advanceToNextPendingAfterBatch();
    }

    logLine(QString("[%1] 批量操作完成,共处理 %2 / %3 个pair")
                .arg(nowStr()).arg(applied).arg(ordered.size()), true);
    if (skippedInvalid > 0) {
        logLine(QStringLiteral("[%1] 批量操作跳过 %2 个对(要删的那一侧已经不在,"
                               "或失效对里没有本对删过的图可撤销)")
                    .arg(nowStr()).arg(skippedInvalid), true);
    }
}

/* ==================================================================
 *                      显示图缓存(滑动窗口)
 * ================================================================== */

bool ImageDeduplicateWindow::getDisplayCache(int pairIndex, QImage &outA, QImage &outB)
{
    IWD_WITH_MMUTEX_SHARED();

    {
        std::lock_guard<std::mutex> lk(m_displayCacheMutex);
        if (!m_displayCacheIndex.contains(pairIndex))
            return false;
    }

    // 显示图就存在 PairInfo 里,直接取 displayPairA / displayPairB
    if (pairIndex < 0 || pairIndex >= (int)m_pairs.size()) return false;
    const auto &p = m_pairs[pairIndex];
    if (!p) return false;

    std::lock_guard<std::mutex> lkState(p->stateMutex);
    if (!p->displayPairReady) return false;

    outA = p->displayPairA;
    outB = p->displayPairB;
    return !outA.isNull() || !outB.isNull();
}

void ImageDeduplicateWindow::putDisplayCache(int pairIndex, const QImage &imgA, const QImage &imgB)
{
    if (imgA.isNull() && imgB.isNull()) return;

    IWD_WITH_MMUTEX_SHARED();

    if (pairIndex < 0 || pairIndex >= (int)m_pairs.size()) return;
    const auto &p = m_pairs[pairIndex];
    if (!p) return;

    {
        std::lock_guard<std::mutex> lkState(p->stateMutex);
        p->displayPairA = imgA;
        p->displayPairB = imgB;
        p->displayPairReady = true;
    }
    {
        std::lock_guard<std::mutex> lkCache(m_displayCacheMutex);
        m_displayCacheIndex.insert(pairIndex, true);
    }
}

bool ImageDeduplicateWindow::markDisplayPrefetchInFlight(int pairIndex)
{
    if (pairIndex < 0) return false;
    std::lock_guard<std::mutex> lk(m_displayCacheMutex);

    // 单调时钟:系统时间被改也不影响"在飞多久"的判断
    const qint64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch()).count();
    auto it = m_displayPrefetchInFlight.find(pairIndex);
    if (it != m_displayPrefetchInFlight.end()) {
        /*
         * 20 秒还没结束的"在飞"标记,按陈旧标记处理。
         *
         * 正常一对图(哪怕 56MP)也就 2 秒左右;超过 20 秒只可能是:
         *   · 任务所在线程出了意外,guard 没跑到;
         *   · 下标被重排过,任务按旧下标清了标记,新下标留在这里。
         * 这两种情况如果继续信任标记,这一对就永远不再派发、永远不上屏。
         * 宁可多解码一次,也不能把图卡死。
         */
        if (now - it.value() < kDisplayPrefetchStaleMs)
            return false;   // 真的还有人在做
        qDebug() << QStringLiteral("[prefetchDisplay] pair=%1 的 in-flight 标记已存在 %2ms,视为陈旧并重新派发")
                        .arg(pairIndex).arg(now - it.value());
    }

    m_displayPrefetchInFlight.insert(pairIndex, now);
    return true;
}

void ImageDeduplicateWindow::clearDisplayPrefetchInFlight(int pairIndex)
{
    std::lock_guard<std::mutex> lk(m_displayCacheMutex);
    m_displayPrefetchInFlight.remove(pairIndex);
}

void ImageDeduplicateWindow::pruneDisplayCache(const QSet<int> &keepPairs)
{
    // 先收集要淘汰的下标(只持自己的锁),再去清 PairInfo 里的图
    QVector<int> evicted;
    {
        std::lock_guard<std::mutex> lk(m_displayCacheMutex);
        for (auto it = m_displayCacheIndex.begin(); it != m_displayCacheIndex.end(); ) {
            if (!keepPairs.contains(it.key())) {
                evicted.push_back(it.key());
                it = m_displayCacheIndex.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (evicted.isEmpty()) return;

    IWD_WITH_MMUTEX_SHARED();

    for (int pairIndex : std::as_const(evicted)) {
        if (pairIndex < 0 || pairIndex >= (int)m_pairs.size()) continue;
        const auto &p = m_pairs[pairIndex];
        if (!p) continue;
        std::lock_guard<std::mutex> lkState(p->stateMutex);
        p->displayPairA = QImage();   // 引用清零 -> 像素立刻释放
        p->displayPairB = QImage();
        p->displayPairReady = false;
    }
}

void ImageDeduplicateWindow::clearDisplayPairImagesLocked()
{
    /*
     * 只动 PairInfo 里的显示图,不碰 m_mutex ——
     * 调用方必须已经持有 m_mutex(共享或排他都可以)。
     * 之所以拆出这个函数:clearState() 是在 load() 已经持有 m_mutex 排他锁的情况下被调用的,
     * 如果里面再去拿一次 m_mutex 就会自锁。
     */
    for (auto &p : m_pairs) {
        if (!p) continue;
        std::lock_guard<std::mutex> lkState(p->stateMutex);
        p->displayPairA = QImage();   // 引用清零 -> 像素立刻释放
        p->displayPairB = QImage();
        p->displayPairReady = false;
    }
}

void ImageDeduplicateWindow::clearDisplayCache()
{
    IWD_WITH_MMUTEX_SHARED();
    clearDisplayPairImagesLocked();

    std::lock_guard<std::mutex> lkCache(m_displayCacheMutex);
    m_displayCacheIndex.clear();
    /*
     * 注意:这里【不】清 m_displayPrefetchInFlight。
     * 那些标记由各自的后台任务在结束时(RAII guard)自己摘掉;
     * 强行清掉会让"同一对重复派发"的保护失效。
     * 数据换了代(epoch)时后台任务本来就会自行放弃并摘标记。
     */
}

QVector<int> ImageDeduplicateWindow::collectDisplayWindowPairs(int centerIndex) const
{
    // 显示图缓存和 pixel diff 预取共用同一套窗口规则(向前Pending / 向后非Invalid)
    return collectPrefetchWindowPairs(centerIndex);
}

void ImageDeduplicateWindow::prefetchDisplayWindow(const QVector<int> &windowPairs, int epoch)
{
    /*
     * 注意:本函数不获取 m_mutex,也不获取 currentIndexMutex。
     * (调用方可能正持有 currentIndexMutex,而且后台排序线程会持 m_mutex 等 currentIndexMutex,
     *  在这里去拿 m_mutex 就是锁序反转 -> 死锁。)
     *
     * 1) 先按窗口裁剪缓存,窗口外的立刻释放;
     * 2) 窗口内已缓存的跳过;
     * 3) 剩余的交给后台线程:读原图 -> 原始分辨率对齐 -> 转QImage -> 塞进缓存。
     *    后台任务里自己会拿 m_mutex 做快照/校验,锁序是 m_mutex -> cacheMutex,和别处一致。
     */
    QSet<int> keep;
    keep.reserve(windowPairs.size());
    for (int idx : std::as_const(windowPairs))
        keep.insert(idx);
    pruneDisplayCache(keep);

    // 找出还没缓存的
    QVector<int> need;
    for (int idx : std::as_const(windowPairs)) {
        QImage a, b;
        if (!getDisplayCache(idx, a, b))
            need.push_back(idx);
    }
    if (need.isEmpty()) return;

    /*
     * 每个 pair 一个任务丢进全局池,让多核同时干 —— 原来是一个串行 for 循环,
     * 窗口里 5 对就依次解码+对齐 5 张大图,串起来就是"切过去还得等好几秒"。
     * 解码/对齐都是只读各自 FileEntry 的 m_mat,互不干扰,cv::Mat 引用计数是线程安全的。
     * 写缓存由 putDisplayCache 自己的锁保护,顺序无所谓。
     *
     * 每个pair用 in-flight 标记登记:同一对不会重复派发(prefetch 可能被反复触发)。
     */
    for (int pairIndex : std::as_const(need)) {
        prepareDisplayPairAsync(pairIndex, epoch);
    }
}

void ImageDeduplicateWindow::prepareDisplayPairAsync(int pairIndex, int epoch)
{
    // 窗口正在析构:别再往池里丢任务(见析构里的 m_tearingDown 说明)
    if (m_tearingDown.load()) return;

    QPointer<ImageDeduplicateWindow> self(this);

    // 已经在预取队列里了:后台完成时会自己回 UI 换图,不用重复派发
    if (!markDisplayPrefetchInFlight(pairIndex))
        return;

    /*
     * 派发到窗口自己的 m_prefetchPool(不是全局池):析构时 waitForDone 才管得住它。
     * 池是 this 的子对象,生命周期覆盖整个窗口;任务体里仍然只认 QPointer,
     * 万一窗口先没了也不会去解引用。
     */
    QtConcurrent::run(m_prefetchPool, [self, pairIndex, epoch]() {
        QElapsedTimer taskTimer;
        taskTimer.start();

        // 无论走哪条 return,离开时都要清掉 in-flight 标记
        struct InFlightGuard {
            QPointer<ImageDeduplicateWindow> w;
            int idx;
            ~InFlightGuard() { if (w) w->clearDisplayPrefetchInFlight(idx); }
        } guard{self, pairIndex};
        if (!self) return;
        if (epoch != self->m_dataEpoch) return;

        int idxA = -1, idxB = -1;
        QString pathA, pathB;
        {
            std::shared_lock<std::shared_mutex> lk(self->m_mutex);
            if (pairIndex < 0 || pairIndex >= (int)self->m_pairs.size()) return;
            const auto &p = self->m_pairs[pairIndex];
            if (!p) return;
            idxA = p->idxA;
            idxB = p->idxB;
            // 路径只用于失败日志(定位"到底是哪张图打不开"),在锁内拷一份
            if (idxA >= 0 && idxA < (int)self->m_files.size())
                pathA = self->m_files[idxA].currentPath;
            if (idxB >= 0 && idxB < (int)self->m_files.size())
                pathB = self->m_files[idxB].currentPath;
        }
        if (idxA < 0 || idxB < 0) return;

        /*
         * 失败上报。
         *
         * 以前这里所有失败分支都是静默 return:界面上只能看到"图一直不上屏",
         * 然后等 8 秒超时去猜。现在把原因(哪一侧、路径、文件在不在、解码/对齐/
         * 转换哪一步失败)带回 UI 线程,由 onDisplayPairLoadFailed 统一收尾 ——
         * 该清空显示区就清空,该记日志就记日志。
         */
        auto reportFailure = [self, pairIndex, epoch, &taskTimer](const QString &why) {
            if (!self) return;
            qDebug().noquote() << QStringLiteral("[prefetchDisplay] pair=%1 失败(耗时 %2ms): %3")
                                      .arg(pairIndex).arg(taskTimer.elapsed()).arg(why);
            QMetaObject::invokeMethod(self, [self, pairIndex, epoch, why]() {
                if (!self) return;
                self->onDisplayPairLoadFailed(pairIndex, epoch, why);
            }, Qt::QueuedConnection);
        };

        auto describeFile = [](int idx, const QString &path) -> QString {
            if (path.isEmpty())
                return QStringLiteral("idx=%1(路径为空)").arg(idx);
            return QStringLiteral("%1(idx=%2, %3)")
                .arg(path).arg(idx)
                .arg(QFile::exists(path) ? QStringLiteral("存在") : QStringLiteral("文件不存在"));
        };

        cv::Mat a, b, aAl, bAl;
        try {
            {
                /*
                 * 只持 m_mutex(m_files 的容器稳定性需要它),【不】持 cacheMutex:
                 * ensureMatCached 自己管理锁,解码那段只占该文件的 matMutex ——
                 * 这一对的解码和其它 pair 的解码可以真正并行,不再互相排队。
                 *
                 * 句柄必须由 ensureMatCached 在锁内交出来(outMat),不能 ensure 完再去读
                 * m_files[].m_mat —— 那中间有窗口:别的线程可能刚好把这个下标从 LRU 淘汰并
                 * releaseMat,这里就会拿到空矩阵然后直接放弃,表现为"图一直不上屏"。
                 */
                std::shared_lock<std::shared_mutex> lk(self->m_mutex);
                const bool okA = self->ensureMatCached(idxA, 1, idxA, idxB, &a);
                const bool okB = self->ensureMatCached(idxB, 1, idxA, idxB, &b);
                if (!okA || !okB) {
                    /*
                     * 文件正在搬运(auto remove / restore 是异步的)时读不到属正常,
                     * 不是"坏图":搬完 handleFileOpResult 会给 pending/当前对补派一次。
                     * 这里绝不能上报失败 —— 否则界面会立刻把这一对标成"图片无法显示",
                     * 而它 200ms 后其实就能正常打开了。
                     */
                    if (self->hasPendingFileOp(idxA) || self->hasPendingFileOp(idxB)) {
                        qDebug() << QStringLiteral("[prefetchDisplay] pair=%1 有文件正在搬运,等搬完再重试")
                                        .arg(pairIndex);
                        return;
                    }
                    reportFailure(QStringLiteral("pair %1 图片解码失败: A=%2 [%3], B=%4 [%5]")
                                      .arg(pairIndex)
                                      .arg(describeFile(idxA, pathA),
                                           okA ? QStringLiteral("ok") : QStringLiteral("解码失败"))
                                      .arg(describeFile(idxB, pathB),
                                           okB ? QStringLiteral("ok") : QStringLiteral("解码失败")));
                    return;
                }
            }
            if (a.empty() || b.empty()) {
                reportFailure(QStringLiteral("pair %1 图片矩阵为空(缓存竞态): A %2x%3, B %4x%5")
                                  .arg(pairIndex).arg(a.cols).arg(a.rows)
                                  .arg(b.cols).arg(b.rows));
                return;
            }

            // 原始分辨率对齐:小的放大到大的尺寸,不降采样
            self->makeAlignedPair(a, b, aAl, bAl);
            if (aAl.empty() || bAl.empty()) {
                reportFailure(QStringLiteral("pair %1 对齐失败: A %2x%3 ch=%4 -> %5x%6, "
                                             "B %7x%8 ch=%9 -> %10x%11")
                                  .arg(pairIndex)
                                  .arg(a.cols).arg(a.rows).arg(a.channels())
                                  .arg(aAl.cols).arg(aAl.rows)
                                  .arg(b.cols).arg(b.rows).arg(b.channels())
                                  .arg(bAl.cols).arg(bAl.rows));
                return;
            }

            /*
             * 转成显示用的 QImage。
             * 不透明的图会走 Format_RGB32 —— 实测 12.6MP 下 QPixmap::fromImage
             * 从 24~29ms 降到 0ms(无需格式转换),绘制时也不再走棋盘格。
             * 真的带透明像素的图仍然保留 RGBA,观感不变。
             * 这遍转换在后台线程,不占 UI。
             */
            QImage imgA = cvMatToDisplayQImage(aAl);
            QImage imgB = cvMatToDisplayQImage(bAl);
            if (imgA.isNull() || imgB.isNull()) {
                reportFailure(QStringLiteral("pair %1 显示转换失败: A %2x%3 ch=%4 depth=%5 -> %6, "
                                             "B %7x%8 ch=%9 depth=%10 -> %11")
                                  .arg(pairIndex)
                                  .arg(aAl.cols).arg(aAl.rows).arg(aAl.channels()).arg(aAl.depth())
                                  .arg(imgA.isNull() ? QStringLiteral("null") : QStringLiteral("ok"))
                                  .arg(bAl.cols).arg(bAl.rows).arg(bAl.channels()).arg(bAl.depth())
                                  .arg(imgB.isNull() ? QStringLiteral("null") : QStringLiteral("ok")));
                return;
            }

            if (!self) return;

            // 写缓存前再确认一次:期间可能发生过排序,下标对应的pair已经变了
            {
                std::shared_lock<std::shared_mutex> lk(self->m_mutex);
                if (epoch != self->m_dataEpoch) return;
                if (pairIndex < 0 || pairIndex >= (int)self->m_pairs.size()) return;
                const auto &p = self->m_pairs[pairIndex];
                if (!p || p->idxA != idxA || p->idxB != idxB) return;  // 已经换人了,丢弃
            }
            self->putDisplayCache(pairIndex, imgA, imgB);
            qDebug() << QStringLiteral("[prefetchDisplay] pair=%1 完成,耗时 %2ms (A:%3x%4 B:%5x%6)")
                            .arg(pairIndex).arg(taskTimer.elapsed())
                            .arg(imgA.width()).arg(imgA.height())
                            .arg(imgB.width()).arg(imgB.height());

            /*
             * 回 UI 线程换图。
             *
             * 这一步是"未命中也能不卡"的关键:displayPair 在未命中时直接返回、
             * 屏幕上先留着上一对图,由这里在准备好之后把新图换上去。
             * 通过队列投递到 UI 线程执行,所以绘制的线程亲和性没有问题。
             * pairIndex 已经不对应当前显示项时,applyDisplayImagesFromCache 会自己丢弃。
             */
            QMetaObject::invokeMethod(self, [self, pairIndex, epoch]() {
                if (!self) return;
                self->applyDisplayImagesFromCache(pairIndex, epoch);
            }, Qt::QueuedConnection);
        } catch (const std::exception &e) {
            // 单张失败不影响其它prefetch,但必须让界面知道(否则就是"图一直不上屏")
            reportFailure(QStringLiteral("pair %1 显示图预处理异常: %2")
                              .arg(pairIndex).arg(QString::fromUtf8(e.what())));
        } catch (...) {
            reportFailure(QStringLiteral("pair %1 显示图预处理异常(未知类型)").arg(pairIndex));
        }
    });
}

void ImageDeduplicateWindow::applyDisplayImagesFromCache(int pairIndex, int epoch)
{
    if (epoch != m_dataEpoch) return;

    /*
     * 之前被"显示图未就绪"拦下的切换,现在可以真正完成了。
     * displayPairByIndex 会去拿 m_mutex 共享锁,所以必须在 currentIndexMutex 之外调用。
     */
    if (m_pendingDisplayIndex >= 0
        && pairIndex == m_pendingDisplayIndex
        && epoch == m_pendingDisplayEpoch) {
        const int target = m_pendingDisplayIndex;
        m_pendingDisplayIndex = -1;
        m_pendingDisplayEpoch = -1;
        displayPairByIndex(target, epoch);
        return;
    }

    std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
    if (pairIndex != m_currentIndex) return;   // 用户已经切走了,别把旧图盖上去

    QImage imgA, imgB;
    if (!getDisplayCache(pairIndex, imgA, imgB)) return;
    if (imgA.isNull() && imgB.isNull()) return;

    m_pixA = QPixmap::fromImage(imgA);
    m_pixB = QPixmap::fromImage(imgB);

    m_showingA = true;
    m_currentPairImageUnavailable = false;   // 后台补图成功:恢复正常角标
    m_currentPairImageLoading = false;
    m_displayLoadFailedIndices.remove(pairIndex);   // 之前失败过也不再算"坏图"
    m_awaitingPaintSerial = m_display->setPixmap(m_pixA);
    m_display->fitToWidget(true);
    m_display->clearLargeZoomedFlag();

    qDebug() << QStringLiteral("[displayPair] pair=%1 后台加载完成,已上屏 (A:%2x%3 B:%4x%5)")
                    .arg(pairIndex)
                    .arg(imgA.width()).arg(imgA.height())
                    .arg(imgB.width()).arg(imgB.height());
}

void ImageDeduplicateWindow::showPairImagesUnavailable(int pairIndex, const QString &detail,
                                                      bool stillLoading)
{
    /*
     * 图片拿不到时的唯一显示收尾。
     *
     * 【为什么必须清空而不是留着上一对】
     * 侧栏/角标/currentIndex 都已经指向这一对,只有画面还是上一对 ——
     * 用户没有任何办法知道自己看的不是当前对。按下 remove 时,
     * 删掉的是"看不见的那一对"里的文件,而且无法撤销。
     * 空白 + 明确角标是唯一不会骗人的状态。
     *
     * 清空是"控件内容"层面的:setPixmap(QPixmap()) 会把 m_pix 置空,
     * 下一次 paint 画底色 + 角标。这里不等 paint ——
     * 等 paint 再改状态会让 3 秒兜底/失败回调互相等待。
     */
    m_currentPairImageUnavailable = !stillLoading;
    m_currentPairImageLoading = stillLoading;
    m_pixA = QPixmap();
    m_pixB = QPixmap();
    m_showingA = true;

    if (m_display) {
        m_awaitingPaintSerial = m_display->setPixmap(QPixmap());
        m_display->fitToWidget(true);
        m_display->clearLargeZoomedFlag();
    } else {
        m_awaitingPaintSerial = 0;
    }

    /*
     * 侧栏立刻贴这一对的信息 + 状态。
     * 屏幕是空白的,不存在"旧图配新数字"的问题;
     * 反而必须贴出来 —— 用户要靠路径/大小判断这一对要不要处理。
     */
    if (m_pendingInfoValid) {
        applyInfoFields(m_pendingInfo);
        m_pendingInfoValid = false;
    }
    m_switchInfoPending = false;

    m_displayPairNumber = pairIndex + 1;
    const QString stateText = stillLoading ? QStringLiteral("加载中")
                                            : QStringLiteral("图片无法显示");
    setPairLoadingHint(QStringLiteral("%1: pair %2").arg(stateText).arg(m_displayPairNumber));
    if (m_display && m_displayPairTotal > 0) {
        m_display->setStatusBadge(QStringLiteral("%1 · pair %2 / %3 · %4")
                                      .arg(m_showingA ? QStringLiteral("A") : QStringLiteral("B"))
                                      .arg(m_displayPairNumber).arg(m_displayPairTotal)
                                      .arg(stateText),
                                  /*busy=*/true);
    }

    /*
     * 解锁:这一对已经"定案"了(空白 + 明示),用户需要能继续 next/previous
     * 或按侧栏信息处理它。再锁着就变成"卡死"。
     */
    setPairActionButtonsEnabled(true);

    if (!detail.isEmpty())
        logLine(detail, true, 3);
}

QString ImageDeduplicateWindow::displayPairPathsForLog(int pairIndex)
{
    /*
     * 只给"超时/失败"这种低频日志用。
     * 下一轮如果再遇到"某一对打不开",这一行就能直接告诉我们是哪两张图,
     * 不用再去猜 pair 下标对应哪两个文件。
     */
    QString pathA, pathB;
    {
        std::shared_lock<std::shared_mutex> lk(m_mutex);
        if (pairIndex < 0 || pairIndex >= (int)m_pairs.size())
            return QStringLiteral("pair %1: 下标越界").arg(pairIndex);
        const auto &p = m_pairs[pairIndex];
        if (!p) return QStringLiteral("pair %1: 空指针").arg(pairIndex);

        auto pathOf = [this](int idx) -> QString {
            if (idx < 0 || idx >= (int)m_files.size())
                return QStringLiteral("<idx %1 越界>").arg(idx);
            return m_files[idx].currentPath;
        };
        pathA = pathOf(p->idxA);
        pathB = pathOf(p->idxB);
    }

    // 文件存在性检查放在锁外:网络盘/慢盘上 QFile::exists 可能卡住
    auto decorate = [](const QString &path) -> QString {
        if (path.isEmpty()) return QStringLiteral("<空路径>");
        if (path.startsWith(QLatin1Char('<'))) return path;   // 越界占位
        return path + (QFile::exists(path) ? QString() : QStringLiteral(" [文件不存在]"));
    };
    return QStringLiteral("A=%1 | B=%2").arg(decorate(pathA), decorate(pathB));
}

void ImageDeduplicateWindow::onDisplayPairLoadFailed(int pairIndex, int epoch,
                                                     const QString &detail)
{
    /*
     * 后台预取发现这一对解不出来。
     *
     * 关键:不管是"用户正等着切过去"(pending)还是"这一对已经在屏幕上但图还没到",
     * 都必须把显示区清空 —— 否则上一对的图会继续冒充这一对。
     * 预取窗口里其它对的失败不打扰当前界面,只在第一次记一行日志。
     */
    if (epoch != m_dataEpoch) return;

    const bool firstFailure = !m_displayLoadFailedIndices.contains(pairIndex);
    if (firstFailure)
        m_displayLoadFailedIndices.insert(pairIndex);

    /*
     * 这里【不】拿 currentIndexMutex:
     * displayPairByIndex 要去拿 m_mutex,而后台排序线程是
     * "先 m_mutex 再 currentIndexMutex";反过来拿就是锁序反转(见文件顶部)。
     * 本函数和 m_pendingDisplayIndex/m_currentIndex 都只在 UI 线程读写,
     * 而 displayPairByIndex 自己会在共享 m_mutex 下写 m_currentIndex。
     */

    // 用户正停在"等这一对上屏"的状态:直接完成切换(displayPair 会清空显示区)
    if (m_pendingDisplayIndex == pairIndex && m_pendingDisplayEpoch == epoch) {
        m_pendingDisplayIndex = -1;
        m_pendingDisplayEpoch = -1;
        if (firstFailure) logLine(detail, true, 3);
        displayPairByIndex(pairIndex, epoch);
        return;
    }

    // 这一对就是当前显示的:把残留的上一对图清掉(可能用户还没察觉)
    if (m_currentIndex == pairIndex) {
        if (firstFailure) logLine(detail, true, 3);
        if (!m_currentPairImageUnavailable)
            showPairImagesUnavailable(pairIndex, QString());
        return;
    }

    // 预取窗口里的邻居失败:不影响屏幕,第一次记一行就够,别刷屏
    if (firstFailure) {
        logLine(QString("[%1] pair %2 预取失败(当前显示不受影响): %3")
                    .arg(nowStr()).arg(pairIndex).arg(detail), false, 1);
    }
}

void ImageDeduplicateWindow::displayPair(shared_ptr<PairInfo> PairPtr,int epoch)
{
    /*
     * 本函数只读 m_pairs/m_files,这里按需加共享锁。
     * 调用方(displayPairByIndex)已经确保 m_mutex 处于“共享持有”状态,
     * 所以这里不会再重复加锁;若将来有人从排他锁上下文调用它,
     * 深度标记同样能拦住重复加锁(见文件顶部的说明)。
     */
    IWD_WITH_MMUTEX_SHARED();

    try{
        int pairIndex = -1;
        {
            if (!PairPtr) return;
            pairIndex = m_currentIndex;   // 调用方(displayPairByIndex)已设好
        }

        /*
         * 进入本函数 = 正在把这一对放到屏幕上,先撤销上一对留下的"图片不可用"标记。
         * 后面命中缓存 / 后台补图成功会保持 false;
         * 明确失败的分支会再把它置回 true(见 showPairImagesUnavailable)。
         */
        m_currentPairImageUnavailable = false;
        m_currentPairImageLoading = false;

        /*
         * 侧栏文字先算好(此刻还在共享锁里),等图真正上屏时再贴。
         *
         * 必须放在 setPixmap 【之前】:一旦提交了重绘,理论上就有可能在
         * 这里和下一条语句之间跑到 paint,那时文字还没准备好就会贴出一片空白。
         * 计算本身是纯字符串拼装(几十微秒),放到前面没有任何代价。
         */
        m_pendingInfo = makeInfoFields(PairPtr, true);
        m_pendingInfoValid = !m_pendingInfo.isEmpty();
        m_switchInfoPending = true;
        if (!m_pendingInfoValid) {
            /*
             * 算不出来(下标越界/pair 无效)—— 侧栏会保持清空状态。
             * 正常流程到不了这里,记一条日志:
             * 下次再出现"信息框黑掉",先看这条,而不是从界面猜。
             */
            logLine(QString("[%1] pair %2 的侧栏信息算不出来(idxA=%3 idxB=%4 files=%5)")
                        .arg(nowStr()).arg(pairIndex)
                        .arg(PairPtr ? PairPtr->idxA : -1)
                        .arg(PairPtr ? PairPtr->idxB : -1)
                        .arg((int)m_files.size()), true, 3);
        }

        /*
         * 显示图缓存策略:预取窗口(中心pair + 向前最近的Pending对 + 向后最近的非Invalid对,
         * 数量由 m_prefetchWindow 决定)。
         *   1) 命中 -> 直接上屏(零解码零对齐);
         *   2) 未命中 -> 交给后台预取(见下面 if (!cacheHit) 的注释),UI 线程绝不阻塞。
         * 整个缓存只覆盖窗口内的pair,窗口滑动时外面的一律立刻释放。
         * 对齐一律原始分辨率:小的放大到大的尺寸,不降采样。
         */
        QImage Aimg, Bimg;
        bool cacheHit = false;
        if (pairIndex >= 0)
            cacheHit = getDisplayCache(pairIndex, Aimg, Bimg);

        qDebug() << QStringLiteral("[displayPair] pair=%1 displayPairA/B %2  (A:%3x%4 B:%5x%6)")
                        .arg(pairIndex)
                        .arg(cacheHit ? QStringLiteral("直接命中缓存,无需解码/对齐")
                                      : QStringLiteral("未命中,转后台加载"))
                        .arg(Aimg.width()).arg(Aimg.height())
                        .arg(Bimg.width()).arg(Bimg.height());

        if (!cacheHit) {
            /*
             * 【性能】未命中时绝不在 UI 线程上解码 + 对齐。
             *
             * 这里原来是:拿着 cacheMutex 同步 ensureMatCached(A/B) 再 makeAlignedPair。
             * 对 32MP(4626x7016)的图,两次整图解码就要一秒多(实测[切图耗时] 显示 1473ms),
             * 而且 cacheMutex 是全局的 —— 这段时间把后台预取、像素计算全部串住。
             * 更糟的是如果解码失败,还会把这一对的两张空图写进显示缓存。
             *
             * 现在改成完全异步:
             *   - 这一对会被紧随其后的 prefetchDisplayWindow 收进窗口(中心 pair 排最前),
             *     在后台并行地解码 + 对齐 + 转 QImage(见 prepareDisplayPairAsync);
             *   - 后台完成后回 UI 线程调 applyDisplayImagesFromCache 把图换上去;
             *   - 这期间屏幕上保持上一对图,但按钮锁着("切换中"角标),
             *     任何 remove/jump 都进不来(见 applyActionToCurrentPair 的锁检查)。
             */
            m_showingA = true;

            auto fileReadable = [this](int fIdx) -> bool {
                if (fIdx < 0 || fIdx >= (int)m_files.size()) return false;
                const FileEntry &fe = m_files[fIdx];
                if (fe.currentPath.isEmpty()) return false;
                if (hasPendingFileOp(fIdx)) return true;   // 正在搬,搬完就能读
                return QFile::exists(fe.currentPath);
            };
            const bool aReadable = fileReadable(PairPtr->idxA);
            const bool bReadable = fileReadable(PairPtr->idxB);

            /*
             * 两种"肯定显示不出来"的情况,必须立刻清空显示区:
             *   1) 两侧文件都不存在(后台也解不出任何东西);
             *   2) 这一对已经被确认加载失败过(8 秒超时 / 后台解码失败)。
             *
             * 以前这里只处理第 1 种,第 2 种会把上一对的图留在屏幕上,
             * 只把侧栏/currentIndex 换成这一对 —— 用户看着上一对的图按 remove,
             * 删掉的是另一对的文件。现在:没有图就显示空白,并用角标明确写出来。
             */
            if (!aReadable && !bReadable) {
                showPairImagesUnavailable(pairIndex,
                    QString("[%1] pair %2 两侧文件都不存在,显示区已清空")
                        .arg(nowStr()).arg(pairIndex));
            } else if (m_displayLoadFailedIndices.contains(pairIndex)) {
                showPairImagesUnavailable(pairIndex,
                    QString("[%1] pair %2 的图片无法加载,显示区已清空(不再显示上一对)")
                        .arg(nowStr()).arg(pairIndex));
            } else {
                logLine(QString("[%1] pair %2 显示图未命中缓存,已转后台加载")
                            .arg(nowStr()).arg(pairIndex));
            }
        } else {
            m_pixA = QPixmap::fromImage(Aimg);
            m_pixB = QPixmap::fromImage(Bimg);

            m_showingA = true;
            m_currentPairImageUnavailable = false;
            m_currentPairImageLoading = false;
            m_displayLoadFailedIndices.remove(pairIndex);   // 能命中缓存 = 上次失败已恢复
            m_awaitingPaintSerial = m_display->setPixmap(m_pixA);
            //显示新图片组后清除右键放大/缩小标记
            m_display->fitToWidget(true);
            m_display->clearLargeZoomedFlag();
        }

        /*
         * 侧栏信息在这里【不贴】,只在前面算好:
         * setPixmap 只是提交了一次重绘,真正上屏要等下一次 paintEvent,
         * 那之前屏幕上还是上一对的图,先贴数字就是"旧图 + 新数字"。
         */
        requestComputePixelForPair(PairPtr,epoch);
    }catch(std::exception&ex){
        logLine(ex.what(), false, 3);
    }

    // request pixel compare
}

void ImageDeduplicateWindow::makeAlignedPair(const cv::Mat &A, const cv::Mat &B, cv::Mat &outA, cv::Mat &outB) const
{
    /*
     * 具体实现搬到了 dedup::makeAlignedPair(deduputils),原因:
     *   - 它是纯图像处理,和查重/显示没有耦合,放一起才符合本文件的分工;
     *   - 更重要的:长宽比失真这种问题必须能【单独测】—— 现在
     *     build/import_test/align_test.cpp 可以直接拿它跑断言,
     *     不用去猜界面里那两张图到底被拉成什么样。
     */
    dedup::alignPairToCommonSize(A, B, outA, outB);
}

void ImageDeduplicateWindow::computePixelForPair(std::shared_ptr<PairInfo> pairptr,int epoch)
{
    if (!m_enablePixelDiff.load()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lkQueue(m_pixelQueuedMutex);
        std::lock_guard<std::mutex> lkState(pairptr->stateMutex);

        if (pairptr->pixelRatioComputed) return;
        if (m_pixelQueued.find(pairptr) != m_pixelQueued.end()) return;

        m_pixelQueued.insert(pairptr);
    }

    // 批量阶段的分母已经在开工前一次性设好(见 m_pixDiffBulkPhase),这里不能再 +1
    if (!m_pixDiffBulkPhase.load(std::memory_order_relaxed)
        && !m_pixDiffProgressFrozen.load(std::memory_order_relaxed))
        m_pixDiffExpectedCount.fetch_add(1);
    computePixelForPairCore(pairptr,epoch);
}
void ImageDeduplicateWindow::requestComputePixelForPair(std::shared_ptr<PairInfo> pairptr,int epoch)
{
    if (!m_enablePixelDiff.load()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lkQueue(m_pixelQueuedMutex);
        std::lock_guard<std::mutex> lkState(pairptr->stateMutex);

        if (pairptr->pixelRatioComputed) return;
        if (m_pixelQueued.find(pairptr) != m_pixelQueued.end()) return;

        m_pixelQueued.insert(pairptr);
    }

    // 批量阶段的分母已经一次性设好;只有"阶段外"的单对请求才需要自己占一格
    if (!m_pixDiffBulkPhase.load(std::memory_order_relaxed)
        && !m_pixDiffProgressFrozen.load(std::memory_order_relaxed))
        m_pixDiffExpectedCount.fetch_add(1);

    auto task = [this, pairptr,epoch]() {
        computePixelForPairCore(pairptr,epoch);
    };

    if (m_pixelThreadPool) {
        // 你原来已经有 FunctionRunnable 的话，直接继续用即可，不需要新增类
        m_pixelThreadPool->start(new FunctionRunnable(task));
    } else {
        task();
    }
}

void ImageDeduplicateWindow::computePixelForPairCore(std::shared_ptr<PairInfo> pairptr,int epoch)
{
    // 确保在任何退出路径都移除 queued 标记
    struct AutoUnqueue {
        ImageDeduplicateWindow *wnd;
        std::shared_ptr<PairInfo> ptr;
        AutoUnqueue(ImageDeduplicateWindow *w, std::shared_ptr<PairInfo> i)
            : wnd(w), ptr(std::move(i)) {}
        ~AutoUnqueue() {
            std::lock_guard<std::mutex> lkQueue(wnd->m_pixelQueuedMutex);
            wnd->m_pixelQueued.erase(ptr);
        }
    } au(this, pairptr);

    // 结束一个pair的pixel diff计算(含提前返回的失败情况、异常情况)都要推进度条。
    // 注意:它必须在 try 之前定义,这样 catch 分支里也能用 ——
    // 以前异常路径不推进度,进度条会永远差那几条算不完。
    auto tickPixelDiffProgress = [this]() {
        /*
         * 冻结期间(收尾补算)不推进度:
         * 主阶段已经让 done/expected 相等(=100%),补算是修复,不该把进度条拽回去。
         */
        if (m_pixDiffProgressFrozen.load(std::memory_order_relaxed)) return;
        const int done = m_pixDiffDoneCount.fetch_add(1) + 1;
        // 分母用“实际受理的任务数”,不是 m_pairs.size()
        const int total = std::max(done, m_pixDiffExpectedCount.load());
        requestProgressRefresh(/*scanning=*/false, done, total);
    };

    try {
        cv::Mat aAl, bAl;

        std::shared_lock<std::shared_mutex> lk(m_mutex);

        if(epoch!=m_dataEpoch)
            return;


        auto normalizeToBGR = [this](cv::Mat &m) {
            if (m.empty()) return;
            if (m.type() == CV_8UC3) return;

            try {
                if (m.channels() == 4) {
                    cv::Mat tmp;
                    cv::cvtColor(m, tmp, cv::COLOR_BGRA2BGR);
                    m = tmp;
                } else if (m.channels() == 1) {
                    cv::Mat tmp;
                    cv::cvtColor(m, tmp, cv::COLOR_GRAY2BGR);
                    m = tmp;
                } else {
                    cv::Mat tmp;
                    if (m.depth() != CV_8U) m.convertTo(tmp, CV_8U);
                    else tmp = m;

                    if (tmp.channels() != 3) {
                        std::vector<cv::Mat> chs;
                        cv::split(tmp, chs);
                        if (chs.size() >= 3) {
                            std::vector<cv::Mat> sel{chs[0], chs[1], chs[2]};
                            cv::merge(sel, m);
                        } else {
                            cv::Mat gray;
                            cv::cvtColor(tmp, gray, cv::COLOR_GRAY2BGR);
                            m = gray;
                        }
                    } else {
                        m = tmp;
                    }
                }
            } catch (const std::exception &e) {
                /*
                 * 原来这里是 catch(...) 直接 m = cv::Mat() —— 失败被彻底吞掉,
                 * 上层只能看到"矩阵空了",于是把这一对记成 pix diff = 1 却不知道为什么。
                 * 这里至少把异常原因写进日志面板(Mostly 是内存不足)。
                 */
                m = cv::Mat();
                logLine(QString("[%1] 像素比较:转换为 BGR 失败: %2")
                            .arg(nowStr()).arg(QString::fromUtf8(e.what())), false, 3);
            } catch (...) {
                m = cv::Mat();
                logLine(QString("[%1] 像素比较:转换为 BGR 失败(未知异常,多半是内存不足)").arg(nowStr()), false, 3);
            }
        };

        {
            const int idxA = pairptr->idxA;
            const int idxB = pairptr->idxB;

            /*
             * 命中任一缓存都直接复用;未命中才加载并放进pix计算的LRU。
             * 注意:A/B正在被使用,LRU淘汰时要避开它们,而且不能碰预加载缓存里的下标
             * (以前的写法会无条件 releaseMat 被淘汰的下标,可能把 displayPair
             * 以为还在的预加载矩阵提前释放掉,导致缓存状态和实际不一致)。
             *
             * 这里【不】持 cacheMutex:ensureMatCached 自己管理锁,解码只占该文件的
             * matMutex —— 否则 32MP 一张 1 秒的解码会把所有后台图像任务串成一条线。
             *
             * 【必须用 outMat 拿句柄】不要 ensure 完再自己去读 m_files[].m_mat:
             * 那两步之间存在窗口,别的线程此时往 LRU 里塞新文件可能刚好把这一对淘汰掉,
             * 读到的就是空矩阵 —— 于是这一对会掉进下面的"计算失败"分支,
             * 被永久记成 pixelDiffRatio=1(实测 bug:两张完全一致的图显示 pix diff=1,
             * 事后重扫又变成 0)。
             */
            cv::Mat A, B;
            const bool okA = ensureMatCached(idxA, 1, idxA, idxB, &A);
            const bool okB = ensureMatCached(idxB, 1, idxA, idxB, &B);
            if (!okA || !okB) {
                qDebug() << QStringLiteral("[pixelDiff] pair 矩阵不可用: idxA=%1(%2) idxB=%3(%4)")
                                .arg(idxA).arg(okA ? "ok" : "fail")
                                .arg(idxB).arg(okB ? "ok" : "fail");
            }

            normalizeToBGR(A);
            normalizeToBGR(B);

            if (A.empty() || B.empty()) {
                /*
                 * 会话已经结束(关窗/换数据集把 epoch 推走了):
                 * 这时候读不到矩阵是**预期**的 —— clear file 会马上把这一对里
                 * 判定为重复的图片移进回收站,而我们在它之后才跑到这里。
                 * 直接放弃,不要记成"像素比较失败"(那会把日志刷满"矩阵不可用")。
                 */
                if (epoch != m_dataEpoch.load(std::memory_order_relaxed))
                    return;

                /*
                 * 计算失败。
                 *
                 * 这个 ratio=1 是"算不了"的哨兵值,不是真的像素差异 ——
                 * 而这里以前是直接落 computed=true + ratio=1,于是:
                 *   1) 界面上永远显示 "pix diff = 1"(用户看到的就是这个);
                 *   2) 因为 computed 已经是 true,这一对再也不会重算,
                 *      只有整批重扫才会变回正确值。
                 *
                 * 现在:记一次失败,**先不锁死**(留给重试),连续失败到上限才落 1,
                 * 并且把原因写进日志面板(以前只写 qDebug,界面上根本看不到)。
                 *
                 * 最常见的失败原因是内存不够:单任务瞬时峰值几百 MB,
                 * 批量扫描并发多个就是 GB 级 —— 分配异常会被 normalizeToBGR
                 * 的 catch(...) 静默吞掉,矩阵就空了。见 kPixelDiffMemoryBudgetMB。
                 */
                handlePixelDiffFailure(pairptr,
                    QStringLiteral("矩阵不可用 idx(%1,%2) A=%3 B=%4 (okA=%5 okB=%6) A路径=%7(%8) B路径=%9(%10)")
                        .arg(idxA).arg(idxB)
                        .arg(A.empty() ? QStringLiteral("空") : QStringLiteral("非空"))
                        .arg(B.empty() ? QStringLiteral("空") : QStringLiteral("非空"))
                        .arg(okA ? 1 : 0).arg(okB ? 1 : 0)
                        .arg(idxA >= 0 && idxA < (int)m_files.size() ? m_files[idxA].currentPath
                                                                    : QStringLiteral("?"))
                        .arg(idxA >= 0 && idxA < (int)m_files.size()
                                 && QFile::exists(m_files[idxA].currentPath)
                                 ? QStringLiteral("在") : QStringLiteral("不在"))
                        .arg(idxB >= 0 && idxB < (int)m_files.size() ? m_files[idxB].currentPath
                                                                    : QStringLiteral("?"))
                        .arg(idxB >= 0 && idxB < (int)m_files.size()
                                 && QFile::exists(m_files[idxB].currentPath)
                                 ? QStringLiteral("在") : QStringLiteral("不在")));
                notifyPixelDiffUpdated(pairptr, epoch);
                tickPixelDiffProgress();
                return;
            }

            makeAlignedPair(A, B, aAl, bAl);

            /*
             * A/B 到这里已经没用了(后面只用 aAl/bAl)。
             * 主动释放掉:它们是 normalizeToBGR 新开的 3 通道副本,
             * 32MP 一对就是 186MB —— 不释放的话,computePixelDiff 里
             * 再分配 absdiff/灰度时峰值会叠上去(实测峰值 714MB 里这部分占 186MB)。
             */
            A.release();
            B.release();
        }

        if (aAl.type() != CV_8UC3) {
            try {
                cv::Mat tmp;
                aAl.convertTo(tmp, CV_8U);
                if (tmp.channels() != 3) cv::cvtColor(tmp, aAl, cv::COLOR_GRAY2BGR);
                else aAl = tmp;
            } catch (...) {}
        }

        if (bAl.type() != CV_8UC3) {
            try {
                cv::Mat tmp;
                bAl.convertTo(tmp, CV_8U);
                if (tmp.channels() != 3) cv::cvtColor(tmp, bAl, cv::COLOR_GRAY2BGR);
                else bAl = tmp;
            } catch (...) {}
        }

        if (aAl.channels() == 4 && bAl.channels() == 3) cv::cvtColor(aAl, aAl, cv::COLOR_BGRA2BGR);
        else if (bAl.channels() == 4 && aAl.channels() == 3) cv::cvtColor(bAl, bAl, cv::COLOR_BGRA2BGR);

        if (aAl.size() != bAl.size()) {
            cv::resize(bAl, bAl, aAl.size(), 0, 0, cv::INTER_LINEAR);
        }

        if (aAl.empty() || bAl.empty() || aAl.size() != bAl.size() || aAl.channels() != bAl.channels()) {
            if (epoch != m_dataEpoch.load(std::memory_order_relaxed))
                return;   // 同上:会话已结束,不是真的失败
            const int fA = pairptr->idxA, fB = pairptr->idxB;
            handlePixelDiffFailure(pairptr,
                QStringLiteral("尺寸/类型不匹配 A(%1x%2 ch=%3 t=%4) B(%5x%6 ch=%7 t=%8) A=%9 B=%10")
                    .arg(aAl.cols).arg(aAl.rows).arg(aAl.channels()).arg(aAl.type())
                    .arg(bAl.cols).arg(bAl.rows).arg(bAl.channels()).arg(bAl.type())
                    .arg(fA >= 0 && fA < (int)m_files.size() ? m_files[fA].currentPath : QStringLiteral("?"))
                    .arg(fB >= 0 && fB < (int)m_files.size() ? m_files[fB].currentPath : QStringLiteral("?")));

            notifyPixelDiffUpdated(pairptr, epoch);

            tickPixelDiffProgress();
            return;
        }


        cv::Mat grayDiff;
        double pixDiffRatio = 1.0;
        double meanDiff = 1.0;

        try {
            std::tie(grayDiff, pixDiffRatio, meanDiff) = dedup::computePixelDiff(aAl, bAl, 0.0);
        } catch (...) {
            cv::Mat diff;
            cv::absdiff(aAl, bAl, diff);
            cv::cvtColor(diff, grayDiff, cv::COLOR_BGR2GRAY);
            cv::Mat th;
            cv::threshold(grayDiff, th, 16.0, 255, cv::THRESH_BINARY);
            pixDiffRatio = (double)cv::countNonZero(th) / (th.rows * th.cols);
            meanDiff = (double)cv::mean(grayDiff)[0] / 255.0;
        }

        /*
         * 诊断:差异几乎 100% 但两侧尺寸【完全相同】。
         *
         * 只讨论真正可疑的那一类:哈希说"这两张几乎一样"(pHash/dHash 都很近),
         * 像素却报 99.9% 不同 —— 那才可能是对齐/取图出错。
         *
         * 以前这里不带哈希条件,于是"相似但内容确实不同"的普通对(漫画相邻页、
         * 同一构图不同上色)也会被写成"结果异常",一次扫描刷出几百条,
         * 把真正的问题淹掉了。普通对降到 qDebug。
         */
        if (pixDiffRatio > 0.999 && aAl.size() == bAl.size()) {
            int pHashD = INT_MAX, dHashD = INT_MAX;
            {
                std::lock_guard<std::mutex> lkState(pairptr->stateMutex);
                pHashD = pairptr->pHashDistance;
                dHashD = pairptr->dHashDistance;
            }
            const bool hashSaysSame = (pHashD <= 2 && dHashD <= 2);

            const int fA = pairptr->idxA, fB = pairptr->idxB;
            const QString pathA = (fA >= 0 && fA < (int)m_files.size())
                                      ? m_files[fA].currentPath : QStringLiteral("?");
            const QString pathB = (fB >= 0 && fB < (int)m_files.size())
                                      ? m_files[fB].currentPath : QStringLiteral("?");
            if (hashSaysSame) {
                logLine(QString("[%1] 像素比较结果异常: 哈希一致(pHash=%2 dHash=%3)但像素 diff=%4,"
                                "同尺寸 %5x%6  A=%7  B=%8")
                            .arg(nowStr()).arg(pHashD).arg(dHashD)
                            .arg(pixDiffRatio, 0, 'f', 9)
                            .arg(aAl.cols).arg(aAl.rows)
                            .arg(pathA).arg(pathB), false, 3);
            } else {
                qDebug() << QStringLiteral("[pixelDiff] 同尺寸但差异接近 100%(哈希也不同,属正常):"
                                           " pair pHash=%1 dHash=%2 diff=%3")
                                .arg(pHashD).arg(dHashD).arg(pixDiffRatio, 0, 'f', 6);
            }
        }

        /*
         * 注意:这里绝对不要缓存 aAl/bAl 转出来的显示图!
         * 本函数会对所有pair跑一遍,每对缓存两张全分辨率图就是GB级内存。
         * 显示图由 displayPair 在需要显示时临时构造(只有当前这一对)。
         * aAl/bAl/grayDiff 都是局部变量,函数返回即释放。
         */
        {
            std::lock_guard<std::mutex> lk_pair(pairptr->stateMutex);
            pairptr->pixelRatioComputed = true;
            pairptr->pixelDiffRatio = pixDiffRatio;
        }

        // 第二层进度条:算完这一对pixel diff就推进一次
        tickPixelDiffProgress();

        // 把结果送回 UI:刷新列表那一行 + (若是当前显示的对)信息面板
        notifyPixelDiffUpdated(pairptr, epoch);
    }
    catch (const cv::Exception &e) {
        handlePixelDiffFailure(pairptr,
            QStringLiteral("cv::Exception: %1").arg(QString::fromUtf8(e.what())));
        notifyPixelDiffUpdated(pairptr, epoch);
        tickPixelDiffProgress();
    }
    catch (const std::exception &e) {
        handlePixelDiffFailure(pairptr,
            QStringLiteral("std::exception: %1").arg(QString::fromUtf8(e.what())));
        notifyPixelDiffUpdated(pairptr, epoch);
        tickPixelDiffProgress();
    }
    catch (...) {
        handlePixelDiffFailure(pairptr, QStringLiteral("未知异常"));
        notifyPixelDiffUpdated(pairptr, epoch);
        tickPixelDiffProgress();
    }
}

bool ImageDeduplicateWindow::handlePixelDiffFailure(std::shared_ptr<PairInfo> pairptr,
                                                   const QString &reason)
{
    if (!pairptr) return false;

    int fails = 0;
    bool latched = false;
    {
        std::lock_guard<std::mutex> lkState(pairptr->stateMutex);
        fails = ++pairptr->pixelDiffFailures;
        latched = (fails >= kPixelDiffMaxFailures);
        if (latched) {
            // 连续失败到上限:落成 1(=100% 差异)当哨兵,停止重试
            pairptr->pixelRatioComputed = true;
            pairptr->pixelDiffRatio = 1.0;
        } else {
            // 先不锁死,留给后面的重试(下次切到这一对 / 单线程补算)
            pairptr->pixelRatioComputed = false;
        }
    }

    /*
     * 级别随结果走:
     *   · 还没到重试上限 -> Detail(1):这是"稍后重试"的中间状态,不是故障;
     *   · 已达上限       -> Warning(3):这一对永远算不出来了,用户该知道。
     * 以前一律 Info,和"真正的故障"混在一起看不出轻重。
     */
    logLine(QString("[%1] 像素比较失败(第%2次): %3 —— %4")
                .arg(nowStr()).arg(fails).arg(reason)
                .arg(latched ? QStringLiteral("已达重试上限,标记为已计算(=1)")
                             : QStringLiteral("稍后会重试")),
            false, latched ? 3 : 1);
    return latched;
}

void ImageDeduplicateWindow::notifyPixelDiffUpdated(std::shared_ptr<PairInfo> pairptr, int epoch)
{
    if (!pairptr) return;

    /*
     * pixel diff 有了结果(算出来了 / 或者判定算不了)之后,把结果送回 UI。
     * 通过队列投递到 UI 线程,所以可以从任意后台线程调用。
     *
     * 做两件事:
     *   1) 刷新列表里那一行 —— 以前这里完全不碰列表,列表会一直停在
     *      "pix diff: computing...",要等下一次 rebuildPairList(切图/状态变化)
     *      才补上,看起来就像"结果没写回来";
     *   2) 如果这一对正在显示,刷新信息面板。
     */
    QMetaObject::invokeMethod(this, [this, pairptr, epoch]() {
        std::shared_lock<std::shared_mutex> lk(m_mutex);
        if (epoch != m_dataEpoch) return;
        /*
         * 登记"本线程已持有 m_mutex":下面 refreshPairListRow 里还有一道
         * IWD_WITH_MMUTEX_SHARED,不登记的话它会在已持共享锁的情况下再拿一次。
         */
        MMutexScope mScope(g_iwdMMutexDepth);

        const int pairIdx = static_cast<int>(
            std::find(m_pairs.begin(), m_pairs.end(), pairptr) - m_pairs.begin());
        if (pairIdx < 0 || pairIdx >= (int)m_pairs.size()) return;   // 已经不在列表里了

        refreshPairListRow(pairIdx);

        std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
        if (m_currentIndex >= 0 && m_currentIndex < (int)m_pairs.size()
            && m_pairs[m_currentIndex] == pairptr) {
            /*
             * 只读 FileEntry 字段,不碰 pair 的 stateMutex —— 这里已经持有
             * currentIndexMutex 和调用方的锁,再叠一把容易自己锁死自己。
             */
            updateInfoPanelForPair(pairptr, m_showingA);
        }
    }, Qt::QueuedConnection);
}

void ImageDeduplicateWindow::onDisplayImagePainted(quint64 serial)
{
    /*
     * 总是先刷新标题/背景色(它反映 A/B 谁在左,与"换没换图"无关)。
     */
    updateWindowTitleAndBackground(m_showingA);

    /*
     * 只有"我换上去的那一张"真的画出来了才算切换完成。
     *
     * 不能用"paint 来了就算" —— 角标重绘、局部刷新都会走到这里,
     * 那样会在新图还没到的时候就把按钮解锁、把"切换中"清掉,
     * 用户正好在这个空隙里对着滞留的上一对图按下 remove。
     * 序号对不上就直接返回:屏幕上画的还是旧图,保护继续有效。
     */
    if (m_awaitingPaintSerial == 0 || serial != m_awaitingPaintSerial)
        return;
    m_awaitingPaintSerial = 0;

    /*
     * 贴侧栏:文字是 displayPair 里持锁算好的,这里只 setText,
     * 绝不碰 m_pairs/m_files、也不拿 m_mutex(后台 pix diff/排序会长期持排他锁)。
     */
    if (m_pendingInfoValid) {
        applyInfoFields(m_pendingInfo);
        m_pendingInfoValid = false;
    }
    m_switchInfoPending = false;

    /*
     * 状态提示收尾:侧栏那行"切换中"清掉,角标换成常驻的 pair 号。
     * 常驻是有意的 —— 相邻两对图长得一样时,这是唯一能判断"切没切"的信号。
     *
     * 但"图片无法显示"这一行不能清:它说明的是当前这一对的状态,
     * 不是"正在切换"的临时提示。
     */
    if (!m_currentPairImageUnavailable && !m_currentPairImageLoading)
        setPairLoadingHint(QString());
    if (m_display && m_displayPairTotal > 0) {
        /*
         * 角标里带 A/B:这张图到底是 A 还是 B,以前只能靠标题栏或侧栏去猜;
         * 而相邻两对图长得一模一样时,标题栏的剩余数也不一定会变。
         * 让角标自己说清楚,配合 toggleAB() 里的即时更新,一眼可辨。
         *
         * 图片无法显示时(空白)必须额外写出来:否则一次空白图的 paint 会把角标
         * 刷成普通的 "A · pair N / M",看起来像"图是黑的",而不是"这一对没有图"。
         */
        QString badge = QStringLiteral("%1 · pair %2 / %3")
                            .arg(m_showingA ? QStringLiteral("A") : QStringLiteral("B"))
                            .arg(m_displayPairNumber).arg(m_displayPairTotal);
        if (m_currentPairImageUnavailable)
            badge += QStringLiteral(" · 图片无法显示");
        else if (m_currentPairImageLoading)
            badge += QStringLiteral(" · 加载中");
        m_display->setStatusBadge(badge,
                                  /*busy=*/m_currentPairImageUnavailable || m_currentPairImageLoading);
    }

    setPairActionButtonsEnabled(true);
}

void ImageDeduplicateWindow::updateWindowTitleAndBackground(bool showingAisLeft)
{
    /*
     * 筛选模式下标题归 updateFilterUi() 管(FILTER ...):
     * 否则每画完一张图就被这里的 "A <剩余总数>" 覆盖掉,
     * 用户就看不出自己正处在筛选里了。
     */
    if (m_filter.active) {
        updateFilterUi();
    } else {
        int remaining = (int)m_pairs.size() - (m_currentIndex + 1);
        QString which = showingAisLeft ? "A" : "B";
        setWindowTitle(QString("%1 %2").arg(which).arg(remaining));
    }

    /*
     * A/B 侧提示色走 QSS 的属性筛选器(见 .ui 里的 QWidget#ImageDeduplicateWindow[abSide="..."]）。
     * 原来这里是 setStyleSheet("background-color: ...") —— setStyleSheet 是「整张替换」而不是追加，
     * 一进来就把 .ui 上那份 QSS（含 QPushButton 的配色）整个冲掉，按钮于是退回系统原样，
     * 这也是"把按钮 QSS 挪到 midWidget 上才管用"的原因。
     *
     * 动态属性变了 QSS 不会自动重算匹配，必须 unpolish/polish；
     * 显示控件是自绘的（paintEvent 拿 palette().window() 填底），只吃挂在它自己身上的那条规则，
     * 所以它得跟着一起 repolish。
     * 这里每次图片画完都会被调用，属性没变就直接返回，避免反复重算（实测整轮 9.1ms -> 1.6ms）。
     */
    const QString side = showingAisLeft ? QStringLiteral("A") : QStringLiteral("B");
    if (property("abSide").toString() == side) return;

    setProperty("abSide", side);
    this->style()->unpolish(this);
    this->style()->polish(this);
    this->update();
}

/*
 * 把"这一对的侧栏文字"算出来(不发 UI 调用)。
 * 必须在持有 m_mutex 的上下文里调用 —— 它读 m_files。
 * 字段顺序固定,和 applyInfoFields 里的 label 数组一一对应。
 */
QVector<ImageDeduplicateWindow::InfoField>
ImageDeduplicateWindow::makeInfoFields(const std::shared_ptr<PairInfo> &p, bool showingAisLeft) const
{
    QVector<InfoField> f;
    if (!p) return f;
    if (p->idxA < 0 || p->idxA >= (int)m_files.size()) return f;
    if (p->idxB < 0 || p->idxB >= (int)m_files.size()) return f;

    const FileEntry &fa = m_files[p->idxA];
    const FileEntry &fb = m_files[p->idxB];

    const FileEntry *left = showingAisLeft ? &fa : &fb;
    const FileEntry *right = showingAisLeft ? &fb : &fa;

    auto add = [&f](const QString &text, const QString &color = QString()) {
        f.push_back({text, color});
    };

    /* 0/1: 路径(不带颜色规则,沿用 QSS) */
    add(fa.currentPath);
    add(fb.currentPath);

    /* 2: 尺寸比较 */
    const long leftArea = (long)left->size.width() * left->size.height();
    const long rightArea = (long)right->size.width() * right->size.height();
    const QString sizeLeft = QString("%1x%2").arg(left->size.width()).arg(left->size.height());
    const QString sizeRight = QString("%1x%2").arg(right->size.width()).arg(right->size.height());
    const double sizeLeftDouble = left->size.width() * left->size.height();
    const double sizeRightDouble = right->size.width() * right->size.height();
    const QString cmp = (leftArea == rightArea) ? QStringLiteral("=")
                                                : ((leftArea > rightArea) ? QStringLiteral(">")
                                                                          : QStringLiteral("<"));
    const QString sizeColor = (cmp == QStringLiteral("=")) ? QStringLiteral("white")
                                                           : ((cmp == QStringLiteral(">")) ? QStringLiteral("green")
                                                                                           : QStringLiteral("red"));
    add(QString("Size: %1 %2 %3 (%4%)").arg(sizeLeft).arg(cmp).arg(sizeRight)
            .arg(sizeRightDouble != 0.0 ? sizeLeftDouble / sizeRightDouble * 100 : 0.0),
        sizeColor);

    /* 3: 格式/质量 */
    const int frLeft  = qualityRank(*left);
    const int frRight = qualityRank(*right);
    const QString fmtColor = (frLeft > frRight) ? QStringLiteral("green")
                                                : ((frLeft < frRight) ? QStringLiteral("red")
                                                                      : QStringLiteral("white"));
    /* 括号里是 qualityRank:JPEG 用 DQT 估计值,无损格式固定 101,无法估计时为 -1 */
    add(QString("Format: %1(%2) vs %3(%4)").arg(left->format).arg(frLeft)
            .arg(right->format).arg(frRight),
        fmtColor);

    /* 4: 文件大小 */
    add(QString("File size: %1 KB vs %2 KB (%3%)").arg(left->filesize / 1024)
            .arg(right->filesize / 1024)
            .arg(right->filesize != 0 ? 100.0 * (double)left->filesize / (double)right->filesize : 0.0),
        (left->filesize == right->filesize) ? QStringLiteral("white")
                                            : (left->filesize > right->filesize ? QStringLiteral("green")
                                                                                : QStringLiteral("red")));

    /* 5/6: 哈希距离 */
    add(QString("pHash distance: %1").arg(p->pHashDistance), QStringLiteral("white"));
    add(QString("d: %1").arg(p->dHashDistance), QStringLiteral("white"));

    /* 7: 像素差异 */
    if (p->pixelRatioComputed) {
        add(QString("Pixel diff ratio: %1").arg(p->pixelDiffRatio, 0, 'f', 8),
            (p->pixelDiffRatio <= m_pixdiffThreshold) ? QStringLiteral("green")
                                                      : QStringLiteral("yellow"));
    } else {
        add(QStringLiteral("Pixel diff: computing..."), QStringLiteral("white"));
    }

    return f;
}

void ImageDeduplicateWindow::applyInfoFields(const QVector<InfoField> &fields)
{
    QLabel *labels[] = {ui.lb_path_a, ui.lb_path_b, ui.lb_pixel, ui.lb_fmt,
                        ui.lb_size, ui.lb_phash, ui.lb_dhash, ui.lb_pix_diff};
    const int n = int(sizeof(labels) / sizeof(labels[0]));

    for (int i = 0; i < n && i < fields.size(); ++i) {
        QLabel *l = labels[i];
        if (!l) continue;
        const InfoField &f = fields[i];
        if (!f.color.isEmpty())
            l->setStyleSheet(QString("color:%1").arg(f.color));
        l->setText(f.text);
    }
    if (ui.info_widget) ui.info_widget->update();
}

void ImageDeduplicateWindow::updateInfoPanelForPair(std::shared_ptr<PairInfo> p, bool showingAisLeft)
{
    /*
     * 这一对的下标可能已经是旧的(排序刚把 m_pairs 换掉 / 数据换代),
     * makeInfoFields 里做了越界检查:宁可整块不填,也不能崩。
     */
    const QVector<InfoField> fields = makeInfoFields(p, showingAisLeft);
    if (fields.isEmpty()) return;
    applyInfoFields(fields);
}

void ImageDeduplicateWindow::clearSideInfoForLoading()
{
    /*
     * 只清"描述这一对图片"的字段,不动 label / label_2 这种静态标题。
     * 屏幕上此刻还是上一对的图:如果侧栏继续写着数字,用户根本没有线索
     * 判断"现在看到的图"和"侧栏的数字"不是同一对。
     */
    QLabel *labels[] = {ui.lb_path_a, ui.lb_path_b, ui.lb_phash, ui.lb_dhash,
                        ui.lb_pixel, ui.lb_fmt, ui.lb_size, ui.lb_pix_diff,
                        ui.lb_diff_rate};
    for (QLabel *l : labels) {
        if (l) l->clear();
    }
    /*
     * 同时作废"算好但还没贴"的那批文字。
     * 不清的话,下一次 paint(可能只是窗口重绘)会把上一对/还没显示的那一对的数字
     * 贴到刚清空的框里 —— 又变成"屏幕和侧栏不是同一对"。
     */
    m_pendingInfoValid = false;
    m_pendingInfo.clear();
    if (ui.info_widget) ui.info_widget->update();
}

void ImageDeduplicateWindow::setPairLoadingHint(const QString &text)
{
    if (!ui.lb_diff_rate) return;

    if (text.isEmpty()) {
        ui.lb_diff_rate->setStyleSheet(QStringLiteral("color: rgb(255, 255, 255);"));
        ui.lb_diff_rate->clear();
    } else {
        /* 高亮(琥珀色):一眼能看出"这不是数据,是状态" */
        ui.lb_diff_rate->setStyleSheet(QStringLiteral("color: rgb(255, 200, 90); font-weight: 600;"));
        ui.lb_diff_rate->setText(text);
    }
    if (ui.info_widget) ui.info_widget->update();
}

void ImageDeduplicateWindow::beginPairSwitchHint(int pairIndex)
{
    m_displayPairNumber = pairIndex + 1;
    m_displayPairTotal  = (int)m_pairs.size();

    const QString pos = QStringLiteral("pair %1 / %2").arg(m_displayPairNumber).arg(m_displayPairTotal);
    setPairLoadingHint(QStringLiteral("切换中: ") + pos);
    if (m_display)
        m_display->setStatusBadge(QStringLiteral("%1 · 切换中 · %2")
                                      .arg(m_showingA ? QStringLiteral("A")
                                                      : QStringLiteral("B"), pos),
                                  /*busy=*/true);
}

void ImageDeduplicateWindow::setPairActionButtonsEnabled(bool on)
{
    QPushButton *btns[] = {ui.btn_rm_a, ui.btn_rm_b, ui.btn_rm_both, ui.btn_jmp,
                           ui.btn_previous, ui.btn_next, ui.btn_auto_rm};

    if (!on) {
        if (m_pairActionButtonsLocked) return;   // 已经锁着:别把"锁后状态"当成基线
        m_pairActionButtonsLocked = true;
        m_pairActionButtonsPrev.clear();
        for (QPushButton *b : btns) {
            if (!b) continue;
            m_pairActionButtonsPrev.push_back({QPointer<QPushButton>(b), b->isEnabled()});
            b->setEnabled(false);
        }
        return;
    }

    if (!m_pairActionButtonsLocked) return;
    m_pairActionButtonsLocked = false;
    for (const auto &e : m_pairActionButtonsPrev) {
        if (e.first) e.first->setEnabled(e.second);   // 恢复锁定前的状态(筛选态可能本来就是灰的)
    }
    m_pairActionButtonsPrev.clear();
}

void ImageDeduplicateWindow::toggleAB()
{
    if (!m_display) return;

    /*
     * "切换中"(新图还没上屏)期间禁止 A/B 切换:
     * 此时屏幕上还是上一对的图,切换没有意义;更关键的是 setPixmap 会自增
     * 控件的换图序号,而 m_awaitingPaintSerial 刻意不跟着改(见下面注释) ——
     * 之后任何一次 paint 都匹配不上,保护要么等 3 秒兜底强行收尾,
     * 要么一直卡到 8 秒超时。等上屏后用户再按,效果完全一样。
     */
    if (m_pairActionButtonsLocked) return;

    auto vp = m_display->getViewport();
    m_showingA = !m_showingA;
    if (m_showingA) m_display->setPixmap(m_pixA);
    else m_display->setPixmap(m_pixB);
    m_display->setViewport(vp);

    /*
     * 左下角角标同步换成新的 A/B。
     * 这是"同一对内部换左右",不经过 onDisplayImagePainted 那条收尾路径,
     * 不在这里改的话角标会一直停在切换前的那一侧。
     * busy 沿用当前状态:还在等上屏的时候不要把它降级成常驻色。
     */
    if (m_displayPairTotal > 0) {
        QString badge = QStringLiteral("%1 · pair %2 / %3")
                            .arg(m_showingA ? QStringLiteral("A") : QStringLiteral("B"))
                            .arg(m_displayPairNumber).arg(m_displayPairTotal);
        if (m_currentPairImageUnavailable)
            badge += QStringLiteral(" · 图片无法显示");
        else if (m_currentPairImageLoading)
            badge += QStringLiteral(" · 加载中");
        m_display->setStatusBadge(badge,
                                  /*busy=*/m_currentPairImageUnavailable
                                      || m_currentPairImageLoading
                                      || m_awaitingPaintSerial != 0);
    }
    /*
     * 刻意【不】更新 m_awaitingPaintSerial:这是同一对内部换左右,
     * 如果让它算"新图已上屏",就会把还没完成的那次切图的保护提前解掉。
     */
    if (m_currentIndex >= 0 && m_currentIndex < m_pairs.size()) {
        /* 同样不能在外部持 stateMutex,理由见上面那处 */
        updateInfoPanelForPair(m_pairs[m_currentIndex], m_showingA);
        /*
         * 侧栏文字是"按哪一侧在左"算出来的。A/B 一换,之前为"等上屏"准备好的
         * 那批文字就过期了 —— 不重算的话,接下来的 paint 会用旧的左右关系贴上去。
         */
        if (m_switchInfoPending) {
            m_pendingInfo = makeInfoFields(m_pairs[m_currentIndex], m_showingA);
            m_pendingInfoValid = !m_pendingInfo.isEmpty();
        }
    }
}

void ImageDeduplicateWindow::onDisplayLeftClicked()
{
    // 如果按下 Ctrl，则进行快速删除（与按钮行为一致），否则做默认切换（toggle）
    if (QApplication::keyboardModifiers() & Qt::ControlModifier) {
        /*
         * 这是一条"直接改状态/搬文件"的入口,所以必须和按钮走同一套闸:
         *   · 显示图还没上屏(按钮锁着)-> 屏幕上还是上一对,删了就是错的;
         *   · 目录方向未定(FolderCompare 开着)-> A/B 谁在左都没定,不能动文件;
         *   · 筛选状态下语义不同(只删另一侧),交给 filterApplyAction。
         */
        if (m_pairActionButtonsLocked) {
            createFloatingLabel(this, QStringLiteral("图片还没上屏,等这一对显示出来再操作"));
            return;
        }
        if (blockedByFolderDecision(QStringLiteral("Ctrl+点击")))
            return;
        if (m_filter.active) {
            // 筛选里锚点永远钉在 A 位,"删另一侧"就是 RemoveB
            filterApplyAction(ActionRecord::RemoveB);
            return;
        }

        std::shared_lock<std::shared_mutex> lk(m_mutex);
        std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
        if (m_currentIndex < 0 || m_currentIndex >= (int)m_pairs.size()) return;
        shared_ptr<PairInfo> pairPtr = m_pairs[m_currentIndex];

        // 和按钮/快捷键一致:上一次操作还在等落盘时不要再动同一个文件
        if (m_awaitFileOpPair) {
            if (m_pairFileOpsInFlight.value(m_awaitFileOpPair.get(), 0) <= 0)
                m_awaitFileOpPair.reset();
            else if (pairPtr == m_awaitFileOpPair) {
                createFloatingLabel(this, QStringLiteral("这一对还在等磁盘操作结果,稍等一下"));
                logLine(QString("[%1] 忽略 Ctrl+点击:上一次操作还在等落盘").arg(nowStr()));
                return;
            }
        }
        // 如果当前显示 A（左），则 Ctrl+点击 -> 删除 B；否则删除 A
        ActionRecord::Type type = m_showingA ? ActionRecord::RemoveB : ActionRecord::RemoveA;

        {
            std::lock_guard<std::mutex> lkState(pairPtr->stateMutex);
            applyActionToPair(pairPtr, type);
        }
        /*
         * Ctrl+左键也是"一次单对处理",和按钮/快捷键同一个 pop。
         * 之前这里漏了 —— 按 a/s/d 有声音、Ctrl+点击同一件事却静音,像是没生效。
         * (筛选分支在上面交给 filterApplyAction,它自己会播。)
         */
        dedupenv::playSound(DedupSound::SingleAction);
        // 立即刷新被操作那一行,再整表重建(连带Invalid的行也要跟着变);
        // ScrollNever:滚动交给下面的 displayPairByIndex(nextIndex)
        refreshPairListRow(m_currentIndex);
        finalizePairStateChange(DuplicatePairListView::ScrollNever);

        logLine(QString("[%1] Ctrl+click performed remove (%2)").arg(nowStr()).arg((type==ActionRecord::RemoveA) ? "RemoveA" : "RemoveB"));

        /*
         * 和 a/s/d/按钮同一套:这一步真的入队了 rename 就先别前进。
         * 成功 -> handleFileOpResult 里前进;失败 -> 状态回滚、留在这一对并提示重试/跳过。
         */
        if (holdAdvanceUntilFileOpsSettle(pairPtr)) {
            logLine(QString("[%1] Ctrl+click: pair %2 已提交磁盘操作,等落盘后再前进")
                        .arg(nowStr()).arg(m_currentIndex));
            return;
        }

        int nextIndex = findNextPending();

        if (nextIndex >= 0 && nextIndex<m_pairs.size()) {
            displayPairByIndex(nextIndex,m_dataEpoch);
        } else {
            // 没有更多 pending pairs
            showNoMorePairs();
        }

    } else {
        std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
        // 非 Ctrl 的左键：切换显示
        toggleAB();
    }
}


void ImageDeduplicateWindow::onDisplayRightClicked()
{
    toggleAB();
}

void ImageDeduplicateWindow::applyActionToPair(shared_ptr<PairInfo> pairptr, ActionRecord::Type type,
                                               QVector<int>* changedRows)
{
    if (!pairptr) return;

    const int idxA = pairptr->idxA;
    const int idxB = pairptr->idxB;

    /*
     * 越界检查:这个函数会在批量循环里被调上千次,一旦某个 pair 的 idx 因为
     * 数据换代(stale epoch)落在 m_files 之外,下面的 m_files[idxA] 在 Debug 构建
     * 里就是 "vector subscript out of range" -> abort()。
     * 正常流程不会出现,但 crash 的代价远高于这里多两次比较。
     */
    if (idxA < 0 || idxA >= (int)m_files.size()
        || idxB < 0 || idxB >= (int)m_files.size()) {
        logLine(QString("[%1] applyActionToPair 跳过: 文件下标越界 idxA=%2 idxB=%3 files=%4")
                    .arg(nowStr()).arg(idxA).arg(idxB).arg(m_files.size()), true, 3);
        return;
    }

    FileEntry &A = m_files[idxA];
    FileEntry &B = m_files[idxB];
    const PairState prevState = pairptr->state;

    /*
     * 打上"第几次动作"的时间戳。
     * 导入整理在窗口关闭后统一生成文件操作计划,一个文件可能同时被两个 pair
     * 判成互相冲突的操作(见 PairInfo::decisionSeq 的说明),到时候就靠这个序号
     * 按用户最后的意图裁决。普通去重模式不用它,写一下也无副作用(atomic)。
     */
    if (type != ActionRecord::None)
        pairptr->decisionSeq = ++m_decisionSeq;

    /*
     * 恢复一个文件(把之前搬到 duplicate 的搬回来)。
     * optimisticState 传 -1:这里的 pair 状态不是由恢复决定的 ——
     * 调用方的 switch 在之后才写最终状态,恢复失败不该回滚到某个中间态;
     * 失败的文件级回滚 + Invalid 标记修正由 handleFileOpResult 负责。
     */
    auto restoreIfNeeded = [&](int fileIdx, FileEntry& file) {
        if (!file.inDuplicate) return;
        if (restoreFromDuplicate(fileIdx, pairptr))
            markRelatedPairsValidState(file, pairptr, true, changedRows);
    };

    /*
     * 把一个文件移走。
     *
     * 两类"看起来没搬但其实已经搬过"的情况由 moveToDuplicate 内部处理:
     *   1) 文件已经在 duplicate 里了(FileEntry::inDuplicate 为真 —— 包括"已排队
     *      要搬进去"但物理上还没搬的,因为 FileEntry 是乐观更新的);
     *   2) currentPath 指向的位置已经没有文件了 —— 别处已经搬走,或者用户手工删了。
     * 这两种都必须让状态成功落下去,否则会出现"点了 Remove B 但那一对还是 Pending、
     * 而图片已经被移走"的僵死状态。
     *
     * 物理 rename 现在是后台串行执行的,这里传下去的乐观状态/回滚状态只用于
     * "移动真的失败"时把 pair 状态还原。
     */
    auto removeFile = [&](int fileIdx, int optimisticState) -> bool {
        return moveToDuplicate(fileIdx, pairptr, optimisticState, (int)prevState);
    };

    switch (type) {
    case ActionRecord::RemoveA: {
        // 如果之前移除了 B，先恢复 B
        if (prevState == PairState::RemovedB ||
            prevState == PairState::RemovedBoth)
        {
            restoreIfNeeded(idxB, B);
        }

        // 移除 A
        if (removeFile(idxA, (int)PairState::RemovedA)) {
            pairptr->state = PairState::RemovedA;
            markRelatedPairsValidState(A, pairptr, false, changedRows);
        }
        break;
    }

    case ActionRecord::RemoveB: {
        // 如果之前移除了 A，先恢复 A
        if (prevState == PairState::RemovedA ||
            prevState == PairState::RemovedBoth)
        {
            restoreIfNeeded(idxA, A);
        }

        // 移除 B
        if (removeFile(idxB, (int)PairState::RemovedB)) {
            pairptr->state = PairState::RemovedB;
            // 检查 A 是否还在其他 pair 中被引用，如果是则标记为 InvalidA
            markRelatedPairsValidState(B, pairptr, false, changedRows);
        }
        break;
    }

    case ActionRecord::RemoveBoth: {
        // 移除 A
        removeFile(idxA, (int)PairState::RemovedBoth);
        markRelatedPairsValidState(A, pairptr, false, changedRows);

        // 移除 B
        removeFile(idxB, (int)PairState::RemovedBoth);
        markRelatedPairsValidState(B, pairptr, false, changedRows);

        pairptr->state = PairState::RemovedBoth;
        break;
    }

    case ActionRecord::Skip: {
        pairptr->state = PairState::Skipped;
        restoreIfNeeded(idxA, A);
        restoreIfNeeded(idxB, B);
        break;
    }

    default:
        break;
    }
}

void ImageDeduplicateWindow::markRelatedPairsValidState(const FileEntry& file,shared_ptr<PairInfo> currentPair, bool valid, QVector<int>* changedRows)
{
    // 找到“当前pair”的下标,用于记录Invalid的原因(点击Invalid项时可以跳过去)
    int causeIndex = -1;
    for (int i = 0; i < (int)m_pairs.size(); ++i) {
        if (m_pairs[i] == currentPair) { causeIndex = i; break; }
    }

    for (int i = 0; i < (int)m_pairs.size(); ++i) {
        shared_ptr<PairInfo> pair = m_pairs[i];
        // 跳过当前 pair
        if (!pair || pair == currentPair) continue;

        // 越界保护:见 applyActionToPair 里的同一处说明
        if (pair->idxA < 0 || pair->idxA >= (int)m_files.size()
            || pair->idxB < 0 || pair->idxB >= (int)m_files.size())
            continue;

        std::lock_guard<std::mutex> lkState(pair->stateMutex);
        FileEntry& pairFileA = m_files[pair->idxA];
        FileEntry& pairFileB = m_files[pair->idxB];

        /*
         * 记录"这一行真的变了"。
         * 调用方据此只刷新这几行,而不是整表重建 —— 35000 对的会话里
         * 整表重建是每点一次按钮都要付的 O(N) 成本。
         */
        const PairState before = pair->state;
        /*
         * 叠加位也要比:失效这一下对 Skipped / Removed* 的对【不改 state】,
         * 只置 invalidMask。如果这里只比 state,这些行就不会进 changedRows,
         * 列表仍显示 "Skipped",看不出 "Skipped + InvalidB" —— 用户报的
         * "复合状态没显示出来"就是这个。
         */
        const quint8 beforeMask = pair->invalidMask;

        if(!valid){
            /*
             * 失效:记"哪一侧没了"(invalidMask,对所有 state 都记 —— 见
             * pairstate_math.h 里的说明),Pending 同时降级成 Invalid*,
             * 这样点击时能跳回肇事的那一对。
             * 已经被判成 Skipped / Removed* 的对【不】改 state:
             * 它的用户决定还在,只是"这一侧没了"这件事得另外记住。
             */
            if (&pairFileA == &file) {
                pair->invalidMask = quint8(pair->invalidMask | SideBitA);
                if (pair->state == PairState::Pending) {
                    pair->state = PairState::InvalidA;
                }
                if (pair->state == PairState::InvalidB) {
                    pair->state = PairState::InvalidBoth;
                }
                if (causeIndex >= 0)
                    pair->invalidCauseForA = causeIndex + 1;   // 存下标+1,0表示无
            }
            else if(&pairFileB == &file){
                pair->invalidMask = quint8(pair->invalidMask | SideBitB);
                if (pair->state == PairState::Pending) {
                    pair->state = PairState::InvalidB;
                }
                if (pair->state == PairState::InvalidA) {
                    pair->state = PairState::InvalidBoth;
                }
                if (causeIndex >= 0)
                    pair->invalidCauseForB = causeIndex + 1;
            }
        }
        else if(valid){
            // 文件被恢复了:清掉失效位,并把Invalid状态降级/还原
            if (&pairFileA == &file) {
                pair->invalidMask = quint8(pair->invalidMask & ~SideBitA);
                if (pair->state == PairState::InvalidBoth) {
                    pair->state = PairState::InvalidB;
                }
                if (pair->state == PairState::InvalidA) {
                    promoteInvalidState(pair->state, pair->removedMask);
                }
                pair->invalidCauseForA = 0;
            }
            else if(&pairFileB == &file){
                pair->invalidMask = quint8(pair->invalidMask & ~SideBitB);
                if (pair->state == PairState::InvalidBoth) {
                    pair->state = PairState::InvalidA;
                }
                if (pair->state == PairState::InvalidB) {
                    promoteInvalidState(pair->state, pair->removedMask);
                }
                pair->invalidCauseForB = 0;
            }
        }

        if (changedRows && (pair->state != before || pair->invalidMask != beforeMask))
            changedRows->push_back(i);
    }
}

/* ==================================================================
 *                     文件移动异步化(后台串行执行)
 * ================================================================== */

namespace {

/*
 * 带退避重试的 rename。
 *
 * Windows 上这类失败绝大多数是共享冲突(ERROR_SHARING_VIOLATION=32):
 * 显示图/缩略图/预取线程正打开着这张图,而 Qt 的 QFile 打开时不带
 * FILE_SHARE_DELETE,MoveFile 就一定失败。但读图是短命的,
 * 退避几十毫秒再试通常就过了。
 *
 * 只有"被占用"这类瞬时错误才重试;路径不存在/磁盘满/无权限重试没意义。
 */
bool renameWithRetryOnSharing(const QString &from, const QString &to, DWORD *lastErrOut)
{
    DWORD lastErr = 0;
    const int delaysMs[] = {0, 50, 150, 400};
    const int attempts = int(sizeof(delaysMs) / sizeof(delaysMs[0]));

    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (attempt > 0)
            QThread::msleep(static_cast<unsigned long>(delaysMs[attempt]));
        if (QFile::rename(from, to)) {
            if (lastErrOut) *lastErrOut = 0;
            return true;
        }

        lastErr = ::GetLastError();
        if (lastErr != ERROR_SHARING_VIOLATION
            && lastErr != ERROR_ACCESS_DENIED
            && lastErr != ERROR_LOCK_VIOLATION) {
            break;
        }
    }

    if (lastErrOut) *lastErrOut = lastErr;
    return false;
}

/*
 * 把一次 rename 失败翻译成人能看懂的原因。
 *
 * 为什么要单独做:QFile::rename 失败时 Qt 只给一个布尔值,以前这里写死
 * "移动失败: from -> to",于是"为什么失败"永远是谜(2026-11-20 用户就是
 * 拿着这条日志来问的)。这里补系统错误码,再探一次源文件能不能打开 ——
 * 打不开基本就是被别的线程占着(显示图/缩略图正在读)。
 */
QString describeRenameFailure(const QString &from, DWORD lastErr)
{
    QString detail;
    if (lastErr != 0) {
        detail = QStringLiteral("系统错误 %1: %2")
                     .arg(lastErr)
                     .arg(qt_error_string(int(lastErr)));
    } else {
        detail = QStringLiteral("没有拿到系统错误码");
    }

    QFile probe(from);
    if (!probe.open(QIODevice::ReadOnly)) {
        detail += QStringLiteral(";源文件现在打不开(%1)").arg(probe.errorString());
    } else {
        probe.close();
        detail += QStringLiteral(";源文件现在可读(失败可能是瞬时的)");
    }
    return detail;
}

/*
 * 在磁盘上真正执行一次"移到 duplicate"。
 * 只碰字符串和文件系统,不碰任何成员 —— 这样它可以在文件操作线程上安全地跑。
 */
bool performMoveToDuplicateOnDisk(const QString &from, const QString &to, QString &err)
{
    if (!QFile::exists(from)) {
        err = QStringLiteral("源文件不存在(可能已被移动或删除): %1").arg(from);
        return false;
    }

    QFileInfo toInfo(to);
    QDir dir;
    const QString toDir = toInfo.absolutePath();
    if (!dir.exists(toDir)) {
        if (!dir.mkpath(toDir)) {
            err = QStringLiteral("无法创建目录: %1").arg(toDir);
            return false;
        }
    }

    /*
     * 重试见 renameWithRetryOnSharing:失败一次就回滚会让"点了 remove B
     * 却没搬走"变成常态(2026-11-20 的现场)。
     */
    DWORD lastErr = 0;
    if (!renameWithRetryOnSharing(from, to, &lastErr)) {
        err = QStringLiteral("移动失败: %1 -> %2(%3)")
                  .arg(from, to, describeRenameFailure(from, lastErr));
        return false;
    }
    return true;
}

/*
 * 在磁盘上真正执行一次"从 duplicate 恢复"。
 * 目标位置已有文件时先给它改名让位(和原来的同步实现行为完全一致),
 * 这些改名只是日志,通过 notes 带回 UI 线程打印。
 */
bool performRestoreFromDuplicateOnDisk(const QString &from, const QString &to,
                                       QString &err, QStringList &notes)
{
    if (!QFile::exists(from)) {
        err = QStringLiteral("duplicate 中的文件不存在(可能已被移动或删除): %1").arg(from);
        return false;
    }

    QFileInfo toInfo(to);
    QDir dir;
    const QString toDir = toInfo.dir().absolutePath();
    if (!dir.exists(toDir)) {
        if (!dir.mkpath(toDir)) {
            err = QStringLiteral("无法创建目录: %1").arg(toDir);
            return false;
        }
    }

    // 目标位置已经有文件:先把它改成 filename_1.ext 让位
    if (QFile::exists(to)) {
        const QString baseName = toInfo.baseName();
        const QString extension = toInfo.suffix();
        const QString pathWithoutExt = toInfo.dir().absolutePath() + "/" + baseName;

        int counter = 1;
        QString newPath;
        do {
            newPath = QString("%1_%2").arg(pathWithoutExt).arg(counter);
            if (!extension.isEmpty())
                newPath += "." + extension;
            counter++;
        } while (QFile::exists(newPath) && counter < 16384); // 防止无限循环

        if (counter >= 16384) {
            err = QStringLiteral("冲突文件太多,无法恢复: %1").arg(to);
            return false;
        }

        DWORD lastErr = 0;
        if (!renameWithRetryOnSharing(to, newPath, &lastErr)) {
            err = QStringLiteral("让位改名失败: %1 -> %2(%3)")
                      .arg(to, newPath, describeRenameFailure(to, lastErr));
            return false;
        }
        notes << QStringLiteral("Renamed existing file to avoid conflict: %1 → %2").arg(to, newPath);
    }

    DWORD lastErr = 0;
    if (!renameWithRetryOnSharing(from, to, &lastErr)) {
        err = QStringLiteral("恢复失败: %1 -> %2(%3)")
                  .arg(from, to, describeRenameFailure(from, lastErr));
        return false;
    }
    return true;
}

} // namespace

bool ImageDeduplicateWindow::hasPendingFileOp(int fileIdx) const
{
    if (fileIdx < 0) return false;
    std::lock_guard<std::mutex> lk(m_fileOpMutex);
    return m_fileOpInFlight.count(fileIdx) > 0;
}

void ImageDeduplicateWindow::enqueueFileOp(const PendingFileOp &op)
{
    /*
     * 记下"这一对还有一个操作没落定"。
     * 和队列本身分开记:队列只关心文件/路径,而"要不要前进"是按 pair 算的
     * (remove both 会给同一对入队两次,必须两个都回来才算完)。
     * 只在 UI 线程读写,不需要额外加锁。
     */
    if (op.pair)
        ++m_pairFileOpsInFlight[op.pair.get()];

    bool needStart = false;
    {
        std::lock_guard<std::mutex> lk(m_fileOpMutex);
        m_fileOpQueue.push_back(op);
        m_fileOpInFlight.insert(op.fileIdx);
        if (!op.to.isEmpty())
            m_fileOpReservedTargets.insert(op.to);
        if (!m_fileOpWorkerActive) {
            m_fileOpWorkerActive = true;
            needStart = true;
        }
    }

    if (!needStart) return;   // 已经有一个执行线程在消耗队列了(单线程池,串行)

    // 用 FunctionRunnable 而不是 QThreadPool::start(lambda):和文件里其它派发方式保持一致
    QPointer<ImageDeduplicateWindow> self(this);
    m_fileOpPool->start(new FunctionRunnable([self]() {
        if (!self) return;
        try {
            self->executeFileOps();
        } catch (...) {
            /*
             * executeFileOps 内部已经包住了磁盘操作;能走到这里说明是队列段抛的。
             * 必须把"执行者"标记复位,否则后续入队的文件操作永远没人消费。
             */
            {
                std::lock_guard<std::mutex> lk(self->m_fileOpMutex);
                self->m_fileOpWorkerActive = false;
            }
            recordTaskException("executeFileOps", "unhandled exception");
        }
    }));
}

void ImageDeduplicateWindow::executeFileOps()
{
    for (;;) {
        PendingFileOp op;
        {
            std::lock_guard<std::mutex> lk(m_fileOpMutex);
            if (m_fileOpQueue.empty()) {
                /*
                 * 队列空了,交出"执行者"身份。
                 * 注意这里是持锁判断+清标记的:如果刚清完就有人入队,
                 * 那个 enqueueFileOp 看到 active==false 会再起一个执行线程,
                 * 不会出现"有活没人干"。
                 */
                m_fileOpWorkerActive = false;
                return;
            }
            op = m_fileOpQueue.front();
            m_fileOpQueue.pop_front();
        }

        bool ok = false;
        QString err;
        QStringList notes;

        try {
            if (op.restore)
                ok = performRestoreFromDuplicateOnDisk(op.from, op.to, err, notes);
            else
                ok = performMoveToDuplicateOnDisk(op.from, op.to, err);
        } catch (...) {
            ok = false;
            err = QStringLiteral("执行文件操作时发生未知异常");
        }

        // 结果(成功与否)回投 UI 线程:回滚和日志都必须在 UI 线程做
        QPointer<ImageDeduplicateWindow> self(this);
        QMetaObject::invokeMethod(this, [self, op, ok, err, notes]() {
            if (!self) return;
            self->handleFileOpResult(op, ok, err, notes);
        }, Qt::QueuedConnection);
    }
}

void ImageDeduplicateWindow::handleFileOpResult(const PendingFileOp &op, bool ok,
                                                const QString &err, const QStringList &notes)
{
    // 先清在途标记/目标路径预定,无论成败、无论是否过期
    {
        std::lock_guard<std::mutex> lk(m_fileOpMutex);
        m_fileOpInFlight.erase(op.fileIdx);
        if (!op.to.isEmpty())
            m_fileOpReservedTargets.erase(op.to);
    }

    for (const QString &n : std::as_const(notes))
        logLine(QString("[%1] %2").arg(nowStr()).arg(n));

    /*
     * ===== 这一对的操作全落定了吗 =====
     *
     * remove both 会给同一对入队两个 rename,必须两个都回来才谈得上"前进",
     * 否则会先按第一个成功前进、第二个失败却已经走远 —— 又是同一个病。
     */
    int leftForPair = -1;
    if (op.pair) {
        auto it = m_pairFileOpsInFlight.find(op.pair.get());
        if (it != m_pairFileOpsInFlight.end()) {
            if (--it.value() <= 0) {
                m_pairFileOpsInFlight.erase(it);
                leftForPair = 0;
            } else {
                leftForPair = it.value();
            }
        }
    }
    /*
     * 是不是"挂着前进、等落盘"的那一对。
     * 用 shared_ptr 比对象本身:整表重排之后下标会变,但这一对还是同一个对象。
     */
    const bool isAwaitPair = (op.pair && m_awaitFileOpPair && op.pair == m_awaitFileOpPair);
    const bool awaitSettled = isAwaitPair && leftForPair == 0;
    const bool stillOnAwaitPair = awaitSettled
            && m_currentIndex >= 0 && m_currentIndex < (int)m_pairs.size()
            && m_pairs[m_currentIndex] == op.pair;
    if (awaitSettled)
        m_awaitFileOpPair.reset();

    /*
     * 失败提示。抽成 lambda 是因为有两条失败出口都要走它:
     *   · 真的回滚了(文件还在原处);
     *   · 没回滚(状态已经不在乐观值,或文件其实已经在目标位置)——
     *     那条路以前在函数中段直接 return,提示会被整段跳过。
     */
    auto notifyAwaitFailure = [this, &op, awaitSettled, stillOnAwaitPair]() {
        if (!awaitSettled) return;

        const int idx = pairIndexOf(op.pair);
        const QString side = (op.pair && op.fileIdx == op.pair->idxB) ? QStringLiteral("B")
                                                                     : QStringLiteral("A");
        if (stillOnAwaitPair) {
            /*
             * 用户还看着这一对:错误音 + 气泡 + 日志。
             * 强调"没有跳到下一对",否则他会以为按的那一下生效了。
             */
            dedupenv::playSound(DedupSound::Error);
            createFloatingLabel(this, QStringLiteral("磁盘操作失败:没有跳到下一对,可以重试或跳过"));
            logLine(QString("[%1] 【%2失败】pair %3 的 %4 侧没有落盘,已退回原状态;"
                            "视图留在这一对(不跳到下一对)—— 可以再按一次重试,"
                            "或按 Q(jump)/next 跳过")
                        .arg(nowStr(), op.restore ? QStringLiteral("恢复") : QStringLiteral("移动"))
                        .arg(idx).arg(side), true, 3);
        } else {
            logLine(QString("[%1] 【%2失败】pair %3 的 %4 侧没有落盘;"
                            "你已经不在那一对,它已退回待处理状态,回上面能看到")
                        .arg(nowStr(), op.restore ? QStringLiteral("恢复") : QStringLiteral("移动"))
                        .arg(idx).arg(side), true, 3);
        }
    };

    if (ok) {
        if (op.restore)
            logLine(QString("[%1] Restored from duplicate: %2 → %3")
                        .arg(nowStr()).arg(op.from).arg(op.to), true);

        /*
         * 文件刚搬完,可能有两对在等它:
         *   · 当前显示的这一对;
         *   · 已经发出切换、但显示图还没上屏的 pending 对
         *     (典型:remove B 之后自动前进到下一对,而下一对正好引用了刚被搬走的文件)。
         *
         * 以前这里只补当前对,于是"动作之后自动跳到的那一对"会在搬运期间
         * 读不到图、那一次预取直接放弃 —— 之后没有任何人再派发,
         * 表现就是"某一对怎么都打不开,切走再回来又好了"。
         */
        /*
         * 落盘成功:这一对的操作才算真的生效,现在才能前进到下一个 Pending。
         *
         * 必须放在下面读 cur/pending 快照之前 —— 前进会改 m_currentIndex,
         * 后面"补派显示图"要按新的当前对来补,否则补的是已经翻过去的那一对。
         */
        if (awaitSettled && stillOnAwaitPair)
            advanceToNextPendingIfOn(op.pair);
        else if (awaitSettled)
            logLine(QString("[%1] 这一对的文件操作已完成(用户已经不在那一对,不自动前进)")
                        .arg(nowStr()));

        int cur = -1, pending = -1;
        {
            std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
            cur = m_currentIndex;
            pending = m_pendingDisplayIndex;
        }
        /*
         * 延后 200ms 再补:搬运期间失败的那个预取任务可能刚好还在收尾,
         * 它的 in-flight 标记会让立即派发被跳过(那样就永远没人再试了)。
         * prepareDisplayPairAsync 自带去重,多派一次没有代价。
         */
        auto scheduleRetry = [this](int idx, int epoch) {
            if (idx < 0) return;
            QTimer::singleShot(200, this, [this, idx, epoch]() {
                if (epoch != m_dataEpoch) return;
                QImage tmpA, tmpB;
                if (!getDisplayCache(idx, tmpA, tmpB))
                    prepareDisplayPairAsync(idx, epoch);
            });
        };
        scheduleRetry(pending, op.epoch);
        if (cur != pending)
            scheduleRetry(cur, op.epoch);

        /*
         * 完全重复归属改判的第二步:回迁已经物理落盘,现在才能把原保留侧
         * 驱逐进 duplicate(登记见 finalizeExactDuplicateOwnership)。
         * epoch 检查 + clearState 清空双保险:数据换代后 keepFileIdx 是旧下标,
         * 绝不能在新数据集上执行驱逐。
         */
        if (op.restore && op.epoch == m_dataEpoch.load(std::memory_order_relaxed)) {
            auto itEvict = m_pendingEvictAfterRestore.constFind(op.fileIdx);
            if (itEvict != m_pendingEvictAfterRestore.constEnd()) {
                const int keepIdx = itEvict.value();
                m_pendingEvictAfterRestore.erase(itEvict);
                if (keepIdx >= 0 && keepIdx < (int)m_files.size()
                    && !m_files[keepIdx].inDuplicate) {
                    if (moveToDuplicate(keepIdx, nullptr)) {
                        logLine(QStringLiteral("[%1] 完全重复归属改判: %2 -> duplicate")
                                    .arg(nowStr(), m_files[keepIdx].originalPath));
                    } else {
                        logLine(QStringLiteral("[%1] 完全重复归属改判失败(保留原状): %2")
                                    .arg(nowStr(), m_files[keepIdx].originalPath), true, 3);
                    }
                }
            }
        }
        return;
    }

    // 失败:数据换代之后旧下标已经指向别的文件了,只能用日志告知,不能回滚
    if (op.epoch != m_dataEpoch.load(std::memory_order_relaxed)) {
        logLine(QString("[%1] 【文件移动失败】%2").arg(nowStr()).arg(err), true, 4);
        return;
    }
    if (op.fileIdx < 0 || op.fileIdx >= (int)m_files.size()) return;

    logLine(QString("[%1] 【文件移动失败】%2").arg(nowStr()).arg(err), true, 4);

    /*
     * 回迁物理失败:放弃"驱逐原保留侧"的登记(见 finalizeExactDuplicateOwnership)。
     * 宁可两份都留在原地,也不能让子集侧困在 duplicate 的同时把保留侧也搬走。
     * (epoch 不符的早退在上面 —— 那种情况 clearState 已经清空过这张表。)
     */
    m_pendingEvictAfterRestore.remove(op.fileIdx);

    /*
     * 用 IWD_WITH_MMUTEX_SHARED() 而不是裸 shared_lock:
     * 万一这条路径是在"本线程已经排他持有 m_mutex"的上下文里被处理的
     * (例如某个持有排他锁的操作里转了事件循环),裸锁会自己锁死自己,
     * 深度守卫则会正确地跳过加锁。
     */
    IWD_WITH_MMUTEX_SHARED();
    FileEntry &file = m_files[op.fileIdx];

    /*
     * 只有 FileEntry 还停在我们乐观写入的那个状态时才回滚。
     * 如果对不上,说明用户之后又做了新操作(路径已经被那次操作改写),
     * 那次操作才是最终意图 —— 在这里改回去反而会把它弄坏。
     */
    bool rolledBack = false;
    if (op.restore) {
        if (!file.inDuplicate && file.currentPath == op.to) {
            /*
             * 恢复失败。不能盲目把 currentPath 写回 op.from:
             * 连续"remove -> restore"且两步物理移动都失败时,op.from(=上一步
             * move 的 duplicate 目标)根本不存在 —— 文件其实一直躺在 op.to(原路径)。
             * 以前会把 currentPath 写成一个不存在的路径并置 inDuplicate=true,
             * 这张图从此"图片无法显示",磁盘与列表彻底对不上。
             * 现在以磁盘为准:op.from 还在才回滚;不在就说明文件在 op.to,
             * 字段已经与磁盘一致,什么都不用改。
             */
            if (QFileInfo::exists(op.from)) {
                file.currentPath = op.from;
                file.inDuplicate = true;
            } else if (!QFileInfo::exists(op.to)) {
                logLine(QString("[%1] 【文件恢复失败】源和目标都不在磁盘上,保持乐观状态: %2")
                            .arg(nowStr()).arg(QDir::toNativeSeparators(op.to)), true, 4);
            }
            rolledBack = true;
        }
    } else {
        if (file.inDuplicate && file.currentPath == op.to) {
            /*
             * 同上:op.from 没了(文件被外部删掉/别的会话动过)就别把
             * currentPath 指向一个不存在的新位置,保持乐观状态并记日志。
             */
            if (QFileInfo::exists(op.from)) {
                file.currentPath = op.from;
                file.inDuplicate = false;
            } else {
                logLine(QString("[%1] 【文件移动失败】源也不在磁盘上,保持乐观状态: %2")
                            .arg(nowStr()).arg(QDir::toNativeSeparators(op.to)), true, 4);
            }
            rolledBack = true;
        }
    }
    if (!rolledBack) {
        // 状态不在乐观值(用户之后又动过)或文件其实已经在目标位置:不做回滚,
        // 但"这次没搬成"仍要告诉用户 —— 尤其是被挂起了前进的那一对。
        notifyAwaitFailure();
        return;
    }

    /*
     * pair 状态回滚 + 撤销连带影响。
     * 物理移动失败意味着"文件其实还在原处",那么:
     *   - 移除失败 -> 这一对不能算已处理,其它因它变 Invalid 的 pair 也要还原;
     *   - 恢复失败 -> 文件仍在 duplicate,其它引用它的 pair 还应该是 Invalid。
     * 注意"文件在哪"这件事与 pair 自身状态无关,所以 Invalid 标记的修正在
     * op.pair 存在时就做,不管状态有没有回滚。
     */
    if (op.pair) {
        bool stateRolled = false;
        if (op.optimisticState >= 0 && op.rollbackState >= 0) {
            std::lock_guard<std::mutex> lkState(op.pair->stateMutex);
            if (static_cast<int>(op.pair->state) == op.optimisticState) {
                op.pair->state = static_cast<PairState>(op.rollbackState);
                stateRolled = true;
            }
        }
        markRelatedPairsValidState(file, op.pair, op.restore ? false : true);
        if (stateRolled)
            logLine(QString("[%1] 已回滚 pair 状态(移动未生效)").arg(nowStr()), true);
    }

    // 让列表立刻显示回滚后的状态(深度已由上面的宏登记,这里是共享持有)
    {
        std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
        finalizePairStateChange(DuplicatePairListView::ScrollNever);
    }

    /*
     * ===== 失败:不前进,留在这一对让用户重试或跳过 =====
     *
     * 以前这里只回滚状态就完了,而用户早在 remove 那一刻就被 findNextPending
     * 带到下一对去了 —— 那一对回滚成 Pending 却留在列表上方,
     * 看起来就像"next pending 跳过了它"(2026-11-20 用户报的那个)。
     * 现在前进本来就挂在文件操作上,失败就停在这里:错误音 + 一条写明出路的日志。
     */
    notifyAwaitFailure();
}

bool ImageDeduplicateWindow::moveToDuplicate(int fileIdx, std::shared_ptr<PairInfo> ctxPair,
                                             int optimisticState, int rollbackState)
{
    if (fileIdx < 0 || fileIdx >= (int)m_files.size()) return false;
    FileEntry &file = m_files[fileIdx];

    if (file.currentPath.isEmpty()) return false;

    // 已经在 duplicate 里(或者已经排队要搬进去),不需要再移动
    if (file.inDuplicate) return true;

    /*
     * 延迟执行模式(导入整理):
     * 这里【只登记】"本会话把这个文件判成了待移除",既不查磁盘也不改 currentPath。
     *
     * 为什么不改 currentPath:窗口后续还要用这条路径读图给用户看(previous 回看、
     * 显示图缓存、缩略图),文件在磁盘上也确实还在原地 —— 提前改成 duplicate/ 下的
     * 假路径会让所有读图路径全部落空。
     * inDuplicate 在这里的含义就退化成"本会话已经处理过它了":文件级别的手动/批量
     * 去重不会再对同一个文件重复动作,批量决策也会跳过它(见 on_btn_auto_rm_clicked)。
     */
    if (m_fileOpMode == FileOpMode::Deferred) {
        file.inDuplicate = true;
        return true;
    }

    /*
     * 没有在途操作时才查磁盘。
     * 有在途操作就说明这个文件的路径正被队列推进,磁盘状态可能落后于 file 的乐观状态 ——
     * 此时必须信任 FileEntry(比如"先搬进去、又恢复回来"这种连续操作,
     * 磁盘上文件还没动,但队列已经排好了两步,再查磁盘会误判成"文件不见了")。
     */
    if (!hasPendingFileOp(fileIdx) && !QFile::exists(file.currentPath)) {
        /*
         * 路径上已经没有文件:可能已被别处搬走,也可能被用户手工删了。
         * 保持原来的语义 —— 这不算失败(调用方据此把状态落下去),
         * 否则会出现"点了 Remove B 但那一对还是 Pending、而图片已经不在"的僵死状态。
         *
         * 但 FileEntry 必须跟着对齐:调用方会把这一对落成 Removed* 并把
         * 兄弟对标失效,而这里如果维持 inDuplicate=false、currentPath 指着
         * 一个不存在的路径,就是"状态说已移除、字段说文件还在原处",
         * 之后的判活/配对/恢复全乱。把它标成"已处理掉"(= 不在工作区)。
         */
        file.inDuplicate = true;
        return true;
    }

    QFileInfo fileInfo(file.currentPath);
    const QString fileName = fileInfo.fileName();
    const QString baseName = fileInfo.baseName();
    const QString suffix = fileInfo.completeSuffix();

    // 目标路径:原目录下的 duplicate/(和原来一致)
    QFileInfo origInfo(file.originalPath);
    const QString dupDir = origInfo.absolutePath() + "/duplicate";

    /*
     * 生成唯一的文件名。
     * 除了磁盘上已存在的,还要避开"已经被在途操作预定"的路径 ——
     * 文件还没真的搬过去,只查磁盘会让两个文件算出同一个目标名。
     */
    QString targetPath = dupDir + "/" + fileName;
    {
        std::lock_guard<std::mutex> lkOp(m_fileOpMutex);
        int counter = 1;
        while (QFile::exists(targetPath) || m_fileOpReservedTargets.count(targetPath) > 0) {
            targetPath = dupDir + "/" + baseName + "_" + QString::number(counter++) + "." + suffix;
        }
    }

    PendingFileOp op;
    op.fileIdx = fileIdx;
    op.from = file.currentPath;
    op.to = targetPath;
    op.restore = false;
    op.epoch = m_dataEpoch.load(std::memory_order_relaxed);
    op.pair = ctxPair;
    op.optimisticState = optimisticState;
    op.rollbackState = rollbackState;

    // 乐观更新:先让 FileEntry 指向目标位置,再入队(后面的决策要看到"已经搬走了")
    file.currentPath = targetPath;
    file.inDuplicate = true;
    enqueueFileOp(op);
    return true;
}

bool ImageDeduplicateWindow::restoreFromDuplicate(int fileIdx, std::shared_ptr<PairInfo> ctxPair,
                                                  int optimisticState, int rollbackState)
{
    if (fileIdx < 0 || fileIdx >= (int)m_files.size()) return false;
    FileEntry &file = m_files[fileIdx];

    if (file.currentPath.isEmpty()) return false;

    // 已经不在 duplicate 里(或已经排队要恢复),不需要恢复
    if (!file.inDuplicate) return true;

    /*
     * 延迟执行模式(导入整理):
     * 上面的 moveToDuplicate 只打了"已移除"的标记、磁盘没有任何动作,
     * 所以恢复就是把这个标记清掉,不需要(也不允许)排队列。
     */
    if (m_fileOpMode == FileOpMode::Deferred) {
        file.inDuplicate = false;
        return true;
    }

    /*
     * 刻意【不】在这里查磁盘 —— 有在途操作时文件可能还没真的搬进 duplicate,
     * 无在途操作时文件不在预期位置也该让后台去发现并报错(比静默跳过清楚)。
     * 队列是 FIFO 的,恢复一定排在那个移动之后执行。
     */
    const QString originalPath = file.originalPath.isEmpty() ? file.currentPath : file.originalPath;

    PendingFileOp op;
    op.fileIdx = fileIdx;
    op.from = file.currentPath;
    op.to = originalPath;
    op.restore = true;
    op.epoch = m_dataEpoch.load(std::memory_order_relaxed);
    op.pair = ctxPair;
    op.optimisticState = optimisticState;
    op.rollbackState = rollbackState;

    file.currentPath = originalPath;
    file.inDuplicate = false;
    enqueueFileOp(op);
    return true;
}

void ImageDeduplicateWindow::logLine(const QString &s, bool isAuto, int level)
{
    QString line = s;
    if (isAuto) line = QString("[auto] %1").arg(s);

    if (QThread::currentThread() != this->thread()) {
        QMetaObject::invokeMethod(this, [this, line, isAuto, level]() { logLine(line, isAuto, level); }, Qt::QueuedConnection);
        return;
    }

    ui.pte_log->appendPlainText(line);
    QScrollBar *bar = ui.pte_log->verticalScrollBar();
    if (bar) bar->setValue(bar->maximum());

    /*
     * 同步到主窗口的 LogWidget。level 就是 LogWidget::Level:
     *   0 Info / 1 Detail / 2 Good / 3 Warning / 4 Error。
     * 只有 3 和 4 会让日志窗口弹出来 + 响提示音,所以这里必须"谁真的失败谁传 3/4",
     * 不能再按 isAuto 一刀切成 Warning(那样自动移重复也会变成警报)。
     */
    emit logMessage(line, level);
}

///////////////////////////
// UI slots
///////////////////////////

void ImageDeduplicateWindow::showNoMorePairs()
{
    /*
     * 注意:这里【不】再做"完全重复归属收尾"。
     *
     * "所有 pair 都处理完"只说明列表里暂时没有 Pending 了,并不等于用户的
     * 去重工作结束 —— 他还可能回看(previous)、进筛选清理某张图、
     * 或者对某个目录对改主意。而收尾一旦跑过(m_autoDupFinalized)就不会再算,
     * 提前定案会把后续操作的结果全部丢掉。
     * 收尾统一放在窗口关闭 / 进入下一轮时,见 finalizeExactDuplicateOwnership()。
     */

    // 所有pair都处理完了:m_currentIndex 停在一个越界的位置(== m_pairs.size()),
    // 这样点previous才能回到最后一组。列表会取消选中并滚到底部。
    // 调用方必须已持有 currentIndexMutex。
    m_currentIndex = (int)m_pairs.size();
    m_pendingDisplayIndex = -1;
    m_pendingDisplayEpoch = -1;
    /*
     * 这些状态必须一起复位,否则下一次 paint 会把"没有更多对"重新刷成
     * "A · pair N / M"(onDisplayImagePainted 用 m_displayPairNumber/Total 拼角标):
     *   · m_switchInfoPending=false -> 不再等这次切换收尾;
     *   · unavailable/loading=false -> 角标不再追加"图片无法显示/加载中";
     *   · m_displayPairTotal=0 -> 让 onDisplayImagePainted 里的
     *     `if (m_display && m_displayPairTotal > 0)` 直接不成立,角标保持空。
     * 标题会被 updateWindowTitleAndBackground 刷成 "A 0"(剩余 0 对),这个是对的。
     */
    m_switchInfoPending = false;
    m_currentPairImageUnavailable = false;
    m_currentPairImageLoading = false;
    m_displayPairNumber = 0;
    m_displayPairTotal = 0;
    m_pixA = QPixmap();
    m_pixB = QPixmap();
    if (m_display) {
        m_awaitingPaintSerial = m_display->setPixmap(QPixmap());
        m_display->setStatusBadge(QString());   // 没有当前对了,角标别留着迷惑人
    }
    setPairLoadingHint(QString());
    setWindowTitle("No more pairs");
    syncPairListSelectionNoLock();
}

void ImageDeduplicateWindow::on_btn_previous_clicked()
{
    goToPreviousPair();
}


/* 动作名(日志/耗时导调用) */
static QString actionName(ActionRecord::Type type)
{
    switch (type) {
    case ActionRecord::RemoveA:    return QStringLiteral("remove A");
    case ActionRecord::RemoveB:    return QStringLiteral("remove B");
    case ActionRecord::RemoveBoth: return QStringLiteral("remove both");
    case ActionRecord::Skip:       return QStringLiteral("jump");
    default:                       return QStringLiteral("none");
    }
}

bool ImageDeduplicateWindow::blockedByFolderDecision(const QString &what)
{
    if (!m_waitingFolderDecision)
        return false;
    createFloatingLabel(this, QStringLiteral("请先完成目录选择(Folder Compare)"));
    logLine(QStringLiteral("[%1] 目录方向未确定,忽略 %2(请先在 Folder Compare 里选择)")
                .arg(nowStr(), what));
    return true;
}

/* ==================== 图片对右键菜单 ==================== */

namespace {

/* 原生 QMenu 只有一行字:"为什么不能点"直接拼在后面的括号里 */
QString withWhy(const QString &text, const QString &why)
{
    return why.isEmpty() ? text : QStringLiteral("%1  (%2)").arg(text, why);
}

} // namespace

int ImageDeduplicateWindow::indexOfPairPointer(const std::shared_ptr<PairInfo> &p)
{
    if (!p) return -1;
    std::shared_lock<std::shared_mutex> lk(m_mutex);
    for (int i = 0; i < (int)m_pairs.size(); ++i) {
        if (m_pairs[i] == p) return i;
    }
    return -1;
}

QVector<int> ImageDeduplicateWindow::indicesOfPairPointers(
    const QVector<std::shared_ptr<PairInfo>> &ps)
{
    QVector<int> out;
    if (ps.isEmpty()) return out;

    /*
     * 先收集要查的指针,再一次遍历 m_pairs 建立映射 —— 批量菜单可能有几百项,
     * 每个都线性扫一遍 35k 对是平方级开销。指针集合只用于查找,顺便去重。
     */
    QSet<const PairInfo *> want;
    for (const auto &p : ps) {
        if (p) want.insert(p.get());
    }
    if (want.isEmpty()) return out;

    std::shared_lock<std::shared_mutex> lk(m_mutex);
    out.reserve(want.size());
    for (int i = 0; i < (int)m_pairs.size() && !want.isEmpty(); ++i) {
        const auto &p = m_pairs[i];
        if (p && want.remove(p.get()))
            out.push_back(i);
    }
    return out;
}

ImageDeduplicateWindow::PairMenuCtx
ImageDeduplicateWindow::collectPairMenuCtx(int pairIndex, const QVector<int> &multiTargets)
{
    PairMenuCtx c;

    std::shared_lock<std::shared_mutex> lk(m_mutex);
    MMutexScope mScope(g_iwdMMutexDepth);
    std::lock_guard<std::mutex> lkIndex(currentIndexMutex);

    c.index         = (pairIndex < 0) ? m_currentIndex : pairIndex;
    c.inFilter      = m_filter.active;
    c.waitingFolder = m_waitingFolderDecision;
    c.filterSupported = (m_fileOpMode != FileOpMode::Deferred);

    if (c.index < 0 || c.index >= (int)m_pairs.size() || !m_pairs[c.index]) {
        c.index = -1;
        return c;
    }

    const std::shared_ptr<PairInfo> &p = m_pairs[c.index];
    {
        std::lock_guard<std::mutex> lkState(p->stateMutex);
        c.state = int(p->state);
        c.invalidMask = int(p->invalidMask);
        c.pathA = (p->idxA >= 0 && p->idxA < (int)m_files.size()) ? m_files[p->idxA].currentPath : QString();
        c.pathB = (p->idxB >= 0 && p->idxB < (int)m_files.size()) ? m_files[p->idxB].currentPath : QString();
        c.cause = p->invalidCauseForA - 1;
        if (p->idxA >= 0 && p->idxA < (int)m_files.size()) {
            c.hasA  = !m_files[p->idxA].currentPath.isEmpty();
            c.aGone = m_files[p->idxA].inDuplicate;
        }
        if (p->idxB >= 0 && p->idxB < (int)m_files.size()) {
            c.hasB  = !m_files[p->idxB].currentPath.isEmpty();
            c.bGone = m_files[p->idxB].inDuplicate;
        }
        /*
         * 能不能"恢复为 Pending":只有本对删过图(或本来就没被牵连失效)才有东西可撤销。
         * 纯失效(自己的叠加位是 0)恢复等于替别的对做主,一律拒绝。
         * 必须在锁里算 —— removedMask 和 state 是同一个锁保护的一对字段。
         */
        c.canRestore = (ownRemovalBits(p->state, p->removedMask) != 0)
                       || !isInvalidPairState(p->state);
    }

    auto dirOf = [](const QString &path) -> QString {
        if (path.isEmpty()) return QString();
        return QDir::cleanPath(QFileInfo(path).absolutePath()).toLower();
    };
    c.dirA = dirOf(c.pathA);
    c.dirB = dirOf(c.pathB);
    c.nameDirA = c.dirA.isEmpty() ? QString() : QFileInfo(c.dirA).fileName();
    c.nameDirB = c.dirB.isEmpty() ? QString() : QFileInfo(c.dirB).fileName();

    c.invalid  = pairLooksInvalid(PairState(c.state), quint8(c.invalidMask));
    c.aAlive   = c.hasA && !c.aGone;
    c.bAlive   = c.hasB && !c.bGone;
    c.folderOk = !c.dirA.isEmpty() && !c.dirB.isEmpty() && c.dirA != c.dirB;

    c.targets = (multiTargets.size() > 1) ? multiTargets : QVector<int>{c.index};
    c.multi   = c.targets.size() > 1;

    /*
     * 下标快照是给"菜单内容/可用性"用的;真正执行动作时只认指针
     * (见 indexOfPairPointer —— 菜单 exec() 期间后台可能整表重排)。
     */
    c.pair = p;
    c.pairTargets.reserve(c.targets.size());
    for (int idx : std::as_const(c.targets)) {
        if (idx >= 0 && idx < (int)m_pairs.size() && m_pairs[idx])
            c.pairTargets.push_back(m_pairs[idx]);
    }
    return c;
}

void ImageDeduplicateWindow::setupActionButtonContextMenus()
{
    /*
     * 每个按钮的右键菜单只放**它自己**的范围变体(不是一个"什么都有"的大菜单):
     *   btn_rm_a    -> 筛选 A / Remove A 整个目录
     *   btn_rm_b    -> 筛选 B / Remove B 整个目录
     *   btn_rm_both -> 筛选 A+B / Remove Both 两个目录
     *   btn_jmp     -> Jump This File / Jump These Folders
     *   btn_next    -> 跳转到下一个目录对比区域(目录去重模式)
     *   btn_previous-> 跳转到上一个目录对比区域(往回走的那个方向)
     * 左键语义完全不变。
     *
     * 两个方向的目录区域跳转各归自己的按钮:next 只给"下一个",previous 只给
     * "上一个"。以前 previous 的右键里两个方向都有,和 next 的右键重复。
     * 快捷键(Shift+←/→)只写在 tooltip 里,菜单条目不再标注。
     */
    struct Binding { QPushButton *btn; int role; QString hint; };
    const Binding bindings[] = {
        { ui.btn_rm_a,    0, QStringLiteral("右键:筛选与 A 相似的配对 / 删掉 A 整个目录") },
        { ui.btn_rm_b,    1, QStringLiteral("右键:筛选与 B 相似的配对 / 删掉 B 整个目录") },
        { ui.btn_rm_both, 2, QStringLiteral("右键:筛选 A、B 的所有配对 / 删掉这两个目录") },
        { ui.btn_jmp,     3, QStringLiteral("右键:Jump This File / Jump These Folders") },
        { ui.btn_next,    4, QStringLiteral("跳到下一个待处理(pending)的对  (→)\n右键:跳转到下一个目录对比区域  (Shift+→)") },
        { ui.btn_previous, 5, QStringLiteral("回到上一对(可回看已处理过的)  (←)\n右键:跳转到上一个目录对比区域的最后一个有效对  (Shift+←)") },
    };

    for (const Binding &b : bindings) {
        if (!b.btn) continue;
        b.btn->setContextMenuPolicy(Qt::CustomContextMenu);
        b.btn->setToolTip(b.hint);
        connect(b.btn, &QWidget::customContextMenuRequested, this,
                [this, btn = b.btn, role = b.role](const QPoint &pos) {
                    showPairButtonMenu(btn, role, btn->mapToGlobal(pos));
                });
    }
}

void ImageDeduplicateWindow::showPairButtonMenu(QWidget *button, int role,
                                               const QPoint &globalPos)
{
    const PairMenuCtx c = collectPairMenuCtx(/*pairIndex=*/-1, QVector<int>());
    if (c.index < 0)
        return;

    /* 状态类动作的拦截原因 */
    QString blocked;
    if (c.invalid)            blocked = QStringLiteral("这一对已失效,改动请到导致它失效的那一对");
    else if (c.waitingFolder) blocked = QStringLiteral("请先完成目录选择(Folder Compare)");
    else if (c.inFilter)      blocked = QStringLiteral("筛选进行中,先点 finished 退出");

    /* 筛选类的拦截原因:失效的对**可以**筛(筛的是那张图,不是这一对的状态) */
    QString filterWhy;
    if (c.waitingFolder)         filterWhy = QStringLiteral("请先完成目录选择(Folder Compare)");
    else if (c.inFilter)         filterWhy = QStringLiteral("筛选进行中,先点 finished 退出");
    else if (!c.filterSupported) filterWhy = QStringLiteral("当前模式(导入整理 / remove cover)不支持筛选");

    QString folderWhy = blocked;
    if (folderWhy.isEmpty() && !c.folderOk)
        folderWhy = QStringLiteral("两张图在同一个目录,没有\"这两个目录\"");

    QVector<pairmenu::Section> sections;

    if (role == 4) {
        /*
         * btn_next 的右键:跳到下一个目录对比区域。
         * 只在目录去重(文件夹对比)模式下有意义 —— 普通/导入模式的 pair
         * 本来就不按目录对排序,"区域"无从谈起;筛选里更没有区域的概念。
         * 等待 FolderCompare 决策期间 m_pairs 还是空的,collectPairMenuCtx
         * 已经在前面 return 了,这里不用再拦。
         */
        pairmenu::Section s1;
        pairmenu::Entry e;
        QString why;
        if (!isFolderCompareMode())
            why = QStringLiteral("只在目录去重(文件夹对比)模式下可用");
        else if (c.inFilter)
            why = QStringLiteral("筛选进行中,先点 finished 退出");
        e.text    = withWhy(QStringLiteral("跳转到下一个目录对比区域"), why);
        e.enabled = why.isEmpty();
        e.trigger = [this]() { goToNextFolderRegionPair(); };
        s1.push_back(e);
        sections.push_back(s1);
    } else if (role == 5) {
        /*
         * btn_previous 的右键:只放**往回走**的这一个方向。
         *
         * "下一个目录对比区域"在 btn_next 的右键里已经有(role 4),这里曾经又
         * 复制了一份(还各自标了 Shift+→),同一个动作两个入口 —— 属于重复实现,
         * 已删。菜单只写动作名,快捷键放在按钮 tooltip 里,不在菜单里堆注释。
         */
        QString why;
        if (!isFolderCompareMode())
            why = QStringLiteral("只在目录去重(文件夹对比)模式下可用");
        else if (c.inFilter)
            why = QStringLiteral("筛选进行中,先点 finished 退出");

        pairmenu::Section s1;

        pairmenu::Entry e1;
        e1.text    = withWhy(QStringLiteral("跳转到上一个目录对比区域的最后一个有效对"), why);
        e1.enabled = why.isEmpty();
        e1.trigger = [this]() { goToLastFolderRegionPair(); };
        s1.push_back(e1);

        sections.push_back(s1);
    } else if (role >= 0 && role <= 2) {
        QString why = filterWhy;
        /*
         * role 0 = btn_rm_a -> 筛选 A(锚点在 A);role 1 -> 筛选 B;role 2 -> A+B。
         * 只要求**锚点侧**有文件路径:已经被移进 duplicate 的图照样能筛
         * (startSimilarityFilter 不重复删锚点,只收拾还活着的相似版本),
         * 以前按 aGone/bGone 禁用等于把这条能力堵死了。
         */
        if (why.isEmpty() && (role == 0 || role == 2) && !c.hasA)
            why = QStringLiteral("A 侧没有有效文件");
        if (why.isEmpty() && (role == 1 || role == 2) && !c.hasB)
            why = QStringLiteral("B 侧没有有效文件");

        pairmenu::Section s1;
        pairmenu::Entry f;
        switch (role) {
        case 0:  f.text = QStringLiteral("筛选:与 A 这张图相似的所有配对"); break;
        case 1:  f.text = QStringLiteral("筛选:与 B 这张图相似的所有配对"); break;
        default: f.text = QStringLiteral("筛选:A 和 B 的所有配对(原对立刻 remove both)"); break;
        }
        const std::shared_ptr<PairInfo> pPtr = c.pair;
        auto filteredRow = [this, pPtr](const QString &what) -> int {
            const int row = indexOfPairPointer(pPtr);
            if (row < 0)
                createFloatingLabel(this, what);
            return row;
        };
        if (role == 1) {
            f.trigger = [this, pPtr, filteredRow]() {
                const int row = filteredRow(QStringLiteral("这一对已经不在列表里,筛选已取消"));
                if (row >= 0) startSimilarityFilter(row, FilterEntryMode::FilterFromB);
            };
        } else if (role == 2) {
            f.trigger = [this, pPtr, filteredRow]() {
                const int row = filteredRow(QStringLiteral("这一对已经不在列表里,筛选已取消"));
                if (row >= 0) startSimilarityFilter(row, FilterEntryMode::FilterFromBoth);
            };
        } else {
            f.trigger = [this, pPtr, filteredRow]() {
                const int row = filteredRow(QStringLiteral("这一对已经不在列表里,筛选已取消"));
                if (row >= 0) startSimilarityFilter(row, FilterEntryMode::FilterFromA);
            };
        }
        f.text = withWhy(f.text, why);
        f.enabled = why.isEmpty();
        s1.push_back(f);
        sections.push_back(s1);

        pairmenu::Section s2;
        pairmenu::Entry r;
        switch (role) {
        case 0:
            r.text = c.nameDirA.isEmpty()
                         ? QStringLiteral("Remove A 整个目录")
                         : QStringLiteral("Remove A 整个目录(%1)").arg(c.nameDirA);
            break;
        case 1:
            r.text = c.nameDirB.isEmpty()
                         ? QStringLiteral("Remove B 整个目录")
                         : QStringLiteral("Remove B 整个目录(%1)").arg(c.nameDirB);
            break;
        default:
            r.text = QStringLiteral("Remove Both 两个目录(%1 ↔ %2)")
                         .arg(c.nameDirA.isEmpty() ? QStringLiteral("?") : c.nameDirA,
                              c.nameDirB.isEmpty() ? QStringLiteral("?") : c.nameDirB);
            break;
        }
        const int side = (role == 1) ? 1 : (role == 2 ? 2 : 0);
        r.text = withWhy(r.text, folderWhy);
        r.enabled = folderWhy.isEmpty();
        r.trigger = [this, pPtr, side]() {
            const int row = indexOfPairPointer(pPtr);
            if (row < 0) {
                createFloatingLabel(this, QStringLiteral("这一对已经不在列表里,目录级删除已取消"));
                return;
            }
            removeFolders(row, side);
        };
        s2.push_back(r);
        sections.push_back(s2);
    } else {
        pairmenu::Section s1;
        // 按钮菜单永远作用在"当前这一对"上,锚点就是屏幕上那张
        const QString anchorTag = m_showingA ? QStringLiteral("(A)") : QStringLiteral("(B)");

        pairmenu::Entry jf;
        jf.text = withWhy(QStringLiteral("Jump This File %1").arg(anchorTag), blocked);
        jf.enabled = blocked.isEmpty();
        {
            const std::shared_ptr<PairInfo> pPtr = c.pair;
            jf.trigger = [this, pPtr]() {
                const int row = indexOfPairPointer(pPtr);
                if (row < 0) {
                    createFloatingLabel(this, QStringLiteral("这一对已经不在列表里,Jump 已取消"));
                    return;
                }
                jumpForPair(row, JumpScope::File);
            };
        }
        s1.push_back(jf);

        pairmenu::Entry j2;
        j2.text = withWhy(QStringLiteral("Jump These Two Files (A + B)"), blocked);
        j2.enabled = blocked.isEmpty();
        {
            const std::shared_ptr<PairInfo> pPtr = c.pair;
            j2.trigger = [this, pPtr]() {
                const int row = indexOfPairPointer(pPtr);
                if (row < 0) {
                    createFloatingLabel(this, QStringLiteral("这一对已经不在列表里,Jump 已取消"));
                    return;
                }
                jumpForPair(row, JumpScope::TwoFiles);
            };
        }
        s1.push_back(j2);

        pairmenu::Entry jd;
        jd.text = withWhy(QStringLiteral("Jump These Folders"), blocked);
        jd.enabled = blocked.isEmpty();
        {
            const std::shared_ptr<PairInfo> pPtr = c.pair;
            jd.trigger = [this, pPtr]() {
                const int row = indexOfPairPointer(pPtr);
                if (row < 0) {
                    createFloatingLabel(this, QStringLiteral("这一对已经不在列表里,Jump 已取消"));
                    return;
                }
                jumpForPair(row, JumpScope::Folders);
            };
        }
        s1.push_back(jd);
        sections.push_back(s1);
    }

    pairmenu::exec(button ? button : this, globalPos, sections);
}

void ImageDeduplicateWindow::onPairListContextMenuRequested(int pairIndex,
                                                          const QVector<int> &targets,
                                                          const QPoint &globalPos)
{
    showPairActionsMenu(pairIndex, targets, m_pairList, globalPos);
}

void ImageDeduplicateWindow::showPairActionsMenu(int pairIndex, const QVector<int> &multiTargets,
                                                QWidget *anchor, const QPoint &globalPos)
{
    const PairMenuCtx c = collectPairMenuCtx(pairIndex, multiTargets);
    if (c.index < 0)
        return;

    /*
     * 可用性按【每一侧】判断,不再给失效对一刀切。
     *
     * 一对失效(InvalidB)只说明 B 那张图被别的对移走了,A 还在原处 ——
     * 这时"remove A"是一个完全独立的决定,应该允许,结果是复合态 InvalidB + RemovedA。
     * 反之 InvalidA 允许 remove B。只有"要删的那一侧本来就不在了"才禁用。
     */
    const bool aAlive = c.hasA && !c.aGone;
    const bool bAlive = c.hasB && !c.bGone;

    const QString baseBlocked = c.waitingFolder
                                    ? QStringLiteral("请先完成目录选择(Folder Compare)")
                                    : QString();

    auto sideWhy = [&](bool alive, const QString &side) -> QString {
        if (!baseBlocked.isEmpty()) return baseBlocked;
        if (!alive) return QStringLiteral("%1 侧已经进 duplicate").arg(side);
        return QString();
    };

    QString removeAWhy = sideWhy(aAlive, QStringLiteral("A"));
    if (removeAWhy.isEmpty() && c.inFilter)
        removeAWhy = QStringLiteral("筛选里锚点已经处理过;要改主意请 finished 退出后在原对上改");

    QString removeBWhy = sideWhy(bAlive, QStringLiteral("B"));

    QString removeBothWhy = baseBlocked;
    if (removeBothWhy.isEmpty() && (!aAlive || !bAlive))
        removeBothWhy = QStringLiteral("有一侧已经进 duplicate,没有\"两边\"可删");
    /*
     * 筛选里 remove both 和 remove B 是同一件事(锚点进入时就删掉了),
     * 所以它的可用性跟着"另一侧还在不在"走 —— 不能因为锚点那一侧已经
     * 进了 duplicate 就把这一项禁掉,否则用户在筛选里点右键发现只有
     * remove B 可点,而列表底部的 s 键却能用,两个入口对不上。
     */
    if (c.inFilter && removeBothWhy.isEmpty())
        removeBothWhy = removeBWhy;

    /*
     * 恢复只能撤销【本对】删过的图:
     *   · 有叠加位(或者本来就是 Removed-A/B/Both、Skipped、Pending)
     *     -> 可以,恢复后按 invalidMask 收尾;
     *   · 纯失效(本对一只都没删)-> 只能去肇事那一对改,否则会把别人的 remove 一起撤销。
     */
    QString restoreWhy = baseBlocked;
    if (restoreWhy.isEmpty() && !c.canRestore)
        restoreWhy = QStringLiteral("这一对只是被别的对牵连失效,本对没删过图;请到肇事那一对改");

    /*
     * 筛选类:失效的对**可以**筛 —— 筛的是锚点那张图,不是这一对的状态。
     * (用户的原话:"对 invalid pair 进行筛选"是列表菜单独有的能力)
     * 锚点那张图**已经被移进 duplicate** 也一样能筛:filter 的用途是"找出所有
     * 和这张图相似的配对",删没删掉不影响这件事(见 addFilter 的说明)。
     */
    QString filterWhy;
    if (c.waitingFolder)         filterWhy = QStringLiteral("请先完成目录选择(Folder Compare)");
    else if (c.inFilter)         filterWhy = QStringLiteral("筛选进行中,先点 finished 退出");
    else if (!c.filterSupported) filterWhy = QStringLiteral("当前模式(导入整理 / remove cover)不支持筛选");

    /*
     * "整个目录"这类动作在筛选里仍然禁止:它会按目录把成百上千张图一起移走,
     * 而筛选列表只是"含锚点的那几对",两者范围完全不搭。
     * (上面的 blocked 在筛选下已经故意置空,所以这里单独补一句)
     */
    QString folderWhy = baseBlocked;
    if (folderWhy.isEmpty() && c.inFilter)
        folderWhy = QStringLiteral("筛选进行中,先点 finished 退出");
    if (folderWhy.isEmpty() && !c.folderOk)
        folderWhy = QStringLiteral("两张图在同一个目录,没有\"这两个目录\"");

    const QString scope = c.multi ? QStringLiteral("  (全部 %1 对)").arg(c.targets.size())
                                  : QString();

    QVector<pairmenu::Section> sections;

    auto addPairAction = [&](pairmenu::Section &sec, const QString &text,
                             PairBatchAction act, const QString &why) {
        pairmenu::Entry e;
        e.text = text + scope;
        e.text = withWhy(e.text, why);
        e.enabled = why.isEmpty();
        const bool m = c.multi;
        /*
         * 只捕获指针,不捕获下标:菜单 exec() 是嵌套事件循环,点下菜单项那一刻
         * m_pairs 可能已经被后台 sortPairsFromIndex 重排 —— 旧下标会指向别的对。
         */
        const QVector<std::shared_ptr<PairInfo>> tPtrs = c.pairTargets;
        const std::shared_ptr<PairInfo> pPtr = c.pair;
        e.trigger = [this, m, tPtrs, pPtr, act]() {
            if (m) {
                const QVector<int> rows = indicesOfPairPointers(tPtrs);
                if (rows.size() != tPtrs.size()) {
                    createFloatingLabel(this,
                        QStringLiteral("列表已经变化(%1 对里只剩 %2 对),操作已取消")
                            .arg(tPtrs.size()).arg(rows.size()));
                    logLine(QStringLiteral("[%1] 右键批量动作取消:菜单打开期间有 pair 被移出列表")
                                .arg(nowStr()), true);
                    return;
                }
                onPairListBatchAction(rows, static_cast<int>(act));
            } else {
                const int row = indexOfPairPointer(pPtr);
                if (row < 0) {
                    createFloatingLabel(this, QStringLiteral("这一对已经不在列表里,操作已取消"));
                    return;
                }
                onPairListPairAction(row, static_cast<int>(act));
            }
        };
        sec.push_back(e);
    };

    auto addFilter = [&](pairmenu::Section &sec, const QString &text, FilterEntryMode mode,
                         bool needA, bool needB) {
        QString why = filterWhy;
        /*
         * 锚点只要还有文件路径就能筛 —— **包括已经被移进 duplicate 的那张图**。
         *
         * filter 的作用是"列出所有和这张图相似的配对",图被移走并不改变它有哪些
         * 相似图;startSimilarityFilter 也不会重复删已经不在的锚点,
         * 只把还活着的相似版本列出来。以前这里按 aGone/bGone 禁用,
         * 等于把"筛一张已经删掉的图"整个堵死在 UI 上。
         *
         * needA/needB 指锚点侧:HasA==false 只发生在 currentPath 为空(占位项),
         * 那种才真的没法筛。
         */
        if (why.isEmpty() && needA && !c.hasA)
            why = QStringLiteral("A 侧没有有效文件");
        if (why.isEmpty() && needB && !c.hasB)
            why = QStringLiteral("B 侧没有有效文件");

        pairmenu::Entry e;
        e.text = withWhy(text, why);
        e.enabled = why.isEmpty();
        const std::shared_ptr<PairInfo> pPtr = c.pair;
        e.trigger = [this, pPtr, mode]() {
            const int row = indexOfPairPointer(pPtr);
            if (row < 0) {
                createFloatingLabel(this, QStringLiteral("这一对已经不在列表里,筛选已取消"));
                return;
            }
            startSimilarityFilter(row, mode);
        };
        sec.push_back(e);
    };

    /* ---- 1) Remove A ---- */
    {
        pairmenu::Section s;
        addPairAction(s, QStringLiteral("Remove A"), PairBatchAction::SetRemovedA, removeAWhy);
        /* 锚点在 A 侧,所以只要求 A 有有效文件(needA, needB) */
        addFilter(s, QStringLiteral("筛选:与 A 这张图相似的所有配对"),
                  FilterEntryMode::FilterFromA, true, false);
        pairmenu::Entry e;
        e.text = withWhy(c.nameDirA.isEmpty()
                             ? QStringLiteral("Remove A 整个目录")
                             : QStringLiteral("Remove A 整个目录(%1)").arg(c.nameDirA),
                         folderWhy);
        e.enabled = folderWhy.isEmpty();
        {
            const std::shared_ptr<PairInfo> pPtr = c.pair;
            e.trigger = [this, pPtr]() {
                const int row = indexOfPairPointer(pPtr);
                if (row < 0) {
                    createFloatingLabel(this, QStringLiteral("这一对已经不在列表里,目录级删除已取消"));
                    return;
                }
                removeFolders(row, 0);
            };
        }
        s.push_back(e);
        sections.push_back(s);
    }

    /* ---- 2) Remove B ---- */
    {
        pairmenu::Section s;
        addPairAction(s, QStringLiteral("Remove B"), PairBatchAction::SetRemovedB, removeBWhy);
        /* 锚点在 B 侧 */
        addFilter(s, QStringLiteral("筛选:与 B 这张图相似的所有配对"),
                  FilterEntryMode::FilterFromB, false, true);
        pairmenu::Entry e;
        e.text = withWhy(c.nameDirB.isEmpty()
                             ? QStringLiteral("Remove B 整个目录")
                             : QStringLiteral("Remove B 整个目录(%1)").arg(c.nameDirB),
                         folderWhy);
        e.enabled = folderWhy.isEmpty();
        {
            const std::shared_ptr<PairInfo> pPtr = c.pair;
            e.trigger = [this, pPtr]() {
                const int row = indexOfPairPointer(pPtr);
                if (row < 0) {
                    createFloatingLabel(this, QStringLiteral("这一对已经不在列表里,目录级删除已取消"));
                    return;
                }
                removeFolders(row, 1);
            };
        }
        s.push_back(e);
        sections.push_back(s);
    }

    /* ---- 3) Remove Both ---- */
    {
        pairmenu::Section s;
        addPairAction(s, QStringLiteral("Remove Both"), PairBatchAction::SetRemovedBoth, removeBothWhy);
        addFilter(s, QStringLiteral("筛选:A 和 B 的所有配对(原对立刻 remove both)"),
                  FilterEntryMode::FilterFromBoth, true, true);
        pairmenu::Entry e;
        e.text = withWhy(QStringLiteral("Remove Both 两个目录(%1 ↔ %2)")
                             .arg(c.nameDirA.isEmpty() ? QStringLiteral("?") : c.nameDirA,
                                  c.nameDirB.isEmpty() ? QStringLiteral("?") : c.nameDirB),
                         folderWhy);
        e.enabled = folderWhy.isEmpty();
        {
            const std::shared_ptr<PairInfo> pPtr = c.pair;
            e.trigger = [this, pPtr]() {
                const int row = indexOfPairPointer(pPtr);
                if (row < 0) {
                    createFloatingLabel(this, QStringLiteral("这一对已经不在列表里,目录级删除已取消"));
                    return;
                }
                removeFolders(row, 2);
            };
        }
        s.push_back(e);
        sections.push_back(s);
    }

    /* ---- 4) Jump ---- */
    {
        pairmenu::Section s;

        /*
         * Jump Pendings:选中的这些对里,凡是仍然 Pending 的标记为 Skipped;
         * 已经 remove 过的对【一个文件都不动】。
         *
         * 它就是原来的"跳过这一对(jump)"和"Jump This File"的替代:
         *   - 前者在批量模式下会把 RemovedA/B 也套上 Skip,而 Skip 分支会把
         *     已经搬进 duplicate 的文件搬回来 —— 等于抹掉之前的 remove 决定;
         *   - 后者只能按"某一张图"跳,想跳过手头这几对反而要来回切图。
         * 现在整段框选 -> 右键 -> Jump Pendings,一次到位。
         */
        /*
         * 筛选里仍然不给 Jump:筛选的"跳过"由按钮(jmp)走 filterApplyAction,
         * 那条路径会避开 restoreIfNeeded;菜单这条 jumpForPair 会把已删除的一侧
         * 从 duplicate 搬回来。
         */
        /*
         * Jump Pendings 只作用于用户框选的这几行,所以筛选下同样可用。
         * 普通模式下它只改状态、不动文件(只碰 Pending);
         * 筛选模式下它会把本对删过的另一侧搬回来再标 Skipped —— 那是"把删过的对
         * 改成 jumped"唯一正确的做法,见 filterBatchAction(Skip)。
         * Jump These Files/Folders 会按"含这张图/这两个目录"圈到筛选集合以外,
         * 筛选下仍然禁用。
         */
        QString jumpPendingWhy = baseBlocked;
        /*
         * 筛选里失效是常态(锚点在进入时就删掉了),不能拿"这一对已失效"把它禁掉:
         * 筛选里的 Jump Pendings 会走 filterBatchAction(Skip) —— 撤销本对删过的
         * 那一侧再标 Skipped,正是"把删过的对改成 jumped"要的操作。
         */
        if (jumpPendingWhy.isEmpty() && c.invalid && !c.inFilter)
            jumpPendingWhy = QStringLiteral("这一对已失效,先处理导致它失效的那一对");

        QString jumpWhy = jumpPendingWhy;
        if (jumpWhy.isEmpty() && c.inFilter)
            jumpWhy = QStringLiteral("筛选进行中,用 jump 按钮跳过(菜单里的 Jump 会动文件)");

        pairmenu::Entry jp;
        /*
         * 筛选里这个条目的语义是"把这几行改成 jumped"(会把本对删过的另一侧搬回来),
         * 不是"只挑 Pending" —— 筛选成员全是 InvalidA,照原名会让人以为没东西可跳。
         */
        const QString jpText = (c.inFilter
                                    ? QStringLiteral("跳过这些行(恢复已删的另一侧)")
                                    : QStringLiteral("Jump Pendings")) + scope;
        jp.text = withWhy(jpText, jumpPendingWhy);
        jp.enabled = jumpPendingWhy.isEmpty();
        {
            const QVector<std::shared_ptr<PairInfo>> tPtrs = c.pairTargets;
            jp.trigger = [this, tPtrs]() {
                const QVector<int> rows = indicesOfPairPointers(tPtrs);
                if (rows.size() != tPtrs.size()) {
                    createFloatingLabel(this,
                        QStringLiteral("列表已经变化(%1 对里只剩 %2 对),操作已取消")
                            .arg(tPtrs.size()).arg(rows.size()));
                    return;
                }
                jumpPendingPairs(rows);
            };
        }
        s.push_back(jp);

        pairmenu::Entry j2;
        j2.text = withWhy(QStringLiteral("Jump These Two Files (A + B)"), jumpWhy);
        j2.enabled = jumpWhy.isEmpty();
        {
            const std::shared_ptr<PairInfo> pPtr = c.pair;
            j2.trigger = [this, pPtr]() {
                const int row = indexOfPairPointer(pPtr);
                if (row < 0) {
                    createFloatingLabel(this, QStringLiteral("这一对已经不在列表里,Jump 已取消"));
                    return;
                }
                jumpForPair(row, JumpScope::TwoFiles);
            };
        }
        s.push_back(j2);

        pairmenu::Entry jd;
        jd.text = withWhy(QStringLiteral("Jump These Folders"), jumpWhy);
        jd.enabled = jumpWhy.isEmpty();
        {
            const std::shared_ptr<PairInfo> pPtr = c.pair;
            jd.trigger = [this, pPtr]() {
                const int row = indexOfPairPointer(pPtr);
                if (row < 0) {
                    createFloatingLabel(this, QStringLiteral("这一对已经不在列表里,Jump 已取消"));
                    return;
                }
                jumpForPair(row, JumpScope::Folders);
            };
        }
        s.push_back(jd);
        sections.push_back(s);
    }

    /* ---- 5) 状态 ---- */
    {
        pairmenu::Section s;
        addPairAction(s, QStringLiteral("恢复为 Pending"), PairBatchAction::SetPending, restoreWhy);
        sections.push_back(s);
    }

    /* ---- 6) 打开所在目录 ---- */
    {
        pairmenu::Section s;
        pairmenu::Entry ea;
        ea.text = QStringLiteral("在资源管理器中打开 A");
        ea.enabled = !c.pathA.isEmpty();
        {
            const QString p = c.pathA;
            ea.trigger = [p]() { dedupenv::revealFileInExplorer(p); };
        }
        s.push_back(ea);

        pairmenu::Entry eb;
        eb.text = QStringLiteral("在资源管理器中打开 B");
        eb.enabled = !c.pathB.isEmpty();
        {
            const QString p = c.pathB;
            eb.trigger = [p]() { dedupenv::revealFileInExplorer(p); };
        }
        s.push_back(eb);
        sections.push_back(s);
    }

    /* ---- 7) 失效对:跳到肇事的那一对(列表行独有) ---- */
    if (c.invalid) {
        pairmenu::Section s;
        pairmenu::Entry e;
        const bool hasCause = (c.cause >= 0 && c.cause < (int)m_pairs.size());
        e.text = withWhy(QStringLiteral("跳到导致它失效的那一对"),
                         hasCause ? QStringLiteral("pair %1").arg(c.cause)
                                  : QStringLiteral("找不到肇事的那一对"));
        e.enabled = hasCause;
        {
            const int k = c.cause;
            e.trigger = [this, k]() {
                if (k < 0 || k >= (int)m_pairs.size()) return;
                std::lock_guard<std::mutex> lk(currentIndexMutex);
                displayPairByIndex(k, m_dataEpoch);
            };
        }
        s.push_back(e);
        sections.push_back(s);
    }

    pairmenu::exec(anchor ? anchor : this, globalPos, sections);
}
void ImageDeduplicateWindow::applyActionToCurrentPair(ActionRecord::Type type)
{
    /*
     * 单键操作(remove A / remove B / remove both / jump)的统一实现。
     *
     * 流程:动作 -> 只刷新被改到的行 -> 找下一个 Pending -> 切过去。
     * 每一步都记了耗时,总耗时超过 150ms 会写一行 [慢操作]:
     * 35000 张的会话里"点一下卡住几分钟"这种问题,光靠猜没用,
     * 这条日志能直接指出是动作本身、列表刷新、查找下一个还是切图慢。
     */
    QElapsedTimer tm;
    tm.start();

    /*
     * 按钮锁着 = 新图还没上屏(见 displayPairByIndex / onDisplayImagePainted)。
     * 按钮本身已经被禁用了,但 a/s/d/空格 这些快捷键不经过按钮 ——
     * 不在这里拦一道,"键盘能删、按钮不能删",保护就形同虚设。
     */
    if (m_pairActionButtonsLocked) {
        createFloatingLabel(this, QStringLiteral("图片还没上屏,等这一对显示出来再操作"));
        logLine(QString("[%1] 忽略 %2:显示图尚未上屏,屏幕内容与当前对不一致")
                    .arg(nowStr(), actionName(type)));
        return;
    }

    std::shared_lock<std::shared_mutex> lk(m_mutex);
    /*
     * 登记"本线程已持有 m_mutex":后面 displayPairByIndex() 内部会走
     * IWD_WITH_MMUTEX_SHARED()。不登记的话它会再拿一次共享锁 ——
     * 如果此刻恰好有线程在等排他锁(后台重排),第二次共享申请会被挡在写者后面,
     * 而写者又在等我们释放 —— 直接死锁。
     */
    MMutexScope mScope(g_iwdMMutexDepth);
    std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
    if (m_currentIndex < 0 || m_currentIndex >= (int)m_pairs.size())
        return;

    /*
     * 这一对还在等上一次操作落盘:再按一次会作用在同一个文件上,
     * 而磁盘状态还没定 —— 先忽略。
     * 同盘 rename 是毫秒级,正常感觉不到;真撞上共享冲突重试时也就几百毫秒。
     */
    if (m_awaitFileOpPair) {
        if (m_pairFileOpsInFlight.value(m_awaitFileOpPair.get(), 0) <= 0) {
            // 自愈:标记还在但已经没有在途操作(线程池异常/窗口正在收尾)
            m_awaitFileOpPair.reset();
        } else if (m_pairs[m_currentIndex] == m_awaitFileOpPair) {
            createFloatingLabel(this, QStringLiteral("这一对还在等磁盘操作结果,稍等一下"));
            logLine(QString("[%1] 忽略 %2:上一次操作还在等落盘")
                        .arg(nowStr(), actionName(type)));
            return;
        }
    }

    /*
     * 目录方向还没定(FolderCompareWidget 开着)时不允许改状态。
     *
     * 这期间 A/B 谁在左、完全重复归谁都不确定,现在删掉的文件很可能就是
     * 用户下一步选"保留 B"时想要的那一份;而且建对/摆位/移动还没跑完,
     * 两边同时动文件会把状态搞乱。
     */
    if (blockedByFolderDecision(actionName(type)))
        return;

    /*
     * 筛选状态:动作语义完全不同(只删"另一侧",remove A 变成 finished),
     * 交给 filterApplyAction 单独处理,不走普通流程。
     */
    if (m_filter.active) {
        filterApplyAction(type);
        return;
    }

    const int actedIndex = m_currentIndex;
    QVector<int> changed;
    changed.push_back(actedIndex);   // 被操作的这一行自己也要刷(applyActionToPair 不填它)

    // 提出来:动作之后要按它查"这一对有没有在途文件操作",决定前进还是挂起
    shared_ptr<PairInfo> currentPair = m_pairs[m_currentIndex];
    {
        // 修改 pair 状态必须在 stateMutex 里;m_showingA 决定的是"显示哪张",不影响这里
        std::lock_guard<std::mutex> lkState(currentPair->stateMutex);
        applyActionToPair(currentPair, type, &changed);
    }
    /*
     * 单对处理的提示音(短促的 pop)。
     *
     * 用户要的是"闭着眼也知道这一下生效了":单对 -> pop,多选批量 -> bubble pop,
     * 两种声音明显不同,不用回去看列表就知道刚才那一下动了一对还是一片。
     * 放在"状态已经改完"之后 —— 被按钮锁/目录方向没定/筛选拦下的操作不会响。
     */
    dedupenv::playSound(DedupSound::SingleAction);
    const qint64 tApply = tm.elapsed();

    finalizePairStateChangeRows(changed, DuplicatePairListView::ScrollNever);
    const qint64 tList = tm.elapsed();

    /*
     * ===== 产生了后台 rename 就先别走 =====
     *
     * 这一步真的往队列里放了文件操作(remove 某一侧 / jump 时恢复某一侧),
     * 说明磁盘上还没定。等它回来再前进:成功 -> 下一个 Pending;
     * 失败 -> 状态回滚,人留在原地,由 handleFileOpResult 播错误音并提示重试/跳过。
     *
     * 没有文件操作的一步(这一侧早就在 duplicate 里、moveToDuplicate 直接受理)
     * 不受影响,照旧立刻前进。
     */
    if (holdAdvanceUntilFileOpsSettle(currentPair)) {
        logLine(QString("[%1] User %2 (button), pair %3:已提交磁盘操作,等落盘后再前进")
                    .arg(nowStr(), actionName(type)).arg(actedIndex));
        const qint64 total2 = tm.elapsed();
        if (total2 > 150) {
            logLine(QStringLiteral("[慢操作] %1 pair=%2 总%3ms | 应用%4 列表刷新%5 (等磁盘)")
                        .arg(actionName(type)).arg(actedIndex).arg(total2)
                        .arg(tApply).arg(tList - tApply), true);
        }
        return;
    }

    const int nextIndex = findNextPending();
    const qint64 tFind = tm.elapsed();

    if (nextIndex >= 0 && nextIndex < (int)m_pairs.size())
        displayPairByIndex(nextIndex, m_dataEpoch);
    else
        showNoMorePairs();
    const qint64 tDisplay = tm.elapsed() - tFind;

    logLine(QString("[%1] User %2 (button), pair %3 -> %4")
                .arg(nowStr(), actionName(type)).arg(actedIndex).arg(nextIndex));

    const qint64 total = tm.elapsed();
    if (total > 150) {
        logLine(QStringLiteral("[慢操作] %1 pair=%2 总%3ms | 应用%4 列表刷新%5 查找下一个%6 切图%7")
                    .arg(actionName(type)).arg(actedIndex).arg(total)
                    .arg(tApply).arg(tList - tApply).arg(tFind - tList)
                    .arg(tDisplay), true);
    }
}

void ImageDeduplicateWindow::dispatchPairAction(ActionRecord::Type type)
{
    /*
     * 按钮 / 快捷键(a/s/d/空格)的总入口:先看列表选了几对。
     *
     * 以前这两条入口只认 m_currentIndex —— 用户在列表里框选了一片,
     * 按 remove A 只有屏幕上那一对被处理,选区等于白选;要批量只能右键菜单。
     * 现在:
     *   · 选中 > 1 对 -> 对整个选区批量处理(和右键批量菜单同一个实现),
     *                    播"批量"音效(bubble pop);
     *   · 选中 0/1 对 -> 原来那条单对路径(只处理当前显示的那一对),
     *                    播"单对"音效(pop)。
     */
    if (m_pairActionButtonsLocked) {
        // 新图还没上屏:和单对路径同一条保护,提示语由它给(见 applyActionToCurrentPair)
        applyActionToCurrentPair(type);
        return;
    }

    const QVector<int> selected = m_pairList ? m_pairList->selectedPairIndices()
                                            : QVector<int>();
    if (selected.size() > 1) {
        /*
         * 筛选状态有自己的批量语义(只碰"另一侧"),走 filterBatchAction;
         * 不要在筛选里叫 applyBatchAction —— 它会拒绝并要求退出筛选。
         */
        if (m_filter.active)
            filterBatchAction(selected, type);
        else
            applyBatchAction(selected, type);
        return;
    }

    applyActionToCurrentPair(type);
}

void ImageDeduplicateWindow::goToNextPair()
{
    /*
     * next:保持当前这一对现有的操作(Pending 也算"什么都不做"),
     * 光标移到下一个 **Pending** 的对。
     *
     * 只认 Pending(2026-10-28 修正):以前是"下一个非 Invalid 的对",
     * 于是已经 remove 过的、jump 掉的、筛选里 finished 的行都会被停一遍 ——
     * 用户按 next 是想找活干,不是想复查已经处理完的行。
     * (想复查就到列表里点,或按 previous 往回走。)
     *
     * 为什么需要它:用户跳回很前面的某一对、看完不想改任何东西时,
     * 以前只能把原来那个动作(remove A / remove B / …)再点一遍才能前进,
     * 那等于对一个已经处理完的文件再下一次手 —— 又危险又反直觉。
     */
    std::shared_lock<std::shared_mutex> lk(m_mutex);
    MMutexScope mScope(g_iwdMMutexDepth);
    std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
    if (m_pairs.empty())
        return;

    /*
     * 筛选模式:next 只在筛选集合里走,而且只停在"还能处理"的成员上
     * (另一侧已经删掉的行没有意义)。
     */
    if (m_filter.active) {
        const int pos = filterMemberPos(m_currentIndex);
        const int next = nextActionableMember(pos, +1);
        if (next < 0) {
            createFloatingLabel(this, QStringLiteral("筛选里没有下一个可处理项了"));
            return;
        }
        displayPairByIndex(m_filter.members[next].pairIndex, m_dataEpoch);
        updateFilterUi();
        return;
    }

    // m_currentIndex 可能等于 m_pairs.size()(所有对都处理完),从那里往后就是没得走
    int i = (m_currentIndex < 0) ? 0 : m_currentIndex + 1;
    for (; i < (int)m_pairs.size(); ++i) {
        if (!m_pairs[i]) continue;
        std::lock_guard<std::mutex> lkState(m_pairs[i]->stateMutex);
        /*
         * 只停在 Pending:已经是 RemovedA/B/Both、Skipped、Invalid 的对都不算"下一个"。
         * (之前的判断是"非 Invalid",于是处理完的对会被一遍遍停下来。)
         */
        if (m_pairs[i]->state == PairState::Pending)
            break;
    }

    if (i >= (int)m_pairs.size()) {
        logLine(QString("[%1] next: 后面没有待处理(pending)的对了").arg(nowStr()));
        createFloatingLabel(this, QStringLiteral("后面没有待处理的对"));
        return;
    }
    displayPairByIndex(i, m_dataEpoch);
    logLine(QString("[%1] next -> pair %2(pending)").arg(nowStr()).arg(i));
}

void ImageDeduplicateWindow::goToNextFolderRegionPair()
{
    /*
     * 目录去重模式的 next 变体:pair 的排序是"目录对比区域"优先的
     * (排序第一层按 pairDirSortKey 聚簇),普通 next 只往后挪一对,
     * 同一区域里还剩一串同目录对时要按很多次才能到下一个目录。
     *
     * 落点规则(下一个区域的**第一条 Pending**)在 folderregionjump.h 里,
     * 那里是纯函数、有单测;这里只负责把 m_pairs 喂成 (区域键, 状态, 失效位)。
     * 区域判定必须用和排序**同一个**键函数 pairDirSortKey —— 排序怎么聚簇,
     * 这里就怎么认区域,否则会跳进"列表里还连在一起"的别的区域。
     */
    if (!isFolderCompareMode()) {
        createFloatingLabel(this, QStringLiteral("只有目录去重(文件夹对比)模式才有目录对比区域"));
        return;
    }

    std::shared_lock<std::shared_mutex> lk(m_mutex);
    MMutexScope mScope(g_iwdMMutexDepth);
    std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
    if (m_pairs.empty())
        return;

    if (m_filter.active) {
        createFloatingLabel(this, QStringLiteral("筛选进行中,先点 finished 退出"));
        return;
    }

    /* 按需读一行(跳一次最多读一遍目标前的那几行),不预先算整表区域键 */
    auto rowAt = [this](int i) -> folderregion::Row {
        folderregion::Row r;
        if (i < 0 || i >= (int)m_pairs.size() || !m_pairs[i])
            return r;                       // 空行:region 留空
        const std::shared_ptr<PairInfo> &p = m_pairs[i];
        int ia = -1, ib = -1;
        {
            std::lock_guard<std::mutex> lkState(p->stateMutex);
            ia = p->idxA;
            ib = p->idxB;
            r.state       = p->state;
            r.invalidMask = p->invalidMask;
        }
        r.region = pairDirSortKey(ia, ib);
        return r;
    };

    const int i = folderregion::nextPendingRow((int)m_pairs.size(), rowAt, m_currentIndex);

    if (i < 0) {
        logLine(QString("[%1] next-folder: 后面没有别的目录对比区域了").arg(nowStr()));
        createFloatingLabel(this, QStringLiteral("后面没有别的目录对比区域了"));
        return;
    }
    displayPairByIndex(i, m_dataEpoch);
    logLine(QString("[%1] next-folder -> pair %2(下一个目录对比区域)").arg(nowStr()).arg(i));
}

void ImageDeduplicateWindow::goToLastFolderRegionPair()
{
    /*
     * goToNextFolderRegionPair 的反向版,落点规则在 folderregionjump.h:
     *   next: 下一个区域的**第一条 Pending**;
     *   last: 上一个区域的**最靠下的一条有效对**(不要求 Pending)。
     *
     * 为什么是"最靠下"而不是区域开头:从后往前过的时候,用户想看的是这个区域
     * 尾部那几对(通常是最新扫进来的),而不是又回到区域开头。
     *
     * 那个区域一对有效对都没有(全被删掉/全部失效)时,退回到它最靠下的那一行 ——
     * 宁可停在一条失效对上,也不能"按了键原地不动":用户会以为快捷键没接上。
     */
    if (!isFolderCompareMode()) {
        createFloatingLabel(this, QStringLiteral("只有目录去重(文件夹对比)模式才有目录对比区域"));
        return;
    }

    std::shared_lock<std::shared_mutex> lk(m_mutex);
    MMutexScope mScope(g_iwdMMutexDepth);
    std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
    if (m_pairs.empty())
        return;

    if (m_filter.active) {
        createFloatingLabel(this, QStringLiteral("筛选进行中,先点 finished 退出"));
        return;
    }

    /* 区域键必须用和排序同一个函数,否则会跳进"列表里还连在一起"的别的区域 */
    auto rowAt = [this](int i) -> folderregion::Row {
        folderregion::Row r;
        if (i < 0 || i >= (int)m_pairs.size() || !m_pairs[i])
            return r;
        const std::shared_ptr<PairInfo> &p = m_pairs[i];
        int ia = -1, ib = -1;
        {
            std::lock_guard<std::mutex> lkState(p->stateMutex);
            ia = p->idxA;
            ib = p->idxB;
            r.state       = p->state;
            r.invalidMask = p->invalidMask;
        }
        r.region = pairDirSortKey(ia, ib);
        return r;
    };

    const int target = folderregion::lastValidRowOfPreviousRegion((int)m_pairs.size(),
                                                                 rowAt, m_currentIndex);
    if (target < 0) {
        logLine(QString("[%1] last-folder: 前面没有别的目录对比区域了").arg(nowStr()));
        createFloatingLabel(this, QStringLiteral("前面没有别的目录对比区域了"));
        return;
    }

    const folderregion::Row landed = rowAt(target);
    displayPairByIndex(target, m_dataEpoch);
    logLine(QString("[%1] last-folder -> pair %2(上一个区域的最后一个%3)")
                .arg(nowStr()).arg(target)
                .arg(pairLooksInvalid(landed.state, landed.invalidMask)
                         ? QStringLiteral("pair(该区域没有有效对)")
                         : QStringLiteral("有效对")));
}

void ImageDeduplicateWindow::goToPreviousPair()
{
    std::shared_lock<std::shared_mutex> lk(m_mutex);
    MMutexScope mScope(g_iwdMMutexDepth);
    std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
    if (m_pairs.empty())
        return;

    if (m_filter.active) {
        int pos = filterMemberPos(m_currentIndex);
        if (pos < 0) pos = (int)m_filter.members.size();
        const int prev = nextActionableMember(pos, -1);
        if (prev < 0) {
            createFloatingLabel(this, QStringLiteral("筛选里没有上一个可处理项了"));
            return;
        }
        displayPairByIndex(m_filter.members[prev].pairIndex, m_dataEpoch);
        updateFilterUi();
        return;
    }

    if (m_currentIndex == 0) {
        logLine(QString("[%1] No previous pair available").arg(nowStr()));
        return;
    }
    if (m_currentIndex < 0) {
        logLine(QString("[%1] No previous pair available").arg(nowStr()));
        return;
    }

    // 越界(所有pair都处理完时 m_currentIndex == m_pairs.size())也要能回到最后一组
    int i = qMin(m_currentIndex, (int)m_pairs.size()) - 1;
    for (; i >= 0; --i) {
        std::lock_guard<std::mutex> lkState(m_pairs[i]->stateMutex);
        if (!pairLooksInvalid(m_pairs[i]->state, m_pairs[i]->invalidMask))
            break;
    }

    if (i < 0) {
        logLine(QString("[%1] No valid previous pair found").arg(nowStr()));
        return;
    }
    displayPairByIndex(i, m_dataEpoch);
    logLine(QString("[%1] Previous: returned to pair %2").arg(nowStr()).arg(i));
}

void ImageDeduplicateWindow::on_btn_rm_a_clicked()
{
    /*
     * 筛选状态下这个按钮已经改成 "finished"(见 updateFilterUi):
     * 点它退出筛选。注意退出时机**完全由用户决定**,不会因为
     * "筛选项都处理完了"自动结束 —— 用户可能还要回头改主意。
     */
    if (m_filter.active) {
        finishSimilarityFilter();
        return;
    }
    dispatchPairAction(ActionRecord::RemoveA);
}

void ImageDeduplicateWindow::on_btn_rm_b_clicked()
{
    dispatchPairAction(ActionRecord::RemoveB);
}

void ImageDeduplicateWindow::on_btn_rm_both_clicked()
{
    dispatchPairAction(ActionRecord::RemoveBoth);
}

void ImageDeduplicateWindow::on_btn_jmp_clicked()
{
    dispatchPairAction(ActionRecord::Skip);
}


void ImageDeduplicateWindow::on_btn_auto_rm_clicked()
{
    if (blockedByFolderDecision(QStringLiteral("auto remove")))
        return;
    if (m_filter.active) {
        createFloatingLabel(this, QStringLiteral("筛选状态下不能用 auto remove"));
        logLine(QStringLiteral("[%1] 筛选状态下忽略 auto remove").arg(nowStr()));
        return;
    }

    std::unique_lock<std::shared_mutex> lk(m_mutex);
    /*
     * 必须登记“本线程已持有 m_mutex”。
     * 这个函数全程持排他锁,而后面会调用 displayPairByIndex()/finalizePairStateChange()
     * 等内部会按需加共享锁的函数;如果不登记,它们就会以为没持锁,
     * 在排他锁下再去拿共享锁 —— 同线程递归加锁,Windows SRWLock 上直接死锁。
     */
    MMutexScope mScope(g_iwdMMutexDepth);

    std::vector<int> candidates;
    for (int i = 0; i < (int)m_pairs.size(); ++i) {
        std::shared_ptr<PairInfo> p = m_pairs[i];
        if (!p) continue;

        {
            std::lock_guard<std::mutex> lkState(p->stateMutex);
            if (p->state != PairState::Pending) continue;
            if (!p->pixelRatioComputed) continue;
            if (p->pixelDiffRatio > m_pixdiffThreshold) continue;
        }

        candidates.push_back(i);
    }

    int autoCount = 0;
    bool ignoreSizeOption = ui.cb_IgnoreSize->isChecked(); // <-- 新：忽略文件大小比较的开关
    /*
     * 像素完全相同那批走了单独的判决(见下面的 pixelIdentical 分支),
     * 分开记账才能在日志里一眼看出"这次到底按哪条规则删的"。
     */
    int pixKeepSmaller = 0;    // 留小删大
    int pixFixedRemoveB = 0;   // Ignore Size 勾选后固定删 B
    int pixEqualKeptA = 0;     // 连大小都一样 -> 留 A 删 B

    for (int idx : candidates) {
        if (idx < 0 || idx >= (int)m_pairs.size()) continue;
        shared_ptr<PairInfo> p= m_pairs[idx];
        if (!p) continue;

        /*
         * 下标越界检查:FileEntry 数组与 pair 的生命周期由 epoch 管理,
         * 但一次 stale 的 idx 就足以让 Debug STL 在下标访问处直接 abort。
         * 这里宁可跳过这一对并记日志,也不要带着非法下标去改状态。
         */
        if (p->idxA < 0 || p->idxA >= (int)m_files.size()
            || p->idxB < 0 || p->idxB >= (int)m_files.size()) {
            logLine(QString("[%1] skipping pair: 文件下标越界 idxA=%2 idxB=%3 files=%4")
                        .arg(nowStr()).arg(p->idxA).arg(p->idxB).arg(m_files.size()), true, 1);
            continue;
        }

        {
            std::lock_guard<std::mutex> lkState(p->stateMutex);
            if (p->state != PairState::Pending) {
                continue;
            }
        }
        std::lock_guard<std::mutex> lkState(p->stateMutex);
        FileEntry&A = m_files[p->idxA], &B = m_files[p->idxB];

        QFileInfo fiA(A.currentPath), fiB(B.currentPath);
        /*
         * 有在途移动操作的文件不能按"磁盘上不存在"处理:
         * 它的路径正被队列推进(比如刚入队要搬进 duplicate,磁盘上还没动),
         * 拿磁盘状态判断会把这一对误跳过。
         */
        const bool missingA = !hasPendingFileOp(p->idxA) && !fiA.exists();
        const bool missingB = !hasPendingFileOp(p->idxB) && !fiB.exists();
        if (missingA || missingB) {
            logLine(QString("[%1] skipping pair %2/%3 because one file no longer exists")
                        .arg(nowStr()).arg(A.currentPath).arg(B.currentPath), true, 1);
            continue;
        }
        if (A.inDuplicate || B.inDuplicate) {
            logLine(QString("[%1] skipping pair %2/%3 because one side already in duplicate")
                        .arg(nowStr()).arg(A.currentPath).arg(B.currentPath), true, 1);
            continue;
        }

        int rA = qualityRank(A);
        int rB = qualityRank(B);

        // --- 决策：以 folderPriority 为主，倾向删除来自更靠后的 folder 的图片 ---
        ActionRecord::Type toDo = ActionRecord::None;
        const FileEntry *keep = nullptr;
        const FileEntry *remove = nullptr;
        bool removeIsA = false;

        /*
         * 像素完全相同(pix diff == 0)是独立的一套判决,优先级是
         *
         *     文件大小  >  合集方向 / 目录顺序
         *
         * 为什么:解码出来每个像素都一样,说明两边的内容没有任何差别。这时文件
         * 更大只可能是带了冗余信息(内嵌缩略图、EXIF/ICC、多余的块)或者被重新
         * 编码过 —— 留大的那一份纯粹白占空间,删掉大的才是不损失任何东西的选择。
         * 所以只要文件大小分得出胜负就留小的;连大小都一样(两边真的完全相同)
         * 才退回"留 A 删 B"。
         *
         * 勾了 Ignore Size 则相反:固定删右侧 B —— 这个开关在这个分支里的含义
         * 是"别跟我比大小,一律删 B",连合集方向也不照顾(真冲突会写日志)。
         *
         * 这里也【不】取 qualityRank:像素都一致了,格式档位没有任何意义
         * (典型例子就是无损重编码出来的 PNG 比原 JPEG 大一截)。
         */
        const bool pixelIdentical = p->pixelRatioComputed && p->pixelDiffRatio <= 0.0;
        const QString folderKeep = folderKeepDirFor(p->idxA, p->idxB);

        enum class PixIdenticalMode { None, KeepSmaller, FixedB, EqualKeepA };
        PixIdenticalMode pixMode = PixIdenticalMode::None;

        if (pixelIdentical) {
            keep = &A; remove = &B; removeIsA = false; toDo = ActionRecord::RemoveB;
            if (ignoreSizeOption) {
                pixMode = PixIdenticalMode::FixedB;
                if (!folderKeep.isEmpty() && dedup::isUnderOrEqual(B.currentPath, folderKeep)) {
                    logLine(QString("[%1] 像素完全相同: Ignore Size 固定删 B,而 B 在合集侧 %2 下 —— 按开关意图照删")
                                .arg(nowStr()).arg(folderKeep), true);
                }
            } else if (A.filesize != B.filesize) {
                // 留小的:大的那份是冗余/重编码的结果
                pixMode = PixIdenticalMode::KeepSmaller;
                if (A.filesize > B.filesize) {
                    keep = &B; remove = &A; removeIsA = true; toDo = ActionRecord::RemoveA;
                }
            } else {
                pixMode = PixIdenticalMode::EqualKeepA;   // 真的完全相同:留 A 删 B
            }
        } else {
            /*
             * 合集优先:同一输入目录(半勾选)的两个顶层子目录已经定过方向时,
             * 必须保留合集侧 —— 否则 auto remove 会把合集拆散,和目录判定的初衷相反。
             */
            if (!folderKeep.isEmpty()) {
                if (dedup::isUnderOrEqual(A.currentPath, folderKeep)) {
                    keep = &A; remove = &B; removeIsA = false; toDo = ActionRecord::RemoveB;
                } else if (dedup::isUnderOrEqual(B.currentPath, folderKeep)) {
                    keep = &B; remove = &A; removeIsA = true; toDo = ActionRecord::RemoveA;
                }
            }

            if (toDo == ActionRecord::None && A.folderPriority != B.folderPriority) {
                if (A.folderPriority < B.folderPriority) {
                    keep = &A; remove = &B; removeIsA = false; toDo = ActionRecord::RemoveB;
                } else {
                    keep = &B; remove = &A; removeIsA = true; toDo = ActionRecord::RemoveA;
                }
            } else if (toDo == ActionRecord::None) {
                if (rA > rB) { keep = &A; remove = &B; removeIsA = false; toDo = ActionRecord::RemoveB; }
                else if (rB > rA) { keep = &B; remove = &A; removeIsA = true; toDo = ActionRecord::RemoveA; }
                else {
                    long areaA = (long)A.size.width() * A.size.height();
                    long areaB = (long)B.size.width() * B.size.height();
                    if (areaA > areaB) { keep = &A; remove = &B; removeIsA = false; toDo = ActionRecord::RemoveB; }
                    else if (areaB > areaA) { keep = &B; remove = &A; removeIsA = true; toDo = ActionRecord::RemoveA; }
                    else {
                        if (A.filesize >= B.filesize) { keep = &A; remove = &B; removeIsA = false; toDo = ActionRecord::RemoveB; }
                        else { keep = &B; remove = &A; removeIsA = true; toDo = ActionRecord::RemoveA; }
                    }
                }
            }
        }

        bool shouldSkip = false;
        if (pixelIdentical) {
            /*
             * 像素相同的一对不过任何"质量兜底":
             *   面积必然一样(像素级一致);
             *   格式档位没有意义(同上);
             *   而"要删的比留的大"正是这个分支的既定意图(留小删大),
             *   再让下面那条 filesize 保护规则拦一道就等于这条规则从没生效。
             */
        } else {
            long removeArea = (long)remove->size.width() * remove->size.height();
            long keepArea = (long)keep->size.width() * keep->size.height();

            // 如果用户没有勾选忽略文件大小，则保留原来的“文件大小更大则跳过”逻辑
            if (!ignoreSizeOption && remove->filesize > keep->filesize) {
                logLine(QString("[%1] skipping auto-remove pair (%2 vs %3): remove candidate larger filesize (%4 > %5)")
                            .arg(nowStr()).arg(A.currentPath).arg(B.currentPath)
                            .arg(remove->filesize).arg(keep->filesize), true);
                shouldSkip = true;
            } else if (removeArea > keepArea) {
                logLine(QString("[%1] skipping auto-remove pair (%2 vs %3): remove candidate larger pixel dimensions (%4x%5 > %6x%7)")
                            .arg(nowStr()).arg(A.currentPath).arg(B.currentPath)
                            .arg(remove->size.width()).arg(remove->size.height())
                            .arg(keep->size.width()).arg(keep->size.height()), true);
                shouldSkip = true;
            } else if (qualityRank(*remove) > qualityRank(*keep)) {
                logLine(QString("[%1] skipping auto-remove pair (%2 vs %3): remove candidate has higher format rank (%4 > %5)")
                            .arg(nowStr()).arg(A.currentPath).arg(B.currentPath)
                            .arg(qualityRank(*remove)).arg(qualityRank(*keep)), true);
                shouldSkip = true;
            }
        }

        if (shouldSkip) continue;

        applyActionToPair(p, toDo);
        autoCount++;

        if (pixMode == PixIdenticalMode::KeepSmaller) ++pixKeepSmaller;
        else if (pixMode == PixIdenticalMode::FixedB) ++pixFixedRemoveB;
        else if (pixMode == PixIdenticalMode::EqualKeepA) ++pixEqualKeptA;

        // 注意：移除的简洁日志由 applyActionToPair 负责输出（格式：rm A/B at <path>）
    }

    /*
     * auto remove 的提示音,按"实际移走了几对"分三种(用户要求):
     *   多对   -> 批量音(bubble pop)
     *   一对   -> 单对音(pop)
     *   一对都没移走 -> 麻将音(sfxmint mahjong),明确告诉用户"按钮响了但什么都没发生",
     *                  否则面对一屏 Pending 用户会以为按钮没响应。
     * 注意用 autoCount 而不是 candidates.size():候选里可能有被"删的比留的大"这类
     * 保护规则跳过的。
     */
    if (autoCount > 1)
        dedupenv::playSound(DedupSound::BatchAction);
    else if (autoCount == 1)
        dedupenv::playSound(DedupSound::SingleAction);
    else
        dedupenv::playSound(DedupSound::NothingRemoved);

    std::lock_guard<std::mutex> lkIndex(currentIndexMutex);

    logLine(QString("[%1] Auto-removed %2 pairs").arg(nowStr()).arg(autoCount), true);

    if (pixKeepSmaller > 0 || pixFixedRemoveB > 0 || pixEqualKeptA > 0) {
        logLine(QString("[%1] 其中像素完全相同: %2 对留小删大, %3 对因 Ignore Size 固定删 B, "
                        "%4 对大小完全一致留 A 删 B")
                    .arg(nowStr()).arg(pixKeepSmaller).arg(pixFixedRemoveB).arg(pixEqualKeptA));
    }

    /*
     * 批量改过状态(含连带Invalid),整体刷新。
     * 用 ScrollNever:滚动位置不在这里定,交给下面统一判断要不要前进。
     */
    finalizePairStateChange(DuplicatePairListView::ScrollNever);

    /*
     * 自动去重后如果当前显示的这对也不在 Pending 了(自己被去重,或它的 A/B 被
     * 别的 pair 移除而连带 Invalid),就跳到下一个 Pending —— 而不是把一对已经
     * 处理完的图继续留在屏幕上,让用户手动再点一次才能走。
     */
    advanceToNextPendingAfterBatch();
}


void ImageDeduplicateWindow::on_btn_clear_clicked()
{
    ui.pte_log->clear();
}

void ImageDeduplicateWindow::on_sb_pixdiff_valueChanged(double v)
{
    m_pixdiffThreshold = v;
}

void ImageDeduplicateWindow::on_sb_area_valueChanged(double v)
{
    m_areaThreshold = v;
}

///////////////////////////
// highlight timer
///////////////////////////
bool ImageDeduplicateWindow::eventFilter(QObject *obj, QEvent *event) {
    if((obj == ui.lb_path_a||obj== ui.lb_path_b)&& event->type() == QEvent::MouseButtonPress){
        /*
         * 左键:在资源管理器中定位。
         * 右键:复制完整路径 —— 和 clear file 的冲突窗口统一,
         * 所有"显示路径的 label"都是这个规矩。
         */
        auto *me = static_cast<QMouseEvent *>(event);
        auto *lb = qobject_cast<QLabel *>(obj);
        if (lb && me->button() == Qt::RightButton) {
            QApplication::clipboard()->setText(lb->text());
            return true;
        }
        dedupenv::revealFileInExplorer(qobject_cast<QLabel*>(obj)->text());
        return true;
    }
    /*
     * 只读日志框会把 ←/→ 当成"移动光标"吃掉,于是焦点落在日志上时
     * previous/next 快捷键失效。这里把这两个键转给窗口处理,其余按键照旧。
     * (Ctrl+A 仍然留给日志自己:复制一整段日志是那个控件的正常用途。)
     */
    if (obj == ui.pte_log && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->modifiers() == Qt::NoModifier
            && (ke->key() == Qt::Key_Left || ke->key() == Qt::Key_Right)) {
            if (ke->key() == Qt::Key_Left) goToPreviousPair();
            else                           goToNextPair();
            return true;
        }
    }
    /*
     * 列表 / 大图控件上的导航键:**在控件自己之前**统一拦下来。
     *
     * 以前靠"控件 ignore -> 事件沿父链冒泡到窗口"这条隐式路径,行为取决于
     * 控件基类有没有先消费掉这个键(QListView 对 ←/→ 就会消费)。于是焦点一进列表,
     * "按右键跳下一对"就时灵时不灵。这里显式拦截,和窗口 keyPressEvent 共用
     * 同一份判定(handleNavigationShortcut),三处(窗口/列表/大图)语义完全一致。
     */
    if ((obj == m_pairList || obj == m_display) && event->type() == QEvent::KeyPress) {
        if (handleNavigationShortcut(static_cast<QKeyEvent *>(event)))
            return true;
    }
    return QObject::eventFilter(obj, event);
}

QString pairStateToString(PairState state) {
    switch (state ) {
    case Pending:       return "Pending";
    case RemovedA:      return "RemovedA";
    case RemovedB:      return "RemovedB";
    case RemovedBoth:   return "RemovedBoth";
    case Skipped:       return "Skipped";
    case InvalidA:      return "InvalidA";
    case InvalidB:      return "InvalidB";
    case InvalidBoth:   return "InvalidBoth";
    default:            return "Unknown";
    }
}

/* ==================================================================
 *                 DuplicatePairListView 交互实现
 * ================================================================== */

const char *pairStateName(PairState state)
{
    switch (state) {
    case PairState::Pending:      return "Pending";
    case PairState::RemovedA:     return "RemovedA";
    case PairState::RemovedB:     return "RemovedB";
    case PairState::RemovedBoth:  return "RemovedBoth";
    case PairState::Skipped:      return "Skipped";
    case PairState::InvalidA:     return "InvalidA";
    case PairState::InvalidB:     return "InvalidB";
    case PairState::InvalidBoth:  return "InvalidBoth";
    default:                      return "Unknown";
    }
}

void ImageDeduplicateWindow::onPairActivated(int pairIndex)
{
    /*
     * 【临时探针】排查"列表选中变了、大图没跟"：
     * 这个槽有三条静默 return 路径,以前一条日志都不留,现场无法分辨是谁吞的。
     */
    dedup::traceShift(QStringLiteral("ACT pair=%1 suppress=%2 filter=%3")
                          .arg(pairIndex).arg(m_suppressListActivation ? 1 : 0)
                          .arg(m_filter.active ? 1 : 0));

    // 程序性选中(例如跳转后同步列表)不要再反过来触发跳转
    if (m_suppressListActivation) {
        dedup::traceShift(QStringLiteral("ACT DROP pair=%1: suppress 仍为 true").arg(pairIndex));
        return;
    }
    if (pairIndex < 0) {
        dedup::traceShift(QStringLiteral("ACT DROP: pairIndex<0"));
        return;
    }

    {
        std::shared_lock<std::shared_mutex> lk(m_mutex);
        if (pairIndex >= (int)m_pairs.size()) {
            dedup::traceShift(QStringLiteral("ACT DROP pair=%1: 越界 size=%2")
                                  .arg(pairIndex).arg(m_pairs.size()));
            return;
        }
        /*
         * 筛选集合里的行就算写着 Invalid 也要能点开 ——
         * 那些行正是"因为锚点失效、被临时恢复来清理"的,
         * 点一下却跳到肇事那一对,筛选就没法用了。
         */
        const bool inFilter = m_filter.active && filterMemberPos(pairIndex) >= 0;
        std::lock_guard<std::mutex> lkState(m_pairs[pairIndex]->stateMutex);
        if (!inFilter && isInvalidPairState(m_pairs[pairIndex]->state)) {
            /*
             * 【临时探针】这一条最关键：列表行显示 Pending、这里却是 Invalid 时,
             * 点击就被静默丢掉了 —— 表现正是"列表切了、大图没动"。
             */
            dedup::traceShift(QStringLiteral("ACT DROP pair=%1: 真实状态=%2 已失效(inFilter=%3)")
                                  .arg(pairIndex).arg(int(m_pairs[pairIndex]->state))
                                  .arg(inFilter ? 1 : 0));
            return;   // Invalid走另一个信号
        }
    }

    // 用户手选:可以直接跳到这个pair。
    // 注意用 ScrollNever —— 这一行是他刚点的、本来就在视野里,
    // 再让列表滚一下(尤其原来还是居中,会明显"跳一下")纯属干扰。
    std::shared_lock<std::shared_mutex> lk(m_mutex);
    std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
    if (pairIndex >= (int)m_pairs.size()) return;
    displayPairByIndex(pairIndex, m_dataEpoch, DuplicatePairListView::ScrollNever);
    logLine(QString("[%1] 列表跳转到 pair %2").arg(nowStr()).arg(pairIndex));
}

void ImageDeduplicateWindow::onInvalidPairActivated(int pairIndex, bool preferA)
{
    /* 【临时探针】同上:静默 return 必须先留痕 */
    dedup::traceShift(QStringLiteral("INV pair=%1 preferA=%2 suppress=%3")
                          .arg(pairIndex).arg(preferA ? 1 : 0)
                          .arg(m_suppressListActivation ? 1 : 0));
    if (m_suppressListActivation) return;
    if (pairIndex < 0) return;

    /*
     * 2026-11-05 按用户要求改:筛选里的失效行【也要】跳到肇事那一对。
     *
     * 以前这里特判"筛选内就留在原地",理由是那些行是临时恢复来清理的;
     * 但用户的实际需求是:看到一行 Invalid,先要知道"是谁把我搞失效的",
     * 跳过去才能改。留在原地只能看到一张已经被删掉的图,什么也做不了。
     * 筛选列表本身不会因为这次跳转而消失(m_filter 还在,点 finished 才退出)。
     */

    const int cause = findInvalidCausePair(pairIndex, preferA);
    if (cause < 0) {
        logLine(QString("[%1] pair %2 已经是Invalid,但找不到导致它失效的那一组"
                        "(可能是本次会话之前的操作造成的)")
                    .arg(nowStr()).arg(pairIndex), false, 3);
        return;
    }

    std::shared_lock<std::shared_mutex> lk(m_mutex);
    std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
    if (cause >= (int)m_pairs.size()) return;

    const bool causeIsA = (m_pairs[pairIndex] && preferA);
    displayPairByIndex(cause, m_dataEpoch);

    logLine(QString("[%1] pair %2 在图%3上已经失效,跳转到导致它失效的 pair %4")
                .arg(nowStr()).arg(pairIndex)
                .arg(causeIsA ? QStringLiteral("A") : QStringLiteral("B"))
                .arg(cause));
}

void ImageDeduplicateWindow::onPairListLazyLoad(const QVector<int> &pairIndices)
{
    loadPairListThumbnails(pairIndices);
}

void ImageDeduplicateWindow::onPairListBatchAction(const QVector<int> &pairIndices, int actionType)
{
    ActionRecord::Type type = ActionRecord::None;
    switch (static_cast<PairBatchAction>(actionType)) {
    case PairBatchAction::SetRemovedA:    type = ActionRecord::RemoveA;    break;
    case PairBatchAction::SetRemovedB:    type = ActionRecord::RemoveB;    break;
    case PairBatchAction::SetRemovedBoth: type = ActionRecord::RemoveBoth; break;
    case PairBatchAction::SetSkipped:     type = ActionRecord::Skip;       break;
    case PairBatchAction::SetPending:     type = ActionRecord::None;       break;
    }

    /*
     * 筛选状态走单独的批量通道。
     *
     * 普通批量会跨到筛选列表以外,而且 applyActionToPair 的 remove 语义是
     * "改主意时先把另一侧恢复回来" —— 在筛选里那等于把刚删掉的另一侧搬回来。
     * 所以这里只对"属于筛选集合"的行做筛选语义的动作,其余跳过。
     */
    if (m_filter.active) {
        filterBatchAction(pairIndices, type);
        return;
    }

    applyBatchAction(pairIndices, type);
}

void ImageDeduplicateWindow::onPairListPairAction(int pairIndex, int actionType)
{
    /*
     * 右键菜单里"只作用于这一对"的动作。
     *
     * 和批量通道(applyBatchAction)的区别:
     *   · 只动一对,所以可以走行级刷新,不必整表重建;
     *   · 如果动的正好是当前显示的那一对,处理完直接前进到下一个待处理对,
     *     跟按钮的行为保持一致(否则用户还得自己按 next)。
     */
    const PairBatchAction action = static_cast<PairBatchAction>(actionType);

    ActionRecord::Type type = ActionRecord::None;
    switch (action) {
    case PairBatchAction::SetRemovedA:    type = ActionRecord::RemoveA;    break;
    case PairBatchAction::SetRemovedB:    type = ActionRecord::RemoveB;    break;
    case PairBatchAction::SetRemovedBoth: type = ActionRecord::RemoveBoth; break;
    case PairBatchAction::SetSkipped:     type = ActionRecord::Skip;       break;
    case PairBatchAction::SetPending:     type = ActionRecord::None;       break;
    }

    QElapsedTimer tm;
    tm.start();

    /*
     * 筛选:动作语义不同,必须在拿 m_mutex 之前分流 ——
     * filterRemoveOtherSide 内部会 refreshFilterList()(整表重建),那需要自己拿锁,
     * 在已经持有 m_mutex 的情况下进去就是自己跟自己抢锁。
     */
    if (m_filter.active) {
        filterPairAction(pairIndex, type);
        return;
    }

    std::shared_lock<std::shared_mutex> lk(m_mutex);
    std::lock_guard<std::mutex> lkIndex(currentIndexMutex);

    if (blockedByFolderDecision(QStringLiteral("右键动作")))
        return;
    if (pairIndex < 0 || pairIndex >= (int)m_pairs.size())
        return;

    shared_ptr<PairInfo> p = m_pairs[pairIndex];
    if (!p) return;

    /*
     * 失效的对不允许再"移除"(它的文件已经被别的对处理过了);
     * 也不允许在这里恢复成 Pending —— 那等于把别的对的 remove 一起撤销。
     * 判定用 pairLooksInvalid:Skipped/Removed* 只要 invalidMask 非 0 也算失效。
     */
    bool invalid = false;
    int causeForMsg = -1;
    {
        std::lock_guard<std::mutex> lkState(p->stateMutex);
        invalid = pairLooksInvalid(p->state, p->invalidMask);
        causeForMsg = (p->invalidCauseForA > 0) ? p->invalidCauseForA - 1
                                               : p->invalidCauseForB - 1;
    }
    if (invalid) {
        /*
         * 失效对不等于"什么都不能做"。
         *
         * 失效只说明【某一侧】被别的对移走了(比如 InvalidB:B 没了,A 还在)。
         * 那么"移除 A"是一个完全独立的决定,应该允许 —— 结果是复合态
         * InvalidB + RemovedA(状态仍 InvalidB,叠加位记 A 是本对删的)。
         * 反过来 InvalidA 允许 remove B。要删的那一侧本来就不在,才拒绝。
         *
         * 不能走 applyActionToPair:它把 RemoveA 当成"改主意",会先把另一侧
         * 从 duplicate 恢复回来 —— 在失效对上那等于撤销肇事那一对的删除。
         * removeSideOfPair 才是"只删这一侧"的正确语义。
         */
        if (type == ActionRecord::RemoveA || type == ActionRecord::RemoveB) {
            const bool sideA = (type == ActionRecord::RemoveA);
            int fileIdx = -1;
            {
                std::lock_guard<std::mutex> lkState(p->stateMutex);
                fileIdx = sideA ? p->idxA : p->idxB;
            }
            const bool alive = fileIdx >= 0 && fileIdx < (int)m_files.size()
                               && !m_files[fileIdx].inDuplicate
                               && !m_files[fileIdx].currentPath.isEmpty();
            if (!alive) {
                createFloatingLabel(this, sideA
                                              ? QStringLiteral("A 侧已经进 duplicate,删不了")
                                              : QStringLiteral("B 侧已经进 duplicate,删不了"));
                return;
            }

            bool ok = false;
            QVector<int> changed;
            changed.push_back(pairIndex);
            {
                std::lock_guard<std::mutex> lkState(p->stateMutex);
                ok = removeSideOfPair(p, sideA, &changed);
            }
            if (ok) {
                finalizePairStateChangeRows(changed, DuplicatePairListView::ScrollNever);
                logLine(QStringLiteral("[%1] 失效对上移除%2 -> pair %3(复合态)")
                            .arg(nowStr(), sideA ? QStringLiteral("A") : QStringLiteral("B"))
                            .arg(pairIndex));
            } else {
                createFloatingLabel(this, QStringLiteral("移除失败(看日志)"));
            }
            return;
        }

        /* 恢复为 Pending 交给下面的公共路径:restorePairToPending 只撤销本对删过的图 */
        if (type != ActionRecord::None) {
            const int cause = findInvalidCausePair(pairIndex, /*preferA=*/true);
            createFloatingLabel(this, cause >= 0
                                          ? QStringLiteral("这一对已失效,请到 pair %1 上修改").arg(cause)
                                          : QStringLiteral("这一对已失效,先处理导致它失效的那一对"));
            logLine(QStringLiteral("[%1] 右键动作被拒: pair %2 已失效(原因 pair=%3)")
                        .arg(nowStr()).arg(pairIndex)
                        .arg(cause >= 0 ? QString::number(cause) : QStringLiteral("未知")), true);
            return;
        }
    }

    QVector<int> changed;
    changed.push_back(pairIndex);

    if (type == ActionRecord::None) {
        // restorePairToPending 内部自己拿 stateMutex,不能在外面先锁住
        if (!restorePairToPending(p))
            return;
        // 恢复会让"包含同一个文件"的其它 Invalid 行回到 Pending,行级刷新覆盖不到,
        // 但数量很少 —— 这里仍然整表刷一次,保证 Invalid/A-B 标记一定同步
        finalizePairStateChange(DuplicatePairListView::ScrollNever);
    } else {
        {
            std::lock_guard<std::mutex> lkState(p->stateMutex);
            applyActionToPair(p, type, &changed);
        }
        finalizePairStateChangeRows(changed, DuplicatePairListView::ScrollNever);
    }

    logLine(QStringLiteral("[%1] 右键 %2 -> pair %3")
                .arg(nowStr(), actionName(type)).arg(pairIndex));
    // 右键单对动作也是"一次单对处理":和按钮/快捷键用同一个 pop
    dedupenv::playSound(DedupSound::SingleAction);

    /*
     * 动的就是当前这一对,而且它已经不是 Pending 了 -> 前进到下一个待处理对。
     * "恢复为 Pending"不前进(用户就是要留在这里改主意)。
     */
    if (pairIndex == m_currentIndex && type != ActionRecord::None) {
        bool stillPending = false;
        {
            std::lock_guard<std::mutex> lkState(p->stateMutex);
            stillPending = (p->state == PairState::Pending);
        }
        if (!stillPending) {
            const int nextIndex = findNextPending();
            if (nextIndex >= 0 && nextIndex < (int)m_pairs.size())
                displayPairByIndex(nextIndex, m_dataEpoch);
            else
                showNoMorePairs();
        }
    }

    const qint64 total = tm.elapsed();
    if (total > 150) {
        logLine(QStringLiteral("[慢操作] 右键 %1 pair=%2 总%3ms")
                    .arg(actionName(type)).arg(pairIndex).arg(total), true);
    }
}

/* ==========================================================================
 *  资源文件(非图片)的二进制去重
 *  PSD/ZIP/GIF/MP4 等:二进制相同 -> 子集侧移入 duplicate;不同 -> 不视为重复。
 *  html/txt/json/tscch 属于辅助文件,直接忽略。
 * ========================================================================== */

static bool isAuxSuffix(const QString &s)
{
    return s == QLatin1String("html") || s == QLatin1String("htm")
        || s == QLatin1String("txt") || s == QLatin1String("json")
        || s == QLatin1String("tscch");
}

int ImageDeduplicateWindow::dedupeResourceFiles()
{
    if (m_folderKeep.isEmpty()) return 0;
    if (m_fileOpMode != FileOpMode::Immediate || m_importSessionActive) return 0;

    const QStringList imageExts = dedup::supportedExtensions();

    auto collect = [&](const QString &dir, QSet<QString> *out) {
        QDirIterator it(dir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const QFileInfo fi(path);
            if (fi.absolutePath().endsWith(QStringLiteral("/duplicate"), Qt::CaseInsensitive)
                || fi.absolutePath().endsWith(QStringLiteral("\\duplicate"), Qt::CaseInsensitive))
                continue;   // 已经在 duplicate 里的不再参与
            const QString sfx = fi.suffix().toLower();
            if (isAuxSuffix(sfx)) continue;           // 辅助文件忽略
            if (imageExts.contains(sfx)) continue;    // 图片交给图片通道
            out->insert(QDir::cleanPath(path));
        }
    };

    int moved = 0;
    for (auto it = m_folderKeep.constBegin(); it != m_folderKeep.constEnd(); ++it) {
        const QStringList parts = it.key().split(QLatin1Char('|'));
        if (parts.size() != 2) continue;

        const QString keepDir = it.value();
        const QString subDir = (parts[0] == keepDir) ? parts[1] : parts[0];
        if (keepDir.isEmpty() || subDir.isEmpty() || keepDir == subDir) continue;

        QSet<QString> keepFiles, subFiles;
        collect(keepDir, &keepFiles);
        collect(subDir, &subFiles);
        if (keepFiles.isEmpty() || subFiles.isEmpty()) continue;

        // 按大小分桶,只对同大小文件算哈希
        QHash<qint64, QStringList> keepBySize;
        for (const QString &f : std::as_const(keepFiles))
            keepBySize[QFileInfo(f).size()] << f;

        for (const QString &sf : std::as_const(subFiles)) {
            const qint64 size = QFileInfo(sf).size();
            const auto bucket = keepBySize.constFind(size);
            if (bucket == keepBySize.constEnd()) continue;

            QFile sfFile(sf);
            if (!sfFile.open(QIODevice::ReadOnly)) continue;
            const QByteArray subHash = QCryptographicHash::hash(sfFile.readAll(),
                                                                QCryptographicHash::Sha256);
            sfFile.close();

            bool same = false;
            for (const QString &kf : bucket.value()) {
                QFile kfFile(kf);
                if (!kfFile.open(QIODevice::ReadOnly)) continue;
                const QByteArray keepHash = QCryptographicHash::hash(kfFile.readAll(),
                                                                     QCryptographicHash::Sha256);
                kfFile.close();
                if (keepHash == subHash) { same = true; break; }
            }
            if (!same) continue;

            const QString dupDir = QDir(subDir).filePath(QStringLiteral("duplicate"));
            const dupmove::Result r = dupmove::moveIntoDuplicate(sf, dupDir);
            if (r.ok) {
                ++moved;
                logLine(QStringLiteral("[%1] 资源重复: %2 -> %3")
                            .arg(nowStr(), sf, r.targetPath));
            } else {
                logLine(QStringLiteral("[%1] 资源去重失败: %2").arg(nowStr(), r.error), true, 3);
            }
        }
    }
    return moved;
}

/* ==========================================================================
 *  完全重复图片:先按合集方向移入 duplicate,人工决策后再调整归属
 * ========================================================================== */

static QString dirPairKeyOf(const QString &a, const QString &b)
{
    /* 只有一份实现:见 folderscope::pairKey(方向/大小写/分隔符都规范化) */
    return folderscope::pairKey(a, b);
}

/*
 * 两张图是否**像素级完全相同**(不是"相似",是每一像素都相等)。
 *
 * 用在完全重复的最终判定上:哈希只能说明"文件字节曾经相同",
 * 而 tscch 里的哈希可能已经过期。这里走一遍生产的对齐 + 像素比较
 * (阈值 0.0),ratio == 0 才算真的一样。
 *
 * 解码失败时返回 false —— 宁可不动文件,也不能猜。
 */
void ImageDeduplicateWindow::autoMoveExactDuplicates()
{
    m_exactDupMoves.clear();
    m_autoDupFinalized = false;

    // 只在普通(立即)模式下做:Deferred 的文件必须留在原地供比较
    if (m_fileOpMode != FileOpMode::Immediate || m_importSessionActive)
        return;
    /*
     * 只处理"目录方向已确定"的完全重复。
     *
     * 没有合集关系(目录级判 None、或用户 jump)时**不自动移**:那属于
     * §6「单张相似图片对的智能排序」的范畴 —— 两张图确实一样,但留哪一份
     * 是用户的选择(可能按目录归属、按文件名、按自己的整理习惯)。
     * 这类对会正常建 pair 进列表,由用户 remove A / remove B。
     */
    if (m_exactDupCandidates.isEmpty())
        return;
    if (m_folderKeep.isEmpty()) {
        /*
         * 有字节相同的候选,但一个方向都没有(目录级判定为"无合集关系"、
         * 或者用户全部 jump 了)。这种情况按设计不自动移 —— 留哪一份是用户的事。
         * 但必须说出来:否则用户只会看到"完全重复一张都没移",不知道是没判定出来
         * 还是判定成"没有合集关系"。
         */
        logLine(QStringLiteral("[%1] 完全重复(字节相同): %2 对候选,但没有合集方向 -> 不自动移,留给用户逐对确认")
                    .arg(nowStr()).arg(m_exactDupCandidates.size()));
        return;
    }

    for (const QPair<int, int> &cand : std::as_const(m_exactDupCandidates)) {
        const int ia = cand.first;
        const int ib = cand.second;
        if (ia < 0 || ib < 0 || ia >= (int)m_files.size() || ib >= (int)m_files.size())
            continue;
        if (m_files[ia].inDuplicate || m_files[ib].inDuplicate)
            continue;

        const QString da = topDirPathOfFile(ia);
        const QString db = topDirPathOfFile(ib);
        const QString key = dirPairKeyOf(da, db);
        if (key.isEmpty())
            continue;

        const QString keep = m_folderKeep.value(key);
        if (keep.isEmpty())
            continue;   // 没有合集方向:留给 §6 的单张对比,用户自己选

        int keepIdx = -1, moveIdx = -1;
        bool keepIsA = true;
        if (da == keep) {
            keepIdx = ia; moveIdx = ib; keepIsA = true;
        } else if (db == keep) {
            keepIdx = ib; moveIdx = ia; keepIsA = false;
        } else {
            continue;
        }

        ExactDupMove mv;
        mv.dirKey = key;
        mv.keepIsA = keepIsA;
        mv.keepFileIdx = keepIdx;
        mv.movedFileIdx = moveIdx;

        // 直接移动文件:这一对不进图片列表,没有 pair 状态需要维护
        if (moveToDuplicate(moveIdx, nullptr)) {
            mv.moved = true;
            markRelatedPairsValidState(m_files[moveIdx], nullptr, false);
            m_exactDupMoves << mv;
        } else {
            logLine(QStringLiteral("[%1] 完全重复移动失败(保留原状): %2")
                        .arg(nowStr(), m_files[moveIdx].originalPath), true, 3);
        }
    }

    if (!m_exactDupMoves.isEmpty()) {
        logLine(QStringLiteral("[%1] 完全重复: %2 张已按合集方向移入 duplicate(不进图片比较列表)")
                    .arg(nowStr()).arg(m_exactDupMoves.size()));
    }
}

/*
 * 人工定完方向后,把"现在有方向"的完全重复对从列表里剔掉并搬走。
 *
 * 为什么需要这一步:建对发生在扫描阶段,那时人工目录对还没有方向,
 * 所以它们的完全重复对是**照常建对**的(留给用户选)。用户选完 keep A/B 之后,
 * 这些对就有了明确归属,再让它们留在列表里让用户逐张确认就重复了。
 *
 * 只处理 SHA256 相同(字节级)的对。
 *
 * 像素相同但字节不同的那种(pix==0,比如无损重编码)【不在这里】处理:它们的
 * 保留侧该由文件大小决定而不是合集方向(见 on_btn_auto_rm_clicked 的
 * pixelIdentical 分支),所以做成"用户的 auto remove 动作"统一走,不许程序
 * 自己按方向搬 —— 详见 finishFolderDecisionsAndBuildPairs 里的说明。
 */
int ImageDeduplicateWindow::pruneDirectedExactDuplicates()
{
    if (m_folderKeep.isEmpty())
        return 0;
    if (m_fileOpMode != FileOpMode::Immediate || m_importSessionActive)
        return 0;

    int moved = 0;
    std::shared_ptr<PairInfo> curPair;
    {
        std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
        if (m_currentIndex >= 0 && m_currentIndex < (int)m_pairs.size())
            curPair = m_pairs[m_currentIndex];
    }
    std::vector<std::shared_ptr<PairInfo>> kept;
    kept.reserve(m_pairs.size());

    for (const auto &pi : std::as_const(m_pairs)) {
        if (!pi) continue;

        int ia = -1, ib = -1;
        {
            std::lock_guard<std::mutex> lkState(pi->stateMutex);
            ia = pi->idxA;
            ib = pi->idxB;
        }
        if (ia < 0 || ib < 0 || ia >= (int)m_files.size() || ib >= (int)m_files.size()) {
            kept.push_back(pi);
            continue;
        }

        const QString keep = folderKeepDirFor(ia, ib);
        const bool exact = shaIsNonZero(m_files[ia])
                           && m_files[ia].sameContentAs(m_files[ib]);
        if (keep.isEmpty() || !exact) {
            kept.push_back(pi);    // 没方向(用户 jump)或不是字节相同 -> 留在列表里
            continue;
        }

        const QString da = topDirPathOfFile(ia);
        const QString db = topDirPathOfFile(ib);
        int keepIdx = -1, moveIdx = -1;
        bool keepIsA = true;
        if (da == keep) {
            keepIdx = ia; moveIdx = ib; keepIsA = true;
        } else if (db == keep) {
            keepIdx = ib; moveIdx = ia; keepIsA = false;
        } else {
            kept.push_back(pi);
            continue;
        }

        if (!moveToDuplicate(moveIdx, nullptr)) {
            kept.push_back(pi);    // 移不动就留着,不能让列表和磁盘不一致
            continue;
        }

        ExactDupMove mv;
        mv.dirKey = dirPairKeyOf(da, db);
        mv.keepIsA = keepIsA;
        mv.keepFileIdx = keepIdx;
        mv.movedFileIdx = moveIdx;
        mv.moved = true;
        m_exactDupMoves << mv;
        ++moved;

        // 引用同一个文件的其它对也要跟着失效(否则列表还显示 Pending、点进去却是空图)
        markRelatedPairsValidState(m_files[moveIdx], pi, false);
    }

    if (moved > 0) {
        m_pairs = std::move(kept);
        /*
         * 按下标记账的东西全变了,但"用户正在看哪一对"不该变:
         * 按 shared_ptr 找回它的新下标(找不到 = 当前这一对本身被剔掉了,
         * 只能交给调用方重新定位)。
         * 以前这里直接 m_currentIndex = -1,于是"定完目录方向"总会跳到别的位置。
         */
        std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
        int keepIndex = -1;
        if (curPair) {
            for (int i = 0; i < (int)m_pairs.size(); ++i) {
                if (m_pairs[i] == curPair) { keepIndex = i; break; }
            }
        }
        m_currentIndex = keepIndex;
        logLine(QStringLiteral("[%1] 方向确定后: %2 对完全重复已移入 duplicate(不再进入比较列表)")
                    .arg(nowStr()).arg(moved));
    }
    return moved;
}

/*
 * 【已取消】像素相同(pix==0)的自动搬移 —— 原 autoRemovePixelIdenticalPairs()
 *
 * 以前这里会在扫描完成 / 定完目录方向后,把 pix==0 的对直接按合集方向搬进
 * duplicate(理由是和 SHA256 相同的对属于同一级别的"完全重复")。
 *
 * 现在不这么做了:像素相同但字节不同的对,保留哪一侧要看**文件大小**
 * (大的那份只可能是带了冗余信息或被重编码过),而"按大小留"和"按合集方向留"
 * 在合集那份更大的时候会得出相反的结论 —— 程序替用户选,选错了这批对还会被
 * 移出比较列表,之后没有机会纠正。
 *
 * 所以这批对一律留在比较列表里,由用户的 auto remove 统一处理
 * (见 on_btn_auto_rm_clicked 的 pixelIdentical 分支):
 *   不勾 Ignore Size -> 留小的;勾了 Ignore Size -> 固定删 B(想保住合集就勾这个)。
 *
 * 这样处理掉的对 autoResolved=false,会被 finalizeExactDuplicateOwnership 里
 * "两个目录哪边移走得多"的统计算成**用户动作**;而哈希相同、程序按方向搬走的
 * 那批仍然单独记在 m_exactDupMoves 里,两边不混账。
 */
void ImageDeduplicateWindow::finalizeExactDuplicateOwnership(bool uiRefresh)
{
    if (m_autoDupFinalized || m_exactDupMoves.isEmpty())
        return;
    m_autoDupFinalized = true;

    /*
     * 触发时机:窗口关闭 / 进入下一轮(clearState)。调用方负责持有
     * m_mutex(共享)与 currentIndexMutex —— 两个入口都在这两把锁下进来,
     * 和原来 showNoMorePairs 里的调用保持一致。
     */

    /*
     * 统计口径:只算用户在 ImageDeduplicateWindow 里【人工】处理掉的对。
     * 完全重复那一批是程序按初始方向自动移走的,把它们算进来会让"删得多不多"
     * 变成程序自己跟自己的结果 —— 提示词 4.2 要的是人工决策带来的偏移。
     * (这批对现在压根不在 m_pairs 里,所以这里的统计天然只含人工对。)
     *
     * first = 合集/保留侧被移走数, second = 子集侧被移走数。
     */
    QHash<QString, QPair<int, int>> deleted;
    for (const shared_ptr<PairInfo> &p : std::as_const(m_pairs)) {
        if (!p) continue;
        int st = 0, ia = -1, ib = -1;
        bool autoResolved = false;
        quint8 mask = 0;
        {
            std::lock_guard<std::mutex> lkState(p->stateMutex);
            st = int(p->state);
            ia = p->idxA;
            ib = p->idxB;
            autoResolved = p->autoResolved;
            mask = p->removedMask;
        }
        /*
         * 叠加态(InvalidA + RemovedB)也要统计:那一侧的删除是用户亲手做的
         * (筛选里删掉"另一侧"),只是这一对同时因为锚点失效。
         * 以前只认 RemovedA/B/Both,这类动作会被整条漏掉。
         */
        const bool invalidPair = isInvalidPairState(PairState(st));
        if (invalidPair) {
            if (mask == 0) continue;        // 纯失效:本对没有任何动作
        } else if (st != RemovedA && st != RemovedB && st != RemovedBoth) {
            continue;
        }
        // 程序自动移走的不算"人工决策",见 PairInfo::autoResolved
        if (autoResolved) continue;
        if (ia < 0 || ib < 0 || ia >= (int)m_files.size() || ib >= (int)m_files.size())
            continue;

        const QString da = topDirPathOfFile(ia);
        const QString db = topDirPathOfFile(ib);
        const QString key = dirPairKeyOf(da, db);
        if (key.isEmpty()) continue;

        /*
         * 只统计"这个目录对本身"的人工决策。
         *
         * key 是由这一对文件各自所属的两个顶层目录拼出来的,所以 A↔B 的删除只会
         * 累加到 A↔B 名下,A↔C 的选择完全不会影响它 —— 这正是 4.2 要的
         * "按这两个文件夹之间的相似对谁移走的更少来决定归属"。
         *
         * 没有合集方向的目录对本来就不会有完全重复被自动移走(见
         * autoMoveExactDuplicates 的早退),所以这里没有 keep 也直接跳过。
         */
        const QString keep = m_folderKeep.value(key);
        if (keep.isEmpty()) continue;
        // 摆位之后合集侧在 A 位;按"合集侧/子集侧"而不是字面 A/B 记,避免被摆位影响
        const bool aIsKeep = (da == keep);

        auto &slot = deleted[key];
        auto bumpKeep = [&]() { ++slot.first; };
        auto bumpSub  = [&]() { ++slot.second; };

        if (invalidPair) {
            // 叠加态:按"本对删掉的是哪一侧"记账(位1=A、位2=B)
            if (mask & 1) { if (aIsKeep) bumpKeep(); else bumpSub(); }
            if (mask & 2) { if (aIsKeep) bumpSub(); else bumpKeep(); }
        } else if (st == RemovedA) {
            if (aIsKeep) bumpKeep(); else bumpSub();
        } else if (st == RemovedB) {
            if (aIsKeep) bumpSub(); else bumpKeep();
        } else {   // RemovedBoth:两边都算
            bumpKeep();
            bumpSub();
        }
    }

    int restored = 0;
    for (const ExactDupMove &mv : std::as_const(m_exactDupMoves)) {
        if (!mv.moved) continue;

        const auto it = deleted.constFind(mv.dirKey);
        const int delKeep = (it == deleted.constEnd()) ? 0 : it.value().first;
        const int delSub  = (it == deleted.constEnd()) ? 0 : it.value().second;

        // 合集侧被删得更多 -> 说明用户实际更想留下子集侧,把完全重复的归属也搬过去
        if (foldercompare::decideExactDupRestore(delKeep, delSub, /*folderKeepIsA=*/true)
            != foldercompare::ExactDupAction::RestoreToSubset)
            continue;

        if (mv.movedFileIdx < 0 || mv.movedFileIdx >= (int)m_files.size()) continue;
        if (!m_files[mv.movedFileIdx].inDuplicate) continue;   // 已经不在 duplicate 里

        if (!restoreFromDuplicate(mv.movedFileIdx, nullptr)) {
            logLine(QStringLiteral("[%1] 完全重复回迁失败: %2")
                        .arg(nowStr(), m_files[mv.movedFileIdx].originalPath), true, 3);
            continue;
        }
        ++restored;

        /*
         * 只回迁文件是不够的:合集侧那一份还在原地,同一套作品就同时存在于两个目录。
         * 归属既然改判给子集侧,合集侧那份就该进 duplicate —— 这才是"把作品集中"。
         *
         * 但驱逐【不能】在这里直接入队:restoreFromDuplicate 返回 true 只代表
         * "操作已受理",物理 rename 在后台串行执行。以前两步各自入队,一旦
         * 回迁物理失败而驱逐成功,子集那份仍在 duplicate、原保留那份也进了
         * duplicate —— 这套作品从工作区整体消失。
         * 现在把驱逐登记起来,挂到回迁的物理成功回调上(handleFileOpResult);
         * 回迁失败或数据换代则放弃驱逐,宁可两份都留在原地。
         */
        if (mv.keepFileIdx >= 0 && mv.keepFileIdx < (int)m_files.size()
            && !m_files[mv.keepFileIdx].inDuplicate) {
            m_pendingEvictAfterRestore.insert(mv.movedFileIdx, mv.keepFileIdx);
        }
    }

    if (restored > 0) {
        logLine(QStringLiteral("[%1] 完全重复归属调整: 回迁 %2 张到子集侧"
                               "(合集侧将在回迁落盘后移入 duplicate)")
                    .arg(nowStr()).arg(restored));
        if (uiRefresh)
            rebuildPairList();
    } else if (!m_exactDupMoves.isEmpty()) {
        logLine(QStringLiteral("[%1] 完全重复归属不变: 人工决策没有偏向子集侧")
                    .arg(nowStr()));
    }
}

bool ImageDeduplicateWindow::isFolderCompareMode() const
{
    for (const FileEntry &fe : std::as_const(m_files)) {
        if (fe.internalMode == static_cast<int>(InternalDedup::AcrossTopDirs))
            return true;
    }
    return false;
}

/* 文件是否"直属"该顶层子目录(只在本层,不含子目录里的图片) */
/* isDirectChildOfDir 已删除:目录模式的 jump 不再按"直属图片"过滤,
 * 它会把嵌套在顶层子目录更深一层的对全部拦掉(实测"跳过了0对"的根源)。 */

void ImageDeduplicateWindow::jumpPendingPairs(const QVector<int> &pairIndices)
{
    if (pairIndices.isEmpty())
        return;

    if (blockedByFolderDecision(QStringLiteral("jump")))
        return;

    /*
     * 筛选状态:这里就是"把选中的这几行标成 jumped"。
     *
     * 直接交给 filterBatchAction(Skip) —— 它知道筛选里"另一侧"是哪一张,
     * 而且会先撤销本对删过的那一侧(把一个 remove B 过的对改成 jumped,
     * 文件必须搬回来)。以前这条路在筛选里只碰 Pending,而筛选成员几乎全是
     * InvalidA(锚点已删),于是"jump 一个 removed B 的对没反应,只能先恢复 Pending"。
     *
     * 和菜单里的 Jump These Files/Folders 不同:那两者按"含这张图/这两个目录"
     * 圈范围,会波及筛选列表以外的对,所以筛选下仍然禁用。
     */
    if (m_filter.active) {
        filterBatchAction(pairIndices, ActionRecord::Skip);
        return;
    }

    QVector<int> ordered = pairIndices;
    std::sort(ordered.begin(), ordered.end());
    ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());

    int jumped = 0;
    int untouched = 0;
    {
        std::shared_lock<std::shared_mutex> lk(m_mutex);
        MMutexScope mScope(g_iwdMMutexDepth);

        for (int idx : std::as_const(ordered)) {
            if (idx < 0 || idx >= (int)m_pairs.size()) continue;
            shared_ptr<PairInfo> p = m_pairs[idx];
            if (!p) continue;

            std::lock_guard<std::mutex> lkState(p->stateMutex);
            /*
             * 只碰 Pending。已经做过 remove 的对【一个文件都不动】:
             * applyActionToPair 的 Skip 分支会 restoreIfNeeded —— 把已经搬进
             * duplicate 的文件搬回来,等于把用户的 remove 决定抹掉。
             * 已经 Invalid 的也不碰(它们要回肇事的那一对上改)。
             */
            if (p->state != PairState::Pending) {
                ++untouched;
                continue;
            }
            applyActionToPair(p, ActionRecord::Skip);
            ++jumped;
        }
    }

    if (jumped == 0) {
        createFloatingLabel(this, QStringLiteral("选中的 %1 对里没有 Pending,没有可跳过的")
                                      .arg(ordered.size()));
        logLine(QStringLiteral("[%1] Jump Pendings: 选中 %2 对,其中 0 对是 Pending")
                    .arg(nowStr()).arg(ordered.size()), true);
        return;
    }

    /*
     * 收尾和批量操作同一套:状态变化可能相互影响(比如某一对被跳过之后,
     * 同文件的其它对仍保持 Pending —— Skip 不改兄弟对,这里只是走标准收尾)。
     */
    {
        std::shared_lock<std::shared_mutex> lk(m_mutex);
        MMutexScope mScope(g_iwdMMutexDepth);
        std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
        finalizePairStateChange(DuplicatePairListView::ScrollNever);
        // 当前显示的这一对如果也被跳过了,直接前进到下一个 Pending
        advanceToNextPendingAfterBatch();
    }

    logLine(QStringLiteral("[%1] Jump Pendings: 选中 %2 对,跳过 %3 对(已 remove / 已失效的 %4 对保持原状)")
                .arg(nowStr()).arg(ordered.size()).arg(jumped).arg(untouched), true);
    createFloatingLabel(this, QStringLiteral("已跳过 %1 对(只处理 Pending%2)")
                                  .arg(jumped)
                                  .arg(untouched > 0
                                           ? QStringLiteral(",另有 %1 对已处理过,未改动").arg(untouched)
                                           : QString()));
}

void ImageDeduplicateWindow::jumpForPair(int pairIndex, JumpScope scope)
{
    /*
     * 筛选状态下 Jump 没有意义而且有害:它的作用域是"包含这张图的所有对"/
     * "这两个目录之间的所有对",会把筛选列表以外的对一起标成 Skipped,
     * 用户回头看到一堆没处理过的 Skipped 会莫名其妙。要跳就在筛选里按空格跳过单对。
     */
    if (m_filter.active) {
        createFloatingLabel(this, QStringLiteral("筛选状态下不能用 Jump,请先点 finished 退出"));
        logLine(QStringLiteral("[%1] 筛选状态下忽略 Jump").arg(nowStr()));
        return;
    }

    /*
     * Jump 的统一实现。三种作用域都常驻,不再按"普通模式/目录模式"切换:
     *
     *   scope = File ("Jump This File")
     *     跳过【所有尚未处理、且 A 或 B 就是这张图】的重复对。
     *     同一张图可能同时出现在多个 pair 里(它和好几张图都相似),
     *     用户既然认定"这张图我要保留",这些对就都不必再看。
     *     用哪张图当锚点:当前显示的那一对按屏幕上的那张(空格/左右键切换过就是那张),
     *     其它行(右键菜单里没被显示的行)统一取 A 侧。
     *
     *   scope = TwoFiles ("Jump These Two Files")
     *     跳过【A 或 B 任意一张出现在里面】的全部未处理对。
     *     这是"这两张图我都不想再看"的一次性表达:
     *     以前只能切到 A 跳一次、切回 B 再跳一次,而且第二次还会停在原地。
     *
     *   scope = Folders ("Jump These Folders")
     *     跳过【这两张图各自直接父目录之间】的全部未处理对(含两个方向)。
     *     例:当前对是 D/fa/sub_f1/p.png ↔ D/fb/sub_f1/p2.png,
     *         只跳 D/fa/sub_f1 ↔ D/fb/sub_f1 之间的对;
     *         D/fa/sub_f2(同层其它子目录)和更深的 sub_f1/sub_f1_2 都不跳。
     *
     * 作用域用【直接父目录】而不是顶层子目录:按顶层子目录过滤会把嵌套一层
     * 的对全部圈进来(范围大得离谱,一按就跳过几千对);按"直属图片"过滤又
     * 会把嵌套的对全部漏掉(实测"跳过了 0 对")。直接父目录正好是用户眼里
     * "这两个文件夹"。
     */
    QElapsedTimer tm;
    tm.start();

    dedup::traceShift(QStringLiteral("WIN jump pair=%1 scope=%2")
                          .arg(pairIndex + 1)
                          .arg(scope == JumpScope::Folders    ? QStringLiteral("folders")
                               : scope == JumpScope::TwoFiles ? QStringLiteral("two-files")
                                                              : QStringLiteral("file")));

    std::shared_lock<std::shared_mutex> lk(m_mutex);
    MMutexScope mScope(g_iwdMMutexDepth);   // displayPairByIndex 会按需再判断,见 applyActionToCurrentPair

    shared_ptr<PairInfo> target;
    {
        std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
        if (pairIndex < 0 || pairIndex >= (int)m_pairs.size())
            return;
        target = m_pairs[pairIndex];
    }
    if (!target) return;

    if (blockedByFolderDecision(QStringLiteral("jump")))
        return;

    // 规范化:Windows 大小写/分隔符差异不能影响"是不是同一张图/同一目录"
    auto norm = [](const QString &p) {
        return QDir::cleanPath(QDir::fromNativeSeparators(p)).toLower();
    };
    auto parentDirOf = [this](int fileIdx) -> QString {
        if (fileIdx < 0 || fileIdx >= (int)m_files.size())
            return QString();
        const QString p = m_files[fileIdx].currentPath;
        if (p.isEmpty())
            return QString();
        return QDir::cleanPath(QFileInfo(p).absolutePath()).toLower();
    };
    auto pathOf = [this, &norm](int fileIdx) -> QString {
        if (fileIdx < 0 || fileIdx >= (int)m_files.size())
            return QString();
        return norm(m_files[fileIdx].currentPath);
    };

    const bool isCurrentPair = (pairIndex == m_currentIndex);

    /*
     * 锚点:File 作用域是一张图(当前显示侧 / A),TwoFiles 是两张。
     * Folders 不用锚点路径,anchorFileIdx 只用来打日志。
     */
    int anchorFileIdx = target->idxA;
    QSet<QString> anchorPaths;
    auto fileIdxValid = [this](int i) { return i >= 0 && i < (int)m_files.size(); };

    if (scope == JumpScope::File) {
        anchorFileIdx = isCurrentPair ? (m_showingA ? target->idxA : target->idxB)
                                      : target->idxA;
        const QString p = pathOf(anchorFileIdx);
        if (p.isEmpty()) return;
        anchorPaths.insert(p);
    } else if (scope == JumpScope::TwoFiles) {
        const QString pa = pathOf(target->idxA);
        const QString pb = pathOf(target->idxB);
        if (pa.isEmpty() && pb.isEmpty()) return;
        if (!pa.isEmpty()) anchorPaths.insert(pa);
        if (!pb.isEmpty()) anchorPaths.insert(pb);
        if (fileIdxValid(target->idxA))      anchorFileIdx = target->idxA;
        else if (fileIdxValid(target->idxB)) anchorFileIdx = target->idxB;
        else                                 return;
    } else if (!fileIdxValid(anchorFileIdx)) {
        if (!fileIdxValid(target->idxB)) return;
        anchorFileIdx = target->idxB;
    }
    if (!fileIdxValid(anchorFileIdx)) return;

    QString curDirA, curDirB;
    bool effectiveFoldersScope = (scope == JumpScope::Folders);
    if (effectiveFoldersScope) {
        curDirA = parentDirOf(target->idxA);
        curDirB = parentDirOf(target->idxB);
        if (curDirA.isEmpty() || curDirB.isEmpty() || curDirA == curDirB) {
            /*
             * 两张图在同一个目录里(理论上不会出现在目录对里,但"直接放在输入根目录"
             * 之类的结构下有可能):目录级跳跃无从下手,退化成单图跳跃,
             * 至少不会什么都不做。
             */
            effectiveFoldersScope = false;
            const QString p = pathOf(target->idxA);
            if (p.isEmpty()) return;
            anchorPaths.insert(p);
            logLine(QStringLiteral("[%1] Jump These Folders 退化:两张图在同一目录(%2),改按单图跳跃")
                        .arg(nowStr(),
                             curDirA.isEmpty() ? QStringLiteral("<根目录>") : curDirA));
        }
    }

    /*
     * 一遍扫描,同时收集"要跳的对"和"每一对的直接父目录"。
     * 父目录只算一次(以前每对都要算两次 cleanPath),35k 对下这一趟能省下不少字符串操作。
     */
    QVector<int> touched;
    for (int i = 0; i < (int)m_pairs.size(); ++i) {
        shared_ptr<PairInfo> p = m_pairs[i];
        if (!p) continue;

        int ia = -1, ib = -1;
        {
            // 先只读状态和下标,后面真的要对它动手时再重新加锁确认一次
            std::lock_guard<std::mutex> lkState(p->stateMutex);
            if (p->state != PairState::Pending) continue;
            ia = p->idxA;
            ib = p->idxB;
        }
        if (ia < 0 || ia >= (int)m_files.size() || ib < 0 || ib >= (int)m_files.size())
            continue;

        if (effectiveFoldersScope) {
            const QString da = parentDirOf(ia);
            const QString db = parentDirOf(ib);
            // 作用域判定和目录级 remove 共用同一份实现(folderscope.h,有单测)
            if (!folderscope::inFolderPair(curDirA, curDirB, da, db))
                continue;   // 同层其它子目录 / 更深的嵌套 / 其它目录之间的对都不跳
        } else {
            if (!anchorPaths.contains(pathOf(ia)) && !anchorPaths.contains(pathOf(ib)))
                continue;
        }

        std::lock_guard<std::mutex> lkState(p->stateMutex);
        if (p->state != PairState::Pending) continue;   // 期间被别的线程改过
        applyActionToPair(p, ActionRecord::Skip);
        touched << i;
    }
    const qint64 tScan = tm.elapsed();

    for (int i : std::as_const(touched))
        refreshPairListRow(i);
    const qint64 tList = tm.elapsed();

    const QString scopeName = effectiveFoldersScope ? QStringLiteral("folders")
                             : (scope == JumpScope::TwoFiles ? QStringLiteral("two-files")
                                                             : QStringLiteral("file"));
    logLine(QStringLiteral("[%1] jump(%2) by %3: %4 pair(s) skipped")
                .arg(nowStr(), scopeName, m_files[anchorFileIdx].currentPath)
                .arg(touched.size()));

    createFloatingLabel(this,
                        effectiveFoldersScope
                            ? QStringLiteral("已跳过 %1 对(这两个目录之间)").arg(touched.size())
                            : (scope == JumpScope::TwoFiles
                                   ? QStringLiteral("已跳过 %1 对(包含这两张图)").arg(touched.size())
                                   : QStringLiteral("已跳过 %1 对(包含该图片)").arg(touched.size())));

    /*
     * 当前这一对如果已经不是 Pending 了,就前进到下一个待处理对 ——
     * 屏幕停在一对"刚刚被判为 Skipped"的图上没有意义。
     *
     * 注意判断条件是"现在还不是 Pending",而不是"本次被我跳过了":
     * 先切到 A 跳一次(这一对变成 Skipped),再切回 B 跳第二次时,
     * 这一对本来就已经不是 Pending 了 —— 以前拿"本次是否跳过它"当条件,
     * 于是第二次执行完就停在原地,得手动点下一对。
     */
    bool currentStillPending = false;
    if (isCurrentPair) {
        std::lock_guard<std::mutex> lkState(target->stateMutex);
        currentStillPending = (target->state == PairState::Pending);
    }

    int nextIndex = -1;
    if (isCurrentPair && !currentStillPending) {
        std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
        nextIndex = findNextPending();
        if (nextIndex >= 0 && nextIndex < (int)m_pairs.size())
            displayPairByIndex(nextIndex, m_dataEpoch);
        else
            showNoMorePairs();
    } else {
        syncPairListSelection(DuplicatePairListView::ScrollNever);
    }

    const qint64 total = tm.elapsed();
    if (total > 150) {
        logLine(QStringLiteral("[慢操作] jump(%1) 总%2ms | 扫描%3 列表%4 其余%5")
                    .arg(scopeName)
                    .arg(total).arg(tScan).arg(tList - tScan)
                    .arg(total - tList), true);
    }
}

void ImageDeduplicateWindow::removeFolders(int pairIndex, int side)
{
    /*
     * folder 级 remove:"这两个目录之间的全部未处理对",作用域和
     * jumpForPair(foldersScope=true) 完全一致 —— 两张图各自的【直接父目录】,
     * 含两个方向。区别只是动作是"删"而不是"跳过"。
     *
     * side: 0 = 删这些对里落在 A 侧目录的图,1 = 落 B 侧目录的图,2 = 两侧都删。
     * 为什么按"目录"而不是按"每对的 A/B 位":摆位(合集优先)会让同一张图在不同
     * 对里出现在 A 或 B 位,按位删会删错边。用户点的是"这个文件夹里参与比较的图"。
     */
    if (m_filter.active) {
        createFloatingLabel(this, QStringLiteral("筛选状态下不能用目录级 remove,请先点 finished 退出"));
        logLine(QStringLiteral("[%1] 筛选状态下忽略目录级 remove").arg(nowStr()));
        return;
    }
    if (side != 0 && side != 1 && side != 2)
        return;

    QElapsedTimer tm;
    tm.start();

    std::shared_lock<std::shared_mutex> lk(m_mutex);
    MMutexScope mScope(g_iwdMMutexDepth);

    shared_ptr<PairInfo> target;
    {
        std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
        if (pairIndex < 0 || pairIndex >= (int)m_pairs.size())
            return;
        target = m_pairs[pairIndex];
    }
    if (!target) return;

    if (blockedByFolderDecision(QStringLiteral("folder remove")))
        return;

    auto parentDirOf = [this](int fileIdx) -> QString {
        if (fileIdx < 0 || fileIdx >= (int)m_files.size())
            return QString();
        const QString p = m_files[fileIdx].currentPath;
        if (p.isEmpty())
            return QString();
        return QDir::cleanPath(QFileInfo(p).absolutePath()).toLower();
    };

    int ia = -1, ib = -1;
    {
        std::lock_guard<std::mutex> lkState(target->stateMutex);
        ia = target->idxA;
        ib = target->idxB;
    }
    const QString dirA = parentDirOf(ia);
    const QString dirB = parentDirOf(ib);
    if (dirA.isEmpty() || dirB.isEmpty() || dirA == dirB) {
        createFloatingLabel(this, QStringLiteral("两张图在同一个目录,没有\"这两个目录\"可删"));
        logLine(QStringLiteral("[%1] 目录级 remove 跳过:两张图在同一目录(%2)")
                    .arg(nowStr(), dirA.isEmpty() ? QStringLiteral("<空>") : dirA));
        return;
    }

    /*
     * 第一步:只读扫描,收集"要动哪些对、每一对删哪一侧"。
     * 不在这一趟里直接下手 —— 一边遍历一边改状态会让 markRelatedPairsValidState
     * 把还没轮到的对改成 Invalid,后面的判断就全乱了。
     */
    struct Hit { shared_ptr<PairInfo> p; ActionRecord::Type type; };
    QVector<Hit> hits;
    for (const shared_ptr<PairInfo> &p : std::as_const(m_pairs)) {
        if (!p) continue;

        int a = -1, b = -1;
        {
            std::lock_guard<std::mutex> lkState(p->stateMutex);
            if (p->state != PairState::Pending) continue;
            a = p->idxA;
            b = p->idxB;
        }
        if (a < 0 || a >= (int)m_files.size() || b < 0 || b >= (int)m_files.size())
            continue;

        const QString da = parentDirOf(a);
        const QString db = parentDirOf(b);
        if (!folderscope::inFolderPair(dirA, dirB, da, db))
            continue;

        const int removeSide = folderscope::removeSideFor(dirA, dirB, da, side);
        if (removeSide < 0)
            continue;
        const ActionRecord::Type t = (removeSide == 2) ? ActionRecord::RemoveBoth
                                  : (removeSide == 0) ? ActionRecord::RemoveA
                                                      : ActionRecord::RemoveB;

        hits.push_back(Hit{p, t});
    }

    if (hits.isEmpty()) {
        createFloatingLabel(this, QStringLiteral("这两个目录之间没有未处理的对"));
        logLine(QStringLiteral("[%1] 目录级 remove:这两个目录之间没有 Pending 对(%2 ↔ %3)")
                    .arg(nowStr(), dirA, dirB));
        return;
    }

    /* 第二步:逐个应用(每对应用前重新确认还是 Pending) */
    QVector<int> changedRows;
    int applied = 0;
    for (const Hit &h : std::as_const(hits)) {
        std::lock_guard<std::mutex> lkState(h.p->stateMutex);
        if (h.p->state != PairState::Pending) continue;
        applyActionToPair(h.p, h.type, &changedRows);
        ++applied;
    }

    /*
     * remove 会让"引用同一个文件"的其它对变成 Invalid,受影响的行远不止 hits ——
     * 所以这里必须整表收尾,不能只刷命中的那几行(和 applyBatchAction 同一套)。
     */
    {
        std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
        finalizePairStateChange(DuplicatePairListView::ScrollNever);
        advanceToNextPendingAfterBatch();
    }

    const QString sideName = (side == 0) ? QStringLiteral("A 侧目录")
                                         : (side == 1 ? QStringLiteral("B 侧目录")
                                                      : QStringLiteral("两侧"));
    logLine(QStringLiteral("[%1] 目录级 remove(%2): 命中 %3 对, 实际处理 %4 对  [%5 ↔ %6]")
                .arg(nowStr(), sideName).arg(hits.size()).arg(applied).arg(dirA, dirB), true);
    createFloatingLabel(this, QStringLiteral("已按目录处理 %1 对(%2)").arg(applied).arg(sideName));

    const qint64 total = tm.elapsed();
    if (total > 500) {
        logLine(QStringLiteral("[慢操作] 目录级 remove 总 %1ms(命中 %2 对)")
                    .arg(total).arg(hits.size()), true);
    }
}

void ImageDeduplicateWindow::on_btn_jump_this_file_clicked()
{
    /* Q 键:保持原来的语义 —— 目录模式跳目录,普通模式跳单图 */
    int idx = -1;
    {
        std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
        idx = m_currentIndex;
    }
    jumpForPair(idx, isFolderCompareMode() ? JumpScope::Folders : JumpScope::File);
}

/* ==========================================================================
 *  筛选:一次把"这张图 + 和它相似的所有版本"整族删掉
 *
 *  语义(2026-11-22 重做,详见头文件里 FilterEntryMode 上方的说明):
 *     · 进入时立刻把"进入时选的动作"落到原对(remove A / B / both),
 *       不再延迟到退出时;
 *     · 列表只列"其它"对,每行锚点钉在 A 位,唯一动作是删掉 B;
 *     · 只要 B 还活着就能操作,不看这一对的状态;
 *     · 没有其它成员就不进筛选,直接执行动作 + 提示 + 跳到下一对。
 * ========================================================================== */

bool ImageDeduplicateWindow::filterMemberUsable(const std::shared_ptr<PairInfo> &p,
                                                int anchorFile) const
{
    /* 调用方必须已持有 p->stateMutex:这里要读 idxA/idxB,而它们会被 swapPairSides 翻 */
    if (!p) return false;
    if (anchorFile < 0 || anchorFile >= (int)m_files.size()) return false;

    const bool anchorIsA = (p->idxA == anchorFile);
    const bool anchorIsB = (p->idxB == anchorFile);
    if (!anchorIsA && !anchorIsB) return false;   // 不含锚点

    const int otherIdx = anchorIsA ? p->idxB : p->idxA;
    if (otherIdx < 0 || otherIdx >= (int)m_files.size()) return false;
    if (otherIdx == anchorFile) return false;

    /*
     * 唯一条件:**另一侧还在**。
     *
     * 筛选里唯一会动文件的操作就是"删掉这一行的另一侧"(锚点在进入筛选那一刻
     * 已经按用户选的动作处理掉了)。所以能不能操作只取决于那一侧还在不在 ——
     * 这一对本身是 Pending、InvalidA(锚点被删)、RemovedB(上次删过)、
     * 甚至 RemovedBoth,只要还有一张活着的相似图,就必须能被收拾。
     *
     * 以前这里按 state 逐条排除(RemovedBoth 不列、invalidMask==0 的 Removed* 不列),
     * 于是"对一个已经 remove both 的对做 filter AB"会筛出空列表或只剩原对,
     * 和 Pending 状态下筛出来的完全不一样。
     */
    const FileEntry &other = m_files[otherIdx];
    return !other.inDuplicate && !other.currentPath.isEmpty();
}

void ImageDeduplicateWindow::swapPairSides(const std::shared_ptr<PairInfo> &p)
{
    /*
     * 把一对的左右对调。调用方必须已持有 p->stateMutex。
     *
     * 不能只换 idxA/idxB:RemovedA/InvalidA 这些状态是**相对当前 A/B** 的,
     * 只换下标会让"已移除"跑到另一侧去。所以状态、失效原因、叠加位一起翻。
     */
    if (!p) return;

    std::swap(p->idxA, p->idxB);
    std::swap(p->invalidCauseForA, p->invalidCauseForB);
    flipStateSides(p->state, p->removedMask);   // 规则见 pairstate_math.h
    /*
     * invalidMask 也要跟着翻:它记的同样是"A 侧/B 侧"。
     * 不翻的话,"B 被别的对删了"在对调后会变成"A 被删了" —— 显示和判断全错位。
     */
    p->invalidMask = flipMaskSides(p->invalidMask);
}

bool ImageDeduplicateWindow::removeSideOfPair(const std::shared_ptr<PairInfo> &p, bool sideA,
                                             QVector<int> *changedRows)
{
    /*
     * 只移除某一侧,不做 remove A/B 那套"先把另一侧恢复回来"的语义。
     * 调用方必须已持有 p->stateMutex。
     *
     * 为什么不能用 applyActionToPair:它把 RemoveA 当成"改主意"——
     * prevState 是 RemovedB 时会先把 B 恢复回来。筛选里锚点已经删掉了,
     * 走那条路会把"要保留的那一侧"乱搬。
     */
    if (!p) return false;

    /*
     * 和 applyActionToPair 一样打"第几次动作"的时间戳。
     *
     * 失效对上的删除(复合态)以前不打 seq,恒为 0 —— 在 importdedupe 的
     * finalizePlan 按 maxSeq 裁决"同一文件被多对下过不同决定"时必输给任何
     * 动作过的对,用户的最后一次删除会被当成"更早的意图"丢掉。
     */
    p->decisionSeq = ++m_decisionSeq;

    const int fileIdx = sideA ? p->idxA : p->idxB;
    if (fileIdx < 0 || fileIdx >= (int)m_files.size()) return false;
    if (m_files[fileIdx].currentPath.isEmpty()) return false;
    if (m_files[fileIdx].inDuplicate) return true;   // 已经在 duplicate 里,视为完成

    if (!moveToDuplicate(fileIdx, p, /*optimistic=*/-1, /*rollback=*/-1))
        return false;

    // 引用同一张图的其它对要跟着失效(和普通 remove 一致)
    markRelatedPairsValidState(m_files[fileIdx], p, false, changedRows);

    /*
     * 状态推进交给 pairstate_math.h 的纯函数:
     *   失效对 -> 保持 Invalid* + 记叠加位(否则点它跳不回肇事那一对);
     *   有效对 -> 收敛成 RemovedA/B/Both。
     */
    applyRemovalToState(p->state, p->removedMask, sideA);
    return true;
}

int ImageDeduplicateWindow::filterMemberPos(int pairIndex) const
{
    for (int i = 0; i < (int)m_filter.members.size(); ++i) {
        if (m_filter.members[i].pairIndex == pairIndex) return i;
    }
    return -1;
}

int ImageDeduplicateWindow::nextActionableMember(int fromPos, int step) const
{
    /*
     * 筛选里只有一件事可做:删掉"另一侧"。锚点钉在 A 位之后,那一侧就是 B。
     * 所以"还能处理" == B 侧还在原处。已经删完的行跳过,不然用户会一直在
     * 死行上来回点。
     */
    if (step == 0) return -1;
    for (int i = fromPos + step; i >= 0 && i < (int)m_filter.members.size(); i += step) {
        const int pi = m_filter.members[i].pairIndex;
        if (pi < 0 || pi >= (int)m_pairs.size()) continue;
        const auto &p = m_pairs[pi];
        if (!p) continue;

        int idxB = -1;
        {
            std::lock_guard<std::mutex> lkState(p->stateMutex);
            idxB = p->idxB;
        }
        if (idxB < 0 || idxB >= (int)m_files.size()) continue;
        if (m_files[idxB].inDuplicate || m_files[idxB].currentPath.isEmpty()) continue;

        return i;
    }
    return -1;
}

void ImageDeduplicateWindow::startFilterFromCurrentPair(FilterEntryMode mode)
{
    /*
     * Shift+A / Shift+S / Shift+D 的入口:以当前显示的这一对为锚点进筛选。
     *
     * **故意不看列表的多选**:筛选的锚点是一张图(filter both 是两张),整个列表
     * 会被重组成"包含这张图的配对";多选在这里没有意义,而且和 remove A/B/both
     * 的"对选中的每一对都做一次"是相反的语义。所以锚点永远是 m_currentIndex。
     */
    int current = -1;
    {
        std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
        current = m_currentIndex;
    }
    if (current < 0 || current >= (int)m_pairs.size()) {
        createFloatingLabel(this, QStringLiteral("当前没有可筛选的图片对"));
        return;
    }

    /*
     * 显示图还没上屏时不能筛:屏幕上贴着的还是上一对的图,用户以为在筛眼前这张,
     * 实际锚点却是还没显示出来的那一对。(remove A/B 的同一道闸在
     * applyActionToCurrentPair / dispatchPairAction 里,这里必须自己挡。)
     */
    if (m_pairActionButtonsLocked) {
        createFloatingLabel(this, QStringLiteral("图片还没上屏,等这一对显示出来再筛选"));
        return;
    }

    /* 已经在筛选里 / 目录方向未定 / Deferred 模式:startSimilarityFilter 自己会拦并给提示 */
    startSimilarityFilter(current, mode);
}

bool ImageDeduplicateWindow::startSimilarityFilter(int pairIndex, FilterEntryMode mode)
{
    if (m_filter.active) {
        createFloatingLabel(this, QStringLiteral("已经在筛选中,不能嵌套"));
        logLine(QStringLiteral("[%1] 忽略筛选请求:已经在筛选状态").arg(nowStr()));
        return false;
    }
    if (blockedByFolderDecision(QStringLiteral("筛选相似图片")))
        return false;

    /*
     * Deferred(导入整理 / remove cover)不支持筛选。
     *
     * 那两个模式的文件操作要等关窗后由会话统一生成计划,而计划是**按 pair 状态**
     * 推出来的(pathA 保留/搬进 B 树、pathB 进回收站…)。筛选里会出现
     * "失效 + 已移除"的叠加态:它的"已移除"只记在 removedMask 里,
     * 计划生成器只认 RemovedA/B/Both,那个删除会被静默丢掉;
     * 硬翻译成 RemovedB 又会让计划去搬一张已经被别的对处理掉的图。
     * 与其给一个会说谎的结果,不如在这里挡住。
     */
    if (m_fileOpMode == FileOpMode::Deferred) {
        createFloatingLabel(this, QStringLiteral("当前模式(导入整理/移除封面)不支持筛选"));
        logLine(QStringLiteral("[%1] 拒绝筛选:Deferred 模式的文件操作由关窗后的计划统一执行")
                    .arg(nowStr()), true);
        return false;
    }

    std::shared_lock<std::shared_mutex> lk(m_mutex);
    MMutexScope mScope(g_iwdMMutexDepth);
    std::lock_guard<std::mutex> lkIndex(currentIndexMutex);

    if (pairIndex < 0 || pairIndex >= (int)m_pairs.size())
        return false;
    std::shared_ptr<PairInfo> origin = m_pairs[pairIndex];
    if (!origin) return false;

    int anchorA = -1, anchorB = -1;
    {
        std::lock_guard<std::mutex> lkState(origin->stateMutex);
        anchorA = origin->idxA;
        anchorB = origin->idxB;
    }
    if (anchorA < 0 || anchorA >= (int)m_files.size()
        || anchorB < 0 || anchorB >= (int)m_files.size())
        return false;

    /* 进入筛选时选的动作 = 原对要立刻执行的动作 */
    QVector<int> anchors;
    ActionRecord::Type originType = ActionRecord::RemoveBoth;
    switch (mode) {
    case FilterFromA:    anchors << anchorA;                originType = ActionRecord::RemoveA;    break;
    case FilterFromB:    anchors << anchorB;                originType = ActionRecord::RemoveB;    break;
    case FilterFromBoth: anchors << anchorA << anchorB;     originType = ActionRecord::RemoveBoth; break;
    }

    auto fileNameOf = [this](int idx) -> QString {
        return (idx >= 0 && idx < (int)m_files.size())
                   ? QFileInfo(m_files[idx].currentPath).fileName()
                   : QStringLiteral("?");
    };

    /*
     * ---- 1) 立刻把动作落到原对 ----
     *
     * 这是这次改动的核心:原对不再"挂着 Pending 等退出筛选时再删"。
     * 用户选 remove both 的 filter,就先把这一对设成 removed both;
     * 后面想改主意,退出筛选后在原对上按 a/d 就是普通的"改主意"语义
     * (applyActionToPair 的 RemoveA 会把之前删过的 B 恢复回来)。
     *
     * 锚点一被移走,所有"含这张图"的兄弟对立刻变成 Invalid A/B —— 那正是
     * 要收拾的那批,它们的另一侧还活着。
     */
    {
        std::lock_guard<std::mutex> lkState(origin->stateMutex);
        applyActionToPair(origin, originType);
    }
    dedupenv::playSound(DedupSound::SingleAction);
    logLine(QStringLiteral("[%1] 进入筛选: 原对 pair %2 先执行 %3(%4 ↔ %5)")
                .arg(nowStr()).arg(pairIndex)
                .arg(actionName(originType), fileNameOf(anchorA), fileNameOf(anchorB)), true);

    /*
     * ---- 2) 收集成员 ----
     *
     * "成员" = 除原对以外、含任一锚点、且**另一侧还活着**的对。
     * 原对不再进列表:它已经按用户的选择处理完了,留一行在列表里只是噪音
     * (以前它还只读,用户报的"一直留一个 pending 动不了"就是它)。
     */
    std::vector<FilterMember> members;
    int rejectedNoOther = 0;      // 含锚点但另一侧已经没了
    for (int i = 0; i < (int)m_pairs.size(); ++i) {
        if (i == pairIndex) continue;
        const std::shared_ptr<PairInfo> &p = m_pairs[i];
        if (!p) continue;

        /*
         * idxA/idxB 和"另一侧还在不在"必须在同一把锁里看:
         * swapPairSides 会就地翻 A/B,分两次读可能把"锚点"和"另一侧"认错边。
         */
        int anchorForThis = -1;
        bool usable = false;
        {
            std::lock_guard<std::mutex> lkState(p->stateMutex);
            for (int a : std::as_const(anchors)) {
                if (p->idxA == a || p->idxB == a) { anchorForThis = a; break; }
            }
            usable = (anchorForThis >= 0) && filterMemberUsable(p, anchorForThis);
        }
        if (anchorForThis < 0) continue;      // 不含锚点
        if (!usable) {
            ++rejectedNoOther;
            continue;
        }

        FilterMember m;
        m.pairIndex = i;
        m.anchorFile = anchorForThis;
        members.push_back(m);
    }

    /*
     * ---- 3) 没有别的成员:不进筛选界面 ----
     *
     * 这一对就是整个"族"(其它含锚点的对都已经没有可删的另一侧了),
     * 进一个只有一个可处理项的筛选界面毫无意义。直接按用户选的动作处理原对,
     * 提示一句,然后往前走。
     */
    if (members.empty()) {
        finalizePairStateChange(DuplicatePairListView::ScrollNever);

        const int next = findNextPending(pairIndex);
        if (next >= 0 && next < (int)m_pairs.size())
            displayPairByIndex(next, m_dataEpoch);
        else
            showNoMorePairs();

        logLine(QStringLiteral("[%1] 筛选未进入: 没有其它相似配对(%2 对含锚点但另一侧已删),"
                               "已直接对 pair %3 执行 %4")
                    .arg(nowStr()).arg(rejectedNoOther).arg(pairIndex)
                    .arg(actionName(originType)), true);
        createFloatingLabel(this,
                            QStringLiteral("没有其它相似配对:已直接执行 %1,跳到下一对")
                                .arg(actionName(originType)));
        return false;
    }

    /* ---- 4) 建立会话 ---- */
    m_filter = PairFilterSession();
    m_filter.active = true;
    m_filter.mode = mode;
    m_filter.anchorFiles = anchors;
    m_filter.originPair = origin;
    m_filter.originPairIndex = pairIndex;
    m_filter.members = members;

    /*
     * 把锚点钉到 A 位。原来在 B 位的就地交换,并记下来 ——
     * 退出时要换回去,否则列表里这一对的左右会莫名其妙反过来。
     */
    for (FilterMember &m : m_filter.members) {
        const std::shared_ptr<PairInfo> &p = m_pairs[m.pairIndex];
        if (!p) continue;
        std::lock_guard<std::mutex> lkState(p->stateMutex);
        if (p->idxB == m.anchorFile && p->idxA != m.anchorFile) {
            swapPairSides(p);
            m.swapped = true;
        }
    }

    rebuildPairList();
    updateFilterUi();

    QString anchorName = fileNameOf(anchors.value(0, -1));
    if (anchors.size() > 1)
        anchorName = QStringLiteral("%1 + %2").arg(anchorName, fileNameOf(anchors[1]));

    logLine(QStringLiteral("[%1] 进入筛选: 锚点 %2(%3), %4 个待收拾的相似版本")
                .arg(nowStr()).arg(anchorName, actionName(originType))
                .arg(m_filter.members.size()), true);
    createFloatingLabel(this, QStringLiteral("已筛选 %1 对(锚点已执行 %2):一路 remove B,finished 退出")
                                  .arg(m_filter.members.size()).arg(actionName(originType)));

    /*
     * ===== 显示切换必须等原对的磁盘操作落地 =====
     *
     * 成员每一行都含锚点,而锚点刚刚被移走:FileEntry 已经被乐观改成
     * <原目录>/duplicate/xxx,但真正 rename 还在文件操作线程的队列里。
     * 如果这一刻就切过去解码,后台读到的是一个还不存在的路径 —— 失败一次就会
     * 给这一对打上"图片无法显示"的标记,用户看到的是一张空白图,而文件其实好好的。
     *
     * holdAdvanceUntilFileOpsSettle 是现成的"等落盘再前进"机制:落地后
     * handleFileOpResult -> advanceToNextPendingIfOn 会调用 findNextPending,
     * 在筛选态下正好落到第一个还能处理的成员上。没有人排队(锚点本来就在
     * duplicate 里)时它返回 false,那就立刻切 —— 那条路上路径一直有效。
     */
    if (holdAdvanceUntilFileOpsSettle(origin)) {
        logLine(QStringLiteral("[%1] 筛选: 原对的移动已入队,等落盘后再切到第一个成员")
                    .arg(nowStr()));
        return true;
    }

    /* 光标:第一个还能处理(另一侧还在)的成员 */
    int startPos = nextActionableMember(-1, +1);
    if (startPos < 0) startPos = 0;

    displayPairByIndex(m_filter.members[startPos].pairIndex, m_dataEpoch);
    return true;
}

bool ImageDeduplicateWindow::filterRemoveOtherSide(int pos, bool navigate)
{
    if (pos < 0 || pos >= (int)m_filter.members.size())
        return false;

    const int pairIndex = m_filter.members[pos].pairIndex;
    if (pairIndex < 0 || pairIndex >= (int)m_pairs.size())
        return false;
    std::shared_ptr<PairInfo> p = m_pairs[pairIndex];
    if (!p) return false;

    bool ok = false;
    {
        // 钉住之后"另一侧"必然在 B 位
        std::lock_guard<std::mutex> lkState(p->stateMutex);

        const int idxB = p->idxB;
        if (idxB < 0 || idxB >= (int)m_files.size()
            || m_files[idxB].inDuplicate || m_files[idxB].currentPath.isEmpty()) {
            /*
             * 另一侧已经不在:这一行没有可删的东西(常见于 remove both 筛选里
             * a-c / b-c 同时在场,删掉 c 之后另一行也跟着没了那一侧)。
             * 明确说一句,不要静默地当成"已经删完"然后往前跳。
             */
            ok = false;
        } else {
            ok = removeSideOfPair(p, /*sideA=*/false);
        }
    }
    if (!ok) {
        createFloatingLabel(this, QStringLiteral("这一对的另一侧已经不在了(没有可删的相似图)"));
        logLine(QStringLiteral("[%1] 筛选: pair %2 的另一侧已不在,跳过")
                    .arg(nowStr()).arg(pairIndex));
        return false;
    }
    // 筛选里的单对处理:和普通列表的 pop 一致(用户听得出"这一下生效了")
    dedupenv::playSound(DedupSound::SingleAction);

    // 成员数量小,直接重建子集列表:兄弟行被牵连成 Invalid 也能一次刷到位
    refreshFilterList();

    if (navigate) {
        const int next = nextActionableMember(pos, +1);
        if (next >= 0) {
            displayPairByIndex(m_filter.members[next].pairIndex, m_dataEpoch);
            updateFilterUi();
        } else {
            // 注意:这里**不**自动退出(用户要求),只是提示
            createFloatingLabel(this, QStringLiteral("筛选里没有剩下的可处理项了,点 finished 退出"));
        }
    }

    logLine(QStringLiteral("[%1] 筛选: pair %2 移除另一侧").arg(nowStr()).arg(pairIndex));
    return true;
}

bool ImageDeduplicateWindow::markPairSkippedUndoingOwnRemoval(const std::shared_ptr<PairInfo> &p)
{
    /*
     * "jump 这一对" = 这一对我不要删任何东西。
     *
     * 所以先把【本对】删过的那一侧从 duplicate 搬回来(不影响锚点:锚点是别的对
     * 删的,记在 invalidMask 里,恢复只认 ownRemovalBits),再把状态标成 Skipped。
     *
     * 以前筛选里的 Key_Skip 只处理 Pending:一个已经 remove B 的对按空格毫无反应,
     * 必须先"恢复为 Pending"再按一次 —— 用户明确报的就是这条。
     */
    if (!p) return false;

    const bool hasA = (p->idxA >= 0 && p->idxA < (int)m_files.size());
    const bool hasB = (p->idxB >= 0 && p->idxB < (int)m_files.size());

    std::lock_guard<std::mutex> lkState(p->stateMutex);

    const quint8 own = ownRemovalBits(p->state, p->removedMask);
    if (hasA && (own & SideBitA)) {
        const int idx = p->idxA;
        if (restoreFromDuplicate(idx, p))
            markRelatedPairsValidState(m_files[idx], p, true);
    }
    if (hasB && (own & SideBitB)) {
        const int idx = p->idxB;
        if (restoreFromDuplicate(idx, p))
            markRelatedPairsValidState(m_files[idx], p, true);
    }

    /*
     * removedMask 清零:本对已经没有"删过谁"这件事了。
     * state 固定成 Skipped —— 即便 invalidMask 里还有一侧是别的对删的,
     * 那件事由 invalidMask 表达(列表显示成 "Skipped + InvalidA/B"),
     * 不需要(也不应该)把它翻译成 Invalid*:用户的意思是"跳过这一对"。
     */
    p->removedMask = 0;
    p->state = PairState::Skipped;
    p->decisionSeq = ++m_decisionSeq;
    return true;
}

bool ImageDeduplicateWindow::filterSkipMember(int pos, bool navigate)
{
    if (pos < 0 || pos >= (int)m_filter.members.size())
        return false;

    const int pairIndex = m_filter.members[pos].pairIndex;
    if (pairIndex < 0 || pairIndex >= (int)m_pairs.size())
        return false;
    std::shared_ptr<PairInfo> p = m_pairs[pairIndex];
    if (!p) return false;

    if (!markPairSkippedUndoingOwnRemoval(p)) {
        createFloatingLabel(this, QStringLiteral("跳过失败(看日志)"));
        logLine(QStringLiteral("[%1] 筛选: pair %2 跳过失败").arg(nowStr()).arg(pairIndex), true, 3);
        return false;
    }
    dedupenv::playSound(DedupSound::SingleAction);

    refreshFilterList();

    if (navigate) {
        const int next = nextActionableMember(pos, +1);
        if (next >= 0) {
            displayPairByIndex(m_filter.members[next].pairIndex, m_dataEpoch);
            updateFilterUi();
        } else {
            createFloatingLabel(this, QStringLiteral("筛选里没有剩下的可处理项了,点 finished 退出"));
        }
    }

    logLine(QStringLiteral("[%1] 筛选: pair %2 跳过(已撤销本对删过的那一侧)")
                .arg(nowStr()).arg(pairIndex));
    return true;
}

void ImageDeduplicateWindow::filterApplyAction(ActionRecord::Type type)
{
    if (!m_filter.active) return;

    const int current = m_currentIndex;
    if (filterMemberPos(current) < 0) {
        createFloatingLabel(this, QStringLiteral("当前这一对不在筛选集合里"));
        return;
    }
    /*
     * 统一下沉到"只对某一行"的实现:按钮/快捷键和右键走同一套语义,
     * 免得两条路对"remove both 在筛选里是什么意思"给出不同答案。
     * navigate=true:处理完自动前进到下一个还能处理的行。
     */
    filterPairAction(current, type);
}

void ImageDeduplicateWindow::filterPairAction(int pairIndex, ActionRecord::Type type)
{
    if (!m_filter.active) return;
    if (blockedByFolderDecision(QStringLiteral("筛选动作"))) return;

    const int pos = filterMemberPos(pairIndex);
    if (pos < 0) {
        createFloatingLabel(this, QStringLiteral("这一对不在筛选集合里"));
        return;
    }

    const bool navigate = (pairIndex == m_currentIndex);

    /*
     * RemoveB / RemoveBoth 在筛选里是同一件事:锚点在进入筛选时已经删掉了,
     * 这一行唯一还能删的就是"另一侧"(钉住后在 B 位)。
     * 菜单里两个项都留着(和普通列表一致),但语义不再分叉。
     */
    if (type == ActionRecord::RemoveB || type == ActionRecord::RemoveBoth) {
        filterRemoveOtherSide(pos, navigate);
        return;
    }
    if (type == ActionRecord::Skip) {
        filterSkipMember(pos, navigate);
        return;
    }
    if (type == ActionRecord::RemoveA) {
        /*
         * Remove A 在筛选里等于"删锚点":它已经在进入时按用户的选择处理过了。
         * 要改主意(比如从 remove both 改成 remove A),finished 退出后在原对上
         * 按 d 就行 —— applyActionToPair 的 RemoveA 会自动把 B 恢复回来。
         */
        createFloatingLabel(this, QStringLiteral("锚点已按进入时的选择处理;要改主意请 finished 退出后在原对上改"));
        return;
    }
    if (type != ActionRecord::None)
        return;

    /* ---- 恢复为 Pending ---- */
    std::shared_ptr<PairInfo> p;
    {
        if (pairIndex < 0 || pairIndex >= (int)m_pairs.size()) return;
        p = m_pairs[pairIndex];
    }
    if (!p) return;

    /* restorePairToPending 内部自己拿 stateMutex,这里不能先锁住 */
    if (!restorePairToPending(p)) {
        createFloatingLabel(this, QStringLiteral("恢复失败:这一对的图是被别的对移走的,"
                                                "请到肇事的那一对上修改"));
        return;
    }

    /*
     * 恢复之后这一行可能重新变成"可处理"(另一侧回来了),
     * 也可能因为锚点已删而不再属于筛选集合 —— 整表重建最省事。
     */
    refreshFilterList();
    updateFilterUi();
    if (navigate) {
        const int again = filterMemberPos(pairIndex);
        const int next = nextActionableMember(again, +1);
        if (next >= 0)
            displayPairByIndex(m_filter.members[next].pairIndex, m_dataEpoch);
    }
    logLine(QStringLiteral("[%1] 筛选: pair %2 恢复为 Pending").arg(nowStr()).arg(pairIndex));
}

/*
 * 筛选状态下的批量动作(Ctrl+A 全选 -> a/s/空格,或框选多行 -> 右键)。
 *
 * 和单行路径同一套规则,只是:
 *   · 不在筛选集合里的目标直接跳过(批量选择可能跨到集合之外);
 *   · 失败时不出浮动提示(几十行同时弹会糊住屏幕),只累计数量写日志;
 *   · 列表只在最后重建一次 —— refreshFilterList 是整表重建,
 *     逐行调用于 35000 对的会话就是几十次 O(N)。
 */
void ImageDeduplicateWindow::filterBatchAction(const QVector<int> &pairIndices,
                                              ActionRecord::Type type)
{
    if (!m_filter.active) return;
    if (blockedByFolderDecision(QStringLiteral("筛选批量操作"))) return;

    if (type != ActionRecord::None && type != ActionRecord::RemoveB
        && type != ActionRecord::RemoveBoth && type != ActionRecord::Skip) {
        createFloatingLabel(this, QStringLiteral("筛选里只能移除\"另一侧\"、跳过,或恢复为 Pending"));
        logLine(QStringLiteral("[%1] 筛选批量:忽略不支持的动作 %2")
                    .arg(nowStr()).arg(int(type)), true);
        return;
    }

    QVector<int> ordered = pairIndices;
    std::sort(ordered.begin(), ordered.end());
    ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());

    /*
     * 批量路径不像 applyActionToCurrentPair 那样自带锁:这里按标准锁序
     * (m_mutex 共享 -> currentIndexMutex)自己拿,并在整段里登记 MMutexScope。
     * 不拿锁的话,restorePairToPending / markRelatedPairsValidState 会一边遍历
     * m_pairs 一边被后台线程改(注释里所有"应外部加锁"都是这个意思)。
     * 末尾的 displayPairByIndex 依赖 MMutexScope 才不会重复加共享锁。
     */
    std::shared_lock<std::shared_mutex> lk(m_mutex);
    MMutexScope mScope(g_iwdMMutexDepth);
    std::lock_guard<std::mutex> lkIndex(currentIndexMutex);

    int applied = 0;
    int skipped = 0;
    for (int pairIndex : std::as_const(ordered)) {
        if (pairIndex < 0 || pairIndex >= (int)m_pairs.size()) { ++skipped; continue; }
        if (filterMemberPos(pairIndex) < 0) { ++skipped; continue; }   // 不在筛选集合里

        std::shared_ptr<PairInfo> p = m_pairs[pairIndex];
        if (!p) { ++skipped; continue; }

        if (type == ActionRecord::None) {
            /*
             * 恢复不再一律拒绝失效对:restorePairToPending 现在只撤销【本对】删过的图,
             * 本对一只都没删(纯失效)时它自己会返回 false —— 交给它判断即可。
             */
            if (restorePairToPending(p)) ++applied;
            else                         ++skipped;
            continue;
        }

        if (type == ActionRecord::Skip) {
            /*
             * 跳过:撤销本对删过的那一侧 + 标 Skipped。
             * 以前这里直接拒绝 Skip(理由是"和筛选语义重叠"),于是用户想
             * "把这个删过的对改成 jumped"只能先逐个恢复 Pending —— 已修。
             */
            if (markPairSkippedUndoingOwnRemoval(p)) ++applied;
            else                                     ++skipped;
            continue;
        }

        /* RemoveB / RemoveBoth:删掉"另一侧"(钉住之后必在 B 位) */
        bool ok = false;
        {
            std::lock_guard<std::mutex> lkState(p->stateMutex);
            const int idxB = p->idxB;
            if (idxB < 0 || idxB >= (int)m_files.size()
                || m_files[idxB].inDuplicate || m_files[idxB].currentPath.isEmpty()) {
                ok = false;                        // 另一侧已经没了
            } else {
                ok = removeSideOfPair(p, /*sideA=*/false);
            }
        }
        if (ok) ++applied;
        else    ++skipped;
    }

    if (applied > 0)
        dedupenv::playSound(DedupSound::BatchAction);

    refreshFilterList();
    updateFilterUi();

    logLine(QStringLiteral("[%1] 筛选批量(%2):处理 %3 / %4(跳过 %5)")
                .arg(nowStr(), actionName(type)).arg(applied).arg(ordered.size()).arg(skipped), true);

    /*
     * 当前显示的那一对如果已经不在了/不可处理,走到下一个可处理项;
     * 没有就原地不动(筛选不自动退出,和 nextActionableMember 的口径一致)。
     */
    const int pos = filterMemberPos(m_currentIndex);
    if (pos < 0) {
        const int next = nextActionableMember(-1, +1);
        if (next >= 0) {
            displayPairByIndex(m_filter.members[next].pairIndex, m_dataEpoch);
            updateFilterUi();
        }
    } else if (nextActionableMember(pos - 1, +1) != pos) {
        /* 当前行自己已经不可处理了:别把一对"没东西可删"的图留在屏幕上 */
        const int next = nextActionableMember(pos, +1);
        if (next >= 0) {
            displayPairByIndex(m_filter.members[next].pairIndex, m_dataEpoch);
            updateFilterUi();
        }
    }
}

void ImageDeduplicateWindow::refreshFilterList()
{
    rebuildPairList();
    syncPairListSelectionNoLock(DuplicatePairListView::ScrollNever);
}

void ImageDeduplicateWindow::updateFilterUi()
{
    // 列表右键菜单要靠它决定"放哪些项"(筛选状态下只留 remove B/both)
    if (m_pairList) {
        m_pairList->setFilterMode(m_filter.active);
        m_pairList->setFilterAvailable(m_fileOpMode != FileOpMode::Deferred);
    }

    if (!ui.btn_rm_a) return;

    if (!m_filter.active) {
        ui.btn_rm_a->setText(QStringLiteral("remove A"));
        if (ui.btn_rm_b)    ui.btn_rm_b->setEnabled(true);
        if (ui.btn_rm_both) ui.btn_rm_both->setEnabled(true);
        return;
    }

    ui.btn_rm_a->setText(QStringLiteral("finished"));

    /*
     * 标题:筛选模式必须一眼可见,否则用户会以为整个列表就剩这么几对。
     * 剩余数量按"B 侧还在"算,和 nextActionableMember 的口径一致。
     */
    int left = 0;
    for (const FilterMember &m : m_filter.members) {
        const int pi = m.pairIndex;
        if (pi < 0 || pi >= (int)m_pairs.size()) continue;
        const auto &p = m_pairs[pi];
        if (!p) continue;
        int idxB = -1;
        {
            std::lock_guard<std::mutex> lkState(p->stateMutex);
            idxB = p->idxB;
        }
        if (idxB >= 0 && idxB < (int)m_files.size()
            && !m_files[idxB].inDuplicate && !m_files[idxB].currentPath.isEmpty())
            ++left;
    }

    QString anchorName = QStringLiteral("?");
    const int firstAnchor = m_filter.anchorFiles.value(0, -1);
    if (firstAnchor >= 0 && firstAnchor < (int)m_files.size())
        anchorName = QFileInfo(m_files[firstAnchor].currentPath).fileName();
    if (m_filter.anchorFiles.size() > 1) {
        const int second = m_filter.anchorFiles[1];
        if (second >= 0 && second < (int)m_files.size()) {
            anchorName += QStringLiteral(" + ")
                          + QFileInfo(m_files[second].currentPath).fileName();
        }
    }
    setWindowTitle(QStringLiteral("FILTER %1  —  %2/%3 left  (finished to exit)")
                       .arg(anchorName).arg(left).arg(m_filter.members.size()));
}

void ImageDeduplicateWindow::finishSimilarityFilter()
{
    if (!m_filter.active) return;

    std::shared_lock<std::shared_mutex> lk(m_mutex);
    MMutexScope mScope(g_iwdMMutexDepth);
    std::lock_guard<std::mutex> lkIndex(currentIndexMutex);

    /*
     * 文件动作在进入筛选那一刻就做完了(原对按用户选的动作处理,成员按需删另一侧)。
     * 退出时只剩记账:把 A/B 换回去、复位会话、回到原对或下一个待处理对。
     */
    int leftToDo = 0;
    for (const FilterMember &m : m_filter.members) {
        if (m.pairIndex < 0 || m.pairIndex >= (int)m_pairs.size()) continue;
        const auto &p = m_pairs[m.pairIndex];
        if (!p) continue;
        int otherIdx = -1;
        {
            std::lock_guard<std::mutex> lkState(p->stateMutex);
            otherIdx = (p->idxA == m.anchorFile) ? p->idxB : p->idxA;
        }
        if (otherIdx >= 0 && otherIdx < (int)m_files.size()
            && !m_files[otherIdx].inDuplicate && !m_files[otherIdx].currentPath.isEmpty()) {
            ++leftToDo;
        }
    }

    /* ---- 1) A/B 换回去 ---- */
    for (auto it = m_filter.members.rbegin(); it != m_filter.members.rend(); ++it) {
        if (!it->swapped) continue;
        if (it->pairIndex < 0 || it->pairIndex >= (int)m_pairs.size()) continue;
        const std::shared_ptr<PairInfo> &p = m_pairs[it->pairIndex];
        if (!p) continue;
        std::lock_guard<std::mutex> lkState(p->stateMutex);
        /* 只有锚点确实还在 A 位时才换回来(否则说明这一对已经被别的路径动过) */
        if (p->idxA != it->anchorFile) continue;
        swapPairSides(p);
    }

    /* ---- 2) 复位会话 ---- */
    const int originIndex = m_filter.originPairIndex;
    const std::shared_ptr<PairInfo> originPair = m_filter.originPair;
    /* 记下进来的方式,退出日志里写清楚这一轮是"哪种筛选"(排查时很重要) */
    const ActionRecord::Type originType =
        (m_filter.mode == FilterFromA) ? ActionRecord::RemoveA
        : (m_filter.mode == FilterFromB ? ActionRecord::RemoveB
                                        : ActionRecord::RemoveBoth);

    m_filter = PairFilterSession();
    updateFilterUi();
    rebuildPairList();

    /* ---- 3) 落点:原对(可能被改回 Pending)或它之后的下一个 Pending ---- */
    int target = -1;
    if (originIndex >= 0 && originIndex < (int)m_pairs.size()
        && m_pairs[originIndex] == originPair) {
        std::lock_guard<std::mutex> lkState(originPair->stateMutex);
        if (originPair->state == PairState::Pending)
            target = originIndex;
    }
    if (target < 0) {
        for (int i = qMax(0, originIndex); i < (int)m_pairs.size(); ++i) {
            std::lock_guard<std::mutex> lkState(m_pairs[i]->stateMutex);
            if (m_pairs[i]->state == PairState::Pending) { target = i; break; }
        }
    }
    if (target < 0)
        target = findNextPending();

    if (target >= 0 && target < (int)m_pairs.size())
        displayPairByIndex(target, m_dataEpoch);
    else
        showNoMorePairs();

    logLine(QStringLiteral("[%1] 退出筛选(%2): 回到 pair %3, 筛选内还剩 %4 对未处理")
                .arg(nowStr(), actionName(originType))
                .arg(target).arg(leftToDo), true);
    createFloatingLabel(this, QStringLiteral("已退出筛选"));
}

/* ==========================================================================
 *  半选 internal:目录级"合集优先"判定
 * ========================================================================== */

QString ImageDeduplicateWindow::topDirPathOfFile(int fileIdx) const
{
    if (fileIdx < 0 || fileIdx >= (int)m_files.size())
        return QString();
    const FileEntry &fe = m_files[fileIdx];
    const QString sub = dedup::topLevelSubDirOf(fe.rootInputFolder, fe.currentPath);
    if (sub.isEmpty())
        return QString();   // 直接放在输入目录下的文件不属于任何顶层子目录
    return QDir::cleanPath(fe.rootInputFolder + QLatin1Char('/') + sub).toLower();
}

QString ImageDeduplicateWindow::parentDirPathOfFile(int fileIdx) const
{
    /*
     * 直属父目录(小写规范化)。原来这套写法散在 jumpForPair / 筛选 /
     * goToNextFolderRegionPair 的 lambda 里,排序分组键改用直属父目录后
     * 收拢成一份 —— 四处的判等必须永远一致,不然"排序聚簇的区域"和
     * "跳转认的区域"会对不上。
     */
    if (fileIdx < 0 || fileIdx >= (int)m_files.size())
        return QString();
    const QString p = m_files[fileIdx].currentPath;
    if (p.isEmpty())
        return QString();
    return QDir::cleanPath(QFileInfo(p).absolutePath()).toLower();
}

bool ImageDeduplicateWindow::preferFileAsA(int ia, int ib) const
{
    if (ia < 0 || ia >= (int)m_files.size() || ib < 0 || ib >= (int)m_files.size())
        return ia <= ib;

    const FileEntry &A = m_files[ia];
    const FileEntry &B = m_files[ib];

    // 1) 目录级整包质量:同一套作品应当整包留在质量更高的目录里
    const QString dirA = topDirPathOfFile(ia);
    const QString dirB = topDirPathOfFile(ib);
    if (!dirA.isEmpty() && !dirB.isEmpty() && dirA != dirB) {
        const int sa = m_dirScore.value(dirA, 0);
        const int sb = m_dirScore.value(dirB, 0);
        if (sa != sb) return sa > sb;
    }

    // 2) 单张指标
    if (A.quality != B.quality) return A.quality > B.quality;
    if (A.filesize != B.filesize) return A.filesize > B.filesize;

    const qint64 pa = qint64(A.size.width()) * A.size.height();
    const qint64 pb = qint64(B.size.width()) * B.size.height();
    if (pa != pb) return pa > pb;

    // 3) 路径深度、所属目录的递归图片数
    const int da = A.currentPath.count(QLatin1Char('/'));
    const int db = B.currentPath.count(QLatin1Char('/'));
    if (da != db) return da > db;      // 更深的一侧放 A

    const int ca = m_dirImageCount.value(dirA);
    const int cb = m_dirImageCount.value(dirB);
    if (ca != cb) return ca > cb;      // 目录图片更多的一侧放 A

    return ia < ib;                    // 稳定兜底
}

QString ImageDeduplicateWindow::pairDirSortKey(int fileIdxA, int fileIdxB) const
{
    /*
     * 分组粒度 = A、B 各自的**直属父目录**(2026-10-28 起,以前是顶层子目录)。
     * D/fa/sub1 ↔ D/fb/sub1 和 D/fa/sub2 ↔ D/fb/sub1 由此分成两个组,
     * "下一个目录对比区域"跳一步就能到;合集方向(m_folderKeep)仍是
     * 顶层子目录粒度,由 folderKeepDirFor 单独按 topDir 判,互不影响。
     */
    return foldercompare::folderPairSortKey(parentDirPathOfFile(fileIdxA),
                                            parentDirPathOfFile(fileIdxB));
}

QHash<QString, foldergroup::Stat> ImageDeduplicateWindow::folderGroupStats() const
{
    QHash<QString, foldergroup::Stat> out;
    if (m_pairs.empty() || m_files.empty())
        return out;

    foldergroup::Thresholds th;
    th.pHash = m_pHashThreshold;
    th.dHash = m_dHashThreshold;
    th.orb   = m_orbMinInlierRatio;

    /*
     * 先按"目录对"把原始记录攒起来。分组键与排序用的 dirKey 同一种规范化
     * (粒度 = A、B 各自的直属父目录,与 pairDirSortKey 一致),
     * 分桶本身(哪张图属于哪一侧、参照侧是谁)在 foldergroup::summarizeDirPair 里,
     * 那是纯函数、有单测 —— 这里只负责从窗口状态里取数。
     */
    QHash<QString, QVector<foldergroup::RawPair>> grouped;
    QHash<QString, QString> keepOf;   // 目录对键 -> 合集侧目录
    grouped.reserve(int(m_pairs.size() / 4) + 8);
    keepOf.reserve(int(m_pairs.size() / 8) + 8);

    for (const std::shared_ptr<PairInfo> &p : m_pairs) {
        if (!p) continue;

        int ia = -1, ib = -1, ph = 0, dh = 0;
        double orb = 0.0;
        {
            std::lock_guard<std::mutex> lkState(p->stateMutex);
            ia = p->idxA;
            ib = p->idxB;
            ph = p->pHashDistance;
            dh = p->dHashDistance;
            orb = p->orbMatchRatio;
        }

        const QString da = parentDirPathOfFile(ia);
        const QString db = parentDirPathOfFile(ib);
        if (da.isEmpty() || db.isEmpty() || da == db)
            continue;   // 同一直属目录内的对(或拿不到路径)不参与组间排序

        const QString key = foldercompare::folderPairSortKey(da, db);
        if (key.isEmpty())
            continue;

        grouped[key].push_back(foldergroup::RawPair{da, db, ia, ib, ph, dh, orb});
        if (!keepOf.contains(key)) {
            /*
             * m_folderKeep 是**顶层子目录**粒度(FolderCompare 比的就是顶层目录),
             * 而组的侧目录现在是直属父目录 —— summarizeDirPair 里是拿
             * keepDir == 组侧目录 判方向的,必须把顶层粒度的 keep 落到
             * 本组的具体侧上:哪个直属父目录在 keep 目录之下,那一侧就是合集侧。
             * 同一细分组里两个直属父目录必然分属两个不同顶层子目录
             * (同顶层的话 dirPairKeyOf 是空串,根本不会有 keep),不会两边都命中。
             */
            const QString keepTop = folderKeepDirFor(ia, ib);
            if (!keepTop.isEmpty()) {
                const QString keepPrefix = keepTop + QLatin1Char('/');
                const QString pa = parentDirPathOfFile(ia);
                const QString pb = parentDirPathOfFile(ib);
                if (pa == keepTop || pa.startsWith(keepPrefix))
                    keepOf.insert(key, pa);
                else if (pb == keepTop || pb.startsWith(keepPrefix))
                    keepOf.insert(key, pb);
            }
        }
    }

    out.reserve(grouped.size());
    for (auto it = grouped.constBegin(); it != grouped.constEnd(); ++it) {
        out.insert(it.key(),
                   foldergroup::summarizeDirPair(it.value(), keepOf.value(it.key()), th));
    }
    return out;
}

QSet<QString> ImageDeduplicateWindow::jumpedFolderPairKeys() const
{
    QSet<QString> keys;
    for (int i : m_folderManualSkipped) {
        if (i < 0 || i >= m_folderManualDirs.size()) continue;
        const QString k = dirPairKeyOf(m_folderManualDirs[i].first,
                                       m_folderManualDirs[i].second);
        if (!k.isEmpty())
            keys.insert(k);
    }
    return keys;
}

int ImageDeduplicateWindow::dropPairsBetweenJumpedFolders()
{
    const QSet<QString> jumped = jumpedFolderPairKeys();
    if (jumped.isEmpty() || m_pairs.empty())
        return 0;

    /*
     * 记下当前显示的那一对:它有可能正好落在被剔掉的组里。
     * 直接 erase 之后 m_currentIndex 会指向"下一对"或越界,后面那次
     * sortPairsFromIndex 按 shared_ptr 找 curPair 也找不到(它已经不在表里),
     * 于是 m_currentIndex 会保留一个错位值。
     */
    std::shared_ptr<PairInfo> curPair;
    if (m_currentIndex >= 0 && m_currentIndex < (int)m_pairs.size())
        curPair = m_pairs[m_currentIndex];

    const size_t before = m_pairs.size();
    m_pairs.erase(std::remove_if(m_pairs.begin(), m_pairs.end(),
                                 [this, &jumped](const std::shared_ptr<PairInfo> &p) {
                                     if (!p) return true;
                                     int ia = -1, ib = -1;
                                     {
                                         std::lock_guard<std::mutex> lkState(p->stateMutex);
                                         ia = p->idxA;
                                         ib = p->idxB;
                                     }
                                     return folderscope::isJumpedPair(
                                         jumped, topDirPathOfFile(ia), topDirPathOfFile(ib));
                                 }),
                   m_pairs.end());

    const int dropped = int(before - m_pairs.size());
    if (dropped <= 0)
        return 0;

    const bool curStillThere = curPair
        && std::find(m_pairs.begin(), m_pairs.end(), curPair) != m_pairs.end();
    if (!curStillThere) {
        std::lock_guard<std::mutex> lkIndex(currentIndexMutex);
        /*
         * 当前这一对正好被剔掉了(它属于被 jump 的那两个目录之间),
         * 只能退回表头 —— 这是唯一一种"必须换位置"的情况。
         */
        m_currentIndex = 0;
    }
    return dropped;
}

void ImageDeduplicateWindow::startPixelDiffPhaseForFolderMode(int epoch)
{
    /*
     * 半选 internal 的特殊之处:建 pair 是在 FolderCompareWidget 之后才发生的
     * (目录方向没定之前不能摆 A/B,否则用户选什么都白选),所以 pix diff
     * 在扫描阶段一对都还没算。
     *
     * 这就意味着"算完 pix diff 再整体重排"在半选模式下是第一次、也是唯一一次
     * 按相似度细化排序的机会:排序键的第一位是目录对,重排只会在组内调整,
     * 不会把用户已经开始处理的分组打乱。
     *
     * 和普通路径一样放到后台线程里做,避免几十对 32MP 图片把 UI 卡死。
     */
    if (!isFolderCompareMode() || m_pixDiffPhaseStarted)
        return;
    m_pixDiffPhaseStarted = true;

    m_pixDiffDoneCount.store(0);
    m_pixDiffExpectedCount.store(0);
    setProgressPhasePixelDiff();

    QPointer<ImageDeduplicateWindow> self(this);
    std::thread([self, epoch]() {
        if (!self) return;
        try {
            self->computeAllPixelDiffsAndResort(epoch);
        } catch (const std::exception &e) {
            recordTaskException("computeAllPixelDiffsAndResort(folder)", e.what());
        } catch (...) {
            recordTaskException("computeAllPixelDiffsAndResort(folder)", "non-std exception");
        }
    }).detach();
}

QString ImageDeduplicateWindow::folderKeepDirFor(int fileIdxA, int fileIdxB) const
{
    if (m_folderKeep.isEmpty())
        return QString();

    const QString da = topDirPathOfFile(fileIdxA);
    const QString db = topDirPathOfFile(fileIdxB);
    const QString key = dirPairKeyOf(da, db);
    if (key.isEmpty())
        return QString();
    return m_folderKeep.value(key);
}

void ImageDeduplicateWindow::recordFolderDecision(const QString &dirKeep, const QString &dirSub)
{
    if (dirKeep.isEmpty() || dirSub.isEmpty())
        return;

    const QString keep = QDir::cleanPath(dirKeep).toLower();
    const QString key = dirPairKeyOf(keep, QDir::cleanPath(dirSub).toLower());
    if (key.isEmpty())
        return;

    m_folderKeep.insert(key, keep);
}

void ImageDeduplicateWindow::clearFolderDecision(const QString &dirA, const QString &dirB)
{
    const QString key = dirPairKeyOf(QDir::cleanPath(dirA).toLower(),
                                     QDir::cleanPath(dirB).toLower());
    if (key.isEmpty())
        return;
    if (m_folderKeep.remove(key))
        logLine(QStringLiteral("[%1] 目录方向已撤销: %2 vs %3")
                    .arg(nowStr(), QFileInfo(dirA).fileName(), QFileInfo(dirB).fileName()));
}

void ImageDeduplicateWindow::analyzeInternalFolders()
{
    m_folderKeep.clear();
    m_folderManualViews.clear();
    m_folderManualDirs.clear();
    m_folderManualSkipped.clear();
    m_folderManualIndex = 0;

    // 只有半勾选(AcrossTopDirs)的输入目录才做合集判定
    QStringList roots;
    for (const FileEntry &fe : std::as_const(m_files)) {
        if (fe.internalMode != static_cast<int>(InternalDedup::AcrossTopDirs)) continue;
        if (!roots.contains(fe.rootInputFolder))
            roots << fe.rootInputFolder;
    }
    if (roots.isEmpty()) {
        logLine(QStringLiteral("[%1] 目录级合集判定: 没有半勾选(cb_internal 半选)的输入目录,跳过")
                    .arg(nowStr()));
        return;
    }
    logLine(QStringLiteral("[%1] 目录级合集判定: %2 个半勾选输入目录")
                .arg(nowStr()).arg(roots.size()));

    const QStringList exts = dedup::supportedExtensions();
    int cmpCount = 0;

    /*
     * ORB 匹配率的会话级缓存。
     * analyze() 里每对目录都要跑两遍贪心匹配(n*m 次相似度打分),同一对图片会被
     * 反复问到;而 verifyOrbPair 每次都是一次完整的 BFMatcher::knnMatch + RANSAC。
     * 目录多、图片多时这里不缓存就是纯浪费 —— 提示词 §8 明确要求"不要重复计算同一特征"。
     * key = 两个文件下标(无序)打包成 64 位,所以一次扫描里每对图片最多算一次。
     */
    QHash<quint64, foldercompare::OrbMatch> orbCache;
    int orbComputed = 0, orbRequested = 0;

    QVector<foldercompare::ImageItem> items;
    QVector<int> fileIdxOfItem;

    for (int i = 0; i < (int)m_files.size(); ++i) {
        const FileEntry &fe = m_files[i];
        // 只有半勾选的输入目录参与(其它输入目录按各自的 internal 规则走)
        if (fe.internalMode != static_cast<int>(InternalDedup::AcrossTopDirs)) continue;

        // 只让图片参与(PSD/ZIP/GIF/MP4 等资源文件不进入目录相似度统计)
        const QString sfx = QFileInfo(fe.currentPath).suffix().toLower();
        if (!exts.contains(sfx)) continue;

        foldercompare::ImageItem it;
        it.path = fe.currentPath;
        it.quality = fe.quality;
        it.filesize = fe.filesize;
        it.width = fe.size.width();
        it.height = fe.size.height();
        it.pHash = fe.pHash;
        it.dHash = fe.dHash;
        it.sha = QByteArray(fe.SHA256, sizeof(fe.SHA256));

        items << it;
        fileIdxOfItem << i;
    }
    if (items.size() < 2) {
        logLine(QStringLiteral("[%1] 目录级合集判定: 参与比较的图片不足 2 张,跳过").arg(nowStr()));
        return;
    }

    foldercompare::Options opt;
    opt.pHashThreshold = m_pHashThreshold;
    opt.dHashThreshold = m_dHashThreshold;
    opt.orbMinInliers  = m_orbMinInliers;    // 与 buildPairsFromFiles 完全一致
    opt.orbMinRatio    = m_orbMinInlierRatio;
    opt.orbMatch = [this, fileIdxOfItem, &orbCache, &orbComputed, &orbRequested]
                   (int a, int b) -> foldercompare::OrbMatch {
        const int fa = fileIdxOfItem.value(a, -1);
        const int fb = fileIdxOfItem.value(b, -1);
        if (fa < 0 || fb < 0)
            return {};
        ++orbRequested;

        const quint32 lo = quint32(qMin(fa, fb));
        const quint32 hi = quint32(qMax(fa, fb));
        const quint64 key = (quint64(hi) << 32) | lo;
        const auto hit = orbCache.constFind(key);
        if (hit != orbCache.constEnd())
            return hit.value();

        foldercompare::OrbMatch r;
        if (!m_files[fa].orbDescriptors.empty() && !m_files[fb].orbDescriptors.empty()) {
            dedup::OrbVerifyOptions vopt;
            vopt.minInliers     = m_orbMinInliers;
            vopt.minInlierRatio = m_orbMinInlierRatio;
            const dedup::OrbVerifyResult v = dedup::verifyOrbPair(
                m_files[fa].orbDescriptors, m_files[fa].orbPoints,
                m_files[fb].orbDescriptors, m_files[fb].orbPoints, vopt);
            r.inliers     = v.inliers;
            r.inlierRatio = v.inlierRatio;
            /* 紧接着的 buildPairsFromFiles 会问同一对,别让它再算一遍 */
            m_orbVerifyCache.insert(key, v);
        }
        orbCache.insert(key, r);
        ++orbComputed;
        return r;
    };

    const foldercompare::Analysis an = foldercompare::analyze(roots, items, opt);
    cmpCount += an.comparisons.size();
    for (const foldercompare::Comparison &cmp : an.comparisons) {
        if (cmp.a < 0 || cmp.b < 0) continue;
        const foldercompare::DirEntry &A = an.dirs[cmp.a];
        const foldercompare::DirEntry &B = an.dirs[cmp.b];

        switch (cmp.decision) {
        case foldercompare::Decision::KeepA:
        case foldercompare::Decision::KeepB: {
            const bool keepA = (cmp.decision == foldercompare::Decision::KeepA);
            const QString keepDir = keepA ? A.path : B.path;
            const QString subDir  = keepA ? B.path : A.path;
            recordFolderDecision(keepDir, subDir);
            logLine(QStringLiteral("[%1] 合集判定: 保留 %2 / 子集 %3 (%4)")
                        .arg(nowStr(), QFileInfo(keepDir).fileName(),
                             QFileInfo(subDir).fileName(), cmp.note));
            break;
        }
        case foldercompare::Decision::None:
            logLine(QStringLiteral("[%1] 目录对 %2 vs %3: 无合集关系(相似 %4/%5)")
                        .arg(nowStr(), QFileInfo(A.path).fileName(), QFileInfo(B.path).fileName())
                        .arg(cmp.bToA.matched).arg(cmp.bToA.nB));
            break;
        case foldercompare::Decision::Manual:
            m_folderManualViews << foldercompare::makeView(cmp, A, B, an);
            m_folderManualDirs << qMakePair(A.path, B.path);
            logLine(QStringLiteral("[%1] 合集判定需要人工: %2 vs %3 (%4)")
                        .arg(nowStr(), QFileInfo(A.path).fileName(),
                             QFileInfo(B.path).fileName(), cmp.note));
            break;
        default:
            break;
        }
    }
    logLine(QStringLiteral("[%1] 目录级合集判定完成: 共 %2 个目录对, 自动定方向 %3 个, 需要人工 %4 个")
                .arg(nowStr()).arg(cmpCount).arg(m_folderKeep.size()).arg(m_folderManualViews.size()));
    logLine(QStringLiteral("[%1] ORB 匹配: 请求 %2 次, 实际计算 %3 次(命中缓存 %4 次)")
                .arg(nowStr()).arg(orbRequested).arg(orbComputed)
                .arg(orbRequested - orbComputed));
}

void ImageDeduplicateWindow::applyFolderOrdering()
{
    if (m_folderKeep.isEmpty())
        return;
    /*
     * Deferred(导入整理)里 A/B 有"结构基准/整理目标"的含义,不能为了排序对调;
     * remove cover 的 loadPairSession 也是 Deferred。
     */
    if (m_fileOpMode != FileOpMode::Immediate || m_importSessionActive)
        return;

    std::unique_lock<std::shared_mutex> lk(m_mutex);
    MMutexScope mScope(g_iwdMMutexDepth);

    int swapped = 0;
    for (const shared_ptr<PairInfo> &p : m_pairs) {
        if (!p) continue;
        std::lock_guard<std::mutex> lkState(p->stateMutex);
        if (p->state != PairState::Pending) continue;

        const QString keep = folderKeepDirFor(p->idxA, p->idxB);
        if (keep.isEmpty()) continue;

        const QString topA = topDirPathOfFile(p->idxA);
        const QString topB = topDirPathOfFile(p->idxB);
        if (topA == keep || topB != keep) continue;

        // 让合集侧排在 A 位:remove B(= 保留 A)才是"保留合集"
        std::swap(p->idxA, p->idxB);
        p->displayPairReady = false;
        p->displayPairA = QImage();
        p->displayPairB = QImage();
        ++swapped;
    }

    if (swapped > 0) {
        logLine(QStringLiteral("[%1] 合集优先: %2 对按合集方向重排").arg(nowStr()).arg(swapped));
        rebuildPairList();
    }
}

void ImageDeduplicateWindow::finishFolderDecisionsAndBuildPairs()
{
    if (!m_waitingFolderDecision)
        return;

    QElapsedTimer finishTimer;
    finishTimer.start();

    int epoch = -1;
    {
        std::unique_lock<std::shared_mutex> lk(m_mutex);
        MMutexScope mScope(g_iwdMMutexDepth);
        m_waitingFolderDecision = false;
        epoch = m_folderPendingEpoch;
        m_folderPendingEpoch = -1;
        if (epoch != m_dataEpoch)
            return;
        /*
         * 这里**不再重建**图片对:扫描阶段就已经建好了(见 onWorkerScanFinished 的注释)。
         * 目录方向只决定两件事 —— A/B 谁在左、完全重复归谁,不改变"哪些对算相似"。
         *
         * 例外:被 jump 的目录对。用户按 jump 的意思是"这两个目录之间别比了",
         * 所以这一组的对要整个剔掉,而不是像以前那样尾插成"散图对"让用户逐对决定。
         * 必须在 applyFolderOrdering / autoMoveExactDuplicates 之前做 ——
         * 否则它们会被摆位、被当成完全重复的候选去搬文件。
         */
        const int dropped = dropPairsBetweenJumpedFolders();
        if (dropped > 0) {
            logLine(QStringLiteral("[%1] 已跳过 %2 对目录: 剔除它们之间的 %3 对相似图片")
                        .arg(nowStr()).arg(m_folderManualSkipped.size()).arg(dropped));
        }
    }

    /*
     * 顺序不能反(和 onWorkerScanFinished 里同一套理由):
     * 先把用户刚定的合集方向摆到 A 位,再移完全重复 ——
     * autoMoveExactDuplicates 只在"A 就是合集侧"时才动手。
     */
    applyFolderOrdering();

    int resMoved = 0;
    {
        std::unique_lock<std::shared_mutex> lk(m_mutex);
        MMutexScope mScope(g_iwdMMutexDepth);
        autoMoveExactDuplicates();
        // 人工选完方向后,那些"建对时还没方向"的字节相同对要在这里剔掉并搬走
        pruneDirectedExactDuplicates();
        resMoved = dedupeResourceFiles();
    }
    /*
     * 像素完全相同(pix==0)但字节不同的对,这里【不再】自动搬。
     *
     * 以前这一段会调 autoRemovePixelIdenticalPairs(),按合集方向把 pix==0 的对
     * 搬进 duplicate —— 于是这批对既不进比较列表,也没机会按新规则(留小的)处理。
     * 现在留给用户的 auto remove:用户按一下,按文件大小留小的那份;想保住合集
     * 就勾 Ignore Size(固定删 B)。用户没按就一直留在列表里,不会凭空消失。
     *
     * 为什么不在自动阶段替用户决定:这批对的"该留哪一侧"和合集方向可能相反
     * (合集那份如果是重编码出来的大文件),程序搬错了既没提示也没法回看。
     */
    if (resMoved > 0) {
        logLine(QStringLiteral("[%1] 资源文件: %2 个重复已移入 duplicate")
                    .arg(nowStr()).arg(resMoved));
    }

    // 摆位之后整表重排一次:目录对键与方向无关,但组内 A/B 变了要在列表里体现
    {
        /*
         * 这一步是 UI 线程上的 O(N log N)(35000 对要跑几十万次比较),
         * 一旦超过 500ms 就写一行,免得"选完目录界面像死了"又变成无头案。
         */
        QElapsedTimer sortTimer;
        sortTimer.start();
        sortPairsFromIndex(/*startIndex=*/0, epoch);
        const qint64 sortMs = sortTimer.elapsed();
        if (sortMs > 500) {
            logLine(QStringLiteral("[慢操作] 目录摆位后重排 %1 对耗时 %2ms")
                        .arg(m_pairs.size()).arg(sortMs), true);
        }
    }

    rebuildPairList();

    if (!m_pairs.empty()) {
        std::lock_guard<std::mutex> lk(currentIndexMutex);
        /*
         * 保持在用户原来看着的那一对上,不要跳。
         *
         * 这里以前是 findNextPending():它从 m_currentIndex + 1 开始往后找第一个
         * Pending,所以"目录选完之后列表总是跳到一个中间的索引位置"。
         * 重排已经按 shared_ptr 把 m_currentIndex 跟到同一个对上了,
         * 直接显示它即可 —— 浏览位置不该被"定目录方向"这件事挪走。
         */
        int start = m_currentIndex;
        if (start < 0 || start >= (int)m_pairs.size()) {
            start = findNextPending();   // 还没有当前对(理论上不会发生)
            if (start < 0 || start >= (int)m_pairs.size())
                start = 0;
        }
        displayPairByIndex(start, epoch);
        logLine(QStringLiteral("[%1] 目录方向已应用: %2 个目录对,仍定位在 pair %3")
                    .arg(nowStr()).arg(m_folderKeep.size()).arg(start));
    } else {
        showNoMorePairs();
    }

    if (!m_pixDiffPhaseStarted && !m_pairs.empty())
        startPixelDiffPhaseForFolderMode(epoch);

    createFloatingLabel(this, QStringLiteral("目录方向已应用: %1 对").arg(m_folderKeep.size()));

    if (finishTimer.elapsed() > 1000) {
        logLine(QStringLiteral("[慢操作] 目录选择收尾耗时 %1ms(含摆位/移重复/重排/重建列表)")
                    .arg(finishTimer.elapsed()), true);
    }

    /*
     * 人工选择全部结束 -> 弹一个确认窗口。
     *
     * 非模态(show 而不是 exec):这里刚做完文件移动/建对,后面还有 pix diff 要跑,
     * 用模态框会把整个流程卡住等用户点确定。用户看到"选了什么、移走了多少",
     * 点关闭即可。
     */
    {
        int manualCount = 0;
        for (int i = 0; i < m_folderManualDirs.size(); ++i) {
            if (!m_folderManualSkipped.contains(i))
                ++manualCount;
        }
        const int skippedCount = m_folderManualSkipped.size();

        QString text = QStringLiteral("目录选择已完成。\n\n"
                                      "· 目录对总数: %1\n"
                                      "· 已指定保留方向: %2\n"
                                      "· 已跳过(jump): %3\n"
                                      "· 参与比较的图片对: %4\n"
                                      "· 本次自动移入 duplicate: %5 张(完全重复)\n")
                           .arg(m_folderManualDirs.size())
                           .arg(manualCount)
                           .arg(skippedCount)
                           .arg(m_pairs.size())
                           .arg(m_exactDupMoves.size());

        if (skippedCount > 0) {
            text += QStringLiteral("\n被跳过的目录对之间**不做任何比较**:"
                                   "它们之间的相似图片不会进入列表,也不会搬动任何文件。");
        }
        text += QStringLiteral("\n\n点 OK 进入图片比较。");

        auto *box = new QMessageBox(QMessageBox::Information,
                                    QStringLiteral("Folder Compare 完成"),
                                    text, QMessageBox::Ok, this);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setModal(false);
        box->show();
    }
}

/* 把人工队列翻译成 FolderCompareWidget 左侧列表要显示的内容 */
static QVector<FolderCompareWidget::QueueEntry> makeFolderQueue(
    const QVector<QPair<QString, QString>> &dirs,
    const QVector<foldercompare::CompareView> &views,
    const QSet<int> &skipped,
    const QHash<QString, QString> &keepMap)
{
    QVector<FolderCompareWidget::QueueEntry> q;
    q.reserve(dirs.size());
    for (int i = 0; i < dirs.size(); ++i) {
        FolderCompareWidget::QueueEntry e;
        e.dirA = dirs[i].first;
        e.dirB = dirs[i].second;
        if (i < views.size()) {
            e.countA = views[i].countA;
            e.countB = views[i].countB;
            // 队列行两侧各画 2x2 四张(首/中二/末),就是需求里要的"每侧最多 4 张"
            e.previewA = views[i].previewA;
            e.previewB = views[i].previewB;
        }
        if (skipped.contains(i)) {
            e.decision = QStringLiteral("jump");
        } else {
            const QString keep = keepMap.value(
                dirPairKeyOf(QDir::cleanPath(e.dirA).toLower(),
                             QDir::cleanPath(e.dirB).toLower()));
            if (!keep.isEmpty()) {
                e.decision = (keep == QDir::cleanPath(e.dirA).toLower())
                                 ? QStringLiteral("A") : QStringLiteral("B");
            }
        }
        q << e;
    }
    return q;
}

void ImageDeduplicateWindow::presentNextFolderCompare()
{
    if (m_folderManualIndex >= m_folderManualViews.size()) {
        if (m_folderCompareWidget)
            m_folderCompareWidget->hide();
        finishFolderDecisionsAndBuildPairs();
        return;
    }

    if (!m_folderCompareWidget) {
        m_folderCompareWidget = new FolderCompareWidget(nullptr);
        m_folderCompareWidget->setWindowFlag(Qt::Window, true);

        connect(m_folderCompareWidget, &FolderCompareWidget::keepA, this, [this]() {
            const int i = m_folderManualIndex;
            if (i >= 0 && i < m_folderManualDirs.size()) {
                m_folderManualSkipped.remove(i);
                recordFolderDecision(m_folderManualDirs[i].first, m_folderManualDirs[i].second);
            }
            ++m_folderManualIndex;
            presentNextFolderCompare();
        });
        connect(m_folderCompareWidget, &FolderCompareWidget::keepB, this, [this]() {
            const int i = m_folderManualIndex;
            if (i >= 0 && i < m_folderManualDirs.size()) {
                m_folderManualSkipped.remove(i);
                recordFolderDecision(m_folderManualDirs[i].second, m_folderManualDirs[i].first);
            }
            ++m_folderManualIndex;
            presentNextFolderCompare();
        });
        /*
         * jump:这一对不比了 —— 既不留旧结论,也不给新方向。
         *
         * 这里只是记下来;真正剔掉"这两个目录之间的图片对"是在收尾时
         * (finishFolderDecisionsAndBuildPairs -> dropPairsBetweenJumpedFolders),
         * 因为建对发生在目录决策之前,那时还不知道谁会按 jump。
         */
        connect(m_folderCompareWidget, &FolderCompareWidget::skip, this, [this]() {
            const int i = m_folderManualIndex;
            if (i >= 0 && i < m_folderManualDirs.size()) {
                const auto &pair = m_folderManualDirs[i];
                clearFolderDecision(pair.first, pair.second);
                m_folderManualSkipped.insert(i);
            }
            ++m_folderManualIndex;
            presentNextFolderCompare();
        });
        connect(m_folderCompareWidget, &FolderCompareWidget::previous, this, [this]() {
            if (m_folderManualIndex <= 0)
                return;
            --m_folderManualIndex;
            /*
             * 回退就要把这一对上一轮的结论撤掉:否则用户改主意按了 jump,
             * 旧方向还留在 m_folderKeep 里继续影响摆位和 auto remove。
             */
            if (m_folderManualIndex < m_folderManualDirs.size()) {
                const auto &pair = m_folderManualDirs[m_folderManualIndex];
                clearFolderDecision(pair.first, pair.second);
                m_folderManualSkipped.remove(m_folderManualIndex);
            }
            presentNextFolderCompare();
        });
        // 列表里点某一对:直接跳过去(同样撤销它的旧结论,让用户重新决定)
        connect(m_folderCompareWidget, &FolderCompareWidget::selectIndex, this, [this](int index) {
            if (index < 0 || index >= m_folderManualViews.size())
                return;
            if (index == m_folderManualIndex)
                return;
            const auto &pair = m_folderManualDirs[index];
            clearFolderDecision(pair.first, pair.second);
            m_folderManualSkipped.remove(index);
            m_folderManualIndex = index;
            presentNextFolderCompare();
        });
        /*
         * 用户用 X 关窗:剩下的目录对全部按 jump 收尾并继续流程。
         * 不这么做的话 m_waitingFolderDecision 永远是 true,图片对一直不建,
         * 界面停在"没有任何可比较对象"的状态,只能关整个窗口。
         */
        connect(m_folderCompareWidget, &FolderCompareWidget::closedByUser, this, [this]() {
            if (!m_waitingFolderDecision)
                return;
            const int left = int(m_folderManualViews.size()) - m_folderManualIndex;
            if (left > 0) {
                logLine(QStringLiteral("[%1] Folder Compare 被关闭: 剩余 %2 对目录按 jump 处理")
                            .arg(nowStr()).arg(left));
            }
            for (int k = m_folderManualIndex; k < m_folderManualDirs.size(); ++k) {
                const auto &pair = m_folderManualDirs[k];
                clearFolderDecision(pair.first, pair.second);
                m_folderManualSkipped.insert(k);
            }
            m_folderManualIndex = int(m_folderManualViews.size());
            finishFolderDecisionsAndBuildPairs();
        });
    }

    const int i = m_folderManualIndex;
    m_folderCompareWidget->setQueueInfo(i, m_folderManualViews.size());
    m_folderCompareWidget->setQueue(makeFolderQueue(m_folderManualDirs, m_folderManualViews,
                                                  m_folderManualSkipped, m_folderKeep), i);
    m_folderCompareWidget->setView(m_folderManualViews[i]);
    m_folderCompareWidget->show();
    m_folderCompareWidget->raise();
    m_folderCompareWidget->activateWindow();
}

void ImageDeduplicateWindow::on_btn_next_clicked()
{
    goToNextPair();
}

