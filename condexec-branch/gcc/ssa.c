/* Static Single Assignment conversion routines for the GNU compiler.
   Copyright (C) 2000 Free Software Foundation, Inc.

This file is part of GNU CC.

GNU CC is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 2, or (at your option) any
later version.

GNU CC is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GNU CC; see the file COPYING.  If not, write to the Free
Software Foundation, 59 Temple Place - Suite 330, Boston, MA
02111-1307, USA.  */

/* References:

   Building an Optimizing Compiler
   Robert Morgan
   Butterworth-Heinemann, 1998

   Static Single Assignment Construction
   Preston Briggs, Tim Harvey, Taylor Simpson
   Technical Report, Rice University, 1995
   ftp://ftp.cs.rice.edu/public/preston/optimizer/SSA.ps.gz
*/

#include "config.h"
#include "system.h"

#include "rtl.h"
#include "regs.h"
#include "hard-reg-set.h"
#include "flags.h"
#include "function.h"
#include "real.h"
#include "insn-config.h"
#include "recog.h"
#include "basic-block.h"
#include "output.h"
#include "partition.h"


/* TODO: 

   ??? What to do about strict_low_part.  Probably I'll have to split
   them out of their current instructions first thing.

   Actually the best solution may be to have a kind of "mid-level rtl"
   in which the RTL encodes exactly what we want, without exposing a
   lot of niggling processor details.  At some later point we lower
   the representation, calling back into optabs to finish any necessary
   expansion.
*/


/* Element I is the single instruction that sets register I+PSEUDO.  */
varray_type ssa_definition;

/* Element I is an INSN_LIST of instructions that use register I+PSEUDO.  */
varray_type ssa_uses;

/* Element I-PSEUDO is the normal register that originated the ssa
   register in question.  */
varray_type ssa_rename_from;

/* The running target ssa register for a given normal register.  */
static rtx *ssa_rename_to;

/* The number of registers that were live on entry to the SSA routines.  */
static unsigned int ssa_max_reg_num;

/* Local function prototypes.  */

static inline rtx * phi_alternative
  PARAMS ((rtx, int));

static int remove_phi_alternative
  PARAMS ((rtx, int));
static void simplify_to_immediate_dominators 
  PARAMS ((int *idom, sbitmap *dominators));
static void compute_dominance_frontiers_1
  PARAMS ((sbitmap *frontiers, int *idom, int bb, sbitmap done));
static void compute_dominance_frontiers
  PARAMS ((sbitmap *frontiers, int *idom));
static void find_evaluations_1
  PARAMS ((rtx dest, rtx set, void *data));
static void find_evaluations
  PARAMS ((sbitmap *evals, int nregs));
static void compute_iterated_dominance_frontiers
  PARAMS ((sbitmap *idfs, sbitmap *frontiers, sbitmap *evals, int nregs));
static void insert_phi_node
  PARAMS ((int regno, int b));
static void insert_phi_nodes
  PARAMS ((sbitmap *idfs, sbitmap *evals, int nregs));
static int rename_insn_1 
  PARAMS ((rtx *ptr, void *data));
static void rename_block 
  PARAMS ((int b, int *idom));
static void rename_registers 
  PARAMS ((int nregs, int *idom));

static inline int ephi_add_node
  PARAMS ((rtx reg, rtx *nodes, int *n_nodes));
static int * ephi_forward
  PARAMS ((int t, sbitmap visited, sbitmap *succ, int *tstack));
static void ephi_backward
  PARAMS ((int t, sbitmap visited, sbitmap *pred, rtx *nodes));
static void ephi_create
  PARAMS ((int t, sbitmap visited, sbitmap *pred, sbitmap *succ, rtx *nodes));
static void eliminate_phi
  PARAMS ((edge e, partition reg_partition));

static int make_regs_equivalent_over_bad_edges 
  PARAMS ((int bb, partition reg_partition));
static int make_equivalent_phi_alternatives_equivalent 
  PARAMS ((int bb, partition reg_partition));
static partition compute_conservative_reg_partition 
  PARAMS ((void));
static int rename_equivalent_regs_in_insn 
  PARAMS ((rtx *ptr, void *data));
static void rename_equivalent_regs 
  PARAMS ((partition reg_partition));


/* Determine if the insn is a PHI node.  */
#define PHI_NODE_P(X)				\
  (X && GET_CODE (X) == INSN			\
   && GET_CODE (PATTERN (X)) == SET		\
   && GET_CODE (SET_SRC (PATTERN (X))) == PHI)

/* Given the SET of a PHI node, return the address of the alternative
   for predecessor block C.  */

static inline rtx *
phi_alternative (set, c)
     rtx set;
     int c;
{
  rtvec phi_vec = XVEC (SET_SRC (set), 0);
  int v;

  for (v = GET_NUM_ELEM (phi_vec) - 2; v >= 0; v -= 2)
    if (INTVAL (RTVEC_ELT (phi_vec, v + 1)) == c)
      return &RTVEC_ELT (phi_vec, v);

  return NULL;
}

/* Given the SET of a phi node, remove the alternative for predecessor
   block C.  Return non-zero on success, or zero if no alternative is
   found for C.  */

static int
remove_phi_alternative (set, c)
     rtx set;
     int c;
{
  rtvec phi_vec = XVEC (SET_SRC (set), 0);
  int num_elem = GET_NUM_ELEM (phi_vec);
  int v;

  for (v = num_elem - 2; v >= 0; v -= 2)
    if (INTVAL (RTVEC_ELT (phi_vec, v + 1)) == c)
      {
	if (v < num_elem - 2)
	  {
	    RTVEC_ELT (phi_vec, v) = RTVEC_ELT (phi_vec, num_elem - 2);
	    RTVEC_ELT (phi_vec, v + 1) = RTVEC_ELT (phi_vec, num_elem - 1);
	  }
	PUT_NUM_ELEM (phi_vec, num_elem - 2);
	return 1;
      }

  return 0;
}

