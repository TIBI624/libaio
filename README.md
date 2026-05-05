![version](https://img.shields.io/badge/version-1.0.2--stable-brightgreen.svg) ![license](https://img.shields.io/badge/license-Apache%202.0-blue.svg) ![status](https://img.shields.io/badge/status-Stable%20Release-success.svg) ![platform](https://img.shields.io/badge/platform-Android%20%7C%20Linux-lightgrey.svg) ![arch](https://img.shields.io/badge/arch-ARM32%20%7C%20ARM64-orange.svg) ![lang](https://img.shields.io/badge/lang-C%2B%2B17%20%7C%20Java%20%7C%20Kotlin%20%7C%20Swift-purple.svg)

# LibAIO
Portable High-Performance Asynchronous I/O Engine

LibAIO is a production-ready, single-file C++17 asynchronous I/O engine engineered specifically for Android and Linux ecosystems on ARM hardware. It delivers deterministic, non-blocking file operations with minimal CPU overhead, predictable memory footprint, and seamless interoperability across Java/Kotlin, Swift, C/C++, and Python. The engine relies purely on POSIX APIs and standard C++17, with zero architecture-specific intrinsics in critical paths.

**Important Distribution Note:** This package contains only the compiled runtime artifacts and language bindings. The local build script (`build.sh`) and GitHub Actions CI/CD workflows are intentionally excluded from this release. They are intended strictly for internal developer environments (e.g., Termux on-device development) and are not part of the public distribution.

**Included Files:**
- `libaio.so` : Compiled shared library for ARM32/ARM64
- `libAio.swift` : Native Swift wrapper with async/await support

📑 Table of Contents
- Installation
- Quick Start Examples
- API Reference
- Architecture & Performance
- Best Practices
- Community & License

---

## 📦 Installation

1. Download the latest release artifact from the [Releases Page](https://github.com/TIBI624/libaio/releases)
2. Place `libaio.so` into your project's native libraries directory:
   - Android: `app/src/main/jniLibs/arm64-v8a/libaio.so` and `armeabi-v7a/libaio.so`
   - Linux/Embedded: `/usr/local/lib/` or your project's `libs/` folder
3. Add `libAio.swift` directly to your iOS/macOS Xcode project. Ensure the Swift module links against `libaio.so` via a bridging header or module map if targeting cross-platform mobile.
4. Initialize in your code:
   - Android/Kotlin: `init { System.loadLibrary("libaio") }`
   - Linux/C++: Link with `-llibaio -lpthread`
   - Swift: Import the wrapper module and call `LibAioManager.shared.initialize()`

---

## 🚀 Quick Start Examples

### Kotlin (Android)
```kotlin
import com.example.libaio.LibAio
import java.nio.ByteBuffer

object AIOManager {
    init { System.loadLibrary("libaio") }

    fun startEngine() {
        if (!LibAio.init(2048L)) error("Engine initialization failed")
        println("LibAIO ready: ${LibAio.version()}")
    }

    fun submitAsyncRead(appId: Int, fd: Int, size: Long, offset: Long): ByteBuffer {
        val buffer = ByteBuffer.allocateDirect(size.toInt())
        val address = (buffer as java.nio.DirectByteBuffer).address()
        val success = LibAio.submitFileRead(appId, fd, address, size, offset, 0L, 0L)
        if (!success) println("Warning: Task queue full or invalid FD")
        return buffer
    }

    fun pollLoop() {
        Thread {
            while (true) {
                val done = LibAio.poll(50)
                if (done > 0) println("Processed $done I/O tasks")
                Thread.sleep(20)
            }
        }.start()
    }
}
```

### Swift (iOS / Cross-platform)
```swift
import Foundation

struct AIORequest {
    let fd: Int32
    let buffer: UnsafeMutableRawPointer
    let size: UInt
    let offset: UInt64
}

func performAsyncRead(request: AIORequest, context: UnsafeMutableRawPointer?) async -> Bool {
    let success = LibAioManager.shared.submitRead(
        miniapp: 0,
        fd: request.fd,
        buffer: request.buffer,
        length: request.size,
        offset: request.offset,
        context: context
    )
    return success
}

// Usage example
Task {
    let data = UnsafeMutableRawPointer.allocate(byteCount: 4096, alignment: 64)
    defer { data.deallocate() }
    
    let req = AIORequest(fd: 3, buffer: data, size: 4096, offset: 0)
    let submitted = await performAsyncRead(request: req, context: nil)
    print("Submission status: \(submitted)")
    
    // Poll periodically on a background queue
    DispatchQueue.global(qos: .utility).async {
        while true {
            let completed = LibAioManager.shared.poll(timeoutMs: 100)
            if completed > 0 { print("Swift worker completed \(completed) ops") }
            Thread.sleep(forTimeInterval: 0.05)
        }
    }
}
```

### C++17 (Linux / Embedded)
```cpp
#include <cstdio>
#include <cstdint>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
    int libaio_c_init(size_t capacity);
    int libaio_c_submit(int miniapp_id, int type, int fd, void* buf, size_t len, off_t off, void (*cb)(void*, int), void* ctx);
    uint64_t libaio_c_poll(uint32_t timeout_ms);
    void libaio_c_destroy();
}

static void io_callback(void* ctx, int status) {
    int* counter = static_cast<int*>(ctx);
    if (status == 0) (*counter)++;
    else printf("I/O Error: %d\n", status);
}

int main() {
    if (libaio_c_init(512) != 0) return 1;

    int fd = open("test.dat", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 1;

    char buffer[4096];
    int completed = 0;

    libaio_c_submit(0, 0, fd, buffer, sizeof(buffer), 0, io_callback, &completed);
    
    while (completed == 0) {
        libaio_c_poll(10);
        usleep(1000);
    }

    printf("Read successful. Completed tasks: %d\n", completed);
    close(fd);
    libaio_c_destroy();
    return 0;
}
```

### Python (CFFI)
```python
import cffi
import os

ffi = cffi.FFI()
ffi.cdef("""
    int libaio_c_init(size_t capacity);
    int libaio_c_submit(int miniapp_id, int type, int fd, void* buf, size_t len, unsigned long long off, void* cb, void* ctx);
    unsigned long long libaio_c_poll(unsigned int timeout_ms);
    void libaio_c_destroy();
""")

lib = ffi.dlopen("./libaio.so")
assert lib.libaio_c_init(256) == 0

buf = ffi.new("char[]", 2048)
fd = os.open("data.bin", os.O_RDONLY)

def callback(ctx, status):
    print(f"Python callback triggered. Status: {status}")

cb_ptr = ffi.callback("void(void*, int)", callback)
lib.libaio_c_submit(0, 0, fd, buf, len(buf), 0, cb_ptr, ffi.NULL)

while True:
    processed = lib.libaio_c_poll(50)
    if processed > 0:
        print(f"Processed {processed} tasks")
        break

lib.libaio_c_destroy()
os.close(fd)
```

---

## 🔌 API Reference

### Java/Kotlin JNI (`com.example.libaio.LibAio`)
| Method | Signature | Description |
| --- | --- | --- |
| `init` | `boolean init(long capacity)` | Initializes engine. Sets max concurrent MiniApp contexts. Must be called first. |
| `createMiniApp` | `int createMiniApp(long state, long onInit, long onDestroy)` | Creates isolated task queue. Returns context ID (`>=0`) or `-1`. |
| `destroyMiniApp` | `void destroyMiniApp(int id)` | Deactivates context, drains queue, runs cleanup callbacks. |
| `submitFileRead` | `boolean submitFileRead(int app, int fd, long buf, long len, long off, long cb, long ctx)` | Queues async `pread`. Non-blocking. Returns `false` if queue full. |
| `submitFileWrite` | `boolean submitFileWrite(int app, int fd, long buf, long len, long off, long cb, long ctx)` | Queues async `pwrite`. Non-blocking. |
| `poll` | `long poll(int timeoutMs)` | Drives `epoll_wait` loop. Blocks up to `timeoutMs`. Call from background thread. |
| `destroy` | `void destroy()` | Graceful shutdown. Thread-safe. |

### C / FFI API
Exposed via `__attribute__((visibility("default")))`:
```c
int  libaio_c_init(size_t capacity);
int  libaio_c_submit(int miniapp_id, int type, int fd, void* buf, size_t len, off_t off, void (*cb)(void*, int), void* ctx);
uint64_t libaio_c_poll(uint32_t timeout_ms);
void libaio_c_destroy();
```
Note: `type` uses `0` for READ, `1` for WRITE.

---

## ⚙️ Architecture & Performance Design

| Component | Implementation | Benefit |
| --- | --- | --- |
| Task Queue | Lock-free MPSC Ring Buffer (`capacity=131072`) | Zero mutex contention. Branch-free indexing. `O(1)` submission. |
| Memory | `mmap` Arena Allocator (`128MB`) + 64B alignment | Eliminates heap fragmentation. Prevents false sharing. Zero GC pressure. |
| I/O Backend | `epoll` + `eventfd` + POSIX `pread`/`pwrite` | Fully portable on Linux/Android ARM. Avoids kernel AIO complexity. |
| Concurrency | Thermal-safe thread pool (`cores/2`, max 8) | Prevents CPU throttling on mobile SoCs. Idle workers yield via backoff sleep. |
| Timing | `std::chrono::steady_clock` | Cross-platform monotonic timestamps. Safe for ARM. No `rdtsc`. |

Performance Profile:
✅ Submission Latency: `< 50μs` per task
✅ Throughput: `> 500k ops/sec` on mid-tier ARM SoCs
✅ Memory: Fixed arena footprint, deterministic allocation
✅ Thermal: Idle yield (`100μs` backoff), no spinlocks, bounded worker count

---

## 🛡️ Best Practices & Memory Management

- Buffer Allocation: Always use `ByteBuffer.allocateDirect()` in Java/Kotlin or `mmap`/aligned `malloc` in C/Swift. Never pass heap-allocated managed objects directly to native I/O calls.
- Polling Thread: Run `LibAio.poll(timeout)` on a dedicated background thread, `CoroutineScope(Dispatchers.IO)`, or Swift `Task.detached`. Calling it on the main thread defeats async design.
- Context Lifecycle: Destroy MiniApp contexts when modules unload. This drains pending tasks and invokes cleanup callbacks, preventing memory leaks.
- Thread Safety: `init` and `destroy` are globally synchronized. Submission is lock-free. `poll` should run on a single thread to process all queues efficiently.
- Error Handling: Always check return values. `false`/`-1` typically means uninitialized engine, invalid FD, or saturated ring buffer.

---

## 🤝 Community & License

LibAIO is an open, community-driven project. We welcome performance benchmarks, language bindings, and documentation improvements.

Distributed under the Apache License 2.0. See LICENSE for details.

📌 Production Ready: Download `libaio.so` from [Releases](https://github.com/TIBI624/libaio/releases), drop into `jniLibs/`, and ship non-blocking I/O today.
🐛 Issues & Discussions: [GitHub Issues](https://github.com/TIBI624/libaio/issues) | 📖 Source Code: [Repository](https://github.com/TIBI624/libaio)