#include "tagmarks.h"
#include "log.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <QSet>


#include <algorithm>

namespace {

constexpr int kChipHeight = 18;
constexpr int kChipPadding = 3;
constexpr int kIconSize = 16;
constexpr int kRowInset = 4;

bool isMultiColor(const QString& name)
{
    static const QSet<QString> names{
        QStringLiteral("rainbow"),QStringLiteral("gradient"),
        QStringLiteral("multicolored"),QStringLiteral("multi-colored"),
        QStringLiteral("two-tone"),QStringLiteral("twotone")};
    return names.contains(name.trimmed().toLower());
}

QVector<QColor> gradientFor(const QString& name)
{
    const QString key=name.trimmed().toLower();
    if(key==QLatin1String("two-tone")||key==QLatin1String("twotone"))
        return {QColor(0xf0,0x8f,0xb0),QColor(0x35,0xc6,0xc6)};
    return {QColor(0xd0,0x34,0x2c),QColor(0xe8,0x86,0x2e),QColor(0xf1,0xc4,0x0f),
            QColor(0x3e,0xa5,0x5b),QColor(0x3b,0x6f,0xd4),QColor(0x9b,0x59,0xb6)};
}

QColor withAlpha(QColor color,int alpha)
{
    color.setAlpha(alpha);
    return color;
}


/// Built-in colour table (MARK-03).
const QHash<QString,QColor>& defaultColors()
{
    static const QHash<QString,QColor> colors{
        {QStringLiteral("white"),QColor(0xf2,0xf2,0xf2)},
        {QStringLiteral("black"),QColor(0x2b,0x2b,0x2b)},
        {QStringLiteral("grey"),QColor(0x8a,0x8a,0x8a)},
        {QStringLiteral("gray"),QColor(0x8a,0x8a,0x8a)},
        {QStringLiteral("silver"),QColor(0xc9,0xcc,0xd1)},
        {QStringLiteral("blonde"),QColor(0xe6,0xc8,0x6e)},
        {QStringLiteral("brown"),QColor(0x8b,0x5a,0x2b)},
        {QStringLiteral("red"),QColor(0xd0,0x34,0x2c)},
        {QStringLiteral("pink"),QColor(0xf0,0x8f,0xb0)},
        {QStringLiteral("purple"),QColor(0x9b,0x59,0xb6)},
        {QStringLiteral("blue"),QColor(0x3b,0x6f,0xd4)},
        {QStringLiteral("aqua"),QColor(0x35,0xc6,0xc6)},
        {QStringLiteral("green"),QColor(0x3e,0xa5,0x5b)},
        {QStringLiteral("orange"),QColor(0xe8,0x86,0x2e)},
        {QStringLiteral("yellow"),QColor(0xf1,0xc4,0x0f)},
    };
    return colors;
}

struct IconRow { const char* tag; const char* icon; };

/// Built-in icon table (MARK-04). No emoji means no icon: a wrong glyph is
/// worse than none, and a PNG can be added in decorations.json later.
const QVector<IconRow>& defaultIcons()
{
    static const QVector<IconRow> rows{
        {"masterpiece","🏆"},          // 🏆
        {"best quality","🏆"},
        {"smile","😀"},                // 😀
        {"light smile","🙂"},          // 🙂
        {"scenery","🏞️"},  // 🏞️
        {"dim light","🔅"},            // 🔅
        {"backlighting","🔆"},         // 🔆
        {"night","🌃"},                // 🌃
        {"night city","🌃"},
        {"pixel art","👾"},            // 👾
        {"blue archive","🎓"},         // 🎓
        {"dress","👗"},                // 👗
        {"skirt","👗"},
        {"shirt","👕"},                // 👕
        {"pants","👖"},                // 👖
        {"swimsuit","👙"},             // 👙
        {"socks","🧦"},                // 🧦
        {"shoes","👠"},                // 👠
        {"hat","🎩"},                  // 🎩
        {"glasses","👓"},              // 👓
        {"weapon","🗡️"},   // 🗡️
        {"cat ears","🐱"},             // 🐱
    };
    return rows;
}

QJsonObject defaultsJson()
{
    QJsonObject colors;
    for(auto it=defaultColors().constBegin();it!=defaultColors().constEnd();++it)
        colors.insert(it.key(),it.value().name());
    QJsonArray rules;
    for(const IconRow& row:defaultIcons()){
        QJsonObject rule;
        rule.insert(QStringLiteral("tag"),QString::fromUtf8(row.tag));
        rule.insert(QStringLiteral("icon"),QString::fromUtf8(row.icon));
        rules.append(rule);
    }
    auto addRegex=[&rules](const QString& pattern,const QString& icon,const QString& colorFrom){
        QJsonObject rule;
        rule.insert(QStringLiteral("match"),pattern);
        if(!icon.isEmpty())
            rule.insert(QStringLiteral("icon"),icon);
        if(!colorFrom.isEmpty())
            rule.insert(QStringLiteral("color_from"),colorFrom);
        rules.append(rule);
    };
    addRegex(QStringLiteral("^(white|black|grey|gray|silver|blonde|brown|red|pink|purple|blue|aqua|green|orange) (hair|eyes)$"),
             QString(),QStringLiteral("group1"));
    addRegex(QStringLiteral("(dress)$"),QStringLiteral("👗"),QString());
    addRegex(QStringLiteral("(skirt)$"),QStringLiteral("👗"),QString());
    addRegex(QStringLiteral("(shirt)$"),QStringLiteral("👕"),QString());
    addRegex(QStringLiteral("(pants)$"),QStringLiteral("👖"),QString());
    addRegex(QStringLiteral("(swimsuit|bikini)$"),QStringLiteral("👙"),QString());
    addRegex(QStringLiteral("(socks)$"),QStringLiteral("🧦"),QString());
    addRegex(QStringLiteral("(shoes)$"),QStringLiteral("👠"),QString());
    addRegex(QStringLiteral("(hat)$"),QStringLiteral("🎩"),QString());
    addRegex(QStringLiteral("(glasses)$"),QStringLiteral("👓"),QString());
    addRegex(QStringLiteral("(weapon|sword|knife)$"),QStringLiteral("🗡️"),QString());
    addRegex(QStringLiteral("(cat ears)$"),QStringLiteral("🐱"),QString());

    QJsonObject root;
    root.insert(QStringLiteral("colors"),colors);
    root.insert(QStringLiteral("rules"),rules);
    return root;
}

int iconWidth(const QFontMetrics& metrics,const TagIconSpec& icon)
{
    if(icon.isImage())
        return kIconSize;
    return std::max(kIconSize,metrics.horizontalAdvance(icon.value));
}

} // namespace

