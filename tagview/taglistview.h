#ifndef TAGLISTVIEW_H
#define TAGLISTVIEW_H

/**
 * @file taglistview.h
 * @brief The model/view based tag list: one row per tag, drawn by TagDelegate.
 *
 * This class used to live in tag_quick_editor/tetaglistwidget.* next to the
 * widget based list the editors still use. Everything program-specific now
 * arrives through interfaces (CaptionSource, TagEditorHost, TagViewHost), so the
 * list itself is shared with gallery_info.
 */

#include "taglistmodel.h"
#include "tagviewcontracts.h"
#include "type_search.h"

#include <QListView>

#include <cstddef>

class QAction;
class QMenu;
class QTimer;

/**
 * @brief The tag list of one picture: a QListView that owns its tags.
 *
 * There is no wrapper widget and no separate controller any more. This class is
 * what mainwindow.ui puts in the window, and it is also the thing the editors,
 * the undo/redo actions and the magnified popup talk to: it holds the tag list,
 * the model, the delegate, the selection (addressed by tag core), the context
 * menu and (when the host supplies one) the popup.
 *
 * The tags are drawn by TagListModel + TagDelegate, one row per tag and no
 * widget per tag, which is what makes switching from one image to the next cheap.
 *
 * Qt's InternalMove would need mime data and a model that accepts drops; the row
 * move is expressed directly through TagListModel::moveRows() instead, which is
 * also what the undo records expect.
 */
class TagListView : public QListView, public TagDisplayOwner, public TagListHost, virtual public teObject
{
    Q_OBJECT
public:
    explicit TagListView(QWidget* parent=nullptr);
    ~TagListView();
    /**
     * @brief Installs the program-specific pieces (popup, long-text window).
     *
     * Safe to call with nullptr: the view then simply has no popup and leaves
     * sentence tags to the delegate (a program that does not set a host is one
     * that does not offer those two editors).
     */
    void setViewHost(TagViewHost* host);

    teTagList* showing_list=nullptr;///< the teTagList which is displaying
    /// Where the list came from and where the editors are told about it. Both
    /// are interfaces (TV-03): this view must not know what a picture file or an
    /// editor list is, or it cannot move into the shared library.
    CaptionSource* m_captionSource=nullptr;
    TagEditorHost* m_editorHost=nullptr;
    /// Image the shown caption belongs to (empty when nothing is shown).
    QString m_imagePath;
    /// The magnified popup (created by the host, may stay null).
    TagDisplay* tagDisplay=nullptr;

    /**
     * @brief Moves the selected rows to where a drop at `viewportPos` would go.
     *
     * dropEvent() is only the plumbing around this; separating the two keeps the
     * placement rule testable without a real drag (a synthetic QDropEvent sent to
     * the viewport never reaches dropEvent()).
     *
     * @return false when there is nothing to move or the move changed nothing.
     */
    bool dropRowsAt(const QPoint& viewportPos);
    /// The model behind the rows.
    TagListModel* tagModel() const { return m_model; }
    /**
     * @brief A model/view list has no widget per tag, so this is always nullptr.
     *
     * This is the whole reason tag and widget can be decoupled: the editor code
     * asks the *list* for a tag's widget, and the only list that has widgets is
     * the editors' own widget based one. Nothing can leak a widget onto these
     * rows, and no "is this tag view owned" flag is needed to prevent it.
     */
    /// The widget showing `core` in this list, or nullptr. A view based list draws
    /// rows and has no widget per tag, so this is where "there is none" is decided.
    teTagWidgetBase* widgetForCore(const std::shared_ptr<teTag>& core) const {
        Q_UNUSED(core);
        return nullptr;
    }
    teTagWidgetBase* ensureWidgetFor(std::shared_ptr<teTag> core) override {
        Q_UNUSED(core);
        return nullptr;
    }
    void releaseWidgetFor(std::shared_ptr<teTag> core) override {
        Q_UNUSED(core);
    }
    /// The delegate that draws and edits the rows.
    TagDelegate* delegate() const { return m_delegate; }
    size_t size()const;
    /// Tag of the current row, or nullptr.
    std::shared_ptr<teTag> currentCore() const;
    /// Selects every row (Ctrl+A).
    void selectAllRows();

    void clear(teTagList* in=nullptr);
    void load(teTagList* newlist=nullptr);
    /// Sets where loadCaption() gets its tags from. Not owned; may be null.
    void setCaptionSource(CaptionSource* source);
    /// Sets who is told when a list was loaded or went away. Not owned.
    void setEditorHost(TagEditorHost* host){ m_editorHost=host; }
    /**
     * @brief Shows the caption of `imagePath`.
     *
     * Loads the list from the caption source (the same list every time, so the
     * undo history of a picture survives switching away and back). A path whose
     * source has nothing to offer empties the view; that is the "no caption
     * file yet" case, not an error.
     */
    void loadCaption(const QString& imagePath);
    /// Drops the shown caption and forgets which image it belonged to.
    void closeCaption();
    /// Image currently shown, or an empty string.
    QString captionImagePath() const { return m_imagePath; }
    /// True when the shown list has changes the caption file does not have yet.
    /// The host decides what to do with it (save prompt, flash, ...); the view
    /// deliberately knows nothing about a save button.
    bool isDirty() const { return showing_list&&!showing_list->isSaved; }
    /// Called by the host after it wrote the caption: clears the dirty state.
    /// (CaptionSource::save() already does this; this is for hosts that write
    /// through another path.)
    void markSaved(){ if(showing_list) showing_list->isSaved=true; }
    void onTagEdited(std::shared_ptr<teTag>tag);
    /**
     * @brief Replaces the shown list's content and makes everyone re-read it.
     *
     * Only undo/redo uses it; it is the equivalent of loading the picture again,
     * with the tags taken from the snapshot instead of the caption file.
     */
    void applySnapshot(const teTagSnapshotList& snapshot);
    void undo();
    void redo();
    bool canUndo() const;
    bool canRedo() const;
    void tagErase(int index);
    void tagErase(std::shared_ptr<teTag> tag=nullptr);
    /**
     * @brief The one way to say "this tag is going away" (see teTagList::retire).
     *
     * Marks the tag and erases it from the list in one step.
     */
    void retire(std::shared_ptr<teTag> tag);
    void tagEdit(std::shared_ptr<teTag>tag, QString text,int removeDuplicate=1,bool ifemit=true);
    void tagEditCore(std::shared_ptr<teTag> core);
    /// Inserts a tag as a row. Returns nullptr: this list has no tag widgets, the
    /// signature is kept because the editors call it.
    teTagWidgetBase* taginsert(int index,std::shared_ptr<teTag> in_tag,int removeDuplicate=1,bool select=true);
    void tagInsertAbove(bool edit=true,std::shared_ptr<teTag>newtag=nullptr,int removeDuplicate=1);
    void tagInsertBelow(bool edit=true,std::shared_ptr<teTag>newtag=nullptr,int removeDuplicate=1);

