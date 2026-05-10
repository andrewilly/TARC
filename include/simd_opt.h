#pragma once
// ============================================================================
// simd_opt.h — SIMD-optimized buffer operations per TARC STRIKE
// ============================================================================
// Fornisce versioni ottimizzate SIMD di memcpy, memset, memcmp e utilita
// per le operazioni critiche del motore (SFX copy, solid buffer, verify).
//
// Auto-detect CPU features a runtime e fallback graceful su hardware senza SIMD.
// Supporta: AVX2 (256-bit), SSE4.2 (128-bit), NEON (ARM), Scalar fallback.
//
// Compilatori supportati: MSVC, GCC, Clang.
// ============================================================================

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>

// ============================================================================
// SIMD Headers — inclusi solo se supportati dal compilatore
// ============================================================================
#if defined(_MSC_VER)
    // MSVC: <intrin.h> fornisce __cpuid, __cpuidex, e tutte le x86 intrinsics
    #include <intrin.h>
#elif defined(__AVX2__)
    #include <immintrin.h>
#elif defined(__SSE2__)
    #include <emmintrin.h>
#endif

#if defined(__ARM_NEON) || defined(__aarch64__)
    #include <arm_neon.h>
#endif

// ============================================================================
// Compile-time: quali set di intrinsics sono disponibili in questo build?
// ============================================================================
// Su MSVC x86_64, SSE2 e' sempre disponibile (garantito dall'architettura).
// Su MSVC, __AVX2__ viene definito solo se /arch:AVX2 e' specificato.
// Su GCC/Clang, __SSE2__ e __AVX2__ vengono definiti da -march/-msse2/-mavx2.
// ============================================================================

#if defined(__AVX2__)
    #define TARC_COMPILER_AVX2 1
#endif

#if defined(__SSE2__) || (defined(_MSC_VER) && defined(_M_X64))
    #define TARC_COMPILER_SSE2 1
#endif

#if defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
    #define TARC_COMPILER_NEON 1
#endif

// ============================================================================
// CPU Feature Detection — runtime, no dependencies
// ============================================================================

namespace SimdOpt {

enum CpuFeatures : uint32_t {
    SIMD_NONE    = 0,
    SIMD_SSE2    = 1 << 0,
    SIMD_SSE42   = 1 << 1,
    SIMD_AVX     = 1 << 2,
    SIMD_AVX2    = 1 << 3,
    SIMD_NEON    = 1 << 4,
};

inline uint32_t detect_cpu_features() {
    uint32_t features = SIMD_NONE;

#if defined(_MSC_VER)
    // MSVC: usa __cpuid / __cpuidex (da <intrin.h>)
    int cpuinfo[4] = {};
    __cpuid(cpuinfo, 1);
    if (cpuinfo[2] & (1 << 20)) features |= SIMD_SSE42;
    if (cpuinfo[2] & (1 << 28)) features |= SIMD_AVX;

    // AVX2: necessita extended leaf 7
    __cpuid(cpuinfo, 0);
    if (cpuinfo[0] >= 7) {
        __cpuidex(cpuinfo, 7, 0);
        if (cpuinfo[1] & (1 << 5)) features |= SIMD_AVX2;
    }
    // SSE2 sempre disponibile su x86_64
    features |= SIMD_SSE2;

#elif defined(__x86_64__) || defined(__i386__)
    // GCC/Clang: usa inline asm cpuid
    uint32_t eax, ebx, ecx, edx;
    // "=b" gia' informa il compilatore che rbx viene sovrascritto da cpuid.
    // Non serve il clobber "rbx" aggiuntivo (su PIC genera impossible constraints).
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    if (edx & (1 << 26)) features |= SIMD_SSE2;
    if (ecx & (1 << 20)) features |= SIMD_SSE42;
    if (ecx & (1 << 28)) features |= SIMD_AVX;

    // AVX2: leaf 7, sub-leaf 0
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
    if (ebx & (1 << 5)) features |= SIMD_AVX2;

#elif defined(TARC_COMPILER_NEON)
    features |= SIMD_NEON;
#endif

    return features;
}

// Lazy-init singleton per CPU features (detect una sola volta)
inline uint32_t cpu_features() {
    static uint32_t features = detect_cpu_features();
    return features;
}

inline bool has_avx2()   { return (cpu_features() & SIMD_AVX2) != 0; }
inline bool has_sse42()  { return (cpu_features() & SIMD_SSE42) != 0; }
inline bool has_sse2()   { return (cpu_features() & SIMD_SSE2) != 0; }
inline bool has_neon()   { return (cpu_features() & SIMD_NEON) != 0; }

// ============================================================================
// SIMD-Accelerated Memory Operations
// ============================================================================

// --- simd_memcpy: copia ottimizzata con SIMD per buffer grandi ---
// Per buffer < 4KB usa memcpy standard (overhead SIMD > beneficio).
// Per buffer >= 4KB usa copia a blocchi 256-bit (AVX2) o 128-bit (SSE).
inline void* simd_memcpy(void* __restrict dst, const void* __restrict src, size_t len) {
    if (len < 4096 || len == 0) {
        return std::memcpy(dst, src, len);
    }

#if defined(TARC_COMPILER_AVX2)
    if (has_avx2()) {
        size_t i = 0;
        const size_t vec_size = 32;
        size_t aligned_len = len & ~(vec_size - 1);

        for (; i < aligned_len; i += vec_size) {
            __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(
                static_cast<const char*>(src) + i));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(
                static_cast<char*>(dst) + i), v);
        }
        if (i < len) {
            std::memcpy(static_cast<char*>(dst) + i,
                        static_cast<const char*>(src) + i, len - i);
        }
        return dst;
    }
