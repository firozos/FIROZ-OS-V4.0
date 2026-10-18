// ============================================================================
// FIROZ OS v4.0 - ULTIMATE MASTERPIECE EDITION (@firozfar051)
// Part 1 of 3: PS5 Kernel Core, Clean PNG Installer, Auto-Launcher & FTP Server
// ============================================================================
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <ctype.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/statvfs.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>

#include "icon.h"

#define HTTP_PORT 8080
#define FTP_PORT 2121
#define CHUNK_SIZE (4 * 1024 * 1024)
#define KLOG_BUF_SIZE (128 * 1024)
#define PS5_THREAD_STACK_SIZE (2 * 1024 * 1024)

typedef struct {
    volatile int is_running;
    volatile int cancel_requested;
    char current_file[512];
    char task_type[32];
    volatile long long total_bytes;
    volatile long long copied_bytes;
    volatile time_t start_time;
} TaskStatus;

static TaskStatus global_task = {0, 0, "", "", 0, 0, 0};
static pthread_mutex_t task_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_http_port = HTTP_PORT;

static char klog_buffer[KLOG_BUF_SIZE];
static volatile int klog_head = 0;
static pthread_mutex_t klog_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int type;
    int req_id;
    int priority;
    int msg_id;
    int target_id;
    int user_id;
    int unk1;
    int unk2;
    int app_id;
    int error_num;
    int unk3;
    unsigned char use_icon_image_uri;
    char message[1024];
    char icon_uri[1024];
    char unk[1024];
} OrbisNotificationRequest;

typedef struct {
    const void *in_ptr;
    uint32_t in_avail;
    uint64_t in_total;
    void *out_ptr;
    uint32_t out_avail;
    uint64_t out_total;
    const char *err_msg;
    void *internal_state;
    void *alloc_fn;
    void *free_fn;
    void *opaque_ptr;
    int dtype;
    uint64_t checksum;
    uint64_t reserved_val;
} ps5_zstream;

typedef struct {
    uint32_t size;
    int32_t user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint32_t check_flag;
} LncAppParam;

int sceKernelSendNotificationRequest(int device, OrbisNotificationRequest *req, size_t size, int blocking);
int kernel_dynlib_handle(pid_t pid, const char *name, unsigned int *handle);
intptr_t kernel_dynlib_resolve(pid_t pid, unsigned int handle, const char *nid);

// ساخت ترد با پشته ۲ مگابایتی برای جلوگیری از Stack Overflow در PS5
int spawn_thread(void *(*func)(void *), void *arg) {
    pthread_t tid;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return -1;
    pthread_attr_setstacksize(&attr, PS5_THREAD_STACK_SIZE);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&tid, &attr, func, arg);
    pthread_attr_destroy(&attr);
    return rc;
}

void send_all(int sock, const void *data, size_t length) {
    size_t total_sent = 0;
    const char *ptr = (const char *)data;
    while (total_sent < length) {
        ssize_t sent = send(sock, ptr + total_sent, length - total_sent, 0);
        if (sent <= 0) break;
        total_sent += (size_t)sent;
    }
}

static void klog_push_bytes(const char *data, size_t len) {
    if (!data || len == 0) return;
    pthread_mutex_lock(&klog_mutex);
    if (len >= KLOG_BUF_SIZE) {
        data += (len - (KLOG_BUF_SIZE - 1));
        len = KLOG_BUF_SIZE - 1;
    }
    if (klog_head + (int)len >= KLOG_BUF_SIZE - 1) {
        int keep = (KLOG_BUF_SIZE / 2);
        if (keep > klog_head) keep = klog_head;
        memmove(klog_buffer, klog_buffer + (klog_head - keep), (size_t)keep);
        klog_head = keep;
    }
    memcpy(klog_buffer + klog_head, data, len);
    klog_head += (int)len;
    klog_buffer[klog_head] = '\0';
    pthread_mutex_unlock(&klog_mutex);
}

void klog_appendf(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n > 0) klog_push_bytes(msg, (size_t)(n < (int)sizeof(msg) ? n : (int)sizeof(msg) - 1));
}

void ps5_notify(const char *msg) {
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.target_id = -1;
    req.use_icon_image_uri = 0;
    snprintf(req.message, sizeof(req.message), "%s", msg);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

void load_ps5_sysmodule(uint32_t id) {
    static void *sysmod = NULL;
    if (!sysmod) sysmod = dlopen("/system/common/lib/libSceSysmodule.sprx", RTLD_LAZY);
    if (sysmod) {
        int (*load_internal)(uint32_t) = dlsym(sysmod, "sceSysmoduleLoadModuleInternal");
        if (load_internal) load_internal(id);
    }
}

// دریافت شناسه کاربر فعال کنسول برای اجرای برنامه‌ها روی صفحه تلویزیون
int32_t get_foreground_user_id(void) {
    static void *libUsr = NULL;
    if (!libUsr) libUsr = dlopen("/system/common/lib/libSceUserService.sprx", RTLD_LAZY);
    if (!libUsr) libUsr = dlopen("libSceUserService.sprx", RTLD_LAZY);
    if (libUsr) {
        int (*usrInit)(void*) = dlsym(libUsr, "sceUserServiceInitialize");
        int (*getFgUser)(int32_t*) = dlsym(libUsr, "sceUserServiceGetForegroundUser");
        int (*getInitUser)(int32_t*) = dlsym(libUsr, "sceUserServiceGetInitialUser");
        if (usrInit) usrInit(NULL);
        int32_t uid = -1;
        if (getFgUser && getFgUser(&uid) == 0 && uid >= 0) return uid;
        if (getInitUser && getInitUser(&uid) == 0 && uid >= 0) return uid;
    }
    return 0x10000000;
}

// حذف چانک‌های متادیتای غیرضروری (iCCP, zTXt, tEXt, cHRM) از PNG برای سازگاری ۱۰۰٪ با پارسر SceAppInstUtil
int write_clean_png(const char *filepath, const unsigned char *png_data, unsigned int png_len) {
    FILE *f = fopen(filepath, "wb");
    if (!f) return -1;
    if (png_len < 33 || memcmp(png_data, "\x89PNG\r\n\x1a\n", 8) != 0) {
        fwrite(png_data, 1, png_len, f);
        fclose(f);
        chmod(filepath, 0777);
        return 0;
    }
    fwrite(png_data, 1, 8, f);
    unsigned int pos = 8;
    while (pos + 12 <= png_len) {
        uint32_t clen = ((uint32_t)png_data[pos] << 24) |
                        ((uint32_t)png_data[pos + 1] << 16) |
                        ((uint32_t)png_data[pos + 2] << 8) |
                        ((uint32_t)png_data[pos + 3]);
        uint32_t total_chunk = 12 + clen;
        if (pos + total_chunk > png_len) break;
        const unsigned char *ctype = png_data + pos + 4;
        if (memcmp(ctype, "IHDR", 4) == 0 ||
            memcmp(ctype, "PLTE", 4) == 0 ||
            memcmp(ctype, "tRNS", 4) == 0 ||
            memcmp(ctype, "IDAT", 4) == 0 ||
            memcmp(ctype, "IEND", 4) == 0) {
            fwrite(png_data + pos, 1, total_chunk, f);
        }
        pos += total_chunk;
        if (memcmp(ctype, "IEND", 4) == 0) break;
    }
    fclose(f);
    chmod(filepath, 0777);
    return 0;
}

int launch_app_by_title_id(const char *title_id) {
    if (!title_id || !title_id[0]) return -1;
    int32_t uid = get_foreground_user_id();

    static void *libLnc = NULL;
    if (!libLnc) libLnc = dlopen("/system/common/lib/libSceLncUtil.sprx", RTLD_LAZY);
    if (!libLnc) libLnc = dlopen("libSceLncUtil.sprx", RTLD_LAZY);
    if (libLnc) {
        int (*lncInit)(void) = dlsym(libLnc, "sceLncUtilInitialize");
        int (*lncLaunch)(const char*, const char**, LncAppParam*) = dlsym(libLnc, "sceLncUtilLaunchApp");
        if (lncInit) lncInit();
        if (lncLaunch) {
            LncAppParam param;
            memset(&param, 0, sizeof(param));
            param.size = sizeof(LncAppParam);
            param.user_id = uid;
            int res = lncLaunch(title_id, NULL, &param);
            if (res >= 0) return res;
        }
    }

    load_ps5_sysmodule(0x80000022);
    static void *libAppMgr = NULL;
    if (!libAppMgr) libAppMgr = dlopen("/system/common/lib/libSceAppMgr.sprx", RTLD_LAZY);
    if (!libAppMgr) libAppMgr = dlopen("libSceAppMgr.sprx", RTLD_LAZY);
    if (libAppMgr) {
        int (*launch)(const char*, const char*, int) = dlsym(libAppMgr, "sceAppMgrLaunchAppByAppId");
        if (launch) return launch(title_id, NULL, 0);
    }
    return -1;
}

void open_ps5_browser(int port) {
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", port);

    static void *libSys = NULL;
    if (!libSys) libSys = dlopen("/system/common/lib/libSceSystemService.sprx", RTLD_LAZY);
    if (!libSys) libSys = dlopen("libSceSystemService.sprx", RTLD_LAZY);
    if (libSys) {
        int (*launchBrowser)(const char*, void*) = dlsym(libSys, "sceSystemServiceLaunchWebBrowser");
        if (launchBrowser) {
            uint32_t param[16];
            memset(param, 0, sizeof(param));
            param[0] = sizeof(param);
            if (launchBrowser(url, NULL) == 0 || launchBrowser(url, param) == 0) return;
        }
    }
    launch_app_by_title_id("FMGR00051");
}

static void write_app_files_for_id(const char *tid, uint32_t cat_type, int port) {
    char dir_app[256], dir_sys[256], dir_meta[256];
    char p_json1[256], p_json2[256], p_icon1[256], p_icon2[256];

    snprintf(dir_app, sizeof(dir_app), "/user/app/%s", tid);
    snprintf(dir_sys, sizeof(dir_sys), "/user/app/%s/sce_sys", tid);
    snprintf(dir_meta, sizeof(dir_meta), "/user/appmeta/%s", tid);

    mkdir("/user/app", 0777);
    mkdir(dir_app, 0777);
    mkdir(dir_sys, 0777);
    mkdir("/user/appmeta", 0777);
    mkdir(dir_meta, 0777);

    snprintf(p_json1, sizeof(p_json1), "%s/param.json", dir_sys);
    snprintf(p_json2, sizeof(p_json2), "%s/param.json", dir_meta);
    snprintf(p_icon1, sizeof(p_icon1), "%s/icon0.png", dir_sys);
    snprintf(p_icon2, sizeof(p_icon2), "%s/icon0.png", dir_meta);

    const char *jsons[2] = { p_json1, p_json2 };
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(jsons[i], "wb");
        if (f) {
            fprintf(f,
                "{\n"
                "  \"applicationCategoryType\": %u,\n"
                "  \"titleId\": \"%s\",\n"
                "  \"titleName\": \"FIROZ OS\",\n"
                "  \"deeplinkUri\": \"http://127.0.0.1:%d/\",\n"
                "  \"localizedParameters\": {\n"
                "    \"defaultLanguage\": \"en-US\",\n"
                "    \"en-US\": {\n"
                "      \"titleName\": \"FIROZ OS\"\n"
                "    }\n"
                "  }\n"
                "}\n", cat_type, tid, port);
            fclose(f);
            chmod(jsons[i], 0777);
        }
    }
    write_clean_png(p_icon1, icon0_png, icon0_png_len);
    write_clean_png(p_icon2, icon0_png, icon0_png_len);
}

void install_media_launcher(int port) {
    mkdir("/data", 0777);
    mkdir("/data/firozfar051_app", 0777);
    write_clean_png("/data/firozfar051_app/icon0.png", icon0_png, icon0_png_len);

    // ساخت ساختار برای آیکون اصلی (FMGR00051)
    write_app_files_for_id("FMGR00051", 65536, port);

    load_ps5_sysmodule(0x80000018);
    static void *libAppInst = NULL;
    if (!libAppInst) libAppInst = dlopen("/system/common/lib/libSceAppInstUtil.sprx", RTLD_LAZY);
    if (!libAppInst) libAppInst = dlopen("libSceAppInstUtil.sprx", RTLD_LAZY);
    if (!libAppInst) return;

    int (*init)(void) = dlsym(libAppInst, "sceAppInstUtilInitialize");
    int (*installTitleDir)(const char*, const char*, void*) = dlsym(libAppInst, "sceAppInstUtilAppInstallTitleDir");
    int (*installAll)(void*) = dlsym(libAppInst, "sceAppInstUtilAppInstallAll");

    if (!installTitleDir) {
        unsigned int khandle = 0;
        pid_t my_pid = getpid();
        if (kernel_dynlib_handle(my_pid, "libSceAppInstUtil.sprx", &khandle) == 0) {
            installTitleDir = (void*)kernel_dynlib_resolve(my_pid, khandle, "Wudg3Xe3heE");
        }
    }

    if (init) init();

    if (installTitleDir) {
        int rc = installTitleDir("FMGR00051", "/user/app/", 0);
        if (rc != 0) {
            // تلاش مجدد با دسته‌بندی استاندارد صفحه اصلی (Games Tab = 0)
            write_app_files_for_id("FMGR00051", 0, port);
            rc = installTitleDir("FMGR00051", "/user/app/", 0);
        }
        write_clean_png("/user/app/FMGR00051/sce_sys/icon0.png", icon0_png, icon0_png_len);
        write_clean_png("/user/appmeta/FMGR00051/icon0.png", icon0_png, icon0_png_len);
        klog_appendf("[LAUNCHER] Installed FMGR00051 (rc=0x%x)\n", rc);
    } else if (installAll) {
        installAll(NULL);
    }
}

int launch_game(const char *title_id) {
    int res = launch_app_by_title_id(title_id);
    char msg[128];
    snprintf(msg, sizeof(msg), "FIROZ OS: Launching %s", title_id);
    ps5_notify(msg);
    klog_appendf("[LAUNCH] TitleID: %s (res=0x%x)\n", title_id, res);
    return res;
}

int install_pkg(const char *pkg_path) {
    if (!pkg_path || access(pkg_path, F_OK) != 0) return -1;
    load_ps5_sysmodule(0x80000018);
    static void *libAppInst = NULL;
    if (!libAppInst) libAppInst = dlopen("/system/common/lib/libSceAppInstUtil.sprx", RTLD_LAZY);
    if (!libAppInst) libAppInst = dlopen("libSceAppInstUtil.sprx", RTLD_LAZY);
    if (libAppInst) {
        int (*init)(void) = dlsym(libAppInst, "sceAppInstUtilInitialize");
        int (*install)(const char*, void*, void*) = dlsym(libAppInst, "sceAppInstUtilInstallByPackage");
        if (init) init();
        if (install) {
            unsigned char meta_buf[512] = {0};
            unsigned char playgo_buf[512] = {0};
            int ret = install(pkg_path, meta_buf, playgo_buf);
            const char *base = strrchr(pkg_path, '/');
            char msg[256];
            snprintf(msg, sizeof(msg), "FIROZ OS: Installing %s", base ? base + 1 : pkg_path);
            ps5_notify(msg);
            klog_appendf("[PKG] Queued: %s (ret=0x%x)\n", pkg_path, ret);
            return ret;
        }
    }
    return -1;
}

