QT += core testlib widgets
QT -= gui

CONFIG += c++17 console
CONFIG -= app_bundle

# ---------------------------------------------------------------------------
# tst_appservice —— 阶段 1 新增逻辑的单测。
#
# 覆盖两件事：
#   * AppService::MoveSummary 统计（从 MainWindow::reportMoveResult 平移而来）
#   * Settings 的浮窗配置读写（含"盒名里有斜杠"这类会搅乱 QSettings 键结构的边界）
#
# 需要 widgets 是因为 AppService 依赖 Collector，而 Collector 用到 Qt 的
# 文件 API；同时 QApplication 也在 Widgets 里（Settings 的路径解析要用到应用名）。
#
# 注意：本测试会把配置写到独立的组织名下（见 tst_appservice.cpp 的 main），
# 不会污染主人的真实 DeskTidy 配置。
# ---------------------------------------------------------------------------

TARGET = tst_appservice
TEMPLATE = app

INCLUDEPATH += .. ../core

SOURCES += \
    tst_appservice.cpp \
    ../core/coretypes.cpp \
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
    ../core/corenames.h \
    ../core/settings.h \
    ../core/boxmanager.h \
    ../core/deskscanner.h \
    ../core/collector.h \
    ../core/undostack.h \
    ../core/appservice.h \
    ../core/opener.h
