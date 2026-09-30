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

/*  GL848+ command-set implementation for the HP Scanjet 200.

    The GL848+ is a lightly customised GL847: the register file, the AHB
    memory windows and the USB control protocol are identical, so almost
    every method reuses the GL847-style helpers (`scanner_*` from
    genesys.cpp / low.cpp, or the AsicType::GL848 branches added to those
    helpers). The HP-specific bits — the base register block, the motor
    slope, the pass-return-home mechanic (there is no separate "go
    home" command; the pass returns on its own after 0x0A=0x40 / 0x01=0xC0)
    and the disabled AFE / shading / calibration paths — live here.

    Register semantics that differ from stock GL847 and are therefore
    interesting when reading this file:

    - Live scan status lives at register 0x101 (high bank; wValue 0x18E,
      wIndex 0x0222). Bit 0x08 is the home sensor, 0x01 is
      motor-running, 0x80 is buffer-full.
    - The valid-word counter is 4 bytes at 0x102 (same as GL124).
    - Motor start uses reg 0x0F = 0xFF (not 0x0E, which would reset the
      whole ASIC).
    - The HP driver never touches the AFE (Analog Front-End) registers
      or the shading DRAM area — the scanner does pixel calibration in
      software, so `set_fe`, `send_shading_data`, `led_calibration`,
      `offset_calibration`, `coarse_gain_calibration` are safe no-ops.
*/

#define DEBUG_DECLARE_ONLY

#include "gl848.h"
#include "gl848_registers.h"
#include "low.h"
#include "test_settings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace genesys {
namespace gl848 {

namespace {

// ----------------------------------------------------------------------------
// HP Scanjet 200 baseline register block, reverse-engineered from vendor
// driver USB traces. Written verbatim by asic_boot.
// ----------------------------------------------------------------------------
struct HpBaselineReg { std::uint16_t addr; std::uint8_t value; };

#include "gl848_scan_tables.h"

const std::array<HpBaselineReg, 218> hp_scanjet_200_baseline = {{
    {0x01, 0xc0}, {0x02, 0x00}, {0x03, 0x8f}, {0x04, 0x40}, {0x05, 0x00},
    {0x06, 0x18}, {0x07, 0x00}, {0x08, 0x70}, {0x09, 0x00}, {0x0a, 0x40},
    {0x0b, 0x2a}, {0x0c, 0x02}, {0x12, 0xe0}, {0x13, 0x28}, {0x14, 0x40},
    {0x15, 0x80}, {0x16, 0x14}, {0x17, 0x00}, {0x18, 0x00}, {0x19, 0x05},
    {0x1a, 0x20}, {0x1b, 0x40}, {0x1c, 0x08}, {0x1d, 0x00}, {0x1e, 0xf8},
    {0x1f, 0x00}, {0x20, 0x04}, {0x21, 0x00}, {0x22, 0x0f}, {0x23, 0x00},
    {0x24, 0x00}, {0x25, 0x00}, {0x26, 0x2a}, {0x27, 0x30}, {0x28, 0x00},
    {0x29, 0x86}, {0x2a, 0xf9}, {0x2b, 0xf0}, {0x2c, 0x02}, {0x2d, 0x58},
    {0x2e, 0x00}, {0x2f, 0x00}, {0x30, 0xc2}, {0x32, 0xe3}, {0x33, 0x0b},
    {0x35, 0xfb}, {0x36, 0x0b}, {0x37, 0x30}, {0x38, 0x4c}, {0x3b, 0x00},
    {0x3c, 0xef}, {0x3d, 0x00}, {0x3e, 0x00}, {0x3f, 0x01}, {0x45, 0x00},
    {0x48, 0x00}, {0x4f, 0x08}, {0x50, 0x00}, {0x51, 0x08}, {0x52, 0x09},
    {0x53, 0x03}, {0x54, 0x03}, {0x55, 0x03}, {0x56, 0x03}, {0x57, 0x03},
    {0x5a, 0x59}, {0x5b, 0x31}, {0x5c, 0xc0}, {0x5d, 0x00}, {0x5e, 0x88},
    {0x5f, 0xc0}, {0x60, 0x02}, {0x61, 0x01}, {0x62, 0x00}, {0x63, 0x00},
    {0x64, 0x00}, {0x65, 0x00}, {0x66, 0x00}, {0x67, 0x00}, {0x68, 0x01},
    {0x69, 0x00}, {0x70, 0x01}, {0x71, 0x03}, {0x72, 0x04}, {0x73, 0x05},
    {0x74, 0x00}, {0x75, 0x00}, {0x76, 0x00}, {0x77, 0x00}, {0x78, 0x07},
    {0x79, 0xfe}, {0x7a, 0x00}, {0x7b, 0x0e}, {0x7c, 0x07}, {0x7d, 0x00},
    {0x7e, 0x15}, {0x7f, 0xdf}, {0x80, 0x01}, {0x81, 0x00}, {0x82, 0x00},
    {0x83, 0x00}, {0x84, 0x00}, {0x85, 0x00}, {0x86, 0x00}, {0x87, 0x00},
    {0x88, 0x01}, {0x89, 0x00}, {0x8a, 0x00}, {0x8b, 0x0f}, {0x8c, 0xa0},
    {0x8d, 0x00}, {0x8e, 0x0f}, {0x8f, 0xa0}, {0x90, 0x00}, {0x91, 0x0f},
    {0x92, 0xa0}, {0x93, 0x00}, {0x94, 0x00}, {0x95, 0x00}, {0x96, 0x00},
    {0x97, 0x00}, {0x98, 0x00}, {0x99, 0x1f}, {0x9a, 0x00}, {0x9b, 0x3f},
    {0x9c, 0x3f}, {0x9d, 0x3f}, {0x9e, 0x00}, {0x9f, 0x00}, {0xa0, 0x00},
    {0xa1, 0x10}, {0xa2, 0x00}, {0xa3, 0x02}, {0xa4, 0x00}, {0xa5, 0x1c},
    {0xa6, 0x00}, {0xa7, 0x50}, {0xa8, 0x00}, {0xa9, 0x50}, {0xaa, 0x00},
    {0xab, 0x1c}, {0xac, 0x00}, {0xad, 0x1c}, {0xae, 0x00}, {0xaf, 0xb8},
    {0xb0, 0x00}, {0xb1, 0xb8}, {0xb2, 0x00}, {0xb3, 0x00}, {0xb4, 0x00},
    {0xb5, 0x00}, {0xb6, 0x00}, {0xb7, 0x00}, {0xb8, 0x00}, {0xb9, 0x00},
    {0xba, 0x00}, {0xbb, 0x00}, {0xbc, 0xff}, {0xbd, 0x00}, {0xbe, 0x07},
    {0xbf, 0x00}, {0xc0, 0x00}, {0xc1, 0x00}, {0xc2, 0x00}, {0xc3, 0x00},
    {0xc4, 0x00}, {0xc5, 0x00}, {0xc6, 0x00}, {0xc7, 0x00}, {0xc8, 0x00},
    {0xc9, 0x00}, {0xca, 0x00}, {0xcb, 0x00}, {0xcc, 0x00}, {0xcd, 0x00},
    {0xce, 0x00}, {0xcf, 0x00}, {0xd0, 0x0a}, {0xd1, 0x1f}, {0xd2, 0x34},
    {0xd3, 0x00}, {0xd4, 0x00}, {0xd5, 0x00}, {0xd6, 0x00}, {0xd7, 0x00},
    {0xd8, 0x00}, {0xd9, 0x00}, {0xda, 0x00}, {0xdb, 0x00}, {0xde, 0x00},
    {0xdf, 0x00}, {0xe0, 0x01}, {0xe1, 0x4c}, {0xe2, 0x08}, {0xe3, 0x25},
    {0xe4, 0x08}, {0xe5, 0x26}, {0xe6, 0x0f}, {0xe7, 0xff}, {0xe8, 0x01},
    {0xe9, 0x4c}, {0xea, 0x08}, {0xeb, 0x25}, {0xec, 0x08}, {0xed, 0x26},
    {0xee, 0x0f}, {0xef, 0xff}, {0xf0, 0x01}, {0xf1, 0x4c}, {0xf2, 0x08},
    {0xf3, 0x25}, {0xf4, 0x08}, {0xf5, 0x26}, {0xf6, 0x0f}, {0xf7, 0xff},
    {0xf8, 0x01}, {0xfe, 0x01},
}};

// Motor slope DRAM (0x10000000 + 0x4000*n). Lengths match the per-mode
// step counts in 0xA5 / 0xAF from the Windows-driver capture.
const std::array<std::uint8_t, 118> hp_scanjet_200_slope = {{
    0x79, 0x3f, 0xb6, 0x30, 0xdf, 0x25, 0xfa, 0x1f, 0x2e, 0x1c, 0x7c, 0x19,
    0x73, 0x17, 0xd9, 0x15, 0x8b, 0x14, 0x75, 0x13, 0x89, 0x12, 0xbd, 0x11,
    0x0a, 0x11, 0x6d, 0x10, 0xe0, 0x0f, 0x61, 0x0f, 0xee, 0x0e, 0x85, 0x0e,
    0x25, 0x0e, 0xcc, 0x0d, 0x7a, 0x0d, 0x2e, 0x0d, 0xe7, 0x0c, 0xa5, 0x0c,
    0x67, 0x0c, 0x2c, 0x0c, 0xf5, 0x0b, 0xc1, 0x0b, 0x90, 0x0b, 0x61, 0x0b,
    0x35, 0x0b, 0x0a, 0x0b, 0xe2, 0x0a, 0xbc, 0x0a, 0x98, 0x0a, 0x75, 0x0a,
    0x55, 0x0a, 0x35, 0x0a, 0x17, 0x0a, 0xfb, 0x09, 0xdf, 0x09, 0xc5, 0x09,
    0xac, 0x09, 0x93, 0x09, 0x7c, 0x09, 0x66, 0x09, 0x50, 0x09, 0x3b, 0x09,
    0x27, 0x09, 0x13, 0x09, 0x01, 0x09, 0xee, 0x08, 0xdd, 0x08, 0xcc, 0x08,
    0xbb, 0x08, 0xab, 0x08, 0x9c, 0x08, 0x8d, 0x08, 0x83, 0x08,
}};

const std::uint8_t hp_slope_600_fast[8] __attribute__((unused)) = {
    0xbc, 0x1f, 0x5b, 0x18, 0xef, 0x12, 0x10, 0x21
};
const std::uint8_t hp_slope_1200_fast[2] __attribute__((unused)) = { 0x98, 0x0f };
const std::uint8_t hp_slope_2400_fast[2] = { 0x6c, 0x3d };
const std::uint8_t hp_slope_1200_slow[48] = {
    0x40, 0x36, 0x65, 0x33, 0x80, 0x2e, 0x63, 0x2c, 0x75, 0x2a, 0xb0, 0x28,
    0x10, 0x27, 0x8f, 0x25, 0x2b, 0x24, 0xe0, 0x22, 0xac, 0x21, 0x8d, 0x20,
    0x97, 0x1d, 0x0a, 0x1a, 0x88, 0x17, 0xc7, 0x14, 0x6c, 0x12, 0x8d, 0x10,
    0xcb, 0x0e, 0x32, 0x0d, 0xc4, 0x0b, 0x0a, 0x0a, 0xe9, 0x08, 0x83, 0x08
};

void send_hp_slope_tables(Genesys_Device* dev, unsigned dpi);
void send_hp_return_slope_tables(Genesys_Device* dev);
void hp_match_fast_stepno_to_scan(Genesys_Register_Set* regs);
void hp_scale_slope_le16(std::vector<std::uint8_t>& bytes, unsigned num, unsigned den);
void hp_arm_motor(Genesys_Device* dev, Genesys_Register_Set* reg);

unsigned g_hp_last_slope_dpi = 300;
bool g_hp_homing = false;

// Optical passes the HP driver exposes. Every user-facing DPI is an
// integer downsample from one of these; hp_choose_mode() picks the
// cheapest one.
struct HpOpticalMode {
    unsigned dpi;
    unsigned samples_per_line;
    unsigned page_lines;
    unsigned feed_lines;
};

const std::array<HpOpticalMode, 4> hp_optical_modes = {{
    {  300,  2580, 10524, 543 },
    {  600,  5160, 21048, 489 },
    { 1200, 10320, 42096, 552 },
    { 2400, 20640, 84168, 552 },
}};

HpOpticalMode hp_choose_mode(unsigned output_dpi, unsigned& scale_out)
{
    for (const auto& mode : hp_optical_modes) {
        if (output_dpi != 0 && mode.dpi % output_dpi == 0 &&
            (mode.dpi / output_dpi) <= 8) {
            scale_out = mode.dpi / output_dpi;
            return mode;
        }
    }
    scale_out = 1;
    return hp_optical_modes[0];
}

void apply_reg_table(Genesys_Register_Set* regs, const HpBaselineReg* table, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        if (!regs->has_reg(table[i].addr)) {
            regs->init_reg(table[i].addr, table[i].value);
        } else {
            regs->set8(table[i].addr, table[i].value);
        }
    }
}

void apply_scan_regs_for_mode(Genesys_Register_Set* regs, unsigned dpi)
{
    switch (dpi) {
        case 600:
            apply_reg_table(regs, hp_scan_regs_600, hp_scan_regs_600_count);
            break;
        case 1200:
            apply_reg_table(regs, hp_scan_regs_1200, hp_scan_regs_1200_count);
            break;
        case 2400:
            apply_reg_table(regs, hp_scan_regs_2400, hp_scan_regs_2400_count);
            break;
        default:
            apply_reg_table(regs, hp_scan_regs_300, hp_scan_regs_300_count);
            break;
    }
    // 1200/2400 vendor snapshots leave 0xA7/0xA9 at 0. The CIS LED then
    // sits at ~22500 on a white sheet (same as a printed page). 0x50 is
    // the lamp-on value used for a full-bright 2400 pass.
    if (dpi >= 1200 && regs->has_reg(0xa7) && regs->get8(0xa7) == 0) {
        regs->set8(0xa7, 0x50);
        if (regs->has_reg(0xa9)) {
            regs->set8(0xa9, 0x50);
        }
    }
    // 2400 vendor capture keeps FASTNO=1 (one-word table in slots 0–2)
    // so AGOHOME does not walk the 24-word scan ramp. Copying 0xAF onto
    // 0xA5 made that ramp the home table and clamped it at 65535.
    if (dpi != 2400) {
        hp_match_fast_stepno_to_scan(regs);
    }
}

/*  Register 0x100 is the scan-buffer status (not 0x101, which is the
    home/motor bank). Data is flowing when bit 0 is set and bit 7 is
    clear (0x33 in the vendor-driver log). */
bool wait_for_cis_data(Genesys_Device* dev, unsigned timeout_ms)
{
    const unsigned step = 5;
    unsigned elapsed = 0;
    std::uint8_t last = 0xff;
    while (elapsed < timeout_ms) {
        std::uint8_t status = 0;
        try {
            status = dev->interface->read_register(0x100);
        } catch (...) {
            status = 0;
        }
        if (status != last) {
            DBG(DBG_info, "wait_for_cis_data: 0x100=0x%02x after %u ms\n",
                status, elapsed);
            last = status;
        }
        if ((status & 0x01) != 0 && (status & 0x80) == 0) {
            return true;
        }
        dev->interface->sleep_ms(step);
        elapsed += step;
    }
    DBG(DBG_error, "wait_for_cis_data: timeout, last 0x100=0x%02x\n", last);
    return false;
}

bool hp_park_and_wait(Genesys_Device* dev, unsigned timeout_ms);
bool hp_dummy_home_pass(Genesys_Device* dev);
void hp_write_vendor_park(Genesys_Device* dev);
bool hp_kick_park(Genesys_Device* dev);

struct HpShading {
    unsigned dpi = 0;
    unsigned width = 0;
    std::uint16_t target = 0;
    // Which model this reading was measured against -- the acceptance
    // floor below is per-model (sensors on the same GL848+ family don't
    // all reach the same brightness). Defaults to HP_SCANJET_200 so the
    // hardcoded scanjet200-app fallback files keep their original,
    // known-good threshold regardless of which device is running.
    ModelId model_id = ModelId::HP_SCANJET_200;
    std::array<std::vector<std::uint16_t>, 3> reference;
    // Per-column dark/offset reference (from a black sheet, via
    // --force-black-calibration). Empty when not yet measured -- flat-field
    // then falls back to gain-only correction (pre-existing behavior).
    std::array<std::vector<std::uint16_t>, 3> black_reference;

