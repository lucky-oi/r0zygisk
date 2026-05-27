#include <mntent.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 1U
#endif

#include "files.hpp"
#include "logging.h"
#include "misc.hpp"
#include "zygisk.hpp"

using namespace std::string_view_literals;

namespace {
    static std::string make_module_dir() {
        std::string p;
        p += '/';
        p += 'd'; p += 'a'; p += 't'; p += 'a';
        p += '/';
        p += 'a'; p += 'd'; p += 'b';
        p += '/';
        p += 'm'; p += 'o'; p += 'd'; p += 'u'; p += 'l'; p += 'e'; p += 's';
        return p;
    }

    static std::string make_data_adb() {
        std::string p;
        p += '/';
        p += 'd'; p += 'a'; p += 't'; p += 'a';
        p += '/';
        p += 'a'; p += 'd'; p += 'b';
        return p;
    }

    static std::string make_ksu_tag() {
        std::string s;
        s += 'K'; s += 'S'; s += 'U';
        return s;
    }

    static std::string make_magisk_tag() {
        std::string s;
        s += 'm'; s += 'a'; s += 'g'; s += 'i'; s += 's'; s += 'k';
        return s;
    }

    static std::string make_worker_tag() {
        std::string s;
        s += 'w'; s += 'o'; s += 'r'; s += 'k'; s += 'e'; s += 'r';
        return s;
    }

    static std::string make_adb_modules() {
        std::string p;
        p += '/';
        p += 'a'; p += 'd'; p += 'b';
        p += '/';
        p += 'm'; p += 'o'; p += 'd'; p += 'u'; p += 'l'; p += 'e'; p += 's';
        return p;
    }

    const std::vector<std::string> make_partitions() {
        return {
            std::string("/") + "system",
            std::string("/") + "vendor",
            std::string("/") + "product",
            std::string("/") + "system" + "_" + "ext",
            std::string("/") + "odm",
            std::string("/") + "oem"
        };
    }

    void lazy_unmount(const char* mountpoint) {
        if (umount2(mountpoint, MNT_DETACH) != -1) {
            LOGD("done %s", mountpoint);
        } else {
#ifndef NDEBUG
            PLOGE("fail %s", mountpoint);
#endif
        }
    }
}

void revert_unmount_ksu() {
    static const auto MODULE_DIR = make_module_dir();
    static const auto DATA_ADB = make_data_adb();
    static const auto KSU_TAG = make_ksu_tag();
    static const auto PARTITIONS = make_partitions();

    std::string loop_src;
    std::vector<std::string> targets;
    targets.emplace_back(MODULE_DIR);

    for (auto& info: parse_mount_info("self")) {
        if (info.target == MODULE_DIR) {
            loop_src = info.source;
            continue;
        }
        if (info.target.starts_with(DATA_ADB)) {
            targets.emplace_back(info.target);
        }
        if (info.type == "overlay"
            && info.source == KSU_TAG
            && std::find(PARTITIONS.begin(), PARTITIONS.end(), info.target) != PARTITIONS.end()) {
            targets.emplace_back(info.target);
        }
        if (info.type == "tmpfs" && info.source == KSU_TAG) {
            targets.emplace_back(info.target);
        }
    }
    for (auto& info: parse_mount_info("self")) {
        if (info.source == loop_src && info.target != MODULE_DIR) {
            targets.emplace_back(info.target);
        }
    }

    for (auto& s: reversed(targets)) {
        lazy_unmount(s.data());
    }
}

void revert_unmount_magisk() {
    static const auto DATA_ADB = make_data_adb();
    static const auto MAGISK_TAG = make_magisk_tag();
    static const auto WORKER_TAG = make_worker_tag();
    static const auto ADB_MOD = make_adb_modules();

    std::vector<std::string> targets;

    for (auto& info: parse_mount_info("self")) {
        if (info.source == MAGISK_TAG || info.source == WORKER_TAG ||
            info.root.starts_with(ADB_MOD)) {
            targets.push_back(info.target);
        }
        if (info.target.starts_with(DATA_ADB)) {
            targets.emplace_back(info.target);
        }
    }

    for (auto& s: reversed(targets)) {
        lazy_unmount(s.data());
    }
}

