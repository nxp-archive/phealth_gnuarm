/* Graph coloring register allocator
   Copyright (C) 2001, 2002, 2003 Free Software Foundation, Inc.
   Contributed by Michael Matz <matz@suse.de>
   and Daniel Berlin <dan@cgsoftware.com>.

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it under the
   terms of the GNU General Public License as published by the Free Software
   Foundation; either version 2, or (at your option) any later version.

   GCC is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
   FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
   details.

   You should have received a copy of the GNU General Public License along
   with GCC; see the file COPYING.  If not, write to the Free Software
   Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "rtl.h"
#include "tm_p.h"
#include "insn-config.h"
#include "recog.h"
#include "reload.h"
#include "integrate.h"
#include "function.h"
#include "regs.h"
#include "obstack.h"
#include "hard-reg-set.h"
#include "basic-block.h"
#include "df.h"
#include "expr.h"
#include "output.h"
#include "toplev.h"
#include "flags.h"
#include "pre-reload.h"
#include "ra.h"

/* This is the toplevel file of a graph coloring register allocator.
   It is able to act like a George & Appel allocator, i.e. with iterative
   coalescing plus spill coalescing/propagation.
   And it can act as a traditional Briggs allocator, although with
   optimistic coalescing.  Additionally it has a custom pass, which
   tries to reduce the overall cost of the colored graph.

   We support two modes of spilling: spill-everywhere, which is extremely
   fast, and interference region spilling, which reduces spill code to a
   large extent, but is slower.

   Helpful documents:

   Briggs, P., Cooper, K. D., and Torczon, L. 1994. Improvements to graph
   coloring register allocation. ACM Trans. Program. Lang. Syst. 16, 3 (May),
   428-455.

   Bergner, P., Dahl, P., Engebretsen, D., and O'Keefe, M. 1997. Spill code
   minimization via interference region spilling. In Proc. ACM SIGPLAN '97
   Conf. on Prog. Language Design and Implementation. ACM, 287-295.

   George, L., Appel, A.W. 1996.  Iterated register coalescing.
   ACM Trans. Program. Lang. Syst. 18, 3 (May), 300-324.

*/

/* This file contains the main entry point (reg_alloc), some helper routines
   used by more than one file of the register allocator, and the toplevel
   driver procedure (one_pass).  */

/* Things, one might do somewhen:

   * Lattice based rematerialization
   * create definitions of ever-life regs at the beginning of
     the insn chain
   * insert loads as soon, stores as late as possible
   * insert spill insns as outward as possible (either looptree, or LCM)
   * reuse stack-slots
   * delete coalesced insns.  Partly done.  The rest can only go, when we get
     rid of reload.
   * don't destroy coalescing information completely when spilling
   * use the constraints from asms
  */

static struct obstack ra_obstack;
static void create_insn_info (struct df *);
static void free_insn_info (void);
static void alloc_mem (struct df *);
static void free_mem (struct df *);
static void free_all_mem (struct df *df);
static int one_pass (struct df *, int);
static void check_df (struct df *);
static void validify_one_insn (rtx);
static void init_ra (void);
static void cleanup_insn_stream (void);
static void detect_possible_mem_refs (struct df *);

void reg_alloc (void);

/* See rtl.h.  */
int newra_in_progress;

/* These global variables are "internal" to the register allocator.
   They are all documented at their declarations in ra.h.  */

/* Somewhen we want to get rid of one of those sbitmaps.
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
int an_unusable_color;

/* The different lists on which a web can be (based on the type).  */
struct dlist *web_lists[(int) LAST_NODE_TYPE];

unsigned int last_def_id;
unsigned int last_use_id;
unsigned int last_num_webs;
int last_max_uid;
sbitmap last_check_uses;
unsigned int remember_conflicts;

HARD_REG_SET never_use_colors;
HARD_REG_SET usable_regs[N_REG_CLASSES];
unsigned int num_free_regs[N_REG_CLASSES];
HARD_REG_SET hardregs_for_mode[NUM_MACHINE_MODES];
HARD_REG_SET invalid_mode_change_regs;
unsigned char byte2bitcount[256];

/* Used to detect spill instructions inserted by me.  */
bitmap emitted_by_spill;

/* Tracking pseudos generated for spill slots by rewrite.  */
bitmap spill_slot_regs;

/* Tracking insns modified/deleted/emitted by allocator in current pass.  */
bitmap ra_modified_insns;

