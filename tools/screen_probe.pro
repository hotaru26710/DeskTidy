QT += core gui widgets

CONFIG += c++17 console
CONFIG -= app_bundle

# ---------------------------------------------------------------------------
# screen_probe —— 抓真实屏幕看标题栏颜色。
#
# 关键：把浮窗**自己移动到一个已知位置**再抓那块区域，于是"抓屏区域"
# 与"窗口位置"同源，不会出现之前那种差 28px 的错位。
# ---------------------------------------------------------------------------

TARGET = screen_probe
TEMPLATE = app

INCLUDEPATH += .. ../core ../ui

SOURCES += \
    screen_probe.cpp \
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
    ../ui/floatinghoveroverlay.cpp \
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
    ../ui/floatinghoveroverlay.h \
    ../ui/floatingboxmanager.h \
    ../core/windowlayout.h \
    ../ui/preferreddropeffect.h \
    ../ui/previewdialog.h
