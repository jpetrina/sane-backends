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

#include "../include/sane/sanei_thread.h"
#include <unistd.h>

#include "cs3200f-scan.h"
#include "cs3200f-dat.h"
#include "cs3200f-drv.h"

/***********************************************************************
 * Local types
 ***********************************************************************/

typedef struct RAW_DATA
{
  struct RAW_DATA *next;
  SANE_Int n_lines;
  SANE_Byte data[1];
} RAW_DATA_T;

/***********************************************************************
 * Local prototypes
 ***********************************************************************/

static int s_scan_thread( void *handle );
void s_unstagger( SANE_Handle handle,
                  SANE_Byte * data,
                  SANE_Int max_length,
                  SANE_Int * length );

/***********************************************************************
 * Interface
 ***********************************************************************/

SANE_Status s_get_params( SANE_Handle handle,
                          SANE_Parameters * params )
{
  DBG( D_CALL, "s_get_params( handle = %08X, params = %08X)\n",
       (unsigned int)handle, (unsigned int)params );

  if ( ( ! params ) ||
       ( a_handle_valid( handle ) != SANE_STATUS_GOOD ) )
    {
      DBG( D_ERR, "Bad parameters\n" );
      return SANE_STATUS_INVAL;
    }

  params->format = a_format( handle );
  DBG( D_RES, "Format: %s\n", 
       (params->format == SANE_FRAME_RGB ) ? "RGB": "Gray" );

  params->last_frame = SANE_TRUE;
  DBG( D_RES, "Last frame: true\n" );

  params->depth = a_depth( handle );
  DBG( D_RES, "Depth: %d\n", params->depth );

  params->lines = a_lines( handle );
  DBG( D_RES, "Lines: %d\n", params->lines );


  params->pixels_per_line = a_pixels_per_line( handle );
  DBG( D_RES, "Pixels per line: %d\n", params->pixels_per_line );


  params->bytes_per_line = a_bytes_per_line( handle );
  DBG( D_RES, "Bytes per line: %d\n", params->bytes_per_line );
  
  return SANE_STATUS_GOOD;
}

SANE_Status s_scan_start( SANE_Handle handle )
{
  DEVICE_H dh = handle;

  DBG( D_CALL, "s_scan_start( handle = %08X )\n",
       (unsigned int)handle );

  a_scan_finish( handle );
  a_scan_init(handle );

  if ( pipe( dh->scan_pipe ) == -1 )
    {
      DBG( D_ERR, "Pipe creation failed\n" );
      return SANE_STATUS_IO_ERROR;
    }

  dh->scan_thread = sanei_thread_begin( s_scan_thread, dh );

  if ( sanei_thread_is_forked() )
    {
      close( dh->scan_pipe[1] );
    }

  if ( dh->scan_thread == -1 )
    {
      DBG( D_ERR, "Scan thread spawning failed\n" );
      return SANE_STATUS_IO_ERROR;
    }
  else
    {
      DBG( D_INFO, "Scan thread %d spawned\n", dh->scan_thread );
    }

  dh->scan_bytes_left = a_lines( handle ) * a_bytes_per_line( handle );

  if ( a_get_mode_e( handle ) == OPT_MODE_COLOR 
       || a_get_resolution( handle ) == 1200 )
    {
      a_scan_init( handle );
    }

  return SANE_STATUS_GOOD;
}

SANE_Status
s_scan_read( SANE_Handle handle,
             SANE_Byte * data,
             SANE_Int max_length,
             SANE_Int * length )
{
  DEVICE_H dh = handle;

  DBG( D_CALL, "s_scan_read( handle = %08X, data = %08X, "
       "max_length = %d, length = %08X )\n",
       (unsigned int)handle, (unsigned int)data,
       max_length, (unsigned int)length );

  if ( dh->dn < 0 )
    {
      *length = read( dh->scan_pipe[0], data, max_length );
    }
  else
    {
      s_unstagger( handle, data, max_length, length );
    }

  if ( *length <= 0 )
    {
      *length = 0;
      DBG( D_INFO, "Read finished\n" );
      DBG( D_INFO, "Killing scan thread %d\n", dh->scan_thread );
      /* TODO: how to handle rbuf in scan thread? */
      sanei_thread_kill( dh->scan_thread );
      /* reset scanner */
      r_reset( dh->dn );
      r_home( dh->dn );
      a_scan_finish( handle );
      return SANE_STATUS_EOF;
    }

  dh->scan_bytes_left -= *length;

  DBG( D_INFO, "Read %d bytes %d left\n", *length, dh->scan_bytes_left );

  return SANE_STATUS_GOOD;
}

