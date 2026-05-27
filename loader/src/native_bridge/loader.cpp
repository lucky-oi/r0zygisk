#include <dlfcn.h>
#include <android/dlext.h>
#include <array>
#include <fcntl.h>
#include <mutex>
#include <string_view>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "daemon.h"
#include "logging.h"
#include "native_bridge_callbacks.h"

#ifndef MODULE_ID
#define MODULE_ID "r0z"
#endif

namespace {

constexpr const char *kModulePath = "/data/adb/modules/" MODULE_ID;
constexpr const char *kZygiskLibPath = LP_SELECT("/system/lib/libr0zgk.so", "/system/lib64/libr0zgk.so");
constexpr const char *kPayloadLibPath = LP_SELECT("/system/lib/libpayload.so", "/system/lib64/libpayload.so");

std::mutex g_lock;
bool g_loaded = false;

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

int create_memfd(const char *name) {
#ifdef SYS_memfd_create
    return static_cast<int>(syscall(SYS_memfd_create, name, MFD_CLOEXEC));
#else
    (void) name;
    return -1;
#endif
}

bool copy_to_fd(int in, int out) {
    char buf[16384];
    for (;;) {
        ssize_t r = read(in, buf, sizeof(buf));
        if (r == 0) return lseek(out, 0, SEEK_SET) == 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        char *p = buf;
        while (r > 0) {
            ssize_t w = write(out, p, static_cast<size_t>(r));
            if (w < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            p += w;
            r -= w;
        }
    }
}

void *dlopen_memfd(const char *path, const char *name, int flags) {
    int in = open(path, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        return nullptr;
    }
    int fd = create_memfd(name);
    if (fd < 0) {
        close(in);
        return nullptr;
    }
    if (!copy_to_fd(in, fd)) {
        close(in);
        close(fd);
        return nullptr;
    }
    close(in);

    android_dlextinfo info{};
    info.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
    info.library_fd = fd;
    void *handle = android_dlopen_ext(name, flags, &info);
    close(fd);
    return handle;
}

bool load_zygisk() {
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_loaded) {
        return true;
    }

    void *payload = dlopen_memfd(kPayloadLibPath, "libandroid_runtime_plugin.so", RTLD_NOW | RTLD_GLOBAL);
    if (!payload) {
        payload = dlopen(kPayloadLibPath, RTLD_NOW | RTLD_GLOBAL);
    }
    if (!payload) {
        LOGW("payload is unavailable at %s: %s", kPayloadLibPath, dlerror());
    }

    void *handle = dlopen_memfd(kZygiskLibPath, "libandroid_runtime_ext.so", RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        handle = dlopen(kZygiskLibPath, RTLD_NOW | RTLD_GLOBAL);
    }
    if (!handle) {
        LOGE("failed to load %s: %s", kZygiskLibPath, dlerror());
        return false;
    }

    using zygisk_entry_t = bool (*)(void *, const char *);
    auto entry = reinterpret_cast<zygisk_entry_t>(dlsym(handle, "zygisk_entry"));
    if (!entry) {
        using legacy_entry_t = void (*)(void *, const char *);
        auto legacy_entry = reinterpret_cast<legacy_entry_t>(dlsym(handle, "entry"));
        if (!legacy_entry) {
            LOGE("failed to find zygisk entry: %s", dlerror());
            return false;
        }
        legacy_entry(handle, kModulePath);
        g_loaded = true;
        return true;
    }

    if (!entry(handle, kModulePath)) {
        return false;
    }

    g_loaded = true;
    return true;
}

bool is_zygote_process() {
    if (getuid() != 0) {
        return false;
    }
    std::string_view cmdline = getprogname();
    static constexpr std::array<const char *, 5> kZygoteProcesses{
            "zygote", "zygote32", "zygote64", "usap32", "usap64"
    };
    for (const auto *name : kZygoteProcesses) {
        if (cmdline == name) {
            return true;
        }
    }
    return false;
}

bool nb_initialize(const NativeBridgeRuntimeCallbacks *, const char *, const char *) {
    // Keep ART startup alive even if injection is not ready yet. The actual load is retried
    // from preZygoteFork, after the daemon has had a chance to start from post-fs-data.
    load_zygisk();
    return true;
}

void *nb_load_library(const char *libpath, int flag) {
    if (!g_loaded) {
        load_zygisk();
    }
    return dlopen(libpath, flag);
}

void *nb_get_trampoline(void *handle, const char *name, const char *, uint32_t) {
    return dlsym(handle, name);
}

bool nb_is_supported(const char *) {
    return true;
}

const char *nb_get_app_env(const char *) {
    return nullptr;
}

bool nb_is_compatible_with(uint32_t version) {
    (void) version;
    return true;
}

NativeBridgeSignalHandlerFn nb_get_signal_handler(int) {
    return nullptr;
}

int nb_unload_library(void *handle) {
    return handle ? dlclose(handle) : 0;
}

const char *nb_get_error() {
    auto *error = dlerror();
    return error ? error : "";
}

bool nb_is_path_supported(const char *) {
    return true;
}

bool nb_init_anonymous_namespace(const char *, const char *) {
    return true;
}

NativeBridgeNamespace *nb_create_namespace(const char *, const char *, const char *, uint64_t,
                                           const char *, NativeBridgeNamespace *) {
    return nullptr;
}

bool nb_link_namespaces(NativeBridgeNamespace *, NativeBridgeNamespace *, const char *) {
    return true;
}

void *nb_load_library_ext(const char *libpath, int flag, const void *) {
    if (!g_loaded) {
        load_zygisk();
    }
    return dlopen(libpath, flag);
}

NativeBridgeNamespace *nb_get_vendor_namespace() {
    return nullptr;
}

NativeBridgeNamespace *nb_get_exported_namespace(const char *) {
    return nullptr;
}

void nb_pre_zygote_fork() {
    load_zygisk();
}

} // namespace

__used __attribute__((constructor))
void zn_constructor() {
    if (!is_zygote_process()) {
        return;
    }
    for (int i = 0; i < 3; ++i) {
        if (load_zygisk()) {
            return;
        }
        sleep(1);
    }
}

extern "C" [[gnu::visibility("default")]]
bool zn_entry(int, const char *) {
    return load_zygisk();
}

extern "C" [[gnu::visibility("default")]]
NativeBridgeCallbacks NativeBridgeItf{
        .version = 6,
        .initialize = nb_initialize,
        .loadLibrary = nb_load_library,
        .getTrampoline = nb_get_trampoline,
        .isSupported = nb_is_supported,
        .getAppEnv = nb_get_app_env,
        .isCompatibleWith = nb_is_compatible_with,
        .getSignalHandler = nb_get_signal_handler,
        .unloadLibrary = nb_unload_library,
        .getError = nb_get_error,
        .isPathSupported = nb_is_path_supported,
        .initAnonymousNamespace = nb_init_anonymous_namespace,
        .createNamespace = nb_create_namespace,
        .linkNamespaces = nb_link_namespaces,
        .loadLibraryExt = nb_load_library_ext,
        .getVendorNamespace = nb_get_vendor_namespace,
        .getExportedNamespace = nb_get_exported_namespace,
        .preZygoteFork = nb_pre_zygote_fork,
};