/* Computing the Immediate Dominators:

   Throughout, we don't actually want the full dominators set as
   calculated by flow, but rather the immediate dominators.
*/

static void
simplify_to_immediate_dominators (idom, dominators)
     int *idom;
     sbitmap *dominators;
{
  sbitmap *tmp;
  int b;

  tmp = sbitmap_vector_alloc (n_basic_blocks, n_basic_blocks);

  /* Begin with tmp(n) = dom(n) - { n }.  */
  for (b = n_basic_blocks; --b >= 0; )
    {
      sbitmap_copy (tmp[b], dominators[b]);
      RESET_BIT (tmp[b], b);
    }

  /* Subtract out all of our dominator's dominators.  */
  for (b = n_basic_blocks; --b >= 0; )
    {
      sbitmap tmp_b = tmp[b];
      int s;

      for (s = n_basic_blocks; --s >= 0; )
	if (TEST_BIT (tmp_b, s))
	  sbitmap_difference (tmp_b, tmp_b, tmp[s]);
    }

  /* Find the one bit set in the bitmap and put it in the output array.  */
  for (b = n_basic_blocks; --b >= 0; )
    {
      int t;
      EXECUTE_IF_SET_IN_SBITMAP (tmp[b], 0, t, { idom[b] = t; });
    }

  sbitmap_vector_free (tmp);
}


/* For all registers, find all blocks in which they are set.

   This is the transform of what would be local kill information that
   we ought to be getting from flow.  */

static sbitmap *fe_evals;
static int fe_current_bb;

static void
find_evaluations_1 (dest, set, data)
     rtx dest;
     rtx set ATTRIBUTE_UNUSED;
     void *data ATTRIBUTE_UNUSED;
{
  if (GET_CODE (dest) == REG
      && REGNO (dest) >= FIRST_PSEUDO_REGISTER)
    SET_BIT (fe_evals[REGNO (dest) - FIRST_PSEUDO_REGISTER], fe_current_bb);
}

static void
find_evaluations (evals, nregs)
     sbitmap *evals;
     int nregs;
{
  int bb;

  sbitmap_vector_zero (evals, nregs);
  fe_evals = evals;

  for (bb = n_basic_blocks; --bb >= 0; )
    {
      rtx p, last;

      fe_current_bb = bb;
      p = BLOCK_HEAD (bb);
      last = BLOCK_END (bb);
      while (1)
	{
	  if (GET_RTX_CLASS (GET_CODE (p)) == 'i')
	    note_stores (PATTERN (p), find_evaluations_1, NULL);

	  if (p == last)
	    break;
	  p = NEXT_INSN (p);
	}
    }
}


/* Computing the Dominance Frontier:
  
   As decribed in Morgan, section 3.5, this may be done simply by 
   walking the dominator tree bottom-up, computing the frontier for
   the children before the parent.  When considering a block B,
   there are two cases:

   (1) A flow graph edge leaving B that does not lead to a child
   of B in the dominator tree must be a block that is either equal
   to B or not dominated by B.  Such blocks belong in the frontier
   of B.

   (2) Consider a block X in the frontier of one of the children C
   of B.  If X is not equal to B and is not dominated by B, it
   is in the frontier of B.
*/

static void
compute_dominance_frontiers_1 (frontiers, idom, bb, done)
     sbitmap *frontiers;
     int *idom;
     int bb;
     sbitmap done;
{
  basic_block b = BASIC_BLOCK (bb);
  edge e;
  int c;

  SET_BIT (done, bb);
  sbitmap_zero (frontiers[bb]);

  /* Do the frontier of the children first.  Not all children in the
     dominator tree (blocks dominated by this one) are children in the
     CFG, so check all blocks.  */
  for (c = 0; c < n_basic_blocks; ++c)
    if (idom[c] == bb && ! TEST_BIT (done, c))
      compute_dominance_frontiers_1 (frontiers, idom, c, done);

  /* Find blocks conforming to rule (1) above.  */
  for (e = b->succ; e; e = e->succ_next)
    {
      if (e->dest == EXIT_BLOCK_PTR)
	continue;
      if (idom[e->dest->index] != bb)
	SET_BIT (frontiers[bb], e->dest->index);
    }

  /* Find blocks conforming to rule (2).  */
  for (c = 0; c < n_basic_blocks; ++c)
    if (idom[c] == bb)
      {
	int x;
	EXECUTE_IF_SET_IN_SBITMAP (frontiers[c], 0, x,
	  {
	    if (idom[x] != bb)
	      SET_BIT (frontiers[bb], x);
	  });
      }
}

static void
compute_dominance_frontiers (frontiers, idom)
     sbitmap *frontiers;
     int *idom;
{
  sbitmap done = sbitmap_alloc (n_basic_blocks);
  sbitmap_zero (done);

  compute_dominance_frontiers_1 (frontiers, idom, 0, done);

  sbitmap_free (done);
}


/* Computing the Iterated Dominance Frontier:

   This is the set of merge points for a given register.

   This is not particularly intuitive.  See section 7.1 of Morgan, in
   particular figures 7.3 and 7.4 and the immediately surrounding text.
*/