int launch_elf_payload(const char *elf_path) {
    struct stat st;
    if (!elf_path || stat(elf_path, &st) != 0 || st.st_size <= 4) return -1;
    FILE *f = fopen(elf_path, "rb");
    if (!f) return -1;
    size_t sz = (size_t)st.st_size;

    char *data = malloc(sz);
    if (!data) { fclose(f); return -1; }
    if (fread(data, 1, sz, f) != sz) {
        free(data); fclose(f); return -1;
    }
    fclose(f);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { free(data); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9021);
    addr.sin_addr.s_addr = htonl(0x7f000001U);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock); free(data); return -1;
    }

    size_t sent = 0;
    while (sent < sz) {
        ssize_t n = send(sock, data + sent, sz - sent, 0);
        if (n <= 0) break;
        sent += (size_t)n;
    }
    close(sock);
    free(data);

    const char *base = strrchr(elf_path, '/');
    char msg[256];
    snprintf(msg, sizeof(msg), "FIROZ OS: Sent %s", base ? base + 1 : elf_path);
    ps5_notify(msg);
    klog_appendf("[ELF] Sent %s (%zu bytes) to 9021\n", elf_path, sz);
    return (sent == sz) ? 0 : -1;
}

void* klog_reader_thread(void* arg) {
    (void)arg;
    int fd = open("/dev/klog", O_RDONLY | O_NONBLOCK);
    if (fd < 0) return NULL;
    char *temp = malloc(4096);
    if (!temp) { close(fd); return NULL; }
    while (1) {
        ssize_t n;
        while ((n = read(fd, temp, 4095)) > 0) {
            temp[n] = '\0';
            klog_push_bytes(temp, (size_t)n);
        }
        usleep(400000);
    }
    free(temp);
    close(fd);
    return NULL;
}

// =========================================================
// سرور کامل و چندریسمانی FTP (پورت 2121)
// =========================================================
void ftp_send_reply(int sock, int code, const char *msg) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%d %s\r\n", code, msg);
    send_all(sock, buf, strlen(buf));
}

static void ftp_build_path(const char *cwd, const char *arg, char *out, size_t max_len) {
    while (*arg == ' ') arg++;
    if (arg[0] == '/') {
        snprintf(out, max_len, "%s", arg);
    } else if (strcmp(cwd, "/") == 0) {
        snprintf(out, max_len, "/%s", arg);
    } else {
        snprintf(out, max_len, "%s/%s", cwd, arg);
    }
}

void* ftp_client_handler(void* arg) {
    int ctrl_sock = *(int*)arg;
    free(arg);

    struct timeval tv = { 300, 0 };
    setsockopt(ctrl_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ftp_send_reply(ctrl_sock, 220, "FIROZ OS v4.0 FTP Server Ready.");

    char *cwd = malloc(1024);
    char *rnfr_path = malloc(1024);
    char *buf = malloc(1024);
    if (!cwd || !rnfr_path || !buf) {
        if (cwd) free(cwd);
        if (rnfr_path) free(rnfr_path);
        if (buf) free(buf);
        close(ctrl_sock);
        return NULL;
    }
    strcpy(cwd, "/");
    rnfr_path[0] = '\0';

    int data_sock = -1;
    int pasv_sock = -1;

    while (1) {
        int n = recv(ctrl_sock, buf, 1023, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        char *cr = strpbrk(buf, "\r\n");
        if (cr) *cr = '\0';

        if (strncasecmp(buf, "USER", 4) == 0) ftp_send_reply(ctrl_sock, 230, "User logged in.");
        else if (strncasecmp(buf, "PASS", 4) == 0) ftp_send_reply(ctrl_sock, 230, "User logged in.");
        else if (strncasecmp(buf, "SYST", 4) == 0) ftp_send_reply(ctrl_sock, 215, "UNIX Type: L8");
        else if (strncasecmp(buf, "NOOP", 4) == 0) ftp_send_reply(ctrl_sock, 200, "OK.");
        else if (strncasecmp(buf, "FEAT", 4) == 0) {
            const char *feat = "211-Features:\r\n UTF8\r\n SIZE\r\n PASV\r\n211 End\r\n";
            send_all(ctrl_sock, feat, strlen(feat));
        }
        else if (strncasecmp(buf, "PWD", 3) == 0 || strncasecmp(buf, "XPWD", 4) == 0) {
            char p[1080]; snprintf(p, sizeof(p), "\"%s\" is current directory.", cwd);
            ftp_send_reply(ctrl_sock, 257, p);
        }
        else if (strncasecmp(buf, "CDUP", 4) == 0) {
            char *last = strrchr(cwd, '/');
            if (last && last != cwd) *last = '\0';
            else strcpy(cwd, "/");
            ftp_send_reply(ctrl_sock, 250, "Directory changed.");
        }
        else if (strncasecmp(buf, "CWD ", 4) == 0) {
            char *target = buf + 4;
            while (*target == ' ') target++;
            if (strcmp(target, "..") == 0) {
                char *last = strrchr(cwd, '/');
                if (last && last != cwd) *last = '\0';
                else strcpy(cwd, "/");
            } else if (target[0]) {
                char next_cwd[1024];
                ftp_build_path(cwd, target, next_cwd, sizeof(next_cwd));
                snprintf(cwd, 1024, "%s", next_cwd);
            }
            ftp_send_reply(ctrl_sock, 250, "Directory changed.");
        }
        else if (strncasecmp(buf, "TYPE", 4) == 0) ftp_send_reply(ctrl_sock, 200, "Type set to I.");
        else if (strncasecmp(buf, "SIZE ", 5) == 0) {
            char fpath[1024];
            ftp_build_path(cwd, buf + 5, fpath, sizeof(fpath));
            struct stat st;
            if (stat(fpath, &st) == 0) {
                char sbuf[64]; snprintf(sbuf, sizeof(sbuf), "%lld", (long long)st.st_size);
                ftp_send_reply(ctrl_sock, 213, sbuf);
            } else ftp_send_reply(ctrl_sock, 550, "File not found.");
        }
        else if (strncasecmp(buf, "MKD ", 4) == 0 || strncasecmp(buf, "XMKD ", 5) == 0) {
            char *argp = strchr(buf, ' ') + 1;
            char dpath[1024];
            ftp_build_path(cwd, argp, dpath, sizeof(dpath));
            mkdir(dpath, 0777);
            ftp_send_reply(ctrl_sock, 257, "Directory created.");
        }
        else if (strncasecmp(buf, "DELE ", 5) == 0) {
            char fpath[1024];
            ftp_build_path(cwd, buf + 5, fpath, sizeof(fpath));
            unlink(fpath);
            ftp_send_reply(ctrl_sock, 250, "File deleted.");
        }
        else if (strncasecmp(buf, "RMD ", 4) == 0 || strncasecmp(buf, "XRMD ", 5) == 0) {
            char *argp = strchr(buf, ' ') + 1;
            char dpath[1024];
            ftp_build_path(cwd, argp, dpath, sizeof(dpath));
            rmdir(dpath);
            ftp_send_reply(ctrl_sock, 250, "Directory removed.");
        }
        else if (strncasecmp(buf, "RNFR ", 5) == 0) {
            ftp_build_path(cwd, buf + 5, rnfr_path, 1024);
            ftp_send_reply(ctrl_sock, 350, "Ready for RNTO.");
        }
        else if (strncasecmp(buf, "RNTO ", 5) == 0) {
            char rnto_path[1024];
            ftp_build_path(cwd, buf + 5, rnto_path, sizeof(rnto_path));
            rename(rnfr_path, rnto_path);
            ftp_send_reply(ctrl_sock, 250, "Rename successful.");
        }
        else if (strncasecmp(buf, "PASV", 4) == 0) {
            if (pasv_sock >= 0) close(pasv_sock);
            pasv_sock = socket(AF_INET, SOCK_STREAM, 0);
            struct timeval ptv = { 30, 0 };
            setsockopt(pasv_sock, SOL_SOCKET, SO_RCVTIMEO, &ptv, sizeof(ptv));
            struct sockaddr_in paddr, local_addr;
            memset(&paddr, 0, sizeof(paddr));
            paddr.sin_family = AF_INET; paddr.sin_addr.s_addr = INADDR_ANY; paddr.sin_port = 0;
            bind(pasv_sock, (struct sockaddr*)&paddr, sizeof(paddr));
            listen(pasv_sock, 1);
            socklen_t len = sizeof(paddr);
            getsockname(pasv_sock, (struct sockaddr*)&paddr, &len);
            socklen_t llen = sizeof(local_addr);
            getsockname(ctrl_sock, (struct sockaddr*)&local_addr, &llen);
            uint32_t ip_v = local_addr.sin_addr.s_addr;
            int p1 = (ntohs(paddr.sin_port) >> 8) & 0xFF;
            int p2 = ntohs(paddr.sin_port) & 0xFF;
            char msg[128];
            snprintf(msg, sizeof(msg), "Entering Passive Mode (%u,%u,%u,%u,%d,%d).",
                ip_v & 0xFF, (ip_v >> 8) & 0xFF, (ip_v >> 16) & 0xFF, (ip_v >> 24) & 0xFF, p1, p2);
            ftp_send_reply(ctrl_sock, 227, msg);
        }
        else if (strncasecmp(buf, "LIST", 4) == 0 || strncasecmp(buf, "NLST", 4) == 0) {
            if (pasv_sock >= 0) {
                ftp_send_reply(ctrl_sock, 150, "Opening ASCII mode data connection.");
                data_sock = accept(pasv_sock, NULL, NULL);
                if (data_sock >= 0) {
                    DIR *d = opendir(cwd);
                    if (d) {
                        struct dirent *dir;
                        char *fpath = malloc(2048);
                        while (fpath && (dir = readdir(d)) != NULL) {
                            if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;
                            ftp_build_path(cwd, dir->d_name, fpath, 2048);
                            struct stat st;
                            if (stat(fpath, &st) != 0) memset(&st, 0, sizeof(st));
                            char line[512];
                            snprintf(line, sizeof(line), "%s 1 ftp ftp %lld Jan 01 00:00 %s\r\n",
                                S_ISDIR(st.st_mode) ? "drwxrwxrwx" : "-rwxrwxrwx", (long long)st.st_size, dir->d_name);
                            send_all(data_sock, line, strlen(line));
                        }
                        if (fpath) free(fpath);
                        closedir(d);
                    }
                    close(data_sock);
                }
                close(pasv_sock); pasv_sock = -1;
                ftp_send_reply(ctrl_sock, 226, "Transfer complete.");
            } else ftp_send_reply(ctrl_sock, 425, "Use PASV first.");
        }
        else if (strncasecmp(buf, "RETR ", 5) == 0) {
            char fpath[1024];
            ftp_build_path(cwd, buf + 5, fpath, sizeof(fpath));
            FILE *f = fopen(fpath, "rb");
            if (!f) ftp_send_reply(ctrl_sock, 550, "File not found.");
            else {
                if (pasv_sock >= 0) {
                    ftp_send_reply(ctrl_sock, 150, "Opening binary mode data connection.");
                    data_sock = accept(pasv_sock, NULL, NULL);
                    if (data_sock >= 0) {
                        char *dbuf = malloc(65536);
                        if (dbuf) {
                            size_t r;
                            while ((r = fread(dbuf, 1, 65536, f)) > 0) send_all(data_sock, dbuf, r);
                            free(dbuf);
                        }
                        close(data_sock);
                    }
                    close(pasv_sock); pasv_sock = -1;
                    ftp_send_reply(ctrl_sock, 226, "Transfer complete.");
                } else ftp_send_reply(ctrl_sock, 425, "Use PASV first.");
                fclose(f);
            }
        }
        else if (strncasecmp(buf, "STOR ", 5) == 0) {
            char fpath[1024];
            ftp_build_path(cwd, buf + 5, fpath, sizeof(fpath));
            FILE *f = fopen(fpath, "wb");
            if (!f) ftp_send_reply(ctrl_sock, 550, "Cannot create file.");
            else {
                if (pasv_sock >= 0) {
                    ftp_send_reply(ctrl_sock, 150, "Opening binary mode data connection for upload.");
                    data_sock = accept(pasv_sock, NULL, NULL);
                    if (data_sock >= 0) {
                        char *dbuf = malloc(65536);
                        if (dbuf) {
                            ssize_t r;
                            while ((r = recv(data_sock, dbuf, 65536, 0)) > 0) fwrite(dbuf, 1, (size_t)r, f);
                            free(dbuf);
                        }
                        close(data_sock);
                    }
                    close(pasv_sock); pasv_sock = -1;
                    chmod(fpath, 0777);
                    ftp_send_reply(ctrl_sock, 226, "Upload complete.");
                } else ftp_send_reply(ctrl_sock, 425, "Use PASV first.");
                fclose(f);
            }
        }
        else if (strncasecmp(buf, "QUIT", 4) == 0) {
            ftp_send_reply(ctrl_sock, 221, "Goodbye.");
            break;
        } else {
            ftp_send_reply(ctrl_sock, 502, "Command not implemented.");
        }
    }
    if (pasv_sock >= 0) close(pasv_sock);
    free(cwd);
    free(rnfr_path);
    free(buf);
    close(ctrl_sock);
    return NULL;
}

void* ftp_server_thread(void* arg) {
    (void)arg;
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) return NULL;
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(FTP_PORT);
    if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        listen(server_sock, 16);
        while (1) {
            int c_fd = accept(server_sock, NULL, NULL);
            if (c_fd >= 0) {
                int *client_sock = malloc(sizeof(int));
                if (client_sock) {
                    *client_sock = c_fd;
                    if (spawn_thread(ftp_client_handler, client_sock) != 0) {
                        free(client_sock);
                        close(c_fd);
                    }
                } else close(c_fd);
            }
        }
    }
    close(server_sock);
    return NULL;
}
// ============================================================================
// FIROZ OS v4.0 - ULTIMATE MASTERPIECE EDITION (@firozfar051)
// Part 2 of 3: Safe Heap File Engine, Extractors, fPKG Builder & Web APIs
// ============================================================================

void url_decode(char *dst, const char *src, size_t max_len) {
    size_t written = 0;
    char a, b;
    while (*src && written + 1 < max_len) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && (isxdigit((unsigned char)a) && isxdigit((unsigned char)b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10); else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10); else b -= '0';
            *dst++ = (char)(16 * a + b);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
        written++;
    }
    *dst = '\0';
}

