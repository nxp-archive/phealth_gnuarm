/* Basic block reordering routines for the GNU compiler.
   Copyright (C) 2000, 2002 Free Software Foundation, Inc.

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   GCC is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
   License for more details.

   You should have received a copy of the GNU General Public License
   along with GCC; see the file COPYING.  If not, write to the Free
   Software Foundation, 59 Temple Place - Suite 330, Boston, MA
   02111-1307, USA.  */

/* This (greedy) algorithm constructs traces in several rounds.
   The construction starts from "seeds".  The seed for the first round
   is the entry point of function.  When there are more than one seed
   that one is selected first that has the lowest key in the heap
   (see function bb_to_key).  Then the algorithm repeatedly adds the most
   probable successor to the end of a trace.  Finally it connects the traces.

   There are two parameters: Branch Threshold and Exec Threshold.
   If the edge to a successor of the actual basic block is lower than
   Branch Threshold or the frequency of the successor is lower than
   Exec Threshold the successor will be the seed in one of the next rounds.
   Each round has these parameters lower than the previous one.
   The last round has to have these parameters set to zero
   so that the remaining blocks are picked up.

   The algorithm selects the most probable successor from all unvisited
   successors and successors that have been added to this trace.
   The other successors (that has not been "sent" to the next round) will be
   other seeds for this round and the secondary traces will start in them.
   If the successor has been visited in this trace the algorithm rotates
   the loop if it is profitable, and terminates the construction of the trace;
   otherwise it is added to the trace (however, there is some heuristic for
   simple branches).

   When connecting traces it first checks whether there is an edge from the
   last block of one trace to the first block of another trace.
   When there are still some unconnected traces it checks whether there exists
   a basic block BB such that BB is a successor of the last bb of one trace
   and BB is a predecessor of the first block of another trace. In this case,
   BB is duplicated and the traces are connected through this duplicate.
   The rest of traces are simply connected so there will be a jump to the
   beginning of the rest of trace.


   References:

   "Software Trace Cache"
   Ramirez, Larriba-Pey, Navarro, Torrellas and Valero; 1999
   http://citeseer.nj.nec.com/15361.html

*/

#include "config.h"
#include "system.h"
#include "tree.h"
#include "rtl.h"
#include "hard-reg-set.h"
#include "basic-block.h"
#include "flags.h"
#include "output.h"
#include "cfglayout.h"
#include "fibheap.h"
#include "target.h"
#include "profile.h"

/* The number of rounds.  */
#define N_ROUNDS 4

/* The number of rounds which the code can grow in.  */
#define N_CODEGROWING_ROUNDS 3

/* Branch thresholds in thousandths (per milles) of the REG_BR_PROB_BASE.  */
static int branch_threshold[N_ROUNDS] = {400, 200, 100, 0};

/* Exec thresholds in thousandths (per milles) of the frequency of bb 0.  */
static int exec_threshold[N_ROUNDS] = {500, 200, 50, 0};

/* If edge frequency is lower than DUPLICATION_THRESHOLD per milles of entry
   block the edge destination is not duplicated while connecting traces.  */
#define DUPLICATION_THRESHOLD 100

/* Length of unconditional jump instruction.  */
static int uncond_jump_length;

/* Original number (before duplications) of basic blocks.  */
static int original_last_basic_block;

/* Which trace is the bb start of (-1 means it is not a start of a trace).  */
static int *start_of_trace;
static int *end_of_trace;

/* Which heap and node is BB in?  */
static fibheap_t *bb_heap;
static fibnode_t *bb_node;

struct trace
{
  /* First and last basic block of the trace.  */
  basic_block first, last;

  /* The round of the STC creation which this trace was found in.  */
  int round;

  /* The length (i.e. the number of basic blocks) of the trace.  */
  int length;
};

/* Maximum frequency and count of one of the entry blocks.  */
int max_entry_frequency;
gcov_type max_entry_count;

/* Local function prototypes.  */
static void find_traces			PARAMS ((int *, struct trace *));
static void mark_bb_visited		PARAMS ((basic_block, int));
static void find_traces_1_round		PARAMS ((int, int, gcov_type,
						 struct trace *, int *, int,
						 fibheap_t *));
