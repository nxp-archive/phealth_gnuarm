/* Prototypes for exported functions defined in arm.c and thumb.c
   Copyright (C) 1991, 93-98, 1999 Free Software Foundation, Inc.
   Contributed by Pieter `Tiggr' Schoenmakers (rcpieter@win.tue.nl)
   and Martin Simmons (@harleqn.co.uk).
   More major hacks by Richard Earnshaw (rearnsha@armltd.co.uk)
   Minor hacks by Nick Clifton (nickc@cygnus.com)

This file is part of GNU CC.

GNU CC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

GNU CC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU CC; see the file COPYING.  If not, write to
the Free Software Foundation, 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.  */

extern void   arm_override_options		PROTO ((void));
extern int    use_return_insn			PROTO ((int));
extern int    arm_regno_class 			PROTO ((int));
extern int    arm_process_pragma		PROTO ((int (*)(void), void (*) (int), char *));
extern void   arm_finalize_pic			PROTO ((void));
extern int    arm_volatile_func			PROTO ((void));
extern char * arm_output_epilogue		PROTO ((void));
/* Used in arm.md, but defined in output.c */
extern void   assemble_align			PROTO ((int)); 
extern void   output_func_epilogue		PROTO ((int));
extern void   arm_expand_prologue		PROTO ((void));

#ifdef TREE_CODE
extern int    arm_return_in_memory		PROTO ((tree));
extern int    arm_valid_machine_decl_attribute	PROTO ((tree, tree, tree));
extern int    arm_comp_type_attributes		PROTO ((tree, tree));
extern int    arm_valid_type_attribute_p	PROTO ((tree, tree, tree, tree));
#endif
#ifdef RTX_CODE
extern int    const_ok_for_arm			PROTO ((HOST_WIDE_INT));
extern int    arm_split_constant		PROTO ((enum rtx_code, enum machine_mode, HOST_WIDE_INT, rtx, rtx, int));
extern enum rtx_code arm_canonicalize_comparison PROTO((enum rtx_code, rtx *));
extern int    legitimate_pic_operand_p		PROTO ((rtx));
extern rtx    legitimize_pic_address		PROTO ((rtx, enum machine_mode, rtx));
extern int    is_pic				PROTO ((rtx));
extern int    arm_rtx_costs			PROTO ((rtx, enum rtx_code, enum rtx_code));
extern int    arm_adjust_cost			PROTO ((rtx, rtx, rtx, int));
extern int    const_double_rtx_ok_for_fpu	PROTO ((rtx));
extern int    neg_const_double_rtx_ok_for_fpu	PROTO ((rtx));
extern int    s_register_operand		PROTO ((rtx, enum machine_mode));
extern int    f_register_operand		PROTO ((rtx, enum machine_mode));
extern int    reg_or_int_operand		PROTO ((rtx, enum machine_mode));
extern int    arm_reload_memory_operand		PROTO ((rtx, enum machine_mode));
extern int    arm_rhs_operand			PROTO ((rtx, enum machine_mode));
extern int    arm_rhsm_operand			PROTO ((rtx, enum machine_mode));
extern int    arm_add_operand			PROTO ((rtx, enum machine_mode));
extern int    arm_not_operand			PROTO ((rtx, enum machine_mode));
extern int    offsettable_memory_operand	PROTO ((rtx, enum machine_mode));
extern int    alignable_memory_operand		PROTO ((rtx, enum machine_mode));
extern int    bad_signed_byte_operand		PROTO ((rtx, enum machine_mode));
extern int    fpu_rhs_operand			PROTO ((rtx, enum machine_mode));
extern int    fpu_add_operand			PROTO ((rtx, enum machine_mode));
extern int    power_of_two_operand		PROTO ((rtx, enum machine_mode));
extern int    nonimmediate_di_operand		PROTO ((rtx, enum machine_mode));
extern int    di_operand			PROTO ((rtx, enum machine_mode));
extern int    nonimmediate_soft_df_operand	PROTO ((rtx, enum machine_mode));
extern int    soft_df_operand			PROTO ((rtx, enum machine_mode));
extern int    index_operand			PROTO ((rtx, enum machine_mode));
extern int    const_shift_operand		PROTO ((rtx, enum machine_mode));
extern int    shiftable_operator		PROTO ((rtx, enum machine_mode));
extern int    shift_operator			PROTO ((rtx, enum machine_mode));
extern int    equality_operator			PROTO ((rtx, enum machine_mode));
extern int    minmax_operator			PROTO ((rtx, enum machine_mode));
extern int    cc_register			PROTO ((rtx, enum machine_mode));
extern int    dominant_cc_register		PROTO ((rtx, enum machine_mode));
extern int    logical_binary_operator		PROTO ((rtx, enum machine_mode));
extern int    multi_register_push		PROTO ((rtx, enum machine_mode));
extern int    load_multiple_operation		PROTO ((rtx, enum machine_mode));
extern int    store_multiple_operation		PROTO ((rtx, enum machine_mode));
extern int    symbol_mentioned_p		PROTO ((rtx));
extern int    label_mentioned_p			PROTO ((rtx));
extern enum rtx_code minmax_code		PROTO ((rtx));
extern int    adjacent_mem_locations		PROTO ((rtx, rtx));
extern int    load_multiple_sequence		PROTO ((rtx *, int, int *, int *, HOST_WIDE_INT *));
extern char * emit_ldm_seq			PROTO ((rtx *, int));
extern int    store_multiple_sequence		PROTO ((rtx *, int, int *, int *, HOST_WIDE_INT *));
extern char * emit_stm_seq			PROTO ((rtx *, int));
extern rtx    arm_gen_load_multiple		PROTO ((int, int, rtx, int, int, int, int, int));
extern rtx    arm_gen_store_multiple		PROTO ((int, int, rtx, int, int, int, int, int));
extern int    arm_gen_movstrqi			PROTO ((rtx *));
extern rtx    gen_rotated_half_load		PROTO ((rtx));
extern enum machine_mode arm_select_cc_mode	PROTO ((enum rtx_code, rtx, rtx));
extern rtx    gen_compare_reg			PROTO ((enum rtx_code, rtx, rtx));
extern void   arm_reload_in_hi			PROTO ((rtx *));
extern void   arm_reload_out_hi			PROTO ((rtx *));
extern void   arm_reorg				PROTO ((rtx));
extern char * fp_immediate_constant		PROTO ((rtx));
extern char * output_call			PROTO ((rtx *));
extern char * output_call_mem			PROTO ((rtx *));
extern char * output_mov_long_double_fpu_from_arm	PROTO ((rtx *));
extern char * output_mov_long_double_arm_from_fpu	PROTO ((rtx *));
extern char * output_mov_long_double_arm_from_arm	PROTO ((rtx *));
extern char * output_mov_double_fpu_from_arm	PROTO ((rtx *));
extern char * output_mov_double_arm_from_fpu	PROTO ((rtx *));
extern char * output_move_double		PROTO ((rtx *));
extern char * output_mov_immediate		PROTO ((rtx *));
extern char * output_add_immediate		PROTO ((rtx *));
extern char * arithmetic_instr			PROTO ((rtx, int));
extern void   output_ascii_pseudo_op		PROTO ((FILE *, unsigned char *, int));
extern char * output_return_instruction		PROTO ((rtx, int, int));
extern void   arm_poke_function_name		PROTO ((FILE *, char *));
extern void   output_arm_prologue		PROTO ((FILE *, int));
extern void   arm_print_operand			PROTO ((FILE *, rtx, int));
extern void   arm_final_prescan_insn		PROTO ((rtx));
extern int    arm_go_if_legitimate_address	PROTO ((enum machine_mode, rtx));
extern int    arm_debugger_arg_offset		PROTO ((int, rtx));

