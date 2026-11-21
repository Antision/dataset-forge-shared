#include "taglistview.h"
#include "tag.h"
#include "taglistmodel.h"
#include "tagmarks.h"
#include "fonts.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDrag>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTimer>

size_t TagListView::size() const{
    if(showing_list)
        return showing_list->size();
    return 0;
}

std::shared_ptr<teTag> TagListView::currentCore() const
{
    if(!this)
        return nullptr;
    const QModelIndex index = this->currentIndex();
    if(!index.isValid())
        return nullptr;
    teTag* raw = index.data(TagListModel::TagCoreRole).value<teTag*>();
    if(!raw)
        return nullptr;
    // Recover the owning shared_ptr through the tag list itself, which is the
    // only place that owns these tags.
    if(showing_list){
        for(const std::shared_ptr<teTag>& core:*showing_list)
            if(core.get()==raw)
                return core;
    }
    return nullptr;
}

void TagListView::scrollToTop()
{
    if(this&&this->verticalScrollBar())
        this->verticalScrollBar()->setValue(0);
}

void TagListView::selectAllRows()
{
    if(this&&m_model&&m_model->rowCount()>0)
        this->selectAll();
}

// --------------------------------------------------------------- selection

void TagListView::setSelectCurrentCore(std::shared_ptr<teTag> core,bool ifclear)
{
    if(!m_model||!this)
        return;
    if(!core){
        // Same as setSelectCurrent(nullptr) in the widget based list: clear.
        // setCurrentIndex(invalid) is a no-op in Qt, so the current row is
        // cleared through the selection model.
        if(QItemSelectionModel* selection = this->selectionModel()){
            selection->clearCurrentIndex();
            selection->clearSelection();
        }
        return;
    }
    const int row = m_model->rowOf(core.get());
    if(row<0)
        return;                         // not in this list: leave the selection alone
    const QModelIndex index = m_model->index(row,0);
    if(ifclear)
        this->clearSelection();
    this->setCurrentIndex(index);
    this->selectionModel()->select(index,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);
    this->scrollTo(index,QAbstractItemView::EnsureVisible);
}

void TagListView::setSelectCore(std::shared_ptr<teTag> core)
{
    if(!m_model||!this)
        return;
    const int row = m_model->rowOf(core.get());
    if(row<0)
        return;
    this->selectionModel()->select(m_model->index(row,0),
                                     QItemSelectionModel::Select|QItemSelectionModel::Rows);
}

int TagListView::setUnselectCore(std::shared_ptr<teTag> core)
{
    if(!m_model||!this)
        return 0;
    if(!core){
        const int count = int(this->selectionModel()->selectedRows().size());
        this->clearSelection();
        return count;
    }
    const int row = m_model->rowOf(core.get());
    if(row<0)
        return 0;
    this->selectionModel()->select(m_model->index(row,0),
                                     QItemSelectionModel::Deselect|QItemSelectionModel::Rows);
    return 1;
}

int TagListView::setSelectRangeCore(std::shared_ptr<teTag> core,bool ifclear)
{
    if(!m_model||!this)
        return 0;
    const int row = m_model->rowOf(core.get());
    const QModelIndex current = this->currentIndex();
    if(row<0||!current.isValid())
        return 0;
    if(ifclear)
        this->clearSelection();
    // Same as the widget based list: select from the current tag to this one.
    const int first = std::min(current.row(),row);
    const int last  = std::max(current.row(),row);
    QItemSelection selection;
    selection.select(m_model->index(first,0),m_model->index(last,0));
    this->selectionModel()->select(selection,QItemSelectionModel::Select|QItemSelectionModel::Rows);
    return last-first+1;
}

bool TagListView::isCoreSelected(std::shared_ptr<teTag> core) const
{
    if(!m_model||!this||!core)
        return false;
    const int row = m_model->rowOf(core.get());
    if(row<0)
        return false;
    return this->selectionModel()->isSelected(m_model->index(row,0));
}

void TagListView::ensureCoreVisible(std::shared_ptr<teTag> core)
{
    if(!m_model||!this||!core)
        return;
    const int row = m_model->rowOf(core.get());
    if(row<0)
        return;
    this->scrollTo(m_model->index(row,0),QAbstractItemView::EnsureVisible);
}

QPoint TagListView::tagDisplayAnchor(std::shared_ptr<teTag> core) const
{
    if(!m_model||!this||!core)
        return QCursor::pos();
    const int row = m_model->rowOf(core.get());
    if(row<0)
        return QCursor::pos();
    const QRect rect = this->visualRect(m_model->index(row,0));
    if(!rect.isValid()||rect.isEmpty())
        return QCursor::pos();
    // Right edge of the view, at the height of that row.
    return this->viewport()->mapToGlobal(QPoint(this->viewport()->width(),rect.center().y()));
}

QString TagListView::getSelectText(){
    // The clipboard has to carry exactly what saving the caption would write:
    // tags joined with ", ", sentences on their own line, brackets kept glued to
    // their word. Joining the rows with newlines (as this used to do) loses all
    // of that. The widget based list already uses the same serializer.
    QVector<std::shared_ptr<teTag>> cores;
    if(m_model&&this){
        QModelIndexList rows = this->selectionModel()->selectedRows();
        std::sort(rows.begin(),rows.end(),[](const QModelIndex&a,const QModelIndex&b){return a.row()<b.row();});
        if(rows.isEmpty()&&this->currentIndex().isValid())
            rows.append(this->currentIndex());    // nothing selected: the current row
        cores.reserve(rows.size());
        for(const QModelIndex& index:rows){
            if(teTag* raw = index.data(TagListModel::TagCoreRole).value<teTag*>())
                if(showing_list)
                    cores.push_back(showing_list->shareAt(index.row()));
        }
    }
    return serializePieces(cores.size(),[&](int i) -> std::shared_ptr<teTag> {
        return cores[i];
    },true);
}

