#ifndef TAGVIEWCONTRACTS_H
#define TAGVIEWCONTRACTS_H

/**
 * @file tagviewcontracts.h
 * @brief What the tag view needs from its host: captions, editors, completion.
 *
 * The dependency inversion from S5. The view itself is shared now
 * (taglistview.*); these interfaces are what keeps it free of either program:
 * gallery_info implements its own caption source and suggestion source, and
 * both programs decide through TagViewHost whether they have a popup and a
 * long-text window.
 */

#include <functional>
#include <memory>

#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>

class QWidget;
class teTag;
class teTagList;
class teTagWidgetBase;
class TagListView;

/**
 * @brief What a tag view needs from the thing that stores captions (TV-03).
 *
 * The view used to take a `tePictureFile*`: it reached into the file for the
 * tag list, and it listened for the file's destruction. That is the tqe-side
 * storage model, and it is exactly what a shared tag view cannot carry into
 * gallery_info. Everything the view needs from a caption is here instead.
 *
 * `open()` replaces the `load(path, TagList&)` sketched in REQUIREMENTS TV-03
 * on purpose: tqe keeps one tag list per picture and the undo history belongs
 * to that list, so the view must show *that* list, not a copy filled into a
 * caller-owned one. gallery_info's source does the same with the list of the
 * opened image. Ownership stays with the source either way.
 */
class CaptionSource
{
public:
    virtual ~CaptionSource()=default;
    /// The tag list of `imagePath`, loaded on first use. The source keeps
    /// ownership; nullptr means this image has no caption storage.
    virtual teTagList* open(const QString& imagePath)=0;
    /// Writes `list` as the caption of `imagePath`. True when it reached disk.
    virtual bool save(const QString& imagePath,teTagList& list)=0;
    /// The caption file `imagePath` is stored next to.
    virtual QString captionPathFor(const QString& imagePath) const=0;
    /**
     * @brief Registers the view's "this list is going away" handler.
     *
     * A list handed out by open() can die while the view still shows it (the
     * picture was removed from the dataset). The view cannot listen for that
     * on its own any more - it does not know what a tePictureFile is - so the
     * source reports it. The handler must not dereference the list: it is
     * called while its destructor runs.
     */
    void setListGoneHandler(std::function<void(teTagList*)> handler){
        m_listGone=std::move(handler);
    }
protected:
    void notifyListGone(teTagList* list){
        if(m_listGone)
            m_listGone(list);
    }
private:
    std::function<void(teTagList*)> m_listGone;
};

/**
 * @brief The editors that read a tag list, as the view sees them (TV-03).
 *
 * tqe's teEditorList normalises the list while reading it (two mergeable
 * clothes tags become one); gallery_info has no editors and passes nullptr.
 * The view only announces "a list was loaded" and "the list went away"; what
 * the host does with that is the host's business.
 *
 * There is no resetAll(): nothing has needed "re-read without normalising" yet,
 * and this code base does not keep virtuals nobody calls. Add it together with
 * its first caller.
 */
class TagEditorHost
{
public:
    virtual ~TagEditorHost()=default;
    /// Re-reads `list` in every editor (nullptr: nothing to read).
    virtual void readAll(teTagList* list)=0;
    /// Drops everything the editors show.
    virtual void clearAll()=0;
    /**
     * @brief Recomputes the decoration of every tag the editors show.
     *
     * The user-config overrides (settings dialog) change how tags look without
     * touching the text, so the stored decoration has to be rebuilt and every
     * per-tag widget re-styled. Programs without editors simply ignore this.
     */
    virtual void refreshDecorations(){}
    /**
     * @brief Temporarily stops the editors from merging tags.
     *
     * Undo/redo rebuilds the list from a snapshot - a state the user had. If the
     * editors were allowed to normalise it again, the merge that was just undone
     * would immediately happen again. The view disables merging around that
     * rebuild and restores the previous value afterwards.
     */
    virtual bool mergeEnabled() const { return true; }
    virtual void setMergeEnabled(bool enabled){ Q_UNUSED(enabled); }
};

/// One completion offered while editing a tag.
struct SuggestionItem
{
    /// Text that is inserted into the editor.
    QString text;
    /// What the suggestion box shows (may include the preferred synonym and
    /// the tag's frequency).
    QString display;
};

/**
 * @brief Where tag completion comes from (TV-03).
 *
 * The editor widget used to search the global `ctags` array itself, which tied
 * every tag list in the program - including a future gallery_info one - to
 * tqe's tag index and its incremental range cache. gallery_info has no index
 * and simply installs no source.
 */
