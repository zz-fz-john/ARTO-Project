#include "xxhash3.h"
#include <string.h>
#include <stdlib.h>
#if defined(__GNUC__) || defined(__clang__)
#  define LIKELY(x)   __builtin_expect(!!(x), 1)
#  define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define LIKELY(x)   (x)
#  define UNLIKELY(x) (x)
#endif

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#  define XXH_LITTLE_ENDIAN 1
#else
#  define XXH_LITTLE_ENDIAN 0
#endif

/* Primes and constants (from your C++ source) */
static const uint32_t PRIME32_1 = 0x9E3779B1U;
static const uint32_t PRIME32_2 = 0x85EBCA77U;
static const uint32_t PRIME32_3 = 0xC2B2AE3DU;

static const uint64_t PRIME64_1 = 0x9E3779B185EBCA87ULL;
static const uint64_t PRIME64_2 = 0xC2B2AE3D27D4EB4FULL;
static const uint64_t PRIME64_3 = 0x165667B19E3779F9ULL;
static const uint64_t PRIME64_4 = 0x85EBCA77C2B2AE63ULL;
static const uint64_t PRIME64_5 = 0x27D4EB2F165667C5ULL;

static const size_t SECRET_DEFAULT_SIZE = 192;
static const size_t SECRET_SIZE_MIN = 136;

static const uint8_t kSecret[192] = {
    0xb8,0xfe,0x6c,0x39,0x23,0xa4,0x4b,0xbe,0x7c,0x01,0x81,0x2c,
    0xf7,0x21,0xad,0x1c,0xde,0xd4,0x6d,0xe9,0x83,0x90,0x97,0xdb,
    0x72,0x40,0xa4,0xa4,0xb7,0xb3,0x67,0x1f,0xcb,0x79,0xe6,0x4e,
    0xcc,0xc0,0xe5,0x78,0x82,0x5a,0xd0,0x7d,0xcc,0xff,0x72,0x21,
    0xb8,0x08,0x46,0x74,0xf7,0x43,0x24,0x8e,0xe0,0x35,0x90,0xe6,
    0x81,0x3a,0x26,0x4c,0x3c,0x28,0x52,0xbb,0x91,0xc3,0x00,0xcb,
    0x88,0xd0,0x65,0x8b,0x1b,0x53,0x2e,0xa3,0x71,0x64,0x48,0x97,
    0xa2,0x0d,0xf9,0x4e,0x38,0x19,0xef,0x46,0xa9,0xde,0xac,0xd8,
    0xa8,0xfa,0x76,0x3f,0xe3,0x9c,0x34,0x3f,0xf9,0xdc,0xbb,0xc7,
    0xc7,0x0b,0x4f,0x1d,0x8a,0x51,0xe0,0x4b,0xcd,0xb4,0x59,0x31,
    0xc8,0x9f,0x7e,0xc9,0xd9,0x78,0x73,0x64,0xea,0xc5,0xac,0x83,
    0x34,0xd3,0xeb,0xc3,0xc5,0x81,0xa0,0xff,0xfa,0x13,0x63,0xeb,
    0x17,0x0d,0xdd,0x51,0xb7,0xf0,0xda,0x49,0xd3,0x16,0x55,0x26,
    0x29,0xd4,0x68,0x9e,0x2b,0x16,0xbe,0x58,0x7d,0x47,0xa1,0xfc,
    0x8f,0xf8,0xb8,0xd1,0x7a,0xd0,0x31,0xce,0x45,0xcb,0x3a,0x8f,
    0x95,0x16,0x04,0x28,0xaf,0xd7,0xfb,0xca,0xbb,0x4b,0x40,0x7e
};

/* helpers: read/write little-endian, optimized with memcpy when possible */
static inline __attribute__((always_inline)) uint32_t readLE32(const uint8_t* p) {
#if XXH_LITTLE_ENDIAN
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
#else
    /* portable byte-wise fallback */
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
#endif
}

