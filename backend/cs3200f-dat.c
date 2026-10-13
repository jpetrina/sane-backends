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

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "cs3200f-dat.h"

/***********************************************************************
 * Option constraint definitions
 ***********************************************************************/

/** Resolutions */
const SANE_Word opt_const_resolutions[] =
  { 
    5,
    75, 150, 300, 600, 1200
  };

/** Sources */
const SANE_String_Const opt_const_sources[] =
  {
    SANE_I18N( "Paper" ),
    SANE_I18N( "Film" ),
    0
  };

/** Modes */
const SANE_String_Const opt_const_modes[] =
  {
    SANE_VALUE_SCAN_MODE_COLOR,
    SANE_VALUE_SCAN_MODE_GRAY,
    SANE_VALUE_SCAN_MODE_LINEART,
    0
  };

/** Sample depths */
const SANE_Word opt_const_depths[] =
  { 
    2,
    8, 16
  };

/** RGB Color */
const SANE_String_Const opt_const_rgb_colors[] =
  {
    SANE_I18N( "Red" ),
    SANE_I18N( "Green" ),
    SANE_I18N( "Blue" ),
    0
  };

/** Gamma */
const SANE_Range opt_gamma_range =
  {
    SANE_FIX(  0.01 ),
    SANE_FIX( 10.00 ),
    0
  };

/** Threshold */
const SANE_Range opt_threshold_range =
  {
    SANE_FIX(   0.0 ),
    SANE_FIX( 100.0 ),
    0
  };

/** X range in mm */
const SANE_Range opt_x_range =
  {
    SANE_FIX(   0.0 ),
    SANE_FIX( OPT_X_MAX ),
    0
  };

/** X range in mm */
const SANE_Range opt_x_range_ta =
  {
    SANE_FIX(   0.0 ),
    SANE_FIX( OPT_X_MAX_TA ),
    0
  };

/** Y range in mm */
const SANE_Range opt_y_range =
  {
    SANE_FIX(   0.0 ),
    SANE_FIX( OPT_Y_MAX ),
    0
  };

/** Y range in mm */
const SANE_Range opt_y_range_ta =
  {
    SANE_FIX(   0.0 ),
    SANE_FIX( OPT_Y_MAX_TA ),
    0
  };


/***********************************************************************
 * Device data
 ***********************************************************************/

static DEVICE_H devices;

/***********************************************************************
 * Local protos
 ***********************************************************************/

static void a_init_dev( DEVICE_H d );
static void a_free_dev( DEVICE_H d );
static void a_init_cal( CAL_T *c, SANE_Int res );

static int a_valid_dev( DEVICE_H d );

/** Correct list value
 *  @param value pointer to the value submitted
 *  @param list pointer to the list of allowed values
 *  @return 0 or SANE_INFO_INEXACT
 */
static SANE_Int 
a_correct_list_value( SANE_Word *value, const SANE_Word *list );

/** Compare two strings ignoring the case
 *  @param a pointer to the first string to compare
 *  @param b pointer to the second string to compare
 *  @return difference of the first differing characters or 0 
 *          if the strings match
 */
static int 
a_strcmp_ic( const char *a, const char *b );

/** Correct string value
 *  @param value pointer to the value submitted
 *  @param strings pointer to the list of allowed strings
 *  @param ev enum value of the given option
 *  @return 0 or SANE_INFO_INEXACT
 *
 * Compares the submitted string to the allowed strings and returns
 * in ev the enum values of the matching string. If no strings match
 * returns the enum value 0 (first value).
 */
static SANE_Int 
a_correct_string_value( SANE_String_Const value, 
                        const SANE_String_Const *strings,
                        unsigned int *ev);

/** Correct range value
 *  @param value pointer to the value submitted
 *  @param range pointer to the list of allowed range
 *  @return 0 or SANE_INFO_INEXACT
 */
static SANE_Int 
a_correct_range_value( SANE_Word *value, const SANE_Range *range );

/***********************************************************************
 * Exported stuff
 ***********************************************************************/

void
a_init_data( void )
{
  DBG( D_CALL, "a_init_data()\n" );
  devices = 0;
}

SANE_Handle
a_new_dev( void )
{
  DEVICE_H d = 0;

  DBG( D_CALL, "a_new_dev()\n" );

  d = (DEVICE_H)malloc( sizeof( DEVICE_T ) );
  if ( d )
    {
      a_init_dev( d );
      d->next = 0;
      if ( devices == 0 )
        {
          devices = d;
          d->prev = 0;
        }
      else
        {
          DEVICE_H dh = devices;
          while ( dh->next )
            {
              dh = dh->next;
            }
          dh->next = d;
          d->prev = dh;
        }
    }

  DBG( D_RES, "return handle %08X\n", (unsigned int)d );
  return (SANE_Handle)d;
}

SANE_Status
a_handle_valid( SANE_Handle h )
{
  DEVICE_H dh = h;

  if ( a_valid_dev( dh ) )
    {
      return SANE_STATUS_GOOD;
    }
  else
    {
      return SANE_STATUS_INVAL;
    }
}

