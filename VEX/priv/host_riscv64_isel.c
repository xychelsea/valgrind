
/*--------------------------------------------------------------------*/
/*--- begin                                    host_riscv64_isel.c ---*/
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
*/

#include "host_riscv64_defs.h"
#include "main_globals.h"
#include "main_util.h"

/*------------------------------------------------------------*/
/*--- ISelEnv                                              ---*/
/*------------------------------------------------------------*/

/* This carries around:

   - A mapping from IRTemp to IRType, giving the type of any IRTemp we might
     encounter. This is computed before insn selection starts, and does not
     change.

   - A mapping from IRTemp to HReg. This tells the insn selector which virtual
     register is associated with each IRTemp temporary. This is computed before
     insn selection starts, and does not change. We expect this mapping to map
     precisely the same set of IRTemps as the type mapping does.

     - vregmap   holds the primary register for the IRTemp.
     - vregmapHI is only used for 128-bit integer-typed IRTemps. It holds the
                 identity of a second 64-bit virtual HReg, which holds the high
                 half of the value.

   - The code array, that is, the insns selected so far.

   - A counter, for generating new virtual registers.

   - The host hardware capabilities word. This is set at the start and does not
     change.

   - A Bool for indicating whether we may generate chain-me instructions for
     control flow transfers, or whether we must use XAssisted.

   - The maximum guest address of any guest insn in this block. Actually, the
     address of the highest-addressed byte from any insn in this block. Is set
     at the start and does not change. This is used for detecting jumps which
     are definitely forward-edges from this block, and therefore can be made
     (chained) to the fast entry point of the destination, thereby avoiding the
     destination's event check.

   - An IRExpr*, which may be NULL, holding the IR expression (an
     IRRoundingMode-encoded value) to which the FPU's rounding mode was most
     recently set. Setting to NULL is always safe. Used to avoid redundant
     settings of the FPU's rounding mode, as described in
     set_FPCR_rounding_mode() below.

   Note, this is all (well, mostly) host-independent.
*/

typedef struct {
   /* Constant -- are set at the start and do not change. */
   IRTypeEnv* type_env;

   HReg* vregmap;
   HReg* vregmapHI;
   Int   n_vregmap;

   UInt hwcaps;

   Bool   chainingAllowed;
   Addr64 max_ga;

   /* These are modified as we go along. */
   HInstrArray* code;
   Int          vreg_ctr;

   IRExpr* previous_rm;
} ISelEnv;

static HReg lookupIRTemp(ISelEnv* env, IRTemp tmp)
{
   vassert(tmp >= 0);
   vassert(tmp < env->n_vregmap);
   return env->vregmap[tmp];
}

static void addInstr(ISelEnv* env, RISCV64Instr* instr)
{
   addHInstr(env->code, instr);
   if (vex_traceflags & VEX_TRACE_VCODE) {
      ppRISCV64Instr(instr, True /*mode64*/);
      vex_printf("\n");
   }
}

static HReg newVRegI(ISelEnv* env)
{
   HReg reg = mkHReg(True /*virtual*/, HRcInt64, 0, env->vreg_ctr);
   env->vreg_ctr++;
   return reg;
}

/*------------------------------------------------------------*/
/*--- ISEL: Forward declarations                           ---*/
/*------------------------------------------------------------*/

/* These are organised as iselXXX and iselXXX_wrk pairs. The iselXXX_wrk do the
   real work, but are not to be called directly. For each XXX, iselXXX calls its
   iselXXX_wrk counterpart, then checks that all returned registers are virtual.
   You should not call the _wrk version directly. */

static HReg iselIntExpr_R(ISelEnv* env, IRExpr* e);
static void iselInt128Expr(HReg* rHi, HReg* rLo, ISelEnv* env, IRExpr* e);

/*------------------------------------------------------------*/
/*--- ISEL: Function call helpers                          ---*/
/*------------------------------------------------------------*/

/* Used only in doHelperCall(). See the big comment in doHelperCall() regarding
   handling of register-parameter arguments. This function figures out whether
   evaluation of an expression might require use of a fixed register. If in
   doubt return True (safe but suboptimal).
*/
static Bool mightRequireFixedRegs(IRExpr* e)
{
   switch (e->tag) {
   // TODO case Iex_VECRET:
   case Iex_GSPTR:
   case Iex_RdTmp:
   case Iex_Const:
   case Iex_Get:
      return False;
   default:
      return True;
   }
}

/* Do a complete function call. |guard| is a Ity_Bit expression indicating
   whether or not the call happens. If guard==NULL, the call is unconditional.
   |retloc| is set to indicate where the return value is after the call. The
   caller (of this fn) must generate code to add |stackAdjustAfterCall| to the
   stack pointer after the call is done. Returns True iff it managed to handle
   this combination of arg/return types, else returns False. */
