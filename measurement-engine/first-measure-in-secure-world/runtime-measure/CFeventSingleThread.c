#include "CFeventSingleThread.h"
#include <errno.h>

// Branch prediction optimization macros
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)

#ifdef COLLECTIME_FOR_ARDUPILOT
    extern volatile int record_time_flag;
#endif
#define ROTL64(x, r)  (((x) << (r)) | ((x) >> (64 - (r))))
//该版本该版本是在一个段结束后，使用哈希查找来加速对控制流完整性的查找，对产生的证据进行一个验证，判断这个段的执行路径是否合法。
// __attribute__((section(".custom_heap_data_base")))  
__thread int start_measure __attribute__((tls_model("local-exec"))) = 0;
//__thread int start_measure __attribute__((tls_model("initial-exec"))) = 0;
// __attribute__((section(".custom_heap_data_base")))  
//     XXHash64 h;
// __attribute__((section(".custom_heap_data_base")))  
//     uint64_t  hash;//used to store the runtime computed hash
__attribute__((section(".custom_heap_data_base")))  
    char hash_true[HASH_OUTBYTES+1];//used to store the runtime computed hash in string format
__attribute__((section(".custom_heap_data_base")))  
    uint64_t valid_hash_cache[HASHCACHE_SIZE];
// __attribute__((section(".custom_heap_data_base")))  
//     char valid_hash_cache[HASHCACHE_SIZE][HASH_OUTBYTES+1];//Used to store verified hashes for hash search.
// __attribute__((section(".custom_heap_data_base")))  
//     char *verifed_recordpoint_cache[HASHCACHE_SIZE];
__attribute__((section(".custom_heap_data_base")))  
    valid_path_cache_entry sub_path_cache[HASHCACHE_SIZE];//已验证合法地址缓冲区
#ifdef BLAKE2S_ENABLED
    int judge_use_blake2s=0;
#endif
__attribute__((section(".custom_heap_data_base")))  
    uint32_t recordpoint_start_addr;
__attribute__((section(".custom_heap_data_base")))  
    uint32_t recordpoint_end_addr;
#ifdef SMALLTEST
__attribute__((section(".custom_heap_data_base")))  
#endif
    RingBuffer  ringbuffer_global;
__attribute__((section(".custom_heap_data_base")))
    char * global_data_base_ptr=NULL;//用于在verify_sub_path_without_init验证通过后标识该段对应的位置

__attribute__((section(".custom_heap_data_base")))
    uint64_t previous_time=0;
__attribute__((section(".custom_heap_data_base")))
    uint64_t sum_negative = 0.0;
__attribute__((section(".custom_heap_data_base")))
    uint64_t sum_positive = 0.0;
__attribute__((section(".custom_heap_data_base")))
    uint64_t mean_time_interval=0;  
__attribute__((section(".custom_heap_data_base")))  
    int if_init_database=0;//判断离线数据库是否初始化了
__attribute__((section(".custom_heap_data_base")))  
    uint64_t hash_global=0;
__attribute__((section(".custom_heap_data_base")))  
uint32_t target_buffer[1000];
__attribute__((section(".custom_heap_data_base")))  
int head=0;
#ifdef SMALLTEST
__attribute__((section(".custom_heap_data_base")))
#endif
uint8_t branch_trace[BUFFER_SIZE];

__attribute__((section(".custom_heap_data_base")))
atomic_int branch_trace_head=0;
__attribute__((section(".custom_heap_data_base")))
    int record_cnt=0;//用于记录当前是第几轮测量
__attribute__((section(".custom_heap_data_base")))
    int record_flag=0;//用于记录验证结果
#ifdef SHADOW_STACK
    StackBuffer shadow_stack;
#endif
extern char _custom_heap_start_data_base[];//受保护数据变量区域的起始地址
extern char _custom_heap_end_data_base[];//受保护数据变量区域的终止地址
extern char _custom_ro_data_start[];//离线数据库的存放地址
extern char _custom_ro_data_end[];//离线数据库的终止地址
static char * custom_ro_data_ptr= (char*)_custom_ro_data_start;//用于存放离线证据，是一个只读数据。
static char* custom_heap_ptr_data_base = (char*)_custom_heap_start_data_base;//用于存放本文件中的全局变量
Random my_random;
extern uint32_t sen_addr_index;
// TEEC_Result results[TA_NUM];
// TEEC_Context contexts[TA_NUM];//context 0 为内置调度器context
// TEEC_Session sessions[TA_NUM];
// TEEC_Operation operations[TA_NUM];
// TEEC_UUID *uuid_tasks[TA_NUM];
// static TEEC_SharedMemory shared_mem;
// struct secure_task_params task_params[TA_NUM];
// uint32_t err_origins[TA_NUM];

// //char shared_buffer_with_TEE[4];
// struct world_params each_core_world_params[CFG_TEE_CORE_NB_CORE];
#define CPSR_N_FLAG (1u<<31)
#define CPSR_Z_FLAG (1u<<30)
#define CPSR_C_FLAG (1u<<29)
#define CPSR_V_FLAG (1u<<28)
// static inline uint32_t MIX_ADDR(uint64_t x) {
//     // 使用 MurmurHash3 的 finalizer 常量，这些常量经过数学验证能产生极好的分布
//     x ^= x >> 33;
//     x *= 0xff51afd7ed558ccdULL;
//     x ^= x >> 33;
//     x *= 0xc4ceb9fe1a85ec53ULL;
//     x ^= x >> 33;
//     return (uint32_t)x & HASHCACHE_MASK;
// }
static inline uint32_t MIX_ADDR(uint64_t key) {
    key = (~key) + (key << 21); // key = (key << 21) - key - 1;
    key = key ^ (key >> 24);
    key = (key + (key << 3)) + (key << 8); // key * 265
    key = key ^ (key >> 14);
    key = (key + (key << 2)) + (key << 4); // key * 21
    key = key ^ (key >> 28);
    key = key + (key << 31);
    return (uint32_t)key & HASHCACHE_MASK;
}
void use_custom_ro_data() 
{
    volatile char c = _custom_ro_data_start[0];  // 防止优化
}
bool push(RingBuffer* ringbuffer,uint64_t hash_arg)
{
    // if(ringbuffer->head==ringbuffer->tail+1)//满了,留有一个空位
    // {
    //     return false;
    // }

    //ringbuffer->buffer[ringbuffer->head]=hash_arg;
    //size_t next_head=(ringbuffer->head + 1) & (BUFFER_MASK);
    //ringbuffer->head=(ringbuffer->head+1)%BUFFER_SIZE;
    //atomic_store(&ringbuffer->head, (ringbuffer->head + 1) & (BUFFER_MASK));
    //atomic_store_explicit(&ringbuffer->head, next_head, memory_order_release);
    #ifdef MULTIOPERATION
        return;
    #endif
    size_t current_head = atomic_load_explicit(&ringbuffer->head, memory_order_relaxed);
    // if(current_head==BUFFER_MASK)
    //     printf("buffer is full\n");
    ringbuffer->buffer[current_head & BUFFER_MASK] = hash_arg;
    atomic_store_explicit(&ringbuffer->head, current_head + 1, memory_order_release);
    return true;
}
uint64_t pop(RingBuffer* ringbuffer)
    {
        // if(ringbuffer->head==ringbuffer->tail+1)
    // {
    //     return 0;
    // }
    uint64_t hash_arg=ringbuffer->buffer[ringbuffer->tail];
    //ringbuffer->tail=(ringbuffer->tail+1)%BUFFER_SIZE;
    atomic_store(&ringbuffer->tail, (ringbuffer->tail + 1) & (BUFFER_MASK));
    return hash_arg;
}
bool is_empty(RingBuffer *ringbuffer)
{
    if(ringbuffer->head==ringbuffer->tail)
    {
        return true;
    }
    return false;
}
void ringbuffer_init()
{
    //memset(&ringbuffer_global,0, sizeof(RingBuffer));
    ringbuffer_global.head=0;
    ringbuffer_global.tail=0;
}
static inline void clean_ringbuffer()
{
    //memset(&ringbuffer_global,0, sizeof(RingBuffer));
    ringbuffer_global.head=0;
    ringbuffer_global.tail=0;
}


// control -flow measurement engine functions----------------------------------
static bool _calculate_truth(int cond, uint32_t cpsr_flags) {
    // 这里的 cpsr_flags 已经是移动到高位的，或者我们在传入前处理
    bool N = (cpsr_flags & CPSR_N_FLAG) ? 1 : 0;
    bool Z = (cpsr_flags & CPSR_Z_FLAG) ? 1 : 0;
    bool C = (cpsr_flags & CPSR_C_FLAG) ? 1 : 0;
    bool V = (cpsr_flags & CPSR_V_FLAG) ? 1 : 0;

    switch (cond) {
        case EQ: return Z;
        case NE: return !Z;
        case HS: return C;
        case LO: return !C;
        case MI: return N;
        case PL: return !N;
        case VS: return V;
        case VC: return !V;
        case HI: return C && !Z;
        case LS: return !C || Z;
        case GE: return N == V;
        case LT: return N != V;
        case GT: return !Z && (N == V);
        case LE: return Z || (N != V);
        case AL: return true;
        default: return false; // NV 或其他
    }
}
uint8_t kConditionTable[256]; // 全局表
void init_branch_lookup_table() {
    for (int c = 0; c < 16; c++) {
        for (int f = 0; f < 16; f++) {
            // f 是 0-15 的整数，代表 NZCV 的组合
            // 我们需要把它移到 28-31 位，模拟真实的 CPSR
            uint32_t simulated_cpsr = (uint32_t)f << 28;
            // 计算结果并填表
            // 索引算法： (条件码 << 4) | (标志位)
            kConditionTable[(c << 4) | f] = _calculate_truth(c, simulated_cpsr);
        }
    }
}
// 定义标志位掩码 (假设你已经在头文件里定义了这些)
#define CPSR_N_BIT 31
#define CPSR_Z_BIT 30
#define CPSR_C_BIT 29
#define CPSR_V_BIT 28
void mission_control()
{
    if(record_cnt==0)
    {
        record_flag=1;
    }
    if(record_cnt<=5)
    {
        record_cnt=record_cnt+1;
    }
    if(record_cnt==5)
    {
        record_flag=0;
    }
    if(record_cnt>5)
    {
        record_cnt=6;
    }
}
static inline __attribute__((always_inline)) bool isBranchTaken_fast(unsigned int cond, uint32_t cpsr)
{
    // 1. 处理 AL (Always, cond=14)
    if (cond == 14) return true;
    
    // 2. 提取标志位 (转为 0 或 1)
    // 编译器会把这些优化为简单的移位指令
    uint32_t N = (cpsr >> CPSR_N_BIT) & 1;
    uint32_t Z = (cpsr >> CPSR_Z_BIT) & 1;
    uint32_t C = (cpsr >> CPSR_C_BIT) & 1;
    uint32_t V = (cpsr >> CPSR_V_BIT) & 1;

    // 3. 核心魔法：利用 ARM 条件码的最低位 (LSB)
    // ARM 条件码通常成对出现，例如 EQ(0000) 和 NE(0001)。
    // 它们的区别仅仅是最后一位反转，意味着逻辑取反。
    
    bool result = false;
    
    // 将 4 位 cond 分解：高 3 位决定基准条件，最低位决定是否取反
    switch (cond >> 1) { // 编译后会变成跳转表或直接计算，比完整的 switch(cond) 快得多
        case 0: result = Z; break;          // EQ (0000) / NE (0001)
        case 1: result = C; break;          // CS/HS (0010) / CC/LO (0011)
        case 2: result = N; break;          // MI (0100) / PL (0101)
        case 3: result = V; break;          // VS (0110) / VC (0111)
        case 4: result = (C && !Z); break;  // HI (1000) / LS (1001) -> 注意 LS 的逻辑是 HI 取反
        case 5: result = (N == V); break;   // GE (1010) / LT (1011)
        case 6: result = (!Z && (N == V)); break; // GT (1100) / LE (1101)
        default: return false; // Should catch NV?
    }

    // 如果 cond 的最低位是 1 (例如 NE, LO, LT...)，则结果取反
    // 使用异或 (^) 来实现无分支的取反： result ^ (cond & 1)
    // 注意：HI/LS 和 GT/LE 的逻辑稍微特殊，LS = !HI, LE = !GT。
    // 上面的 switch 已经针对偶数项写了逻辑。
    // 对于 HI (cond=8)，result = C && !Z。
    // 对于 LS (cond=9)，我们要的是 !HI = !(C && !Z) = !C || Z。
    // 只要 cond 是奇数，就对结果取反，这涵盖了所有情况。
    
    return result ^ (cond & 1);
}

