QT += core gui
QT -= widgets

CONFIG += c++17 console
CONFIG -= app_bundle

# ---------------------------------------------------------------------------
# delete_probe —— 删除收纳盒的实机验证。
#
# 需要 gui 模块是因为被测的 core/opener.cpp 用了 QDesktopServices
# （虽然删盒流程本身用不到它，但链进来就得满足它的依赖）。
# 不用 widgets：本探针不建任何窗口。
# ---------------------------------------------------------------------------

TARGET = delete_probe
TEMPLATE = app

INCLUDEPATH += .. ../core

SOURCES += \
    delete_probe.cpp \
    ../core/coretypes.cpp \
    ../core/windowlayout.cpp \
    ../core/corenames.cpp \
    ../core/settings.cpp \
    ../core/boxmanager.cpp \
    ../core/deskscanner.cpp \
    ../core/collector.cpp \
    ../core/undostack.cpp \
    ../core/appservice.cpp \
    ../core/opener.cpp

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
    ../core/opener.h