// TODO Review return value handling and asserts.
static Bool doHelperCall(/*OUT*/ UInt*   stackAdjustAfterCall,
                         /*OUT*/ RetLoc* retloc,
                         ISelEnv*        env,
                         IRExpr*         guard,
                         IRCallee*       cee,
                         IRType          retTy,
                         IRExpr**        args)
{
   HReg   cond;
   HReg   argregs[RISCV64_N_ARGREGS];
   HReg   tmpregs[RISCV64_N_ARGREGS];
   Bool   go_fast;
   Int    n_args, i, nextArgReg;

   vassert(RISCV64_N_ARGREGS == 8);

   /* Set default returns.  We'll update them later if needed. */
   *stackAdjustAfterCall = 0;
   *retloc               = mk_RetLoc_INVALID();

   /* These are used for cross-checking that IR-level constraints on the use of
      IRExpr_VECRET() and IRExpr_GSPTR() are observed. */
   UInt nVECRETs = 0;
   UInt nGSPTRs  = 0;

   /* Marshal args for a call and do the call.

      This function only deals with a tiny set of possibilities, which cover all
      helpers in practice. The restrictions are that only arguments in registers
      are supported, hence only RISCV64_N_REGPARMS x 64 integer bits in total
      can be passed. In fact the only supported arg type is I64.

      Note that the cee->regparms field is meaningless on riscv64 hosts (since
      we only implement one calling convention) and so we always ignore it.

      The return type can be I{64,32,16,8} or V128. In the V128 case, it is
      expected that |args| will contain the special node IRExpr_VECRET(), in
      which case this routine generates code to allocate space on the stack for
      the vector return value.  Since we are not passing any scalars on the
      stack, it is enough to preallocate the return space before marshalling any
      arguments, in this case.

      |args| may also contain IRExpr_GSPTR(), in which case the value in the
      guest state pointer register minus BASEBLOCK_OFFSET_ADJUSTMENT is passed
      as the corresponding argument.

      Generating code which is both efficient and correct when parameters are to
      be passed in registers is difficult, for the reasons elaborated in detail
      in comments attached to doHelperCall() in VEX/priv/host_x86_isel.c. Here,
      we use a variant of the method described in those comments.

      The problem is split into two cases: the fast scheme and the slow scheme.
      In the fast scheme, arguments are computed directly into the target (real)
      registers. This is only safe when we can be sure that computation of each
      argument will not trash any real registers set by computation of any other
      argument.

      In the slow scheme, all args are first computed into vregs, and once they
      are all done, they are moved to the relevant real regs. This always gives
      correct code, but it also gives a bunch of vreg-to-rreg moves which are
      usually redundant but are hard for the register allocator to get rid of.

      To decide which scheme to use, all argument expressions are first
      examined. If they are all so simple that it is clear they will be
      evaluated without use of any fixed registers, use the fast scheme, else
      use the slow scheme. Note also that only unconditional calls may use the
      fast scheme, since having to compute a condition expression could itself
      trash real registers.

      Note this requires being able to examine an expression and determine
      whether or not evaluation of it might use a fixed register. That requires
      knowledge of how the rest of this insn selector works. Currently just the
      following 4 are regarded as safe -- hopefully they cover the majority of
      arguments in practice: IRExpr_GSPTR, IRExpr_Tmp, IRExpr_Const, IRExpr_Get.
   */

   n_args = 0;
   for (i = 0; args[i]; i++) {
      IRExpr* arg = args[i];
      if (UNLIKELY(arg->tag == Iex_VECRET))
         nVECRETs++;
      else if (UNLIKELY(arg->tag == Iex_GSPTR))
         nGSPTRs++;
      n_args++;
   }

   /* If this fails, the IR is ill-formed. */
   vassert(nGSPTRs == 0 || nGSPTRs == 1);

   /* If we have a VECRET, allocate space on the stack for the return value, and
      record the stack pointer after that. */
   HReg r_vecRetAddr = INVALID_HREG;
   if (nVECRETs == 1) {
      vassert(retTy == Ity_V128);
      r_vecRetAddr = newVRegI(env);
      addInstr(env,
               RISCV64Instr_ADDI(hregRISCV64_x2(), hregRISCV64_x2(), -16));
      addInstr(env, RISCV64Instr_MV(r_vecRetAddr, hregRISCV64_x2()));
   } else {
      /* If either of these fail, the IR is ill-formed. */
      vassert(retTy != Ity_V128 && retTy != Ity_V256);
      vassert(nVECRETs == 0);
   }

   argregs[0] = hregRISCV64_x10();
   argregs[1] = hregRISCV64_x11();
   argregs[2] = hregRISCV64_x12();
   argregs[3] = hregRISCV64_x13();
   argregs[4] = hregRISCV64_x14();
   argregs[5] = hregRISCV64_x15();
   argregs[6] = hregRISCV64_x16();
   argregs[7] = hregRISCV64_x17();

   tmpregs[0] = tmpregs[1] = tmpregs[2] = tmpregs[3] = INVALID_HREG;
   tmpregs[4] = tmpregs[5] = tmpregs[6] = tmpregs[7] = INVALID_HREG;

   /* First decide which scheme (slow or fast) is to be used. First assume the
      fast scheme, and select slow if any contraindications (wow) appear. */

   go_fast = True;

   // TODO Review if this is really needed.
   /* We'll need space on the stack for the return value. Avoid possible
      complications with nested calls by using the slow scheme. */
   if (retTy == Ity_V128 || retTy == Ity_V256)
      go_fast = False;

   if (go_fast && guard != NULL) {
      if (guard->tag == Iex_Const && guard->Iex.Const.con->tag == Ico_U1 &&
          guard->Iex.Const.con->Ico.U1 == True) {
         /* Unconditional. */
      } else {
         /* Not manifestly unconditional -- be conservative. */
         go_fast = False;
      }
   }

   if (go_fast)
      for (i = 0; i < n_args; i++) {
         if (mightRequireFixedRegs(args[i])) {
            go_fast = False;
            break;
         }
      }

   /* At this point the scheme to use has been established. Generate code to get
      the arg values into the argument regs. If we run out of arg regs, give up.
    */

   if (go_fast) {

      /* FAST SCHEME */
      nextArgReg = 0;

      for (i = 0; i < n_args; i++) {
         IRExpr* arg = args[i];

         IRType aTy = Ity_INVALID;
         if (LIKELY(!is_IRExpr_VECRET_or_GSPTR(arg)))
            aTy = typeOfIRExpr(env->type_env, args[i]);

         if (nextArgReg >= RISCV64_N_ARGREGS)
            return False; /* Out of argregs. */

         if (aTy == Ity_I64) {
            addInstr(env, RISCV64Instr_MV(argregs[nextArgReg],
                                          iselIntExpr_R(env, args[i])));
            nextArgReg++;
         } else if (arg->tag == Iex_GSPTR) {
            // TODO Implement?
            vassert(0); // ATC
            addInstr(env, RISCV64Instr_MV(argregs[nextArgReg], INVALID_HREG));
            nextArgReg++;
         } else if (arg->tag == Iex_VECRET) {
            /* Because of the go_fast logic above, we can't get here, since
               vector return values make us use the slow path instead. */
            vassert(0);
         } else
            return False; /* Unhandled arg type. */
      }

      /* Fast scheme only applies for unconditional calls. Hence: */
      cond = INVALID_HREG;

   } else {

      /* SLOW SCHEME; move via temporaries. */
      nextArgReg = 0;

      for (i = 0; i < n_args; i++) {
         IRExpr* arg = args[i];

         IRType aTy = Ity_INVALID;
         if (LIKELY(!is_IRExpr_VECRET_or_GSPTR(arg)))
            aTy = typeOfIRExpr(env->type_env, args[i]);

         if (nextArgReg >= RISCV64_N_ARGREGS)
            return False; /* Out of argregs. */

         if (aTy == Ity_I64) {
            tmpregs[nextArgReg] = iselIntExpr_R(env, args[i]);
            nextArgReg++;
         } else if (arg->tag == Iex_GSPTR) {
            // TODO Implement?
            vassert(0); // ATC
            tmpregs[nextArgReg] = INVALID_HREG;
            nextArgReg++;
         } else if (arg->tag == Iex_VECRET) {
            vassert(!hregIsInvalid(r_vecRetAddr));
            tmpregs[nextArgReg] = r_vecRetAddr;
            nextArgReg++;
         } else
            return False; /* Unhandled arg type. */
      }

      /* Compute the condition. Be a bit clever to handle the common case where
         the guard is 1:Bit. */
      cond = INVALID_HREG;
      if (guard) {
         if (guard->tag == Iex_Const && guard->Iex.Const.con->tag == Ico_U1 &&
             guard->Iex.Const.con->Ico.U1 == True) {
            /* Unconditional -- do nothing. */
         } else {
            cond = iselIntExpr_R(env, guard);
         }
      }

      /* Move the args to their final destinations. */
      for (i = 0; i < nextArgReg; i++) {
         vassert(!(hregIsInvalid(tmpregs[i])));
         addInstr(env, RISCV64Instr_MV(argregs[i], tmpregs[i]));
      }
   }

   /* Should be assured by checks above. */
   vassert(nextArgReg <= RISCV64_N_ARGREGS);

   /* Do final checks, set the return values, and generate the call instruction
      proper. */
   vassert(nGSPTRs == 0 || nGSPTRs == 1);
   vassert(nVECRETs == ((retTy == Ity_V128 || retTy == Ity_V256) ? 1 : 0));
   vassert(*stackAdjustAfterCall == 0);
   vassert(is_RetLoc_INVALID(*retloc));
   switch (retTy) {
   case Ity_INVALID:
      /* Function doesn't return a value. */
      *retloc = mk_RetLoc_simple(RLPri_None);
      break;
   case Ity_I64:
   case Ity_I32:
   case Ity_I16:
   case Ity_I8:
      *retloc = mk_RetLoc_simple(RLPri_Int);
      break;
   case Ity_V128:
      *retloc               = mk_RetLoc_spRel(RLPri_V128SpRel, 0);
      *stackAdjustAfterCall = 16;
      break;
   default:
      /* IR can denote other possible return types, but we don't handle those
         here. */
      vassert(0);
   }

   /* Finally, generate the call itself. This needs the *retloc value set in the
      switch above, which is why it's at the end. */

   /* nextArgReg doles out argument registers. Since these are assigned in the
      order x10/a0 .. x17/a7, its numeric value at this point, which must be
      between 0 and 8 inclusive, is going to be equal to the number of arg regs
      in use for the call. Hence bake that number into the call (we'll need to
      know it when doing register allocation, to know what regs the call reads.)
    */

   addInstr(env,
            RISCV64Instr_Call(*retloc, (Addr64)cee->addr, cond, nextArgReg));

   return True;
}

