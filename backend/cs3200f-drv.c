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

#include "cs3200f-drv.h"
#include "cs3200f-usb.h"

#include "../include/md5.h"

#include "../include/sane/sanei.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

/***********************************************************************
 * Local types
 ***********************************************************************/

typedef struct
{
  unsigned char min;
  unsigned char max;
} LIMITS_T;

typedef struct
{
  LIMITS_T r;
  LIMITS_T g;
  LIMITS_T b;
} LEVELS_T;

/** Motor ramp type */
typedef enum
  {
    MOVE_RAMP,
    SCAN_RAMP,
    TRACE_BACK_RAMP
  } RAMP_E;

/***********************************************************************
 * Local constants and macros
 ***********************************************************************/

#define FW_SIZE 39492
#define FW_BUF_LEN ((FW_SIZE+511)&(~511))

static const unsigned char fw_md5_ref[16] =
  { 0xFF, 0x0C, 0xAE, 0x40, 0x3C, 0x64, 0x6B, 0x9A, 
    0xE0, 0x89, 0xDC, 0x26, 0x70, 0x37, 0x9D, 0x64 };

#define LOB( __x__ ) ((char)((__x__)&0xFF))
#define HIB( __x__ ) ((char)(((__x__)>>8)&0xFF))

/* The following are fixed HW settings. 
   Exact meaning is unknown. */
static const SANE_Byte dd_param_300[] = {
    0xc0, 0x1f, 0xf0, 0x01, 0x00, 0x60, 0x00, 0x00, 
    0x00, 0x0e, 0x00, 0x00, 0xc0, 0xff, 0xff, 0x3f, 
    0xc0, 0xcc, 0xcc, 0x0c, 0x00, 0x00, 0x3c, 0x00, 
    0x80, 0x87, 0x87, 0x07, 0xe0, 0x1d, 0x06, 0x04, 
    0x14, 0x1d, 0x06, 0x18, 0x1d, 0x06, 0x01, 0x18, 
    0xf0, 0x00, 0x26, 0x90, 0xc0, 0x1f, 0xf0, 0x01, 
    0x00, 0xe0, 0x0f, 0x3e, 0x10, 0x04, 0x40, 0x9a, 
    0x62, 0x07, 0x9e };
static const SANE_Byte dd_param_600[] = {
    0xc0, 0xff, 0x03, 0x00, 0x00, 0x80, 0x03, 0x00, 
    0x00, 0x78, 0x00, 0x00, 0xc0, 0xff, 0xff, 0x3f, 
    0x40, 0x66, 0x66, 0x26, 0x00, 0x00, 0xe0, 0x01, 
    0x00, 0x3c, 0x3c, 0x3c, 0xe0, 0x1d, 0x06, 0x04, 
    0x14, 0x1d, 0x06, 0x18, 0x1d, 0x06, 0x01, 0x18, 
    0xf0, 0x00, 0x06, 0x80, 0xc0, 0xff, 0x03, 0x00, 
    0x00, 0x00, 0xfc, 0x3f, 0x10, 0x00, 0x40, 0x9a, 
    0x62, 0x07, 0x9e };
static const SANE_Byte dd_param_1200[] = {
    0xc0, 0xff, 0xff, 0x3f, 0x00, 0x80, 0x03, 0x00, 
    0x00, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x40, 0x66, 0x66, 0x26, 0x00, 0x00, 0xe0, 0x01, 
    0x00, 0x3c, 0x3c, 0x3c, 0xe0, 0x1d, 0x06, 0x04, 
    0x14, 0x1d, 0x06, 0x18, 0x1d, 0x06, 0x01, 0x18, 
    0xf0, 0x40, 0x06, 0x80, 0xc0, 0xff, 0xff, 0x3f, 
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x40, 0x9a, 
    0x62, 0x07, 0x9e };

#define DD_PAR_LEN 59

/***********************************************************************
 * Local prototypes
 ***********************************************************************/

static SANE_Status r_upload( SANE_Int dn );
/** Generate and upload stepper motor speed ramp
 *  @param dn device number of the scanner
 *  @param r enumeration telling which ramp to generate
 *  @param len length of the ramp
 *  @param start start interval time of the ramp
 *  @param end end interval time of the ramp
 */
static void r_ramp_gnu( SANE_Int dn, 
                        RAMP_E r, 
                        int len, 
                        int start, 
                        int end );
static void r_mcurve( SANE_Int dn );
/** Set motor voltage level
 *  @param dn device number of the scanner
 *  @param level the motor voltage level (0-3)
 */
static void r_motor( SANE_Int dn, int level );
static void r_param_fill( int res, SANE_Byte *buf );
void r_params_set( SANE_Int dn, 
                   SANE_Int res, 
                   SANE_Int stride_len, 
                   SANE_Int stride1, 
                   SANE_Int stride2 );
static LEVELS_T *r_level_scan( SANE_Int dn, 
                               SANE_Int res, 
                               SANE_Bool ta,
                               LEVELS_T *levels );
static void r_scan_curves( SANE_Int dn, SANE_Int exp );

/***********************************************************************
 * The device interface
 ***********************************************************************/

