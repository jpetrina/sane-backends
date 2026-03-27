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

#include "../include/sane/config.h"

#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <setjmp.h>
#include <list>
#include <mutex>
#include <memory>

#include <jpeglib.h>
#include <jerror.h>

#include "brother_mfp-common.h"
#include "brother_mfp-driver.h"
#include "brother_mfp-data-decoder.h"

#include "../include/sane/sane.h"
#include "../include/sane/sanei.h"
#include "../include/sane/saneopts.h"
#include "../include/sane/sanei_config.h"
#include "../include/sane/sanei_usb.h"


/*
 * Functionality sets.
 *
 * For the moment, I am going to assume that the various proprietary
 * drivers follow the broad protocol versions and I will reflect this
 * until I see different.
 *
 * So these will reflect the brscan2, brscan3, etc that Brother use.
 *
 * We will have a encoder/decoder for each family.
 * From what I have seen, pretty much all the Brother families implement
 * the same basic protocol sequence, just with variations in technology.
 * So we don't need to abstract out the entire sequence, just the encoding
 * and decoding of each packet. The biggest aspect by far is the difference
 * in scan data formats.
 *
 */
typedef enum
{
  BROTHER_FAMILY_NONE,
  BROTHER_FAMILY_2,
  BROTHER_FAMILY_3,
  BROTHER_FAMILY_4,
  BROTHER_FAMILY_5
} BrotherFamilyType;


typedef SANE_Int BrotherSensor;

#define BROTHER_SENSOR_NONE     0
#define BROTHER_SENSOR_EMAIL    (1 << 0)
#define BROTHER_SENSOR_FILE     (1 << 1)
#define BROTHER_SENSOR_OCR      (1 << 2)
#define BROTHER_SENSOR_IMAGE    (1 << 3)

#define BROTHER_DATA_BLOCK_NONE                 0x00

struct BrotherButtonQueryResponse
{
  SANE_Bool has_button_press;
};

struct BrotherButtonStateResponse
{
  BrotherSensor button_value;   // raw value we get from the packet
};


struct BrotherBasicParamResponse
{
  int dummy;
  // fill me
};

struct BrotherQueryResponse
{
  int dummy;
  // fill me
};

struct BrotherSourceStatusResponse
{
  SANE_Bool source_ready;
};

struct ScanDataHeader
{
  ScanDataHeader();

  /*
   * A code defined by Brother, identifying the type of packet.
   * We set it to zero when we have processed all data in this packet.
   */
  SANE_Byte block_type;
  size_t block_len;
  SANE_Int image_number;
};


class BrotherImageCache
{
public:
  BrotherImageCache():
    cache_file(nullptr),
    has_read(false)
  {
  }

  ~BrotherImageCache()
  {
    fclose(cache_file);
    cache_file = nullptr;
  }

  bool WriteData(const SANE_Byte *buffer, size_t buffer_len);
  bool ReadData(SANE_Byte *buffer, size_t buffer_len, size_t &data_written);

private:
  FILE *cache_file;
  bool has_read;
};


class BrotherFamily
{
public:
  BrotherFamily (BrotherTransportType transport_type, SANE_String_Const device_name,
		 SANE_Word capabilities)
  {
    // TODO: Do we need this?
    (void)capabilities;

    switch (transport_type)
    {
      case BROTHERTRANSPORT_USB:
	transport = new BrotherTransportUSB(device_name);
	break;

      case BROTHERTRANSPORT_ETHERNET:
      default:
	;
	// TODO: failure
    }
  }

  virtual ~BrotherFamily ()
  {
    delete transport;
  }

  /*
   * Settings.
   *
   */
  SANE_Status SetSource(BrotherSource source);
  SANE_Status SetScanMode(BrotherScanMode scan_mode);
  SANE_Status SetRes(SANE_Int x, SANE_Int y);
  SANE_Status SetContrast(SANE_Int contrast);
  SANE_Status SetBrightness(SANE_Int brightness);
  SANE_Status SetCompression(SANE_Bool compression);
  SANE_Status SetScanDimensions(SANE_Int pixel_x_offset, SANE_Int pixel_x_width,
				SANE_Int pixel_y_offset, SANE_Int pixel_y_height);

  virtual SANE_Status Connect() = 0;

  virtual SANE_Status Disconnect() = 0;

  virtual SANE_Status StartScan() = 0;

  virtual SANE_Status ReadData(SANE_Byte *data, size_t max_length, size_t *length) = 0;

  virtual SANE_Status SetAsyncIOMode(bool async_io) = 0;

  virtual void Cancel() = 0;

protected:
  BrotherTransport *transport;
  BrotherParameters scan_params;
};