static void
compute_iterated_dominance_frontiers (idfs, frontiers, evals, nregs)
     sbitmap *idfs;
     sbitmap *frontiers;
     sbitmap *evals;
     int nregs;
{
  sbitmap worklist;
  int reg, passes = 0;

  worklist = sbitmap_alloc (n_basic_blocks);

  for (reg = 0; reg < nregs; ++reg)
    {
      sbitmap idf = idfs[reg];
      int b, changed;

      /* Start the iterative process by considering those blocks that
	 evaluate REG.  We'll add their dominance frontiers to the
	 IDF, and then consider the blocks we just added.  */
      sbitmap_copy (worklist, evals[reg]);

      /* Morgan's algorithm is incorrect here.  Blocks that evaluate
	 REG aren't necessarily in REG's IDF.  Start with an empty IDF.  */
      sbitmap_zero (idf);

      /* Iterate until the worklist is empty.  */
      do
	{
	  changed = 0;
	  passes++;
	  EXECUTE_IF_SET_IN_SBITMAP (worklist, 0, b,
	    {
	      RESET_BIT (worklist, b);
	      /* For each block on the worklist, add to the IDF all
		 blocks on its dominance frontier that aren't already
		 on the IDF.  Every block that's added is also added
		 to the worklist.  */
	      sbitmap_union_of_diff (worklist, worklist, frontiers[b], idf);
	      sbitmap_a_or_b (idf, idf, frontiers[b]);
	      changed = 1;
	    });
	}
      while (changed);
    }

  sbitmap_free (worklist);

  if (rtl_dump_file)
    {
      fprintf(rtl_dump_file,
	      "Iterated dominance frontier: %d passes on %d regs.\n",
	      passes, nregs);
    }
}


/* Insert the phi nodes.  */

static void
insert_phi_node (regno, bb)
     int regno, bb;
{
  basic_block b = BASIC_BLOCK (bb);
  edge e;
  int npred, i;
  rtvec vec;
  rtx phi, reg;

  /* Find out how many predecessors there are.  */
  for (e = b->pred, npred = 0; e; e = e->pred_next)
    if (e->src != ENTRY_BLOCK_PTR)
      npred++;

  /* If this block has no "interesting" preds, then there is nothing to
     do.  Consider a block that only has the entry block as a pred.  */
  if (npred == 0)
    return;

  /* This is the register to which the phi function will be assinged.  */
  reg = regno_reg_rtx[regno + FIRST_PSEUDO_REGISTER];

  /* Construct the arguments to the PHI node.  The use of pc_rtx is just
     a placeholder; we'll insert the proper value in rename_registers.  */
  vec = rtvec_alloc (npred * 2);
  for (e = b->pred, i = 0; e ; e = e->pred_next, i += 2)
    if (e->src != ENTRY_BLOCK_PTR)
      {
	RTVEC_ELT (vec, i + 0) = pc_rtx;
	RTVEC_ELT (vec, i + 1) = GEN_INT (e->src->index);
      }

  phi = gen_rtx_PHI (VOIDmode, vec);
  phi = gen_rtx_SET (VOIDmode, reg, phi);

  if (GET_CODE (b->head) == CODE_LABEL)
    emit_insn_after (phi, b->head);
  else
    b->head = emit_insn_before (phi, b->head);
}


static void
insert_phi_nodes (idfs, evals, nregs)
     sbitmap *idfs;
     sbitmap *evals ATTRIBUTE_UNUSED;
     int nregs;
{
  int reg;

  for (reg = 0; reg < nregs; ++reg)
    {
      int b;
      EXECUTE_IF_SET_IN_SBITMAP (idfs[reg], 0, b,
	{
	  if (REGNO_REG_SET_P (BASIC_BLOCK (b)->global_live_at_start, 
			       reg + FIRST_PSEUDO_REGISTER))
	    insert_phi_node (reg, b);
	});
    }
}

/* Rename the registers to conform to SSA. 

   This is essentially the algorithm presented in Figure 7.8 of Morgan,
   with a few changes to reduce pattern search time in favour of a bit
   more memory usage.  */


/* One of these is created for each set.  It will live in a list local
   to its basic block for the duration of that block's processing.  */
struct rename_set_data
{
  struct rename_set_data *next;
  rtx *reg_loc;
  rtx set_dest;
  rtx new_reg;
  rtx prev_reg;
};

static void new_registers_for_updates 
  PARAMS ((struct rename_set_data *set_data,
	   struct rename_set_data *old_set_data, rtx insn));

/* This is part of a rather ugly hack to allow the pre-ssa regno to be
   reused.  If, during processing, a register has not yet been touched,
   ssa_rename_to[regno] will be NULL.  Now, in the course of pushing
   and popping values from ssa_rename_to, when we would ordinarily 
   pop NULL back in, we pop RENAME_NO_RTX.  We treat this exactly the
   same as NULL, except that it signals that the original regno has
   already been reused.  */
#define RENAME_NO_RTX  pc_rtx

/* Part one of the first step of rename_block, called through for_each_rtx. 
   Mark pseudos that are set for later update.  Transform uses of pseudos.  */

static int
rename_insn_1 (ptr, data)
     rtx *ptr;
     void *data;
{
  rtx x = *ptr;
  struct rename_set_data **set_datap = data;

  if (x == NULL_RTX)
    return 0;

  switch (GET_CODE (x))
    {
    case SET:
      {
	rtx *destp = &SET_DEST (x);
	rtx dest = SET_DEST (x);

	/* Subregs at word 0 are interesting.  Subregs at word != 0 are
	   presumed to be part of a contiguous multi-word set sequence.  */
	while (GET_CODE (dest) == SUBREG
	       && SUBREG_WORD (dest) == 0)
	  {
	    destp = &SUBREG_REG (dest);
	    dest = SUBREG_REG (dest);
	  }

	if (GET_CODE (dest) == REG
	    && REGNO (dest) >= FIRST_PSEUDO_REGISTER)
	  {
	    /* We found a genuine set of an interesting register.  Tag
	       it so that we can create a new name for it after we finish
	       processing this insn.  */

	    struct rename_set_data *r;
	    r = (struct rename_set_data *) xmalloc (sizeof(*r));

	    r->reg_loc = destp;
	    r->set_dest = SET_DEST (x);
	    r->next = *set_datap;
	    *set_datap = r;

	    /* Since we do not wish to (directly) traverse the
	       SET_DEST, recurse through for_each_rtx for the SET_SRC
	       and return.  */
	    for_each_rtx (&SET_SRC (x), rename_insn_1, data);
	    return -1;
	  }

	/* Otherwise, this was not an interesting destination.  Continue
	   on, marking uses as normal.  */
	return 0;
      }

    case REG:
      if (REGNO (x) >= FIRST_PSEUDO_REGISTER
	  && REGNO (x) < ssa_max_reg_num)
	{
	  rtx new_reg = ssa_rename_to[REGNO(x) - FIRST_PSEUDO_REGISTER];

	  if (new_reg != NULL_RTX && new_reg != RENAME_NO_RTX)
	    {
	      if (GET_MODE (x) != GET_MODE (new_reg))
		abort ();
	      *ptr = new_reg;
	      /* ??? Mark for a new ssa_uses entry.  */
	    }
	  /* Else this is a use before a set.  Warn?  */
	}
      return -1;

    case PHI:
      /* Never muck with the phi.  We do that elsewhere, special-like.  */
      return -1;

    default:
      /* Anything else, continue traversing.  */
      return 0;
    }
}

