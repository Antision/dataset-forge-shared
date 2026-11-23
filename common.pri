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

# --- log:程序日志(teLog / telog) ---
# 原来是 tqe 的 func.h 里的设施;tag 层搬进 common 后两边都要用,单独成模块。
# 只是转发到 qDebug + 一个 GUI 线程上的 target,不依赖控件。
INCLUDEPATH += $$PWD/log
SOURCES     += $$PWD/log/log.cpp
HEADERS     += $$PWD/log/log.h

# --- ui:程序级外观(字体) ---
# appFontFamily() 原来是两个程序各写一份;共用资源前缀不同,函数里都试一遍。
# svgicon.h:Lucide 线条 SVG 的运行时着色渲染,需要 QtSvg 模块 —— 在这里
# 统一打开,两个消费方(tqe / gi)不用各自往 .pro 里加。
INCLUDEPATH += $$PWD/ui
SOURCES     += $$PWD/ui/fonts.cpp
HEADERS     += $$PWD/ui/fonts.h \
               $$PWD/ui/svgicon.h
QT          += svg

# --- task:可等待/可命名的后台任务 ---
# 线程池由这里拥有(不再由各程序 main.cpp 定义),补全搜索与缩略图解码共用;
# 退出时能按名字报告/等待仍未返回的任务。BS_thread_pool 是第三方单头文件。
INCLUDEPATH += $$PWD/task
SOURCES     += $$PWD/task/task_runner.cpp
HEADERS     += $$PWD/task/task_runner.h \
               $$PWD/task/BS_thread_pool.hpp

# --- teobject:可拷贝数据对象的手写信号/槽 ---
# teTag / teWord 这些数据对象会被复制、每切一次图创建上百个,不能是 QObject,
# 所以它们用这套轻量连接。名字保留 te 前缀:它是 tag 层自己的词汇(见 TASKS §17)。
INCLUDEPATH += $$PWD/teobject
SOURCES     += $$PWD/teobject/teobject.cpp
HEADERS     += $$PWD/teobject/teobject.h

# --- tagview:tag 数据层 ---
# teWord / teTag / teTagList / 快照 / undo / 文本切分。没有一行控件代码,
# 两个程序共用;per-tag 控件仍留在 tqe 的 tetag.h。
INCLUDEPATH += $$PWD/tagview
SOURCES     += $$PWD/tagview/tag.cpp \
               $$PWD/tagview/tagviewcontracts.cpp \
               $$PWD/tagview/suggestionedit.cpp \
               $$PWD/tagview/taglistmodel.cpp \
               $$PWD/tagview/tagmarks.cpp \
               $$PWD/tagview/taglistview.cpp
HEADERS     += $$PWD/tagview/tag.h \
               $$PWD/tagview/tagviewcontracts.h \
               $$PWD/tagview/suggestionedit.h \
               $$PWD/tagview/taglistmodel.h \
               $$PWD/tagview/tagmarks.h \
               $$PWD/tagview/tagvisual.h \
               $$PWD/tagview/taglistview.h \
               $$PWD/tagview/type_search.h

# --- imageview:图片查看窗口(滚轮缩放/拖动/点击切换) ---
# 出自 tqe 的 teimagewidget.*;窗口外观由宿主自己套(tqe 的 teImageWidget 薄壳)。
INCLUDEPATH += $$PWD/imageview
SOURCES     += $$PWD/imageview/imageview.cpp
HEADERS     += $$PWD/imageview/imageview.h

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

# --- litewidget:无边框窗口底座 ---
# litewidget   最简方角窗口(菜单 / 浮窗)
# litewidget2  右上角凸起条的圆角窗口,自绘轮廓 + 自管最大化(弹层/渲染窗)
# litewindow   普通窗口:整宽标题栏 + 三颗标准按钮(大图/筛选/日志)
# geometry     纯函数,不依赖控件,回归工程可以直接断言
# windowglyphbutton  线条绘制的窗口按钮(关闭/最大化/还原/最小化)
INCLUDEPATH += $$PWD/litewidget
SOURCES     += $$PWD/litewidget/litewidget.cpp \
               $$PWD/litewidget/litewindow.cpp \
               $$PWD/litewidget/litewidget2.cpp \
               $$PWD/litewidget/litewidget2geometry.cpp \
               $$PWD/litewidget/windowglyphbutton.cpp
HEADERS     += $$PWD/litewidget/litewidget.h \
               $$PWD/litewidget/litewindow.h \
               $$PWD/litewidget/litewidget2.h \
               $$PWD/litewidget/litewidget2geometry.h \
               $$PWD/litewidget/windowglyphbutton.h

# LiteWidget2 用 Get/SetWindowLongPtr + SetWindowPos 去掉非客户区并打开原生
# 边缘缩放(user32)。放在这里而不是让每个消费方自己加:少了它 gi 会在链接
# 阶段报三个 LNK2019(SetWindowPos / GetWindowLongPtrW / SetWindowLongPtrW)。
win32: LIBS += -luser32