    bool has_black() const
    {
        return width > 0 &&
               black_reference[0].size() == width &&
               black_reference[1].size() == width &&
               black_reference[2].size() == width;
    }

    bool valid() const
    {
        return width > 0 && target > 4096 &&
               reference[0].size() == width &&
               reference[1].size() == width &&
               reference[2].size() == width;
    }
};

const unsigned kHpShadingDpis[] = { 300, 600, 1200, 2400 };

// 300/600/1200: a white sheet sits around 42300, a printed page ~22000.
// 2400: with the CIS LED GPIOs armed the same sheet is also ~40000;
// without them the pass integrates about half as much light (~23000).
//
// ponytail: HP_SCANJET_300 numbers are a guess, not a vendor-driver capture.
// This unit's CIS/lamp never reads above ~27000 at 300/600/1200 dpi (vs.
// ScanJet 200's ~42300) with tables cloned from ScanJet 200 -- real exposure
// or gain registers likely differ between the two sensors. Floor set to
// ~80% of the observed ceiling, scaled to 2400dpi with the same ratio
// ScanJet 200 uses. Replace with real numbers once a Windows USB trace of
// this specific model exists.
std::uint16_t hp_shading_min_white(unsigned dpi, ModelId model_id)
{
    if (model_id == ModelId::HP_SCANJET_300) {
        return dpi >= 2400 ? 12000 : 22000;
    }
    return dpi >= 2400 ? 18000 : 32000;
}

// Ceiling for accepting a row as "dark enough" during a black-reference
// capture. No dedicated vendor-verified black target exists for any model
// yet -- half the white floor is a simple derived guess, not a measured
// constant. Loosen or tighten if --force-black-calibration rejects a
// genuinely black sheet, or accepts a merely grey one.
std::uint16_t hp_shading_max_black(unsigned dpi, ModelId model_id)
{
    return static_cast<std::uint16_t>(hp_shading_min_white(dpi, model_id) / 2);
}

bool hp_shading_is_white_sheet(const HpShading& shading)
{
    return shading.valid() && shading.target >= hp_shading_min_white(shading.dpi, shading.model_id);
}

// Max black as a fraction of the white target. 10% left 300 dpi a little
// washed out on magazine type (halftone-averaged ink sits above 10%);
// 12.5% lets the 0.5th percentile of a 300 dpi frame crush that extra
// grey. 600 dpi already lands near the 0.5th percentile and is unchanged.
std::uint16_t hp_shading_black(const HpShading& shading)
{
    return static_cast<std::uint16_t>(shading.target / 8u);
}

std::string hp_shading_legacy_path(unsigned dpi)
{
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        home = std::getenv("USERPROFILE");
    }
    if (home == nullptr) {
        return std::string();
    }
    return std::string(home) + "/.sane/scanjet200-shading-" + std::to_string(dpi) + ".bin";
}

// Same SJSH format as ~/.sane, written by the HP Scanjet 200 application.
std::string hp_shading_companion_path(unsigned dpi)
{
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        return std::string();
    }
    return std::string(home) + "/Library/Application Support/scanjet/shading-" +
           std::to_string(dpi) + ".bin";
}

std::string hp_shading_path_sane(const Genesys_Device* dev, unsigned dpi)
{
    if (dev != nullptr && !dev->calib_file.empty()) {
        return host_shading_path(dev->calib_file, dpi);
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        home = std::getenv("USERPROFILE");
    }
    if (home == nullptr) {
        return std::string();
    }
    std::string stem = "hewlett-packard-scanjet-200";
    if (dev != nullptr && dev->model != nullptr && dev->model->name != nullptr) {
        stem = dev->model->name;
    }
    return std::string(home) + "/.sane/" + stem + "-shading-" + std::to_string(dpi) + ".bin";
}

std::map<std::string, HpShading> g_hp_shading_cache;

void hp_shading_cache_clear()
{
    g_hp_shading_cache.clear();
}

bool hp_shading_load(const std::string& path, HpShading& out)
{
    // SJSH is the original host-side shading file: 4-byte magic,
    // little-endian width and target, then three uint16 white planes.
    // SJS2 adds an optional black/offset block after that: 1-byte
    // has_black flag, then (if set) three more uint16 planes. Old SJSH
    // files still load fine, just without black_reference (gain-only
    // correction, same as before this format existed).
    if (path.empty()) {
        return false;
    }
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        return false;
    }
    char magic[4] = {};
    in.read(magic, 4);
    const bool is_v2 = std::memcmp(magic, "SJS2", 4) == 0;
    if (!in || (!is_v2 && std::memcmp(magic, "SJSH", 4) != 0)) {
        return false;
    }
    std::uint16_t width = 0;
    std::uint16_t target = 0;
    in.read(reinterpret_cast<char*>(&width), 2);
    in.read(reinterpret_cast<char*>(&target), 2);
    if (!in || width == 0) {
        return false;
    }
    HpShading s;
    s.width = width;
    s.target = target;
    for (unsigned c = 0; c < 3; ++c) {
        s.reference[c].resize(width);
        in.read(reinterpret_cast<char*>(s.reference[c].data()),
                static_cast<std::streamsize>(width * 2));
    }
    if (!in) {
        return false;
    }
    if (is_v2) {
        char has_black = 0;
        in.read(&has_black, 1);
        if (in && has_black != 0) {
            for (unsigned c = 0; c < 3; ++c) {
                s.black_reference[c].resize(width);
                in.read(reinterpret_cast<char*>(s.black_reference[c].data()),
                        static_cast<std::streamsize>(width * 2));
            }
            if (!in) {
                for (unsigned c = 0; c < 3; ++c) {
                    s.black_reference[c].clear();
                }
            }
        }
    }
    out = std::move(s);
    return out.valid();
}

