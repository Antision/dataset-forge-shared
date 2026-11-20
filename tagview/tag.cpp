#include "tag.h"
#include "log.h"

#include <QRegularExpression>

#include <algorithm>

int teTagList::initialize_push_back(const QString& str, bool forceSentence)
{
    return initialize_push_back(std::make_shared<teTag>(str, forceSentence));
}

int teTagList::initialize_push_back(const std::string &in, bool forceSentence)
{
    return initialize_push_back(QString::fromStdString(in), forceSentence);
}

int teTagList::initialize_push_back(std::shared_ptr<teTag> tag)
{
    if (remove_duplicate(tag, false)) {
        return -1;
    } else {
        tags.append(tag);
        connectTag(tag);
        // Building a list (a caption being loaded) is not an edit: it must not
        // land on the undo stack, and the state the first real edit starts from
        // has to include it. The old code pushed an "edit" record and advanced
        // inip past it, which is the same statement with more moving parts.
        syncUndoBaseline();
        return tags.size() - 1;
    }
}

bool teTagList::remove_duplicate(std::shared_ptr<teTag> tag, bool keepself)
{
    std::lock_guard<std::recursive_mutex> lg(tagsMt);

    if (tag == nullptr && keepself == true)
        telog("[teTagList::remove_duplicate]:tag==nullptr&&keepself==true");

    bool ret = false;
    if (tag != nullptr) {
        for (int i = 0; i < tags.size(); ) {
            if (*tags[i] == *tag && tags[i] != tag) {
                if (keepself) {
                    erase(i);           // erase() already shrinks the list
                    ret = true;
                    continue;
                }
                return true;
            }
            ++i;
        }
        return ret;
    }

    for (int i = 0; i < tags.size(); ++i) {
        for (int j = i + 1; j < tags.size(); ) {
            if (*tags[i] == *tags[j]) {
                erase(j);
                ret = true;
                continue;
            }
            ++j;
        }
    }
    return ret;
}

std::shared_ptr<teTag> teTagList::takeErased(int id)
{
    std::lock_guard<std::recursive_mutex> lg(tagsMt);
    if (id < 0 || id >= tags.size()) {
        telog(QString("[teTagList::erase] index %1 out of range (size %2)").arg(id).arg(tags.size()));
        return nullptr;
    }
    std::shared_ptr<teTag> core = tags.takeAt(id);
    // The undo step is recorded now, but nothing is announced: the announcement
    // (notifyErased) runs the editors, and an editor may merge - which erases
    // further tags. That must not happen while a model sits between
    // beginRemoveRows() and endRemoveRows(): a nested structural change there
    // corrupts Qt's persistent indexes (the "persistent model indexes corrupted"
    // assert). The model calls notifyErased() once it is consistent again.
    onTagErased(core, id, false);
    return core;
}

void teTagList::notifyErased(std::shared_ptr<teTag> core,int id,bool ifemit)
{
    Q_UNUSED(id);
    if(!core)
        return;
    if(ifemit){
        emit tagErased(core);
        teemit(teCallbackType::edit);
    }
    core->teDisconnect(this);
    core->teemit(teCallbackType::destroy);
}

void teTagList::erase(int id)
{
    std::shared_ptr<teTag> core = takeErased(id);
    if(core)
        notifyErased(core,id,isTagsLoaded);
}

int teTagList::edit(std::shared_ptr<teTag> core, QString text, int removeDuplicate, bool ifemit)
{
    if (!core)
        return -1;

    {
        // Whether this is a change the user made - asked before the suppressor
        // below turns recording off for the duration of the call. A normalisation
        // pass (a list being read) rewrites words too, and that must not count as
        // the user touching the tag.
        const bool userGesture = recordingChanges();
        // read() announces the change itself (edit_with_layout), which is what
        // makes the tag widgets, the editors and the list view react at once -
        // an in-place text change used to be visible in the list only after the
        // next repaint. The list records and re-emits it below instead, so its
        // own reaction to that signal is suppressed while it runs.
        ChangeSuppressor suppressor(this);
        std::lock_guard<std::recursive_mutex> lg(tagsMt);
        // Editing a tag - even entering the editor and confirming without changing
        // a word - is the user taking it back: the automatic merge that produced it
        // may happen again (see teTag::mergeSuppressed).
        if(userGesture)
            core->mergeSuppressed=false;
        core->read(text);
    }

    if (removeDuplicate == 1 || removeDuplicate == 2) {
        // Merging: the tag the user just renamed wins, every identical tag is
        // erased. Returning early here (as the old code did) is exactly what
        // left two identical tags in the list.
        remove_duplicate(core, true);
    }

    onTagEdited(core, ifemit);
    return core->retired ? -1 : 0;
}

