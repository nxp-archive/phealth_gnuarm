/* Calculate branch probabilities, and basic block execution counts.
   Copyright (C) 1990, 1991, 1992, 1993, 1994, 1996, 1997, 1998, 1999,
   2000, 2001  Free Software Foundation, Inc.
   Contributed by James E. Wilson, UC Berkeley/Cygnus Support;
   based on some ideas from Dain Samples of UC Berkeley.
   Further mangling by Bob Manson, Cygnus Support.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 2, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING.  If not, write to the Free
Software Foundation, 59 Temple Place - Suite 330, Boston, MA
02111-1307, USA.  */

/* Generate basic block profile instrumentation and auxiliary files.
   Profile generation is optimized, so that not all arcs in the basic
   block graph need instrumenting. First, the BB graph is closed with
   one entry (function start), and one exit (function exit).  Any
   ABNORMAL_EDGE cannot be instrumented (because there is no control
   path to place the code). We close the graph by inserting fake
   EDGE_FAKE edges to the EXIT_BLOCK, from the sources of abnormal
   edges that do not go to the exit_block. We ignore such abnormal
   edges.  Naturally these fake edges are never directly traversed,
   and so *cannot* be directly instrumented.  Some other graph
   massaging is done. To optimize the instrumentation we generate the
   BB minimal span tree, only edges that are not on the span tree
   (plus the entry point) need instrumenting. From that information
   all other edge counts can be deduced.  By construction all fake
   edges must be on the spanning tree. We also attempt to place
   EDGE_CRITICAL edges on the spanning tree.

   The auxiliary file generated is <dumpbase>.bbg. The format is
   described in full in gcov-io.h.  */

/* ??? Register allocation should use basic block execution counts to
   give preference to the most commonly executed blocks.  */

/* ??? Should calculate branch probabilities before instrumenting code, since
   then we can use arc counts to help decide which arcs to instrument.  */

#include "config.h"
#include "system.h"
#include "rtl.h"
#include "tree.h"
#include "flags.h"
#include "insn-config.h"
#include "output.h"
#include "regs.h"
#include "expr.h"
#include "function.h"
#include "toplev.h"
#include "ggc.h"
#include "hard-reg-set.h"
#include "basic-block.h"
#include "cfgloop.h"
#include "params.h"
#include "gcov-io.h"
#include "target.h"
#include "profile.h"
#include "libfuncs.h"
#include "langhooks.h"
#include "hashtab.h"
#include "vpt.h"

/* Additional information about the edges we need.  */
struct edge_info {
  unsigned int count_valid : 1;
  
  /* Is on the spanning tree.  */
  unsigned int on_tree : 1;
  
  /* Pretend this edge does not exist (it is abnormal and we've
     inserted a fake to compensate).  */
  unsigned int ignore : 1;
};

struct bb_info {
  unsigned int count_valid : 1;

  /* Number of successor and predecessor edges.  */
  gcov_type succ_count;
  gcov_type pred_count;
};

struct function_list
{
  struct function_list *next; 	/* next function */
  const char *name; 		/* function name */
  unsigned cfg_checksum;	/* function checksum */
  unsigned n_counter_sections;	/* number of counter sections */
  struct counter_section counter_sections[MAX_COUNTER_SECTIONS];
  				/* the sections */
};

static struct function_list *functions_head = 0;
static struct function_list **functions_tail = &functions_head;

#define EDGE_INFO(e)  ((struct edge_info *) (e)->aux)
#define BB_INFO(b)  ((struct bb_info *) (b)->aux)

/* Keep all basic block indexes nonnegative in the gcov output.  Index 0
   is used for entry block, last block exit block.  */
#define BB_TO_GCOV_INDEX(bb)  ((bb) == ENTRY_BLOCK_PTR ? 0		\
			       : ((bb) == EXIT_BLOCK_PTR		\
				  ? last_basic_block + 1 : (bb)->index + 1))

/* Instantiate the profile info structure.  */

struct profile_info profile_info;

/* Name and file pointer of the output file for the basic block graph.  */

static FILE *bbg_file;
static char *bbg_file_name;

/* Name and file pointer of the input file for the arc count data.  */

static FILE *da_file;
static char *da_file_name;

/* The name of the count table. Used by the edge profiling code.  */
static GTY(()) rtx profiler_label;

/* The name of the loop histograms table.  */
static GTY(()) rtx loop_histograms_label;

/* The name of the value histograms table.  */
static GTY(()) rtx value_histograms_label;

/* The name of the same value histograms table.  */
static GTY(()) rtx same_value_histograms_label;

/* Collect statistics on the performance of this pass for the entire source
   file.  */

static int total_num_blocks;
static int total_num_edges;
static int total_num_edges_ignored;
static int total_num_edges_instrumented;
static int total_num_blocks_created;
static int total_num_passes;
static int total_num_times_called;
static int total_hist_br_prob[20];
static int total_num_never_executed;
static int total_num_branches;

/* Forward declarations.  */
static void find_spanning_tree PARAMS ((struct edge_list *));
static rtx gen_edge_profiler PARAMS ((int));
static rtx gen_interval_profiler PARAMS ((struct histogram_value *, rtx, int));
static rtx gen_range_profiler PARAMS ((struct histogram_value *, rtx, int));
static rtx gen_pow2_profiler PARAMS ((struct histogram_value *, rtx, int));
static rtx gen_one_value_profiler PARAMS ((struct histogram_value *, rtx, int));
static void instrument_edges PARAMS ((struct edge_list *));
static void instrument_loops PARAMS ((struct loops *));
static void instrument_values PARAMS ((unsigned, struct histogram_value *));
static void compute_branch_probabilities PARAMS ((void));
static void compute_loop_histograms PARAMS ((struct loops *));
static void compute_value_histograms PARAMS ((unsigned, struct histogram_value *));
static hashval_t htab_counts_index_hash PARAMS ((const void *));
static int htab_counts_index_eq PARAMS ((const void *, const void *));
static void htab_counts_index_del PARAMS ((void *));
static void cleanup_counts_index PARAMS ((int));
static int index_counts_file PARAMS ((void));
static gcov_type * get_exec_counts PARAMS ((void));
static gcov_type * get_histogram_counts PARAMS ((unsigned, unsigned));
static unsigned compute_checksum PARAMS ((void));
static basic_block find_group PARAMS ((basic_block));
static void union_groups PARAMS ((basic_block, basic_block));
static void set_purpose PARAMS ((tree, tree));
static rtx label_for_tag PARAMS ((unsigned));
static tree build_counter_section_fields PARAMS ((void));
static tree build_counter_section_value PARAMS ((unsigned, unsigned));
static tree build_counter_section_data_fields PARAMS ((void));
static tree build_counter_section_data_value PARAMS ((unsigned, unsigned));
static tree build_function_info_fields PARAMS ((void));
static tree build_function_info_value PARAMS ((struct function_list *));
static tree build_gcov_info_fields PARAMS ((tree));
static tree build_gcov_info_value PARAMS ((void));


/* Add edge instrumentation code to the entire insn chain.

   F is the first insn of the chain.
   NUM_BLOCKS is the number of basic blocks found in F.  */

static void
instrument_edges (el)
     struct edge_list *el;
{
  int num_instr_edges = 0;
  int num_edges = NUM_EDGES (el);
  basic_block bb;
  struct section_info *section_info;
  remove_fake_edges ();

  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, NULL, next_bb)
    {
      edge e = bb->succ;
      while (e)
	{
	  struct edge_info *inf = EDGE_INFO (e);
	  if (!inf->ignore && !inf->on_tree)
	    {
	      if (e->flags & EDGE_ABNORMAL)
		abort ();
	      if (rtl_dump_file)
		fprintf (rtl_dump_file, "Edge %d to %d instrumented%s\n",
			 e->src->index, e->dest->index,
			 EDGE_CRITICAL_P (e) ? " (and split)" : "");
	      insert_insn_on_edge (
			 gen_edge_profiler (total_num_edges_instrumented
					    + num_instr_edges++), e);
	    }
	  e = e->succ_next;
	}
    }

  section_info = find_counters_section (GCOV_TAG_ARC_COUNTS);
  section_info->n_counters_now = num_instr_edges;
  total_num_edges_instrumented += num_instr_edges;
  section_info->n_counters = total_num_edges_instrumented;

  total_num_blocks_created += num_edges;
  if (rtl_dump_file)
    fprintf (rtl_dump_file, "%d edges instrumented\n", num_instr_edges);
}

/* Add code that counts histograms of first iterations of LOOPS.  */
static void
instrument_loops (loops)
     struct loops *loops;
{
  enum machine_mode mode = mode_for_size (GCOV_TYPE_SIZE, MODE_INT, 0);
  rtx *loop_counters;
  rtx sequence;
  unsigned i, histogram_steps;
  basic_block bb;
  edge e;
  int n_histogram_counters;
  struct section_info *section_info;
  
  histogram_steps = PARAM_VALUE (PARAM_MAX_PEEL_TIMES);
  if (histogram_steps < (unsigned) PARAM_VALUE (PARAM_MAX_UNROLL_TIMES))
    histogram_steps = PARAM_VALUE (PARAM_MAX_UNROLL_TIMES);

  loop_counters = xmalloc (sizeof (rtx) * loops->num);
  for (i = 1; i < loops->num; i++)
    loop_counters[i] = gen_reg_rtx (mode);

  section_info = find_counters_section (GCOV_TAG_LOOP_HISTOGRAMS);
  /* First the easy part -- code to initalize counter on preheader edge &
     to increase it on latch one.  */
  for (i = 1; i < loops->num; i++)
    {
      rtx tmp;

      start_sequence ();
      emit_move_insn (loop_counters[i], const0_rtx);
      sequence = get_insns ();
      end_sequence ();
      insert_insn_on_edge (sequence, loop_preheader_edge (loops->parray[i]));

      start_sequence ();
      tmp = expand_simple_binop (mode, PLUS, loop_counters[i], const1_rtx,
				 loop_counters[i], 0, OPTAB_WIDEN);
      if (tmp != loop_counters[i])
	emit_move_insn (loop_counters[i], tmp);
      sequence = get_insns ();
      end_sequence ();
      insert_insn_on_edge (sequence, loop_latch_edge (loops->parray[i]));
    }
 
  /* And now emit code to generate the histogram on exit edges. The trouble
     is that there may be more than one edge leaving the loop and the single
     edge may exit multiple loops.  The other problem is that the exit edge
     may be abnormal & critical; in this case we just ignore it.  */

  FOR_EACH_BB (bb)
    {
      for (e = bb->succ; e; e = e->succ_next)
	{
	  struct loop *src_loop, *dest_loop, *loop;

	  if ((e->flags & EDGE_ABNORMAL) && EDGE_CRITICAL_P (e))
	    continue;

	  src_loop = e->src->loop_father;
	  dest_loop = find_common_loop (src_loop, e->dest->loop_father);

	  for (loop = src_loop; loop != dest_loop; loop = loop->outer)
	    {
	      struct histogram_value cdesc;

	      cdesc.value = loop_counters[loop->num];
	      cdesc.mode = mode;
	      cdesc.seq = NULL_RTX;
	      cdesc.hdata.intvl.int_start = 0;
	      cdesc.hdata.intvl.steps = histogram_steps;
	      cdesc.hdata.intvl.may_be_less = 0;
	      cdesc.hdata.intvl.may_be_more = 1;
	      insert_insn_on_edge (
			gen_interval_profiler (&cdesc, loop_histograms_label,
			section_info->n_counters
				+ (loop->num - 1) * (histogram_steps + 1)),
			e);
	    }
	}
    }
  free (loop_counters);

  n_histogram_counters = (loops->num - 1) * (histogram_steps + 1);
  section_info->n_counters_now = n_histogram_counters;
  section_info->n_counters += n_histogram_counters;
}

/* Add code to measure histograms of VALUES.  */
static void
instrument_values (n_values, values)
     unsigned n_values;
     struct histogram_value *values;
{
  rtx sequence;
  unsigned i;
  edge e;
  int n_histogram_counters = 0, n_sv_histogram_counters = 0;
  struct section_info *section_info, *sv_section_info;
 
