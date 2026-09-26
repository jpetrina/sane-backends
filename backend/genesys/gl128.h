/* sane - Scanner Access Now Easy.

   Copyright (C) 2026 vermiculous <vermiculous@disroot.org>
   Portions derived from pyopticfilm (https://github.com/jboneng/pyopticfilm),
   Copyright (C) jboneng, used with permission.

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
 * GL128 CommandSet (Plustek OpticFilm 8200i SE).
 *
 * Inherits CommandSetCommon for is_head_home(), set_xpa_lamp_power() and
 * set_motor_mode(), whose generic paths apply to this scanner.
 */

#ifndef BACKEND_GENESYS_GL128_H
#define BACKEND_GENESYS_GL128_H

#include "genesys.h"
#include "command_set_common.h"

namespace genesys {
namespace gl128 {

class CommandSetGl128 : public CommandSetCommon
{
public:
    ~CommandSetGl128() override = default;

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

    ScanSession calculate_scan_session(const Genesys_Device* dev,
                                       const Genesys_Sensor& sensor,
                                       const Genesys_Settings& settings) const override;

    void asic_boot(Genesys_Device* dev, bool cold) const override;

};

// Host downsample factor for a requested resolution: below 600 dpi the ASIC
// runs at 600 dpi and the host averages factor x factor blocks.
unsigned gl128_host_downsample(unsigned xres);

} // namespace gl128
} // namespace genesys

#endif // BACKEND_GENESYS_GL128_H