SANE_Status
r_boot( SANE_Int dn )
{
  SANE_Status status = SANE_STATUS_GOOD;
  SANE_Byte b;
  SANE_Byte version[9];

  DBG( D_CALL, "r_boot( dn = %d )\n", dn );

  b = u_b_r( dn, 0xfe1f ) & 3;
  if ( b == 1 )
    {
      u_b_w( dn, 0xfe1f, b );
      u_b_w( dn, 0xfe1f, 3 );
      u_b_w( dn, 0xfe1f, 7 );
      u_b_w( dn, 0xfe1f, 6 );
    }

  u_b_w( dn, 0xffef, u_b_r( dn, 0xffef ) | 0x20 );
  u_b_w( dn, 0xffb4, u_b_r( dn, 0xffb4 ) | 0x20 );
  u_b_w( dn, 0xffed, u_b_r( dn, 0xffed ) & 0x03 );

  u_b_w( dn, 0xffe3, ( u_b_r( dn, 0xffe3 ) & 0xFD ) | 0x01 );

  if ( ! ( u_b_r( dn, 0xffe2 ) & 0x80 ) )
    {
      DBG( D_INFO, "Need to upload firmware\n" );

      status = r_upload( dn );
      if ( status != SANE_STATUS_GOOD )
        {
          DBG( D_ERR, "Could not upload firmware\n" );
          return status;
        }
    }
  else
    {
      DBG( D_INFO, "Firmware already uploaded\n" );
    }

  status = u_read( dn, 0x0080, 0, version, 8 );
  version[8] = 0;
  DBG( D_INFO, "Firmware version %s\n", version );

  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Could not get version\n" );
      return status;
    }

  if ( memcmp( version, "00000054", 8 ) )
    {
      DBG( D_ERR, "Firmware version does not match\n" );
      return SANE_STATUS_IO_ERROR;
    }

  DBG( D_INFO, "Boot succesful\n" );

  return status;

}

SANE_Status
r_wd_set( SANE_Int dn, SANE_Int c1, SANE_Int c2 )
{
  SANE_Status status;
  SANE_Byte d[5];

  DBG( D_CALL, "r_wd_set( dn = %d, c1 = %d, c2 = %d )\n", dn, c1, c2 );

  d[0] = ( c1 >> 8 ) & 0xff;
  d[1] = c1 & 0xff;
  d[2] = ( c2 >> 8 ) & 0xff;
  d[3] = c2 & 0xff;
  d[4] = 0;
  
  status = u_write( dn, 0x0050, 0, d, 5 );
    
  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Problem in setting lamp watchdog\n" );
    }
  
  return status;
}

SANE_Status
r_lamp_on( SANE_Int dn, LAMP_E lamp )
{
  SANE_Status status;
  SANE_Byte d[2] = { 0xff, 0 };

  DBG( D_CALL, "r_lamp_on( dn = %d, lamp = %d )\n", dn, lamp );

  r_wd_set( dn, 4000, 20000 );

  switch ( lamp )
    {
    case LAMP_MAIN_LOW:
      u_b_w( dn, 0xffb0, u_b_r( dn, 0xffb0 ) | 0x80 );
      u_b_w( dn, 0xffb1, u_b_r( dn, 0xffb1 ) & 0x7f );
      u_b_w( dn, 0xffbf, ( u_b_r( dn, 0xffbf ) & 0xf7 ) | 0x04 );
      u_b_w( dn, 0xffb0, u_b_r( dn, 0xffb0 ) | 0x0c );
      u_b_w( dn, 0xffb1, u_b_r( dn, 0xffb1 ) & 0xf3 );
      break;
    case LAMP_MAIN_HIGH:
      u_b_w( dn, 0xffbf, u_b_r( dn, 0xffbf ) & 0xf3 );
      u_b_w( dn, 0xffb0, u_b_r( dn, 0xffb0 ) | 0x8c );
      u_b_w( dn, 0xffb1, ( u_b_r( dn, 0xffb1 ) & 0x7f ) | 0x0c );
      break;
    case LAMP_TA:
      u_b_w( dn, 0xffbf, u_b_r( dn, 0xffbf ) & 0xf3 );
      u_b_w( dn, 0xffb0, u_b_r( dn, 0xffb0 ) | 0x8c );
      u_b_w( dn, 0xffb1, ( u_b_r( dn, 0xffb1 ) & 0xf3 ) | 0x80 );
      break;
    default:
      break;
    }

  status = u_write( dn, 0x002b, 1, d, 2 );

  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Problem in turning lamp on\n" );
    }
  
  return status;
}

void
r_lamp_off( SANE_Int dn )
{
  DBG( D_CALL, "r_lamp_off( dn = %d )\n", dn );

  u_b_w( dn, 0xffbf, u_b_r( dn, 0xffbf ) & 0xf3 );
  u_b_w( dn, 0xffb0, u_b_r( dn, 0xffb0 ) | 0x8c );
  u_b_w( dn, 0xffb1, u_b_r( dn, 0xffb1 ) & 0x73 );
}

void
r_afe_set( SANE_Int dn, AFE_SET_T *as )
{
  int status;
  SANE_Byte d[3] = { 0, 0, 0 };
  
  DBG( D_CALL, "r_afe_set( dn = %d,"
       " *as = { %02X, %02X, %02X, %02X, %02X, %02X } )\n", 
       dn, 
       as->offs.r & 0xff, 
       as->offs.g & 0xff, 
       as->offs.b & 0xff,
       as->gain.r & 0xff, 
       as->gain.g & 0xff, 
       as->gain.b & 0xff );

  d[1] = as->offs.r;
  status = u_write( dn, 0x00d0, 0x0020, d, 3 );
  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Problem in setting AFE offset red\n" );
    }
  d[1] = as->offs.g;
  status = u_write( dn, 0x00d0, 0x0021, d, 3 );
  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Problem in setting AFE offset green\n" );
    }
  d[1] = as->offs.b;
  status = u_write( dn, 0x00d0, 0x0022, d, 3 );
  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Problem in setting AFE offset blue\n" );
    }
  d[1] = as->gain.r;
  status = u_write( dn, 0x00d0, 0x0028, d, 3 );
  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Problem in setting AFE gain red\n" );
    }
  d[1] = as->gain.g;
  status = u_write( dn, 0x00d0, 0x0029, d, 3 );
  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Problem in setting AFE gain green\n" );
    }
  d[1] = as->gain.b;
  status = u_write( dn, 0x00d0, 0x002a, d, 3 );
  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Problem in setting AFE gain blue\n" );
    }
}

