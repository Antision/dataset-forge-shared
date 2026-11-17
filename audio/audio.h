#ifndef AUDIO_H
#define AUDIO_H

/*
 * audio - 程序级提示音 + 主音量(两个项目共用)。
 *
 * 从 gallery_info 的 volumepopupwidget.{h,cpp} 里搬出来的:那两份代码本来是
 * gi 专用的,但 tag_quick_editor 的日志窗口也要响,而"一条声音一条通路、
 * 各自节流、按级别分开"这套逻辑没有任何 gi 特有的东西,所以放进 common。
 *
 * 消费方必须在 .pro 里加 `QT += multimedia`(QMediaPlayer/QAudioOutput),
 * 否则链接不过。
 *
 * 音效素材编进 qrc(common/audio/sounds.qrc,约 290 KB),不再依赖构建时往
 * exe 目录拷文件;同时保留 `<exe 目录>/sounds/` 的后备查找,老部署(gi 的
 * QMAKE_POST_LINK 仍在拷)不用动也是好的。
 *
 * 主音量存 exe 同目录 config.ini 的 [app] masterVolume(0..100,默认 70),
 * 和 gi 原来的位置、键名完全一致,所以用户已有的配置文件继续生效。
 */

#include <QString>

class QWidget;

namespace appaudio {

/*
 * 提示音分组(每条一个独立的 QMediaPlayer,互不打断)。
 *
 * 只有日志窗口会用的两条(Warning/Error)是 tqe 的需求;其余是 gi 原有的
 * 训练集/去重反馈。放进同一个枚举,两个项目共用一张表,不会出现"同一条
 * 声音两个文件"的历史。
 */
enum class Sound {
    DatasetSuccess,   // 训练集:图片入队(拖入/监视到新导出) —— modern success
    DatasetWarning,   // 训练集:警告 —— soft-ui warning
    Warning,          // 日志窗口:Warning —— universfield warning notification
    Error,            // 日志窗口:Error   —— universfield error notification
    SingleAction,     // 去重:只处理当前这一对 —— dragon-studio pop
    BatchAction,      // 去重:按选中多对批量处理 —— dragon-studio bubble pop
    NothingRemoved,   // 去重:auto remove 一对都没移走 —— sfxmint mahjong
    Count             // 哨兵(不是一条声音)
};

/* 0..100(默认 70;首次访问从 config.ini 读一次,之后走缓存) */
int  masterVolume();
/* 更新缓存并立即写回 config.ini */
void setMasterVolume(int percent);

/*
 * 播一条提示音(非阻塞;同一条声音重复调用会打断上一条重新播)。
 * 音量 = 主音量;素材缺失或多媒体后端起不来就退回 QApplication::beep()。
 * 只能在主线程调(内部是 QMediaPlayer)。
 */
void playSound(Sound kind);

} // namespace appaudio

#endif // AUDIO_H
