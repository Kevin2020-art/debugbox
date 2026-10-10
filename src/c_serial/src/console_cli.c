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

int main(int argc, char *argv[]) {
    const char *socket_path = "/run/debugbox_serial.sock";
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("用法: %s [UNIX Socket 路径]\n", argv[0]);
        printf("默认路径: /run/debugbox_serial.sock\n");
        printf("提示: 在会话中按下快捷键 Ctrl + ] 即可安全退出\n");
        return 0;
    }
    if (argc > 1) {
        socket_path = argv[1];
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
        fprintf(stderr, "   请确认后台服务是否正常运行: sudo systemctl status debugbox-serial.service\n\n");
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
    printf("\r\n[已安全退出 DebugBox 控制台，后台持续存盘未受影响]\r\n");
    return 0;
}
