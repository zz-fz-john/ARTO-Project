//optee_os/core/pta/rtoai_pta.c
//srcs-y += rtoai_pta.c
#include <kernel/pseudo_ta.h>
#include <kernel/msg_param.h>
#include <mm/core_memprot.h>
#include <drivers/rtoai_secure_timer.h> 


#define TA_RTOAI_UUID \
    { 0x12345678, 0x8765, 0x4321, { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 } }

#define CMD_INIT_DYNAMIC_MONITOR 0x102


static TEE_Result invoke_command(void *pSessionContext __unused,
                                 uint32_t nCommandID,
                                 uint32_t nParamTypes,
                                 TEE_Param pParams[4])
{
    switch (nCommandID) {
    case CMD_INIT_DYNAMIC_MONITOR:

        if (TEE_PARAM_TYPE_GET(nParamTypes, 0) != TEE_PARAM_TYPE_MEMREF_INOUT) {
            return TEE_ERROR_BAD_PARAMETERS;
        }
        if (TEE_PARAM_TYPE_GET(nParamTypes, 1) != TEE_PARAM_TYPE_VALUE_INPUT) {
            return TEE_ERROR_BAD_PARAMETERS;
        }

        /* 2. 提取参数 */
        void *va = pParams[0].memref.buffer; 
        size_t size = pParams[0].memref.size;
        uint32_t interval = pParams[1].value.a;

        if (!va || size < sizeof(uint32_t)) {
            return TEE_ERROR_BAD_PARAMETERS;
        }

    
        return rtoai_secure_timer_start(interval, va,size);//version >=3.14
        //return rtoai_secure_timer_start(interval, va);//version <3.14
    default:
        return TEE_ERROR_NOT_SUPPORTED;
    }
}


pseudo_ta_register(.uuid = TA_RTOAI_UUID, 
                   .name = "rtoai_service",
                   .flags = PTA_DEFAULT_FLAGS | TA_FLAG_SECURE_DATA_PATH,
                   .invoke_command_entry_point = invoke_command);