//class BrotherFamily234Base : public BrotherFamily
//{
//public:
//  explicit BrotherFamily234Base(SANE_Word caps):
//    capabilities(caps),
//    last_block_type(0),
//    end_image_flag(false),
//    out_of_docs(false),
//    was_cancelled(false)
//  {
//  }
//
//  ~BrotherFamily234Base ()
//  {
//  }
//
//  SANE_Status Connect() override;
//
//  SANE_Status Disconnect() override;
//
//  SANE_Status Start() override;
//
//  SANE_Status Read() override;
//
//  SANE_Status SetAsyncIOMode(bool async_io) override;
//
//  SANE_Status Cancel() override;
//
//private:
//  void NewPage() override;
//
//  SANE_Status EncodeQueryBlock (SANE_Byte *data, size_t data_len, size_t *length);
//
//  SANE_Status DecodeSessionResp (const SANE_Byte *data, size_t data_len,
//                                  BrotherSessionResponse &response);
//
//  SANE_Status EncodeBasicParameterBlock (SANE_Byte *data, size_t data_len, size_t *length);
//
//  SANE_Status DecodeBasicParameterBlockResp (const SANE_Byte *data, size_t data_len,
//                                              BrotherBasicParamResponse &response);
//
//  SANE_Status EncodeSourceStatusBlock (SANE_Byte *data, size_t data_len, size_t *length) ;
//
//  SANE_Status DecodeSourceStatusBlockResp (const SANE_Byte *data, size_t data_len,
//                                            BrotherSourceStatusResponse &response);
//
//  SANE_Status EncodeSourceSelectBlock (SANE_Byte *data, size_t data_len, size_t *length);
//
//  SANE_Status DecodeSourceSelectBlockResp (const SANE_Byte *data, size_t data_len,
//                                            BrotherSourceStatusResponse &response);
//
//  SANE_Status EncodeParameterBlock (SANE_Byte *data, size_t data_len, size_t *length);
//
//  SANE_Status EncodeParameterBlockBlank (SANE_Byte *data, size_t data_len, size_t *length);
//
//  SANE_Status DecodeScanData (const SANE_Byte *src_data, size_t src_data_len,
//                               size_t *src_data_consumed, SANE_Byte *dst_data, size_t dest_data_len,
//                               size_t *dest_data_written)
//
//  SANE_Status DecodeButtonQueryResp (const SANE_Byte *data, size_t data_len,
//                                      BrotherButtonQueryResponse &response)
//
//  SANE_Status DecodeButtonStateResp (const SANE_Byte *data, size_t data_len,
//                                      BrotherButtonStateResponse &response);
//
//  SANE_Status EncodeCancelBlock (SANE_Byte *data, size_t data_len, size_t *length);
//
//  SANE_Status HasDocs();
//
//  virtual SANE_Status DecodeScanDataHeader (const SANE_Byte *src_data, size_t src_data_len,
//                                             size_t *src_data_consumed, ScanDataHeader &header) = 0;
//
//  virtual char const* GetFamilySpecificParameterBlock ()
//  {
//    char const * res = "";
//    return res;
//  }
//
//  SANE_Word capabilities;
//  ScanDataHeader current_header;
//
//  std::list<std::unique_ptr<BrotherScanDataDecoder> > decoders;
//  SANE_Byte last_block_type;
//
//  bool end_image_flag;
//  bool out_of_docs;
//  bool was_cancelled;
//};
//
//
//class BrotherFamily2 : public BrotherFamily234Base
//{
//  using BrotherFamily234Base::BrotherFamily234Base;
//protected:
//  SANE_Status DecodeQueryBlockResp (const SANE_Byte *data, size_t data_len,
//                                             BrotherQueryResponse &response) override;
//  SANE_Status DecodeScanDataHeader (const SANE_Byte *src_data, size_t src_data_len,
//                                     size_t *src_data_consumed, ScanDataHeader &header) override;
//};
//
//class BrotherFamily3 : public BrotherFamily234Base
//{
//  using BrotherFamily234Base::BrotherFamily234Base;
//protected:
//  SANE_Status DecodeQueryBlockResp (const SANE_Byte *data, size_t data_len,
//                                             BrotherQueryResponse &response) override;
//
//  SANE_Status DecodeScanDataHeader (const SANE_Byte *src_data, size_t src_data_len,
//                                     size_t *src_data_consumed, ScanDataHeader &header) override;
//
//};
//
//class BrotherFamily4 : public BrotherFamily234Base
//{
//  using BrotherFamily234Base::BrotherFamily234Base;
//
//  SANE_Status EncodeParameterBlockBlank (SANE_Byte *data, size_t data_len,
//                                          size_t *length) override;
//protected:
//  SANE_Status DecodeScanDataHeader (const SANE_Byte *src_data, size_t src_data_len,
//                                     size_t *src_data_consumed, ScanDataHeader &header) override;
//
//  SANE_Status DecodeQueryBlockResp (const SANE_Byte *data, size_t data_len,
//                                             BrotherQueryResponse &response) override;
//
//  char const* GetFamilySpecificParameterBlock () override;
//};