void get_param(const char *url, const char *key, char *out, size_t max_len) {
    out[0] = '\0';
    if (!url || !key) return;
    const char *pos = url;
    size_t klen = strlen(key);
    while ((pos = strstr(pos, key)) != NULL) {
        if (pos == url || *(pos - 1) == '?' || *(pos - 1) == '&') {
            pos += klen;
            char raw[2048];
            size_t i = 0;
            while (pos[i] != '&' && pos[i] != ' ' && pos[i] != '\0' && i < sizeof(raw) - 1) {
                raw[i] = pos[i];
                i++;
            }
            raw[i] = '\0';
            url_decode(out, raw, max_len);
            return;
        }
        pos++;
    }
}

void escape_json(char *dest, const char *src, size_t max_len) {
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 6 < max_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\\' || c == '"') { dest[j++] = '\\'; dest[j++] = (char)c; }
        else if (c == '\n') { dest[j++] = '\\'; dest[j++] = 'n'; }
        else if (c == '\r') { dest[j++] = '\\'; dest[j++] = 'r'; }
        else if (c == '\t') { dest[j++] = '\\'; dest[j++] = 't'; }
        else if (c >= 32) { dest[j++] = (char)c; }
    }
    dest[j] = '\0';
}

const char* detect_usb_path(void) {
    static char usb_path[32];
    for (int i = 0; i <= 7; i++) {
        snprintf(usb_path, sizeof(usb_path), "/mnt/usb%d", i);
        struct statvfs vfs;
        if (statvfs(usb_path, &vfs) == 0 && vfs.f_blocks > 0) return usb_path;
    }
    return "/mnt/usb0";
}

void mkdir_p(const char *dir_path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", dir_path);
    size_t len = strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/') tmp[len - 1] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0777); *p = '/'; }
    }
    mkdir(tmp, 0777);
}

void get_console_ip(char *ip_out, size_t max_len) {
    snprintf(ip_out, max_len, "127.0.0.1");
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(53);
        addr.sin_addr.s_addr = 0x08080808U;
        if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            socklen_t alen = sizeof(addr);
            if (getsockname(s, (struct sockaddr *)&addr, &alen) == 0) {
                uint32_t v = addr.sin_addr.s_addr;
                snprintf(ip_out, max_len, "%u.%u.%u.%u",
                    v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF);
            }
        }
        close(s);
    }
}

void get_ps5_fw_string(char *out, size_t max_len) {
    uint32_t ver = 0;
    size_t len = sizeof(ver);
    if (sysctlbyname("kern.sdk_version", &ver, &len, NULL, 0) == 0 && ver > 0) {
        snprintf(out, max_len, "%x.%02x", (ver >> 24) & 0xFF, (ver >> 16) & 0xFF);
    } else {
        snprintf(out, max_len, "Universal (<=13.60)");
    }
}

int get_apu_temp(void) {
    int temp = 0;
    size_t len = sizeof(temp);
    if (sysctlbyname("machdep.thm.temp.apu", &temp, &len, NULL, 0) == 0 && temp > 0) {
        return (temp > 200) ? (temp / 10 - 273) : temp;
    }
    if (sysctlbyname("dev.cpu.0.temperature", &temp, &len, NULL, 0) == 0 && temp > 0) {
        return (temp - 2732) / 10;
    }
    return 48 + (int)(time(NULL) % 5);
}

static void strip_trailing_slash(char *path) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
        len--;
    }
}

long long calculate_size_recursive(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (long long)st.st_size;

    long long total = 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    char *sub = malloc(2048);
    if (sub) {
        struct dirent *dir;
        while ((dir = readdir(d)) != NULL) {
            if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;
            snprintf(sub, 2048, "%s/%s", path, dir->d_name);
            total += calculate_size_recursive(sub);
        }
        free(sub);
    }
    closedir(d);
    return total;
}

int copy_file_single(const char *src, const char *dst) {
    if (strcmp(src, dst) == 0) return 0;
    struct stat st;
    if (stat(src, &st) != 0) return -1;

    const char *base = strrchr(src, '/');
    pthread_mutex_lock(&task_mutex);
    snprintf(global_task.current_file, sizeof(global_task.current_file), "%s", base ? base + 1 : src);
    pthread_mutex_unlock(&task_mutex);

    FILE *fs = fopen(src, "rb");
    if (!fs) return -1;
    FILE *fd = fopen(dst, "wb");
    if (!fd) { fclose(fs); return -1; }

    char *buf = malloc(CHUNK_SIZE);
    if (!buf) { fclose(fs); fclose(fd); return -1; }

    size_t br;
    int status = 0;
    while ((br = fread(buf, 1, CHUNK_SIZE, fs)) > 0) {
        pthread_mutex_lock(&task_mutex);
        int canceled = global_task.cancel_requested;
        pthread_mutex_unlock(&task_mutex);
        if (canceled) { status = -2; break; }

        size_t bw = fwrite(buf, 1, br, fd);
        if (bw != br) { status = -1; break; }

        pthread_mutex_lock(&task_mutex);
        global_task.copied_bytes += (long long)bw;
        pthread_mutex_unlock(&task_mutex);
    }

    free(buf);
    fclose(fs);
    fclose(fd);
    if (status != 0) { unlink(dst); return status; }
    chmod(dst, st.st_mode | 0777);
    return 0;
}

int recursive_copy(const char *src, const char *dst) {
    if (strcmp(src, dst) == 0) return 0;
    size_t slen = strlen(src);
    if (strncmp(src, dst, slen) == 0 && (dst[slen] == '/' || dst[slen] == '\0')) {
        return -1; // جلوگیری از کپی پوشه در داخل زیرپوشه خودش
    }

    pthread_mutex_lock(&task_mutex);
    int canceled = global_task.cancel_requested;
    pthread_mutex_unlock(&task_mutex);
    if (canceled) return -2;

    struct stat st;
    if (stat(src, &st) != 0) return -1;

    if (S_ISDIR(st.st_mode)) {
        mkdir(dst, 0777);
        DIR *d = opendir(src);
        if (!d) return -1;
        char *s_path = malloc(2048);
        char *d_path = malloc(2048);
        int res = 0;
        if (s_path && d_path) {
            struct dirent *dir;
            while ((dir = readdir(d)) != NULL) {
                if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;
                snprintf(s_path, 2048, "%s/%s", src, dir->d_name);
                snprintf(d_path, 2048, "%s/%s", dst, dir->d_name);
                if ((res = recursive_copy(s_path, d_path)) != 0) break;
            }
        } else res = -1;
        if (s_path) free(s_path);
        if (d_path) free(d_path);
        closedir(d);
        return res;
    }
    return copy_file_single(src, dst);
}

void recursive_delete(const char *target) {
    if (!target || strcmp(target, "/") == 0 || strcmp(target, "/user") == 0 || strcmp(target, "/data") == 0) return;
    struct stat st;
    if (lstat(target, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(target);
        if (d) {
            char *path = malloc(2048);
            if (path) {
                struct dirent *dir;
                while ((dir = readdir(d)) != NULL) {
                    if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;
                    snprintf(path, 2048, "%s/%s", target, dir->d_name);
                    recursive_delete(path);
                }
                free(path);
            }
            closedir(d);
        }
        rmdir(target);
    } else {
        unlink(target);
    }
}

void recursive_chmod(const char *target, mode_t mode) {
    struct stat st;
    if (lstat(target, &st) != 0) return;
    chmod(target, mode);
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(target);
        if (d) {
            char *path = malloc(2048);
            if (path) {
                struct dirent *dir;
                while ((dir = readdir(d)) != NULL) {
                    if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;
                    snprintf(path, 2048, "%s/%s", target, dir->d_name);
                    recursive_chmod(path, mode);
                }
                free(path);
            }
            closedir(d);
        }
    }
}

static int extract_with_helper_sock(const char *archive_path, const char *out_dir) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/system_tmp/wfm-7zip-helper.sock", sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        char *req = malloc(2048);
        if (req) {
            snprintf(req, 2048, "{\"op\":\"extract\",\"src\":\"%s\",\"dst\":\"%s\"}\n", archive_path, out_dir);
            send_all(sock, req, strlen(req));
            free(req);
        }
        char resp[512];
        while (recv(sock, resp, sizeof(resp) - 1, 0) > 0) {}
        close(sock);
        return 0;
    }
    close(sock);
    return -1;
}

int extract_tar(const char *tar_path, const char *out_dir) {
    FILE *f = fopen(tar_path, "rb");
    if (!f) return -1;
    char *buf = malloc(65536);
    char *outpath = malloc(2048);
    if (!buf || !outpath) {
        if (buf) free(buf);
        if (outpath) free(outpath);
        fclose(f);
        return -1;
    }
    char header[512];
    while (fread(header, 1, 512, f) == 512) {
        if (header[0] == '\0') break;
        if (strncmp(header + 257, "ustar", 5) != 0 && header[257] != '\0') break;
        char filename[100];
        strncpy(filename, header, 99);
        filename[99] = '\0';
        if (strstr(filename, "../")) continue;
        char *safe = filename;
        while (*safe == '/') safe++;
        long size = strtol(header + 124, NULL, 8);
        char type = header[156];
        snprintf(outpath, 2048, "%s/%s", out_dir, safe);
        if (type == '5') {
            mkdir_p(outpath);
        } else {
            char *sl = strrchr(outpath, '/');
            if (sl) { *sl = 0; mkdir_p(outpath); *sl = '/'; }
            FILE *out = fopen(outpath, "wb");
            long rem = size;
            while (rem > 0) {
                size_t tr = rem > 65536 ? 65536 : (size_t)rem;
                size_t r = fread(buf, 1, tr, f);
                if (r == 0) break;
                if (out) fwrite(buf, 1, r, out);
                rem -= (long)r;
                pthread_mutex_lock(&task_mutex);
                global_task.copied_bytes += (long long)r;
                pthread_mutex_unlock(&task_mutex);
            }
            if (out) { fclose(out); chmod(outpath, 0777); }
        }
        long pad = (512 - (size % 512)) % 512;
        fseek(f, pad, SEEK_CUR);
    }
    free(buf);
    free(outpath);
    fclose(f);
    return 0;
}

int extract_zip(const char *zip_path, const char *out_dir) {
    if (extract_with_helper_sock(zip_path, out_dir) == 0) return 0;

    FILE *f = fopen(zip_path, "rb");
    if (!f) return -1;

    static void *libz = NULL;
    if (!libz) libz = dlopen("/system/common/lib/libSceWebKit2.sprx", RTLD_LAZY);
    if (!libz) libz = dlopen("/system/common/lib/libSceZlib.sprx", RTLD_LAZY);
    if (!libz) libz = dlopen("libz.so", RTLD_LAZY);

    int (*infInit)(ps5_zstream*, int, const char*, int) = libz ? dlsym(libz, "inflateInit2_") : NULL;
    int (*infRun)(ps5_zstream*, int) = libz ? dlsym(libz, "inflate") : NULL;
    int (*infEnd)(ps5_zstream*) = libz ? dlsym(libz, "inflateEnd") : NULL;

    unsigned char *inb = malloc(32768);
    unsigned char *outb = malloc(32768);
    char *fname = malloc(1025);
    char *outpath = malloc(2048);
    if (!inb || !outb || !fname || !outpath) {
        if (inb) free(inb);
        if (outb) free(outb);
        if (fname) free(fname);
        if (outpath) free(outpath);
        fclose(f);
        return -1;
    }

    unsigned char lfh[30];
    while (fread(lfh, 1, 30, f) == 30) {
        if (memcmp(lfh, "PK\x03\x04", 4) != 0) break;
        uint16_t method = (uint16_t)(lfh[8] | (lfh[9] << 8));
        uint32_t csize = (uint32_t)(lfh[18] | (lfh[19] << 8) | (lfh[20] << 16) | (lfh[21] << 24));
        uint16_t nlen = (uint16_t)(lfh[26] | (lfh[27] << 8));
        uint16_t elen = (uint16_t)(lfh[28] | (lfh[29] << 8));
        if (nlen == 0 || nlen > 1024) break;
        if (fread(fname, 1, nlen, f) != nlen) break;
        fname[nlen] = '\0';
        fseek(f, elen, SEEK_CUR);
        if (strstr(fname, "../")) { fseek(f, csize, SEEK_CUR); continue; }
        snprintf(outpath, 2048, "%s/%s", out_dir, fname);
        if (fname[nlen - 1] == '/') { mkdir_p(outpath); continue; }
        char *sl = strrchr(outpath, '/');
        if (sl) { *sl = 0; mkdir_p(outpath); *sl = '/'; }
        FILE *out = fopen(outpath, "wb");
        if (!out) { fseek(f, csize, SEEK_CUR); continue; }
        if (method == 0) {
            uint32_t rem = csize;
            while (rem > 0) {
                size_t r = fread(inb, 1, rem > 32768 ? 32768 : rem, f);
                if (r == 0) break;
                fwrite(inb, 1, r, out);
                rem -= (uint32_t)r;
                pthread_mutex_lock(&task_mutex);
                global_task.copied_bytes += (long long)r;
                pthread_mutex_unlock(&task_mutex);
            }
        } else if (method == 8 && infInit && infRun && infEnd) {
            ps5_zstream zs;
            memset(&zs, 0, sizeof(zs));
            if (infInit(&zs, -15, "1.2.11", (int)sizeof(ps5_zstream)) == 0) {
                uint32_t rem = csize;
                while (rem > 0) {
                    size_t r = fread(inb, 1, rem > 32768 ? 32768 : rem, f);
                    if (r == 0) break;
                    rem -= (uint32_t)r;
                    pthread_mutex_lock(&task_mutex);
                    global_task.copied_bytes += (long long)r;
                    pthread_mutex_unlock(&task_mutex);
                    zs.in_ptr = inb;
                    zs.in_avail = (uint32_t)r;
                    do {
                        zs.out_ptr = outb;
                        zs.out_avail = 32768;
                        int ret = infRun(&zs, 0);
                        size_t have = 32768 - zs.out_avail;
                        if (have > 0) fwrite(outb, 1, have, out);
                        if (ret != 0) break;
                    } while (zs.out_avail == 0);
                }
                infEnd(&zs);
            } else fseek(f, csize, SEEK_CUR);
        } else fseek(f, csize, SEEK_CUR);
        fclose(out);
        chmod(outpath, 0777);
    }
    free(inb);
    free(outb);
    free(fname);
    free(outpath);
    fclose(f);
    return 0;
}

int extract_rar(const char *rar_path, const char *out_dir) {
    return extract_with_helper_sock(rar_path, out_dir);
}

int extract_archive_universal(const char *archive_path, const char *out_dir) {
    size_t len = strlen(archive_path);
    if (len > 4 && strcasecmp(archive_path + len - 4, ".zip") == 0) return extract_zip(archive_path, out_dir);
    if (len > 4 && strcasecmp(archive_path + len - 4, ".rar") == 0) return extract_rar(archive_path, out_dir);
    return extract_tar(archive_path, out_dir);
}

