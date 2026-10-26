#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "native_ui.h"
#include "platform.hpp"
#include "storage.h"
#include "panel_input.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <fcntl.h>
#include <fstream>
#include <link.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" int pmu_prepare_manager(const char *, const char *, const char *);

namespace {
#if defined(__aarch64__)
char source[PATH_MAX] = PMU_HELPER_DIRECTORY;
extern "C" int pmu_set_helper_directory(const char *);
bool selectSource() {
    Dl_info module{};
    char path[PATH_MAX];
    if (!dladdr(reinterpret_cast<void *>(&selectSource), &module) || !module.dli_fname
        || !realpath(module.dli_fname, path) || strcmp(path, module.dli_fname) != 0) return false;
    char *leaf = strrchr(path, '/');
    if (!leaf || strcmp(leaf, "/manager-entry-aarch64.so") != 0) return false;
    *leaf = 0;
    if (!pmu_helper_directory_valid(path) || !pmu_set_helper_directory(path)) return false;
    strcpy(source, path);
    return true;
}
#else
constexpr const char *source = PMU_HELPER_DIRECTORY;
#endif
constexpr const char *statePath = "/run/mpc-usb-manager-session";
constexpr const char marker[] = "mpc-usb-manager-v1\n";
constexpr const char enabled[] = "open-mpc-manager-reusable-v1\n";
constexpr const char preparationToken[] = "open-mpc-manager-prepare-v1\n";
constexpr const char readyToken[] = "READY\n";
constexpr const char storageToken[] = "open-mpc-manager-storage-v1\n";
constexpr const char storageSuffix[] = "/Plugin Manager USB";

bool firmwareMatches() {
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC) != 0) return false;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (posix_spawn_file_actions_init(&actions) != 0) { close(pipes[0]); close(pipes[1]); return false; }
    if (posix_spawnattr_init(&attributes) != 0) {
        posix_spawn_file_actions_destroy(&actions); close(pipes[0]); close(pipes[1]); return false;
    }
    sigset_t empty, defaults;
    sigemptyset(&empty); sigfillset(&defaults); sigdelset(&defaults, SIGKILL); sigdelset(&defaults, SIGSTOP);
    int error = posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    error |= posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    error |= posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    error |= posix_spawnattr_setsigmask(&attributes, &empty);
    error |= posix_spawnattr_setsigdefault(&attributes, &defaults);
    error |= posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    char executable[] = "/usr/bin/busybox.nosuid", command[] = "sha256sum", file[] = "/usr/bin/MPC";
    char lang[] = "LANG=C", path[] = "PATH=/usr/bin:/bin";
    char *arguments[] = {executable, command, file, nullptr}, *environment[] = {lang, path, nullptr};
    pid_t child = -1;
    if (!error) error = posix_spawn(&child, executable, &actions, &attributes, arguments, environment);
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes); close(pipes[1]);
    if (error) { close(pipes[0]); return false; }
    std::string output;
    bool finished = false;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd input{pipes[0], POLLIN | POLLHUP, 0};
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        const int ready = poll(&input, 1, static_cast<int>(left > 0 ? left : 0));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        char bytes[256];
        const ssize_t count = read(pipes[0], bytes, sizeof bytes);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 || output.size() + static_cast<size_t>(count) > sizeof bytes) break;
        output.append(bytes, static_cast<size_t>(count));
        if (count == 0) {
            const pid_t result = waitpid(child, &status, WNOHANG);
            if (result == child) { finished = true; break; }
            if (result < 0 && errno != EINTR) break;
            poll(nullptr, 0, 1);
        }
    }
    close(pipes[0]);
    if (!finished) { kill(child, SIGKILL); while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {} }
    return finished && WIFEXITED(status) && WEXITSTATUS(status) == 0
        && output == PMU_FIRMWARE_SHA256 "  /usr/bin/MPC\n";
}

struct Mount {
    std::string path, identity;
    unsigned long id = 0;
    dev_t device = 0;
    bool sd = false;
    bool operator==(const Mount &other) const {
        return path == other.path && identity == other.identity && id == other.id;
    }
};

bool usbDevice(const std::string &path, bool &device) {
    int fd = open((path + "/uevent").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    std::string text;
    char bytes[1024];
    ssize_t count;
    while ((count = read(fd, bytes, sizeof bytes)) != 0) {
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 || text.size() + static_cast<size_t>(count) > 4096) { close(fd); return false; }
        text.append(bytes, static_cast<size_t>(count));
    }
    close(fd);
    text.insert(0, 1, '\n');
    device = text.find("\nDEVTYPE=usb_device\n") != std::string::npos;
    return device || text.find("\nDEVTYPE=usb_interface\n") != std::string::npos;
}

struct UsbAttachment {
    std::string path;
    dev_t device;
    ino_t inode;
    int fd;
};

struct UsbBaseline {
    std::vector<UsbAttachment> attachments;
    std::string subsystem;
    bool valid = false;

    ~UsbBaseline() {
        for (const auto &attachment : attachments) close(attachment.fd);
    }