#endif

#if defined(TARC_COMPILER_SSE2)
    if (has_sse2()) {
        size_t i = 0;
        const size_t vec_size = 16;
        size_t aligned_len = len & ~(vec_size - 1);

        for (; i < aligned_len; i += vec_size) {
            __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(
                static_cast<const char*>(src) + i));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(
                static_cast<char*>(dst) + i), v);
        }
        if (i < len) {
            std::memcpy(static_cast<char*>(dst) + i,
                        static_cast<const char*>(src) + i, len - i);
        }
        return dst;
    }
#endif

#if defined(TARC_COMPILER_NEON)
    if (has_neon()) {
        size_t i = 0;
        const size_t vec_size = 16;
        size_t aligned_len = len & ~(vec_size - 1);

        for (; i < aligned_len; i += vec_size) {
            uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(
                static_cast<const char*>(src) + i));
            vst1q_u8(reinterpret_cast<uint8_t*>(
                static_cast<char*>(dst) + i), v);
        }
        if (i < len) {
            std::memcpy(static_cast<char*>(dst) + i,
                        static_cast<const char*>(src) + i, len - i);
        }
        return dst;
    }
#endif

    return std::memcpy(dst, src, len);
}

// --- simd_memset_zero: zero-fill ottimizzato con SIMD ---
inline void* simd_memset_zero(void* dst, size_t len) {
    if (len == 0) return dst;

#if defined(TARC_COMPILER_AVX2)
    if (has_avx2()) {
        __m256i zero = _mm256_setzero_si256();
        size_t i = 0;
        const size_t vec_size = 32;
        size_t aligned_len = len & ~(vec_size - 1);

        for (; i < aligned_len; i += vec_size) {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(
                static_cast<char*>(dst) + i), zero);
        }
        if (i + 16 <= len) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(
                static_cast<char*>(dst) + i), _mm_setzero_si128());
            i += 16;
        }
        if (i < len) {
            std::memset(static_cast<char*>(dst) + i, 0, len - i);
        }
        return dst;
    }
#endif

#if defined(TARC_COMPILER_SSE2)
    if (has_sse2()) {
        __m128i zero = _mm_setzero_si128();
        size_t i = 0;
        const size_t vec_size = 16;
        size_t aligned_len = len & ~(vec_size - 1);

        for (; i < aligned_len; i += vec_size) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(
                static_cast<char*>(dst) + i), zero);
        }
        if (i < len) {
            std::memset(static_cast<char*>(dst) + i, 0, len - i);
        }
        return dst;
    }
#endif

#if defined(TARC_COMPILER_NEON)
    if (has_neon()) {
        uint8x16_t zero = vdupq_n_u8(0);
        size_t i = 0;
        const size_t vec_size = 16;
        size_t aligned_len = len & ~(vec_size - 1);

        for (; i < aligned_len; i += vec_size) {
            vst1q_u8(reinterpret_cast<uint8_t*>(
                static_cast<char*>(dst) + i), zero);
        }
        if (i < len) {
            std::memset(static_cast<char*>(dst) + i, 0, len - i);
        }
        return dst;
    }
#endif

    return std::memset(dst, 0, len);
}

// --- simd_memcmp: confronto ottimizzato con SIMD ---
// Ritorna 0 se identici, !=0 altrimenti (come memcmp).
// Per verification integrity: compara blocchi decompressi con expected.
inline int simd_memcmp(const void* a, const void* b, size_t len) {
    if (len == 0) return 0;
    if (len < 64) return std::memcmp(a, b, len);

#if defined(TARC_COMPILER_AVX2)
    if (has_avx2()) {
        const uint8_t* pa = static_cast<const uint8_t*>(a);
        const uint8_t* pb = static_cast<const uint8_t*>(b);
        size_t i = 0;
        const size_t vec_size = 32;
        size_t aligned_len = len & ~(vec_size - 1);

        for (; i < aligned_len; i += vec_size) {
            __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pa + i));
            __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pb + i));
            __m256i cmp = _mm256_xor_si256(va, vb);
            if (!_mm256_testz_si256(cmp, cmp)) {
                return std::memcmp(pa + i, pb + i, len - i);
            }
        }
        if (i < len) return std::memcmp(pa + i, pb + i, len - i);
        return 0;
    }