extern struct df2ra build_df2ra PARAMS ((struct df*, struct ra_info*));
static struct ra_info *ra_info;
struct df2ra df2ra;

unsigned int debug_new_regalloc = -1;
int flag_ra_dump_only_costs = 0;
int flag_ra_biased = 0;
int flag_ra_improved_spilling = 0;
int flag_ra_ir_spilling = 0;
int flag_ra_split_webs = 0;
int flag_ra_optimistic_coalescing = 0;
int flag_ra_break_aliases = 0;
int flag_ra_merge_spill_costs = 0;
int flag_ra_spill_every_use = 0;
int flag_ra_dump_notes = 0;

/* Fast allocation of small objects, which live until the allocator
   is done.  Allocate an object of SIZE bytes.  */

void *
ra_alloc (size_t size)
{
  return obstack_alloc (&ra_obstack, size);
}

/* Like ra_alloc(), but clear the returned memory.  */

void *
ra_calloc (size_t size)
{
  void *p = obstack_alloc (&ra_obstack, size);
  memset (p, 0, size);
  return p;
}

/* Returns the number of hardregs in HARD_REG_SET RS.  */

int
hard_regs_count (HARD_REG_SET rs)
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
ra_emit_move_insn (rtx x, rtx y)
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

/* Create the insn_df structure for each insn to have fast access to
   all valid defs and uses in an insn.  */

static void
create_insn_info (struct df *df)
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

/* Free the insn_df structures.  */

static void
free_insn_info (void)
{
  free (refs_for_insn_df);
  refs_for_insn_df = NULL;
  free (insn_df);
  insn_df = NULL;
  insn_df_max_uid = 0;
}

/* Search WEB for a subweb, which represents REG.  REG needs to
   be a SUBREG, and the inner reg of it needs to be the one which is
   represented by WEB.  Returns the matching subweb or NULL.  */

struct web *
find_subweb (struct web *web, rtx reg)
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

/* Similar to find_subweb(), but matches according to SIZE_WORD,
   a collection of the needed size and offset (in bytes).  */

struct web *
find_subweb_2 (struct web *web, unsigned int size_word)
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

/* Returns the superweb for SUBWEB.  */

struct web *
find_web_for_subweb_1 (struct web *subweb)
{
  while (subweb->parent_web)
    subweb = subweb->parent_web;
  return subweb;
}

/* Determine if two hard register sets intersect.
   Return 1 if they do.  */

int
hard_regs_intersect_p (HARD_REG_SET *a, HARD_REG_SET *b)
{
  HARD_REG_SET c;
  COPY_HARD_REG_SET (c, *a);
  AND_HARD_REG_SET (c, *b);
  GO_IF_HARD_REG_SUBSET (c, reg_class_contents[(int) NO_REGS], lose);
  return 1;
lose:
  return 0;
}

/* Two webs can be combined only if usable_regs of webs are intersects and
   hardregs required for web W1 must fits to intersected usable_regs.
   Return nonzero if they do.   */

int
hard_regs_combinable_p (w1, w2)
     struct web *w1;
     struct web *w2;
{
  HARD_REG_SET c;
  COPY_HARD_REG_SET (c, w1->usable_regs);
  AND_HARD_REG_SET (c, w2->usable_regs);
  return count_long_blocks (c, 1 + w1->add_hardregs);
}

/* Returns 1 of hard register set A and B are equal.  */

int
hard_regs_same_p (a, b)
     HARD_REG_SET a, b;
{
  GO_IF_HARD_REG_EQUAL (a, b, equal);
  return 0;
equal:
  return 1;
}

/* Allocate and initialize the memory necessary for one pass of the
   register allocator.  */

static void
alloc_mem (struct df *df)
{
  int i;
  ra_build_realloc (df);
  if (!live_at_end)
    {
      live_at_end = xmalloc ((last_basic_block + 2) * sizeof (bitmap));
      for (i = 0; i < last_basic_block + 2; i++)
	live_at_end[i] = BITMAP_XMALLOC ();
      live_at_end += 2;
    }
  create_insn_info (df);
}

extern void free_split_costs PARAMS ((void));

/* Free the memory which isn't necessary for the next pass.  */

static void
free_mem (struct df *df ATTRIBUTE_UNUSED)
{
  free_insn_info ();
  ra_build_free ();
  if (flag_ra_split_webs)
    free_split_costs ();
}

/* Free all memory allocated for the register allocator.  Used, when
   it's done.  */

