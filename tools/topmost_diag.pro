QT += core gui widgets

CONFIG += c++17 console
CONFIG -= app_bundle

# ---------------------------------------------------------------------------
# topmost_diag —— 验证"切换总在最前之后圆角遮罩是否还在"。
#
# 背景：applyAlwaysOnTop 里 setWindowFlags() 会重建原生窗口，
# 而 mask 是挂在原生窗口上的属性。用户报的"顶部全透明、点不到"
# 很可能是那条路径残留了错位的旧遮罩。
# 这个程序在切换前后各量一次 mask 指纹，不一致就说明确实坏了。
# ---------------------------------------------------------------------------

TARGET = topmost_diag
TEMPLATE = app

INCLUDEPATH += .. ../core ../ui

SOURCES += \
    topmost_diag.cpp \
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
