QT += core gui widgets

CONFIG += c++17 console
CONFIG -= app_bundle

# ---------------------------------------------------------------------------
# rollup_diag —— 采样卷起 / 展开的高度曲线。
# 验证三件事：动画是否渐变、终值是否正确、反复展开高度是否衰减。
# ---------------------------------------------------------------------------

TARGET = rollup_diag
TEMPLATE = app

INCLUDEPATH += .. ../core ../ui

SOURCES += \
    rollup_diag.cpp \
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