static basic_block copy_bb		PARAMS ((basic_block, edge,
						 basic_block, int));
static fibheapkey_t bb_to_key		PARAMS ((basic_block));
static bool better_edge_p		PARAMS ((basic_block, edge, int, int,
						 int, int));
static void connect_traces		PARAMS ((int, struct trace *));
static bool copy_bb_p			PARAMS ((basic_block, int));
static int get_uncond_jump_length	PARAMS ((void));

/* Find the traces for Software Trace Cache.  Chain each trace through
   RBI()->next.  Store the number of traces to N_TRACES and description of
   traces to TRACES.  */

static void
find_traces (n_traces, traces)
     int *n_traces;
     struct trace *traces;
{
  int i;
  edge e;
  fibheap_t heap;

  /* We need to remember the old number of basic blocks because
     while connecting traces, blocks can be copied and thus their
     index can be higher than the size of arrays.  */
  original_last_basic_block = last_basic_block;

  /* We need to know some information for each basic block.  */
  start_of_trace = xmalloc (original_last_basic_block * sizeof (int));
  end_of_trace = xmalloc (original_last_basic_block * sizeof (int));
  bb_heap = xmalloc (original_last_basic_block * sizeof (fibheap_t));
  bb_node = xmalloc (original_last_basic_block * sizeof (fibnode_t));
  for (i = 0; i < original_last_basic_block; i++)
    {
      start_of_trace[i] = -1;
      end_of_trace[i] = -1;
      bb_heap[i] = NULL;
      bb_node[i] = NULL;
    }

  /* Insert entry points of function into heap.  */
  heap = fibheap_new ();
  max_entry_frequency = 0;
  max_entry_count = 0;
  for (e = ENTRY_BLOCK_PTR->succ; e; e = e->succ_next)
    {
      int bb_index = e->dest->index;
      bb_heap[bb_index] = heap;
      bb_node[bb_index] = fibheap_insert (heap, bb_to_key (e->dest), e->dest);
      if (e->dest->frequency > max_entry_frequency)
	max_entry_frequency = e->dest->frequency;
      if (e->dest->count > max_entry_count)
	max_entry_count = e->dest->count;
    }

  /* Find the traces.  */
  for (i = 0; i < N_ROUNDS; i++)
    {
      gcov_type count_threshold;

      if (rtl_dump_file)
	fprintf (rtl_dump_file, "STC - round %d\n", i + 1);

      if (max_entry_count < INT_MAX / 1000)
	count_threshold = max_entry_count * exec_threshold[i] / 1000;
      else
	count_threshold = max_entry_count / 1000 * exec_threshold[i];
      find_traces_1_round (REG_BR_PROB_BASE * branch_threshold[i] / 1000,
			   max_entry_frequency * exec_threshold[i] / 1000,
			   count_threshold, traces, n_traces, i, &heap);
    }
  fibheap_delete (heap);
  free (bb_node);
  free (bb_heap);

  if (rtl_dump_file)
    {
      for (i = 0; i < *n_traces; i++)
	{
	  basic_block bb;
	  fprintf (rtl_dump_file, "Trace %d (round %d):  ", i + 1,
		   traces[i].round + 1);
	  for (bb = traces[i].first; bb != traces[i].last; bb = RBI (bb)->next)
	    fprintf (rtl_dump_file, "%d [%d] ", bb->index, bb->frequency);
	  fprintf (rtl_dump_file, "%d [%d]\n", bb->index, bb->frequency);
	}
      fflush (rtl_dump_file);
    }
}

/* This function marks BB that it was visited in trace number TRACE.  */

static void
mark_bb_visited (bb, trace)
     basic_block bb;
     int trace;
{
  RBI (bb)->visited = trace;
  if (bb_heap[bb->index])
    {
      fibheap_delete_node (bb_heap[bb->index], bb_node[bb->index]);
      bb_heap[bb->index] = NULL;
      bb_node[bb->index] = NULL;
    }
}