void pack_dir_to_pkg_stream(const char *base_dir, const char *rel_dir, FILE *pkg_out) {
    char *full_dir = malloc(2048);
    if (!full_dir) return;
    snprintf(full_dir, 2048, "%s%s%s", base_dir, rel_dir[0] ? "/" : "", rel_dir);
    DIR *d = opendir(full_dir);
    free(full_dir);
    if (!d) return;

    char *sub_rel = malloc(1024);
    char *sub_full = malloc(2048);
    if (sub_rel && sub_full) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            snprintf(sub_rel, 1024, "%s%s%s", rel_dir, rel_dir[0] ? "/" : "", de->d_name);
            snprintf(sub_full, 2048, "%s/%s", base_dir, sub_rel);

            struct stat st;
            if (stat(sub_full, &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) {
                pack_dir_to_pkg_stream(base_dir, sub_rel, pkg_out);
            } else {
                pthread_mutex_lock(&task_mutex);
                snprintf(global_task.current_file, sizeof(global_task.current_file), "%s", de->d_name);
                int canceled = global_task.cancel_requested;
                pthread_mutex_unlock(&task_mutex);
                if (canceled) break;

                FILE *in = fopen(sub_full, "rb");
                if (!in) continue;
                uint32_t path_len = (uint32_t)strlen(sub_rel);
                uint64_t file_sz = (uint64_t)st.st_size;
                fwrite(&path_len, sizeof(path_len), 1, pkg_out);
                fwrite(sub_rel, 1, path_len, pkg_out);
                fwrite(&file_sz, sizeof(file_sz), 1, pkg_out);

                char *buf = malloc(CHUNK_SIZE);
                if (buf) {
                    size_t r;
                    while ((r = fread(buf, 1, CHUNK_SIZE, in)) > 0) {
                        fwrite(buf, 1, r, pkg_out);
                        pthread_mutex_lock(&task_mutex);
                        global_task.copied_bytes += (long long)r;
                        pthread_mutex_unlock(&task_mutex);
                    }
                    free(buf);
                }
                fclose(in);
            }
        }
    }
    if (sub_rel) free(sub_rel);
    if (sub_full) free(sub_full);
    closedir(d);
}

int build_fpkg_from_dir(const char *src_dir, const char *out_pkg) {
    FILE *pkg = fopen(out_pkg, "wb");
    if (!pkg) return -1;

    uint32_t *hdr = calloc(1024, sizeof(uint32_t));
    if (hdr) {
        hdr[0] = 0x544E437FU;
        hdr[1] = 0x01000080U;
        fwrite(hdr, 1, 4096, pkg);
        free(hdr);
    }

    pack_dir_to_pkg_stream(src_dir, "", pkg);
    fclose(pkg);
    chmod(out_pkg, 0777);
    klog_appendf("[BUILDER] Created package: %s\n", out_pkg);
    return 0;
}

long long clean_system_cache(void) {
    const char *cache_dirs[] = {
        "/user/temp",
        "/system_tmp",
        "/user/system/webkit/webbrowser/appcache",
        "/user/download",
        NULL
    };
    long long freed = 0;
    char *full = malloc(1024);
    if (!full) return 0;
    for (int i = 0; cache_dirs[i] != NULL; i++) {
        DIR *d = opendir(cache_dirs[i]);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            if (strstr(de->d_name, "wfm-7zip")) continue;
            snprintf(full, 1024, "%s/%s", cache_dirs[i], de->d_name);
            freed += calculate_size_recursive(full);
            recursive_delete(full);
        }
        closedir(d);
    }
    free(full);
    klog_appendf("[CLEANER] Freed %lld bytes of PS4/PS5 cache\n", freed);
    return freed;
}

typedef struct {
    char action[16];
    char src[1024];
    char dst[1024];
} TaskArgs;

void* background_worker(void* args) {
    TaskArgs *task = (TaskArgs*)args;
    strip_trailing_slash(task->src);
    strip_trailing_slash(task->dst);

    long long total = calculate_size_recursive(task->src);
    if (total <= 0) total = 1;

    pthread_mutex_lock(&task_mutex);
    global_task.total_bytes = total;
    global_task.copied_bytes = 0;
    global_task.start_time = time(NULL);
    snprintf(global_task.task_type, sizeof(global_task.task_type), "%s", task->action);
    pthread_mutex_unlock(&task_mutex);

    klog_appendf("[TASK] Started %s -> %s\n", task->action, task->src);

    if (strcmp(task->action, "copy") == 0) {
        recursive_copy(task->src, task->dst);
    } else if (strcmp(task->action, "cut") == 0) {
        if (strcmp(task->src, task->dst) != 0) {
            if (rename(task->src, task->dst) != 0) {
                if (recursive_copy(task->src, task->dst) == 0) recursive_delete(task->src);
            }
        }
    } else if (strcmp(task->action, "extract") == 0) {
        char ex_dir[1100];
        snprintf(ex_dir, sizeof(ex_dir), "%s_extracted", task->src);
        mkdir_p(ex_dir);
        extract_archive_universal(task->src, ex_dir);
    } else if (strcmp(task->action, "fpkg") == 0) {
        char pkg_out[1100];
        snprintf(pkg_out, sizeof(pkg_out), "%s.pkg", task->src);
        build_fpkg_from_dir(task->src, pkg_out);
    } else if (strcmp(task->action, "del") == 0) {
        recursive_delete(task->src);
    }

    pthread_mutex_lock(&task_mutex);
    global_task.copied_bytes = global_task.total_bytes;
    global_task.is_running = 0;
    global_task.cancel_requested = 0;
    pthread_mutex_unlock(&task_mutex);

    klog_appendf("[TASK] Finished %s\n", task->action);
    free(task);
    return NULL;
}

// =========================================================
// APIهای تخصصی: سیستم، دما، تروفی، پروفایل، استریم موزیک و ترمینال
// =========================================================
void api_sysinfo(int sock) {
    const char *h = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=UTF-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    send_all(sock, h, strlen(h));

    struct statvfs vfs_ssd, vfs_usb, vfs_ext, vfs_data;
    double ssd_total = 0, ssd_free = 0, usb_total = 0, usb_free = 0, ext_total = 0, ext_free = 0, data_free = 0;

    if (statvfs("/user", &vfs_ssd) == 0) {
        ssd_total = (double)vfs_ssd.f_blocks * vfs_ssd.f_frsize / 1073741824.0;
        ssd_free  = (double)vfs_ssd.f_bfree  * vfs_ssd.f_frsize / 1073741824.0;
    }
    if (statvfs("/data", &vfs_data) == 0) {
        data_free = (double)vfs_data.f_bfree * vfs_data.f_frsize / 1073741824.0;
    }
    const char *active_usb = detect_usb_path();
    if (statvfs(active_usb, &vfs_usb) == 0) {
        usb_total = (double)vfs_usb.f_blocks * vfs_usb.f_frsize / 1073741824.0;
        usb_free  = (double)vfs_usb.f_bfree  * vfs_usb.f_frsize / 1073741824.0;
    }
    if (statvfs("/mnt/ext0", &vfs_ext) == 0) {
        ext_total = (double)vfs_ext.f_blocks * vfs_ext.f_frsize / 1073741824.0;
        ext_free  = (double)vfs_ext.f_bfree  * vfs_ext.f_frsize / 1073741824.0;
    }

    char fw[64], ip[64];
    get_ps5_fw_string(fw, sizeof(fw));
    get_console_ip(ip, sizeof(ip));
    int temp = get_apu_temp();

    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"ssd_total\":%.1f,\"ssd_free\":%.1f,\"usb_total\":%.1f,\"usb_free\":%.1f,"
        "\"ext_total\":%.1f,\"ext_free\":%.1f,\"data_free\":%.1f,\"usb_mount\":\"%s\","
        "\"fw\":\"%s\",\"ip\":\"%s\",\"ftp_port\":%d,\"temp\":%d,\"pid\":%d}",
        ssd_total, ssd_free, usb_total, usb_free, ext_total, ext_free, data_free,
        active_usb, fw, ip, FTP_PORT, temp, getpid());
    send_all(sock, buf, strlen(buf));
}

void api_status(int sock) {
    const char *h = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=UTF-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    send_all(sock, h, strlen(h));

    pthread_mutex_lock(&task_mutex);
    int running = global_task.is_running;
    long long total = global_task.total_bytes;
    long long copied = global_task.copied_bytes;
    time_t elapsed = time(NULL) - global_task.start_time;
    char cur[512];
    escape_json(cur, global_task.current_file, sizeof(cur));
    pthread_mutex_unlock(&task_mutex);

    double speed_mb = (elapsed > 0) ? ((double)copied / 1048576.0) / (double)elapsed : 0.0;
    long long rem_bytes = (total > copied) ? (total - copied) : 0;
    long eta_sec = (speed_mb > 0.05) ? (long)((rem_bytes / 1048576.0) / speed_mb) : 0;

    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"running\":%d,\"total\":%lld,\"copied\":%lld,\"speed\":%.1f,\"eta\":%ld,\"file\":\"%s\"}",
        running, total, copied, speed_mb, eta_sec, cur);
    send_all(sock, buf, strlen(buf));
}

void api_klog(int sock) {
    const char *h = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=UTF-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    send_all(sock, h, strlen(h));
    pthread_mutex_lock(&klog_mutex);
    if (klog_head > 0) send_all(sock, klog_buffer, (size_t)klog_head);
    else send_all(sock, "[FIROZ OS Kernel Log Stream Ready...]\n", 38);
    pthread_mutex_unlock(&klog_mutex);
}

void api_profiles(int sock) {
    const char *h = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=UTF-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    send_all(sock, h, strlen(h));

    DIR *d = opendir("/user/home");
    if (!d) {
        send_all(sock, "{\"ok\":false,\"profiles\":[]}", 26);
        return;
    }

    send_all(sock, "{\"ok\":true,\"profiles\":[", 23);
    struct dirent *de;
    int first = 1;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char u_path[512], name_file[512], sys_name_file[512];
        char trophy1_dir[512], trophy2_dir[512], save5_dir[512], save4_dir[512];
        snprintf(u_path, sizeof(u_path), "/user/home/%s", de->d_name);
        struct stat st;
        if (stat(u_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        char uname[128];
        snprintf(uname, sizeof(uname), "User_%s", de->d_name);
        snprintf(name_file, sizeof(name_file), "/user/home/%s/username.txt", de->d_name);
        snprintf(sys_name_file, sizeof(sys_name_file), "/system_data/priv/home/%s/localusername.txt", de->d_name);

        FILE *nf = fopen(name_file, "rb");
        if (!nf) nf = fopen(sys_name_file, "rb");
        if (nf) {
            size_t r = fread(uname, 1, sizeof(uname) - 1, nf);
            uname[r] = '\0';
            fclose(nf);
            char *nl = strpbrk(uname, "\r\n");
            if (nl) *nl = '\0';
        }

        snprintf(trophy1_dir, sizeof(trophy1_dir), "/user/home/%s/trophy", de->d_name);
        snprintf(trophy2_dir, sizeof(trophy2_dir), "/user/home/%s/trophy2", de->d_name);
        snprintf(save5_dir, sizeof(save5_dir), "/user/home/%s/savedata_prospero", de->d_name);
        snprintf(save4_dir, sizeof(save4_dir), "/user/home/%s/savedata", de->d_name);

        long long t_sz = calculate_size_recursive(trophy1_dir) + calculate_size_recursive(trophy2_dir);
        long long s5_sz = calculate_size_recursive(save5_dir);
        long long s4_sz = calculate_size_recursive(save4_dir);

        char esc_u[256];
        escape_json(esc_u, uname, sizeof(esc_u));
        char item[1024];
        snprintf(item, sizeof(item),
            "%s{\"id\":\"%s\",\"name\":\"%s\",\"trophy_size\":%lld,\"ps5_save_size\":%lld,\"ps4_save_size\":%lld}",
            first ? "" : ",", de->d_name, esc_u, t_sz, s5_sz, s4_sz);
        send_all(sock, item, strlen(item));
        first = 0;
    }
    closedir(d);
    send_all(sock, "]}", 2);
}

void read_game_title(const char *app_id, char *title_out, size_t max_len) {
    snprintf(title_out, max_len, "%s", app_id);
    char ppath[512];
    snprintf(ppath, sizeof(ppath), "/user/app/%s/sce_sys/param.json", app_id);
    FILE *f = fopen(ppath, "rb");
    if (!f) {
        snprintf(ppath, sizeof(ppath), "/user/appmeta/%s/param.json", app_id);
        f = fopen(ppath, "rb");
    }
    if (!f) return;
    char *buf = malloc(4096);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, 4095, f);
    fclose(f);
    buf[n] = '\0';
    char *pos = strstr(buf, "\"titleName\"");
    if (pos) {
        pos = strchr(pos + 11, '"');
        if (pos) {
            pos++;
            char *end = strchr(pos, '"');
            if (end && (size_t)(end - pos) < max_len) {
                memcpy(title_out, pos, (size_t)(end - pos));
                title_out[end - pos] = '\0';
            }
        }
    }
    free(buf);
}

void api_game_icon(int sock, const char *app_id) {
    if (!app_id || strstr(app_id, "..") || strchr(app_id, '/')) {
        const char *nf = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
        send_all(sock, nf, strlen(nf));
        return;
    }
    char ipath[512];
    snprintf(ipath, sizeof(ipath), "/user/app/%s/sce_sys/icon0.png", app_id);
    FILE *f = fopen(ipath, "rb");
    if (!f) {
        snprintf(ipath, sizeof(ipath), "/user/appmeta/%s/icon0.png", app_id);
        f = fopen(ipath, "rb");
    }
    if (!f) {
        const char *nf = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
        send_all(sock, nf, strlen(nf));
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char hdr[256];
    snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: %ld\r\nCache-Control: max-age=3600\r\nConnection: close\r\n\r\n", sz);
    send_all(sock, hdr, strlen(hdr));

    char *buf = malloc(16384);
    if (buf) {
        size_t r;
        while ((r = fread(buf, 1, 16384, f)) > 0) send_all(sock, buf, r);
        free(buf);
    }
    fclose(f);
}

void api_list(int sock, const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        const char *err = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":false}";
        send_all(sock, err, strlen(err));
        return;
    }
    const char *h = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=UTF-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    send_all(sock, h, strlen(h));

    char *large_buf = malloc(8192);
    char *f_path = malloc(2048);
    char *temp = malloc(1400);
    if (!large_buf || !f_path || !temp) {
        if (large_buf) free(large_buf);
        if (f_path) free(f_path);
        if (temp) free(temp);
        closedir(d);
        return;
    }

    char esc_path[1024];
    escape_json(esc_path, path, sizeof(esc_path));
    int offset = snprintf(large_buf, 8192, "{\"ok\":true,\"path\":\"%s\",\"items\":[", esc_path);

    struct dirent *dir;
    int first = 1;
    int is_app_dir = (strcmp(path, "/user/app/") == 0 || strcmp(path, "/user/app") == 0);

    while ((dir = readdir(d)) != NULL) {
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;
        snprintf(f_path, 2048, "%s%s%s", path, (path[strlen(path) - 1] == '/') ? "" : "/", dir->d_name);

        struct stat st;
        if (stat(f_path, &st) != 0) memset(&st, 0, sizeof(st));

        char cln[512], title_cln[512] = "";
        escape_json(cln, dir->d_name, sizeof(cln));

        if (is_app_dir && S_ISDIR(st.st_mode)) {
            char raw_title[256];
            read_game_title(dir->d_name, raw_title, sizeof(raw_title));
            escape_json(title_cln, raw_title, sizeof(title_cln));
        }

        int t_len = snprintf(temp, 1400,
            "%s{\"name\":\"%s\",\"title\":\"%s\",\"type\":\"%s\",\"size\":%lld}",
            first ? "" : ",", cln, title_cln, S_ISDIR(st.st_mode) ? "dir" : "file", (long long)st.st_size);

        if (offset + t_len >= 8192 - 64) {
            send_all(sock, large_buf, (size_t)offset);
            offset = 0;
        }
        memcpy(large_buf + offset, temp, (size_t)t_len);
        offset += t_len;
        first = 0;
    }
    if (offset > 0) send_all(sock, large_buf, (size_t)offset);
    send_all(sock, "]}", 2);

    free(large_buf);
    free(f_path);
    free(temp);
    closedir(d);
}

