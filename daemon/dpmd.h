#ifndef DPMD_H
#define DPMD_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <limits.h>

#define DPMD_TCP_PORT 7878
#define DPMD_LISTEN_BACKLOG 16
#define DPMD_MAX_CLIENTS 64
#define DPMD_MAX_JSON 8192
#define DPMD_MAX_DEVICES 1024
#define DPMD_DEBUGFS_STATUS_PATH "/sys/kernel/debug/dpmd/status"

typedef struct {
    uint16_t vid;
    uint16_t pid;
    uint8_t  cls;    // USB class
    char     type_name[64];
    char     name[128];
    char     devpath[PATH_MAX]; // "/devices/.../usbX/Y-Z"
    char     sysfs_path[PATH_MAX]; // "/sys" + devpath
    char     power_control[8]; // "on" or "auto"
    bool     present;
    time_t   last_seen;
} device_t;

typedef enum {
    PROFILE_BALANCED = 0,
    PROFILE_AGGRESSIVE = 1,
    PROFILE_PERFORMANCE = 2
} profile_t;

const char *usb_class_name(uint8_t cls);
bool parse_hex_u16(const char *s, uint16_t *out);
bool write_file_str(const char *path, const char *s);
bool read_file_str(const char *path, char *buf, size_t bufsz);
bool path_exists(const char *path);

#endif // DPMD_H
