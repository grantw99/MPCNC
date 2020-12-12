/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2020 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "../inc/MarlinConfigPre.h"

#include "tool_change.h"

#include "probe.h"
#include "motion.h"
#include "planner.h"
#include "temperature.h"

#include "../MarlinCore.h"

#define DEBUG_OUT ENABLED(DEBUG_LEVELING_FEATURE)
#include "../core/debug_out.h"

#if EXTRUDERS > 1
  toolchange_settings_t toolchange_settings;  // Initialized by settings.load()
#endif

#if ENABLED(SINGLENOZZLE)
  uint16_t singlenozzle_temp[EXTRUDERS];
  #if FAN_COUNT > 0
    uint8_t singlenozzle_fan_speed[EXTRUDERS];
  #endif
#endif

#if ENABLED(MAGNETIC_PARKING_EXTRUDER) || defined(EVENT_GCODE_AFTER_TOOLCHANGE) || (ENABLED(PARKING_EXTRUDER) && PARKING_EXTRUDER_SOLENOIDS_DELAY > 0)
  #include "../gcode/gcode.h"
#endif

#if ANY(SWITCHING_EXTRUDER, SWITCHING_NOZZLE, SWITCHING_TOOLHEAD)
  #include "servo.h"
#endif

#if ENABLED(EXT_SOLENOID) && DISABLED(PARKING_EXTRUDER)
  #include "../feature/solenoid.h"
#endif

#if ENABLED(MK2_MULTIPLEXER)
  #include "../feature/snmm.h"
#endif

#if ENABLED(MIXING_EXTRUDER)
  #include "../feature/mixing.h"
#endif

#if HAS_LEVELING
  #include "../feature/bedlevel/bedlevel.h"
#endif

#if HAS_FANMUX
  #include "../feature/fanmux.h"
#endif

#if ENABLED(PRUSA_MMU2)
  #include "../feature/mmu2/mmu2.h"
#endif

#if HAS_LCD_MENU
  #include "../lcd/ultralcd.h"
#endif

#if ENABLED(ADVANCED_PAUSE_FEATURE)
  #include "../feature/pause.h"
#endif

#if DO_SWITCH_EXTRUDER

  #if EXTRUDERS > 3
    #define _SERVO_NR(E) ((E) < 2 ? SWITCHING_EXTRUDER_SERVO_NR : SWITCHING_EXTRUDER_E23_SERVO_NR)
  #else
    #define _SERVO_NR(E) SWITCHING_EXTRUDER_SERVO_NR
  #endif

  void move_extruder_servo(const uint8_t e) {
    planner.synchronize();
    #if EXTRUDERS & 1
      if (e < EXTRUDERS - 1)
    #endif
    {
      MOVE_SERVO(_SERVO_NR(e), servo_angles[_SERVO_NR(e)][e]);
      safe_delay(500);
    }
  }

#endif // DO_SWITCH_EXTRUDER

#if ENABLED(SWITCHING_NOZZLE)

  #if SWITCHING_NOZZLE_TWO_SERVOS

    inline void _move_nozzle_servo(const uint8_t e, const uint8_t angle_index) {
      constexpr int8_t  sns_index[2] = { SWITCHING_NOZZLE_SERVO_NR, SWITCHING_NOZZLE_E1_SERVO_NR };
      constexpr int16_t sns_angles[2] = SWITCHING_NOZZLE_SERVO_ANGLES;
      planner.synchronize();
      MOVE_SERVO(sns_index[e], sns_angles[angle_index]);
      safe_delay(500);
    }

    void lower_nozzle(const uint8_t e) { _move_nozzle_servo(e, 0); }
    void raise_nozzle(const uint8_t e) { _move_nozzle_servo(e, 1); }

  #else

    void move_nozzle_servo(const uint8_t angle_index) {
      planner.synchronize();
      MOVE_SERVO(SWITCHING_NOZZLE_SERVO_NR, servo_angles[SWITCHING_NOZZLE_SERVO_NR][angle_index]);
      safe_delay(500);
    }

  #endif

#endif // SWITCHING_NOZZLE

inline void _line_to_current(const AxisEnum fr_axis, const float fscale=1) {
  line_to_current_position(planner.settings.max_feedrate_mm_s[fr_axis] * fscale);
}
inline void slow_line_to_current(const AxisEnum fr_axis) { _line_to_current(fr_axis, 0.5f); }
inline void fast_line_to_current(const AxisEnum fr_axis) { _line_to_current(fr_axis); }

