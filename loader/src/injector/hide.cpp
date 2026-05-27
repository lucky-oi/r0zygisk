#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <android/dlext.h>
#include <jni.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <lsplt.hpp>

#include "hide.h"
#include "logging.h"
#include "zygisk.hpp"

namespace hide {

static std::vector<std::string> g_filters;
static std::set<int> g_sensitive_fds;
static std::set<int> g_prop_fds;
static std::mutex g_maps_mutex;
static bool g_maps_hide_active = false;

static int (*orig_openat)(int, const char *, int, ...) = nullptr;
static ssize_t (*orig_read)(int, void *, size_t) = nullptr;
static int (*orig_close)(int) = nullptr;
static int (*orig_faccessat)(int, const char *, int, ...) = nullptr;
static int (*orig_connect)(int, const struct sockaddr *, socklen_t) = nullptr;

static bool is_adb_path(const char *path) {
    if (!path) return false;
    return strstr(path, "/dev/socket/adbd") != nullptr ||
           strstr(path, "/dev/usb-ffs/adb") != nullptr;
}

static bool is_adb_socket_addr(const struct sockaddr *addr, socklen_t len) {
    if (!addr || len < sizeof(sa_family_t) || addr->sa_family != AF_UNIX) return false;
    auto *un = reinterpret_cast<const struct sockaddr_un *>(addr);
    size_t max_len = len > offsetof(sockaddr_un, sun_path) ? len - offsetof(sockaddr_un, sun_path) : 0;
    if (max_len == 0) return false;

    if (un->sun_path[0] == '\0') {
        std::string_view name(un->sun_path + 1, strnlen(un->sun_path + 1, max_len - 1));
        return name == "jdwp-control";
    }

    std::string_view path(un->sun_path, strnlen(un->sun_path, max_len));
    return path == "/dev/socket/adbd" || path.find("/dev/socket/adbd") != std::string_view::npos;
}

static bool is_prop_file_path(const char *path) {
    if (!path) return false;
    return strstr(path, "/dev/__properties__/") != nullptr;
}

bool init() {
    return true;
}

bool hide_handle(void *handle) {
    (void) handle;
    return false;
}

void restore_handle(void *handle) {
    (void) handle;
}

void add_maps_filter(const char *pattern) {
    std::lock_guard<std::mutex> lock(g_maps_mutex);
    for (const auto &filter : g_filters) {
        if (filter == pattern) return;
    }
    g_filters.emplace_back(pattern);
}

void set_maps_hide_active(bool active) {
    g_maps_hide_active = active;
}

static bool should_hide_line(std::string_view line) {
    for (auto &f : g_filters) {
        if (line.find(f) != std::string_view::npos) {
            return true;
        }
    }
    // Hide ADB port 5555 (0x15B3) from /proc/net/tcp
    if (line.find(":15B3") != std::string_view::npos ||
        line.find(":15b3") != std::string_view::npos) {
        return true;
    }
    // Hide JDWP and adbd sockets from /proc/net/unix
    if (line.find("@jdwp-control") != std::string_view::npos ||
        line.find("dev/socket/adbd") != std::string_view::npos) {
        return true;
    }
    return false;
}

static bool is_sensitive_path(const char *path) {
    if (!path) return false;
    std::string_view p(path);
    if (p == "/proc/self/maps") return true;
    if (p.find("/proc/") == 0 && p.find("/maps") != std::string_view::npos) return true;
    if (p == "/proc/self/mountinfo") return true;
    if (p.find("/proc/") == 0 && p.find("/mountinfo") != std::string_view::npos) return true;
    if (p == "/proc/self/mounts") return true;
    if (p.find("/proc/") == 0 && p.find("/mounts") != std::string_view::npos) return true;
    if (p == "/proc/net/tcp" || p == "/proc/net/tcp6") return true;
    if (p.find("/proc/") == 0 && p.find("/net/tcp") != std::string_view::npos) return true;
    if (p == "/proc/net/unix" || p == "/proc/self/net/unix") return true;
    if (p.find("/proc/") == 0 && p.find("/net/unix") != std::string_view::npos) return true;
    return false;
}

static bool check_fd_is_sensitive(int fd) {
    char link[PATH_MAX];
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    ssize_t len = readlink(path, link, sizeof(link) - 1);
    if (len <= 0) return false;
    link[len] = '\0';
    return is_sensitive_path(link);
}

static int hooked_openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    va_list ap;
    va_start(ap, flags);
    if (flags & (O_CREAT | O_TMPFILE)) mode = va_arg(ap, int);
    va_end(ap);

