/* Common hooks for ATMEL AVR.
   Copyright (C) 1998-2019 Free Software Foundation, Inc.

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.
   
   GCC is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with GCC; see the file COPYING3.  If not see
   <http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "common/common-target.h"
#include "common/common-target-def.h"

/* Implement TARGET_OPTION_OPTIMIZATION_TABLE.  */
static const struct default_options avr_option_optimization_table[] =
  {
    // With -fdelete-null-pointer-checks option, the compiler assumes
    // that dereferencing of a null pointer would halt the program.
    // For AVR this assumption is not true and a program can safely
    // dereference null pointers.  Changes made by this option may not
    // work properly for AVR.  So disable this option.
    { OPT_LEVELS_ALL, OPT_fdelete_null_pointer_checks, NULL, 0 },
    // The only effect of -fcaller-saves might be that it triggers
    // a frame without need when it tries to be smart around calls.
    { OPT_LEVELS_ALL, OPT_fcaller_saves, NULL, 0 },
    { OPT_LEVELS_1_PLUS_NOT_DEBUG, OPT_mgas_isr_prologues, NULL, 1 },
    { OPT_LEVELS_1_PLUS, OPT_mmain_is_OS_task, NULL, 1 },
    /* Allow optimizer to introduce store data races. This used to be the
       default -- it was changed because bigger targets did not see any
       performance decrease. For the AVR though, disallowing data races
       introduces additional code in LIM and increases reg pressure.  */
    { OPT_LEVELS_ALL, OPT_fallow_store_data_races, NULL, 1 },
    { OPT_LEVELS_NONE, 0, NULL, 0 }
  };

#undef TARGET_OPTION_OPTIMIZATION_TABLE
#define TARGET_OPTION_OPTIMIZATION_TABLE avr_option_optimization_table

#undef TARGET_EXCEPT_UNWIND_INFO
#define TARGET_EXCEPT_UNWIND_INFO sjlj_except_unwind_info

struct gcc_targetm_common targetm_common = TARGETM_COMMON_INITIALIZER;
