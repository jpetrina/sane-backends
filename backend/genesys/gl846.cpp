/* sane - Scanner Access Now Easy.

   Copyright (C) 2012-2013 Stéphane Voltz <stef.dev@free.fr>


   This file is part of the SANE package.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

/** @file
 *
 * This file handles GL846 and GL845 ASICs since they are really close to each other.
 */

#define DEBUG_DECLARE_ONLY

#include "gl846.h"
#include "gl846_registers.h"
#include "test_settings.h"

#include <vector>
#include <cstdlib>
#include <cstring>

namespace genesys {
namespace gl846 {

/**
 * compute the step multiplier used
 */
static int gl846_get_step_multiplier (Genesys_Register_Set * regs)
{
    unsigned value = (regs->get8(0x9d) & 0x0f) >> 1;
    return 1 << value;
}

/** @brief set all registers to default values .
 * This function is called only once at the beginning and
 * fills register startup values for registers reused across scans.
 * Those that are rarely modified or not modified are written
 * individually.
 * @param dev device structure holding register set to initialize
 */
static void
gl846_init_registers (Genesys_Device * dev)
{
    DBG_HELPER(dbg);

    dev->reg.clear();

    dev->reg.init_reg(0x01, 0x60);
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_7400 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_8200I ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800)
    {
        dev->reg.init_reg(0x01, 0x22);
    }
    dev->reg.init_reg(0x02, 0x38);
    dev->reg.init_reg(0x03, 0x03);
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_7400 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_8200I)
    {
        dev->reg.init_reg(0x03, 0xbf);
    }
    dev->reg.init_reg(0x04, 0x22);
    dev->reg.init_reg(0x05, 0x60);
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_7400 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_8200I ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800)
    {
        dev->reg.init_reg(0x05, 0x48);
    }
    dev->reg.init_reg(0x06, 0x10);
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_7400 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_8200I ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800)
    {
        dev->reg.init_reg(0x06, 0xf0);
    }
    dev->reg.init_reg(0x08, 0x60);
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_7400 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_8200I ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800)
    {
        dev->reg.init_reg(0x08, 0x00);
    }
    dev->reg.init_reg(0x09, 0x00);
    dev->reg.init_reg(0x0a, 0x00);
    dev->reg.init_reg(0x0b, 0x8b);
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_3800) {
        dev->reg.init_reg(0x0b, 0x2a);
    }
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800) {
        dev->reg.init_reg(0x0b, 0x4a);   // 40 MHz clock, as the native driver
    }
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_7400 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_8200I)
    {
        dev->reg.init_reg(0x0b, 0x4a);
    }
    dev->reg.init_reg(0x0c, 0x00);
    dev->reg.init_reg(0x0d, 0x00);
    dev->reg.init_reg(0x10, 0x00); // exposure, set during sensor setup
    dev->reg.init_reg(0x11, 0x00); // exposure, set during sensor setup
    dev->reg.init_reg(0x12, 0x00); // exposure, set during sensor setup
    dev->reg.init_reg(0x13, 0x00); // exposure, set during sensor setup
    dev->reg.init_reg(0x14, 0x00); // exposure, set during sensor setup
    dev->reg.init_reg(0x15, 0x00); // exposure, set during sensor setup
    dev->reg.init_reg(0x16, 0xbb); // SENSOR_DEF
    dev->reg.init_reg(0x17, 0x13); // SENSOR_DEF
    dev->reg.init_reg(0x18, 0x10); // SENSOR_DEF
    dev->reg.init_reg(0x19, 0x2a); // SENSOR_DEF
    dev->reg.init_reg(0x1a, 0x34); // SENSOR_DEF
    dev->reg.init_reg(0x1b, 0x00); // SENSOR_DEF
    dev->reg.init_reg(0x1c, 0x20); // SENSOR_DEF
    dev->reg.init_reg(0x1d, 0x06); // SENSOR_DEF
    dev->reg.init_reg(0x1e, 0xf0); // WDTIME, LINESEL: set during sensor and motor setup

     // SCANFED
    dev->reg.init_reg(0x1f, 0x01);
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_7400 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800) {
        dev->reg.init_reg(0x1f, 0x00);
    }

    dev->reg.init_reg(0x20, 0x03);
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_7400 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_8200I)
    {
        dev->reg.init_reg(0x20, 0x55);
    }
    dev->reg.init_reg(0x21, 0x10); // STEPNO: set during motor setup
    dev->reg.init_reg(0x22, 0x60); // FWDSTEP: set during motor setup
    dev->reg.init_reg(0x23, 0x60); // BWDSTEP: set during motor setup
    dev->reg.init_reg(0x24, 0x60); // FASTNO: set during motor setup
    dev->reg.init_reg(0x25, 0x00); // LINCNT: set during motor setup
    dev->reg.init_reg(0x26, 0x00); // LINCNT: set during motor setup
    dev->reg.init_reg(0x27, 0x00); // LINCNT: set during motor setup
    dev->reg.init_reg(0x2c, 0x00); // DPISET: set during sensor setup
    dev->reg.init_reg(0x2d, 0x00); // DPISET: set during sensor setup
    dev->reg.init_reg(0x2e, 0x80); // BWHI: set during sensor setup
    dev->reg.init_reg(0x2f, 0x80); // BWLOW: set during sensor setup
    dev->reg.init_reg(0x30, 0x00); // STRPIXEL: set during sensor setup
    dev->reg.init_reg(0x31, 0x00); // STRPIXEL: set during sensor setup
    dev->reg.init_reg(0x32, 0x00); // ENDPIXEL: set during sensor setup
    dev->reg.init_reg(0x33, 0x00); // ENDPIXEL: set during sensor setup

    // DUMMY: the number of CCD dummy pixels
    dev->reg.init_reg(0x34, 0x1f);
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_7400 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_8200I ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800)
    {
        dev->reg.init_reg(0x34, 0x14);
    }

    dev->reg.init_reg(0x35, 0x00); // MAXWD: set during scan setup
    dev->reg.init_reg(0x36, 0x40); // MAXWD: set during scan setup
    dev->reg.init_reg(0x37, 0x00); // MAXWD: set during scan setup
    dev->reg.init_reg(0x38, 0x2a); // LPERIOD: set during sensor setup
    dev->reg.init_reg(0x39, 0xf8); // LPERIOD: set during sensor setup
    dev->reg.init_reg(0x3d, 0x00); // FEEDL: set during motor setup
    dev->reg.init_reg(0x3e, 0x00); // FEEDL: set during motor setup
    dev->reg.init_reg(0x3f, 0x01); // FEEDL: set during motor setup
    dev->reg.init_reg(0x52, 0x02); // SENSOR_DEF
    dev->reg.init_reg(0x53, 0x04); // SENSOR_DEF
    dev->reg.init_reg(0x54, 0x06); // SENSOR_DEF
    dev->reg.init_reg(0x55, 0x08); // SENSOR_DEF
    dev->reg.init_reg(0x56, 0x0a); // SENSOR_DEF
    dev->reg.init_reg(0x57, 0x00); // SENSOR_DEF
    dev->reg.init_reg(0x58, 0x59); // SENSOR_DEF
    dev->reg.init_reg(0x59, 0x31); // SENSOR_DEF
    dev->reg.init_reg(0x5a, 0x40); // SENSOR_DEF

    // DECSEL, STEPTIM
    dev->reg.init_reg(0x5e, 0x1f);
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_7400 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_8200I)
    {
        dev->reg.init_reg(0x5e, 0x01);
    }
    dev->reg.init_reg(0x5f, 0x01); // FMOVDEC: overwritten during motor setup
    dev->reg.init_reg(0x60, 0x00); // STEPSEL, Z1MOD: overwritten during motor setup
    dev->reg.init_reg(0x61, 0x00); // Z1MOD: overwritten during motor setup
    dev->reg.init_reg(0x62, 0x00); // Z1MOD: overwritten during motor setup
    dev->reg.init_reg(0x63, 0x00); // FSTPSEL, Z2MOD: overwritten during motor setup
    dev->reg.init_reg(0x64, 0x00); // Z2MOD: overwritten during motor setup
    dev->reg.init_reg(0x65, 0x00); // Z2MOD: overwritten during motor setup
    dev->reg.init_reg(0x67, 0x7f); // MTRPWM: overwritten during motor setup
    dev->reg.init_reg(0x68, 0x7f); // FASTPWM: overwritten during motor setup
    dev->reg.init_reg(0x69, 0x01); // FSHDEC: overwritten during motor setup
    dev->reg.init_reg(0x6a, 0x01); // FMOVNO: overwritten during motor setup
    // 0x6b, 0x6c, 0x6d, 0x6e, 0x6f - gpio
    dev->reg.init_reg(0x70, 0x01); // SENSOR_DEF
    dev->reg.init_reg(0x71, 0x00); // SENSOR_DEF
    dev->reg.init_reg(0x72, 0x02); // SENSOR_DEF
    dev->reg.init_reg(0x73, 0x01); // SENSOR_DEF
    dev->reg.init_reg(0x74, 0x00); // SENSOR_DEF
    dev->reg.init_reg(0x75, 0x00); // SENSOR_DEF
    dev->reg.init_reg(0x76, 0x00); // SENSOR_DEF
    dev->reg.init_reg(0x77, 0x00); // SENSOR_DEF
    dev->reg.init_reg(0x78, 0x00); // SENSOR_DEF
    dev->reg.init_reg(0x79, 0x3f); // SENSOR_DEF
    dev->reg.init_reg(0x7a, 0x00); // SENSOR_DEF
    dev->reg.init_reg(0x7b, 0x09); // SENSOR_DEF
    dev->reg.init_reg(0x7c, 0x99); // SENSOR_DEF
    dev->reg.init_reg(0x7d, 0x20); // SENSOR_DEF
    dev->reg.init_reg(0x7f, 0x05);
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_7400 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_8200I ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800)
    {
        dev->reg.init_reg(0x7f, 0x00);
    }
    dev->reg.init_reg(0x80, 0x4f); // overwritten during motor setup
    dev->reg.init_reg(0x87, 0x02); // SENSOR_DEF

    // MTRPLS: pulse width of ADF motor trigger signal
    dev->reg.init_reg(0x94, 0x00);
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_3800 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800) {
        dev->reg.init_reg(0x94, 0xff);
    }
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_3800 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800) {
        dev->reg.init_reg(0x98, 0x20); // ONDUR
        dev->reg.init_reg(0x99, 0x00); // ONDUR
        dev->reg.init_reg(0x9a, 0x90); // OFFDUR
        dev->reg.init_reg(0x9b, 0x00); // OFFDUR
    }

    dev->reg.init_reg(0x9d, 0x00); // contains STEPTIM
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_3800 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800) {
        dev->reg.init_reg(0x9d, 0x04);
    }
    dev->reg.init_reg(0x9e, 0x00);
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_3800 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800) {
        dev->reg.init_reg(0xa1, 0xe0);
    }

    // RFHSET (SDRAM refresh time)
    dev->reg.init_reg(0xa2, 0x1f);
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_7400 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_8200I ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800)
    {
        dev->reg.init_reg(0xa2, 0x0f);
    }

    // 0xa6, 0xa7 0xa8, 0xa9 - gpio

    // Various important settings: GPOM9, MULSTOP, NODECEL, TB3TB1, TB5TB2, FIX16CLK
    dev->reg.init_reg(0xab, 0xc0);
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_7400 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_8200I)
    {
        dev->reg.init_reg(0xab, 0x01);
    }
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_3800 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800) {
        dev->reg.init_reg(0xbb, 0x00); // FIXME: default is the same
    }
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_3800 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800) {
        dev->reg.init_reg(0xbc, 0x0f);
        dev->reg.init_reg(0xdb, 0xff);
    }
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_7400 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800) {
        dev->reg.init_reg(0xbe, 0x07);
    }

    // 0xd0, 0xd1, 0xd2 - SH0DWN, SH1DWN, SH2DWN - shading bank[0..2] for CCD.
    // Set during memory layout setup

    // [0xe0..0xf7] - image buffer addresses. Set during memory layout setup
    dev->reg.init_reg(0xf8, 0x05); // MAXSEL, MINSEL

    if (dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_3800 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800) {
        dev->reg.init_reg(0xfe, 0x08); // MOTTGST, AUTO_O
        dev->reg.init_reg(0xff, 0x02); // AUTO_S
    }

    const auto& sensor = sanei_genesys_find_sensor_any(dev);
    const auto& dpihw_sensor = sanei_genesys_find_sensor(dev, sensor.full_resolution,
                                                         3, dev->model->default_method);
    sanei_genesys_set_dpihw(dev->reg, dpihw_sensor.register_dpihw);
}

