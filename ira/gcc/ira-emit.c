/* Integrated Register Allocator.  Changing code and generating moves.
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
#include "regs.h"
#include "rtl.h"
#include "tm_p.h"
#include "target.h"
#include "flags.h"
#include "obstack.h"
#include "bitmap.h"
#include "hard-reg-set.h"
#include "basic-block.h"
#include "expr.h"
#include "recog.h"
#include "params.h"
#include "timevar.h"
#include "tree-pass.h"
#include "output.h"
#include "reload.h"
#include "errors.h"
#include "df.h"
#include "ira-int.h"

struct move;

static struct move *create_move (allocno_t, allocno_t);
static void free_move (struct move *);
static void free_move_list (struct move *);
static int eq_move_lists_p (struct move *, struct move *);
static int change_regs (rtx *);
static void add_to_edge_list (edge, struct move *, int);
static rtx create_new_reg (rtx);
static int subloop_tree_node_p (loop_tree_node_t, loop_tree_node_t);
static void set_allocno_reg (allocno_t, rtx);
static int not_modified_p (allocno_t, allocno_t);
static void generate_edge_moves (edge);
static void change_loop (loop_tree_node_t);
static int eq_edge_move_lists_p (VEC(edge,gc) *);
static void unify_moves (basic_block, int);
static void traverse_moves (struct move *);
static struct move *modify_move_list (struct move *);
static rtx emit_move_list (struct move *, int);
static void emit_moves (void);
static void update_costs (allocno_t, int, int);
static void add_range_and_copies_from_move_list (struct move *,
						 loop_tree_node_t,
						 bitmap, int);
static void add_ranges_and_copies (void);

/* The structure represents allocno shuffling.  */
struct move
{
  /* The shuffled allocnos.  */
  allocno_t from, to;
  /* The next move in the sequence.  */
  struct move *next;
  /* Use for finding dependencies.  */
  int visited_p;
  /* The size of the following array. */
  int deps_num;
  /* Moves on which given move depends on.  Dependency can be cyclic.
     It means we need a temporary to generates the moves.  */
  struct move **deps;
  /* First insn generated for the move.  */
  rtx insn;
};

/* Array of moves (indexed by BB index) which should be put at the
   start/end of the corresponding blocks.  */
static struct move **at_bb_start, **at_bb_end;

/* Max regno before renaming some pseudo-registers.  For example, the
   same pseudo-register can be renamed in loop if its allocation is
   different outside the loop.  */
static int max_regno_before_changing;

/* The function returns new move of allocnos TO and FROM.  */
static struct move *
create_move (allocno_t to, allocno_t from)
{
  struct move *move;

  move = ira_allocate (sizeof (struct move));
  move->deps = NULL;
  move->deps_num = 0;
  move->to = to;
  move->from = from;
  move->next = NULL;
  move->insn = NULL_RTX;
  move->visited_p = FALSE;
  return move;
}

/* The function frees memory for MOVE and its dependencies.  */
static void
free_move (struct move *move)
{
  if (move->deps != NULL)
    ira_free (move->deps);
  ira_free (move);
}

/* The function frees memory for list of the moves given by its
   HEAD.  */
static void
free_move_list (struct move *head)
{
  struct move *next;
  
  for (; head != NULL; head = next)
    {
      next = head->next;
      free_move (head);
    }
}

/* The function returns nonzero if the the move list LIST1 and LIST2
   are equal (two moves are equal if they shuffles the same
   allocnos).  */
static int
eq_move_lists_p (struct move *list1, struct move *list2)
{
  for (; list1 != NULL && list2 != NULL;
       list1 = list1->next, list2 = list2->next)
    if (list1->from != list2->from || list1->to != list2->to)
      return FALSE;
  return list1 == list2;
}

/* This recursive function changes pseudo-registers in *LOC if it is
   necessary.  The function returns non-zero if a change was done.  */
static int
change_regs (rtx *loc)
{
  int i, regno, result = 0;
  const char *fmt;
  enum rtx_code code;

  if (*loc == NULL_RTX)
    return 0;
  code = GET_CODE (*loc);
  if (code == REG)
    {
      regno = REGNO (*loc);
      if (regno < FIRST_PSEUDO_REGISTER)
	return 0;
      if (regno >= max_regno_before_changing)
	/* It is a shared register which was changed already.  */
	return 0;
      /* ??? That is for reg_equal.  */
      if (ira_curr_loop_tree_node->regno_allocno_map [regno] == NULL)
	return 0;
      *loc = ALLOCNO_REG (ira_curr_loop_tree_node->regno_allocno_map [regno]);
      return 1;
    }

  fmt = GET_RTX_FORMAT (code);
  for (i = GET_RTX_LENGTH (code) - 1; i >= 0; i--)
    {
      if (fmt[i] == 'e')
	result = change_regs (&XEXP (*loc, i)) || result;
      else if (fmt[i] == 'E')
	{
	  int j;

	  for (j = XVECLEN (*loc, i) - 1; j >= 0; j--)
	    result = change_regs (&XVECEXP (*loc, i, j)) || result;
	}
    }
  return result;
}