void hp_shading_save(const std::string& path, const HpShading& shading)
{
    if (path.empty() || !hp_shading_is_white_sheet(shading)) {
        return;
    }
    const auto slash = path.rfind('/');
    if (slash != std::string::npos) {
        const std::string dir = path.substr(0, slash);
        mkdir(dir.c_str(), 0755);
    }
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) {
        DBG(DBG_error, "hp_shading_save: cannot write %s\n", path.c_str());
        return;
    }
    out.write("SJS2", 4);
    const std::uint16_t width = static_cast<std::uint16_t>(shading.width);
    out.write(reinterpret_cast<const char*>(&width), 2);
    out.write(reinterpret_cast<const char*>(&shading.target), 2);
    for (unsigned c = 0; c < 3; ++c) {
        out.write(reinterpret_cast<const char*>(shading.reference[c].data()),
                  static_cast<std::streamsize>(shading.width * 2));
    }
    const char has_black = shading.has_black() ? 1 : 0;
    out.write(&has_black, 1);
    if (has_black) {
        for (unsigned c = 0; c < 3; ++c) {
            out.write(reinterpret_cast<const char*>(shading.black_reference[c].data()),
                      static_cast<std::streamsize>(shading.width * 2));
        }
    }
}

bool hp_try_white_profile(const std::string& path, unsigned dpi, HpShading& out)
{
    HpShading loaded;
    if (!hp_shading_load(path, loaded)) {
        return false;
    }
    loaded.dpi = dpi;
    if (!hp_shading_is_white_sheet(loaded)) {
        return false;
    }
    out = std::move(loaded);
    return true;
}

HpShading hp_measure_shading(Genesys_Device* dev, const HpOpticalMode& mode)
{
    // 20 mm is enough at 300/600. At 2400 a 20 mm strip is still useful once
    // the CIS LED GPIOs are armed; skip rows whose centre band is darker
    // than a white sheet (lid gap after FEEDL).
    const unsigned mm = 20u;
    const unsigned rgb_rows = std::max(240u, (mode.dpi * mm + 12u) / 25u);
    const unsigned cis_lines = rgb_rows * 3;
    DBG(DBG_info, "hp_measure_shading: capturing white reference at %u dpi "
        "(%u RGB rows, %u mm)\n",
        mode.dpi, rgb_rows, mm);
    const unsigned samples = mode.samples_per_line;
    const unsigned x0 = samples * 15 / 100;
    const unsigned x1 = std::max(x0 + 1, samples * 85 / 100);
    const std::uint16_t min_white = hp_shading_min_white(mode.dpi, dev->model->model_id);

    Genesys_Register_Set regs = dev->initial_regs;
    for (std::uint16_t addr : { std::uint16_t{0x31}, std::uint16_t{0x34}, std::uint16_t{0x39} }) {
        if (!regs.has_reg(addr)) {
            regs.init_reg(addr, 0x00);
        }
    }
    apply_scan_regs_for_mode(&regs, mode.dpi);
    regs.set24(REG_LINCNT, cis_lines);
    regs.set24(REG_FEEDL, mode.feed_lines);
    send_hp_slope_tables(dev, mode.dpi);
    if (regs.has_reg(REG_0x01)) {
        regs.set8(REG_0x01, static_cast<std::uint8_t>(regs.get8(REG_0x01) & ~REG_0x01_SCAN));
    }

    dev->interface->write_registers(regs);
    hp_arm_motor(dev, &regs);
    dev->interface->write_register(REG_0x0D, REG_0x0D_CLRLNCNT);
    dev->interface->write_register(REG_0x0D, REG_0x0D_CLRMCNT);

    const std::uint8_t scan_01 = static_cast<std::uint8_t>(
        REG_0x01_CISSET | REG_0x01_DOGENB | REG_0x01_SCAN);
    dev->interface->write_register(REG_0x01, scan_01);
    scanner_start_action(*dev, true);
    if (!wait_for_cis_data(dev, 30000)) {
        hp_park_and_wait(dev, 10000);
        throw SaneException(SANE_STATUS_IO_ERROR, "shading capture: CIS did not start");
    }

    auto read_usb = [dev](std::size_t size, std::uint8_t* data)
    {
        dev->interface->bulk_read_data(0x45, data, size);
        return true;
    };

    const std::size_t usb_row = static_cast<std::size_t>(samples) * 2;
    ImagePipelineStack pipeline;
    auto& src = pipeline.push_first_node<ImagePipelineNodeBufferedCallableSource>(
        samples, cis_lines, PixelFormat::I16,
        static_cast<std::size_t>(align_multiple_ceil(usb_row * 32, 2)), read_usb);
    src.set_last_read_multiple(2);
    pipeline.push_node<ImagePipelineNodeSwap16BitEndian>();
    pipeline.push_node<ImagePipelineNodeMergeMonoLinesToColor>(ColorOrder::RGB);

    std::array<std::vector<std::uint64_t>, 3> sums;
    for (unsigned c = 0; c < 3; ++c) {
        sums[c].assign(samples, 0);
    }
    std::uint64_t count = 0;
    std::uint64_t skipped_dark = 0;
    std::uint16_t brightest = 0;
    std::vector<std::uint8_t> row(pipeline.get_output_row_bytes());
    const unsigned first = rgb_rows * 15 / 100;
    const unsigned last = rgb_rows * 85 / 100;
    for (unsigned y = 0; y < rgb_rows; ++y) {
        pipeline.get_next_row_data(row.data());
        if (y < first || y >= last || (y % 3) != 0) {
            continue;
        }
        std::uint64_t centre = 0;
        for (unsigned x = x0; x < x1; ++x) {
            centre += get_raw_channel_from_row(row.data(), x, 1, PixelFormat::RGB161616);
        }
        const std::uint16_t grow = static_cast<std::uint16_t>(centre / (x1 - x0));
        if (grow > brightest) {
            brightest = grow;
        }
        if (grow < min_white) {
            skipped_dark++;
            continue;
        }
        for (unsigned x = 0; x < samples; ++x) {
            for (unsigned c = 0; c < 3; ++c) {
                sums[c][x] += get_raw_channel_from_row(row.data(), x, c, PixelFormat::RGB161616);
            }
        }
        count++;
    }

    hp_park_and_wait(dev, 60000);
    dev->set_head_pos_zero(ScanHeadId::PRIMARY);

    if (count < 30) {
        throw SaneException("not a white sheet (bright rows=%llu, dark rows=%llu, "
                            "brightest=%u, need >= %u) - keep the previous profile",
                            static_cast<unsigned long long>(count),
                            static_cast<unsigned long long>(skipped_dark),
                            brightest, min_white);
    }

    HpShading shading;
    shading.dpi = mode.dpi;
    shading.model_id = dev->model->model_id;
    shading.width = samples;
    for (unsigned c = 0; c < 3; ++c) {
        shading.reference[c].resize(samples);
        for (unsigned x = 0; x < samples; ++x) {
            shading.reference[c][x] = static_cast<std::uint16_t>(
                std::min<std::uint64_t>(65535, sums[c][x] / count));
        }
    }
    auto green = shading.reference[1];
    std::nth_element(green.begin() + static_cast<std::ptrdiff_t>(x0),
                     green.begin() + static_cast<std::ptrdiff_t>((x0 + x1) / 2),
                     green.begin() + static_cast<std::ptrdiff_t>(x1));
    shading.target = green[(x0 + x1) / 2];
    auto gmin = *std::min_element(shading.reference[1].begin(), shading.reference[1].end());
    auto gmax = *std::max_element(shading.reference[1].begin(), shading.reference[1].end());
    DBG(DBG_info, "hp_measure_shading: target=%u count=%llu skipped_dark=%llu green min=%u max=%u\n",
        shading.target, static_cast<unsigned long long>(count),
        static_cast<unsigned long long>(skipped_dark), gmin, gmax);
    if (!hp_shading_is_white_sheet(shading)) {
        throw SaneException("not a white sheet (target=%u, need >= %u) - keep the previous profile",
                            shading.target, hp_shading_min_white(mode.dpi, dev->model->model_id));
    }
    return shading;
}