static inline __attribute__((always_inline)) uint64_t readLE64(const uint8_t* p) {
#if XXH_LITTLE_ENDIAN
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
#else
    uint64_t lo = readLE32(p);
    uint64_t hi = readLE32(p + 4);
    return lo | (hi << 32);
#endif
}

static inline __attribute__((always_inline)) void writeLE64(uint8_t* dst, uint64_t v) {
#if XXH_LITTLE_ENDIAN
    memcpy(dst, &v, sizeof(v));
#else
    for (int i = 0; i < 8; ++i) dst[i] = (uint8_t)(v >> (i * 8));
#endif
}

static inline uint32_t swap32(uint32_t x) {
    return ((x << 24) & 0xff000000u) |
           ((x << 8)  & 0x00ff0000u) |
           ((x >> 8)  & 0x0000ff00u) |
           ((x >> 24) & 0x000000ffu);
}

static inline uint64_t swap64(uint64_t x) {
    return ((x << 56) & 0xff00000000000000ULL) |
           ((x << 40) & 0x00ff000000000000ULL) |
           ((x << 24) & 0x0000ff0000000000ULL) |
           ((x << 8)  & 0x000000ff00000000ULL) |
           ((x >> 8)  & 0x00000000ff000000ULL) |
           ((x >> 24) & 0x0000000000ff0000ULL) |
           ((x >> 40) & 0x000000000000ff00ULL) |
           ((x >> 56) & 0x00000000000000ffULL);
}

/* 64x64 -> 128 multiply; then fold to 64 (xor of high/low) */
static inline __attribute__((always_inline)) uint64_t mul128_fold64(uint64_t a, uint64_t b) {
#if defined(__SIZEOF_INT128__)
    __uint128_t p = (__uint128_t)a * (__uint128_t)b;
    uint64_t lo = (uint64_t)p;
    uint64_t hi = (uint64_t)(p >> 64);
    return lo ^ hi;
#else
    /* portable decomposition fallback */
    uint64_t a_lo = (uint32_t)a;
    uint64_t a_hi = a >> 32;
    uint64_t b_lo = (uint32_t)b;
    uint64_t b_hi = b >> 32;

    uint64_t lo_lo = a_lo * b_lo;
    uint64_t lo_hi = a_lo * b_hi;
    uint64_t hi_lo = a_hi * b_lo;
    uint64_t hi_hi = a_hi * b_hi;

    uint64_t cross = (lo_lo >> 32) + (uint32_t)lo_hi + (uint32_t)hi_lo;
    uint64_t upper = (lo_hi >> 32) + (hi_lo >> 32) + hi_hi + (cross >> 32);
    uint64_t lower = (cross << 32) | (uint32_t)lo_lo;
    return lower ^ upper;
#endif
}

/* avalanches */
static inline uint64_t XXH64_avalanche(uint64_t h) {
    h = (h ^ (h >> 33)) * PRIME64_2;
    h = (h ^ (h >> 29)) * PRIME64_3;
    h ^= (h >> 32);
    return h;
}

static inline uint64_t XXH3_avalanche(uint64_t h) {
    h = (h ^ (h >> 37)) * 0x165667919E3779F9ULL;
    h ^= (h >> 32);
    return h;
}

static inline uint64_t rrmxmx(uint64_t h, uint64_t len) {
    uint64_t x1 = (h << 49) | (h >> (64 - 49));
    uint64_t x2 = (h << 24) | (h >> (64 - 24));
    h ^= (x1 ^ x2);
    h *= 0x9FB21C651E98DF25ULL;
    h ^= (h >> 35) + len;
    h *= 0x9FB21C651E98DF25ULL;
    h ^= (h >> 28);
    return h;
}

/* mix 16 bytes with secret and seed */
static inline uint64_t mix16B(const uint8_t* input, const uint8_t* secret, uint64_t seed) {
    uint64_t a = readLE64(input) ^ (readLE64(secret) + seed);
    uint64_t b = readLE64(input + 8) ^ (readLE64(secret + 8) - seed);
    return mul128_fold64(a, b);
}

