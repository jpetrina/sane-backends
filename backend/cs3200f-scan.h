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

#ifndef CS3200F_SCAN_H
#define CS3200F_SCAN_H

#include "../include/sane/sane.h"

/***********************************************************************
 * Interface
 ***********************************************************************/

/** Get scanning parameters
 *  @param handle handle of the device
 *  @param params pointer to the parameter structure
 *  @return status
 */
SANE_Status s_get_params( SANE_Handle handle,
                          SANE_Parameters * params );

/** Start scanning with the current options
 *  @param handle handle of the device
 *  @return status
 */
SANE_Status s_scan_start( SANE_Handle handle );

/** Read scan data
 */
SANE_Status
s_scan_read( SANE_Handle handle,
             SANE_Byte * data,
             SANE_Int max_length,
             SANE_Int * length );


/** Cancel scanning
 */
void
s_scan_cancel( SANE_Handle handle );



#endif /* ndef CS3200F_SCAN_H */