/*------------------------------------------------------------*/
/*--- ISEL: Integer expressions (64/32/16/8/1 bit)         ---*/
/*------------------------------------------------------------*/

/* Select insns for an integer-typed expression, and add them to the code list.
   Return a reg holding the result. This reg will be a virtual register. THE
   RETURNED REG MUST NOT BE MODIFIED. If you want to modify it, ask for a new
   vreg, copy it in there, and modify the copy. The register allocator will do
   its best to map both vregs to the same real register, so the copies will
   often disappear later in the game.

   This should handle expressions of 64, 32, 16, 8 and 1-bit type. All results
   are returned in a 64-bit register. For an N-bit expression, the upper 64-N
   bits are arbitrary, so you should mask or sign-extend partial values if
   necessary.

   The riscv64 backend however internally always extends the values as follows:
   * a 32/16/8-bit integer result is sign-extended to 64 bits,
   * a 1-bit logical result is zero-extended to 64 bits.

   This schema follows the approach taken by the RV64 ISA which by default
   sign-extends any 32/16/8-bit operation result to 64 bits. Matching the isel
   with the ISA generally results in requiring less instructions. For instance,
   it allows that any Ico_U32 immediate can be always materialized at maximum
   using two instructions (LUI+ADDIW).

   An important consequence of this design is that any Iop_<N>Sto64 extension is
   a no-op. On the other hand, any Iop_64to<N> operation must additionally
   perform an N-bit sign-extension. This is the opposite situation than in most
   other VEX backends.
*/

/* -------------------------- Reg --------------------------- */

