QT += core gui widgets

CONFIG += c++17 console
CONFIG -= app_bundle

# ---------------------------------------------------------------------------
# hover_diag —— 悬停自动展开 / 离开自动卷起的行为验证。
#
# 这个探针是必要的，因为单测覆盖不到这件事：悬停展开的全部逻辑都挂在
# "定时器到点"和"鼠标进出事件"上，而这两样都必须有**真事件循环**才会跑。
# 单测里那种"直接调函数看返回值"的写法，对"250ms 后到底有没有展开"
# 一个字都说明不了。
#
# 这里用 QApplication::sendEvent 手工投递 QEnterEvent / QEvent::Leave，
# 再用 QEventLoop + QTimer 推进真实时间，让悬停定时器自然到点。
#
# ⚠️ 已知局限：手工投递事件绕过了窗口系统，所以"真实鼠标移动是否会产生
# enter/leave"这件事**没有被验证**。探针验证的是"收到事件之后的逻辑对不对"。
# 真实手感需要人工把鼠标移上去确认。
# ---------------------------------------------------------------------------

TARGET = hover_diag
TEMPLATE = app

INCLUDEPATH += .. ../core ../ui

SOURCES += \
    hover_diag.cpp \
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
    ../ui/floatingboxmanager.h \
    ../ui/preferreddropeffect.h \
    ../ui/previewdialog.h