    if (g_maps_hide_active && is_adb_path(path)) {
        errno = ENOENT;
        return -1;
    }

    int fd = orig_openat(dirfd, path, flags, mode);
    if (fd >= 0 && is_sensitive_path(path)) {
        std::lock_guard<std::mutex> lock(g_maps_mutex);
        g_sensitive_fds.insert(fd);
    }
    if (fd >= 0 && is_prop_file_path(path)) {
        LOGI("hide: openat prop fd=%d path=%s", fd, path);
        std::lock_guard<std::mutex> lock(g_maps_mutex);
        g_prop_fds.insert(fd);
    }
    return fd;
}

static int hooked_close(int fd) {
    if (fd >= 0) {
        std::lock_guard<std::mutex> lock(g_maps_mutex);
        g_sensitive_fds.erase(fd);
        g_prop_fds.erase(fd);
    }
    return orig_close(fd);
}

static int hooked_faccessat(int dirfd, const char *pathname, int mode, ...) {
    int flags = 0;
    va_list ap;
    va_start(ap, mode);
    flags = va_arg(ap, int);
    va_end(ap);

    if (g_maps_hide_active && is_adb_path(pathname)) {
        errno = ENOENT;
        return -1;
    }
    return orig_faccessat(dirfd, pathname, mode, flags);
}

static int hooked_connect(int sockfd, const struct sockaddr *addr, socklen_t len) {
    if (g_maps_hide_active && is_adb_socket_addr(addr, len)) {
        errno = ENOENT;
        LOGI("hide: block adb/jdwp connect");
        return -1;
    }
    return orig_connect(sockfd, addr, len);
}

static ssize_t hooked_read(int fd, void *buf, size_t count) {
    ssize_t n = orig_read(fd, buf, count);
    if (n <= 0) return n;
    if (!g_maps_hide_active) return n;

    // Check if this is a property file FD — apply binary patching
    {
        std::lock_guard<std::mutex> lock(g_maps_mutex);
        if (g_prop_fds.count(fd) > 0) {
            patch_prop_data(static_cast<char *>(buf), n);
            LOGI("hide: read patch %zd bytes prop fd=%d", n, fd);
            return n;
        }
    }

    // Check if this is a maps/sensitive FD — apply line filtering
    bool is_maps;
    {
        std::lock_guard<std::mutex> lock(g_maps_mutex);
        is_maps = g_sensitive_fds.count(fd) > 0;
    }

    if (!is_maps && check_fd_is_sensitive(fd)) {
        std::lock_guard<std::mutex> lock(g_maps_mutex);
        g_sensitive_fds.insert(fd);
        is_maps = true;
    }

    if (!is_maps) return n;

    auto *data = static_cast<char *>(buf);
    size_t wp = 0;
    size_t rp = 0;
    while (rp < static_cast<size_t>(n)) {
        size_t eol = rp;
        while (eol < static_cast<size_t>(n) && data[eol] != '\n') eol++;

        std::string_view line(data + rp, eol - rp);

        std::lock_guard<std::mutex> lock(g_maps_mutex);
        if (!should_hide_line(line)) {
            size_t len = eol - rp;
            if (eol < static_cast<size_t>(n)) len++;
            memmove(data + wp, data + rp, len);
            wp += len;
        }
        rp = (eol < static_cast<size_t>(n)) ? eol + 1 : eol;
    }

    return static_cast<ssize_t>(wp);
}