#if ENABLED(MAGNETIC_PARKING_EXTRUDER)

  float parkingposx[2],           // M951 R L
        parkinggrabdistance,      // M951 I
        parkingslowspeed,         // M951 J
        parkinghighspeed,         // M951 H
        parkingtraveldistance,    // M951 D
        compensationmultiplier;

  inline void magnetic_parking_extruder_tool_change(const uint8_t new_tool) {

    const float oldx = current_position.x,
                grabpos = mpe_settings.parking_xpos[new_tool] + (new_tool ? mpe_settings.grab_distance : -mpe_settings.grab_distance),
                offsetcompensation = (0
                  #if HAS_HOTEND_OFFSET
                    + hotend_offset[active_extruder].x * mpe_settings.compensation_factor
                  #endif
                );

    if (axis_unhomed_error(_BV(X_AXIS))) return;

    /**
     * Z Lift and Nozzle Offset shift ar defined in caller method to work equal with any Multi Hotend realization
     *
     * Steps:
     *   1. Move high speed to park position of new extruder
     *   2. Move to couple position of new extruder (this also discouple the old extruder)
     *   3. Move to park position of new extruder
     *   4. Move high speed to approach park position of old extruder
     *   5. Move to park position of old extruder
     *   6. Move to starting position
     */

    // STEP 1

    current_position.x = mpe_settings.parking_xpos[new_tool] + offsetcompensation;

    if (DEBUGGING(LEVELING)) {
      DEBUG_ECHOPAIR("(1) Move extruder ", int(new_tool));
      DEBUG_POS(" to new extruder ParkPos", current_position);
    }

    planner.buffer_line(current_position, mpe_settings.fast_feedrate, new_tool);
    planner.synchronize();

    // STEP 2

    current_position.x = grabpos + offsetcompensation;

    if (DEBUGGING(LEVELING)) {
      DEBUG_ECHOPAIR("(2) Couple extruder ", int(new_tool));
      DEBUG_POS(" to new extruder GrabPos", current_position);
    }

    planner.buffer_line(current_position, mpe_settings.slow_feedrate, new_tool);
    planner.synchronize();

    // Delay before moving tool, to allow magnetic coupling
    gcode.dwell(150);

    // STEP 3

    current_position.x = mpe_settings.parking_xpos[new_tool] + offsetcompensation;
    if (DEBUGGING(LEVELING)) {
      DEBUG_ECHOPAIR("(3) Move extruder ", int(new_tool));
      DEBUG_POS(" back to new extruder ParkPos", current_position);
    }

    planner.buffer_line(current_position, mpe_settings.slow_feedrate, new_tool);
    planner.synchronize();

    // STEP 4

    current_position.x = mpe_settings.parking_xpos[active_extruder] + (active_extruder == 0 ? MPE_TRAVEL_DISTANCE : -MPE_TRAVEL_DISTANCE) + offsetcompensation;
    if (DEBUGGING(LEVELING)) {
      DEBUG_ECHOPAIR("(4) Move extruder ", int(new_tool));
      DEBUG_POS(" close to old extruder ParkPos", current_position);
    }

    planner.buffer_line(current_position, mpe_settings.fast_feedrate, new_tool);
    planner.synchronize();

    // STEP 5

    current_position.x = mpe_settings.parking_xpos[active_extruder] + offsetcompensation;

    if (DEBUGGING(LEVELING)) {
      DEBUG_ECHOPAIR("(5) Park extruder ", int(new_tool));
      DEBUG_POS(" at old extruder ParkPos", current_position);
    }

    planner.buffer_line(current_position, mpe_settings.slow_feedrate, new_tool);
    planner.synchronize();

    // STEP 6

    current_position.x = oldx;

    if (DEBUGGING(LEVELING)) {
      DEBUG_ECHOPAIR("(6) Move extruder ", int(new_tool));
      DEBUG_POS(" to starting position", current_position);
    }

    planner.buffer_line(current_position, mpe_settings.fast_feedrate, new_tool);
    planner.synchronize();

    if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("Autopark done.");
  }

#elif ENABLED(PARKING_EXTRUDER)

  void pe_solenoid_init() {
    LOOP_LE_N(n, 1)
      #if ENABLED(PARKING_EXTRUDER_SOLENOIDS_INVERT)
        pe_activate_solenoid(n);
      #else
        pe_deactivate_solenoid(n);
      #endif
  }

  void pe_set_solenoid(const uint8_t extruder_num, const uint8_t state) {
    switch (extruder_num) {
      case 1: OUT_WRITE(SOL1_PIN, state); break;
      default: OUT_WRITE(SOL0_PIN, state); break;
    }
    #if PARKING_EXTRUDER_SOLENOIDS_DELAY > 0
      gcode.dwell(PARKING_EXTRUDER_SOLENOIDS_DELAY);
    #endif
  }

  inline void parking_extruder_tool_change(const uint8_t new_tool, bool no_move) {
    if (!no_move) {

      constexpr float parkingposx[] = PARKING_EXTRUDER_PARKING_X;

      #if HAS_HOTEND_OFFSET
        const float x_offset = hotend_offset[active_extruder].x;
      #else
        constexpr float x_offset = 0;
      #endif

      const float midpos = (parkingposx[0] + parkingposx[1]) * 0.5f + x_offset,
                  grabpos = parkingposx[new_tool] + (new_tool ? PARKING_EXTRUDER_GRAB_DISTANCE : -(PARKING_EXTRUDER_GRAB_DISTANCE)) + x_offset;

      /**
       * 1. Move to park position of old extruder
       * 2. Disengage magnetic field, wait for delay
       * 3. Move near new extruder
       * 4. Engage magnetic field for new extruder
       * 5. Move to parking incl. offset of new extruder
       * 6. Lower Z-Axis
       */

      // STEP 1

      if (DEBUGGING(LEVELING)) DEBUG_POS("Start PE Tool-Change", current_position);

      current_position.x = parkingposx[active_extruder] + x_offset;
      if (DEBUGGING(LEVELING)) {
        DEBUG_ECHOLNPAIR("(1) Park extruder ", int(active_extruder));
        DEBUG_POS("Moving ParkPos", current_position);
      }
      fast_line_to_current(X_AXIS);

      // STEP 2

      planner.synchronize();
      if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("(2) Disengage magnet");
      pe_deactivate_solenoid(active_extruder);

      // STEP 3

      current_position.x += active_extruder ? -10 : 10; // move 10mm away from parked extruder
      if (DEBUGGING(LEVELING)) {
        DEBUG_ECHOLNPGM("(3) Move near new extruder");
        DEBUG_POS("Move away from parked extruder", current_position);
      }
      fast_line_to_current(X_AXIS);

      // STEP 4

      planner.synchronize();
      if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("(4) Engage magnetic field");
      #if ENABLED(PARKING_EXTRUDER_SOLENOIDS_INVERT)
        pe_activate_solenoid(active_extruder); // Just save power for inverted magnets
      #endif
      pe_activate_solenoid(new_tool);

      // STEP 5

      current_position.x = grabpos + (new_tool ? -10 : 10);
      fast_line_to_current(X_AXIS);

      current_position.x = grabpos;
      if (DEBUGGING(LEVELING)) {
        planner.synchronize();
        DEBUG_POS("(5) Unpark extruder", current_position);
      }
      slow_line_to_current(X_AXIS);

      // STEP 6

      current_position.x = midpos
        #if HAS_HOTEND_OFFSET
          - hotend_offset[new_tool].x
        #endif
      ;
      if (DEBUGGING(LEVELING)) {
        planner.synchronize();
        DEBUG_POS("(6) Move midway between hotends", current_position);
      }
      fast_line_to_current(X_AXIS);
      planner.synchronize(); // Always sync the final move

      if (DEBUGGING(LEVELING)) DEBUG_POS("PE Tool-Change done.", current_position);
    }
    else { // nomove == true
      // Only engage magnetic field for new extruder
      pe_activate_solenoid(new_tool);
      #if ENABLED(PARKING_EXTRUDER_SOLENOIDS_INVERT)
        pe_activate_solenoid(active_extruder); // Just save power for inverted magnets
      #endif
    }
  }

