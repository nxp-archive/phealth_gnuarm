/* Graph coloring register allocator
   Copyright (C) 2001, 2002 Free Software Foundation, Inc.
   Contributed by Michael Matz <matzmich@cs.tu-berlin.de>
   and Daniel Berlin <dan@cgsoftware.com>

   This file is part of GNU CC.

   GNU CC is free software; you can redistribute it and/or modify it under the
   terms of the GNU General Public License as published by the Free Software
   Foundation; either version 2, or (at your option) any later version.

   GNU CC is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
   FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
   details.

   You should have received a copy of the GNU General Public License along
   with GNU CC; see the file COPYING.  If not, write to the Free Software
   Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

#include "config.h"
#include "system.h"
#include "rtl.h"
#include "tm_p.h"
#include "insn-config.h"
#include "recog.h"
#include "function.h"
#include "regs.h"
#include "obstack.h"
#include "hard-reg-set.h"
#include "basic-block.h"
#include "df.h"
#include "sbitmap.h"
#include "expr.h"
#include "output.h"
#include "toplev.h"
#include "flags.h"
#include "ggc.h"
#include "reload.h"
#include "real.h"
#include "pre-reload.h"
#include "integrate.h"
#include "ra.h"


#define NO_REMAT
#define obstack_chunk_alloc xmalloc
#define obstack_chunk_free free

/* The algorithm used is currently Iterated Register Coalescing by
   L.A.George, and Appel. XXX not true anymore
*/

/* TODO
 
   * Lattice based rematerialization
   * do lots of commenting
   * look through all XXX's and do something about them
   * handle REG_NO_CONFLICTS blocks correctly (the current ad hoc approach
     might miss some conflicts due to insns which only seem to be in a 
     REG_NO_CONLICTS block)
     -- Don't necessary anymore, I believe, because SUBREG tracking is
     implemented.
   * create definitions of ever-life regs at the beginning of
     the insn chain
   * insert loads as soon, stores as late as possile
   * insert spill insns as outward as possible (either looptree, or LCM)
   * reuse stack-slots
   * use the frame-pointer, when we can (possibly done)
   * delete coalesced insns.  Partly done.  The rest can only go, when we get
     rid of reload.
   * don't insert hard-regs, but a limited set of pseudo-reg
     in emit_colors, and setup reg_renumber accordingly (done, but this
     needs reload, which I want to go away)
   * don't destroy coalescing information completely when spilling
   * use the constraints from asms
   * implement spill coalescing/propagation
  */

static struct obstack ra_obstack;
static void create_insn_info PARAMS ((struct df *));
static void free_insn_info PARAMS ((void));
static void alloc_mem PARAMS ((struct df *));
static void free_mem PARAMS ((struct df *));
static void free_all_mem PARAMS ((struct df *df));
static int one_pass PARAMS ((struct df *, int));
static void validify_one_insn PARAMS ((rtx));
static void check_df PARAMS ((struct df *));

static void init_ra PARAMS ((void));

void reg_alloc PARAMS ((void));

/* Uhhuuhu.  Don't the hell use two sbitmaps! XXX
   (for now I need the sup_igraph to note if there is any conflict between
   parts of webs at all.  I can't use igraph for this, as there only the real
   conflicts are noted.)  This is only used to prevent coalescing two
   conflicting webs, were only parts of them are in conflict.  */
sbitmap igraph;
sbitmap sup_igraph;

/* Note the insns not inserted by the allocator, where we detected any
   deaths of pseudos.  It is used to detect closeness of defs and uses.
   In the first pass this is empty (we could initialize it from REG_DEAD
   notes), in the other passes it is left from the pass before.  */
sbitmap insns_with_deaths;
int death_insns_max_uid;

struct web_part *web_parts;

unsigned int num_webs;
unsigned int num_subwebs;
unsigned int num_allwebs;
struct web **id2web;
struct web *hardreg2web[FIRST_PSEUDO_REGISTER];
struct web **def2web;
struct web **use2web;
struct move_list *wl_moves;
int ra_max_regno;
short *ra_reg_renumber;
struct df *df;
bitmap *live_at_end;
int ra_pass;
unsigned int max_normal_pseudo;
int an_unusable_color;
 
/* The different lists on which a web can be (based on the type).  */
struct dlist *web_lists[(int) LAST_NODE_TYPE];

unsigned int last_def_id;
unsigned int last_use_id;
unsigned int last_num_webs;
int last_max_uid;
sbitmap last_check_uses;
unsigned int remember_conflicts;

/* Used to detect spill instructions inserted by me.  */
int orig_max_uid;