void 
r_home( SANE_Int dn )
{
  SANE_Status status;
  SANE_Byte home_command[] = { 0,0,2,0 }; 
  int line;
  int count = 0;

  DBG( D_CALL, "r_home( dn = %d )\n", dn );
  
  if ( u_home_q( dn ) ) return;
  
  r_mcurve( dn );
  
  r_motor( dn, 3 );

  status = u_write( dn, 0x0071, 0x0000, home_command, 4 );
  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Problem in giving home command\n" );
    }

  line = u_line_q( dn );
  
  while ( line != 0 ) {
    usleep( 50000 );
    line = u_line_q( dn );
    count++;
    if ( count > 200 )
      {
        DBG( D_ERR, "Move failed\n" );
        return;
      }
  }
  DBG( D_RES, "Home!\n" );
}

void 
r_move( SANE_Int dn, SANE_Int line )
{
  SANE_Status status;
  int cline;
  int dist;
  SANE_Byte cmd[5] = { 0,0,0,2,0 };
  int count = 0;
  
  DBG( D_CALL, "r_move( dn = %d, line = %d )\n", dn, line );

  cline = u_line_q( dn );
  if ( cline == line ) return;
  
  r_mcurve( dn );
  
  r_motor( dn, 3 );

  if ( cline > line ) {
    /* back */
    dist = cline - line;
    cmd[2] = (char)( dist & 0xFF );
    cmd[1] = (char)( ( dist >> 8 ) & 0xFF );
    status = u_write( dn, 0x0024, 0x0000, cmd, 5 );
  } else {
    /* fwd */
    dist = line - cline;
    cmd[2] = (char)( dist & 0xFF );
    cmd[1] = (char)( ( dist >> 8 ) & 0xFF ); 
    status = u_write( dn, 0x0023, 0x0000, cmd, 5 );
  }
  
  while ( cline != line ) {
    usleep( 50000 );
    cline = u_line_q( dn );
    DBG( D_INFO, "current line = %d\n", cline );
    count++;
    if ( count > 200 )
      {
        DBG( D_ERR, "Move failed\n" );
        return;
      }
  }

  DBG( D_RES, "At line %d\n", line );
}

SANE_Status 
r_lamp_settle( SANE_Int dn, SANE_Bool ta, SANE_Int nmax )
{
  AFE_SET_T afe_lamp = { { 0x60, 0x60, 0x60 }, { 0x4B, 0x4B ,0x4B } };
  LEVELS_T *levels1 = 0;
  LEVELS_T *levels2 = 0;
  LEVELS_T *tmp;
  int diff;
  SANE_Status status = SANE_STATUS_GOOD;
  int n = 0;
  
  DBG( D_CALL, "r_lamp_settle( dn = %d, ta = %d, nmax = %d )\n",
       dn, ta, nmax );
  
  r_afe_set( dn, &afe_lamp );
  
  levels1 = r_level_scan( dn, 600, ta, levels1 );
  if ( ! levels1 )
    {
      DBG( D_ERR, "Lamp settling scan 1 failed\n" );
      return SANE_STATUS_CANCELLED;
    }
  
  if ( (int)(levels1->r.max) - (int)(levels1->r.min) < 10
       && (int)(levels1->g.max) - (int)(levels1->g.min) < 10
       && (int)(levels1->b.max) - (int)(levels1->b.min) < 10 )
    {
      DBG( D_WARN, "Lamp may not be on!\n" );
    }

  for (;;) 
    {
      sleep( 5 );
      
      levels2 = r_level_scan( dn, 600, ta, levels2 );
      if ( !levels2 )
        {
          DBG( D_ERR, "Lamp settling scan 2 failed\n" );
          return SANE_STATUS_CANCELLED;
        }
      
      diff = (int)(levels2->r.min) - (int)(levels1->r.min);
      diff += (int)(levels2->r.max) - (int)(levels1->r.max);
      diff += (int)(levels2->g.min) - (int)(levels1->g.min);
      diff += (int)(levels2->g.max) - (int)(levels1->g.max);
      diff += (int)(levels2->b.min) - (int)(levels1->b.min);
      diff += (int)(levels2->b.max) - (int)(levels1->b.max);
      DBG( D_INFO, "Lamp settling n = %d, diff = %d\n", n, diff );
      
      if ( abs(diff) < 3 )
        {
          DBG( D_INFO, "Lamp settled\n" );
          break;
        }
      
      if ( ++n >= nmax ) 
        {
          DBG( D_ERR, "Lamp did not settle\n" );
          status = SANE_STATUS_CANCELLED;
          break;
	}
      
      tmp = levels2;
      levels2 = levels1;
      levels1 = tmp;
    }
  
  if ( levels1 ) 
    {
      free( levels1 );
    }
  if ( levels2 ) 
    {
      free( levels2 );
    }
  
  r_home( dn );
  return status;
}

