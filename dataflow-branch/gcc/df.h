/* Form lists of pseudo register references for autoinc optimization
   for GNU compiler.  This is part of flow optimization.
   Copyright (C) 1999, 2000, 2001, 2003, 2004, 2005, 2006
   Free Software Foundation, Inc.
   Originally contributed by Michael P. Hayes 
             (m.hayes@elec.canterbury.ac.nz, mhayes@redhat.com)
   Major rewrite contributed by Danny Berlin (dberlin@dberlin.org)
             and Kenneth Zadeck (zadeck@naturalbridge.com).

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

#ifndef GCC_DF_H
#define GCC_DF_H

#include "bitmap.h"
#include "basic-block.h"
#include "alloc-pool.h"

struct dataflow;
struct df;
struct df_problem;
struct df_link;

/* Data flow problems.  All problems must have a unique here.  */ 
/* Scanning is not really a dataflow problem, but it is useful to have
   the basic block functions in the vector so that things get done in
   a uniform manner.  */
#define DF_SCAN  0 
#define DF_RU    1      /* Reaching Uses. */
#define DF_RD    2      /* Reaching Defs. */
#define DF_LR    3      /* Live Registers backward. */
#define DF_UR    4      /* Uninitialized Registers forwards. */
#define DF_LIVE  5      /* Live Registers & Uninitialized Registers */
#define DF_UREC  6      /* Uninitialized Registers with Early Clobber. */
#define DF_CHAIN 7      /* Def-Use and/or Use-Def Chains. */
#define DF_RI    8      /* Register Info. */
#define DF_LAST_PROBLEM_PLUS1 (DF_RI + 1)


/* Dataflow direction.  */
enum df_flow_dir
  {
    DF_NONE,
    DF_FORWARD,
    DF_BACKWARD
  };


/* The first of these is a set of a register.  The remaining three are
   all uses of a register (the mem_load and mem_store relate to how
   the register as an addressing operand).  */
enum df_ref_type {DF_REF_REG_DEF, DF_REF_REG_USE, DF_REF_REG_MEM_LOAD,
		  DF_REF_REG_MEM_STORE};

#define DF_REF_TYPE_NAMES {"def", "use", "mem load", "mem store"}

enum df_ref_flags
  {
    /* Read-modify-write refs generate both a use and a def and
       these are marked with this flag to show that they are not
       independent.  */
    DF_REF_READ_WRITE = 1 << 0,

    /* If this flag is set for an artificial use or def, that ref
       logically happens at the top of the block.  If it is not set
       for an artificial use or def, that ref logically happens at the
       bottom of the block.  This is never set for regular refs.  */
    DF_REF_AT_TOP = 1 << 1,

    /* This flag is set if the use is inside a REG_EQUAL or REG_EQUIV
       note.  */
    DF_REF_IN_NOTE = 1 << 2,

    /* This flag is set if this ref, generally a def, may clobber the
       referenced register.  This is generally only set for hard
       registers that cross a call site.  With better information
       about calls, some of these could be changed in the future to
       DF_REF_MUST_CLOBBER.  */
    DF_REF_MAY_CLOBBER = 1 << 3,

    /* This flag is set if this ref, generally a def, is a real
       clobber. This is not currently set for registers live across a
       call because that clobbering may or may not happen.  

       Most of the uses of this are with sets that have a
       GET_CODE(..)==CLOBBER.  Note that this is set even if the
       clobber is to a subreg.  So in order to tell if the clobber
       wipes out the entire register, it is necessary to also check
       the DF_REF_PARTIAL flag.  */
    DF_REF_MUST_CLOBBER = 1 << 4,

    /* This bit is true if this ref is part of a multiword hardreg.  */
    DF_REF_MW_HARDREG = 1 << 5,

    /* This flag is set if this ref is a partial use or def of the
       associated register.  */
    DF_REF_PARTIAL = 1 << 6,
    
    /* This flag is set if this ref occurs inside of a conditional
       execution instruction.  */
    DF_REF_CONDITIONAL = 1 << 7,

    /* This flag is set if this ref is inside a pre/post modify.  */
    DF_REF_PRE_POST_MODIFY = 1 << 8,

    /* This flag is set if this ref is a usage of the stack pointer by
       a function call.  */
    DF_REF_CALL_STACK_USAGE = 1 << 9,

    /* This flag is used internally to group
       the hardregs. */
    DF_REF_MW_HARDREG_GROUP = 1 << 10,

    /* This flag is a marker for general purpose use.
       Used for verification of existing refs. */
    DF_REF_MARKER = 1 << 11
  };


/* Function prototypes added to df_problem instance.  */

/* Allocate the problem specific data.  */
typedef void (*df_alloc_function) (struct dataflow *, bitmap, bitmap);