#endif // PARKING_EXTRUDER

#if ENABLED(SWITCHING_TOOLHEAD)

  inline void swt_lock(const bool locked=true) {
    const uint16_t swt_angles[2] = SWITCHING_TOOLHEAD_SERVO_ANGLES;
    MOVE_SERVO(SWITCHING_TOOLHEAD_SERVO_NR, swt_angles[locked ? 0 : 1]);
  }

  void swt_init() { swt_lock(); }

  inline void switching_toolhead_tool_change(const uint8_t new_tool, bool no_move/*=false*/) {
    if (no_move) return;

    constexpr float toolheadposx[] = SWITCHING_TOOLHEAD_X_POS;
    const float placexpos = toolheadposx[active_extruder],
                grabxpos = toolheadposx[new_tool];

    /**
     * 1. Move to switch position of current toolhead
     * 2. Unlock tool and drop it in the dock
     * 3. Move to the new toolhead
     * 4. Grab and lock the new toolhead
     */

    // 1. Move to switch position of current toolhead

    if (DEBUGGING(LEVELING)) DEBUG_POS("Start ST Tool-Change", current_position);

    current_position.x = placexpos;
    if (DEBUGGING(LEVELING)) {
      DEBUG_ECHOLNPAIR("(1) Place old tool ", int(active_extruder));
      DEBUG_POS("Move X SwitchPos", current_position);
    }
    fast_line_to_current(X_AXIS);

    current_position.y = SWITCHING_TOOLHEAD_Y_POS - (SWITCHING_TOOLHEAD_Y_SECURITY);
    if (DEBUGGING(LEVELING)) {
      planner.synchronize();
      DEBUG_POS("Move Y SwitchPos + Security", current_position);
    }
    fast_line_to_current(Y_AXIS);

    // 2. Unlock tool and drop it in the dock

    planner.synchronize();
    if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("(2) Unlock and Place Toolhead");
    swt_lock(false);
    safe_delay(500);

    current_position.y = SWITCHING_TOOLHEAD_Y_POS;
    if (DEBUGGING(LEVELING)) DEBUG_POS("Move Y SwitchPos", current_position);
    slow_line_to_current(Y_AXIS);

    // Wait for move to complete, then another 0.2s
    planner.synchronize();
    safe_delay(200);

    current_position.y -= SWITCHING_TOOLHEAD_Y_CLEAR;
    if (DEBUGGING(LEVELING)) DEBUG_POS("Move back Y clear", current_position);
    fast_line_to_current(Y_AXIS); // move away from docked toolhead

    // 3. Move to the new toolhead

    current_position.x = grabxpos;
    if (DEBUGGING(LEVELING)) {
      planner.synchronize();
      DEBUG_ECHOLNPGM("(3) Move to new toolhead position");
      DEBUG_POS("Move to new toolhead X", current_position);
    }
    fast_line_to_current(X_AXIS);

    current_position.y = SWITCHING_TOOLHEAD_Y_POS - (SWITCHING_TOOLHEAD_Y_SECURITY);
    if (DEBUGGING(LEVELING)) {
      planner.synchronize();
      DEBUG_POS("Move Y SwitchPos + Security", current_position);
    }
    fast_line_to_current(Y_AXIS);

    // 4. Grab and lock the new toolhead

    current_position.y = SWITCHING_TOOLHEAD_Y_POS;
    if (DEBUGGING(LEVELING)) {
      planner.synchronize();
      DEBUG_ECHOLNPGM("(4) Grab and lock new toolhead");
      DEBUG_POS("Move Y SwitchPos", current_position);
    }
    slow_line_to_current(Y_AXIS);

    // Wait for move to finish, pause 0.2s, move servo, pause 0.5s
    planner.synchronize();
    safe_delay(200);
    swt_lock();
    safe_delay(500);

    current_position.y -= SWITCHING_TOOLHEAD_Y_CLEAR;
    if (DEBUGGING(LEVELING)) DEBUG_POS("Move back Y clear", current_position);
    fast_line_to_current(Y_AXIS); // Move away from docked toolhead
    planner.synchronize();        // Always sync the final move

    if (DEBUGGING(LEVELING)) DEBUG_POS("ST Tool-Change done.", current_position);
  }

