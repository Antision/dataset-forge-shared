# dataset_forge 公用代码库的 qmake 入口。
#
# 用法(在两个项目的 .pro 里各加一行):
#     include($$PWD/common/common.pri)
#
# 注意:common 是 git submodule。clone 之后若没有 common.pri,qmake 会报
# "Cannot find file",先执行:
#     git submodule update --init --recursive

# 每个模块一个子目录,头文件在 INCLUDEPATH 里,项目侧仍写
#   #include "qflowlayout.h"
# 这样从"项目内自带一份"迁到共享库时,源文件的 include 不用改。

# --- qflowlayout:流式布局 + 拖拽重排 ---
# 出自 Qt 官方 Flow Layout 示例,已扩充 moveItem/indexAt/insertIndexAt
# 与 QFlowLayoutReorderer。文件头保留 Qt 的 BSD-3-Clause 许可声明。
INCLUDEPATH += $$PWD/qflowlayout
SOURCES     += $$PWD/qflowlayout/qflowlayout.cpp
HEADERS     += $$PWD/qflowlayout/qflowlayout.h

# --- audio:程序级提示音 + 主音量 ---
# 需要一个能出声的 Qt:消费方必须在 .pro 里 `QT += multimedia`,否则这里
# 的 QMediaPlayer 链接不过(缺 Qt6Multimedia.dll 时运行期退回 beep)。
# 音效素材编进 qrc,不依赖构建时往 exe 目录拷文件。
INCLUDEPATH += $$PWD/audio
SOURCES     += $$PWD/audio/audio.cpp
HEADERS     += $$PWD/audio/audio.h
RESOURCES   += $$PWD/audio/sounds.qrc

# --- logwidget:分级日志窗口(带颜色/提示音/自动唤醒) ---
# 依赖 audio。需要 FORMS:uic 会生成 ui_logwidget.h,
# 由 SOURCES 里的 logwidget.cpp 直接 include。
INCLUDEPATH += $$PWD/logwidget
SOURCES     += $$PWD/logwidget/logwidget.cpp
HEADERS     += $$PWD/logwidget/logwidget.h
FORMS       += $$PWD/logwidget/logwidget.ui
