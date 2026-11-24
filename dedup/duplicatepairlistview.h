#ifndef DUPLICATEPAIRLISTVIEW_H
#define DUPLICATEPAIRLISTVIEW_H

/*
 * DuplicatePairListView —— ImageDeduplicateWindow 的图片对列表
 *
 * 每一项显示:
 *   [缩略图A][3px][缩略图B] │ pHash distance: xx        ORB similarity: xx%
 *                           │ 尺寸比较: A > B           质量比较: A(png 101) vs B(jpg 90)
 *                           │ pix diff: x.xxxxxx         State: Pending
 *                           │ rootInputFolder名称:filename
 *
 * 两张缩略图整体靠左对齐,最长边 128。缩略图懒加载:只算“真正要显示出来的行”。
 *
 * 交互:
 *   - 左键单击(无修饰键)一个非Invalid的项 -> 请求把主显示区跳到该 pair;
 *   - Ctrl/Shift 多选 -> 只影响选择集,不影响 m_currentIndex 和主显示区;
 *   - 右键 -> 弹菜单(不改变选中项):批量改状态(按顺序依次处理)、在资源管理器打开A/B;
 *   - 点击 Invalid 项 -> 请求跳到“导致它 Invalid 的那个 pair”(A优先)。
 */

#include "deduppch.h"

class DuplicatePairListView;

/* 列表里一行需要显示的全部信息(是PairInfo的只读快照,避免跨线程直接访问) */
struct DuplicatePairItem {
    int pairIndex = -1;
    int idxA = -1;
    int idxB = -1;

    /* A/B 的基础信息 */
    QString pathA, pathB;
    QString rootA, rootB;                     // rootInputFolder
    QSize sizeA{0, 0}, sizeB{0, 0};
    qint64 filesizeA = 0, filesizeB = 0;
    QString formatA, formatB;
    int qualityA = -1, qualityB = -1;
    int qualityRankA = 0, qualityRankB = 0;   // 由ImageDeduplicateWindow算好传入

    bool inDuplicateA = false, inDuplicateB = false;

    /* 查重指标 */
    int pHashDistance = INT_MAX;
    int dHashDistance = INT_MAX;
    double orbMatchRatio = 0.0;

    /* 像素差异(异步算好后刷新) */
    bool pixelDiffComputed = false;
    double pixelDiffRatio = 1.0;

    /* 状态(PairState) */
    int state = 0;
    /*
     * 叠加态:state 是 Invalid* 时,记录"这一侧被本对删过"(位1=A、位2=B)。
     * 列表显示成 "InvalidA + RemovedB" —— 两者都是事实,不能只留一个。
     */
    int removedMask = 0;
    /*
     * 第二条叠加轴:哪一侧已经被【别的对】移走(位1=A、位2=B)。
     *
     * 和 removedMask 不同,它对任何 state 都有效 —— 被 jump 掉的对
     * (Skipped)照样可能因为别的对删了其中一张而失效,列表要显示
     * "Skipped + InvalidB",点它也要按失效处理(跳回肇事那一对)。
     */
    int invalidMask = 0;
    /*
     * 筛选模式:这一行被钉在 A 位的图片(-1 = 普通模式)。
     * 只用于在行上画一个标记,让用户知道"现在看的是筛选结果,不是全表"。
     */
    int filterAnchor = -1;

    /* 懒加载出来的缩略图(不写回FileEntry) */
    QImage thumbA, thumbB;
};

/*
 * 右键菜单里的批量操作类型,与 ImageDeduplicateWindow::applyActionToPair 对应。
 * 注意:它是通过 int 信号参数传出去的,所以新值一律【往后追加】,
 * 不要插在中间 —— 否则老的下标会整体错位(比如 SetRemovedB 变成"跳过")。
 */
enum class PairBatchAction {
    SetRemovedA = 0,
    SetRemovedB,
    SetSkipped,
    SetPending,
    SetRemovedBoth   // 追加:批量"移除两者"
};

/* ============================ Model ============================ */
class DuplicatePairListModel : public QAbstractListModel
{
    Q_OBJECT
public:
    explicit DuplicatePairListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    void setItems(QVector<DuplicatePairItem> &&items);
    void clearItems();
    int count() const { return m_items.size(); }

    const DuplicatePairItem *itemAt(int row) const;
    int rowForPairIndex(int pairIndex) const;
    int pairIndexAt(int row) const;

