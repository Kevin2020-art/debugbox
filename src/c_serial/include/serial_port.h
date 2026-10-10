#ifndef SERIAL_PORT_H
#define SERIAL_PORT_H

#include <stddef.h>
#include <stdbool.h>

/**
 * 打开串口设备并设置为 POSIX RAW (原始透传) 模式
 *
 * @param device_path  串口设备路径 (如 "/dev/target_uart" 或 "/dev/ttyUSB0")
 * @param baud_rate    波特率 (支持 9600, 19200, 38400, 57600, 115200, 921600, 1500000 等)
 * @return 成功返回有效文件描述符 (fd >= 0)，失败返回 -1
 */
int serial_open(const char *device_path, int baud_rate);

/**
 * 关闭串口文件描述符
 *
 * @param fd 串口描述符
 */
void serial_close(int *fd);

/**
 * 向串口写入数据 (带完整写出保证)
 *
 * @param fd   串口描述符
 * @param buf  数据指针
 * @param len  数据字节数
 * @return 成功写出的字节数，出错返回 -1
 */
long serial_write_all(int fd, const void *buf, size_t len);

#endif /* SERIAL_PORT_H */
