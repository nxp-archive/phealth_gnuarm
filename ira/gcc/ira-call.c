/* Splitting ranges around calls for IRA.
   Copyright (C) 2006, 2007
   Free Software Foundation, Inc.
   Contributed by Vladimir Makarov <vmakarov@redhat.com>.

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
Software Foundation, 51 Franklin Street, Fifth Floor, Boston, MA
02110-1301, USA.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "hard-reg-set.h"
#include "rtl.h"
#include "tm_p.h"
#include "target.h"
#include "regs.h"
#include "flags.h"
#include "basic-block.h"
#include "toplev.h"
#include "expr.h"
#include "params.h"
#include "reload.h"
#include "df.h"
#include "output.h"
#include "ira-int.h"

/* The file is responsible for splitting the live range of
   pseudo-registers living through calls which are assigned to
   call-used hard-registers in two parts: one range (a new
   pseudo-register is created for this) which lives through the calls
   and another range (the original pseudo-register is used for the
   range) lives between the calls.  Memory is assigned to new
   pseudo-registers.  Move instructions connecting the two live ranges
   (the original and new pseudo-registers) will be transformed into
   load/store instructions in the reload pass.

   This file also does global save/restore code redundancy
   elimination.  It calculates points to put save/restore instructions
   according the following data flow equations:

   SaveOut(b) = intersect (SaveIn(p) - SaveIgnore(pb))
                for each p in pred(b)

                    | 0              if depth (b) <= depth (p)
   SaveIgnore(pb) = | 
                    | Ref(loop(b))   if depth (b) > depth (p)

   SaveIn(b) = (SaveOut(b) - Kill(b)) U SaveGen(b)

   RestoreIn(b) = intersect (RestoreOut(s) - RestoreIgnore(bs))
                  for each s in succ(b)

                       | 0           if depth (b) <= depth (s)
   RestoreIgnore(bs) = |
                       | Ref(loop(b))if depth (b) > depth (s)

   RestoreOut(b) = (RestoreIn(b) - Kill(b)) U RestoreGen(b)

   Here, Kill(b) is the set of allocnos referenced in basic block b
   and SaveGen(b) and RestoreGen(b) is the set of allocnos which
   should be correspondingly saved and restored in basic block b and
   which are not referenced correspondingly before the last and after
   the first calls they live through in basic block b.  SaveIn(b),
   SaveOut(b), RestoreIn(b), RestoreOut(b) are allocnos
   correspondingly to save and to restore at the start and the end of
   basic block b.  Save and restore code is not moved to more
   frequently executed points (inside loops).  The code can be moved
   through a loop unless it is referenced in the loop (this set of
   allocnos is denoted by Ref(loop)).

   We should put code to save/restore an allocno on an edge (p,s) if
   the allocno lives on the edge and the corresponding values of the
   sets at end of p and at the start of s are different.  In practice,
   code unification is done: if the save/restore code should be on all
   outgoing edges or all incoming edges, it is placed at the edge
   source and destination correspondingly.

   Putting live ranges living through calls into memory means that
   some conflicting pseudo-registers (such pseudo-registers should not
   live through calls) assigned to memory have a chance to be assigned
   to the corresponding call-used hard-register.  It is done by
   ira-color.c:reassign_conflict_allocnos using simple priority-based
   colouring for the conflicting pseudo-registers.  The bigger the
   live range of pseudo-register living through calls, the better such
   a chance is.  Therefore, we move spill/restore code as far as
   possible inside basic blocks.

   The implementation of save/restore code generation before the
   reload pass has several advantages:

     o simpler implementation of sharing stack slots used for spilled
       pseudos and for saving pseudo values around calls.  Actually,
       the same code for sharing stack slots allocated for pseudos is
       used in this case.

     o simpler implementation of moving save/restore code to increase
       the range of memory pseudo can be stored in.

     o simpler implementation of improving allocation by assigning
       hard-registers to spilled pseudos which conflict with new
       pseudos living through calls.

   The disadvantage of such an approach is mainly in the reload pass,
   whose behavior is hard to predict.  If the reload pass decides that
   the original pseudos should be spilled, save/restore code will be
   transformed into a memory-memory move.  To remove such nasty moves,
   IRA is trying to use the same stack slot for the two pseudos.  It
   is achieved using a standard preference technique to use the same
   stack slot for pseudos involved in moves.  A move between pseudos
   assigned to the same memory could be removed by post-reload
   optimizations, but it is implemented in the reload pass because, if
   it is not done earlier, a hard-register would be required for this
   and most probably a pseudo-register would be spilled by the reload
   to free the hard-register.

*/

/* ??? TODO: Abnormal edges  */
/* The following structure contains basic block data flow information
   used to calculate registers to save restore.  */