QColor tagBorderColor(const QColor& color,int alpha)
{
    float h=0,s=0,l=0,a=0;
    color.getHslF(&h,&s,&l,&a);
    // Pure black is a special case: lifting it to grey still looks muddy on the
    // black row, so it gets the same white border as every other bright tag.
    if(l<0.22f)
        return QColor(0xf2,0xf2,0xf2);
    if(l<0.36f)
        l=0.42f;
    QColor lifted=QColor::fromHslF(h,s,l,1.0);
    lifted.setAlpha(alpha);
    return lifted;
}

bool TagIconSpec::isImage() const
{
    const QString lower=value.trimmed().toLower();
    return lower.endsWith(QLatin1String(".png"))||lower.endsWith(QLatin1String(".ico"))
           ||lower.endsWith(QLatin1String(".jpg"))||lower.endsWith(QLatin1String(".jpeg"));
}

TagMarkRegistry& TagMarkRegistry::instance()
{
    static TagMarkRegistry registry;
    return registry;
}

void TagMarkRegistry::loadDefaults()
{
    m_colors=defaultColors();
    m_rules.clear();
    for(const IconRow& row:defaultIcons()){
        Rule rule;
        rule.tag=QString::fromUtf8(row.tag);
        rule.icon=QString::fromUtf8(row.icon);
        m_rules.append(rule);
    }
    auto addRegex=[this](const QString& pattern,const QString& icon,const QString& colorFrom){
        Rule rule;
        rule.regex=QRegularExpression(pattern,QRegularExpression::CaseInsensitiveOption);
        rule.icon=icon;
        rule.colorFrom=colorFrom;
        m_rules.append(rule);
    };
    addRegex(QStringLiteral("^(white|black|grey|gray|silver|blonde|brown|red|pink|purple|blue|aqua|green|orange) (hair|eyes)$"),
             QString(),QStringLiteral("group1"));
    addRegex(QStringLiteral("(dress)$"),QStringLiteral("👗"),QString());
    addRegex(QStringLiteral("(skirt)$"),QStringLiteral("👗"),QString());
    addRegex(QStringLiteral("(shirt)$"),QStringLiteral("👕"),QString());
    addRegex(QStringLiteral("(pants)$"),QStringLiteral("👖"),QString());
    addRegex(QStringLiteral("(swimsuit|bikini)$"),QStringLiteral("👙"),QString());
    addRegex(QStringLiteral("(socks)$"),QStringLiteral("🧦"),QString());
    addRegex(QStringLiteral("(shoes)$"),QStringLiteral("👠"),QString());
    addRegex(QStringLiteral("(hat)$"),QStringLiteral("🎩"),QString());
    addRegex(QStringLiteral("(glasses)$"),QStringLiteral("👓"),QString());
    addRegex(QStringLiteral("(weapon|sword|knife)$"),QStringLiteral("🗡️"),QString());
    addRegex(QStringLiteral("(cat ears)$"),QStringLiteral("🐱"),QString());
}

