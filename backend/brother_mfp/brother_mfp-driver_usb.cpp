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

#define BROTHER_READ_BUFFER_LEN (16 * 1024)


/*-----------------------------------------------------------------*/

SANE_Status BrotherUSBDriver::Connect ()
{
  SANE_Status res;

  DBG (DBG_EVENT, "BrotherUSBDriver::Connect: `%s'\n", devicename);

  if (!encoder)
    {
      DBG (DBG_WARN, "BrotherUSBDriver::Connect: no encoder\n");
      return SANE_STATUS_UNSUPPORTED;
    }

  if (is_open)
    {
      DBG (DBG_WARN, "BrotherUSBDriver::Connect: already open `%s'\n",
           devicename);
      return SANE_STATUS_DEVICE_BUSY;
    }

  /*
   * We set this here so that Disconnect() can be used to tidy up the device in
   * the event of a failure during the open.
   *
   */
  is_open = true;
  next_frame_number = 0;
  out_of_docs = false;

  res = sanei_usb_open (devicename, &fd);

  if (res != SANE_STATUS_GOOD)
    {
      Disconnect ();
      DBG (DBG_SERIOUS,
           "BrotherUSBDriver::Connect: couldn't open device `%s': %s\n",
           devicename, sane_strstatus (res));
      return res;
    }

  /*
   * Initialisation.
   *
   */
  res = StartSession ();
  if (res != SANE_STATUS_GOOD)
    {
      DBG (DBG_SERIOUS,
           "BrotherUSBDriver::Connect: failed to start session: %d\n", res);
      Disconnect ();
      return res;
    }

  /*
   * Execute the intialisation sequence.
   *
   */
  SANE_Status init_res = Init ();

  res = StopSession ();
  if (res != SANE_STATUS_GOOD)
    {
      DBG (DBG_SERIOUS,
           "BrotherUSBDriver::Connect: failed to stop session: %d\n", res);
      Disconnect ();
      return res;
    }

  /*
   * If the init sequence failed, then that's not good either.
   *
   */
  if (init_res != SANE_STATUS_GOOD)
    {
      DBG (DBG_SERIOUS,
           "BrotherUSBDriver::Connect: failed to send init session: %d\n",
           init_res);
      Disconnect ();
    }

  return init_res;
}