void TagListView::tagErase(int index){    hideTagDisplay();
    if(!showing_list||!m_model||index<0||index>=int(showing_list->size())){
        telog(QString("[TagListView::tagErase] index %1 out of range").arg(index));
        return;
    }
    // The view keeps the current row on a neighbour of the erased one.
    const int current = this->currentIndex().isValid()?this->currentIndex().row():-1;
    if(!m_model->eraseTag(index))
        return;
    const int rows = m_model->rowCount();
    if(rows>0&&(current==index||current<0)){
        const int next = index<rows?index:rows-1;
        setSelectCurrentCore(showing_list->shareAt(next));
    }
    scrollToTop();
}

void TagListView::tagErase(std::shared_ptr<teTag>tag){
    if(!showing_list||!m_model)
        return;
    const int current = this->currentIndex().isValid()?this->currentIndex().row():-1;
    if(tag==nullptr&&m_model->rowCount()==0)
        return;
    if(tag==nullptr){
        // "erase the selection": the current row plus every selected row.
        QModelIndexList selected = this->selectionModel()->selectedRows();
        if(selected.isEmpty()&&current>=0)
            selected.append(m_model->index(current,0));
        QVector<int> indexes;
        indexes.reserve(selected.size());
        for(const QModelIndex& index:selected)
            indexes.append(index.row());
        std::sort(indexes.begin(),indexes.end(),std::greater<int>());
        QStringList erased;
        for(int row:indexes){
            if(row>=0&&row<int(showing_list->size())){
                erased.append(static_cast<QString>(*showing_list->shareAt(row)));
                m_model->eraseTag(row);
            }
        }
        if(!erased.isEmpty())
            telog(QString("[teTagListWidget] erased %1 tag(s): %2")
                      .arg(erased.size()).arg(erased.join(QStringLiteral(", "))));
        const int left = m_model->rowCount();
        if(left>0)
            setSelectCurrentCore(showing_list->shareAt(std::clamp(current<0?0:current-1,0,left-1)));
        scrollToTop();
        return;
    }
    const int pos = showing_list->find(tag);
    if(pos<0){
        telog("[TagListView::tagErase]:tag not exist");
        return;
    }
    if(!m_model->eraseTag(pos))
        return;
    const int left = m_model->rowCount();
    if(left>0&&pos==current)
        setSelectCurrentCore(showing_list->shareAt(pos<left?pos:left-1));
    scrollToTop();
}


void TagListView::retire(std::shared_ptr<teTag> tag)
{
    // The tag list owns the tags, so it owns retirement; this only forwards.
    if(showing_list)
        showing_list->retire(tag);
}

void TagListView::tagEdit(std::shared_ptr<teTag>tag, QString text,int removeDuplicate,bool ifemit){
    if(!showing_list)
        return;
    if(showing_list->edit(tag,text,removeDuplicate,ifemit)==-1&&removeDuplicate==1)
        tagErase(tag);
}

void TagListView::startEditingRow(int row,int wordStart,int wordLength)
{
    if(!m_model||row<0||row>=m_model->rowCount())
        return;
    const QModelIndex index = m_model->index(row,0);
    auto* raw = index.data(TagListModel::TagCoreRole).value<teTag*>();
    if(raw&&raw->type==teTag::sentence){
        // Sentences are edited in the program's long-text window; the view only
        // knows the tag core, the host owns the window.
        hideTagDisplay();
        if(m_viewHost&&showing_list)
            if(std::shared_ptr<teTag> core=showing_list->shareAt(row))
                m_viewHost->editLongText(this,core);
        return;
    }
    // The editor opens with a selection: the word that was double-clicked (plus
    // its trailing space) when there was one, the whole tag otherwise. A caller
    // with no word (F2, insert) must not overwrite a range edit() already worked
    // out for this very double click - it runs in the same gesture, and the mouse
    // position is gone by then.
    if(m_delegate&&wordStart>=0)
        m_delegate->setPendingWordSelection(wordStart,wordLength);
    clearHoverWord();
    this->setCurrentIndex(index);
    // Base class on purpose: this path already decided the range, and going
    // through the reimplemented edit() again would let it decide a second time -
    // with no mouse position left, it would overwrite the range with "everything".
    QListView::edit(index,AllEditTriggers,nullptr);
}

void TagListView::tagEditCore(std::shared_ptr<teTag> core)
{
    hideTagDisplay();
    if(!core||!m_model)
        return;
    const int row = m_model->rowOf(core.get());
    if(row>=0)
        startEditingRow(row);
}

void TagListView::tagInsertAbove(bool edit,std::shared_ptr<teTag>newtag,int removeDuplicate){
    if(!showing_list||!m_model)
        return;
    if(newtag==nullptr){
        newtag=std::make_shared<teTag>();
        removeDuplicate=2;
    }
    const int current = this->currentIndex().isValid()?this->currentIndex().row():-1;
    const int row = current<0?0:current;
    if(m_model->insertTag(row,newtag,removeDuplicate)<0)
        return;
    // The editors may have merged the new tag into an existing one (they run from
    // the insertion's announcement), so find where it is now instead of trusting
    // the row we asked for.
    const int inserted = m_model->rowOf(newtag.get());
    if(inserted<0)
        return;                                 // merged away: nothing to select
    setSelectCurrentCore(newtag);
    if(edit)
        startEditingRow(inserted);
}

