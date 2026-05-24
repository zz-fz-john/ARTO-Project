#pragma once
#ifdef __cplusplus
#define COLLECTTIME
extern "C"{
#endif
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include "CFeventSingleThread.h"
#define HASHMAP_SIZE  0x1001
#define MAX_SET_COUNT 150
//#define MAX_SET_SIZE  509
#ifdef SMALLTEST
    #define MAX_SET_SIZE 4096 
#else
    // #define MAX_SET_SIZE 4096
    #define MAX_SET_SIZE 8192
#endif
#define SET_MASK (MAX_SET_SIZE - 1)
#define IDX(addr) ((addr) & SET_MASK)
typedef struct node {
    uint32_t key;
    uint32_t value;
    int setID;
    struct node *next;
} node_t;
typedef struct hashmap {
    node_t *bucket[HASHMAP_SIZE];
    uint32_t p;
} hashmap_t;
/*!
 * \brief hashmap_loopup
 * Look up a data_use event in the current hashmap;
 */
__attribute__((section(".trampoline")))
node_t* hashmap_lookup(hashmap_t *hmap, uint32_t key);

/*!
 * \brief hashmap_update
 * insert/modify a data_def event in the current hashmap;
 */
__attribute__((section(".trampoline")))
void hashmap_update(hashmap_t *hmap, uint32_t key, uint32_t value,int setID); 
__attribute__((section(".trampoline")))
    void def_check(uint32_t addr ,uint32_t val,int setID);
__attribute__((section(".trampoline")))
    void use_check(uint32_t addr ,uint32_t val,int setID);
__attribute__((section(".trampoline")))
    void Critical_def_check(uint32_t addr ,uint32_t val,int setID,uint32_t maxvalue,uint32_t minvalue);
__attribute__((section(".trampoline")))
    void non_sen_def_check(uint32_t addr );
__attribute__((section(".trampoline")))
    void use_check_for_basic_type_in_struct(uint32_t addr ,uint32_t val,int setID);
__attribute__((section(".trampoline")))
    void def_check_for_basic_type_in_struct(uint32_t addr ,uint32_t val,int setID);
__attribute__((section(".trampoline")))
    void Critical_def_check_for_float(uint32_t addr ,float val,int setID,float maxvalue,float minvalue);
#ifdef COLLECTTIME
__attribute__((section(".trampoline")))
    void def_collect(uint32_t val,uint32_t CriticalPointID);
#endif
#ifdef __cplusplus
}
#endif