/**
 * Set register values of Analog Device type frontend
 * */
// OpticBook 4800: the shading pass runs in the scan's own window and read-out
// mode, with a lamp-off dark pass; this used to be limited to the full CCD
// clock, OB4800_CALIB_LEGACY=1 restores that limit for A/B comparison.
static bool ob4800_scan_window_calibration(const Genesys_Sensor& sensor, unsigned resolution)
{
    if (std::getenv("OB4800_CALIB_LEGACY") != nullptr) {
        return resolution == sensor.full_resolution;
    }
    return true;
}

static void gl846_set_adi_fe(Genesys_Device* dev, const Genesys_Sensor& sensor, std::uint8_t set)
{
    DBG_HELPER(dbg);
  int i;

    // wait for FE to be ready
    auto status = scanner_read_status(*dev);
    while (status.is_front_end_busy) {
        dev->interface->sleep_ms(10);
        status = scanner_read_status(*dev);
    };

    // OpticBook 4800: AFE registers 2-4 are offsets and 5-7 are gains
    // (reverse of the generic order); values are those computed by the
    // native driver's calibration for this unit
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800) {
        // AFE configuration register: 0xf8 for the 1200 dpi pixel pitch
        // the CCD clocking group of the session being set up: 300/600/1200.
        // (dev->session is not yet updated at this point, so it must not be used)
        unsigned pitch = sensor.shading_resolution;
        dev->interface->write_fe_register(0x00, pitch >= 1200 ? 0xf8 : 0x70);
        dev->interface->write_fe_register(0x01, 0x80);
        // AFE registers 2-4 are the gains (5-7 turned out to be offsets).
        // Values are from the native driver's 300 dpi dump; the 600 and 1200
        // dpi CCD clockings deliver ~30% more signal and saturate on white
        // paper, so they run 24 codes lower (measured on a white sheet)
        unsigned ofs[3] = { 0x3e, 0x34, 0x39 };
        if (pitch >= 600) {
            ofs[0] = 0x26; ofs[1] = 0x1c; ofs[2] = 0x21;
        }
        if (const char* e = std::getenv("OB4800_AFE_OFS_DELTA")) {
            int d = std::atoi(e);
            for (unsigned k = 0; k < 3; k++) {
                int v = static_cast<int>(ofs[k]) + d;
                ofs[k] = static_cast<unsigned>(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
        }
        dev->interface->write_fe_register(0x02, ofs[0]);
        dev->interface->write_fe_register(0x03, ofs[1]);
        dev->interface->write_fe_register(0x04, ofs[2]);
        unsigned gain[3] = { 0x114, 0x10c, 0x10d };
        if (const char* e = std::getenv("OB4800_AFE_GAIN_DELTA")) {
            int d = std::atoi(e);
            for (unsigned k = 0; k < 3; k++) {
                int v = static_cast<int>(gain[k] & 0xff) + d;
                v = v < 0 ? 0 : (v > 255 ? 255 : v);
                gain[k] = (gain[k] & 0x100) | static_cast<unsigned>(v);
            }
        }
        dev->interface->write_fe_register(0x05, gain[0]);
        dev->interface->write_fe_register(0x06, gain[1]);
        dev->interface->write_fe_register(0x07, gain[2]);
        return;
    }

    if (set == AFE_INIT) {
        dev->frontend = dev->frontend_initial;
    }

    // write them to analog frontend
    dev->interface->write_fe_register(0x00, dev->frontend.regs.get_value(0x00));

    dev->interface->write_fe_register(0x01, dev->frontend.regs.get_value(0x01));

    for (i = 0; i < 3; i++) {
        dev->interface->write_fe_register(0x02 + i, dev->frontend.get_gain(i));
    }
    for (i = 0; i < 3; i++) {
        dev->interface->write_fe_register(0x05 + i, dev->frontend.get_offset(i));
    }
}

// Set values of analog frontend
void CommandSetGl846::set_fe(Genesys_Device* dev, const Genesys_Sensor& sensor,
                             std::uint8_t set) const
{
    DBG_HELPER_ARGS(dbg, "%s", set == AFE_INIT ? "init" :
                               set == AFE_SET ? "set" :
                               set == AFE_POWER_SAVE ? "powersave" : "huh?");
    (void) sensor;

    // route to specific analog frontend setup
    std::uint8_t frontend_type = dev->reg.find_reg(0x04).value & REG_0x04_FESET;
    switch (frontend_type) {
      case 0x02: /* ADI FE */
        gl846_set_adi_fe(dev, sensor, set);
        break;
      default:
            throw SaneException("unsupported frontend type %d", frontend_type);
    }
}


// @brief set up motor related register for scan
static void gl846_init_motor_regs_scan(Genesys_Device* dev,
                                       const Genesys_Sensor& sensor,
                                       const ScanSession& session,
                                       Genesys_Register_Set* reg,
                                       const MotorProfile& motor_profile,
                                       unsigned int scan_exposure_time,
                                       unsigned scan_yres,
                                       unsigned int scan_lines,
                                       unsigned int scan_dummy,
                                       unsigned int feed_steps,
                                       ScanFlag flags)
{
    DBG_HELPER_ARGS(dbg, "scan_exposure_time=%d, scan_yres=%d, step_type=%d, scan_lines=%d, "
                         "scan_dummy=%d, feed_steps=%d, flags=%x",
                    scan_exposure_time, scan_yres, static_cast<unsigned>(motor_profile.step_type),
                    scan_lines, scan_dummy, feed_steps, static_cast<unsigned>(flags));

    unsigned step_multiplier = gl846_get_step_multiplier(reg);

    reg->set24(REG_LINCNT, scan_lines);

    reg->set8(REG_0x02, 0);
    sanei_genesys_set_motor_power(*reg, true);

    std::uint8_t reg02 = reg->get8(REG_0x02);
    reg02 &= ~REG_0x02_FASTFED;

    if (has_flag(flags, ScanFlag::AUTO_GO_HOME)) {
        reg02 |= REG_0x02_AGOHOME | REG_0x02_NOTHOME;
    }

    if (has_flag(flags, ScanFlag::DISABLE_BUFFER_FULL_MOVE) || (scan_yres>=sensor.full_resolution)) {
        reg02 |= REG_0x02_ACDCDIS;
    }
    if (has_flag(flags, ScanFlag::REVERSE)) {
        reg02 |= REG_0x02_MTRREV;
    } else {
        reg02 &= ~REG_0x02_MTRREV;
    }
    reg->set8(REG_0x02, reg02);

    // scan and backtracking slope table
    auto scan_table = create_slope_table(dev->model->asic_type, dev->motor, scan_yres,
                                         scan_exposure_time, step_multiplier, motor_profile);

    scanner_send_slope_table(dev, sensor, SCAN_TABLE, scan_table.table);
    scanner_send_slope_table(dev, sensor, BACKTRACK_TABLE, scan_table.table);
    scanner_send_slope_table(dev, sensor, STOP_TABLE, scan_table.table);

    reg->set8(REG_STEPNO, scan_table.table.size() / step_multiplier);
    reg->set8(REG_FASTNO, scan_table.table.size() / step_multiplier);
    reg->set8(REG_FSHDEC, scan_table.table.size() / step_multiplier);

    // fast table
    const auto* fast_profile = get_motor_profile_ptr(dev->motor.fast_profiles, 0, session);
    if (fast_profile == nullptr) {
        fast_profile = &motor_profile;
    }

    auto fast_table = create_slope_table_fastest(dev->model->asic_type, step_multiplier,
                                                 *fast_profile);

    scanner_send_slope_table(dev, sensor, FAST_TABLE, fast_table.table);
    scanner_send_slope_table(dev, sensor, HOME_TABLE, fast_table.table);

    reg->set8(REG_FMOVNO, fast_table.table.size() / step_multiplier);
    reg->set8(REG_FMOVDEC, fast_table.table.size() / step_multiplier);

    if (motor_profile.motor_vref != -1 && fast_profile->motor_vref != 1) {
        std::uint8_t vref = 0;
        vref |= (motor_profile.motor_vref << REG_0x80S_TABLE1_NORMAL) & REG_0x80_TABLE1_NORMAL;
        vref |= (motor_profile.motor_vref << REG_0x80S_TABLE2_BACK) & REG_0x80_TABLE2_BACK;
        vref |= (fast_profile->motor_vref << REG_0x80S_TABLE4_FAST) & REG_0x80_TABLE4_FAST;
        vref |= (fast_profile->motor_vref << REG_0x80S_TABLE5_GO_HOME) & REG_0x80_TABLE5_GO_HOME;
        reg->set8(REG_0x80, vref);
    }

    unsigned feedl = feed_steps;
    unsigned dist = 0;

    feedl <<= static_cast<unsigned>(motor_profile.step_type);
    dist = scan_table.table.size();
    if (has_flag(flags, ScanFlag::FEEDING)) {
        dist *= 2;
    }

    // check for overflow
    if (dist < feedl) {
        feedl -= dist;
    } else {
        feedl = 0;
    }

    reg->set24(REG_FEEDL, feedl);

    unsigned ccdlmt = (reg->get8(REG_0x0C) & REG_0x0C_CCDLMT) + 1;
    unsigned tgtime = 1 << (reg->get8(REG_0x1C) & REG_0x1C_TGTIME);

  /* hi res motor speed GPIO */
  /*
    uint8_t effective = dev->interface->read_register(REG_0x6C);
  */

  /* if quarter step, bipolar Vref2 */
  /* XXX STEF XXX GPIO
  if (motor_profile.step_type > 1)
    {
      if (motor_profile.step_type < 3)
        {
            val = effective & ~REG_0x6C_GPIO13;
        }
      else
        {
            val = effective | REG_0x6C_GPIO13;
        }
    }
  else
    {
      val = effective;
    }
    dev->interface->write_register(REG_0x6C, val);
    */

  /* effective scan */
  /*
    effective = dev->interface->read_register(REG_0x6C);
    val = effective | REG_0x6C_GPIO10;
    dev->interface->write_register(REG_0x6C, val);
  */

    unsigned min_restep = (scan_table.table.size() / step_multiplier) / 2 - 1;
    if (min_restep < 1) {
        min_restep = 1;
    }

    reg->set8(REG_FWDSTEP, min_restep);
    reg->set8(REG_BWDSTEP, min_restep);

    std::uint32_t z1, z2;
    sanei_genesys_calculate_zmod(false,
                                 scan_exposure_time * ccdlmt * tgtime,
                                 scan_table.table,
                                 scan_table.table.size(),
                                 feedl,
                                 min_restep * step_multiplier,
                                 &z1,
                                 &z2);

    reg->set24(REG_0x60, z1 | (static_cast<unsigned>(motor_profile.step_type) << (16 + REG_0x60S_STEPSEL)));
    reg->set24(REG_0x63, z2 | (static_cast<unsigned>(motor_profile.step_type) << (16 + REG_0x63S_FSTPSEL)));

    reg->set8_mask(REG_0x1E, scan_dummy, 0x0f);

    reg->set8(REG_0x67, 0x7f);
    reg->set8(REG_0x68, 0x7f);
}


/** @brief set up registers related to sensor
 * Set up the following registers
   0x01
   0x03
   0x10-0x015     R/G/B exposures
   0x19           EXPDMY
   0x2e           BWHI
   0x2f           BWLO
   0x04
   0x87
   0x05
   0x2c,0x2d      DPISET
   0x30,0x31      STRPIXEL
   0x32,0x33      ENDPIXEL
   0x35,0x36,0x37 MAXWD [25:2] (>>2)
   0x38,0x39      LPERIOD
   0x34           DUMMY
 */
static void gl846_init_optical_regs_scan(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                         Genesys_Register_Set* reg, unsigned int exposure_time,
                                         const ScanSession& session)
{
    DBG_HELPER_ARGS(dbg, "exposure_time=%d", exposure_time);

    scanner_setup_sensor(*dev, sensor, *reg);

    dev->cmd_set->set_fe(dev, sensor, AFE_SET);

  /* enable shading */
    regs_set_optical_off(dev->model->asic_type, *reg);
    reg->find_reg(REG_0x01).value |= REG_0x01_SHDAREA;
    if (has_flag(session.params.flags, ScanFlag::DISABLE_SHADING) ||
        has_flag(dev->model->flags, ModelFlag::DISABLE_SHADING_CALIBRATION) ||
        session.use_host_side_calib)
    {
        reg->find_reg(REG_0x01).value &= ~REG_0x01_DVDSET;
    } else {
        reg->find_reg(REG_0x01).value |= REG_0x01_DVDSET;
    }

    reg->find_reg(REG_0x03).value &= ~REG_0x03_AVEENB;

    sanei_genesys_set_lamp_power(dev, sensor, *reg,
                                 !has_flag(session.params.flags, ScanFlag::DISABLE_LAMP));
    reg->state.is_xpa_on = has_flag(session.params.flags, ScanFlag::USE_XPA);

    // BW threshold
    reg->set8(0x2e, 0x7f);
    reg->set8(0x2f, 0x7f);

  /* monochrome / color scan */
    switch (session.params.depth) {
    case 8:
            reg->find_reg(REG_0x04).value &= ~(REG_0x04_LINEART | REG_0x04_BITSET);
      break;
    case 16:
            reg->find_reg(REG_0x04).value &= ~REG_0x04_LINEART;
            reg->find_reg(REG_0x04).value |= REG_0x04_BITSET;
      break;
    }

    reg->find_reg(REG_0x04).value &= ~(REG_0x04_FILTER | REG_0x04_AFEMOD);
  if (session.params.channels == 1)
    {
      switch (session.params.color_filter)
        {
            case ColorFilter::RED:
                reg->find_reg(REG_0x04).value |= 0x24;
                break;
            case ColorFilter::BLUE:
                reg->find_reg(REG_0x04).value |= 0x2c;
                break;
            case ColorFilter::GREEN:
                reg->find_reg(REG_0x04).value |= 0x28;
                break;
            default:
                break; // should not happen
        }
    } else {
        reg->find_reg(REG_0x04).value |= 0x20; // mono
    }

    const auto& dpihw_sensor = sanei_genesys_find_sensor(dev, session.output_resolution,
                                                         session.params.channels,
                                                         session.params.scan_method);
    sanei_genesys_set_dpihw(*reg, dpihw_sensor.register_dpihw);

    if (should_enable_gamma(session, sensor)) {
        reg->find_reg(REG_0x05).value |= REG_0x05_GMMENB;
    } else {
        reg->find_reg(REG_0x05).value &= ~REG_0x05_GMMENB;
    }

  /* CIS scanners can do true gray by setting LEDADD */
  /* we set up LEDADD only when asked */
    if (dev->model->is_cis) {
        reg->find_reg(0x87).value &= ~REG_0x87_LEDADD;

        if (session.enable_ledadd) {
            reg->find_reg(0x87).value |= REG_0x87_LEDADD;
        }
      /* RGB weighting
        reg->find_reg(0x01).value &= ~REG_0x01_TRUEGRAY;

      if (session.enable_ledadd))
        {
            reg->find_reg(0x01).value |= REG_0x01_TRUEGRAY;
        }*/
    }

    reg->set16(REG_DPISET, sensor.register_dpiset);
    reg->set16(REG_STRPIXEL, session.pixel_startx);
    reg->set16(REG_ENDPIXEL, session.pixel_endx);

    setup_image_pipeline(*dev, session);

  /* MAXWD is expressed in 4 words unit */
    // BUG: we shouldn't multiply by channels here
    reg->set24(REG_MAXWD, (session.output_line_bytes_raw * session.params.channels >> 2));
    reg->set16(REG_LPERIOD, exposure_time);
    reg->set8(0x34, sensor.dummy_pixel);

    // OpticBook 4800: capture only works with SCANMOD=0 (native driver
    // sets 0x06 from 0xf0 to 0x10 right before scanning)
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800) {
        reg->find_reg(REG_0x06).value &= ~REG_0x06_SCANMOD;
    }

    // OpticBook 4800: memory layout depends on channel count (native driver
    // uses two large segments repeated 3x for gray, six segments for color)
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800) {
        static const std::uint8_t gray[] = {
            0x00,0x68, 0x08,0x33, 0x08,0x34, 0x0f,0xff,
            0x00,0x68, 0x08,0x33, 0x08,0x34, 0x0f,0xff,
            0x00,0x68, 0x08,0x33, 0x08,0x34, 0x0f,0xff, 0x23 };
        static const std::uint8_t color[] = {
            0x00,0x68, 0x03,0x00, 0x03,0x01, 0x05,0x99,
            0x05,0x9a, 0x08,0x32, 0x08,0x33, 0x0a,0xcb,
            0x0a,0xcc, 0x0d,0x64, 0x0d,0x65, 0x0f,0xfd, 0x05 };
        const std::uint8_t* lay = (session.params.channels == 1) ? gray : color;
        // GPIO 0x6d: the native driver clears it for the 1200 dpi mode
        dev->interface->write_register(0x6d, session.params.xres >= 1200 ? 0x00 : 0x80);
        for (unsigned i = 0; i < 24; i++) {
            dev->interface->write_register(0xe0 + i, lay[i]);
        }
        reg->set8(0xf8, lay[24]);   // 0xf8 is part of the register set and
                                    // would be overwritten by the bulk write
    }
}