AFE_SET_T *
r_afe_calibrate( SANE_Int dn, 
                 SANE_Int res, 
                 SANE_Bool ta,
                 AFE_SET_T *afe_set,
                 SANE_Int target_swing,
                 SANE_Int target_offset )
{
  LEVELS_T *levels = 0;
  float gr,gg,gb;

  DBG( D_CALL ,"r_afe_calibrate( dn = %d, res = %d, ta= %d,"
       " afe_set = %08X, target_swing = %d, target_offset = %d )\n",
       dn, res, ta, (unsigned int)afe_set, target_swing, target_offset );

  if ( !afe_set ) {
    afe_set = malloc( sizeof( AFE_SET_T ) );
    if ( !afe_set )
      {
        DBG( D_ERR, "Can't allocate memory for AFE settings\n" );
        return 0;
      }
    afe_set->offs.r = 0x00;
    afe_set->offs.g = 0x00;
    afe_set->offs.b = 0x00;
    afe_set->gain.r = 0x4b;
    afe_set->gain.g = 0x4b;
    afe_set->gain.b = 0x4b;
  }

  r_afe_set( dn, afe_set );

  levels = r_level_scan( dn, res, ta, levels );
    
  DBG( D_INFO, "Gain - Red: %3d..%3d   Green: %3d..%3d   Blue: %3d..%3d\n",
       levels->r.min, levels->r.max,
       levels->g.min, levels->g.max,
       levels->b.min, levels->b.max );

#define TG ((double)target_swing)
  gr = TG/(levels->r.max - levels->r.min);
  gg = TG/(levels->g.max - levels->g.min);
  gb = TG/(levels->b.max - levels->b.min);

  afe_set->gain.r = (char)(283. - 208./gr);
  afe_set->gain.g = (char)(283. - 208./gg);
  afe_set->gain.b = (char)(283. - 208./gb);

  gr = 208./(283. - (float)(unsigned char)afe_set->gain.r);
  gg = 208./(283. - (float)(unsigned char)afe_set->gain.g);
  gb = 208./(283. - (float)(unsigned char)afe_set->gain.b);

  DBG( D_INFO, "gain r: %f g: %f b: %f\n", gr, gg, gb );

  r_afe_set( dn, afe_set );

  levels = r_level_scan( dn, res, ta, levels );

  DBG( D_INFO, "Offs - Red: %3d..%3d   Green: %3d..%3d   Blue: %3d..%3d\n",
       levels->r.min, levels->r.max,
       levels->g.min, levels->g.max,
       levels->b.min, levels->b.max );

#define TO ((double)target_offset)
  afe_set->offs.r += (char)( (3./0.52) * ( levels->r.min - TO ) / gr );
  afe_set->offs.g += (char)( (3./0.52) * ( levels->g.min - TO ) / gg );
  afe_set->offs.b += (char)( (3./0.52) * ( levels->b.min - TO ) / gb );

  r_afe_set( dn, afe_set );

  levels = r_level_scan( dn, res, ta, levels );

  DBG( D_INFO, "Final - Red: %3d..%3d   Green: %3d..%3d   Blue: %3d..%3d\n",
       levels->r.min, levels->r.max,
       levels->g.min, levels->g.max,
       levels->b.min, levels->b.max );
  DBG( D_INFO, "Final AFE set gain %02X %02X %02X offs %02X %02X %02X\n",
       afe_set->gain.r & 0xff, 
       afe_set->gain.g & 0xff, 
       afe_set->gain.b & 0xff,
       afe_set->offs.r & 0xff, 
       afe_set->offs.g & 0xff, 
       afe_set->offs.b & 0xff );
  
  r_home( dn );

  if ( levels ) 
    {
      free ( levels );
    }

  return afe_set;
}

void r_offset( SANE_Int dn, SANE_Int col, SANE_Int row )
{
  SANE_Status status;
  SANE_Byte cmd[5] = { 0,0,0,0,0 };

  DBG( D_CALL ,"r_offset( dn = %d, col = %d, row= %d )\n",
       dn, col, row );

  cmd[0] = (SANE_Byte)(row&0xff);
  cmd[1] = (SANE_Byte)((row>>8)&0xff);
  cmd[2] = (SANE_Byte)(col&0xff);
  cmd[3] = (SANE_Byte)((col>>8)&0xff);

  status = u_write( dn, 0x0029, 0x0000, cmd, 5 );
  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Offset setting failed\n" );
    }
}

void 
r_reset( SANE_Int dn )
{
  SANE_Status status;
  SANE_Byte cmd70[] = { 0 };
  int n;

  DBG( D_CALL, "r_reset( dn = %d )\n", dn );

  status = u_write( dn, 0x0070, 0x0000, cmd70, 1 );

  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Scan reset failed\n" );
    }
  
  n = 0;
  while ( u_state_q( dn ) )
    {
      usleep( 50000 );
      DBG( D_INFO, "Waiting for state 0\n" );
      n++;
      if ( n > 10 )
        {
          DBG( D_INFO, "State did not reset\n" );
        }
    }
}