#elif ENABLED(MAGNETIC_SWITCHING_TOOLHEAD)

  inline void magnetic_switching_toolhead_tool_change(const uint8_t new_tool, bool no_move/*=false*/) {
    if (no_move) return;

    constexpr float toolheadposx[] = SWITCHING_TOOLHEAD_X_POS,
                    toolheadclearx[] = SWITCHING_TOOLHEAD_X_SECURITY;

    const float placexpos = toolheadposx[active_extruder],
                placexclear = toolheadclearx[active_extruder],
                grabxpos = toolheadposx[new_tool],
                grabxclear = toolheadclearx[new_tool];

    /**
     * 1. Move to switch position of current toolhead
     * 2. Release and place toolhead in the dock
     * 3. Move to the new toolhead
     * 4. Grab the new toolhead and move to security position
     */

    if (DEBUGGING(LEVELING)) DEBUG_POS("Start MST Tool-Change", current_position);

    // 1. Move to switch position current toolhead

    current_position.y = SWITCHING_TOOLHEAD_Y_POS + SWITCHING_TOOLHEAD_Y_CLEAR;
    if (DEBUGGING(LEVELING)) {
      SERIAL_ECHOLNPAIR("(1) Place old tool ", int(active_extruder));
      DEBUG_POS("Move Y SwitchPos + Security", current_position);
    }
    fast_line_to_current(Y_AXIS);

    current_position.x = placexclear;
    if (DEBUGGING(LEVELING)) {
      planner.synchronize();
      DEBUG_POS("Move X SwitchPos + Security", current_position);
    }
    fast_line_to_current(X_AXIS);

    current_position.y = SWITCHING_TOOLHEAD_Y_POS;
    if (DEBUGGING(LEVELING)) {
      planner.synchronize();
      DEBUG_POS("Move Y SwitchPos", current_position);
    }
    fast_line_to_current(Y_AXIS);

    current_position.x = placexpos;
    if (DEBUGGING(LEVELING)) {
      planner.synchronize();
      DEBUG_POS("Move X SwitchPos", current_position);
    }
    line_to_current_position(planner.settings.max_feedrate_mm_s[X_AXIS] * 0.25f);

    // 2. Release and place toolhead in the dock

    if (DEBUGGING(LEVELING)) {
      planner.synchronize();
      DEBUG_ECHOLNPGM("(2) Release and Place Toolhead");
    }

    current_position.y = SWITCHING_TOOLHEAD_Y_POS + SWITCHING_TOOLHEAD_Y_RELEASE;
    if (DEBUGGING(LEVELING)) DEBUG_POS("Move Y SwitchPos + Release", current_position);
    line_to_current_position(planner.settings.max_feedrate_mm_s[Y_AXIS] * 0.1f);

    current_position.y = SWITCHING_TOOLHEAD_Y_POS + SWITCHING_TOOLHEAD_Y_SECURITY;
    if (DEBUGGING(LEVELING)) {
      planner.synchronize();
      DEBUG_POS("Move Y SwitchPos + Security", current_position);
    }
    line_to_current_position(planner.settings.max_feedrate_mm_s[Y_AXIS]);

    // 3. Move to new toolhead position

    if (DEBUGGING(LEVELING)) {
      planner.synchronize();
      DEBUG_ECHOLNPGM("(3) Move to new toolhead position");
    }

    current_position.x = grabxpos;
    if (DEBUGGING(LEVELING)) DEBUG_POS("Move to new toolhead X", current_position);
    fast_line_to_current(X_AXIS);

    // 4. Grab the new toolhead and move to security position

    if (DEBUGGING(LEVELING)) {
      planner.synchronize();
      DEBUG_ECHOLNPGM("(4) Grab new toolhead, move to security position");
    }

    current_position.y = SWITCHING_TOOLHEAD_Y_POS + SWITCHING_TOOLHEAD_Y_RELEASE;
    if (DEBUGGING(LEVELING)) DEBUG_POS("Move Y SwitchPos + Release", current_position);
    line_to_current_position(planner.settings.max_feedrate_mm_s[Y_AXIS]);

    current_position.y = SWITCHING_TOOLHEAD_Y_POS;
    if (DEBUGGING(LEVELING)) {
      planner.synchronize();
      DEBUG_POS("Move Y SwitchPos", current_position);
    }
    _line_to_current(Y_AXIS, 0.2f);

    #if ENABLED(PRIME_BEFORE_REMOVE) && (SWITCHING_TOOLHEAD_PRIME_MM || SWITCHING_TOOLHEAD_RETRACT_MM)
      #if SWITCHING_TOOLHEAD_PRIME_MM
        current_position.e += SWITCHING_TOOLHEAD_PRIME_MM;
        planner.buffer_line(current_position, MMM_TO_MMS(SWITCHING_TOOLHEAD_PRIME_FEEDRATE), new_tool);
      #endif
      #if SWITCHING_TOOLHEAD_RETRACT_MM
        current_position.e -= SWITCHING_TOOLHEAD_RETRACT_MM;
        planner.buffer_line(current_position, MMM_TO_MMS(SWITCHING_TOOLHEAD_RETRACT_FEEDRATE), new_tool);
      #endif
    #else
      planner.synchronize();
      safe_delay(100); // Give switch time to settle
    #endif

    current_position.x = grabxclear;
    if (DEBUGGING(LEVELING)) DEBUG_POS("Move to new toolhead X + Security", current_position);
    _line_to_current(X_AXIS, 0.1f);
    planner.synchronize();
    safe_delay(100); // Give switch time to settle

    current_position.y += SWITCHING_TOOLHEAD_Y_CLEAR;
    if (DEBUGGING(LEVELING)) DEBUG_POS("Move back Y clear", current_position);
    fast_line_to_current(Y_AXIS); // move away from docked toolhead
    planner.synchronize(); // Always sync last tool-change move

    if (DEBUGGING(LEVELING)) DEBUG_POS("MST Tool-Change done.", current_position);
  }

