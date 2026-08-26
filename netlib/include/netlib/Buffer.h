#pragma once
// netlib::Buffer —— 三段式缓冲：[0, readIdx_) prependable、[readIdx_, writeIdx_) readable、
// [writeIdx_, size) writable。所有多字节整数使用网络字节序（大端）。
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "netlib/Config.h"

namespace netlib {

class Buffer {
public:
    explicit Buffer(size_t init = config::kBufferInit)
        : buf_(config::kBufferPrepend + init), readIdx_(config::kBufferPrepend),
          writeIdx_(config::kBufferPrepend) {}

    size_t readableBytes() const { return writeIdx_ - readIdx_; }
    size_t writableBytes() const { return buf_.size() - writeIdx_; }
    size_t prependableBytes() const { return readIdx_; }

    const char* peek() const { return begin() + readIdx_; }

    void retrieve(size_t n) {
        if (n < readableBytes()) {
            readIdx_ += n;
        } else {
            retrieveAll();
        }
    }
    void retrieveAll() {
        readIdx_ = config::kBufferPrepend;
        writeIdx_ = config::kBufferPrepend;
    }
    // 大端整数 peek（不移出）
    uint16_t peekUint16() const { return peekUintN<uint16_t>(2); }

    uint16_t peekUint16At(size_t offset) const {
        return peekUintNAt<uint16_t>(offset, 2);
    }
    uint32_t peekUint32At(size_t offset) const {
        return peekUintNAt<uint32_t>(offset, 4);
    }
    uint64_t peekUint64At(size_t offset) const {
        return peekUintNAt<uint64_t>(offset, 8);
    }

    void retrieveUint16() { retrieve(2); }
    void retrieveUint32() { retrieve(4); }

    void append(const void* data, size_t len) {
        if (writableBytes() < len) makeSpace(len);
        std::memcpy(begin() + writeIdx_, data, len);
        writeIdx_ += len;
    }
    void append(std::string_view s) { append(s.data(), s.size()); }
    void appendUint8(uint8_t v) { append(&v, 1); }
    void appendUint16(uint16_t v) { appendUintN(v); }
    void appendUint32(uint32_t v) { appendUintN(v); }
    void appendUint64(uint64_t v) { appendUintN(v); }

    // 从 fd 读入（栈上 extrabuf + readv），返回读取字节数：
    // 0 表示 EOF；-1 且 errno==EAGAIN 表示暂时读完
    ssize_t readFd(int fd, int* savedErrno);

private:
    char* begin() { return buf_.data(); }
    const char* begin() const { return buf_.data(); }

    template <typename T>
    T peekUintN(size_t n) const {
        T v = 0;
        for (size_t i = 0; i < n; ++i) {
            v = (v << 8) | static_cast<uint8_t>(peek()[i]);
        }
        return v;
    }

    template <typename T>
    T peekUintNAt(size_t offset, size_t n) const {
        T v = 0;
        for (size_t i = 0; i < n; ++i) {
            v = (v << 8) | static_cast<uint8_t>(peek()[offset + i]);
        }
        return v;
    }

    template <typename T>
    void appendUintN(T v) {
        uint8_t buf[sizeof(T)];
        for (size_t i = 0; i < sizeof(T); ++i) {
            buf[sizeof(T) - 1 - i] = static_cast<uint8_t>(v & 0xFF);
            v >>= 8;
        }
        append(buf, sizeof(T));
    }

    void makeSpace(size_t len) {
        if (writableBytes() + prependableBytes() < len + config::kBufferPrepend) {
            buf_.resize(writeIdx_ + len);
        } else {
            // prependable 足够：整体前移
            size_t readable = readableBytes();
            std::memmove(begin() + config::kBufferPrepend, begin() + readIdx_, readable);
            readIdx_ = config::kBufferPrepend;
            writeIdx_ = readIdx_ + readable;
        }
    }

    std::vector<char> buf_;
    size_t readIdx_;
    size_t writeIdx_;
};

} // namespace netlib
