add_executable(tcp_ping_server
    tcp_ping_server.c
    ap3216c.c
    icm20608.c
    dht11.c
    camera_capture.c
)
target_compile_options(tcp_ping_server PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(tcp_ping_server PRIVATE rt)