void CommandSetGl846::init_regs_for_scan_session(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                                 Genesys_Register_Set* reg,
                                                 const ScanSession& session) const
{
    DBG_HELPER(dbg);
    session.assert_computed();

  int exposure_time;

  int slope_dpi = 0;

    // FIXME: on cis scanners we may want to scan at reduced resolution
    int dummy = 0;

/* slope_dpi */
/* cis color scan is effectively a gray scan with 3 gray lines per color
   line and a FILTER of 0 */
    if (dev->model->is_cis) {
        slope_dpi = session.params.yres * session.params.channels;
    } else {
        slope_dpi = session.params.yres;
    }

  slope_dpi = slope_dpi * (1 + dummy);

    // OpticBook 4800: the motor never runs slower than the 300 dpi pitch,
    // lower resolutions are produced by averaging lines in the pipeline
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800 && slope_dpi < 300) {
        slope_dpi = (slope_dpi == 200) ? 600 : 300;
    }

    exposure_time = session.params.exposure_lperiod;
    const auto& motor_profile = get_motor_profile(dev->motor.profiles, exposure_time, session);

  /* we enable true gray for cis scanners only, and just when doing
   * scan since color calibration is OK for this mode
   */
    gl846_init_optical_regs_scan(dev, sensor, reg, exposure_time, session);
    gl846_init_motor_regs_scan(dev, sensor, session, reg, motor_profile, exposure_time, slope_dpi,
                               session.optical_line_count, dummy, session.params.starty,
                               session.params.flags);

  /*** prepares data reordering ***/

    dev->read_active = true;

    dev->session = session;

    dev->total_bytes_read = 0;
    dev->total_bytes_to_read = (size_t)session.output_line_bytes_requested * (size_t)session.params.lines;

    DBG(DBG_info, "%s: total bytes to send = %zu\n", __func__, dev->total_bytes_to_read);
}

