#include "taglistmodel.h"
#include "fonts.h"
#include "tagmarks.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QLinearGradient>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QTimer>

#include <algorithm>

namespace {

/// Colours of one tag widget state, taken from the sheets teTagBase::setStyle()
/// applies to the widget based list (teTagBase::normal / select / select_current).
struct TagColors{
    QColor border;
    QColor background;
    QColor hoverBorder;
};

TagColors colorsFor(bool selected,bool current,bool hovered)
{
    TagColors colors;
    if(current){
        colors.border      = QColor(0x10,0x94,0x52);
        colors.hoverBorder = QColor(0x00,0xd9,0x6d);
        colors.background  = QColor(0x1a,0x57,0x59);
    }else if(selected){
        colors.border      = QColor(0x3e,0x5e,0x4b);
        colors.hoverBorder = QColor(0x2a,0x6b,0x45);
        colors.background  = QColor(0x17,0x3d,0x3e);
    }else{
        colors.border      = QColor(0x43,0x36,0x7b);
        colors.hoverBorder = QColor(0xb2,0x88,0xff);
        colors.background  = QColor(0x00,0x00,0x00);
    }
    Q_UNUSED(hovered);
    return colors;
}

}

// ---------------------------------------------------------------- TagListModel

TagListModel::TagListModel(QObject* parent):QAbstractListModel(parent)
{
}

TagListModel::~TagListModel()
{
    disconnectList();
}

void TagListModel::connectList(teTagList* list)
{
    if(!list)
        return;
    connect(list,&teTagList::tagInserted,this,&TagListModel::onListInserted);
    connect(list,&teTagList::tagErased,this,&TagListModel::onListErased);
    connect(list,&teTagList::tagEdited,this,&TagListModel::onListEdited);
    connect(list,&teTagList::tagMoved,this,[this]{
        // Ignore our own moves: the signal arrives between beginMoveRows() and
        // endMoveRows(), and announcing a layout change there corrupts the
        // view's bookkeeping.
        if(m_selfMutation)
            return;
        notifyExternalReorder();
    });
}

void TagListModel::disconnectList()
{
    if(!m_list)
        return;
    disconnect(m_list,0,this,0);
    m_list=nullptr;
}

void TagListModel::setTagList(teTagList* list)
{
    if(m_list==list)
        return;
    beginResetModel();
    disconnectList();
    m_list=list;
    connectList(m_list);
    endResetModel();
}

int TagListModel::rowCount(const QModelIndex& parent) const
{
    if(parent.isValid()||!m_list)
        return 0;
    return int(m_list->size());
}

QVariant TagListModel::data(const QModelIndex& index,int role) const
{
    if(!m_list||!index.isValid()||index.row()<0||index.row()>=int(m_list->size()))
        return QVariant();
    // The row is looked up instead of trusting a stored pointer: a stale index
    // (kept by a delegate or by the selection model) must not hand out a
    // dangling tag.
    teTag* tag = m_list->at(index.row());
    if(!tag)
        return QVariant();
    switch(role){
    case Qt::DisplayRole:
    case Qt::EditRole:
    case Qt::ToolTipRole:
    case Qt::AccessibleTextRole:
        return QVariant(static_cast<QString>(*tag));
    case TagCoreRole:
        return QVariant::fromValue(tag);
    default:
        return QVariant();
    }
}

bool TagListModel::setData(const QModelIndex& index,const QVariant& value,int role)
{
    if(role!=Qt::EditRole||!value.canConvert<QString>())
        return false;
    return setTagText(index.row(),value.toString());
}

Qt::ItemFlags TagListModel::flags(const QModelIndex& index) const
{
    // The invalid index stands for the list itself, and it has to accept drops:
    // dropping *between* two tags is a drop on the parent, so returning
    // NoItemFlags here made Qt refuse every drop that was not *onto* a tag.
    if(!index.isValid())
        return Qt::ItemIsDropEnabled;
    return Qt::ItemIsSelectable|Qt::ItemIsEnabled|Qt::ItemIsEditable
           |Qt::ItemIsDragEnabled|Qt::ItemIsDropEnabled;
}

teTag* TagListModel::tagAt(int row) const
{
    if(!m_list||row<0||row>=int(m_list->size()))
        return nullptr;
    return m_list->at(row);
}