void r_gamma( SANE_Int dn, 
              SANE_Fixed t, 
              SANE_Fixed rg, 
              SANE_Fixed gg, 
              SANE_Fixed bg )
{
  SANE_Byte *gd;
  int i;
  double g,a,b,k,x,y;
  SANE_Status status;
  size_t len;

  DBG( D_CALL, "r_gamma( dn = %d, t = %d, rg = %d, gg = %d, bg = %d )\n",
       dn, t, rg, gg, bg );

  gd = malloc( 0x2000 );
  if ( !gd )
    {
      DBG( D_ERR, "Can't get memory for gamma upload\n" );
      return;
    }

  /* red */
  g = SANE_UNFIX(rg);
  a = 1./((g-1.)*pow(t,g)+1.);
  b = 1.-a;
  k = g*pow(t,g-1.)*a;

  for ( i = 0; i < 0x2000; i++ ) {
    x = (double)i/(double)0x2000;
    y = ( x < t ) ? ( k * x ) : ( a * pow( x, g )  + b );
    gd[i] = (SANE_Byte)(256.*y);
  }

  len = 0x2000;
  status = u_mem_write( dn, 0x010000, gd, &len );
  if ( len != 0x2000 || status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Gamma upload 0x010000 failed\n" );
      free( gd );
      return;
    }

  len = 0x2000;
  status = u_mem_write( dn, 0x110000, gd, &len );
  if ( len != 0x2000 || status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Gamma upload 0x110000 failed\n" );
      free( gd );
      return;
    }

  /* green */
  g = SANE_UNFIX(gg);
  a = 1./((g-1.)*pow(t,g)+1.);
  b = 1.-a;
  k = g*pow(t,g-1.)*a;

  for ( i = 0; i < 0x2000; i++ ) {
    x = (double)i/(double)0x2000;
    y = ( x < t ) ? ( k * x ) : ( a * pow( x, g )  + b );
    gd[i] = (SANE_Byte)(256.*y);
  }

  len = 0x2000;
  status = u_mem_write( dn, 0x012000, gd, &len );
  if ( len != 0x2000 || status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Gamma upload 0x012000 failed\n" );
      free( gd );
      return;
    }

  len = 0x2000;
  status = u_mem_write( dn, 0x112000, gd, &len );
  if ( len != 0x2000 || status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Gamma upload 0x112000 failed\n" );
      free( gd );
      return;
    }

  /* blue */
  g = SANE_UNFIX(bg);
  a = 1./((g-1.)*pow(t,g)+1.);
  b = 1.-a;
  k = g*pow(t,g-1.)*a;
  
  for ( i = 0; i < 0x2000; i++ ) {
    x = (double)i/(double)0x2000;
    y = ( x < t ) ? ( k * x ) : ( a * pow( x, g )  + b );
    gd[i] = (SANE_Byte)(256.*y);
  }

  len = 0x2000;
  status = u_mem_write( dn, 0x014000, gd, &len );
  if ( len != 0x2000 || status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Gamma upload 0x014000 failed\n" );
      free( gd );
      return;
    }

  len = 0x2000;
  status = u_mem_write( dn, 0x114000, gd, &len );
  if ( len != 0x2000 || status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Gamma upload 0x114000 failed\n" );
      free( gd );
      return;
    }

  free( gd );
}

SANE_Status
r_null_shading( SANE_Int dn )
{
  SANE_Int addr;
  SANE_Byte *shd;
  size_t len;
  SANE_Status status;

  DBG( D_CALL, "r_null_shading( dn = %d )\n", dn );

  shd = malloc( 0xf000 );
  if ( ! shd )
    {
      DBG( D_ERR, "Can't get memory for null shading\n" );
      return SANE_STATUS_NO_MEM;
    }
  memset( shd, 0, 0xf000 );
  len = 0xf000;
  status = u_mem_write( dn, 0x100000, shd, &len );
  if ( status != SANE_STATUS_GOOD || len != 0xf000 )
    {
      DBG( D_ERR, "Can't write null shading\n" );
      return SANE_STATUS_IO_ERROR;
    }

  free( shd );

  for ( addr = 0xff40; addr <= 0xff45; addr++ )
    {
      u_b_w( dn, addr, 0xff );
    }

  for ( addr = 0xff46; addr <= 0xff58; addr++ )
    {
      u_b_w( dn, addr, 0 );
    }
  
  u_b_w( dn, 0xff4f, 2 );

  return SANE_STATUS_GOOD;
}

SANE_Status
r_scan( SANE_Int dn, 
        SANE_Int res, 
        SANE_Int x,
        SANE_Int w,
        SANE_Int y,
        SANE_Int l,
        COLOR_E c )
{
  SANE_Status status;
  SANE_Int exp = 208;
  SANE_Int ccd_res = 300;
  SANE_Int btxl;
  SANE_Int col;
  SANE_Int row;
  SANE_Int wid;
  SANE_Int len;
  SANE_Byte motv = 0x20;
  SANE_Byte speed = 0x04;

  SANE_Byte cmd[29];

  DBG( D_CALL, "r_scan( dn = %d, res = %d,"
       " x = %d, w = %d, y = %d, l = %d, c = %d )\n",
       dn, res, x, w, y, l, c );

  len = l;

  switch( res )
    {
    case 75:
      exp = 69;
      motv = 0x30;
      speed = 0x00;
      /* len += (len+2)/3 + 4; when scanning in color res is 100 lpi */
      r_params_set( dn, 300, 24, 24, 1 );
      break;
    case 150:
      exp = 208; /* for usb1.1, 104 for usb2.0 */
      speed = 0x00;
      r_params_set( dn, 300, 16, 8, 1 );
      break;
    case 300:
      exp = 416;
      r_params_set( dn, 300, 16, 4, 1 );
      break;
    case 600:
      exp = 1666;
      ccd_res = 600;
      r_params_set( dn, 600, 16, 2, 2 );
      break;
    case 1200:
      exp = 1500;
      ccd_res = 1200;
      motv = 0x10;
      /* len += 8; for in-color staggering */
      r_params_set( dn, 1200, 36, 4, 9 );
      break;
    default:
      DBG( D_ERR, "Bad resolution\n" );
      return SANE_STATUS_INVAL;
      break;
    }

  /* len += 48*res/1200; */

  r_scan_curves( dn, exp );

  col = x*1200/res;
  row = y*1200/res;
  wid = w*1200/res;
  btxl = w/2;

  cmd[0] = LOB(exp);
  cmd[1] = HIB(exp);
  cmd[2] = LOB(res);
  cmd[3] = HIB(res);
  cmd[4] = c;
  cmd[5] = 0x40; /* 0x40, -> FFE4 */

  /* 6: Shading/Gamma control
   * Bit 7-4: <unused>
   * Bit 3: ???
   * Bit 2: Shading?
   * Bit 1: Shading?
   * Bit 0: Apply gamma?
   */
  cmd[6] = 0x01; /* ->FFE1[3:0] */

  /* 7: Byte 7 bit vector
   * Bit 7: 1 = new scan, 0 = continue scan
   * Bit 6: 1 = normal scan, 0 = calibration scan
   * Bit 5: 1 = use back tracking, 0 = no back tracking
   * Bit 4: 1 = (calibration) preview scan, 0 = normal scan
   * Bit 3: initial movement 0 = forwards, 1 = backwards
   * Bit 2: <unused>
   * Bit 1: 1 = apply line offset (cmd 29 param 0-1)
   * Bit 0: 1 = apply pixel offset (cmd 29 param 2-3)
   */
  cmd[7] = 0xe3; /* 1110 0011 */

  cmd[8] = LOB(btxl); /* bulk tx length */
  cmd[9] = HIB(btxl);
  cmd[10] = LOB(col); /* start column in 1200 dpi pixels */
  cmd[11] = HIB(col);
  cmd[12] = LOB(row); /* start line in 2400 lpi lines */
  cmd[13] = HIB(row);
  cmd[14] = LOB(wid); /* width in 1200 dpi pixels */
  cmd[15] = HIB(wid);
  cmd[16] = 0; /* unused */
  cmd[17] = 0; /* unused */

  /* 18: Speed bit vector: xxxx 00xx = 0x00 move at normal speed
   *                       xxxx x1xx = 0x04 move at half speed
   *                       xxxx 10xx = 0x08 move at double speed */
  cmd[18] = speed; /* speed bitvector */
  cmd[19] = 0; /* unused */
  cmd[20] = 0; /* unused */
  cmd[21] = 0; /* unused */
  cmd[22] = LOB(len); /* number of lines to output */
  cmd[23] = HIB(len);
  cmd[24] = LOB(ccd_res);
  cmd[25] = HIB(ccd_res);

  /* motor voltage during scan */
  cmd[26] = motv; /* ->FFB1[5:4] */
  cmd[27] = 0; /* unused */
  cmd[28] = 0; /* end */

  DBG( D_INFO, "Scan command\n" );
  status = u_write( dn, 0x0030, 0x0000, cmd, 29 );
  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Scan command failed\n" );
    }

  return status;
}

