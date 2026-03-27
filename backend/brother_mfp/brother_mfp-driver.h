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

//#include "brother_mfp-encoder.h"

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
  BrotherTransport():
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

  virtual SANE_Status SendCommand (const SANE_Byte *send_buffer, size_t send_size) = 0;

  virtual SANE_Status ReceiveResponse (SANE_Byte *rec_buffer, size_t *rec_size,
				       useconds_t *rec_timeout) = 0;

  virtual SANE_Status SendAndReceive (const SANE_Byte *send_buffer, size_t send_size,
				      SANE_Byte *rec_buffer, size_t *rec_size,
				      useconds_t *rec_timeout) = 0;

  virtual SANE_Status ReadScanData(SANE_Byte *buffer, size_t buffer_len, size_t &bytes_read) = 0;

  virtual SANE_Status SetAsyncIOMode(bool async_io);

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

  SANE_Status Acquire();

  SANE_Status Release();

  SANE_Bool IsAcquired();

  SANE_Status Connect();

  SANE_Status Disconnect();

  SANE_Bool IsConnected();

  SANE_Status SendCommand(const SANE_Byte *send_buffer, size_t send_size);

  SANE_Status ReceiveResponse(SANE_Byte *rec_buffer, size_t *rec_size, useconds_t *rec_timeout);

  SANE_Status SendAndReceive(const SANE_Byte *send_buffer, size_t send_size, SANE_Byte *rec_buffer,
			     size_t *rec_size, useconds_t *rec_timeout);

  SANE_Status ReadScanData(SANE_Byte *buffer, size_t buffer_len, size_t &bytes_read)
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

  void Cancel()
  {
    want_cancel = true;
    const std::lock_guard<std::mutex> lock(mutex);
    want_cancel = false;
  }
private:
  SANE_Status ExecStartSession ();
  SANE_Status ExecStopSession ();

  SANE_Status DecodeSessionResp (const SANE_Byte *data, size_t data_len,
				 BrotherSessionResponse &response);

  SANE_Status PollForReadFlush (useconds_t max_time);

  SANE_Status PollForRead (SANE_Byte *buffer, size_t *buf_len,
                           useconds_t *max_time);

  SANE_Status PollDataFromScanner();

  SANE_Bool IsCancelled()
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

/*
 * Transport base and implementations.
 *
 * We have a base which lays the groundwork for our
 * scanner communications and the interface for the SANE
 * frontend to talk with the device.
 *
 * We have implementations for both USB and Ethernet.
 *
 */
//class BrotherDriver
//{
//public:
//  explicit BrotherDriver (BrotherFamily family, SANE_Word capabilities);
//
//  BrotherDriver(const BrotherDriver &) = delete;
//  BrotherDriver &operator=(const BrotherDriver &) = delete;
//
//  virtual ~BrotherDriver();
//
//  // TODO: I don't like the fact that multiple are being passed around.
//  // Maybe use a shared smart pointer?
//  SANE_Parameters params;
//
//  /*
//   *  Will include the INIT sequence.
//   *
//   */
//  virtual SANE_Status Connect () = 0;
//
//  /*
//   * Will ensure that we stop a session if active.
//   *
//   */
//  virtual SANE_Status Disconnect () = 0;
//
//  /*
//   * Will open a session.
//   *
//   */
//  virtual SANE_Status StartScan () = 0;
//
//  virtual SANE_Status CancelScan () = 0;
//
//  virtual SANE_Status CheckSensor(BrotherSensor &status) = 0;
//
//  virtual SANE_Status ReadScanData (SANE_Byte *data, size_t data_len,
//                                    size_t *bytes_read) = 0;
//
//  virtual SANE_Status SetAsyncIOMode(bool async_io) = 0;
//
//  /*
//   * Parameter setting.
//   *
//   */
//  SANE_Status SetSource (BrotherSource source)
//  {
//    return encoder->SetSource (source);
//  }
//
//  SANE_Status SetScanMode (BrotherScanMode scan_mode)
//  {
//    return encoder->SetScanMode (scan_mode);
//  }
//
//  SANE_Status SetRes (SANE_Int x, SANE_Int y)
//  {
//    return encoder->SetRes (x, y);
//  }
//
//  SANE_Status SetContrast (SANE_Int contrast)
//  {
//    return encoder->SetContrast (contrast);
//  }
//
//  SANE_Status SetBrightness (SANE_Int brightness)
//  {
//    return encoder->SetBrightness (brightness);
//  }
//
//  SANE_Status SetCompression (SANE_Bool compression)
//  {
//    return encoder->SetCompression (compression);
//  }
//
//  SANE_Status SetScanDimensions (SANE_Int pixel_x_offset, SANE_Int pixel_x_width,
//                                 SANE_Int pixel_y_offset, SANE_Int pixel_y_height)
//  {
//    return encoder->SetScanDimensions (pixel_x_offset, pixel_x_width,
//				       pixel_y_offset, pixel_y_height);
//  }
//
//protected:
//  BrotherFamily family;
//  SANE_Word capabilities;
//  BrotherEncoder *encoder;
//};

