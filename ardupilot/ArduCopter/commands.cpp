#include "Copter.h"
#include <time.h>
#include <chrono>
#include <AP_WCET/AP_WCET.h>

namespace {

unsigned long long update_home_from_EKF_total = 0;
unsigned long long update_home_from_EKF_count = 0;
uint64_t update_home_from_EKF_t1;
uint64_t update_home_from_EKF_t2;
TimingLogger update_home_from_EKF_logger("update_home_from_EKF");

}
// checks if we should update ahrs/RTL home position from the EKF
void Copter::update_home_from_EKF()
{
    update_home_from_EKF_t1= AP_WCET::now_us();
    // exit immediately if home already set
    if (ahrs.home_is_set()) {
        return;
    }

    // special logic if home is set in-flight
    if (motors->armed()) {
        set_home_to_current_location_inflight();
    } else {
        // move home to current ekf location (this will set home_state to HOME_SET)
        if (!set_home_to_current_location(false)) {
            // ignore failure
        }
    }
    update_home_from_EKF_t2 = AP_WCET::now_us();
    uint64_t duration = update_home_from_EKF_t2 -update_home_from_EKF_t1;
    update_home_from_EKF_total += duration;
    update_home_from_EKF_count += 1;
    if (update_home_from_EKF_count % 1000 == 0){
        //printf("average update_home_from_EKF measure time microseconds is %f!\n",update_home_from_EKF_total/1000.0);
        update_home_from_EKF_total = 0;
        update_home_from_EKF_count = 0;
    }
    update_home_from_EKF_logger.log(duration);
}

// set_home_to_current_location_inflight - set home to current GPS location (horizontally) and EKF origin vertically
void Copter::set_home_to_current_location_inflight() {
    // get current location from EKF
    Location temp_loc;
    Location ekf_origin;
    if (ahrs.get_location(temp_loc) && ahrs.get_origin(ekf_origin)) {
        temp_loc.alt = ekf_origin.alt;
        if (!set_home(temp_loc, false)) {
            return;
        }
        // we have successfully set AHRS home, set it for SmartRTL
#if MODE_SMARTRTL_ENABLED == ENABLED
        g2.smart_rtl.set_home(true);
#endif
    }
}

// set_home_to_current_location - set home to current GPS location
bool Copter::set_home_to_current_location(bool lock) {
    // get current location from EKF
    Location temp_loc;
    if (ahrs.get_location(temp_loc)) {
        if (!set_home(temp_loc, lock)) {
            return false;
        }
        // we have successfully set AHRS home, set it for SmartRTL
#if MODE_SMARTRTL_ENABLED == ENABLED
        g2.smart_rtl.set_home(true);
#endif
        return true;
    }
    return false;
}

// set_home - sets ahrs home (used for RTL) to specified location
//  initialises inertial nav and compass on first call
//  returns true if home location set successfully
bool Copter::set_home(const Location& loc, bool lock)
{
    // check EKF origin has been set
    Location ekf_origin;
    if (!ahrs.get_origin(ekf_origin)) {
        return false;
    }

    // check home is close to EKF origin
    if (far_from_EKF_origin(loc)) {
        return false;
    }

    const bool home_was_set = ahrs.home_is_set();

    // set ahrs home (used for RTL)
    if (!ahrs.set_home(loc)) {
        return false;
    }

    // init inav and compass declination
    if (!home_was_set) {
#if MODE_AUTO_ENABLED == ENABLED
        // log new home position which mission library will pull from ahrs
        if (should_log(MASK_LOG_CMD)) {
            AP_Mission::Mission_Command temp_cmd;
            if (mode_auto.mission.read_cmd_from_storage(0, temp_cmd)) {
                logger.Write_Mission_Cmd(mode_auto.mission, temp_cmd);
            }
        }
#endif
    }

    // lock home position
    if (lock) {
        ahrs.lock_home();
    }

    // return success
    return true;
}

// far_from_EKF_origin - checks if a location is too far from the EKF origin
//  returns true if too far
bool Copter::far_from_EKF_origin(const Location& loc)
{
    // check distance to EKF origin
    Location ekf_origin;
    if (ahrs.get_origin(ekf_origin)) {
        if (labs(ekf_origin.alt - loc.alt)*0.01 > EKF_ORIGIN_MAX_ALT_KM*1000.0) {
            return true;
        }
    }

    // close enough to origin
    return false;
}
