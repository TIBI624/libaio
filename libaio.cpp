/**
libaio.cpp - Portable High-Performance Async I/O Engine
Targets: Android/Linux/macOS (ARM32/ARM64/x86/x86_64)
Standard: C++17, no architecture-specific instructions in critical paths
*/
#include <jni.h>

// Platform-specific includes
#if defined(ANDROID)
#include <android/log.h>
#endif

// Common POSIX headers
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

// C++ standard headers
#include <atomic>
#include <vector>
#include <cstring>
#include <new>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <array>
#include <thread>
#include <cassert>
#include <type_traits>
#include <functional>
#include <cerrno>

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
// sys/syscall.h не используется, можно удалить
#endif

#if defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
#error "Windows is not supported. This library targets Android/Linux/macOS."
#endif

// ============================================================
// CONFIGURATION
// ============================================================
#define LIBAIO_VERSION "1.0.2-stable"
#define LIBAIO_LOG_TAG "libaio"
#define LIBAIO_CACHE_LINE 64
#define LIBAIO_MAX_TASKS 1048576
#define LIBAIO_MINIAPP_LIMIT 8192
#define LIBAIO_EPOLL_EVENTS 128
#define LIBAIO_RING_CAPACITY 131072

static inline uint32_t libaio_get_worker_count() {
    uint32_t cores = std::thread::hardware_concurrency();
    if (cores == 0) cores = 4;
    return std::max(2u, std::min(cores / 2, 8u));
}

// Logging: on Android use android log, otherwise fallback to printf
#if defined(ANDROID)
#define LIBAIO_LOGI(...) __android_log_print(ANDROID_LOG_INFO, LIBAIO_LOG_TAG, __VA_ARGS__)
#define LIBAIO_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LIBAIO_LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LIBAIO_LOGI(...) printf("[INFO] " __VA_ARGS__); printf("\n")
#define LIBAIO_LOGE(...) printf("[ERROR] " __VA_ARGS__); printf("\n")
#endif

#if defined(__aarch64__)
#define LIBAIO_ARCH_STR  "arm64"
#elif defined(__arm__)
#define LIBAIO_ARCH_STR  "arm32"
#elif defined(__x86_64__)
#define LIBAIO_ARCH_STR  "x86_64"
#elif defined(__i386__)
#define LIBAIO_ARCH_STR  "i386"
#else
#define LIBAIO_ARCH_STR  "unknown"
#endif

inline uint64_t libaio_timestamp() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

#if defined(__GNUC__) || defined(__clang__)
#define LIBAIO_PREFETCH(p, rw, locality) __builtin_prefetch(p, rw, locality)
#else
#define LIBAIO_PREFETCH(p, rw, locality) ((void)0)
#endif

// ============================================================
// CACHE-LINE UTILITIES
// ============================================================
struct alignas(LIBAIO_CACHE_LINE) CacheLinePad {
    std::byte padding[LIBAIO_CACHE_LINE];
};

template<typename T>
struct alignas(LIBAIO_CACHE_LINE) AtomicAligned {
    std::atomic<T> value;
    CacheLinePad pad;
    AtomicAligned() : value(0) {}
    explicit AtomicAligned(T v) : value(v) {}
    AtomicAligned(const AtomicAligned&) = delete;
    AtomicAligned& operator=(const AtomicAligned&) = delete;
};

// ============================================================
// TASK DEFINITION
// ============================================================
struct Task {
    enum Type { FILE_READ, FILE_WRITE, MINIAPP_EXEC, CUSTOM };
    Type type;
    int fd;
    void* buffer;
    size_t length;
    off_t offset;
    void (*callback)(void* ctx, int status);
    void* user_ctx;
    uint64_t sequence;

    Task() : type(FILE_READ), fd(-1), buffer(nullptr), length(0), 
             offset(0), callback(nullptr), user_ctx(nullptr), sequence(0) {}
};

// ============================================================
// LOCK-FREE RING BUFFER (MPSC, power-of-two capacity)
// ============================================================
class LockFreeRing {
public:
    explicit LockFreeRing(size_t capacity = LIBAIO_RING_CAPACITY)
    : capacity_(capacity), mask_(capacity - 1) {
        assert((capacity & (capacity - 1)) == 0 && "Capacity must be power of two");
        buffer_ = new Task[capacity];
    }
    ~LockFreeRing() { delete[] buffer_; }

