QT += core gui widgets

CONFIG += c++17 console
CONFIG -= app_bundle

# ---------------------------------------------------------------------------
# hover_feedback_diag —— 悬停触感（光影描边）的动态行为验证。
#
# 与 hover_diag 同源：必须要真事件循环才能验证的三件事
#   1) 连续 / 快速进出后进度收敛；
#   2) 快速跨多个浮窗时各自收敛；
#   3) 整个过程不改动窗口尺寸 / 位置 / 透明度 / 遮罩。
#
# 依赖链与 hover_diag 完全一致（浮窗会用真的 AppService / ItemListWidget）。
# ---------------------------------------------------------------------------

TARGET = hover_feedback_diag
TEMPLATE = app

INCLUDEPATH += .. ../core ../ui

SOURCES += \
    hover_feedback_diag.cpp \
    ../core/coretypes.cpp \
    ../core/corenames.cpp \
    ../core/settings.cpp \
    ../core/boxmanager.cpp \
    ../core/deskscanner.cpp \
    ../core/collector.cpp \
    ../core/undostack.cpp \
    ../core/appservice.cpp \
    ../core/windowlayout.cpp \
    ../core/opener.cpp \
    ../ui/itemlistwidget.cpp \
    ../ui/floatingboxwidget.cpp \
    ../ui/floatinghoveroverlay.cpp \
    ../ui/floatingboxmanager.cpp \
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
    ../core/windowlayout.h \
    ../core/opener.h \
    ../ui/itemlistwidget.h \
    ../ui/floatingboxwidget.h \
    ../ui/floatinghoveroverlay.h \
    ../ui/floatingboxmanager.h \
    ../ui/preferreddropeffect.h \
    ../ui/previewdialog.h