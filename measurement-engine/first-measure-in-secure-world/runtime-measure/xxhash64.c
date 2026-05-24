#pragma once
#include "xxhash64.h"
/* primes */
#define PRIME1 11400714785074694791ULL
#define PRIME2 14029467366897019727ULL
#define PRIME3 1609587929392839161ULL
#define PRIME4 9650029242287828579ULL
#define PRIME5 2870177450012600261ULL

#define MAX_BUFFER 32

/* rotate left 64, optimized for ARM */
static inline uint64_t rotl64(uint64_t x, unsigned n) {
#if defined(__has_builtin)
#  if __has_builtin(__builtin_rotateleft64)
    return __builtin_rotateleft64(x, n);
#  endif
#endif
    /* fallback */
    return (x << n) | (x >> (64 - n));
}

static inline uint64_t processSingle(uint64_t previous, uint64_t input) {
    return rotl64(previous + input * PRIME2, 31) * PRIME1;
}

/* main block processing */
static inline void processBlock(const void *data,
    uint64_t *s0, uint64_t *s1, uint64_t *s2, uint64_t *s3)
{
    const uint64_t *block = (const uint64_t *)data;
    *s0 = processSingle(*s0, block[0]);
    *s1 = processSingle(*s1, block[1]);
    *s2 = processSingle(*s2, block[2]);
    *s3 = processSingle(*s3, block[3]);
}

/* init */
 void XXHash64_init(XXHash64 *h, uint64_t seed)
{
    h->state[0] = seed + PRIME1 + PRIME2;
    h->state[1] = seed + PRIME2;
    h->state[2] = seed;
    h->state[3] = seed - PRIME1;
    h->bufferSize  = 0;
    h->totalLength = 0;
}

/* update */
 bool XXHash64_add(XXHash64 *h, const void *input, uint32_t length)
{
    if (!input || length == 0)
        return false;

    h->totalLength += length;
    const uint8_t *data = (const uint8_t*)input;

    if (h->bufferSize + length < MAX_BUFFER)
    {
        while (length--)
            h->buffer[h->bufferSize++] = *data++;
        return true;
    }

    const uint8_t *stop      = data + length;
    const uint8_t *stopBlock = stop - MAX_BUFFER;

    if (h->bufferSize > 0)
    {
        while (h->bufferSize < MAX_BUFFER)
            h->buffer[h->bufferSize++] = *data++;

        processBlock(h->buffer, &h->state[0], &h->state[1], &h->state[2], &h->state[3]);
    }

    uint64_t s0 = h->state[0];
    uint64_t s1 = h->state[1];
    uint64_t s2 = h->state[2];
    uint64_t s3 = h->state[3];

    while (data <= stopBlock)
    {
        processBlock(data, &s0, &s1, &s2, &s3);
        data += 32;
    }

    h->state[0] = s0;
    h->state[1] = s1;
    h->state[2] = s2;
    h->state[3] = s3;

    h->bufferSize = stop - data;
    for (uint32_t i=0; i< h->bufferSize; i++)
        h->buffer[i] = data[i];

    return true;
}

/* finalize */
uint64_t XXHash64_hash(const XXHash64 *h)
{
    uint64_t result;

    if (h->totalLength >= MAX_BUFFER)
    {
        result =
            rotl64(h->state[0], 1) +
            rotl64(h->state[1], 7) +
            rotl64(h->state[2], 12) +
            rotl64(h->state[3], 18);

        result = (result ^ processSingle(0, h->state[0])) * PRIME1 + PRIME4;
        result = (result ^ processSingle(0, h->state[1])) * PRIME1 + PRIME4;
        result = (result ^ processSingle(0, h->state[2])) * PRIME1 + PRIME4;
        result = (result ^ processSingle(0, h->state[3])) * PRIME1 + PRIME4;
    }
    else
    {
        result = h->state[2] + PRIME5;
    }

    result += h->totalLength;

    const uint8_t *data = h->buffer;
    const uint8_t *stop = data + h->bufferSize;

    while (data + 8 <= stop)
    {
        uint64_t v = *(uint64_t*)data;
        result = rotl64(result ^ processSingle(0, v), 27) * PRIME1 + PRIME4;
        data += 8;
    }

    if (data + 4 <= stop)
    {
        uint32_t v = *(uint32_t*)data;
        result = rotl64(result ^ (v * PRIME1), 23) * PRIME2 + PRIME3;
        data += 4;
    }

    while (data != stop)
        result = rotl64(result ^ (*data++ * PRIME5), 11) * PRIME1;

    /* avalanche */
    result ^= result >> 33;
    result *= PRIME2;
    result ^= result >> 29;
    result *= PRIME3;
    result ^= result >> 32;

    return result;
}

/* one-shot API */
uint64_t XXHash64_hashBuffer(const void *input, uint32_t length, uint64_t seed)
{
    XXHash64 h;
    XXHash64_init(&h, seed);
    XXHash64_add(&h, input, length);
    return XXHash64_hash(&h);
}

