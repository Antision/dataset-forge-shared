#include "foldercomparewidget.h"
#include "ui_foldercomparewidget.h"

#include "dedupenv.h"
#include "fonts.h"          // appFontFamily()

#include <QCloseEvent>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QListView>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QUrl>
#include <QVBoxLayout>
#include <QtMath>

namespace {

constexpr int kThumbW = 168;
constexpr int kThumbH = 126;
/* 队列行:每侧是一个 2x2 预览块,单张缩略图 52px(方形,等比缩放居中) */
constexpr int kQueueThumb    = 52;
constexpr int kQueueMargin   = 6;    // 行内边距
constexpr int kQueueBlockGap = 10;   // A 块与 B 块之间

/* A 更优 -> 绿;平局 -> 白;B 更优 -> 红 */
const QColor kColorBetter(0, 200, 100);
const QColor kColorWorse(255, 90, 90);
const QColor kColorNeutral(255, 255, 255);

QLabel *makeThumb()
{
    auto *l = new QLabel;
    l->setFixedSize(kThumbW, kThumbH);
    l->setAlignment(Qt::AlignCenter);
    l->setScaledContents(false);
    l->setStyleSheet(QStringLiteral("QLabel{background-color:#141414;color:#666;}"));
    return l;
}

/*
 * 只解码到缩略图大小。
 * 两个坑:
 *   1) QImageReader::setScaledSize 只有部分解码器支持(JPEG 支持,PNG 之类可能被
 *      忽略),这时 read() 会返回整张原图。直接把原图塞进固定尺寸的 QLabel 会被
 *      "居中裁掉",看起来就像缩略图被裁了一块。所以读出来之后无条件再等比缩一次。
 *   2) 高 DPI 屏上按逻辑像素取图会糊:按 devicePixelRatio 放大解码再设回去。
 * 全程 Qt::KeepAspectRatio,不拉伸。
 */
QPixmap loadThumb(const QString &path, const QSize &box, qreal dpr)
{
    const qreal ratio = (dpr > 1.0) ? dpr : 1.0;
    const QSize px(qCeil(box.width() * ratio), qCeil(box.height() * ratio));

    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize src = reader.size();
    if (src.isValid() && !src.isEmpty()
        && (src.width() > px.width() || src.height() > px.height())) {
        reader.setScaledSize(src.scaled(px, Qt::KeepAspectRatio));
    }

    QImage img = reader.read();
    if (img.isNull())
        return QPixmap();
    if (img.width() > px.width() || img.height() > px.height())
        img = img.scaled(px, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QPixmap pm = QPixmap::fromImage(img);
    if (ratio > 1.0)
        pm.setDevicePixelRatio(ratio);
    return pm;
}

QColor betterColor(double a, double b, double eps = 1e-9)
{
    if (a > b + eps) return kColorBetter;
    if (b > a + eps) return kColorWorse;
    return kColorNeutral;
}

/* 指标标签的底色由窗口样式表给,这里只改颜色 —— 逐条设置时不能带上其他声明 */
void setLabelColor(QLabel *l, const QColor &c)
{
    l->setStyleSheet(QStringLiteral("QLabel{color:%1;}").arg(c.name()));
}

} // namespace

FolderCompareWidget::FolderCompareWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::FolderCompareWidget)
{
    ui->setupUi(this);
    setWindowTitle(QStringLiteral("Folder Compare"));

    // 先把 .ui 的按钮样式留一份基线,后面高亮/取消高亮都基于它
    m_baseStyleKeepA = ui->btn_keep_a->styleSheet();
    m_baseStyleKeepB = ui->btn_keep_b->styleSheet();


    ui->lb_folder_a_path->installEventFilter(this);
    ui->lb_folder_b_path->installEventFilter(this);
    ui->lb_folder_a_path->setCursor(Qt::PointingHandCursor);
    ui->lb_folder_b_path->setCursor(Qt::PointingHandCursor);
    /*
     * 路径标签自己不做宽度主张:宽度由布局给,文字按可用宽度中间省略。
     * 用 Ignored 横向量,省略后的文本长度就不会反过来把布局撑宽(否则会来回抖)。
     */
    for (QLabel *l : {ui->lb_folder_a_path, ui->lb_folder_b_path})
        l->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    /*
     * 中间列表 = 整个人工队列。
     * 每一行照 ImageDeduplicateWindow 的 pair 列表来做:A/B 各一张缩略图 + 目录名 +
     * 张数 + 结论徽标。点一行就跳到那一对(已答过的会撤销原结论重新问),
     * 选中即切换,不需要双击。
     */
    setupLayout();

    m_queueModel = new FolderQueueModel(this);
    ui->listView->setModel(m_queueModel);
    ui->listView->setItemDelegate(new FolderQueueDelegate(this));
    ui->listView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->listView->setSelectionMode(QAbstractItemView::SingleSelection);
    ui->listView->setUniformItemSizes(true);
    ui->listView->setMinimumWidth(320);
    ui->listView->setMouseTracking(true);
    // 行宽有限:横向滚动条比省略号更碍事,文字尾部用省略号就好
    ui->listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->listView->setTextElideMode(Qt::ElideRight);
    connect(ui->listView, &QListView::clicked,
            this, &FolderCompareWidget::on_list_row_clicked);
}

