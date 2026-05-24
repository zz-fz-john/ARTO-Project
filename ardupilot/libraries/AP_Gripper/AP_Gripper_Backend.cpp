#include "AP_Gripper_Backend.h"
#include <AP_Math/AP_Math.h>
#include <iostream>
#include <time.h>
#include <chrono>
#include <AP_WCET/AP_WCET.h>

namespace {

unsigned long long AP_Gripper_Backend_update_total = 0;
unsigned long long AP_Gripper_Backend_update_count = 0;
uint64_t AP_Gripper_Backend_update_t1;
uint64_t AP_Gripper_Backend_update_t2;
TimingLogger AP_Gripper_Backend_update_logger("AP_Gripper_Backend_update");

}
extern const AP_HAL::HAL& hal;

void AP_Gripper_Backend::init()
{
    init_gripper();
}

// update - should be called at at least 10hz
void AP_Gripper_Backend::update()
{
    AP_Gripper_Backend_update_t1= AP_WCET::now_us();
    update_gripper();

    // close the gripper again if autoclose_time > 0.0
    if (config.state == AP_Gripper::STATE_RELEASED && (_last_grab_or_release > 0) &&
        (is_positive(config.autoclose_time)) &&
        (AP_HAL::millis() - _last_grab_or_release > (config.autoclose_time * 1000.0))) {
        grab();
    }
    AP_Gripper_Backend_update_t2 = AP_WCET::now_us();
    uint64_t duration = AP_Gripper_Backend_update_t2 -AP_Gripper_Backend_update_t1;
    AP_Gripper_Backend_update_total += duration;
    AP_Gripper_Backend_update_count += 1;
    if (AP_Gripper_Backend_update_count % 1000 == 0){
        //printf("average AP_Gripper_Backend_update measure time microseconds is %f!\n",AP_Gripper_Backend_update_total/1000.0);
        AP_Gripper_Backend_update_total = 0;
        AP_Gripper_Backend_update_count = 0;
    }
    AP_Gripper_Backend_update_logger.log(duration);
}
