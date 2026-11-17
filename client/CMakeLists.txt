cmake_minimum_required(VERSION 3.19)
project(VehiclePanel LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Network LinguistTools)

qt_standard_project_setup()

qt_add_executable(VehiclePanel
    WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
)

qt_add_translations(
    TARGETS VehiclePanel
    TS_FILES VehiclePanel_zh_CN.ts
)

target_link_libraries(VehiclePanel
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::Network
)

install(TARGETS VehiclePanel
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET VehiclePanel
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