bool isBranchTaken( Condcode cond,uint32_t cpsr)
{
    bool N=cpsr&CPSR_N_FLAG;
    bool Z=cpsr&CPSR_Z_FLAG;
    bool C=cpsr&CPSR_C_FLAG;
    bool V=cpsr&CPSR_V_FLAG;
    switch(cond){
        case EQ:return Z;
        case NE: return !Z;
        case HS: return C;
        case LO:return !C;
        case MI:return N;
        case PL:return !N;
        case VS:return V;
        case VC:return !V;
        case HI: return C && !Z;
        case LS: return !C || Z;
        case GE: return N == V;
        case LT: return N != V;
        case GT: return !Z && (N == V);
        case LE: return Z || (N != V);
        case AL: return true;
        default: return false;
    }
}
// void collect_bcc_backup(uint32_t cpsr,uint32_t condtype)
// {
//     #ifdef ONLYDFI
//         return;
//     #endif
//     #ifdef COLLECTTIME
//         return;
//     #endif
//     if (start_measure==0)
//     {
//         return;
//     }
//     Condcode condtype1=(Condcode)condtype;
//     bool branch_taken=isBranchTaken(condtype1,cpsr);
//     if(branch_taken)
//     {
//         // uint32_t destAddr=dest;
//         //printf("run true\n");
//         int idx = atomic_fetch_add(&branch_trace_head, 1);
//         branch_trace[idx]=1;
//         #ifdef DBG
//             printf("collect_indirect_call_pred---dest%0x---pthreadID is:%d\n",dest,pthread_self());
//         #endif
//         return;
//     }
//     else{
//         //printf("run false\n");
//         int idx = atomic_fetch_add(&branch_trace_head, 1);
//         branch_trace[idx]=0;
//         return;
//     }
// }
// void collect_bcc(uint32_t cpsr,uint32_t condtype)
// {
//     #ifdef ONLYDFI
//         return;
//     #endif
//     #ifdef COLLECTTIME
//         return;
//     #endif
//     if (start_measure==0)
//     {
//         return;
//     }
//     bool taken = isBranchTaken_fast((Condcode)condtype, cpsr);
//     int idx = atomic_fetch_add_explicit(&branch_trace_head, 1, memory_order_relaxed);
//     branch_trace[idx] = taken;
// }
void collect_bcc(uint32_t cpsr, uint32_t condtype)//开销占比约40us
{

    #ifdef ONLYDFI
        return;
    #endif
    #ifdef COLLECTTIME
        return;
    #endif
    #ifdef TEST
        if(record_flag==0)
        {
            return;
        }
    #endif
    #ifndef EMBENCH_TEST
    if (start_measure==0)
    {
        return;
    }
    #endif
    uint8_t index = (condtype << 4) | (cpsr >> 28);

    // 4. O(1) 查表
    uint8_t taken = kConditionTable[index];

    // 5. Relaxed 原子操作
    // memory_order_relaxed: 只有原子性，没有内存屏障，ARM上极快
    #ifndef EMBENCH_TEST
        int idx = atomic_fetch_add_explicit(&branch_trace_head, 1, memory_order_relaxed);
    #else
        int idx=branch_trace_head;
        branch_trace_head=branch_trace_head+1;
    #endif
    #ifdef MULTIOPERATION
        return;
    #endif
    // 6. 写入结果
    // 建议 branch_trace 定义为 uint8_t 类型以节省带宽
    branch_trace[idx&BUFFER_MASK] = taken;
}
//对于间接跳转，哈希这条边
void collect_bx(uint32_t dest)//r0=dest, r1=src
{
    #ifdef COLLECTTIME
        return;
    #endif
    #ifdef ONLYDFI
        return;
    #endif
    #ifdef TEST
        if(record_flag==0)
        {
            return;
        }
    #endif
    //start_measure=(int*)pthread_getspecific(key);
    #ifndef EMBENCH_TEST
    if (start_measure==0)
    {
        return;
    }
    #endif
    // uint32_t srcAddr=src;
    uint32_t destAddr=dest;
    //push(&ringbuffer_global,destAddr);
    //checkpoint=(CheckPoint*)pthread_getspecific(key2);
    //blake2s_update(&(checkpoint.blake_state),&srcAddr,sizeof(srcAddr));
    //blake2s_update(&(blake_state),&destAddr,sizeof(destAddr));
    //XXHash64_add(&h, &destAddr,sizeof(destAddr));
    #ifdef DBG
    printf("collect_bx--dest:%0x---pthreadID is:%d\n",dest,pthread_self());
    #endif
    //pthread_mutex_unlock(&lock);
    return;
}



void collect_ldmia_ret(uint32_t dest)
{
    #ifdef COLLECTTIME
        return;
    #endif
    #ifdef ONLYDFI
        return;
    #endif
    #ifdef TEST
        if(record_flag==0)
        {
            return;
        }
    #endif
    //start_measure=(int*)pthread_getspecific(key);
    #ifndef EMBENCH_TEST
    if (start_measure==0)
    {
        return;
    }
    #endif
    #ifdef SHADOW_STACK
        uint32_t destAddr=dest;
        uint32_t correct_target;
        correct_target=shadow_stack.buffer[shadow_stack.top];
        if(unlikely(shadow_stack.top == 0))
        {
            
            printf("top is 0\n");
            abort();
            return;
        }
        shadow_stack.top--;
        if(likely(correct_target==destAddr))
        {
            return;
        }
        else
        {   
        
            //printf("src is %0x\n",src);
            printf("dest is %0x\n",dest);
            printf("top is %0x\n",correct_target);
            abort();
            // printf("occur backward hijacking in :\n");
            return;
        }
        return;
    #endif
    //uint32_t destAddr=dest;
    //push(&ringbuffer_global,destAddr);
    //ringbuffer_global.buffer[ringbuffer_global.head]=destAddr;
    //ringbuffer_global.head+=1;
    // hash_global ^= dest;
    // hash_global = ROTL64(hash_global, 13);
    target_buffer[head] = dest;
    head=head+1;
    return;
}


//对于间接调用，要哈希这条边，同时将间接调用指令的下一条入栈
void collect_blx(uint32_t dest)
{
    #ifdef COLLECTTIME
        return;
    #endif
    #ifdef ONLYDFI
        return;
    #endif
    #ifdef TEST
        if(record_flag==0)
        {
            return;
        }
    #endif
    #ifndef EMBENCH_TEST
    if (start_measure==0)
    {
        return;
    }
    #endif
    //uint32_t destAddr=dest;
    //ringbuffer_global.buffer[ringbuffer_global.head]=destAddr;
    //ringbuffer_global.head+=1;
    // hash_global ^= dest;
    // hash_global = ROTL64(hash_global, 13);
    target_buffer[head] = dest;
    head=head+1;
    //blake2s_update(&(blake_state),&destAddr,sizeof(destAddr));
    //XXHash64_add(&h, &destAddr,sizeof(destAddr));
    #ifdef DBG
        printf("collect_blx---dest:%0x---pthreadID is:%d\n",dest,pthread_self());
    #endif

    return;

}



//对于indirect call pred 同上述对indirect call的处理
void collect_indirect_call_pred(uint32_t dest,uint32_t cpsr,uint32_t condtype)
{
    #ifdef ONLYDFI
        return;
    #endif
    #ifdef COLLECTTIME
        return;
    #endif
    return;
    #ifndef EMBENCH_TEST
    if (start_measure==0)
    {
        return;
    }
    #endif
    Condcode condtype1=(Condcode)condtype;
    bool branch_taken=isBranchTaken(condtype1,cpsr);
    if(branch_taken)
    {
        uint32_t destAddr=dest;
        //push(&ringbuffer_global,destAddr);
        //blake2s_update(&(blake_state),&destAddr,sizeof(destAddr));
        //XXHash64_add(&h, &destAddr,sizeof(destAddr));
        #ifdef DBG
            printf("collect_indirect_call_pred---dest%0x---pthreadID is:%d\n",dest,pthread_self());
        #endif
        return;
    }
    else{
        return;
    }

}

void collect_indirect_jump_pred(uint32_t dest,uint32_t cpsr, uint32_t condtype)
{
    #ifdef COLLECTTIME
        return;
    #endif
    #ifdef ONLYDFI
        return;
    #endif
    #ifdef TEST
        if(record_flag==0)
        {
            return;
        }
    #endif
    #ifndef EMBENCH_TEST
    if (start_measure==0)
    {
        return;
    }
    #endif
    //pthread_mutex_lock(&lock);
    Condcode condtype1=(Condcode)condtype;
    bool branch_taken=isBranchTaken(condtype1,cpsr);
    if(branch_taken)
    {
        uint32_t destAddr=dest;
        //push(&ringbuffer_global,destAddr);
        //blake2s_update(&(blake_state),destAddr,sizeof(destAddr));
        //XXHash64_add(&h, &destAddr,sizeof(destAddr));
        #ifdef DBG
            printf("collect_indirect_jump_pred--dest:%0x---pthreadID is:%d\n",dest,pthread_self());
        #endif
        return;
    }
    else{
        return;
    }
}

//Local Verify engine functions---------------------------------------

