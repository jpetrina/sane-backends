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

#ifndef CS3200F_DRV_H
#define CS3200F_DRV_H

#include "../include/sane/sane.h"

/***********************************************************************
 * Interface constants
 ***********************************************************************/

/** Lamp control */
typedef enum
  {
    LAMP_MAIN_LOW,
    LAMP_MAIN_HIGH,
    LAMP_TA
  } LAMP_E;

/** Color line of the scan */
typedef enum
  {
    COLOR_RED = 0,
    COLOR_GREEN,
    COLOR_BLUE,
    COLOR_ALL
  } COLOR_E;

/***********************************************************************
 * The device interface
 ***********************************************************************/

/** Boot the scanner
 *  @param dn device number of the scanner
 *  @return status of the boot
 */
SANE_Status
r_boot( SANE_Int dn );

/** Set lamp watchdog counts
 *  @param dn device number of the scanner
 *  @param c1 zero state count
 *  @param c2 one state count
 *  @return status of the boot
 *  @note Unit of the counts is such that there are 31+13/15 counts
 *        in one second. Typically c1 = 4000 and c2 = 20000 making
 *        the lamp tun-off time 24000/(31+13/15) = 753 s or about
 *        12.5 min.
 */
SANE_Status
r_wd_set( SANE_Int dn, SANE_Int c1, SANE_Int c2 );

/** Turn a lamp on
 *  @param dn device number of the scanner
 *  @param lamp which lamp to turn on
 *  @return status of the command
 */
SANE_Status
r_lamp_on( SANE_Int dn, LAMP_E lamp );

/** Turn lamps off
 *  @param dn device number of the scanner
 */
void
r_lamp_off( SANE_Int dn );

/** Set AFE
 *  @param dn device number of the scanner
 *  @param as the AFE settings
 */
void
r_afe_set( SANE_Int dn, AFE_SET_T *as );

/** Move scan head home
 *  @param dn device number of the scanner
 */
void r_home( SANE_Int dn );

/** Move scan head to the given line
 *  @param dn device number of the scanner
 *  @param line line to move to
 */
void r_move( SANE_Int dn, SANE_Int line );

/** Wait until the lamp intensity settles 
 *  @param dn device number of the scanner
 *  @param ta tells if transparency adapter is being used
 *  @param nmax maximum number of trials (also approximately max 
 *              waiting time is 5*nmax seconds)
 *  @return SANE_STATUS_GOOD if settled, else SANE_STATUS_CANCELLED
 */
SANE_Status 
r_lamp_settle( SANE_Int dn, SANE_Bool ta, SANE_Int nmax );

/** Calibrate AFE to given targets
 *  @param dn device number of the scanner
 *  @param res scan head resolution (300/600/1200)
 *  @param ta tells if transparency adapter is being used
 *  @param afe_set pointer to initial AFE settings or null if
 *                 offset 0 gain 1 should be used
 *  @param target_swing target swing of the signal (0..255, typ 250)
 *  @param target_offset target offset of the signal (0..255, typ 2 )
 *  @return pointer to a AFE setting structure which is afe_set or
 *          a newly allocated block if afe_set is null
 */
AFE_SET_T *
r_afe_calibrate( SANE_Int dn, 
                 SANE_Int res, 
                 SANE_Bool ta,
                 AFE_SET_T *afe_set,
                 SANE_Int target_swing,
                 SANE_Int target_offset );

/** Set scanning offsets
 *  @param dn device number of the scanner
 *  @param col horizontal offset in 1200 dpi pixels
 *  @param row vertical offset in 2400 dpi lines
 */
void r_offset( SANE_Int dn, SANE_Int col, SANE_Int row );

/** Reset scan
 *  @param dn device number of the scanner
 */
void r_reset( SANE_Int dn );

/** Set gamma
 *  @param dn device number of the scanner
 *  @param t linear threshold (typ 0.05, rane 0-1)
 *  @param rg red gamma
 *  @param gg green gamma
 *  @param bg blue gamma
 */
void r_gamma( SANE_Int dn, 
              SANE_Fixed t, 
              SANE_Fixed rg, 
              SANE_Fixed gg, 
              SANE_Fixed bg );

/** Set null shading
 *  @param dn device number of the scanner
 */
SANE_Status
r_null_shading( SANE_Int dn );

/** Start color scan
 *  @param dn device number of the scanner
 *  @param res resolution
 *  @param x column offset in pixels of current resolution
 *  @param w scan width in pixels of currebt resolution
 *  @param y starting line in current resoloution
 *  @param l number of lines in current resolution
 *  @param c color line of the scan
 */
SANE_Status
r_scan( SANE_Int dn, 
        SANE_Int res, 
        SANE_Int x,
        SANE_Int w,
        SANE_Int y,
        SANE_Int l,
        COLOR_E c );

/** Find the top left corner of the scanning area
 *  @param dn device number of the scanner
 *  @param res resolution
 *  @param top pointer to var to receive the x coordinate of 
 *             the top left corner in 1200 lpi lines
 *  @param left pointer to var to receive the y coordinate of 
 *              the top left corner in 1200 dpi pixels
 */
void r_find_top_left( SANE_Int dn, 
                      SANE_Int res, 
                      SANE_Int *top,
                      SANE_Int *left );

#endif /* ndef CS3200F_DRV_H */

/************************************************************************
 * $Id: cs3200f-drv.h,v 1.2 2006/03/31 18:55:51 lapi-guest Exp $
 ************************************************************************/
