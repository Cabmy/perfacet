#include "detail/Random.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace perfacet {

std::string randomHex(std::size_t nBytes) {
    std::string out;
    out.resize(nBytes * 2);
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    unsigned char buf[64];
    std::size_t got = 0;
    if (urandom) {
        while (got < nBytes) {
            const std::size_t chunk = std::min(nBytes - got, sizeof(buf));
            urandom.read(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(chunk));
            if (!urandom) break;
            const auto n = static_cast<std::size_t>(urandom.gcount());
            for (std::size_t i = 0; i < n; ++i) {
                static constexpr char kHex[] = "0123456789abcdef";
                out[(got + i) * 2] = kHex[buf[i] >> 4];
                out[(got + i) * 2 + 1] = kHex[buf[i] & 0xf];
            }
            got += n;
        }
    }
    if (got < nBytes) {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<int> dist(0, 255);
        for (std::size_t i = got; i < nBytes; ++i) {
            const int v = dist(gen);
            static constexpr char kHex[] = "0123456789abcdef";
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
