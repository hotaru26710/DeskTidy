QT += core gui widgets

CONFIG += c++17 console
CONFIG -= app_bundle

# ---------------------------------------------------------------------------
# heightlock_diag —— 验证卷起时的 min/max 高度锁会不会顶掉高度动画。
# 写动画之前必须先把这件事摸清楚，否则很容易做出"看起来没跑"的动画。
# ---------------------------------------------------------------------------

TARGET = heightlock_diag
TEMPLATE = app

INCLUDEPATH += .. ../core ../ui

SOURCES += \
    heightlock_diag.cpp \
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
    ../ui/floatinghoveroverlay.cpp \
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
    ../ui/floatinghoveroverlay.h \
    ../ui/floatingboxmanager.h \
    ../ui/preferreddropeffect.h \
    ../ui/previewdialog.h
