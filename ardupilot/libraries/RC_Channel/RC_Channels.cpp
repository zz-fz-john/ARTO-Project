/*
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 *       RC_Channels.cpp - class containing an array of RC_Channel objects
 *
 */

#include <stdlib.h>
#include <cmath>

#include <AP_HAL/AP_HAL.h>
extern const AP_HAL::HAL& hal;

#include <AP_Math/AP_Math.h>
#include <AP_Logger/AP_Logger.h>

#include "RC_Channel.h"
#include <time.h>
#include <chrono>
#include <AP_WCET/AP_WCET.h>

namespace {

unsigned long long read_aux_all_total = 0;
unsigned long long read_aux_all_count = 0;
uint64_t read_aux_all_t1;
uint64_t read_aux_all_t2;
TimingLogger read_aux_all_logger("read_aux_all");

unsigned long long read_mode_switch_total = 0;
unsigned long long read_mode_switch_count = 0;
uint64_t read_mode_switch_t1;
uint64_t read_mode_switch_t2;
TimingLogger read_mode_switch_logger("read_mode_switch");
}
/*
  channels group object constructor
 */
RC_Channels::RC_Channels(void)
{
    // set defaults from the parameter table
    AP_Param::setup_object_defaults(this, var_info);

    if (_singleton != nullptr) {
        AP_HAL::panic("RC_Channels must be singleton");
    }
    _singleton = this;
}

void RC_Channels::init(void)
{
    // setup ch_in on channels
    for (uint8_t i=0; i<NUM_RC_CHANNELS; i++) {
        channel(i)->ch_in = i;
    }

    init_aux_all();
}

uint8_t RC_Channels::get_radio_in(uint16_t *chans, const uint8_t num_channels)
{
    memset(chans, 0, num_channels*sizeof(*chans));

    const uint8_t read_channels = MIN(num_channels, NUM_RC_CHANNELS);
    for (uint8_t i = 0; i < read_channels; i++) {
        chans[i] = channel(i)->get_radio_in();
    }

    return read_channels;
}

// update all the input channels
bool RC_Channels::read_input(void)
{
    if (hal.rcin->new_input()) {
        _has_had_rc_receiver = true;
    } else if (!has_new_overrides) {
        return false;
    }

    has_new_overrides = false;

    last_update_ms = AP_HAL::millis();

    bool success = false;
    for (uint8_t i=0; i<NUM_RC_CHANNELS; i++) {
        success |= channel(i)->update();
    }

    return success;
}

uint8_t RC_Channels::get_valid_channel_count(void)
{
    return MIN(NUM_RC_CHANNELS, hal.rcin->num_channels());
}

int16_t RC_Channels::get_receiver_rssi(void)
{
    return hal.rcin->get_rssi();
}
int16_t RC_Channels::get_receiver_link_quality(void)
{
    return hal.rcin->get_rx_link_quality();
}
void RC_Channels::clear_overrides(void)
{
    RC_Channels &_rc = rc();
    for (uint8_t i = 0; i < NUM_RC_CHANNELS; i++) {
        _rc.channel(i)->clear_override();
    }
    // we really should set has_new_overrides to true, and rerun read_input from
    // the vehicle code however doing so currently breaks the failsafe system on
    // copter and plane, RC_Channels needs to control failsafes to resolve this
}

uint16_t RC_Channels::get_override_mask(void)
{
    uint16_t ret = 0;
    RC_Channels &_rc = rc();
    for (uint8_t i = 0; i < NUM_RC_CHANNELS; i++) {
        if (_rc.channel(i)->has_override()) {
            ret |= (1U << i);
        }
    }
    return ret;
}

void RC_Channels::set_override(const uint8_t chan, const int16_t value, const uint32_t timestamp_ms)
{
    RC_Channels &_rc = rc();
    if (chan < NUM_RC_CHANNELS) {
        _rc.channel(chan)->set_override(value, timestamp_ms);
    }
}

bool RC_Channels::has_active_overrides()
{
    RC_Channels &_rc = rc();
    for (uint8_t i = 0; i < NUM_RC_CHANNELS; i++) {
        if (_rc.channel(i)->has_override()) {
            return true;
        }
    }

    return false;
}

bool RC_Channels::receiver_bind(const int dsmMode)
{
    return hal.rcin->rc_bind(dsmMode);
}


// support for auxiliary switches:
// read_aux_switches - checks aux switch positions and invokes configured actions
void RC_Channels::read_aux_all()
{
    read_aux_all_t1= AP_WCET::now_us();
    if (!has_valid_input()) {
        // exit immediately when no RC input
        return;
    }
    bool need_log = false;

    for (uint8_t i=0; i<NUM_RC_CHANNELS; i++) {
        RC_Channel *c = channel(i);
        if (c == nullptr) {
            continue;
        }
        need_log |= c->read_aux();
    }
    if (need_log) {
        // guarantee that we log when a switch changes
        AP::logger().Write_RCIN();
    }
    read_aux_all_t2 = AP_WCET::now_us();
    uint64_t duration = read_aux_all_t2 -read_aux_all_t1;
    read_aux_all_total += duration;
    read_aux_all_count += 1;
    if (read_aux_all_count % 1000 == 0){
        //printf("average read_aux_all measure time microseconds is %f!\n",read_aux_all_total/1000.0);
        read_aux_all_total = 0;
        read_aux_all_count = 0;
    }
    read_aux_all_logger.log(duration);
}

