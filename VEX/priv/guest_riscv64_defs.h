
/*--------------------------------------------------------------------*/
/*--- begin                                   guest_riscv64_defs.h ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of Valgrind, a dynamic binary instrumentation
   framework.

   Copyright (C) 2020-2021 Petr Pavlu
      setup@dagobah.cz

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.

   The GNU General Public License is contained in the file COPYING.

   Neither the names of the U.S. Department of Energy nor the
   University of California nor the names of its contributors may be
   used to endorse or promote products derived from this software
   without prior written permission.
*/

/* Only to be used within the guest_riscv64_* files. */

#ifndef __VEX_GUEST_RISCV64_DEFS_H
#define __VEX_GUEST_RISCV64_DEFS_H

#include "libvex_basictypes.h"

#include "guest_generic_bb_to_IR.h"

/*------------------------------------------------------------*/
/*--- riscv64 to IR conversion                             ---*/
/*------------------------------------------------------------*/

/* Convert one riscv64 insn to IR. See the type DisOneInstrFn in
   guest_generic_bb_to_IR.h. */
DisResult disInstr_RISCV64(IRSB*              irbb,
                           const UChar*       guest_code,
                           Long               delta,
                           Addr               guest_IP,
                           VexArch            guest_arch,
                           const VexArchInfo* archinfo,
                           const VexAbiInfo*  abiinfo,
                           VexEndness         host_endness,
                           Bool               sigill_diag);

/* Used by the optimiser to specialise calls to helpers. */
IRExpr* guest_riscv64_spechelper(const HChar* function_name,
                                 IRExpr**     args,
                                 IRStmt**     precedingStmts,
                                 Int          n_precedingStmts);

/* Describes to the optimiser which part of the guest state require precise
   memory exceptions. This is logically part of the guest state description. */
Bool guest_riscv64_state_requires_precise_mem_exns(
   Int minoff, Int maxoff, VexRegisterUpdates pxControl);

extern VexGuestLayout riscv64guest_layout;

/*------------------------------------------------------------*/
/*--- riscv64 guest helpers                                ---*/
/*------------------------------------------------------------*/

#endif /* ndef __VEX_GUEST_RISCV64_DEFS_H */

/*--------------------------------------------------------------------*/
/*--- end                                     guest_riscv64_defs.h ---*/
/*--------------------------------------------------------------------*/
