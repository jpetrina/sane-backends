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
#include <fcntl.h>
#include <sys/ioctl.h>
#include <algorithm>

#include <jpeglib.h>
#include <jerror.h>

#include "../include/sane/sane.h"
#include "../include/sane/sanei.h"
#include "../include/sane/saneopts.h"
#include "../include/sane/sanei_config.h"
#include "../include/sane/sanei_usb.h"
#include "../include/sane/sanei_debug.h"

#include "brother_mfp-common.h"
#include "brother_mfp-encoder.h"

/*
 * Block header identifiers.
 *
 * Scan data codes.
 *
 */
// Brother 5 codes
#define BROTHER_DATA_BLOCK_RLENGTH5             0x01
#define BROTHER_DATA_BLOCK_JPEG5                0x02

#define BROTHER_DATA_BLOCK_END_OF_FRAME_NO_MORE5  0x21

// Brother 2, 3, 4 codes
#define BROTHER_DATA_BLOCK_GRAY_RAW             0x40
#define BROTHER_DATA_BLOCK_GRAY_RLENGTH         0x42
#define BROTHER_DATA_BLOCK_RED_RAW              0x44
#define BROTHER_DATA_BLOCK_GREEN_RAW            0x48
#define BROTHER_DATA_BLOCK_BLUE_RAW             0x4c

#define BROTHER_DATA_BLOCK_JPEG                 0x64

/*
 * Termination codes.
 */
#define BROTHER_DATA_BLOCK_SCAN_FINISHED        0x80
#define BROTHER_DATA_BLOCK_END_OF_FRAME_MORE    0x81
#define BROTHER_DATA_BLOCK_END_OF_FRAME_NO_MORE 0x82
#define BROTHER_DATA_BLOCK_CANCEL               0x86

/*
 * Error condition codes.
 *
 */
#define BROTHER_DATA_QUERY_RESPONSE             0xc1
#define BROTHER_DATA_BLOCK_NO_DOCS              0xc2
#define BROTHER_DATA_BLOCK_PAPER_JAM            0xc3
#define BROTHER_DATA_BLOCK_COVER_OPEN           0xc4
// unsure as to the meaning of this or how to get it again.
#define BROTHER_DATA_BLOCK_CLEARING_PAPER       0xc5
// unsure about this one, too. Only recovery is a printer restart.
// Can happen when the previous scan was interrupted by a driver crash.
#define BROTHER_DATA_BLOCK_DEVICE_STUCK         0xc6

/*
 * Other protocol error?
 *
 */
#define BROTHER_DATA_BLOCK_PROTOCOL_ERROR       0xd0

const char* BrotherEncoder::ScanModeToText (BrotherScanMode scan_mode)
{
  static const char *scan_mode_text[] =
    { "CGRAY", "GRAY64", "ERRDIF", "TEXT" };

  return scan_mode_text[scan_mode];
}

