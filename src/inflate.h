// Minimal DEFLATE decoder (RFC 1951) with gzip (RFC 1952) and zlib (RFC 1950) wrappers. No external dependency.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace wx {

// Raw deflate stream. Returns false on corrupt/truncated input or when output would exceed maxOut bytes.
bool inflateRaw(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t maxOut, size_t* consumed = nullptr);
bool gunzip(const std::string& in, std::vector<uint8_t>& out, size_t maxOut);
bool zlibDecompress(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t maxOut);

} // namespace wx
