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

#include <string.h>

#include "cs3200f-opt.h"

/* temporarily for testing only */
#include "cs3200f-drv.h"

/************************************************************************
 * Option descriptors
 ************************************************************************/

static const SANE_Option_Descriptor o_od_num_opts =
  {
    /* name  = */  "",
    /* title = */  SANE_TITLE_NUM_OPTIONS,
    /* desc  = */  SANE_DESC_NUM_OPTIONS,
    /* type  = */  SANE_TYPE_INT,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_SOFT_DETECT,
    /* constraint_type  = */ SANE_CONSTRAINT_NONE, 
    /* constraint.range = */ { 0 } 
  };

static const SANE_Option_Descriptor o_od_resolution =
  {
    /* name  = */  SANE_NAME_SCAN_RESOLUTION,
    /* title = */  SANE_TITLE_SCAN_RESOLUTION,
    /* desc  = */  SANE_DESC_SCAN_RESOLUTION,
    /* type  = */  SANE_TYPE_INT,
    /* unit  = */  SANE_UNIT_DPI,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT,
    /* constraint_type  = */ SANE_CONSTRAINT_WORD_LIST, 
    /* constraint.word_list = */ { (const void *)opt_const_resolutions }
  };

static const SANE_Option_Descriptor o_od_source =
  {
    /* name  = */  "source",
    /* title = */  SANE_I18N( "Source" ),
    /* desc  = */  SANE_I18N( "Source for the scan." ),
    /* type  = */  SANE_TYPE_STRING,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  OPT_CONST_SOURCES_MAX_LEN,
    /* cap   = */  SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT,
    /* constraint_type  = */ SANE_CONSTRAINT_STRING_LIST, 
    /* constraint.string_list = */ { opt_const_sources }
  };

static const SANE_Option_Descriptor o_od_mode_group =
  {
    /* name  = */  "",
    /* title = */  SANE_I18N( "Mode settings" ),
    /* desc  = */  "",
    /* type  = */  SANE_TYPE_GROUP,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  0,
    /* cap   = */  0,
    /* constraint_type  = */ SANE_CONSTRAINT_NONE, 
    /* constraint.range = */ { 0 }
  };

static const SANE_Option_Descriptor o_od_mode =
  {
    /* name  = */  SANE_NAME_SCAN_MODE,
    /* title = */  SANE_TITLE_SCAN_MODE,
    /* desc  = */  SANE_DESC_SCAN_MODE,
    /* type  = */  SANE_TYPE_STRING,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  OPT_CONST_MODES_MAX_LEN,
    /* cap   = */  SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT,
    /* constraint_type  = */ SANE_CONSTRAINT_STRING_LIST, 
    /* constraint.range = */ { opt_const_modes }
  };

static const SANE_Option_Descriptor o_od_depth =
  {
    /* name  = */  SANE_NAME_BIT_DEPTH,
    /* title = */  SANE_TITLE_BIT_DEPTH,
    /* desc  = */  SANE_DESC_BIT_DEPTH,
    /* type  = */  SANE_TYPE_INT,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  sizeof( SANE_Word ),
    /* cap   = */  SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT,
    /* constraint_type  = */ SANE_CONSTRAINT_WORD_LIST, 
    /* constraint.range = */ { (const void *)opt_const_depths }
  };

static const SANE_Option_Descriptor o_od_depth_ia =
  {
    /* name  = */  SANE_NAME_BIT_DEPTH,
    /* title = */  SANE_TITLE_BIT_DEPTH,
    /* desc  = */  SANE_DESC_BIT_DEPTH,
    /* type  = */  SANE_TYPE_INT,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  sizeof( SANE_Word ),
    /* cap   = */  SANE_CAP_INACTIVE,
    /* constraint_type  = */ SANE_CONSTRAINT_WORD_LIST, 
    /* constraint.range = */ { (const void *)opt_const_depths }
  };