struct bb_info
{
  /* Local bb info: */
  /* Registers mentioned in the BB.  */
  bitmap kill;
  /* Registers needed to be saved and this save not killed (see above)
     by an insn in the BB before that.  */
  bitmap saveloc;
  /* Registers needed to be restored and this restore not killed by an
     insn in the BB after that.  */
  bitmap restoreloc;
  /* Global save and restore info.  */
  bitmap savein, saveout, restorein, restoreout;
};

/* Macros for accessing data flow information of basic blocks.  */
#define BB_INFO(BB) ((struct bb_info *) (BB)->aux)
#define BB_INFO_BY_INDEX(N) BB_INFO (BASIC_BLOCK(N))

/* DF infrastructure data.  */
static struct df_problem problem;
static struct dataflow dflow;

/* Basic blocks in postorder.  */
static int *postorder;
/* The size of the previous array.  */
static int n_blocks;
/* Bitmap of all basic blocks.  */
static bitmap current_all_blocks;

static rtx *reg_map;

/* Numbers of currently live pseudo-registers.  */
static bitmap regs_live;

/* Numbers of all registers which should be split around calls.  */
static bitmap regs_to_save_restore;

/* Bitmap used to collect numbers of referenced regs inside a rtx.  */
static bitmap referenced_regs;

/* Array of bitmaps for each loop node.  The bitmap contains numbers
   of registers mentioned int the corresponding loop (and all its
   subloops).  */
static bitmap *loop_referenced_regs_array;
/* The size of the previous array.  */
static int loop_referenced_regs_array_len;

/* Bitmaps used for saving intermediate results.  */
static bitmap temp_bitmap;
static bitmap temp_bitmap2;

/* Set of hard regs (except eliminable ones) currently live (during
   scan of all insns).  */
static HARD_REG_SET hard_regs_live;

/* Allocate and initialize data used for splitting allocnos around
   calls.  */
static void
init_ira_call_data (void)
{
  int i;

  postorder = ira_allocate (sizeof (int) * last_basic_block);
  current_all_blocks = ira_allocate_bitmap ();

  n_blocks = post_order_compute (postorder, true, false);

  if (n_blocks != n_basic_blocks)
    delete_unreachable_blocks ();

  alloc_aux_for_blocks (sizeof (struct bb_info));
  for (i = 0; i < n_blocks; i++)
    {
      struct bb_info *bb_info;

      bitmap_set_bit (current_all_blocks, postorder [i]);
      bb_info = BB_INFO_BY_INDEX (postorder [i]);
      bb_info->kill = ira_allocate_bitmap ();
      bb_info->saveloc = ira_allocate_bitmap ();
      bb_info->restoreloc = ira_allocate_bitmap ();
      bb_info->savein = ira_allocate_bitmap ();
      bb_info->saveout = ira_allocate_bitmap ();
      bb_info->restorein = ira_allocate_bitmap ();
      bb_info->restoreout = ira_allocate_bitmap ();
    }

  loop_referenced_regs_array_len = VEC_length (loop_p, ira_loops.larray);
  loop_referenced_regs_array
    = ira_allocate (loop_referenced_regs_array_len * sizeof (bitmap));
  for (i = 0; i < loop_referenced_regs_array_len; i++)
    loop_referenced_regs_array [i] = ira_allocate_bitmap ();

  memset (&problem, 0, sizeof (problem));
  memset (&dflow, 0, sizeof (dflow));
  dflow.problem= &problem;

  reg_map = ira_allocate (sizeof (rtx) * max_reg_num ());
  memset (reg_map, 0, sizeof (rtx) * max_reg_num ());
  regs_live = ira_allocate_bitmap ();
  referenced_regs = ira_allocate_bitmap ();
  regs_to_save_restore = ira_allocate_bitmap ();
  temp_bitmap = ira_allocate_bitmap ();
  temp_bitmap2 = ira_allocate_bitmap ();
}

/* Print bitmap B with TITLE to file F.  */
static void
print_bitmap (FILE *f, bitmap b, const char *title)
{
  unsigned int j;
  bitmap_iterator bi;

  fprintf (f, "%s:", title);
  EXECUTE_IF_SET_IN_BITMAP (b, FIRST_PSEUDO_REGISTER, j, bi)
    fprintf (f, " %d", j);
  fprintf (f, "\n");
}

/* Print data used for splitting allocnos around calls to file F.  */
static void
print_ira_call_data (FILE *f)
{
  int i;
  basic_block bb;

  print_bitmap (f, regs_to_save_restore, "to save/restore") ;
  for (i = 0; i < loop_referenced_regs_array_len; i++)
    {
      fprintf (f, "Loop %d -- ", i);
      print_bitmap (f, loop_referenced_regs_array [i], "referenced");
    }
  FOR_EACH_BB (bb)
    {
      struct bb_info *bb_info;

      bb_info = BB_INFO (bb);
      fprintf (f, "BB %d (loop %d)\n", bb->index, bb->loop_father->num);
      print_bitmap (f, bb_info->kill, "  kill") ;
      print_bitmap (f, bb_info->saveloc, "  saveloc") ;
      print_bitmap (f, bb_info->restoreloc, "  restoreloc") ;
      print_bitmap (f, bb_info->savein, "  savein") ;
      print_bitmap (f, bb_info->saveout, "  saveout") ;
      print_bitmap (f, bb_info->restorein, "  restorein") ;
      print_bitmap (f, bb_info->restoreout, "  restoreout") ;
    }
}