/* The function attaches MOVE to the edge E.  The move is attached to
   the head of the list if HEAD_P is nonzero.  */
static void
add_to_edge_list (edge e, struct move *move, int head_p)
{
  struct move *last;

  if (head_p || e->aux == NULL)
    {
      move->next = e->aux;
      e->aux = move;
    }
  else
    {
      for (last = e->aux; last->next != NULL; last = last->next)
	;
      last->next = move;
      move->next = NULL;
    }
}

/* The function creates and returns new pseudo-register with the same
   attributes as ORIGINAL_REG.  */
static rtx
create_new_reg (rtx original_reg)
{
  rtx new_reg;

  new_reg = gen_reg_rtx (GET_MODE (original_reg));
  ORIGINAL_REGNO (new_reg) = ORIGINAL_REGNO (original_reg);
  REG_USERVAR_P (new_reg) = REG_USERVAR_P (original_reg);
  REG_POINTER (new_reg) = REG_POINTER (original_reg);
  REG_ATTRS (new_reg) = REG_ATTRS (original_reg);
  if (internal_flag_ira_verbose > 3 && ira_dump_file != NULL)
    fprintf (ira_dump_file, "      Creating newreg=%i from oldreg=%i\n",
	     REGNO (new_reg), REGNO (original_reg));
  return new_reg;
}

/* The function returns non-zero if loop given by SUBNODE inside the
   loop given by NODE.  */
static int
subloop_tree_node_p (loop_tree_node_t subnode, loop_tree_node_t node)
{
  for (; subnode != NULL; subnode = subnode->father)
    if (subnode == node)
      return TRUE;
  return FALSE;
}

/* The function sets up field `reg' to REG for allocnos which has the
   same regno as ALLOCNO and which are inside the loop corresponding to
   ALLOCNO. */
static void
set_allocno_reg (allocno_t allocno, rtx reg)
{
  allocno_t a;
  loop_tree_node_t node;

  node = ALLOCNO_LOOP_TREE_NODE (allocno);
  for (a = regno_allocno_map [ALLOCNO_REGNO (allocno)];
       a != NULL;
       a = ALLOCNO_NEXT_REGNO_ALLOCNO (a))
    if (subloop_tree_node_p (ALLOCNO_LOOP_TREE_NODE (a), node))
      ALLOCNO_REG (a) = reg;
}

/* The following function returns nonzero if move insn of SRC_ALLOCNO
   to DEST_ALLOCNO does not change value of the destination.  */
static int
not_modified_p (allocno_t src_allocno, allocno_t dest_allocno)
{
  int regno, orig_regno;
  allocno_t a;
  loop_tree_node_t node;

  orig_regno = ALLOCNO_REGNO (src_allocno);
  regno = REGNO (ALLOCNO_REG (dest_allocno));
  for (node = ALLOCNO_LOOP_TREE_NODE (src_allocno);
       node != NULL;
       node = node->father)
    if ((a = node->regno_allocno_map [orig_regno]) == NULL)
      break;
    else if (REGNO (ALLOCNO_REG (a)) == (unsigned) regno)
      return TRUE;
    else if (bitmap_bit_p (node->modified_regnos, orig_regno))
      return FALSE;
  return node != NULL;
}

/* The function generates and attaches moves to the edge E.  It looks
   at the final regnos of allocnos living on the edge with the same
   original regno to find what moves should be generated.  */