  sv_section_info = find_counters_section (GCOV_TAG_SAME_VALUE_HISTOGRAMS);
  section_info = find_counters_section (GCOV_TAG_VALUE_HISTOGRAMS);

  /* Emit code to generate the histograms before the insns.  */

  for (i = 0; i < n_values; i++)
    {
      e = split_block (BLOCK_FOR_INSN (values[i].insn),
		       PREV_INSN (values[i].insn));

      switch (values[i].type)
	{
	case HIST_TYPE_INTERVAL:
	  sequence = 
	      gen_interval_profiler (values + i, value_histograms_label,
			section_info->n_counters + n_histogram_counters);
	  n_histogram_counters += values[i].n_counters;
	  break;

	case HIST_TYPE_RANGE:
	  sequence = 
	      gen_range_profiler (values + i, value_histograms_label,
			section_info->n_counters + n_histogram_counters);
	  n_histogram_counters += values[i].n_counters;
	  break;

	case HIST_TYPE_POW2:
	  sequence = 
	      gen_pow2_profiler (values + i, value_histograms_label,
			section_info->n_counters + n_histogram_counters);
	  n_histogram_counters += values[i].n_counters;
	  break;

	case HIST_TYPE_ONE_VALUE:
	  sequence = 
	      gen_one_value_profiler (values + i, same_value_histograms_label,
			sv_section_info->n_counters + n_sv_histogram_counters);
	  n_sv_histogram_counters += values[i].n_counters;
	  break;

	default:
	  abort ();
	}

      insert_insn_on_edge (sequence, e);
    }

  section_info->n_counters_now = n_histogram_counters;
  section_info->n_counters += n_histogram_counters;
  sv_section_info->n_counters_now = n_sv_histogram_counters;
  sv_section_info->n_counters += n_sv_histogram_counters;
}

struct section_reference
{
  long offset;
  int owns_summary;
  long *summary;
};

struct da_index_entry
{
  /* We hash by  */
  char *function_name;
  unsigned section;
  /* and store  */
  unsigned checksum;
  unsigned n_offsets;
  struct section_reference *offsets;
};

static hashval_t
htab_counts_index_hash (of)
     const void *of;
{
  const struct da_index_entry *entry = of;

  return htab_hash_string (entry->function_name) ^ entry->section;
}

static int
htab_counts_index_eq (of1, of2)
     const void *of1;
     const void *of2;
{
  const struct da_index_entry *entry1 = of1;
  const struct da_index_entry *entry2 = of2;

  return !strcmp (entry1->function_name, entry2->function_name)
	  && entry1->section == entry2->section;
}

static void
htab_counts_index_del (what)
     void *what;
{
  struct da_index_entry *entry = what;
  unsigned i;

  for (i = 0; i < entry->n_offsets; i++)
    {
      struct section_reference *act = entry->offsets + i;
      if (act->owns_summary)
	free (act->summary);
    }
  free (entry->function_name);
  free (entry->offsets);
  free (entry);
}

static char *counts_file_name;
static htab_t counts_file_index = NULL;

static void
cleanup_counts_index (close_file)
     int close_file;
{
  if (da_file && close_file)
    {
      fclose (da_file);
      da_file = NULL;
    }
  if (counts_file_name)
    free (counts_file_name);
  counts_file_name = NULL;
  if (counts_file_index)
    htab_delete (counts_file_index);
  counts_file_index = NULL;
}

static int
index_counts_file ()
{
  char *function_name_buffer = NULL;
  unsigned magic, version, ix, checksum;
  long *summary;

  if (!da_file)
    return 0;
  counts_file_index = htab_create (10, htab_counts_index_hash, htab_counts_index_eq, htab_counts_index_del);

  /* No .da file, no data.  */
  if (!da_file)
    return 0;

  /* Now index all profile sections.  */

  rewind (da_file);

  summary = NULL;

  if (gcov_read_unsigned (da_file, &magic) || magic != GCOV_DATA_MAGIC)
    {
      warning ("`%s' is not a gcov data file", da_file_name);
      goto cleanup;
    }
  if (gcov_read_unsigned (da_file, &version) || version != GCOV_VERSION)
    {
      char v[4], e[4];
      magic = GCOV_VERSION;
      
      for (ix = 4; ix--; magic >>= 8, version >>= 8)
	{
	  v[ix] = version;
	  e[ix] = magic;
	}
      warning ("`%s' is version `%.4s', expected version `%.4s'",
	       da_file_name, v, e);
      goto cleanup;
    }
  
  while (1)
    {
      unsigned tag, length;
      long offset;
      
      offset = gcov_save_position (da_file);
      if (gcov_read_unsigned (da_file, &tag)
	  || gcov_read_unsigned (da_file, &length))
	{
	  if (feof (da_file))
	    break;
	corrupt:;
	  warning ("`%s' is corrupted", da_file_name);
	  goto cleanup;
	}
      if (tag == GCOV_TAG_FUNCTION)
	{
	  if (gcov_read_string (da_file, &function_name_buffer, NULL)
	      || gcov_read_unsigned (da_file, &checksum))
	    goto corrupt;
	  continue;
	}
      if (tag == GCOV_TAG_PROGRAM_SUMMARY)
	{
	  if (length != GCOV_SUMMARY_LENGTH)
	    goto corrupt;

	  if (summary)
	    *summary = offset;
	  summary = NULL;
	}
      else
	{
	  if (function_name_buffer)
	    {
	      struct da_index_entry **slot, elt;
	      elt.function_name = function_name_buffer;
	      elt.section = tag;

	      slot = (struct da_index_entry **)
		htab_find_slot (counts_file_index, &elt, INSERT);
	      if (*slot)
		{
		  if ((*slot)->checksum != checksum)
		    {
		      warning ("profile mismatch for `%s'", function_name_buffer);
		      goto cleanup;
		    }
		  (*slot)->n_offsets++;
		  (*slot)->offsets = xrealloc ((*slot)->offsets,
					       sizeof (struct section_reference) * (*slot)->n_offsets);
		}
	      else
		{
		  *slot = xmalloc (sizeof (struct da_index_entry));
		  (*slot)->function_name = xstrdup (function_name_buffer);
		  (*slot)->section = tag;
		  (*slot)->checksum = checksum;
		  (*slot)->n_offsets = 1;
		  (*slot)->offsets = xmalloc (sizeof (struct section_reference));
		}
	      (*slot)->offsets[(*slot)->n_offsets - 1].offset = offset;
	      if (summary)
		(*slot)->offsets[(*slot)->n_offsets - 1].owns_summary = 0;
	      else
		{
		  summary = xmalloc (sizeof (long));
		  *summary = -1;
		  (*slot)->offsets[(*slot)->n_offsets - 1].owns_summary = 1;
		}
	      (*slot)->offsets[(*slot)->n_offsets - 1].summary = summary;
	    }
	}
      if (gcov_skip (da_file, length))
	goto corrupt;
    }

  free (function_name_buffer);

  return 1;

cleanup:
  cleanup_counts_index (1);
  if (function_name_buffer)
    free (function_name_buffer);
  return 0;
}

/* Computes hybrid profile for all matching entries in da_file.
   Sets max_counter_in_program as a side effect.  */

static gcov_type *
get_exec_counts ()
{
  unsigned num_edges = 0;
  basic_block bb;
  gcov_type *profile;
  gcov_type max_count;
  unsigned ix, i, tag, length, num;
  const char *name = IDENTIFIER_POINTER (DECL_ASSEMBLER_NAME (current_function_decl));
  struct da_index_entry *entry, what;
  struct section_reference *act;
  gcov_type count;
  struct gcov_summary summ;

  profile_info.max_counter_in_program = 0;
  profile_info.count_profiles_merged = 0;

  /* No .da file, no execution counts.  */
  if (!da_file)
    return NULL;
  if (!counts_file_index)
    abort ();

  /* Count the edges to be (possibly) instrumented.  */

  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, NULL, next_bb)
    {
      edge e;
      for (e = bb->succ; e; e = e->succ_next)
	if (!EDGE_INFO (e)->ignore && !EDGE_INFO (e)->on_tree)
	  num_edges++;
    }

  /* now read and combine all matching profiles.  */

  profile = xmalloc (sizeof (gcov_type) * num_edges);

  for (ix = 0; ix < num_edges; ix++)
    profile[ix] = 0;

  what.function_name = (char *) name;
  what.section = GCOV_TAG_ARC_COUNTS;
  entry = htab_find (counts_file_index, &what);
  if (!entry)
    {
      warning ("No profile for function '%s' found.", name);
      goto cleanup;
    }
  
  if (entry->checksum != profile_info.current_function_cfg_checksum)
    {
      warning ("profile mismatch for `%s'", current_function_name);
      goto cleanup;
    }

  for (i = 0; i < entry->n_offsets; i++)
    {
      act = entry->offsets + i;

      /* Read arc counters.  */
      max_count = 0;
      gcov_resync (da_file, act->offset, 0);

      if (gcov_read_unsigned (da_file, &tag)
	  || gcov_read_unsigned (da_file, &length)
	  || tag != GCOV_TAG_ARC_COUNTS)
	{
	  /* We have already passed through file, so any error means
	     something is rotten.  */
	  abort ();
	}
      num = length / 8;

      if (num != num_edges)
	{
	  warning ("profile mismatch for `%s'", current_function_name);
	  goto cleanup;
	}
	  
      for (ix = 0; ix != num; ix++)
	{
	  if (gcov_read_counter (da_file, &count, false))
	    abort ();
	  if (count > max_count)
	    max_count = count;
	  profile[ix] += count;
	}

      /* Read program summary.  */
      if (*act->summary != -1)
	{
	  gcov_resync (da_file, *act->summary, 0);
	  if (gcov_read_unsigned (da_file, &tag)
	      || gcov_read_unsigned (da_file, &length)
	      || tag != GCOV_TAG_PROGRAM_SUMMARY
	      || gcov_read_summary (da_file, &summ))
	    abort ();
	  profile_info.count_profiles_merged += summ.runs;
	  profile_info.max_counter_in_program += summ.arc_sum_max;
	}
      else
	summ.runs = 0;
      if (!summ.runs)
	{
	  profile_info.count_profiles_merged++;
	  profile_info.max_counter_in_program += max_count;
	}
    }

  if (rtl_dump_file)
    {
      fprintf(rtl_dump_file, "Merged %i profiles with maximal count %i.\n",
	      profile_info.count_profiles_merged,
	      (int)profile_info.max_counter_in_program);
    }

  return profile;

cleanup:;
  free (profile);
  cleanup_counts_index (1);
  return 0;
}

/* Get histogram counters.  */
static gcov_type *
get_histogram_counts (section_tag, n_counters)
     unsigned section_tag;
     unsigned n_counters;
{
  gcov_type *profile;
  unsigned ix, i, tag, length, num;
  const char *name = IDENTIFIER_POINTER (DECL_ASSEMBLER_NAME (current_function_decl));
  struct da_index_entry *entry, what;
  struct section_reference *act;
  gcov_type count;
  merger_function merger;

  /* No .da file, no execution counts.  */
  if (!da_file)
    return NULL;
  if (!counts_file_index)
    abort ();

  /* No counters to read.  */
  if (!n_counters)
    return NULL;

  /* now read and combine all matching profiles.  */

  profile = xmalloc (sizeof (gcov_type) * n_counters);

  for (ix = 0; ix < n_counters; ix++)
    profile[ix] = 0;

  what.function_name = (char *) name;
  what.section = section_tag;
  entry = htab_find (counts_file_index, &what);
  if (!entry)
    {
      warning ("No profile for function '%s' found.", name);
      goto cleanup;
    }
  
  if (entry->checksum != profile_info.current_function_cfg_checksum)
    {
      warning ("profile mismatch for `%s'", current_function_name);
      goto cleanup;
    }

  for (i = 0; i < entry->n_offsets; i++)
    {
      act = entry->offsets + i;

      /* Read arc counters.  */
      gcov_resync (da_file, act->offset, 0);

      if (gcov_read_unsigned (da_file, &tag)
	  || gcov_read_unsigned (da_file, &length)
	  || tag != section_tag)
	{
	  /* We have already passed through file, so any error means
	     something is rotten.  */
	  abort ();
	}
      num = length / 8;

      if (num != n_counters)
	{
	  warning ("profile mismatch for `%s'", current_function_name);
	  goto cleanup;
	}

      if ((merger = profile_merger_for_tag (tag)))
	{
	  if ((*merger) (da_file, profile, n_counters))
	    {
	      warning ("profile mismatch for `%s'", current_function_name);
	      goto cleanup;
	    }
	}
      else
	{
	  for (ix = 0; ix != num; ix++)
	    {
	      if (gcov_read_counter (da_file, &count, false))
		{
		  warning ("profile mismatch for `%s'", current_function_name);
		  goto cleanup;
		}
	      profile[ix] += count;
	    }
	}
    }

  return profile;

cleanup:;
  free (profile);
  cleanup_counts_index (1);
  return 0;
}