static const SANE_Option_Descriptor o_od_gray_color =
  {
    /* name  = */  SANE_I18N( "graycolor" ),
    /* title = */  SANE_I18N( "Gray color" ),
    /* desc  = */  SANE_I18N( "The color that will be used for the gray scale scanning." ),
    /* type  = */  SANE_TYPE_STRING,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  OPT_CONST_RGB_COLORS_MAX_LEN,
    /* cap   = */  SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT,
    /* constraint_type  = */ SANE_CONSTRAINT_STRING_LIST, 
    /* constraint.range = */ { opt_const_rgb_colors }
  };

static const SANE_Option_Descriptor o_od_gray_color_ia =
  {
    /* name  = */  SANE_I18N( "graycolor" ),
    /* title = */  SANE_I18N( "Gray color" ),
    /* desc  = */  SANE_I18N( "The color that will be used for the gray scale scanning." ),
    /* type  = */  SANE_TYPE_STRING,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  OPT_CONST_RGB_COLORS_MAX_LEN,
    /* cap   = */  SANE_CAP_INACTIVE,
    /* constraint_type  = */ SANE_CONSTRAINT_STRING_LIST, 
    /* constraint.range = */ { opt_const_rgb_colors }
  };

static const SANE_Option_Descriptor o_od_gamma_group =
  {
    /* name  = */  "",
    /* title = */  SANE_I18N( "Gamma settings") ,
    /* desc  = */  "",
    /* type  = */  SANE_TYPE_GROUP,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  0,
    /* cap   = */  0,
    /* constraint_type  = */ SANE_CONSTRAINT_NONE, 
    /* constraint.range = */ { 0 }
  };

static const SANE_Option_Descriptor o_od_preview =
  {
    /* name  = */  SANE_NAME_PREVIEW,
    /* title = */  SANE_TITLE_PREVIEW,
    /* desc  = */  SANE_DESC_PREVIEW,
    /* type  = */  SANE_TYPE_BOOL,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT,
    /* constraint_type  = */ SANE_CONSTRAINT_NONE, 
    /* constraint.range = */ { 0 }
  };

static const SANE_Option_Descriptor o_od_rgb_bind =
  {
    /* name  = */  SANE_NAME_RGB_BIND,
    /* title = */  SANE_TITLE_RGB_BIND,
    /* desc  = */  SANE_DESC_RGB_BIND,
    /* type  = */  SANE_TYPE_BOOL,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT,
    /* constraint_type  = */ SANE_CONSTRAINT_NONE, 
    /* constraint.range = */ { 0 }
  };

static const SANE_Option_Descriptor o_od_rgb_bind_ia =
  {
    /* name  = */  SANE_NAME_RGB_BIND,
    /* title = */  SANE_TITLE_RGB_BIND,
    /* desc  = */  SANE_DESC_RGB_BIND,
    /* type  = */  SANE_TYPE_BOOL,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_INACTIVE,
    /* constraint_type  = */ SANE_CONSTRAINT_NONE, 
    /* constraint.range = */ { 0 }
  };

static const SANE_Option_Descriptor o_od_gamma =
  {
    /* name  = */  "gamma",
    /* title = */  SANE_I18N( "Gamma" ),
    /* desc  = */  SANE_I18N( "Gamma correction for scanning." ),
    /* type  = */  SANE_TYPE_FIXED,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT,
    /* constraint_type  = */ SANE_CONSTRAINT_RANGE, 
    /* constraint.range = */ { (const void *)&opt_gamma_range }
  };

static const SANE_Option_Descriptor o_od_gamma_ia =
  {
    /* name  = */  "gamma",
    /* title = */  SANE_I18N( "Gamma" ),
    /* desc  = */  SANE_I18N( "Gamma correction for scanning." ),
    /* type  = */  SANE_TYPE_FIXED,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_INACTIVE,
    /* constraint_type  = */ SANE_CONSTRAINT_RANGE, 
    /* constraint.range = */ { (const void *)&opt_gamma_range }
  };

static const SANE_Option_Descriptor o_od_gamma_r =
  {
    /* name  = */  "gammar",
    /* title = */  SANE_I18N( "Red Gamma" ),
    /* desc  = */  SANE_I18N( "Gamma correction for scanning red." ),
    /* type  = */  SANE_TYPE_FIXED,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT,
    /* constraint_type  = */ SANE_CONSTRAINT_RANGE, 
    /* constraint.range = */ { (const void *)&opt_gamma_range }
  };

