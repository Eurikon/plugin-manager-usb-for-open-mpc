#define _GNU_SOURCE
#include <errno.h>
#include "platform.hpp"
#include "storage.h"
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PMU_REUSABLE
#define PMU_REUSABLE 0
#endif
#ifndef PMU_CONFIRMED
#define PMU_CONFIRMED 0
#endif

static char directory_path[PATH_MAX] = PMU_HELPER_DIRECTORY;
static char bootstrap_path[PATH_MAX] = PMU_HELPER_DIRECTORY "/bootstrap.sh";
#if PMU_CONFIRMED
static char payload_path[PATH_MAX];
#endif
__attribute__((visibility("hidden"))) int pmu_set_helper_directory(const char *path) {
#if !defined(__aarch64__)
    if (!path || strcmp(path, PMU_HELPER_DIRECTORY) != 0) return 0;
#endif
    if (!path || !pmu_helper_directory_valid(path) || strlen(path) + sizeof("/bootstrap.sh") > sizeof bootstrap_path) return 0;
    strcpy(directory_path, path);
    strcpy(bootstrap_path, path);
    strcat(bootstrap_path, "/bootstrap.sh");
    return 1;
}
#if PMU_REUSABLE
static const char request_name[] = "manager-enabled";
static const char request_token[] = "open-mpc-manager-reusable-v1\n";
#else
static const char request_name[] = "manager-request.once";
static const char claim_name[] = "manager-claim.once";
static const char request_token[] = "open-mpc-manager-test-once-v1\n";
#endif
static char *launch_arguments[] = {
    "/usr/bin/systemd-run",
    "--unit=open-mpc-manager-bootstrap-test",
    "--collect",
    "--no-block",
    "-pRestart=no",
    "-pRuntimeMaxSec=90",
    "-pTimeoutStopSec=10",
    "-pKillMode=control-group",
    "-pStandardOutput=null",
    "-pStandardError=null",
    "/bin/sh",
    bootstrap_path,
#if PMU_CONFIRMED
    "--confirmed", NULL, NULL, NULL,
    NULL, NULL, NULL,
#endif
    NULL,
};
#if PMU_CONFIRMED
__attribute__((visibility("hidden"))) int pmu_set_payload_context(
    const char *root, const char *payload, const char *bootstrap, const char *generation) {
    char prefix[PATH_MAX];
    if (!root || !payload || !bootstrap || !generation || strlen(generation) != 64
        || strspn(generation, "0123456789abcdef") != 64
        || strlen(root) + sizeof("/.pmu-versions/") + 64 >= sizeof prefix) return 0;
    strcpy(prefix, root);
    strcat(prefix, "/.pmu-versions/");
    strcat(prefix, generation);
    size_t length = strlen(prefix);
    if (strcmp(payload, prefix) != 0 || strncmp(bootstrap, prefix, length) != 0
        || bootstrap[length] != '/' || !bootstrap[length + 1]
        || strlen(bootstrap) >= sizeof bootstrap_path
        || strspn(bootstrap + length + 1, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-/ ") != strlen(bootstrap + length + 1)) return 0;
    const char *part = bootstrap + length + 1;
    while (*part) {
        const char *end = strchr(part, '/');
        size_t count = end ? (size_t)(end - part) : strlen(part);
        if (count == 0 || (count == 1 && part[0] == '.')
            || (count == 2 && part[0] == '.' && part[1] == '.')) return 0;
        if (!end) break;
        part = end + 1;
        if (!*part) return 0;
    }
    if (!pmu_set_helper_directory(root)) return 0;
    strcpy(payload_path, payload);
    strcpy(bootstrap_path, bootstrap);
    launch_arguments[16] = "--pmu-helper-v1";
    launch_arguments[17] = directory_path;
    launch_arguments[18] = payload_path;
    return 1;
}
#endif
static char *const launch_environment[] = {
    "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C", NULL,
};

#if PMU_REUSABLE && !PMU_CONFIRMED
static int claim_boot(void) {
    struct stat status;
    int directory = open("/run", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0)
        return 0;
    if (fstat(directory, &status) != 0 || !S_ISDIR(status.st_mode)
        || status.st_uid != 0 || (status.st_mode & 0022) != 0) {
        close(directory);
        return 0;
    }
    int file = openat(directory, "open-mpc-manager-startup.attempt",
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    close(directory);
    if (file < 0)
        return 0;
    int valid = fchmod(file, 0600) == 0 && fstat(file, &status) == 0
        && S_ISREG(status.st_mode) && status.st_uid == 0
        && (status.st_mode & 07777) == 0600 && status.st_nlink == 1;
    ssize_t count = valid ? write(file, request_token, sizeof(request_token) - 1) : -1;
    int closed = close(file);
    return count == (ssize_t)(sizeof(request_token) - 1) && closed == 0;
}
#endif

static void close_inherited_descriptors(int maximum) {
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 3U, UINT_MAX, 0U) == 0)
        return;
#endif
    for (int file = 3; file < maximum; ++file)
        close(file);
}

static void handoff(int null_file, int maximum, const struct sigaction *defaults,
                    const sigset_t *unblocked) {
    pid_t child = fork();
    if (child < 0)
        return;
    if (child == 0) {
        for (int number = 1; number < NSIG; ++number) {
            if (number != SIGKILL && number != SIGSTOP)
                sigaction(number, defaults, NULL);
        }
        if (sigprocmask(SIG_SETMASK, unblocked, NULL) != 0)
            _exit(125);
        alarm(10);
        if (setsid() < 0)
            _exit(125);
        pid_t detached = fork();
        if (detached < 0)
            _exit(125);
        if (detached > 0)
            _exit(0);
        alarm(10);
        for (int file = 0; file < 3; ++file) {
            if (dup2(null_file, file) < 0 || fcntl(file, F_SETFD, 0) != 0)
                _exit(125);
        }
        if (chdir("/") != 0)
            _exit(125);
        close_inherited_descriptors(maximum);
        execve(launch_arguments[0], launch_arguments, launch_environment);
        _exit(127);
    }
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
}

#if PMU_CONFIRMED
static int dispatch_selected(const char *usb, const char *identity, const char *permit, int prepare) {
    const char *prefix = prepare ? "/run/open-mpc-manager-preparation." : "/run/open-mpc-manager-approval.";
    const char *suffix = prepare ? "/request" : "/permit";
    const size_t prefix_length = strlen(prefix);
    static const char unit_prefix[] = "--unit=open-mpc-manager-bootstrap-preparation-";
    char preparation_unit[sizeof(unit_prefix) + 6];
    if (!usb || strncmp(usb, "/media/", 7) != 0 || !usb[7]
        || usb[7] == '/'
        || (strchr(usb + 7, '/') && strcmp(strchr(usb + 7, '/'), "/Plugin Manager USB") != 0)
        || strchr(usb, '\n') || strchr(usb, '\r')
        || !identity || !*identity || strspn(identity, "0123456789:") != strlen(identity)
        || strlen(identity) > 96 || !permit
        || strncmp(permit, prefix, prefix_length) != 0
        || strlen(permit) != prefix_length + 6 + strlen(suffix)
        || strcmp(permit + prefix_length + 6, suffix) != 0
        || strspn(permit + prefix_length, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != 6)
        return 0;
    memcpy(preparation_unit, unit_prefix, sizeof(unit_prefix) - 1);
    memcpy(preparation_unit + sizeof(unit_prefix) - 1, permit + prefix_length, 6);
    preparation_unit[sizeof(unit_prefix) + 5] = '\0';
    launch_arguments[1] = prepare ? preparation_unit : "--unit=open-mpc-manager-bootstrap-test";
    launch_arguments[12] = prepare ? "--prepare" : "--confirmed";
    launch_arguments[13] = (char *)usb;
    launch_arguments[14] = (char *)identity;
    launch_arguments[15] = (char *)permit;
#else
int pmu_start_manager(void *state) {
    (void)state;
#endif
    struct stat status;
    struct rlimit descriptors;
    sigset_t unblocked;
    struct sigaction defaults = {0};
    defaults.sa_handler = SIG_DFL;
    if (geteuid() != 0 || sigemptyset(&defaults.sa_mask) != 0 || sigemptyset(&unblocked) != 0
        || getrlimit(RLIMIT_NOFILE, &descriptors) != 0
        || descriptors.rlim_cur == RLIM_INFINITY || descriptors.rlim_cur > INT_MAX)
        return 0;
    int directory = open(directory_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0)
        return 0;
    if (fstat(directory, &status) != 0 || !S_ISDIR(status.st_mode)
        || status.st_uid != 0 || ((status.st_mode & 0722) != 0700 && !pmu_helper_exfat(directory))
        || fstatat(directory, bootstrap_path + strlen(directory_path) + 1, &status, AT_SYMLINK_NOFOLLOW) != 0
        || !S_ISREG(status.st_mode) || status.st_uid != 0
        || ((status.st_mode & 0022) != 0 && !pmu_helper_exfat(directory)) || status.st_size <= 0 || status.st_size > 64 * 1024) {
        close(directory);
        return 0;
    }
    int file = openat(directory, request_name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (file < 0) {
        close(directory);
        return 0;
    }
    char bytes[sizeof(request_token)];
    int valid = fstat(file, &status) == 0 && S_ISREG(status.st_mode)
        && status.st_uid == 0 && ((status.st_mode & 0022) == 0 || pmu_helper_exfat(file))
        && status.st_size == (off_t)(sizeof(request_token) - 1);
    ssize_t count = valid ? read(file, bytes, sizeof(bytes)) : -1;
    int closed = close(file);
    if (closed != 0 || count != (ssize_t)(sizeof(request_token) - 1)
        || memcmp(bytes, request_token, sizeof(request_token) - 1) != 0) {
        close(directory);
        return 0;
    }
#if PMU_REUSABLE && !PMU_CONFIRMED
    if (!claim_boot()) {
        close(directory);
        return 0;
    }
#endif
    int null_file = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null_file < 0) {
        close(directory);
        return 0;
    }
#if !PMU_REUSABLE
    file = openat(directory, claim_name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (file < 0) {
        close(null_file);
        close(directory);
        return 0;
    }
    count = write(file, request_token, sizeof(request_token) - 1);
    closed = close(file);
    if (count != (ssize_t)(sizeof(request_token) - 1) || closed != 0
        || unlinkat(directory, request_name, 0) != 0
        || fstatat(directory, request_name, &status, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
        close(null_file);
        close(directory);
        return 0;
    }
#endif
    close(directory);
    handoff(null_file, (int)descriptors.rlim_cur, &defaults, &unblocked);
    close(null_file);
    return 0;
}
#if PMU_CONFIRMED
int pmu_launch_confirmed(const char *usb, const char *identity, const char *permit) {
    return dispatch_selected(usb, identity, permit, 0);
}

int pmu_prepare_manager(const char *usb, const char *identity, const char *request) {
    return dispatch_selected(usb, identity, request, 1);
}
#endif