/* Load loop histograms from .da file.  */
static void
compute_loop_histograms (loops)
     struct loops *loops;
{
  unsigned histogram_steps;
  unsigned i;
  gcov_type *histogram_counts, *act_count;
 
  histogram_steps = PARAM_VALUE (PARAM_MAX_PEEL_TIMES);
  if (histogram_steps < (unsigned) PARAM_VALUE (PARAM_MAX_UNROLL_TIMES))
    histogram_steps = PARAM_VALUE (PARAM_MAX_UNROLL_TIMES);

  histogram_counts = get_histogram_counts (GCOV_TAG_LOOP_HISTOGRAMS,
					   (loops->num - 1) * (histogram_steps + 1));
  if (!histogram_counts)
    return;

  act_count = histogram_counts;
  for (i = 1; i < loops->num; i++)
    {
      edge latch = loop_latch_edge (loops->parray[i]);

      latch->loop_histogram = xmalloc (sizeof (struct loop_histogram));
      latch->loop_histogram->steps = histogram_steps;
      latch->loop_histogram->counts = xmalloc (histogram_steps * sizeof (gcov_type));
      memcpy (latch->loop_histogram->counts, act_count,
	      histogram_steps * sizeof (gcov_type));
      latch->loop_histogram->more = act_count[histogram_steps];
      act_count += histogram_steps + 1;
    }

  free (histogram_counts);
  
  find_counters_section (GCOV_TAG_LOOP_HISTOGRAMS)->present = 1;
}

/* Load value histograms from .da file.  */
static void
compute_value_histograms (n_values, values)
     unsigned n_values;
     struct histogram_value *values;
{
  unsigned i, j, n_histogram_counters, n_sv_histogram_counters;
  gcov_type *histogram_counts, *act_count;
  gcov_type *sv_histogram_counts, *sv_act_count;
  gcov_type *aact_count;
  
  n_histogram_counters = 0;
  n_sv_histogram_counters = 0;
  for (i = 0; i < n_values; i++)
    {
      if (values[i].type == HIST_TYPE_ONE_VALUE)
	n_sv_histogram_counters += values[i].n_counters;
      else
	n_histogram_counters += values[i].n_counters;
    }

  histogram_counts = get_histogram_counts (GCOV_TAG_VALUE_HISTOGRAMS,
					   n_histogram_counters);
  sv_histogram_counts = get_histogram_counts (GCOV_TAG_SAME_VALUE_HISTOGRAMS,
   					      n_sv_histogram_counters);
  if (!histogram_counts && !sv_histogram_counts)
    return;

  act_count = histogram_counts;
  sv_act_count = sv_histogram_counts;
  for (i = 0; i < n_values; i++)
    {
      rtx hist_list = NULL_RTX;

      
      if (values[i].type == HIST_TYPE_ONE_VALUE)
	{
	  aact_count = sv_act_count;
	  sv_act_count += values[i].n_counters;
	}
      else
	{
	  aact_count = act_count;
	  act_count += values[i].n_counters;
	}
      for (j = values[i].n_counters; j > 0; j--)
	hist_list = alloc_EXPR_LIST (0, GEN_INT (aact_count[j - 1]), hist_list);
      hist_list = alloc_EXPR_LIST (0, copy_rtx (values[i].value), hist_list);
      hist_list = alloc_EXPR_LIST (0, GEN_INT (values[i].type), hist_list);
      REG_NOTES (values[i].insn) =
	      alloc_EXPR_LIST (REG_VALUE_HISTOGRAM, hist_list,
			       REG_NOTES (values[i].insn));
    }

  free (histogram_counts);
  free (sv_histogram_counts);
  find_counters_section (GCOV_TAG_VALUE_HISTOGRAMS)->present = 1;
  find_counters_section (GCOV_TAG_SAME_VALUE_HISTOGRAMS)->present = 1;
}

/* Compute the branch probabilities for the various branches.
   Annotate them accordingly.  */

static void
compute_branch_probabilities ()
{
  basic_block bb;
  int i;
  int num_edges = 0;
  int changes;
  int passes;
  int hist_br_prob[20];
  int num_never_executed;
  int num_branches;
  gcov_type *exec_counts = get_exec_counts ();
  int exec_counts_pos = 0;

  /* Attach extra info block to each bb.  */

  alloc_aux_for_blocks (sizeof (struct bb_info));
  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, NULL, next_bb)
    {
      edge e;

      for (e = bb->succ; e; e = e->succ_next)
	if (!EDGE_INFO (e)->ignore)
	  BB_INFO (bb)->succ_count++;
      for (e = bb->pred; e; e = e->pred_next)
	if (!EDGE_INFO (e)->ignore)
	  BB_INFO (bb)->pred_count++;
    }

  /* Avoid predicting entry on exit nodes.  */
  BB_INFO (EXIT_BLOCK_PTR)->succ_count = 2;
  BB_INFO (ENTRY_BLOCK_PTR)->pred_count = 2;

  /* For each edge not on the spanning tree, set its execution count from
     the .da file.  */

  /* The first count in the .da file is the number of times that the function
     was entered.  This is the exec_count for block zero.  */

  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, NULL, next_bb)
    {
      edge e;
      for (e = bb->succ; e; e = e->succ_next)
	if (!EDGE_INFO (e)->ignore && !EDGE_INFO (e)->on_tree)
	  {
	    num_edges++;
	    if (exec_counts)
	      {
		e->count = exec_counts[exec_counts_pos++];
	      }
	    else
	      e->count = 0;

	    EDGE_INFO (e)->count_valid = 1;
	    BB_INFO (bb)->succ_count--;
	    BB_INFO (e->dest)->pred_count--;
	    if (rtl_dump_file)
	      {
		fprintf (rtl_dump_file, "\nRead edge from %i to %i, count:",
			 bb->index, e->dest->index);
		fprintf (rtl_dump_file, HOST_WIDEST_INT_PRINT_DEC,
			 (HOST_WIDEST_INT) e->count);
	      }
	  }
    }

  if (rtl_dump_file)
    fprintf (rtl_dump_file, "\n%d edge counts read\n", num_edges);

  /* For every block in the file,
     - if every exit/entrance edge has a known count, then set the block count
     - if the block count is known, and every exit/entrance edge but one has
     a known execution count, then set the count of the remaining edge

     As edge counts are set, decrement the succ/pred count, but don't delete
     the edge, that way we can easily tell when all edges are known, or only
     one edge is unknown.  */

  /* The order that the basic blocks are iterated through is important.
     Since the code that finds spanning trees starts with block 0, low numbered
     edges are put on the spanning tree in preference to high numbered edges.
     Hence, most instrumented edges are at the end.  Graph solving works much
     faster if we propagate numbers from the end to the start.

     This takes an average of slightly more than 3 passes.  */

  changes = 1;
  passes = 0;
  while (changes)
    {
      passes++;
      changes = 0;
      FOR_BB_BETWEEN (bb, EXIT_BLOCK_PTR, NULL, prev_bb)
	{
	  struct bb_info *bi = BB_INFO (bb);
	  if (! bi->count_valid)
	    {
	      if (bi->succ_count == 0)
		{
		  edge e;
		  gcov_type total = 0;

		  for (e = bb->succ; e; e = e->succ_next)
		    total += e->count;
		  bb->count = total;
		  bi->count_valid = 1;
		  changes = 1;
		}
	      else if (bi->pred_count == 0)
		{
		  edge e;
		  gcov_type total = 0;

		  for (e = bb->pred; e; e = e->pred_next)
		    total += e->count;
		  bb->count = total;
		  bi->count_valid = 1;
		  changes = 1;
		}
	    }
	  if (bi->count_valid)
	    {
	      if (bi->succ_count == 1)
		{
		  edge e;
		  gcov_type total = 0;

		  /* One of the counts will be invalid, but it is zero,
		     so adding it in also doesn't hurt.  */
		  for (e = bb->succ; e; e = e->succ_next)
		    total += e->count;

		  /* Seedgeh for the invalid edge, and set its count.  */
		  for (e = bb->succ; e; e = e->succ_next)
		    if (! EDGE_INFO (e)->count_valid && ! EDGE_INFO (e)->ignore)
		      break;

		  /* Calculate count for remaining edge by conservation.  */
		  total = bb->count - total;

		  if (! e)
		    abort ();
		  EDGE_INFO (e)->count_valid = 1;
		  e->count = total;
		  bi->succ_count--;

		  BB_INFO (e->dest)->pred_count--;
		  changes = 1;
		}
	      if (bi->pred_count == 1)
		{
		  edge e;
		  gcov_type total = 0;

		  /* One of the counts will be invalid, but it is zero,
		     so adding it in also doesn't hurt.  */
		  for (e = bb->pred; e; e = e->pred_next)
		    total += e->count;

		  /* Seedgeh for the invalid edge, and set its count.  */
		  for (e = bb->pred; e; e = e->pred_next)
		    if (! EDGE_INFO (e)->count_valid && ! EDGE_INFO (e)->ignore)
		      break;

		  /* Calculate count for remaining edge by conservation.  */
		  total = bb->count - total + e->count;

		  if (! e)
		    abort ();
		  EDGE_INFO (e)->count_valid = 1;
		  e->count = total;
		  bi->pred_count--;

		  BB_INFO (e->src)->succ_count--;
		  changes = 1;
		}
	    }
	}
    }
  if (rtl_dump_file)
    dump_flow_info (rtl_dump_file);

  total_num_passes += passes;
  if (rtl_dump_file)
    fprintf (rtl_dump_file, "Graph solving took %d passes.\n\n", passes);

  /* If the graph has been correctly solved, every block will have a
     succ and pred count of zero.  */
  FOR_EACH_BB (bb)
    {
      if (BB_INFO (bb)->succ_count || BB_INFO (bb)->pred_count)
	abort ();
    }

  /* For every edge, calculate its branch probability and add a reg_note
     to the branch insn to indicate this.  */

  for (i = 0; i < 20; i++)
    hist_br_prob[i] = 0;
  num_never_executed = 0;
  num_branches = 0;

  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, NULL, next_bb)
    {
      edge e;
      gcov_type total;
      rtx note;

      total = bb->count;
      if (total)
	{
	  for (e = bb->succ; e; e = e->succ_next)
	    {
		/* Function may return twice in the cased the called fucntion is
		   setjmp or calls fork, but we can't represent this by extra
		   edge from the entry, since extra edge from the exit is
		   already present.  We get negative frequency from the entry
		   point.  */
		if ((e->count < 0
		     && e->dest == EXIT_BLOCK_PTR)
		    || (e->count > total
		        && e->dest != EXIT_BLOCK_PTR))
		  {
		    rtx insn = bb->end;

		    while (GET_CODE (insn) != CALL_INSN
			   && insn != bb->head
			   && keep_with_call_p (insn))
		      insn = PREV_INSN (insn);
		    if (GET_CODE (insn) == CALL_INSN)
		      e->count = e->count < 0 ? 0 : total;
		  }

		e->probability = (e->count * REG_BR_PROB_BASE + total / 2) / total;
		if (e->probability < 0 || e->probability > REG_BR_PROB_BASE)
		  {
		    error ("corrupted profile info: prob for %d-%d thought to be %f",
			   e->src->index, e->dest->index,
			   e->probability/(double) REG_BR_PROB_BASE);
		    e->probability = REG_BR_PROB_BASE / 2;
		  }
	    }
	  if (bb->index >= 0
	      && any_condjump_p (bb->end)
	      && bb->succ->succ_next)
	    {
	      int prob;
	      edge e;
	      int index;

	      /* Find the branch edge.  It is possible that we do have fake
		 edges here.  */
	      for (e = bb->succ; e->flags & (EDGE_FAKE | EDGE_FALLTHRU);
		   e = e->succ_next)
		continue; /* Loop body has been intentionally left blank.  */

	      prob = e->probability;
	      index = prob * 20 / REG_BR_PROB_BASE;

	      if (index == 20)
		index = 19;
	      hist_br_prob[index]++;

	      note = find_reg_note (bb->end, REG_BR_PROB, 0);
	      /* There may be already note put by some other pass, such
		 as builtin_expect expander.  */
	      if (note)
		XEXP (note, 0) = GEN_INT (prob);
	      else
		REG_NOTES (bb->end)
		  = gen_rtx_EXPR_LIST (REG_BR_PROB, GEN_INT (prob),
				       REG_NOTES (bb->end));
	      num_branches++;
	    }
	}
      /* Otherwise distribute the probabilities evenly so we get sane
	 sum.  Use simple heuristics that if there are normal edges,
	 give all abnormals frequency of 0, otherwise distribute the
	 frequency over abnormals (this is the case of noreturn
	 calls).  */
      else
	{
	  for (e = bb->succ; e; e = e->succ_next)
	    if (!(e->flags & (EDGE_COMPLEX | EDGE_FAKE)))
	      total ++;
	  if (total)
	    {
	      for (e = bb->succ; e; e = e->succ_next)
		if (!(e->flags & (EDGE_COMPLEX | EDGE_FAKE)))
		  e->probability = REG_BR_PROB_BASE / total;
		else
		  e->probability = 0;
	    }
	  else
	    {
	      for (e = bb->succ; e; e = e->succ_next)
		total ++;
	      for (e = bb->succ; e; e = e->succ_next)
		e->probability = REG_BR_PROB_BASE / total;
	    }
	  if (bb->index >= 0
	      && any_condjump_p (bb->end)
	      && bb->succ->succ_next)
	    num_branches++, num_never_executed;
	}
    }

  if (rtl_dump_file)
    {
      fprintf (rtl_dump_file, "%d branches\n", num_branches);
      fprintf (rtl_dump_file, "%d branches never executed\n",
	       num_never_executed);
      if (num_branches)
	for (i = 0; i < 10; i++)
	  fprintf (rtl_dump_file, "%d%% branches in range %d-%d%%\n",
		   (hist_br_prob[i] + hist_br_prob[19-i]) * 100 / num_branches,
		   5 * i, 5 * i + 5);

      total_num_branches += num_branches;
      total_num_never_executed += num_never_executed;
      for (i = 0; i < 20; i++)
	total_hist_br_prob[i] += hist_br_prob[i];

      fputc ('\n', rtl_dump_file);
      fputc ('\n', rtl_dump_file);
    }

  free_aux_for_blocks ();
  if (exec_counts)
    free (exec_counts);
  find_counters_section (GCOV_TAG_ARC_COUNTS)->present = 1;
}

