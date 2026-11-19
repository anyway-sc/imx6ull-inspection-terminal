add_executable(tcp_ping_server
    tcp_ping_server.c
    ap3216c.c
    icm20608.c
    dht11.c
    sr04.c
    sr501.c
    camera_capture.c
    sensor_sampler.c
    camera_worker.c
)
find_package(Threads REQUIRED)
target_compile_options(tcp_ping_server PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(tcp_ping_server PRIVATE Threads::Threads rt)
