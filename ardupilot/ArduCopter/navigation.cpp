#include "Copter.h"
#include <time.h>
#include <chrono>
#include <AP_WCET/AP_WCET.h>
// run_nav_updates - top level call for the autopilot
// ensures calculations such as "distance to waypoint" are calculated before autopilot makes decisions
// To-Do - rename and move this function to make it's purpose more clear
namespace {

unsigned long long run_nav_updates_total = 0;
unsigned long long run_nav_updates_count = 0;
uint64_t run_nav_updates_t1;
uint64_t run_nav_updates_t2;
TimingLogger run_nav_updates_logger("run_nav_updates");


}
void Copter::run_nav_updates(void)
{
    run_nav_updates_t1 = AP_WCET::now_us();
    update_super_simple_bearing(false);
    run_nav_updates_t2 = AP_WCET::now_us();
    uint64_t duration = run_nav_updates_t2 -run_nav_updates_t1;
    run_nav_updates_total += duration;
    run_nav_updates_count += 1;
    if (run_nav_updates_count % 1000 == 0){
        //printf("average run_nav_updates measure time microseconds is %f!\n",run_nav_updates_total/1000.0);
        run_nav_updates_total = 0;
        run_nav_updates_count = 0;
    }
    run_nav_updates_logger.log(duration);
}

// distance between vehicle and home in cm
uint32_t Copter::home_distance()
{
    if (position_ok()) {
        _home_distance = current_loc.get_distance(ahrs.get_home()) * 100;
    }
    return _home_distance;
}

// The location of home in relation to the vehicle in centi-degrees
int32_t Copter::home_bearing()
{
    if (position_ok()) {
        _home_bearing = current_loc.get_bearing_to(ahrs.get_home());
    }
    return _home_bearing;
}