/* Compute checksum for the current function.  We generate a CRC32.  */

static unsigned
compute_checksum ()
{
  unsigned chksum = 0;
  basic_block bb;
  
  FOR_EACH_BB (bb)
    {
      edge e = NULL;
      
      do
	{
	  unsigned value = BB_TO_GCOV_INDEX (e ? e->dest : bb);
	  unsigned ix;

	  /* No need to use all bits in value identically, nearly all
	     functions have less than 256 blocks.  */
	  value ^= value << 16;
	  value ^= value << 8;
	  
	  for (ix = 8; ix--; value <<= 1)
	    {
	      unsigned feedback;

	      feedback = (value ^ chksum) & 0x80000000 ? 0x04c11db7 : 0;
	      chksum <<= 1;
	      chksum ^= feedback;
	    }
	  
	  e = e ? e->succ_next : bb->succ;
	}
      while (e);
    }

  return chksum;
}

/* Instrument and/or analyze program behavior based on program flow graph.
   In either case, this function builds a flow graph for the function being
   compiled.  The flow graph is stored in BB_GRAPH.

   When FLAG_PROFILE_ARCS is nonzero, this function instruments the edges in
   the flow graph that are needed to reconstruct the dynamic behavior of the
   flow graph.

   When FLAG_BRANCH_PROBABILITIES is nonzero, this function reads auxiliary
   information from a data file containing edge count information from previous
   executions of the function being compiled.  In this case, the flow graph is
   annotated with actual execution counts, which are later propagated into the
   rtl for optimization purposes.

   Main entry point of this file.  */

void
branch_prob ()
{
  basic_block bb;
  unsigned i;
  unsigned num_edges, ignored_edges;
  struct edge_list *el;
  struct loops loops;
  unsigned n_values;
  struct histogram_value *values;
  const char *name = IDENTIFIER_POINTER
		      (DECL_ASSEMBLER_NAME (current_function_decl));

  profile_info.current_function_cfg_checksum = compute_checksum ();
  for (i = 0; i < profile_info.n_sections; i++)
    {
      profile_info.section_info[i].n_counters_now = 0;
      profile_info.section_info[i].present = 0;
    }

  if (rtl_dump_file)
    fprintf (rtl_dump_file, "CFG checksum is %u\n",
	profile_info.current_function_cfg_checksum);

  total_num_times_called++;

  flow_call_edges_add (NULL);
  add_noreturn_fake_exit_edges ();

  /* We can't handle cyclic regions constructed using abnormal edges.
     To avoid these we replace every source of abnormal edge by a fake
     edge from entry node and every destination by fake edge to exit.
     This keeps graph acyclic and our calculation exact for all normal
     edges except for exit and entrance ones.

     We also add fake exit edges for each call and asm statement in the
     basic, since it may not return.  */

  FOR_EACH_BB (bb)
    {
      int need_exit_edge = 0, need_entry_edge = 0;
      int have_exit_edge = 0, have_entry_edge = 0;
      rtx insn;
      edge e;

      /* Functions returning multiple times are not handled by extra edges.
         Instead we simply allow negative counts on edges from exit to the
         block past call and corresponding probabilities.  We can't go
         with the extra edges because that would result in flowgraph that
	 needs to have fake edges outside the spanning tree.  */

      for (e = bb->succ; e; e = e->succ_next)
	{
	  if ((e->flags & (EDGE_ABNORMAL | EDGE_ABNORMAL_CALL))
	       && e->dest != EXIT_BLOCK_PTR)
	    need_exit_edge = 1;
	  if (e->dest == EXIT_BLOCK_PTR)
	    have_exit_edge = 1;
	}
      for (e = bb->pred; e; e = e->pred_next)
	{
	  if ((e->flags & (EDGE_ABNORMAL | EDGE_ABNORMAL_CALL))
	       && e->src != ENTRY_BLOCK_PTR)
	    need_entry_edge = 1;
	  if (e->src == ENTRY_BLOCK_PTR)
	    have_entry_edge = 1;
	}

      if (need_exit_edge && !have_exit_edge)
	{
	  if (rtl_dump_file)
	    fprintf (rtl_dump_file, "Adding fake exit edge to bb %i\n",
		     bb->index);
	  make_edge (bb, EXIT_BLOCK_PTR, EDGE_FAKE);
	}
      if (need_entry_edge && !have_entry_edge)
	{
	  if (rtl_dump_file)
	    fprintf (rtl_dump_file, "Adding fake entry edge to bb %i\n",
		     bb->index);
	  make_edge (ENTRY_BLOCK_PTR, bb, EDGE_FAKE);
	}
    }

  if (flag_loop_histograms)
    {
      /* Find loops and bring them into canonical shape.  */
      flow_loops_find (&loops, LOOP_TREE);
      create_preheaders (&loops, 0);
      /* Release dominators -- we aren't going to need them nor update them.  */
      if (loops.cfg.dom)
	{
	  free_dominance_info (loops.cfg.dom);
	  loops.cfg.dom = NULL;
	}
    }

  el = create_edge_list ();
  num_edges = NUM_EDGES (el);
  alloc_aux_for_edges (sizeof (struct edge_info));

  /* The basic blocks are expected to be numbered sequentially.  */
  compact_blocks ();

  ignored_edges = 0;
  for (i = 0 ; i < num_edges ; i++)
    {
      edge e = INDEX_EDGE (el, i);
      e->count = 0;

      /* Mark edges we've replaced by fake edges above as ignored.  */
      if ((e->flags & (EDGE_ABNORMAL | EDGE_ABNORMAL_CALL))
	  && e->src != ENTRY_BLOCK_PTR && e->dest != EXIT_BLOCK_PTR)
	{
	  EDGE_INFO (e)->ignore = 1;
	  ignored_edges++;
	}
    }

#ifdef ENABLE_CHECKING
  verify_flow_info ();
#endif

  /* Create spanning tree from basic block graph, mark each edge that is
     on the spanning tree.  We insert as many abnormal and critical edges
     as possible to minimize number of edge splits necessary.  */

  find_spanning_tree (el);

  /* Fake edges that are not on the tree will not be instrumented, so
     mark them ignored.  */
  for (i = 0; i < num_edges; i++)
    {
      edge e = INDEX_EDGE (el, i);
      struct edge_info *inf = EDGE_INFO (e);
      if ((e->flags & EDGE_FAKE) && !inf->ignore && !inf->on_tree)
	{
	  inf->ignore = 1;
	  ignored_edges++;
	}
    }

  total_num_blocks += n_basic_blocks + 2;
  if (rtl_dump_file)
    fprintf (rtl_dump_file, "%d basic blocks\n", n_basic_blocks);

  total_num_edges += num_edges;
  if (rtl_dump_file)
    fprintf (rtl_dump_file, "%d edges\n", num_edges);

  total_num_edges_ignored += ignored_edges;
  if (rtl_dump_file)
    fprintf (rtl_dump_file, "%d ignored edges\n", ignored_edges);

  /* Create a .bbg file from which gcov can reconstruct the basic block
     graph.  First output the number of basic blocks, and then for every
     edge output the source and target basic block numbers.
     NOTE: The format of this file must be compatible with gcov.  */

  if (flag_test_coverage && bbg_file)
    {
      long offset;
      
      /* Announce function */
      if (gcov_write_unsigned (bbg_file, GCOV_TAG_FUNCTION)
	  || !(offset = gcov_reserve_length (bbg_file))
	  || gcov_write_string (bbg_file, name,
			     strlen (name))
	  || gcov_write_unsigned (bbg_file,
			    profile_info.current_function_cfg_checksum)
	  || gcov_write_length (bbg_file, offset))
	goto bbg_error;

      /* Basic block flags */
      if (gcov_write_unsigned (bbg_file, GCOV_TAG_BLOCKS)
	  || !(offset = gcov_reserve_length (bbg_file)))
	goto bbg_error;
      for (i = 0; i != (unsigned) (n_basic_blocks + 2); i++)
	if (gcov_write_unsigned (bbg_file, 0))
	  goto bbg_error;
      if (gcov_write_length (bbg_file, offset))
	goto bbg_error;
      
      /* Arcs */
      FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, EXIT_BLOCK_PTR, next_bb)
	{
	  edge e;

	  if (gcov_write_unsigned (bbg_file, GCOV_TAG_ARCS)
	      || !(offset = gcov_reserve_length (bbg_file))
	      || gcov_write_unsigned (bbg_file, BB_TO_GCOV_INDEX (bb)))
	    goto bbg_error;

	  for (e = bb->succ; e; e = e->succ_next)
	    {
	      struct edge_info *i = EDGE_INFO (e);
	      if (!i->ignore)
		{
		  unsigned flag_bits = 0;
		  
		  if (i->on_tree)
		    flag_bits |= GCOV_ARC_ON_TREE;
		  if (e->flags & EDGE_FAKE)
		    flag_bits |= GCOV_ARC_FAKE;
		  if (e->flags & EDGE_FALLTHRU)
		    flag_bits |= GCOV_ARC_FALLTHROUGH;

		  if (gcov_write_unsigned (bbg_file,
					   BB_TO_GCOV_INDEX (e->dest))
		      || gcov_write_unsigned (bbg_file, flag_bits))
		    goto bbg_error;
	        }
	    }

	  if (gcov_write_length (bbg_file, offset))
	    goto bbg_error;
	}

      /* Output line number information about each basic block for
     	 GCOV utility.  */
      {
	char const *prev_file_name = NULL;
	
	FOR_EACH_BB (bb)
	  {
	    rtx insn = bb->head;
	    int ignore_next_note = 0;
	    
	    offset = 0;
	    
	    /* We are looking for line number notes.  Search backward
	       before basic block to find correct ones.  */
	    insn = prev_nonnote_insn (insn);
	    if (!insn)
	      insn = get_insns ();
	    else
	      insn = NEXT_INSN (insn);

	    while (insn != bb->end)
	      {
		if (GET_CODE (insn) == NOTE)
		  {
		     /* Must ignore the line number notes that immediately
		     	follow the end of an inline function to avoid counting
		     	it twice.  There is a note before the call, and one
		     	after the call.  */
		    if (NOTE_LINE_NUMBER (insn)
			== NOTE_INSN_REPEATED_LINE_NUMBER)
		      ignore_next_note = 1;
		    else if (NOTE_LINE_NUMBER (insn) <= 0)
		      /*NOP*/;
		    else if (ignore_next_note)
		      ignore_next_note = 0;
		    else
		      {
			if (offset)
			  /*NOP*/;
			else if (gcov_write_unsigned (bbg_file, GCOV_TAG_LINES)
				 || !(offset = gcov_reserve_length (bbg_file))
				 || gcov_write_unsigned (bbg_file,
						   BB_TO_GCOV_INDEX (bb)))
			  goto bbg_error;
			/* If this is a new source file, then output
			   the file's name to the .bb file.  */
			if (!prev_file_name
			    || strcmp (NOTE_SOURCE_FILE (insn),
				       prev_file_name))
			  {
			    prev_file_name = NOTE_SOURCE_FILE (insn);
			    if (gcov_write_unsigned (bbg_file, 0)
				|| gcov_write_string (bbg_file, prev_file_name,
						      strlen (prev_file_name)))
			      goto bbg_error;
			  }
			if (gcov_write_unsigned (bbg_file, NOTE_LINE_NUMBER (insn)))
			  goto bbg_error;
		      }
		  }
		insn = NEXT_INSN (insn);
	      }

	    if (offset)
	      {
		if (gcov_write_unsigned (bbg_file, 0)
		    || gcov_write_string (bbg_file, NULL, 0)
		    || gcov_write_length (bbg_file, offset))
		  {
		  bbg_error:;
		    warning ("error writing `%s'", bbg_file_name);
		    fclose (bbg_file);
		    bbg_file = NULL;
		  }
	      }
	  }
      }
    }

  if (flag_value_histograms)
    {
      find_values_to_profile (&n_values, &values);
      allocate_reg_info (max_reg_num (), FALSE, FALSE);
    }

  if (flag_branch_probabilities)
    {
      compute_branch_probabilities ();
      if (flag_loop_histograms)
	compute_loop_histograms (&loops);
      if (flag_value_histograms)
	compute_value_histograms (n_values, values);
    }

  /* For each edge not on the spanning tree, add counting code as rtl.  */

  if (cfun->arc_profile && profile_arc_flag)
    {
      struct function_list *item;
      
      instrument_edges (el);
      if (flag_loop_histograms)
	instrument_loops (&loops);
      if (flag_value_histograms)
	instrument_values (n_values, values);

      /* Commit changes done by instrumentation.  */
      commit_edge_insertions_watch_calls ();
      allocate_reg_info (max_reg_num (), FALSE, FALSE);

      /* ??? Probably should re-use the existing struct function.  */
      item = xmalloc (sizeof (struct function_list));
      
      *functions_tail = item;
      functions_tail = &item->next;
      
      item->next = 0;
      item->name = xstrdup (name);
      item->cfg_checksum = profile_info.current_function_cfg_checksum;
      item->n_counter_sections = 0;
      for (i = 0; i < profile_info.n_sections; i++)
	if (profile_info.section_info[i].n_counters_now)
	  {
	    item->counter_sections[item->n_counter_sections].tag = 
		    profile_info.section_info[i].tag;
	    item->counter_sections[item->n_counter_sections].n_counters =
		    profile_info.section_info[i].n_counters_now;
	    item->n_counter_sections++;
	  }
    }

  if (flag_loop_histograms)
    {
      /* Free the loop datastructure.  */
      flow_loops_free (&loops);
    }

  if (flag_value_histograms)
    {
      /* Free list of interesting values.  */
      free_profiled_values (n_values, values);
    }

  remove_fake_edges ();
  free_aux_for_edges ();
  /* Re-merge split basic blocks and the mess introduced by
     insert_insn_on_edge.  */
  cleanup_cfg (profile_arc_flag ? CLEANUP_EXPENSIVE : 0);
  if (rtl_dump_file)
    dump_flow_info (rtl_dump_file);

  free_edge_list (el);
}

