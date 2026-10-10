#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "serial_port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>

/**
 * 将整型波特率数值转换为 POSIX 标准 speed_t 常量
 */
static speed_t get_baud_speed(int baud_rate) {
    switch (baud_rate) {
        case 9600:    return B9600;
        case 19200:   return B19200;
        case 38400:   return B38400;
        case 57600:   return B57600;
        case 115200:  return B115200;
        case 230400:  return B230400;
#ifdef B460800
        case 460800:  return B460800;
#endif
#ifdef B921600
        case 921600:  return B921600;
#endif
#ifdef B1500000
        case 1500000: return B1500000;
#endif
        default:
            fprintf(stderr, "[SERIAL] 警告: 未知波特率 %d，默认回退至 115200\n", baud_rate);
            return B115200;
    }
}

int serial_open(const char *device_path, int baud_rate) {
    if (!device_path) {
        return -1;
    }

    /*
     * 打开串口标志位说明:
     * 1. O_RDWR:     可读可写
     * 2. O_NOCTTY:   关键! 串口不会变成当前守护进程的“控制终端”，
     *                防止单板意外发送 break/Ctrl+C 信号杀死我们自身进程
     * 3. O_NONBLOCK: 非阻塞 I/O，配合外部 poll() 异步事件循环使用
     */
    int fd = open(device_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    struct termios tio;
    memset(&tio, 0, sizeof(tio));

    if (tcgetattr(fd, &tio) != 0) {
        perror("[SERIAL] 获取串口 termios 属性失败");
        close(fd);
        return -1;
    }

    /*
     * 使用 cfmakeraw 将串口设置为纯 RAW (原始透传) 模式:
     * - 关闭 ICANON: 关闭规范模式 (无需等待回车换行，有字节即刻返回)
     * - 关闭 ECHO:   关闭串口本地自动回显
     * - 关闭 ISIG:   关闭将 Ctrl+C/Ctrl+Z 解释为系统中断信号
     * - 关闭 IXON:   关闭 XON/XOFF 软件流控 (防止 0x11/0x13 二进制字符导致假死)
     * - 关闭 OPOST:  关闭输出加工 (不自动把 \n 转换为 \r\n)
     */
    cfmakeraw(&tio);

    /* 8位数据位，1位停止位，无奇偶校验位 (8N1) */
    tio.c_cflag &= ~PARENB;         /* 无校验 */
    tio.c_cflag &= ~CSTOPB;         /* 1个停止位 */
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;             /* 8个数据位 */

    /* 硬件流控关闭 (TTL 模块通常只有 TX/RX/GND，无 RTS/CTS) */
#ifdef CRTSCTS
    tio.c_cflag &= ~CRTSCTS;
#endif

    /* 开启接收器，并忽略调制解调器状态线 (DCD 等) */
    tio.c_cflag |= (CREAD | CLOCAL);

    /* 非阻塞读取超时控制: 有 1 个字节立即返回，不等待 */
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;

    /* 设置输入和输出波特率 */
    speed_t speed = get_baud_speed(baud_rate);
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);

    /* 清空历史未读缓冲，使配置立刻生效 (TCSANOW) */
    tcflush(fd, TCIFLUSH);
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        perror("[SERIAL] 设置串口 termios 属性失败");
        close(fd);
        return -1;
    }

    return fd;
}

void serial_close(int *fd) {
    if (fd && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

long serial_write_all(int fd, const void *buf, size_t len) {
    if (fd < 0 || !buf || len == 0) {
        return 0;
    }

    const char *ptr = (const char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t written = write(fd, ptr, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue; /* 被信号打断，重试 */
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000); /* 缓冲区暂满，稍等1ms重试 */
                continue;
            }
            return -1; /* 硬件错误或断开 */
        }
        remaining -= (size_t)written;
        ptr += written;
    }

    return (long)len;
}