#elif ENABLED(ELECTROMAGNETIC_SWITCHING_TOOLHEAD)

  inline void est_activate_solenoid()   { OUT_WRITE(SOL0_PIN, HIGH); }
  inline void est_deactivate_solenoid() { OUT_WRITE(SOL0_PIN, LOW); }
  void est_init() { est_activate_solenoid(); }

  inline void em_switching_toolhead_tool_change(const uint8_t new_tool, bool no_move) {
    if (no_move) return;

    constexpr float toolheadposx[] = SWITCHING_TOOLHEAD_X_POS;
    const float placexpos = toolheadposx[active_extruder],
                grabxpos = toolheadposx[new_tool];
    const xyz_pos_t &hoffs = hotend_offset[active_extruder];

    /**
     * 1. Raise Z-Axis to give enough clearance
     * 2. Move to position near active extruder parking
     * 3. Move gently to park position of active extruder
     * 4. Disengage magnetic field, wait for delay
     * 5. Leave extruder and move to position near new extruder parking
     * 6. Move gently to park position of new extruder
     * 7. Engage magnetic field for new extruder parking
     * 8. Unpark extruder
     * 9. Apply Z hotend offset to current position
     */

    if (DEBUGGING(LEVELING)) DEBUG_POS("Start EMST Tool-Change", current_position);

    // 1. Raise Z-Axis to give enough clearance

    current_position.z += SWITCHING_TOOLHEAD_Z_HOP;
    if (DEBUGGING(LEVELING)) DEBUG_POS("(1) Raise Z-Axis ", current_position);
    fast_line_to_current(Z_AXIS);

    // 2. Move to position near active extruder parking

    if (DEBUGGING(LEVELING)) {
      planner.synchronize();
      SERIAL_ECHOLNPAIR("(2) Move near active extruder parking", active_extruder);
      DEBUG_POS("Moving ParkPos", current_position);
    }
    current_position.set(hoffs.x + placexpos,
                         hoffs.y + SWITCHING_TOOLHEAD_Y_POS + SWITCHING_TOOLHEAD_Y_CLEAR);
    fast_line_to_current(X_AXIS);

    // 3. Move gently to park position of active extruder

    if (DEBUGGING(LEVELING)) {
      planner.synchronize();
      SERIAL_ECHOLNPAIR("(3) Move gently to park position of active extruder", active_extruder);
      DEBUG_POS("Moving ParkPos", current_position);
    }

    current_position.y -= SWITCHING_TOOLHEAD_Y_CLEAR;
    slow_line_to_current(Y_AXIS);

    // 4. Disengage magnetic field, wait for delay

    planner.synchronize();
    if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("(4) Disengage magnet");
    est_deactivate_solenoid();

    // 5. Leave extruder and move to position near new extruder parking

    if (DEBUGGING(LEVELING)) {
      DEBUG_ECHOLNPGM("(5) Move near new extruder parking");
      DEBUG_POS("Moving ParkPos", current_position);
    }

    current_position.y += SWITCHING_TOOLHEAD_Y_CLEAR;
    slow_line_to_current(Y_AXIS);
    current_position.set(hoffs.x + grabxpos,
                         hoffs.y + SWITCHING_TOOLHEAD_Y_POS + SWITCHING_TOOLHEAD_Y_CLEAR);
    fast_line_to_current(X_AXIS);

    // 6. Move gently to park position of new extruder

    current_position.y -= SWITCHING_TOOLHEAD_Y_CLEAR;
    if (DEBUGGING(LEVELING)) {
      planner.synchronize();
      DEBUG_ECHOLNPGM("(6) Move near new extruder");
    }
    slow_line_to_current(Y_AXIS);

    // 7. Engage magnetic field for new extruder parking

    planner.synchronize();
    if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("(7) Engage magnetic field");
    est_activate_solenoid();

    // 8. Unpark extruder

    current_position.y += SWITCHING_TOOLHEAD_Y_CLEAR;
    if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("(8) Unpark extruder");
    slow_line_to_current(X_AXIS);
    planner.synchronize(); // Always sync the final move

    // 9. Apply Z hotend offset to current position

    if (DEBUGGING(LEVELING)) DEBUG_POS("(9) Applying Z-offset", current_position);
    current_position.z += hoffs.z - hotend_offset[new_tool].z;

    if (DEBUGGING(LEVELING)) DEBUG_POS("EMST Tool-Change done.", current_position);
  }