/* Second part of the first step of rename_block.  The portion of the list
   beginning at SET_DATA through OLD_SET_DATA contain the sets present in
   INSN.  Update data structures accordingly.  */

static void
new_registers_for_updates (set_data, old_set_data, insn)
     struct rename_set_data *set_data, *old_set_data;
     rtx insn;
{
  while (set_data != old_set_data)
    {
      int regno, new_regno;
      rtx old_reg, new_reg, prev_reg;

      old_reg = *set_data->reg_loc;
      regno = REGNO (*set_data->reg_loc);

      /* For the first set we come across, reuse the original regno.  */
      if (ssa_rename_to[regno - FIRST_PSEUDO_REGISTER] == NULL_RTX)
	{
	  new_reg = old_reg;
	  prev_reg = RENAME_NO_RTX;
	}
      else
	{
	  prev_reg = ssa_rename_to[regno - FIRST_PSEUDO_REGISTER];
	  new_reg = gen_reg_rtx (GET_MODE (old_reg));
	}

      set_data->new_reg = new_reg;
      set_data->prev_reg = prev_reg;
      new_regno = REGNO (new_reg);
      ssa_rename_to[regno - FIRST_PSEUDO_REGISTER] = new_reg;

      if (new_regno >= (int) ssa_definition->num_elements)
	{
	  int new_limit = new_regno * 5 / 4;
	  ssa_definition = VARRAY_GROW (ssa_definition, new_limit);
	  ssa_uses = VARRAY_GROW (ssa_uses, new_limit);
	  ssa_rename_from = VARRAY_GROW (ssa_rename_from, new_limit);
	}

      VARRAY_RTX (ssa_definition, new_regno) = insn;
      VARRAY_RTX (ssa_rename_from, new_regno) = old_reg;

      set_data = set_data->next;
    }
}

static void
rename_block (bb, idom)
     int bb;
     int *idom;
{
  basic_block b = BASIC_BLOCK (bb);
  edge e;
  rtx insn, next, last;
  struct rename_set_data *set_data = NULL;
  int c;

  /* Step One: Walk the basic block, adding new names for sets and
     replacing uses.  */
     
  next = b->head;
  last = b->end;
  do
    {
      insn = next;
      if (GET_RTX_CLASS (GET_CODE (insn)) == 'i')
	{
	  struct rename_set_data *old_set_data = set_data;

	  for_each_rtx (&PATTERN (insn), rename_insn_1, &set_data);
	  for_each_rtx (&REG_NOTES (insn), rename_insn_1, &set_data);
	  
	  new_registers_for_updates (set_data, old_set_data, insn);
	}

      next = NEXT_INSN (insn);
    }
  while (insn != last);

  /* Step Two: Update the phi nodes of this block's successors.  */

  for (e = b->succ; e; e = e->succ_next)
    {
      if (e->dest == EXIT_BLOCK_PTR)
	continue;

      insn = e->dest->head;
      if (GET_CODE (insn) == CODE_LABEL)
	insn = NEXT_INSN (insn);

      while (PHI_NODE_P (insn))
	{
	  rtx phi = PATTERN (insn);
	  unsigned int regno;
	  rtx reg;

	  /* Find out which of our outgoing registers this node is
	     indended to replace.  Note that if this not the first PHI
	     node to have been created for this register, we have to
	     jump through rename links to figure out which register
	     we're talking about.  This can easily be recognized by
	     noting that the regno is new to this pass.  */
	  regno = REGNO (SET_DEST (phi));
	  if (regno >= ssa_max_reg_num)
	    regno = REGNO (VARRAY_RTX (ssa_rename_from, regno));
	  reg = ssa_rename_to[regno - FIRST_PSEUDO_REGISTER];

	  /* It is possible for the variable to be uninitialized on
	     edges in.  Reduce the arity of the PHI so that we don't
	     consider those edges.  */
	  if (reg == NULL || reg == RENAME_NO_RTX)
	    {
	      if (! remove_phi_alternative (phi, bb))
		abort ();
	    }
	  else
	    {
	      /* When we created the PHI nodes, we did not know what mode
	     the register should be.  Now that we've found an original,
	     we can fill that in.  */
	      if (GET_MODE (SET_DEST (phi)) == VOIDmode)
		PUT_MODE (SET_DEST (phi), GET_MODE (reg));
	      else if (GET_MODE (SET_DEST (phi)) != GET_MODE (reg))
		abort();

	      *phi_alternative (phi, bb) = reg;
	      /* ??? Mark for a new ssa_uses entry.  */
	    }

	  insn = NEXT_INSN (insn);
	}
    }

  /* Step Three: Do the same to the children of this block in
     dominator order.  */

  for (c = 0; c < n_basic_blocks; ++c)
    if (idom[c] == bb)
      rename_block (c, idom);

  /* Step Four: Update the sets to refer to their new register.  */

  while (set_data)
    {
      struct rename_set_data *next;
      rtx old_reg;

      old_reg = *set_data->reg_loc;
      *set_data->reg_loc = set_data->new_reg;
      ssa_rename_to[REGNO (old_reg)-FIRST_PSEUDO_REGISTER]
	= set_data->prev_reg;

      next = set_data->next;
      free (set_data);
      set_data = next;
    }      
}

