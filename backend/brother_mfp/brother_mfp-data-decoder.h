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

#include "../include/sane/config.h"

#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <setjmp.h>

#include <jpeglib.h>
#include <jerror.h>

#include "../include/sane/sane.h"
#include "../include/sane/sanei.h"

#include "brother_mfp-common.h"

/*
 * A base class / interface for decoding the actual image data.
 *
 */
class BrotherScanDataDecoder
{
public:
  BrotherScanDataDecoder(const BrotherParameters &params):
    decode_params(params)
  {
  }

  virtual ~BrotherScanDataDecoder()
  {
  }

  virtual void NewBlock() = 0;

  virtual SANE_Status DecodeScanData (const SANE_Byte *src_data, size_t src_data_len,
                               size_t *src_data_consumed, SANE_Byte *dst_data, size_t dest_data_len,
                               size_t *dest_data_written) = 0;

  virtual SANE_Bool HasUnprocessedDecodeData() = 0;

protected:
  BrotherParameters decode_params;
};

class BrotherJFIFDecoder : public BrotherScanDataDecoder
{
public:
  BrotherJFIFDecoder(const BrotherParameters &params):
    BrotherScanDataDecoder(params),
    decompress_bytes(0),
    raw_data_buffer(nullptr),
    decomp_line_size(0),
    dest_line_size(0)
  {
    InitLibJpeg();
  }

  static void ErrorExitManager(j_common_ptr cinfo);
  static void OutputMessageHandler(j_common_ptr cinfo);

  ~BrotherJFIFDecoder()
  {
    jpeg_destroy_decompress(&state.cinfo);
    delete [] raw_data_buffer;
    raw_data_buffer = nullptr;
  }

  void NewBlock() override;

  SANE_Status DecodeScanData (const SANE_Byte *src_data, size_t src_data_len,
                               size_t *src_data_consumed, SANE_Byte *dst_data, size_t dest_data_len,
                               size_t *dest_data_written) override;

  SANE_Bool HasUnprocessedDecodeData() override
    {
      return !state.have_read_header || (state.cinfo.output_scanline < state.cinfo.output_height);
    }

private:
  void InitLibJpeg()
  {
    /*
     * Not sure if this is a safe way to get the cinfo into
     * a consistent state but at least all pointers will be NULL.
     *
     */
    (void)memset(&state.cinfo, 0, sizeof(state.cinfo));

    state.cinfo.err = jpeg_std_error(&state.jerr);
    state.cinfo.err->error_exit = ErrorExitManager;
    state.cinfo.err->output_message = OutputMessageHandler;

    jpeg_create_decompress (&state.cinfo);

    /*
     * Set up source manager.
     *
     */
    state.src_mgr.init_source = InitSource;
    state.src_mgr.fill_input_buffer = FillInputBuffer;
    state.src_mgr.skip_input_data = SkipInputData;
    state.src_mgr.resync_to_restart = jpeg_resync_to_restart;
    state.src_mgr.term_source = TermSource;

    state.cinfo.src = &state.src_mgr;
  }

  static void InitSource (j_decompress_ptr cinfo);
  static boolean FillInputBuffer(j_decompress_ptr cinfo);
  static void SkipInputData(j_decompress_ptr cinfo, long num_bytes);
  static void TermSource(j_decompress_ptr cinfo);

  SANE_Status DecodeScanData_CompressBuffer (const SANE_Byte *src_data, size_t src_data_len,
                                              size_t *src_data_consumed, SANE_Byte *dst_data,
                                              size_t dest_data_len, size_t *dest_data_written);

  struct CompressionState
  {
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    struct jpeg_source_mgr src_mgr;
    bool have_read_header;
    jmp_buf my_env;
  } state;

  SANE_Byte decompress_buffer[1024 * 16];	// for the received JPEG data
  size_t decompress_bytes;

  SANE_Byte *raw_data_buffer;			// for the decompressed raw data.
  size_t decomp_line_size;

  size_t dest_line_size;
};

class BrotherGrayRLengthDecoder : public BrotherScanDataDecoder
{
public:
  BrotherGrayRLengthDecoder(const BrotherParameters &params):
    BrotherScanDataDecoder(params)
  {
  }
  void NewBlock() override;

  SANE_Status DecodeScanData (const SANE_Byte *src_data, size_t src_data_len,
                               size_t *src_data_consumed, SANE_Byte *dst_data, size_t dest_data_len,
                               size_t *dest_data_written) override;

  SANE_Bool HasUnprocessedDecodeData() override
  {
    return FALSE;
  }

private:
  BrotherParameters decode_params;
};


class BrotherInterleavedRGBColourDecoder : public BrotherScanDataDecoder
{
public:
  explicit BrotherInterleavedRGBColourDecoder(const BrotherParameters &params, SANE_Word capabilities):
    BrotherScanDataDecoder(params),
    capabilities(capabilities),
    scanline_buffer(nullptr),
    scanline_buffer_size(0),
    scanline_length(0),
    scanline_buffer_data(0)
  {
  }

  ~BrotherInterleavedRGBColourDecoder()
  {
    free(scanline_buffer);
  }
  void NewBlock() override;

  SANE_Status DecodeScanData (const SANE_Byte *src_data, size_t src_data_len,
                               size_t *src_data_consumed, SANE_Byte *dst_data, size_t dest_data_len,
                               size_t *dest_data_written) override;

  SANE_Bool HasUnprocessedDecodeData() override
  {
    return FALSE;
  }

  enum ChannelEncoding
  {
    CHANNELS_RGB,       // RGB
    CHANNELS_CrYCb      // YCrCb
  };
private:
  static void ConvertYCbCrToRGB (SANE_Byte y, SANE_Byte cb, SANE_Byte cr, SANE_Byte *red,
                                 SANE_Byte *green, SANE_Byte *blue);

//  BrotherParameters decode_params;
  SANE_Word capabilities;

  SANE_Byte *scanline_buffer;
  size_t scanline_buffer_size;
  size_t scanline_length;
  size_t scanline_buffer_data;
};


class BrotherGrayRawDecoder : public BrotherScanDataDecoder
{
public:
  BrotherGrayRawDecoder(const BrotherParameters &params):
    BrotherScanDataDecoder(params)
  {
  }
  void NewBlock() override;

  SANE_Status DecodeScanData (const SANE_Byte *src_data, size_t src_data_len,
                               size_t *src_data_consumed, SANE_Byte *dst_data, size_t dest_data_len,
                               size_t *dest_data_written) override;

  SANE_Bool HasUnprocessedDecodeData() override
  {
    return FALSE;
  }

private:
  BrotherParameters decode_params;
};
