#ifndef FOLDERCOMPAREWIDGET_H
#define FOLDERCOMPAREWIDGET_H

#include <QAbstractListModel>
#include <QHash>
#include <QImage>
#include <QLabel>
#include <QStringList>
#include <QStyledItemDelegate>
#include <QVector>
#include <QWidget>

#include "foldercompare.h"

namespace Ui {
class FolderCompareWidget;
}

/*
 * 人工队列的一行。
 *
 * 照 ImageDeduplicateWindow 的 pair 列表设计:每行左侧是 A 侧 2x2 四张预览图、
 * 紧挨着 B 侧 2x2 四张,右边是目录名 + 张数 + 结论。
 * 缩略图在 setQueue() 时就缩好放进结构体,delegate 的 paint 里不再读盘。
 */
struct FolderQueueEntry {
    QString dirA;
    QString dirB;
    int     countA = 0;
    int     countB = 0;
    QString decision;        // ""=未决定 / "A" / "B" / "jump"
    QStringList previewA;    // 目录 A 的预览图路径(首/中二/末,最多 4)
    QStringList previewB;
    bool    current = false; // 当前视图就是这一行
    QVector<QImage> thumbsA; // 已缩放到 delegate 需要的大小(最多 4)
    QVector<QImage> thumbsB;
};

/* 队列模型(纯数据,不含任何文件操作) */
class FolderQueueModel : public QAbstractListModel
{
    Q_OBJECT
public:
    explicit FolderQueueModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    void setItems(const QVector<FolderQueueEntry> &items);
    void setCurrentRow(int row);
    const FolderQueueEntry *itemAt(int row) const;

private:
    QVector<FolderQueueEntry> m_items;
    int m_currentRow = -1;
};

/* 队列委托:两侧缩略图 + 两行文字 + 结论徽标,选中/悬停有底色 */
class FolderQueueDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit FolderQueueDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;
};

/*
 * 目录级比较窗口(半选 internal 合集判定的"人工复核"环节)。
 *
 * 布局两段:左(当前目录对的路径/指标/按钮) | 右(整个人工队列,每行带 A/B 各 2x2 预览)。
 * 需求明确要求"每侧最多 4 张缩略图、按递归路径取首/中二/末",所以预览图就画在
 * 队列行里,**不再单独开右侧预览面板** —— 一份数据两个地方画,既占地方又容易不一致。
 */
class FolderCompareWidget : public QWidget
{
    Q_OBJECT

public:
    using QueueEntry = FolderQueueEntry;

    explicit FolderCompareWidget(QWidget *parent = nullptr);
    ~FolderCompareWidget() override;

    /* 设置当前要比较的目录对(含统计与缩略图路径) */
    void setView(const foldercompare::CompareView &view);

    /* 队列进度(显示在标题上);total<=0 表示单条 */
    void setQueueInfo(int index, int total);

    /* 整个人工队列 + 当前位于第几项 */
    void setQueue(const QVector<QueueEntry> &items, int current);

    /* 当前视图(调用方据此知道"这一对是谁") */
    const foldercompare::CompareView &view() const { return m_view; }

signals:
    void keepA();
    void keepB();
    void skip();
    void previous();
    /* 用户在列表里点了另一对(参数是队列下标) */
    void selectIndex(int index);
    /*
     * 用户直接用标题栏的 X 关窗。调用方必须拿它当"剩下的目录对全部 jump"来收尾,
     * 否则队列永远停在原地、图片对一直不建(界面看起来就是卡死)。
     */
    void closedByUser();

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void on_btn_keep_a_clicked();
    void on_btn_keep_b_clicked();
    void on_btn_jump_clicked();
    void on_btn_previous_clicked();
    void on_list_row_clicked(const QModelIndex &index);

private:
    /* 两栏布局 + 窗口初始尺寸(预览图在队列行里,不再单独开面板) */
    void setupLayout();
    void updateLabels();
    /*
     * 高亮"自动建议"那一侧的按钮。
     * setStyleSheet 是整表替换,不能在无建议时写 setStyleSheet(QString()) ——
     * 那会把 .ui 里两个按钮的配色一起擦掉(人工项本来就没有建议,等于必中)。
     */
    void applyHintHighlight();
    /* 目录:进入目录本身;文件:在资源管理器里定位 */
    void openFolderOf(const QString &path);
    /*
     * 路径标签按可用宽度做"中间省略"。
     * 完整路径里没有空格,QLabel 的 wordWrap 根本不会换行,直接 setText 会被硬裁掉
     * 后半截,看起来像路径本身不完整。完整值仍在 tooltip 里。
     */
    void updatePathLabels();
    /* 队列行缩略图:按目标尺寸读,读过的按路径缓存(切队列时不重复解码) */
    QImage loadQueueThumb(const QString &path);

    Ui::FolderCompareWidget *ui;
    foldercompare::CompareView m_view;

    FolderQueueModel *m_queueModel = nullptr;
    /* 队列缩略图缓存:key = 图片路径 */
    QHash<QString, QImage> m_thumbCache;
    /* true = 正在程序性同步选中行,不要回发 selectIndex */
    bool m_syncingQueue = false;
    /* 两条路径的完整(原生分隔符)文本,用于按宽度重新省略 */
    QString m_pathA;
    QString m_pathB;

    /* .ui 里的按钮基线样式,setStyleSheet 覆盖后要能还原 */
    QString m_baseStyleKeepA;
    QString m_baseStyleKeepB;
};

#endif // FOLDERCOMPAREWIDGET_H
