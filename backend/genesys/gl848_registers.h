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
   Register map for the Genesys GL848+ ASIC.

   The GL848+ is closely related to GL847: the low bank (0x00..0xff) shares
   layout and semantics; scan status and the valid-word counter live in the
   high bank at 0x100..0x103 (accessed with USB wValue bit 0x100 set).

   Values here are copies of the GL847 definitions, kept in a separate
   namespace so that GL848+-specific bits do not accidentally land in the
   GL847 header. High-bank status, AHB bases and the HP Scanjet 200
   (USB 0x03f0:0x1c05) quirks come from USB traces of the vendor Windows
   driver (independent implementation).
*/

#ifndef BACKEND_GENESYS_GL848_REGISTERS_H
#define BACKEND_GENESYS_GL848_REGISTERS_H

#include <cstdint>

namespace genesys {
namespace gl848 {

using RegAddr = std::uint16_t;
using RegMask = std::uint8_t;
using RegShift = unsigned;

// --- Low-bank control registers (0x00..0x0f) ---
static constexpr RegAddr REG_0x01 = 0x01;
static constexpr RegMask REG_0x01_CISSET = 0x80;
static constexpr RegMask REG_0x01_DOGENB = 0x40;
static constexpr RegMask REG_0x01_DVDSET = 0x20;
static constexpr RegMask REG_0x01_STAGGER = 0x10;
static constexpr RegMask REG_0x01_COMPENB = 0x08;
static constexpr RegMask REG_0x01_TRUEGRAY = 0x04;
static constexpr RegMask REG_0x01_SHDAREA = 0x02;
static constexpr RegMask REG_0x01_SCAN = 0x01;

static constexpr RegAddr REG_0x02 = 0x02;
static constexpr RegMask REG_0x02_NOTHOME = 0x80;
static constexpr RegMask REG_0x02_ACDCDIS = 0x40;
static constexpr RegMask REG_0x02_AGOHOME = 0x20;
static constexpr RegMask REG_0x02_MTRPWR = 0x10;
static constexpr RegMask REG_0x02_FASTFED = 0x08;
static constexpr RegMask REG_0x02_MTRREV = 0x04;
static constexpr RegMask REG_0x02_HOMENEG = 0x02;
static constexpr RegMask REG_0x02_LONGCURV = 0x01;

static constexpr RegAddr REG_0x03 = 0x03;
static constexpr RegMask REG_0x03_LAMPDOG = 0x80;
static constexpr RegMask REG_0x03_AVEENB = 0x40;
static constexpr RegMask REG_0x03_XPASEL = 0x20;
static constexpr RegMask REG_0x03_LAMPPWR = 0x10;
static constexpr RegMask REG_0x03_LAMPTIM = 0x0f;

static constexpr RegAddr REG_0x04 = 0x04;
static constexpr RegMask REG_0x04_LINEART = 0x80;
static constexpr RegMask REG_0x04_BITSET = 0x40;
static constexpr RegMask REG_0x04_AFEMOD = 0x30;
static constexpr RegMask REG_0x04_FILTER = 0x0c;
static constexpr RegMask REG_0x04_FESET = 0x03;
static constexpr RegShift REG_0x04S_AFEMOD = 4;

static constexpr RegAddr REG_0x05 = 0x05;
static constexpr RegMask REG_0x05_DPIHW = 0xc0;
static constexpr RegMask REG_0x05_DPIHW_600 = 0x00;
static constexpr RegMask REG_0x05_DPIHW_1200 = 0x40;
static constexpr RegMask REG_0x05_DPIHW_2400 = 0x80;
static constexpr RegMask REG_0x05_DPIHW_4800 = 0xc0;
static constexpr RegMask REG_0x05_MTLLAMP = 0x30;
static constexpr RegMask REG_0x05_GMMENB = 0x08;
static constexpr RegMask REG_0x05_MTLBASE = 0x03;

static constexpr RegAddr REG_0x06 = 0x06;
static constexpr RegMask REG_0x06_SCANMOD = 0xe0;
static constexpr RegShift REG_0x06S_SCANMOD = 5;
static constexpr RegMask REG_0x06_PWRBIT = 0x10;
static constexpr RegMask REG_0x06_GAIN4 = 0x08;
static constexpr RegMask REG_0x06_OPTEST = 0x07;

static constexpr RegMask REG_0x07_LAMPSIM = 0x80;

static constexpr RegMask REG_0x08_DRAM2X = 0x80;
static constexpr RegMask REG_0x08_MPENB = 0x20;
static constexpr RegMask REG_0x08_CIS_LINE = 0x10;
static constexpr RegMask REG_0x08_IR1ENB = 0x08;
static constexpr RegMask REG_0x08_IR2ENB = 0x04;
static constexpr RegMask REG_0x08_ENB24M = 0x01;

static constexpr RegMask REG_0x09_MCNTSET = 0xc0;
static constexpr RegMask REG_0x09_EVEN1ST = 0x20;
static constexpr RegMask REG_0x09_BLINE1ST = 0x10;
static constexpr RegMask REG_0x09_BACKSCAN = 0x08;
static constexpr RegMask REG_0x09_ENHANCE = 0x04;
static constexpr RegMask REG_0x09_SHORTTG = 0x02;
static constexpr RegMask REG_0x09_NWAIT = 0x01;
static constexpr RegShift REG_0x09S_MCNTSET = 6;
static constexpr RegShift REG_0x09S_CLKSET = 4;

static constexpr RegMask REG_0x0A_LPWMEN = 0x10;

static constexpr RegAddr REG_0x0B = 0x0b;
static constexpr RegMask REG_0x0B_DRAMSEL = 0x07;
static constexpr RegMask REG_0x0B_ENBDRAM = 0x08;
static constexpr RegMask REG_0x0B_RFHDIS = 0x10;
static constexpr RegMask REG_0x0B_CLKSET = 0xe0;
static constexpr RegMask REG_0x0B_24MHZ = 0x00;
static constexpr RegMask REG_0x0B_30MHZ = 0x20;
static constexpr RegMask REG_0x0B_40MHZ = 0x40;
static constexpr RegMask REG_0x0B_48MHZ = 0x60;
static constexpr RegMask REG_0x0B_60MHZ = 0x80;

static constexpr RegAddr REG_0x0C = 0x0c;
static constexpr RegMask REG_0x0C_CCDLMT = 0x0f;

static constexpr RegAddr REG_0x0D = 0x0d;
static constexpr RegMask REG_0x0D_FULLSTP = 0x10;
static constexpr RegMask REG_0x0D_SEND = 0x80;
static constexpr RegMask REG_0x0D_CLRMCNT = 0x04;
static constexpr RegMask REG_0x0D_CLRDOCJM = 0x02;
static constexpr RegMask REG_0x0D_CLRLNCNT = 0x01;

// REG_0x0F=0xFF starts a capture; the low bit is used to clear the home flag while idle.
static constexpr RegAddr REG_0x0F = 0x0f;

// --- Sensor timing group (0x16..0x1f) ---
static constexpr RegMask REG_0x16_CTRLHI = 0x80;
static constexpr RegMask REG_0x16_TOSHIBA = 0x40;
static constexpr RegMask REG_0x16_TGINV = 0x20;
static constexpr RegMask REG_0x16_CK1INV = 0x10;
static constexpr RegMask REG_0x16_CK2INV = 0x08;
static constexpr RegMask REG_0x16_CTRLINV = 0x04;
static constexpr RegMask REG_0x16_CKDIS = 0x02;
static constexpr RegMask REG_0x16_CTRLDIS = 0x01;

static constexpr RegMask REG_0x17_TGMODE = 0xc0;
static constexpr RegMask REG_0x17_TGMODE_NO_DUMMY = 0x00;
static constexpr RegMask REG_0x17_TGMODE_REF = 0x40;
static constexpr RegMask REG_0x17_TGMODE_XPA = 0x80;
static constexpr RegMask REG_0x17_TGW = 0x3f;
static constexpr RegShift REG_0x17S_TGW = 0;

static constexpr RegAddr REG_0x18 = 0x18;
static constexpr RegMask REG_0x18_CNSET = 0x80;
static constexpr RegMask REG_0x18_DCKSEL = 0x60;
static constexpr RegMask REG_0x18_CKTOGGLE = 0x10;
static constexpr RegMask REG_0x18_CKDELAY = 0x0c;
static constexpr RegMask REG_0x18_CKSEL = 0x03;

static constexpr RegMask REG_0x1A_SW2SET = 0x80;
static constexpr RegMask REG_0x1A_SW1SET = 0x40;
static constexpr RegMask REG_0x1A_MANUAL3 = 0x02;
static constexpr RegMask REG_0x1A_MANUAL1 = 0x01;
static constexpr RegMask REG_0x1A_CK4INV = 0x08;
static constexpr RegMask REG_0x1A_CK3INV = 0x04;
static constexpr RegMask REG_0x1A_LINECLP = 0x02;

static constexpr RegAddr REG_0x1C = 0x1c;
static constexpr RegMask REG_0x1C_TGTIME = 0x07;

static constexpr RegMask REG_0x1D_CK4LOW = 0x80;
static constexpr RegMask REG_0x1D_CK3LOW = 0x40;
static constexpr RegMask REG_0x1D_CK1LOW = 0x20;
static constexpr RegMask REG_0x1D_TGSHLD = 0x1f;
static constexpr RegShift REG_0x1DS_TGSHLD = 0;

static constexpr RegAddr REG_0x1E = 0x1e;
static constexpr RegMask REG_0x1E_WDTIME = 0xf0;
static constexpr RegShift REG_0x1ES_WDTIME = 4;
static constexpr RegMask REG_0x1E_LINESEL = 0x0f;
static constexpr RegShift REG_0x1ES_LINESEL = 0;

static constexpr RegAddr REG_FEDCNT = 0x1f;

// --- Frame counters / status (low bank) ---
// In the low bank REG_0x40 is only used for cold-boot / motor-flag probes;
// live scan status for GL848+ is at REG_STATUS (0x101), not 0x41.
static constexpr RegAddr REG_0x40 = 0x40;
static constexpr RegMask REG_0x40_CHKVER = 0x10;
static constexpr RegMask REG_0x40_HISPDFLG = 0x04;
static constexpr RegMask REG_0x40_MOTMFLG = 0x02;
static constexpr RegMask REG_0x40_DATAENB = 0x01;

static constexpr RegMask REG_0x41_PWRBIT = 0x80;
static constexpr RegMask REG_0x41_BUFEMPTY = 0x40;
static constexpr RegMask REG_0x41_FEEDFSH = 0x20;
static constexpr RegMask REG_0x41_SCANFSH = 0x10;
static constexpr RegMask REG_0x41_HOMESNR = 0x08;
static constexpr RegMask REG_0x41_LAMPSTS = 0x04;
static constexpr RegMask REG_0x41_FEBUSY = 0x02;
static constexpr RegMask REG_0x41_MOTORENB = 0x01;

// --- CIS analog front-end (0x58..0x5e) ---
static constexpr RegMask REG_0x58_VSMP = 0xf8;
static constexpr RegShift REG_0x58S_VSMP = 3;
static constexpr RegMask REG_0x58_VSMPW = 0x07;
static constexpr RegShift REG_0x58S_VSMPW = 0;

static constexpr RegMask REG_0x59_BSMP = 0xf8;
static constexpr RegShift REG_0x59S_BSMP = 3;
static constexpr RegMask REG_0x59_BSMPW = 0x07;
static constexpr RegShift REG_0x59S_BSMPW = 0;

static constexpr RegMask REG_0x5A_ADCLKINV = 0x80;
static constexpr RegMask REG_0x5A_RLCSEL = 0x40;
static constexpr RegMask REG_0x5A_CDSREF = 0x30;
static constexpr RegShift REG_0x5AS_CDSREF = 4;
static constexpr RegMask REG_0x5A_RLC = 0x0f;
static constexpr RegShift REG_0x5AS_RLC = 0;

static constexpr RegMask REG_0x5E_DECSEL = 0xe0;
static constexpr RegShift REG_0x5ES_DECSEL = 5;
static constexpr RegMask REG_0x5E_STOPTIM = 0x1f;
static constexpr RegShift REG_0x5ES_STOPTIM = 0;

// --- Motor Z / step tables (0x60..0x6a) ---
static constexpr RegAddr REG_0x60 = 0x60;
static constexpr RegMask REG_0x60_Z1MOD = 0x1f;
static constexpr RegAddr REG_0x61 = 0x61;
static constexpr RegMask REG_0x61_Z1MOD = 0xff;
static constexpr RegAddr REG_0x62 = 0x62;
static constexpr RegMask REG_0x62_Z1MOD = 0xff;

static constexpr RegAddr REG_0x63 = 0x63;
static constexpr RegMask REG_0x63_Z2MOD = 0x1f;
static constexpr RegAddr REG_0x64 = 0x64;
static constexpr RegMask REG_0x64_Z2MOD = 0xff;
static constexpr RegAddr REG_0x65 = 0x65;
static constexpr RegMask REG_0x65_Z2MOD = 0xff;

static constexpr RegShift REG_0x60S_STEPSEL = 5;
static constexpr RegMask REG_0x60_STEPSEL = 0xe0;
static constexpr RegMask REG_0x60_FULLSTEP = 0x00;
static constexpr RegMask REG_0x60_HALFSTEP = 0x20;
static constexpr RegMask REG_0x60_EIGHTHSTEP = 0x60;
static constexpr RegMask REG_0x60_16THSTEP = 0x80;

static constexpr RegShift REG_0x63S_FSTPSEL = 5;
static constexpr RegMask REG_0x63_FSTPSEL = 0xe0;
static constexpr RegMask REG_0x63_FULLSTEP = 0x00;
static constexpr RegMask REG_0x63_HALFSTEP = 0x20;
static constexpr RegMask REG_0x63_EIGHTHSTEP = 0x60;
static constexpr RegMask REG_0x63_16THSTEP = 0x80;

static constexpr RegAddr REG_0x67 = 0x67;
static constexpr RegMask REG_0x67_MTRPWM = 0x80;

static constexpr RegAddr REG_0x68 = 0x68;
static constexpr RegMask REG_0x68_FASTPWM = 0x80;

// --- GPIO group ---
// NOTE for GL848+: the Windows driver *does not* touch 0x6B..0x6F on the HP Scanjet 200.
// Value/enable pairs live at 0x6c/0x6d and 0x6e/0x6f as on GL847.
static constexpr RegAddr REG_0x6B = 0x6b;
static constexpr RegMask REG_0x6B_MULTFILM = 0x80;
static constexpr RegMask REG_0x6B_GPOM13 = 0x40;
static constexpr RegMask REG_0x6B_GPOM12 = 0x20;
static constexpr RegMask REG_0x6B_GPOM11 = 0x10;
static constexpr RegMask REG_0x6B_GPO18 = 0x02;
static constexpr RegMask REG_0x6B_GPO17 = 0x01;

static constexpr RegShift REG_0x6C = 0x6c;
static constexpr RegMask REG_0x6C_GPIO16 = 0x80;
static constexpr RegMask REG_0x6C_GPIO15 = 0x40;
static constexpr RegMask REG_0x6C_GPIO14 = 0x20;
static constexpr RegMask REG_0x6C_GPIO13 = 0x10;
static constexpr RegMask REG_0x6C_GPIO12 = 0x08;
static constexpr RegMask REG_0x6C_GPIO11 = 0x04;
static constexpr RegMask REG_0x6C_GPIO10 = 0x02;
static constexpr RegMask REG_0x6C_GPIO9 = 0x01;
static constexpr RegMask REG_0x6C_GPIOH = 0xff;
static constexpr RegMask REG_0x6C_GPIOL = 0xff;

static constexpr RegAddr REG_0x6D = 0x6d;
static constexpr RegAddr REG_0x6E = 0x6e;
static constexpr RegAddr REG_0x6F = 0x6f;
static constexpr RegAddr REG_0x7E = 0x7e;

static constexpr RegMask REG_0x87_LEDADD = 0x04;

static constexpr RegAddr REG_0x9E = 0x9e;
static constexpr RegAddr REG_0x9F = 0x9f;

// A6..A9, AB — LED / lamp timing on the HP CIS.
static constexpr RegAddr REG_0xA6 = 0xa6;
static constexpr RegAddr REG_0xA7 = 0xa7;
static constexpr RegAddr REG_0xA8 = 0xa8;
static constexpr RegAddr REG_0xA9 = 0xa9;
static constexpr RegAddr REG_0xAB = 0xab;

// Semantic register aliases (same offsets as GL847).
static constexpr RegAddr REG_EXPR = 0x10;
static constexpr RegAddr REG_EXPG = 0x12;
static constexpr RegAddr REG_EXPB = 0x14;
static constexpr RegAddr REG_EXPDMY = 0x19;
static constexpr RegAddr REG_STEPNO = 0x21;
static constexpr RegAddr REG_FWDSTEP = 0x22;
static constexpr RegAddr REG_BWDSTEP = 0x23;
static constexpr RegAddr REG_FASTNO = 0x24;
static constexpr RegAddr REG_DPISET = 0x2c;
static constexpr RegAddr REG_STRPIXEL = 0x30;
static constexpr RegAddr REG_ENDPIXEL = 0x32;
// LINCNT counts single-colour CIS lines. Multiple of three; patched at scan start.
static constexpr RegAddr REG_LINCNT = 0x25;
static constexpr RegAddr REG_MAXWD = 0x35;
static constexpr RegAddr REG_LPERIOD = 0x38;
// FEEDL — park-to-frame steps. HP Scanjet 200 defaults per pass:
//   300 dpi   -> 543
//   600 dpi   -> 489
//   1200 dpi  -> 552
//   2400 dpi  -> 552
static constexpr RegAddr REG_FEEDL = 0x3d;
static constexpr RegAddr REG_FMOVDEC = 0x5f;
static constexpr RegAddr REG_FSHDEC = 0x69;
static constexpr RegAddr REG_FMOVNO = 0x6a;
static constexpr RegAddr REG_CK1MAP = 0x74;
static constexpr RegAddr REG_CK3MAP = 0x77;
static constexpr RegAddr REG_CK4MAP = 0x7a;

// --- High-bank live scan status (GL848+ specific) ---
// Reads use USB wValue = 0x18E (0x8E | 0x100).
// REG_STATUS transitions during a capture: 0xb2 (buf empty) -> 0x36 (ramp) -> 0x33 (data).
static constexpr RegAddr REG_STATUS = 0x101;
// Four-byte valid-word counter (as on GL124): 0x102..0x105.
static constexpr RegAddr REG_VALIDWORDS = 0x102;

// Motor / home sensor bits within REG_STATUS.
// Bit 0x08 — home sensor, bit 0x01 — motor running.
static constexpr RegMask REG_STATUS_HOMESNR = 0x08;
static constexpr RegMask REG_STATUS_MOTORRUN = 0x01;

// AHB base addresses used by the HP driver on GL848+.
// Slope tables: five identical 59-word tables at 0x10000000 + 0x4000*n.
static constexpr std::uint32_t AHB_SLOPE_BASE = 0x10000000u;
static constexpr std::uint32_t AHB_SLOPE_STRIDE = 0x00004000u;
// Gamma tables (three per-channel 256-entry 16-bit LE tables) at 0x01000000 + 0x200*ch.
static constexpr std::uint32_t AHB_GAMMA_BASE = 0x01000000u;
static constexpr std::uint32_t AHB_GAMMA_STRIDE = 0x00000200u;

// USB bulk chunk sizes observed in vendor-driver traces:
//   300 dpi  -> 196080 bytes  (38 lines * 2580 samples * 2 bytes)
//   600 dpi  -> 1042320 bytes
//   1200 dpi -> 1032000 bytes
//   2400 dpi -> 1032000 bytes
// A cross-check is that samples-per-line * 2 must divide the chunk size.

} // namespace gl848
} // namespace genesys

#endif // BACKEND_GENESYS_GL848_REGISTERS_H