/* This function is called if the problem has global data that needs
   to be cleared when ever the set of blocks changes.  The bitmap
   contains the set of blocks that may require special attention.
   This call is only made if some of the blocks are going to change.
   If everything is to be deleted, the wholesale deletion mechanisms
   apply. */
typedef void (*df_reset_function) (struct dataflow *, bitmap);

/* Free the basic block info.  Called from the block reordering code
   to get rid of the blocks that have been squished down.   */
typedef void (*df_free_bb_function) (struct dataflow *, basic_block, void *);

/* Local compute function.  */
typedef void (*df_local_compute_function) (struct dataflow *, bitmap, bitmap);

/* Init the solution specific data.  */
typedef void (*df_init_function) (struct dataflow *, bitmap);

/* Iterative dataflow function.  */
typedef void (*df_dataflow_function) (struct dataflow *, bitmap, bitmap, 
				   int *, int, bool);

/* Confluence operator for blocks with 0 out (or in) edges.  */
typedef void (*df_confluence_function_0) (struct dataflow *, basic_block);

/* Confluence operator for blocks with 1 or more out (or in) edges.  */
typedef void (*df_confluence_function_n) (struct dataflow *, edge);

/* Transfer function for blocks.  */
typedef bool (*df_transfer_function) (struct dataflow *, int);

/* Function to massage the information after the problem solving.  */
typedef void (*df_finalizer_function) (struct dataflow*, bitmap);

/* Function to free all of the problem specific datastructures.  */
typedef void (*df_free_function) (struct dataflow *);

/* Function to remove this problem from the stack of dataflow problems
   without effecting the other problems in the stack except for those
   that depend on this problem.  */
typedef void (*df_remove_problem_function) (struct dataflow *);

/* Function to dump basic block independent results to FILE.  */
typedef void (*df_dump_problem_function) (struct dataflow *, FILE *);

/* Function to dump top or bottom of basic block results to FILE.  */
typedef void (*df_dump_bb_problem_function) (struct dataflow *, basic_block, FILE *);

/* The static description of a dataflow problem to solve.  See above
   typedefs for doc for the function fields.  */

struct df_problem {
  /* The unique id of the problem.  This is used it index into
     df->defined_problems to make accessing the problem data easy.  */
  unsigned int id;                        
  enum df_flow_dir dir;			/* Dataflow direction.  */
  df_alloc_function alloc_fun;
  df_reset_function reset_fun;
  df_free_bb_function free_bb_fun;
  df_local_compute_function local_compute_fun;
  df_init_function init_fun;
  df_dataflow_function dataflow_fun;
  df_confluence_function_0 con_fun_0;
  df_confluence_function_n con_fun_n;
  df_transfer_function trans_fun;
  df_finalizer_function finalize_fun;
  df_free_function free_fun;
  df_remove_problem_function remove_problem_fun;
  df_dump_problem_function dump_start_fun;
  df_dump_bb_problem_function dump_top_fun;
  df_dump_bb_problem_function dump_bottom_fun;
  struct df_problem *dependent_problem;
};


/* The specific instance of the problem to solve.  */
struct dataflow
{
  struct df *df;                        /* Instance of df we are working in.  */
  struct df_problem *problem;           /* The problem to be solved.  */

  /* Communication between iterative_dataflow and hybrid_search. */
  sbitmap visited, pending, considered; 

  /* Array indexed by bb->index, that contains basic block problem and
     solution specific information.  */
  void **block_info;
  unsigned int block_info_size;

  /* The pool to allocate the block_info from. */
  alloc_pool block_pool;                

  /* Other problem specific data that is not on a per basic block
     basis.  The structure is generally defined privately for the
     problem.  The exception being the scanning problem where it is
     fully public.  */
  void *problem_data;
  
  /* True if this problem of this instance has been initialized.  */
  bool computed;
};


/* The set of multiword hardregs used as operands to this
   instruction. These are factored into individual uses and defs but
   the aggregate is still needed to service the REG_DEAD and
   REG_UNUSED notes.  */
struct df_mw_hardreg
{
  rtx mw_reg;                   /* The multiword hardreg.  */ 
  enum df_ref_type type;        /* Used to see if the ref is read or write.  */
  enum df_ref_flags flags;	/* Various flags.  */
  struct df_link *regs;         /* The individual regs that make up
				   this hardreg.  */
  struct df_mw_hardreg *next;   /* The next mw_hardreg in this insn.  */
};
 

