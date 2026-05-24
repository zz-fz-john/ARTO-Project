//
// functions to support precision landing
//

#include "Rover.h"
#include <iostream>
#include <time.h>
#include <AP_WCET/AP_WCET.h>

namespace {

unsigned long long update_precland_total = 0;
unsigned long long update_precland_count = 0;
uint64_t update_precland_t1;
uint64_t update_precland_t2;
TimingLogger update_precland_logger("update_precland");

}
#if PRECISION_LANDING == ENABLED

void Rover::init_precland()
{
    rover.precland.init(400);
}

void Rover::update_precland()
{
    update_precland_t1 = AP_WCET::now_us();
    // alt will be unused if we pass false through as the second parameter:
    precland.update(0, false);
    update_precland_t2 = AP_WCET::now_us();
    uint64_t duration = update_precland_t2 - update_precland_t1;
    update_precland_total += duration;
    update_precland_count += 1;
    
    update_precland_logger.log(duration);
    if (update_precland_count % 1000 == 0){
        printf("average update_precland measure time microseconds is %f!\n",update_precland_total/1000.0);
        update_precland_total = 0;
        update_precland_count = 0; 
    }
    return;
}
#endif