// Second calibration point: per-column dark/offset reference, captured the
// same way as hp_measure_shading but with a black/dark sheet on the bed
// instead of white, and validating darkness instead of brightness. Fills
// shading.black_reference in place -- shading must already carry a valid
// white reference (width/dpi/target) from hp_measure_shading or a loaded
// file; this never touches shading.reference or shading.target.
void hp_measure_black_shading(Genesys_Device* dev, const HpOpticalMode& mode, HpShading& shading)
{
    const unsigned mm = 20u;
    const unsigned rgb_rows = std::max(240u, (mode.dpi * mm + 12u) / 25u);
    const unsigned cis_lines = rgb_rows * 3;
    DBG(DBG_info, "hp_measure_black_shading: capturing black reference at %u dpi "
        "(%u RGB rows, %u mm)\n", mode.dpi, rgb_rows, mm);
    const unsigned samples = mode.samples_per_line;
    const unsigned x0 = samples * 15 / 100;
    const unsigned x1 = std::max(x0 + 1, samples * 85 / 100);
    const std::uint16_t max_black = hp_shading_max_black(mode.dpi, dev->model->model_id);

    Genesys_Register_Set regs = dev->initial_regs;
    for (std::uint16_t addr : { std::uint16_t{0x31}, std::uint16_t{0x34}, std::uint16_t{0x39} }) {
        if (!regs.has_reg(addr)) {
            regs.init_reg(addr, 0x00);
        }
    }
    apply_scan_regs_for_mode(&regs, mode.dpi);
    regs.set24(REG_LINCNT, cis_lines);
    regs.set24(REG_FEEDL, mode.feed_lines);
    send_hp_slope_tables(dev, mode.dpi);
    if (regs.has_reg(REG_0x01)) {
        regs.set8(REG_0x01, static_cast<std::uint8_t>(regs.get8(REG_0x01) & ~REG_0x01_SCAN));
    }

    dev->interface->write_registers(regs);
    hp_arm_motor(dev, &regs);
    dev->interface->write_register(REG_0x0D, REG_0x0D_CLRLNCNT);
    dev->interface->write_register(REG_0x0D, REG_0x0D_CLRMCNT);

    const std::uint8_t scan_01 = static_cast<std::uint8_t>(
        REG_0x01_CISSET | REG_0x01_DOGENB | REG_0x01_SCAN);
    dev->interface->write_register(REG_0x01, scan_01);
    scanner_start_action(*dev, true);
    if (!wait_for_cis_data(dev, 30000)) {
        hp_park_and_wait(dev, 10000);
        throw SaneException(SANE_STATUS_IO_ERROR, "black shading capture: CIS did not start");
    }

    auto read_usb = [dev](std::size_t size, std::uint8_t* data)
    {
        dev->interface->bulk_read_data(0x45, data, size);
        return true;
    };

    const std::size_t usb_row = static_cast<std::size_t>(samples) * 2;
    ImagePipelineStack pipeline;
    auto& src = pipeline.push_first_node<ImagePipelineNodeBufferedCallableSource>(
        samples, cis_lines, PixelFormat::I16,
        static_cast<std::size_t>(align_multiple_ceil(usb_row * 32, 2)), read_usb);
    src.set_last_read_multiple(2);
    pipeline.push_node<ImagePipelineNodeSwap16BitEndian>();
    pipeline.push_node<ImagePipelineNodeMergeMonoLinesToColor>(ColorOrder::RGB);

    std::array<std::vector<std::uint64_t>, 3> sums;
    for (unsigned c = 0; c < 3; ++c) {
        sums[c].assign(samples, 0);
    }
    std::uint64_t count = 0;
    std::uint64_t skipped_bright = 0;
    std::uint16_t darkest = 65535;
    std::vector<std::uint8_t> row(pipeline.get_output_row_bytes());
    const unsigned first = rgb_rows * 15 / 100;
    const unsigned last = rgb_rows * 85 / 100;
    for (unsigned y = 0; y < rgb_rows; ++y) {
        pipeline.get_next_row_data(row.data());
        if (y < first || y >= last || (y % 3) != 0) {
            continue;
        }
        std::uint64_t centre = 0;
        for (unsigned x = x0; x < x1; ++x) {
            centre += get_raw_channel_from_row(row.data(), x, 1, PixelFormat::RGB161616);
        }
        const std::uint16_t grow = static_cast<std::uint16_t>(centre / (x1 - x0));
        if (grow < darkest) {
            darkest = grow;
        }
        if (grow > max_black) {
            skipped_bright++;
            continue;
        }
        for (unsigned x = 0; x < samples; ++x) {
            for (unsigned c = 0; c < 3; ++c) {
                sums[c][x] += get_raw_channel_from_row(row.data(), x, c, PixelFormat::RGB161616);
            }
        }
        count++;
    }

    hp_park_and_wait(dev, 60000);
    dev->set_head_pos_zero(ScanHeadId::PRIMARY);

    if (count < 30) {
        throw SaneException("not a black sheet (dark rows=%llu, bright rows=%llu, "
                            "darkest=%u, need <= %u) - keep the previous profile",
                            static_cast<unsigned long long>(count),
                            static_cast<unsigned long long>(skipped_bright),
                            darkest, max_black);
    }

    for (unsigned c = 0; c < 3; ++c) {
        shading.black_reference[c].assign(samples, 0);
        for (unsigned x = 0; x < samples; ++x) {
            shading.black_reference[c][x] = static_cast<std::uint16_t>(
                std::min<std::uint64_t>(65535, sums[c][x] / count));
        }
    }
    DBG(DBG_info, "hp_measure_black_shading: count=%llu skipped_bright=%llu darkest=%u\n",
        static_cast<unsigned long long>(count), static_cast<unsigned long long>(skipped_bright), darkest);
}

HpShading hp_ensure_shading(Genesys_Device* dev, const HpOpticalMode& mode)
{
    if (is_testing_mode()) {
        return HpShading{};
    }

    const std::string sane_path = hp_shading_path_sane(dev, mode.dpi);
    const bool force_white = (dev != nullptr && dev->force_calibration != 0);
    const bool force_black = (dev != nullptr && dev->force_black_calibration != 0);

    HpShading shading;
    bool have_white = false;
    bool should_persist = false;

    if (!force_white) {
        if (!sane_path.empty()) {
            auto cached = g_hp_shading_cache.find(sane_path);
            if (cached != g_hp_shading_cache.end() && hp_shading_is_white_sheet(cached->second)) {
                shading = cached->second;
                have_white = true;
            }
        }
        if (!have_white && hp_shading_load(sane_path, shading)) {
            shading.dpi = mode.dpi;
            shading.model_id = dev->model->model_id;
            if (hp_shading_is_white_sheet(shading)) {
                have_white = true;
                DBG(DBG_info, "hp_ensure_shading: loaded %s target=%u has_black=%d\n",
                    sane_path.c_str(), shading.target, shading.has_black());
            } else {
                DBG(DBG_warn, "hp_ensure_shading: ignoring dark profile %s target=%u "
                    "(need a blank white sheet, target>=%u)\n",
                    sane_path.c_str(), shading.target, hp_shading_min_white(mode.dpi, dev->model->model_id));
                shading = HpShading{};
            }
        }
        if (!have_white) {
            const std::string fallbacks[] = {
                hp_shading_legacy_path(mode.dpi),
                hp_shading_companion_path(mode.dpi),
            };
            for (const std::string& fallback : fallbacks) {
                if (hp_try_white_profile(fallback, mode.dpi, shading)) {
                    have_white = true;
                    should_persist = true;
                    DBG(DBG_info, "hp_ensure_shading: loaded %s target=%u\n",
                        fallback.c_str(), shading.target);
                    break;
                }
            }
        }
        if (!have_white) {
            DBG(DBG_info, "hp_ensure_shading: no profile for %u dpi — scan without flat-field "
                          "(use --force-calibration with a white sheet)\n", mode.dpi);
        }
    } else {
        try {
            DBG(DBG_info, "hp_ensure_shading: force-calibration at %u dpi — scanning the white sheet\n",
                mode.dpi);
            shading = hp_measure_shading(dev, mode);
            have_white = true;
            should_persist = true;
            DBG(DBG_info, "hp_ensure_shading: measured white target=%u\n", shading.target);
        } catch (const std::exception& e) {
            DBG(DBG_warn, "hp_ensure_shading: white measure failed: %s\n", e.what());
            const std::string keep[] = {
                sane_path,
                hp_shading_legacy_path(mode.dpi),
                hp_shading_companion_path(mode.dpi),
            };
            for (const std::string& path : keep) {
                if (hp_try_white_profile(path, mode.dpi, shading)) {
                    have_white = true;
                    should_persist = true;
                    DBG(DBG_info, "hp_ensure_shading: keeping %s target=%u\n",
                        path.c_str(), shading.target);
                    break;
                }
            }
        }
        if (dev != nullptr) {
            dev->force_calibration = 0;
        }
    }

    if (force_black) {
        if (have_white) {
            try {
                DBG(DBG_info, "hp_ensure_shading: force-black-calibration at %u dpi — "
                              "scanning the black sheet\n", mode.dpi);
                hp_measure_black_shading(dev, mode, shading);
                should_persist = true;
                DBG(DBG_info, "hp_ensure_shading: measured black reference for %u dpi\n", mode.dpi);
            } catch (const std::exception& e) {
                DBG(DBG_warn, "hp_ensure_shading: black measure failed: %s\n", e.what());
            }
        } else {
            DBG(DBG_warn, "hp_ensure_shading: force-black-calibration requested but no white "
                          "reference yet for %u dpi — run --force-calibration with a white sheet "
                          "first\n", mode.dpi);
        }
        if (dev != nullptr) {
            dev->force_black_calibration = 0;
        }
    }

    if (have_white) {
        if (should_persist) {
            hp_shading_save(sane_path, shading);
        }
        g_hp_shading_cache[sane_path] = shading;
        return shading;
    }

    return HpShading{};
}

/*  Hardware always emits a full-width 16-bit CIS stream: samples_per_line
    pixels, three mono lines per RGB row, big-endian samples. User DPI and
    the SANE rectangle are host-side: un-mirror, crop,
    integer box-filter, then 16→8 if the frontend asked for 8-bit. */
struct HpPassGeometry {
    HpOpticalMode mode;
    unsigned scale_x;
    unsigned scale_y;
    unsigned optical_x0;
    unsigned optical_xw;
    unsigned skip_y;
    unsigned color_rows;
    unsigned cis_lines;
    unsigned feed_lines;
};

HpPassGeometry hp_compute_pass(const Genesys_Device& dev, const ScanSession& session)
{
    const Genesys_Settings& s = dev.settings;
    float x0 = s.tl_x;
    float y0 = s.tl_y;
    if (!dev.ignore_offsets && dev.model != nullptr) {
        x0 += dev.model->x_offset;
        y0 += dev.model->y_offset;
    }
    float width = s.scan_width_mm;
    float height = s.scan_height_mm;
    if (width <= 0.f) {
        width = kCisWidthMM;
        x0 = 0.f;
    }
    if (height <= 0.f) {
        height = kCisHeightMM;
        y0 = 0.f;
    }

    const PassGeometry pg = compute_pass_geometry(session.params.xres, session.params.yres,
                                                  x0, y0, x0 + width, y0 + height);
    HpPassGeometry g;
    g.mode.dpi = pg.pass.dpi;
    g.mode.samples_per_line = pg.pass.samples_per_line;
    g.mode.page_lines = pg.pass.page_lines;
    g.mode.feed_lines = pg.pass.feed_lines;
    g.scale_x = pg.scale_x;
    g.scale_y = pg.scale_y;
    g.optical_x0 = pg.optical_x0;
    g.optical_xw = pg.optical_xw;
    g.skip_y = pg.skip_y;
    g.color_rows = pg.color_rows;
    g.cis_lines = pg.cis_lines;
    g.feed_lines = pg.pass.feed_lines;
    return g;
}