/* DO NOT CALL THIS DIRECTLY ! */
static HReg iselIntExpr_R_wrk(ISelEnv* env, IRExpr* e)
{
   IRType ty = typeOfIRExpr(env->type_env, e);
   vassert(ty == Ity_I64 || ty == Ity_I32 || ty == Ity_I16 || ty == Ity_I8 ||
           ty == Ity_I1);

   switch (e->tag) {
   /* ------------------------ TEMP ------------------------- */
   case Iex_RdTmp: {
      return lookupIRTemp(env, e->Iex.RdTmp.tmp);
   }

   /* ------------------------ LOAD ------------------------- */
   case Iex_Load: {
      if (e->Iex.Load.end != Iend_LE)
         goto irreducible;

      HReg dst = newVRegI(env);
      /* TODO Optimize the cases with small imm Add64/Sub64. */
      HReg addr = iselIntExpr_R(env, e->Iex.Load.addr);

      if (ty == Ity_I64)
         addInstr(env, RISCV64Instr_LD(dst, addr, 0));
      else if (ty == Ity_I32)
         addInstr(env, RISCV64Instr_LW(dst, addr, 0));
      else if (ty == Ity_I16)
         addInstr(env, RISCV64Instr_LH(dst, addr, 0));
      else if (ty == Ity_I8)
         addInstr(env, RISCV64Instr_LB(dst, addr, 0));
      else
         goto irreducible;
      return dst;
   }

   /* ---------------------- BINARY OP ---------------------- */
   case Iex_Binop: {
      /* TODO Optimize for small imms by generating <instr>i. */
      switch (e->Iex.Binop.op) {
      case Iop_Add64: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_ADD(dst, argL, argR));
         return dst;
      }
      case Iop_Add32: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_ADDW(dst, argL, argR));
         return dst;
      }
      case Iop_Sub64: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_SUB(dst, argL, argR));
         return dst;
      }
      case Iop_Sub32: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_SUBW(dst, argL, argR));
         return dst;
      }
      case Iop_Xor64:
      case Iop_Xor32: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_XOR(dst, argL, argR));
         return dst;
      }
      case Iop_Or64:
      case Iop_Or32:
      case Iop_Or1: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_OR(dst, argL, argR));
         return dst;
      }
      case Iop_And64:
      case Iop_And32:
      case Iop_And1: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_AND(dst, argL, argR));
         return dst;
      }
      case Iop_Shl64: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_SLL(dst, argL, argR));
         return dst;
      }
      case Iop_Shl32: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_SLLW(dst, argL, argR));
         return dst;
      }
      case Iop_Shr64: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_SRL(dst, argL, argR));
         return dst;
      }
      case Iop_Shr32: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_SRLW(dst, argL, argR));
         return dst;
      }
      case Iop_Sar64: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_SRA(dst, argL, argR));
         return dst;
      }
      case Iop_Sar32: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_SRAW(dst, argL, argR));
         return dst;
      }
      case Iop_Mul64: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_MUL(dst, argL, argR));
         return dst;
      }
      case Iop_Mul32: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_MULW(dst, argL, argR));
         return dst;
      }
      case Iop_DivU64: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_DIVU(dst, argL, argR));
         return dst;
      }
      case Iop_DivU32: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_DIVUW(dst, argL, argR));
         return dst;
      }
      case Iop_DivS64: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_DIV(dst, argL, argR));
         return dst;
      }
      case Iop_DivS32: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_DIVW(dst, argL, argR));
         return dst;
      }
      case Iop_CmpEQ64:
      case Iop_CmpEQ32:
      case Iop_CasCmpEQ64:
      case Iop_CasCmpEQ32: {
         HReg tmp  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_SUB(tmp, argL, argR));
         HReg dst = newVRegI(env);
         addInstr(env, RISCV64Instr_SLTIU(dst, tmp, 1));
         return dst;
      }
      case Iop_CmpNE64:
      case Iop_CmpNE32:
      case Iop_CasCmpNE64:
      case Iop_CasCmpNE32: {
         HReg tmp  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_SUB(tmp, argL, argR));
         HReg dst = newVRegI(env);
         addInstr(env, RISCV64Instr_SLTU(dst, hregRISCV64_x0(), tmp));
         return dst;
      }
      case Iop_CmpLT64S:
      case Iop_CmpLT32S: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_SLT(dst, argL, argR));
         return dst;
      }
      case Iop_CmpLE64S:
      case Iop_CmpLE32S: {
         HReg tmp  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_SLT(tmp, argR, argL));
         HReg dst = newVRegI(env);
         addInstr(env, RISCV64Instr_SLTIU(dst, tmp, 1));
         return dst;
      }
      case Iop_CmpLT64U:
      case Iop_CmpLT32U: {
         HReg dst  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_SLTU(dst, argL, argR));
         return dst;
      }
      case Iop_CmpLE64U:
      case Iop_CmpLE32U: {
         HReg tmp  = newVRegI(env);
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         addInstr(env, RISCV64Instr_SLTU(tmp, argR, argL));
         HReg dst = newVRegI(env);
         addInstr(env, RISCV64Instr_SLTIU(dst, tmp, 1));
         return dst;
      }
      case Iop_Max32U: {
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         HReg cond = newVRegI(env);
         addInstr(env, RISCV64Instr_SLTU(cond, argL, argR));
         HReg dst = newVRegI(env);
         addInstr(env, RISCV64Instr_CSEL(dst, argR, argL, cond));
         return dst;
      }
      case Iop_32HLto64: {
         HReg hi32s = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg lo32s = iselIntExpr_R(env, e->Iex.Binop.arg2);

         HReg lo32_tmp = newVRegI(env);
         addInstr(env, RISCV64Instr_SLLI(lo32_tmp, lo32s, 32));
         HReg lo32 = newVRegI(env);
         addInstr(env, RISCV64Instr_SRLI(lo32, lo32_tmp, 32));

         HReg hi32 = newVRegI(env);
         addInstr(env, RISCV64Instr_SLLI(hi32, hi32s, 32));

         HReg dst = newVRegI(env);
         addInstr(env, RISCV64Instr_OR(dst, hi32, lo32));
         return dst;
      }
      case Iop_DivModS32to32: {
         /* TODO Improve in conjunction with Iop_64HIto32. */
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);

         HReg remw = newVRegI(env);
         addInstr(env, RISCV64Instr_REMW(remw, argL, argR));
         HReg remw_hi = newVRegI(env);
         addInstr(env, RISCV64Instr_SLLI(remw_hi, remw, 32));

         HReg divw = newVRegI(env);
         addInstr(env, RISCV64Instr_DIVW(divw, argL, argR));
         HReg divw_hi = newVRegI(env);
         addInstr(env, RISCV64Instr_SLLI(divw_hi, divw, 32));
         HReg divw_lo = newVRegI(env);
         addInstr(env, RISCV64Instr_SRLI(divw_lo, divw_hi, 32));

         HReg dst = newVRegI(env);
         addInstr(env, RISCV64Instr_OR(dst, remw_hi, divw_lo));
         return dst;
      }
      case Iop_DivModU32to32: {
         /* TODO Improve in conjunction with Iop_64HIto32. */
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);

         HReg remuw = newVRegI(env);
         addInstr(env, RISCV64Instr_REMUW(remuw, argL, argR));
         HReg remuw_hi = newVRegI(env);
         addInstr(env, RISCV64Instr_SLLI(remuw_hi, remuw, 32));

         HReg divuw = newVRegI(env);
         addInstr(env, RISCV64Instr_DIVUW(divuw, argL, argR));
         HReg divuw_hi = newVRegI(env);
         addInstr(env, RISCV64Instr_SLLI(divuw_hi, divuw, 32));
         HReg divuw_lo = newVRegI(env);
         addInstr(env, RISCV64Instr_SRLI(divuw_lo, divuw_hi, 32));

         HReg dst = newVRegI(env);
         addInstr(env, RISCV64Instr_OR(dst, remuw_hi, divuw_lo));
         return dst;
      }
      default:
         break;
      }

      break;
   }

   /* ---------------------- UNARY OP ----------------------- */
   case Iex_Unop: {
      switch (e->Iex.Unop.op) {
      case Iop_Not64:
      case Iop_Not32: {
         HReg dst = newVRegI(env);
         HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
         addInstr(env, RISCV64Instr_XORI(dst, src, -1));
         return dst;
      }
      case Iop_Not1: {
         HReg dst = newVRegI(env);
         HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
         addInstr(env, RISCV64Instr_SLTIU(dst, src, 1));
         return dst;
      }
      case Iop_8Uto32:
      case Iop_8Uto64:
      case Iop_16Uto64:
      case Iop_32Uto64: {
         UInt shift =
            64 - 8 * sizeofIRType(typeOfIRExpr(env->type_env, e->Iex.Unop.arg));
         HReg tmp = newVRegI(env);
         HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
         addInstr(env, RISCV64Instr_SLLI(tmp, src, shift));
         HReg dst = newVRegI(env);
         addInstr(env, RISCV64Instr_SRLI(dst, tmp, shift));
         return dst;
      }
      case Iop_1Sto32:
      case Iop_1Sto64: {
         HReg tmp = newVRegI(env);
         HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
         addInstr(env, RISCV64Instr_SLLI(tmp, src, 63));
         HReg dst = newVRegI(env);
         addInstr(env, RISCV64Instr_SRAI(dst, tmp, 63));
         return dst;
      }
      case Iop_1Uto64:
      case Iop_8Sto64:
      case Iop_16Sto64:
      case Iop_32Sto64:
         /* These are no-ops. */
         return iselIntExpr_R(env, e->Iex.Unop.arg);
      case Iop_64to8:
      case Iop_64to16:
      case Iop_64to32: {
         UInt shift = 64 - 8 * sizeofIRType(ty);
         HReg tmp = newVRegI(env);
         HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
         addInstr(env, RISCV64Instr_SLLI(tmp, src, shift));
         HReg dst = newVRegI(env);
         addInstr(env, RISCV64Instr_SRAI(dst, tmp, shift));
         return dst;
      }
      case Iop_128HIto64: {
         HReg rHi, rLo;
         iselInt128Expr(&rHi, &rLo, env, e->Iex.Unop.arg);
         return rHi; /* and abandon rLo */
      }
      case Iop_64HIto32: {
         HReg dst = newVRegI(env);
         HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
         addInstr(env, RISCV64Instr_SRAI(dst, src, 32));
         return dst;
      }
      case Iop_CmpNEZ8:
      case Iop_CmpNEZ32:
      case Iop_CmpNEZ64: {
         HReg dst = newVRegI(env);
         HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
         addInstr(env, RISCV64Instr_SLTU(dst, hregRISCV64_x0(), src));
         return dst;
      }
      case Iop_CmpwNEZ32:
      case Iop_CmpwNEZ64: {
         /* Use the fact that x | -x == 0 iff x == 0. Otherwise, either X or -X
            will have a 1 in the MSB. */
         HReg neg = newVRegI(env);
         HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
         addInstr(env, RISCV64Instr_SUB(neg, hregRISCV64_x0(), src));
         HReg or = newVRegI(env);
         addInstr(env, RISCV64Instr_OR(or, src, neg));
         HReg dst = newVRegI(env);
         addInstr(env, RISCV64Instr_SRAI(dst, or, 63));
         return dst;
      }
      case Iop_Left32:
      case Iop_Left64: {
         /* Left32/64(src) = src | -src. */
         HReg neg = newVRegI(env);
         HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
         addInstr(env, RISCV64Instr_SUB(neg, hregRISCV64_x0(), src));
         HReg dst = newVRegI(env);
         addInstr(env, RISCV64Instr_OR(dst, src, neg));
         return dst;
      }
      default:
         break;
      }

      break;
   }

   /* ------------------------- GET ------------------------- */
   case Iex_Get: {
      HReg dst  = newVRegI(env);
      HReg base = get_baseblock_register();
      Int  off  = e->Iex.Get.offset - BASEBLOCK_OFFSET_ADJUSTMENT;
      vassert(off >= -2048 && off < 2048);

      if (ty == Ity_I64)
         addInstr(env, RISCV64Instr_LD(dst, base, off));
      else if (ty == Ity_I32)
         addInstr(env, RISCV64Instr_LW(dst, base, off));
      else if (ty == Ity_I16)
         addInstr(env, RISCV64Instr_LH(dst, base, off));
      else if (ty == Ity_I8)
         addInstr(env, RISCV64Instr_LB(dst, base, off));
      else
         goto irreducible;
      return dst;
   }

   /* ----------------------- LITERAL ----------------------- */
   /* 64/32/16/8-bit literals. */
   case Iex_Const: {
      ULong u;
      HReg  dst = newVRegI(env);
      switch (e->Iex.Const.con->tag) {
      case Ico_U64:
         u = e->Iex.Const.con->Ico.U64;
         break;
      case Ico_U32:
         vassert(ty == Ity_I32);
         u = vex_sx_to_64(e->Iex.Const.con->Ico.U32, 32);
         break;
      case Ico_U16:
         vassert(ty == Ity_I16);
         u = vex_sx_to_64(e->Iex.Const.con->Ico.U16, 16);
         break;
      case Ico_U8:
         vassert(ty == Ity_I8);
         u = vex_sx_to_64(e->Iex.Const.con->Ico.U8, 8);
         break;
      default:
         goto irreducible;
      }
      addInstr(env, RISCV64Instr_LI(dst, u));
      return dst;
   }

   /* ---------------------- MULTIPLEX ---------------------- */
   case Iex_ITE: {
      /* ITE(ccexpr, iftrue, iffalse) */
      if (ty == Ity_I64 || ty == Ity_I32) {
         HReg dst     = newVRegI(env);
         HReg iftrue  = iselIntExpr_R(env, e->Iex.ITE.iftrue);
         HReg iffalse = iselIntExpr_R(env, e->Iex.ITE.iffalse);
         HReg cond    = iselIntExpr_R(env, e->Iex.ITE.cond);
         addInstr(env, RISCV64Instr_CSEL(dst, iftrue, iffalse, cond));
         return dst;
      }
      break;
   }

   default:
      break;
   }

   /* We get here if no pattern matched. */