HARD_REG_SET never_use_colors;
HARD_REG_SET usable_regs[N_REG_CLASSES];
unsigned int num_free_regs[N_REG_CLASSES];
HARD_REG_SET hardregs_for_mode[NUM_MACHINE_MODES];
unsigned char byte2bitcount[256];

#if 0
extern struct df2ra build_df2ra PARAMS ((struct df*, struct ra_info*));
static struct ra_info *ra_info;
#endif
struct df2ra df2ra;
static enum reg_class web_preferred_class PARAMS ((struct web *));
void web_class PARAMS ((void));

unsigned int debug_new_regalloc = -1;
int flag_ra_dump_only_costs = 0;
int flag_ra_biased = 0;
int flag_ra_ir_spilling = 0;
int flag_ra_optimistic_coalescing = 0;
int flag_ra_break_aliases = 0;
int flag_ra_merge_spill_costs = 0;
int flag_ra_spill_every_use = 0;
int flag_ra_dump_notes = 0;

void *
ra_alloc (size)
     size_t size;
{
  return obstack_alloc (&ra_obstack, size);
}

void *
ra_calloc (size)
     size_t size;
{
  void *p = obstack_alloc (&ra_obstack, size);
  memset (p, 0, size);
  return p;
}

int
hard_regs_count (rs)
     HARD_REG_SET rs;
{
  int count = 0;
#ifdef HARD_REG_SET
  while (rs)
    {
      unsigned char byte = rs & 0xFF;
      rs >>= 8;
      /* Avoid memory access, if nothing is set.  */
      if (byte)
        count += byte2bitcount[byte];
    }
#else
  unsigned int ofs;
  for (ofs = 0; ofs < HARD_REG_SET_LONGS; ofs++)
    {
      HARD_REG_ELT_TYPE elt = rs[ofs];
      while (elt)
	{
	  unsigned char byte = elt & 0xFF;
	  elt >>= 8;
	  if (byte)
	    count += byte2bitcount[byte];
	}
    }
#endif
  return count;
}

/* Basically like emit_move_insn (i.e. validifies constants and such),
   but also handle MODE_CC moves (but then the operands must already
   be basically valid.  */
rtx
ra_emit_move_insn (x, y)
     rtx x, y;
{
  enum machine_mode mode = GET_MODE (x);
  if (GET_MODE_CLASS (mode) == MODE_CC)
    return emit_insn (gen_move_insn (x, y));
  else
    return emit_move_insn (x, y);
}

int insn_df_max_uid;
struct ra_insn_info *insn_df;
static struct ref **refs_for_insn_df;

static void
create_insn_info (df)
     struct df *df;
{
  rtx insn;
  struct ref **act_refs;
  insn_df_max_uid = get_max_uid ();
  insn_df = xcalloc (insn_df_max_uid, sizeof (insn_df[0]));
  refs_for_insn_df = xcalloc (df->def_id + df->use_id, sizeof (struct ref *));
  act_refs = refs_for_insn_df;
  /* We create those things backwards to mimic the order in which
     the insns are visited in rewrite_program2() and live_in().  */
  for (insn = get_last_insn (); insn; insn = PREV_INSN (insn))
    {
      int uid = INSN_UID (insn);
      unsigned int n;
      struct df_link *link;
      if (!INSN_P (insn))
	continue;
      for (n = 0, link = DF_INSN_DEFS (df, insn); link; link = link->next)
        if (link->ref
	    && (DF_REF_REGNO (link->ref) >= FIRST_PSEUDO_REGISTER
		|| !TEST_HARD_REG_BIT (never_use_colors,
				       DF_REF_REGNO (link->ref))))
	  {
	    if (n == 0)
	      insn_df[uid].defs = act_refs;
	    insn_df[uid].defs[n++] = link->ref;
	  }
      act_refs += n;
      insn_df[uid].num_defs = n;
      for (n = 0, link = DF_INSN_USES (df, insn); link; link = link->next)
        if (link->ref
	    && (DF_REF_REGNO (link->ref) >= FIRST_PSEUDO_REGISTER
		|| !TEST_HARD_REG_BIT (never_use_colors,
				       DF_REF_REGNO (link->ref))))
	  {
	    if (n == 0)
	      insn_df[uid].uses = act_refs;
	    insn_df[uid].uses[n++] = link->ref;
	  }
      act_refs += n;
      insn_df[uid].num_uses = n;
    }
  if (refs_for_insn_df + (df->def_id + df->use_id) < act_refs)
    abort ();
}