void
a_delete_dev( SANE_Handle d )
{
  DEVICE_H dh = d;

  DBG( D_CALL, "a_delete_dev( d = %08X)\n", (unsigned int)d );

  if ( a_valid_dev( dh ) )
    {
      a_free_dev( dh );

      if ( dh->prev )
        {
          dh->prev->next = dh->next;
        }
      else
        {
          devices = dh->next;
        }

      if ( dh->next )
        {
          dh->next->prev = dh->prev;
        }

      free( dh );
    }
}

/***********************************************************************
 * Getters
 ***********************************************************************/

SANE_Int
a_get_resolution( SANE_Handle d )
{
  DEVICE_H dh = d;

  DBG( D_CALL, "a_get_resolution( d = %08X )\n", (unsigned int)d );

  DBG( D_RES, "return %d\n", dh->opt.resolution );
  return dh->opt.resolution;
}

SANE_String_Const
a_get_source( SANE_Handle d )
{
  DEVICE_H dh = d;

  DBG( D_CALL, "a_get_source( d = %08X )\n", (unsigned int)d );

  DBG( D_RES, "return %s\n", opt_const_sources[ dh->opt.source ] );
  return opt_const_sources[ dh->opt.source ];
}

OPT_SOURCE_E
a_get_source_e( SANE_Handle d )
{
  DEVICE_H dh = d;

  DBG( D_CALL, "a_get_source_e( d = %08X )\n", (unsigned int)d );

  DBG( D_RES, "return %d\n", dh->opt.source );
  return dh->opt.source;
}

SANE_String_Const
a_get_mode( SANE_Handle d )
{
  DEVICE_H dh = d;

  DBG( D_CALL, "a_get_mode( d = %08X )\n", (unsigned int)d );

  DBG( D_RES, "return %s\n", opt_const_modes[ dh->opt.mode ] );
  return opt_const_modes[ dh->opt.mode ];
}

OPT_MODE_E
a_get_mode_e( SANE_Handle d )
{
  DEVICE_H dh = d;

  DBG( D_CALL, "a_get_mode_e( d = %08X )\n", (unsigned int)d );

  DBG( D_RES, "return %d\n", dh->opt.mode );
  return dh->opt.mode;
}

SANE_Int
a_get_depth( SANE_Handle d )
{
  DEVICE_H dh = d;
  SANE_Int depth;

  DBG( D_CALL, "a_get_depth( d = %08X )\n", (unsigned int)d );

  if ( dh->opt.mode == OPT_MODE_LINEART )
    {
      depth = 1;
    }
  else
    {
      depth = dh->opt.depth;
    }
  DBG( D_RES, "return %d\n", depth );
  return depth;
}

SANE_String_Const
a_get_gray_color( SANE_Handle d )
{
  DEVICE_H dh = d;

  DBG( D_CALL, "a_get_gray_color( d = %08X )\n", (unsigned int)d );

  DBG( D_RES, "return %s\n", opt_const_rgb_colors[dh->opt.gray_color] );
  return opt_const_rgb_colors[dh->opt.gray_color];
}

OPT_RGB_COLOR_E
a_get_gray_color_e( SANE_Handle d )
{
  DEVICE_H dh = d;

  DBG( D_CALL, "a_get_gray_color_e( d = %08X )\n", (unsigned int)d );

  DBG( D_RES, "return %d\n", dh->opt.gray_color );
  return dh->opt.gray_color;
}

SANE_Bool
a_get_preview( SANE_Handle d )
{
  DEVICE_H dh = d;

  DBG( D_CALL, "a_get_preview( d = %08X )\n", (unsigned int)d );

  DBG( D_RES, "return %d\n", dh->opt.preview );
  return dh->opt.preview;
}

SANE_Bool
a_get_rgb_bind( SANE_Handle d )
{
  DEVICE_H dh = d;

  DBG( D_CALL, "a_get_rgb_bind( d = %08X )\n", (unsigned int)d );

  DBG( D_RES, "return %d\n", dh->opt.rgb_bind );
  return dh->opt.rgb_bind;
}

SANE_Fixed
a_get_gamma( SANE_Handle d )
{
  DEVICE_H dh = d;

  DBG( D_CALL, "a_get_gamma( d = %08X )\n", (unsigned int)d );

  DBG( D_RES, "return %f\n", SANE_UNFIX( dh->opt.gamma ) );
  return dh->opt.gamma;
}

SANE_Fixed
a_get_gamma_r( SANE_Handle d )
{
  DEVICE_H dh = d;

  DBG( D_CALL, "a_get_gamma_r( d = %08X )\n", (unsigned int)d );

  DBG( D_RES, "return %f\n", SANE_UNFIX( dh->opt.gamma_r ) );
  return dh->opt.gamma_r;
}

SANE_Fixed
a_get_gamma_g( SANE_Handle d )
{
  DEVICE_H dh = d;

  DBG( D_CALL, "a_get_gamma_g( d = %08X )\n", (unsigned int)d );

  DBG( D_RES, "return %f\n", SANE_UNFIX( dh->opt.gamma_g ) );
  return dh->opt.gamma_g;
}

