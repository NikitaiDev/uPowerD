#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <ctype.h>
#include <time.h>

#include "dpmd.h"
#include "jsmn.h"

/* ---------- Utils ---------- */
static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
    return 0;
}

bool path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

bool read_file_str(const char *path, char *buf, size_t bufsz) {
    if (bufsz == 0) return false;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t want = (bufsz > 0U) ? (bufsz - 1U) : 0U;
    ssize_t n = read(fd, buf, want);    
    close(fd);
    if (n < 0) return false;
    buf[(size_t)n] = '\0';
    // trim trailing whitespace
    while (n > 0 && (buf[(size_t)n - 1U] == '\n' || buf[(size_t)n - 1U] == '\r' || buf[(size_t)n - 1U] == ' ' || buf[(size_t)n - 1U] == '\t')) {
        buf[(size_t)--n] = '\0';
    }
    return true;
}

bool write_file_str(const char *path, const char *s) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t len = strlen(s);
    ssize_t n = write(fd, s, len);
    close(fd);
    return (n == (ssize_t)len);
}

bool parse_hex_u16(const char *s, uint16_t *out) {
    if (!s || !out) return false;
    unsigned int val = 0U;
    if (sscanf(s, "%x", &val) != 1) return false;
    if (val > 0xFFFFU) return false;
    *out = (uint16_t)val;
    return true;
}

/* ---------- USB class names ---------- */
const char *usb_class_name(uint8_t cls) {
    switch (cls) {
        case 0x00: return "Device";
        case 0x01: return "Audio";
        case 0x02: return "Communications and CDC Control";
        case 0x03: return "HID (Human Interface Device)";
        case 0x05: return "Physical";
        case 0x06: return "Image";
        case 0x07: return "Printer";
        case 0x08: return "Mass Storage";
        case 0x09: return "Hub";
        case 0x0A: return "CDC-Data";
        case 0x0B: return "Smart Card";
        case 0x0D: return "Content Security";
        case 0x0E: return "Video";
        case 0x0F: return "Personal Healthcare";
        case 0x10: return "Audio/Video Devices";
        case 0x11: return "Billboard Device Class";
        case 0x12: return "USB Type-C Bridge Class";
        case 0x13: return "USB Bulk Display Protocol Device Class";
        case 0x14: return "MCTP over USB Protocol Endpoint Device Class";
        case 0x3C: return "I3C Device Class";
        case 0xDC: return "Diagnostic Device";
        case 0xE0: return "Wireless Controller";
        case 0xEF: return "Miscellaneous";
        case 0xFE: return "Application Specific";
        case 0xFF: return "Vendor Specific";
        default:   return "Unknown";
    }
}

/* ---------- Global state ---------- */
static device_t g_devices[DPMD_MAX_DEVICES];
static size_t   g_ndev = 0U;
static profile_t g_profile = PROFILE_BALANCED;

/* ---------- DebugFS updater ---------- */
static void debugfs_update_status(void) {
    char *buf = NULL;
    size_t cap = 4096U;
    buf = (char*)malloc(cap);
    if (!buf) return;
    size_t len = 0U;

    int n = snprintf(buf, cap,
                     "profile=%d\ncount=%zu\n",
                     (int)g_profile, g_ndev);
    if (n < 0) { free(buf); return; }
    len = (size_t)n;

    for (size_t i = 0; i < g_ndev; i++) {
        const device_t *d = &g_devices[i];
        char line[512];
        int m = snprintf(line, sizeof line,
                         "device[%zu]=%s|%s|%04x:%04x|%s|%s|present=%d\n",
                         i,
                         d->type_name,
                         d->name,
                         (unsigned int)d->vid, (unsigned int)d->pid,
                         d->devpath,
                         d->power_control,
                         d->present ? 1 : 0);
        if (m < 0) { free(buf); return; }
        size_t need = len + (size_t)m + 1U;
        if (need > cap) {
            cap = need * 2U;
            char *nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); return; }
            buf = nb;
        }
        memcpy(buf + len, line, (size_t)m);
        len += (size_t)m;
        buf[len] = '\0';
    }

    (void)write_file_str(DPMD_DEBUGFS_STATUS_PATH, buf);
    free(buf);
}

/* ---------- Device management ---------- */
static int device_find_by_vidpid(uint16_t vid, uint16_t pid) {
    for (size_t i = 0; i < g_ndev; i++) {
        if (g_devices[i].vid == vid && g_devices[i].pid == pid) return (int)i;
    }
    return -1;
}