static void
free_insn_info (void)
{
  free (refs_for_insn_df);
  refs_for_insn_df = NULL;
  free (insn_df);
  insn_df = NULL;
  insn_df_max_uid = 0;
}

struct web *
find_subweb (web, reg)
     struct web *web;
     rtx reg;
{
  struct web *w;
  if (GET_CODE (reg) != SUBREG)
    abort ();
  for (w = web->subreg_next; w; w = w->subreg_next)
    if (GET_MODE (w->orig_x) == GET_MODE (reg)
	&& SUBREG_BYTE (w->orig_x) == SUBREG_BYTE (reg))
      return w;
  return NULL;
}

struct web *
find_subweb_2 (web, size_word)
     struct web *web;
     unsigned int size_word;
{
  struct web *w = web;
  if (size_word == GET_MODE_SIZE (GET_MODE (web->orig_x)))
    /* size_word == size means BYTE_BEGIN(size_word) == 0.  */
    return web;
  for (w = web->subreg_next; w; w = w->subreg_next)
    {
      unsigned int bl = rtx_to_bits (w->orig_x);
      if (size_word == bl)
        return w;
    }
  return NULL;
}

struct web *
find_web_for_subweb_1 (subweb)
     struct web *subweb;
{
  while (subweb->parent_web)
    subweb = subweb->parent_web;
  return subweb;
}

/* Determine if two hard register sets intersect.
   Return 1 if they do.  */
int
hard_regs_intersect_p (a, b)
     HARD_REG_SET *a, *b;
{
  HARD_REG_SET c;
  COPY_HARD_REG_SET (c, *a);
  AND_HARD_REG_SET (c, *b);
  GO_IF_HARD_REG_SUBSET (c, reg_class_contents[(int) NO_REGS], lose);
  return 1;
lose:
  return 0;
}

/* Allocate the memory necessary for the register allocator.  */
static void
alloc_mem (df)
     struct df *df;
{
  int i;
  ra_build_realloc (df);
  if (!live_at_end)
    {
      live_at_end = (bitmap *) xmalloc ((last_basic_block + 2)
					* sizeof (bitmap));
      for (i = 0; i < last_basic_block + 2; i++)
	live_at_end[i] = BITMAP_XMALLOC ();
      live_at_end += 2;
    }
  create_insn_info (df);
}

/* Free the memory used by the register allocator.  */
static void
free_mem (df)
     struct df *df ATTRIBUTE_UNUSED;
{
  free_insn_info ();
  ra_build_free ();
}

static void
free_all_mem (df)
     struct df *df;
{
  unsigned int i;
  live_at_end -= 2;
  for (i = 0; i < (unsigned)last_basic_block + 2; i++)
    BITMAP_XFREE (live_at_end[i]);
  free (live_at_end);

  ra_colorize_free_all ();
  ra_build_free_all (df);
  obstack_free (&ra_obstack, NULL);
}

static long ticks_build;
static long ticks_rebuild;

/* Perform one pass of iterated coalescing.  */
static int
one_pass (df, rebuild)
     struct df *df;
     int rebuild;
{
  long ticks = clock ();
  int something_spilled;
  remember_conflicts = 0;
  build_i_graph (df);
  remember_conflicts = 1;
  if (!rebuild)
    dump_igraph_machine ();

  ra_colorize_graph (df);

  last_max_uid = get_max_uid ();
  /* actual_spill() might change WEBS(SPILLED) and even empty it,
     so we need to remember it's state.  */
  something_spilled = !!WEBS(SPILLED);
  if (something_spilled)
    actual_spill ();
  ticks = clock () - ticks;
  if (rebuild)
    ticks_rebuild += ticks;
  else
    ticks_build += ticks;
  return something_spilled;
}