FolderCompareWidget::~FolderCompareWidget()
{
    delete ui;
}

void FolderCompareWidget::setupLayout()
{
    /*
     * 只有两栏:左(当前目录对的路径/指标/按钮) | 右(人工队列)。
     * 队列每行自己就带 A/B 各 2x2 预览图 —— 需求要的就是这个,
     * 不再额外开一个右侧预览面板(同一个目录画两遍,既占地方又容易对不上)。
     */
    ui->splitter->setStretchFactor(0, 3);
    ui->splitter->setStretchFactor(1, 7);

    // 队列行:两侧各 2x2 张 52px 缩略图 + 文字 -> 至少 620 宽才不挤
    ui->listView->setMinimumWidth(620);

    const QSize need = ui->layoutWidget ? ui->layoutWidget->sizeHint() : QSize(360, 520);
    resize(qMax(width(), need.width() + 660), qMax(height(), qMax(560, need.height() + 80)));
}

void FolderCompareWidget::setQueueInfo(int index, int total)
{
    if (total > 1)
        setWindowTitle(QStringLiteral("Folder Compare (%1/%2)").arg(index + 1).arg(total));
    else
        setWindowTitle(QStringLiteral("Folder Compare"));
}

void FolderCompareWidget::setQueue(const QVector<QueueEntry> &items, int current)
{
    if (!m_queueModel)
        return;

    /*
     * 缩略图在这里一次缩好:队列一般只有个位数到十几个目录对,
     * 用 QImageReader::setScaledSize 读比"整图解码再缩放"快一个量级,
     * 而且只读每边第一张预览图(用户只需要认出这是哪个目录)。
     */
    QVector<QueueEntry> prepared = items;
    for (int i = 0; i < prepared.size(); ++i) {
        prepared[i].thumbsA.clear();
        prepared[i].thumbsB.clear();
        for (const QString &p : std::as_const(prepared[i].previewA)) {
            const QImage img = loadQueueThumb(p);
            if (!img.isNull())
                prepared[i].thumbsA << img;
        }
        for (const QString &p : std::as_const(prepared[i].previewB)) {
            const QImage img = loadQueueThumb(p);
            if (!img.isNull())
                prepared[i].thumbsB << img;
        }
        prepared[i].current = (i == current);
    }

    m_syncingQueue = true;
    m_queueModel->setItems(prepared);
    m_queueModel->setCurrentRow(current);

    if (m_queueModel->rowCount() > 0) {
        const int row = qBound(0, current, m_queueModel->rowCount() - 1);
        ui->listView->setCurrentIndex(m_queueModel->index(row, 0));
        ui->listView->scrollTo(m_queueModel->index(row, 0), QAbstractItemView::EnsureVisible);
    }
    m_syncingQueue = false;
}