    // -- clipboard -------------------------------------------------------------
    void copy();
    void cut();
    void paste();

    // -- selection, addressed by tag core --------------------------------------
    void setSelectCurrentCore(std::shared_ptr<teTag> core,bool ifclear=true);
    void setSelectCore(std::shared_ptr<teTag> core);
    int setUnselectCore(std::shared_ptr<teTag> core=nullptr);
    int setSelectRangeCore(std::shared_ptr<teTag> core,bool ifclear=true);
    bool isCoreSelected(std::shared_ptr<teTag> core) const;
    void ensureCoreVisible(std::shared_ptr<teTag> core);
    /// Scrolls back to the top of the list.
    void scrollToTop();

    // -- the magnified popup ---------------------------------------------------
    /// TagDisplayOwner: the tag list currently shown.
    teTagList* shownList() const override { return showing_list; }
    /// TagDisplayOwner: a click inside the view keeps the popup open.
    bool ownsWidget(QWidget* widget) const override {
        return widget&&(widget==this||isAncestorOf(widget));
    }
    /// Right edge of the row showing `core`.
    QPoint tagDisplayAnchor(std::shared_ptr<teTag> core) const;
    void showTagDisplayCore(std::shared_ptr<teTag> core);
    void hideTagDisplay();
    /// Keeps the popup following the current row while it is up.
    void followTagDisplayCore(std::shared_ptr<teTag> core);
    /// Esc closes the popup (returns true when it consumed the key).
    bool handleTagDisplayEscape(QKeyEvent* event);

    /// The text of the selected rows, in the caption's own format.
    QString getSelectText();
    /// Hides the magnified popup when the pointer really left it.
    bool eventFilter(QObject* watched,QEvent* event)override;
public slots:
    void onViewClicked(const QModelIndex& index);
    void onViewDoubleClicked(const QModelIndex& index);
    void onViewContextMenu(const QPoint& pos);
    void onViewCurrentChanged(const QModelIndex& current,const QModelIndex& previous);
    void onModelReset();
protected:
    void dropEvent(QDropEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    /// Hovering the list gives it the keyboard focus, so the list under the mouse
    /// is the one the keyboard talks to (its shortcuts and its type-to-search).
    /// This is how the widget based list behaved before the refactor.
    void enterEvent(QEnterEvent* event) override {
        setFocus();
        QListView::enterEvent(event);
    }
    /// Moves the current row to the tag starting with what has been typed.
    /// Shared with the multi tag list, see type_search.h.
    void typeSearch(const QString& text);
    /// Draws its own drag pixmap and, more importantly, does not let Qt remove
    /// the dragged rows a second time (see the comment in the implementation).
    void startDrag(Qt::DropActions supportedActions) override;
private:
    static void sortRows(QVector<int>& rows);
    /// Creates the model, delegate, menu, plain text window and popup.
    void createModelAndDelegate();
    /// Builds the context menu.
    void createMenu();
    /// Starts editing `row` (or opens the sentence window).
    void startEditingRow(int row);
    /// Erases every selected row.
    void eraseSelectedRows();
    /// Drops a row that was left empty by the inline editor.
    void finishRowEdit(int row);
    /// Scrolls the popup away when the pointer left it (grace period).
    void scheduleTagDisplayHide();
    void tagDisplayHideTick();
    /// CaptionSource reports that a list it handed out is about to die.
    void onCaptionListGone(teTagList* list);

    TagListModel* m_model=nullptr;
    TagDelegate* m_delegate=nullptr;
    /// Context menu and its actions.
    QMenu* menu=nullptr;
    QAction *editAction=nullptr,*deleteAction=nullptr,*insertAction=nullptr,
            *insertBelowAction=nullptr,*cutAction=nullptr,*copyAction=nullptr,
            *pasteAction=nullptr;
    /// The popup's grace timer.
    QTimer* tagDisplayHideTimer=nullptr;
    /// Core that was current before a model reset (external changes rebuild the
    /// model, so the selection has to be restored by core).
    std::shared_ptr<teTag> m_currentCoreBeforeReset;
    /// Row to select as soon as the next list is loaded (kept across a reset).
    int m_pendingSelectionIndex=-1;
    /// Type-to-search state (see type_search.h).
    TypeSearch typeSearchState;
    /// Who builds the popup and the long-text window; may be null.
    TagViewHost* m_viewHost=nullptr;
};

#endif // TAGLISTVIEW_H