void TagMarkRegistry::load(const QString& filePath)
{
    m_path=filePath.isEmpty()
               ? QCoreApplication::applicationDirPath()+QStringLiteral("/data/decorations.json")
               : filePath;
    m_loaded=true;
    loadDefaults();

    QFileInfo info(m_path);
    if(!info.exists()){
        if(!info.absoluteDir().exists())
            QDir().mkpath(info.absolutePath());
        writeDefaults(m_path);
        return;
    }

    QFile file(m_path);
    if(!file.open(QIODevice::ReadOnly)){
        tewarn(QStringLiteral("[decorations] cannot read %1").arg(m_path));
        return;
    }
    QJsonParseError error{};
    const QJsonDocument doc=QJsonDocument::fromJson(file.readAll(),&error);
    if(error.error!=QJsonParseError::NoError||!doc.isObject()){
        tewarn(QStringLiteral("[decorations] %1: %2").arg(m_path,error.errorString()));
        return;                         // keep the built-in defaults
    }

    const QJsonObject root=doc.object();
    const QJsonObject colors=root.value(QStringLiteral("colors")).toObject();
    for(auto it=colors.constBegin();it!=colors.constEnd();++it){
        const QColor color(it.value().toString());
        if(color.isValid())
            m_colors.insert(it.key(),color);
    }

    // File rules go first: the first matching rule wins, so a user rule must be
    // able to override a built-in one.
    QVector<Rule> fileRules;
    for(const QJsonValue& value:root.value(QStringLiteral("rules")).toArray()){
        const QJsonObject obj=value.toObject();
        Rule rule;
        rule.tag=obj.value(QStringLiteral("tag")).toString();
        rule.colorFrom=obj.value(QStringLiteral("color_from")).toString();
        rule.icon=obj.value(QStringLiteral("icon")).toString();
        const QString pattern=obj.value(QStringLiteral("match")).toString();
        if(!pattern.isEmpty()){
            rule.regex=QRegularExpression(pattern,QRegularExpression::CaseInsensitiveOption);
            if(!rule.regex.isValid()){
                tewarn(QStringLiteral("[decorations] bad regex: %1").arg(pattern));
                continue;
            }
        }
        if(rule.tag.isEmpty()&&pattern.isEmpty())
            continue;
        fileRules.append(rule);
    }
    m_rules=fileRules+m_rules;
}

bool TagMarkRegistry::writeDefaults(const QString& filePath)
{
    QSaveFile file(filePath);
    if(!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(defaultsJson()).toJson(QJsonDocument::Indented));
    return file.commit();
}

QColor TagMarkRegistry::colorFor(const QString& name) const
{
    return m_colors.value(name.trimmed().toLower());
}

