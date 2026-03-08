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

#include "brother_mfp-common.h"

#include "../include/sane/sane.h"
#include "../include/sane/sanei.h"
#include "../include/sane/saneopts.h"
#include "../include/sane/sanei_config.h"
#include "../include/sane/sanei_usb.h"


/*
 * Functionality sets.
 *
 * For the moment, I am going to assume that the various proprietary
 * drivers follow the broad protocol versions and I will reflect this
 * until I see different.
 *
 * So these will reflect the brscan2, brscan3, etc that Brother use.
 *
 * We will have a encoder/decoder for each family.
 * From what I have seen, pretty much all the Brother families implement
 * the same basic protocol sequence, just with variations in technology.
 * So we don't need to abstract out the entire sequence, just the encoding
 * and decoding of each packet. The biggest aspect by far is the difference
 * in scan data formats.
 *
 */
typedef enum
{
  BROTHER_FAMILY_NONE,
  BROTHER_FAMILY_1,
  BROTHER_FAMILY_2,
  BROTHER_FAMILY_3,
  BROTHER_FAMILY_4,
  BROTHER_FAMILY_5
} BrotherFamily;



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

typedef SANE_Int BrotherSensor;

#define BROTHER_SENSOR_NONE     0
#define BROTHER_SENSOR_EMAIL    (1 << 0)
#define BROTHER_SENSOR_FILE     (1 << 1)
#define BROTHER_SENSOR_OCR      (1 << 2)
#define BROTHER_SENSOR_IMAGE    (1 << 3)

struct BrotherSessionResponse
{
  SANE_Bool ready;
};

struct BrotherButtonQueryResponse
{
  SANE_Bool has_button_press;
};

struct BrotherButtonStateResponse
{
  BrotherSensor button_value;   // raw value we get from the packet
};

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

struct BrotherBasicParamResponse
{
  int dummy;
  // fill me
};

struct BrotherQueryResponse
{
  int dummy;
  // fill me
};

struct BrotherSourceStatusResponse
{
  SANE_Bool source_ready;
};


/*
 * DecodeStatus
 *
 */
//enum DecodeStatus
//{
//  DECODE_STATUS_GOOD,
//  /* The next item (header, line, ...) is not yet complete */
//  DECODE_STATUS_TRUNCATED,
//  /* End of scan */
//  DECODE_STATUS_ENDOFDATA,
//  /* End of the last frame */
////  DECODE_STATUS_ENDOFFRAME_NO_MORE,
//  /* End of a frame, more frames will be coming */
////  DECODE_STATUS_ENDOFFRAME_WITH_MORE,
//  DECODE_STATUS_CANCEL,
//  DECODE_STATUS_ERROR,
//  DECODE_STATUS_MEMORY,
//  DECODE_STATUS_INVAL,
//  DECODE_STATUS_UNSUPPORTED,
//  DECODE_STATUS_PAPER_JAM,
//  DECODE_STATUS_COVER_OPEN,
//  DECODE_STATUS_NO_DOCS,
//};

struct ScanDataHeader
{
  ScanDataHeader():
    block_type(0),
    block_len(0),
    image_number(0)
  {
  }
  /*
   * A code defined by Brother, identifying the type of packet.
   * We set it to zero when we have processed all data in this packet.
   */
  SANE_Byte block_type;
  size_t block_len;
  SANE_Int image_number;
};

class BrotherEncoder
{
public:
  BrotherEncoder ()
  {
  }

  virtual ~BrotherEncoder ()
  {
  }

  virtual void NewPage () = 0;

  SANE_Status SetSource (BrotherSource source);
  SANE_Status SetScanMode (BrotherScanMode scan_mode);
  SANE_Status SetRes (SANE_Int x, SANE_Int y);
  SANE_Status SetContrast (SANE_Int contrast);
  SANE_Status SetBrightness (SANE_Int brightness);
  SANE_Status SetCompression (SANE_Bool compression);

