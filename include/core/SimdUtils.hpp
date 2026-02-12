#pragma once
#include <string>
#include <cstddef>

namespace UltraImageViewer {
namespace Core {
namespace Simd {

// Call once at startup (CPUID detection)
void DetectFeatures();
bool HasAVX2();
bool HasSSE42();

// AVX2/SSE2 accelerated wchar_t in-place lowercasing.
// Only converts ASCII A-Z (0x41-0x5A) to a-z (0x61-0x7A).
// Non-ASCII characters are preserved (Windows paths are ASCII case-insensitive).
void ToLowerInPlace(wchar_t* data, size_t length);

inline void ToLowerInPlace(std::wstring& s) {
    ToLowerInPlace(s.data(), s.size());
}

} // namespace Simd
} // namespace Core
} // namespace UltraImageViewer
