/* sane - Scanner Access Now Easy.
   Copyright (C) 2006 Lauri Pirttiaho <lauri.pirttiaho@cornell.edu>

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
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston,
   MA 02111-1307, USA.

   As a special exception, the authors of SANE give permission for
   additional uses of the libraries contained in this release of SANE.

   The exception is that, if you link a SANE library with other files
   to produce an executable, this does not by itself cause the
   resulting executable to be covered by the GNU General Public
   License.  Your use of that executable is in no way restricted on
   account of linking the SANE library code into it.

   This exception does not, however, invalidate any other reasons why
   the executable file might be covered by the GNU General Public
   License.

   If you submit changes to SANE to the maintainers to be included in
   a subsequent release, you agree by submitting the changes that
   those changes may be distributed with this exception intact.

   If you write modifications of your own for SANE, it is your choice
   whether to permit this exception to apply to your modifications.
   If you do not wish that, delete this exception notice.

   This backend is for CanoScan 3200F.
*/

#ifndef CS3200F_DAT_H
#define CS3200F_DAT_H

#include "../include/sane/sane.h"

/***********************************************************************
 * Options
 ***********************************************************************/

/** Option identifiers */
typedef enum
  {
    OPT_NUM_OPTS = 0,
    OPT_RESOLUTION,      /*  1 */
    OPT_SOURCE,          /*  2 */

    OPT_MODE_GROUP,      /*  3 */
    OPT_MODE,            /*  4 */
    OPT_DEPTH,           /*  5 */
    OPT_GRAY_COLOR,      /*  6 */
    OPT_PREVIEW,         /*  7 */

    OPT_GAMMA_GROUP,     /*  8 */
    OPT_RGB_BIND,        /*  9 */
    OPT_GAMMA,           /* 10 */
    OPT_GAMMA_R,         /* 11 */
    OPT_GAMMA_G,         /* 12 */
    OPT_GAMMA_B,         /* 13 */
    OPT_THRESHOLD,       /* 14 */

    OPT_GEOMETRY_GROUP,  /* 15 */
    OPT_TL_X,            /* 16 */
    OPT_TL_Y,            /* 17 */
    OPT_BR_X,            /* 18 */
    OPT_BR_Y,            /* 19 */

    OPT_CALIBRATE_GROUP, /* 20 */
    OPT_CALIBRATE,       /* 21 */
    OPT_CALIBRATE_BEFORE_SCAN, /* 22 */

    NUM_OPTIONS          /* 23 */
  } OPT_E;

/************************************************************************
 * Option constraints
 ************************************************************************/

/** Sources */
typedef enum
  {
    OPT_SOURCE_PAPER,
    OPT_SOURCE_FILM
  } OPT_SOURCE_E;

/** Modes */
typedef enum
  {
    OPT_MODE_COLOR,
    OPT_MODE_GRAY,
    OPT_MODE_LINEART
  } OPT_MODE_E;

/** RGB Color */
typedef enum
  {
    OPT_RGB_COLOR_RED,
    OPT_RGB_COLOR_GREEN,
    OPT_RGB_COLOR_BLUE
  } OPT_RGB_COLOR_E;

/** Geometry limits in mm */
#define OPT_X_MAX 219.0
#define OPT_X_MAX_TA 38.0
#define OPT_Y_MAX 299.0
#define OPT_Y_MAX_TA 25.0

/** Resolutions */
extern const SANE_Word opt_const_resolutions[];

/** Sources */
extern const SANE_String_Const opt_const_sources[];
#define OPT_CONST_SOURCES_MAX_LEN (sizeof(SANE_I18N("Paper")))

/** Modes */
extern const SANE_String_Const opt_const_modes[];
#define OPT_CONST_MODES_MAX_LEN (sizeof(SANE_VALUE_SCAN_MODE_LINEART))

/** Sample depths */
extern const SANE_Word opt_const_depths[];

/** RGB Color */
extern const SANE_String_Const opt_const_rgb_colors[];
#define OPT_CONST_RGB_COLORS_MAX_LEN (sizeof(SANE_I18N("Green")))

/** Gamma */
extern const SANE_Range opt_gamma_range;

/** Threshold */
extern const SANE_Range opt_threshold_range;

/** X range in mm */
extern const SANE_Range opt_x_range;

/** X range in mm */
extern const SANE_Range opt_x_range_ta;

/** Y range in mm */
extern const SANE_Range opt_y_range;

/** Y range in mm */
extern const SANE_Range opt_y_range_ta;