SANE_Fixed
a_get_gamma_b( SANE_Handle d )
{
  DEVICE_H dh = d;

  DBG( D_CALL, "a_get_gamma_b( d = %08X )\n", (unsigned int)d );

  DBG( D_RES, "return %f\n", SANE_UNFIX( dh->opt.gamma_b ) );
  return dh->opt.gamma_b;
}

SANE_Fixed
a_get_threshold( SANE_Handle d )
{
  DEVICE_H dh = d;

  DBG( D_CALL, "a_get_threshold( d = %08X )\n", (unsigned int)d );

  DBG( D_RES, "return %f\n", SANE_UNFIX( dh->opt.threshold ) );
  return dh->opt.threshold;
}

SANE_Fixed
a_get_tl_x( SANE_Handle d )
{
  DEVICE_H dh = d;
  SANE_Fixed x;

  DBG( D_CALL, "a_get_tl_x( d = %08X )\n", (unsigned int)d );

  if ( dh->opt.source == OPT_SOURCE_PAPER )
    {
      x = dh->opt.tl_x;
    }
  else
    {
      x = dh->opt.tl_x_ta;
    }
  DBG( D_RES, "return %f\n", SANE_UNFIX( x ) );
  return x;
}

SANE_Fixed
a_get_tl_y( SANE_Handle d )
{
  DEVICE_H dh = d;
  SANE_Fixed y;

  DBG( D_CALL, "a_get_tl_y( d = %08X )\n", (unsigned int)d );

  if ( dh->opt.source == OPT_SOURCE_PAPER )
    {
      y = dh->opt.tl_y;
    }
  else
    {
      y = dh->opt.tl_y_ta;
    }

  DBG( D_RES, "return %f\n", SANE_UNFIX( y ) );
  return y;
}

SANE_Fixed
a_get_br_x( SANE_Handle d )
{
  DEVICE_H dh = d;
  SANE_Fixed x;

  DBG( D_CALL, "a_get_br_x( d = %08X )\n", (unsigned int)d );

  if ( dh->opt.source == OPT_SOURCE_PAPER )
    {
      x = dh->opt.br_x;
    }
  else
    {
      x = dh->opt.br_x_ta;
    }

  DBG( D_RES, "return %f\n", SANE_UNFIX( x ) );
  return x;
}

SANE_Fixed
a_get_br_y( SANE_Handle d )
{
  DEVICE_H dh = d;
  SANE_Fixed y;

  DBG( D_CALL, "a_get_br_y( d = %08X )\n", (unsigned int)d );

  if ( dh->opt.source == OPT_SOURCE_PAPER )
    {
      y = dh->opt.br_y;
    }
  else
    {
      y = dh->opt.br_y_ta;
    }

  DBG( D_RES, "return %f\n", SANE_UNFIX( y ) );
  return y;
}

/* CCD start pixel is always even and width is also even */

SANE_Int
a_get_tl_x_pix( SANE_Handle d )
{
  return (SANE_Int)floor( ( SANE_UNFIX( a_get_tl_x( d ) ) *
                            a_get_resolution( d ) ) / 25.4 ) & ~1;
}

SANE_Int
a_get_tl_y_pix( SANE_Handle d )
{
  return (SANE_Int)floor( ( SANE_UNFIX( a_get_tl_y( d ) ) *
                            a_get_resolution( d ) ) / 25.4 );
}

SANE_Int
a_get_br_x_pix( SANE_Handle d )
{
  return (SANE_Int)ceil( ( ( SANE_UNFIX( a_get_br_x( d ) ) *
                           a_get_resolution( d ) ) / 25.4 ) + 1 ) & ~1;
}

SANE_Int
a_get_br_y_pix( SANE_Handle d )
{
  return (SANE_Int)ceil( ( SANE_UNFIX( a_get_br_y( d ) ) *
                           a_get_resolution( d ) ) / 25.4 );
}

SANE_Bool
a_get_calibrate_before_scan( SANE_Handle d )
{
  DEVICE_H dh = d;

  DBG( D_CALL, "a_get_calibrate_before_scan( d = %08X )\n", (unsigned int)d );

  DBG( D_RES, "return %d\n", dh->opt.calibrate_before_scan );
  return dh->opt.calibrate_before_scan;
}

/***********************************************************************
 * Setters
 ***********************************************************************/

SANE_Int a_set_resolution( SANE_Handle d, SANE_Int resolution )
{
  DEVICE_H dh = d;
  SANE_Int status;

  DBG( D_CALL, "a_set_resolution( d = %08X, resolution = %d )\n", 
       (unsigned int)d, resolution );

  status = a_correct_list_value( &resolution, opt_const_resolutions );
  if ( dh->opt.resolution != resolution )
    {
      dh->opt.resolution = resolution;
      status |= SANE_INFO_RELOAD_PARAMS;
    }

  return status;
}

SANE_Int a_set_source( SANE_Handle d, SANE_String_Const source )
{
  DEVICE_H dh = d;
  unsigned int ev;
  SANE_Int status;

  DBG( D_CALL, "a_set_source( d = %08X, source = %s )\n", 
       (unsigned int)d, source );

  status = a_correct_string_value( source, opt_const_sources, &ev );
  if ( dh->opt.source != ev )
    {
      dh->opt.source = ev;
      status |= SANE_INFO_RELOAD_PARAMS | SANE_INFO_RELOAD_OPTIONS;
    }

  return status;
}

