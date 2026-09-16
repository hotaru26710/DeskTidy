QT += core testlib gui

CONFIG += c++17 console
CONFIG -= app_bundle

# ---------------------------------------------------------------------------
# tst_corelogic —— CoreNames 纯函数单测（21 项）。
#
# 只编 corenames.cpp：被测的是零依赖的纯计算层，
# 不必拖上整个 core 与 UI，编译快、跑起来是毫秒级。
# ---------------------------------------------------------------------------

TARGET = tst_corelogic
TEMPLATE = app

INCLUDEPATH += .. ../core

SOURCES += \
    tst_corelogic.cpp \
    ../core/coretypes.cpp \
    ../core/corenames.cpp

HEADERS += \
    ../core/coretypes.h \
    ../core/corenames.h
