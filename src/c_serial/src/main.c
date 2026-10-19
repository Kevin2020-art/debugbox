#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <poll.h>
#include <errno.h>

#include "serial_port.h"
#include "log_rotator.h"
#include "client_hub.h"

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void print_usage(const char *prog) {
    printf("用法: %s [选项]\n", prog);
    printf("选项:\n");
    printf("  -c, --channel <名称>    串口通道名称 (例如: uart1, 自动映射设备/日志/套接字路径)\n");
    printf("  -d, --device <路径>     串口设备路径 (默认: /dev/target_uart 或 /dev/target_<通道>)\n");
    printf("  -b, --baud <波特率>     串口波特率 (默认: 115200)\n");
    printf("  -l, --log-dir <目录>    日志存放目录 (默认: /var/log/debugbox 或 /var/log/debugbox/<通道>)\n");
    printf("  -s, --sock <路径>       UNIX Socket 路径 (默认: /run/debugbox_serial.sock 或 /run/debugbox_<通道>.sock)\n");
    printf("  -m, --max-size <MB>     单文件最大大小 (默认: 20)\n");
    printf("  -n, --max-files <数量>  保留最多历史文件数 (默认: 50)\n");
    printf("  -h, --help              显示帮助信息\n");
}

int main(int argc, char *argv[]) {
    char channel_name[64] = "";
    char device_path[256] = "";
    int baud_rate = 115200;
    char log_dir[256] = "";
    char socket_path[108] = "";
    int max_size_mb = 20;
    int max_files = 50;

    static struct option long_options[] = {
        {"channel",   required_argument, 0, 'c'},
        {"device",    required_argument, 0, 'd'},
        {"baud",      required_argument, 0, 'b'},
        {"log-dir",   required_argument, 0, 'l'},
        {"sock",      required_argument, 0, 's'},
        {"max-size",  required_argument, 0, 'm'},
        {"max-files", required_argument, 0, 'n'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:d:b:l:s:m:n:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c': strncpy(channel_name, optarg, sizeof(channel_name) - 1); break;
            case 'd': strncpy(device_path, optarg, sizeof(device_path) - 1); break;
            case 'b': baud_rate = atoi(optarg); break;
            case 'l': strncpy(log_dir, optarg, sizeof(log_dir) - 1); break;
            case 's': strncpy(socket_path, optarg, sizeof(socket_path) - 1); break;
            case 'm': max_size_mb = atoi(optarg); break;
            case 'n': max_files = atoi(optarg); break;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }

    /* 根据 channel_name 自动填充未显式指定的参数默认值 */
    if (strlen(channel_name) > 0) {
        if (strlen(device_path) == 0) {
            snprintf(device_path, sizeof(device_path), "/dev/target_%s", channel_name);
        }
        if (strlen(log_dir) == 0) {
            snprintf(log_dir, sizeof(log_dir), "/var/log/debugbox/%s", channel_name);
        }
        if (strlen(socket_path) == 0) {
            snprintf(socket_path, sizeof(socket_path), "/run/debugbox_%s.sock", channel_name);
        }
    } else {
        if (strlen(device_path) == 0) {
            strncpy(device_path, "/dev/target_uart", sizeof(device_path) - 1);
        }
        if (strlen(log_dir) == 0) {
            strncpy(log_dir, "/var/log/debugbox", sizeof(log_dir) - 1);
        }
        if (strlen(socket_path) == 0) {
            strncpy(socket_path, "/run/debugbox_serial.sock", sizeof(socket_path) - 1);
        }
    }

    printf("==================================================================\n");
    printf(">>> DebugBox C-Language Serial Supervisor 启动\n");
    printf("  - 目标串口:   %s (Baud: %d)\n", device_path, baud_rate);
    printf("  - 日志目录:   %s (单文件限制: %d MB, 最大保留: %d 个)\n", log_dir, max_size_mb, max_files);
    printf("  - 终端套接字: %s\n", socket_path);
    printf("==================================================================\n");

    /* 注册信号处理 (忽略 SIGPIPE，捕获 SIGINT/SIGTERM 实现优雅退出) */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /* 1. 初始化日志轮转器 */
    LogRotator rotator;
    if (log_rotator_init(&rotator, log_dir, max_size_mb, max_files) != 0) {
        fprintf(stderr, "[MAIN] 初始化日志轮转器失败\n");
        return 1;
    }

    /* 2. 初始化客户端终端 Hub (UNIX Socket) */
    ClientHub hub;
    if (client_hub_init(&hub, socket_path) != 0) {
        fprintf(stderr, "[MAIN] 初始化 UNIX Socket Hub 失败\n");
        log_rotator_close(&rotator);
        return 1;
    }

    /* 3. 打开物理串口 (支持热插拔，若刚开机未插入 USB，后续循环会每秒自动重连) */
    int serial_fd = serial_open(device_path, baud_rate);
    if (serial_fd >= 0) {
        printf("[MAIN] ✓ 物理串口已成功打开 (fd=%d)\n", serial_fd);
    } else {
        printf("[MAIN] ⚠️ 物理串口 %s 暂不可用，已进入掉线重连模式...\n", device_path);
    }

    char rx_buf[2048];

    /* ==============================================================
     * 核心单线程 poll() 全异步事件循环
     * ============================================================== */
    while (g_running) {
        /* 构建 poll 监听描述符列表 */
        struct pollfd fds[2 + MAX_CLIENTS];
        int nfds = 0;

        /* [0]: 物理串口 */
        int serial_idx = -1;
        if (serial_fd >= 0) {
            serial_idx = nfds;
            fds[nfds].fd = serial_fd;
            fds[nfds].events = POLLIN | POLLERR | POLLHUP;
            fds[nfds].revents = 0;
            nfds++;
        }

        /* [1]: UNIX Socket 监听端口 (等待工程师连接) */
        int listen_idx = nfds;
        fds[nfds].fd = hub.listen_fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;

        /* [2..k]: 工程师客户端连接列表 (等待工程师敲键盘发指令) */
        int client_map[MAX_CLIENTS];
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_map[i] = -1;
            if (hub.client_fds[i] >= 0) {
                client_map[i] = nfds;
                fds[nfds].fd = hub.client_fds[i];
                fds[nfds].events = POLLIN | POLLERR | POLLHUP;
                fds[nfds].revents = 0;
                nfds++;
            }
        }

        /* 超时策略: 若串口未打开，1秒超时以便定时尝试重连；若正常运行，500ms超时 */
        int timeout_ms = (serial_fd < 0) ? 1000 : 500;
        int ret = poll(fds, nfds, timeout_ms);

        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("[MAIN] poll 异常");
            break;
        }

        /* 定时重连检测 (串口掉线或未插入) */
        if (serial_fd < 0) {
            serial_fd = serial_open(device_path, baud_rate);
            if (serial_fd >= 0) {
                printf("[MAIN] 🎉 串口重新接入成功! (fd=%d)\n", serial_fd);
            }
        }

        if (ret == 0) {
            /* 超时无事件，定期刷新日志缓冲到磁盘 */
            log_rotator_flush(&rotator);
            continue;
        }

        /* ----------------------------------------------------------
         * 事件 1: 物理串口有数据到来 (单板吐 Log)
         * ---------------------------------------------------------- */
        if (serial_idx >= 0) {
            if (fds[serial_idx].revents & (POLLERR | POLLHUP)) {
                printf("[MAIN] ⚠️ 物理串口连接断开 (USB 可能被拔出)\n");
                serial_close(&serial_fd);
            } else if (fds[serial_idx].revents & POLLIN) {
                ssize_t n = read(serial_fd, rx_buf, sizeof(rx_buf));
                if (n > 0) {
                    /* 分支 A: 毫秒时间戳格式化存盘 */
                    log_rotator_write_dut(&rotator, rx_buf, (size_t)n);

                    /* 分支 B: 广播推送给所有工程师控制台屏幕 */
                    client_hub_broadcast(&hub, rx_buf, (size_t)n);
                } else if (n < 0 && (errno != EAGAIN && errno != EWOULDBLOCK)) {
                    printf("[MAIN] ⚠️ 串口读取失败 (errno=%d)，关闭重连\n", errno);
                    serial_close(&serial_fd);
                }
            }
        }

        /* ----------------------------------------------------------
         * 事件 2: 有新的工程师终端连接 (执行 debug-console)
         * ---------------------------------------------------------- */
        if (fds[listen_idx].revents & POLLIN) {
            client_hub_accept(&hub);
        }

        /* ----------------------------------------------------------
         * 事件 3: 工程师终端敲键盘下发指令 (Socket 有数据流入)
         * ---------------------------------------------------------- */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int pfd_idx = client_map[i];
            if (pfd_idx < 0) continue;

            int cfd = hub.client_fds[i];
            if (fds[pfd_idx].revents & (POLLERR | POLLHUP)) {
                client_hub_disconnect_client(&hub, cfd);
            } else if (fds[pfd_idx].revents & POLLIN) {
                char cmd_buf[1024];
                ssize_t n = read(cfd, cmd_buf, sizeof(cmd_buf));
                if (n > 0) {
                    /* 将工程师输入的命令直接原样打入物理串口送给目标板 */
                    if (serial_fd >= 0) {
                        serial_write_all(serial_fd, cmd_buf, (size_t)n);
                    } else {
                        const char *err_msg = "\r\n[Warning: Target UART is currently offline!]\r\n";
                        (void)write(cfd, err_msg, strlen(err_msg));
                    }

                    /* 同时在存盘日志中打上 [INPUT] 标签留痕审计 */
                    log_rotator_write_input(&rotator, cmd_buf, (size_t)n);
                } else if (n <= 0) {
                    /* 客户端主动断开连接 */
                    client_hub_disconnect_client(&hub, cfd);
                }
            }
        }
    }

    printf("\n[MAIN] 正在安全退出守护进程...\n");
    serial_close(&serial_fd);
    client_hub_close(&hub);
    log_rotator_close(&rotator);
    printf("[MAIN] 服务已完全停止。\n");

    return 0;
}
