#include "detail/Random.h"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <sys/random.h>

namespace perfacet {

std::string randomHex(std::size_t nBytes) {
    std::string out;
    out.resize(nBytes * 2);
    std::size_t got = 0;
    unsigned char buf[64];
    while (got < nBytes) {
        const std::size_t chunk = std::min(nBytes - got, sizeof(buf));
        const ssize_t n = ::getrandom(buf, chunk, 0);
        if (n <= 0) break;
        static constexpr char kHex[] = "0123456789abcdef";
        for (ssize_t i = 0; i < n; ++i) {
            out[(got + static_cast<std::size_t>(i)) * 2] = kHex[buf[i] >> 4];
            out[(got + static_cast<std::size_t>(i)) * 2 + 1] = kHex[buf[i] & 0xf];
        }
        got += static_cast<std::size_t>(n);
    }
    if (got < nBytes) {
        thread_local std::mt19937_64 gen = [] {
            std::random_device rd;
            return std::mt19937_64(rd());
        }();
        std::uniform_int_distribution<int> dist(0, 255);
        static constexpr char kHex[] = "0123456789abcdef";
        for (std::size_t i = got; i < nBytes; ++i) {
            const int v = dist(gen);
            out[i * 2] = kHex[v >> 4];
            out[i * 2 + 1] = kHex[v & 0xf];
        }
    }
    return out;
}

std::string iso8601Utc(uint64_t unixMs) {
    const std::time_t sec = static_cast<std::time_t>(unixMs / 1000);
    const int ms = static_cast<int>(unixMs % 1000);
    std::tm tm{};
    gmtime_r(&sec, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.'
        << std::setw(3) << std::setfill('0') << ms << 'Z';
    return oss.str();
}

} // namespace perfacet