//class BrotherUSBDriver : public BrotherDriver
//{
//public:
//  BrotherUSBDriver (const char *devicename, BrotherFamily family, SANE_Word capabilities);
//
//  ~BrotherUSBDriver ();
//
//  SANE_Status Connect () override;
//
//  SANE_Status Disconnect () override;
//
//  SANE_Status StartScan () override;
//
//  SANE_Status CancelScan () override;
//
//  SANE_Status CheckSensor(BrotherSensor &status) override;
//
//  SANE_Status ReadScanData (SANE_Byte *data, size_t data_len,
//                            size_t *bytes_read) override;
//
//  SANE_Status SetAsyncIOMode(bool async_io) override;
//
//private:
//  SANE_Status StartSession ();
//  SANE_Status StopSession ();
//  SANE_Status ExecStartSession ();
//  SANE_Status ExecStopSession ();
//  SANE_Status Init ();
//
//  SANE_Status PollForReadFlush (useconds_t max_time);
//  SANE_Status PollForRead (SANE_Byte *buffer, size_t *buf_len,
//                           useconds_t *max_time);
//
//  SANE_Status PollDataFromScanner();
//
//  SANE_Status DecodeReadBuffer(SANE_Byte *data, size_t max_length, size_t *length);
//
//  SANE_Status StartScan_BasicParameter();
//  SANE_Status StartScan_SourceSelect();
//  SANE_Status StartScan_Parameter();
//  SANE_Status StartScan_ParameterBlank();
//
//  SANE_Status CancelScan_Internal ();
//
//  enum ScanState
//  {
//    SCANSTATE_IDLE,		// no doing anything.
//    SCANSTATE_ACQUIRING,	// set after Start() called. (if IDLE or EOF)
//    SCANSTATE_EOF,		// set when end of image received (if ACQUIRING)
//  } scan_state;
//
//  bool is_open;
//  bool in_session;
//  char *devicename;
//  SANE_Int next_frame_number;
//  SANE_Int fd;
//
//  SANE_Byte small_buffer[1024];
//  SANE_Byte *data_buffer;
//  size_t data_buffer_bytes;
//
//  bool async_io_mode;
//
//  /*
//   * These manage the relationship of CancelScan() to all other functionality.
//   * Any public function (other than CancelScan) must take the lock and monitor
//   * want_cancel and exit as quickly as possible if it goes true, although it is OK
//   * to call CancelScan_Internal() to perform the cancel and return SANE_STATUS_CANCELLED
//   * if that flag is seen, while retaining the mutex. In this case, CancelScan()
//   * will become a no-op. No other function in this class can call CancelScan()!
//   * If you want to do a cancel, call CancelScan_Internal() to avoid deadlocking.
//   *
//   */
//  bool want_cancel;
//  std::mutex mutex;
//};

//class BrotherNetworkDriver : public BrotherDriver
//{
//public:
//  BrotherNetworkDriver (const char *devicename, BrotherFamily family, SANE_Word capabilities);
//
//  ~BrotherNetworkDriver ();
//
//  SANE_Status Connect () override;
//
//  SANE_Status Disconnect () override;
//
//  SANE_Status StartScan () override;
//
//  SANE_Status CancelScan () override;
//
//  SANE_Status CheckSensor(BrotherSensor &status) override;
//
//  SANE_Status ReadScanData (SANE_Byte *data, size_t data_len,
//                            size_t *bytes_read) override;
//
//private:
//  SANE_Status Init ();
//
//  SANE_Status SNMPRegisterButtons ();
//
////  SANE_Status PollForReadFlush (useconds_t max_time);
////  SANE_Status PollForRead (SANE_Byte *buffer, size_t *buf_len,
////                           useconds_t *max_time);
////  bool is_open;
////  bool in_session;
////  bool is_scanning;
////  bool was_cancelled;
//  char *devicename;
////  SANE_Int next_frame_number;
//
//  SANE_Byte small_buffer[1024];
//  SANE_Byte *data_buffer;
////  size_t data_buffer_bytes;
//
////  bool out_of_docs;
//
//  int sockfd;
//
//};
