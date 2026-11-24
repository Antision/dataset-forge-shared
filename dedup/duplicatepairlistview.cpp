#include "duplicatepairlistview.h"
#include "deduputils.h"

#include "dedupenv.h"

#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QSignalBlocker>
#include <QTextStream>

using namespace dedup;

namespace {

/* 我们自己改选择集时,别让 onSelectionChanged 把它记成"异常变化" */
struct KnownSelectionChange {
    bool *flag;
    explicit KnownSelectionChange(bool *f) : flag(f) { if (flag) *flag = true; }
    ~KnownSelectionChange() { if (flag) *flag = false; }
};

constexpr int kThumbSize = dedup::kThumbnailMaxSize;  // 128
constexpr int kThumbGap  = 3;
constexpr int kMargin    = 6;
/* 两栏文字的左边界:两张缩略图(含间距)之后 */
constexpr int kCol1Left  = kMargin + kThumbSize + kThumbGap + kThumbSize + 12;
constexpr int kColGap    = 18;
constexpr int kLineCount = 4;
constexpr int kFontSize  = 12;
constexpr int kRowHeight = kThumbSize + 2 * kMargin;

QString pairStateName(int state)
{
    switch (state) {
    case 0: return QStringLiteral("Pending");
    case 1: return QStringLiteral("RemovedA");
    case 2: return QStringLiteral("RemovedB");
    case 3: return QStringLiteral("RemovedBoth");
    case 4: return QStringLiteral("Skipped");
    case 5: return QStringLiteral("InvalidA");
    case 6: return QStringLiteral("InvalidB");
    case 7: return QStringLiteral("InvalidBoth");
    default: return QStringLiteral("Unknown");
    }
}

bool isInvalidState(int state)
{
    return state == 5 || state == 6 || state == 7;
}

QColor stateColor(int state)
{
    switch (state) {
    case 0:  return QColor(220, 220, 220);   // Pending
    case 1:
    case 2:
    case 3:  return QColor(120, 200, 255);   // Removed*
    case 4:  return QColor(200, 200, 130);   // Skipped
    case 5:
    case 6:
    case 7:  return QColor(255, 120, 120);   // Invalid*
    default: return QColor(220, 220, 220);
    }
}

QString compareMark(qint64 a, qint64 b)
{
    if (a == b) return QStringLiteral("=");
    return (a > b) ? QStringLiteral(">") : QStringLiteral("<");
}

QString formatSizeText(const QSize &s)
{
    return QStringLiteral("%1x%2").arg(s.width()).arg(s.height());
}

QString fileNameOf(const QString &path)
{
    if (path.isEmpty()) return QStringLiteral("?");
    return QFileInfo(path).fileName();
}

QString folderNameOf(const QString &path)
{
    if (path.isEmpty()) return QStringLiteral("?");
    const QString name = QFileInfo(path).fileName();
    return name.isEmpty() ? QStringLiteral("?") : name;
}

/* 文件所在的直接父目录(规范化:cleanPath + 小写;Windows 大小写/分隔符不敏感) */
QString parentDirOfPath(const QString &path)
{
    if (path.isEmpty()) return QString();
    return QDir::cleanPath(QFileInfo(path).absolutePath()).toLower();
}

} // namespace

/* ============================ Model ============================ */

DuplicatePairListModel::DuplicatePairListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int DuplicatePairListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return m_items.size();
}

QVariant DuplicatePairListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return QVariant();

    const DuplicatePairItem &it = m_items[index.row()];

    switch (role) {
    case Qt::DisplayRole:
        return QStringLiteral("%1:%2 | %3:%4")
            .arg(folderNameOf(it.rootA), fileNameOf(it.pathA),
                 folderNameOf(it.rootB), fileNameOf(it.pathB));
    case Qt::ToolTipRole:
        return QStringLiteral("A: %1\nB: %2\nState: %3")
            .arg(it.pathA, it.pathB, pairStateName(it.state));
    case Qt::UserRole:      return it.pairIndex;
    case Qt::UserRole + 1:  return it.state;
    case Qt::UserRole + 2:  return it.pHashDistance;
    case Qt::UserRole + 3:  return it.orbMatchRatio;
    case Qt::UserRole + 4:  return it.pixelDiffComputed;
    case Qt::UserRole + 5:  return it.pixelDiffRatio;
    default:                return QVariant();
    }
}

void DuplicatePairListModel::setItems(QVector<DuplicatePairItem> &&items)
{
    beginResetModel();
    m_items = std::move(items);
    m_pairIndexToRow.clear();
    m_pairIndexToRow.reserve(m_items.size());
    for (int i = 0; i < m_items.size(); ++i)
        m_pairIndexToRow.insert(m_items[i].pairIndex, i);
    endResetModel();
}

void DuplicatePairListModel::clearItems()
{
    beginResetModel();
    m_items.clear();
    m_pairIndexToRow.clear();
    endResetModel();
}

const DuplicatePairItem *DuplicatePairListModel::itemAt(int row) const
{
    if (row < 0 || row >= m_items.size()) return nullptr;
    return &m_items[row];
}

int DuplicatePairListModel::rowForPairIndex(int pairIndex) const
{
    return m_pairIndexToRow.value(pairIndex, -1);
}

int DuplicatePairListModel::pairIndexAt(int row) const
{
    if (row < 0 || row >= m_items.size()) return -1;
    return m_items[row].pairIndex;
}

void DuplicatePairListModel::notifyRowChanged(int row)
{
    if (row < 0 || row >= m_items.size()) return;
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, {Qt::DisplayRole, Qt::UserRole + 1, Qt::UserRole + 2,
                                Qt::UserRole + 3, Qt::UserRole + 4, Qt::UserRole + 5});
}

void DuplicatePairListModel::setRowState(int row, int state, int removedMask, int invalidMask)
{
    if (row < 0 || row >= m_items.size()) return;
    DuplicatePairItem &it = m_items[row];
    if (it.state == state && it.removedMask == removedMask && it.invalidMask == invalidMask)
        return;
    it.state = state;
    it.removedMask = removedMask;
    it.invalidMask = invalidMask;
    notifyRowChanged(row);
}


void DuplicatePairListModel::setRowPixelDiff(int row, bool computed, double ratio)
{
    if (row < 0 || row >= m_items.size()) return;
    m_items[row].pixelDiffComputed = computed;
    m_items[row].pixelDiffRatio = ratio;
    notifyRowChanged(row);
}

