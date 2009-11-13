2009-11-13  Richard Henderson  <rth@redhat.com>

	* gimple.def (GIMPLE_TRANSACTION): Rename from GIMPLE_TM_ATOMIC.
	* tree.def (TRANSACTION_EXPR): Rename from TM_ATOMIC.
	* c-typeck.c, cp/semantics.c, gimple-low.c, gimple-pretty-print.c,
	gimple.c, gimple.h, gimplify.c, gssstruct.def, omp-low.c, trans-mem.c,
	tree-cfg.c, tree-inline.c, tree-pretty-print.c: Update to match.
	Global replace tm_atomic with transaction.

2009-11-11  Aldy Hernandez  <aldyh@redhat.com>

	* trans-mem.c: Include params.h.  New global
	tm_log_must_generate_saves.
	(tm_log_entry): Add save_var entry.
	(tm_log_add): Initialize save_var.
	(tm_log_emit_stmt): New.
	(transaction_invariant_address_p): New.
	(tm_log_dump): Rename.  Handle transaction invariants.
	(tm_log_emit_saves): New.
	(tm_log_emit_restores): New.
	(tm_log_emit_save_or_restores): New.
	(execute_tm_mark): Call tm_log_emit.
	(expand_block_edges): Fix typo.
	(expand_tm_atomic): Emit save/restore sequences.
	(execute_tm_edges): Call htab_delete.
	* doc/invoke.texi: Document tm-max-aggregate-size.
	* builtin-types.def: Define BT_FN_VOID_VPTR_SIZE.
	* gtm-builtins.def: Define TM_LOG* builtins.
	* Makefile.in (trans-mem.o): Add PARAM_H dependency.
	* gimple.c (strip_invariant_refs): Unconstify.
	* gimple.h (strip_invariant_refs): Add prototype.
	* params.def: Define PARAM_TM_MAX_AGGREGATE_SIZE.

2009-10-28  Richard Henderson  <rth@redhat.com>

	* c-common.c (c_common_attribute_table): Add
	transaction_may_cancel_outer.
	(tm_attr_to_mask, tm_mask_to_attr): Remove TM_ATTR_UNKNOWN; add
	TM_ATTR_MAY_CANCEL_OUTER.
	* c-common.h (TM_ATTR_UNKNOWN): Remove.
	(TM_ATTR_MAY_CANCEL_OUTER): New.
	* c-parser.c (c_parser_transaction): Don't pass the outer
	TM_STMT_ATTR_OUTER attribute into c_finish_transaction.
	* cp/parser.c (cp_parser_transaction): Similarly.
	* gimple-pretty-print.c (dump_gimple_tm_atomic): Subtract
	bits from subcode as we print them; print remaining mask at the end.
	* gimple.c (walk_gimple_stmt): Separate out from OMP cases.
	* trans-mem.c (get_attrs_for): Extract the type from any expression;
	handle METHOD_TYPE properly.
	(is_tm_pure, is_tm_irrevocable): Tidy.
	(is_tm_safe): Include transaction_may_cancel_outer.
	(is_tm_callable): Likewise.
	(DIAG_TM_OUTER, DIAG_TM_SAFE, DIAG_TM_RELAXED): New.
	(struct diagnose_tm): New.
	(diagnose_tm_1): Rename from diagnose_tm_safe_1.  Handle
	transaction_may_cancel_outer, relaxed and outer transactions.
	(diagnose_tm_blocks): Walk all functions.  Set tm_may_enter_irr.
	(pass_diagnose_tm_blocks): Use TV_TRANS_MEM.
	(lower_tm_atomic): Preserve GTMA_DECLARATION_MASK.
	(tm_atomic_subcode_ior): Tidy.
	(expand_call_tm): Simplify call to is_tm_safe.
	(execute_tm_mark): Preserve GTMA_DECLARATION_MASK.
	(ipa_tm_scan_calls_tm_atomic): Don't set tm_may_enter_irr here.
	(ipa_tm_scan_calls_clone): Likewise.
	(ipa_tm_transform_clone): Likewise.
	(ipa_tm_mayenterirr_function): New.
	(ipa_tm_diagnose_tm_safe, ipa_tm_diagnose_tm_atomic): New.
	(ipa_tm_transform_calls): Mark tm_may_enter_irr here.
	(ipa_tm_execute): Queue even local tm_callable functions early.
	Use ipa_tm_mayenterirr_function and propagate tm_may_enter_irr
	around the cgraph.  Validate tm_safe and atomic regions.

	* testsuite/c-c++-common/tm/safe-1.c: Add transaction_may_cancel_outer
	tests; add indirect function call tests.
	* testsuite/gcc.dg/tm/memopt-3.c: Mark test transaction_safe.
	* testsuite/c-c++-common/tm/outer-1.c: New.
	* testsuite/c-c++-common/tm/safe-2.c: New.
	* testsuite/c-c++-common/tm/safe-3.c: New.
	* testsuite/gcc.dg/tm/props-1.c: Rename from props.c.

2009-10-27  Aldy Hernandez  <aldyh@redhat.com>

	* trans-mem.c (requires_barrier): Handle thread local memory.
	(lower_sequence_tm): Fix whitespace.
	(examine_assign_tm): Pass 2nd argument to requires_barrier.
	(expand_assign_tm): Same.
	(expand_call_tm): Same.
	(execute_tm_mark): Handle log.
	(tm_log_add): New.
	(tm_log_hash): New.
	(tm_log_eq): New.
	(tm_log_free): New.
	(tm_log_init): New.
	(tm_log_dump): New.

2009-10-22  Richard Henderson  <rth@redhat.com>

	* gimple-pretty-print.c (dump_gimple_tm_atomic_subcode): Merge into...
	(dump_gimple_tm_atomic): ... here.  Dump OUTER and RELAXED bits.
	* tree-pretty-print.c (dump_generic_node) [TM_ATOMIC]: Likewise.
	* gimple.h (GTMA_IS_OUTER, GTMA_IS_RELAXED): New.
	(GTMA_DECLARATION_MASK): New.
	(GTMA_HAVE_CALL_TM, GTMA_HAVE_UNCOMMITTED_THROW): Remove.
	* gimplify.c (gimplify_tm_atomic): Copy OUTER and RELAXED bits.

