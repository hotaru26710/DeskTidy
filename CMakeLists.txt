cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# DeskTidy —— 桌面收纳盒工具。
#
# 这份 CMakeLists 与 DeskTidy.pro 并存且等价，两者都指向同一批源文件。
# 用意：Ninja 构建更快，适合命令行一键验证；.pro 供 Qt Creator 打开使用。
# 改源文件清单时请同步修改两处。
# ---------------------------------------------------------------------------

project(DeskTidy VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

set(DESKTIDY_CORE_SOURCES
    core/coretypes.cpp
    core/corenames.cpp
    core/windowlayout.cpp
    core/settings.cpp
    core/autostart.cpp
    core/deskscanner.cpp
    core/boxmanager.cpp
    core/collector.cpp
    core/opener.cpp
    core/undostack.cpp
    core/appservice.cpp
)

set(DESKTIDY_CORE_HEADERS
    core/coretypes.h
    core/corenames.h
    core/windowlayout.h
    core/settings.h
    core/autostart.h
    core/deskscanner.h
    core/boxmanager.h
    core/collector.h
    core/opener.h
    core/undostack.h
    core/appservice.h
)

set(DESKTIDY_UI_SOURCES
    ui/mainwindow.cpp
    ui/boxlistwidget.cpp
    ui/itemlistwidget.cpp
    ui/previewdialog.cpp
    ui/preferreddropeffect.cpp
    ui/floatingboxwidget.cpp
    ui/floatinghoveroverlay.cpp
    ui/floatingboxmanager.cpp
    ui/trayicon.cpp
    ui/appearancedialog.cpp
)

set(DESKTIDY_UI_HEADERS
    ui/mainwindow.h
    ui/boxlistwidget.h
    ui/itemlistwidget.h
    ui/previewdialog.h
    ui/preferreddropeffect.h
    ui/floatingboxwidget.h
    ui/floatinghoveroverlay.h
    ui/floatingboxmanager.h
    ui/trayicon.h
    ui/appearancedialog.h
)

add_executable(DeskTidy WIN32
    main.cpp
    ${DESKTIDY_CORE_SOURCES}
    ${DESKTIDY_CORE_HEADERS}
    ${DESKTIDY_UI_SOURCES}
    ${DESKTIDY_UI_HEADERS}
)

target_include_directories(DeskTidy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
)

target_link_libraries(DeskTidy PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