int TagListModel::rowOf(teTag* tag) const
{
    if(!m_list||!tag)
        return -1;
    for(int i=0;i<int(m_list->size());++i)
        if(m_list->at(i)==tag)
            return i;
    return -1;
}

int TagListModel::insertTag(int row,std::shared_ptr<teTag> tag,int removeDuplicate)
{
    if(!m_list||!tag)
        return -1;
    if(tag->retired)
        return -1;
    if(m_mutationDepth>0){
        // A merge (or another structural change) asked for this while an outer
        // change is still announcing itself: queue it, see m_mutationDepth.
        m_pendingMutations.push_back([this,row,tag,removeDuplicate]{
            insertTag(row,tag,removeDuplicate);
        });
        return 0;
    }
    MutationGuard guard(this);
    // Refuse before announcing anything: the old widget based code announced the
    // insertion first and then had to take it back.
    if(removeDuplicate==1&&m_list->remove_duplicate(tag,false))
        return -1;
    if(removeDuplicate==2){
        // Merging erases the tags already there; those erases go through the
        // signals, so they are announced properly and this stays a plain insert.
        m_list->remove_duplicate(tag,true);
    }
    row = std::clamp(row,0,int(m_list->size()));
    beginInsertRows(QModelIndex(),row,row);
    m_selfMutation=true;
    // Insert *without* letting teTagList announce it: that announcement reaches
    // the editors, whose filter() merges the new tag into an existing one - and a
    // merge erases rows. A nested structural change inside begin/endInsertRows()
    // corrupts Qt's persistent indexes (it crashed in endInsertRows() as soon as
    // anything was selected). The announcement is made below instead, once the
    // model is consistent again.
    const int result = m_list->insert(row,tag,0,false);     // 0: dedupe already done
    const int inserted = m_list->find(tag);
    m_selfMutation=false;
    endInsertRows();
    if(inserted >= 0)
        m_list->onTagInserted(tag,inserted,true);           // now the editors may react
    return result;
}

bool TagListModel::eraseTag(int row)
{
    if(!m_list||row<0||row>=int(m_list->size()))
        return false;
    if(m_mutationDepth>0){
        // Same as insertTag(): run it once the outer change is consistent.
        const std::shared_ptr<teTag> tag = m_list->shareAt(row);
        m_pendingMutations.push_back([this,tag]{
            if(!m_list||!tag)
                return;
            const int at = rowOf(tag.get());
            if(at>=0)
                eraseTag(at);
        });
        return true;
    }
    MutationGuard guard(this);
    const std::shared_ptr<teTag> tag = m_list->shareAt(row);
    beginRemoveRows(QModelIndex(),row,row);
    m_selfMutation=true;
    // Erase *without* the list announcing it (takeErased records the undo step
    // only): the announcement runs the editors, and a merge there erases further
    // tags. That is what corrupted the persistent indexes; notifyErased() below
    // runs the editors once this removal is complete, and any nested request is
    // queued (see m_mutationDepth).
    m_list->takeErased(row);
    m_selfMutation=false;
    endRemoveRows();
    if(tag)
        m_list->notifyErased(tag,row,m_list->isTagsLoaded);
    return true;
}

bool TagListModel::eraseRows(int row,int count)
{
    if(count<=0)
        return false;
    // From the back, so the remaining indices stay valid.
    for(int i=row+count-1;i>=row;--i)
        if(!eraseTag(i))
            return false;
    return true;
}

bool TagListModel::removeRows(int row,int count,const QModelIndex& parent)
{
    if(parent.isValid()||!m_list||count<=0)
        return false;
    if(row<0||row+count>int(m_list->size()))
        return false;
    return eraseRows(row,count);
}

bool TagListModel::moveTag(int from,int to)
{
    if(!m_list)
        return false;
    const int size = int(m_list->size());
    if(from<0||from>=size||to<0||to>=size||from==to)
        return false;
    // `to` is the position the row ends up at; Qt wants the index *before which*
    // the row is inserted, expressed in the list as it is before the move.
    return moveRows(QModelIndex(),from,1,QModelIndex(),to>from?to+1:to);
}

