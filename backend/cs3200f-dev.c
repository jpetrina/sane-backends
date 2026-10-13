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

#include "cs3200f-dev.h"
#include "cs3200f-dat.h"
#include "cs3200f-drv.h"
#include "cs3200f-usb.h"

#include "../include/sane/sanei_access.h"

/***********************************************************************
 * Local types
 ***********************************************************************/

typedef struct DEVICE_NODE DEVICE_NODE_T;

struct DEVICE_NODE
{
  SANE_Device dev;
  SANE_String name;
  DEVICE_NODE_T *next;
};


/***********************************************************************
 * Local prototypes
 ***********************************************************************/

static void build_device_list( void );
static SANE_Status tell_device( SANE_String_Const name );
static void get_first_device( void );
static SANE_Status first_device( SANE_String_Const name );

/***********************************************************************
 * Local data
 ***********************************************************************/

static SANE_String_Const str_c_vendor_canon = 
  SANE_I18N( "CANON" );
static SANE_String_Const str_c_model_cs3200f = 
  SANE_I18N( "CS3200F" );
static SANE_String_Const str_c_type_flatbed_scanner = 
  SANE_I18N( "flatbed scanner" );

static const SANE_Int usb_vendor_canon   = 0x04A9;
static const SANE_Int usb_device_cs3200f = 0x2216;

static DEVICE_NODE_T *current_device_list;
static const SANE_Device **current_device_table;

static SANE_String first_device_name;

/***********************************************************************
 * Interface
 ***********************************************************************/

void d_init_devices( void )
{
  DBG( D_CALL, "d_init_devices()\n" );

  current_device_list = 0;
  current_device_table = 0;
  first_device_name = 0;

  a_init_data();
}

const SANE_Device **
d_get_devices( void )
{
  const SANE_Device **dt;

  DBG( D_CALL, "d_get_devices()\n" );

  build_device_list();
  dt = current_device_table;

  DBG( D_RES, "return device table %08X\n", (unsigned int)dt );
  return dt;
}

SANE_Status
d_open_device( SANE_String_Const devicename, SANE_Handle *handle )
{
  DEVICE_T *dh;
  SANE_String_Const the_name;

  DBG( D_CALL, "d_open_device( devicename = %s, handle = %08X )\n",
       devicename, (unsigned int)handle );

  if ( handle )
    {
      *handle = a_new_dev();
    }
  else
    {
      return SANE_STATUS_INVAL;
    }

  if ( ! *handle )
    {
      DBG( D_ERR, "Can't allocate memory for the device\n" );
      return SANE_STATUS_NO_MEM;
    }

  dh = *handle;

  if ( strcmp( devicename, "dummy" ) == 0 )
    {
      DBG( D_RES, "Dummy device handle %08X\n", (unsigned int)*handle );
      return SANE_STATUS_GOOD;
    } 

  if ( devicename[0] == 0 )
    {
      get_first_device();

      if ( first_device_name == 0 )
        {
          DBG( D_INFO, "No CS3200F found\n" );
          a_delete_dev( *handle );
          *handle = 0;
          return SANE_STATUS_IO_ERROR;
        }
      else
        {
          DBG( D_INFO, "CS3200F found at [%s]\n", first_device_name );
          the_name = first_device_name;
        }
    }
  else
    {
      the_name = devicename;
    }

  dh->name = malloc( strlen( the_name ) + 1 );
  strcpy( dh->name, the_name );

  /* check is there is already device with that name open */
  DBG( D_INFO, "Trying to lock %s\n", the_name );
  if ( sanei_access_lock( the_name, 5 ) )
    {
      DBG( D_INFO, "Device already open by someone else\n" );
      a_delete_dev( *handle );
      *handle = 0;
      return SANE_STATUS_DEVICE_BUSY;
    }
  DBG( D_INFO, "Lock obtained\n" );


  if ( u_open( dh ) != SANE_STATUS_GOOD )
    {
      sane_close( *handle );
      *handle = 0;
      return SANE_STATUS_DEVICE_BUSY;
    }

  if ( r_boot( dh->dn ) != SANE_STATUS_GOOD )
    {
      sane_close( *handle );
      *handle = 0;
      return SANE_STATUS_IO_ERROR;
    }

  DBG( D_RES, "Device handle %08X\n", (unsigned int)*handle );

  return SANE_STATUS_GOOD;
}

