//该版本该版本是在一个段结束后，使用哈希查找来加速对控制流完整性的查找，对产生的证据进行一个验证，判断这个段的执行路径是否合法。
#pragma once
#ifdef __cplusplus
extern "C"{
#endif
#define _GNU_SOURCE   
#include <string.h>
//#include <stdint.h>
//#include <stddef.h>

#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sched.h>
#include <time.h>
#include <fcntl.h>
#include <sys/timerfd.h>
#ifdef HARDWARESECURE
    #include <tee_client_api.h>
#endif
#include <sys/uio.h> // for writev
#include <ta_hello_world.h>
//#include "xxhash64.h"
#include "xxhash3.h"
//#include "xxhash.h"
#define TA_NAME "check_checkpoint.pta"
// #include "blake2.h"
//#define SMALLTEST //enable for small test case
//#define EMBENCH_TEST //用于在embench中进行测试
//#define TEST //只记录前几次控制循环产生的证据，为了快速记录，验证verifier

//#define DBG //用于在出现错误时打印错误。
//#define MEASURETIME //用于收集时间相关数据
//#define COLLECTTIME //用于收集关键变量数据的最大值和最小值
#define ONLYVERIFY  //不作CFI检查,只记录
//#define ONLYCFI //只进行控制流检查，包括cfa和cfi
//#define ONLYDFI //只进行数据流检查
//#define BLAKE2S_ENABLED //使用blake2s进行哈希计算，否则使用xxhash3_64
#ifdef BLAKE2S_ENABLED
    #include "blake2.h"
#endif
//#define COLLECTIME_FOR_ARDUPILOT //用于在ardupilot中收集时间相关数据，px4 不要开
//#define MULTIOPERATION
//#define SHADOW_STACK //test shadows stack for compare
#ifdef SMALLTEST
    #define BUFFER_SIZE (4096)
#else
    #define BUFFER_SIZE (1024 * 1024)
#endif
#define BUFFER_MASK (BUFFER_SIZE - 1)
#define HASH_OUTBYTES 8
#define CFG_TEE_CORE_NB_CORE	4
#define SECURE_TASK_NUM		1
#define TA_NUM 20
#ifdef SMALLTEST
    #define HASHCACHE_SIZE  64U
#else
    #define HASHCACHE_SIZE  1024U
#endif
#define HASHCACHE_MASK (HASHCACHE_SIZE - 1)
//#define MIX_ADDR(addr64) (((addr64) ^ ((addr64) >> 12)) & HASHCACHE_MASK)


typedef struct {
    int start_random;
    int end_random;
}Random;

typedef struct 
{
    uint64_t buffer[BUFFER_SIZE];
    atomic_size_t  head;//where to write
    atomic_size_t  tail;//where to read

}RingBuffer;
struct hash_entry {  // 先声明结构体
    struct hash_entry* next;  // 使用完整的结构体名称
    char hash_offline[HASH_OUTBYTES+1];
};
/*use Blake2s*/
// typedef struct {
//     uint32_t start_addr[3];
//     uint32_t end_addr[3];
//     char* corresponding_data_base_entry[3];
//     char hash_cache[3][HASH_OUTBYTES+1];
// }valid_path_cache_entry;
/*use xxhash3_64*/
typedef struct {
    union {
            struct {
                uint32_t end_addr;
                uint32_t start_addr;
                
            };
            uint64_t key_pair; // 核心优化点：用于快速比较
    };
    char* corresponding_data_base_entry;
    //uint64_t hash_cache;
}__attribute__((aligned(16))) valid_path_cache_entry;
// typedef struct hash_entry hash_entry;  // 这样才可以用 hash_entry 作为类型
// struct recordpoint {
//     uint32_t start_addr;
//     uint32_t end_addr;
//     hash_entry *hash_entry_head;
//     char * corresponding_data_base_entry;
//     hash_entry* tail;//指向最后一个hash_entry,方便插入
//     struct recordpoint * next;  
// };

//task_init_flag indicate whether this task need to initialized
//1: task initialization, 0: no task initialization
struct secure_task_params{
	int period;
	int priority;
	int execution_time;
	int task_init_flag;
	int cpu;
	void *cstm_param_addr; //used to transfer other params
	int cstm_param_size;   //used to transfer other params
};

struct world_params
{
	int secure_period;//周期
	int secure_budget;
	int non_secure_period;
	int non_secure_budget;
};


typedef enum  {
        EQ,NE,HS,LO,MI,PL,VS,VC,HI,LS,GE,LT,GT,LE,AL
}Condcode;

__attribute__((section(".trampoline")))
void use_custom_ro_data();  
__attribute__((section(".trampoline")))
void ringbuffer_init();

__attribute__((section(".trampoline")))
static inline void clean_ringbuffer();

__attribute__((section(".trampoline")))
bool push(RingBuffer* ringbuffer,uint64_t hash_arg);

__attribute__((section(".trampoline")))
bool is_empty(RingBuffer *ringbuffer);

__attribute__((section(".trampoline")))
uint64_t pop(RingBuffer* ringbuffer);






// control -flow measurement engine functions

//判断当前分支是否被执行
__attribute__((section(".trampoline")))
bool isBranchTaken( Condcode cond,uint32_t cpsr);

//收集有关于indirect jump的目标地址和源地址
__attribute__((section(".trampoline")))
void collect_bx(uint32_t dest);



//对return指令进行处理
__attribute__((section(".trampoline")))
void collect_bx_lr(uint32_t dest);



//对ldmia ret指令进行处理
__attribute__((section(".trampoline")))
void collect_ldmia_ret(uint32_t dest);



//对blx指令进行处理
__attribute__((section(".trampoline")))
void collect_blx(uint32_t dest);



//对有条件的blx指令进行处理,实际上没有这种指令
__attribute__((section(".trampoline")))
void collect_indirect_call_pred(uint32_t dest,uint32_t cpsr,uint32_t condtype);



//对有条件的bx指令进行处理
__attribute__((section(".trampoline")))
void collect_indirect_jump_pred(uint32_t dest,uint32_t cpsr,uint32_t condtype);

// random update monitor
void start_random_update_monitor(int interval_ms);


//Local Verify engine functions

//在递归函数的entry处插入该函数，打破递归
__attribute__((section(".trampoline")))
void recursive_recordpoint();

//在循环函数的latch处插入该函数,打破循环
__attribute__((section(".trampoline")))
void loop_recordpoint();

//实际上没有用
__attribute__((section(".trampoline")))
void recursive_latch();

//在没有函数调用的循环函数的latch处插入该函数
__attribute__((section(".trampoline")))
void loop_latch();

//在leaf函数的entry处插入该函数
__attribute__((section(".trampoline")))
void checkpoint();

//在递归函数的return处插入该函数
__attribute__((section(".trampoline")))
void ret_recursive_recordpoint();

//操作开始，进行初始化
__attribute__((section(".trampoline")))
void start_collecting();

//操作结束，进行收尾
__attribute__((section(".trampoline")))
void end_collecting();

__attribute__((section(".trampoline")))
void start_new_thread();
//对运行时measurement进行验证
/* use blake2s*/
// __attribute__((section(".trampoline")))
// int fast_verify_sub_path(char*hash_arg,char * data_base_ptr);
/* use xxhash*/
__attribute__((section(".trampoline")))
int fast_verify_sub_path(uint64_t hash_arg,char * data_base_ptr);
//该方案用于在每个段结束后就进行证明，而不是在所有段结束后再进行证明
//cache中没有找到对应的段，需要在数据库头部重新查找
/*use blake2s*/
// __attribute__((section(".trampoline")))
// int verify_sub_path_without_init(uint32_t start_addr_arg,uint32_t end_addr_arg,char*hash_arg);
/*use xxhash3_64*/
__attribute__((section(".trampoline")))
int verify_sub_path_without_init(uint32_t start_addr_arg,uint32_t end_addr_arg,uint64_t hash_arg);

__attribute__((section(".trampoline")))
void rt_tee_scheduler_start(struct world_params *first_world_params, unsigned int cmd_id);

__attribute__((section(".trampoline")))
void write_time_to_file(uint64_t time_interval);

__attribute__((section(".trampoline")))
void world_params_init(struct world_params *world_params, int secure_period,\
	int secure_budget, int non_secure_period, int non_secure_budget);

__attribute__((section(".trampoline")))
void task_param_init(struct secure_task_params * task_params, int period,\
 int priority, int execution_time, int task_init_flag, int cpu);

__attribute__((section(".trampoline")))
void rt_tee_open_session(int taskId);

__attribute__((section(".trampoline")))
void rt_tee_scheduler_start(struct world_params *first_world_params, unsigned int cmd_id);

__attribute__((section(".trampoline")))
void rt_tee_close_sessions(int taskId);


__attribute__((section(".trampoline")))
void trigger_timer_on_cores(int pin_core);

__attribute__((section(".trampoline")))
void rt_tee_task_init(int task_index);

__attribute__((section(".trampoline")))
void world_sched_init();

__attribute__((section(".trampoline")))
void world_sched_start(int core);

__attribute__((section(".trampoline")))
void write_target_buffer_to_file(const char* filename);

__attribute__((section(".trampoline")))
void write_ringbuffer_to_file(const char* filename);

__attribute__((section(".trampoline")))
void write_branch_trace_to_file(const char* filename) ;

__attribute__((section(".trampoline")))
void init_secure_scheduler();
#ifdef __cplusplus
}
#endif


#ifdef SHADOW_STACK
typedef struct{
    uint32_t buffer[2048];
    _Atomic size_t top;

}StackBuffer;
__attribute__((section(".trampoline")))
void collect_bx_lr(uint32_t dest);

__attribute__((section(".trampoline")))
void collect_bl(uint32_t src);


__attribute__((section(".trampoline")))
void collect_bl_pred(uint32_t cpsr, uint32_t condtype,uint32_t src);
#endif