static int device_find_by_devpath(const char *devpath) {
    for (size_t i = 0; i < g_ndev; i++) {
        if (strcmp(g_devices[i].devpath, devpath) == 0) return (int)i;
    }
    return -1;
}

static uint8_t read_usb_class_from_sysfs(const char *sysfs_path) {
    char p[PATH_MAX];
    char buf[64];

    // Try device class first
    int rc = snprintf(p, sizeof p, "%s/bDeviceClass", sysfs_path);
    if (rc < 0 || (size_t)rc >= sizeof p) { errno = ENAMETOOLONG; return 1; }

    if (read_file_str(p, buf, sizeof buf)) {
        uint8_t v = 0U;
        if (sscanf(buf, "%hhx", &v) == 1) return v;
    }
    // Fallback: first interface class
    rc = snprintf(p, sizeof p, "%s/bInterfaceClass", sysfs_path);
    if (rc < 0 || (size_t)rc >= sizeof p) { errno = ENAMETOOLONG; return 1; }
    (void)buf;
    return 0x00U;
}

static void read_usb_name_from_sysfs(const char *sysfs_path, char *out, size_t outsz) {
    char manu[128] = {0};
    char prod[128] = {0};
    char p[PATH_MAX];
    (void)snprintf(p, sizeof p, "%s/manufacturer", sysfs_path);
    (void)read_file_str(p, manu, sizeof manu);
    (void)snprintf(p, sizeof p, "%s/product", sysfs_path);
    (void)read_file_str(p, prod, sizeof prod);
    if (manu[0] != '\0' && prod[0] != '\0') {
        (void)snprintf(out, outsz, "%s %s", manu, prod);
    } else if (prod[0] != '\0') {
        (void)snprintf(out, outsz, "%s", prod);
    } else if (manu[0] != '\0') {
        (void)snprintf(out, outsz, "%s", manu);
    } else {
        (void)snprintf(out, outsz, "USB Device");
    }
}

static void set_power_control(device_t *d, const char *mode) {
    char p[PATH_MAX];
    int rc = snprintf(p, sizeof p, "%s/power/control", d->sysfs_path);
    if (rc < 0 || (size_t)rc >= sizeof p) { errno = ENAMETOOLONG; return; }
    if (path_exists(p)) {
        if (write_file_str(p, mode ? mode : "auto")) {
            (void)snprintf(d->power_control, sizeof d->power_control, "%s", mode ? mode : "auto");
        }
    }
}

static void apply_profile_to_device(device_t *d) {
    switch (g_profile) {
        case PROFILE_BALANCED:
            set_power_control(d, "auto");
            break;
        case PROFILE_AGGRESSIVE:
            if (d->cls == 0x08 || d->cls == 0x0E || d->cls == 0x01) { // Storage/Video/Audio -> allow "on"
                set_power_control(d, "on");
            } else {
                set_power_control(d, "auto");
            }
            break;
        case PROFILE_PERFORMANCE:
            set_power_control(d, "on");
            break;
        default:
            set_power_control(d, "auto");
            break;
    }
}

static void device_add_or_update(uint16_t vid, uint16_t pid, const char *devpath) {
    int idx = device_find_by_devpath(devpath);
    device_t *d = NULL;
    if (idx >= 0) {
        d = &g_devices[(size_t)idx];
    } else {
        if (g_ndev >= DPMD_MAX_DEVICES) return;
        d = &g_devices[g_ndev++];
        memset(d, 0, sizeof(*d));
        d->vid = vid;
        d->pid = pid;
        (void)snprintf(d->devpath, sizeof d->devpath, "%s", devpath);
        (void)snprintf(d->sysfs_path, sizeof d->sysfs_path, "/sys%s", devpath);
    }
    d->present = true;
    d->last_seen = time(NULL);

    d->cls = read_usb_class_from_sysfs(d->sysfs_path);
    (void)snprintf(d->type_name, sizeof d->type_name, "%s", usb_class_name(d->cls));
    read_usb_name_from_sysfs(d->sysfs_path, d->name, sizeof d->name);
    apply_profile_to_device(d);
    debugfs_update_status();
}

static void device_remove_by_devpath(const char *devpath) {
    int idx = device_find_by_devpath(devpath);
    if (idx < 0) return;
    g_devices[(size_t)idx].present = false;
    g_devices[(size_t)idx].last_seen = time(NULL);
    debugfs_update_status();
}

/* ---------- Netlink (uevent) ---------- */
static int netlink_open(void) {
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) return -1;
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof sa);
    sa.nl_family = AF_NETLINK;
    sa.nl_pid = (uint32_t)getpid();
    sa.nl_groups = 1U; // receive broadcast uevents
    if (bind(fd, (struct sockaddr*)&sa, sizeof sa) < 0) {
        close(fd);
        return -1;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (int[]){ 1024 * 1024 }, sizeof(int));
    (void)set_nonblock(fd);
    return fd;
}