bool TagListModel::moveRows(const QModelIndex& sourceParent,int sourceRow,int count,
                              const QModelIndex& destinationParent,int destinationRow)
{
    if(sourceParent.isValid()||destinationParent.isValid()||!m_list||count<=0)
        return false;
    const int size = int(m_list->size());
    if(sourceRow<0||destinationRow<0||sourceRow+count>size||destinationRow>size)
        return false;
    // Dropping a block onto itself (or right after itself) changes nothing.
    if(destinationRow>=sourceRow&&destinationRow<=sourceRow+count)
        return false;

    // Insertion index in the list the block has been taken out of.
    const int insertAt = destinationRow>sourceRow?destinationRow-count:destinationRow;
    if(!beginMoveRows(sourceParent,sourceRow,sourceRow+count-1,destinationParent,destinationRow))
        return false;
    m_selfMutation=true;
    m_list->reorderBlock(sourceRow,count,insertAt);
    m_selfMutation=false;
    endMoveRows();
    return true;
}

bool TagListModel::setTagText(int row,const QString& text)
{
    if(!m_list||row<0||row>=int(m_list->size()))
        return false;
    teTag* tag = m_list->at(row);
    if(!tag)
        return false;
    if(static_cast<QString>(*tag)==text)
        return true;                // nothing to do, and nothing to record for undo
    // teTagList::edit() emits tagEdited (turned into a dataChanged by
    // onListEdited) and, when the rename merged into an existing tag, tagErased
    // (turned into a model reset). Both are handled by the slots, so there is
    // deliberately nothing to announce here.
    m_list->edit(row,text,1,true);
    return true;
}

bool TagListModel::moveTags(const QVector<int>& rows,int destination)
{
    if(!m_list||rows.isEmpty())
        return false;
    QVector<int> valid;
    valid.reserve(rows.size());
    for(int row:rows)
        if(row>=0&&row<int(m_list->size()))
            valid.append(row);
    if(valid.isEmpty())
        return false;
    std::sort(valid.begin(),valid.end());
    valid.erase(std::unique(valid.begin(),valid.end()),valid.end());

    // Our own announcements: the tagMoved() signals below must not be mistaken
    // for somebody changing the list behind the model's back.
    const bool wasSelfMutation=m_selfMutation;
    m_selfMutation=true;

    // Take the rows out from the back, so the remaining indices stay valid.
    QVector<std::shared_ptr<teTag>> moved;
    moved.reserve(valid.size());
    for(int i=int(valid.size())-1;i>=0;--i){
        const int row=valid[i];
        beginRemoveRows(QModelIndex(),row,row);
        moved.prepend(m_list->takeTagOut(row));
        endRemoveRows();
    }
    // `destination` was given in the list as it was before the removals.
    int insertAt=destination;
    for(int row:valid)
        if(row<destination)
            --insertAt;
    insertAt=std::clamp(insertAt,0,int(m_list->size()));

    beginInsertRows(QModelIndex(),insertAt,insertAt+int(moved.size())-1);
    for(int i=0;i<int(moved.size());++i)
        m_list->insertTagIn(insertAt+i,moved[i]);
    endInsertRows();

    // One undo record per moved tag, like the widget based drop used to get from
    // teTagList::move().
    for(int i=0;i<int(moved.size());++i)
        m_list->onTagMoved(moved[i],valid[i],insertAt+i);

    m_selfMutation=wasSelfMutation;
    return true;
}

QStringList TagListModel::mimeTypes() const
{
    return {QStringLiteral("application/x-taglistrow")};
}

QMimeData* TagListModel::mimeData(const QModelIndexList& indexes) const
{
    QByteArray rows;
    for(const QModelIndex& index:indexes){
        if(!index.isValid()||index.column()!=0)
            continue;
        rows += QByteArray::number(index.row());
        rows += ';';
    }
    if(rows.isEmpty())
        return nullptr;
    auto* data = new QMimeData;
    data->setData(mimeTypes().first(),rows);
    data->setText(QString::fromUtf8(rows));
    return data;
}

void TagListModel::notifyExternalReorder(){
    if(!m_list)
        return;
    if(m_mutationDepth>0){
        // Reordering (or announcing a reset) inside somebody else's structural
        // change would corrupt the view's bookkeeping just the same.
        m_pendingReset=true;
        return;
    }
    // No signal tells us *how* the order changed, so announce a layout change;
    // it keeps the persistent indexes valid, unlike a reset.
    emit layoutAboutToBeChanged();
    emit layoutChanged();
}