void DuplicatePairListModel::setRowMetrics(int row, int pHashDistance, int dHashDistance,
                                           double orbMatchRatio)
{
    if (row < 0 || row >= m_items.size()) return;
    m_items[row].pHashDistance = pHashDistance;
    m_items[row].dHashDistance = dHashDistance;
    m_items[row].orbMatchRatio = orbMatchRatio;
    notifyRowChanged(row);
}

void DuplicatePairListModel::setThumbnail(int row, bool isA, const QImage &img)
{
    if (row < 0 || row >= m_items.size()) return;

    if (isA) {
        if (!m_items[row].thumbA.isNull()) return;
        m_items[row].thumbA = img;
    } else {
        if (!m_items[row].thumbB.isNull()) return;
        m_items[row].thumbB = img;
    }
    notifyRowChanged(row);
}

/* ============================ Delegate ============================ */

DuplicatePairListDelegate::DuplicatePairListDelegate(DuplicatePairListView *view, QObject *parent)
    : QStyledItemDelegate(parent), m_view(view)
{
}

QSize DuplicatePairListDelegate::sizeHint(const QStyleOptionViewItem &option,
                                          const QModelIndex &index) const
{
    Q_UNUSED(option);
    Q_UNUSED(index);
    return QSize(760, kRowHeight);
}

void DuplicatePairListDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                      const QModelIndex &index) const
{
    if (!painter || !index.isValid()) return;

    const DuplicatePairListModel *model =
        qobject_cast<const DuplicatePairListModel*>(index.model());
    if (!model) return;
    const DuplicatePairItem *item = model->itemAt(index.row());
    if (!item) return;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, false);

    const QRect r = option.rect;
    const bool selected = option.state & QStyle::State_Selected;
    const bool invalid = isInvalidState(item->state);

    if (selected)
        painter->fillRect(r, QColor(30, 125, 93));
    else if (invalid)
        painter->fillRect(r, QColor(60, 26, 26));
    else if (option.state & QStyle::State_MouseOver)
        painter->fillRect(r, QColor(38, 31, 56));
    else
        painter->fillRect(r, QColor(24, 24, 40));

    // ===== 两张缩略图,整体靠左对齐,间隔3px =====
    const QRect thumbARect(r.left() + kMargin, r.top() + kMargin, kThumbSize, kThumbSize);
    const QRect thumbBRect(thumbARect.right() + 1 + kThumbGap, r.top() + kMargin,
                           kThumbSize, kThumbSize);

    auto drawThumb = [&](const QRect &box, const QImage &img, const QString &path, bool isA) {
        painter->fillRect(box, QColor(12, 12, 24));

        if (!img.isNull()) {
            const QSize scaled = img.size().scaled(box.size(), Qt::KeepAspectRatio);
            const QRect dst(box.left() + (box.width() - scaled.width()) / 2,
                            box.top() + (box.height() - scaled.height()) / 2,
                            scaled.width(), scaled.height());
            painter->drawImage(dst, img);
            return;
        }

        // 缩略图还没有 -> 登记懒加载,并先显示文件名占位
        if (m_view) m_view->markRowPainted(index.row(), isA);

        QFont f(appFontFamily());
        f.setPixelSize(11);
        painter->setFont(f);
        painter->setPen(QColor(120, 120, 140));
        const QFontMetrics fm(f);
        painter->drawText(box.adjusted(4, 4, -4, -4), Qt::AlignCenter | Qt::TextWordWrap,
                          fm.elidedText(fileNameOf(path), Qt::ElideMiddle, box.width() - 8));
    };

    drawThumb(thumbARect, item->thumbA, item->pathA, true);
    drawThumb(thumbBRect, item->thumbB, item->pathB, false);

    // ===== 右侧文字:两栏,最后一行单独占满 =====
    const int textLeft  = r.left() + kCol1Left;
    const int textWidth = std::max(80, r.width() - kCol1Left - kMargin);
    const int colWidth  = std::max(60, (textWidth - kColGap) / 2);
    const int col2Left  = textLeft + colWidth + kColGap;

    QFont font(appFontFamily());
    font.setPixelSize(kFontSize);
    painter->setFont(font);
    const QFontMetrics fm(font);

    const int lineHeight = std::max(fm.height(), (kRowHeight - 2 * kMargin) / kLineCount);
    const int topY = r.top() + kMargin;

    auto drawCell = [&](int col, int line, const QString &text, const QColor &color) {
        const int left = (col == 0) ? textLeft : col2Left;
        painter->setPen(color);
        painter->drawText(left, topY + line * lineHeight + fm.ascent(),
                          fm.elidedText(text, Qt::ElideMiddle, colWidth));
    };

    const QColor plain(230, 230, 230);

    // 第0行: pHash distance | ORB similarity
    drawCell(0, 0,
             QStringLiteral("pHash distance: %1")
                 .arg(item->pHashDistance == INT_MAX ? QStringLiteral("-")
                                                     : QString::number(item->pHashDistance)),
             plain);
    drawCell(1, 0,
             QStringLiteral("ORB similarity: %1%").arg(item->orbMatchRatio * 100.0, 0, 'f', 1),
             plain);

    // 第1行: 尺寸比较 | 质量比较
    const qint64 areaA = static_cast<qint64>(item->sizeA.width()) * item->sizeA.height();
    const qint64 areaB = static_cast<qint64>(item->sizeB.width()) * item->sizeB.height();
    const double areaRatio = (areaB > 0) ? 100.0 * areaA / areaB : 0.0;

    drawCell(0, 1,
             QStringLiteral("Size: %1 %2 %3 (%4%)")
                 .arg(formatSizeText(item->sizeA), compareMark(areaA, areaB),
                      formatSizeText(item->sizeB))
                 .arg(areaRatio, 0, 'f', 1),
             plain);
    /* 质量行:括号里是 qualityRank,JPEG 用 DQT 估计值、无损格式固定 101 */
    drawCell(1, 1,
             QStringLiteral("Quality: %1(%2) %3 %4(%5)")
                 .arg(item->formatA).arg(item->qualityRankA)
                 .arg(compareMark(item->qualityRankA, item->qualityRankB))
                 .arg(item->formatB).arg(item->qualityRankB),
             plain);

    // 第2行: pix diff | State
    drawCell(0, 2,
             item->pixelDiffComputed
                 ? QStringLiteral("pix diff: %1").arg(item->pixelDiffRatio, 0, 'f', 8)
                 : QStringLiteral("pix diff: computing..."),
             plain);
    /*
     * 状态文字要能表达"叠加态":失效 + 已移除同时成立。
     * 例:筛选里处理了一张 invalid 的图 d 的对(d-e),删掉 e 之后
     * 这一对既"因为 d 失效"又"e 已被本对删掉",只显示一个都会丢信息。
     */
    QString stateText = pairStateName(item->state);
    if (isInvalidState(item->state) && item->removedMask != 0) {
        QStringList extra;
        if (item->removedMask & 1) extra << QStringLiteral("RemovedA");
        if (item->removedMask & 2) extra << QStringLiteral("RemovedB");
        if (!extra.isEmpty())
            stateText += QStringLiteral(" + ") + extra.join(QLatin1Char('+'));
    }
    /*
     * 第二条叠加轴:state 不是 Invalid*,但某一侧已经被别的对移走了。
     * 例:A-B 被 jump 掉,之后在 B-C 里删了 B —— A-B 仍是 Skipped,
     * 但它的 B 已经不在原处了,不能恢复、不能操作。只显示 "Skipped" 会骗人。
     */
    if (!isInvalidState(item->state) && item->invalidMask != 0) {
        QStringList extra;
        if (item->invalidMask & 1) extra << QStringLiteral("InvalidA");
        if (item->invalidMask & 2) extra << QStringLiteral("InvalidB");
        if (!extra.isEmpty())
            stateText += QStringLiteral(" + ") + extra.join(QLatin1Char('+'));
    }
    if (item->filterAnchor >= 0)
        stateText.prepend(QStringLiteral("FILTER | "));
    drawCell(1, 2, QStringLiteral("State: %1").arg(stateText),
             (isInvalidState(item->state) || item->invalidMask != 0)
                 ? stateColor(5)          // 只要有一侧没了,就按失效的红字显示
                 : stateColor(item->state));

    // 第3行: rootInputFolder名称:filename (单独占最后一行)
    const QString line4 = QStringLiteral("%1:%2   |   %3:%4")
                              .arg(folderNameOf(item->rootA), fileNameOf(item->pathA),
                                   folderNameOf(item->rootB), fileNameOf(item->pathB));
    painter->setPen(QColor(170, 210, 255));
    painter->drawText(textLeft, topY + 3 * lineHeight + fm.ascent(),
                      fm.elidedText(line4, Qt::ElideMiddle, textWidth));

    painter->restore();
}

