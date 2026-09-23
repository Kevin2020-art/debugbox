#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "gpio_controller.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

static void msleep_interruptible(gpio_controller_t *ctrl, led_state_t expected_state, int ms) {
    int chunks = ms / 20;
    int rem = ms % 20;
    for (int i = 0; i < chunks; i++) {
        if (!ctrl->running || ctrl->current_state != expected_state) {
            return;
        }
        usleep(20000);
    }
    if (rem > 0 && ctrl->running && ctrl->current_state == expected_state) {
        usleep(rem * 1000);
    }
}

static void set_led_physical(gpio_controller_t *ctrl, bool on) {
    if (!ctrl->led_request) return;
    enum gpiod_line_value val = on ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
    gpiod_line_request_set_value(ctrl->led_request, ctrl->led_pin, val);
}

/* ==================== LED 闪烁工作线程 ==================== */
static void *led_worker(void *arg) {
    gpio_controller_t *ctrl = (gpio_controller_t *)arg;

    while (ctrl->running) {
        led_state_t st = ctrl->current_state;

        switch (st) {
            case LED_STATE_OFF:
                set_led_physical(ctrl, false);
                msleep_interruptible(ctrl, st, 100);
                break;

            case LED_STATE_AP_MODE:
                /* 慢闪：0.8s 开，0.8s 关 */
                set_led_physical(ctrl, true);
                msleep_interruptible(ctrl, st, 800);
                set_led_physical(ctrl, false);
                msleep_interruptible(ctrl, st, 800);
                break;

            case LED_STATE_CONNECTING:
                /* 中闪：0.25s 开，0.25s 关 */
                set_led_physical(ctrl, true);
                msleep_interruptible(ctrl, st, 250);
                set_led_physical(ctrl, false);
                msleep_interruptible(ctrl, st, 250);
                break;

            case LED_STATE_ONLINE:
                /* 心跳双闪：0.1s开 -> 0.15s关 -> 0.1s开 -> 1.65s关 (周期 2.0s) */
                set_led_physical(ctrl, true);
                msleep_interruptible(ctrl, st, 100);
                set_led_physical(ctrl, false);
                msleep_interruptible(ctrl, st, 150);
                set_led_physical(ctrl, true);
                msleep_interruptible(ctrl, st, 100);
                set_led_physical(ctrl, false);
                msleep_interruptible(ctrl, st, 1650);
                break;

            case LED_STATE_RESETTING:
                /* 急促狂闪：0.08s 开，0.08s 关 */
                set_led_physical(ctrl, true);
                msleep_interruptible(ctrl, st, 80);
                set_led_physical(ctrl, false);
                msleep_interruptible(ctrl, st, 80);
                break;

            default:
                set_led_physical(ctrl, false);
                msleep_interruptible(ctrl, st, 100);
                break;
        }
    }

    set_led_physical(ctrl, false);
    return NULL;
}

/* ==================== 按键长按监听工作线程 ==================== */
static double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void *btn_worker(void *arg) {
    gpio_controller_t *ctrl = (gpio_controller_t *)arg;

    bool is_pressed = false;
    double press_start = 0.0;
    bool triggered = false;

    while (ctrl->running) {
        /* 读取 Pin 17 电平：开启了弱上拉，按键接地为 INACTIVE (0 = 按下) */
        enum gpiod_line_value val = gpiod_line_request_get_value(ctrl->btn_request, ctrl->button_pin);
        bool pressed_now = (val == GPIOD_LINE_VALUE_INACTIVE);

        double now = get_time_sec();

        if (pressed_now) {
            if (!is_pressed) {
                /* 按键初次闭合 */
                is_pressed = true;
                press_start = now;
                triggered = false;
            } else {
                /* 持续按住中，检查是否达到长按判定时间 */
                if (!triggered && (now - press_start >= ctrl->long_press_sec)) {
                    triggered = true;
                    fprintf(stderr, "[C-GPIO] 检测到按键长按满 %.1f 秒！触发网络复位\n", ctrl->long_press_sec);
                    
                    /* 立即给予视觉反馈：切换至狂闪 */
                    gpio_controller_set_led_state(ctrl, LED_STATE_RESETTING);

                    /* 触发复位回调 */
                    if (ctrl->on_reset) {
                        ctrl->on_reset(ctrl->user_data);
                    }
                }
            }
        } else {
            if (is_pressed) {
                /* 按键松开 */
                is_pressed = false;
                triggered = false;
            }
        }

        /* 10ms 轮询检测，CPU 占用低于 0.1% */
        usleep(10000);
    }

    return NULL;
}