static const SANE_Option_Descriptor o_od_gamma_r_ia =
  {
    /* name  = */  "gammar",
    /* title = */  SANE_I18N( "Red Gamma" ),
    /* desc  = */  SANE_I18N( "Gamma correction for scanning red." ),
    /* type  = */  SANE_TYPE_FIXED,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_INACTIVE,
    /* constraint_type  = */ SANE_CONSTRAINT_RANGE, 
    /* constraint.range = */ { (const void *)&opt_gamma_range }
  };

static const SANE_Option_Descriptor o_od_gamma_g =
  {
    /* name  = */  "gammag",
    /* title = */  SANE_I18N( "Green Gamma" ),
    /* desc  = */  SANE_I18N( "Gamma correction for scanning green." ),
    /* type  = */  SANE_TYPE_FIXED,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT,
    /* constraint_type  = */ SANE_CONSTRAINT_RANGE, 
    /* constraint.range = */ { (const void *)&opt_gamma_range }
  };

static const SANE_Option_Descriptor o_od_gamma_g_ia =
  {
    /* name  = */  "gammag",
    /* title = */  SANE_I18N( "Green Gamma" ),
    /* desc  = */  SANE_I18N( "Gamma correction for scanning green." ),
    /* type  = */  SANE_TYPE_FIXED,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_INACTIVE,
    /* constraint_type  = */ SANE_CONSTRAINT_RANGE, 
    /* constraint.range = */ { (const void *)&opt_gamma_range }
  };

static const SANE_Option_Descriptor o_od_gamma_b =
  {
    /* name  = */  "gammab",
    /* title = */  SANE_I18N( "Blue Gamma" ),
    /* desc  = */  SANE_I18N( "Gamma correction for scanning blue." ),
    /* type  = */  SANE_TYPE_FIXED,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT,
    /* constraint_type  = */ SANE_CONSTRAINT_RANGE, 
    /* constraint.range = */ { (const void *)&opt_gamma_range }
  };

static const SANE_Option_Descriptor o_od_gamma_b_ia =
  {
    /* name  = */  "gammab",
    /* title = */  SANE_I18N( "Blue Gamma" ),
    /* desc  = */  SANE_I18N( "Gamma correction for scanning blue." ),
    /* type  = */  SANE_TYPE_FIXED,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_INACTIVE,
    /* constraint_type  = */ SANE_CONSTRAINT_RANGE, 
    /* constraint.range = */ { (const void *)&opt_gamma_range }
  };

static const SANE_Option_Descriptor o_od_threshold =
  {
    /* name  = */  SANE_NAME_THRESHOLD,
    /* title = */  SANE_TITLE_THRESHOLD,
    /* desc  = */  SANE_DESC_THRESHOLD,
    /* type  = */  SANE_TYPE_FIXED,
    /* unit  = */  SANE_UNIT_PERCENT,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT,
    /* constraint_type  = */ SANE_CONSTRAINT_RANGE, 
    /* constraint.range = */ { (const void *)&opt_threshold_range }
  };

static const SANE_Option_Descriptor o_od_threshold_ia =
  {
    /* name  = */  SANE_NAME_THRESHOLD,
    /* title = */  SANE_TITLE_THRESHOLD,
    /* desc  = */  SANE_DESC_THRESHOLD,
    /* type  = */  SANE_TYPE_FIXED,
    /* unit  = */  SANE_UNIT_PERCENT,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_INACTIVE,
    /* constraint_type  = */ SANE_CONSTRAINT_RANGE, 
    /* constraint.range = */ { (const void *)&opt_threshold_range }
  };

static const SANE_Option_Descriptor o_od_geometry_group =
  {
    /* name  = */  "",
    /* title = */  SANE_I18N( "Geometry" ),
    /* desc  = */  "",
    /* type  = */  SANE_TYPE_GROUP,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  0,
    /* cap   = */  0,
    /* constraint_type  = */ SANE_CONSTRAINT_NONE, 
    /* constraint.range = */ { 0 }
  };

