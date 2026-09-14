QT += core gui widgets

CONFIG += c++17 console
CONFIG -= app_bundle

# ---------------------------------------------------------------------------
# e2e_floating —— 浮窗信号链路的自动化验证。
#
# 与 e2e_probe 的分工：
#   e2e_probe    只测 core（QCoreApplication，秒级），验证"文件有没有被正确搬动"；
#   e2e_floating 需要 GUI，验证"信号有没有接上" —— 收纳/撤销之后，
#                浮窗的列表与按钮会不会自己更新。
#
# 后者是人工最难发现的一类 bug：界面看着正常，只是数据没跟着变。
#
# 需要 widgets 是因为要真的构造浮窗控件；但它不进入事件循环，
# 只靠 processEvents 推一下信号，所以跑起来仍是秒级。
# ---------------------------------------------------------------------------

TARGET = e2e_floating
TEMPLATE = app

INCLUDEPATH += .. ../core ../ui

SOURCES += \
    e2e_floating.cpp \
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