/* Initialize the register allocator.  */
static void
init_ra (void)
{
  int i;
  HARD_REG_SET rs;
#ifdef ELIMINABLE_REGS
  static struct {int from, to; } eliminables[] = ELIMINABLE_REGS;
  unsigned int j;
#endif
  int need_fp
    = (! flag_omit_frame_pointer
#ifdef EXIT_IGNORE_STACK
       || (current_function_calls_alloca && EXIT_IGNORE_STACK)
#endif
       || FRAME_POINTER_REQUIRED);

  ra_colorize_init ();

  COPY_HARD_REG_SET (never_use_colors, fixed_reg_set);

#ifdef ELIMINABLE_REGS
  for (j = 0; j < ARRAY_SIZE (eliminables); j++)
    {
      if (! CAN_ELIMINATE (eliminables[j].from, eliminables[j].to)
	  || (eliminables[j].to == STACK_POINTER_REGNUM && need_fp))
	for (i = HARD_REGNO_NREGS (eliminables[j].from, Pmode); i--;)
	  SET_HARD_REG_BIT (never_use_colors, eliminables[j].from + i);
    }
#if FRAME_POINTER_REGNUM != HARD_FRAME_POINTER_REGNUM
  if (need_fp)
    for (i = HARD_REGNO_NREGS (HARD_FRAME_POINTER_REGNUM, Pmode); i--;)
      SET_HARD_REG_BIT (never_use_colors, HARD_FRAME_POINTER_REGNUM + i);
#endif

#else
  if (need_fp)
    for (i = HARD_REGNO_NREGS (FRAME_POINTER_REGNUM, Pmode); i--;)
      SET_HARD_REG_BIT (never_use_colors, FRAME_POINTER_REGNUM + i);
#endif
/*
#if HARD_FRAME_POINTER_REGNUM != FRAME_POINTER_REGNUM
  for (i = HARD_REGNO_NREGS (HARD_FRAME_POINTER_REGNUM, Pmode); i--;)
    SET_HARD_REG_BIT (never_use_colors, HARD_FRAME_POINTER_REGNUM + i);
#endif

  for (i = HARD_REGNO_NREGS (FRAME_POINTER_REGNUM, Pmode); i--;)
    SET_HARD_REG_BIT (never_use_colors, FRAME_POINTER_REGNUM + i);
*/
  for (i = HARD_REGNO_NREGS (STACK_POINTER_REGNUM, Pmode); i--;)
    SET_HARD_REG_BIT (never_use_colors, STACK_POINTER_REGNUM + i);

  for (i = HARD_REGNO_NREGS (ARG_POINTER_REGNUM, Pmode); i--;)
    SET_HARD_REG_BIT (never_use_colors, ARG_POINTER_REGNUM + i);
	
  for (i = 0; i < 256; i++)
    {
      unsigned char byte = ((unsigned) i) & 0xFF;
      unsigned char count = 0;
      while (byte)
	{
	  if (byte & 1)
	    count++;
	  byte >>= 1;
	}
      byte2bitcount[i] = count;
    }
  
  for (i = 0; i < N_REG_CLASSES; i++)
    {
      int size;
      COPY_HARD_REG_SET (rs, reg_class_contents[i]);
      AND_COMPL_HARD_REG_SET (rs, never_use_colors);
      size = hard_regs_count (rs);
      num_free_regs[i] = size;
      COPY_HARD_REG_SET (usable_regs[i], rs);
    }

  for (i = 0; i < NUM_MACHINE_MODES; i++)
    {
      int reg, size;
      CLEAR_HARD_REG_SET (rs);
      for (reg = 0; reg < FIRST_PSEUDO_REGISTER; reg++)
	if (HARD_REGNO_MODE_OK (reg, i)
	    /* Ignore VOIDmode and similar things.  */
	    && (size = HARD_REGNO_NREGS (reg, i)) != 0
	    && (reg + size) <= FIRST_PSEUDO_REGISTER)
	  {
	    while (size--)
	      SET_HARD_REG_BIT (rs, reg + size);
	  }
      COPY_HARD_REG_SET (hardregs_for_mode[i], rs);
    }
  
  for (an_unusable_color = 0; an_unusable_color < FIRST_PSEUDO_REGISTER;
       an_unusable_color++)
    if (TEST_HARD_REG_BIT (never_use_colors, an_unusable_color))
      break;
  if (an_unusable_color == FIRST_PSEUDO_REGISTER)
    abort ();

  orig_max_uid = get_max_uid ();
  compute_bb_for_insn ();
  ra_reg_renumber = NULL;
  insns_with_deaths = sbitmap_alloc (orig_max_uid);
  death_insns_max_uid = orig_max_uid;
  sbitmap_ones (insns_with_deaths);
  gcc_obstack_init (&ra_obstack);
}

/* Check the consistency of DF.  */
static void
check_df (df)
     struct df *df;
{
  struct df_link *link;
  rtx insn;
  int regno;
  unsigned int ui;
  bitmap b = BITMAP_XMALLOC ();
  bitmap empty_defs = BITMAP_XMALLOC ();
  bitmap empty_uses = BITMAP_XMALLOC ();

  for (ui = 0; ui < df->def_id; ui++)
    if (!df->defs[ui])
      bitmap_set_bit (empty_defs, ui);
  for (ui = 0; ui < df->use_id; ui++)
    if (!df->uses[ui])
      bitmap_set_bit (empty_uses, ui);
  