static void
rename_registers (nregs, idom)
     int nregs;
     int *idom;
{
  VARRAY_RTX_INIT (ssa_definition, nregs * 3, "ssa_definition");
  VARRAY_RTX_INIT (ssa_uses, nregs * 3, "ssa_uses");
  VARRAY_RTX_INIT (ssa_rename_from, nregs * 3, "ssa_rename_from");

  ssa_rename_to = (rtx *) alloca (nregs * sizeof(rtx));
  bzero ((char *) ssa_rename_to, nregs * sizeof(rtx));

  rename_block (0, idom);

  /* ??? Update basic_block_live_at_start, and other flow info 
     as needed.  */

  ssa_rename_to = NULL;
}


/* The main entry point for moving to SSA.  */

void
convert_to_ssa()
{
  /* Element I is the set of blocks that set register I.  */
  sbitmap *evals;

  /* Dominator bitmaps.  */
  sbitmap *dominators;
  sbitmap *dfs;
  sbitmap *idfs;

  /* Element I is the immediate dominator of block I.  */
  int *idom;

  int nregs;

  find_basic_blocks (get_insns (), max_reg_num(), NULL);
  /* The dominator algorithms assume all blocks are reachable, clean
     up first.  */
  cleanup_cfg (get_insns ());
  life_analysis (get_insns (), max_reg_num (), NULL, 1);

  /* Compute dominators.  */
  dominators = sbitmap_vector_alloc (n_basic_blocks, n_basic_blocks);
  compute_flow_dominators (dominators, NULL);

  idom = (int *) alloca (n_basic_blocks * sizeof (int));
  memset ((void *)idom, -1, (size_t)n_basic_blocks * sizeof (int));
  simplify_to_immediate_dominators (idom, dominators);

  sbitmap_vector_free (dominators);

  if (rtl_dump_file)
    {
      int i;
      fputs (";; Immediate Dominators:\n", rtl_dump_file);
      for (i = 0; i < n_basic_blocks; ++i)
	fprintf (rtl_dump_file, ";\t%3d = %3d\n", i, idom[i]);
      fflush (rtl_dump_file);
    }

  /* Compute dominance frontiers.  */

  dfs = sbitmap_vector_alloc (n_basic_blocks, n_basic_blocks);
  compute_dominance_frontiers (dfs, idom);

  if (rtl_dump_file)
    {
      dump_sbitmap_vector (rtl_dump_file, ";; Dominance Frontiers:",
			   "; Basic Block", dfs, n_basic_blocks);
      fflush (rtl_dump_file);
    }

  /* Compute register evaluations.  */

  ssa_max_reg_num = max_reg_num();
  nregs = ssa_max_reg_num - FIRST_PSEUDO_REGISTER;
  evals = sbitmap_vector_alloc (nregs, n_basic_blocks);
  find_evaluations (evals, nregs);

  /* Compute the iterated dominance frontier for each register.  */

  idfs = sbitmap_vector_alloc (nregs, n_basic_blocks);
  compute_iterated_dominance_frontiers (idfs, dfs, evals, nregs);

  if (rtl_dump_file)
    {
      dump_sbitmap_vector (rtl_dump_file, ";; Iterated Dominance Frontiers:",
			   "; Register-FIRST_PSEUDO_REGISTER", idfs, nregs);
      fflush (rtl_dump_file);
    }

  /* Insert the phi nodes.  */

  insert_phi_nodes (idfs, evals, nregs);

  /* Rename the registers to satisfy SSA.  */

  rename_registers (nregs, idom);

  /* All done!  Clean up and go home.  */

  sbitmap_vector_free (dfs);
  sbitmap_vector_free (evals);
  sbitmap_vector_free (idfs);
}


/* This is intended to be the FIND of a UNION/FIND algorithm managing
   the partitioning of the pseudos.  Glancing through the rest of the
   global optimizations, it seems things will work out best if the
   partition is set up just before convert_from_ssa is called.  See
   section 11.4 of Morgan.

   ??? Morgan's algorithm, perhaps with some care, may allow copy
   propagation to happen concurrently with the conversion from SSA.

   However, it presents potential problems with critical edges -- to
   split or not to split.  He mentions beginning the partitioning by
   unioning registers associated by a PHI across abnormal critical
   edges.  This is the approache taken here.  It is unclear to me how
   we are able to do that arbitrarily, though.

   Alternately, Briggs presents an algorithm in which critical edges
   need not be split, at the expense of the creation of new pseudos,
   and the need for some concurrent register renaming.  Moreover, it
   is ameanable for modification such that the instructions can be
   placed anywhere in the target block, which solves the before-call
   placement problem.  However, I don't immediately see how we could
   do that concurrently with copy propoagation.

   More study is required.  */


/*
 * Eliminate the PHI across the edge from C to B.
 */

/* REG is the representative temporary of its partition.  Add it to the
   set of nodes to be processed, if it hasn't been already.  Return the
   index of this register in the node set.  */

static inline int
ephi_add_node (reg, nodes, n_nodes)
     rtx reg, *nodes;
     int *n_nodes;
{
  int i;
  for (i = *n_nodes - 1; i >= 0; --i)
    if (REGNO (reg) == REGNO (nodes[i]))
      return i;

  nodes[i = (*n_nodes)++] = reg;
  return i;
}

