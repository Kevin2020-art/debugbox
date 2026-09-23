#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
第一步功能自动化验证脚本 (Step 1 Verification Test)
测试：
1. GPIOManager 状态机流转与长按回调
2. WiFiController 热点名称生成与扫描数据格式化
3. PortalServer 网页服务、Captive Portal 重定向及 API 接口调用
"""

import sys
import os
import time
import urllib.request
import json

# 导入模块
CUR_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(CUR_DIR)
sys.path.insert(0, PROJECT_ROOT)

from src.gpio_manager import GPIOManager, LEDState
from src.wifi_portal import WiFiController, PortalServer


def test_gpio_manager():
    print("\n[1/3] 测试 GPIOManager 状态机与按键回调...")
    reset_triggered = False

    def on_reset():
        nonlocal reset_triggered
        reset_triggered = True

    manager = GPIOManager(button_pin=17, led_pin=27, long_press_sec=0.2, on_reset_callback=on_reset)
    
    # 状态机测试
    assert manager.current_state == LEDState.OFF
    manager.set_state(LEDState.AP_MODE)
    assert manager.current_state == LEDState.AP_MODE
    manager.set_state(LEDState.ONLINE)
    assert manager.current_state == LEDState.ONLINE

    # 模拟长按测试
    manager.mock_trigger_long_press()
    time.sleep(0.3)
    assert reset_triggered, "长按按键未成功触发 on_reset 回调！"
    assert manager.current_state == LEDState.RESETTING

    manager.cleanup()
    print("✓ GPIO 状态机与长按重置逻辑验证通过！")


def test_wifi_controller():
    print("\n[2/3] 测试 WiFiController 逻辑...")
    wifi = WiFiController(interface="wlan0", ap_prefix="DebugBox")
    
    # 测试 SSID 生成
    assert wifi.hotspot_ssid.startswith("DebugBox-")
    print(f"  生成的热点 SSID: {wifi.hotspot_ssid}")

    # 测试周边 Wi-Fi 扫描结果
    scan_results = wifi.scan_wifi_list()
    assert isinstance(scan_results, list)
    assert len(scan_results) > 0
    assert "ssid" in scan_results[0]
    assert "signal" in scan_results[0]
    print(f"  扫描返回热点数量: {len(scan_results)}, 首个热点: {scan_results[0]['ssid']}")

    print("✓ WiFiController 基本逻辑验证通过！")


def test_portal_server():
    print("\n[3/3] 测试 PortalServer 配网 Web 服务与 API...")
    wifi = WiFiController(interface="wlan0")
    received_connect = {}

    def on_connect_mock(ssid, pwd):
        received_connect["ssid"] = ssid
        received_connect["pwd"] = pwd

    # 启动在测试端口 8899
    server = PortalServer(host="127.0.0.1", port=8899, wifi_controller=wifi, on_connect=on_connect_mock)
    server.start()
    time.sleep(0.5)

    base_url = f"http://127.0.0.1:{server.port}"

    try:
        # 1. 测试访问根页面
        req = urllib.request.urlopen(f"{base_url}/")
        html = req.read().decode("utf-8")
        assert "工厂 Wi-Fi 快速配置" in html
        print("  ✓ 根页面 HTML 加载成功")

        # 2. 测试 Captive Portal 探针重定向 (例如 Android generate_204)
        opener = urllib.request.build_opener(urllib.request.HTTPRedirectHandler)
        # 禁止自动跳转以捕获 302
        class NoRedirectHandler(urllib.request.HTTPRedirectHandler):
            def http_error_302(self, req, fp, code, msg, headers):
                return fp
        
        custom_opener = urllib.request.build_opener(NoRedirectHandler)
        resp = custom_opener.open(f"{base_url}/generate_204")
        assert resp.status == 302
        assert "192.168.4.1" in resp.headers.get("Location")
        print("  ✓ Captive Portal 强制弹窗探测响应正常 (302 Redirect)")

        # 3. 测试 API /api/scan
        req = urllib.request.urlopen(f"{base_url}/api/scan")
        data = json.loads(req.read().decode("utf-8"))
        assert isinstance(data, list)
        print(f"  ✓ /api/scan 接口正常，返回 {len(data)} 个热点")

        # 4. 测试 API POST /api/connect
        post_data = json.dumps({"ssid": "Test-Factory-WiFi", "password": "SafePassword123"}).encode("utf-8")
        post_req = urllib.request.Request(
            f"{base_url}/api/connect",
            data=post_data,
            headers={"Content-Type": "application/json"}
        )
        resp = urllib.request.urlopen(post_req)
        resp_data = json.loads(resp.read().decode("utf-8"))
        assert resp_data["status"] == "ok"
        time.sleep(1.2)
        assert received_connect.get("ssid") == "Test-Factory-WiFi"
        assert received_connect.get("pwd") == "SafePassword123"
        print("  ✓ /api/connect 配网提交接口正常，已成功触发连接任务")

    finally:
        server.stop()

    print("✓ PortalServer 所有功能验证通过！")


if __name__ == "__main__":
    print("================ 开始第一步单元与逻辑验证 ================")
    test_gpio_manager()
    test_wifi_controller()
    test_portal_server()
    print("\n================ 第一步所有测试全部通过 (PASSED) ================")