/* One of these structures is allocated for every insn.  */
struct df_insn_info
{
  struct df_ref *defs;	        /* Head of insn-def chain.  */
  struct df_ref *uses;	        /* Head of insn-use chain.  */
  struct df_mw_hardreg *mw_hardregs;   
  /* Head of insn-use chain for uses in REG_EQUAL/EQUIV notes.  */
  struct df_ref *eq_uses;       
  /* ???? The following luid field should be considered private so that
     we can change it on the fly to accommodate new insns?  */
  int luid;			/* Logical UID.  */
  bool contains_asm;            /* Contains an asm instruction.  */
};


/* Define a register reference structure.  One of these is allocated
   for every register reference (use or def).  Note some register
   references (e.g., post_inc, subreg) generate both a def and a use.  */
struct df_ref
{
  rtx reg;			/* The register referenced.  */
  unsigned int regno;           /* The register number referenced.  */
  basic_block bb;               /* Basic block containing the instruction. */

  /* Insn containing ref. This will be null if this is an artificial
     reference.  */
  rtx insn;
  rtx *loc;			/* The location of the reg.  */
  struct df_link *chain;	/* Head of def-use, use-def.  */
  unsigned int id;		/* Location in table.  */
  enum df_ref_type type;	/* Type of ref.  */
  enum df_ref_flags flags;	/* Various flags.  */

  /* For each regno, there are two chains of refs, one for the uses
     and one for the defs.  These chains go thru the refs themselves
     rather than using an external structure.  */
  struct df_ref *next_reg;     /* Next ref with same regno and type.  */
  struct df_ref *prev_reg;     /* Prev ref with same regno and type.  */

  /* Each insn has two lists, one for the uses and one for the
     defs. This is the next field in either of these chains. */
  struct df_ref *next_ref; 
};

/* These links are used for two purposes:
   1) def-use or use-def chains. 
   2) Multiword hard registers that underly a single hardware register.  */
struct df_link
{
  struct df_ref *ref;
  struct df_link *next;
};


enum df_permanent_flags 
{
  /* Flags that control the building of chains.  */
  DF_DU_CHAIN      =  1, /* Build DU chains.  */  
  DF_UD_CHAIN      =  2, /* Build UD chains.  */
  /* Flag to control the building of register info.  */
  DF_RI_LIFE       =  4, /* Build register info.  */
  DF_RI_SETJMP     =  8  /* Build pseudos that cross setjmp info.  */
};

enum df_changeable_flags 
{
  /* Scanning flags.  */
  /* Flag to control the running of dce as a side effect of building LR.  */
  DF_LR_RUN_DCE    = 1,  /* Run DCE.  */
  DF_NO_HARD_REGS  = 2,  /* Skip hard registers in RD and CHAIN Building.  */
  DF_EQ_NOTES      = 4,  /* Build chains with uses present in EQUIV/EQUAL notes. */
  DF_RI_NO_UPDATE  = 8   /* Do not update the register info when df_analyze is run.  */
};

/* Two of these structures are inline in df, one for the uses and one
   for the defs.  */
struct df_ref_info
{
  struct df_ref **refs;         /* Ref table, indexed by id.  */
  unsigned int *begin;          /* First ref_index for this pseudo.  */
  unsigned int refs_size;       /* Size of currently allocated refs table.  */
  unsigned int bitmap_size;	/* Number of refs seen.  */

  /* True if refs table is organized so that every reference for a
     pseudo is contiguous.  */
  bool refs_organized_alone;
  /* True if the next refs should be added immediately or false to
     defer to later to reorganize the table.  */
  bool refs_organized_with_eq_uses;
  /* True if the next refs should be added immediately or false to
     defer to later to reorganize the table.  */
  bool add_refs_inline; 
};

/* Three of these structures are allocated for every pseudo reg. One
   for the uses, one for the eq_uses and one for the defs.  */
struct df_reg_info
{
  /* Head of chain for refs of that type and regno.  */
  struct df_ref *reg_chain;
  /* Number of refs in the chain.  */
  unsigned int n_refs;
};


/*----------------------------------------------------------------------------
   Problem data for the scanning dataflow problem.  Unlike the other
   dataflow problems, the problem data for scanning is fully exposed and
   used by owners of the problem.
----------------------------------------------------------------------------*/

struct df
{

  /* The set of problems to be solved is stored in two arrays.  In
     PROBLEMS_IN_ORDER, the problems are stored in the order that they
     are solved.  This is an internally dense array that may have
     nulls at the end of it.  In PROBLEMS_BY_INDEX, the problem is
     stored by the value in df_problem.id.  These are used to access
     the problem local data without having to search the first
     array.  */

  struct dataflow *problems_in_order [DF_LAST_PROBLEM_PLUS1]; 
  struct dataflow *problems_by_index [DF_LAST_PROBLEM_PLUS1]; 
  int num_problems_defined;

  /* If not NULL, the subset of blocks of the program to be considered
     for analysis.  */ 
  bitmap blocks_to_analyze;

