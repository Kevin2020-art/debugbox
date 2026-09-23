#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "gpio_controller.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#define SOCKET_PATH "/run/debugbox_gpio.sock"

static volatile sig_atomic_t g_keep_running = 1;
static char g_reset_cmd[256] = "/home/dong/rasp4b_for_remote_debuger/scripts/on_network_reset.sh";

static void sig_handler(int sig) {
    (void)sig;
    g_keep_running = 0;
}

static void on_reset_triggered(void *user_data) {
    (void)user_data;
    fprintf(stderr, "[C-Daemon] >>> 触发网络重置命令: %s <<<\n", g_reset_cmd);
    
    /* 异步调用重置脚本，不阻塞硬件监听线程 */
    char full_cmd[512];
    snprintf(full_cmd, sizeof(full_cmd), "bash %s &", g_reset_cmd);
    system(full_cmd);
}

/* 简单的客户端命令发送 (用于 CLI: gpio_ctl set ONLINE) */
static int run_client(int argc, char **argv) {
    if (argc < 2) {
        printf("用法:\n");
        printf("  %s set <AP_MODE|CONNECTING|ONLINE|RESETTING|OFF>\n", argv[0]);
        printf("  %s get\n", argv[0]);
        printf("  %s reset\n", argv[0]);
        return 1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "无法连接到 GPIO 守护进程 (%s)。守护进程是否已启动？\n", strerror(errno));
        close(fd);
        return 1;
    }

    char buf[128] = {0};
    if (strcasecmp(argv[1], "set") == 0 && argc >= 3) {
        snprintf(buf, sizeof(buf), "SET %s\n", argv[2]);
    } else if (strcasecmp(argv[1], "get") == 0) {
        snprintf(buf, sizeof(buf), "GET\n");
    } else if (strcasecmp(argv[1], "reset") == 0) {
        snprintf(buf, sizeof(buf), "RESET\n");
    } else {
        snprintf(buf, sizeof(buf), "%s\n", argv[1]);
    }

    write(fd, buf, strlen(buf));
    int n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }

    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    /* 如果可执行文件名包含 'ctl'，或者传了 'set'/'get'，作为客户端运行 */
    if (strstr(argv[0], "ctl") != NULL || (argc >= 2 && (strcmp(argv[1], "set") == 0 || strcmp(argv[1], "get") == 0))) {
        return run_client(argc, argv);
    }

    unsigned int btn_pin = 17;
    unsigned int led_pin = 27;
    double long_press_sec = 3.0;
    const char *chip_path = "/dev/gpiochip0";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--btn") == 0 && i + 1 < argc) btn_pin = atoi(argv[++i]);
        else if (strcmp(argv[i], "--led") == 0 && i + 1 < argc) led_pin = atoi(argv[++i]);
        else if (strcmp(argv[i], "--chip") == 0 && i + 1 < argc) chip_path = argv[++i];
        else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) long_press_sec = atof(argv[++i]);
        else if (strcmp(argv[i], "--reset-cmd") == 0 && i + 1 < argc) {
            strncpy(g_reset_cmd, argv[++i], sizeof(g_reset_cmd) - 1);
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("====================================================\n");
    printf("  DebugBox C-Language GPIO Daemon 启动\n");
    printf("  配置: Button=GPIO %u, LED=GPIO %u, 判定时长=%.1fs\n", btn_pin, led_pin, long_press_sec);
    printf("  IPC 监听: %s\n", SOCKET_PATH);
    printf("====================================================\n");

    gpio_controller_t ctrl;
    if (gpio_controller_init(&ctrl, chip_path, btn_pin, led_pin, long_press_sec, on_reset_triggered, NULL) != 0) {
        fprintf(stderr, "初始化 GPIO 控制器失败！\n");
        return 1;
    }

    if (gpio_controller_start(&ctrl) != 0) {
        fprintf(stderr, "启动 GPIO 控制器失败！\n");
        gpio_controller_destroy(&ctrl);
        return 1;
    }

    /* 启动时设为 CONNECTING 中闪，等待系统检测或连接网络 */
    gpio_controller_set_led_state(&ctrl, LED_STATE_CONNECTING);

    /* 创建 UNIX Domain Socket */
    unlink(SOCKET_PATH);
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); goto cleanup; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sfd);
        goto cleanup;
    }
    chmod(SOCKET_PATH, 0666);

    if (listen(sfd, 5) < 0) {
        perror("listen");
        close(sfd);
        goto cleanup;
    }

    /* 设置为非阻塞以支持信号打断 */
    fcntl(sfd, F_SETFL, O_NONBLOCK);

    while (g_keep_running) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(50000); /* 50ms */
                continue;
            }
            break;
        }

        char buf[128] = {0};
        int n = read(cfd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            /* 去除换行符 */
            char *p = strchr(buf, '\r'); if (p) *p = '\0';
            p = strchr(buf, '\n'); if (p) *p = '\0';

            if (strncmp(buf, "SET ", 4) == 0) {
                led_state_t new_st = led_state_from_string(buf + 4);
                gpio_controller_set_led_state(&ctrl, new_st);
                write(cfd, "OK\n", 3);
            } else if (strcmp(buf, "GET") == 0) {
                led_state_t cur_st = gpio_controller_get_led_state(&ctrl);
                char reply[64];
                snprintf(reply, sizeof(reply), "%s\n", led_state_to_string(cur_st));
                write(cfd, reply, strlen(reply));
            } else if (strcmp(buf, "RESET") == 0) {
                on_reset_triggered(NULL);
                write(cfd, "RESET_TRIGGERED\n", 16);
            } else {
                write(cfd, "ERR_UNKNOWN_CMD\n", 16);
            }
        }
        close(cfd);
    }

    close(sfd);
    unlink(SOCKET_PATH);

cleanup:
    printf("\n正在安全退出 C-GPIO 守护进程...\n");
    gpio_controller_destroy(&ctrl);
    printf("退出完毕。\n");
    return 0;
}