void
s_scan_cancel( SANE_Handle handle )
{
  DEVICE_H dh = handle;
  int info;

  DBG( D_CALL, "s_scan_cancel( handle = %08X )\n",
       (unsigned int)handle );

  a_scan_finish( handle );

  if ( dh->scan_thread != -1 )
    {
#if 1
      DBG( D_INFO, "Waiting scan thread %d\n", dh->scan_thread );
      sanei_thread_waitpid( dh->scan_thread, &info );
#else
      DBG( D_INFO, "Killing scan thread %d\n", dh->scan_thread );
      sanei_thread_kill( dh->scan_thread );
#endif
    }

  dh->scan_thread = -1;
  close( dh->scan_pipe[0] );
}

/***********************************************************************
 * Local code
 ***********************************************************************/

static int s_scan_thread( void *handle )
{
  DEVICE_H dh = handle;
  int i;

  DBG( D_INFO, "Scan thread starting\n" );
      
  i = 0;

  if ( sanei_thread_is_forked() )
    {
      close( dh->scan_pipe[0] );
    }
      
  if ( dh->dn < 0 ) /* dummy scanner */
    {
      int n_bytes = 0;

      char buf1[3] = { '\377', '\000', '\000' };
      char buf2[3] = { '\000', '\000', '\377' };
      
      n_bytes = a_lines( handle ) * a_bytes_per_line( handle );

      while ( n_bytes > 0 )
        {
          if ( i == 64 )
            { 
              n_bytes -= write( dh->scan_pipe[1], buf1, 3 );
              i = 0;
              DBG( D_INFO, "bytes left in scan %d\n", n_bytes );
            }
          else
            {
              n_bytes -= write( dh->scan_pipe[1], buf2, 3 );
              i++;
            }
          /* usleep(1); */
        }
    }
  else /* real scanner */
    {
      AFE_SET_T *as;
      SANE_Int ccd_res;
      COLOR_E c;
      SANE_Int m;
      SANE_Fixed rg,gg,bg;
      SANE_Status status;
      SANE_Byte *rbuf;
      size_t n_bytes;
      size_t n_bytes_r;
      size_t len;
      SANE_Int res;
      SANE_Int n_lines;
      SANE_Int width;
      
      /* turn lamp on */
      r_lamp_on( dh->dn, LAMP_MAIN_HIGH );
      r_lamp_settle( dh->dn, SANE_FALSE, 30 );

      /* calibrate if not done or if every scan */
      ccd_res = a_get_resolution( handle );
      if ( ccd_res < 300 )
        {
          ccd_res = 300;
        }

      as = r_afe_calibrate( dh->dn, 
                            ccd_res,
                            SANE_FALSE,
                            0,
                            250,
                            2 );

      /* set AFE */
      r_afe_set( dh->dn, as );

      /* set gamma */
      if ( a_get_rgb_bind( handle ) )
        {
          rg = gg = bg = a_get_gamma( handle );
        }
      else
        {
          rg = a_get_gamma_r( handle );
          gg = a_get_gamma_g( handle );
          bg = a_get_gamma_b( handle );
        }
      r_gamma( dh->dn, SANE_FIX( 0.05 ),
               rg,gg,bg );

      /* set shading */
      r_null_shading( dh->dn );

      /* set offset */
      {
        SANE_Int offs_x = 0;
        SANE_Int offs_y = 0;

        switch( ccd_res )
          {
          case 300:
            offs_x = dh->cal_300.offs_x;
            offs_y = dh->cal_300.offs_y;
            break;
          case 600:
            offs_x = dh->cal_600.offs_x;
            offs_y = dh->cal_600.offs_y;
            break;
          case 1200:
            offs_x = dh->cal_1200.offs_x;
            offs_y = dh->cal_1200.offs_y;
            break;
          default:
            break;
          }
        r_offset( dh->dn, offs_x, offs_y );
      }

      /* start scan */

      res = a_get_resolution( handle );
      n_lines = a_lines( handle );

      if ( a_get_resolution( handle ) == 75 )
        {
          n_lines += ( n_lines + 2 ) / 3 + 1;
        }

      if ( a_get_mode_e( handle ) == OPT_MODE_COLOR )
        {
          c = 3;
          m = 3;
          n_lines += 48*res/1200;
          if ( a_get_resolution( handle ) == 75 )
            {
              n_lines += ( n_lines + 2 ) / 3 + 1;
            }
        }
      else
        {
          c = a_get_gray_color_e( handle );
          m = 1;
        }

      if ( a_get_resolution( handle ) == 1200 )
        {
          n_lines += 4;
        }

      width = a_pixels_per_line( handle );

      status = r_scan( dh->dn,
                       res,
                       a_get_tl_x_pix( handle ),
                       width,
                       a_get_tl_y_pix( handle ),
                       n_lines,
                       c
                       );

      if ( status != SANE_STATUS_GOOD )
        {
          DBG( D_ERR, "Can't start scan\n" );
          close( dh->scan_pipe[1] );
          return 0;
        }

      rbuf = malloc( 0x10000 );
      /* in loop read all bytes */
      n_bytes_r = n_lines * width * m;
      while ( n_bytes_r > 0 )
        {
          n_bytes = n_bytes_r;
          if ( n_bytes > 0x10000 )
            {
              n_bytes = 0x10000;
            }
          
          len = ( n_bytes + 511 ) & ~511;
          DBG( D_INFO, ">>Bytes left %d now reading %d bytes\n", 
               n_bytes_r, n_bytes );

          status = u_bulk_read( dh->dn, rbuf, &len );

          n_bytes = len;
          if ( status != SANE_STATUS_GOOD || len != n_bytes )
            {
              DBG( D_ERR, "Can't read scan data (status %d len %d of %d)\n",
                   status, len, n_bytes );
              break;
            }
          
          write( dh->scan_pipe[1], rbuf, n_bytes );

          if ( n_bytes > n_bytes_r )
            {
              n_bytes_r = 0;
            }
          else
            {
              n_bytes_r -= n_bytes;
            }
        }
      
      /* reset scanner */
      r_reset( dh->dn );
      r_home( dh->dn );

      free( rbuf );
    }

  close( dh->scan_pipe[1] );
  DBG( D_INFO, "Scan thread exiting\n" );
  return 0;
}