  /* The set of blocks whose transfer functions are out of date.  */
  bitmap out_of_date_transfer_functions;

  /* True if the something has changed which invalidates the dataflow
     solutions.  */
  bool solutions_dirty;

  /* The following information is really the problem data for the
     scanning instance but it is used too often by the other problems
     to keep getting it from there.  */
  struct df_ref_info def_info;   /* Def info.  */
  struct df_ref_info use_info;   /* Use info.  */

  /* The following three arrays are allocated in parallel.   They contain
     the sets of refs of each type for each reg.  */
  struct df_reg_info **def_regs;       /* Def reg info.  */
  struct df_reg_info **use_regs;       /* Eq_use reg info.  */
  struct df_reg_info **eq_use_regs;    /* Eq_use info.  */
  unsigned int regs_size;       /* Size of currently allocated regs table.  */
  unsigned int regs_inited;     /* Number of regs with reg_infos allocated.  */


  struct df_insn_info **insns;   /* Insn table, indexed by insn UID.  */
  unsigned int insns_size;       /* Size of insn table.  */
  bitmap hardware_regs_used;     /* The set of hardware registers used.  */
  /* The set of hard regs that are in the artificial uses at the end
     of a regular basic block.  */
  bitmap regular_block_artificial_uses;
  /* The set of hard regs that are in the artificial uses at the end
     of a basic block that has an EH pred.  */
  bitmap eh_block_artificial_uses;
  bitmap entry_block_defs;       /* The set of hardware registers live on entry to the function.  */
  bitmap exit_block_uses;        /* The set of hardware registers used in exit block.  */
  int *postorder;                /* The current set of basic blocks in postorder.  */
  int n_blocks;                  /* The number of blocks in postorder.  */

  /* Problem specific control infomation.  */
  enum df_permanent_flags permanent_flags;
  enum df_changeable_flags changeable_flags;
};

#define DF_SCAN_BB_INFO(DF, BB) (df_scan_get_bb_info((DF)->problems_by_index[DF_SCAN],(BB)->index))
#define DF_RU_BB_INFO(DF, BB) (df_ru_get_bb_info((DF)->problems_by_index[DF_RU],(BB)->index))
#define DF_RD_BB_INFO(DF, BB) (df_rd_get_bb_info((DF)->problems_by_index[DF_RD],(BB)->index))
#define DF_LR_BB_INFO(DF, BB) (df_lr_get_bb_info((DF)->problems_by_index[DF_LR],(BB)->index))
#define DF_UR_BB_INFO(DF, BB) (df_ur_get_bb_info((DF)->problems_by_index[DF_UR],(BB)->index))
#define DF_UREC_BB_INFO(DF, BB) (df_urec_get_bb_info((DF)->problems_by_index[DF_UREC],(BB)->index))
#define DF_LIVE_BB_INFO(DF, BB) (df_live_get_bb_info((DF)->problems_by_index[DF_LIVE],(BB)->index))

/* Most transformations that wish to use live register analysis will
   use these macros.  This info is the and of the lr and ur sets.  */
#define DF_LIVE_IN(DF, BB) (DF_LIVE_BB_INFO(DF, BB)->in) 
#define DF_LIVE_OUT(DF, BB) (DF_LIVE_BB_INFO(DF, BB)->out) 


/* Live in for register allocation also takes into account several other factors.  */
#define DF_RA_LIVE_IN(DF, BB) (DF_UREC_BB_INFO(DF, BB)->in) 
#define DF_RA_LIVE_OUT(DF, BB) (DF_UREC_BB_INFO(DF, BB)->out) 

/* These macros are currently used by only reg-stack since it is not
   tolerant of uninitialized variables.  This intolerance should be
   fixed because it causes other problems.  */ 
#define DF_LR_IN(DF, BB) (DF_LR_BB_INFO(DF, BB)->in) 
#define DF_LR_OUT(DF, BB) (DF_LR_BB_INFO(DF, BB)->out) 

/* These macros are currently used by only combine which needs to know
   what is really uninitialized.  */ 
#define DF_UR_IN(DF, BB) (DF_UR_BB_INFO(DF, BB)->in) 
#define DF_UR_OUT(DF, BB) (DF_UR_BB_INFO(DF, BB)->out) 


/* Macros to access the elements within the ref structure.  */


#define DF_REF_REAL_REG(REF) (GET_CODE ((REF)->reg) == SUBREG \
				? SUBREG_REG ((REF)->reg) : ((REF)->reg))
#define DF_REF_REGNO(REF) ((REF)->regno)
#define DF_REF_REAL_LOC(REF) (GET_CODE (*((REF)->loc)) == SUBREG \
                               ? &SUBREG_REG (*((REF)->loc)) : ((REF)->loc))