//cache中已经找到了对应的段，但是该哈希先前没遇到过，使用corresponding_data_base_entry快速找到离线数据库中该段中对应的位置
//data_base_ptr指向该段在离线数据库中的位置。
/* use blake2s*/
// int fast_verify_sub_path(char*hash_arg,char * data_base_ptr)
// {
//     char* ro_data=data_base_ptr;
//     uint32_t hash_count=*(uint32_t*)(ro_data+12);
//     ro_data+=16;
//     int i=0;
//     // char * current_recordpoint=ro_data-16;
//     for(i=0 ;i<hash_count;i++)
//     {
//         char hash_offline[HASH_OUTBYTES];
//         for(int j=0;j<HASH_OUTBYTES;j++)
//         {
//             //sprintf(&hash_offline[j * 2], "%02x", (unsigned char)ro_data[j]);
//             hash_offline[j]=ro_data[j];
//         }
//         hash_offline[HASH_OUTBYTES]='\0';
//         bool finded_hash=false;
//         if(strcmp(hash_arg,hash_offline)==0)
//         {
//             return 2;//2表示验证通过
//         }
//         else{
//             ro_data+=HASH_OUTBYTES;   
//         }               
//     }
//     printf("fast_verify_sub_path not find the hash in database\n");
//     return 0;//没有找到对应的哈希
// }
/* use xxhash*/
int fast_verify_sub_path(uint64_t hash_arg,char * data_base_ptr)
{
    #ifdef ONLYVERIFY
        return 2;
    #endif 
    char* ro_data=data_base_ptr;
    uint32_t hash_count=*(uint32_t*)(ro_data+12);
    ro_data+=16;
    int i=0;
    for(i=0 ;i<hash_count;i++)
    {
        uint64_t hash_offline=*(uint64_t*)ro_data;
        if(hash_arg==hash_offline)
        {
            return 2;//2表示验证通过
        }
        else if(hash_offline==0){
             return 0;
        }
        else{
            ro_data+=HASH_OUTBYTES;   
        }               
    }
    return 0;//没有找到对应的哈希
}
//该方案用于在每个段结束后就进行证明，而不是在所有段结束后再进行证明
//cache中没有找到对应的段，需要在数据库头部重新查找
/* use blake2s*/
// int verify_sub_path_without_init(uint32_t start_addr_arg,uint32_t end_addr_arg,char*hash_arg)
// {
//     char* ro_data=_custom_ro_data_start;
//     #ifdef DBG
//         printf("start_addr is %0x\n",point->start_addr);
//         printf("end_addr is %0x\n",point->end_addr);
//     #endif
//     char* ro_data_end=_custom_ro_data_end;
//     uint32_t element_type=*(uint32_t*)ro_data;//用于判断是否结束
//     while(element_type!=0)
//     {
//         uint32_t start_addr=*(uint32_t*)(ro_data+4);
//         uint32_t end_addr=*(uint32_t*)(ro_data+8);
//         uint32_t hash_count=*(uint32_t*)(ro_data+12);
//         ro_data+=16;
//         if(start_addr==start_addr_arg&&end_addr==end_addr_arg)//在离线数据库中找到了对应的recordpoint
//         {
//             int i=0;
//             char * current_recordpoint=ro_data-16;
            
//             for(i=0 ;i<hash_count;i++)
//             {
//                 char hash_offline[HASH_OUTBYTES];
//                 for(int j=0;j<HASH_OUTBYTES;j++)
//                 {
//                     //sprintf(&hash_offline[j * 2], "%02x", (unsigned char)ro_data[j]);
//                     hash_offline[j]=ro_data[j];
//                 }
//                 hash_offline[HASH_OUTBYTES]='\0';
//                 bool finded_hash=false;
//                 if(strcmp(hash_arg,hash_offline)==0)
//                 {
//                     global_data_base_ptr=current_recordpoint;
//                     return 2;//2表示验证通过
//                 }
//                 else{
//                     ro_data+=HASH_OUTBYTES;   
//                 }               
//             }
//         }
//         else//没有找到对应的recordpoint，则继续查找
//         {
//             ro_data+=HASH_OUTBYTES*hash_count;
//         }
//         element_type = *(uint32_t*)ro_data;
//     }
//     return 1;//没有找到对应的段
// }
/*use xxhash3_64*/
//todo :优化这个代码
int verify_sub_path_without_init(uint32_t start_addr_arg,uint32_t end_addr_arg,uint64_t hash_arg)
{
    #ifdef ONLYVERIFY
        return 2;
    #endif 
    uint64_t target_key = (uint64_t)start_addr_arg | ((uint64_t)end_addr_arg << 32);
    char* ro_data=_custom_ro_data_start;
    char* ro_data_end=_custom_ro_data_end;
    
    // Early exit if database is empty
    if(ro_data >= ro_data_end || *(uint32_t*)ro_data == 0) {
        return 1;
    }
    
    #ifdef DBG
        printf("start_addr is %0x\n",point->start_addr);
        printf("end_addr is %0x\n",point->end_addr);
    #endif
    
    uint32_t element_type=*(uint32_t*)(ro_data+8);//用于判断是否结束
    while(element_type != 0 && ro_data < ro_data_end)
    {
        // uint32_t start_addr=*(uint32_t*)(ro_data);
        // uint32_t end_addr=*(uint32_t*)(ro_data+4);
        uint64_t curr_key=*(uint64_t*)(ro_data);
        uint32_t hash_count=*(uint32_t*)(ro_data+12);
        ro_data+=16;
        if(curr_key == target_key)//在离线数据库中找到了对应的recordpoint
        {
            char * current_recordpoint=ro_data-16;
            
            for(int i = 0; i < hash_count; i++)
            {
                if(*(uint64_t*)ro_data == hash_arg)
                {
                    global_data_base_ptr=current_recordpoint;
                    return 2;//2表示验证通过
                }
                ro_data += HASH_OUTBYTES;
            }
            // Hash not found in this segment
            return 0;
        }
        else//没有找到对应的recordpoint，则继续查找
        {
            ro_data+=HASH_OUTBYTES*hash_count;
        }
        element_type = *(uint32_t*)(ro_data+8);
    }
    return 1;//没有找到对应的段
}
//use to segment the recursive loops
void recursive_recordpoint()
{
    #ifdef ONLYDFI
        return;
    #endif
    #ifdef COLLECTTIME
    return;
    #endif
    #ifndef EMBENCH_TEST
    if (start_measure==0)
    {
        return;
    }
    #endif
    #ifdef TEST
        if(record_flag==0)
        {
            return;
        }
    #endif
    // recordpoint_start_addr = (uint32_t)__builtin_return_address(0);
    // head=0;
    // return;
    /* 如果 ringbuffer 为空，直接视作已验证（最常见情况） */
    if (head == 0) {
       
        recordpoint_start_addr = (uint32_t)__builtin_return_address(0);  // 可省略，视调用者而定
        // uint64_t hash=3244421341483603138;
        //

        return;
    }
    // Compute hash first
    
    // uint64_t hash = hash_global;
    //     for(int i=0;i<head;i++)
    // {
    //     printf("hash target_buffer[i]: %0x\n", target_buffer[i]);
    // }
    #ifndef BLAKE2S_ENABLED
        uint64_t hash=XXH3_64bits(&target_buffer, head * sizeof(uint32_t));
    #endif     
    #ifdef BLAKE2S_ENABLED
         uint64_t hash;
         blake2s(&hash,8, &target_buffer, head * sizeof(uint32_t), NULL, 0);
    #endif
    
    if(hash==hash_global)
    {   
        // #ifndef ONLYVERIFY
            push(&ringbuffer_global,hash);
        // #endif
        recordpoint_start_addr = (uint32_t)__builtin_return_address(0); 
        return;
    }
    //hash_global = 0;
    //recordpoint_end_addr = (uint32_t)__builtin_return_address(0);
    uint32_t current_end_addr = (uint32_t)__builtin_return_address(0);
    // Compute indices once
    uint32_t hash_cache_idx = (uint32_t)(hash & HASHCACHE_MASK);
    uint32_t current_start_addr=recordpoint_start_addr;
    // First-level cache check: global hash cache (most likely hit)
    if(likely(valid_hash_cache[hash_cache_idx] == hash)){
        // #ifndef ONLYVERIFY
            push(&ringbuffer_global,hash);
        // #endif
        recordpoint_start_addr = current_end_addr;
        hash_global = hash;
        head=0;
        return;
    }
    
    // Compute segment indices only if first-level cache missed
    uint64_t current_path_key = ((uint64_t)current_start_addr ) | (((uint64_t)current_end_addr) << 32);
    //uint32_t segment_cache_idx = (uint32_t)(current& HASHCACHE_MASK) ;
    uint32_t segment_cache_idx = MIX_ADDR(current_path_key);

    
    
    valid_path_cache_entry* cache_entry_ptr = &sub_path_cache[segment_cache_idx];

    // Second-level cache check: hash-only check (faster path)

    //uint64_t cached = ((uint64_t)cache_entry_ptr->start_addr << 32) | cache_entry_ptr->end_addr;
    //uint64_t cached_path_key = cache_entry_ptr->key_pair;
    if(likely((cache_entry_ptr->key_pair == current_path_key))){
        // Segment found, verify hash from database (fast path)
        if(likely(fast_verify_sub_path(hash, cache_entry_ptr->corresponding_data_base_entry) == 2)){
            //cache_entry_ptr->hash_cache = hash;
            // #ifndef ONLYVERIFY
                push(&ringbuffer_global,hash);
            // #endif
            valid_hash_cache[hash_cache_idx] = hash;
            recordpoint_start_addr = current_end_addr;
            hash_global = hash;
            head=0;
            return;
        }
        printf("fast_verify_sub_path_recursive record start addr is %0x\n",current_start_addr);
        printf("fast_verify_sub_path_recursive record end addr is %0x\n",current_end_addr);
        printf("fast_verify_sub_path_recursive online hash is %llu\n",hash);
        printf("cache_entry_ptr->key_pair is %llu\n",cache_entry_ptr->key_pair);
        printf("current_path_key is %llu\n",current_path_key);
        printf("cheche_entry_ptr->corresponding_data_base_entry is %p\n",cache_entry_ptr->corresponding_data_base_entry);
        write_target_buffer_to_file("./target_buffer.txt");
        write_ringbuffer_to_file("hash_value.txt");
        write_branch_trace_to_file("branch_trace.txt");
        abort();
    }
    
    // Segment not in cache, verify from database (slow path)
    if(unlikely(verify_sub_path_without_init(current_start_addr, current_end_addr, hash) == 2)){
        // #ifndef ONLYVERIFY
            push(&ringbuffer_global,hash);
        // #endif
        // Cache the verified segment and hash
        cache_entry_ptr->key_pair = current_path_key;
        cache_entry_ptr->corresponding_data_base_entry = global_data_base_ptr;
        //cache_entry_ptr->hash_cache = hash;
        valid_hash_cache[hash_cache_idx] = hash;
        recordpoint_start_addr = current_end_addr;
        hash_global = hash;
        head=0;
        return;
    }
    printf("record start addr is %0x\n",current_start_addr);
    printf("record end addr is %0x\n",current_end_addr);
    printf("online hash is %llu\n",hash);
    write_target_buffer_to_file("./target_buffer.txt");
    write_branch_trace_to_file("branch_trace.txt");
    write_ringbuffer_to_file("hash_value.txt");
    abort();


}
//use to segment the loops
void loop_recordpoint(){
    #ifdef ONLYDFI
        return;
    #endif
    #ifdef COLLECTTIME
        return;
    #endif
    #ifdef TEST
        if(record_flag==0)
        {
            return;
        }
    #endif
    #ifndef EMBENCH_TEST
    if (start_measure==0)
    {
        return;
    }
    #endif
    // recordpoint_start_addr = (uint32_t)__builtin_return_address(0);
    // head=0;
    // return;
    /* 如果 ringbuffer 为空，直接视作已验证（最常见情况） */
    if (head == 0) {
        recordpoint_start_addr = (uint32_t)__builtin_return_address(0);  // 可省略，视调用者而定
        return;
    }
    // Compute hash first
    
    // uint64_t hash = hash_global;
    #ifndef BLAKE2S_ENABLED
        uint64_t hash=XXH3_64bits(&target_buffer, head * sizeof(uint32_t));
    #endif     
    #ifdef BLAKE2S_ENABLED
         uint64_t hash;
         blake2s(&hash,8, &target_buffer, head * sizeof(uint32_t), NULL, 0);
    #endif
    
    if(hash==hash_global)
    {

        // #ifndef ONLYVERIFY
            push(&ringbuffer_global,hash);
        // #endif

        recordpoint_start_addr = (uint32_t)__builtin_return_address(0); 
        return;
    }
    //recordpoint_end_addr = (uint32_t)__builtin_return_address(0);
    uint32_t current_end_addr = (uint32_t)__builtin_return_address(0);
    // Compute indices once
    uint32_t hash_cache_idx = (uint32_t)(hash & HASHCACHE_MASK);
    uint32_t current_start_addr=recordpoint_start_addr;
    // First-level cache check: global hash cache (most likely hit)
    if(likely(valid_hash_cache[hash_cache_idx] == hash)){
        // #ifndef ONLYVERIFY
            push(&ringbuffer_global,hash);
        // #endif
        recordpoint_start_addr = current_end_addr;
        hash_global = hash;
        head=0;
        return;
    }
    
    // Compute segment indices only if first-level cache missed
    uint64_t current_path_key = ((uint64_t)current_start_addr ) | (((uint64_t)current_end_addr) << 32);
    //uint32_t segment_cache_idx = (uint32_t)(current& HASHCACHE_MASK) ;
    uint32_t segment_cache_idx = MIX_ADDR(current_path_key);

    
    
    valid_path_cache_entry* cache_entry_ptr = &sub_path_cache[segment_cache_idx];

    // Second-level cache check: hash-only check (faster path)

    //uint64_t cached = ((uint64_t)cache_entry_ptr->start_addr << 32) | cache_entry_ptr->end_addr;
    //uint64_t cached_path_key = cache_entry_ptr->key_pair;
    if(likely((cache_entry_ptr->key_pair == current_path_key))){
        // Segment found, verify hash from database (fast path)
        if(likely(fast_verify_sub_path(hash, cache_entry_ptr->corresponding_data_base_entry) == 2)){
            //cache_entry_ptr->hash_cache = hash;
            // #ifndef ONLYVERIFY
                push(&ringbuffer_global,hash);
            // #endif
            valid_hash_cache[hash_cache_idx] = hash;
            recordpoint_start_addr = current_end_addr;
            hash_global = hash;
            head=0;
            return;
        }
        printf("fast_verify_sub_path_loop record start addr is %0x\n",current_start_addr);
        printf("fast_verify_sub_path_loop record end addr is %0x\n",current_end_addr);
        printf("fast_verify_sub_path_loop online hash is %llu\n",hash);
        printf("cache_entry_ptr->key_pair is %llu\n",cache_entry_ptr->key_pair);
        printf("current_path_key is %llu\n",current_path_key);
        printf("cheche_entry_ptr->corresponding_data_base_entry is %p\n",cache_entry_ptr->corresponding_data_base_entry);
        write_target_buffer_to_file("./target_buffer.txt");
        write_branch_trace_to_file("branch_trace.txt");
        write_ringbuffer_to_file("hash_value.txt");
        abort();

    }
    
    // Segment not in cache, verify from database (slow path)
    if(unlikely(verify_sub_path_without_init(current_start_addr, current_end_addr, hash) == 2)){
        // #ifndef ONLYVERIFY
            push(&ringbuffer_global,hash);
        // #endif
        // Cache the verified segment and hash
        cache_entry_ptr->key_pair = current_path_key;
        cache_entry_ptr->corresponding_data_base_entry = global_data_base_ptr;
        //cache_entry_ptr->hash_cache = hash;
        valid_hash_cache[hash_cache_idx] = hash;
        recordpoint_start_addr = current_end_addr;
        hash_global = hash;
        head=0;
        return;
    }
    printf("record start addr is %0x\n",current_start_addr);
    printf("record end addr is %0x\n",current_end_addr);
    printf("online hash is %llu\n",hash);
   write_target_buffer_to_file("./target_buffer.txt");
   write_branch_trace_to_file("branch_trace.txt");
   write_ringbuffer_to_file("hash_value.txt");
    abort();
}

