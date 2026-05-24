//use to measure time 

#include "measure_time.h"
//初始化时间测量，打开日志
//filename:日志文件名
//mode:文件打开模式（“a”为追加，“w”为覆盖)
//返回初始化的时间测量结构体指针，失败时返回NULL
TimeMeasurement *init_time_measurement(const char * filename,const char * mode){
    TimeMeasurement *tm=(TimeMeasurement *)malloc(sizeof(TimeMeasurement));
    if(!tm) {
        perror("Memory allcotion failed");
        return NULL;
    }
    tm->log_file = fopen(filename, mode);
    if(!tm->log_file) {
        perror("Failed to open log file");
        free(tm);
        return NULL;
    }
    //写入日志文件头
    fprintf(tm->log_file,"----Time Measurement Log ----\n");
    fprintf(tm->log_file, "%-25s | %-15s | %-10s\n", 
            "Operation", "Time (seconds)", "Time (ns)");
    fprintf(tm->log_file,"----------------------------------------\n");
    return tm;
}
//开始计时
//tm: 时间测量结构体指针
//operation_name:操作名称，用于记录日志
void start_measurement(TimeMeasurement *tm,const char * operation_name){
    if (!tm) return;
    tm->operation_name = operation_name;
    clock_gettime(CLOCK_MONOTONIC, &tm->start_time);
}
// 停止计时并记录到文件
// tm: 时间测量结构体指针
// 返回操作耗时（纳秒）
long long end_measurement(TimeMeasurement* tm) {
    if (!tm) return 0;
    
    clock_gettime(CLOCK_MONOTONIC, &tm->end_time);
    
    long long start_ns = tm->start_time.tv_sec * 1000000000LL + tm->start_time.tv_nsec;
    long long end_ns = tm->end_time.tv_sec * 1000000000LL + tm->end_time.tv_nsec;
    long long elapsed_ns = end_ns - start_ns;
    double elapsed_seconds = elapsed_ns / 1000000000.0;
    
    // 写入日志
    if (tm->log_file && tm->operation_name) {
        fprintf(tm->log_file, "%-25s | %-15.9f | %-10lld\n", 
                tm->operation_name, elapsed_seconds, elapsed_ns);
        fflush(tm->log_file); // 确保立即写入文件
    }
    
    return elapsed_ns;
}
// 清理并关闭文件
// tm: 时间测量结构体指针
void cleanup_measurement(TimeMeasurement* tm) {
    if (!tm) return;
    
    if (tm->log_file) {
        fprintf(tm->log_file, "----------------------------------------------------\n");
        fclose(tm->log_file);
    }
    
    free(tm);
}