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
#include "../include/sane/saneopts.h"
#include "../include/sane/sanei_config.h"
#include "../include/sane/sanei_usb.h"
#include "../include/sane/sanei_debug.h"

#include "brother_mfp-common.h"
#include "brother_mfp-data-decoder.h"

void BrotherGrayRLengthDecoder::NewBlock ()
{
}

/*
 * Returns:
 *
 * SANE_STATUS_GOOD - block all decoded.
 * SANE_STATUS_ERROR - error detected in data.
 *
 */
SANE_Status BrotherGrayRLengthDecoder::DecodeScanData (const SANE_Byte *in_buffer,
                                                        size_t in_buffer_len,
                                                        size_t *bytes_consumed,
                                                        SANE_Byte *out_buffer,
                                                        size_t out_buffer_len,
                                                        size_t *bytes_written)
{
  FDBGNP (DBG_EVENT, "");

  *bytes_consumed = 0;
  *bytes_written = 0;

  size_t in_buffer_len_start = in_buffer_len;
  size_t out_buffer_len_start = out_buffer_len;

  /*
   * Notice that we do the check for in and out space at the bottom, not here:
   * We might not have any additional src, but we might still be able to progress the decode,
   * especially in the case of an in-progress decompression.
   *
   */
  do
    {
      /*
       * We need src to be able to decode.
       *
       */
      if (in_buffer_len == 0)
        {
          break;
        }

      /*
       * INIT state: we are ready to start to process another sub-block.
       *
       * If there is data then we should see a block length.
       *
       * If byte & 0x80 then it is a compressed byte block.
       * Else it is a sequence of uncompressed bytes.
       *
       */
      size_t consumed_input = 0;
      size_t written_output = 0;

      if (in_buffer[0] & 0x80)
        {
          /*
           * To process the expansion miniblock, we have to see the next byte.
           * We might not have it yet. We will just have to wait for more bytes to come in.
           *
           */
          if (in_buffer_len < 2)
            {
              FDBGNP (DBG_DETAIL, "in_buffer_len < 2: can't decode yet.");
              break;
            }

          /*
           * Check that we have enough output space for the expanded mini block.
           *
           */
          size_t output_req = 0xff - in_buffer[0] + 2;
          if (output_req > out_buffer_len)
            {
              break;
            }

          SANE_Byte decode_expand_char = in_buffer[1];

          in_buffer += 2;
          in_buffer_len -= 2;

          (void) memset (out_buffer, decode_expand_char, output_req);
          written_output = output_req;

	  FDBG(DBG_DETAIL, "expand block: %zu bytes of 0x%2.2x", output_req,
	       (int ) decode_expand_char);
	}
      else
        {

          /*
           * Check that we have enough space for the expanded mini block
           * and that we have the entire miniblock in the src buffer.
           *
           */
          size_t output_req = in_buffer[0] + 1;
          if ((output_req > out_buffer_len) || (in_buffer_len < output_req + 1))
            {
              break;
            }

          in_buffer += 1;
          in_buffer_len -= 1;

          (void) memcpy (out_buffer, in_buffer, output_req);
          consumed_input = output_req;
          written_output = output_req;

	  FDBG(DBG_DETAIL, "bytes block: %zu bytes", output_req);
	}

      /*
       * Adjust for the consumed input and written output.
       *
       */
      in_buffer += consumed_input;
      in_buffer_len -= consumed_input;
      out_buffer += written_output;
      out_buffer_len -= written_output;

    } while (in_buffer_len && out_buffer_len);

  if (!in_buffer_len || !out_buffer_len)
    {
      FDBG(DBG_DETAIL, "ran out of buffer: in %zu out %zu", in_buffer_len, out_buffer_len);
    }
  *bytes_consumed = in_buffer_len_start - in_buffer_len;
  *bytes_written = out_buffer_len_start - out_buffer_len;

  return SANE_STATUS_GOOD;
}

