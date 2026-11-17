#include "audio.h"

#include <QAudioOutput>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QSettings>
#include <QUrl>
#include <QApplication>

#include <atomic>

namespace {

/*
 * 和 preview 窗口同一份配置文件(exe 同目录 config.ini,方便用户直接改)。
 * preview 的音量在 [preview] volume,主音量放 [app] masterVolume。
 */
QString configFilePath()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/config.ini");
}

/* -1 = 还没从配置里加载过 */
std::atomic<int> g_masterVolume{-1};

int loadMasterFromConfig()
{
    QSettings cfg(configFilePath(), QSettings::IniFormat);
    return qBound(0, cfg.value(QStringLiteral("app/masterVolume"), 70).toInt(), 100);
}

/*
 * 每个 Sound 一条通路:懒创建、常驻。不同声音用不同的播放器,互不打断
 * (日志的 Warning 响到一半又来一条,就重播同一条;去重的批量音效
 *  不会把日志的警告音顶掉)。
 */
struct SoundPlayer {
    QMediaPlayer *player = nullptr;
    QAudioOutput *audio = nullptr;
};

struct SoundEntry {
    appaudio::Sound kind;
    const char *file;
};

/* 声音表:一种 Sound 一行。 */
const SoundEntry kSoundTable[] = {
    { appaudio::Sound::DatasetSuccess, "vadim_makes_sound-modern-success-confirmation-notification-566072.mp3" },
    { appaudio::Sound::DatasetWarning, "vadim_makes_sound-soft-ui-warning-alert-1-614153.mp3" },
    { appaudio::Sound::Warning,        "universfield-warning-notification-199277.mp3" },
    { appaudio::Sound::Error,          "universfield-error-notification-012-132111.mp3" },
    { appaudio::Sound::SingleAction,   "dragon-studio-pop-402324.mp3" },
    { appaudio::Sound::BatchAction,    "dragon-studio-bubble-pop-406640.mp3" },
    { appaudio::Sound::NothingRemoved, "sfxmint-mahjong-04.mp3" },
};

constexpr int kSoundCount = int(appaudio::Sound::Count);

int soundIndex(appaudio::Sound kind)
{
    const int i = int(kind);
    return (i >= 0 && i < kSoundCount) ? i : 0;
}

SoundPlayer &soundPlayer(appaudio::Sound kind)
{
    static SoundPlayer s_players[kSoundCount];
    return s_players[soundIndex(kind)];
}

/*
 * 素材位置:先 exe 目录的 sounds\(用户可以替换同名文件换音效,这是 gi 一直
 * 保留的用法),再回落到编进 exe 的 qrc(没有素材目录也不会只剩 beep)。
 */
QString soundSource(appaudio::Sound kind)
{
    const int i = soundIndex(kind);
    const QString local = QCoreApplication::applicationDirPath() + QStringLiteral("/sounds/")
                          + QString::fromLatin1(kSoundTable[i].file);
    if (QFileInfo::exists(local))
        return local;
    return QStringLiteral("qrc:/audio/sounds/") + QString::fromLatin1(kSoundTable[i].file);
}

} // namespace

namespace appaudio {

int masterVolume()
{
    // 两个线程同时首读也只是各算一遍,写进去的都是有效值,不值得为此上锁
    int v = g_masterVolume.load(std::memory_order_acquire);
    if (v < 0) {
        v = loadMasterFromConfig();
        g_masterVolume.store(v, std::memory_order_release);
    }
    return v;
}

void setMasterVolume(int percent)
{
    const int v = qBound(0, percent, 100);
    g_masterVolume.store(v, std::memory_order_release);
    QSettings cfg(configFilePath(), QSettings::IniFormat);
    cfg.setValue(QStringLiteral("app/masterVolume"), v);
}

void playSound(Sound kind)
{
    const QString source = soundSource(kind);
    if (!QFileInfo::exists(source)) {
        QApplication::beep();        // 素材不在:至少还有个响
        return;
    }

    SoundPlayer &sp = soundPlayer(kind);
    if (!sp.player) {
        sp.player = new QMediaPlayer(QCoreApplication::instance());
        sp.audio = new QAudioOutput(sp.player);
        sp.player->setAudioOutput(sp.audio);
        sp.audio->setVolume(1.0);
        // 后端起不来/解码失败(缺插件、文件损坏):beep 兜底,反馈不能没有
        QObject::connect(sp.player, &QMediaPlayer::errorOccurred, sp.player,
                         [](QMediaPlayer::Error, const QString &) { QApplication::beep(); });
    }
    // 主音量在这里生效(拖动滑块立刻影响下一条提示音)
    sp.audio->setVolume(masterVolume() / 100.0);
    sp.player->setSource(source.startsWith(QLatin1String("qrc"))
                             ? QUrl(source)
                             : QUrl::fromLocalFile(source));
    sp.player->play();
}

} // namespace appaudio
