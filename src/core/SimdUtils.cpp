#include "core/SimdUtils.hpp"
#include <intrin.h>
#include <immintrin.h>

namespace UltraImageViewer {
namespace Core {
namespace Simd {

static bool s_hasAVX2 = false;
static bool s_hasSSE42 = false;

void DetectFeatures()
{
    int cpuInfo[4];

    __cpuid(cpuInfo, 0);
    int nIds = cpuInfo[0];

    // SSE4.2: Leaf 1, ECX bit 20
    __cpuid(cpuInfo, 1);
    s_hasSSE42 = (cpuInfo[2] & (1 << 20)) != 0;

    // AVX requires OS XSAVE support (Leaf 1, ECX bit 27) + AVX bit (ECX bit 28)
    bool hasAVX = (cpuInfo[2] & (1 << 27)) != 0 && (cpuInfo[2] & (1 << 28)) != 0;

    // AVX2: Leaf 7, Sub-leaf 0, EBX bit 5
    if (hasAVX && nIds >= 7) {
        __cpuidex(cpuInfo, 7, 0);
        s_hasAVX2 = (cpuInfo[1] & (1 << 5)) != 0;
    }
}

bool HasAVX2()  { return s_hasAVX2; }
bool HasSSE42() { return s_hasSSE42; }

// ---- AVX2 path: 16 wchar_t (32 bytes) per iteration ----
// Range check: (ch > 0x40) AND (0x5B > ch) identifies A-Z.
// For chars > 0x7FFF (CJK etc.), signed cmpgt sees them as negative,
// so (negative > 0x40) = false, making the AND mask 0. Safe.

static void ToLower_AVX2(wchar_t* data, size_t count)
{
    const __m256i v40 = _mm256_set1_epi16(0x0040);  // 'A' - 1
    const __m256i v5B = _mm256_set1_epi16(0x005B);  // 'Z' + 1
    const __m256i v20 = _mm256_set1_epi16(0x0020);  // lowercase bit

    size_t i = 0;
    for (; i + 16 <= count; i += 16) {
        __m256i chars = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));

        __m256i gt40 = _mm256_cmpgt_epi16(chars, v40);
        __m256i lt5B = _mm256_cmpgt_epi16(v5B, chars);
        __m256i mask = _mm256_and_si256(gt40, lt5B);

        __m256i lowBit = _mm256_and_si256(mask, v20);
        chars = _mm256_or_si256(chars, lowBit);

        _mm256_storeu_si256(reinterpret_cast<__m256i*>(data + i), chars);
    }

    // Scalar tail
    for (; i < count; ++i) {
        wchar_t c = data[i];
        if (c >= L'A' && c <= L'Z') c |= 0x0020;
        data[i] = c;
    }
}

// ---- SSE2 path: 8 wchar_t (16 bytes) per iteration ----

static void ToLower_SSE2(wchar_t* data, size_t count)
{
    const __m128i v40 = _mm_set1_epi16(0x0040);
    const __m128i v5B = _mm_set1_epi16(0x005B);
    const __m128i v20 = _mm_set1_epi16(0x0020);

    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        __m128i chars = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i));

        __m128i gt40 = _mm_cmpgt_epi16(chars, v40);
        __m128i lt5B = _mm_cmpgt_epi16(v5B, chars);
        __m128i mask = _mm_and_si128(gt40, lt5B);

        __m128i lowBit = _mm_and_si128(mask, v20);
        chars = _mm_or_si128(chars, lowBit);

        _mm_storeu_si128(reinterpret_cast<__m128i*>(data + i), chars);
    }

    // Scalar tail
    for (; i < count; ++i) {
        wchar_t c = data[i];
        if (c >= L'A' && c <= L'Z') c |= 0x0020;
        data[i] = c;
    }
}

void ToLowerInPlace(wchar_t* data, size_t length)
{
    if (length == 0 || !data) return;

    if (s_hasAVX2 && length >= 16) {
        ToLower_AVX2(data, length);
        return;
    }

    if (length >= 8) {
        ToLower_SSE2(data, length);
        return;
    }

    // Pure scalar for very short strings
    for (size_t i = 0; i < length; ++i) {
        wchar_t c = data[i];
        if (c >= L'A' && c <= L'Z') c |= 0x0020;
        data[i] = c;
    }
}

} // namespace Simd
} // namespace Core
} // namespace UltraImageViewer