    bool push(const Task& task) {
        size_t current_tail = tail_.value.load(std::memory_order_relaxed);
        while (true) {
            size_t next_tail = (current_tail + 1) & mask_;
            size_t current_head = head_.value.load(std::memory_order_acquire);
            if (next_tail == current_head) return false; // full
            
            if (tail_.value.compare_exchange_weak(current_tail, next_tail, 
                                                  std::memory_order_relaxed, std::memory_order_relaxed)) {
                buffer_[current_tail] = task;
                tail_.value.store(next_tail, std::memory_order_release);
                return true;
            }
        }
    }

    bool pop(Task& task) {
        size_t current_head = head_.value.load(std::memory_order_relaxed);
        while (true) {
            size_t current_tail = tail_.value.load(std::memory_order_acquire);
            if (current_head == current_tail) return false; // empty
            
            task = buffer_[current_head];
            if (head_.value.compare_exchange_weak(current_head, (current_head + 1) & mask_,
                                                  std::memory_order_relaxed, std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    bool empty() const {
        return head_.value.load(std::memory_order_acquire) == 
               tail_.value.load(std::memory_order_acquire);
    }
private:
    Task* buffer_;
    size_t capacity_;
    size_t mask_;
    AtomicAligned<size_t> head_;
    AtomicAligned<size_t> tail_;
};

// ============================================================
// MINIAPP CONTEXT
// ============================================================
struct MiniAppContext {
    int id;
    std::unique_ptr<LockFreeRing> task_queue;
    std::atomic<bool> active;
    std::atomic<uint64_t> completed_ops;
    void* user_state;
    void (*on_init)(void* state);
    void (*on_destroy)(void* state);

    MiniAppContext() : id(-1), task_queue(nullptr), active(false), 
                       completed_ops(0), user_state(nullptr), 
                       on_init(nullptr), on_destroy(nullptr)  {}

    MiniAppContext(MiniAppContext&& other) noexcept 
        : id(other.id), task_queue(std::move(other.task_queue)),
          active(other.active.load()), completed_ops(other.completed_ops.load()),
          user_state(other.user_state), on_init(other.on_init), on_destroy(other.on_destroy) {
        other.id = -1;
        other.user_state = nullptr;
        other.on_init = nullptr;
        other.on_destroy = nullptr;
    }

    MiniAppContext& operator=(MiniAppContext&& other) noexcept {
        if (this != &other) {
            id = other.id;
            task_queue = std::move(other.task_queue);
            active.store(other.active.load());
            completed_ops.store(other.completed_ops.load());
            user_state = other.user_state;
            on_init = other.on_init;
            on_destroy = other.on_destroy;
            other.id = -1;
            other.user_state = nullptr;
            other.on_init = nullptr;
            other.on_destroy = nullptr;
        }
        return *this;
    }

    MiniAppContext(const MiniAppContext&) = delete;
    MiniAppContext& operator=(const MiniAppContext&) = delete;
};

// ============================================================
// ARENA ALLOCATOR
// ============================================================
class ArenaAllocator {
public:
    explicit ArenaAllocator(size_t capacity = 64 * 1024 * 1024)
    : capacity_(capacity), offset_(0), ptr_(nullptr) {
        ptr_ = static_cast<std::byte*>(mmap(nullptr, capacity_,
                                           PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (ptr_ == MAP_FAILED) {
            LIBAIO_LOGE("Arena mmap failed: %s", strerror(errno));
            ptr_ = nullptr;
            capacity_ = 0;
        }
    }
    ~ArenaAllocator() {
        if (ptr_ && ptr_ != MAP_FAILED) {
            munmap(ptr_, capacity_);
        }
    }

    template<typename T>
    T* allocate(size_t count = 1) {
        if (!ptr_) return nullptr;
        size_t size = count * sizeof(T);
        size_t aligned = (size + LIBAIO_CACHE_LINE - 1) & ~(LIBAIO_CACHE_LINE - 1);
        
        size_t current = offset_.fetch_add(aligned, std::memory_order_relaxed);
        if (current + aligned > capacity_) {
            return nullptr;
        }
        return reinterpret_cast<T*>(ptr_ + current);
    }

    void reset() { offset_.store(0, std::memory_order_release); }
    size_t used() const { return offset_.load(std::memory_order_acquire); }
private:
    std::byte* ptr_;
    size_t capacity_;
    std::atomic<size_t> offset_;
};

// ============================================================
// MAIN AIO ENGINE
// ============================================================
class LibAioEngine {
public:
    LibAioEngine() : epoll_fd_(-1), wake_fd_(-1), wake_write_fd_(-1), running_(false), completed_ops_(0) {}
    ~LibAioEngine() { destroy(); }

    bool init(size_t miniapp_capacity = 1024) {
        // Create notification mechanism
#if defined(__linux__)
        wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (wake_fd_ < 0) { LIBAIO_LOGE("eventfd: %s", strerror(errno)); return false; }
        wake_write_fd_ = wake_fd_; // same fd for write
#elif defined(__APPLE__)
        int pipefd[2];
        if (pipe(pipefd) == -1) { LIBAIO_LOGE("pipe: %s", strerror(errno)); return false; }
        fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
        fcntl(pipefd[1], F_SETFL, O_NONBLOCK);
        wake_fd_ = pipefd[0];      // read end
        wake_write_fd_ = pipefd[1]; // write end
#endif

        // Create event loop
#if defined(__linux__)
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) { LIBAIO_LOGE("epoll_create1: %s", strerror(errno)); return false; }
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = wake_fd_;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev) < 0) {
            LIBAIO_LOGE("epoll_ctl wake_fd: %s", strerror(errno)); return false;
        }
#elif defined(__APPLE__)
        epoll_fd_ = kqueue(); // use epoll_fd_ as kqueue fd for simplicity
        if (epoll_fd_ < 0) { LIBAIO_LOGE("kqueue: %s", strerror(errno)); return false; }
        struct kevent ev;
        EV_SET(&ev, wake_fd_, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        if (kevent(epoll_fd_, &ev, 1, nullptr, 0, nullptr) < 0) {
            LIBAIO_LOGE("kevent add: %s", strerror(errno)); return false;
        }
#endif

        global_queue_ = std::make_unique<LockFreeRing>();
        allocator_ = std::make_unique<ArenaAllocator>(128 * 1024 * 1024);
        
        miniapps_.reserve(miniapp_capacity);
        for (size_t i = 0; i < miniapp_capacity; ++i) {
            MiniAppContext ctx;
            ctx.id = static_cast<int>(i);
            ctx.task_queue = std::make_unique<LockFreeRing>();
            ctx.active.store(false);
            ctx.completed_ops.store(0);
            miniapps_.emplace_back(std::move(ctx));
        }

        running_.store(true, std::memory_order_release);
        start_workers();
        LIBAIO_LOGI("libaio init: arch=%s, workers=%zu, miniapps=%zu", 
                    LIBAIO_ARCH_STR, worker_threads_.size(), miniapps_.size());
        return true;
    }

    int create_miniapp(void* user_state, 
                       void (*on_init)(void*),  
                       void (*on_destroy)(void*)) {
        for (auto& ctx : miniapps_) {
            bool expected = false;
            if (ctx.active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                ctx.user_state = user_state;
                ctx.on_init = on_init;
                ctx.on_destroy = on_destroy;
                ctx.completed_ops.store(0);
                if (on_init) on_init(user_state);
                return ctx.id;
            }
        }
        LIBAIO_LOGE("MiniApp limit reached (%zu)", miniapps_.size());
        return -1;
    }

    void destroy_miniapp(int id) {
        if (id < 0 || id >= static_cast<int>(miniapps_.size())) return;
        auto& ctx = miniapps_[id];
        
        bool expected = true;
        if (ctx.active.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            if (ctx.on_destroy) ctx.on_destroy(ctx.user_state);
            Task dummy;
            while (ctx.task_queue->pop(dummy)) {}
            ctx.user_state = nullptr;
            ctx.on_init = nullptr;
            ctx.on_destroy = nullptr;
            LIBAIO_LOGI("MiniApp %d destroyed", id);
        }
    }

    bool submit_task(int miniapp_id, Task::Type type, int fd, void* buf, 
                     size_t len, off_t off, void (*cb)(void*, int), void* ctx) {
        if (!running_.load(std::memory_order_acquire)) return false;
        
        Task t{};
        t.type = type; t.fd = fd; t.buffer = buf; t.length = len;
        t.offset = off; t.callback = cb; t.user_ctx = ctx; 
        t.sequence = libaio_timestamp();

        if (miniapp_id >= 0 && miniapp_id < static_cast<int>(miniapps_.size())) {
            auto& mctx = miniapps_[miniapp_id];
            if (mctx.active.load(std::memory_order_acquire) && mctx.task_queue) {
                return mctx.task_queue->push(t);
            }
        }
        return global_queue_->push(t);
    }

    uint64_t poll_completions(uint32_t timeout_ms = 0) {
        // Wait for notification
        int n = 0;
#if defined(__linux__)
        epoll_event events[LIBAIO_EPOLL_EVENTS];
        n = epoll_wait(epoll_fd_, events, LIBAIO_EPOLL_EVENTS, static_cast<int>(timeout_ms));
#elif defined(__APPLE__)
        struct kevent events[LIBAIO_EPOLL_EVENTS];
        struct timespec ts;
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
        n = kevent(epoll_fd_, nullptr, 0, events, LIBAIO_EPOLL_EVENTS, (timeout_ms == (uint32_t)-1) ? nullptr : &ts);
#endif
        if (n > 0) {
            // Drain the notification
#if defined(__linux__)
            uint64_t val;
            while (read(wake_fd_, &val, sizeof(val)) > 0);
#elif defined(__APPLE__)
            char buf[64];
            while (read(wake_fd_, buf, sizeof(buf)) > 0);
#endif
        }

        // Return number of completed ops since last poll
        return completed_ops_.exchange(0, std::memory_order_acq_rel);
    }

    void destroy() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        wake();
        for (auto& t : worker_threads_) {
            if (t.joinable()) t.join();
        }
        worker_threads_.clear();
        if (epoll_fd_ >= 0) { close(epoll_fd_); epoll_fd_ = -1; }
        if (wake_fd_ >= 0) { close(wake_fd_); wake_fd_ = -1; }
        if (wake_write_fd_ >= 0 && wake_write_fd_ != wake_fd_) { close(wake_write_fd_); }
        for (auto& ctx : miniapps_) {
            if (ctx.active.load()) destroy_miniapp(ctx.id);
        }
        LIBAIO_LOGI("libaio destroyed");
    }

private:
    void start_workers() {
        uint32_t count = libaio_get_worker_count();
        worker_threads_.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            worker_threads_.emplace_back([this]() { worker_loop(); });
        }
    }

    void worker_loop() {
        std::chrono::milliseconds backoff{1};
        while (running_.load(std::memory_order_acquire)) {
            Task t;
            bool did_work = false;
            if (global_queue_->pop(t)) {
                execute_task(t);
                did_work = true;
            }
            
            for (auto& ctx : miniapps_) {
                if (ctx.active.load(std::memory_order_relaxed) && ctx.task_queue && ctx.task_queue->pop(t)) {
                    execute_task(t);
                    ctx.completed_ops.fetch_add(1, std::memory_order_relaxed);
                    did_work = true;
                    break;
                }
            }
            
            if (!did_work) {
                std::this_thread::sleep_for(backoff);
                if (backoff < std::chrono::milliseconds(50)) backoff *= 2;
            } else {
                backoff = std::chrono::milliseconds(1);
            }
        }
    }

    void execute_task(Task& t) {
        int status = 0;
        switch (t.type) {
            case Task::FILE_READ:
                if (t.fd >= 0 && t.buffer && t.length > 0) {
                    ssize_t r = pread(t.fd, t.buffer, t.length, t.offset);
                    status = (r >= 0) ? 0 : -errno;
                } else status = -EINVAL;
                break;
            case Task::FILE_WRITE:
                if (t.fd >= 0 && t.buffer && t.length > 0) {
                    ssize_t w = pwrite(t.fd, t.buffer, t.length, t.offset);
                    status = (w >= 0) ? 0 : -errno;
                } else status = -EINVAL;
                break;
            case Task::MINIAPP_EXEC: status = 0; break;
            default: status = -ENOSYS; break;
        }
        if (t.callback) t.callback(t.user_ctx, status);
        // Notify completion
        completed_ops_.fetch_add(1, std::memory_order_release);
        wake();
    }

    void wake() {
        if (wake_write_fd_ >= 0) {
#if defined(__linux__)
            uint64_t val = 1;
            ssize_t r = write(wake_write_fd_, &val, sizeof(val));
#elif defined(__APPLE__)
            char c = 1;
            ssize_t r = write(wake_write_fd_, &c, 1);
#endif
            (void)r;
        }
    }

    int epoll_fd_;      // epoll or kqueue fd
    int wake_fd_;       // read end of notification
    int wake_write_fd_; // write end (same as wake_fd_ on Linux)
    std::atomic<bool> running_;
    std::unique_ptr<LockFreeRing> global_queue_;
    std::vector<MiniAppContext> miniapps_;
    std::unique_ptr<ArenaAllocator> allocator_;
    std::vector<std::thread> worker_threads_;
    std::atomic<uint64_t> completed_ops_;
};

static std::unique_ptr<LibAioEngine> g_engine;
static std::mutex g_engine_mutex;

// JNI functions (unchanged, but use updated engine)
extern "C" {
JNIEXPORT jboolean JNICALL Java_com_example_libaio_LibAio_init(JNIEnv*, jclass, jlong capacity) {
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    if (g_engine) return JNI_FALSE;
    g_engine = std::make_unique<LibAioEngine>();
    return g_engine->init(static_cast<size_t>(capacity)) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jint JNICALL Java_com_example_libaio_LibAio_createMiniApp(JNIEnv*, jclass, jlong state_ptr, jlong on_init_ptr, jlong on_destroy_ptr) {
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    if (!g_engine) return -1;
    return g_engine->create_miniapp(
        reinterpret_cast<void*>(state_ptr),
        reinterpret_cast<void(*)(void*)>(on_init_ptr),
        reinterpret_cast<void(*)(void*)>(on_destroy_ptr)
    );
}
JNIEXPORT void JNICALL Java_com_example_libaio_LibAio_destroyMiniApp(JNIEnv*, jclass, jint id) {
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    if (g_engine) g_engine->destroy_miniapp(id);
}
JNIEXPORT jboolean JNICALL Java_com_example_libaio_LibAio_submitFileRead(JNIEnv*, jclass, jint miniapp, jint fd, jlong buffer, jlong len, jlong offset, jlong cb_ptr, jlong ctx) {
    if (!g_engine) return JNI_FALSE;
    return g_engine->submit_task(miniapp, Task::FILE_READ, fd,
        reinterpret_cast<void*>(buffer), static_cast<size_t>(len),
        static_cast<off_t>(offset), reinterpret_cast<void (*)(void*, int)>(cb_ptr),
        reinterpret_cast<void*>(ctx)) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL Java_com_example_libaio_LibAio_submitFileWrite(JNIEnv*, jclass, jint miniapp, jint fd, jlong buffer, jlong len, jlong offset, jlong cb_ptr, jlong ctx) {
    if (!g_engine) return JNI_FALSE;
    return g_engine->submit_task(miniapp, Task::FILE_WRITE, fd,
        reinterpret_cast<void*>(buffer), static_cast<size_t>(len),
        static_cast<off_t>(offset), reinterpret_cast<void (*)(void*, int)>(cb_ptr),
        reinterpret_cast<void*>(ctx)) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jlong JNICALL Java_com_example_libaio_LibAio_poll(JNIEnv*, jclass, jint timeout_ms) {
    if (!g_engine) return 0;
    return static_cast<jlong>(g_engine->poll_completions(static_cast<uint32_t>(timeout_ms)));
}
JNIEXPORT void JNICALL Java_com_example_libaio_LibAio_destroy(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    if (g_engine) { g_engine->destroy(); g_engine.reset(); }
}
JNIEXPORT jstring JNICALL Java_com_example_libaio_LibAio_version(JNIEnv* env, jclass) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s [%s]", LIBAIO_VERSION, LIBAIO_ARCH_STR);
    return env->NewStringUTF(buf);
}

// C API (now also declared in libaio.h)
__attribute__((visibility("default")))
int libaio_c_init(size_t capacity) {
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    if (g_engine) return -1;
    g_engine = std::make_unique<LibAioEngine>();
    return g_engine->init(capacity) ? 0 : -1;
}
__attribute__((visibility("default")))
int libaio_c_submit(int miniapp_id, int type, int fd, void* buf, size_t len, off_t off, void (*cb)(void*, int), void* ctx) {
    if (!g_engine) return -1;
    return g_engine->submit_task(miniapp_id, static_cast<Task::Type>(type), fd, buf, len, off, cb, ctx) ? 0 : -1;
}
__attribute__((visibility("default")))
uint64_t libaio_c_poll(uint32_t timeout_ms) {
    if (!g_engine) return 0;
    return g_engine->poll_completions(timeout_ms);
}
__attribute__((visibility("default")))
void libaio_c_destroy() {
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    if (g_engine) { g_engine->destroy(); g_engine.reset(); }
}
}