#define DF_REF_REG(REF) ((REF)->reg)
#define DF_REF_LOC(REF) ((REF)->loc)
#define DF_REF_BB(REF) ((REF)->bb)
#define DF_REF_BBNO(REF) (DF_REF_BB (REF)->index)
#define DF_REF_INSN(REF) ((REF)->insn)
#define DF_REF_INSN_UID(REF) (INSN_UID ((REF)->insn))
#define DF_REF_TYPE(REF) ((REF)->type)
#define DF_REF_CHAIN(REF) ((REF)->chain)
#define DF_REF_ID(REF) ((REF)->id)
#define DF_REF_FLAGS(REF) ((REF)->flags)
#define DF_REF_FLAGS_IS_SET(REF, v) ((DF_REF_FLAGS (REF) & (v)) != 0)
#define DF_REF_FLAGS_SET(REF, v) (DF_REF_FLAGS (REF) |= (v))
#define DF_REF_FLAGS_CLEAR(REF, v) (DF_REF_FLAGS (REF) &= ~(v))
/* If DF_REF_IS_ARTIFICIAL () is true, this is not a real definition/use, 
   but an artificial one created to model 
   always live registers, eh uses, etc.  
   ARTIFICIAL refs has NULL insn.  */
#define DF_REF_IS_ARTIFICIAL(REF) ((REF)->insn == NULL)
#define DF_REF_MARK(REF) (DF_REF_FLAGS_SET ((REF),DF_REF_MARKER))
#define DF_REF_UNMARK(REF) (DF_REF_FLAGS_CLEAR ((REF),DF_REF_MARKER))
#define DF_REF_IS_MARKED(REF) (DF_REF_FLAGS_IS_SET ((REF),DF_REF_MARKER))
#define DF_REF_NEXT_REG(REF) ((REF)->next_reg)
#define DF_REF_PREV_REG(REF) ((REF)->prev_reg)
#define DF_REF_NEXT_REF(REF) ((REF)->next_ref)

/* Macros to determine the reference type.  */

#define DF_REF_REG_DEF_P(REF) (DF_REF_TYPE (REF) == DF_REF_REG_DEF)
#define DF_REF_REG_USE_P(REF) ((REF) && !DF_REF_REG_DEF_P (REF))
#define DF_REF_REG_MEM_STORE_P(REF) (DF_REF_TYPE (REF) == DF_REF_REG_MEM_STORE)
#define DF_REF_REG_MEM_LOAD_P(REF) (DF_REF_TYPE (REF) == DF_REF_REG_MEM_LOAD)
#define DF_REF_REG_MEM_P(REF) (DF_REF_REG_MEM_STORE_P (REF) \
                               || DF_REF_REG_MEM_LOAD_P (REF))

/* Macros to get the refs out of def_info or use_info refs table.  */
#define DF_DEFS_SIZE(DF) ((DF)->def_info.bitmap_size)
#define DF_DEFS_GET(DF,ID) ((DF)->def_info.refs[(ID)])
#define DF_DEFS_SET(DF,ID,VAL) ((DF)->def_info.refs[(ID)]=(VAL))
#define DF_DEFS_COUNT(DF,ID) (DF_REG_DEF_COUNT(DF,ID))
#define DF_DEFS_BEGIN(DF,ID) ((DF)->def_info.begin[(ID)])
#define DF_USES_SIZE(DF) ((DF)->use_info.bitmap_size)
#define DF_USES_GET(DF,ID) ((DF)->use_info.refs[(ID)])
#define DF_USES_SET(DF,ID,VAL) ((DF)->use_info.refs[(ID)]=(VAL))
#define DF_USES_COUNT(DF,ID) (DF_REG_USE_COUNT(DF,ID)+DF_REG_EQ_USE_COUNT(DF,ID))
#define DF_USES_BEGIN(DF,ID) ((DF)->use_info.begin[(ID)])

/* Macros to access the register information from scan dataflow record.  */

#define DF_REG_SIZE(DF) ((DF)->regs_inited)
#define DF_REG_DEF_GET(DF,REG) ((DF)->def_regs[(REG)])
#define DF_REG_DEF_CHAIN(DF,REG) ((DF)->def_regs[(REG)]->reg_chain)
#define DF_REG_DEF_COUNT(DF,REG) ((DF)->def_regs[(REG)]->n_refs)
#define DF_REG_USE_GET(DF,REG) ((DF)->use_regs[(REG)])
#define DF_REG_USE_CHAIN(DF,REG) ((DF)->use_regs[(REG)]->reg_chain)
#define DF_REG_USE_COUNT(DF,REG) ((DF)->use_regs[(REG)]->n_refs)
#define DF_REG_EQ_USE_GET(DF,REG) ((DF)->eq_use_regs[(REG)])
#define DF_REG_EQ_USE_CHAIN(DF,REG) ((DF)->eq_use_regs[(REG)]->reg_chain)
#define DF_REG_EQ_USE_COUNT(DF,REG) ((DF)->eq_use_regs[(REG)]->n_refs)