#endif

#if defined(TARC_COMPILER_SSE2)
    if (has_sse2()) {
        const uint8_t* pa = static_cast<const uint8_t*>(a);
        const uint8_t* pb = static_cast<const uint8_t*>(b);
        size_t i = 0;
        const size_t vec_size = 16;
        size_t aligned_len = len & ~(vec_size - 1);

        for (; i < aligned_len; i += vec_size) {
            __m128i va = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pa + i));
            __m128i vb = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pb + i));
            __m128i cmp = _mm_xor_si128(va, vb);
            // _mm_test_all_zeros e' SSE4.2 — versione SSE2 compatibile:
            // se xor != 0, almeno un byte differisce
            if (_mm_movemask_epi8(_mm_cmpeq_epi8(cmp, _mm_setzero_si128())) != 0xFFFF) {
                return std::memcmp(pa + i, pb + i, len - i);
            }
        }
        if (i < len) return std::memcmp(pa + i, pb + i, len - i);
        return 0;
    }
#endif

#if defined(TARC_COMPILER_NEON)
    if (has_neon()) {
        const uint8_t* pa = static_cast<const uint8_t*>(a);
        const uint8_t* pb = static_cast<const uint8_t*>(b);
        size_t i = 0;
        const size_t vec_size = 16;
        size_t aligned_len = len & ~(vec_size - 1);

        for (; i < aligned_len; i += vec_size) {
            uint8x16_t va = vld1q_u8(pa + i);
            uint8x16_t vb = vld1q_u8(pb + i);
            if (vminvq_u8(vceqq_u8(va, vb)) == 0) {
                return std::memcmp(pa + i, pb + i, len - i);
            }
        }
        if (i < len) return std::memcmp(pa + i, pb + i, len - i);
        return 0;
    }
#endif

    return std::memcmp(a, b, len);
}

// --- simd_buffer_xor: XOR two buffers (utile per diff/cipher) ---
inline void simd_buffer_xor(void* __restrict dst, const void* __restrict a,
                             const void* __restrict b, size_t len) {
    if (len == 0) return;

#if defined(TARC_COMPILER_AVX2)
    if (has_avx2() && len >= 32) {
        const size_t vec_size = 32;
        size_t aligned_len = len & ~(vec_size - 1);
        size_t i = 0;
        for (; i < aligned_len; i += vec_size) {
            __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(
                static_cast<const char*>(a) + i));
            __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(
                static_cast<const char*>(b) + i));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(
                static_cast<char*>(dst) + i), _mm256_xor_si256(va, vb));
        }
        if (i < len) {
            auto* d = static_cast<uint8_t*>(dst) + i;
            auto* aa = static_cast<const uint8_t*>(a) + i;
            auto* bb = static_cast<const uint8_t*>(b) + i;
            for (size_t j = 0; j < len - i; j++) d[j] = aa[j] ^ bb[j];
        }
        return;
    }
#endif

    // Scalar fallback
    auto* d = static_cast<uint8_t*>(dst);
    auto* aa = static_cast<const uint8_t*>(a);
    auto* bb = static_cast<const uint8_t*>(b);
    for (size_t i = 0; i < len; i++) d[i] = aa[i] ^ bb[i];
}

// ============================================================================
// SIMD info — per il banner e il debug
// ============================================================================
// Mostra il livello SIMD piu' alto che il binario PUO' usare (compile-time)
// E che la CPU SUPPORTA (runtime). Se il compilatore non ha AVX2 ma la CPU si,
// mostra SSE4.2/SSE2 (il massimo disponibile nel build).
// ============================================================================
inline std::string simd_info_string() {
    std::string info;

#if defined(TARC_COMPILER_AVX2)
    // Se il binario ha le intrinsics AVX2, mostra AVX2 se la CPU lo supporta
    if (has_avx2()) {
        info += "AVX2 ";
    } else if (has_sse42()) {
        info += "SSE4.2 ";
    } else if (has_sse2()) {
        info += "SSE2 ";
    }
#elif defined(TARC_COMPILER_SSE2)
    // Binario con solo SSE2 (MSVC x86_64 senza /arch:AVX2, o GCC/Clang -msse2)
    if (has_sse42()) {
        info += "SSE4.2 ";
    } else if (has_sse2()) {
        info += "SSE2 ";
    }
#endif

#if defined(TARC_COMPILER_NEON)
    if (has_neon()) info += "NEON ";
#endif

    if (info.empty()) info = "Scalar";
    else info.pop_back();  // rimuovi spazio finale
    return info;
}

} // namespace SimdOpt
