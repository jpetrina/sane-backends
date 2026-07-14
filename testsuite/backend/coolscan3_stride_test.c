/* Scanner-free regression tests for coolscan3 multisample layout.
 *
 * Copyright (C) 2026 Rohan Pandula
 *
 * This file is part of the SANE package.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2.
 */

#include <assert.h>

#include "coolscan3-stride.h"

int
main(void)
{
	/* Odd-width 8-bit RGB has three 6-byte planes per pass. */
	assert(cs3_multisample_read_size(18, 4) == 72);
	assert(cs3_multisample_read_size(512, 4) == 2048);
	assert(cs3_multisample_read_size(512, 1) == 512);
	assert(cs3_multisample_read_size(24064, 4) == 96256);
	assert(cs3_8bit_sample_offset(18, 5, 0, 0, 1, 0) == 1);
	assert(cs3_8bit_sample_offset(18, 5, 2, 0, 1, 0) == 13);
	assert(cs3_8bit_sample_offset(18, 5, 0, 1, 1, 0) == 19);
	assert(cs3_8bit_sample_offset(18, 5, 2, 3, 1, 4) == 71);
	assert(cs3_8bit_sample_offset(512, 5, 0, 1, 1, 0) == 513);
	assert(cs3_8bit_sample_offset(512, 5, 2, 3, 1, 4) == 1553);

	/* At 4000 dpi, one 3946-pixel, 16-bit RGB pass has 23,676
	 * payload bytes and a 24,064-byte padded transfer stride. */
	assert(cs3_16bit_sample_offset(24064, 3946, 0, 0, 0) == 0);
	assert(cs3_16bit_sample_offset(24064, 3946, 2, 0, 3945) == 23674);
	assert(cs3_16bit_sample_offset(24064, 3946, 0, 1, 0) == 24064);
	assert(cs3_16bit_sample_offset(24064, 3946, 0, 1, 0) -
	       cs3_16bit_sample_offset(23676, 3946, 0, 1, 0) == 388);
	assert(cs3_16bit_sample_offset(24064, 3946, 2, 3, 3945) == 95866);

	{
		unsigned char wire[256];
		const unsigned long stride = 64, width = 5;
		unsigned int sample, color;
		unsigned long pixel, offset, value, sum, wrong;

		/* Four identical RGB passes have 30 payload bytes each.  Fill
		 * the rest of every 64-byte pass with a visible sentinel. */
		for (offset = 0; offset < sizeof(wire); offset++)
			wire[offset] = 0xee;
		for (sample = 0; sample < 4; sample++)
			for (color = 0; color < 3; color++)
				for (pixel = 0; pixel < width; pixel++) {
					value = (color + 1) * 1000 + pixel;
					offset = sample * stride
						+ 2 * (color * width + pixel);
					wire[offset] = (value >> 8) & 0xff;
					wire[offset + 1] = value & 0xff;
				}
		for (color = 0; color < 3; color++)
			for (pixel = 0; pixel < width; pixel++) {
				value = (color + 1) * 1000 + pixel;
				sum = 0;
				for (sample = 0; sample < 4; sample++) {
					offset = cs3_16bit_sample_offset(
						stride, width, color,
						sample, pixel);
					sum += ((unsigned long) wire[offset] << 8)
						+ wire[offset + 1];
				}
				assert(sum / 4 == value);
			}

		/* The old 30-byte payload stride lands in padding when it
		 * tries to read the first pixel of pass two. */
		offset = cs3_16bit_sample_offset(30, width, 0, 1, 0);
		wrong = ((unsigned long) wire[offset] << 8)
			+ wire[offset + 1];
		assert(wrong != 1000);
	}

	return 0;
}
