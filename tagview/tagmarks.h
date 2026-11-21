#ifndef TAGMARKS_H
#define TAGMARKS_H

/**
 * @file tagmarks.h
 * @brief Display marks for tag rows: colours, emoji/PNG icons and the rules that
 *        produce them from the tag text (MARK-01..06).
 *
 * Nothing here changes a tag. The registry answers "how should this tag look",
 * and TagDelegate draws the answer; the same registry is used by tqe and
 * gallery_info, which is what keeps their decoration identical.
 */

#include "tag.h"
#include "tagvisual.h"

#include <QColor>
#include <QFontMetrics>
#include <QHash>
#include <QRegularExpression>
#include <QRect>
#include <QString>
#include <QVector>

/// A dark colour (black hair) disappears on the black row; lift the lightness
/// until it reads as a border, keeping the hue. Used by the delegate too.
QColor tagBorderColor(const QColor& color,int alpha=217);

/**
 * @brief Recomputes the tag's stored decoration from its text.
 *
 * Decoration is display data the editors own: the rules run when a control reads
 * or re-reads a tag, never inside paint(). Call this after the text changed.
 */
void refreshTagDecoration(const std::shared_ptr<teTag>& tag);

/// Writes the decoration only when the tag has none yet (first read).
void ensureTagDecoration(const std::shared_ptr<teTag>& tag);

/// One drawn chip and the rectangle it occupies (used for painting and hit
/// testing - both call layoutTagRow()).
struct TagChip {
    QString id;
    QString icon;
    QString tooltip;
    QRect rect;
    /// Emoji/PNG size for this chip. Shrinks when a row has several icons.
    int iconPx=16;
};

/// The geometry of one row: the chips on the left and the text that is left.
struct TagRowLayout {
    QRect textRect;
    QVector<TagChip> chips;
};

/**
 * @brief Draws and hit-tests one row from the same geometry (MARK-05).
 *
 * `rowRect` is the delegate's item rectangle. Chips are 18px high, padded 6px
 * left/right, and the text starts after the last one; without chips the text
 * keeps the old 4px inset.
 */
TagRowLayout layoutTagRow(const QRect& rowRect,const TagVisual& visual,
                          const QFontMetrics& metrics);

/**
 * @brief One word of a tag row: where it is drawn and which characters it covers.
 *
 * The old widget based list had one widget per word, so hovering and
 * double-click-to-select were free. With one row drawn as a whole, words are
 * geometry instead: layoutTagWords() is the single place that decides where a
 * word is, and both the hover frame and the double-click selection use it.
 */
struct TagWord {
    QRect rect;      ///< viewport coordinates, vertically centred in the row
    int start=0;     ///< first character of the word in the tag text
    /// Characters to select for this word: the word plus the one space after it,
    /// so that typing over a selection replaces a word, not the whole tag.
    int length=0;
};

/**
 * @brief Splits `text` into the words drawn inside `textRect`.
 *
 * Same font and the same left alignment the delegate paints with, so a word
 * frame lands exactly on the word it belongs to. Returns an empty list when the
 * text is elided: the drawn string is then not the tag text, and mapping a
 * position back to a character range would be a lie.
 */
QVector<TagWord> layoutTagWords(const QRect& textRect,const QString& text,
                                const QFontMetrics& metrics);

/**
 * @brief The word under `pos`, or -1 when the position is in a gap / off the text.
 */
int wordAtPosition(const QVector<TagWord>& words,const QPoint& pos);

/**
 * @brief One user-defined decoration for an exact tag text.
 *
 * Stored in the program's own config (the settings dialog edits it); the registry
 * only applies it. `color` is a colour name from the table or "#rrggbb"; `icon`
 * is an emoji/character or a file name under data/icons (or an absolute path).
 */
struct TagMarkOverride {
    QString tag;
    QString color;
    QString icon;
};

/**
 * @brief The process-wide decoration registry.
 *
 * Loaded lazily from `<appDir>/data/decorations.json`; a missing file is
 * generated from the built-in defaults. Marks set by editor controls win over
 * the rules, so a control can always override what a rule says.
 */
class TagMarkRegistry
{
public:
    static TagMarkRegistry& instance();

    /// Loads `filePath`, or the default path when it is empty. Replaces the
    /// current rules; safe to call again (the decoration editor does).
    void load(const QString& filePath=QString());
    /// Path load() used (empty before the first call).
    QString filePath() const { return m_path; }
    bool isLoaded() const { return m_loaded; }
    int ruleCount() const { return int(m_rules.size()); }

    /// The decoration the rules produce for `tag` (what the editor control
    /// writes into teTag::decoration).
    TagVisual decorate(const teTag& tag) const;
    /// How `tag` should be drawn: its stored decoration plus control marks.
    /// No text rules here - a tag without a decoration control stays plain.
    TagVisual resolve(const teTag& tag) const;
    /// Colour for a name from the table; invalid when unknown.
    QColor colorFor(const QString& name) const;

    /// User overrides from the settings dialog. They win over every rule.
    void setOverrides(const QVector<TagMarkOverride>& overrides) { m_overrides=overrides; }
    const QVector<TagMarkOverride>& overrides() const { return m_overrides; }

    /// Writes the built-in defaults to `filePath` (used on first run and by the
    /// decoration editor's "restore defaults").
    static bool writeDefaults(const QString& filePath);

private:
    struct Rule {
        QString tag;            ///< exact match (empty = regex rule)
        QRegularExpression regex;
        QString colorFrom;      ///< regex capture group name for the colour
        QString icon;
    };

    void loadDefaults();
    void applyRules(const QString& text,TagVisual& visual) const;
    void applyColorName(const QString& name,TagVisual& visual) const;
    /// Solid colour (no pattern): the shared body of applyColorName().
    void applySolidColor(const QColor& color,TagVisual& visual) const;
    /// The settings dialog's per-tag overrides, applied after the rules.
    void applyOverrides(const QString& text,TagVisual& visual) const;
    /// Colours named in `text`, in the order they appear (MARK-03/06).
    QStringList colorsInText(const QString& text) const;
    /// "gradient from a to b", "x hair with y streaks", rainbow ... .
    void applyTextPattern(const QString& text,TagVisual& visual) const;

    bool m_loaded=false;
    QString m_path;
    QHash<QString,QColor> m_colors;
    QVector<Rule> m_rules;
    QVector<TagMarkOverride> m_overrides;
};

#endif // TAGMARKS_H
