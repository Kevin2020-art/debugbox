#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
硬件引脚连通性自检脚本 (Hardware Smoke Test)
用于在正式启动系统前，验证按键与 LED 的物理连线是否正确。
"""

import sys
import time

print("=" * 60)
print("  树莓派 4B 远程调试盒 - 硬件接线自检程序")
print("=" * 60)

try:
    from gpiozero import Button, LED
except ImportError:
    print("[错误] 未检测到 gpiozero 库。")
    print("请先在树莓派终端运行: sudo apt-get install -y python3-gpiozero")
    sys.exit(1)

BUTTON_PIN = 17  # Pin 11
LED_PIN = 27     # Pin 13

print(f"\n[1/3] 正在初始化硬件端口 (Button: GPIO {BUTTON_PIN}, LED: GPIO {LED_PIN})...")
try:
    led = LED(LED_PIN)
    button = Button(BUTTON_PIN, pull_up=True, bounce_time=0.05, hold_time=3.0)
    print("✓ 端口初始化成功！")
except Exception as e:
    print(f"[失败] 无法打开 GPIO 端口: {e}")
    sys.exit(1)

# ----------------- 步骤 1: LED 点亮测试 -----------------
print("\n[2/3] 测试 LED 灯硬件连线...")
print("  >> 正在点亮 LED (持续 2 秒)... 请观察绿灯是否亮起")
led.on()
time.sleep(2)
led.off()

print("  >> 正在测试 3 次快闪...")
for i in range(3):
    led.on()
    time.sleep(0.2)
    led.off()
    time.sleep(0.2)

# ----------------- 步骤 2: 按键交互测试 -----------------
print("\n[3/3] 测试复位按键硬件连线...")
print("  >> 请【轻按一下】复位按键（用于测试单次点击触发）...")

pressed_flag = False

def on_press():
    global pressed_flag
    pressed_flag = True
    print("\n  [成功检测到按下] 按键已闭合 (Pin 11 -> GND 通路正常)！")
    led.on()

def on_release():
    print("  [检测到松开] 按键断开，LED 熄灭。")
    led.off()

button.when_pressed = on_press
button.when_released = on_release

timeout = 15
start_time = time.time()
while not pressed_flag and (time.time() - start_time < timeout):
    time.sleep(0.1)

if not pressed_flag:
    print("\n  [超时未检测到按键] 请检查：")
    print("  1. 按键是否确实插在 Pin 11 (GPIO 17) 与 Pin 9 (GND) 上？")
    print("  2. 面包板或轻触开关引脚方向是否插反（两组常开触点）？")
else:
    print("\n  >> 接下来测试【长按 3 秒】重置功能...")
    print("  >> 请按住按键不放，保持 3 秒以上...")
    
    held_flag = False
    def on_hold():
        nonlocal held_flag
        held_flag = True
        print("\n  🎉 [长按 3 秒达标！] 成功检测到长按事件！")
        # 触发快速闪烁提示
        for _ in range(5):
            led.on()
            time.sleep(0.08)
            led.off()
            time.sleep(0.08)
        print("  >> 现在可以松手了！")

    button.when_held = on_hold
    
    start_time = time.time()
    while not held_flag and (time.time() - start_time < timeout):
        time.sleep(0.1)

    if held_flag:
        print("\n" + "=" * 60)
        print("  ✓ 恭喜！按键与 LED 硬件接线 100% 正确无误！")
        print("=" * 60)
    else:
        print("\n  [提示] 未在 15 秒内检测到持续 3 秒的长按。")

# 清理退出
led.close()
button.close()