ScanSession CommandSetGl846::calculate_scan_session(const Genesys_Device* dev,
                                                    const Genesys_Sensor& sensor,
                                                    const Genesys_Settings& settings) const
{
    DBG(DBG_info, "%s ", __func__);
    debug_dump(DBG_info, settings);

    ScanFlag flags = ScanFlag::NONE;

    unsigned move_dpi = dev->motor.base_ydpi;

    float move = dev->model->y_offset;
    if (settings.scan_method == ScanMethod::TRANSPARENCY ||
        settings.scan_method == ScanMethod::TRANSPARENCY_INFRARED)
    {
        // note: scanner_move_to_ta() function has already been called and the sensor is at the
        // transparency adapter
        if (!dev->ignore_offsets) {
            move = dev->model->y_offset_ta - dev->model->y_offset_sensor_to_ta;
        }
        flags |= ScanFlag::USE_XPA;
    } else {
        if (!dev->ignore_offsets) {
            move = dev->model->y_offset;
        }
    }

    move = move + settings.tl_y;
    move = static_cast<float>((move * move_dpi) / MM_PER_INCH);
    move -= dev->head_pos(ScanHeadId::PRIMARY);

    float start = dev->model->x_offset;
    if (settings.scan_method == ScanMethod::TRANSPARENCY ||
        settings.scan_method == ScanMethod::TRANSPARENCY_INFRARED)
    {
        start = dev->model->x_offset_ta;
    } else {
        start = dev->model->x_offset;
    }

    start = start + dev->settings.tl_x;
    start = static_cast<float>((start * settings.xres) / MM_PER_INCH);

    ScanSession session;
    session.params.xres = settings.xres;
    session.params.yres = settings.yres;
    session.params.startx = static_cast<unsigned>(start);
    session.params.starty = static_cast<unsigned>(move);
    session.params.pixels = settings.pixels;
    session.params.requested_pixels = settings.requested_pixels;
    session.params.lines = settings.lines;
    session.params.depth = settings.depth;
    session.params.channels = settings.get_channels();
    session.params.scan_method = settings.scan_method;
    session.params.scan_mode = settings.scan_mode;
    session.params.color_filter = settings.color_filter;
    session.params.contrast_adjustment = settings.contrast;
    session.params.brightness_adjustment = settings.brightness;
    session.params.exposure_lperiod = dev->settings.exposure_lperiod;
    // backtracking isn't handled well, so don't enable it
    session.params.flags = flags;

    compute_session(dev, session, sensor);

    return session;
}