/* Macros to access the elements within the reg_info structure table.  */

#define DF_REGNO_FIRST_DEF(DF, REGNUM) \
(DF_REG_DEF_GET(DF, REGNUM) ? DF_REG_DEF_GET(DF, REGNUM) : 0)
#define DF_REGNO_LAST_USE(DF, REGNUM) \
(DF_REG_USE_GET(DF, REGNUM) ? DF_REG_USE_GET(DF, REGNUM) : 0)

/* Macros to access the elements within the insn_info structure table.  */

#define DF_INSN_SIZE(DF) ((DF)->insns_size)
#define DF_INSN_GET(DF,INSN) ((DF)->insns[(INSN_UID(INSN))])
#define DF_INSN_SET(DF,INSN,VAL) ((DF)->insns[(INSN_UID (INSN))]=(VAL))
#define DF_INSN_CONTAINS_ASM(DF, INSN) (DF_INSN_GET(DF,INSN)->contains_asm)
#define DF_INSN_LUID(DF,INSN) (DF_INSN_GET(DF,INSN)->luid)
#define DF_INSN_DEFS(DF,INSN) (DF_INSN_GET(DF,INSN)->defs)
#define DF_INSN_USES(DF,INSN) (DF_INSN_GET(DF,INSN)->uses)
#define DF_INSN_EQ_USES(DF,INSN) (DF_INSN_GET(DF,INSN)->eq_uses)

#define DF_INSN_UID_GET(DF,UID) ((DF)->insns[(UID)])
#define DF_INSN_UID_SAFE_GET(DF,UID) (((UID) < DF_INSN_SIZE (DF)) \
                                     ? DF_INSN_UID_GET (DF, UID) \
                                     : NULL)
#define DF_INSN_UID_LUID(DF,INSN) (DF_INSN_UID_GET(DF,INSN)->luid)
#define DF_INSN_UID_DEFS(DF,INSN) (DF_INSN_UID_GET(DF,INSN)->defs)
#define DF_INSN_UID_USES(DF,INSN) (DF_INSN_UID_GET(DF,INSN)->uses)
#define DF_INSN_UID_EQ_USES(DF,INSN) (DF_INSN_UID_GET(DF,INSN)->eq_uses)
#define DF_INSN_UID_MWS(DF,INSN) (DF_INSN_UID_GET(DF,INSN)->mw_hardregs)

/* This is a bitmap copy of regs_invalidated_by_call so that we can
   easily add it into bitmaps, etc. */ 

extern bitmap df_invalidated_by_call;


/* One of these structures is allocated for every basic block.  */
struct df_scan_bb_info
{
  /* Defs at the start of a basic block that is the target of an
     exception edge.  */
  struct df_ref *artificial_defs;

  /* Uses of hard registers that are live at every block.  */
  struct df_ref *artificial_uses;
};


/* Reaching uses.  All bitmaps are indexed by the id field of the ref
   except sparse_kill (see below).  */
struct df_ru_bb_info 
{
  /* Local sets to describe the basic blocks.  */
  /* The kill set is the set of uses that are killed in this block.
     However, if the number of uses for this register is greater than
     DF_SPARSE_THRESHOLD, the sparse_kill is used instead. In
     sparse_kill, each register gets a slot and a 1 in this bitvector
     means that all of the uses of that register are killed.  This is
     a very useful efficiency hack in that it keeps from having push
     around big groups of 1s.  This is implemented by the
     bitmap_clear_range call.  */

  bitmap kill;
  bitmap sparse_kill;
  bitmap gen;   /* The set of uses generated in this block.  */

  /* The results of the dataflow problem.  */
  bitmap in;    /* At the top of the block.  */
  bitmap out;   /* At the bottom of the block.  */
};


/* Reaching definitions.  All bitmaps are indexed by the id field of
   the ref except sparse_kill (see above).  */
struct df_rd_bb_info 
{
  /* Local sets to describe the basic blocks.  See the note in the RU
     datastructures for kill and sparse_kill.  */
  bitmap kill;  
  bitmap sparse_kill;
  bitmap gen;   /* The set of defs generated in this block.  */

  /* The results of the dataflow problem.  */
  bitmap in;    /* At the top of the block.  */
  bitmap out;   /* At the bottom of the block.  */
};


