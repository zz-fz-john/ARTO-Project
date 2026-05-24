//use to measure time 
#pragma once
#ifdef __cplusplus
extern "C"{
#endif
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
// time measure struct 
typedef struct {
    struct timespec start_time;
    struct timespec end_time;
    const char * operation_name;
    FILE * log_file;
} TimeMeasurement;
TimeMeasurement *init_time_measurement(const char * filename,const char * mode);
void start_measurement(TimeMeasurement *tm,const char * operation_name);
long long end_measurement(TimeMeasurement* tm) ;
void cleanup_measurement(TimeMeasurement* tm);
#ifdef __cplusplus
}
#endif