class SuggestionSource
{
public:
    virtual ~SuggestionSource()=default;
    /// Up to `limit` completions for `prefix`, most frequent first.
    virtual QVector<SuggestionItem> suggest(const QString& prefix,int limit)=0;
};

/// The source every suggestion editor uses. Null means "no completion".
/// Install it once at startup; it has to outlive every editor that may ask.
SuggestionSource* suggestionSource();
void setSuggestionSource(SuggestionSource* source);

/**
 * @brief What the tag view needs from its magnified popup.
 *
 * The popup stays in tag_quick_editor: it is built from per-word widgets and
 * drags a tag widget, neither of which the shared model/view list has. The view
 * only needs to show it for a row, hide it, and know where it is, so that is all
 * this interface has. gallery_info can leave the popup out by not creating one.
 */
class TagDisplay
{
public:
    virtual ~TagDisplay()=default;
    /// The widget behind the popup (the view deletes it with itself).
    virtual QWidget* widget()=0;
    virtual bool isVisible() const=0;
    virtual QRect frameGeometry() const=0;
    /// Shows `core` next to `anchor` (a global position).
    virtual void showForCore(std::shared_ptr<teTag> core,const QPoint& anchor)=0;
    virtual void hideDisplay()=0;
    /// Called when the pointer leaves the popup; the view decides when to hide.
    virtual void setPointerLeftHandler(std::function<void()> handler)=0;
};

/**
 * @brief What the popup needs from the list showing it.
 *
 * Moved here from tag_quick_editor/tetagdisplaywidget.h: the view implements it
 * and the view is shared now.
 */
class TagDisplayOwner
{
public:
    virtual ~TagDisplayOwner()=default;
    /// The tag list currently shown (used to check whether a tag still exists).
    virtual teTagList* shownList() const = 0;
    /// Selects the tag the popup belongs to.
    virtual void setSelectCurrentCore(std::shared_ptr<teTag> core,bool ifclear=true) = 0;
    /// Edits a tag that has no widget of its own.
    virtual void tagEditCore(std::shared_ptr<teTag> core) = 0;
    /// Replaces the text of a tag.
    virtual void tagEdit(std::shared_ptr<teTag> tag,QString text,int removeDuplicate=1,bool ifemit=true) = 0;
    /// Hides the popup.
    virtual void hideTagDisplay() = 0;
    /// True when `widget` belongs to the list (a click inside the list keeps the
    /// popup open, a click anywhere else closes it).
    virtual bool ownsWidget(QWidget* widget) const = 0;
    /// The popup body was dragged. Only a list whose tags have widgets can move a
    /// tag that way, so the default does nothing.
    virtual void tagDragged(teTagWidgetBase* tag,Qt::KeyboardModifiers modifiers){
        Q_UNUSED(tag);
        Q_UNUSED(modifiers);
    }
};

/**
 * @brief What "a tag list" is, as far as the code holding tags is concerned.
 *
 * Widgets belong to the list that shows them, not to the tag. The widget based
 * list (the editors' tag lists) implements this with real widgets; the view
 * based list implements it as a no-op, because a row drawn by a delegate has no
 * widget per tag.
 */
class TagListHost
{
public:
    virtual ~TagListHost()=default;
    /// The widget showing `core` in this list, building it on demand. nullptr
    /// when this list does not show widgets at all.
    virtual teTagWidgetBase* ensureWidgetFor(std::shared_ptr<teTag> core) = 0;
    /// Gives the widget of `core` back to the pool.
    virtual void releaseWidgetFor(std::shared_ptr<teTag> core) = 0;
};

/**
 * @brief What the shared view needs from the program around it.
 *
 * Two things are still program-specific: the magnified popup (tqe has one,
 * gallery_info may not) and the long-text window for sentence tags (tqe's is a
 * LiteWidget2). Both arrive through here instead of being constructed by the
 * view, which is what lets the view live in common.
 */
class TagViewHost
{
public:
    virtual ~TagViewHost()=default;
    /// Builds the popup for `owner`; nullptr means "this program has no popup".
    /// The view owns the returned display (and deletes its widget).
    virtual TagDisplay* createTagDisplay(TagDisplayOwner* owner)=0;
    /// Opens the program's long-text window for a sentence tag.
    virtual void editLongText(TagListView* view,std::shared_ptr<teTag> core)=0;
};

#endif // TAGVIEWCONTRACTS_H