static void handle_uevent_msg(const char *buf, ssize_t len) {
    // uevent payload: multiple NUL-terminated strings
    const char *end = buf + len;
    const char *p = buf;
    const char *action = NULL;
    const char *subsystem = NULL;
    const char *devpath = NULL;
    const char *product = NULL;
    const char *devtype = NULL;

    while (p < end) {
        size_t l = strlen(p);
        if (l == 0U) break;
        if (strncmp(p, "ACTION=", 7) == 0) action = p + 7;
        else if (strncmp(p, "SUBSYSTEM=", 10) == 0) subsystem = p + 10;
        else if (strncmp(p, "DEVPATH=", 8) == 0) devpath = p + 8;
        else if (strncmp(p, "PRODUCT=", 8) == 0) product = p + 8;
        else if (strncmp(p, "DEVTYPE=", 8) == 0) devtype = p + 8;
        p += l + 1U;
    }

    if (!subsystem || strcmp(subsystem, "usb") != 0) return;
    if (!devtype || strcmp(devtype, "usb_device") != 0) return;
    if (!devpath || !action) return;

    uint16_t vid = 0U, pid = 0U;
    if (product) {
        // PRODUCT=vvvv/pppp/ssss
        char v[5] = {0}, phex[5] = {0};
        size_t plen = strlen(product);
        const char *slash1 = memchr(product, '/', plen);
        if (slash1) {
            const char *slash2 = memchr(slash1 + 1, '/', plen - (size_t)(slash1 - product) - 1U);
            if (slash2 && (size_t)(slash1 - product) >= 4U && (size_t)(slash2 - (slash1 + 1)) >= 4U) {
                memcpy(v, product, 4U);
                memcpy(phex, slash1 + 1, 4U);
                (void)parse_hex_u16(v, &vid);
                (void)parse_hex_u16(phex, &pid);
            }
        }
    }

    if (strcmp(action, "add") == 0 || strcmp(action, "change") == 0) {
        device_add_or_update(vid, pid, devpath);
    } else if (strcmp(action, "remove") == 0) {
        device_remove_by_devpath(devpath);
    }
}

/* ---------- JSON-over-TCP ---------- */
typedef struct {
    int fd;
    char inbuf[DPMD_MAX_JSON];
    size_t inlen;
} client_t;

static client_t g_clients[DPMD_MAX_CLIENTS];

static int tcp_listen_open(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)DPMD_TCP_PORT);
    if (bind(fd, (struct sockaddr*)&sa, sizeof sa) < 0) { close(fd); return -1; }
    if (listen(fd, DPMD_LISTEN_BACKLOG) < 0) { close(fd); return -1; }
    (void)set_nonblock(fd);
    return fd;
}

static void json_writef(int fd, const char *fmt, ...) {
    char buf[DPMD_MAX_JSON];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    (void)write(fd, buf, (size_t)n);
}

static const char *profile_name(profile_t p) {
    switch (p) {
        case PROFILE_BALANCED: return "balanced";
        case PROFILE_AGGRESSIVE: return "aggressive";
        case PROFILE_PERFORMANCE: return "performance";
        default: return "balanced";
    }
}

static bool parse_string(const char *js, jsmntok_t *t, const char **out, char *tmp, size_t tmpsz) {
    if (t->type != JSMN_STRING) return false;
    int len = t->end - t->start;
    if (len < 0 || (size_t)len >= tmpsz) return false;
    memcpy(tmp, js + t->start, (size_t)len);
    tmp[(size_t)len] = '\0';
    *out = tmp;
    return true;
}

