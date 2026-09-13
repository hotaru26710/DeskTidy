QT += core gui

CONFIG += c++17 console
CONFIG -= app_bundle

# ---------------------------------------------------------------------------
# windowlayout_diag —— 浮窗"互相让位"的几何校验。
#
# WindowLayout 是纯计算，没有 QWidget 依赖，所以这个探针只链 core 那几个
# 最基础的翻译单元，跑起来是毫秒级的，可以放心放进 verify.bat。
#
# 它和 tests\tst_floatinglogic.cpp 的 C 组有重叠，但**不重复**：
# 单测钉的是每个约束的边界（"恰好推多少"），这个探针钉的是
# **真实屏幕尺寸下的整体不变量** —— 让位之后任意两个窗口都不重叠、
# 没人跑到屏幕外、没人被推错方向。前者防写错，后者防"三条约束各自
# 都对、合起来却互相打架"。
# ---------------------------------------------------------------------------

TARGET = windowlayout_diag
TEMPLATE = app

INCLUDEPATH += .. ../core

SOURCES += \
    windowlayout_diag.cpp \
    ../core/windowlayout.cpp

HEADERS += \
    ../core/windowlayout.h