void TagListView::tagInsertBelow(bool edit,std::shared_ptr<teTag>newtag,int removeDuplicate){
    if(!showing_list||!m_model)
        return;
    if(newtag==nullptr){
        newtag=std::make_shared<teTag>();
        removeDuplicate=2;
    }
    const int current = this->currentIndex().isValid()?this->currentIndex().row():-1;
    const int row = current<0?m_model->rowCount():current+1;
    if(m_model->insertTag(row,newtag,removeDuplicate)<0)
        return;
    const int inserted = m_model->rowOf(newtag.get());
    if(inserted<0)
        return;                                 // merged away: nothing to select
    setSelectCurrentCore(newtag);
    if(edit)
        startEditingRow(inserted);
}

void TagListView::keyPressEvent(QKeyEvent *event) {
    if(!showing_list) return;
    if(handleTagDisplayEscape(event))
        return;
    if (event->matches(QKeySequence::SelectAll)) {
        selectAllRows();
    } else if ((event->key() == Qt::Key_W && event->modifiers() == Qt::ControlModifier)||event->key() == Qt::Key_F4) {
        tagInsertAbove();
    } else if ((event->key() == Qt::Key_D && event->modifiers() == Qt::ControlModifier)||event->key() == Qt::Key_Delete) {
        tagErase();
        if(this)
            this->setFocus();
    } else if (event->key() == Qt::Key_Enter||event->key() == Qt::Key_Return) {
        tagEditCore(currentCore());
    } else if ((event->key() == Qt::Key_E && event->modifiers() == Qt::ControlModifier)||event->key() == Qt::Key_F2) {
        tagEditCore(currentCore());
    } else if (event->key() == Qt::Key_X && event->modifiers() == Qt::ControlModifier) {
        cut();
    } else if (event->key() == Qt::Key_C && event->modifiers() == Qt::ControlModifier) {
        copy();
    } else if (event->key() == Qt::Key_V && event->modifiers() == Qt::ControlModifier) {
        paste();
    } else if (TypeSearch::isSearchKey(event)) {
        // A plain printable key searches the list: typing "solo" walks the rows
        // to the tag that starts with it. Qt's own search cannot be reached from
        // here (this override never calls the base class), so it is done by hand.
        typeSearch(event->text());
        event->accept();
    } else {
        event->ignore();        // the view handles everything else
    }
}

void TagListView::typeSearch(const QString &text){
    TagListModel* list = tagModel();
    if(!list)
        return;
    typeSearchState.search(this,list->rowCount(),currentIndex().row(),[list](int row){
        teTag* tag = list->tagAt(row);
        return tag?static_cast<QString>(*tag):QString();
    },text);
}

void TagListView::onCaptionListGone(teTagList *list){
    if(!list||list!=showing_list)
        return;
    // The list the view was showing is dying with its picture: clear() drops
    // every pointer before it becomes dangling and tells the editors to let go.
    clear();
}

void TagListView::refreshDecorations()
{
    if(!showing_list)
        return;
    for(const std::shared_ptr<teTag>& tag:showing_list->getTags())
        refreshTagDecoration(tag);
    // The editors keep their own widgets; they recompute and re-style their tags.
    if(m_editorHost)
        m_editorHost->refreshDecorations();
    if(m_model)
        m_model->refreshAll();
}

void TagListView::load(teTagList*newlist){
    hideTagDisplay();
    // Half typed word of the previous list: a new list must not be searched with
    // the old list's prefix (switching pictures by mouse and typing again within
    // the keyboard input interval would otherwise match the wrong rows).
    typeSearchState.reset();
    showing_list=newlist;
    if(!showing_list)
        return;
    // No teTagList::load() here any more: that is what built one widget per tag
    // Neither the tags nor their words have a widget table any more: widgets
    // belong to the list that shows them (teTagListWidgetBase keeps the table) and
    // this list shows rows, so there is nothing to flag or unload here.
    const int tmpSelectIndex=m_pendingSelectionIndex;
    m_model->setTagList(showing_list);
    // The editors read the list here, before the selection is restored: reading
    // may merge tags, which rebuilds rows, and a row index chosen before that
    // would point at whatever moved into its place.
    if(m_editorHost){
        m_editorHost->readAll(showing_list);
    }else{
        // Program without editors (gallery_info): the same rule engine, applied
        // once on load. tqe has the decoration control, so it never comes here.
        for(const std::shared_ptr<teTag>& tag:showing_list->getTags()){
            tag->decoration=TagMarkRegistry::instance().decorate(*tag);
            tag->decorationSet=!tag->decoration.isEmpty();
        }
    }

    if(tmpSelectIndex>-1&&m_model->rowCount()>0){
        const int row = std::min(tmpSelectIndex,m_model->rowCount()-1);
        setSelectCurrentCore(showing_list->shareAt(row));
    }
}

void TagListView::setCaptionSource(CaptionSource *source)
{
    m_captionSource=source;
    if(m_captionSource)
        // The source may outlive this view (the window keeps it until the last
        // picture is gone), so the handler must not reach a destroyed view. A
        // QPointer turns that into a null check instead of a crash.
        m_captionSource->setListGoneHandler([view=QPointer<TagListView>(this)](teTagList* list){
            if(view)
                view->onCaptionListGone(list);
        });
}

void TagListView::loadCaption(const QString &imagePath)
{
    if(imagePath.isEmpty()){
        closeCaption();
        return;
    }
    if(imagePath==m_imagePath&&showing_list)
        return;                     // already showing this image
    clear();
    m_imagePath=imagePath;
    if(!m_captionSource)
        return;
    if(teTagList* list=m_captionSource->open(imagePath))
        load(list);
}

void TagListView::closeCaption()
{
    clear();            // also forgets the image path
}