/* Part one of the topological sort.  This is a forward (downward) search
   through the graph collecting a stack of nodes to process.  Assuming no
   cycles, the nodes at top of the stack when we are finished will have
   no other dependancies.  */

static int *
ephi_forward (t, visited, succ, tstack)
     int t;
     sbitmap visited;
     sbitmap *succ;
     int *tstack;
{
  int s;

  SET_BIT (visited, t);

  EXECUTE_IF_SET_IN_SBITMAP (succ[t], 0, s,
    {
      if (! TEST_BIT (visited, s))
        tstack = ephi_forward (s, visited, succ, tstack);
    });

  *tstack++ = t;
  return tstack;
}

/* Part two of the topological sort.  The is a backward search through
   a cycle in the graph, copying the data forward as we go.  */

static void
ephi_backward (t, visited, pred, nodes)
     int t;
     sbitmap visited, *pred;
     rtx *nodes;
{
  int p;

  SET_BIT (visited, t);

  EXECUTE_IF_SET_IN_SBITMAP (pred[t], 0, p,
    {
      if (! TEST_BIT (visited, p))
	{
	  ephi_backward (p, visited, pred, nodes);
	  emit_move_insn (nodes[p], nodes[t]);
	}
    });
}

/* Part two of the topological sort.  Create the copy for a register
   and any cycle of which it is a member.  */

static void
ephi_create (t, visited, pred, succ, nodes)
     int t;
     sbitmap visited, *pred, *succ;
     rtx *nodes;
{
  rtx reg_u = NULL_RTX;
  int unvisited_predecessors = 0;
  int p;

  /* Iterate through the predecessor list looking for unvisited nodes.
     If there are any, we have a cycle, and must deal with that.  At 
     the same time, look for a visited predecessor.  If there is one,
     we won't need to create a temporary.  */

  EXECUTE_IF_SET_IN_SBITMAP (pred[t], 0, p,
    {
      if (! TEST_BIT (visited, p))
	unvisited_predecessors = 1;
      else if (!reg_u)
	reg_u = nodes[p];
    });

  if (unvisited_predecessors)
    {
      /* We found a cycle.  Copy out one element of the ring (if necessary),
	 then traverse the ring copying as we go.  */

      if (!reg_u)
	{
	  reg_u = gen_reg_rtx (GET_MODE (nodes[t]));
	  emit_move_insn (reg_u, nodes[t]);
	}

      EXECUTE_IF_SET_IN_SBITMAP (pred[t], 0, p,
	{
	  if (! TEST_BIT (visited, p))
	    {
	      ephi_backward (p, visited, pred, nodes);
	      emit_move_insn (nodes[p], reg_u);
	    }
	});
    }  
  else 
    {
      /* No cycle.  Just copy the value from a successor.  */

      int s;
      EXECUTE_IF_SET_IN_SBITMAP (succ[t], 0, s,
	{
	  SET_BIT (visited, t);
	  emit_move_insn (nodes[t], nodes[s]);
	  return;
	});
    }
}

/* Convert the edge to normal form.  */

static void
eliminate_phi (e, reg_partition)
     edge e;
     partition reg_partition;
{
  int n_nodes;
  sbitmap *pred, *succ;
  sbitmap visited;
  rtx *nodes;
  int *stack, *tstack;
  rtx insn;
  int i;

  /* Collect an upper bound on the number of registers needing processing.  */

  insn = e->dest->head;
  if (GET_CODE (insn) == CODE_LABEL)
    insn = next_nonnote_insn (insn);

  n_nodes = 0;
  while (PHI_NODE_P (insn))
    {
      insn = next_nonnote_insn (insn);
      n_nodes += 2;
    }

  if (n_nodes == 0)
    return;

  /* Build the auxilliary graph R(B). 

     The nodes of the graph are the members of the register partition
     present in Phi(B).  There is an edge from FIND(T0)->FIND(T1) for
     each T0 = PHI(...,T1,...), where T1 is for the edge from block C.  */

  nodes = (rtx *) alloca (n_nodes * sizeof(rtx));
  pred = sbitmap_vector_alloc (n_nodes, n_nodes);
  succ = sbitmap_vector_alloc (n_nodes, n_nodes);
  sbitmap_vector_zero (pred, n_nodes);
  sbitmap_vector_zero (succ, n_nodes);

  insn = e->dest->head;
  if (GET_CODE (insn) == CODE_LABEL)
    insn = next_nonnote_insn (insn);

  n_nodes = 0;
  for (; PHI_NODE_P (insn); insn = next_nonnote_insn (insn))
    {
      rtx* preg = phi_alternative (PATTERN (insn), e->src->index);
      rtx tgt = SET_DEST (PATTERN (insn));
      rtx reg;

      /* There may be no phi alternative corresponding to this edge.
	 This indicates that the phi variable is undefined along this
	 edge.  */
      if (preg == NULL)
	continue;
      reg = *preg;

      if (GET_CODE (reg) != REG || GET_CODE (tgt) != REG)
	abort();

      /* If the two registers are already in the same partition, 
	 nothing will need to be done.  */
      if (partition_find (reg_partition, REGNO (reg)) 
	  != partition_find (reg_partition, REGNO (tgt)))
	{
	  int ireg, itgt;

	  ireg = ephi_add_node (reg, nodes, &n_nodes);
	  itgt = ephi_add_node (tgt, nodes, &n_nodes);

	  SET_BIT (pred[ireg], itgt);
	  SET_BIT (succ[itgt], ireg);
	}
    }

  if (n_nodes == 0)
    goto out;

  /* Begin a topological sort of the graph.  */

  visited = sbitmap_alloc (n_nodes);
  sbitmap_zero (visited);

  tstack = stack = (int *) alloca (n_nodes * sizeof (int));

  for (i = 0; i < n_nodes; ++i)
    if (! TEST_BIT (visited, i))
      tstack = ephi_forward (i, visited, succ, tstack);

  sbitmap_zero (visited);

  /* As we find a solution to the tsort, collect the implementation 
     insns in a sequence.  */
  start_sequence ();
  
  while (tstack != stack)
    {
      i = *--tstack;
      if (! TEST_BIT (visited, i))
	ephi_create (i, visited, pred, succ, nodes);
    }

  insn = gen_sequence ();
  end_sequence ();
  insert_insn_on_edge (insn, e);
  if (rtl_dump_file)
    fprintf (rtl_dump_file, "Emitting copy on edge (%d,%d)\n",
	     e->src->index, e->dest->index);

  sbitmap_free (visited);
out:
  sbitmap_vector_free (pred);
  sbitmap_vector_free (succ);
}


