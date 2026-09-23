#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
网络与物理交互主控守护进程 (Network & Hardware Supervisor)
融合：GPIO按键复位、LED状态机驱动、Wi-Fi热点自启与手机Web配网
"""

import sys
import os
import time
import logging
import signal
import threading

# 确保能正确导入内部模块
CUR_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(CUR_DIR)
if PROJECT_ROOT not in sys.path:
    sys.path.insert(0, PROJECT_ROOT)

from src.gpio_manager import GPIOManager, LEDState, CGPIOClient
from src.wifi_portal import WiFiController, PortalServer

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] (%(name)s) %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S"
)
logger = logging.getLogger("NetworkSupervisor")


def load_config():
    """轻量读取 yaml 配置，若无 pyyaml 库则采用安全默认值"""
    config_path = os.path.join(PROJECT_ROOT, "config", "config.yaml")
    defaults = {
        "button_pin": 17,
        "led_pin": 27,
        "long_press_seconds": 3.0,
        "interface": "wlan0",
        "ap_ssid_prefix": "DebugBox",
        "ap_ip": "192.168.4.1",
        "portal_port": 80,
        "connect_timeout": 25,
        "status_check_interval": 5
    }
    if not os.path.exists(config_path):
        return defaults

    try:
        import yaml
        with open(config_path, "r", encoding="utf-8") as f:
            data = yaml.safe_load(f)
            hw = data.get("hardware", {})
            wf = data.get("wifi", {})
            sys_cfg = data.get("system", {})
            return {
                "button_pin": hw.get("button_pin", defaults["button_pin"]),
                "led_pin": hw.get("led_pin", defaults["led_pin"]),
                "long_press_seconds": hw.get("long_press_seconds", defaults["long_press_seconds"]),
                "interface": wf.get("interface", defaults["interface"]),
                "ap_ssid_prefix": wf.get("ap_ssid_prefix", defaults["ap_ssid_prefix"]),
                "ap_ip": wf.get("ap_ip", defaults["ap_ip"]),
                "portal_port": wf.get("portal_port", defaults["portal_port"]),
                "connect_timeout": wf.get("connect_timeout", defaults["connect_timeout"]),
                "status_check_interval": sys_cfg.get("status_check_interval", defaults["status_check_interval"])
            }
    except Exception as e:
        logger.warning(f"解析 config.yaml 异常 ({e})，使用默认参数运行")
        return defaults


class NetworkSupervisor:
    def __init__(self):
        self.config = load_config()
        self.running = True
        self.in_ap_mode = False
        self._lock = threading.Lock()

        # 初始化控制器
        self.gpio = GPIOManager(
            button_pin=self.config["button_pin"],
            led_pin=self.config["led_pin"],
            long_press_sec=self.config["long_press_seconds"],
            on_reset_callback=self.on_reset_button_triggered
        )

        self.wifi = WiFiController(
            interface=self.config["interface"],
            ap_prefix=self.config["ap_ssid_prefix"],
            ap_ip=self.config["ap_ip"]
        )

        self.portal = PortalServer(
            host="0.0.0.0",
            port=self.config["portal_port"],
            wifi_controller=self.wifi,
            on_connect=self.on_worker_submitted_wifi
        )

    def set_led_state(self, state: LEDState):
        """优先使用 C 语言 GPIO 守护进程，若未启动则回退到 Python 驱动"""
        if CGPIOClient.set_state(state.value):
            return
        self.gpio.set_state(state)

    def start(self):
        logger.info("=== 远程调试盒子网络守护进程启动 ===")
        logger.info(f"配置: 按键=GPIO {self.config['button_pin']}, LED=GPIO {self.config['led_pin']}")
        logger.info(f"热点前缀: {self.config['ap_ssid_prefix']}, IP: {self.config['ap_ip']}")

        # 启动阶段：优先尝试利用已有配置连网
        self.set_led_state(LEDState.CONNECTING)
        time.sleep(2)  # 给 NetworkManager 启动预留时间

        connected = False
        logger.info("正在检测当前是否已有可用 Wi-Fi...")
        for _ in range(3):
            if self.wifi.is_connected_to_wifi():
                connected = True
                break
            time.sleep(2)

        if connected:
            info = self.wifi.get_current_wifi_info()
            logger.info(f"成功连接至已知 Wi-Fi [{info['ssid']}]，IP: {info['ip']}")
            self.set_led_state(LEDState.ONLINE)
            self.in_ap_mode = False
        else:
            logger.warning("未检测到已连接的有效 Wi-Fi，立即切换至 AP 配网模式")
            self._enter_ap_mode()

        # 进入主监控循环
        self._main_loop()

    def _enter_ap_mode(self):
        """进入手机 AP 配网模式"""
        with self._lock:
            self.in_ap_mode = True
            self.set_led_state(LEDState.AP_MODE)
            self.wifi.start_hotspot_ap()
            self.portal.start()
            logger.info(f"【就绪】请工人用手机连接 Wi-Fi [{self.wifi.hotspot_ssid}] 并打开 192.168.4.1 进行配网")

    def _exit_ap_mode(self):
        """退出 AP 模式"""
        with self._lock:
            self.portal.stop()
            self.wifi.stop_hotspot_ap()
            self.in_ap_mode = False

    def on_worker_submitted_wifi(self, ssid: str, password: str):
        """工人从手机网页提交了新的工厂 Wi-Fi"""
        logger.info(f"收到工人配网请求: 尝试连入 [{ssid}]")
        self.set_led_state(LEDState.CONNECTING)
        
        # 关闭配网服务与热点
        self._exit_ap_mode()

        # 尝试连接目标 Wi-Fi
        ok, res = self.wifi.connect_wifi(ssid, password, timeout=self.config["connect_timeout"])
        if ok:
            logger.info(f"成功接入工厂 Wi-Fi [{ssid}]！IP: {res}")
            self.set_led_state(LEDState.ONLINE)
        else:
            logger.error(f"连接工厂 Wi-Fi [{ssid}] 失败: {res}，重新退回配网热点模式")
            time.sleep(2)
            self._enter_ap_mode()

    def on_reset_button_triggered(self):
        """物理按键长按 3 秒触发重置网络"""
        logger.warning(">>> 物理按键触发网络重置 <<<")
        # 清除所有已存网络
        self.wifi.reset_all_saved_wifi()
        time.sleep(1)
        # 重新启动 AP 配网
        self._enter_ap_mode()

    def _main_loop(self):
        """监控主循环：定期检查网络存活"""
        consecutive_lost_count = 0
        try:
            while self.running:
                time.sleep(self.config["status_check_interval"])
                
                # 如果处于配网模式，无需监控掉线
                if self.in_ap_mode:
                    continue

                is_conn = self.wifi.is_connected_to_wifi()
                if not is_conn:
                    consecutive_lost_count += 1
                    logger.warning(f"Wi-Fi 断开连接 ({consecutive_lost_count}/4)")
                    self.set_led_state(LEDState.CONNECTING)

                    # 持续断开超过 4 次检查（约 20 秒），说明网络丢失或工厂环境改变，自动切回配网
                    if consecutive_lost_count >= 4:
                        logger.error("Wi-Fi 持续不可用，自动启动 AP 配网热点自愈")
                        self._enter_ap_mode()
                        consecutive_lost_count = 0
                else:
                    consecutive_lost_count = 0
                    if self.gpio.current_state != LEDState.ONLINE:
                        self.set_led_state(LEDState.ONLINE)

        except KeyboardInterrupt:
            logger.info("收到退出信号，正在关闭服务...")
        finally:
            self.stop()

    def stop(self):
        self.running = False
        self._exit_ap_mode()
        self.gpio.cleanup()
        logger.info("网络守护服务已安全退出")


if __name__ == "__main__":
    supervisor = NetworkSupervisor()

    def sig_handler(sig, frame):
        supervisor.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, sig_handler)
    signal.signal(signal.SIGTERM, sig_handler)
    supervisor.start()