/* ============================ View ============================ */

DuplicatePairListView::DuplicatePairListView(QWidget *parent)
    : QListView(parent)
{
    m_model = new DuplicatePairListModel(this);
    m_delegate = new DuplicatePairListDelegate(this, this);

    setModel(m_model);
    setItemDelegate(m_delegate);

    setFlow(QListView::TopToBottom);
    setWrapping(false);
    setResizeMode(QListView::Adjust);
    setUniformItemSizes(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);   // 支持Ctrl/Shift多选
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setMouseTracking(true);
    setSpacing(2);
    setContextMenuPolicy(Qt::DefaultContextMenu);   // 走我们重写的contextMenuEvent

    // 只用于浏览和批量操作,不接收文件drop
    setAcceptDrops(false);
    setDragEnabled(false);
    setDragDropMode(QAbstractItemView::NoDragDrop);
    if (viewport()) viewport()->installEventFilter(this);

    /*
     * 锚点跟随当前行。
     *
     * 之前锚点只在三条路径上更新(普通左键、↑↓ 键盘导航、syncCurrentIndex),
     * 而 currentIndex 还可能被别的地方推着走(重建、重排后的回填、任何"顺手同步
     * 一下选中"的分支)。只要有一条路径没带上锚点,就会出现
     * "屏幕上当前停在 2901,Shift+左键却从之前的 2824 开始选一大片"。
     *
     * 与其一条条堵路径,不如把不变量钉死:**锚点 = 用户看到的当前行**。
     * Shift/拖动只改选择集、不改 current,所以这里加钩子不会破坏区域选择。
     */
    if (selectionModel()) {
        connect(selectionModel(), &QItemSelectionModel::selectionChanged, this,
                &DuplicatePairListView::onSelectionChanged);
        connect(selectionModel(), &QItemSelectionModel::currentRowChanged, this,
                [this](const QModelIndex &cur, const QModelIndex &) {
                    /*
                     * 例外:拖动/右键菜单结束后我们【有意】把 current 还原到按下前那一行
                     * (current 代表主显示区正在显示的 pair,不该被多选带走),
                     * 那次还原不该把锚点也一起拖回去。
                     */
                    if (m_suppressAnchorFromCurrent) return;
                    m_anchorRow = cur.isValid() ? cur.row() : -1;
                });
    }

    // 懒加载:滚动/尺寸变化/重绘之后汇总一次
    m_lazyTimer = new QTimer(this);
    m_lazyTimer->setSingleShot(true);
    m_lazyTimer->setInterval(60);
    connect(m_lazyTimer, &QTimer::timeout, this, &DuplicatePairListView::flushLazyLoad);

    if (verticalScrollBar())
        connect(verticalScrollBar(), &QScrollBar::valueChanged,
                this, &DuplicatePairListView::onScrollValueChanged);
}