void
d_close_device( SANE_Handle handle )
{
  DEVICE_T *dh = handle;

  DBG( D_CALL, "d_close_device( handle = %08X )\n",
       (unsigned int)handle );

  if ( a_handle_valid( handle ) != SANE_STATUS_GOOD )
    {
      return;
    }

  u_close( dh );

  if ( dh->name )
    {
      DBG( D_INFO, "Unlocking %s\n", dh->name );
      (void)sanei_access_unlock( dh->name );
    }

  a_delete_dev( handle );
}

/***********************************************************************
 * Local code
 ***********************************************************************/

static void build_device_list( void )
{
  DEVICE_NODE_T *dn;
  int n_devices;
  int i;

  DBG( D_CALL, "build_device_list()\n" );

  /* kill old device list */
  while( current_device_list )
    {
      dn = current_device_list;
      current_device_list = current_device_list->next;
      if ( dn->name )
        {
          free( dn->name );
        }
      free( dn );
    }

  /* kill old device table */
  if ( current_device_table )
    {
      free( current_device_table );
      current_device_table = 0;
    }

  /* build new name list */
  sanei_usb_find_devices( usb_vendor_canon,
                          usb_device_cs3200f,
                          tell_device );

  /* add dummy device if the list is empty */
  if ( ! current_device_list )
    {
      (void)tell_device( "dummy" );
    }

  /* build new device table & device list*/
  n_devices = 0;
  dn = current_device_list;
  while ( dn )
    {
      ++n_devices;
      dn = dn->next;
    }

  current_device_table = 
    malloc( ( n_devices + 1 ) * sizeof( SANE_Device * ) );

  dn = current_device_list;

  for ( i = 0; i < n_devices; i++ )
    {
      current_device_table[i] = &(dn->dev);
      dn = dn->next;
    }
  current_device_table[i] = 0;
}

static SANE_Status tell_device( SANE_String_Const name )
{
  DEVICE_NODE_T *dn;

  DBG( D_CALL, "tell_device_name( name = %s )\n", name );

  if ( ! name )
    {
      /* got null string pointer */
      return SANE_STATUS_INVAL;
    }

  DBG( D_INFO, "Found device %s\n", name );

  dn = current_device_list;
  while ( dn && dn->next )
    {
      dn = dn->next;
    }

  if ( dn )
    {
      dn = dn->next = malloc( sizeof( DEVICE_NODE_T ) );
    }
  else
    {
      dn = current_device_list = malloc( sizeof( DEVICE_NODE_T ) );
    }

  dn->name = malloc( strlen( name ) + 1 );
  strcpy( dn->name, name );

  dn->dev.name = dn->name;
  dn->dev.vendor = str_c_vendor_canon;
  dn->dev.model = str_c_model_cs3200f;
  dn->dev.type = str_c_type_flatbed_scanner;

  dn->next = 0;

  return SANE_STATUS_GOOD;
}

static void get_first_device( void )
{
  DBG( D_CALL, "get_first_device()\n" );

  if ( first_device_name )
    {
      free( first_device_name );
      first_device_name = 0;
    }

  sanei_usb_find_devices( usb_vendor_canon,
                          usb_device_cs3200f,
                          first_device );
}

static SANE_Status first_device( SANE_String_Const name )
{
  DBG( D_CALL, "first_device( name = %s )\n", name );

  if ( ! first_device_name )
    {
        first_device_name = malloc( strlen( name ) + 1 );
        strcpy( first_device_name, name );
    }

  return SANE_STATUS_GOOD;
}

/************************************************************************
 * $Id: cs3200f-dev.c,v 1.1 2006/03/26 19:01:59 lapi-guest Exp $
 ************************************************************************/