void TagListView::clear(teTagList *in){
    hideTagDisplay();
    // Editing state of the view's inline editor belongs to the old list: end it,
    // otherwise its suggestion box outlives the picture (the editor is hidden by
    // Qt through a QWidget*, which used to leave the box on screen).
    if(m_delegate)
        m_delegate->stopEditing();
    if(in==showing_list||in==nullptr){
        m_imagePath.clear();
        if(m_model)
            m_model->setTagList(nullptr);
        if(showing_list){
            // No flag to clear any more: widgets belong to the list that shows
            // them, and this one never had any.
            disconnect(showing_list,0,this,0);
        }
        showing_list=nullptr;
        m_currentCoreBeforeReset.reset();
        // The editors hold the list they last read (their own connectedList) and
        // touch it on the next read. Clearing the view is the moment they have to
        // let go: the picture may delete its list right after this, and the next
        // unloadList() would disconnect from freed memory.
        if(m_editorHost)
            m_editorHost->clearAll();
    }
}

teTagWidgetBase* TagListView::taginsert(int index, std::shared_ptr<teTag>in_tag,int removeDuplicate,bool select){
    if(!showing_list||!m_model)
        return nullptr;
    while(index<0)
        index+=int(showing_list->size())+1;
    const int row = m_model->insertTag(index,in_tag,removeDuplicate);
    if(row<0){
        // The tag was never part of the list, so nothing may be recorded on the
        // undo stack for it. Rejected tags are destroyed like before, which is
        // what tells the editors that linked to it to let go.
        if(in_tag)
            in_tag->teemit(teCallbackType::destroy);
        return nullptr;
    }
    if(select)
        setSelectCurrentCore(in_tag);
    return nullptr;                     // no widget per tag any more
}
namespace {
/// While a snapshot is being applied the editors must not merge anything: the
/// snapshot is a state the user had, and re-merging it would undo the undo (a
/// merge that is undone would immediately happen again on the re-read). The
/// switch lives in the editor host now - tqe keeps it as a global, a program
/// without editors does not have one at all.
class SnapshotGuard{
public:
    explicit SnapshotGuard(TagEditorHost* host)
        :m_host(host),m_saved(host?host->mergeEnabled():true)
    {
        if(m_host)
            m_host->setMergeEnabled(false);
    }
    ~SnapshotGuard(){
        if(m_host)
            m_host->setMergeEnabled(m_saved);
    }
    SnapshotGuard(const SnapshotGuard&)=delete;
    SnapshotGuard& operator=(const SnapshotGuard&)=delete;
private:
    TagEditorHost* m_host;
    bool m_saved;
};
}

/**
 * @brief Puts the list content back to `snapshot` and makes everyone re-read.
 *
 * The tags are rebuilt, so this is a heavier operation than the old per-tag
 * undo - and it is also the only one that is correct: a gesture can touch
 * several tags (a merge rewrites one and erases another), and replaying the
 * individual operations left the list half merged whenever one of them no
 * longer applied. Rebuilding also means the editors can simply be asked to read
 * the list again, which is the same path a freshly loaded picture takes.
 */

void TagListView::applySnapshot(const teTagSnapshotList& snapshot)
{
    if(!showing_list||!m_model)
        return;
    // The snapshot is applied through the list's ordinary row operations: the model
    // keeps its row bookkeeping, the editors see the destroy / insert callbacks
    // they always see, and the widget pool hands its widgets back on the way out.
    // Rebuilding the tags behind the model's back (the first attempt) left all
    // three of them looking at tags that were no longer in the list.
    teTagList::ChangeSuppressor suppress(showing_list);     // this is not an edit
    SnapshotGuard guard(m_editorHost);                                    // and it must not merge
    hideTagDisplay();
    if(m_delegate)
        m_delegate->stopEditing();
    // The selection is a property of the view, not of the state being restored:
    // keep it on the same tag when that tag survives the step.
    const std::shared_ptr<teTag> selected=currentCore();

    // Only what differs is touched. A snapshot that differs in one tag - which is
    // what an undo almost always is - costs one erase and one insert, instead of
    // rebuilding a list of sixty.
    const QVector<std::shared_ptr<teTag>> current=showing_list->getTags();
    QVector<bool> kept(current.size(),false);
    QVector<std::shared_ptr<teTag>> target;
    target.reserve(snapshot.size());
    for(const teTagSnapshot& one : snapshot){
        std::shared_ptr<teTag> found;
        for(int i=0;i<current.size();++i){
            if(kept[i]||current[i]->wordTexts()!=one.words)
                continue;
            kept[i]=true;
            found=current[i];
            // The words match, the rest comes from the snapshot (a fake edit clears
            // the merge mark without changing a word).
            found->type=teTag::teTagType(one.type);
            found->weight=one.weight;
            found->mergeSuppressed=one.mergeSuppressed;
            break;
        }
        target.append(found);                   // nullptr: this one has to be built
    }

    // Tags the snapshot does not contain: erase them (from the end, so the row
    // indexes of the ones still to be looked at stay valid).
    for(int i=int(current.size())-1;i>=0;--i){
        if(kept[i])
            continue;
        const int row=showing_list->find(current[i]);
        if(row>=0)
            tagErase(row);
    }
    // Tags the snapshot has and the list does not: build and insert them. Neither
    // the insert nor the erase above may move the selection (an insert selects by
    // default): the one the user had is restored at the end.
    for(int i=0;i<target.size();++i){
        if(target[i])
            continue;
        auto tag=std::make_shared<teTag>();
        for(const QString& word : snapshot[i].words)
            tag->words.append(new teWord{word});
        tag->type=teTag::teTagType(snapshot[i].type);
        tag->weight=snapshot[i].weight;
        tag->mergeSuppressed=snapshot[i].mergeSuppressed;
        target[i]=tag;
        taginsert(int(showing_list->size()),tag,0,false);
    }
    // The set is right; the order may not be. Setting it is one assignment plus a
    // refresh - the editors' own lists keep their own order and are not touched.
    bool ordered=true;
    const QVector<std::shared_ptr<teTag>> now=showing_list->getTags();
    for(int i=0;i<target.size()&&i<now.size();++i){
        if(now[i]!=target[i]){
            ordered=false;
            break;
        }
    }
    if(!ordered){
        showing_list->reorderTo(target);
        m_model->notifyExternalReorder();
    }
    if(selected&&showing_list->find(selected)>=0)
        setSelectCurrentCore(selected);
    // The state the next step starts from is what is on screen now.
    showing_list->syncUndoBaseline();
    showing_list->isSaved=false;
}