// for fast power saving methods only, like disabling certain amplifiers
void CommandSetGl846::save_power(Genesys_Device* dev, bool enable) const
{
    (void) dev;
    DBG_HELPER_ARGS(dbg, "enable = %d", enable);
}

void CommandSetGl846::set_powersaving(Genesys_Device* dev, int delay /* in minutes */) const
{
    (void) dev;
    DBG_HELPER_ARGS(dbg, "delay = %d", delay);
}

// Send the low-level scan command
void CommandSetGl846::begin_scan(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                 Genesys_Register_Set* reg, bool start_motor) const
{
    DBG_HELPER(dbg);
    (void) sensor;

    if (reg->state.is_xpa_on && reg->state.is_lamp_on) {
        dev->cmd_set->set_xpa_lamp_power(*dev, true);
    }

    scanner_clear_scan_and_feed_counts(*dev);
    std::uint8_t val = dev->interface->read_register(REG_0x01);
    val |= REG_0x01_SCAN;
    dev->interface->write_register(REG_0x01, val);
    reg->set8(REG_0x01, val);

    scanner_start_action(*dev, start_motor);

    dev->advance_head_pos_by_session(ScanHeadId::PRIMARY);
}


// Send the stop scan command
void CommandSetGl846::end_scan(Genesys_Device* dev, Genesys_Register_Set* reg,
                               bool check_stop) const
{
    (void) reg;
    DBG_HELPER_ARGS(dbg, "check_stop = %d", check_stop);

    if (reg->state.is_xpa_on) {
        dev->cmd_set->set_xpa_lamp_power(*dev, false);
    }

    if (!dev->model->is_sheetfed) {
        scanner_stop_action(*dev);
    }
}