void recursive_latch(){//useless
    return;
}   
//在没有函数调用的循环的latch处，插入该函数
//insert at the latch point of loops without function calls
void loop_latch(){
    return;
}
//在leaf函数的entry处 ，插入该函数
//insert at the entry point of leaf functions
void checkpoint()
{
    #ifdef ONLYDFI
        return;
    #endif
    #ifdef COLLECTTIME
        return;
    #endif
    #ifdef TEST
        if(record_flag==0)
        {
            return;
        }
    #endif
    if (start_measure == 0)
    {
        return;
    }
    /* 如果 ringbuffer 为空，直接视作已验证（最常见情况） */
    if (head == 0) {
        recordpoint_start_addr = (uint32_t)__builtin_return_address(0);  // 可省略，视调用者而定
        // uint64_t hash=3244421341483603138;
        // push(&ringbuffer_global,hash);
        head=0;
        return;
    }
    // Compute hash first


    // uint64_t hash = hash_global;
    // for(int i=0;i<head;i++)
    // {
    //     printf("hash target_buffer[i]: %0x\n", target_buffer[i]);
    // }
    #ifndef BLAKE2S_ENABLED
        uint64_t hash=XXH3_64bits(&target_buffer, head * sizeof(uint32_t));
    #endif     
    #ifdef BLAKE2S_ENABLED
         uint64_t hash;
         blake2s(&hash,8, &target_buffer, head * sizeof(uint32_t), NULL, 0);
    #endif
    
    
    // if(hash==hash_global)
    // {
    //     // #ifndef ONLYVERIFY
    //         push(&ringbuffer_global,hash);
    //     // #endif
    //     recordpoint_start_addr = (uint32_t)__builtin_return_address(0); 
    //     return;
    // }
    //recordpoint_end_addr = (uint32_t)__builtin_return_address(0);
    uint32_t current_end_addr = (uint32_t)__builtin_return_address(0);
    // Compute indices once
    uint32_t hash_cache_idx = (uint32_t)(hash & HASHCACHE_MASK);
    uint32_t current_start_addr=recordpoint_start_addr;
    // First-level cache check: global hash cache (most likely hit)
    if(likely(valid_hash_cache[hash_cache_idx] == hash)){
        // #ifndef ONLYVERIFY
            push(&ringbuffer_global,hash);
        // #endif
        recordpoint_start_addr = current_end_addr;
        hash_global = hash;
        head=0;
        return;
    }
    
    // Compute segment indices only if first-level cache missed
    uint64_t current_path_key = ((uint64_t)current_start_addr ) | (((uint64_t)current_end_addr) << 32);
    //uint32_t segment_cache_idx = (uint32_t)(current& HASHCACHE_MASK) ;
    uint32_t segment_cache_idx = MIX_ADDR(current_path_key);

    
    
    valid_path_cache_entry* cache_entry_ptr = &sub_path_cache[segment_cache_idx];

    // Second-level cache check: hash-only check (faster path)

    //uint64_t cached = ((uint64_t)cache_entry_ptr->start_addr << 32) | cache_entry_ptr->end_addr;
    //uint64_t cached_path_key = cache_entry_ptr->key_pair;
    if(likely((cache_entry_ptr->key_pair == current_path_key))){
        // Segment found, verify hash from database (fast path)
        if(likely(fast_verify_sub_path(hash, cache_entry_ptr->corresponding_data_base_entry) == 2)){
            //cache_entry_ptr->hash_cache = hash;
            // #ifndef ONLYVERIFY
                push(&ringbuffer_global,hash);
            // #endif
            valid_hash_cache[hash_cache_idx] = hash;
            recordpoint_start_addr = current_end_addr;
            hash_global = hash;
            head=0;
            return;
        }
        printf("fast_verify_sub_path record start addr is %0x\n",current_start_addr);
        printf("fast_verify_sub_pathrecord end addr is %0x\n",current_end_addr);
        printf("fast_verify_sub_path online hash is %llu\n",hash);
        printf("cache_entry_ptr->key_pair is %llu\n",cache_entry_ptr->key_pair);
        printf("current_path_key is %llu\n",current_path_key);
        printf("cheche_entry_ptr->corresponding_data_base_entry is %p\n",cache_entry_ptr->corresponding_data_base_entry);
        fflush(stdout);
        write_target_buffer_to_file("./target_buffer.txt");
        write_branch_trace_to_file("branch_trace.txt");
        write_ringbuffer_to_file("hash_value.txt");
        abort();

    }
    
    // Segment not in cache, verify from database (slow path)
    if(unlikely(verify_sub_path_without_init(current_start_addr, current_end_addr, hash) == 2)){
        // #ifndef ONLYVERIFY
            push(&ringbuffer_global,hash);
        // #endif
        //printf("verify_sub_path_without_init success ---record start addr is %0x,--record end addr is %0x\n",current_start_addr,current_end_addr);
        // Cache the verified segment and hash
        cache_entry_ptr->key_pair = current_path_key;
        cache_entry_ptr->corresponding_data_base_entry = global_data_base_ptr;
        //cache_entry_ptr->hash_cache = hash;
        valid_hash_cache[hash_cache_idx] = hash;
        recordpoint_start_addr = current_end_addr;
        hash_global =hash;
        head=0;
        return;
    }
        printf("record start addr is %0x\n",current_start_addr);
        printf("record end addr is %0x\n",current_end_addr);
        printf("online hash is %llu\n",hash);
        write_target_buffer_to_file("./target_buffer.txt");
        write_ringbuffer_to_file("hash_value.txt");
        write_branch_trace_to_file("branch_trace.txt");
        abort();
}    
//在递归函数的return处，插入该函数
//insert at the return point of recursive functions
void ret_recursive_recordpoint()
{    
    #ifdef ONLYDFI
        return;
    #endif
    #ifdef COLLECTTIME
    return;
    #endif
    #ifdef TEST
        if(record_flag==0)
        {
            return;
        }
    #endif
    if (start_measure == 0)
    {
        return;
    }
    // recordpoint_start_addr = (uint32_t)__builtin_return_address(0);
    // head=0;
    // return;
    /* 如果 ringbuffer 为空，直接视作已验证（最常见情况） */
    if (head == 0) {
        recordpoint_start_addr = (uint32_t)__builtin_return_address(0);  // 可省略，视调用者而定
        // uint64_t hash=3244421341483603138;
        // push(&ringbuffer_global,hash);
        return;
    }

    // Compute hash first
    //     for(int i=0;i<head;i++)
    // {
    //     printf("hash target_buffer[i]: %0x\n", target_buffer[i]);
    // }
    // uint64_t hash = hash_global;
    #ifndef BLAKE2S_ENABLED
        uint64_t hash=XXH3_64bits(&target_buffer, head * sizeof(uint32_t));
    #endif     
    #ifdef BLAKE2S_ENABLED
         uint64_t hash;
         blake2s(&hash,8, &target_buffer, head * sizeof(uint32_t), NULL, 0);
    #endif
    
    if(hash==hash_global)
    {
        // #ifndef ONLYVERIFY
            push(&ringbuffer_global,hash);
        // #endif
        recordpoint_start_addr = (uint32_t)__builtin_return_address(0); 
        return;
    }

    //recordpoint_end_addr = (uint32_t)__builtin_return_address(0);
    uint32_t current_end_addr = (uint32_t)__builtin_return_address(0);
    // Compute indices once
    uint32_t hash_cache_idx = (uint32_t)(hash & HASHCACHE_MASK);
    uint32_t current_start_addr=recordpoint_start_addr;
    // First-level cache check: global hash cache (most likely hit)
    if(likely(valid_hash_cache[hash_cache_idx] == hash)){
        // #ifndef ONLYVERIFY
            push(&ringbuffer_global,hash);
        // #endif
        recordpoint_start_addr = current_end_addr;
        hash_global = hash;
        head=0;
        return;
    }
    
    // Compute segment indices only if first-level cache missed
    uint64_t current_path_key = ((uint64_t)current_start_addr ) | (((uint64_t)current_end_addr) << 32);
    //uint32_t segment_cache_idx = (uint32_t)(current& HASHCACHE_MASK) ;
    uint32_t segment_cache_idx = MIX_ADDR(current_path_key);

    
    
    valid_path_cache_entry* cache_entry_ptr = &sub_path_cache[segment_cache_idx];

    // Second-level cache check: hash-only check (faster path)

    //uint64_t cached = ((uint64_t)cache_entry_ptr->start_addr << 32) | cache_entry_ptr->end_addr;
    //uint64_t cached_path_key = cache_entry_ptr->key_pair;
    if(likely((cache_entry_ptr->key_pair == current_path_key))){
        // Segment found, verify hash from database (fast path)
        if(likely(fast_verify_sub_path(hash, cache_entry_ptr->corresponding_data_base_entry) == 2)){
            //cache_entry_ptr->hash_cache = hash;
            // #ifndef ONLYVERIFY
                push(&ringbuffer_global,hash);
            // #endif
            valid_hash_cache[hash_cache_idx] = hash;
            recordpoint_start_addr = current_end_addr;
            hash_global = hash;
            head=0;
            return;
        }
        printf("fast_verify_sub_path_ret_recursive record start addr is %0x\n",current_start_addr);
        printf("fast_verify_sub_path_ret_recursive record end addr is %0x\n",current_end_addr);
        printf("fast_verify_sub_path_ret_recursive online hash is %llu\n",hash);
        printf("cache_entry_ptr->key_pair is %llu\n",cache_entry_ptr->key_pair);
        printf("current_path_key is %llu\n",current_path_key);
        printf("cheche_entry_ptr->corresponding_data_base_entry is %p\n",cache_entry_ptr->corresponding_data_base_entry);
        write_target_buffer_to_file("./target_buffer.txt");
        write_branch_trace_to_file("branch_trace.txt");
        write_ringbuffer_to_file("hash_value.txt");
        abort();

    }
    
    // Segment not in cache, verify from database (slow path)
    if(unlikely(verify_sub_path_without_init(current_start_addr, current_end_addr, hash) == 2)){
        // #ifndef ONLYVERIFY
            push(&ringbuffer_global,hash);
        // #endif
        // Cache the verified segment and hash
        cache_entry_ptr->key_pair = current_path_key;
        cache_entry_ptr->corresponding_data_base_entry = global_data_base_ptr;
        //cache_entry_ptr->hash_cache = hash;
        valid_hash_cache[hash_cache_idx] = hash;
        recordpoint_start_addr = current_end_addr;
        hash_global =hash;
        head=0;
        return;
    }
    printf("record start addr is %0x\n",current_start_addr);
    printf("record end addr is %0x\n",current_end_addr);
    printf("online hash is %llu\n",hash);
    write_target_buffer_to_file("./target_buffer.txt");
    write_branch_trace_to_file("branch_trace.txt");
    write_ringbuffer_to_file("hash_value.txt");
    abort();
}