SANE_Int a_set_mode( SANE_Handle d, SANE_String_Const mode )
{
  DEVICE_H dh = d;
  unsigned int ev;
  SANE_Int status;

  DBG( D_CALL, "a_set_mode( d = %08X, mode = %s )\n", 
       (unsigned int)d, mode );

  status = a_correct_string_value( mode, opt_const_modes, &ev );
  if ( dh->opt.mode != ev )
    {
      dh->opt.mode = ev;
      status |= SANE_INFO_RELOAD_PARAMS | SANE_INFO_RELOAD_OPTIONS;
    }

  return status;
}

SANE_Int a_set_depth( SANE_Handle d, SANE_Int depth )
{
  DEVICE_H dh = d;
  SANE_Int status;

  DBG( D_CALL, "a_set_depth( d = %08X, depth = %d )\n", 
       (unsigned int)d, depth );

  status = a_correct_list_value( &depth, opt_const_depths );
  if ( dh->opt.depth != depth )
    {
      dh->opt.depth = depth;
      status |= SANE_INFO_RELOAD_PARAMS;
    }

  return status;
}

SANE_Int a_set_gray_color( SANE_Handle d, SANE_String_Const gray_color )
{
  DEVICE_H dh = d;
  unsigned int ev;
  SANE_Int status;

  DBG( D_CALL, "a_set_gray_color( d = %08X, gray_color = %s )\n", 
       (unsigned int)d, gray_color );

  status = a_correct_string_value( gray_color, 
                                   opt_const_rgb_colors, 
                                   &ev );
  dh->opt.gray_color = ev;

  return status;
}

SANE_Int a_set_preview( SANE_Handle d, SANE_Bool preview )
{
  DEVICE_H dh = d;

  DBG( D_CALL, "a_set_preview( d = %08X, preview = %d )\n", 
       (unsigned int)d, preview );
  
  dh->opt.preview = preview;

  return 0;
}

SANE_Int a_set_rgb_bind( SANE_Handle d, SANE_Bool rgb_bind )
{
  DEVICE_H dh = d;
  SANE_Int status = 0;

  DBG( D_CALL, "a_set_rgb_bind( d = %08X, rgb_bind = %d )\n",
       (unsigned int)d, rgb_bind );

  if ( dh->opt.rgb_bind != rgb_bind )
    {
      dh->opt.rgb_bind = rgb_bind;
      status = SANE_INFO_RELOAD_OPTIONS;
    }

  return status;
}

SANE_Int a_set_gamma( SANE_Handle d, SANE_Fixed gamma )
{
  DEVICE_H dh = d;
  SANE_Int status;

  DBG( D_CALL, "a_set_gamma( d = %08X, gamma = %f )\n",
       (unsigned int)d, SANE_UNFIX(gamma) );

  status = a_correct_range_value( &gamma, &opt_gamma_range );
  dh->opt.gamma = gamma;
  
  return status;
}

SANE_Int a_set_gamma_r( SANE_Handle d, SANE_Fixed gamma_r )
{
  DEVICE_H dh = d;
  SANE_Int status;

  DBG( D_CALL, "a_set_gamma_r( d = %08X, gamma_r = %f )\n",
       (unsigned int)d, SANE_UNFIX(gamma_r) );

  status = a_correct_range_value( &gamma_r, &opt_gamma_range );
  dh->opt.gamma_r = gamma_r;
  
  return status;
}

SANE_Int a_set_gamma_g( SANE_Handle d, SANE_Fixed gamma_g )
{
  DEVICE_H dh = d;
  SANE_Int status;

  DBG( D_CALL, "a_set_gamma_g( d = %08X, gamma_g = %f )\n",
       (unsigned int)d, SANE_UNFIX(gamma_g) );

  status = a_correct_range_value( &gamma_g, &opt_gamma_range );
  dh->opt.gamma_g = gamma_g;
  
  return status;
}

SANE_Int a_set_gamma_b( SANE_Handle d, SANE_Fixed gamma_b )
{
  DEVICE_H dh = d;
  SANE_Int status;

  DBG( D_CALL, "a_set_gamma_b( d = %08X, gamma_b = %f )\n",
       (unsigned int)d, SANE_UNFIX(gamma_b) );

  status = a_correct_range_value( &gamma_b, &opt_gamma_range );
  dh->opt.gamma_b = gamma_b;
  
  return status;
}

SANE_Int a_set_threshold( SANE_Handle d, SANE_Fixed threshold )
{
  DEVICE_H dh = d;
  SANE_Int status;

  DBG( D_CALL, "a_set_threshold( d = %08X, threshold = %f )\n",
       (unsigned int)d, SANE_UNFIX(threshold) );

  status = a_correct_range_value( &threshold, &opt_threshold_range );
  dh->opt.threshold = threshold;
  
  return status;
}