void DuplicatePairListView::setPairs(const QVector<DuplicatePairItem> &items)
{
    /*
     * 重建整个模型(beginResetModel/endResetModel)会把选中和滚动位置一起清掉。
     * pair数量在千级,ListView的滚动位置是按像素滚的,重建之后如果不做处理,
     * 用户"滚了半天的位置"就会丢 —— 表现就是每次状态变化后列表自己弹回顶部/当前项。
     *
     * 这里把原来的滚动位置记下来,重建后再对回同一个pair(按pairIndex而不是行号,
     * 因为重排/状态变化后行号可能已经变了)。找不到就保持顶部。
     */
    const int keepPairIndex = currentPairIndex();
    const int keepScroll = verticalScrollBar() ? verticalScrollBar()->value() : 0;
    const int anchorBeforeSetPairs = m_anchorRow;

    m_paintedRowsA.clear();
    m_paintedRowsB.clear();
    m_requestedRows.clear();

    m_model->setItems(QVector<DuplicatePairItem>(items));

    if (m_model->count() <= 0) {
        // 列表空了:作废可能还挂着的"恢复滚动位置"回调,免得它去动一个空列表
        ++m_scrollRestoreToken;
        m_anchorRow = -1;
        emitLazyLoadForVisibleRows();
        return;
    }

    /*
     * 模型 reset 之后 selectionModel 的 currentIndex 已经失效,必须重落一个,
     * 否则列表没有"当前行"。
     *
     * 【关键】不能无条件落到第 0 行:调用方(rebuildPairList)之后虽然大多会
     * 跟着 syncPairListSelection 把 current 挪回 m_currentIndex,但只要有路径漏掉
     * (例如"完全重复归属收尾"里的 rebuildPairList),第 0 行就会留下来当
     * Shift 区域选择的起点 —— 表现就是"从一个很靠前的位置开始区域选择"。
     * 这里直接按重建前的当前 pair 回填,漏同步也不会跑偏。
     */
    {
        const int keepRow = m_model->rowForPairIndex(keepPairIndex);
        KnownSelectionChange guard(&m_knownSelectionChange);
        if (keepRow >= 0) {
            selectionModel()->setCurrentIndex(m_model->index(keepRow, 0),
                                              QItemSelectionModel::ClearAndSelect);
        } else {
            selectionModel()->clearSelection();
            selectionModel()->clearCurrentIndex();
        }
        m_anchorRow = keepRow;
        dedup::traceShift(QStringLiteral("SETPAIRS n=%1 keepPair=%2 keepRow=%3 anchor %4 -> %5")
                       .arg(m_model->count()).arg(keepPairIndex).arg(keepRow)
                       .arg(anchorBeforeSetPairs).arg(m_anchorRow));
    }

    /*
     * 恢复滚动位置。
     *
     * 重建后行列布局不变(行高固定),所以直接把像素偏移塞回去就能回到原来的位置。
     * 注意:这里【刻意不做】scrollTo(PositionAtCenter) —— 那个是"跳转"才需要的语义,
     * 而 rebuild 只是刷新状态。把滚动位置原样还给用户才是对的。
     *
     * model reset 之后 QListView 往往还没完成内建 layout(滚动条 range 也还没更新),
     * 这时 setValue 会被 clamp 掉。所以延到事件循环下一轮再设一次;
     * 同时记下这次要恢复的目标,避免被随后的其它 setValue 覆盖。
     */
    const int targetScroll = keepScroll;
    const bool keepScrollValid = (keepPairIndex < 0 || m_model->rowForPairIndex(keepPairIndex) >= 0);

    const int token = ++m_scrollRestoreToken;
    auto restoreScroll = [this, targetScroll, keepScrollValid, token]() {
        // 已经被一次真正的"跳转"(syncCurrentIndex ScrollAlways)接管了,放弃这次恢复
        if (token != m_scrollRestoreToken) return;
        if (!verticalScrollBar()) return;
        if (!keepScrollValid) { scrollToTop(); return; }
        verticalScrollBar()->setValue(targetScroll);
    };

    if (verticalScrollBar()) {
        // 先试一次(通常这次就够);range 未就绪时下次事件循环再补一次
        restoreScroll();
        QTimer::singleShot(0, this, restoreScroll);
    }

    emitLazyLoadForVisibleRows();
}

void DuplicatePairListView::updatePairRow(int pairIndex, int state,
                                          int removedMask, int invalidMask,
                                          bool pixelComputed, double pixelRatio)
{
    const int row = m_model->rowForPairIndex(pairIndex);
    if (row < 0) return;

    m_model->setRowState(row, state, removedMask, invalidMask);
    m_model->setRowPixelDiff(row, pixelComputed, pixelRatio);
}

void DuplicatePairListView::updatePairRowMetrics(int pairIndex, int pHashDistance,
                                                 int dHashDistance, double orbMatchRatio)
{
    const int row = m_model->rowForPairIndex(pairIndex);
    if (row < 0) return;
    m_model->setRowMetrics(row, pHashDistance, dHashDistance, orbMatchRatio);
}


void DuplicatePairListView::setThumbnail(int pairIndex, bool isA, const QImage &img)
{
    const int row = m_model->rowForPairIndex(pairIndex);
    if (row < 0) return;
    m_model->setThumbnail(row, isA, img);
}

void DuplicatePairListView::clearPairs()
{
    m_paintedRowsA.clear();
    m_paintedRowsB.clear();
    m_requestedRows.clear();
    m_anchorRow = -1;

    m_model->clearItems();
    if (selectionModel()) {
        selectionModel()->clearSelection();
        selectionModel()->clearCurrentIndex();
    }
}

int DuplicatePairListView::pairIndexAt(int row) const
{
    return m_model->pairIndexAt(row);
}

int DuplicatePairListView::rowForPairIndex(int pairIndex) const
{
    return m_model->rowForPairIndex(pairIndex);
}

void DuplicatePairListView::syncCurrentIndex(int index, int pairCount, ScrollMode scrollMode)
{
    auto clearSelectionAndReset = [this]() {
        if (selectionModel()) {
            KnownSelectionChange guard(&m_knownSelectionChange);
            selectionModel()->clearSelection();
            selectionModel()->clearCurrentIndex();
        }
        // 没有当前行了,锚点也一并作废:下一次 Shift+左键退化为只选被点的那一行
        m_anchorRow = -1;
    };

    dedup::traceShift(QStringLiteral("SYNC index=%1 count=%2 scroll=%3 anchorBefore=%4")
                   .arg(index).arg(pairCount).arg(int(scrollMode)).arg(m_anchorRow));

    if (index < 0) {
        // 越界(向前):取消选中;只有显式要求跳转时才滚回顶部
        clearSelectionAndReset();
        if (scrollMode == ScrollAlways)
            scrollToTop();
        return;
    }

    if (index >= pairCount) {
        // 越界(向后,所有pair都处理完了):取消选中;只有显式要求跳转时才滚到底部
        clearSelectionAndReset();
        if (scrollMode == ScrollAlways && m_model->count() > 0)
            scrollTo(m_model->index(m_model->count() - 1, 0), QAbstractItemView::PositionAtBottom);
        return;
    }

    const int row = m_model->rowForPairIndex(index);
    if (row < 0) {
        clearSelectionAndReset();
        return;
    }

    const QModelIndex idx = m_model->index(row, 0);
    {
        KnownSelectionChange guard(&m_knownSelectionChange);
        selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect);
    }

    /*
     * 当前行就是区域选择的锚点。
     * 所有程序性跳转(remove A/B 后自动前进、previous/next、Jump、批量操作后定位)
     * 都会走这里,所以"用户眼里的当前位置"和"Shift 选择的起点"永远是同一个。
     */
    m_anchorRow = row;
    dedup::traceShift(QStringLiteral("SYNC -> row=%1 anchor=%2").arg(row).arg(m_anchorRow));

    if (scrollMode == ScrollAlways) {
        /*
         * 用户点击列表项触发的主显示区切换会走到这里(ScrollNever 就不会进来),
         * 而那种场景下目标行本来就在屏幕里 —— 无需居中。
         * 只有目标行已经滚出视野(快捷键/按钮跳转、批量操作后跳到的Pending可能离得很远)
         * 才真正滚动,并且优先用 EnsureVisible:
         *   - 行完全可见 -> 什么都不做,滚动位置保持不变;
         *   - 行不可见   -> 最小幅度滚动让它露出,而不是粗暴地居中。
         * 这样"列表卡顿时向下滚了半天,一操作就被拽回中央"的问题就没了。
         */
        const QRect itemRect = visualRect(idx);
        const QRect area = viewport() ? viewport()->rect() : rect();
        if (!itemRect.isValid() || !area.contains(itemRect)) {
            // 这是一次真正的"跳转":作废 setPairs 里排队的那次"恢复原滚动位置",
            // 否则它会在事件循环下一轮把刚跳到的视图又拽回去。
            ++m_scrollRestoreToken;
            scrollTo(idx, QAbstractItemView::EnsureVisible);
        }
    }

    emitLazyLoadForVisibleRows();
}