static void
generate_edge_moves (edge e)
{
  loop_tree_node_t src_loop_node, dest_loop_node;
  unsigned int regno;
  bitmap_iterator bi;
  allocno_t src_allocno, dest_allocno, *src_map, *dest_map;
  struct move *move;

  src_loop_node = IRA_BB_NODE (e->src)->father;
  dest_loop_node = IRA_BB_NODE (e->dest)->father;
  e->aux = NULL;
  if (src_loop_node == dest_loop_node)
    return;
  src_map = src_loop_node->regno_allocno_map;
  dest_map = dest_loop_node->regno_allocno_map;
  EXECUTE_IF_SET_IN_REG_SET (DF_LR_IN (e->dest),
			     FIRST_PSEUDO_REGISTER, regno, bi)
    if (bitmap_bit_p (DF_LR_OUT (e->src), regno))
      {
	src_allocno = src_map [regno];
	dest_allocno = dest_map [regno];
	if (REGNO (ALLOCNO_REG (src_allocno))
	    == REGNO (ALLOCNO_REG (dest_allocno)))
	  continue;
	/* Actually it is not a optimization we need this code because
	   the memory (remember about equivalent memory) might be ROM
	   (or placed in read only section).  */
 	if (ALLOCNO_HARD_REGNO (dest_allocno) < 0
	    && ALLOCNO_HARD_REGNO (src_allocno) >= 0
	    && not_modified_p (src_allocno, dest_allocno))
	  {
	    ALLOCNO_MEM_OPTIMIZED_DEST (src_allocno) = dest_allocno;
	    ALLOCNO_MEM_OPTIMIZED_DEST_P (dest_allocno) = TRUE;
	    if (internal_flag_ira_verbose > 3 && ira_dump_file != NULL)
	      fprintf (ira_dump_file, "      Remove r%d:a%d->a%d(mem)\n",
		       regno, ALLOCNO_NUM (src_allocno),
		       ALLOCNO_NUM (dest_allocno));
	    continue;
	  }
	move = create_move (dest_allocno, src_allocno);
	add_to_edge_list (e, move, TRUE);
    }
}

/* Bitmap of allocnos local for the current loop.  */
static bitmap local_allocno_bitmap;

/* This bitmap is used to find that we need to generate and use a new
   pseudo-register when processing allocnos with the same original
   regno.  */
static bitmap used_regno_bitmap;

/* The following function changes (if necessary) pseudo-registers
   inside loop given by loop tree node NODE.  */
static void
change_loop (loop_tree_node_t node)
{
  bitmap_iterator bi;
  unsigned int i;
  int regno, used_p;
  allocno_t allocno, father_allocno, *map;
  rtx insn, original_reg;
  
  if (node != ira_loop_tree_root)
    {
      
      if (node->bb != NULL)
	{
	  FOR_BB_INSNS (node->bb, insn)
	    if (INSN_P (insn) && change_regs (&insn))
	      {
		df_insn_rescan (insn);
		df_notes_rescan (insn);
	      }
	  return;
	}
      
      if (internal_flag_ira_verbose > 3 && ira_dump_file != NULL)
	fprintf (ira_dump_file,
		 "      Changing RTL for loop %d (header bb%d)\n",
		 node->loop->num, node->loop->header->index);
      
      map = ira_curr_loop_tree_node->father->regno_allocno_map;
      EXECUTE_IF_SET_IN_REG_SET (ira_curr_loop_tree_node->border_allocnos,
				 0, i, bi)
	{
	  allocno = allocnos [i];
	  regno = ALLOCNO_REGNO (allocno);
	  father_allocno = map [regno];
	  /* We generate the same register move because the reload can
	     put a allocno into memory in this case we will have live
	     range splitting.  If it does not happen such the same
	     hard register moves will be removed.  The worst case when
	     the both allocnos are put into memory by the reload is
	     very rare.  */
	  if (father_allocno != NULL
	      && (ALLOCNO_HARD_REGNO (allocno)
		  == ALLOCNO_HARD_REGNO (father_allocno))
	      && (ALLOCNO_HARD_REGNO (allocno) < 0
		  || TEST_HARD_REG_BIT (prohibited_mode_move_regs
					[ALLOCNO_MODE (allocno)],
					ALLOCNO_HARD_REGNO (allocno))
		  /* don't create copies because reload can spill a
		     allocno set by copy although allocno will not get
		     memory slot.  */
		  || reg_equiv_invariant_p [regno]
		  || reg_equiv_const [regno] != NULL_RTX))
	    continue;
	  original_reg = ALLOCNO_REG (allocno);
	  if (father_allocno == NULL
	      || REGNO (ALLOCNO_REG (father_allocno)) == REGNO (original_reg))
	    {
	      if (internal_flag_ira_verbose > 3 && ira_dump_file)
		fprintf (ira_dump_file, "  %i vs father %i:",
			 ALLOCNO_HARD_REGNO (allocno),
			 ALLOCNO_HARD_REGNO (father_allocno));
	      set_allocno_reg (allocno, create_new_reg (original_reg));
	    }
	}
    }
  /* Rename locals: Local allocnos with same regno in different loops
     might get the different hard register.  So we need to change
     ALLOCNO_REG.  */
  bitmap_and_compl (local_allocno_bitmap,
		    ira_curr_loop_tree_node->mentioned_allocnos,
		    ira_curr_loop_tree_node->border_allocnos);
  EXECUTE_IF_SET_IN_REG_SET (local_allocno_bitmap, 0, i, bi)
    {
      allocno = allocnos [i];
      regno = ALLOCNO_REGNO (allocno);
      if (ALLOCNO_CAP_MEMBER (allocno) != NULL)
	continue;
      used_p = bitmap_bit_p (used_regno_bitmap, regno);
      bitmap_set_bit (used_regno_bitmap, regno);
      if (! used_p)
	continue;
      set_allocno_reg (allocno, create_new_reg (ALLOCNO_REG (allocno)));
    }
}

