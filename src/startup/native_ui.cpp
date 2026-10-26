#include "native_ui.h"
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <new>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <typeinfo>
#include <unistd.h>

constexpr bool arm64 = sizeof(void *) == 8;
constexpr size_t main_queue_offset = arm64 ? 0x30 : 0x18;
constexpr size_t progress_owner_offset = arm64 ? 0x20 : 0x10;
struct NativeString { void *value; };
struct NativeFunction {
    unsigned char data[2 * sizeof(void *)];
    bool (*manage)(NativeFunction *, const NativeFunction *, int);
    void (*invoke)(const void *, const int *);
};
struct QuestionInfo {
    int kind;
    NativeString title, content;
    unsigned char url[arm64 ? 72 : 48];
    int count;
    NativeString labels[3];
    int results[3];
    unsigned char keys[36];
    NativeFunction result;
};
struct WeakHandle { void *value, *control; };
struct QueueCallback {
    void (*invoke)(void *);
    void (*manage)(int, void *, void *);
    unsigned char data[arm64 ? 160 : 144];
};
struct NativeProgressSession {
    unsigned char signal[arm64 ? 88 : 44];
    void *progress;
    float value;
    NativeString title;
    unsigned char active, cancellable, reserved[2];
};
static_assert(sizeof(QuestionInfo) == (arm64 ? 208 : 140)
              && sizeof(QueueCallback) == (arm64 ? 176 : 152)
              && sizeof(WeakHandle) == 2 * sizeof(void *), "Unsupported native UI layout");
static_assert(offsetof(QuestionInfo, count) == (arm64 ? 96 : 60)
              && offsetof(QuestionInfo, result) == (arm64 ? 176 : 124)
              && offsetof(NativeFunction, manage) == 2 * sizeof(void *), "Unsupported question layout");
static_assert(sizeof(NativeProgressSession) == (arm64 ? 120 : 60)
              && offsetof(NativeProgressSession, progress) == (arm64 ? 88 : 44)
              && offsetof(NativeProgressSession, title) == (arm64 ? 104 : 52)
              && offsetof(NativeProgressSession, active) == (arm64 ? 112 : 56)
              && offsetof(NativeProgressSession, cancellable) == (arm64 ? 113 : 57), "Unsupported progress layout");

static bool read_memory(void *, uintptr_t, size_t);

struct PmuUi {
    std::atomic<bool> active{true};
    std::mutex posting;
    uintptr_t image_base = 0;
    void *questions = nullptr;
    void *queue = nullptr;
    bool (*enqueue)(PmuUi *, QueueCallback *) = nullptr;
    bool (*raw_enqueue)(void *, QueueCallback *, uintptr_t) = nullptr;
    bool (*try_lock)(void *) = nullptr;
    void (*unlock)(void *) = nullptr;
    void (*cleanup)(void *) = nullptr;
    void (*string_default)(NativeString *) = nullptr;
    void (*string_text)(NativeString *, const char *) = nullptr;
    void (*string_destroy)(NativeString *) = nullptr;
    void (*url_default)(void *) = nullptr;
    void (*question_destroy)(QuestionInfo *) = nullptr;
    void (*ask)(WeakHandle *, void *, const QuestionInfo *) = nullptr;
    void (*cancel)(void *, WeakHandle *) = nullptr;
    void (*release_weak)(void *) = nullptr;
    bool (*read)(void *, uintptr_t, size_t) = read_memory;
    void *(*progress_context)(PmuUi *) = nullptr;
    void (*session_construct)(NativeProgressSession *, void *) = nullptr;
    void (*session_cancellable)(NativeProgressSession *, bool) = nullptr;
    void (*session_begin)(NativeProgressSession *) = nullptr;
    void (*session_title)(NativeProgressSession *, const NativeString *) = nullptr;
    void (*session_destroy)(NativeProgressSession *) = nullptr;
};
struct PmuUiLoadingHandle {
    PmuUi *ui;
    NativeProgressSession session{};
};
struct PmuUiHandle {
    PmuUi *ui;
    WeakHandle native{};
    PmuUiResult result;
    void *data;
    std::atomic<unsigned> references{1};
    std::atomic<bool> answered{false};
    bool invoking = false;
};
struct Capture { PmuUiHandle *handle; };
struct PostedTask { PmuUi *ui; PmuUiTask task; void *data; PmuUiTask destroy; };