static const SANE_Option_Descriptor o_od_tl_x =
  {
    /* name  = */  SANE_NAME_SCAN_TL_X,
    /* title = */  SANE_TITLE_SCAN_TL_X,
    /* desc  = */  SANE_DESC_SCAN_TL_X,
    /* type  = */  SANE_TYPE_FIXED,
    /* unit  = */  SANE_UNIT_MM,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT,
    /* constraint_type  = */ SANE_CONSTRAINT_RANGE, 
    /* constraint.range = */ { (const void *)&opt_x_range }
  };

static const SANE_Option_Descriptor o_od_tl_y =
  {
    /* name  = */  SANE_NAME_SCAN_TL_Y,
    /* title = */  SANE_TITLE_SCAN_TL_Y,
    /* desc  = */  SANE_DESC_SCAN_TL_Y,
    /* type  = */  SANE_TYPE_FIXED,
    /* unit  = */  SANE_UNIT_MM,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT,
    /* constraint_type  = */ SANE_CONSTRAINT_RANGE, 
    /* constraint.range = */ { (const void *)&opt_y_range }
  };

static const SANE_Option_Descriptor o_od_br_x =
  {
    /* name  = */  SANE_NAME_SCAN_BR_X,
    /* title = */  SANE_TITLE_SCAN_BR_X,
    /* desc  = */  SANE_DESC_SCAN_BR_X,
    /* type  = */  SANE_TYPE_FIXED,
    /* unit  = */  SANE_UNIT_MM,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT,
    /* constraint_type  = */ SANE_CONSTRAINT_RANGE, 
    /* constraint.range = */ { (const void *)&opt_x_range }
  };

static const SANE_Option_Descriptor o_od_br_x_ta =
  {
    /* name  = */  SANE_NAME_SCAN_BR_X,
    /* title = */  SANE_TITLE_SCAN_BR_X,
    /* desc  = */  SANE_DESC_SCAN_BR_X,
    /* type  = */  SANE_TYPE_FIXED,
    /* unit  = */  SANE_UNIT_MM,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT,
    /* constraint_type  = */ SANE_CONSTRAINT_RANGE, 
    /* constraint.range = */ { (const void *)&opt_x_range_ta }
  };

static const SANE_Option_Descriptor o_od_br_y =
  {
    /* name  = */  SANE_NAME_SCAN_BR_Y,
    /* title = */  SANE_TITLE_SCAN_BR_Y,
    /* desc  = */  SANE_DESC_SCAN_BR_Y,
    /* type  = */  SANE_TYPE_FIXED,
    /* unit  = */  SANE_UNIT_MM,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT,
    /* constraint_type  = */ SANE_CONSTRAINT_RANGE, 
    /* constraint.range = */ { (const void *)&opt_y_range }
  };

static const SANE_Option_Descriptor o_od_br_y_ta =
  {
    /* name  = */  SANE_NAME_SCAN_BR_Y,
    /* title = */  SANE_TITLE_SCAN_BR_Y,
    /* desc  = */  SANE_DESC_SCAN_BR_Y,
    /* type  = */  SANE_TYPE_FIXED,
    /* unit  = */  SANE_UNIT_MM,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT,
    /* constraint_type  = */ SANE_CONSTRAINT_RANGE, 
    /* constraint.range = */ { (const void *)&opt_y_range_ta }
  };

static const SANE_Option_Descriptor o_od_calibrate_group =
  {
    /* name  = */  "",
    /* title = */  SANE_I18N( "Calibration") ,
    /* desc  = */  "",
    /* type  = */  SANE_TYPE_GROUP,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  0,
    /* cap   = */  0,
    /* constraint_type  = */ SANE_CONSTRAINT_NONE, 
    /* constraint.range = */ { 0 }
  };

static const SANE_Option_Descriptor o_od_calibrate =
  {
    /* name  = */  "calibrate",
    /* title = */  SANE_I18N( "Calibrate now" ),
    /* desc  = */  SANE_I18N( "Calibrates the current source" ),
    /* type  = */  SANE_TYPE_BUTTON,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  0,
    /* cap   = */  SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT,
    /* constraint_type  = */ SANE_CONSTRAINT_NONE, 
    /* constraint.range = */ { 0 }
  };