/* One round of finding traces. Find traces for BRANCH_TH and EXEC_TH i.e. do
   not include basic blocks their probability is lower than BRANCH_TH or their
   frequency is lower than EXEC_TH into traces (or count is lower than
   COUNT_TH).  It stores the new traces into TRACES and modifies the number of
   traces *N_TRACES. Sets the round (which the trace belongs to) to ROUND. It
   expects that starting basic blocks are in *HEAP and at the end it deletes
   *HEAP and stores starting points for the next round into *HEAP.  */

static void
find_traces_1_round (branch_th, exec_th, count_th, traces, n_traces, round,
		     heap)
     int branch_th;
     int exec_th;
     gcov_type count_th;
     struct trace *traces;
     int *n_traces;
     int round;
     fibheap_t *heap;
{
  /* Heap for discarded basic blocks which are possible starting points for
     the next round.  */
  fibheap_t new_heap = fibheap_new ();

  while (!fibheap_empty (*heap))
    {
      basic_block bb;
      struct trace *trace;
      edge best_edge, e;

      bb = fibheap_extract_min (*heap);
      bb_heap[bb->index] = NULL;
      bb_node[bb->index] = NULL;

      if (rtl_dump_file)
	fprintf (rtl_dump_file, "Getting bb %d\n", bb->index);
      if (RBI (bb)->visited)
	abort ();	/* For debugging purposes.  */

      /* If the BB's frequency is too low send BB to the next round.  */
      if (bb->frequency < exec_th || bb->count < count_th
	  || ((round < N_ROUNDS - 1) && probably_never_executed_bb_p (bb)))
	{
	  int key = bb_to_key (bb);
	  bb_heap[bb->index] = new_heap;
	  bb_node[bb->index] = fibheap_insert (new_heap, key, bb);

	  if (rtl_dump_file)
	    fprintf (rtl_dump_file,
		     "  Possible start point of next round: %d (key: %d)\n",
		     bb->index, key);
	  continue;
	}

      trace = traces + *n_traces;
      trace->first = bb;
      trace->round = round;
      trace->length = 0;
      (*n_traces)++;

      do
	{
	  int prob, freq;

	  /* The probability and frequency of the best edge.  */
	  int best_prob = INT_MIN / 2;
	  int best_freq = INT_MIN / 2;

	  best_edge = NULL;
	  mark_bb_visited (bb, *n_traces);
	  trace->length++;

	  if (rtl_dump_file)
	    fprintf (rtl_dump_file, "Basic block %d was visited in trace %d\n",
		     bb->index, *n_traces - 1);

	  /* Select the successor that will be placed after BB.  */
	  for (e = bb->succ; e; e = e->succ_next)
	    {
	      if (e->flags & EDGE_FAKE)
		abort ();
	      if (e->dest == EXIT_BLOCK_PTR)
		continue;

	      if (RBI (e->dest)->visited
		  && RBI (e->dest)->visited != *n_traces)
		continue;

	      prob = e->probability;
	      freq = EDGE_FREQUENCY (e);

	      /* Edge that cannot be fallthru or improbable or infrequent
		 successor (ie. it is unsuitable successor).  */
	      if (!(e->flags & EDGE_CAN_FALLTHRU) || (e->flags & EDGE_COMPLEX)
		  || prob < branch_th || freq < exec_th || e->count < count_th)
		continue;

	      if (better_edge_p (bb, e, prob, freq, best_prob, best_freq))
		{
		  best_edge = e;
		  best_prob = prob;
		  best_freq = freq;
		}
	    }

	  /* Add all nonselected successors to the heaps.  */
	  for (e = bb->succ; e; e = e->succ_next)
	    {
	      int bb_index = e->dest->index;
	      fibheapkey_t key;

	      if (e == best_edge
		  || e->dest == EXIT_BLOCK_PTR
		  || RBI (e->dest)->visited)
		continue;

	      key = bb_to_key (e->dest);

	      if (bb_heap[bb_index])
		{
		  if (key != bb_node[bb_index]->key)
		    {
		      if (rtl_dump_file)
			{
			  fprintf (rtl_dump_file,
				   "Changing key for bb %d from %ld to %ld.\n",
				   bb_index, (long) bb_node[bb_index]->key,
				   key);
			}
		      fibheap_replace_key (bb_heap[bb_index],
					   bb_node[bb_index], key);
		    }
		}
	      else
		{
		  fibheap_t which_heap = *heap;

		  prob = e->probability;
		  freq = EDGE_FREQUENCY (e);

		  if (!(e->flags & EDGE_CAN_FALLTHRU) || (e->flags & EDGE_COMPLEX)
		      || prob < branch_th || freq < exec_th
		      || e->count < count_th)
		    {
		      if (round < N_ROUNDS - 1)
			which_heap = new_heap;
		    }

		  bb_heap[bb_index] = which_heap;
		  bb_node[bb_index] = fibheap_insert (which_heap, key, e->dest);

		  if (rtl_dump_file)
		    {
		      fprintf (rtl_dump_file,
			       "  Possible start of %s round: %d (key: %ld)\n",
			       (which_heap == new_heap) ? "next" : "this",
			       bb_index, (long) key);
		    }

		}
	    }

	  if (best_edge) /* Found suitable successor.  */
	    {
	      if (RBI (best_edge->dest)->visited == *n_traces)
		{
                  edge other_edge;
                  for (other_edge = bb->succ; other_edge;
                       other_edge = other_edge->succ_next)
                    if ((other_edge->flags & EDGE_CAN_FALLTHRU)
                        && other_edge != best_edge)
                      break;

                  /* In the case the edge is already not a fallthru, or there
                     is other edge we can make fallhtru, we are happy.  */
                  if (!(best_edge->flags & EDGE_FALLTHRU) || other_edge)
                    ;
                  else if (best_edge->dest != bb
                           && best_edge->dest != ENTRY_BLOCK_PTR->next_bb)
                    {
                      if (EDGE_FREQUENCY (best_edge)
                          > 4 * best_edge->dest->frequency / 5)
                        {
                           /* The loop has at least 4 iterations.  */
                          edge e;

                          /* Check whether the loop has not been rotated yet.  */
                          for (e = best_edge->dest->succ; e; e = e->succ_next)
                            if (e->dest == RBI (best_edge->dest)->next)
                              break;
                          if (e)
                            /* The loop has not been rotated yet.  */
                            {
                              /* Rotate the loop.  */

                              if (rtl_dump_file)
                                fprintf (rtl_dump_file,
                                         "Rotating loop %d - %d\n",
                                         best_edge->dest->index, bb->index);

			      /* ??? we need to find the basic block
				 that is sensible end of the loop,
				 not rotate to random one.  */
                              if (best_edge->dest == trace->first)
                                {
                                  RBI (bb)->next = trace->first;
                                  trace->first = RBI (trace->first)->next;
                                  RBI (best_edge->dest)->next = NULL;
                                  bb = best_edge->dest;
                                }
                              else
                                {
                                  basic_block temp;

                                  for (temp = trace->first;
                                       RBI (temp)->next != best_edge->dest;
                                       temp = RBI (temp)->next);
                                  RBI (temp)->next
                                    = RBI (best_edge->dest)->next;
                                  RBI (bb)->next = best_edge->dest;
                                  RBI (best_edge->dest)->next = NULL;
                                  bb = best_edge->dest;
                                }
                            }
                        }
		    }

		  /* Terminate the trace.  */
		  break;
		}
	      else
		{
		  /* Check for a situation

		    A
		   /|
		  B |
		   \|
		    C

		  where
		  EDGE_FREQUENCY (AB) + EDGE_FREQUENCY (BC)
		    >= EDGE_FREQUENCY (AC).
		  (i.e. 2 * B->frequency >= EDGE_FREQUENCY (AC) )
		  Best ordering is then A B C.

		  This situation is created for example by:

		  if (A) B;
		  C;

		  */

		  for (e = bb->succ; e; e = e->succ_next)
		    if (e != best_edge
			&& (e->flags & EDGE_CAN_FALLTHRU)
			&& !(e->flags & EDGE_COMPLEX)
			&& !RBI (e->dest)->visited
			&& !e->dest->pred->pred_next
			&& e->dest->succ
			&& (e->dest->succ->flags & EDGE_CAN_FALLTHRU)
			&& !(e->dest->succ->flags & EDGE_COMPLEX)
			&& !e->dest->succ->succ_next
			&& e->dest->succ->dest == best_edge->dest
			&& 2 * e->dest->frequency >= EDGE_FREQUENCY (best_edge))
		      {
			best_edge = e;
			if (rtl_dump_file)
			  fprintf (rtl_dump_file, "Selecting BB %d\n",
				   best_edge->dest->index);
			break;
		      }

		  RBI (bb)->next = best_edge->dest;
		  bb = best_edge->dest;
		}
	    }
	}
      while (best_edge);
      trace->last = bb;
      start_of_trace[trace->first->index] = *n_traces - 1;
      end_of_trace[trace->last->index] = *n_traces - 1;
    }

  fibheap_delete (*heap);

  /* "Return" the new heap.  */
  *heap = new_heap;
}

