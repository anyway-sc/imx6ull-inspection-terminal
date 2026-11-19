"""Linux 多进程故障回归，执行真实主程序与相机子程序，不连接开发板。"""
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

import server_network_check as network
from server_network_check import Connection, Server, wait_for


def running(pid):
    try:
        return Path(f'/proc/{pid}/stat').read_text().split(') ')[1][0] != 'Z'
    except FileNotFoundError:
        return False


def connected_child(server, previous=None):
    children = server.children()
    return len(children) == 1 and children[0] != previous and running(children[0])


def check_queue(executable):
    server = Server(executable, delay=200)
    connection = Connection()
    try:
        connection.send(b'PREVIEW_START 1\n')
        assert connection.receive() == b'OK 1 PREVIEW_STARTED\n'
        connection.send(b''.join(f'PREVIEW_FRAME {i}\n'.encode() for i in range(10, 42)))
        identifiers, busy, frames = set(), 0, 0
        for _ in range(32):
            line = connection.receive()
            fields = line.split()
            identifier = int(fields[1])
            assert identifier not in identifiers
            identifiers.add(identifier)
            if fields[0] == b'JPEG':
                frames += 1
            else:
                assert fields[0] == b'ERR' and fields[2] == b'CAMERA_BUSY', line
                busy += 1
        assert identifiers == set(range(10, 42)) and frames > 0 and busy > 0
        connection.send(b'PREVIEW_STOP 50\n')
        assert connection.receive() == b'OK 50 PREVIEW_STOPPED\n'
        connection.close()
        wait_for(server.idle)
        server.stop()
        print('通过：有界请求、队列满完整回复、请求编号和预览停止')
    finally:
        connection.close()
        log = server.close()
        if sys.exc_info()[0]: print(log)


def check_crash_restart(executable):
    server = Server(executable, delay=300)
    connection = Connection()
    try:
        connection.send(b'PREVIEW_START 1\n')
        assert connection.receive() == b'OK 1 PREVIEW_STARTED\n'
        child = server.children()[0]
        # 正式子程序只继承标准流、任务读端 3 和结果写端 4。
        descriptors = sorted(int(p.name) for p in Path(f'/proc/{child}/fd').iterdir())
        assert descriptors == [0, 1, 2, 3, 4], descriptors
        child_pipes = [os.readlink(f'/proc/{child}/fd/{fd}') for fd in (3, 4)]
        assert all(name.startswith('pipe:[') for name in child_pipes)
        assert child_pipes[0] != child_pipes[1]
        parent_targets = [os.readlink(fd) for fd in Path(f'/proc/{server.process.pid}/fd').iterdir()]
        assert all(parent_targets.count(name) == 1 for name in child_pipes), parent_targets
        assert server.fd_count() == server.idle_fds + 3  # 客户端加两根管道，各保留一个端点。
        for fd, direction in ((3, os.O_RDONLY), (4, os.O_WRONLY)):
            info = Path(f'/proc/{child}/fdinfo/{fd}').read_text().splitlines()
            flags = int(next(line.split()[1] for line in info if line.startswith('flags:')), 8)
            assert flags & os.O_ACCMODE == direction
        status = Path(f'/proc/{server.process.pid}/status').read_text().splitlines()
        ignored = int(next(line.split()[1] for line in status if line.startswith('SigIgn:')), 16)
        assert ignored & (1 << (signal.SIGPIPE - 1)), '主服务未忽略 SIGPIPE'
        assert len(list(Path(f'/proc/{child}/task').iterdir())) == 1
        assert server.thread_count() == 6
        connection.send(b'PREVIEW_FRAME 2\nPREVIEW_FRAME 3\n')
        time.sleep(0.05)
        os.kill(child, signal.SIGSEGV)
        replies = [connection.receive(), connection.receive()]
        assert set(replies) == {b'ERR 2 CAMERA_START\n', b'ERR 3 CAMERA_START\n'}, replies
        connection.send(b'PING\nREAD_SR04 4\n')
        assert connection.receive() == b'PONG\n'
        assert connection.receive() == b'SR04 4 250 1458\n'
        wait_for(lambda: connected_child(server, child))
        connection.send(b'PREVIEW_FRAME 5\n')
        assert connection.receive() == b'ERR 5 PREVIEW_NOT_STARTED\n'
        connection.send(b'PREVIEW_START 6\nPREVIEW_FRAME 7\n')
        assert connection.receive() == b'OK 6 PREVIEW_STARTED\n'
        assert connection.receive() == b'JPEG 7 65536\n'
        # 每连接最多初始启动加两次重启，超出预算保持降级，避免重启风暴。
        child = server.children()[0]
        os.kill(child, signal.SIGKILL)
        wait_for(lambda: connected_child(server, child))
        child = server.children()[0]
        os.kill(child, signal.SIGKILL)
        wait_for(lambda: not server.children())
        time.sleep(0.8)
        assert not server.children()
        connection.send(b'CAPTURE_JPEG 8\nPING\nREAD_DHT11 9\n')
        assert connection.receive() == b'ERR 8 CAMERA_START\n'
        assert connection.receive() == b'PONG\n'
        assert connection.receive() == b'DHT11 9 600 260\n'
        connection.close()
        wait_for(server.idle)
        server.stop()
        print('通过：相机崩溃隔离、失败任务清理、独立恢复、预览状态重置、有限重启、FD 隔离')
    finally:
        connection.close()
        log = server.close()
        if sys.exc_info()[0]: print(log)