static const SANE_Option_Descriptor o_od_calibrate_before_scan =
  {
    /* name  = */  "calibratebs",
    /* title = */  SANE_I18N( "Calibrate before every scan" ),
    /* desc  = */  SANE_I18N( "If this is selected the chosen mode and source will"
                              "be calibrated before each scan" ),
    /* type  = */  SANE_TYPE_BOOL,
    /* unit  = */  SANE_UNIT_NONE,
    /* size  = */  sizeof(SANE_Word),
    /* cap   = */  SANE_CAP_SOFT_DETECT | SANE_CAP_SOFT_SELECT,
    /* constraint_type  = */ SANE_CONSTRAINT_NONE, 
    /* constraint.range = */ { 0 }
  };

/************************************************************************
 * Option descriptor get
 ************************************************************************/

const SANE_Option_Descriptor *
o_opt_desc( SANE_Handle handle, 
            SANE_Int option )
{
  DEVICE_H dh = handle;

  switch ( option )
    {
    case OPT_NUM_OPTS:
      return &o_od_num_opts;
      break;
    case OPT_RESOLUTION:
      return &o_od_resolution;
      break;
    case OPT_SOURCE:
      return &o_od_source;
      break;
    case OPT_MODE_GROUP:
      return &o_od_mode_group;
      break;
    case OPT_MODE:
      return &o_od_mode;
      break;
    case OPT_DEPTH:
      if ( dh->opt.mode == OPT_MODE_LINEART )
        {
          return &o_od_depth_ia;
        }
      else
        {
          return &o_od_depth;
        }
      break;
    case OPT_GRAY_COLOR:
      if ( dh->opt.mode == OPT_MODE_COLOR )
        {
          return &o_od_gray_color_ia;
        }
      else
        {
          return &o_od_gray_color;
        }
      break;
    case OPT_PREVIEW:
      return &o_od_preview;
      break;
    case OPT_GAMMA_GROUP:
      return &o_od_gamma_group;
      break;
    case OPT_RGB_BIND:
      if ( dh->opt.mode == OPT_MODE_COLOR )
        {
          return &o_od_rgb_bind;
        }
      else
        {
          return &o_od_rgb_bind_ia;
        }
      break;
    case OPT_GAMMA:
      if ( dh->opt.mode == OPT_MODE_LINEART ||
           ( dh->opt.rgb_bind == SANE_FALSE &&
             dh->opt.mode == OPT_MODE_COLOR ) )
        {
          return &o_od_gamma_ia;
        }
      else
        {
          return &o_od_gamma;
        }
      break;
    case OPT_GAMMA_R:
      if ( ( dh->opt.rgb_bind == SANE_TRUE &&
           dh->opt.mode == OPT_MODE_COLOR ) ||
           dh->opt.mode != OPT_MODE_COLOR )
        {
          return &o_od_gamma_r_ia;
        }
      else
        {
          return &o_od_gamma_r;
        }
      break;
    case OPT_GAMMA_G:
      if ( ( dh->opt.rgb_bind == SANE_TRUE &&
           dh->opt.mode == OPT_MODE_COLOR ) ||
           dh->opt.mode != OPT_MODE_COLOR )
        {
          return &o_od_gamma_g_ia;
        }
      else
        {
          return &o_od_gamma_g;
        }
      break;
    case OPT_GAMMA_B:
      if ( ( dh->opt.rgb_bind == SANE_TRUE &&
           dh->opt.mode == OPT_MODE_COLOR ) ||
           dh->opt.mode != OPT_MODE_COLOR )
        {
          return &o_od_gamma_b_ia;
        }
      else
        {
          return &o_od_gamma_b;
        }
      break;
    case OPT_THRESHOLD:
      if ( dh->opt.mode == OPT_MODE_LINEART )
        {
          return &o_od_threshold;
        }
      else
        {
          return &o_od_threshold_ia;
        }
      break;
    case OPT_GEOMETRY_GROUP:
      return &o_od_geometry_group;
      break;
    case OPT_TL_X:
      return &o_od_tl_x;
      break;
    case OPT_TL_Y:
      return &o_od_tl_y;
      break;
    case OPT_BR_X:
      if ( dh->opt.source == OPT_SOURCE_PAPER )
        {
          return &o_od_br_x;
        }
      else
        {
          return &o_od_br_x_ta;
        }
      break;
    case OPT_BR_Y:
      if ( dh->opt.source == OPT_SOURCE_PAPER )
        {
          return &o_od_br_y;
        }
      else
        {
          return &o_od_br_y_ta;
        }
      break;
    case OPT_CALIBRATE_GROUP:
      return &o_od_calibrate_group;
      break;
    case OPT_CALIBRATE:
      return &o_od_calibrate;
      break;
    case OPT_CALIBRATE_BEFORE_SCAN:
      return &o_od_calibrate_before_scan;
      break;
    default:
      return 0;
      break;
    }
}

