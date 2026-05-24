#include "Copter.h"
#include <time.h>
#include <chrono>
#include <AP_WCET/AP_WCET.h>

namespace {

unsigned long long landinggear_update_total = 0;
unsigned long long landinggear_update_count = 0;
uint64_t landinggear_update_t1;
uint64_t landinggear_update_t2;
TimingLogger landinggear_update_logger("landinggear_update");

}
#if LANDING_GEAR_ENABLED == ENABLED

// Run landing gear controller at 10Hz
void Copter::landinggear_update()
{
    landinggear_update_t1 = AP_WCET::now_us();
    // exit immediately if no landing gear output has been enabled
    if (!SRV_Channels::function_assigned(SRV_Channel::k_landing_gear_control)) {
        return;
    }

    // support height based triggering using rangefinder or altitude above ground
    int32_t height_cm = flightmode->get_alt_above_ground_cm();

    // use rangefinder if available
    switch (rangefinder.status_orient(ROTATION_PITCH_270)) {
    case RangeFinder::Status::NotConnected:
    case RangeFinder::Status::NoData:
        // use altitude above home for non-functioning rangefinder
        break;

    case RangeFinder::Status::OutOfRangeLow:
        // altitude is close to zero (gear should deploy)
        height_cm = 0;
        break;

    case RangeFinder::Status::OutOfRangeHigh:
    case RangeFinder::Status::Good:
        // use last good reading
        height_cm = rangefinder_state.alt_cm_filt.get();
        break;
    }

    landinggear.update(height_cm * 0.01f); // convert cm->m for update call
    landinggear_update_t2 = AP_WCET::now_us();
    uint64_t duration = landinggear_update_t2 -landinggear_update_t1;
    landinggear_update_total += duration;
    landinggear_update_count += 1;
    if (landinggear_update_count % 1000 == 0){
       // printf("average landinggear_update measure time microseconds is %f!\n",landinggear_update_total/1000.0);
        landinggear_update_total = 0;
        landinggear_update_count = 0;
    }
    landinggear_update_logger.log(duration);
}

#endif // LANDING_GEAR_ENABLED
