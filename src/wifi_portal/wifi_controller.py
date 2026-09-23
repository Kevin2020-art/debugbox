#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Wi-Fi 控制层：基于 Linux NetworkManager (nmcli) 实现
提供：扫描热点、开启配网AP、连接工厂Wi-Fi、重置清除配置
"""

import subprocess
import logging
import re
import time
import socket

logger = logging.getLogger("WiFiController")

class WiFiController:
    def __init__(self, interface="wlan0", ap_prefix="DebugBox", ap_ip="192.168.4.1", ap_password=""):
        self.interface = interface
        self.ap_prefix = ap_prefix
        self.ap_ip = ap_ip
        self.ap_password = ap_password
        self.ap_conn_name = "DebugBox-Hotspot"
        
        self.is_linux = self._check_nmcli_available()
        # 缓存设备唯一 SSID (基于 MAC 后 4 位)
        self.hotspot_ssid = self._generate_hotspot_ssid()

    def _check_nmcli_available(self) -> bool:
        try:
            res = subprocess.run(["which", "nmcli"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            return res.returncode == 0
        except Exception:
            return False

    def _run_cmd(self, cmd_list, timeout=20):
        if not self.is_linux:
            logger.debug(f"[Mock] 运行命令: {' '.join(cmd_list)}")
            return True, ""
        try:
            res = subprocess.run(cmd_list, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout)
            if res.returncode != 0:
                logger.warning(f"命令执行失败 ({res.returncode}): {' '.join(cmd_list)} | Err: {res.stderr.strip()}")
                return False, res.stderr.strip()
            return True, res.stdout.strip()
        except subprocess.TimeoutExpired:
            logger.error(f"命令执行超时: {' '.join(cmd_list)}")
            return False, "timeout"
        except Exception as e:
            logger.error(f"命令执行异常: {e}")
            return False, str(e)

    def _get_mac_address(self) -> str:
        """获取无线网卡 MAC 地址"""
        if not self.is_linux:
            return "B827EB123456"
        try:
            with open(f"/sys/class/net/{self.interface}/address", "r") as f:
                return f.read().strip().replace(":", "").upper()
        except Exception:
            return "4B0000"

    def _generate_hotspot_ssid(self) -> str:
        mac = self._get_mac_address()
        suffix = mac[-4:] if len(mac) >= 4 else "DEMO"
        return f"{self.ap_prefix}-{suffix}"

    # ==================== Wi-Fi 状态查询 ====================

    def is_connected_to_wifi(self) -> bool:
        """判断当前 wlan0 是否已经作为客户端连上 Wi-Fi"""
        if not self.is_linux:
            return False
        # 检查是否处于激活连接状态
        ok, out = self._run_cmd(["nmcli", "-t", "-f", "DEVICE,TYPE,STATE", "device"])
        if not ok:
            return False
        for line in out.splitlines():
            parts = line.split(":")
            if len(parts) >= 3 and parts[0] == self.interface and parts[1] == "wifi":
                return parts[2] == "connected"
        return False

    def check_internet_reachability(self, timeout=2) -> bool:
        """检查互联网可达性 (快速向公共 DNS 发起握手探测)"""
        # 尝试几个常用公共 DNS
        test_ips = [("223.5.5.5", 53), ("114.114.114.114", 53), ("8.8.8.8", 53)]
        for ip, port in test_ips:
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(timeout)
                sock.connect((ip, port))
                sock.close()
                return True
            except Exception:
                continue
        return False

    def get_current_wifi_info(self):
        """获取当前已连接 Wi-Fi 的 SSID 和 IP 地址"""
        if not self.is_linux:
            return {"ssid": "MockFactoryWiFi", "ip": "192.168.1.188"}
        
        ssid = "Unknown"
        ip = ""
        # 获取 SSID
        ok, out = self._run_cmd(["nmcli", "-t", "-f", "ACTIVE,SSID", "dev", "wifi"])
        if ok:
            for line in out.splitlines():
                if line.startswith("yes:"):
                    ssid = line.split(":", 1)[1]
                    break
        # 获取 IP
        ok, out = self._run_cmd(["nmcli", "-t", "-f", "IP4.ADDRESS", "dev", "show", self.interface])
        if ok and out:
            # 第一行通常为 IP/掩码
            first_line = out.splitlines()[0]
            ip = first_line.split("/")[0]

        return {"ssid": ssid, "ip": ip}

    # ==================== 扫描周边 Wi-Fi ====================

    def scan_wifi_list(self):
        """扫描周边热点并去重排序，返回适合前端展示的列表"""
        if not self.is_linux:
            return [
                {"ssid": "Factory-Office-5G", "signal": 95, "security": "WPA2"},
                {"ssid": "Production-Line-1", "signal": 80, "security": "WPA2"},
                {"ssid": "Workshop-Guest", "signal": 65, "security": "Open"},
                {"ssid": "Warehouse-IoT", "signal": 40, "security": "WPA2"}
            ]

        # 触发重新扫描
        self._run_cmd(["nmcli", "dev", "wifi", "rescan", "ifname", self.interface], timeout=8)
        time.sleep(1)

        ok, out = self._run_cmd([
            "nmcli", "-t", "-f", "SSID,SIGNAL,SECURITY", "dev", "wifi", "list", "ifname", self.interface
        ], timeout=10)

        if not ok or not out:
            return []

        wifi_map = {}
        for line in out.splitlines():
            parts = line.split(":")
            if len(parts) >= 3:
                ssid = parts[0].strip()
                if not ssid or ssid == "--":
                    continue
                try:
                    signal = int(parts[1].strip())
                except ValueError:
                    signal = 0
                security = parts[2].strip() or "Open"
                
                # 如果同一 SSID 存在多个 AP，保留信号最好的一个
                if ssid not in wifi_map or signal > wifi_map[ssid]["signal"]:
                    wifi_map[ssid] = {
                        "ssid": ssid,
                        "signal": signal,
                        "security": security
                    }

        # 按信号强度从高到低排序
        sorted_list = sorted(wifi_map.values(), key=lambda x: x["signal"], reverse=True)
        return sorted_list

    # ==================== AP 模式与连接控制 ====================

    def start_hotspot_ap(self) -> bool:
        """
        切换为 AP 热点模式：
        使用 NetworkManager 的 shared 模式，自动启动 DHCP 为手机下发 192.168.4.x 地址
        """
        logger.info(f"正在启动配网热点 AP: SSID={self.hotspot_ssid}, IP={self.ap_ip}")
        if not self.is_linux:
            logger.info(f"[Mock] 成功开启 AP: {self.hotspot_ssid}")
            return True

        # 1. 确保旧的 AP 配置被清除
        self._run_cmd(["nmcli", "con", "delete", self.ap_conn_name])

        # 2. 创建并激活 AP 热点配置
        cmd = [
            "nmcli", "con", "add",
            "type", "wifi",
            "ifname", self.interface,
            "con-name", self.ap_conn_name,
            "autoconnect", "no",
            "ssid", self.hotspot_ssid,
            "mode", "ap",
            "802-11-wireless.band", "bg",
            "802-11-wireless.channel", "6",
            "ipv4.method", "shared",
            "ipv4.addresses", f"{self.ap_ip}/24"
        ]

        if self.ap_password and len(self.ap_password) >= 8:
            cmd.extend([
                "wifi-sec.key-mgmt", "wpa-psk",
                "wifi-sec.psk", self.ap_password
            ])

        ok, out = self._run_cmd(cmd)
        if not ok:
            logger.error(f"创建 AP 配置失败: {out}")
            return False

        # 3. 启用热点
        ok, out = self._run_cmd(["nmcli", "con", "up", self.ap_conn_name], timeout=15)
        if not ok:
            logger.error(f"激活 AP 热点失败: {out}")
            return False

        logger.info(f"配网热点 [{self.hotspot_ssid}] 启动成功！")
        return True

    def stop_hotspot_ap(self):
        """关闭热点"""
        logger.info("关闭配网热点...")
        if not self.is_linux:
            return True
        self._run_cmd(["nmcli", "con", "down", self.ap_conn_name])
        self._run_cmd(["nmcli", "con", "delete", self.ap_conn_name])

    def connect_wifi(self, ssid: str, password: str, timeout=25) -> (bool, str):
        """
        尝试连接目标 Wi-Fi
        """
        logger.info(f"正在尝试连接 Wi-Fi: {ssid}")
        if not self.is_linux:
            logger.info(f"[Mock] 模拟连接成功: {ssid}")
            return True, "192.168.1.188"

        # 连接前先关闭热点
        self.stop_hotspot_ap()
        time.sleep(1)

        cmd = ["nmcli", "dev", "wifi", "connect", ssid]
        if password:
            cmd.extend(["password", password])
        cmd.extend(["ifname", self.interface])

        ok, out = self._run_cmd(cmd, timeout=timeout)
        if not ok:
            logger.warning(f"连接 Wi-Fi [{ssid}] 失败: {out}")
            return False, out

        # 等待分配 IP 地址
        for _ in range(8):
            info = self.get_current_wifi_info()
            if info["ip"]:
                logger.info(f"成功连入 [{ssid}]，获取到 IP: {info['ip']}")
                return True, info["ip"]
            time.sleep(1)

        return True, "IP分配中"

    def reset_all_saved_wifi(self) -> bool:
        """
        重置清空所有保存过的 Wi-Fi 配置
        """
        logger.warning("正在清空所有保存的历史 Wi-Fi 连接...")
        if not self.is_linux:
            return True

        # 列出所有无线网络连接配置
        ok, out = self._run_cmd(["nmcli", "-t", "-f", "NAME,TYPE", "connection", "show"])
        if ok and out:
            for line in out.splitlines():
                parts = line.split(":")
                if len(parts) >= 2 and parts[1] == "802-11-wireless":
                    conn_name = parts[0]
                    logger.info(f"删除 Wi-Fi 配置: {conn_name}")
                    self._run_cmd(["nmcli", "con", "delete", conn_name])

        return True