SANE_Int a_set_tl_x( SANE_Handle d, SANE_Fixed tl_x )
{
  DEVICE_H dh = d;
  SANE_Int status;

  DBG( D_CALL, "a_set_tl_x( d = %08X, tl_x = %f )\n",
       (unsigned int)d, SANE_UNFIX(tl_x) );

  if ( dh->opt.source == OPT_SOURCE_PAPER )
    {
      status = a_correct_range_value( &tl_x, &opt_x_range );
      dh->opt.tl_x = tl_x;
    }
  else
    {
      status = a_correct_range_value( &tl_x, &opt_x_range_ta );
      dh->opt.tl_x_ta = tl_x;
    }

  return status;
}

SANE_Int a_set_tl_y( SANE_Handle d, SANE_Fixed tl_y )
{
  DEVICE_H dh = d;
  SANE_Int status;

  DBG( D_CALL, "a_set_tl_y( d = %08X, tl_y = %f )\n",
       (unsigned int)d, SANE_UNFIX(tl_y) );

  if ( dh->opt.source == OPT_SOURCE_PAPER )
    {
      status = a_correct_range_value( &tl_y, &opt_y_range );
      dh->opt.tl_y = tl_y;
    }
  else
    {
      status = a_correct_range_value( &tl_y, &opt_y_range_ta );
      dh->opt.tl_y_ta = tl_y;
    }

  return status;
}

SANE_Int a_set_br_x( SANE_Handle d, SANE_Fixed br_x )
{
  DEVICE_H dh = d;
  SANE_Int status;

  DBG( D_CALL, "a_set_br_x( d = %08X, br_x = %f )\n",
       (unsigned int)d, SANE_UNFIX(br_x) );

  if ( dh->opt.source == OPT_SOURCE_PAPER )
    {
      status = a_correct_range_value( &br_x, &opt_x_range );
      dh->opt.br_x = br_x;
    }
  else
    {
      status = a_correct_range_value( &br_x, &opt_x_range_ta );
      dh->opt.br_x_ta = br_x;
    }

  return status;
}

SANE_Int a_set_br_y( SANE_Handle d, SANE_Fixed br_y )
{
  DEVICE_H dh = d;
  SANE_Int status;

  DBG( D_CALL, "a_set_br_y( d = %08X, br_y = %f )\n",
       (unsigned int)d, SANE_UNFIX(br_y) );

  if ( dh->opt.source == OPT_SOURCE_PAPER )
    {
      status = a_correct_range_value( &br_y, &opt_y_range );
      dh->opt.br_y = br_y;
    }
  else
    {
      status = a_correct_range_value( &br_y, &opt_y_range_ta );
      dh->opt.br_y_ta = br_y;
    }

  return status;
}

SANE_Int a_set_calibrate_before_scan( SANE_Handle d, SANE_Bool cbs )
{
  DEVICE_H dh = d;

  DBG( D_CALL, "a_set_calibrate_before_scan( d = %08X, cbs = %d )\n", 
       (unsigned int)d, cbs );

  dh->opt.calibrate_before_scan = cbs;

  return 0;
}


/***********************************************************************
 * Scan parameters
 ***********************************************************************/

SANE_Frame a_format( SANE_Handle handle )
{
  if ( a_get_mode_e( handle ) == OPT_MODE_COLOR )
    {
      return SANE_FRAME_RGB;
    }
  else
    {
      return  SANE_FRAME_GRAY;
    }
}

SANE_Int a_depth( SANE_Handle handle )
{
  SANE_Int depth;

  if ( a_get_mode_e( handle ) == OPT_MODE_LINEART )
    {
      depth = 1;
    }
  else
    {
      depth = a_get_depth( handle );
    }

  return depth;
}

SANE_Int a_lines( SANE_Handle handle )
{
  SANE_Int lines =
    a_get_br_y_pix( handle ) - a_get_tl_y_pix( handle ) + 1;

  return lines;  
}

SANE_Int a_pixels_per_line( SANE_Handle handle )
{
  /* this is even because of how a_get..x_pix are implemented */
  return a_get_br_x_pix( handle ) - a_get_tl_x_pix( handle );
}

SANE_Int a_bytes_per_line( SANE_Handle handle )
{
  SANE_Int bytes_per_line = 0;

  switch( a_get_mode_e( handle ) )
    {
    case OPT_MODE_COLOR:
      bytes_per_line = 
        a_pixels_per_line(handle) * 3 *
        ( ( a_get_depth( handle ) == 16 ) ? 2 : 1 );
      break;
    case OPT_MODE_GRAY:
      bytes_per_line = 
        a_pixels_per_line(handle) *
        ( ( a_get_depth( handle ) == 16 ) ? 2 : 1 );
      break;
    case OPT_MODE_LINEART:
      bytes_per_line = ( a_pixels_per_line(handle) + 7 ) / 8;
      break;
    default:
      break;
    }

  return bytes_per_line;
}

