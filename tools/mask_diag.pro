QT += core gui widgets

CONFIG += c++17 console
CONFIG -= app_bundle

# ---------------------------------------------------------------------------
# mask_diag —— 诊断"浮窗顶部全透明、点不到"。
# 被 mask 裁掉的区域不接收鼠标事件，所以逐行看 mask 覆盖率就能定位。
# ---------------------------------------------------------------------------

TARGET = mask_diag
TEMPLATE = app

INCLUDEPATH += .. ../core ../ui

SOURCES += \
    mask_diag.cpp \
    ../core/coretypes.cpp \
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
    ../core/windowlayout.cpp \
    ../ui/preferreddropeffect.cpp \
    ../ui/previewdialog.cpp

HEADERS += \
    ../core/coretypes.h \
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
    ../core/windowlayout.h \
    ../ui/preferreddropeffect.h \
    ../ui/previewdialog.h
