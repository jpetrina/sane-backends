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
 * GL128 register addresses and bit masks, from USB captures of the vendor
 * driver (as decoded by pyopticfilm's Gl128Registers). GL124-family, but not
 * identical to GL124: status is at 0x101, and the front end, exposure and
 * depth registers differ from gl124_registers.h.
 */

#ifndef BACKEND_GENESYS_GL128_REGISTERS_H
#define BACKEND_GENESYS_GL128_REGISTERS_H

#include <cstdint>

namespace genesys {
namespace gl128 {

using RegAddr = std::uint16_t;
using RegMask = std::uint8_t;

// -- 0x01 scan control --------------------------------------------------
static constexpr RegAddr REG_0x01 = 0x01;
static constexpr RegMask REG_0x01_DVDSET = 0x20;
static constexpr RegMask REG_0x01_STAGGER = 0x10;
static constexpr RegMask REG_0x01_SHDAREA = 0x02;
static constexpr RegMask REG_0x01_SCAN = 0x01;

// -- 0x02 motor -----------------------------------------------------------
static constexpr RegAddr REG_0x02 = 0x02;
static constexpr RegMask REG_0x02_AGOHOME = 0x20;
static constexpr RegMask REG_0x02_MTRPWR = 0x10;
static constexpr RegMask REG_0x02_FASTFED = 0x08;
static constexpr RegMask REG_0x02_MTRREV = 0x04;

// -- 0x03 lamp --------------------------------------------------------------
// XPASEL is held for every transparency operation; LAMPPWR gates the white
// lamp. IR passes clear LAMPPWR and keep XPASEL.
static constexpr RegAddr REG_0x03 = 0x03;
static constexpr RegMask REG_0x03_AVEENB = 0x40;
static constexpr RegMask REG_0x03_XPASEL = 0x20;
static constexpr RegMask REG_0x03_LAMPPWR = 0x10;

// -- counters / start -------------------------------------------------------
static constexpr RegAddr REG_CLRCNT = 0x0D;  // write CLRCNT_ALL to clear line/motor/feed counters
static constexpr RegMask CLRCNT_ALL = 0x07;
static constexpr RegAddr REG_START = 0x0F;   // write START_GO to launch the configured operation
static constexpr RegMask START_GO = 0x01;

// -- scan geometry / timing --------------------------------------------------
static constexpr RegAddr REG_LINCNT = 0x25;    // 24-bit BE, native (7200 dpi) lines
static constexpr RegAddr REG_LPERIOD = 0x28;   // 24-bit BE line exposure period
static constexpr RegAddr REG_DPISET = 0x2C;    // 16-bit BE, == effective_dpi / 6
static constexpr RegAddr REG_DEPTH_A = 0x33;   // 0x04 = 16-bit output, 0x1F = 8-bit
static constexpr RegMask DEPTH16_A = 0x04;
static constexpr RegMask DEPTH8_A = 0x1F;

// -- infrared -----------------------------------------------------------------
static constexpr RegAddr REG_IR = 0x37;        // bit 2 enables the IR LED; never read-modify-written
static constexpr RegMask IR_LED = 0x04;

static constexpr RegAddr REG_FEEDL = 0x3D;     // 24-bit BE feed distance, move-only ops

// -- analog frontend ------------------------------------------------------------
// Front end registers are written as index + value through 0x51/0x5D/0x5E
// (not 0x3a/0x3b as on GL845).
static constexpr RegAddr REG_FE_INDEX = 0x51;  // frontend register index
static constexpr RegAddr REG_FE_HIGH = 0x5D;   // frontend value, high byte
static constexpr RegAddr REG_FE_LOW = 0x5E;    // frontend value, low byte

static constexpr RegAddr REG_EXPOSURE = 0x7D;  // 24-bit BE base exposure (captures: 14000)

// -- image geometry --------------------------------------------------------------
static constexpr RegAddr REG_STRPIXEL = 0x82;  // 24-bit BE, native 7200 dpi units
static constexpr RegAddr REG_ENDPIXEL = 0x85;  // 24-bit BE, native 7200 dpi units

static constexpr RegAddr REG_DEPTH_B = 0xAF;   // 0x46 = 16-bit output, 0xFF = 8-bit
static constexpr RegMask DEPTH16_B = 0x46;
static constexpr RegMask DEPTH8_B = 0xFF;

// -- status -------------------------------------------------------------------------
// High-address read. Bit layout as GL845's 0x41 (GL124 uses 0x100).
static constexpr RegAddr REG_STATUS = 0x101;
static constexpr RegMask STATUS_PWRBIT = 0x80;
static constexpr RegMask STATUS_BUFEMPTY = 0x40;
static constexpr RegMask STATUS_FEEDFSH = 0x20;
static constexpr RegMask STATUS_SCANFSH = 0x10;
static constexpr RegMask STATUS_HOMESNR = 0x08;
static constexpr RegMask STATUS_LAMPSTS = 0x04;
static constexpr RegMask STATUS_FEBUSY = 0x02;
static constexpr RegMask STATUS_MOTORENB = 0x01;

// -- bulk transfer preambles -------------------------------------------------------
static constexpr std::uint8_t BULK_INDEX_RAM = 0x00;   // RAM / calibration reads
static constexpr std::uint8_t BULK_INDEX_IMAGE = 0x08; // image stream

// -- AHB windows -------------------------------------------------------------------
// Per-channel exposure, motor slopes, shading table upload targets.
static constexpr std::uint32_t AHB_CHANNEL_R = 0x10000000;
static constexpr std::uint32_t AHB_CHANNEL_G = 0x10004000;
static constexpr std::uint32_t AHB_CHANNEL_B = 0x10008000;
static constexpr std::uint32_t AHB_SLOPE_SCAN = 0x1000C000;
static constexpr std::uint32_t AHB_SLOPE_FAST = 0x10010000;
static constexpr std::uint32_t AHB_SHADING = 0x10014000;

// -- vendor probe (feed completion polling) ------------------------------------------
static constexpr std::uint8_t FEED_PROBE_INDEX = 0x21;
static constexpr std::uint8_t FEED_PROBE_DONE = 0x04;

} // namespace gl128
} // namespace genesys

#endif // BACKEND_GENESYS_GL128_REGISTERS_H
