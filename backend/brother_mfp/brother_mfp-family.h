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
 * drivers follow the broad protocol versions and I will assume this
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

struct BrotherQueryScanInfoResponse
{
  SANE_Int actual_x_res;
  SANE_Int actual_y_res;
  SANE_Int min_x;	// 0.1mm unit
  SANE_Int max_x;	// 0.1mm unit
  SANE_Int min_y;	// 0.1mm unit
  SANE_Int max_y;	// 0.1mm unit
};

struct BrotherQueryResponse
{
  SANE_Int info_size;
  SANE_Int protocol_type;
  SANE_Int colour_type;
  SANE_Int hardware_version;
  SANE_Int dpi_set;
  SANE_Int paper_size_max;
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
  BrotherFamily(BrotherFamilyType family_type, BrotherTransportType transport_type,
		SANE_String_Const device_name, SANE_Word capabilities) :
      family_type(family_type),
      transport(nullptr),
      capabilities(capabilities)
  {
    switch (transport_type)
      {
      case BROTHERTRANSPORT_USB:
	transport = new BrotherTransportUSB(device_name);
	break;

      case BROTHERTRANSPORT_ETHERNET:
      default:
	FDBG(DBG_SERIOUS, "unsupported transport [%d]", transport_type);
	break;
      }
  }

  virtual ~BrotherFamily()
  {
    delete transport;
  }

  /*
   * Settings.
   *
   * Can call these any time when not scanning.
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

  /*
   * Operations.
   *
   */
  virtual SANE_Status Connect() = 0;

  virtual SANE_Status Disconnect() = 0;

  virtual SANE_Status StartScan() = 0;

  virtual SANE_Status ReadData(SANE_Byte *data, size_t max_length, size_t *length) = 0;

  virtual SANE_Status ReadButtons(SANE_Int &button_mask) = 0;

  virtual SANE_Status SetAsyncIOMode(bool async_io) = 0;

  virtual void Cancel() = 0;

protected:
  BrotherFamilyType family_type;
  BrotherTransport *transport;
  BrotherParameters scan_params;
  SANE_Word capabilities;
};

class BrotherFamily234Base : public BrotherFamily
{
public:
  explicit BrotherFamily234Base(BrotherFamilyType family_type, BrotherTransportType transport_type,
				SANE_String_Const device_name, SANE_Word capabilities) :
      BrotherFamily(family_type, transport_type, device_name, capabilities),
      scan_state(SCANSTATE_IDLE),
      data_buffer_bytes(0),
      last_block_category(BLOCKCATEGORY_NONE),
      end_image_flag(false),
      out_of_docs(false),
      was_cancelled(false)
  {
  }

  virtual ~BrotherFamily234Base()
  {
  }

  SANE_Status Connect() override;

  SANE_Status Disconnect() override;

  SANE_Status StartScan() override;

  SANE_Status ReadData(SANE_Byte *data, size_t max_length, size_t *length) override;

  SANE_Status ReadButtons(SANE_Int &button_mask) override;

  SANE_Status SetAsyncIOMode(bool async_io) override;

  void Cancel() override;

protected:
  const char* ScanModeToText(BrotherScanMode scan_mode);

  enum BlockCategory
  {
    BLOCKCATEGORY_NONE,
    BLOCKCATEGORY_JPEG,
    BLOCKCATEGORY_GRAY_RLENGTH,
    BLOCKCATEGORY_GRAY_RAW,
    BLOCKCATEGORY_COLOUR_RAW
  };

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
  SANE_Status StartScan_SelectSource();
  SANE_Status StartScan_QueryScanInfo();
  SANE_Status StartScan_ParameterBlank();
  SANE_Status StartScan_Parameter();
  SANE_Status CancelScan_Internal();

  /*
   * Packet encode/decode.
   *
   */
  SANE_Status EncodeQueryBlock(SANE_Byte *data, size_t data_len, size_t *length);

  SANE_Status DecodeQueryBlockResp(const SANE_Byte *data, size_t data_len,
				   BrotherQueryResponse &response);

  SANE_Status DecodeSessionResp(const SANE_Byte *data, size_t data_len,
				BrotherSessionResponse &response);

  SANE_Status EncodeQueryScanInfoBlock(SANE_Byte *data, size_t data_len, size_t *length);

  SANE_Status DecodeQueryScanInfoBlockResp(const SANE_Byte *data, size_t data_len,
					    BrotherQueryScanInfoResponse &response);

  SANE_Status EncodeSourceStatusBlock(SANE_Byte *data, size_t data_len, size_t *length);