bool setup_maps_hide() {
    dev_t libc_dev = 0;
    ino_t libc_inode = 0;
    for (auto &map : lsplt::MapInfo::Scan()) {
        if (map.path.ends_with("/libc.so")) {
            libc_dev = map.dev;
            libc_inode = map.inode;
            break;
        }
    }
    if (!libc_inode) {
        LOGW("hide: libc.so not found, maps filter disabled");
        return false;
    }

    bool ok = true;
    if (!lsplt::RegisterHook(libc_dev, libc_inode, "openat",
                             reinterpret_cast<void *>(hooked_openat),
                             reinterpret_cast<void **>(&orig_openat))) {
        LOGW("hide: failed to register openat hook");
        ok = false;
    }
    if (!lsplt::RegisterHook(libc_dev, libc_inode, "read",
                             reinterpret_cast<void *>(hooked_read),
                             reinterpret_cast<void **>(&orig_read))) {
        LOGW("hide: failed to register read hook");
        ok = false;
    }
    if (!lsplt::RegisterHook(libc_dev, libc_inode, "close",
                             reinterpret_cast<void *>(hooked_close),
                             reinterpret_cast<void **>(&orig_close))) {
        LOGW("hide: failed to register close hook");
        ok = false;
    }
    if (!lsplt::RegisterHook(libc_dev, libc_inode, "faccessat",
                             reinterpret_cast<void *>(hooked_faccessat),
                             reinterpret_cast<void **>(&orig_faccessat))) {
        LOGW("hide: failed to register faccessat hook");
        ok = false;
    }
    if (!lsplt::RegisterHook(libc_dev, libc_inode, "connect",
                             reinterpret_cast<void *>(hooked_connect),
                             reinterpret_cast<void **>(&orig_connect))) {
        LOGW("hide: failed to register connect hook");
        ok = false;
    }

    if (ok) {
        LOGI("hide: maps filter hooks registered");
    }
    return ok;
}

void cleanup() {
    std::lock_guard<std::mutex> lock(g_maps_mutex);
    g_sensitive_fds.clear();
    g_prop_fds.clear();
}

} // namespace hide

