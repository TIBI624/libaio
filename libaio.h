#ifndef LIBAIO_H
#define LIBAIO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> // for off_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize libaio engine.
 * @param capacity Max number of MiniApp contexts.
 * @return 0 on success, -1 on error.
 */
int libaio_c_init(size_t capacity);

/**
 * Submit an asynchronous I/O task.
 * @param miniapp_id MiniApp context ID (0 for global, or from create_miniapp).
 * @param type 0 for read, 1 for write.
 * @param fd File descriptor.
 * @param buf Buffer pointer.
 * @param len Buffer length.
 * @param off Offset.
 * @param cb Callback function (can be NULL).
 * @param ctx User context passed to callback.
 * @return 0 on success, -1 on error.
 */
int libaio_c_submit(int miniapp_id, int type, int fd, void* buf, size_t len, off_t off, void (*cb)(void*, int), void* ctx);

/**
 * Poll for completed operations.
 * @param timeout_ms Timeout in milliseconds (0 = no wait).
 * @return Number of completed operations since last poll.
 */
uint64_t libaio_c_poll(uint32_t timeout_ms);

/**
 * Destroy engine and free resources.
 */
void libaio_c_destroy();

#ifdef __cplusplus
}
#endif

#endif // LIBAIO_H