QT += core gui widgets

CONFIG += c++17 console
CONFIG -= app_bundle

# ---------------------------------------------------------------------------
# overlap_diag —— 三浮窗"二次推动 / 多锚点收起"仍然重叠的回归探针。
#
# 这些缺陷都只在三个浮窗、窗口接连展开/收起时才暴露，纯几何单测和
# 两个浮窗的 pushdown_diag 都覆盖不到 —— 它们出在 manager 与浮窗之间
# 的接线（位移语义、归位时机、两套动画争 geometry、回家标记时序）上，
# 必须在真实窗口 + 真事件循环里观察。
# ---------------------------------------------------------------------------

TARGET = overlap_diag
TEMPLATE = app

INCLUDEPATH += .. ../core ../ui

SOURCES += \
    overlap_diag.cpp \
    ../core/coretypes.cpp \
    ../core/windowlayout.cpp \
    ../core/corenames.cpp \
    ../core/settings.cpp \
    ../core/boxmanager.cpp \
    ../core/deskscanner.cpp \
    ../core/collector.cpp \
    ../core/undostack.cpp \
    ../core/appservice.cpp \
    ../core/opener.cpp \
    ../ui/itemlistwidget.cpp \
    ../ui/floatingboxwidget.cpp \
    ../ui/floatingboxmanager.cpp \
    ../ui/preferreddropeffect.cpp \
    ../ui/previewdialog.cpp

HEADERS += \
    ../core/coretypes.h \
    ../core/windowlayout.h \
    ../core/corenames.h \
    ../core/settings.h \
    ../core/boxmanager.h \
    ../core/deskscanner.h \
    ../core/collector.h \
    ../core/undostack.h \
    ../core/appservice.h \
    ../core/opener.h \
    ../ui/itemlistwidget.h \
    ../ui/floatingboxwidget.h \
    ../ui/floatingboxmanager.h \
    ../ui/preferreddropeffect.h \
    ../ui/previewdialog.h
