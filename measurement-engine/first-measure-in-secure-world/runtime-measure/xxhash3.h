#ifndef XXH3_C_H
#define XXH3_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public APIs */
uint64_t XXH3_64bits(const void* input, size_t len);
uint64_t XXH3_64bits_withSeed(const void* input, size_t len, uint64_t seed);
uint64_t XXH3_64bits_withSecret(const void* input, size_t len, const void* secret, size_t secretSize);

#ifdef __cplusplus
}
#endif

#endif /* XXH3_C_H */
