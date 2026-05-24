#include "data_flow.h"
//#define ADD_OAT
#define ALIASTABLE
//#define COLLECTTIME
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
//#define SMALLTEST //enable for small test case,
extern __thread int start_measure __attribute__((tls_model("local-exec")));
//extern __thread int start_measure __attribute__((tls_model("initial-exec")));
//static char* custom_heap_ptr_data_flow_base = (char*)_custom_heap_start_data_flow_base;
//__attribute__((section(".custom_heap_data_base")))
uint32_t def_use_table[MAX_SET_COUNT][MAX_SET_SIZE] = {0};
__attribute__((section(".custom_heap_data_base")))
uint32_t sen_addr[MAX_SET_SIZE]={0};
__attribute__((section(".custom_heap_data_base")))
uint32_t sen_addr_index=0;
#ifdef ALIASTABLE
    uint8_t set_addr_map[MAX_SET_COUNT][MAX_SET_SIZE] = {0};
#endif
#ifdef ADD_OAT
    //test oat
    uint32_t def_table[MAX_SET_SIZE]= {0};
#endif

static inline uint32_t idx_mix(uint32_t a) {
    a ^= a >> 4;
    a *= 0x27d4eb2dU;   
    return a & SET_MASK;
}

void def_check(uint32_t addr ,uint32_t val,int setID)
{
    #ifdef ONLYCFI
        return;
    #endif
    #ifndef SMALLTEST    
        if (start_measure==0)
        {
            return;
        }
    #endif
    // int has_found_addr=0;
    // for(int i=0;i<sen_addr_index;i++)
    // {
    //     if(addr==sen_addr[i])
    //     {
    //         has_found_addr=1;   
    //         break;
    //     }
    // }
    // if(has_found_addr==0)
    // {
    //     sen_addr[sen_addr_index]=addr;
    //     sen_addr_index++;
    // }
    #ifdef ADD_OAT
        printf("def_check addr is %x, val is %x, setID is %d\n", addr, val, setID);  
        def_table[idx_mix(addr)]=val;
    #endif
    #ifdef ALIASTABLE
        set_addr_map[setID][idx_mix(addr)] = 1;
    #endif
    def_use_table[setID][idx_mix(addr)]=val;
}
void non_sen_def_check(uint32_t addr )
{
    #ifdef ONLYCFI
        return;
    #endif
    #ifndef SMALLTEST    
       if (start_measure==0)
        {
            return;
        }
    #endif
    //printf("non_sen_def_check addr is %x\n", addr);
    int i=0;
    while(i!=sen_addr_index)
    {
        
        if(addr==sen_addr[i])
        {
            printf("data flow error! addr is %x\n", addr);
            return;
        }
        i++;
    }
}
void use_check(uint32_t addr ,uint32_t val,int setID)
{
    #ifdef ONLYCFI
        return;
    #endif
    #ifndef SMALLTEST    
        if (start_measure==0)
        {
            return;
        }
    #endif
    #ifdef ADD_OAT
        if(def_table[idx_mix(addr)]==val)
        {
            printf("data flow correct! addr is %x, val is %x, setID is %d\n", addr, val, setID);
        }
        if(def_table[idx_mix(addr)]!=val)
        {
            printf("data flow error! addr is %x, val is %x, setID is %d\n", addr, val, setID);
            printf("expected value is %x\n",def_table[idx_mix(addr)]);
            return;
        }
    #endif
    #ifdef ALIASTABLE
        if (set_addr_map[setID][idx_mix(addr)] == 0)
        {
            printf("use_check data flow error! addr is %x, val is %x, setID is %d\n", addr, val, setID);
            printf("address not defined before use!\n");
            abort();
        }
    #endif
    //printf("use_check addr is %x, val is %x, setID is %d\n", addr, val, setID);
    if(def_use_table[setID][idx_mix(addr)]!=val)
    {
        // if(def_use_table[setID][idx_mix(addr)]==0 )
        // {
        //     def_use_table[setID][idx_mix(addr)]=val;
        //     return;
        // }
        printf("use_check data flow error! addr is %x, val is %x, setID is %d\n", addr, val, setID);
        printf("expected value is %x\n",def_use_table[setID][idx_mix(addr)]);
        abort();
        return;
    }
    else
    {
        //printf("data flow correct! addr is %x, val is %x, setID is %d\n", addr, val, setID);
        return;
    }
}
void use_check_for_ptr_in_struct(uint32_t addr ,uint32_t val,int setID)
{
    #ifdef ONLYCFI
        return;
    #endif
    #ifndef SMALLTEST    
       if (start_measure==0)
        {
            return;
        }
    #endif
    //return;
    int i=0;
    int has_found=0;
    // while(i!=sen_addr_index)
    // {
        
    //     if(addr==sen_addr[i])
    //     {
    //         has_found=1;   
    //         break;
    //     }
    //     i++;
    // }
    //printf("use_check addr is %x, val is %x, setID is %d\n", addr, val, setID);
    uint32_t expected_value=def_use_table[setID][idx_mix(addr)];
    // if(has_found==1)
    // {
        if(expected_value!=val)
        {
            //printf("use_check_for_ptr_in_struct data flow error! addr is %x, val is %x, setID is %d\n", addr, val, setID);
            //printf("expected value is %x\n",expected_value);
             //abort();
            return;
        }
        else
        {
            //printf("data flow correct! addr is %x, val is %f, setID is %d\n", addr, val, setID);
            return;
        }
    // }
    // else
    //     return;

}
void use_check_for_basic_type_in_struct(uint32_t addr ,uint32_t val,int setID)
{
    #ifdef ONLYCFI
        return;
    #endif
    #ifndef SMALLTEST    
        if (start_measure==0)
        {
            return;
        }
    #endif
    return;
    // int i=0;
    // int has_found=0;
    // while(i!=sen_addr_index)
    // {
        
    //     if(addr==sen_addr[i])
    //     {
    //         has_found=1;   
    //         break;
    //     }
    //     i++;
    // }
    //printf("use_check addr is %x, val is %x, setID is %d\n", addr, val, setID);
    uint32_t expected_value=def_use_table[setID][idx_mix(addr)];
    // if(has_found==1)
    // {
        if(expected_value!=val)
        {
            // printf("use_check_for_basic_type_in_struct data flow error! addr is %x, val is %x, setID is %d\n", addr, val, setID);
            // printf("expected value is %x\n",expected_value);
            // abort();
            return;
        }
        else
        {
            //printf("data flow correct! addr is %x, val is %f, setID is %d\n", addr, val, setID);
            return;
        }
    // }
    // else
    //     return;

}
void def_check_for_ptr_in_struct(uint32_t addr ,uint32_t val,int setID)
{
    #ifdef ONLYCFI
        return;
    #endif
    #ifndef SMALLTEST    
        if (start_measure==0)
        {
            return;
        }
    #endif
    // int has_found_addr=0;
    // for(int i=0;i<sen_addr_index;i++)
    // {
    //     if(addr==sen_addr[i])
    //     {
    //         has_found_addr=1;   
    //         break;
    //     }
    // }
    // if(has_found_addr==0)
    // {
    //     sen_addr[sen_addr_index]=addr;
    //     sen_addr_index++;
    // }
    #ifdef ADD_OAT
        printf("def_check addr is %x, val is %x, setID is %d\n", addr, val, setID);
        def_table[idx_mix(addr)]=val;
    #endif
    #ifdef ALIASTABLE
        set_addr_map[setID][idx_mix(addr)] = 1;
    #endif
    def_use_table[setID][idx_mix(addr)]=val;
}
void def_check_for_basic_type_in_struct(uint32_t addr ,uint32_t val,int setID)
{
    #ifdef ONLYCFI
        return;
    #endif
    #ifndef SMALLTEST    
        if (start_measure==0)
        {
            return;
        }
    #endif

    //int has_found_addr=0;
    // for(int i=0;i<sen_addr_index;i++)
    // {
    //     if(addr==sen_addr[i])
    //     {
    //         has_found_addr=1;   
    //         break;
    //     }
    // }
    // if(has_found_addr==0)
    // {
    //     sen_addr[sen_addr_index]=addr;
    //     sen_addr_index++;
    // }

    #ifdef ADD_OAT
        printf("def_check addr is %x, val is %x, setID is %d\n", addr, val, setID);
        def_table[idx_mix(addr)]=val;
    #endif
    #ifdef ALIASTABLE
        set_addr_map[setID][idx_mix(addr)] = 1;
    #endif
    def_use_table[setID][idx_mix(addr)]=val;
}
void Critical_def_check(uint32_t addr ,uint32_t val,int setID,uint32_t maxvalue,uint32_t minvalue)
{
    #ifdef ONLYCFI
        return;
    #endif
    #ifndef SMALLTEST    
        if (start_measure==0)
        {
            return;
        }
    #endif
    // float maxVar= (float)maxvalue;
    // float minVar= (float)minvalue;
    // float currVar= (float)val;
    
    // if(currVar>maxVar||currVar<minVar)
    if(val>maxvalue||val<minvalue)
    {
        //printf("critical variable value error! addr is %x, val is %x, setID is %d\n", addr, val, setID);
        def_use_table[setID][idx_mix(addr)]=val;
        return;
    }
    // int has_found_addr=0;
    // for(int i=0;i<sen_addr_index;i++)
    // {
    //     if(addr==sen_addr[i])
    //     {
    //         has_found_addr=1;   
    //         break;
    //     }
    // }
    // if(has_found_addr==0)
    // {
    //     sen_addr[sen_addr_index]=addr;
    //     sen_addr_index++;
    // }

    //printf("critical_def_check addr is %x, val is %x, setID is %d\n", addr, val, setID);
    #ifdef ADD_OAT
        printf("def_check addr is %x, val is %x, setID is %d\n", addr, val, setID);
        def_table[idx_mix(addr)]=val;
    #endif
    #ifdef ALIASTABLE
        set_addr_map[setID][idx_mix(addr)] = 1;
    #endif
    def_use_table[setID][idx_mix(addr)]=val;
}
void Critical_def_check_for_float(uint32_t addr ,float val,int setID,float maxvalue,float minvalue)
{
    #ifdef ONLYCFI
        return;
    #endif
    #ifndef SMALLTEST    
        if (start_measure == 0)
        {
            return;
        }
    #endif

    if(val>maxvalue||val<minvalue)
    {
        //printf("critical variable value error! addr is %x, val is %f, setID is %d\n", addr, val, setID);
        def_use_table[setID][idx_mix(addr)]=*(uint32_t*)&val;
        return;
    }
    //int has_found_addr=0;
    // for(int i=0;i<sen_addr_index;i++)
    // {
    //     if(addr==sen_addr[i])
    //     {
    //         has_found_addr=1;   
    //         break;
    //     }
    // }
    // if(has_found_addr==0)
    // {
    //     sen_addr[sen_addr_index]=addr;
    //     sen_addr_index++;
    // }

    //printf("def_check addr is %x, val is %f, setID is %d\n", addr, val, setID);
    #ifdef ALIASTABLE
        set_addr_map[setID][idx_mix(addr)] = 1;
    #endif
    def_use_table[setID][idx_mix(addr)]=*(uint32_t*)&val;
}
#ifdef COLLECTTIME
    void def_collect(uint32_t val,uint32_t CriticalPointID)
    {
        
        if (likely(start_measure == 0))
        {
            return;
        }
        FILE *fp;
        float currVar= (float)val;
        fp = fopen("collected_variable_value.txt", "a");
        if (fp == NULL) {
            printf("Error opening file!\n");
            return;
        }
        fprintf(fp, "CriticalPointID: %d, Value: %f\n", CriticalPointID, currVar);
        fclose(fp);
    }
    void def_collect_for_float(float val,uint32_t CriticalPointID)
    {
       
        if (likely(start_measure == 0))
        {
            return;
        }
        FILE *fp;
        float currVar= (float)val;
        fp = fopen("collected_variable_value.txt", "a");
        if (fp == NULL) {
            printf("Error opening file!\n");
            return;
        }
        fprintf(fp, "CriticalPointID: %d, Value: %f\n", CriticalPointID, currVar);
        fclose(fp);
    }
#endif
