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
#define BROTHER_DATA_BLOCK_END_OF_IMAGE		0x82
#define BROTHER_DATA_BLOCK_DATA_TRUNC_EARLY	0x83	// backend cancel
#define BROTHER_DATA_BLOCK_CANCEL               0x86	// stop button pressed

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

SANE_Status BrotherFamily234Base::Connect()
{
  FDBGNP(DBG_EVENT, "");

  const std::lock_guard<std::mutex> lock(mutex);

  SANE_Status res;

  res = transport->Acquire();
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to acquire: %d", res);
      return res;
    }

  res = transport->Connect();
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to connect: %d", res);
      return res;
    }

  res = Init();
  if (res != SANE_STATUS_GOOD)
    {
      (void) transport->Disconnect();
      FDBG(DBG_SERIOUS, "failed to init: %d", res);
      return res;
    }

  res = transport->Disconnect();
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to disconnect: %d", res);
      return res;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::Disconnect()
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

SANE_Status BrotherFamily234Base::StartScan()
{
  FDBGNP(DBG_EVENT, "");

  const std::lock_guard<std::mutex> lock(mutex);

  SANE_Status res;

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
	  FDBG(DBG_SERIOUS, "failed to connect: %d", res);
	  (void) CancelScan_Internal();
	  return res;
	}

      res = StartScan_SelectSource();
      if (res != SANE_STATUS_GOOD)
	{
	  FDBG(DBG_SERIOUS, "failed to send source select block: %d", res);
	  (void) CancelScan_Internal();
	  return res;
	}

      res = StartScan_QueryScanInfo();
      if (res != SANE_STATUS_GOOD)
	{
	  FDBG(DBG_SERIOUS, "failed to send query scan info parameter block: %d", res);
	  (void) CancelScan_Internal();
	  return res;
	}

      res = StartScan_Parameter();
      if (res != SANE_STATUS_GOOD)
	{
	  FDBG(DBG_SERIOUS, "failed to send parameter block: %d", res);
	  (void) CancelScan_Internal();
	  return res;
	}
    }
  else
    {
      if (out_of_docs)
	{
	  FDBGNP(DBG_EVENT, "sending NO_DOCS. There are no docs left.");
	  return SANE_STATUS_NO_DOCS;
	}

      /*
       * FAMILY_4 doesn't require this. It just sends the next frame.
       *
       */
      if (family_type != BROTHER_FAMILY_4)
	{
	  res = StartScan_ParameterBlank();
	  if (res != SANE_STATUS_GOOD)
	    {
	      FDBG(DBG_SERIOUS, "failed to send blank parameter block: %d", res);
	      (void) CancelScan_Internal();
	      return res;
	    }
	}
      ResetState(false);
    }
  scan_state = SCANSTATE_ACQUIRING;

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::ReadData(SANE_Byte *data, size_t max_length, size_t *length)
{
  FDBGNP(DBG_EVENT, "");

  const std::lock_guard<std::mutex> lock(mutex);

  *length = 0;

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
SANE_Status BrotherFamily234Base::DecodeReadBuffer(SANE_Byte *data, size_t max_length,
						   size_t *length)
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
      (void)memmove(data_buffer, data_buffer + data_consumed, data_buffer_bytes - data_consumed);
      data_buffer_bytes -= data_consumed;
    }

  FDBG(DBG_DETAIL, "decoder consumes %zu bytes and writes %zu bytes, returning %s", data_consumed,
       *length, sane_strstatus(dec_ret));

  return dec_ret;
}

SANE_Status BrotherFamily234Base::SetAsyncIOMode(bool async_io)
{
  FDBGNP(DBG_EVENT, "");

  const std::lock_guard<std::mutex> lock(mutex);

  if (scan_state != SCANSTATE_ACQUIRING)
    {
      return SANE_STATUS_INVAL;
    }

  return transport->SetAsyncIOMode(async_io);
}

void BrotherFamily234Base::Cancel()
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

