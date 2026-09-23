import os
import socket
import logging

logger = logging.getLogger("CGPIOClient")
PRIMARY_SOCKET = "/run/debugbox_gpio.sock"
FALLBACK_SOCKET = "/tmp/debugbox_gpio.sock"

def get_socket_path():
    if os.path.exists(PRIMARY_SOCKET):
        return PRIMARY_SOCKET
    return FALLBACK_SOCKET

class CGPIOClient:
    @staticmethod
    def set_state(state_name: str) -> bool:
        """向 C 守护进程设置 LED 状态 (AP_MODE, CONNECTING, ONLINE, RESETTING, OFF)"""
        sock_path = get_socket_path()
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(2.0)
                client.connect(sock_path)
                client.sendall(f"SET {state_name}\n".encode("utf-8"))
                resp = client.recv(64).decode("utf-8").strip()
                return resp == "OK"
        except Exception as e:
            logger.debug(f"无法连接 C-GPIO Socket {sock_path} ({e})")
            return False

    @staticmethod
    def get_state() -> str:
        """从 C 守护进程读取当前 LED 状态"""
        sock_path = get_socket_path()
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(2.0)
                client.connect(sock_path)
                client.sendall(b"GET\n")
                return client.recv(64).decode("utf-8").strip()
        except Exception as e:
            logger.debug(f"无法连接 C-GPIO Socket {sock_path} ({e})")
            return "UNKNOWN"