/** Device specific options */
typedef struct
{
  SANE_Int        resolution;
  OPT_SOURCE_E    source;
  OPT_MODE_E      mode;
  SANE_Int        depth;
  OPT_RGB_COLOR_E gray_color;
  SANE_Bool       preview;
  SANE_Bool       rgb_bind;
  SANE_Fixed      gamma;
  SANE_Fixed      gamma_r;
  SANE_Fixed      gamma_g;
  SANE_Fixed      gamma_b;
  SANE_Fixed      threshold;
  SANE_Fixed      tl_x;
  SANE_Fixed      tl_y;
  SANE_Fixed      br_x;
  SANE_Fixed      br_y;
  SANE_Fixed      tl_x_ta;
  SANE_Fixed      tl_y_ta;
  SANE_Fixed      br_x_ta;
  SANE_Fixed      br_y_ta;
  SANE_Bool       calibrate_before_scan;
} OPT_T;

/************************************************************************
 * HW control
 ************************************************************************/

/** RGB triplet */
typedef struct
{
  char r;
  char g;
  char b;
} RGB_T;

/** AFE settings */
typedef struct {
  RGB_T offs;
  RGB_T gain;
} AFE_SET_T;

/************************************************************************
 * Calibration data
 ************************************************************************/

typedef struct
{
  SANE_Int res; /*< resolution */
  SANE_Int offs_x; /*< imaging area offset in 1200 dpi pixels */
  SANE_Int offs_y; /*< imaging area offset in 2400 dpi lines */
  AFE_SET_T afe_set; /*< AFE settings */
  SANE_Int width; /*< width of the imaging area in pixels */
  double *bshd; /*< averaged black shading data */
  double *wshd; /*< averaged white shading data */
} CAL_T;

/************************************************************************
 * Line FIFO
 ************************************************************************/

typedef struct
{
    int width;
    int length;
    int length2;
    int write_idx;
    int read_idx;
    SANE_Byte *data;
} LINE_FIFO_T;

typedef struct
{
  SANE_Int n_lines;
  SANE_Int current_line;
  SANE_Int line_width;
  SANE_Int line_pos;
  SANE_Byte *line;

  LINE_FIFO_T *r;
  LINE_FIFO_T *g;
  LINE_FIFO_T *b;
} SCAN_T;

/************************************************************************
 * Device data
 ************************************************************************/

typedef struct DEVICE DEVICE_T;
typedef DEVICE_T *DEVICE_H;

struct DEVICE
{
  DEVICE_T *next;
  DEVICE_T *prev;

  SANE_String name;

  SANE_Int dn;
  SANE_Bool open;
  SANE_Bool hs;

  SANE_Bool scanning;
  SANE_Pid scan_thread;
  int scan_pipe[2];
  int scan_bytes_left;

  OPT_T opt;

  CAL_T cal_300;
  CAL_T cal_600;
  CAL_T cal_1200;
  CAL_T cal_300_ta;
  CAL_T cal_600_ta;
  CAL_T cal_1200_ta;

  SCAN_T scan;
};

/** Initialize device data
 */
void
a_init_data( void );

/** Create new device data
 *  @return handle to the device
 */
SANE_Handle
a_new_dev( void );

/** Check if the give handle is valid
 *  @param h handle of the device
 *  @return SANE_STATUS_GOOD if OK, SANE_STATUS_INVAL if invalid
 */
SANE_Status
a_handle_valid( SANE_Handle h );

/** Delete device data
 *  @param d handle to the device
 */
void
a_delete_dev( SANE_Handle d );

/** Option getters */
SANE_Int          a_get_resolution( SANE_Handle d );
SANE_String_Const a_get_source( SANE_Handle d );
OPT_SOURCE_E      a_get_source_e( SANE_Handle d );
SANE_String_Const a_get_mode( SANE_Handle d );
OPT_MODE_E        a_get_mode_e( SANE_Handle d );
SANE_Int          a_get_depth( SANE_Handle d );
SANE_String_Const a_get_gray_color( SANE_Handle d );
OPT_RGB_COLOR_E   a_get_gray_color_e( SANE_Handle d );
SANE_Bool         a_get_preview( SANE_Handle d );
SANE_Bool         a_get_rgb_bind( SANE_Handle d );
SANE_Fixed        a_get_gamma( SANE_Handle d );
SANE_Fixed        a_get_gamma_r( SANE_Handle d );
SANE_Fixed        a_get_gamma_g( SANE_Handle d );
SANE_Fixed        a_get_gamma_b( SANE_Handle d );
SANE_Fixed        a_get_threshold( SANE_Handle d );
SANE_Fixed        a_get_tl_x( SANE_Handle d );
SANE_Fixed        a_get_tl_y( SANE_Handle d );
SANE_Fixed        a_get_br_x( SANE_Handle d );
SANE_Fixed        a_get_br_y( SANE_Handle d );
SANE_Int          a_get_tl_x_pix( SANE_Handle d );
SANE_Int          a_get_tl_y_pix( SANE_Handle d );
SANE_Int          a_get_br_x_pix( SANE_Handle d );
SANE_Int          a_get_br_y_pix( SANE_Handle d );
SANE_Bool         a_get_calibrate_before_scan( SANE_Handle d );