void revert_unmount_apatch() {
    static const auto DATA_ADB = make_data_adb();
    static const auto MODULE_DIR = make_module_dir();

    std::vector<std::string> targets;
    std::string loop_src;

    for (auto& info: parse_mount_info("self")) {
        if (info.target == MODULE_DIR) {
            loop_src = info.source;
        }
        // Unmount bind mounts from module directory.
        if (info.source.compare(0, MODULE_DIR.size(), MODULE_DIR) == 0) {
            targets.emplace_back(info.target);
        }
        if (info.target.compare(0, DATA_ADB.size(), DATA_ADB) == 0) {
            targets.emplace_back(info.target);
        }
    }

    if (!loop_src.empty()) {
        for (auto& info: parse_mount_info("self")) {
            if (info.source == loop_src && info.target != MODULE_DIR) {
                targets.emplace_back(info.target);
            }
        }
    }

    for (auto& s: reversed(targets)) {
        lazy_unmount(s.data());
    }
}

void hide_from_maps() {
    static bool initialized = false;
    static std::vector<std::string> lib_markers;
    if (!initialized) {
        auto make_tag = [](const char *s) -> std::string {
            std::string r;
            while (*s) r += *s++;
            return r;
        };
        lib_markers.push_back(make_tag("\x6c\x69\x62\x61\x6f\x73\x70\x70\x72\x65\x66\x73")); // libaospprefs
        lib_markers.push_back(make_tag("\x6c\x69\x62\x73\x79\x73\x62\x72\x69\x64\x67\x65")); // libsysbridge
        lib_markers.push_back(make_tag("\x6c\x69\x62\x73\x79\x73\x61\x75\x78")); // libsysaux
        initialized = true;
    }

    auto my_addr = (uintptr_t)&hide_from_maps;
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) return;

    struct mapping_info {
        void *addr;
        size_t size;
        int prot;
    };
    std::vector<mapping_info> to_hide;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        bool should_hide = false;
        for (const auto &marker: lib_markers) {
            if (strstr(line, marker.c_str())) {
                should_hide = true;
                break;
            }
        }
        if (!should_hide) continue;

        unsigned long start, end;
        char perms[5] = {};
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3) continue;
        if (perms[0] != 'r') continue;
        if (start <= my_addr && my_addr < end && perms[2] == 'x') continue;

        size_t size = end - start;
        if (size == 0 || size > 64 * 1024 * 1024) continue;

        int prot = 0;
        if (perms[0] == 'r') prot |= PROT_READ;
        if (perms[1] == 'w') prot |= PROT_WRITE;
        if (perms[2] == 'x') prot |= PROT_EXEC;

        to_hide.push_back({(void *)start, size, prot});
    }
    fclose(fp);

    size_t hidden = 0;
    for (auto &m: to_hide) {
        void *saved = malloc(m.size);
        if (!saved) continue;
        memcpy(saved, m.addr, m.size);

        void *new_map = mmap(m.addr, m.size, PROT_READ | PROT_WRITE,
                             MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (new_map == MAP_FAILED) {
            free(saved);
            continue;
        }

        memcpy(m.addr, saved, m.size);
        mprotect(m.addr, m.size, m.prot);
        free(saved);
        hidden++;
    }
    LOGI("hide_from_maps remapped %zu mappings", hidden);
}

