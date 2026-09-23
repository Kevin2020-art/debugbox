#ifndef GPIO_CONTROLLER_H
#define GPIO_CONTROLLER_H

#include <stdbool.h>
#include <pthread.h>
#include <gpiod.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_STATE_OFF = 0,
    LED_STATE_AP_MODE = 1,     /* 慢闪：0.8s 开，0.8s 关 (等待手机配网) */
    LED_STATE_CONNECTING = 2,  /* 中闪：0.25s 开，0.25s 关 (正在连接Wi-Fi) */
    LED_STATE_ONLINE = 3,      /* 心跳双闪：0.1s开->0.15s关->0.1s开->1.65s关 (已联网) */
    LED_STATE_RESETTING = 4    /* 急促狂闪：0.08s 开，0.08s 关 (重置确认) */
} led_state_t;

typedef void (*reset_callback_t)(void *user_data);

typedef struct {
    char chip_path[64];
    unsigned int button_pin;
    unsigned int led_pin;
    double long_press_sec;
    
    reset_callback_t on_reset;
    void *user_data;

    /* libgpiod v2 资源 */
    struct gpiod_chip *chip;
    struct gpiod_line_request *led_request;
    struct gpiod_line_request *btn_request;

    /* 线程与状态 */
    volatile led_state_t current_state;
    volatile bool running;
    pthread_t led_thread;
    pthread_t btn_thread;
    pthread_mutex_t state_mutex;
} gpio_controller_t;

/* 初始化与启动 */
int gpio_controller_init(gpio_controller_t *ctrl,
                         const char *chip_path,
                         unsigned int button_pin,
                         unsigned int led_pin,
                         double long_press_sec,
                         reset_callback_t on_reset,
                         void *user_data);

int gpio_controller_start(gpio_controller_t *ctrl);
void gpio_controller_stop(gpio_controller_t *ctrl);
void gpio_controller_destroy(gpio_controller_t *ctrl);

/* LED 状态控制与查询 */
void gpio_controller_set_led_state(gpio_controller_t *ctrl, led_state_t state);
led_state_t gpio_controller_get_led_state(gpio_controller_t *ctrl);

const char *led_state_to_string(led_state_t state);
led_state_t led_state_from_string(const char *str);

#ifdef __cplusplus
}
#endif

#endif /* GPIO_CONTROLLER_H */