void TagListModel::flushPendingMutations()
{
    // Runs at depth 0, so a queued change is a normal one again. It may queue
    // more work (a merge erases a tag, the editors react by merging again), so
    // loop until quiet - with a bound, because a misbehaving editor must not hang
    // the UI thread.
    for(int round=0;round<32;++round){
        if(m_pendingReset){
            m_pendingReset=false;
            beginResetModel();
            endResetModel();
        }
        if(m_pendingMutations.isEmpty())
            return;
        const QVector<std::function<void()>> batch = m_pendingMutations;
        m_pendingMutations.clear();
        for(const std::function<void()>& call:batch)
            call();
    }
    telog("[TagListModel] pending structural changes did not settle (32 rounds)");
}

void TagListModel::onListInserted(std::shared_ptr<teTag> tag)
{
    Q_UNUSED(tag);
    if(m_selfMutation)
        return;
    if(m_mutationDepth>0){
        // An editor inserted a tag while another change is being announced: a
        // reset here would corrupt that change. Do it once we are consistent.
        m_pendingReset=true;
        return;
    }
    // Somebody inserted behind our back (editing a multi selection inserts the
    // tag into every selected image). The row already exists, so a proper
    // beginInsertRows() is not possible any more; a reset is the honest answer
    // and the view restores its selection by core afterwards.
    beginResetModel();
    endResetModel();
}

void TagListModel::onListErased(std::shared_ptr<teTag> tag)
{
    Q_UNUSED(tag);
    if(m_selfMutation)
        return;
    if(m_mutationDepth>0){
        m_pendingReset=true;
        return;
    }
    beginResetModel();
    endResetModel();
}

void TagListModel::onListEdited(std::shared_ptr<teTag> tag)
{
    if(m_selfMutation)
        return;
    const int row = rowOf(tag.get());
    if(row<0)
        return;
    const QModelIndex idx = index(row,0);
    emit dataChanged(idx,idx,{Qt::DisplayRole,Qt::EditRole});
}

// ------------------------------------------------------------------- TagDelegate

TagDelegate::TagDelegate(QWidget* parent)
{
    m_suggestionBox=new QListWidget;
    m_suggestionBox->setWindowFlags(Qt::FramelessWindowHint|Qt::Tool);
    m_suggestionBox->hide();
    // No parent on purpose: the view reparents the editor into its viewport when
    // it is first handed out (createEditor()), and the QPointers above make the
    // ownership transfer safe whichever of the two dies first.
    m_editor=new SuggestionEdit(m_suggestionBox,nullptr);
    m_editor->hide();
    connect(m_editor,&QLineEdit::editingFinished,this,[this]{
        emit commitData(m_editor);
        if(m_editor)
            m_editor->stop();
    });
    m_editor->installEventFilter(this);
}

TagDelegate::~TagDelegate()
{
    // Deleting the editor deletes the suggestion box with it.
    if(m_editor)
        delete m_editor.data();
    else if(m_suggestionBox)
        delete m_suggestionBox.data();
}

QSize TagDelegate::sizeHint(const QStyleOptionViewItem& option,const QModelIndex& index) const
{
    Q_UNUSED(option);
    if(!index.isValid())
        return QSize(200,m_rowHeight);
    return QSize(200,m_rowHeight);
}

