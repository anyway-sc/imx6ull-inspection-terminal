"""仅作为进程测试子程序：注入碎片、半帧、错误头和无响应，不访问硬件。"""
import os
import socket
import struct
import sys
import time


def main():
    fd, _parent, session, generation = map(int, sys.argv[1:5])
    channel = socket.socket(fileno=fd)
    mode = os.environ.get('FAKE_IPC_FAULT', 'fragment')

    def header(kind, identifier=0, code=0, length=0, width=0, height=0, gen=generation):
        return struct.pack('!10I', 0x5643414D, 1, kind, session, gen, identifier,
                           code, length, width, height)

    def transmit(data):
        if mode == 'fragment':
            for byte in data:
                channel.sendall(bytes([byte]))
                time.sleep(0.001)
        else:
            channel.sendall(data)

    if mode == 'no_ready':
        time.sleep(60)
        return
    transmit(header(1))
    while True:
        request = b''
        while len(request) < 40:
            data = channel.recv(40 - len(request))
            if not data:
                return
            request += data
        words = struct.unpack('!10I', request)
        identifier, kind = words[5:7]
        if mode == 'no_reply':
            time.sleep(60)
            return
        if mode == 'oversize':
            transmit(header(3, identifier, 1, 4 * 1024 * 1024 + 1, 640, 480))
            return
        if mode == 'wrong_generation':
            transmit(header(3, identifier, 1, 32, 640, 480, generation + 1))
            return
        if mode == 'wrong_id':
            transmit(header(3, identifier + 1, 1, 32, 640, 480))
            return
        if kind in (0, 2):
            image = b'\xff\xd8\n\0' + b'Z' * 26 + b'\xff\xd9'
            transmit(header(3, identifier, 1, len(image), 640, 480))
            if mode == 'half':
                channel.sendall(image[:4])
                return
            transmit(image)
        else:
            transmit(header(3, identifier, 2 if kind == 1 else 3))


if __name__ == '__main__':
    main()