/* The function returns nonzero if move lists on all edges in vector
   VEC are equal.  */
static int
eq_edge_move_lists_p (VEC(edge,gc) *vec)
{
  struct move *list;
  int i;

  list = EDGE_I (vec, 0)->aux;
  for (i = EDGE_COUNT (vec) - 1; i > 0; i--)
    if (! eq_move_lists_p (list, EDGE_I (vec, i)->aux))
      return FALSE;
  return TRUE;
}

/* The function looks at all enter edges (if START_P) or exit edges of
   basic block BB and puts move lists at the BB start or end if it is
   possible.  In other words, it decreases code duplication of
   shuffling allocnos.  */
static void
unify_moves (basic_block bb, int start_p)
{
  int i;
  edge e;
  struct move *list;
  VEC(edge,gc) *vec;

  vec = (start_p ? bb->preds : bb->succs);
  if (EDGE_COUNT (vec) == 0 || ! eq_edge_move_lists_p (vec))
    return;
  e = EDGE_I (vec, 0);
  list = e->aux;
  if (! start_p && control_flow_insn_p (BB_END (bb)))
    return;
  e->aux = NULL;
  for (i = EDGE_COUNT (vec) - 1; i > 0; i--)
    {
      e = EDGE_I (vec, i);
      free_move_list (e->aux);
      e->aux = NULL;
    }
  if (start_p)
    at_bb_start [bb->index] = list;
  else
    at_bb_end [bb->index] = list;
}

/* Last move (in move sequence being processed) setting up the
   corresponding hard register.  */
static struct move *hard_regno_last_set [FIRST_PSEUDO_REGISTER];

/* If the element value is equal to CURR_TICK then the corresponding
   element in `hard_regno_last_set' is defined and correct.  */
static int hard_regno_last_set_check [FIRST_PSEUDO_REGISTER];

/* Last move (in move sequence being processed) setting up the
   corresponding allocno.  */
static struct move **allocno_last_set;

/* If the element value is equal to CURR_TICK then the corresponding
   element in . `allocno_last_set' is defined and correct.  */
static int *allocno_last_set_check;

/* This array contains moves sorted topologically (depth-first) on
   their dependency graph.  */
static varray_type move_varray;

/* The variable value is used to check correctness of values of
   elements of arrays `hard_regno_last_set' and
   `allocno_last_set_check'.  */
static int curr_tick;

/* This recursive function traverses dependecies of MOVE and do
   toplogical sorting (in depth-first order).  */
static void
traverse_moves (struct move *move)
{
  int i;

  if (move->visited_p)
    return;
  move->visited_p = TRUE;
  for (i = move->deps_num - 1; i >= 0; i--)
    traverse_moves (move->deps [i]);
  VARRAY_PUSH_GENERIC_PTR (move_varray, move);
}

/* The function removes unnecessary moves in the LIST, makes
   topological sorting, and removes cycles on hard reg dependencies by
   introducing new allocnos assigned to memory and additional moves.
   It returns the result move list.  */