void BrotherInterleavedRGBColourDecoder::NewBlock ()
{
}

void BrotherInterleavedRGBColourDecoder::ConvertYCbCrToRGB (SANE_Byte y, SANE_Byte cb, SANE_Byte cr,
                                                            SANE_Byte *red, SANE_Byte *green,
                                                            SANE_Byte *blue)
{
  double y_double = (double) y;
  double cb_double = (double) cb;
  double cr_double = (double) cr;

  double red_chan = y_double + 1.40200 * (cr_double - 0x80);
  double green_chan = y_double - 0.34414 * (cb_double - 0x80) - 0.71414 * (cr_double - 0x80);
  double blue_chan = y_double + 1.77200 * (cb_double - 0x80);

  *red = std::max (0.0, std::min (255.0, red_chan));
  *green = std::max (0.0, std::min (255.0, green_chan));
  *blue = std::max (0.0, std::min (255.0, blue_chan));
}

/*
 * Returns:
 *
 * SANE_STATUS_GOOD - block all decoded.
 * SANE_STATUS_ERROR - error detected in data.
 *
 * TODO: This function is assuming 24bit colour. I don't know
 * if that is always valid.
 *
 */
SANE_Status BrotherInterleavedRGBColourDecoder::DecodeScanData (const SANE_Byte *in_buffer,
                                                                 size_t in_buffer_len,
                                                                 size_t *bytes_consumed,
                                                                 SANE_Byte *out_buffer,
                                                                 size_t out_buffer_len,
                                                                 size_t *bytes_written)
{
  *bytes_consumed = 0;
  *bytes_written = 0;

  size_t in_buffer_len_start = in_buffer_len;
  size_t out_buffer_len_start = out_buffer_len;

  if (scanline_length == 0)
    {
      /*
       * Make sure that we have an allocated buffer big enough for
       * a single output scanline that we will assemble the scanline into.
       *
       */
      scanline_length = decode_params.param_pixel_x_width * 3;

      if (scanline_length > scanline_buffer_size)
        {
          SANE_Byte *new_buffer = (SANE_Byte *)realloc (scanline_buffer, scanline_length);
          if (new_buffer == nullptr)
            {
              scanline_length = 0;
              return SANE_STATUS_NO_MEM;
            }
          scanline_buffer = new_buffer;
          scanline_buffer_size = scanline_length;
        }

      /*
       * Reset our decoding.
       *
       */
      scanline_buffer_data = 0;
    }

  /*
   * Assemble src data into the scanline buffer.
   *
   */
  while (in_buffer_len && (out_buffer_len > scanline_length))
    {
      size_t bytes_avail = std::min(in_buffer_len, scanline_length - scanline_buffer_data);
      (void)memcpy(scanline_buffer + scanline_buffer_data, in_buffer, bytes_avail);

      scanline_buffer_data += bytes_avail;
      in_buffer += bytes_avail;
      in_buffer_len -= bytes_avail;

      /*
       * See if we have a full scanline now.
       *
       */
      if (scanline_buffer_data >= scanline_length)
        {
          size_t stride = decode_params.param_pixel_x_width;

          SANE_Byte *red = scanline_buffer;
          SANE_Byte *green = scanline_buffer + stride;
          SANE_Byte *blue = scanline_buffer + (2 * stride);

          for (size_t col_index = 0; col_index < (size_t) decode_params.param_pixel_x_width;
              col_index++)
            {
              /*
               * Sort out the raw encoding format.
               *
               */
              if (capabilities & CAP_ENCODING_RAW_IS_CrYCb)
                {
                  SANE_Byte R;
                  SANE_Byte G;
                  SANE_Byte B;

                  ConvertYCbCrToRGB(*green++, *blue++, *red++, &R, &G, &B);

                  *out_buffer++ = R;
                  *out_buffer++ = G;
                  *out_buffer++ = B;
                }
              else
                {
                  /*
                   * Must be just plain RBG.
                   *
                   */
                  *out_buffer++ = *red++;
                  *out_buffer++ = *green++;
                  *out_buffer++ = *blue++;
                }
            }
          scanline_buffer_data = 0;
          out_buffer_len -= scanline_length;
        }
    }

  *bytes_consumed = in_buffer_len_start - in_buffer_len;
  *bytes_written = out_buffer_len_start - out_buffer_len;

  return SANE_STATUS_GOOD;
}

