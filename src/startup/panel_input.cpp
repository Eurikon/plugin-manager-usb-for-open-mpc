#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "panel_input.h"
#include "shortcut_state.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
using Read = ssize_t (*)(void *, void *, size_t);
using Name = const char *(*)(void *);
using Sequence = uint64_t (*)(const char *, const char *);
pthread_mutex_t panelLock = PTHREAD_MUTEX_INITIALIZER;
Read originalRead;
Name originalName;
Sequence observedSequence;
void **readSlot;
int outputFd = -1;
unsigned generation;
char activeSocket[sizeof(sockaddr_un::sun_path)], activePort[256];
PanelMidi panelParser;
struct PermissionRepair { void *page; size_t size; int permissions; bool pending; } permissionRepair;

struct Mapping { uintptr_t begin, end; int permissions; };
bool mapping(uintptr_t address, size_t size, Mapping &result) {
    FILE *maps = fopen("/proc/self/maps", "re");
    if (!maps) return false;
    char line[4096], permissions[5];
    unsigned long begin, end;
    bool found = false;
    while (fgets(line, sizeof line, maps)) {
        if (sscanf(line, "%lx-%lx %4s", &begin, &end, permissions) != 3) continue;
        if (address < begin || address >= end || size > end - address) continue;
        result = {begin, end, (permissions[0] == 'r' ? PROT_READ : 0)
            | (permissions[1] == 'w' ? PROT_WRITE : 0) | (permissions[2] == 'x' ? PROT_EXEC : 0)};
        found = true;
        break;
    }
    fclose(maps);
    return found;
}
bool executable(void *pointer) {
    Mapping region{};
    return pointer && mapping(reinterpret_cast<uintptr_t>(pointer) & ~uintptr_t(1), 1, region)
        && (region.permissions & PROT_EXEC);
}
bool contains(const dl_phdr_info &image, uintptr_t address, size_t size, unsigned kind) {
    for (unsigned i = 0; i < image.dlpi_phnum; ++i) {
        const auto &segment = image.dlpi_phdr[i];
        const uintptr_t start = image.dlpi_addr + segment.p_vaddr;
        if (segment.p_type == kind && address >= start && address - start < segment.p_memsz
            && size <= segment.p_memsz - (address - start)) return true;
    }
    return false;
}
uintptr_t dynamicAddress(const dl_phdr_info &image, uintptr_t address, size_t size) {
    if (contains(image, address, size, PT_LOAD)) return address;
    if (address <= UINTPTR_MAX - image.dlpi_addr
        && contains(image, image.dlpi_addr + address, size, PT_LOAD)) return image.dlpi_addr + address;
    return 0;
}
struct Imports { void **read = nullptr, **name = nullptr; bool valid = false; };
template<class Relocation> bool imports(const dl_phdr_info &image, uintptr_t relocations, size_t bytes,
                                       uintptr_t symbols, const char *strings, size_t stringSize,
                                       Imports &result) {
    if (!bytes || bytes % sizeof(Relocation) || !contains(image, relocations, bytes, PT_LOAD)) return false;
    unsigned reads = 0, names = 0;
    for (size_t offset = 0; offset < bytes; offset += sizeof(Relocation)) {
        const auto &relocation = *reinterpret_cast<const Relocation *>(relocations + offset);
#if __SIZEOF_POINTER__ == 8
        const unsigned type = ELF64_R_TYPE(relocation.r_info);
        const size_t index = ELF64_R_SYM(relocation.r_info);
#else
        const unsigned type = ELF32_R_TYPE(relocation.r_info);
        const size_t index = ELF32_R_SYM(relocation.r_info);
#endif
#if defined(__arm__)
        if (type != R_ARM_JUMP_SLOT) continue;
#elif defined(__aarch64__)
        if (type != R_AARCH64_JUMP_SLOT) continue;
#elif defined(__x86_64__)
        if (type != R_X86_64_JUMP_SLOT) continue;
#else
#error Unsupported ELF import architecture
#endif
        if (index > (UINTPTR_MAX - symbols) / sizeof(ElfW(Sym))) return false;
        const uintptr_t location = symbols + index * sizeof(ElfW(Sym));
        if (!contains(image, location, sizeof(ElfW(Sym)), PT_LOAD)) return false;
        const auto &symbol = *reinterpret_cast<const ElfW(Sym) *>(location);
        if (symbol.st_name >= stringSize || !memchr(strings + symbol.st_name, 0, stringSize - symbol.st_name)) return false;
        const char *name = strings + symbol.st_name;
        const bool read = strcmp(name, "snd_rawmidi_read") == 0;
        if (!read && strcmp(name, "snd_rawmidi_name") != 0) continue;
        if (symbol.st_shndx != SHN_UNDEF || ELF32_ST_TYPE(symbol.st_info) != STT_FUNC) return false;
        const uintptr_t slot = image.dlpi_addr + relocation.r_offset;
        if (slot % alignof(void *) || !contains(image, slot, sizeof(void *), PT_LOAD)
            || !contains(image, slot, sizeof(void *), PT_GNU_RELRO)) return false;
        if (read) { ++reads; result.read = reinterpret_cast<void **>(slot); }
        else { ++names; result.name = reinterpret_cast<void **>(slot); }
    }
    return reads == 1 && names == 1;
}
int findImports(dl_phdr_info *image, size_t, void *data) {
    if (image->dlpi_name && *image->dlpi_name) return 0;
    auto &result = *static_cast<Imports *>(data);
    const ElfW(Dyn) *dynamic = nullptr;
    size_t count = 0;
    for (unsigned i = 0; i < image->dlpi_phnum; ++i) {
        const auto &segment = image->dlpi_phdr[i];
        if (segment.p_type != PT_DYNAMIC) continue;
        if (dynamic || segment.p_memsz % sizeof(ElfW(Dyn))) return 1;
        dynamic = reinterpret_cast<const ElfW(Dyn) *>(image->dlpi_addr + segment.p_vaddr);
        count = segment.p_memsz / sizeof(ElfW(Dyn));
    }
    if (!dynamic || !contains(*image, reinterpret_cast<uintptr_t>(dynamic), count * sizeof(*dynamic), PT_LOAD)) return 1;
    uintptr_t relocations = 0, symbols = 0, strings = 0;
    size_t bytes = 0, stringSize = 0, symbolSize = 0;
    unsigned relocationKind = 0;
    bool eager = false, ended = false;
    for (size_t i = 0; i < count; ++i) {
        const auto &entry = dynamic[i];
        if (entry.d_tag == DT_NULL) { ended = true; break; }
        switch (entry.d_tag) {
        case DT_JMPREL: relocations = entry.d_un.d_ptr; break;
        case DT_PLTRELSZ: bytes = entry.d_un.d_val; break;
        case DT_PLTREL: relocationKind = entry.d_un.d_val; break;
        case DT_SYMTAB: symbols = entry.d_un.d_ptr; break;
        case DT_SYMENT: symbolSize = entry.d_un.d_val; break;
        case DT_STRTAB: strings = entry.d_un.d_ptr; break;
        case DT_STRSZ: stringSize = entry.d_un.d_val; break;
        case DT_BIND_NOW: eager = true; break;
        case DT_FLAGS: eager |= (entry.d_un.d_val & DF_BIND_NOW) != 0; break;
        case DT_FLAGS_1: eager |= (entry.d_un.d_val & DF_1_NOW) != 0; break;
        default: break;
        }
    }
    relocations = dynamicAddress(*image, relocations, bytes);
    symbols = dynamicAddress(*image, symbols, sizeof(ElfW(Sym)));
    strings = dynamicAddress(*image, strings, stringSize);
    if (!ended || !eager || !relocations || !symbols || !strings || !stringSize || symbolSize != sizeof(ElfW(Sym))) return 1;
    if (relocationKind == DT_REL)
        result.valid = imports<ElfW(Rel)>(*image, relocations, bytes, symbols, reinterpret_cast<const char *>(strings), stringSize, result);
    else if (relocationKind == DT_RELA)
        result.valid = imports<ElfW(Rela)>(*image, relocations, bytes, symbols, reinterpret_cast<const char *>(strings), stringSize, result);
    return 1;
}
bool repairPermissions() {
    if (!permissionRepair.pending) return true;
    if (mprotect(permissionRepair.page, permissionRepair.size, permissionRepair.permissions) != 0) return false;
    permissionRepair.pending = false;
    return true;
}
bool replace(void **slot, void *expected, void *replacement) {
    if (!repairPermissions()) return false;
    const long pageSize = sysconf(_SC_PAGESIZE);
    Mapping region{};
    const uintptr_t address = reinterpret_cast<uintptr_t>(slot);
    if (pageSize <= 0 || (pageSize & (pageSize - 1)) || !mapping(address, sizeof(void *), region)
        || !(region.permissions & PROT_READ) || (region.permissions & PROT_EXEC)) return false;
    const uintptr_t page = address & ~static_cast<uintptr_t>(pageSize - 1);
    if (page < region.begin || static_cast<uintptr_t>(pageSize) > region.end - page) return false;
    if (mprotect(reinterpret_cast<void *>(page), pageSize, region.permissions | PROT_WRITE) != 0) return false;
    const bool changed = __atomic_compare_exchange_n(slot, &expected, replacement, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    if (mprotect(reinterpret_cast<void *>(page), pageSize, region.permissions) == 0) return changed;
    // A failed permission restore must not leave an armed observer behind.
    if (changed) __atomic_compare_exchange_n(slot, &replacement, expected, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    permissionRepair = {reinterpret_cast<void *>(page), static_cast<size_t>(pageSize), region.permissions, true};
    repairPermissions();
    return false;
}
void sendEvent(const unsigned char *bytes, size_t length) {
    if (outputFd >= 0 && send(outputFd, bytes, length, MSG_DONTWAIT | MSG_NOSIGNAL) < 0
        && (errno == ECONNREFUSED || errno == ENOENT)) { close(outputFd); outputFd = -1; }
}
ssize_t observeRead(void *port, void *buffer, size_t size) {
    const int incoming = errno;
    pthread_mutex_lock(&panelLock);
    const Read target = originalRead;
    const Sequence sequence = observedSequence;
    const unsigned current = generation;
    char socketPath[sizeof activeSocket], portName[sizeof activePort];
    memcpy(socketPath, activeSocket, sizeof socketPath);
    memcpy(portName, activePort, sizeof portName);
    pthread_mutex_unlock(&panelLock);
    const uint64_t before = sequence ? sequence(socketPath, portName) : 0;
    errno = incoming;
    const ssize_t result = target(port, buffer, size);
    const int saved = errno;
    const bool alreadyObserved = sequence && before != sequence(socketPath, portName);
    if (result > 0 && static_cast<size_t>(result) <= size && !alreadyObserved) {
        pthread_mutex_lock(&panelLock);
        if (current == generation && outputFd >= 0) {
            const char *name = originalName(port);
            if (name && strcmp(name, activePort) == 0)
                for (ssize_t i = 0; i < result; ++i)
                    panelParser.feed(static_cast<unsigned char *>(buffer)[i], [](unsigned char note, bool down) {
                        const unsigned char message[] = {'B', note, static_cast<unsigned char>(down)};
                        sendEvent(message, sizeof message);
                    });
        }
        pthread_mutex_unlock(&panelLock);
    }
    errno = saved;
    return result;
}
bool sessionDirectory(const char *path, char *parent, size_t capacity, int &directory) {
    if (!path || path[0] != '/' || strlen(path) >= sizeof activeSocket) return false;
    const char *slash = strrchr(path, '/');
    if (!slash || strcmp(slash, "/shortcut.sock") || static_cast<size_t>(slash - path) >= capacity) return false;
    memcpy(parent, path, slash - path); parent[slash - path] = 0;
    char canonical[4096];
    if (!realpath(parent, canonical) || strcmp(parent, canonical)) return false;
    directory = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st{};
    if (directory < 0) return false;
    if (fstat(directory, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0777) != 0700
        || fstatat(directory, "shortcut.sock", &st, AT_SYMLINK_NOFOLLOW) != 0
        || !S_ISSOCK(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0777) != 0600 || st.st_nlink != 1) {
        close(directory); directory = -1; return false;
    }
    return true;
}
bool markerForSession(const char *parent, Sequence &sequence) {
    sequence = reinterpret_cast<Sequence>(dlsym(RTLD_DEFAULT, "pmu_shortcut_observed_sequence"));
    if (sequence) {
        Dl_info marker{}, read{};
        if (!dladdr(reinterpret_cast<void *>(sequence), &marker) || !marker.dli_fname) return false;
        void *handle = dlopen(marker.dli_fname, RTLD_NOW | RTLD_NOLOAD);
        if (!handle) return false;
        const bool valid = dladdr(dlsym(handle, "snd_rawmidi_read"), &read) && marker.dli_fbase == read.dli_fbase;
        dlclose(handle);
        if (!valid) return false;
    }
    char hook[4096];
    if (snprintf(hook, sizeof hook, "%s/shortcut-hook.so", parent) >= static_cast<int>(sizeof hook)) return false;
    void *handle = dlopen(hook, RTLD_NOW | RTLD_NOLOAD);
    if (!handle) return true;
    const bool supported = dlsym(handle, "pmu_shortcut_observed_sequence") != nullptr;
    dlclose(handle);
    return supported;
}
bool environment(int directory, const char *path, char *port, size_t capacity) {
    const int fd = openat(directory, "shortcut.env", O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st{};
    char text[65537];
    size_t used = 0;
    bool valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0
        && (st.st_mode & 0777) == 0600 && st.st_nlink == 1 && st.st_size > 0 && st.st_size <= 65536;
    while (valid && used < sizeof text) {
        const ssize_t count = read(fd, text + used, sizeof text - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { valid = false; break; }
        if (!count) break;
        used += static_cast<size_t>(count);
    }
    close(fd);
    if (!valid || used != static_cast<size_t>(st.st_size) || used > 65536 || text[used - 1] != '\n') return false;
    unsigned ports = 0, sockets = 0, preloads = 0;
    size_t offset = 0;
    while (offset < used) {
        size_t end = offset;
        while (end < used && text[end] != '\n') ++end;
        const char *line = text + offset;
        const size_t length = end - offset;
        const bool panel = length > 15 && memcmp(line, "MPC_PANEL_PORT=", 15) == 0;
        const bool socket = length > 20 && memcmp(line, "MPC_SHORTCUT_SOCKET=", 20) == 0;
        const bool preload = length > 11 && memcmp(line, "LD_PRELOAD=", 11) == 0;
        if (panel || socket || preload) {
            const size_t prefix = panel ? 15 : socket ? 20 : 11;
            if (length < prefix + 2 || line[prefix] != '"' || line[length - 1] != '"') return false;
            char value[256]; size_t written = 0;
            for (size_t i = prefix + 1; i < length - 1; ++i) {
                char byte = line[i];
                if (byte == '\\') {
                    if (++i >= length - 1 || (line[i] != '\\' && line[i] != '"')) return false;
                    byte = line[i];
                } else if (byte == '"') return false;
                if (static_cast<unsigned char>(byte) < 32 || byte == 127) return false;
                if (!preload) {
                    if (written + 1 >= sizeof value) return false;
                    value[written++] = byte;
                }
            }
            value[written] = 0;
            if (preload) { if (++preloads != 1) return false; }
            else if (!written) return false;
            else if (panel) {
                if (++ports != 1 || written >= capacity) return false;
                memcpy(port, value, written + 1);
            } else if (++sockets != 1 || strcmp(value, path)) return false;
        } else return false;
        offset = end + 1;
    }
    return ports == 1 && sockets == 1 && preloads == 1;
}
}

extern "C" int pmu_panel_start(const char *path, const char *port) {
    if (!port || !*port || strlen(port) >= sizeof activePort) return 0;
    char parent[4096]; int directory = -1;
    if (!sessionDirectory(path, parent, sizeof parent, directory)) return 0;
    close(directory);
    Imports found;
    dl_iterate_phdr(findImports, &found);
    if (!found.valid) return 0;
    Sequence sequence = nullptr;
    if (!markerForSession(parent, sequence)) return 0;
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    sockaddr_un destination{}; destination.sun_family = AF_UNIX; strcpy(destination.sun_path, path);
    if (connect(fd, reinterpret_cast<sockaddr *>(&destination), sizeof destination) != 0) { close(fd); return 0; }
    pthread_mutex_lock(&panelLock);
    if (!repairPermissions()) { pthread_mutex_unlock(&panelLock); close(fd); return 0; }
    void *current = __atomic_load_n(found.read, __ATOMIC_ACQUIRE);
    void *name = __atomic_load_n(found.name, __ATOMIC_ACQUIRE);
    bool success = false;
    bool adopted = false;
    if (outputFd >= 0 && readSlot == found.read && current == reinterpret_cast<void *>(observeRead)
        && strcmp(path, activeSocket) == 0 && strcmp(port, activePort) == 0) success = true;
    else if (executable(current) && executable(name)
        && (!originalRead || (readSlot == found.read && (current == reinterpret_cast<void *>(originalRead)
            || current == reinterpret_cast<void *>(observeRead))))) {
        if (!originalRead) { originalRead = reinterpret_cast<Read>(current); readSlot = found.read; }
        originalName = reinterpret_cast<Name>(name);
        if (current == reinterpret_cast<void *>(observeRead) || replace(readSlot, current, reinterpret_cast<void *>(observeRead))) {
            if (outputFd >= 0) close(outputFd);
            outputFd = fd;
            adopted = true;
            observedSequence = sequence;
            strcpy(activeSocket, path); strcpy(activePort, port);
            panelParser = {}; ++generation;
            const unsigned char ready = 'R'; sendEvent(&ready, 1);
            success = outputFd >= 0;
            if (!success) replace(readSlot, reinterpret_cast<void *>(observeRead), reinterpret_cast<void *>(originalRead));
        }
    }
    if (!adopted) close(fd);
    pthread_mutex_unlock(&panelLock);
    return success;
}
extern "C" int pmu_arm_current_panel(const char *path) {
    char parent[4096], port[sizeof activePort]; int directory = -1;
    if (!sessionDirectory(path, parent, sizeof parent, directory)) return 0;
    const bool valid = environment(directory, path, port, sizeof port);
    close(directory);
    return valid && pmu_panel_start(path, port);
}
extern "C" int pmu_panel_stop(void) {
    pthread_mutex_lock(&panelLock);
    ++generation;
    if (outputFd >= 0) { close(outputFd); outputFd = -1; }
    panelParser = {};
    bool restored = repairPermissions();
    if (readSlot && __atomic_load_n(readSlot, __ATOMIC_ACQUIRE) == reinterpret_cast<void *>(observeRead))
        restored = replace(readSlot, reinterpret_cast<void *>(observeRead), reinterpret_cast<void *>(originalRead));
    pthread_mutex_unlock(&panelLock);
    return restored;
}
