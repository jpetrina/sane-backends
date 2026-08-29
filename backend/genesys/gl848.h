/* sane - Scanner Access Now Easy.

   Copyright (C) 2026 SANE project contributors

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

/*
   Command set for the Genesys GL848+ ASIC.

   Currently only the HP Scanjet 200 (USB 0x03f0:0x1c05) is known to use
   this chip. The protocol was reverse-engineered from USB traces of the
   vendor Windows driver (independent implementation; no vendor firmware
   or driver code is included). Scanning is implemented: native optical
   passes at 300/600/1200/2400 dpi, host-side downsample for the other
   listed resolutions, and host-side flat-field calibration.
*/

#ifndef BACKEND_GENESYS_GL848_H
#define BACKEND_GENESYS_GL848_H

#include "genesys.h"
#include "command_set_common.h"

namespace genesys {
namespace gl848 {

class CommandSetGl848 : public CommandSetCommon
{
public:
    ~CommandSetGl848() override = default;

    bool needs_home_before_init_regs_for_scan(Genesys_Device* dev) const override;

    void init(Genesys_Device* dev) const override;

    void init_regs_for_warmup(Genesys_Device* dev, const Genesys_Sensor& sensor,
                              Genesys_Register_Set* regs) const override;

    void init_regs_for_shading(Genesys_Device* dev, const Genesys_Sensor& sensor,
                               Genesys_Register_Set& regs) const override;

    void init_regs_for_scan_session(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                    Genesys_Register_Set* reg,
                                    const ScanSession& session) const override;

    void set_fe(Genesys_Device* dev, const Genesys_Sensor& sensor, std::uint8_t set) const override;
    void set_powersaving(Genesys_Device* dev, int delay) const override;
    void save_power(Genesys_Device* dev, bool enable) const override;

    void begin_scan(Genesys_Device* dev, const Genesys_Sensor& sensor,
                    Genesys_Register_Set* regs, bool start_motor) const override;

    void end_scan(Genesys_Device* dev, Genesys_Register_Set* regs, bool check_stop) const override;

    void send_gamma_table(Genesys_Device* dev, const Genesys_Sensor& sensor) const override;

    void offset_calibration(Genesys_Device* dev, const Genesys_Sensor& sensor,
                            Genesys_Register_Set& regs) const override;

    void coarse_gain_calibration(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                 Genesys_Register_Set& regs, int dpi) const override;

    SensorExposure led_calibration(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                   Genesys_Register_Set& regs) const override;

    void wait_for_motor_stop(Genesys_Device* dev) const override;

    void move_back_home(Genesys_Device* dev, bool wait_until_home) const override;

    void update_hardware_sensors(struct Genesys_Scanner* s) const override;

    void update_home_sensor_gpio(Genesys_Device& dev) const override;

    void load_document(Genesys_Device* dev) const override;

    void detect_document_end(Genesys_Device* dev) const override;

    void eject_document(Genesys_Device* dev) const override;

    void send_shading_data(Genesys_Device* dev, const Genesys_Sensor& sensor, std::uint8_t* data,
                           int size) const override;

    void clear_host_side_calibration(Genesys_Device* dev) const override;

    ScanSession calculate_scan_session(const Genesys_Device* dev,
                                       const Genesys_Sensor& sensor,
                                       const Genesys_Settings& settings) const override;

    void asic_boot(Genesys_Device* dev, bool cold) const override;
};

// Slope-table slots as used by the HP driver on GL848+.
// Five identical tables at 0x10000000 + 0x4000 * n.
enum SlopeTable
{
    SCAN_TABLE = 0,
    BACKTRACK_TABLE = 1,
    STOP_TABLE = 2,
    FAST_TABLE = 3,
    HOME_TABLE = 4,
};

/** Host-side shading path derived from genesys `--calibration-file`.
    `foo.cal` at 300 dpi becomes `foo-shading-300.bin`. */
std::string host_shading_path(const std::string& calib_file, unsigned dpi);

/** Native CIS pass selected for a user-facing DPI (integer downsample). */
struct OpticalPass {
    unsigned dpi = 0;
    unsigned samples_per_line = 0;
    unsigned page_lines = 0;
    unsigned feed_lines = 0;
    unsigned scale = 1;
};

OpticalPass choose_optical_pass(unsigned user_dpi);

/** CIS bed used for millimetre → sample mapping (matches the HP frame). */
const float kCisWidthMM = 218.0f;
const float kCisHeightMM = 297.0f;

/** Host-side crop / skip / line-count for one GL848+ pass.
    Hardware always clocks a full-width 16-bit CIS stream;
    `cis_lines == (skip_y + color_rows) * 3`. FEEDL is park-to-page only.
    Coordinates are millimetres from the left/top of the 218×297 mm CIS. */
struct PassGeometry {
    OpticalPass pass;
    unsigned scale_x = 1;
    unsigned scale_y = 1;
    unsigned optical_x0 = 0;
    unsigned optical_xw = 0;
    unsigned skip_y = 0;
    unsigned color_rows = 0;
    unsigned cis_lines = 0;
    unsigned output_width = 0;
    unsigned output_height = 0;
};

PassGeometry compute_pass_geometry(unsigned xres, unsigned yres,
                                   float glass_x0_mm, float glass_y0_mm,
                                   float glass_x1_mm, float glass_y1_mm);

} // namespace gl848
} // namespace genesys

#endif // BACKEND_GENESYS_GL848_H