irreducible:
   ppIRExpr(e);
   vpanic("iselIntExpr_R: cannot reduce tree");
}

static HReg iselIntExpr_R(ISelEnv* env, IRExpr* e)
{
   HReg r = iselIntExpr_R_wrk(env, e);

   /* Sanity checks ... */
   vassert(hregClass(r) == HRcInt64);
   vassert(hregIsVirtual(r));

   return r;
}

/*------------------------------------------------------------*/
/*--- ISEL: Integer expressions (128 bit)                  ---*/
/*------------------------------------------------------------*/

/* DO NOT CALL THIS DIRECTLY ! */
static void iselInt128Expr_wrk(HReg* rHi, HReg* rLo, ISelEnv* env, IRExpr* e)
{
   vassert(typeOfIRExpr(env->type_env, e) == Ity_I128);

   /* ---------------------- BINARY OP ---------------------- */
   if (e->tag == Iex_Binop) {
      switch (e->Iex.Binop.op) {
      /* 64 x 64 -> 128 multiply */
      case Iop_MullS64:
      case Iop_MullU64: {
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         *rHi      = newVRegI(env);
         *rLo      = newVRegI(env);
         if (e->Iex.Binop.op == Iop_MullS64)
            addInstr(env, RISCV64Instr_MULH(*rHi, argL, argR));
         else
            addInstr(env, RISCV64Instr_MULHU(*rHi, argL, argR));
         addInstr(env, RISCV64Instr_MUL(*rLo, argL, argR));
         return;
      }

      /* 64 x 64 -> (64(rem),64(div)) division */
      case Iop_DivModS64to64:
      case Iop_DivModU64to64: {
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         *rHi      = newVRegI(env);
         *rLo      = newVRegI(env);
         if (e->Iex.Binop.op == Iop_DivModS64to64) {
            addInstr(env, RISCV64Instr_REM(*rHi, argL, argR));
            addInstr(env, RISCV64Instr_DIV(*rLo, argL, argR));
         } else {
            addInstr(env, RISCV64Instr_REMU(*rHi, argL, argR));
            addInstr(env, RISCV64Instr_DIVU(*rLo, argL, argR));
         }
         return;
      }

      /* 64HLto128(e1,e2) */
      case Iop_64HLto128:
         *rHi = iselIntExpr_R(env, e->Iex.Binop.arg1);
         *rLo = iselIntExpr_R(env, e->Iex.Binop.arg2);
         return;

      default:
         break;
      }
   }

   ppIRExpr(e);
   vpanic("iselInt128Expr(riscv64)");
}