    /* 局部刷新:改了model内容后通知视图 */
    void notifyRowChanged(int row);
    /*
     * 状态行刷新必须连两条叠加位一起传。
     * 只传 state 的话,"Skipped + InvalidB" 这类复合状态在行级刷新时永远刷不出来
     * (state 没变、invalidMask 变了),列表看起来就跟没生效一样。
     */
    void setRowState(int row, int state, int removedMask, int invalidMask);
    void setRowPixelDiff(int row, bool computed, double ratio);
    void setRowMetrics(int row, int pHashDistance, int dHashDistance, double orbMatchRatio);

    /* 懒加载:把算好的缩略图放进来 */
    void setThumbnail(int row, bool isA, const QImage &img);

private:
    QVector<DuplicatePairItem> m_items;
    QHash<int, int> m_pairIndexToRow;
};

/* ============================ Delegate ============================ */
class DuplicatePairListDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit DuplicatePairListDelegate(DuplicatePairListView *view, QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;

private:
    DuplicatePairListView *m_view = nullptr;
};

/* ============================ View ============================ */
class DuplicatePairListView : public QListView
{
    Q_OBJECT
public:
    explicit DuplicatePairListView(QWidget *parent = nullptr);

    /* 重建整个列表(扫描完成 / 排序变化后调用) */
    void setPairs(const QVector<DuplicatePairItem> &items);
    /* 只刷新一行(状态/pixdiff/指标变化时由ImageDeduplicateWindow调用) */
    void updatePairRow(int pairIndex, int state, int removedMask, int invalidMask,
                       bool pixelComputed, double pixelRatio);
    void updatePairRowMetrics(int pairIndex, int pHashDistance, int dHashDistance,
                              double orbMatchRatio);
    /* 懒加载算好的缩略图 */
    void setThumbnail(int pairIndex, bool isA, const QImage &img);
    /* 清空(clearState用) */
    void clearPairs();

    int pairIndexAt(int row) const;
    int rowForPairIndex(int pairIndex) const;


    /*
     * 让列表选中 m_currentIndex 对应的行。
     *
     * scrollMode 决定"要不要动滚动条" —— 这是把【选中态同步】和【视图跳转】解耦的关键:
     *   - ScrollAlways : 程序性跳转(按钮/快捷键 removeA/Jump、previous、auto去重后
     *                    跳到下一个Pending、批量操作后跳转)必须把目标行滚进视野;
     *                    目标行本来就可见时只做 EnsureVisible,避免"点一下就居中"的突兀位移;
     *   - ScrollNever  : 只同步选中态,完全不碰滚动条。
     *
     * 用户用鼠标点选/任意选中列表项时走 ScrollNever:
     * 他本来就在看这一行,再把它踢到正中央既没必要又把滚动位置弄丢了。
     *
     * index越界时:清空所有选中;只有 ScrollAlways 才会滚到顶部/底部。
     */
    enum ScrollMode { ScrollNever, ScrollAlways };
    void syncCurrentIndex(int index, int pairCount, ScrollMode scrollMode = ScrollAlways);

    /* 当前被选中的pair(多选时可能有多个) */
    QVector<int> selectedPairIndices() const;
    /* 当前主显示对应的pair,-1表示没有 */
    int currentPairIndex() const;

    /*
     * 筛选模式开关(由 ImageDeduplicateWindow 在进出筛选时设置)。
     * 只影响右键菜单放哪些项:筛选里只有 "移除另一侧" 是有意义的。
     */
    void setFilterMode(bool on) { m_filterMode = on; }
    bool filterMode() const { return m_filterMode; }
    /* 筛选功能在这个会话里是否可用(Deferred 模式不支持,见 startSimilarityFilter) */
    void setFilterAvailable(bool on) { m_filterAvailable = on; }

    /*
     * 主显示区正在等图(切 pair 时显示图还没解码出来,窗口暂时不切 current)。
     * 这期间列表的当前行仍然是上一对,而用户可能已经在看新位置了 ——
     * 记下来,Shift 日志里能直接看出是不是这个窗口期出的问题。
     */
    void setDisplaySwitchPending(bool on) { m_displaySwitchPending = on; }
    bool displaySwitchPending() const { return m_displaySwitchPending; }

    /* 代理在paint里回调,标记某行某侧已经画过(懒加载用) */
    void markRowPainted(int row, bool isA) const;

