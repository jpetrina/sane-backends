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

#include "../include/sane/sane.h"
#include "../include/sane/sanei.h"
#include "../include/sane/saneopts.h"
#include "../include/sane/sanei_config.h"
#include "../include/sane/sanei_usb.h"
#include "../include/sane/sanei_debug.h"

#include "brother_mfp-common.h"
#include "brother_mfp-driver.h"

/*
 * Protocol defines.
 *
 */
#define BROTHER_USB_REQ_STARTSESSION       1
#define BROTHER_USB_REQ_STOPSESSION        2
#define BROTHER_USB_REQ_BUTTONSTATE        3


/*-----------------------------------------------------------------*/

SANE_Status BrotherTransportUSB::Acquire ()
{
  SANE_Status res;

  FDBG(DBG_EVENT, "`%s'", devicename.c_str());

  const std::lock_guard<std::mutex> lock(mutex);

  if (is_acquired)
    {
      FDBG(DBG_WARN, "already acquired `%s'", devicename.c_str());
      return SANE_STATUS_DEVICE_BUSY;
    }

  /*
   * We set this here so that Disconnect() can be used to tidy up the device in
   * the event of a failure during the open.
   *
   */
  res = sanei_usb_open(devicename.c_str(), &fd);

  if (res != SANE_STATUS_GOOD)
    {
      (void)Release();
      FDBG(DBG_SERIOUS, "couldn't open device `%s': %s", devicename.c_str(), sane_strstatus(res));
      return res;
    }

  is_acquired = true;

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherTransportUSB::Release()
{
  FDBG(DBG_EVENT, "`%s'", devicename.c_str());

  const std::lock_guard<std::mutex> lock(mutex);

  if (!is_acquired || (fd <= 0))
    {
      FDBG(DBG_WARN, "not acquired `%s'", devicename.c_str());
      return SANE_STATUS_INVAL;
    }

  /*
   * Disconnect if necessary.
   *
   */
  if (in_session)
    {
      (void)Disconnect();
    }

  is_acquired = false;

  sanei_usb_close(fd);
  fd = 0;

  return SANE_STATUS_GOOD;
}

SANE_Bool BrotherTransportUSB::IsAcquired()
{
  return is_acquired? SANE_TRUE: SANE_FALSE;
}


SANE_Status BrotherTransportUSB::Connect()
{
  FDBG(DBG_EVENT, "`%s'", devicename.c_str());

  const std::lock_guard<std::mutex> lock(mutex);

  if (IsCancelled())
    {
      return SANE_STATUS_CANCELLED;
    }

  /*
   * Initialisation.
   *
   */
  if (!is_acquired)
    {
      FDBG(DBG_WARN, "device is not open: %s", devicename.c_str());
      return SANE_STATUS_INVAL;
    }

  if (in_session)
    {
      FDBG(DBG_WARN, "session already open: %s", devicename.c_str());
      return SANE_STATUS_INVAL;
    }

  SANE_Status res = ExecStartSession();
  if (res != SANE_STATUS_GOOD)
    {
      if (IsCancelled())
        {
          return SANE_STATUS_CANCELLED;
        }

      /*
       * Perhaps a session is in progress from a prior crash?
       * We will try to stop then restart.
       *
       */
      res = ExecStopSession();
      if (res != SANE_STATUS_GOOD)
	{
	  FDBG(DBG_WARN, "failed to send stop session sequence for restart: %s.",
	       devicename.c_str());
	  return SANE_STATUS_DEVICE_BUSY;
	}

      if (IsCancelled())
        {
          return SANE_STATUS_CANCELLED;
        }

      res = ExecStartSession();
      if (res != SANE_STATUS_GOOD)
	{
	  FDBG(DBG_WARN, "failed to send start session sequence for restart: %s.",
	       devicename.c_str());
	  return SANE_STATUS_DEVICE_BUSY;
	}
    }

  in_session = true;

  if (IsCancelled())
    {
      return SANE_STATUS_CANCELLED;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherTransportUSB::Disconnect ()
{
  FDBG(DBG_EVENT, "`%s'", devicename.c_str());

  const std::lock_guard<std::mutex> lock(mutex);

  if (IsCancelled())
    {
      return SANE_STATUS_CANCELLED;
    }

  /*
   * Initialisation.
   *
   */
  if (!is_acquired)
    {
      FDBG(DBG_WARN, "device not open: %s.", devicename.c_str());
      return SANE_STATUS_INVAL;
    }

  if (!in_session)
    {
      FDBG(DBG_WARN, "not in session: %s.", devicename.c_str());
      return SANE_STATUS_INVAL;
    }

  /*
   * Flush out USB data and close session.
   *
   */
  (void)PollForReadFlush (TIMEOUT_SECS(1));

  SANE_Status res = ExecStopSession();
  if (res != SANE_STATUS_GOOD)
    {
      FDBGNP(DBG_WARN, "failed to send stop sequence.");
      return res;
    }

  in_session = false;

  if (IsCancelled())
    {
      return SANE_STATUS_CANCELLED;
    }

  return SANE_STATUS_GOOD;
}

SANE_Bool BrotherTransportUSB::IsConnected()
{
  return in_session? SANE_TRUE: SANE_FALSE;
}

SANE_Status BrotherTransportUSB::ExecStartSession ()
{
  SANE_Status res = sanei_usb_control_msg(fd, USB_DIR_IN | USB_TYPE_VENDOR,
					  BROTHER_USB_REQ_STARTSESSION,
					  2, 0, 5, small_buffer);

  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_WARN, "failed to send start session sequence: %s.", devicename.c_str());
      return res;
    }

  BrotherSessionResponse resp;

  if (DecodeSessionResp(small_buffer, 5, resp) != SANE_STATUS_GOOD)
    {
      FDBGNP(DBG_WARN, "start session response is invalid.");
      return SANE_STATUS_IO_ERROR;
    }

  if (!resp.ready)
    {
      FDBGNP(DBG_WARN, "scanner indicates not ready.");
      return SANE_STATUS_DEVICE_BUSY;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherTransportUSB::ExecStopSession ()
{
  SANE_Status res = sanei_usb_control_msg(fd, USB_DIR_IN | USB_TYPE_VENDOR,
					  BROTHER_USB_REQ_STOPSESSION,
					  2, 0, 5, small_buffer);

  if (res != SANE_STATUS_GOOD)
    {

      FDBGNP(DBG_WARN, "failed to send stop sequence.");
      return res;
    }

  BrotherSessionResponse resp;

  if (DecodeSessionResp(small_buffer, 5, resp) != SANE_STATUS_GOOD)
    {
      FDBGNP(DBG_WARN, "stop session response is invalid.");
      return SANE_STATUS_IO_ERROR;
    }

  if (!resp.ready)
    {
      FDBGNP(DBG_WARN, "scanner indicates not ready.");
      return SANE_STATUS_DEVICE_BUSY;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherTransportUSB::DecodeSessionResp (const SANE_Byte *data, size_t data_len,
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
   * Check the valid status values are 0x00 or 0x20
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
 * Fill `data_buffer` by reading from the USB device.
 *
 * This is a little different from what we use to receive bulk packet
 * responses because we are using an internal buffer to read more than
 * the front end might want to receive in one go. The data packets are
 * typically quite large and reading them in one go is much more efficient.
 *
 * It libusb has some buffer size restrictions that we should honour.
 *
 */
SANE_Status BrotherTransportUSB::PollDataFromScanner()
{
  /*
   * First, see if we need to do a read from the device to satisfy the read.
   * Only if we have space to read though. Obviously.
   *
   * We will place a lower bound on the amount of space. If we have a substantial amount of data
   * in the read buffer, we won't attempt a read.
   *
   */
  size_t bytes_to_read = (BROTHER_READ_BUFFER_LEN - data_buffer_bytes) & ~(1024 - 1);
  if (bytes_to_read > 1024)
    {

      /*
       * Try to do a read from the driver.
       * Timeout of zero ensures that we only try once.
       *
       * Note that we trim the buffer so that it is a multiple of 1024. This seems to be
       * a requirement of libusb to avoid some potential buffer overflow conditions.
       * It's OK: as long as we are reading something substantial, that is fine.
       *
       */
      FDBG(DBG_SERIOUS, "attempting read, space for %zu bytes", bytes_to_read);

      useconds_t max_time = 0;

      SANE_Status res = PollForRead (data_buffer + data_buffer_bytes, &bytes_to_read, &max_time);
      if (res != SANE_STATUS_GOOD)
	{
	  FDBG(DBG_EVENT, "read aborted due to read error: %d.", res);
	  return res;
	}

      data_buffer_bytes += bytes_to_read;
      FDBG(DBG_SERIOUS, "read %zu bytes, now buffer has %zu bytes", bytes_to_read,
	   data_buffer_bytes);
    }
  return SANE_STATUS_GOOD;
}

/*
 * Try to fill `data` with scanlines by decoding `data_buffer`. Successfully
 * decoded data will be removed from `data_buffer`.
 *
 */
//SANE_Status BrotherTransportUSB::DecodeReadBuffer(SANE_Byte *data, size_t max_length, size_t *length)
//{
//  size_t data_consumed = 0;
//
//  FDBGNP (DBG_EVENT, "");
//
//  SANE_Status dec_ret = encoder->DecodeScanData (data_buffer,
//						 data_buffer_bytes,
//						 &data_consumed,
//						 data,
//						 max_length,
//						 length);
//
//  /*
//   * Shift down the read buffer so that we can maximise our
//   * space to read new data.
//   *
//   */
//  if (data_consumed)
//    {
//      (void) memmove (data_buffer, data_buffer + data_consumed, data_buffer_bytes - data_consumed);
//      data_buffer_bytes -= data_consumed;
//    }
//
//  FDBG (DBG_DETAIL,
//       "decoder consumes %zu bytes and writes %zu bytes, returning %s",
//       data_consumed,
//       *length,
//       sane_strstatus(dec_ret));
//
//  return dec_ret;
//}

//SANE_Status BrotherUSBDriver::ReadScanData (SANE_Byte *data, size_t max_length, size_t *length)
//{
//  *length = 0;
//
//  FDBG (DBG_EVENT, "`%s'", devicename);
//
//  if (!encoder)
//    {
//      FDBGNP (DBG_WARN, "no encoder");
//      return SANE_STATUS_UNSUPPORTED;
//    }
//
//  const std::lock_guard<std::mutex> lock(mutex);
//
//  if (scan_state != SCANSTATE_ACQUIRING)
//    {
//      return SANE_STATUS_EOF;
//    }
//
//  /*
//   * OK. If we are in async IO mode, then we will only return if we read something
//   * while in the acquisition state.
//   *
//   * Also we could exit if:
//   *
//   * - Cancel requested
//   * - Got EOF notification
//   * - Error.
//   *
//   */
//  SANE_Status res = SANE_STATUS_GOOD;
//
//  for (;;)
//    {
//
//      /*
//       * First we try to fill the output buffer from scanned data.
//       * No need to read from USB when we still have plenty of data to process.
//       *
//       */
//      res = DecodeReadBuffer(data, max_length, length);
//      if ((res == SANE_STATUS_GOOD) && (*length == 0))
//	{
//	  /*
//	   * In this case we read from USB to further fill `data`.
//	   *
//	   */
//	  res = PollDataFromScanner();
//	  if (res != SANE_STATUS_GOOD)
//	    {
//	      return res;
//	    }
//
//	  res = DecodeReadBuffer(data, max_length, length);
//	}
//
//      /*
//       * If the decoder says that we finished the image, then we change state.
//       *
//       */
//      if (res == SANE_STATUS_EOF)
//	{
//	  scan_state = SCANSTATE_EOF;
//	  break;
//	}
//      else if (res != SANE_STATUS_GOOD)
//	{
//	  /*
//	   * None of these error states require a cancel to move on.
//	   *
//	   */
//	  scan_state = SCANSTATE_IDLE;
//	  break;
//	}
//
//      /*
//       * We might have been cancelled.
//       *
//       */
//      if (want_cancel)
//	{
//	  FDBGNP(DBG_DETAIL, "cancel detected during read. Will perform cancel.");
//
//	  res = SANE_STATUS_CANCELLED;
//	  (void)CancelScan_Internal();
//	  break;
//	}
//
//      /*
//       * If we are in blocking IO mode, then if we read nothing and no error, wait a bit
//       * and try again. We could release the lock while we're sleeping but we aren't sleeping
//       * for very long and it would make things complicated. Let's not.
//       *
//       */
//      if (async_io_mode || (*length != 0))
//	{
//	  break;
//	}
//
//      FDBGNP(DBG_DETAIL, "no read, so sleeping for a bit.");
//      struct timespec ts = {0, 100000};
//      nanosleep(&ts, NULL);
//
//      /*
//       * We waited a bit so check cancel again.
//       *
//       */
//      if (want_cancel)
//	{
//	  FDBGNP(DBG_DETAIL, "cancel detected during read. Will perform cancel.");
//
//	  res = SANE_STATUS_CANCELLED;
//	  (void)CancelScan_Internal();
//	  break;
//	}
//
//    }
//
//  return res;
//}

SANE_Status BrotherTransport::SetAsyncIOMode(bool async_io)
{
  FDBGNP (DBG_EVENT, "");

  async_io_mode = async_io;

  return SANE_STATUS_GOOD;
}

bool BrotherTransport::GetAsyncIOMode()
{
  return async_io_mode;
}

SANE_Status BrotherTransportUSB::SendCommand(const SANE_Byte *send_buffer, size_t send_size)
{
  const std::lock_guard<std::mutex> lock(mutex);

  if (IsCancelled())
    {
      return SANE_STATUS_CANCELLED;
    }

  size_t buf_size = send_size;
  SANE_Status res = sanei_usb_write_bulk(fd, send_buffer, &buf_size);
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to send USB bulk block: %d", res);
      return res;
    }

  if (buf_size != send_size)
    {
      FDBGNP(DBG_SERIOUS, "failed to write USB bulk block");
      return SANE_STATUS_IO_ERROR;
    }

  if (IsCancelled())
    {
      return SANE_STATUS_CANCELLED;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherTransportUSB::ReceiveResponse(SANE_Byte *rec_buffer, size_t *rec_size,
						 useconds_t *rec_timeout)
{
  const std::lock_guard<std::mutex> lock(mutex);

  if (IsCancelled())
    {
      return SANE_STATUS_CANCELLED;
    }

  /*
   * A timeout in this case in an error.
   *
   */
  SANE_Status res = PollForRead(rec_buffer, rec_size, rec_timeout);
  if ((res == SANE_STATUS_GOOD) && (*rec_timeout == 0))
    {
      return SANE_STATUS_IO_ERROR;
    }

  if (IsCancelled())
    {
      return SANE_STATUS_CANCELLED;
    }

  return res;
}

SANE_Status BrotherTransportUSB::SendAndReceive(const SANE_Byte *send_buffer, size_t send_size,
						SANE_Byte *rec_buffer, size_t *rec_size,
						useconds_t *rec_timeout)
{
  SANE_Status res = SendCommand(send_buffer, send_size);
  if (res != SANE_STATUS_GOOD)
    {
      return res;
    }

  res = ReceiveResponse(rec_buffer, rec_size, rec_timeout);
  if (res != SANE_STATUS_GOOD)
    {
      return res;
    }

  return SANE_STATUS_GOOD;
}


/*
 * Just suck any cached data from the read stream and discard it.
 *
 * We will continue to read data until we wait for the specified time
 * having read no data.
 *
 * Hard maximum of 10 seconds though.
 *
 */
SANE_Status BrotherTransportUSB::PollForReadFlush (useconds_t max_time)
{
  useconds_t orig_time = max_time;
  useconds_t hard_time_limit = TIMEOUT_SECS(10);

  do
    {
      /*
       * We use this to find out how much time elapsed during the next
       * read. We will use it to update the hard limit.
       *
       */
      useconds_t start_time = max_time;
      size_t buf_len = sizeof(small_buffer);

      SANE_Status res = PollForRead (small_buffer, &buf_len, &max_time);
      if (res != SANE_STATUS_GOOD)
	{
	  FDBG(DBG_WARN, "failed to flush poll: %d", res);
	  return res;
	}

      /*
       * Update the hard limit timeout.
       *
       */
      hard_time_limit -= std::min(hard_time_limit, start_time - max_time);

      /*
       * If we read some data, reset the timeout timer,
       *
       */
      if (buf_len > 0)
        {
          max_time = orig_time;
        }
    }
  while (max_time && hard_time_limit);

  return SANE_STATUS_GOOD;
}

/*
 * If you only want to read once, then set *max_time to 0.
 *
 * max_time is updated to the amount of time left in the timeout.
 * If we timed out, then max_time will be set to 0. We don't return
 * an error code in this case, because timing out might be a legitimate
 * scenario, especially when we are waiting for data for a period. However
 * when waiting once for a command response, it most certainly is.
 *
 */
SANE_Status BrotherTransportUSB::PollForRead (SANE_Byte *buffer, size_t *buf_len, useconds_t *max_time)
{
  SANE_Status res;

  FDBG(DBG_EVENT, "`%s'", devicename.c_str());

  useconds_t timeout = 0;
  size_t read_amt;
  do
    {
      read_amt = *buf_len;

      res = sanei_usb_read_bulk (fd, buffer, &read_amt);
      if (res == SANE_STATUS_GOOD)
	{
	  break;
	}

      if (res != SANE_STATUS_EOF)
	{
	  FDBG(DBG_WARN, "failed to poll for read: %d", res);
	  *max_time -= timeout;
	  return res;
	}

      /*
       * Check for cancel.
       *
       */
      if (IsCancelled())
	{
	  return SANE_STATUS_CANCELLED;
	}

      /*
       * TODO: Is this a reasonable time?
       *
       */
      usleep (10 * 1000);
      timeout += (10 * 1000);

      /*
       * Check for cancel again after sleep.
       *
       */
      if (want_cancel)
	{
	  return SANE_STATUS_CANCELLED;
	}
    }
  while (timeout < *max_time);

  /*
   * We timed out. No data and no time left.
   *
   */
  if (*max_time && (timeout >= *max_time))
    {
      *buf_len = 0;
      *max_time = 0;
      return SANE_STATUS_GOOD;
    }

  /*
   * We read some data!
   *
   */
  *max_time -= timeout;
  *buf_len = read_amt;

  return SANE_STATUS_GOOD;
}

//SANE_Status BrotherUSBDriver::Init ()
//{
//  SANE_Status res;
//
//  FDBG(DBG_EVENT, "`%s'", devicename);
//
//  /*
//   * First wait for a second, flushing stuff out.
//   *
//   */
//  res = PollForReadFlush (TIMEOUT_SECS(1));
//  if (res != SANE_STATUS_GOOD)
//    {
//      FDBG(DBG_WARN, "initial flush failure: %d", res);
//      return res;
//    }
//
//  /*
//   * Execute Query request.
//   *
//   */
//  size_t packet_len = 0;
//  SANE_Status dec_ret = encoder->EncodeQueryBlock (small_buffer, sizeof(small_buffer), &packet_len);
//  if (dec_ret != SANE_STATUS_GOOD)
//    {
//      FDBG(DBG_SERIOUS, "failed to generate query block: %d", dec_ret);
//      (void) CancelScan_Internal ();
//      return SANE_STATUS_INVAL;
//    }
//
//  size_t buf_size = packet_len;
//  res = sanei_usb_write_bulk (fd, small_buffer, &buf_size);
//  if (res != SANE_STATUS_GOOD)
//    {
//      FDBG(DBG_SERIOUS, "failed to send query block: %d", res);
//      (void) CancelScan_Internal ();
//      return res;
//    }
//
//  if (buf_size != packet_len)
//    {
//      FDBGNP(DBG_SERIOUS, "failed to write query block");
//      (void) CancelScan_Internal ();
//      return SANE_STATUS_IO_ERROR;
//    }
//
//  /*
//   * Try to read the response.
//   *
//   */
//  buf_size = sizeof(small_buffer);
//  useconds_t timeout = TIMEOUT_SECS(8);
//
//  res = PollForRead (small_buffer, &buf_size, &timeout);
//  if (res != SANE_STATUS_GOOD)
//    {
//      FDBG(DBG_SERIOUS, "failed to read query block response: %d", res);
//      (void) CancelScan_Internal ();
//      return res;
//    }
//
//  if (buf_size == 0)
//    {
//      FDBGNP(DBG_SERIOUS, "timed out waiting for query block response.");
//      (void) CancelScan_Internal ();
//      return SANE_STATUS_IO_ERROR;
//    }
//
//  BrotherQueryResponse query_resp;
//
//  dec_ret = encoder->DecodeQueryBlockResp (small_buffer, buf_size, query_resp);
//  if (dec_ret != SANE_STATUS_GOOD)
//    {
//      FDBG(DBG_SERIOUS, "failed to read query block response: %d", dec_ret);
//      (void) CancelScan_Internal ();
//      return res;
//    }
//
//  return SANE_STATUS_GOOD;
//}

/*
 * Only call this from sane_cancel.
 *
 */
//SANE_Status BrotherUSBDriver::CancelScan ()
//{
//  FDBG (DBG_EVENT, "`%s'", devicename);
//
//  if (!encoder)
//    {
//      FDBGNP (DBG_WARN, "no encoder");
//      return SANE_STATUS_UNSUPPORTED;
//    }
//
//  /*
//   * Signal the flag to kill any blocking operations then take the
//   * lock. It could be that when we were called, we were acquiring an
//   * image but by setting this flag, we could precipitate a cancel, in which
//   * case there is nothing actually for us to do here. We can check state
//   * to see if we need to no anything. But after we take the lock!
//   *
//   * Note that because of this flag we don't support multiple simultaneous
//   * calls to sane_cancel() from different threads.
//   *
//   * Another thread is also free to call CancelScan_Internal() if it saw
//   * the want_cancel flag. When we acquire the lock and see that the SCANSTATE
//   * is now IDLE, then we'll know there is nothing for us to do.
//   *
//   */
//  want_cancel = true;
//  const std::lock_guard<std::mutex> lock(mutex);
//  want_cancel = false;
//
//  /*
//   * Perform the cancel sequence with the device.
//   *
//   */
//  return CancelScan_Internal();
//}


/*
 * This function is safe for us to call from other operations.
 *
 */
//SANE_Status BrotherUSBDriver::CancelScan_Internal ()
//{
//  SANE_Status res;
//  size_t packet_len = 0;
//
//  FDBG (DBG_EVENT, "`%s'", devicename);
//
//  if (!encoder)
//    {
//      FDBGNP (DBG_WARN, "no encoder");
//      return SANE_STATUS_UNSUPPORTED;
//    }
//
//  if (scan_state == SCANSTATE_IDLE)
//    {
//      FDBG (DBG_WARN, "not scanning `%s'", devicename);
//      return SANE_STATUS_GOOD;
//    }
//
//  scan_state = SCANSTATE_IDLE;
//
//  /*
//   * Send the cancel sequence.
//   *
//   * 0x1b52
//   *
//   * We could test to see if we have actually requested a scan
//   * but it looks like sending the cancel scan code is OK if we are not.
//   * It's more complicated to add code to check than it is to just send it.
//   *
//   * TODO: we should use the encoder to obtain this command. I'm fairly sure
//   * FAMILY_5 has a different cancel sequence.
//   *
//   */
//  res = encoder->EncodeCancelBlock (small_buffer, sizeof(small_buffer), &packet_len);
//
//  if (res != SANE_STATUS_GOOD)
//    {
//      FDBG(DBG_SERIOUS, "failed to generate basic param block: %d", res);
//      return SANE_STATUS_INVAL;
//    }
//
//
////  (void) memcpy (small_buffer, "\x1b" "R", 2);
////  size_t buf_size = 2;
//
//  size_t buf_size = packet_len;
//  res = sanei_usb_write_bulk (fd, small_buffer, &buf_size);
//  if (res != SANE_STATUS_GOOD)
//    {
//      FDBG(DBG_SERIOUS, "failed to send basic parameter block: %d", res);
//      return res;
//    }
//
//  if (buf_size != packet_len)
//    {
//      FDBGNP(DBG_SERIOUS, "failed to write basic parameter block");
//      return SANE_STATUS_IO_ERROR;
//    }
//
//  /*
//   * Flush out any buffered scan data.
//   *
//   */
//  res = PollForReadFlush (TIMEOUT_SECS(1));
//  if (res != SANE_STATUS_GOOD)
//    {
//      FDBG (DBG_WARN, "initial flush failure: %d", res);
//      return res;
//    }
//
//  /*
//   * Finally terminate the scan session.
//   *
//   */
//  res = StopSession ();
//
//  if (res != SANE_STATUS_GOOD)
//    {
//      FDBG (DBG_SERIOUS, "failed to send stop session control: %d", res);
//      return res;
//    }
//
//  /*
//   * Free read buffer.
//   *
//   */
//  delete [] data_buffer;
//  data_buffer = NULL;
//  data_buffer_bytes = 0;
//
//  return SANE_STATUS_GOOD;
//}

//SANE_Status BrotherUSBDriver::StartScan_BasicParameter ()
//{
//  SANE_Status res;
//  size_t packet_len = 0;
//
//  FDBGNP (DBG_EVENT, "");
//
//  res = encoder->EncodeBasicParameterBlock (small_buffer, sizeof(small_buffer), &packet_len);
//
//  if (res != SANE_STATUS_GOOD)
//    {
//      FDBG(DBG_SERIOUS, "failed to generate basic param block: %d", res);
//      return SANE_STATUS_INVAL;
//    }
//
//  size_t buf_size = packet_len;
//  res = sanei_usb_write_bulk (fd, small_buffer, &buf_size);
//  if (res != SANE_STATUS_GOOD)
//    {
//      FDBG(DBG_SERIOUS, "failed to send basic parameter block: %d", res);
//      return res;
//    }
//
//  if (buf_size != packet_len)
//    {
//      FDBGNP(DBG_SERIOUS, "failed to write basic parameter block");
//      return SANE_STATUS_IO_ERROR;
//    }
//
//  /*
//   * Try to read the response.
//   *
//   */
//  buf_size = sizeof(small_buffer);
//  useconds_t timeout = TIMEOUT_SECS(8);
//
//  res = PollForRead (small_buffer, &buf_size, &timeout);
//  if (res != SANE_STATUS_GOOD)
//    {
//      FDBG(DBG_SERIOUS, "failed to read prep parameter block response: %d", res);
//      return res;
//    }
//
//  if (buf_size == 0)
//    {
//      FDBGNP(DBG_SERIOUS, "timed out waiting for parameter block response.");
//      (void) CancelScan_Internal ();
//      return SANE_STATUS_IO_ERROR;
//    }
//
//  BrotherBasicParamResponse basic_param_resp;
//
//  res = encoder->DecodeBasicParameterBlockResp (small_buffer, buf_size, basic_param_resp);
//  if (res != SANE_STATUS_GOOD)
//    {
//      FDBG(DBG_SERIOUS, "failed to read prep parameter block response: %d", res);
//      return res;
//    }
//
//  return SANE_STATUS_GOOD;
//}

//SANE_Status BrotherUSBDriver::StartScan_SourceSelect ()
//{
//  SANE_Status res;
//  size_t packet_len = 0;
//
//  FDBGNP(DBG_EVENT, "");
//
//  res = encoder->EncodeSourceSelectBlock (small_buffer, sizeof(small_buffer), &packet_len);
//  if (res != SANE_STATUS_UNSUPPORTED)
//    {
//      if (res != SANE_STATUS_GOOD)
//	{
//	  FDBG(DBG_SERIOUS, "failed to generate source select block: %d", res);
//	  return SANE_STATUS_INVAL;
//	}
//
//      size_t buf_size = packet_len;
//      res = sanei_usb_write_bulk (fd, small_buffer, &buf_size);
//      if (res != SANE_STATUS_GOOD)
//	{
//	  FDBG(DBG_SERIOUS, "failed to send source select block: %d", res);
//	  return res;
//	}
//
//      if (buf_size != packet_len)
//	{
//	  FDBGNP(DBG_SERIOUS, "failed to write all source select block");
//	  return SANE_STATUS_IO_ERROR;
//	}
//
//      /*
//       * Try to read the response.
//       *
//       */
//      buf_size = sizeof(small_buffer);
//      useconds_t timeout = TIMEOUT_SECS(8);
//
//      res = PollForRead (small_buffer, &buf_size, &timeout);
//      if (res != SANE_STATUS_GOOD)
//	{
//	  FDBG(DBG_SERIOUS, "failed to read source select block response: %d", res);
//	  return res;
//	}
//
//      if (buf_size == 0)
//        {
//          FDBGNP(DBG_SERIOUS, "timed out waiting for source select block response.");
////          (void) CancelScan_Internal ();
//          return SANE_STATUS_IO_ERROR;
//        }
//
//      BrotherSourceStatusResponse source_resp;
//
//      res = encoder->DecodeSourceStatusBlockResp (small_buffer, buf_size, source_resp);
//      if (res != SANE_STATUS_GOOD)
//	{
//	  FDBG(DBG_SERIOUS, "source select response block invalid: %d", res);
//	  return res;
//	}
//
//      FDBG(DBG_DETAIL, "Source reports readiness?: %s", source_resp.source_ready ? "Yes" : "No");
//    }
//
//  return SANE_STATUS_GOOD;
//}

//SANE_Status BrotherUSBDriver::StartScan_Parameter()
//{
//  SANE_Status res;
//  size_t packet_len = 0;
//
//  FDBGNP (DBG_EVENT, "");
//
//  res = encoder->EncodeParameterBlock (small_buffer, sizeof(small_buffer),
//					&packet_len);
//  if (res != SANE_STATUS_GOOD)
//    {
//      FDBG(DBG_SERIOUS, "failed to generate param block: %d", res);
//      return res;
//    }
//
//  size_t buf_size = packet_len;
//  res = sanei_usb_write_bulk (fd, small_buffer, &buf_size);
//  if (res != SANE_STATUS_GOOD)
//    {
//      FDBG(DBG_SERIOUS, "failed to send parameter block: %d", res);
//      return res;
//    }
//
//  if (buf_size != packet_len)
//    {
//      FDBGNP(DBG_SERIOUS, "failed to write parameter block");
//      return SANE_STATUS_IO_ERROR;
//    }
//
//  return SANE_STATUS_GOOD;
//}

//SANE_Status BrotherUSBDriver::StartScan_ParameterBlank ()
//{
//  SANE_Status res;
//  size_t packet_len = 0;
//
//  FDBGNP (DBG_EVENT, "");
//
//  res = encoder->EncodeParameterBlockBlank (small_buffer, sizeof(small_buffer), &packet_len);
//
//  /*
//   * If SANE_STATUS_UNSUPPORTED, then we don't need to send this block.
//   *
//   */
//  if (res == SANE_STATUS_NO_DOCS)
//    {
//      FDBGNP(DBG_SERIOUS, "there are no documents, so we cannot acquire image.");
//      return res;
//    }
//  if (res == SANE_STATUS_GOOD)
//    {
//      size_t buf_size = packet_len;
//      res = sanei_usb_write_bulk (fd, small_buffer, &buf_size);
//      if (res != SANE_STATUS_GOOD)
//	{
//	  FDBG(DBG_SERIOUS, "failed to send parameter block: %d", res);
//	  return res;
//	}
//
//      if (buf_size != packet_len)
//	{
//	  FDBGNP(DBG_SERIOUS, "failed to write parameter block");
//	  return SANE_STATUS_IO_ERROR;
//	}
//    }
//  else if (res != SANE_STATUS_UNSUPPORTED)
//    {
//      FDBG(DBG_SERIOUS, "failed to generate blank param block: %d", res);
//      return res;
//    }
//
//  return SANE_STATUS_GOOD;
//}

//SANE_Status BrotherUSBDriver::StartScan ()
//{
//  SANE_Status res;
//
//  FDBG (DBG_EVENT, "`%s'", devicename);
//
//  if (!encoder)
//    {
//      FDBGNP (DBG_WARN, "no encoder");
//      return SANE_STATUS_UNSUPPORTED;
//    }
//
//  const std::lock_guard<std::mutex> lock(mutex);
//
//  /*
//   * Check state.
//   * Only IDLE or EOF are really valid.
//   *
//   * If IDLE, then we are starting a completely new session.
//   * if EOF, then we are acquiring the "next" image.
//   *
//   * If we are already scanning an image, then we will ignore it.
//   * SANE API spec. isn't really helpful here. It might be correct
//   * to return SANE_STATUS_INVAL or SANE_STATUS_BUSY.
//   *
//   */
//  if (scan_state == SCANSTATE_ACQUIRING)
//    {
//      return SANE_STATUS_GOOD;
//    }
//
//  /*
//   * If IDLE then we need to kick off a completely new session.
//   * There are a few things that we need to send to get things moving.
//   *
//   */
//  if (scan_state == SCANSTATE_IDLE)
//    {
//      scan_state = SCANSTATE_ACQUIRING;
//
//      /*
//       * Initialisation of a new scanning session.
//       *
//       */
//      res = StartSession();
//      if (res != SANE_STATUS_GOOD)
//        {
//          FDBG (DBG_SERIOUS, "failed to start session: %d", res);
////          (void)CancelScan_Internal();
//          return res;
//        }
//
//      res = PollForReadFlush (TIMEOUT_SECS(1));
//      if (res != SANE_STATUS_GOOD)
//        {
//          FDBG (DBG_SERIOUS, "failed to read flush: %d", res);
////          (void) CancelScan_Internal ();
//          return res;
//        }
//
//      res = StartScan_BasicParameter();
//      if (res != SANE_STATUS_GOOD)
//	{
//          FDBG (DBG_SERIOUS, "failed to send basic parameter block: %d", res);
////          (void) CancelScan_Internal ();
//          return res;
//	}
//
//      res = StartScan_SourceSelect();
//      if (res != SANE_STATUS_GOOD)
//	{
//          FDBG (DBG_SERIOUS, "failed to send source select block: %d", res);
////          (void) CancelScan_Internal ();
//          return res;
//	}
//
//      res = StartScan_Parameter();
//      if (res != SANE_STATUS_GOOD)
//	{
//          FDBG (DBG_SERIOUS, "failed to send parameter block: %d", res);
////          (void) CancelScan_Internal ();
//          return res;
//	}
//
//      /*
//       * Allocate a read buffer.
//       *
//       */
//      data_buffer = new SANE_Byte[BROTHER_READ_BUFFER_LEN];
//      if (nullptr == data_buffer)
//        {
//          FDBG (DBG_SERIOUS, "failed to allocate read buffer: %zu", (size_t) BROTHER_READ_BUFFER_LEN);
////          (void) CancelScan_Internal ();
//          return SANE_STATUS_NO_MEM;
//        }
//     }
//  else if (scan_state == SCANSTATE_EOF)	// get next image.
//    {
//      scan_state = SCANSTATE_ACQUIRING;
//
//      /*
//       * If this is not the first image, then we merely need
//       * to ask for another image. We do this with a blank parameter block.
//       * This is not necessary on all families.
//       *
//       */
//      res = StartScan_ParameterBlank();
//      if (res == SANE_STATUS_NO_DOCS)
//	{
//	  scan_state = SCANSTATE_IDLE;
//          FDBGNP (DBG_SERIOUS, "no docs indicated, so no further scanning possible.");
//          return res;
//	}
//      else if (res != SANE_STATUS_GOOD)
//	{
//          FDBG (DBG_SERIOUS, "failed to send blank parameter block: %d", res);
////          (void) CancelScan_Internal ();
//          return res;
//	}
//    }
//
//  data_buffer_bytes = 0;
//
//  /*
//   * Reset the encoder/decoder.
//   *
//   */
//  encoder->NewPage();
//
//  /*
//   * We might have been cancelled.
//   *
//   */
//  if (want_cancel)
//    {
//      FDBGNP(DBG_DETAIL, "cancel detected during start. Will perform cancel.");
//
////      (void)CancelScan_Internal();
//      return SANE_STATUS_CANCELLED;
//    }
//
//  return SANE_STATUS_GOOD;
//}

//SANE_Status BrotherUSBDriver::CheckSensor (BrotherSensor &status)
//{
//  if (!encoder)
//    {
//      FDBGNP (DBG_WARN, "no encoder");
//      return SANE_STATUS_UNSUPPORTED;
//    }
//
//  const std::lock_guard<std::mutex> lock(mutex);
//
//  SANE_Status res = sanei_usb_control_msg (fd, USB_DIR_IN | USB_TYPE_VENDOR,
//                                           BROTHER_USB_REQ_BUTTONSTATE,
//					   0, 0, 255, small_buffer);
//
//  if (res != SANE_STATUS_GOOD)
//    {
//      FDBG(DBG_WARN, "failed to send check sensor sequence: %s.", devicename);
//      return res;
//    }
//
// BrotherButtonQueryResponse resp;
//
// if (encoder->DecodeButtonQueryResp (small_buffer, 4, resp) != SANE_STATUS_GOOD)
// {
//   FDBGNP (DBG_WARN, "button state response is invalid.");
//   return SANE_STATUS_IO_ERROR;
// }
//
// if (resp.has_button_press == BROTHER_SENSOR_NONE)
//   {
//     status = BROTHER_SENSOR_NONE;
//     return SANE_STATUS_GOOD;
//   }
//
// FDBGNP (DBG_DETAIL, "scanner indicates a button was pushed.");
//
// /*
//  * Now we query the button state.
//  * This is done by the same request. Not sure why the first response even
//  * exists, but this is how it appears to work. Also note that there may be
//  * default ("no button pressed") responses in between, hence the loop and
//  * timeout.
//  *
//  */
//  useconds_t time_left = TIMEOUT_SECS(2);
//  static const useconds_t deltaT = TIMEOUT_MILLISECS(100);
//
//  while (time_left > deltaT)
//    {
//      res = sanei_usb_control_msg (fd, USB_DIR_IN | USB_TYPE_VENDOR,
//                                  BROTHER_USB_REQ_BUTTONSTATE,
//                                  0, 0, 255, small_buffer);
//
//      if (res != SANE_STATUS_GOOD)
//	{
//	  FDBG(DBG_WARN, "failed to send check sensor sequence: %s.", devicename);
//	  return res;
//	}
//
//      /*
//      * The second response is a bit longer and button specific.
//      *
//      */
//      BrotherButtonStateResponse state_resp;
//
//      SANE_Status dec_ret = encoder->DecodeButtonStateResp (small_buffer, 9, state_resp);
//
//      if (dec_ret == SANE_STATUS_GOOD)
//        {
//          status = state_resp.button_value;
//          return SANE_STATUS_GOOD;
//        }
//      /* use INVAL to indicate a "no button pressed" response */
//      else if (dec_ret != SANE_STATUS_INVAL)
//        {
//          FDBGNP (DBG_WARN, "button info response is invalid.");
//          return dec_ret;
//        }
//      else
//        {
//          usleep(100 * 1000);
//          time_left -= deltaT;
//        }
//    }
//  /* If we get here, we got 1/2 packets, but the second one never arrived */
//  return SANE_STATUS_IO_ERROR;
//}

//BrotherDriver::BrotherDriver (BrotherFamily family, SANE_Word capabilities) :
//    family (family),
//    capabilities(capabilities),
//    encoder(nullptr)
//{
//  switch (family)
//  {
//    case BROTHER_FAMILY_5:
//      encoder = new BrotherEncoderFamily5(capabilities);
//      break;
//
//    case BROTHER_FAMILY_4:
//      encoder = new BrotherEncoderFamily4(capabilities);
//      break;
//
//    case BROTHER_FAMILY_2:
//      encoder = new BrotherEncoderFamily2(capabilities);
//      break;
//
//    case BROTHER_FAMILY_3:
//        encoder = new BrotherEncoderFamily3(capabilities);
//        break;
//
//    case BROTHER_FAMILY_1:
//    case BROTHER_FAMILY_NONE:
//    default:
//      FDBG(DBG_SERIOUS, "no encoder available for family: %d.", family);
//      break;
//    }
//
//  if (nullptr == encoder)
//    {
//      FDBGNP(DBG_SERIOUS, "failed to allocate encoder.");
//    }
//}

//BrotherDriver::~BrotherDriver ()
//{
//  delete encoder;
//}

//BrotherUSBDriver::BrotherUSBDriver (const char *devicename, BrotherFamily family,
//                                    SANE_Word capabilities) :
//     BrotherDriver (family, capabilities),
//     scan_state(SCANSTATE_IDLE),
//     is_open (false),
//     in_session (false),
//     devicename (nullptr),
//     next_frame_number(0),
//     fd (0),
//     small_buffer {0},
//     data_buffer (nullptr),
//     data_buffer_bytes (0),
//     async_io_mode(false),
//     want_cancel(false)
// {
//   this->devicename = strdup(devicename);
// }
//
//BrotherUSBDriver::~BrotherUSBDriver ()
//{
//  delete[] data_buffer;
//  free (devicename);
//}