/* Live registers.  All bitmaps are referenced by the register number.  */
struct df_lr_bb_info 
{
  /* Local sets to describe the basic blocks.  */
  bitmap def;   /* The set of registers set in this block.  */
  bitmap use;   /* The set of registers used in this block.  */

  /* The results of the dataflow problem.  */
  bitmap in;    /* At the top of the block.  */
  bitmap out;   /* At the bottom of the block.  */
};


/* Uninitialized registers.  All bitmaps are referenced by the register number.  */
struct df_ur_bb_info 
{
  /* Local sets to describe the basic blocks.  */
  bitmap kill;  /* The set of registers unset in this block.  Calls,
		   for instance, unset registers.  */
  bitmap gen;   /* The set of registers set in this block.  */

  /* The results of the dataflow problem.  */
  bitmap in;    /* At the top of the block.  */
  bitmap out;   /* At the bottom of the block.  */
};

/* Anded results of LR and UR.  */
struct df_live_bb_info 
{
  /* The results of the dataflow problem.  */
  bitmap in;    /* At the top of the block.  */
  bitmap out;   /* At the bottom of the block.  */
};


/* Uninitialized registers.  All bitmaps are referenced by the register number.  */
struct df_urec_bb_info 
{
  /* Local sets to describe the basic blocks.  */
  bitmap earlyclobber;  /* The set of registers that are referenced
			   with an an early clobber mode.  */
  /* Kill and gen are defined as in the UR problem.  */
  bitmap kill;
  bitmap gen;

  /* The results of the dataflow problem.  */
  bitmap in;    /* At the top of the block. 
                   See df_urec_local_finalize for caveat.  */
  bitmap out;   /* At the bottom of the block.  */
};


#define df_finish(df) {df_finish1(df); df=NULL;}

/* This is used for debugging and for the dumpers to find the latest
   instance so that the df info can be added to the dumps.  This
   should not be used by regular code.  */ 
extern struct df *df_current_instance;

/* Functions defined in df-core.c.  */

extern struct df *df_init (enum df_permanent_flags, enum df_changeable_flags);
extern struct dataflow *df_add_problem (struct df *, struct df_problem *);
extern void df_remove_problem (struct df *, struct dataflow *);
extern enum df_changeable_flags df_set_flags (struct df *, enum df_changeable_flags);
extern enum df_changeable_flags df_clear_flags (struct df *, enum df_changeable_flags);
extern void df_set_blocks (struct df*, bitmap);
extern void df_delete_basic_block (int);
extern void df_finish1 (struct df *);
extern void df_analyze_problem (struct dataflow *, bitmap, bitmap, bitmap, int *, int, bool);
extern void df_analyze (struct df *);
extern int df_get_n_blocks (struct df *);
extern int *df_get_postorder (struct df *);
extern void df_simple_iterative_dataflow (enum df_flow_dir, df_init_function,
					  df_confluence_function_0, df_confluence_function_n,
					  df_transfer_function, bitmap, int *, int);
extern void df_mark_solutions_dirty (struct df *);
extern bool df_get_bb_dirty (basic_block);
extern void df_set_bb_dirty (basic_block);
extern void df_compact_blocks (void);
extern void df_bb_replace (int, basic_block);
extern struct df_ref *df_bb_regno_last_use_find (struct df *, basic_block, unsigned int);
extern struct df_ref *df_bb_regno_first_def_find (struct df *, basic_block, unsigned int);
extern struct df_ref *df_bb_regno_last_def_find (struct df *, basic_block, unsigned int);
extern bool df_insn_regno_def_p (struct df *, rtx, unsigned int);
extern struct df_ref *df_find_def (struct df *, rtx, rtx);
extern bool df_reg_defined (struct df *, rtx, rtx);
extern struct df_ref *df_find_use (struct df *, rtx, rtx);
extern bool df_reg_used (struct df *, rtx, rtx);
extern void df_iterative_dataflow (struct dataflow *, bitmap, bitmap, int *, int, bool);
extern void df_print_regset (FILE *file, bitmap r);
extern void df_dump (struct df *, FILE *);
extern void df_dump_start (struct df *, FILE *);
extern void df_dump_top (struct df *, basic_block, FILE *);
extern void df_dump_bottom (struct df *, basic_block, FILE *);
extern void df_refs_chain_dump (struct df_ref *, bool, FILE *);
extern void df_regs_chain_dump (struct df *, struct df_ref *,  FILE *);
extern void df_insn_debug (struct df *, rtx, bool, FILE *);
extern void df_insn_debug_regno (struct df *, rtx, FILE *);
extern void df_regno_debug (struct df *, unsigned int, FILE *);
extern void df_ref_debug (struct df_ref *, FILE *);
extern void debug_df_insn (rtx);
extern void debug_df_regno (unsigned int);
extern void debug_df_reg (rtx);
extern void debug_df_defno (unsigned int);
extern void debug_df_useno (unsigned int);
extern void debug_df_ref (struct df_ref *);
extern void debug_df_chain (struct df_link *);
/* An instance of df that can be shared between global_alloc and the
   reload passes.  */