  SANE_Status SetScanDimensions (SANE_Int pixel_x_offset, SANE_Int pixel_x_width,
                                  SANE_Int pixel_y_offset, SANE_Int pixel_y_height);

//  SANE_Status DecodeStatusToSaneStatus(DecodeStatus dec_ret)
//  {
//    static const SANE_Status status_lookup[] =
//        {
//            SANE_STATUS_GOOD,
//            SANE_STATUS_GOOD,
//            SANE_STATUS_EOF,
//            SANE_STATUS_EOF,
//            SANE_STATUS_EOF,
//            SANE_STATUS_CANCELLED,
//            SANE_STATUS_IO_ERROR,
//            SANE_STATUS_NO_MEM,
//            SANE_STATUS_INVAL,
//            SANE_STATUS_UNSUPPORTED,
//            SANE_STATUS_JAMMED,
//            SANE_STATUS_COVER_OPEN,
//            SANE_STATUS_NO_DOCS,
//      };
//
//    return status_lookup[dec_ret];
//  }

//  const char *DecodeStatusToString(DecodeStatus dec_ret)
//  {
//    static const char * status_lookup[] =
//        {
//            "DECODE_STATUS_GOOD",
//            "DECODE_STATUS_TRUNCATED",
//            "DECODE_STATUS_ENDOFDATA",
//            "DECODE_STATUS_ENDOFFRAME_NO_MORE",
//            "DECODE_STATUS_ENDOFFRAME_WITH_MORE",
//            "DECODE_STATUS_CANCEL",
//            "DECODE_STATUS_ERROR",
//            "DECODE_STATUS_MEMORY",
//            "DECODE_STATUS_INVAL",
//            "DECODE_STATUS_UNSUPPORTED",
//            "DECODE_STATUS_PAPER_JAM",
//            "DECODE_STATUS_COVER_OPEN",
//            "DECODE_STATUS_NO_DOCS",
//      };
//
//    return status_lookup[dec_ret];
//  }


  virtual const char* ScanModeToText (BrotherScanMode scan_mode);

  // Buttons.
  virtual SANE_Status DecodeSessionResp (const SANE_Byte *data, size_t data_len,
                                          BrotherSessionResponse &response) = 0;

  virtual SANE_Status DecodeButtonQueryResp (const SANE_Byte *data, size_t data_len,
                                              BrotherButtonQueryResponse &response) = 0;

  virtual SANE_Status DecodeButtonStateResp (const SANE_Byte *data, size_t data_len,
                                              BrotherButtonStateResponse &response) = 0;

  // Requests and responses.
  virtual SANE_Status EncodeQueryBlock (SANE_Byte *data, size_t data_len, size_t *length) = 0;

  virtual SANE_Status DecodeQueryBlockResp (const SANE_Byte *data, size_t data_len,
                                             BrotherQueryResponse &response) = 0;

  virtual SANE_Status EncodeBasicParameterBlock (SANE_Byte *data, size_t data_len,
                                                  size_t *length) = 0;

  virtual SANE_Status DecodeBasicParameterBlockResp (const SANE_Byte *data, size_t data_len,
                                                      BrotherBasicParamResponse &response) = 0;

  virtual SANE_Status EncodeSourceStatusBlock (SANE_Byte *data, size_t data_len, size_t *length) = 0;

  virtual SANE_Status DecodeSourceStatusBlockResp (const SANE_Byte *data, size_t data_len,
                                                    BrotherSourceStatusResponse &response) = 0;

  virtual SANE_Status EncodeSourceSelectBlock (SANE_Byte *data, size_t data_len, size_t *length) = 0;

  virtual SANE_Status DecodeSourceSelectBlockResp (const SANE_Byte *data, size_t data_len,
                                                    BrotherSourceStatusResponse &response) = 0;

  virtual SANE_Status EncodeParameterBlock (SANE_Byte *data, size_t data_len, size_t *length) = 0;

  virtual SANE_Status EncodeParameterBlockBlank (SANE_Byte *data, size_t data_len,
                                                  size_t *length) = 0;

  // Scan data decoding.
  virtual SANE_Status DecodeScanData (const SANE_Byte *src_data, size_t src_data_len,
                                       size_t *src_data_consumed, SANE_Byte *dst_data,
                                       size_t dest_data_len, size_t *dest_data_written) = 0;

protected:
  BrotherParameters scan_params;
};