    void capture() {
        char resolved[4096];
        if (!realpath("/sys/bus/usb", resolved)) return;
        subsystem = resolved;
        DIR *directory = opendir("/sys/bus/usb/devices");
        if (!directory) return;
        bool complete = true;
        while (complete) {
            errno = 0;
            const auto *entry = readdir(directory);
            if (!entry) { complete = errno == 0; break; }
            if (entry->d_name[0] == '.') continue;
            const auto path = std::string("/sys/bus/usb/devices/") + entry->d_name;
            bool device = false;
            if (!realpath(path.c_str(), resolved) || !usbDevice(resolved, device)) { complete = false; break; }
            if (!device) continue;
            int fd = open(resolved, O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            struct stat st{};
            if (fd < 0 || fstat(fd, &st) != 0) {
                if (fd >= 0) close(fd);
                complete = false;
                break;
            }
            // Retain the inode so a replacement at the same USB port cannot reuse its identity.
            attachments.push_back({resolved, st.st_dev, st.st_ino, fd});
        }
        closedir(directory);
        valid = complete;
    }

    bool newlyAttached(dev_t device) {
        if (!valid) return false;
        const auto block = "/sys/dev/block/" + std::to_string(major(device)) + ':' + std::to_string(minor(device));
        char resolved[4096];
        if (!realpath(block.c_str(), resolved)) return false;
        std::string path = resolved;
        while (path.size() > 1) {
            if (realpath((path + "/subsystem").c_str(), resolved)) {
                if (subsystem == resolved) {
                    bool usb = false;
                    if (!usbDevice(path, usb)) return false;
                    if (usb) {
                        int fd = open(path.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                        struct stat st{};
                        if (fd < 0) return false;
                        const bool identified = fstat(fd, &st) == 0;
                        if (!identified) { close(fd); return false; }
                        for (auto &attachment : attachments) {
                            if (attachment.path != path) continue;
                            if (attachment.device == st.st_dev && attachment.inode == st.st_ino) {
                                close(fd);
                                return false;
                            }
                            close(attachment.fd);
                            attachment = {path, st.st_dev, st.st_ino, fd};
                            return true;
                        }
                        attachments.push_back({path, st.st_dev, st.st_ino, fd});
                        return true;
                    }
                }
            } else if (errno != ENOENT) return false;
            path.resize(path.rfind('/'));
        }
        return false;
    }
};

bool exactFile(const std::string &path, const char *bytes, size_t length, bool owned) {
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st{};
    char buffer[64];
    const bool valid = length < sizeof buffer && fstat(fd, &st) == 0 && S_ISREG(st.st_mode)
        && st.st_size == static_cast<off_t>(length)
        && (!owned || (st.st_uid == 0 && (!(st.st_mode & 0022) || pmu_helper_exfat(fd)) && st.st_nlink == 1))
        && read(fd, buffer, length + 1) == static_cast<ssize_t>(length)
        && memcmp(bytes, buffer, length) == 0;
    close(fd);
    return valid;
}

bool enabledSource() {
    struct stat st{};
    int fd = open(source, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    const bool valid = fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == 0
        && ((st.st_mode & 0722) == 0700 || pmu_helper_exfat(fd));
    close(fd);
    return valid && exactFile(std::string(source) + "/manager-enabled", enabled, sizeof enabled - 1, true);
}

bool unescape(const std::string &text, std::string &out) {
    out.clear();
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\\') {
            if (i + 3 >= text.size()) return false;
            const auto code = text.substr(i + 1, 3);
            if (code == "040") c = ' ';
            else if (code == "134") c = '\\';
            else return false;
            i += 3;
        }
        if (static_cast<unsigned char>(c) < 32 || c == 127) return false;
        out += c;
    }
    return out.size() < 4096;
}

std::string identity(const struct stat &root, const struct stat &file) {
    return std::to_string(root.st_dev) + ':' + std::to_string(root.st_ino) + ':'
        + std::to_string(file.st_dev) + ':' + std::to_string(file.st_ino);
}

bool managerSocket(const Mount &, std::string &);
bool contactManager(const std::string &, char);
bool ownedText(const std::string &, std::string &);

bool configuredDevice(dev_t &device) {
    std::string text;
    constexpr const char capability[] = "open-mpc-manager-sd-v1\n";
    if (!exactFile(std::string(source) + "/sd-install-capability", capability, sizeof capability - 1, true)
        || !ownedText(std::string(source) + "/installed-storage", text)
        || text.compare(0, sizeof storageToken - 1, storageToken) != 0 || text.back() != '\n') return false;
    const auto uuid = text.substr(sizeof storageToken - 1, text.size() - sizeof storageToken);
    if (uuid.empty() || uuid.size() > 64
        || uuid.find_first_not_of("0123456789abcdefABCDEF-") != std::string::npos) return false;
    struct stat block{}, internal{};
    if (stat(("/dev/disk/by-uuid/" + uuid).c_str(), &block) != 0 || !S_ISBLK(block.st_mode)) return false;
    for (const char *path : {"/", "/data", "/sdcard", "/nvme", "/storage", "/media/az01-internal"})
        if (stat(path, &internal) == 0 && block.st_rdev == internal.st_dev) return false;
    device = block.st_rdev;
    return true;
}

bool discover(int fd, Mount &result, Mount *usbCandidate = nullptr, Mount *sdCandidate = nullptr) {
    if (usbCandidate) *usbCandidate = {};
    if (sdCandidate) *sdCandidate = {};
    if (lseek(fd, 0, SEEK_SET) < 0) return false;
    std::string content;
    char buffer[4096];
    ssize_t count;
    while ((count = read(fd, buffer, sizeof buffer)) != 0) {
        if (count < 0) { if (errno == EINTR) continue; return false; }
        content.append(buffer, static_cast<size_t>(count));
        if (content.size() > 1024 * 1024) return false;
    }
    result = {};
    dev_t installedDevice = 0;
    const bool installed = configuredDevice(installedDevice);
    std::vector<Mount> usb, sd;
    unsigned installedMounts = 0;
    std::istringstream lines(content);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        unsigned long id, parent;
        std::string device, root, point, options, decoded;
        if (!(fields >> id >> parent >> device >> root >> point >> options)) return false;
        if (!unescape(point, decoded) || decoded.compare(0, 7, "/media/") != 0
            || decoded.size() == 7 || decoded.find('/', 7) != std::string::npos) continue;
        struct stat volume{};
        if (lstat(decoded.c_str(), &volume) != 0 || !S_ISDIR(volume.st_mode)) continue;
        const bool selected = installed && volume.st_dev == installedDevice;
        if (selected && ++installedMounts > 1) return false;
        for (int kind = 0; kind < (selected ? 2 : 1); ++kind) {
            const std::string path = decoded + (kind ? storageSuffix : "");
            struct stat directory{}, file{};
            char resolved[4096];
            const auto markerPath = path + "/.mpc-manager-id";
            if (!exactFile(markerPath, marker, sizeof marker - 1, false)) continue;
            if (!realpath(path.c_str(), resolved) || path != resolved
                || lstat(path.c_str(), &directory) || !S_ISDIR(directory.st_mode)
                || directory.st_dev != volume.st_dev
                || lstat(markerPath.c_str(), &file) || !S_ISREG(file.st_mode)
                || file.st_dev != directory.st_dev) return false;
            const auto expectedDevice = std::to_string(major(directory.st_dev)) + ':'
                + std::to_string(minor(directory.st_dev));
            if (device != expectedDevice) return false;
            (kind ? sd : usb).push_back({path, identity(directory, file), id, directory.st_dev, kind != 0});
        }
    }
    if (usb.size() > 1 || sd.size() > 1) return false;
    if (usbCandidate && !usb.empty()) *usbCandidate = usb.front();
    if (sdCandidate && !sd.empty()) *sdCandidate = sd.front();
    std::string socket;
    for (const auto *candidates : {&usb, &sd})
        for (const auto &candidate : *candidates)
            if (managerSocket(candidate, socket) && contactManager(socket, 0)) {
                if (!result.path.empty()) return false;
                result = candidate;
            }
    if (!result.path.empty()) return true;
    if (!usb.empty()) result = usb.front();
    else if (!sd.empty()) result = sd.front();
    return true;
}