/* Union find algorithm implementation for the basic blocks using
   aux fields.  */

static basic_block
find_group (bb)
     basic_block bb;
{
  basic_block group = bb, bb1;

  while ((basic_block) group->aux != group)
    group = (basic_block) group->aux;

  /* Compress path.  */
  while ((basic_block) bb->aux != group)
    {
      bb1 = (basic_block) bb->aux;
      bb->aux = (void *) group;
      bb = bb1;
    }
  return group;
}

static void
union_groups (bb1, bb2)
     basic_block bb1, bb2;
{
  basic_block bb1g = find_group (bb1);
  basic_block bb2g = find_group (bb2);

  /* ??? I don't have a place for the rank field.  OK.  Lets go w/o it,
     this code is unlikely going to be performance problem anyway.  */
  if (bb1g == bb2g)
    abort ();

  bb1g->aux = bb2g;
}

/* This function searches all of the edges in the program flow graph, and puts
   as many bad edges as possible onto the spanning tree.  Bad edges include
   abnormals edges, which can't be instrumented at the moment.  Since it is
   possible for fake edges to form a cycle, we will have to develop some
   better way in the future.  Also put critical edges to the tree, since they
   are more expensive to instrument.  */

static void
find_spanning_tree (el)
     struct edge_list *el;
{
  int i;
  int num_edges = NUM_EDGES (el);
  basic_block bb;

  /* We use aux field for standard union-find algorithm.  */
  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, NULL, next_bb)
    bb->aux = bb;

  /* Add fake edge exit to entry we can't instrument.  */
  union_groups (EXIT_BLOCK_PTR, ENTRY_BLOCK_PTR);

  /* First add all abnormal edges to the tree unless they form a cycle. Also
     add all edges to EXIT_BLOCK_PTR to avoid inserting profiling code behind
     setting return value from function.  */
  for (i = 0; i < num_edges; i++)
    {
      edge e = INDEX_EDGE (el, i);
      if (((e->flags & (EDGE_ABNORMAL | EDGE_ABNORMAL_CALL | EDGE_FAKE))
	   || e->dest == EXIT_BLOCK_PTR
	   )
	  && !EDGE_INFO (e)->ignore
	  && (find_group (e->src) != find_group (e->dest)))
	{
	  if (rtl_dump_file)
	    fprintf (rtl_dump_file, "Abnormal edge %d to %d put to tree\n",
		     e->src->index, e->dest->index);
	  EDGE_INFO (e)->on_tree = 1;
	  union_groups (e->src, e->dest);
	}
    }

  /* Now insert all critical edges to the tree unless they form a cycle.  */
  for (i = 0; i < num_edges; i++)
    {
      edge e = INDEX_EDGE (el, i);
      if ((EDGE_CRITICAL_P (e))
	  && !EDGE_INFO (e)->ignore
	  && (find_group (e->src) != find_group (e->dest)))
	{
	  if (rtl_dump_file)
	    fprintf (rtl_dump_file, "Critical edge %d to %d put to tree\n",
		     e->src->index, e->dest->index);
	  EDGE_INFO (e)->on_tree = 1;
	  union_groups (e->src, e->dest);
	}
    }

  /* And now the rest.  */
  for (i = 0; i < num_edges; i++)
    {
      edge e = INDEX_EDGE (el, i);
      if (find_group (e->src) != find_group (e->dest)
	  && !EDGE_INFO (e)->ignore)
	{
	  if (rtl_dump_file)
	    fprintf (rtl_dump_file, "Normal edge %d to %d put to tree\n",
		     e->src->index, e->dest->index);
	  EDGE_INFO (e)->on_tree = 1;
	  union_groups (e->src, e->dest);
	}
    }

  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, NULL, next_bb)
    bb->aux = NULL;
}

/* Perform file-level initialization for branch-prob processing.  */

void
init_branch_prob (filename)
  const char *filename;
{
  int len = strlen (filename);
  int i;

  if (flag_test_coverage)
    {
      /* Open the bbg output file.  */
      bbg_file_name = (char *) xmalloc (len + strlen (GCOV_GRAPH_SUFFIX) + 1);
      strcpy (bbg_file_name, filename);
      strcat (bbg_file_name, GCOV_GRAPH_SUFFIX);
      bbg_file = fopen (bbg_file_name, "wb");
      if (!bbg_file)
	fatal_io_error ("cannot open %s", bbg_file_name);

      if (gcov_write_unsigned (bbg_file, GCOV_GRAPH_MAGIC)
	  || gcov_write_unsigned (bbg_file, GCOV_VERSION))
	{
	  fclose (bbg_file);
	  fatal_io_error ("cannot write `%s'", bbg_file_name);
	}
    }

  da_file_name = (char *) xmalloc (len + strlen (GCOV_DATA_SUFFIX) + 1);
  strcpy (da_file_name, filename);
  strcat (da_file_name, GCOV_DATA_SUFFIX);
  
  if (flag_branch_probabilities)
    {
      da_file = fopen (da_file_name, "rb");
      if (!da_file)
	warning ("file %s not found, execution counts assumed to be zero",
		 da_file_name);
      if (counts_file_index && strcmp (da_file_name, counts_file_name))
       	cleanup_counts_index (0);
      if (index_counts_file ())
	counts_file_name = xstrdup (da_file_name);
    }

  if (profile_arc_flag)
    {
      /* Generate and save a copy of this so it can be shared.  */
      char buf[20];
      
      ASM_GENERATE_INTERNAL_LABEL (buf, "LPBX", 2);
      profiler_label = gen_rtx_SYMBOL_REF (Pmode, ggc_strdup (buf));

      ASM_GENERATE_INTERNAL_LABEL (buf, "LPBX", 3);
      loop_histograms_label = gen_rtx_SYMBOL_REF (Pmode, ggc_strdup (buf));

      ASM_GENERATE_INTERNAL_LABEL (buf, "LPBX", 4);
      value_histograms_label = gen_rtx_SYMBOL_REF (Pmode, ggc_strdup (buf));

      ASM_GENERATE_INTERNAL_LABEL (buf, "LPBX", 5);
      same_value_histograms_label = gen_rtx_SYMBOL_REF (Pmode, ggc_strdup (buf));
    }
  
  total_num_blocks = 0;
  total_num_edges = 0;
  total_num_edges_ignored = 0;
  total_num_edges_instrumented = 0;
  total_num_blocks_created = 0;
  total_num_passes = 0;
  total_num_times_called = 0;
  total_num_branches = 0;
  total_num_never_executed = 0;
  for (i = 0; i < 20; i++)
    total_hist_br_prob[i] = 0;
}

/* Performs file-level cleanup after branch-prob processing
   is completed.  */