/** A base class / interface for decoding the actual image data */
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
      return decompress_bytes > 0 ? SANE_TRUE : SANE_FALSE;
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

  BrotherParameters decode_params;
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

class BrotherEncoderFamily234Base : public BrotherEncoder
{
public:
  explicit BrotherEncoderFamily234Base(SANE_Word caps):
    capabilities(caps),
    current_decoder(nullptr)
  {
  }

  ~BrotherEncoderFamily234Base ()
  {
  }

  void NewPage() override
  {
    current_header.block_type = 0;

    delete current_decoder;
    current_decoder = nullptr;
  }

  SANE_Status EncodeQueryBlock (SANE_Byte *data, size_t data_len, size_t *length) override;

  SANE_Status DecodeSessionResp (const SANE_Byte *data, size_t data_len,
                                  BrotherSessionResponse &response) override;

  SANE_Status EncodeBasicParameterBlock (SANE_Byte *data, size_t data_len, size_t *length)
      override;

  SANE_Status DecodeBasicParameterBlockResp (const SANE_Byte *data, size_t data_len,
                                              BrotherBasicParamResponse &response) override;

  SANE_Status EncodeSourceStatusBlock (SANE_Byte *data, size_t data_len, size_t *length) override;

  SANE_Status DecodeSourceStatusBlockResp (const SANE_Byte *data, size_t data_len,
                                            BrotherSourceStatusResponse &response) override;

  SANE_Status EncodeSourceSelectBlock (SANE_Byte *data, size_t data_len, size_t *length) override;

  SANE_Status DecodeSourceSelectBlockResp (const SANE_Byte *data, size_t data_len,
                                            BrotherSourceStatusResponse &response) override;

  SANE_Status EncodeParameterBlock (SANE_Byte *data, size_t data_len, size_t *length) override;

  SANE_Status EncodeParameterBlockBlank (SANE_Byte *data, size_t data_len, size_t *length)
      override;

  SANE_Status DecodeScanData (const SANE_Byte *src_data, size_t src_data_len,
                               size_t *src_data_consumed, SANE_Byte *dst_data, size_t dest_data_len,
                               size_t *dest_data_written) override;

  SANE_Status DecodeButtonQueryResp (const SANE_Byte *data, size_t data_len,
                                      BrotherButtonQueryResponse &response) override;

  SANE_Status DecodeButtonStateResp (const SANE_Byte *data, size_t data_len,
                                      BrotherButtonStateResponse &response) override;

protected:
  SANE_Status ProcessNextPacket(const SANE_Byte* &src_data, size_t &src_data_len,
                              size_t dest_data_len, size_t orig_dest_data_len,
                              bool &new_block);

  virtual SANE_Status DecodeScanDataHeader (const SANE_Byte *src_data, size_t src_data_len,
                                             size_t *src_data_consumed, ScanDataHeader &header);

  virtual char const* GetFamilySpecificParameterBlock ()
  {
    char const * res = "";
    return res;
  }

  SANE_Word capabilities;
  ScanDataHeader current_header;
  BrotherScanDataDecoder *current_decoder;
};


class BrotherEncoderFamily2 : public BrotherEncoderFamily234Base
{
  using BrotherEncoderFamily234Base::BrotherEncoderFamily234Base;
protected:
  SANE_Status DecodeQueryBlockResp (const SANE_Byte *data, size_t data_len,
                                             BrotherQueryResponse &response) override;
  SANE_Status DecodeScanDataHeader (const SANE_Byte *src_data, size_t src_data_len,
                                     size_t *src_data_consumed, ScanDataHeader &header) override;
};

// The difference between family2 and family3 is miniscule, so this is more efficient
class BrotherEncoderFamily3 : public BrotherEncoderFamily2
{
  using BrotherEncoderFamily2::BrotherEncoderFamily2;
protected:
  SANE_Status DecodeQueryBlockResp (const SANE_Byte *data, size_t data_len,
                                             BrotherQueryResponse &response) override;
};