void setup_hp_scanjet_pipeline(Genesys_Device& dev, const ScanSession& session,
                               const HpPassGeometry& geom, const HpShading* shading)
{
    const unsigned samples = geom.mode.samples_per_line;
    const std::size_t usb_row_bytes = static_cast<std::size_t>(samples) * 2;
    std::size_t buffer_size = usb_row_bytes * 32;
    buffer_size = static_cast<std::size_t>(align_multiple_ceil(buffer_size, 2));

    const unsigned home_dpi = geom.mode.dpi;
    const std::size_t usb_total = static_cast<std::size_t>(geom.cis_lines) * usb_row_bytes;
    auto usb_got = std::make_shared<std::size_t>(0);
    auto read_data_from_usb = [&dev, usb_got, usb_total, home_dpi](std::size_t size,
                                                                  std::uint8_t* data)
    {
        DBG(DBG_info, "hp_read_usb: %zu bytes\n", size);
        dev.interface->bulk_read_data(0x45, data, size);
        *usb_got += size;
        // AutoLevels at 2400 fills a temp file then stretches the PNG.
        // Drop SCAN now so AGOHOME starts during LUT, same as the
        // vendor driver. Do not start a new 0x0F pass from the field:
        // that leaves the motor humming with no steps.
        if (home_dpi == 2400 && *usb_got >= usb_total) {
            DBG(DBG_info, "hp_read_usb: last CIS row, vendor park\n");
            hp_kick_park(&dev);
        }
        return true;
    };

    ImagePipelineStack pipeline;
    auto& src = pipeline.push_first_node<ImagePipelineNodeBufferedCallableSource>(
        samples, geom.cis_lines, PixelFormat::I16, buffer_size, read_data_from_usb);
    src.set_last_read_multiple(2);

    // USB 16-bit samples are big-endian. SJSH columns are raw CIS order
    // (first sample = first USB pixel); un-mirror after the flat-field.
    pipeline.push_node<ImagePipelineNodeSwap16BitEndian>();
    pipeline.push_node<ImagePipelineNodeMergeMonoLinesToColor>(ColorOrder::RGB);
    if (shading != nullptr && shading->valid() && shading->width == samples) {
        DBG(DBG_info, "hp_pipeline: flat-field %u dpi width=%u target=%u has_black=%d\n",
            geom.mode.dpi, shading->width, shading->target, shading->has_black());
        if (shading->has_black()) {
            pipeline.push_node<ImagePipelineNodeFlatField>(
                shading->reference[0], shading->reference[1], shading->reference[2],
                shading->black_reference[0], shading->black_reference[1], shading->black_reference[2],
                shading->target);
        } else {
            pipeline.push_node<ImagePipelineNodeFlatField>(
                shading->reference[0], shading->reference[1], shading->reference[2],
                shading->target);
        }
    } else if (shading != nullptr && shading->valid()) {
        DBG(DBG_warn, "hp_pipeline: shading width %u != CIS %u — skipping flat-field\n",
            shading->width, samples);
    } else {
        DBG(DBG_info, "hp_pipeline: no shading at %u dpi — AutoLevels fallback\n",
            geom.mode.dpi);
    }
    pipeline.push_node<ImagePipelineNodeMirrorRows>();
    pipeline.push_node<ImagePipelineNodeExtract>(geom.optical_x0, geom.skip_y,
                                                 geom.optical_xw, geom.color_rows);

    const bool gray = session.params.channels == 1;
    const bool lineart = session.params.scan_mode == ScanColorMode::LINEART;
    if (gray) {
        // Rec.601 integer weights, applied at optical resolution like the
        // reference decoder (r*77 + g*150 + b*29) >> 8.
        pipeline.push_node<ImagePipelineNodeMergeColorToGray>(77u, 150u, 29u, 256u);
    }

    if (geom.scale_y > 1) {
        pipeline.push_node<ImagePipelineNodeDecimateLines>(geom.scale_y);
    }
    if (geom.scale_x > 1) {
        pipeline.push_node<ImagePipelineNodeDecimateColumns>(geom.scale_x);
    }

    const unsigned requested = session.params.get_requested_pixels();
    if (pipeline.get_output_width() != requested) {
        pipeline.push_node<ImagePipelineNodeScaleRows>(requested);
    }

    if (shading != nullptr && shading->valid()) {
        pipeline.push_node<ImagePipelineNodeAutoLevels>(
            hp_shading_black(*shading), shading->target, true);
    } else {
        pipeline.push_node<ImagePipelineNodeAutoLevels>();
    }

    if (session.params.depth == 8) {
        auto out = pipeline.get_output_format();
        if (out == PixelFormat::I16) {
            pipeline.push_node<ImagePipelineNodeFormatConvert>(PixelFormat::I8);
        } else if (out == PixelFormat::RGB161616) {
            pipeline.push_node<ImagePipelineNodeFormatConvert>(PixelFormat::RGB888);
        }
    }

    if (lineart && pipeline.get_output_format() == PixelFormat::I8) {
        pipeline.push_node<ImagePipelineNodeThreshold8>(180);
    }

    dev.pipeline = std::move(pipeline);

    auto read_from_pipeline = [&dev](std::size_t size, std::uint8_t* out_data)
    {
        (void) size;
        return dev.pipeline.get_next_row_data(out_data);
    };
    dev.pipeline_buffer = ImageBuffer{dev.pipeline.get_output_row_bytes(),
                                       read_from_pipeline};
}

} // namespace

std::string host_shading_path(const std::string& calib_file, unsigned dpi)
{
    std::string base = calib_file;
    const char suffix[] = ".cal";
    const std::size_t suffix_len = 4;
    if (base.size() >= suffix_len &&
        base.compare(base.size() - suffix_len, suffix_len, suffix) == 0) {
        base.resize(base.size() - suffix_len);
    }
    return base + "-shading-" + std::to_string(dpi) + ".bin";
}

OpticalPass choose_optical_pass(unsigned user_dpi)
{
    unsigned scale = 1;
    const HpOpticalMode mode = hp_choose_mode(user_dpi, scale);
    OpticalPass pass;
    pass.dpi = mode.dpi;
    pass.samples_per_line = mode.samples_per_line;
    pass.page_lines = mode.page_lines;
    pass.feed_lines = mode.feed_lines;
    pass.scale = scale;
    return pass;
}

static unsigned round_nn(double x)
{
    if (x <= 0.0) {
        return 0;
    }
    return static_cast<unsigned>(std::floor(x + 0.5));
}

PassGeometry compute_pass_geometry(unsigned xres, unsigned yres,
                                   float glass_x0_mm, float glass_y0_mm,
                                   float glass_x1_mm, float glass_y1_mm)
{
    (void) yres;
    PassGeometry out;
    out.pass = choose_optical_pass(xres);
    out.scale_x = out.pass.scale;
    out.scale_y = out.pass.scale;
    if (yres != 0 && out.pass.dpi != 0 && out.pass.dpi % yres == 0) {
        out.scale_y = out.pass.dpi / yres;
    }

    const unsigned samples = out.pass.samples_per_line;
    const unsigned scale = std::max(1u, out.pass.scale);
    const unsigned page_rows = out.pass.page_lines / 3;
    if (glass_x1_mm < glass_x0_mm) {
        std::swap(glass_x0_mm, glass_x1_mm);
    }
    if (glass_y1_mm < glass_y0_mm) {
        std::swap(glass_y0_mm, glass_y1_mm);
    }

    unsigned crop_start = round_nn(static_cast<double>(glass_x0_mm) / kCisWidthMM * samples);
    unsigned crop_end = round_nn(static_cast<double>(glass_x1_mm) / kCisWidthMM * samples);
    if (crop_end > samples) {
        crop_end = samples;
    }
    if (crop_start > crop_end) {
        crop_start = crop_end;
    }

    unsigned skip_raw = round_nn(static_cast<double>(page_rows) * glass_y0_mm / kCisHeightMM);
    skip_raw = skip_raw / scale * scale;
    const float height_mm = glass_y1_mm - glass_y0_mm;
    unsigned wanted = round_nn(static_cast<double>(page_rows) * height_mm / kCisHeightMM);
    if (wanted < scale) {
        wanted = scale;
    }
    unsigned rows = wanted / scale * scale;
    if (skip_raw >= page_rows) {
        skip_raw = page_rows / scale * scale;
        if (skip_raw >= page_rows && page_rows >= scale) {
            skip_raw = page_rows - scale;
        }
    }
    const unsigned remaining = page_rows - skip_raw;
    if (rows > remaining) {
        rows = remaining / scale * scale;
    }
    if (rows < scale) {
        rows = scale;
    }

    const unsigned cropped = crop_end - crop_start;
    out.output_width = std::max(1u, cropped / scale);
    out.output_height = rows / scale;
    out.optical_x0 = crop_start;
    out.optical_xw = out.output_width * scale;
    out.skip_y = skip_raw;
    out.color_rows = rows;
    out.cis_lines = (out.skip_y + out.color_rows) * 3;
    return out;
}

// ----------------------------------------------------------------------------
// Trivial / no-op overrides
// ----------------------------------------------------------------------------

bool CommandSetGl848::needs_home_before_init_regs_for_scan(Genesys_Device* /*dev*/) const
{
    return false;
}

void CommandSetGl848::init(Genesys_Device* dev) const
{
    DBG_INIT();
    DBG_HELPER(dbg);
    sanei_genesys_asic_init(dev);
}

void CommandSetGl848::update_hardware_sensors(Genesys_Scanner* /*s*/) const
{
    // HP Scanjet 200 has no front-panel buttons.
}

void CommandSetGl848::update_home_sensor_gpio(Genesys_Device& /*dev*/) const
{
    // The Windows driver never touches the GPIO enable bank on this device.
}

void CommandSetGl848::set_powersaving(Genesys_Device* /*dev*/, int /*delay*/) const
{
    // Windows driver never sets a powersave delay on the Scanjet 200.
}

void CommandSetGl848::save_power(Genesys_Device* /*dev*/, bool /*enable*/) const
{
}

void CommandSetGl848::load_document(Genesys_Device* /*dev*/) const
{
    // Flatbed only.
}

void CommandSetGl848::detect_document_end(Genesys_Device* /*dev*/) const
{
}

void CommandSetGl848::eject_document(Genesys_Device* /*dev*/) const
{
}

void CommandSetGl848::set_fe(Genesys_Device* /*dev*/, const Genesys_Sensor& /*sensor*/,
                             std::uint8_t /*set*/) const
{
    // AFE is embedded and driven implicitly by the register block written
    // in asic_boot(); the HP Windows driver never touches AFE registers
    // outside that block, so we mirror that here.
    DBG_HELPER(dbg);
}

void CommandSetGl848::offset_calibration(Genesys_Device* /*dev*/, const Genesys_Sensor& /*sensor*/,
                                        Genesys_Register_Set& /*regs*/) const
{
    // Dark is a host-side floor: min(0.5th percentile of the frame,
    // analog tail of the white reference, 10% of the shading target).
    DBG_HELPER(dbg);
}

void CommandSetGl848::coarse_gain_calibration(Genesys_Device* /*dev*/,
                                              const Genesys_Sensor& /*sensor*/,
                                              Genesys_Register_Set& /*regs*/, int /*dpi*/) const
{
    // Gain is fixed on this AFE.
    DBG_HELPER(dbg);
}

SensorExposure CommandSetGl848::led_calibration(Genesys_Device* /*dev*/,
                                                const Genesys_Sensor& sensor,
                                                Genesys_Register_Set& /*regs*/) const
{
    // The HP driver does not perform LED calibration; return the exposure
    // stored in the sensor table so the caller has a valid struct.
    DBG_HELPER(dbg);
    return sensor.exposure;
}

void CommandSetGl848::send_shading_data(Genesys_Device* /*dev*/, const Genesys_Sensor& /*sensor*/,
                                        std::uint8_t* /*data*/, int size) const
{
    // Host-side shading: the driver keeps the shading correction in RAM and
    // applies it during frame decoding, so nothing needs to be uploaded to
    // the ASIC. We accept the buffer for API completeness.
    DBG_HELPER_ARGS(dbg, "discarding %d bytes of shading data (host-side)", size);
    (void) size;
}

