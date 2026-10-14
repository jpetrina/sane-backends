/* Scanner-independent safety policy shared by coolscan3 and its tests.
 *
 * Copyright (C) 2026 Rohan Pandula
 *
 * This file is part of the SANE package.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2.
 */

#include "coolscan3-policy.h"

cs3_scan_mode_result_t
cs3_scan_mode_check(int infrared_supported, int infrared,
		    int samples_per_scan)
{
	if (infrared && !infrared_supported)
		return CS3_SCAN_MODE_INFRARED_UNSUPPORTED;
	if (infrared && samples_per_scan > 1)
		return CS3_SCAN_MODE_PACKED_LAYOUT_REQUIRED;
	return CS3_SCAN_MODE_OK;
}

int
cs3_start_result_is_scanning(int start_succeeded)
{
	return start_succeeded != 0;
}

int
cs3_preview_change_requires_reload_params(int current_preview,
					  int requested_preview)
{
	return current_preview != requested_preview;
}

int
cs3_frame_count_for_frame(int frame_capacity, int frame, int frame_count)
{
	int remaining;

	if (frame_capacity < 1 || frame < 1 || frame > frame_capacity ||
	    frame_count < 0)
		return -1;
	if (frame_count == 0)
		return 0;
	remaining = frame_capacity - frame + 1;
	return frame_count < remaining ? frame_count : remaining;
}

int
cs3_frame_selection_is_valid(int frame_capacity, int frame, int frame_count)
{
	int adjusted = cs3_frame_count_for_frame(frame_capacity, frame,
						 frame_count);

	return adjusted >= 0 && adjusted == frame_count;
}

void
cs3_reissue_state_init(cs3_reissue_state_t * state, unsigned int limit)
{
	state->attempts = 0;
	state->limit = limit;
}

cs3_reissue_action_t
cs3_reissue_state_step(cs3_reissue_state_t * state, int reissue_requested)
{
	if (!reissue_requested)
		return CS3_REISSUE_DONE;
	if (state->attempts < state->limit) {
		state->attempts++;
		return CS3_REISSUE_ISSUE;
	}
	return CS3_REISSUE_EXHAUSTED;
}