/* stripe & accumulation constants */
enum { STRIPE_LEN = 64 };
enum { SECRET_CONSUME_RATE = 8 };
enum { ACC_NB = STRIPE_LEN / 8 };

/* accumulate 512 bits (64 bytes) into acc array */
static void accumulate_512(uint64_t* acc, const uint8_t* input, const uint8_t* secret) {
    const uint8_t* in = input;
    const uint8_t* sec = secret;
    for (size_t i = 0; i < ACC_NB; ++i) {
        uint64_t data_val = readLE64(in);
        uint64_t data_key = data_val ^ readLE64(sec);
        acc[i ^ 1] += data_val;
        acc[i] += (uint32_t)data_key * (data_key >> 32);
        in += 8;
        sec += 8;
    }
}

/* hashing long inputs (>= 240) using secret */
static uint64_t hashLong_64b_internal(const uint8_t* input, size_t len, const uint8_t* secret, size_t secretSize) {
    uint64_t acc[ACC_NB] = {
        (uint64_t)PRIME32_3, PRIME64_1, PRIME64_2, PRIME64_3,
        PRIME64_4, (uint64_t)PRIME32_2, PRIME64_5, (uint64_t)PRIME32_1
    };

    size_t nbStripesPerBlock = (secretSize - STRIPE_LEN) / SECRET_CONSUME_RATE;
    if (nbStripesPerBlock == 0) nbStripesPerBlock = 1;
    size_t block_len = STRIPE_LEN * nbStripesPerBlock;
    size_t nb_blocks = (len - 1) / block_len;

    const uint8_t* in_ptr = input;
    for (size_t n = 0; n < nb_blocks; ++n) {
        const uint8_t* s_ptr = secret;
        for (size_t i = 0; i < nbStripesPerBlock; ++i) {
#if defined(__GNUC__) || defined(__clang__)
            __builtin_prefetch(in_ptr + i * STRIPE_LEN + 64, 0, 0);
#endif
            accumulate_512(acc, in_ptr + i * STRIPE_LEN, s_ptr);
            s_ptr += SECRET_CONSUME_RATE;
        }
        in_ptr += block_len;
        const uint8_t* secret_tail = secret + secretSize - STRIPE_LEN;
        for (size_t i = 0; i < ACC_NB; ++i) {
            acc[i] = (acc[i] ^ (acc[i] >> 47) ^ readLE64(secret_tail + 8 * i)) * PRIME32_1;
        }
    }

    size_t nbStripes = ((len - 1) - (block_len * nb_blocks)) / STRIPE_LEN;
    const uint8_t* s_ptr2 = secret;
    for (size_t i = 0; i < nbStripes; ++i) {
        accumulate_512(acc, in_ptr + i * STRIPE_LEN, s_ptr2);
        s_ptr2 += SECRET_CONSUME_RATE;
    }

    /* last stripe */
    accumulate_512(acc, input + len - STRIPE_LEN,
                   secret + secretSize - STRIPE_LEN - 7);

    uint64_t result = (uint64_t)len * PRIME64_1;
    for (size_t i = 0; i < 4; ++i) {
        uint64_t a = acc[2*i] ^ readLE64(secret + 11 + 16 * i);
        uint64_t b = acc[2*i + 1] ^ readLE64(secret + 11 + 16 * i + 8);
        result += mul128_fold64(a, b);
    }
    return XXH3_avalanche(result);
}