void a_scan_init( SANE_Handle handle )
{
  DEVICE_H dh = handle;
  SANE_Int width;
  SANE_Int res;
  SANE_Int in_line_stagger;
  SANE_Int inter_line_stagger;

  DBG( D_CALL, "a_scan_init( handle = %08X )\n", 
       (unsigned int)handle );

  dh->scan.n_lines = a_lines( handle );
  dh->scan.current_line = 0;
  dh->scan.line_width = a_bytes_per_line( handle );
  dh->scan.line_pos = dh->scan.line_width;
  dh->scan.line = (SANE_Byte *)malloc( dh->scan.line_width );
  if ( ! dh->scan.line )
    {
      DBG( D_ERR, "Can't allocate line buffer\n" );
      return;
    }

  width = a_pixels_per_line( handle );
  res = a_get_resolution( handle );
  in_line_stagger = ( ( res == 1200 ) ? 4 : 0 );
  inter_line_stagger = ( 24 * res + 1200 - 1 ) / 1200;

  dh->scan.r = a_new_fifo( width, 1 + in_line_stagger, in_line_stagger );

  if ( a_get_mode_e( handle ) == OPT_MODE_COLOR )
    {
      dh->scan.g = a_new_fifo( width, 
                               1 + inter_line_stagger + in_line_stagger, 
                               in_line_stagger );
      dh->scan.b = a_new_fifo( width, 
                               1 + 2*inter_line_stagger + in_line_stagger, 
                               in_line_stagger );
    }

  if ( !dh->scan.r || !dh->scan.g || !dh->scan.b )
    {
      DBG( D_ERR, "Can't allocate line FIFO's\n" );
    }
}

void a_scan_finish( SANE_Handle handle )
{
  DEVICE_H dh = handle;

  if ( dh->scan.line )
    {
      free( dh->scan.line );
    }
  a_delete_fifo( dh->scan.r );
  a_delete_fifo( dh->scan.g );
  a_delete_fifo( dh->scan.b );

  memset( &(dh->scan), 0, sizeof(SCAN_T) );
}


/***********************************************************************
 * Line FIFO
 ***********************************************************************/

LINE_FIFO_T *a_new_fifo( int width, int length, int length2 )
{
  LINE_FIFO_T *fifo;

  fifo = (LINE_FIFO_T *)malloc( sizeof( LINE_FIFO_T ) );
  if ( ! fifo )
    {
      return 0;
    }
  fifo->data = (SANE_Byte *)malloc( width * length );
  if ( ! fifo->data )
    {
      free( fifo );
      return 0;
    }
  fifo->width = width;
  fifo->length = length;
  fifo->write_idx = 0;
  fifo->read_idx = 0;
  fifo->length2 = length2;

  return fifo;
}

void a_delete_fifo( LINE_FIFO_T *fifo )
{
  if ( fifo )
    {
      if ( fifo->data )
        {
          free( fifo->data );
        }
      free( fifo );
    }
}

int a_write_idx( LINE_FIFO_T *fifo )
{
  return fifo->write_idx % fifo->length;
}

int a_read_idx( LINE_FIFO_T *fifo )
{
  return fifo->read_idx % fifo->length;
}

int a_read_idx2( LINE_FIFO_T *fifo )
{
  return ( fifo->read_idx + fifo->length2 ) % fifo->length;
}

void a_write_idx_inc( LINE_FIFO_T *fifo )
{
  fifo->write_idx++;
  if ( fifo->write_idx == fifo->length * 2 )
    {
      fifo->write_idx = 0;
    }
}

void a_read_idx_inc( LINE_FIFO_T *fifo )
{
  fifo->read_idx++;
  if ( fifo->read_idx == fifo->length * 2 )
    {
      fifo->read_idx = 0;
    }
}

int a_space_in_fifo( LINE_FIFO_T *fifo )
{
  int n_data;

  n_data = fifo->write_idx - fifo->read_idx;
  if ( n_data < 0 ) 
    {
      n_data += 2 * fifo->length;
    }

  return fifo->length - n_data;
}

SANE_Byte *a_fifo_write_ptr( LINE_FIFO_T *fifo )
{
  return fifo->data + fifo->width * a_write_idx( fifo );
}

SANE_Byte *a_fifo_read_ptr( LINE_FIFO_T *fifo )
{
  return fifo->data + fifo->width * a_read_idx( fifo );
}

SANE_Byte *a_fifo_read_ptr2( LINE_FIFO_T *fifo )
{
  return fifo->data + fifo->width * a_read_idx2( fifo );
}


int a_fifo_width( LINE_FIFO_T *fifo )
{
  return fifo->width;
}

int a_write_to_fifo( LINE_FIFO_T *fifo, int in_pipe )
{
  int n_read;
  int n_left;
  SANE_Byte *wptr;

  if ( !a_space_in_fifo( fifo ) )
    {
      return 0;
    }

  wptr = a_fifo_write_ptr( fifo );
  n_left = a_fifo_width( fifo );
    
  while ( n_left )
    {
      DBG( D_INFO, ">>> n left %d\n", n_left );
      n_read = read( in_pipe, wptr, n_left );
      if ( n_read < 0 )
        {
          DBG( D_ERR, "Unexpected end of pipe when writing to FIFO\n" );
          return 0;
        }
      n_left -= n_read;
      wptr += n_read;
    }
    
  a_write_idx_inc( fifo );

  return 1;
}