class BrotherEncoderFamily4 : public BrotherEncoderFamily234Base
{
  using BrotherEncoderFamily234Base::BrotherEncoderFamily234Base;

  SANE_Status EncodeParameterBlockBlank (SANE_Byte *data, size_t data_len,
                                          size_t *length) override;
protected:
  SANE_Status DecodeScanDataHeader (const SANE_Byte *src_data, size_t src_data_len,
                                     size_t *src_data_consumed, ScanDataHeader &header) override;

  SANE_Status DecodeQueryBlockResp (const SANE_Byte *data, size_t data_len,
                                             BrotherQueryResponse &response) override;

  char const* GetFamilySpecificParameterBlock () override;
};

class BrotherImageCache
{
public:
  BrotherImageCache():
    cache_file(nullptr),
    has_read(false)
  {
  }

  ~BrotherImageCache()
  {
    fclose(cache_file);
    cache_file = nullptr;
  }

  bool WriteData(const SANE_Byte *buffer, size_t buffer_len);
  bool ReadData(SANE_Byte *buffer, size_t buffer_len, size_t &data_written);

private:
  FILE *cache_file;
  bool has_read;
};

class BrotherEncoderFamily5 : public BrotherEncoder
{
public:
  explicit BrotherEncoderFamily5(SANE_Word capabilities):
  end_image_flag(false),
  out_of_docs(false),
  current_decoder(nullptr)

  {
    (void)capabilities;
  }

  ~BrotherEncoderFamily5 ()
  {
  }

  void NewPage() override
  {
    current_header.block_type = 0;
    delete current_decoder;
    current_decoder = nullptr;
    end_image_flag = false;
  }

  const char* ScanModeToText (BrotherScanMode scan_mode) override;

  SANE_Status EncodeQueryBlock (SANE_Byte *data, size_t data_len, size_t *length) override;

  SANE_Status DecodeQueryBlockResp (const SANE_Byte *data, size_t data_len,
                                     BrotherQueryResponse &response) override;

  SANE_Status DecodeSessionResp (const SANE_Byte *data, size_t data_len,
                                  BrotherSessionResponse &response) override;

  SANE_Status EncodeBasicParameterBlock (SANE_Byte *data, size_t data_len, size_t *length)
      override;

  SANE_Status DecodeBasicParameterBlockResp (const SANE_Byte *data, size_t data_len,
                                              BrotherBasicParamResponse &response) override;

  SANE_Status EncodeSourceStatusBlock (SANE_Byte *data, size_t data_len, size_t *length) override;

  SANE_Status DecodeSourceStatusBlockResp (const SANE_Byte *data, size_t data_len,
                                            BrotherSourceStatusResponse &response) override;

  SANE_Status EncodeSourceSelectBlock (SANE_Byte *data, size_t data_len, size_t *length) override;

  SANE_Status DecodeSourceSelectBlockResp (const SANE_Byte *data, size_t data_len,
                                            BrotherSourceStatusResponse &response) override;

  SANE_Status EncodeParameterBlock (SANE_Byte *data, size_t data_len, size_t *length) override;

  SANE_Status EncodeParameterBlockBlank (SANE_Byte *data, size_t data_len, size_t *length)
      override;

  SANE_Status DecodeScanData (const SANE_Byte *src_data, size_t src_data_len,
                               size_t *src_data_consumed, SANE_Byte *dst_data, size_t dest_data_len,
                               size_t *dest_data_written) override;

  SANE_Status DecodeButtonQueryResp (const SANE_Byte *data, size_t data_len,
                                      BrotherButtonQueryResponse &response) override;

  SANE_Status DecodeButtonStateResp (const SANE_Byte *data, size_t data_len,
                                      BrotherButtonStateResponse &response) override;

private:
  SANE_Status DecodeScanDataHeader (const SANE_Byte *src_data, size_t src_data_len,
                                     size_t *src_data_consumed, ScanDataHeader &header);

  ScanDataHeader current_header;
  bool end_image_flag;
  bool out_of_docs;
  BrotherScanDataDecoder *current_decoder;


};