static void
free_all_mem (struct df *df)
{
  unsigned int i;
  live_at_end -= 2;
  for (i = 0; i < (unsigned)last_basic_block + 2; i++)
    BITMAP_XFREE (live_at_end[i]);
  free (live_at_end);

  ra_colorize_free_all ();
  ra_build_free_all (df);
  if (last_changed_insns)
    BITMAP_XFREE (last_changed_insns);
  last_changed_insns = NULL;
  obstack_free (&ra_obstack, NULL);
}

static long ticks_build;
static long ticks_rebuild;

extern int any_splits_found;

/* Perform one pass of allocation.  Returns nonzero, if some spill code
   was added, i.e. if the allocator needs to rerun.  */

static int
one_pass (struct df *df, int rebuild)
{
  long ticks = clock ();
  int something_spilled;
  remember_conflicts = 0;

  /* Build the complete interference graph, or if this is not the first
     pass, rebuild it incrementally.  */
  build_i_graph (df);

  /* From now on, if we create new conflicts, we need to remember the
     initial list of conflicts per web.  */
  remember_conflicts = 1;
  if (!rebuild)
    dump_igraph_machine ();

  if (!WEBS (SPILLED))
    {
      /* Colorize the I-graph.  This results in either a list of
	 spilled_webs, in which case we need to run the spill phase, and
	 rerun the allocator, or that list is empty, meaning we are done.  */
      ra_colorize_graph (df);
      
      last_max_uid = get_max_uid ();

      /* actual_spill() might change WEBS(SPILLED) and even empty it,
	 so we need to remember it's state.  */
      something_spilled = !!WEBS(SPILLED);

      /* Add spill code if necessary.  */
      if (something_spilled || any_splits_found)
	something_spilled = actual_spill (1);

      /* Check all colored webs to detect ones colored by an_unusable_color.
	 These webs are spill temporaries and must be substituted by stack
	 slots. `subst_to_stack_p' performs checking.  */
      if (!something_spilled && subst_to_stack_p ())
	{
	  something_spilled = 1;
	  ra_debug_msg (DUMP_NEARLY_EVER,
			"Stack spill slots must be added.\n");
	  actual_spill (0);
	}
    }
  else if (ra_pass == 1)
    {
      if (death_insns_max_uid < get_max_uid ())
	{
	  sbitmap_free (insns_with_deaths);
	  insns_with_deaths = sbitmap_alloc (get_max_uid ());
	  sbitmap_ones (insns_with_deaths);
	  death_insns_max_uid = get_max_uid ();
	}
      last_changed_insns = ra_modified_insns;
      detect_web_parts_to_rebuild ();
      last_changed_insns = NULL;
      something_spilled = 1;
      last_max_uid = get_max_uid ();
    }
  else
    abort ();
  
  ticks = clock () - ticks;
  if (rebuild)
    ticks_rebuild += ticks;
  else
    ticks_build += ticks;
  return something_spilled;
}

/* Initialize various arrays for the register allocator.  */

static void
init_ra (void)
{
  int i;
  HARD_REG_SET rs;
#ifdef ELIMINABLE_REGS
  static const struct {const int from, to; } eliminables[] = ELIMINABLE_REGS;
  unsigned int j;
#endif
  int need_fp
    = (! flag_omit_frame_pointer
#ifdef EXIT_IGNORE_STACK
       || (current_function_calls_alloca && EXIT_IGNORE_STACK)
#endif
       || FRAME_POINTER_REQUIRED);

#ifdef ORDER_REGS_FOR_LOCAL_ALLOC
  if (1)
    ORDER_REGS_FOR_LOCAL_ALLOC;
#endif

  ra_colorize_init ();

  /* We can't ever use any of the fixed regs.  */
  COPY_HARD_REG_SET (never_use_colors, fixed_reg_set);

  /* Additionally don't even try to use hardregs, which we already
     know are not eliminable.  This includes also either the
     hard framepointer or all regs which are eliminable into the
     stack pointer, if need_fp is set.  */
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

  /* Stack and argument pointer are also rather useless to us.  */
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

  /* Setup hardregs_for_mode[].
     We are not interested only in the beginning of a multi-reg, but in
     all the hardregs involved.  Maybe HARD_REGNO_MODE_OK() only ok's
     for beginnings.  */
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

  CLEAR_HARD_REG_SET (invalid_mode_change_regs);
#ifdef CANNOT_CHANGE_MODE_CLASS
  if (0)
  for (i = 0; i < NUM_MACHINE_MODES; i++)
    {
      enum machine_mode from = (enum machine_mode) i;
      enum machine_mode to;
      for (to = VOIDmode; to < MAX_MACHINE_MODE; ++to)
	{
	  int r;
	  for (r = 0; r < FIRST_PSEUDO_REGISTER; r++)
	    if (REG_CANNOT_CHANGE_MODE_P (from, to, r))
	      SET_HARD_REG_BIT (invalid_mode_change_regs, r);
	}
    }
#endif

  for (an_unusable_color = 0; an_unusable_color < FIRST_PSEUDO_REGISTER;
       an_unusable_color++)
    if (TEST_HARD_REG_BIT (never_use_colors, an_unusable_color))
      break;
  if (an_unusable_color == FIRST_PSEUDO_REGISTER)
    abort ();
  compute_bb_for_insn ();
  ra_reg_renumber = NULL;
  insns_with_deaths = NULL;
  emitted_by_spill = BITMAP_XMALLOC ();
  spill_slot_regs = BITMAP_XMALLOC ();
  gcc_obstack_init (&ra_obstack);
}

