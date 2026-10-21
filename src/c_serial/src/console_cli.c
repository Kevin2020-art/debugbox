#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <errno.h>

static struct termios g_orig_tio;
static int g_terminal_is_raw = 0;

static void restore_terminal(void) {
    if (g_terminal_is_raw) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_tio);
        g_terminal_is_raw = 0;
    }
}

static void handle_exit_signal(int sig) {
    (void)sig;
    restore_terminal();
    printf("\r\n[Console received signal, exiting...]\r\n");
    exit(0);
}

static void print_usage(const char *prog) {
    printf("用法: %s [通道名|Socket路径|选项]\n", prog);
    printf("示例:\n");
    printf("  %s                  # 自动连接当前可用串口 (或默认 uart1)\n", prog);
    printf("  %s uart1            # 连接 1 号串口通道 (/run/debugbox_uart1.sock)\n", prog);
    printf("  %s uart2            # 连接 2 号串口通道 (/run/debugbox_uart2.sock)\n", prog);
    printf("  %s 1                # 数字简写，等同于 uart1\n", prog);
    printf("  %s /run/my.sock     # 连接指定 UNIX Domain Socket 路径\n", prog);
    printf("选项:\n");
    printf("  -l, --list, list    列出当前系统中正在运行的所有串口控制台通道\n");
    printf("  -h, --help          显示帮助信息\n");
    printf("提示: 接入会话后按下快捷键 Ctrl + ] 即可随时安全退出\n");
}

/**
 * 扫描 /run 目录列出所有正在运行的 debugbox 串口套接字
 * 返回找到的有效套接字数量
 */
static int scan_active_channels(char channels[][64], char socket_paths[][128], int max_count) {
    DIR *dir = opendir("/run");
    if (!dir) return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "debugbox_", 9) == 0) {
            /* 排除非串口类的控制套接字 (如 GPIO 守护进程) */
            if (strcmp(entry->d_name, "debugbox_gpio.sock") == 0) {
                continue;
            }
            size_t len = strlen(entry->d_name);
            if (len > 5 && strcmp(entry->d_name + len - 5, ".sock") == 0) {
                if (count < max_count) {
                    snprintf(socket_paths[count], 128, "/run/%s", entry->d_name);
                    /* 提取通道名: 去除前缀 debugbox_ 与后缀 .sock */
                    size_t ch_len = len - 9 - 5;
                    if (ch_len > 0 && ch_len < 63) {
                        strncpy(channels[count], entry->d_name + 9, ch_len);
                        channels[count][ch_len] = '\0';
                    } else {
                        snprintf(channels[count], sizeof(channels[count]), "%s", entry->d_name);
                    }
                    count++;
                }
            }
        }
    }
    closedir(dir);
    return count;
}

static void show_active_channels(void) {
    char channels[16][64];
    char paths[16][128];
    int count = scan_active_channels(channels, paths, 16);

    printf("==================================================================\n");
    printf(">>> 当前系统在线的 DebugBox 串口通道列表\n");
    printf("==================================================================\n");
    if (count == 0) {
        printf("⚠️  当前未检测到任何正在运行的串口服务。\n");
        printf("   请先启动服务: sudo systemctl start debugbox-serial@uart1.service\n");
    } else {
        for (int i = 0; i < count; i++) {
            printf("  [%d] 通道名: %-10s  (套接字: %s)\n", i + 1, channels[i], paths[i]);
            printf("      接入命令: debug-console %s\n", channels[i]);
        }
    }
    printf("==================================================================\n");
}