SANE_Status BrotherUSBDriver::Disconnect ()
{
  DBG (DBG_EVENT, "BrotherUSBDriver::Disconnect: `%s'\n", devicename);

  if (!encoder)
    {
      DBG (DBG_WARN, "BrotherUSBDriver::Disconnect: no encoder\n");
      return SANE_STATUS_UNSUPPORTED;
    }

  if (!is_open)
    {
      DBG (DBG_WARN, "BrotherUSBDriver::Disconnect: not open `%s'\n", devicename);
      return SANE_STATUS_DEVICE_BUSY;
    }

  /*
   * If we are in a session, try to close it off.
   *
   */
  if (in_session)
    {
      SANE_Status res = StopSession();
      if (res != SANE_STATUS_GOOD)
        {
          DBG (DBG_WARN, "BrotherUSBDriver::Disconnect: failed to stop session `%s'\n", devicename);
          // continue however
        }
    }

  is_open = false;

  sanei_usb_close (fd);
  fd = 0;

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherUSBDriver::StartSession ()
{
  /*
   * Initialisation.
   *
   */
  if (!is_open)
    {
      DBG (DBG_WARN, "BrotherUSBDriver::StartSession: device is not open: %s",
           devicename);
      return SANE_STATUS_INVAL;
    }

  if (in_session)
    {
      DBG (DBG_WARN, "BrotherUSBDriver::StartSession: session already open: %s",
           devicename);
      return SANE_STATUS_GOOD;
    }

  SANE_Status res = ExecStartSession ();
  if (res != SANE_STATUS_GOOD)
    {

      /*
       * Perhaps a session is in progress from a prior crash?
       * We will try to stop then restart.
       *
       */
      res = ExecStopSession ();
      if (res != SANE_STATUS_GOOD)
        {
          DBG (DBG_WARN,
               "BrotherUSBDriver::StartSession: failed to send stop session sequence for restart: %s.\n",
               devicename);
          return SANE_STATUS_DEVICE_BUSY;
        }

      res = ExecStartSession ();
      if (res != SANE_STATUS_GOOD)
        {
          DBG (DBG_WARN,
               "BrotherUSBDriver::StartSession: failed to send start session sequence for restart: %s.\n",
               devicename);
          return SANE_STATUS_DEVICE_BUSY;
        }
    }

    in_session = true;

    return SANE_STATUS_GOOD;
}

SANE_Status BrotherUSBDriver::StopSession ()
{
  /*
   * Initialisation.
   *
   */
  if (!is_open)
    {
      DBG (DBG_WARN, "BrotherUSBDriver::StopSession: device not open: %s.\n",
           devicename);
      return SANE_STATUS_INVAL;
    }

  if (!in_session)
    {
      DBG (DBG_WARN, "BrotherUSBDriver::StopSession: not in session: %s.\n",
           devicename);
      return SANE_STATUS_GOOD;
    }

  SANE_Status res = ExecStopSession ();
  if (res != SANE_STATUS_GOOD)
    {
      DBG (DBG_WARN, "BrotherUSBDriver::StopSession: failed to send stop sequence.\n");
      return res;
    }

  in_session = false;

  return SANE_STATUS_GOOD;
}


SANE_Status BrotherUSBDriver::ExecStartSession ()
{
  SANE_Status res = sanei_usb_control_msg (fd, USB_DIR_IN | USB_TYPE_VENDOR,
                                           BROTHER_USB_REQ_STARTSESSION,
                                           2, 0, 5, small_buffer);

  if (res != SANE_STATUS_GOOD)
    {
      DBG (DBG_WARN,
          "BrotherUSBDriver::StartSession: failed to send start session sequence: %s.\n",
          devicename);
      return res;
    }

  BrotherSessionResponse resp;

  if (encoder->DecodeSessionResp (small_buffer, 5, resp) != DECODE_STATUS_GOOD)
  {
    DBG (DBG_WARN,
        "BrotherUSBDriver::StartSession: start session response is invalid.\n");
    return SANE_STATUS_IO_ERROR;
  }

  if (!resp.ready)
    {
      DBG (DBG_WARN,
          "BrotherUSBDriver::StartSession: scanner indicates not ready.\n");
      return SANE_STATUS_DEVICE_BUSY;
    }

  return SANE_STATUS_GOOD;
}


SANE_Status BrotherUSBDriver::ExecStopSession ()
{
   SANE_Status res = sanei_usb_control_msg (fd, USB_DIR_IN | USB_TYPE_VENDOR,
                               BROTHER_USB_REQ_STOPSESSION,
                               2, 0, 5, small_buffer);

  if (res != SANE_STATUS_GOOD)
    {

      DBG (DBG_WARN,
           "BrotherUSBDriver::StopSession: failed to send stop sequence.\n");
      return res;
    }

  BrotherSessionResponse resp;

  if (encoder->DecodeSessionResp (small_buffer, 5, resp) != DECODE_STATUS_GOOD)
  {
    DBG (DBG_WARN,
        "BrotherUSBDriver::StopSession: stop session response is invalid.\n");
    return SANE_STATUS_IO_ERROR;
  }

  if (!resp.ready)
    {
      DBG (DBG_WARN,
          "BrotherUSBDriver::StopSession: scanner indicates not ready.\n");
      return SANE_STATUS_DEVICE_BUSY;
    }

  return SANE_STATUS_GOOD;
}

/* 
 * Fill `data_buffer` by reading from the USB device.
 */
SANE_Status BrotherUSBDriver::PollDataFromScanner()
{
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
      DBG (DBG_EVENT,
           "BrotherUSBDriver::ReadScanData: attempting read, space for %zu bytes\n",
           bytes_to_read);

      useconds_t max_time = 0;

      SANE_Status res = PollForRead (data_buffer + data_buffer_bytes, &bytes_to_read, &max_time);
      if (res != SANE_STATUS_GOOD)
        {
          DBG (DBG_EVENT, "BrotherUSBDriver::ReadScanData: read aborted due to read error: %d.\n", res);
          (void)CancelScan();
          return res;
        }

      data_buffer_bytes += bytes_to_read;
      DBG (DBG_DETAIL,
           "BrotherUSBDriver::ReadScanData: read %zu bytes, now buffer has %zu bytes\n",
           bytes_to_read, data_buffer_bytes);
    }
  return SANE_STATUS_GOOD;
}

/* 
 * Try to fill `data` with scanlines by decoding `data_buffer`. Successfully
 * decoded data will be removed from `data_buffer`.
 * 
 */
DecodeStatus BrotherUSBDriver::DecodeReadBuffer(SANE_Byte *data, size_t max_length, size_t *length)
{
  size_t data_consumed = 0;

  DecodeStatus dec_ret = encoder->DecodeScanData (data_buffer,
                                                   data_buffer_bytes,
                                                   &data_consumed,
                                                   data,
                                                   max_length,
                                                   length);

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

  DBG (DBG_DETAIL,
       "BrotherUSBDriver::ReadScanData: decoder consumes %zu bytes and writes %zu bytes, returning %s\n",
       data_consumed,
       *length,
       encoder->DecodeStatusToString(dec_ret));

  return dec_ret;
}