QVector<int> DuplicatePairListView::selectedPairIndices() const
{
    QVector<int> out;
    if (selectionModel()) {
        const QModelIndexList rows = selectionModel()->selectedIndexes();
        for (const QModelIndex &idx : rows) {
            const int pi = m_model->pairIndexAt(idx.row());
            if (pi >= 0) out.push_back(pi);
        }
    }

    if (out.isEmpty()) {
        const int pi = currentPairIndex();
        if (pi >= 0) out.push_back(pi);
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

int DuplicatePairListView::currentPairIndex() const
{
    const QModelIndex idx = currentIndex();
    if (!idx.isValid()) return -1;
    return m_model->pairIndexAt(idx.row());
}

/* ============================ 交互 ============================ */

void DuplicatePairListView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        /*
         * 右键(以及中键)一律不交给基类。
         *
         * QAbstractItemView::mousePressEvent 对**任何**按键都会把"按下的那一行"
         * 设成 current 并重新选择它。实测(offscreen 探针,普通 QListView +
         * ExtendedSelection,当前行=2):
         *     右键按下 row 6 之前 : current=2, 选中={2}
         *     右键按下 row 6 之后 : current=6, 选中={6}
         * 于是"右键的那一行"被画成选中态(用户看到的"假选中"),真正选中/正在
         * 显示的那一行反而失去高亮;连带 Shift 区域选择的锚点也跑到这一行上。
         *
         * 菜单由 contextMenuEvent 负责(它是独立事件,不依赖这次 press),
         * 所以这里直接吞掉不会丢菜单。
         */
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        const Qt::KeyboardModifiers mods = event->modifiers();
        m_pressWasPlain = !(mods & (Qt::ControlModifier | Qt::ShiftModifier));
        m_pressPos = event->pos();
        m_currentRowBeforePress = currentIndex().isValid() ? currentIndex().row() : -1;
        const QModelIndex idx = indexAt(event->pos());
        m_pressedPairIndex = idx.isValid() ? m_model->pairIndexAt(idx.row()) : -1;

        /*
         * Shift+左键:自己算区域范围,不交给基类。
         *
         * 基类的做法是"selectionModel 的 currentIndex -> 点击行",而 currentIndex
         * 是外部随时会推的(重建列表、程序性跳转、清空选中)。只要有一条路径没同步好,
         * 这里就会从那个陈旧的行一路选下来 —— 用户看到的就是"有时从一个很靠前的位置
         * 开始区域选择"。改成用本类自己维护的锚点,行为就与 Qt 内部状态无关了。
         *
         * 区域选择只改选择集,和普通点击一样不触发主显示区跳转,所以直接把
         * m_pressedPairIndex 清掉(mouseReleaseEvent 靠它决定要不要 emit)。
         */
        if ((mods & Qt::ShiftModifier) && idx.isValid()) {
            applyRangeSelection(idx.row());
            m_pressedPairIndex = -1;
            m_pressWasPlain = false;
            m_shiftPressSwallowed = true;   // 这次按下没交给基类,移动事件也不给(见 mouseMoveEvent)
            // 自己吞掉了这次按下,基类不会帮忙抢焦点 —— 后面按 a/s/d 还得让
            // 快捷键继续冒泡到 ImageDeduplicateWindow,焦点必须在列表上
            setFocus(Qt::MouseFocusReason);
            event->accept();
            return;
        }

        // 普通左键 / Ctrl+左键 -> 这一行成为新的区域选择锚点
        if (idx.isValid()) {
            m_anchorRow = idx.row();
            dedup::traceShift(QStringLiteral("CLICK row=%1 mods=%2").arg(idx.row())
                           .arg(int(event->modifiers())));
        }
    }
    QListView::mousePressEvent(event);
}