/* Print data used for splitting allocnos around calls to STDERR.  */
extern void
debug_ira_call_data (void)
{
  print_ira_call_data (stderr);
}

/* Finish data used for splitting allocnos around calls.  */
static void
finish_ira_call_data (void)
{
  int i;

  ira_free_bitmap (temp_bitmap2);
  ira_free_bitmap (temp_bitmap);
  ira_free_bitmap (regs_to_save_restore);
  ira_free_bitmap (referenced_regs);
  ira_free_bitmap (regs_live);
  ira_free (reg_map);
  for (i = 0; i < loop_referenced_regs_array_len; i++)
    ira_free_bitmap (loop_referenced_regs_array [i]);
  ira_free (loop_referenced_regs_array);
  for (i = 0; i < n_blocks; i++)
    {
      struct bb_info *bb_info;

      bb_info = BB_INFO_BY_INDEX (postorder [i]);
      ira_free_bitmap (bb_info->restoreout);
      ira_free_bitmap (bb_info->restorein);
      ira_free_bitmap (bb_info->saveout);
      ira_free_bitmap (bb_info->savein);
      ira_free_bitmap (bb_info->restoreloc);
      ira_free_bitmap (bb_info->saveloc);
      ira_free_bitmap (bb_info->kill);
    }
  free_aux_for_blocks ();
  ira_free_bitmap (current_all_blocks);
  ira_free (postorder);
}

/* TRUE if insns and new registers are created.  */
static int change_p;

/* Record all regs that are set in any one insn.  Communication from
   mark_reg_{store,clobber}.  */
static VEC(rtx, heap) *regs_set;

/* Handle the case where REG is set by the insn being scanned.  Store
   a 1 in hard_regs_live or regs_live for this register.

   REG might actually be something other than a register; if so, we do
   nothing.

   SETTER is 0 if this register was modified by an auto-increment
   (i.e., a REG_INC note was found for it).  */
static void
mark_reg_store (rtx reg, const_rtx setter ATTRIBUTE_UNUSED,
		void *data ATTRIBUTE_UNUSED)
{
  int regno;

  if (GET_CODE (reg) == SUBREG)
    reg = SUBREG_REG (reg);

  if (! REG_P (reg))
    return;

  VEC_safe_push (rtx, heap, regs_set, reg);

  regno = REGNO (reg);

  if (regno >= FIRST_PSEUDO_REGISTER)
    bitmap_set_bit (regs_live, regno);
  else if (! TEST_HARD_REG_BIT (no_alloc_regs, regno))
    {
      int last = regno + hard_regno_nregs [regno] [GET_MODE (reg)];

      while (regno < last)
	{
	  if (! TEST_HARD_REG_BIT (eliminable_regset, regno))
	    SET_HARD_REG_BIT (hard_regs_live, regno);
	  regno++;
	}
    }
}

/* Like mark_reg_store except notice just CLOBBERs; ignore SETs.  */
static void
mark_reg_clobber (rtx reg, const_rtx setter, void *data)
{
  if (GET_CODE (setter) == CLOBBER)
    mark_reg_store (reg, setter, data);
}

/* Mark REG as being dead (following the insn being scanned now).
   Store a 0 in hard_regs_live or regs_live for this register.  */
static void
mark_reg_death (rtx reg)
{
  int regno = REGNO (reg);

  if (regno >= FIRST_PSEUDO_REGISTER)
    bitmap_clear_bit (regs_live, regno);
  else if (! TEST_HARD_REG_BIT (no_alloc_regs, regno))
    {
      int last = regno + hard_regno_nregs [regno] [GET_MODE (reg)];

      while (regno < last)
	{
	  CLEAR_HARD_REG_BIT (hard_regs_live, regno);
	  regno++;
	}
    }
}

/* The recursive function walks X and records all referenced registers
   in REFERENCED_REGS.  */