/************************************************************************
 * Option manipulation
 ************************************************************************/

SANE_Status
o_opt_get( SANE_Handle handle, 
           SANE_Int option,
           void *value )
{
  DBG( D_CALL, "o_opt_get( handle = %08X, option = %d, value = %08X )\n", 
       (unsigned int)handle, option, (unsigned int)value );

  if ( ( ( ! value ) && option != OPT_CALIBRATE ) ||
       ( a_handle_valid( handle ) != SANE_STATUS_GOOD ) )
    {
      return SANE_STATUS_INVAL;
    }

  switch ( option )
    {
    case OPT_NUM_OPTS:
      *(SANE_Int *)value = NUM_OPTIONS;
      DBG( D_RES, "return number of options: %d\n", NUM_OPTIONS );
      break;
    case OPT_RESOLUTION:
      *(SANE_Int *)value = a_get_resolution( handle );
      break;
    case OPT_SOURCE:
      strcpy( (SANE_String)value, a_get_source( handle ) );
      break;
    case OPT_MODE:
      strcpy( (SANE_String)value, a_get_mode( handle ) );
      break;
    case OPT_DEPTH:
      *(SANE_Int *)value = a_get_depth( handle );
      break;
    case OPT_GRAY_COLOR:
      strcpy( (SANE_String)value, a_get_gray_color( handle ) );
      break;
    case OPT_PREVIEW:
      *(SANE_Bool *)value = a_get_preview( handle );
      break;
    case OPT_RGB_BIND:
      *(SANE_Bool *)value = a_get_rgb_bind( handle );
      break;
    case OPT_GAMMA:
      *(SANE_Fixed *)value = a_get_gamma( handle );
      break;
    case OPT_GAMMA_R:
      *(SANE_Fixed *)value = a_get_gamma_r( handle );
      break;
    case OPT_GAMMA_G:
      *(SANE_Fixed *)value = a_get_gamma_g( handle );
      break;
    case OPT_GAMMA_B:
      *(SANE_Fixed *)value = a_get_gamma_b( handle );
      break;
    case OPT_THRESHOLD:
      *(SANE_Fixed *)value = a_get_threshold( handle );
      break;
    case OPT_TL_X:
      *(SANE_Fixed *)value = a_get_tl_x( handle );
      break;
    case OPT_TL_Y:
      *(SANE_Fixed *)value = a_get_tl_y( handle );
      break;
    case OPT_BR_X:
      *(SANE_Fixed *)value = a_get_br_x( handle );
      break;
    case OPT_BR_Y:
      *(SANE_Fixed *)value = a_get_br_y( handle );
      break;
    case OPT_CALIBRATE:
      break;
    case OPT_CALIBRATE_BEFORE_SCAN:
      *(SANE_Bool *)value = a_get_calibrate_before_scan( handle );
      break;
    default:
      return SANE_STATUS_INVAL;
      break;
    }

  return SANE_STATUS_GOOD;
}