void CommandSetGl848::clear_host_side_calibration(Genesys_Device* dev) const
{
    hp_shading_cache_clear();
    for (unsigned dpi : kHpShadingDpis) {
        const std::string path = hp_shading_path_sane(dev, dpi);
        if (!path.empty()) {
            unlink(path.c_str());
        }
        const std::string legacy = hp_shading_legacy_path(dpi);
        if (!legacy.empty()) {
            unlink(legacy.c_str());
        }
    }
}

void CommandSetGl848::send_gamma_table(Genesys_Device* dev, const Genesys_Sensor& sensor) const
{
    // The captured HP gamma table on GL848+ is a linear 16-bit LE ramp per
    // channel written to AHB 0x0100_0000 + 0x200 * channel. The generic
    // helper writes to the same address family for GL847-class ASICs, so
    // it is safe to reuse here.
    DBG_HELPER(dbg);
    sanei_genesys_send_gamma_table(dev, sensor);
}

void CommandSetGl848::wait_for_motor_stop(Genesys_Device* /*dev*/) const
{
    // The GL848+ home / stop condition is polled through REG_STATUS by
    // scanner_move_back_home(); begin_scan / end_scan do not need to
    // spin here.
}

// ----------------------------------------------------------------------------
// Scan-session calculation (used by sane_open to populate SANE_Parameters)
// ----------------------------------------------------------------------------

ScanSession CommandSetGl848::calculate_scan_session(const Genesys_Device* dev,
                                                    const Genesys_Sensor& sensor,
                                                    const Genesys_Settings& settings) const
{
    DBG_HELPER(dbg);
    debug_dump(DBG_info, settings);

    // No backtracking on this scanner — the Windows driver never uses it.
    ScanFlag flags = ScanFlag::DISABLE_BUFFER_FULL_MOVE;

    // X/Y crop is host-side on this CIS; FEEDL is park-to-page only, so the
    // session origin is zero and pixels/lines already match the output size.
    ScanSession session;
    session.params.xres = settings.xres;
    session.params.yres = settings.yres;
    session.params.startx = 0;
    session.params.starty = 0;
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
    session.params.exposure_lperiod = settings.exposure_lperiod;
    session.params.flags = flags;

    compute_session(dev, session, sensor);
    return session;
}

// ----------------------------------------------------------------------------
// asic_boot: apply the reverse-engineered HP baseline register block +
// motor slope + gamma tables. This is what puts the ASIC into a state
// from which a pass can be started.
// ----------------------------------------------------------------------------

namespace {

/*  Write the linear 16-bit LE gamma ramp (0..255) that the vendor driver
    uses. Address family is 0x01000000 + 0x200 * channel. */
void send_hp_gamma_ramp(Genesys_Device* dev)
{
    std::vector<std::uint8_t> table(512, 0);
    for (unsigned i = 0; i < 256; ++i) {
        table[i * 2 + 0] = static_cast<std::uint8_t>(i);
        table[i * 2 + 1] = 0;
    }
    for (unsigned channel = 0; channel < 3; ++channel) {
        std::uint32_t addr = 0x01000000u + 0x200u * channel;
        dev->interface->write_ahb(addr, static_cast<std::uint32_t>(table.size()),
                                  table.data());
    }
    dev->interface->write_register(0xbd, 0x00);
    dev->interface->write_register(0xbe, 0x07);
}

/*  64024-byte table at AHB 0x10014000. The vendor driver writes it on
    every optical pass, before the slope slots. After a short
    uninitialized host-buffer prefix the capture is this 24-byte period
    (8003 × 8-byte records). 300 dpi can run without it; 600+ uses the
    slot as the per-line motor clock. */
void send_hp_motor_clock_table(Genesys_Device* dev)
{
    static const std::uint8_t period[24] = {
        0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0xaa, 0x0a,
        0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20,
        0x00, 0x00, 0xaa, 0x0a, 0x00, 0x00, 0x00, 0x20
    };
    const unsigned kSize = 64024;
    std::vector<std::uint8_t> table(kSize);
    for (unsigned i = 0; i < kSize; ++i) {
        table[i] = period[i % 24];
    }
    dev->interface->write_ahb(0x10014000u, kSize, table.data());
}

/*  Upload per-mode motor slope tables. 0xA0 selects the step type for the
    slots that follow (Windows-driver capture): 0x08/0x00 at 300 dpi,
    0x09/0x01 at 600, 0x0B/0x03 at 1200/2400. Table length is written to
    0xA5/0xAB/0xAD/0xAF/0xB1 immediately after each slot. AHB payload
    length must match the capture (8 / 2 / 48 / 118) — the ASIC takes
    the transfer size as the table size. Must run before the scan
    overlay sets 0x3C=0xEF (LED clock). */
void send_hp_slope_tables(Genesys_Device* dev, unsigned dpi)
{
    std::vector<std::uint8_t> scan_slope;
    std::vector<std::uint8_t> feed_slope;
    std::uint8_t a0_fast = 0x08;
    std::uint8_t a0_slow = 0x00;
    std::uint8_t steps_fast = 0x3b;
    std::uint8_t steps_slow = 0x3b;
    std::uint8_t fwd_hi = 0x00;
    std::uint8_t fwd_lo = 0xc8;
    // Vendor capture: short table in slots 0–2 and scan table in 3–4.
    // 600/1200 stretch the scan table into every slot so a pass that
    // never leaves 0–2 still matches CIS timing:
    //   600  ×15/8  — ×2 is ~1.0× on an 80 mm crop (ramp-dominated) but
    //                 a full sheet undershoots: the carriage stops a few
    //                 cm early. ×15/8 speeds cruise ~6%.
    //   1200 ×9/5   — ×2 stretched ~1.10× (more of the page missing)
    // 2400 must keep the capture split. Stretching the 24-word scan
    // table ×36/5 into slots 0–2 clamped the ramp at 65535, so AGOHOME
    // crawled. The capture uses a one-word table (0x3d6c, FASTNO=1)
    // there; 0x0A/0x01 then homes at that cruise.
    unsigned slope_num = 1;
    unsigned slope_den = 1;
    bool vendor_fast_slots = false;
    if (dpi == 600) {
        scan_slope.assign(hp_scanjet_200_slope.begin(), hp_scanjet_200_slope.end());
        slope_num = 15;
        slope_den = 8;
        a0_fast = 0x09;
        a0_slow = 0x01;
        steps_fast = 0x3b;
        fwd_hi = 0x01;
        fwd_lo = 0x90;
    } else if (dpi == 1200) {
        scan_slope.assign(hp_slope_1200_slow, hp_slope_1200_slow + 48);
        slope_num = 9;
        slope_den = 5;
        a0_fast = 0x0b;
        a0_slow = 0x03;
        steps_fast = 0x18;
        steps_slow = 0x18;
        fwd_hi = 0x00;
        fwd_lo = 0x00;
    } else if (dpi == 2400) {
        scan_slope.assign(hp_slope_1200_slow, hp_slope_1200_slow + 48);
        feed_slope.assign(hp_slope_2400_fast, hp_slope_2400_fast + 2);
        vendor_fast_slots = true;
        a0_fast = 0x0b;
        a0_slow = 0x03;
        steps_fast = 0x01;
        steps_slow = 0x18;
        fwd_hi = 0x00;
        fwd_lo = 0x00;
    } else {
        scan_slope.assign(hp_scanjet_200_slope.begin(), hp_scanjet_200_slope.end());
    }
    if (slope_num != 1 || slope_den != 1) {
        hp_scale_slope_le16(scan_slope, slope_num, slope_den);
    }
    g_hp_last_slope_dpi = dpi;
    const std::uint8_t* fast = vendor_fast_slots ? feed_slope.data() : scan_slope.data();
    unsigned fast_len = vendor_fast_slots ? static_cast<unsigned>(feed_slope.size())
                                           : static_cast<unsigned>(scan_slope.size());
    const std::uint8_t* slow = scan_slope.data();
    unsigned slow_len = static_cast<unsigned>(scan_slope.size());

    DBG(DBG_info, "send_hp_slope_tables: dpi=%u a0_fast=0x%02x steps_fast=%u fast_len=%u "
        "slope_scale=%u/%u\n",
        dpi, a0_fast, steps_fast, fast_len, slope_num, slope_den);

    send_hp_motor_clock_table(dev);

    auto write_slot = [&](unsigned n, const std::uint8_t* data, unsigned len) {
        std::uint32_t addr = 0x10000000u + 0x4000u * n;
        std::vector<std::uint8_t> slot(data, data + len);
        dev->interface->write_ahb(addr, len, slot.data());
    };

    // Motor power on, FASTFED off — same fence as the vendor capture
    // immediately before the slope AHB writes.
    dev->interface->write_register(0x02, 0x10);
    dev->interface->write_register(0xa6, fwd_hi);
    dev->interface->write_register(0xa7, fwd_lo);
    dev->interface->write_register(0xa8, fwd_hi);
    dev->interface->write_register(0xa9, fwd_lo);

    dev->interface->write_register(0xa0, a0_fast);
    dev->interface->write_register(0x3c, 0x0a);
    write_slot(0, fast, fast_len);
    dev->interface->write_register(0xa4, 0x00);
    dev->interface->write_register(0xa5, steps_fast);
    write_slot(1, fast, fast_len);
    dev->interface->write_register(0xaa, 0x00);
    dev->interface->write_register(0xab, steps_fast);
    write_slot(2, fast, fast_len);
    dev->interface->write_register(0xac, 0x00);
    dev->interface->write_register(0xad, steps_fast);
    dev->interface->write_register(0xa0, a0_slow);
    dev->interface->write_register(0x3c, 0xaa);
    write_slot(3, slow, slow_len);
    dev->interface->write_register(0xae, 0x00);
    dev->interface->write_register(0xaf, steps_slow);
    write_slot(4, slow, slow_len);
    dev->interface->write_register(0xb0, 0x00);
    dev->interface->write_register(0xb1, steps_slow);

    // Vendor capture after the slope AHB writes: FEEDL=1, drop motor
    // power, then a short-lived 0x28..0x2a triple. write_registers()
    // restores the per-mode overlay (including real FEEDL) afterwards.
    dev->interface->write_register(0x3d, 0x00);
    dev->interface->write_register(0x3e, 0x00);
    dev->interface->write_register(0x3f, 0x01);
    dev->interface->write_register(0x02, 0x10);
    dev->interface->write_register(0x02, 0x00);
    std::uint8_t d29 = 0x3c;
    std::uint8_t d2a = 0x78;
    if (dpi == 600) {
        d29 = 0x78;
        d2a = 0xf0;
    } else if (dpi == 1200) {
        d29 = 0xf1;
        d2a = 0xe0;
    } else if (dpi == 2400) {
        d29 = 0xf1;
        d2a = 0xe0;
    }
    dev->interface->write_register(0x28, 0x00);
    dev->interface->write_register(0x29, d29);
    dev->interface->write_register(0x2a, d2a);
    dev->interface->write_register(0x09, 0x00);
}

/*  AGOHOME reuses whatever slope is in DRAM. 600/1200 overwrite the
    five slots with the unscaled table of the same length. 2400 already
    has the one-word home table in slots 0–2; do not replace it with
    the 24-word scan ramp. Do not change 0xA1, DPIHW, 0x02 or 0x0F —
    rewriting the motor clock at the far stop stalls the carriage. */
void send_hp_return_slope_tables(Genesys_Device* dev)
{
    if (dev == nullptr || is_testing_mode()) {
        return;
    }
    const unsigned dpi = g_hp_last_slope_dpi;
    if (dpi < 600 || dpi == 2400) {
        return;
    }

    std::vector<std::uint8_t> table;
    if (dpi == 600) {
        table.assign(hp_scanjet_200_slope.begin(), hp_scanjet_200_slope.end());
    } else {
        table.assign(hp_slope_1200_slow, hp_slope_1200_slow + 48);
    }
    DBG(DBG_info, "send_hp_return_slope_tables: unscaled %u dpi len=%u\n",
        dpi, static_cast<unsigned>(table.size()));
    for (unsigned n = 0; n < 5; ++n) {
        const std::uint32_t addr = 0x10000000u + 0x4000u * n;
        try {
            std::vector<std::uint8_t> slot(table.begin(), table.end());
            dev->interface->write_ahb(addr, static_cast<unsigned>(slot.size()), slot.data());
        } catch (...) {
            DBG(DBG_warn, "send_hp_return_slope_tables: AHB write failed at slot %u\n", n);
            return;
        }
    }
}

/*  Overlay 0xa5/0xab/0xad is FASTNO (short table). 0xae/0xaf is the
    scan-table length. Copy the scan length onto FASTNO so a pass that
    never leaves slot 0–2 still ramps to the scan-table cruise. */
void hp_match_fast_stepno_to_scan(Genesys_Register_Set* regs)
{
    if (regs == nullptr || !regs->has_reg(0xaf)) {
        return;
    }
    const std::uint8_t scan_steps = regs->get8(0xaf);
    if (regs->has_reg(0xa5)) {
        regs->set8(0xa5, scan_steps);
    }
    if (regs->has_reg(0xab)) {
        regs->set8(0xab, scan_steps);
    }
    if (regs->has_reg(0xad)) {
        regs->set8(0xad, scan_steps);
    }
}

void hp_scale_slope_le16(std::vector<std::uint8_t>& bytes, unsigned num, unsigned den)
{
    if (num == 0 || den == 0) {
        return;
    }
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        unsigned w = static_cast<unsigned>(bytes[i]) |
                     (static_cast<unsigned>(bytes[i + 1]) << 8);
        w = std::min(65535u, (w * num) / den);
        bytes[i] = static_cast<std::uint8_t>(w & 0xff);
        bytes[i + 1] = static_cast<std::uint8_t>((w >> 8) & 0xff);
    }
}