/* Compute a 128-bit value into a register pair, which is returned as the first
   two parameters. As with iselIntExpr_R, these may be either real or virtual
   regs; in any case they must not be changed by subsequent code emitted by the
   caller. */
static void iselInt128Expr(HReg* rHi, HReg* rLo, ISelEnv* env, IRExpr* e)
{
   iselInt128Expr_wrk(rHi, rLo, env, e);

   /* Sanity checks ... */
   vassert(hregClass(*rHi) == HRcInt64);
   vassert(hregIsVirtual(*rHi));
   vassert(hregClass(*rLo) == HRcInt64);
   vassert(hregIsVirtual(*rLo));
}

/*------------------------------------------------------------*/
/*--- ISEL: Statements                                     ---*/
/*------------------------------------------------------------*/

static void iselStmt(ISelEnv* env, IRStmt* stmt)
{
   if (vex_traceflags & VEX_TRACE_VCODE) {
      vex_printf("\n-- ");
      ppIRStmt(stmt);
      vex_printf("\n");
   }

   switch (stmt->tag) {
   /* ------------------------ STORE ------------------------ */
   /* Little-endian write to memory. */
   case Ist_Store: {
      IRType tyd = typeOfIRExpr(env->type_env, stmt->Ist.Store.data);
      if (tyd == Ity_I64 || tyd == Ity_I32 || tyd == Ity_I16 || tyd == Ity_I8) {
         HReg src = iselIntExpr_R(env, stmt->Ist.Store.data);
         /* TODO Optimize the cases with small imm Add64/Sub64. */
         HReg addr = iselIntExpr_R(env, stmt->Ist.Store.addr);

         if (tyd == Ity_I64)
            addInstr(env, RISCV64Instr_SD(src, addr, 0));
         else if (tyd == Ity_I32)
            addInstr(env, RISCV64Instr_SW(src, addr, 0));
         else if (tyd == Ity_I16)
            addInstr(env, RISCV64Instr_SH(src, addr, 0));
         else if (tyd == Ity_I8)
            addInstr(env, RISCV64Instr_SB(src, addr, 0));
         else
            vassert(0);
         return;
      }
      break;
   }

   /* ------------------------- PUT ------------------------- */
   /* Write guest state, fixed offset. */
   case Ist_Put: {
      IRType tyd = typeOfIRExpr(env->type_env, stmt->Ist.Put.data);
      if (tyd == Ity_I64 || tyd == Ity_I32 || tyd == Ity_I16 || tyd == Ity_I8) {
         HReg src  = iselIntExpr_R(env, stmt->Ist.Put.data);
         HReg base = get_baseblock_register();
         Int  off  = stmt->Ist.Put.offset - BASEBLOCK_OFFSET_ADJUSTMENT;
         vassert(off >= -2048 && off < 2048);

         if (tyd == Ity_I64)
            addInstr(env, RISCV64Instr_SD(src, base, off));
         else if (tyd == Ity_I32)
            addInstr(env, RISCV64Instr_SW(src, base, off));
         else if (tyd == Ity_I16)
            addInstr(env, RISCV64Instr_SH(src, base, off));
         else if (tyd == Ity_I8)
            addInstr(env, RISCV64Instr_SB(src, base, off));
         else
            vassert(0);
         return;
      }
      break;
   }

   /* ------------------------- TMP ------------------------- */
   /* Assign value to temporary. */
   case Ist_WrTmp: {
      IRType ty = typeOfIRTemp(env->type_env, stmt->Ist.WrTmp.tmp);
      if (ty == Ity_I64 || ty == Ity_I32 || ty == Ity_I16 || ty == Ity_I8 ||
          ty == Ity_I1) {
         HReg dst = lookupIRTemp(env, stmt->Ist.WrTmp.tmp);
         HReg src = iselIntExpr_R(env, stmt->Ist.WrTmp.data);
         addInstr(env, RISCV64Instr_MV(dst, src));
         return;
      }
      break;
   }

   /* ---------------- Call to DIRTY helper ----------------- */
   /* Call complex ("dirty") helper function. */
   case Ist_Dirty: {
      IRDirty* d = stmt->Ist.Dirty.details;

      /* Figure out the return type, if any. */
      IRType retty = Ity_INVALID;
      if (d->tmp != IRTemp_INVALID)
         retty = typeOfIRTemp(env->type_env, d->tmp);

      Bool retty_ok;
      switch (retty) {
      case Ity_INVALID: /* Function doesn't return anything. */
      case Ity_I64:
      case Ity_I32:
      case Ity_I16:
      case Ity_I8:
         retty_ok = True;
         break;
      default:
         retty_ok = False;
         break;
      }
      if (!retty_ok)
         break;

      /* Marshal args, do the call, and set the return value to 0x555..555 if
         this is a conditional call that returns a value and the call is
         skipped. */
      UInt   addToSp = 0;
      RetLoc rloc    = mk_RetLoc_INVALID();
      doHelperCall(&addToSp, &rloc, env, d->guard, d->cee, retty, d->args);
      vassert(is_sane_RetLoc(rloc));

      /* Now figure out what to do with the returned value, if any. */
      switch (retty) {
      case Ity_INVALID: {
         /* No return value. Nothing to do. */
         vassert(d->tmp == IRTemp_INVALID);
         vassert(rloc.pri == RLPri_None);
         vassert(addToSp == 0);
         return;
      }
      case Ity_I64: {
         vassert(rloc.pri == RLPri_Int);
         vassert(addToSp == 0);
         /* The returned value is in x10/a0. Park it in the register associated
            with tmp. */
         HReg dst = lookupIRTemp(env, d->tmp);
         addInstr(env, RISCV64Instr_MV(dst, hregRISCV64_x10()));
         return;
      }
      case Ity_I32: {
         vassert(rloc.pri == RLPri_Int);
         vassert(addToSp == 0);
         /* Sign-extend the value returned from the helper as is expected by the
            rest of the backend. */
         HReg dst = lookupIRTemp(env, d->tmp);
         addInstr(env, RISCV64Instr_ADDIW(dst, hregRISCV64_x10(), 0));
         return;
      }
      case Ity_I16:
      case Ity_I8: {
         vassert(rloc.pri == RLPri_Int);
         vassert(addToSp == 0);
         HReg dst   = lookupIRTemp(env, d->tmp);
         UInt shift = 64 - 8 * sizeofIRType(retty);
         HReg tmp   = newVRegI(env);
         addInstr(env, RISCV64Instr_SLLI(tmp, hregRISCV64_x10(), shift));
         addInstr(env, RISCV64Instr_SRAI(dst, tmp, shift));
         return;
      }
      default:
         vassert(0);
      }
      break;
   }

   /* ---------- Load Linked and Store Conditional ---------- */
   case Ist_LLSC: {
      if (stmt->Ist.LLSC.storedata == NULL) {
         /* LL */
         IRTemp res = stmt->Ist.LLSC.result;
         IRType ty  = typeOfIRTemp(env->type_env, res);
         if (ty == Ity_I32) {
            HReg r_dst  = lookupIRTemp(env, res);
            HReg r_addr = iselIntExpr_R(env, stmt->Ist.LLSC.addr);
            addInstr(env, RISCV64Instr_LR_W(r_dst, r_addr));
            return;
         }
      } else {
         /* SC */
         IRType tyd = typeOfIRExpr(env->type_env, stmt->Ist.LLSC.storedata);
         if (tyd == Ity_I32) {
            HReg r_tmp  = newVRegI(env);
            HReg r_src  = iselIntExpr_R(env, stmt->Ist.LLSC.storedata);
            HReg r_addr = iselIntExpr_R(env, stmt->Ist.LLSC.addr);
            addInstr(env, RISCV64Instr_SC_W(r_tmp, r_src, r_addr));

            /* Now r_tmp is non-zero if failed, 0 if success. Change to IR
               conventions (0 is fail, 1 is success). */
            IRTemp res   = stmt->Ist.LLSC.result;
            HReg   r_res = lookupIRTemp(env, res);
            IRType ty    = typeOfIRTemp(env->type_env, res);
            vassert(ty == Ity_I1);
            addInstr(env, RISCV64Instr_SLTIU(r_res, r_tmp, 1));
            return;
         }
      }
      break;
   }

   /* ------------------------ ACAS ------------------------- */
   case Ist_CAS: {
      if (stmt->Ist.CAS.details->oldHi == IRTemp_INVALID) {
         /* "Normal" singleton CAS. */
         IRCAS* cas = stmt->Ist.CAS.details;
         IRType tyd = typeOfIRTemp(env->type_env, cas->oldLo);
         if (tyd == Ity_I64 || tyd == Ity_I32) {
            HReg old  = lookupIRTemp(env, cas->oldLo);
            HReg addr = iselIntExpr_R(env, cas->addr);
            HReg expd = iselIntExpr_R(env, cas->expdLo);
            HReg data = iselIntExpr_R(env, cas->dataLo);
            if (tyd == Ity_I64)
               addInstr(env, RISCV64Instr_CAS_D(old, addr, expd, data));
            else
               addInstr(env, RISCV64Instr_CAS_W(old, addr, expd, data));
            return;
         }
      }
      break;
   }

   /* ---------------------- MEM FENCE ---------------------- */
   case Ist_MBE:
      switch (stmt->Ist.MBE.event) {
      case Imbe_Fence:
         addInstr(env, RISCV64Instr_FENCE());
         return;
      default:
         break;
      }
      break;

   /* --------------------- INSTR MARK ---------------------- */
   /* Doesn't generate any executable code ... */
   case Ist_IMark:
      return;

   /* ------------------------ NO-OP ------------------------ */
   case Ist_NoOp:
      return;

   /* ------------------------ EXIT ------------------------- */
   case Ist_Exit: {
      if (stmt->Ist.Exit.dst->tag != Ico_U64)
         vpanic("iselStmt(riscv64): Ist_Exit: dst is not a 64-bit value");

      HReg cond   = iselIntExpr_R(env, stmt->Ist.Exit.guard);
      HReg base   = get_baseblock_register();
      Int  soff12 = stmt->Ist.Exit.offsIP - BASEBLOCK_OFFSET_ADJUSTMENT;
      vassert(soff12 >= -2048 && soff12 < 2048);

      /* Case: boring transfer to known address. */
      if (stmt->Ist.Exit.jk == Ijk_Boring) {
         if (env->chainingAllowed) {
            /* .. almost always true .. */
            /* Skip the event check at the dst if this is a forwards edge. */
            Bool toFastEP = (Addr64)stmt->Ist.Exit.dst->Ico.U64 > env->max_ga;
            if (0)
               vex_printf("%s", toFastEP ? "Y" : ",");
            addInstr(env, RISCV64Instr_XDirect(stmt->Ist.Exit.dst->Ico.U64,
                                               base, soff12, cond, toFastEP));
         } else {
            /* .. very occasionally .. */
            /* We can't use chaining, so ask for an assisted transfer, as
               that's the only alternative that is allowable. */
            HReg r = iselIntExpr_R(env, IRExpr_Const(stmt->Ist.Exit.dst));
            addInstr(env,
                     RISCV64Instr_XAssisted(r, base, soff12, cond, Ijk_Boring));
         }
         return;
      }

      /* Case: assisted transfer to arbitrary address. */
      switch (stmt->Ist.Exit.jk) {
      /* Keep this list in sync with that for iselNext below. */
      case Ijk_ClientReq:
      case Ijk_NoDecode:
      case Ijk_NoRedir:
      case Ijk_Sys_syscall:
      case Ijk_InvalICache:
      case Ijk_FlushDCache:
      case Ijk_SigTRAP:
      case Ijk_Yield: {
         HReg r = iselIntExpr_R(env, IRExpr_Const(stmt->Ist.Exit.dst));
         addInstr(env, RISCV64Instr_XAssisted(r, base, soff12, cond,
                                              stmt->Ist.Exit.jk));
         return;
      }
      default:
         break;
      }

      /* Do we ever expect to see any other kind? */
      goto stmt_fail;
   }

   default:
      break;
   }

stmt_fail:
   ppIRStmt(stmt);
   vpanic("iselStmt");
}

