#include "Rover.h"
#include <time.h>
#include <AP_WCET/AP_WCET.h>

namespace {

unsigned long long set_servos_total = 0;
unsigned long long set_servos_count = 0;
uint64_t set_servos_t1;
uint64_t set_servos_t2;
TimingLogger set_servos_logger("set_servos");

}
/*****************************************
    Set the flight control servos based on the current calculated values
*****************************************/
void Rover::set_servos(void)
{
    set_servos_t1 = AP_WCET::now_us();
    // send output signals to motors
    if (motor_test) {
        motor_test_output();
    } else {
        // get ground speed
        float speed;
        if (!g2.attitude_control.get_forward_speed(speed)) {
            speed = 0.0f;
        }

        g2.motors.output(arming.is_armed(), speed, G_Dt);
    }
    set_servos_t2 = AP_WCET::now_us();
    uint64_t duration = set_servos_t2 - set_servos_t1;
    set_servos_total += duration;
    set_servos_count += 1;
    
    set_servos_logger.log(duration);
    if (set_servos_count % 1000 == 0){
        printf("average set_servos measure time microseconds is %f!\n",set_servos_total/1000.0);
        set_servos_total = 0;
        set_servos_count = 0;
    }
}
