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

#define DEBUG_DECLARE_ONLY

#include "../include/sane/config.h"

#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "../include/sane/sane.h"
#include "../include/sane/sanei.h"
#include "../include/sane/sanei_debug.h"

#include "brother_mfp-common.h"
#include "brother_mfp-family.h"

ScanDataHeader::ScanDataHeader () :
    block_type (BROTHER_DATA_BLOCK_NONE),
    block_len (0),
    image_number (0)
{
}


//const char* BrotherFamily2::ScanModeToText (BrotherScanMode scan_mode)
//{
//  static const char *scan_mode_text[] =
//    { "CGRAY", "GRAY64", "ERRDIF", "TEXT" };
//
//  return scan_mode_text[scan_mode];
//}

SANE_Status BrotherFamily::SetSource (BrotherSource source)
{
  scan_params.param_source = source;

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily::SetScanMode (BrotherScanMode scan_mode)
{

  /*
   * TODO: Perhaps check the validity of the parameter?
   */
  scan_params.param_scan_mode = scan_mode;

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily::SetRes (SANE_Int x, SANE_Int y)
{
  FDBG (DBG_EVENT, "XXXX res set to %d/%d", x, y);

  /*
   * TODO: Perhaps check the validity of the parameter?
   */
  scan_params.param_x_res = x;
  scan_params.param_y_res = y;

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily::SetContrast (SANE_Int contrast)
{

  /*
   * TODO: Perhaps check the validity of the parameter?
   */
  scan_params.param_contrast = contrast;

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily::SetBrightness (SANE_Int brightness)
{

  /*
   * TODO: Perhaps check the validity of the parameter?
   */
  scan_params.param_brightness = brightness;

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily::SetCompression (SANE_Bool compression)
{
  scan_params.param_compression = compression;

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily::SetScanDimensions (SANE_Int pixel_x_offset, SANE_Int pixel_x_width,
                                                SANE_Int pixel_y_offset, SANE_Int pixel_y_height)
{

  /*
   * TODO: Perhaps check the validity of the parameter?
   */
  scan_params.param_pixel_x_offset = pixel_x_offset;
  scan_params.param_pixel_x_width = pixel_x_width;
  scan_params.param_pixel_y_offset = pixel_y_offset;
  scan_params.param_pixel_y_height = pixel_y_height;

  FDBG (DBG_DETAIL,
       "scan dimensions: x:%d,%d; y:%d,%d",
       pixel_x_offset,
       pixel_x_width,
       pixel_y_offset,
       pixel_y_height);

  return SANE_STATUS_GOOD;
}

/*
 * BrotherImageCache.
 *
 * A store for images received and decoded during simultaneous duplex
 * scanning. In this case, we can receive out of order images interleaved
 * together. We need to decode both, send one and store the other. Then
 * later send the other.
 *
 * This will primarily be stored in a temporary file.
 *
 * Once reading has occurred, writing is not longer permitted.
 *
 */
bool BrotherImageCache::WriteData(const SANE_Byte *buffer, size_t buffer_len)
{
  if (has_read)
    {
      FDBGNP (DBG_SERIOUS, "Attempt to write to image cache after having read.");
      return false;
    }

  /*
   * Create and open a temporary file to receive the data if we
   * haven't already.
   *
   */
  if (!cache_file)
    {
      cache_file = tmpfile();
      if (!cache_file)
	{
	  FDBG(DBG_SERIOUS, "Failed to create an image cache file [%s].", strerror(errno));
	  return false;
	}
    }

  if (fwrite(buffer, 1, buffer_len, cache_file) != buffer_len)
    {
      FDBG(DBG_SERIOUS, "Failed to write to an image cache file [%s].", strerror(errno));
      return false;
    }

  return true;
}

bool BrotherImageCache::ReadData(SANE_Byte *buffer, size_t buffer_len, size_t &data_written)
{
  if (!cache_file)
    {
      FDBGNP (DBG_SERIOUS, "Attempt to read from empty image cache file.");
      return false;
    }

  if (!has_read)
    {
      has_read = true;
      if (fseek(cache_file, 0, SEEK_SET) != 0)
	{
	  FDBG (DBG_SERIOUS, "An error occurred reading the image cache [%s].", strerror(errno));
	  return false;
	}
    }

  data_written = fread(buffer, 1, buffer_len, cache_file);
  if (ferror(cache_file) != 0)
    {
      FDBG (DBG_SERIOUS, "An error occurred reading the image cache [%s].", strerror(errno));
      return false;
    }

  return true;
}