void RC_Channels::init_aux_all()
{
    for (uint8_t i=0; i<NUM_RC_CHANNELS; i++) {
        RC_Channel *c = channel(i);
        if (c == nullptr) {
            continue;
        }
        c->init_aux();
    }
    reset_mode_switch();
}

//
// Support for mode switches
//
RC_Channel *RC_Channels::flight_mode_channel() const
{
    const int8_t num = flight_mode_channel_number();
    if (num <= 0) {
        return nullptr;
    }
    if (num >= NUM_RC_CHANNELS) {
        return nullptr;
    }
    return rc_channel(num-1);
}

void RC_Channels::reset_mode_switch()
{
    RC_Channel *c = flight_mode_channel();
    if (c == nullptr) {
        return;
    }
    c->reset_mode_switch();
}

void RC_Channels::read_mode_switch()
{
    read_mode_switch_t1= AP_WCET::now_us();
    if (!has_valid_input()) {
        // exit immediately when no RC input
        return;
    }
    RC_Channel *c = flight_mode_channel();
    if (c == nullptr) {
        return;
    }
    c->read_mode_switch();
    read_mode_switch_t2 = AP_WCET::now_us();
    uint64_t duration = read_mode_switch_t2 -read_mode_switch_t1;
    read_mode_switch_total += duration;
    read_mode_switch_count += 1;
    if (read_mode_switch_count % 1000 == 0){
      //  printf("average read_mode_switch measure time microseconds is %f!\n",read_mode_switch_total/1000.0);
        read_mode_switch_total = 0;
        read_mode_switch_count = 0;
    }
    read_mode_switch_logger.log(duration);
}

// check if flight mode channel is assigned RC option
// return true if assigned
bool RC_Channels::flight_mode_channel_conflicts_with_rc_option() const
{
    RC_Channel *chan = flight_mode_channel();
    if (chan == nullptr) {
        return false;
    }
    return (RC_Channel::aux_func_t)chan->option.get() != RC_Channel::AUX_FUNC::DO_NOTHING;
}

/*
  get the RC input PWM value given a channel number.  Note that
  channel numbers start at 1, as this API is designed for use in
  LUA
*/
bool RC_Channels::get_pwm(uint8_t c, uint16_t &pwm) const
{
    RC_Channel *chan = rc_channel(c-1);
    if (chan == nullptr) {
        return false;
    }
    int16_t pwm_signed = chan->get_radio_in();
    if (pwm_signed < 0) {
        return false;
    }
    pwm = (uint16_t)pwm_signed;
    return true;
}

// return mask of enabled protocols.
uint32_t RC_Channels::enabled_protocols() const
{
    if (_singleton == nullptr) {
        // for example firmware
        return 1U;
    }
    return uint32_t(_protocols.get());
}

#if AP_SCRIPTING_ENABLED
/*
  implement aux function cache for scripting
 */

/*
  get last aux cached value for scripting. Returns false if never set, otherwise 0,1,2
*/
bool RC_Channels::get_aux_cached(RC_Channel::aux_func_t aux_fn, uint8_t &pos)
{
    const uint16_t aux_idx = uint16_t(aux_fn);
    if (aux_idx >= unsigned(RC_Channel::AUX_FUNC::AUX_FUNCTION_MAX)) {
        return false;
    }
    WITH_SEMAPHORE(aux_cache_sem);
    uint8_t v = aux_cached.get(aux_idx*2) | (aux_cached.get(aux_idx*2+1)<<1);
    if (v == 0) {
        // never been set
        return false;
    }
    pos = v-1;
    return true;
}

/*
  set cached value of an aux function
 */
void RC_Channels::set_aux_cached(RC_Channel::aux_func_t aux_fn, RC_Channel::AuxSwitchPos pos)
{
    const uint16_t aux_idx = uint16_t(aux_fn);
    if (aux_idx < unsigned(RC_Channel::AUX_FUNC::AUX_FUNCTION_MAX)) {
        WITH_SEMAPHORE(aux_cache_sem);
        uint8_t v = unsigned(pos)+1;
        aux_cached.setonoff(aux_idx*2, v&1);
        aux_cached.setonoff(aux_idx*2+1, v>>1);
    }
}
#endif // AP_SCRIPTING_ENABLED

// singleton instance
RC_Channels *RC_Channels::_singleton;


RC_Channels &rc()
{
    return *RC_Channels::get_singleton();
}