//useless
void write_time_to_file(uint64_t time_interval)
{
    FILE *file = fopen("time.txt", "ab+");
    if (file == NULL) {
        printf("Error opening file!\n");
        return;
    }
    fprintf(file, "%lu\n", time_interval);
    fclose(file);
}
void init_database_ptr()
{
    char* ro_data=_custom_ro_data_start;
    char* ro_data_end=_custom_ro_data_end;
    // Early exit if database is empty
    if(ro_data >= ro_data_end || *(uint32_t*)ro_data == 0) {
        return ;
    }
    
    #ifdef DBG
        printf("start_addr is %0x\n",point->start_addr);
        printf("end_addr is %0x\n",point->end_addr);
    #endif
    
    uint32_t element_type=*(uint32_t*)(ro_data+8);//用于判断是否结束
    while(element_type != 0 && ro_data < ro_data_end)
    {
        uint64_t curr_key=*(uint64_t*)(ro_data);
        uint32_t hash_count=*(uint32_t*)(ro_data+12);
        ro_data+=16;
        // Compute segment indices only if first-level cache missed
        // uint64_t current_path_key = ((uint64_t)start_addr << 32) | end_addr;
        //uint32_t segment_cache_idx = (uint32_t)(current& HASHCACHE_MASK) ;
        uint32_t segment_cache_idx = MIX_ADDR(curr_key);
        valid_path_cache_entry* cache_entry_ptr = &sub_path_cache[segment_cache_idx];
        cache_entry_ptr->key_pair = curr_key;
        cache_entry_ptr->corresponding_data_base_entry = ro_data-16;
        ro_data+=HASH_OUTBYTES*hash_count;
        element_type = *(uint32_t*)(ro_data+8);
    }
}
//在操作开始处插入该函数,主要是负责初始化一些线程私有变量,也相当于checkpoint,初始化一些变量
void start_collecting(){
    #ifdef MEASURETIME
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t current_time = ts.tv_sec * 1000000000ULL + ts.tv_nsec; // 单位：纳秒
        uint64_t time_interval = current_time -previous_time;
        write_time_to_file(time_interval);
        previous_time = current_time;   
    #endif
    #ifdef COLLECTTIME
        start_measure=1;
        sen_addr_index=0;
        return;
    #endif
    #ifdef ONLYDFI
        start_measure=1;
        if(if_init_database==0){
            if_init_database=1;
            #ifdef COLLECTIME_FOR_ARDUPILOT
                record_time_flag=1;
            #endif
        }
        return;
    #endif
    #ifdef TEST
        mission_control();
        if(record_flag==0)
        {
            start_measure=0;
            return;
        }
    #endif
    if(if_init_database==0)
    {
        if_init_database=1;
        init_database_ptr();
        init_branch_lookup_table();
        #ifdef COLLECTIME_FOR_ARDUPILOT
            record_time_flag=1;
        #endif
        //init_secure_scheduler();
        #if !defined(SMALLTEST) && !defined(TEST)
            //ringbuffer_init();  // 必须在start_new_thread之前初始化
            start_new_thread();
            //start_random_update_monitor(50000);
        #endif
        //fd = create_and_open_file("./hash_value.txt");
        //init_database();
    }
    start_measure=1;
    srandom(time(NULL));  // 使用时间作为种子
    my_random.start_random =rand();  // 返回随机数
    recordpoint_start_addr=(uint32_t)__builtin_return_address(0);
    head=0;

    push(&ringbuffer_global,0);

    int idx = atomic_fetch_add_explicit(&branch_trace_head, 1, memory_order_relaxed);
    branch_trace[idx&BUFFER_MASK] = 2;
    //blake2s_init(&(blake_state),HASH_OUTBYTES);
    //XXHash64_init(&h, 0);
    return ;
}

//在操作结束处插入此函数。
void end_collecting(){
    #ifdef COLLECTTIME
        return;
    #endif
    #ifdef ONLYDFI
        start_measure=0;
        return;
    #endif
    #ifdef TEST
        if(record_flag==0)
        {
            return;
        }
    #endif
    start_measure=0;
    srandom(time(NULL));  // 使用时间作为种子
    my_random.end_random =rand();  // 返回随机数
    /* 如果 ringbuffer 为空，直接视作已验证（最常见情况） */
        // printf("ringbuffer->head is : %d\n",ringbuffer_global.head);
        // printf("ringbuffer->tail is : %d\n",ringbuffer_global.tail);
    if (head == 0) {
        recordpoint_start_addr =(uint32_t)__builtin_return_address(0);  // 可省略，视调用者而定
        // uint64_t hash=3244421341483603138;
        // push(&ringbuffer_global,hash);
        #if defined(SMALLTEST)  || defined(TEST)
            write_ringbuffer_to_file("hash_value.txt");
            write_branch_trace_to_file("branch_trace.txt");
            ringbuffer_global.head = 0;
            branch_trace_head = 0;
        #endif
        return;
    }


    // uint64_t hash = hash_global;
    #ifndef BLAKE2S_ENABLED
        uint64_t hash=XXH3_64bits(&target_buffer, head * sizeof(uint32_t));
    #endif
    #ifdef BLAKE2S_ENABLED
        uint64_t hash;
        blake2s(&hash,8, &target_buffer, head * sizeof(uint32_t), NULL, 0);
         if(judge_use_blake2s==0)
         {
            printf("use blake2s hash\n");
            judge_use_blake2s=1;
         }
    #endif
    // if(hash==hash_global)
    // {
    //     // #ifndef ONLYVERIFY
    //         push(&ringbuffer_global,hash);
    //     // #endif
    //     recordpoint_start_addr = (uint32_t)__builtin_return_address(0); 
    //     #if defined(SMALLTEST)  || defined(TEST)
    //         write_ringbuffer_to_file("hash_value.txt");
    //         write_branch_trace_to_file("branch_trace.txt");
    //         ringbuffer_global.head = 0;
    //         branch_trace_head = 0;
    //     #endif
    //     return;
    // }
    uint32_t hash_cache_idx =(uint32_t)( hash & HASHCACHE_MASK);
    uint32_t current_end_addr = (uint32_t)__builtin_return_address(0);
    //uint64_t hash_cache_entry=valid_hash_cache[hash%HASHCACHE_SIZE];
    if(likely(valid_hash_cache[hash_cache_idx] == hash)){
        recordpoint_start_addr = current_end_addr;
        hash_global = hash;
        // #ifndef ONLYVERIFY
            push(&ringbuffer_global,hash);
        // #endif
        head=0;
        #if defined(SMALLTEST)  || defined(TEST)
            write_ringbuffer_to_file("hash_value.txt");
            write_branch_trace_to_file("branch_trace.txt");
            ringbuffer_global.head = 0;
            branch_trace_head = 0;
        #endif
        return;
    }
    uint32_t current_start_addr=recordpoint_start_addr;
    // Compute segment indices only if first-level cache missed
    uint64_t current_path_key = ((uint64_t)current_start_addr ) | (((uint64_t)current_end_addr) << 32);
    uint32_t segment_cache_idx = MIX_ADDR(current_path_key);
    
    valid_path_cache_entry* cache_entry_ptr = &sub_path_cache[segment_cache_idx];
    // Third-level: segment match check

    
    if(likely(cache_entry_ptr->key_pair== current_path_key)){
        // Segment found, verify hash from database (fast path)
        if(likely(fast_verify_sub_path(hash, cache_entry_ptr->corresponding_data_base_entry) == 2)){
            //cache_entry_ptr->hash_cache = hash;
            valid_hash_cache[hash_cache_idx] = hash;
            recordpoint_start_addr=0;
            hash_global =hash;
            //#ifndef ONLYVERIFY
                push(&ringbuffer_global,hash);
            // #endif
            head=0;
            #if defined(SMALLTEST)  || defined(TEST)
                write_ringbuffer_to_file("hash_value.txt");
                write_branch_trace_to_file("branch_trace.txt");
                
                ringbuffer_global.head = 0;
                branch_trace_head = 0;
            #endif
            return;
        }
       write_target_buffer_to_file("./target_buffer.txt");
       write_branch_trace_to_file("branch_trace.txt");
       write_ringbuffer_to_file("hash_value.txt");
        abort();
    }

    //说明缓存中没有找到对应的段
    //printf("end_collecting not find in cache , verify from database\n");
    // Segment not in cache, verify from database (slow path)
    if(unlikely(verify_sub_path_without_init(current_start_addr, current_end_addr, hash) == 2)){
        // Cache the verified segment and hash
        cache_entry_ptr->key_pair = current_path_key;
        cache_entry_ptr->corresponding_data_base_entry = global_data_base_ptr;
        //#ifndef ONLYVERIFY
            push(&ringbuffer_global,hash);
        //#endif
        head=0;
        //cache_entry_ptr->hash_cache = hash;
        valid_hash_cache[hash_cache_idx] = hash;
        recordpoint_start_addr=0;
        hash_global =hash;
        #if defined(SMALLTEST)  || defined(TEST)
            write_ringbuffer_to_file("hash_value.txt");
            write_branch_trace_to_file("branch_trace.txt");

            ringbuffer_global.head = 0;
            branch_trace_head = 0;
        #endif
        //ringbuffer_global.head = 0;
        return;
    }
    write_target_buffer_to_file("./target_buffer.txt");
    write_branch_trace_to_file("branch_trace.txt");
    write_ringbuffer_to_file("hash_value.txt");
    abort();

}