SANE_Status BrotherUSBDriver::ReadScanData (SANE_Byte *data, size_t max_length, size_t *length)
{
  SANE_Status res;
  *length = 0;

  DBG (DBG_EVENT, "BrotherUSBDriver::ReadScanData: `%s'\n", devicename);

  if (!encoder)
    {
      DBG (DBG_WARN, "BrotherUSBDriver::ReadScanData: no encoder\n");
      return SANE_STATUS_UNSUPPORTED;
    }

  /*
   * Some checks on our status.
   *
   */
  if (was_cancelled)
    {
      return SANE_STATUS_CANCELLED;
    }

  if (!is_scanning)
    {
      return SANE_STATUS_EOF;
    }

  const size_t bytes_per_frame = params.bytes_per_line*params.lines;
  if (pad_or_crop_in_progress)
    {
      /*
       * The scanner has delivered either more or less lines than expected,
       * which we handle by cropping / padding. This flag indicates that such
       * an action is in progress. We might need to deliver more padded lines
       * or send a final EOF.
       */
      if (bytes_written_this_page < bytes_per_frame)
        {
          RepeatPreviousScanline(data, max_length, length);
          bytes_written_this_page += *length;
          return SANE_STATUS_GOOD;
        }
      else
        {
          /* 
           * The code that set pad_or_crop_in_progress has already absorbed
           * the EOF block.
           */
          pad_or_crop_in_progress = false;
          return SANE_STATUS_EOF;
        }
    }

  /*
   * Everything is good. Let's get on with it.
   *
   */
  if (!in_session)
    {
      DBG (DBG_SERIOUS, "BrotherUSBDriver::ReadScanData: NULL handle passed.\n");
      return SANE_STATUS_INVAL;
    }

  if (!is_scanning)
    {
      DBG (DBG_EVENT, "BrotherUSBDriver::ReadScanData: is not scanning.\n");
      return SANE_STATUS_CANCELLED;

    }

  /* First we try to fill the output buffer from scanned data. 
   * No need to read from USB when we still have plenty of data to process. */
  DecodeStatus dec_ret = DecodeReadBuffer(data, max_length, length);
  if (*length > 0)
    {
      /* We could call DecodeReadBuffer() again if there is space for at least
       * one more scanline, but that requires even more careful EOF and buffer
       * handling. */
      DBG(DBG_DETAIL, "BrotherUSBDriver::ReadScanData: Filled the output buffer by processing data, not pulling from USB\n");
    }
  else
    {
      /* In this case we read from USB to further fill `data`. */
      res = PollDataFromScanner();
      if (res != SANE_STATUS_GOOD)
        {
          return res;
        }

      dec_ret = DecodeReadBuffer(data, max_length, length);
    }

  res = encoder->DecodeStatusToSaneStatus(dec_ret);

  if ((dec_ret == DECODE_STATUS_ENDOFDATA) || (dec_ret == DECODE_STATUS_ENDOFFRAME_NO_MORE)
      || (dec_ret == DECODE_STATUS_NO_DOCS))
    {
      next_frame_number++;
      out_of_docs = true;
    }
  else if (dec_ret == DECODE_STATUS_ENDOFFRAME_WITH_MORE)
    {
      next_frame_number++;
      encoder->NewPage();
    }

  /* Store the most recent scanline in case we need it for padding */
  const size_t bytes_per_line = (size_t)params.bytes_per_line;
  if (*length >= bytes_per_line)
    memcpy(previous_scanline, data + *length - bytes_per_line, bytes_per_line);

  /* Handle edge case: Scanner delivered fewer lines than announced */
  if (res == SANE_STATUS_EOF && bytes_written_this_page < bytes_per_frame)
    {
      RepeatPreviousScanline(data, max_length, length);
      res = SANE_STATUS_GOOD;
      /* Set a flag to continue or EOF on the next call of sane_read() */
      pad_or_crop_in_progress = true;
    }
  /* Handle edge case: Scanner delivered more lines than announced */
  else if (res == SANE_STATUS_GOOD && bytes_written_this_page + *length > bytes_per_frame)
    {
      const size_t extra_lines = (bytes_written_this_page + *length - bytes_per_frame) / bytes_per_line;
      DBG(DBG_SERIOUS, "BrotherUSBDriver::ReadScanData: WARNING: Discarding %zu unexpected scan lines\n", extra_lines);
      /* Flush the remaining data from the reader, using the unused back part
       * of `data` as a dummy target buffer */
      *length = bytes_per_frame - bytes_written_this_page;
      SANE_Byte *unused_data_buffer = data + *length;
      size_t dummy_length = 0;
      do 
        {
          res = PollDataFromScanner();
          if (res != SANE_STATUS_GOOD)
              return res;
          auto dec_ret = DecodeReadBuffer(unused_data_buffer,
                                          max_length - *length,
                                          &dummy_length);
          res = encoder->DecodeStatusToSaneStatus(dec_ret);
          if (dummy_length > 0)
            DBG(DBG_DEBUG, "BrotherUSBDriver::ReadScanData: WARNING: Discarding another %zu lines\n", dummy_length / bytes_per_line);
        }
      while (res == SANE_STATUS_GOOD);
      if (res == SANE_STATUS_EOF)
      {
        /* Return the remaining data and send an EOF on the next read */
        pad_or_crop_in_progress = true;
        res = SANE_STATUS_GOOD;
      }
    }

  bytes_written_this_page += *length;

  return res;
}