void r_find_top_left( SANE_Int dn, 
                      SANE_Int res, 
                      SANE_Int *top,
                      SANE_Int *left )
{
  DBG( D_CALL, "r_find_edges( dn = %d, res = %d,"
       " top = %08X, left = %08X )\n",
       dn, res, (unsigned int)top, (unsigned int)left );

  
}
        

/***********************************************************************
 * Local utilities
 ***********************************************************************/

static SANE_Status 
r_upload( SANE_Int dn )
{
  SANE_Status status = SANE_STATUS_GOOD;
  size_t len;
  SANE_Byte warm_up_timer;
  SANE_Byte start_cmd[3] = { 0, 0, 0 };
  SANE_Byte warm_up_cmd[3];

  FILE *fw_file;
  char *fw_buffer;
  char *fw_conf;
  unsigned char md5[16];

  DBG( D_CALL, "r_upload( dn = %d )\n", dn );

  fw_buffer = malloc( FW_BUF_LEN );

  if ( ! fw_buffer ) 
    {
      DBG( D_ERR, "Can't get memory for the firmware buffer\n" );
      return SANE_STATUS_NO_MEM;
    }

  memset( fw_buffer, 0, FW_BUF_LEN );

  DBG( D_INFO, "Opening " STRINGIFY( PATH_SANE_DATA_DIR ) 
          "/sane/cs3200f/cs3200f_v0054.fw\n" );

  fw_file = fopen( STRINGIFY( PATH_SANE_DATA_DIR ) 
                   "/sane/cs3200f/cs3200f_v0054.fw", "rb" );

  if ( ! fw_file ) 
    {
      DBG( D_ERR, "Can't open firmware file\n" );
      free( fw_buffer );
      return SANE_STATUS_IO_ERROR;
    }

  if ( FW_SIZE != 
       fread( fw_buffer, sizeof( char ), FW_SIZE, fw_file ) )
    {
      DBG( D_ERR, "Can't read whole firmware file\n" );
      fclose( fw_file );
      free( fw_buffer );
      return SANE_STATUS_IO_ERROR;
    }
  
  DBG( D_INFO, "Firmware file succesfully read in\n" );

  fclose( fw_file );

  md5_buffer( fw_buffer, FW_SIZE, md5 );

  DBG( D_INFO, "Firmware MD5: "
       "%02X%02X%02X%02X%02X%02X%02X%02X"
       "%02X%02X%02X%02X%02X%02X%02X%02X\n",
       md5[0], md5[1], md5[2], md5[3], md5[4], md5[5], md5[6], md5[7],
       md5[8], md5[9], md5[10], md5[11], md5[12], md5[13], md5[14], md5[15] );

  if ( memcmp( md5, fw_md5_ref, 16 ) )
    {
      DBG( D_ERR, "Firmware MD5 is not as expected\n" );
      free( fw_buffer );
      return SANE_STATUS_IO_ERROR;
    }

  DBG( D_INFO, "Uploading firmware\n" );

  len = FW_BUF_LEN;
  status = u_mem_write( dn, 0, (SANE_Byte *)fw_buffer, &len );

  if ( status != SANE_STATUS_GOOD || len != FW_BUF_LEN )
    {
      DBG( D_ERR, "Firmware upload failed\n" );
      free( fw_buffer );
      return SANE_STATUS_IO_ERROR;
    }

  fw_conf = malloc( FW_BUF_LEN );
  
  status = u_mem_read( dn, 0, (SANE_Byte *)fw_conf, &len );

  if ( status != SANE_STATUS_GOOD || len != FW_BUF_LEN )
    {
      DBG( D_ERR, "Firmware read-back failed\n" );
      free( fw_conf );
      free( fw_buffer );
      return SANE_STATUS_IO_ERROR;
    }

  if ( memcmp( fw_buffer, fw_conf, FW_BUF_LEN ) )
    {
      DBG( D_ERR, "Firmware read-back verify failed\n" );
      free( fw_conf );
      free( fw_buffer );
      return SANE_STATUS_IO_ERROR;
    }

  free( fw_conf );
  free( fw_buffer );

  DBG( D_INFO, "Firmware upload succesful, restarting scanner\n" );

  warm_up_timer = u_b_r( dn, 0xfdf6 );
  status = u_write( dn, 0x00b0, 0, start_cmd, 3 );

  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Start command failed\n" );
      return SANE_STATUS_IO_ERROR;
    }

  status = u_read( dn, 0x002c, 0, warm_up_cmd, 3 );
  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Warm-up read failed\n" );
      return SANE_STATUS_IO_ERROR;
    }

  warm_up_cmd[0] = 0x80;
  warm_up_timer += warm_up_timer/24;
  if ( warm_up_timer <= warm_up_cmd[1] )
    {
      warm_up_cmd[1] -= warm_up_timer;
    }
  warm_up_cmd[2] = 0;

  status = u_write( dn, 0x002b, 0, warm_up_cmd, 3 );
  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Warm-up write failed\n" );
      return SANE_STATUS_IO_ERROR;
    }

  DBG( D_INFO, "Scanner restart succesful\n" );

  return status;
}

