#ifndef PMU_STORAGE_H
#define PMU_STORAGE_H
#include <sys/stat.h>
#include <sys/vfs.h>
#include <string.h>

static inline int pmu_helper_directory_valid(const char *path) {
    static const char *roots[] = {"/nvme", "/sdcard", "/storage", "/data/UserData"};
    static const char suffix[] = "/MPC Documents/Devices/Plugin Manager helper";
    for (unsigned i = 0; i < sizeof roots / sizeof roots[0]; ++i) {
        size_t length = strlen(roots[i]);
        if (strncmp(path, roots[i], length) == 0 && strcmp(path + length, suffix) == 0) return 1;
    }
    return 0;
}
static inline int pmu_helper_exfat(int fd) {
#if defined(__aarch64__)
    static const char *roots[] = {"/nvme", "/sdcard", "/storage", "/data/UserData"};
    struct statfs filesystem;
    struct stat internal, source;
    if (fstatfs(fd, &filesystem) != 0 || filesystem.f_type != 0x2011bab0 || fstat(fd, &source) != 0) return 0;
    for (unsigned i = 0; i < sizeof roots / sizeof roots[0]; ++i)
        if (stat(roots[i], &internal) == 0 && source.st_dev == internal.st_dev) return 1;
#else
    (void)fd;
#endif
    return 0;
}
#endif
