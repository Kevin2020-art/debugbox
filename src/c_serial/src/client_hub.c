#define _POSIX_C_SOURCE 200809L

#include "client_hub.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int client_hub_init(ClientHub *hub, const char *socket_path) {
    if (!hub || !socket_path) return -1;

    memset(hub, 0, sizeof(ClientHub));
    snprintf(hub->socket_path, sizeof(hub->socket_path), "%s", socket_path);
    hub->listen_fd = -1;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        hub->client_fds[i] = -1;
    }

    /* 如果残留了旧的 socket 节点，先清理 */
    unlink(hub->socket_path);

    /* 1. 创建本地 UNIX 流式套接字 (AF_UNIX) */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[HUB] 创建 UNIX Socket 失败");
        return -1;
    }

    set_nonblocking(fd);

    /* 2. 绑定路径 */
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", hub->socket_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("[HUB] 绑定 UNIX Socket 路径失败");
        close(fd);
        return -1;
    }

    /* 3. 设置权限为 0666，确保普通用户 (非 root 工程师) 也可随时连入 */
    chmod(hub->socket_path, 0666);

    /* 4. 开始监听 */
    if (listen(fd, 5) != 0) {
        perror("[HUB] 监听 UNIX Socket 失败");
        close(fd);
        unlink(hub->socket_path);
        return -1;
    }

    hub->listen_fd = fd;
    printf("[HUB] UNIX 套接字就绪，监听路径: %s\n", hub->socket_path);
    return 0;
}

int client_hub_accept(ClientHub *hub) {
    if (!hub || hub->listen_fd < 0) return -1;

    int client_fd = accept(hub->listen_fd, NULL, NULL);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("[HUB] 接受客户端连接失败");
        }
        return -1;
    }

    set_nonblocking(client_fd);

    /* 寻找空闲槽位 */
    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (hub->client_fds[i] < 0) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        fprintf(stderr, "[HUB] 警告: 客户端连接数已达上限 (%d)，拒绝新连接\n", MAX_CLIENTS);
        const char *msg = "Error: DebugBox console connections full.\n";
        (void)write(client_fd, msg, strlen(msg));
        close(client_fd);
        return -1;
    }

    hub->client_fds[slot] = client_fd;
    hub->client_count++;
    printf("[HUB] 工程师控制台接入 (slot=%d, fd=%d, 当前在线: %d)\n",
           slot, client_fd, hub->client_count);

    /* 欢迎提示 */
    const char *welcome = "\r\n=== Connected to DebugBox Serial Console (Press Ctrl+] to detach) ===\r\n";
    (void)write(client_fd, welcome, strlen(welcome));

    return client_fd;
}

void client_hub_broadcast(ClientHub *hub, const void *data, size_t len) {
    if (!hub || !data || len == 0 || hub->client_count == 0) return;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        int fd = hub->client_fds[i];
        if (fd < 0) continue;

        /* 使用 MSG_NOSIGNAL 防止客户端突然断开时触发 SIGPIPE 杀死守护进程 */
        ssize_t ret = send(fd, data, len, MSG_NOSIGNAL);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue; /* 缓冲区暂满，跳过本次推送 */
            }
            /* 发生不可逆错误 (如对端已断开)，安全注销该客户端 */
            client_hub_disconnect_client(hub, fd);
        }
    }
}

void client_hub_disconnect_client(ClientHub *hub, int client_fd) {
    if (!hub || client_fd < 0) return;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (hub->client_fds[i] == client_fd) {
            close(client_fd);
            hub->client_fds[i] = -1;
            hub->client_count--;
            printf("[HUB] 工程师控制台断开 (fd=%d, 剩余在线: %d)\n",
                   client_fd, hub->client_count);
            break;
        }
    }
}

void client_hub_close(ClientHub *hub) {
    if (!hub) return;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (hub->client_fds[i] >= 0) {
            close(hub->client_fds[i]);
            hub->client_fds[i] = -1;
        }
    }
    hub->client_count = 0;

    if (hub->listen_fd >= 0) {
        close(hub->listen_fd);
        hub->listen_fd = -1;
    }

    if (hub->socket_path[0] != '\0') {
        unlink(hub->socket_path);
    }
}