#if defined TREE_CODE
extern rtx    arm_function_arg			PROTO ((CUMULATIVE_ARGS *, enum machine_mode, tree, int));
extern void   arm_init_cumulative_args		PROTO ((CUMULATIVE_ARGS *, tree, rtx, int));
#endif

#if defined AOF_ASSEMBLER 
extern rtx    aof_pic_entry			PROTO ((rtx));
extern void   aof_dump_pic_table		PROTO ((FILE *));
extern char * aof_text_section			PROTO ((void));
extern char * aof_data_section			PROTO ((void));
extern void   aof_add_import			PROTO ((char *));
extern void   aof_delete_import			PROTO ((char *));
extern void   aof_dump_imports			PROTO ((FILE *));
#endif /* AOF_ASSEMBLER */

#endif /* RTX_CODE */

/* Defined in thumb.c */
extern void   arm_init_expanders		PROTO ((void));
extern int    thumb_far_jump_used_p		PROTO ((void));
extern char * thumb_unexpanded_epilogue		PROTO ((void));
extern void   thumb_expand_prologue		PROTO ((void));
extern void   thumb_expand_epilogue		PROTO ((void));
#ifdef TREE_CODE
extern int    is_called_in_ARM_mode		PROTO ((tree));
#endif
#ifdef RTX_CODE
extern int    thumb_shiftable_const		PROTO ((unsigned HOST_WIDE_INT));
extern void   thumb_final_prescan_insn		PROTO ((rtx));
extern char * thumb_load_double_from_address	PROTO ((rtx *));
extern void   output_thumb_prologue		PROTO ((FILE *));
extern char * thumb_output_move_mem_multiple	PROTO ((int, rtx *));
extern void   thumb_expand_movstrqi		PROTO ((rtx *));
extern int    thumb_cmp_operand			PROTO ((rtx, enum machine_mode));
extern char * thumb_condition_code		PROTO ((rtx, int));
extern rtx *  thumb_legitimize_pic_address	PROTO ((rtx, enum machine_mode, rtx));
extern int    thumb_go_if_legitimate_address	PROTO ((enum machine_mode, rtx));
extern rtx    arm_return_addr_rtx		PROTO ((int, rtx));
extern void   thumb_reload_out_hi		PROTO ((rtx *));
extern void   thumb_reload_in_hi		PROTO ((rtx *));
#endif

/* Defined in pe.c */
extern int  arm_dllexport_name_p 		PROTO ((char *));
extern int  arm_dllimport_name_p 		PROTO ((char *));

#ifdef TREE_CODE
extern int  arm_pe_valid_machine_decl_attribute	PROTO ((tree, tree, tree, tree));
extern tree arm_pe_merge_machine_decl_attributes PROTO ((tree, tree));
extern void arm_pe_unique_section 		PROTO ((tree, int));
extern void arm_pe_encode_section_info 		PROTO ((tree));
extern int  arm_dllexport_p 			PROTO ((tree));
extern int  arm_dllimport_p 			PROTO ((tree));
extern void arm_mark_dllexport 			PROTO ((tree));
extern void arm_mark_dllimport 			PROTO ((tree));
#endif