static bool main_thread() { return syscall(SYS_gettid) == getpid(); }
static void retain(PmuUiHandle *handle) {
    handle->references.fetch_add(1, std::memory_order_relaxed);
}
static void release(PmuUiHandle *handle) {
    if (handle->references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete handle;
}
static bool read_memory(void *destination, uintptr_t source, size_t size) {
    if (!source) return false;
    iovec local{destination, size}, remote{reinterpret_cast<void *>(source), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}
static bool word_matches(uintptr_t address, uintptr_t expected) {
    uintptr_t value = 0;
    return read_memory(&value, address, sizeof(value)) && value == expected;
}
static bool instruction_matches(uintptr_t address, uint32_t expected) {
    uint32_t value = 0;
    return read_memory(&value, address, sizeof(value)) && value == expected;
}
static bool loading_fingerprint(uintptr_t base) {
    if (arm64)
        return instruction_matches(base + 0x1d49de0, 0xd503233f)
            && instruction_matches(base + 0x1d49e1c, 0xf9002e74)
            && instruction_matches(base + 0x1d49ec4, 0xd503233f)
            && instruction_matches(base + 0x1d49f60, 0x3941c000)
            && instruction_matches(base + 0x1d4a020, 0xd503233f)
            && instruction_matches(base + 0x1d4a400, 0xd503233f)
            && instruction_matches(base + 0x102d960, 0xd503245f);
    return instruction_matches(base + 0x180783c, 0xe16d41f0)
        && instruction_matches(base + 0x1807884, 0xe584502c)
        && instruction_matches(base + 0x1807960, 0xe16d40fc)
        && instruction_matches(base + 0x1807a04, 0xe5d03038)
        && instruction_matches(base + 0x1807a6c, 0xe5803010)
        && instruction_matches(base + 0x1807b08, 0xe1a0c001)
        && instruction_matches(base + 0x1808084, 0xe16d41f0)
        && instruction_matches(base + 0x180809c, 0xe5c06038)
        && instruction_matches(base + 0x934bc8, 0xe5903000);
}
static void *native_progress_context(PmuUi *ui) {
    const uintptr_t base = ui->image_base;
    uintptr_t application = 0, window = 0, context = 0, progress = 0;
    auto word = [&](uintptr_t address, uintptr_t expected) {
        uintptr_t value = 0;
        return ui->read(&value, address, sizeof(value)) && value == expected;
    };
    if (!ui->read(&application, base + (arm64 ? 0x7e88028 : 0x6f35c88), sizeof(application))
        || !application || !word(application, base + (arm64 ? 0x7a75ad8 : 0x6cdfaf4))
        || !ui->read(&window, application + (arm64 ? 296 : 0xc4), sizeof(window))
        || !window || !word(window, base + (arm64 ? 0x7a76188 : 0x6cdfe94))
        || !ui->read(&context, window + (arm64 ? 952 : 0x26c), sizeof(context))
        || !context || !word(context, base + (arm64 ? 0x7a80438 : 0x6ce5650))
        || !word(context + (arm64 ? 544 : 0x120), reinterpret_cast<uintptr_t>(ui->questions))
        || !ui->read(&progress, context + (arm64 ? 560 : 0x14c), sizeof(progress))
        || !progress || (progress & (sizeof(void *) - 1))) return nullptr;
    unsigned char state[arm64 ? 40 : 20];
    return ui->read(state, progress, sizeof(state)) ? reinterpret_cast<void *>(progress) : nullptr;
}
static bool native_enqueue(PmuUi *ui, QueueCallback *callback) {
    auto *queue = static_cast<unsigned char *>(ui->queue);
    void *mutex = queue + (arm64 ? 80 : 44);
    if (!ui->try_lock(mutex)) return false;
    bool accepted = false;
    try {
        auto *raw = reinterpret_cast<uintptr_t *>(queue + sizeof(void *));
        ui->cleanup(raw);
        const uintptr_t capacity = raw[0], write = raw[4], boundary = raw[2];
        const uintptr_t buffer = raw[arm64 ? 6 : 7];
        if (!reinterpret_cast<unsigned char *>(raw)[arm64 ? 45 : 25] && capacity > 1
            && capacity <= 1048576 && write < capacity && boundary < capacity && buffer) {
            const uintptr_t next = write + 1 == capacity ? 0 : write + 1;
            const uintptr_t state = buffer + write * (arm64 ? 192 : 160) + sizeof(QueueCallback);
            if (next != boundary && state >= buffer
                && (__atomic_load_n(reinterpret_cast<uintptr_t *>(state), __ATOMIC_ACQUIRE) & 3) == 0)
                accepted = ui->raw_enqueue(raw, callback, 0);
        }
    } catch (...) {
        ui->unlock(mutex);
        throw;
    }
    ui->unlock(mutex);
    return accepted;
}

#if defined(__aarch64__)
struct NativeWeakResult {
    void *value = nullptr, *control = nullptr;
    ~NativeWeakResult() {}
};
static void native_ask64(WeakHandle *out, void *questions, const QuestionInfo *info) {
    auto **table = *static_cast<void ***>(questions);
    auto ask = reinterpret_cast<NativeWeakResult (*)(void *, const QuestionInfo *)>(table[3]);
    auto result = ask(questions, info);
    *out = {result.value, result.control};
}
#endif

extern "C" PmuUi *pmu_ui_init(void *lua_state, uintptr_t base) {
    static std::mutex lock;
    static PmuUi ui;
    std::lock_guard<std::mutex> guard(lock);
    if (!lua_state || !base || !instruction_matches(base + (arm64 ? 0x4a0e350 : 0x4941920), arm64 ? 0xd503245f : 0xe3510000)
        || !instruction_matches(base + (arm64 ? 0x4a0f040 : 0x4942630), arm64 ? 0xd503245f : 0xe3510000)
        || !instruction_matches(base + (arm64 ? 0x3069ac0 : 0x2d2b7b8), arm64 ? 0xd503233f : 0xe16d41f8)
        || !loading_fingerprint(base)) return nullptr;
    auto type = reinterpret_cast<int (*)(void *, int)>(base + (arm64 ? 0x4a0e350 : 0x4941920));
    auto userdata = reinterpret_cast<void *(*)(void *, int)>(base + (arm64 ? 0x4a0f040 : 0x4942630));
    if (type(lua_state, 1) != 7) return nullptr;
    uintptr_t argument = reinterpret_cast<uintptr_t>(userdata(lua_state, 1));
    if (!argument) return nullptr;
    argument = (argument + sizeof(void *) - 1) & ~uintptr_t(sizeof(void *) - 1);
    uintptr_t function[4]{}, closure[5]{}, member[3]{}, queues = 0, questions = 0;
    if (!read_memory(function, argument, sizeof(function))
        || function[2] != base + (arm64 ? 0x18f7720 : 0x134ee30) || function[3] != base + (arm64 ? 0x190e6a0 : 0x134e7c8)
        || !read_memory(closure, function[0], sizeof(closure))
        || closure[3] != base + (arm64 ? 0x1922860 : 0x13494a0) || closure[4] != base + (arm64 ? 0x1922420 : 0x13490b4)
        || !read_memory(member, closure[1], sizeof(member))
        || member[1] != base + (arm64 ? 0x191f2d0 : 0x1345b10) || member[2] != 0
        || !word_matches(member[0], base + (arm64 ? 0x79fac30 : 0x6ca0750))
        || !read_memory(&questions, member[0] + (arm64 ? 160 : 96), sizeof(questions))
        || !word_matches(questions, base + (arm64 ? 0x7a03280 : 0x6ca498c))
        || !read_memory(&queues, closure[0], sizeof(queues))
        || !word_matches(queues, base + (arm64 ? 0x7a2b870 : 0x6cb958c))
        || !word_matches(queues + main_queue_offset, base + (arm64 ? 0x7a2b780 : 0x6cb9514))
        || !word_matches(base + (arm64 ? 0x7a2b790 : 0x6cb951c), base + (arm64 ? 0x3069ac0 : 0x2d2b7b8))) return nullptr;
    if (ui.image_base)
        return ui.image_base == base && ui.questions == reinterpret_cast<void *>(questions)
            && ui.queue == reinterpret_cast<void *>(queues + main_queue_offset) ? &ui : nullptr;
    Dl_info location{};
    if (!dladdr(reinterpret_cast<void *>(&pmu_ui_init), &location) || !location.dli_fname
        || !dlopen(location.dli_fname, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE)) return nullptr;
    ui.questions = reinterpret_cast<void *>(questions);
    ui.queue = reinterpret_cast<void *>(queues + main_queue_offset);
    ui.enqueue = native_enqueue;
    ui.raw_enqueue = reinterpret_cast<decltype(ui.raw_enqueue)>(base + (arm64 ? 0x306e380 : 0x2d3299c));
    ui.try_lock = reinterpret_cast<decltype(ui.try_lock)>(base + (arm64 ? 0x1063fe8 : 0x96fb64));
    ui.unlock = reinterpret_cast<decltype(ui.unlock)>(base + (arm64 ? 0x106400c : 0x96fb84));
    ui.cleanup = reinterpret_cast<decltype(ui.cleanup)>(base + (arm64 ? 0x306dd20 : 0x2d32370));
    ui.string_default = reinterpret_cast<decltype(ui.string_default)>(base + (arm64 ? 0x103fbcc : 0x948a20));
    ui.string_text = reinterpret_cast<decltype(ui.string_text)>(base + (arm64 ? 0x10764c4 : 0x983ea0));
    ui.string_destroy = reinterpret_cast<decltype(ui.string_destroy)>(base + (arm64 ? 0x102d960 : 0x934bc8));
    ui.url_default = reinterpret_cast<decltype(ui.url_default)>(base + (arm64 ? 0x1068ee0 : 0x975340));
    ui.question_destroy = reinterpret_cast<decltype(ui.question_destroy)>(base + (arm64 ? 0x15d4540 : 0xfa1d74));
#if defined(__aarch64__)
    ui.ask = native_ask64;
#else
    ui.ask = reinterpret_cast<decltype(ui.ask)>(base + 0x180a96c);
#endif
    ui.cancel = reinterpret_cast<decltype(ui.cancel)>(base + (arm64 ? 0x1d4c9e0 : 0x180ad00));
    ui.release_weak = reinterpret_cast<decltype(ui.release_weak)>(base + (arm64 ? 0x15f5020 : 0xfc60f0));
    ui.progress_context = native_progress_context;
    ui.session_construct = reinterpret_cast<decltype(ui.session_construct)>(base + (arm64 ? 0x1d49de0 : 0x180783c));
    ui.session_cancellable = reinterpret_cast<decltype(ui.session_cancellable)>(base + (arm64 ? 0x1d4a020 : 0x1807b08));
    ui.session_begin = reinterpret_cast<decltype(ui.session_begin)>(base + (arm64 ? 0x1d4a400 : 0x1808084));
    ui.session_title = reinterpret_cast<decltype(ui.session_title)>(base + (arm64 ? 0x1d49ec4 : 0x1807960));
    ui.session_destroy = reinterpret_cast<decltype(ui.session_destroy)>(base + (arm64 ? 0x1d49f4c : 0x1807a04));
    ui.image_base = base;
    return &ui;
}

extern "C" PmuUiLoadingHandle *pmu_ui_loading_begin(PmuUi *ui, const char *title) {
    if (!ui || !title || !ui->active.load() || !main_thread()
        || strnlen(title, 129) > 128 || !ui->progress_context || !ui->session_construct
        || !ui->session_cancellable || !ui->session_begin || !ui->session_title
        || !ui->session_destroy || !ui->string_text || !ui->string_destroy) return nullptr;
    void *progress = ui->progress_context(ui);
    uintptr_t owner = 0;
    if (!progress || !ui->read(&owner, reinterpret_cast<uintptr_t>(progress) + progress_owner_offset, sizeof(owner))
        || owner) return nullptr;
    auto *handle = new (std::nothrow) PmuUiLoadingHandle{ui, {}};
    if (!handle) return nullptr;
    bool constructed = false, text_constructed = false;
    NativeString text{};
    try {
        ui->session_construct(&handle->session, progress);
        constructed = true;
        ui->string_text(&text, title);
        text_constructed = true;
        ui->session_cancellable(&handle->session, false);
        ui->session_begin(&handle->session);
        ui->session_title(&handle->session, &text);
        ui->string_destroy(&text);
        text_constructed = false;
        if (handle->session.active && !handle->session.cancellable
            && handle->session.progress == progress
            && ui->read(&owner, reinterpret_cast<uintptr_t>(progress) + progress_owner_offset, sizeof(owner))
            && owner == reinterpret_cast<uintptr_t>(&handle->session)) return handle;
    } catch (...) {}
    if (text_constructed) try { ui->string_destroy(&text); } catch (...) {}
    if (constructed) try { ui->session_destroy(&handle->session); } catch (...) {}
    delete handle;
    return nullptr;
}
extern "C" void pmu_ui_loading_end(PmuUi *ui, PmuUiLoadingHandle *handle) {
    if (!ui || !handle || handle->ui != ui || !main_thread()) return;
    try { ui->session_destroy(&handle->session); } catch (...) { return; }
    delete handle;
}

static void destroy_task(PostedTask *task, bool invoke) {
    if (!task) return;
    if (invoke) {
        static std::atomic<bool> reported{false};
        const bool main = main_thread();
        if (!main && !reported.exchange(true))
            std::fprintf(stderr, "open-mpc manager rejected UI callback pid=%ld tid=%ld\n",
                         static_cast<long>(getpid()), static_cast<long>(syscall(SYS_gettid)));
        try { if (main && task->ui->active.load()) task->task(task->data); } catch (...) {}
    }
    try { if (task->destroy) task->destroy(task->data); } catch (...) {}
    delete task;
}
static void task_invoke(void *payload) {
    PostedTask *task = nullptr;
    std::memcpy(&task, payload, sizeof(task));
    std::memset(payload, 0, sizeof(task));
    destroy_task(task, true);
}
static void task_manage(int operation, void *source, void *destination) {
    PostedTask *task = nullptr;
    std::memcpy(&task, source, sizeof(task));
    if (operation == 0) std::memcpy(destination, &task, sizeof(task));
    else if (operation == 1) destroy_task(task, false);
    if (operation == 0 || operation == 1) std::memset(source, 0, sizeof(task));
}
extern "C" int pmu_ui_post(PmuUi *ui, PmuUiTask task, void *data, PmuUiTask destroy) {
    if (!ui || !task) return 0;
    std::lock_guard<std::mutex> guard(ui->posting);
    if (!ui->active.load()) return 0;
    auto *posted = new (std::nothrow) PostedTask{ui, task, data, destroy};
    if (!posted) return 0;
    QueueCallback callback{task_invoke, task_manage, {}};
    std::memcpy(callback.data, &posted, sizeof(posted));
    try {
        if (ui->enqueue(ui, &callback)) return 1;
    } catch (...) {}
    std::memcpy(&posted, callback.data, sizeof(posted));
    if (!posted) return 1;
    delete posted;
    return 0;
}
extern "C" void pmu_ui_stop(PmuUi *ui) {
    if (!ui) return;
    std::lock_guard<std::mutex> guard(ui->posting);
    ui->active.store(false);
}

static bool result_manage(NativeFunction *destination, const NativeFunction *source, int operation) {
    Capture capture{};
    std::memcpy(&capture, source->data, sizeof(capture));
    if (operation == 0) {
        const std::type_info *type = &typeid(Capture);
        std::memcpy(destination->data, &type, sizeof(type));
    } else if (operation == 1) {
        const void *address = source->data;
        std::memcpy(destination->data, &address, sizeof(address));
    } else if (operation == 2) {
        retain(capture.handle);
        std::memcpy(destination->data, source->data, sizeof(destination->data));
    } else if (operation == 3) {
        release(capture.handle);
    }
    return false;
}
static void result_invoke(const void *payload, const int *choice) {
    Capture capture{};
    std::memcpy(&capture, payload, sizeof(capture));
    PmuUiHandle *handle = capture.handle;
    if (!main_thread() || !choice || !handle->ui->active.load() || handle->answered.exchange(true)) return;
    handle->invoking = true;
    try { handle->result(*choice == 1 ? 1 : 0, handle->data); } catch (...) {}
    handle->invoking = false;
}
extern "C" PmuUiHandle *pmu_ui_show(PmuUi *ui, const char *title, const char *message,
                                    PmuUiResult result, void *data) {
    if (!ui || !title || !message || !result || !ui->active.load() || !main_thread()
        || strnlen(title, 129) > 128 || strnlen(message, 4097) > 4096) return nullptr;
    auto *handle = new (std::nothrow) PmuUiHandle{ui, {}, result, data};
    if (!handle) return nullptr;
    QuestionInfo info{};
    bool initialized = false;
    try {
        ui->string_default(&info.title);
        ui->string_default(&info.content);
        for (auto &label : info.labels) ui->string_default(&label);
        ui->url_default(info.url);
        initialized = true;
        info.kind = 1;
        ui->string_text(&info.title, title);
        ui->string_text(&info.content, message);
        ui->string_text(&info.labels[0], "Open");
        ui->string_text(&info.labels[1], "Cancel");
        info.count = 2;
        info.results[0] = 1;
        Capture capture{handle};
        retain(handle);
        std::memcpy(info.result.data, &capture, sizeof(capture));
        info.result.manage = result_manage;
        info.result.invoke = result_invoke;
        ui->ask(&handle->native, ui->questions, &info);
        ui->question_destroy(&info);
        initialized = false;
        if (handle->native.value) return handle;
    } catch (...) {
        if (initialized) ui->question_destroy(&info);
    }
    handle->answered.store(true);
    if (handle->native.control) ui->release_weak(handle->native.control);
    release(handle);
    return nullptr;
}
extern "C" void pmu_ui_dismiss(PmuUi *ui, PmuUiHandle *handle) {
    if (!ui || !handle || handle->ui != ui || !ui->active.load() || !main_thread() || handle->invoking) return;
    handle->answered.store(true);
    try { if (handle->native.value) ui->cancel(ui->questions, &handle->native); } catch (...) {}
    if (handle->native.control) ui->release_weak(handle->native.control);
    release(handle);
}