static struct move *
modify_move_list (struct move *list)
{
  int i, n, nregs, hard_regno;
  allocno_t to, from, new_allocno;
  struct move *move, *new_move, *set_move, *first, *last;

  if (list == NULL)
    return NULL;
  /* Creat move deps.  */
  curr_tick++;
  for (move = list; move != NULL; move = move->next)
    {
      to = move->to;
      if ((hard_regno = ALLOCNO_HARD_REGNO (to)) < 0)
	continue;
      nregs = hard_regno_nregs [hard_regno] [ALLOCNO_MODE (to)];
      for (i = 0; i < nregs; i++)
	{
	  hard_regno_last_set [hard_regno + i] = move;
	  hard_regno_last_set_check [hard_regno + i] = curr_tick;
	}
    }
  for (move = list; move != NULL; move = move->next)
    {
      from = move->from;
      to = move->to;
      if ((hard_regno = ALLOCNO_HARD_REGNO (from)) >= 0)
	{
	  nregs = hard_regno_nregs [hard_regno] [ALLOCNO_MODE (from)];
	  for (n = i = 0; i < nregs; i++)
	    if (hard_regno_last_set_check [hard_regno + i] == curr_tick
		&& (ALLOCNO_REGNO (hard_regno_last_set [hard_regno + i]->to)
		    != ALLOCNO_REGNO (from)))
	      n++;
	  move->deps = ira_allocate (n * sizeof (struct move *));
	  for (n = i = 0; i < nregs; i++)
	    if (hard_regno_last_set_check [hard_regno + i] == curr_tick
		&& (ALLOCNO_REGNO (hard_regno_last_set [hard_regno + i]->to)
		    != ALLOCNO_REGNO (from)))
	      move->deps [n++] = hard_regno_last_set [hard_regno + i];
	  move->deps_num = n;
	}
    }
  /* Toplogical sorting:  */
  VARRAY_POP_ALL (move_varray);
  for (move = list; move != NULL; move = move->next)
    traverse_moves (move);
  last = NULL;
  for (i = VARRAY_ACTIVE_SIZE (move_varray) - 1; i >= 0; i--)
    {
      move = VARRAY_GENERIC_PTR (move_varray, i);
      move->next = NULL;
      if (last != NULL)
	last->next = move;
      last = move;
    }
  first = VARRAY_TOP_GENERIC_PTR (move_varray);
  /* Removing cycles:  */
  curr_tick++;
  VARRAY_POP_ALL (move_varray);
  for (move = first; move != NULL; move = move->next)
    {
      from = move->from;
      to = move->to;
      if ((hard_regno = ALLOCNO_HARD_REGNO (from)) >= 0)
	{
	  nregs = hard_regno_nregs [hard_regno] [ALLOCNO_MODE (from)];
	  for (i = 0; i < nregs; i++)
	    if (hard_regno_last_set_check [hard_regno + i] == curr_tick
		&& ALLOCNO_HARD_REGNO (hard_regno_last_set
				       [hard_regno + i]->to) >= 0)
	      {
		set_move = hard_regno_last_set [hard_regno + i];
		/* It does not matter what loop_tree_node (of TO or
		   FROM) to use for the new allocno because of
		   subsequent IR flattening.  */
		new_allocno
		  = create_allocno (ALLOCNO_REGNO (set_move->to), FALSE,
				    ALLOCNO_LOOP_TREE_NODE (set_move->to));
		ALLOCNO_MODE (new_allocno) = ALLOCNO_MODE (set_move->to);
		ALLOCNO_COVER_CLASS (new_allocno)
		  = ALLOCNO_COVER_CLASS (set_move->to);
		ALLOCNO_BEST_CLASS (new_allocno)
		  = ALLOCNO_COVER_CLASS (new_allocno);
		ALLOCNO_ASSIGNED_P (new_allocno) = TRUE;
		ALLOCNO_HARD_REGNO (new_allocno) = -1;
		ALLOCNO_REG (new_allocno)
		  = create_new_reg (ALLOCNO_REG (set_move->to));
		new_move = create_move (set_move->to, new_allocno);
		set_move->to = new_allocno;
		VARRAY_PUSH_GENERIC_PTR (move_varray, new_move);
		move_loops_num++;
		if (internal_flag_ira_verbose > 2 && ira_dump_file != NULL)
		  fprintf (ira_dump_file,
			   "    Creating temporary allocno a%dr%d\n",
			   ALLOCNO_NUM (new_allocno),
			   REGNO (ALLOCNO_REG (new_allocno)));
	      }
	}
      if ((hard_regno = ALLOCNO_HARD_REGNO (to)) < 0)
	continue;
      nregs = hard_regno_nregs [hard_regno] [ALLOCNO_MODE (to)];
      for (i = 0; i < nregs; i++)
	{
	  hard_regno_last_set [hard_regno + i] = move;
	  hard_regno_last_set_check [hard_regno + i] = curr_tick;
	}
    }
  for (i = VARRAY_ACTIVE_SIZE (move_varray) - 1; i >= 0; i--)
    {
      move = VARRAY_GENERIC_PTR (move_varray, i);
      move->next = NULL;
      last->next = move;
      last = move;
    }
  return first;
}

/* The function generates rtx move insns from the move list LIST.  It
   updates allocation cost using move execution frequency FERQ.  */