void
end_branch_prob ()
{
  if (flag_test_coverage)
    {
      if (bbg_file)
	{
#if __GNUC__ && !CROSS_COMPILE && SUPPORTS_WEAK
	  /* If __gcov_init has a value in the compiler, it means we
	     are instrumenting ourselves. We should not remove the
	     counts file, because we might be recompiling
	     ourselves. The .da files are all removed during copying
	     the stage1 files.  */
	  extern void __gcov_init (void *)
	    __attribute__ ((weak));
	  
	  if (!__gcov_init)
	    unlink (da_file_name);
#else
	  unlink (da_file_name);
#endif
	  fclose (bbg_file);
	}
      else
	{
	  unlink (bbg_file_name);
	  unlink (da_file_name);
	}
    }

  if (da_file)
    fclose (da_file);

  if (rtl_dump_file)
    {
      fprintf (rtl_dump_file, "\n");
      fprintf (rtl_dump_file, "Total number of blocks: %d\n",
	       total_num_blocks);
      fprintf (rtl_dump_file, "Total number of edges: %d\n", total_num_edges);
      fprintf (rtl_dump_file, "Total number of ignored edges: %d\n",
	       total_num_edges_ignored);
      fprintf (rtl_dump_file, "Total number of instrumented edges: %d\n",
	       total_num_edges_instrumented);
      fprintf (rtl_dump_file, "Total number of blocks created: %d\n",
	       total_num_blocks_created);
      fprintf (rtl_dump_file, "Total number of graph solution passes: %d\n",
	       total_num_passes);
      if (total_num_times_called != 0)
	fprintf (rtl_dump_file, "Average number of graph solution passes: %d\n",
		 (total_num_passes + (total_num_times_called  >> 1))
		 / total_num_times_called);
      fprintf (rtl_dump_file, "Total number of branches: %d\n",
	       total_num_branches);
      fprintf (rtl_dump_file, "Total number of branches never executed: %d\n",
	       total_num_never_executed);
      if (total_num_branches)
	{
	  int i;

	  for (i = 0; i < 10; i++)
	    fprintf (rtl_dump_file, "%d%% branches in range %d-%d%%\n",
		     (total_hist_br_prob[i] + total_hist_br_prob[19-i]) * 100
		     / total_num_branches, 5*i, 5*i+5);
	}
    }
}

/* Find (and create if not present) a section with TAG.  */
struct section_info *
find_counters_section (tag)
     unsigned tag;
{
  unsigned i;

  for (i = 0; i < profile_info.n_sections; i++)
    if (profile_info.section_info[i].tag == tag)
      return profile_info.section_info + i;

  if (i == MAX_COUNTER_SECTIONS)
    abort ();

  profile_info.section_info[i].tag = tag;
  profile_info.section_info[i].present = 0;
  profile_info.section_info[i].n_counters = 0;
  profile_info.section_info[i].n_counters_now = 0;
  profile_info.n_sections++;

  return profile_info.section_info + i;
}

/* Set FIELDS as purpose to VALUE.  */
static void
set_purpose (value, fields)
     tree value;
     tree fields;
{
  tree act_field, act_value;
  
  for (act_field = fields, act_value = value;
       act_field;
       act_field = TREE_CHAIN (act_field), act_value = TREE_CHAIN (act_value))
    TREE_PURPOSE (act_value) = act_field;
}

/* Returns label for base of counters inside TAG section.  */
static rtx
label_for_tag (tag)
     unsigned tag;
{
  switch (tag)
    {
    case GCOV_TAG_ARC_COUNTS:
      return profiler_label;
    case GCOV_TAG_LOOP_HISTOGRAMS:
      return loop_histograms_label;
    case GCOV_TAG_VALUE_HISTOGRAMS:
      return value_histograms_label;
    case GCOV_TAG_SAME_VALUE_HISTOGRAMS:
      return same_value_histograms_label;
    default:
      abort ();
    }
}

/* Creates fields of struct counter_section (in gcov-io.h).  */
static tree
build_counter_section_fields ()
{
  tree field, fields;

  /* tag */
  fields = build_decl (FIELD_DECL, NULL_TREE, unsigned_type_node);

  /* n_counters */
  field = build_decl (FIELD_DECL, NULL_TREE, unsigned_type_node);
  TREE_CHAIN (field) = fields;
  fields = field;

  return fields;
}

/* Creates value of struct counter_section (in gcov-io.h).  */
static tree
build_counter_section_value (tag, n_counters)
     unsigned tag;
     unsigned n_counters;
{
  tree value = NULL_TREE;

  /* tag */
  value = tree_cons (NULL_TREE,
		     convert (unsigned_type_node,
			      build_int_2 (tag, 0)),
		     value);
  
  /* n_counters */
  value = tree_cons (NULL_TREE,
		     convert (unsigned_type_node,
			      build_int_2 (n_counters, 0)),
		     value);

  return value;
}

/* Creates fields of struct counter_section_data (in gcov-io.h).  */
static tree
build_counter_section_data_fields ()
{
  tree field, fields, gcov_type, gcov_ptr_type;

  gcov_type = make_signed_type (GCOV_TYPE_SIZE);
  gcov_ptr_type =
	  build_pointer_type (build_qualified_type (gcov_type,
						    TYPE_QUAL_CONST));

  /* tag */
  fields = build_decl (FIELD_DECL, NULL_TREE, unsigned_type_node);

  /* n_counters */
  field = build_decl (FIELD_DECL, NULL_TREE, unsigned_type_node);
  TREE_CHAIN (field) = fields;
  fields = field;

  /* counters */
  field = build_decl (FIELD_DECL, NULL_TREE, gcov_ptr_type);
  TREE_CHAIN (field) = fields;
  fields = field;

  return fields;
}

/* Creates value of struct counter_section_data (in gcov-io.h).  */
static tree
build_counter_section_data_value (tag, n_counters)
     unsigned tag;
     unsigned n_counters;
{
  tree value = NULL_TREE, counts_table, gcov_type, gcov_ptr_type;

  gcov_type = make_signed_type (GCOV_TYPE_SIZE);
  gcov_ptr_type
    = build_pointer_type (build_qualified_type
			  (gcov_type, TYPE_QUAL_CONST));

  /* tag */
  value = tree_cons (NULL_TREE,
		     convert (unsigned_type_node,
			      build_int_2 (tag, 0)),
		     value);
  
  /* n_counters */
  value = tree_cons (NULL_TREE,
		     convert (unsigned_type_node,
			      build_int_2 (n_counters, 0)),
		     value);

  /* counters */
  if (n_counters)
    {
      tree gcov_type_array_type =
	      build_array_type (gcov_type,
				build_index_type (build_int_2 (n_counters - 1,
							       0)));
      counts_table =
	      build (VAR_DECL, gcov_type_array_type, NULL_TREE, NULL_TREE);
      TREE_STATIC (counts_table) = 1;
      DECL_NAME (counts_table) = get_identifier (XSTR (label_for_tag (tag), 0));
      assemble_variable (counts_table, 0, 0, 0);
      counts_table = build1 (ADDR_EXPR, gcov_ptr_type, counts_table);
    }
  else
    counts_table = null_pointer_node;

  value = tree_cons (NULL_TREE, counts_table, value);

  return value;
}

/* Creates fields for struct function_info type (in gcov-io.h).  */
static tree
build_function_info_fields ()
{
  tree field, fields, counter_section_fields, counter_section_type;
  tree counter_sections_ptr_type;
  tree string_type =
	  build_pointer_type (build_qualified_type (char_type_node,
						    TYPE_QUAL_CONST));
  /* name */
  fields = build_decl (FIELD_DECL, NULL_TREE, string_type);

  /* checksum */
  field = build_decl (FIELD_DECL, NULL_TREE, unsigned_type_node);
  TREE_CHAIN (field) = fields;
  fields = field;

  /* n_counter_sections */
  field = build_decl (FIELD_DECL, NULL_TREE, unsigned_type_node);
  TREE_CHAIN (field) = fields;
  fields = field;

  /* counter_sections */
  counter_section_fields = build_counter_section_fields ();
  counter_section_type = (*lang_hooks.types.make_type) (RECORD_TYPE);
  finish_builtin_struct (counter_section_type, "__counter_section",
			 counter_section_fields, NULL_TREE);
  counter_sections_ptr_type =
	  build_pointer_type
	  	(build_qualified_type (counter_section_type,
				       TYPE_QUAL_CONST));
  field = build_decl (FIELD_DECL, NULL_TREE, counter_sections_ptr_type);
  TREE_CHAIN (field) = fields;
  fields = field;

  return fields;
}

/* Creates value for struct function_info (in gcov-io.h).  */
static tree
build_function_info_value (function)
     struct function_list *function;
{
  tree value = NULL_TREE;
  size_t name_len = strlen (function->name);
  tree fname = build_string (name_len + 1, function->name);
  tree string_type =
	  build_pointer_type (build_qualified_type (char_type_node,
						    TYPE_QUAL_CONST));
  tree counter_section_fields, counter_section_type, counter_sections_value;
  tree counter_sections_ptr_type, counter_sections_array_type;
  unsigned i;

  /* name */
  TREE_TYPE (fname) =
	  build_array_type (char_type_node,
			    build_index_type (build_int_2 (name_len, 0)));
  value = tree_cons (NULL_TREE,
		     build1 (ADDR_EXPR,
			     string_type,
			     fname),
		     value);

  /* checksum */
  value = tree_cons (NULL_TREE,
		     convert (unsigned_type_node,
			      build_int_2 (function->cfg_checksum, 0)),
		     value);

  /* n_counter_sections */

  value = tree_cons (NULL_TREE,
		     convert (unsigned_type_node,
			      build_int_2 (function->n_counter_sections, 0)),
	    	    value);

  /* counter_sections */
  counter_section_fields = build_counter_section_fields ();
  counter_section_type = (*lang_hooks.types.make_type) (RECORD_TYPE);
  counter_sections_ptr_type =
	  build_pointer_type
	  	(build_qualified_type (counter_section_type,
				       TYPE_QUAL_CONST));
  counter_sections_array_type =
	  build_array_type (counter_section_type,
			    build_index_type (
      				build_int_2 (function->n_counter_sections - 1,
		  			     0)));

  counter_sections_value = NULL_TREE;
  for (i = 0; i < function->n_counter_sections; i++)
    {
      tree counter_section_value =
	      build_counter_section_value (function->counter_sections[i].tag,
					   function->counter_sections[i].n_counters);
      set_purpose (counter_section_value, counter_section_fields);
      counter_sections_value = tree_cons (NULL_TREE,
					  build (CONSTRUCTOR,
						 counter_section_type,
						 NULL_TREE,
						 nreverse (counter_section_value)),
					  counter_sections_value);
    }
  finish_builtin_struct (counter_section_type, "__counter_section",
			 counter_section_fields, NULL_TREE);

  if (function->n_counter_sections)
    {
      counter_sections_value = 
	      build (CONSTRUCTOR,
 		     counter_sections_array_type,
		     NULL_TREE,
		     nreverse (counter_sections_value)),
      counter_sections_value = build1 (ADDR_EXPR,
				       counter_sections_ptr_type,
				       counter_sections_value);
    }
  else
    counter_sections_value = null_pointer_node;

  value = tree_cons (NULL_TREE, counter_sections_value, value);

  return value;
}

