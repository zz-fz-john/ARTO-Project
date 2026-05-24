#include "Rover.h"
#include <time.h>
#include <AP_WCET/AP_WCET.h>
namespace {

unsigned long long fence_check_total = 0;
unsigned long long fence_check_count = 0;
uint64_t fence_check_t1;
uint64_t fence_check_t2;
TimingLogger fence_check_logger("fence_check");

}
// fence_check - ask fence library to check for breaches and initiate the response
void Rover::fence_check()
{
    fence_check_t1 = AP_WCET::now_us();
#if AP_FENCE_ENABLED
    uint8_t new_breaches;  // the type of fence that has been breached
    const uint8_t orig_breaches = fence.get_breaches();

    // check for a breach
    new_breaches = fence.check();

    // return immediately if motors are not armed
    if (!arming.is_armed()) {
        return;
    }

    // if there is a new breach take action
    if (new_breaches) {
        // if the user wants some kind of response and motors are armed
        if (fence.get_action() != Failsafe_Action_None) {
            // if within 100m of the fence, it will take the action specified by the FENCE_ACTION parameter
            if (fence.get_breach_distance(new_breaches) <= AC_FENCE_GIVE_UP_DISTANCE) {
                switch (fence.get_action()) {
                case Failsafe_Action_None:
                    break;
                case Failsafe_Action_RTL:
                    if (!set_mode(mode_rtl, ModeReason::FENCE_BREACHED)) {
                        set_mode(mode_hold, ModeReason::FENCE_BREACHED);
                    }
                    break;
                case Failsafe_Action_Hold:
                    set_mode(mode_hold, ModeReason::FENCE_BREACHED);
                    break;
                case Failsafe_Action_SmartRTL:
                    if (!set_mode(mode_smartrtl, ModeReason::FENCE_BREACHED)) {
                        if (!set_mode(mode_rtl, ModeReason::FENCE_BREACHED)) {
                            set_mode(mode_hold, ModeReason::FENCE_BREACHED);
                        }
                    }
                    break;
                case Failsafe_Action_SmartRTL_Hold:
                    if (!set_mode(mode_smartrtl, ModeReason::FENCE_BREACHED)) {
                        set_mode(mode_hold, ModeReason::FENCE_BREACHED);
                    }
                    break;
                }
            } else {
                // if more than 100m outside the fence just force to HOLD
                set_mode(mode_hold, ModeReason::FENCE_BREACHED);
            }
        }
        AP::logger().Write_Error(LogErrorSubsystem::FAILSAFE_FENCE, LogErrorCode(new_breaches));

    } else if (orig_breaches) {
        // record clearing of breach
        AP::logger().Write_Error(LogErrorSubsystem::FAILSAFE_FENCE,
                                 LogErrorCode::ERROR_RESOLVED);
    }
#endif // AP_FENCE_ENABLED
    fence_check_t2 = AP_WCET::now_us();
    uint64_t duration = fence_check_t2 - fence_check_t1;
    fence_check_total += duration;
    fence_check_count += 1;
    
    fence_check_logger.log(duration);
    if (fence_check_count % 1000 == 0){
        printf("average fence_check measure time microseconds is %f!\n",fence_check_total/1000.0);
        fence_check_total = 0;
        fence_check_count = 0; 
    }
}