namespace {

static bool hide_active = false;

using prop_get_fn = int (*)(const char*, char*);
using prop_read_fn = int (*)(const prop_info*, char*, char*);
using prop_read_cb_fn = void (*)(const prop_info*, void(*)(void*, const char*, const char*, uint32_t), void*);
using property_get_fn = int (*)(const char*, char*, const char*);
using jni_get1_fn = jstring (*)(JNIEnv*, jclass, jstring);
using jni_get2_fn = jstring (*)(JNIEnv*, jclass, jstring, jstring);
using jni_get_int_fn = jint (*)(JNIEnv*, jclass, jstring, jint);
using jni_get_long_fn = jlong (*)(JNIEnv*, jclass, jstring, jlong);
using jni_get_bool_fn = jboolean (*)(JNIEnv*, jclass, jstring, jboolean);
using jni_find_fn = jlong (*)(JNIEnv*, jclass, jstring);
using jni_hget1_fn = jstring (*)(JNIEnv*, jclass, jlong);
using jni_hget2_fn = jstring (*)(JNIEnv*, jclass, jlong, jstring);
using jni_set_fn = void (*)(JNIEnv*, jclass, jstring, jstring);
using jni_set_key_fn = void (*)(JNIEnv*, jclass, jstring, jstring, jstring);

static prop_get_fn orig_system_property_get = nullptr;
static prop_read_fn orig_system_property_read = nullptr;
static prop_read_cb_fn orig_system_property_read_callback = nullptr;
static property_get_fn orig_property_get = nullptr;
static jni_get1_fn orig_jni_native_get1 = nullptr;
static jni_get2_fn orig_jni_native_get2 = nullptr;
static jni_get_int_fn orig_jni_native_get_int = nullptr;
static jni_get_long_fn orig_jni_native_get_long = nullptr;
static jni_get_bool_fn orig_jni_native_get_bool = nullptr;
static jni_find_fn orig_jni_native_find = nullptr;
static jni_hget1_fn orig_jni_native_hget1 = nullptr;
static jni_hget2_fn orig_jni_native_hget2 = nullptr;
static jni_set_fn orig_jni_native_set = nullptr;
static jni_set_key_fn orig_jni_native_set_key = nullptr;
static std::mutex jni_prop_mutex;
static std::unordered_map<jlong, std::string> jni_prop_handles;

struct prop_override {
    const char* name;
    const char* safe_value;
};

static const prop_override overrides[] = {
    {"ro.debuggable", "0"},
    {"ro.secure", "1"},
    {"ro.adb.secure", "1"},
    {"ro.build.type", "user"},
    {"ro.build.tags", "release-keys"},
    {"ro.kernel.qemu", "0"},
    {"init.svc.adbd", "stopped"},
    {"sys.usb.config", "mtp"},
    {"sys.usb.state", "mtp"},
    {"persist.sys.usb.config", "mtp"},
};

static const char* find_override(const char* name) {
    for (auto& o : overrides) {
        if (strcmp(name, o.name) == 0) return o.safe_value;
    }
    if (strstr(name, "magisk") || strstr(name, "ksu") || strstr(name, "zygisk") ||
        strstr(name, "apatch") || strstr(name, "supersu")) {
        return "";
    }
    if (strstr(name, "adb")) {
        return "";
    }
    return nullptr;
}

static void strip_adb_from_value(char *value) {
    if (!value || !*value) return;
    char *p = strstr(value, "adb");
    if (!p) return;

    char *start = p;
    char *end = p + 3;
    if (start > value && *(start - 1) == ',') {
        start--;
    } else if (*end == ',') {
        end++;
    }
    memmove(start, end, strlen(end) + 1);

    if (!*value) {
        strcpy(value, "mtp");
    }
    strip_adb_from_value(value);
}

static bool should_strip_adb_value(const char *name) {
    return strstr(name, "usb.config") != nullptr ||
           strstr(name, "usb_config") != nullptr ||
           strstr(name, "usb.state") != nullptr;
}

static bool safe_prop_for_name(const char *name, std::string *out) {
    if (!hide_active || !name || !out) return false;
    if (const char *safe = find_override(name)) {
        *out = safe;
        return true;
    }
    if (should_strip_adb_value(name)) {
        char value[PROP_VALUE_MAX] = {};
        if (orig_system_property_get && orig_system_property_get(name, value) > 0) {
            strip_adb_from_value(value);
            *out = value;
            return true;
        }
    }
    return false;
}

static bool safe_prop_for_jstring(JNIEnv *env, jstring name, std::string *out) {
    if (!env || !name) return false;
    const char *chars = env->GetStringUTFChars(name, nullptr);
    if (!chars) return false;
    bool hit = safe_prop_for_name(chars, out);
    env->ReleaseStringUTFChars(name, chars);
    return hit;
}

static jstring new_jni_native_get1(JNIEnv *env, jclass clazz, jstring name) {
    std::string safe;
    if (safe_prop_for_jstring(env, name, &safe)) {
        LOGI("java prop override: %s", safe.c_str());
        return env->NewStringUTF(safe.c_str());
    }
    return orig_jni_native_get1 ? orig_jni_native_get1(env, clazz, name) : env->NewStringUTF("");
}

static jstring new_jni_native_get2(JNIEnv *env, jclass clazz, jstring name, jstring def) {
    std::string safe;
    if (safe_prop_for_jstring(env, name, &safe)) {
        LOGI("java prop override: %s", safe.c_str());
        return env->NewStringUTF(safe.c_str());
    }
    return orig_jni_native_get2 ? orig_jni_native_get2(env, clazz, name, def) : def;
}

static jint new_jni_native_get_int(JNIEnv *env, jclass clazz, jstring name, jint def) {
    std::string safe;
    if (safe_prop_for_jstring(env, name, &safe)) {
        char *end = nullptr;
        long v = strtol(safe.c_str(), &end, 10);
        return end && *end == '\0' ? static_cast<jint>(v) : def;
    }
    return orig_jni_native_get_int ? orig_jni_native_get_int(env, clazz, name, def) : def;
}

static jlong new_jni_native_get_long(JNIEnv *env, jclass clazz, jstring name, jlong def) {
    std::string safe;
    if (safe_prop_for_jstring(env, name, &safe)) {
        char *end = nullptr;
        long long v = strtoll(safe.c_str(), &end, 10);
        return end && *end == '\0' ? static_cast<jlong>(v) : def;
    }
    return orig_jni_native_get_long ? orig_jni_native_get_long(env, clazz, name, def) : def;
}

static jboolean new_jni_native_get_bool(JNIEnv *env, jclass clazz, jstring name, jboolean def) {
    std::string safe;
    if (safe_prop_for_jstring(env, name, &safe)) {
        if (safe == "1" || safe == "true" || safe == "y" || safe == "yes" || safe == "on") return JNI_TRUE;
        if (safe == "0" || safe == "false" || safe == "n" || safe == "no" || safe == "off") return JNI_FALSE;
        return def;
    }
    return orig_jni_native_get_bool ? orig_jni_native_get_bool(env, clazz, name, def) : def;
}

static jlong new_jni_native_find(JNIEnv *env, jclass clazz, jstring name) {
    jlong handle = orig_jni_native_find ? orig_jni_native_find(env, clazz, name) : 0;
    std::string safe;
    if (handle != 0 && safe_prop_for_jstring(env, name, &safe)) {
        std::lock_guard<std::mutex> lock(jni_prop_mutex);
        jni_prop_handles[handle] = safe;
    }
    return handle;
}

static bool safe_prop_for_handle(jlong handle, std::string *out) {
    if (!hide_active || !out) return false;
    std::lock_guard<std::mutex> lock(jni_prop_mutex);
    auto it = jni_prop_handles.find(handle);
    if (it == jni_prop_handles.end()) return false;
    *out = it->second;
    return true;
}

static jstring new_jni_native_hget1(JNIEnv *env, jclass clazz, jlong handle) {
    std::string safe;
    if (safe_prop_for_handle(handle, &safe)) return env->NewStringUTF(safe.c_str());
    return orig_jni_native_hget1 ? orig_jni_native_hget1(env, clazz, handle) : env->NewStringUTF("");
}

static jstring new_jni_native_hget2(JNIEnv *env, jclass clazz, jlong handle, jstring def) {
    std::string safe;
    if (safe_prop_for_handle(handle, &safe)) return env->NewStringUTF(safe.c_str());
    return orig_jni_native_hget2 ? orig_jni_native_hget2(env, clazz, handle, def) : def;
}

static int new___system_property_get(const char* name, char* value) {
    if (hide_active) {
        const char* safe = find_override(name);
        if (safe) {
            size_t len = strlen(safe);
            if (len >= PROP_VALUE_MAX) len = PROP_VALUE_MAX - 1;
            memcpy(value, safe, len);
            value[len] = '\0';
            if (strstr(name, "adb") || strstr(name, "usb")) {
                LOGI("prop_get override: %s=%s", name, safe);
            }
            return static_cast<int>(len);
        }
    }
    int ret = orig_system_property_get(name, value);
    if (hide_active && ret > 0 && should_strip_adb_value(name)) {
        strip_adb_from_value(value);
        ret = static_cast<int>(strlen(value));
        LOGI("prop_get strip: %s=%s", name, value);
    }
    return ret;
}

static void new___system_property_read_callback(
        const prop_info* pi,
        void (*orig_cb)(void* cookie, const char* name, const char* value, uint32_t serial),
        void* cookie) {
    if (!hide_active) {
        orig_system_property_read_callback(pi, orig_cb, cookie);
        return;
    }
    struct cb_data {
        void (*cb)(void*, const char*, const char*, uint32_t);
        void* cookie;
    };
    auto* d = new cb_data{orig_cb, cookie};
    auto wrapped = [](void* p, const char* name, const char* value, uint32_t serial) {
        auto* cd = static_cast<cb_data*>(p);
        const char* v = value;
        if (const char* safe = find_override(name)) v = safe;
        else if (should_strip_adb_value(name)) {
            static thread_local char buf[PROP_VALUE_MAX];
            strncpy(buf, value, PROP_VALUE_MAX - 1);
            buf[PROP_VALUE_MAX - 1] = '\0';
            strip_adb_from_value(buf);
            v = buf;
        }
        cd->cb(cd->cookie, name, v, serial);
        delete cd;
    };
    orig_system_property_read_callback(pi, wrapped, d);
}

static int new___system_property_read(const prop_info* pi, char* name, char* value) {
    int ret = orig_system_property_read(pi, name, value);
    if (!hide_active || ret <= 0 || !name || !value) {
        return ret;
    }
    if (const char* safe = find_override(name)) {
        size_t len = strlen(safe);
        if (len >= PROP_VALUE_MAX) len = PROP_VALUE_MAX - 1;
        memcpy(value, safe, len);
        value[len] = '\0';
        LOGI("prop_read override: %s=%s", name, safe);
        return static_cast<int>(len);
    }
    if (should_strip_adb_value(name)) {
        strip_adb_from_value(value);
        LOGI("prop_read strip: %s=%s", name, value);
        return static_cast<int>(strlen(value));
    }
    return ret;
}

static int new_property_get(const char* name, char* value, const char* default_value) {
    if (hide_active) {
        const char* safe = find_override(name);
        if (safe) {
            size_t len = strlen(safe);
            if (len >= PROP_VALUE_MAX) len = PROP_VALUE_MAX - 1;
            memcpy(value, safe, len);
            value[len] = '\0';
            return static_cast<int>(len);
        }
    }
    int ret = orig_property_get(name, value, default_value);
    if (hide_active && ret > 0 && should_strip_adb_value(name)) {
        strip_adb_from_value(value);
        ret = static_cast<int>(strlen(value));
    }
    return ret;
}

// Strategy A: Hook SystemProperties.native_set() to intercept property writes
// When system_server writes ADB-related properties, we sanitize the values at source
static bool should_modify_set_value(const char *name, const char *value, char *out, size_t out_size) {
    if (!name || !value || !out) return false;

    // Override specific properties entirely
    for (auto &o : overrides) {
        if (strcmp(name, o.name) == 0) {
            strncpy(out, o.safe_value, out_size - 1);
            out[out_size - 1] = '\0';
            return true;
        }
    }

    // Strip "adb" from USB config values
    if (should_strip_adb_value(name) && strstr(value, "adb")) {
        strncpy(out, value, out_size - 1);
        out[out_size - 1] = '\0';
        strip_adb_from_value(out);
        return true;
    }

    return false;
}

static void new_jni_native_set(JNIEnv *env, jclass clazz, jstring name, jstring value) {
    if (!hide_active || !name || !value) {
        if (orig_jni_native_set) orig_jni_native_set(env, clazz, name, value);
        return;
    }
    const char *name_str = env->GetStringUTFChars(name, nullptr);
    const char *value_str = env->GetStringUTFChars(value, nullptr);
    if (!name_str || !value_str) {
        if (name_str) env->ReleaseStringUTFChars(name, name_str);
        if (value_str) env->ReleaseStringUTFChars(value, value_str);
        if (orig_jni_native_set) orig_jni_native_set(env, clazz, name, value);
        return;
    }

    char modified[PROP_VALUE_MAX] = {};
    if (should_modify_set_value(name_str, value_str, modified, sizeof(modified))) {
        LOGI("prop_set override: %s=%s → %s", name_str, value_str, modified);
        jstring new_val = env->NewStringUTF(modified);
        env->ReleaseStringUTFChars(name, name_str);
        env->ReleaseStringUTFChars(value, value_str);
        if (orig_jni_native_set) orig_jni_native_set(env, clazz, name, new_val);
        env->DeleteLocalRef(new_val);
    } else {
        env->ReleaseStringUTFChars(name, name_str);
        env->ReleaseStringUTFChars(value, value_str);
        if (orig_jni_native_set) orig_jni_native_set(env, clazz, name, value);
    }
}

static void new_jni_native_set_key(JNIEnv *env, jclass clazz, jstring name, jstring value, jstring key) {
    // native_set with 3 args (name, value, key) — delegate to 2-arg version
    if (!hide_active || !name || !value) {
        if (orig_jni_native_set_key) orig_jni_native_set_key(env, clazz, name, value, key);
        return;
    }
    const char *name_str = env->GetStringUTFChars(name, nullptr);
    const char *value_str = env->GetStringUTFChars(value, nullptr);
    if (!name_str || !value_str) {
        if (name_str) env->ReleaseStringUTFChars(name, name_str);
        if (value_str) env->ReleaseStringUTFChars(value, value_str);
        if (orig_jni_native_set_key) orig_jni_native_set_key(env, clazz, name, value, key);
        return;
    }

    char modified[PROP_VALUE_MAX] = {};
    if (should_modify_set_value(name_str, value_str, modified, sizeof(modified))) {
        LOGI("prop_set_key override: %s=%s → %s", name_str, value_str, modified);
        jstring new_val = env->NewStringUTF(modified);
        env->ReleaseStringUTFChars(name, name_str);
        env->ReleaseStringUTFChars(value, value_str);
        if (orig_jni_native_set_key) orig_jni_native_set_key(env, clazz, name, new_val, key);
        env->DeleteLocalRef(new_val);
    } else {
        env->ReleaseStringUTFChars(name, name_str);
        env->ReleaseStringUTFChars(value, value_str);
        if (orig_jni_native_set_key) orig_jni_native_set_key(env, clazz, name, value, key);
    }
}

} // namespace