#endif // ELECTROMAGNETIC_SWITCHING_TOOLHEAD

#if EXTRUDERS
  inline void invalid_extruder_error(const uint8_t e) {
    SERIAL_ECHO_START();
    SERIAL_CHAR('T'); SERIAL_ECHO(int(e));
    SERIAL_CHAR(' '); SERIAL_ECHOLNPGM(STR_INVALID_EXTRUDER);
  }
#endif

#if ENABLED(DUAL_X_CARRIAGE)

  inline void dualx_tool_change(const uint8_t new_tool, bool &no_move) {
    if (DEBUGGING(LEVELING)) {
      DEBUG_ECHOPGM("Dual X Carriage Mode ");
      switch (dual_x_carriage_mode) {
        case DXC_FULL_CONTROL_MODE: DEBUG_ECHOLNPGM("FULL_CONTROL"); break;
        case DXC_AUTO_PARK_MODE:    DEBUG_ECHOLNPGM("AUTO_PARK");    break;
        case DXC_DUPLICATION_MODE:  DEBUG_ECHOLNPGM("DUPLICATION");  break;
        case DXC_MIRRORED_MODE:     DEBUG_ECHOLNPGM("MIRRORED");     break;
      }
    }

    const float xhome = x_home_pos(active_extruder);
    if (dual_x_carriage_mode == DXC_AUTO_PARK_MODE
        && IsRunning() && !no_move
        && (delayed_move_time || current_position.x != xhome)
    ) {

      if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPAIR("MoveX to ", xhome);

      // Park old head
      current_position.x = xhome;
      line_to_current_position(planner.settings.max_feedrate_mm_s[X_AXIS]);
      planner.synchronize();
    }

    // Activate the new extruder ahead of calling set_axis_is_at_home!
    active_extruder = new_tool;

    // This function resets the max/min values - the current position may be overwritten below.
    set_axis_is_at_home(X_AXIS);

    if (DEBUGGING(LEVELING)) DEBUG_POS("New Extruder", current_position);

    switch (dual_x_carriage_mode) {
      case DXC_FULL_CONTROL_MODE:
        // New current position is the position of the activated extruder
        current_position.x = inactive_extruder_x_pos;
        // Save the inactive extruder's position (from the old current_position)
        inactive_extruder_x_pos = destination.x;
        break;
      case DXC_AUTO_PARK_MODE:
        // record current raised toolhead position for use by unpark
        raised_parked_position = current_position;
        active_extruder_parked = true;
        delayed_move_time = 0;
        break;
      default:
        break;
    }

    if (DEBUGGING(LEVELING)) {
      DEBUG_ECHOLNPAIR("Active extruder parked: ", active_extruder_parked ? "yes" : "no");
      DEBUG_POS("New extruder (parked)", current_position);
    }
  }

#endif // DUAL_X_CARRIAGE

/**
 * Perform a tool-change, which may result in moving the
 * previous tool out of the way and the new tool into place.
 */