/* Creates fields of struct gcov_info type (in gcov-io.h).  */
static tree
build_gcov_info_fields (gcov_info_type)
     tree gcov_info_type;
{
  tree field, fields;
  char *filename;
  int filename_len;
  tree string_type =
	  build_pointer_type (build_qualified_type (char_type_node,
						    TYPE_QUAL_CONST));
  tree function_info_fields, function_info_type, function_info_ptr_type;
  tree counter_section_data_fields, counter_section_data_type;
  tree counter_section_data_ptr_type;

  /* Version ident */
  fields = build_decl (FIELD_DECL, NULL_TREE, long_unsigned_type_node);

  /* next -- NULL */
  field = build_decl (FIELD_DECL, NULL_TREE,
		      build_pointer_type (build_qualified_type (gcov_info_type,
								TYPE_QUAL_CONST)));
  TREE_CHAIN (field) = fields;
  fields = field;
  
  /* Filename */
  filename = getpwd ();
  filename = (filename && da_file_name[0] != '/'
	      ? concat (filename, "/", da_file_name, NULL)
	      : da_file_name);
  filename_len = strlen (filename);
  if (filename != da_file_name)
    free (filename);

  field = build_decl (FIELD_DECL, NULL_TREE, string_type);
  TREE_CHAIN (field) = fields;
  fields = field;
  
  /* Workspace */
  field = build_decl (FIELD_DECL, NULL_TREE, long_integer_type_node);
  TREE_CHAIN (field) = fields;
  fields = field;

  /* number of functions */
  field = build_decl (FIELD_DECL, NULL_TREE, unsigned_type_node);
  TREE_CHAIN (field) = fields;
  fields = field;
      
  /* function_info table */
  function_info_fields = build_function_info_fields ();
  function_info_type = (*lang_hooks.types.make_type) (RECORD_TYPE);
  finish_builtin_struct (function_info_type, "__function_info",
			 function_info_fields, NULL_TREE);
  function_info_ptr_type =
	  build_pointer_type
	  	(build_qualified_type (function_info_type,
				       TYPE_QUAL_CONST));
  field = build_decl (FIELD_DECL, NULL_TREE, function_info_ptr_type);
  TREE_CHAIN (field) = fields;
  fields = field;
    
  /* n_counter_sections  */
  field = build_decl (FIELD_DECL, NULL_TREE, unsigned_type_node);
  TREE_CHAIN (field) = fields;
  fields = field;
  
  /* counter sections */
  counter_section_data_fields = build_counter_section_data_fields ();
  counter_section_data_type = (*lang_hooks.types.make_type) (RECORD_TYPE);
  finish_builtin_struct (counter_section_data_type, "__counter_section_data",
			 counter_section_data_fields, NULL_TREE);
  counter_section_data_ptr_type =
	  build_pointer_type
	  	(build_qualified_type (counter_section_data_type,
				       TYPE_QUAL_CONST));
  field = build_decl (FIELD_DECL, NULL_TREE, counter_section_data_ptr_type);
  TREE_CHAIN (field) = fields;
  fields = field;

  return fields;
}

/* Creates struct gcov_info value (in gcov-io.h).  */
static tree
build_gcov_info_value ()
{
  tree value = NULL_TREE;
  tree filename_string;
  char *filename;
  int filename_len;
  unsigned n_functions, i;
  struct function_list *item;
  tree string_type =
	  build_pointer_type (build_qualified_type (char_type_node,
						    TYPE_QUAL_CONST));
  tree function_info_fields, function_info_type, function_info_ptr_type;
  tree functions;
  tree counter_section_data_fields, counter_section_data_type;
  tree counter_section_data_ptr_type, counter_sections;

  /* Version ident */
  value = tree_cons (NULL_TREE,
		     convert (long_unsigned_type_node,
			      build_int_2 (GCOV_VERSION, 0)),
		     value);

  /* next -- NULL */
  value = tree_cons (NULL_TREE, null_pointer_node, value);
  
  /* Filename */
  filename = getpwd ();
  filename = (filename && da_file_name[0] != '/'
	      ? concat (filename, "/", da_file_name, NULL)
	      : da_file_name);
  filename_len = strlen (filename);
  filename_string = build_string (filename_len + 1, filename);
  if (filename != da_file_name)
    free (filename);
  TREE_TYPE (filename_string) =
	  build_array_type (char_type_node,
			    build_index_type (build_int_2 (filename_len, 0)));
  value = tree_cons (NULL_TREE,
		     build1 (ADDR_EXPR,
			     string_type,
		       	     filename_string),
		     value);
  
  /* Workspace */
  value = tree_cons (NULL_TREE,
		     convert (long_integer_type_node, integer_zero_node),
		     value);
      
  /* number of functions */
  n_functions = 0;
  for (item = functions_head; item != 0; item = item->next, n_functions++)
    continue;
  value = tree_cons (NULL_TREE,
		     convert (unsigned_type_node,
			      build_int_2 (n_functions, 0)),
		     value);

  /* function_info table */
  function_info_fields = build_function_info_fields ();
  function_info_type = (*lang_hooks.types.make_type) (RECORD_TYPE);
  function_info_ptr_type =
	  build_pointer_type (
		build_qualified_type (function_info_type,
	       			      TYPE_QUAL_CONST));
  functions = NULL_TREE;
  for (item = functions_head; item != 0; item = item->next)
    {
      tree function_info_value = build_function_info_value (item);
      set_purpose (function_info_value, function_info_fields);
      functions = tree_cons (NULL_TREE,
    			     build (CONSTRUCTOR,
			    	    function_info_type,
				    NULL_TREE,
				    nreverse (function_info_value)),
			     functions);
    }
  finish_builtin_struct (function_info_type, "__function_info",
			 function_info_fields, NULL_TREE);

  /* Create constructor for array.  */
  if (n_functions)
    {
      tree array_type;

      array_type = build_array_type (
			function_info_type,
   			build_index_type (build_int_2 (n_functions - 1, 0)));
      functions = build (CONSTRUCTOR,
      			 array_type,
			 NULL_TREE,
			 nreverse (functions));
      functions = build1 (ADDR_EXPR,
			  function_info_ptr_type,
			  functions);
    }
  else
    functions = null_pointer_node;

  value = tree_cons (NULL_TREE, functions, value);

  /* n_counter_sections  */
  value = tree_cons (NULL_TREE,
		     convert (unsigned_type_node,
			      build_int_2 (profile_info.n_sections, 0)),
		     value);
  
  /* counter sections */
  counter_section_data_fields = build_counter_section_data_fields ();
  counter_section_data_type = (*lang_hooks.types.make_type) (RECORD_TYPE);
  counter_sections = NULL_TREE;
  for (i = 0; i < profile_info.n_sections; i++)
    {
      tree counter_sections_value =
	      build_counter_section_data_value (
		profile_info.section_info[i].tag,
		profile_info.section_info[i].n_counters);
      set_purpose (counter_sections_value, counter_section_data_fields);
      counter_sections = tree_cons (NULL_TREE,
		       		    build (CONSTRUCTOR,
		       			   counter_section_data_type,
		       			   NULL_TREE,
		       			   nreverse (counter_sections_value)),
		       		    counter_sections);
    }
  finish_builtin_struct (counter_section_data_type, "__counter_section_data",
			 counter_section_data_fields, NULL_TREE);
  counter_section_data_ptr_type =
	  build_pointer_type
	  	(build_qualified_type (counter_section_data_type,
				       TYPE_QUAL_CONST));

  if (profile_info.n_sections)
    {
      counter_sections =
    	      build (CONSTRUCTOR,
    		     build_array_type (
	       			       counter_section_data_type,
		       		       build_index_type (build_int_2 (profile_info.n_sections - 1, 0))),
		     NULL_TREE,
		     nreverse (counter_sections));
      counter_sections = build1 (ADDR_EXPR,
				 counter_section_data_ptr_type,
				 counter_sections);
    }
  else
    counter_sections = null_pointer_node;
  value = tree_cons (NULL_TREE, counter_sections, value);

  return value;
}

/* Write out the structure which libgcc uses to locate all the arc
   counters.  The structures used here must match those defined in
   gcov-io.h.  Write out the constructor to call __gcov_init.  */

void
create_profiler ()
{
  tree gcov_info_fields, gcov_info_type, gcov_info_value, gcov_info;
  char name[20];
  char *ctor_name;
  tree ctor;
  rtx gcov_info_address;
  int save_flag_inline_functions = flag_inline_functions;
  unsigned i;

  for (i = 0; i < profile_info.n_sections; i++)
    if (profile_info.section_info[i].n_counters_now)
      break;
  if (i == profile_info.n_sections)
    return;
  
  gcov_info_type = (*lang_hooks.types.make_type) (RECORD_TYPE);
  gcov_info_fields = build_gcov_info_fields (gcov_info_type);
  gcov_info_value = build_gcov_info_value ();
  set_purpose (gcov_info_value, gcov_info_fields);
  finish_builtin_struct (gcov_info_type, "__gcov_info",
			 gcov_info_fields, NULL_TREE);

  gcov_info = build (VAR_DECL, gcov_info_type, NULL_TREE, NULL_TREE);
  DECL_INITIAL (gcov_info) =
	  build (CONSTRUCTOR, gcov_info_type, NULL_TREE,
		 nreverse (gcov_info_value));

  TREE_STATIC (gcov_info) = 1;
  ASM_GENERATE_INTERNAL_LABEL (name, "LPBX", 0);
  DECL_NAME (gcov_info) = get_identifier (name);
  
  /* Build structure.  */
  assemble_variable (gcov_info, 0, 0, 0);

  /* Build the constructor function to invoke __gcov_init. */
  ctor_name = concat (IDENTIFIER_POINTER (get_file_function_name ('I')),
		      "_GCOV", NULL);
  ctor = build_decl (FUNCTION_DECL, get_identifier (ctor_name),
		     build_function_type (void_type_node, NULL_TREE));
  free (ctor_name);
  DECL_EXTERNAL (ctor) = 0;

  /* It can be a static function as long as collect2 does not have
     to scan the object file to find its ctor/dtor routine.  */
  TREE_PUBLIC (ctor) = ! targetm.have_ctors_dtors;
  TREE_USED (ctor) = 1;
  DECL_RESULT (ctor) = build_decl (RESULT_DECL, NULL_TREE, void_type_node);

  ctor = (*lang_hooks.decls.pushdecl) (ctor);
  rest_of_decl_compilation (ctor, 0, 1, 0);
  announce_function (ctor);
  current_function_decl = ctor;
  DECL_INITIAL (ctor) = error_mark_node;
  make_decl_rtl (ctor, NULL);
  init_function_start (ctor, input_filename, lineno);
  (*lang_hooks.decls.pushlevel) (0);
  expand_function_start (ctor, 0);
  cfun->arc_profile = 0;

  /* Actually generate the code to call __gcov_init.  */
  gcov_info_address = force_reg (Pmode,
				 gen_rtx_SYMBOL_REF (
					Pmode,
					IDENTIFIER_POINTER (
						DECL_NAME (gcov_info))));
  emit_library_call (gen_rtx_SYMBOL_REF (Pmode, "__gcov_init"),
		     LCT_NORMAL, VOIDmode, 1,
		     gcov_info_address, Pmode);

  expand_function_end (input_filename, lineno, 0);
  (*lang_hooks.decls.poplevel) (1, 0, 1);

  /* Since ctor isn't in the list of globals, it would never be emitted
     when it's considered to be 'safe' for inlining, so turn off
     flag_inline_functions.  */
  flag_inline_functions = 0;

  rest_of_compilation (ctor);

  /* Reset flag_inline_functions to its original value.  */
  flag_inline_functions = save_flag_inline_functions;

  if (! quiet_flag)
    fflush (asm_out_file);
  current_function_decl = NULL_TREE;

  if (targetm.have_ctors_dtors)
    (* targetm.asm_out.constructor) (XEXP (DECL_RTL (ctor), 0),
				     DEFAULT_INIT_PRIORITY);
}

/* Output instructions as RTL to increment the edge execution count.  */

static rtx
gen_edge_profiler (edgeno)
     int edgeno;
{
  enum machine_mode mode = mode_for_size (GCOV_TYPE_SIZE, MODE_INT, 0);
  rtx mem_ref, tmp;
  rtx sequence;

  start_sequence ();

  tmp = force_reg (Pmode, profiler_label);
  tmp = plus_constant (tmp, GCOV_TYPE_SIZE / BITS_PER_UNIT * edgeno);
  mem_ref = validize_mem (gen_rtx_MEM (mode, tmp));

  set_mem_alias_set (mem_ref, new_alias_set ());

  tmp = expand_simple_binop (mode, PLUS, mem_ref, const1_rtx,
			     mem_ref, 0, OPTAB_WIDEN);

  if (tmp != mem_ref)
    emit_move_insn (copy_rtx (mem_ref), tmp);

  sequence = get_insns ();
  end_sequence ();
  return sequence;
}

/* Output instructions as RTL to increment the interval histogram counter.
   VALUE is the expression whose value is profiled.  BASE_LABEL is the base
   of histogram counters, BASE is offset from this position.  */