#include <jpeglib.h>
#include <jerror.h>

void BrotherJFIFDecoder::NewBlock()
{
  // Nothing to do.
}

void BrotherJFIFDecoder::InitSource (j_decompress_ptr cinfo)
{
  FDBGNP (DBG_EVENT, "");

  (void)cinfo;

  /*
   * We will have already setup all the available data.
   *
   */
  return;
}

boolean BrotherJFIFDecoder::FillInputBuffer(j_decompress_ptr cinfo)
{
  (void)cinfo;

  /*
   * We can not supply more data.
   * We setup the data buffer already with everything we have.
   *
   */
  FDBGNP (DBG_EVENT, "");
  return FALSE;
}

void BrotherJFIFDecoder::SkipInputData(j_decompress_ptr cinfo, long num_bytes)
{
  (void)cinfo;
  (void)num_bytes;

  /*
   * TODO: Note the special attention required here.
   * num_bytes might exceed the number of bytes in the src_data so we must remember
   * the excess so that we can discard it when we add more data later!!!!!!!
   *
   */
  FDBGNP (DBG_EVENT, "");
}

void BrotherJFIFDecoder::TermSource(j_decompress_ptr cinfo)
{
  (void)cinfo;
  FDBGNP (DBG_EVENT, "");
}

void BrotherJFIFDecoder::OutputMessageHandler(j_common_ptr cinfo)
{
  /* based on libjpeg-turbo/jerror.c */
  char jpegLastErrorMsg[JMSG_LENGTH_MAX];
  (*cinfo->err->format_message) (cinfo, jpegLastErrorMsg);
  FDBG (DBG_WARN, "libjpeg warning=[%s]", jpegLastErrorMsg);
}

void BrotherJFIFDecoder::ErrorExitManager(j_common_ptr cinfo)
{
  char jpegLastErrorMsg[JMSG_LENGTH_MAX];
  ( *(cinfo->err->format_message) ) (cinfo, jpegLastErrorMsg);
  FDBG (DBG_SERIOUS, "libjpeg error=[%s]", jpegLastErrorMsg);

  CompressionState *state = reinterpret_cast<CompressionState *>(cinfo);
  longjmp(state->my_env, 1);
}