  SANE_Status DecodeSourceStatusBlockResp(const SANE_Byte *data, size_t data_len,
					  BrotherSourceStatusResponse &response);

  SANE_Status EncodeSourceSelectBlock(SANE_Byte *data, size_t data_len, size_t *length);

  SANE_Status DecodeSourceSelectBlockResp(const SANE_Byte *data, size_t data_len,
					  BrotherSourceStatusResponse &response);

  SANE_Status EncodeParameterBlockBlank(SANE_Byte *data, size_t data_len, size_t *length);

  SANE_Status EncodeCancelBlock(SANE_Byte *data, size_t data_len, size_t *length);

  SANE_Status DecodeScanData(const SANE_Byte *src_data, size_t src_data_len,
			     size_t *src_data_consumed, SANE_Byte *dst_data, size_t dest_data_len,
			     size_t *dest_data_written);

  SANE_Status DecodeButtonQueryResp(const SANE_Byte *data, size_t data_len,
				    BrotherButtonQueryResponse &response);

  SANE_Status DecodeButtonStateResp(const SANE_Byte *data, size_t data_len,
				    BrotherButtonStateResponse &response);

protected:
  void ResetState(bool full)
  {
    /*
     * Reset all state to known situation.
     *
     * If full is TRUE then reset for first Start().
     * If full is FALSE then prepare for next image.
     *
     */
    decoders.clear();
    end_image_flag = false;

    if (full)
      {
	data_buffer_bytes = 0;
	current_header.block_type = BROTHER_DATA_BLOCK_NONE;
	out_of_docs = false;
      }
  }

  virtual SANE_Status DecodeScanDataHeader(const SANE_Byte *src_data, size_t src_data_len,
				   size_t *src_data_consumed, ScanDataHeader &header) = 0;

  virtual SANE_Status EncodeParameterBlock(SANE_Byte *data, size_t data_len, size_t *length) = 0;

  SANE_Status DecodeReadBuffer(SANE_Byte *data, size_t max_length, size_t *length);

  ScanDataHeader current_header;
  ScanState scan_state;
  SANE_Byte small_send_buffer[1024];
  SANE_Byte small_rec_buffer[1024];
  SANE_Byte data_buffer[BROTHER_READ_BUFFER_LEN];
  size_t data_buffer_bytes;

  std::list<std::unique_ptr<BrotherScanDataDecoder> > decoders;
  BlockCategory last_block_category;

  bool end_image_flag;
  bool out_of_docs;
  bool was_cancelled;	// allows next (Read()) API call to return cancelled status.
  std::mutex mutex;
};

class BrotherFamily2 : public BrotherFamily234Base
{
public:
  explicit BrotherFamily2(BrotherTransportType transport_type, SANE_String_Const device_name,
			  SANE_Word capabilities) :
      BrotherFamily234Base(BROTHER_FAMILY_2, transport_type, device_name, capabilities)
  {
  }

  virtual ~BrotherFamily2()
  {
  }

protected:
  SANE_Status DecodeScanDataHeader(const SANE_Byte *src_data, size_t src_data_len,
					   size_t *src_data_consumed, ScanDataHeader &header) override;

  SANE_Status EncodeParameterBlock(SANE_Byte *data, size_t data_len, size_t *length) override;
};

class BrotherFamily3 : public BrotherFamily234Base
{
public:
  explicit BrotherFamily3(BrotherTransportType transport_type, SANE_String_Const device_name,
			  SANE_Word capabilities) :
      BrotherFamily234Base(BROTHER_FAMILY_3, transport_type, device_name, capabilities)
  {
  }

  virtual ~BrotherFamily3()
  {
  }

protected:
  SANE_Status DecodeScanDataHeader(const SANE_Byte *src_data, size_t src_data_len,
					   size_t *src_data_consumed, ScanDataHeader &header) override;

  SANE_Status EncodeParameterBlock(SANE_Byte *data, size_t data_len, size_t *length) override;
};

class BrotherFamily4 : public BrotherFamily234Base
{
public:
  explicit BrotherFamily4(BrotherTransportType transport_type, SANE_String_Const device_name,
			  SANE_Word capabilities) :
      BrotherFamily234Base(BROTHER_FAMILY_4, transport_type, device_name, capabilities)
  {
  }

  virtual ~BrotherFamily4()
  {
  }

protected:
  SANE_Status DecodeScanDataHeader(const SANE_Byte *src_data, size_t src_data_len,
					   size_t *src_data_consumed, ScanDataHeader &header) override;