void set_hide_active(bool active) {
    hide_active = active;
    hide::set_maps_hide_active(active);
}

void hook_native_properties() {
    // Register property hooks for ALL libraries, not just specific system ones.
    // App native libraries (.so from APK) need to be hooked too.
    bool found_any = false;
    int registered = 0;

    // First pass: hook system libraries specifically (these are guaranteed to be loaded)
    for (auto& map : lsplt::MapInfo::Scan()) {
        if (map.offset != 0 || !map.is_private || !(map.perms & PROT_READ)) continue;

        // Skip libc.so - we can't PLT-hook a library's own exports
        if (map.path.ends_with("/libc.so")) continue;

        if (map.path.ends_with("/libandroid_runtime.so") ||
            map.path.ends_with("/libbase.so") ||
            map.path.ends_with("/libcutils.so") ||
            map.path.ends_with("/libsystemproperties.so") ||
            map.path.ends_with("/libselinux.so") ||
            map.path.ends_with("/libhwui.so")) {

            if (!orig_system_property_get) {
                if (lsplt::RegisterHook(map.dev, map.inode, "__system_property_get",
                                    (void*)new___system_property_get,
                                    (void**)&orig_system_property_get)) {
                    registered++;
                }
            }
            if (!orig_system_property_read_callback) {
                if (lsplt::RegisterHook(map.dev, map.inode, "__system_property_read_callback",
                                    (void*)new___system_property_read_callback,
                                    (void**)&orig_system_property_read_callback)) {
                    registered++;
                }
            }
            if (!orig_system_property_read) {
                if (lsplt::RegisterHook(map.dev, map.inode, "__system_property_read",
                                    (void*)new___system_property_read,
                                    (void**)&orig_system_property_read)) {
                    registered++;
                }
            }
            if (map.path.ends_with("/libcutils.so") && !orig_property_get) {
                if (lsplt::RegisterHook(map.dev, map.inode, "property_get",
                                    (void*)new_property_get,
                                    (void**)&orig_property_get)) {
                    registered++;
                }
            }
            found_any = true;
        }
    }

    if (registered > 0) {
        LOGI("hide: registered %d property hooks", registered);
    }
    if (!found_any) {
        LOGW("hide: no libraries found for property hooks");
    }
}

