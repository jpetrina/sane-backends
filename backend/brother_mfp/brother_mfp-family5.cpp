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
  FDBGNP(DBG_EVENT, "");

  const std::lock_guard<std::mutex> lock(mutex);

  SANE_Status res;

  res = transport->Acquire();
  if (res != SANE_STATUS_GOOD)
    {
      FDBG (DBG_SERIOUS, "failed to acquire: %d", res);
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
      FDBG (DBG_SERIOUS, "failed to init: %d", res);
      return res;
    }

  res = transport->Disconnect();
  if (res != SANE_STATUS_GOOD)
    {
      FDBG (DBG_SERIOUS, "failed to disconnect: %d", res);
      return res;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily5::Disconnect ()
{
  FDBGNP(DBG_EVENT, "");

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

    was_cancelled = false;

  /*
   * If IDLE then we need to kick off a completely new session.
   * There are a few things that we need to send to get things moving.
   *
   */
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

      res = StartScan_QueryScanInfo();
      if (res != SANE_STATUS_GOOD)
	{
          FDBG (DBG_SERIOUS, "failed to send query scan info block: %d", res);
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

  /*
   * Make sure that we signal that cancelling happened once
   * and only once.
   *
   */
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
      if ((res == SANE_STATUS_GOOD) && (*length == 0))	 // nothing to decode.
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
	      scan_state = SCANSTATE_IDLE;
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
       * More important to return CANCEL if we were cancelled.
       *
       */
      if (was_cancelled)
        {
          was_cancelled = false;
	  (void)CancelScan_Internal();
	  scan_state = SCANSTATE_IDLE;
          return SANE_STATUS_CANCELLED;
        }

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
       * and try again.
       *
       * We could release the lock while we're sleeping but we aren't sleeping
       * for very long and it would make things a bit more complicated. Let's not.
       * TODO: Maybe we should?
       *
       */
      if (transport->GetAsyncIOMode() || (*length != 0))
	{
	  break;
	}

      FDBGNP(DBG_DETAIL, "no read, so sleeping for a bit.");
      usleep(TIMEOUT_MILLISECS(100));
    }

  return res;
}

/*
 * Decode data from data_buffer and remove what we consumed.
 *
 */
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
      (void) memmove(data_buffer, data_buffer + data_consumed, data_buffer_bytes - data_consumed);
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
  FDBGNP(DBG_EVENT, "");

  /*
   * Do transport cancel before we take the mutex!!!
   * Otherwise, we will never actually abort anything since
   * all other operations hold the mutex as well and we will never
   * take the mutex while another operation is in progress.
   *
   * Note that the transport cancel just interrupts any currently
   * executing operation. It doesn't interact with the device.
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



SANE_Status BrotherFamily5::StartScan_QueryScanInfo ()
{
  SANE_Status res;

  FDBGNP (DBG_EVENT, "");

  size_t send_len = 0;
  res = EncodeQueryScanInfoBlock (small_send_buffer, sizeof(small_send_buffer), &send_len);
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to generate query scan info block: %d", res);
      return SANE_STATUS_INVAL;
    }

  size_t rec_len = sizeof(small_rec_buffer);
  useconds_t timeout = TIMEOUT_SECS(8);

  res = transport->SendAndReceive(small_send_buffer, send_len, small_rec_buffer, &rec_len, &timeout);
  if ((res != SANE_STATUS_GOOD) || (timeout == 0))
    {
      FDBG(DBG_SERIOUS, "failed to send query scan info block: %d", res);
      return res;
    }

  BrotherQueryScanInfoResponse query_scan_info_resp;

  res = DecodeQueryScanInfoBlockResp (small_rec_buffer, rec_len, query_scan_info_resp);
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to read query scan info block response: %d", res);
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
      FDBG(DBG_SERIOUS, "query block too long for buffer: %zu", *length);
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily5::DecodeQueryBlockResp(const SANE_Byte *data, size_t data_len,
						 BrotherQueryResponse &response)
{

  /*
   * Formatting and content checks.
   *
   */
  if ((data_len != 75) || (data[0] != BROTHER_DATA_QUERY_RESPONSE))

    {
      FDBG(DBG_SERIOUS, "invalid query response block: len = %zu", data_len);
      return SANE_STATUS_IO_ERROR;
    }

  /*
   * TODO: decode the response block to fill in the details.
   *
   */
  memset(&response, 0, sizeof(response));

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
      FDBG(DBG_SERIOUS, "invalid session response block: len = %zu", data_len);
      return SANE_STATUS_IO_ERROR;
    }

  /*
   * TODO: Check the valid status values are 0x00 or 0x20
   *
   */
  if ((data[4] != 0x00) && (data[4] != 0x80))

    {
      FDBG(DBG_SERIOUS, "invalid session response: data[4]=%u", (unsigned int ) data[4]);
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
SANE_Status BrotherFamily5::EncodeQueryScanInfoBlock (SANE_Byte *data, size_t data_len,
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
      FDBG(DBG_SERIOUS, "parameter block too long for buffer: %zu", *length);
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily5::DecodeQueryScanInfoBlockResp(const SANE_Byte *data, size_t data_len,
							 BrotherQueryScanInfoResponse &response)
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
      FDBG(DBG_SERIOUS, "parameter block too long for buffer: %zu", *length);
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily5::DecodeScanData(const SANE_Byte *src_data, size_t src_data_len,
					   size_t *src_data_consumed, SANE_Byte *dest_data,
					   size_t dest_data_len, size_t *dest_data_written)
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
      FDBGNP(DBG_DETAIL, "returning no_docs");
      return SANE_STATUS_NO_DOCS;
    }

  if (end_image_flag && !curr_reader)
    {
      FDBGNP(DBG_DETAIL, "returning eof");
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
	      FDBGNP(DBG_DETAIL, "READING AND WRITING SAME DECODER.");

	      size_t in_len = std::min(src_data_len, current_header.block_len);

	      ret_status = curr_writer->DecodeScanData(src_data, in_len, &bytes_consumed, dest_data,
						       dest_data_len, &bytes_written);
	    }
	  else if (curr_reader && curr_writer && (curr_writer != curr_reader))

	    {
	      FDBGNP(DBG_DETAIL, "READING AND WRITING DIFFERENT DECODER.");

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
	      FDBGNP(DBG_DETAIL, "READING ONLY: DRAINING.");

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

	  /*
	   * Quick sanity check.
	   *
	   * We cannot consume more data than is in the current block.
	   *
	   */
	  if (current_header.block_len < bytes_consumed)
	    {
		  FDBG(DBG_SERIOUS, "ERROR: we consumed more data than is in the block.=%zu",
		       current_header.block_len);
		  ret_status = SANE_STATUS_IO_ERROR;
	    }

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

	  if ((end_image_flag || (decoders.size() > 1) || (ret_status == SANE_STATUS_EOF))
	      && !curr_reader->HasUnprocessedDecodeData())
	    {
	      decoders.pop_front();	// remove current reader.

	      FDBG(DBG_DETAIL, "DELETING DRAINED decoder. Now there are %zu decoders.", decoders.size());

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
	      FDBGNP(DBG_DETAIL, "exiting with GOOD: end of image seen, but we wrote some data.");
	      return SANE_STATUS_GOOD;	// cached data was available.
	    }
	  else
	    {
	      if (!decoders.empty ())
		{
		  FDBGNP(
		      DBG_DETAIL,
		      "exiting with GOOD: end of image seen, but there is cached data, but no output space.");
		  return SANE_STATUS_GOOD;	// have cached data, but no output buffer space.
		}
	      else
		{
		  FDBGNP(DBG_DETAIL, "exiting with ENDOFDATA: end of image and decoder flushed.");
		  return out_of_docs? SANE_STATUS_NO_DOCS: SANE_STATUS_EOF;	// All done.
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

	  if (ret_status != SANE_STATUS_GOOD)
	    {
	      /*
	       * All other statii are terminal. Something went badly wrong.
	       * Probably an error condition on the device, like paper jam.
	       *
	       */
	      FDBGNP(DBG_SERIOUS, "read error status from header read.");
	      break;
	    }
	  if (header_consumed == 0)
	    {
	      // This is OK. we just need more data from the device probably.
	      FDBGNP(DBG_DETAIL, "need decode header but need more data.");
	      return SANE_STATUS_GOOD;
	    }

	  src_data += header_consumed;
	  src_data_len -= header_consumed;
	  *src_data_consumed += header_consumed;

	  /*
	   * First: aborts. Scan won't finish so we need to stop immediately.
	   * Can't rely on scanning coming to a normal conclusion so we delete
	   * the decoders.
	   *
	   */
	  if ((current_header.block_type == BROTHER_DATA_BLOCK_PROT_CANCEL_5) ||
	      (current_header.block_type == BROTHER_DATA_BLOCK_DEV_CANCEL_5))
	    {
	      end_image_flag = true;
	      out_of_docs = true;
	      was_cancelled = true;

	      decoders.clear();
	    }
	  else if (current_header.block_type == BROTHER_DATA_BLOCK_END_OF_FRAME_5)
	    {
	      FDBGNP(DBG_DETAIL, "got frame termination header.");

	      end_image_flag = true;
	    }
	  else if (current_header.block_type == BROTHER_DATA_BLOCK_END_OF_ALL_FRAMES_5)
	    {
	      FDBGNP(DBG_DETAIL, "got end of frame ALL.");
	      end_image_flag = true;
	      out_of_docs = true;
	    }
	  else
	    {
	      /*
	       * Create a new decoder, since this is the start of a new data block.
	       * But only if we have a block for a different type of data.
	       *
	       */
	      BrotherScanDataDecoder *new_decoder = nullptr;
	      if ((last_block_type != current_header.block_type) || !curr_writer)
		{
		  if (current_header.block_type == BROTHER_DATA_BLOCK_JPEG5)
		    {
		      new_decoder = new BrotherJFIFDecoder(scan_params);
		      FDBGNP(DBG_EVENT, "CREATED JFIF DECODER.");
		    }
		  else if (current_header.block_type == BROTHER_DATA_BLOCK_RLENGTH5)
		    {
		      new_decoder = new BrotherGrayRLengthDecoder(scan_params);
		      FDBGNP(DBG_EVENT, "CREATED GRAY RLENGTH DECODER.");
		    }
		  else
		    {
		      FDBG(DBG_SERIOUS, "detected unrecognised data block type [%d].",
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
		  FDBGNP(DBG_DETAIL, "using existing writer: no change to type.");
		}
	    }
	}
      else if (!bytes_consumed)
	{
	  FDBG(DBG_DETAIL,
	       "no bytes were consumed. Try for more. src_data_len=%zu dest_data_len=%zu",
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
	  FDBGNP (DBG_DETAIL, "read no_docs header.");
	  return SANE_STATUS_NO_DOCS;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_PAPER_JAM_5)
	{
	  FDBGNP (DBG_DETAIL, "read no_paper jam header.");
	  return SANE_STATUS_JAMMED;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_BUSY_ERR)
	{
	  FDBGNP (DBG_DETAIL, "read device busy header.");
	  return SANE_STATUS_DEVICE_BUSY;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_PROT_ERR)
	{
	  FDBGNP (DBG_DETAIL, "read protocol error header.");
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

	  FDBG (DBG_DETAIL, "read end of frame header [0x%2.2x].", header.block_type);

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

	  FDBG (DBG_DETAIL, "read scan termination code [0x%2.2x].", header.block_type);

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
       * OK we can be here in a few scenarios:
       *
       * 1) We might need to send a cancel command. The situations where this is necessary are:
       *   a) We are in SCANSTATE_EOF state which means we got the end of an image and the device
       *      wants to set more images. We will know this because it will try to send us more data.
       *      For this case, we need to read something further to know what to do. We will poll
       *      read for a while to see what turns up.
       *   b) We are in SCANSTATE_ACQUIRING which means that the frontend wants us to abort the current
       *      image.
       *
       * 2) If we are SCANSTATE_IDLE, then there is probably nothing we need to do.
       *
       * As it happens, it is OK to send ABT at any time before the final "NO MORE DOCS"
       * response is sent so if we are not IDLE then we can send it.
       *
       */
      if (scan_state != SCANSTATE_IDLE)
	{
	  res = EncodeCancelBlock(small_send_buffer, sizeof(small_send_buffer), &send_len);

	  if (res != SANE_STATUS_GOOD)
	    {
	      FDBG(DBG_SERIOUS, "failed to generate cancel block: %d", res);
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