/*------------------------------------------------------------*/
/*--- ISEL: Basic block terminators (Nexts)                ---*/
/*------------------------------------------------------------*/

static void iselNext(ISelEnv* env, IRExpr* next, IRJumpKind jk, Int offsIP)
{
   if (vex_traceflags & VEX_TRACE_VCODE) {
      vex_printf("\n-- PUT(%d) = ", offsIP);
      ppIRExpr(next);
      vex_printf("; exit-");
      ppIRJumpKind(jk);
      vex_printf("\n");
   }

   HReg base   = get_baseblock_register();
   Int  soff12 = offsIP - BASEBLOCK_OFFSET_ADJUSTMENT;
   vassert(soff12 >= -2048 && soff12 < 2048);

   /* Case: boring transfer to known address. */
   if (next->tag == Iex_Const) {
      IRConst* cdst = next->Iex.Const.con;
      vassert(cdst->tag == Ico_U64);
      if (jk == Ijk_Boring || jk == Ijk_Call) {
         /* Boring transfer to known address. */
         if (env->chainingAllowed) {
            /* .. almost always true .. */
            /* Skip the event check at the dst if this is a forwards edge. */
            Bool toFastEP = (Addr64)cdst->Ico.U64 > env->max_ga;
            if (0)
               vex_printf("%s", toFastEP ? "X" : ".");
            addInstr(env, RISCV64Instr_XDirect(cdst->Ico.U64, base, soff12,
                                               INVALID_HREG, toFastEP));
         } else {
            /* .. very occasionally .. */
            /* We can't use chaining, so ask for an assisted transfer, as that's
               the only alternative that is allowable. */
            HReg r = iselIntExpr_R(env, next);
            addInstr(env, RISCV64Instr_XAssisted(r, base, soff12, INVALID_HREG,
                                                 Ijk_Boring));
         }
         return;
      }
   }

   /* Case: call/return (==boring) transfer to any address. */
   switch (jk) {
   case Ijk_Boring:
   case Ijk_Ret:
   case Ijk_Call: {
      HReg r = iselIntExpr_R(env, next);
      if (env->chainingAllowed)
         addInstr(env, RISCV64Instr_XIndir(r, base, soff12, INVALID_HREG));
      else
         addInstr(env, RISCV64Instr_XAssisted(r, base, soff12, INVALID_HREG,
                                              Ijk_Boring));
      return;
   }
   default:
      break;
   }

   /* Case: assisted transfer to arbitrary address. */
   switch (jk) {
   /* Keep this list in sync with that for Ist_Exit above. */
   case Ijk_ClientReq:
   case Ijk_NoDecode:
   case Ijk_NoRedir:
   case Ijk_Sys_syscall:
   case Ijk_InvalICache:
   case Ijk_FlushDCache:
   case Ijk_SigTRAP:
   case Ijk_Yield: {
      HReg r = iselIntExpr_R(env, next);
      addInstr(env, RISCV64Instr_XAssisted(r, base, soff12, INVALID_HREG, jk));
      return;
   }
   default:
      break;
   }

   vex_printf("\n-- PUT(%d) = ", offsIP);
   ppIRExpr(next);
   vex_printf("; exit-");
   ppIRJumpKind(jk);
   vex_printf("\n");
   vassert(0); /* Are we expecting any other kind? */
}

