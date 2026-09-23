import os
import json
import logging
import threading
import socket
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse

logger = logging.getLogger("PortalServer")

class CaptiveDNSServer:
    """极轻量 DNS 泛解析劫持服务 (UDP 53)，促使手机自动弹出全屏配网登录窗口"""
    def __init__(self, ip="192.168.4.1"):
        self.ip = ip
        self.sock = None
        self.running = False
        self.thread = None

    def start(self):
        self.running = True
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.bind(("0.0.0.0", 53))
            self.thread = threading.Thread(target=self._dns_loop, daemon=True)
            self.thread.start()
            logger.info(f"Captive DNS 泛解析劫持服务已在 UDP 53 启动 (全部域名解析至 {self.ip})")
        except Exception as e:
            logger.warning(f"无法启动 UDP 53 DNS 劫持服务: {e}")

    def _dns_loop(self):
        while self.running:
            try:
                data, addr = self.sock.recvfrom(512)
                if len(data) < 12:
                    continue

                # 构造 DNS 应答报文 (Standard Query Response, No Error)
                tid = data[:2]
                flags = b"\x81\x80"
                qdcount = data[4:6]
                ancount = qdcount # 应答数与请求数相同
                nscount = b"\x00\x00"
                arcount = b"\x00\x00"

                # 提取 Question 区段 (跳过 12 字节头部)
                # 构造 Answer: Name 指针 0xc00c, Type A (0x0001), Class IN (0x0001), TTL 60s, Len 4, IP
                ip_parts = [int(p) for p in self.ip.split(".")]
                answer = b"\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04" + bytes(ip_parts)
                response = tid + flags + qdcount + ancount + nscount + arcount + data[12:] + answer
                self.sock.sendto(response, addr)
            except Exception:
                break

    def stop(self):
        self.running = False
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass

class PortalHandler(BaseHTTPRequestHandler):
    wifi_controller = None
    on_connect_requested = None  # 回调函数：触发连接并切换模式
    html_content = ""

    def log_message(self, format, *args):
        # 简化请求日志输出
        logger.debug(f"{self.client_address[0]} - {args[0]}")

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path

        # 1. 常见手机/电脑连上 Wi-Fi 时的 Captive Portal 探针拦截 -> 引导打开主页
        captive_probes = [
            "/generate_204", "/gen_204", "/ncsi.txt",
            "/hotspot-detect.html", "/success.txt", "/canonical.html",
            "/connecttest.txt"
        ]
        if path in captive_probes:
            # 返回 302 重定向到根路径，触发手机弹出配网网页
            self.send_response(302)
            self.send_header("Location", "http://192.168.4.1/")
            self.end_headers()
            return

        # 2. API: 扫描 Wi-Fi 列表
        if path == "/api/scan":
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            
            wifi_list = []
            if PortalHandler.wifi_controller:
                wifi_list = PortalHandler.wifi_controller.scan_wifi_list()
            self.wfile.write(json.dumps(wifi_list, ensure_ascii=False).encode("utf-8"))
            return

        # 3. API: 获取当前状态
        if path == "/api/status":
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.end_headers()
            info = {}
            if PortalHandler.wifi_controller:
                info = PortalHandler.wifi_controller.get_current_wifi_info()
            self.wfile.write(json.dumps(info, ensure_ascii=False).encode("utf-8"))
            return

        # 4. 默认返回配网页面 HTML
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(self.html_content.encode("utf-8"))

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path

        # 提交连接 Wi-Fi 凭证
        if path == "/api/connect":
            content_length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(content_length).decode("utf-8")
            try:
                data = json.loads(body)
                ssid = data.get("ssid", "").strip()
                password = data.get("password", "")

                if not ssid:
                    self._send_json({"status": "error", "message": "SSID 不能为空"}, status=400)
                    return

                logger.info(f"收到手机提交的 Wi-Fi 配置: SSID={ssid}")
                
                # 先给前端响应成功，告知已收到并正在切换
                self._send_json({"status": "ok", "message": "配置已接收，正在连接..."})

                # 异步触发系统网络连接动作，防止客户端未收到 HTTP 返回就断网
                if PortalHandler.on_connect_requested:
                    threading.Timer(1.0, PortalHandler.on_connect_requested, args=(ssid, password)).start()

            except Exception as e:
                logger.error(f"处理配网请求失败: {e}")
                self._send_json({"status": "error", "message": str(e)}, status=500)
            return

        self.send_response(404)
        self.end_headers()

    def _send_json(self, obj, status=200):
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.end_headers()
        self.wfile.write(json.dumps(obj, ensure_ascii=False).encode("utf-8"))


class PortalServer:
    def __init__(self, host="0.0.0.0", port=80, wifi_controller=None, on_connect=None):
        self.host = host
        self.port = port
        self.wifi_controller = wifi_controller
        self.on_connect = on_connect
        self.server = None
        self.thread = None
        dns_ip = self.wifi_controller.ap_ip if self.wifi_controller else "192.168.4.1"
        self.dns_server = CaptiveDNSServer(ip=dns_ip)
        self._load_html()

    def _load_html(self):
        cur_dir = os.path.dirname(os.path.abspath(__file__))
        html_path = os.path.join(cur_dir, "templates", "index.html")
        try:
            with open(html_path, "r", encoding="utf-8") as f:
                PortalHandler.html_content = f.read()
        except Exception as e:
            logger.error(f"读取 index.html 失败: {e}")
            PortalHandler.html_content = "<h1>Wi-Fi Portal Loading Error</h1>"

    def start(self):
        PortalHandler.wifi_controller = self.wifi_controller
        PortalHandler.on_connect_requested = self.on_connect

        # 启动 UDP 53 DNS 泛解析劫持 (促使手机全屏弹窗)
        self.dns_server.start()

        # 如果标准 80 端口无权限（非 root），可以备选 8080
        try:
            self.server = HTTPServer((self.host, self.port), PortalHandler)
            logger.info(f"配网 HTTP 服务已在 {self.host}:{self.port} 启动")
        except PermissionError:
            self.port = 8080
            self.server = HTTPServer((self.host, self.port), PortalHandler)
            logger.warning(f"无法监听 80 端口，转为监听备用端口 {self.port}")

        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def stop(self):
        self.dns_server.stop()
        if self.server:
            logger.info("关闭配网 HTTP 服务...")
            self.server.shutdown()
            self.server.server_close()
            self.server = None
