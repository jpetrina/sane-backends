/* sane - Scanner Access Now Easy.

   BACKEND brother_mfp

   Copyright (C) 2022 Ralph Little <skelband@gmail.com>

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

   This file implements a SANE backend for Brother Multifunction Devices.
   */

#pragma once

#include<string>
#include "../include/sane/sane.h"

#define BACKEND_NAME            brother_mfp
#define BROTHER_MFP_CONFIG_FILE "brother_mfp.conf"

/*
 * Model capabilities.
 *
 * TODO: Some of these capabilities might be redundant if all models possess them.
 * E.g. MODE_GRAY
 *
 */
#define CAP_MODE_COLOUR                 (1u << 0)
#define CAP_MODE_GRAY                   (1u << 1)
#define CAP_MODE_GRAY_DITHER            (1u << 2)
#define CAP_MODE_BW                     (1u << 3)

#define CAP_BUTTON_HAS_SCAN_EMAIL       (1u << 4)
#define CAP_BUTTON_HAS_SCAN_OCR         (1u << 5)
#define CAP_BUTTON_HAS_SCAN_FILE        (1u << 6)
#define CAP_BUTTON_HAS_SCAN_IMAGE       (1u << 7)

#define CAP_SOURCE_HAS_FLATBED          (1u << 8)
#define CAP_SOURCE_HAS_ADF              (1u << 9)
#define CAP_SOURCE_HAS_ADF_DUPLEX       (1u << 10)

#define CAP_ENCODING_HAS_RAW            (1u << 11)
#define CAP_ENCODING_HAS_JPEG           (1u << 12)

// Oddities of particular models.
#define CAP_ENCODING_RAW_IS_CrYCb       (1u << 13)

/*
 * Diagnostic levels.
 *
 */
#define DBG_IMPORTANT   1
#define DBG_SERIOUS     2
#define DBG_WARN        3
#define DBG_EVENT       4
#define DBG_DETAIL      5
#define DBG_DEBUG       6

#define BROTHER_READ_BUFFER_LEN (64 * 1024)

/* based on https://stackoverflow.com/a/29856690 */
/* a constexpr would be great, but std::string_view is a C++17 feature */
const std::string computeMethodName(const std::string& function, const std::string& prettyFunction);

#define __COMPACT_PRETTY_FUNCTION__ computeMethodName(__FUNCTION__,__PRETTY_FUNCTION__).c_str() //c_str() is optional

/* Unfortunately, in C++11 there is no nice way to handle these cases in a single macro that I am aware of */
#define FDBG(LEVEL, TEXT, ...) DBG(LEVEL, "%s: " TEXT "\n", __COMPACT_PRETTY_FUNCTION__, __VA_ARGS__)
/* no parameter version */
#define FDBGNP(LEVEL, TEXT) DBG(LEVEL, "%s: " TEXT "\n", __COMPACT_PRETTY_FUNCTION__)

typedef enum
{
  BROTHER_SCAN_MODE_COLOR,
  BROTHER_SCAN_MODE_GRAY,
  BROTHER_SCAN_MODE_GRAY_DITHERED,
  BROTHER_SCAN_MODE_TEXT
} BrotherScanMode;

typedef enum
{
  BROTHER_SOURCE_NONE,
  BROTHER_SOURCE_AUTO,
  BROTHER_SOURCE_FLATBED,
  BROTHER_SOURCE_ADF,
  BROTHER_SOURCE_ADF_DUPLEX,
} BrotherSource;

struct BrotherParameters
{
  BrotherParameters():
    param_brightness (0),
    param_contrast (0),
    param_compression (SANE_TRUE),
    param_pixel_x_offset (0),
    param_pixel_x_width (0),
    param_pixel_y_offset (0),
    param_pixel_y_height (0),
    param_scan_mode (BROTHER_SCAN_MODE_COLOR),
    param_source(BROTHER_SOURCE_FLATBED),
    param_x_res(100),
    param_y_res(100)
  {
  }
  SANE_Int param_brightness;
  SANE_Int param_contrast;
  SANE_Bool param_compression;

  SANE_Int param_pixel_x_offset;
  SANE_Int param_pixel_x_width;
  SANE_Int param_pixel_y_offset;
  SANE_Int param_pixel_y_height;

  BrotherScanMode param_scan_mode;
  BrotherSource param_source;

  SANE_Int param_x_res;
  SANE_Int param_y_res;

};
