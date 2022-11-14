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

#include "cs3200f-usb.h"

#include <stdlib.h>
#include <unistd.h>

#include "../include/sane/sanei.h"
#include "../include/sane/sanei_usb.h"

/***********************************************************************
 * local prototypes
 ***********************************************************************/

/** Check if the device is connected via high speed USB
 *  @param dn usb device number
 *  @return SANE_TRUE if connected via USB HS, else SANE_FALSE
 */
static SANE_Bool
is_hs( SANE_Int dn );

/***********************************************************************
 * Basic interface functions
 ***********************************************************************/

SANE_Status
u_open( DEVICE_T *dev )
{
  SANE_Status status;

  DBG( D_CALL, "u_open( dev = %08X )\n", (unsigned int)dev );

  status = sanei_usb_open( dev->name, &(dev->dn) );

  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "sanei_usb_open() failed with status %d\n", status );
      return status;
    }

  dev->open = SANE_TRUE;

  dev->hs = is_hs( dev->dn );

  return status;
}

void
u_close( DEVICE_T *dev )
{
  DBG( D_CALL, "u_close( dev = %08X )\n", (unsigned int)dev );

  if ( dev->open ) {
    sanei_usb_close( dev->dn );
  }

  dev->open = SANE_FALSE;

}

SANE_Byte
u_byte_read( SANE_Int dn,
             SANE_Int value,
             SANE_Int index )
{
  SANE_Byte data;
  SANE_Status status;

  DBG( D_CALL, "u_byte_read( dn = %d, value = %04X, index = %04X )\n",
       dn, value, index );

  status = sanei_usb_control_msg( dn,
                                  USB_DIR_IN
                                  | USB_TYPE_VENDOR
                                  | USB_RECIP_DEVICE,
                                  0x0c,
                                  value,
                                  index,
                                  1,
                                  &data );

  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "u_byte_read() failed\n" );
      return 0;
    }

  DBG( D_RES, "return: %2X\n", (unsigned int) data );

  return data;
}

void
u_byte_write( SANE_Int dn,
               SANE_Int value,
               SANE_Int index,
               SANE_Byte data )
{
  SANE_Byte data_out[2] = { 0, 0 };
  SANE_Status status;

  DBG( D_CALL, "u_byte_write( dn = %d, value = %04X,"
       " index = %04X, data = %02X )\n",
       dn, value, index, data );

  data_out[0] = data;

  status = sanei_usb_control_msg( dn,
                                  USB_DIR_OUT
                                  | USB_TYPE_VENDOR
                                  | USB_RECIP_DEVICE,
                                  0x04,
                                  value,
                                  index,
                                  2,
                                  data_out );

  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "u_byte_write() failed\n" );
    }
}

SANE_Status
u_read( SANE_Int dn,
        SANE_Int value,
        SANE_Int index,
        SANE_Byte *data,
        SANE_Int len )
{
  SANE_Status status;

  DBG( D_CALL, "u_read( dn = %d, value = %04X, index = %04X,"
       " data = %08X, len = %d, len )\n",
       dn, value, index, (unsigned int)data, len );

  status = sanei_usb_control_msg( dn,
                                  USB_DIR_IN
                                  | USB_TYPE_VENDOR
                                  | USB_RECIP_DEVICE,
                                  0x04,
                                  value,
                                  index,
                                  len,
                                  data );

  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "u_read() failed\n" );
    }

  return status;
}

SANE_Status
u_write( SANE_Int dn,
         SANE_Int value,
         SANE_Int index,
         SANE_Byte *data,
         SANE_Int len )
{
  SANE_Status status;

  DBG( D_CALL, "u_write( dn = %d, value = %04X, index = %04X,"
       " data = %08X, len = %d )\n",
       dn, value, index, (unsigned int)data, len);

  if ( data[len-1] != 0 )
    {
      DBG( D_WARN, "The last byte of the control write should be \\0\n" );
    }

  status = sanei_usb_control_msg( dn,
                                  USB_DIR_OUT
                                  | USB_TYPE_VENDOR
                                  | USB_RECIP_DEVICE,
                                  0x04,
                                  value,
                                  index,
                                  len,
                                  data );

  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "u_write() failed\n" );
    }

  return status;
}

SANE_Status
u_bulk_read( SANE_Int dn,
             SANE_Byte *data,
             size_t *len )
{
  SANE_Status status;
  int retry;
  size_t len_read;
  size_t len_remaining;

  DBG( D_CALL, "u_bulk_read( dn = %d, data = %08X, len = %d )\n",
       dn, (unsigned int)data, (int)(*len) );

#define RETRY_COUNT 60

  len_read = 0;

  for ( retry = 0; retry < RETRY_COUNT; retry++ )
    {
      len_remaining = *len - len_read;
      status = sanei_usb_read_bulk( dn, data+len_read, &len_remaining );
      len_read += len_remaining;
      if ( status != SANE_STATUS_IO_ERROR )
        {
          break;
        }
      usleep( 10000 );
      /* allow 10 ms for the device to catch up, then try again */
    }

  *len = len_read;

  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "u_bulk_read() failed\n" );
    }

  return status;
}

SANE_Status
u_bulk_write( SANE_Int dn,
              SANE_Byte *data,
              size_t *len )
{
  SANE_Status status;

  DBG( D_CALL, "u_bulk_write( dn = %d, data = %08X, len = %d )\n",
       dn, (unsigned int)data, *len );

  status = sanei_usb_write_bulk( dn, data, len );

  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "u_bulk_write() failed\n" );
    }

  return status;
}

