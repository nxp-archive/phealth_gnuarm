;; Machine description of the ARC ARCompact cpu for GNU C compiler
;; Copyright (C) 1994, 1997, 1999, 2006, 2007, 2008, 2009
;; Free Software Foundation, Inc.

;;    Sources derived from work done by Sankhya Technologies (www.sankhya.com)

;;    Position Independent Code support added,Code cleaned up, 
;;    Comments and Support For ARC700 instructions added by
;;    Saurabh Verma (saurabh.verma@codito.com)
;;    Ramana Radhakrishnan(ramana.radhakrishnan@codito.com)
;;
;;    Profiling support and performance improvements by
;;    Joern Rennecke (joern.rennecke@arc.com)
;;
;;    Support for DSP multiply instructions and mul64
;;    instructions for ARC600; and improvements in flag setting
;;    instructions by
;;    Muhammad Khurram Riaz (Khurram.Riaz@arc.com)

;; This file is part of GCC.

;; GCC is free software; you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation; either version 3, or (at your option)
;; any later version.

;; GCC is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with GCC; see the file COPYING3.  If not see
;; <http://www.gnu.org/licenses/>.

;; See file "rtl.def" for documentation on define_insn, match_*, et. al.

;; <op> dest, src         Two operand instruction's syntax
;; <op> dest, src1, src2  Three operand instruction's syntax

;; ARC and ARCompact PREDICATES:
;;
;;   comparison_operator   LT, GT, LE, GE, LTU, GTU, LEU, GEU, EQ, NE
;;   memory_operand        memory                         [m]
;;   immediate_operand     immediate constant             [IJKLMNOP]
;;   register_operand      register                       [rq]
;;   general_operand       register, memory, constant     [rqmIJKLMNOP]

;;  Note that the predicates are only used when selecting a pattern
;;  to determine if an operand is valid.

;;  The constraints then select which of the possible valid operands
;;  is present (and guide register selection). The actual assembly
;;  instruction is then selected on the basis of the constraints.

;; ARC and ARCompact CONSTRAINTS:
;;
;;   b  stack pointer                           r28
;;   f  frame pointer                           r27
;;   Rgp global pointer                         r26
;;   g  general reg, memory, constant
;;   m  memory
;;   p  memory address
;;   q  registers commonly used in
;;      16-bit insns                            r0-r3, r12-r15
;;   c  core registers				r0-r60, ap, pcl
;;   r  general registers                       r0-r28, blink, ap, pcl
;;
;;   H  fp 16-bit constant
;;   I signed 12-bit immediate (for ARCompact)
;;      signed 9-bit immediate (for ARCtangent-A4)
;;   J  long immediate (signed 32-bit immediate)
;;   K  unsigned 3-bit immediate (for ARCompact)
;;   L  unsigned 6-bit immediate (for ARCompact)
;;   M  unsinged 5-bit immediate (for ARCompact)
;;   O  unsinged 7-bit immediate (for ARCompact)
;;   P  unsinged 8-bit immediate (for ARCompact)
;;   N  constant '1' (for ARCompact)


;;  ashwin : include options.h from build dir
;; (include "arc.c")


;; TODO:
;; -> Supporting arith/logic insns which update the status flag based on the
;;    operation result (i.e <op>.f type insns).
;; -> conditional jump Jcc
;; -> prefetch instruction
;; -> rsub insn

;;  -----------------------------------------------------------------------------

;; Include DFA scheduluers
(include ("arc600.md"))
(include ("arc700.md"))

;; Predicates

(include ("predicates.md"))
(include ("constraints.md"))
;;  -----------------------------------------------------------------------------

;; UNSPEC Usage:
;; ~~~~~~~~~~~~
;;  -----------------------------------------------------------------------------
;;  Symbolic name  Value              Desc.
;;  -----------------------------------------------------------------------------
;;  UNSPEC_PLT       3        symbol to be referenced through the PLT
;;  UNSPEC_GOT       4        symbol to be rerenced through the GOT
;;  UNSPEC_GOTOFF    5        Local symbol.To be referenced relative to the
;;                            GOTBASE.(Referenced as @GOTOFF)
;;  ----------------------------------------------------------------------------


(define_constants
  [(UNSPEC_NORM 11) ; norm generation through builtins. candidate for scheduling
   (UNSPEC_NORMW 12) ; normw generation through builtins. candidate for scheduling
   (UNSPEC_SWAP 13) ; swap generation through builtins. candidate for scheduling
   (UNSPEC_MUL64 14) ; mul64 generation through builtins. candidate for scheduling
   (UNSPEC_MULU64 15) ; mulu64 generation through builtins. candidate for scheduling
   (UNSPEC_DIVAW 16) ; divaw generation through builtins. candidate for scheduling
   (UNSPEC_DIRECT 17)
   (UNSPEC_PROF 18) ; profile callgraph counter
   (UNSPEC_LP 19) ; to set LP_END
   (UNSPEC_CASESI 20)
   (VUNSPEC_RTIE 17) ; blockage insn for rtie generation
   (VUNSPEC_SYNC 18) ; blockage insn for sync generation
   (VUNSPEC_BRK 19) ; blockage insn for brk generation
   (VUNSPEC_FLAG 20) ; blockage insn for flag generation
   (VUNSPEC_SLEEP 21) ; blockage insn for sleep generation
   (VUNSPEC_SWI 22) ; blockage insn for swi generation
   (VUNSPEC_CORE_READ 23) ; blockage insn for reading a core register 
   (VUNSPEC_CORE_WRITE 24) ; blockage insn for writing to a core register 
   (VUNSPEC_LR 25) ; blockage insn for reading an auxillary register 
   (VUNSPEC_SR 26) ; blockage insn for writing to an auxillary register 
   (VUNSPEC_TRAP_S 27) ; blockage insn for trap_s generation
   (VUNSPEC_UNIMP_S 28) ; blockage insn for unimp_s generation

   (SP_REG 28)
   (ILINK1_REGNUM 29)
   (ILINK2_REGNUM 30)
   (RETURN_ADDR_REGNUM 31)
   (LP_COUNT 60)
   (CC_REG 61)
   (LP_START 144)
   (LP_END 145)
  ]
)

(define_attr "is_sfunc" "no,yes" (const_string "no"))

;; Insn type.  Used to default other attribute values.

(define_attr "type"
  "move,load,store,cmove,unary,binary,compare,shift,uncond_branch,jump,branch,
   brcc,brcc_no_delay_slot,call,sfunc,call_no_delay_slot,
   multi,umulti, two_cycle_core,lr,sr,divaw,loop_setup,loop_end,return,
   misc,spfp,dpfp_mult,dpfp_addsub,mulmac_600,cc_arith,
   simd_vload, simd_vload128, simd_vstore, simd_vmove, simd_vmove_else_zero,
   simd_vmove_with_acc, simd_varith_1cycle, simd_varith_2cycle, 
   simd_varith_with_acc, simd_vlogic, simd_vlogic_with_acc,
   simd_vcompare, simd_vpermute, simd_vpack, simd_vpack_with_acc,
   simd_valign, simd_valign_with_acc, simd_vcontrol,
   simd_vspecial_3cycle, simd_vspecial_4cycle, simd_dma"
  (cond [(eq_attr "is_sfunc" "yes")
	 (cond [(eq (symbol_ref "TARGET_LONG_CALLS_SET") (const_int 0))
		(const_string "call")
		(ne (symbol_ref "flag_pic") (const_int 0))
		(const_string "sfunc")]
	       (const_string "call_no_delay_slot"))]
	(const_string "binary")))

;; The following three attributes are mixed case so that they can be
;; used conveniently with the CALL_ATTR macro.
(define_attr "is_CALL" "no,yes"
  (cond [(eq_attr "is_sfunc" "yes") (const_string "yes")
	 (eq_attr "type" "call,call_no_delay_slot") (const_string "yes")]
	(const_string "no")))

(define_attr "is_SIBCALL" "no,yes" (const_string "no"))

(define_attr "is_NON_SIBCALL" "no,yes"
  (cond [(eq_attr "is_SIBCALL" "yes") (const_string "no")
	 (eq_attr "is_CALL" "yes") (const_string "yes")]
	(const_string "no")))
  

;; Attribute describing the processor
(define_attr "cpu" "A4,A5,ARC600,ARC700"
  (const (symbol_ref "arc_cpu_attr")))

;; true for compact instructions (those with _s suffix)
;; "maybe" means compact unless we conditionalize the insn.
(define_attr "iscompact" "true,maybe,true_limm,maybe_limm,false"
  (cond [(eq_attr "type" "sfunc")
         (const_string "maybe")]
        (const_string "false")))


;; Condition codes: this one is used by final_prescan_insn to speed up
;; conditionalizing instructions.  It saves having to scan the rtl to see if
;; it uses or alters the condition codes.

;; USE: This insn uses the condition codes (eg: a conditional branch).
;; CANUSE: This insn can use the condition codes (for conditional execution).
;; SET: All condition codes are set by this insn.
;; SET_ZN: the Z and N flags are set by this insn.
;; SET_ZNC: the Z, N, and C flags are set by this insn.
;; CLOB: The condition codes are set to unknown values by this insn.
;; NOCOND: This insn can't use and doesn't affect the condition codes.

(define_attr "cond" "use,canuse,canuse_limm,canuse_limm_add,set,set_zn,clob,nocond"
  (if_then_else (eq_attr "cpu" "A4")
      (cond [(and (eq_attr "type" "unary,move")
                  (match_operand 1 "register_operand" ""))
             (const_string "canuse")

             (and (eq_attr "type" "binary")
                  (match_operand 2 "register_operand" ""))
             (const_string "canuse")

             (eq_attr "type" "compare")
             (const_string "set")

             (eq_attr "type" "cmove,branch")
             (const_string "use")

             (eq_attr "type" "multi,misc,shift")
             (const_string "clob")
            ]

            (const_string "nocond"))

      (if_then_else (eq_attr "iscompact" "maybe,false")
          (cond [(eq_attr "type" "unary,move")
                 (if_then_else
                    (ior (match_operand 1 "u6_immediate_operand" "")
                         (match_operand 1 "long_immediate_operand" ""))
                    (const_string "canuse")
                    (const_string "nocond"))

		 (eq_attr "type" "binary")
		 (cond [(ne (symbol_ref "REGNO (operands[0])")
                            (symbol_ref "REGNO (operands[1])"))
			(const_string "nocond")
			(match_operand 2 "register_operand" "")
			(const_string "canuse")
                        (match_operand 2 "u6_immediate_operand" "")
			(const_string "canuse")
                        (match_operand 2 "long_immediate_operand" "")
			(const_string "canuse")
                        (match_operand 2 "const_int_operand" "")
			(const_string "canuse_limm")]
		       (const_string "nocond"))

                 (eq_attr "type" "compare")
                 (const_string "set")

                 (eq_attr "type" "cmove,branch")
                 (const_string "use")

		 (eq_attr "is_sfunc" "yes")
		 (cond [(ne (symbol_ref "(TARGET_MEDIUM_CALLS
					  && !TARGET_LONG_CALLS_SET
					  && flag_pic)")
			    (const_int 0))
		        (const_string "canuse_limm_add")
			(ne (symbol_ref "(TARGET_MEDIUM_CALLS
					  && !TARGET_LONG_CALLS_SET)")
			    (const_int 0))
		        (const_string "canuse_limm")]
		       (const_string "canuse"))

                ]

                (const_string "nocond"))

          (cond [(eq_attr "type" "compare")
                 (const_string "set")

                 (eq_attr "type" "cmove,branch")
                 (const_string "use")

                ]

                (const_string "nocond")))))

(define_attr "verify_short" "no,yes"
  (if_then_else
    (ne (symbol_ref "arc_verify_short (insn, insn_current_address & 2, 0)")
	(const_int 0))
    (const_string "yes") (const_string "no")))