void rehook_properties_for_app() {
    // Re-register property hooks for all currently loaded libraries.
    // Called from the unshare hook to catch any newly loaded libraries.
    int registered = 0;
    for (auto& map : lsplt::MapInfo::Scan()) {
        if (map.offset != 0 || !map.is_private || !(map.perms & PROT_READ)) continue;
        if (map.path.ends_with("/libc.so")) continue;

        if (orig_system_property_get) {
            if (lsplt::RegisterHook(map.dev, map.inode, "__system_property_get",
                                (void*)new___system_property_get,
                                nullptr)) {
                registered++;
            }
        }
        if (orig_system_property_read_callback) {
            if (lsplt::RegisterHook(map.dev, map.inode, "__system_property_read_callback",
                                (void*)new___system_property_read_callback,
                                nullptr)) {
                registered++;
            }
        }
        if (orig_system_property_read) {
            if (lsplt::RegisterHook(map.dev, map.inode, "__system_property_read",
                                (void*)new___system_property_read,
                                nullptr)) {
                registered++;
            }
        }
    }

    if (registered > 0) {
        lsplt::CommitHook();
        LOGI("hide: rehooked %d libs for property hooks", registered);
    }
}

// Hook android_dlopen_ext to re-register property hooks when app libraries load
static void* (*orig_android_dlopen_ext)(const char*, int, const android_dlextinfo*) = nullptr;