SANE_Status BrotherJFIFDecoder::DecodeScanData (const SANE_Byte *src_data, size_t src_data_len,
                                                 size_t *src_data_consumed, SANE_Byte *dest_data,
                                                 size_t dest_data_len, size_t *dest_data_written)
{
  FDBG (DBG_EVENT,
       "src_data_len=%zu dest_data_len=%zu",
       src_data_len,
       dest_data_len);

  *src_data_consumed = 0;
  *dest_data_written = 0;

  /*
   * Loop around, appending data to our decompress buffer and trying to
   * decompress as we go until we exhaust the output buffer or the input.
   *
   */
  for (;;)
    {
      FDBGNP (DBG_DETAIL, "In Loop...");

      /*
       * Append what we can to the decompress buffer.
       *
       */
      if (src_data_len > 0)
	{
	  size_t bytes_to_copy = std::min(sizeof(decompress_buffer) - decompress_bytes, src_data_len);

	  (void) memcpy (decompress_buffer + decompress_bytes, src_data, bytes_to_copy);
	  decompress_bytes += bytes_to_copy;

	  src_data += bytes_to_copy;
	  src_data_len -= bytes_to_copy;
	  *src_data_consumed += bytes_to_copy;
	}

      /*
       * Now to try to decompress.
       *
       */
      if (!decompress_bytes && !HasUnprocessedDecodeData())
	{
	  break;
	}

      size_t decompress_consumed = 0;
      size_t bytes_written = 0;
      SANE_Status res = DecodeScanData_CompressBuffer (decompress_buffer,
						       decompress_bytes,
						       &decompress_consumed,
						       dest_data,
						       dest_data_len,
						       &bytes_written);
      if (res != SANE_STATUS_GOOD)
	{
	  *src_data_consumed = 0;
	  *dest_data_written = 0;
	  return res;
	}

      /*
       * Shift out any data that we consumed.
       *
       */
      (void) memmove (decompress_buffer,
		      decompress_buffer + decompress_consumed,
		      decompress_bytes - decompress_consumed);
      decompress_bytes -= decompress_consumed;

      if (bytes_written == 0)
	{
	  break;
	}

      dest_data += bytes_written;
      dest_data_len -= bytes_written;
      *dest_data_written += bytes_written;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status BrotherJFIFDecoder::DecodeScanData_CompressBuffer (const SANE_Byte *src_data,
                                                                size_t src_data_len,
                                                                size_t *src_data_consumed,
                                                                SANE_Byte *dest_data,
                                                                size_t dest_data_len,
                                                                size_t *dest_data_written)
{
  FDBGNP (DBG_EVENT, "");

  *src_data_consumed = 0;
  *dest_data_written = 0;

  /*
   * We may get here if all of the requested output lines have been produced, but the scanner is still sending
   * data in the current frame. This can happen when the scanner returns inconsistent JFIF data.
   * See the end of this function for more explanations and handling of this edge case.
   * 
   */
  if (state.cinfo.output_height > 0 && state.cinfo.output_scanline >= state.cinfo.output_height)
  {
    FDBG(DBG_DETAIL, "Back in compress_buffer even though all output lines of the current frame have been produced. Ignoring the rest of the frame data (%d bytes left).", src_data_len);
    *src_data_consumed = src_data_len;
    return SANE_STATUS_GOOD;
  }

  FDBG (DBG_DETAIL,
       "src_data_len=%zu dest_data_len=%zu",
       src_data_len,
       dest_data_len);

  /*
   * Initialise read buffer.
   *
   */
  state.src_mgr.bytes_in_buffer = src_data_len;
  state.src_mgr.next_input_byte = src_data;

  if (setjmp(state.my_env) != 0)
    {
      FDBGNP (DBG_SERIOUS, "setjmp error return");
      return SANE_STATUS_IO_ERROR;
    }

  /*
   * Read header if we haven't yet.
   *
   */
  if (!state.have_read_header)
    {
      FDBGNP (DBG_DETAIL, "Trying to read header.");

      int read_status = jpeg_read_header(&state.cinfo, TRUE);
      if (read_status == JPEG_SUSPENDED)
	{
	  return SANE_STATUS_GOOD;
	}
      else if (read_status != JPEG_HEADER_OK)
	{
	  return SANE_STATUS_IO_ERROR;
	}
      state.have_read_header = true;

      /*
       * Set the required colour space for the image we expect.
       * For gray it is JCS_GRAYSCALE. For colour it is JCS_RGB.
       *
       * Note: it does not seem like Brother machines use this mode.
       * Grey images are sent as 3 channel colour with all channels containing
       * the grey data.
       */
//      if (decode_params.param_scan_mode == BROTHER_SCAN_MODE_GRAY)
//        {
//          state.cinfo.out_color_space = JCS_GRAYSCALE;
//        }

      FDBGNP (DBG_DETAIL, "Starting decompress.");

      if (!jpeg_start_decompress(&state.cinfo))
	{
	  /*
	   * Need more data.
	   *
	   */
	  return SANE_STATUS_GOOD;
	}

      /*
       * Make sure that we will be getting the data that we expect.
       * This is for colour only, so we expect 3 components.
       *
       */
      if (state.cinfo.num_components != 3)
	{
          FDBG (DBG_SERIOUS,
               "image components invalid, expecting 3: %d",
               state.cinfo.num_components);
	  return SANE_STATUS_IO_ERROR;
	}

      FDBG (DBG_DETAIL,
           "image parameters: width=%zu height=%zu",
           (size_t)state.cinfo.image_width, (size_t)state.cinfo.image_height);

      /*
       * Allocate a buffer to receive the decompressed data.
       * We *could* decompress straight into the frontend's data buffer
       * but for the fact that GREY data is actually returned as 3-channel
       * colour data :( So we would overrun the frontend's buffer under some
       * circumstances.
       *
       */
      decomp_line_size = sizeof(JSAMPLE)
    			      * state.cinfo.output_width
    			      * state.cinfo.out_color_components;

      raw_data_buffer = new SANE_Byte[decomp_line_size];
      if (!raw_data_buffer)
	{
	  return SANE_STATUS_NO_MEM;
	}

      dest_line_size = state.cinfo.image_width
	  * (decode_params.param_scan_mode == BROTHER_SCAN_MODE_GRAY ? 1 : 3);

      /*
       * TODO: Perhaps also check the dimensions are what we are expecting.
       * Depending on the issues related to width alignment, we might
       * have to do some trimming on the scan lines if we had to round up the
       * the width and/or round down the left margin.
       *
       */
    }

  /*
   * Let's try to decompress some JPEG!
   *
   */
  if (!raw_data_buffer)
    {
      FDBGNP (DBG_SERIOUS, "we don't have a raw buffer to decode into!");
      return SANE_STATUS_NO_MEM;
    }

  FDBG (DBG_DETAIL,
       "Converting data: dest_line_size=%zu decomp_line_size=%zu state->output_width=%zu components=%d",
       dest_line_size,
       decomp_line_size,
       (size_t)state.cinfo.output_width,
       state.cinfo.out_color_components);

  SANE_Byte *output_ptr = dest_data;
  size_t output_size = dest_data_len;

  while (output_size >= dest_line_size)
    {
      /*
       * We have to ask for single lines because I think that the jpeg_read_scanlines()
       * function is expecting an array of line buffer pointers. We don't have that
       * but we have a single contiguous buffer, so asking for a number of lines at
       * once is *not* going to work, which is a shame because this is less efficient
       * potentially. :(
       *
       */

      /*
       * Check if we have read all scanlines before attempting to read.
       * This suppresses a spurious error message.
       *
       */
      if (state.cinfo.output_scanline >= state.cinfo.output_height)
        {
          FDBGNP (DBG_DETAIL, "converted all scanlines, exiting JFIF decode loop");
          break;
        }

      JDIMENSION converted = jpeg_read_scanlines (&state.cinfo, &raw_data_buffer, 1);

      if (converted == 0)
        {
          FDBGNP (DBG_DETAIL, "converted none");
          break;
        }

      /*
       * Downscale the line if we are expecting grey data.
       * We choose the first channel and discard the rest: they
       * are all the same.
       *
       */
      if (decode_params.param_scan_mode == BROTHER_SCAN_MODE_GRAY)
	{
	  for (size_t idx = 0; idx < dest_line_size; idx++)
	    {
	      *output_ptr++ = raw_data_buffer[idx * 3];
	    }
	}
      else	// colour: just copy the whole line.
	{
	  (void)memcpy(output_ptr, raw_data_buffer, dest_line_size);
	  output_ptr += dest_line_size;
	}

      output_size -= dest_line_size;

      FDBG (DBG_DETAIL,
           "Converted %u scanlines. "
           "Remaining scanlines: %u output_remaining=%zu decompress_remaining=%zu",
           (unsigned int) converted,
           (unsigned int) (state.cinfo.output_height - state.cinfo.output_scanline),
           output_size,
	   state.src_mgr.bytes_in_buffer);

    }

  /*
   * Account for what we have processed.
   *
   * Note that libjpeg will have altered state.src_mgr.bytes_in_buffer
   * to a backtracked position for it to restart again.
   * This is why it is CRUCIAL that we do not try to guess.
   *
   */
  *dest_data_written = output_ptr - dest_data;
  *src_data_consumed = src_data_len - state.src_mgr.bytes_in_buffer;

  FDBG (DBG_DETAIL,
       "Finished: consumed %zu bytes, written %zu bytes",
       *src_data_consumed,
       *dest_data_written);

  /*
   * If we have output all the scanlines, then we need to finish up.
   *
   */
  if (state.cinfo.output_scanline >= state.cinfo.output_height)
    {
      /*
       * As explained above, *src_data_consumed will not account for all the bytes that have been read
       * so libjpeg can restart an unfinished decode. In case we are finished, however, we do want
       * to absorb all the remaining bytes so the caller knows that all the data has been processed.
       * 
       * However, this alone is not sufficient, since there may be more invalid JFIF data in this frame
       * coming from the scanner that has not been read into the source buffer (see the edge case handling below).
       * Therefore, we also need to make sure that the remainder of the current frame's source data is
       * absorbed, which is being done at the start of the function.
       */
      *src_data_consumed = src_data_len;
      FDBGNP (DBG_EVENT, "Converted all scanlines. Finishing.");

      if (!jpeg_finish_decompress(&state.cinfo))
        {
          FDBGNP (DBG_WARN, "Failed to finish JPEG decompress.");
          /*
           * Some scanners return invalid JFIF in the sense that the `output_height` in the header does not match
           * the actual number of lines in the image. This has been observed for the `DCP-L2500D` model.
           * In this case, `jpeg_finish_decompress` can fail, and we need to abort the JPEG decompression instead.
           * The correct behaviour here is to finish returning the current page and to proceeding to the next page
           * (if there is one).
           * 
           * `jpeg_abort_decompress` has been observed to preserve `cinfo.output_scanline` and `cinfo.output_height`.
           * Therefore, the next call to this function can check for `output_scanline >= output_height` and react accordingly.
           * See the beginning of this function.
           *
           */
          jpeg_abort_decompress(&state.cinfo);
          FDBG (DBG_DETAIL, "Scanline after abort: %d; Height after abort: %d", state.cinfo.output_scanline, state.cinfo.output_height);
        }
    }

  return SANE_STATUS_GOOD;
}


void BrotherGrayRawDecoder::NewBlock()
{
  // Nothing to do.
}

SANE_Status BrotherGrayRawDecoder::DecodeScanData (const SANE_Byte *src_data, size_t src_data_len,
                                                    size_t *src_data_consumed, SANE_Byte *dest_data,
                                                    size_t dest_data_len, size_t *dest_data_written)
{
  FDBGNP (DBG_EVENT, "");

  /*
   * Quick check. This isn't strictly necessary because the following code copes
   * properly with src_data_len == 0, but we might change this code in the future
   * and we might not realise that it must.
   *
   * At least we make it clear here that we need src_data to do something useful.
   *
   */
  if (src_data_len == 0)
    {
      return SANE_STATUS_GOOD;
    }

  /*
   * Just copy to the output what we can from the input.
   * It's just raw data.
   *
   */
  size_t copy_len = std::min(src_data_len, dest_data_len);

  (void)memcpy(dest_data, src_data, copy_len);
  *dest_data_written = copy_len;
  *src_data_consumed = copy_len;

  FDBG(DBG_DETAIL, "Transferred %zu GRAY data.", copy_len);

  return SANE_STATUS_GOOD;
}