class BrotherFamily5 : public BrotherFamily
{
public:
  explicit BrotherFamily5(BrotherTransportType transport_type, SANE_String_Const device_name,
			  SANE_Word capabilities) :
      BrotherFamily(transport_type, device_name, capabilities),
      current_decoder(nullptr),
      end_image_flag(false),
      out_of_docs(false),
      was_cancelled(false),
      scan_state(SCANSTATE_IDLE),
      data_buffer_bytes(0)
  {
    (void) capabilities;
  }

  ~BrotherFamily5 ()
  {
  }

  SANE_Status Connect() override;

  SANE_Status Disconnect() override;

  SANE_Status StartScan () override;

  SANE_Status ReadData(SANE_Byte *data, size_t max_length, size_t *length) override;

  SANE_Status SetAsyncIOMode(bool async_io) override;

  void Cancel() override;

private:

  enum ScanState
  {
    SCANSTATE_IDLE,
    SCANSTATE_ACQUIRING,
    SCANSTATE_EOF
  };

  /*
   * Transport operations.
   *
   */
  SANE_Status Init();
  SANE_Status StartScan_BasicParameter();
  SANE_Status StartScan_Parameter();
  SANE_Status CancelScan_Internal();

  /*
   * Packet encode/decode.
   *
   */
  const char* ScanModeToText (BrotherScanMode scan_mode);

  SANE_Status EncodeQueryBlock (SANE_Byte *data, size_t data_len, size_t *length);

  SANE_Status DecodeQueryBlockResp (const SANE_Byte *data, size_t data_len,
                                     BrotherQueryResponse &response);

  SANE_Status DecodeSessionResp (const SANE_Byte *data, size_t data_len,
                                  BrotherSessionResponse &response);

  SANE_Status EncodeBasicParameterBlock (SANE_Byte *data, size_t data_len, size_t *length);

  SANE_Status DecodeBasicParameterBlockResp (const SANE_Byte *data, size_t data_len,
                                              BrotherBasicParamResponse &response);

  SANE_Status EncodeSourceStatusBlock (SANE_Byte *data, size_t data_len, size_t *length);

  SANE_Status DecodeSourceStatusBlockResp (const SANE_Byte *data, size_t data_len,
                                            BrotherSourceStatusResponse &response);

  SANE_Status EncodeSourceSelectBlock (SANE_Byte *data, size_t data_len, size_t *length);

  SANE_Status DecodeSourceSelectBlockResp (const SANE_Byte *data, size_t data_len,
                                            BrotherSourceStatusResponse &response);

  SANE_Status EncodeParameterBlock (SANE_Byte *data, size_t data_len, size_t *length);

  SANE_Status EncodeParameterBlockBlank (SANE_Byte *data, size_t data_len, size_t *length);

  SANE_Status EncodeCancelBlock (SANE_Byte *data, size_t data_len, size_t *length);

  SANE_Status DecodeScanData (const SANE_Byte *src_data, size_t src_data_len,
                               size_t *src_data_consumed, SANE_Byte *dst_data, size_t dest_data_len,
                               size_t *dest_data_written);

  SANE_Status DecodeButtonQueryResp (const SANE_Byte *data, size_t data_len,
                                      BrotherButtonQueryResponse &response);

  SANE_Status DecodeButtonStateResp (const SANE_Byte *data, size_t data_len,
                                      BrotherButtonStateResponse &response);

private:
  void ResetState(bool full)
  {
    /*
     * Reset all state. Some of these shouldn't be necessary be if something
     * went wrong, this should get us into a known state.
     *
     */
    delete current_decoder;
    current_decoder = nullptr;

    end_image_flag = false;
    out_of_docs = false;

    if (full)
      {
	data_buffer_bytes = 0;
	current_header.block_type = BROTHER_DATA_BLOCK_NONE;
      }
  }

  SANE_Status DecodeScanDataHeader (const SANE_Byte *src_data, size_t src_data_len,
                                     size_t *src_data_consumed, ScanDataHeader &header);

  SANE_Status DecodeReadBuffer(SANE_Byte *data, size_t max_length, size_t *length);

  ScanDataHeader current_header;
  BrotherScanDataDecoder *current_decoder;
  bool end_image_flag;
  bool out_of_docs;
  bool was_cancelled;
  ScanState scan_state;
  SANE_Byte small_send_buffer[1024];
  SANE_Byte small_rec_buffer[1024];
  SANE_Byte data_buffer[BROTHER_READ_BUFFER_LEN];
  size_t data_buffer_bytes;

  std::mutex mutex;
};