int teTagList::edit(int index, QString text, int removeDuplicate, bool ifemit)
{
    std::shared_ptr<teTag> core;
    {
        std::lock_guard<std::recursive_mutex> lg(tagsMt);
        if (index < 0 || index >= tags.size()) {
            telog(QString("[teTagList::edit] index %1 out of range (size %2)").arg(index).arg(tags.size()));
            return -1;
        }
        core = tags.at(index);
    }
    return edit(core, text, removeDuplicate, ifemit);
}

int teTagList::insert(int pos, std::shared_ptr<teTag> tag, int removeDuplicate, bool ifSendSignal)
{
    if (!tag)
        return -1;
    if (removeDuplicate == 1 && remove_duplicate(tag, false))
        return -1;
    // Undo of an erase puts the tag back: it is alive again, so the retirement
    // marker has to go with it (a tag that sits in the list while marked would be
    // removed by the next purge, and its editors would let go of it).
    tag->retired = false;

    connectTag(tag);
    {
        std::lock_guard<std::recursive_mutex> lg(tagsMt);
        pos = std::clamp(pos, 0, int(tags.size()));
        tags.insert(pos, tag);
    }
    if (removeDuplicate == 2)
        remove_duplicate(tag, true);

    // Announce the insertion *after* the list is consistent; the old code
    // emitted before inserting, so a slot that inspected the list saw the tag
    // missing (and a rejected insertion was announced as well).
    onTagInserted(tag, pos, ifSendSignal);
    return 0;
}

void teTagList::move(int originPos, int newPos)
{
    std::shared_ptr<teTag> taketag;
    {
        std::lock_guard<std::recursive_mutex> lg(tagsMt);
        if (originPos < 0 || originPos >= tags.size()) {
            telog(QString("[teTagList::move] origin %1 out of range (size %2)").arg(originPos).arg(tags.size()));
            return;
        }
        taketag = tags.takeAt(originPos);
        newPos = std::clamp(newPos, 0, int(tags.size()));
        tags.insert(newPos, taketag);
    }
    onTagMoved(taketag, originPos, newPos);
}

void teTagList::move(std::shared_ptr<teTag> tag, int newPos)
{
    const int o = tags.indexOf(tag);
    if (o < 0) {
        telog("[teTagList::move]:could not find input tag in taglist");
        return;
    }
    move(o, newPos);
}

void teTagList::reorderBlock(int from, int count, int insertAt)
{
    if (count <= 0)
        return;

    QVector<std::shared_ptr<teTag>> block;
    {
        std::lock_guard<std::recursive_mutex> lg(tagsMt);
        if (from < 0 || from + count > int(tags.size())) {
            telog(QString("[teTagList::reorderBlock] block %1+%2 out of range (size %3)")
                      .arg(from).arg(count).arg(tags.size()));
            return;
        }
        block.reserve(count);
        for (int i = 0; i < count; ++i)
            block.append(tags.takeAt(from));
        insertAt = std::clamp(insertAt, 0, int(tags.size()));
        for (int i = 0; i < count; ++i)
            tags.insert(insertAt + i, block[i]);
    }

    isSaved = false;
    // One step for the whole block move, not one per row: a multi-row drag is a
    // single gesture, and undoing half of it makes no sense.
    ChangeRecorder recorder(this);
    emit tagMoved(block.first());
}

QString teTagList::toText()
{
    return serializePieces(size(), [this](int i) -> std::shared_ptr<teTag> {
        return tags[i];
    }, true);
}