// Moves the slider to the home (top) position slowly
void CommandSetGl846::move_back_home(Genesys_Device* dev, bool wait_until_home) const
{
    scanner_move_back_home(*dev, wait_until_home);
}

// init registers for shading calibration
void CommandSetGl846::init_regs_for_shading(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                            Genesys_Register_Set& regs) const
{
    DBG_HELPER(dbg);

    unsigned move_dpi = dev->motor.base_ydpi;

    float calib_size_mm = 0;
    if (dev->settings.scan_method == ScanMethod::TRANSPARENCY ||
        dev->settings.scan_method == ScanMethod::TRANSPARENCY_INFRARED)
    {
        calib_size_mm = dev->model->y_size_calib_ta_mm;
    } else {
        calib_size_mm = dev->model->y_size_calib_mm;
    }

    unsigned channels = 3;
    // OB4800: calibrate in the same channel mode as the scan. The CCD read-out
    // differs between 1- and 3-channel sessions and narrow response defects
    // (dust close to the sensor) sit at different pixels in each mode, so a
    // 3-channel reference cannot correct a gray scan at 1200 dpi. The single
    // channel is spread to R,G,B later (the chip always consumes 3-channel
    // shading records). OB4800_CALIB_3CH=1 restores the old behaviour.
    unsigned resolution = sensor.shading_resolution;
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800 &&
        std::getenv("OB4800_CALIB_3CH") == nullptr &&
        dev->settings.get_channels() == 1 &&
        ob4800_scan_window_calibration(sensor, resolution)) {
        // single-channel calibration is only needed at the full CCD clock
        // (1200 dpi), where it removes the vertical stripes. At the half/quarter
        // clock (200/600 and 75-300) it wrecks the left edge and those groups
        // have no stripes, so they keep the 3-channel calibration.
        channels = 1;
    }

    const auto& calib_sensor = sanei_genesys_find_sensor(dev, resolution, channels,
                                                         dev->settings.scan_method);

    float move = 0;
    ScanFlag flags = ScanFlag::DISABLE_SHADING |
                     ScanFlag::DISABLE_GAMMA |
                     ScanFlag::DISABLE_BUFFER_FULL_MOVE;

    if (dev->settings.scan_method == ScanMethod::TRANSPARENCY ||
        dev->settings.scan_method == ScanMethod::TRANSPARENCY_INFRARED)
    {
        // note: scanner_move_to_ta() function has already been called and the sensor is at the
        // transparency adapter
        move = static_cast<int>(dev->model->y_offset_calib_white_ta - dev->model->y_offset_sensor_to_ta);
        flags |= ScanFlag::USE_XPA;
    } else {
        move = static_cast<int>(dev->model->y_offset_calib_white);
        if (dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800) {
            // white reference capture: calibrate on a sheet lying on the glass
            if (const char* e = std::getenv("OB4800_CALIB_Y_MM")) {
                float y = static_cast<float>(std::atof(e));
                if (y >= 18.0f && y <= 60.0f) {
                    move = y;
                }
            }
        }
        // the carriage cannot move backwards: the calibration area must end
        // before the scan starts, otherwise the head is driven into the stop
        float scan_start = dev->model->y_offset + dev->settings.tl_y;
        if (move + calib_size_mm > scan_start) {
            DBG(DBG_error0, "OpticBook 4800: refusing to calibrate at %.1f-%.1f mm because the scan "
                           "starts at %.1f mm and the carriage cannot move backwards. Start the "
                           "scan further down (-t) or move the calibration area up.\n",
                move, move + calib_size_mm, scan_start);
            throw SaneException(SANE_STATUS_INVAL,
                                "calibration area %.1f-%.1f mm must end before the scan start "
                                "at %.1f mm (carriage cannot move backwards)",
                                move, move + calib_size_mm, scan_start);
        }
    }

    move = static_cast<float>((move * move_dpi) / MM_PER_INCH);

    unsigned calib_lines = static_cast<unsigned>(calib_size_mm * resolution / MM_PER_INCH);

    ScanSession session;
    session.params.xres = resolution;
    session.params.yres = resolution;
    session.params.startx = 0;
    session.params.starty = static_cast<unsigned>(move);
    session.params.pixels = dev->model->x_size_calib_mm * resolution / MM_PER_INCH;
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800 &&
        ob4800_scan_window_calibration(sensor, resolution)) {
        // Single-channel shading pass: use the scan's own window. The chip does
        // not deliver a single-channel read-out that starts at sensor pixel 0 at
        // the full 1200 dpi clock, and a window equal to the scan's lines the
        // reference up with the frame pixel for pixel.
        unsigned full = session.params.pixels;
        unsigned sx = static_cast<unsigned>((dev->model->x_offset + dev->settings.tl_x) *
                                            resolution / MM_PER_INCH);
        unsigned px = static_cast<unsigned>(static_cast<unsigned long long>(dev->settings.pixels) *
                                            resolution / dev->settings.xres);
        if (sx < full && px > 0) {
            if (sx + px > full) {
                px = full - sx;
            }
            session.params.startx = sx;
            session.params.pixels = px;
        }
        DBG(DBG_info, "%s: OB4800 shading window startx=%u pixels=%u (channels %u)\n",
            __func__, session.params.startx, session.params.pixels, channels);
    }
    session.params.lines = calib_lines;
    // OpticBook 4800: the chip delivers 8 bit data only; the calibration
    // reader widens it to 16 bit
    session.params.depth = (dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800) ? 8 : 16;
    session.params.channels = channels;
    session.params.scan_method = dev->settings.scan_method;
    session.params.scan_mode = ScanColorMode::COLOR_SINGLE_PASS;
    session.params.color_filter = dev->settings.color_filter;
    session.params.contrast_adjustment = dev->settings.contrast;
    session.params.brightness_adjustment = dev->settings.brightness;
    session.params.exposure_lperiod = dev->settings.exposure_lperiod;
    session.params.flags = flags;
    compute_session(dev, session, calib_sensor);

    init_regs_for_scan_session(dev, calib_sensor, &regs, session);

  /* we use ModelFlag::SHADING_REPARK */
    dev->set_head_pos_zero(ScanHeadId::PRIMARY);

    dev->calib_session = session;
}