void a_read_from_fifo_g( SCAN_T *scan )
{
  SANE_Byte *rptr;
  SANE_Byte *lptr;
  int i;
  int width = a_fifo_width( scan->r );

  rptr = a_fifo_read_ptr( scan->r );
  lptr = scan->line;

  for ( i = 0; i < width; i++ )
    {
      *lptr++ = *rptr++;
    }

  a_read_idx_inc( scan->r );
  scan->line_pos = 0;
  scan->current_line++;
}

void a_read_from_fifo_g_2( SCAN_T *scan )
{
  SANE_Byte *rptr;
  SANE_Byte *rptr2;
  SANE_Byte *lptr;
  int i;
  int width = a_fifo_width( scan->r );

  rptr = a_fifo_read_ptr( scan->r );
  rptr2 = a_fifo_read_ptr2( scan->r );
  lptr = scan->line;

  for ( i = 0; i < width; i++ )
    {
      *lptr++ = (i&1) ? *rptr : *rptr2;
      rptr++;
      rptr2++;
    }

  a_read_idx_inc( scan->r );
  scan->line_pos = 0;
  scan->current_line++;
}

void a_read_from_fifos_c( SCAN_T *scan )
{
  SANE_Byte *rptr_r;
  SANE_Byte *rptr_g;
  SANE_Byte *rptr_b;
  SANE_Byte *lptr;
  int i;
  int width = a_fifo_width( scan->r );

  rptr_r = a_fifo_read_ptr( scan->r );
  rptr_g = a_fifo_read_ptr( scan->g );
  rptr_b = a_fifo_read_ptr( scan->b );
  lptr = scan->line;

  for ( i = 0; i < width; i++ )
    {
      *lptr++ = *rptr_r++;
      *lptr++ = *rptr_g++;
      *lptr++ = *rptr_b++;
    }
  a_read_idx_inc( scan->r );
  a_read_idx_inc( scan->g );
  a_read_idx_inc( scan->b );
  scan->line_pos = 0;
  scan->current_line++;
}

void a_read_from_fifos_c_2( SCAN_T *scan )
{
  SANE_Byte *rptr_r;
  SANE_Byte *rptr_g;
  SANE_Byte *rptr_b;
  SANE_Byte *rptr2_r;
  SANE_Byte *rptr2_g;
  SANE_Byte *rptr2_b;
  SANE_Byte *lptr;
  int i;
  int width = a_fifo_width( scan->r );

  rptr_r = a_fifo_read_ptr( scan->r );
  rptr_g = a_fifo_read_ptr( scan->g );
  rptr_b = a_fifo_read_ptr( scan->b );
  rptr2_r = a_fifo_read_ptr2( scan->r );
  rptr2_g = a_fifo_read_ptr2( scan->g );
  rptr2_b = a_fifo_read_ptr2( scan->b );
  lptr = scan->line;

  for ( i = 0; i < width; i++ )
    {
      *lptr++ = (i&1) ? *rptr_r : *rptr2_r;
      *lptr++ = (i&1) ? *rptr_g : *rptr2_g;
      *lptr++ = (i&1) ? *rptr_b : *rptr2_b;
      rptr_r++; rptr2_r++;
      rptr_g++; rptr2_g++;
      rptr_b++; rptr2_b++;
    }
  a_read_idx_inc( scan->r );
  a_read_idx_inc( scan->g );
  a_read_idx_inc( scan->b );
  scan->line_pos = 0;
  scan->current_line++;
}

SANE_Int a_read_from_scan( SCAN_T *scan, SANE_Byte *data, SANE_Int max )
{
  SANE_Int n_read = 0;

  while ( scan->line_pos < scan->line_width
          && n_read < max )
    {
      data[n_read++] = scan->line[scan->line_pos++];
    }

  return n_read;
}

SANE_Bool a_lines_left( SCAN_T *scan )
{
  if ( scan->current_line < scan->n_lines )
    {
      return SANE_TRUE;
    }
  return SANE_FALSE;
}

SANE_Bool a_line_empty( SCAN_T *scan )
{
  if ( scan->line_pos < scan->line_width )
    {
      return SANE_FALSE;
    }
  return SANE_TRUE;
}

/***********************************************************************
 * Local stuff
 ***********************************************************************/