  SANE_Status EncodeParameterBlock(SANE_Byte *data, size_t data_len, size_t *length) override;
};


class BrotherFamily5 : public BrotherFamily
{
public:
  explicit BrotherFamily5(BrotherTransportType transport_type, SANE_String_Const device_name,
			  SANE_Word capabilities) :
      BrotherFamily(BROTHER_FAMILY_5, transport_type, device_name, capabilities),
      scan_state(SCANSTATE_IDLE),
      data_buffer_bytes(0),
      last_block_type(BROTHER_DATA_BLOCK_NONE),
      end_image_flag(false),
      out_of_docs(false),
      was_cancelled(false)
  {
  }

  ~BrotherFamily5()
  {
  }

  SANE_Status Connect() override;

  SANE_Status Disconnect() override;

  SANE_Status StartScan() override;

  SANE_Status ReadData(SANE_Byte *data, size_t max_length, size_t *length) override;

  SANE_Status ReadButtons(SANE_Int &button_mask) override;

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
  SANE_Status StartScan_QueryScanInfo();
  SANE_Status StartScan_Parameter();
  SANE_Status CancelScan_Internal();

  /*
   * Packet encode/decode.
   *
   */
  const char* ScanModeToText(BrotherScanMode scan_mode);

  SANE_Status EncodeQueryBlock(SANE_Byte *data, size_t data_len, size_t *length);

  SANE_Status DecodeQueryBlockResp(const SANE_Byte *data, size_t data_len,
				   BrotherQueryResponse &response);

  SANE_Status DecodeSessionResp(const SANE_Byte *data, size_t data_len,
				BrotherSessionResponse &response);

  SANE_Status EncodeQueryScanInfoBlock(SANE_Byte *data, size_t data_len, size_t *length);

  SANE_Status DecodeQueryScanInfoBlockResp(const SANE_Byte *data, size_t data_len,
					    BrotherQueryScanInfoResponse &response);

  SANE_Status EncodeSourceStatusBlock(SANE_Byte *data, size_t data_len, size_t *length);

  SANE_Status DecodeSourceStatusBlockResp(const SANE_Byte *data, size_t data_len,
					  BrotherSourceStatusResponse &response);

  SANE_Status EncodeSourceSelectBlock(SANE_Byte *data, size_t data_len, size_t *length);

  SANE_Status DecodeSourceSelectBlockResp(const SANE_Byte *data, size_t data_len,
					  BrotherSourceStatusResponse &response);

  SANE_Status EncodeParameterBlock(SANE_Byte *data, size_t data_len, size_t *length);

  SANE_Status EncodeParameterBlockBlank(SANE_Byte *data, size_t data_len, size_t *length);

  SANE_Status EncodeCancelBlock(SANE_Byte *data, size_t data_len, size_t *length);

  SANE_Status DecodeScanData(const SANE_Byte *src_data, size_t src_data_len,
			     size_t *src_data_consumed, SANE_Byte *dst_data, size_t dest_data_len,
			     size_t *dest_data_written);

  SANE_Status DecodeButtonQueryResp(const SANE_Byte *data, size_t data_len,
				    BrotherButtonQueryResponse &response);

  SANE_Status DecodeButtonStateResp(const SANE_Byte *data, size_t data_len,
				    BrotherButtonStateResponse &response);

private:
  void ResetState(bool full)
  {
    /*
     * Reset all state to known situation.
     *
     * If full is TRUE then reset for first Start().
     * If full is FALSE then prepare for next image.
     *
     */
    decoders.clear();
    end_image_flag = false;

    if (full)
      {
	data_buffer_bytes = 0;
	current_header.block_type = BROTHER_DATA_BLOCK_NONE;
	out_of_docs = false;
      }
  }

  SANE_Status DecodeScanDataHeader(const SANE_Byte *src_data, size_t src_data_len,
				   size_t *src_data_consumed, ScanDataHeader &header);

  SANE_Status DecodeReadBuffer(SANE_Byte *data, size_t max_length, size_t *length);

  ScanDataHeader current_header;
  ScanState scan_state;
  SANE_Byte small_send_buffer[1024];
  SANE_Byte small_rec_buffer[1024];
  SANE_Byte data_buffer[BROTHER_READ_BUFFER_LEN];
  size_t data_buffer_bytes;

  std::list<std::unique_ptr<BrotherScanDataDecoder> > decoders;
  SANE_Int last_block_type;

  bool end_image_flag;
  bool out_of_docs;
  bool was_cancelled;	// allows next (Read()) API call to return cancelled status.
  std::mutex mutex;
};