bool patch_prop_data(char *data, size_t size) {
    bool changed = false;
    int adb_count = 0, running_count = 0;
    // Replace "running\0" with "stopped\0" (same length 7+1)
    for (size_t i = 0; i + 8 <= size; i++) {
        if (memcmp(data + i, "running\0", 8) == 0) {
            memcpy(data + i, "stopped\0", 8);
            changed = true;
            running_count++;
            i += 7;
        }
    }
    // Replace standalone "adb" as a null-terminated value with "mtp"
    // Pattern: \0adb\0 or start-of-buffer "adb\0"
    for (size_t i = 0; i + 4 <= size; i++) {
        if (data[i] == 'a' && data[i + 1] == 'd' && data[i + 2] == 'b' && data[i + 3] == '\0') {
            bool left_ok = (i == 0 || data[i - 1] == '\0');
            if (left_ok) {
                memcpy(data + i, "mtp\0", 4);
                changed = true;
                adb_count++;
                i += 3;
            }
        }
    }
    // Handle comma-separated USB values: strip "adb" from multi-value strings
    // e.g. "mtp,adb\0" → "mtp\0", "adb,mtp\0" → "mtp\0", "rndis,adb,mtp\0" → "rndis,mtp\0"
    for (size_t i = 0; i + 3 <= size; i++) {
        if (data[i] == 'a' && data[i + 1] == 'd' && data[i + 2] == 'b') {
            // Check if this "adb" was already handled as a standalone value
            if (data[i + 3] == '\0' && (i == 0 || data[i - 1] == '\0')) continue;

            // Pattern: ,adb\0 or ,adb, → strip ",adb"
            if (i > 0 && data[i - 1] == ',') {
                size_t end = i + 3;
                if (data[end] == ',' || data[end] == '\0') {
                    // Remove ",adb" or ",adb,"
                    size_t remove_len = 4; // ",adb"
                    if (data[end] == ',') { remove_len = 5; end++; } // ",adb,"
                    size_t remaining = 0;
                    for (size_t j = end; data[j] != '\0' && j < size; j++) remaining++;
                    memmove(data + i - 1, data + i - 1 + remove_len, remaining + 1);
                    changed = true;
                    adb_count++;
                }
            }
            // Pattern: adb,\0 or adb,xxx → strip "adb,"
            else if (data[i + 3] == ',') {
                size_t end = i + 4; // after "adb,"
                size_t remaining = 0;
                for (size_t j = end; data[j] != '\0' && j < size; j++) remaining++;
                memmove(data + i, data + end, remaining + 1);
                changed = true;
                adb_count++;
            }
        }
    }
    if (adb_count > 0 || running_count > 0) {
        LOGI("patch_prop_data: adb=%d running=%d size=%zu", adb_count, running_count, size);
    }
    return changed;
}

// Debug: scan for "adb" substring in raw data
static void debug_scan_for_adb(char *data, size_t size, const char *label) {
    int found = 0;
    for (size_t i = 0; i + 3 <= size; i++) {
        if (data[i] == 'a' && data[i + 1] == 'd' && data[i + 2] == 'b') {
            found++;
            if (found <= 3) {
                // Print context around the match
                size_t ctx_start = (i >= 16) ? i - 16 : 0;
                size_t ctx_end = (i + 16 < size) ? i + 16 : size;
                char hex[128] = {};
                for (size_t j = ctx_start; j < ctx_end && (j - ctx_start) * 3 < 120; j++) {
                    snprintf(hex + (j - ctx_start) * 3, 4, "%02x ", (unsigned char)data[j]);
                }
                LOGI("scan_adb [%s] at %zu: %s", label, i, hex);
            }
        }
    }
    if (found > 0) LOGI("scan_adb [%s] total=%d", label, found);
}

void hide_adb_in_memory() {
    // Patch property file mmap regions by replacing with anonymous mappings
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) return;

    struct prop_mapping {
        void *addr;
        size_t size;
        int prot;
    };
    std::vector<prop_mapping> targets;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, "/dev/__properties__/")) continue;

        unsigned long start = 0, end = 0;
        char perms[5] = {};
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3) continue;
        if (end <= start) continue;
        size_t sz = end - start;
        if (sz == 0 || sz > 4 * 1024 * 1024) continue;

        int prot = 0;
        if (perms[0] == 'r') prot |= PROT_READ;
        if (perms[1] == 'w') prot |= PROT_WRITE;
        if (perms[2] == 'x') prot |= PROT_EXEC;
        if ((prot & PROT_READ) == 0) continue;

        targets.push_back({(void *)start, sz, prot});
    }
    fclose(fp);

    size_t mmap_patched = 0;
    for (auto &m : targets) {
        void *saved = malloc(m.size);
        if (!saved) continue;
        memcpy(saved, m.addr, m.size);

        void *new_map = mmap(m.addr, m.size, PROT_READ | PROT_WRITE,
                             MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (new_map == MAP_FAILED) {
            free(saved);
            continue;
        }

        memcpy(m.addr, saved, m.size);
        debug_scan_for_adb(static_cast<char *>(m.addr), m.size, "before");
        patch_prop_data(static_cast<char *>(m.addr), m.size);
        mprotect(m.addr, m.size, m.prot);
        free(saved);
        mmap_patched++;
    }

    LOGI("hide_adb_in_memory mmap=%zu total=%zu", mmap_patched, targets.size());
}