  for (insn = get_insns (); insn; insn = NEXT_INSN (insn))
    if (INSN_P (insn))
      {
	bitmap_clear (b);
	for (link = DF_INSN_DEFS (df, insn); link; link = link->next)
	  if (!link->ref || bitmap_bit_p (empty_defs, DF_REF_ID (link->ref))
	      || bitmap_bit_p (b, DF_REF_ID (link->ref)))
	    abort ();
	  else
	    bitmap_set_bit (b, DF_REF_ID (link->ref));
			    
	bitmap_clear (b);
	for (link = DF_INSN_USES (df, insn); link; link = link->next)
	  if (!link->ref || bitmap_bit_p (empty_uses, DF_REF_ID (link->ref))
	      || bitmap_bit_p (b, DF_REF_ID (link->ref)))
	    abort ();
	  else
	    bitmap_set_bit (b, DF_REF_ID (link->ref));
      }

  for (regno = 0; regno < max_reg_num (); regno++)
    {
      bitmap_clear (b);
      for (link = df->regs[regno].defs; link; link = link->next)
	if (!link->ref || bitmap_bit_p (empty_defs, DF_REF_ID (link->ref))
	    || bitmap_bit_p (b, DF_REF_ID (link->ref)))
	  abort ();
	else
	  bitmap_set_bit (b, DF_REF_ID (link->ref));

      bitmap_clear (b);
      for (link = df->regs[regno].uses; link; link = link->next)
	if (!link->ref || bitmap_bit_p (empty_uses, DF_REF_ID (link->ref))
	    || bitmap_bit_p (b, DF_REF_ID (link->ref)))
	  abort ();
	else
	  bitmap_set_bit (b, DF_REF_ID (link->ref));
    }
  
  BITMAP_XFREE (empty_uses);
  BITMAP_XFREE (empty_defs);
  BITMAP_XFREE (b);
}

static void
validify_one_insn (insn)
     rtx insn;
{
  int alt, valid, n_ops;
  int i;
  int commutative = -1;
  extract_insn (insn);
  valid = constrain_operands (0);
  preprocess_constraints ();
  alt = which_alternative;
  n_ops = recog_data.n_operands;
  for (i = 0; i < n_ops; i++)
    if (strchr (recog_data.constraints[i], '%') != NULL)
      commutative = i;
  ra_print_rtx_top (rtl_dump_file, insn, 0);
  if (recog_data.n_alternatives == 0 || n_ops == 0)
    {
      if (!valid)
	abort ();
      fprintf (rtl_dump_file,
	       "   --> has no constrained operands, i.e. is valid\n");
    }
  else if (valid)
    {
      if (alt < 0)
	abort ();
      fprintf (rtl_dump_file, "   --> matched alternative %d\n", alt);
      for (i = 0; i < n_ops; i++)
	{
	  char *constraint = xstrdup (recog_op_alt[i][alt].constraint);
	  char *comma = strchr (constraint, ',');
	  int len;
	  if (comma)
	    *comma = 0;
	  len = strlen (constraint);
	  fprintf (rtl_dump_file, "\top%d: %s\t", i, constraint);
	  if (len <= 2)
	    fprintf (rtl_dump_file, "\t");
	  if (comma)
	    *comma = ',';
	  ra_print_rtx (rtl_dump_file, recog_data.operand[i], 0);
	  fprintf (rtl_dump_file, "\n");
	  free (constraint);
	}
    }
  else
    {
      fprintf (rtl_dump_file, "  --> invalid insn");
      if (commutative >= 0)
	fprintf (rtl_dump_file, ", but commutative in op %d", commutative);
      fprintf (rtl_dump_file, "\n");
    }
}

static void
make_insns_structurally_valid (void)
{
  rtx insn;
  int old_rip = reload_in_progress;
  if (!rtl_dump_file || (debug_new_regalloc & DUMP_VALIDIFY) == 0)
    return;
  reload_in_progress = 0;
  for (insn = get_insns (); insn; insn = NEXT_INSN (insn))
    if (INSN_P (insn))
      {
	validify_one_insn (insn);
      }
  reload_in_progress = old_rip;
}

/* XXX see recog.c  */
extern int while_newra;