SANE_Status
o_opt_set( SANE_Handle handle, 
           SANE_Int option,
           void *value,
           SANE_Int * info)
{
  SANE_Int no_info;

  DBG( D_CALL, "o_opt_set( handle = %08X, option = %d,"
       " value = %08X, info =%08X )\n", 
       (unsigned int)handle, option, 
       (unsigned int)value, (unsigned int)info );

  if ( ( ( ! value ) && option != OPT_CALIBRATE ) ||
       ( a_handle_valid( handle ) != SANE_STATUS_GOOD ) )
    {
      return SANE_STATUS_INVAL;
    }

  if ( !info )
    {
      info = &no_info;
    }

  switch ( option )
    {
    case OPT_RESOLUTION:
      *info = a_set_resolution( handle, *(SANE_Int *)value );
      break;
    case OPT_SOURCE:
      *info = a_set_source( handle, (SANE_String_Const)value );
      break;
    case OPT_MODE:
      *info = a_set_mode( handle, (SANE_String_Const)value );
      break;
    case OPT_DEPTH:
      *info = a_set_depth( handle, *(SANE_Int *)value );
      break;
    case OPT_GRAY_COLOR:
      *info = a_set_gray_color( handle, (SANE_String_Const)value );
      break;
    case OPT_PREVIEW:
      *info = a_set_preview( handle, *(SANE_Bool *)value );
      break;
    case OPT_RGB_BIND:
      *info = a_set_rgb_bind( handle, *(SANE_Bool *)value );
      break;
    case OPT_GAMMA:
      *info = a_set_gamma( handle, *(SANE_Fixed *)value );
      break;
    case OPT_GAMMA_R:
      *info = a_set_gamma_r( handle, *(SANE_Fixed *)value );
      break;
    case OPT_GAMMA_G:
      *info = a_set_gamma_g( handle, *(SANE_Fixed *)value );
      break;
    case OPT_GAMMA_B:
      *info = a_set_gamma_b( handle, *(SANE_Fixed *)value );
      break;
    case OPT_THRESHOLD:
      *info = a_set_threshold( handle, *(SANE_Fixed *)value );
      break;
    case OPT_TL_X:
      *info = a_set_tl_x( handle, *(SANE_Fixed *)value );
      break;
    case OPT_TL_Y:
      *info = a_set_tl_y( handle, *(SANE_Fixed *)value );
      if ( 0 )
        {
          SANE_Int line;
          DEVICE_H dh = handle;
          line = SANE_UNFIX( a_get_tl_y( handle ) ) * 2400. / 25.4;
          DBG( D_INFO, "Moving to line %d\n", line );
          if ( dh->dn >= 0 )
            {
              r_move( dh->dn, line );
            }
        }
      break;
    case OPT_BR_X:
      *info = a_set_br_x( handle, *(SANE_Fixed *)value );
      break;
    case OPT_BR_Y:
      *info = a_set_br_y( handle, *(SANE_Fixed *)value );
      break;
    case OPT_CALIBRATE:
      DBG( D_INFO, "Calibrating now!\n" );
      {
        DEVICE_H dh = handle;
        AFE_SET_T *afe_set = 0;

        r_lamp_on( dh->dn, LAMP_MAIN_HIGH );
        r_lamp_settle( dh->dn, SANE_FALSE, 120 );
        r_afe_calibrate( dh->dn, 600, SANE_FALSE, afe_set, 250, 2 );
        free( afe_set );
      }
      *info = 0;
      break;
    case OPT_CALIBRATE_BEFORE_SCAN:
      *info = a_set_calibrate_before_scan( handle, *(SANE_Bool *)value );
      break;
    default:
      return SANE_STATUS_INVAL;
      break;
    }

  DBG( D_RES, "info: %s, %s, %s\n",
       *info & SANE_INFO_INEXACT ? "inexact" : "exact",
       *info & SANE_INFO_RELOAD_OPTIONS ? "reload options" : "keep options",
       *info & SANE_INFO_RELOAD_PARAMS ? "reload params" : "keep params" );

  return SANE_STATUS_GOOD;
}

/************************************************************************
 * $Id: cs3200f-opt.c,v 1.1 2006/03/26 19:01:59 lapi-guest Exp $
 ************************************************************************/
