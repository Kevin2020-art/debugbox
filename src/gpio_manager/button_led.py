#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
GPIO 管理器：负责按键长按检测与状态指示灯（LED）的多模式闪烁
支持树莓派环境 (gpiozero) 与开发调试模拟环境 (Mock)
"""

import time
import threading
import logging
from enum import Enum

logger = logging.getLogger("GPIOManager")

class LEDState(Enum):
    OFF = "OFF"
    AP_MODE = "AP_MODE"          # 慢闪 (0.8s开, 0.8s关)：热点开启，等待手机配网
    CONNECTING = "CONNECTING"    # 中闪 (0.25s开, 0.25s关)：正在尝试连接工厂 Wi-Fi
    ONLINE = "ONLINE"            # 双跳心跳闪烁：已连接网络，工作正常
    RESETTING = "RESETTING"      # 急促狂闪 (0.08s开, 0.08s关)：按键长按达标，触发复位确认

class GPIOManager:
    def __init__(self, button_pin=17, led_pin=27, long_press_sec=3.0, on_reset_callback=None):
        self.button_pin = button_pin
        self.led_pin = led_pin
        self.long_press_sec = long_press_sec
        self.on_reset_callback = on_reset_callback

        self.current_state = LEDState.OFF
        self._stop_led_thread = False
        self._led_thread = None

        self._press_start_time = None
        self._is_holding = False
        self._long_press_triggered = False
        self._check_timer = None

        # 尝试初始化真实 GPIO，失败则切换至 Mock 模式
        self.is_mock = False
        self._init_hardware()

    def _init_hardware(self):
        try:
            from gpiozero import Button, LED
            self.button = Button(
                self.button_pin,
                pull_up=True,
                bounce_time=0.05,
                hold_time=self.long_press_sec
            )
            self.led = LED(self.led_pin)
            
            # 使用 gpiozero 原生事件
            self.button.when_held = self._handle_button_held
            self.button.when_pressed = self._handle_button_pressed
            self.button.when_released = self._handle_button_released
            logger.info(f"真实 GPIO 初始化成功 (Button: GPIO {self.button_pin}, LED: GPIO {self.led_pin})")
        except Exception as e:
            self.is_mock = True
            logger.warning(f"无法初始化真实 GPIO 硬件 ({e})，自动进入模拟 (Mock) 模式")

    # ==================== LED 状态机 ====================

    def set_state(self, state: LEDState):
        if self.current_state == state:
            return
        logger.info(f"LED 状态切换: {self.current_state.value} -> {state.value}")
        self.current_state = state
        self._restart_led_loop()

    def _restart_led_loop(self):
        self._stop_led_thread = True
        if self._led_thread and self._led_thread.is_alive():
            self._led_thread.join(timeout=0.5)
        
        self._stop_led_thread = False
        self._led_thread = threading.Thread(target=self._led_worker, daemon=True)
        self._led_thread.start()

    def _set_led_physical(self, on: bool):
        if not self.is_mock:
            try:
                if on:
                    self.led.on()
                else:
                    self.led.off()
            except Exception as e:
                logger.error(f"控制 LED 失败: {e}")
        else:
            # 模拟模式下仅记录或静默
            pass

    def _led_worker(self):
        """根据当前状态执行不同的闪烁模式"""
        while not self._stop_led_thread:
            state = self.current_state

            if state == LEDState.OFF:
                self._set_led_physical(False)
                time.sleep(0.2)

            elif state == LEDState.AP_MODE:
                # 慢闪：0.8s 开，0.8s 关
                self._set_led_physical(True)
                time.sleep(0.8)
                self._set_led_physical(False)
                time.sleep(0.8)

            elif state == LEDState.CONNECTING:
                # 中闪：0.25s 开，0.25s 关
                self._set_led_physical(True)
                time.sleep(0.25)
                self._set_led_physical(False)
                time.sleep(0.25)

            elif state == LEDState.ONLINE:
                # 心跳双跳：开0.1s -> 关0.15s -> 开0.1s -> 关1.65s (周期 2.0s)
                self._set_led_physical(True)
                time.sleep(0.1)
                self._set_led_physical(False)
                time.sleep(0.15)
                self._set_led_physical(True)
                time.sleep(0.1)
                self._set_led_physical(False)
                time.sleep(1.65)

            elif state == LEDState.RESETTING:
                # 急促狂闪：0.08s 开，0.08s 关
                self._set_led_physical(True)
                time.sleep(0.08)
                self._set_led_physical(False)
                time.sleep(0.08)

    # ==================== 按键处理逻辑 ====================

    def _handle_button_pressed(self):
        logger.debug("复位按键被按下")
        self._press_start_time = time.time()
        self._long_press_triggered = False

    def _handle_button_held(self):
        """按住超过 long_press_sec 触发"""
        logger.warning(f"检测到按键长按满 {self.long_press_sec} 秒！触发系统网络重置")
        self._long_press_triggered = True
        
        # 立即给予操作反馈：LED 狂闪 1.5 秒告知工人“重置已接受，请松开”
        self.set_state(LEDState.RESETTING)

        if self.on_reset_callback:
            try:
                # 在独立线程执行重置回调，避免阻塞 GPIO 事件
                threading.Thread(target=self.on_reset_callback, daemon=True).start()
            except Exception as e:
                logger.error(f"执行重置回调失败: {e}")

    def _handle_button_released(self):
        press_duration = time.time() - (self._press_start_time or time.time())
        logger.debug(f"复位按键松开，按下时长: {press_duration:.2f}s")
        self._press_start_time = None

    # 供测试用的模拟触发方法
    def mock_trigger_long_press(self):
        """模拟外部长按按键"""
        logger.info("[Mock] 模拟外部触发长按按键")
        self._handle_button_pressed()
        time.sleep(0.1)
        self._handle_button_held()
        time.sleep(0.1)
        self._handle_button_released()

    def cleanup(self):
        self._stop_led_thread = True
        if not self.is_mock:
            try:
                self.led.off()
                self.led.close()
                self.button.close()
            except Exception:
                pass
        logger.info("GPIO 资源已清理")