SANE_Status
u_mem_read( SANE_Int dn,
            SANE_Int addr,
            SANE_Byte *data,
            size_t *len )
{
  SANE_Status status;
  SANE_Byte saddr[5];
  SANE_Byte slen[4];
  int i;

  DBG( D_CALL, "u_mem_read( dn = %d, addr = %08X, data = %08X, len = %d )\n",
       dn, addr, (unsigned int)data, (int)(*len));

  if ( ( addr & 0xf ) != 0 )
    {
      DBG( D_ERR, "Error: address must be multiple of 16\n" );
      return SANE_STATUS_INVAL;
    }

  if ( ( addr & 0x3f ) != 0 )
    {
      DBG( D_ERR, "Error: length must be multiple of 64\n" );
      return SANE_STATUS_INVAL;
    }

  for ( i = 0; i < 4; i++ )
    {
      saddr[i] = ( addr >> (8*i+1) ) & 0xff;
    }
  saddr[4] = 0;

  for ( i = 0; i < 3; i++ )
    {
      slen[i] = ( (SANE_Int)(*len) >> (8*i+1) ) & 0xff;
    }
  slen[3] = 0;

  status = u_state_q( dn );
  status = u_write( dn, 0x00f0, 0x0000, saddr, 5 );

  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Can't set address\n" );
      return status;
    }

  status = u_state_q( dn );
  status = u_write( dn, 0x00f4, 0x0000, slen, 4 );

  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Can't set length\n" );
      return status;
    }

  status = u_bulk_read( dn, data, len );

  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "u_mem_read() failed\n" );
    }

  return status;
}

SANE_Status
u_mem_write( SANE_Int dn,
             SANE_Int addr,
             SANE_Byte *data,
             size_t *len )
{
  SANE_Status status;
  SANE_Byte saddr[5];
  SANE_Byte slen[4];
  int i;

  DBG( D_CALL, "u_mem_write( dn = %d, addr = %08X, data = %08X, len = %d )\n",
       dn, addr, (unsigned int)data, (int)(*len) );

  if ( ( addr & 0xf ) != 0 )
    {
      DBG( D_ERR, "Error: address must be multiple of 16\n" );
      return SANE_STATUS_INVAL;
    }

  if ( ( addr & 0xf ) != 0 )
    {
      DBG( D_ERR, "Error: length must be multiple of 16\n" );
      return SANE_STATUS_INVAL;
    }

  for ( i = 0; i < 4; i++ )
    {
      saddr[i] = ( addr >> (8*i+1) ) & 0xff;
    }
  saddr[4] = 0;

  for ( i = 0; i < 3; i++ )
    {
      slen[i] = ( (SANE_Int)(*len) >> (8*i+1) ) & 0xff;
    }
  slen[3] = 0;

  status = u_state_q( dn );
  status = u_write( dn, 0x00f0, 0x0000, saddr, 5 );

  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Can't set address\n" );
      return status;
    }

  status = u_state_q( dn );
  status = u_write( dn, 0x00f3, 0x0000, slen, 4 );

  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "Can't set length\n" );
      return status;
    }

  status = u_bulk_write( dn, data, len );

  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "u_mem_write() failed\n" );
    }

  return status;
}

/***********************************************************************
 * Applied interface functions
 ***********************************************************************/

SANE_Byte
u_b_r( SANE_Int dn, SANE_Int addr )
{
  return u_byte_read( dn, 0x00e1, addr );
}

void
u_b_w( SANE_Int dn, SANE_Int addr, SANE_Byte data )
{
  u_byte_write( dn, 0x00e0, addr, data );
}

SANE_Byte
u_state_q( SANE_Int dn )
{
  SANE_Byte status[1] = { 0 };

  (void)u_read( dn, 0x0040, 0, status, 1 );

  return status[0];
}

SANE_Bool
u_home_q( SANE_Int dn )
{
  SANE_Byte status[2] = { 0, 0 };

  (void)u_read( dn, 0x0040, 0, status, 2 );

  return status[1]?SANE_FALSE:SANE_TRUE;
}

SANE_Int
u_line_q( SANE_Int dn )
{
  SANE_Byte status[4] = { 0, 0, 0, 0 };

  (void)u_read( dn, 0x0040, 0, status, 4 );

  return ( ( (SANE_Int)status[2] & 0xFF ) << 8 ) +
         ( (SANE_Int)status[3] & 0xFF );
}

/***********************************************************************
 * local utilities
 ***********************************************************************/

static SANE_Bool
is_hs( SANE_Int dn )
{
  SANE_Byte device_descriptor[ USB_DT_DEVICE_SIZE ];
  SANE_Status status;

  DBG( D_CALL, "is_hs( dn = %d )\n", dn );

  status = sanei_usb_control_msg( dn,
                                  USB_DIR_IN
                                  | USB_TYPE_STANDARD
                                  | USB_RECIP_DEVICE,
                                  USB_REQ_GET_DESCRIPTOR,
                                  ( USB_DT_DEVICE << 8 ),
                                  0,
                                  USB_DT_DEVICE_SIZE,
                                  device_descriptor );

  if ( status != SANE_STATUS_GOOD )
    {
      DBG( D_ERR, "xFailed to get device descriptor\n" );
      return SANE_FALSE;
    }

  DBG( D_RES, "USB interface version %X.%02X\n",
       device_descriptor[3], device_descriptor[2]);

  if ( device_descriptor[3] == 2 )
    {
      return SANE_TRUE;
    }

  return SANE_FALSE;
}

/************************************************************************
 * $Id: cs3200f-usb.c,v 1.4 2006/05/30 18:10:45 lapi-guest Exp $
 ************************************************************************/
