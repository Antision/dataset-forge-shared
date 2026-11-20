#ifndef TAGVISUAL_H
#define TAGVISUAL_H

#include <QColor>
#include <QString>
#include <QVector>

/// One icon shown on a tag row (emoji text or a PNG path).
struct TagIconSpec {
    QString value;
    QString tooltip;
    /// True when `value` is a file path rather than an emoji/character.
    bool isImage() const;
};

/// What a tag row should look like. Filled by the decoration editor control,
/// stored on the tag, read by the delegate and by the reftaglist QSS.
struct TagVisual {
    bool hasColor=false;
    /// Left-to-right gradient (rainbow / "gradient from a to b" / multicolored).
    bool gradient=false;
    /// Equal-width colour bands ("blue and white striped panties").
    bool striped=false;
    /// Hair streaks: base colour + one cell per streak colour.
    bool streaked=false;
    /// Raw "green streaked hair": no base colour, only the streak cells.
    bool rawStreak=false;
    /// White base / first gradient stop: white fill+border, black text.
    bool whiteDominant=false;
    /// Base colour of a streak tag (valid when `hasBase`).
    QColor baseColor;
    bool hasBase=false;
    QColor border;
    QColor fill;
    /// Gradient stops or stripe/streak colours, left to right.
    QVector<QColor> gradientStops;
    /// Left to right, after the merge chips.
    QVector<TagIconSpec> icons;

    bool isEmpty() const { return !hasColor&&icons.isEmpty(); }
};

#endif // TAGVISUAL_H