bool TagListView::canUndo() const{
    return showing_list&&showing_list->canUndo();
}

bool TagListView::canRedo() const{
    return showing_list&&showing_list->canRedo();
}

void TagListView::undo(){
    if(!showing_list)
        return;
    // The list consumes the step and hands out the state to restore; an open
    // gesture belongs to this undo (see teTagList::takeUndoSnapshot).
    teTagSnapshotList before;
    if(showing_list->takeUndoSnapshot(before))
        applySnapshot(before);
}

void TagListView::redo(){
    if(!showing_list)
        return;
    teTagSnapshotList after;
    if(showing_list->takeRedoSnapshot(after))
        applySnapshot(after);
}

void TagListView::onTagEdited(std::shared_ptr<teTag>tag){
    // Editor controls may turn a tag into a copy of another one (a rename, a
    // colour swap, ...). Merging keeps the list free of duplicates; the old
    // code erased the tag twice here, which could take the index of a removed
    // row.
    if(showing_list&&tag&&showing_list->remove_duplicate(tag,true))
        telog("[TagListView::onTagEdited] merged a duplicated tag");
}

// ---------------------------------------------------------------- the view

TagListView::TagListView(QWidget* parent)
    : QListView(parent)
{
    // The menu first: createModelAndDelegate() connects its actions, and doing
    // that before they exist made every one of those connects a no-op (Qt logged
    // "invalid nullptr parameter"), i.e. the view's context menu was dead.
    createMenu();
    createModelAndDelegate();
    tagDisplayHideTimer = new QTimer(this);
    tagDisplayHideTimer->setSingleShot(true);
    connect(tagDisplayHideTimer,&QTimer::timeout,this,&TagListView::tagDisplayHideTick);
}

TagListView::~TagListView()
{
    clear();
    // The display is a top-level tool window (no QWidget parent), so the view
    // owns it explicitly.
    if(tagDisplay){
        delete tagDisplay->widget();
        tagDisplay=nullptr;
    }
}

void TagListView::setViewHost(TagViewHost* host)
{
    m_viewHost=host;
    if(tagDisplay){
        delete tagDisplay->widget();
        tagDisplay=nullptr;
    }
    if(!m_viewHost)
        return;
    // Magnified popup: it appears when a tag is clicked, not after a hover delay,
    // and stays while the pointer travels into it.
    tagDisplay=m_viewHost->createTagDisplay(this);
    if(tagDisplay)
        tagDisplay->setPointerLeftHandler([this]{ scheduleTagDisplayHide(); });
}

void TagListView::createMenu()
{
    menu = new QMenu(this);
    editAction = menu->addAction(QIcon(":/res/menu_edit.png"),"edit (F2/Ctrl+E)");
    deleteAction = menu->addAction(QIcon(":/res/menu_remove.png"),"delete (Ctrl+D/del)");
    insertAction = menu->addAction(QIcon(":/res/menu_add.png"),"insert above (Ctrl+W)");
    insertBelowAction = menu->addAction(QIcon(":/res/menu_add.png"),"insert below");
    cutAction = menu->addAction(QIcon(":/res/menu_cut.png"),"cut (Ctrl+X)");
    copyAction = menu->addAction(QIcon(":/res/menu_copy.png"),"copy (Ctrl+C)");
    pasteAction = menu->addAction(QIcon(":/res/menu_paste.png"),"paste (Ctrl+V)");
    menu->setStyleSheet(
        R"(QMenu {
        background-color: transparent;
        font-size:14px;
        color:#8dfda7;
        border-radius:3px;
    }
    QMenu::item {
        background-color: rgb(27,29,37);
        padding: 3px;
        border-radius:2px;
        margin: 0px;
        border: 1px solid #8b9ac6;
    }
    QMenu::item:selected {background-color: #3d258f;}
)");
    menu->setAttribute(Qt::WA_TranslucentBackground);
    menu->setWindowFlags(menu->windowFlags() | Qt::FramelessWindowHint);
}

void TagListView::copy()
{
    QApplication::clipboard()->setText(getSelectText());
}

void TagListView::cut()
{
    copy();
    eraseSelectedRows();
}

void TagListView::paste()
{
    const QString text = QApplication::clipboard()->text();
    const auto pieces = splitTextToPieces(text);
    for(const auto& piece : pieces)
        tagInsertBelow(false,std::make_shared<teTag>(piece.text,piece.sentence),2);
}

void TagListView::setEditPopupAlways(bool always)
{
    if(m_editPopupAlways==always)
        return;
    m_editPopupAlways=always;
    // Turning it off must not leave a popup that the new rule would not allow.
    if(!always&&tagDisplay&&tagDisplay->isVisible()&&!shouldShowEditPopup(currentIndex()))
        hideTagDisplay();
}

bool TagListView::rowTextIsElided(const QModelIndex& index) const
{
    if(!index.isValid())
        return false;
    const QString text=index.data(Qt::DisplayRole).toString();
    if(text.isEmpty())
        return false;
    const auto* core=index.data(TagListModel::TagCoreRole).value<teTag*>();
    const QFont font(appFontFamily(),15);
    const QFontMetrics metrics(font);
    const TagRowLayout rowLayout=layoutTagRow(visualRect(index),
                                              core?TagMarkRegistry::instance().resolve(*core):TagVisual(),
                                              metrics);
    // Same condition paint() uses before it calls elidedText().
    return metrics.horizontalAdvance(text)>rowLayout.textRect.width();
}