/* Create a duplicate of the basic block OLD_BB and redirect edge E to it, add
   it to trace after BB, mark OLD_BB visited and update pass' datastructures
   (TRACE is a number of trace which OLD_BB is duplicated to).  */

static basic_block
copy_bb (old_bb, e, bb, n_traces)
     basic_block old_bb;
     edge e;
     basic_block bb;
     int n_traces;
{
  basic_block new_bb;

  new_bb = cfg_layout_duplicate_bb (old_bb, e);
  if (e->dest != new_bb)
    abort ();
  if (RBI (e->dest)->visited)
    abort ();
  if (rtl_dump_file)
    fprintf (rtl_dump_file,
	     "Duplicated bb %d (created bb %d)\n",
	     old_bb->index, new_bb->index);
  RBI (new_bb)->visited = n_traces;
  RBI (bb)->next = new_bb;

  return new_bb;
}

/* Compute and return the key (for the heap) of the basic block BB.  */

static fibheapkey_t
bb_to_key (bb)
     basic_block bb;
{
  edge e;

  int priority = 2;

  if (probably_never_executed_bb_p (bb))
    return BB_FREQ_MAX;

  for (e = bb->pred; e; e = e->pred_next)
    if (!(e->flags & EDGE_DFS_BACK) && !RBI (e->src)->visited)
      {
	priority = 0;
	break;
      }

  for (e = bb->pred; e; e = e->pred_next)
    {
      int index = e->src->index;
      if (end_of_trace[index] >= 0)
	{
	  priority++;
	  break;
	}
    }

  /* All edges from predecessors of BB are DFS back edges or the predecessors
     of BB are visited.  I want such basic blocks first.  */
  return -100 * BB_FREQ_MAX * priority - bb->frequency;
}