static void
mark_referenced_regs (rtx x)
{
  enum rtx_code code;
  const char *fmt;
  int i, j;

  if (x == NULL_RTX)
    return;
  code = GET_CODE (x);
  if (code == SET)
    mark_referenced_regs (SET_SRC (x));
  if (code == SET || code == CLOBBER)
    {
      x = SET_DEST (x);
      code = GET_CODE (x);
      if ((code == REG && REGNO (x) < FIRST_PSEUDO_REGISTER)
	  || code == PC || code == CC0)
	return;
    }
  if (code == MEM || code == SUBREG)
    {
      x = XEXP (x, 0);
      code = GET_CODE (x);
    }

  if (code == REG)
    {
      int regno = REGNO (x);

      bitmap_set_bit (referenced_regs, regno);
      return;
    }

  fmt = GET_RTX_FORMAT (code);
  for (i = GET_RTX_LENGTH (code) - 1; i >= 0; i--)
    {
      if (fmt[i] == 'e')
	mark_referenced_regs (XEXP (x, i));
      else if (fmt[i] == 'E')
	for (j = XVECLEN (x, i) - 1; j >= 0; j--)
	  mark_referenced_regs (XVECEXP (x, i, j));
    }
}

/* The function sets up REFERENCED_REGS for rtx X and all their
   equivalences.  */
static void
mark_all_referenced_regs (rtx x)
{
  rtx list, note;
  unsigned int j;
  bitmap_iterator bi;

  mark_referenced_regs (x);
  bitmap_copy (temp_bitmap, referenced_regs);
  bitmap_copy (temp_bitmap2, referenced_regs);
  for (;;)
    {
      bitmap_clear (referenced_regs);
      EXECUTE_IF_SET_IN_BITMAP (temp_bitmap2, FIRST_PSEUDO_REGISTER, j, bi)
	if (j < (unsigned) ira_max_regno_before)
	  for (list = reg_equiv_init [j];
	       list != NULL_RTX;
	       list = XEXP (list, 1))
	    {
	      note = find_reg_note (XEXP (list, 0), REG_EQUIV, NULL_RTX);
	      
	      if (note == NULL_RTX)
		continue;
	      
	      mark_referenced_regs (XEXP (note, 0));
	    }
      bitmap_and_compl (temp_bitmap2, temp_bitmap, referenced_regs);
      if (! bitmap_ior_into (temp_bitmap, referenced_regs))
	break;
    }
  bitmap_copy (referenced_regs, temp_bitmap);
}

/* The function emits a new save/restore insn with pattern PATH before
   (if BEFORE_P) or after INSN.  */
static void
insert_one_insn (rtx insn, int before_p, rtx pat)
{
  rtx new_insn;

  change_p = TRUE;
#ifdef HAVE_cc0
  /* If INSN references CC0, put our insns in front of the insn that
     sets CC0.  This is always safe, since the only way we could be
     passed an insn that references CC0 is for a restore, and doing a
     restore earlier isn't a problem.  We do, however, assume here
     that CALL_INSNs don't reference CC0.  Guard against non-INSN's
     like CODE_LABEL.  */

  if ((NONJUMP_INSN_P (insn) || JUMP_P (insn))
      && before_p && reg_referenced_p (cc0_rtx, PATTERN (insn)))
    insn = PREV_INSN (insn);
#endif

  if (before_p)
    {
      new_insn = emit_insn_before (pat, insn);
      if (insn == BB_HEAD (BLOCK_FOR_INSN (insn)))
	BB_HEAD (BLOCK_FOR_INSN (insn)) = new_insn;
    }
  else
    {
      if (GET_CODE (insn) != CODE_LABEL)
	new_insn = emit_insn_after (pat, insn);
      else
	{
	  /* Put the insn after bb note in a empty basic block.  */
	  gcc_assert (NEXT_INSN (insn) && NOTE_P (NEXT_INSN (insn)));
	  new_insn = emit_insn_after (pat, NEXT_INSN (insn));
	}
      if (insn == BB_END (BLOCK_FOR_INSN (insn)))
	BB_END (BLOCK_FOR_INSN (insn)) = new_insn;
    }
  if (ira_dump_file != NULL)
    fprintf (ira_dump_file,
	     "Generating save/restore insn %d:%d<-%d in bb %d\n",
	     INSN_UID (new_insn), REGNO (SET_DEST (pat)),
	     REGNO (SET_SRC (pat)), BLOCK_FOR_INSN (insn)->index);
}

/* The function creates a new register (if it is not created yet) and
   returns it for allocno with REGNO.  */
static rtx
get_new_reg (unsigned int regno)
{
  rtx reg, newreg;

  reg = regno_reg_rtx [regno];
  if ((newreg = reg_map [regno]) == NULL_RTX)
    {
      newreg = gen_reg_rtx (GET_MODE (reg));
      REG_USERVAR_P (newreg) = REG_USERVAR_P (reg);
      REG_POINTER (newreg) = REG_POINTER (reg);
      REG_ATTRS (newreg) = REG_ATTRS (reg);
      reg_map [regno] = newreg;
    }
  return newreg;
}

/* Return move insn dest<-src.  */
static rtx
get_move_insn (rtx src, rtx dest) 
{
  rtx result;

  start_sequence ();
  emit_move_insn (src, dest);
  result = get_insns ();
  end_sequence ();
  return result;
}

