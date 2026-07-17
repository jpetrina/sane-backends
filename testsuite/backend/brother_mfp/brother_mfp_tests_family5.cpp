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

/*
 * TEST: BrotherImageCache()
 *
 */
static void test_family5_image_cache()
{
  BrotherImageCache cache;

  // Text
  ASSERT_TRUE(cache.WriteData((const SANE_Byte *)"Hello", 5));
  ASSERT_TRUE(cache.WriteData((const SANE_Byte *)"", 0));
  ASSERT_TRUE(cache.WriteData((const SANE_Byte *)"ThereYou!", 9));

  // Binary
  ASSERT_TRUE(cache.WriteData((const SANE_Byte *)"\xff,\x01,\x00", 3));

  // Read back.
  SANE_Byte read_buffer[100];
  size_t bytes_read = 0;

  ASSERT_TRUE(cache.ReadData(read_buffer, 3, bytes_read));
  ASSERT_EQ(bytes_read, (size_t)3);
  ASSERT_EQ(memcmp("Hel", read_buffer, 3), 0);

  ASSERT_TRUE(cache.ReadData(read_buffer, 6, bytes_read));
  ASSERT_EQ(bytes_read, (size_t)6);
  ASSERT_EQ(memcmp("loTher", read_buffer, 6), 0);

  ASSERT_TRUE(cache.ReadData(read_buffer, 10, bytes_read));
  ASSERT_EQ(bytes_read, (size_t)8);
  ASSERT_EQ(memcmp("eYou!\xff,\x01,\x00", read_buffer, 8), 0);

  // Can't write after reading.
  ASSERT_TRUE(!cache.WriteData((const SANE_Byte *)"NOT", 3));
}

/*
 * Run all the tests.
 *
 */
void test_family5 ()
{
  // Decodes.
  test_family5_image_cache();
}