; Is there an instruction that we are actually putting into the delay slot?
(define_attr "delay_slot_filled" "no,yes"
  (cond [(ne (symbol_ref "NEXT_INSN (PREV_INSN (insn)) == insn") (const_int 0))
	 (const_string "no")
	 (ne (symbol_ref "!TARGET_AT_DBR_CONDEXEC
			  && INSN_ANNULLED_BRANCH_P (insn)
			  && !INSN_FROM_TARGET_P (NEXT_INSN (insn))")
	     (const_int 0))
	 (const_string "no")]
	(const_string "yes")))

; Is a delay slot present for purposes of shorten_branches?
; We have to take the length of this insn into account for forward branches
; even if we don't put the insn actually into a delay slot.
(define_attr "delay_slot_present" "no,yes"
  (cond [(ne (symbol_ref "NEXT_INSN (PREV_INSN (insn)) == insn") (const_int 0))
	 (const_string "no")]
	(const_string "yes")))

; We can't use get_attr_length (NEXT_INSN (insn)) because this gives the
; length of a different insn with the same uid.
(define_attr "delay_slot_length" ""
  (cond [(ne (symbol_ref "NEXT_INSN (PREV_INSN (insn)) == insn") (const_int 0))
	 (const_int 0)]
	(symbol_ref "get_attr_length (NEXT_INSN (PREV_INSN (insn)))
		     - get_attr_length (insn)")))


;; The length variations of ARCompact can't be expressed with the traditional
;; mechanisms in a way that avoids infinite looping in shorten_branches.
;; Given a set of potentially short insns, we could express the effect of
;; arc_verify_short returing zero as an alignment of the current or the
;; following insn, depending on alignment.
;; However, which branch / jump insns are potentially short in turn depends
;; on instruction length.
(define_attr "lock_length" "" (const_int 0))

;; Length (in # of bytes, long immediate constants counted too).
;; ??? There's a nasty interaction between the conditional execution fsm
;; and insn lengths: insns with shimm values cannot be conditionally executed.
(define_attr "length" ""
  (cond
   [(eq_attr "lock_length" "!0") (const_int 2)
    (eq_attr "cpu" "A4")
      (cond [(eq_attr "type" "load")
             (if_then_else
                 (match_operand 1 "long_immediate_loadstore_operand" "")
                 (const_int 8) (const_int 4))

             (eq_attr "type" "store")
             (if_then_else
                 (match_operand 0 "long_immediate_loadstore_operand" "")
                 (const_int 8) (const_int 4))

             (eq_attr "type" "move,unary,compare")
             (if_then_else (match_operand 1 "long_immediate_operand" "")
                           (const_int 8) (const_int 4))

             (eq_attr "type" "binary")
             (if_then_else (match_operand 2 "long_immediate_operand" "")
                           (const_int 8) (const_int 4))

             (eq_attr "type" "cmove")
             (if_then_else (match_operand 1 "register_operand" "")
                           (const_int 4) (const_int 8)) 

             (eq_attr "type" "multi") (const_int 8)
		     (eq_attr "cond" "set,set_zn,clob") (const_int 8)
            ]

            (const_int 4))

    (eq_attr "iscompact" "true,maybe")
    ; The length can vary because of ADJUST_INSN_LENGTH.
    ; make sure that variable_length_p will be true.
    (cond
      [(and (ne (symbol_ref "0") (const_int 0)) (eq (match_dup 0) (pc)))
       (const_int 4)
       (eq_attr "verify_short" "yes")
       (const_int 2)]
      (const_int 4))
  
    (eq_attr "iscompact" "true_limm,maybe_limm")
    (cond
      [(and (ne (symbol_ref "0") (const_int 0)) (eq (match_dup 0) (pc)))
       (const_int 8)
       (eq_attr "verify_short" "yes")
       (const_int 6)]
      (const_int 8))
]

          (cond [(eq_attr "type" "load")
                 (if_then_else
                    (match_operand 1 "long_immediate_loadstore_operand" "")
                    (const_int 8) (const_int 4))

                 (eq_attr "type" "store")
                 (if_then_else
                   (ior (match_operand 0 "long_immediate_loadstore_operand" "")
			(match_operand 1 "immediate_operand" ""))
                   (const_int 8) (const_int 4))

                 (eq_attr "type" "move,unary")
                 (if_then_else (match_operand 1 "long_immediate_operand" "")
                               (const_int 8) (const_int 4))

;; Added this for adjusting length of nops, for bbit offset calculation to be correct

	         (eq_attr "type" "compare")
                 (if_then_else (match_operand 1 "long_immediate_operand" "")
 			       (if_then_else (eq_attr "cpu" "ARC700") 
 					     (const_int 8) (const_int 12))
;			       (const_int 8) 
 			       (if_then_else (eq_attr "cpu" "ARC700") 
 					    (const_int 4) (const_int 8)))


		 (and (eq_attr "type" "shift")
		      (match_operand 1 "immediate_operand"))
		 (const_int 8)
                 (eq_attr "type" "binary,shift")
                 (if_then_else
                    (ior (match_operand 2 "long_immediate_operand" "")
                         (and (ne (symbol_ref "REGNO(operands[0])")
                                  (symbol_ref "REGNO(operands[1])"))
                              (eq (match_operand 2 "u6_immediate_operand" "")
                                  (const_int 0))))
                            
                    (const_int 8) (const_int 4))

                 (eq_attr "type" "cmove")
                    (if_then_else (match_operand 1 "register_operand" "")
                                  (const_int 4) (const_int 8)) 

                 (eq_attr "type" "call_no_delay_slot") (const_int 8)
                ]

                (const_int 4))
))

;; The length here is the length of a single asm.  Unfortunately it might be
;; 4 or 8 so we must allow for 8.  That's ok though.  How often will users
;; lament asm's not being put in delay slots?
;;
(define_asm_attributes
  [(set_attr "length" "8")
   (set_attr "type" "multi")
   (set_attr "cond" "clob") ])

;; Delay slots.
;; The first two cond clauses and the default are necessary for correctness;
;; the remaining cond clause is mainly an optimization, as otherwise nops
;; would be inserted; however, if we didn't do this optimization, we would
;; have to be more conservative in our length calculations.

(define_attr "in_delay_slot" "false,true"
  (cond [(eq_attr "type" "uncond_branch,jump,branch,
			  call,sfunc,call_no_delay_slot, 
                          brcc, brcc_no_delay_slot,loop_setup,loop_end")
	 (const_string "false")
	 (ne (symbol_ref "arc_write_ext_corereg (insn)") (const_int 0))
	 (const_string "false")
	 (gt (symbol_ref "arc_hazard (prev_active_insn (insn),
				      next_active_insn (insn))")
	     (symbol_ref "(arc_hazard (prev_active_insn (insn), insn)
			   + arc_hazard (insn, next_active_insn (insn)))"))
	 (const_string "false")
	 ]

	 (if_then_else (eq_attr "length" "2,4")
		       (const_string "true")
		       (const_string "false"))))

; must not put an insn inside that refers to blink.
(define_attr "in_call_delay_slot" "false,true"
  (cond [(eq_attr "in_delay_slot" "false")
	 (const_string "false")
	 (ne (symbol_ref "arc_regno_use_in (RETURN_ADDR_REGNUM,
					    PATTERN (insn))")
	     (const_int 0))
	 (const_string "false")]
	(const_string "true")))

(define_attr "in_sfunc_delay_slot" "false,true"
  (cond [(eq_attr "in_call_delay_slot" "false")
	 (const_string "false")
	 (ne (symbol_ref "arc_regno_use_in (12, PATTERN (insn))")
	     (const_int 0))
	 (const_string "false")]
	(const_string "true")))

;; Instructions that we can put into a delay slot and conditionalize.
(define_attr "cond_delay_insn" "no,yes"
  (cond [(eq_attr "cond" "!canuse") (const_string "no")
	 (eq_attr "type" "call,branch,uncond_branch,jump,brcc")
	 (const_string "no")
	 (eq_attr "length" "2,4") (const_string "yes")]
	(const_string "no")))

(define_attr "in_ret_delay_slot" "no,yes"
  (cond [(eq_attr "in_delay_slot" "false")
	 (const_string "no")
	 (ne (symbol_ref "regno_clobbered_p
			    (arc_return_address_regs
			      [arc_compute_function_type (cfun)],
			     insn, SImode, 1)")
	     (const_int 0))
	 (const_string "no")]
	(const_string "yes")))

(define_attr "cond_ret_delay_insn" "no,yes"
  (cond [(eq_attr "in_ret_delay_slot" "no") (const_string "no")
	 (eq_attr "cond_delay_insn" "no") (const_string "no")]
	(const_string "yes")))


(define_delay (and (eq_attr "type" "call,branch,uncond_branch,jump")
                   (ne (symbol_ref "TARGET_A4") (const_int 0)))
  [(eq_attr "in_delay_slot" "true")
   (eq_attr "in_delay_slot" "true")
   (eq_attr "in_delay_slot" "true")])

;; Delay slot definition for ARCompact ISA
;; ??? FIXME:
;; When outputting an annul-true insn elegible for cond-exec
;; in a cbranch delay slot, unless optimizing for size, we use cond-exec
;; for ARC600; we could also use this for ARC700 if the branch can't be
;; unaligned and is at least somewhat likely (add parameter for this).

;; (define_delay (and (eq_attr "type" "call,branch,uncond_branch,jump,brcc")
;;                    (ne (symbol_ref "TARGET_ARCOMPACT") (const_int 0)))
;;   [(eq_attr "in_delay_slot" "true")
;;    (eq_attr "in_delay_slot" "true")
;;    (nil)])
(define_delay (and (ne (symbol_ref "TARGET_ARCOMPACT") (const_int 0))
		   (eq_attr "type" "call"))
  [(eq_attr "in_call_delay_slot" "true")
   (eq_attr "in_call_delay_slot" "true")
   (nil)])

(define_delay (and (ne (symbol_ref "TARGET_ARCOMPACT
				    && !TARGET_AT_DBR_CONDEXEC")
		       (const_int 0))
		   (eq_attr "type" "brcc"))
  [(eq_attr "in_delay_slot" "true")
   (eq_attr "in_delay_slot" "true")
   (nil)])

(define_delay (and (ne (symbol_ref "TARGET_ARCOMPACT && TARGET_AT_DBR_CONDEXEC")
		       (const_int 0))
		   (eq_attr "type" "brcc"))
  [(eq_attr "in_delay_slot" "true")
   (nil)
   (nil)])

(define_delay
  (and (ne (symbol_ref "TARGET_ARCOMPACT")
	   (const_int 0))
       (eq_attr "type" "return"))
  [(eq_attr "in_ret_delay_slot" "yes")
   (eq_attr "type" "!call,branch,uncond_branch,jump,brcc,return,sfunc")
   (eq_attr "cond_ret_delay_insn" "yes")])

;; For ARC600, unexposing the delay sloy incurs a penalty also in the
;; non-taken case, so the only meaningful way to have an annull-true
;; filled delay slot is to conditionalize the delay slot insn.
(define_delay (and (ne (symbol_ref "TARGET_AT_DBR_CONDEXEC") (const_int 0))
		   (eq_attr "type" "branch,uncond_branch,jump")
		   (eq (symbol_ref "optimize_size") (const_int 0)))
  [(eq_attr "in_delay_slot" "true")
   (eq_attr "cond_delay_insn" "yes")
   (eq_attr "cond_delay_insn" "yes")])

;; For ARC700, anything goes for annulled-true insns, since there is no
;; penalty for the unexposed delay slot when the branch is not taken,
;; however, we must avoid things that have a delay slot themselvese to
;; avoid confucing gcc.
(define_delay (and (ne (symbol_ref "!TARGET_AT_DBR_CONDEXEC") (const_int 0))
		   (eq_attr "type" "branch,uncond_branch,jump")
		   (eq (symbol_ref "optimize_size") (const_int 0)))
  [(eq_attr "in_delay_slot" "true")
   (eq_attr "type" "!call,branch,uncond_branch,jump,brcc,return,sfunc")
   (eq_attr "cond_delay_insn" "yes")])

;; -mlongcall -fpic sfuncs use r12 to load the function address
(define_delay (and  (ne (symbol_ref "TARGET_ARCOMPACT") (const_int 0))
		    (eq_attr "type" "sfunc"))
  [(eq_attr "in_sfunc_delay_slot" "true")
   (eq_attr "in_sfunc_delay_slot" "true")
   (nil)])
;; ??? need to use a working strategy for canuse_limm:
;; - either canuse_limm is not eligible for delay slots, and has no
;;   delay slots, or arc_reorg has to treat them as nocond, or it has to
;;   somehow modify them to become inelegible for delay slots if a decision
;;   is made that makes conditional execution required.

(define_attr "tune" "none,arc600,arc700_4_2_std,arc700_4_2_xmac"
  (const
   (cond [(symbol_ref "arc_tune == TUNE_ARC600")
	  (const_string "arc600")
	  (symbol_ref "arc_tune == TUNE_ARC700_4_2_STD")
	  (const_string "arc700_4_2_std")
	  (symbol_ref "arc_tune == TUNE_ARC700_4_2_XMAC")
	  (const_string "arc700_4_2_xmac")]
         (const_string "none"))))

(define_attr "tune_arc700" "false,true"
  (if_then_else (eq_attr "tune" "arc700_4_2_std, arc700_4_2_xmac")
		(const_string "true")
		(const_string "false")))

;; Function units of the ARC

;; (define_function_unit {name} {num-units} {n-users} {test}
;;                       {ready-delay} {issue-delay} [{conflict-list}])

;; 1) A conditional jump cannot immediately follow the insn setting the flags in pre ARC700 processors.
;; The ARC700 has no problems with consecutive instructions setting and
;; using flags.
;; (define_function_unit "compare" 1 0 (and (eq_attr "type" "compare") (not (eq_attr "cpu" "ARC700"))) 2 2 [(eq_attr "type" "branch")])
;; (define_function_unit "compare" 1 0 (and (eq_attr "type" "compare") (eq_attr "cpu" "ARC700") ) 1 1 [(eq_attr "type" "branch")])

;; 2) References to loaded registers should wait a cycle.

;; Memory with load-delay of 1 (i.e., 2 cycle load).
;; (define_function_unit "memory" 1 1 (eq_attr "type" "load") 2 0)

;; Units that take one cycle do not need to be specified.

;; Move instructions.
(define_expand "movqi"
  [(set (match_operand:QI 0 "move_dest_operand" "")
	(match_operand:QI 1 "general_operand" ""))]
  ""
  "if (prepare_move_operands (operands, QImode)) DONE;")

; In order to allow the ccfsm machinery to do its work, the leading compact
; alternatives say 'canuse' - there is another alternative that will match
; when the condition codes are used.
; Rcq won't match if the condition is actually used; to avoid a spurious match
; via q, q is inactivated as constraint there.
; Likewise, the length of an alternative that might be shifted to conditional
; execution must reflect this, lest out-of-range branches are created.
; The iscompact attribute allows the epilogue expander to know for which
; insns it should lengthen the return insn.
(define_insn "*movqi_insn"
  [(set (match_operand:QI 0 "move_dest_operand" "=Rcq,Rcq#q,w, w,w,???w, w,Rcq,S,!*x,r,m,???m")
	(match_operand:QI 1 "move_src_operand"   "cL,cP,Rcq#q,cL,I,?Rac,?i,T,Rcq,Usd,m,c,?Rac"))]
  "register_operand (operands[0], QImode)
   || register_operand (operands[1], QImode)"
  "@
   mov%? %0,%1%&
   mov%? %0,%1%&
   mov%? %0,%1%&
   mov%? %0,%1
   mov%? %0,%1
   mov%? %0,%1
   mov%? %0,%S1
   ldb%? %0,%1%&
   stb%? %1,%0%&
   ldb%? %0,%1%&
   ldb%U1%V1 %0,%1
   stb%U0%V0 %1,%0
   stb%U0%V0 %1,%0"
  [(set_attr "type" "move,move,move,move,move,move,move,load,store,load,load,store,store")
   (set_attr "iscompact" "maybe,maybe,maybe,false,false,false,false,true,true,true,false,false,false")
   (set_attr "cond" "canuse,canuse_limm,canuse,canuse,canuse_limm,canuse,canuse,nocond,nocond,nocond,nocond,nocond,nocond")])

(define_expand "movhi"
  [(set (match_operand:HI 0 "move_dest_operand" "")
	(match_operand:HI 1 "general_operand" ""))]
  ""
  "if (prepare_move_operands (operands, HImode)) DONE;")

(define_insn "*movhi_insn"
  [(set (match_operand:HI 0 "move_dest_operand" "=Rcq,Rcq#q,w, w,w,???w,Rcq#q,w,Rcq,S,r,m,???m,VUsc")
	(match_operand:HI 1 "move_src_operand"   "cL,cP,Rcq#q,cL,I,?Rac,  ?i,?i,T,Rcq,m,c,?Rac,i"))]
  "register_operand (operands[0], HImode)
   || register_operand (operands[1], HImode)
   || (CONSTANT_P (operands[1])
       /* Don't use a LIMM that we could load with a single insn - we loose
	  delay-slot filling opportunities.  */
       && !satisfies_constraint_I (operands[1])
       && satisfies_constraint_Usc (operands[0]))"
  "@
   mov%? %0,%1%&
   mov%? %0,%1%&
   mov%? %0,%1%&
   mov%? %0,%1
   mov%? %0,%1
   mov%? %0,%1
   mov%? %0,%S1%&
   mov%? %0,%S1
   ldw%? %0,%1%&
   stw%? %1,%0%&
   ldw%U1%V1 %0,%1
   stw%U0%V0 %1,%0
   stw%U0%V0 %1,%0
   stw%U0%V0 %S1,%0"
  [(set_attr "type" "move,move,move,move,move,move,move,move,load,store,load,store,store,store")
   (set_attr "iscompact" "maybe,maybe,maybe,false,false,false,maybe_limm,false,true,true,false,false,false,false")
   (set_attr "cond" "canuse,canuse_limm,canuse,canuse,canuse_limm,canuse,canuse,canuse,nocond,nocond,nocond,nocond,nocond,nocond")])

(define_expand "movsi"
  [(set (match_operand:SI 0 "move_dest_operand" "")
	(match_operand:SI 1 "general_operand" ""))]
  ""
  "if (prepare_move_operands (operands, SImode)) DONE;")


; In order to allow the ccfsm machinery to do its work, the leading compact
; alternatives say 'canuse' - there is another alternative that will match
; when the condition codes are used.
; Rcq won't match if the condition is actually used; to avoid a spurious match
; via q, q is inactivated as constraint there.
; Likewise, the length of an alternative that might be shifted to conditional
; execution must reflect this, lest out-of-range branches are created.
; the iscompact attribute allows the epilogue expander to know for which
; insns it should lengthen the return insn.
; N.B. operand 1 of alternative 7 expands into pcl,symbol@gotpc .
(define_insn "*movsi_insn"
  [(set (match_operand:SI 0 "move_dest_operand" "=Rcq,Rcq#q,w, w,w,  w,???w, ?w,  w,Rcq#q, w,Rcq,  S,Us<,RcqRck,!*x,r,m,???m,VUsc")
	(match_operand:SI 1 "move_src_operand"  " cL,cP,Rcq#q,cL,I,Crr,?Rac,Cpc,Clb,?Cal,?Cal,T,Rcq,RcqRck,Us>,Usd,m,c,?Rac,C32"))]
  "register_operand (operands[0], SImode)
   || register_operand (operands[1], SImode)
   || (CONSTANT_P (operands[1])
       /* Don't use a LIMM that we could load with a single insn - we loose
	  delay-slot filling opportunities.  */
       && !satisfies_constraint_I (operands[1])
       && satisfies_constraint_Usc (operands[0]))"
  "@
   mov%? %0,%1%&
   mov%? %0,%1%&
   mov%? %0,%1%&
   mov%? %0,%1
   mov%? %0,%1
   ror %0,((%1*2+1) & 0x3f)
   mov%? %0,%1
   add %0,%S1
   * return arc_get_unalign () ? \"add %0,pcl,%1-.+2\" : \"add %0,pcl,%1-.\";
   mov%? %0,%S1%&
   mov%? %0,%S1
   ld%? %0,%1%&
   st%? %1,%0%&
   * return arc_short_long (insn, \"push%? %1%&\", \"st%U0 %1,%0%&\");
   * return arc_short_long (insn, \"pop%? %0%&\",  \"ld%U1 %0,%1%&\");
   ld%? %0,%1%&
   ld%U1%V1 %0,%1
   st%U0%V0 %1,%0
   st%U0%V0 %1,%0
   st%U0%V0 %S1,%0"
  [(set_attr "type" "move,move,move,move,move,two_cycle_core,move,binary,binary,move,move,load,store,store,load,load,load,store,store,store")
   (set_attr "iscompact" "maybe,maybe,maybe,false,false,false,false,false,false,maybe_limm,false,true,true,true,true,true,false,false,false,false")
   ; Use default length for iscompact to mark length varying.  But set length
   ; of Crr to 4.
   (set_attr "length" "*,*,*,4,4,4,4,8,8,*,8,*,*,*,*,*,*,*,*,8")
   (set_attr "cond" "canuse,canuse_limm,canuse,canuse,canuse_limm,canuse_limm,canuse,nocond,nocond,canuse,canuse,nocond,nocond,nocond,nocond,nocond,nocond,nocond,nocond,nocond")])

;; sometimes generated by the epilogue code.  We don't want to
;; recognize these addresses in general, because the limm is costly,
;; and we can't use them for stores.  */
(define_insn "*movsi_pre_mod"
  [(set (match_operand:SI 0 "register_operand" "=w")
	(mem:SI (pre_modify
		  (reg:SI SP_REG)
		  (plus:SI (reg:SI SP_REG)
			   (match_operand 1 "immediate_operand" "Cal")))))]
  "reload_completed"
  "ld.a %0,[sp,%1]"
  [(set_attr "type" "load")
   (set_attr "length" "8")])

/* Store a value to directly to memory.  The location might also be cached.
   Since the cached copy can cause a write-back at unpredictable times,
   we first write cached, then we write uncached.  */
(define_insn "store_direct"
  [(set (match_operand:SI 0 "move_dest_operand" "=m")
      (unspec:SI [(match_operand:SI 1 "register_operand" "c")]
       UNSPEC_DIRECT))]
  ""
  "st%U0 %1,%0\;st%U0.di %1,%0"
  [(set_attr "type" "store")])

(define_insn "*movsi_set_cc_insn"
  [(set (match_operand:CC_ZN 2 "cc_set_register" "")
	(match_operator 3 "zn_compare_operator"
	  [(match_operand:SI 1 "nonmemory_operand" "cI,Cal") (const_int 0)]))
   (set (match_operand:SI 0 "register_operand" "=w,w")
	(match_dup 1))]
  ""
  "mov%?.f %0,%S1"
  [(set_attr "type" "compare")
   (set_attr "cond" "set_zn")
   (set_attr "length" "4,8")])

(define_insn "unary_comparison"
  [(set (match_operand:CC_ZN 0 "cc_set_register" "")
	(match_operator 3 "zn_compare_operator"
	  [(match_operator:SI 2 "unary_operator"
	     [(match_operand:SI 1 "register_operand" "c")])
	   (const_int 0)]))]
  ""
  "%O2.f 0,%1"
  [(set_attr "type" "compare")
   (set_attr "cond" "set_zn")])


; this pattern is needed by combiner for cases like if (c=(~b)) { ... }
(define_insn "*unary_comparison_result_used"
  [(set (match_operand 2 "cc_register" "")
	(match_operator 4 "zn_compare_operator"
          [(match_operator:SI 3 "unary_operator"
	     [(match_operand:SI 1 "register_operand" "c")])
	       (const_int 0)]))
   (set (match_operand:SI 0 "register_operand" "=w")
	(match_dup 3))]
  ""
  "%O3.f %0,%1"
  [(set_attr "type" "compare")
   (set_attr "cond" "set_zn")
   (set_attr "length" "4")])

(define_insn "*tst"
  [(set
     (match_operand 0 "cc_register" "")
     (match_operator 3 "zn_compare_operator"
       [(and:SI
	  (match_operand:SI 1 "register_operand"  "%Rcq,Rcq, c,  c,  c,  c,  c")
	  (match_operand:SI 2 "nonmemory_operand"  "Rcq,C0p,cI,C1p,Ccp,CnL,Cal"))
	(const_int 0)]))]
  "TARGET_ARCOMPACT
   && ((register_operand (operands[1], SImode)
       && nonmemory_operand (operands[2], SImode))
      || (memory_operand (operands[1], SImode)
	  && satisfies_constraint_Cux (operands[2])))"
  "*
    switch (which_alternative)
    {
    case 0: case 2: case 6:
      return \"tst%? %1,%2\";
    case 1:
      return \"btst%? %1,%z2\";
    case 3:
      return \"bmsk%?.f 0,%1,%Z2%&\";
    case 4:
      return \"bclr%?.f 0,%1,%M2%&\";
    case 5:
      return \"bic%?.f 0,%1,%n2-1\";
    default:
      gcc_unreachable ();
    }
  "
  [(set_attr "iscompact" "maybe,maybe,false,false,false,false,false")
   (set_attr "type" "compare")
   (set_attr "length" "*,*,4,4,4,4,8")
   (set_attr "cond" "set_zn")])

(define_insn "*commutative_binary_comparison"
  [(set (match_operand:CC_ZN 0 "cc_set_register" "")
	(match_operator 5 "zn_compare_operator"
	  [(match_operator:SI 4 "commutative_operator"
	     [(match_operand:SI 1 "register_operand" "%c,c,c")
	      (match_operand:SI 2 "nonmemory_operand" "cL,I,?Cal")])
	   (const_int 0)]))
   (clobber (match_scratch:SI 3 "=X,1,X"))]
  ""
  "%O4.f 0,%1,%2"
  [(set_attr "type" "compare")
   (set_attr "cond" "set_zn")
   (set_attr "length" "4,4,8")])

; for flag setting 'add' instructions like if (a+b) { ...}
; the combiner needs this pattern
(define_insn "*addsi_compare"
  [(set (reg:CC_ZN CC_REG)
	(compare:CC_ZN (match_operand:SI 0 "register_operand" "c")
		       (neg:SI (match_operand:SI 1 "register_operand" "c"))))]
  ""
  "add.f 0,%0,%1"
  [(set_attr "cond" "set")
   (set_attr "type" "compare")
   (set_attr "length" "4")])

; for flag setting 'add' instructions like if (a+b < a) { ...}
; the combiner needs this pattern
(define_insn "addsi_compare_2"
  [(set (reg:CC_C CC_REG)
	(compare:CC_C (plus:SI (match_operand:SI 0 "register_operand" "c,c")
			       (match_operand:SI 1 "nonmemory_operand" "cL,Cal"))
		      (match_dup 0)))]
  ""
  "add.f 0,%0,%1"
  [(set_attr "cond" "set")
   (set_attr "type" "compare")
   (set_attr "length" "4,8")])

(define_insn "*addsi_compare_3"
  [(set (reg:CC_C CC_REG)
	(compare:CC_C (plus:SI (match_operand:SI 0 "register_operand" "c")
			       (match_operand:SI 1 "register_operand" "c"))
		      (match_dup 1)))]
  ""
  "add.f 0,%0,%1"
  [(set_attr "cond" "set")
   (set_attr "type" "compare")
   (set_attr "length" "4")])

; this pattern is needed by combiner for cases like if (c=a+b) { ... }
(define_insn "*commutative_binary_comparison_result_used"
  [(set (match_operand 3 "cc_register" "")
	(match_operator 5 "zn_compare_operator"
          [(match_operator:SI 4 "commutative_operator"
	     [(match_operand:SI 1 "register_operand" "c,0,c")
	      (match_operand:SI 2 "nonmemory_operand" "cL,I,?Cal")])
	   (const_int 0)]))
   (set (match_operand:SI 0 "register_operand" "=w,w,w")
	(match_dup 4))]
  ""
  "%O4.f %0,%1,%2"
  [(set_attr "type" "compare,compare,compare")
   (set_attr "cond" "set_zn,set_zn,set_zn")
   (set_attr "length" "4,4,8")])

; this pattern is needed by combiner for cases like if (c=a<<b) { ... }
(define_insn "*noncommutative_binary_comparison_result_used"
  [(set (match_operand 3 "cc_register" "")
	(match_operator 5 "zn_compare_operator"
          [(match_operator:SI 4 "noncommutative_operator"
	     [(match_operand:SI 1 "register_operand" "c,0,c")
	      (match_operand:SI 2 "nonmemory_operand" "cL,I,?Cal")])
	       (const_int 0)]))
   (set (match_operand:SI 0 "register_operand" "=w,w,w")
	(match_dup 4 ))]
  ""
  "%O4.f %0,%1,%2"
  [(set_attr "type" "compare,compare,compare")
   (set_attr "cond" "set_zn,set_zn,set_zn")
   (set_attr "length" "4,4,8")])

(define_insn "*noncommutative_binary_comparison"
  [(set (match_operand:CC_ZN 0 "cc_set_register" "")
	(match_operator 5 "zn_compare_operator"
	  [(match_operator:SI 4 "noncommutative_operator"
	     [(match_operand:SI 1 "register_operand" "c,c,c")
	      (match_operand:SI 2 "nonmemory_operand" "cL,I,?Cal")])
	   (const_int 0)]))
   (clobber (match_scratch:SI 3 "=X,1,X"))]
  ""
  "%O4.f 0,%1,%2"
  [(set_attr "type" "compare")
   (set_attr "cond" "set_zn")
   (set_attr "length" "4,4,8")])

(define_expand "movdi"
  [(set (match_operand:DI 0 "move_dest_operand" "")
	(match_operand:DI 1 "general_operand" ""))]
  ""
  "
{
  /* Everything except mem = const or mem = mem can be done easily.  */

  if (GET_CODE (operands[0]) == MEM)
    operands[1] = force_reg (DImode, operands[1]);
}")

(define_insn_and_split "*movdi_insn"
  [(set (match_operand:DI 0 "move_dest_operand" "=w,w,r,m")
	(match_operand:DI 1 "move_double_src_operand" "c,HJi,m,c"))]
  "register_operand (operands[0], DImode)
   || register_operand (operands[1], DImode)"
  "*
{
  switch (which_alternative)
    {
    default:
    case 0 :
      /* We normally copy the low-numbered register first.  However, if
	 the first register operand 0 is the same as the second register of
	 operand 1, we must copy in the opposite order.  */
      if (REGNO (operands[0]) == REGNO (operands[1]) + 1)
	return \"mov%? %R0,%R1\;mov%? %0,%1\";
      else
      return \"mov%? %0,%1\;mov%? %R0,%R1\";
    case 1 :
      return \"mov%? %L0,%L1\;mov%? %H0,%H1\";
    case 2 :
      /* If the low-address word is used in the address, we must load it
	 last.  Otherwise, load it first.  Note that we cannot have
	 auto-increment in that case since the address register is known to be
	 dead.  */
      if (refers_to_regno_p (REGNO (operands[0]), REGNO (operands[0]) + 1,
			     operands [1], 0))
	return \"ld%V1 %R0,%R1\;ld%V1 %0,%1\";
      else switch (GET_CODE (XEXP(operands[1], 0)))
	{
	case POST_MODIFY: case POST_INC: case POST_DEC:
	  return \"ld%V1 %R0,%R1\;ld%U1%V1 %0,%1\";
	case PRE_MODIFY: case PRE_INC: case PRE_DEC:
	  return \"ld%U1%V1 %0,%1\;ld%V1 %R0,%R1\";
	default:
	  return \"ld%U1%V1 %0,%1\;ld%U1%V1 %R0,%R1\";
	}
    case 3 :
      switch (GET_CODE (XEXP(operands[0], 0)))
	{
	case POST_MODIFY: case POST_INC: case POST_DEC:
     	  return \"st%V0 %R1,%R0\;st%U0%V0 %1,%0\";
	case PRE_MODIFY: case PRE_INC: case PRE_DEC:
     	  return \"st%U0%V0 %1,%0\;st%V0 %R1,%R0\";
	default:
     	  return \"st%U0%V0 %1,%0\;st%U0%V0 %R1,%R0\";
	}
    }
}"
  "&& reload_completed && optimize"
  [(set (match_dup 2) (match_dup 3)) (set (match_dup 4) (match_dup 5))]
  "arc_split_move (operands);"
  [(set_attr "type" "move,move,load,store")
   ;; ??? The ld/st values could be 4 if it's [reg,bignum].
   (set_attr "length" "8,16,16,16")])


;; Floating point move insns.

(define_expand "movsf"
  [(set (match_operand:SF 0 "general_operand" "")
	(match_operand:SF 1 "general_operand" ""))]
  ""
  "if (prepare_move_operands (operands, SFmode)) DONE;")

(define_insn "*movsf_insn"
  [(set (match_operand:SF 0 "move_dest_operand" "=w,w,r,m")
	(match_operand:SF 1 "move_src_operand" "c,E,m,c"))]
  "register_operand (operands[0], SFmode)
   || register_operand (operands[1], SFmode)"
  "@
   mov%? %0,%1
   mov%? %0,%1 ; %A1
   ld%U1%V1 %0,%1
   st%U0%V0 %1,%0"
  [(set_attr "type" "move,move,load,store")])

(define_expand "movdf"
  [(set (match_operand:DF 0 "general_operand" "")
	(match_operand:DF 1 "general_operand" ""))]
  ""
  "if (prepare_move_operands (operands, DFmode)) DONE;")

(define_insn_and_split "*movdf_insn"
  [(set (match_operand:DF 0 "move_dest_operand"      "=w,w,r,m,D,r")
	(match_operand:DF 1 "move_double_src_operand" "c,E,m,c,r,D"))]
  "register_operand (operands[0], DFmode)
   || register_operand (operands[1], DFmode)"
  "*
{
  switch (which_alternative)
    {
    default:
    case 0 :
      /* We normally copy the low-numbered register first.  However, if
	 the first register operand 0 is the same as the second register of
	 operand 1, we must copy in the opposite order.  */
      if (REGNO (operands[0]) == REGNO (operands[1]) + 1)
	return \"mov %R0,%R1\;mov %0,%1\";
      else
      return \"mov%? %0,%1\;mov%? %R0,%R1\";
    case 1 :
      return \"mov%? %L0,%L1\;mov%? %H0,%H1 ; %A1\";
    case 2 :
      /* If the low-address word is used in the address, we must load it
	 last.  Otherwise, load it first.  Note that we cannot have
	 auto-increment in that case since the address register is known to be
	 dead.  */
      if (refers_to_regno_p (REGNO (operands[0]), REGNO (operands[0]) + 1,
			     operands [1], 0))
	return \"ld%V1 %R0,%R1\;ld%V1 %0,%1\";
      else switch (GET_CODE (XEXP(operands[1], 0)))
	{
	case POST_MODIFY: case POST_INC: case POST_DEC:
	  return \"ld%V1 %R0,%R1\;ld%U1%V1 %0,%1\";
	case PRE_MODIFY: case PRE_INC: case PRE_DEC:
	  return \"ld%U1%V1 %0,%1\;ld%V1 %R0,%R1\";
	default:
	  return \"ld%U1%V1 %0,%1\;ld%U1%V1 %R0,%R1\";
	}
    case 3 :
      switch (GET_CODE (XEXP(operands[0], 0)))
	{
	case POST_MODIFY: case POST_INC: case POST_DEC:
     	  return \"st%V0 %R1,%R0\;st%U0%V0 %1,%0\";
	case PRE_MODIFY: case PRE_INC: case PRE_DEC:
     	  return \"st%U0%V0 %1,%0\;st%V0 %R1,%R0\";
	default:
     	  return \"st%U0%V0 %1,%0\;st%U0%V0 %R1,%R0\";
	}
    case 4:
        if (!TARGET_DPFP)
          {
            fatal_error (\"DPFP register allocated without -mdpfp\\n\");
          }
        return \"dexcl%F0 0, %H1, %L1\";
    case 5:
        return \"lr %H0,[%H1h]\;lr %L0,[%H1l] ; double reg moves\";

    }
}"
  "&& reload_completed && optimize
   /* Don't touch the DOUBLE_REGS alternatives.  */
   && !refers_to_regno_p (40, 44, operands[0], 0)
   && !refers_to_regno_p (40, 44, operands[1], 0)"
  [(set (match_dup 2) (match_dup 3)) (set (match_dup 4) (match_dup 5))]
  "arc_split_move (operands);"
  [(set_attr "type" "move,move,load,store, move,lr")
   (set_attr "cond" "canuse,canuse,nocond,nocond,nocond,nocond")
   ;; ??? The ld/st values could be 16 if it's [reg,bignum].
   (set_attr "length" "8,16,16,16,4,16")])

;; Load/Store with update instructions.
;;
;; Some of these we can get by using pre-decrement or pre-increment, but the
;; hardware can also do cases where the increment is not the size of the
;; object.
;;
;; In all these cases, we use operands 0 and 1 for the register being
;; incremented because those are the operands that local-alloc will
;; tie and these are the pair most likely to be tieable (and the ones
;; that will benefit the most).
;;
;; We use match_operator here because we need to know whether the memory
;; object is volatile or not.


;; Note: loadqi_update has no 16-bit variant
(define_insn "*loadqi_update"
  [(set (match_operand:QI 3 "dest_reg_operand" "=r,r")
	(match_operator:QI 4 "load_update_operand"
	 [(match_operand:SI 1 "register_operand" "0,0")
	  (match_operand:SI 2 "nonmemory_operand" "rI,Cal")]))
   (set (match_operand:SI 0 "dest_reg_operand" "=r,r")
	(plus:SI (match_dup 1) (match_dup 2)))]
  ""
  "ldb.a%V4 %3,[%0,%S2]"
  [(set_attr "type" "load,load")
   (set_attr "length" "4,8")])

(define_insn "*load_zeroextendqisi_update"
  [(set (match_operand:SI 3 "dest_reg_operand" "=r,r")
	(zero_extend:SI (match_operator:QI 4 "load_update_operand"
			 [(match_operand:SI 1 "register_operand" "0,0")
			  (match_operand:SI 2 "nonmemory_operand" "rI,Cal")])))
   (set (match_operand:SI 0 "dest_reg_operand" "=r,r")
	(plus:SI (match_dup 1) (match_dup 2)))]
  ""
  "ldb.a%V4 %3,[%0,%S2]"
  [(set_attr "type" "load,load")
   (set_attr "length" "4,8")])

(define_insn "*load_signextendqisi_update"
  [(set (match_operand:SI 3 "dest_reg_operand" "=r,r")
	(sign_extend:SI (match_operator:QI 4 "load_update_operand"
			 [(match_operand:SI 1 "register_operand" "0,0")
			  (match_operand:SI 2 "nonmemory_operand" "rI,Cal")])))
   (set (match_operand:SI 0 "dest_reg_operand" "=r,r")
	(plus:SI (match_dup 1) (match_dup 2)))]
  ""
  "ldb.x.a%V4 %3,[%0,%S2]"
  [(set_attr "type" "load,load")
   (set_attr "length" "4,8")])

(define_insn "*storeqi_update"
  [(set (match_operator:QI 4 "store_update_operand"
	 [(match_operand:SI 1 "register_operand" "0")
	  (match_operand:SI 2 "short_immediate_operand" "I")])
	(match_operand:QI 3 "register_operand" "c"))
   (set (match_operand:SI 0 "dest_reg_operand" "=w")
	(plus:SI (match_dup 1) (match_dup 2)))]
  ""
  "stb.a%V4 %3,[%0,%2]"
  [(set_attr "type" "store")
   (set_attr "length" "4")])

;; ??? pattern may have to be re-written
;; Note: no 16-bit variant for this pattern
(define_insn "*loadhi_update"
  [(set (match_operand:HI 3 "dest_reg_operand" "=r,r")
	(match_operator:HI 4 "load_update_operand"
	 [(match_operand:SI 1 "register_operand" "0,0")
	  (match_operand:SI 2 "nonmemory_operand" "rI,Cal")]))
   (set (match_operand:SI 0 "dest_reg_operand" "=w,w")
	(plus:SI (match_dup 1) (match_dup 2)))]
  ""
  "ldw.a%V4 %3,[%0,%S2]"
  [(set_attr "type" "load,load")
   (set_attr "length" "4,8")])

(define_insn "*load_zeroextendhisi_update"
  [(set (match_operand:SI 3 "dest_reg_operand" "=r,r")
	(zero_extend:SI (match_operator:HI 4 "load_update_operand"
			 [(match_operand:SI 1 "register_operand" "0,0")
			  (match_operand:SI 2 "nonmemory_operand" "rI,Cal")])))
   (set (match_operand:SI 0 "dest_reg_operand" "=r,r")
	(plus:SI (match_dup 1) (match_dup 2)))]
  ""
  "ldw.a%V4 %3,[%0,%S2]"
  [(set_attr "type" "load,load")
   (set_attr "length" "4,8")])

;; Note: no 16-bit variant for this instruction
(define_insn "*load_signextendhisi_update"
  [(set (match_operand:SI 3 "dest_reg_operand" "=r,r")
	(sign_extend:SI (match_operator:HI 4 "load_update_operand"
			 [(match_operand:SI 1 "register_operand" "0,0")
			  (match_operand:SI 2 "nonmemory_operand" "rI,Cal")])))
   (set (match_operand:SI 0 "dest_reg_operand" "=w,w")
	(plus:SI (match_dup 1) (match_dup 2)))]
  ""
  "ldw.x.a%V4 %3,[%0,%S2]"
  [(set_attr "type" "load,load")
   (set_attr "length" "4,8")])

(define_insn "*storehi_update"
  [(set (match_operator:HI 4 "store_update_operand"
	 [(match_operand:SI 1 "register_operand" "0")
	  (match_operand:SI 2 "short_immediate_operand" "I")])
	(match_operand:HI 3 "register_operand" "c"))
   (set (match_operand:SI 0 "dest_reg_operand" "=w")
	(plus:SI (match_dup 1) (match_dup 2)))]
  ""
  "stw.a%V4 %3,[%0,%2]"
  [(set_attr "type" "store")
   (set_attr "length" "4")])

;; No 16-bit variant for this instruction pattern
(define_insn "*loadsi_update"
  [(set (match_operand:SI 3 "dest_reg_operand" "=r,r")
	(match_operator:SI 4 "load_update_operand"
	 [(match_operand:SI 1 "register_operand" "0,0")
	  (match_operand:SI 2 "nonmemory_operand" "rI,Cal")]))
   (set (match_operand:SI 0 "dest_reg_operand" "=w,w")
	(plus:SI (match_dup 1) (match_dup 2)))]
  ""
  "ld.a%V4 %3,[%0,%S2]"
  [(set_attr "type" "load,load")
   (set_attr "length" "4,8")])

(define_insn "*storesi_update"
  [(set (match_operator:SI 4 "store_update_operand"
	 [(match_operand:SI 1 "register_operand" "0")
	  (match_operand:SI 2 "short_immediate_operand" "I")])
	(match_operand:SI 3 "register_operand" "c"))
   (set (match_operand:SI 0 "dest_reg_operand" "=w")
	(plus:SI (match_dup 1) (match_dup 2)))]
  ""
  "st.a%V4 %3,[%0,%2]"
  [(set_attr "type" "store")
   (set_attr "length" "4")])

(define_insn "*loadsf_update"
  [(set (match_operand:SF 3 "dest_reg_operand" "=r,r")
	(match_operator:SF 4 "load_update_operand"
	 [(match_operand:SI 1 "register_operand" "0,0")
	  (match_operand:SI 2 "nonmemory_operand" "rI,Cal")]))
   (set (match_operand:SI 0 "dest_reg_operand" "=w,w")
	(plus:SI (match_dup 1) (match_dup 2)))]
  ""
  "ld.a%V4 %3,[%0,%S2]"
  [(set_attr "type" "load,load")
   (set_attr "length" "4,8")])

(define_insn "*storesf_update"
  [(set (match_operator:SF 4 "store_update_operand"
	 [(match_operand:SI 1 "register_operand" "0")
	  (match_operand:SI 2 "short_immediate_operand" "I")])
	(match_operand:SF 3 "register_operand" "c"))
   (set (match_operand:SI 0 "dest_reg_operand" "=w")
	(plus:SI (match_dup 1) (match_dup 2)))]
  ""
  "st.a%V4 %3,[%0,%2]"
  [(set_attr "type" "store")
   (set_attr "length" "4")])

;; Conditional move instructions.

(define_expand "movsicc"
  [(set (match_operand:SI 0 "dest_reg_operand" "")
	(if_then_else:SI (match_operand 1 "comparison_operator" "")
		         (match_operand:SI 2 "nonmemory_operand" "")
 		         (match_operand:SI 3 "register_operand" "")))]
  ""
  "
{
  enum rtx_code code = GET_CODE (operands[1]);

  operands[1] = gen_compare_reg (code, VOIDmode);
}")


(define_expand "movdicc"
  [(set (match_operand:DI 0 "dest_reg_operand" "")
	(if_then_else:DI(match_operand 1 "comparison_operator" "")
		        (match_operand:DI 2 "nonmemory_operand" "")
		        (match_operand:DI 3 "register_operand" "")))]
  ""
  "
{
  enum rtx_code code = GET_CODE (operands[1]);

  operands[1] = gen_compare_reg (code, VOIDmode);
}")


(define_expand "movsfcc"
  [(set (match_operand:SF 0 "dest_reg_operand" "")
	(if_then_else:SF (match_operand 1 "comparison_operator" "")
		      (match_operand:SF 2 "nonmemory_operand" "")
		      (match_operand:SF 3 "register_operand" "")))]
  ""
  "
{
  enum rtx_code code = GET_CODE (operands[1]);

  operands[1] = gen_compare_reg (code, VOIDmode);
}")

(define_expand "movdfcc"
  [(set (match_operand:DF 0 "dest_reg_operand" "")
	(if_then_else:DF (match_operand 1 "comparison_operator" "")
		      (match_operand:DF 2 "nonmemory_operand" "")
		      (match_operand:DF 3 "register_operand" "")))]
  ""
  "
{
  enum rtx_code code = GET_CODE (operands[1]);

  operands[1] = gen_compare_reg (code, VOIDmode);
}")

(define_insn "*movsicc_insn"
  [(set (match_operand:SI 0 "dest_reg_operand" "=w,w")
  	(if_then_else:SI (match_operator 3 "proper_comparison_operator"
  		       [(match_operand 4 "cc_register" "") (const_int 0)])
  		      (match_operand:SI 1 "nonmemory_operand" "cL,Cal")
  		      (match_operand:SI 2 "register_operand" "0,0")))]
  ""
{
  if (rtx_equal_p (operands[1], const0_rtx) && GET_CODE (operands[3]) == NE
      && satisfies_constraint_Rcq (operands[0]))
    return "sub%?.ne %0,%0,%0";
  /* ??? might be good for speed on ARC600 too, *if* properly scheduled.  */
  if ((TARGET_ARC700 || optimize_size)
      && rtx_equal_p (operands[1], constm1_rtx)
      && GET_CODE (operands[3]) == LTU)
    return "sbc.cs %0,%0,%0";
  return "mov.%d3 %0,%S1";
}
  [(set_attr "type" "cmove,cmove")
   (set_attr "length" "4,8")])

; Try to generate more short moves, and/or less limms, by substituting a
; conditional move with a conditional sub.
(define_peephole2
  [(set (match_operand:SI 0 "compact_register_operand")
	(match_operand:SI 1 "const_int_operand"))
   (set (match_dup 0)
  	(if_then_else:SI (match_operator 3 "proper_comparison_operator"
			   [(match_operand 4 "cc_register" "") (const_int 0)])
			    (match_operand:SI 2 "const_int_operand" "")
  		      (match_dup 0)))]
  "!satisfies_constraint_P (operands[1])
   && satisfies_constraint_P (operands[2])
   && UNSIGNED_INT6 (INTVAL (operands[2]) - INTVAL (operands[1]))"
  [(set (match_dup 0) (match_dup 2))
   (cond_exec
     (match_dup 3)
     (set (match_dup 0)
	  (plus:SI (match_dup 0) (match_dup 1))))]
  "operands[3] = gen_rtx_fmt_ee (REVERSE_CONDITION (GET_CODE (operands[3]),
						    GET_MODE (operands[4])),
				 VOIDmode, operands[4], const0_rtx);
   operands[1] = GEN_INT (INTVAL (operands[1]) - INTVAL (operands[2]));")

(define_insn "*movdicc_insn"
  [(set (match_operand:DI 0 "dest_reg_operand" "=&w,w")
	(if_then_else:DI (match_operator 3 "proper_comparison_operator"
			[(match_operand 4 "cc_register" "") (const_int 0)])
		      (match_operand:DI 1 "nonmemory_operand" "c,Ji")
		      (match_operand:DI 2 "register_operand" "0,0")))]
   ""
   "*
{
   switch (which_alternative)
     {
     default:
     case 0 :
       /* We normally copy the low-numbered register first.  However, if
 	 the first register operand 0 is the same as the second register of
 	 operand 1, we must copy in the opposite order.  */
       if (REGNO (operands[0]) == REGNO (operands[1]) + 1)
 	return \"mov.%d3 %R0,%R1\;mov.%d3 %0,%1\";
       else
 	return \"mov.%d3 %0,%1\;mov.%d3 %R0,%R1\";
     case 1 :
	return \"mov.%d3 %L0,%L1\;mov.%d3 %H0,%H1\";


     }
}"
  [(set_attr "type" "cmove,cmove")
   (set_attr "length" "8,16")])


(define_insn "*movsfcc_insn"
  [(set (match_operand:SF 0 "dest_reg_operand" "=w,w")
	(if_then_else:SF (match_operator 3 "proper_comparison_operator"
		       [(match_operand 4 "cc_register" "") (const_int 0)])
		      (match_operand:SF 1 "nonmemory_operand" "c,E")
		      (match_operand:SF 2 "register_operand" "0,0")))]
  ""
  "@
   mov.%d3 %0,%1
   mov.%d3 %0,%1 ; %A1"
  [(set_attr "type" "cmove,cmove")])

(define_insn "*movdfcc_insn"
  [(set (match_operand:DF 0 "dest_reg_operand" "=w,w")
	(if_then_else:DF (match_operator 1 "proper_comparison_operator"
		 [(match_operand 4 "cc_register" "") (const_int 0)])
		      (match_operand:DF 2 "nonmemory_operand" "c,E")
		      (match_operand:DF 3 "register_operand" "0,0")))]
  ""
  "*
{
  switch (which_alternative)
    {
    default:
    case 0 :
      /* We normally copy the low-numbered register first.  However, if
	 the first register operand 0 is the same as the second register of
	 operand 1, we must copy in the opposite order.  */
      if (REGNO (operands[0]) == REGNO (operands[2]) + 1)
	return \"mov.%d1 %R0,%R2\;mov.%d1 %0,%2\";
      else
	return \"mov.%d1 %0,%2\;mov.%d1 %R0,%R2\";
    case 1 :
	      return \"mov.%d1 %L0,%L2\;mov.%d1 %H0,%H2; %A2 \";

    }
}"
  [(set_attr "type" "cmove,cmove")
   (set_attr "length" "8,16")])


;; TODO - Support push_s and pop_s insns
;; PUSH/POP instruction
;(define_insn "*pushsi"
;  [(set (mem:SI (pre_dec:SI (reg:SI 28)))
;        (match_operand:SI 0 "register_operand" "q"))]
;  "TARGET_MIXED_CODE"
;  "push_s %0"
;  [(set_attr "type" "push")
;   (set_attr "iscompact" "true")
;   (set_attr "length" "2")])
;
;(define_insn "*popsi"
;  [(set (match_operand:SI 0 "register_operand" "=q")
;        (mem:SI (post_inc:SI (reg:SI 28))))]
;  "TARGET_MIXED_CODE"
;  "pop_s %0"
;  [(set_attr "type" "pop")
;   (set_attr "iscompact" "true")
;   (set_attr "length" "2")])

;; Zero extension instructions.
;; ??? We don't support volatile memrefs here, but I'm not sure why.

(define_insn "*zero_extendqihi2_a4"
  [(set (match_operand:HI 0 "dest_reg_operand" "=r")
	(zero_extend:HI (match_operand:QI 1 "register_operand" "r")))]
  "TARGET_A4"
  "extb%? %0,%1"
  [(set_attr "type" "unary")])

(define_insn "*zero_extendqihi2_i"
  [(set (match_operand:HI 0 "dest_reg_operand" "=Rcq,Rcq#q,Rcw,w,r")
	(zero_extend:HI (match_operand:QI 1 "nonvol_nonimm_operand" "0,Rcq#q,0,c,m")))]
  ""
  "@
   extb%? %0,%1%&
   extb%? %0,%1%&
   bmsk%? %0,%1,7
   extb %0,%1
   ldb%U1 %0,%1"
  [(set_attr "type" "unary,unary,unary,unary,load")
   (set_attr "iscompact" "maybe,true,false,false,false")
   (set_attr "cond" "canuse,nocond,canuse,nocond,nocond")])

(define_expand "zero_extendqihi2"
  [(set (match_operand:HI 0 "dest_reg_operand" "")
	(zero_extend:HI (match_operand:QI 1 "nonvol_nonimm_operand" "")))]
  ""
  "if (prepare_extend_operands (operands, ZERO_EXTEND, HImode)) DONE;"
)

;; (define_insn "zero_extendqihi2"
;;   [(set (match_operand:HI 0 "register_operand" "=r,r")
;; 	(zero_extend:HI (match_operand:QI 1 "nonvol_nonimm_operand" "r,m")))]
;;   ""
;;   "@
;;    extb %0,%1
;;    ldb%U1 %0,%1"
;;   [(set_attr "type" "unary,load")
;;    (set_attr "cond" "nocond,nocond")])

(define_insn "*zero_extendqisi2_a4"
  [(set (match_operand:SI 0 "dest_reg_operand" "=r,r")
;	(zero_extend:SI (match_operand:QI 1 "register_operand" "r")))]
	(zero_extend:SI (match_operand:QI 1 "nonvol_nonimm_operand" "r,m")))]
  "TARGET_A4"
  "@
   extb%? %0,%1
   ldb%U1 %0,%1"
  [(set_attr "type" "unary,load")])

(define_insn "*zero_extendqisi2_ac"
  [(set (match_operand:SI 0 "dest_reg_operand" "=Rcq,Rcq#q,Rcw,w,qRcq,!*x,r")
	(zero_extend:SI (match_operand:QI 1 "nonvol_nonimm_operand" "0,Rcq#q,0,c,T,Usd,m")))]
  "TARGET_ARCOMPACT"
  "*
   switch (which_alternative)
   {
    case 0: case 1:
      return \"extb%? %0,%1%&\";
    case 2:
      return \"bmsk%? %0,%1,7\";
    case 3:
      return \"extb %0,%1\";
    case 4: case 5:
      return \"ldb%? %0,%1%&\";
    case 6:
      return \"ldb%U1 %0,%1\";
    default:
      gcc_unreachable ();
   }"
  [(set_attr "type" "unary,unary,unary,unary,load,load,load")
   (set_attr "iscompact" "maybe,true,false,false,true,true,false")
   (set_attr "cond" "canuse,nocond,canuse,nocond,nocond,nocond,nocond")])

(define_expand "zero_extendqisi2"
  [(set (match_operand:SI 0 "dest_reg_operand" "")
	(zero_extend:SI (match_operand:QI 1 "nonvol_nonimm_operand" "")))]
  ""
  "if (prepare_extend_operands (operands, ZERO_EXTEND, SImode)) DONE;"
)
;   (set_attr "length" "2,4,8")

;; (define_insn "zero_extendqisi2"
;;   [(set (match_operand:SI 0 "register_operand" "=r,r")
;; 	(zero_extend:SI (match_operand:QI 1 "nonvol_nonimm_operand" "r,m")))]
;;   ""
;;   "@
;;    extb %0,%1
;;    ldb%U1 %0,%1"
;;   [(set_attr "type" "unary,load")
;;    (set_attr "cond" "nocond,nocond")])

(define_insn "*zero_extendhisi2_a4"
  [(set (match_operand:SI 0 "dest_reg_operand" "=r")
	(zero_extend:SI (match_operand:HI 1 "register_operand" "r")))]
  "TARGET_A4"
  "extw%? %0,%1"
  [(set_attr "type" "unary")])

(define_insn "*zero_extendhisi2_i"
  [(set (match_operand:SI 0 "dest_reg_operand" "=Rcq,q,Rcw,w,!x,Rcqq,r")
	(zero_extend:SI (match_operand:HI 1 "nonvol_nonimm_operand" "0,q,0,c,Usd,Usd,m")))]
  ""
  "*
  switch (which_alternative)
    {
    case 0: case 1:
      return \"extw%? %0,%1%&\";
    case 2:
      return \"bmsk%? %0,%1,15\";
    case 3:
      return \"extw %0,%1\";
    case 4:
      return \"ldw%? %0,%1%&\";
    case 5:
      return \"ldw%U1 %0,%1\";
    case 6:
      return \"ldw%U1%V1 %0,%1\";
    default:
      gcc_unreachable ();
    }"
  [(set_attr "type" "unary,unary,unary,unary,load,load,load")
   (set_attr "iscompact" "maybe,true,false,false,true,false,false")
   (set_attr "cond" "canuse,nocond,canuse,nocond,nocond,nocond,nocond")])


(define_expand "zero_extendhisi2"
  [(set (match_operand:SI 0 "dest_reg_operand" "")
	(zero_extend:SI (match_operand:HI 1 "nonvol_nonimm_operand" "")))]
  ""
  "if (prepare_extend_operands (operands, ZERO_EXTEND, SImode)) DONE;"
)

;; (define_insn "zero_extendhisi2"
;;   [(set (match_operand:SI 0 "dest_reg_operand" "=r,r")
;; 	(zero_extend:SI (match_operand:HI 1 "nonvol_nonimm_operand" "r,m")))]
;;   ""
;;   "@
;;    extw %0,%1
;;    ldw%U1 %0,%1"
;;   [(set_attr "type" "unary,load")
;;    (set_attr "cond" "nocond,nocond")])

;; Sign extension instructions.

(define_insn "*extendqihi2_a4"
  [(set (match_operand:HI 0 "dest_reg_operand" "=r")
	(sign_extend:HI (match_operand:QI 1 "register_operand" "r")))]
  "TARGET_A4"
  "sexb%? %0,%1"
  [(set_attr "type" "unary")])

(define_insn "*extendqihi2_i"
  [(set (match_operand:HI 0 "dest_reg_operand" "=Rcqq,r,r")
	(sign_extend:HI (match_operand:QI 1 "nonvol_nonimm_operand" "Rcqq,r,m")))]
  ""
  "@
   sexb%? %0,%1%&
   sexb %0,%1
   ldb.x%U1 %0,%1"
  [(set_attr "type" "unary,unary,load")
   (set_attr "iscompact" "true,false,false")
   (set_attr "cond" "nocond,nocond,nocond")])


(define_expand "extendqihi2"
  [(set (match_operand:HI 0 "dest_reg_operand" "")
	(sign_extend:HI (match_operand:QI 1 "nonvol_nonimm_operand" "")))]
  ""
  "if (prepare_extend_operands (operands, SIGN_EXTEND, HImode)) DONE;"
)

;; (define_insn "extendqihi2"
;;   [(set (match_operand:HI 0 "register_operand" "=r,r")
;; 	(sign_extend:HI (match_operand:QI 1 "nonvol_nonimm_operand" "r,m")))]
;;   ""
;;   "@
;;    sexb %0,%1
;;    ldb.x%U1 %0,%1"
;;   [(set_attr "type" "unary,load")
;;    (set_attr "cond" "nocond,nocond")])

(define_insn "*extendqisi2_a4"
  [(set (match_operand:SI 0 "dest_reg_operand" "=r,r")
;	(sign_extend:SI (match_operand:QI 1 "register_operand" "r")))]
	(sign_extend:SI (match_operand:QI 1 "nonvol_nonimm_operand" "r,m")))]
  "TARGET_A4"
  "@
    sexb%? %0,%1
    ldb.x%U1 %0,%1"
  [(set_attr "type" "unary,load")])

;; (define_insn "*extendqisi2_mixed"
;;   [(set (match_operand:SI 0 "compact_register_operand" "=q")
;; 	(sign_extend:SI (match_operand:QI 1 "compact_register_operand" "q")))]
;;   "TARGET_MIXED_CODE"
;;   "sexb_s %0,%1"
;;   [(set_attr "type" "unary")
;;    (set_attr "iscompact" "true")])

(define_insn "*extendqisi2_ac"
  [(set (match_operand:SI 0 "dest_reg_operand" "=Rcqq,w,r")
	(sign_extend:SI (match_operand:QI 1 "nonvol_nonimm_operand" "Rcqq,c,m")))]
  "TARGET_ARCOMPACT"
  "@
   sexb%? %0,%1%&
   sexb %0,%1
   ldb.x%U1 %0,%1"
  [(set_attr "type" "unary,unary,load")
   (set_attr "iscompact" "true,false,false")
   (set_attr "cond" "nocond,nocond,nocond")])

(define_expand "extendqisi2"
  [(set (match_operand:SI 0 "dest_reg_operand" "")
	(sign_extend:SI (match_operand:QI 1 "nonvol_nonimm_operand" "")))]
  ""
  "if (prepare_extend_operands (operands, SIGN_EXTEND, SImode)) DONE;"
)

(define_insn "*extendhisi2_a4"
  [(set (match_operand:SI 0 "dest_reg_operand" "=r")
	(sign_extend:SI (match_operand:HI 1 "register_operand" "r")))]
  "TARGET_A4"
  "sexw%? %0,%1"
  [(set_attr "type" "unary")])

;; (define_insn "*extendhisi2_mixed"
;;   [(set (match_operand:SI 0 "compact_register_operand" "=q")
;; 	(sign_extend:SI (match_operand:HI 1 "compact_register_operand" "q")))]
;;   "TARGET_MIXED_CODE"
;;   "sexw_s %0,%1"
;;   [(set_attr "type" "unary")
;;    (set_attr "iscompact" "true")])


(define_insn "*extendhisi2_i"
  [(set (match_operand:SI 0 "dest_reg_operand" "=Rcqq,w,r")
	(sign_extend:SI (match_operand:HI 1 "nonvol_nonimm_operand" "Rcqq,c,m")))]
  ""
  "@
   sexw%? %0,%1%&
   sexw %0,%1
   ldw.x%U1%V1 %0,%1"
  [(set_attr "type" "unary,unary,load")
   (set_attr "iscompact" "true,false,false")
   (set_attr "cond" "nocond,nocond,nocond")])

(define_expand "extendhisi2"
  [(set (match_operand:SI 0 "dest_reg_operand" "")
	(sign_extend:SI (match_operand:HI 1 "nonvol_nonimm_operand" "")))]
  ""
  "if (prepare_extend_operands (operands, SIGN_EXTEND, SImode)) DONE;"
)

;; Unary arithmetic insns

;; We allow constant operands to enable late constant propagation, but it is
;; not worth while to have more than one dedicated alternative to output them -
;; if we are really worried about getting these the maximum benefit of all
;; the available alternatives, we should add an extra pass to fold such
;; operations to movsi.

;; Absolute instructions

(define_insn "*abssi2_mixed"
  [(set (match_operand:SI 0 "compact_register_operand" "=q")
        (abs:SI (match_operand:SI 1 "compact_register_operand" "q")))]
  "TARGET_MIXED_CODE"
  "abs%? %0,%1%&"
  [(set_attr "type" "two_cycle_core")
   (set_attr "iscompact" "true")])

(define_insn "abssi2"
  [(set (match_operand:SI 0 "dest_reg_operand" "=Rcq#q,w,w")
        (abs:SI (match_operand:SI 1 "nonmemory_operand" "Rcq#q,cL,Cal")))]
  "TARGET_ARCOMPACT"
  "abs%? %0,%1%&"
  [(set_attr "type" "two_cycle_core")
   (set_attr "length" "*,4,8")
   (set_attr "iscompact" "true,false,false")
   (set_attr "cond" "nocond,nocond,nocond")])

;; Maximum and minimum insns

(define_insn "smaxsi3"
   [(set (match_operand:SI 0 "dest_reg_operand"         "=Rcw, w,  w")
         (smax:SI (match_operand:SI 1 "register_operand"  "%0, c,  c")
                  (match_operand:SI 2 "nonmemory_operand" "cL,cL,Cal")))]
  "TARGET_MINMAX"
  "max%? %0,%1,%2"
  [(set_attr "type" "two_cycle_core")
   (set_attr "length" "4,4,8")
   (set_attr "cond" "canuse,nocond,nocond")]
)

(define_insn "sminsi3"
   [(set (match_operand:SI 0 "dest_reg_operand"         "=Rcw, w,  w")
         (smin:SI (match_operand:SI 1 "register_operand"  "%0, c,  c")
                  (match_operand:SI 2 "nonmemory_operand" "cL,cL,Cal")))]
  "TARGET_MINMAX"
  "min%? %0,%1,%2"
  [(set_attr "type" "two_cycle_core")
   (set_attr "length" "4,4,8")
   (set_attr "cond" "canuse,nocond,nocond")]
)

;; Arithmetic instructions.
;; (define_insn "*addsi3_mixed"
;;   [(set (match_operand:SI 0 "compact_register_operand" "=q,q,q,r")
;; 	(plus:SI (match_operand:SI 1 "compact_register_operand" "%q,0,0,r")
;; 		 (match_operand:SI 2 "nonmemory_operand" "qK,rO,Ji,rJi")))]
;;   "TARGET_MIXED_CODE"
;;   "*
;;    {
;;      switch (which_alternative)
;;      {
;;        case 0:
;;          return \"add_s %0,%1,%2\";
;;        case 1:
;;          return \"add_s %0,%1,%2\";
;;        case 12:
;;          if (INTVAL (operands[2]) < 0)
;;             return \"sub%? %0,%1,%n2\"; 
;;          else
;;            return \"add%? %0,%1,%2\";
;;        case 2:
;;          return \"add_s %0,%1,%S2\";
;;        case 3:
;;          return \"add%? %0,%1,%S2\";
;;        default:
;;          abort ();
;;      }
;;    }"
;;   [(set_attr "iscompact" "true,true,true,false")
;;    (set_attr "length" "*,*,6,8")
;;    (set_attr "cond" "nocond,nocond,nocond,canuse")])

; We say an insn can be conditionalized if this doesn't introduce a long
; immediate.  We set the type such that we still have good scheduling if the
; insn is conditionalized.
; ??? It would make sense to allow introduction of long immediates, but
;     we'd need to communicate to the ccfsm machinery the extra cost.
; The alternatives in the constraints still serve three purposes:
; - estimate insn size assuming conditional execution
; - guide reload to re-order the second and third operand to get a better fit.
; - give tentative insn type to guide scheduling
;   N.B. "%" for commutativity doesn't help when there is another matching
;   (but longer) alternative.
(define_insn_and_split "*addsi3_mixed"
  ;;                                                      0       1   2   3   4   5   6    7     8    9     a   b c   d    e   f  10
  [(set (match_operand:SI 0 "dest_reg_operand"          "=Rcq#q,Rcq,Rcw,Rcw,Rcq,Rcb,Rcq, Rcw, Rcqq,Rcqq,    w,  w,w,  w,Rcqq,Rcw,  w")
	(plus:SI (match_operand:SI 1 "register_operand" "%0,      c,  0,  c,  0,  0,Rcb,   0, Rcqq,   0,    c,  c,0,  0,   0,  0,  c")
		 (match_operand:SI 2 "nonmemory_operand" "cL,     0, cL,  0,CL2,Csp,CM4,cCca,RcqqK,  cO,cLCmL,Cca,I,C2a, Cal,Cal,Cal")))]
  "TARGET_ARCOMPACT"
  "*if (which_alternative == 6)
      return arc_short_long (insn, \"add%? %0,%1,%2\", \"add1 %0,%1,%2/2\");
    return arc_output_addsi (operands,
			     arc_ccfsm_cond_exec_p () ? \"%?\" : \"\");"
  "&& reload_completed && get_attr_length (insn) == 8
   && satisfies_constraint_I (operands[2])"
  [(set (match_dup 0) (match_dup 3)) (set (match_dup 0) (match_dup 4))]
  "split_addsi (operands);"
  [(set_attr "type" "*,*,*,*,two_cycle_core,two_cycle_core,*,two_cycle_core,*,*,*,two_cycle_core,*,two_cycle_core,*,*,*")
   (set (attr "iscompact")
	(cond [(eq (symbol_ref "*arc_output_addsi (operands, 0)") (const_int 0))
	       (const_string "false")
	       (match_operand 2 "long_immediate_operand" "")
	       (const_string "maybe_limm")]
	      (const_string "maybe")))
   (set_attr "length" "*,*,4,4,*,*,*,4,*,*,4,4,4,4,*,8,8")
   (set_attr "cond" "canuse,canuse,canuse,canuse,canuse,canuse,nocond,canuse,nocond,nocond,nocond,nocond,canuse_limm,canuse_limm,canuse,canuse,nocond")])

;; (define_insn "*addsi3_mixed"
;;   [(set (match_operand:SI 0 "dest_reg_operand" "=q,q,q,q,r,r,r,r,r,r,r,r,r")
;; 	(plus:SI (match_operand:SI 1 "register_operand" "%q,0,0,0,0,r,0,r,0,0,r,0,r")
;; 		 (match_operand:SI 2 "nonmemory_operand" "qK,rO,J,i,r,r,L,L,I,J,J,i,i")))]
;;   ""
;;   "*
;;    {
;;      switch (which_alternative)
;;      {
;;        case 0:
;;          return \"add_s %0,%1,%2\";
;;        case 1:
;;          return \"add_s %0,%1,%2\";
;;        case 4:
;;          return \"add%? %0,%1,%2\";
;;        case 5:
;;          return \"add %0,%1,%2\";
;;        case 6:
;;          return \"add%? %0,%1,%2\";
;;        case 7:
;;          return \"add %0,%1,%2\";
;;        case 8:
;;          return \"add %0,%1,%2\";
;;        case 2:
;;          {
;;            int intval = INTVAL (operands[2]);
;;            if (intval < 0)
;;             {
;;               if (-intval< 0x20)
;;                 return \"sub_s %0,%1,%n2\"; 
;;               else
;;                 return \"sub %0,%1,%n2\"; 
;;             }
;;          else
;;            return \"add_s %0,%1,%2\";
;;          }
;;        case 3:
;;          return \"add_s %0,%1,%S2\";
;;        case 9:
;;          if (INTVAL (operands[2]) < 0)
;;             return \"sub%? %0,%1,%n2\"; 
;;          else
;;            return \"add%? %0,%1,%2\";
;;        case 10:
;;          if (INTVAL (operands[2]) < 0)
;;             return \"sub %0,%1,%n2\"; 
;;          else
;;            return \"add %0,%1,%2\";
;;        case 11:
;;          return \"add%? %0,%1,%S2\";
;;        case 12:
;;          return \"add %0,%1,%S2\";
;;        default:
;;          abort ();
;;      }
;;    }"
;;   [(set_attr "iscompact" "true,true,false,true,false,false,false,false,false,false,false,false,false")
;;    (set_attr "length" "2,2,8,6,4,4,4,4,4,8,8,8,8")
;;    (set_attr "cond" "nocond,nocond,nocond,nocond,canuse,nocond,canuse,nocond,nocond,canuse,nocond,canuse,nocond")])

;; ARC700/ARC600 multiply
;; SI <- SI * SI

(define_expand "mulsi3"
 [(set (match_operand:SI 0 "nonimmediate_operand"            "")
        (mult:SI (match_operand:SI 1 "register_operand"  "")
                 (match_operand:SI 2 "nonmemory_operand" "")))]
  "(TARGET_ARC700 && !TARGET_NOMPY_SET)
    || TARGET_MUL64_SET || TARGET_MULMAC_32BY16_SET"
  "
{
  if ((TARGET_ARC700 && !TARGET_NOMPY_SET) &&
      !register_operand (operands[0], SImode))
    {
      rtx result = gen_reg_rtx (SImode);

      emit_insn (gen_mulsi3 (result, operands[1], operands[2]));
      emit_move_insn (operands[0], result);
      DONE;
    }
  else if (TARGET_MUL64_SET)
    {
      emit_insn (gen_mulsi_600 (operands[1], operands[2],
				gen_mlo (), gen_mhi ()));
      emit_move_insn (operands[0], gen_mlo ());
      DONE;
    }
  else if (TARGET_MULMAC_32BY16_SET)
    {
      if (immediate_operand (operands[2], SImode)
	  && INTVAL (operands[2]) >= 0
	  && INTVAL (operands[2]) <= 65535)
        {
	  emit_insn (gen_umul_600 (operands[1], operands[2],
				     gen_acc2 (), gen_acc1 ()));
	  emit_move_insn (operands[0], gen_acc2 ());
	  DONE;
	}
      operands[2] = force_reg (SImode, operands[2]);
      emit_insn (gen_umul_600 (operands[1], operands[2],
			       gen_acc2 (), gen_acc1 ()));
      emit_insn (gen_mac_600 (operands[1], operands[2],
			       gen_acc2 (), gen_acc1 ()));
      emit_move_insn (operands[0], gen_acc2 ());
      DONE;
    }
}")

; mululw conditional execution without a LIMM clobbers an input register;
; we'd need a different pattern to describe this.
(define_insn "umul_600"
  [(set (match_operand:SI 2 "acc2_operand" "")
        (mult:SI (match_operand:SI 0 "register_operand"  "c,c,c")
                 (zero_extract:SI (match_operand:SI 1 "nonmemory_operand"  "c,L,J")
                                  (const_int 16)
                                  (const_int 0))))
   (clobber (match_operand:SI 3 "acc1_operand" ""))]
  "TARGET_MULMAC_32BY16_SET"
  "mululw%? 0, %0, %1"
  [(set_attr "length" "4,4,8")
   (set_attr "type" "mulmac_600, mulmac_600, mulmac_600")
   (set_attr "cond" "nocond, canuse_limm, canuse")])

(define_insn "mac_600"
  [(set (match_operand:SI 2 "acc2_operand" "")
	(plus:SI
	  (mult:SI (match_operand:SI 0 "register_operand" "c,c,c")
		   (ashift:SI
		     (zero_extract:SI (match_operand:SI 1 "nonmemory_operand" "c,L,J")
				      (const_int 16)
				      (const_int 16))
		     (const_int 16)))
	  (match_dup 2)))
   (clobber (match_operand:SI 3 "acc1_operand" ""))]
  "TARGET_MULMAC_32BY16_SET"
  "machlw%? 0, %0, %1"
  [(set_attr "length" "4,4,8")
   (set_attr "type" "mulmac_600, mulmac_600, mulmac_600")
   (set_attr "cond" "nocond, canuse_limm, canuse")])

(define_insn "mulsi_600"
  [(set (match_operand:SI 2 "mlo_operand" "")
	(mult:SI (match_operand:SI 0 "register_operand"  "Rcq#q,c,c,%c")
		 (match_operand:SI 1 "nonmemory_operand" "Rcq#q,cL,I,J")))
   (clobber (match_operand:SI 3 "mhi_operand" ""))]
  "TARGET_MUL64_SET"
  "mul64%? \t0, %0, %1%&"
  [(set_attr "length" "*,4,4,8")
  (set_attr "iscompact" "maybe,false,false,false")
  (set_attr "type" "multi,multi,multi,multi")
   (set_attr "cond" "canuse,canuse,canuse_limm,canuse")])

(define_insn "mulsidi_600"
  [(set (reg:DI 58)
	(mult:DI (sign_extend:DI
		   (match_operand:SI 0 "register_operand"  "Rcq#q,c,c,%c"))
		 (sign_extend:DI
		   (match_operand:SI 1 "register_operand" "Rcq#q,cL,I,J"))))]
  "TARGET_MUL64_SET"
  "mul64%? \t0, %0, %1%&"
  [(set_attr "length" "*,4,4,8")
  (set_attr "iscompact" "maybe,false,false,false")
  (set_attr "type" "multi,multi,multi,multi")
   (set_attr "cond" "canuse,canuse,canuse_limm,canuse")])

(define_insn "umulsidi_600"
  [(set (reg:DI 58)
	(mult:DI (zero_extend:DI
		   (match_operand:SI 0 "register_operand"  "Rcq#q,c,c,%c"))
		 (sign_extend:DI
		   (match_operand:SI 1 "register_operand" "Rcq#q,cL,I,J"))))]
  "TARGET_MUL64_SET"
  "mulu64%? \t0, %0, %1%&"
  [(set_attr "length" "*,4,4,8")
  (set_attr "iscompact" "maybe,false,false,false")
  (set_attr "type" "umulti")
   (set_attr "cond" "canuse,canuse,canuse_limm,canuse")])

; ARC700 mpy* instructions: This is a multi-cycle extension, and thus 'w'
; may not be used as destination constraint.

; The result of mpy and mpyu is the same except for flag setting (if enabled),
; but mpyu is faster for the standard multiplier.
(define_insn "mulsi3_700"
 [(set (match_operand:SI 0 "dest_reg_operand"          "=Rcr, r,r,Rcr,  r")
        (mult:SI (match_operand:SI 1 "register_operand"  " 0, c,0,  0,  c")
                 (match_operand:SI 2 "nonmemory_operand" "cL,cL,I,Cal,Cal")))]
"TARGET_ARC700 && !TARGET_NOMPY_SET"
  "mpyu%? %0,%1,%2"
  [(set_attr "length" "4,4,4,8,8")
  (set_attr "type" "umulti")
  (set_attr "cond" "canuse,nocond,canuse_limm,canuse,nocond")])


(define_expand "mulsidi3"
  [(set (match_operand:DI 0 "nonimmediate_operand" "")
        (mult:DI (sign_extend:DI(match_operand:SI 1 "register_operand" ""))
                 (sign_extend:DI(match_operand:SI 2 "nonmemory_operand" ""))))]
  "(TARGET_ARC700 && !TARGET_NOMPY_SET)
   || TARGET_MULMAC_32BY16_SET"
"
{
  if ((TARGET_ARC700 && !TARGET_NOMPY_SET))
    {
      operands[2] = force_reg (SImode, operands[2]);
      if (!register_operand (operands[0], DImode))
	{
	  rtx result = gen_reg_rtx (DImode);

	  operands[2] = force_reg (SImode, operands[2]);
	  emit_insn (gen_mulsidi3 (result, operands[1], operands[2]));
	  emit_move_insn (operands[0], result);
	  DONE;
	}
    }
  if (TARGET_MUL64_SET)
    {
      operands[2] = force_reg (SImode, operands[2]);
      emit_insn (gen_mulsidi_600 (operands[1], operands[2]));
      emit_move_insn (operands[0], gen_rtx_REG (DImode, 58));
    }
  else if (TARGET_MULMAC_32BY16_SET)
    {
      rtx result_hi = gen_highpart(SImode, operands[0]);
      rtx result_low = gen_lowpart(SImode, operands[0]);

      emit_insn (gen_mul64_600 (operands[1], operands[2]));
      emit_insn (gen_mac64_600 (result_hi, operands[1], operands[2]));
      emit_move_insn (result_low, gen_acc2 ());
      DONE;
    }
}")

(define_insn "mul64_600"
  [(set (reg:DI 56)
        (mult:DI (sign_extend:DI (match_operand:SI 0 "register_operand"  "c,c,c"))
                 (zero_extract:DI (match_operand:SI 1 "nonmemory_operand"  "c,L,J")
                                  (const_int 16)
                                  (const_int 0))))
  ]
  "TARGET_MULMAC_32BY16_SET"
  "mullw%? 0, %0, %1"
  [(set_attr "length" "4,4,8")
   (set_attr "type" "mulmac_600")
   (set_attr "cond" "nocond, canuse_limm, canuse")])


;; ??? check if this is canonical rtl
(define_insn "mac64_600"
  [(set (reg:DI 56)
        (plus:DI
	  (mult:DI (sign_extend:DI (match_operand:SI 1 "register_operand" "c,c,c"))
		   (ashift:DI
		     (sign_extract:DI (match_operand:SI 2 "nonmemory_operand" "c,L,J")
				      (const_int 16) (const_int 16))
		     (const_int 16)))
	  (reg:DI 56)))
   (set (match_operand:SI 0 "register_operand" "=w,w,w")
	(zero_extract:SI
	  (plus:DI
	    (mult:DI (sign_extend:DI (match_dup 1))
		     (ashift:DI
		       (sign_extract:DI (match_dup 2)
					(const_int 16) (const_int 16))
			  (const_int 16)))
	    (reg:DI 56))
	  (const_int 32) (const_int 32)))]
  "TARGET_MULMAC_32BY16_SET"
  "machlw%? %0, %1, %2"
  [(set_attr "length" "4,4,8")
   (set_attr "type" "mulmac_600")
   (set_attr "cond" "nocond, canuse_limm, canuse")])


;; DI <- DI(signed SI) * DI(signed SI)
(define_insn_and_split "mulsidi3_700"
  [(set (match_operand:DI 0 "register_operand" "=&r")
	(mult:DI (sign_extend:DI (match_operand:SI 1 "register_operand" "%c"))
		 (sign_extend:DI (match_operand:SI 2 "register_operand" "cL"))))]
  "TARGET_ARC700 && !TARGET_NOMPY_SET"
  "#"
  "reload_completed"
  [(const_int 0)]
{
  int hi = !TARGET_BIG_ENDIAN;
  int lo = !hi;
  rtx l0 = operand_subword (operands[0], lo, 0, DImode);
  rtx h0 = operand_subword (operands[0], hi, 0, DImode);
  emit_insn (gen_mulsi3_highpart (h0, operands[1], operands[2]));
  emit_insn (gen_mulsi3_700 (l0, operands[1], operands[2]));
  DONE;
}
  [(set_attr "type" "multi")
   (set_attr "length" "8")])

(define_insn "mulsi3_highpart"
  [(set (match_operand:SI 0 "register_operand"                  "=Rcr,r,Rcr,r")
	(truncate:SI
	 (lshiftrt:DI
	  (mult:DI
	   (sign_extend:DI (match_operand:SI 1 "register_operand" "%0,c,  0,c"))
	   (sign_extend:DI (match_operand:SI 2 "extend_operand"    "c,c,  s,s")))
	  (const_int 32))))]
  "TARGET_ARC700 && !TARGET_NOMPY_SET"
  "mpyh%? %0,%1,%2"
  [(set_attr "length" "4,4,8,8")
   (set_attr "type" "multi")
   (set_attr "cond" "canuse,nocond,canuse,nocond")])

; Note that mpyhu has the same latency as mpy / mpyh,
; thus we use the type multi.
(define_insn "*umulsi3_highpart_i"
  [(set (match_operand:SI 0 "register_operand"                  "=Rcr,r,Rcr,r")
	(truncate:SI
	 (lshiftrt:DI
	  (mult:DI
	   (zero_extend:DI (match_operand:SI 1 "register_operand" "%0,c,  0,c"))
	   (zero_extend:DI (match_operand:SI 2 "extend_operand"    "c,c,  s,s")))
	  (const_int 32))))]
  "TARGET_ARC700 && !TARGET_NOMPY_SET"
  "mpyhu%? %0,%1,%2"
  [(set_attr "length" "4,4,8,8")
   (set_attr "type" "multi")
   (set_attr "cond" "canuse,nocond,canuse,nocond")])

;; (zero_extend:DI (const_int)) leads to internal errors in combine, so we
;; need a separate pattern for immediates
;; ??? This is fine for combine, but not for reload.
(define_insn "umulsi3_highpart_int"
  [(set (match_operand:SI 0 "register_operand"            "=Rcr, r, r,Rcr,  r")
	(truncate:SI
	 (lshiftrt:DI
	  (mult:DI
	   (zero_extend:DI (match_operand:SI 1 "register_operand"  " 0, c, 0,  0,  c"))
	   (match_operand:DI 2 "immediate_usidi_operand" "L, L, I, Cal, Cal"))
	  (const_int 32))))]
  "TARGET_ARC700 && !TARGET_NOMPY_SET"
  "mpyhu%? %0,%1,%2"
  [(set_attr "length" "4,4,4,8,8")
   (set_attr "type" "multi")
   (set_attr "cond" "canuse,nocond,canuse_limm,canuse,nocond")])

(define_expand "umulsi3_highpart"
  [(set (match_operand:SI 0 "general_operand"  "")
	(truncate:SI
	 (lshiftrt:DI
	  (mult:DI
	   (zero_extend:DI (match_operand:SI 1 "register_operand" ""))
	   (zero_extend:DI (match_operand:SI 2 "nonmemory_operand" "")))
	  (const_int 32))))]
  "TARGET_ARC700 && !TARGET_NOMPY_SET"
  "
{
  rtx target = operands[0];

  if (!register_operand (target, SImode))
    target = gen_reg_rtx (SImode);

  if (CONST_INT_P (operands[2]) && INTVAL (operands[2]) < 0)
    operands[2] = simplify_const_unary_operation (ZERO_EXTEND, DImode,
						  operands[2], SImode);
  else if (!immediate_operand (operands[2], SImode))
    operands[2] = gen_rtx_ZERO_EXTEND (DImode, operands[2]);
  emit_insn (gen_umulsi3_highpart_int (target, operands[1], operands[2]));
  if (target != operands[0])
    emit_move_insn (operands[0], target);
  DONE;
}")

(define_expand "umulsidi3"
  [(set (match_operand:DI 0 "nonimmediate_operand" "")
        (mult:DI (zero_extend:DI(match_operand:SI 1 "register_operand" ""))
                 (zero_extend:DI(match_operand:SI 2 "nonmemory_operand" ""))))]
  "(TARGET_ARC700 && !TARGET_NOMPY_SET)
   || TARGET_MULMAC_32BY16_SET"
"
{
  if ((TARGET_ARC700 && !TARGET_NOMPY_SET))
    {
      operands[2] = force_reg (SImode, operands[2]);
      if (!register_operand (operands[0], DImode))
	{
	  rtx result = gen_reg_rtx (DImode);

	  emit_insn (gen_umulsidi3 (result, operands[1], operands[2]));
	  emit_move_insn (operands[0], result);
	  DONE;
	}
    }
  else if (TARGET_MUL64_SET)
    {
      operands[2] = force_reg (SImode, operands[2]);
      emit_insn (gen_mulsidi_600 (operands[1], operands[2]));
      emit_move_insn (operands[0], gen_rtx_REG (DImode, 58));
    }
  else if (TARGET_MULMAC_32BY16_SET)
    {
      rtx result_hi = gen_reg_rtx (SImode);
      rtx result_low = gen_reg_rtx (SImode);

      result_hi = gen_highpart(SImode , operands[0]);
      result_low = gen_lowpart(SImode , operands[0]);

      emit_insn (gen_umul64_600 (operands[1], operands[2]));
      emit_insn (gen_umac64_600 (result_hi, operands[1], operands[2]));
      emit_move_insn (result_low, gen_acc2 ());
      DONE;
    }
}")

(define_insn "umul64_600"
  [(set (reg:DI 56)
        (mult:DI (zero_extend:DI (match_operand:SI 0 "register_operand"  "c,c,c"))
                 (zero_extract:DI (match_operand:SI 1 "nonmemory_operand"  "c,L,Cal")
                                  (const_int 16)
                                  (const_int 0))))
  ]
  "TARGET_MULMAC_32BY16_SET"
  "mululw%? 0, %0, %1"
  [(set_attr "length" "4,4,8")
   (set_attr "type" "mulmac_600")
   (set_attr "cond" "nocond, canuse_limm, canuse")])


(define_insn "umac64_600"
  [(set (reg:DI 56)
        (plus:DI
	  (mult:DI (zero_extend:DI (match_operand:SI 1 "register_operand" "c,c,c"))
		   (ashift:DI
		     (zero_extract:DI (match_operand:SI 2 "nonmemory_operand" "c,L,Cal")
				      (const_int 16) (const_int 16))
		     (const_int 16)))
	  (reg:DI 56)))
   (set (match_operand:SI 0 "register_operand" "=w,w,w")
	(zero_extract:SI
	  (plus:DI
	    (mult:DI (zero_extend:DI (match_dup 1))
		     (ashift:DI
		       (zero_extract:DI (match_dup 2)
					(const_int 16) (const_int 16))
			  (const_int 16)))
	    (reg:DI 56))
	  (const_int 32) (const_int 32)))]
  "TARGET_MULMAC_32BY16_SET"
  "machulw%? %0, %1, %2"
 [(set_attr "length" "4,4,8")
  (set_attr "type" "mulmac_600")
  (set_attr "cond" "nocond, canuse_limm, canuse")])



;; DI <- DI(unsigned SI) * DI(unsigned SI)
(define_insn_and_split "umulsidi3_700"
  [(set (match_operand:DI 0 "dest_reg_operand" "=&r")
	(mult:DI (zero_extend:DI (match_operand:SI 1 "register_operand" "%c"))
		 (zero_extend:DI (match_operand:SI 2 "register_operand" "c"))))]
;;		 (zero_extend:DI (match_operand:SI 2 "register_operand" "rL"))))]
  "TARGET_ARC700 && !TARGET_NOMPY_SET"
  "#"
  "reload_completed"
  [(const_int 0)]
{
  int hi = !TARGET_BIG_ENDIAN;
  int lo = !hi;
  rtx l0 = operand_subword (operands[0], lo, 0, DImode);
  rtx h0 = operand_subword (operands[0], hi, 0, DImode);
  emit_insn (gen_umulsi3_highpart (h0, operands[1], operands[2]));
  emit_insn (gen_mulsi3_700 (l0, operands[1], operands[2]));
  DONE;
}
  [(set_attr "type" "umulti")
  (set_attr "length" "8")])

(define_expand "addsi3"
  [(set (match_operand:SI 0 "dest_reg_operand" "")
	(plus:SI (match_operand:SI 1 "register_operand" "")
		 (match_operand:SI 2 "nonmemory_operand" "")))]
  ""
  "if (flag_pic && arc_raw_symbolic_reference_mentioned_p (operands[2]) ) 
     {
       operands[2]=force_reg(SImode, operands[2]);
     }
  else if (!TARGET_NO_SDATA_SET && small_data_pattern (operands[2], Pmode))
   {
      operands[2] = force_reg (SImode, arc_rewrite_small_data (operands[2]));
   }

  ")

(define_insn "*addsi3_insn_a4"
  [(set (match_operand:SI 0 "dest_reg_operand" "=r,r,r,r,r,r,r,r,r")
	(plus:SI (match_operand:SI 1 "register_operand" "%0,r,0,r,0,0,r,0,r")
		 (match_operand:SI 2 "nonmemory_operand" "r,r,L,L,I,J,J,i,i")))]
  "TARGET_A4"
  "*
   {
     switch (which_alternative)
     {
       case 1:
         return \"add %0,%1,%2\";
       case 0:
         return \"add%? %0,%1,%2\";
       case 3:
         return \"add %0,%1,%2\";
       case 2:
         return \"add%? %0,%1,%2\";
       case 4:
         return \"add %0,%1,%2\";
       case 6:
         if (INTVAL (operands[2]) < 0)
            return \"sub %0,%1,%n2\"; 
         else
           return \"add %0,%1,%2\";
       case 5:
         if (INTVAL (operands[2]) < 0)
            return \"sub%? %0,%1,%n2\"; 
         else
           return \"add%? %0,%1,%2\";
       case 8:
         return \"add %0,%1,%S2\";
       case 7:
         return \"add%? %0,%1,%S2\";
       default:
         gcc_unreachable ();
     }
   }"
  [(set_attr "length" "4,4,4,4,4,8,8,8,8")
  (set_attr "cond" "canuse,nocond,canuse,nocond,nocond,canuse,nocond,canuse,nocond")]
)

(define_expand "adddi3"
  [(parallel [(set (match_operand:DI 0 "dest_reg_operand" "")
		   (plus:DI (match_operand:DI 1 "register_operand" "")
			    (match_operand:DI 2 "nonmemory_operand" "")))
	      (clobber (reg:CC CC_REG))])]
  ""
{
  if (TARGET_EXPAND_ADDDI)
    {
      rtx l0 = gen_lowpart (SImode, operands[0]);
      rtx h0 = disi_highpart (operands[0]);
      rtx l1 = gen_lowpart (SImode, operands[1]);
      rtx h1 = disi_highpart (operands[1]);
      rtx l2 = gen_lowpart (SImode, operands[2]);
      rtx h2 = disi_highpart (operands[2]);
      rtx cc_c = gen_rtx_REG (CC_Cmode, 61);

      if (CONST_INT_P (h2) && INTVAL (h2) < 0 && SIGNED_INT12 (INTVAL (h2)))
	{
	  emit_insn (gen_sub_f (l0, l1, gen_int_mode (-INTVAL (l2), SImode)));
	  emit_insn (gen_sbc (h0, h1,
			      gen_int_mode (-INTVAL (h2) - (l1 != 0), SImode),
			      cc_c));
	  DONE;
	}
      emit_insn (gen_add_f (l0, l1, l2));
      emit_insn (gen_adc (h0, h1, h2));
      DONE;
    }
})

; This assumes that there can be no strictly partial overlap between
; operands[1] and operands[2].
(define_insn_and_split "*adddi3_i"
  [(set (match_operand:DI 0 "dest_reg_operand" "=&w,w,w")
	(plus:DI (match_operand:DI 1 "register_operand" "%c,0,c")
		 (match_operand:DI 2 "nonmemory_operand" "ci,ci,!i")))
   (clobber (reg:CC CC_REG))]
  ""
  "#"
  "reload_completed"
  [(const_int 0)]
{
  int hi = !TARGET_BIG_ENDIAN;
  int lo = !hi;
  rtx l0 = operand_subword (operands[0], lo, 0, DImode);
  rtx h0 = operand_subword (operands[0], hi, 0, DImode);
  rtx l1 = operand_subword (operands[1], lo, 0, DImode);
  rtx h1 = operand_subword (operands[1], hi, 0, DImode);
  rtx l2 = operand_subword (operands[2], lo, 0, DImode);
  rtx h2 = operand_subword (operands[2], hi, 0, DImode);

  
  if (l2 == const0_rtx)
    {
      if (!rtx_equal_p (l0, l1) && !rtx_equal_p (l0, h1))
	emit_move_insn (l0, l1);
      emit_insn (gen_addsi3 (h0, h1, h2));
      if (!rtx_equal_p (l0, l1) && rtx_equal_p (l0, h1))
	emit_move_insn (l0, l1);
      DONE;
    }
  if (CONST_INT_P (operands[2]) && INTVAL (operands[2]) < 0
      && INTVAL (operands[2]) >= -0x7fffffff)
    {
      emit_insn (gen_subdi3_i (operands[0], operands[1],
		 GEN_INT (-INTVAL (operands[2]))));
      DONE;
    }
  if (rtx_equal_p (l0, h1))
    {
      if (h2 != const0_rtx)
	emit_insn (gen_addsi3 (h0, h1, h2));
      else if (!rtx_equal_p (h0, h1))
	emit_move_insn (h0, h1);
      emit_insn (gen_add_f (l0, l1, l2));
      emit_insn
	(gen_rtx_COND_EXEC
	  (VOIDmode,
	   gen_rtx_LTU (VOIDmode, gen_rtx_REG (CC_Cmode, CC_REG), GEN_INT (0)),
	   gen_rtx_SET (VOIDmode, h0, plus_constant (h0, 1))));
      DONE;
    }
  emit_insn (gen_add_f (l0, l1, l2));
  emit_insn (gen_adc (h0, h1, h2));
  DONE;
}
  [(set_attr "cond" "clob")
   (set_attr "type" "binary")
   (set_attr "length" "16,16,20")])

(define_insn "add_f"
  [(set (reg:CC_C CC_REG)
	(compare:CC_C
	  (plus:SI (match_operand:SI 1 "register_operand" "c,0,c")
		   (match_operand:SI 2 "nonmemory_operand" "cL,I,cCal"))
	  (match_dup 1)))
   (set (match_operand:SI 0 "dest_reg_operand" "=w,Rcw,w")
	(plus:SI (match_dup 1) (match_dup 2)))]
  ""
  "add.f %0,%1,%2"
  [(set_attr "cond" "set")
   (set_attr "type" "compare")
   (set_attr "length" "4,4,8")])

(define_insn "*add_f_2"
  [(set (reg:CC_C CC_REG)
	(compare:CC_C
	  (plus:SI (match_operand:SI 1 "register_operand" "c,0,c")
		   (match_operand:SI 2 "nonmemory_operand" "cL,I,cCal"))
	  (match_dup 2)))
   (set (match_operand:SI 0 "dest_reg_operand" "=w,Rcw,w")
	(plus:SI (match_dup 1) (match_dup 2)))]
  ""
  "add.f %0,%1,%2"
  [(set_attr "cond" "set")
   (set_attr "type" "compare")
   (set_attr "length" "4,4,8")])

; w/c/c comes first (rather than w/0/C_0) to prevent the middle-end
; needlessly prioritizing the matching constraint.
; Rcw/0/C_0 comes before w/c/L so that the lower latency conditional
; execution ; is used where possible.
(define_insn_and_split "adc"
  [(set (match_operand:SI 0 "dest_reg_operand" "=w,Rcw,w,Rcw,w")
	(plus:SI (plus:SI (ltu:SI (reg:CC_C CC_REG) (const_int 0))
			  (match_operand:SI 1 "nonmemory_operand"
							 "%c,0,c,0,cCal"))
		 (match_operand:SI 2 "nonmemory_operand" "c,C_0,L,I,cCal")))]
  "register_operand (operands[1], SImode)
   || register_operand (operands[2], SImode)"
  "@
	adc %0,%1,%2
	add.cs %0,%1,1
	adc %0,%1,%2
	adc %0,%1,%2
	adc %0,%1,%2"
  ; if we have a bad schedule after sched2, split.
  "reload_completed
   && !optimize_size && TARGET_ARC700
   && arc_scheduling_not_expected ()
   && arc_sets_cc_p (prev_nonnote_insn (insn))
   /* If next comes a return or other insn that needs a delay slot,
      expect the adc to get into the delay slot.  */
   && next_nonnote_insn (insn)
   && !arc_need_delay (next_nonnote_insn (insn))
   /* Restore operands before emitting.  */
   && (extract_insn_cached (insn), 1)"
  [(set (match_dup 0) (match_dup 3))
   (cond_exec
     (ltu (reg:CC_C CC_REG) (const_int 0))
     (set (match_dup 0) (plus:SI (match_dup 0) (const_int 1))))]
  "operands[3] = simplify_gen_binary (PLUS, SImode, operands[1], operands[2]);"
  [(set_attr "cond" "use")
   (set_attr "type" "cc_arith")
   (set_attr "length" "4,4,4,4,8")])

; combiner-splitter cmp / scc -> cmp / adc
(define_split
  [(set (match_operand:SI 0 "dest_reg_operand" "")
	(gtu:SI (match_operand:SI 1 "register_operand" "")
		(match_operand:SI 2 "register_operand" "")))
   (clobber (reg CC_REG))]
  ""
  [(set (reg:CC_C CC_REG) (compare:CC_C (match_dup 2) (match_dup 1)))
   (set (match_dup 0) (ltu:SI (reg:CC_C CC_REG) (const_int 0)))])

; combine won't work when an intermediate result is used later...
; add %0,%1,%2 ` cmp %0,%[12] -> add.f %0,%1,%2
(define_peephole2
  [(set (match_operand:SI 0 "dest_reg_operand" "")
	(plus:SI (match_operand:SI 1 "register_operand" "")
		 (match_operand:SI 2 "nonmemory_operand" "")))
   (set (reg:CC_C 61)
        (compare:CC_C (match_dup 0)
                      (match_operand:SI 3 "nonmemory_operand" "")))]
  "rtx_equal_p (operands[1], operands[3])
   || rtx_equal_p (operands[2], operands[3])"
  [(parallel
     [(set (reg:CC_C CC_REG)
	   (compare:CC_C (plus:SI (match_dup 1) (match_dup 2)) (match_dup 1)))
      (set (match_dup 0)
	   (plus:SI (match_dup 1) (match_dup 2)))])])

;(define_insn "*adc_0"
;  [(set (match_operand:SI 0 "dest_reg_operand" "=w")
;	(plus:SI (ltu:SI (reg:CC_C CC_REG) (const_int 0))
;		 (match_operand:SI 1 "register_operand" "c")))]
;  ""
;  "adc %0,%1,0"
;  [(set_attr "cond" "use")
;   (set_attr "type" "cc_arith")
;   (set_attr "length" "4")])
;
;(define_split
;  [(set (match_operand:SI 0 "dest_reg_operand" "=w")
;	(plus:SI (gtu:SI (match_operand:SI 1 "register_operand" "c")
;			 (match_operand:SI 2 "register_operand" "c"))
;		 (match_operand:SI 3 "register_operand" "c")))
;   (clobber (reg CC_REG))]
;  ""
;  [(set (reg:CC_C CC_REG) (compare:CC_C (match_dup 2) (match_dup 1)))
;   (set (match_dup 0)
;	(plus:SI (ltu:SI (reg:CC_C CC_REG) (const_int 0))
;		 (match_dup 3)))])

;; (define_insn "*subsi3_mixed"
;;   [(set (match_operand:SI 0 "register_operand" "=q,q,r")
;; 	(minus:SI (match_operand:SI 1 "register_operand" "q,0,r")
;; 		  (match_operand:SI 2 "nonmemory_operand" "K,qM,rJ")))]
;;   "TARGET_MIXED_CODE"
;;   "@
;;    sub_s %0,%1,%2
;;    sub_s %0,%1,%2
;;    sub%? %0,%1,%S2"
;;   [(set_attr "iscompact" "true,true,false")
;;    (set_attr "length" "2,2,*")])

(define_expand "subsi3"
  [(set (match_operand:SI 0 "dest_reg_operand" "")
	(minus:SI (match_operand:SI 1 "nonmemory_operand" "")
		 (match_operand:SI 2 "nonmemory_operand" "")))]
  ""
  "
{
  int c = 1;

  if (!register_operand (operands[2], SImode))
    {
      operands[1] = force_reg (SImode, operands[1]);
      c = 2;
    }
  if (flag_pic && arc_raw_symbolic_reference_mentioned_p (operands[c]) ) 
    operands[c] = force_reg (SImode, operands[c]);
  else if (!TARGET_NO_SDATA_SET && small_data_pattern (operands[c], Pmode))
      operands[c] = force_reg (SImode, arc_rewrite_small_data (operands[c]));
}")

; the casesi expander might generate a sub of zero, so we have to recognize it.
; combine should make such an insn go away.
(define_insn_and_split "subsi3_insn"
  [(set (match_operand:SI 0 "dest_reg_operand"          "=Rcqq,Rcw,Rcw,w,w,w,  w,  w,  w")
	(minus:SI (match_operand:SI 1 "nonmemory_operand"   "0,  0, cL,c,L,I,Cal,Cal,  c")
		  (match_operand:SI 2 "nonmemory_operand" "Rcqq, c,  0,c,c,0,  0,  c,Cal")))]
  "register_operand (operands[1], SImode)
   || register_operand (operands[2], SImode)"
  "@
    sub%? %0,%1,%2%&
    sub%? %0,%1,%2
    rsub%? %0,%2,%1
    sub %0,%1,%2
    rsub %0,%2,%1
    rsub %0,%2,%1
    rsub%? %0,%2,%1
    rsub %0,%2,%1
    sub %0,%1,%2"
  "reload_completed && get_attr_length (insn) == 8
   && satisfies_constraint_I (operands[1])"
  [(set (match_dup 0) (match_dup 3)) (set (match_dup 0) (match_dup 4))]
  "split_subsi (operands);"
  [(set_attr "iscompact" "maybe,false,false,false,false,false,false,false, false")
  (set_attr "length" "*,4,4,4,4,4,8,8,8")
  (set_attr "cond" "canuse,canuse,canuse,nocond,nocond,canuse_limm,canuse,nocond,nocond")])

(define_expand "subdi3"
  [(parallel [(set (match_operand:DI 0 "dest_reg_operand" "")
		   (minus:DI (match_operand:DI 1 "nonmemory_operand" "")
			     (match_operand:DI 2 "nonmemory_operand" "")))
	      (clobber (reg:CC 61))])]
  ""
{
  if (!register_operand (operands[2], DImode))
    operands[1] = force_reg (DImode, operands[1]);
  if (TARGET_EXPAND_ADDDI)
    {
      rtx l0 = gen_lowpart (SImode, operands[0]);
      rtx h0 = disi_highpart (operands[0]);
      rtx l1 = gen_lowpart (SImode, operands[1]);
      rtx h1 = disi_highpart (operands[1]);
      rtx l2 = gen_lowpart (SImode, operands[2]);
      rtx h2 = disi_highpart (operands[2]);
      rtx cc_c = gen_rtx_REG (CC_Cmode, 61);

      emit_insn (gen_sub_f (l0, l1, l2));
      emit_insn (gen_sbc (h0, h1, h2, cc_c));
      DONE;
    }
})

(define_insn_and_split "subdi3_i"
  [(set (match_operand:DI 0 "dest_reg_operand" "=&w,w,w,w,w")
	(minus:DI (match_operand:DI 1 "nonmemory_operand" "ci,0,ci,c,!i")
		  (match_operand:DI 2 "nonmemory_operand" "ci,ci,0,!i,c")))
   (clobber (reg:CC 61))]
  "register_operand (operands[1], DImode)
   || register_operand (operands[2], DImode)"
  "#"
  "reload_completed"
  [(const_int 0)]
{
  int hi = !TARGET_BIG_ENDIAN;
  int lo = !hi;
  rtx l0 = operand_subword (operands[0], lo, 0, DImode);
  rtx h0 = operand_subword (operands[0], hi, 0, DImode);
  rtx l1 = operand_subword (operands[1], lo, 0, DImode);
  rtx h1 = operand_subword (operands[1], hi, 0, DImode);
  rtx l2 = operand_subword (operands[2], lo, 0, DImode);
  rtx h2 = operand_subword (operands[2], hi, 0, DImode);

  if (rtx_equal_p (l0, h1) || rtx_equal_p (l0, h2))
    {
      h1 = simplify_gen_binary (MINUS, SImode, h1, h2);
      if (!rtx_equal_p (h0, h1))
	emit_insn (gen_rtx_SET (VOIDmode, h0, h1));
      emit_insn (gen_sub_f (l0, l1, l2));
      emit_insn
	(gen_rtx_COND_EXEC
	  (VOIDmode,
	   gen_rtx_LTU (VOIDmode, gen_rtx_REG (CC_Cmode, CC_REG), GEN_INT (0)),
	   gen_rtx_SET (VOIDmode, h0, plus_constant (h0, -1))));
      DONE;
    }
  emit_insn (gen_sub_f (l0, l1, l2));
  emit_insn (gen_sbc (h0, h1, h2, gen_rtx_REG (CCmode, CC_REG)));
  DONE;
}
  [(set_attr "cond" "clob")
   (set_attr "length" "16,16,16,20,20")])

(define_insn "*sbc_0"
  [(set (match_operand:SI 0 "dest_reg_operand" "=w")
	(minus:SI (match_operand:SI 1 "register_operand" "c")
		  (ltu:SI (match_operand:CC_C 2 "cc_use_register")
			  (const_int 0))))]
  ""
  "sbc %0,%1,0"
  [(set_attr "cond" "use")
   (set_attr "type" "cc_arith")
   (set_attr "length" "4")])

; w/c/c comes first (rather than Rcw/0/C_0) to prevent the middle-end
; needlessly prioritizing the matching constraint.
; Rcw/0/C_0 comes before w/c/L so that the lower latency conditional execution
; is used where possible.
(define_insn_and_split "sbc"
  [(set (match_operand:SI 0 "dest_reg_operand" "=w,Rcw,w,Rcw,w")
	(minus:SI (minus:SI (match_operand:SI 1 "nonmemory_operand"
						"c,0,c,0,cCal")
			    (ltu:SI (match_operand:CC_C 3 "cc_use_register")
				    (const_int 0)))
		  (match_operand:SI 2 "nonmemory_operand" "c,C_0,L,I,cCal")))]
  "register_operand (operands[1], SImode)
   || register_operand (operands[2], SImode)"
  "@
	sbc %0,%1,%2
	sub.cs %0,%1,1
	sbc %0,%1,%2
	sbc %0,%1,%2
	sbc %0,%1,%2"
  ; if we have a bad schedule after sched2, split.
  "reload_completed
   && !optimize_size && TARGET_ARC700
   && arc_scheduling_not_expected ()
   && arc_sets_cc_p (prev_nonnote_insn (insn))
   /* If next comes a return or other insn that needs a delay slot,
      expect the adc to get into the delay slot.  */
   && next_nonnote_insn (insn)
   && !arc_need_delay (next_nonnote_insn (insn))
   /* Restore operands before emitting.  */
   && (extract_insn_cached (insn), 1)"
  [(set (match_dup 0) (match_dup 4))
   (cond_exec
     (ltu (reg:CC_C CC_REG) (const_int 0))
     (set (match_dup 0) (plus:SI (match_dup 0) (const_int -1))))]
  "operands[4] = simplify_gen_binary (MINUS, SImode, operands[1], operands[2]);"
  [(set_attr "cond" "use")
   (set_attr "type" "cc_arith")
   (set_attr "length" "4,4,4,4,8")])

(define_insn "sub_f"
  [(set (reg:CC CC_REG)
        (compare:CC (match_operand:SI 1 "nonmemory_operand" " c,L,0,I,c,Cal")
		    (match_operand:SI 2 "nonmemory_operand" "cL,c,I,0,Cal,c")))
   (set (match_operand:SI 0 "dest_reg_operand" "=w,w,Rcw,Rcw,w,w")
	(minus:SI (match_dup 1) (match_dup 2)))]
  "register_operand (operands[1], SImode)
   || register_operand (operands[2], SImode)"
  "@
	sub.f %0,%1,%2
	rsub.f %0,%2,%1
	sub.f %0,%1,%2
	rsub.f %0,%2,%1
	sub.f %0,%1,%2
	sub.f %0,%1,%2"
  [(set_attr "type" "compare")
   (set_attr "length" "4,4,4,4,8,8")])

; combine won't work when an intermediate result is used later...
; add %0,%1,%2 ` cmp %0,%[12] -> add.f %0,%1,%2
(define_peephole2
  [(set (reg:CC CC_REG)
        (compare:CC (match_operand:SI 1 "register_operand" "")
		    (match_operand:SI 2 "nonmemory_operand" "")))
   (set (match_operand:SI 0 "dest_reg_operand" "")
	(minus:SI (match_dup 1) (match_dup 2)))]
  ""
  [(parallel
     [(set (reg:CC CC_REG) (compare:CC (match_dup 1) (match_dup 2)))
      (set (match_dup 0) (minus:SI (match_dup 1) (match_dup 2)))])])

(define_peephole2
  [(set (reg:CC CC_REG)
        (compare:CC (match_operand:SI 1 "register_operand" "")
		    (match_operand:SI 2 "nonmemory_operand" "")))
   (set (match_operand 3 "" "") (match_operand 4 "" ""))
   (set (match_operand:SI 0 "dest_reg_operand" "")
	(minus:SI (match_dup 1) (match_dup 2)))]
  "!reg_overlap_mentioned_p (operands[3], operands[1])
   && !reg_overlap_mentioned_p (operands[3], operands[2])
   && !reg_overlap_mentioned_p (operands[0], operands[4])
   && !reg_overlap_mentioned_p (operands[0], operands[3])"
  [(parallel
     [(set (reg:CC CC_REG) (compare:CC (match_dup 1) (match_dup 2)))
      (set (match_dup 0) (minus:SI (match_dup 1) (match_dup 2)))])
   (set (match_dup 3) (match_dup 4))])

(define_insn "*add_n"
  [(set (match_operand:SI 0 "dest_reg_operand" "=Rcqq,Rcw,w,w")
	(plus:SI (mult:SI (match_operand:SI 1 "register_operand" "Rcqq,c,c,c")
			  (match_operand:SI 2 "_2_4_8_operand" ""))
		 (match_operand:SI 3 "nonmemory_operand" "0,0,c,?Cal")))]
  ""
  "add%z2%? %0,%3,%1%&"
  [(set_attr "type" "shift")
   (set_attr "length" "*,4,4,8")
   (set_attr "cond" "canuse,canuse,nocond,nocond")
   (set_attr "iscompact" "maybe,false,false,false")])

;; N.B. sub[123] has the operands of the MINUS in the opposite order from
;; what synth_mult likes.
(define_insn "*sub_n"
  [(set (match_operand:SI 0 "dest_reg_operand" "=Rcw,w,w")
	(minus:SI (match_operand:SI 1 "nonmemory_operand" "0,c,?Cal")
		  (mult:SI (match_operand:SI 2 "register_operand" "c,c,c")
			   (match_operand:SI 3 "_2_4_8_operand" ""))))]
  ""
  "sub%z3%? %0,%1,%2"
  [(set_attr "type" "shift")
   (set_attr "length" "4,4,8")
   (set_attr "cond" "canuse,nocond,nocond")
   (set_attr "iscompact" "false")])

; ??? check if combine matches this.
(define_insn "*bset"
  [(set (match_operand:SI 0 "dest_reg_operand" "=Rcw,w,w") 
	(ior:SI (ashift:SI (const_int 1) 
			   (match_operand:SI 1 "nonmemory_operand" "cL,cL,c"))
		(match_operand:SI 2 "nonmemory_operand" "0,c,Cal")))]
  "TARGET_ARCOMPACT"
  "bset%? %0,%2,%1"
  [(set_attr "length" "4,4,8")
   (set_attr "cond" "canuse,nocond,nocond")]
)

; ??? check if combine matches this.
(define_insn "*bxor"
  [(set (match_operand:SI 0 "dest_reg_operand" "=Rcw,w,w") 
	(xor:SI (ashift:SI (const_int 1) 
			   (match_operand:SI 1 "nonmemory_operand" "cL,cL,c")) 
		(match_operand:SI 2 "nonmemory_operand" "0,c,Cal")))]
  "TARGET_ARCOMPACT"
  "bxor%? %0,%2,%1"
  [(set_attr "length" "4,4,8")
   (set_attr "cond" "canuse,nocond,nocond")]
)

; ??? check if combine matches this.
(define_insn "*bclr"
  [(set (match_operand:SI 0 "dest_reg_operand" "=Rcw,w,w") 
	(and:SI (not:SI (ashift:SI (const_int 1) 
				   (match_operand:SI 1 "nonmemory_operand" "cL,cL,c"))) 
		(match_operand:SI 2 "nonmemory_operand" "0,c,Cal")))]
  "TARGET_ARCOMPACT"
  "bclr%? %0,%2,%1"
  [(set_attr "length" "4,4,8")
   (set_attr "cond" "canuse,nocond,nocond")]
)

; ??? FIXME: find combine patterns for bmsk.

;;Following are the define_insns added for the purpose of peephole2's

; see also iorsi3 for use with constant bit number.
(define_insn "*bset_insn"
  [(set (match_operand:SI 0 "dest_reg_operand" "=Rcw,w,w") 
	(ior:SI (match_operand:SI 1 "nonmemory_operand" "0,c,Cal") 
		(ashift:SI (const_int 1) 
			   (match_operand:SI 2 "nonmemory_operand" "cL,cL,c"))) ) ]
  "TARGET_ARCOMPACT"
  "@
     bset%? %0,%1,%2 ;;peep2, constr 1
     bset %0,%1,%2 ;;peep2, constr 2
     bset %0,%S1,%2 ;;peep2, constr 3"
  [(set_attr "length" "4,4,8")]
)

; see also xorsi3 for use with constant bit number.
(define_insn "*bxor_insn"
  [(set (match_operand:SI 0 "dest_reg_operand" "=Rcw,w,w") 
	(xor:SI (match_operand:SI 1 "nonmemory_operand" "0,c,Cal") 
		(ashift:SI (const_int 1) 
			(match_operand:SI 2 "nonmemory_operand" "cL,cL,c"))) ) ]
  "TARGET_ARCOMPACT"
  "@
     bxor%? %0,%1,%2
     bxor %0,%1,%2
     bxor %0,%S1,%2"
  [(set_attr "length" "4,4,8")
   (set_attr "cond" "canuse,nocond,nocond")]
)

; see also andsi3 for use with constant bit number.
(define_insn "*bclr_insn"
  [(set (match_operand:SI 0 "dest_reg_operand" "=Rcw,w,w")
	(and:SI (not:SI (ashift:SI (const_int 1)
				   (match_operand:SI 2 "nonmemory_operand" "cL,rL,r")))
		(match_operand:SI 1 "nonmemory_operand" "0,c,Cal")))]
  "TARGET_ARCOMPACT"
  "@
     bclr%? %0,%1,%2
     bclr %0,%1,%2
     bclr %0,%S1,%2"
  [(set_attr "length" "4,4,8")
   (set_attr "cond" "canuse,nocond,nocond")]
)

; see also andsi3 for use with constant bit number.
(define_insn "*bmsk_insn"
  [(set (match_operand:SI 0 "dest_reg_operand" "=Rcw,w,w")
	(and:SI (match_operand:SI 1 "nonmemory_operand" "0,c,Cal")
		(plus:SI (ashift:SI (const_int 1)
				    (plus:SI (match_operand:SI 2 "nonmemory_operand" "rL,rL,r")
					     (const_int 1)))
			 (const_int -1))))]
  "TARGET_ARCOMPACT"
  "@
     bmsk%? %0,%S1,%2
     bmsk %0,%1,%2
     bmsk %0,%S1,%2"
  [(set_attr "length" "4,4,8")]
)

;;Instructions added for peephole2s end

;; Boolean instructions.

;; (define_insn "*andsi3_mixed"
;;   [(set (match_operand:SI 0 "compact_register_operand" "=q")
;; 	(and:SI (match_operand:SI 1 "compact_register_operand" "0") 
;; 		(match_operand:SI 2 "compact_register_operand" "q")))]
;;   "TARGET_MIXED_CODE"
;;   "and_s %0,%1,%2"
;;   [(set_attr "iscompact" "true")
;;    (set_attr "type" "binary")
;;    (set_attr "length" "2")])

(define_insn "*andsi3_insn_a4"
  [(set (match_operand:SI 0 "dest_reg_operand" "=r,r")
	(and:SI (match_operand:SI 1 "register_operand" "%r,r")
		(match_operand:SI 2 "nonmemory_operand" "r,Ji")))]
  "TARGET_A4"
  "@
    and%? %0,%1,%2
    and%? %0,%1,%S2"
  [(set_attr "type" "binary, binary")
   (set_attr "length" "4, 8")])

(define_expand "andsi3"
  [(set (match_operand:SI 0 "dest_reg_operand" "")
        (and:SI (match_operand:SI 1 "nonimmediate_operand" "")
                (match_operand:SI 2 "nonmemory_operand" "")))]
  "TARGET_ARCOMPACT"
  "if (!satisfies_constraint_Cux (operands[2]))
     operands[1] = force_reg (SImode, operands[1]);
   else if (!TARGET_NO_SDATA_SET && small_data_pattern (operands[1], Pmode))
     operands[1] = arc_rewrite_small_data (operands[1]);")

(define_insn "andsi3_i"
  [(set (match_operand:SI 0 "dest_reg_operand"          "=Rcqq,Rcq,Rcqq,Rcqq,Rcqq,Rcw,Rcw,Rcw,Rcw,Rcw,Rcw,  w,  w,  w,  w,w,Rcw,  w,  w")
	(and:SI (match_operand:SI 1 "nonimmediate_operand" "%0,Rcq,   0,   0,Rcqq,  0,  c,  0,  0,  0,  0,  c,  c,  c,  c,0,  0,  c,  o") 
		(match_operand:SI 2 "nonmemory_operand" " Rcqq,  0, C1p, Ccp, Cux, cL,  0,C1p,Ccp,CnL,  I, Lc,C1p,Ccp,CnL,I,Cal,Cal,Cux")))]
  "TARGET_ARCOMPACT
   && ((register_operand (operands[1], SImode)
       && nonmemory_operand (operands[2], SImode))
      || (memory_operand (operands[1], SImode)
	  && satisfies_constraint_Cux (operands[2])))"
  "*
{
  switch (which_alternative)
    {
    case 0: case 5: case 10: case 11: case 15: case 16: case 17:
      return \"and%? %0,%1,%2%&\";
    case 1: case 6:
      return \"and%? %0,%2,%1%&\";
    case 2: case 7: case 12:
      return \"bmsk%? %0,%1,%Z2%&\";
    case 3: case 8: case 13:
      return \"bclr%? %0,%1,%M2%&\";
    case 4:
      return (INTVAL (operands[2]) == 0xff
	      ? \"extb%? %0,%1%&\" : \"extw%? %0,%1%&\");
    case 9: case 14: return \"bic%? %0,%1,%n2-1\";
    case 18:
      if (TARGET_BIG_ENDIAN)
	{
	  rtx xop[2];

	  xop[0] = operands[0];
	  xop[1] = adjust_address (operands[1], QImode,
				   INTVAL (operands[2]) == 0xff ? 3 : 2);
	  output_asm_insn (INTVAL (operands[2]) == 0xff
			   ? \"ldb %0,%1\" : \"ldw %0,%1\",
			   xop);
	  return \"\";
	}
      return INTVAL (operands[2]) == 0xff ? \"ldb %0,%1\" : \"ldw %0,%1\";
    default:
      gcc_unreachable ();
    }
}"
  [(set_attr "iscompact" "maybe,maybe,maybe,maybe,true,false,false,false,false,false,false,false,false,false,false,false,false,false,false")
   (set_attr "type" "binary,binary,binary,binary,binary,binary,binary,binary,binary,binary,binary,binary,binary,binary,binary,binary,binary,binary,load")
   (set_attr "length" "*,*,*,*,*,4,4,4,4,4,4,4,4,4,4,4,8,8,*")
   (set_attr "cond" "canuse,canuse,canuse,canuse,nocond,canuse,canuse,canuse,canuse,canuse,canuse_limm,nocond,nocond,nocond,nocond,canuse_limm,canuse,nocond,nocond")])

; combiner splitter, pattern found in ldtoa.c .
; and op3,op0,op1 / cmp op3,op2 -> add op3,op0,op4 / bmsk.f 0,op3,op1
(define_split
  [(set (reg:CC_Z CC_REG)
	(compare:CC_Z (and:SI (match_operand:SI 0 "register_operand" "")
			      (match_operand 1 "const_int_operand" ""))
		      (match_operand 2 "const_int_operand" "")))
   (clobber (match_operand:SI 3 "register_operand" ""))]
  "((INTVAL (operands[1]) + 1) & INTVAL (operands[1])) == 0"
  [(set (match_dup 3)
	(plus:SI (match_dup 0) (match_dup 4)))
   (set (reg:CC_Z CC_REG)
	(compare:CC_Z (and:SI (match_dup 3) (match_dup 1))
		      (const_int 0)))]
  "operands[4] = GEN_INT ( -(~INTVAL (operands[1]) | INTVAL (operands[2])));")

;; (define_insn "andsi3"
;;   [(set (match_operand:SI 0 "register_operand" "=r,r")
;; 	(and:SI (match_operand:SI 1 "register_operand" "%r,r")
;; 		(match_operand:SI 2 "nonmemory_operand" "r,Ji")))]
;;   ""
;;   "@
;;     and%? %0,%1,%2
;;     and%? %0,%1,%S2"
;;   [(set_attr "type" "binary, binary")
;;    (set_attr "length" "4, 8")])

(define_insn_and_split "anddi3"
  [(set (match_operand:DI 0 "dest_reg_operand" "=&w,w,&w,w")
	(and:DI (match_operand:DI 1 "register_operand" "%c,0,c,0")
		(match_operand:DI 2 "nonmemory_operand" "c,c,H,H")))]
  "TARGET_OLD_DI_PATTERNS"
  "#"
  "reload_completed"
  [(set (match_dup 3) (and:SI (match_dup 4) (match_dup 5)))
   (set (match_dup 6) (and:SI (match_dup 7) (match_dup 8)))]
  "arc_split_dilogic (operands, AND); DONE;"
  [(set_attr "length" "8,8,16,16")])

;; (define_insn "*bicsi3_insn_mixed"
;;   [(set (match_operand:SI 0 "compact_register_operand" "=q")
;; 	(and:SI (match_operand:SI 1 "compact_register_operand" "0")
;; 		(not:SI (match_operand:SI 2 "compact_register_operand" "q"))))]
;;   "TARGET_MIXED_CODE"
;;   "bic_s %0,%1,%2"
;;   [(set_attr "iscompact" "true")])


;;bic define_insn that allows limm to be the first operand
(define_insn "*bicsi3_insn"
   [(set (match_operand:SI 0 "dest_reg_operand" "=Rcqq,Rcw,Rcw,Rcw,w,w,w")
 	(and:SI	(not:SI (match_operand:SI 1 "nonmemory_operand" "Rcqq,Lc,I,Cal,Lc,Cal,c"))
 		(match_operand:SI 2 "nonmemory_operand" "0,0,0,0,c,c,Cal")))]
  ""
  "@
   bic%? %0, %2, %1%& ;;constraint 0
   bic%? %0,%2,%1  ;;constraint 1
   bic %0,%2,%1    ;;constraint 2, FIXME: will it ever get generated ???
   bic%? %0,%2,%S1 ;;constraint 3, FIXME: will it ever get generated ???
   bic %0,%2,%1    ;;constraint 4
   bic %0,%2,%S1   ;;constraint 5, FIXME: will it ever get generated ???
   bic %0,%S2,%1   ;;constraint 6"
  [(set_attr "length" "*,4,4,8,4,8,8")
  (set_attr "iscompact" "maybe, false, false, false, false, false, false")
  (set_attr "cond" "canuse,canuse,canuse_limm,canuse,nocond,nocond,nocond")])


(define_insn "*iorsi3_a4"
  [(set (match_operand:SI 0 "dest_reg_operand" "=r,r")
	(ior:SI (match_operand:SI 1 "register_operand" "%r,r")
		(match_operand:SI 2 "nonmemory_operand" "r,Ji")))]
  "TARGET_A4"
  "@
    or%? %0,%1,%2
    or%? %0,%1,%S2"
  [(set_attr "type" "binary, binary")
   (set_attr "length" "4, 8")])

(define_insn "iorsi3"
  [(set (match_operand:SI 0 "dest_reg_operand"        "=Rcqq,Rcq,Rcqq,Rcw,Rcw,Rcw,Rcw,w,  w,w,Rcw,  w")
	(ior:SI (match_operand:SI 1 "nonmemory_operand" "% 0,Rcq,   0,  0,  c,  0, 0, c,  c,0,  0,  c")
		(match_operand:SI 2 "nonmemory_operand" "Rcqq, 0, C0p, cL,  0,C0p, I,cL,C0p,I,Cal,Cal")))]
  "TARGET_ARCOMPACT"
  "*
  switch (which_alternative)
    {
    case 0: case 3: case 6: case 7: case 9: case 10: case 11:
      return \"or%? %0,%1,%2%&\";
    case 1: case 4:
      return \"or%? %0,%2,%1%&\";
    case 2: case 5: case 8:
      return \"bset%? %0,%1,%z2%&\";
    default:
      gcc_unreachable ();
    }"
  [(set_attr "iscompact" "maybe,maybe,maybe,false,false,false,false,false,false,false,false,false")
   (set_attr "length" "*,*,*,4,4,4,4,4,4,4,8,8")
   (set_attr "cond" "canuse,canuse,canuse,canuse,canuse,canuse,canuse_limm,nocond,nocond,canuse_limm,canuse,nocond")])

;; (define_insn "iorsi3"
;;   [(set (match_operand:SI 0 "register_operand" "=r,r")
;; 	(ior:SI (match_operand:SI 1 "register_operand" "%r,r")
;; 		(match_operand:SI 2 "nonmemory_operand" "r,Ji")))]
;;   ""
;;   "@
;;     or%? %0,%1,%2
;;     or%? %0,%1,%S2"
;;   [(set_attr "type" "binary, binary")
;;    (set_attr "length" "4, 8")])

(define_insn_and_split "iordi3"
  [(set (match_operand:DI 0 "dest_reg_operand" "=&w,w,&w,w")
	(ior:DI (match_operand:DI 1 "register_operand" "%c,0,c,0")
		(match_operand:DI 2 "nonmemory_operand" "c,c,H,H")))]
  "TARGET_OLD_DI_PATTERNS"
  "#"
  "reload_completed"
  [(set (match_dup 3) (ior:SI (match_dup 4) (match_dup 5)))
   (set (match_dup 6) (ior:SI (match_dup 7) (match_dup 8)))]
  "arc_split_dilogic (operands, IOR); DONE;"
  [(set_attr "length" "8,8,16,16")])

;; (define_insn "*xorsi3_mixed"
;;   [(set (match_operand:SI 0 "compact_register_operand" "=q")
;; 	(xor:SI (match_operand:SI 1 "compact_register_operand" "0")
;; 		(match_operand:SI 2 "compact_register_operand" "q")))]
;;   "TARGET_MIXED_CODE"
;;   "xor_s %0,%1,%2"
;;   [(set_attr "iscompact" "true")
;;    (set_attr "length" "2")])

(define_insn "xorsi3"
  [(set (match_operand:SI 0 "dest_reg_operand"          "=Rcqq,Rcq,Rcw,Rcw,Rcw,Rcw, w,  w,w,  w,  w")
	(xor:SI (match_operand:SI 1 "register_operand"  "%0,   Rcq,  0,  c,  0,  0, c,  c,0,  0,  c")
		(match_operand:SI 2 "nonmemory_operand" " Rcqq,  0, cL,  0,C0p,  I,cL,C0p,I,Cal,Cal")))]
  ""
  "*
  switch (which_alternative)
    {
    case 0: case 2: case 5: case 6: case 8: case 9: case 10:
      return \"xor%? %0,%1,%2%&\";
    case 1: case 3:
      return \"xor%? %0,%2,%1%&\";
    case 4: case 7:
      return \"bxor%? %0,%1,%z2\";
    default:
      gcc_unreachable ();
    }
  "
  [(set_attr "iscompact" "maybe,maybe,false,false,false,false,false,false,false,false,false")
   (set_attr "type" "binary")
   (set_attr "length" "*,*,4,4,4,4,4,4,4,8,8")
   (set_attr "cond" "canuse,canuse,canuse,canuse,canuse,canuse_limm,nocond,nocond,canuse_limm,canuse,nocond")])

(define_insn_and_split "xordi3"
  [(set (match_operand:DI 0 "dest_reg_operand" "=&w,w,&w,w")
	(xor:DI (match_operand:DI 1 "register_operand" "%c,0,c,0")
		(match_operand:DI 2 "nonmemory_operand" "c,c,H,H")))]
  "TARGET_OLD_DI_PATTERNS"
  "#"
  "reload_completed"
  [(set (match_dup 3) (xor:SI (match_dup 4) (match_dup 5)))
   (set (match_dup 6) (xor:SI (match_dup 7) (match_dup 8)))]
  "arc_split_dilogic (operands, XOR); DONE;"
  [(set_attr "length" "8,8,16,16")])

(define_insn "*negsi2_a4"
  [(set (match_operand:SI 0 "dest_reg_operand" "=r")
	(neg:SI (match_operand:SI 1 "register_operand" "r")))]
  "TARGET_A4"
  "sub%? %0,0,%1"
  [(set_attr "type" "unary")
   (set_attr "length" "4")])

(define_insn "negsi2"
  [(set (match_operand:SI 0 "dest_reg_operand" "=Rcqq,Rcqq,Rcw,w")
	(neg:SI (match_operand:SI 1 "register_operand" "0,Rcqq,0,c")))]
  ""
  "neg%? %0,%1%&"
  [(set_attr "type" "unary")
   (set_attr "iscompact" "maybe,true,false,false")
   (set_attr "cond" "canuse,nocond,canuse,nocond")])

;; (define_insn "negsi2"
;;   [(set (match_operand:SI 0 "register_operand" "=r")
;; 	(neg:SI (match_operand:SI 1 "register_operand" "r")))]
;;   ""
;;   "sub%? %0,0,%1"
;;   [(set_attr "type" "unary")
;;    (set_attr "length" "8")])

(define_insn_and_split "negdi2"
  [(set (match_operand:DI 0 "dest_reg_operand" "=&w")
	(neg:DI (match_operand:DI 1 "register_operand" "c")))
   (clobber (reg:SI 61))]
  "TARGET_OLD_DI_PATTERNS"
  "#"
  ""
  [(set (match_dup 0) (minus:DI (const_int 0) (match_dup 1)))]
  ""
  [(set_attr "type" "unary")
   (set_attr "cond" "clob")
   (set_attr "length" "16")])

(define_insn "*one_cmplsi2_a4"
  [(set (match_operand:SI 0 "dest_reg_operand" "=r")
	(not:SI (match_operand:SI 1 "register_operand" "r")))]
  "TARGET_A4"
  "xor %0,%1,-1"
  [(set_attr "type" "unary")
   (set_attr "cond" "nocond")])

(define_insn "one_cmplsi2"
  [(set (match_operand:SI 0 "dest_reg_operand" "=Rcqq,w")
	(not:SI (match_operand:SI 1 "register_operand" "Rcqq,c")))]
  ""
  "not%? %0,%1%&"
  [(set_attr "type" "unary,unary")
   (set_attr "iscompact" "true,false")])

;; (define_insn "*one_cmplsi2_ac32"
;;   [(set (match_operand:SI 0 "register_operand" "=r")
;; 	(not:SI (match_operand:SI 1 "register_operand" "r")))]
;;   "TARGET_ARCOMPACT"
;;   "not %0,%1"
;;   [(set_attr "type" "unary")
;;    (set_attr "cond" "nocond")])

(define_insn "*one_cmpldi2_a4"
  [(set (match_operand:DI 0 "dest_reg_operand" "=&r")
	(not:DI (match_operand:DI 1 "register_operand" "r")))]
  "TARGET_A4"
  "xor %H0,%H1,-1\;xor %L0,%L1,-1"
  [(set_attr "type" "unary")
   (set_attr "cond" "nocond")
   (set_attr "length" "16")])

(define_insn_and_split "one_cmpldi2"
  [(set (match_operand:DI 0 "dest_reg_operand" "=q,w")
	(not:DI (match_operand:DI 1 "register_operand" "q,c")))]
  "TARGET_ARCOMPACT"
  "#"
  "&& reload_completed"
  [(set (match_dup 2) (not:SI (match_dup 3)))
   (set (match_dup 4) (not:SI (match_dup 5)))]
{
  int swap = (true_regnum (operands[0]) == true_regnum (operands[1]) + 1);

  operands[2] = operand_subword (operands[0], 0+swap, 0, DImode);
  operands[3] = operand_subword (operands[1], 0+swap, 0, DImode);
  operands[4] = operand_subword (operands[0], 1-swap, 0, DImode);
  operands[5] = operand_subword (operands[1], 1-swap, 0, DImode);
}
  [(set_attr "type" "unary,unary")
   (set_attr "cond" "nocond,nocond")
   (set_attr "length" "4,8")])

;; Shift instructions.

(define_expand "ashlsi3"
  [(set (match_operand:SI 0 "dest_reg_operand" "")
	(ashift:SI (match_operand:SI 1 "register_operand" "")
		   (match_operand:SI 2 "nonmemory_operand" "")))]
  ""
  "
{
  if (!TARGET_SHIFTER)
    {
/* ashwin : all gen_rtx (VAR.. ) are converted to gen_rtx_VAR (..) */
      emit_insn (gen_rtx_PARALLEL
		 (VOIDmode,
		  gen_rtvec (3,
			     gen_rtx_SET (VOIDmode, operands[0],
				      gen_rtx_ASHIFT (SImode, operands[1], operands[2])),
			     gen_rtx_CLOBBER (VOIDmode, gen_rtx_SCRATCH (SImode)),
			     gen_rtx_CLOBBER (VOIDmode, gen_rtx_REG (CCmode, 61))
			)));
      DONE;
    }
}")

(define_expand "ashrsi3"
  [(set (match_operand:SI 0 "dest_reg_operand" "")
	(ashiftrt:SI (match_operand:SI 1 "register_operand" "")
		     (match_operand:SI 2 "nonmemory_operand" "")))]
  ""
  "
{
  if (!TARGET_SHIFTER)
    {
      emit_insn (gen_rtx_PARALLEL
		 (VOIDmode,
		  gen_rtvec (3,
			     gen_rtx_SET (VOIDmode, operands[0],
				      gen_rtx_ASHIFTRT (SImode, operands[1], operands[2])),
			     gen_rtx_CLOBBER (VOIDmode, gen_rtx_SCRATCH (SImode)),
			     gen_rtx_CLOBBER (VOIDmode, gen_rtx_REG (CCmode, 61)))));
      DONE;
    }
}")

(define_expand "lshrsi3"
  [(set (match_operand:SI 0 "dest_reg_operand" "")
	(lshiftrt:SI (match_operand:SI 1 "register_operand" "")
		     (match_operand:SI 2 "nonmemory_operand" "")))]
  ""
  "
{
  if (!TARGET_SHIFTER)
    {
      emit_insn (gen_rtx_PARALLEL
		 (VOIDmode,
		  gen_rtvec (3,
			     gen_rtx_SET (VOIDmode, operands[0],
				      gen_rtx_LSHIFTRT (SImode, operands[1], operands[2])),
	 		     gen_rtx_CLOBBER (VOIDmode, gen_rtx_SCRATCH (SImode)),
                             gen_rtx_CLOBBER (VOIDmode, gen_rtx_REG (CCmode,61)))));
						      DONE;
    }
}")

(define_insn "*shift_si3"
  [(set (match_operand:SI 0 "dest_reg_operand" "=r")
	(match_operator:SI 3 "shift_operator"
			   [(match_operand:SI 1 "register_operand" "0")
			    (match_operand:SI 2 "nonmemory_operand" "rJ")]))
   (clobber (match_scratch:SI 4 "=&r"))
   (clobber (reg:CC 61))
  ]
  "!TARGET_SHIFTER"
  "* return output_shift (operands);"
  [(set_attr "type" "shift")
   (set_attr "length" "32")])

; asl, asr, lsr patterns:
; There is no point in including an 'I' alternative since only the lowest 5
; bits are used for the shift.  OTOH Cal can be useful if the shift amount
; is defined in an external symbol, as we don't have special relocations
; to truncate a symbol in a u6 immediate; but that's rather exotic, so only
; provide one alternatice for this, without condexec support.
(define_insn "*ashlsi3_insn_mixed"
  [(set (match_operand:SI 0 "dest_reg_operand"           "=Rcq,Rcqq,Rcqq,Rcw, w,   w")
        (ashift:SI (match_operand:SI 1 "nonmemory_operand" "!0,Rcqq,   0,  0, c,cCal")
                   (match_operand:SI 2 "nonmemory_operand"  "K,  K,RcqqM, cL,cL,cCal")))]
  "TARGET_SHIFTER
   && (register_operand (operands[1], SImode)
       || register_operand (operands[2], SImode))"
  "asl%? %0,%1,%2%&"
  [(set_attr "type" "shift")
   (set_attr "iscompact" "maybe,maybe,maybe,false,false,false")
   (set_attr "cond" "canuse,nocond,canuse,canuse,nocond,nocond")])

;; (define_insn "*ashlsi3_insn"
;;   [(set (match_operand:SI 0 "register_operand" "=r,r,r,r")
;;         (ashift:SI (match_operand:SI 1 "register_operand" "r,r,r,0")
;;                    (match_operand:SI 2 "nonmemory_operand" "N,r,Cal,rJ")))]
;;   "TARGET_SHIFTER"
;;   "@
;;    asl %0,%1
;;    asl %0, %1, %2
;;    asl %0, %1, %2
;;    asl%? %0,%1,%S2"
;;   [(set_attr "type" "shift,shift,shift,shift")
;;   (set_attr "length" "4, 4, 8, 8")
;;    (set_attr "cond" "nocond,nocond,nocond,canuse")])

;; (define_insn "*ashrsi3_insn_mixed"
;;   [(set (match_operand:SI 0 "register_operand" "=q,q,q,r,r")
;;         (ashiftrt:SI (match_operand:SI 1 "register_operand" "q,q,0,0,r")
;;                      (match_operand:SI 2 "nonmemory_operand" "N,K,qM,rJ,rJ")))]
;;   "TARGET_MIXED_CODE"
;;   "@
;;    asr_s %0,%1
;;    asr_s %0,%1,%2
;;    asr_s %0,%1,%2
;;    asr%? %0,%1,%S2
;;    asr   %0,%1,%S2"
;;   [(set_attr "type" "shift,shift,shift,shift,shift")
;;    (set_attr "iscompact" "true,true,true,false,false")])

;; (define_insn "*ashrsi3_insn"
;;   [(set (match_operand:SI 0 "register_operand" "=r,r,r")
;;         (ashiftrt:SI (match_operand:SI 1 "register_operand" "r,0,r")
;;                      (match_operand:SI 2 "nonmemory_operand" "N,rJ,rJ")))]
;;   "TARGET_SHIFTER"
;;   "@
;;    asr %0,%1
;;    asr%? %0,%1,%S2
;;    asr %0,%1,%S2"
;;   [(set_attr "type" "shift,shift,shift")
;;    (set_attr "cond" "nocond,canuse,nocond")])
(define_insn "*ashrsi3_insn_mixed"
  [(set (match_operand:SI 0 "dest_reg_operand"             "=Rcq,Rcqq,Rcqq,Rcw, w,   w")
        (ashiftrt:SI (match_operand:SI 1 "nonmemory_operand" "!0,Rcqq,   0,  0, c,cCal")
                   (match_operand:SI 2 "nonmemory_operand"    "K,  K,RcqqM, cL,cL,cCal")))]
  "TARGET_SHIFTER
   && (register_operand (operands[1], SImode)
       || register_operand (operands[2], SImode))"
  "asr%? %0,%1,%2%&"
  [(set_attr "type" "shift")
   (set_attr "iscompact" "maybe,maybe,maybe,false,false,false")
   (set_attr "cond" "canuse,nocond,canuse,canuse,nocond,nocond")])

;; (define_insn "*lshrsi3_insn_mixed"
;;   [(set (match_operand:SI 0 "register_operand" "=q,q,r")
;;         (lshiftrt:SI (match_operand:SI 1 "register_operand" "q,0,r")
;;                      (match_operand:SI 2 "nonmemory_operand" "N,qM,rJ")))]
;;   "TARGET_MIXED_CODE"
;;   "@
;;    lsr_s %0,%1
;;    lsr_s %0,%1,%2
;;    lsr%? %0,%1,%S2"
;;   [(set_attr "type" "shift,shift,shift")
;;    (set_attr "iscompact" "true,true,false")])

;; (define_insn "*lshrsi3_insn"
;;   [(set (match_operand:SI 0 "register_operand" "=r,r,r")
;;           (lshiftrt:SI (match_operand:SI 1 "register_operand" "r,0,r")
;;                      (match_operand:SI 2 "nonmemory_operand" "N,rJ,rJ")))]
;;   "TARGET_SHIFTER"
;;   "@
;;    lsr %0,%1
;;    lsr%? %0,%1,%S2
;;    lsr %0,%1,%S2"
;;   [(set_attr "type" "shift,shift,shift")
;;    (set_attr "cond" "nocond,canuse,nocond")])
(define_insn "*lshrsi3_insn_mixed"
  [(set (match_operand:SI 0 "dest_reg_operand"             "=Rcq,Rcqq,Rcqq,Rcw, w,   w")
        (lshiftrt:SI (match_operand:SI 1 "nonmemory_operand" "!0,Rcqq,   0,  0, c,cCal")
                   (match_operand:SI 2 "nonmemory_operand"    "N,  N,RcqqM, cL,cL,cCal")))]
  "TARGET_SHIFTER
   && (register_operand (operands[1], SImode)
       || register_operand (operands[2], SImode))"
  "*return (which_alternative <= 1 && !arc_ccfsm_cond_exec_p ()
	    ?  \"lsr%? %0,%1%&\" : \"lsr%? %0,%1,%2%&\");"
  [(set_attr "type" "shift")
   (set_attr "iscompact" "maybe,maybe,maybe,false,false,false")
   (set_attr "cond" "canuse,nocond,canuse,canuse,nocond,nocond")])

(define_insn "rotrsi3"
  [(set (match_operand:SI 0 "dest_reg_operand"             "=Rcw, w,   w")
        (rotatert:SI (match_operand:SI 1 "register_operand"  " 0,cL,cCal")
                     (match_operand:SI 2 "nonmemory_operand" "cL,cL,cCal")))]
  "TARGET_SHIFTER"
  "ror%? %0,%1,%2"
  [(set_attr "type" "shift,shift,shift")
   (set_attr "cond" "canuse,nocond,nocond")
   (set_attr "length" "4,4,8")])

;; Compare instructions.
;; This controls RTL generation and register allocation.

;; We generate RTL for comparisons and branches by having the cmpxx
;; patterns store away the operands.  Then, the scc and bcc patterns
;; emit RTL for both the compare and the branch.

(define_expand "cmpsi"
  [(set (reg:CC 61)
	(compare:CC (match_operand:SI 0 "register_operand" "")
		    (match_operand:SI 1 "nonmemory_operand" "")))]
  ""
  "
{
  arc_compare_op0 = operands[0];
  if (GET_CODE(operands[1]) == SYMBOL_REF && flag_pic)
    arc_compare_op1 = force_reg (SImode, operands[1]);
  else
   arc_compare_op1 = operands[1];
  DONE;
}")

;; ??? We may be able to relax this a bit by adding a new constraint for 0.
;; This assumes sub.f 0,symbol,0 is a valid insn.
;; Note that "sub.f 0,r0,1" is an 8 byte insn.  To avoid unnecessarily
;; creating 8 byte insns we duplicate %1 in the destination reg of the insn
;; if it's a small constant.

(define_insn "*cmpsi_cc_insn_a4"
  [(set (reg:CC 61)
	(compare:CC (match_operand:SI 0 "register_operand" "r,r,r")
		    (match_operand:SI 1 "nonmemory_operand" "r,I,Cal")))]
  "TARGET_A4"
  "@
   sub.f 0,%0,%1
   sub.f %1,%0,%1
   sub.f 0,%0,%S1"
  [(set_attr "type" "compare,compare,compare")
  ])

(define_insn "*cmpsi_cczn_insn_a4"
  [(set (reg:CC_ZN 61)
	(compare:CC_ZN (match_operand:SI 0 "register_operand" "r,r,r")
		       (match_operand:SI 1 "nonmemory_operand" "r,I,Cal")))]
  "TARGET_A4"
  "@
   sub.f 0,%0,%1
   sub.f %1,%0,%1
   sub.f 0,%0,%S1"
  [(set_attr "type" "compare,compare,compare")
  ])

;; (define_insn "*cmpsi_cc_insn_mixed"
;;   [(set (reg:CC 61)
;;         (compare:CC (match_operand:SI 0 "compact_register_operand" "q,r")
;;                     (match_operand:SI 1 "nonmemory_operand" "rO,rJ")))]
;;   "TARGET_MIXED_CODE"
;;   "@
;;    cmp_s %0,%1
;;    cmp%? %0,%S1"
;;   [(set_attr "type" "compare,compare")
;;    (set_attr "iscompact" "true,false")])

;; (define_insn "*cmpsi_cc_insn"
;;   [(set (reg:CC 61)
;;         (compare:CC (match_operand:SI 0 "register_operand" "r")
;;                     (match_operand:SI 1 "nonmemory_operand" "rJ")))]
;;   "TARGET_ARCOMPACT"
;;   "cmp%? %0,%S1"
;;   [(set_attr "type" "compare")
;;   ])

;; ??? Could add a peephole to generate compare with swapped operands and
;; modifed cc user if second, but not first operand is a compact register.
(define_insn "cmpsi_cc_insn_mixed"
  [(set (reg:CC 61)
        (compare:CC (match_operand:SI 0 "register_operand"  "Rcq#q, c, qRcq, c")
                    (match_operand:SI 1 "nonmemory_operand"  "cO,cI,  Cal, Cal")))]
  "TARGET_ARCOMPACT"
  "cmp%? %0,%B1%&"
  [(set_attr "type" "compare")
   (set_attr "iscompact" "true,false,true_limm,false")
   (set_attr "cond" "set")
   (set_attr "length" "*,4,*,8")])

(define_insn "*cmpsi_cc_zn_insn"
  [(set (reg:CC_ZN 61)
        (compare:CC_ZN (match_operand:SI 0 "register_operand"  "qRcq,c")
                       (const_int 0)))]
  "TARGET_ARCOMPACT"
  "tst%? %0,%0%&"
  [(set_attr "type" "compare,compare")
   (set_attr "iscompact" "true,false")
   (set_attr "cond" "set_zn")
   (set_attr "length" "*,4")])

; combiner pattern observed for unwind-dw2-fde.c:linear_search_fdes.
(define_insn "*btst"
  [(set (reg:CC_ZN CC_REG)
	(compare:CC_ZN
	  (zero_extract:SI (match_operand:SI 0 "register_operand" "Rcqq,c")
			   (const_int 1)
			   (match_operand:SI 1 "nonmemory_operand" "L,Lc"))
	  (const_int 0)))]
  ""
  "btst%? %0,%1"
  [(set_attr "iscompact" "true,false")
   (set_attr "cond" "set")
   (set_attr "type" "compare")
   (set_attr "length" "*,4")])

; combine suffers from 'simplifications' that replace a one-bit zero
; extract with a shift if it can prove that the upper bits are zero.
; arc_reorg sees the code after sched2, which can have caused our
; inputs to be clobbered even if they were not clobbered before.
; Therefore, add a third way to convert btst / b{eq,ne} to bbit{0,1}
; OTOH, this is somewhat marginal, and can leat to out-of-range
; bbit (i.e. bad scheduling) and missed conditional execution,
; so make this an option.
(define_peephole2
  [(set (reg:CC_ZN CC_REG)
	(compare:CC_ZN
	  (zero_extract:SI (match_operand:SI 0 "register_operand" "")
			   (const_int 1)
			   (match_operand:SI 1 "nonmemory_operand" ""))
	  (const_int 0)))
   (set (pc)
	(if_then_else (match_operator 3 "equality_comparison_operator"
				      [(reg:CC_ZN CC_REG) (const_int 0)])
		      (label_ref (match_operand 2 "" ""))
		      (pc)))]
  "TARGET_BBIT_PEEPHOLE && peep2_regno_dead_p (2, CC_REG)"
  [(parallel [(set (pc)
		   (if_then_else
		     (match_op_dup 3
		       [(zero_extract:SI (match_dup 0)
					 (const_int 1) (match_dup 1))
			(const_int 0)])
		     (label_ref (match_dup 2))
		     (pc)))
	      (clobber (reg:CC_ZN CC_REG))])])

(define_insn "*cmpsi_cc_z_insn"
  [(set (reg:CC_Z 61)
        (compare:CC_Z (match_operand:SI 0 "register_operand"  "qRcq,c")
                      (match_operand:SI 1 "p2_immediate_operand"  "O,n")))]
  "TARGET_ARCOMPACT"
  "@
	cmp%? %0,%1%&
	bxor.f 0,%0,%z1"
  [(set_attr "type" "compare,compare")
   (set_attr "iscompact" "true,false")
   (set_attr "cond" "set,set_zn")
   (set_attr "length" "*,4")])

(define_insn "*cmpsi_cc_c_insn"
  [(set (reg:CC_C 61)
        (compare:CC_C (match_operand:SI 0 "register_operand"  "Rcqq, c,Rcqq,  c")
                      (match_operand:SI 1 "nonmemory_operand" "cO,  cI, Cal,Cal")))]
  "TARGET_ARCOMPACT"
  "cmp%? %0,%S1%&"
  [(set_attr "type" "compare")
   (set_attr "iscompact" "true,false,true_limm,false")
   (set_attr "cond" "set")
   (set_attr "length" "*,4,*,8")])

;; Next come the scc insns.

(define_expand "seq"
  [(set (match_operand:SI 0 "dest_reg_operand" "=r") (match_dup 1))]
  ""
  "
{
  operands[1] = gen_compare_reg (EQ, SImode);
}")

(define_expand "sne"
  [(set (match_operand:SI 0 "dest_reg_operand" "=r") (match_dup 1))]
  ""
  "
{
  operands[1] = gen_compare_reg (NE, SImode);
}")

(define_expand "sgt"
  [(set (match_operand:SI 0 "dest_reg_operand" "=r") (match_dup 1))]
  ""
  "
{
  operands[1] = gen_compare_reg (GT, SImode);
}")

(define_expand "sle"
  [(set (match_operand:SI 0 "dest_reg_operand" "=r") (match_dup 1))]
  ""
  "
{
  operands[1] = gen_compare_reg (LE, SImode);
}")

(define_expand "sge"
  [(set (match_operand:SI 0 "dest_reg_operand" "=r") (match_dup 1))]
  ""
  "
{
  operands[1] = gen_compare_reg (GE, SImode);
}")

(define_expand "slt"
  [(set (match_operand:SI 0 "dest_reg_operand" "=r") (match_dup 1))]
  ""
  "
{
  operands[1] = gen_compare_reg (LT, SImode);
}")

(define_expand "sgtu"
  [(set (match_operand:SI 0 "dest_reg_operand" "=r") (match_dup 1))]
  ""
  "
{
  operands[1] = gen_compare_reg (GTU, SImode);
}")

(define_expand "sleu"
  [(set (match_operand:SI 0 "dest_reg_operand" "=r") (match_dup 1))]
  ""
  "
{
  operands[1] = gen_compare_reg (LEU, SImode);
}")

(define_expand "sgeu"
  [(set (match_operand:SI 0 "dest_reg_operand" "=r") (match_dup 1))]
  ""
  "
{
  operands[1] = gen_compare_reg (GEU, SImode);
}")

(define_expand "sltu"
  [(set (match_operand:SI 0 "dest_reg_operand" "=r") (match_dup 1))]
  ""
  "
{
  operands[1] = gen_compare_reg (LTU, SImode);
}")

(define_insn_and_split "*scc_insn"
  [(set (match_operand:SI 0 "dest_reg_operand" "=w")
	(match_operator:SI 1 "proper_comparison_operator" [(reg 61) (const_int 0)]))]
  ""
  "#"
  "reload_completed"
  [(set (match_dup 0) (const_int 1))
   (cond_exec
     (match_dup 1)
     (set (match_dup 0) (const_int 0)))]
{
  operands[1]
    = gen_rtx_fmt_ee (REVERSE_CONDITION (GET_CODE (operands[1]),
					 GET_MODE (XEXP (operands[1], 0))),
		      VOIDmode,
		      XEXP (operands[1], 0), XEXP (operands[1], 1));
}
  [(set_attr "type" "unary")])

;; ??? At least for ARC600, we should use sbc b,b,s12 if we want a value
;; that is one lower if the carry flag is set.

;; ??? Look up negscc insn.  See pa.md for example.
(define_insn "*neg_scc_insn"
  [(set (match_operand:SI 0 "dest_reg_operand" "=w")
	(neg:SI (match_operator:SI 1 "proper_comparison_operator"
		 [(reg 61) (const_int 0)])))]
  ""
  "mov %0,-1\;sub.%D1 %0,%0,%0"
  [(set_attr "type" "unary")
   (set_attr "length" "8")])

(define_insn "*not_scc_insn"
  [(set (match_operand:SI 0 "dest_reg_operand" "=w")
	(not:SI (match_operator:SI 1 "proper_comparison_operator"
		 [(reg 61) (const_int 0)])))]
  ""
  "mov %0,1\;sub.%d1 %0,%0,%0"
  [(set_attr "type" "unary")
   (set_attr "length" "8")])

; cond_exec patterns
(define_insn "*movsi_ne"
  [(cond_exec
     (ne (match_operand:CC_Z 2 "cc_use_register" "") (const_int 0))
     (set (match_operand:SI 0 "dest_reg_operand" "=Rcq#q,w,w")
	  (match_operand:SI 1 "nonmemory_operand" "%C_0,Lc,?Cal")))]
  ""
  "@
	sub%?.ne %0,%0,%0%&
	mov.ne %0,%1
	mov.ne %0,%S1"
  [(set_attr "type" "cmove,cmove,cmove")
   (set_attr "iscompact" "true,false,false")
   (set_attr "length" "*,4,8")])

(define_insn "*movsi_cond_exec"
  [(cond_exec
     (match_operator 3 "proper_comparison_operator"
       [(match_operand 2 "cc_register" "") (const_int 0)])
     (set (match_operand:SI 0 "dest_reg_operand" "=w,w")
	  (match_operand:SI 1 "nonmemory_operand" "%Lc,?Cal")))]
  ""
  "mov.%d3 %0,%S1"
  [(set_attr "type" "cmove")
   (set_attr "length" "4,8")])

(define_insn "*add_cond_exec"
  [(cond_exec
     (match_operator 4 "proper_comparison_operator"
       [(match_operand 3 "cc_register" "") (const_int 0)])
     (set (match_operand:SI 0 "dest_reg_operand" "=w,w")
	  (plus:SI (match_operand:SI 1 "register_operand" "%0,0")
		   (match_operand:SI 2 "nonmemory_operand" "cCca,?Cal"))))]
  ""
  "*return arc_output_addsi (operands, \".%d4\");"
  [(set_attr "cond" "use")
   (set_attr "type" "cmove")
   (set_attr "length" "4,8")])

; ??? and could use bclr,bmsk
; ??? or / xor could use bset / bxor
(define_insn "*commutative_cond_exec"
  [(cond_exec
     (match_operator 5 "proper_comparison_operator"
       [(match_operand 4 "cc_register" "") (const_int 0)])
     (set (match_operand:SI 0 "dest_reg_operand" "=w,w")
	  (match_operator:SI 3 "commutative_operator"
	    [(match_operand:SI 1 "register_operand" "%0,0")
	     (match_operand:SI 2 "nonmemory_operand" "cL,?Cal")])))]
  ""
  "%O3.%d5 %0,%1,%2"
  [(set_attr "cond" "use")
   (set_attr "type" "cmove")
   (set_attr "length" "4,8")])

(define_insn "*sub_cond_exec"
  [(cond_exec
     (match_operator 4 "proper_comparison_operator"
       [(match_operand 3 "cc_register" "") (const_int 0)])
     (set (match_operand:SI 0 "dest_reg_operand" "=w,w,w")
	  (minus:SI (match_operand:SI 1 "register_operand" "0,cL,Cal")
		    (match_operand:SI 2 "nonmemory_operand" "cL,0,0"))))]
  ""
  "@
	sub.%d4 %0,%1,%2
	rsub.%d4 %0,%2,%1
	rsub.%d4 %0,%2,%1"
  [(set_attr "cond" "use")
   (set_attr "type" "cmove")
   (set_attr "length" "4,4,8")])

(define_insn "*noncommutative_cond_exec"
  [(cond_exec
     (match_operator 5 "proper_comparison_operator"
       [(match_operand 4 "cc_register" "") (const_int 0)])
     (set (match_operand:SI 0 "dest_reg_operand" "=w,w")
	  (match_operator:SI 3 "noncommutative_operator"
	    [(match_operand:SI 1 "register_operand" "0,0")
	     (match_operand:SI 2 "nonmemory_operand" "cL,Cal")])))]
  ""
  "%O3.%d5 %0,%1,%2"
  [(set_attr "cond" "use")
   (set_attr "type" "cmove")
   (set_attr "length" "4,8")])

;; These control RTL generation for conditional jump insns

(define_expand "beq"
  [(set (pc)
	(if_then_else (match_dup 1)
		      (label_ref (match_operand 0 "" ""))
		      (pc)))]
  ""
  "
{
  operands[1] = gen_compare_reg (EQ, VOIDmode);
}")

(define_expand "bne"
  [(set (pc)
	(if_then_else (match_dup 1)
		      (label_ref (match_operand 0 "" ""))
		      (pc)))]
  ""
  "
{
  operands[1] = gen_compare_reg (NE, VOIDmode);
}")

(define_expand "bgt"
  [(set (pc)
	(if_then_else (match_dup 1)
		      (label_ref (match_operand 0 "" ""))
		      (pc)))]
  ""
  "
{
  operands[1] = gen_compare_reg (GT, VOIDmode);
}")

(define_expand "ble"
  [(set (pc)
	(if_then_else (match_dup 1)
		      (label_ref (match_operand 0 "" ""))
		      (pc)))]
  ""
  "
{
  operands[1] = gen_compare_reg (LE, VOIDmode);
}")

(define_expand "bge"
  [(set (pc)
	(if_then_else (match_dup 1)
		      (label_ref (match_operand 0 "" ""))
		      (pc)))]
  ""
  "
{
  operands[1] = gen_compare_reg (GE, VOIDmode);
}")

(define_expand "blt"
  [(set (pc)
	(if_then_else (match_dup 1)
		      (label_ref (match_operand 0 "" ""))
		      (pc)))]
  ""
  "
{
  operands[1] = gen_compare_reg (LT, VOIDmode);
}")

(define_expand "bgtu"
  [(set (pc)
	(if_then_else (match_dup 1)
		      (label_ref (match_operand 0 "" ""))
		      (pc)))]
  ""
  "
{
  operands[1] = gen_compare_reg (GTU, VOIDmode);
}")

(define_expand "bleu"
  [(set (pc)
	(if_then_else (match_dup 1)
		      (label_ref (match_operand 0 "" ""))
		      (pc)))]
  ""
  "
{
  operands[1] = gen_compare_reg (LEU, VOIDmode);
}")

(define_expand "bgeu"
  [(set (pc)
	(if_then_else (match_dup 1)
		      (label_ref (match_operand 0 "" ""))
		      (pc)))]
  ""
  "
{
  operands[1] = gen_compare_reg (GEU, VOIDmode);
}")

(define_expand "bltu"
  [(set (pc)
	(if_then_else (match_dup 1)
		      (label_ref (match_operand 0 "" ""))
		      (pc)))]
  ""
  "
{
  operands[1] = gen_compare_reg (LTU, VOIDmode);
}")

(define_expand "bunge"
  [(set (pc)
	(if_then_else (match_dup 1)
		      (label_ref (match_operand 0 "" ""))
		      (pc)))]
  ""
  "
{
  operands[1] = gen_compare_reg (UNGE, VOIDmode);
}")

(define_expand "bungt"
  [(set (pc)
	(if_then_else (match_dup 1)
		      (label_ref (match_operand 0 "" ""))
		      (pc)))]
  ""
  "
{
  operands[1] = gen_compare_reg (UNGT, VOIDmode);
}")

(define_expand "bunle"
  [(set (pc)
	(if_then_else (match_dup 1)
		      (label_ref (match_operand 0 "" ""))
		      (pc)))]
  ""
  "
{
  operands[1] = gen_compare_reg (UNLE, VOIDmode);
}")

(define_expand "bunlt"
  [(set (pc)
	(if_then_else (match_dup 1)
		      (label_ref (match_operand 0 "" ""))
		      (pc)))]
  ""
  "
{
  operands[1] = gen_compare_reg (UNLT, VOIDmode);
}")

(define_expand "buneq"
  [(set (pc)
	(if_then_else (match_dup 1)
		      (label_ref (match_operand 0 "" ""))
		      (pc)))]
  ""
  "
{
  operands[1] = gen_compare_reg (UNEQ, VOIDmode);
}")

(define_expand "bltgt"
  [(set (pc)
	(if_then_else (match_dup 1)
		      (label_ref (match_operand 0 "" ""))
		      (pc)))]
  ""
  "
{
  operands[1] = gen_compare_reg (LTGT, VOIDmode);
}")

(define_expand "bordered"
  [(set (pc)
	(if_then_else (match_dup 1)
		      (label_ref (match_operand 0 "" ""))
		      (pc)))]
  ""
  "
{
  operands[1] = gen_compare_reg (ORDERED, VOIDmode);
}")

(define_expand "bunordered"
  [(set (pc)
	(if_then_else (match_dup 1)
		      (label_ref (match_operand 0 "" ""))
		      (pc)))]
  ""
  "
{
  operands[1] = gen_compare_reg (UNORDERED, VOIDmode);
}")

;; Now match both normal and inverted jump.

;; TODO - supporting 16-bit conditional short branch insns if needed.

; (define_insn "*branch_insn_mixed"
;   [(set (pc)
; 	(if_then_else (match_operator 1 "comparison_operator"
; 				      [(reg 61) (const_int 0)])
; 		      (label_ref (match_operand 0 "" ""))
; 		      (pc)))]
;   "TARGET_MIXED_CODE"
;   "*
; {
;   if (arc_ccfsm_branch_deleted_p ())
;     {
;       arc_ccfsm_record_branch_deleted ();
;       return \"; branch deleted, next insns conditionalized\";
;     }
;   else
;     return \"b%d1_s %^%l0\";
; }"
;  [(set_attr "type" "branch")])

; When estimating sizes during arc_reorg, when optimizing for speed, there
; are three reasons why we need to consider branches to be length 6:
; - unnull-false delay slot insns are implemented using conditional execution,
;   thus preventing short insn formation where used.
; - for ARC600: unnul-true delay slot isnns are implemented where possile
;   using conditional execution, preventing short insn formation where used.
; - for ARC700: likely or somewhat likely taken branches are made long and
;   unaligned if possible to avoid branch penalty.
(define_insn "*branch_insn"
  [(set (pc)
	(if_then_else (match_operator 1 "proper_comparison_operator"
				      [(reg 61) (const_int 0)])
		      (label_ref (match_operand 0 "" ""))
		      (pc)))]
  ""
  "*
{
  if (arc_ccfsm_branch_deleted_p ())
    {
      arc_ccfsm_record_branch_deleted ();
      return \"; branch deleted, next insns conditionalized\";
    }
  else
    {
      arc_ccfsm_record_condition (operands[1], 0, insn, 0);
      if (get_attr_length (insn) == 2)
         return \"b%d1%? %^%l0%&\";
      else
         return \"b%d1%# %^%l0\";
    }
}"
  [(set_attr "type" "branch")
   (set
     (attr "lock_length")
     (cond [
       ; In arc_reorg we just guesstimate; might be more or less.
       (ne (symbol_ref "arc_branch_size_unknown_p ()") (const_int 0))
       (const_int 4)

       (eq_attr "delay_slot_filled" "yes")
       (const_int 4)

       (ne
	 (if_then_else
	   (match_operand 1 "equality_comparison_operator" "")
	   (ior (lt (minus (match_dup 0) (pc)) (const_int -512))
		(gt (minus (match_dup 0) (pc))
		    (minus (const_int 506)
			   (symbol_ref "get_attr_delay_slot_length (insn)"))))
	   (ior (lt (minus (match_dup 0) (pc)) (const_int -64))
		(gt (minus (match_dup 0) (pc))
		    (minus (const_int 58)
			   (symbol_ref "get_attr_delay_slot_length (insn)")))))
	 (const_int 0))
       (const_int 4)

       (eq_attr "verify_short" "yes")
       (const_int 2)]
      (const_int 4)))
   (set (attr "iscompact")
	(cond [(eq_attr "lock_length" "2") (const_string "true")]
	      (const_string "false")))])

(define_insn "*rev_branch_insn"
  [(set (pc)
	(if_then_else (match_operator 1 "proper_comparison_operator"
				      [(reg 61) (const_int 0)])
		      (pc)
		      (label_ref (match_operand 0 "" ""))))]
  "REVERSIBLE_CC_MODE (GET_MODE (XEXP (operands[1], 0)))"
  "*
{
  if (arc_ccfsm_branch_deleted_p ())
    {
      arc_ccfsm_record_branch_deleted ();
      return \"; branch deleted, next insns conditionalized\";
    }
  else
    {
      arc_ccfsm_record_condition (operands[1], 1, insn, 0);
      if (get_attr_length (insn) == 2)
	 return \"b%D1%? %^%l0\";
      else
	 return \"b%D1%# %^%l0\";
    }
}"
  [(set_attr "type" "branch")
   (set
     (attr "lock_length")
     (cond [
       ; In arc_reorg we just guesstimate; might be more or less.
       (ne (symbol_ref "arc_branch_size_unknown_p ()") (const_int 0))
       (const_int 4)

       (eq_attr "delay_slot_filled" "yes")
       (const_int 4)

       (ne
	 (if_then_else
	   (match_operand 1 "equality_comparison_operator" "")
	   (ior (lt (minus (match_dup 0) (pc)) (const_int -512))
		(gt (minus (match_dup 0) (pc))
		    (minus (const_int 506)
			   (symbol_ref "get_attr_delay_slot_length (insn)"))))
	   (ior (lt (minus (match_dup 0) (pc)) (const_int -64))
		(gt (minus (match_dup 0) (pc))
		    (minus (const_int 58)
			   (symbol_ref "get_attr_delay_slot_length (insn)")))))
	 (const_int 0))
       (const_int 4)

       (eq_attr "verify_short" "yes")
       (const_int 2)]
      (const_int 4)))
   (set (attr "iscompact")
	(cond [(eq_attr "lock_length" "2") (const_string "true")]
	      (const_string "false")))])

;; Unconditional and other jump instructions.

;; TODO - supporting 16-bit short branch insns if needed.
;(define_insn "*jump_mixed"
;  [(set (pc) (label_ref (match_operand 0 "" "")))]
;  "TARGET_MIXED_CODE"
;  "b_s %^%l0"
;  [(set_attr "type" "uncond_branch")])

(define_expand "jump"
  [(set (pc) (label_ref (match_operand 0 "" "")))]
  ""
  "")

(define_insn "jump_i"
  [(set (pc) (label_ref (match_operand 0 "" "")))]
  "!TARGET_LONG_CALLS_SET || !find_reg_note (insn, REG_CROSSING_JUMP, NULL_RTX)"
  "b%!%* %^%l0%&"
  [(set_attr "type" "uncond_branch")
   (set (attr "iscompact")
	(if_then_else (eq_attr "lock_length" "2")
		      (const_string "true") (const_string "false")))
   (set_attr "cond" "canuse")
   (set (attr "lock_length")
	(cond [
	  ; In arc_reorg we just guesstimate; might be more or less.
	  (ne (symbol_ref "arc_branch_size_unknown_p ()") (const_int 0))
	  (const_int 4)

	  (eq_attr "delay_slot_filled" "yes")
	  (const_int 4)

	  (ne (symbol_ref "find_reg_note (insn, REG_CROSSING_JUMP, NULL_RTX)")
	      (const_int 0))
	  (const_int 4)

	  (ior (lt (minus (match_dup 0) (pc)) (const_int -512))
	       (gt (minus (match_dup 0) (pc))
		   (minus (const_int 506)
			  (symbol_ref "get_attr_delay_slot_length (insn)"))))
	  (const_int 4)

	  (and (ne (symbol_ref "arc_ccfsm_advance_to (insn),
				arc_ccfsm_cond_exec_p ()")
		   (const_int 0))
	       (ior (lt (minus (match_dup 0) (pc)) (const_int -64))
		    (gt (minus (match_dup 0) (pc))
			(minus (const_int 58)
			       (symbol_ref
				"get_attr_delay_slot_length (insn)")))))
	  (const_int 4)

	  (eq_attr "verify_short" "yes")
	  (const_int 2)]

	 (const_int 4)))])

(define_insn "indirect_jump"
  [(set (pc) (match_operand:SI 0 "nonmemory_operand" "L,I,Cal,Rcqq,r"))]
  ""
  "j%!%* [%0]%&"
  [(set_attr "type" "jump")
   (set_attr "iscompact" "false,false,false,maybe,false")
   (set_attr "cond" "canuse,canuse_limm,canuse,canuse,canuse")])

;; (define_insn "indirect_jump"
;;   [(set (pc) (match_operand:SI 0 "register_operand" "r"))]
;;   ""
;;   "j%* [%0]"
;;   [(set_attr "type" "jump")])

;; Implement a switch statement.
; ??? the following comment shows ignorance of gcc internals
; - or possibly code that old that it predates the current facilities.
;; This wouldn't be necessary in the non-pic case if we could distinguish
;; label refs of the jump table from other label refs.  The problem is that
;; label refs are output as "%st(.LL42)" but we don't want the %st - we want
;; the real address since it's the address of the table.

(define_expand "casesi"
  [(set (match_dup 5)
	(minus:SI (match_operand:SI 0 "register_operand" "")
		  (match_operand:SI 1 "nonmemory_operand" "")))
   (set (reg:CC CC_REG)
	(compare:CC (match_dup 5)
		    (match_operand:SI 2 "nonmemory_operand" "")))
   (set (pc)
	(if_then_else (gtu (reg:CC 61)
			   (const_int 0))
		      (label_ref (match_operand 4 "" ""))
		      (pc)))
   (set (match_dup 6)
	(unspec:SI [(match_operand 3 "" "")
		    (match_dup 5) (match_dup 7)] UNSPEC_CASESI))
   (parallel [(set (pc) (match_dup 6)) (use (match_dup 7))])]
  ""
  "
{
  rtx x;

  operands[5] = gen_reg_rtx (SImode);
  operands[6] = gen_reg_rtx (SImode);
  operands[7] = operands[3];
  emit_insn (gen_subsi3 (operands[5], operands[0], operands[1]));
  emit_insn (gen_cmpsi_cc_insn_mixed (operands[5], operands[2]));
  x = gen_rtx_GTU (VOIDmode, gen_rtx_REG (CCmode, CC_REG), const0_rtx);
  x = gen_rtx_IF_THEN_ELSE (VOIDmode, x,
			    gen_rtx_LABEL_REF (VOIDmode, operands[4]), pc_rtx);
  emit_jump_insn (gen_rtx_SET (VOIDmode, pc_rtx, x));
  if (TARGET_COMPACT_CASESI)
    {
      emit_jump_insn (gen_casesi_compact_jump (operands[5], operands[7]));
    }
  else
    {
      operands[3] = gen_rtx_LABEL_REF (VOIDmode, operands[3]);
      if (flag_pic || !cse_not_expected)
	operands[3] = force_reg (Pmode, operands[3]);
      emit_insn (gen_casesi_load (operands[6],
				  operands[3], operands[5], operands[7]));
      if (CASE_VECTOR_PC_RELATIVE || flag_pic)
	emit_insn (gen_addsi3 (operands[6], operands[6], operands[3]));
      emit_jump_insn (gen_casesi_jump (operands[6], operands[7]));
    }
  DONE;
}")

(define_insn "casesi_load"
  [(set (match_operand:SI 0 "register_operand"             "=Rcq,r,r")
	(unspec:SI [(match_operand:SI 1 "nonmemory_operand" "Rcq,c,Cal")
		    (match_operand:SI 2 "register_operand"  "Rcq,c,c")
		    (label_ref (match_operand 3 "" ""))] UNSPEC_CASESI))]
  ""
  "*
{
  rtx diff_vec = PATTERN (next_real_insn (operands[3]));

  if (GET_CODE (diff_vec) != ADDR_DIFF_VEC)
    {
      gcc_assert (GET_CODE (diff_vec) == ADDR_VEC);
      gcc_assert (GET_MODE (diff_vec) == SImode);
      gcc_assert (!CASE_VECTOR_PC_RELATIVE && !flag_pic);
    }

  switch (GET_MODE (diff_vec))
    {
    case SImode:
      return \"ld.as %0,[%1,%2]%&\";
    case HImode:
      if (ADDR_DIFF_VEC_FLAGS (diff_vec).offset_unsigned)
	return \"ldw.as %0,[%1,%2]\";
      return \"ldw.x.as %0,[%1,%2]\";
    case QImode:
      if (ADDR_DIFF_VEC_FLAGS (diff_vec).offset_unsigned)
	return \"ldb%? %0,[%1,%2]%&\";
      return \"ldb.x %0,[%1,%2]\";
    default:
      gcc_unreachable ();
    }
}"
  [(set_attr "type" "load")
   (set_attr_alternative "iscompact"
     [(cond
	[(ne (symbol_ref "GET_MODE (PATTERN (next_real_insn (operands[3])))")
	     (symbol_ref "QImode"))
	 (const_string "false")
	 (eq (symbol_ref "ADDR_DIFF_VEC_FLAGS (PATTERN (next_real_insn (operands[3]))).offset_unsigned") (const_int 0))
	 (const_string "false")]
	(const_string "true"))
      (const_string "false")
      (const_string "false")])
   (set_attr_alternative "length"
     [(cond
	[(eq_attr "iscompact" "false")
	 (const_int 4)
	 (and (ne (symbol_ref "0") (const_int 0)) (eq (match_dup 0) (pc)))
	 (const_int 4)
	 (eq_attr "verify_short" "yes")
	 (const_int 2)]
	(const_int 4))
      (const_int 4)
      (const_int 8)])])

; Unlike the canonical tablejump, this pattern always uses a jump address,
; even for CASE_VECTOR_PC_RELATIVE.
(define_insn "casesi_jump"
  [(set (pc) (match_operand:SI 0 "register_operand" "Cal,Rcqq,c"))
   (use (label_ref (match_operand 1 "" "")))]
  ""
  "j%!%* [%0]%&"
  [(set_attr "type" "jump")
   (set_attr "iscompact" "false,maybe,false")
   (set_attr "cond" "canuse")])

(define_insn "casesi_compact_jump"
  [(set (pc)
	(unspec:SI [(match_operand:SI 0 "register_operand" "c,q")]
		   UNSPEC_CASESI))
   (use (label_ref (match_operand 1 "" "")))
   (clobber (match_scratch:SI 2 "=q,0"))]
  "TARGET_COMPACT_CASESI"
  "*
{
  rtx diff_vec = PATTERN (next_real_insn (operands[1]));
  int unalign = arc_get_unalign ();
  rtx xop[3];
  const char *s;

  xop[0] = operands[0];
  xop[2] = operands[2];
  gcc_assert (GET_CODE (diff_vec) == ADDR_DIFF_VEC);

  switch (GET_MODE (diff_vec))
    {
    case SImode:
      /* Max length can be 12 in this case, but this is OK because
	 2 of these are for alignment, and are anticipated in the length
	 of the ADDR_DIFF_VEC.  */
      if (unalign && !satisfies_constraint_Rcq (xop[0]))
	s = \"add2 %2,pcl,%0\n\tld_s%2,[%2,12]\";
      else if (unalign)
	s = \"add_s %2,%0,2\n\tld.as %2,[pcl,%2]\";
      else
	s = \"add %2,%0,2\n\tld.as %2,[pcl,%2]\";
      arc_clear_unalign ();
      break;
    case HImode:
      if (ADDR_DIFF_VEC_FLAGS (diff_vec).offset_unsigned)
	{
	  if (satisfies_constraint_Rcq (xop[0]))
	    {
	      s = \"add_s %2,%0,%1\n\tldw.as %2,[pcl,%2]\";
	      xop[1] = GEN_INT ((10 - unalign) / 2U);
	    }
	  else
	    {
	      s = \"add1 %2,pcl,%0\n\tldw_s %2,[%2,%1]\";
	      xop[1] = GEN_INT (10 + unalign);
	    }
	}
      else
	{
	  if (satisfies_constraint_Rcq (xop[0]))
	    {
	      s = \"add_s %2,%0,%1\n\tldw.x.as %2,[pcl,%2]\";
	      xop[1] = GEN_INT ((10 - unalign) / 2U);
	    }
	  else
	    {
	      s = \"add1 %2,pcl,%0\n\tldw_s.x %2,[%2,%1]\";
	      xop[1] = GEN_INT (10 + unalign);
	    }
	}
      arc_toggle_unalign ();
      break;
    case QImode:
      if (ADDR_DIFF_VEC_FLAGS (diff_vec).offset_unsigned)
	{
	  if (rtx_equal_p (xop[2], xop[0])
	      || find_reg_note (insn, REG_DEAD, xop[0]))
	    {
	      s = \"add_s %0,%0,pcl\n\tldb_s %2,[%0,%1]\";
	      xop[1] = GEN_INT (8 + unalign);
	    }
	  else
	    {
	      s = \"add %2,%0,pcl\n\tldb_s %2,[%2,%1]\";
	      xop[1] = GEN_INT (10 + unalign);
	      arc_toggle_unalign ();
	    }
	}
      else if (rtx_equal_p (xop[0], xop[2])
	       || find_reg_note (insn, REG_DEAD, xop[0]))
	{
	  s = \"add_s %0,%0,%1\n\tldb.x %2,[pcl,%0]\";
	  xop[1] = GEN_INT (10 - unalign);
	  arc_toggle_unalign ();
	}
      else
	{
	  /* ??? Length is 12.  */
	  s = \"add %2,%0,%1\n\tldb.x %2,[pcl,%2]\";
	  xop[1] = GEN_INT (8 + unalign);
	}
      break;
    default:
      gcc_unreachable ();
    }
  output_asm_insn (s, xop);
  return \"add_s %2,%2,pcl\n\tj_s%* [%2]\";
}"
  [(set_attr "length" "10")
   (set_attr "type" "jump")
   (set_attr "iscompact" "true")
   (set_attr "cond" "nocond")])

;; TODO: Splitting it up as separate patterns (when enabling this pattern) for
;;       TARGET_MIXED_CODE so that length can be set correctly.
(define_insn "tablejump"
  [(set (pc) (match_operand:SI 0 "address_operand" "p"))
   (use (label_ref (match_operand 1 "" "")))]
  "0 /* disabled -> using casesi now */"
  "j%* %a0"
  [(set_attr "type" "jump")])

(define_expand "call"
  ;; operands[1] is stack_size_rtx
  ;; operands[2] is next_arg_register
  [(parallel [(call (match_operand:SI 0 "call_operand" "")
		    (match_operand 1 "" ""))
	     (clobber (reg:SI 31))])]
  ""
  "{
    rtx callee;

    gcc_assert (MEM_P (operands[0]));
    callee  = XEXP (operands[0], 0);
    if (crtl->profile && arc_profile_call (callee))
      {
	emit_call_insn (gen_call_prof (gen_rtx_SYMBOL_REF (Pmode,
							   \"_mcount_call\"),
				       operands[1]));
	DONE;
      }
    /* This is to decide if we should generate indirect calls by loading the
       32 bit address of the callee into a register before performing the
       branch and link - this exposes cse opportunities.
       Also, in weird cases like compile/20010107-1.c, we may get a PLUS.  */
    if (GET_CODE (callee) != REG
	&& (GET_CODE (callee) == PLUS || arc_is_longcall_p (callee)))
      XEXP (operands[0], 0) = force_reg (Pmode, callee);
  }
")


(define_insn "*call_i"
  [(call (mem:SI (match_operand:SI 0 "call_address_operand" "Rcqq,c,Cbr,L,I,Cal"))
	 (match_operand 1 "" ""))
   (clobber (reg:SI 31))]
  ""
  "@
   jl%!%* [%0]%&
   jl%!%* [%0]
   bl%!%* %P0
   jl%!%* %S0
   jl%* %S0
   jl%! %S0"
  [(set_attr "type" "call,call,call,call,call,call_no_delay_slot")
   (set_attr "iscompact" "maybe,*,*,*,*,*")
   (set_attr_alternative "cond"
     [(const_string "canuse")
      (const_string "canuse")
      (cond [(eq (symbol_ref "TARGET_MEDIUM_CALLS") (const_int 0))
	     (const_string "canuse")
	     (eq_attr "delay_slot_filled" "yes")
	     (const_string "nocond")
	     (eq (symbol_ref "flag_pic") (const_int 0))
	     (const_string "canuse_limm")]
	    (const_string "nocond"))
      (const_string "canuse")
      (if_then_else (eq_attr "delay_slot_filled" "yes")
		    (const_string "nocond")
		    (const_string "canuse_limm"))
      (const_string "canuse")])
   (set_attr "length" "*,4,4,4,4,8")])

(define_insn "call_prof"
  [(call (mem:SI (match_operand:SI 0 "symbolic_operand" "Cbr,Cal"))
	 (match_operand 1 "" ""))
   (clobber (reg:SI 31))
   (use (reg:SI 8))
   (use (reg:SI 9))]
   ""
  "@
   bl%!%* %P0;2
   jl%! %^%S0"
  [(set_attr "type" "call,call_no_delay_slot")
   (set_attr "cond" "canuse,canuse")
   (set_attr "length" "4,8")])

(define_expand "call_value"
  ;; operand 2 is stack_size_rtx
  ;; operand 3 is next_arg_register
  [(parallel [(set (match_operand 0 "dest_reg_operand" "=r")
		   (call (match_operand:SI 1 "call_operand" "")
			 (match_operand 2 "" "")))
	     (clobber (reg:SI 31))])]
  ""
  "
  {
    rtx callee;

    gcc_assert (MEM_P (operands[1]));
    callee = XEXP (operands[1], 0);
    if (crtl->profile && arc_profile_call (callee))
      {
	emit_call_insn (gen_call_value_prof (operands[0],
					     gen_rtx_SYMBOL_REF (Pmode,
							    \"_mcount_call\"),
					     operands[2]));
	DONE;
      }
     /* See the comment in define_expand \"call\".  */
    if (GET_CODE (callee) != REG
	&& (GET_CODE (callee) == PLUS || arc_is_longcall_p (callee)))
      XEXP (operands[1], 0) = force_reg (Pmode, callee);
  }")


(define_insn "*call_value_i"
  [(set (match_operand 0 "dest_reg_operand"  "=Rcqq,w,  w,w,w,  w")
	(call (mem:SI (match_operand:SI 1
		       "call_address_operand" "Rcqq,c,Cbr,L,I,Cal"))
	      (match_operand 2 "" "")))
   (clobber (reg:SI 31))]
  ""
  "@
   jl%!%* [%1]%&
   jl%!%* [%1]
   bl%!%* %P1;1
   jl%!%* %S1
   jl%* %S1
   jl%! %S1"
  [(set_attr "type" "call,call,call,call,call,call_no_delay_slot")
   (set_attr "iscompact" "maybe,*,*,*,*,*")
   (set_attr_alternative "cond"
     [(const_string "canuse")
      (const_string "canuse")
      (cond [(eq (symbol_ref "TARGET_MEDIUM_CALLS") (const_int 0))
	     (const_string "canuse")
	     (eq_attr "delay_slot_filled" "yes")
	     (const_string "nocond")
	     (eq (symbol_ref "flag_pic") (const_int 0))
	     (const_string "canuse_limm")]
	    (const_string "nocond"))
      (const_string "canuse")
      (const_string "canuse_limm")
      (const_string "canuse")])
   (set_attr "length" "*,4,4,4,4,8")])


;; TODO - supporting 16-bit short "branch and link" insns if required.
;(define_insn "*call_value_via_label_mixed"
;  [(set (match_operand 0 "register_operand" "=r")
;	 (call (mem:SI (match_operand:SI 1 "call_address_operand" ""))
;	       (match_operand 2 "" "")))
;  (clobber (reg:SI 31))]
;  "TARGET_MIXED_CODE"
;  "bl_s %1"
;  [(set_attr "type" "call")])

(define_insn "call_value_prof"
  [(set (match_operand 0 "dest_reg_operand" "=r,r")
	(call (mem:SI (match_operand:SI 1 "symbolic_operand" "Cbr,Cal"))
	      (match_operand 2 "" "")))
   (clobber (reg:SI 31))
   (use (reg:SI 8))
   (use (reg:SI 9))]
   ""
  "@
   bl%!%* %P1;1
   jl%! %^%S1"
  [(set_attr "type" "call,call_no_delay_slot")
   (set_attr "cond" "canuse,canuse")
   (set_attr "length" "4,8")])

(define_insn "nop"
  [(const_int 0)]
  ""
  "nop%?"
  [(set_attr "type" "misc")
   (set_attr "iscompact" "true")
   (set_attr "cond" "canuse")
   (set_attr "length" "2")])

;; Special pattern to flush the icache.
;; ??? Not sure what to do here.  Some ARC's are known to support this.

(define_insn "flush_icache"
  [(unspec_volatile [(match_operand:SI 0 "memory_operand" "m")] 0)]
  ""
  "* return \"\";"
  [(set_attr "type" "misc")])

;; Split up troublesome insns for better scheduling.

;; Peepholes go at the end.
;;asl followed by add can be replaced by an add{1,2,3}
;; Three define_peepholes have been added for this optimization
;; ??? This used to target non-canonical rtl.  Now we use add_n, which
;; can be generated by combine.  Check if these peepholes still provide
;; any benefit.

;; -------------------------------------------------------------
;; Pattern 1 : r0 = r1 << {i}
;;             r3 = r4/INT + r0     ;;and commutative
;;                 ||
;;                 \/
;;             add{i} r3,r4/INT,r1
;; -------------------------------------------------------------

(define_peephole2
  [(set (match_operand:SI 0 "dest_reg_operand" "")
        (ashift:SI (match_operand:SI 1 "register_operand" "")
                   (match_operand:SI 2 "const_int_operand" "")))
  (set (match_operand:SI 3 "dest_reg_operand" "")
       (plus:SI (match_operand:SI 4 "nonmemory_operand" "")
		(match_operand:SI 5 "nonmemory_operand" "")))]
  "TARGET_ARCOMPACT
   && TARGET_DROSS
   && (INTVAL (operands[2]) == 1
       || INTVAL (operands[2]) == 2
       || INTVAL (operands[2]) == 3)
   && (true_regnum (operands[4]) == true_regnum (operands[0])
       || true_regnum (operands[5]) == true_regnum (operands[0]))
   && (peep2_reg_dead_p (2, operands[0]) || (true_regnum (operands[3]) == true_regnum (operands[0])))"
 ;; the preparation statements take care to put proper operand in operands[4]
 ;; operands[4] will always contain the correct operand. This is added to satisfy commutativity
  [(set (match_dup 3)
	(plus:SI (mult:SI (match_dup 1)
			  (match_dup 2))
		 (match_dup 4)))]
  "DROSS (\"addn peephole2\");
   if (true_regnum (operands[4]) == true_regnum (operands[0]))
      operands[4] = operands[5];
   operands[2] = GEN_INT (1 << INTVAL (operands[2]));"
)

;; triggered by -g -O2 -mARC600 -mmul64 -mnorm
;; libstdc++-v3/testsuite/23_containers/deque/check_construct_destroy.cc
(define_peephole
  [(set (match_operand:SI 0 "dest_reg_operand" "=w,w")
        (ashift:SI (match_operand:SI 1 "register_operand" "c,c")
                   (match_operand:SI 2 "const_int_operand" "n,n")))
  (set (match_operand:SI 3 "dest_reg_operand" "=w,w")
       (plus:SI (match_operand:SI 4 "nonmemory_operand" "ci,0")
		(match_operand:SI 5 "nonmemory_operand" "0,ci")))]
  "TARGET_ARCOMPACT
   && (INTVAL (operands[2]) == 1
       || INTVAL (operands[2]) == 2
       || INTVAL (operands[2]) == 3)
   && (true_regnum (operands[4]) == true_regnum (operands[0])
       || true_regnum (operands[5]) == true_regnum (operands[0]))
   && arc_dead_or_set_postreload_p (insn, operands[0])"
  "*{
    DROSS (\"addn_peephole\");
    /* This handles commutativity */
    if (which_alternative == 1)
      operands[4] = operands[5];
    switch (INTVAL (operands[2])) {
         case 1: return \"add1 %3,%S4,%1;;addi peephole - pattern 1\";
         case 2: return \"add2 %3,%S4,%1;;addi peephole - pattern 2\";
         case 3: return \"add3 %3,%S4,%1;;addi peephole - pattern 3\";
         default: gcc_unreachable ();
    }
  }"
  [(set_attr "length" "8")]
)

; ??? For ARC700, bbit peepholes should be replaced with a combiner pattern
; combining a CC_Z btst with a bne/beq, and then properly branch shortened.
; The combined pattern should be grokked by the ccfsm machinery.
; For ARC600, the peephole should be adjusted to use btst as input.
;
; bbit0, bbit1 peephole that incorporates bic
(define_peephole
  [(set (match_operand:SI 0 "dest_reg_operand" "=w")
	(and:SI (not:SI (match_operand:SI 1 "register_operand" "c"))
		(match_operand:SI 2 "immediate_operand" "Cal")))
   (set (reg:CC_ZN 61)
	(compare:CC_ZN (match_dup 0)
		       (match_operand:SI 3 "immediate_operand" "Cal")))
   (set (pc)
 	(if_then_else (match_operator 4 "proper_comparison_operator"
 				      [(reg 61) (const_int 0)])
 		      (label_ref (match_operand 5 "" ""))
 		      (pc)))
  ]
  "TARGET_ARCOMPACT
   && valid_bbit_pattern_p (operands,insn)
   && arc_dead_or_set_postreload_p (prev_nonnote_insn (insn), operands[0])
   && arc_dead_or_set_postreload_p (insn, XEXP (operands[4], 0))"
   "* return gen_bbit_bic_insns (operands);"
   [(set_attr "type" "branch")
    (set_attr "length" "4")]
)


; bbit0,bbit1 peephole optimization

(define_peephole
  [(set (match_operand:SI 0 "dest_reg_operand" "=w")
	(and:SI (match_operand:SI 1 "register_operand" "%c")
		(match_operand:SI 2 "immediate_operand" "Cal")))
   (set (reg:CC_ZN 61)
	(compare:CC_ZN (match_dup 0)
		       (match_operand:SI 3 "immediate_operand" "Cal")))
   (set (pc)
	(if_then_else (match_operator 4 "proper_comparison_operator"
			[(reg 61) (const_int 0)])
		      (label_ref (match_operand 5 "" ""))
		      (pc)))
 ]
  "TARGET_ARCOMPACT
   && valid_bbit_pattern_p (operands,insn)
   && arc_dead_or_set_postreload_p (prev_nonnote_insn(insn), operands [0])"
  "* return gen_bbit_insns(operands);"
  [ (set_attr "type" "branch")
    (set_attr "length" "4")])


;; bset peephole2 optimization
(define_peephole2
  [(set (match_operand:SI 0 "dest_reg_operand" "")
	(const_int 1))
  (set (match_operand:SI 1 "dest_reg_operand" "")
        (ashift:SI (match_dup 0)
                   (match_operand:SI 2 "register_operand" "")))
  (set (match_operand:SI 3 "dest_reg_operand" "")
       (ior:SI (match_operand:SI 4 "nonmemory_operand" "")
	       (match_operand:SI 5 "nonmemory_operand" "")))]
  "TARGET_ARCOMPACT
   && TARGET_DROSS
   && (peep2_reg_dead_p (2, operands[0]) || (true_regnum (operands[1]) == true_regnum (operands[0])))
   && (peep2_reg_dead_p (3, operands[1]) || (true_regnum (operands[3]) == true_regnum (operands[1])))
   && (true_regnum (operands[4]) == true_regnum (operands[1])
       || true_regnum (operands[5]) == true_regnum (operands[1]))"
  [(set (match_dup 3)
	(ior:SI (match_dup 4)
		(ashift:SI (const_int 1) (match_dup 2))))]  
  "DROSS (\"bset peephole2\");
   if (true_regnum (operands[4]) == true_regnum (operands[1]))
      operands[4] = operands[5];"
)


;; -------------------------------------------------------------
;; Pattern 1 : r0 = r1 << {i}
;;             r3 = r4 - r0 
;;                 ||
;;                 \/
;;             sub{i} r3,r4,r1
;; -------------------------------------------------------------

(define_peephole2
  [(set (match_operand:SI 0 "dest_reg_operand" "")
        (ashift:SI (match_operand:SI 1 "register_operand" "")
                   (match_operand:SI 2 "const_int_operand" "")))
   (set (match_operand:SI 3 "dest_reg_operand" "")
	(minus:SI (match_operand:SI 4 "nonmemory_operand" "")
		  (match_dup 0)))]
  "TARGET_ARCOMPACT
   && TARGET_DROSS
   && (INTVAL (operands[2]) == 1
       || INTVAL (operands[2]) == 2
       || INTVAL (operands[2]) == 3)
   && (peep2_reg_dead_p (2, operands[0]) || (true_regnum (operands[3]) == true_regnum (operands[0])))"
  [(set (match_dup 3)
	(minus:SI (match_dup 4)
		  (mult:SI (match_dup 1)
			   (match_dup 2))))]
  "DROSS (\"subn peephole2 1/2\");
   operands[2] = GEN_INT (1 << INTVAL (operands[2]));"
)

(define_peephole
  [(set (match_operand:SI 0 "dest_reg_operand" "=w")
        (ashift:SI (match_operand:SI 1 "register_operand" "c")
                   (match_operand:SI 2 "const_int_operand" "i")))
  (set (match_operand:SI 3 "dest_reg_operand" "=r")
       (minus:SI (match_operand:SI 4 "nonmemory_operand" "cCal")
		(match_dup 0)))]
  "TARGET_ARCOMPACT
   && TARGET_DROSS
   && (INTVAL (operands[2]) == 1
       || INTVAL (operands[2]) == 2
       || INTVAL (operands[2]) == 3)
   && arc_dead_or_set_postreload_p (insn, operands[0])"
  "*
   {
     DROSS (\"subn peephole 1/2\");
     switch (INTVAL (operands[2]))
       {
       case 1: return \";;sub1 peephole - pattern 1\;sub1%? %3,%S4,%1\";
       case 2: return \";;sub2 peephole - pattern 1\;sub2%? %3,%S4,%1\";
       case 3: return \";;sub3 peephole - pattern 1\;sub3%? %3,%S4,%1\";
       default: gcc_unreachable ();
       }
    }"
    [(set_attr "length" "4")]
)



;; -------------------------------------------------------------
;; Pattern 2 : r0 = r1 << {i}
;;             r3 = INT
;;             r5 = r3 - r0
;;                 ||
;;                 \/
;;             sub{i} r5,INT,r1
;; -------------------------------------------------------------

(define_peephole2
  [(set (match_operand:SI 0 "dest_reg_operand" "")
       (ashift:SI (match_operand:SI 1 "register_operand" "")
                  (match_operand:SI 2 "immediate_operand" "")))
   (set (match_operand:SI 3 "dest_reg_operand" "") 
	(match_operand:SI 4 "immediate_operand" ""))
   (set (match_operand:SI 5 "dest_reg_operand" "")
	(minus:SI (match_dup 3)
		  (match_dup 0)))]
  "TARGET_ARCOMPACT
   && TARGET_DROSS
   && GET_CODE (operands[2]) == CONST_INT 
   && (INTVAL (operands[2]) == 1
       || INTVAL (operands[2]) == 2
       || INTVAL (operands[2]) == 3)
   && (peep2_reg_dead_p (3, operands[0]) || (true_regnum (operands[5]) == true_regnum (operands[0])))
   && (peep2_reg_dead_p (3, operands[3]) || (true_regnum (operands[5]) == true_regnum (operands[3])))"
  [(set (match_dup 5)
	(minus:SI (match_dup 4)
		  (mult:SI (match_dup 1)
			   (match_dup 2))))]
  "DROSS (\"subn peephole2 2/2\");
   operands[2] = GEN_INT (1 << INTVAL (operands[2]));"
)

(define_peephole
  [(set (match_operand:SI 0 "dest_reg_operand" "=w")
       (ashift:SI (match_operand:SI 1 "register_operand" "c")
                  (match_operand:SI 2 "immediate_operand" "Cal")))
   (set (match_operand:SI 3 "dest_reg_operand" "=w") 
	(match_operand:SI 4 "immediate_operand" "Cal"))
   (set (match_operand:SI 5 "dest_reg_operand" "=w")
       (minus:SI (match_dup 3)
		(match_dup 0)
		))]
  "TARGET_ARCOMPACT
   && TARGET_DROSS
   && GET_CODE (operands[2]) == CONST_INT 
   && (INTVAL (operands[2]) == 1
       ||  INTVAL (operands[2]) == 2
       ||  INTVAL (operands[2]) == 3)
   && arc_dead_or_set_postreload_p (insn, operands[3])
   && arc_dead_or_set_postreload_p (insn, operands[0])"
  "*
   {
     DROSS (\"subn peephole 2/2\");
     switch (INTVAL (operands[2]))
       {
       case 1: return \";;sub1 peephole - pattern 2\;sub1%? %5,%S4,%1\";
       case 2: return \";;sub2 peephole - pattern 2\;sub2%? %5,%S4,%1\";
       case 3: return \";;sub3 peephole - pattern 2\;sub3%? %5,%S4,%1\";
       default: gcc_unreachable ();
       }
   }"
    [(set_attr "length" "4")]
)

;;bxor peephole2
(define_peephole2
  [(set (match_operand:SI 0 "dest_reg_operand" "")
	(const_int 1))
  (set (match_operand:SI 1 "dest_reg_operand" "")
        (ashift:SI (match_dup 0)
                   (match_operand:SI 2 "register_operand" "")))
  (set (match_operand:SI 3 "dest_reg_operand" "")
       (xor:SI (match_operand:SI 4 "nonmemory_operand" "")
	       (match_operand:SI 5 "nonmemory_operand" ""))) 
]
 "
  TARGET_ARCOMPACT
  && TARGET_DROSS
  && ( peep2_reg_dead_p (3, operands[1]) || ( true_regnum(operands[3])==true_regnum(operands[1]) ) )
  && ( peep2_reg_dead_p (2, operands[0]) || ( true_regnum(operands[1])==true_regnum(operands[0]) ) )
  && ( true_regnum(operands[4])==true_regnum(operands[1]) || true_regnum(operands[5])==true_regnum(operands[1]) )"

 ;; the preparation statements take care to put proper operand in operands[4]
 ;; operands[4] will always contain the correct operand
 [(set (match_dup 3)
	(xor:SI (match_dup 4)
		(ashift:SI (const_int 1)
			(match_dup 2) ) ) ) ]

  "DROSS (\"bxor peephole2\");
   if ( true_regnum(operands[4])==true_regnum(operands[1]) )
	operands[4] = operands[5];
  "
)

;; bclr peephole2 optimization 
(define_peephole2
  [(set (match_operand:SI 0 "dest_reg_operand" "")
	(const_int 1))
   (set (match_operand:SI 1 "dest_reg_operand" "")
       (ashift:SI (match_dup 0)
		  (match_operand:SI 2 "register_operand" "")))
   (set (match_operand:SI 3 "dest_reg_operand" "")
	(and:SI	(not:SI (match_dup 1))
		(match_operand:SI 4 "nonmemory_operand" "")))]
  "TARGET_ARCOMPACT 
   && TARGET_DROSS
   && ( peep2_reg_dead_p (2, operands[0]) || true_regnum(operands[1])==true_regnum(operands[0]) )
   && ( peep2_reg_dead_p (3, operands[1]) || true_regnum(operands[3])==true_regnum(operands[1]) )"
  [(set (match_dup 3)
	(and:SI (not:SI (ashift:SI (const_int 1)
				   (match_dup 2)))
		(match_dup 4)))]
  "DROSS (\"bclr peephole2\");"
)

;; bmsk peephole2 optimization 
(define_peephole2
  [(set (match_operand:SI 0 "dest_reg_operand" "")
       (plus:SI (match_operand:SI 1 "register_operand" "")
		(const_int 1)))
  (set (match_operand:SI 2 "dest_reg_operand" "")
	(const_int 1))
  (set (match_operand:SI 3 "dest_reg_operand" "")
       (ashift:SI (match_dup 2)
		  (match_dup 0)))
  (set (match_operand:SI 4 "dest_reg_operand" "")
       (plus:SI (match_dup 3) 
		 (const_int -1) ))
  (set (match_operand:SI 5 "dest_reg_operand" "")
       (and:SI (match_operand:SI 6 "nonmemory_operand" "")
	       (match_operand:SI 7 "nonmemory_operand" ""))) 
  ]
  "TARGET_ARCOMPACT
   && TARGET_DROSS
  && ( peep2_reg_dead_p (3, operands[0]) || true_regnum(operands[3])==true_regnum(operands[0]) )
  && ( peep2_reg_dead_p (3, operands[2]) || true_regnum(operands[3])==true_regnum(operands[2]) )
  && ( peep2_reg_dead_p (4, operands[3]) || true_regnum(operands[4])==true_regnum(operands[3]) )
  && ( peep2_reg_dead_p (5, operands[4]) || true_regnum(operands[5])==true_regnum(operands[4]) )
  && ( true_regnum(operands[6])==true_regnum(operands[4]) || true_regnum(operands[7])==true_regnum(operands[4]) )"
  [(set (match_dup 5)
	(and:SI (match_dup 6)
		(plus:SI (ashift:SI (const_int 1)
				    (plus:SI (match_dup 1)
					     (const_int 1)))
			 (const_int -1))))]
  "DROSS (\"bmsk peephole2\");
   if ( true_regnum(operands[6])==true_regnum(operands[4]) )
      operands[6]=operands[7];
  "
)

;; Instructions generated through builtins

(define_insn "norm"
  [(set (match_operand:SI  0 "dest_reg_operand" "=w,w")
	(unspec:SI [(match_operand:SI 1 "general_operand" "cL,Cal")]
			    UNSPEC_NORM))]
  "TARGET_NORM"
  "@
   norm \t%0, %1
   norm \t%0, %S1"
  [(set_attr "length" "4,8")
   (set_attr "type" "two_cycle_core,two_cycle_core")])

(define_insn "normw"
  [(set (match_operand:SI  0 "dest_reg_operand" "=w,w")
	(unspec:SI [(match_operand:HI 1 "general_operand" "cL,Cal")]
			    UNSPEC_NORMW))]
  "TARGET_NORM"
  "@
   normw \t%0, %1
   normw \t%0, %S1"
  [(set_attr "length" "4,8")
   (set_attr "type" "two_cycle_core,two_cycle_core")])


(define_insn "swap"
  [(set (match_operand:SI  0 "dest_reg_operand" "=w,w,w")
	(unspec:SI [(match_operand:SI 1 "general_operand" "L,Cal,c")]
			    UNSPEC_SWAP))]
  "TARGET_SWAP"
  "@
   swap \t%0, %1
   swap \t%0, %S1
   swap \t%0, %1"
  [(set_attr "length" "4,8,4")
   (set_attr "type" "two_cycle_core,two_cycle_core,two_cycle_core")])

;; FIXME: an intrinsic for multiply is daft.  Can we remove this?
(define_insn "mul64"
  [(unspec [(match_operand:SI 0 "general_operand" "q,r,r,%r")
		     (match_operand:SI 1 "general_operand" "q,rL,I,Cal")]
		   UNSPEC_MUL64)]
  "TARGET_MUL64_SET"
  "@
   mul64%? \t0, %0, %1%&
   mul64%? \t0, %0, %1
   mul64 \t0, %0, %1
   mul64%? \t0, %0, %S1"
  [(set_attr "length" "2,4,4,8")
  (set_attr "iscompact" "true,false,false,false")
  (set_attr "type" "binary,binary,binary,binary")
  (set_attr "cond" "canuse,canuse, nocond, canuse")])

(define_insn "mulu64"
  [(unspec [(match_operand:SI 0 "general_operand" "%r,r,r,r")
		     (match_operand:SI 1 "general_operand" "rL,I,r,Cal")]
		   UNSPEC_MULU64)]
  "TARGET_MUL64_SET"
  "@
   mulu64%? \t0, %0, %1
   mulu64 \t0, %0, %1
   mulu64 \t0, %0, %1
   mulu64%? \t0, %0, %S1"
  [(set_attr "length" "4,4,4,8")
   (set_attr "type" "binary,binary,binary,binary")
   (set_attr "cond" "canuse,nocond,nocond,canuse")])

(define_insn "divaw"
  [(set (match_operand:SI 0 "dest_reg_operand" "=&w,&w,&w")
			  (unspec:SI [(div:SI (match_operand:SI 1 "general_operand" "r,Cal,r")
					   (match_operand:SI 2 "general_operand" "r,r,Cal"))]
					   UNSPEC_DIVAW))]
  "TARGET_EA_SET && TARGET_ARCOMPACT"
  "@
   divaw \t%0, %1, %2
   divaw \t%0, %S1, %2
   divaw \t%0, %1, %S2"
  [(set_attr "length" "4,8,8")
   (set_attr "type" "divaw,divaw,divaw")])

; FIXME: The %? is of no use here, since cond is not canuse
(define_insn "flag"
  [(unspec_volatile [(match_operand:SI 0 "nonmemory_operand" "rL,I,Cal")]
		   VUNSPEC_FLAG)]
  ""
  "@
    flag%? %0
    flag %0
    flag%? %S0"
  [(set_attr "length" "4,4,8")
   (set_attr "type" "misc,misc,misc")
   (set_attr "cond" "clob,clob,clob")])

(define_insn "brk"
  [(unspec_volatile [(match_operand:SI 0 "immediate_operand" "N")]
		   VUNSPEC_BRK)]
  ""
  "brk"
  [(set_attr "length" "4")
  (set_attr "type" "misc")])

(define_insn "rtie"
  [(unspec_volatile [(match_operand:SI 0 "immediate_operand" "N")]
		   VUNSPEC_RTIE)]
  ""
  "rtie"
  [(set_attr "length" "4")
  (set_attr "type" "misc")
  (set_attr "cond" "clob")])

(define_insn "sync"
  [(unspec_volatile [(match_operand:SI 0 "immediate_operand" "N")]
		   VUNSPEC_SYNC)]
  ""
  "sync"
  [(set_attr "length" "4")
  (set_attr "type" "misc")])

(define_insn "swi"
  [(unspec_volatile [(match_operand:SI 0 "immediate_operand" "N")]
		   VUNSPEC_SWI)]
  ""
  "*
{
    if(TARGET_ARC700)
        return \"trap0\";
    else
        return \"swi\";
}"
  [(set_attr "length" "4")
  (set_attr "type" "misc")])


(define_insn "sleep"
  [(unspec_volatile [(match_operand:SI 0 "immediate_operand" "L")]
		   VUNSPEC_SLEEP)]
  "(TARGET_A4 || check_if_valid_sleep_operand(operands,0))"
  "*
   if (TARGET_A4)
      return \"sleep\";
   else
      return \"sleep %0\";
  "
  [(set_attr "length" "4")
  (set_attr "type" "misc")])

(define_insn "core_read"
  [(set (match_operand:SI  0 "dest_reg_operand" "=r,r")
	(unspec_volatile:SI [(match_operand:SI 1 "general_operand" "HJ,!r")]
			    VUNSPEC_CORE_READ))]
  ""
  "*
    if(check_if_valid_regno_const (operands,1))
       return \"mov \t%0, r%1\";
    return \"mov \t%0, r%1\";
  "
  [(set_attr "length" "4")
   (set_attr "type" "unary")])

(define_insn "core_write"
  [(unspec_volatile [(match_operand:SI 0 "general_operand" "r,r")
		     (match_operand:SI 1 "general_operand" "HJ,!r")]
		   VUNSPEC_CORE_WRITE)]
  ""
  "*
    if(check_if_valid_regno_const (operands,1))
       return \"mov \tr%1, %0\";
    return \"mov \tr%1, %0\";
  "
  [(set_attr "length" "4")
   (set_attr "type" "unary")])

(define_insn "lr"
  [(set (match_operand:SI  0 "dest_reg_operand" "=r,r,r,r")
	(unspec_volatile:SI [(match_operand:SI 1 "general_operand" "I,HJ,r,D")]
			    VUNSPEC_LR))]
  ""
  "lr\t%0, [%1]"
  [(set_attr "length" "4,8,4,8")
   (set_attr "type" "lr,lr,lr,lr")])

(define_insn "sr"
  [(unspec_volatile [(match_operand:SI 0 "general_operand" "Cal,r,r,r")
		     (match_operand:SI 1 "general_operand" "Ir,I,HJ,r")]
		   VUNSPEC_SR)]
  ""
  "sr\t%S0, [%1]"
  [(set_attr "length" "8,4,8,4")
   (set_attr "type" "sr,sr,sr,sr")])

(define_insn "trap_s"
  [(unspec_volatile [(match_operand:SI 0 "immediate_operand" "L,Cal")]
		   VUNSPEC_TRAP_S)]
  "TARGET_ARC700"
  "*
    if (which_alternative == 0)
       return \"trap_s %0\";

    fatal_error (\"Operand to trap_s should be an unsigned 6-bit value.\");
  "
  [(set_attr "length" "4")
  (set_attr "type" "misc")])

(define_insn "unimp_s"
  [(unspec_volatile [(match_operand:SI 0 "immediate_operand" "N")]
		   VUNSPEC_UNIMP_S)]
  "TARGET_ARC700"
  "unimp_s"
  [(set_attr "length" "4")
  (set_attr "type" "misc")])

;; End of instructions generated through builtins

; Since the demise of REG_N_SETS, it is no longer possible to find out
; in the prologue / epilogue expanders how many times blink is set.
; Using df_regs_ever_live_p to decide if blink needs saving means that
; any explicit use of blink will cause it to be saved; hence we cannot
; represent the blink use in return / sibcall instructions themselves, and
; instead have to show it in EPILOGUE_USES and must explicitly
; forbid instructions that change blink in the return / sibcall delay slot.
(define_expand "sibcall"
  [(parallel [(call (match_operand 0 "memory_operand" "")
		    (match_operand 1 "general_operand" ""))
	      (return)
	      (use (match_operand 2 "" ""))])]
  ""
  "
  {
    rtx callee = XEXP (operands[0], 0);

    if (operands[2] == NULL_RTX)
      operands[2] = const0_rtx;
    if (crtl->profile && arc_profile_call (callee))
      {
	emit_insn (gen_sibcall_prof
		    (gen_rtx_SYMBOL_REF (Pmode, \"_mcount_call\"),
		     operands[1], operands[2]));
	DONE;
      }
    if (GET_CODE (callee) != REG
	&& (GET_CODE (callee) == PLUS || arc_is_longcall_p (callee)))
      XEXP (operands[0], 0) = force_reg (Pmode, callee);
  }"
)

(define_expand "sibcall_value"
  [(parallel [(set (match_operand 0 "dest_reg_operand" "")
		   (call (match_operand 1 "memory_operand" "")
			 (match_operand 2 "general_operand" "")))
	      (return)
	      (use (match_operand 3 "" ""))])]
  ""
  "
  {
    rtx callee = XEXP (operands[1], 0);

    if (operands[3] == NULL_RTX)
      operands[3] = const0_rtx;
    if (crtl->profile && arc_profile_call (XEXP (operands[1], 0)))
      {
	emit_insn (gen_sibcall_value_prof
		    (operands[0], gen_rtx_SYMBOL_REF (Pmode, \"_mcount_call\"),
		     operands[2], operands[3]));
	DONE;
      }
    if (GET_CODE (callee) != REG && arc_is_longcall_p (callee))
      XEXP (operands[1], 0) = force_reg (Pmode, callee);
  }"
)

(define_insn "*sibcall_insn"
 [(call (mem:SI (match_operand:SI 0 "call_address_operand" "Cbr,Rs5,Rsc,Cal"))
	(match_operand 1 "" ""))
  (return)
  (use (match_operand 2 "" ""))]
  ""
  "@
   b%!%* %P0
   j%!%* [%0]%&
   j%!%* [%0]
   j%! %P0"
  [(set_attr "type" "call,call,call,call_no_delay_slot")
   (set_attr "iscompact" "false,maybe,false,false")
   (set_attr "is_SIBCALL" "yes")]
)

(define_insn "*sibcall_value_insn"
 [(set (match_operand 0 "dest_reg_operand" "")
       (call (mem:SI (match_operand:SI 1 "call_address_operand" "Cbr,Rs5,Rsc,Cal"))
	     (match_operand 2 "" "")))
  (return)
  (use (match_operand 3 "" ""))]
  ""
  "@
   b%!%* %P1
   j%!%* [%1]%&
   j%!%* [%1]
   j%! %P1"
  [(set_attr "type" "call,call,call,call_no_delay_slot")
   (set_attr "iscompact" "false,maybe,false,false")
   (set_attr "is_SIBCALL" "yes")]
)

(define_insn "sibcall_prof"
 [(call (mem:SI (match_operand:SI 0 "call_address_operand" "Cbr,Cal"))
	(match_operand 1 "" ""))
  (return)
  (use (match_operand 2 "" ""))
  (use (reg:SI 8))
  (use (reg:SI 9))]
  ""
  "@
   b%!%* %P0;2
   j%! %^%S0;2"
  [(set_attr "type" "call,call_no_delay_slot")
   (set_attr "is_SIBCALL" "yes")]
)

(define_insn "sibcall_value_prof"
 [(set (match_operand 0 "dest_reg_operand" "")
       (call (mem:SI (match_operand:SI 1 "call_address_operand" "Cbr,Cal"))
	     (match_operand 2 "" "")))
  (return)
  (use (match_operand 3 "" ""))
  (use (reg:SI 8))
  (use (reg:SI 9))]
  ""
  "@
   b%!%* %P1;1
   j%! %^%S1;1"
  [(set_attr "type" "call,call_no_delay_slot")
   (set_attr "is_SIBCALL" "yes")]
)

(define_expand "prologue"
  [(pc)]
  ""
{
  arc_expand_prologue ();
  DONE;
})

(define_expand "epilogue"
  [(pc)]
  ""
{
  arc_expand_epilogue (0);
  DONE;
})

(define_expand "sibcall_epilogue"
  [(pc)]
  "TARGET_ARCOMPACT"
{
  arc_expand_epilogue (1);
  DONE;
})

; Since the demise of REG_N_SETS, it is no longer possible to find out
; in the prologue / epilogue expanders how many times blink is set.
; Using df_regs_ever_live_p to decide if blink needs saving means that
; any explicit use of blink will cause it to be saved; hence we cannot
; represent the blink use in return / sibcall instructions themselves, and
; instead have to show it in EPILOGUE_USES and must explicitly
; forbid instructions that change blink in the return / sibcall delay slot.
(define_insn "return_i"
  [(return)]
  "reload_completed"
{
  rtx reg
    = gen_rtx_REG (Pmode,
		   arc_return_address_regs[arc_compute_function_type (cfun)]);

  if (TARGET_PAD_RETURN)
    arc_pad_return ();
  output_asm_insn (\"j%!%* [%0]%&\", &reg);
  return \"\";
}
  [(set_attr "type" "return")
   (set_attr "cond" "canuse")
   (set (attr "iscompact")
	(cond [(eq (symbol_ref "arc_compute_function_type (cfun)")
		   (symbol_ref "ARC_FUNCTION_NORMAL"))
	       (const_string "maybe")]
	      (const_string "false")))
   (set (attr "length")
	(cond [(eq (symbol_ref "arc_compute_function_type (cfun)")
		   (symbol_ref "ARC_FUNCTION_NORMAL"))
	       (const_int 4)
	       (and (ne (symbol_ref "0") (const_int 0))
		    (ne (minus (match_dup 0) (pc)) (const_int 0)))
	       (const_int 4)
	       (eq_attr "verify_short" "no")
	       (const_int 4)]
	      (const_int 2)))])

 ;; Comment in final.c (insn_current_reference_address) says
 ;; forward branch addresses are calculated from the next insn after branch
 ;; and for backward branches, it is calculated from the branch insn start.
 ;; The shortening logic here is tuned to accomodate this behaviour
;; ??? This should be grokked by the ccfsm machinery.
(define_insn "cbranchsi4_scratch"
  [(set (pc)
	(if_then_else (match_operator 0 "proper_comparison_operator"
			[(match_operand:SI 1 "register_operand" "c,c, c")
			 (match_operand:SI 2 "nonmemory_operand" "L,c,?Cal")])
		      (label_ref (match_operand 3 "" ""))
		      (pc)))
   (clobber (match_operand 4 "cc_register" ""))]
   "TARGET_ARCOMPACT
    && (reload_completed
	|| (TARGET_EARLY_CBRANCHSI
	    && brcc_nolimm_operator (operands[0], VOIDmode)))
    && !find_reg_note (insn, REG_CROSSING_JUMP, NULL_RTX)"
   "*
     switch (get_attr_length (insn))
     {
       case 2: return \"br%d0%? %1, %2, %^%l3%&\";
       case 4: return \"br%d0%* %1, %B2, %^%l3\";
       case 8: if (!brcc_nolimm_operator (operands[0], VOIDmode))
		 return \"br%d0%* %1, %B2, %^%l3\";
       case 6: case 10:
       case 12:return \"cmp%? %1, %B2\\n\\tb%d0%* %^%l3%&;br%d0 out of range\";
       default: fprintf (stderr, \"unexpected length %d\\n\", get_attr_length (insn)); fflush (stderr); gcc_unreachable ();
     }
   "
  [(set_attr "cond" "clob, clob, clob")
   (set (attr "iscompact")
	(cond [(eq_attr "lock_length" "2,6,10") (const_string "true")]
	      (const_string "false")))
   (set (attr "type")
	(if_then_else
	  (ne (symbol_ref "valid_brcc_with_delay_p (operands)") (const_int 0))
	  (const_string "brcc")
	  (const_string "brcc_no_delay_slot")))
   ; For forward branches, we need to account not only for the distance to
   ; the target, but also the difference between pcl and pc, the instruction
   ; length, and any delay insn, if present.
   (set
     (attr "lock_length")
     (cond ; the outer cond does a test independent of branch shortening.
       [(match_operand 0 "brcc_nolimm_operator" "")
	(cond
	  [(and (match_operand:CC_Z 4 "cc_register")
		(eq_attr "delay_slot_filled" "no")
		(ge (minus (match_dup 3) (pc)) (const_int -128))
		(le (minus (match_dup 3) (pc))
		    (minus (const_int 122)
			   (symbol_ref "get_attr_delay_slot_length (insn)"))))
	   (const_int 2)
	   (and (ge (minus (match_dup 3) (pc)) (const_int -256))
		(le (minus (match_dup 3) (pc))
		    (minus (const_int 248)
			   (symbol_ref "get_attr_delay_slot_length (insn)"))))
	   (const_int 4)
	   (match_operand:SI 1 "compact_register_operand" "")
	   (const_int 6)]
	  (const_int 8))]
	 (cond [(and (ge (minus (match_dup 3) (pc)) (const_int -256))
		     (le (minus (match_dup 3) (pc)) (const_int 244)))
		(const_int 8)
		(match_operand:SI 1 "compact_register_operand" "")
		(const_int 10)]
	       (const_int 12))))
   (set
     (attr "length")
     (cond ; the outer cond does a test independent of branch shortening.
       [(match_operand 0 "brcc_nolimm_operator" "")
	(cond
	  [(and (match_operand:CC_Z 4 "cc_register")
		(eq_attr "delay_slot_filled" "no")
		(ge (minus (match_dup 3) (pc)) (const_int -128))
		(le (minus (match_dup 3) (pc))
		    (minus (const_int 122)
			   (symbol_ref "get_attr_delay_slot_length (insn)")))
		(eq_attr "verify_short" "yes"))
	   (const_int 2)
	   (and (ge (minus (match_dup 3) (pc)) (const_int -256))
		(le (minus (match_dup 3) (pc))
		    (minus (const_int 248)
			   (symbol_ref "get_attr_delay_slot_length (insn)"))))
	   (const_int 4)
	   (and (match_operand:SI 1 "compact_register_operand" "")
	        (eq_attr "verify_short" "yes"))
	   (const_int 6)]
	  (const_int 8))]
	 (cond [(and (ge (minus (match_dup 3) (pc)) (const_int -256))
		     (le (minus (match_dup 3) (pc)) (const_int 244)))
		(const_int 8)
		(and (match_operand:SI 1 "compact_register_operand" "")
		     (eq_attr "verify_short" "yes"))
		(const_int 10)]
	       (const_int 12))))])

; combiner pattern observed for unwind-dw2-fde.c:linear_search_fdes.
(define_insn "*bbit"
  [(set (pc)
	(if_then_else
	  (match_operator 3 "equality_comparison_operator"
	    [(zero_extract:SI (match_operand:SI 1 "register_operand" "Rcqq,c")
			      (const_int 1)
			      (match_operand:SI 2 "nonmemory_operand" "L,Lc"))
	     (const_int 0)])
	     (label_ref (match_operand 0 "" ""))
	     (pc)))
   (clobber (reg:CC_ZN CC_REG))]
  "!find_reg_note (insn, REG_CROSSING_JUMP, NULL_RTX)"
{
  switch (get_attr_length (insn))
    {
      case 4: return (GET_CODE (operands[3]) == EQ
		      ? \"bbit0%* %1,%2,%0\" : \"bbit1%* %1,%2,%0\");
      case 6:
      case 8: return \"btst%? %1,%2\n\tb%d3%* %0; bbit out of range\";
      default: gcc_unreachable ();
    }
}
  [(set_attr "type" "brcc")
   (set_attr "cond" "clob")
   (set (attr "lock_length")
	(cond [(and (ge (minus (match_dup 0) (pc)) (const_int -254))
		    (le (minus (match_dup 0) (pc))
		    (minus (const_int 248)
			   (symbol_ref "get_attr_delay_slot_length (insn)"))))
	       (const_int 4)
	       (eq (symbol_ref "which_alternative") (const_int 0))
	       (const_int 6)]
	      (const_int 8)))
   (set (attr "length")
	(cond [(and (ge (minus (match_dup 0) (pc)) (const_int -254))
		    (le (minus (match_dup 0) (pc))
		    (minus (const_int 248)
			   (symbol_ref "get_attr_delay_slot_length (insn)"))))
	       (const_int 4)
	       (and (eq (symbol_ref "which_alternative") (const_int 0))
		    (eq_attr "verify_short" "yes"))
	       (const_int 6)]
	      (const_int 8)))
   (set (attr "iscompact")
	(if_then_else (eq_attr "lock_length" "6")
		      (const_string "true") (const_string "false")))])

; operand 0 is the loop count pseudo register
; operand 1 is the number of loop iterations or 0 if it is unknown
; operand 2 is the maximum number of loop iterations
; operand 3 is the number of levels of enclosed loops
; operand 4 is the loop end pattern
(define_expand "doloop_begin"
  [(use (match_operand 0 "register_operand" ""))
   (use (match_operand:QI 1 "const_int_operand" ""))
   (use (match_operand:QI 2 "const_int_operand" ""))
   (use (match_operand:QI 3 "const_int_operand" ""))
   (use (match_operand 4 "" ""))]
  ""
{
  if (INTVAL (operands[3]) > 1)
    FAIL;
  emit_insn (gen_doloop_begin_i (operands[0], const0_rtx,
				 GEN_INT (INSN_UID (operands[4])),
				 const0_rtx, const0_rtx));
  DONE;
})

; ??? can't describe the insn properly as then the optimizers try to
; hoist the SETs.
;(define_insn "doloop_begin_i"
;  [(set (reg:SI LP_START) (pc))
;   (set (reg:SI LP_END) (unspec:SI [(pc)] UNSPEC_LP))
;   (use (match_operand 0 "const_int_operand" "n"))]
;  ""
;  "lp .L__GCC__LP%0"
;)

; If operand1 is still zero after arc_reorg, this is an orphaned loop
; instruction that was not at the start of the loop.
; There is no point is reloading this insn - then lp_count would still not
; be available for the loop end.
(define_insn "doloop_begin_i"
  [(unspec:SI [(pc)] UNSPEC_LP)
   (clobber (reg:SI LP_START))
   (clobber (reg:SI LP_END))
   (use (match_operand:SI 0 "register_operand" "l,l,????*X"))
   (use (match_operand 1 "const_int_operand" "n,n,C_0"))
   (use (match_operand 2 "const_int_operand" "n,n,X"))
   (use (match_operand 3 "const_int_operand" "C_0,n,X"))
   (use (match_operand 4 "const_int_operand" "C_0,X,X"))]
  ""
{
  rtx scan;
  int len, size = 0;
  int n_insns = 0;
  rtx loop_start = operands[4];

  if (CONST_INT_P (loop_start))
    loop_start = NULL_RTX;
  /* Size implications of the alignment will be taken care off by the
     alignemnt inserted at the loop start.  */
  if (LOOP_ALIGN (0) && INTVAL (operands[1]))
    asm_fprintf (asm_out_file, "\t.p2align %d\\n", LOOP_ALIGN (0));
  if (!INTVAL (operands[1]))
    return "; LITTLE LOST LOOP";
  if (loop_start && flag_pic)
    /* ??? Can do better for when a scratch register
       is known.  But that would require extra testing.  */
    return ".p2align 2\;push_s r0\;add r0,pcl,%4-.+2\;sr r0,[2]; LP_START\;add r0,pcl,.L__GCC__LP%1-.+2\;sr r0,[3]; LP_END\;pop_s r0";
  /* Check if the loop end is in range to be set by the lp instruction.  */
  size = INTVAL (operands[3]) < 2 ? 0 : 2048;
  for (size = 0, scan = insn; scan && size < 2048; scan = NEXT_INSN (scan))
    {
      if (!INSN_P (scan))
	continue;
      if (recog_memoized (scan) == CODE_FOR_doloop_end_i
	  && (XEXP (XVECEXP (PATTERN (scan), 0, 4), 0)
	      == XEXP (XVECEXP (PATTERN (insn), 0, 4), 0)))
	break;
      len = get_attr_length (scan);
      size += len;
    }
  /* Try to verify that there are at least three instruction fetches
     between the loop setup and the first encounter of the loop end.  */
  for (scan = NEXT_INSN (insn); scan && n_insns < 3; scan = NEXT_INSN (scan))
    {
      if (!INSN_P (scan))
	continue;
      if (GET_CODE (PATTERN (scan)) == SEQUENCE)
	scan = XVECEXP (PATTERN (scan), 0, 0);
      if (JUMP_P (scan))
	{
	  if (recog_memoized (scan) != CODE_FOR_doloop_end_i)
	    {
	      n_insns += 2;
	      if (simplejump_p (scan))
		{
		  insn = XEXP (SET_SRC (PATTERN (scan)), 0);
		  continue;
		}
	      if (JUMP_LABEL (scan)
		  && (!next_active_insn (JUMP_LABEL (scan))
		      || (recog_memoized (next_active_insn (JUMP_LABEL (scan)))
			  != CODE_FOR_doloop_begin_i))
		  && (!next_active_insn (NEXT_INSN (PREV_INSN (scan)))
		      || (recog_memoized
			   (next_active_insn (NEXT_INSN (PREV_INSN (scan))))
			  != CODE_FOR_doloop_begin_i)))
		n_insns++;
	    }
	  break;
	}
      len = get_attr_length (scan);
      /* Size estimation of asms assumes that each line which is nonempty
	 codes an insn, and that each has a long immediate.  For minimum insn
	 count, assume merely that a nonempty asm has at least one insn.  */
      if (GET_CODE (PATTERN (scan)) == ASM_INPUT
	  || asm_noperands (PATTERN (scan)) >= 0)
	n_insns += (len != 0);
      else
	n_insns += (len > 4 ? 2 : (len ? 1 : 0));
    }
  if (LOOP_ALIGN (0))
    asm_fprintf (asm_out_file, "\t.p2align %d\\n", LOOP_ALIGN (0));
  gcc_assert (n_insns || GET_CODE (next_nonnote_insn (insn)) == CODE_LABEL);
  if (size >= 2048 || (TARGET_ARC600 && n_insns == 1) || loop_start)
    {
      if (flag_pic)
	/* ??? Can do better for when a scratch register
	   is known.  But that would require extra testing.  */
	return ".p2align 2\;push_s r0\;add r0,pcl,24\;sr r0,[2]; LP_START\;add r0,pcl,.L__GCC__LP%1-.+2\;sr r0,[3]; LP_END\;pop_s r0";
      output_asm_insn ((size < 2048
			? "lp .L__GCC__LP%1" : "sr .L__GCC__LP%1,[3]; LP_END"),
		       operands);
      output_asm_insn (loop_start
		       ? "sr %4,[2]; LP_START" : "sr 0f,[2]; LP_START",
		       operands);
      if (TARGET_ARC600 && n_insns < 1)
	output_asm_insn ("nop", operands);
      return (TARGET_ARC600 && n_insns < 3) ? "nop_s\;nop_s\;0:" : "0:";
    }
  else if (TARGET_ARC600 && n_insns < 3)
    {
      /* At least four instructions are needed between the setting of LP_COUNT
	 and the loop end - but the lp instruction qualifies as one.  */
      rtx prev = prev_nonnote_insn (insn);

      if (!INSN_P (prev) || dead_or_set_regno_p (prev, LP_COUNT))
	output_asm_insn ("nop", operands);
    }
  return "lp .L__GCC__LP%1";
}
  [(set_attr "type" "loop_setup")
   (set_attr_alternative "length"
     [(if_then_else (ne (symbol_ref "TARGET_ARC600") (const_int 0))
		    (const_int 16) (const_int 4))
      (if_then_else (ne (symbol_ref "flag_pic") (const_int 0))
		    (const_int 28) (const_int 16))
      (const_int 0)])]
  ;; ??? strictly speaking, we should branch shorten this insn, but then
  ;; we'd need a proper label first.  We could say it is always 24 bytes in
  ;; length, but that would be very pessimistic; also, when the loop insn
  ;; goes out of range, it is very likely that the same insns that have
  ;; done so will already have made all other small offset branches go out
  ;; of range, making the need for exact length information here mostly
  ;; academic.
)

; operand 0 is the loop count pseudo register
; operand 1 is the number of loop iterations or 0 if it is unknown
; operand 2 is the maximum number of loop iterations
; operand 3 is the number of levels of enclosed loops
; operand 4 is the label to jump to at the top of the loop
; operand 5 is nonzero if the loop is entered at its top.
; Use this for the ARC600 and ARC700.  For ARCtangent-A5, this is unsafe
; without further checking for nearby branches etc., and without proper
; annotation of shift patterns that clobber lp_count
; ??? ARC600 might want to check if the loop has few iteration and only a
; single insn - loop setup is expensive then.
(define_expand "doloop_end"
  [(use (match_operand 0 "register_operand" ""))
   (use (match_operand:QI 1 "const_int_operand" ""))
   (use (match_operand:QI 2 "const_int_operand" ""))
   (use (match_operand:QI 3 "const_int_operand" ""))
   (use (label_ref (match_operand 4 "" "")))
   (use (match_operand:QI 5 "const_int_operand" ""))]
  "(TARGET_ARC600 || TARGET_ARC700) && arc_experimental_mask & 1"
{
  if (INTVAL (operands[3]) > 1)
    FAIL;
  /* Setting up the loop with two sr isntructions costs 6 cycles.  */
  if (TARGET_ARC700 && !INTVAL (operands[5])
      && INTVAL (operands[1]) && INTVAL (operands[1]) <= (flag_pic ? 6 : 3))
    FAIL;
  /* We could do smaller bivs with biv widening, and wider bivs by having
     a high-word counter in an outer loop - but punt on this for now.  */
  if (GET_MODE (operands[0]) != SImode)
    FAIL;
  emit_jump_insn (gen_doloop_end_i (operands[0], operands[4], const0_rtx));
  DONE;
})

(define_insn_and_split "doloop_end_i"
  [(set (pc)
	(if_then_else (ne (match_operand:SI 0 "shouldbe_register_operand" "+l,*c,*m")
			   (const_int 1))
		      (label_ref (match_operand 1 "" ""))
		      (pc)))
   (set (match_dup 0) (plus:SI (match_dup 0) (const_int -1)))
   (use (reg:SI LP_START))
   (use (reg:SI LP_END))
   (use (match_operand 2 "const_int_operand" "n,???C_0,???X"))
   (clobber (match_scratch:SI 3 "=X,X,&????r"))]
  ""
  "*
{
  rtx prev = prev_nonnote_insn (insn);

  /* If there is an immediately preceding label, we must output a nop,
     lest a branch to that label will fall out of the loop.
     ??? We could try to avoid this by claiming to have a delay slot if there
     is a preceding label, and outputting the delay slot insn instead, if
     present.
     Or we could have some optimization that changes the source edge to update
     the loop count and jump to the loop start instead.  */
  /* For ARC600, we must also prevent jumps inside the loop and jumps where
     the loop counter value is live at the target from being directly at the
     loop end.  Being sure that the loop counter is dead at the target is
     too much hair - we can't rely on data flow information at this point -
     so insert a nop for all branches.
     The ARC600 also can't read the loop counter in the last insn of a loop.  */
  if (LABEL_P (prev))
    output_asm_insn (\"nop%?\", operands);
  return \"\\n.L__GCC__LP%2: ; loop end, start is %1\";
}"
  "&& memory_operand (operands[0], SImode)"
  [(pc)]
{
  emit_move_insn (operands[3], operands[0]);
  emit_jump_insn (gen_doloop_fallback_m (operands[3], operands[1], operands[0]));
  DONE;
}
  [(set_attr "type" "loop_end")
   (set (attr "length")
	(if_then_else (ne (symbol_ref "LABEL_P (prev_nonnote_insn (insn))")
			  (const_int 0))
		      (const_int 4) (const_int 0)))]
)

; This pattern is generated by arc_reorg when there is no recognizable
; loop start.
(define_insn "*doloop_fallback"
  [(set (pc) (if_then_else (ne (match_operand:SI 0 "register_operand" "+r,!w")
			        (const_int 1))
			   (label_ref (match_operand 1 "" ""))
			   (pc)))
   (set (match_dup 0) (plus:SI (match_dup 0) (const_int -1)))]
   ; avoid fooling the loop optimizer into assuming this is a special insn.
  "reload_completed"
  "*return get_attr_length (insn) == 8
   ? \"brne.d %0,1,%1\;sub %0,%0,1\"
   : \"breq %0,1,0f\;b.d %1\;sub %0,%0,1\\n0:\";"
  [(set (attr "length")
	(if_then_else (and (ge (minus (match_dup 1) (pc)) (const_int -256)) 
 			   (le (minus (match_dup 1) (pc)) (const_int 244)))
 		      (const_int 8) (const_int 12)))
   (set_attr "type" "brcc_no_delay_slot")
   (set_attr "cond" "nocond")]
)

; reload can't make output reloads for jump insns, so we have to do this by hand.
(define_insn "doloop_fallback_m"
  [(set (pc) (if_then_else (ne (match_operand:SI 0 "register_operand" "+&r")
			        (const_int 1))
			   (label_ref (match_operand 1 "" ""))
			   (pc)))
   (set (match_dup 0) (plus:SI (match_dup 0) (const_int -1)))
   (set (match_operand:SI 2 "memory_operand" "=m")
	(plus:SI (match_dup 0) (const_int -1)))]
   ; avoid fooling the loop optimizer into assuming this is a special insn.
  "reload_completed"
  "*return get_attr_length (insn) == 12
   ? \"sub %0,%0,1\;brne.d %0,0,%1\;st%U2%V2 %0,%2\"
   : \"sub %0,%0,1\;breq %0,0,0f\;b.d %1\\n0:\tst%U2%V2 %0,%2\";"
  [(set (attr "length")
	(if_then_else (and (ge (minus (match_dup 1) (pc)) (const_int -252)) 
 			   (le (minus (match_dup 1) (pc)) (const_int 244)))
 		      (const_int 12) (const_int 16)))
   (set_attr "type" "brcc_no_delay_slot")
   (set_attr "cond" "nocond")]
)

(define_expand "movmemsi"
  [(match_operand:BLK 0 "" "")
   (match_operand:BLK 1 "" "")
   (match_operand:SI 2 "nonmemory_operand" "")
   (match_operand 3 "immediate_operand" "")]
  ""
  "if (arc_expand_movmem (operands)) DONE; else FAIL;")

;; See http://gcc.gnu.org/bugzilla/show_bug.cgi?id=35803 why we can't
;; get rid of this bogosity.
(define_expand "cmpsf"
  [(set (reg:CC 61)
	(compare:CC (match_operand:SF 0 "general_operand" "")
		    (match_operand:SF 1 "general_operand" "")))]
  "TARGET_OPTFPE"
  "
{
  arc_compare_op0 = operands[0];
  arc_compare_op1 = operands[1];
  DONE;
}")

(define_expand "cmpdf"
  [(set (reg:CC 61)
	(compare:CC (match_operand:DF 0 "general_operand" "")
		    (match_operand:DF 1 "general_operand" "")))]
  "TARGET_OPTFPE && !TARGET_DPFP"
  "
{
  arc_compare_op0 = operands[0];
  arc_compare_op1 = operands[1];
  DONE;
}")

(define_expand "cmp_float"
  [(parallel [(set (match_operand 0 "") (match_operand 1 ""))
	      (clobber (reg:SI 31))
	      (clobber (reg:SI 12))])]
  ""
  "")

(define_insn "*cmpsf_eq"
  [(set (reg:CC_Z 61) (compare:CC_Z (reg:SF 0) (reg:SF 1)))
   (clobber (reg:SI 31))
   (clobber (reg:SI 12))]
  "TARGET_OPTFPE && !TARGET_SPFP"
  "*return arc_output_libcall (\"__eqsf2\");"
  [(set_attr "is_sfunc" "yes")]
)
	      
(define_insn "*cmpdf_eq"
  [(set (reg:CC_Z 61) (compare:CC_Z (reg:DF 0) (reg:DF 2)))
   (clobber (reg:SI 31))
   (clobber (reg:SI 12))]
  "TARGET_OPTFPE && !TARGET_DPFP"
  "*return arc_output_libcall (\"__eqdf2\");"
  [(set_attr "is_sfunc" "yes")]
)
	      
(define_insn "*cmpsf_gt"
  [(set (reg:CC_FP_GT 61) (compare:CC_FP_GT (reg:SF 0) (reg:SF 1)))
   (clobber (reg:SI 31))
   (clobber (reg:SI 12))]
  "TARGET_OPTFPE && !TARGET_SPFP"
  "*return arc_output_libcall (\"__gtsf2\");"
  [(set_attr "is_sfunc" "yes")]
)
	      
(define_insn "*cmpdf_gt"
  [(set (reg:CC_FP_GT 61) (compare:CC_FP_GT (reg:DF 0) (reg:DF 2)))
   (clobber (reg:SI 31))
   (clobber (reg:SI 12))]
  "TARGET_OPTFPE && !TARGET_DPFP"
  "*return arc_output_libcall (\"__gtdf2\");"
  [(set_attr "is_sfunc" "yes")]
)
	      
(define_insn "*cmpsf_ge"
  [(set (reg:CC_FP_GE 61) (compare:CC_FP_GE (reg:SF 0) (reg:SF 1)))
   (clobber (reg:SI 31))
   (clobber (reg:SI 12))]
  "TARGET_OPTFPE && !TARGET_SPFP"
  "*return arc_output_libcall (\"__gesf2\");"
  [(set_attr "is_sfunc" "yes")]
)
	      
(define_insn "*cmpdf_ge"
  [(set (reg:CC_FP_GE 61) (compare:CC_FP_GE (reg:DF 0) (reg:DF 2)))
   (clobber (reg:SI 31))
   (clobber (reg:SI 12))]
  "TARGET_OPTFPE && !TARGET_DPFP"
  "*return arc_output_libcall (\"__gedf2\");"
  [(set_attr "is_sfunc" "yes")]
)
	      
(define_insn "*cmpsf_uneq"
  [(set (reg:CC_FP_UNEQ 61) (compare:CC_FP_UNEQ (reg:SF 0) (reg:SF 1)))
   (clobber (reg:SI 31))
   (clobber (reg:SI 12))]
  "TARGET_OPTFPE && !TARGET_SPFP"
  "*return arc_output_libcall (\"__uneqsf2\");"
  [(set_attr "is_sfunc" "yes")]
)
	      
(define_insn "*cmpdf_uneq"
  [(set (reg:CC_FP_UNEQ 61) (compare:CC_FP_UNEQ (reg:DF 0) (reg:DF 2)))
   (clobber (reg:SI 31))
   (clobber (reg:SI 12))]
  "TARGET_OPTFPE && !TARGET_DPFP"
  "*return arc_output_libcall (\"__uneqdf2\");"
  [(set_attr "is_sfunc" "yes")]
)
	      
(define_insn "*cmpsf_ord"
  [(set (reg:CC_FP_ORD 61) (compare:CC_FP_ORD (reg:SF 0) (reg:SF 1)))
   (clobber (reg:SI 31))
   (clobber (reg:SI 12))]
  "TARGET_OPTFPE && !TARGET_SPFP"
  "*return arc_output_libcall (\"__ordsf2\");"
  [(set_attr "is_sfunc" "yes")]
)
	      
;; N.B. double precision fpx sets bit 31 for NaNs.  We need bit 51 set
;; for the floating point emulation to recognize the NaN.
(define_insn "*cmpdf_ord"
  [(set (reg:CC_FP_ORD 61) (compare:CC_FP_ORD (reg:DF 0) (reg:DF 2)))
   (clobber (reg:SI 31))
   (clobber (reg:SI 12))]
  "TARGET_OPTFPE && !TARGET_DPFP"
  "*return arc_output_libcall (\"__orddf2\");"
  [(set_attr "is_sfunc" "yes")]
)

(define_insn "abssf2"
  [(set (match_operand:SF 0 "dest_reg_operand"    "=Rcq#q,Rcw,w")
	(abs:SF (match_operand:SF 1 "register_operand" "0,  0,c")))]
  ""
  "bclr%? %0,%1,31%&"
  [(set_attr "type" "unary")
   (set_attr "iscompact" "maybe,false,false")
   (set_attr "length" "2,4,4")
   (set_attr "cond" "canuse,canuse,nocond")])
	      
(define_insn "negsf2"
  [(set (match_operand:SF 0 "dest_reg_operand" "=Rcw,w")
	(neg:SF (match_operand:SF 1 "register_operand" "0,c")))]
  ""
  "bxor%? %0,%1,31"
  [(set_attr "type" "unary")
   (set_attr "cond" "canuse,nocond")])

;; ??? Should this use arc_output_libcall and set is_sfunc?
(define_insn "*millicode_thunk_st"
  [(match_parallel 0 "millicode_store_operation"
     [(set (mem:SI (reg:SI SP_REG)) (reg:SI 13))])]
  ""
{
  output_asm_insn ("bl%* __st_r13_to_%0",
		   &SET_SRC (XVECEXP (operands[0], 0,
				      XVECLEN (operands[0], 0) - 2)));
  return "";
}
  [(set_attr "type" "call")])
	      
(define_insn "*millicode_thunk_ld"
  [(match_parallel 0 "millicode_load_clob_operation"
     [(set (reg:SI 13) (mem:SI (reg:SI SP_REG)))])]
  ""
{
  output_asm_insn ("bl%* __ld_r13_to_%0",
		   &SET_DEST (XVECEXP (operands[0], 0,
				       XVECLEN (operands[0], 0) - 2)));
  return "";
}
  [(set_attr "type" "call")])

(define_insn "*millicode_sibthunk_ld"
  [(match_parallel 0 "millicode_load_operation"
     [(return)
      (set (reg:SI SP_REG) (plus:SI (reg:SI SP_REG) (reg:SI 12)))
      (set (reg:SI 13) (mem:SI (reg:SI SP_REG)))])]
  ""
{
  output_asm_insn ("b%* __ld_r13_to_%0_ret",
		   &SET_DEST (XVECEXP (operands[0], 0,
				       XVECLEN (operands[0], 0) - 1)));
  return "";
}
  [(set_attr "type" "call")
   (set_attr "is_SIBCALL" "yes")])
	      
;; If hardware floating point is available, don't define a negdf pattern;
;; it would be something like:
;;(define_insn "negdf2"
;;  [(set (match_operand:DF 0 "register_operand" "=w,w,D,?r")
;;	(neg:DF (match_operand:DF 1 "register_operand" "0,c,D,D")))
;;   (clobber (match_scratch:DF 2 "=X,X,X,X,D1"))]
;;  ""
;;  "@
;;   bxor%? %H0,%H1,31
;;   bxor %H0,%H1,31 ` mov %L0,%L1
;;   drsubh%F0%F1 0,0,0
;;   drsubh%F2%F1 %H0,0,0 ` dexcl%F2 %L0,%H0,%L0"
;;  [(set_attr "type" "unary,unary,dpfp_addsub,dpfp_addsub")
;;   (set_attr "iscompact" "false,false,false,false")
;;   (set_attr "length" "4,4,8,12")
;;   (set_attr "cond" "canuse,nocond,nocond,nocond")])
;; and this suffers from always requiring a long immediate when using
;; the floating point hardware.
;; We then want the sub[sd]f patterns to be used, so that we can load the
;; constant zero efficiently into a register when we want to do the
;; computation using the floating point hardware.  There should be a special
;; subdf alternative that matches a zero operand 1, which then can allow
;; to use bxor to flip the high bit of an integer register.
;; ??? we actually can't use the floating point hardware for neg, because
;; this would not work right for -0.  OTOH optabs.c has already code
;; to synthesyze nagate by flipping the sign bit.

	      
;; include the arc-FPX instructions
(include "fpx.md")

(include "simdext.md")
