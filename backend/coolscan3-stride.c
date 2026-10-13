/* Scanner-independent multisample layout helpers for coolscan3.
 *
 * Copyright (C) 2026 Rohan Pandula
 *
 * This file is part of the SANE package.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2.
 */

#include "coolscan3-stride.h"

unsigned long
cs3_multisample_read_size(unsigned long single_sample_bytes,
			  unsigned int samples_per_scan)
{
	return single_sample_bytes * samples_per_scan;
}

unsigned long
cs3_8bit_sample_offset(unsigned long pass_stride_bytes,
		       unsigned long logical_width, unsigned int color,
		       unsigned int sample, unsigned int odd_padding,
		       unsigned long pixel)
{
	return sample * pass_stride_bytes
		+ color * (logical_width + odd_padding) + odd_padding
		+ pixel;
}

unsigned long
cs3_16bit_sample_offset(unsigned long pass_stride_bytes,
			unsigned long logical_width, unsigned int color,
			unsigned int sample, unsigned long pixel)
{
	return sample * pass_stride_bytes
		+ 2UL * (color * logical_width + pixel);
}
