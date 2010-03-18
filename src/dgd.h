/*
 * This file is part of DGD, http://dgd-osr.sourceforge.net/
 * Copyright (C) 1993-2010 Dworkin B.V.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

# include "host.h"

typedef struct _string_ string;
typedef struct _array_ array;
typedef struct _object_ object;
typedef struct _value_ value;
typedef struct _control_ control;
typedef struct _dataplane_ dataplane;
typedef struct _dataspace_ dataspace;
typedef struct _frame_ frame;

# include "config.h"
# include "alloc.h"
# include "error.h"

# define BSET(map, bit)		(map[(bit) >> 3] |= (1 << ((bit) & 7)))
# define BCLR(map, bit)		(map[(bit) >> 3] &= ~(1 << ((bit) & 7)))
# define BTST(map, bit)		(map[(bit) >> 3] & (1 << ((bit) & 7)))

extern bool call_driver_object	(frame*, char*, int);
extern void interrupt		(void);
extern void endthread		(void);
extern void errhandler		(frame*, Int);
# ifdef DGD_EXTENSION
extern void dgd_error		();
# endif
extern int  dgd_main		(int, char**);

extern bool intr;