bool sessionBase(const std::string &base) {
    const std::string prefix = "/tmp/mpc-usb-session.";
    struct stat st{};
    return base.compare(0, prefix.size(), prefix) == 0 && base.size() > prefix.size()
        && base.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-", 5) == std::string::npos
        && lstat(base.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == 0
        && (st.st_mode & 07777) == 0700;
}

bool managerSocket(const Mount &mount, std::string &socketPath) {
    int fd = open(statePath, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st{};
    char buffer[8192];
    const bool safe = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0
        && (st.st_mode & 07777) == 0600 && st.st_nlink == 1
        && st.st_size > 0 && st.st_size < static_cast<off_t>(sizeof buffer);
    const ssize_t count = safe ? read(fd, buffer, sizeof buffer) : -1;
    close(fd);
    if (count != st.st_size || count <= 0) return false;
    std::istringstream lines(std::string(buffer, static_cast<size_t>(count)));
    std::string base, usb, extra;
    if (!std::getline(lines, base) || !std::getline(lines, usb) || std::getline(lines, extra)) return false;
    if (usb != mount.path || !sessionBase(base) || !extra.empty()) return false;
    socketPath = base + "/shortcut.sock";
    return lstat(socketPath.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)
        && st.st_uid == 0 && (st.st_mode & 07777) == 0600;
}

bool contactManager(const std::string &path, char command) {
    sockaddr_un target{};
    target.sun_family = AF_UNIX;
    if (path.size() >= sizeof target.sun_path) return false;
    memcpy(target.sun_path, path.c_str(), path.size() + 1);
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    bool success = connect(fd, reinterpret_cast<sockaddr *>(&target), sizeof target) == 0;
    if (success && command) success = send(fd, &command, 1, MSG_NOSIGNAL | MSG_DONTWAIT) == 1;
    close(fd);
    return success;
}

struct Session;
struct Prompt {
    Session *session;
    unsigned generation;
    PmuUiHandle *handle = nullptr;
};
struct PendingUi {
    bool present = false, show = false, due = false;
    unsigned generation = 0, delayMs = 100;
    std::chrono::steady_clock::time_point deadline;
};
struct Session {
    PmuUi *ui = nullptr;
    pthread_t thread{};
    bool started = false;
    int wake = -1, timer = -1, loadingTimer = -1, preparationTimer = -1, preparationWatch = -1, events = -1;
    std::atomic<bool> stopping{false}, unloading{false}, loadingWanted{false};
    std::atomic<unsigned> generation{1}, answeredGeneration{0};
    std::atomic<int> answer{-1};
    Prompt *prompt = nullptr;
    PmuUiLoadingHandle *loading = nullptr;
    PendingUi pending;
    unsigned rejectedGeneration = 0, showFailureGeneration = 0;
    std::string request, preparedBase, cancelledBase;
    dev_t preparationDevice = 0;
    ino_t requestInode = 0, preparationDirectoryInode = 0;
    Mount preparationMount;
    bool preparationReady = false;
};
Session *session = nullptr;

void wake(Session *s) {
    const uint64_t one = 1;
    if (s->wake >= 0) (void)write(s->wake, &one, sizeof one);
}
void answer(int result, void *data) {
    auto *p = static_cast<Prompt *>(data);
    auto *s = p->session;
    if (s->stopping || s->generation != p->generation) return;
    if (result == 1) {
        if (!s->loading) s->loading = pmu_ui_loading_begin(s->ui, "Opening Plugin Manager USB");
        if (!s->loading) {
            std::fprintf(stderr, "Plugin manager helper: loading popup unavailable; continuing launch\n");
        } else if (s->stopping || s->generation != p->generation) {
            pmu_ui_loading_end(s->ui, s->loading);
            s->loading = nullptr;
            return;
        } else s->loadingWanted = true;
    }
    s->answeredGeneration = p->generation;
    s->answer.store(result == 1 ? 1 : 0, std::memory_order_release);
    wake(s);
}
void dismiss(Session *s) {
    if (!s->prompt) return;
    pmu_ui_dismiss(s->ui, s->prompt->handle);
    delete s->prompt;
    s->prompt = nullptr;
}
struct UiTask { Session *session; unsigned generation; bool show; };
void uiTask(void *data) {
    auto *task = static_cast<UiTask *>(data);
    auto *s = task->session;
    if (s->loading && (s->stopping || !s->loadingWanted)) {
        pmu_ui_loading_end(s->ui, s->loading);
        s->loading = nullptr;
    }
    if (s->prompt && (s->stopping || s->prompt->generation != s->generation
        || (!task->show && s->prompt->generation == task->generation))) dismiss(s);
    if (!task->show || s->stopping || task->generation != s->generation || s->prompt
        || s->answeredGeneration == task->generation) return;
    auto *p = new Prompt{s, task->generation};
    p->handle = pmu_ui_show(s->ui, "MPC PLUGIN INSTALLER",
        "Have you saved your project?\n\nOpening the installer stops the MPC application. Your session restarts when you return.", answer, p);
    if (!p->handle) {
        if (s->showFailureGeneration != task->generation) {
            s->showFailureGeneration = task->generation;
            std::fprintf(stderr, "Plugin manager helper: dialog unavailable for generation %u\n", task->generation);
        }
        delete p;
        return;
    }
    s->prompt = p;
}
bool postUi(Session *s, unsigned generation, bool show) {
    auto *task = new UiTask{s, generation, show};
    if (pmu_ui_post(s->ui, uiTask, task, [](void *p) { delete static_cast<UiTask *>(p); })) return true;
    delete task;
    return false;
}
void disarmUi(Session *s) {
    if (s->timer < 0) return;
    itimerspec timer{};
    if (timerfd_settime(s->timer, 0, &timer, nullptr) != 0) throw 1;
    uint64_t expired;
    while (read(s->timer, &expired, sizeof expired) == sizeof expired) {}
}
void clearUi(Session *s) {
    s->pending = {};
    disarmUi(s);
}
void requestUi(Session *s, bool show) {
    const unsigned generation = s->generation;
    if (show && s->answeredGeneration == generation) show = false;
    if (s->pending.present && s->pending.generation == generation && s->pending.show == show) return;
    disarmUi(s);
    s->pending = {true, show, true, generation, 100,
                  std::chrono::steady_clock::now() + std::chrono::seconds(10)};
}
void armLoading(Session *s, unsigned seconds) {
    if (s->loadingTimer < 0) return;
    itimerspec timer{};
    if (timerfd_settime(s->loadingTimer, 0, &timer, nullptr) != 0) throw 1;
    uint64_t expired;
    while (read(s->loadingTimer, &expired, sizeof expired) == sizeof expired) {}
    timer.it_value.tv_sec = s->loadingWanted ? seconds : 0;
    if (seconds && timerfd_settime(s->loadingTimer, 0, &timer, nullptr) != 0) throw 1;
}
void clearLoading(Session *s) {
    s->loadingWanted = false;
    armLoading(s, 0);
    requestUi(s, false);
}
void retryUi(Session *s) {
    auto &pending = s->pending;
    if (!pending.present || !pending.due) return;
    const auto now = std::chrono::steady_clock::now();
    if (s->stopping || pending.generation != s->generation || now >= pending.deadline) {
        clearUi(s);
        return;
    }
    if (pending.show && s->answeredGeneration == pending.generation) requestUi(s, false);
    if (postUi(s, pending.generation, pending.show)) {
        clearUi(s);
        return;
    }
    if (s->rejectedGeneration != pending.generation) {
        s->rejectedGeneration = pending.generation;
        std::fprintf(stderr, "Plugin manager helper: UI post rejected for generation %u\n", pending.generation);
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(pending.deadline
        - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) { clearUi(s); return; }
    const auto delay = std::min<int64_t>(pending.delayMs, remaining);
    itimerspec timer{};
    timer.it_value.tv_sec = delay / 1000;
    timer.it_value.tv_nsec = (delay % 1000) * 1000000;
    if (timerfd_settime(s->timer, 0, &timer, nullptr) != 0) throw 1;
    pending.delayMs = std::min(pending.delayMs * 2, 500U);
    pending.due = false;
}
bool ownedDirectory(const std::string &path) {
    struct stat st{};
    return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == 0
        && (st.st_mode & 07777) == 0700;
}
bool ownedText(const std::string &path, std::string &text) {
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st{};
    char bytes[256];
    const bool safe = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0
        && ((st.st_mode & 07777) == 0600 || (path.compare(0, strlen(source) + 1, std::string(source) + "/") == 0 && pmu_helper_exfat(fd)))
        && st.st_nlink == 1 && st.st_size > 0 && st.st_size < static_cast<off_t>(sizeof bytes);
    const ssize_t count = safe ? read(fd, bytes, sizeof bytes) : -1;
    close(fd);
    if (count <= 0 || count != st.st_size) return false;
    text.assign(bytes, static_cast<size_t>(count));
    return true;
}
bool preparationRequest(Session *s) {
    if (s->request.empty()) return false;
    const auto directory = s->request.substr(0, s->request.rfind('/'));
    struct stat dir{}, file{};
    std::string text;
    return ownedDirectory(directory) && lstat(directory.c_str(), &dir) == 0
        && dir.st_dev == s->preparationDevice && dir.st_ino == s->preparationDirectoryInode
        && lstat(s->request.c_str(), &file) == 0 && file.st_dev == s->preparationDevice
        && file.st_ino == s->requestInode && ownedText(s->request, text) && text == preparationToken;
}
bool rememberPreparation(Session *s) {
    if (!preparationRequest(s)) return false;
    const auto directory = s->request.substr(0, s->request.rfind('/'));
    std::string text;
    if (!ownedText(directory + "/session", text) || text.back() != '\n'
        || text.find('\n') != text.size() - 1) return false;
    const auto base = text.substr(0, text.size() - 1);
    if (!sessionBase(base) || (!s->preparedBase.empty() && s->preparedBase != base)) return false;
    s->preparedBase = base;
    return true;
}
bool preparedSocket(Session *s, std::string &socket) {
    return !s->preparedBase.empty() && managerSocket(s->preparationMount, socket)
        && socket == s->preparedBase + "/shortcut.sock";
}
bool preparationReady(Session *s) {
    if (s->request.empty()) return false;
    const auto directory = s->request.substr(0, s->request.rfind('/'));
    std::string ready, socket;
    if (!rememberPreparation(s) || !ownedText(directory + "/ready", ready) || ready.back() != '\n'
        || ready.find('\n') != ready.size() - 1 || !managerSocket(s->preparationMount, socket)) return false;
    const auto base = ready.substr(0, ready.size() - 1);
    std::string initialized;
    if (socket != base + "/shortcut.sock" || !ownedText(base + "/ready", initialized)
        || initialized != readyToken || !contactManager(socket, 0)) return false;
    if (!s->preparedBase.empty() && s->preparedBase != base) return false;
    s->preparedBase = base;
    return true;
}
void finishPreparation(Session *s, bool cancel) {
    if (s->request.empty()) return;
    const auto directory = s->request.substr(0, s->request.rfind('/'));
    if (cancel && s->preparedBase.empty()) (void)rememberPreparation(s);
    unlink(s->request.c_str());
    if (cancel) {
        std::string socket;
        if (!s->preparedBase.empty()) s->cancelledBase = s->preparedBase;
        if (preparedSocket(s, socket)) (void)contactManager(socket, 'Q');
    }
    if (s->preparationWatch >= 0) inotify_rm_watch(s->events, s->preparationWatch);
    s->preparationWatch = -1;
    itimerspec timer{};
    if (s->preparationTimer >= 0) (void)timerfd_settime(s->preparationTimer, 0, &timer, nullptr);
    unlink((directory + "/session").c_str());
    unlink((directory + "/ready").c_str());
    unlink((directory + "/failed").c_str());
    rmdir(directory.c_str());
    s->request.clear();
    s->preparedBase.clear();
    s->preparationMount = {};
    s->preparationReady = false;
}
void invalidate(Session *s, bool keepLoading = false) {
    pmu_panel_stop();
    ++s->generation;
    s->answer = -1;
    finishPreparation(s, true);
    if (keepLoading) requestUi(s, false);
    else clearLoading(s);
}
bool prepareManager(Session *s, const Mount &mount) {
    struct stat st{};
    if (lstat(statePath, &st) == 0 || errno != ENOENT) return false;
    char directory[] = "/run/open-mpc-manager-preparation.XXXXXX";
    if (!mkdtemp(directory)) return false;
    s->request = std::string(directory) + "/request";
    int fd = open(s->request.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat dir{}, request{};
    const bool valid = ownedDirectory(directory) && lstat(directory, &dir) == 0
        && fd >= 0 && fchmod(fd, 0600) == 0 && fstat(fd, &request) == 0
        && write(fd, preparationToken, sizeof preparationToken - 1) == static_cast<ssize_t>(sizeof preparationToken - 1);
    if (fd >= 0) close(fd);
    s->preparationDevice = dir.st_dev;
    s->preparationDirectoryInode = dir.st_ino;
    s->requestInode = request.st_ino;
    s->preparationWatch = valid ? inotify_add_watch(s->events, directory,
        IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF) : -1;
    itimerspec timer{};
    timer.it_value.tv_sec = 75;
    if (s->preparationWatch < 0 || timerfd_settime(s->preparationTimer, 0, &timer, nullptr) != 0) {
        finishPreparation(s, true);
        return false;
    }
    s->preparationMount = mount;
    pmu_prepare_manager(mount.path.c_str(), mount.identity.c_str(), s->request.c_str());
    return true;
}

bool absent(const std::string &path) {
    struct stat st{};
    return lstat(path.c_str(), &st) != 0 && errno == ENOENT;
}
bool managerServiceIdle() {
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC | O_NONBLOCK) != 0) return false;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) { close(pipes[0]); close(pipes[1]); return false; }
    int error = posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    error |= posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    error |= posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    char executable[] = "systemctl", show[] = "show", service[] = "mpc-usb-manager.service";
    char properties[] = "-pLoadState", active[] = "-pActiveState", pid[] = "-pMainPID";
    char lang[] = "LANG=C", path[] = "PATH=/usr/bin:/bin";
    char *arguments[] = {executable, show, service, properties, active, pid, nullptr};
    char *environment[] = {lang, path, nullptr};
    pid_t child = -1;
    if (!error) error = posix_spawnp(&child, executable, &actions, nullptr, arguments, environment);
    posix_spawn_file_actions_destroy(&actions);
    close(pipes[1]);
    if (error) { close(pipes[0]); return false; }
    std::string output;
    int status = 0;
    bool finished = false, valid = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (valid && std::chrono::steady_clock::now() < deadline) {
        char bytes[256];
        const ssize_t count = read(pipes[0], bytes, sizeof bytes);
        if (count > 0) {
            if (output.size() + static_cast<size_t>(count) > sizeof bytes) { valid = false; break; }
            output.append(bytes, static_cast<size_t>(count));
        } else if (count < 0 && errno != EINTR && errno != EAGAIN) { valid = false; break; }
        if (!finished) {
            const pid_t result = waitpid(child, &status, WNOHANG);
            if (result == child) finished = true;
            else if (result < 0 && errno != EINTR) { valid = false; break; }
        }
        if (finished && count == 0) break;
        pollfd input{pipes[0], POLLIN | POLLHUP, 0};
        (void)poll(&input, 1, 10);
    }
    close(pipes[0]);
    if (!finished) { kill(child, SIGKILL); while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {} }
    if (!valid || !finished || !WIFEXITED(status) || (WEXITSTATUS(status) != 0 && WEXITSTATUS(status) != 4)) return false;
    std::istringstream lines(output);
    std::string line;
    bool load = false, activeState = false, mainPid = false;
    while (std::getline(lines, line)) {
        if (line == "LoadState=not-found" && !load) load = true;
        else if (line == "ActiveState=inactive" && !activeState) activeState = true;
        else if (line == "MainPID=0" && !mainPid) mainPid = true;
        else return false;
    }
    return load && activeState && mainPid;
}
bool retired(const std::string &base) {
    if ((!base.empty() && !absent(base)) || !absent(statePath)
        || !absent("/run/systemd/system/acvs.service.d/99-mpc-usb-shortcut.conf")
        || !absent("/run/systemd/system/inmusic-mpc.service.d/99-mpc-usb-shortcut.conf")) return false;
    int fd = open("/run/mpc-usb-manager.lock", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT && managerServiceIdle();
    const bool unlocked = flock(fd, LOCK_EX | LOCK_NB) == 0;
    close(fd);
    return unlocked && managerServiceIdle();
}

void *observe(void *data) {
    auto *s = static_cast<Session *>(data);
    int mounts = -1, events = -1, usbWatch = -1, tmpWatch = -1, offerWatch = -1;
    try {
        UsbBaseline baseline;
        baseline.capture();
        mounts = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
        events = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        s->events = events;
        s->preparationTimer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        s->timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        s->loadingTimer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        const int sourceWatch = events < 0 ? -1 : inotify_add_watch(events, source,
            IN_DELETE_SELF | IN_MOVE_SELF | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE);
        if (mounts < 0 || events < 0 || sourceWatch < 0 || s->timer < 0
            || s->loadingTimer < 0 || s->preparationTimer < 0) throw 1;
        Mount current;
        bool scan = true, force = false, waitForCancellation = false;
        bool preparationChanged = false, cancellationChanged = false;
        bool promptCurrent = false;
        enum class Offer { None, Consent, Retiring, Preparing, Rollback, Restoring };
        Offer offer = Offer::None;
        Mount offered, retainedSd, seenUsb;
        std::string retiringBase;
        bool retainSd = false, offerValid = false;
        std::chrono::steady_clock::time_point retirementDeadline;
        auto tickRetirement = [&] {
            itimerspec timer{};
            timer.it_value.tv_nsec = 100000000;
            if (timerfd_settime(s->preparationTimer, 0, &timer, nullptr) != 0) throw 1;
        };
        auto clearOfferWatch = [&] {
            if (offerWatch >= 0) inotify_rm_watch(events, offerWatch);
            offerWatch = -1;
        };
        auto restoreSd = [&] {
            invalidate(s);
            clearOfferWatch();
            offer = Offer::Rollback;
            retirementDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            tickRetirement();
        };
        while (!s->stopping) {
            if (scan) {
                Mount found, usb, sd;
                const bool valid = enabledSource() && discover(mounts, found, &usb, &sd);
                if (!valid) { found = {}; usb = {}; sd = {}; }
                offerValid = !offered.path.empty() && offered == usb;
                if (offer == Offer::Consent && (!offerValid || !(sd == retainedSd))) {
                    invalidate(s);
                    clearOfferWatch();
                    offer = Offer::None;
                    std::string socket;
                    if (sd == retainedSd && managerSocket(retainedSd, socket) && contactManager(socket, 0))
                        (void)pmu_arm_current_panel(socket.c_str());
                }
                if (offer == Offer::Preparing && (!offerValid || !(sd == retainedSd))) restoreSd();
                if (offer == Offer::Restoring && !(sd == retainedSd)) {
                    invalidate(s);
                    offer = Offer::None;
                    retainSd = false;
                }
                if (retainSd && sd == retainedSd && !sd.path.empty()) found = sd;
                else if (retainSd) retainSd = false;
                const bool managedOffer = offer == Offer::Retiring || offer == Offer::Preparing
                    || offer == Offer::Rollback || offer == Offer::Restoring;
                if (!managedOffer && (force || !(found == current))) {
                    std::string previousSocket;
                    if (!(found == current) && !current.path.empty() && managerSocket(current, previousSocket))
                        s->cancelledBase = previousSocket.substr(0, previousSocket.rfind('/'));
                    invalidate(s);
                    if (usbWatch >= 0) inotify_rm_watch(events, usbWatch);
                    usbWatch = -1;
                    if (tmpWatch >= 0) inotify_rm_watch(events, tmpWatch);
                    tmpWatch = -1;
                    current = found;
                    waitForCancellation = false;
                    if (!found.path.empty()) {
                        usbWatch = inotify_add_watch(events, found.path.c_str(),
                            IN_UNMOUNT | IN_DELETE_SELF | IN_MOVE_SELF | IN_DELETE | IN_MOVED_FROM);
                        Mount checked;
                        if (usbWatch < 0 || !discover(mounts, checked) || !(checked == found)) {
                            current = {};
                        } else {
                            promptCurrent = !current.sd && baseline.newlyAttached(current.device);
                            struct stat cancelled{};
                            waitForCancellation = !s->cancelledBase.empty()
                                && lstat(s->cancelledBase.c_str(), &cancelled) == 0;
                            if (waitForCancellation) {
                                tmpWatch = inotify_add_watch(events, "/tmp", IN_DELETE | IN_MOVED_FROM);
                                if (tmpWatch < 0) throw 1;
                                cancellationChanged = true;
                            } else {
                                std::string socket;
                                if (!managerSocket(current, socket) || !contactManager(socket, 0))
                                    preparationChanged = prepareManager(s, current);
                                else (void)pmu_arm_current_panel(socket.c_str());
                            }
                        }
                    }
                }
                if (!usb.path.empty() && !(usb == seenUsb)) {
                    seenUsb = usb;
                    const bool fresh = baseline.newlyAttached(usb.device);
                    std::string socket;
                    if (offer == Offer::None && current.sd && sd == current && fresh
                        && s->request.empty() && managerSocket(current, socket) && contactManager(socket, 0)) {
                        retainedSd = current;
                        offered = usb;
                        offerValid = true;
                        offerWatch = inotify_add_watch(events, offered.path.c_str(),
                            IN_UNMOUNT | IN_DELETE_SELF | IN_MOVE_SELF | IN_DELETE | IN_MOVED_FROM);
                        if (offerWatch >= 0) {
                            ++s->generation;
                            s->answer = -1;
                            offer = Offer::Consent;
                            requestUi(s, true);
                        }
                    }
                } else if (usb.path.empty()) seenUsb = {};
                scan = false;
                force = false;
            }
            if (offer == Offer::Retiring || offer == Offer::Rollback) {
                Mount selected, usb, sd;
                const bool valid = enabledSource() && discover(mounts, selected, &usb, &sd);
                offerValid = valid && usb == offered && !offered.path.empty();
                const bool sdValid = valid && sd == retainedSd && !retainedSd.path.empty();
                const auto base = offer == Offer::Retiring ? retiringBase : s->cancelledBase;
                if (retired(base)) {
                    if (offer == Offer::Retiring && offerValid && sdValid) {
                        invalidate(s, true);
                        current = offered;
                        promptCurrent = false;
                        offer = Offer::Preparing;
                        preparationChanged = prepareManager(s, current);
                        if (!preparationChanged) restoreSd();
                    } else if (sdValid) {
                        clearLoading(s);
                        current = retainedSd;
                        retainSd = true;
                        offer = Offer::Restoring;
                        promptCurrent = false;
                        preparationChanged = prepareManager(s, current);
                        if (!preparationChanged) offer = Offer::None;
                    } else {
                        clearLoading(s);
                        offer = Offer::None;
                        scan = true;
                    }
                } else if (std::chrono::steady_clock::now() >= retirementDeadline) {
                    clearLoading(s);
                    std::string socket;
                    if (sdValid && managerSocket(retainedSd, socket) && contactManager(socket, 0)) {
                        current = retainedSd;
                        retainSd = true;
                        (void)pmu_arm_current_panel(socket.c_str());
                    }
                    offer = Offer::None;
                    clearOfferWatch();
                    itimerspec timer{};
                    if (timerfd_settime(s->preparationTimer, 0, &timer, nullptr) != 0) throw 1;
                } else tickRetirement();
            }
            if (waitForCancellation && cancellationChanged) {
                cancellationChanged = false;
                struct stat state{};
                if (lstat(s->cancelledBase.c_str(), &state) != 0 && errno == ENOENT) {
                    waitForCancellation = false;
                    inotify_rm_watch(events, tmpWatch);
                    tmpWatch = -1;
                    s->cancelledBase.clear();
                    preparationChanged = prepareManager(s, current);
                }
            }
            if (preparationChanged && !s->request.empty()) {
                preparationChanged = false;
                (void)rememberPreparation(s);
                const auto directory = s->request.substr(0, s->request.rfind('/'));
                struct stat failed{};
                if (!preparationRequest(s) || lstat((directory + "/failed").c_str(), &failed) == 0
                    || (s->preparationReady && !preparationReady(s))) {
                    if (offer == Offer::Preparing) restoreSd();
                    else { invalidate(s); if (offer == Offer::Restoring) offer = Offer::None; }
                } else if (!s->preparationReady && preparationReady(s)) {
                    s->preparationReady = true;
                    itimerspec timer{};
                    if (timerfd_settime(s->preparationTimer, 0, &timer, nullptr) != 0) throw 1;
                    if (offer == Offer::Preparing) {
                        Mount selected, usb, sd;
                        std::string socket;
                        const bool opened = enabledSource() && discover(mounts, selected, &usb, &sd)
                            && usb == offered && sd == retainedSd && preparedSocket(s, socket)
                            && contactManager(socket, 'O');
                        if (opened) {
                            armLoading(s, 10);
                            finishPreparation(s, false);
                            offer = Offer::None;
                            retainSd = false;
                            clearOfferWatch();
                            if (usbWatch >= 0) inotify_rm_watch(events, usbWatch);
                            usbWatch = inotify_add_watch(events, current.path.c_str(),
                                IN_UNMOUNT | IN_DELETE_SELF | IN_MOVE_SELF | IN_DELETE | IN_MOVED_FROM);
                        } else restoreSd();
                    } else if (promptCurrent) requestUi(s, true);
                    else {
                        std::string socket;
                        const bool armed = preparedSocket(s, socket) && pmu_arm_current_panel(socket.c_str());
                        finishPreparation(s, !armed);
                        if (offer == Offer::Restoring) {
                            offer = Offer::None;
                            clearOfferWatch();
                        }
                    }
                }
            }
            retryUi(s);
            pollfd wait[] = {{mounts, POLLPRI, 0}, {events, POLLIN, 0}, {s->wake, POLLIN, 0}, {s->timer, POLLIN, 0}, {s->preparationTimer, POLLIN, 0}, {s->loadingTimer, POLLIN, 0}};
            int ready;
            do { ready = poll(wait, 6, -1); } while (ready < 0 && errno == EINTR);
            if (ready < 0) throw 1;
            if (wait[0].revents & (POLLPRI | POLLERR)) scan = true;
            if (wait[0].revents & (POLLHUP | POLLNVAL)) throw 1;
            if (wait[1].revents) {
                alignas(inotify_event) char buffer[4096];
                ssize_t count;
                while ((count = read(events, buffer, sizeof buffer)) > 0) {
                    for (ssize_t offset = 0; offset < count;) {
                        const auto *event = reinterpret_cast<const inotify_event *>(buffer + offset);
                        if (event->mask & IN_Q_OVERFLOW) {
                            force = true; scan = true; preparationChanged = true; cancellationChanged = true;
                        }
                        if (event->wd == s->preparationWatch) preparationChanged = true;
                        if (event->wd == offerWatch) scan = true;
                        if (event->wd == tmpWatch && event->len
                            && s->cancelledBase.substr(5) == event->name) cancellationChanged = true;
                        if (event->wd == sourceWatch) {
                            if (event->len && strcmp(event->name, "installed-storage") == 0) {
                                force = true; scan = true;
                            }
                            if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED))
                                || (event->len && (strcmp(event->name, "manager-enabled") == 0
                                    || strcmp(event->name, "controller.ini") == 0
                                    || strcmp(event->name, "manager-entry.so") == 0))) s->stopping = true;
                        }
                        if (event->wd == usbWatch && ((event->mask & (IN_UNMOUNT | IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED))
                            || (event->len && strcmp(event->name, ".mpc-manager-id") == 0))) {
                            force = true; scan = true;
                        }
                        offset += sizeof(inotify_event) + event->len;
                    }
                }
            }
            if (wait[2].revents) {
                uint64_t value;
                while (read(s->wake, &value, sizeof value) == sizeof value) {}
            }
            if (wait[3].revents & (POLLERR | POLLHUP | POLLNVAL)) throw 1;
            if (wait[3].revents & POLLIN) {
                uint64_t expired;
                if (read(s->timer, &expired, sizeof expired) == sizeof expired) s->pending.due = true;
            }
            if (wait[4].revents & (POLLERR | POLLHUP | POLLNVAL)) throw 1;
            if (wait[4].revents & POLLIN) {
                uint64_t expired;
                if (read(s->preparationTimer, &expired, sizeof expired) == sizeof expired) {
                    if (offer == Offer::Preparing) restoreSd();
                    else if (offer != Offer::Retiring && offer != Offer::Rollback) {
                        invalidate(s);
                        if (offer == Offer::Restoring) offer = Offer::None;
                    }
                }
            }
            if (wait[5].revents & (POLLERR | POLLHUP | POLLNVAL)) throw 1;
            if (wait[5].revents & POLLIN) {
                uint64_t expired;
                if (read(s->loadingTimer, &expired, sizeof expired) == sizeof expired) {
                    if (offer == Offer::Retiring || offer == Offer::Preparing) restoreSd();
                    else clearLoading(s);
                }
            }
            if (s->stopping) break;
            if (force && offer != Offer::Retiring && offer != Offer::Rollback
                && offer != Offer::Preparing && offer != Offer::Restoring) invalidate(s);
            const int verdict = s->answer.exchange(-1, std::memory_order_acquire);
            if (verdict >= 0 && s->answeredGeneration == s->generation) {
                requestUi(s, false);
                if (verdict == 1) armLoading(s, 90);
                if (offer == Offer::Consent) {
                    Mount selected, usb, sd;
                    std::string socket;
                    const bool accepted = verdict == 1 && !force && enabledSource()
                        && discover(mounts, selected, &usb, &sd) && usb == offered && sd == retainedSd
                        && managerSocket(retainedSd, socket) && contactManager(socket, 0);
                    if (accepted) {
                        pmu_panel_stop();
                        retiringBase = socket.substr(0, socket.rfind('/'));
                        offer = Offer::Retiring;
                        retirementDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                        if (!contactManager(socket, 'H')) restoreSd();
                        else tickRetirement();
                    } else {
                        clearLoading(s);
                        offer = Offer::None;
                        clearOfferWatch();
                    }
                    continue;
                }
                Mount verified;
                std::string socket;
                const bool opened = verdict == 1 && !force && enabledSource() && discover(mounts, verified)
                    && !current.path.empty() && verified == current && s->preparationReady
                    && preparationReady(s) && preparedSocket(s, socket) && contactManager(socket, 'O');
                if (opened) armLoading(s, 10);
                else clearLoading(s);
                finishPreparation(s, !opened);
            }
        }
    } catch (...) {}
    s->stopping = true;
    try {
        invalidate(s);
        clearUi(s);
        if (!s->unloading) (void)postUi(s, s->generation, false);
    } catch (...) {}
    if (s->timer >= 0) { close(s->timer); s->timer = -1; }
    if (s->loadingTimer >= 0) { close(s->loadingTimer); s->loadingTimer = -1; }
    if (s->preparationTimer >= 0) { close(s->preparationTimer); s->preparationTimer = -1; }
    if (events >= 0) close(events);
    s->events = -1;
    if (mounts >= 0) close(mounts);
    return nullptr;
}

