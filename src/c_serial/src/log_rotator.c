#define _POSIX_C_SOURCE 200809L

#include "log_rotator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>

/**
 * 递归创建目录 (相当于 mkdir -p)
 */
static int mkdir_p(const char *dir) {
    char tmp[512];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", dir);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/**
 * 获取当前高精度纳秒时钟，并格式化为毫秒时间戳: [YYYY-MM-DD HH:MM:SS.mmm]
 */
static void get_millisecond_timestamp(char *buf, size_t size) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);

    long ms = ts.tv_nsec / 1000000;
    snprintf(buf, size, "[%04d-%02d-%02d %02d:%02d:%02d.%03ld]",
             tm_info.tm_year + 1900,
             tm_info.tm_mon + 1,
             tm_info.tm_mday,
             tm_info.tm_hour,
             tm_info.tm_min,
             tm_info.tm_sec,
             ms);
}

/**
 * 比较函数：按文件名升序排列 (时间戳前缀保证了字母升序 = 时间先后)
 */
static int compare_strings(const void *a, const void *b) {
    const char *str_a = *(const char **)a;
    const char *str_b = *(const char **)b;
    return strcmp(str_a, str_b);
}

/**
 * 扫描目录并清理超过数量限制的最老历史日志文件 (防爆卡)
 */
static void cleanup_old_files(LogRotator *rotator) {
    if (rotator->max_files <= 0) return;

    DIR *d = opendir(rotator->log_dir);
    if (!d) return;

    struct dirent *dir;
    char *files[256];
    int count = 0;

    while ((dir = readdir(d)) != NULL) {
        /* 只匹配形如 serial_*.log 的文件 */
        if (strncmp(dir->d_name, "serial_", 7) == 0 &&
            strstr(dir->d_name, ".log") != NULL) {
            if (count < 256) {
                files[count++] = strdup(dir->d_name);
            }
        }
    }
    closedir(d);

    if (count > rotator->max_files) {
        /* 排序后，数组前排即为最老的文件 */
        qsort(files, count, sizeof(char *), compare_strings);
        int delete_num = count - rotator->max_files;
        for (int i = 0; i < delete_num; i++) {
            char old_path[512];
            snprintf(old_path, sizeof(old_path), "%s/%s", rotator->log_dir, files[i]);
            unlink(old_path);
        }
    }

    for (int i = 0; i < count; i++) {
        free(files[i]);
    }
}

/**
 * 打开新的日志文件 (当旧文件满或首次初始化时调用)
 */
static int open_new_log_file(LogRotator *rotator) {
    if (rotator->current_fp) {
        fclose(rotator->current_fp);
        rotator->current_fp = NULL;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);

    snprintf(rotator->current_path, sizeof(rotator->current_path),
             "%s/serial_%04d%02d%02d_%02d%02d%02d.log",
             rotator->log_dir,
             tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);

    rotator->current_fp = fopen(rotator->current_path, "a");
    if (!rotator->current_fp) {
        perror("[LOG] 打开日志文件失败");
        return -1;
    }

    /* 获取已有文件大小 */
    struct stat st;
    if (stat(rotator->current_path, &st) == 0) {
        rotator->current_file_size = st.st_size;
    } else {
        rotator->current_file_size = 0;
    }

    /* 清理超出数量的最老文件 */
    cleanup_old_files(rotator);
    return 0;
}

int log_rotator_init(LogRotator *rotator, const char *log_dir, int max_size_mb, int max_files) {
    if (!rotator || !log_dir) return -1;

    memset(rotator, 0, sizeof(LogRotator));
    snprintf(rotator->log_dir, sizeof(rotator->log_dir), "%s", log_dir);
    rotator->max_size_bytes = (size_t)max_size_mb * 1024 * 1024;
    rotator->max_files = max_files;
    rotator->at_line_start = true;

    if (mkdir_p(rotator->log_dir) != 0) {
        perror("[LOG] 创建日志目录失败");
        return -1;
    }

    return open_new_log_file(rotator);
}

static void check_rotation(LogRotator *rotator, size_t incoming_len) {
    if (!rotator->current_fp ||
        (rotator->current_file_size + incoming_len >= rotator->max_size_bytes)) {
        open_new_log_file(rotator);
    }
}

void log_rotator_write_dut(LogRotator *rotator, const void *data, size_t len) {
    if (!rotator || !rotator->current_fp || !data || len == 0) return;

    check_rotation(rotator, len);

    const char *p = (const char *)data;
    char ts_buf[64];

    for (size_t i = 0; i < len; i++) {
        char ch = p[i];

        /* 若处于行首，先打印时间戳和 [DUT] 来源标签 */
        if (rotator->at_line_start) {
            get_millisecond_timestamp(ts_buf, sizeof(ts_buf));
            int written = fprintf(rotator->current_fp, "%s [DUT] ", ts_buf);
            if (written > 0) {
                rotator->current_file_size += (size_t)written;
            }
            rotator->at_line_start = false;
        }

        fputc(ch, rotator->current_fp);
        rotator->current_file_size += 1;

        if (ch == '\n') {
            rotator->at_line_start = true;
        }
    }
}

void log_rotator_write_input(LogRotator *rotator, const void *cmd, size_t len) {
    if (!rotator || !rotator->current_fp || !cmd || len == 0) return;

    check_rotation(rotator, len + 64);

    /* 若前面有未闭合的半行，先补换行 */
    if (!rotator->at_line_start) {
        fputc('\n', rotator->current_fp);
        rotator->current_file_size += 1;
        rotator->at_line_start = true;
    }

    char ts_buf[64];
    get_millisecond_timestamp(ts_buf, sizeof(ts_buf));

    int written = fprintf(rotator->current_fp, "%s [INPUT] %.*s",
                          ts_buf, (int)len, (const char *)cmd);
    if (written > 0) {
        rotator->current_file_size += (size_t)written;
    }

    const char *c = (const char *)cmd;
    if (c[len - 1] != '\n') {
        fputc('\n', rotator->current_fp);
        rotator->current_file_size += 1;
    }

    rotator->at_line_start = true;
    fflush(rotator->current_fp);
}

void log_rotator_flush(LogRotator *rotator) {
    if (rotator && rotator->current_fp) {
        fflush(rotator->current_fp);
    }
}

void log_rotator_close(LogRotator *rotator) {
    if (rotator && rotator->current_fp) {
        fclose(rotator->current_fp);
        rotator->current_fp = NULL;
    }
}
