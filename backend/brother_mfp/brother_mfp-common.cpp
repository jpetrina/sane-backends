/* sane - Scanner Access Now Easy.

   BACKEND brother_mfp

   Copyright (C) 2023 Jonathan Schulz

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
   along with this program.  If not, see <https://www.gnu.org/licenses/>.

   This file implements a SANE backend for Brother Multifunction Devices.
   */

#include "brother_mfp-common.h"

/* based on https://stackoverflow.com/a/29856690 */
const std::string computeMethodName(const std::string& function, const std::string& prettyFunction) {
    size_t locFunName = prettyFunction.find(function);
    size_t begin = prettyFunction.rfind(" ",locFunName) + 1;
    size_t end = prettyFunction.find("(",locFunName + function.length());
    return prettyFunction.substr(begin,end - begin);
}
