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

#ifndef COOLSCAN3_STRIDE_H
#define COOLSCAN3_STRIDE_H

unsigned long cs3_multisample_read_size(unsigned long single_sample_bytes,
					unsigned int samples_per_scan);

/* pass_stride_bytes is the complete padded single-pass transfer length.
 * Multi-sample passes begin at this stride, not at the end of the bare
 * pixel payload. */
unsigned long cs3_8bit_sample_offset(unsigned long pass_stride_bytes,
				     unsigned long logical_width,
				     unsigned int color,
				     unsigned int sample,
				     unsigned int odd_padding,
				     unsigned long pixel);
unsigned long cs3_16bit_sample_offset(unsigned long pass_stride_bytes,
				      unsigned long logical_width,
				      unsigned int color,
				      unsigned int sample,
				      unsigned long pixel);

#endif