/* Return true when the edge E from basic block BB is better than the temporary
   best edge (details are in function).  The probability of edge E is PROB. The
   frequency of the successor is FREQ.  The current best probability is
   BEST_PROB, the best frequency is BEST_FREQ.
   The edge is considered to be equivalent when PROB does not differ much from
   BEST_PROB; similarly for frequency.  */

static bool
better_edge_p (bb, e, prob, freq, best_prob, best_freq)
     basic_block bb;
     edge e;
     int prob;
     int freq;
     int best_prob;
     int best_freq;
{
  bool is_better_edge;

  /* The BEST_* values do not have to be best, but can be a bit smaller than
     maximum values.  */
  int diff_prob = best_prob / 10;
  int diff_freq = best_freq / 10;

  if (prob > best_prob + diff_prob)
    /* The edge has higher probability than the temporary best edge.  */
    is_better_edge = true;
  else if (prob < best_prob - diff_prob)
    /* The edge has lower probability than the temporary best edge.  */
    is_better_edge = false;
  else if (freq < best_freq - diff_freq)
    /* The edge and the temporary best edge  have almost equivalent
       probabilities.  The higher frequency of a successor now means
       that there is another edge going into that successor.
       This successor has lower frequency so it is better.  */
    is_better_edge = true;
  else if (freq > best_freq + diff_freq)
    /* This successor has higher frequency so it is worse.  */
    is_better_edge = false;
  else if (e->dest->prev_bb == bb)
    /* The edges have equivalent probabilities and the successors
       have equivalent frequencies.  Select the previous successor.  */
    is_better_edge = true;
  else
    is_better_edge = false;

  return is_better_edge;
}

/* Connect traces in array TRACES, N_TRACES is the count of traces.  */

