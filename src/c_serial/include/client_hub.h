#ifndef CLIENT_HUB_H
#define CLIENT_HUB_H

#include <stddef.h>
#include <stdbool.h>

#define MAX_CLIENTS 8

typedef struct {
    char socket_path[108];      /* 套接字路径 (匹配 sockaddr_un.sun_path 大小: 108 字节) */
    int listen_fd;              /* UNIX Socket 监听文件描述符 */
    int client_fds[MAX_CLIENTS];/* 当前连入的工程师客户端描述符数组 */
    int client_count;           /* 当前客户端数量 */
} ClientHub;

/**
 * 初始化客户端中继 Hub (创建 UNIX Domain Socket 监听服务端)
 *
 * @param hub         结构体指针
 * @param socket_path 监听路径 (如 "/run/debugbox_serial.sock")
 * @return 0 成功，-1 失败
 */
int client_hub_init(ClientHub *hub, const char *socket_path);

/**
 * 接受新的工程师终端连接 (accept)
 *
 * @param hub 结构体指针
 * @return 新连接的客户端 fd (>= 0)，若失败或已达上限返回 -1
 */
int client_hub_accept(ClientHub *hub);

/**
 * 将串口接收到的单板数据实时广播给所有已连接的工程师终端屏幕
 *
 * @param hub  结构体指针
 * @param data 数据指针
 * @param len  数据长度
 */
void client_hub_broadcast(ClientHub *hub, const void *data, size_t len);

/**
 * 关闭并移除指定的客户端连接 (当客户端退出或断开时)
 *
 * @param hub       结构体指针
 * @param client_fd 待关闭的客户端描述符
 */
void client_hub_disconnect_client(ClientHub *hub, int client_fd);

/**
 * 关闭 Hub 服务端并清理 socket 文件
 */
void client_hub_close(ClientHub *hub);

#endif /* CLIENT_HUB_H */