void TagDelegate::paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index) const
{
    if(!index.isValid())
        return;
    const QString text = index.data(Qt::DisplayRole).toString();
    if(text.isEmpty())
        return;

    const bool selected = option.state&QStyle::State_Selected;
    const bool hovered  = option.state&QStyle::State_MouseOver;
    const bool current  = selected&&(option.state&QStyle::State_HasFocus);
    const TagColors colors = colorsFor(selected,current,hovered);

    auto* core = index.data(TagListModel::TagCoreRole).value<teTag*>();
    const TagVisual visual = core ? TagMarkRegistry::instance().resolve(*core)
                                  : TagVisual();
    QFont font(appFontFamily(),15);
    const QFontMetrics metrics(font);
    const TagRowLayout layout = layoutTagRow(option.rect,visual,metrics);

    painter->save();
    const QRect rect = option.rect;

    // Fill: the mark's colour / stripes / gradient when there is one. Selection
    // never fills the row - it only changes the border below, so a selected tag
    // and a coloured tag cannot be mistaken for each other.
    if(visual.streaked&&!visual.gradientStops.isEmpty()){
        const int streaks=visual.gradientStops.size();
        int cells=12;
        if(streaks+2>cells)
            cells=streaks+2+(streaks&1);        // keep the count even
        const double width=double(rect.width())/double(cells);
        if(visual.hasBase){
            QColor base=visual.baseColor;
            if(visual.whiteDominant)
                painter->fillRect(rect,QColor(0xf2,0xf2,0xf2));
            else{
                base.setAlpha(70);
                painter->fillRect(rect,base);
            }
        }else{
            painter->fillRect(rect,QColor(0,0,0));
        }
        for(int i=0;i<streaks;++i){
            QColor color=visual.gradientStops[i];
            color.setAlpha(visual.whiteDominant?70:120);
            painter->fillRect(QRectF(rect.left()+(2+i)*width,rect.top(),
                                     width+1.0,rect.height()),color);
        }
    }else if(visual.striped&&!visual.gradientStops.isEmpty()){
        const int colors=visual.gradientStops.size();
        const int cells=std::max(12,colors);
        const double width=double(rect.width())/double(cells);
        for(int i=0;i<cells;++i){
            QColor band=visual.gradientStops[i%colors];
            band.setAlpha(visual.whiteDominant?90:90);
            painter->fillRect(QRectF(rect.left()+i*width,rect.top(),
                                     width+1.0,rect.height()),band);
        }
    }else if(visual.gradient&&!visual.gradientStops.isEmpty()){
        QLinearGradient gradient(rect.topLeft(),rect.topRight());
        for(int i=0;i<visual.gradientStops.size();++i){
            QColor color=visual.gradientStops[i];
            color.setAlpha(48);
            gradient.setColorAt(visual.gradientStops.size()>1
                                    ?double(i)/double(visual.gradientStops.size()-1):0.0,
                                color);
        }
        painter->setBrush(gradient);
        painter->setPen(Qt::NoPen);
        painter->drawRect(rect);
    }else if(visual.hasColor){
        painter->setBrush(visual.fill);
        painter->setPen(Qt::NoPen);
        painter->drawRect(rect);
    }else{
        painter->setBrush(QColor(0,0,0));
        painter->setPen(Qt::NoPen);
        painter->drawRect(rect);
    }

    // Chips (MARK-05): the same layoutTagRow() the hit test uses.
    painter->setFont(font);
    for(const TagChip& chip:layout.chips){
        // No button frame around a decorative icon: the emoji/PNG is drawn as
        // itself. The chip rect is only the hit area (MARK-05).
        const TagIconSpec spec{chip.icon,QString()};
        if(spec.isImage()){
            const QString path=QFileInfo(spec.value).isAbsolute()
                                   ?spec.value
                                   :QCoreApplication::applicationDirPath()
                                        +QStringLiteral("/data/icons/")+spec.value;
            QPixmap pixmap(path);
            if(!pixmap.isNull()){
                const QPixmap scaled=pixmap.scaled(chip.iconPx,chip.iconPx,Qt::KeepAspectRatio,
                                                   Qt::SmoothTransformation);
                const QRect target(chip.rect.center().x()-scaled.width()/2,
                                   chip.rect.center().y()-scaled.height()/2,
                                   scaled.width(),scaled.height());
                painter->drawPixmap(target,scaled);
            }else{
                painter->setPen(Qt::white);
                painter->drawText(chip.rect,Qt::AlignCenter,spec.value);
            }
        }else{
            QFont iconFont=font;
            iconFont.setPixelSize(chip.iconPx);
            painter->setFont(iconFont);
            painter->setPen(Qt::white);
            painter->drawText(chip.rect,Qt::AlignCenter,spec.value);
            painter->setFont(font);
        }
    }

    // Border: selection only changes this border (no green fill). A coloured tag
    // keeps its own colour while unselected; white-dominant tags get a white
    // border so the black text has a frame to sit in.
    painter->setBrush(Qt::NoBrush);
    if(current){
        painter->setPen(QPen(QColor(0x00,0xd9,0x6d),1));
    }else if(selected){
        painter->setPen(QPen(QColor(0x2a,0x6b,0x45),1));
    }else if(visual.whiteDominant){
        painter->setPen(QPen(QColor(0xf2,0xf2,0xf2),1));
    }else if(visual.gradient&&visual.gradientStops.size()>=2){
        QLinearGradient gradient(rect.topLeft(),rect.topRight());
        for(int i=0;i<visual.gradientStops.size();++i)
            gradient.setColorAt(double(i)/double(visual.gradientStops.size()-1),
                                tagBorderColor(visual.gradientStops[i],255));
        painter->setPen(QPen(QBrush(gradient),1));
    }else if(visual.streaked||visual.striped){
        const QColor base=visual.hasBase?visual.baseColor
                                        :visual.gradientStops.value(0);
        painter->setPen(QPen(tagBorderColor(base),1));
    }else if(visual.hasColor){
        painter->setPen(QPen(tagBorderColor(visual.border),1));
    }else{
        painter->setPen(QPen(hovered?colors.hoverBorder:colors.border,1));
    }
    painter->drawRect(rect.adjusted(1,1,-1,-1));

    // The text is hidden only while the inline editor is really on screen for
    // this row. Clearing m_editingRow when the editor closes is not enough on its
    // own: cancelling with Escape used to leave the row blank until the next row
    // was edited (and it stayed blank even after switching images).
    if(!(m_activeEditor&&m_activeEditor->isVisible()&&m_editingRow==index.row())){
        painter->setFont(font);
        painter->setPen(selected?QColor(0x00,0xd9,0x6d)
                             :(visual.whiteDominant?QColor(0,0,0):Qt::white));
        const QString elided=metrics.elidedText(text,Qt::ElideMiddle,
                                                std::max(0,layout.textRect.width()));
        painter->drawText(layout.textRect,Qt::AlignVCenter|Qt::AlignLeft,elided);
    }
    painter->restore();
}

