#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

namespace perfacet {

std::string randomHex(std::size_t nBytes);
std::string iso8601Utc(uint64_t unixMs);

} // namespace perfacet