QImage FolderCompareWidget::loadQueueThumb(const QString &path)
{
    if (path.isEmpty())
        return QImage();

    const auto it = m_thumbCache.constFind(path);
    if (it != m_thumbCache.constEnd())
        return it.value();

    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize orig = reader.size();
    if (orig.isValid()) {
        QSize target = orig;
        target.scale(QSize(128, 128), Qt::KeepAspectRatio);
        if (target.width() > 0 && target.height() > 0)
            reader.setScaledSize(target);
    }
    const QImage img = reader.read();
    if (img.isNull())
        return QImage();

    m_thumbCache.insert(path, img);
    return img;
}

void FolderCompareWidget::on_list_row_clicked(const QModelIndex &index)
{
    if (m_syncingQueue || !index.isValid())
        return;
    emit selectIndex(index.row());
}

void FolderCompareWidget::on_btn_previous_clicked()
{
    // 回到上一对;撤销/重答由调用方处理(见 presentNextFolderCompare)
    emit previous();
}

void FolderCompareWidget::setView(const foldercompare::CompareView &view)
{
    m_view = view;

    ui->lb_folder_a_path->setToolTip(QStringLiteral("点击在资源管理器中打开:%1").arg(view.dirA));
    ui->lb_folder_b_path->setToolTip(QStringLiteral("点击在资源管理器中打开:%1").arg(view.dirB));
    // 文本不直接写:统一交给 updatePathLabels() 按可用宽度做中间省略
    m_pathA = QDir::toNativeSeparators(view.dirA);
    m_pathB = QDir::toNativeSeparators(view.dirB);
    updatePathLabels();

    updateLabels();
    applyHintHighlight();

}

void FolderCompareWidget::updateLabels()
{
    const auto &v = m_view;
    const QString kNa = QStringLiteral("N/A");

    // ---- 递归图片数:多的那边绿,平的白色,少的红 ----
    setLabelColor(ui->lb_filecount, betterColor(v.countA, v.countB));
    ui->lb_filecount->setText(QStringLiteral("图片数  A %1 / B %2").arg(v.countA).arg(v.countB));
    ui->lb_filecount->setToolTip(QStringLiteral("递归图片总数,含不相似图片。A 更多=绿,平=白,B 更多=红。"));

    // ---- 平均质量(只统计相似匹配到的图片;没有匹配项时显示 N/A) ----
    if (v.hasMatch) {
        setLabelColor(ui->lb_avg_quality, betterColor(v.qualityA, v.qualityB));
        ui->lb_avg_quality->setText(
            QStringLiteral("平均质量  A %1 / B %2")
                .arg(foldercompare::formatDouble(v.qualityA, 4),
                     foldercompare::formatDouble(v.qualityB, 4)));
    } else {
        setLabelColor(ui->lb_avg_quality, kColorNeutral);
        ui->lb_avg_quality->setText(QStringLiteral("平均质量  A %1 / B %2").arg(kNa, kNa));
    }
    ui->lb_avg_quality->setToolTip(
        QStringLiteral("只统计与对方相似匹配到的图片;压缩质量是估算值,不是最终视觉结论。"));

    // ---- 平均分辨率:显示 WxH,比较按像素总数(宽*高) ----
    if (v.hasMatch) {
        setLabelColor(ui->lb_avg_resolution, betterColor(v.pixelsA, v.pixelsB));
        ui->lb_avg_resolution->setText(
            QStringLiteral("平均分辨率  A %1x%2 / B %3x%4")
                .arg(qRound(v.widthA)).arg(qRound(v.heightA))
                .arg(qRound(v.widthB)).arg(qRound(v.heightB)));
    } else {
        setLabelColor(ui->lb_avg_resolution, kColorNeutral);
        ui->lb_avg_resolution->setText(QStringLiteral("平均分辨率  A %1 / B %1").arg(kNa));
    }
    ui->lb_avg_resolution->setToolTip(
        QStringLiteral("按平均像素总数(宽*高)比较,不是按字符串比较。"));

    // ---- 平均文件大小(MB,6 位小数) ----
    if (v.hasMatch) {
        setLabelColor(ui->lb_avg_filesize, betterColor(v.sizeA, v.sizeB));
        ui->lb_avg_filesize->setText(
            QStringLiteral("平均大小  A %1 MB / B %2 MB")
                .arg(foldercompare::formatDouble(v.sizeA, 6),
                     foldercompare::formatDouble(v.sizeB, 6)));
    } else {
        setLabelColor(ui->lb_avg_filesize, kColorNeutral);
        ui->lb_avg_filesize->setText(QStringLiteral("平均大小  A %1 / B %2").arg(kNa, kNa));
    }
    ui->lb_avg_filesize->setToolTip(QStringLiteral("只统计相似匹配到的图片,单位 MB。"));

    /*
     * ---- 相似/重复数量 ----
     * 语义 = 各侧"被相似命中的图片数"(不是对数,也不是"可被覆盖的文件数")。
     * 这是纯计数:命中多只说明重叠多,不代表这一侧更该留 ——
     * 所以用中性色,不要套"A 更优=绿"的规则(提示词 §5.2)。
     */
    setLabelColor(ui->lb_duplicate_count, kColorNeutral);
    ui->lb_duplicate_count->setText(
        QStringLiteral("相似命中  A %1 / B %2").arg(v.duplicateA).arg(v.duplicateB));
    QString dupTip = QStringLiteral("两侧各自被相似命中的图片数(一对一匹配),计数不代表优劣。");
    if (!v.note.isEmpty())
        dupTip += QStringLiteral("\n判定依据:%1").arg(v.note);
    ui->lb_duplicate_count->setToolTip(dupTip);

    // ---- 判定依据(为什么把你叫到这个人工资格) ----
    QString note = v.note;
    if (!v.keepHint.isEmpty()) {
        note = QStringLiteral("建议保留 %1 · %2").arg(v.keepHint, v.note);
    }
    if (ui->lb_note)
        ui->lb_note->setText(note);
}

