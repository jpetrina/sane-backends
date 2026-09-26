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
 * GL128 command set (Plustek OpticFilm 8200i SE, USB 07b3:1825).
 *
 * The GL128 is a GL124-family ASIC. Register values, sequences and tables in
 * this file come from USB captures of the vendor driver (SilverFast) on this
 * scanner, largely as decoded by the pyopticfilm project; the source of each
 * value is noted next to it.
 */

#define DEBUG_DECLARE_ONLY

#include "gl128.h"
#include "gl128_registers.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <iterator>
#include <thread>
#include <utility>
#include <vector>

namespace genesys {
namespace gl128 {

namespace {

struct Gl128DpiEntry {
    unsigned dpi;
    unsigned asic_dpi;
    unsigned output_pixel_offset;
    unsigned dummy_pixel;   // 0x2b, image pass
    unsigned pixel_clock;   // 0xa5/0xab, image pass
};

// Per-resolution sensor programming (pyopticfilm OUTPUT_PIXEL_OFFSET,
// DUMMY_BY_DPI, PIXEL_CLOCK_BY_DPI). 150 and 300 dpi scan at 600 dpi and are
// block-averaged on the host. DPISET is always asic_dpi / 6.
constexpr Gl128DpiEntry GL128_DPI_TABLE[] = {
    { 150,  600,  10, 0x01, 0x02 },
    { 300,  600,  10, 0x01, 0x02 },
    { 600,  600,  10, 0x01, 0x02 },
    { 720,  720,  12, 0x01, 0x02 },
    { 900,  900,  15, 0x01, 0x02 },
    { 1200, 1200, 20, 0x02, 0x02 },
    { 1440, 1440, 24, 0x02, 0x02 },
    { 1800, 1800, 30, 0x02, 0x02 },
    { 2400, 2400, 40, 0x03, 0x01 },
    { 3600, 3600, 60, 0x04, 0x01 },
    { 7200, 7200, 120, 0x17, 0x01 },
};

const Gl128DpiEntry& gl128_dpi_entry(unsigned dpi)
{
    for (const auto& e : GL128_DPI_TABLE) {
        if (e.dpi == dpi) {
            return e;
        }
    }
    throw SaneException("GL128: unsupported resolution %u", dpi);
}

} // namespace

unsigned gl128_host_downsample(unsigned xres)
{
    return std::max(1u, gl128_dpi_entry(xres).asic_dpi / xres);
}

namespace {

// Like Genesys_Register_Set::set16()/set24() (MSB first), but adding the
// registers if the set does not hold them yet.
void gl128_init_reg16(Genesys_Register_Set* reg, std::uint16_t address, std::uint16_t value)
{
    reg->init_reg(address, static_cast<std::uint8_t>((value >> 8) & 0xff));
    reg->init_reg(static_cast<std::uint16_t>(address + 1), static_cast<std::uint8_t>(value & 0xff));
}

void gl128_init_reg24(Genesys_Register_Set* reg, std::uint16_t address, std::uint32_t value)
{
    reg->init_reg(address, static_cast<std::uint8_t>((value >> 16) & 0xff));
    reg->init_reg(static_cast<std::uint16_t>(address + 1), static_cast<std::uint8_t>((value >> 8) & 0xff));
    reg->init_reg(static_cast<std::uint16_t>(address + 2), static_cast<std::uint8_t>(value & 0xff));
}


} // namespace

// --- AFE strip helpers ----------------------------------------------------
// Used by gl128_run_asic_shading() for the AFE probe and relight strips.
namespace {

// Stationary AFE strip: STRPIXEL 64 to ENDPIXEL 576, 512 px of 16-bit RGB.
constexpr unsigned GL128_AFE_STRIP_PIXELS = 512;
constexpr unsigned GL128_AFE_STRIP_BYTES = GL128_AFE_STRIP_PIXELS * 3 * 2;

// Mean R/G/B of a 16-bit LE strip, planar (RRR...GGG...BBB...) or
// interleaved (RGBRGB...).
std::array<double, 3> gl128_channel_means_u16(const std::vector<std::uint8_t>& strip,
                                              unsigned pixels, bool planar)
{
    const std::size_t expected = static_cast<std::size_t>(pixels) * 3 * 2;
    if (strip.size() < expected) {
        throw SaneException("GL128: AFE strip too short: %zu < %zu",
                            strip.size(), expected);
    }
    double sums[3] = {0.0, 0.0, 0.0};
    if (planar) {
        const std::size_t plane = static_cast<std::size_t>(pixels) * 2;
        for (int c = 0; c < 3; c++) {
            const std::size_t base = static_cast<std::size_t>(c) * plane;
            for (unsigned i = 0; i < pixels; i++) {
                std::size_t off = base + static_cast<std::size_t>(i) * 2;
                sums[c] += static_cast<double>(strip[off] | (strip[off + 1] << 8));
            }
        }
    } else {
        for (unsigned i = 0; i < pixels; i++) {
            std::size_t base = static_cast<std::size_t>(i) * 6;
            for (int c = 0; c < 3; c++) {
                std::size_t off = base + static_cast<std::size_t>(c) * 2;
                sums[c] += static_cast<double>(strip[off] | (strip[off + 1] << 8));
            }
        }
    }
    return { sums[0] / pixels, sums[1] / pixels, sums[2] / pixels };
}


// Lamp control: single writes of 0x03 (and 0x37 for infrared).
void gl128_apply_infrared(Genesys_Device* dev, bool enabled)
{
    // 0x37 is never read-modify-written: its readback differs from what was
    // written (SilverFast writes 0xc0 at boot and reads back 0xb0). SilverFast
    // does not write 0x37 during a colour scan. Infrared (not supported yet)
    // would be the boot value | IR_LED.
    dev->interface->write_register(REG_IR, static_cast<std::uint8_t>(enabled ? (0xc0 | IR_LED) : 0xc0));
}

void gl128_lamp_on(Genesys_Device* dev, bool infrared)
{
    // Also kept in dev->reg: sanei_genesys_set_lamp_power() modifies 0x03
    // there and needs the register to exist.
    if (infrared) {
        dev->interface->write_register(REG_0x03, REG_0x03_XPASEL);
        dev->reg.init_reg(REG_0x03, REG_0x03_XPASEL);
        gl128_apply_infrared(dev, true);
    } else {
        std::uint8_t val = static_cast<std::uint8_t>(REG_0x03_XPASEL | REG_0x03_LAMPPWR);
        dev->interface->write_register(REG_0x03, val);
        dev->reg.init_reg(REG_0x03, val);
        // No 0x37 write for colour: SilverFast writes 0x37 only at boot.
    }
}

void gl128_lamp_off(Genesys_Device* dev)
{
    // Only 0x03, as SilverFast does before its dark strip (0x03=0x20). Also
    // writing 0x37 kept the lamp lit through the dark strip.
    dev->interface->write_register(REG_0x03, REG_0x03_XPASEL);
    dev->reg.init_reg(REG_0x03, REG_0x03_XPASEL);
}

} // namespace

// --- Stationary strips ------------------------------------------------------
// Reading a strip without moving the carriage (AFE probes, dark reference).
namespace {

constexpr unsigned GL128_AFE_STRPIXEL = 0x40;       // 64
constexpr unsigned GL128_AFE_ENDPIXEL = 0x240;       // 576 -> 512 px
constexpr unsigned GL128_AFE_WIDE_BYTES = 62268;
constexpr unsigned GL128_AFE_WIDE_PIXELS = GL128_AFE_WIDE_BYTES / 6; // 10378
constexpr unsigned GL128_AFE_WIDE_ENDPIXEL = GL128_AFE_STRPIXEL + GL128_AFE_WIDE_PIXELS; // 10442

constexpr double GL128_STATIONARY_IDLE_WAIT_S = 2.0;
constexpr int GL128_STATIONARY_START_RETRIES = 1;
constexpr std::uint8_t GL128_CANCEL_REG01 = 0x22;

// Status bytes seen in the captures when stationary data is ready.
bool gl128_is_stationary_data_ready_code(std::uint8_t status_byte)
{
    return status_byte == 0xBD || status_byte == 0xA9 ||
           status_byte == 0xAD || status_byte == 0x9C;
}

// Stationary AFE strip registers, motor off: the capture's calibration
// window at DPISET 1200 with dummy 0x04 and clocks 0x01/0x30. The memory
// layout registers (0xd0-0xf8) keep their asic_boot() values.
void gl128_setup_afe_strip_regs(Genesys_Device* dev, const Genesys_Sensor& sensor, bool wide)
{
    // Sensor registers, except lamp (0x03) and IR (0x37).
    for (const auto& r : sensor.custom_regs) {
        if (r.address == REG_0x03 || r.address == REG_IR) {
            continue;
        }
        dev->interface->write_register(r.address, static_cast<std::uint8_t>(r.value));
    }

    constexpr unsigned afe_clock_dpi = 1200;
    constexpr std::uint8_t dummy = 0x04;
    constexpr std::uint8_t clk_a = 0x01;
    constexpr std::uint8_t clk_b = 0x30;
    (void) afe_clock_dpi;

    dev->interface->write_register(0x2B, dummy);
    dev->interface->write_register(0xA5, clk_a);
    dev->interface->write_register(0xAB, clk_b);
    dev->interface->write_register(0xA3, 0x01);

    const unsigned end = wide ? GL128_AFE_WIDE_ENDPIXEL : GL128_AFE_ENDPIXEL;
    gl128_init_reg24(&dev->reg, REG_LINCNT, 1);
    // DPISET = 7200 / 6: the AFE strips always run at native resolution.
    gl128_init_reg16(&dev->reg, REG_DPISET, static_cast<std::uint16_t>(sensor.full_resolution / 6));
    gl128_init_reg24(&dev->reg, REG_STRPIXEL, GL128_AFE_STRPIXEL);
    gl128_init_reg24(&dev->reg, REG_ENDPIXEL, end);
    gl128_init_reg24(&dev->reg, REG_FEEDL, 1);
    gl128_init_reg24(&dev->reg, REG_LPERIOD, 14000);
    gl128_init_reg24(&dev->reg, REG_EXPOSURE, 14000);
    dev->interface->write_register(REG_DEPTH_A, DEPTH16_A);
    dev->interface->write_register(REG_DEPTH_B, DEPTH16_B);

    // No motor: keep 0x02 clear of MTRPWR/AGOHOME/FASTFED.
    dev->interface->write_register(REG_0x02, 0x00);
    dev->reg.init_reg(REG_0x02, 0x00);

    // SHDAREA, no SCAN yet (set by the start sequence).
    std::uint8_t reg01 = dev->reg.get8(REG_0x01);
    reg01 = (reg01 | REG_0x01_SHDAREA) & ~REG_0x01_SCAN & ~REG_0x01_DVDSET;
    dev->interface->write_register(REG_0x01, reg01);
    dev->reg.init_reg(REG_0x01, reg01);

    // gl128_init_reg16/24() only update dev->reg; write the window out.
    for (std::uint16_t addr : { REG_LINCNT, static_cast<std::uint16_t>(REG_LINCNT + 1),
                                static_cast<std::uint16_t>(REG_LINCNT + 2),
                                REG_DPISET, static_cast<std::uint16_t>(REG_DPISET + 1),
                                REG_STRPIXEL, static_cast<std::uint16_t>(REG_STRPIXEL + 1),
                                static_cast<std::uint16_t>(REG_STRPIXEL + 2),
                                REG_ENDPIXEL, static_cast<std::uint16_t>(REG_ENDPIXEL + 1),
                                static_cast<std::uint16_t>(REG_ENDPIXEL + 2),
                                REG_FEEDL, static_cast<std::uint16_t>(REG_FEEDL + 1),
                                static_cast<std::uint16_t>(REG_FEEDL + 2),
                                REG_LPERIOD, static_cast<std::uint16_t>(REG_LPERIOD + 1),
                                static_cast<std::uint16_t>(REG_LPERIOD + 2),
                                REG_EXPOSURE, static_cast<std::uint16_t>(REG_EXPOSURE + 1),
                                static_cast<std::uint16_t>(REG_EXPOSURE + 2) }) {
        dev->interface->write_register(addr, dev->reg.get8(addr));
    }
}

// Start sequence from the captures: motor off, CLRCNT, 0x01 written from the
// cached value (a read-modify-write can pick up SCAN/DVDSET left by the
// previous image pass, and then START never fills the buffer), START.
void gl128_start_stationary_strip(Genesys_Device* dev)
{
    const std::uint8_t motor_mask = REG_0x02_MTRPWR | REG_0x02_AGOHOME | REG_0x02_FASTFED;
    dev->interface->write_register(REG_0x02, 0x00);
    std::uint8_t reg02 = dev->interface->read_register(REG_0x02);
    if (reg02 & motor_mask) {
        dev->interface->write_register(REG_0x02, 0x00);
    }
    dev->interface->write_register(REG_CLRCNT, CLRCNT_ALL);
    std::uint8_t reg01 = (dev->reg.get8(REG_0x01) | REG_0x01_SHDAREA | REG_0x01_SCAN) &
                         ~REG_0x01_DVDSET;
    dev->interface->write_register(REG_0x01, reg01);
    dev->reg.init_reg(REG_0x01, reg01);
    dev->interface->write_register(REG_START, START_GO);
}

// Poll 0x101 until strip data is ready. Stationary: HOME and not BUFEMPTY,
// or a known ready code (rejects motor-busy 0xa5, which lacks HOME).
// Motorized (white strip): any not-BUFEMPTY, since the carriage leaves home.
void gl128_wait_shading_data_ready(Genesys_Device* dev, double timeout_s, bool motorized,
                                   const char* where)
{
    scanner_read_status(*dev);
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(static_cast<long>(timeout_s * 1000));
    std::uint8_t last = 0xff;
    while (std::chrono::steady_clock::now() < deadline) {
        Status status = scanner_read_status(*dev);
        last = dev->interface->read_register(REG_STATUS);
        if (motorized) {
            if (!status.is_buffer_empty) {
                return;
            }
        } else if (gl128_is_stationary_data_ready_code(last) ||
                  (!status.is_buffer_empty && status.is_at_home)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw SaneException("GL128: %s: no %s data ready within %.0fs (last status=0x%02x)",
                        where, motorized ? "motorized" : "stationary", timeout_s, last);
}

// Abort a leftover image SCAN so stationary geometry writes stick. A park
// leaves 0x02 armed (MTRPWR|AGOHOME); the next stationary START with AGOHOME
// still set hangs with status 0xcd. SCAN and motor bits are read back from
// the hardware each poll, since dev->reg drops AGOHOME after a park.
void gl128_reset_stationary_scan_engine(Genesys_Device* dev, const char* where)
{
    (void) where;
    const std::uint8_t motor_mask = REG_0x02_MTRPWR | REG_0x02_AGOHOME | REG_0x02_FASTFED;
    dev->interface->write_register(REG_START, 0x00);
    dev->interface->write_register(REG_0x01, GL128_CANCEL_REG01);
    dev->interface->write_register(REG_0x02, 0x00);
    dev->interface->write_register(REG_CLRCNT, CLRCNT_ALL);

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(
                        static_cast<long>(GL128_STATIONARY_IDLE_WAIT_S * 1000));
    std::uint8_t last01 = 0, last02 = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        last01 = dev->interface->read_register(REG_0x01);
        last02 = dev->interface->read_register(REG_0x02);
        Status status = scanner_read_status(*dev);
        bool scan_clear = (last01 & REG_0x01_SCAN) == 0;
        bool motor_clear = (last02 & motor_mask) == 0;
        bool idle = status.is_at_home && !status.is_motor_enabled;
        if (scan_clear && motor_clear && idle) {
            dev->reg.init_reg(REG_0x01, last01);
            dev->reg.init_reg(REG_0x02, last02);
            if (last01 & REG_0x01_DVDSET) {
                std::uint8_t fixed = (last01 | REG_0x01_SHDAREA) & ~REG_0x01_SCAN &
                                     ~REG_0x01_DVDSET;
                dev->interface->write_register(REG_0x01, fixed);
            }
            return;
        }
        if (last01 & REG_0x01_SCAN) {
            std::uint8_t fixed = (last01 | REG_0x01_SHDAREA) & ~REG_0x01_SCAN &
                                 ~REG_0x01_DVDSET;
            dev->interface->write_register(REG_0x01, fixed);
        }
        if (last02 & motor_mask) {
            dev->interface->write_register(REG_0x02, 0x00);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // A timed-out reset is not fatal: the caller retries the strip.
}

// Read one stationary 16-bit AFE strip. Does not move the carriage.
std::vector<std::uint8_t> gl128_acquire_afe_strip(Genesys_Device* dev,
                                                  const Genesys_Sensor& sensor,
                                                  unsigned size)
{
    gl128_reset_stationary_scan_engine(dev, "AFE strip");
    gl128_setup_afe_strip_regs(dev, sensor, size >= GL128_AFE_WIDE_BYTES);

    bool started = false;
    SaneException last_exc(SANE_STATUS_INVAL);
    bool have_exc = false;
    for (int attempt = 0; attempt <= GL128_STATIONARY_START_RETRIES; attempt++) {
        if (attempt) {
            gl128_reset_stationary_scan_engine(dev, "AFE strip retry");
        }
        gl128_start_stationary_strip(dev);
        try {
            gl128_wait_shading_data_ready(dev, 5.0, false, "AFE strip");
            started = true;
            break;
        } catch (const SaneException& exc) {
            last_exc = exc;
            have_exc = true;
            gl128_reset_stationary_scan_engine(dev, "AFE strip fail");
        }
    }
    if (!started) {
        if (have_exc) {
            throw last_exc;
        }
        throw SaneException("GL128: AFE strip start failed");
    }

    std::vector<std::uint8_t> buf(size);
    // addr is ignored: bulk_read_data_send_header() uses the AHB address
    // 0x10000000 for GL124-family chips.
    dev->interface->bulk_read_data(0, buf.data(), size);

    std::uint8_t reg01 = dev->reg.get8(REG_0x01);
    dev->interface->write_register(REG_0x01, static_cast<std::uint8_t>(reg01 & ~REG_0x01_SCAN));
    dev->reg.init_reg(REG_0x01, static_cast<std::uint8_t>(reg01 & ~REG_0x01_SCAN));

    return buf;
}


// Poll status until home and motor idle. Status is read twice: the first
// read after an operation can still show the previous state.
void gl128_wait_until_at_home(Genesys_Device* dev, double timeout_s)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(static_cast<long>(timeout_s * 1000));
    while (std::chrono::steady_clock::now() < deadline) {
        scanner_read_status(*dev); // stale first read
        Status status = scanner_read_status(*dev);
        if (status.is_at_home && !status.is_motor_enabled) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw SaneException("GL128: carriage did not reach home within %.0fs (AGOHOME park)",
                        timeout_s);
}

// End or abort an image pass as SilverFast does. With AGOHOME armed: lamp
// strobe, 0x01=0x22, second strobe, wait for the park (up to 60 s). Without
// AGOHOME (e.g. a feed timeout): clear SCAN only; no capture shows a
// mid-feed abort.
void gl128_stop_motor(Genesys_Device* dev)
{
    static constexpr std::uint8_t CANCEL_LAMP_PRE[] = { 0x30, 0x20 };
    static constexpr std::uint8_t CANCEL_LAMP_POST[] = { 0x10, 0x00, 0x20, 0x30, 0x20, 0x30 };
    static constexpr std::uint8_t CANCEL_REG01 = 0x22;

    std::uint8_t reg02 = dev->reg.get8(REG_0x02);
    if (reg02 & REG_0x02_AGOHOME) {
        for (std::uint8_t v : CANCEL_LAMP_PRE) {
            dev->interface->write_register(REG_0x03, v);
        }
        dev->interface->write_register(REG_0x01, CANCEL_REG01);
        dev->reg.init_reg(REG_0x01, CANCEL_REG01);
        for (std::uint8_t v : CANCEL_LAMP_POST) {
            dev->interface->write_register(REG_0x03, v);
        }
        // The captures leave 0x02 alone after a cancel. Dropping AGOHOME from
        // dev->reg stops a second call (end_scan(), then sane_close()) from
        // strobing and waiting again.
        gl128_wait_until_at_home(dev, 60.0);
        dev->reg.init_reg(REG_0x02, static_cast<std::uint8_t>(reg02 & ~REG_0x02_AGOHOME));
    } else {
        std::uint8_t reg01 = dev->reg.get8(REG_0x01);
        reg01 = static_cast<std::uint8_t>(reg01 & ~REG_0x01_SCAN);
        dev->interface->write_register(REG_0x01, reg01);
        dev->reg.init_reg(REG_0x01, reg01);
    }
}

unsigned gl128_lperiod_for_dpi(unsigned dpi)
{
    // pyopticfilm LPERIOD_BY_DPI; 1800 dpi from SilverFast's own 1800 dpi
    // capture (the pyopticfilm table has 11490).
    static constexpr struct { unsigned dpi; unsigned lperiod; } TABLE[] = {
        { 150, 11064 }, { 300, 11064 }, { 600, 11064 }, { 720, 11106 },
        { 900, 11170 }, { 1200, 11277 }, { 1440, 11362 }, { 1800, 11206 },
        { 2400, 11703 }, { 3600, 13407 }, { 7200, 15963 },
    };
    for (const auto& e : TABLE) {
        if (e.dpi == dpi) {
            return e.lperiod;
        }
    }
    throw SaneException("GL128: no LPERIOD for %u dpi", dpi);
}

// 0x2b (dummy) and 0xa5/0xab (pixel clocks) for one pass.
struct Gl128PassClocks {
    unsigned dummy;
    unsigned clk_a;
    unsigned clk_b;
};

// Line timing for one scan: LPERIOD (shared by the dark, white and image
// passes, as in every SilverFast capture) and the per-pass clocks.
struct Gl128Timing {
    unsigned lperiod;
    Gl128PassClocks image;
    Gl128PassClocks dark;
    Gl128PassClocks white;
};

// width_native = ENDPIXEL - STRPIXEL (7200 dpi units).
Gl128Timing gl128_timing(unsigned asic_dpi, unsigned width_native)
{
    // 7200 dpi: SilverFast sets the line timing from the window width. Four
    // captures (window widths 1416, 2832, 5664 and 10200 native pixels, END
    // 10610) give LPERIOD = 10851 + width / 2 exactly; the dummy and clock
    // bytes follow no formula, so the captured programs are used as a table.
    // A window uses the narrowest program at least as wide as itself.
    struct Program {
        unsigned width;
        Gl128Timing timing;
    };
    static constexpr Program PROGRAMS_7200[] = {
        //  width   LPERIOD  image            dark               white
        {  1416, { 11559, { 0x03, 1, 1 }, { 0x04, 1, 0x30 }, { 0x02, 2, 2 } } },
        {  2832, { 12267, { 0x05, 1, 1 }, { 0x07, 1, 0x30 }, { 0x03, 2, 2 } } },
        {  5664, { 13683, { 0x09, 1, 1 }, { 0x0d, 1, 0x30 }, { 0x09, 1, 1 } } },
        { 10200, { 15951, { 0x17, 1, 1 }, { 0x17, 1, 0x30 }, { 0x0f, 1, 1 } } },
    };
    if (asic_dpi == 7200) {
        for (const auto& p : PROGRAMS_7200) {
            if (width_native <= p.width) {
                return p.timing;
            }
        }
        // Wider than any SilverFast window: the full-width values below
        // (verified on hardware at 36.58 mm).
    }

    const Gl128DpiEntry& e = gl128_dpi_entry(asic_dpi);
    const Gl128PassClocks image{ e.dummy_pixel, e.pixel_clock, e.pixel_clock };
    Gl128Timing t{ gl128_lperiod_for_dpi(asic_dpi), image, image, image };
    // Dark strip clocks at 1200 and 1800 dpi (pyopticfilm SHADING_DARK_*;
    // dummy 0x04 at 1800 from SilverFast's 1800 dpi capture).
    if (asic_dpi == 1200 || asic_dpi == 1800) {
        t.dark = { 0x04, 0x01, 0x30 };
    }
    return t;
}

// Registers for genesys' host-side shading passes, built into `regs` only:
// genesys_shading_calibration_impl() copies dev->reg into its local set before
// calling init_regs_for_shading() and writes that set out afterwards.
// Window, DPISET and LPERIOD of the image scan, FEEDL 1, SHDAREA (SCAN is set
// by begin_scan()), as in SilverFast's shading passes.
void gl128_setup_shading_strip_regs(const Genesys_Sensor& sensor, const ScanSession& real_session,
                                    unsigned lincnt, Genesys_Register_Set& regs)
{
    const unsigned dpi = real_session.params.xres;
    const Gl128DpiEntry& dpi_entry = gl128_dpi_entry(dpi);

    for (const auto& r : sensor.custom_regs) {
        if (r.address == REG_0x03 || r.address == REG_IR) {
            continue;
        }
        regs.init_reg(r.address, static_cast<std::uint8_t>(r.value));
    }
    const Gl128Timing timing = gl128_timing(dpi_entry.asic_dpi,
                                           real_session.pixel_endx - real_session.pixel_startx);
    regs.init_reg(0x2B, static_cast<std::uint8_t>(timing.image.dummy));
    regs.init_reg(0xA5, static_cast<std::uint8_t>(timing.image.clk_a));
    regs.init_reg(0xAB, static_cast<std::uint8_t>(timing.image.clk_b));

    gl128_init_reg24(&regs, REG_LINCNT, lincnt);
    gl128_init_reg16(&regs, REG_DPISET, static_cast<std::uint16_t>(dpi_entry.asic_dpi / 6));
    gl128_init_reg24(&regs, REG_STRPIXEL, real_session.pixel_startx);
    gl128_init_reg24(&regs, REG_ENDPIXEL, real_session.pixel_endx);
    gl128_init_reg24(&regs, REG_FEEDL, 1);
    gl128_init_reg24(&regs, REG_LPERIOD, timing.lperiod);
    gl128_init_reg24(&regs, REG_EXPOSURE, 14000);
    // Same depth registers as the image pass (chunky 16-bit RGB), so genesys
    // can average the data as interleaved RGB. SilverFast's DEPTH16 setting
    // returns planar lines.
    regs.init_reg(REG_DEPTH_A, DEPTH8_A);
    regs.init_reg(REG_DEPTH_B, DEPTH8_B);

    // Motor off: both passes are stationary here (see begin_scan()).
    regs.init_reg(REG_0x02, 0x00);
    regs.init_reg(REG_0x01, static_cast<std::uint8_t>(REG_0x01_SHDAREA));
}


// --- Motor slope and channel exposure tables (AHB) --------------------------
// Byte-exact from the SilverFast capture. The fast slope goes to both
// 0x1000c000 (scan) and 0x10010000 (fast); one exposure table goes to each of
// 0x10000000/0x10004000/0x10008000 (R/G/B). Without a slope table the motor
// has no ramp and does not advance.
constexpr std::uint8_t GL128_SLOPE_TABLE_FAST_BYTES[512] = {
    0xde, 0x16, 0xdb, 0x06, 0x40, 0x05, 0x7e, 0x04, 0x06, 0x04, 0xb1, 0x03, 0x71, 0x03, 0x3e, 0x03,
    0x14, 0x03, 0xf2, 0x02, 0xd4, 0x02, 0xba, 0x02, 0xa3, 0x02, 0x8e, 0x02, 0x7c, 0x02, 0x6b, 0x02,
    0x5c, 0x02, 0x4e, 0x02, 0x41, 0x02, 0x35, 0x02, 0x2a, 0x02, 0x20, 0x02, 0x16, 0x02, 0x0d, 0x02,
    0x05, 0x02, 0xfd, 0x01, 0xf5, 0x01, 0xee, 0x01, 0xe7, 0x01, 0xe0, 0x01, 0xda, 0x01, 0xd4, 0x01,
    0xce, 0x01, 0xc9, 0x01, 0xc4, 0x01, 0xbf, 0x01, 0xba, 0x01, 0xb5, 0x01, 0xb1, 0x01, 0xac, 0x01,
    0xa8, 0x01, 0xa4, 0x01, 0xa0, 0x01, 0x9d, 0x01, 0x99, 0x01, 0x95, 0x01, 0x92, 0x01, 0x8f, 0x01,
    0x8c, 0x01, 0x88, 0x01, 0x85, 0x01, 0x82, 0x01, 0x80, 0x01, 0x7d, 0x01, 0x7a, 0x01, 0x77, 0x01,
    0x75, 0x01, 0x72, 0x01, 0x70, 0x01, 0x6d, 0x01, 0x6b, 0x01, 0x69, 0x01, 0x66, 0x01, 0x64, 0x01,
    0x62, 0x01, 0x60, 0x01, 0x5e, 0x01, 0x5c, 0x01, 0x5a, 0x01, 0x58, 0x01, 0x56, 0x01, 0x54, 0x01,
    0x52, 0x01, 0x51, 0x01, 0x4f, 0x01, 0x4d, 0x01, 0x4b, 0x01, 0x4a, 0x01, 0x48, 0x01, 0x46, 0x01,
    0x45, 0x01, 0x43, 0x01, 0x42, 0x01, 0x40, 0x01, 0x3f, 0x01, 0x3d, 0x01, 0x3c, 0x01, 0x3b, 0x01,
    0x39, 0x01, 0x38, 0x01, 0x36, 0x01, 0x35, 0x01, 0x34, 0x01, 0x33, 0x01, 0x31, 0x01, 0x30, 0x01,
    0x2f, 0x01, 0x2e, 0x01, 0x2c, 0x01, 0x2b, 0x01, 0x2a, 0x01, 0x29, 0x01, 0x28, 0x01, 0x27, 0x01,
    0x26, 0x01, 0x25, 0x01, 0x24, 0x01, 0x22, 0x01, 0x21, 0x01, 0x20, 0x01, 0x1f, 0x01, 0x1e, 0x01,
    0x1d, 0x01, 0x1c, 0x01, 0x1b, 0x01, 0x1a, 0x01, 0x1a, 0x01, 0x19, 0x01, 0x18, 0x01, 0x17, 0x01,
    0x16, 0x01, 0x15, 0x01, 0x14, 0x01, 0x13, 0x01, 0x12, 0x01, 0x12, 0x01, 0x11, 0x01, 0x10, 0x01,
    0x0f, 0x01, 0x0e, 0x01, 0x0d, 0x01, 0x0d, 0x01, 0x0c, 0x01, 0x0b, 0x01, 0x0a, 0x01, 0x0a, 0x01,
    0x09, 0x01, 0x08, 0x01, 0x07, 0x01, 0x07, 0x01, 0x06, 0x01, 0x05, 0x01, 0x04, 0x01, 0x04, 0x01,
    0x03, 0x01, 0x02, 0x01, 0x02, 0x01, 0x01, 0x01, 0x00, 0x01, 0x00, 0x01, 0xff, 0x00, 0xfe, 0x00,
    0xfe, 0x00, 0xfd, 0x00, 0xfc, 0x00, 0xfc, 0x00, 0xfb, 0x00, 0xfb, 0x00, 0xfa, 0x00, 0xf9, 0x00,
    0xf9, 0x00, 0xf8, 0x00, 0xf8, 0x00, 0xf7, 0x00, 0xf6, 0x00, 0xf6, 0x00, 0xf5, 0x00, 0xf5, 0x00,
    0xf4, 0x00, 0xf4, 0x00, 0xf3, 0x00, 0xf2, 0x00, 0xf2, 0x00, 0xf1, 0x00, 0xf1, 0x00, 0xf0, 0x00,
    0xf0, 0x00, 0xef, 0x00, 0xef, 0x00, 0xee, 0x00, 0xee, 0x00, 0xed, 0x00, 0xed, 0x00, 0xec, 0x00,
    0xec, 0x00, 0xeb, 0x00, 0xeb, 0x00, 0xea, 0x00, 0xea, 0x00, 0xe9, 0x00, 0xe9, 0x00, 0xe8, 0x00,
    0xe8, 0x00, 0xe7, 0x00, 0xe7, 0x00, 0xe6, 0x00, 0xe6, 0x00, 0xe6, 0x00, 0xe5, 0x00, 0xe5, 0x00,
    0xe4, 0x00, 0xe4, 0x00, 0xe3, 0x00, 0xe3, 0x00, 0xe3, 0x00, 0xe2, 0x00, 0xe2, 0x00, 0xe1, 0x00,
    0xe1, 0x00, 0xe0, 0x00, 0xe0, 0x00, 0xe0, 0x00, 0xdf, 0x00, 0xdf, 0x00, 0xde, 0x00, 0xde, 0x00,
    0xde, 0x00, 0xdd, 0x00, 0xdd, 0x00, 0xdc, 0x00, 0xdc, 0x00, 0xdc, 0x00, 0xdb, 0x00, 0xdb, 0x00,
    0xda, 0x00, 0xda, 0x00, 0xda, 0x00, 0xd9, 0x00, 0xd9, 0x00, 0xd9, 0x00, 0xd8, 0x00, 0xd8, 0x00,
    0xd8, 0x00, 0xd7, 0x00, 0xd7, 0x00, 0xd6, 0x00, 0xd6, 0x00, 0xd6, 0x00, 0xd5, 0x00, 0xd5, 0x00,
    0xd5, 0x00, 0xd4, 0x00, 0xd4, 0x00, 0xd4, 0x00, 0xd3, 0x00, 0xd3, 0x00, 0xd3, 0x00, 0xd2, 0x00,
    0xd2, 0x00, 0xd2, 0x00, 0xd1, 0x00, 0xd1, 0x00, 0xd1, 0x00, 0xd0, 0x00, 0xd0, 0x00, 0xd0, 0x00,
};
constexpr std::uint8_t GL128_EXPOSURE_TABLE_600_BYTES[512] = {
    0xbc, 0x0d, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
    0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04, 0x8e, 0x04,
};

constexpr std::uint32_t GL128_AHB_CHANNEL_R = 0x10000000;
constexpr std::uint32_t GL128_AHB_CHANNEL_G = 0x10004000;
constexpr std::uint32_t GL128_AHB_CHANNEL_B = 0x10008000;
constexpr std::uint32_t GL128_AHB_SLOPE_SCAN = 0x1000c000;
constexpr std::uint32_t GL128_AHB_SLOPE_FAST = 0x10010000;

void gl128_upload_image_tables(Genesys_Device* dev, unsigned asic_dpi)
{
    // Per-channel exposure table (pyopticfilm exposure_table()): 256 u16
    // words of 14000 / (7200 / asic_dpi), entry 0 = that + 16. The value must
    // fit 16 bits. 600 dpi uses the table from the SilverFast capture
    // (entry 0 = 3516, body 1166).
    std::vector<std::uint8_t> expo;
    if (asic_dpi == 600) {
        expo.assign(std::begin(GL128_EXPOSURE_TABLE_600_BYTES),
                    std::end(GL128_EXPOSURE_TABLE_600_BYTES));
    } else {
        const unsigned oversample = std::max(1u, 7200u / asic_dpi);
        const unsigned body = 14000u / oversample;
        if (body > 0xffff) {
            throw SaneException("GL128: channel exposure %u does not fit 16 bits", body);
        }
        const unsigned first = (body + 16) & 0xffff;
        expo.resize(512);
        for (unsigned k = 0; k < 256; k++) {
            unsigned v = (k == 0) ? first : body;
            expo[k * 2] = static_cast<std::uint8_t>(v & 0xff);
            expo[k * 2 + 1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
        }
    }
    // No slope upload here: the fast ramp from the second positioning feed
    // stays in place for the image pass, as in the capture.
    for (std::uint32_t addr : { GL128_AHB_CHANNEL_R, GL128_AHB_CHANNEL_G, GL128_AHB_CHANNEL_B }) {
        dev->interface->write_ahb(addr, expo.size(), expo.data());
    }
}

// --- Positioning feed pair (from home to the scan-start line) --------------
// Replayed from the SilverFast capture (600 dpi):
//   feed 1: FEEDL 28292, slow slope, done when 0x21 reads 0x04
//   feed 2: FEEDL 13128 (top of the window), fast slope, done via 0x101
// Each slope goes to both slope slots. START is 0x0f=01 alone (no 0x0d
// counter clear). An image START at home with MOTORENB set hangs with
// status 0xcd.
constexpr std::uint8_t GL128_SLOPE_TABLE_SLOW_BYTES[512] = {
    0xb4, 0x1f, 0xff, 0x12, 0xa6, 0x0f, 0xfd, 0x0d, 0xea, 0x0c, 0x21, 0x0c, 0x8b, 0x0b, 0x0c, 0x0b,
    0xa6, 0x0a, 0x4f, 0x0a, 0x01, 0x0a, 0xbf, 0x09, 0x84, 0x09, 0x4f, 0x09, 0x1e, 0x09, 0xf1, 0x08,
    0xc8, 0x08, 0xa2, 0x08, 0x7f, 0x08, 0x60, 0x08, 0x41, 0x08, 0x25, 0x08, 0x09, 0x08, 0xf0, 0x07,
    0xd9, 0x07, 0xc1, 0x07, 0xad, 0x07, 0x98, 0x07, 0x84, 0x07, 0x71, 0x07, 0x60, 0x07, 0x4f, 0x07,
    0x3e, 0x07, 0x2f, 0x07, 0x1f, 0x07, 0x10, 0x07, 0x02, 0x07, 0xf5, 0x06, 0xe8, 0x06, 0xdb, 0x06,
    0xcf, 0x06, 0xc3, 0x06, 0xb7, 0x06, 0xac, 0x06, 0xa1, 0x06, 0x96, 0x06, 0x8c, 0x06, 0x82, 0x06,
    0x78, 0x06, 0x6f, 0x06, 0x66, 0x06, 0x5d, 0x06, 0x54, 0x06, 0x4b, 0x06, 0x43, 0x06, 0x3b, 0x06,
    0x33, 0x06, 0x2c, 0x06, 0x24, 0x06, 0x1c, 0x06, 0x15, 0x06, 0x0d, 0x06, 0x07, 0x06, 0x00, 0x06,
    0xf9, 0x05, 0xf3, 0x05, 0xec, 0x05, 0xe6, 0x05, 0xe0, 0x05, 0xd9, 0x05, 0xd4, 0x05, 0xce, 0x05,
    0xc8, 0x05, 0xc2, 0x05, 0xbd, 0x05, 0xb7, 0x05, 0xb2, 0x05, 0xad, 0x05, 0xa7, 0x05, 0xa2, 0x05,
    0x9d, 0x05, 0x98, 0x05, 0x93, 0x05, 0x8e, 0x05, 0x8a, 0x05, 0x86, 0x05, 0x81, 0x05, 0x7c, 0x05,
    0x78, 0x05, 0x73, 0x05, 0x6f, 0x05, 0x6b, 0x05, 0x66, 0x05, 0x62, 0x05, 0x5f, 0x05, 0x5a, 0x05,
    0x56, 0x05, 0x53, 0x05, 0x4e, 0x05, 0x4b, 0x05, 0x47, 0x05, 0x43, 0x05, 0x40, 0x05, 0x3c, 0x05,
    0x38, 0x05, 0x35, 0x05, 0x31, 0x05, 0x2e, 0x05, 0x2a, 0x05, 0x27, 0x05, 0x23, 0x05, 0x21, 0x05,
    0x1d, 0x05, 0x1a, 0x05, 0x16, 0x05, 0x14, 0x05, 0x10, 0x05, 0x0d, 0x05, 0x0a, 0x05, 0x07, 0x05,
    0x04, 0x05, 0x01, 0x05, 0xfe, 0x04, 0xfb, 0x04, 0xf8, 0x04, 0xf6, 0x04, 0xf3, 0x04, 0xf0, 0x04,
    0xed, 0x04, 0xea, 0x04, 0xe8, 0x04, 0xe5, 0x04, 0xe3, 0x04, 0xe0, 0x04, 0xdd, 0x04, 0xdb, 0x04,
    0xd8, 0x04, 0xd6, 0x04, 0xd3, 0x04, 0xd1, 0x04, 0xce, 0x04, 0xcc, 0x04, 0xca, 0x04, 0xc7, 0x04,
    0xc5, 0x04, 0xc2, 0x04, 0xc0, 0x04, 0xbe, 0x04, 0xbb, 0x04, 0xb9, 0x04, 0xb7, 0x04, 0xb5, 0x04,
    0xb3, 0x04, 0xb0, 0x04, 0xae, 0x04, 0xac, 0x04, 0xaa, 0x04, 0xa8, 0x04, 0xa5, 0x04, 0xa4, 0x04,
    0xa1, 0x04, 0x9f, 0x04, 0x9e, 0x04, 0x9b, 0x04, 0x99, 0x04, 0x97, 0x04, 0x95, 0x04, 0x94, 0x04,
    0x91, 0x04, 0x90, 0x04, 0x8e, 0x04, 0x8c, 0x04, 0x8a, 0x04, 0x88, 0x04, 0x86, 0x04, 0x84, 0x04,
    0x82, 0x04, 0x81, 0x04, 0x7f, 0x04, 0x7d, 0x04, 0x7b, 0x04, 0x79, 0x04, 0x78, 0x04, 0x76, 0x04,
    0x74, 0x04, 0x72, 0x04, 0x71, 0x04, 0x6f, 0x04, 0x6d, 0x04, 0x6c, 0x04, 0x6a, 0x04, 0x69, 0x04,
    0x67, 0x04, 0x65, 0x04, 0x64, 0x04, 0x62, 0x04, 0x61, 0x04, 0x5f, 0x04, 0x5d, 0x04, 0x5c, 0x04,
    0x5a, 0x04, 0x59, 0x04, 0x57, 0x04, 0x56, 0x04, 0x54, 0x04, 0x53, 0x04, 0x51, 0x04, 0x50, 0x04,
    0x4e, 0x04, 0x4d, 0x04, 0x4b, 0x04, 0x4a, 0x04, 0x48, 0x04, 0x47, 0x04, 0x46, 0x04, 0x44, 0x04,
    0x43, 0x04, 0x41, 0x04, 0x40, 0x04, 0x3e, 0x04, 0x3d, 0x04, 0x3c, 0x04, 0x3a, 0x04, 0x39, 0x04,
    0x37, 0x04, 0x36, 0x04, 0x35, 0x04, 0x34, 0x04, 0x32, 0x04, 0x31, 0x04, 0x30, 0x04, 0x2f, 0x04,
    0x2d, 0x04, 0x2c, 0x04, 0x2b, 0x04, 0x29, 0x04, 0x28, 0x04, 0x27, 0x04, 0x26, 0x04, 0x24, 0x04,
    0x23, 0x04, 0x22, 0x04, 0x21, 0x04, 0x1f, 0x04, 0x1e, 0x04, 0x1d, 0x04, 0x1c, 0x04, 0x1b, 0x04,
    0x19, 0x04, 0x18, 0x04, 0x17, 0x04, 0x16, 0x04, 0x15, 0x04, 0x14, 0x04, 0x12, 0x04, 0x11, 0x04,
};

constexpr unsigned GL128_FEED1_STEPS = 28292; // capture: 0x3d-0x3f = 00 6e 84
constexpr unsigned GL128_FEED2_STEPS = 13128; // capture: 0x3d-0x3f = 00 33 48
// End of the TA scan window (pyopticfilm scan_window_end_steps).
constexpr unsigned GL128_SCAN_WINDOW_END_STEPS = 27636;

// The second feed runs from the top of the window (13128) towards its end
// (27636) in proportion to tl_y / y_size_ta (pyopticfilm
// feed_to_scan_steps_for_area()). A SilverFast lower-half crop at 1800 dpi
// used 20232, all other registers unchanged.
unsigned gl128_feed2_steps(const Genesys_Device* dev)
{
    const float y_size = dev->model->y_size_ta;
    float y1 = y_size > 0.0f ? dev->settings.tl_y / y_size : 0.0f;
    y1 = std::max(0.0f, std::min(1.0f, y1));
    return static_cast<unsigned>(lround(GL128_FEED2_STEPS +
            y1 * static_cast<float>(GL128_SCAN_WINDOW_END_STEPS - GL128_FEED2_STEPS)));
}

std::uint8_t gl128_read_feed_probe(Genesys_Device* dev)
{
    // read_request_register(0x21): bRequest=0x0c, wValue=0x8e, wIndex=0x21.
    std::uint8_t val = 0;
    dev->interface->get_usb_device().control_msg(REQUEST_TYPE_IN, REQUEST_REGISTER,
                                                 VALUE_GET_REGISTER, 0x21, 1, &val);
    return val;
}

Status gl128_read_status_reliable(Genesys_Device* dev)
{
    scanner_read_status(*dev); // the first read can be stale
    return scanner_read_status(*dev);
}

void gl128_upload_slope(Genesys_Device* dev, const std::uint8_t (&table)[512])
{
    std::vector<std::uint8_t> slope(std::begin(table), std::end(table));
    dev->interface->write_ahb(GL128_AHB_SLOPE_SCAN, slope.size(), slope.data());
    dev->interface->write_ahb(GL128_AHB_SLOPE_FAST, slope.size(), slope.data());
}

void gl128_write_feedl(Genesys_Device* dev, unsigned steps)
{
    dev->interface->write_register(REG_FEEDL, static_cast<std::uint8_t>((steps >> 16) & 0xff));
    dev->interface->write_register(REG_FEEDL + 1, static_cast<std::uint8_t>((steps >> 8) & 0xff));
    dev->interface->write_register(REG_FEEDL + 2, static_cast<std::uint8_t>(steps & 0xff));
}

// Wait for a feed: first MOTORENB, then after a minimum run time 0x21 == 0x04
// or FEEDFSH with MOTORENB clear.
void gl128_wait_feed_done(Genesys_Device* dev, unsigned steps, double timeout_s)
{
    const double expected_s = 1.0 * (static_cast<double>(steps) / GL128_FEED1_STEPS) * 0.9;
    const double min_motion_s = std::min(0.25, std::max(0.05, expected_s));
    const auto t0 = std::chrono::steady_clock::now();
    const auto deadline = t0 + std::chrono::milliseconds(static_cast<long>(timeout_s * 1000));
    bool motion_seen = false;
    auto motion_t = t0;
    while (std::chrono::steady_clock::now() < deadline) {
        std::uint8_t probe = gl128_read_feed_probe(dev);
        Status st = gl128_read_status_reliable(dev);
        if (!motion_seen) {
            if (st.is_motor_enabled) {
                motion_seen = true;
                motion_t = std::chrono::steady_clock::now();
            }
        } else {
            double moved = std::chrono::duration<double>(std::chrono::steady_clock::now() - motion_t).count();
            if (moved >= min_motion_s &&
                (probe == 0x04 || (st.is_feeding_finished && !st.is_motor_enabled)))
            {
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // Timeout: clear SCAN and drop motor power (no AGOHOME park).
    std::uint8_t r01 = dev->interface->read_register(REG_0x01);
    dev->interface->write_register(REG_0x01, static_cast<std::uint8_t>(r01 & ~REG_0x01_SCAN));
    std::uint8_t r02 = dev->interface->read_register(REG_0x02);
    dev->interface->write_register(REG_0x02, static_cast<std::uint8_t>(r02 & ~REG_0x02_MTRPWR));
    throw SaneException("GL128: feed of %u steps did not finish within %.0fs (motion seen: %s)",
                        steps, timeout_s, motion_seen ? "yes" : "no");
}

// --- ASIC (hardware) shading -------------------------------------------------
// The ASIC applies an uploaded dark/gain table to the image when 0x01 has
// DVDSET, as SilverFast does; genesys' host-side correction is disabled for
// this model.

// Set by asic_boot() on a cold start; cleared once the lamp has warmed up.
bool g_gl128_lamp_cold = false;

constexpr std::uint32_t GL128_AHB_SHADING = 0x10014000;
constexpr unsigned GL128_SHADING_LINES = 128;
constexpr unsigned GL128_SHADING_BLOCK_DATA_PAIRS = 126;
constexpr unsigned GL128_SHADING_BLOCK_PAD_PAIRS = 2;   // 128 pairs per 512-byte block
constexpr unsigned GL128_SHADING_GAIN_UNITY = 0x2000;
constexpr unsigned GL128_SHADING_GAIN_TARGET = 0xFFFF;
constexpr unsigned GL128_SHADING_GAIN_MIN = GL128_SHADING_GAIN_UNITY / 4;
constexpr unsigned GL128_SHADING_GAIN_MAX = GL128_SHADING_GAIN_UNITY * 4;

using Gl128Rgb = std::array<unsigned, 3>;

unsigned gl128_declared_shading_size(unsigned entries)
{
    unsigned data = entries * 3;
    return 4 * (data + GL128_SHADING_BLOCK_PAD_PAIRS * (data / GL128_SHADING_BLOCK_DATA_PAIRS));
}

// Table layout: (dark, gain) u16 LE pairs per pixel and channel, two
// (last dark, 0) pad pairs after every 126 data pairs, zero-filled tail.
std::vector<std::uint8_t> gl128_pack_shading_table(const std::vector<Gl128Rgb>& dark,
                                                   const std::vector<Gl128Rgb>& gain,
                                                   unsigned declared_size)
{
    std::vector<std::uint8_t> out;
    auto put16 = [&out](unsigned v) {
        out.push_back(static_cast<std::uint8_t>(v & 0xff));
        out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    };
    unsigned pairs = 0;
    unsigned last_dark = 0;
    for (std::size_t i = 0; i < dark.size(); i++) {
        for (int c = 0; c < 3; c++) {
            if (pairs && pairs % GL128_SHADING_BLOCK_DATA_PAIRS == 0) {
                for (unsigned k = 0; k < GL128_SHADING_BLOCK_PAD_PAIRS; k++) {
                    put16(last_dark);
                    put16(0);
                }
            }
            last_dark = dark[i][c] & 0xffff;
            put16(last_dark);
            put16(gain[i][c] & 0xffff);
            pairs++;
        }
    }
    if (out.size() > declared_size) {
        throw SaneException("GL128: shading payload %zu exceeds declared size %u",
                            out.size(), declared_size);
    }
    out.resize(declared_size, 0);
    return out;
}

// Dark level per column parity. The CCD's two staggered rows have their own
// dark level: SilverFast's 7200 dpi shading tables hold one value per channel
// and parity (even 800/979/895, odd 1134/1285/1221), the even and odd column
// means of its dark strip. A single mean leaves vertical stripes in the
// shadows at 1440/2400/7200, where output columns alternate between the rows.
std::vector<Gl128Rgb> gl128_parity_dark(const std::vector<Gl128Rgb>& dark)
{
    double acc[2][3] = {};
    std::size_t n[2] = { 0, 0 };
    for (std::size_t x = 0; x < dark.size(); x++) {
        for (int c = 0; c < 3; c++) {
            acc[x % 2][c] += dark[x][c];
        }
        n[x % 2]++;
    }
    Gl128Rgb mean[2]{};
    for (int p = 0; p < 2; p++) {
        const std::size_t count = n[p] ? n[p] : n[1 - p];
        const double* sum = n[p] ? acc[p] : acc[1 - p];
        for (int c = 0; c < 3; c++) {
            mean[p][c] = static_cast<unsigned>(lround(sum[c] / std::max<std::size_t>(1, count)));
        }
    }
    std::vector<Gl128Rgb> out(dark.size());
    for (std::size_t x = 0; x < dark.size(); x++) {
        out[x] = mean[x % 2];
    }
    return out;
}

// Column means of chunky 16-bit RGB strip data.
std::vector<Gl128Rgb> gl128_average_columns_chunky(const std::vector<std::uint8_t>& raw,
                                                   unsigned pixels, unsigned lines)
{
    std::vector<double> sums(pixels * 3, 0.0);
    const std::size_t row = static_cast<std::size_t>(pixels) * 6;
    for (unsigned y = 0; y < lines; y++) {
        for (unsigned x = 0; x < pixels; x++) {
            for (int c = 0; c < 3; c++) {
                std::size_t off = y * row + x * 6 + c * 2;
                sums[x * 3 + c] += raw[off] | (raw[off + 1] << 8);
            }
        }
    }
    std::vector<Gl128Rgb> out(pixels);
    for (unsigned x = 0; x < pixels; x++) {
        for (int c = 0; c < 3; c++) {
            out[x][c] = static_cast<unsigned>(lround(sums[x * 3 + c] / lines));
        }
    }
    return out;
}

unsigned gl128_shading_gain_for_white(unsigned white_unity)
{
    double denom = std::max(1.0, static_cast<double>(white_unity));
    long gain = lround(static_cast<double>(GL128_SHADING_GAIN_TARGET) * GL128_SHADING_GAIN_UNITY / denom);
    gain = std::max<long>(GL128_SHADING_GAIN_MIN, std::min<long>(GL128_SHADING_GAIN_MAX, gain));
    return static_cast<unsigned>(gain);
}

struct Gl128ShadingWindow {
    unsigned strpixel;
    unsigned endpixel;
    unsigned dpiset;
    unsigned asic_dpi;
    unsigned pixels;
};

// Dark strip: stationary, lamp off. White strip: DVDSET, motor powered.
std::vector<std::uint8_t> gl128_acquire_shading_strip(Genesys_Device* dev,
                                                      const Gl128ShadingWindow& w, bool dvdset)
{
    const Gl128Timing timing = gl128_timing(w.asic_dpi, w.endpixel - w.strpixel);
    const Gl128PassClocks& clocks = dvdset ? timing.white : timing.dark;
    if (!dvdset) {
        gl128_reset_stationary_scan_engine(dev, "shading strip");
    }
    dev->interface->write_register(0x2B, static_cast<std::uint8_t>(clocks.dummy));
    dev->interface->write_register(0xA5, static_cast<std::uint8_t>(clocks.clk_a));
    dev->interface->write_register(0xAB, static_cast<std::uint8_t>(clocks.clk_b));
    auto w24 = [dev](std::uint16_t a, unsigned v) {
        dev->interface->write_register(a, static_cast<std::uint8_t>((v >> 16) & 0xff));
        dev->interface->write_register(a + 1, static_cast<std::uint8_t>((v >> 8) & 0xff));
        dev->interface->write_register(a + 2, static_cast<std::uint8_t>(v & 0xff));
    };
    w24(REG_LINCNT, GL128_SHADING_LINES);
    dev->interface->write_register(REG_DPISET, static_cast<std::uint8_t>((w.dpiset >> 8) & 0xff));
    dev->interface->write_register(REG_DPISET + 1, static_cast<std::uint8_t>(w.dpiset & 0xff));
    w24(REG_STRPIXEL, w.strpixel);
    w24(REG_ENDPIXEL, w.endpixel);
    w24(REG_FEEDL, 1);
    w24(REG_LPERIOD, timing.lperiod);
    w24(REG_EXPOSURE, 14000);
    dev->interface->write_register(REG_DEPTH_A, DEPTH16_A);
    dev->interface->write_register(REG_DEPTH_B, DEPTH16_B);

    std::uint8_t reg01;
    if (dvdset) {
        // As SilverFast: 0x02=0x10 (MTRPWR, no AGOHOME). The carriage stays
        // out (status 0xd4) and the positioning feeds start from there. An
        // AGOHOME park here left the motor unable to feed on cold starts.
        dev->interface->write_register(REG_0x02, static_cast<std::uint8_t>(REG_0x02_MTRPWR));
        dev->interface->write_register(0x3B, 0x00);
        dev->interface->write_register(0xA3, 0x00);
        reg01 = static_cast<std::uint8_t>((0x22 | REG_0x01_SHDAREA | REG_0x01_DVDSET) & ~REG_0x01_SCAN);
    } else {
        dev->interface->write_register(REG_0x02, 0x00);
        dev->interface->write_register(0xA3, 0x01);
        reg01 = static_cast<std::uint8_t>((0x22 | REG_0x01_SHDAREA) & ~REG_0x01_SCAN & ~REG_0x01_DVDSET);
    }
    dev->interface->write_register(REG_0x01, reg01);

    dev->interface->write_register(REG_CLRCNT, CLRCNT_ALL);
    reg01 = static_cast<std::uint8_t>(reg01 | REG_0x01_SCAN);
    dev->interface->write_register(REG_0x01, reg01);
    dev->interface->write_register(REG_START, START_GO);
    try {
        gl128_wait_shading_data_ready(dev, 30.0, dvdset, "shading strip");
    } catch (...) {
        dev->interface->write_register(REG_0x01, static_cast<std::uint8_t>(reg01 & ~REG_0x01_SCAN));
        if (dvdset) {
            dev->interface->write_register(REG_0x02, 0x00);
        }
        throw;
    }
    const std::size_t size = static_cast<std::size_t>(w.pixels) * GL128_SHADING_LINES * 6;
    std::vector<std::uint8_t> buf(size);
    dev->interface->bulk_read_data(0, buf.data(), size);

    // Clear SCAN, then drop motor power (SilverFast: status d4, no park).
    dev->interface->write_register(REG_0x01, static_cast<std::uint8_t>(reg01 & ~REG_0x01_SCAN));
    dev->interface->write_register(REG_0x02, 0x00);
    return buf;
}

// ASIC shading for a colour scan: AFE probes, dark strip, unity table, lamp
// on, white strip, measured table. The carriage must be at home.
void gl128_run_asic_shading(Genesys_Device* dev, const Genesys_Sensor& sensor,
                            const Gl128ShadingWindow& w)
{
    DBG_HELPER(dbg);
    Status st = gl128_read_status_reliable(dev);
    if (!st.is_at_home) {
        throw SaneException("GL128 ASIC shading: carriage is not at home. Power-cycle, then retry.");
    }
    // Clock/timing registers first, same values for shading, feeds and image.
    for (const auto& r : sensor.custom_regs) {
        if (r.address == REG_0x03 || r.address == REG_IR) {
            continue;
        }
        dev->interface->write_register(r.address, static_cast<std::uint8_t>(r.value));
        dev->reg.init_reg(r.address, static_cast<std::uint8_t>(r.value));
    }
    const unsigned declared = gl128_declared_shading_size(w.pixels);

    // Sequence, order and front-end codes as in SilverFast's 1800 dpi capture.
    auto apply_fe = [&](std::array<int, 3> offs, std::array<int, 3> gains) {
        for (int c = 0; c < 3; c++) {
            dev->frontend.set_offset(c, static_cast<std::uint16_t>(offs[c]));
            dev->frontend.set_gain(c, static_cast<std::uint16_t>(gains[c]));
        }
        dev->cmd_set->set_fe(dev, sensor, AFE_SET);
    };
    const std::array<int, 3> level_offs = { 39, 32, 39 }; // SilverFast shading offsets

    // Cold start: the lamp needs to warm up, or the white strip (62 ms after
    // switching on, as SilverFast) reads 0. Read stationary lamp-on strips
    // once a second until the brightest channel changes < 1% over three
    // readings (at least 5 s, at most 60 s).
    if (g_gl128_lamp_cold) {
        gl128_lamp_on(dev, false);
        apply_fe(level_offs, { 56, 69, 61 });
        double prev = 0.0;
        int stable = 0;
        for (int i = 0; i < 60; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            auto strip = gl128_acquire_afe_strip(dev, sensor, GL128_AFE_STRIP_BYTES);
            auto m = gl128_channel_means_u16(strip, GL128_AFE_STRIP_PIXELS, true);
            double peak = std::max({ m[0], m[1], m[2] });
            DBG(DBG_io, "GL128 cold lamp warm-up %ds: peak=%.0f\n", i + 1, peak);
            stable = (prev > 0.0 && std::abs(peak - prev) < 0.01 * peak) ? stable + 1 : 0;
            prev = peak;
            if (i >= 4 && stable >= 3) {
                break;
            }
        }
        g_gl128_lamp_cold = false;
    }

    // Fast slope to both slope slots, then calibration exposure tables
    // (entries 0 and 1 = 14000, rest 0) to R/G/B.
    gl128_upload_slope(dev, GL128_SLOPE_TABLE_FAST_BYTES);
    {
        std::vector<std::uint8_t> cal(512, 0);
        for (int k = 0; k < 2; k++) {
            cal[k * 2] = static_cast<std::uint8_t>(14000 & 0xff);
            cal[k * 2 + 1] = static_cast<std::uint8_t>(14000 >> 8);
        }
        for (std::uint32_t addr : { GL128_AHB_CHANNEL_R, GL128_AHB_CHANNEL_G, GL128_AHB_CHANNEL_B }) {
            dev->interface->write_ahb(addr, cal.size(), cal.data());
        }
    }

    // AFE probes with the lamp on (0x03=0x30):
    //   offsets 0,  gains 128        -> strip
    //   offsets 0,  gains 255        -> strip
    //   offsets 0,  gains 310/278/293 -> wide strip
    //   offsets 39/32/39, gains 128  -> strip
    //   offsets 39/32/39, gains 255  -> strip
    //   gains 19/26/24
    gl128_lamp_on(dev, false);
    // GPO 0xa7 and 0xa9 are 1 for the probes and both strips; the feed
    // registers set them back to 0.
    dev->interface->write_register(0xA7, 0x01);
    dev->interface->write_register(0xA9, 0x01);
    apply_fe({ 0, 0, 0 }, { 128, 128, 128 });
    gl128_acquire_afe_strip(dev, sensor, GL128_AFE_STRIP_BYTES);
    apply_fe({ 0, 0, 0 }, { 255, 255, 255 });
    gl128_acquire_afe_strip(dev, sensor, GL128_AFE_STRIP_BYTES);
    apply_fe({ 0, 0, 0 }, { 310, 278, 293 });
    gl128_acquire_afe_strip(dev, sensor, GL128_AFE_WIDE_BYTES);
    apply_fe(level_offs, { 128, 128, 128 });
    gl128_acquire_afe_strip(dev, sensor, GL128_AFE_STRIP_BYTES);
    apply_fe(level_offs, { 255, 255, 255 });
    gl128_acquire_afe_strip(dev, sensor, GL128_AFE_STRIP_BYTES);
    apply_fe(level_offs, { 19, 26, 24 });

    // Lamp off (0x03=0x20); the dark strip starts 0.34 s later, gains
    // 50/62/55.
    gl128_lamp_off(dev);
    std::this_thread::sleep_for(std::chrono::milliseconds(340));
    apply_fe(level_offs, { 50, 62, 55 });
    auto dark_raw = gl128_acquire_shading_strip(dev, w, false);
    auto dark = gl128_average_columns_chunky(dark_raw, w.pixels, GL128_SHADING_LINES);
    auto dark_flat = gl128_parity_dark(dark);
    DBG(DBG_info, "GL128 shading dark even=(%u,%u,%u) odd=(%u,%u,%u)\n",
        dark_flat[0][0], dark_flat[0][1], dark_flat[0][2],
        dark_flat[dark_flat.size() > 1 ? 1 : 0][0], dark_flat[dark_flat.size() > 1 ? 1 : 0][1],
        dark_flat[dark_flat.size() > 1 ? 1 : 0][2]);

    // Unity table, lamp on, gains 56/69/61, image exposure tables, white
    // strip.
    std::vector<Gl128Rgb> unity_gain(w.pixels, Gl128Rgb{ GL128_SHADING_GAIN_UNITY, GL128_SHADING_GAIN_UNITY,
                                                          GL128_SHADING_GAIN_UNITY });
    auto unity = gl128_pack_shading_table(dark_flat, unity_gain, declared);

    // Wait for the lamp before the white strip. SilverFast starts it 62 ms
    // after switching the lamp on, but after a power cycle the lamp is not
    // lit that soon and the white strip reads <= dark. Read stationary strips
    // until lit and steady (< 2%), at most 10 s. The unity table is uploaded
    // afterwards, since strip data lands in the same AHB RAM.
    gl128_lamp_on(dev, false);
    apply_fe(level_offs, { 56, 69, 61 });
    {
        double prev = 0.0;
        for (int i = 0; i < 20; i++) {
            auto strip = gl128_acquire_afe_strip(dev, sensor, GL128_AFE_STRIP_BYTES);
            auto m = gl128_channel_means_u16(strip, GL128_AFE_STRIP_PIXELS, true);
            double peak = std::max({ m[0], m[1], m[2] });
            DBG(DBG_io, "GL128 relight %d: peak=%.0f\n", i, peak);
            if (peak > 30000.0 && prev > 0.0 && std::abs(peak - prev) < 0.02 * peak) {
                break;
            }
            prev = peak;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        gl128_reset_stationary_scan_engine(dev, "post relight");
    }
    dev->interface->write_ahb(GL128_AHB_SHADING, unity.size(), unity.data());
    std::array<int, 3> white_gains = { 56, 69, 61 };
    apply_fe(level_offs, white_gains);
    gl128_upload_image_tables(dev, w.asic_dpi);
    auto white_raw = gl128_acquire_shading_strip(dev, w, true);
    auto white = gl128_average_columns_chunky(white_raw, w.pixels, GL128_SHADING_LINES);

    std::vector<Gl128Rgb> gains(w.pixels);
    for (unsigned x = 0; x < w.pixels; x++) {
        for (int c = 0; c < 3; c++) {
            gains[x][c] = gl128_shading_gain_for_white(white[x][c]);
        }
    }
    DBG(DBG_info, "GL128 shading white0=(%u,%u,%u) gain0=(%u,%u,%u)\n",
        white[0][0], white[0][1], white[0][2], gains[0][0], gains[0][1], gains[0][2]);
    auto measured = gl128_pack_shading_table(dark_flat, gains, declared);
    dev->interface->write_ahb(GL128_AHB_SHADING, measured.size(), measured.data());
    dev->interface->write_register(REG_0x02, 0x00);
    // Image offsets: SilverFast writes 41/35/42 after its white strip, i.e.
    // +2/+3/+3 over the shading offsets.
    apply_fe({ std::min(255, level_offs[0] + 2), std::min(255, level_offs[1] + 3),
               std::min(255, level_offs[2] + 3) }, white_gains);

    // Depth registers back to 8-bit before the feeds (SilverFast writes 0x33
    // twice, 0x17 then 0x1f).
    dev->interface->write_register(REG_DEPTH_A, 0x17);
    dev->interface->write_register(REG_DEPTH_A, 0x1f);
}

void gl128_position_for_image(Genesys_Device* dev, const Genesys_Sensor& sensor,
                              const ScanSession& session, unsigned feed2_steps)
{
    DBG(DBG_info, "GL128: positioning feed pair %u then %u steps\n",
        GL128_FEED1_STEPS, feed2_steps);

    // Lamp on before moving (0x03=0x30, as in SilverFast's image registers);
    // gl128_lamp_on() also sets it in dev->reg so the image register write
    // keeps it. The feeds give the lamp about 3 s to settle.
    gl128_lamp_on(dev, false);

    // Sensor registers before any motor move: they include the system/motor
    // clock (0x0b=0x4c) and the motor watchdog (0x1e). SilverFast writes them
    // once, before calibration and the feeds. Feeds at the power-on clock
    // followed by a park at 0x4c stopped short of home.
    for (const auto& r : sensor.custom_regs) {
        if (r.address == REG_0x03 || r.address == REG_IR) {
            continue;
        }
        dev->interface->write_register(r.address, static_cast<std::uint8_t>(r.value));
        dev->reg.init_reg(r.address, static_cast<std::uint8_t>(r.value));
    }

    // 0x01=0x22, 0x02=0x78, then the feeds. MOTORENB may still be set (status
    // 0x4d after a power cycle); SilverFast feeds without clearing it. The
    // feeds start where the white strip left the carriage, not at home.
    dev->interface->write_register(REG_0x01, 0x22);
    dev->interface->write_register(REG_0x02, 0x78);

    // Feed 1, in the capture's order. The sensor window (STRPIXEL, ENDPIXEL,
    // DPISET, clocks 0xa5/0xab) is the image scan's, as SilverFast writes it
    // at 600 and 7200 dpi; the motor registers are the same in both captures.
    const Gl128DpiEntry& de = gl128_dpi_entry(session.params.xres);
    const unsigned dpiset = de.asic_dpi / 6;
    const auto b = [](unsigned v, unsigned shift) { return static_cast<std::uint8_t>((v >> shift) & 0xff); };
    const std::pair<std::uint16_t, std::uint8_t> FEED1_REGS[] = {
        { 0x3b, 0x01 }, { 0x01, 0x22 }, { 0x04, 0x42 }, { 0x05, 0x48 },
        { 0x3d, 0x00 }, { 0x3e, 0x6e }, { 0x3f, 0x84 },
        { 0xa6, 0x00 }, { 0xa7, 0x00 }, { 0xa8, 0x00 }, { 0xa9, 0x00 },
        { 0x7d, 0x00 }, { 0x7e, 0x36 }, { 0x7f, 0xb0 },
        { 0x80, 0x00 }, { 0x81, 0x40 },
        { 0x82, b(session.pixel_startx, 16) }, { 0x83, b(session.pixel_startx, 8) },
        { 0x84, b(session.pixel_startx, 0) },
        { 0x85, b(session.pixel_endx, 16) }, { 0x86, b(session.pixel_endx, 8) },
        { 0x87, b(session.pixel_endx, 0) },
        { 0x2c, b(dpiset, 8) }, { 0x2d, b(dpiset, 0) }, { 0x1d, 0x80 }, { 0x1c, 0x20 },
        { 0xa4, 0x00 }, { 0xa5, static_cast<std::uint8_t>(de.pixel_clock) },
        { 0xaa, 0x00 }, { 0xab, static_cast<std::uint8_t>(de.pixel_clock) },
        { 0x02, 0x18 }, { 0xae, 0x00 }, { 0xaf, 0x7f },
    };
    for (const auto& r : FEED1_REGS) {
        dev->interface->write_register(r.first, r.second);
    }
    gl128_upload_slope(dev, GL128_SLOPE_TABLE_SLOW_BYTES);
    dev->interface->write_register(REG_START, START_GO);
    gl128_wait_feed_done(dev, GL128_FEED1_STEPS, 30.0);
    // Between the feeds: 0x02=0x08, FEEDL 1.
    dev->interface->write_register(REG_0x02, 0x08);
    gl128_write_feedl(dev, 1);

    // Feed 2: FEEDL from the crop's tl_y, see gl128_feed2_steps().
    const std::pair<std::uint16_t, std::uint8_t> FEED2_REGS[] = {
        { 0x02, 0x18 },
        { 0x3d, static_cast<std::uint8_t>((feed2_steps >> 16) & 0xff) },
        { 0x3e, static_cast<std::uint8_t>((feed2_steps >> 8) & 0xff) },
        { 0x3f, static_cast<std::uint8_t>(feed2_steps & 0xff) },
        { 0x02, 0x18 },
        { 0x7d, 0x00 }, { 0x7e, 0x36 }, { 0x7f, 0xb0 }, { 0x1c, 0x20 },
        { 0x8a, 0x00 }, { 0x8b, 0x00 }, { 0x8c, 0x00 }, { 0x8d, 0x00 }, { 0x8e, 0x00 },
        { 0x8f, 0x00 }, { 0x90, 0x00 }, { 0x91, 0x00 }, { 0x92, 0x00 },
        { 0xae, 0x00 }, { 0xaf, 0xff },
    };
    for (const auto& r : FEED2_REGS) {
        dev->interface->write_register(r.first, r.second);
    }
    gl128_upload_slope(dev, GL128_SLOPE_TABLE_FAST_BYTES);
    dev->interface->write_register(REG_START, START_GO);
    gl128_wait_feed_done(dev, feed2_steps, 30.0);

    Status end = gl128_read_status_reliable(dev);
    DBG(DBG_info, "GL128: positioned (at_home=%d motor=%d)\n",
        end.is_at_home ? 1 : 0, end.is_motor_enabled ? 1 : 0);
}

} // namespace

// --- CommandSet ----------------------------------------------------------------

bool CommandSetGl128::needs_home_before_init_regs_for_scan(Genesys_Device* dev) const
{
    (void) dev;
    // Every scan starts from home: the positioning feeds are measured from it.
    return true;
}

void CommandSetGl128::wait_for_motor_stop(Genesys_Device* dev) const
{
    (void) dev;
    // No-op: GL124 polls MOTMFLG in 0x100, which GL128 does not have (its
    // status is 0x101). Scans end with gl128_stop_motor(), which waits for
    // the park itself.
}

void CommandSetGl128::move_back_home(Genesys_Device* dev, bool wait_until_home) const
{
    (void) wait_until_home;
    // No standalone home seek: one drove the carriage into the home stop.
    // The carriage parks with AGOHOME at the end of every image pass.
    // genesys calls this before every scan, so succeed when already home.
    if (is_head_home(*dev, ScanHeadId::PRIMARY)) {
        return;
    }

    throw SaneException("GL128 has no capture-proven standalone home seek; "
                        "the carriage isn't at home and moving it there needs "
                        "confirmed real-hardware behavior we don't have yet");
}

void CommandSetGl128::init(Genesys_Device* dev) const
{
    DBG_HELPER(dbg);
    // As GL124. Cold/warm detection reads PWRBIT in 0x06, which is not
    // documented for GL128; treating a warm start as cold only repeats the
    // boot sequence.
    sanei_genesys_asic_init(dev);
}

// Power-on initialisation: the two boot blobs, then the boot, memory layout
// and GPO registers (pyopticfilm INIT_REGS, MEMORY_LAYOUT_REGS, GPO_REGS), in
// SilverFast's order. Lamp (0x03) and IR (0x37) are armed separately below.
// No cold-reset sequence: none is known for GL128.
void CommandSetGl128::asic_boot(Genesys_Device* dev, bool cold) const
{
    DBG_HELPER(dbg);
    (void) cold;

    static constexpr struct { std::uint16_t addr; std::uint8_t value; } BOOT_REGS[] = {
    // INIT_REGS
    { 0x01, 0x22 },
    { 0x02, 0x78 },
    { 0x03, 0x20 },
    { 0x04, 0x02 },
    { 0x05, 0x48 },
    { 0x06, 0x18 },
    { 0x07, 0x00 },
    { 0x08, 0x00 },
    { 0x09, 0x00 },
    { 0x0a, 0x40 },
    { 0x0b, 0x6c },
    { 0x0c, 0x00 },
    { 0x0d, 0x00 },
    { 0x11, 0x00 },
    { 0x12, 0x04 },
    { 0x13, 0x08 },
    { 0x14, 0x01 },
    { 0x15, 0x80 },
    { 0x16, 0x27 },
    { 0x17, 0x0c },
    { 0x18, 0x10 },
    { 0x19, 0x02 },
    { 0x1a, 0x00 },
    { 0x1b, 0x00 },
    { 0x1c, 0x00 },
    { 0x1d, 0x00 },
    { 0x1e, 0x10 },
    { 0x1f, 0x00 },
    { 0x20, 0x0c },
    { 0x21, 0x00 },
    { 0x22, 0x1a },
    { 0x23, 0x00 },
    { 0x24, 0x1a },
    { 0x25, 0x00 },
    { 0x26, 0x00 },
    { 0x27, 0x00 },
    { 0x2b, 0x20 },
    { 0x2c, 0x12 },
    { 0x2d, 0xc0 },
    { 0x30, 0x6f },
    { 0x31, 0x00 },
    { 0x32, 0x22 },
    { 0x33, 0x04 },
    { 0x34, 0x80 },
    { 0x35, 0x2f },
    { 0x36, 0x1c },
    { 0x37, 0xc0 },
    { 0x38, 0x44 },
    { 0x39, 0x00 },
    { 0x3a, 0x00 },
    { 0x3b, 0xff },
    { 0x3c, 0xff },
    { 0x3d, 0x00 },
    { 0x3e, 0x00 },
    { 0x3f, 0x01 },
    { 0x4f, 0x03 },
    { 0x52, 0x07 },
    { 0x53, 0x09 },
    { 0x54, 0x0b },
    { 0x55, 0x01 },
    { 0x56, 0x03 },
    { 0x57, 0x05 },
    { 0x5a, 0x12 },
    { 0x5b, 0x00 },
    { 0x5c, 0x40 },
    { 0x5e, 0x1f },
    { 0x5f, 0x05 },
    { 0x60, 0x00 },
    { 0x61, 0x00 },
    { 0x63, 0x20 },
    { 0x67, 0x7f },
    { 0x68, 0x7f },
    { 0x69, 0x01 },
    { 0x70, 0x01 },
    { 0x71, 0x02 },
    { 0x72, 0x03 },
    { 0x73, 0x04 },
    { 0x74, 0x00 },
    { 0x75, 0x00 },
    { 0x76, 0x00 },
    { 0x77, 0x00 },
    { 0x78, 0x00 },
    { 0x79, 0x0f },
    { 0x7a, 0xff },
    { 0x7b, 0xff },
    { 0x7c, 0xff },
    { 0x7d, 0x00 },
    { 0x7e, 0x2a },
    { 0x7f, 0xf8 },
    { 0x80, 0x00 },
    { 0x81, 0x22 },
    { 0x82, 0x00 },
    { 0x83, 0x01 },
    { 0x84, 0x18 },
    { 0x85, 0x00 },
    { 0x86, 0x00 },
    { 0x87, 0x00 },
    { 0x93, 0x00 },
    { 0x94, 0x00 },
    { 0x95, 0x00 },
    { 0x9d, 0x08 },
    { 0xa0, 0x12 },
    { 0xa4, 0x00 },
    { 0xa5, 0x20 },
    { 0xa6, 0x00 },
    { 0xa7, 0x00 },
    { 0xa8, 0x00 },
    { 0xa9, 0x00 },
    { 0xaa, 0x00 },
    { 0xab, 0x30 },
    { 0xb8, 0x00 },
    { 0xb9, 0x38 },
    { 0xba, 0x00 },
    { 0xbd, 0x00 },
    { 0xbe, 0x00 },
    { 0xbf, 0x00 },
    // MEMORY_LAYOUT_REGS
    { 0xd0, 0x0a },
    { 0xd1, 0x0a },
    { 0xd2, 0x0a },
    { 0xe0, 0x00 },
    { 0xe1, 0x68 },
    { 0xe2, 0x0b },
    { 0xe3, 0x00 },
    { 0xe4, 0x0b },
    { 0xe5, 0x01 },
    { 0xe6, 0x15 },
    { 0xe7, 0x99 },
    { 0xe8, 0x15 },
    { 0xe9, 0x9a },
    { 0xea, 0x20 },
    { 0xeb, 0x32 },
    { 0xec, 0x20 },
    { 0xed, 0x33 },
    { 0xee, 0x2a },
    { 0xef, 0xcb },
    { 0xf0, 0x2a },
    { 0xf1, 0xcc },
    { 0xf2, 0x35 },
    { 0xf3, 0x64 },
    { 0xf4, 0x35 },
    { 0xf5, 0x65 },
    { 0xf6, 0x3f },
    { 0xf7, 0xfd },
    { 0xf8, 0x05 },
    // GPO_REGS
    { 0xa2, 0x00 },
    { 0xa3, 0x00 },
    { 0xa4, 0x00 },
    { 0xa6, 0x00 },
    { 0xa7, 0x00 },
    { 0xa8, 0x00 },
    { 0xa9, 0x00 },
    { 0xaa, 0x00 },
    { 0xac, 0x00 },
    { 0xad, 0x01 },
    { 0xae, 0x00 },
    };

    // Two boot blobs first, as SilverFast writes them; boot does not work
    // without them.
    {
        std::vector<std::uint8_t> blob_a(34, 0);
        std::vector<std::uint8_t> blob_b(34, 0);
        blob_b[32] = 0x33;
        dev->interface->write_ahb(0x000FFF00, blob_a.size(), blob_a.data());
        dev->interface->write_ahb(0x000FFF01, blob_b.size(), blob_b.data());
    }
    for (const auto& r : BOOT_REGS) {
        dev->interface->write_register(r.addr, r.value);
        // Shared code (e.g. regs_set_optical_off()) looks registers up in
        // dev->reg, so every boot register must exist there.
        dev->reg.init_reg(r.addr, r.value);
    }

    // 0x01 is set per scan, but shared code may read it from dev->reg before
    // the first one; 0x22 is its boot value.
    dev->reg.init_reg(REG_0x01, 0x22);

    // Lamp arming sequence on 0x03, as SilverFast writes it after boot:
    // 0x10, 0x00, 0x20, 0x30, 0x20, 0x30. Without it 0x03=0x20 does not switch
    // the lamp off after a power cycle, and the dark reference is lit.
    for (std::uint8_t v : { 0x10, 0x00, 0x20, 0x30, 0x20, 0x30 }) {
        dev->interface->write_register(REG_0x03, v);
    }
    dev->reg.init_reg(REG_0x03, 0x30);
    g_gl128_lamp_cold = cold; // asic_boot() also runs on warm opens
}

void CommandSetGl128::init_regs_for_warmup(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                           Genesys_Register_Set* regs) const
{
    (void) dev; (void) sensor; (void) regs;
    // Not used: the lamp warms up inside gl128_run_asic_shading().
    throw SaneException("GL128 init_regs_for_warmup() not yet implemented");
}

void CommandSetGl128::init_regs_for_shading(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                            Genesys_Register_Set& regs) const
{
    DBG_HELPER(dbg);

    // Called for both the dark and the white pass; genesys switches lamp and
    // motor after this returns.
    constexpr unsigned SHADING_LINES = 128;

    // Recompute the session from dev->settings: scanner_move() overwrites
    // dev->session with its own (empty-window) session before this runs.
    ScanSession real_session = dev->cmd_set->calculate_scan_session(dev, sensor, dev->settings);

    // DEPTH8 registers deliver LINCNT/2 rows, so LINCNT = 2 * 128 gives 128
    // rows; genesys reads channels*2*pixels*(params.lines + 1) bytes, so
    // params.lines = 127.
    gl128_setup_shading_strip_regs(sensor, real_session, 2 * SHADING_LINES, regs);

    // The reference passes need the image's per-channel exposure tables, as
    // SilverFast uploads them before its shading passes.
    gl128_upload_image_tables(dev, gl128_dpi_entry(real_session.params.xres).asic_dpi);

    // genesys_shading_calibration_impl() sizes its read from
    // dev->calib_session: channels * 2 * params.pixels * (params.lines + 1).
    // Width: (ENDPIXEL - STRPIXEL) / (7200 / asic_dpi).
    const unsigned dpi = real_session.params.xres;
    const Gl128DpiEntry& dpi_entry = gl128_dpi_entry(dpi);
    const unsigned span = real_session.pixel_endx - real_session.pixel_startx;
    const unsigned factor = std::max(1u, sensor.full_resolution / dpi_entry.asic_dpi);
    const unsigned acquire_width = std::max(1u, span / factor);

    ScanSession calib;
    calib.params.xres = dpi;
    calib.params.yres = dpi;
    calib.params.startx = 0;
    calib.params.starty = 0;
    calib.params.pixels = acquire_width;
    calib.params.lines = SHADING_LINES - 1;
    calib.params.depth = 16;
    calib.params.channels = 3;
    calib.params.scan_method = dev->settings.scan_method;
    calib.params.scan_mode = ScanColorMode::COLOR_SINGLE_PASS;
    calib.params.color_filter = ColorFilter::RED;
    calib.params.flags = ScanFlag::DISABLE_SHADING | ScanFlag::DISABLE_GAMMA |
                         ScanFlag::DISABLE_BUFFER_FULL_MOVE;
    calib.full_resolution = sensor.full_resolution;
    calib.optical_resolution = sensor.full_resolution;
    calib.output_resolution = dpi;
    calib.output_pixels = acquire_width;
    calib.optical_pixels = acquire_width;
    calib.pixel_startx = real_session.pixel_startx;
    calib.pixel_endx = real_session.pixel_endx;
    calib.output_line_count = SHADING_LINES;
    calib.optical_line_count = SHADING_LINES;
    calib.output_channel_bytes = acquire_width * 2;
    calib.output_line_bytes = calib.output_channel_bytes * 3;
    calib.output_line_bytes_raw = calib.output_line_bytes;
    calib.output_total_bytes_raw = calib.output_line_bytes * SHADING_LINES;
    calib.output_total_bytes = calib.output_total_bytes_raw;
    // As in calculate_scan_session().
    calib.buffer_size_read = align_multiple_ceil(calib.output_line_bytes_raw * 64, 2);
    calib.use_host_side_calib = sensor.use_host_side_calib;
    calib.computed = true;

    dev->calib_session = calib;
}

void CommandSetGl128::init_regs_for_scan_session(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                                 Genesys_Register_Set* reg,
                                                 const ScanSession& session) const
{
    // Image pass only (scanner_move() feeds carry ScanFlag::FEEDING): ASIC
    // shading and the positioning feeds run here, before the image registers
    // are built, since they change dev->reg.
    if ((session.params.flags & ScanFlag::FEEDING) == ScanFlag::NONE) {
        // Travel limit, checked before any motion: the image pass (LINCNT *
        // 14400 / (4 * asic_dpi) steps) must end no further than SilverFast's
        // own full-frame passes on this scanner: feed2 13560 plus 13752 steps
        // (600 dpi, LINCNT 2292), i.e. step 27312. That is about 25 mm from
        // the top of the window at every resolution.
        const unsigned feed2 = gl128_feed2_steps(dev);
        {
            constexpr long GL128_MAX_IMAGE_END_STEPS = 13560 + 13752;
            const unsigned asic_dpi_gate = gl128_dpi_entry(session.params.yres).asic_dpi;
            const long travel = GL128_MAX_IMAGE_END_STEPS - static_cast<long>(feed2);
            const unsigned max_lincnt = travel <= 0 ? 0u :
                    std::max(4u, (static_cast<unsigned>(travel) * asic_dpi_gate * 4u / 14400u) / 4u * 4u);
            const unsigned want_lincnt = session.params.lines * 4u;
            if (want_lincnt > max_lincnt) {
                throw SaneException(SANE_STATUS_INVAL,
                        "GL128: scan height too large -- %u lines at %u dpi would run "
                        "past the scan-window end (max %u lines). Reduce the height.",
                        session.params.lines / gl128_host_downsample(session.params.yres),
                        session.params.yres,
                        max_lincnt / 4u / gl128_host_downsample(session.params.yres));
            }
        }
        {
            const Gl128DpiEntry& de = gl128_dpi_entry(session.params.xres);
            Gl128ShadingWindow w{};
            w.strpixel = session.pixel_startx;
            w.endpixel = session.pixel_endx;
            w.asic_dpi = de.asic_dpi;
            w.dpiset = de.asic_dpi / 6;
            w.pixels = std::max(1u, (session.pixel_endx - session.pixel_startx) /
                                    std::max(1u, 7200u / de.asic_dpi));
            // The front end must be programmed before the references are
            // measured (as GL124's scan setup does); with the power-on AFE
            // the dark reference reads 33-44k.
            set_fe(dev, sensor, AFE_SET);
            gl128_run_asic_shading(dev, sensor, w);
        }
        gl128_position_for_image(dev, sensor, session, feed2);
        DBG(DBG_info, "GL128: image %ux%u dpi, %u lines, unstagger %u buffer rows\n",
            session.params.xres, session.params.yres, session.params.lines,
            session.num_staggered_lines);
    }

    // Sensor timing and AFE sampling registers (sensor.custom_regs), as
    // gl124_setup_sensor() applies them. Without them the CCD is not clocked.
    for (const auto& r : sensor.custom_regs) {
        reg->init_reg(r.address, static_cast<std::uint8_t>(r.value));
    }

    DBG_HELPER(dbg);
    session.assert_computed();
    (void) sensor;

    // Register values for the pass; begin_scan() starts it.

    const unsigned dpi = session.params.xres;
    const Gl128DpiEntry& dpi_entry = gl128_dpi_entry(dpi);
    const unsigned asic_dpi = dpi_entry.asic_dpi;

    // 0x01: SHDAREA on, SCAN (set by begin_scan()) and STAGGER off. DVDSET on
    // for the image pass, so the ASIC applies the shading table from
    // gl128_run_asic_shading() (SilverFast's image 0x01 is 0x23); off for
    // feeds.
    std::uint8_t reg01 = 0x22; // boot value
    reg01 = (reg01 | REG_0x01_SHDAREA) & ~REG_0x01_SCAN & ~REG_0x01_STAGGER;
    if ((session.params.flags & ScanFlag::FEEDING) == ScanFlag::NONE) {
        reg01 |= REG_0x01_DVDSET;
    } else {
        reg01 &= ~REG_0x01_DVDSET;
    }
    reg->init_reg(REG_0x01, reg01);

    // 0x02: MTRPWR|AGOHOME for the image pass (0x30 in the captures), so the
    // carriage parks at the end. scanner_move() feeds (ScanFlag::FEEDING)
    // with a real distance use MTRPWR|FASTFED (0x18, as SilverFast's feeds).
    // A zero-distance feed (scanner_move_to_ta() on this TA-only scanner)
    // keeps MTRPWR|AGOHOME with FEEDL 1: FASTFED with FEEDL 0 never reported
    // the motor stopped.
    const bool is_feeding = (session.params.flags & ScanFlag::FEEDING) != ScanFlag::NONE;
    const bool is_real_feed = is_feeding && session.params.starty > 0;
    reg->init_reg(REG_0x02, is_real_feed
                       ? static_cast<std::uint8_t>(REG_0x02_MTRPWR | REG_0x02_FASTFED)
                       : static_cast<std::uint8_t>(REG_0x02_MTRPWR | REG_0x02_AGOHOME));

    // REG_0x03 / REG_IR: not written here. For the image path the lamp is
    // switched on (0x03=0x30, capture value) by gl128_position_for_image()
    // above, which mirrors it into dev->reg before these values are built.

    // Depth registers 0x33=0x1f, 0xaf=0xff: 16-bit LE samples.
    reg->init_reg(REG_DEPTH_A, DEPTH8_A);
    reg->init_reg(REG_DEPTH_B, DEPTH8_B);

    // LINCNT counts half buffer rows (see calculate_scan_session()).
    const unsigned register_lincnt = session.optical_line_count * 2;
    gl128_init_reg24(reg, REG_LINCNT, register_lincnt);

    // LPERIOD and the image-pass clocks: gl128_timing() (per-dpi table;
    // width-dependent at 7200 dpi from the SilverFast captures).
    const Gl128Timing timing = gl128_timing(asic_dpi, session.pixel_endx - session.pixel_startx);
    gl128_init_reg24(reg, REG_LPERIOD, timing.lperiod);

    gl128_init_reg16(reg, REG_DPISET, static_cast<std::uint16_t>(asic_dpi / 6));

    gl128_init_reg24(reg, REG_STRPIXEL, session.pixel_startx);
    gl128_init_reg24(reg, REG_ENDPIXEL, session.pixel_endx);

    // EXPOSURE is 14000 at every resolution in the captures.
    gl128_init_reg24(reg, REG_EXPOSURE, 14000);

    // FEEDL: 1 for the image pass (as in the captures) and for zero-distance
    // feeds (see 0x02 above); the requested steps for a real feed.
    gl128_init_reg24(reg, REG_FEEDL, is_real_feed ? session.params.starty : 1);

    // Image-pass dummy and pixel clocks.
    reg->init_reg(0x2B, static_cast<std::uint8_t>(timing.image.dummy));
    reg->init_reg(0xA5, static_cast<std::uint8_t>(timing.image.clk_a));
    reg->init_reg(0xAB, static_cast<std::uint8_t>(timing.image.clk_b));

    // Motor registers are not taken from dev->motor.profiles (unlike GL124):
    // the image pass uses the slope tables uploaded from the capture.

    dev->read_active = true;
    dev->session = session;
    dev->total_bytes_read = 0;
    dev->total_bytes_to_read = static_cast<std::size_t>(session.output_line_bytes_requested) *
                               static_cast<std::size_t>(session.params.lines);

    // As gl124: set up the image pipeline that sane_read() pulls from.
    dev->line_count = 0;
    setup_image_pipeline(*dev, session);

    // The frontend reads what the pipeline outputs (also what
    // sane_get_parameters() reports). The travel limit leaves no room to scan
    // extra lines for the colour line shift, so the image is a few rows
    // shorter than params.lines.
    dev->total_bytes_to_read = dev->pipeline.get_output_row_bytes() *
                               dev->pipeline.get_output_height();

    // Image pass: channel exposure tables before the register writes and
    // START, in the capture's order.
    if (!is_feeding) {
        gl128_upload_image_tables(dev, asic_dpi);
    }
}

void CommandSetGl128::set_fe(Genesys_Device* dev, const Genesys_Sensor& sensor,
                             std::uint8_t set) const
{
    DBG_HELPER(dbg);
    (void) sensor;

    if (set == AFE_INIT) {
        dev->frontend = dev->frontend_initial;
    }

    // AFE registers 0x00-0x07 through the 0x51 (index) / 0x5d, 0x5e (data)
    // bridge, which write_fe_register() uses for GL124 and GL128. GL124's TI
    // AFE start sequence does not apply to this Analog Devices style AFE.
    for (std::uint16_t addr = 0x00; addr <= 0x07; addr++) {
        dev->interface->write_fe_register(static_cast<std::uint8_t>(addr),
                                          dev->frontend.regs.get_value(addr));
    }
}

void CommandSetGl128::set_powersaving(Genesys_Device* dev, int delay) const
{
    DBG_HELPER_ARGS(dbg, "delay = %d", delay);
    (void) dev;
    // No-op. GL124 puts the delay in the upper nibble of 0x03, which holds
    // the lamp and XPA bits on GL128; no power-saving mechanism is known.
}

void CommandSetGl128::save_power(Genesys_Device* dev, bool enable) const
{
    DBG_HELPER_ARGS(dbg, "enable = %d", enable);
    (void) dev;
    // No-op, as GL124.
}

void CommandSetGl128::begin_scan(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                 Genesys_Register_Set* regs, bool start_motor) const
{
    DBG_HELPER(dbg);
    (void) sensor; (void) start_motor;

    // genesys' host-side shading passes set MTRPWR. A register set without
    // AGOHOME or FASTFED is such a pass; run it stationary, since nothing
    // would bring the carriage back home afterwards.
    if (regs != nullptr && regs->has_reg(REG_0x02)) {
        std::uint8_t r02 = regs->get8(REG_0x02);
        if ((r02 & (REG_0x02_AGOHOME | REG_0x02_FASTFED)) == 0 && (r02 & REG_0x02_MTRPWR)) {
            r02 = static_cast<std::uint8_t>(r02 & ~REG_0x02_MTRPWR);
            regs->find_reg(REG_0x02).value = r02;
            dev->interface->write_register(REG_0x02, r02);
        }
    }

    // Capture order: 0x0d=0x07, 0x01 |= SCAN, 0x0f=0x01. Stationary passes
    // are started the same way (the motor state comes from 0x02).

    dev->interface->write_register(REG_CLRCNT, CLRCNT_ALL);
    std::uint8_t reg01 = static_cast<std::uint8_t>(dev->reg.get8(REG_0x01) | REG_0x01_SCAN);
    if (regs != nullptr && regs->has_reg(REG_0x01)) {
        reg01 = static_cast<std::uint8_t>(regs->get8(REG_0x01) | REG_0x01_SCAN);
        regs->find_reg(REG_0x01).value = reg01;
    }
    dev->interface->write_register(REG_0x01, reg01);
    dev->reg.init_reg(REG_0x01, reg01);
    dev->interface->write_register(REG_START, START_GO);
}

void CommandSetGl128::end_scan(Genesys_Device* dev, Genesys_Register_Set* regs,
                               bool check_stop) const
{
    DBG_HELPER(dbg);
    (void) regs; (void) check_stop;
    gl128_stop_motor(dev);
}

void CommandSetGl128::send_gamma_table(Genesys_Device* dev, const Genesys_Sensor& sensor) const
{
    DBG_HELPER(dbg);
    (void) dev; (void) sensor;
    // No-op: output is linear. The shared gamma upload writes AHB
    // 0x01000000, which SilverFast never writes.
}

// offset_calibration() / coarse_gain_calibration(): not used. The model sets
// ModelFlag::DISABLE_ADC_CALIBRATION; gl128_run_asic_shading() programs the
// front end with SilverFast's fixed codes before every image pass.
void CommandSetGl128::offset_calibration(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                         Genesys_Register_Set& regs) const
{
    (void) dev; (void) sensor; (void) regs;
}

void CommandSetGl128::coarse_gain_calibration(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                              Genesys_Register_Set& regs, int dpi) const
{
    (void) dev; (void) sensor; (void) regs; (void) dpi;
}

SensorExposure CommandSetGl128::led_calibration(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                                Genesys_Register_Set& regs) const
{
    (void) dev; (void) sensor; (void) regs;
    // Not used: exposure is fixed (14000) and the lamp is not an LED array.
    throw SaneException("GL128 led_calibration() not yet implemented");
}

void CommandSetGl128::update_hardware_sensors(struct Genesys_Scanner* s) const
{
    DBG_HELPER(dbg);
    (void) s;
    // No-op: button polling is not known for this scanner.
}

void CommandSetGl128::update_home_sensor_gpio(Genesys_Device& dev) const
{
    DBG_HELPER(dbg);
    (void) dev;
    // No-op: the captures show no GPIO access around scan start.
}

void CommandSetGl128::load_document(Genesys_Device* dev) const
{
    (void) dev;
    // Not a sheetfed scanner.
    throw SaneException("not implemented");
}

void CommandSetGl128::detect_document_end(Genesys_Device* dev) const
{
    (void) dev;
    throw SaneException("not implemented");
}

void CommandSetGl128::eject_document(Genesys_Device* dev) const
{
    (void) dev;
    throw SaneException("not implemented");
}

void CommandSetGl128::send_shading_data(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                        std::uint8_t* data, int size) const
{
    (void) dev; (void) sensor; (void) data; (void) size;
    // Not used: host-side shading is disabled; gl128_run_asic_shading()
    // uploads the ASIC shading table itself.
    throw SaneException("GL128 send_shading_data() not yet implemented");
}

ScanSession CommandSetGl128::calculate_scan_session(const Genesys_Device* dev,
                                                    const Genesys_Sensor& sensor,
                                                    const Genesys_Settings& settings) const
{
    // Geometry in native 7200 dpi units (pyopticfilm compute_geometry(),
    // native-units branch).

    const unsigned dpi = settings.xres;
    const Gl128DpiEntry& dpi_entry = gl128_dpi_entry(dpi);
    const unsigned asic_dpi = dpi_entry.asic_dpi;
    const unsigned offset = dpi_entry.output_pixel_offset;
    const unsigned optical_res = sensor.full_resolution; // 7200

    // Transparency-only scanner: the TA offsets always apply.
    // The image is mirrored in X after readout, so SANE's tl_x
    // (measured from the displayed left edge) maps to the far end of the
    // native scan window: native = x_size_ta - (tl_x + width).
    const float width_mm = static_cast<float>(settings.pixels) * MM_PER_INCH /
                           static_cast<float>(settings.xres);
    const float mirrored_tl_x = std::max(0.0f, dev->model->x_size_ta - (settings.tl_x + width_mm));
    const float tl_x_mm = dev->model->x_offset_ta + mirrored_tl_x;
    const float tl_y_mm = dev->model->y_offset_ta + settings.tl_y;

    // Inactive pixels at the native end of the sensor (pyopticfilm
    // optical_end_inactive_native).
    constexpr unsigned OPTICAL_END_INACTIVE_NATIVE = 96;

    const unsigned origin_native = offset * optical_res / asic_dpi;
    const unsigned frame_shift = OPTICAL_END_INACTIVE_NATIVE;
    const long tlx_native = lround(tl_x_mm * optical_res / MM_PER_INCH);
    if (tlx_native < 0) {
        throw SaneException("GL128: requested scan area starts left of the TA origin");
    }

    // The CCD's even and odd native columns are 4/7200 in apart in Y (see the
    // stagger_y comment below). The ASIC reads every (optical_res/asic_dpi)-th
    // native column from STRPIXEL: an even step keeps one parity (no zipper),
    // an odd step (7200: 1, 2400: 3, 1440: 5) alternates parity per output
    // column. The unstagger shift is applied per pipeline column parity, so
    // the window must start on an even native column (as genesys
    // compute_session() aligns pixel_startx for staggered sensors) and on a
    // multiple of the step: round down to a multiple of 2*step.
    const unsigned column_step = std::max(1u, optical_res / asic_dpi);
    const bool staggered = (column_step % 2) == 1;
    unsigned pixel_startx = origin_native + frame_shift +
                            static_cast<unsigned>(tlx_native);
    if (staggered) {
        pixel_startx -= pixel_startx % (2 * column_step);
    }

    // Native width from the requested pixel count. Below 600 dpi the ASIC
    // runs at asic_dpi and the host averages host_downsample pixels.
    const unsigned host_downsample = gl128_host_downsample(settings.xres);
    unsigned optical_pixels = static_cast<unsigned>(
        (static_cast<std::uint64_t>(settings.pixels) * optical_res) / settings.xres);
    // The native span must be a multiple of lcm(4, 2 * step) so each USB
    // line has an even pixel count; odd counts shear the image. Rounded up,
    // and build_image_pipeline() crops the surplus off the native end (the
    // displayed left after mirroring), so the output is exactly the
    // requested width.
    {
        const unsigned factor = std::max(1u, optical_res / asic_dpi);
        unsigned a = 4, b = 2 * factor;
        while (b) { unsigned t = a % b; a = b; b = t; }
        const unsigned align = 4 * (2 * factor) / a; // lcm(4, 2*factor)
        optical_pixels = ((optical_pixels + align - 1) / align) * align;
    }

    const unsigned pixel_endx = pixel_startx + optical_pixels;
    const unsigned output_pixels = static_cast<unsigned>(
        (static_cast<std::uint64_t>(optical_pixels) * asic_dpi) / optical_res);

    const unsigned output_startx = pixel_startx * asic_dpi / optical_res;
    if (output_startx < offset) {
        throw SaneException("GL128: computed startx underflows output_pixel_offset");
    }
    const unsigned startx = output_startx - offset;
    const unsigned starty = static_cast<unsigned>(
        std::max(0L, lround(tl_y_mm * dev->motor.base_ydpi / MM_PER_INCH)));

    ScanSession session;
    session.params.xres = settings.xres;
    session.params.yres = settings.yres;
    session.params.startx = startx;
    session.params.starty = starty;
    session.params.pixels = output_pixels;
    session.params.requested_pixels = settings.requested_pixels;
    // Lines at asic_dpi; the pipeline averages host_downsample of them per
    // output line.
    session.params.lines = settings.lines * host_downsample;
    session.params.depth = settings.depth;
    session.params.channels = settings.get_channels();
    session.params.scan_method = settings.scan_method;
    session.params.scan_mode = settings.scan_mode;
    // Gray: the GL128 always delivers chunky RGB. Scan colour and keep the
    // selected colour filter's channel on the host (as genesys does for
    // ModelFlag::HOST_SIDE_GRAY).
    session.use_host_side_gray = false;
    if (session.params.channels == 1) {
        session.use_host_side_gray = true;
        session.params.channels = 3;
        session.params.scan_mode = ScanColorMode::COLOR_SINGLE_PASS;
    }
    session.params.color_filter = settings.color_filter;
    session.params.contrast_adjustment = dev->settings.contrast;
    session.params.brightness_adjustment = dev->settings.brightness;
    // Not used for GL128: LPERIOD comes from gl128_timing().
    session.params.exposure_lperiod = dev->settings.exposure_lperiod;
    session.params.flags = ScanFlag::NONE;

    session.full_resolution = optical_res;
    session.optical_resolution = optical_res;
    session.output_resolution = settings.xres;
    session.output_pixels = output_pixels;
    session.optical_pixels = optical_pixels;
    session.optical_pixels_raw = optical_pixels;
    session.pixel_startx = pixel_startx;
    session.pixel_endx = pixel_endx;
    session.output_startx = output_startx;

    // Colour line shift 0/24/48 at 7200 dpi, scaled as compute_session()
    // does.
    session.color_shift_lines_r = dev->model->ld_shift_r * asic_dpi / dev->motor.base_ydpi;
    session.color_shift_lines_g = dev->model->ld_shift_g * asic_dpi / dev->motor.base_ydpi;
    session.color_shift_lines_b = dev->model->ld_shift_b * asic_dpi / dev->motor.base_ydpi;
    session.max_color_shift_lines = std::max(session.color_shift_lines_r,
                                             std::max(session.color_shift_lines_g,
                                                      session.color_shift_lines_b));

    // The ASIC's STAGGER bit (0x01) is never set; SilverFast corrects the
    // CCD stagger on the host. Even native columns see the film 4/7200 in
    // after odd ones (measured in SilverFast's raw 7200 dpi data: 7-8 buffer
    // rows), the same as CCD_PLUSTEK_OPTICFILM_8200I's StaggerConfig{4, 0}.
    // At odd column steps (7200, 2400, 1440) output columns alternate
    // between even and odd native columns. Below 7200 the offset is not a
    // whole number of lines, so for GL128 stagger_y counts 2x-oversampled
    // buffer rows and build_image_pipeline() applies it before the row-pair
    // average: round(8 * asic_dpi / 7200) rows (7200: 8, 2400: 3, 1440: 2).
    // Measured on hardware after the correction: 0.25 lines left at 7200,
    // 0.4 at 2400 (1.6 before). Like the colour shift, it crops lines off the
    // bottom.
    if (staggered) {
        const unsigned rows = (8 * asic_dpi + optical_res / 2) / optical_res;
        session.stagger_y = StaggerConfig{rows, 0};
        session.num_staggered_lines = rows;
    } else {
        session.stagger_y = StaggerConfig{};
        session.num_staggered_lines = 0;
    }

    // LINCNT is 4 per output line (145 lines -> 580 in the captures), with no
    // extra lines for the colour shift.
    constexpr unsigned LINCNT_PER_LINE = 4;
    const unsigned register_lincnt = (session.params.lines /* + 0 extra_lines */) * LINCNT_PER_LINE;

    // The scanner returns LINCNT / 2 rows of 16-bit RGB: Y is sampled twice
    // per output line, and the pipeline averages the row pairs.
    session.output_line_count = register_lincnt / 2;
    session.optical_line_count = session.output_line_count; // not is_cis

    session.output_channel_bytes = (output_pixels * settings.depth + 7) / 8;
    session.output_line_bytes = session.output_channel_bytes * session.params.channels;
    session.output_line_bytes_raw = session.output_line_bytes;
    session.output_line_bytes_requested = session.output_line_bytes;
    session.output_total_bytes_raw = session.output_line_bytes * session.optical_line_count;
    session.output_total_bytes = session.output_total_bytes_raw;

    // USB read buffer size, as compute_session() sets it.
    session.buffer_size_read = align_multiple_ceil(session.output_line_bytes_raw * 64, 2);

    session.use_host_side_calib = sensor.use_host_side_calib;
    session.computed = true;

    return session;
}

} // namespace gl128
} // namespace genesys
