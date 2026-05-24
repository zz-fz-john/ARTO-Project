#include "Copter.h"
#include <time.h>
#include <chrono>
#include <AP_WCET/AP_WCET.h>
namespace {

unsigned long long read_inertia_total = 0;
unsigned long long read_inertia_count = 0;
uint64_t read_inertia_t1;
uint64_t read_inertia_t2;
TimingLogger read_inertia_logger("read_inertia");

}

// read_inertia - read inertia in from accelerometers
void Copter::read_inertia()
{
    read_inertia_t1 = AP_WCET::now_us();
    // inertial altitude estimates. Use barometer climb rate during high vibrations
    inertial_nav.update(vibration_check.high_vibes);

    // pull position from ahrs
    Location loc;
    ahrs.get_location(loc);
    current_loc.lat = loc.lat;
    current_loc.lng = loc.lng;

    // exit immediately if we do not have an altitude estimate
    if (!inertial_nav.get_filter_status().flags.vert_pos) {
        read_inertia_t2 = AP_WCET::now_us();
        uint64_t duration = read_inertia_t2 - read_inertia_t1;
        read_inertia_total += duration;
        read_inertia_count += 1;
        if (read_inertia_count % 1000 == 0){
           // printf("average read_inertia measure time microseconds is %f!\n", read_inertia_total/1000.0);
            read_inertia_total = 0;
            read_inertia_count = 0;
        }
        read_inertia_logger.log(duration);
        return;
    }

    // current_loc.alt is alt-above-home, converted from inertial nav's alt-above-ekf-origin
    const int32_t alt_above_origin_cm = inertial_nav.get_position_z_up_cm();
    current_loc.set_alt_cm(alt_above_origin_cm, Location::AltFrame::ABOVE_ORIGIN);
    if (!ahrs.home_is_set() || !current_loc.change_alt_frame(Location::AltFrame::ABOVE_HOME)) {
        // if home has not been set yet we treat alt-above-origin as alt-above-home
        current_loc.set_alt_cm(alt_above_origin_cm, Location::AltFrame::ABOVE_HOME);
    }
    read_inertia_t2 = AP_WCET::now_us();
    uint64_t duration = read_inertia_t2 - read_inertia_t1;
    read_inertia_total += duration;
    read_inertia_count += 1;
    if (read_inertia_count % 1000 == 0){
      //  printf("average read_inertia measure time microseconds is %f!\n", read_inertia_total/1000.0);
        read_inertia_total = 0;
        read_inertia_count = 0;
    }
    read_inertia_logger.log(duration);
}