static void
connect_traces (n_traces, traces)
     int n_traces;
     struct trace *traces;
{
  int i;
  bool *connected;
  int last_trace;
  int freq_threshold;
  gcov_type count_threshold;

  freq_threshold = max_entry_frequency * DUPLICATION_THRESHOLD / 1000;
  if (max_entry_count < INT_MAX / 1000)
    count_threshold = max_entry_count * DUPLICATION_THRESHOLD / 1000;
  else
    count_threshold = max_entry_count / 1000 * DUPLICATION_THRESHOLD;

  connected = xcalloc (n_traces, sizeof (bool));
  last_trace = -1;
  for (i = 0; i < n_traces; i++)
    {
      int t = i;
      int t2;

      if (!connected[t])
	{
	  edge e, best;
	  int best_len;

	  connected[t] = true;

	  /* Find the predecessor traces.  */
	  for (t2 = t; t2 > 0;)
	    {
	      best = NULL;
	      best_len = 0;
	      for (e = traces[t2].first->pred; e; e = e->pred_next)
		{
		  int si = e->src->index;

		  if (e->src != ENTRY_BLOCK_PTR
		      && (e->flags & EDGE_CAN_FALLTHRU)
		      && !(e->flags & EDGE_COMPLEX)
		      && si < original_last_basic_block
		      && end_of_trace[si] >= 0
		      && !connected[end_of_trace[si]]
		      && (!best 
			  || e->probability > best->probability
			  || (e->probability == best->probability
			      && traces[end_of_trace[si]].length > best_len)))
		    {
		      best = e;
		      best_len = traces[end_of_trace[si]].length;
		    }
		}
	      if (best)
		{
		  RBI (best->src)->next = best->dest;
		  t2 = end_of_trace[best->src->index];
		  connected[t2] = true;
		  if (rtl_dump_file)
		    {
		      fprintf (rtl_dump_file, "Connection: %d %d\n",
			       best->src->index, best->dest->index);
		    }
		}
	      else
		break;
	    }

	  if (last_trace >= 0)
	    RBI (traces[last_trace].last)->next = traces[t2].first;
	  last_trace = t;

	  /* Find the successor traces.  */
	  for (;;)
	    {
	      /* Find the continuation of the chain.  */
	      best = NULL;
	      best_len = 0;
	      for (e = traces[t].last->succ; e; e = e->succ_next)
		{
		  int di = e->dest->index;

		  if (e->dest != EXIT_BLOCK_PTR
		      && (e->flags & EDGE_CAN_FALLTHRU)
		      && !(e->flags & EDGE_COMPLEX)
		      && di < original_last_basic_block
		      && start_of_trace[di] >= 0
		      && !connected[start_of_trace[di]]
		      && (!best
			  || e->probability > best->probability
			  || (e->probability == best->probability
			      && traces[end_of_trace[di]].length > best_len)))
		    {
		      best = e;
		      best_len = traces[end_of_trace[di]].length;
		    }
		}

	      if (best)
		{
		  if (rtl_dump_file)
		    {
		      fprintf (rtl_dump_file, "Connection: %d %d\n",
			       best->src->index, best->dest->index);
		    }
		}
	      else
		{
		  /* Try to connect the traces by duplication of 1 block.  */
		  edge e2;
		  basic_block next_bb;
		  for (e = traces[t].last->succ; e; e = e->succ_next)
		    if (e->dest != EXIT_BLOCK_PTR
			&& (e->flags & EDGE_CAN_FALLTHRU)
			&& (EDGE_FREQUENCY (e) >= freq_threshold)
			&& (e->count >= count_threshold)
			&& (!best 
			    || e->probability > best->probability))
		      {
			edge best2 = NULL;
			int best2_len = 0;

			for (e2 = e->dest->succ; e2; e2 = e2->succ_next)
			  {
			    int di = e2->dest->index;

			    if (e2->dest == EXIT_BLOCK_PTR
				|| ((e2->flags & EDGE_CAN_FALLTHRU)
				    && di < original_last_basic_block
				    && start_of_trace[di] >= 0
				    && !connected[start_of_trace[di]]
				    && (EDGE_FREQUENCY (e2) >= freq_threshold)
				    && (e2->count >= count_threshold)
				    && (!best2
					|| e2->probability > best2->probability
					|| (e2->probability
					    == best2->probability
					    && traces[end_of_trace[di]].length 
					    > best2_len))))
			      {
				best = e;
				best2 = e2;
				if (e2->dest != EXIT_BLOCK_PTR)
				  best2_len = traces[start_of_trace[di]].length;
				else
				  best2_len = INT_MAX;
				next_bb = e2->dest;
			      }
			  }
		      }
		  if (best)
		    {
		      if (copy_bb_p (best->dest, !optimize_size))
			{
			  basic_block new_bb;

			  if (rtl_dump_file)
			    {
			      fprintf (rtl_dump_file, "Connection: %d %d ",
				       traces[t].last->index,
				       best->dest->index);
			      if (next_bb == EXIT_BLOCK_PTR)
				fprintf (rtl_dump_file, "exit\n");
			      else
				fprintf (rtl_dump_file, "%d\n", next_bb->index);
			    }

			  new_bb = copy_bb (best->dest, best,
					    traces[t].last, t);
			  traces[t].last = new_bb;
			  if (next_bb != EXIT_BLOCK_PTR)
			    {
			      for (best = new_bb->succ; best;
				   best = best->succ_next)
				if (best->dest == next_bb)
				  break;
			    }
			  else
			    best = NULL;
			}
		      else
			best = NULL;
		    }
		}

	      if (best)
		{
		  t = start_of_trace[best->dest->index];
		  RBI (traces[last_trace].last)->next = traces[t].first;
		  connected[t] = true;
		  last_trace = t;
		}
	      else
		break;
	    }
	}
    }