/* core internal selecting branches based on length */
static uint64_t XXH3_64bits_internal(const uint8_t* input, size_t len, uint64_t seed, const uint8_t* secret, size_t secretLen) {
    if (UNLIKELY(len == 0)) {
        uint64_t s1 = readLE64(secret + 56);
        uint64_t s2 = readLE64(secret + 64);
        return XXH64_avalanche(seed ^ (s1 ^ s2));
    } else if (UNLIKELY(len < 4)) {
        uint32_t c1 = (uint32_t)input[0];
        uint32_t c2 = (uint32_t)input[len >> 1];
        uint32_t c3 = (uint32_t)input[len - 1];
        uint64_t keyed = ((c1 << 16) | (c2 << 24) | c3 | ((uint32_t)len << 8))
                         ^ ((readLE32(secret) ^ readLE32(secret + 4)) + (uint32_t)seed);
        return XXH64_avalanche(keyed);
    } else if (len <= 8) {
        uint64_t lo = readLE32(input) ;
        uint64_t hi = readLE32(input + len - 4);
        uint64_t keyed = (hi + (lo << 32)) ^ ((readLE64(secret + 8) ^ readLE64(secret + 16)) - (seed ^ ((uint64_t)swap32((uint32_t)seed) << 32)));
        return rrmxmx(keyed, len);
    } else if (len <= 16) {
        uint64_t input_lo = readLE64(input) ^ ((readLE64(secret + 24) ^ readLE64(secret + 32)) + seed);
        uint64_t input_hi = readLE64(input + len - 8) ^ ((readLE64(secret + 40) ^ readLE64(secret + 48)) - seed);
        uint64_t acc = (uint64_t)len + swap64(input_lo) + input_hi + mul128_fold64(input_lo, input_hi);
        return XXH3_avalanche(acc);
    } else if (len <= 128) {
        uint64_t acc = (uint64_t)len * PRIME64_1;
        size_t secret_off = 0;
        size_t i = 0, j = len;
        while (j > i) {
            acc += mix16B(input + i, secret + secret_off, seed);
            if (j >= 16) {
                acc += mix16B(input + j - 16, secret + secret_off + 16, seed);
            }
            secret_off += 32;
            i += 16;
            j -= 16;
        }
        return XXH3_avalanche(acc);
    } else if (len <= 240) {
        uint64_t acc = (uint64_t)len * PRIME64_1;
        for (size_t i = 0; i < 128; i += 16)
            acc += mix16B(input + i, secret + i, seed);
        acc = XXH3_avalanche(acc);
        size_t limit = (len / 16) * 16;
        for (size_t i = 128; i < limit; i += 16)
            acc += mix16B(input + i, secret + (i - 128) + 3, seed);
        acc += mix16B(input + len - 16, secret + SECRET_SIZE_MIN - 17, seed);
        return XXH3_avalanche(acc);
    } else {
        return hashLong_64b_internal(input, len, secret, secretLen);
    }
}

/* Public APIs */

uint64_t XXH3_64bits_withSecret(const void* inputVoid, size_t len, const void* secretVoid, size_t secretSize) {
    const uint8_t* input = (const uint8_t*)inputVoid;
    const uint8_t* secret = (const uint8_t*)secretVoid;
    if (len == 0) return XXH64_avalanche( (uint64_t) (readLE64(secret + 56) ^ readLE64(secret + 64)) );
    return XXH3_64bits_internal(input, len, 0, secret, secretSize);
}

uint64_t XXH3_64bits_withSeed(const void* inputVoid, size_t len, uint64_t seed) {
    const uint8_t* input = (const uint8_t*)inputVoid;
    if (seed == 0) {
        /* default seed path uses kSecret */
        return XXH3_64bits_internal(input, len, 0, kSecret, SECRET_DEFAULT_SIZE);
    } else {
        uint8_t secret[SECRET_DEFAULT_SIZE];
        for (size_t i = 0; i < SECRET_DEFAULT_SIZE; i += 16) {
            uint64_t s0 = readLE64(kSecret + i) + seed;
            uint64_t s1 = readLE64(kSecret + i + 8) - seed;
            writeLE64(secret + i, s0);
            writeLE64(secret + i + 8, s1);
        }
        return XXH3_64bits_internal(input, len, seed, secret, SECRET_DEFAULT_SIZE);
    }
}

uint64_t XXH3_64bits(const void* input, size_t len) {
    return XXH3_64bits_withSeed(input, len, 0);
}
