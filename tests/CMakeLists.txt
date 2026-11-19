set(CLIENT "${PROJECT_SOURCE_DIR}/client")
set(SERVER "${PROJECT_SOURCE_DIR}/server")

add_library(VehiclePanelWindow STATIC "${CLIENT}/mainwindow.cpp" "${CLIENT}/mainwindow.h")
target_include_directories(VehiclePanelWindow PUBLIC "${CLIENT}")
target_link_libraries(VehiclePanelWindow PUBLIC Qt::Core Qt::Widgets Qt::Network)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(VehiclePanelWindow PRIVATE -Wall -Wextra -Wpedantic)
endif()

function(add_window_check target source)
    qt_add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE VehiclePanelWindow)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endfunction()

add_window_check(Dht11Check dht11_check.cpp)
add_window_check(ImuCheck imu_check.cpp)
add_window_check(PageCheck page_check.cpp)
add_window_check(PreviewCheck preview_check.cpp)


add_executable(Dht11FrameCheck frame_check.c)
add_test(NAME Dht11FrameCheck COMMAND Dht11FrameCheck)