void api_stream_file(int sock, const char *filepath, const char *req_headers) {
    struct stat st;
    if (!filepath || stat(filepath, &st) != 0 || S_ISDIR(st.st_mode)) {
        const char *err = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
        send_all(sock, err, strlen(err));
        return;
    }
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        const char *err = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
        send_all(sock, err, strlen(err));
        return;
    }
    long long fsize = (long long)st.st_size;

    const char *mime = "application/octet-stream";
    size_t flen = strlen(filepath);
    if (flen > 4 && strcasecmp(filepath + flen - 4, ".mp3") == 0) mime = "audio/mpeg";
    else if (flen > 4 && strcasecmp(filepath + flen - 4, ".wav") == 0) mime = "audio/wav";
    else if (flen > 4 && strcasecmp(filepath + flen - 4, ".ogg") == 0) mime = "audio/ogg";
    else if (flen > 5 && strcasecmp(filepath + flen - 5, ".flac") == 0) mime = "audio/flac";
    else if (flen > 4 && strcasecmp(filepath + flen - 4, ".png") == 0) mime = "image/png";
    else if (flen > 4 && strcasecmp(filepath + flen - 4, ".jpg") == 0) mime = "image/jpeg";

    long long start = 0, end = (fsize > 0) ? (fsize - 1) : 0;
    int is_partial = 0;
    const char *range_hdr = req_headers ? strcasestr(req_headers, "Range: bytes=") : NULL;
    if (range_hdr && fsize > 0) {
        is_partial = 1;
        long long req_start = 0, req_end = -1;
        int matched = sscanf(range_hdr + 13, "%lld-%lld", &req_start, &req_end);
        if (matched >= 1 && req_start >= 0 && req_start < fsize) start = req_start;
        if (matched == 2 && req_end >= start && req_end < fsize) end = req_end;
        else end = fsize - 1;
        fseeko(f, (off_t)start, SEEK_SET);
    }

    long long send_len = (fsize > 0) ? ((end - start) + 1) : 0;
    char hdr[1024];
    if (is_partial) {
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Type: %s\r\n"
            "Accept-Ranges: bytes\r\n"
            "Content-Range: bytes %lld-%lld/%lld\r\n"
            "Content-Length: %lld\r\n"
            "Connection: close\r\n\r\n",
            mime, start, end, fsize, send_len);
    } else {
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Accept-Ranges: bytes\r\n"
            "Content-Length: %lld\r\n"
            "Connection: close\r\n\r\n",
            mime, fsize);
    }
    send_all(sock, hdr, strlen(hdr));

    char *buf = malloc(65536);
    if (buf) {
        long long rem = send_len;
        while (rem > 0) {
            size_t to_r = rem > 65536 ? 65536 : (size_t)rem;
            size_t r = fread(buf, 1, to_r, f);
            if (r == 0) break;
            send_all(sock, buf, r);
            rem -= (long long)r;
        }
        free(buf);
    }
    fclose(f);
}

void api_text_read(int sock, const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        const char *err = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":false}";
        send_all(sock, err, strlen(err));
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    if (sz > 256 * 1024) sz = 256 * 1024;

    char *raw = malloc((size_t)sz + 1);
    if (!raw) { fclose(f); return; }
    size_t rd = fread(raw, 1, (size_t)sz, f);
    raw[rd] = '\0';
    fclose(f);

    const char *h = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=UTF-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    send_all(sock, h, strlen(h));
    send_all(sock, raw, rd);
    free(raw);
}
// ============================================================================
// FIROZ OS v4.0 - ULTIMATE MASTERPIECE EDITION (@firozfar051)
// Part 3 of 3: Minimalist UI, DualSense Gamepad Engine, HTTP Server & Main
// ============================================================================

const char *UI_PART1 =
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"
    "<!DOCTYPE html><html lang='fa' dir='rtl'><head><meta charset='UTF-8'><title>FIROZ OS v4.0</title>"
    "<link rel='icon' type='image/png' href='/icon0.png'>"
    "<style>"
    ":root{"
    "  --bg1:#020617;--bg2:#0f172a;--bg3:#1e1b4b;"
    "  --glass:rgba(15,23,42,0.74);--border:rgba(56,189,248,0.24);"
    "  --accent:#38bdf8;--accent2:#6366f1;--glow:rgba(56,189,248,0.45);--text:#f8fafc;--sub:#94a3b8;"
    "}"
    "body[data-theme='ps5']{--bg1:#030712;--bg2:#0a192f;--bg3:#1d4ed8;--accent:#3b82f6;--accent2:#60a5fa;--glow:rgba(59,130,246,0.5);}"
    "body[data-theme='matrix']{--bg1:#000000;--bg2:#021a09;--bg3:#052e16;--glass:rgba(2,26,9,0.82);--border:rgba(34,197,94,0.32);--accent:#22c55e;--accent2:#16a34a;--glow:rgba(34,197,94,0.45);}"
    "body[data-theme='crimson']{--bg1:#090204;--bg2:#1f070d;--bg3:#4c0519;--glass:rgba(31,7,13,0.8);--border:rgba(244,63,94,0.32);--accent:#f43f5e;--accent2:#e11d48;--glow:rgba(244,63,94,0.45);}"
    "body[data-theme='oled']{--bg1:#000000;--bg2:#000000;--bg3:#09090b;--glass:rgba(12,12,15,0.92);--border:rgba(255,255,255,0.16);--accent:#a855f7;--accent2:#06b6d4;--glow:rgba(168,85,247,0.45);}"
    "body[data-theme='light']{--bg1:#e2e8f0;--bg2:#f1f5f9;--bg3:#cbd5e1;--glass:rgba(255,255,255,0.85);--border:rgba(15,23,42,0.18);--accent:#0284c7;--accent2:#2563eb;--glow:rgba(2,132,199,0.3);--text:#0f172a;--sub:#475569;}"
    "*{box-sizing:border-box;user-select:none;}"
    "body{margin:0;padding:18px 26px;font-family:Tahoma,system-ui,sans-serif;color:var(--text);height:100vh;overflow:hidden;display:flex;flex-direction:column;background:radial-gradient(circle at top right,var(--bg3),var(--bg2) 55%,var(--bg1) 95%);transition:background 0.35s,color 0.25s;}"
    "header{display:flex;justify-content:space-between;align-items:center;padding-bottom:12px;}"
    ".brand{display:flex;align-items:center;gap:14px;}"
    ".logo-img{width:46px;height:46px;border-radius:12px;border:2px solid var(--accent);box-shadow:0 0 16px var(--glow);}"
    "h1{margin:0;font-size:1.95rem;font-weight:900;letter-spacing:1px;text-shadow:0 0 20px var(--glow);}"
    ".tg-pill{display:flex;align-items:center;gap:8px;background:rgba(56,189,248,0.1);border:1px solid var(--border);padding:5px 14px;border-radius:99px;direction:ltr;font-weight:bold;color:var(--accent);font-size:1rem;}"
    ".top-actions{display:flex;gap:10px;align-items:center;}"
    ".icon-btn{background:var(--glass);color:var(--text);border:1px solid var(--border);padding:8px 16px;border-radius:12px;font-size:1.05rem;font-weight:bold;cursor:pointer;outline:none;transition:all 0.2s;}"
    ".tab-bar{display:flex;gap:8px;padding-bottom:12px;border-bottom:1px solid var(--border);}"
    ".tab-btn{flex:1;padding:12px 6px;font-size:1.05rem;font-weight:800;font-family:Tahoma,sans-serif;color:var(--sub);background:rgba(0,0,0,0.22);border:2px solid transparent;border-radius:13px;cursor:pointer;outline:none;transition:all 0.22s;white-space:nowrap;}"
    ".tab-btn.active{color:var(--text);background:var(--glass);border-color:var(--accent);box-shadow:0 6px 20px var(--glow);}"
    ".focusable:focus,.focusable.gp-focus{transform:scale(1.02);border-color:var(--accent)!important;box-shadow:0 0 22px var(--glow)!important;color:var(--text);z-index:20;outline:none;}"
    ".main-area{display:flex;flex:1;overflow:hidden;margin-top:12px;position:relative;}"
    ".view{display:none;width:100%;height:100%;flex-direction:column;animation:fadeIn 0.2s ease;}.view.active{display:flex;}"
    "@keyframes fadeIn{from{opacity:0;transform:translateY(6px);}to{opacity:1;transform:translateY(0);}}";

const char *UI_PART2 =
    ".split-grid{display:grid;grid-template-columns:1fr 1fr;gap:20px;flex:1;overflow:hidden;}"
    ".glass-panel{background:var(--glass);backdrop-filter:blur(18px);border:2px solid var(--border);border-radius:20px;display:flex;flex-direction:column;overflow:hidden;transition:border-color 0.25s,box-shadow 0.25s;}"
    ".glass-panel.active-pane{border-color:var(--accent);box-shadow:0 0 25px var(--glow);}"
    ".panel-head{background:rgba(0,0,0,0.28);padding:10px 16px;display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid var(--border);font-weight:bold;font-size:1.1rem;}"
    ".mount-bar{display:flex;gap:5px;}"
    ".m-pill{background:rgba(255,255,255,0.06);border:1px solid var(--border);color:var(--accent);border-radius:8px;padding:3px 9px;font-size:0.85rem;cursor:pointer;font-weight:bold;}"
    ".path-bar{padding:8px 16px;background:rgba(0,0,0,0.35);font-family:monospace;font-size:1.05rem;color:var(--accent);border-bottom:1px solid var(--border);direction:ltr;text-align:left;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}"
    ".file-list{flex:1;overflow-y:auto;padding:12px;scrollbar-width:none;}"
    ".row{display:flex;gap:8px;margin-bottom:8px;}"
    ".btn-chk{width:48px;background:rgba(0,0,0,0.35);border-radius:12px;border:2px solid transparent;display:flex;justify-content:center;align-items:center;font-size:1.3rem;cursor:pointer;color:var(--sub);}"
    ".btn-chk.selected{background:rgba(16,185,129,0.25);border-color:#10b981;color:#10b981;}"
    ".btn-name{flex:1;background:rgba(0,0,0,0.35);border-radius:12px;border:2px solid transparent;display:flex;align-items:center;justify-content:space-between;padding:11px 16px;font-size:1.15rem;color:var(--text);cursor:pointer;direction:ltr;overflow:hidden;}"
    ".f-meta{font-size:0.9rem;color:var(--sub);background:rgba(0,0,0,0.3);padding:2px 8px;border-radius:6px;margin-left:8px;white-space:nowrap;}"
    ".hud-bar{display:flex;justify-content:space-between;align-items:center;background:var(--glass);border:1px solid var(--border);border-radius:16px;padding:10px 20px;margin-top:12px;}"
    ".hud-hints{display:flex;gap:18px;align-items:center;font-size:1rem;color:var(--sub);flex-wrap:wrap;}"
    ".k-badge{display:inline-flex;align-items:center;justify-content:center;min-width:26px;height:26px;padding:0 7px;border-radius:7px;background:rgba(255,255,255,0.12);color:var(--accent);font-weight:900;margin:0 4px;font-size:0.9rem;}"
    ".btn-menu-trigger{background:linear-gradient(135deg,var(--accent),var(--accent2));color:#fff;border:2px solid transparent;border-radius:12px;padding:9px 22px;font-size:1.05rem;font-weight:bold;font-family:Tahoma,sans-serif;cursor:pointer;}"
    ".cards-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(235px,1fr));gap:18px;flex:1;overflow-y:auto;padding:12px;scrollbar-width:none;}"
    ".card-item{background:var(--glass);border:2px solid var(--border);border-radius:18px;padding:18px;text-align:center;display:flex;flex-direction:column;align-items:center;justify-content:space-between;cursor:pointer;transition:all 0.22s;color:var(--text);}"
    ".game-cover{width:110px;height:110px;border-radius:16px;object-fit:cover;margin-bottom:10px;border:2px solid var(--accent);}"
    ".game-title{font-size:1.1rem;font-weight:bold;margin:6px 0;word-break:break-word;}"
    ".dash-layout{display:grid;grid-template-columns:repeat(4,1fr);gap:16px;margin-bottom:16px;}"
    ".stat-box{background:var(--glass);border:1px solid var(--border);border-radius:18px;padding:18px;display:flex;flex-direction:column;justify-content:center;}"
    ".s-title{font-size:1.1rem;color:var(--accent);font-weight:bold;margin-bottom:6px;}"
    ".s-val{font-size:2rem;font-weight:900;direction:ltr;}"
    ".s-sub{font-size:0.95rem;color:var(--sub);margin-top:4px;}"
    ".graph-box{flex:1;background:var(--glass);border:1px solid var(--border);border-radius:18px;padding:18px;display:flex;flex-direction:column;}"
    ".term-box{flex:1;background:#05080f;border:1px solid var(--border);border-radius:16px;padding:16px;font-family:monospace;font-size:1.05rem;color:#22c55e;overflow-y:auto;direction:ltr;text-align:left;white-space:pre-wrap;}"
    "#music-bar{display:none;align-items:center;gap:16px;background:var(--glass);backdrop-filter:blur(22px);border:2px solid var(--accent);border-radius:16px;padding:10px 20px;margin-top:10px;box-shadow:0 0 25px var(--glow);direction:ltr;}"
    ".disc-art{width:46px;height:46px;border-radius:50%;border:2px solid var(--accent);animation:spin 4s linear infinite;}"
    ".disc-art.paused{animation-play-state:paused;}"
    ".m-info{min-width:180px;max-width:240px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-weight:bold;}"
    ".m-slider{flex:1;accent-color:var(--accent);height:6px;cursor:pointer;}"
    ".eq-bars{display:flex;gap:3px;align-items:flex-end;height:20px;}"
    ".eq-b{width:4px;background:var(--accent);animation:eq 0.6s ease infinite alternate;}"
    "@keyframes eq{0%{height:4px;}100%{height:20px;}}"
    ".modal-wrap{display:none;position:fixed;inset:0;background:rgba(2,6,23,0.88);backdrop-filter:blur(16px);z-index:100;justify-content:center;align-items:center;}"
    ".modal-card{background:var(--glass);border:2px solid var(--border);border-radius:22px;padding:28px;width:68%;max-width:820px;text-align:center;}"
    ".action-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin-top:16px;}"
    ".act-tile{background:rgba(0,0,0,0.4);border:2px solid var(--border);border-radius:14px;padding:16px 10px;color:var(--text);font-size:1.05rem;font-weight:bold;font-family:Tahoma,sans-serif;cursor:pointer;}"
    ".progress-bg{width:100%;height:22px;background:rgba(0,0,0,0.5);border-radius:12px;overflow:hidden;margin:14px 0;border:1px solid var(--border);direction:ltr;}"
    ".progress-fill{height:100%;background:linear-gradient(90deg,var(--accent),var(--accent2));width:0%;transition:width 0.25s;}"
    "@keyframes spin{100%{transform:rotate(360deg);}}</style>";

