QT += core gui widgets
VERSION = 1.0.0

CONFIG += c++17

# core/ 与 ui/ 两套 include 皆从此解析。
INCLUDEPATH += core ui

SOURCES += \
    main.cpp \
    core/coretypes.cpp \
    core/corenames.cpp \
    core/windowlayout.cpp \
    core/settings.cpp \
    core/autostart.cpp \
    core/deskscanner.cpp \
    core/boxmanager.cpp \
    core/collector.cpp \
    core/opener.cpp \
    core/undostack.cpp \
    core/appservice.cpp \
    ui/mainwindow.cpp \
    ui/boxlistwidget.cpp \
    ui/itemlistwidget.cpp \
    ui/previewdialog.cpp \
    ui/preferreddropeffect.cpp \
    ui/floatingboxwidget.cpp \
    ui/floatinghoveroverlay.cpp \
    ui/floatingboxmanager.cpp \
    ui/trayicon.cpp \
    ui/appearancedialog.cpp \
    ui/themedialog.cpp \
    ui/settingsdialog.cpp

HEADERS += \
    core/coretypes.h \
    core/corenames.h \
    core/windowlayout.h \
    core/settings.h \
    core/autostart.h \
    core/deskscanner.h \
    core/boxmanager.h \
    core/collector.h \
    core/opener.h \
    core/undostack.h \
    core/appservice.h \
    ui/mainwindow.h \
    ui/boxlistwidget.h \
    ui/itemlistwidget.h \
    ui/previewdialog.h \
    ui/preferreddropeffect.h \
    ui/floatingboxwidget.h \
    ui/floatinghoveroverlay.h \
    ui/floatingboxmanager.h \
    ui/trayicon.h \
    ui/appearancedialog.h \
    ui/themedialog.h \
    ui/settingsdialog.h

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
