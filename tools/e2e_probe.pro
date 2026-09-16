QT += core gui

CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = e2e_probe
TEMPLATE = app

INCLUDEPATH += .. ../core

# 阶段 7（删除收纳盒）会调 AppService::deleteBox，故需拉进 appservice.cpp
# 与它依赖的 settings.cpp —— AppService 内部持有 Settings 值成员。
SOURCES += \
    e2e_probe.cpp \
    ../core/coretypes.cpp \
    ../core/windowlayout.cpp \
    ../core/corenames.cpp \
    ../core/settings.cpp \
    ../core/deskscanner.cpp \
    ../core/boxmanager.cpp \
    ../core/collector.cpp \
    ../core/undostack.cpp \
    ../core/appservice.cpp

HEADERS += \
    ../core/coretypes.h \
    ../core/windowlayout.h \
    ../core/corenames.h \
    ../core/settings.h \
    ../core/deskscanner.h \
    ../core/boxmanager.h \
    ../core/collector.h \
    ../core/undostack.h \
    ../core/appservice.h