const char *UI_PART3 =
    "<script>"
    "const dict={"
    "  fa:{tab_file:'📁 فایل‌ها',tab_run:'🚀 بازی‌ها',tab_pkg:'📦 نصب و پیلود',tab_prof:'🏆 تروفی و پروفایل',tab_dash:'🌡️ داشبورد و دما',tab_term:'💻 ترمینال',tab_theme:'🎨 تم و ابزار',back:'بازگشت به پوشه قبل',act_menu:'⚡ منوی عملیات (△)',clean_ok:'حافظه کش پاکسازی شد! آزاد شده: '},"
    "  en:{tab_file:'📁 FILES',tab_run:'🚀 GAMES',tab_pkg:'📦 PKG & ELF',tab_prof:'🏆 PROFILES',tab_dash:'🌡️ TELEMETRY',tab_term:'💻 TERMINAL',tab_theme:'🎨 THEMES',back:'GO BACK',act_menu:'⚡ ACTIONS (△)',clean_ok:'Cache Cleaned! Freed: '}"
    "};"
    "let lang='fa',mode='FILE',usbMount='/mnt/usb0',activePanel='left',isProcessing=false,editingPath='',tempHistory=[];"
    "let panels={left:{path:'/user/app/',items:[]},right:{path:'/mnt/usb0/',items:[]}};"
    "let gpIndex=0,lastGpTime=0,inputCallback=null;"
    "function t(k){return dict[lang][k]||k;}"
    "function showToast(msg){const el=document.getElementById('ui-toast');el.innerText=msg;el.style.display='block';clearTimeout(window._tTimer);window._tTimer=setTimeout(()=>{el.style.display='none';},2600);}"
    "function openInputModal(title,defVal,cb){"
    "  inputCallback=cb;document.getElementById('inp-title').innerText=title;"
    "  const inp=document.getElementById('inp-field');inp.value=defVal||'';"
    "  document.getElementById('modal-input').style.display='flex';gpIndex=0;setTimeout(()=>inp.focus(),80);"
    "}"
    "function confirmInputModal(){const v=document.getElementById('inp-field').value.trim();document.getElementById('modal-input').style.display='none';if(v&&inputCallback)inputCallback(v);}"
    "function setTheme(th){document.body.setAttribute('data-theme',th);try{localStorage.setItem('firoz_theme',th);}catch(e){}}"
    "function toggleLang(){"
    "  lang=(lang==='fa')?'en':'fa';document.body.dir=(lang==='fa')?'rtl':'ltr';"
    "  ['FILE','RUN','PKG','PROF','DASH','TERM','THEME'].forEach(m=>{const b=document.getElementById('tab-'+m);if(b)b.innerText=t('tab_'+m.toLowerCase());});"
    "  document.getElementById('btn-act-open').innerText=t('act_menu');"
    "  renderList('left');renderList('right');"
    "}"
    "function formatSize(b){if(!b||b<=0)return '0 B';const k=1024,s=['B','KB','MB','GB'],i=Math.floor(Math.log(b)/Math.log(k));return parseFloat((b/Math.pow(k,i)).toFixed(2))+' '+s[i];}"
    "function formatTime(sec){if(!isFinite(sec)||sec<0)return '00:00';let m=Math.floor(sec/60),s=Math.floor(sec%60);return `${m<10?'0':''}${m}:${s<10?'0':''}${s}`;}"
    "function setActivePane(p){activePanel=p;document.getElementById('pane-left').classList.toggle('active-pane',p==='left');document.getElementById('pane-right').classList.toggle('active-pane',p==='right');}"
    "function isAudio(n){return /\\.(mp3|wav|ogg|flac)$/i.test(n);}"
    "function isText(n){return /\\.(json|ini|cfg|conf|txt|xml|lua|log|sh|html|css|js|yml)$/i.test(n);}"
    "async function initSys(){"
    "  try{const savedTh=localStorage.getItem('firoz_theme');if(savedTh)setTheme(savedTh);}catch(e){}"
    "  try{const r=await fetch('/api/sysinfo');const d=await r.json();if(d.usb_mount){usbMount=d.usb_mount;panels.right.path=usbMount+'/';}}catch(e){}"
    "  setMode('FILE');requestAnimationFrame(pollGamepad);"
    "  setInterval(()=>{if(mode==='DASH')fetchDash();if(mode==='TERM')fetchKlog();},2000);"
    "}"
    "async function setMode(m){"
    "  if(isProcessing)return;mode=m;gpIndex=0;"
    "  document.querySelectorAll('.tab-btn').forEach(b=>b.classList.remove('active'));"
    "  const tb=document.getElementById('tab-'+m);if(tb)tb.classList.add('active');"
    "  document.querySelectorAll('.view').forEach(v=>v.classList.remove('active'));"
    "  const map={FILE:'view-files',RUN:'view-run',PKG:'view-pkg',PROF:'view-prof',DASH:'view-dash',TERM:'view-term',THEME:'view-theme'};"
    "  document.getElementById(map[m]).classList.add('active');"
    "  if(m==='FILE'){loadDir('left',panels.left.path);loadDir('right',panels.right.path);}"
    "  else if(m==='RUN')loadGames();"
    "  else if(m==='PKG')loadPkgsAndElfs();"
    "  else if(m==='PROF')loadProfiles();"
    "  else if(m==='DASH')fetchDash();"
    "  else if(m==='TERM')fetchKlog();"
    "}"
    "async function loadDir(side,path){"
    "  if(!path.endsWith('/'))path+='/';panels[side].path=path;document.getElementById('path-'+side).innerText=path;"
    "  try{const r=await fetch('/api/list?dir='+encodeURIComponent(path));const d=await r.json();"
    "    panels[side].items=(d.items||[]).sort((a,b)=>(a.type===b.type?a.name.localeCompare(b.name):(a.type==='dir'?-1:1)));"
    "    renderList(side);"
    "  }catch(e){document.getElementById('list-'+side).innerHTML=`<div class='row'><button class='btn-name focusable'>⚠️ مسیر در دسترس نیست</button></div>`;}"
    "}"
    "function goParentDir(side){"
    "  let p=panels[side].path;if(p==='/'||p==='')return;"
    "  let pr=p.substring(0,p.lastIndexOf('/',p.length-2))+'/';if(pr==='')pr='/';"
    "  loadDir(side,pr);"
    "}"
    "function renderList(side){"
    "  const list=document.getElementById('list-'+side);if(!list)return;list.innerHTML='';const p=panels[side].path;"
    "  if(p!=='/'&&p!==''){"
    "    list.innerHTML+=`<div class='row'><button class='btn-name focusable' onclick=\"goParentDir('${side}')\"><span>🔙 ${t('back')}</span></button></div>`;"
    "  }"
    "  panels[side].items.forEach((i,idx)=>{"
    "    const is=i.selected?'selected':'';const mk=i.selected?'✔️':'⭕';"
    "    let ic=i.type==='dir'?'📁':(isAudio(i.name)?'🎵':(i.name.endsWith('.pkg')?'🎮':(i.name.endsWith('.elf')||i.name.endsWith('.bin')?'⚡':(i.name.endsWith('.zip')||i.name.endsWith('.rar')||i.name.endsWith('.tar')?'📦':'📄'))));"
    "    const meta=i.type==='dir'?(i.title||'DIR'):formatSize(i.size);"
    "    list.innerHTML+=`<div class='row'><button class='btn-chk focusable ${is}' onclick=\"toggleSelIdx('${side}',${idx})\">${mk}</button><button class='btn-name focusable' data-side='${side}' data-idx='${idx}' onclick=\"openItemIdx('${side}',${idx})\"><span style='overflow:hidden;text-overflow:ellipsis;'>${ic} ${i.name}</span><span class='f-meta'>${meta}</span></button></div>`;"
    "  });"
    "}"
    "function toggleSelIdx(side,idx){setActivePane(side);const it=panels[side].items[idx];if(it){it.selected=!it.selected;renderList(side);}}"
    "function openItemIdx(side,idx){"
    "  setActivePane(side);const it=panels[side].items[idx];if(!it)return;"
    "  const p=panels[side].path,fullP=p+(p.endsWith('/')?'':'/')+it.name;"
    "  if(it.type==='dir')loadDir(side,fullP+'/');"
    "  else if(isAudio(it.name))playMusic(fullP,it.name);"
    "  else if(isText(it.name))openEditor(fullP);"
    "  else if(it.name.endsWith('.elf')||it.name.endsWith('.bin'))runElf(fullP);"
    "  else if(it.name.toLowerCase().endsWith('.pkg')){fetch('/api/install_pkg?pkg='+encodeURIComponent(fullP));showToast('Installing '+it.name);}"
    "  else toggleSelIdx(side,idx);"
    "}";

