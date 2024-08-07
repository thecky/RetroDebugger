/*
*************************************************************
** P64 reference implementation by Benjamin 'BeRo' Rosseaux *
*************************************************************
**
** Copyright (c) 2011-2012, Benjamin Rosseaux
**
** This software is provided 'as-is', without any express or implied
** warranty. In no event will the authors be held liable for any damages
** arising from the use of this software.
**
** Permission is granted to anyone to use this software for any purpose,
** including commercial applications, and to alter it and redistribute it
** freely, subject to the following restrictions:
**
**    1. The origin of this software must not be misrepresented; you must not
**    claim that you wrote the original software. If you use this software
**    in a product, an acknowledgment in the product documentation would be
**    appreciated but is not required.
**
**    2. Altered source versions must be plainly marked as such, and must not be
**    misrepresented as being the original software.
**
**    3. This notice may not be removed or altered from any source
**   distribution.
**
*/

#ifndef P64CONFIG_H
#define P64CONFIG_H

#include "lib.h"
#include "vicetypes.h"

#ifdef P64_USE_STDINT
#undef P64_USE_STDINT
#endif

#define P64_USE_OWN_TYPES

#define p64_malloc lib_malloc
#define p64_realloc lib_realloc
#define p64_free lib_free

typedef SIGNED_CHAR p64_int8_t;
typedef SWORD p64_int16_t;
typedef SDWORD p64_int32_t;

typedef BYTE p64_uint8_t;
typedef WORD p64_uint16_t;
typedef DWORD p64_uint32_t;

#endif