static void 
r_ramp_gnu( SANE_Int dn, RAMP_E r, int len, int start, int end )
{
  SANE_Byte *cve;
  int i,j;
  int spd;
  int addr;
  size_t wlen;
  SANE_Status status;
  
  double a = (1./(double)start-1./(double)end)/(double)len/(double)len;
  double b = -2.*a*(double)len;
  double c = 1./(double)start;
  
  DBG( D_CALL, 
       "r_ramp_gnu( dn = %d, r = %s, len = %d, start = %d, end = %d )\n",
       dn,
       (r==MOVE_RAMP?"move":(r==SCAN_RAMP?"scan":"trace back")),
       len, start, end );

  DBG( D_INFO, "a = %f, b = %f, c = %f \n", a, b, c );

  switch ( r ) 
    {
    case MOVE_RAMP:
      addr = 0xDD00;
      break;
    case SCAN_RAMP:
      addr = 0xE500;
      break;
    case TRACE_BACK_RAMP:
      addr = 0xF100;
      break;
    default:
      DBG( D_ERR, "Illegal ramp type\n" );
      return;
      break;
    }
  
#define PRE 8
  len -= PRE;
  
  cve = malloc( 0x800 );
  if ( !cve )
    {
      DBG( D_ERR, "Out of memory in ramp generation\n" );
      return;
    }
  
  spd = start;
  for ( i = 0; i < PRE; i++ ) 
    {
      cve[i]       = HIB(spd);
      cve[0x400+i] = LOB(spd);
    }
  for ( j = 0; j < len; i++,j++ ) 
    {
      spd = 1./(c+j*(b+j*a));
      cve[i]       = HIB(spd);
      cve[0x400+i] = LOB(spd);
    }
  for ( /* i = i */; i < 0x400; i++ ) 
    {
      cve[i]       = HIB(spd);
      cve[0x400+i] = LOB(spd);
    }
  
  wlen = 0x0800;
  status = u_mem_write( dn, addr, (SANE_Byte *)cve, &wlen );
  
  if ( status != SANE_STATUS_GOOD
       || wlen != 0x800 )
    {
      DBG( D_ERR, "Ramp upload failed\n" );
    }
  
  free( cve );
}

static void 
r_mcurve( SANE_Int dn )
{
  SANE_Byte move_params[] = { 0x03, 0xff, 0x00 };
  SANE_Status status;
  
  DBG( D_CALL, "r_mcurve( dn = %d )\n", dn );

  r_ramp_gnu( dn, MOVE_RAMP, 0x0400, 2000, 50 );
  status = u_write( dn, 0x002b, 0x0010, move_params, 3 );
  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Can't set move params\n" );
    }
}

static void 
r_motor( SANE_Int dn, int level )
{
  DBG( D_CALL, "r_motor( dn = %d, level = %d )\n", dn, level );

  u_b_w( dn, 0xFFB0, u_b_r( dn, 0xFFB0 ) | 0x30 );
  u_b_w( dn, 0xFFB1, ( u_b_r( dn, 0xFFB1 ) & 0xCF ) 
                     | ( ( level & 3 ) << 4 ) );
}

static void 
r_param_fill( int res, SANE_Byte *buf )
{
    const SANE_Byte *param;

    if ( res <= 300 ) 
      { 
        param = dd_param_300; 
      }
    else if ( res <= 600 ) 
      { 
        param = dd_param_600; 
      }
    else 
      { 
        param = dd_param_1200; 
      }

    memcpy( buf, param, DD_PAR_LEN );
}