SANE_Status BrotherFamily234Base::Init()
{
  FDBGNP(DBG_EVENT, "");

  SANE_Status res;

  /*
   * Execute Query request.
   *
   */
  FDBGNP(DBG_EVENT, "Sending QUERY request.");

  size_t send_len = 0;
  SANE_Status dec_ret = EncodeQueryBlock(small_send_buffer, sizeof(small_send_buffer), &send_len);
  if (dec_ret != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to generate query block: %d", dec_ret);
      return SANE_STATUS_INVAL;
    }

  size_t rec_len = sizeof(small_rec_buffer);
  useconds_t timeout = TIMEOUT_SECS(8);

  res = transport->SendAndReceive(small_send_buffer, send_len, small_rec_buffer, &rec_len,
				  &timeout);
  if ((res != SANE_STATUS_GOOD) || (timeout == 0))
    {
      FDBG(DBG_SERIOUS, "failed to send query block: %d", res);
      return res;
    }

  BrotherQueryResponse query_resp;

  res = DecodeQueryBlockResp(small_rec_buffer, rec_len, query_resp);
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to read query block response: %d", res);
      return res;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::StartScan_SelectSource()
{
  SANE_Status res;

  FDBGNP(DBG_EVENT, "");

  size_t send_len = 0;
  res = EncodeSourceSelectBlock(small_send_buffer, sizeof(small_send_buffer), &send_len);
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to source select block: %d", res);
      return SANE_STATUS_INVAL;
    }

  size_t rec_len = sizeof(small_rec_buffer);
  useconds_t timeout = TIMEOUT_SECS(8);

  res = transport->SendAndReceive(small_send_buffer, send_len, small_rec_buffer, &rec_len,
				  &timeout);
  if ((res != SANE_STATUS_GOOD) || (timeout == 0))
    {
      FDBG(DBG_SERIOUS, "failed to send source select block: %d", res);
      return res;
    }

  BrotherSourceStatusResponse source_select_resp;

  res = DecodeSourceSelectBlockResp(small_rec_buffer, rec_len, source_select_resp);
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to read source select block response: %d", res);
      return res;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::StartScan_QueryScanInfo()
{
  SANE_Status res;

  FDBGNP(DBG_EVENT, "");

  size_t send_len = 0;
  res = EncodeQueryScanInfoBlock(small_send_buffer, sizeof(small_send_buffer), &send_len);
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to generate query scan info block: %d", res);
      return SANE_STATUS_INVAL;
    }

  size_t rec_len = sizeof(small_rec_buffer);
  useconds_t timeout = TIMEOUT_SECS(8);

  res = transport->SendAndReceive(small_send_buffer, send_len, small_rec_buffer, &rec_len,
				  &timeout);
  if ((res != SANE_STATUS_GOOD) || (timeout == 0))
    {
      FDBG(DBG_SERIOUS, "failed to send query scan info block: %d", res);
      return res;
    }

  BrotherQueryScanInfoResponse query_scan_info_resp;

  res = DecodeQueryScanInfoBlockResp(small_rec_buffer, rec_len, query_scan_info_resp);
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to read query scan info block response: %d", res);
      return res;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::StartScan_Parameter()
{
  SANE_Status res;

  FDBGNP(DBG_EVENT, "");

  size_t send_len = 0;
  res = EncodeParameterBlock(small_send_buffer, sizeof(small_send_buffer), &send_len);
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to generate parameter block: %d", res);
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

SANE_Status BrotherFamily234Base::StartScan_ParameterBlank()
{
  SANE_Status res;

  FDBGNP(DBG_EVENT, "");

  size_t send_len = 0;
  res = EncodeParameterBlockBlank(small_send_buffer, sizeof(small_send_buffer), &send_len);
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to generate param blank block: %d", res);
      return SANE_STATUS_INVAL;
    }

  res = transport->SendCommand(small_send_buffer, send_len);
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to send parameter blank block: %d", res);
      return res;
    }

  return SANE_STATUS_GOOD;
}

const char* BrotherFamily234Base::ScanModeToText(BrotherScanMode scan_mode)
{
  static const char *scan_mode_text[] = { "CGRAY", "GRAY64", "ERRDIF", "TEXT" };  // also C256?

  return scan_mode_text[scan_mode];
}

