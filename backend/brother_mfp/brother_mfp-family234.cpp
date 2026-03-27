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
#include <list>
#include <memory>

#include <jpeglib.h>
#include <jerror.h>

#include "../include/sane/sane.h"
#include "../include/sane/sanei.h"
#include "../include/sane/sanei_debug.h"

#include "brother_mfp-common.h"
#include "brother_mfp-family.h"

/*
 * Block header identifiers.
 *
 * Scan data codes.
 *
 */
#define BROTHER_DATA_BLOCK_GRAY_RAW             0x40
#define BROTHER_DATA_BLOCK_GRAY_RLENGTH         0x42
#define BROTHER_DATA_BLOCK_RED_RAW              0x44
#define BROTHER_DATA_BLOCK_GREEN_RAW            0x48
#define BROTHER_DATA_BLOCK_BLUE_RAW             0x4c

#define BROTHER_DATA_BLOCK_JPEG                 0x64

/*
 * Termination codes.
 */
// Brother 2, 3, 4 codes
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
#define BROTHER_DATA_BLOCK_SERVICE_ERR          0xc5
// unsure about this one, too. Only recovery is a printer restart.
// Can happen when the previous scan was interrupted by a driver crash.
#define BROTHER_DATA_BLOCK_DEVICE_STUCK         0xc6

/*
 * Other protocol error?
 *
 */
#define BROTHER_DATA_BLOCK_PROTOCOL_ERROR       0xd0

ScanDataHeader::ScanDataHeader () :
    block_type (BROTHER_DATA_BLOCK_NONE),
    block_len (0),
    image_number (0)
{
}