static void 
a_init_dev( DEVICE_H d )
{
  d->name = 0;
  d->dn = -1;
  d->open = SANE_FALSE;
  d->hs = SANE_FALSE;

  d->scanning = SANE_FALSE;
  d->scan_thread = -1;
  d->scan_pipe[0] = -1;
  d->scan_pipe[1] = -1;

  d->opt.resolution = 300;
  d->opt.source = OPT_SOURCE_PAPER;
  d->opt.mode = OPT_MODE_COLOR;
  d->opt.depth = 8;
  d->opt.gray_color = OPT_RGB_COLOR_RED;
  d->opt.preview = SANE_FALSE;
  d->opt.rgb_bind = SANE_TRUE;
  d->opt.gamma = SANE_FIX( 1.0 );
  d->opt.gamma_r = SANE_FIX( 1.0 );
  d->opt.gamma_g = SANE_FIX( 1.0 );
  d->opt.gamma_b = SANE_FIX( 1.0 );
  d->opt.threshold = SANE_FIX( 50.0 );
  d->opt.tl_x = SANE_FIX( 0.0 );
  d->opt.tl_y = SANE_FIX( 0.0 );
  d->opt.br_x = SANE_FIX( OPT_X_MAX );
  d->opt.br_y = SANE_FIX( OPT_Y_MAX );
  d->opt.tl_x_ta = SANE_FIX( 0.0 );
  d->opt.tl_y_ta = SANE_FIX( 0.0 );
  d->opt.br_x_ta = SANE_FIX( OPT_X_MAX_TA );
  d->opt.br_y_ta = SANE_FIX( OPT_Y_MAX_TA );
  d->opt.calibrate_before_scan = SANE_FALSE;

  a_init_cal( &(d->cal_300), 300 );
  a_init_cal( &(d->cal_600), 600 );
  a_init_cal( &(d->cal_1200), 1200 );
  a_init_cal( &(d->cal_300_ta), 300 );
  a_init_cal( &(d->cal_600_ta), 600 );
  a_init_cal( &(d->cal_1200_ta), 1200 );

  memset( &(d->scan), 0, sizeof(SCAN_T) );
}

static void
a_free_dev( DEVICE_H d )
{
  if ( d->name ) free( d->name );
  if ( d->cal_300.wshd ) free( d->cal_300.wshd );
  if ( d->cal_600.wshd ) free( d->cal_600.wshd );
  if ( d->cal_1200.wshd ) free( d->cal_1200.wshd );
  if ( d->cal_300_ta.wshd ) free( d->cal_300_ta.wshd );
  if ( d->cal_600_ta.wshd ) free( d->cal_600_ta.wshd );
  if ( d->cal_1200_ta.wshd ) free( d->cal_1200_ta.wshd );
  if ( d->cal_300.bshd ) free( d->cal_300.bshd );
  if ( d->cal_600.bshd ) free( d->cal_600.bshd );
  if ( d->cal_1200.bshd ) free( d->cal_1200.bshd );
  if ( d->cal_300_ta.bshd ) free( d->cal_300_ta.bshd );
  if ( d->cal_600_ta.bshd ) free( d->cal_600_ta.bshd );
  if ( d->cal_1200_ta.bshd ) free( d->cal_1200_ta.bshd );
  if ( d->scan.line ) free( d->scan.line );
  if ( d->scan.r ) free( d->scan.r );
  if ( d->scan.g ) free( d->scan.g );
  if ( d->scan.b ) free( d->scan.b );
}

static void 
a_init_cal( CAL_T *c, SANE_Int res )
{
  const AFE_SET_T init_set = {{0,0,0},{0x4b,0x4b,0x4b}};

  c->res = res;
  c->offs_x = 0;
  c->offs_y = 0;
  c->afe_set = init_set;
  c->width = 0;
  c->bshd = 0;
  c->wshd = 0;
}

static int 
a_valid_dev( DEVICE_H d )
{
  DEVICE_H dh = devices;

  while ( dh )
    {
      if ( dh == d )
        {
          return 1;
        }
      dh = dh->next;
    }

  return 0;
}

static SANE_Int 
a_correct_list_value( SANE_Word *value, const SANE_Word *list )
{
  int n_values = list[0];
  int i;
  int diff;
  int cand;

  for ( i = 1; i <= n_values; i++ )
    {
      if ( list[i] == *value )
        {
          return 0;
        }
    }

  diff = abs( list[1] - *value );
  cand = 1;
  for ( i = 2; i <= n_values; i++ )
    {
      if ( diff > abs( list[i] - *value ) )
        {
          diff = abs( list[i] - *value );
          cand = i;
        }
    }

  *value = list[cand];
  return SANE_INFO_INEXACT;
}

static int 
a_strcmp_ic( const char *a, const char *b )
{
  while ( *a && *b && tolower(*a) == tolower(*b) )
    {
      ++a;
      ++b;
    }
  return (int)(tolower(*a)-tolower(*b));
}

static SANE_Int 
a_correct_string_value( SANE_String_Const value, 
                        const SANE_String_Const *strings,
                        unsigned int *ev)
{
  unsigned int i;
  
  for ( i = 0; strings[i] && a_strcmp_ic( value, strings[i] ); i++ );

  if ( strings[i] )
    {
      *ev = i;
      return 0;
    }

  *ev = 0;
  return SANE_INFO_INEXACT;
}

static SANE_Int 
a_correct_range_value( SANE_Word *value, const SANE_Range *range )
{
  if ( *value < range->min )
    {
      *value = range->min;
      return SANE_INFO_INEXACT;
    }

  if ( *value > range->max )
    {
      *value = range->max;
      return SANE_INFO_INEXACT;
    }

  if ( range->quant && ( *value - range->min ) % range->quant )
    {
      *value -= ( *value - range->min ) % range->quant;
      return SANE_INFO_INEXACT;
    }

  return 0;
}

/************************************************************************
 * $Id: cs3200f-dat.c,v 1.1 2006/03/26 19:01:59 lapi-guest Exp $
 ************************************************************************/
