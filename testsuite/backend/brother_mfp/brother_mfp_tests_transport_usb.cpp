/* sane - Scanner Access Now Easy.

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
*/

#define DEBUG_DECLARE_ONLY

#include "brother_mfp_tests.h"
#include "../../backend/brother_mfp/brother_mfp-family.h"
#include "../genesys/minigtest.h"

class BrotherTransportUSBTests
{
public:
  void test_decode_session_resp()
  {
    SANE_Status decode_resp;
    BrotherSessionResponse sess_resp;

    BrotherTransportUSB transport("");

    // SUCCESS status
    const SANE_Byte *data = (const SANE_Byte *)"\x05" "\x10" "\x01" "\x02" "\x00";

    decode_resp = transport.DecodeSessionResp (data, 5, sess_resp);

    ASSERT_EQ(decode_resp, SANE_STATUS_GOOD);
    ASSERT_TRUE(sess_resp.ready);

    // BUSY status
    data = (const SANE_Byte *)"\x05" "\x10" "\x01" "\x02" "\x80";

    decode_resp = transport.DecodeSessionResp (data, 5, sess_resp);

    ASSERT_EQ(decode_resp, SANE_STATUS_GOOD);
    ASSERT_FALSE(sess_resp.ready);

    // Length issues
    data = (const SANE_Byte *)"\x05" "\x10" "\x01" "\x02" "\x20" "\x32";

    decode_resp = transport.DecodeSessionResp (data, 6, sess_resp);
    ASSERT_EQ(decode_resp, SANE_STATUS_IO_ERROR);

    decode_resp = transport.DecodeSessionResp (data, 4, sess_resp);
    ASSERT_EQ(decode_resp, SANE_STATUS_IO_ERROR);

    decode_resp = transport.DecodeSessionResp (data, 0, sess_resp);
    ASSERT_EQ(decode_resp, SANE_STATUS_IO_ERROR);

    // Content problems.
    const char *content_problems[] = {
        "\x32" "\x10" "\x01" "\x02" "\x20",
        "\x06" "\x32" "\x01" "\x02" "\x20",
        "\x06" "\x10" "\x32" "\x02" "\x20",
        "\x06" "\x10" "\x01" "\x32" "\x20",
        "\x06" "\x10" "\x01" "\x02" "\x32",
        nullptr
    };

    for (size_t test = 0; test; test++)
      {
        decode_resp = transport.DecodeSessionResp ((const SANE_Byte *)content_problems[test], 5, sess_resp);
        ASSERT_EQ(decode_resp, SANE_STATUS_IO_ERROR);
      }
  }

  void test_decode_button_resp()
  {
    SANE_Status decode_resp;
    BrotherButtonStateResponse button_resp;
    BrotherTransportUSB transport("");

    // Real "No button" response from DCP-L2500D
    const SANE_Byte *data = (const SANE_Byte *)"\x05" "\x10" "\x03" "\x00" "\x01";

    decode_resp = transport.DecodeButtonStateResp(data, 5, button_resp);
    ASSERT_EQ(decode_resp, SANE_STATUS_GOOD);
    ASSERT_EQ(button_resp.button_detected, SANE_FALSE);

    // Real "No button" response from DCP-L2500D. The first \x04 happens from time to time.
    data = (const SANE_Byte *)"\x04" "\x10" "\x03" "\x00" "\x01";

    decode_resp = transport.DecodeButtonStateResp(data, 5, button_resp);
    ASSERT_EQ(decode_resp, SANE_STATUS_GOOD);
    ASSERT_EQ(button_resp.button_detected, SANE_FALSE);

    // Real "Image button" response from DCP-L2500D
    data = (const SANE_Byte *)"\x09" "\x10" "\x03" "\x20" "\x03";

    decode_resp = transport.DecodeButtonStateResp(data, 5, button_resp);
    ASSERT_EQ(decode_resp, SANE_STATUS_GOOD);
    ASSERT_EQ(button_resp.button_detected, SANE_TRUE);
    ASSERT_EQ(button_resp.button_value, BROTHER_SENSOR_IMAGE);
  }
};

/*
 * TEST: DecodeSessionResp()
 *
// */
//static void test_family4_decode_session_resp()
//{
//  SANE_Status decode_resp;
//  BrotherSessionResponse sess_resp;
//
//  BrotherTransportUSB transport("");
//
//  // SUCCESS status
//  const SANE_Byte *data = (const SANE_Byte *)"\x05" "\x10" "\x01" "\x02" "\x00";
//
//  decode_resp = transport.DecodeSessionResp (data, 5, sess_resp);
//
//  ASSERT_EQ(decode_resp, SANE_STATUS_GOOD);
//  ASSERT_TRUE(sess_resp.ready);
//
//  // BUSY status
//  data = (const SANE_Byte *)"\x05" "\x10" "\x01" "\x02" "\x80";
//
//  decode_resp = transport.DecodeSessionResp (data, 5, sess_resp);
//
//  ASSERT_EQ(decode_resp, SANE_STATUS_GOOD);
//  ASSERT_FALSE(sess_resp.ready);
//
//  // Length issues
//  data = (const SANE_Byte *)"\x05" "\x10" "\x01" "\x02" "\x20" "\x32";
//
//  decode_resp = transport.DecodeSessionResp (data, 6, sess_resp);
//  ASSERT_EQ(decode_resp, SANE_STATUS_IO_ERROR);
//
//  decode_resp = transport.DecodeSessionResp (data, 4, sess_resp);
//  ASSERT_EQ(decode_resp, SANE_STATUS_IO_ERROR);
//
//  decode_resp = transport.DecodeSessionResp (data, 0, sess_resp);
//  ASSERT_EQ(decode_resp, SANE_STATUS_IO_ERROR);
//
//  // Content problems.
//  const char *content_problems[] = {
//      "\x32" "\x10" "\x01" "\x02" "\x20",
//      "\x06" "\x32" "\x01" "\x02" "\x20",
//      "\x06" "\x10" "\x32" "\x02" "\x20",
//      "\x06" "\x10" "\x01" "\x32" "\x20",
//      "\x06" "\x10" "\x01" "\x02" "\x32",
//      nullptr
//  };
//
//  for (size_t test = 0; test; test++)
//    {
//      decode_resp = transport.DecodeSessionResp ((const SANE_Byte *)content_problems[test], 5, sess_resp);
//      ASSERT_EQ(decode_resp, SANE_STATUS_IO_ERROR);
//    }
//}

/*
 * Run all the tests.
 *
 */
void test_family_transport_usb ()
{
  BrotherTransportUSBTests test_suite;
  // Decodes.
  test_suite.test_decode_session_resp();
  test_suite.test_decode_button_resp();
}
