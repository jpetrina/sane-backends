/* Scanner-free regression tests for coolscan3 option and command policy.
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

#include "coolscan3-policy.h"

int
main(void)
{
	cs3_reissue_state_t reissue;

	assert(cs3_scan_mode_check(1, 0, 4) == CS3_SCAN_MODE_OK);
	assert(cs3_scan_mode_check(1, 1, 4) ==
	       CS3_SCAN_MODE_INFRARED_MULTISAMPLE_UNSAFE);
	assert(cs3_scan_mode_check(1, 1, 1) == CS3_SCAN_MODE_OK);
	assert(cs3_scan_mode_check(1, 1, 2) ==
	       CS3_SCAN_MODE_INFRARED_MULTISAMPLE_UNSAFE);
	assert(cs3_scan_mode_check(0, 1, 1) ==
	       CS3_SCAN_MODE_INFRARED_UNSUPPORTED);
	assert(!cs3_start_result_is_scanning(0));
	assert(cs3_start_result_is_scanning(1));
	assert(cs3_preview_change_requires_reload_params(0, 1));
	assert(cs3_preview_change_requires_reload_params(1, 0));
	assert(!cs3_preview_change_requires_reload_params(0, 0));
	assert(!cs3_preview_change_requires_reload_params(1, 1));
	assert(cs3_frame_selection_is_valid(40, 1, 40));
	assert(!cs3_frame_selection_is_valid(40, 2, 40));
	assert(cs3_frame_selection_is_valid(40, 2, 39));
	/* Frame first: clamp 40 to the 39 positions remaining at frame 2. */
	assert(cs3_frame_count_for_frame(40, 2, 40) == 39);
	/* Count first: an already valid count remains exact. */
	assert(cs3_frame_count_for_frame(40, 2, 39) == 39);
	assert(cs3_frame_selection_is_valid(40, 40, 1));
	assert(!cs3_frame_selection_is_valid(40, 40, 2));
	assert(cs3_frame_selection_is_valid(1, 1, 1));
	assert(!cs3_frame_selection_is_valid(40, 0, 1));
	assert(!cs3_frame_selection_is_valid(40, 41, 1));
	/* An exhausted batch may be repositioned before frame-count is reset. */
	assert(cs3_frame_selection_is_valid(40, 1, 0));
	assert(cs3_frame_count_for_frame(40, 2, 0) == 0);
	assert(!cs3_frame_selection_is_valid(40, 1, -1));

	cs3_reissue_state_init(&reissue, 4);
	assert(cs3_reissue_state_step(&reissue, 0) == CS3_REISSUE_DONE);
	assert(reissue.attempts == 0);

	cs3_reissue_state_init(&reissue, 4);
	assert(cs3_reissue_state_step(&reissue, 1) == CS3_REISSUE_ISSUE);
	assert(reissue.attempts == 1);
	assert(cs3_reissue_state_step(&reissue, 1) == CS3_REISSUE_ISSUE);
	assert(reissue.attempts == 2);
	assert(cs3_reissue_state_step(&reissue, 0) == CS3_REISSUE_DONE);

	cs3_reissue_state_init(&reissue, 4);
	assert(cs3_reissue_state_step(&reissue, 1) == CS3_REISSUE_ISSUE);
	assert(cs3_reissue_state_step(&reissue, 1) == CS3_REISSUE_ISSUE);
	assert(cs3_reissue_state_step(&reissue, 1) == CS3_REISSUE_ISSUE);
	assert(cs3_reissue_state_step(&reissue, 1) == CS3_REISSUE_ISSUE);
	assert(cs3_reissue_state_step(&reissue, 1) == CS3_REISSUE_EXHAUSTED);
	assert(reissue.attempts == 4);
	return 0;
}