bool TagListView::shouldShowEditPopup(const QModelIndex& index) const
{
    if(!index.isValid())
        return false;
    if(m_editPopupAlways)
        return true;
    if(!selectionModel()||!selectionModel()->isSelected(index))
        return false;
    // A sentence is always worth previewing: the row shows one line of it.
    if(const auto* core=index.data(TagListModel::TagCoreRole).value<teTag*>())
        if(core->type==teTag::sentence)
            return true;
    return rowTextIsElided(index);
}

void TagListView::showTagDisplayCore(std::shared_ptr<teTag> core)
{
    if(!core||!tagDisplay)
        return;
    tagDisplay->showForCore(core,tagDisplayAnchor(core));
}

void TagListView::followTagDisplayCore(std::shared_ptr<teTag> core)
{
    if(!tagDisplay||!tagDisplay->isVisible()||!core)
        return;
    if(tagDisplayHideTimer)
        tagDisplayHideTimer->stop();
    showTagDisplayCore(core);
}

void TagListView::hideTagDisplay()
{
    if(tagDisplayHideTimer)
        tagDisplayHideTimer->stop();
    if(tagDisplay)
        tagDisplay->hideDisplay();
}

bool TagListView::handleTagDisplayEscape(QKeyEvent* event)
{
    if(event&&event->key()==Qt::Key_Escape&&tagDisplay&&tagDisplay->isVisible()){
        hideTagDisplay();
        return true;
    }
    return false;
}

void TagListView::scheduleTagDisplayHide()
{
    if(!tagDisplay||!tagDisplay->isVisible()||!tagDisplayHideTimer)
        return;
    tagDisplayHideTimer->start(180);
}

void TagListView::tagDisplayHideTick()
{
    const QPoint pos = QCursor::pos();
    if(tagDisplay&&tagDisplay->isVisible()&&tagDisplay->frameGeometry().contains(pos))
        return;                         // the pointer is inside the popup
    hideTagDisplay();
}

void TagListView::createModelAndDelegate()
{
    m_model = new TagListModel(this);
    m_delegate = new TagDelegate(this);
    setModel(m_model);
    setItemDelegate(m_delegate);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setEditTriggers(QAbstractItemView::DoubleClicked|QAbstractItemView::EditKeyPressed);
    setUniformItemSizes(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::InternalMove);
    setDefaultDropAction(Qt::MoveAction);
    setFrameShape(QFrame::NoFrame);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    viewport()->installEventFilter(this);

    connect(this,&QListView::clicked,this,&TagListView::onViewClicked);
    connect(this,&QListView::doubleClicked,this,&TagListView::onViewDoubleClicked);
    connect(this,&QListView::customContextMenuRequested,this,&TagListView::onViewContextMenu);
    if(QItemSelectionModel* selection = selectionModel())
        connect(selection,&QItemSelectionModel::currentChanged,this,&TagListView::onViewCurrentChanged);
    connect(m_model,&QAbstractItemModel::modelAboutToBeReset,this,[this]{
        m_currentCoreBeforeReset = currentCore();
    });
    connect(m_model,&QAbstractItemModel::modelReset,this,&TagListView::onModelReset);
    // An empty row left behind by the inline editor disappears.
    connect(m_delegate,&QAbstractItemDelegate::closeEditor,this,[this](QWidget*,QAbstractItemDelegate::EndEditHint){
        finishRowEdit(this->currentIndex().row());
    });

    // The base class built the menu; here the actions have to work on rows.
    connect(editAction,&QAction::triggered,this,[this]{ tagEditCore(currentCore()); });
    connect(deleteAction,&QAction::triggered,this,&TagListView::eraseSelectedRows);
    connect(insertAction,&QAction::triggered,this,[this]{ tagInsertAbove(true,nullptr); });
    connect(insertBelowAction,&QAction::triggered,this,[this]{ tagInsertBelow(true,nullptr); });
    connect(cutAction,&QAction::triggered,this,[this]{ cut(); });
    connect(copyAction,&QAction::triggered,this,[this]{ copy(); });
    connect(pasteAction,&QAction::triggered,this,[this]{ paste(); });
}

void TagListView::dropEvent(QDropEvent* event)
{
    if(!dropRowsAt(event->position().toPoint())){
        event->ignore();
        return;
    }
    event->accept();
}

bool TagListView::dropRowsAt(const QPoint& viewportPos)
{
    auto* tagModel = qobject_cast<TagListModel*>(model());
    if(!tagModel)
        return false;
    QVector<int> rows;
    for(const QModelIndex& index:selectionModel()->selectedRows())
        rows.append(index.row());
    if(rows.isEmpty())
        return false;
    // Bias the position by half a row: dropping above a row's middle inserts
    // before it, below inserts after it. That is exactly the rule Qt uses to draw
    // its drop indicator, so the drop lands where the line is drawn - including
    // between two tags.
    auto* tagDelegate = qobject_cast<TagDelegate*>(itemDelegate());
    const int rowHeight = tagDelegate?tagDelegate->rowHeight():30;
    const QPoint pos(viewportPos.x(),viewportPos.y()+rowHeight/2);
    const QModelIndex target = indexAt(pos);
    const int dropRow = target.isValid()?target.row():tagModel->rowCount();
    if(!tagModel->moveTags(rows,dropRow))
        return false;
    // Keep the moved rows selected.
    sortRows(rows);
    selectionModel()->clearSelection();
    int first = dropRow;
    for(int row:rows)
        if(row<dropRow)
            --first;
    first = std::clamp(first,0,tagModel->rowCount()-1);
    for(int i=0;i<int(rows.size())&&first+i<tagModel->rowCount();++i)
        selectionModel()->select(tagModel->index(first+i,0),
                                 QItemSelectionModel::Select|QItemSelectionModel::Rows);
    setCurrentIndex(tagModel->index(first,0));
    return true;
}