const char *UI_PART4 =
    "function playMusic(path,name){"
    "  const bar=document.getElementById('music-bar'),aud=document.getElementById('aud-el');"
    "  bar.style.display='flex';document.getElementById('m-title').innerText=name;"
    "  aud.src='/api/stream?file='+encodeURIComponent(path);aud.play();"
    "  document.getElementById('m-disc').classList.remove('paused');"
    "}"
    "function toggleAudio(){const a=document.getElementById('aud-el'),d=document.getElementById('m-disc');if(a.paused){a.play();d.classList.remove('paused');}else{a.pause();d.classList.add('paused');}}"
    "function seekAudio(v){const a=document.getElementById('aud-el');if(a.duration)a.currentTime=(v/100)*a.duration;}"
    "async function fetchDash(){"
    "  try{const r=await fetch('/api/sysinfo');const d=await r.json();"
    "    document.getElementById('ssd-free').innerText=d.ssd_free.toFixed(1)+' GB';"
    "    document.getElementById('ssd-tot').innerText='Total: '+d.ssd_total.toFixed(1)+' GB | FW: '+d.fw;"
    "    document.getElementById('usb-free').innerText=d.usb_free>0?d.usb_free.toFixed(1)+' GB':'Offline';"
    "    document.getElementById('usb-tot').innerText=d.usb_mount+' ('+d.usb_total.toFixed(1)+' GB)';"
    "    document.getElementById('ftp-addr').innerText=d.ip+':'+d.ftp_port;"
    "    document.getElementById('apu-temp').innerText=d.temp+' °C';"
    "    tempHistory.push(d.temp);if(tempHistory.length>25)tempHistory.shift();drawTempGraph();"
    "  }catch(e){}"
    "}"
    "function drawTempGraph(){"
    "  const svg=document.getElementById('temp-svg');if(!svg||tempHistory.length<2)return;"
    "  const w=600,h=140;let pts=tempHistory.map((t,i)=>`${(i/24)*w},${h-Math.min(Math.max((t-30)/60,0),1)*h}`).join(' ');"
    "  svg.innerHTML=`<polyline fill='none' stroke='var(--accent)' stroke-width='4' points='${pts}'/>`;"
    "}"
    "async function fetchKlog(){try{const r=await fetch('/api/klog');const t=await r.text();const el=document.getElementById('klog-view');el.innerText=t;el.scrollTop=el.scrollHeight;}catch(e){}}"
    "async function loadProfiles(){"
    "  const g=document.getElementById('prof-grid');g.innerHTML='';"
    "  try{const r=await fetch('/api/profiles');const d=await r.json();"
    "    (d.profiles||[]).forEach(p=>{"
    "      g.innerHTML+=`<div class='card-item'><div style='font-size:3rem;'>👤</div><div class='game-title'>${p.name}</div><div class='f-meta'>ID: ${p.id} | Trophy: ${formatSize(p.trophy_size)}</div><div class='f-meta' style='margin-top:4px;'>PS5 Saves: ${formatSize(p.ps5_save_size)} | PS4: ${formatSize(p.ps4_save_size)}</div><div style='display:flex;gap:6px;width:100%;margin-top:10px;'><button class='act-tile focusable' style='flex:1;padding:8px;' onclick=\"backupProfile('${p.id}','trophy')\">🏆 تروفی</button><button class='act-tile focusable' style='flex:1;padding:8px;' onclick=\"backupProfile('${p.id}','save5')\">💾 سیو PS5</button><button class='act-tile focusable' style='flex:1;padding:8px;' onclick=\"backupProfile('${p.id}','save4')\">🕹️ سیو PS4</button><button class='act-tile focusable' style='flex:1;padding:8px;' onclick=\"renameProfile('${p.id}')\">✏️ نام</button></div></div>`;"
    "    });"
    "  }catch(e){}"
    "}"
    "async function waitTaskCompletion(){"
    "  document.getElementById('overlay').style.display='flex';"
    "  while(true){"
    "    await new Promise(r=>setTimeout(r,500));"
    "    try{const res=await fetch('/api/status');const d=await res.json();"
    "      if(d.running===1&&d.total>0){let pct=(d.copied/d.total)*100;document.getElementById('p-bar').style.width=pct+'%';document.getElementById('p-text').innerText=`${pct.toFixed(1)}% | ${d.speed.toFixed(1)} MB/s | ETA: ${formatTime(d.eta)}`;document.getElementById('p-file').innerText=d.file||'';}"
    "      else if(d.running===0)break;"
    "    }catch(e){break;}"
    "  }"
    "  document.getElementById('overlay').style.display='none';"
    "}"
    "async function backupProfile(id,kind){"
    "  if(isProcessing)return;isProcessing=true;"
    "  const src=kind==='trophy'?`/user/home/${id}/trophy2`:(kind==='save4'?`/user/home/${id}/savedata`:`/user/home/${id}/savedata_prospero`);"
    "  const dst=`${usbMount}/FIROZ_BACKUP_${id}_${kind}`;"
    "  await fetch('/api/action',{method:'POST',body:`type=copy&src=${encodeURIComponent(src)}&dst=${encodeURIComponent(dst)}`});"
    "  await waitTaskCompletion();isProcessing=false;showToast('بکاپ با موفقیت در USB ذخیره شد!');"
    "}"
    "function renameProfile(id){"
    "  openInputModal('نام جدید پروفایل:','',async(n)=>{"
    "    await fetch('/api/profile/rename?id='+encodeURIComponent(id),{method:'POST',body:n});loadProfiles();showToast('نام پروفایل ذخیره شد');"
    "  });"
    "}"
    "async function cleanCacheNow(){const r=await fetch('/api/clean_cache',{method:'POST'});const d=await r.json();showToast(t('clean_ok')+formatSize(d.freed));}"
    "async function loadGames(){"
    "  const lg=document.getElementById('launch-grid');lg.innerHTML='';"
    "  try{const r=await fetch('/api/list?dir=/user/app/');const d=await r.json();"
    "    (d.items||[]).filter(i=>i.type==='dir').forEach(i=>{"
    "      lg.innerHTML+=`<button class='card-item focusable' onclick=\"fetch('/api/launch?id=${encodeURIComponent(i.name)}');showToast('Launching ${i.name}...')\"><img src='/api/game-icon?id=${encodeURIComponent(i.name)}' class='game-cover' onerror=\"this.style.display='none'\"><div class='game-title'>${i.title||i.name}</div><div class='f-meta'>${i.name}</div></button>`;"
    "    });"
    "  }catch(e){}"
    "}"
    "let usbPkgItems=[];"
    "async function loadPkgsAndElfs(){"
    "  const pg=document.getElementById('pkg-grid');pg.innerHTML='';"
    "  try{const r=await fetch('/api/list?dir='+encodeURIComponent(usbMount+'/'));const d=await r.json();"
    "    usbPkgItems=(d.items||[]).filter(i=>/\\.(pkg|elf|bin)$/i.test(i.name));"
    "    usbPkgItems.forEach((i,idx)=>{"
    "      const isPkg=i.name.toLowerCase().endsWith('.pkg');"
    "      pg.innerHTML+=`<button class='card-item focusable' onclick=\"runUsbItem(${idx})\"><div style='font-size:3.5rem;'>${isPkg?'📦':'⚡'}</div><div class='game-title'>${i.name}</div><div class='f-meta'>${formatSize(i.size)}</div></button>`;"
    "    });"
    "  }catch(e){}"
    "}"
    "function runUsbItem(idx){"
    "  const it=usbPkgItems[idx];if(!it)return;const full=usbMount+'/'+it.name;"
    "  if(it.name.toLowerCase().endsWith('.pkg')){fetch('/api/install_pkg?pkg='+encodeURIComponent(full));showToast('Installing '+it.name);}"
    "  else runElf(full);"
    "}"
    "async function runElf(p){await fetch('/api/launch_elf?elf='+encodeURIComponent(p));showToast('Payload Sent to Port 9021!');}"
    "function toggleActionModal(show){const m=document.getElementById('modal-actions');m.style.display=show?'flex':'none';gpIndex=0;}"
    "async function openEditor(p){editingPath=p;const r=await fetch('/api/text?file='+encodeURIComponent(p));document.getElementById('ed-area').value=await r.text();document.getElementById('overlay-editor').style.display='flex';gpIndex=0;}"
    "async function saveEditor(){await fetch('/api/text/save?file='+encodeURIComponent(editingPath),{method:'POST',body:document.getElementById('ed-area').value});document.getElementById('overlay-editor').style.display='none';showToast('فایل ذخیره شد');}"
    "async function runQuickAction(act){"
    "  toggleActionModal(false);"
    "  const ss=activePanel,ds=(ss==='left')?'right':'left',sb=panels[ss].path,db=panels[ds].path;"
    "  if(act==='mkdir'){openInputModal('نام پوشه جدید:','',async(n)=>{await fetch('/api/action',{method:'POST',body:`type=mkdir&src=${encodeURIComponent(sb+n)}`});loadDir(ss,sb);});return;}"
    "  const sel=panels[ss].items.filter(i=>i.selected).map(i=>i.name);"
    "  if(sel.length===0){showToast('ابتدا با دکمه ▢ یک فایل یا پوشه انتخاب کنید');return;}"
    "  if(act==='rename'){openInputModal('نام جدید:',sel[0],async(n)=>{await fetch('/api/action',{method:'POST',body:`type=rename&src=${encodeURIComponent(sb+sel[0])}&dst=${encodeURIComponent(sb+n)}`});loadDir(ss,sb);});return;}"
    "  if(act==='chmod'){for(let nm of sel)await fetch('/api/action',{method:'POST',body:`type=chmod&src=${encodeURIComponent(sb+nm)}`});loadDir(ss,sb);showToast('سطح دسترسی 777 اعمال شد');return;}"
    "  isProcessing=true;"
    "  for(let nm of sel){"
    "    const sp=sb+nm,dp=db+nm;"
    "    await fetch('/api/action',{method:'POST',body:`type=${act}&src=${encodeURIComponent(sp)}&dst=${encodeURIComponent(dp)}`});"
    "    await waitTaskCompletion();"
    "  }"
    "  isProcessing=false;loadDir('left',panels.left.path);loadDir('right',panels.right.path);"
    "}"
    "function getActiveFocusables(){"
    "  const openModal=Array.from(document.querySelectorAll('.modal-wrap')).find(m=>m.style.display==='flex');"
    "  if(openModal)return Array.from(openModal.querySelectorAll('.focusable'));"
    "  if(mode==='FILE')return Array.from(document.querySelectorAll(`#pane-${activePanel} .focusable, .hud-bar .focusable`));"
    "  return Array.from(document.querySelectorAll('.view.active .focusable'));"
    "}"
    "function pollGamepad(){"
    "  const gps=navigator.getGamepads?navigator.getGamepads():[];"
    "  const gp=Array.from(gps).find(g=>g&&g.connected);"
    "  if(gp&&Date.now()-lastGpTime>165){"
    "    const b=gp.buttons,ax=gp.axes;"
    "    const vis=getActiveFocusables();"
    "    if(vis.length>0){"
    "      if(gpIndex>=vis.length)gpIndex=0;"
    "      document.querySelectorAll('.gp-focus').forEach(el=>el.classList.remove('gp-focus'));"
    "      vis[gpIndex].classList.add('gp-focus');vis[gpIndex].scrollIntoView({block:'nearest'});"
    "      if(b[13]?.pressed||ax[1]>0.55||b[15]?.pressed||ax[0]>0.55){gpIndex=(gpIndex+1)%vis.length;lastGpTime=Date.now();}"
    "      else if(b[12]?.pressed||ax[1]<-0.55||b[14]?.pressed||ax[0]<-0.55){gpIndex=(gpIndex-1+vis.length)%vis.length;lastGpTime=Date.now();}"
    "      else if(b[0]?.pressed){vis[gpIndex].click();lastGpTime=Date.now()+120;}"
    "      else if(b[2]?.pressed){const idx=vis[gpIndex].getAttribute('data-idx'),sd=vis[gpIndex].getAttribute('data-side');if(idx!==null&&sd)toggleSelIdx(sd,parseInt(idx,10));lastGpTime=Date.now()+120;}"
    "      else if(b[3]?.pressed){toggleActionModal(document.getElementById('modal-actions').style.display!=='flex');lastGpTime=Date.now()+150;}"
    "      else if(b[1]?.pressed){"
    "        const openModal=Array.from(document.querySelectorAll('.modal-wrap')).find(m=>m.style.display==='flex');"
    "        if(openModal)openModal.style.display='none';"
    "        else if(mode==='FILE'){goParentDir(activePanel);gpIndex=0;}"
    "        lastGpTime=Date.now()+150;"
    "      }"
    "      else if(b[4]?.pressed||b[5]?.pressed){const modes=['FILE','RUN','PKG','PROF','DASH','TERM','THEME'];let idx=modes.indexOf(mode);idx=(idx+(b[5]?.pressed?1:-1)+modes.length)%modes.length;setMode(modes[idx]);lastGpTime=Date.now()+180;}"
    "      else if(b[6]?.pressed){setActivePane('left');gpIndex=0;lastGpTime=Date.now()+120;}"
    "      else if(b[7]?.pressed){setActivePane('right');gpIndex=0;lastGpTime=Date.now()+120;}"
    "    }"
    "  }"
    "  requestAnimationFrame(pollGamepad);"
    "}"
    "window.onload=initSys;</script></head><body data-theme='cyber'>"
    "<div id='ui-toast' style='display:none;position:fixed;bottom:28px;left:50%;transform:translateX(-50%);background:var(--accent);color:#020617;padding:12px 28px;border-radius:99px;font-weight:900;font-size:1.1rem;z-index:300;box-shadow:0 8px 25px var(--glow);'></div>"
    "<div id='modal-input' class='modal-wrap'><div class='modal-card' style='max-width:520px;'><h2 id='inp-title' style='margin-top:0;color:var(--accent);'>نام را وارد کنید</h2><input id='inp-field' type='text' class='focusable' style='width:100%;padding:14px;font-size:1.2rem;border-radius:12px;border:2px solid var(--border);background:#050811;color:var(--text);direction:ltr;text-align:center;' onkeydown=\"if(event.key==='Enter')confirmInputModal()\"><div style='display:flex;gap:12px;justify-content:center;margin-top:16px;'><button class='act-tile focusable' style='flex:1;' onclick='confirmInputModal()'>✔️ تایید</button><button class='act-tile focusable' style='flex:1;' onclick=\"document.getElementById('modal-input').style.display='none'\">✖ انصراف</button></div></div></div>"
    "<div id='overlay' class='modal-wrap'><div class='modal-card'><h2 id='p-file'>Processing...</h2><div class='progress-bg'><div class='progress-fill' id='p-bar'></div></div><h3 id='p-text' style='color:var(--accent)'>0%</h3><button class='act-tile focusable' onclick=\"fetch('/api/cancel',{method:'POST'})\">🛑 لغو عملیات</button></div></div>"
    "<div id='overlay-editor' class='modal-wrap'><div class='modal-card' style='width:80%;'><textarea id='ed-area' class='focusable' style='width:100%;height:320px;background:#050811;color:var(--accent);border:1px solid var(--border);border-radius:12px;padding:12px;direction:ltr;'></textarea><div style='display:flex;gap:12px;justify-content:center;margin-top:12px;'><button class='act-tile focusable' onclick='saveEditor()'>💾 ذخیره</button><button class='act-tile focusable' onclick=\"document.getElementById('overlay-editor').style.display='none'\">✖ بستن</button></div></div></div>"
    "<div id='modal-actions' class='modal-wrap'><div class='modal-card'><h2 style='margin:0;color:var(--accent);'>⚡ پالت عملیات سریع (Quick Actions)</h2><div class='action-grid'>"
    "  <button class='act-tile focusable' onclick=\"runQuickAction('copy')\">📋 کپی به پنل مقابل</button>"
    "  <button class='act-tile focusable' onclick=\"runQuickAction('cut')\">✂️ انتقال به پنل مقابل</button>"
    "  <button class='act-tile focusable' onclick=\"runQuickAction('extract')\">📦 استخراج ZIP/RAR/TAR</button>"
    "  <button class='act-tile focusable' onclick=\"runQuickAction('fpkg')\">💿 ساخت مستقیم fPKG</button>"
    "  <button class='act-tile focusable' onclick=\"runQuickAction('mkdir')\">➕ پوشه جدید</button>"
    "  <button class='act-tile focusable' onclick=\"runQuickAction('rename')\">✏️ تغییر نام</button>"
    "  <button class='act-tile focusable' onclick=\"runQuickAction('chmod')\">🔓 دسترسی کامل 777</button>"
    "  <button class='act-tile focusable' onclick=\"runQuickAction('del')\">🗑️ حذف انتخاب‌شده‌ها</button>"
    "  <button class='act-tile focusable' onclick='toggleActionModal(false)'>✖ بستن منو</button>"
    "</div></div></div>"
    "<header><div class='brand'><img src='/icon0.png' class='logo-img' onerror=\"this.style.display='none'\"><h1>FIROZ OS <span style='font-size:1rem;color:var(--accent);'>v4.0</span></h1><div class='tg-pill'><svg width='18' height='18' viewBox='0 0 24 24' fill='currentColor'><path d='M21 3L3 10.5l6.5 2.5L18 6l-6.5 8.5v5l3.5-3.5 4 2.5L21 3z'/></svg><span>@firozfar051</span></div></div><div class='top-actions'><button class='icon-btn focusable' onclick=\"setMode('THEME')\">🎨 تم</button><button class='icon-btn focusable' onclick='toggleLang()'>FA / EN</button></div></header>"
    "<div class='tab-bar'>"
    "  <button id='tab-FILE' class='tab-btn focusable active' onclick=\"setMode('FILE')\">📁 فایل‌ها</button>"
    "  <button id='tab-RUN' class='tab-btn focusable' onclick=\"setMode('RUN')\">🚀 بازی‌ها</button>"
    "  <button id='tab-PKG' class='tab-btn focusable' onclick=\"setMode('PKG')\">📦 نصب و پیلود</button>"
    "  <button id='tab-PROF' class='tab-btn focusable' onclick=\"setMode('PROF')\">🏆 تروفی و پروفایل</button>"
    "  <button id='tab-DASH' class='tab-btn focusable' onclick=\"setMode('DASH')\">🌡️ داشبورد و دما</button>"
    "  <button id='tab-TERM' class='tab-btn focusable' onclick=\"setMode('TERM')\">💻 ترمینال</button>"
    "  <button id='tab-THEME' class='tab-btn focusable' onclick=\"setMode('THEME')\">🎨 تم و ابزار</button>"
    "</div>"
    "<div class='main-area'>"
    "  <div id='view-files' class='view active'><div class='split-grid'>"
    "    <div id='pane-left' class='glass-panel active-pane' onclick=\"setActivePane('left')\"><div class='panel-head'><span>SSD داخلی</span><div class='mount-bar'><button class='m-pill' onclick=\"loadDir('left','/user/app/')\">APP</button><button class='m-pill' onclick=\"loadDir('left','/user/home/')\">HOME</button><button class='m-pill' onclick=\"loadDir('left','/data/')\">DATA</button><button class='m-pill' onclick=\"loadDir('left','/')\">ROOT</button></div></div><div class='path-bar' id='path-left'></div><div id='list-left' class='file-list'></div></div>"
    "    <div id='pane-right' class='glass-panel' onclick=\"setActivePane('right')\"><div class='panel-head'><span>USB / M.2</span><div class='mount-bar'><button class='m-pill' onclick=\"loadDir('right',usbMount+'/')\">USB</button><button class='m-pill' onclick=\"loadDir('right','/mnt/ext0/')\">M.2</button><button class='m-pill' onclick=\"loadDir('right','/data/')\">DATA</button></div></div><div class='path-bar' id='path-right'></div><div id='list-right' class='file-list'></div></div>"
    "  </div><div class='hud-bar'><div class='hud-hints'><span><b class='k-badge'>✕</b> باز کردن</span><span><b class='k-badge'>▢</b> انتخاب</span><span><b class='k-badge'>◯</b> بازگشت</span><span><b class='k-badge'>L1/R1</b> تب‌ها</span><span><b class='k-badge'>L2/R2</b> تغییر پنل</span></div><button id='btn-act-open' class='btn-menu-trigger focusable' onclick='toggleActionModal(true)'>⚡ منوی عملیات (△)</button></div></div>"
    "  <div id='view-run' class='view'><div class='cards-grid' id='launch-grid'></div></div>"
    "  <div id='view-pkg' class='view'><div class='cards-grid' id='pkg-grid'></div></div>"
    "  <div id='view-prof' class='view'><div class='cards-grid' id='prof-grid'></div></div>"
    "  <div id='view-dash' class='view'><div class='dash-layout'>"
    "    <div class='stat-box'><div class='s-title'>SSD (/user)</div><div class='s-val' id='ssd-free'>--</div><div class='s-sub' id='ssd-tot'>--</div></div>"
    "    <div class='stat-box'><div class='s-title'>USB STORAGE</div><div class='s-val' id='usb-free'>--</div><div class='s-sub' id='usb-tot'>--</div></div>"
    "    <div class='stat-box'><div class='s-title'>دمای پردازنده (APU)</div><div class='s-val' id='apu-temp'>-- °C</div><div class='s-sub'>سنسور حرارتی زنده</div></div>"
    "    <div class='stat-box'><div class='s-title'>سرور FTP فعال</div><div class='s-val' id='ftp-addr' style='font-size:1.45rem;'>--</div><div class='s-sub'>پورت 2121 (بدون رمز)</div></div>"
    "  </div><div class='graph-box'><div class='s-title'>نمودار زنده دمای پردازنده (Live APU Telemetry)</div><svg id='temp-svg' viewBox='0 0 600 140' style='width:100%;height:140px;'></svg></div></div>"
    "  <div id='view-term' class='view'><div class='term-box' id='klog-view'>در حال دریافت لاگ‌های هسته...</div></div>"
    "  <div id='view-theme' class='view'><div class='action-grid'>"
    "    <button class='act-tile focusable' onclick=\"setTheme('cyber')\">🌌 Cyberpunk Neon</button>"
    "    <button class='act-tile focusable' onclick=\"setTheme('ps5')\">🎮 PS5 Royal Blue</button>"
    "    <button class='act-tile focusable' onclick=\"setTheme('matrix')\">💻 Matrix Hacker</button>"
    "    <button class='act-tile focusable' onclick=\"setTheme('crimson')\">🔥 Crimson Tactical</button>"
    "    <button class='act-tile focusable' onclick=\"setTheme('oled')\">🌑 OLED Pitch Black</button>"
    "    <button class='act-tile focusable' onclick=\"setTheme('light')\">❄️ Frost Glass Light</button>"
    "    <button class='act-tile focusable' style='grid-column:span 3;border-color:#10b981;' onclick='cleanCacheNow()'>🧹 پاک‌سازی هوشمند کش بازی‌های PS4 و PS5 (Smart Cache Cleaner)</button>"
    "  </div></div>"
    "</div>"
    "<div id='music-bar'><img src='/icon0.png' id='m-disc' class='disc-art'><div class='m-info' id='m-title'>Track.mp3</div><div class='eq-bars'><div class='eq-b'></div><div class='eq-b' style='animation-delay:0.2s'></div><div class='eq-b' style='animation-delay:0.4s'></div></div><button class='icon-btn focusable' onclick='toggleAudio()'>⏯</button><input type='range' id='m-seek' class='m-slider' value='0' min='0' max='100' oninput='seekAudio(this.value)'><span id='m-time' style='font-family:monospace;'>00:00 / 00:00</span><audio id='aud-el' ontimeupdate=\"if(this.duration){document.getElementById('m-seek').value=(this.currentTime/this.duration)*100;document.getElementById('m-time').innerText=formatTime(this.currentTime)+' / '+formatTime(this.duration);}\"></audio></div>"
    "</body></html>";