/* Pad the bottom of the output image by repeating the final scanline.
 * This is done if the printer delivered fewer scanlines than announced. */
void BrotherUSBDriver::RepeatPreviousScanline(SANE_Byte *data, size_t max_length, size_t *length)
  {
    const auto bytes_per_line = params.bytes_per_line;
    const size_t lines_missing = params.lines - (bytes_written_this_page / bytes_per_line);
    const size_t lines_to_write = std::min(lines_missing, max_length / bytes_per_line);
    DBG(DBG_SERIOUS, "BrotherUSBDriver::ReadScanData: WARNING: Scanner delivered %zu fewer lines than announced, padding with previous line\n", lines_missing);
    for (size_t i=0; i < lines_to_write; i++)
      {
        memcpy(&data[i*bytes_per_line], previous_scanline, bytes_per_line);
      }
    DBG(DBG_DEBUG, "BrotherUSBDriver::ReadScanData: Added %zu padded scanlines\n", lines_to_write);
    *length += lines_to_write * bytes_per_line;
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
SANE_Status BrotherUSBDriver::PollForReadFlush (useconds_t max_time)
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
          DBG (DBG_WARN,
               "BrotherUSBDriver::PollForReadFlush: failed to flush poll: %d\n",
               res);
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
 */
SANE_Status BrotherUSBDriver::PollForRead (SANE_Byte *buffer, size_t *buf_len,
                                           useconds_t *max_time)
{
  SANE_Status res;

  DBG (DBG_EVENT, "BrotherUSBDriver::PollForRead: `%s'\n", devicename);

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
          DBG (DBG_WARN, "BrotherUSBDriver::PollForRead: failed to poll for read: %d\n", res);
          *max_time -= timeout;
          return res;
        }

      /*
       * TODO: Is this a reasonable time?
       *
       */
      usleep(100 * 1000);
      timeout += (100 * 1000);
    } while (timeout < *max_time);

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