  if (rtl_dump_file)
    {
      basic_block bb;

      fprintf (rtl_dump_file, "Final order:\n");
      for (bb = traces[0].first; bb; bb = RBI (bb)->next)
	fprintf (rtl_dump_file, "%d ", bb->index);
      fprintf (rtl_dump_file, "\n");
      fflush (rtl_dump_file);
    }

  free (connected);
  free (end_of_trace);
  free (start_of_trace);
}

/* Return true when BB can and should be copied.  The trace with number TRACE
   is now being built.  SIZE_CAN_GROW is the flag whether the code is permited
   to grow.  */

static bool
copy_bb_p (bb, size_can_grow)
     basic_block bb;
     int size_can_grow;
{
  int size = 0;
  int max_size = uncond_jump_length;
  rtx insn;

  if (!bb->frequency)
    return false;
  if (!bb->pred || !bb->pred->pred_next)
    return false;
  if (!cfg_layout_can_duplicate_bb_p (bb))
    return false;

  if (size_can_grow && maybe_hot_bb_p (bb))
    max_size *= 8;

  for (insn = bb->head; insn != NEXT_INSN (bb->end);
       insn = NEXT_INSN (insn))
    {
      if (INSN_P (insn))
	size += get_attr_length (insn);
    }

  if (size <= max_size)
    return true;

  if (rtl_dump_file)
    {
      fprintf (rtl_dump_file,
	       "Block %d can't be copied because its size = %d.\n",
	       bb->index, size);
    }

  return false;
}

/* Return the maximum length of unconditional jump.  */

static int
get_uncond_jump_length ()
{
  rtx label, jump;
  int length;

  label = emit_label_before (gen_label_rtx (), get_insns ());
  jump = emit_jump_insn (gen_jump (label));

  length = get_attr_length (jump);

  delete_insn (jump);
  delete_insn (label);
  return length;
}

/* Reorder basic blocks.  The main entry point to this file.  */

void
reorder_basic_blocks ()
{
  int n_traces;
  struct trace *traces;

  if (n_basic_blocks <= 1)
    return;

  if ((* targetm.cannot_modify_jumps_p) ())
    return;

  cfg_layout_initialize ();

  set_edge_can_fallthru_flag ();
  mark_dfs_back_edges ();
  uncond_jump_length = get_uncond_jump_length ();

  traces = xmalloc (n_basic_blocks * sizeof (struct trace));
  n_traces = 0;
  find_traces (&n_traces, traces);
  connect_traces (n_traces, traces);
  free (traces);

  if (rtl_dump_file)
    dump_flow_info (rtl_dump_file);

  cfg_layout_finalize ();
}
