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
#define BROTHER_DATA_BLOCK_RLENGTH5             0x01
#define BROTHER_DATA_BLOCK_JPEG5                0x02

/*
 * Termination codes.
 */
#define BROTHER_DATA_BLOCK_END_OF_FRAME_5	0x21 // 0x00 21 nn nn
#define BROTHER_DATA_BLOCK_END_OF_ALL_FRAMES_5  0x20 // 0x00 20
#define BROTHER_DATA_BLOCK_PROT_CANCEL_5        0x40 // 0x00 40
#define BROTHER_DATA_BLOCK_DEV_CANCEL_5         0x41 // 0x00 41

/*
 * Error condition codes.
 *
 */
#define BROTHER_DATA_BLOCK_BUSY_ERR             0x81 // 0x81 00
#define BROTHER_DATA_BLOCK_PROT_ERR             0x83 // 0x83 00

#define BROTHER_DATA_BLOCK_NO_DOCS_5            0x90 // 0x90 00
#define BROTHER_DATA_BLOCK_PAPER_JAM_5          0x91 // 0x91 00

/*
 * Error responses.
 *
 */
#define BROTHER_DATA_QUERY_RESPONSE             0xc1

SANE_Status BrotherFamily5::Connect ()
{
  const std::lock_guard<std::mutex> lock(mutex);

  SANE_Status res;

  res = transport->Acquire();
  if (res != SANE_STATUS_GOOD)
    {
      FDBG (DBG_SERIOUS, "failed to connect: %d", res);
      return res;
    }

  res = transport->Connect();
  if (res != SANE_STATUS_GOOD)
    {
      FDBG (DBG_SERIOUS, "failed to connect: %d", res);
      return res;
    }

  res = Init();
  if (res != SANE_STATUS_GOOD)
    {
      (void)transport->Disconnect();
      FDBG (DBG_SERIOUS, "failed to connect: %d", res);
      return res;
    }

  res = transport->Disconnect();
  if (res != SANE_STATUS_GOOD)
    {
      FDBG (DBG_SERIOUS, "failed to connect: %d", res);
      return res;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily5::Disconnect ()
{
  const std::lock_guard<std::mutex> lock(mutex);

  SANE_Status res;

  res = transport->Release();
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to release: %d", res);
      return res;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily5::StartScan ()
{
  const std::lock_guard<std::mutex> lock(mutex);

  SANE_Status res;

  FDBGNP (DBG_EVENT, "");

  /*
   * Check state.
   * Only IDLE or EOF are really valid.
   *
   * If IDLE, then we are starting a completely new session.
   * if EOF, then we are acquiring the "next" image.
   *
   */
  if (scan_state == SCANSTATE_ACQUIRING)
    {
      return SANE_STATUS_INVAL;
    }

  /*
   * If IDLE then we need to kick off a completely new session.
   * There are a few things that we need to send to get things moving.
   *
   */
  was_cancelled = false;

  if (scan_state == SCANSTATE_IDLE)
    {
      ResetState(true);

      /*
       * Initialisation of a new scanning session.
       *
       */
      res = transport->Connect();
      if (res != SANE_STATUS_GOOD)
        {
          FDBG (DBG_SERIOUS, "failed to connect: %d", res);
          (void)CancelScan_Internal();
          return res;
        }

      res = StartScan_BasicParameter();
      if (res != SANE_STATUS_GOOD)
	{
          FDBG (DBG_SERIOUS, "failed to send basic parameter block: %d", res);
          (void) CancelScan_Internal ();
          return res;
	}

      res = StartScan_Parameter();
      if (res != SANE_STATUS_GOOD)
	{
          FDBG (DBG_SERIOUS, "failed to send parameter block: %d", res);
          (void) CancelScan_Internal ();
          return res;
	}
     }
  else
    {
      ResetState(false);
    }
  scan_state = SCANSTATE_ACQUIRING;

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily5::ReadData(SANE_Byte *data, size_t max_length, size_t *length)
{
  const std::lock_guard<std::mutex> lock(mutex);

  *length = 0;

  FDBGNP(DBG_EVENT, "");

  if (was_cancelled)
    {
      was_cancelled = false;
      return SANE_STATUS_CANCELLED;
    }

  if (scan_state != SCANSTATE_ACQUIRING)
    {
      return SANE_STATUS_INVAL;
    }

  /*
   * OK. If we are in async IO mode, then we will only return if we read something
   * while in the acquisition state.
   *
   * Also we could exit if:
   *
   * - Cancel requested
   * - Got EOF notification
   * - Error.
   *
   */
  SANE_Status res = SANE_STATUS_GOOD;

  for (;;)
    {

      /*
       * First we try to fill the output buffer from scanned data.
       * No need to read from USB when we still have plenty of data to process and it
       * will avoid unnecessary hammering of the device.
       *
       */
      res = DecodeReadBuffer(data, max_length, length);
      if ((res == SANE_STATUS_GOOD) && (*length == 0))
	{
	  /*
	   * In this case we read from transport to further fill data.
	   *
	   */
	  size_t bytes_read = 0;
	  res = transport->ReadScanData(data_buffer + data_buffer_bytes,
					BROTHER_READ_BUFFER_LEN - data_buffer_bytes,
					bytes_read);
	  if (res != SANE_STATUS_GOOD)
	    {
	      (void)CancelScan_Internal();
	      return res;
	    }

	  /*
	   * If we read something, then have another go at decoding.
	   *
	   */
	  if (bytes_read)
	    {
	      data_buffer_bytes += bytes_read;
	      res = DecodeReadBuffer(data, max_length, length);
	    }
	}

      /*
       * At this point, 'res' should only contain what we got from DecodeReadBuffer()!
       * If the decoder says that we finished the image, then we change state.
       *
       * We *only* expect to get SANE_STATUS_EOF when we get the image terminating sequence
       * from the scanner, *not* the end of the scan data for this image otherwise we're
       * going to get really confused. Also, the SANE API standard says that if we are returning
       * SANE_STATUS_EOF, we cannot be passing back any data.
       *
       */
      if (res == SANE_STATUS_EOF)
	{
	  scan_state = SCANSTATE_EOF;
	  break;
	}
      else if (res != SANE_STATUS_GOOD)
	{
	  (void)CancelScan_Internal();
	  scan_state = SCANSTATE_IDLE;
	  break;
	}

      /*
       * If we are in blocking IO mode, then if we read nothing and no error, wait a bit
       * and try again. We could release the lock while we're sleeping but we aren't sleeping
       * for very long and it would make things complicated. Let's not.
       *
       */
      if (transport->GetAsyncIOMode() || (*length != 0))
	{
	  break;
	}

      FDBGNP(DBG_DETAIL, "no read, so sleeping for a bit.");
      struct timespec ts = { 0, 100000 };

      nanosleep(&ts, NULL);
    }

  return res;
}

SANE_Status BrotherFamily5::DecodeReadBuffer(SANE_Byte *data, size_t max_length, size_t *length)
{
  size_t data_consumed = 0;

  FDBGNP(DBG_EVENT, "");

  SANE_Status dec_ret = DecodeScanData(data_buffer, data_buffer_bytes, &data_consumed, data,
				       max_length, length);

  /*
   * Shift down the read buffer so that we can maximise our
   * space to read new data.
   *
   */
  if (data_consumed)
    {
      (void) memmove (data_buffer, data_buffer + data_consumed, data_buffer_bytes - data_consumed);
      data_buffer_bytes -= data_consumed;
    }

  FDBG (DBG_DETAIL,
       "decoder consumes %zu bytes and writes %zu bytes, returning %s",
       data_consumed,
       *length,
       sane_strstatus(dec_ret));

  return dec_ret;
}

SANE_Status BrotherFamily5::SetAsyncIOMode(bool async_io)
{
  const std::lock_guard<std::mutex> lock(mutex);

  if (scan_state != SCANSTATE_ACQUIRING)
    {
      return SANE_STATUS_INVAL;
    }

  return transport->SetAsyncIOMode(async_io);
}

void BrotherFamily5::Cancel()
{
  /*
   * Do transport cancel before we take the mutex!!!
   * Otherwise, we will never actually abort anything since
   * all other operations hold the mutex as well and we will never
   * take the mutex while another operation is in progress.
   *
   * It is safe to call this one transport function outside
   * the mutex lock and it will try to abort whatever we are
   * doing in the transport at the moment. Then taking our lock
   * makes sure that we are safe to shutdown the session.
   *
   */
  transport->Cancel();
  was_cancelled = true;

  const std::lock_guard<std::mutex> lock(mutex);

  /*
   * Terminate any active transport operation.
   * Then shutdown everything back to the IDLE state.
   *
   */
  (void)CancelScan_Internal();
}

SANE_Status BrotherFamily5::Init ()
{
  FDBGNP (DBG_EVENT, "");

  SANE_Status res;

  /*
   * Execute Query request.
   *
   */
  FDBGNP(DBG_EVENT, "Sending QUERY request.");

  size_t send_len = 0;
  SANE_Status dec_ret = EncodeQueryBlock (small_send_buffer, sizeof(small_send_buffer), &send_len);
  if (dec_ret != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to generate query block: %d", dec_ret);
      return SANE_STATUS_INVAL;
    }

  size_t rec_len = sizeof(small_rec_buffer);
  useconds_t timeout = TIMEOUT_SECS(8);

  res = transport->SendAndReceive(small_send_buffer, send_len, small_rec_buffer, &rec_len, &timeout);
  if ((res != SANE_STATUS_GOOD) || (timeout == 0))
    {
      FDBG(DBG_SERIOUS, "failed to send query block: %d", res);
      return res;
    }

  BrotherQueryResponse query_resp;

  res = DecodeQueryBlockResp (small_rec_buffer, rec_len, query_resp);
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to read query block response: %d", res);
      return res;
    }

  return SANE_STATUS_GOOD;
}



SANE_Status BrotherFamily5::StartScan_BasicParameter ()
{
  SANE_Status res;

  FDBGNP (DBG_EVENT, "");

  size_t send_len = 0;
  res = EncodeBasicParameterBlock (small_send_buffer, sizeof(small_send_buffer), &send_len);
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to generate basic param block: %d", res);
      return SANE_STATUS_INVAL;
    }

  size_t rec_len = sizeof(small_rec_buffer);
  useconds_t timeout = TIMEOUT_SECS(8);

  res = transport->SendAndReceive(small_send_buffer, send_len, small_rec_buffer, &rec_len, &timeout);
  if ((res != SANE_STATUS_GOOD) || (timeout == 0))
    {
      FDBG(DBG_SERIOUS, "failed to send basic parameter block: %d", res);
      return res;
    }

  BrotherBasicParamResponse basic_param_resp;

  res = DecodeBasicParameterBlockResp (small_rec_buffer, rec_len, basic_param_resp);
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to read prep parameter block response: %d", res);
      return res;
    }

  return SANE_STATUS_GOOD;
}


SANE_Status BrotherFamily5::StartScan_Parameter()
{
  SANE_Status res;

  FDBGNP (DBG_EVENT, "");

  size_t send_len = 0;
  res = EncodeParameterBlock (small_send_buffer, sizeof(small_send_buffer), &send_len);
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to generate param block: %d", res);
      return SANE_STATUS_INVAL;
    }

  res = transport->SendCommand(small_send_buffer, send_len);
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to send parameter block: %d", res);
      return res;
    }

  return SANE_STATUS_GOOD;
}