static rtx
emit_move_list (struct move *list, int freq)
{
  int cost;
  rtx result, insn;
  enum machine_mode mode;
  enum reg_class cover_class;

  start_sequence ();
  for (; list != NULL; list = list->next)
    {
      start_sequence ();
      emit_move_insn (ALLOCNO_REG (list->to), ALLOCNO_REG (list->from));
      list->insn = get_insns ();
      end_sequence ();
      /* The reload needs to have set up insn codes.  If the reload
	 sets up insn codes by itself, it may fail because insns will
	 have hard registers instead of pseudos and there may be no
	 machine insn with given hard registers.  */
      for (insn = list->insn; insn != NULL_RTX; insn = NEXT_INSN (insn))
	recog_memoized (insn);
      emit_insn (list->insn);
      mode = ALLOCNO_MODE (list->to);
      cover_class = ALLOCNO_COVER_CLASS (list->to);
      cost = 0;
      if (ALLOCNO_HARD_REGNO (list->to) < 0)
	{
	  if (ALLOCNO_HARD_REGNO (list->from) >= 0)
	    {
	      cost = memory_move_cost [mode] [cover_class] [0] * freq;
	      store_cost += cost;
	    }
	}
      else if (ALLOCNO_HARD_REGNO (list->from) < 0)
	{
	  if (ALLOCNO_HARD_REGNO (list->to) >= 0)
	    {
	      cost = memory_move_cost [mode] [cover_class] [0] * freq;
	      load_cost += cost;
	    }
	}
      else
	{
	  cost = register_move_cost [mode] [cover_class] [cover_class] * freq;
	  shuffle_cost += cost;
	}
      overall_cost += cost;
    }
  result = get_insns ();
  end_sequence ();
  return result;
}

/* The function generates rtx move insns from move lists attached to
   basic blocks and edges.  */
static void
emit_moves (void)
{
  basic_block bb;
  edge_iterator ei;
  edge e;
  rtx insns, tmp;

  FOR_EACH_BB (bb)
    {
      if (at_bb_start [bb->index] != NULL)
	{
	  at_bb_start [bb->index] = modify_move_list (at_bb_start [bb->index]);
	  insns = emit_move_list (at_bb_start [bb->index],
				  REG_FREQ_FROM_BB (bb));
	  tmp = BB_HEAD (bb);
	  if (LABEL_P (tmp))
	    tmp = NEXT_INSN (tmp);
	  if (NOTE_INSN_BASIC_BLOCK_P (tmp))
	    tmp = NEXT_INSN (tmp);
	  if (tmp == BB_HEAD (bb))
	    emit_insn_before (insns, tmp);
	  else if (tmp != NULL_RTX)
	    emit_insn_after (insns, PREV_INSN (tmp));
	  else
	    emit_insn_after (insns, get_last_insn ());
	}

      if (at_bb_end [bb->index] != NULL)
	{
	  at_bb_end [bb->index] = modify_move_list (at_bb_end [bb->index]);
	  insns = emit_move_list (at_bb_end [bb->index],
				  REG_FREQ_FROM_BB (bb));
	  ira_assert (! control_flow_insn_p (BB_END (bb)));
	  emit_insn_after (insns, BB_END (bb));
	}

      FOR_EACH_EDGE (e, ei, bb->succs)
	{
	  if (e->aux == NULL)
	    continue;
	  ira_assert ((e->flags & EDGE_ABNORMAL) == 0
		      || ! EDGE_CRITICAL_P (e));
	  e->aux = modify_move_list (e->aux);
	  insert_insn_on_edge
	    (emit_move_list (e->aux,
			     REG_FREQ_FROM_EDGE_FREQ (EDGE_FREQUENCY (e))),
	     e);
	  if (e->src->next_bb != e->dest)
	    additional_jumps_num++;
	}
    }
}

/* Update costs of A and its parents from reading (if READ_P) or
   writing A on an execution path with FREQ.  */
static void
update_costs (allocno_t a, int read_p, int freq)
{
  loop_tree_node_t father;

  for (;;)
    {
      ALLOCNO_NREFS (a)++;
      ALLOCNO_FREQ (a) += freq;
      ALLOCNO_MEMORY_COST (a)
	+= (memory_move_cost [ALLOCNO_MODE (a)] [ALLOCNO_COVER_CLASS (a)]
	    [read_p ? 1 : 0] * freq);
      if ((father = ALLOCNO_LOOP_TREE_NODE (a)->father) == NULL
	  || (a = father->regno_allocno_map [ALLOCNO_REGNO (a)]) == NULL)
	break;
    }
}