bool teTagList::takeUndoSnapshot(teTagSnapshotList& out)
{
    // A gesture that is still open belongs to this undo: a click followed by
    // Ctrl+Z before the event loop came back is one step, not zero.
    commitChange();
    const teUndoHistory::Step* step = history.stepToUndo();
    if (!step)
        return false;
    out = step->before;
    history.stepUndone();
    return true;
}

bool teTagList::takeRedoSnapshot(teTagSnapshotList& out)
{
    commitChange();
    const teUndoHistory::Step* step = history.stepToRedo();
    if (!step)
        return false;
    out = step->after;
    history.stepRedone();
    return true;
}

teTag::teTag(const QList<teWord *> in){
    for(teWord*w:in)
        words.push_back(new teWord(w->text));
}

teTag::teTag(const QString &str, bool forceSentence){
    // The flag used to be dropped here, so a tag built as a sentence came out as
    // a plain tag and was then edited in the wrong window.
    read(str,true,forceSentence);
}

teTag::teTag(const char *str, bool forceSentence){
    read(QString(str),true,forceSentence);
}

teTag::teTag(teTag &&in):words(std::move(in.words)){
    info=in.info;
    marks=std::move(in.marks);
    markOrder=std::move(in.markOrder);
    decoration=std::move(in.decoration);
    decorationSet=in.decorationSet;
}

teTag &teTag::operator=(const teTag &in){
    if(!words.empty()){
        for(teWord*wc:words)
            delete wc;
        words.clear();
    }
    for(teWord*wc:in.words)
        words.push_back(new teWord{*wc});
    weight=in.weight;
    marks=in.marks;
    markOrder=in.markOrder;
    decoration=in.decoration;
    decorationSet=in.decorationSet;
    edited();
    return *this;
}

void teTag::read(const QString &str, bool ifclear, bool forceSentence)
{
    if (ifclear)
        clear();

    QString text = str.trimmed();

    // Force sentence mode: used for the result of openpath / splitTextToPieces
    if (forceSentence) {
        if (!text.endsWith('.'))
            text += '.';

        words.push_back(new teWord(text));
        type = sentence;

        if (ifclear)
            edited_with_layout();
        return;
    }

    // Manually adding a tag: if it ends with a period, treat it as a sentence
    if (text.endsWith('.')) {
        words.push_back(new teWord(text));
        type = sentence;

        if (ifclear)
            edited_with_layout();
        return;
    }

    // Logic for normal tags
    QStringList result;
    QStringList wordlist;

    if (text.indexOf("(") != -1 || text.indexOf(")") != -1) {
        QRegularExpression regex(R"(\\\(|\(|\\\)|\))");
        int lastIndex = 0;
        QRegularExpressionMatchIterator it = regex.globalMatch(text);
        while (it.hasNext()) {
            QRegularExpressionMatch match = it.next();
            int start = match.capturedStart();
            if (start > lastIndex)
                result.append(text.mid(lastIndex, start - lastIndex));
            result.append(match.captured());
            lastIndex = match.capturedEnd();
        }
        if (lastIndex < text.length())
            result.append(text.mid(lastIndex));
    } else {
        result = {text};
    }

    for (QString s : result)
        wordlist.append(s.split(" ", Qt::SkipEmptyParts));

    for (QString &tmpword : wordlist)
        words.push_back(new teWord(std::move(tmpword)));

    type = tag;

    if (ifclear)
        edited_with_layout();   // read() rebuilds the words; the list listens to this one
}

teWord *teTag::takeWordAt(int index, bool ifSendSignal){
    while(index<0)index += words.size();
    teWord* wc = words.takeAt(index);
    // The word widget (if the list that shows this tag owns one) is rebuilt from
    // the core by the edited_with_layout() below, so nothing has to be unplugged
    // here any more: the tag does not know about widgets at all.
    if(ifSendSignal)
        edited_with_layout();
    return wc;
}

