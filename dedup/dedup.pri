# dataset_forge 公用代码库 —— 图片去重模块(MERGE-01)
#
# 从 gallery_info 的 deduplicate_window/ 整体搬来的模块。用法:
#
#     DEDUP_OPENCV_ROOT = F:/libs/opencv
#     DEDUP_JPEG_ROOT   = F:/libs/libjpeg-turbo64
#     include($$PWD/common/dedup/dedup.pri)
#
# 两个根路径必须由消费方提供:它们是构建机上的绝对路径,不写进 common
# (这个仓库是公开的)。模块只需要 OpenCV 与 libjpeg-turbo —— 计划里提到的
# ffmpeg 在这个模块里没有用到,视频解码属于 gi 的 preview_window。
#
# 头文件按文件名直接 include(INCLUDEPATH 加了 $$PWD),类名保持原样:
# 迁移不改类名(MERGE-01)。

isEmpty(DEDUP_OPENCV_ROOT): error("dedup.pri: set DEDUP_OPENCV_ROOT before including this file")
isEmpty(DEDUP_JPEG_ROOT):   error("dedup.pri: set DEDUP_JPEG_ROOT before including this file")

QT += concurrent

INCLUDEPATH += $$PWD
INCLUDEPATH += $$DEDUP_OPENCV_ROOT/build/include
INCLUDEPATH += $$DEDUP_JPEG_ROOT/include

CONFIG(debug, debug|release) {
    LIBS += -L$$DEDUP_OPENCV_ROOT/build/x64/vc16/lib -lopencv_world4120d
} else {
    LIBS += -L$$DEDUP_OPENCV_ROOT/build/x64/vc16/lib -lopencv_world4120
}
LIBS += -L$$DEDUP_JPEG_ROOT/lib -ljpeg

# dedupenv.cpp:回收站(SHFileOperationW)、资源管理器定位(ILCreateFromPathW)、
# 暗色标题栏(DwmSetWindowAttribute)。
win32: LIBS += -lshell32 -lole32 -ldwmapi

# --- 算法层 ---------------------------------------------------------------
# 纯算法 / 缓存 / 特征计算 / 后台扫描,不依赖窗口。
SOURCES += \
    $$PWD/dedupenv.cpp \
    $$PWD/cacheio.cpp \
    $$PWD/deduputils.cpp \
    $$PWD/webpquality.cpp \
    $$PWD/dedupworker.cpp

HEADERS += \
    $$PWD/deduppch.h \
    $$PWD/dedupenv.h \
    $$PWD/cacheio.h \
    $$PWD/deduputils.h \
    $$PWD/webpquality.h \
    $$PWD/pairstate_math.h \
    $$PWD/dedupworker.h \
    $$PWD/pathguard.h \
    $$PWD/lrucache.h

# --- 视图层 ---------------------------------------------------------------
# 双图对照、配对列表、目录对比、导入整理会话。仍然是控件,但不依赖任何
# gi 的窗口。
SOURCES += \
    $$PWD/dedupimagedisplaywidget.cpp \
    $$PWD/duplicatepairlistview.cpp \
    $$PWD/paircontextmenu.cpp \
    $$PWD/foldercomparewidget.cpp \
    $$PWD/importdedupe.cpp \
    $$PWD/foldercompare.cpp \
    $$PWD/dupmove.cpp

HEADERS += \
    $$PWD/DedupImageDisplayWidget.h \
    $$PWD/duplicatepairlistview.h \
    $$PWD/paircontextmenu.h \
    $$PWD/foldercomparewidget.h \
    $$PWD/importdedupe.h \
    $$PWD/foldercompare.h \
    $$PWD/dupmove.h \
    $$PWD/folderscope.h \
    $$PWD/folderregionjump.h \
    $$PWD/foldergroup.h

FORMS += \
    $$PWD/foldercomparewidget.ui

# --- 窗口层 ---------------------------------------------------------------
# 去重/配对主窗口。tqe 从 Tools 菜单打开它;gi 仍在 main() 里长期持有一个。
SOURCES += $$PWD/imagededuplicatewindow.cpp
HEADERS += $$PWD/imagededuplicatewindow.h
FORMS   += $$PWD/imagededuplicatewindow.ui