static void* hooked_android_dlopen_ext(const char* filename, int flags, const android_dlextinfo* info) {
    void* result = orig_android_dlopen_ext(filename, flags, info);
    if (result && filename && hide_active) {
        // Re-register property hooks for the newly loaded library
        rehook_properties_for_app();
    }
    return result;
}

void hook_dlopen_for_rehook() {
    for (auto& map : lsplt::MapInfo::Scan()) {
        if (map.path.ends_with("/libc.so")) {
            lsplt::RegisterHook(map.dev, map.inode, "android_dlopen_ext",
                               (void*)hooked_android_dlopen_ext,
                               (void**)&orig_android_dlopen_ext);
            break;
        }
    }
}

void hook_java_system_properties(
        JNIEnv *env,
        void (*hooker)(JNIEnv *, const char *, JNINativeMethod *, int)) {
    if (!env || !hooker) return;

    JNINativeMethod methods[] = {
            {"native_get", "(Ljava/lang/String;)Ljava/lang/String;", reinterpret_cast<void *>(new_jni_native_get1)},
            {"native_get", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", reinterpret_cast<void *>(new_jni_native_get2)},
            {"native_get_int", "(Ljava/lang/String;I)I", reinterpret_cast<void *>(new_jni_native_get_int)},
            {"native_get_long", "(Ljava/lang/String;J)J", reinterpret_cast<void *>(new_jni_native_get_long)},
            {"native_get_boolean", "(Ljava/lang/String;Z)Z", reinterpret_cast<void *>(new_jni_native_get_bool)},
            {"native_find", "(Ljava/lang/String;)J", reinterpret_cast<void *>(new_jni_native_find)},
            {"native_get", "(J)Ljava/lang/String;", reinterpret_cast<void *>(new_jni_native_hget1)},
            {"native_get", "(JLjava/lang/String;)Ljava/lang/String;", reinterpret_cast<void *>(new_jni_native_hget2)},
            {"native_set", "(Ljava/lang/String;Ljava/lang/String;)V", reinterpret_cast<void *>(new_jni_native_set)},
            {"native_set", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", reinterpret_cast<void *>(new_jni_native_set_key)},
    };

    hooker(env, "android/os/SystemProperties", methods,
           static_cast<int>(sizeof(methods) / sizeof(methods[0])));

    orig_jni_native_get1 = reinterpret_cast<jni_get1_fn>(methods[0].fnPtr);
    orig_jni_native_get2 = reinterpret_cast<jni_get2_fn>(methods[1].fnPtr);
    orig_jni_native_get_int = reinterpret_cast<jni_get_int_fn>(methods[2].fnPtr);
    orig_jni_native_get_long = reinterpret_cast<jni_get_long_fn>(methods[3].fnPtr);
    orig_jni_native_get_bool = reinterpret_cast<jni_get_bool_fn>(methods[4].fnPtr);
    orig_jni_native_find = reinterpret_cast<jni_find_fn>(methods[5].fnPtr);
    orig_jni_native_hget1 = reinterpret_cast<jni_hget1_fn>(methods[6].fnPtr);
    orig_jni_native_hget2 = reinterpret_cast<jni_hget2_fn>(methods[7].fnPtr);
    orig_jni_native_set = reinterpret_cast<jni_set_fn>(methods[8].fnPtr);
    orig_jni_native_set_key = reinterpret_cast<jni_set_key_fn>(methods[9].fnPtr);

    int hooked = 0;
    for (auto &m : methods) {
        if (m.fnPtr) hooked++;
    }
    LOGI("hide: hooked %d SystemProperties JNI methods", hooked);
}