void TagMarkRegistry::applyColorName(const QString& name,TagVisual& visual) const
{
    if(isMultiColor(name)){
        visual.hasColor=true;
        visual.gradient=true;
        visual.striped=false;
        visual.streaked=false;
        visual.rawStreak=false;
        visual.hasBase=false;
        visual.whiteDominant=false;
        visual.gradientStops=gradientFor(name);
        visual.border=visual.gradientStops.value(0);
        visual.fill=withAlpha(visual.border,41);
        return;
    }
    const QColor color=colorFor(name);
    if(!color.isValid())
        return;
    applySolidColor(color,visual);
}

void TagMarkRegistry::applySolidColor(const QColor& color,TagVisual& visual) const
{
    if(!color.isValid())
        return;
    visual.hasColor=true;
    visual.gradient=false;
    visual.striped=false;
    visual.streaked=false;
    visual.rawStreak=false;
    visual.gradientStops.clear();
    visual.hasBase=false;
    visual.whiteDominant=color.lightnessF()>=0.90f;
    visual.border=tagBorderColor(color);   // 0.85, lifted when too dark
    // A black tag keeps the row's black background: a tinted black would be
    // *lighter* than a normal row, which is exactly backwards.
    visual.fill=color.lightnessF()<0.25f?QColor(0,0,0,0)
                 :(visual.whiteDominant?QColor(0xf2,0xf2,0xf2):withAlpha(color,41));
}

void TagMarkRegistry::applyOverrides(const QString& text,TagVisual& visual) const
{
    for(const TagMarkOverride& item:m_overrides){
        if(item.tag.isEmpty()||item.tag.compare(text,Qt::CaseInsensitive)!=0)
            continue;
        if(!item.color.trimmed().isEmpty()){
            const QString key=item.color.trimmed();
            QColor color=colorFor(key.toLower());
            if(!color.isValid()){
                if(isMultiColor(key.toLower()))
                    applyColorName(key.toLower(),visual);
                else if((color=QColor(key)).isValid())
                    applySolidColor(color,visual);
            }else{
                applySolidColor(color,visual);
            }
        }
        if(!item.icon.trimmed().isEmpty()){
            // The user's icon goes first: it is what they added the override for.
            const QString icon=item.icon.trimmed();
            for(int i=int(visual.icons.size())-1;i>=0;--i)
                if(visual.icons[i].value==icon)
                    visual.icons.removeAt(i);
            visual.icons.prepend(TagIconSpec{icon,QString()});
        }
    }
}

QStringList TagMarkRegistry::colorsInText(const QString& text) const
{
    QStringList colors;
    // Words, with the punctuation tags may carry around them ("(red)", "blue,").
    static const QRegularExpression separator(QStringLiteral("[\\s,.;:()\\[\\]]+"));
    for(const QString& word:text.toLower().split(separator,Qt::SkipEmptyParts))
        if(m_colors.contains(word)&&!colors.contains(word))
            colors.append(word);
    return colors;
}