static void handle_json_cmd(int fd, const char *json, size_t json_len) {
    jsmn_parser p;
    jsmntok_t tok[128];
    jsmn_init(&p);
    int r = jsmn_parse(&p, json, json_len, tok, (unsigned int)(sizeof tok / sizeof tok[0]));
    if (r < 0 || r < 1 || tok[0].type != JSMN_OBJECT) {
        json_writef(fd, "{\"ok\":false,\"error\":\"parse\"}\n");
        return;
    }

    const char *cmd = NULL;
    for (int i = 1; i < r; i += 2) {
        if (i + 1 >= r) break;
        if (tok[i].type != JSMN_STRING) continue;
        int klen = tok[i].end - tok[i].start;
        if (klen <= 0) continue;
        if (klen == 3 && strncmp(json + tok[i].start, "cmd", (size_t)klen) == 0) {
            char tmp[64];
            const char *val = NULL;
            if (parse_string(json, &tok[i + 1], &val, tmp, sizeof tmp)) cmd = val;
        }
    }

    if (!cmd) { json_writef(fd, "{\"ok\":false,\"error\":\"no_cmd\"}\n"); return; }

    if (strcmp(cmd, "list_devices") == 0) {
        json_writef(fd, "{\"ok\":true,\"devices\":[");
        bool first = true;
        for (size_t i = 0; i < g_ndev; i++) {
            const device_t *d = &g_devices[i];
            if (!first) json_writef(fd, ",");
            first = false;
            // Order: type then name
            json_writef(fd,
                        "{\"type\":\"%s\",\"name\":\"%s\",\"vid\":\"%04x\",\"pid\":\"%04x\",\"present\":%s,\"power\":\"%s\"}",
                        d->type_name, d->name,
                        (unsigned int)d->vid, (unsigned int)d->pid,
                        d->present ? "true" : "false",
                        d->power_control);
        }
        json_writef(fd, "]}\n");
        return;
    }

    if (strcmp(cmd, "set_profile") == 0) {
        const char *profile = NULL;
        for (int i = 1; i < r; i += 2) {
            if (i + 1 >= r) break;
            if (tok[i].type != JSMN_STRING) continue;
            int klen = tok[i].end - tok[i].start;
            if (klen == 7 && strncmp(json + tok[i].start, "profile", 7U) == 0) {
                char tmp[32];
                const char *val = NULL;
                (void)parse_string(json, &tok[i + 1], &val, tmp, sizeof tmp);
                profile = val;
            }
        }
        if (!profile) { json_writef(fd, "{\"ok\":false,\"error\":\"no_profile\"}\n"); return; }
        if (strcmp(profile, "balanced") == 0) g_profile = PROFILE_BALANCED;
        else if (strcmp(profile, "aggressive") == 0) g_profile = PROFILE_AGGRESSIVE;
        else if (strcmp(profile, "performance") == 0) g_profile = PROFILE_PERFORMANCE;
        else { json_writef(fd, "{\"ok\":false,\"error\":\"bad_profile\"}\n"); return; }

        for (size_t i = 0; i < g_ndev; i++) apply_profile_to_device(&g_devices[i]);
        debugfs_update_status();
        json_writef(fd, "{\"ok\":true,\"profile\":\"%s\"}\n", profile_name(g_profile));
        return;
    }

    if (strcmp(cmd, "set_device") == 0) {
        uint16_t vid = 0U, pid = 0U;
        char state[8] = {0};
        bool have_vid = false, have_pid = false, have_state = false;
        for (int i = 1; i < r; i += 2) {
            if (i + 1 >= r) break;
            if (tok[i].type != JSMN_STRING) continue;
            int klen = tok[i].end - tok[i].start;
            const char *key = json + tok[i].start;
            if (klen == 3 && strncmp(key, "vid", 3U) == 0) {
                // accept string "0x1234" or number
                if (tok[i + 1].type == JSMN_STRING) {
                    char tmp[16]; const char *val = NULL;
                    (void)parse_string(json, &tok[i + 1], &val, tmp, sizeof tmp);
                    have_vid = parse_hex_u16(val, &vid);
                } else if (tok[i + 1].type == JSMN_PRIMITIVE) {
                    unsigned int v = 0U;
                    (void)sscanf(json + tok[i + 1].start, "%u", &v);
                    vid = (uint16_t)v; have_vid = true;
                }
            } else if (klen == 3 && strncmp(key, "pid", 3U) == 0) {
                if (tok[i + 1].type == JSMN_STRING) {
                    char tmp[16]; const char *val = NULL;
                    (void)parse_string(json, &tok[i + 1], &val, tmp, sizeof tmp);
                    have_pid = parse_hex_u16(val, &pid);
                } else if (tok[i + 1].type == JSMN_PRIMITIVE) {
                    unsigned int v = 0U;
                    (void)sscanf(json + tok[i + 1].start, "%u", &v);
                    pid = (uint16_t)v; have_pid = true;
                }
            } else if (klen == 5 && strncmp(key, "state", 5U) == 0) {
                if (tok[i + 1].type == JSMN_STRING) {
                    char tmp[8]; const char *val = NULL;
                    (void)parse_string(json, &tok[i + 1], &val, tmp, sizeof tmp);
                    (void)snprintf(state, sizeof state, "%s", tmp);
                    have_state = true;
                }
            }
        }
        if (!have_vid || !have_pid || !have_state) {
            json_writef(fd, "{\"ok\":false,\"error\":\"args\"}\n");
            return;
        }
        int idx = device_find_by_vidpid(vid, pid);
        if (idx < 0) { json_writef(fd, "{\"ok\":false,\"error\":\"not_found\"}\n"); return; }
        device_t *d = &g_devices[(size_t)idx];
        if (strcmp(state, "on") == 0 || strcmp(state, "auto") == 0) {
            set_power_control(d, state);
            debugfs_update_status();
            json_writef(fd, "{\"ok\":true,\"device\":{\"type\":\"%s\",\"name\":\"%s\",\"vid\":\"%04x\",\"pid\":\"%04x\",\"power\":\"%s\"}}\n",
                        d->type_name, d->name, (unsigned int)d->vid, (unsigned int)d->pid, d->power_control);
        } else {
            json_writef(fd, "{\"ok\":false,\"error\":\"bad_state\"}\n");
        }
        return;
    }

    if (strcmp(cmd, "stats") == 0) {
        json_writef(fd, "{\"ok\":true,\"profile\":\"%s\",\"count\":%zu}\n", profile_name(g_profile), g_ndev);
        return;
    }

    json_writef(fd, "{\"ok\":false,\"error\":\"unknown_cmd\"}\n");
}