/* For basic block B, consider all phi insns which provide an
   alternative corresponding to an incoming abnormal critical edge.
   Place the phi alternative corresponding to that abnormal critical
   edge in the same register class as the destination of the set.  

   From Morgan, p. 178:

     For each abnormal critical edge (C, B), 
     if T0 = phi (T1, ..., Ti, ..., Tm) is a phi node in B, 
     and C is the ith predecessor of B, 
     then T0 and Ti must be equivalent. 

   Return non-zero iff any such cases were found for which the two
   regs were not already in the same class.  */

static int
make_regs_equivalent_over_bad_edges (bb, reg_partition)
     int bb;
     partition reg_partition;
{
  int changed = 0;
  basic_block b = BASIC_BLOCK (bb);
  rtx phi = b->head;

  /* Advance to the first phi node.  */
  if (GET_CODE (phi) == CODE_LABEL)
    phi = next_nonnote_insn (phi);

  /* Scan all the phi nodes.  */
  for (; 
       PHI_NODE_P (phi);
       phi = next_nonnote_insn (phi))
    {
      edge e;
      int tgt_regno;
      rtx set = PATTERN (phi);
      rtx tgt = SET_DEST (set);

      /* The set target is expected to be a pseudo.  */
      if (GET_CODE (tgt) != REG 
	  || REGNO (tgt) < FIRST_PSEUDO_REGISTER)
	abort ();
      tgt_regno = REGNO (tgt);

      /* Scan incoming abnormal critical edges.  */
      for (e = b->pred; e; e = e->pred_next)
	if (e->flags & (EDGE_ABNORMAL | EDGE_CRITICAL))
	  {
	    rtx *alt = phi_alternative (set, e->src->index);
	    int alt_regno;

	    /* If there is no alternative corresponding to this edge,
	       the value is undefined along the edge, so just go on.  */
	    if (alt == 0)
	      continue;

	    /* The phi alternative is expected to be a pseudo.  */
	    if (GET_CODE (*alt) != REG 
		|| REGNO (*alt) < FIRST_PSEUDO_REGISTER)
	      abort ();
	    alt_regno = REGNO (*alt);

	    /* If the set destination and the phi alternative aren't
	       already in the same class...  */
	    if (partition_find (reg_partition, tgt_regno) 
		!= partition_find (reg_partition, alt_regno))
	      {
		/* ... make them such.  */
		partition_union (reg_partition, 
				 tgt_regno, alt_regno);
		++changed;
	      }
	  }
    }

  return changed;
}


/* Consider phi insns in basic block BB pairwise.  If the set target
   of both isns are equivalent pseudos, make the corresponding phi
   alternatives in each phi corresponding equivalent.

   Return nonzero if any new register classes were unioned.  */

static int
make_equivalent_phi_alternatives_equivalent (bb, reg_partition)
     int bb;
     partition reg_partition;
{
  int changed = 0;
  rtx phi = BLOCK_HEAD (bb);
  basic_block b = BASIC_BLOCK (bb);

  /* Advance to the first phi node.  */
  if (GET_CODE (phi) == CODE_LABEL)
    phi = next_nonnote_insn (phi);

  /* Scan all the phi nodes.  */
  for (; 
       PHI_NODE_P (phi);
       phi = next_nonnote_insn (phi))
    {
      rtx set = PATTERN (phi);
      /* The regno of the destination of the set.  */
      int tgt_regno = REGNO (SET_DEST (PATTERN (phi)));

      rtx phi2 = next_nonnote_insn (phi);

      /* Scan all phi nodes following this one.  */
      for (;
	   PHI_NODE_P (phi2);
	   phi2 = next_nonnote_insn (phi2))
	{
	  rtx set2 = PATTERN (phi2);
	  /* The regno of the destination of the set.  */
	  int tgt2_regno = REGNO (SET_DEST (set2));
		  
	  /* Are the set destinations equivalent regs?  */
	  if (partition_find (reg_partition, tgt_regno) ==
	      partition_find (reg_partition, tgt2_regno))
	    {
	      edge e;
	      /* Scan over edges.  */
	      for (e = b->pred; e; e = e->pred_next)
		{
		  int pred_block = e->src->index;
		  /* Identify the phi altnernatives from both phi
		     nodes corresponding to this edge.  */
		  rtx *alt = phi_alternative (set, pred_block);
		  rtx *alt2 = phi_alternative (set2, pred_block);

		  /* If one of the phi nodes doesn't have a
		     corresponding alternative, just skip it.  */
		  if (alt == 0 || alt2 == 0)
		    continue;

		  /* Both alternatives should be pseudos.  */
		  if (GET_CODE (*alt) != REG
		      || REGNO (*alt) < FIRST_PSEUDO_REGISTER)
		    abort ();
		  if (GET_CODE (*alt2) != REG
		      || REGNO (*alt2) < FIRST_PSEUDO_REGISTER)
		    abort ();

		  /* If the altneratives aren't already in the same
		     class ... */
		  if (partition_find (reg_partition, REGNO (*alt)) 
		      != partition_find (reg_partition, REGNO (*alt2)))
		    {
		      /* ... make them so.  */
		      partition_union (reg_partition, 
				       REGNO (*alt), REGNO (*alt2));
		      ++changed;
		    }
		}
	    }
	}
    }

  return changed;
}


