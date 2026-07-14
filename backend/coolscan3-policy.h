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

#ifndef COOLSCAN3_POLICY_H
#define COOLSCAN3_POLICY_H

typedef enum
{
	CS3_SCAN_MODE_OK,
	CS3_SCAN_MODE_INFRARED_UNSUPPORTED,
	CS3_SCAN_MODE_PACKED_LAYOUT_REQUIRED
}
cs3_scan_mode_result_t;

cs3_scan_mode_result_t cs3_scan_mode_check(int infrared_supported,
					    int infrared,
					    int samples_per_scan);

int cs3_start_result_is_scanning(int start_succeeded);
int cs3_preview_change_requires_reload_params(int current_preview,
					      int requested_preview);

/* frame_count == 0 is the backend's exhausted-batch sentinel. */
int cs3_frame_count_for_frame(int frame_capacity, int frame,
			      int frame_count);
int cs3_frame_selection_is_valid(int frame_capacity, int frame,
				 int frame_count);

typedef enum
{
	CS3_REISSUE_DONE,
	CS3_REISSUE_ISSUE,
	CS3_REISSUE_EXHAUSTED
}
cs3_reissue_action_t;

typedef struct
{
	unsigned int attempts;
	unsigned int limit;
}
cs3_reissue_state_t;

void cs3_reissue_state_init(cs3_reissue_state_t * state,
			    unsigned int limit);
cs3_reissue_action_t cs3_reissue_state_step(cs3_reissue_state_t * state,
					    int reissue_requested);

#endif
