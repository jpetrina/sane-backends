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


#include <cstddef>
#include <stdio.h>
#include <string.h>
#include <jpeglib.h>
#include <queue>

#include "../genesys/minigtest.h"
#include "brother_mfp_tests_usb_mock.h"


std::queue<std::vector<SANE_Byte>> mock_device_x_responses;

const size_t control_activate_response_length = 5;
const SANE_Byte control_activate_response[control_activate_response_length] = {
  0x05, 0x10, 0x01, 0x02, 0x00
};

const size_t q_response_length = 72;
const SANE_Byte q_response[q_response_length] = {
  0xC1, 0x00, 0x46, 0x0B, 0xFF, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0x00, 0x01, 0x04, 0x01, 0x01, 0x01, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x34, 0x08, 0x5F, 0x0B, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x1D, 0x00, 0x1D, 0x00, 0x00, 0x01, 0x01, 0x58, 0x02, 0x60,
  0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00
};

const size_t s_response_length = 1;
const SANE_Byte s_response[s_response_length] = { 0x80 };

const size_t i_response_length = 32;
const SANE_Byte i_response[i_response_length] = "\0\x1D\0300,300,2,209,2480,291,3437,\0";

/* The state of the mocked device in integration tests. */
struct {
  char last_query_type = '\0';
} mock_device_state;

extern "C" void
sanei_init_debug (const char * backend, int * var) {
  // nothing to do in this mock
  (void)backend;
  (void)var;
}


extern "C" void
sanei_debug_msg (int level, int max_level, const char *be, const char *fmt, va_list ap) {
  // keep it simple for this mock, just print to stdout
  (void)level;
  (void)max_level;
  (void)be;
  vfprintf (stdout, fmt, ap);
}

extern "C" SANE_Status
sanei_usb_control_msg (SANE_Int dn, SANE_Int rtype, SANE_Int req,
		       SANE_Int value, SANE_Int index, SANE_Int len,
		       SANE_Byte * data) {
  printf("control_transfer() mock called\n");
  (void)dn;
  (void)rtype;
  (void)req;
  (void)value;
  (void)index;
  ASSERT_TRUE((size_t)len >= control_activate_response_length);
  memcpy(data, control_activate_response, control_activate_response_length);  
  return SANE_STATUS_GOOD;
}

extern "C" SANE_Status sanei_usb_open (SANE_String_Const devname, SANE_Int * dn) {
  (void)devname;
  (void)dn;
  printf("open() mock called\n");
  return SANE_STATUS_GOOD;
}

extern "C" void sanei_usb_close (SANE_Int dn) {
  (void)dn;
  printf("close() mock called\n");
  mock_device_state.last_query_type = '\0';
}


void add_mock_x_response(std::vector<SANE_Byte> &x)
{
   mock_device_x_responses.push(x);
}

void add_mock_x_response(std::vector<SANE_Byte> &&x)
{
   mock_device_x_responses.push(x);
}

extern "C" SANE_Status
sanei_usb_read_bulk (SANE_Int dn, SANE_Byte * buffer, size_t * size) {
  (void)dn;
  printf("read_bulk() mock called\n");
  switch (mock_device_state.last_query_type) {
    case '\0':
      // no query was made, so nothing to read
      *size = 0;
      return SANE_STATUS_GOOD;
    case 'Q':
      *size = q_response_length;
      memcpy(buffer, q_response, q_response_length);
      mock_device_state.last_query_type = '\0';
      return SANE_STATUS_GOOD;
    case 'S':
      *size = s_response_length;
      memcpy(buffer, s_response, s_response_length);
      mock_device_state.last_query_type = '\0';
      return SANE_STATUS_GOOD;
    case 'I':
      *size = i_response_length;
      memcpy(buffer, i_response, i_response_length);
      mock_device_state.last_query_type = '\0';
      return SANE_STATUS_GOOD;
    case 'X': {
      if (mock_device_x_responses.empty()) {
        // we assume this is the "flush" mode - pretend that no more data is available  
        *size = 0;
        return SANE_STATUS_GOOD;
      }

      *size = mock_device_x_responses.front().size();
      memcpy(buffer, mock_device_x_responses.front().data(), mock_device_x_responses.front().size());
      mock_device_x_responses.pop();
      return SANE_STATUS_GOOD;
    }
    default:
      printf("ERROR IN USB MOCK: Unhandled query type: %c\n", mock_device_state.last_query_type);
      return SANE_STATUS_IO_ERROR;
  }
}

extern "C" SANE_Status
sanei_usb_write_bulk (SANE_Int dn, const SANE_Byte * buffer, size_t * size) {
  (void)dn;
  printf("write_bulk() mock called\n");
  if (*size >= 2) {
    mock_device_state.last_query_type = buffer[1];
    printf("write_bulk() mock: query type %c\n", mock_device_state.last_query_type);
  }
  return SANE_STATUS_GOOD;
}
