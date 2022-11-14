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

#ifndef CS3200F_USB_H
#define CS3200F_USB_H

#include "../include/sane/sane.h"

/***********************************************************************
 * Basic interface functions
 ***********************************************************************/

/** Open the scanner
 *  @param dev the device to be opened
 *  @return status of the opening
 */
SANE_Status
u_open( DEVICE_T *dev );

/** Close the scanner
 *  @param dev the device to be close
 */
void
u_close( DEVICE_T *dev );

/** Read one byte from the scanner 
 *  @param dn device number of the scanner
 *  @param value wValue of the control transfer
 *  @param index wIndex of the control transfer
 *  @return the byte read
 */
SANE_Byte
u_byte_read( SANE_Int dn, 
             SANE_Int value, 
             SANE_Int index );

/** Write one byte to the scanner 
 *  @param dn device number of the scanner
 *  @param value wValue of the control transfer
 *  @param index wIndex of the control transfer
 *  @param data  the byte to be written
 */
void
u_byte_write( SANE_Int dn, 
              SANE_Int value, 
              SANE_Int index,
              SANE_Byte data );

/** Read bytes from the scanner 
 *  @param dn device number of the scanner
 *  @param value wValue of the control transfer
 *  @param index wIndex of the control transfer
 *  @param data pointer to the data buffer
 *  @param len number of bytes to read
 *  @return status of the read
 */
SANE_Status
u_read( SANE_Int dn, 
        SANE_Int value, 
        SANE_Int index,
        SANE_Byte *data,
        SANE_Int len );

/** Write bytes to the scanner 
 *  @param dn device number of the scanner
 *  @param value wValue of the control transfer
 *  @param index wIndex of the control transfer
 *  @param data  the byte to be written
 *  @param len number of bytes to write
 *  @note The last byte of the data must be 0!
 */
SANE_Status
u_write( SANE_Int dn, 
         SANE_Int value, 
         SANE_Int index,
         SANE_Byte *data,
         SANE_Int len );

/** Bulk read bytes from the scanner 
 *  @param dn device number of the scanner
 *  @param data pointer to the data buffer
 *  @param len pointer to the number of bytes to read;
 *             after return the number indicates bytes read
 *  @return status of the read
 */
SANE_Status
u_bulk_read( SANE_Int dn, 
             SANE_Byte *data,
             size_t *len );

/** Bulk write bytes to the scanner 
 *  @param dn device number of the scanner
 *  @param data pointer to the data buffer
 *  @param len pointer to the number of bytes to write
 *             after return the number indicates bytes written
 *  @return status of the write
 */
SANE_Status
u_bulk_write( SANE_Int dn, 
              SANE_Byte *data,
              size_t *len );

/** Read bytes from the scanner memory
 *  @param dn device number of the scanner
 *  @param addr address to read from
 *  @param data pointer to the data buffer
 *  @param len pointer to the number of bytes to read;
 *             after return the number indicates bytes read
 *  @return status of the read
 *  @note Address must be multiple of 16 and length multiple of 64!
 */
SANE_Status
u_mem_read( SANE_Int dn, 
            SANE_Int addr,
            SANE_Byte *data,
            size_t *len );

/** Write bytes to the scanner memory
 *  @param dn device number of the scanner
 *  @param addr address to write to
 *  @param data pointer to the data buffer
 *  @param len poiter to the number of bytes to write;
 *             after return the number indicates bytes written
 *  @return status of the write
 *  @note Address and length must be multiples of 16!
 */
SANE_Status
u_mem_write( SANE_Int dn, 
             SANE_Int addr,
             SANE_Byte *data,
             size_t *len );

/***********************************************************************
 * Applied interface functions
 ***********************************************************************/

/** Read a byte from the scanner memory
 *  @param dn the device number of the scanner
 *  @param addr address to read form
 *  @return the byte or 0 in failure
 */
SANE_Byte
u_b_r( SANE_Int dn, SANE_Int addr );

/** Write a byte from the scanner memory
 *  @param dn the device number of the scanner
 *  @param addr address to read form
 *  @param data the byte to write
 */
void
u_b_w( SANE_Int dn, SANE_Int addr, SANE_Byte data );

/** Query the scan engine state
 *  @param dn the device number of the scanner
 *  @return the state
 */
SANE_Byte
u_state_q( SANE_Int dn );

/** Query if the scanner cartridge is at home
 *  @param dn the device number of the scanner
 *  @return the home status
 */
SANE_Bool
u_home_q( SANE_Int dn );

/** Query the scanner cartridge position (in 1200 dpi lines)
 *  @param dn the device number of the scanner
 *  @return the line number
 */
SANE_Int
u_line_q( SANE_Int dn );

#endif /* ndef CS3200F_USB_H */
