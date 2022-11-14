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

#ifndef CS3200F_OPT_H
#define CS3200F_OPT_H

#include "../include/sane/sane.h"
#include "cs3200f-dat.h"

/** Get option decriptor
 *  @param handle the device for which the descriptor is queried
 *  @param option the option for which the descriptor is queried
 *  @return pointer to the descriptor
 */
const SANE_Option_Descriptor *
o_opt_desc( SANE_Handle handle, 
            SANE_Int option );

/** Get option
 *  @param handle the device for which the option is queried
 *  @param option the option which is being queried
 *  @param value pointer to the location where value is put
 *  @return status of the query
 */
SANE_Status
o_opt_get( SANE_Handle handle, 
           SANE_Int option,
           void *value );

/** Set option
 *  @param handle the device for which the option is set
 *  @param option the option which is being set
 *  @param value pointer to the location where the new value is
 *  @param info tells about the side effects of setting
 *  @return status of the query
 */
SANE_Status
o_opt_set( SANE_Handle handle, 
           SANE_Int option,
           void *value,
           SANE_Int * info);

#endif /* ndef CS3200F_OPT_H */

/************************************************************************
 * $Id: cs3200f-opt.h,v 1.1 2006/03/26 19:01:59 lapi-guest Exp $
 ************************************************************************/