void DuplicatePairListView::applyRangeSelection(int row)
{
    if (!selectionModel() || !m_model) return;
    if (row < 0 || row >= m_model->count()) return;
    /*
     * 锚点失效(列表刚重建/被清空/当前 pair 越界)时退化成"只选这一行",
     * 并且把这一行当成新锚点 —— 之后的 Shift+点击就从这个位置继续延伸。
     * 这里刻意不退化到"从第 0 行开始":那正是这轮要修掉的现象。
     */
    int anchor = m_anchorRow;
    const bool anchorValid = (anchor >= 0 && anchor < m_model->count());
    if (!anchorValid) {
        anchor = row;
        m_anchorRow = row;
    }

    const int lo = qMin(anchor, row);
    const int hi = qMax(anchor, row);
    const QItemSelection range(m_model->index(lo, 0), m_model->index(hi, 0));

    {
        const QModelIndex cur = currentIndex();
        const int curRow = cur.isValid() ? cur.row() : -1;
        const QRect vr = visualRect(m_model->index(anchor, 0));
        dedup::traceShift(QStringLiteral("SHIFT click=%1 anchor=%2(orig %3) current=%4 rows=%5..%6 "
                                  "anchorVisible=%7 pairAnchor=%8 pairClick=%9 pending=%10")
                       .arg(row).arg(anchor).arg(m_anchorRow).arg(curRow).arg(lo).arg(hi)
                       .arg(vr.isValid() && viewport() && viewport()->rect().intersects(vr) ? 1 : 0)
                       .arg(m_model->pairIndexAt(anchor))
                       .arg(m_model->pairIndexAt(row))
                       .arg(m_displaySwitchPending ? 1 : 0));
    }

    // ClearAndSelect:区域选择就是要一整块连续区间,和 Windows 资源管理器一致
    {
        KnownSelectionChange guard(&m_knownSelectionChange);
        selectionModel()->select(range, QItemSelectionModel::ClearAndSelect);
    }

    /*
     * 刻意【不】把 currentIndex 挪到点击行:
     *   · currentIndex 代表"主显示区正在显示哪一对",它属于 ImageDeduplicateWindow
     *     的 m_currentIndex,由 syncCurrentIndex 统一同步;
     *   · 如果这里把它改掉,就会出现"列表当前行"和"正在显示的 pair"两份真相,
     *     下一次 Shift 的范围、previous/next 的起点都会跟着乱。
     * 锚点保持不动,连续多次 Shift+点击都以同一个锚点为准(标准行为)。
     */
    emitLazyLoadForVisibleRows();
}

void DuplicatePairListView::mouseMoveEvent(QMouseEvent *event)
{
    /*
     * Shift+左键那一下被我们吞掉了(没交给基类),基类的"按下行"还是上一次普通点击
     * 留下的陈旧值。只要用户按下去之后手抖一两个像素,基类就会用它当拖拽起点,
     * 一路选到当前鼠标位置 —— 表现就是"我们算的范围明明只有两行,屏幕上却是一大片"。
     *
     * 所以这一次按下期间的所有移动一律不交给基类。Shift+点击本来就不该拖拽。
     */
    if (m_shiftPressSwallowed) {
        event->accept();
        return;
    }
    /*
     * 其余情况什么也不额外做 —— "按住左键划过多个行"是 QListView 内建的弹性选择,
     * 只有把事件继续交给基类才会生效。
     */
    QListView::mouseMoveEvent(event);
}

void DuplicatePairListView::mouseReleaseEvent(QMouseEvent *event)
{
    /*
     * Shift+左键那次按下没有交给基类,松手也就不要交给它 ——
     * 基类会拿它记忆里的"按下行"做收尾(可能顺手改选择集)。
     */
    const bool shiftPressWasSwallowed = m_shiftPressSwallowed;
    m_shiftPressSwallowed = false;   // 按钮松开,后面再移动就是正常的拖拽/悬停了

    const int pressedPair = m_pressedPairIndex;
    const bool plain = m_pressWasPlain;
    const QPoint pressPos = m_pressPos;
    const int rowBefore = m_currentRowBeforePress;
    m_pressedPairIndex = -1;

    if (!shiftPressWasSwallowed)
        QListView::mouseReleaseEvent(event);

    if (event->button() != Qt::LeftButton) return;

    /*
     * 拖动多选:松手时不能把"跳转"发出去。
     *
     * 发出去的话窗口会 displayPairByIndex(按下那一行) -> syncCurrentIndex(ClearAndSelect),
     * 整块选区立刻塌回一行 —— 用户看到的就是"按住时多选了,一松手只剩按下那一项"。
     * 两种判据都认:按下点到松手点走了超过拖拽阈值,或者选择集里确实不止一项。
     */
    const bool dragged = (pressPos - event->pos()).manhattanLength()
                         > QApplication::startDragDistance();
    const bool multiSelected = selectionModel()
                               && selectionModel()->selectedIndexes().size() > 1;
    if (dragged || multiSelected) {
        /*
         * 【临时探针】这两条以前是静默 return:用户"点了一下"却什么都没发生,
         * 日志里连一行都没有。尤其是"点一个已经在多选里的行" —— Qt 会保留整个
         * 选区,于是 multiSelected 成立,这次点击就被当成"拖动多选"吞掉。
         */
        dedup::traceShift(QStringLiteral("REL DROP rowBefore=%1 pressedPair=%2 dragged=%3(%4px) "
                                         "multi=%5(sel=%6)")
                              .arg(rowBefore).arg(pressedPair)
                              .arg(dragged ? 1 : 0)
                              .arg((pressPos - event->pos()).manhattanLength())
                              .arg(multiSelected ? 1 : 0)
                              .arg(selectionModel() ? selectionModel()->selectedIndexes().size() : -1));
        /*
         * 选区保留,但"当前行"退回按下之前那一行。
         * 本类与窗口的约定是:currentIndex 只有一个,代表主显示区正在显示的 pair;
         * 拖动改的是选择集(用于右键批量),不该顺手把"当前位置"挪到划过的最末尾。
         */
        if (selectionModel() && rowBefore >= 0 && rowBefore < m_model->count()) {
            m_suppressAnchorFromCurrent = true;   // 这次还原不是"用户换了当前行",锚点不动
            KnownSelectionChange guard(&m_knownSelectionChange);
            selectionModel()->setCurrentIndex(m_model->index(rowBefore, 0),
                                              QItemSelectionModel::NoUpdate);
            m_suppressAnchorFromCurrent = false;
        }
        emitLazyLoadForVisibleRows();
        return;
    }

    if (!plain) {
        dedup::traceShift(QStringLiteral("REL DROP pressedPair=%1: 非普通点击(mods)").arg(pressedPair));
        return;              // Ctrl/Shift 只做多选,不跳转
    }
    if (pressedPair < 0) {
        dedup::traceShift(QStringLiteral("REL DROP rowBefore=%1: pressedPair<0(按下那一行没取到 pair)")
                              .arg(rowBefore));
        return;
    }

    const int row = m_model->rowForPairIndex(pressedPair);
    const DuplicatePairItem *item = m_model->itemAt(row);
    if (!item) {
        /* 【临时探针】pair->row 映射断掉时以前也是静默丢弃 */
        dedup::traceShift(QStringLiteral("REL DROP pressedPair=%1: rowForPairIndex 返回 %2,itemAt 为空")
                              .arg(pressedPair).arg(row));
        return;
    }

    /*
     * 失效判定不能只看 state:Skipped/Removed* 只要 invalidMask 非 0,
     * 说明某一侧已经被别的对移走,点它同样应该跳到肇事那一对
     * (在那种对上改不了任何东西)。
     *
     * 筛选行(filterAnchor >= 0)例外:那些行天然就是 InvalidA(锚点在进入筛选时
     * 已经删掉了),而它们的另一侧还活着、正是要清理的相似版本。
     * 点一下却跳到"肇事那一对"会让整个筛选列表没法用 —— 这里必须按"打开这一对"处理。
     */
    /*
     * 【临时探针】视图眼里的行状态。和窗口 m_pairs 里的真实状态对不上时,
     * 就出现"列表显示 Pending、点它却被窗口当成 Invalid 丢掉"。
     */
    dedup::traceShift(QStringLiteral("REL EMIT pair=%1 state=%2 invalidMask=%3 filterAnchor=%4 -> %5")
                          .arg(pressedPair).arg(int(item->state)).arg(int(item->invalidMask))
                          .arg(item->filterAnchor)
                          .arg((item->filterAnchor < 0
                                && (isInvalidState(item->state) || item->invalidMask != 0))
                                   ? QStringLiteral("invalidPairActivated")
                                   : QStringLiteral("pairActivated")));

    if (item->filterAnchor < 0
        && (isInvalidState(item->state) || item->invalidMask != 0))
        emit invalidPairActivated(pressedPair, true);   // A优先
    else
        emit pairActivated(pressedPair);

    emitLazyLoadForVisibleRows();
}