/*------------------------------------------------------------*/
/*--- Insn selector top-level                              ---*/
/*------------------------------------------------------------*/

/* Translate an entire SB to riscv64 code. */

HInstrArray* iselSB_RISCV64(const IRSB*        bb,
                            VexArch            arch_host,
                            const VexArchInfo* archinfo_host,
                            const VexAbiInfo*  vbi /*UNUSED*/,
                            Int                offs_Host_EvC_Counter,
                            Int                offs_Host_EvC_FailAddr,
                            Bool               chainingAllowed,
                            Bool               addProfInc,
                            Addr               max_ga)
{
   Int      i, j;
   HReg     hreg, hregHI;
   ISelEnv* env;

   /* Do some sanity checks. */
   vassert(arch_host == VexArchRISCV64);

   /* Check that the host's endianness is as expected. */
   vassert(archinfo_host->endness == VexEndnessLE);

   /* Guard against unexpected space regressions. */
   vassert(sizeof(RISCV64Instr) <= 32);

   /* Make up an initial environment to use. */
   env           = LibVEX_Alloc_inline(sizeof(ISelEnv));
   env->vreg_ctr = 0;

   /* Set up output code array. */
   env->code = newHInstrArray();

   /* Copy BB's type env. */
   env->type_env = bb->tyenv;

   /* Make up an IRTemp -> virtual HReg mapping. This doesn't change as we go
      along. */
   env->n_vregmap = bb->tyenv->types_used;
   env->vregmap   = LibVEX_Alloc_inline(env->n_vregmap * sizeof(HReg));
   env->vregmapHI = LibVEX_Alloc_inline(env->n_vregmap * sizeof(HReg));

   /* and finally ... */
   env->chainingAllowed = chainingAllowed;
   env->hwcaps          = archinfo_host->hwcaps;
   env->previous_rm     = NULL;
   env->max_ga          = max_ga;

   /* For each IR temporary, allocate a suitably-kinded virtual register. */
   j = 0;
   for (i = 0; i < env->n_vregmap; i++) {
      hregHI = hreg = INVALID_HREG;
      switch (bb->tyenv->types[i]) {
      case Ity_I1:
      case Ity_I8:
      case Ity_I16:
      case Ity_I32:
      case Ity_I64:
         hreg = mkHReg(True, HRcInt64, 0, j++);
         break;
      case Ity_I128:
         hreg   = mkHReg(True, HRcInt64, 0, j++);
         hregHI = mkHReg(True, HRcInt64, 0, j++);
         break;
      default:
         ppIRType(bb->tyenv->types[i]);
         vpanic("iselBB(riscv64): IRTemp type");
      }
      env->vregmap[i]   = hreg;
      env->vregmapHI[i] = hregHI;
   }
   env->vreg_ctr = j;

   /* The very first instruction must be an event check. */
   HReg base             = get_baseblock_register();
   Int  soff12_amCounter = offs_Host_EvC_Counter - BASEBLOCK_OFFSET_ADJUSTMENT;
   vassert(soff12_amCounter >= -2048 && soff12_amCounter < 2048);
   Int soff12_amFailAddr = offs_Host_EvC_FailAddr - BASEBLOCK_OFFSET_ADJUSTMENT;
   vassert(soff12_amFailAddr >= -2048 && soff12_amFailAddr < 2048);
   addInstr(env, RISCV64Instr_EvCheck(base, soff12_amCounter, base,
                                      soff12_amFailAddr));

   /* TODO */
#if 0
   /* Possibly a block counter increment (for profiling). At this point we don't
      know the address of the counter, so just pretend it is zero. It will have
      to be patched later, but before this translation is used, by a call to
      LibVEX_patchProfCtr(). */
   if (addProfInc)
      addInstr(env, ARM64Instr_ProfInc());
#endif

   /* Ok, finally we can iterate over the statements. */
   for (i = 0; i < bb->stmts_used; i++)
      iselStmt(env, bb->stmts[i]);

   iselNext(env, bb->next, bb->jumpkind, bb->offsIP);

   /* Record the number of vregs we used. */
   env->code->n_vregs = env->vreg_ctr;
   return env->code;
}

/*--------------------------------------------------------------------*/
/*--- end                                      host_riscv64_isel.c ---*/
/*--------------------------------------------------------------------*/