2009-10-22  Richard Henderson  <rth@redhat.com>

	* builtin-attrs.def (ATTR_TM_PURE): Use transaction_pure.
	* c-common.c (c_common_reswords): Rename __tm_abort and __tm_atomic
	to __transaction_cancel and __transaction respectively.
	(c_common_attribute_table): s/tm_/transaction_/g.  Remove tm_unkown.
	(parse_tm_stmt_attr): New.
	* c-common.h (RID_TRANSACTION, RID_TRANSACTION_CANCEL): Rename from
	RID_TM_ATOMIC, RID_TM_ABORT.
	(TM_STMT_ATTR_OUTER, TM_STMT_ATTR_ATOMIC, TM_STMT_ATTR_RELAXED): New.
	(parse_tm_stmt_attr): Declare.
	* c-parser.c (struct c_parser): Rename in_tm_atomic to in_transaction;
	widen to 4 bits.
	(c_parser_attribute_any_word): Split out from c_parser_attributes.
	(c_parser_statement_after_labels): Update for renames.
	(c_parser_transaction_attributes): New.
	(c_parser_transaction): Rename from c_parser_tm_atomic; parse the
	attributes in the new syntax.
	(c_parser_transaction_cancel): Similarly.
	* c-tree.h: Update decls.
	* c-typeck.c (c_finish_transaction): Rename from c_finish_tm_atomic;
	add flags parameter to fill in OUTER and RELAXED bits.
	* trans-mem.c (AR_OUTERABORT): New.
	(is_tm_pure): Update attribute names.
	(is_tm_irrevocable, is_tm_safe, is_tm_callable): Likewise.
	(is_tm_may_cancel_outer): New.
	(build_tm_abort_call): Add is_outer parameter.
	* tree.h: Update decls.
	(TM_ATOMIC_OUTER, TM_ATOMIC_RELAXED): New.

	* cp/class.c (set_one_vmethod_tm_attributes): Remove TM_ATTR_UNKNOWN
	tests.
	* cp/cp-tree.h: Update decls.
	* cp/except.c (do_get_exception_ptr): s/tm_/transaction_/.
	* cp/parser.c (struct cp_parser): Rename in_tm_atomic to
	in_transaction; widen from bool to uchar.
	(cp_parser_statement): Update for renames.
	(cp_parser_function_definition_after_declarator): Likewise.
	(cp_parser_token_starts_function_definition_p): Likewise.
	(cp_parser_txn_attribute_opt): New.
	(cp_parser_transaction): Rename from cp_parser_tm_atomic; parse
	the new attributes.
	(cp_parser_function_transaction): Similarly.
	(cp_parser_transaction_cancel): Similarly.
	* cp/semantics.c (begin_transaction_stmt): Rename from
	begin_tm_atomic_stmt.
	(finish_transaction_stmt): Rename from finish_tm_atomic_stmt;
	add flags parameter and fill in OUTER and RELAXED bits.

	* testsuite/g++.dg/tm/*: Adjust for new syntax.
	* testsuite/gcc.dg/tm/*: Likewise.

2009-10-20  Richard Henderson  <rth@redhat.com>

	* trans-mem.c (tm_memopt_init_sets): Use XOBNEW.
	(tm_memopt_free_sets): Move call to bitmap_obstack_release...
	(execute_tm_memopt): ... here.  Empty the hash table within
	the loop; free the hash table after the loop.

2009-10-20  Richard Henderson  <rth@redhat.com>

	* tree-inline.c (remap_gimple_stmt): Restore GIMPLE_TM_ATOMIC
	from before merge.

	* gimple-pretty-print.c (dump_gimple_eh_else): Tidy.
	* tree-cfg.c (verify_types_in_gimple_seq_2): Handle GIMPLE_EH_ELSE.
	* tree-eh.c: Restore EH_ELSE from before merge.

2009-10-20  Richard Henderson  <rth@redhat.com>

	* ipa-inline.c (analyze_function): Early return if no function body.
	* ipa-reference.c (analyze_function): Likewise.

2009-10-20  Richard Henderson  <rth@redhat.com>

	* trans-mem.c (gate_tm_init): Set tm-clone entry_block to the
	successor of ENTRY_BLOCK.
	(get_tm_region_blocks): Allow for a null exit_blocks bitmap.
	(tm_memopt_compute_available, tm_memopt_compute_antic): Likewise.

2009-10-13  Aldy Hernandez  <aldyh@redhat.com>

	* trans-mem.c (ipa_tm_insert_gettmclone_call): Cast return value
	from gettmclone.

2009-10-12  Richard Henderson  <rth@redhat.com>

	* calls.c (emit_call_1): Don't add REG_TM note here.
	* cfgbuild.c (make_edges): Add edges for REG_TM notes.
	* cfgexpand.c (expand_gimple_stmt): Add REG_TM notes.
	(gimple_expand_cfg): Free the tm_restart map.
	* cfgrtl.c (purge_dead_edges): Look for REG_TM notes.
	* combine.c (distribute_notes): Handle REG_TM notes.
	* emit-rtl.c (try_split): Likewise.
	* recog.c (peephole2_optimize): Likewise.
	* rtlanal.c (alloc_reg_note): Likewise.
	* function.h (struct tm_restart_node): New.
	(struct function): Add tm_restart member.
	* trans-mem.c (lower_tm_atomic): Use __builtin_eh_pointer; flatten
	GIMPLE_TM_ATOMIC immediately.
	(struct tm_region): Add inner and outer members; remove region_nr.
	(tm_region_init_0): New.
	(tm_region_init_1, tm_region_init): Rewrite for a direct scan.
	(tm_region_init_2): Remove.
	(gate_tm_init): Adjust for tm_region_init interface change.
	(make_tm_edge): Record edge in tm_restart map.
	(expand_block_edges): Tidy.
	(expand_tm_atomic): Don't call set_eh_region_tree_label.
	(ipa_tm_region_init): Adjust for tm_region_init changes.
	(add_stmt_to_tm_region): Remove.
	(ipa_tm_insert_irr_call): Don't call it.
	(ipa_tm_insert_gettmclone_call): Likewise.
	* tree-eh.c (struct_ptr_eq, struct_ptr_hash): Export.
	* tree.h (struct_ptr_eq, struct_ptr_hash): Declare.

2009-10-08  Aldy Hernandez  <aldyh@redhat.com>

	* trans-mem.c (tm_memopt_compute_avin): Do not special case entry
	block.  Do not seed with uninitialized blocks.
	(tm_memopt_compute_antin): Do not special case exit blocks.  Do
	not seed with uninitialized blocks.
	(execute_tm_memopt): Call tm_memopt_clear_visited.
	(tm_memopt_clear_visited): New.

2009-10-03  Aldy Hernandez  <aldyh@redhat.com>

	* tree.h (BUILTIN_TM_LOAD_STORE_P): New.
	(BUILTIN_TM_LOAD_P): New.
	(BUILTIN_TM_STORE_P): New.
	(CASE_BUILT_IN_TM_LOAD): New.
	(CASE_BUILT_IN_TM_STORE): New.
	* testsuite/gcc.dg/tm/memopt-2.c: Update for new dump code.
	* tree-ssa-alias.c (ref_maybe_used_by_call_p_1): Replace load
	cases by a call to BUILTIN_TM_LOAD_P.
	(call_may_clobber_ref_p_1): Replace store cases by a call to
	BUILTIN_TM_STORE_P.
	* trans-mem.c (is_tm_load): Replace load cases by a call to
	BUILTIN_TM_LOAD_P.
	(is_tm_store): Replace store cases by a call to
	BUILTIN_TM_STORE_P.
	(dump_tm_memopt_transform): Do not use prefix; just dump the new
	statement.
	(TRANSFORM_*): New macros.
	(tm_memopt_transform_stmt): Do the actual transformation.
	(tm_memopt_transform_blocks): Change calls to
	dump_tm_memopt_transform into calls to tm_memopt_transform_stmt.
	* calls.c (special_function_p): Change TM load/store cases into a
	call to BUILTIN_TM_LOAD_STORE_P.
	* gtm-builtins.def: New builtins for the following variants: WaR,
	WaW, RaR, RaW, RfW.
	* tree-ssa-structalias.c (find_func_aliases): Use BUILTIN_TM_*_P
	macros.

2009-09-29  Aldy Hernandez  <aldyh@redhat.com>

	* trans-mem.c (tm_memopt_compute_antic): New.
	(tm_memopt_compute_available): Merge seed and initialization
	loops.  Remove entry_block check.
	(tm_memopt_compute_antin): New.
	(execute_tm_memopt): Call tm_memopt_compute_antic.

2009-09-28  Aldy Hernandez  <aldyh@redhat.com>

	* trans-mem.c (tm_memopt_compute_available): Do not put entry
	block in the worklist.
	(tm_memopt_init_sets): Use obstack.
	(tm_memopt_free_sets): Adapt for obstack.
	(execute_tm_memopt): Same.

2009-09-25  Aldy Hernandez  <aldyh@redhat.com>

	* trans-mem.c: Include tree-dump.h
	(is_tm_load): New.
	(is_tm_store): New.
	(get_tm_region_blocks): New.
	(tm_memop_hash): New.
	(tm_memop_eq): New.
	(tm_memopt_value_number): New.
	(tm_memopt_accumulate_memops): New.
	(dump_tm_memopt_transform): New.
	(dump_tm_memopt_set): New.
	(dump_tm_memopt_sets): New.
	(tm_memopt_compute_avin): New.
	(tm_memopt_compute_available): New.
	(tm_memopt_transform_blocks): New.
	(tm_memopt_init_sets): New.
	(tm_memopt_free_sets): New.
	(execute_tm_memopt): New.
	(gate_tm_memopt): Activate.
	* Makefile.in (trans-mem.o): Depend on TREE_DUMP_H.

2009-09-10  Aldy Hernandez  <aldyh@redhat.com>

	* trans-mem.c (record_tm_replacement): Set DECL_UNINLINABLE.

2009-09-09  Aldy Hernandez  <aldyh@redhat.com>

	* trans-mem.c (find_tm_replacement_function): Check tm_wrap_map
	before checking for builtins.

2009-09-07  Aldy Hernandez  <aldyh@redhat.com>

	* builtin-attrs.def: (ATTR_TM_PURE): New.
	(ATTR_TM_TMPURE_NOTHROW_LIST): New.
	(ATTR_TM_PURE_TMPURE_NOTHROW_LIST): New.
	(ATTR_TM_PURE_NOTHROW_LIST): Remove.
	* gtm-builtins.def (BUILT_IN_TM_LOAD_*): Use
	ATTR_TM_PURE_TMPURE_NOTHROW_LIST instead.
	* tree-ssa-alias.c (ref_maybe_used_by_call_p_1): Handle TM builtins.
	(call_may_clobber_ref_p_1): Same.
	* tree-ssa-structalias.c (find_func_aliases): Same.
	Fix function comment to reflect reality.

2009-08-26  Aldy Hernandez  <aldyh@redhat.com>

	* tree.h, gimple-pretty-print.c, trans-mem.c, trans-mem.h,
	calls.c, c-common.c, c-common.h, gtm-builtins.def, gimple.h:
	Replace all instances of "irrevok*" with "irrevoc*".

2007-08-05  Richard Henderson  <rth@redhat.com>

	* except.c (duplicate_eh_regions_1): Zero tm_atomic_stmt.
	* tree-eh.c (stmt_can_throw_internal): Exclude __tm_atomic
	and the tm builtins.
	* tree-inline.c (remap_gimple_stmt): Update the tm_atomic_stmt
	of the ERT_TRANSACTION when copying __tm_atomic.
	(copy_bb): Tidy eh region updates.

2007-08-03  Richard Henderson  <rth@redhat.com>

	* varasm.c (record_tm_clone_pair): Allow entries to be
	optimized away by cgraph.

2007-08-03  Richard Henderson  <rth@redhat.com>

	* trans-mem.c (pass_diagnose_tm_blocks): Prefix pass name with *.
	(pass_tm_init): Likewise.

2009-07-24  Aldy Hernandez  <aldyh@redhat.com>

	* cgraph.h (cgraph_local_info): Add tm_may_enter_irr.
	* gimple-pretty-print.c (dump_gimple_tm_atomic_subcode): Rename
	GTMA_MUST_CALL_IRREVOKABLE to GTMA_DOES_GO_IRREVOKABLE.
	* trans-mem.c (expand_call_tm): Handle irrevocability.
	(execute_tm_mark): Set GTMA_DOES_GO_IRREVOKABLE and
	GTMA_MAY_ENTER_IRREVOKABLE.
	(expand_tm_atomic): Set PR_DOESGOIRREVOKABLE,
	PR_UNINSTRUMENTEDCODE, and PR_HASNOIRREVOKABLE appropriately.
	(ipa_tm_scan_calls_tm_atomic): Set tm_may_enter_irr for
	replacement functions.
	(ipa_tm_scan_calls_clone): Same.
	(ipa_tm_transform_tm_atomic): If the block goes irrevocable, don't
	transform anything.
	(ipa_tm_transform_clone): Set tm_may_enter_irr for non safe
	functions.
	(ipa_tm_execute): Set tm_may_enter_irr for non-local tm_callable
	functions. Swap loops calling ipa_tm_transform_clone and
	ipa_tm_transform_tm_atomic.
	* gimple.h: Rename GTMA_MUST_CALL_IRREVOKABLE into
	GTMA_DOES_GO_IRREVOKABLE.
					 
2009-07-23  Aldy Hernandez  <aldyh@redhat.com>

	* gimple-pretty-print.c (dump_gimple_tm_atomic_subcode): Add space
	after every GTMA_*.
	* trans-mem.c (is_tm_irrevokable): Return true for direct calls to
	the irrevocable builtin.
	(lower_sequence_tm): Rename GTMA_HAVE_CALL_IRREVOKABLE into
	GTMA_MAY_ENTER_IRREVOKABLE.
	(ipa_tm_insert_irr_call): Same.
	(expand_call_tm): Early return when is_tm_abort .
	Mark as GTMA_HAVE_STORE if the LHS of a call requires a barrier.
	(execute_tm_mark): Do not clear the GTMA_MAY_ENTER_IRREVOKABLE.
	(ipa_tm_insert_gettmclone_call): Set GTMA_MAY_ENTER_IRREVOKABLE
	unless safe.
	* gimple.h: Rename GTMA_HAVE_CALL_IRREVOKABLE into
	GTMA_MAY_ENTER_IRREVOKABLE.
	Add comments for GTMA_*IRREV*.
	* c-parser.c (c_parser_tm_abort): Complain if using tm_abort
	without TM enabled, and return a NOP_EXPR.

2009-07-22  Richard Henderson  <rth@redhat.com>

	* cgraphunit.c (verify_cgraph): Don't exempt clones from verification.
	* trans-mem.c (ipa_tm_execute): Call it.

	* cp/except.c (do_allocate_exception): Add tm_wrap declaration.
	(build_throw): Likewise.  Remove it from __cxa_rethrow.

2009-07-22  Aldy Hernandez  <aldyh@redhat.com>

	* trans-mem.c (ipa_tm_insert_irr_call): Make a new block for
	TM_IRREVOKABLE calls.
	(pass_ipa_tm): Enable dump.

2009-07-17  Aldy Hernandez  <aldyh@redhat.com>

	* gimple-pretty-print.c (dump_gimple_tm_atomic_subcode): New.
	(dump_gimple_tm_atomic): Dump subcodes.
	* trans-mem.c (is_tm_abort): New.
	(examine_call_tm): Use is_tm_abort.
	(expand_call_tm): Set GTMA_HAVE_ABORT.

2009-07-16  Richard Henderson  <rth@redhat.com>

	* except.c, except.h, tree-eh.c: Revert can_throw_internal_1
	change from 2009-07-10.

	* gimple.h (GF_CALL_NOINLINE): New.
	(gimple_call_noinline_p, gimple_call_set_noinline_p): New.
	* trans-mem.c (ipa_tm_insert_gettmclone_call): Set noinline on call.
	* tree-eh.c (inlinable_call_p): Test noinline on call.

2009-07-16  Aldy Hernandez  <aldyh@redhat.com>

	* gimple-pretty-print.c:
	Include trans-mem.h
	(dump_gimple_call): Dump arguments for calls to
	_ITM_beginTransaction.
	* trans-mem.c: Include trans-mem.h
	Move PR_* macros...
	* trans-mem.h: ...here.
	* Makefile.in (gimple-pretty-print.o): Depend on TRANS_MEM_H.
	(trans-mem.o): Same.
	* config/i386/i386.c (ix86_handle_cconv_attribute): Tag unused
	arguments as so.

2009-07-10  Richard Henderson  <rth@redhat.com>

	* gimple.def (GIMPLE_EH_ELSE): New.
	* gsstruct.def (GSS_EH_ELSE): New.
	* gimple-low.c (lower_stmt): Handle GIMPLE_EH_ELSE.
	(gimple_stmt_may_fallthru): Likewise.
	* gimple-pretty-print.c (dump_gimple_eh_else): New.
	(dump_gimple_stmt): Call it.
	* gimple.c (gss_for_code): Add GIMPLE_EH_ELSE.
	(gimple_size): Add GSS_EH_ELSE.
	(gimple_build_eh_else): New.
	(walk_gimple_stmt, gimple_copy): Handle GIMPLE_EH_ELSE.
	* gimple.h (struct gimple_statement_eh_else): New.
	(gimple_statement_d): Add it.
	(gimple_has_substatements): Add GIMPLE_EH_ELSE.
	(gimple_eh_else_n_body, gimple_eh_else_e_body): New.
	(gimple_eh_else_set_n_body, gimple_eh_else_set_e_body): New.
	* tree-eh.c (collect_finally_tree): Handle GIMPLE_EH_ELSE.
	(replace_goto_queue_1): Likewise.
	(get_eh_else): New.
	(honor_protect_cleanup_actions): Handle GIMPLE_EH_ELSE.
	(lower_try_finally_nofallthru): Likewise.
	(lower_try_finally_onedest): Likewise.
	(lower_try_finally_copy): Likewise.
	(lower_try_finally_switch): Likewise.
	(decide_copy_try_finally): Likewise.
	(lower_eh_constructs_2): Likewise.
	(refactor_eh_r): Likewise.

	* calls.c (special_function_p): Add BUILT_IN_TM_COMMIT_EH.
	* gimple.c (walk_gimple_seq): Honor removed_stmt.
	(gimple_call_flags): Handle GF_CALL_NOTHROW.
	* gimple.h (GF_CALL_NOTHROW): New.
	(gimple_call_set_nothrow_p): New.
	(struct walk_stmt_info): Use BOOL_BITFIELD; add removed_stmt.
	* gtm-builtins.def (BUILT_IN_TM_COMMIT_EH): New.
	* trans-mem.c (is_tm_ending_fndecl): Handle it.
	(tm_region_init_2): Likewise.
	(lower_tm_atomic, lower_sequence_tm, lower_sequence_no_tm): Rewrite
	in terms of walk_gimple_seq.
	(ipa_tm_insert_gettmclone_call): Save nothrow bit on call.
	* tree-eh.c (stmt_could_throw_p): Use gimple_call_nothrow_p.

	* cp/class.c (apply_tm_attr): Export.
	* cp/cp-tree.h: Declare it.
	* cp/except.c (do_get_exception_ptr): Apply tm_pure.
	(do_begin_catch): Record tm replacement function.
	(do_end_catch, build_throw): Likewise.

	* except.c (can_throw_internal_1): Remove inlinable_call parameter.
	(can_throw_internal): Don't pass it.
	* except.h (can_throw_internal_1): Update decl.
	* tree-eh.c (stmt_can_throw_internal): Don't pass inlinable_call.

2009-07-09  Richard Henderson  <rth@redhat.com>

	* c-common.c (handle_tm_wrap_attribute): New.
	(c_common_attribute_table): Add it.
	* trans-mem.c (tm_wrap_map, record_tm_replacement): New.
	(find_tm_replacement_function): Use tm_wrap_map.
	(diagnose_tm_safe_1): Use find_tm_replacement_function.
	* tree.h (record_tm_replacement): Declare.

2009-07-09  Richard Henderson  <rth@redhat.com>

	* c-decl.c (c_gimple_diagnostics_recursively): Split out
	non-recursive part to...
	* c-common.c (c_gimple_diagnostics): ... here.  Handle tm_safe.
	* c-common.h (c_gimple_diagnostics): Declare.
	* gimple.h (diagnose_tm_safe_errors): Declare.
	* trans-mem.c (diagnose_tm_safe_1, diagnose_tm_safe_errors): New.
	(ipa_tm_insert_gettmclone_call): Fix is_tm_safe call.

	* cp/decl.c (finish_function): Call c_gimple_diagnostics.

	* omp-low.c (WALK_SUBSTMTS): Add GIMPLE_TM_ATOMIC.

2009-07-09  Richard Henderson  <rth@redhat.com>

	* builtin-attrs.def (ATTR_TM_NOTHROW_NONNULL): Remove.
	* gtm-builtins.def (BUILT_IN_TM_MEMCPY, BUILT_IN_TM_MEMMOVE): Use
	ATTR_TM_NOTHROW_LIST.
	(BUILT_IN_TM_GETTMCLONE_IRR): Use ATTR_TM_CONST_NOTHROW_LIST.
	* trans-mem.c (ipa_tm_insert_gettmclone_call): Look through
	OBJ_TYPE_REF.

	* testsuite/g++.dg/tm/opt-1.C: New test.

2009-07-08  Richard Henderson  <rth@redhat.com>

	* c-common.c (handle_tm_attribute): Rename from
	handle_tm_fntype_attribute; handle class types.
	(tm_attr_to_mask): Rename from tm_attribute_mask; export.
	(tm_mask_to_attr, find_tm_attribute): New.
	* c-common.h: Add decls for new functions.
	(TM_ATTR_SAFE, TM_ATTR_CALLABLE, TM_ATTR_PURE,
	TM_ATTR_IRREVOKABLE, TM_ATTR_UNKNOWN): New.

	* cp/class.c (check_bases): Compute tm attributes for the class
	based on its base classes.
	(apply_tm_attr, look_for_tm_attr_overrides): New.
	(set_one_vmethod_tm_attributes, set_method_tm_attributes): New.
	(finish_struct_1): Call it.

	* testsuite/g++.dg/tm/attrib-[1234].C: New.
	* testsuite/gcc.dg/tm/attrib-1.c: Update warning text.

2009-07-06  Richard Henderson  <rth@redhat.com>

	* attribs.c (init_attributes): Allow '*' prefix for overrides.
	(register_attribute): Likewise.
	* builtin-attrs.def (ATTR_TM_REGPARM): New.
	(ATTR_TM_NOTHROW_LIST, ATTR_TM_NORETURN_NOTHROW_LIST,
	ATTR_TM_NOTHROW_NONNULL, ATTR_TM_CONST_NOTHROW_LIST,
	ATTR_TM_PURE_NOTHROW_LIST): New.
	* c-common.c (ignore_attribute): New.
	(c_common_attribute_table): Add "*tm regparm".

	* config/i386/i386.c (ix86_handle_tm_regparm_attribute): New.
	(ix86_attribute_table): Add "*tm regparm".

2009-07-02  Richard Henderson  <rth@redhat.com>

	* c-typeck.c (c_finish_tm_atomic): Use build_stmt.

	* cp/parser.c (struct cp_parser): Add in_tm_atomic.
	(cp_parser_tm_atomic, cp_parser_tm_abort): New.
	(cp_parser_statement): Use them.
	(cp_parser_function_tm_atomic): New.
	(cp_parser_function_definition_after_declarator): Use it.
	(cp_parser_token_starts_function_definition_p): Allow __tm_atomic.
	* cp/semantics.c (begin_tm_atomic_stmt): New.
	(finish_tm_atomic_stmt): New.
	* cp/cp-tree.h: Update decls.

	* gimple.c (walk_gimple_op): Handle GIMPLE_TM_ATOMIC.
	* gimple.h (gimple_has_substatements): Add GIMPLE_TM_ATOMIC.
	(gimple_tm_atomic_label_ptr): New.
	* tree-inline.c (remap_gimple_stmt): Handle GIMPLE_TM_ATOMIC.

	* testsuite/gcc.dg/tm/ipa-2.c: Add -asmname flag.
	* testsuite/g++.dg/dg.exp: Run tm tests.
	* testsuite/g++.dg/tm/: New subdirectory.

2009-07-01  Richard Henderson  <rth@redhat.com>

	* timevar.def (TV_TRANS_MEM): New.
	* trans-mem.c (pass_lower_tm, pass_tm_init, pass_tm_mark,
	pass_tm_edges, pass_tm_memopt, pass_ipa_tm): Use it.
	(mark_vops_in_stmt): Remove.
	(ipa_tm_execute): Don't call tree_versionable_function_p
	on unavailable functions.
	* tree-eh.c (struct leh_state): Fix eh_region_d tag use.
	* except.c: Re-merge some bits that accidentally got dropped.
	(remove_unreachable_regions): Don't remove transactions.
	* tree-cfg.c (verify_gimple_tm_atomic): New.
	(verify_types_in_gimple_stmt): Use it.
	* testsuite/gcc.dg/tm/ipa-1.c: Use -asmname dump flag.

2009-06-26  Richard Henderson  <rth@redhat.com>

	* trans-mem.c (ipa_tm_execute): Check tree_versionable_function_p.

2009-01-30  Richard Henderson  <rth@redhat.com>

	* trans-mem.c (gimplify_addr): Rename from gimplify_mem_ref_addr;
	handle any object not just TMR's.

	* trans-mem.c (ipa_tm_insert_gettmclone_call): Do
	add_referenced_var on the new temporary.

	* trans-mem.c (ipa_tm_execute): Fix test for when we can
	expect an external clone symbol.

	* c-common.c (c_common_attribute_table): Add tm_irrevokable.
	(tm_attribute_mask): Handle it.
	(handle_tm_fntype_attribute): Likewise.
	* calls.c (special_function_p): Add BUILT_IN_TM_GETTMCLONE_IRR.
	* gtm-builtins.def (BUILT_IN_TM_GETTMCLONE_IRR): New.
	(BUILT_IN_TM_GETTMCLONE_SAFE): New.
	* trans-mem.c (is_tm_irrevokable, is_tm_safe): New.
	(requires_barrier): Handle TARGET_MEM_REF.
	(gimplify_mem_ref_addr): New.
	(build_tm_load, build_tm_store): Use it.
	(expand_assign_tm): Early exit for no load or store.
	(expand_call_tm): Expect indirect calls.
	(expand_tm_atomic): Look for sharing loop back edges.
	(ipa_tm_scan_irr_block): Don't consider indirect calls irrevokable.
	(ipa_tm_insert_gettmclone_call): New.
	(ipa_tm_transform_calls): Use it.
	* tree.h (is_tm_safe, is_tm_irrevokable): Declare.

2009-01-28  Richard Henderson  <rth@redhat.com>

	* output.h (record_tm_clone_pair, finish_tm_clone_pairs): Declare.
	* toplev.c (compile_file): Call finish_tm_clone_pairs.
	* trans-mem.c (ipa_tm_create_version): Call record_tm_clone_pair.
	(ipa_tm_execute): Likewise.
	* varasm.c (tm_clone_pairs): New.
	(record_tm_clone_pair, finish_tm_clone_pairs): New.
	* crtstuff.c (__TMC_LIST__, __TMC_END__): New.
	(__do_global_dtors_aux): Deregister clone table.
	(frame_dummy): Register clone table.

	* cgraphunit.c (cgraph_function_versioning): Move lowered setting...
	(cgraph_copy_node_for_versioning): ... here.
	* trans-mem.c (ipa_tm_create_version): Clear DECL_RTL; mark the
	clone as needed, if externally visible.
	* testsuite/gcc.dg/tm/ipa-3.c: New.

2009-01-27  Richard Henderson  <rth@redhat.com>

	* c-common.h (RID_TM_RETRY): Remove.
	* c-common.c (c_common_resword): Remove RID_TM_RETRY.
	* c-parser.c (c_parser_tm_abort): Rename from c_parser_tm_abort_retry.
	(c_parser_statement_after_labels): Remove RID_TM_RETRY.
	* calls.c (special_function_p): Remove BUILT_IN_TM_RETRY.
	* gtm-builtins.def: Rename all functions to match the Intel ABI.
	* trans-mem.c (PR_*, A_*, AR_*, MODE_*): New define constants
	from the Intel ABI.
	(is_tm_ending_fndecl): Remove BUILT_IN_TM_RETRY.
	(build_tm_abort_call): New.
	(TM_START_RESTORE_LIVE_IN, TM_START_ABORT): Remove.
	(expand_tm_atomic): Update for library changes.
	* tree.h (build_tm_abort_call): Declare.

2008-11-13  Richard Henderson  <rth@redhat.com>

	* cgraphunit.c (cgraph_function_versioning): Copy assembler name
	from decl.
	* gimplify.c (create_tmp_var_name): Use clean_symbol_name.
	* tree-inline.c (initialize_cfun): Copy setjmp and alloca.

2008-11-11  Richard Henderson  <rth@redhat.com>

	* cgraph.h (struct cgraph_edge): Remove tm_atomic_call.
	(cgraph_copy_node_for_versioning): Declare.
	* cgraphunit.c (cgraph_copy_node_for_versioning): Export;
	copy analyzed from old version.
	(cgraph_function_versioning): Do DECL_NAME et al setup here ...
	* tree-inline.c (tree_function_versioning): ... not here.
	* ipa-inline.c (cgraph_gate_early_inlining): Don't check TM_CLONE.
	* passes.c (init_optimization_passes): Do ipa_tm before ipa_cp.
	* trans-mem.c (is_tm_pure_call): New.
	(is_tm_ending_fndecl, find_tm_replacement_function): New.
	(examine_call_tm): Don't look for irrevokable calls here.
	(execute_lower_tm): Assert we don't see TM_CLONEs.
	(tm_region_init_2, tm_region_init): Break out from ...
	(gate_tm_init): ... here.
	(expand_irrevokable): Remove.
	(expand_call_tm): Don't call it.
	(expand_block_tm): Return true if done expanding.
	(execute_tm_mark): Notice that.
	(ipa_tm_analyze_function, ipa_tm_add_new_function,
	ipa_tm_generate_summary, ipa_tm_decide_version): Remove.
	(struct tm_ipa_cg_data, cgraph_node_p, cgraph_node_queue,
	get_cg_data, maybe_push_queue, ipa_tm_scan_calls_tm_atomic,
	ipa_tm_scan_calls_clone, ipa_tm_note_irrevokable,
	ipa_tm_scan_irr_block, ipa_tm_scan_irr_blocks, ipa_tm_propagate_irr,
	ipa_tm_decrement_clone_counts, ipa_tm_scan_irr_function,
	ipa_tm_region_init, ipa_tm_mark_for_rename, ipa_tm_insert_irr_call,
	ipa_tm_transform_calls, ipa_tm_transform_tm_atomic, 
	ipa_tm_transform_clone): New.
	(ipa_tm_create_version): Don't use cgraph_function_versioning.
	(ipa_tm_execute): Rewrite.
	(pass_ipa_tm): Use simple_ipa_opt_pass.
	* tree-pretty-print.c (PRINT_FUNCTION_NAME): Merge into ...
	(dump_function_name): ... here.
	(maybe_dump_asm_name): New.

	* testsuite/gcc.dg/tm/ipa-1.c, testsuite/gcc.dg/tm/ipa-2.c: New.

2008-11-05  Richard Henderson  <rth@redhat.com>

	* trans-mem.c (ipa_tm_create_version): Use C++ style mangling.
	* Makefile.in (trans-mem.o): Update dependencies.

2008-11-05  Richard Henderson  <rth@redhat.com>

	* trans-mem.c (expand_irrevokable): Mark all call-clobbered tags
	for renaming.
	(execute_tm_edges): Clear all_tm_regions at the end.
	(gate_tm_memopt): Disable.
	* tree-ssa-operands.c (add_all_call_clobber_ops): Handle .GLOBAL_VAR.
	Use add_virtual_operand directly and pretend to be a call site.
	(add_call_clobber_ops): Streamline .GLOBAL_VAR handling.
	(add_call_read_ops): Likewise.
	* testsuite/gcc.dg/tm/opt-1.c: New.

2008-11-04  Richard Henderson  <rth@redhat.com>

	* c-common.c (handle_tm_callable_attribute,
	handle_tm_pure_attribute, handle_tm_unknown_attribute): Remove.
	(handle_tm_fntype_attribute): New.  Combine those and apply to
	function types instead of to decls.
	(tm_attribute_mask): New.
	* trans-mem.c (get_attrs_for, is_tm_pure, is_tm_callable): New.
	(requires_barrier): Don't query tm_pure on variables.
	(examine_call_tm): Query tm_pure on indirect calls.
	(expand_call_tm): Likewise.
	* tree.h (tree_decl_with_vis): Remove tm_var_pure.
	(DECL_IS_TM_PURE_VAR): Remove.
	(DECL_IS_TM_PURE, DECL_IS_TM_UNKNOWN, DECL_IS_TM_CALLABLE): Remove.
	(struct tree_function_decl): Remove tm_callable_flag, tm_pure_flag.
	(is_tm_pure, is_tm_callable): Declare.
	* omp-low.c (copy_var_decl): Don't copy DECL_IS_TM_PURE_VAR.
	* testsuite/gcc.dg/tm/attrib-1.c: New.

2008-11-03  Richard Henderson  <rth@redhat.com>

	* trans-mem.c (expand_assign_tm): Use memmove by default
	for block copies.
	(expand_call_tm): Translate memcpy/memmove into their TM
	counterparts.

2008-11-03  Richard Henderson  <rth@redhat.com>

	* builtin-types.def (BT_FN_LDOUBLE_VPTR, BT_FN_VOID_VPTR_LDOUBLE): New.
	* gtm-builtins.def (BUILT_IN_TM_RETRY, BUILT_IN_TM_MEMMOVE): New.
	(BUILT_IN_TM_STORE_LDOUBLE, BUILT_IN_TM_LOAD_LDOUBLE): New.
	* calls.c (special_function_p): Handle them.
	* trans-mem.c (build_tm_load): Handle BUILT_IN_TM_LOAD_LDOUBLE.
	Set call lhs before insertting it.
	(build_tm_store): Handle BUILT_IN_TM_STORE_LDOUBLE.
	(expand_assign_tm): Remove the old stmt before adding the new ones.

	* c-common.c (c_common_reswords): Add __tm_retry.
	* c-common.h (RID_TM_RETRY): New.
	* c-parser.c (struct c_parser): Add in_tm_atomic.
	(c_parser_statement_after_labels): Handle RID_TM_RETRY.
	(c_parser_tm_atomic): Error if !flag_tm; mirror
	c_parser_omp_structured_block for parsing the contained statement.
	(c_parser_tm_abort_retry): Rename from c_parser_tm_abort; error
	if not contained within a __tm_atomic block; handle __tm_retry.
	* c-tree.h (c_begin_tm_atomic): Remove.
	* c-typeck.c (c_begin_tm_atomic): Remove.
	(c_finish_tm_atomic): Don't c_end_compound_stmt.

	* testsuite/gcc.dg/tm: New directory.

2008-10-29  Richard Henderson  <rth@redhat.com>

	* cgraph.h (struct cgraph_edge): Steal a bit from loop_nest
	for tm_atomic_call.
	* cgraphbuild.c: Revert entire file.
	* gimple-pretty-print.c (dump_gimple_call): Dump in-atomic.
	* gimple.h (GF_CALL_IN_TM_ATOMIC): New.
	(gimple_call_set_in_tm_atomic, gimple_call_in_tm_atomic_p): New.
	* print-tree.c (print_node): Dump tm-clone.
	* trans-mem.c (examine_call_tm): Set tm-atomic in the call stmt.
	(add_stmt_to_tm_region): New.
	(find_tm_clone): Remove.
	(expand_call_tm): Don't call it.  Exit for DECL_IS_TM_CLONE.
	(function_insertion_hook_holder, ipa_tm_analyze_function,
	ipa_tm_add_new_function, ipa_tm_generate_summary,
	ipa_tm_create_version, ipa_tm_decide_version, ipa_tm_execute): New.
	(pass_ipa_tm): Use full IPA_PASS.
	* tree-pass.h: Update to match.
	* tree-cfg.c (is_ctrl_altering_stmt): TM_OPS functions only
	throw (visibly) if they have an eh region number.
	* tree-inline.c (tree_versionable_function_p): Don't check for
	inlining.

2008-10-28  Richard Henderson  <rth@redhat.com>

	* gimple.def (GIMPLE_TM_ATOMIC): Move before GIMPLE_RETURN.
	* gimple.h (gimple_statement_tm_atomic): Inherit from
	gimple_statement_with_memory_ops_base.
	* tree-passes.h, passes.c (pass_tm_done): Remove.
	* trans-mem.c (PROB_VERY_UNLIKELY, PROB_ALWAYS): New.
	(execute_tm_done): Merge into ...
	(execute_tm_edges): ... here.  Collect region before splitting edges.
	(pass_tm_done): Remove.
	(split_and_add_tm_edge): Merge into ...
	(expand_block_edges): ... here.
	(checkpoint_live_in_variables): Remove.
	(expand_tm_atomic): Update edge probabilities, create eh label.
	* tree-cfg.c (gimple_redirect_edge_and_branch): Handle TM_ATOMIC.
	* tree-eh.c (lower_try_finally): Zero this_state.

2008-10-27  Richard Henderson  <rth@redhat.com>

	* trans-mem.c (mark_vops_in_stmt): New.
	(build_tm_load): Construct gimple directly.
	(build_tm_store, expand_assign_tm): Likewise.
	(expand_tm_atomic): Simplify.  Mark VOPS.
	(execute_tm_edges): Call it.
	(pass_tm_edges): Clear gate function, update ssa.
	(pass_tm_memopt): Fix name.

2008-10-27  Richard Henderson  <rth@redhat.com>

	* omp-low.c, gimple-low.c, gsstruct.def, tree-eh.c,
	gimple-pretty-print.c, trans-mem.c, tree-nested.c,
	tree-inline.c, gimple.c, gimple.h, tree-cfg.c: Revert
	the gimple_omp_{set_,}body name change.  Use gimple_tm_atomic_body
	for GIMPLE_TM_ATOMIC nodes.

	* cp/gimplify.c: Likewise.

2008-10-24  Richard Henderson  <rth@redhat.com>

	* calls.c (special_function_p): Include more TM builtins.
	* cfgexpand.c: Revert all changes.
	* except.c (struct eh_region): Add u.transaction.tm_atomic_stmt.
	(gen_eh_region_transaction): Take and store the stmt.
	(for_each_tm_atomic): New.
	* except.h (for_each_tm_atomic): Declare.
	* gimple-pretty-print.c (dump_gimple_assign): Revert.
	* gimple.c (gimple_rhs_class_table): Revert.
	* gimple.h (GTMA_HAVE_CALL_INDIRECT): Remove.
	(GTMA_HAVE_UNCOMMITTED_THROW): New.
	* gimplify.c (gimplify_tm_atomic): Remove EH wrapping.
	* gtm-builtins.def (BUILT_IN_TM_IRREVOKABLE): New.
	(BUILT_IN_TM_MEMCPY): New.
	(BUILT_IN_TM_LOAD_*): Mark PURE.
	* passes.c (init_optimization_passes): Place TM passes.
	* trans-mem.c: Rewrite.
	* tree-cfg.c (make_edges): Revert.
	(is_ctrl_stmt): Rewrite as switch.
	(is_ctrl_altering_stmt): Likewise.  Handle GIMPLE_TM_ATOMIC.
	* tree-eh.c (lower_tm_atomic_eh): New.
	(lower_eh_constructs_2): Record EH region for transactional stmts.
	* tree-flow.h (make_tm_edge): Remove.
	* tree-passes.h (pass_checkpoint_tm): Remove.
	(pass_tm_init, pass_tm_mark, pass_tm_memopt,
	pass_tm_edges, pass_tm_done, pass_ipa_tm): New.
	* tree-ssa-operands.c (get_addr_dereference_operands): Handle
	ADDR_EXPR.
	(add_tm_call_ops): New.
	(maybe_add_call_clobbered_vops): Use it.
	(add_all_call_clobber_ops): Split out from ... 
	(get_asm_expr_operands): ... here.
	(parse_ssa_operands): Convert to switch.
	* tree.def (TM_LOAD, TM_STORE): Remove.

2008-10-21  Richard Henderson  <rth@redhat.com>

	* trans-mem.c: Rename from gtm-low.c.
	* Makefile.in: Update to match.

2008-10-21  Richard Henderson  <rth@redhat.com>

	* tree-cfg.c (cleanup_dead_labels): Fix typo last change.

2008-10-20  Richard Henderson  <rth@redhat.com>

	* builtin-attrs.def (ATTR_RETURNS_TWICE): Remove.
	(ATTR_RETURNSTWICE_NOTHROW_LIST): Remove.
	* builtin-types.def: The TM builtin types are not variadic.
	* calls.c (emit_call_1): Set REG_EH_REGION properly for nothrow
	tm functions; add REG_TM as needed.
	(special_function_p): Add ECF_TM_OPS for TM builtins.
	(flags_from_decl_or_type): Add ECF_TM_OPS for TM clones.
	* cfgbuild.c (control_flow_insn_p): Tidy.
	(struct tmee_data, rtl_make_eh_edge_1): New.
	(rtl_make_eh_edge): Use them with foreach_reachable_handler;
	use foreach_reachable_transaction for TM functions.
	* cfgexpand.c (gimple_assign_rhs_to_tree): Assert we don't
	look past TM_LOAD/TM_STORE.
	(build_tm_load, build_tm_store): New.
	(gimple_to_tree): Use them.
	* except.c (get_eh_region_rtl_label): New.
	(frob_transaction_start): New.
	(build_post_landing_pads): Call it.
	(build_post_landing_pads_tm_only): New.
	(finish_eh_generation): Call it.
	(arh_to_landing_pad, arh_to_label, reachable_handlers): Remove.
	(can_throw_internal): True for TM insns inside a transaction.
	(gate_handle_eh): Enable for TM.
	* except.h (reachable_handlers): Remove.
	(get_eh_region_rtl_label): Declare.
	* gimple.c (get_call_expr_in): Look through VIEW_CONVERT_EXPR.
	* gtm-builtins.def (BUILT_IN_TM_START): Don't mark RETURNS_TWICE.
	* reg-notes.def (TM): New.
	* tree-eh.c (maybe_clean_or_replace_eh_stmt): Handle transactions.
	* tree-optimize.c (execute_fixup_cfg): Likewise.
	* tree.h (ECF_TM_OPS): New.

2008-10-17  Richard Henderson  <rth@redhat.com>

	* except.c (struct eh_region): Add ERT_TRANSACTION.
	(gen_eh_region): Allow if flag_tm.
	(gen_eh_region_transaction, get_eh_region_from_number): New.
	(remove_eh_handler): Export.
	(current_function_has_exception_handlers): Handle ERT_TRANSACTION.
	(build_post_landing_pads, reachable_next_level): Likewise.
	(collect_one_action_chain): Likewise.
	(foreach_reachable_transaction): New.
	* except.h: Add new exported decls.
	* gimple-low.c (struct lower_data): Remove in_transaction.
	(lower_tm_atomic, record_vars_into_tm): Remove.
	* gimple-pretty-print.c (dump_gimple_fmt): Add %x.
	(dump_gimple_assign): Handle TM_LOAD/STORE.
	(dump_gimple_tm_atomic): Dump the subcode.
	* gimple.h (GTMA_HAVE_ABORT, GTMA_HAVE_LOAD, GTMA_HAVE_STORE,
	GTMA_HAVE_CALL_TM, GTMA_HAVE_CALL_IRREVOKABLE, 
	GTMA_MUST_CALL_IRREVOKABLE, GTMA_HAVE_CALL_INDIRECT): New.
	(gimple_tm_atomic_subcode, gimple_tm_atomic_set_subcode): New.
	* gtm-low.c (struct ltm_state, add_stmt_to_transaction,
	lower_assign_tm, lower_call_tm, remove_tm_commits,
	lower_tm_atomic, lower_sequence_tm, lower_sequence_no_tm): New.
	(execute_lower_tm): Use them.
	(TM_START_RESTORE_LIVE_IN, TM_START_ABORT): New.
	(checkpoint_live_in_variables): Rewrite.
	(checkpoint_tm_txn, checkpoint_tm): Remove.
	(expand_tm_atomic): New.
	(execute_checkpoint_tm): Use it.
	(make_tm_edge_1, make_tm_edge, is_transactional_stmt): New.
	(pass_lower_tm): Rename from pass_expand_tm.
	* passes.c (init_optimization_passes): Run pass_lower_tm
	immediately after pass_lower_eh.  Run pass_checkpoint_tm
	after early optimizations.
	* tree-cfg.c (make_edges): Call make_tm_edge.  Conditionally
	create the __tm_atomic abort edge.
	(cleanup_dead_labels): Handle GIMPLE_TM_ATOMIC.  Avoid unnecessary
	writes into the statements to update labels.
	(is_ctrl_altering_stmt): Include is_transactional_stmt.
	(verify_stmt): Handle transactional edges.
	* tree-eh.c (collect_finally_tree): Walk GIMPLE_TM_ATOMIC.
	(lower_eh_constructs_2): Create EH regions for them.
	(verify_eh_edges): Handle transactional edges.
	* tree-flow.h (make_tm_edge, is_transactional_stmt): Declare.

	* c-parser.c (c_parser_tm_abort): Call add_stmt.
	* cgraphbuild.c (prepare_tm_clone): Disable for now.

2008-10-15  Richard Henderson  <rth@redhat.com>

	* builtin-attrs.def (ATTR_RETURNS_TWICE): New.
	(ATTR_RETURNSTWICE_NOTHROW_LIST): New.
	* builtin-types.def: Remove many TM special types.
	* c-common.c (struct c_common_resword): Add __tm_atomic.
	* c-common.h (RID_TM_ATOMIC): New.
	* c-parser.c (c_parser_tm_abort): New.
	(c_parser_omp_construct): Remove PRAGMA_GTM_ATOMIC.
	* c-pragma.c (handle_pragma_optimize): Remove tm atomic pragma.
	* c-pragma.h (PRAGMA_GTM_ATOMIC): Remove.
	(enum pragma_gtm_clause): Remove.
	(c_finish_tm_abort): Remove.
	(prepare_tm_clone): Match Intel's transaction suffix.  Remove	
	code to add an explicit handle.
	* defaults.h (TINYSTM_VERSION_0_9_5, GTM_EXPL_HANDLE,
	GTM_IMPL_HANDLE, TANGER): Remove.
	* gimple-low.c (lower_tm_atomic): Add label after the tm_atomic.
	(lower_stmt): Remove GIMPLE_GTM_RETURN, GIMPLE_GTM_ABORT.
	* gimple.c (gss_for_code): Likewise.
	(gimple_size): Add GSS_TM_ATOMIC.
	(gimple_build_tm_atomic): Add label.
	* gimple-pretty-print.c (dump_gimple_gtm_other): Remove.
	* gimple.def (GIMPLE_GTM_RETURN, GIMPLE_GTM_ABORT): Remove.
	* gimple.h (struct gimple_statement_tm_atomic): New.
	(gimple_tm_atomic_label, gimple_tm_atomic_set_label): New.
	(is_gimple_gtm): Remove.
	* gimplify.c (gimplify_tm_atomic): Wrap the body in EH stuff.
	* gsstruct.def (GSS_TM_ATOMIC): New.
	* gtm-builtins.def: Remove conditional code, simplify interface.
	* gtm-low.c (root_gtm_region, dump_gtm_region, debug_gtm_region,
	debug_all_gtm_regions, new_gtm_region, free_gtm_region_1,
	free_gtm_regions, build_gtm_regions_1, build_gtm_regions,
	remove_gtm_stmts): Remove.
	* tree-cfg.c (make_edges) [GIMPLE_TM_ATOMIC]: Make the edge to
	the out label.
	* tree-flow.h (struct gtm_region): Remove.
	* tree.def (GTM_ABORT): Remove.

	* all files: s/GTM/TM/, s/gtm/tm/.

2008-10-15  Richard Henderson  <rth@redhat.com>

	* Makefile.in (gtm-low.o): Don't depend on gtm-defines.h.
	* builtin-types.def: Don't include it.
	* c-common.c (handle_gtm_unknown_attribute): Stub out
	setting DECL_IS_GTM_UNKNOWN.
	* c-decl.c (merge_decls): Don't propogate it.
	* c-parser.c (c_parser_gtm): Remove decl.
	* c-typeck.c (c_finish_gtm_txn): Don't special-case for OpenMP;
	make that the only case.
	* cgraphbuild.c (prepare_gtm_clone): Use MAIN_NAME_P.  Use concat
	for resetting the assembler name.
	* defaults.h (TINYSTM_VERSION_0_9_5): Set.
	* gimple-low.c (struct lower_data): Add in_transaction.
	(mark_gtm_save_vars): Remove.
	(lower_gtm_directive): Simplify.
	(record_vars_into_tm): Split out from record_vars_into.  When
	inside a transaction, set DECL_IS_GTM_PURE_VAR.
	(lower_gimple_bind): Call it.
	* gimple-pretty-print.c (dump_gimple_gtm_txn): New.
	(dump_gimple_gtm_other): New.
	(dump_gimple_stmt): Call them.
	* gimple.c (gcc_for_code): Add GTM codes.
	(gimple_size): Restructure based on GSS codes.
	(gimple_build_gtm_txn): New.
	(walk_gimple_stmt): Add GTM codes.
	(gimple_rhs_class_table): Make GTM_LOAD/STORE be GIMPLE_SINGLE_RHS.
	* gimple.def (GIMPLE_GTM_TXN, GIMPLE_GTM_RETURN, GIMPLE_GTM_ABORT): New.
	* gimple.h (struct gimple_statement_seq): Rename from 
	gimple_statement_omp.  Update all users.
	(is_gimple_gtm): New.
	* gtm-low.c (new_gtm_region): Remove type code.
	(build_gtm_regions_1): Gimplify.
	(build_gtm_regions): Don't do dominance here.
	(remove_gtm_stmts): Gimpify.
	(collapse_gtm_regions, query_STM_for_flat_nesting): Remove.
	(set_gtm_pure_var, is_gtm_pure_var): Remove.
	(requires_barrier): Merge requires_read_barrier and
	requires_write_barrier; rewrite to match memory variables only.
	(get_real_stm_decl, get_uint_stm_decl, insert_temporary,
	may_repair_rhs, compensate_for_taking_the_address, 
	compose_stm_store_call, compose_stm_load_call, 
	insert_rhs_stm_call, replace_lhs, maybe_replace_rhs_stmt,
	replace_txn_mod_stmt, build_txn_call_expr, replace_txn_stmt,
	setup_recover_bb, setup_begin_bb, expand_gtm_abort, 
	maybe_insert_stm_new, insert_stm_init_thread, 
	maybe_insert_stm_init_thread, insert_stm_exit_thread,
	maybe_insert_stm_exit_thread, expand_gtm_txn_marker,
	expand_gtm_return, insert_stm_init, insert_stm_exit,
	init_label_table, record_bb_into_table, check_and_mark_edges,
	instrument_edges, instrument_return_expr, expand_gtm,
	annotate_gtm_function_body): Remove.
	(maybe_transactify_assign, maybe_transactify_call): New.
	(transactify_stmt): New.
	(checkpoint_live_in_variables, checkpoint_gtm_txn): If 0.
	* omp-low.c (lower_gtm_txn): Remove.
	(diagnose_sb_1): Don't call it.
	* passes.c (pass_checkpoint_gtm): Disable.
	* tree-flow.h (NUM_BB_TXN): Remove.
	(struct gtm_region): Remove txn_bbs, type.
	* tree-pretty-print.c (dump_generic_node): Handle GTM codes.
	* tree.def (GTM_LOAD, GTM_STORE): Add.
	(GTM_RETURN): Remove.
	* tree.h (GTM_DIRECTIVE_P): Remove.
	(DECL_IS_GTM_UNKNOWN): Disable.

2008-10-10  Richard Henderson  <rth@redhat.com>

	Original work from Martin Schindewolf <schindew@ira.uka.de> et al.
	- Imported their 4.3 based patch into this 4.4 branch.
	- Raw conflicts resolved, but not yet converted to gimple tuples.