static rtx
gen_interval_profiler (value, base_label, base)
     struct histogram_value *value;
     rtx base_label;
     int base;
{
  enum machine_mode mode = mode_for_size (GCOV_TYPE_SIZE, MODE_INT, 0);
  rtx mem_ref, tmp, tmp1, mr, val;
  rtx sequence;
  rtx more_label = gen_label_rtx ();
  rtx less_label = gen_label_rtx ();
  rtx end_of_code_label = gen_label_rtx ();
  int per_counter = GCOV_TYPE_SIZE / BITS_PER_UNIT;

  start_sequence ();

  if (value->seq)
    emit_insn (value->seq);

  mr = gen_reg_rtx (Pmode);

  tmp = force_reg (Pmode, base_label);
  tmp = plus_constant (tmp, per_counter * base);

  val = expand_simple_binop (value->mode, MINUS,
			     copy_rtx (value->value),
			     GEN_INT (value->hdata.intvl.int_start),
			     NULL_RTX, 0, OPTAB_WIDEN);

  if (value->hdata.intvl.may_be_more)
    do_compare_rtx_and_jump (copy_rtx (val), GEN_INT (value->hdata.intvl.steps),
			     GE, 0, value->mode, NULL_RTX, NULL_RTX, more_label);
  if (value->hdata.intvl.may_be_less)
    do_compare_rtx_and_jump (copy_rtx (val), const0_rtx, LT, 0, value->mode,
			     NULL_RTX, NULL_RTX, less_label);

  /* We are in range.  */
  tmp1 = expand_simple_binop (value->mode, MULT, copy_rtx (val), GEN_INT (per_counter),
			      NULL_RTX, 0, OPTAB_WIDEN);
  tmp1 = expand_simple_binop (Pmode, PLUS, copy_rtx (tmp), tmp1, mr, 0, OPTAB_WIDEN);
  if (tmp1 != mr)
    emit_move_insn (copy_rtx (mr), tmp1);

  if (value->hdata.intvl.may_be_more
      || value->hdata.intvl.may_be_less)
    {
      emit_jump_insn (gen_jump (end_of_code_label));
      emit_barrier ();
    }

  /* Above the interval.  */
  if (value->hdata.intvl.may_be_more)
    {
      emit_label (more_label);
      tmp1 = expand_simple_binop (Pmode, PLUS, copy_rtx (tmp),
				  GEN_INT (per_counter * value->hdata.intvl.steps),
    				  mr, 0, OPTAB_WIDEN);
      if (tmp1 != mr)
	emit_move_insn (copy_rtx (mr), tmp1);
      if (value->hdata.intvl.may_be_less)
	{
	  emit_jump_insn (gen_jump (end_of_code_label));
	  emit_barrier ();
	}
    }

  /* Below the interval.  */
  if (value->hdata.intvl.may_be_less)
    {
      emit_label (less_label);
      tmp1 = expand_simple_binop (Pmode, PLUS, copy_rtx (tmp),
		GEN_INT (per_counter * (value->hdata.intvl.steps
					+ (value->hdata.intvl.may_be_more ? 1 : 0))),
		mr, 0, OPTAB_WIDEN);
      if (tmp1 != mr)
	emit_move_insn (copy_rtx (mr), tmp1);
    }

  if (value->hdata.intvl.may_be_more
      || value->hdata.intvl.may_be_less)
    emit_label (end_of_code_label);

  mem_ref = validize_mem (gen_rtx_MEM (mode, mr));

  tmp = expand_simple_binop (mode, PLUS, copy_rtx (mem_ref), const1_rtx,
			     mem_ref, 0, OPTAB_WIDEN);

  if (tmp != mem_ref)
    emit_move_insn (copy_rtx (mem_ref), tmp);

  sequence = get_insns ();
  end_sequence ();
  rebuild_jump_labels (sequence);
  return sequence;
}

/* Output instructions as RTL to increment the range histogram counter.
   VALUE is the expression whose value is profiled.  BASE_LABEL is the base
   of histogram counters, BASE is offset from this position.  */

static rtx
gen_range_profiler (value, base_label, base)
     struct histogram_value *value;
     rtx base_label;
     int base;
{
  enum machine_mode mode = mode_for_size (GCOV_TYPE_SIZE, MODE_INT, 0);
  rtx mem_ref, tmp, mr, uval;
  rtx sequence;
  rtx end_of_code_label = gen_label_rtx ();
  int per_counter = GCOV_TYPE_SIZE / BITS_PER_UNIT, i;

  start_sequence ();

  if (value->seq)
    emit_insn (value->seq);

  mr = gen_reg_rtx (Pmode);

  tmp = force_reg (Pmode, base_label);
  tmp = plus_constant (tmp, per_counter * base);
  emit_move_insn (mr, tmp);

  if (REG_P (value->value))
    {
      uval = value->value;
    }
  else
    {
      uval = gen_reg_rtx (value->mode);
      emit_move_insn (uval, copy_rtx (value->value));
    }

  for (i = 0; i < value->hdata.range.n_ranges; i++)
    {
      do_compare_rtx_and_jump (copy_rtx (uval), GEN_INT (value->hdata.range.ranges[i]),
			       LT, 0, value->mode, NULL_RTX,
    			       NULL_RTX, end_of_code_label);
      tmp = expand_simple_binop (Pmode, PLUS, copy_rtx (mr),
				 GEN_INT (per_counter), mr, 0, OPTAB_WIDEN);
      if (tmp != mr)
	emit_move_insn (copy_rtx (mr), tmp);
    }

  emit_label (end_of_code_label);

  mem_ref = validize_mem (gen_rtx_MEM (mode, mr));

  tmp = expand_simple_binop (mode, PLUS, mem_ref, const1_rtx,
			     mem_ref, 0, OPTAB_WIDEN);

  if (tmp != mem_ref)
    emit_move_insn (copy_rtx (mem_ref), tmp);

  sequence = get_insns ();
  end_sequence ();
  rebuild_jump_labels (sequence);
  return sequence;
}

/* Output instructions as RTL to increment the power of two histogram counter.
   VALUE is the expression whose value is profiled.  BASE_LABEL is the base
   of histogram counters, BASE is offset from this position.  */

static rtx
gen_pow2_profiler (value, base_label, base)
     struct histogram_value *value;
     rtx base_label;
     int base;
{
  enum machine_mode mode = mode_for_size (GCOV_TYPE_SIZE, MODE_INT, 0);
  rtx mem_ref, tmp, mr, uval;
  rtx sequence;
  rtx end_of_code_label = gen_label_rtx ();
  rtx loop_label = gen_label_rtx ();
  int per_counter = GCOV_TYPE_SIZE / BITS_PER_UNIT;

  start_sequence ();

  if (value->seq)
    emit_insn (value->seq);

  mr = gen_reg_rtx (Pmode);
  tmp = force_reg (Pmode, base_label);
  tmp = plus_constant (tmp, per_counter * base);
  emit_move_insn (mr, tmp);

  uval = gen_reg_rtx (value->mode);
  emit_move_insn (uval, copy_rtx (value->value));

  /* Check for non-power of 2.  */
  if (value->hdata.pow2.may_be_other)
    {
      do_compare_rtx_and_jump (copy_rtx (uval), const0_rtx, LE, 0, value->mode,
			       NULL_RTX, NULL_RTX, end_of_code_label);
      tmp = expand_simple_binop (value->mode, PLUS, copy_rtx (uval),
				 constm1_rtx, NULL_RTX, 0, OPTAB_WIDEN);
      tmp = expand_simple_binop (value->mode, AND, copy_rtx (uval), tmp,
				 NULL_RTX, 0, OPTAB_WIDEN);
      do_compare_rtx_and_jump (tmp, const0_rtx, NE, 0, value->mode, NULL_RTX,
    			       NULL_RTX, end_of_code_label);
    }

  /* Count log_2(value).  */
  emit_label (loop_label);

  tmp = expand_simple_binop (Pmode, PLUS, copy_rtx (mr), GEN_INT (per_counter), mr, 0, OPTAB_WIDEN);
  if (tmp != mr)
    emit_move_insn (copy_rtx (mr), tmp);

  tmp = expand_simple_binop (value->mode, ASHIFTRT, copy_rtx (uval), const1_rtx,
			     uval, 0, OPTAB_WIDEN);
  if (tmp != uval)
    emit_move_insn (copy_rtx (uval), tmp);

  do_compare_rtx_and_jump (copy_rtx (uval), const0_rtx, NE, 0, value->mode,
			   NULL_RTX, NULL_RTX, loop_label);

  /* Increase the counter.  */
  emit_label (end_of_code_label);

  mem_ref = validize_mem (gen_rtx_MEM (mode, mr));

  tmp = expand_simple_binop (mode, PLUS, copy_rtx (mem_ref), const1_rtx,
			     mem_ref, 0, OPTAB_WIDEN);

  if (tmp != mem_ref)
    emit_move_insn (copy_rtx (mem_ref), tmp);

  sequence = get_insns ();
  end_sequence ();
  rebuild_jump_labels (sequence);
  return sequence;
}

/* Output instructions as RTL for code to find the most common value.
   VALUE is the expression whose value is profiled.  BASE_LABEL is the base
   of histogram counters, BASE is offset from this position.  */

static rtx
gen_one_value_profiler (value, base_label, base)
     struct histogram_value *value;
     rtx base_label;
     int base;
{
  enum machine_mode mode = mode_for_size (GCOV_TYPE_SIZE, MODE_INT, 0);
  rtx stored_value_ref, counter_ref, all_ref, stored_value, counter, all, tmp, uval;
  rtx sequence;
  rtx same_label = gen_label_rtx ();
  rtx zero_label = gen_label_rtx ();
  rtx end_of_code_label = gen_label_rtx ();
  int per_counter = GCOV_TYPE_SIZE / BITS_PER_UNIT;

  start_sequence ();

  if (value->seq)
    emit_insn (value->seq);

  tmp = force_reg (Pmode, base_label);
  stored_value = plus_constant (tmp, per_counter * base);
  counter = plus_constant (stored_value, per_counter);
  all = plus_constant (counter, per_counter);
  stored_value_ref = validize_mem (gen_rtx_MEM (mode, stored_value));
  counter_ref = validize_mem (gen_rtx_MEM (mode, counter));
  all_ref = validize_mem (gen_rtx_MEM (mode, all));

  uval = gen_reg_rtx (mode);
  convert_move (uval, copy_rtx (value->value), 0);

  /* Check if the stored value matches.  */
  do_compare_rtx_and_jump (copy_rtx (uval), copy_rtx (stored_value_ref), EQ,
			   0, mode, NULL_RTX, NULL_RTX, same_label);
  
  /* Does not match; check whether the counter is zero.  */
  do_compare_rtx_and_jump (copy_rtx (counter_ref), const0_rtx, EQ, 0, mode,
			   NULL_RTX, NULL_RTX, zero_label);

  /* The counter is not zero yet.  */
  tmp = expand_simple_binop (mode, PLUS, copy_rtx (counter_ref), constm1_rtx,
			     counter_ref, 0, OPTAB_WIDEN);

  if (tmp != counter_ref)
    emit_move_insn (copy_rtx (counter_ref), tmp);

  emit_jump_insn (gen_jump (end_of_code_label));
  emit_barrier ();
 
  emit_label (zero_label);
  /* Set new value.  */
  emit_move_insn (copy_rtx (stored_value_ref), copy_rtx (uval));

  emit_label (same_label);
  /* Increase the counter.  */
  tmp = expand_simple_binop (mode, PLUS, copy_rtx (counter_ref), const1_rtx,
			     counter_ref, 0, OPTAB_WIDEN);

  if (tmp != counter_ref)
    emit_move_insn (copy_rtx (counter_ref), tmp);
  
  emit_label (end_of_code_label);

  /* Increase the counter of all executions; this seems redundant given
     that ve have counts for edges in cfg, but it may happen that some
     optimization will change the counts for the block (either because
     it is unable to update them correctly, or because it will duplicate
     the block or its part).  */
  tmp = expand_simple_binop (mode, PLUS, copy_rtx (all_ref), const1_rtx,
			     all_ref, 0, OPTAB_WIDEN);

  if (tmp != all_ref)
    emit_move_insn (copy_rtx (all_ref), tmp);
  sequence = get_insns ();
  end_sequence ();
  rebuild_jump_labels (sequence);
  return sequence;
}
#include "gt-profile.h"