/*  Two-step 0x02 as in the vendor capture immediately before 0x0F=0xFF:
    first without AGOHOME, then the overlay (0x30 @300, 0x38 @600+).
    Do not raise SCAN or zero FEEDL here — that starts the pass early. */
void hp_arm_motor(Genesys_Device* dev, Genesys_Register_Set* reg)
{
    const std::uint8_t overlay_02 = (reg != nullptr && reg->has_reg(REG_0x02))
                                        ? reg->get8(REG_0x02) : 0x30;
    const std::uint8_t overlay_a0 = (reg != nullptr && reg->has_reg(0xa0))
                                        ? reg->get8(0xa0) : 0x00;
    const std::uint8_t overlay_a1 = (reg != nullptr && reg->has_reg(0xa1))
                                        ? reg->get8(0xa1) : 0x10;
    const std::uint8_t fed = static_cast<std::uint8_t>(overlay_02 & ~REG_0x02_AGOHOME);
    dev->interface->write_register(REG_0x02, fed);
    dev->interface->write_register(REG_0x02, overlay_02);
    dev->interface->write_register(0xa0, overlay_a0);
    dev->interface->write_register(0xa1, overlay_a1);
}

/*  Populate dev->reg with the HP baseline block so subsequent
    write_registers(dev->reg) uploads it in one shot. */
void install_hp_baseline_registers(Genesys_Device* dev)
{
    for (const auto& pair : hp_scanjet_200_baseline) {
        // High-bank registers (>= 0x100) do not live in Genesys_Register_Set;
        // write those directly. Everything below 0x100 goes through the
        // regular register set so it can be replayed later.
        if (pair.addr < 0x100) {
            dev->reg.init_reg(static_cast<std::uint16_t>(pair.addr), pair.value);
        }
    }
}

} // namespace

void CommandSetGl848::asic_boot(Genesys_Device* dev, bool cold) const
{
    DBG_HELPER_ARGS(dbg, "cold=%d", cold ? 1 : 0);

    // The HP driver deliberately does not toggle 0x0E (ASIC reset).
    (void) cold;

    install_hp_baseline_registers(dev);
    dev->interface->write_registers(dev->reg);

    // Enable DRAM by re-writing 0x0B with ENBDRAM set; then re-write 0x08
    // to nudge the arbiter (mirrors what gl847_init_registers() does).
    std::uint8_t val_0b = dev->reg.find_reg(0x0b).value | REG_0x0B_ENBDRAM;
    dev->interface->write_register(REG_0x0B, val_0b);
    dev->reg.find_reg(0x0b).value = val_0b;
    dev->interface->write_register(0x08, dev->reg.find_reg(0x08).value);

    // Buffer-end access, taken verbatim from the Windows log.
    dev->interface->write_0x8c(0x10, 0x0b);
    dev->interface->write_0x8c(0x13, 0x0b);

    send_hp_gamma_ramp(dev);
    send_hp_slope_tables(dev, 300);
}

// ----------------------------------------------------------------------------
// Register set for a scan pass. This is where per-mode differences would
// live; for now we clone the sensor's captured 300-dpi baseline and patch
// line count / feed length from the requested ScanSession.
// ----------------------------------------------------------------------------

void CommandSetGl848::init_regs_for_scan_session(Genesys_Device* dev,
                                                 const Genesys_Sensor& sensor,
                                                 Genesys_Register_Set* regs,
                                                 const ScanSession& session) const
{
    DBG_HELPER(dbg);
    session.assert_computed();
    (void) sensor;

    // The idle block from asic_boot() is not enough to start a pass: the
    // Windows driver rewrites ~220 registers with per-mode values (GPIO
    // 0xA7/0xA9, sensor segment tables 0x7E..0x92, exposure 0xE0..0xF7)
    // immediately before 0x0F=0xFF. Those snapshots are in
    // gl848_scan_tables.h, generated from the hp_{300,600,1200,2400}
    // captures. Overlay them on top of the idle file, then patch only
    // LINCNT and FEEDL from the requested geometry. Optical registers
    // (STRPIXEL/ENDPIXEL/LPERIOD/MAXWD) stay at the captured values —
    // SANE's computed ones do not clock this CIS.
    *regs = dev->initial_regs;
    for (std::uint16_t addr : { std::uint16_t{0x31}, std::uint16_t{0x34}, std::uint16_t{0x39} }) {
        if (!regs->has_reg(addr)) {
            regs->init_reg(addr, 0x00);
        }
    }

    const HpPassGeometry geom = hp_compute_pass(*dev, session);
    HpShading shading;
    try {
        shading = hp_ensure_shading(dev, geom.mode);
    } catch (const std::exception& e) {
        DBG(DBG_warn, "%s: shading skipped: %s\n", __func__, e.what());
    }

    apply_scan_regs_for_mode(regs, geom.mode.dpi);

    // LINCNT is CIS lines (3 per RGB row). FEEDL is only the park-to-page
    // offset; Y crop is a software skip.
    regs->set24(REG_LINCNT, geom.cis_lines);
    regs->set24(REG_FEEDL, geom.feed_lines);
    send_hp_slope_tables(dev, geom.mode.dpi);

    setup_hp_scanjet_pipeline(*dev, session, geom, shading.valid() ? &shading : nullptr);

    dev->read_active = true;
    dev->session = session;

    dev->total_bytes_read = 0;
    dev->total_bytes_to_read = static_cast<std::size_t>(session.output_line_bytes_requested)
                             * static_cast<std::size_t>(session.params.lines);

    DBG(DBG_info, "%s: hw pass %u dpi, scale %u×%u, crop %u+%u, skip_y=%u, "
                  "lincnt=%u, feedl=%u, total bytes to send=%zu\n",
        __func__, geom.mode.dpi, geom.scale_x, geom.scale_y, geom.optical_x0,
        geom.optical_xw, geom.skip_y, geom.cis_lines, geom.feed_lines,
        dev->total_bytes_to_read);
}

// ----------------------------------------------------------------------------
// Warm-up / shading register setup: minimal wrappers that reuse
// init_regs_for_scan_session with the calibration-oriented flags. The
// device does host-side shading, so calibration on the ASIC is
// unnecessary — but SANE still calls into these entry points, and they
// must not throw.
// ----------------------------------------------------------------------------

void CommandSetGl848::init_regs_for_warmup(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                           Genesys_Register_Set* regs) const
{
    DBG_HELPER(dbg);

    // A warm-up pass captures a few lines at optical resolution and throws
    // them away; we mimic the behaviour of gl847/gl846 with a 1-inch tall
    // sample and no motor move.
    ScanSession session;
    session.params.xres = sensor.full_resolution;
    session.params.yres = sensor.full_resolution;
    session.params.startx = 0;
    session.params.starty = 0;
    session.params.pixels = dev->model->x_size_calib_mm * sensor.full_resolution / MM_PER_INCH;
    session.params.lines = sensor.full_resolution; // one inch worth
    session.params.depth = 16;
    session.params.channels = 3;
    session.params.scan_method = dev->settings.scan_method;
    session.params.scan_mode = ScanColorMode::COLOR_SINGLE_PASS;
    session.params.color_filter = dev->settings.color_filter;
    session.params.contrast_adjustment = dev->settings.contrast;
    session.params.brightness_adjustment = dev->settings.brightness;
    session.params.exposure_lperiod = dev->settings.exposure_lperiod != 0
                                          ? dev->settings.exposure_lperiod
                                          : static_cast<unsigned>(sensor.exposure_lperiod);
    session.params.flags = ScanFlag::DISABLE_SHADING |
                           ScanFlag::DISABLE_GAMMA |
                           ScanFlag::SINGLE_LINE;
    compute_session(dev, session, sensor);

    init_regs_for_scan_session(dev, sensor, regs, session);
}