/* Check the consistency of DF.  This aborts if it violates some
   invariances we expect.  */

static void
check_df (struct df *df)
{
  struct df_link *link;
  rtx insn;
  int regno;
  unsigned int ui;
  bitmap b = BITMAP_XMALLOC ();
  bitmap empty_defs = BITMAP_XMALLOC ();
  bitmap empty_uses = BITMAP_XMALLOC ();

  /* Collect all the IDs of NULL references in the ID->REF arrays,
     as df.c leaves them when updating the df structure.  */
  for (ui = 0; ui < df->def_id; ui++)
    if (!df->defs[ui])
      bitmap_set_bit (empty_defs, ui);
  for (ui = 0; ui < df->use_id; ui++)
    if (!df->uses[ui])
      bitmap_set_bit (empty_uses, ui);

  /* For each insn we check if the chain of references contain each
     ref only once, doesn't contain NULL refs, or refs whose ID is invalid
     (it df->refs[id] element is NULL).  */
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

  /* Now the same for the chains per register number.  */
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
validify_one_insn (rtx insn)
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

static void
detect_possible_mem_refs (struct df *df)
{
  rtx insn;
  int old_rip = reload_in_progress;
  reload_in_progress = 0;
  for (insn = get_insns (); insn; insn = NEXT_INSN (insn))
    if (INSN_P (insn))
      {
	struct df_link *l;
	int alt, valid, n_ops;
	int i, pass;
	struct operand_alternative *op_alt;
	extract_insn (insn);
	valid = constrain_operands (0);
	if (!valid)
	  continue;
	preprocess_constraints ();
	alt = which_alternative;
	n_ops = recog_data.n_operands;
	for (pass = 0; pass < 2; pass++)
	  for (l = (pass == 0) ? DF_INSN_DEFS (df, insn)
	       : DF_INSN_USES (df, insn); l; l = l->next)
	    for (i = 0; i < n_ops; i++)
	      if (recog_data.operand_loc[i] == DF_REF_LOC (l->ref))
		{
		  op_alt = &recog_op_alt[i][alt];
		  while (op_alt->matches > 0)
		    op_alt = &recog_op_alt[op_alt->matches][alt];
		  if (op_alt->memory_ok || op_alt->offmem_ok ||
		      op_alt->nonoffmem_ok || op_alt->anything_ok)
		    DF_REF_FLAGS (l->ref) |= DF_REF_MEM_OK;
		  else
		    DF_REF_FLAGS (l->ref) &= ~DF_REF_MEM_OK;
		}
      }
  reload_in_progress = old_rip;
}

/* Cleans up the insn stream.  It deletes stray clobber insns
   which start REG_NO_CONFLICT blocks, and the ending self moves.
   We track lifetimes of subregs precisely, and they only constrain
   the allocator.  */

static void
cleanup_insn_stream ()
{
  rtx insn;
  for (insn = get_insns(); insn; insn = NEXT_INSN (insn))
    if (INSN_P (insn))
      {
	rtx pat = PATTERN (insn);
	if (GET_CODE (pat) == SET
	    && SET_SRC (pat) == SET_DEST (pat)
	    && REG_P (SET_DEST (pat))
	    && REGNO (SET_DEST (pat)) >= FIRST_PSEUDO_REGISTER
	    && find_reg_note (insn, REG_RETVAL, NULL_RTX))
	  delete_insn_and_edges (insn);
	/* Remove all candidate clobbers, not just those which have
	   REG_LIBCALL notes.  */
	else if (GET_CODE (pat) == CLOBBER
		 && REG_P (SET_DEST (pat))
		 && REGNO (SET_DEST (pat)) >= FIRST_PSEUDO_REGISTER)
	  delete_insn_and_edges (insn);
      }
}

static void
ATTRIBUTE_UNUSED
split_critical_edges (void)
{
  basic_block bb;
  FOR_EACH_BB (bb)
    {
      edge e, e_succ;
      for (e = bb->succ; e; e = e_succ)
	{
	  e_succ = e->succ_next;
	  if (EDGE_CRITICAL_P (e)
	      && (e->flags & EDGE_ABNORMAL) == 0)
	    split_edge (e);
	}
    }
}

/* XXX see recog.c  */
extern int while_newra;

/* Main register allocator entry point.  */

void
reg_alloc (void)
{
  int changed;
  FILE *ra_dump_file = rtl_dump_file;
  rtx last;
  bitmap use_insns = BITMAP_XMALLOC ();

  delete_trivially_dead_insns (get_insns (), max_reg_num ());
  /* The above might have deleted some trapping insns making some basic
     blocks unreachable.  So do a simple cleanup pass to remove them.  */
  cleanup_cfg (0);
  last = get_last_insn ();

  /*
  flag_ra_pre_reload = 0;
  flag_ra_spanned_deaths_from_scratch = 0;
  */
  while_newra = 1;
  if (! INSN_P (last))
    last = prev_real_insn (last);
  /* If this is an empty function we shouldn't do all the following,
     but instead just setup what's necessary, and return.  */

  /* We currently rely on the existence of the return value USE as
     one of the last insns.  Add it if it's not there anymore.  */
  if (last)
    {
      edge e;
      for (e = EXIT_BLOCK_PTR->pred; e; e = e->pred_next)
	{
	  basic_block bb = e->src;
	  last = bb->end;
	  if (!INSN_P (last) || GET_CODE (PATTERN (last)) != USE)
	    {
	      rtx insn, insns;
	      start_sequence ();
	      use_return_register ();
	      insns = get_insns ();
	      end_sequence ();
	      for (insn = insns; insn; insn = NEXT_INSN (insn))
		bitmap_set_bit (use_insns, INSN_UID (insn));
	      emit_insn_after (insns, last);
	    }
	}
    }

  /* Setup debugging levels.  */
  switch (0)
    {
      /* Some useful presets of the debug level, I often use.  */
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

  /* First cleanup the insn stream of confusing clobber and self-copy
     insns which setup REG_NO_CONFLICT blocks.  */
  cleanup_insn_stream ();
  /*split_critical_edges ();*/

  /* Then run regclass, so we know the preferred and alternate classes
     for each pseudo.  Deactivate emitting of debug info, if it's not
     explicitly requested.  */
  if ((debug_new_regalloc & DUMP_REGCLASS) == 0)
    rtl_dump_file = NULL;
  if (!flag_ra_pre_reload)
    regclass (get_insns (), max_reg_num (), rtl_dump_file);
  rtl_dump_file = ra_dump_file;

  /* Initialize the different global arrays and regsets.  */
  init_ra ();

  /* And some global variables.  */
  ra_pass = 0;
  no_new_pseudos = 0;
  ra_rewrite_init ();
  last_def_id = 0;
  last_use_id = 0;
  last_num_webs = 0;
  last_max_uid = 0;
  last_changed_insns = NULL;
  last_check_uses = NULL;
  live_at_end = NULL;
  WEBS(INITIAL) = NULL;
  WEBS(FREE) = NULL;
  memset (hardreg2web, 0, sizeof (hardreg2web));
  ticks_build = ticks_rebuild = 0;

  /* The default is to use optimistic coalescing with interference
     region spilling, without biased coloring.  */
  flag_ra_biased = 0;
  flag_ra_spill_every_use = 0;
  flag_ra_improved_spilling = 1;
  flag_ra_ir_spilling = 0;
  flag_ra_split_webs = 1;
  flag_ra_break_aliases = 0;
  flag_ra_optimistic_coalescing = 1;
  flag_ra_merge_spill_costs = 1;
  if (flag_ra_optimistic_coalescing)
    flag_ra_break_aliases = 1;
  flag_ra_dump_notes = 0;
  if (max_reg_num () > 9000)
    flag_ra_split_webs = 0;
  if (flag_ra_ir_spilling && flag_ra_split_webs)
    abort ();
  make_insns_structurally_valid ();

  /* Allocate the global df structure.  */
  df = df_init ();

  ra_modified_insns = NULL;
  if (flag_ra_pre_reload)
    {
      ra_info = ra_info_init (max_reg_num ());
    }

  newra_in_progress = 1;

  /* This is the main loop, calling one_pass as long as there are still
     some spilled webs.  */
  do
    {
      ra_debug_msg (DUMP_NEARLY_EVER, "RegAlloc Pass %d\n\n", ra_pass);
      if (ra_pass++ > 40)
	internal_error ("Didn't find a coloring.\n");

      if (flag_ra_pre_reload)
	{
	  pre_reload (ra_info, ra_modified_insns);
	  if (rtl_dump_file && ra_pass == 1 && (debug_new_regalloc & DUMP_RTL))
	    {
	      ra_debug_msg (DUMP_NEARLY_EVER, "Original function:\n");
	      ra_print_rtl_with_bb (rtl_dump_file, get_insns ());
	      fflush (rtl_dump_file);
	    }
	}

      /* We don't use those NOTEs, and as we anyway change all registers,
	 they only make problems later.  But remove them _after_ the first
         pre_reload(), as that one can make use of those notes.  */
      if (ra_pass == 1)
	count_or_remove_death_notes (NULL, 1);

      if (!ra_modified_insns)
	ra_modified_insns = BITMAP_XMALLOC ();
      else
	bitmap_clear (ra_modified_insns);

      if (!insns_with_deaths)
	{
	  death_insns_max_uid = get_max_uid ();
	  insns_with_deaths = sbitmap_alloc (death_insns_max_uid);
	  sbitmap_ones (insns_with_deaths);
	}

      if (flag_ra_pre_reload)
	{
	  allocate_reg_info (max_reg_num (), FALSE, FALSE);
	  compute_bb_for_insn ();
	  reg_scan_update (get_insns (), NULL, max_regno);
	  max_regno = max_reg_num ();
	}

      /* First collect all the register refs and put them into
	 chains per insn, and per regno.  In later passes only update
	 that info from the new and modified insns.  */
      df_analyse (df, (ra_pass == 1) ? 0 : (bitmap) -1,
		  DF_HARD_REGS | DF_RD_CHAIN | DF_RU_CHAIN | DF_FOR_REGALLOC);

      if (flag_ra_pre_reload)
	df2ra = build_df2ra (df, ra_info);

      if ((debug_new_regalloc & DUMP_DF) != 0)
	{
	  rtx insn;
	  df_dump (df, DF_HARD_REGS, rtl_dump_file);
	  for (insn = get_insns (); insn; insn = NEXT_INSN (insn))
            if (INSN_P (insn))
	      df_insn_debug_regno (df, insn, rtl_dump_file);
	}
      check_df (df);

      /* Now allocate the memory needed for this pass, or (if it's not the
	 first pass), reallocate only additional memory.  */
      alloc_mem (df);
      /*ra_debug_msg (DUMP_EVER, "before one_pass()\n");
      if (rtl_dump_file)
	print_rtl_with_bb (rtl_dump_file, get_insns ());
      verify_flow_info ();*/

      detect_possible_mem_refs (df);
      /* Build and colorize the interference graph, and possibly emit
	 spill insns.  This also might delete certain move insns.  */
      changed = one_pass (df, ra_pass > 1);
      /*ra_debug_msg (DUMP_EVER, "after one_pass()\n");
      if (rtl_dump_file)
        print_rtl_with_bb (rtl_dump_file, get_insns ());
      verify_flow_info ();*/

      if (flag_ra_pre_reload)
	{
	  free (df2ra.def2def);
	  free (df2ra.use2use);
	}

      /* If that produced no changes, the graph was colorizable.  */
      if (!changed)
	{
	  /* Change the insns to refer to the new pseudos (one per web).  */
          emit_colors (df);
	  /* Already setup a preliminary reg_renumber[] array, but don't
	     free our own version.  reg_renumber[] will again be destroyed
	     later.  We right now need it in dump_constraints() for
	     constrain_operands(1) whose subproc sometimes reference
	     it (because we are checking strictly, i.e. as if
	     after reload).  */
	  setup_renumber (0);
	  /* Delete some more of the coalesced moves.  */
	  delete_moves ();
	  create_flow_barriers ();
	  dump_constraints ();
	}
      else
	{
	  /* If there were changes, this means spill code was added,
	     therefore repeat some things, including some initialization
	     of global data structures.  */
	  if ((debug_new_regalloc & DUMP_REGCLASS) == 0)
	    rtl_dump_file = NULL;
	  /* We have new pseudos (the stackwebs).  */
	  allocate_reg_info (max_reg_num (), FALSE, FALSE);
	  /* And new insns.  */
	  compute_bb_for_insn ();
	  /* Some of them might be dead.  */
	  /* XXX  When deleting such useless insns it can happen, that
	     it deletes a whole web.  But in contrast to delete_useless_defs()
	     this isn't yet handled correctly when incrementally rebuilding
	     the graph (cf. ggRotatingPinholeCamera.cc of 252.eon).  
	  delete_trivially_dead_insns_df (get_insns (), max_reg_num (), df);
	  */
	  /* Those new pseudos need to have their REFS count set.  */
	  reg_scan_update (get_insns (), NULL, max_regno);
	  max_regno = max_reg_num ();
	  /* And they need usefull classes too.  */
	  if (!flag_ra_pre_reload)
	    regclass (get_insns (), max_reg_num (), rtl_dump_file);
	  rtl_dump_file = ra_dump_file;
	  /* Remember the number of defs and uses, so we can distinguish
	     new from old refs in the next pass.  */
	  last_def_id = df->def_id;
	  last_use_id = df->use_id;
	}

      /* Output the graph, and possibly the current insn sequence.  */
      dump_ra (df);
      if (changed && (debug_new_regalloc & DUMP_RTL) != 0)
	{
	  ra_print_rtl_with_bb (rtl_dump_file, get_insns ());
	  fflush (rtl_dump_file);
	}

      /* Reset the web lists.  */
      reset_lists ();
      free_mem (df);
    }
  while (changed);

  if (ra_modified_insns)
    BITMAP_XFREE (ra_modified_insns);

  if (flag_ra_pre_reload)
    ra_info_free (ra_info);

  /* We are done with allocation, free all memory and output some
     debug info.  */
  free_all_mem (df);
  df_finish (df);
  if ((debug_new_regalloc & DUMP_RESULTS) == 0)
    dump_cost (DUMP_COSTS);
  ra_debug_msg (DUMP_COSTS, "ticks for build-phase: %ld\n", ticks_build);
  ra_debug_msg (DUMP_COSTS, "ticks for rebuild-phase: %ld\n", ticks_rebuild);
  if ((debug_new_regalloc & (DUMP_FINAL_RTL | DUMP_RTL)) != 0)
    ra_print_rtl_with_bb (rtl_dump_file, get_insns ());

  /* We might have new pseudos, so allocate the info arrays for them.  */
  if ((debug_new_regalloc & DUMP_SM) == 0)
    rtl_dump_file = NULL;
  no_new_pseudos = 0;
  allocate_reg_info (max_reg_num (), FALSE, FALSE);
  while_newra = 1;
  no_new_pseudos = 1;
  newra_in_progress = 0;
  rtl_dump_file = ra_dump_file;

    {
      edge e;
      for (e = EXIT_BLOCK_PTR->pred; e; e = e->pred_next)
	{
	  basic_block bb = e->src;
	  last = bb->end;
	  for (last = bb->end; last; last = PREV_INSN (last))
	    {
	      if (last == bb->head)
		break;
	      if (bitmap_bit_p (use_insns, INSN_UID (last)))
		delete_insn (last);
	    }
	}
    }
  BITMAP_XFREE (use_insns);
  rebuild_jump_labels (get_insns ());
  /* We might have deleted/moved dead stores, which could trap (mem accesses
     with flag_non_call_exceptions).  This might have made some edges dead.
     Get rid of them now.  No need to rebuild life info with that call,
     we do it anyway some statements below.  */
  purge_all_dead_edges (0);

  /* Some spill insns could've been inserted after trapping calls, i.e.
     at the end of a basic block, which really ends at that call.
     Fixup that breakages by adjusting basic block boundaries.  */
  fixup_abnormal_edges ();

  /* Cleanup the flow graph.  */
  if ((debug_new_regalloc & DUMP_LAST_FLOW) == 0)
    rtl_dump_file = NULL;
  life_analysis (get_insns (), rtl_dump_file,
		 PROP_DEATH_NOTES | PROP_LOG_LINKS  | PROP_REG_INFO);
  cleanup_cfg (CLEANUP_EXPENSIVE | CLEANUP_UPDATE_LIFE);
  recompute_reg_usage (get_insns (), TRUE);
  if (rtl_dump_file)
    dump_flow_info (rtl_dump_file);
  rtl_dump_file = ra_dump_file;

  /* update_equiv_regs() can't be called after register allocation.
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

  /* Setup the reg_renumber[] array for reload.  */
  setup_renumber (1);
  sbitmap_free (insns_with_deaths);

  /* And then delete the clobbers again, which were inserted just as
     flow barriers.  */
    {
      rtx insn, next;
      for (insn = get_insns (); insn; insn = next)
	{
	  next = NEXT_INSN (insn);
	  if (INSN_P (insn))
	    {
	      rtx pat = PATTERN (insn);
	      if (GET_CODE (pat) == CLOBBER
		  && REG_P (SET_DEST (pat))
		  && REGNO (SET_DEST (pat)) >= FIRST_PSEUDO_REGISTER)
		delete_insn_and_edges (insn);
	    }
	}
    }

  /* Build the insn chain before deleting some of the REG_DEAD notes.
     It initializes the chain->live_throughout bitmap, and when we delete
     some REG_DEAD we leave some pseudo in those bitmaps for insns, where
     they really are dead already.  This can confuse caller-save.  */
  build_insn_chain (get_insns ());
  /* Remove REG_DEAD notes which are incorrectly set.  See the docu
     of that function.  */
/*  remove_suspicious_death_notes ();*/

  /* Since we can color some webs the same without them being coalesced
     (e.g. by using the prefer_colors set), we have the same problem
     potentially for all webs, not just for those coalesced to hardregs.
     For now we need to remove _all_ REG_DEAD notes, not just those
     mentioned above.  */
#if 0
  count_or_remove_death_notes (NULL, 1);
  /* Bah.  No, that won't work.  Deleting all REG_DEAD notes constrains
     reload too much, and it can't sometimes find registers for reloads,
     e.g. by thinking some hardregs are live during an insn, when there
     aren't.  We use this frightening kludge to get around that.
     We first change everything into hardregs, add REG_DEAD notes for
     that representation, and finally change it back to pseudos.  This
     replaces sometimes pseudos back into the REG_DEAD notes and some-
     times not (in case it created a copy of the reg rtx for the note.
     But this doesn't matter, because reload itself is also replacing
     it with the hardregs again.  */
    {
      int i;
      for (i = FIRST_PSEUDO_REGISTER; i < max_regno; i++)
	if (regno_reg_rtx[i] && GET_CODE (regno_reg_rtx[i]) == REG)
	  REGNO (regno_reg_rtx[i])
	      = reg_renumber[i] >= 0 ? reg_renumber[i] : i;
      /*mark_regs_live_at_end (EXIT_BLOCK_PTR->global_live_at_start);
      update_life_info (NULL, UPDATE_LIFE_GLOBAL, PROP_DEATH_NOTES);*/
      life_analysis (get_insns (), rtl_dump_file, PROP_DEATH_NOTES);
      for (i = FIRST_PSEUDO_REGISTER; i < max_regno; i++)
	if (regno_reg_rtx[i] && GET_CODE (regno_reg_rtx[i]) == REG)
	  REGNO (regno_reg_rtx[i]) = i;
    }
  recompute_reg_usage (get_insns (), TRUE);
#endif

  if ((debug_new_regalloc & DUMP_LAST_RTL) != 0)
    ra_print_rtl_with_bb (rtl_dump_file, get_insns ());
  dump_static_insn_cost (rtl_dump_file,
			 "after allocation/spilling, before reload", NULL);

  /* Allocate the reg_equiv_memory_loc array for reload.  */
  reg_equiv_memory_loc = xcalloc (max_regno, sizeof (rtx));
  /* And possibly initialize it.  */
  allocate_initial_values (reg_equiv_memory_loc);
  /* And one last regclass pass just before reload.  */
  regclass (get_insns (), max_reg_num (), rtl_dump_file);
  BITMAP_XFREE (emitted_by_spill);
  BITMAP_XFREE (spill_slot_regs);
}

/*
vim:cinoptions={.5s,g0,p5,t0,(0,^-0.5s,n-0.5s:tw=78:cindent:sw=4:
*/