/* Main register allocator entry point.  */
void
reg_alloc (void)
{
  int changed;
  FILE *ra_dump_file = rtl_dump_file;
  rtx last = get_last_insn ();

  if (! INSN_P (last))
    last = prev_real_insn (last);
  /* If this is an empty function we shouldn't do all the following,
     but instead just setup what's necessary, and return.  */
  if (last)
    {
      edge e;
      for (e = EXIT_BLOCK_PTR->pred; e; e = e->pred_next)
	{
	  basic_block bb = e->src;
	  last = bb->end;
	  if (!INSN_P (last) || GET_CODE (PATTERN (last)) != USE)
	    {
	      rtx insns;
	      start_sequence ();
	      use_return_register ();
	      insns = get_insns ();
	      end_sequence ();
	      emit_insn_after (insns, last);
	    }
	}
    }

  /*verify_flow_info ();*/

  switch (0)
    {
      case 0: debug_new_regalloc = DUMP_EVER; break;
      case 1: debug_new_regalloc = DUMP_COSTS; break;
      case 2: debug_new_regalloc = DUMP_IGRAPH_M; break;
      case 3: debug_new_regalloc = DUMP_COLORIZE + DUMP_COSTS; break;
      case 4: debug_new_regalloc = DUMP_COLORIZE + DUMP_COSTS + DUMP_WEBS;
	      break;
      case 5: debug_new_regalloc = DUMP_FINAL_RTL + DUMP_COSTS +
	      DUMP_CONSTRAINTS;
	      break;
      case 6: debug_new_regalloc = DUMP_VALIDIFY; break;
    }
  if (!rtl_dump_file)
    debug_new_regalloc = 0;

  if ((debug_new_regalloc & DUMP_REGCLASS) == 0)
    rtl_dump_file = NULL;
  regclass (get_insns (), max_reg_num (), rtl_dump_file);
  rtl_dump_file = ra_dump_file;

/*  ra_info = ra_info_init (max_reg_num ());
  pre_reload (ra_info);
  {
    allocate_reg_info (max_reg_num (), FALSE, FALSE);
    compute_bb_for_insn ();
    delete_trivially_dead_insns (get_insns (), max_reg_num ());
    reg_scan_update (get_insns (), BLOCK_END (n_basic_blocks - 1),
		     max_regno);
    max_regno = max_reg_num ();
    regclass (get_insns (), max_reg_num (), rtl_dump_file);
  }
  ra_info_free (ra_info);
  free (ra_info);*/
  
  /* XXX the REG_EQUIV notes currently are screwed up, when pseudos are
     coalesced, which have such notes.  In that case, the whole combined
     web gets that note too, which is wrong.  */
  /*update_equiv_regs();*/
  init_ra ();
  /*  find_nesting_depths (); */
  ra_pass = 0;
  no_new_pseudos = 0;
  max_normal_pseudo = (unsigned) max_reg_num ();
  /* We don't use those NOTEs, and as we anyway change all registers,
     they only make problems later.  */
  count_or_remove_death_notes (NULL, 1);
  ra_rewrite_init ();
  last_def_id = 0;
  last_use_id = 0;
  last_num_webs = 0;
  last_max_uid = 0;
  last_check_uses = NULL;
  live_at_end = NULL;
  WEBS(INITIAL) = NULL;
  WEBS(FREE) = NULL;
  memset (hardreg2web, 0, sizeof (hardreg2web));
  ticks_build = ticks_rebuild = 0;
  flag_ra_biased = 0;
  flag_ra_spill_every_use = 0;
  flag_ra_ir_spilling = 1;
  flag_ra_break_aliases = 0;
  flag_ra_optimistic_coalescing = 1;
  flag_ra_merge_spill_costs = 1;
  if (flag_ra_optimistic_coalescing)
    flag_ra_break_aliases = 1;
  flag_ra_dump_notes = 0;
  make_insns_structurally_valid ();
  /*verify_flow_info ();*/
  df = df_init ();
  do
    {
      ra_debug_msg (DUMP_NEARLY_EVER, "RegAlloc Pass %d\n\n", ra_pass);
      if (ra_pass++ > 40)
	internal_error ("Didn't find a coloring.\n");

      /* FIXME denisc@overta.ru
	 Example of usage ra_info ... routines */
#if 0
      ra_info = ra_info_init (max_reg_num ());
      pre_reload (ra_info);

      {
	allocate_reg_info (max_reg_num (), FALSE, FALSE);
	compute_bb_for_insn ();
	delete_trivially_dead_insns (get_insns (), max_reg_num ());
	reg_scan_update (get_insns (), BLOCK_END (n_basic_blocks - 1),
			 max_regno);
	max_regno = max_reg_num ();
	regclass (get_insns (), max_reg_num (), rtl_dump_file);
	orig_max_uid = get_max_uid ();
	death_insns_max_uid = orig_max_uid;
      }
#endif
      
      df_analyse (df, (ra_pass == 1) ? 0 : (bitmap) -1,
		  DF_HARD_REGS | DF_RD_CHAIN | DF_RU_CHAIN
#ifndef NO_REMAT
		  | DF_DU_CHAIN | DF_UD_CHAIN
#endif
		 );
      
      /* FIXME denisc@overta.ru
	 Example of usage ra_info ... routines */
#if 0
      df2ra = build_df2ra (df, ra_info);
#endif
      if ((debug_new_regalloc & DUMP_DF) != 0)
	{
	  rtx insn;
	  df_dump (df, DF_HARD_REGS, rtl_dump_file);
	  for (insn = get_insns (); insn; insn = NEXT_INSN (insn))
            if (INSN_P (insn))
	      {
	        df_insn_debug_regno (df, insn, rtl_dump_file);
	      }
	}
      check_df (df);
      alloc_mem (df);
      /*ra_debug_msg (DUMP_EVER, "before one_pass()\n");
      if (rtl_dump_file)
	print_rtl_with_bb (rtl_dump_file, get_insns ()); 
      verify_flow_info ();*/
      changed = one_pass (df, ra_pass > 1);
      /*ra_debug_msg (DUMP_EVER, "after one_pass()\n");
      if (rtl_dump_file)
        print_rtl_with_bb (rtl_dump_file, get_insns ()); 
      verify_flow_info ();*/
      /* FIXME denisc@overta.ru
	 Example of usage ra_info ... routines */
#if 0
      ra_info_free (ra_info);
      free (df2ra.def2def);
      free (df2ra.use2use);
      free (ra_info);
#endif 
      if (!changed)
	{
          emit_colors (df);
	  /* Already setup a preliminary reg_renumber[] array, but don't
	     free our own version.  reg_renumber[] will again be destroyed
	     later.  We right now need it in dump_constraints() for
	     constrain_operands(1) whose subproc sometimes reference
	     it (because we are cehcking strictly, i.e. as if
	     after reload).  */
	  setup_renumber (0);
	  delete_moves ();
	  dump_constraints ();
	}
      else
	{
	  if ((debug_new_regalloc & DUMP_REGCLASS) == 0)
	    rtl_dump_file = NULL;
	  allocate_reg_info (max_reg_num (), FALSE, FALSE);
	  compute_bb_for_insn ();
	  delete_trivially_dead_insns (get_insns (), max_reg_num ());
	  reg_scan_update (get_insns (), NULL, max_regno);
	  max_regno = max_reg_num ();
	  regclass (get_insns (), max_reg_num (), rtl_dump_file);
	  rtl_dump_file = ra_dump_file;

	  last_def_id = df->def_id;
	  last_use_id = df->use_id;
	}
      dump_ra (df);
      if (changed && (debug_new_regalloc & DUMP_RTL) != 0)
	{
	  ra_print_rtl_with_bb (rtl_dump_file, get_insns ());
	  fflush (rtl_dump_file);
	}
      reset_lists ();
      free_mem (df);
    }
  while (changed);
  free_all_mem (df);
  /*  free (depths); */
  df_finish (df);
  if ((debug_new_regalloc & DUMP_RESULTS) == 0)
    dump_cost (DUMP_COSTS);
  /*ra_debug_msg (DUMP_COSTS, "ticks for build-phase: %ld\n", ticks_build);
  ra_debug_msg (DUMP_COSTS, "ticks for rebuild-phase: %ld\n", ticks_rebuild);*/
  if ((debug_new_regalloc & (DUMP_FINAL_RTL | DUMP_RTL)) != 0)
    ra_print_rtl_with_bb (rtl_dump_file, get_insns ()); 
  
  if ((debug_new_regalloc & DUMP_SM) == 0)
    rtl_dump_file = NULL;
  no_new_pseudos = 0;
  allocate_reg_info (max_reg_num (), FALSE, FALSE);
  /*compute_bb_for_insn ();*/
  while_newra = 1;
  /*store_motion ();*/
  no_new_pseudos = 1;
  rtl_dump_file = ra_dump_file;

  fixup_abnormal_edges ();
  if ((debug_new_regalloc & DUMP_LAST_FLOW) == 0)
    rtl_dump_file = NULL;
  /*free_bb_for_insn ();
  find_basic_blocks (get_insns (), max_reg_num (), rtl_dump_file);*/
  /*compute_bb_for_insn ();*/
  /*clear_log_links (get_insns ());*/
  life_analysis (get_insns (), rtl_dump_file, 
		 PROP_DEATH_NOTES | PROP_LOG_LINKS  | PROP_REG_INFO);
/*  recompute_reg_usage (get_insns (), TRUE);
  life_analysis (get_insns (), rtl_dump_file, 
		 PROP_SCAN_DEAD_CODE | PROP_KILL_DEAD_CODE); */
  cleanup_cfg (CLEANUP_EXPENSIVE);
  recompute_reg_usage (get_insns (), TRUE);
/*  delete_trivially_dead_insns (get_insns (), max_reg_num ());*/
  if (rtl_dump_file)
    dump_flow_info (rtl_dump_file);
	  
  /* XXX: reg_scan screws up reg_renumber, and without reg_scan, we can't do
     regclass. */
  /*reg_scan (get_insns (), max_reg_num (), 1);
    regclass (get_insns (), max_reg_num (), rtl_dump_file); */
  rtl_dump_file = ra_dump_file;

  /* Also update_equiv_regs() can't be called after register allocation.
     It might delete some pseudos, and insert other insns setting
     up those pseudos in different places.  This of course screws up
     the allocation because that may destroy a hardreg for another
     pseudo.
     XXX we probably should do something like that on our own.  I.e.
     creating REG_EQUIV notes.  */
  /*update_equiv_regs ();*/
  /* We must maintain our own reg_renumber[] array, because life_analysis()
     destroys any prior set up reg_renumber[].  */
  while_newra = 0;
  setup_renumber (1);
  sbitmap_free (insns_with_deaths);

  remove_suspicious_death_notes ();
  if ((debug_new_regalloc & DUMP_LAST_RTL) != 0)
    ra_print_rtl_with_bb (rtl_dump_file, get_insns ()); 
  dump_static_insn_cost (rtl_dump_file,
			 "after allocation/spilling, before reload", NULL);

  /* Allocate the reg_equiv_memory_loc array for reload.  */
  reg_equiv_memory_loc = (rtx *) xcalloc (max_regno, sizeof (rtx));
  /* And possibly initialize it.  */
  allocate_initial_values (reg_equiv_memory_loc);
  regclass (get_insns (), max_reg_num (), rtl_dump_file);
}