void tool_change(const uint8_t new_tool, bool no_move/*=false*/) {

  #if ENABLED(MAGNETIC_SWITCHING_TOOLHEAD)
    if (new_tool == active_extruder) return;
  #endif

  #if ENABLED(MIXING_EXTRUDER)

    UNUSED(no_move);

    if (new_tool >= MIXING_VIRTUAL_TOOLS)
      return invalid_extruder_error(new_tool);

    #if MIXING_VIRTUAL_TOOLS > 1
      // T0-Tnnn: Switch virtual tool by changing the index to the mix
      mixer.T(new_tool);
    #endif

  #elif ENABLED(PRUSA_MMU2)

    UNUSED(no_move);

    mmu2.tool_change(new_tool);

  #elif EXTRUDERS == 0

    // Nothing to do
    UNUSED(new_tool); UNUSED(no_move);

  #elif EXTRUDERS < 2

    UNUSED(no_move);

    if (new_tool) invalid_extruder_error(new_tool);
    return;

  #else // EXTRUDERS > 1

    planner.synchronize();

    #if ENABLED(DUAL_X_CARRIAGE)  // Only T0 allowed if the Printer is in DXC_DUPLICATION_MODE or DXC_MIRRORED_MODE
      if (new_tool != 0 && dxc_is_duplicating())
         return invalid_extruder_error(new_tool);
    #endif

    if (new_tool >= EXTRUDERS)
      return invalid_extruder_error(new_tool);

    if (!no_move && !homing_needed()) {
      no_move = true;
      if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("No move (not homed)");
    }

    #if HAS_LCD_MENU
      if (!no_move) ui.return_to_status();
    #endif

    #if ENABLED(DUAL_X_CARRIAGE)
      const bool idex_full_control = dual_x_carriage_mode == DXC_FULL_CONTROL_MODE;
    #else
      constexpr bool idex_full_control = false;
    #endif

    const uint8_t old_tool = active_extruder;
    const bool can_move_away = !no_move && !idex_full_control;

    #if ENABLED(TOOLCHANGE_FILAMENT_SWAP)
      const bool should_swap = can_move_away && toolchange_settings.swap_length;
      #if ENABLED(PREVENT_COLD_EXTRUSION)
        const bool too_cold = !DEBUGGING(DRYRUN) && (thermalManager.targetTooColdToExtrude(old_tool) || thermalManager.targetTooColdToExtrude(new_tool));
      #else
        constexpr bool too_cold = false;
      #endif
      if (should_swap) {
        if (too_cold) {
          SERIAL_ECHO_MSG(STR_ERR_HOTEND_TOO_COLD);
          #if ENABLED(SINGLENOZZLE)
            active_extruder = new_tool;
            return;
          #endif
        }
        else {
          #if ENABLED(ADVANCED_PAUSE_FEATURE)
            unscaled_e_move(-toolchange_settings.swap_length, MMM_TO_MMS(toolchange_settings.retract_speed));
          #else
            current_position.e -= toolchange_settings.swap_length / planner.e_factor[old_tool];
            planner.buffer_line(current_position, MMM_TO_MMS(toolchange_settings.retract_speed), old_tool);
            planner.synchronize();
          #endif
        }
      }
    #endif // TOOLCHANGE_FILAMENT_SWAP

    #if HAS_LEVELING && DISABLED(SINGLENOZZLE)
      // Set current position to the physical position
      TEMPORARY_BED_LEVELING_STATE(false);
    #endif

    if (new_tool != old_tool) {

      #if SWITCHING_NOZZLE_TWO_SERVOS
        raise_nozzle(old_tool);
      #endif

      REMEMBER(fr, feedrate_mm_s, XY_PROBE_FEEDRATE_MM_S);

      #if HAS_SOFTWARE_ENDSTOPS
        #if HAS_HOTEND_OFFSET
          #define _EXT_ARGS , old_tool, new_tool
        #else
          #define _EXT_ARGS
        #endif
        update_software_endstops(X_AXIS _EXT_ARGS);
        #if DISABLED(DUAL_X_CARRIAGE)
          update_software_endstops(Y_AXIS _EXT_ARGS);
          update_software_endstops(Z_AXIS _EXT_ARGS);
        #endif
      #endif

      destination = current_position;

      #if DISABLED(SWITCHING_NOZZLE)
        if (can_move_away) {
          // Do a small lift to avoid the workpiece in the move back (below)
          current_position.z += toolchange_settings.z_raise;
          #if HAS_SOFTWARE_ENDSTOPS
            NOMORE(current_position.z, soft_endstop.max.z);
          #endif
          fast_line_to_current(Z_AXIS);
          #if ENABLED(TOOLCHANGE_PARK)
            current_position = toolchange_settings.change_point;
          #endif
          planner.buffer_line(current_position, feedrate_mm_s, old_tool);
          planner.synchronize();
        }
      #endif

      #if HAS_HOTEND_OFFSET
        xyz_pos_t diff = hotend_offset[new_tool] - hotend_offset[old_tool];
        #if ENABLED(DUAL_X_CARRIAGE)
          diff.x = 0;
        #endif
      #else
        constexpr xyz_pos_t diff{0};
      #endif

      #if ENABLED(DUAL_X_CARRIAGE)
        dualx_tool_change(new_tool, no_move);
      #elif ENABLED(PARKING_EXTRUDER)                                   // Dual Parking extruder
        parking_extruder_tool_change(new_tool, no_move);
      #elif ENABLED(MAGNETIC_PARKING_EXTRUDER)                          // Magnetic Parking extruder
        magnetic_parking_extruder_tool_change(new_tool);
      #elif ENABLED(SWITCHING_TOOLHEAD)                                 // Switching Toolhead
        switching_toolhead_tool_change(new_tool, no_move);
      #elif ENABLED(MAGNETIC_SWITCHING_TOOLHEAD)                        // Magnetic Switching Toolhead
        magnetic_switching_toolhead_tool_change(new_tool, no_move);
      #elif ENABLED(ELECTROMAGNETIC_SWITCHING_TOOLHEAD)                 // Magnetic Switching ToolChanger
        em_switching_toolhead_tool_change(new_tool, no_move);
      #elif ENABLED(SWITCHING_NOZZLE) && !SWITCHING_NOZZLE_TWO_SERVOS   // Switching Nozzle (single servo)
        // Raise by a configured distance to avoid workpiece, except with
        // SWITCHING_NOZZLE_TWO_SERVOS, as both nozzles will lift instead.
        if (!no_move) {
          #if HAS_SOFTWARE_ENDSTOPS
            const float maxz = _MIN(soft_endstop.max.z, Z_MAX_POS);
          #else
            constexpr float maxz = Z_MAX_POS;
          #endif

          // Check if Z has space to compensate at least z_offset, and if not, just abort now
          const float newz = current_position.z + _MAX(-diff.z, 0.0);
          if (newz > maxz) return;

          current_position.z = _MIN(newz + toolchange_settings.z_raise, maxz);
          fast_line_to_current(Z_AXIS);
        }
        move_nozzle_servo(new_tool);
      #endif

      #if DISABLED(DUAL_X_CARRIAGE)
        active_extruder = new_tool; // Set the new active extruder
      #endif

      // The newly-selected extruder XYZ is actually at...
      if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPAIR("Offset Tool XYZ by { ", diff.x, ", ", diff.y, ", ", diff.z, " }");
      current_position += diff;

      // Tell the planner the new "current position"
      sync_plan_position();

      #if ENABLED(DELTA)
        //LOOP_XYZ(i) update_software_endstops(i); // or modify the constrain function
        const bool safe_to_move = current_position.z < delta_clip_start_height - 1;
      #else
        constexpr bool safe_to_move = true;
      #endif

      // Return to position and lower again
      if (safe_to_move && !no_move && IsRunning()) {

        #if ENABLED(SINGLENOZZLE)
          #if FAN_COUNT > 0
            singlenozzle_fan_speed[old_tool] = thermalManager.fan_speed[0];
            thermalManager.fan_speed[0] = singlenozzle_fan_speed[new_tool];
          #endif

          singlenozzle_temp[old_tool] = thermalManager.temp_hotend[0].target;
          if (singlenozzle_temp[new_tool] && singlenozzle_temp[new_tool] != singlenozzle_temp[old_tool]) {
            thermalManager.setTargetHotend(singlenozzle_temp[new_tool], 0);
            #if HAS_DISPLAY
              thermalManager.set_heating_message(0);
            #endif
            (void)thermalManager.wait_for_hotend(0, false);  // Wait for heating or cooling
          }
        #endif

        #if ENABLED(TOOLCHANGE_FILAMENT_SWAP)
          if (should_swap && !too_cold) {
            #if ENABLED(ADVANCED_PAUSE_FEATURE)
              unscaled_e_move(toolchange_settings.swap_length, MMM_TO_MMS(toolchange_settings.prime_speed));
              unscaled_e_move(toolchange_settings.extra_prime, ADVANCED_PAUSE_PURGE_FEEDRATE);
            #else
              current_position.e += toolchange_settings.swap_length / planner.e_factor[new_tool];
              planner.buffer_line(current_position, MMM_TO_MMS(toolchange_settings.prime_speed), new_tool);
              current_position.e += toolchange_settings.extra_prime / planner.e_factor[new_tool];
              planner.buffer_line(current_position, MMM_TO_MMS(toolchange_settings.prime_speed * 0.2f), new_tool);
            #endif
            planner.synchronize();
            planner.set_e_position_mm((destination.e = current_position.e = current_position.e - (TOOLCHANGE_FIL_EXTRA_PRIME)));
          }
        #endif

        // Prevent a move outside physical bounds
        #if ENABLED(MAGNETIC_SWITCHING_TOOLHEAD)
          // If the original position is within tool store area, go to X origin at once
          if (destination.y < SWITCHING_TOOLHEAD_Y_POS + SWITCHING_TOOLHEAD_Y_CLEAR) {
            current_position.x = 0;
            planner.buffer_line(current_position, planner.settings.max_feedrate_mm_s[X_AXIS], new_tool);
            planner.synchronize();
          }
        #else
          apply_motion_limits(destination);
        #endif

        // Should the nozzle move back to the old position?
        if (can_move_away) {
          #if ENABLED(TOOLCHANGE_NO_RETURN)
            // Just move back down
            if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("Move back Z only");
            do_blocking_move_to_z(destination.z, planner.settings.max_feedrate_mm_s[Z_AXIS]);
          #else
            // Move back to the original (or adjusted) position
            if (DEBUGGING(LEVELING)) DEBUG_POS("Move back", destination);
            do_blocking_move_to(destination);
          #endif
        }
        else if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("Move back skipped");

        #if ENABLED(DUAL_X_CARRIAGE)
          active_extruder_parked = false;
        #endif
      }
      #if ENABLED(SWITCHING_NOZZLE)
        else {
          // Move back down. (Including when the new tool is higher.)
          do_blocking_move_to_z(destination.z, planner.settings.max_feedrate_mm_s[Z_AXIS]);
        }
      #endif

      #if ENABLED(PRUSA_MMU2)
        mmu2.tool_change(new_tool);
      #endif

      #if SWITCHING_NOZZLE_TWO_SERVOS
        lower_nozzle(new_tool);
      #endif

    } // (new_tool != old_tool)

    planner.synchronize();

    #if ENABLED(EXT_SOLENOID) && DISABLED(PARKING_EXTRUDER)
      disable_all_solenoids();
      enable_solenoid_on_active_extruder();
    #endif

    #if ENABLED(MK2_MULTIPLEXER)
      if (new_tool >= E_STEPPERS) return invalid_extruder_error(new_tool);
      select_multiplexed_stepper(new_tool);
    #endif

    #if DO_SWITCH_EXTRUDER
      planner.synchronize();
      move_extruder_servo(active_extruder);
    #endif

    #if HAS_FANMUX
      fanmux_switch(active_extruder);
    #endif

    #ifdef EVENT_GCODE_AFTER_TOOLCHANGE
      if (!no_move && TERN1(DUAL_X_CARRIAGE, dual_x_carriage_mode == DXC_AUTO_PARK_MODE))
        gcode.process_subcommands_now_P(PSTR(EVENT_GCODE_AFTER_TOOLCHANGE));
    #endif

    SERIAL_ECHO_START();
    SERIAL_ECHOLNPAIR(STR_ACTIVE_EXTRUDER, int(active_extruder));

  #endif // EXTRUDERS > 1
}