extern struct df *ra_df; 


/* Functions defined in df-problems.c. */

extern struct df_link *df_chain_create (struct dataflow *, struct df_ref *, struct df_ref *);
extern void df_chain_unlink (struct dataflow *, struct df_ref *, struct df_link *);
extern void df_chain_copy (struct dataflow *, struct df_ref *, struct df_link *);
extern bitmap df_get_live_in (struct df *, basic_block);
extern bitmap df_get_live_out (struct df *, basic_block);
extern void df_grow_bb_info (struct dataflow *);
extern void df_chain_dump (struct df_link *, FILE *);
extern void df_print_bb_index (basic_block bb, FILE *file);
extern struct dataflow *df_ru_add_problem (struct df *);
extern struct df_ru_bb_info *df_ru_get_bb_info (struct dataflow *, unsigned int);
extern struct dataflow *df_rd_add_problem (struct df *);
extern struct df_rd_bb_info *df_rd_get_bb_info (struct dataflow *, unsigned int);
extern struct dataflow *df_lr_add_problem (struct df *);
extern struct df_lr_bb_info *df_lr_get_bb_info (struct dataflow *, unsigned int);
extern void df_lr_simulate_artificial_refs_at_end (struct df *, basic_block, bitmap);
extern void df_lr_simulate_one_insn (struct df *, basic_block, rtx, bitmap);
extern struct dataflow *df_ur_add_problem (struct df *);
extern struct df_ur_bb_info *df_ur_get_bb_info (struct dataflow *, unsigned int);
extern struct dataflow *df_live_add_problem (struct df *);
extern struct df_live_bb_info *df_live_get_bb_info (struct dataflow *, unsigned int);
extern struct dataflow *df_urec_add_problem (struct df *);
extern struct df_urec_bb_info *df_urec_get_bb_info (struct dataflow *, unsigned int);
extern struct dataflow *df_chain_add_problem (struct df *);
extern struct dataflow *df_ri_add_problem (struct df *);
extern bitmap df_ri_get_setjmp_crosses (struct df *);

extern void df_urec_get_live_at_top (struct df *, basic_block, bitmap);

/* Functions defined in df-scan.c.  */

extern void df_scan_alloc (struct dataflow *, bitmap, bitmap);
extern struct df_scan_bb_info *df_scan_get_bb_info (struct dataflow *, unsigned int);
extern struct dataflow *df_scan_add_problem (struct df *);
extern void df_scan_blocks (struct df *);
extern struct df_ref *df_ref_create (struct df *, rtx, rtx *, rtx,basic_block,enum df_ref_type, enum df_ref_flags);
extern struct df_ref *df_get_artificial_defs (struct df *, unsigned int);
extern struct df_ref *df_get_artificial_uses (struct df *, unsigned int);
extern void df_reg_chain_create (struct df_reg_info *, struct df_ref *);
extern struct df_ref *df_reg_chain_unlink (struct dataflow *, struct df_ref *);
extern void df_ref_remove (struct df *, struct df_ref *);
extern void df_insn_create_insn_record (struct dataflow *, rtx);
extern void df_insn_delete (rtx);
extern void df_bb_delete (unsigned int);
extern bool df_insn_rescan (rtx);
extern void df_insn_refs_record (struct dataflow *, rtx, struct df_ref *);
extern bool df_has_eh_preds (basic_block);
extern void df_recompute_luids (struct df *, basic_block);
extern void df_insn_change_bb (rtx);
extern void df_maybe_reorganize_use_refs (struct df *);
extern void df_maybe_reorganize_def_refs (struct df *);
extern void df_hard_reg_init (void);
extern bool df_read_modify_subreg_p (rtx);
extern bool df_verify_blocks (struct dataflow *, bitmap);
extern void df_compute_regs_ever_live (struct df *, char [FIRST_PSEUDO_REGISTER]);

/* web */

/* This entry is allocated for each reference in the insn stream.  */
struct web_entry
{
  /* Pointer to the parent in the union/find tree.  */
  struct web_entry *pred;
  /* Newly assigned register to the entry.  Set only for roots.  */
  rtx reg;
  void* extra_info;
};

extern struct web_entry *unionfind_root (struct web_entry *);
extern bool unionfind_union (struct web_entry *, struct web_entry *);
extern void union_defs (struct df *, struct df_ref *,
                        struct web_entry *, struct web_entry *,
			bool (*fun) (struct web_entry *, struct web_entry *));


#endif /* GCC_DF_H */