/* Compute a conservative partition of outstanding pseudo registers.
   See Morgan 7.3.1.  */

static partition
compute_conservative_reg_partition ()
{
  int bb;
  int changed = 0;

  /* We don't actually work with hard registers, but it's easier to
     carry them around anyway rather than constantly doing register
     number arithmetic.  */
  partition p = 
    partition_new (ssa_definition->num_elements + FIRST_PSEUDO_REGISTER);

  /* The first priority is to make sure registers that might have to
     be copied on abnormal critical edges are placed in the same
     partition.  This saves us from having to split abnormal critical
     edges.  */
  for (bb = n_basic_blocks; --bb >= 0; )
    changed += make_regs_equivalent_over_bad_edges (bb, p);
  
  /* Now we have to insure that corresponding arguments of phi nodes
     assigning to corresponding regs are equivalent.  Iterate until
     nothing changes.  */
  while (changed > 0)
    {
      changed = 0;
      for (bb = n_basic_blocks; --bb >= 0; )
	changed += make_equivalent_phi_alternatives_equivalent (bb, p);
    }

  return p;
}


/* Rename regs in insn PTR that are equivalent.  DATA is the register
   partition which specifies equivalences.  */

static int
rename_equivalent_regs_in_insn (ptr, data)
     rtx *ptr;
     void* data;
{
  rtx x = *ptr;
  partition reg_partition = (partition) data;

  if (x == NULL_RTX)
    return 0;

  switch (GET_CODE (x))
    {
    case SET:
      {
	rtx *destp = &SET_DEST (x);
	rtx dest = SET_DEST (x);

	/* Subregs at word 0 are interesting.  Subregs at word != 0 are
	   presumed to be part of a contiguous multi-word set sequence.  */
	while (GET_CODE (dest) == SUBREG
	       && SUBREG_WORD (dest) == 0)
	  {
	    destp = &SUBREG_REG (dest);
	    dest = SUBREG_REG (dest);
	  }

	if (GET_CODE (dest) == REG
	    && REGNO (dest) >= FIRST_PSEUDO_REGISTER)
	  {
	    /* Got a pseudo; replace it.  */
	    int regno = REGNO (dest);
	    int new_regno = partition_find (reg_partition, regno);
	    if (regno != new_regno)
	      *destp = regno_reg_rtx [new_regno];

	    for_each_rtx (&SET_SRC (x), 
			  rename_equivalent_regs_in_insn, 
			  data);
	    return -1;
	  }

	/* Otherwise, this was not an interesting destination.  Continue
	   on, marking uses as normal.  */
	return 0;
      }

    case REG:
      if (REGNO (x) >= FIRST_PSEUDO_REGISTER)
	{
	  int regno = REGNO (x);
	  int new_regno = partition_find (reg_partition, regno);
	  if (regno != new_regno)
	    {
	      rtx new_reg = regno_reg_rtx[new_regno];
	      if (GET_MODE (x) != GET_MODE (new_reg))
		abort ();
	      *ptr = new_reg;
	    }
	}
      return -1;

    case PHI:
      /* No need to rename the phi nodes.  We'll check equivalence
	 when inserting copies.  */
      return -1;

    default:
      /* Anything else, continue traversing.  */
      return 0;
    }
}


/* Rename regs that are equivalent in REG_PARTITION.  */

static void
rename_equivalent_regs (reg_partition)
     partition reg_partition;
{
  int bb;

  for (bb = n_basic_blocks; --bb >= 0; )
    {
      basic_block b = BASIC_BLOCK (bb);
      rtx next = b->head;
      rtx last = b->end;
      rtx insn;

      do
	{
	  insn = next;
	  if (GET_RTX_CLASS (GET_CODE (insn)) == 'i')
	    {
	      for_each_rtx (&PATTERN (insn), 
			    rename_equivalent_regs_in_insn, 
			    reg_partition);
	      for_each_rtx (&REG_NOTES (insn), 
			    rename_equivalent_regs_in_insn, 
			    reg_partition);
	    }

	  next = NEXT_INSN (insn);
	}
      while (insn != last);
    }
}


/* The main entry point for moving from SSA.  */

void
convert_from_ssa()
{
  int bb;
  partition reg_partition;
  
  reg_partition = compute_conservative_reg_partition ();
  rename_equivalent_regs (reg_partition);

  /* Eliminate the PHI nodes.  */
  for (bb = n_basic_blocks; --bb >= 0; )
    {
      basic_block b = BASIC_BLOCK (bb);
      edge e;

      for (e = b->pred; e; e = e->pred_next)
	if (e->src != ENTRY_BLOCK_PTR)
	  eliminate_phi (e, reg_partition);
    }

  partition_delete (reg_partition);

  /* Actually delete the PHI nodes.  */
  for (bb = n_basic_blocks; --bb >= 0; )
    {
      rtx insn = BLOCK_HEAD (bb);
      int start = (GET_CODE (insn) != CODE_LABEL);

      if (! start)
	insn = next_nonnote_insn (insn);
      while (PHI_NODE_P (insn))
	{
	  insn = delete_insn (insn);
	  if (GET_CODE (insn) == NOTE)
	    insn = next_nonnote_insn (insn);
	}
      if (start)
	BLOCK_HEAD (bb) = insn;
    }

  /* Commit all the copy nodes needed to convert out of SSA form.  */
  commit_edge_insertions ();

  count_or_remove_death_notes (NULL, 1);
}