SANE_Status BrotherFamily234Base::EncodeQueryBlock(SANE_Byte *data, size_t data_len, size_t *length)
{
  *length = snprintf((char*)data, data_len, "\x1b" "\x51" "\x0a" "\x80");

  if (*length > data_len)
    {
      FDBG(DBG_SERIOUS, "query block too long for buffer: %zu", *length);
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::DecodeSessionResp(const SANE_Byte *data, size_t data_len,
						    BrotherSessionResponse &response)
{
  /*
   * Formatting and content checks.
   *
   *
   */
  if ((data_len != 5)
      || ((memcmp(data, "\x05" "\x10" "\x01" "\x02", 4) != 0)
	  && (memcmp(data, "\x05" "\x10" "\x02" "\x02", 4) != 0)))

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
SANE_Status BrotherFamily234Base::EncodeQueryScanInfoBlock(SANE_Byte *data, size_t data_len,
							   size_t *length)
{
  const char *mode_text = ScanModeToText(scan_params.param_scan_mode);
  if (nullptr == mode_text)
    {
      FDBG(DBG_SERIOUS, "failed to get scan mode text for mode %d", scan_params.param_scan_mode);
      return SANE_STATUS_INVAL;
    }

  *length = snprintf((char*)data, data_len, "\x1b" "I\nR=%u,%u\nM=%s\n" "\x80",
		     (unsigned int)scan_params.param_x_res, (unsigned int)scan_params.param_y_res,
		     mode_text);

  if (*length > data_len)
    {
      FDBG(DBG_SERIOUS, "parameter block too long for buffer: %zu", *length);
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::DecodeQueryScanInfoBlockResp(
    const SANE_Byte *data, size_t data_len, BrotherQueryScanInfoResponse &response)
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

SANE_Status BrotherFamily234Base::EncodeSourceStatusBlock(SANE_Byte *data, size_t data_len,
							  size_t *length)
{
  *length = snprintf((char*)data, data_len, "\x1b" "D\nADF\n" "\x80");

  if (*length > data_len)
    {
      FDBG(DBG_SERIOUS, "Source status block too long for buffer: %zu", *length);
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::DecodeSourceStatusBlockResp(const SANE_Byte *data,
							      size_t data_len,
							      BrotherSourceStatusResponse &response)
{
  if (data_len != 1)
    {
      FDBG(DBG_SERIOUS, "source status response invalid: %zu", data_len);
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
      FDBG(DBG_SERIOUS, "Source select block too long for buffer: %zu", *length);
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::DecodeSourceSelectBlockResp(const SANE_Byte *data,
							      size_t data_len,
							      BrotherSourceStatusResponse &response)
{
  if (data_len != 1)
    {
      FDBG(DBG_SERIOUS, "source select response invalid: %zu", data_len);
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

SANE_Status BrotherFamily234Base::EncodeCancelBlock(SANE_Byte *data, size_t data_len,
						    size_t *length)
{
  *length = snprintf((char*)data, data_len, "\x1b" "R");

  if (*length > data_len)
    {
      FDBG(DBG_SERIOUS, "cancel block too long for buffer: %zu", *length);
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::DecodeScanData(const SANE_Byte *src_data, size_t src_data_len,
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
	      FDBGNP(DBG_DEBUG, "READING AND WRITING SAME DECODER.");

	      size_t in_len = std::min(src_data_len, current_header.block_len);

	      ret_status = curr_writer->DecodeScanData(src_data, in_len, &bytes_consumed, dest_data,
						       dest_data_len, &bytes_written);
	    }
	  else if (curr_reader && curr_writer && (curr_writer != curr_reader))

	    {
	      FDBGNP(DBG_DEBUG, "READING AND WRITING DIFFERENT DECODER.");

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
	      FDBGNP(DBG_DEBUG, "READING ONLY: DRAINING.");

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
	      FDBG(DBG_SERIOUS, "ERROR: we consumed more data than is in the block=%zu",
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

	      FDBG(DBG_DETAIL, "DELETING DRAINED decoder. Now there are %zu decoders.",
		   decoders.size());

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
	      if (!decoders.empty())
		{
		  FDBGNP(
		      DBG_DETAIL,
		      "exiting with GOOD: end of image seen, but there is cached data, but no output space.");
		  return SANE_STATUS_GOOD;	// have cached data, but no output buffer space.
		}
	      else
		{
		  /*
		   * Unlike in FAMILY_5, don't return NO_DOCS here.
		   * This is because we don't get a separate notification from the scanner
		   * after the image termination code. The "end of all scanning: *is* the final
		   * image termination code. We set out_of_docs to true though so if the
		   * frontend calls read() again, it will get the NO_DOCS status then.
		   *
		   * Except for FAMILY_4 of course.
		   *
		   */
		  FDBGNP(DBG_DETAIL, "exiting with ENDOFDATA: end of image and decoder flushed.");
		  if (family_type == BROTHER_FAMILY_4)
		    {
		      return out_of_docs? SANE_STATUS_NO_DOCS: SANE_STATUS_EOF;
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
	  if ((current_header.block_type == BROTHER_DATA_BLOCK_DATA_TRUNC_EARLY)
	      || (current_header.block_type == BROTHER_DATA_BLOCK_CANCEL))
	    {
	      end_image_flag = true;
	      out_of_docs = true;
	      was_cancelled = true;

	      decoders.clear();
	    }
	  else if (current_header.block_type == BROTHER_DATA_BLOCK_END_OF_FRAME_MORE)
	    {
	      FDBGNP(DBG_DETAIL, "got frame termination header.");

	      end_image_flag = true;
	    }
	  else if (current_header.block_type == BROTHER_DATA_BLOCK_SCAN_FINISHED)
	    {
	      FDBGNP(DBG_DETAIL, "got end of frame ALL.");
	      end_image_flag = true;
	      out_of_docs = true;
	    }
	  else if (current_header.block_type == BROTHER_DATA_BLOCK_END_OF_IMAGE)
	    {
	      FDBGNP(DBG_DETAIL, "got end of frame block.");
	      end_image_flag = true;
	    }
	  else
	    {
	      /*
	       * Create a new decoder, since this is the start of a new data block.
	       * But only if we have a block for a different type of data.
	       *
	       */
	      BrotherScanDataDecoder *new_decoder = nullptr;

	      /*
	       * Figure out the new block type.
	       *
	       */
	      BlockCategory block_category = BLOCKCATEGORY_NONE;
	      switch (current_header.block_type)
		{
		case BROTHER_DATA_BLOCK_JPEG:
		  block_category = BLOCKCATEGORY_JPEG;
		  break;

		case BROTHER_DATA_BLOCK_GRAY_RLENGTH:
		  block_category = BLOCKCATEGORY_GRAY_RLENGTH;
		  break;

		case BROTHER_DATA_BLOCK_GRAY_RAW:
		  block_category = BLOCKCATEGORY_GRAY_RAW;
		  break;

		case BROTHER_DATA_BLOCK_RED_RAW:
		case BROTHER_DATA_BLOCK_GREEN_RAW:
		case BROTHER_DATA_BLOCK_BLUE_RAW:
		  block_category = BLOCKCATEGORY_COLOUR_RAW;
		  break;

		default:
		  break;
		}

	      if ((last_block_category != block_category) || !curr_writer)
		{
		  switch (block_category)
		    {
		    case BLOCKCATEGORY_JPEG:
		      new_decoder = new BrotherJFIFDecoder(scan_params);
		      FDBGNP(DBG_EVENT, "CREATED JFIF DECODER.");
		      break;

		    case BLOCKCATEGORY_GRAY_RLENGTH:
		      new_decoder = new BrotherGrayRLengthDecoder(scan_params);
		      FDBGNP(DBG_EVENT, "CREATED GRAY RLENGTH DECODER.");
		      break;

		    case BLOCKCATEGORY_GRAY_RAW:
		      new_decoder = new BrotherGrayRawDecoder(scan_params);
		      FDBGNP(DBG_EVENT, "CREATED GRAY DECODER.");
		      break;

		    case BLOCKCATEGORY_COLOUR_RAW:
		      new_decoder = new BrotherInterleavedRGBColourDecoder(scan_params,
									   capabilities);
		      FDBGNP(DBG_EVENT, "CREATED RGB COLOUR DECODER.");
		      break;

		    default:
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
		  decoders.push_back(std::unique_ptr<BrotherScanDataDecoder>(new_decoder));
		  curr_writer = new_decoder;
		  if (!curr_reader)
		    {
		      curr_reader = new_decoder;
		    }
		  last_block_category = block_category;
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
  FDBG(DBG_WARN, "exiting with error: %d(%s).", ret_status, sane_strstatus(ret_status));

  return ret_status;
}

SANE_Status BrotherFamily234Base::DecodeButtonQueryResp(const SANE_Byte *data, size_t data_len,
							BrotherButtonQueryResponse &response)
{
  /* family4 sometimes begins with a 0x05 */
  if ((data_len != 4) || (data[0] != 0x05 && data[0] != 0x04) || (data[1] != 0x10)
      || (data[2] != 0x03))
    {
      return SANE_STATUS_IO_ERROR;
    }

  response.has_button_press = data[3] == 0x10;

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::DecodeButtonStateResp(const SANE_Byte *data, size_t data_len,
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
      FDBG(DBG_WARN, "unknown button code: %d.", data[4]);
      return SANE_STATUS_UNSUPPORTED;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::CancelScan_Internal()
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
       * If we are not IDLE, then for the most part we can send the 'R' cancel code to stop
       * scanning. However, we must not send it when the final 0x80 end of scanning has been sent
       * which terminates the final image. If we do, the scanner might get in a weird state
       * and doesn't send the response to us. However, when we get 0x80, we set out_of_docs so
       * we can check for that.
       *
       * Note that there is a subtle difference between the 2/3/4 families and family 5.
       *
       */
      if ((scan_state != SCANSTATE_IDLE) && !out_of_docs)
	{

	  /*
	   * If !out_of_docs, that means we have not seen the code that says that
	   * we have finished everything. However, we don't know if there will be
	   * more documents to come. We might get 0x80 termination or we might get
	   * another frame. However, if we cancel when we are not receiving a frame
	   * then devices generally don't like this. We will wait for a bit to see if we
	   * get more data to decide. It will either be start packet for the next frame or
	   * the end of images code 0x80.
	   *
	   */
	  size_t rec_len = sizeof(small_rec_buffer);
	  useconds_t timeout = TIMEOUT_SECS(8);

	  res = transport->ReceiveResponse(small_rec_buffer, &rec_len, &timeout);
	  if ((res == SANE_STATUS_GOOD) && (timeout != 0))
	    {
	      if ((rec_len > 0) && (small_rec_buffer[0] != BROTHER_DATA_BLOCK_SCAN_FINISHED))
		{
		  res = EncodeCancelBlock(small_send_buffer, sizeof(small_send_buffer), &send_len);
		  if (res == SANE_STATUS_GOOD)
		    {
		      FDBGNP(DBG_EVENT, "Sending cancel sequence.");

		      res = transport->SendCommand(small_send_buffer, send_len);
		      if (res != SANE_STATUS_GOOD)
			{
			  FDBG(DBG_SERIOUS, "failed to send cancel/receive response block: %d",
			       res);
			}
		    }
		}
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

SANE_Status BrotherFamily234Base::EncodeParameterBlockBlank(SANE_Byte *data, size_t data_len,
							    size_t *length)
{
  const char *mode_text = ScanModeToText(scan_params.param_scan_mode);
  if (nullptr == mode_text)
    {
      FDBG(DBG_SERIOUS, "failed to get scan mode text for mode %d", scan_params.param_scan_mode);
      return SANE_STATUS_INVAL;
    }

  *length = snprintf((char*)data, data_len, "\x1b" "X\n\x80");

  if (*length > data_len)
    {
      FDBG(DBG_SERIOUS, "parameter block too long for buffer: %zu", *length);
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::DecodeQueryBlockResp(const SANE_Byte *data, size_t data_len,
						       BrotherQueryResponse &response)
{
  memset(&response, 0, sizeof(response));

  /*
   * Formatting and content checks.
   *
   */
  if ((data_len < 16) || (data[0] != BROTHER_DATA_QUERY_RESPONSE))
    {
      FDBG(DBG_SERIOUS, "invalid query response block: len = %zu", data_len);
      return SANE_STATUS_IO_ERROR;
    }

  response.info_size = data[2];
  if ((size_t)(response.info_size + 2) != data_len)
    {
      FDBG(DBG_SERIOUS, "invalid query response block length: len = %zu", data_len);
      return SANE_STATUS_IO_ERROR;
    }

  /*
   * TODO: decode the response block to fill in the details.
   *
   */

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherFamily234Base::ReadButtons(SANE_Int &button_mask)
{
  return transport->ReadButtons(button_mask);
}


//==================Brother 2============================================================
SANE_Status BrotherFamily2::DecodeScanDataHeader(const SANE_Byte *src_data, size_t src_data_len,
						 size_t *src_data_consumed, ScanDataHeader &header)
{
  FDBGNP(DBG_EVENT, "");

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
	  FDBGNP(DBG_DETAIL, "read no_docs header.");
	  return SANE_STATUS_NO_DOCS;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_PAPER_JAM)
	{
	  FDBGNP(DBG_DETAIL, "read no_paper jam header.");
	  return SANE_STATUS_JAMMED;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_COVER_OPEN)
	{
	  FDBGNP(DBG_DETAIL, "cover open header.");
	  return SANE_STATUS_COVER_OPEN;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_SERVICE_ERR)
	{
	  FDBGNP(DBG_DETAIL, "service error header.");
	  return SANE_STATUS_IO_ERROR;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_DEVICE_STUCK)
	{
	  FDBGNP(DBG_DETAIL, "device stuck header.");
	  return SANE_STATUS_IO_ERROR;
	}

      // Don't know this one!
      FDBG(DBG_SERIOUS, "unknown error status encountered: 0x%2.2x",
	   (unsigned int ) header.block_type);

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
	  FDBG(DBG_DETAIL, "read scan finished header [0x%2.2x].", header.block_type);
	  *src_data_consumed = 1;
	  return SANE_STATUS_GOOD;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_END_OF_FRAME_MORE)
	{
	  FDBG(DBG_DETAIL, "read scan finished/no more header [0x%2.2x].", header.block_type);
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

      FDBG(DBG_DETAIL, "read scan data header [0x%2.2x].", header.block_type);
      return SANE_STATUS_GOOD;
    }

  /*
   * Unknown data block type.
   *
   */
  FDBG(DBG_SERIOUS, "unknown data block type encountered: 0x%2.2x",
       (unsigned int ) header.block_type);

  return SANE_STATUS_UNSUPPORTED;
}

SANE_Status BrotherFamily2::EncodeParameterBlock(SANE_Byte *data, size_t data_len, size_t *length)
{
  const char *mode_text = ScanModeToText(scan_params.param_scan_mode);
  if (nullptr == mode_text)
    {
      FDBG(DBG_SERIOUS, "failed to get scan mode text for mode %d", scan_params.param_scan_mode);
      return SANE_STATUS_INVAL;
    }

  const char *format_string = "\x1b"
			      "X\nR=%u,%u\nM=%s\n" /* resolution, color mode */
			      "%s\n" /* compression */
			      "B=%u\nN=%u\n" /* brightness, contrast */
			      "A=%u,%u,%u,%u\n" /* scan area */
			      "D=%s\n" /* single / duplex */
			      "\x80";

  *length = snprintf(
	  (char*) data,
	  data_len,
	  format_string,
	  (unsigned int) scan_params.param_x_res,
	  (unsigned int) scan_params.param_y_res,
	  mode_text,
	  (scan_params.param_scan_mode == BROTHER_SCAN_MODE_COLOR) ?
	      (scan_params.param_compression ? "C=JPEG\nJ=MID" : "C=NONE") : "C=RLENGTH",
	  (unsigned int) (scan_params.param_brightness + 50),
	  (unsigned int) (scan_params.param_contrast + 50),
	  (unsigned int) (scan_params.param_pixel_x_offset),
	  (unsigned int) (scan_params.param_pixel_y_offset),
	  (unsigned int) (scan_params.param_pixel_x_offset + scan_params.param_pixel_x_width),
	  (unsigned int) (scan_params.param_pixel_y_offset + scan_params.param_pixel_y_height),
	  scan_params.param_source == BROTHER_SOURCE_ADF_DUPLEX ? "DUP" : "SIN");

  if (*length > data_len)
    {
      FDBG(DBG_SERIOUS, "parameter block too long for buffer: %zu", *length);
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}

//==================Brother 3============================================================

SANE_Status BrotherFamily3::DecodeScanDataHeader(const SANE_Byte *src_data, size_t src_data_len,
						 size_t *src_data_consumed, ScanDataHeader &header)
{
  FDBGNP(DBG_EVENT, "");

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
	  FDBGNP(DBG_DETAIL, "read no_docs header.");
	  return SANE_STATUS_NO_DOCS;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_PAPER_JAM)
	{
	  FDBGNP(DBG_DETAIL, "read no_paper jam header.");
	  return SANE_STATUS_JAMMED;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_COVER_OPEN)
	{
	  FDBGNP(DBG_DETAIL, "cover open header.");
	  return SANE_STATUS_COVER_OPEN;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_SERVICE_ERR)
	{
	  FDBGNP(DBG_DETAIL, "service error header.");
	  return SANE_STATUS_IO_ERROR;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_DEVICE_STUCK)
	{
	  FDBGNP(DBG_DETAIL, "device stuck header.");
	  return SANE_STATUS_IO_ERROR;
	}

      // Don't know this one!
      FDBG(DBG_SERIOUS, "unknown error status encountered: 0x%2.2x",
	   (unsigned int ) header.block_type);

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
      if (header.block_type == BROTHER_DATA_BLOCK_DATA_TRUNC_EARLY)
	{
	  FDBG(DBG_DETAIL, "read data truncated early (backend cancel) code [0x%2.2x].",
	       header.block_type);
	  *src_data_consumed = 1;
	  return SANE_STATUS_CANCELLED;
	}

      if (header.block_type == BROTHER_DATA_BLOCK_CANCEL)
	{
	  FDBG(DBG_DETAIL, "read device cancel code [0x%2.2x].", header.block_type);
	  *src_data_consumed = 1;
	  return SANE_STATUS_CANCELLED;
	}

      if (header.block_type == BROTHER_DATA_BLOCK_SCAN_FINISHED)
	{
	  FDBG(DBG_DETAIL, "read scan finished header [0x%2.2x].", header.block_type);
	  *src_data_consumed = 1;
	  return SANE_STATUS_GOOD;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_END_OF_FRAME_MORE)
	{
	  FDBG(DBG_DETAIL, "read scan finished/no more header [0x%2.2x].", header.block_type);
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

      FDBG(DBG_DETAIL, "read scan data header [0x%2.2x].", header.block_type);
      return SANE_STATUS_GOOD;
    }

  /*
   * Unknown data block type.
   *
   */
  FDBG(DBG_SERIOUS, "unknown data block type encountered: 0x%2.2x",
       (unsigned int ) header.block_type);

  return SANE_STATUS_UNSUPPORTED;
}

SANE_Status BrotherFamily3::EncodeParameterBlock(SANE_Byte *data, size_t data_len, size_t *length)
{
  const char *mode_text = ScanModeToText(scan_params.param_scan_mode);
  if (nullptr == mode_text)
    {
      FDBG(DBG_SERIOUS, "failed to get scan mode text for mode %d", scan_params.param_scan_mode);
      return SANE_STATUS_INVAL;
    }

  const char *format_string = "\x1b"
			      "X\nR=%u,%u\nM=%s\n" /* resolution, color mode */
			      "%s\n" /* compression */
			      "B=%u\nN=%u\n" /* brightness, contrast */
			      "A=%u,%u,%u,%u\n" /* scan area */
			      "D=%s\n" /* single / duplex */
//			      "U=OFF\nP=OFF\n"		// TODO: perhaps we don't need this?
			      "\x80";
  *length = snprintf(
      (char*) data,
      data_len,
      format_string,
      (unsigned int) scan_params.param_x_res,
      (unsigned int) scan_params.param_y_res,
      mode_text,
      (scan_params.param_scan_mode == BROTHER_SCAN_MODE_COLOR) ?
	  (scan_params.param_compression ? "C=JPEG\nJ=MID" : "C=NONE") : "C=RLENGTH",
      (unsigned int) (scan_params.param_brightness + 50),
      (unsigned int) (scan_params.param_contrast + 50),
      (unsigned int) (scan_params.param_pixel_x_offset),
      (unsigned int) (scan_params.param_pixel_y_offset),
      (unsigned int) (scan_params.param_pixel_x_offset + scan_params.param_pixel_x_width),
      (unsigned int) (scan_params.param_pixel_y_offset + scan_params.param_pixel_y_height),
      scan_params.param_source == BROTHER_SOURCE_ADF_DUPLEX ? "DUP" : "SIN");

  if (*length > data_len)
    {
      FDBG(DBG_SERIOUS, "parameter block too long for buffer: %zu", *length);
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}

//==================Brother 4============================================================

SANE_Status BrotherFamily4::DecodeScanDataHeader(const SANE_Byte *src_data, size_t src_data_len,
						 size_t *src_data_consumed, ScanDataHeader &header)
{
  FDBGNP(DBG_EVENT, "");

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
	  FDBGNP(DBG_DETAIL, "read no_docs header.");
	  return SANE_STATUS_NO_DOCS;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_PAPER_JAM)
	{
	  FDBGNP(DBG_DETAIL, "read no_paper jam header.");
	  return SANE_STATUS_JAMMED;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_COVER_OPEN)
	{
	  FDBGNP(DBG_DETAIL, "cover open header.");
	  return SANE_STATUS_COVER_OPEN;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_SERVICE_ERR)
	{
	  FDBGNP(DBG_DETAIL, "service error header.");
	  return SANE_STATUS_IO_ERROR;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_DEVICE_STUCK)
	{
	  FDBGNP(DBG_DETAIL, "device stuck header.");
	  return SANE_STATUS_IO_ERROR;
	}

      // Don't know this one!
      FDBG(DBG_SERIOUS, "unknown error status encountered: 0x%2.2x",
	   (unsigned int ) header.block_type);

      return SANE_STATUS_UNSUPPORTED;
    }

  /*
   * Normal status indications.
   *
   * 0x80 - no frames left
   * 0x82 07 00 nn nn 84 88 00 00 00 xx xx:
   * 	- "nn nn" is frame number
   *	- "xx xx" is block length (bytes to follow)
   *
   * Both values are 2-byte little-endian.
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
	  FDBG(DBG_DETAIL, "read scan finished header [0x%2.2x].", header.block_type);
	  *src_data_consumed = 1;
	  return SANE_STATUS_GOOD;
	}
      if (header.block_type == BROTHER_DATA_BLOCK_END_OF_IMAGE)
	{
	  if (src_data_len < 10)
	    {
	      return SANE_STATUS_GOOD;	// not enough data yet.
	    }
	  *src_data_consumed = 10;
	  header.image_number = src_data[3] + (src_data[4] << 8);
	  FDBG(DBG_DETAIL, "read end of image [0x%2.2x] image[%d].", header.block_type,
	       header.image_number);
	  return SANE_STATUS_GOOD;
	}
    }

  /*
     * Other data-carrying packets.
     *
   */
  if (src_data[0] & 0x40)
    {
      if (src_data_len < 12)
	{
	  return SANE_STATUS_GOOD;	// truncated, more to come.
	}

      *src_data_consumed = 12;

      header.block_type = src_data[0];
      header.image_number = src_data[3] + (src_data[4] << 8);
      header.block_len = src_data[10] + (src_data[11] << 8);

      FDBG(DBG_DETAIL, "read scan data header [0x%2.2x].", header.block_type);
      return SANE_STATUS_GOOD;
    }

  /*
   * Unknown data block type.
   *
   */
  FDBG(DBG_SERIOUS, "unknown data block type encountered: 0x%2.2x",
       (unsigned int ) header.block_type);

  return SANE_STATUS_UNSUPPORTED;
}

SANE_Status BrotherFamily4::EncodeParameterBlock(SANE_Byte *data, size_t data_len, size_t *length)
{
  const char *mode_text = ScanModeToText(scan_params.param_scan_mode);
  if (nullptr == mode_text)
    {
      FDBG(DBG_SERIOUS, "failed to get scan mode text for mode %d", scan_params.param_scan_mode);
      return SANE_STATUS_INVAL;
    }

  const char *format_string = "\x1b" "X\nR=%u,%u\nM=%s\n"
			      "%s\n"
			      "B=%u\nN=%u\n"
			      "A=%u,%u,%u,%u\n"
			      "D=%s\n"
			      "\x80";

  *length = snprintf ((char*) data,
                      data_len,
		      format_string,
                     (unsigned int) scan_params.param_x_res,
                      (unsigned int) scan_params.param_y_res,
                      mode_text,
                      (scan_params.param_scan_mode == BROTHER_SCAN_MODE_COLOR) ?
                          "C=JPEG\nJ=MID" : "C=RLENGTH",
                      (unsigned int) (scan_params.param_brightness + 50),
                      (unsigned int) (scan_params.param_contrast + 50),
                      (unsigned int) (scan_params.param_pixel_x_offset),
                      (unsigned int) (scan_params.param_pixel_y_offset),
                      (unsigned int) (scan_params.param_pixel_x_offset
                          + scan_params.param_pixel_x_width),
                      (unsigned int) (scan_params.param_pixel_y_offset
                          + scan_params.param_pixel_y_height),
                      scan_params.param_source == BROTHER_SOURCE_ADF_DUPLEX ? "DUP" : "SIN");

  if (*length > data_len)
    {
      FDBG(DBG_SERIOUS, "parameter block too long for buffer: %zu", *length);
      return SANE_STATUS_INVAL;
    }

  return SANE_STATUS_GOOD;
}
