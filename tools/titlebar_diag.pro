QT += core gui widgets

CONFIG += c++17 console
CONFIG -= app_bundle

# ---------------------------------------------------------------------------
# titlebar_diag —— 用 QWidget::grab() 读标题栏自己的渲染结果。
#
# 用户报"整个标题栏透明、但字还在"。用 grab() 渲染控件自身，
# 直接看那一行像素里有没有标题栏底色 #f1f3f4 —— 有就没有这个问题，
# 没有就说明底色确实没被画出来。
# ---------------------------------------------------------------------------

TARGET = titlebar_diag
TEMPLATE = app

INCLUDEPATH += .. ../core ../ui

SOURCES += \
    titlebar_diag.cpp \
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