/* The function inserts save/restore code which can be placed in any
   case inside the BB and caclulate local bb info (kill, saveloc,
   restoreloc).  */
static void
put_save_restore_and_calculate_local_info (void)
{
  int i;
  unsigned int j;
  basic_block bb;
  rtx insn, reg, newreg, pat;
  bitmap_iterator bi;
  bitmap reg_live_in, reg_live_out, saveloc, restoreloc, kill;

  /* Make a vector that mark_reg_{store,clobber} will store in.  */
  if (! regs_set)
    regs_set = VEC_alloc (rtx, heap, 10);
  FOR_EACH_BB (bb)
    {
      rtx first_insn, last_insn;
      struct loop *loop;
      struct bb_info *bb_info = BB_INFO (bb);

      saveloc = bb_info->saveloc;
      restoreloc = bb_info->restoreloc;
      kill = bb_info->kill;
      reg_live_in = DF_LR_IN (bb);
      reg_live_out = DF_LR_OUT (bb);
      REG_SET_TO_HARD_REG_SET (hard_regs_live, reg_live_in);
      AND_COMPL_HARD_REG_SET (hard_regs_live, eliminable_regset);
      bitmap_copy (regs_live, reg_live_in);
      first_insn = last_insn = NULL_RTX;
      /* Scan the code of this basic block, noting which regs and hard
	 regs are born or die.  */
      FOR_BB_INSNS (bb, insn)
	{
	  rtx link;
	  
	  if (! INSN_P (insn))
	    continue;
	  
	  if (first_insn == NULL_RTX)
	    first_insn = insn;
	  last_insn = insn;

	  bitmap_clear (referenced_regs);
	  mark_all_referenced_regs (insn);
	  
	  EXECUTE_IF_SET_IN_BITMAP (restoreloc, FIRST_PSEUDO_REGISTER, j, bi)
	    if (bitmap_bit_p (referenced_regs, j))
	      insert_one_insn (insn, TRUE,
			       get_move_insn (regno_reg_rtx [j],
					      get_new_reg (j)));
	  
	  bitmap_ior_into (kill, referenced_regs);
	  bitmap_and_compl_into (restoreloc, referenced_regs);

	  /* Check that regs_set is an empty set.  */
	  gcc_assert (VEC_empty (rtx, regs_set));
      
	  /* Mark any regs clobbered by INSN as live, so they
	     conflict with the inputs.  */
	  note_stores (PATTERN (insn), mark_reg_clobber, NULL);
	  
	  /* Mark any regs dead after INSN as dead now.  */
	  for (link = REG_NOTES (insn); link; link = XEXP (link, 1))
	    if (REG_NOTE_KIND (link) == REG_DEAD)
	      mark_reg_death (XEXP (link, 0));
	  
	  if (CALL_P (insn) && ! find_reg_note (insn, REG_NORETURN, NULL))
	    {
	      EXECUTE_IF_SET_IN_BITMAP (regs_live, FIRST_PSEUDO_REGISTER,
					j, bi)
		if ((j >= (unsigned) ira_max_regno_before
		     || (reg_equiv_const [j] == NULL_RTX
			 && ! reg_equiv_invariant_p [j]))
		    && reg_renumber [j] >= 0
		    && ! hard_reg_not_in_set_p (reg_renumber [j],
						GET_MODE (regno_reg_rtx [j]),
						call_used_reg_set))
		  {
		    bitmap_set_bit (regs_to_save_restore, j);
		    if (! bitmap_bit_p (restoreloc, j)
			&& bitmap_bit_p (kill, j))
		      {
			for (i = 0; i < (int) VEC_length (rtx, regs_set); i++)
			  if (REGNO (VEC_index (rtx, regs_set, i)) == j)
			    break;
			if (i < (int) VEC_length (rtx, regs_set))
			  continue;
			/* Insert save insn */
			reg = regno_reg_rtx [j];
			newreg = get_new_reg (j);
			pat = get_move_insn (newreg, reg);
			insert_one_insn (insn, TRUE, pat);
		      }
		    if (! bitmap_bit_p (kill, j))
		      bitmap_set_bit (saveloc, j);
		    bitmap_set_bit (restoreloc, j);
		  }
	    }
	  
	  /* Mark any regs set in INSN as live.  */
	  note_stores (PATTERN (insn), mark_reg_store, NULL);
	  
#ifdef AUTO_INC_DEC
	  for (link = REG_NOTES (insn); link; link = XEXP (link, 1))
	    if (REG_NOTE_KIND (link) == REG_INC)
	      mark_reg_store (XEXP (link, 0), NULL_RTX, NULL);
#endif
	  
	  /* Mark any regs set in INSN and then never used.  */
	  while (! VEC_empty (rtx, regs_set))
	    {
	      rtx reg = VEC_pop (rtx, regs_set);
	      rtx note = find_regno_note (insn, REG_UNUSED, REGNO (reg));

	      if (note)
		mark_reg_death (XEXP (note, 0));
	    }
	}
      if (! flag_ira_move_spills)
	{
	  EXECUTE_IF_SET_IN_BITMAP (saveloc, FIRST_PSEUDO_REGISTER, j, bi)
	    insert_one_insn (first_insn, TRUE,
			     get_move_insn (get_new_reg (j),
					    regno_reg_rtx [j]));
	  EXECUTE_IF_SET_IN_BITMAP (restoreloc, FIRST_PSEUDO_REGISTER, j, bi)
	    insert_one_insn (last_insn, JUMP_P (last_insn),
			     get_move_insn (regno_reg_rtx [j],
					    get_new_reg (j)));
	}
      for (loop = bb->loop_father; loop != NULL; loop = loop_outer (loop))
	bitmap_ior_into (loop_referenced_regs_array [loop->num], kill);
    }
}