void TagListView::sortRows(QVector<int>& rows)
{
    std::sort(rows.begin(),rows.end());
    rows.erase(std::unique(rows.begin(),rows.end()),rows.end());
}

void TagListView::startDrag(Qt::DropActions supportedActions)
{
    // Dragging a tag is no time for the popup: it hides as soon as the drag
    // starts (the user asked for exactly that).
    if(auto* list = qobject_cast<TagListView*>(parentWidget()))
        list->hideTagDisplay();
    // Deliberately not QListView::startDrag(): that one calls clearOrRemove()
    // when the drag ends with MoveAction, which removes the dragged rows a
    // second time - after dropEvent() already moved them into place. That is
    // what made rows vanish (and, with the model out of sync, crash) while
    // dragging. The multi tag list draws its own pixmap for the same reason.
    auto* tagModel = qobject_cast<TagListModel*>(model());
    const QModelIndexList indexes = selectionModel()->selectedRows();
    if(!tagModel||indexes.isEmpty())
        return;

    QFont font(appFontFamily(),10);
    const QFontMetrics metrics(font);
    const QString text = indexes.first().data(Qt::DisplayRole).toString();
    const int width = std::max(40,metrics.horizontalAdvance(text)+16);
    const int height = metrics.height()+8;
    QPixmap pixmap(width,height);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(QColor(21,63,34));
        painter.setPen(QColor(76,230,56));
        painter.drawRect(QRect(1,1,width-2,height-2));
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(QRect(7,0,width-14,height),Qt::AlignLeft|Qt::AlignVCenter,text);
    }

    QMimeData* mimeData = tagModel->mimeData(indexes);
    if(!mimeData)
        return;
    QDrag* drag = new QDrag(this);
    drag->setMimeData(mimeData);
    drag->setPixmap(pixmap);
    drag->setHotSpot(QPoint(8,height/2));
    drag->exec(supportedActions,defaultDropAction());
}

void TagListView::onViewClicked(const QModelIndex& index)
{
    // The popup opens on a click (and on the selection it makes), not on hover.
    if(!index.isValid()||!showing_list)
        return;
    const int row=index.row();
    if(row<0||row>=int(showing_list->size()))
        return;
    // With the Options switch off this is the "preview a tag the row cannot show"
    // case: a sentence, or a tag whose text is elided. Anything else stays quiet.
    if(shouldShowEditPopup(index))
        showTagDisplayCore(showing_list->shareAt(row));
    else
        hideTagDisplay();
}

void TagListView::onViewDoubleClicked(const QModelIndex& index)
{
    if(!index.isValid())
        return;
    // Which word was clicked decides what the editor selects. A double click in
    // the gap between words (or on an elided row) leaves wordStart at -1 and the
    // editor selects everything, like every other way of starting an edit.
    int wordStart=-1;
    int wordLength=0;
    if(m_doubleClickPos.x()>=0){
        const auto* core=index.data(TagListModel::TagCoreRole).value<teTag*>();
        if(core){
            const QFont font(appFontFamily(),15);
            const QFontMetrics metrics(font);
            const TagRowLayout rowLayout=layoutTagRow(visualRect(index),
                                                      TagMarkRegistry::instance().resolve(*core),
                                                      metrics);
            const QVector<TagWord> words=layoutTagWords(rowLayout.textRect,
                                                        index.data(Qt::DisplayRole).toString(),
                                                        metrics);
            const int hit=wordAtPosition(words,m_doubleClickPos);
            if(hit>=0){
                wordStart=words[hit].start;
                wordLength=words[hit].length;
            }
        }
    }
    m_doubleClickPos=QPoint(-1,-1);
    startEditingRow(index.row(),wordStart,wordLength);
}

int TagListView::wordStartAt(const QModelIndex& index,const QPoint& pos) const
{
    if(!index.isValid()||!m_model)
        return -1;
    const auto* core=index.data(TagListModel::TagCoreRole).value<teTag*>();
    if(!core)
        return -1;
    const QFont font(appFontFamily(),15);
    const QFontMetrics metrics(font);
    const TagRowLayout rowLayout=layoutTagRow(visualRect(index),
                                              TagMarkRegistry::instance().resolve(*core),
                                              metrics);
    const QVector<TagWord> words=layoutTagWords(rowLayout.textRect,
                                                index.data(Qt::DisplayRole).toString(),
                                                metrics);
    const int hit=wordAtPosition(words,pos);
    return hit<0?-1:words[hit].start;
}

void TagListView::clearHoverWord()
{
    if(!m_delegate||m_delegate->hoverWordStart()<0)
        return;
    const int row=m_delegate->hoverRow();
    m_delegate->setHoverWord(-1,-1);
    if(m_model&&row>=0)
        viewport()->update(visualRect(m_model->index(row,0)));
}

void TagListView::mouseMoveEvent(QMouseEvent* event)
{
    if(m_delegate){
        const QPoint pos=event->position().toPoint();
        const QModelIndex index=indexAt(pos);
        const int row=index.isValid()?index.row():-1;
        const int wordStart=row>=0?wordStartAt(index,pos):-1;
        if(row!=m_delegate->hoverRow()||wordStart!=m_delegate->hoverWordStart()){
            const int oldRow=m_delegate->hoverRow();
            m_delegate->setHoverWord(row,wordStart);
            if(m_model&&oldRow>=0)
                viewport()->update(visualRect(m_model->index(oldRow,0)));
            if(m_model&&row>=0)
                viewport()->update(visualRect(m_model->index(row,0)));
        }
    }
    QListView::mouseMoveEvent(event);
}

void TagListView::leaveEvent(QEvent* event)
{
    clearHoverWord();
    QListView::leaveEvent(event);
}