void 
r_params_set( SANE_Int dn, 
              SANE_Int res, 
              SANE_Int stride_len, 
              SANE_Int stride1, 
              SANE_Int stride2 )
{
    SANE_Byte *params;
    int i;
    SANE_Status status;
    size_t len = 512;

    DBG( D_CALL, "r_params_set( dn = %d, res = %d, stride_len = %d, "
         "stride1 = %d, stride2 = %d )\n",
         dn, res, stride_len, stride1, stride2 );

    params = malloc( len );
    if ( !params )
      {
        DBG( D_ERR, "Can't allocate memory for parameters\n" );
        return;
      }

    memset( params, 0, len );
    r_param_fill( res, params );
    
    params[ 0x01c0 ] = stride_len - 1;

    for ( i = 0; i < stride_len; i++ ) 
      {
	if ( i % stride1 == 0 ) 
          {
	    params[0x100+i] = 1;
          }
	if ( i % stride2 == 0 ) 
          {
	    params[0x180+i] = 1;
          }
      }

    status = u_mem_write( dn, 0xf900, params, &len );
    if ( status != SANE_STATUS_GOOD
         || len != 512 )
      {
        DBG( D_ERR, "Parameter upload failed\n" );
      }

    free( params );
}


static LEVELS_T *
r_level_scan( SANE_Int dn, 
              SANE_Int res,
              SANE_Bool ta,
              LEVELS_T *levels )
{
  SANE_Byte *scan;
  size_t size = 0;
  size_t br_size;
  unsigned char *pr, *pg, *pb;
  size_t i;
  SANE_Status status;
  
  SANE_Byte cmd31[] = { 
    0x00, 0x00, 0x00, 0x00, 0x03, 0x40, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00 };
  
  /* used for transparency adapter indication, not implemented yet */
  (void)ta;

  DBG( D_CALL, "r_level_scan( dn = %d, res = %d, levels = %08X )\n",
       dn, res, (unsigned int)levels );
  
  switch ( res )
    {
    case 300:
      size = 2560;
      cmd31[0] = LOB(416);
      cmd31[1] = HIB(416);
      cmd31[2] = LOB(300);
      cmd31[3] = HIB(300);
      cmd31[9] = 5;
      cmd31[24] = LOB(300);
      cmd31[25] = HIB(300);
      r_params_set( dn, 300, 16, 4, 1 );
      break;
    case 600:
      size = 5120;
      cmd31[0] = LOB(1666);
      cmd31[1] = HIB(1666);
      cmd31[2] = LOB(600);
      cmd31[3] = HIB(600);
      cmd31[9] = 10;
      cmd31[24] = LOB(600);
      cmd31[25] = HIB(600);
      r_params_set( dn, 600, 16, 2, 2 );
      break;
    case 1200:
      size = 10240;
      cmd31[0] = LOB(1500);
      cmd31[1] = HIB(1500);
      cmd31[2] = LOB(1200);
      cmd31[3] = HIB(1200);
      cmd31[9] = 20;
      cmd31[24] = LOB(1200);
      cmd31[25] = HIB(1200);
      r_params_set( dn, 1200, 36, 4, 9 );
      break;
    default:
      DBG( D_ERR, "Bad resolution\n" );
      return 0;
      break;
    }
  
  r_move( dn, 420 );
  
  scan = malloc( size * 3 );
  if ( !scan )
    {
      DBG( D_ERR, "Can't allocate memory for line scan\n" );
      return 0; 
    }
  
  status = u_write( dn, 0x0031, 0x0000, cmd31, 29 );
  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Line scan command failed\n" );
      free( scan );
      return 0;
    }
  
  br_size = size * 3;
  status = u_bulk_read( dn, scan, &br_size );
  if ( status != SANE_STATUS_GOOD
       || br_size != size * 3 )
    {
      DBG( D_ERR, "Line scan bulk read failed\n" );
      free( scan );
      return 0;
    }
  
  r_reset( dn );
    
  pr = (unsigned char *)scan;
  pg = pr + size;
  pb = pg + size;
  
  if ( ! levels )
    {
      levels = malloc( sizeof( LEVELS_T ) );
    }
  if ( ! levels ) 
    {
      DBG( D_ERR, "Can't allocate memory for levels\n" );
      free( scan );
      return 0;
    }
  
  levels->r.min = 255;
  levels->g.min = 255;
  levels->b.min = 255;
  levels->r.max = 0;
  levels->g.max = 0;
  levels->b.max = 0;
  
  for ( i = 0; i < size; ++i ) {
    if ( *pr > levels->r.max ) levels->r.max = *pr;
    if ( *pg > levels->g.max ) levels->g.max = *pg;
    if ( *pb > levels->b.max ) levels->b.max = *pb;
    if ( *pr < levels->r.min ) levels->r.min = *pr;
    if ( *pg < levels->g.min ) levels->g.min = *pg;
    if ( *pb < levels->b.min ) levels->b.min = *pb;
    ++pr; ++pg; ++pb;
  }
  
  free( scan );
  
  return levels;
}

static void r_scan_curves( SANE_Int dn, SANE_Int exp )
{
  /* was 128/128/96 */
#define L1 256
#define L2 256
  /* was 128 */
#define L3 512

  SANE_Status status;
  SANE_Byte cmd[] = { HIB(L1-1), LOB(L1-1),
                      HIB(L2-1), LOB(L2-1),
                      HIB(L3-1), LOB(L3-1), 
                      HIB(L3-1), LOB(L3-1), 
                      0x03, 0xff, 
                      0x00 };

  DBG( D_CALL, "r_scan_curves( dn = %d, exp = %d )\n",
       dn, exp );

  r_ramp_gnu( dn, MOVE_RAMP, L1, 925, exp );
  r_ramp_gnu( dn, SCAN_RAMP, L2, exp, exp );
  r_ramp_gnu( dn, TRACE_BACK_RAMP, L3, 600, exp );

  status = u_write( dn, 0x002b, 0x0008, cmd, 11 );
  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Scan curve limits setting failed\n" );
    }
}

/************************************************************************
 * $Id: cs3200f-drv.c,v 1.2 2006/03/31 18:55:51 lapi-guest Exp $
 ************************************************************************/