SANE_Status BrotherUSBDriver::Init ()
{
  SANE_Status res;

  DBG (DBG_EVENT, "BrotherUSBDriver::Init: `%s'\n", devicename);

  /*
   * First wait for a second, flushing stuff out.
   *
   */
  res = PollForReadFlush (TIMEOUT_SECS(1));
  if (res != SANE_STATUS_GOOD)
    {
      DBG (DBG_WARN, "BrotherUSBDriver::Init: initial flush failure: %d\n",
           res);
      return res;
    }

  /*
   * Execute Query request.
   *
   */
  size_t packet_len = 0;
  DecodeStatus dec_ret = encoder->EncodeQueryBlock (small_buffer,
                                                    sizeof(small_buffer),
                                                    &packet_len);
  if (dec_ret != DECODE_STATUS_GOOD)
    {
      DBG (DBG_SERIOUS,
           "BrotherUSBDriver::Init: failed to generate query block: %d\n",
           dec_ret);
      (void) CancelScan ();
      return SANE_STATUS_INVAL;
    }

  size_t buf_size = packet_len;
  res = sanei_usb_write_bulk (fd, small_buffer, &buf_size);
  if (res != SANE_STATUS_GOOD)
    {
      DBG (DBG_SERIOUS,
           "BrotherUSBDriver::Init: failed to send query block: %d\n",
           res);
      (void) CancelScan ();
      return res;
    }

  if (buf_size != packet_len)
    {
      DBG (DBG_SERIOUS, "BrotherUSBDriver::Init: failed to write query block\n");
      (void) CancelScan ();
      return SANE_STATUS_IO_ERROR;
    }

  /*
   * Try to read the response.
   *
   */
  buf_size = sizeof(small_buffer);
  useconds_t timeout = TIMEOUT_SECS(8);

  res = PollForRead (small_buffer, &buf_size, &timeout);
  if (res != SANE_STATUS_GOOD)
    {
      DBG (
      DBG_SERIOUS,
           "BrotherUSBDriver::Init: failed to read query block response: %d\n", res);
      (void) CancelScan ();
      return res;
    }

  BrotherQueryResponse query_resp;

  dec_ret = encoder->DecodeQueryBlockResp (small_buffer, buf_size, query_resp);
  if (dec_ret != DECODE_STATUS_GOOD)
    {
      DBG (DBG_SERIOUS,
           "BrotherUSBDriver::Init: failed to read query block response: %d\n",
           dec_ret);
      (void) CancelScan ();
      return res;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherUSBDriver::CancelScan ()
{
  SANE_Status res;

  DBG (DBG_EVENT, "BrotherUSBDriver::CancelScan: `%s'\n", devicename);

  if (!encoder)
    {
      DBG (DBG_WARN, "BrotherUSBDriver::CancelScan: no encoder\n");
      return SANE_STATUS_UNSUPPORTED;
    }

  if (!is_scanning)
    {
      DBG (DBG_WARN, "BrotherUSBDriver::CancelScan: not scanning `%s'\n",
           devicename);
      return SANE_STATUS_GOOD;
    }

  is_scanning = false;
  was_cancelled = true;
  next_frame_number = 0;
  out_of_docs = false;

  /*
   * Send the cancel sequence.
   *
   * 0x1b52
   *
   * We could test to see if we have actually requested a scan
   * but it looks like sending the cancel scan code is OK if we are not.
   * It's more complicated to add code to check than it is to just send it.
   *
   */
  (void) memcpy (small_buffer, "\x1b" "R", 2);
  size_t buf_size = 2;

  res = sanei_usb_write_bulk (fd, small_buffer, &buf_size);
  if (res != SANE_STATUS_GOOD)
    {
      DBG (DBG_SERIOUS,
           "BrotherUSBDriver::CancelScan: failed to send cancel session: %d\n",
           res);
      return res;
    }

  if (buf_size != 2)
    {
      DBG (DBG_SERIOUS, "BrotherUSBDriver::CancelScan: failed to write cancel session\n");
      return SANE_STATUS_IO_ERROR;
    }

  /*
   * Flush out any buffered scan data.
   *
   */
  res = PollForReadFlush (TIMEOUT_SECS(1));
  if (res != SANE_STATUS_GOOD)
    {
      DBG (DBG_WARN, "BrotherUSBDriver::CancelScan: initial flush failure: %d\n",
           res);
      return res;
    }

  /*
   * Finally terminate the scan session.
   *
   */
  res = StopSession ();

  if (res != SANE_STATUS_GOOD)
    {
      DBG (DBG_SERIOUS,
          "BrotherUSBDriver::CancelScan: failed to send stop session control: %d\n",
          res);
      return res;
    }

  /*
   * Free read buffer buffer.
   *
   */
  delete [] data_buffer;
  data_buffer = NULL;
  data_buffer_bytes = 0;

  delete [] previous_scanline;
  previous_scanline = nullptr;


  return SANE_STATUS_GOOD;
}

SANE_Status BrotherUSBDriver::StartScan ()
{
  SANE_Status res;

  DBG (DBG_EVENT, "BrotherUSBDriver::StartScan: `%s'\n", devicename);

  if (!encoder)
    {
      DBG (DBG_WARN, "BrotherUSBDriver::StartScan: no encoder\n");
      return SANE_STATUS_UNSUPPORTED;
    }

  /*
   * This is the count of frames we have acquired so far.
   * 0 means that we are at the start of a session so we need to
   * so all the setup.
   *
   * If not, then we have come back here to get the next frame, probably
   * from an ADF.
   *
   */
  if (next_frame_number == 0)
    {
      /*
       * This flag indicated that we exhausted all of the frames in this session.
       * Cancel is required before more scanning is possible.
       *
       */
      if (out_of_docs)
        {
          return SANE_STATUS_NO_DOCS;
        }

      if (is_scanning)
        {
          DBG (DBG_WARN, "BrotherUSBDriver::StartScan: already scanning `%s'\n",
               devicename);
          return SANE_STATUS_DEVICE_BUSY;
        }

      is_scanning = true;
      was_cancelled = false;

      /*
       * Initialisation.
       *
       */
      res = StartSession();
      if (res != SANE_STATUS_GOOD)
        {
          DBG (DBG_SERIOUS, "BrotherUSBDriver::StartScan: failed to start session: %d\n", res);
          (void)CancelScan();
          return res;
        }

      /*
       * Begin prologue.
       *
       */
      res = PollForReadFlush (TIMEOUT_SECS(1));
      if (res != SANE_STATUS_GOOD)
        {
          DBG (DBG_SERIOUS,
               "BrotherUSBDriver::StartScan: failed to read flush: %d\n", res);
          (void) CancelScan ();
          return res;
        }

      /*
       * Construct the preparatory info block.
       * This gets the scanner going for calibration, etc I think.
       *
       */
      size_t packet_len = 0;
      DecodeStatus dec_ret = encoder->EncodeBasicParameterBlock (small_buffer,
                                                                 sizeof(small_buffer),
                                                                 &packet_len);
      if (dec_ret != DECODE_STATUS_GOOD)
        {
          DBG (DBG_SERIOUS,
               "BrotherUSBDriver::StartScan: failed to generate basic param block: %d\n",
               dec_ret);
          (void) CancelScan ();
          return SANE_STATUS_INVAL;
        }

      size_t buf_size = packet_len;
      res = sanei_usb_write_bulk (fd, small_buffer, &buf_size);
      if (res != SANE_STATUS_GOOD)
        {
          DBG (DBG_SERIOUS,
               "BrotherUSBDriver::StartScan: failed to send basic parameter block: %d\n",
               res);
          (void) CancelScan ();
          return res;
        }

      if (buf_size != packet_len)
        {
          DBG (DBG_SERIOUS, "BrotherUSBDriver::StartScan: failed to write basic parameter block\n");
          (void) CancelScan ();
          return SANE_STATUS_IO_ERROR;
        }

      /*
       * Try to read the response.
       *
       */
      buf_size = sizeof(small_buffer);
      useconds_t timeout = TIMEOUT_SECS(8);

      res = PollForRead (small_buffer, &buf_size, &timeout);
      if (res != SANE_STATUS_GOOD)
        {
          DBG (
          DBG_SERIOUS,
               "BrotherUSBDriver::StartScan: failed to read prep parameter block response: %d\n", res);
          (void) CancelScan ();
          return res;
        }

      BrotherBasicParamResponse basic_param_resp;

      dec_ret = encoder->DecodeBasicParameterBlockResp (small_buffer, buf_size, basic_param_resp);
      if (dec_ret != DECODE_STATUS_GOOD)
        {
          DBG (DBG_SERIOUS,
               "BrotherUSBDriver::StartScan: failed to read prep parameter block response: %d\n",
               dec_ret);
          (void) CancelScan ();
          return res;
        }

      /*
       * Construct the Source select block.
       *
       */
      packet_len = 0;
      dec_ret = encoder->EncodeSourceSelectBlock (small_buffer, sizeof(small_buffer), &packet_len);
      if (dec_ret != DECODE_STATUS_UNSUPPORTED)
        {
          if (dec_ret != DECODE_STATUS_GOOD)
            {
              DBG (DBG_SERIOUS,
                   "BrotherUSBDriver::StartScan: failed to generate source select block: %d\n",
                   dec_ret);
              (void) CancelScan ();
              return SANE_STATUS_INVAL;
            }

          buf_size = packet_len;
          res = sanei_usb_write_bulk (fd, small_buffer, &buf_size);
          if (res != SANE_STATUS_GOOD)
            {
              DBG (DBG_SERIOUS, "BrotherUSBDriver::StartScan: failed to send source select block: %d\n", res);
              (void) CancelScan ();
              return res;
            }

          if (buf_size != packet_len)
            {
              DBG (DBG_SERIOUS, "BrotherUSBDriver::StartScan: failed to write source select block\n");
              (void) CancelScan ();
              return SANE_STATUS_IO_ERROR;
            }

          /*
           * Try to read the response.
           *
           */
          buf_size = sizeof(small_buffer);
          timeout = TIMEOUT_SECS(8);

          res = PollForRead (small_buffer, &buf_size, &timeout);
          if (res != SANE_STATUS_GOOD)
            {
              DBG (DBG_SERIOUS,
                   "BrotherUSBDriver::StartScan: failed to read source select block response: %d\n",
                   res);
              (void) CancelScan ();
              return res;
            }

          BrotherSourceStatusResponse source_resp;

          dec_ret = encoder->DecodeSourceStatusBlockResp (small_buffer, buf_size, source_resp);
          if (dec_ret != DECODE_STATUS_GOOD)
            {
              DBG (DBG_SERIOUS,
                   "BrotherUSBDriver::StartScan: source select response block invalid: %d\n",
                   dec_ret);
              (void) CancelScan ();
              return encoder->DecodeStatusToSaneStatus(dec_ret);
            }

          DBG (DBG_DETAIL,
               "BrotherUSBDriver::StartScan: Source reports readiness?: %s\n",
               source_resp.source_ready? "Yes": "No");
        }

      /*
       * Construct the MAIN parameter block. This will precipitate the actual scan.
       *
       */
      packet_len = 0;
      dec_ret = encoder->EncodeParameterBlock (small_buffer, sizeof(small_buffer), &packet_len);
      if (dec_ret != DECODE_STATUS_GOOD)
        {
          DBG (DBG_SERIOUS,
               "BrotherUSBDriver::StartScan: failed to generate param block: %d\n",
               dec_ret);
          (void) CancelScan ();
          return SANE_STATUS_INVAL;
        }

      buf_size = packet_len;
      res = sanei_usb_write_bulk (fd, small_buffer, &buf_size);
      if (res != SANE_STATUS_GOOD)
        {
          DBG (DBG_SERIOUS,
               "BrotherUSBDriver::StartScan: failed to send parameter block: %d\n",
               res);
          (void) CancelScan ();
          return res;
        }

      if (buf_size != packet_len)
        {
          DBG (DBG_SERIOUS, "BrotherUSBDriver::StartScan: failed to write parameter block\n");
          (void) CancelScan ();
          return SANE_STATUS_IO_ERROR;
        }

      /*
       * Allocate a read buffer.
       *
       */
      data_buffer = new SANE_Byte[BROTHER_READ_BUFFER_LEN];
      if (nullptr == data_buffer)
        {
          DBG (DBG_SERIOUS,
               "BrotherUSBDriver::StartScan: failed to allocate read buffer: %zu\n",
               (size_t) BROTHER_READ_BUFFER_LEN);
          (void) CancelScan ();
          return SANE_STATUS_NO_MEM;
        }
      data_buffer_bytes = 0;

      previous_scanline = new SANE_Byte[params.bytes_per_line] {};
      if (nullptr == previous_scanline)
        return SANE_STATUS_NO_MEM;
    }
  else
    {
      if (!is_scanning)
        {
          DBG (DBG_SERIOUS,
               "BrotherUSBDriver::StartScan: next image: we are no scanning!\n");
          (void) CancelScan ();
          return SANE_STATUS_INVAL;
        }

      /*
       * Check that we have documents left.
       * We will have set this flag if the device told us that there were
       * no more docs to scan.
       *
       */
      if (out_of_docs)
        {
          return SANE_STATUS_NO_DOCS;
        }

      /*
       * If this is not the first image, then we merely need
       * to ask for another image. We do this with a blank parameter block.
       * This is not necessary on all families.
       */
      size_t packet_len = 0;
      DecodeStatus dec_ret = encoder->EncodeParameterBlockBlank (small_buffer,
                                                                 sizeof(small_buffer),
                                                                 &packet_len);
      if (dec_ret != DECODE_STATUS_GOOD)
        {
          DBG (DBG_SERIOUS,
               "BrotherUSBDriver::StartScan: failed to generate param block: %d\n",
               dec_ret);
          (void) CancelScan ();
          return SANE_STATUS_INVAL;
        }

      /* packet_len == 0 encodes that no package should be sent */
      if (packet_len > 0)
      {
        size_t buf_size = packet_len;
        res = sanei_usb_write_bulk (fd, small_buffer, &buf_size);
        if (res != SANE_STATUS_GOOD)
          {
            DBG (DBG_SERIOUS,
                 "BrotherUSBDriver::StartScan: failed to send parameter block: %d\n",
                 res);
            (void) CancelScan ();
            return res;
          }

        if (buf_size != packet_len)
          {
            DBG (DBG_SERIOUS, "BrotherUSBDriver::StartScan: failed to write parameter block\n");
            (void) CancelScan ();
            return SANE_STATUS_IO_ERROR;
          }
      }
    }

  /*
   * Reset the encoder/decoder.
   *
   */
  encoder->NewPage();
  bytes_written_this_page = 0;

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherUSBDriver::CheckSensor (BrotherSensor &status)
{
  if (!encoder)
    {
      DBG (DBG_WARN, "BrotherUSBDriver::CheckSensor: no encoder\n");
      return SANE_STATUS_UNSUPPORTED;
    }

  SANE_Status res = sanei_usb_control_msg (fd, USB_DIR_IN | USB_TYPE_VENDOR,
                                           BROTHER_USB_REQ_BUTTONSTATE,
                                           0, 0, 255, small_buffer);

 if (res != SANE_STATUS_GOOD)
   {
     DBG (DBG_WARN,
         "BrotherUSBDriver::CheckSensor: failed to send check sensor sequence: %s.\n",
         devicename);
     return res;
   }

 BrotherButtonQueryResponse resp;

 if (encoder->DecodeButtonQueryResp (small_buffer, 4, resp) != DECODE_STATUS_GOOD)
 {
   DBG (DBG_WARN,
       "BrotherUSBDriver::CheckSensor: button state response is invalid.\n");
   return SANE_STATUS_IO_ERROR;
 }

 if (resp.has_button_press == BROTHER_SENSOR_NONE)
   {
     status = BROTHER_SENSOR_NONE;
     return SANE_STATUS_GOOD;
   }

 DBG (DBG_DETAIL,
     "BrotherUSBDriver::CheckSensor: scanner indicates a button was pushed.\n");

 /*
  * Now we query the button state.
  * This is done by the same request. Not sure why the first response even
  * exists, but this is how it appears to work. Also note that there may be
  * default ("no button pressed") responses in between, hence the loop and
  * timeout.
  *
  */
  useconds_t time_left = TIMEOUT_SECS(2);
  static const useconds_t deltaT = TIMEOUT_MILLISECS(100);
  
  while (time_left > deltaT)
    {
      res = sanei_usb_control_msg (fd, USB_DIR_IN | USB_TYPE_VENDOR,
                                  BROTHER_USB_REQ_BUTTONSTATE,
                                  0, 0, 255, small_buffer);

    if (res != SANE_STATUS_GOOD)
      {
        DBG (DBG_WARN,
            "BrotherUSBDriver::CheckSensor: failed to send check sensor sequence: %s.\n",
            devicename);
          return res;
      }

      /*
      * The second response is a bit longer and button specific.
      *
      */
      BrotherButtonStateResponse state_resp;

      DecodeStatus dec_ret = encoder->DecodeButtonStateResp (small_buffer, 9, state_resp);

      if (dec_ret == DECODE_STATUS_GOOD)
        {
          status = state_resp.button_value;
          return SANE_STATUS_GOOD;
        }
      /* use INVAL to indicate a "no button pressed" response */
      else if (dec_ret != DECODE_STATUS_INVAL)
        {
          DBG (DBG_WARN, "BrotherUSBDriver::CheckSensor: button info response is invalid.\n");
          return encoder->DecodeStatusToSaneStatus (dec_ret);
        }
      else
        {
          usleep(100 * 1000);
          time_left -= deltaT;
        }
    }
  /* If we get here, we got 1/2 packets, but the second one never arrived */
  return SANE_STATUS_IO_ERROR;
}

BrotherDriver::BrotherDriver (BrotherFamily family, SANE_Word capabilities) :
    family (family),
    capabilities(capabilities),
    encoder(nullptr)
{
  switch (family)
  {
    case BROTHER_FAMILY_5:
      encoder = new BrotherEncoderFamily5(capabilities);
      break;

    case BROTHER_FAMILY_4:
      encoder = new BrotherEncoderFamily4(capabilities);
      break;

    case BROTHER_FAMILY_2:
      encoder = new BrotherEncoderFamily2(capabilities);
      break;

    case BROTHER_FAMILY_3:
        encoder = new BrotherEncoderFamily3(capabilities);
        break;

    case BROTHER_FAMILY_1:
    case BROTHER_FAMILY_NONE:
    default:
      DBG (DBG_SERIOUS,
           "BrotherDriver::BrotherDriver: no encoder available for family: %d.\n", family);
      break;
  }

  if (nullptr == encoder)
    {
      DBG (DBG_SERIOUS,
	   "BrotherDriver::BrotherDriver: failed to allocate encoder.\n");
    }
}

BrotherDriver::~BrotherDriver ()
{
  delete encoder;
}

BrotherUSBDriver::BrotherUSBDriver (const char *devicename, BrotherFamily family,
                                    SANE_Word capabilities) :
     BrotherDriver (family, capabilities),
     is_open (false),
     in_session (false),
     is_scanning (false),
     was_cancelled(false),
     devicename (nullptr),
     next_frame_number(0),
     fd (0),
     small_buffer {0},
     data_buffer (nullptr),
     data_buffer_bytes (0),
     bytes_written_this_page (0),
     previous_scanline(nullptr),
     out_of_docs(false),
     pad_or_crop_in_progress(false)
 {
   this->devicename = strdup(devicename);
 }

BrotherUSBDriver::~BrotherUSBDriver ()
{
  delete[] data_buffer;
  free (devicename);
}