void FolderCompareWidget::applyHintHighlight()
{
    const bool hintA = (m_view.keepHint == QLatin1String("A"));
    const bool hintB = (m_view.keepHint == QLatin1String("B"));

    // 追加一条规则(而不是替换整表):保留 .ui 的底色/悬停,只加一圈提示边框
    const QString rule = QStringLiteral("\nQPushButton{border:2px solid %1;}").arg(kColorBetter.name());
    ui->btn_keep_a->setStyleSheet(hintA ? m_baseStyleKeepA + rule : m_baseStyleKeepA);
    ui->btn_keep_b->setStyleSheet(hintB ? m_baseStyleKeepB + rule : m_baseStyleKeepB);
}

void FolderCompareWidget::on_btn_keep_a_clicked()
{
    emit keepA();
}

void FolderCompareWidget::on_btn_keep_b_clicked()
{
    emit keepB();
}

void FolderCompareWidget::on_btn_jump_clicked()
{
    emit skip();
}

void FolderCompareWidget::closeEvent(QCloseEvent *event)
{
    // 交给调用方收尾(剩余目录对按 jump 处理),否则主窗口会一直等一个不会再来的决定
    emit closedByUser();
    QWidget::closeEvent(event);
}

void FolderCompareWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updatePathLabels();
}

void FolderCompareWidget::updatePathLabels()
{
    auto elide = [](QLabel *l, const QString &full) {
        if (!l)
            return;
        // 标签还没被布局过时宽度不可信,给个下限,等 resize 事件再补
        const int avail = qMax(80, l->width() - 8);
        const QString shown = full.isEmpty()
                                  ? QString()
                                  : l->fontMetrics().elidedText(full, Qt::ElideMiddle, avail);
        if (l->text() != shown)
            l->setText(shown);   // 只在真要变时写,避免 resize -> setText -> 重新布局的死循环
    };
    elide(ui->lb_folder_a_path, m_pathA);
    elide(ui->lb_folder_b_path, m_pathB);
}

void FolderCompareWidget::openFolderOf(const QString &path)
{
    if (path.isEmpty())
        return;

    const QFileInfo fi(path);
    if (fi.isDir()) {
        /*
         * 这两个标签是"目录",用户点它就是想进去看。
         * utils::revealFileInExplorer() 对目录是"打开父目录并选中它",不符合这里的预期,
         * 所以目录走 ShellExecute 的 open(等价于资源管理器双击),失败再退回定位。
         */
        if (QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absoluteFilePath())))
            return;
    }
    dedupenv::revealFileInExplorer(fi.absoluteFilePath());
}