def check_fault_protocol(executable, fault_program):
    for mode in ('fragment', 'half', 'oversize', 'wrong_generation', 'wrong_id',
                 'no_ready', 'no_reply', 'close_request', 'close_response'):
        server = Server(executable, camera_program=fault_program, extra_env={'FAKE_IPC_FAULT': mode})
        connection = Connection()
        connection.socket.settimeout(15)
        try:
            begin = time.monotonic()
            connection.send(b'CAPTURE_JPEG 1\nPING\n')
            assert connection.receive() == b'PONG\n'
            reply = connection.receive()
            expected = b'JPEG 1 32\n' if mode == 'fragment' else b'ERR 1 CAMERA_START\n'
            assert reply == expected, (mode, reply)
            assert time.monotonic() - begin < (5 if mode.startswith('close_') else 13)
            connection.send(b'READ_SR04 2\n')
            assert connection.receive() == b'SR04 2 250 1458\n'
            connection.close()
            wait_for(server.idle)
            server.stop()
            print(f'通过：IPC {mode}；不向 Qt 暴露不完整图像')
        finally:
            connection.close()
            log = server.close()
            if sys.exc_info()[0]: print(log)


def check_missing_service(executable):
    server = Server(executable, camera_program='/no-such-vehicle-camera-service')
    connection = Connection()
    try:
        connection.send(b'PING\nREAD_SR04 1\nCAPTURE_JPEG 2\n')
        assert connection.receive() == b'PONG\n'
        assert connection.receive() == b'SR04 1 250 1458\n'
        assert connection.receive() == b'ERR 2 CAMERA_START\n'
        connection.close()
        wait_for(server.idle)
        server.stop()
        print('通过：子程序缺失时基础监测继续运行')
    finally:
        connection.close()
        log = server.close()
        if sys.exc_info()[0]: print(log)


def check_exit(executable):
    server = Server(executable, delay=30000)
    connection = Connection()
    try:
        connection.send(b'PREVIEW_START 1\n')
        assert connection.receive() == b'OK 1 PREVIEW_STARTED\n'
        child = server.children()[0]
        connection.send(b'PREVIEW_FRAME 2\n')
        time.sleep(0.05)
        begin = time.monotonic()
        server.stop()
        assert time.monotonic() - begin < 3
        assert not Path(f'/proc/{child}').exists(), 'SIGTERM 后子进程未回收'
        print('通过：取帧期间 SIGTERM 回收相机子进程和采集线程')
    finally:
        connection.close()
        log = server.close()
        if sys.exc_info()[0]: print(log)

    server = Server(executable)
    connection = Connection()
    try:
        connection.send(b'PREVIEW_START 1\n')
        assert connection.receive() == b'OK 1 PREVIEW_STARTED\n'
        child = server.children()[0]
        server.process.kill()
        server.process.wait(timeout=3)
        wait_for(lambda: not running(child))
        print('通过：主进程异常退出后相机子进程停止运行')
    finally:
        connection.close()
        server.close()


def main():
    assert sys.platform.startswith('linux')
    executable = str(Path(sys.argv[1]).resolve())
    network.CAMERA_SERVICE = str(Path(sys.argv[2]).resolve())
    check_queue(executable)
    check_crash_restart(executable)
    check_missing_service(executable)
    with tempfile.TemporaryDirectory(prefix='vehicle-ipc-fault-') as directory:
        program = Path(directory) / 'fault-camera'
        source = Path(__file__).with_name('ipc_fault_service.py').read_text()
        program.write_text(f'#!{sys.executable}\n' + source)
        program.chmod(0o700)
        check_fault_protocol(executable, str(program))
    check_exit(executable)


if __name__ == '__main__':
    main()