bool TagDelegate::editorEvent(QEvent* event,QAbstractItemModel* model,
                             const QStyleOptionViewItem& option,const QModelIndex& index)
{
    const QEvent::Type type=event->type();
    if(type!=QEvent::MouseMove&&type!=QEvent::MouseButtonPress
        &&type!=QEvent::MouseButtonRelease&&type!=QEvent::MouseButtonDblClick
        &&type!=QEvent::Leave)
        return false;

    auto* view=qobject_cast<QAbstractItemView*>(parent());
    auto updateRow=[view,model](int row){
        if(view&&model&&row>=0)
            view->update(model->index(row,0));
    };
    auto layoutFor=[&option,&index](TagRowLayout& out)->bool{
        const auto* core=index.data(TagListModel::TagCoreRole).value<teTag*>();
        if(!core)
            return false;
        const QFont font(appFontFamily(),15);
        out=layoutTagRow(option.rect,TagMarkRegistry::instance().resolve(*core),
                         QFontMetrics(font));
        return true;
    };
    auto chipAt=[&layoutFor,&option](const QPoint& pos)->QString{
        TagRowLayout layout;
        if(!layoutFor(layout)||!option.rect.contains(pos))
            return QString();
        for(const TagChip& chip:layout.chips)
            if(chip.rect.contains(pos))
                return chip.id;
        return QString();
    };
    auto clearHover=[&]{
        if(m_hoverRow<0&&m_hoverChip.isEmpty())
            return;
        const int oldRow=m_hoverRow;
        m_hoverRow=-1;
        m_hoverChip.clear();
        updateRow(oldRow);
    };

    if(type==QEvent::Leave){
        clearHover();
        return false;
    }

    const QPoint pos=static_cast<QMouseEvent*>(event)->position().toPoint();
    if(type==QEvent::MouseMove){
        if(!index.isValid()){
            clearHover();
            return false;
        }
        const QString chip=chipAt(pos);
        if(m_hoverRow!=index.row()||m_hoverChip!=chip){
            const int oldRow=m_hoverRow;
            m_hoverRow=index.row();
            m_hoverChip=chip;
            updateRow(oldRow);
            updateRow(m_hoverRow);
        }
        return false;                       // hovering still belongs to the view
    }

    if(!index.isValid())
        return false;

    if(type==QEvent::MouseButtonPress){
        if(!(static_cast<QMouseEvent*>(event)->button()&Qt::LeftButton))
            return false;
        const QString chip=chipAt(pos);
        if(chip.isEmpty())
            return false;
        m_pressedRow=index.row();
        m_pressedChip=chip;
        updateRow(m_pressedRow);
        return true;                        // do not select / start a drag
    }

    if(type==QEvent::MouseButtonDblClick)
        return !chipAt(pos).isEmpty();      // a chip double click is not an edit

    // MouseButtonRelease: a press that started on a chip is swallowed even when
    // the pointer moved away, so the row does not select on release.
    if(m_pressedChip.isEmpty())
        return false;
    const int pressedRow=m_pressedRow;
    const QString pressedChip=m_pressedChip;
    m_pressedRow=-1;
    m_pressedChip.clear();
    updateRow(pressedRow);
    updateRow(index.row());
    if(index.row()==pressedRow&&chipAt(pos)==pressedChip){
        auto* core=index.data(TagListModel::TagCoreRole).value<teTag*>();
        if(core)
            emit chipActivated(core,pressedChip);
    }
    return true;
}