/* The function used by the DF equation solver to propagate restore
   info through block with BB_INDEX.  */
static bool
save_trans_fun (int bb_index)
{
  struct bb_info *bb_info = BB_INFO_BY_INDEX (bb_index);
  bitmap in = bb_info->savein;
  bitmap out = bb_info->saveout;
  bitmap loc = bb_info->saveloc;
  bitmap kill = bb_info->kill;

  return bitmap_ior_and_compl (in, loc, out, kill);
}

/* The function used by the DF equation solver to set up save info
   for a block BB without successors.  */
static void
save_con_fun_0 (basic_block bb)
{
  bitmap_clear (BB_INFO (bb)->saveout);
}

/* The function used by the DF equation solver to propagate save info
   from successor to predecessor on edge E.  */
static void
save_con_fun_n (edge e)
{
  bitmap op1 = BB_INFO (e->src)->saveout;
  bitmap op2 = BB_INFO (e->dest)->savein;

  if (e->src->loop_depth > e->dest->loop_depth)
    {
      bitmap_and_into (op1, op2);
      bitmap_and_compl_into
	(op1, loop_referenced_regs_array [e->src->loop_father->num]);
    }
  else
    bitmap_and_into (op1, op2);
}

/* The function calculates savein/saveout sets.  */
static void
calculate_save (void)
{
  basic_block bb;

  /* Initialize relations to find maximal solution.  */
  FOR_ALL_BB (bb)
    {
      bitmap_copy (BB_INFO (bb)->savein, regs_to_save_restore);
      bitmap_copy (BB_INFO (bb)->saveout, regs_to_save_restore);
    }
  df_simple_dataflow (DF_BACKWARD, NULL, save_con_fun_0, save_con_fun_n,
		      save_trans_fun, current_all_blocks,
		      df_get_postorder (DF_BACKWARD),
		      df_get_n_blocks (DF_BACKWARD));
}



/* The function used by the DF equation solver to propagate restore
   info through block with BB_INDEX.  */
static bool
restore_trans_fun (int bb_index)
{
  struct bb_info *bb_info = BB_INFO_BY_INDEX (bb_index);
  bitmap in = bb_info->restorein;
  bitmap out = bb_info->restoreout;
  bitmap loc = bb_info->restoreloc;
  bitmap kill = bb_info->kill;

  return bitmap_ior_and_compl (out, loc, in, kill);
}

/* The function used by the DF equation solver to set up restore info
   for a block BB without predecessors.  */
static void
restore_con_fun_0 (basic_block bb)
{
  bitmap_clear (BB_INFO (bb)->restorein);
}

/* The function used by the DF equation solver to propagate restore
   info from predecessor to successor on edge E.  */
static void
restore_con_fun_n (edge e)
{
  bitmap op1 = BB_INFO (e->dest)->restorein;
  bitmap op2 = BB_INFO (e->src)->restoreout;

  if (e->dest->loop_depth > e->src->loop_depth)
    {
      bitmap_and_into (op1, op2);
      bitmap_and_compl_into
	(op1, loop_referenced_regs_array [e->dest->loop_father->num]);
    }
  else
    bitmap_and_into (op1, op2);
}

/* The function calculates restorein/restoreout sets.  */
static void
calculate_restore (void)
{
  basic_block bb;

  /* Initialize relations to find maximal solution.  */
  FOR_ALL_BB (bb)
    {
      bitmap_copy (BB_INFO (bb)->restoreout, regs_to_save_restore);
      bitmap_copy (BB_INFO (bb)->restorein, regs_to_save_restore);
    }
  df_simple_dataflow (DF_FORWARD, NULL, restore_con_fun_0, restore_con_fun_n,
		      restore_trans_fun, current_all_blocks,
		      df_get_postorder (DF_FORWARD),
		      df_get_n_blocks (DF_FORWARD));
}