/* ---------- Main loop ---------- */
int main(void) {
    int nl_fd = netlink_open();
    if (nl_fd < 0) return 1;

    int ls_fd = tcp_listen_open();
    if (ls_fd < 0) {
        close(nl_fd);
        return 1;
    }

    for (size_t i = 0; i < DPMD_MAX_CLIENTS; i++) {
        g_clients[i].fd = -1;
        g_clients[i].inlen = 0U;
    }

    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) {
        close(ls_fd);
        close(nl_fd);
        return 1;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;
    ev.data.fd = nl_fd;
    (void)epoll_ctl(ep, EPOLL_CTL_ADD, nl_fd, &ev);
    ev.data.fd = ls_fd;
    (void)epoll_ctl(ep, EPOLL_CTL_ADD, ls_fd, &ev);

    struct sockaddr_nl nlsa;
    socklen_t nlsalen = (socklen_t)sizeof nlsa;

    for (;;) {
        struct epoll_event events[64];
        int n = epoll_wait(ep, events, 64, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == nl_fd) {
                char buf[8192];
                ssize_t r = recvfrom(nl_fd, buf, (ssize_t)sizeof buf, 0, (struct sockaddr*)&nlsa, &nlsalen);
                if (r > 0) handle_uevent_msg(buf, r);
            } else if (fd == ls_fd) {
                for (;;) {
                    int cfd = accept4(ls_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) break;
                    // Register client
                    int slot = -1;
                    for (int k = 0; k < DPMD_MAX_CLIENTS; k++) { if (g_clients[k].fd < 0) { slot = k; break; } }
                    if (slot < 0) { close(cfd); break; }
                    g_clients[slot].fd = cfd;
                    g_clients[slot].inlen = 0U;
                    struct epoll_event cev; memset(&cev, 0, sizeof cev);
                    cev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
                    cev.data.fd = cfd;
                    (void)epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &cev);
                }
            } else {
                // client socket
                bool close_it = false;
                for (int k = 0; k < DPMD_MAX_CLIENTS; k++) {
                    if (g_clients[k].fd == fd) {
                        char *ib = g_clients[k].inbuf;
                        size_t *ilen = &g_clients[k].inlen;
                        for (;;) {
                            ssize_t r = read(fd, ib + *ilen, sizeof g_clients[k].inbuf - *ilen);
                            if (r <= 0) {
                                if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                                close_it = true;
                                break;
                            }
                            *ilen += (size_t)r;
                            if (*ilen >= 2U && (ib[*ilen - 1U] == '\n' || ib[*ilen - 1U] == '\0')) {
                                // one JSON message per line
                                handle_json_cmd(fd, ib, *ilen);
                                *ilen = 0U;
                                break;
                            }
                            if (*ilen == sizeof g_clients[k].inbuf) {
                                // overflow, reset
                                *ilen = 0U;
                                json_writef(fd, "{\"ok\":false,\"error\":\"overflow\"}\n");
                                break;
                            }
                        }
                        break;
                    }
                }
                if (close_it || (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0U) {
                    (void)epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                    for (int k = 0; k < DPMD_MAX_CLIENTS; k++) if (g_clients[k].fd == fd) g_clients[k].fd = -1;
                }
            }
        }
    }

    close(ls_fd);
    close(nl_fd);
    close(ep);
    return 0;
}