DecodeStatus BrotherEncoder::SetSource (BrotherSource source)
{
  scan_params.param_source = source;

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoder::SetScanMode (BrotherScanMode scan_mode)
{

  /*
   * TODO: Perhaps check the validity of the parameter?
   */
  scan_params.param_scan_mode = scan_mode;

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoder::SetRes (SANE_Int x, SANE_Int y)
{
  FDBG (DBG_EVENT, "XXXX res set to %d/%d", x, y);

  /*
   * TODO: Perhaps check the validity of the parameter?
   */
  scan_params.param_x_res = x;
  scan_params.param_y_res = y;

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoder::SetContrast (SANE_Int contrast)
{

  /*
   * TODO: Perhaps check the validity of the parameter?
   */
  scan_params.param_contrast = contrast;

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoder::SetBrightness (SANE_Int brightness)
{

  /*
   * TODO: Perhaps check the validity of the parameter?
   */
  scan_params.param_brightness = brightness;

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoder::SetCompression (SANE_Bool compression)
{
  scan_params.param_compression = compression;

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoder::SetScanDimensions (SANE_Int pixel_x_offset, SANE_Int pixel_x_width,
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

  return DECODE_STATUS_GOOD;
}

/*
 * --------------------------------------------
 * Family 2,3,4 Encoding
 * --------------------------------------------
 *
 */
DecodeStatus BrotherEncoderFamily234Base::EncodeQueryBlock (SANE_Byte *data, size_t data_len,
                                                      size_t *length)
{
  *length = snprintf ((char*) data, data_len, "\x1b" "\x51" "\x0a" "\x80");

  if (*length > data_len)
    {
      FDBG (DBG_SERIOUS,
           "query block too long for buffer: %zu",
           *length);
      return DECODE_STATUS_INVAL;
    }

  return DECODE_STATUS_GOOD;

}

DecodeStatus BrotherEncoderFamily2::DecodeQueryBlockResp (const SANE_Byte *data, size_t data_len,
                                                          BrotherQueryResponse &response)
{

  /*
   * Formatting and content checks.
   *
   */
  if ((data_len != 16) || (data[0] != BROTHER_DATA_QUERY_RESPONSE))

    {
      FDBG (DBG_SERIOUS,
           "invalid query response block: len = %zu",
           data_len);

      return DECODE_STATUS_ERROR;
    }

  /*
   * TODO: decode the response block to fill in the details.
   *
   */
  response.dummy = 0;

  return DECODE_STATUS_GOOD;
}


DecodeStatus BrotherEncoderFamily3::DecodeQueryBlockResp (const SANE_Byte *data, size_t data_len,
                                                          BrotherQueryResponse &response)
{

  /*
   * Formatting and content checks.
   *
   */
  if ((data_len != 20) || (data[0] != BROTHER_DATA_QUERY_RESPONSE))

    {
      FDBG (DBG_SERIOUS,
           "invalid query response block: len = %zu",
           data_len);

      return DECODE_STATUS_ERROR;
    }

  /*
   * TODO: decode the response block to fill in the details.
   *
   */
  response.dummy = 0;

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoderFamily4::DecodeQueryBlockResp (const SANE_Byte *data, size_t data_len,
                                                          BrotherQueryResponse &response)
{

  /*
   * Formatting and content checks.
   *
   */
  if ((data_len != 72) || (data[0] != BROTHER_DATA_QUERY_RESPONSE))

    {
      FDBG (DBG_SERIOUS,
           "invalid query response block: len = %zu",
           data_len);

      return DECODE_STATUS_ERROR;
    }

  /*
   * TODO: decode the response block to fill in the details.
   *
   */
  response.dummy = 0;

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoderFamily234Base::EncodeSourceStatusBlock (SANE_Byte *data, size_t data_len, size_t *length)
{
  *length = snprintf ((char*) data, data_len, "\x1b" "D\nADF\n" "\x80");

  if (*length > data_len)
    {
      FDBG (DBG_SERIOUS,
           "Source status block too long for buffer: %zu",
           *length);
      return DECODE_STATUS_INVAL;
    }

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoderFamily234Base::DecodeSourceStatusBlockResp (const SANE_Byte *data, size_t data_len,
                                                                 BrotherSourceStatusResponse &response)
{
  if (data_len != 1)
    {
      FDBG (DBG_SERIOUS,
           "source status response invalid: %zu",
           data_len);
      return DECODE_STATUS_ERROR;
    }

  /*
   * We can get error condition codes from this block.
   *
   */
  switch (data[0])
  {
    case BROTHER_DATA_BLOCK_PAPER_JAM:
      return DECODE_STATUS_PAPER_JAM;

    case BROTHER_DATA_BLOCK_COVER_OPEN:
      return DECODE_STATUS_COVER_OPEN;

    case BROTHER_DATA_BLOCK_NO_DOCS:
      response.source_ready = SANE_FALSE;
      return DECODE_STATUS_GOOD;

    case BROTHER_DATA_BLOCK_SCAN_FINISHED:
      response.source_ready = SANE_TRUE;
      return DECODE_STATUS_GOOD;

    default:
      return DECODE_STATUS_ERROR;
  }
}

DecodeStatus BrotherEncoderFamily234Base::EncodeSourceSelectBlock (SANE_Byte *data, size_t data_len, size_t *length)
{
  *length = snprintf ((char*) data,
                      data_len,
                      "\x1b" "S\n%s\n" "\x80",
                      scan_params.param_source == BROTHER_SOURCE_AUTO ?
                          "AUTO" :
                          (scan_params.param_source == BROTHER_SOURCE_FLATBED ? "FB" : "ADF"));

  if (*length > data_len)
    {
      FDBG (DBG_SERIOUS,
           "Source select block too long for buffer: %zu",
           *length);
      return DECODE_STATUS_INVAL;
    }

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoderFamily234Base::DecodeSourceSelectBlockResp (const SANE_Byte *data,
                                                                 size_t data_len,
                                                                 BrotherSourceStatusResponse &response)
{
  if (data_len != 1)
    {
      FDBG (DBG_SERIOUS,
           "source select response invalid: %zu",
           data_len);
      return DECODE_STATUS_ERROR;
    }

  /*
   * We can get error condition codes from this block.
   *
   */
  switch (data[0])
  {
    case BROTHER_DATA_BLOCK_SCAN_FINISHED:
      response.source_ready = SANE_TRUE;
      return DECODE_STATUS_GOOD;

    case BROTHER_DATA_BLOCK_PROTOCOL_ERROR:
      response.source_ready = SANE_FALSE;
      return DECODE_STATUS_INVAL;

    default:
      return DECODE_STATUS_ERROR;
  }
}

DecodeStatus BrotherEncoderFamily234Base::DecodeSessionResp (const SANE_Byte *data, size_t data_len,
                                                       BrotherSessionResponse &response)
{
  /*
   * Formatting and content checks.
   *
   *
   */
  if ((data_len != 5)
      || ((memcmp (data, "\x05" "\x10" "\x01" "\x02", 4) != 0)
          && (memcmp (data, "\x05" "\x10" "\x02" "\x02", 4) != 0)))

    {
      FDBG (DBG_SERIOUS,
           "invalid session response block: len = %zu",
           data_len);

      return DECODE_STATUS_ERROR;
    }

  /*
   * TODO: Check the valid status values are 0x00 or 0x20
   *
   */
  if ((data[4] != 0x00) && (data[4] != 0x80))

    {
      FDBG (DBG_SERIOUS,
           "invalid session response: data[4]=%u",
           (unsigned int) data[4]);

      return DECODE_STATUS_ERROR;
    }

  /*
   * TODO: figure out what the rest of the packet is supposed to be.
   *
   */
  response.ready = data[4] == 0x00? SANE_TRUE: SANE_FALSE;

  return DECODE_STATUS_GOOD;
}

/*
 * NOTE:
 * Supply a buffer that is at least 1 char longer than is necessary
 * as snprintf() will require space for a terminating NUL.
 *
 */
DecodeStatus BrotherEncoderFamily234Base::EncodeBasicParameterBlock (SANE_Byte *data, size_t data_len,
                                                               size_t *length)
{
  const char *mode_text = ScanModeToText (scan_params.param_scan_mode);
  if (nullptr == mode_text)
    {
      FDBG (DBG_SERIOUS,
           "failed to get scan mode text for mode %d",
           scan_params.param_scan_mode);
      return DECODE_STATUS_INVAL;
    }

  *length = snprintf ((char*) data,
                      data_len,
                      "\x1b" "I\nR=%u,%u\nM=%s\n" "\x80",
                      (unsigned int) scan_params.param_x_res,
                      (unsigned int) scan_params.param_y_res,
                      mode_text);

  if (*length > data_len)
    {
      FDBG (DBG_SERIOUS,
           "parameter block too long for buffer: %zu",
           *length);
      return DECODE_STATUS_INVAL;
    }

  return DECODE_STATUS_GOOD;
}


DecodeStatus BrotherEncoderFamily234Base::DecodeBasicParameterBlockResp (const SANE_Byte *data, size_t data_len,
                                                                   BrotherBasicParamResponse &response)
{
  /*
   * TODO: Decode this block.
   * Might contain some useful stuff, like limits for selected parameters.
   *
   */
  (void)data;
  (void)data_len;
  (void)response;

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoderFamily234Base::EncodeParameterBlockBlank (SANE_Byte *data, size_t data_len,
                                                               size_t *length)
{
  const char *mode_text = ScanModeToText (scan_params.param_scan_mode);
  if (nullptr == mode_text)
    {
      FDBG (DBG_SERIOUS,
           "failed to get scan mode text for mode %d",
           scan_params.param_scan_mode);
      return DECODE_STATUS_INVAL;
    }

  *length = snprintf ((char*) data, data_len, "\x1b" "X\n\x80");

  if (*length > data_len)
    {
      FDBG (DBG_SERIOUS,
           "parameter block too long for buffer: %zu",
           *length);
      return DECODE_STATUS_INVAL;
    }

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoderFamily234Base::EncodeParameterBlock (SANE_Byte *data, size_t data_len,
                                                          size_t *length)
{
  const char *mode_text = ScanModeToText (scan_params.param_scan_mode);
  if (nullptr == mode_text)
    {
      FDBG (DBG_SERIOUS,
           "failed to get scan mode text for mode %d",
           scan_params.param_scan_mode);
      return DECODE_STATUS_INVAL;
    }

  const char* format_string =
    "\x1b"
    "X\nR=%u,%u\nM=%s\n"  /* resolution, color mode */
    "%s\n"                /* compression */
    "B=%u\nN=%u\n"        /* brightness, contrast */
    "A=%u,%u,%u,%u\n"     /* scan area */
    "%s"                  /* family specific parameters */
    "D=%s\n"              /* single / duplex */
    "\x80";
  *length = snprintf (
    (char*) data,
    data_len,
    format_string,
    (unsigned int) scan_params.param_x_res,
    (unsigned int) scan_params.param_y_res,
    mode_text,
    (scan_params.param_scan_mode == BROTHER_SCAN_MODE_COLOR) ?
        (scan_params.param_compression ? "C=JPEG\nJ=MID" : "C=NONE") :
        "C=RLENGTH",
    (unsigned int) (scan_params.param_brightness + 50),
    (unsigned int) (scan_params.param_contrast + 50),
    (unsigned int) (scan_params.param_pixel_x_offset),
    (unsigned int) (scan_params.param_pixel_y_offset),
    (unsigned int) (scan_params.param_pixel_x_offset
        + scan_params.param_pixel_x_width),
    (unsigned int) (scan_params.param_pixel_y_offset
        + scan_params.param_pixel_y_height),
    GetFamilySpecificParameterBlock(),
    scan_params.param_source == BROTHER_SOURCE_ADF_DUPLEX ? "DUP" : "SIN");

  if (*length > data_len)
    {
      FDBG (DBG_SERIOUS,
           "parameter block too long for buffer: %zu",
           *length);
      return DECODE_STATUS_INVAL;
    }

  return DECODE_STATUS_GOOD;
}

char const* BrotherEncoderFamily4::GetFamilySpecificParameterBlock ()
{
  /* additional scan parameters in family4, might not be needed */
  return "S=NORMAL_SCAN\nP=0\nG=0\nL=0\n";
}

/*
 *
 * This function updates current_header in case the current packet has been processed entirely.
 * Some special cases are dealt with, including the case where the next packet is an EOF
 * but there is unprocessed data left in the decoder.
 * 
 */
DecodeStatus BrotherEncoderFamily234Base::ProcessNextPacket(const SANE_Byte* &src_data, size_t &src_data_len,
                                                   size_t dest_data_len, size_t orig_dest_data_len,
                                                   bool &new_block)
{
    /*
    * If we are not in a block, then decode the next block.
    *
    * We might not have enough bytes to fully decode the header.
    * In which case, we wait for some more data.
    *
    */
  new_block = false;


  if (current_header.block_type != 0)
    /* The current packet has not been processed completely */
    return DECODE_STATUS_GOOD;
  
  /* The number of bytes we consume in this method call */
  size_t header_consumed = 0;
  ScanDataHeader next_header = {};
  DecodeStatus ret_status = DecodeScanDataHeader(src_data, src_data_len, &header_consumed, next_header);

  switch (ret_status)
  {
    case DECODE_STATUS_TRUNCATED:
      /* This means we don't have enough data to decode a header yet.
      Try again next time if we have read more data. */
      header_consumed = 0;
      break;
    case DECODE_STATUS_CANCEL:
      FDBGNP (DBG_IMPORTANT, "cancel detected");
      /* in this case we can return early, no more processing needed */
      return ret_status;
    case DECODE_STATUS_GOOD:
      current_header = next_header;

      FDBG (DBG_IMPORTANT,
          "decoded new block header: 0x%2.2x, length=%zu",
          current_header.block_type,
          current_header.block_len);

      new_block = true;
      break;
    /* The first one likely isn't returned anymore, but it doesn't hurt to cover the case */
    case DECODE_STATUS_ENDOFDATA:
    case DECODE_STATUS_ENDOFFRAME_NO_MORE:
    case DECODE_STATUS_ENDOFFRAME_WITH_MORE:
      FDBG (DBG_DETAIL,
          "Next packet is an EOF: %s",
          DecodeStatusToString(ret_status));
      /* These are handled below */
      break;
    default:
      /* All the remaining decode statuses are errors of varying severity */
      FDBG (DBG_IMPORTANT,
          "Error %s",
          DecodeStatusToString(ret_status));
      /* In this case we can also return early since the scan will be aborted */
      return ret_status;
  }

  if (ret_status != DECODE_STATUS_GOOD)
    /*
     * Multiple scenarios lead here. For example, ret_status could be one of the EOFs
     * or STATUS_TRUNCATED. There are three cases here:
     * 1. There is unprocessed data left in a decoder.
     *    This happens e.g. when the previous call of sane_read() moved the penultimate packet
     *    into the JFIF decode buffer, but the output buffer was too small for the entire
     *    decompressed contents, so some data is left in the decode buffer.
     *    In this case, the data must be processed first, so we don't consume this header now.
     *
     * 2. There is no data in the decoder, but the calling DecodeScanData() has produced some output.
     *    This happens when the decode buffer and the penultimate packet were exhausted
     *    in this call of sane_read(), partially filling the output buffer.
     *    In this case, we want to return SANE_STATUS_GOOD to transmit the remaining data
     *    and return SANE_STATUS_EOF on the subsequent call of sane_read().
     *    Therefore, this header will not be consumed now.
     * 
     * 3. There is no data in the decode and we have not produced output.
     *    In this case, we do want to consume the header and return ret_status.
     *
     */
    {
      if (jfif_decoder.HasUnprocessedDecodeData())
        {
          FDBGNP (DBG_DEBUG, "Processing cached data before handling next packet");
          /*
           * We turn the packet back into a JPEG block, even though it has length zero.
           * This causes the subsequent code to call the JFIF decoder, which in turn drains its decode buffer.
           * A cleaner solution could be an internal enum for the current state of the decoder.
           */
          current_header.block_type = BROTHER_DATA_BLOCK_JPEG;
          /* The header will be consumed at a later point */
          header_consumed = 0;
          ret_status = DECODE_STATUS_GOOD;
        }
      else if (orig_dest_data_len > dest_data_len)
        {
          FDBGNP (DBG_DEBUG, "Transmitting generated data before handling next packet");
          header_consumed = 0;
          /* Here we leave current_header.block_type at 0 so we exit the caller's loop early */
          ret_status = DECODE_STATUS_GOOD;
        }
    }

  /* Move the source pointer to the beginning of the packet's payload */
  src_data += header_consumed;
  src_data_len -= header_consumed;

  return ret_status;
}

DecodeStatus BrotherEncoderFamily234Base::DecodeScanData (const SANE_Byte *src_data, size_t src_data_len,
                                                    size_t *src_data_consumed, SANE_Byte *dest_data,
                                                    size_t dest_data_len, size_t *dest_data_written)
{
  FDBG (DBG_EVENT, "Getting %zu bytes of new data", src_data_len);
  DecodeStatus ret_status = DECODE_STATUS_GOOD;

  *src_data_consumed = 0;
  *dest_data_written = 0;

  /*
   * We will use this to compute the bytes consumed at the end.
   *
   */
  size_t orig_src_data_len = src_data_len;
  size_t orig_dest_data_len = dest_data_len;

  /*
   * Now take note here that we do not *necessarily* need src_data to be able to decode.
   * We might have terminated the previous call not because we ran out of input, but because
   * we ran out of output space. Some blocks are just repetitions that are unrolled and require
   * no additional src data. So we enter this loop *even if there is nothing in the src_data buffer*.
   *
   * All of the functions that we are about to call must be able to deal with src_data_len == 0
   * intelligently.
   *
   */
  for (;;)
    {
      /*
       * If we are not in a block, then decode the next block.
       * We might not have enough bytes to fully decode the header.
       * In that case we wait for some more data.
       *
       */
      bool new_block = false;
      size_t bytes_consumed = 0;
      size_t bytes_written = 0;

      ret_status = ProcessNextPacket(src_data, src_data_len, dest_data_len, orig_dest_data_len, new_block);
      if (ret_status != DECODE_STATUS_GOOD || current_header.block_type == 0)
      {
        /*
         * block_type == 0 means that we don't have further data to process,
         * e.g. because the next packet is an EOF and *dest_data_written > 0.
         */
        break;
      }

      size_t in_len = std::min(src_data_len, current_header.block_len);

      BrotherScanDataDecoder *decoder = nullptr;

      if (current_header.block_type == BROTHER_DATA_BLOCK_JPEG)
        {
          decoder = &jfif_decoder;
        }
      else if (current_header.block_type == BROTHER_DATA_BLOCK_GRAY_RLENGTH)
        {
          decoder = &gray_decoder;
        }
      else if (current_header.block_type == BROTHER_DATA_BLOCK_GRAY_RAW)
        {
          decoder = &gray_raw_decoder;
        }
      else if ((current_header.block_type == BROTHER_DATA_BLOCK_RED_RAW)
          || (current_header.block_type == BROTHER_DATA_BLOCK_GREEN_RAW)
          || (current_header.block_type == BROTHER_DATA_BLOCK_BLUE_RAW))
        {
          decoder = &colour_decoder;
        }
      else
        {
          FDBG (DBG_IMPORTANT,
               "unknown block encountered: 0x%2.2x",
               (unsigned int) current_header.block_type);
          ret_status = DECODE_STATUS_ERROR;
          break;
        }
// TODO test experimental switch-case version:
/*
      switch (current_header.block_type)
      {
        case BROTHER_DATA_BLOCK_JPEG:
          decoder = &jfif_decoder;
          break;
        case BROTHER_DATA_BLOCK_GRAY_RLENGTH:
          decoder = &gray_decoder;
          break;
        case BROTHER_DATA_BLOCK_GRAY_RAW:
          decoder = &gray_raw_decoder;
          break;
        case BROTHER_DATA_BLOCK_RED_RAW:
        case BROTHER_DATA_BLOCK_GREEN_RAW:
        case BROTHER_DATA_BLOCK_BLUE_RAW:
          decoder = &colour_decoder;
      }
      if (decoder == nullptr)
      {
          FDBG (DBG_IMPORTANT,
               "unknown block encountered: 0x%2.2x",
               (unsigned int) current_header.block_type);
          ret_status = DECODE_STATUS_ERROR;
          break;
      }*/

      /* Can be removed - none of the NewBlock() calls did anything */
      /* if (new_block)
        {
          decoder.NewBlock ();
        } */
      ret_status = decoder->DecodeScanData (src_data,
                                          in_len,
                                          &bytes_consumed,
                                          dest_data,
                                          dest_data_len,
                                          &bytes_written);

      FDBG (DBG_DETAIL, "written=%zu", bytes_written);

      if (ret_status != DECODE_STATUS_GOOD)
        {
          current_header.block_type = 0;
          break;
        }

      src_data += bytes_consumed;
      src_data_len -= bytes_consumed;
      current_header.block_len -= bytes_consumed;

      FDBG (DBG_DETAIL,
           "current block bytes remaining after decode=%zu",
           current_header.block_len);

      dest_data += bytes_written;
      dest_data_len -= bytes_written;

      /*
       * Perhaps we have completed the block.
       *
       */
      if (current_header.block_len == 0)
        {
          current_header.block_type = 0;
        }

      /*
       * Deal with special situation whereby we have input
       * and output buffer space, but we couldn't decode anything.
       * It might be that there is data in the input buffer
       * but we need more to proceed.
       *
       * If we haven't written anything, we cannot have made any progress.
       *
       */
      if (bytes_written == 0)
        {
          break;
        }
    }

  *src_data_consumed = orig_src_data_len - src_data_len;
  *dest_data_written = orig_dest_data_len - dest_data_len;

  return ret_status;
}


/*
 * EndOfFrame (0x82) is 10 bytes long because it doesn't have the
 * 2-byte packet length that the others do, so we could assume that
 * the packet length is not part of the header really.
 * However, it is convenient in coding terms to assume that it does.
 *
 * 0x80 - single byte end of data.
 * 0x82 - end of frame, with 10 bytes
 * Others - 12 bytes, including little endian length at the end.
 *
 * This implementation covers the parts that are identical in the families 2-4.
 * The subclasses will override and call this method.
 */
DecodeStatus BrotherEncoderFamily234Base::DecodeScanDataHeader (const SANE_Byte *src_data,
                                                          size_t src_data_len,
                                                          size_t *src_data_consumed,
                                                          ScanDataHeader &header)
{
  if (src_data_len == 0)
    {
      return DECODE_STATUS_TRUNCATED;
    }

  header.block_type = src_data[0];
  header.block_len = 0;
  header.image_number = 0;

  if (header.block_type == BROTHER_DATA_BLOCK_NO_DOCS)
    {
      *src_data_consumed = 1;
      return DECODE_STATUS_NO_DOCS;
    }

  if (header.block_type == BROTHER_DATA_BLOCK_SCAN_FINISHED)
    {
      *src_data_consumed = 1;
      return DECODE_STATUS_ENDOFDATA;
    }

  /* These seem to appear on family2 and family3 devices only */
  if (header.block_type == BROTHER_DATA_BLOCK_END_OF_FRAME_MORE)
    {
      *src_data_consumed = 1;
      return DECODE_STATUS_ENDOFFRAME_WITH_MORE;
    }

  if (header.block_type == BROTHER_DATA_BLOCK_CANCEL)
    {
      *src_data_consumed = 1;
      return DECODE_STATUS_CANCEL;
    }

  if ((header.block_type == BROTHER_DATA_BLOCK_PAPER_JAM)
      || (header.block_type == BROTHER_DATA_BLOCK_CLEARING_PAPER))
    {
      *src_data_consumed = 1;
      return DECODE_STATUS_PAPER_JAM;
    }

  if ((header.block_type == BROTHER_DATA_BLOCK_DEVICE_STUCK))
    {
      *src_data_consumed = 1;
      FDBGNP(DBG_IMPORTANT, "The scanner is stuck and must be turned off and back on.");
      return DECODE_STATUS_ERROR;
    }

  if (header.block_type == BROTHER_DATA_BLOCK_COVER_OPEN)
    {
      *src_data_consumed = 1;
      return DECODE_STATUS_COVER_OPEN;
    }

  /* We use this status to indicate that the parsing is incomplete */
  return DECODE_STATUS_GOOD;
}


DecodeStatus BrotherEncoderFamily2::DecodeScanDataHeader (const SANE_Byte *src_data,
                                                          size_t src_data_len,
                                                          size_t *src_data_consumed,
                                                          ScanDataHeader &header)
{
  DecodeStatus status_ret = BrotherEncoderFamily234Base::DecodeScanDataHeader(src_data, src_data_len, src_data_consumed, header);
  if (status_ret != DECODE_STATUS_GOOD)
  {
    return status_ret;
  }

  /* TODO: Is this the correct treatment for families 2 and 3, or should we
   * treat them like family 4? See the comment on Family4::DecodeScanDataHeader() */

  if (header.block_type == BROTHER_DATA_BLOCK_END_OF_FRAME_NO_MORE)
    {
      if (src_data_len < 10)
        {
          return DECODE_STATUS_TRUNCATED;
        }

      *src_data_consumed = 10;
      return DECODE_STATUS_ENDOFFRAME_NO_MORE;
    }

  /*
   * Other data-carrying packets.
   *
   */
  if (src_data_len < 3)
    {
      return DECODE_STATUS_TRUNCATED;
    }

  *src_data_consumed = 3;

  header.block_len = src_data[1] + (src_data[2] << 8);

  return DECODE_STATUS_GOOD;
}


DecodeStatus BrotherEncoderFamily4::DecodeScanDataHeader (const SANE_Byte *src_data,
                                                          size_t src_data_len,
                                                          size_t *src_data_consumed,
                                                          ScanDataHeader &header)
{
  DecodeStatus status_ret = BrotherEncoderFamily234Base::DecodeScanDataHeader(src_data, src_data_len, src_data_consumed, header);
  if (status_ret != DECODE_STATUS_GOOD)
  {
    return status_ret;
  }

  if (header.block_type == BROTHER_DATA_BLOCK_END_OF_FRAME_NO_MORE)
    {
      /* Family4 devices never return END_OF_FRAME_MORE. Therefore, 
       * the "end of frame" is unspecific and could be either of
       * ENDOFFRAME_NO_MORE or ENDOFFRAME_WITH_MORE. To distinguish the cases
       * we peek ahead to the first byte of the next packet. If it is a
       * SCAN_FINISHED, we won't get any additional frames. In any other case
       * it marks the beginning of a new frame.
       * 
       * Therefore, even though the packet is 10 bytes long, we return
       * TRUNCATED if we don't have the first byte of the next packet yet.
       */
      if (src_data_len < 10 + 1)
        {
          return DECODE_STATUS_TRUNCATED;
        }

      *src_data_consumed = 10;
      return (src_data[10] == BROTHER_DATA_BLOCK_SCAN_FINISHED) ?
        DECODE_STATUS_ENDOFFRAME_NO_MORE :
        DECODE_STATUS_ENDOFFRAME_WITH_MORE;
    }

  /*
   * Other data-carrying packets.
   *
   */
  if (src_data_len < 12)
    {
      return DECODE_STATUS_TRUNCATED;
    }

  *src_data_consumed = 12;

  header.block_len = src_data[10] + (src_data[11] << 8);
  header.image_number = src_data[3] + (src_data[4] << 8);

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoderFamily4::EncodeParameterBlockBlank (SANE_Byte *data, size_t data_len,
                                                               size_t *length)
{
  /* Sending blank X commands after the first page is unnecessary on family4 and slows down the scan */
  (void)data;
  (void)data_len;
  *length = 0;
  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoderFamily234Base::DecodeButtonQueryResp (const SANE_Byte *data, size_t data_len,
                                                          BrotherButtonQueryResponse &response)
{
  /* family4 sometimes begins with a 0x05 */
  if ((data_len != 4) || (data[0] != 0x05 && data[0] != 0x04) ||(data[1] != 0x10) || (data[2] != 0x03))
    {
      return DECODE_STATUS_ERROR;
    }

  response.has_button_press = data[3] == 0x10;

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoderFamily234Base::DecodeButtonStateResp (const SANE_Byte *data, size_t data_len,
                                                           BrotherButtonStateResponse &response)
{
  if (data_len != 9)
    {
      return DECODE_STATUS_ERROR;
    }
  BrotherButtonQueryResponse dummy;
  if (DecodeButtonQueryResp(data, 4, dummy) == DECODE_STATUS_GOOD)
    {
      /* We may get "no button pressed" responses while waiting for the second
       * packet containing info on which button was pressed. We encode these
       * intermediate packets by DECODE_STATUS_INVAL. */
      return DECODE_STATUS_INVAL;
    }
  if (memcmp(data, "\x09" "\x10" "\x03" "\x20", 4) != 0)
    {
      return DECODE_STATUS_ERROR;
    }

  response.button_value = 0;

  switch (data[4])
  {
    case 0x05:
      response.button_value = BROTHER_SENSOR_FILE;
      break;

    case 0x02:
      response.button_value = BROTHER_SENSOR_OCR;
      break;

    case 0x03:
      response.button_value = BROTHER_SENSOR_IMAGE;
      break;

    case 0x08:
      response.button_value = BROTHER_SENSOR_EMAIL;
      break;

    case 0x00:
      break;

    default:
      FDBG (DBG_WARN, "unknown button code: %d.", data[4]);
      return DECODE_STATUS_UNSUPPORTED;
  }

  return DECODE_STATUS_GOOD;
}


/*
 * --------------------------------------------
 * Family 5 Encoding
 * --------------------------------------------
 *
 */

const char* BrotherEncoderFamily5::ScanModeToText (BrotherScanMode scan_mode)
{
  static const char *scan_mode_text[] =
    { "C24BIT", "GRAY256", "ERRDIF", "TEXT" };  // also C256?

  return scan_mode_text[scan_mode];
}

DecodeStatus BrotherEncoderFamily5::EncodeQueryBlock (SANE_Byte *data, size_t data_len,
                                                      size_t *length)
{
  *length = snprintf ((char*) data, data_len, "\x1b" "\x51" "\x0a" "\x80");

  if (*length > data_len)
    {
      FDBG (DBG_SERIOUS,
           "query block too long for buffer: %zu",
           *length);
      return DECODE_STATUS_INVAL;
    }

  return DECODE_STATUS_GOOD;

}

DecodeStatus BrotherEncoderFamily5::DecodeQueryBlockResp (const SANE_Byte *data, size_t data_len,
                                                          BrotherQueryResponse &response)
{

  /*
   * Formatting and content checks.
   *
   */
  if ((data_len != 75) || (data[0] != BROTHER_DATA_QUERY_RESPONSE))

    {
      FDBG (DBG_SERIOUS,
           "invalid query response block: len = %zu",
           data_len);

      return DECODE_STATUS_ERROR;
    }

  /*
   * TODO: decode the response block to fill in the details.
   *
   */
  response.dummy = 0;

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoderFamily5::DecodeSessionResp (const SANE_Byte *data, size_t data_len,
                                                       BrotherSessionResponse &response)
{
  /*
   * Formatting and content checks.
   *
   *
   */
  if ((data_len != 5)
      || ((memcmp (data, "\x05" "\x10" "\x01" "\x02", 4) != 0)
          && (memcmp (data, "\x05" "\x10" "\x02" "\x02", 4) != 0)))

    {
      FDBG (DBG_SERIOUS,
           "invalid session response block: len = %zu",
           data_len);

      return DECODE_STATUS_ERROR;
    }

  /*
   * TODO: Check the valid status values are 0x00 or 0x20
   *
   */
  if ((data[4] != 0x00) && (data[4] != 0x80))

    {
      FDBG (DBG_SERIOUS,
           "invalid session response: data[4]=%u",
           (unsigned int) data[4]);

      return DECODE_STATUS_ERROR;
    }

  /*
   * TODO: figure out what the rest of the packet is supposed to be.
   *
   */
  response.ready = data[4] == 0x00? SANE_TRUE: SANE_FALSE;

  return DECODE_STATUS_GOOD;
}

/*
 * NOTE:
 * Supply a buffer that is at least 1 char longer than is necessary
 * as snprintf() will require space for a terminating NUL.
 *
 */
DecodeStatus BrotherEncoderFamily5::EncodeBasicParameterBlock (SANE_Byte *data, size_t data_len,
                                                               size_t *length)
{
  const char *mode_text = ScanModeToText (scan_params.param_scan_mode);
  if (nullptr == mode_text)
    {
      FDBG (DBG_SERIOUS,
           "failed to get scan mode text for mode %d",
           scan_params.param_scan_mode);
      return DECODE_STATUS_INVAL;
    }

  *length = snprintf ((char*) data,
                      data_len,
                      "\x1b" "SSP\nOS=LNX\nPSRC=%s\nRESO=%u,%u\n"
                      "CLR=%s\nAREA=NORMAL\nMRGN=0,0,0,0\n"
                      "DPLX=%s\nBRIT=%u\nCONT=%u\nATCL=0,0\n"
                      "COMP=%s\nRATE=50\nJSF=420\n"
                      "IPRC=NORMAL\nPTYPE=NORMAL\nPAGE=0\n"
                      "LONG=OFF\nCARR=OFF\nRMGC=OFF\nDTDF=OFF\nDT4V=OFF\n"
                      "DSKW=OFF\nLSMD=OFF\nRMBP=OFF\nRMMR=OFF\nGMMA=OFF\n"
                      "TONE=ON\nQTFD=OFF\nATCN=OFF\nATCRP=OFF\nUSER=\n"
                      "\x80",
                      scan_params.param_source == BROTHER_SOURCE_AUTO ?
                          "AUTO" :
                          (scan_params.param_source == BROTHER_SOURCE_FLATBED ? "FB" : "ADF"),
                      (unsigned int) scan_params.param_x_res,
                      (unsigned int) scan_params.param_y_res,
                      mode_text,
                      scan_params.param_source == BROTHER_SOURCE_ADF_DUPLEX ? "ON" : "OFF",
                      (unsigned int) (scan_params.param_brightness + 50),
                      (unsigned int) (scan_params.param_contrast + 50),
                      (scan_params.param_scan_mode == BROTHER_SCAN_MODE_COLOR)
                          || (scan_params.param_scan_mode == BROTHER_SCAN_MODE_GRAY) ?
                          "JPEG" : "RLENGTH");

  if (*length > data_len)
    {
      FDBG (DBG_SERIOUS,
           "parameter block too long for buffer: %zu",
           *length);
      return DECODE_STATUS_INVAL;
    }

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoderFamily5::DecodeBasicParameterBlockResp (const SANE_Byte *data, size_t data_len,
                                                                   BrotherBasicParamResponse &response)
{
  /*
   * TODO: Decode this block.
   * Might contain some useful stuff, like limits for selected parameters.
   *
   */
  (void)data;
  (void)data_len;
  (void)response;

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoderFamily5::EncodeSourceStatusBlock (SANE_Byte *data, size_t data_len, size_t *length)
{
  (void)data;
  (void)data_len;
  (void)length;

//  *length = snprintf ((char*) data, data_len, "\x1b" "D\nADF\n" "\x80");
//
//  if (*length > data_len)
//    {
//      DBG (DBG_SERIOUS,
//           "BrotherEncoderFamily5::EncodeSourceStatusBlock: Source status block too long for buffer: %zu",
//           *length);
//      return DECODE_STATUS_INVAL;
//    }
//
//  return DECODE_STATUS_GOOD;

  return DECODE_STATUS_UNSUPPORTED;
}

DecodeStatus BrotherEncoderFamily5::DecodeSourceStatusBlockResp (const SANE_Byte *data,
                                                                 size_t data_len,
                                                                 BrotherSourceStatusResponse &response)
{
  (void)data;
  (void)data_len;
  (void)response;

//  if (data_len != 1)
//    {
//      DBG (DBG_SERIOUS,
//           "BrotherEncoderFamily2::DecodeSourceStatusBlockResp: source status response invalid: %zu",
//           data_len);
//      return DECODE_STATUS_ERROR;
//    }
//
//  /*
//   * We can get error condition codes from this block.
//   *
//   */
//  switch (data[0])
//  {
//    case BROTHER_DATA_BLOCK_PAPER_JAM:
//      return DECODE_STATUS_PAPER_JAM;
//
//    case BROTHER_DATA_BLOCK_COVER_OPEN:
//      return DECODE_STATUS_COVER_OPEN;
//
//    case BROTHER_DATA_BLOCK_NO_DOCS:
//      response.source_ready = SANE_FALSE;
//      return DECODE_STATUS_GOOD;
//
//    case BROTHER_DATA_BLOCK_SCAN_FINISHED:
//      response.source_ready = SANE_TRUE;
//      return DECODE_STATUS_GOOD;
//
//    default:
//      return DECODE_STATUS_ERROR;
//  }

  return DECODE_STATUS_UNSUPPORTED;
}

DecodeStatus BrotherEncoderFamily5::EncodeSourceSelectBlock (SANE_Byte *data, size_t data_len, size_t *length)
{
  (void)data;
  (void)data_len;
  (void)length;

//  *length = snprintf ((char*) data,
//                      data_len,
//                      "\x1b" "S\n%s\n" "\x80",
//                      scan_params.param_source == BROTHER_SOURCE_AUTO ?
//                          "AUTO" :
//                          (scan_params.param_source == BROTHER_SOURCE_FLATBED ? "FB" : "ADF"));
//
//  if (*length > data_len)
//    {
//      DBG (DBG_SERIOUS,
//           "BrotherEncoderFamily5::EncodeSourceSelectBlock: Source select block too long for buffer: %zu",
//           *length);
//      return DECODE_STATUS_INVAL;
//    }
//
//  return DECODE_STATUS_GOOD;

  return DECODE_STATUS_UNSUPPORTED;
}

DecodeStatus BrotherEncoderFamily5::DecodeSourceSelectBlockResp (const SANE_Byte *data,
                                                                 size_t data_len,
                                                                 BrotherSourceStatusResponse &response)
{
  (void)data;
  (void)data_len;
  (void)response;

//  if (data_len != 1)
//    {
//      DBG (DBG_SERIOUS,
//           "BrotherEncoderFamily2::DecodeSourceStatusBlockResp: source select response invalid: %zu",
//           data_len);
//      return DECODE_STATUS_ERROR;
//    }
//
//  /*
//   * We can get error condition codes from this block.
//   *
//   */
//  switch (data[0])
//  {
//    case BROTHER_DATA_BLOCK_SCAN_FINISHED:
//      response.source_ready = SANE_TRUE;
//      return DECODE_STATUS_GOOD;
//
//    case BROTHER_DATA_BLOCK_PROTOCOL_ERROR:
//      response.source_ready = SANE_FALSE;
//      return DECODE_STATUS_INVAL;
//
//    default:
//      return DECODE_STATUS_ERROR;
//  }

  return DECODE_STATUS_UNSUPPORTED;
}

DecodeStatus BrotherEncoderFamily5::EncodeParameterBlockBlank (SANE_Byte *data, size_t data_len,
                                                               size_t *length)
{
  const char *mode_text = ScanModeToText (scan_params.param_scan_mode);
  if (nullptr == mode_text)
    {
      FDBG (DBG_SERIOUS,
           "failed to get scan mode text for mode %d",
           scan_params.param_scan_mode);
      return DECODE_STATUS_INVAL;
    }

  *length = snprintf ((char*) data, data_len, "\x1b" "X\n\x80");

  if (*length > data_len)
    {
      FDBG (DBG_SERIOUS,
           "parameter block too long for buffer: %zu",
           *length);
      return DECODE_STATUS_INVAL;
    }

  return DECODE_STATUS_GOOD;
}


DecodeStatus BrotherEncoderFamily5::EncodeParameterBlock (SANE_Byte *data, size_t data_len,
                                                         size_t *length)
{
  const char *mode_text = ScanModeToText (scan_params.param_scan_mode);
  if (nullptr == mode_text)
    {
      FDBG (DBG_SERIOUS,
           "failed to get scan mode text for mode %d",
           scan_params.param_scan_mode);
      return DECODE_STATUS_INVAL;
    }

  *length = snprintf ((char*) data,
                      data_len,
                      "\x1b" "XSC\nRESO=%u,%u\n"
                      "AREA=%u,%u,%u,%u\n"
                      "MODE=NORMAL\n"
                      "\x80",
                      (unsigned int) scan_params.param_x_res,
                      (unsigned int) scan_params.param_y_res,
                      (unsigned int) (scan_params.param_pixel_x_offset),
                      (unsigned int) (scan_params.param_pixel_y_offset),
                      (unsigned int) (scan_params.param_pixel_x_offset
                          + scan_params.param_pixel_x_width),
                      (unsigned int) (scan_params.param_pixel_y_offset
                          + scan_params.param_pixel_y_height));

  if (*length > data_len)
    {
      FDBG (DBG_SERIOUS,
           "parameter block too long for buffer: %zu",
           *length);
      return DECODE_STATUS_INVAL;
    }

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoderFamily5::DecodeScanData (const SANE_Byte *src_data, size_t src_data_len,
                                                    size_t *src_data_consumed, SANE_Byte *dest_data,
                                                    size_t dest_data_len, size_t *dest_data_written)
{
  FDBGNP (DBG_EVENT, "");
  DecodeStatus ret_status = DECODE_STATUS_GOOD;

  *src_data_consumed = 0;
  *dest_data_written = 0;

  /*
   * We will use this to compute the bytes consumed at the end.
   *
   */
  size_t orig_src_data_len = src_data_len;
  size_t orig_dest_data_len = dest_data_len;

  /*
   * Now take note here that we do not *necessarily* need src_data to be able to decode.
   * We might have terminated the previous call not because we ran out of input, but because
   * we ran out of output space. Some blocks are just repetitions that are unrolled and require
   * no additional src data. So we enter this loop *even if there is nothing in the src_data buffer*.
   *
   * All of the functions that we are about to call must be able to deal with src_data_len == 0
   * intelligently.
   *
   */
  for (;;)
    {
      /*
       * If we are not in a block, then decode the next block.
       *
       * We might not have enough bytes to fully decode the header.
       * In which case, we wait for some more data.
       *
       */
      bool new_block = false;

      if (current_header.block_type == 0)
        {
          size_t header_consumed = 0;
          ret_status = DecodeScanDataHeader(src_data, src_data_len, &header_consumed, current_header);
          if (ret_status == DECODE_STATUS_TRUNCATED)
            {
              /*
               * This means we don't have enough data to decode a header yet.
               * Try again next time if we have read more data.
               *
               */
              ret_status = DECODE_STATUS_GOOD;
              break;
            }

          if (ret_status == DECODE_STATUS_CANCEL)
            {
              FDBGNP (DBG_IMPORTANT, "cancel detected");
              break;
            }

          /*
            * Detect special case situations.
            *
            */
          if (ret_status != DECODE_STATUS_GOOD)
            {
              FDBG (DBG_WARN,
                   "%s.",
                   DecodeStatusToString (ret_status));

              current_header.block_type = 0;

              /*
               * If we have data in hand, then we will send this up first.
               * Otherwise, send SANE_STATUS_EOF to trigger stop of scan.
               *
               * We don't consume this header so we will see it the next time around.
               *
               */
              if (orig_dest_data_len > dest_data_len)
                {
                  ret_status = DECODE_STATUS_GOOD;
                }
              else
                {
                  // Consume the header.
                  src_data_len -= header_consumed;
                }
              break;
            }
          if (ret_status != DECODE_STATUS_GOOD)
            {
              FDBGNP (DBG_IMPORTANT, "failed to decode header");
              current_header.block_type = 0;
              break;
            }

          FDBG (DBG_IMPORTANT,
               "decoded new block header: 0x%2.2x, length=%zu",
               current_header.block_type,
               current_header.block_len);

          src_data += header_consumed;
          src_data_len -= header_consumed;

          new_block = true;
        }

      /*
       * Attempt to decode the data block.
       * We will exit the function if we can only partially
       * decode it, hoping to do more next time.
       *
       */
      size_t bytes_consumed = 0;
      size_t bytes_written = 0;
      size_t in_len = std::min(src_data_len, current_header.block_len);

      if (current_header.block_type == BROTHER_DATA_BLOCK_JPEG5)
        {
          if (new_block)
            {
              jfif_decoder.NewBlock ();
            }

          ret_status = jfif_decoder.DecodeScanData (src_data,
                                                    in_len,
                                                    &bytes_consumed,
                                                    dest_data,
                                                    dest_data_len,
                                                    &bytes_written);
        }
      else if (current_header.block_type == BROTHER_DATA_BLOCK_RLENGTH5)
        {
          if (new_block)
            {
              gray_decoder.NewBlock ();
            }

          ret_status = gray_decoder.DecodeScanData (src_data,
                                                    in_len,
                                                    &bytes_consumed,
                                                    dest_data,
                                                    dest_data_len,
                                                    &bytes_written);
        }
      else
        {
          FDBG (DBG_IMPORTANT,
               "unknown block encountered: 0x%2.2x",
               (unsigned int) current_header.block_type);
          ret_status = DECODE_STATUS_ERROR;
          break;
        }

      if (ret_status != DECODE_STATUS_GOOD)
        {
          current_header.block_type = 0;
          break;
        }

      src_data += bytes_consumed;
      src_data_len -= bytes_consumed;
      current_header.block_len -= bytes_consumed;

      FDBG (DBG_DETAIL,
           "current block bytes remaining after decode=%zu",
           current_header.block_len);

      dest_data += bytes_written;
      dest_data_len -= bytes_written;

      /*
       * Perhaps we have completed the block.
       *
       */
      if (current_header.block_len == 0)
        {
          current_header.block_type = 0;
        }

      /*
       * Deal with special situation whereby we have input
       * and output buffer space, but we couldn't decode anything.
       * It might be that there is data in the input buffer
       * but we need more to proceed.
       *
       * If we haven't written anything, we cannot have made any progress.
       *
       */
      if (bytes_written == 0)
        {
          break;
        }
    }

  *src_data_consumed = orig_src_data_len - src_data_len;
  *dest_data_written = orig_dest_data_len - dest_data_len;

  return ret_status;
}


/*
 * EndOfFrame (0x82) is 10 bytes long because it doesn't have the
 * 2-byte packet length that the others do, so we could assume that
 * the packet length is not part of the header really.
 * However, it is convenient in coding terms to assume that it does.
 *
 * Single error condition code, or
 * Others - 14 byte data header.
 *
 */
DecodeStatus BrotherEncoderFamily5::DecodeScanDataHeader (const SANE_Byte *src_data,
                                                          size_t src_data_len,
                                                          size_t *src_data_consumed,
                                                          ScanDataHeader &header)
{
  if (src_data_len == 0)
    {
      return DECODE_STATUS_TRUNCATED;
    }

  header.block_type = src_data[1];
  header.block_len = 0;
  header.image_number = 0;

//  if (header.block_type == BROTHER_DATA_BLOCK_NO_DOCS)
//    {
//      *src_data_consumed = 1;
//      return DECODE_STATUS_NO_DOCS;
//    }
//
//  if (header.block_type == BROTHER_DATA_BLOCK_SCAN_FINISHED)
//    {
//      *src_data_consumed = 1;
//      return DECODE_STATUS_ENDOFDATA;
//    }
//
//  if (header.block_type == BROTHER_DATA_BLOCK_CANCEL)
//    {
//      *src_data_consumed = 1;
//      return DECODE_STATUS_CANCEL;
//    }
//
//  if ((header.block_type == BROTHER_DATA_BLOCK_PAPER_JAM)
//      || (header.block_type == BROTHER_DATA_BLOCK_CLEARING_PAPER))
//    {
//      *src_data_consumed = 1;
//      return DECODE_STATUS_PAPER_JAM;
//    }
//
//  if (header.block_type == BROTHER_DATA_BLOCK_COVER_OPEN)
//    {
//      *src_data_consumed = 1;
//      return DECODE_STATUS_COVER_OPEN;
//    }

  if (header.block_type == BROTHER_DATA_BLOCK_END_OF_FRAME_NO_MORE5)
    {
      if (src_data_len < 4)
        {
          return DECODE_STATUS_TRUNCATED;
        }

      *src_data_consumed = 4;
      return DECODE_STATUS_ENDOFFRAME_NO_MORE;
    }

  /*
   * Other data-carrying packets.
   *
   */
  if (src_data_len < 14)
    {
      return DECODE_STATUS_TRUNCATED;
    }

  *src_data_consumed = 14;

  header.block_len = src_data[6] + (src_data[7] << 8);
  header.image_number = src_data[2] + (src_data[3] << 8);

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoderFamily5::DecodeButtonQueryResp (const SANE_Byte *data, size_t data_len,
                                                           BrotherButtonQueryResponse &response)
{
  if ((data_len != 4) || (data[0] != 0x04) ||(data[1] != 0x10) || (data[2] != 0x03))
    {
      return DECODE_STATUS_ERROR;
    }

  response.has_button_press = data[3] == 0x10;

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherEncoderFamily5::DecodeButtonStateResp (const SANE_Byte *data, size_t data_len,
                                                           BrotherButtonStateResponse &response)
{
  if ((data_len != 9) || (memcmp(data, "\x09" "\x10" "\x03" "\x20", 4) != 0))
    {
      return DECODE_STATUS_ERROR;
    }

  response.button_value = 0;

  switch (data[4])
  {
    case 0x05:
      response.button_value = BROTHER_SENSOR_FILE;
      break;

    case 0x02:
      response.button_value = BROTHER_SENSOR_OCR;
      break;

    case 0x03:
      response.button_value = BROTHER_SENSOR_IMAGE;
      break;

    case 0x08:
      response.button_value = BROTHER_SENSOR_EMAIL;
      break;

    case 0x00:
      break;

    default:
      FDBG (DBG_WARN, "unknown button code: %d.", data[4]);
      return DECODE_STATUS_UNSUPPORTED;
  }

  return DECODE_STATUS_GOOD;
}

void BrotherGrayRLengthDecoder::NewBlock ()
{
}

void BrotherGrayRLengthDecoder::NewPage(const BrotherParameters &params)
{
  decode_params = params;
  NewBlock();
}

/*
 * Returns:
 *
 * DECODE_STATUS_GOOD - block all decoded.
 * DECODE_STATUS_ERROR - error detected in data.
 *
 */
DecodeStatus BrotherGrayRLengthDecoder::DecodeScanData (const SANE_Byte *in_buffer,
                                                        size_t in_buffer_len,
                                                        size_t *bytes_consumed,
                                                        SANE_Byte *out_buffer,
                                                        size_t out_buffer_len,
                                                        size_t *bytes_written)
{
  *bytes_consumed = 0;
  *bytes_written = 0;

  size_t in_buffer_len_start = in_buffer_len;
  size_t out_buffer_len_start = out_buffer_len;

  /*
   * Notice that we do the check for in and out space at the bottom, not here:
   * We might not have any additional src, but we might still be able to progress the decode,
   * especially in the case of an in-progress decompression.
   *
   */
  do
    {
      /*
       * We need src to be able to decode.
       *
       */
      if (in_buffer_len == 0)
        {
          break;
        }

      /*
       * INIT state: we are ready to start to process another sub-block.
       *
       * If there is data then we should see a block length.
       *
       * If byte & 0x80 then it is a compressed byte block.
       * Else it is a sequence of uncompressed bytes.
       *
       */
      size_t consumed_input = 0;
      size_t written_output = 0;

      if (in_buffer[0] & 0x80)
        {
          /*
           * To process the expansion miniblock, we have to see the next byte.
           * We might not have it yet. We will just have to wait for more bytes to come in.
           *
           */
          if (in_buffer_len < 2)
            {
              FDBGNP (DBG_IMPORTANT, "in_buffer_len < 2: can't decode yet.");
              break;
            }

          /*
           * Check that we have enough output space for the expanded mini block.
           *
           */
          size_t output_req = 0xff - in_buffer[0] + 2;
          if (output_req > out_buffer_len)
            {
              break;
            }

          SANE_Byte decode_expand_char = in_buffer[1];

          in_buffer += 2;
          in_buffer_len -= 2;

          (void) memset (out_buffer, decode_expand_char, output_req);
          written_output = output_req;

          FDBG (DBG_IMPORTANT,
               "expand block: %zu bytes of 0x%2.2x",
               output_req,
               (int) decode_expand_char);
        }
      else
        {

          /*
           * Check that we have enough space for the expanded mini block
           * and that we have the entire miniblock in the src buffer.
           *
           */
          size_t output_req = in_buffer[0] + 1;
          if ((output_req > out_buffer_len) || (in_buffer_len < output_req + 1))
            {
              break;
            }

          in_buffer += 1;
          in_buffer_len -= 1;

          (void) memcpy (out_buffer, in_buffer, output_req);
          consumed_input = output_req;
          written_output = output_req;

          FDBG (DBG_IMPORTANT,
               "bytes block: %zu bytes",
               output_req);
        }

      /*
       * Adjust for the consumed input and written output.
       *
       */
      in_buffer += consumed_input;
      in_buffer_len -= consumed_input;
      out_buffer += written_output;
      out_buffer_len -= written_output;

    } while (in_buffer_len && out_buffer_len);

  if (!in_buffer_len || !out_buffer_len)
    {
      FDBG (DBG_DETAIL,
           "ran out of buffer: in %zu out %zu",
           in_buffer_len,
           out_buffer_len);

    }
  *bytes_consumed = in_buffer_len_start - in_buffer_len;
  *bytes_written = out_buffer_len_start - out_buffer_len;

  return DECODE_STATUS_GOOD;
}

void BrotherInterleavedRGBColourDecoder::NewBlock ()
{
}

void BrotherInterleavedRGBColourDecoder::NewPage(const BrotherParameters &params)
{
  decode_params = params;
  NewBlock();

  /*
   * Signal that we need to reallocate the scanline_buffer.
   * We defer the allocation because this function returns void
   * so we cannot signal an error if there is a problem allocating memory.
   *
   */
  scanline_length = 0;
  scanline_buffer_size = 0;
}

void BrotherInterleavedRGBColourDecoder::ConvertYCbCrToRGB (SANE_Byte y, SANE_Byte cb, SANE_Byte cr,
                                                            SANE_Byte *red, SANE_Byte *green,
                                                            SANE_Byte *blue)
{
  double y_double = (double) y;
  double cb_double = (double) cb;
  double cr_double = (double) cr;

  double red_chan = y_double + 1.40200 * (cr_double - 0x80);
  double green_chan = y_double - 0.34414 * (cb_double - 0x80) - 0.71414 * (cr_double - 0x80);
  double blue_chan = y_double + 1.77200 * (cb_double - 0x80);

  *red = std::max (0.0, std::min (255.0, red_chan));
  *green = std::max (0.0, std::min (255.0, green_chan));
  *blue = std::max (0.0, std::min (255.0, blue_chan));
}

/*
 * Returns:
 *
 * DECODE_STATUS_GOOD - block all decoded.
 * DECODE_STATUS_ERROR - error detected in data.
 *
 * TODO: This function is assuming 24bit colour. I don't know
 * if that is always valid.
 *
 */
DecodeStatus BrotherInterleavedRGBColourDecoder::DecodeScanData (const SANE_Byte *in_buffer,
                                                                 size_t in_buffer_len,
                                                                 size_t *bytes_consumed,
                                                                 SANE_Byte *out_buffer,
                                                                 size_t out_buffer_len,
                                                                 size_t *bytes_written)
{
  *bytes_consumed = 0;
  *bytes_written = 0;

  size_t in_buffer_len_start = in_buffer_len;
  size_t out_buffer_len_start = out_buffer_len;

  if (scanline_length == 0)
    {
      /*
       * Make sure that we have an allocated buffer big enough for
       * a single output scanline that we will assemble the scanline into.
       *
       */
      scanline_length = decode_params.param_pixel_x_width * 3;

      if (scanline_length > scanline_buffer_size)
        {
          SANE_Byte *new_buffer = (SANE_Byte *)realloc (scanline_buffer, scanline_length);
          if (new_buffer == nullptr)
            {
              scanline_length = 0;
              return DECODE_STATUS_MEMORY;
            }
          scanline_buffer = new_buffer;
          scanline_buffer_size = scanline_length;
        }

      /*
       * Reset our decoding.
       *
       */
      scanline_buffer_data = 0;
    }

  /*
   * Assemble src data into the scanline buffer.
   *
   */
  while (in_buffer_len && (out_buffer_len > scanline_length))
    {
      size_t bytes_avail = std::min(in_buffer_len, scanline_length - scanline_buffer_data);
      (void)memcpy(scanline_buffer + scanline_buffer_data, in_buffer, bytes_avail);

      scanline_buffer_data += bytes_avail;
      in_buffer += bytes_avail;
      in_buffer_len -= bytes_avail;

      /*
       * See if we have a full scanline now.
       *
       */
      if (scanline_buffer_data >= scanline_length)
        {
          size_t stride = decode_params.param_pixel_x_width;

          SANE_Byte *red = scanline_buffer;
          SANE_Byte *green = scanline_buffer + stride;
          SANE_Byte *blue = scanline_buffer + (2 * stride);

          for (size_t col_index = 0; col_index < (size_t) decode_params.param_pixel_x_width;
              col_index++)
            {
              /*
               * Sort out the raw encoding format.
               *
               */
              if (capabilities & CAP_ENCODING_RAW_IS_CrYCb)
                {
                  SANE_Byte R;
                  SANE_Byte G;
                  SANE_Byte B;

                  ConvertYCbCrToRGB(*green++, *blue++, *red++, &R, &G, &B);

                  *out_buffer++ = R;
                  *out_buffer++ = G;
                  *out_buffer++ = B;
                }
              else
                {
                  /*
                   * Must be just plain RBG.
                   *
                   */
                  *out_buffer++ = *red++;
                  *out_buffer++ = *green++;
                  *out_buffer++ = *blue++;
                }
            }
          scanline_buffer_data = 0;
          out_buffer_len -= scanline_length;
        }
    }

  *bytes_consumed = in_buffer_len_start - in_buffer_len;
  *bytes_written = out_buffer_len_start - out_buffer_len;

  return DECODE_STATUS_GOOD;
}

#include <jpeglib.h>
#include <jerror.h>

void BrotherJFIFDecoder::NewBlock()
{
  // Nothing to do.
}

void BrotherJFIFDecoder::NewPage (const BrotherParameters &params)
{
  decode_params = params;

  /*
   * Aborting a non-running state should be OK.
   *
   */
  jpeg_abort_decompress(&state.cinfo);
  state = {};
  decompress_bytes = 0;

  // Re-initialize the library for a new image
  InitLibJpeg();
}

void BrotherJFIFDecoder::InitSource (j_decompress_ptr cinfo)
{
  FDBGNP (DBG_EVENT, "");

  (void)cinfo;

  /*
   * We will have already setup all the available data.
   *
   */
  return;
}

boolean BrotherJFIFDecoder::FillInputBuffer(j_decompress_ptr cinfo)
{
  (void)cinfo;

  /*
   * We can not supply more data.
   * We setup the data buffer already with everything we have.
   *
   */
  FDBGNP (DBG_EVENT, "");
  return FALSE;
}

void BrotherJFIFDecoder::SkipInputData(j_decompress_ptr cinfo, long num_bytes)
{
  (void)cinfo;
  (void)num_bytes;

  /*
   * TODO: Note the special attention required here.
   * num_bytes might exceed the number of bytes in the src_data so we must remember
   * the excess so that we can discard it when we add more data later!!!!!!!
   *
   */
  FDBGNP (DBG_EVENT, "");
}

void BrotherJFIFDecoder::TermSource(j_decompress_ptr cinfo)
{
  (void)cinfo;
  FDBGNP (DBG_EVENT, "");
}

void BrotherJFIFDecoder::OutputMessageHandler(j_common_ptr cinfo)
{
  /* based on libjpeg-turbo/jerror.c */
  char jpegLastErrorMsg[JMSG_LENGTH_MAX];
  (*cinfo->err->format_message) (cinfo, jpegLastErrorMsg);
  FDBG (DBG_WARN, "libjpeg warning=[%s]", jpegLastErrorMsg);
}

void BrotherJFIFDecoder::ErrorExitManager(j_common_ptr cinfo)
{
  char jpegLastErrorMsg[JMSG_LENGTH_MAX];
  ( *(cinfo->err->format_message) ) (cinfo, jpegLastErrorMsg);
  FDBG (DBG_SERIOUS, "libjpeg error=[%s]", jpegLastErrorMsg);

  CompressionState *state = reinterpret_cast<CompressionState *>(cinfo);
  longjmp(state->my_env, 1);
}


DecodeStatus BrotherJFIFDecoder::DecodeScanData (const SANE_Byte *src_data, size_t src_data_len,
                                                 size_t *src_data_consumed, SANE_Byte *dest_data,
                                                 size_t dest_data_len, size_t *dest_data_written)
{
  FDBG (DBG_EVENT,
       "src_data_len=%zu dest_data_len=%zu",
       src_data_len,
       dest_data_len);

  *src_data_consumed = 0;
  *dest_data_written = 0;

  /*
   * Loop around, appending data to our decompress buffer and trying to
   * decompress as we go until we exhaust the output buffer or the input.
   *
   */
  while (src_data_len > 0 || decompress_bytes > 0)
    {
      /*
       * Append what we can to the decompress buffer.
       *
       */
      size_t bytes_to_copy = std::min(sizeof(decompress_buffer) - decompress_bytes, src_data_len);

      (void) memcpy (decompress_buffer + decompress_bytes, src_data, bytes_to_copy);
      decompress_bytes += bytes_to_copy;

      src_data += bytes_to_copy;
      src_data_len -= bytes_to_copy;
      *src_data_consumed += bytes_to_copy;

      /*
       * Now to try to decompress.
       *
       */
      size_t decompress_consumed = 0;
      size_t bytes_written = 0;
      DecodeStatus res = DecodeScanData_CompressBuffer (decompress_buffer,
                                                        decompress_bytes,
                                                        &decompress_consumed,
                                                        dest_data,
                                                        dest_data_len,
                                                        &bytes_written);
      if (res == DECODE_STATUS_ENDOFFRAME_WITH_MORE)
        {
          if (*dest_data_written > 0)
            {
              /* We have produced data, which must be returned
               * before sending EOF
               */
              res = DECODE_STATUS_GOOD;
            }
          return res;
        }

      if (res != DECODE_STATUS_GOOD)
        {
          *src_data_consumed = 0;
          *dest_data_written = 0;
          return res;
        }

      /*
       * Shift out any data that we consumed.
       *
       */
      (void) memmove (decompress_buffer,
                      decompress_buffer + decompress_consumed,
                      decompress_bytes - decompress_consumed);
      decompress_bytes -= decompress_consumed;

      if (bytes_written == 0)
        {
          break;
        }

      dest_data += bytes_written;
      dest_data_len -= bytes_written;
      *dest_data_written += bytes_written;
    }

  return DECODE_STATUS_GOOD;
}

DecodeStatus BrotherJFIFDecoder::DecodeScanData_CompressBuffer (const SANE_Byte *src_data,
                                                                size_t src_data_len,
                                                                size_t *src_data_consumed,
                                                                SANE_Byte *dest_data,
                                                                size_t dest_data_len,
                                                                size_t *dest_data_written)
{
  *src_data_consumed = 0;
  *dest_data_written = 0;
  if (src_data_len == 0)
  {
    // TODO: Is this desired behaviour? What is the scenario here?
    FDBGNP(DBG_DETAIL, "JFIF decoder was called on an empty buffer. Returning EOF");
    return DECODE_STATUS_ENDOFFRAME_WITH_MORE;
  }

  /*
   * Check if the decoder has finished already. While we should not normally get here, this check can catch some spurious errors.
   * For example, if a slightly invalid number of rows was requested, the scanner returns JFIF data whose header does not always
   * match the actual number of rows.
   */
  if (state.cinfo.output_height > 0 && state.cinfo.output_scanline >= state.cinfo.output_height)
  {
    FDBG (DBG_SERIOUS,
      "WARNING: JFIF decoder was called on a finished buffer (data left: %zu). This can happen when the scanner returns more data than announced",
      src_data_len);
    // Skip the remaining data on this page by absorbing it
    *src_data_consumed = src_data_len;
    // return STATUS_GOOD so we can process an EOF packet on the next turn
    return DECODE_STATUS_GOOD;
  }
  FDBG (DBG_EVENT,
       "src_data_len=%zu dest_data_len=%zu",
       src_data_len,
       dest_data_len);

  /*
   * Initialise read buffer.
   *
   */
  state.src_mgr.bytes_in_buffer = src_data_len;
  state.src_mgr.next_input_byte = src_data;

  if (setjmp(state.my_env) != 0)
    {
      FDBGNP (DBG_EVENT, "setjmp error return");
      return DECODE_STATUS_ERROR;
    }

  /*
   * Read header if we haven't yet.
   *
   */
  if (!state.have_read_header)
    {
      FDBGNP (DBG_EVENT, "Trying to read header.");

      int read_status = jpeg_read_header(&state.cinfo, TRUE);
      if (read_status == JPEG_SUSPENDED)
	{
	  return DECODE_STATUS_GOOD;
	}
      else if (read_status != JPEG_HEADER_OK)
	{
	  return DECODE_STATUS_ERROR;
	}
      state.have_read_header = true;

      /*
       * Set the required colour space for the image we expect.
       * For gray it is JCS_GRAYSCALE. For colour it is JCS_RGB.
       *
       */
      if (decode_params.param_scan_mode == BROTHER_SCAN_MODE_GRAY)
        {
          state.cinfo.out_color_space = JCS_GRAYSCALE;
        }

      FDBGNP (DBG_EVENT, "Starting decompress.");

      if (!jpeg_start_decompress(&state.cinfo))
	{
	  /*
	   * Need more data.
	   *
	   */
	  return DECODE_STATUS_GOOD;
	}

      /*
       * Make sure that we will be getting the data that we expect.
       * This is for colour only, so we expect 3 components.
       *
       */
      if (state.cinfo.num_components != 3)
	{
          FDBG (DBG_SERIOUS,
               "image components invalid, expecting 3: %d",
               state.cinfo.num_components);
	  return DECODE_STATUS_ERROR;
	}

      FDBG (DBG_IMPORTANT,
           "image parameters: width=%zu height=%zu",
           (size_t)state.cinfo.image_width, (size_t)state.cinfo.image_height);

      /*
       * TODO: Perhaps also check the dimensions are what we are expecting.
       * Depending on the issues related to width alignment, we might
       * have to do some trimming on the scan lines if we had to round up the
       * the width and/or round down the left margin.
       *
       */
    }

  /*
   * Let's try to decompress some JPEG!
   *
   */
  SANE_Byte *output_ptr = dest_data;

  const size_t line_size = sizeof(JSAMPLE) * state.cinfo.output_width * state.cinfo.out_color_components;

  FDBG (DBG_DETAIL,
       "Converting data: line_size=%zu state->output_width=%zu components=%d",
       line_size,
       (size_t)state.cinfo.output_width,
       state.cinfo.out_color_components);

  size_t output_size = dest_data_len;

  while (output_size >= line_size)
    {
      /*
       * We have to ask for single lines because I think that the jpeg_read_scanlines()
       * function is expecting an array of line buffer pointers. We don't have that
       * but we have a single contiguous buffer, so asking for a number of lines at
       * once is *not* going to work, which is a shame because this is less efficient
       * potentially. :(
       *
       */

      /* Check if we have read all scanlines before attempting to read.
       * This suppresses a spurious error message.
       */
      if (state.cinfo.output_scanline >= state.cinfo.output_height)
        {
          FDBGNP (DBG_DETAIL, "converted all scanlines, exiting JFIF decode loop");
          break;
        }

      JDIMENSION converted = jpeg_read_scanlines (&state.cinfo, &output_ptr, 1);

      if (converted == 0)
        {
          FDBGNP (DBG_DETAIL, "converted none");
          break;
        }

      output_size -= line_size;
      output_ptr += converted * line_size;

      FDBG (DBG_DETAIL,
           "Converted %u scanlines. "
           "Remaining scanlines: %u output_remaining=%zu",
           (unsigned int) converted,
           (unsigned int) (state.cinfo.output_height - state.cinfo.output_scanline),
           output_size);

    }

  /*
   * Account for what we have processed.
   *
   * Note that libjpeg will have altered state.src_mgr.bytes_in_buffer
   * to a backtracked position for it to restart again.
   * This is why it is CRUCIAL that we do not try to guess.
   *
   */
  *dest_data_written = output_ptr - dest_data;
  *src_data_consumed = src_data_len - state.src_mgr.bytes_in_buffer;

  FDBG (DBG_EVENT,
       "Finished: consumed %zu bytes, written %zu bytes",
       *src_data_consumed,
       *dest_data_written);

  /*
   * If we have output all the scanlines, then we need to finish up.
   *
   */
  if (state.cinfo.output_scanline >= state.cinfo.output_height)
    {
      /*
       * As explained above, *src_data_consumed will not account for all the bytes that have been read
       * so libjpeg can restart an unfinished decode. In case we are finished, however, we do want
       * to absorb all the remaining bytes so the caller knows that all the data has been processed.
       */
      *src_data_consumed = src_data_len;
      FDBGNP (DBG_EVENT, "Converted all scanlines. Finishing.");

      if (!jpeg_finish_decompress(&state.cinfo))
        {
          FDBGNP (DBG_WARN,
               "Failed to finish JPEG decompress.");
          /* This can happen under some circumstances which are not entirely clear to me.
           * It may have to do with the fact that the device sometimes returns invalid JFIF
           * in the sense that the size in the header doesn't match the image size.
           * We don't want to abort the whole scan here. Just log the error, set cinfo to zero,
           * and hope we don't leak too much memory.
           */
          memset(&state.cinfo, 0, sizeof(state.cinfo));
        }
    }

  return DECODE_STATUS_GOOD;
}


void BrotherGrayRawDecoder::NewPage(const BrotherParameters &params)
{
  decode_params = params;
}

void BrotherGrayRawDecoder::NewBlock()
{
  // Nothing to do.
}

DecodeStatus BrotherGrayRawDecoder::DecodeScanData (const SANE_Byte *src_data, size_t src_data_len,
                                                    size_t *src_data_consumed, SANE_Byte *dest_data,
                                                    size_t dest_data_len, size_t *dest_data_written)
{
  /*
   * Quick check. This isn't strictly necessary because the following code copes
   * properly with src_data_len == 0, but we might change this code in the future
   * and we might not realise that it must.
   *
   * At least we make it clear here that we need src_data to do something useful.
   *
   */
  if (src_data_len == 0)
    {
      return DECODE_STATUS_GOOD;
    }

  /*
   * Just copy to the output what we can from the input.
   * It's just raw data.
   *
   */
  size_t copy_len = std::min(src_data_len, dest_data_len);

  (void)memcpy(dest_data, src_data, copy_len);
  *dest_data_written = copy_len;
  *src_data_consumed = copy_len;

  return DECODE_STATUS_GOOD;
}