void DuplicatePairListView::contextMenuEvent(QContextMenuEvent *event)
{
    // 右键不改变选中项,只把"对谁弹菜单"告诉窗口
    if (!event || !viewport()) return;

    int row = -1;
    const QModelIndex idx = indexAt(viewport()->mapFromGlobal(event->globalPos()));
    if (idx.isValid()) row = idx.row();
    else {
        const QModelIndex cur = currentIndex();
        if (cur.isValid()) row = cur.row();
    }

    const DuplicatePairItem *item = m_model->itemAt(row);
    const int ctxPair = item ? item->pairIndex : -1;
    if (ctxPair < 0) return;

    /*
     * 多选规则:右键的那一行如果在选择集里,动作作用于整个选择集(保留批量能力);
     * 否则只作用于这一行 —— 这样"右键某一对 -> Remove A"不会误伤别处的选中项。
     */
    QVector<int> sel = selectedPairIndices();
    const QVector<int> targets = (sel.size() > 1 && sel.contains(ctxPair))
                                     ? sel
                                     : QVector<int>{ctxPair};

    /*
     * 这里【不再】把 current/选择集/锚点还原成弹菜单前的样子。
     *
     * 以前做过"双保险":右键是嵌套事件循环,怕样式/平台在菜单期间顺手改了选中,
     * 于是快照 + 菜单关掉后再还原。但菜单里的动作(Jump This File / Jump Pendings /
     * Remove A/B)本来就会把当前对推进到新的 pending 上、甚至重建列表 ——
     * 还原等于把动作的结果抹掉:
     *   · 列表当前行和锚点退回弹菜单前那一行;
     *   · 而窗口那边 m_currentIndex 已经前进;
     *   · 用户随后 Shift+左键,就从那个旧位置一路选过来。
     * (稳定触发条件:选中区域后面紧跟着一个 invalid pair —— 那种情况下跳转后
     *  没有第二次同步把列表拉回来,错位就一直留着,直到某次重建才自愈。)
     *
     * 右键按下本身在 mousePressEvent 里已经吞掉了,基类拿不到这次按下,
     * 不会发生"右键改选中"这种事,这层还原没有任何收益,只会盖掉真实动作。
     */
    emit contextMenuRequested(ctxPair, targets, event->globalPos());
}


void DuplicatePairListView::resizeEvent(QResizeEvent *event)
{
    QListView::resizeEvent(event);
    emitLazyLoadForVisibleRows();
}

bool DuplicatePairListView::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched);
    switch (event->type()) {
    case QEvent::DragEnter:
    case QEvent::DragMove:
    case QEvent::DragLeave:
    case QEvent::Drop:
        event->ignore();
        return true;
    case QEvent::Wheel:
    case QEvent::Resize:
    case QEvent::Show:
        emitLazyLoadForVisibleRows();
        break;
    default:
        break;
    }
    return QListView::eventFilter(watched, event);
}

void DuplicatePairListView::keyPressEvent(QKeyEvent *event)
{
    /*
     * 把 a/s/d/空格/q 以及 ←/→ 让给父窗口(ImageDeduplicateWindow)处理。
     * 直接 event->ignore() + return,Qt 会把未处理的按键事件沿父链继续传递。
     * 只在没有修饰键时让路,免得吃掉 Ctrl+A 这类标准操作。
     *
     * 例外:Shift+A/S/D 是"以当前这一对为锚点进筛选"(见 ImageDeduplicateWindow::
     * startFilterFromCurrentPair),同样必须冒泡 —— 否则 QListView 会把它当成
     * 键盘增量搜索(keyboardSearch),选中项乱跳、快捷键也到不了窗口。
     * 只放行"纯 Shift";Shift+Ctrl 之类仍然留给列表。
     *
     * ←/→ 本来是 QListView 的内部导航键(在 IconMode 里左右移动选中项),
     * 在 ListMode 下它们没有意义却仍会被消费掉 —— 于是父窗口的
     * "previous/next" 快捷键只要列表有焦点就失效。这里一律放行;
     * ↑/↓ 仍然留给列表自己切换项。
     */
    if (event->modifiers() == Qt::ShiftModifier
        && (event->key() == Qt::Key_A || event->key() == Qt::Key_S
            || event->key() == Qt::Key_D
            /*
             * Shift+←/→ 是"上一个/下一个目录对比区域",Shift+Backspace 是
             * "恢复为 Pending"。三个都必须冒泡到窗口 —— 列表一旦消费掉,
             * 父窗口的 keyPressEvent 就永远收不到。
             */
            || event->key() == Qt::Key_Left || event->key() == Qt::Key_Right
            || event->key() == Qt::Key_Backspace)) {
        event->ignore();
        return;
    }

    if (event->modifiers() == Qt::NoModifier) {
        switch (event->key()) {
        case Qt::Key_A:
        case Qt::Key_S:
        case Qt::Key_D:
        case Qt::Key_Q:
        case Qt::Key_Space:
        case Qt::Key_Left:
        case Qt::Key_Right:
            event->ignore();
            return;
        default:
            break;
        }
    }

    /*
     * ↑↓/Home/End/PageUp/PageDown 由列表自己移动当前项。移动之后当前行就是
     * 新的区域选择锚点 —— 否则"用键盘往下走几十行,再 Shift+左键"会从键盘移动
     * 之前的那一行开始选。
     */
    const bool navKey = (event->modifiers() == Qt::NoModifier)
                        && (event->key() == Qt::Key_Up
                            || event->key() == Qt::Key_Down
                            || event->key() == Qt::Key_Home
                            || event->key() == Qt::Key_End
                            || event->key() == Qt::Key_PageUp
                            || event->key() == Qt::Key_PageDown);

    QListView::keyPressEvent(event);

    if (navKey) {
        const QModelIndex cur = currentIndex();
        m_anchorRow = cur.isValid() ? cur.row() : -1;
        dedup::traceShift(QStringLiteral("NAV key=%1 -> anchor=%2").arg(event->key()).arg(m_anchorRow));
    }
}