bool FolderCompareWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->lb_folder_a_path || watched == ui->lb_folder_b_path) {
        /*
         * 标签宽度由布局决定,而且是在 setView 之后才定下来,所以不能只在 setView 里
         * 算一次省略(那时宽度还是初值)。这里跟着标签自己的 resize 重算。
         * updatePathLabels 内部有"内容没变就不写"的保护,不会和布局互相触发。
         */
        if (event->type() == QEvent::Resize) {
            updatePathLabels();
        } else if (event->type() == QEvent::MouseButtonRelease) {
            const QString path = (watched == ui->lb_folder_a_path) ? m_view.dirA : m_view.dirB;
            if (!path.isEmpty()) {
                openFolderOf(path);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}


/* ==========================================================================
 *  人工队列:model + delegate
 *
 *  为什么不用 QStandardItemModel 塞字符串(上一版就是那么干的):
 *  目录对比较里"这是哪两个目录"是唯一需要一眼认出的信息,纯文本行要读完路径
 *  才知道,而且列宽一窄就只剩省略号。这里照 DuplicatePairListView 的做法,
 *  把两侧首图缩成 52px 直接画在行里,再配两行文字,扫一眼就能定位。
 * ========================================================================== */

FolderQueueModel::FolderQueueModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int FolderQueueModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_items.size());
}

QVariant FolderQueueModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return QVariant();

    const FolderQueueEntry &e = m_items[index.row()];
    switch (role) {
    case Qt::DisplayRole:
        return QStringLiteral("%1(%2) vs %3(%4)")
            .arg(QFileInfo(e.dirA).fileName()).arg(e.countA)
            .arg(QFileInfo(e.dirB).fileName()).arg(e.countB);
    case Qt::ToolTipRole:
        return QStringLiteral("%1\n%2").arg(e.dirA, e.dirB);
    default:
        return QVariant();
    }
}

void FolderQueueModel::setItems(const QVector<FolderQueueEntry> &items)
{
    beginResetModel();
    m_items = items;
    endResetModel();
}

void FolderQueueModel::setCurrentRow(int row)
{
    m_currentRow = row;
}

const FolderQueueEntry *FolderQueueModel::itemAt(int row) const
{
    if (row < 0 || row >= m_items.size())
        return nullptr;
    return &m_items[row];
}

FolderQueueDelegate::FolderQueueDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

QSize FolderQueueDelegate::sizeHint(const QStyleOptionViewItem &option,
                                    const QModelIndex &index) const
{
    Q_UNUSED(option);
    Q_UNUSED(index);
    // 两侧各一个 2x2 预览块(2*52 + 1px 缝) + 上下 margin
    return QSize(640, kQueueThumb * 2 + 1 + 2 * kQueueMargin);
}

void FolderQueueDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                const QModelIndex &index) const
{
    const auto *model = qobject_cast<const FolderQueueModel *>(index.model());
    if (!model) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }
    const FolderQueueEntry *e = model->itemAt(index.row());
    if (!e)
        return;

    painter->save();
    // 与 ImageDeduplicateWindow 的 pair 列表保持同一套观感:
    // 选中 = 墨绿底、悬停 = 紫灰底、普通 = near-black;文字 12px。
    const QRect r = option.rect;
    const bool selected = (option.state & QStyle::State_Selected) != 0;
    if (selected)
        painter->fillRect(r, QColor(30, 125, 93));
    else if (option.state & QStyle::State_MouseOver)
        painter->fillRect(r, QColor(38, 31, 56));
    else
        painter->fillRect(r, QColor(24, 24, 40));

    const int m = kQueueMargin;
    const int gap = 1;                       // 2x2 内部缝
    const int blockW = kQueueThumb * 2 + gap;

    // ---- A 侧 2x2 ----
    const QRect blockA(r.left() + m, r.top() + m, blockW, blockW);
    // ---- B 侧 2x2 ----
    const QRect blockB(blockA.right() + 1 + kQueueBlockGap, r.top() + m, blockW, blockW);

    auto drawBlock = [&](const QRect &block, const QVector<QImage> &imgs) {
        for (int i = 0; i < 4; ++i) {
            const QRect cell(block.left() + (i % 2) * (kQueueThumb + gap),
                             block.top() + (i / 2) * (kQueueThumb + gap),
                             kQueueThumb, kQueueThumb);
            painter->fillRect(cell, QColor(12, 12, 24));
            if (i >= imgs.size() || imgs[i].isNull()) {
                // 图片不足 4 张:留空位,不重复引用同一张造成误导
                painter->setPen(QColor(70, 70, 90));
                painter->drawText(cell, Qt::AlignCenter, QStringLiteral("-"));
                continue;
            }
            const QImage &img = imgs[i];
            const QSize scaled = img.size().scaled(cell.size(), Qt::KeepAspectRatio);
            const QRect dst(cell.left() + (cell.width() - scaled.width()) / 2,
                            cell.top() + (cell.height() - scaled.height()) / 2,
                            scaled.width(), scaled.height());
            painter->drawImage(dst, img);
        }
    };
    drawBlock(blockA, e->thumbsA);
    drawBlock(blockB, e->thumbsB);

    // A / B 的角标:2x2 左上角那一格压一个小字母,避免两团图分不清哪边是 A
    auto drawTag = [&](const QRect &block, const QString &text, const QColor &color) {
        const QRect tag(block.left() + 2, block.top() + 2, 14, 14);
        painter->fillRect(tag, QColor(0, 0, 0, 190));
        QFont f(appFontFamily());
        f.setPixelSize(10);
        f.setBold(true);
        painter->setFont(f);
        painter->setPen(color);
        painter->drawText(tag, Qt::AlignCenter, text);
    };
    drawTag(blockA, QStringLiteral("A"), QColor(95, 215, 164));
    drawTag(blockB, QStringLiteral("B"), QColor(255, 138, 138));

    // ---- 右侧文字 ----
    const int textLeft = blockB.right() + 1 + kQueueBlockGap + 4;
    const int textWidth = qMax(120, r.right() - textLeft - m);

    QFont font(appFontFamily());
    font.setPixelSize(12);
    painter->setFont(font);
    const QFontMetrics fm(font);
    const int lineH = fm.height() + 2;
    int y = r.top() + m;

    // 第 1 行:序号 + 两个目录名 + 张数
    painter->setPen(QColor(230, 230, 230));
    const QString line1 = QStringLiteral("%1. %2 (%3)  vs  %4 (%5)")
                              .arg(index.row() + 1)
                              .arg(QFileInfo(e->dirA).fileName()).arg(e->countA)
                              .arg(QFileInfo(e->dirB).fileName()).arg(e->countB);
    painter->drawText(textLeft, y + fm.ascent(),
                      fm.elidedText(line1, Qt::ElideMiddle, textWidth));
    y += lineH;

    // 第 2 行:结论
    QString line2;
    QColor color2(160, 160, 175);
    if (e->decision == QLatin1String("jump")) {
        line2 = QStringLiteral("已跳过：这两个目录之间不比较");
        color2 = QColor(232, 182, 92);
    } else if (!e->decision.isEmpty()) {
        line2 = QStringLiteral("已选择：保留 %1").arg(e->decision);
        color2 = QColor(95, 215, 164);
    } else if (e->current) {
        line2 = QStringLiteral("正在处理");
        color2 = QColor(120, 190, 255);
    } else {
        line2 = QStringLiteral("待处理");
    }
    painter->setPen(color2);
    painter->drawText(textLeft, y + fm.ascent(),
                      fm.elidedText(line2, Qt::ElideMiddle, textWidth));
    y += lineH;

    // 第 3、4 行:两条完整路径(中间省略) —— pair 列表里也是这么放的
    painter->setPen(QColor(170, 210, 255));
    painter->drawText(textLeft, y + fm.ascent(),
                      fm.elidedText(QDir::toNativeSeparators(e->dirA), Qt::ElideMiddle, textWidth));
    y += lineH;
    painter->drawText(textLeft, y + fm.ascent(),
                      fm.elidedText(QDir::toNativeSeparators(e->dirB), Qt::ElideMiddle, textWidth));

    // 当前行左侧竖条
    if (e->current)
        painter->fillRect(QRect(r.left(), r.top(), 3, r.height()), QColor(120, 190, 255));

    painter->restore();
}
