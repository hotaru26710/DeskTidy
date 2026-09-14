QT += core gui widgets

CONFIG += c++17 console
CONFIG -= app_bundle

# ---------------------------------------------------------------------------
# corner_mask —— 通过读窗口 mask 验证圆角（不抓屏）。
#
# 前两版用抓屏读像素，都被"抓到的区域对不上"和"桌面背景与窗口亮度差太小"
# 两件事绕住了。改用 mask 作判据：mask 就是 Qt 裁剪窗口可见区域用的位图，
# 它是"圆角有没有生效"的唯一真值，且完全不受桌面背景干扰。
# ---------------------------------------------------------------------------

TARGET = mask_anim_diag
TEMPLATE = app

INCLUDEPATH += .. ../core ../ui

SOURCES += \
    mask_anim_diag.cpp \
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