bool teTag::operator==(const teTag &in) const{
    if(this->words.size()!=in.words.size()) return false;
    else{
        int size = words.size();
        for(int i =0;i<size;++i){
            if(words[i]->text!=in.words[i]->text)
                return false;
        }
        return true;
    }
}

teTag::operator QString() const
{
    QString out;

    auto isOpenParen = [](const QString& s) {
        return s == "(" || s == R"(\()";
    };

    auto isCloseParen = [](const QString& s) {
        return s == ")" || s == R"(\))";
    };

    QString prev;
    bool first = true;

    for (teWord* word : words) {
        if (!word)
            continue;

        const QString cur = word->text;

        if (first) {
            out += cur;
            prev = cur;
            first = false;
            continue;
        }

        // 唯一不加空格的两种情况：
        // 1. 左括号后
        // 2. 右括号前
        if (!isOpenParen(prev) && !isCloseParen(cur))
            out += ' ';

        out += cur;
        prev = cur;
    }

    return out;
}

teTag::~teTag(){
    onDestroy();
    clear();
}

teWord::~teWord(){
    // Nothing to release: the tag widget owns its word widgets (they are its
    // layout's children), so a word is data plus signals.
}

QList<ParsedPiece> splitTextToPieces(const QString& raw)
{
    QList<ParsedPiece> out;

    QString text = raw;
    text.replace("\r\n", "\n");
    text.replace('\r', '\n');

    const QStringList lines =
        text.split('\n', Qt::KeepEmptyParts);

    // 判断当前位置的 "..." 是否属于连续三个点。
    // 只要这里是 "...", 就绝不能把其中任何一个 '.' 当成句号。
    auto isEllipsis = [](const QString& line, int pos) -> bool {
        return pos >= 0 &&
               pos + 3 <= line.size() &&
               line.mid(pos, 3) == "...";
    };

    // 找真正的句号。
    //
    // 例如：
    //   "abc."       -> 找到
    //   "d..."       -> 找不到
    //   "..."        -> 找不到
    //   "abc...,def" -> 找不到
    auto findSentenceDot =
        [&](const QString& line, int start) -> int
    {
        int pos = start;

        while (pos < line.size()) {
            if (line[pos] != '.') {
                ++pos;
                continue;
            }

            // "...": 整组三个点跳过去
            if (isEllipsis(line, pos)) {
                pos += 3;
                continue;
            }

            // 单独的 '.' 才是真正的句号
            return pos;
        }

        return -1;
    };

    for (QString line : lines) {
        line = line.trimmed();

        if (line.isEmpty())
            continue;

        /*
         * 先判断这一行有没有真正的句号。
         *
         * 没有真正句号：
         *     完全按照普通 tag 处理。
         *
         * 例如：
         *     ...,a,b,d...,...
         *
         * 得到：
         *     ...
         *     a
         *     b
         *     d...
         *     ...
         */
        const int firstDot =
            findSentenceDot(line, 0);

        if (firstDot < 0) {
            const QStringList parts =
                line.split(',', Qt::SkipEmptyParts);

            for (QString part : parts) {
                part = part.trimmed();

                if (!part.isEmpty())
                    out.push_back({part, false});
            }

            continue;
        }

        /*
         * 这一行存在真正的句号，
         * 因此按照 sentence 解析。
         *
         * 注意：
         * sentence 内部的逗号全部保留，
         * 不能再用 ',' 拆分。
         */
        int start = 0;

        while (start < line.size()) {
            const int dot =
                findSentenceDot(line, start);

            if (dot < 0) {
                QString tail =
                    line.mid(start).trimmed();

                if (!tail.isEmpty()) {
                    const QStringList parts =
                        tail.split(',', Qt::SkipEmptyParts);

                    for (QString part : parts) {
                        part = part.trimmed();

                        if (!part.isEmpty())
                            out.push_back({part, false});
                    }
                }

                break;
            }

            QString piece =
                line.mid(start, dot - start + 1).trimmed();

            if (!piece.isEmpty())
                out.push_back({piece, true});

            start = dot + 1;

            while (start < line.size() &&
                   line[start].isSpace()) {
                ++start;
            }
        }
    }

    return out;
}