/**
 * Send shading calibration data. The buffer is considered to always hold values
 * for all the channels.
 */
void CommandSetGl846::send_shading_data(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                        std::uint8_t* data, int size) const
{
    DBG_HELPER_ARGS(dbg, "writing %d bytes of shading data", size);
    std::uint32_t addr, i;

    unsigned length = static_cast<unsigned>(size / 3);

    // we're using SHDAREA, thus we only need to upload part of the line
    unsigned offset = dev->session.pixel_count_ratio.apply(
                dev->session.params.startx * sensor.full_resolution / dev->session.params.xres);
    unsigned pixels = dev->session.pixel_count_ratio.apply(dev->session.optical_pixels_raw);

    // turn pixel value into bytes 2x16 bits words
    offset *= 2 * 2;
    pixels *= 2 * 2;

    dev->interface->record_key_value("shading_offset", std::to_string(offset));
    dev->interface->record_key_value("shading_pixels", std::to_string(pixels));
    dev->interface->record_key_value("shading_length", std::to_string(length));
    dev->interface->record_key_value("shading_factor", std::to_string(sensor.shading_factor));

    std::vector<std::uint8_t> buffer(pixels, 0);

  DBG(DBG_io2, "%s: using chunks of %d (0x%04x) bytes\n", __func__, pixels, pixels);

  /* base addr of data has been written in reg D0-D4 in 4K word, so AHB address
   * is 8192*reg value */

    if (dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800) {
        // OpticBook 4800: all three shading banks (0xd0-0xd2) point to the
        // same memory, and the ASIC expects one block with the three channels
        // interleaved per pixel: dark_R gain_R dark_G gain_G dark_B gain_B.
        // Source coefficients are at calibration pitch; take every
        // shading_factor-th one for the output pitch.
        // the shading table is paged: 512 bytes per page, 42 records of 12 bytes
        // (dark/gain for R, G, B) per page, the last 8 bytes of a page unused.
        // data already holds one 12-byte record per calibration pixel; the chip
        // indexes the table from the first pixel of the scan window
        const unsigned REC = 12, PAGE = 512, PER_PAGE = PAGE / REC;
        unsigned count = static_cast<unsigned>(size) / REC;
        // the chip counts the table from params.startx, i.e. without the
        // sensor's output_pixel_offset (verified by aligning a dust speck)
        // The averaged lines are stored with the start offset that genesys uses
        // for them, startx * full_resolution / xres (see
        // genesys_shading_calibration_impl); the table must be read from the
        // same index, otherwise the correction is displaced by
        // startx * (full_resolution / xres - 1) pixels at the lower clockings.
        unsigned src_px = static_cast<unsigned>(
            static_cast<unsigned long long>(dev->session.params.startx) * sensor.full_resolution
            / dev->session.params.xres);
        if (const char* e = std::getenv("OB4800_SHADING_SHIFT")) {
            int sh = std::atoi(e);
            src_px = (sh < 0 && static_cast<unsigned>(-sh) > src_px) ? 0 : src_px + sh;
        }
        if (src_px < count) {
            count -= src_px;
        } else {
            count = 0;
        }
        unsigned pages = (count + PER_PAGE - 1) / PER_PAGE;
        std::vector<std::uint8_t> mixed(pages * PAGE, 0);
        for (unsigned n = 0; n < count; n++) {
            std::uint8_t* ptr = mixed.data() + (n / PER_PAGE) * PAGE + (n % PER_PAGE) * REC;
            std::memcpy(ptr, data + (src_px + n) * REC, REC);
        }
        (void) pixels;
        (void) length;
        if (std::getenv("OB4800_SHADING_TEST")) {
            // synthetic table: unity gain everywhere, test stripes:
            // 500-519 x2, 560-579 x4, 620-639 x6, 680-699 x0.5, 740-759 x1.5
            for (unsigned n = 0; n < count; n++) {
                std::uint8_t* ptr = mixed.data() + (n / PER_PAGE) * PAGE + (n % PER_PAGE) * REC;
                unsigned g = 0x2000;
                if (n >= 500 && n < 520) g = 0x4000;
                if (n >= 560 && n < 580) g = 0x8000;
                if (n >= 620 && n < 640) g = 0xC000;
                if (n >= 680 && n < 700) g = 0x1000;
                if (n >= 740 && n < 760) g = 0x3000;
                for (i = 0; i < 3; i++) {
                    ptr[0] = 0; ptr[1] = 0;
                    ptr[2] = g & 0xff; ptr[3] = g >> 8;
                    ptr += 4;
                }
            }
        }
        std::uint8_t val = dev->interface->read_register(0xd0);
        addr = val * 8192 + 0x10000000;
        dev->interface->write_ahb(addr, pages * PAGE, mixed.data());
        return;
    }

  /* write actual color channel data */
  for(i=0;i<3;i++)
    {
      /* build up actual shading data by copying the part from the full width one
       * to the one corresponding to SHDAREA */
        std::uint8_t* ptr = buffer.data();

      /* iterate on both sensor segment */
        for (unsigned x = 0; x < pixels; x += 4 * sensor.shading_factor) {
          // coefficient source
            std::uint8_t* src = (data + offset + i * length) + x;

          /* coefficient copy */
          ptr[0]=src[0];
          ptr[1]=src[1];
          ptr[2]=src[2];
          ptr[3]=src[3];

          /* next shading coefficient */
          ptr+=4;
        }

        std::uint8_t val = dev->interface->read_register(0xd0+i);
        addr = val * 8192 + 0x10000000;
        dev->interface->write_ahb(addr, pixels, buffer.data());
    }
}