/* The function put save/restores insn according to the calculated
   equation.  The function even puts the insns inside blocks to
   increase live range of allocnos which will be in memory.  */
static void
put_save_restore (void)
{
  basic_block bb;
  edge e;
  edge_iterator ei;
  unsigned int j;
  bitmap_iterator bi;
  rtx pat;
  int first_p;
  bitmap save_at_end, restore_at_start, progress;

  save_at_end = ira_allocate_bitmap ();
  restore_at_start = ira_allocate_bitmap ();
  progress = ira_allocate_bitmap ();
  FOR_EACH_BB (bb)
    {
      struct bb_info *bb_info = BB_INFO (bb);
      bitmap kill = bb_info->kill;
      bitmap restoreout = bb_info->restoreout;
      bitmap saveout = bb_info->saveout;
      bitmap savein = bb_info->savein;
      bitmap restorein = bb_info->restorein;
      bitmap live_at_start;
      rtx bb_head, bb_end;
      rtx insn, next_insn;

      for (bb_head = BB_HEAD (bb);
	   bb_head != BB_END (bb) && ! INSN_P (bb_head);
	   bb_head = NEXT_INSN (bb_head))
	;
      for (bb_end = BB_END (bb);
	   bb_end != BB_HEAD (bb) && ! INSN_P (bb_end);
	   bb_end = PREV_INSN (bb_end))
	;

      bitmap_clear (save_at_end);
      first_p = TRUE;
      FOR_EACH_EDGE (e, ei, bb->succs)
	{
	  bitmap savein = BB_INFO (e->dest)->savein;

	  live_at_start = DF_LR_IN (e->dest);
	  /* (savein - restoreout) ^ (kill U ! saveout) ==
              ^ live_at_start ==
	     (savein - restoreout) ^ live_at_start ^ kill
             U (savein - restoreout) ^ live_at_start - saveout
	  */
	  bitmap_and_compl (temp_bitmap2, savein, restoreout);
	  bitmap_and_into (temp_bitmap2, live_at_start);
	  bitmap_and (temp_bitmap, temp_bitmap2, kill);
	  bitmap_ior_and_compl_into (temp_bitmap, temp_bitmap2, saveout);
	  if (first_p)
	    {
	      bitmap_copy (save_at_end, temp_bitmap);
	      first_p = FALSE;
	    }
	  else
	    bitmap_and_into (save_at_end, temp_bitmap);
	}

      bitmap_copy (progress, save_at_end);
      for (insn = BB_END (bb);
	   insn != PREV_INSN (BB_HEAD (bb));
	   insn = next_insn)
	{
	  next_insn = PREV_INSN (insn);
	  if (! INSN_P (insn))
	    continue;
	  bitmap_clear (referenced_regs);
	  mark_all_referenced_regs (insn);
	  EXECUTE_IF_SET_IN_BITMAP (referenced_regs, FIRST_PSEUDO_REGISTER,
				    j, bi)
	    if (bitmap_bit_p (progress, j))
	      {
		pat = get_move_insn (get_new_reg (j), regno_reg_rtx [j]);
		insert_one_insn (insn, JUMP_P (insn), pat);
		bitmap_clear_bit (progress, j);
	      }
	}
      /* When we don't move info inside the loop, progress can
	 be not empty here.  */
      EXECUTE_IF_SET_IN_BITMAP (progress, FIRST_PSEUDO_REGISTER, j, bi)
	{
	  pat = get_move_insn (get_new_reg (j), regno_reg_rtx [j]);
	  insert_one_insn (bb_head, TRUE, pat);
	}

      bitmap_clear (restore_at_start);
      first_p = TRUE;
      live_at_start = DF_LR_IN (bb);
      FOR_EACH_EDGE (e, ei, bb->preds)
	{
	  bitmap restoreout = BB_INFO (e->src)->restoreout;

	  /* (restoreout - savein) ^ (kill U ! restorein)
              ^ live_at_start ==
	     (((restoreout - savein) ^ live_at_start) ^ kill
              U ((restoreout - savein) ^ live_at_start) - restorein)
	  */
	  bitmap_and_compl (temp_bitmap2, restoreout, savein);
	  bitmap_and_into (temp_bitmap2, live_at_start);
	  bitmap_and (temp_bitmap, temp_bitmap2, kill);
	  bitmap_ior_and_compl_into (temp_bitmap, temp_bitmap2, restorein);
	  if (first_p)
	    {
	      bitmap_copy (restore_at_start, temp_bitmap);
	      first_p = FALSE;
	    }
	  else
	    bitmap_and_into (restore_at_start, temp_bitmap);
	}
	    
      bitmap_copy (progress, restore_at_start);
      for (insn = BB_HEAD (bb);
	   insn != NEXT_INSN (BB_END (bb));
	   insn = next_insn)
	{
	  next_insn = NEXT_INSN (insn);
	  if (! INSN_P (insn))
	    continue;
	  bitmap_clear (referenced_regs);
	  mark_all_referenced_regs (insn);
	  EXECUTE_IF_SET_IN_BITMAP (referenced_regs, FIRST_PSEUDO_REGISTER,
				    j, bi)
	    if (bitmap_bit_p (progress, j))
	      {
		pat = get_move_insn (regno_reg_rtx [j], get_new_reg (j));
		insert_one_insn (insn, TRUE, pat);
		bitmap_clear_bit (progress, j);
	      }
	}
      /* When we don't move info inside the loop, progress can
	 be not empty here.  */
      EXECUTE_IF_SET_IN_BITMAP (progress, FIRST_PSEUDO_REGISTER, j, bi)
	{
	  pat = get_move_insn (regno_reg_rtx [j], get_new_reg (j));
	  insert_one_insn (bb_end, JUMP_P (bb_end), pat);
	}
	
      FOR_EACH_EDGE (e, ei, bb->succs)
	{
	  bitmap savein = BB_INFO (e->dest)->savein;

	  live_at_start = DF_LR_IN (e->dest);
	  EXECUTE_IF_SET_IN_BITMAP (savein, FIRST_PSEUDO_REGISTER, j, bi)
	    if (! bitmap_bit_p (restoreout, j)
		&& (bitmap_bit_p (kill, j)
		    || ! bitmap_bit_p (saveout, j))
		&& ! bitmap_bit_p (save_at_end, j)
		&& bitmap_bit_p (live_at_start, j))
	      {
		pat = get_move_insn (get_new_reg (j),
				   regno_reg_rtx [j]);
		insert_insn_on_edge (pat, e);
		change_p = TRUE;
		if (ira_dump_file != NULL)
		  fprintf
		    (ira_dump_file,
		     "Generating save/restore insn %d<-%d on edge %d->%d\n",
		     REGNO (SET_DEST (pat)), REGNO (SET_SRC (pat)),
		     e->src->index, e->dest->index);
	      }
	}

      live_at_start = DF_LR_IN (bb);
      FOR_EACH_EDGE (e, ei, bb->preds)
	{
	  bitmap restoreout = BB_INFO (e->src)->restoreout;

	  EXECUTE_IF_SET_IN_BITMAP (restoreout, FIRST_PSEUDO_REGISTER, j, bi)
	    if (! bitmap_bit_p (savein, j)
		&& (bitmap_bit_p (kill, j)
		    || ! bitmap_bit_p (restorein, j))
		&& ! bitmap_bit_p (restore_at_start, j)
		&& bitmap_bit_p (live_at_start, j))
	      {
		pat = get_move_insn (regno_reg_rtx [j],
				   get_new_reg (j));
		insert_insn_on_edge (pat, e);
		change_p = TRUE;
		if (ira_dump_file != NULL)
		  fprintf
		    (ira_dump_file,
		     "Generating save/restore insn %d<-%d on edge %d->%d\n",
		     REGNO (SET_DEST (pat)), REGNO (SET_SRC (pat)),
		     e->src->index, e->dest->index);
	      }
	}
    }
  ira_free_bitmap (progress);
  ira_free_bitmap (save_at_end);
  ira_free_bitmap (restore_at_start);
}

