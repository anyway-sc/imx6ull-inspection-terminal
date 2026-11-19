add_executable(tcp_ping_server
    tcp_ping_server.c
    ap3216c.c
    icm20608.c
    dht11.c
    sr04.c
    sr501.c
    sensor_sampler.c
    camera_worker.c
    ipc_protocol.c
    process_manager.c
)
find_package(Threads REQUIRED)
target_compile_options(tcp_ping_server PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(tcp_ping_server PRIVATE Threads::Threads rt)

add_executable(camera_service camera_service.c camera_capture.c ipc_protocol.c process_manager.c)
target_compile_options(camera_service PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(camera_service PRIVATE rt)
add_dependencies(tcp_ping_server camera_service)
