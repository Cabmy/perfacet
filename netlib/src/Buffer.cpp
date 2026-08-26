#include "netlib/Buffer.h"

#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>

namespace netlib {

ssize_t Buffer::readFd(int fd, int* savedErrno) {
    char extrabuf[config::kBufferExtra];
    iovec vec[2];
    const size_t writable = writableBytes();
    vec[0].iov_base = begin() + writeIdx_;
    vec[0].iov_len = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);
    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, iovcnt);
    if (n < 0) {
        *savedErrno = errno;
    } else if (static_cast<size_t>(n) <= writable) {
        writeIdx_ += static_cast<size_t>(n);
    } else {
        writeIdx_ = buf_.size();
        append(extrabuf, static_cast<size_t>(n) - writable);
    }
    return n;
}

} // namespace netlib
