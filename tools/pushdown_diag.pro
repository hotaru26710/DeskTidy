QT += core gui widgets

CONFIG += c++17 console
CONFIG -= app_bundle

# ---------------------------------------------------------------------------
# pushdown_diag —— 浮窗"互相让位 + 平滑移动 + 钉住"的行为验证。
#
# 这一条必须是探针而不是单测：推动的几何计算（WindowLayout）已经被
# 64 项单测覆盖了，但那一层只保证"算得对"。本步真正新引入的风险全在
# **接线上**：
#   * 展开的那一刻高度动画才刚起步，喂给计算的是不是展开后的尺寸？
#   * 位移是不是**滑**过去的（而不是瞬移）？
#   * 卷起时有没有滑回原位？
#   * 被推开的位移有没有**偷偷落盘**（这是本步最容易错、后果最难查的一处）？
#   * 钉住的窗口会不会被推？
# 这些都只能在真实窗口 + 真事件循环里观察。
# ---------------------------------------------------------------------------

TARGET = pushdown_diag
TEMPLATE = app

INCLUDEPATH += .. ../core ../ui

SOURCES += \
    pushdown_diag.cpp \
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