/************************************************************************
 * Unstaggerer
 ************************************************************************/

void unstagger_gray( int width, int length );
void unstagger_color( int width, int length, int res );
void unstagger_color_2( int width, int length );

void s_unstagger( SANE_Handle handle,
                  SANE_Byte * data,
                  SANE_Int max_length,
                  SANE_Int * length )
{
  int res;
  int gray;
  DEVICE_H dh = handle;
  SCAN_T *scan;
  int in_pipe;

  scan = &(dh->scan);
  in_pipe = dh->scan_pipe[0];

  gray = 1;
  if ( a_get_mode_e( handle ) == OPT_MODE_COLOR )
    {
      gray = 0;
    }
  res = a_get_resolution( handle );

  *length = 0;

  while ( a_lines_left( scan ) && *length != max_length )
    {
      DBG( D_INFO, ">> line %d length %d\n", scan->current_line, *length );
      *length += a_read_from_scan( scan, 
                                   &data[*length],
                                   max_length - *length );

      if ( a_line_empty( scan ) )
        {
          if ( gray )
            {
              while ( a_space_in_fifo( scan->r ) )
                {
                  a_write_to_fifo( scan->r, in_pipe );
                }

              if ( res == 1200 )
                {
                  a_read_from_fifo_g_2( scan );
                }
              else
                {
                  a_read_from_fifo_g( scan );
                }
            }
          else
            {
              if ( res == 75 && ( scan->current_line & 3 ) == 3 )
                {
                  a_read_idx_inc( scan->r );
                  a_read_idx_inc( scan->g );
                  a_read_idx_inc( scan->b );
                }
              while ( a_space_in_fifo( scan->r ) ||
                      a_space_in_fifo( scan->g ) ||
                      a_space_in_fifo( scan->b ) )
                {
                 if ( !a_space_in_fifo( scan->r ) )
                    {
                      a_read_idx_inc( scan->r );
                    }
                  if ( !a_space_in_fifo( scan->g ) )
                    {
                      a_read_idx_inc( scan->g );
                    }
                  a_write_to_fifo( scan->r, in_pipe );
                  a_write_to_fifo( scan->g, in_pipe );
                  a_write_to_fifo( scan->b, in_pipe );
                }

              if ( res == 1200 )
                {
                  a_read_from_fifos_c_2( scan );
                }
              else
                {
                  a_read_from_fifos_c( scan );
                }
            }
        }
    }
}