bool TagListView::edit(const QModelIndex& index,EditTrigger trigger,QEvent* event)
{
    if(m_delegate&&index.isValid()){
        int wordStart=-1;
        int wordLength=0;
        const bool doubleClick=(trigger==DoubleClicked)
                               ||(event&&event->type()==QEvent::MouseButtonDblClick);
        // A double click is the only trigger that carries a position; the others
        // (F2, insert) have no word to preserve, so the editor selects everything.
        QPoint pos(-1,-1);
        if(event&&event->type()==QEvent::MouseButtonDblClick)
            pos=static_cast<QMouseEvent*>(event)->position().toPoint();
        else if(trigger==DoubleClicked&&m_doubleClickPos.x()>=0)
            pos=m_doubleClickPos;
        if(pos.x()>=0){
            if(const auto* core=index.data(TagListModel::TagCoreRole).value<teTag*>()){
                const QFont font(appFontFamily(),15);
                const QFontMetrics metrics(font);
                const TagRowLayout rowLayout=layoutTagRow(visualRect(index),
                                                          TagMarkRegistry::instance().resolve(*core),
                                                          metrics);
                const QVector<TagWord> words=layoutTagWords(rowLayout.textRect,
                                                            index.data(Qt::DisplayRole).toString(),
                                                            metrics);
                const int hit=wordAtPosition(words,pos);
                if(hit>=0){
                    wordStart=words[hit].start;
                    wordLength=words[hit].length;
                }
            }
        }
        // Only a double click says anything about a word. Any other trigger
        // explicitly means "the whole tag", so it resets the range instead of
        // inheriting whatever the last double click left behind.
        if(doubleClick)
            m_delegate->setPendingWordSelection(wordStart,wordLength);
        else
            m_delegate->setPendingWordSelection(-1,0);
    }
    return QListView::edit(index,trigger,event);
}

void TagListView::mouseDoubleClickEvent(QMouseEvent* event)
{
    m_doubleClickPos=event->position().toPoint();
    // The word itself is decided in edit(), which Qt calls before it builds the
    // editor; this only keeps the position for the triggers that carry none.
    QListView::mouseDoubleClickEvent(event);
}

void TagListView::onViewCurrentChanged(const QModelIndex& current,const QModelIndex& previous)
{
    Q_UNUSED(previous);
    if(current.isValid())
        m_pendingSelectionIndex=current.row();
}

void TagListView::onModelReset()
{
    // Somebody changed the list behind the model's back (an editor inserted a
    // tag, a rename merged two tags). Rows were rebuilt, so the selection is
    // restored by tag core, which is what the editors address anyway.
    if(m_currentCoreBeforeReset){
        const int row = m_model->rowOf(m_currentCoreBeforeReset.get());
        if(row>=0)
            setSelectCurrentCore(m_currentCoreBeforeReset);
    }
    m_currentCoreBeforeReset.reset();
}

void TagListView::eraseSelectedRows()
{
    if(!m_model)
        return;
    tagErase(nullptr);      // "erase the selection", shared with the editors' path
}
void TagListView::finishRowEdit(int row)
{
    if(!m_model||row<0||row>=m_model->rowCount())
        return;
    teTag* raw = m_model->index(row,0).data(TagListModel::TagCoreRole).value<teTag*>();
    if(raw&&static_cast<QString>(*raw).trimmed().isEmpty()){
        // An empty tag is not worth keeping: this is how the old inline editor
        // behaved for a freshly inserted tag.
        telog("[teTagListWidget] dropping an empty tag");
        m_model->eraseTag(row);
    }
}

void TagListView::onViewContextMenu(const QPoint& pos)
{
    const QModelIndex index = this->indexAt(pos);
    if(index.isValid()&&!this->selectionModel()->isSelected(index))
        setSelectCurrentCore(showing_list&&index.row()<int(showing_list->size())
                                 ?showing_list->shareAt(index.row())
                                 :nullptr);
    menu->exec(this->viewport()->mapToGlobal(pos));
}

bool TagListView::eventFilter(QObject* watched,QEvent* event)
{
    if(this&&watched==this->viewport()){
        switch(event->type()){
        case QEvent::MouseMove:{
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            const QModelIndex index = this->indexAt(mouseEvent->position().toPoint());
            if(!tagDisplay)
                break;
            if(!index.isValid()){
                hideTagDisplay();
                break;
            }
            teTag* raw = index.data(TagListModel::TagCoreRole).value<teTag*>();
            if(!raw)
                break;
            std::shared_ptr<teTag> core;
            for(const std::shared_ptr<teTag>& candidate:*showing_list)
                if(candidate.get()==raw){
                    core=candidate;
                    break;
                }
            if(!core)
                break;
            if(m_editPopupAlways){
                // The old behaviour: a click opened it, hovering then walks it
                // along the rows. Hovering alone never opens it.
                if(tagDisplay->isVisible())
                    followTagDisplayCore(core);
            }else if(shouldShowEditPopup(index)){
                // Preview mode: the pointer over the *selected* sentence / long
                // tag is what shows it (and moves it between such rows).
                if(tagDisplay->isVisible())
                    followTagDisplayCore(core);
                else
                    showTagDisplayCore(core);
            }else if(tagDisplay->isVisible()){
                hideTagDisplay();
            }
            break;
        }
        case QEvent::Leave:
            // Leave arrives at the viewport, not at the view, so the word frame
            // has to be dropped here - a frame that outlives the pointer looks
            // like a second selection.
            clearHoverWord();
            scheduleTagDisplayHide();
            break;
        case QEvent::MouseButtonPress:{
            // A click on empty space below the tags closes the popup.
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if(!this->indexAt(mouseEvent->position().toPoint()).isValid())
                hideTagDisplay();
            break;
        }
        default:
            break;
        }
    }
    return QObject::eventFilter(watched,event);
}