void CommandSetGl848::init_regs_for_shading(Genesys_Device* dev, const Genesys_Sensor& sensor,
                                            Genesys_Register_Set& regs) const
{
    DBG_HELPER(dbg);

    // Same idea as warm-up: a short shading pass at the sensor's optical
    // resolution. The captured frame is post-processed on the host, not
    // uploaded back to the ASIC, so this is really only here so that
    // sanei_genesys_asic_init()'s follow-up code sees a valid reg set.
    unsigned resolution = sensor.shading_resolution
                              ? sensor.shading_resolution
                              : sensor.full_resolution;

    const auto& calib_sensor =
        sanei_genesys_find_sensor(dev, resolution, 3, dev->settings.scan_method);

    ScanSession session;
    session.params.xres = resolution;
    session.params.yres = resolution;
    session.params.startx = 0;
    session.params.starty = 0;
    session.params.pixels = dev->model->x_size_calib_mm * resolution / MM_PER_INCH;
    session.params.lines = static_cast<unsigned>(
        dev->model->y_size_calib_mm * resolution / MM_PER_INCH);
    session.params.depth = 16;
    session.params.channels = 3;
    session.params.scan_method = dev->settings.scan_method;
    session.params.scan_mode = ScanColorMode::COLOR_SINGLE_PASS;
    session.params.color_filter = dev->settings.color_filter;
    session.params.contrast_adjustment = dev->settings.contrast;
    session.params.brightness_adjustment = dev->settings.brightness;
    session.params.exposure_lperiod = dev->settings.exposure_lperiod != 0
                                          ? dev->settings.exposure_lperiod
                                          : static_cast<unsigned>(calib_sensor.exposure_lperiod);
    session.params.flags = ScanFlag::DISABLE_SHADING |
                           ScanFlag::DISABLE_GAMMA |
                           ScanFlag::DISABLE_BUFFER_FULL_MOVE;
    compute_session(dev, session, calib_sensor);

    init_regs_for_scan_session(dev, calib_sensor, &regs, session);
    dev->set_head_pos_zero(ScanHeadId::PRIMARY);
    dev->calib_session = session;
}

// ----------------------------------------------------------------------------
// begin_scan / end_scan / move_back_home. USB sequences from the vendor
// driver traces:
//
//   start:  clear LNCNT/MCNT, set SCAN in 0x01, then write 0x0F = 0xFF.
//   stop:   clear SCAN in 0x01, then park the carriage (0x0A=0x40; 0x01=0xC0
//           twice), polling 0x101 for (home && !motor).
//   home:   same as stop; the pass returns on its own after 0x01=0xC0.
// ----------------------------------------------------------------------------

void CommandSetGl848::begin_scan(Genesys_Device* dev, const Genesys_Sensor& /*sensor*/,
                                 Genesys_Register_Set* reg, bool start_motor) const
{
    DBG_HELPER_ARGS(dbg, "start_motor=%d", start_motor ? 1 : 0);

    // write_registers() has just pushed the per-mode scan snapshot
    // (lamp GPIO, sensor tables, 0x03=0x9f). Do not reload slope/gamma
    // here: send_hp_slope_tables() fences each slot with 0x3C=0x0A/0xAA
    // and would smash the scan-time 0x3C=0xEF, which is what actually
    // clocks the CIS LED. Tables are already in DRAM from asic_boot().

    g_hp_homing = false;

    if (start_motor) {
        hp_arm_motor(dev, reg);
        if (!is_testing_mode()) {
            const std::uint8_t r01 = dev->interface->read_register(REG_0x01);
            const std::uint8_t r02 = dev->interface->read_register(REG_0x02);
            const std::uint8_t ra0 = dev->interface->read_register(0xa0);
            const std::uint8_t ra1 = dev->interface->read_register(0xa1);
            const std::uint8_t ra5 = dev->interface->read_register(0xa5);
            const std::uint8_t raf = dev->interface->read_register(0xaf);
            DBG(DBG_warn, "begin_scan: 0x01=0x%02x 0x02=0x%02x "
                "0xa0=0x%02x 0xa1=0x%02x 0xa5=0x%02x 0xaf=0x%02x\n",
                r01, r02, ra0, ra1, ra5, raf);
        }
    }

    dev->interface->write_register(REG_0x0D, REG_0x0D_CLRLNCNT);
    dev->interface->write_register(REG_0x0D, REG_0x0D_CLRMCNT);

    std::uint8_t val = dev->interface->read_register(REG_0x01);
    val |= REG_0x01_SCAN;
    dev->interface->write_register(REG_0x01, val);
    if (reg->has_reg(REG_0x01)) {
        reg->set8(REG_0x01, val);
    }

    scanner_start_action(*dev, start_motor);

    if (start_motor && !is_testing_mode()) {
        if (!wait_for_cis_data(dev, 15000)) {
            throw SaneException(SANE_STATUS_IO_ERROR,
                                "GL848+: CIS did not start (0x100 never reached data-ready)");
        }
    }

    dev->advance_head_pos_by_session(ScanHeadId::PRIMARY);
}

namespace {

void hp_write_vendor_park(Genesys_Device* dev)
{
    for (int repeat = 0; repeat < 2; ++repeat) {
        try { dev->interface->write_register(0x0a, 0x40); } catch (...) {}
        try { dev->interface->write_register(REG_0x01, 0xc0); } catch (...) {}
    }
}

bool hp_kick_park(Genesys_Device* dev)
{
    if (dev == nullptr || is_testing_mode() || g_hp_homing) {
        return false;
    }
    g_hp_homing = true;
    hp_write_vendor_park(dev);
    return true;
}

/*  Park. On the Scanjet 200 the pass itself returns the carriage to
    home; there is no separate "go home" command. Set 0x0A back to its
    idle value, clear the SCAN bit in 0x01, and wait for REG_STATUS to
    report both "home sensor set" and "motor stopped". The vendor driver
    writes the two control registers twice in a row (some ASIC revisions
    apparently latch the first write with the motor still ramping down);
    we do the same.

    Do not write 0x0F=0x00 and do not start a new pass from the field:
    that aborts CIS clocking and leaves the motor humming in place. */
bool hp_park_and_wait(Genesys_Device* dev, unsigned timeout_ms)
{
    hp_kick_park(dev);
    send_hp_return_slope_tables(dev);
    hp_write_vendor_park(dev);

    const unsigned step = 20;
    unsigned elapsed = 0;
    while (elapsed < timeout_ms) {
        try {
            auto status = scanner_read_status(*dev);
            if (status.is_at_home && !status.is_motor_enabled) {
                return true;
            }
        } catch (...) {
            // ignore transient read errors and keep polling
        }
        dev->interface->sleep_ms(step);
        elapsed += step;
    }
    return false;
}

/*  Carriage already stopped in the field (previous run died). A new
    0x0F is safe only when the motor bit is clear. Do not abort a live
    pass from here. FEEDL=0 so this cannot walk into the far stop. */
bool hp_dummy_home_pass(Genesys_Device* dev)
{
    DBG(DBG_info, "GL848+: dummy 300 dpi home (LINCNT=6, FEEDL=0)\n");

    try {
        auto status = scanner_read_status(*dev);
        if (status.is_motor_enabled) {
            DBG(DBG_warn, "GL848+: dummy skipped, motor still running\n");
            hp_write_vendor_park(dev);
            return false;
        }
    } catch (...) {
    }

    Genesys_Register_Set regs = dev->initial_regs;
    for (std::uint16_t addr : { std::uint16_t{0x31}, std::uint16_t{0x34}, std::uint16_t{0x39} }) {
        if (!regs.has_reg(addr)) {
            regs.init_reg(addr, 0x00);
        }
    }
    apply_scan_regs_for_mode(&regs, 300);
    regs.set24(REG_LINCNT, 6);
    regs.set24(REG_FEEDL, 0);
    send_hp_slope_tables(dev, 300);
    if (regs.has_reg(REG_0x01)) {
        regs.set8(REG_0x01, static_cast<std::uint8_t>(regs.get8(REG_0x01) & ~REG_0x01_SCAN));
    }

    dev->interface->write_registers(regs);
    hp_arm_motor(dev, &regs);
    dev->interface->write_register(REG_0x0D, REG_0x0D_CLRLNCNT);
    dev->interface->write_register(REG_0x0D, REG_0x0D_CLRMCNT);

    const std::uint8_t scan_01 = static_cast<std::uint8_t>(
        REG_0x01_CISSET | REG_0x01_DOGENB | REG_0x01_SCAN);
    dev->interface->write_register(REG_0x01, scan_01);
    scanner_start_action(*dev, true);
    if (!wait_for_cis_data(dev, 3000)) {
        DBG(DBG_warn, "GL848+: dummy CIS did not start, cutting motor\n");
        try { dev->interface->write_register(REG_0x02, 0x00); } catch (...) {}
        hp_write_vendor_park(dev);
        return false;
    }
    unsigned scale = 1;
    const unsigned samples = hp_choose_mode(300, scale).samples_per_line;
    const std::size_t row = static_cast<std::size_t>(samples) * 2;
    std::vector<std::uint8_t> buf(row);
    for (int i = 0; i < 6; ++i) {
        try {
            dev->interface->bulk_read_data(0x45, buf.data(), row);
        } catch (...) {
            break;
        }
    }
    hp_write_vendor_park(dev);
    return true;
}

} // namespace

void CommandSetGl848::end_scan(Genesys_Device* dev, Genesys_Register_Set* /*reg*/,
                               bool check_stop) const
{
    DBG_HELPER_ARGS(dbg, "check_stop=%d", check_stop ? 1 : 0);

    if (dev->model->is_sheetfed) {
        return;
    }

    // Best-effort park: genesys_read_ordered_data calls us after the
    // frontend has drained the last byte. Swallow a timeout rather than
    // propagate SANE_STATUS_IO_ERROR (that would discard the scan).
    hp_park_and_wait(dev, 60000);
}

void CommandSetGl848::move_back_home(Genesys_Device* dev, bool wait_until_home) const
{
    DBG_HELPER_ARGS(dbg, "wait_until_home=%d", wait_until_home ? 1 : 0);

    try {
        auto status = scanner_read_status(*dev);
        if (status.is_at_home && !status.is_motor_enabled) {
            dev->set_head_pos_zero(ScanHeadId::PRIMARY);
            return;
        }
        if (status.is_motor_enabled) {
            // A pass is already returning; just wait for the sensor.
            hp_park_and_wait(dev, wait_until_home ? 60000 : 500);
            dev->set_head_pos_zero(ScanHeadId::PRIMARY);
            return;
        }
    } catch (...) {
        // fall through to dummy pass
    }

    if (wait_until_home) {
        hp_dummy_home_pass(dev);
        hp_park_and_wait(dev, 60000);
    } else {
        hp_park_and_wait(dev, 500);
    }
    dev->set_head_pos_zero(ScanHeadId::PRIMARY);
}

} // namespace gl848
} // namespace genesys
