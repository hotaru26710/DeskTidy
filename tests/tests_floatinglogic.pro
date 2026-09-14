QT += core testlib widgets
QT -= gui

CONFIG += c++17 console
CONFIG -= app_bundle

# ---------------------------------------------------------------------------
# tst_floatinglogic —— 阶段 3/4 新增逻辑的单测。
#
# 覆盖两大块：
#   * Settings 的浮窗配置键编解码边界（重点是盒名含 '/' 时不能搅乱
#     QSettings 的层级结构）；
#   * FloatingBoxManager 的状态机（开关浮窗时内存状态与配置落盘是否一致、
#     盒目录消失时是否被正确丢弃、退出时是否保住配置）。
#
# 之所以要拉上这么多源文件：FloatingBoxManager 会真的 new 出
# FloatingBoxWidget，而后者又依赖 ItemListWidget / AppService / Opener /
# PreviewDialog。这条依赖链是产品结构决定的，测试只能照单全收 ——
# 这也侧面说明 manager 与 widget 的耦合较紧，日后若要单独测 manager，
# 值得考虑给 widget 的创建过程加一层可替换的工厂。
#
# 注意：本测试把配置写到独立的组织名/应用名下（见 cpp 的 main），
# 且刻意与 tst_appservice 用不同的名字，避免两个测试并行跑时
# 互相清空对方的配置目录。
# ---------------------------------------------------------------------------

TARGET = tst_floatinglogic
TEMPLATE = app

INCLUDEPATH += .. ../core ../ui

SOURCES += \
    tst_floatinglogic.cpp \
    ../core/coretypes.cpp \
    ../core/corenames.cpp \
    ../core/windowlayout.cpp \
    ../core/settings.cpp \
    ../core/boxmanager.cpp \
    ../core/deskscanner.cpp \
    ../core/collector.cpp \
    ../core/undostack.cpp \
    ../core/appservice.cpp \
    ../core/opener.cpp \
    ../ui/itemlistwidget.cpp \
    ../ui/preferreddropeffect.cpp \
    ../ui/previewdialog.cpp \
    ../ui/floatingboxwidget.cpp \
    ../ui/floatinghoveroverlay.cpp \
    ../ui/floatingboxmanager.cpp

HEADERS += \
    ../core/coretypes.h \
    ../core/corenames.h \
    ../core/windowlayout.h \
    ../core/settings.h \
    ../core/boxmanager.h \
    ../core/deskscanner.h \
    ../core/collector.h \
    ../core/undostack.h \
    ../core/appservice.h \
    ../core/opener.h \
    ../ui/itemlistwidget.h \
    ../ui/preferreddropeffect.h \
    ../ui/previewdialog.h \
    ../ui/floatingboxwidget.h \
    ../ui/floatinghoveroverlay.h \
    ../ui/floatingboxmanager.h