void TagMarkRegistry::applyTextPattern(const QString& text,TagVisual& visual) const
{
    const QString lower=text.toLower();
    const QStringList colorNames=colorsInText(text);
    auto toColors=[](const QStringList& names) {
        QVector<QColor> out;
        for(const QString& name:names){
            const QColor color=TagMarkRegistry::instance().colorFor(name);
            if(color.isValid())
                out.append(color);
        }
        return out;
    };
    auto isWhite=[](const QColor& color){ return color.lightnessF()>=0.90f; };

    // Merged streak: "yellow hair with blue and green streaks". The first colour
    // is the base, everything after "with" is a streak colour.
    static const QRegularExpression merged(
        QStringLiteral("(.*?)\\bhair\\s+with\\s+(.*?)\\bstreaks?\\b"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch mergedMatch=merged.match(text);
    if(mergedMatch.hasMatch()){
        const QStringList baseNames=colorsInText(mergedMatch.captured(1));
        const QVector<QColor> streakColors=toColors(colorsInText(mergedMatch.captured(2)));
        if(!baseNames.isEmpty()&&!streakColors.isEmpty()){
            const QColor base=colorFor(baseNames.last());
            if(base.isValid()){
                visual.hasColor=true;
                visual.streaked=true;
                visual.rawStreak=false;
                visual.striped=false;
                visual.gradient=false;
                visual.hasBase=true;
                visual.baseColor=base;
                visual.gradientStops=streakColors;
                visual.whiteDominant=isWhite(base);
                visual.border=tagBorderColor(base);
                visual.fill=visual.whiteDominant?QColor(0xf2,0xf2,0xf2):withAlpha(base,41);
                return;
            }
        }
    }

    // Raw streak: "green streaked hair" (no base colour yet). Only the streak
    // cells are painted; the rest of the row keeps the normal background.
    if((lower.contains(QLatin1String("streaked"))||lower.contains(QLatin1String("streaks")))
        &&!lower.contains(QLatin1String("with"))&&!lower.contains(QLatin1String("striped"))){
        const QVector<QColor> streakColors=toColors(colorNames);
        if(!streakColors.isEmpty()){
            visual.hasColor=true;
            visual.streaked=true;
            visual.rawStreak=true;
            visual.striped=false;
            visual.gradient=false;
            visual.hasBase=false;
            visual.gradientStops=streakColors;
            visual.whiteDominant=false;
            visual.border=tagBorderColor(streakColors.first());
            visual.fill=QColor(0,0,0,0);
            return;
        }
    }

    // Striped clothing: "blue and white striped panties".
    if(lower.contains(QLatin1String("striped"))){
        const QVector<QColor> bands=toColors(colorNames);
        if(bands.size()>=2){
            visual.hasColor=true;
            visual.striped=true;
            visual.streaked=false;
            visual.gradient=false;
            visual.hasBase=false;
            visual.gradientStops=bands;
            visual.whiteDominant=isWhite(bands.first());
            visual.border=tagBorderColor(bands.first());
            visual.fill=visual.whiteDominant?QColor(0xf2,0xf2,0xf2):withAlpha(bands.first(),41);
            return;
        }
    }

    // A plain colour word ("blue dress", "red eyes") is a colour too; the hair/eyes
    // rules only cover "colour hair", so clothes used to stay uncoloured.
    if(colorNames.size()==1){
        applyColorName(colorNames.first(),visual);
        return;
    }

    const bool wantsGradient=lower.contains(QLatin1String("gradient"))
                             ||lower.contains(QLatin1String("rainbow"))
                             ||lower.contains(QLatin1String("multicolor"))
                             ||lower.contains(QLatin1String("multi-color"))
                             ||lower.contains(QLatin1String("two-tone"));
    if(!wantsGradient)
        return;

    QVector<QColor> stops=toColors(colorNames);
    if(stops.isEmpty())
        stops=gradientFor(lower.contains(QLatin1String("two-tone"))
                              ?QStringLiteral("two-tone"):QStringLiteral("rainbow"));
    if(stops.size()<2)
        return;
    visual.hasColor=true;
    visual.gradient=true;
    visual.striped=false;
    visual.streaked=false;
    visual.hasBase=false;
    visual.gradientStops=stops;
    visual.whiteDominant=isWhite(stops.first());
    visual.border=tagBorderColor(stops.first());
    visual.fill=visual.whiteDominant?QColor(0xf2,0xf2,0xf2):withAlpha(stops.first(),41);
}

void TagMarkRegistry::applyRules(const QString& text,TagVisual& visual) const
{
    auto addIcon=[&visual](const QString& icon){
        if(icon.isEmpty())
            return;
        for(const TagIconSpec& existing:visual.icons)
            if(existing.value==icon)
                return;
        visual.icons.append({icon,QString()});
    };

    // Exact rules first (MARK-06). Every exact icon is kept: a tag may carry
    // several prefix emojis, and the row shrinks them to fit.
    for(const Rule& rule:m_rules){
        if(rule.tag.isEmpty()||rule.tag!=text)
            continue;
        addIcon(rule.icon);
    }
    for(const Rule& rule:m_rules){
        if(rule.tag.isEmpty()||rule.tag!=text)
            continue;
        if(!visual.hasColor&&!rule.colorFrom.isEmpty())
            applyColorName(rule.colorFrom,visual);
    }

    // Regex rules in array order; colour and icon may come from different rules.
    bool colorSet=visual.hasColor;
    for(const Rule& rule:m_rules){
        if(!rule.tag.isEmpty()||rule.regex.pattern().isEmpty())
            continue;

        const QRegularExpressionMatch match=rule.regex.match(text);
        if(!match.hasMatch())
            continue;
        if(!colorSet&&!rule.colorFrom.isEmpty()){
            // "group1" means capture group 1 (MARK-06); a plain number works
            // too, and anything else is taken as a literal colour name.
            QString captured;
            const QString key=rule.colorFrom.trimmed();
            if(key.startsWith(QLatin1String("group"))){
                bool ok=false;
                const int index=key.mid(5).toInt(&ok);
                if(ok&&index>0)
                    captured=match.captured(index);
            }else{
                bool ok=false;
                const int index=key.toInt(&ok);
                captured=ok&&index>0?match.captured(index):key;
            }
            if(!captured.isEmpty()){
                applyColorName(captured,visual);
                colorSet=visual.hasColor;
            }
        }
        addIcon(rule.icon);
    }
}

TagVisual TagMarkRegistry::decorate(const teTag& tag) const
{
    if(!m_loaded)
        const_cast<TagMarkRegistry*>(this)->load();
    TagVisual visual;
    // The tag text itself is the decoration source ("gradient from a to b",
    // "red hair with blue streaks", "blue dress"). The editor control calls this
    // once per tag and stores the result; painting never runs the rules.
    const QString text=static_cast<QString>(tag);
    applyTextPattern(text,visual);
    applyRules(text,visual);
    // The settings dialog is the user's last word: it overrides the rules above.
    applyOverrides(text,visual);
    return visual;
}

TagVisual TagMarkRegistry::resolve(const teTag& tag) const
{
    // Only what an editor control wrote. A tag without a decoration control is
    // deliberately plain - decoration is not a paint-time global any more.
    TagVisual visual=tag.decorationSet?tag.decoration:TagVisual();
    for(const QString& owner:tag.markOrder){
        const QString mark=tag.marks.value(owner);
        if(mark.isEmpty())
            continue;
        if(isMultiColor(mark)||m_colors.contains(mark.trimmed().toLower()))
            applyColorName(mark,visual);
        else
            visual.icons.append({mark,QString()});
    }
    return visual;
}

void refreshTagDecoration(const std::shared_ptr<teTag>& tag)
{
    if(!tag||tag->retired)
        return;
    tag->decoration=TagMarkRegistry::instance().decorate(*tag);
    tag->decorationSet=!tag->decoration.isEmpty();
}

void ensureTagDecoration(const std::shared_ptr<teTag>& tag)
{
    if(!tag||tag->retired||tag->decorationSet)
        return;
    refreshTagDecoration(tag);
}

TagRowLayout layoutTagRow(const QRect& rowRect,const TagVisual& visual,
                          const QFontMetrics& metrics)
{
    Q_UNUSED(metrics);
    TagRowLayout layout;
    // Icons sit close to the border and to each other; the old 4+3px insets plus
    // a 15pt advance made one emoji push the text a whole chip-width away.
    constexpr int inset=2;
    constexpr int gap=1;
    const int count=visual.icons.size();
    int iconPx=16;
    if(count>=2) iconPx=13;
    if(count>=3) iconPx=12;
    if(count>=4) iconPx=std::max(9,12-(count-3));
    const int chipWidth=iconPx+2;

    int x=rowRect.left()+inset;
    const int top=rowRect.top()+(rowRect.height()-18)/2;
    for(int i=0;i<count;++i){
        const TagIconSpec& icon=visual.icons[i];
        if(icon.value.isEmpty())
            continue;
        TagChip chip;
        chip.id=QStringLiteral("icon:%1").arg(i);
        chip.icon=icon.value;
        chip.tooltip=icon.tooltip;
        chip.iconPx=iconPx;
        chip.rect=QRect(x,top,chipWidth,18);
        layout.chips.append(chip);
        x+=chipWidth+gap;
    }

    const int textLeft=layout.chips.isEmpty()?rowRect.left()+inset:x+1;
    layout.textRect=QRect(textLeft,rowRect.top(),
                          std::max(0,rowRect.right()-textLeft-inset),
                          rowRect.height());
    return layout;
}