/* ==================== 接口实现 ==================== */

int gpio_controller_init(gpio_controller_t *ctrl,
                         const char *chip_path,
                         unsigned int button_pin,
                         unsigned int led_pin,
                         double long_press_sec,
                         reset_callback_t on_reset,
                         void *user_data) {
    if (!ctrl) return -1;
    memset(ctrl, 0, sizeof(*ctrl));

    strncpy(ctrl->chip_path, chip_path ? chip_path : "/dev/gpiochip0", sizeof(ctrl->chip_path) - 1);
    ctrl->button_pin = button_pin;
    ctrl->led_pin = led_pin;
    ctrl->long_press_sec = (long_press_sec > 0.1) ? long_press_sec : 3.0;
    ctrl->on_reset = on_reset;
    ctrl->user_data = user_data;
    ctrl->current_state = LED_STATE_OFF;
    pthread_mutex_init(&ctrl->state_mutex, NULL);

    /* 1. 打开 GPIO Chip */
    ctrl->chip = gpiod_chip_open(ctrl->chip_path);
    if (!ctrl->chip) {
        fprintf(stderr, "[C-GPIO] 无法打开 GPIO Chip: %s (%s)\n", ctrl->chip_path, strerror(errno));
        return -1;
    }

    /* 2. 配置 LED 引脚为 OUTPUT */
    struct gpiod_line_settings *led_settings = gpiod_line_settings_new();
    if (!led_settings) goto fail_chip;
    gpiod_line_settings_set_direction(led_settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(led_settings, GPIOD_LINE_VALUE_INACTIVE);

    struct gpiod_line_config *led_cfg = gpiod_line_config_new();
    if (!led_cfg) {
        gpiod_line_settings_free(led_settings);
        goto fail_chip;
    }
    gpiod_line_config_add_line_settings(led_cfg, &ctrl->led_pin, 1, led_settings);
    ctrl->led_request = gpiod_chip_request_lines(ctrl->chip, NULL, led_cfg);
    gpiod_line_config_free(led_cfg);
    gpiod_line_settings_free(led_settings);

    if (!ctrl->led_request) {
        fprintf(stderr, "[C-GPIO] 请求 LED 引脚 (Pin %u) 失败 (%s)\n", ctrl->led_pin, strerror(errno));
        goto fail_chip;
    }

    /* 3. 配置 按键 引脚为 INPUT 并开启内部 PULL-UP */
    struct gpiod_line_settings *btn_settings = gpiod_line_settings_new();
    if (!btn_settings) goto fail_led;
    gpiod_line_settings_set_direction(btn_settings, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_bias(btn_settings, GPIOD_LINE_BIAS_PULL_UP);

    struct gpiod_line_config *btn_cfg = gpiod_line_config_new();
    if (!btn_cfg) {
        gpiod_line_settings_free(btn_settings);
        goto fail_led;
    }
    gpiod_line_config_add_line_settings(btn_cfg, &ctrl->button_pin, 1, btn_settings);
    ctrl->btn_request = gpiod_chip_request_lines(ctrl->chip, NULL, btn_cfg);
    gpiod_line_config_free(btn_cfg);
    gpiod_line_settings_free(btn_settings);

    if (!ctrl->btn_request) {
        fprintf(stderr, "[C-GPIO] 请求按键引脚 (Pin %u) 失败 (%s)\n", ctrl->button_pin, strerror(errno));
        goto fail_led;
    }

    fprintf(stderr, "[C-GPIO] 硬件引脚初始化完成 (Chip: %s, Button: GPIO %u, LED: GPIO %u)\n",
            ctrl->chip_path, ctrl->button_pin, ctrl->led_pin);
    return 0;

fail_led:
    if (ctrl->led_request) gpiod_line_request_release(ctrl->led_request);
fail_chip:
    if (ctrl->chip) gpiod_chip_close(ctrl->chip);
    return -1;
}

int gpio_controller_start(gpio_controller_t *ctrl) {
    if (!ctrl) return -1;
    ctrl->running = true;

    if (pthread_create(&ctrl->led_thread, NULL, led_worker, ctrl) != 0) {
        perror("[C-GPIO] 创建 LED 线程失败");
        ctrl->running = false;
        return -1;
    }

    if (pthread_create(&ctrl->btn_thread, NULL, btn_worker, ctrl) != 0) {
        perror("[C-GPIO] 创建 按键 线程失败");
        ctrl->running = false;
        pthread_join(ctrl->led_thread, NULL);
        return -1;
    }

    return 0;
}

void gpio_controller_stop(gpio_controller_t *ctrl) {
    if (!ctrl || !ctrl->running) return;
    ctrl->running = false;
    pthread_join(ctrl->led_thread, NULL);
    pthread_join(ctrl->btn_thread, NULL);
}

void gpio_controller_destroy(gpio_controller_t *ctrl) {
    if (!ctrl) return;
    gpio_controller_stop(ctrl);

    if (ctrl->led_request) {
        set_led_physical(ctrl, false);
        gpiod_line_request_release(ctrl->led_request);
        ctrl->led_request = NULL;
    }
    if (ctrl->btn_request) {
        gpiod_line_request_release(ctrl->btn_request);
        ctrl->btn_request = NULL;
    }
    if (ctrl->chip) {
        gpiod_chip_close(ctrl->chip);
        ctrl->chip = NULL;
    }

    pthread_mutex_destroy(&ctrl->state_mutex);
}

void gpio_controller_set_led_state(gpio_controller_t *ctrl, led_state_t state) {
    if (!ctrl) return;
    pthread_mutex_lock(&ctrl->state_mutex);
    if (ctrl->current_state != state) {
        fprintf(stderr, "[C-GPIO] LED 状态切换: %s -> %s\n",
                led_state_to_string(ctrl->current_state),
                led_state_to_string(state));
        ctrl->current_state = state;
    }
    pthread_mutex_unlock(&ctrl->state_mutex);
}

led_state_t gpio_controller_get_led_state(gpio_controller_t *ctrl) {
    if (!ctrl) return LED_STATE_OFF;
    pthread_mutex_lock(&ctrl->state_mutex);
    led_state_t st = ctrl->current_state;
    pthread_mutex_unlock(&ctrl->state_mutex);
    return st;
}

const char *led_state_to_string(led_state_t state) {
    switch (state) {
        case LED_STATE_OFF: return "OFF";
        case LED_STATE_AP_MODE: return "AP_MODE";
        case LED_STATE_CONNECTING: return "CONNECTING";
        case LED_STATE_ONLINE: return "ONLINE";
        case LED_STATE_RESETTING: return "RESETTING";
        default: return "UNKNOWN";
    }
}

led_state_t led_state_from_string(const char *str) {
    if (!str) return LED_STATE_OFF;
    if (strcasecmp(str, "AP_MODE") == 0) return LED_STATE_AP_MODE;
    if (strcasecmp(str, "CONNECTING") == 0) return LED_STATE_CONNECTING;
    if (strcasecmp(str, "ONLINE") == 0) return LED_STATE_ONLINE;
    if (strcasecmp(str, "RESETTING") == 0) return LED_STATE_RESETTING;
    return LED_STATE_OFF;
}