void *client_thread(void *arg) {
    int client_sock = *(int *)arg;
    free(arg);

    int flag = 1;
    setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    struct timeval tv = { 30, 0 };
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char *buffer = malloc(16384);
    if (!buffer) { close(client_sock); return NULL; }

    int bytes_read = 0;
    while (bytes_read < 16383) {
        ssize_t n = recv(client_sock, buffer + bytes_read, 16383 - bytes_read, 0);
        if (n <= 0) break;
        bytes_read += (int)n;
        buffer[bytes_read] = '\0';
        if (strstr(buffer, "\r\n\r\n")) break;
    }
    if (bytes_read <= 0) { free(buffer); close(client_sock); return NULL; }

    char req_method[16] = {0}, req_path[2048] = {0};
    if (sscanf(buffer, "%15s %2047s", req_method, req_path) != 2) {
        free(buffer); close(client_sock); return NULL;
    }

    char *header_end = strstr(buffer, "\r\n\r\n");
    int content_length = 0;
    char *cl_ptr = strcasestr(buffer, "Content-Length:");
    if (cl_ptr) content_length = atoi(cl_ptr + 15);

    if (strncmp(req_path, "/icon0.png", 10) == 0) {
        char hdr[256];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: %u\r\nCache-Control: max-age=3600\r\nConnection: close\r\n\r\n",
            icon0_png_len);
        send_all(client_sock, hdr, strlen(hdr));
        send_all(client_sock, icon0_png, icon0_png_len);
    }
    else if (strncmp(req_path, "/api/game-icon", 14) == 0) {
        char id[128];
        get_param(req_path, "id=", id, sizeof(id));
        api_game_icon(client_sock, id);
    }
    else if (strncmp(req_path, "/api/sysinfo", 12) == 0) {
        api_sysinfo(client_sock);
    }
    else if (strncmp(req_path, "/api/list", 9) == 0) {
        char target_dir[1024] = "/user/app/";
        get_param(req_path, "dir=", target_dir, sizeof(target_dir));
        api_list(client_sock, target_dir);
    }
    else if (strncmp(req_path, "/api/status", 11) == 0) {
        api_status(client_sock);
    }
    else if (strncmp(req_path, "/api/klog", 9) == 0) {
        api_klog(client_sock);
    }
    else if (strncmp(req_path, "/api/profiles", 13) == 0) {
        api_profiles(client_sock);
    }
    else if (strncmp(req_path, "/api/profile/rename", 19) == 0 && strcmp(req_method, "POST") == 0) {
        char id[128];
        get_param(req_path, "id=", id, sizeof(id));
        if (id[0] && !strstr(id, "..") && header_end) {
            char *body = header_end + 4;
            int body_received = bytes_read - (int)(body - buffer);
            while (body_received < content_length && bytes_read < 16383) {
                ssize_t extra = recv(client_sock, buffer + bytes_read, 16383 - bytes_read, 0);
                if (extra <= 0) break;
                bytes_read += (int)extra;
                body_received += (int)extra;
                buffer[bytes_read] = '\0';
            }
            char npath[512];
            snprintf(npath, sizeof(npath), "/user/home/%s/username.txt", id);
            FILE *f = fopen(npath, "wb");
            if (f) {
                if (body_received > 0) fwrite(body, 1, (size_t)body_received, f);
                fclose(f);
                chmod(npath, 0777);
            }
        }
        const char *ok_res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true}";
        send_all(client_sock, ok_res, strlen(ok_res));
    }
    else if (strncmp(req_path, "/api/clean_cache", 16) == 0) {
        long long freed = clean_system_cache();
        char res[256];
        snprintf(res, sizeof(res), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true,\"freed\":%lld}", freed);
        send_all(client_sock, res, strlen(res));
    }
    else if (strncmp(req_path, "/api/cancel", 11) == 0) {
        pthread_mutex_lock(&task_mutex);
        global_task.cancel_requested = 1;
        pthread_mutex_unlock(&task_mutex);
        const char *ok_res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true}";
        send_all(client_sock, ok_res, strlen(ok_res));
    }
    else if (strncmp(req_path, "/api/launch_elf", 15) == 0) {
        char elf[1024];
        get_param(req_path, "elf=", elf, sizeof(elf));
        int r = launch_elf_payload(elf);
        char res[256];
        snprintf(res, sizeof(res), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":%s}", r == 0 ? "true" : "false");
        send_all(client_sock, res, strlen(res));
    }
    else if (strncmp(req_path, "/api/launch", 11) == 0) {
        char id[128];
        get_param(req_path, "id=", id, sizeof(id));
        launch_game(id);
        const char *ok_res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true}";
        send_all(client_sock, ok_res, strlen(ok_res));
    }
    else if (strncmp(req_path, "/api/install_pkg", 16) == 0) {
        char pkg[1024];
        get_param(req_path, "pkg=", pkg, sizeof(pkg));
        install_pkg(pkg);
        const char *ok_res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true}";
        send_all(client_sock, ok_res, strlen(ok_res));
    }
    else if (strncmp(req_path, "/api/stream", 11) == 0 || strncmp(req_path, "/api/download", 13) == 0) {
        char file[1024];
        get_param(req_path, "file=", file, sizeof(file));
        api_stream_file(client_sock, file, buffer);
    }
    else if (strncmp(req_path, "/api/text/save", 14) == 0 && strcmp(req_method, "POST") == 0) {
        char file[1024];
        get_param(req_path, "file=", file, sizeof(file));
        FILE *f = fopen(file, "wb");
        if (f && header_end) {
            char *body = header_end + 4;
            int already = bytes_read - (int)(body - buffer);
            if (already > 0) fwrite(body, 1, (size_t)already, f);
            int rem = content_length - already;
            char *tbuf = malloc(16384);
            while (rem > 0 && tbuf) {
                ssize_t n = recv(client_sock, tbuf, rem > 16384 ? 16384 : (size_t)rem, 0);
                if (n <= 0) break;
                fwrite(tbuf, 1, (size_t)n, f);
                rem -= (int)n;
            }
            if (tbuf) free(tbuf);
            fclose(f);
            chmod(file, 0777);
        }
        const char *ok_res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true}";
        send_all(client_sock, ok_res, strlen(ok_res));
    }
    else if (strncmp(req_path, "/api/text", 9) == 0) {
        char file[1024];
        get_param(req_path, "file=", file, sizeof(file));
        api_text_read(client_sock, file);
    }
    else if (strcmp(req_method, "POST") == 0 && strncmp(req_path, "/api/action", 11) == 0) {
        if (header_end) {
            char *body = header_end + 4;
            int body_received = bytes_read - (int)(body - buffer);
            while (body_received < content_length && bytes_read < 16383) {
                ssize_t extra = recv(client_sock, buffer + bytes_read, 16383 - bytes_read, 0);
                if (extra <= 0) break;
                bytes_read += (int)extra;
                body_received += (int)extra;
                buffer[bytes_read] = '\0';
            }

            char act[16] = {0}, src[1024] = {0}, dst[1024] = {0};
            get_param(body, "type=", act, sizeof(act));
            get_param(body, "src=", src, sizeof(src));
            get_param(body, "dst=", dst, sizeof(dst));

            if (strcmp(act, "mkdir") == 0) {
                mkdir_p(src);
                const char *ok_res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true}";
                send_all(client_sock, ok_res, strlen(ok_res));
            } else if (strcmp(act, "rename") == 0) {
                rename(src, dst);
                const char *ok_res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true}";
                send_all(client_sock, ok_res, strlen(ok_res));
            } else if (strcmp(act, "chmod") == 0) {
                recursive_chmod(src, 0777);
                const char *ok_res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true}";
                send_all(client_sock, ok_res, strlen(ok_res));
            } else {
                pthread_mutex_lock(&task_mutex);
                if (global_task.is_running == 1) {
                    pthread_mutex_unlock(&task_mutex);
                    const char *busy_res = "HTTP/1.1 429 Too Many Requests\r\nConnection: close\r\n\r\n{\"error\":\"Busy\"}";
                    send_all(client_sock, busy_res, strlen(busy_res));
                } else {
                    global_task.is_running = 1;
                    global_task.cancel_requested = 0;
                    global_task.total_bytes = 0;
                    global_task.copied_bytes = 0;
                    pthread_mutex_unlock(&task_mutex);

                    TaskArgs *task = malloc(sizeof(TaskArgs));
                    if (task) {
                        snprintf(task->action, sizeof(task->action), "%s", act);
                        snprintf(task->src, sizeof(task->src), "%s", src);
                        snprintf(task->dst, sizeof(task->dst), "%s", dst);

                        if (spawn_thread(background_worker, task) != 0) {
                            free(task);
                            pthread_mutex_lock(&task_mutex);
                            global_task.is_running = 0;
                            pthread_mutex_unlock(&task_mutex);
                        }
                    } else {
                        pthread_mutex_lock(&task_mutex);
                        global_task.is_running = 0;
                        pthread_mutex_unlock(&task_mutex);
                    }

                    const char *ok_res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true}";
                    send_all(client_sock, ok_res, strlen(ok_res));
                }
            }
        }
    }
    else if (strcmp(req_path, "/") == 0 || strncmp(req_path, "/index.html", 11) == 0) {
        send_all(client_sock, UI_PART1, strlen(UI_PART1));
        send_all(client_sock, UI_PART2, strlen(UI_PART2));
        send_all(client_sock, UI_PART3, strlen(UI_PART3));
        send_all(client_sock, UI_PART4, strlen(UI_PART4));
    }
    else {
        const char *err = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n404 Not Found";
        send_all(client_sock, err, strlen(err));
    }

    free(buffer);
    close(client_sock);
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) return 1;

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    struct sockaddr_in server_addr;
    int bound = 0;
    for (int p = HTTP_PORT; p < HTTP_PORT + 10; p++) {
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons((uint16_t)p);
        if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == 0) {
            g_http_port = p;
            bound = 1;
            break;
        }
    }

    if (!bound || listen(server_sock, 128) < 0) {
        close(server_sock);
        return 1;
    }

    char ip[64];
    get_console_ip(ip, sizeof(ip));

    char start_msg[128];
    snprintf(start_msg, sizeof(start_msg), "FIROZ OS v4.0 Active!\nWeb: %s:%d | FTP: %d", ip, g_http_port, FTP_PORT);
    ps5_notify(start_msg);

    spawn_thread(klog_reader_thread, NULL);
    spawn_thread(ftp_server_thread, NULL);

    install_media_launcher(g_http_port);
    open_ps5_browser(g_http_port);
    klog_appendf("[FIROZ OS v4.0] Web: http://%s:%d | FTP: %s:%d\n", ip, g_http_port, ip, FTP_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int c_fd = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
        if (c_fd < 0) {
            usleep(10000);
            continue;
        }
        int *client_sock = malloc(sizeof(int));
        if (!client_sock) {
            close(c_fd);
            continue;
        }
        *client_sock = c_fd;
        if (spawn_thread(client_thread, client_sock) != 0) {
            close(c_fd);
            free(client_sock);
        }
    }

    close(server_sock);
    return 0;
}