int main(int argc, char *argv[]) {
    char socket_path[128] = "";
    char display_name[64] = "default";

    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[1], "-l") == 0 || strcmp(argv[1], "--list") == 0 || strcmp(argv[1], "list") == 0) {
            show_active_channels();
            return 0;
        }

        const char *arg = argv[1];
        if (arg[0] == '/') {
            /* 绝对路径 */
            snprintf(socket_path, sizeof(socket_path), "%s", arg);
            snprintf(display_name, sizeof(display_name), "%s", arg);
        } else if (strncmp(arg, "uart", 4) == 0) {
            /* 如: uart1, uart2 */
            snprintf(socket_path, sizeof(socket_path), "/run/debugbox_%s.sock", arg);
            snprintf(display_name, sizeof(display_name), "%s", arg);
        } else if (arg[0] >= '0' && arg[0] <= '9') {
            /* 纯数字简写: 1 -> uart1 */
            snprintf(socket_path, sizeof(socket_path), "/run/debugbox_uart%s.sock", arg);
            snprintf(display_name, sizeof(display_name), "uart%s", arg);
        } else {
            /* 自定义名称 */
            snprintf(socket_path, sizeof(socket_path), "/run/debugbox_%s.sock", arg);
            snprintf(display_name, sizeof(display_name), "%s", arg);
        }
    } else {
        /* 未传参，按优先级自动探测 */
        if (access("/run/debugbox_serial.sock", F_OK) == 0) {
            snprintf(socket_path, sizeof(socket_path), "/run/debugbox_serial.sock");
            snprintf(display_name, sizeof(display_name), "serial");
        } else if (access("/run/debugbox_uart1.sock", F_OK) == 0) {
            snprintf(socket_path, sizeof(socket_path), "/run/debugbox_uart1.sock");
            snprintf(display_name, sizeof(display_name), "uart1");
        } else {
            /* 扫描是否有其他通道 */
            char channels[16][64];
            char paths[16][128];
            int count = scan_active_channels(channels, paths, 16);
            if (count == 1) {
                /* 只有一个通道，直接连接该通道 */
                snprintf(socket_path, sizeof(socket_path), "%s", paths[0]);
                snprintf(display_name, sizeof(display_name), "%s", channels[0]);
            } else if (count > 1) {
                printf("\n⚠️  检测到系统中有多个串口通道在运行，请指定需要接入的通道：\n");
                show_active_channels();
                return 1;
            } else {
                /* 默认路径尝试 */
                snprintf(socket_path, sizeof(socket_path), "/run/debugbox_serial.sock");
                snprintf(display_name, sizeof(display_name), "default");
            }
        }
    }

    /* 1. 连接本地 UNIX 套接字 */
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("创建 socket 失败");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "\n❌ 无法连接到串口守护服务 (%s)\n", socket_path);
        fprintf(stderr, "   请确认目标后台服务是否正常运行，例如: sudo systemctl status debugbox-serial@%s.service\n", display_name);
        fprintf(stderr, "   运行 'debug-console --list' 可查看当前在线的通道。\n\n");
        close(sock_fd);
        return 1;
    }

    /* 2. 保存当前终端属性，注册退出自动恢复 */
    if (tcgetattr(STDIN_FILENO, &g_orig_tio) == 0) {
        atexit(restore_terminal);
        signal(SIGINT, handle_exit_signal);
        signal(SIGTERM, handle_exit_signal);

        /* 3. 将工程师终端切换为 RAW 原始透传模式 (敲一键送一键) */
        struct termios raw = g_orig_tio;
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        g_terminal_is_raw = 1;
    }

    printf("\r\n=== Connected to DebugBox Serial Console [%s] (Press Ctrl+] to detach) ===\r\n\r\n", display_name);

    char buf[1024];
    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO;  /* 监听键盘输入 */
    fds[0].events = POLLIN;
    fds[1].fd = sock_fd;       /* 监听单板返回的数据 */
    fds[1].events = POLLIN;

    /* 4. 双向透传事件循环 */
    while (1) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* 工程师敲击键盘 */
        if (fds[0].revents & POLLIN) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;

            /* 检测是否按下了退出快捷键 Ctrl + ] (ASCII 0x1D) */
            int exit_triggered = 0;
            for (ssize_t i = 0; i < n; i++) {
                if ((unsigned char)buf[i] == 0x1D) {
                    exit_triggered = 1;
                    break;
                }
            }
            if (exit_triggered) {
                break;
            }

            /* 发送按键到守护进程 */
            if (write(sock_fd, buf, (size_t)n) < 0) {
                break;
            }
        }

        /* 收到单板返回的数据 */
        if (fds[1].revents & POLLIN) {
            ssize_t n = read(sock_fd, buf, sizeof(buf));
            if (n <= 0) {
                /* 服务端断开 */
                break;
            }
            /* 原样打在屏幕上 */
            (void)write(STDOUT_FILENO, buf, (size_t)n);
        }

        if (fds[1].revents & (POLLERR | POLLHUP)) {
            break;
        }
    }

    /* 5. 退出处理 */
    restore_terminal();
    close(sock_fd);
    printf("\r\n[已安全退出 DebugBox [%s] 控制台，后台持续存盘未受影响]\r\n", display_name);
    return 0;
}
