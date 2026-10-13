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

#define BACKEND_NAME cs3200f

#include "../include/sane/config.h"
#include "../include/sane/sane.h"
#include "../include/sane/sanei_backend.h"
#include "../include/sane/sanei_debug.h"
#include "../include/sane/sanei_access.h"
#include "../include/sane/sanei_usb.h"
#include "../include/sane/sanei_thread.h"

#include "../include/sane/saneopts.h"
#include "cs3200f-dat.h"
#include "cs3200f-dev.h"
#include "cs3200f-opt.h"
#include "cs3200f-scan.h"

/***********************************************************************
 * Debug info levels
 ***********************************************************************/

#define D_ERR    1
#define D_WARN   2
#define D_INFO   3
#define D_CALL   4
#define D_RES    5

/***********************************************************************
 * Library
 ***********************************************************************/

#define CS_VERSION_MAJOR 1
#define CS_VERSION_MINOR 1
#define CS_VERSION_BUILD 1

static const SANE_Int 
c_version = SANE_VERSION_CODE( CS_VERSION_MAJOR,
                               CS_VERSION_MINOR,
                               CS_VERSION_BUILD );

SANE_Status
sane_init( SANE_Int * version_code,
           SANE_Auth_Callback authorize )
{

  (void)authorize; /* we don't need authorizations */

  DBG_INIT();
  
  DBG( D_CALL, "sane_init( version_code = %08X, authorize = %08X )\n",
       (unsigned int)version_code, (unsigned int)authorize );

  sanei_access_init( STRINGIFY(BACKEND_NAME) );
  sanei_usb_init();
  sanei_thread_init();

  d_init_devices();

  if ( version_code )
    {
      *version_code = c_version;
      DBG( D_RES, "return version %08X\n", c_version );
    }

  return SANE_STATUS_GOOD;
}

void
sane_exit( void )
{
  DBG( D_CALL, "sane_exit()\n" );
}

/***********************************************************************
 * Devices
 ***********************************************************************/

SANE_Status
sane_get_devices( const SANE_Device *** device_list,
                  SANE_Bool local_only )
{
  (void)local_only; /* we don't look for remote devices anyway */

  DBG( D_CALL, "sane_get_devices( device_list = %08X, local_only = %d )\n",
       (unsigned int)device_list, local_only );

  if ( device_list )
    {
      *device_list = d_get_devices();
    }

  return SANE_STATUS_GOOD;
}

SANE_Status
sane_open( SANE_String_Const devicename,
           SANE_Handle * handle )
{
  DBG( D_CALL, "sane_open( devicename = %s, handle = %08X )\n",
       devicename, (unsigned int)handle );

  return d_open_device( devicename, handle );
}

void
sane_close( SANE_Handle handle )
{
  (void)handle;

  DBG( D_CALL, "sane_close( handle = %08X )\n",
       (unsigned int)handle );

  d_close_device( handle );
}

/***********************************************************************
 * Options
 ***********************************************************************/

const SANE_Option_Descriptor *
sane_get_option_descriptor( SANE_Handle handle, 
                            SANE_Int option )
{
  const SANE_Option_Descriptor * od;

  (void)handle;

  DBG( D_CALL, "sane_get_option_descriptor( handle = %08X, option = %d )\n",
       (unsigned int)handle, option );

  od = o_opt_desc( handle, option );

  if ( od )
    {
      DBG( D_RES, "return option descriptor for %s\n", od->title );
    }
  else
    {
      DBG( D_ERR, "no option descriptor to return\n" );
    }

  return od;
}

SANE_Status
sane_control_option( SANE_Handle handle, 
                     SANE_Int option,
                     SANE_Action action, 
                     void *value,
                     SANE_Int * info )
{
  (void)handle;
  (void)option;
  (void)info;

  DBG( D_CALL, "sane_control_option( handle = %08X, option = %d, "
       "action = %d, value = %08X, info = %08X)\n",
       (unsigned int)handle, option, action,
       (unsigned int)value, (unsigned int)info );

  switch ( action )
    {
    case SANE_ACTION_GET_VALUE:
      return o_opt_get( handle, option, value );
    case SANE_ACTION_SET_VALUE:
      return o_opt_set( handle, option, value, info );
    case SANE_ACTION_SET_AUTO:
      return SANE_STATUS_UNSUPPORTED;
    default:
      return SANE_STATUS_INVAL;
    }
}

/***********************************************************************
 * Scanning
 ***********************************************************************/

SANE_Status
sane_get_parameters( SANE_Handle handle,
                     SANE_Parameters * params )
{
  DBG( D_CALL, "sane_get_parameters( handle = %08X, params = %08X )\n",
       (unsigned int)handle, (unsigned int)params );

  return s_get_params( handle, params );
}

SANE_Status
sane_start( SANE_Handle handle )
{
  DBG( D_CALL, "sane_start( handle = %08X )\n",
       (unsigned int)handle );

  return s_scan_start( handle );
}

SANE_Status
sane_read( SANE_Handle handle,
           SANE_Byte * data,
           SANE_Int max_length,
           SANE_Int * length )
{
  DBG( D_CALL, "sane_read( handle = %08X, data = %08X, "
       "max_length = %d, length = %08X )\n",
       (unsigned int)handle, (unsigned int)data,
       max_length, (unsigned int)length );

  return s_scan_read( handle, data, max_length, length );
}

void
sane_cancel( SANE_Handle handle )
{
  DBG( D_CALL, "sane_cancel( handle = %08X )\n",
       (unsigned int)handle );

  s_scan_cancel( handle );
}

SANE_Status
sane_set_io_mode( SANE_Handle handle,
                  SANE_Bool non_blocking )
{
  (void)handle;
  (void)non_blocking;

  DBG( D_CALL, "sane_set_io_mode( handle = %08X, non_blocking = %d )\n",
       (unsigned int)handle, non_blocking );

  return SANE_STATUS_UNSUPPORTED;
}

SANE_Status
sane_get_select_fd( SANE_Handle handle,
                    SANE_Int * fd )
{
  (void)handle;
  (void)fd;

  DBG( D_CALL, "sane_get_select_fd( handle = %08X, fd = %08X )\n",
       (unsigned int)handle, (unsigned int)fd );

  return SANE_STATUS_UNSUPPORTED;
}


/***********************************************************************
 * Level 2
 ***********************************************************************/

#include "cs3200f-dev.c"
#include "cs3200f-opt.c"
#include "cs3200f-scan.c"

/***********************************************************************
 * Level 3
 ***********************************************************************/

#include "cs3200f-drv.c"

/***********************************************************************
 * Level 4
 ***********************************************************************/

#include "cs3200f-dat.c"
#include "cs3200f-usb.c"

/************************************************************************
 * $Id: cs3200f.c,v 1.2 2006/05/27 18:47:49 lapi-guest Exp $
 ************************************************************************/