/* The function processes moves from LIST with execution FREQ to add
   ranges, copies, and modify costs.  All regnos living through the
   list is in LIVE_THROUGH, and the loop tree node used to find
   corresponding allocnos is NODE.  */
static void
add_range_and_copies_from_move_list (struct move *list, loop_tree_node_t node,
				     bitmap live_through, int freq)
{
  int start, n;
  unsigned int regno;
  struct move *move;
  allocno_t to, from, a;
  copy_t cp;
  allocno_live_range_t r;
  bitmap_iterator bi;
  HARD_REG_SET hard_regs_live;

  if (list == NULL)
    return;
  n = 0;
  EXECUTE_IF_SET_IN_BITMAP (live_through, FIRST_PSEUDO_REGISTER, regno, bi)
    n++;
  REG_SET_TO_HARD_REG_SET (hard_regs_live, live_through);
  /* This is a trick to guarantee that new ranges is not merged with
     the old ones.  */
  max_point++;
  start = max_point;
  for (move = list; move != NULL; move = move->next)
    {
      from = move->from;
      to = move->to;
      if (ALLOCNO_CONFLICT_ALLOCNO_VEC (to) == NULL)
	{
	  if (internal_flag_ira_verbose > 2 && ira_dump_file != NULL)
	    fprintf (ira_dump_file,
		     "    Allocate conflict vector of size %d for a%dr%d\n",
		     n, ALLOCNO_NUM (to), REGNO (ALLOCNO_REG (to)));
	  allocate_allocno_conflicts (to, n);
	}
      bitmap_clear_bit (live_through, ALLOCNO_REGNO (from));
      bitmap_clear_bit (live_through, ALLOCNO_REGNO (to));
      IOR_HARD_REG_SET (ALLOCNO_CONFLICT_HARD_REGS (from), hard_regs_live);
      IOR_HARD_REG_SET (ALLOCNO_CONFLICT_HARD_REGS (to), hard_regs_live);
      IOR_HARD_REG_SET (ALLOCNO_TOTAL_CONFLICT_HARD_REGS (from),
			hard_regs_live);
      IOR_HARD_REG_SET (ALLOCNO_TOTAL_CONFLICT_HARD_REGS (to), hard_regs_live);
      update_costs (from, TRUE, freq);
      update_costs (to, FALSE, freq);
      cp = add_allocno_copy (from, to, freq, move->insn, NULL);
      if (internal_flag_ira_verbose > 2 && ira_dump_file != NULL)
	fprintf (ira_dump_file, "    Adding cp%d:a%dr%d-a%dr%d\n",
		 cp->num, ALLOCNO_NUM (cp->first),
		 REGNO (ALLOCNO_REG (cp->first)), ALLOCNO_NUM (cp->second),
		 REGNO (ALLOCNO_REG (cp->second)));
      r = ALLOCNO_LIVE_RANGES (from);
      if (r == NULL || r->finish >= 0)
	{
	  ALLOCNO_LIVE_RANGES (from)
	    = create_allocno_live_range (from, start, max_point, r);
	  if (internal_flag_ira_verbose > 2 && ira_dump_file != NULL)
	    fprintf (ira_dump_file,
		     "    Adding range [%d..%d] to allocno a%dr%d\n",
		     start, max_point, ALLOCNO_NUM (from),
		     REGNO (ALLOCNO_REG (from)));
	}
      else
	r->finish = max_point;
      max_point++;
      ALLOCNO_LIVE_RANGES (to)
	= create_allocno_live_range (to, max_point, -1,
				     ALLOCNO_LIVE_RANGES (to));
      max_point++;
    }
  for (move = list; move != NULL; move = move->next)
    {
      r = ALLOCNO_LIVE_RANGES (move->to);
      if (r->finish < 0)
	{
	  r->finish = max_point - 1;
	  if (internal_flag_ira_verbose > 2 && ira_dump_file != NULL)
	    fprintf (ira_dump_file,
		     "    Adding range [%d..%d] to allocno a%dr%d\n",
		     r->start, r->finish, ALLOCNO_NUM (move->to),
		     REGNO (ALLOCNO_REG (move->to)));
	}
    }
  EXECUTE_IF_SET_IN_BITMAP (live_through, FIRST_PSEUDO_REGISTER, regno, bi)
    {
      a = node->regno_allocno_map [regno];
      if (ALLOCNO_MEM_OPTIMIZED_DEST (a) == NULL)
	{
	  ALLOCNO_LIVE_RANGES (a)
	    = create_allocno_live_range (a, start, max_point - 1,
					 ALLOCNO_LIVE_RANGES (a));
	  if (internal_flag_ira_verbose > 2 && ira_dump_file != NULL)
	    fprintf
	      (ira_dump_file,
	       "    Adding range [%d..%d] to live through allocno a%dr%d\n",
	       start, max_point - 1, ALLOCNO_NUM (a), REGNO (ALLOCNO_REG (a)));
	}
    }
}