/* The function splits allocnos living through calls and assigned to a
   call used register.  If FLAG_IRA_MOVE_SPILLS, the function moves
   save/restore insns to correspondingly to the top and bottom of the
   CFG but not moving them to more frequently executed places.  */
int
split_around_calls (void)
{
  change_p = FALSE;
  init_ira_call_data ();
  put_save_restore_and_calculate_local_info ();
  if (flag_ira_move_spills)
    {
      calculate_save ();
      calculate_restore ();
      put_save_restore ();
    }
  finish_ira_call_data ();
  fixup_abnormal_edges ();
  commit_edge_insertions ();
  return change_p;
}



/* The function returns regno of living through call allocno which is
   result of splitting allocno with ORIGINAL_REGNO.  If there is no
   such regno, the function returns -1.  */
int
get_around_calls_regno (int original_regno)
{
  allocno_t a, another_a;
  copy_t cp, next_cp;

  if (original_regno >= ira_max_regno_call_before)
    return -1;
  a = regno_allocno_map [original_regno];
  for (cp = ALLOCNO_COPIES (a); cp != NULL; cp = next_cp)
    {
      if (cp->first == a)
	{
	  another_a = cp->second;
	  next_cp = cp->next_first_allocno_copy;
	}
      else
	{
	  another_a = cp->first;
	  next_cp = cp->next_second_allocno_copy;
	}
      if (cp->move_insn == NULL_RTX)
	continue;
      if (ALLOCNO_REGNO (another_a) >= ira_max_regno_call_before)
	return ALLOCNO_REGNO (another_a);
    }
  return -1;
}
