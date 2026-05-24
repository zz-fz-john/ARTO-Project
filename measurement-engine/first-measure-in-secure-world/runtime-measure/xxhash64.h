#ifdef __cplusplus
extern "C"{
#endif
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef struct {
    uint64_t state[4];
    unsigned char buffer[32];   // MaxBufferSize = 31+1
    uint64_t bufferSize;
    uint64_t totalLength;
} XXHash64;
void XXHash64_init(XXHash64 *h, uint64_t seed);
bool XXHash64_add(XXHash64 *h, const void *input, uint32_t len);
uint64_t XXHash64_hash(const XXHash64 *h);
uint64_t XXHash64_hashBuffer(const void *input, uint32_t length, uint64_t seed);
#ifdef __cplusplus
}
#endif