/** Option setters */
SANE_Int a_set_resolution( SANE_Handle d, SANE_Int resolution );
SANE_Int a_set_source( SANE_Handle d, SANE_String_Const source );
SANE_Int a_set_mode( SANE_Handle d, SANE_String_Const mode );
SANE_Int a_set_depth( SANE_Handle d, SANE_Int depth );
SANE_Int a_set_gray_color( SANE_Handle d, SANE_String_Const gray_color );
SANE_Int a_set_preview( SANE_Handle d, SANE_Bool preview );
SANE_Int a_set_rgb_bind( SANE_Handle d, SANE_Bool rgb_bind );
SANE_Int a_set_gamma( SANE_Handle d, SANE_Fixed gamma );
SANE_Int a_set_gamma_r( SANE_Handle d, SANE_Fixed gamma_r );
SANE_Int a_set_gamma_g( SANE_Handle d, SANE_Fixed gamma_g );
SANE_Int a_set_gamma_b( SANE_Handle d, SANE_Fixed gamma_b );
SANE_Int a_set_threshold( SANE_Handle d, SANE_Fixed threshold );
SANE_Int a_set_tl_x( SANE_Handle d, SANE_Fixed tl_x );
SANE_Int a_set_tl_y( SANE_Handle d, SANE_Fixed tl_y );
SANE_Int a_set_br_x( SANE_Handle d, SANE_Fixed br_x );
SANE_Int a_set_br_y( SANE_Handle d, SANE_Fixed br_y );
SANE_Int a_set_calibrate_before_scan( SANE_Handle d, SANE_Bool cbs );

/** Scan parameters */
SANE_Frame a_format( SANE_Handle handle );
SANE_Int a_depth( SANE_Handle handle );
SANE_Int a_lines( SANE_Handle handle );
SANE_Int a_pixels_per_line( SANE_Handle handle );
SANE_Int a_bytes_per_line( SANE_Handle handle );
void a_scan_init( SANE_Handle handle );
void a_scan_finish( SANE_Handle handle );

/** Line FIFO */
LINE_FIFO_T *a_new_fifo( int width, int length, int length2 );
void a_delete_fifo( LINE_FIFO_T *fifo );
int a_write_idx( LINE_FIFO_T *fifo );
int a_read_idx( LINE_FIFO_T *fifo );
int a_read_idx2( LINE_FIFO_T *fifo );
void a_write_idx_inc( LINE_FIFO_T *fifo );
void a_read_idx_inc( LINE_FIFO_T *fifo );
int a_space_in_fifo( LINE_FIFO_T *fifo );
SANE_Byte *a_fifo_write_ptr( LINE_FIFO_T *fifo );
SANE_Byte *a_fifo_read_ptr( LINE_FIFO_T *fifo );
SANE_Byte *a_fifo_read_ptr2( LINE_FIFO_T *fifo );
int a_fifo_width( LINE_FIFO_T *fifo );
int a_write_to_fifo( LINE_FIFO_T *fifo, int in_pipe );
void a_read_from_fifo_g( SCAN_T *scan );
void a_read_from_fifo_g_2( SCAN_T *scan );
void a_read_from_fifos_c( SCAN_T *scan );
void a_read_from_fifos_c_2( SCAN_T *scan );
SANE_Int a_read_from_scan( SCAN_T *scan, SANE_Byte *data, SANE_Int max );
SANE_Bool a_lines_left( SCAN_T *scan );
SANE_Bool a_line_empty( SCAN_T *scan );

#endif /* ndef CS3200F_DAT_H */

/************************************************************************
 * $Id: cs3200f-dat.h,v 1.1 2006/03/26 19:01:59 lapi-guest Exp $
 ************************************************************************/
