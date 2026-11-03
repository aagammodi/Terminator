#include "ipc.h"

/*
 * Reliably write exactly 'count' bytes to pipe/socket descriptor.
 * Retries on EINTR/EAGAIN to prevent partial IPC packet corruption.
 */
static bool write_all(int fd, const void *buf, size_t count) {
    if (fd < 0 || !buf) return false;
    const char *p = (const char *)buf;
    size_t written = 0;
    while (written < count) {
        ssize_t n = write(fd, p + written, count - written);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return false;
        }
        written += (size_t)n;
    }
    return true;
}

/*
 * Reliably read exactly 'count' bytes from pipe descriptor.
 * Loops until full length-prefixed payload or header is received.
 */
static bool read_all(int fd, void *buf, size_t count) {
    if (fd < 0 || !buf) return false;
    char *p = (char *)buf;
    size_t nread = 0;
    while (nread < count) {
        ssize_t n = read(fd, p + nread, count - nread);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return false;
        }
        nread += (size_t)n;
    }
    return true;
}

/*
 * Frame and transmit an IPC message (header + optional variable-length payload).
 * Used for GUI <-> worker tab communication across both Thread and Process modes.
 */
bool ipc_send_msg(int fd, IPCMsgType type, const void *payload, uint32_t len) {
    if (fd < 0) return false;
    IPCHeader hdr;
    hdr.type = (uint32_t)type;
    hdr.length = len;

    if (!write_all(fd, &hdr, sizeof(hdr))) {
        return false;
    }
    if (len > 0 && payload) {
        if (!write_all(fd, payload, (size_t)len)) {
            return false;
        }
    }
    return true;
}

/*
 * Receive fixed-size 8-byte message header specifying message type and payload length.
 */
bool ipc_recv_header(int fd, IPCHeader *hdr) {
    if (fd < 0 || !hdr) return false;
    return read_all(fd, hdr, sizeof(IPCHeader));
}

/*
 * Read exact payload bytes specified in previously parsed header.
 */
bool ipc_recv_payload(int fd, void *buf, uint32_t len) {
    if (fd < 0 || !buf || len == 0) return false;
    return read_all(fd, buf, (size_t)len);
}