/* The function processes all move list to add ranges, conflicts,
   copies, and modify costs.  */
static void
add_ranges_and_copies (void)
{
  basic_block bb;
  edge_iterator ei;
  edge e;
  loop_tree_node_t node;
  bitmap live_through;

  live_through = ira_allocate_bitmap ();
  FOR_EACH_BB (bb)
    {
      /* It does not matter what loop_tree_node (of source or
	 destination block) to use for searching allocnos by their
	 regnos because of subsequent IR flattening.  */
      node = IRA_BB_NODE (bb)->father;
      bitmap_copy (live_through, DF_LR_IN (bb));
      add_range_and_copies_from_move_list
	(at_bb_start [bb->index], node, live_through, REG_FREQ_FROM_BB (bb));
      bitmap_copy (live_through, DF_LR_OUT (bb));
      add_range_and_copies_from_move_list
	(at_bb_end [bb->index], node, live_through, REG_FREQ_FROM_BB (bb));
      FOR_EACH_EDGE (e, ei, bb->succs)
	{
	  bitmap_and (live_through, DF_LR_IN (e->dest), DF_LR_OUT (bb));
	  add_range_and_copies_from_move_list
	    (e->aux, node, live_through,
	     REG_FREQ_FROM_EDGE_FREQ (EDGE_FREQUENCY (e)));
	}
    }
  ira_free_bitmap (live_through);
}

/* Entry function changing code and generating allocno shuffling for
   the regional (LOOPS_P is TRUE in this case) register allocation.  */
void
ira_emit (int loops_p)
{
  int i;
  basic_block bb;
  edge_iterator ei;
  edge e;

  for (i = 0; i < allocnos_num; i++)
    ALLOCNO_REG (allocnos [i]) = regno_reg_rtx [ALLOCNO_REGNO (allocnos [i])];
  if (! loops_p)
    return;
  at_bb_start = ira_allocate (sizeof (struct move *) * last_basic_block);
  memset (at_bb_start, 0, sizeof (struct move *) * last_basic_block);
  at_bb_end = ira_allocate (sizeof (struct move *) * last_basic_block);
  memset (at_bb_end, 0, sizeof (struct move *) * last_basic_block);
  local_allocno_bitmap = ira_allocate_bitmap ();
  used_regno_bitmap = ira_allocate_bitmap ();
  max_regno_before_changing = max_reg_num ();
  traverse_loop_tree (FALSE, ira_loop_tree_root, change_loop, NULL);
  ira_free_bitmap (used_regno_bitmap);
  ira_free_bitmap (local_allocno_bitmap);
  FOR_EACH_BB (bb)
    {
      at_bb_start [bb->index] = NULL;
      at_bb_end [bb->index] = NULL;
      FOR_EACH_EDGE (e, ei, bb->succs)
	if (e->dest != EXIT_BLOCK_PTR)
	  generate_edge_moves (e);
    }
  allocno_last_set = ira_allocate (sizeof (struct move *) * max_reg_num ());
  allocno_last_set_check = ira_allocate (sizeof (int) * max_reg_num ());
  memset (allocno_last_set_check, 0, sizeof (int) * max_reg_num ());
  memset (hard_regno_last_set_check, 0, sizeof (hard_regno_last_set_check));
  curr_tick = 0;
  FOR_EACH_BB (bb)
    unify_moves (bb, TRUE);
  FOR_EACH_BB (bb)
    unify_moves (bb, FALSE);
  VARRAY_GENERIC_PTR_NOGC_INIT (move_varray, allocnos_num, "ordered moves");
  emit_moves ();
  add_ranges_and_copies ();
  /* Clean up: */
  FOR_EACH_BB (bb)
    {
      free_move_list (at_bb_start [bb->index]);
      free_move_list (at_bb_end [bb->index]);
      FOR_EACH_EDGE (e, ei, bb->succs)
	{
	  free_move_list (e->aux);
	  e->aux = NULL;
	}
    }
  VARRAY_FREE (move_varray);
  ira_free (allocno_last_set_check);
  ira_free (allocno_last_set);
  commit_edge_insertions ();
  ira_free (at_bb_end);
  ira_free (at_bb_start);
}
