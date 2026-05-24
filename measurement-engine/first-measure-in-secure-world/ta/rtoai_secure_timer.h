//optee_os/core/include/drivers/rtoai_secure_timer.h
#ifndef RTOAI_SECURE_TIMER_H
#define RTOAI_SECURE_TIMER_H

#include <tee_api_types.h>


TEE_Result rtoai_secure_timer_start(uint32_t interval_ms, void *va, size_t len);//version >=3.14
//TEE_Result rtoai_secure_timer_start(uint32_t interval_ms, void *va); //version <3.14

#endif /* RTOAI_SECURE_TIMER_H */