//打印哈希
void print_hash(uint8_t *hs ,size_t length )
{
    for(size_t i=0;i<length;i++)
    {
        printf("%02x",hs[i]);

    }
    printf("\n");

}
#if !defined(SMALLTEST) && !defined(TEST)
    static int fd=-1; // 文件描述符，全局变量
    static int branch_trace_fd = -1; // 新增：branch_trace 文件描述符
#else 
    static FILE *fd=NULL; // 文件描述符，全局变量
#endif
    //write to file func------------------------------------------
void write_ringbuffer_to_file(const char* filename) {
    // 1. 打开文件 (覆盖模式：每次调用都会清空文件重新写)
    int file_fd = open(filename, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (file_fd < 0) {
        perror("Failed to open file");
        return;
    }

    // 2. 获取当前写到了哪里 (Head)
    size_t current_head = ringbuffer_global.head;

    // 4. 直接写入文件
    // 从 buffer 的起始地址 (&buffer[0]) 开始，写入 write_count 个元素
    ssize_t ret = write(file_fd, ringbuffer_global.buffer, current_head * sizeof(uint64_t));
    if (ret < 0) {
        perror("Write failed");
    } else {
        printf("Dumped %zu elements to %s\n", current_head, filename);
    }
    // 5. 关闭文件
    close(file_fd);
}
void write_target_buffer_to_file(const char* filename) {
    // 1. 打开文件 (追加模式)
    for(int i=0;i<head;i++)
    {
        printf("write_target_buffer_to_file target_buffer[i]: %0x\n", target_buffer[i]);
    }
    int file_fd = open(filename, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (file_fd < 0) {
        perror("Failed to open target_buffer file");
        return;
    }

    // 2. 写入当前 head 位置之前的所有数据
    if (head > 0) {
        ssize_t ret = write(file_fd, target_buffer, head * sizeof(uint32_t));
        if (ret < 0) {
            perror("Write target_buffer failed");
        } else {
            printf("Dumped %d target_buffer elements to %s\n", head, filename);
        }
    }
    
    // 3. 关闭文件
    close(file_fd);
}

void write_branch_trace_to_file(const char* filename) {
    // 1. 打开文件 (追加模式)
    int file_fd = open(filename, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (file_fd < 0) {
        perror("Failed to open file");
        return;
    }

    // 2. 获取当前写到了哪里 (Head) - 使用原子操作读取
    int current_head = atomic_load(&branch_trace_head);

    // 3. 直接写入文件
    // 从 branch_trace 的起始地址开始，写入 current_head 个元素
    ssize_t ret = write(file_fd, branch_trace, current_head * sizeof(uint8_t));
    if (ret < 0) {
        perror("Write failed");
    } else {
        printf("Dumped %d branch trace elements to %s\n", current_head, filename);
    }
    
    // 4. 关闭文件
    close(file_fd);
}

#if defined(SMALLTEST) || defined(TEST)
// 假设你的数据类型是 uint64_t，如果不是请修改这里
// 假设 BUFFER_SIZE 已经定义


int init_hash_file(const char *filename)
{
    fd = fopen(filename, "ab");   // binary + append
    if (!fd) {
        perror("fopen failed");
        return -1;
    }

    /* 可选：关闭用户态缓冲，降低实时性干扰 */
    // setvbuf(hash_fp, NULL, _IONBF, 0);

    return 0;
}
void write_uint64_to_file(uint64_t value) {

    ssize_t ret = fwrite(&value, sizeof(uint64_t), 1, fd);
    if (ret != 1) {
        perror("fwrite failed");
    }

}
void close_hash_file(void)
{
    if (fd) {
        fflush(fd);   // 确保数据写出
        fclose(fd);
        fd = NULL;
    }
}

#endif

//attestation thread--------------------------------------------


#if !defined(SMALLTEST) && !defined(TEST)
    // 假设 RingBuffer 的相关定义如下 (你需要根据实际情况保留你的头文件)
    // extern bool is_empty(RingBuffer* rb);
    // extern uint64_t pop(RingBuffer* rb);
    // extern RingBuffer ringBuffer;

    // ==========================================
    // 1. 简化的文件创建逻辑
    // ==========================================
    // 直接返回文件描述符 fd，方便后续写入
    int create_and_open_file(const char* filename) {
        // O_TRUNC: 如果文件存在，清空它
        // O_CREAT: 如果不存在，创建它
        // O_WRONLY: 只写
        // O_APPEND: 追加模式 (可选，但在 truncate 后通常是从头写)
        // 0644: 权限
        fd = open(filename, O_WRONLY | O_CREAT | O_APPEND, 0644);
        
        if (fd < 0) {
            perror("Failed to open file");
            return -1;
        }
        branch_trace_fd=open("branch_trace.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (branch_trace_fd < 0) {
            perror("Failed to open branch_trace file");
            close(fd);
            return -1;
        }
        printf("Successfully initialized file %s (fd: %d)\n", filename, fd);
        return fd;
    }

    // ==========================================
    // 2. 线程与写入逻辑 (核心优化)
    // ==========================================


    void* read_measurement(){
        if (fd < 0) return NULL;

        // 【优化点 1】: 提高阈值
        // 既然有 8MB 空间，我们积压 64KB (8192 * 8 bytes) 再写，吞吐量更高
        // 这对生产者毫无压力，因为还有 99% 的空间空闲
        const size_t WRITE_THRESHOLD = 8192; 
        const size_t BRANCH_WRITE_THRESHOLD = 8192; // branch_trace 的写入阈值
        // 用于 writev 的结构体数组，最多两块（尾部一块，头部一块）
        struct iovec iov[2]; 
        struct iovec branch_iov[2];

        while (true) {
            size_t head = atomic_load_explicit(&ringbuffer_global.head, memory_order_acquire);
            size_t tail = atomic_load_explicit(&ringbuffer_global.tail, memory_order_relaxed);
            size_t available = head - tail;

            if (available >= WRITE_THRESHOLD) {
                
                size_t write_idx = tail & BUFFER_MASK;
                size_t contiguous_len = BUFFER_SIZE - write_idx;
                
                // 【优化点 2】: 准备 writev 参数
                int iov_count = 0;
                size_t total_items_to_write = available; // 甚至可以限制一次最大写 1MB，防止单次IO太久
                
                if (available <= contiguous_len) {
                    // 情况 A: 没有回绕，或者数据都在第一段
                    iov[0].iov_base = &ringbuffer_global.buffer[write_idx];
                    iov[0].iov_len = available * sizeof(uint64_t);
                    iov_count = 1;
                } else {
                    // 情况 B: 发生回绕 (Wrap Around)
                    // 第一块：从 write_idx 到 数组末尾
                    iov[0].iov_base = &ringbuffer_global.buffer[write_idx];
                    iov[0].iov_len = contiguous_len * sizeof(uint64_t);
                    
                    // 第二块：从 数组开头 到 剩余部分
                    size_t wrapped_len = available - contiguous_len;
                    iov[1].iov_base = &ringbuffer_global.buffer[0];
                    iov[1].iov_len = wrapped_len * sizeof(uint64_t);
                    iov_count = 2;
                }

                // 【优化点 3】: 使用 writev 替代 write
                // 内核会将这就 1块 或 2块 内存合并写入文件，只需一次 syscall
                ssize_t bytes_written = writev(fd, iov, iov_count);
                
                if (bytes_written > 0) {
                    // 更新 tail
                    size_t items_written = bytes_written / sizeof(uint64_t);
                    atomic_store_explicit(&ringbuffer_global.tail, tail + items_written, memory_order_release);
                } else if (bytes_written < 0) {
                    perror("Writev failed");
                    break;
                }
            } 
            // ========== 处理 branch_trace ==========
                int branch_head = atomic_load_explicit(&branch_trace_head, memory_order_acquire);
                
                if (branch_head >= BRANCH_WRITE_THRESHOLD) {
                    // branch_trace 是线性数组，不需要考虑环形回绕
                    branch_iov[0].iov_base = branch_trace;
                    branch_iov[0].iov_len = branch_head * sizeof(uint8_t);
                    
                    ssize_t branch_bytes_written = writev(branch_trace_fd, branch_iov, 1);
                    
                    if (branch_bytes_written > 0) {
                        // 写入成功后重置 head
                        atomic_store_explicit(&branch_trace_head, 0, memory_order_release);
                    } else if (branch_bytes_written < 0) {
                        perror("Writev (branch_trace) failed");
                        break;
                    }
                }

            if ((available > 0 && available < WRITE_THRESHOLD) || (branch_head > 0 && branch_head < BRANCH_WRITE_THRESHOLD)){
                // 数据不足 64KB
                // usleep(730) 是 0.73ms，在大 buffer 下是合理的。
                // 也可以考虑自适应休眠：如果有数据但不够阈值，睡短点；如果完全空，睡长点。
                usleep(3000); 
            }
        }
        
        close(fd);
        close(branch_trace_fd);
        return NULL;
    }

    void run_thread(void *(*function)(void*), void* arg){
        pthread_t thread;
        // 将文件描述符传给线程
        int ret = pthread_create(&thread, NULL, function, arg);
        if (ret != 0) {
            printf("pthread_create failed: %s\n", strerror(ret));
            return;
        }
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(3, &cpuset);
        ret = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
        if (ret != 0) {
            printf("pthread_setaffinity_np failed after create: %s\n", strerror(ret));
        } else {
            printf("Thread bound to CPU 3\n");
        }
        //pthread_create(&thread, NULL, function, arg);
        pthread_detach(thread);
    }

    void start_new_thread(){
        // 1. 在主线程打开文件，确保线程启动时文件已就绪
        fd = create_and_open_file("./hash_value.txt");
        
        if (fd >= 0) {
        // run_thread(my_print, ...); 
        run_thread(read_measurement, NULL);
        }
    }
#endif


// random update monitor --------------------------------------------
#if !defined(SMALLTEST) && !defined(TEST)
static int random_timer_fd = -1;
static atomic_int random_monitor_started = 0;

static int create_periodic_timerfd(int interval_ms) {
    int fd_time = timerfd_create(CLOCK_MONOTONIC, 0);
    if (fd_time < 0) {
        perror("timerfd_create failed");
        return -1;
    }
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = interval_ms / 1000;
    its.it_value.tv_nsec = (long)(interval_ms % 1000) * 1000000L;
    its.it_interval = its.it_value; // periodic
    if (timerfd_settime(fd_time, 0, &its, NULL) < 0) {
        perror("timerfd_settime failed");
        close(fd_time);
        return -1;
    }
    return fd_time;
}

static void* random_monitor_thread(void* arg) {
    (void)arg;
    int last_start = my_random.start_random;
    int last_end = my_random.end_random;
    for (;;) {
        uint64_t expirations = 0;
        ssize_t r = read(random_timer_fd, &expirations, sizeof(expirations));
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("random_monitor read");
            break;
        }
        int curr_start = my_random.start_random;
        int curr_end = my_random.end_random;
        if (curr_start != last_start || curr_end != last_end) {
            last_start = curr_start;
            last_end = curr_end;
            printf("[random-monitor] updated: start=%d end=%d\n", curr_start, curr_end);
        } else {
            // no update detected this interval
        }
    }
    return NULL;
}

void start_random_update_monitor(int interval_ms) {
    if (atomic_exchange_explicit(&random_monitor_started, 1, memory_order_acq_rel)) return;
    random_timer_fd = create_periodic_timerfd(interval_ms);
    if (random_timer_fd < 0) {
        atomic_store_explicit(&random_monitor_started, 0, memory_order_release);
        return;
    }
    pthread_t t;
    int ret = pthread_create(&t, NULL, random_monitor_thread, NULL);
    if (ret != 0) {
        printf("random monitor pthread_create failed: %s\n", strerror(ret));
        close(random_timer_fd);
        random_timer_fd = -1;
        atomic_store_explicit(&random_monitor_started, 0, memory_order_release);
        return;
    }
    pthread_detach(t);
}
#endif


// //Periodic Random Self-Check engine functions---------------------------------------
// extern long loop_result;
// extern long mult_busy_loop(unsigned long execution_time);
// extern int test_finish[CFG_TEE_CORE_NB_CORE][5];
// extern int task1_exe_time[CFG_TEE_CORE_NB_CORE];
// extern getCurrentTime_micro();
// extern int get_current_core();
// extern uint64_t previous_random;
// struct checkpoint {
//     int start_random;
//     int end_random;
// }; 
// //初始化每个核上每个世界的参数
// void world_params_init(struct world_params *world_params, int secure_period,\
// 	int secure_budget, int non_secure_period, int non_secure_budget){
// 	world_params->secure_period = secure_period;//安全世界时间周期，指定一段时间循环的长度。
// 	world_params->secure_budget = secure_budget;//安全世界时间预算， 指定在一个周期内，允许该世界运行的时间。
// 	world_params->non_secure_period = non_secure_period;
// 	world_params->non_secure_budget = non_secure_budget;
// }

// //对每个任务的调度参数初始化
// void task_param_init(struct secure_task_params * task_params, int period,\
//  int priority, int execution_time, int task_init_flag, int cpu){
//  	task_params->period = period;
//  	task_params->priority = priority;
//  	task_params->execution_time = execution_time;
//  	task_params->task_init_flag = task_init_flag;
//  	task_params->cpu = cpu;
// }

// // void rt_tee_open_session(int taskId){
// // 	/* Initialize a context connecting us to the TEE */
// // 	results[taskId] = TEEC_InitializeContext(NULL, &contexts[taskId]);
// // 	if (results[taskId] != TEEC_SUCCESS)
// // 		errx(1, "TEEC_InitializeContext failed with code 0x%x", results[taskId]);

// // 	// * Open a session to the TA, the TA will perform the task
// // 	//TEEC_MEMREF_INOUT
// // 	//TEEC_MEMREF_TEMP_INPUT
// //     //zrz add to register a shared memory
// //     if(taskId>0)
// //     {
// //         shared_mem.buffer = &my_random;
// //         shared_mem.size = sizeof(Random);
// //         shared_mem.flags = TEEC_MEM_INPUT | TEEC_MEM_OUTPUT;
// //         results[taskId] = TEEC_RegisterSharedMemory(&contexts[taskId],&shared_mem);
// //         if (results[taskId] != TEEC_SUCCESS)
// //             errx(1, "TEEC_RegisterSharedMemory failed with code 0x%x", results[taskId]);
// //         task_params[taskId].cstm_param_addr = &shared_mem;
// //     }
// //     //zrz add end
// // 	operations[taskId].paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT, TEEC_NONE, TEEC_NONE, TEEC_NONE);
// // 	operations[taskId].params[0].memref.parent = &task_params[taskId];
// // 	operations[taskId].params[0].memref.offset = 0;
// // 	operations[taskId].params[0].memref.size = sizeof(struct secure_task_params);

// // 	results[taskId] = TEEC_OpenSession(&contexts[taskId], &sessions[taskId], uuid_tasks[taskId],
// // 			       TEEC_LOGIN_PUBLIC, NULL, &operations[taskId], &err_origins[taskId]);
// // 	if (results[taskId] != TEEC_SUCCESS)
// // 		errx(1, "TEEC_Opensession failed with code 0x%x origin 0x%x",
// // 			results[taskId], err_origins[taskId]);
// // }
// void rt_tee_open_session(int taskId){
//     /* Initialize a context connecting us to the TEE */
//     results[taskId] = TEEC_InitializeContext(NULL, &contexts[taskId]);
//     if (results[taskId] != TEEC_SUCCESS)
//         errx(1, "TEEC_InitializeContext failed with code 0x%x", results[taskId]);

//     // 初始化参数类型：注意第二个参数改为 TEEC_MEMREF_WHOLE
//     uint32_t param_type_1 = TEEC_NONE;
    
//     if(taskId > 0)
//     {
//         // 1. 注册共享内存
//         shared_mem.buffer = &my_random;
//         shared_mem.size = sizeof(Random);
//         shared_mem.flags = TEEC_MEM_INPUT | TEEC_MEM_OUTPUT;
        
//         results[taskId] = TEEC_RegisterSharedMemory(&contexts[taskId], &shared_mem);
//         if (results[taskId] != TEEC_SUCCESS)
//             errx(1, "TEEC_RegisterSharedMemory failed with code 0x%x", results[taskId]);

//         // 2. 标记我们要传递这块内存
//         param_type_1 = TEEC_MEMREF_WHOLE; 
//         // 注意：不要把 &shared_mem 地址赋值给 task_params 里的变量，那个没用
//     }

//     // 3. 设置参数类型：参数0是结构体，参数1是共享内存
//     operations[taskId].paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT, param_type_1, TEEC_NONE, TEEC_NONE);

//     // 设置参数 0 (task_params)
//     operations[taskId].params[0].memref.parent = &task_params[taskId];
//     operations[taskId].params[0].memref.offset = 0;
//     operations[taskId].params[0].memref.size = sizeof(struct secure_task_params);

//     // 4. 设置参数 1 (共享内存) - 只有当 param_type_1 是 WHOLE 时才设置
//     if (param_type_1 == TEEC_MEMREF_WHOLE) {
//         operations[taskId].params[1].memref.parent = &shared_mem; // 传入注册好的 shared_mem 结构体指针
//         operations[taskId].params[1].memref.offset = 0;
//         operations[taskId].params[1].memref.size = shared_mem.size;
//     }

//     results[taskId] = TEEC_OpenSession(&contexts[taskId], &sessions[taskId], uuid_tasks[taskId],
//                    TEEC_LOGIN_PUBLIC, NULL, &operations[taskId], &err_origins[taskId]);
                   
//     if (results[taskId] != TEEC_SUCCESS)
//         errx(1, "TEEC_Opensession failed with code 0x%x origin 0x%x",
//             results[taskId], err_origins[taskId]);
// }
// // void rt_tee_scheduler_start(struct world_params *curr_world_params){
// void rt_tee_scheduler_start(struct world_params *first_world_params, unsigned int cmd_id){//为每个核初始化每个世界的参数

// 	cpu_set_t mask;
// 	CPU_ZERO(&mask);
// 	CPU_SET(0, &mask);
// 	sched_setaffinity(0, sizeof(cpu_set_t), &mask);//设置cpu任务的关联性

// 	//initiate world scheduer params(world period and budget) and start rt-tee-scheduler
// 	memset(&operations[0], 0, sizeof(operations[0]));
// 	operations[0].paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT, TEEC_NONE,
// 					 TEEC_NONE, TEEC_NONE);

// 	// operations[0].params[0].memref.parent = curr_world_params;
// 	operations[0].params[0].memref.parent = first_world_params;
// 	operations[0].params[0].memref.offset = 0;
// 	// operations[0].params[0].memref.size = sizeof(struct world_params);
// 	operations[0].params[0].memref.size = CFG_TEE_CORE_NB_CORE * sizeof(struct world_params);

// 	//start scheduler
// 	results[0] = TEEC_InvokeCommand(&sessions[0], cmd_id, &operations[0],
// 				 &err_origins[0]);
// 	if (results[0] != TEEC_SUCCESS)
// 		errx(1, "TEEC_InvokeCommand failed with code 0x%x origin 0x%x",
// 			results[0], err_origins[0]);
// }

// void rt_tee_close_sessions(int taskId){
// 	TEEC_CloseSession(&sessions[taskId]);
// 	TEEC_FinalizeContext(&contexts[taskId]);
// }

// //trigger timer on core 1,2,3
// void trigger_timer_on_cores(int pin_core){//在pin_core核上启动调度

// 	cpu_set_t mask;//表示一个cpu集合
// 	CPU_ZERO(&mask);//清空一个集合
//     CPU_SET(pin_core, &mask);//将一个给定的cpu号pin_core，加入到集合mask中，cpu_clr()表示从一个集合中删除该cpu号。
//     sched_setaffinity(0, sizeof(cpu_set_t), &mask);//将某一个核和指定的线程绑定到一块运行。//sched_setaffinity(pid_t pid, unsigned int cpusetsize, cpu_set_t *mask)
// //如果pid的值为0,则表示指定的是当前进程,使当前进程运行在mask所设定的那些CPU上.
// //第二个参数cpusetsize是mask所指定的数的长度.通常设定为sizeof(cpu_set_t).如果当前pid所指定的进程此时没有运行在mask所指定的任意一个CPU上,则该指定的进程会从其它CPU上迁移到mask的指定的一个CPU上运行.
// //mask 即用户 通过CPU_SET 接口，线程ID 绑定到集合中的一个CPU上，使用mask来表示cpu集合中的CPU



// 	/* code */
// 	TEEC_Result res;
// 	TEEC_Context ctx;
// 	TEEC_Operation op;

// 	uint32_t err_origin;

// 	res = TEEC_InitializeContext(NULL, &ctx);
// 	if (res != TEEC_SUCCESS)
// 		errx(1, "TEEC_InitializeContext failed with code 0x%x", res);

// 	memset(&op, 0, sizeof(op));
// 	op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INOUT, TEEC_NONE,
// 					 TEEC_NONE, TEEC_NONE);
// 	op.params[0].value.a = 42;

// 	res = TEEC_InvokeCommand(&sessions[0], PTA_RT_TEE_TRIGGER_TIMER, &op,
// 				 &err_origin);	

// 	if (res != TEEC_SUCCESS)
// 		errx(1, "TEEC_InvokeCommand failed with code 0x%x origin 0x%x",
// 			res, err_origin);

// 	return ;
// }

// //init task ,only open session ,to do  modify here,register a shared memmory
// void rt_tee_task_init(int task_index){
// 	rt_tee_open_session(task_index);
// }
// //invoke command to start rt-tee-scheduler ,cmd_id = PTA_RT_TEE_SCHEDULER_CMD_START,only init the world parameters
// void world_sched_init(){
// 	rt_tee_scheduler_start(&each_core_world_params[0], PTA_RT_TEE_SCHEDULER_CMD_START);//只为0号core初始化每个世界的参数
// }
// //really start the rt-tee-scheduler, cmd_id =PTA_RT_TEE_TRIGGER_TIMER
// void world_sched_start(int core){
// 	trigger_timer_on_cores(core);//在core核上触发调度
// }

// void init_secure_scheduler()
// {
// 	cpu_set_t mask;
//  //    CPU_ZERO(&mask);
//  //    CPU_SET(1, &mask);
//  //    sched_setaffinity(0, sizeof(cpu_set_t), &mask);

// 	//initialize uuids for each task and period, budgets, and execution for each task
// 	//TEEC_UUID uuid_scheduler_start = TA_HELLO_WORLD_UUID;
// 	// TEEC_UUID uuid_dta_task1 = DTA_TASK1_UUID;
// 	// TEEC_UUID uuid_dta_task2 = DTA_TASK2_UUID;
// 	// TEEC_UUID uuid_dta_task3 = DTA_TASK3_UUID;
// 	// TEEC_UUID uuid_dta_task4 = DTA_TASK4_UUID;
// 	// TEEC_UUID uuid_dta_task5 = DTA_TASK5_UUID;
// 	// TEEC_UUID uuid_dta_task6 = DTA_TASK6_UUID;	
// 	TEEC_UUID uuid_task1 = PTA_TASK1_UUID;
// 	// TEEC_UUID uuid_task2 = PTA_TASK2_UUID;
// 	// TEEC_UUID uuid_task3 = PTA_TASK3_UUID;
// 	// TEEC_UUID uuid_task4 = PTA_TASK4_UUID;
// 	// TEEC_UUID uuid_task5 = PTA_TASK5_UUID;
// 	// TEEC_UUID uuid_task6 = PTA_TASK6_UUID;
// 	// TEEC_UUID uuid_task7 = PTA_TASK7_UUID;
// 	// TEEC_UUID uuid_task8 = PTA_TASK8_UUID;
// 	// TEEC_UUID uuid_task9 = PTA_TASK9_UUID;
// 	// TEEC_UUID uuid_task10 = PTA_TASK10_UUID;

// 	TEEC_UUID uuid_rt_tee_scheduler = PTA_RT_TEE_SCHEDULER_UUID;
// 	TEEC_UUID uuid_trigger_timer = PTA_TRIGGER_TIMER_UUID;

// 	uuid_tasks[0] = &uuid_rt_tee_scheduler;

// 	uuid_tasks[1] = &uuid_task1;
// 	// uuid_tasks[2] = &uuid_task2;
// 	// uuid_tasks[3] = &uuid_task3;
// 	// uuid_tasks[4] = &uuid_task4;
// 	// uuid_tasks[5] = &uuid_task5;


// 	// uuid_tasks[1] = &uuid_dta_task1;
// 	// uuid_tasks[2] = &uuid_dta_task2;
// 	// uuid_tasks[3] = &uuid_dta_task3;
// 	// uuid_tasks[4] = &uuid_dta_task4;
// 	// uuid_tasks[5] = &uuid_dta_task5;
// 	// uuid_tasks[6] = &uuid_dta_task6;	

// 	//set task one params(period, priority, execution, \
// 	whether a task which need to allocate task storage struct, asigned cpu)
// 	task_param_init(&task_params[0], 1,1,1,0,0); 
// 	// // 70%
// 	task_param_init(&task_params[1], 51000,0,7000,1,0); 
// // 	task_param_init(&task_params[2], 65000,1,7000,1,0); 
// // 	task_param_init(&task_params[3], 142000,2,6000,1,0);
// // 	task_param_init(&task_params[4], 214000,3,9000,1,0); 
// 	// task_param_init(&task_params[5], 285000,4,8000,1,0);	

// 	//setting secure_period, secure_budegt, non_secure_period, non_secure_budget
// 	//50:50
// 	// //70%
//  	world_params_init(&each_core_world_params[0], 20000, 9400, 22000, 12800);
// 	//world_params_init(&each_core_world_params[1], 20000, 9400, 22000, 12800);
// 	//world_params_init(&each_core_world_params[2], 20000, 9400, 22000, 12800);
// 	//world_params_init(&each_core_world_params[3], 20000, 9400, 22000, 12800);

// 	//corresponding normal world tasks execution parameters (execution time, period) ms
// 	//(8, 60)
// 	//(5, 78)
// 	//(9, 98)
// 	//(6, 121)
// 	//(7, 500)


// 	//open session for rt-tee-scheduler initialization and start, 
// 	//the task param for scheduelr start task is meaningless
// 	for(int i = 1; i <= SECURE_TASK_NUM; i++){
// 		rt_tee_task_init(i);
// 	}
// 	rt_tee_task_init(0);


// 	//this will cause dta invocation fails
// 	// rt_tee_open_session(6);

// 	//open session for each secure tasks and creation tasks in rt-tee



// 	//initiate world scheduer params(world period and budget) and start rt-tee-scheduler
// 	world_sched_init();//初始化每个世界的参数，并没有启动scheduler

// 	//trigger_timer_on_cores(0);
// 	world_sched_start(0);//在0号核上触发调度。

// 	// trigger_timer_on_cores(2);

// 	// trigger_timer_on_cores(2);

	
// 	sleep(1000);

// 	// finish tasks and close all sessions to destroy the context
// 	for(int i = 0; i <= SECURE_TASK_NUM; i++){
// 		rt_tee_close_sessions(i);
// 	}
// 	return 0;
// }
///###########################  hardware secure timer#################
#ifdef HARDWARESECURE
    TEEC_Context ctx;
    TEEC_Session sess;
    TEEC_SharedMemory shm; 
    #define CMD_INIT_DYNAMIC_MONITOR 0x102 
    #define TA_RTOAI_UUID \
        { 0x12345678, 0x8765, 0x4321, { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 } }
    #define INITIAL_RAND_SEED        0x12345678 
    void cleanup_tee() {
        TEEC_ReleaseSharedMemory(&shm);
        TEEC_CloseSession(&sess);
        TEEC_FinalizeContext(&ctx);
    }
    /* 初始化 TEE 会话并分配动态共享内存 */
    uint32_t* setup_secure_monitor(uint32_t interval_ms) {
        TEEC_Result res;
        TEEC_Operation op;
        TEEC_UUID uuid = TA_RTOAI_UUID;
        uint32_t err_origin;

        /* 1. 初始化 TEE 上下文 */
        res = TEEC_InitializeContext(NULL, &ctx);
        if (res != TEEC_SUCCESS) {
            fprintf(stderr, "TEEC_InitializeContext failed: 0x%x\n", res);
            exit(1);
        }

        /* 2. 打开会话 */
        res = TEEC_OpenSession(&ctx, &sess, &uuid,
                            TEEC_LOGIN_PUBLIC, NULL, NULL, &err_origin);
        if (res != TEEC_SUCCESS) {
            fprintf(stderr, "TEEC_OpenSession failed: 0x%x\n", res);
            TEEC_FinalizeContext(&ctx);
            exit(1);
        }

        /* 3. 【核心修改】动态分配共享内存 */
        /* 这种方式不需要 root 权限访问 /dev/mem，也不依赖设备树地址 */
        shm.size = 4096; // 分配一个页的大小
        shm.flags = TEEC_MEM_INPUT | TEEC_MEM_OUTPUT; // 允许双向读写

        res = TEEC_AllocateSharedMemory(&ctx, &shm);
        if (res != TEEC_SUCCESS) {
            fprintf(stderr, "TEEC_AllocateSharedMemory failed: 0x%x\n", res);
            cleanup_tee();
            exit(1);
        }

        uint32_t *rand_ptr = (uint32_t *)shm.buffer;
        
        /* 4. 在通知 TA 之前先初始化内存值 (防止由于竞争导致的误报) */
        *rand_ptr = INITIAL_RAND_SEED;

        /* 5. 准备参数：将内存引用发送给 TA */
        memset(&op, 0, sizeof(op));
        /* 参数 0: 内存引用 (整个分配的内存) */
        op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_WHOLE, TEEC_VALUE_INPUT, TEEC_NONE, TEEC_NONE);
        op.params[0].memref.parent = &shm;
        op.params[0].memref.offset = 0;
        op.params[0].memref.size = shm.size;

        /* 参数 1: 监控间隔时间 */
        op.params[1].value.a = interval_ms;

        /* 6. 发送指令：TA 接收后会记录该内存物理地址并启动定时器 */
        printf("[CA] Registering Dynamic SHM and Starting Secure Timer...\n");
        res = TEEC_InvokeCommand(&sess, CMD_INIT_DYNAMIC_MONITOR, &op, &err_origin);

        if (res != TEEC_SUCCESS) {
            fprintf(stderr, "TEEC_InvokeCommand failed: 0x%x\n", res);
            cleanup_tee();
            exit(1);
        }

        printf("[CA] SUCCESS! Monitor active at dynamic buffer %p\n", shm.buffer);
        return rand_ptr;
    }
#endif
#ifndef SHADOW_STACK
void collect_bx_lr(uint32_t dest)
{
    return;
}
void collect_bl(uint32_t src )
{
    return;
}
void collect_bl_pred(uint32_t cpsr, uint32_t condtype,uint32_t src)
{
    return;
}


#endif
#ifdef SHADOW_STACK
    void collect_bx_lr(uint32_t dest)
    {

        #ifdef ONLYDFI
            return;
        #endif
        #ifdef COLLECTTIME
            return;
        #endif
        //start_measure=(int*)pthread_getspecific(key);
        if (unlikely(start_measure == 0))
        {
            return;
        }

        uint32_t destAddr=dest;
        uint32_t correct_target=shadow_stack.buffer[shadow_stack.top];
        if(unlikely(shadow_stack.top == 0))
        {
            printf("top is 0\n");
            abort();
            return;
        }
        shadow_stack.top--;
        if(likely(correct_target == dest))
        {
            return;
        }
        else
        {
            printf("occur backward break\n");
            abort();
            return ;
            

        }
        #ifdef DBG
            printf("collect_bx_lr---dest:%0x---pthreadID is:%d\n",dest,pthread_self());
        #endif
        //pthread_mutex_unlock(&lock);
        return;

    }
    void collect_bl(uint32_t src )
    {

        #ifdef ONLYDFI
            return;
        #endif
        #ifdef COLLECTTIME
            return;
        #endif
        if(likely(start_measure == 0))
        {
            return;
        }
        #ifdef DBG
            printf("run bl shadow stack\n");
        #endif
        shadow_stack.top++;
        shadow_stack.buffer[shadow_stack.top]=src+4;
        return;
    }
    void collect_bl_pred(uint32_t cpsr, uint32_t condtype,uint32_t src)
    {

        #ifdef ONLYDFI
            return;
        #endif
        #ifdef COLLECTTIME
            return;
        #endif
        if (likely(start_measure == 0))
        {
            return;
        }
        Condcode condtype1=(Condcode)condtype;
        bool branch_taken=isBranchTaken(condtype1,cpsr);
        if(branch_taken)
        {
            shadow_stack.top++;
            shadow_stack.buffer[shadow_stack.top]=src+4;
            #ifdef DBG
                printf("collect_indirect_jump_pred--src:%0x----dest:%0x---pthreadID is:%d\n",src,dest,pthread_self());
            #endif
            return;
        }
        else{

            return;
        }  
    }
#endif