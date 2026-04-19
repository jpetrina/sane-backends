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

#define BUILD 0

#include "../include/sane/config.h"

#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <mutex>
#include <string>

#include "brother_mfp-common.h"

#include "../include/sane/sane.h"
#include "../include/sane/sanei.h"
#include "../include/sane/saneopts.h"
#include "../include/sane/sanei_config.h"
#include "../include/sane/sanei_usb.h"

/*-----------------------------------------------------------------*/

/*
 * Handy timeout macros for use with usecond_t-based functions.
 *
 */
#define TIMEOUT_SECS(x)         ((x) * 1000000)
#define TIMEOUT_MILLISECS(x)    ((x) * 1000)
#define TIMEOUT_MICROSECS(x)    (x)

enum BrotherTransportType
{
  BROTHERTRANSPORT_NONE,
  BROTHERTRANSPORT_USB,
  BROTHERTRANSPORT_ETHERNET
};

struct BrotherSessionResponse
{
  SANE_Bool ready;
};

class BrotherTransport
{
public:
  BrotherTransport() :
      async_io_mode(false)
  {
  }

  virtual ~BrotherTransport()
  {
  }

  // Open close device
  virtual SANE_Status Acquire() = 0;

  virtual SANE_Status Release() = 0;

  virtual SANE_Bool IsAcquired() = 0;

  // Start/stop session.
  virtual SANE_Status Connect() = 0;

  virtual SANE_Status Disconnect() = 0;

  virtual SANE_Bool IsConnected() = 0;

  virtual SANE_Status SendCommand(const SANE_Byte *send_buffer, size_t send_size) = 0;

  virtual SANE_Status ReceiveResponse(SANE_Byte *rec_buffer, size_t *rec_size,
				      useconds_t *rec_timeout) = 0;

  virtual SANE_Status SendAndReceive(const SANE_Byte *send_buffer, size_t send_size,
				     SANE_Byte *rec_buffer, size_t *rec_size,
				     useconds_t *rec_timeout) = 0;

  virtual SANE_Status ReadScanData(SANE_Byte *buffer, size_t buffer_len, size_t &bytes_read) = 0;

  virtual SANE_Status SetAsyncIOMode(bool async_io) = 0;

  virtual bool GetAsyncIOMode();

  virtual void Cancel() = 0;

  virtual SANE_Bool IsCancelled() = 0;

protected:
  bool async_io_mode;
};

class BrotherTransportUSB: public BrotherTransport
{
public:
  BrotherTransportUSB (SANE_String_Const device_name) :
      is_acquired(false),
      in_session(false),
      fd(-1),
      devicename(device_name),
      data_buffer_bytes(0),
      want_cancel(false)
  {
  }

  SANE_Status Acquire() override;

  SANE_Status Release() override;

  SANE_Bool IsAcquired() override;

  SANE_Status Connect() override;

  SANE_Status Disconnect() override;

  SANE_Bool IsConnected() override;

  SANE_Status SendCommand(const SANE_Byte *send_buffer, size_t send_size) override;

  SANE_Status ReceiveResponse(SANE_Byte *rec_buffer, size_t *rec_size, useconds_t *rec_timeout) override;

  SANE_Status SendAndReceive(const SANE_Byte *send_buffer, size_t send_size, SANE_Byte *rec_buffer,
			     size_t *rec_size, useconds_t *rec_timeout) override;

  SANE_Status ReadScanData(SANE_Byte *buffer, size_t buffer_len, size_t &bytes_read) override
  {
    SANE_Status res;

    const std::lock_guard<std::mutex> lock(mutex);

    if (IsCancelled())
      {
        return SANE_STATUS_CANCELLED;
      }

    bytes_read = 0;

    res = PollDataFromScanner();
    if (res != SANE_STATUS_GOOD)
      {
	return res;
      }

    if (IsCancelled())
      {
        return SANE_STATUS_CANCELLED;
      }

    /*
     * Copy what data we can to the provided buffer.
     *
     */
    size_t copy_len = std::min(buffer_len, data_buffer_bytes);
    memcpy(buffer, data_buffer, copy_len);
    (void)memmove(data_buffer, data_buffer + copy_len, data_buffer_bytes - copy_len);
    data_buffer_bytes -= copy_len;

    bytes_read = copy_len;

    if (IsCancelled())
      {
	return SANE_STATUS_CANCELLED;
      }
    return res;
  }

  void Cancel() override
  {
    want_cancel = true;
    const std::lock_guard<std::mutex> lock(mutex);
    want_cancel = false;
  }

  SANE_Status SetAsyncIOMode(bool async_io) override;

private:
  SANE_Status ExecStartSession();
  SANE_Status ExecStopSession();

  SANE_Status DecodeSessionResp(const SANE_Byte *data, size_t data_len,
				BrotherSessionResponse &response);

  SANE_Status PollForReadFlush(useconds_t max_time);

  SANE_Status PollForRead(SANE_Byte *buffer, size_t *buf_len, useconds_t *max_time);

  SANE_Status PollDataFromScanner();

  SANE_Bool IsCancelled() override
  {
    return want_cancel;
  }

  bool is_acquired;
  bool in_session;
  SANE_Int fd;

  std::string devicename;
  SANE_Byte small_buffer[1024];

  SANE_Byte data_buffer[BROTHER_READ_BUFFER_LEN];
  size_t data_buffer_bytes;

  /*
   * These manage the relationship of CancelScan() to all other functionality.
   * Any public function (other than CancelScan) must take the lock and monitor
   * want_cancel and exit as quickly as possible if it goes true, although it is OK
   * to call CancelScan_Internal() to perform the cancel and return SANE_STATUS_CANCELLED
   * if that flag is seen, while retaining the mutex. In this case, CancelScan()
   * will become a no-op. No other function in this class can call CancelScan()!
   * If you want to do a cancel, call CancelScan_Internal() to avoid deadlocking.
   *
   */
  bool want_cancel;
  std::mutex mutex;
};
