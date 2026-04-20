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
#include <string>
#include <mutex>

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

#define BROTHER_READ_BUFFER_LEN (64 * 1024)


/*-----------------------------------------------------------------*/

SANE_Status BrotherTransportUSB::Acquire ()
{
  SANE_Status res;

  const std::lock_guard<std::mutex> lock(mutex);

  FDBG (DBG_EVENT, "`%s'", devicename.c_str());

  if (is_acquired)
    {
      FDBG (DBG_WARN, "already acquired `%s'", devicename.c_str());
      return SANE_STATUS_DEVICE_BUSY;
    }

  res = sanei_usb_open (devicename.c_str(), &fd);

  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "couldn't acquire device `%s': %s", devicename.c_str(), sane_strstatus(res));
      return res;
    }

  is_acquired = true;

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherTransportUSB::Connect ()
{
  SANE_Status res;

  const std::lock_guard<std::mutex> lock(mutex);

  FDBG (DBG_EVENT, "`%s'", devicename.c_str());

  if (in_session)
    {
      FDBG (DBG_WARN, "already open `%s'", devicename.c_str());
      return SANE_STATUS_DEVICE_BUSY;
    }

  if (!is_acquired)
    {
      FDBG (DBG_WARN, "not acquired `%s'", devicename.c_str());
      return SANE_STATUS_INVAL;
    }

  /*
   * Initialisation.
   *
   */
  res = ExecStartSession();
  if (res != SANE_STATUS_GOOD)
    {
	FDBG(DBG_SERIOUS, "failed to start session: %d", res);
	Disconnect();
	return res;
    }

  /*
   * Flush.
   *
   */
  in_session = true;

  PollForReadFlush(TIMEOUT_SECS(1));

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherTransportUSB::Disconnect()
{
  FDBG(DBG_EVENT, "`%s'", devicename.c_str());

  const std::lock_guard<std::mutex> lock(mutex);

  if (!in_session)
    {
      FDBG(DBG_WARN, "not connected `%s'", devicename.c_str());
      return SANE_STATUS_DEVICE_BUSY;
    }

  /*
   * Flush through whatever might be cached for a little while.
   *
   */
  PollForReadFlush(TIMEOUT_SECS(5));

  SANE_Status res = ExecStopSession();
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_WARN, "failed to stop session `%s'", devicename.c_str());
      // continue however
    }

  in_session = false;

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherTransportUSB::Release()
{
  FDBG(DBG_EVENT, "`%s'", devicename.c_str());

  const std::lock_guard<std::mutex> lock(mutex);

  if (in_session)
    {
      (void)Disconnect();
    }

  sanei_usb_close(fd);
  fd = -1;

  is_acquired = false;

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherTransportUSB::ExecStartSession()
{
  SANE_Status res = sanei_usb_control_msg(fd, USB_DIR_IN | USB_TYPE_VENDOR,
					  BROTHER_USB_REQ_STARTSESSION,
					  2, 0, 5, small_buffer);

  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to send start session sequence: %s.", devicename.c_str());
      return res;
    }

  BrotherSessionResponse resp;

  if (DecodeSessionResp(small_buffer, 5, resp) != SANE_STATUS_GOOD)
    {
      FDBGNP(DBG_SERIOUS, "start session response is invalid.");
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
   SANE_Status res = sanei_usb_control_msg (fd, USB_DIR_IN | USB_TYPE_VENDOR,
                               BROTHER_USB_REQ_STOPSESSION,
                               2, 0, 5, small_buffer);

  if (res != SANE_STATUS_GOOD)
    {

      FDBGNP(DBG_SERIOUS, "failed to send stop sequence.");
      return res;
    }

  BrotherSessionResponse resp;

  if (DecodeSessionResp(small_buffer, 5, resp) != SANE_STATUS_GOOD)
    {
      FDBGNP(DBG_SERIOUS, "BrotherUSBDriver::StopSession: stop session response is invalid.");
      return SANE_STATUS_IO_ERROR;
    }

  if (!resp.ready)
    {
      FDBGNP(DBG_WARN, "scanner indicates not ready.");
      return SANE_STATUS_DEVICE_BUSY;
    }

  return SANE_STATUS_GOOD;
}

SANE_Bool BrotherTransportUSB::IsConnected()
{
  return in_session? SANE_TRUE: SANE_FALSE;
}

SANE_Bool BrotherTransportUSB::IsAcquired()
{
  return is_acquired? SANE_TRUE: SANE_FALSE;
}

SANE_Status BrotherTransportUSB::PollDataFromScanner()
{
  FDBGNP (DBG_EVENT, "");

  /*
   * First, see if we need to do a read from the device to satisfy the read.
   * Only if we have space to read though. Obviously.
   *
   * We will place a lower bound on the amount of space. BROTHER_READ_BUFFER_LEN is set to 16k
   * so there's really no need to read if we already have 15k to work on.
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
      FDBG(DBG_DETAIL, "attempting read, space for %zu bytes", bytes_to_read);

      useconds_t max_time = 0;

      SANE_Status res = PollForRead (data_buffer + data_buffer_bytes, &bytes_to_read, &max_time);
      if (res != SANE_STATUS_GOOD)
	{
	  FDBG(DBG_SERIOUS, "read aborted due to read error: %d.", res);
	  return res;
	}

      data_buffer_bytes += bytes_to_read;
      FDBG(DBG_DETAIL, "read %zu bytes, now buffer has %zu bytes", bytes_to_read,
	   data_buffer_bytes);
    }
  return SANE_STATUS_GOOD;
}
/*
 * Just suck any cached data from the read stream and discard it.
 *
 * We will continue to read data until we get nothing for a maximum of
 * max_time time.
 *
 * Hard maximum of 10 seconds though.
 *
 */
SANE_Status BrotherTransportUSB::PollForReadFlush (useconds_t max_time)
{
  useconds_t orig_time = max_time;
  useconds_t hard_time_limit = TIMEOUT_SECS(10);

  FDBG(DBG_EVENT, "`%s'", devicename.c_str());

  do
    {
      /*
       * We use this to find out how much time elapsed during the next
       * read. We will use it to update the hard limit.
       *
       */
      useconds_t start_time = max_time;
      max_time = TIMEOUT_MILLISECS(100);
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
      else
	{
	  break;	// didn't read anything.
	}
    }
  while (max_time && hard_time_limit);

  return SANE_STATUS_GOOD;
}

/*
 * If you only want to read once, then set *max_time to 0.
 * max_time will contain the remaining time on return. If timed out, then it will be 0.
 *
 */
SANE_Status BrotherTransportUSB::PollForRead(SANE_Byte *buffer, size_t *buf_len,
					     useconds_t *max_time)
{
  SANE_Status res;

  FDBG(DBG_EVENT, "`%s'", devicename.c_str());

  useconds_t timeout = 0;
  size_t read_amt;
  do
    {
      read_amt = *buf_len;

      res = sanei_usb_read_bulk(fd, buffer, &read_amt);
      if (res == SANE_STATUS_GOOD)
	{
	  break;
	}

      if (res != SANE_STATUS_EOF)
	{
	  FDBG(DBG_WARN, "failed to poll for read: %d", res);
	  *buf_len = 0;
	  *max_time -= timeout;
	  return res;
	}

      usleep(TIMEOUT_MILLISECS(30));
      timeout += TIMEOUT_MILLISECS(30);
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

SANE_Status BrotherTransportUSB::SendCommand(const SANE_Byte *send_buffer, size_t send_size)
{
  SANE_Status res;

  FDBGNP (DBG_EVENT, "");

  size_t buf_size = send_size;
  res = sanei_usb_write_bulk (fd, send_buffer, &buf_size);
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to write command: %d", res);
      return res;
    }

  if (buf_size != send_size)
    {
      FDBGNP(DBG_SERIOUS, "failed to send all command.");
      return SANE_STATUS_IO_ERROR;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherTransportUSB::ReceiveResponse(SANE_Byte *rec_buffer, size_t *rec_size,
						 useconds_t *rec_timeout)
{
  SANE_Status res;

  res = PollForRead (rec_buffer, rec_size, rec_timeout);
  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to read response packet: %d", res);
      return res;
    }

  return SANE_STATUS_GOOD;
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

SANE_Status BrotherTransportUSB::SetAsyncIOMode(bool async_io)
{
  const std::lock_guard<std::mutex> lock(mutex);
  async_io_mode = async_io;

  return SANE_STATUS_GOOD;
}

bool BrotherTransport::GetAsyncIOMode()
{
  return async_io_mode;
}

SANE_Status BrotherTransportUSB::ReadButtons(SANE_Int &button_mask)
{
  SANE_Status res = sanei_usb_control_msg(fd, USB_DIR_IN | USB_TYPE_VENDOR,
					  BROTHER_USB_REQ_BUTTONSTATE,
					  0, 0, 255, small_buffer);

  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to send check sensor sequence: %s.", devicename.c_str());
      return res;
    }

  BrotherButtonQueryResponse resp;

  if (DecodeButtonQueryResp(small_buffer, 4, resp) != SANE_STATUS_GOOD)
    {
      FDBGNP(DBG_SERIOUS, "button state response is invalid.");
      return SANE_STATUS_IO_ERROR;
    }

  if (resp.has_button_press == BROTHER_SENSOR_NONE)
    {
      button_mask = BROTHER_SENSOR_NONE;
      return SANE_STATUS_GOOD;
    }

  FDBGNP(DBG_DETAIL, "scanner indicates a button was pushed.");

  /*
   * Now we query the button state.
   * The request is the same apparently but we get a different response.
   * Not really sure I understand because it seems stateful this but it is what we observe.
   *
   */
  res = sanei_usb_control_msg(fd, USB_DIR_IN | USB_TYPE_VENDOR,
			      BROTHER_USB_REQ_BUTTONSTATE,
			      0, 0, 255, small_buffer);

  if (res != SANE_STATUS_GOOD)
    {
      FDBG(DBG_SERIOUS, "failed to send check sensor sequence: %s.", devicename.c_str());
      return res;
    }

  /*
   * And the second response is a bit longer and button specific.
   *
   */
  BrotherButtonStateResponse state_resp;

  res = DecodeButtonStateResp(small_buffer, 9, state_resp);
  if (res != SANE_STATUS_GOOD)
    {
      FDBGNP(DBG_WARN, "button info response is invalid.");
      return res;
    }

  button_mask = state_resp.button_value;

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherTransportUSB::DecodeButtonStateResp(const SANE_Byte *data, size_t data_len,
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

SANE_Status BrotherTransportUSB::DecodeButtonQueryResp(const SANE_Byte *data, size_t data_len,
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