    /*
     * 区域选择的锚点行(-1 = 还没有锚点)。
     *
     * 之所以不交给 QAbstractItemView 内建的 Shift 选择:
     * 它的范围是"selectionModel 的 currentIndex -> 点击行",而 currentIndex
     * 由外部(setPairs 重建 / syncCurrentIndex / 清空选中)推着走,一旦某条路径
     * 忘了同步(例如重建时把 current 复位到第 0 行),下一次 Shift+左键就会
     * 从那一行一路选下来 —— 表现就是"有时从一个很靠前的位置开始区域选择"。
     *
     * 这里自己记锚点:普通左键/Ctrl+左键按下、程序性同步选中都更新它,
     * Shift+左键由 applyRangeSelection() 自己算范围,与 Qt 内部状态无关。
     */
    int selectionAnchorRow() const { return m_anchorRow; }

signals:
    /* 左键单击了某个非Invalid的pair,请求主窗口跳转 */
    void pairActivated(int pairIndex);
    /* 点击了Invalid的pair,请求跳到导致它invalid的那个pair */
    void invalidPairActivated(int pairIndex, bool preferA);
    /* 懒加载:这些pair的缩略图现在需要了 */
    void lazyLoadRequested(const QVector<int> &pairIndices);
    /*
     * 右键请求:菜单内容/样式统一由 ImageDeduplicateWindow 构建
     * (它才有 PairInfo、目录方向、筛选状态、模式这些信息)。
     * targets 是多选集合(只有一项时就是这一行)。
     */
    void contextMenuRequested(int pairIndex, const QVector<int> &targets, const QPoint &globalPos);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    /*
     * a / s / d / 空格 是 ImageDeduplicateWindow 的快捷键(移除A/移除B/移除两者/切换AB),
     * 这里必须让它们继续往上冒泡 —— 否则列表获得焦点时:
     *   空格 会被 QAbstractItemView 当成“选中当前项”吃掉;
     *   字母 会触发 QListView 的键盘增量搜索(keyboardSearch),把选中项乱跳。
     */
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void onScrollValueChanged(int value);
    void flushLazyLoad();
    /* 选择集被"别的东西"改动时的追踪(见实现里的说明) */
    void onSelectionChanged(const QItemSelection &selected, const QItemSelection &deselected);

private:
    void emitLazyLoadForVisibleRows();
    /* 从锚点行到 row 的连续区域选择(Shift+左键);anchor 无效时退化为只选 row */
    void applyRangeSelection(int row);

    DuplicatePairListModel *m_model = nullptr;
    bool m_filterMode = false;
    bool m_filterAvailable = true;
    DuplicatePairListDelegate *m_delegate = nullptr;

    bool m_pressWasPlain = true;      // 本次按下是否没有Ctrl/Shift(有的话只多选,不跳转)
    int  m_pressedPairIndex = -1;
    int  m_anchorRow = -1;            // 区域选择锚点行,见 selectionAnchorRow() 的注释
    /*
     * 程序性还原 currentIndex(拖动多选松手、右键菜单关掉)期间置位:
     * 那次还原不是"用户换了当前行",锚点不该跟着走。
     */
    bool m_suppressAnchorFromCurrent = false;
    /* 主显示区正在等图(见 setDisplaySwitchPending) */
    bool m_displaySwitchPending = false;
    /*
     * Shift+左键的按下被我们自己吞掉了(没交给基类)。这期间不能让基类处理
     * 鼠标移动,否则它会拿上一次普通点击留下的"按下行"当拖拽起点。
     */
    bool m_shiftPressSwallowed = false;
    /* 我们自己发起的选择变化期间置位,避免 onSelectionChanged 把正常操作也记成异常 */
    bool m_knownSelectionChange = false;

    /*
     * 拖动多选(按住左键划过若干行)。
     *
     * 松手时绝不能再发 pairActivated:那会让窗口 syncCurrentIndex(ClearAndSelect),
     * 整块选区立刻塌回一行 —— 用户看到的就是"拖动时多选了,一松手只剩按下那一行"。
     * 这里靠"按下点到松手点走了多远"判断,而不是拦截 mouseMove ——
     * 划选本身是 QListView 内建行为,只要不打断它就行。
     */
    QPoint m_pressPos;
    int    m_currentRowBeforePress = -1;   // 拖动只改选区,current 要退回按下前那一行

    mutable QSet<int> m_paintedRowsA;
    mutable QSet<int> m_paintedRowsB;
    QTimer *m_lazyTimer = nullptr;
    QSet<int> m_requestedRows;

    /*
     * setPairs 重建后要"把滚动位置还给用户",但 model reset 之后滚动条的 range
     * 可能还没算好,所以那一刻的 setValue 会被 clamp,需要延到下一轮事件循环补一次。
     *
     * 用一个自增的令牌来作废这次延迟恢复:
     * 如果在这之前 syncCurrentIndex(ScrollAlways) 已经因为"跳转"自己滚到位了,
     * 就把令牌+1,让那个延迟回调直接放弃 —— 否则它会把用户刚跳过去的视图又拽回原处。
     */
    int m_scrollRestoreToken = 0;
};

#endif // DUPLICATEPAIRLISTVIEW_H
