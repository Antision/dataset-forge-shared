#ifndef TAGVIEWCONTRACTS_H
#define TAGVIEWCONTRACTS_H

/**
 * @file tagviewcontracts.h
 * @brief What the tag view needs from its host: captions, editors, completion.
 *
 * These three interfaces are the dependency inversion from S5. The view itself
 * still lives in tag_quick_editor; the interfaces live here because they only
 * talk about the tag data layer, and gallery_info implements the other side of
 * two of them (a caption source over its dataset, no editor host).
 */

#include <functional>
#include <QString>
#include <QVector>

class teTagList;

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

#endif // TAGVIEWCONTRACTS_H