const char* BrotherFamily5::ScanModeToText (BrotherScanMode scan_mode)
{
  static const char *scan_mode_text[] =
    { "C24BIT", "GRAY256", "ERRDIF", "TEXT" };  // also C256?

  return scan_mode_text[scan_mode];
}

SANE_Status BrotherFamily5::EncodeQueryBlock (SANE_Byte *data, size_t data_len,
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

SANE_Status BrotherFamily5::DecodeQueryBlockResp (const SANE_Byte *data, size_t data_len,
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

      return SANE_STATUS_IO_ERROR;
    }

  /*
   * TODO: decode the response block to fill in the details.
   *
   */
  response.dummy = 0;

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily5::DecodeSessionResp (const SANE_Byte *data, size_t data_len,
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

      return SANE_STATUS_IO_ERROR;
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

      return SANE_STATUS_IO_ERROR;
    }

  /*
   * TODO: figure out what the rest of the packet is supposed to be.
   *
   */
  response.ready = data[4] == 0x00? SANE_TRUE: SANE_FALSE;

  return SANE_STATUS_GOOD;
}

/*
 * NOTE:
 * Supply a buffer that is at least 1 char longer than is necessary
 * as snprintf() will require space for a terminating NUL.
 *
 */
SANE_Status BrotherFamily5::EncodeBasicParameterBlock (SANE_Byte *data, size_t data_len,
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
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily5::DecodeBasicParameterBlockResp (const SANE_Byte *data, size_t data_len,
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

SANE_Status BrotherFamily5::EncodeSourceStatusBlock (SANE_Byte *data, size_t data_len, size_t *length)
{
  (void)data;
  (void)data_len;
  (void)length;

//  *length = snprintf ((char*) data, data_len, "\x1b" "D\nADF\n" "\x80");
//
//  if (*length > data_len)
//    {
//      DBG (DBG_SERIOUS,
//           "BrotherFamily5::EncodeSourceStatusBlock: Source status block too long for buffer: %zu",
//           *length);
//      return SANE_STATUS_INVAL;
//    }
//
//  return SANE_STATUS_GOOD;

  return SANE_STATUS_UNSUPPORTED;
}

SANE_Status BrotherFamily5::DecodeSourceStatusBlockResp (const SANE_Byte *data,
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
//      return SANE_STATUS_ERROR;
//    }
//
//  /*
//   * We can get error condition codes from this block.
//   *
//   */
//  switch (data[0])
//  {
//    case BROTHER_DATA_BLOCK_PAPER_JAM:
//      return SANE_STATUS_JAMMED;
//
//    case BROTHER_DATA_BLOCK_COVER_OPEN:
//      return SANE_STATUS_COVER_OPEN;
//
//    case BROTHER_DATA_BLOCK_NO_DOCS:
//      response.source_ready = SANE_FALSE;
//      return SANE_STATUS_GOOD;
//
//    case BROTHER_DATA_BLOCK_SCAN_FINISHED:
//      response.source_ready = SANE_TRUE;
//      return SANE_STATUS_GOOD;
//
//    default:
//      return SANE_STATUS_ERROR;
//  }

  return SANE_STATUS_UNSUPPORTED;
}

SANE_Status BrotherFamily5::EncodeSourceSelectBlock (SANE_Byte *data, size_t data_len, size_t *length)
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
//           "BrotherFamily5::EncodeSourceSelectBlock: Source select block too long for buffer: %zu",
//           *length);
//      return SANE_STATUS_INVAL;
//    }
//
//  return SANE_STATUS_GOOD;

  return SANE_STATUS_UNSUPPORTED;
}

SANE_Status BrotherFamily5::DecodeSourceSelectBlockResp (const SANE_Byte *data,
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
//      return SANE_STATUS_ERROR;
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
//      return SANE_STATUS_GOOD;
//
//    case BROTHER_DATA_BLOCK_PROTOCOL_ERROR:
//      response.source_ready = SANE_FALSE;
//      return SANE_STATUS_INVAL;
//
//    default:
//      return SANE_STATUS_ERROR;
//  }

  return SANE_STATUS_UNSUPPORTED;
}

SANE_Status BrotherFamily5::EncodeCancelBlock (SANE_Byte *data, size_t data_len,
                                                               size_t *length)
{
  *length = snprintf ((char*) data, data_len, "\x1b" "ABT\n\x80");

  if (*length > data_len)
    {
      FDBG(DBG_SERIOUS, "cancel block too long for buffer: %zu", *length);
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily5::EncodeParameterBlock (SANE_Byte *data, size_t data_len,
                                                         size_t *length)
{
  const char *mode_text = ScanModeToText (scan_params.param_scan_mode);
  if (nullptr == mode_text)
    {
      FDBG(DBG_SERIOUS, "failed to get scan mode text for mode %d", scan_params.param_scan_mode);
      return SANE_STATUS_INVAL;
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
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily5::DecodeScanData(const SANE_Byte *src_data, size_t src_data_len,
					   size_t *src_data_consumed, SANE_Byte *dest_data,
					   size_t dest_data_len, size_t *dest_data_written)
{
  FDBGNP(DBG_SERIOUS, "");
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
  if (out_of_docs && !current_decoder)
    {
      return SANE_STATUS_NO_DOCS;
    }

  if (end_image_flag && !current_decoder)
    {
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

      if (current_decoder)
	{
	  if (current_header.block_type != BROTHER_DATA_BLOCK_NONE)
	    {
	      size_t in_len = std::min(src_data_len, current_header.block_len);

	      ret_status = current_decoder->DecodeScanData(src_data, in_len, &bytes_consumed,
							   dest_data, dest_data_len,
							   &bytes_written);
	    }
	  else
	    {
	      FDBGNP(DBG_SERIOUS, "decoding, but no source.");

	      ret_status = current_decoder->DecodeScanData(nullptr, 0, &bytes_consumed, dest_data,
							   dest_data_len, &bytes_written);
	    }

	  src_data += bytes_consumed;
	  src_data_len -= bytes_consumed;
	  *src_data_consumed += bytes_consumed;

	  if (ret_status != SANE_STATUS_GOOD)
	    {
	      // Something bad :(
	      break;
	    }

	  // TODO: check current_header.block_len >= bytes_consumed;

	  current_header.block_len -= bytes_consumed;

	  FDBG(DBG_DETAIL, "current block bytes remaining after decode=%zu",
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

	  if (end_image_flag && !current_decoder->HasUnprocessedDecodeData())
	    {
	      delete current_decoder;
	      current_decoder = nullptr;
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
	      FDBGNP(DBG_DETAIL, "exiting with GOOD: end of image seen, but we wrote some data.");
	      return SANE_STATUS_GOOD;	// cached data was available.
	    }
	  else
	    {
	      if (current_decoder)
		{
		  FDBGNP(
		      DBG_DETAIL,
		      "exiting with GOOD: end of image seen, but there is cached data, but no output space.");
		  return SANE_STATUS_GOOD;	// have cached data, but no output buffer space.
		}
	      else
		{
		  FDBGNP(DBG_DETAIL, "exiting with ENDOFDATA: end of image and decoder flushed.");
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

	  src_data += header_consumed;
	  src_data_len -= header_consumed;
	  *src_data_consumed += header_consumed;

	  if (header_consumed == 0)
	    {
	      // This is OK. we just need more data from the device probably.
	      return SANE_STATUS_GOOD;
	    }
	  else if (ret_status != SANE_STATUS_GOOD)
	    {
	      /*
	       * All other statii are terminal. Something went badly wrong.
	       * Probably an error condition on the device, like paper jam.
	       *
	       */
	      FDBGNP(DBG_WARN, "something went really wrong reading the block header.");
	      break;
	    }

	  /*
	   * First: aborts. Scan won't finish so we need to stop immediately.
	   * Can't rely on scanning coming to a normal conclusion so we delete
	   * the decoder.
	   *
	   */
	  if ((current_header.block_type == BROTHER_DATA_BLOCK_PROT_CANCEL_5)
	      || (current_header.block_type == BROTHER_DATA_BLOCK_DEV_CANCEL_5))
	    {
	      end_image_flag = true;
	      out_of_docs = true;
	      was_cancelled = true;

	      delete current_decoder;
	      current_decoder = nullptr;
	    }
	  else if (current_header.block_type == BROTHER_DATA_BLOCK_END_OF_FRAME_5)
	    {
	      end_image_flag = true;
	    }
	  else if (current_header.block_type == BROTHER_DATA_BLOCK_END_OF_ALL_FRAMES_5)
	    {
	      out_of_docs = true;
	    }
	  else
	    {
	      if (!current_decoder)
		{
		  if (current_header.block_type == BROTHER_DATA_BLOCK_JPEG5)
		    {
		      current_decoder = new BrotherJFIFDecoder(scan_params);
		    }
		  else if (current_header.block_type == BROTHER_DATA_BLOCK_RLENGTH5)
		    {
		      current_decoder = new BrotherGrayRLengthDecoder(scan_params);
		    }
		  else
		    {
		      // TODO generate error.
		    }
		  //TODO Check current_decoder
		}
	    }
	}
      else if (!bytes_consumed)
	{
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
  delete current_decoder;
  current_decoder = nullptr;
  FDBG(DBG_DETAIL, "exiting with error: %d(%s).", ret_status, sane_strstatus(ret_status));

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
 * If there is insufficient data to decode the header, then SANE_STATUS_GOOD
 * is returned and ^src_data_consumed contains 0.
 *
 */
SANE_Status BrotherFamily5::DecodeScanDataHeader(const SANE_Byte *src_data, size_t src_data_len,
						 size_t *src_data_consumed, ScanDataHeader &header)
{
  FDBGNP (DBG_SERIOUS, "");

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
   * Error conditions.
   *
   */
  if (src_data[0] & 0x80)
    {
      if (src_data_len < 2)
	{
	  return SANE_STATUS_GOOD;	// truncated.
	}

      header.block_type = src_data[0];
      *src_data_consumed = 2;

      if (header.block_type == BROTHER_DATA_BLOCK_NO_DOCS_5)
	{
	  FDBGNP (DBG_SERIOUS, "read no_docs header.");
	  return SANE_STATUS_NO_DOCS;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_PAPER_JAM_5)
	{
	  FDBGNP (DBG_SERIOUS, "read no_paper jam header.");
	  return SANE_STATUS_JAMMED;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_BUSY_ERR)
	{
	  FDBGNP (DBG_SERIOUS, "read device busy header.");
	  return SANE_STATUS_DEVICE_BUSY;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_PROT_ERR)
	{
	  FDBGNP (DBG_SERIOUS, "read protocol error header.");
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
  if (src_data[0] == 0)
    {
      header.block_type = src_data[1];
      header.block_len = 0;
      header.image_number = 0;

      /*
       * This block appears at the end of each frame.
       *
       */
      if (header.block_type == BROTHER_DATA_BLOCK_END_OF_FRAME_5)
	{
	  if (src_data_len < 4)
	    {
	      return SANE_STATUS_GOOD;	// truncated.
	    }

	  FDBG (DBG_SERIOUS, "read end of frame header [0x%2.2x].", header.block_type);

	  *src_data_consumed = 4;
	  header.image_number = src_data[2] + (src_data[3] << 8);
	  return SANE_STATUS_GOOD;
	}

      /*
       * These are short control blocks.
       *
       */
      if ((header.block_type == BROTHER_DATA_BLOCK_END_OF_ALL_FRAMES_5)
	  || (header.block_type == BROTHER_DATA_BLOCK_DEV_CANCEL_5)
	  || (header.block_type == BROTHER_DATA_BLOCK_PROT_CANCEL_5))
	{
	  if (src_data_len < 2)
	    {
	      return SANE_STATUS_GOOD;	// truncated
	    }

	  FDBG (DBG_SERIOUS, "read scan termination code [0x%2.2x].", header.block_type);

	  *src_data_consumed = 2;
	  return SANE_STATUS_GOOD;
	}

      /*
       * Data-carrying packets.
       *
       */
      if ((header.block_type == BROTHER_DATA_BLOCK_RLENGTH5) ||
	  (header.block_type == BROTHER_DATA_BLOCK_JPEG5))
	{
	  if (src_data_len < 14)
	    {
	      return SANE_STATUS_GOOD;	// truncated
	    }

	  *src_data_consumed = 14;

	  header.block_len = src_data[6] + (src_data[7] << 8);
	  header.image_number = src_data[2] + (src_data[3] << 8);

	  FDBG (DBG_DETAIL,
	       "block header decoded. type=0x%2.2x len=%zu image_num=%u",
	       (unsigned int) header.block_type,
	       header.block_len,
	       (unsigned int)header.image_number);

	  return SANE_STATUS_GOOD;
	}

      /*
       * Unknown data block type.
       *
       */
      FDBG (DBG_SERIOUS,
           "unknown data block type encountered: 0x%2.2x",
           (unsigned int) header.block_type);

      return SANE_STATUS_UNSUPPORTED;

    }

  /*
   * Don't know this block type.
   * We might also have gotten out of sync.
   *
   */
  FDBG (DBG_SERIOUS,
       "unknown scan block type encountered: 0x%2.2x",
       (unsigned int) src_data[0]);

  return SANE_STATUS_UNSUPPORTED;
}

SANE_Status BrotherFamily5::DecodeButtonQueryResp (const SANE_Byte *data, size_t data_len,
                                                           BrotherButtonQueryResponse &response)
{
  if ((data_len != 4) || (data[0] != 0x04) || (data[1] != 0x10) || (data[2] != 0x03))
    {
      return SANE_STATUS_IO_ERROR;
    }

  response.has_button_press = data[3] == 0x10;

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily5::DecodeButtonStateResp (const SANE_Byte *data, size_t data_len,
                                                           BrotherButtonStateResponse &response)
{
  if ((data_len != 9) || (memcmp(data, "\x09" "\x10" "\x03" "\x20", 4) != 0))
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

SANE_Status BrotherFamily5::CancelScan_Internal()
{
  SANE_Status res;
  size_t send_len = 0;

  FDBGNP(DBG_EVENT, "");

  if (scan_state == SCANSTATE_IDLE)
    {
      FDBGNP(DBG_DETAIL, "not scanning.");
      return SANE_STATUS_GOOD;
    }

  if (transport->IsConnected())
    {

      /*
       * Send the cancel command.
       *
       */
      if (scan_state == SCANSTATE_ACQUIRING)
	{
	  res = EncodeCancelBlock(small_send_buffer, sizeof(small_send_buffer), &send_len);

	  if (res != SANE_STATUS_GOOD)
	    {
	      FDBG(DBG_SERIOUS, "failed to generate basic param block: %d", res);
	      return SANE_STATUS_INVAL;
	    }

	  res = transport->SendCommand(small_send_buffer, send_len);
	  if (res != SANE_STATUS_GOOD)
	    {
	      FDBG(DBG_SERIOUS, "failed to send cancel block: %d", res);
	      return res;
	    }
	}

      scan_state = SCANSTATE_IDLE;

      /*
       * Finally terminate the scan session.
       *
       */
      res = transport->Disconnect();

      if (res != SANE_STATUS_GOOD)
	{
	  FDBG(DBG_SERIOUS, "failed to disconnect: %d", res);
	  return res;
	}
    }

  ResetState(true);

  return SANE_STATUS_GOOD;
}