static enum reg_class *reg_class_of_web;

static enum reg_class
web_preferred_class (web)
     struct web *web;
{
  if (!reg_class_of_web)
    abort ();

  if (web->id >= num_webs)
    abort ();

  return reg_class_of_web[find_web_for_subweb(web)->id];
}

void
web_class ()
{
  unsigned int n;
  unsigned int i;
  char class[LIM_REG_CLASSES];
  ra_ref *rref;
  struct ref* dref;
  enum reg_class best;

  if (reg_class_of_web)
    free (reg_class_of_web);

  reg_class_of_web = xmalloc (sizeof (enum reg_class) * (num_webs
							 - num_subwebs));
  for (n = 0; n < num_webs - num_subwebs; ++n)
    {
      struct web *web = id2web[n];
      int founded = 0;

      reg_class_of_web[n] = NO_REGS;

      if (web->type == PRECOLORED)
	continue;
      
      for (i = 0; i < LIM_REG_CLASSES; ++i)
	class[i] = 0;

      for (i = 0; i < web->num_defs; ++i)
	{
	  dref = web->defs[i];
	  rref = DF2RA (df2ra, dref);
	  if (rref)
	    ++class[rref->class];
	}

      for (i = 0; i < web->num_uses; ++i)
	{
	  dref = web->uses[i];
	  rref = DF2RA (df2ra, dref);
	  if (rref)
	    ++class[rref->class];
	}

/*        fprintf (stderr, "Web: %d ", web->id); */
      best = ALL_REGS;
      for (i = 0; i < LIM_REG_CLASSES; ++i)
	if (class[i])
	  {
	    if (reg_class_subset_p (i, best))
	      {
		best = i;
		founded = 1;
	      }
	    else if (!reg_class_subset_p (best, i))
	      best = NO_REGS;
/*  	    fprintf (stderr, "%s: %d ", reg_class_names[i], class[i]); */
	  }
/*    fprintf (stderr, " BEST: %s\n", reg_class_names[best]); */
      if (best == NO_REGS)
	{
	  fprintf (stderr, "Web: %d (%d) NO_REGS\n", web->id, web->regno);
	  best = GENERAL_REGS;
	}
      reg_class_of_web[n] = best;
    }
}

/*
vim:cinoptions={.5s,g0,p5,t0,(0,^-0.5s,n-0.5s:tw=78:cindent:sw=4:
*/
