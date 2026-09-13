QT += core gui widgets

CONFIG += c++17
CONFIG -= app_bundle

TARGET = drag_probe2
TEMPLATE = app

# QWindowsMimeConverter 虽然在 private 头目录里，但类本身是 Q_GUI_EXPORT 导出的。
# 不需要链接额外库（就在 Qt6Gui 内）。
SOURCES += drag_probe2.cpp
