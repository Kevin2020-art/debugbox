#ifndef LOG_ROTATOR_H
#define LOG_ROTATOR_H

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

typedef struct {
    char log_dir[256];          /* 日志存储目录，如 "/var/log/debugbox" */
    size_t max_size_bytes;      /* 单个文件最大字节数，如 20 * 1024 * 1024 (20MB) */
    int max_files;              /* 目录中保留的最大历史文件数，如 50 */
    
    FILE *current_fp;           /* 当前打开的日志文件句柄 */
    char current_path[512];     /* 当前文件绝对路径 */
    size_t current_file_size;   /* 当前文件已写入大小 */

    char line_buffer[4096];     /* 行拼接缓冲区 (处理串口流断字问题) */
    size_t line_buffer_len;     /* 当前行缓冲区中未完成的字符数 */
    bool at_line_start;         /* 标记当前是否处于新行行首 (决定是否打印时间戳) */
} LogRotator;

/**
 * 初始化日志轮转器
 *
 * @param rotator         结构体指针
 * @param log_dir         存储目录 (不存在会自动创建)
 * @param max_size_mb     单个日志文件最大大小 (单位: MB)
 * @param max_files       保留的最大文件个数
 * @return 0 成功，-1 失败
 */
int log_rotator_init(LogRotator *rotator, const char *log_dir, int max_size_mb, int max_files);

/**
 * 处理目标板 (DUT) 串口收到的原始字节流，自动拼接行并追加毫秒时间戳后写入日志
 *
 * @param rotator 结构体指针
 * @param data    串口接收到的数据
 * @param len     数据长度
 */
void log_rotator_write_dut(LogRotator *rotator, const void *data, size_t len);

/**
 * 将工程师通过终端下发的调试命令写入日志 (打上 [INPUT] 标签供日后审计)
 *
 * @param rotator 结构体指针
 * @param cmd     命令内容
 * @param len     命令长度
 */
void log_rotator_write_input(LogRotator *rotator, const void *cmd, size_t len);

/**
 * 强制将内存缓冲区数据刷入磁盘 (fflush)
 */
void log_rotator_flush(LogRotator *rotator);

/**
 * 关闭并释放日志轮转器资源
 */
void log_rotator_close(LogRotator *rotator);

#endif /* LOG_ROTATOR_H */
