/* Copyright (C) 2003, 2004, 2005, 2006, 2008, 2009 Dean Beeler, Jerome Fisher
 * Copyright (C) 2011-2020 Dean Beeler, Jerome Fisher, Sergey V. Mikayev
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Hand-written replacement for the CMake-generated config.h (upstream: config.h.in).
 * The library is compiled directly into the application as a static library,
 * exposing only the C++ API (exports type 0). */

#ifndef MT32EMU_CONFIG_H
#define MT32EMU_CONFIG_H

#define MT32EMU_VERSION      "2.4.0"
#define MT32EMU_VERSION_MAJOR 2
#define MT32EMU_VERSION_MINOR 4
#define MT32EMU_VERSION_PATCH 0

/* 0: The full-featured C++ API is only available in this build. */
#define MT32EMU_EXPORTS_TYPE  0

#endif