/** @brief calibrates led exposure
 * Calibrate exposure by scanning a white area until the used exposure gives
 * data white enough.
 * @param dev device to calibrate
 */
SensorExposure CommandSetGl846::led_calibration(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                                Genesys_Register_Set& regs) const
{
    return scanner_led_calibration(*dev, sensor, regs);
}

/**
 * set up GPIO/GPOE for idle state
 */
static void gl846_init_gpio(Genesys_Device* dev)
{
    DBG_HELPER(dbg);
    apply_registers_ordered(dev->gpo.regs, { 0x6e, 0x6f }, [&](const GenesysRegisterSetting& reg)
    {
        dev->interface->write_register(reg.address, reg.value);
    });
}

/**
 * set memory layout by filling values in dedicated registers
 */
static void gl846_init_memory_layout(Genesys_Device* dev)
{
    DBG_HELPER(dbg);

    // prevent further writings by bulk write register
    dev->reg.remove_reg(0x0b);

    apply_reg_settings_to_device_write_only(*dev, dev->memory_layout.regs);
}

/* *
 * initialize ASIC from power on condition
 */
void CommandSetGl846::asic_boot(Genesys_Device* dev, bool cold) const
{
    DBG_HELPER(dbg);
    std::uint8_t val;

    // reset ASIC if cold boot
    if (cold) {
        dev->interface->write_register(0x0e, 0x01);
        dev->interface->write_register(0x0e, 0x00);
    }

    // OpticBook 4800 (GL845): register 0x0e must stay at 1 after reset,
    // otherwise BUFEMPTY never clears and valid word count reads as 0
    // even though scanned data is present in memory
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800) {
        dev->interface->write_register(0x0e, 0x01);
    }

    if (dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_3800 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800) {
        if (dev->usb_mode == 1) {
            val = 0x14;
        } else {
            val = 0x11;
        }
        dev->interface->write_0x8c(0x0f, val);
    }

    // test CHKVER
    val = dev->interface->read_register(REG_0x40);
    if (val & REG_0x40_CHKVER) {
        val = dev->interface->read_register(0x00);
        DBG(DBG_info, "%s: reported version for genesys chip is 0x%02x\n", __func__, val);
    }

    gl846_init_registers (dev);

    // Write initial registers
    dev->interface->write_registers(dev->reg);

  /* CIS_LINE */
  if (dev->model->is_cis)
    {
        dev->reg.init_reg(0x08, REG_0x08_CIS_LINE);
        dev->interface->write_register(0x08, dev->reg.find_reg(0x08).value);
    }

    // set up clocks
    if (dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_7400 ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICFILM_8200I ||
        dev->model->model_id == ModelId::PLUSTEK_OPTICBOOK_4800)
    {
        dev->interface->write_0x8c(0x10, 0x0c);
        dev->interface->write_0x8c(0x13, 0x0c);
    } else {
        dev->interface->write_0x8c(0x10, 0x0e);
        dev->interface->write_0x8c(0x13, 0x0e);
    }

    // setup gpio
    gl846_init_gpio(dev);

    // setup internal memory layout
    gl846_init_memory_layout(dev);

  dev->reg.init_reg(0xf8, 0x05);
    dev->interface->write_register(0xf8, dev->reg.find_reg(0xf8).value);
}

/**
 * initialize backend and ASIC : registers, motor tables, and gamma tables
 * then ensure scanner's head is at home
 */
void CommandSetGl846::init(Genesys_Device* dev) const
{
  DBG_INIT ();
    DBG_HELPER(dbg);

    sanei_genesys_asic_init(dev);
}

void CommandSetGl846::update_hardware_sensors(Genesys_Scanner* s) const
{
    DBG_HELPER(dbg);
  /* do what is needed to get a new set of events, but try to not lose
     any of them.
   */
    std::uint8_t scan, file, email, copy;
  switch(s->dev->model->gpio_id)
    {
      default:
        scan=0x01;
        file=0x02;
        email=0x04;
        copy=0x08;
    }
    std::uint8_t val = s->dev->interface->read_register(REG_0x6D);

    s->buttons[BUTTON_SCAN_SW].write((val & scan) == 0);
    s->buttons[BUTTON_FILE_SW].write((val & file) == 0);
    s->buttons[BUTTON_EMAIL_SW].write((val & email) == 0);
    s->buttons[BUTTON_COPY_SW].write((val & copy) == 0);
}


void CommandSetGl846::update_home_sensor_gpio(Genesys_Device& dev) const
{
    DBG_HELPER(dbg);

    std::uint8_t val = dev.interface->read_register(REG_0x6C);
    val |= 0x41;
    dev.interface->write_register(REG_0x6C, val);
}

void CommandSetGl846::offset_calibration(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                         Genesys_Register_Set& regs) const
{
    scanner_offset_calibration(*dev, sensor, regs);
}

void CommandSetGl846::coarse_gain_calibration(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                              Genesys_Register_Set& regs, int dpi) const
{
    scanner_coarse_gain_calibration(*dev, sensor, regs, dpi);
}

bool CommandSetGl846::needs_home_before_init_regs_for_scan(Genesys_Device* dev) const
{
    (void) dev;
    return false;
}

void CommandSetGl846::init_regs_for_warmup(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                           Genesys_Register_Set* regs) const
{
    (void) dev;
    (void) sensor;
    (void) regs;
    throw SaneException("not implemented");
}

void CommandSetGl846::send_gamma_table(Genesys_Device* dev, const Genesys_Sensor& sensor) const
{
    sanei_genesys_send_gamma_table(dev, sensor);
}

void CommandSetGl846::wait_for_motor_stop(Genesys_Device* dev) const
{
    (void) dev;
}

void CommandSetGl846::load_document(Genesys_Device* dev) const
{
    (void) dev;
    throw SaneException("not implemented");
}

void CommandSetGl846::detect_document_end(Genesys_Device* dev) const
{
    (void) dev;
    throw SaneException("not implemented");
}

void CommandSetGl846::eject_document(Genesys_Device* dev) const
{
    (void) dev;
    throw SaneException("not implemented");
}

} // namespace gl846
} // namespace genesys