int mainImage(dl_phdr_info *info, size_t, void *result) {
    if (info->dlpi_name && info->dlpi_name[0] == 0) {
        *static_cast<uintptr_t *>(result) = info->dlpi_addr;
        return 1;
    }
    return 0;
}
}

extern "C" int pmu_start_manager(void *lua) {
    try {
        if (session || geteuid() != 0) return 0;
#if defined(__aarch64__)
        if (!selectSource()) return 0;
#endif
        if (!enabledSource()) return 0;
        char executable[64]{};
        if (readlink("/proc/self/exe", executable, sizeof executable - 1) != 12
            || strcmp(executable, "/usr/bin/MPC") != 0) return 0;
        if (!firmwareMatches()) {
            std::fprintf(stderr, "Plugin manager helper: firmware verification failed\n");
            return 0;
        }
        uintptr_t base = 0;
        dl_iterate_phdr(mainImage, &base);
        if (!base) return 0;
        PmuUi *ui = pmu_ui_init(lua, base);
        if (!ui) {
            std::fprintf(stderr, "Plugin manager helper: native UI binding unavailable\n");
            return 0;
        }
        auto *s = new Session;
        s->ui = ui;
        s->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (s->wake < 0) { delete s; return 0; }
        session = s;
        if (pthread_create(&s->thread, nullptr, observe, s) == 0) s->started = true;
        else {
            s->stopping = true; close(s->wake); s->wake = -1;
            std::fprintf(stderr, "Plugin manager helper: USB observer could not start\n");
        }
    } catch (...) {
        std::fprintf(stderr, "Plugin manager helper: initialization failed\n");
    }
    return 0;
}

extern "C" int pmu_stop_manager(void *) {
    auto *s = session;
    if (!s) return 0;
    s->unloading = true;
    pmu_panel_stop();
    s->loadingWanted = false;
    if (s->loading) {
        pmu_ui_loading_end(s->ui, s->loading);
        s->loading = nullptr;
    }
    pmu_ui_stop(s->ui);
    s->stopping = true;
    ++s->generation;
    wake(s);
    if (s->started) { pthread_join(s->thread, nullptr); s->started = false; }
    return 0;
}