QWidget* TagDelegate::createEditor(QWidget* parent,const QStyleOptionViewItem& option,const QModelIndex& index) const
{
    Q_UNUSED(option);
    if(!index.isValid()||!m_editor)
        return nullptr;
    // The view hands its viewport in and expects the editor to live there: the
    // geometry it applies later is in viewport coordinates.
    if(parent&&m_editor->parent()!=parent)
        m_editor->setParent(parent);
    m_editingRow=index.row();
    m_activeEditor=m_editor;
    m_editor->start(index.data(Qt::EditRole).toString());
    return m_editor;
}

void TagDelegate::setEditorData(QWidget* editor,const QModelIndex& index) const
{
    auto* lineEdit = qobject_cast<QLineEdit*>(editor);
    if(!lineEdit)
        return;
    lineEdit->setText(index.data(Qt::EditRole).toString());
}

void TagDelegate::setModelData(QWidget* editor,QAbstractItemModel* model,const QModelIndex& index) const
{
    auto* lineEdit = qobject_cast<QLineEdit*>(editor);
    if(!lineEdit||!model)
        return;
    m_editingRow=-1;
    model->setData(index,lineEdit->text(),Qt::EditRole);
    // The row is painted again after the editor is really gone. While it is
    // visible paint() skips this row's text (the editor shows it instead), and
    // hiding the editor does not schedule a repaint on its own - the row kept
    // showing the text from before the edit.
    const QPersistentModelIndex persistent(index);
    QTimer::singleShot(0,const_cast<TagDelegate*>(this),[this,persistent]{
        if(!persistent.isValid())
            return;
        if(auto* view=qobject_cast<QAbstractItemView*>(parent()))
            view->update(QModelIndex(persistent));
    });
    emit const_cast<TagDelegate*>(this)->closeEditor(editor,QAbstractItemDelegate::NoHint);
}

void TagDelegate::updateEditorGeometry(QWidget* editor,const QStyleOptionViewItem& option,const QModelIndex& index) const
{
    Q_UNUSED(index);
    if(!editor)
        return;
    editor->setGeometry(option.rect.adjusted(3,2,-2,-2));
    if(auto* lineEdit = qobject_cast<SuggestionEdit*>(editor))
        lineEdit->moveSuggestionBox();
}

bool TagDelegate::eventFilter(QObject* watched,QEvent* event)
{
    auto* lineEdit = dynamic_cast<SuggestionEdit*>(watched);
    if(!lineEdit)
        return QStyledItemDelegate::eventFilter(watched,event);

    if(event->type()==QEvent::FocusOut){
        auto* focusEvent = static_cast<QFocusEvent*>(event);
        // Clicking the suggestion box must not close the editor.
        if(focusEvent->reason()==Qt::FocusReason::OtherFocusReason
           ||focusEvent->reason()==Qt::FocusReason::ActiveWindowFocusReason){
            lineEdit->setFocus();
            return true;
        }
        emit lineEdit->editingFinished();
        return true;
    }
    if(event->type()==QEvent::KeyPress){
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if(keyEvent->key()==Qt::Key_Tab){
            if(m_suggestionBox&&m_suggestionBox->currentItem())
                lineEdit->setText(m_suggestionBox->currentItem()->data(Qt::UserRole).toString());
            m_suggestionBox->hide();
            return true;
        }
    }
    return QStyledItemDelegate::eventFilter(watched,event);
}