void DuplicatePairListView::onScrollValueChanged(int value)
{
    Q_UNUSED(value);
    emitLazyLoadForVisibleRows();
}

/* ============================ 懒加载 ============================ */

void DuplicatePairListView::markRowPainted(int row, bool isA) const
{
    if (isA) m_paintedRowsA.insert(row);
    else     m_paintedRowsB.insert(row);
}

void DuplicatePairListView::onSelectionChanged(const QItemSelection &selected,
                                               const QItemSelection &deselected)
{
    Q_UNUSED(selected);
    Q_UNUSED(deselected);
    if (m_knownSelectionChange) return;   // 我们自己发起的选择变化

    /*
     * 走到这里说明选择集是【别的东西】改的(基类的拖拽/键盘、某些平台行为……)。
     * 这正是"我们算的范围是对的、屏幕上却是一大片"的来源,记下来。
     */
    const QModelIndexList all = selectionModel() ? selectionModel()->selectedIndexes()
                                                 : QModelIndexList();
    int lo = -1, hi = -1;
    for (const QModelIndex &i : all) {
        if (lo < 0 || i.row() < lo) lo = i.row();
        if (i.row() > hi) hi = i.row();
    }
    dedup::traceShift(QStringLiteral("SELCHG(!) n=%1 rows=%2..%3 anchor=%4 current=%5 pending=%6")
                          .arg(all.size()).arg(lo).arg(hi).arg(m_anchorRow)
                          .arg(currentIndex().isValid() ? currentIndex().row() : -1)
                          .arg(m_displaySwitchPending ? 1 : 0));
}

void DuplicatePairListView::flushLazyLoad()
{
    if (m_paintedRowsA.isEmpty() && m_paintedRowsB.isEmpty()) return;

    QVector<int> need;
    auto collect = [&](const QSet<int> &rows) {
        for (int row : rows) {
            const int pi = m_model->pairIndexAt(row);
            if (pi < 0) continue;
            if (m_requestedRows.contains(pi)) continue;
            m_requestedRows.insert(pi);
            need.push_back(pi);
        }
    };
    collect(m_paintedRowsA);
    collect(m_paintedRowsB);

    m_paintedRowsA.clear();
    m_paintedRowsB.clear();

    if (!need.isEmpty())
        emit lazyLoadRequested(need);
}

void DuplicatePairListView::emitLazyLoadForVisibleRows()
{
    /*
     * 只登记"当前真正可见"的行,让它们去懒加载缩略图。
     *
     * 【重要】不能用 for(row=0..count) + visualRect() 去筛:
     * pair 是千级规模,而 visualRect() 每次都要求一次 layout 计算,
     * 这样遍历一遍是 O(N) 且每次都要碰 layout ——
     * 而本函数挂在滚动条 valueChanged / Wheel / Resize 上,滚一下就会被调用,
     * 于是"列表卡顿"就这么来了(越滚越卡,因为每滚一格都全表扫一遍)。
     *
     * QListView 在 TopToBottom 流下,indexAt() 是二分查找,indexAt(视口左上角)
     * 和 indexAt(视口右下角)就能直接框定可见行区间 —— O(log N) 拿到上下界,
     * 只对这几十行做 visualRect,开销可以忽略。
     */
    if (m_model->count() <= 0) return;

    const QRect area = viewport() ? viewport()->rect() : rect();
    if (area.isEmpty()) return;

    // 视口四角 -> 行号(留 1 行余量,避免上下边缘半露的行漏掉)
    int firstRow = -1;
    int lastRow  = -1;
    if (viewport()) {
        const QModelIndex topIdx = indexAt(QPoint(area.left() + 1, area.top() + 1));
        const QModelIndex botIdx = indexAt(QPoint(area.left() + 1, area.bottom() - 1));
        if (topIdx.isValid()) firstRow = topIdx.row();
        if (botIdx.isValid()) lastRow  = botIdx.row();
    }

    if (firstRow < 0 || lastRow < 0) {
        // indexAt 偶尔会在极窄视口/布局未就绪时返回无效,退化成"只取第一行",
        // 总比全表扫强;真正可见的行会在随后的 paint 里通过 markRowPainted 补登记。
        firstRow = qMax(0, firstRow < 0 ? 0 : firstRow);
        lastRow  = qMin(m_model->count() - 1, lastRow < 0 ? firstRow : lastRow);
    }

    firstRow = qMax(0, firstRow - 1);
    lastRow  = qMin(m_model->count() - 1, lastRow + 1);

    for (int row = firstRow; row <= lastRow; ++row) {
        const QRect itemRect = visualRect(m_model->index(row, 0));
        if (itemRect.isValid() && itemRect.intersects(area)) {
            m_paintedRowsA.insert(row);
            m_paintedRowsB.insert(row);
        }
    }

    /*
     * 【注意别 resume 定时器】
     * 原来是每次都 m_lazyTimer->start(),而 start() 会重置 60ms 计时 ——
     * 鼠标滚轮连续滚动时 valueChanged 每隔几十毫秒就来一次,
     * 于是计时器永远等不到触发,缩略图只在"用户彻底停下 60ms 之后"才开始加载,
     * 这就是"滚过去看不到预览图,得再滚一下/停一下才出现"的直接原因。
     *
     * 改成只在计时器空闲时才启动:第一个滚动事件起 60ms 后就开始加载,
     * 之后每完成一批,下一个滚动事件又会重新起一个周期 —— 滚动过程中就能陆续出图。
     */
    if (m_lazyTimer && !m_lazyTimer->isActive())
        m_lazyTimer->start();
}