SANE_Status BrotherFamily234Base::EncodeQueryBlock (SANE_Byte *data, size_t data_len,
                                                      size_t *length)
{
  *length = snprintf ((char*) data, data_len, "\x1b" "\x51" "\x0a" "\x80");

  if (*length > data_len)
    {
      FDBG (DBG_SERIOUS,
           "query block too long for buffer: %zu",
           *length);
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;

}

SANE_Status BrotherFamily234Base::EncodeSourceStatusBlock (SANE_Byte *data, size_t data_len, size_t *length)
{
  *length = snprintf ((char*) data, data_len, "\x1b" "D\nADF\n" "\x80");

  if (*length > data_len)
    {
      FDBG (DBG_SERIOUS,
           "Source status block too long for buffer: %zu",
           *length);
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::DecodeSourceStatusBlockResp (const SANE_Byte *data, size_t data_len,
                                                                 BrotherSourceStatusResponse &response)
{
  if (data_len != 1)
    {
      FDBG (DBG_SERIOUS,
           "source status response invalid: %zu",
           data_len);
      return SANE_STATUS_IO_ERROR;
    }

  /*
   * We can get error condition codes from this block.
   *
   */
  switch (data[0])
  {
    case BROTHER_DATA_BLOCK_PAPER_JAM:
      return SANE_STATUS_JAMMED;

    case BROTHER_DATA_BLOCK_COVER_OPEN:
      return SANE_STATUS_COVER_OPEN;

    case BROTHER_DATA_BLOCK_NO_DOCS:
      response.source_ready = SANE_FALSE;
      return SANE_STATUS_GOOD;

    case BROTHER_DATA_BLOCK_SCAN_FINISHED:
      response.source_ready = SANE_TRUE;
      return SANE_STATUS_GOOD;

    default:
      return SANE_STATUS_IO_ERROR;
  }
}

SANE_Status BrotherFamily234Base::EncodeSourceSelectBlock (SANE_Byte *data, size_t data_len, size_t *length)
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
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::DecodeSourceSelectBlockResp (const SANE_Byte *data,
                                                                 size_t data_len,
                                                                 BrotherSourceStatusResponse &response)
{
  if (data_len != 1)
    {
      FDBG (DBG_SERIOUS,
           "source select response invalid: %zu",
           data_len);
      return SANE_STATUS_IO_ERROR;
    }

  /*
   * We can get error condition codes from this block.
   *
   */
  switch (data[0])
  {
    case BROTHER_DATA_BLOCK_SCAN_FINISHED:
      response.source_ready = SANE_TRUE;
      return SANE_STATUS_GOOD;

    case BROTHER_DATA_BLOCK_PROTOCOL_ERROR:
      response.source_ready = SANE_FALSE;
      return SANE_STATUS_INVAL;

    default:
      return SANE_STATUS_IO_ERROR;
  }
}

/*
 * NOTE:
 * Supply a buffer that is at least 1 char longer than is necessary
 * as snprintf() will require space for a terminating NUL.
 *
 */
SANE_Status BrotherFamily234Base::EncodeBasicParameterBlock (SANE_Byte *data, size_t data_len,
                                                               size_t *length)
{
  const char *mode_text = ScanModeToText (scan_params.param_scan_mode);
  if (nullptr == mode_text)
    {
      FDBG (DBG_SERIOUS,
           "failed to get scan mode text for mode %d",
           scan_params.param_scan_mode);
      return SANE_STATUS_INVAL;
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
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}


SANE_Status BrotherFamily234Base::DecodeBasicParameterBlockResp (const SANE_Byte *data, size_t data_len,
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

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::EncodeCancelBlock (SANE_Byte *data, size_t data_len,
                                                               size_t *length)
{
  *length = snprintf ((char*) data, data_len, "\x1b" "R");

  if (*length > data_len)
    {
      FDBG(DBG_SERIOUS, "cancel block too long for buffer: %zu", *length);
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::HasDocs()
{
  return out_of_docs? SANE_STATUS_NO_DOCS: SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::EncodeParameterBlockBlank (SANE_Byte *data, size_t data_len,
                                                               size_t *length)
{
  if (out_of_docs)
    {
      return SANE_STATUS_NO_DOCS;
    }

  const char *mode_text = ScanModeToText (scan_params.param_scan_mode);
  if (nullptr == mode_text)
    {
      FDBG (DBG_SERIOUS,
           "failed to get scan mode text for mode %d",
           scan_params.param_scan_mode);
      return SANE_STATUS_INVAL;
    }

  *length = snprintf ((char*) data, data_len, "\x1b" "X\n\x80");

  if (*length > data_len)
    {
      FDBG (DBG_SERIOUS,
           "parameter block too long for buffer: %zu",
           *length);
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::EncodeParameterBlock (SANE_Byte *data, size_t data_len,
                                                          size_t *length)
{
  const char *mode_text = ScanModeToText (scan_params.param_scan_mode);
  if (nullptr == mode_text)
    {
      FDBG (DBG_SERIOUS,
           "failed to get scan mode text for mode %d",
           scan_params.param_scan_mode);
      return SANE_STATUS_INVAL;
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
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::DecodeScanData (const SANE_Byte *src_data,
							 size_t src_data_len,
							 size_t *src_data_consumed,
							 SANE_Byte *dest_data, size_t dest_data_len,
							 size_t *dest_data_written)
{
  FDBGNP(DBG_EVENT, "");
  SANE_Status ret_status = SANE_STATUS_GOOD;

  /*
   * Check for terminated statii.
   * We have read everything and we cannot return anything further.
   *
   * SANE_STATUS_NO_DOCS - sticks until we receive a cancel().
   * SANE_STATUS_ENDOFDATA - sticks until we receive a start().
   *
   * Note that we check current_decoder because if we still have a decoder
   * we are draining data after receiving the OUTOFDOCS or EOF notifications
   * already and they are pending on all the remaining data being read back.
   * We are generally reading from the device ahead of our interactions with
   * the frontend.
   *
   */

  /*
   * writer - what we supplying scan data to.
   * reader - what we are extracting decode data from to put in dest.
   *
   * If we are actively decoding, we should *always" have a reader.
   *
   */
  BrotherScanDataDecoder *curr_writer = nullptr;
  BrotherScanDataDecoder *curr_reader = nullptr;

  if (!decoders.empty())
    {
      curr_reader = decoders.front().get();
      if (!end_image_flag)
	{
	  curr_writer = decoders.back().get();
	}
    }

  if (out_of_docs && !curr_reader)
    {
      FDBGNP(DBG_SERIOUS, "returning no_docs");
      return SANE_STATUS_NO_DOCS;
    }

  if (end_image_flag && !curr_reader)
    {
      FDBGNP(DBG_SERIOUS, "returning eof");
      return SANE_STATUS_EOF;
    }

  /*
   * Main read loop. At least we have not terminated yet.
   *
   */
  *src_data_consumed = 0;
  *dest_data_written = 0;

  for (;;)
    {
      size_t bytes_consumed = 0;
      size_t bytes_written = 0;

      /*
       * If we are actively decoding, then we must *always*
       * have a reader. At the very end, we might not have a writer once
       * we have written all available data to the final decoder.
       *
       */
      if (curr_reader)
	{
	  if (curr_reader && (curr_writer == curr_reader))
	    {
	      FDBGNP(DBG_SERIOUS, "READING AND WRITING SAME DECODER.");

	      size_t in_len = std::min(src_data_len, current_header.block_len);

	      ret_status = curr_writer->DecodeScanData(src_data, in_len, &bytes_consumed, dest_data,
						       dest_data_len, &bytes_written);
	    }
	  else if (curr_reader && curr_writer && (curr_writer != curr_reader))

	    {
	      FDBGNP(DBG_SERIOUS, "READING AND WRITING DIFFERENT DECODER.");

	      // Input
	      size_t in_len = std::min(src_data_len, current_header.block_len);
	      ret_status = curr_writer->DecodeScanData(src_data, in_len, &bytes_consumed, NULL, 0,
						       &bytes_written);

	      // Output
	      ret_status = curr_reader->DecodeScanData(NULL, 0, &bytes_consumed, dest_data,
						       dest_data_len, &bytes_written);
	    }
	  else
	    {
	      FDBGNP(DBG_SERIOUS, "READING ONLY: DRAINING.");

	      ret_status = curr_reader->DecodeScanData(NULL, 0, &bytes_consumed, dest_data,
						       dest_data_len, &bytes_written);

	    }

	  src_data += bytes_consumed;
	  src_data_len -= bytes_consumed;
	  *src_data_consumed += bytes_consumed;

	  if ((ret_status != SANE_STATUS_GOOD) && (ret_status != SANE_STATUS_EOF))
	    {
	      // Something bad :(
	      break;
	    }

	  // TODO: check current_header.block_len >= bytes_consumed;

	  current_header.block_len -= bytes_consumed;

	  FDBG(DBG_SERIOUS, "current block bytes remaining after decode=%zu",
	       current_header.block_len);

	  dest_data += bytes_written;
	  dest_data_len -= bytes_written;
	  *dest_data_written += bytes_written;

	  /*
	   * Perhaps we have completed the block.
	   *
	   */
	  if (current_header.block_len == 0)
	    {
	      current_header.block_type = BROTHER_DATA_BLOCK_NONE;
	    }

	  if ((end_image_flag || (decoders.size() > 1) || (ret_status == SANE_STATUS_EOF))
	      && !curr_reader->HasUnprocessedDecodeData())
	    {
	      decoders.pop_front();	// remove current reader.

	      FDBG(DBG_SERIOUS, "DELETING DRAINED decoder. Now there are %zu decoders.", decoders.size());

	      /*
	       * Reset the reader/writer.
	       *
	       * We are changing over to the next queued decoder, if there
	       * is one.
	       *
	       */
	      if (!decoders.empty())
		{
		  curr_reader = decoders.front().get();
		  if (!end_image_flag)
		    {
		      curr_writer = decoders.back().get();
		    }
		  else
		    {
		      curr_writer = nullptr;
		    }
		}
	      else
		{
		  curr_writer = nullptr;
		  curr_reader = nullptr;
		}
	    }
	}

      /*
       * Check end conditions.
       *
       */
      if (end_image_flag)
	{
	  if (*dest_data_written)
	    {
	      FDBGNP(DBG_SERIOUS, "exiting with GOOD: end of image seen, but we wrote some data.");
	      return SANE_STATUS_GOOD;	// cached data was available.
	    }
	  else
	    {
	      if (!decoders.empty ())
		{
		  FDBGNP(
		      DBG_SERIOUS,
		      "exiting with GOOD: end of image seen, but there is cached data, but no output space.");
		  return SANE_STATUS_GOOD;	// have cached data, but no output buffer space.
		}
	      else
		{
		  FDBGNP(DBG_SERIOUS, "exiting with ENDOFDATA: end of image and decoder flushed.");
		  if (was_cancelled)
		    {
		      return SANE_STATUS_CANCELLED;
		    }
		  return SANE_STATUS_EOF;	// All done.
		}
	    }
	}

      /*
       * New block read?
       *
       */
      if (current_header.block_type == BROTHER_DATA_BLOCK_NONE)
	{
	  size_t header_consumed = 0;

	  ret_status = DecodeScanDataHeader(src_data, src_data_len, &header_consumed,
					    current_header);

	  if (header_consumed == 0)
	    {
	      // This is OK. we just need more data from the device probably.
	      FDBGNP(DBG_SERIOUS, "need decode header but need more data.");
	      return SANE_STATUS_GOOD;
	    }
	  else if (ret_status != SANE_STATUS_GOOD)
	    {
	      /*
	       * All other statii are terminal. Something went badly wrong.
	       * Probably an error condition on the device, like paper jam.
	       *
	       */
	      FDBGNP(DBG_SERIOUS, "read error status from header read.");
	      break;
	    }

	  src_data += header_consumed;
	  src_data_len -= header_consumed;
	  *src_data_consumed += header_consumed;

	  /*
	   * First: aborts. Scan won't finish so we need to stop immediately.
	   * Can't rely on scanning coming to a normal conclusion so we delete
	   * the decoder.
	   *
	   */
	  if (current_header.block_type == BROTHER_DATA_BLOCK_CANCEL)
	    {
	      end_image_flag = true;
	      out_of_docs = true;
	      was_cancelled = true;

	      decoders.clear();
	    }
	  else if (current_header.block_type == BROTHER_DATA_BLOCK_END_OF_FRAME_MORE)
	    {
	      FDBGNP(DBG_SERIOUS, "got end of frame more header.");

	      end_image_flag = true;
	    }
	  else if (current_header.block_type == BROTHER_DATA_BLOCK_SCAN_FINISHED)
	    {
	      FDBGNP(DBG_SERIOUS, "got end of frame ALL.");
	      end_image_flag = true;
	      out_of_docs = true;
	    }
	  else
	    {
	      /*
	       * Create a new decoder.
	       *
	       */
	      BrotherScanDataDecoder *new_decoder = nullptr;
	      if ((last_block_type != current_header.block_type) || !curr_writer)
		{
		  if (current_header.block_type == BROTHER_DATA_BLOCK_JPEG)
		    {
		      new_decoder = new BrotherJFIFDecoder(scan_params);
		      FDBGNP(DBG_SERIOUS, "CREATED JFIF DECODER.");
		    }
		  else if (current_header.block_type == BROTHER_DATA_BLOCK_GRAY_RLENGTH)
		    {
		      new_decoder = new BrotherGrayRLengthDecoder(scan_params);
		      FDBGNP(DBG_SERIOUS, "CREATED GRAY RLENGTH DECODER.");
		    }
		  else if (current_header.block_type == BROTHER_DATA_BLOCK_GRAY_RAW)
		    {
		      new_decoder = new BrotherGrayRawDecoder(scan_params);
		      FDBGNP(DBG_SERIOUS, "CREATED GRAY DECODER.");
		    }
		  else if ((current_header.block_type == BROTHER_DATA_BLOCK_RED_RAW)
		      || (current_header.block_type == BROTHER_DATA_BLOCK_GREEN_RAW)
		      || (current_header.block_type == BROTHER_DATA_BLOCK_BLUE_RAW))
		    {
		      new_decoder = new BrotherInterleavedRGBColourDecoder(scan_params,
									   capabilities);
		      FDBGNP(DBG_SERIOUS, "CREATED RGB COLOUR DECODER.");
		    }
		  else
		    {
		      FDBG(DBG_SERIOUS, "detected unrecognised block type [%d].",
			   current_header.block_type);
		      ret_status = SANE_STATUS_IO_ERROR;
		      break;
		    }

		  if (!new_decoder)
		    {
		      FDBGNP(DBG_SERIOUS, "failed to create new decoder instance.");
		      ret_status = SANE_STATUS_NO_MEM;
		      break;
		    }

		  /*
		   * Add new decoder to the back of the list.
		   * It will be our new writer (and maybe reader).
		   *
		   */
		  decoders.push_back (std::unique_ptr<BrotherScanDataDecoder> (new_decoder));
		  curr_writer = new_decoder;
		  if (!curr_reader)
		    {
		      curr_reader = new_decoder;
		    }
		  last_block_type = current_header.block_type;
		}
	      else
		{
		  FDBGNP(DBG_SERIOUS, "using existing writer: no change to type.");
		}
	    }
	}
	else if (!bytes_consumed)
	  {
	    FDBG(DBG_SERIOUS, "no bytes were consumed. Try for more. src_data_len=%zu dest_data_len=%zu",
		   src_data_len, dest_data_len);
	    return SANE_STATUS_GOOD;	// awaiting more data.
	  }
    }

  /*
   * Exiting the loop to here means that something went very wrong.
   * Probably an error condition on the device, like paper jam.
   * ret_status will contain something appropriate.
   *
   */
  end_image_flag = true;
  decoders.clear();
  FDBG (DBG_SERIOUS, "exiting with error: %d(%s).", ret_status, sane_strstatus(ret_status));

  return ret_status;
}

SANE_Status BrotherFamily234Base::DecodeButtonQueryResp (const SANE_Byte *data, size_t data_len,
                                                          BrotherButtonQueryResponse &response)
{
  /* family4 sometimes begins with a 0x05 */
  if ((data_len != 4) || (data[0] != 0x05 && data[0] != 0x04) ||(data[1] != 0x10) || (data[2] != 0x03))
    {
      return SANE_STATUS_IO_ERROR;
    }

  response.has_button_press = data[3] == 0x10;

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::DecodeButtonStateResp (const SANE_Byte *data, size_t data_len,
                                                           BrotherButtonStateResponse &response)
{
  if (data_len != 9)
    {
      return SANE_STATUS_IO_ERROR;
    }
  BrotherButtonQueryResponse dummy;
  if (DecodeButtonQueryResp(data, 4, dummy) == SANE_STATUS_GOOD)
    {
      /* We may get "no button pressed" responses while waiting for the second
       * packet containing info on which button was pressed. We encode these
       * intermediate packets by SANE_STATUS_INVAL. */
      return SANE_STATUS_INVAL;
    }
  if (memcmp(data, "\x09" "\x10" "\x03" "\x20", 4) != 0)
    {
      return SANE_STATUS_IO_ERROR;
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
      return SANE_STATUS_UNSUPPORTED;
  }

  return SANE_STATUS_GOOD;
}

void BrotherFamily234Base::NewPage()
{
  current_header.block_type = BROTHER_DATA_BLOCK_NONE;
  decoders.clear();
  last_block_type = BROTHER_DATA_BLOCK_NONE;
  end_image_flag = false;
  out_of_docs = false;
  was_cancelled = false;
}

/*
 * Family 2.
 *
 */

SANE_Status BrotherFamily2::DecodeQueryBlockResp (const SANE_Byte *data, size_t data_len,
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

      return SANE_STATUS_IO_ERROR;
    }

  /*
   * TODO: decode the response block to fill in the details.
   *
   */
  response.dummy = 0;

  return SANE_STATUS_GOOD;
}


/*
 *  If there is insufficient data to decode the header, then SANE_STATUS_GOOD
 * is returned and ^src_data_consumed contains 0.
 *
 */
SANE_Status BrotherFamily2::DecodeScanDataHeader (const SANE_Byte *src_data,
                                                          size_t src_data_len,
                                                          size_t *src_data_consumed,
                                                          ScanDataHeader &header)
{
  FDBGNP (DBG_EVENT, "");

  if (src_data_len == 0)
    {
      return SANE_STATUS_GOOD;	// truncated.
    }

  /*
   * Clear out.
   *
   */
  header.block_type = BROTHER_DATA_BLOCK_NONE;
  header.block_len = 0;
  header.image_number = 0;

  /*
   * Error conditions. Single bytes: 0xc*
   *
   */
  if ((src_data[0] & 0xc0) == 0xc0)
    {
      header.block_type = src_data[0];
      *src_data_consumed = 1;

      if (header.block_type == BROTHER_DATA_BLOCK_NO_DOCS)
	{
	  FDBGNP (DBG_DETAIL, "read no_docs header.");
	  return SANE_STATUS_NO_DOCS;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_PAPER_JAM)
	{
	  FDBGNP (DBG_DETAIL, "read no_paper jam header.");
	  return SANE_STATUS_JAMMED;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_COVER_OPEN)
	{
	  FDBGNP (DBG_DETAIL, "cover open header.");
	  return SANE_STATUS_COVER_OPEN;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_SERVICE_ERR)
	{
	  FDBGNP (DBG_DETAIL, "service error header.");
	  return SANE_STATUS_IO_ERROR;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_DEVICE_STUCK)
	{
	  FDBGNP (DBG_DETAIL, "device stuck header.");
	  return SANE_STATUS_IO_ERROR;
	}

      // Don't know this one!
      FDBG (DBG_SERIOUS,
           "unknown error status encountered: 0x%2.2x",
           (unsigned int) header.block_type);

      return SANE_STATUS_UNSUPPORTED;
    }

  /*
   * Normal status indications.
   *
   */
  if ((src_data[0] & 0x80) == 0x80)
    {
      header.block_type = src_data[0];
      header.block_len = 0;

      /*
       * This block appears at the end of each frame.
       *
       */
      if (header.block_type == BROTHER_DATA_BLOCK_SCAN_FINISHED)
	{
	  FDBG (DBG_DETAIL, "read scan finished header [0x%2.2x].", header.block_type);
	  *src_data_consumed = 1;
	  return SANE_STATUS_GOOD;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_END_OF_FRAME_MORE)
	{
	  FDBG (DBG_DETAIL, "read scan finished/no more header [0x%2.2x].", header.block_type);
	  *src_data_consumed = 1;
	  return SANE_STATUS_GOOD;
	}
    }

    /*
     * Other data-carrying packets.
     *
     */
  if (src_data[0] & 0x40)
    {
      if (src_data_len < 3)
	{
	  return SANE_STATUS_GOOD;	// truncated, more to come.
	}

      *src_data_consumed = 3;

      header.block_type = src_data[0];
      header.block_len = src_data[1] + (src_data[2] << 8);

      FDBG (DBG_DETAIL, "read scan data header [0x%2.2x].", header.block_type);

      return SANE_STATUS_GOOD;
    }

  /*
   * Unknown data block type.
   *
   */
  FDBG(DBG_SERIOUS,
       "unknown data block type encountered: 0x%2.2x",
       (unsigned int ) header.block_type);

  return SANE_STATUS_UNSUPPORTED;
}

/*
 * Family 3.
 *
 */

SANE_Status BrotherFamily3::DecodeQueryBlockResp (const SANE_Byte *data, size_t data_len,
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

      return SANE_STATUS_IO_ERROR;
    }

  /*
   * TODO: decode the response block to fill in the details.
   *
   */
  response.dummy = 0;

  return SANE_STATUS_GOOD;
}


SANE_Status BrotherFamily3::DecodeScanDataHeader (const SANE_Byte *src_data,
                                                          size_t src_data_len,
                                                          size_t *src_data_consumed,
                                                          ScanDataHeader &header)
{
  FDBGNP (DBG_EVENT, "");

  if (src_data_len == 0)
    {
      return SANE_STATUS_GOOD;	// truncated.
    }

  /*
   * Clear out.
   *
   */
  header.block_type = BROTHER_DATA_BLOCK_NONE;
  header.block_len = 0;
  header.image_number = 0;

  /*
   * Error conditions. Single bytes: 0xc*
   *
   */
  if ((src_data[0] & 0xc0) == 0xc0)
    {
      header.block_type = src_data[0];
      *src_data_consumed = 1;

      if (header.block_type == BROTHER_DATA_BLOCK_NO_DOCS)
	{
	  FDBGNP (DBG_DETAIL, "read no_docs header.");
	  return SANE_STATUS_NO_DOCS;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_PAPER_JAM)
	{
	  FDBGNP (DBG_DETAIL, "read no_paper jam header.");
	  return SANE_STATUS_JAMMED;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_COVER_OPEN)
	{
	  FDBGNP (DBG_DETAIL, "cover open header.");
	  return SANE_STATUS_COVER_OPEN;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_SERVICE_ERR)
	{
	  FDBGNP (DBG_DETAIL, "service error header.");
	  return SANE_STATUS_IO_ERROR;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_DEVICE_STUCK)
	{
	  FDBGNP (DBG_DETAIL, "device stuck header.");
	  return SANE_STATUS_IO_ERROR;
	}

      // Don't know this one!
      FDBG (DBG_SERIOUS,
           "unknown error status encountered: 0x%2.2x",
           (unsigned int) header.block_type);

      return SANE_STATUS_UNSUPPORTED;
    }

  /*
   * Normal status indications.
   *
   */
  if ((src_data[0] & 0x80) == 0x80)
    {
      header.block_type = src_data[0];
      header.block_len = 0;

      /*
       * This block appears at the end of each frame.
       *
       */
      if (header.block_type == BROTHER_DATA_BLOCK_SCAN_FINISHED)
	{
	  FDBG (DBG_DETAIL, "read scan finished header [0x%2.2x].", header.block_type);
	  *src_data_consumed = 1;
	  return SANE_STATUS_GOOD;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_END_OF_FRAME_MORE)
	{
	  FDBG (DBG_DETAIL, "read scan finished/no more header [0x%2.2x].", header.block_type);
	  *src_data_consumed = 1;
	  return SANE_STATUS_GOOD;
	}
    }

    /*
     * Other data-carrying packets.
     *
     */
  if (src_data[0] & 0x40)
    {
      if (src_data_len < 3)
	{
	  return SANE_STATUS_GOOD;	// truncated, more to come.
	}

      *src_data_consumed = 3;

      header.block_type = src_data[0];
      header.block_len = src_data[1] + (src_data[2] << 8);

      FDBG (DBG_DETAIL, "read scan data header [0x%2.2x].", header.block_type);

      return SANE_STATUS_GOOD;
    }

  /*
   * Unknown data block type.
   *
   */
  FDBG(DBG_SERIOUS,
       "unknown data block type encountered: 0x%2.2x",
       (unsigned int ) header.block_type);

  return SANE_STATUS_UNSUPPORTED;
}


/*
 * Family 4.
 *
 */

SANE_Status BrotherFamily4::DecodeQueryBlockResp (const SANE_Byte *data, size_t data_len,
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

      return SANE_STATUS_IO_ERROR;
    }

  /*
   * TODO: decode the response block to fill in the details.
   *
   */
  response.dummy = 0;

  return SANE_STATUS_GOOD;
}


char const* BrotherFamily4::GetFamilySpecificParameterBlock ()
{
  /* additional scan parameters in family4, might not be needed */
  return "S=NORMAL_SCAN\nP=0\nG=0\nL=0\n";
}


/*
 * If there is insufficient data to decode the header, then SANE_STATUS_GOOD
 * is returned and ^src_data_consumed contains 0.
 *
 */
SANE_Status BrotherFamily4::DecodeScanDataHeader (const SANE_Byte *src_data,
                                                          size_t src_data_len,
                                                          size_t *src_data_consumed,
                                                          ScanDataHeader &header)
{
//  SANE_Status status_ret = BrotherEncoderFamily234Base::DecodeScanDataHeader(src_data, src_data_len, src_data_consumed, header);
//  if (status_ret != SANE_STATUS_GOOD)
//  {
//    return status_ret;
//  }
//
//  if (header.block_type == BROTHER_DATA_BLOCK_END_OF_FRAME_NO_MORE)
//    {
//      /* Family4 devices never return END_OF_FRAME_MORE. Therefore,
//       * the "end of frame" is unspecific and could be either of
//       * ENDOFFRAME_NO_MORE or ENDOFFRAME_WITH_MORE. To distinguish the cases
//       * we peek ahead to the first byte of the next packet. If it is a
//       * SCAN_FINISHED, we won't get any additional frames. In any other case
//       * it marks the beginning of a new frame.
//       *
//       * Therefore, even though the packet is 10 bytes long, we return
//       * TRUNCATED if we don't have the first byte of the next packet yet.
//       */
//      if (src_data_len < 10 + 1)
//        {
//          return SANE_STATUS_TRUNCATED;
//        }
//
//      *src_data_consumed = 10;
//      return (src_data[10] == BROTHER_DATA_BLOCK_SCAN_FINISHED) ?
//        SANE_STATUS_ENDOFFRAME_NO_MORE :
//        SANE_STATUS_ENDOFFRAME_WITH_MORE;
//    }
//
//  /*
//   * Other data-carrying packets.
//   *
//   */
//  if (src_data_len < 12)
//    {
//      return SANE_STATUS_TRUNCATED;
//    }
//
//  *src_data_consumed = 12;
//
//  header.block_len = src_data[10] + (src_data[11] << 8);
//  header.image_number = src_data[3] + (src_data[4] << 8);

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily4::EncodeParameterBlockBlank (SANE_Byte *data, size_t data_len,
                                                               size_t *length)
{
  /* Sending blank X commands after the first page is unnecessary on family4 and slows down the scan */
  (void)data;
  (void)data_len;
  *length = 0;
  return SANE_STATUS_GOOD;
}
