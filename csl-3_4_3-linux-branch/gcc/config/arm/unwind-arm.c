/* ARM EABI compliant unwinding routines/
   Copyright (C) 2004  Free Software Foundation, Inc.
   Contributed by Paul Brook

   This file is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; either version 2, or (at your option) any
   later version.

   In addition to the permissions in the GNU General Public License, the
   Free Software Foundation gives you unlimited permission to link the
   compiled version of this file into combinations with other programs,
   and to distribute those combinations without any restriction coming
   from the use of this file.  (The General Public License restrictions
   do apply in other respects; for example, they cover modification of
   the file, and distribution when not linked into a combine
   executable.)

   This file is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING.  If not, write to
   the Free Software Foundation, 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.  */
#include "unwind-arm.h"

/* Definitions for C++ runtime support routines.  We make these weak
   declarations to avoid pulling in libsupc++ unneccesarily.  */
typedef unsigned char bool;

typedef struct _ZSt9type_info type_info; /* This names C++ type_info type */

void __attribute__((weak)) __cxa_call_unexpected(_Unwind_Control_Block *ucbp);
bool __attribute__((weak)) __cxa_begin_cleanup(_Unwind_Control_Block *ucbp);
bool __attribute__((weak)) __cxa_type_match(_Unwind_Control_Block *ucbp,
					    const type_info *rttip,
					    void **matched_object);

_Unwind_Ptr __attribute__((weak))
__gnu_Unwind_Find_exidx (_Unwind_Ptr, int *);

/* Misc constants.  */
#define R_IP	12
#define R_SP	13
#define R_LR	14
#define R_PC	15

#define EXIDX_CANTUNWIND 1
#define uint32_highbit (((_uw) 1) << 31)

#define UCB_PR_ADDR(ucbp) ((ucbp)->unwinder_cache.reserved2)
#define UCB_SAVED_CALLSITE_ADDR(ucbp) ((ucbp)->unwinder_cache.reserved3)

struct core_regs
{
  _uw r[16];
};

/* We use normal integer types here to avoid the compiler generating
   coprocessor instructions.  */
struct vfp_regs
{
  _uw64 d[16];
  _uw pad;
};

struct fpa_reg
{
  _uw w[3];
};

struct fpa_regs
{
  struct fpa_reg f[8];
};

/* Unwind descriptors.  */

typedef struct
{
  _uw16 length;
  _uw16 offset;
} EHT16;

typedef struct
{
  _uw length;
  _uw offset;
} EHT32;

/* The ABI specifies that the unwind routines may only use core registers,
   except when actually manipulating coprocessor state).  This allows
   us to write one implementation that works on all platforms by
   demand-saving coprocessor registers.

   During unwinding we hold the coprocessor state in the actual hardware
   registers and allocate demand-save areas for use during phase1
   unwinding.  */

typedef struct
{
  /* The first fields must be the same as a phase2_vrs.  */
  _uw demand_save_flags;
  struct core_regs core;
  struct vfp_regs vfp;
  struct fpa_regs fpa;
} phase1_vrs;

#define DEMAND_SAVE_VFP 1

/* This must match the structure created by the assembly wrappers.  */
typedef struct
{
  _uw demand_save_flags;
  struct core_regs core;
} phase2_vrs;


/* An exeption index table entry.  */

typedef struct __EIT_entry
{
  _uw fnoffset;
  _uw content;
} __EIT_entry;

/* Assembly helper functions.  */

/* Restore core register state.  Never returns.  */
void __attribute__((noreturn)) restore_core_regs (struct core_regs *);


/* Register state manipulation functions.  */

void __gnu_Unwind_Save_VFP (struct vfp_regs * p);
void __gnu_Unwind_Restore_VFP (struct vfp_regs * p);

/* Restore coprocessor state after phase1 unwinding.  */
static void
restore_non_core_regs (phase1_vrs * vrs)
{
  if ((vrs->demand_save_flags & DEMAND_SAVE_VFP) == 0)
    __gnu_Unwind_Restore_VFP (&vrs->vfp);
}

/* A better way to do this would probably be to compare the absolute address
   with a segment relative relocation of the same symbol.  */

extern int __text_start;
extern int __data_start;

/* The exception index table location.  */
extern __EIT_entry __exidx_start;
extern __EIT_entry __exidx_end;

/* ABI defined personality routines.  */
extern _Unwind_Reason_Code __aeabi_unwind_cpp_pr0 (_Unwind_State,
    _Unwind_Control_Block *, _Unwind_Context *);// __attribute__((weak));
extern _Unwind_Reason_Code __aeabi_unwind_cpp_pr1 (_Unwind_State,
    _Unwind_Control_Block *, _Unwind_Context *) __attribute__((weak));
extern _Unwind_Reason_Code __aeabi_unwind_cpp_pr2 (_Unwind_State,
    _Unwind_Control_Block *, _Unwind_Context *) __attribute__((weak));

/* Store a virtual register to memory.  */

_Unwind_VRS_Result _Unwind_VRS_Get (_Unwind_Context *context,
				    _Unwind_VRS_RegClass regclass,
				    _uw regno,
				    _Unwind_VRS_DataRepresentation representation,
				    void *valuep)
{
  phase1_vrs *vrs = (phase1_vrs *) context;

  switch (regclass)
    {
    case _UVRSC_CORE:
      if (representation != _UVRSD_UINT32
	  || regno > 15)
	return _UVRSR_FAILED;
      *(_uw *) valuep = vrs->core.r[regno];
      return _UVRSR_OK;

    case _UVRSC_VFP:
    case _UVRSC_FPA:
    case _UVRSC_WMMXD:
    case _UVRSC_WMMXC:
      return _UVRSR_NOT_IMPLEMENTED;

    default:
      return _UVRSR_FAILED;
    }
}


/* Load a virtual register from memory.  */

_Unwind_VRS_Result _Unwind_VRS_Set (_Unwind_Context *context,
				    _Unwind_VRS_RegClass regclass,
				    _uw regno,
				    _Unwind_VRS_DataRepresentation representation,
				    void *valuep)
{
  phase1_vrs *vrs = (phase1_vrs *) context;

  switch (regclass)
    {
    case _UVRSC_CORE:
      if (representation != _UVRSD_UINT32
	  || regno > 15)
	return _UVRSR_FAILED;

      vrs->core.r[regno] = *(_uw *) valuep;
      return _UVRSR_OK;

    case _UVRSC_VFP:
    case _UVRSC_FPA:
    case _UVRSC_WMMXD:
    case _UVRSC_WMMXC:
      return _UVRSR_NOT_IMPLEMENTED;

    default:
      return _UVRSR_FAILED;
    }
}


/* Pop registers off the stack.  */

_Unwind_VRS_Result _Unwind_VRS_Pop (_Unwind_Context *context,
				    _Unwind_VRS_RegClass regclass,
				    _uw discriminator,
				    _Unwind_VRS_DataRepresentation representation)
{
  phase1_vrs *vrs = (phase1_vrs *) context;

  switch (regclass)
    {
    case _UVRSC_CORE:
      {
	_uw *ptr;
	_uw mask;
	int i;

	if (representation != _UVRSD_UINT32)
	  return _UVRSR_FAILED;

	mask = discriminator & 0xffff;
	ptr = (_uw *) vrs->core.r[R_SP];
	/* Pop the requested registers.  */
	for (i = 0; i < 16; i++)
	  {
	    if (mask & (1 << i))
	      vrs->core.r[i] = *(ptr++);
	  }
	/* Writeback the stack pointer value if it wasn't restored.  */
	if ((mask & (1 << R_SP)) == 0)
	  vrs->core.r[R_SP] = (_uw) ptr;
      }
      return _UVRSR_OK;

    case _UVRSC_VFP:
      {
	_uw start = discriminator >> 16;
	_uw count = discriminator & 0xffff;
	struct vfp_regs tmp;
	_uw *sp;
	_uw *dest;

	if ((representation != _UVRSD_VFPX && representation != _UVRSD_DOUBLE)
	    || start + count > 16)
	  return _UVRSR_FAILED;

	if (vrs->demand_save_flags & DEMAND_SAVE_VFP)
	  {
	    /* Demand-save resisters for stage1.  */
	    vrs->demand_save_flags &= ~DEMAND_SAVE_VFP;
	    __gnu_Unwind_Save_VFP (&vrs->vfp);
	  }

	/* Restore the registers from the stack.  Do this by saving the
	   current VFP registers to a memory area, moving the in-memory
	   values into that area, and restoring from the whole area.
	   For _UVRSD_VFPX we assume FSTMX standard format 1.  */
	__gnu_Unwind_Save_VFP (&tmp);

	/* The stack address is only guaranteed to be word aligned, so
	   we can't use doubleword copies.  */
	sp = (_uw *) vrs->core.r[R_SP];
	dest = (_uw *) &tmp.d[start];
	count *= 2;
	while (count--)
	  *(dest++) = *(sp++);

	/* Skip the pad word */
	if (representation == _UVRSD_VFPX)
	  sp++;

	/* Set the new stack pointer.  */
	vrs->core.r[R_SP] = (_uw) sp;

	/* Reload the registers.  */
	__gnu_Unwind_Restore_VFP (&tmp);
      }
      return _UVRSR_OK;

    case _UVRSC_FPA:
    case _UVRSC_WMMXD:
    case _UVRSC_WMMXC:
      return _UVRSR_NOT_IMPLEMENTED;

    default:
      return _UVRSR_FAILED;
    }
}


/* Core unwinding functions.  */

/* Dereference a 31-bit self-relative offset.  */
static inline _uw
selfrel_offset31 (const _uw *p)
{
  _uw offset;

  offset = *p;
  /* Sign extend to 32 bits.  */
  if (offset & (1 << 30))
    offset |= 1u << 31;

  return offset + (_uw) p;
}


/* Perform a binary search of an index table.  */

static const __EIT_entry *
search_EIT_table (const __EIT_entry * table, int nrec, _uw return_address)
{
  _uw next_fn;
  _uw this_fn;
  int n, left, right;

  if (nrec == 0)
    return (__EIT_entry *) 0;

  left = 0;
  right = nrec - 1;

  while (1)
    {
      n = (left + right) / 2;
      this_fn = selfrel_offset31 (&table[n].fnoffset);
      if (n != nrec - 1)
	next_fn = selfrel_offset31 (&table[n + 1].fnoffset);
      else
	next_fn = ~(_uw) 0;

      if (return_address < this_fn)
	{
	  if (n == left)
	    return (__EIT_entry *) 0;
	  right = n - 1;
	}
      else if (return_address < next_fn)
	return &table[n];
      else
	left = n + 1;
    }
}

/* Find the exception index table eintry for the given address.
   Fill in the relevant fields of the UCB.  */

static _Unwind_Reason_Code
get_eit_entry (_Unwind_Control_Block *ucbp, _uw return_address)
{
  const __EIT_entry * eitp;
  int nrec;
  
  if (__gnu_Unwind_Find_exidx)
    {
      eitp = (const __EIT_entry *) __gnu_Unwind_Find_exidx (return_address,
							    &nrec);
      if (!eitp)
	{
	  UCB_PR_ADDR (ucbp) = 0;
	  return _URC_FAILURE;
	}
    }
  else
    {
      eitp = &__exidx_start;
      nrec = &__exidx_end - &__exidx_start;
    }

  eitp = search_EIT_table (eitp, nrec, return_address);

  if (!eitp)
    {
      UCB_PR_ADDR (ucbp) = 0;
      return _URC_FAILURE;
    }
  ucbp->pr_cache.fnstart = selfrel_offset31 (&eitp->fnoffset);

  /* Can this frame be unwound at all?  */
  if (eitp->content == EXIDX_CANTUNWIND)
    {
      UCB_PR_ADDR (ucbp) = 0;
      return _URC_FAILURE;
    }

  /* Obtain the address of the "real" __EHT_Header word.  */

  if (eitp->content & uint32_highbit)
    {
      /* It is immediate data.  */
      ucbp->pr_cache.ehtp = (_Unwind_EHT_Header *)&eitp->content;
      ucbp->pr_cache.additional = 1;
    }
  else
    {
      /* The low 31 bits of the content field are a self-relative
	 offset to an _Unwind_EHT_Entry structure.  */
      ucbp->pr_cache.ehtp =
	(_Unwind_EHT_Header *) selfrel_offset31 (&eitp->content);
      ucbp->pr_cache.additional = 0;
    }

  /* Discover the personality routine address.  */
  if (*ucbp->pr_cache.ehtp & (1u << 31))
    {
      /* One of the predefined standard routines.  */
      _uw idx = (*(_uw *) ucbp->pr_cache.ehtp >> 24) & 0xf;
      if (idx == 0)
	UCB_PR_ADDR (ucbp) = (_uw) &__aeabi_unwind_cpp_pr0;
      else if (idx == 1)
	UCB_PR_ADDR (ucbp) = (_uw) &__aeabi_unwind_cpp_pr1;
      else if (idx == 2)
	UCB_PR_ADDR (ucbp) = (_uw) &__aeabi_unwind_cpp_pr2;
      else
	{ /* Failed */
	  UCB_PR_ADDR (ucbp) = 0;
	  return _URC_FAILURE;
	}
    } 
  else
    {
      /* Execute region offset to PR */
      UCB_PR_ADDR (ucbp) = selfrel_offset31 (ucbp->pr_cache.ehtp);
    }
  return _URC_OK;
}


/* Perform phase2 unwinding.  */

static void __attribute__((noreturn))
unwind_phase2 (_Unwind_Control_Block * ucbp, phase2_vrs * vrs)
{
  _Unwind_Reason_Code pr_result;

  for(;;)
    {
      /* Find the entry for this routine.  */
      if (get_eit_entry (ucbp, vrs->core.r[R_PC]) != _URC_OK)
	abort ();

      UCB_SAVED_CALLSITE_ADDR (ucbp) = vrs->core.r[R_PC];
      
      /* Call the pr to decide what to do.  */
      pr_result = ((personality_routine) UCB_PR_ADDR (ucbp))
	(_US_UNWIND_FRAME_STARTING, ucbp, (_Unwind_Context *) vrs);

      if (pr_result != _URC_CONTINUE_UNWIND)
	break;
    }
  
  if (pr_result != _URC_INSTALL_CONTEXT)
    abort();
  
  restore_core_regs (&vrs->core);
}

/* Perform phase1 unwinding.  */

_Unwind_Reason_Code
__gnu_Unwind_RaiseException (_Unwind_Control_Block *, phase2_vrs *);

_Unwind_Reason_Code
__gnu_Unwind_RaiseException (_Unwind_Control_Block * ucbp,
			     phase2_vrs * entry_vrs)
{
  phase1_vrs saved_vrs;
  _Unwind_Reason_Code pr_result;

  /* Set the pc to the call site.  */
  entry_vrs->core.r[R_PC] = entry_vrs->core.r[R_LR];

  /* Save the core registers.  */
  saved_vrs.core = entry_vrs->core;
  /* Set demand-save flags.  */
  saved_vrs.demand_save_flags = ~(_uw) 0;
  
  /* Unwind until we reach a propagation barrier.  */
  for (;;)
    {
      /* Find the entry for this routine.  */
      if (get_eit_entry (ucbp, saved_vrs.core.r[R_PC]) != _URC_OK)
	return _URC_FAILURE;

      /* Call the pr to decide what to do.  */
      pr_result = ((personality_routine) UCB_PR_ADDR (ucbp))
	(_US_VIRTUAL_UNWIND_FRAME, ucbp, (void *) &saved_vrs);

      if (pr_result != _URC_CONTINUE_UNWIND)
	break;
    }

  /* We've unwound as far as we want to go, so restore the original
     register state.  */
  restore_non_core_regs (&saved_vrs);
  if (pr_result != _URC_HANDLER_FOUND)
    {
      /* Some sort of failure has occurred in the pr and probably the
	 pr returned _URC_FAILURE.  */
      return _URC_FAILURE;
    }
  
  unwind_phase2 (ucbp, entry_vrs);
}

_Unwind_Reason_Code
__gnu_Unwind_Resume (_Unwind_Control_Block *, phase2_vrs *);

_Unwind_Reason_Code
__gnu_Unwind_Resume (_Unwind_Control_Block * ucbp, phase2_vrs * entry_vrs)
{
  _Unwind_Reason_Code pr_result;

  /* Recover the saved address.  */
  entry_vrs->core.r[R_PC] = UCB_SAVED_CALLSITE_ADDR (ucbp);
  
  /* Call the cached PR.  */
  pr_result = ((personality_routine) UCB_PR_ADDR (ucbp))
	(_US_UNWIND_FRAME_RESUME, ucbp, (_Unwind_Context *) entry_vrs);

  switch (pr_result)
    {
    case _URC_INSTALL_CONTEXT:
      /* Upload the registers to enter the landing pad.  */
      restore_core_regs (&entry_vrs->core);

    case _URC_CONTINUE_UNWIND:
      /* Continue unwinding the next frame.  */
      unwind_phase2 (ucbp, entry_vrs);

    default:
      abort ();
    }
}

void
_Unwind_Complete (_Unwind_Control_Block * ucbp __attribute__((unused)))
{
}

/* Personality routine helper functions.  These should go in a separate
   file so they can be used with the arm runtime.  */

#define CODE_FINISH (0xb0)

static inline _uw8
next_unwind_byte (__gnu_unwind_state * uws)
{
  _uw8 b;

  if (uws->bytes_left == 0)
    {
      /* Load another word */
      if (uws->words_left == 0)
	return CODE_FINISH; /* Nothing left.  */
      uws->words_left--;
      uws->data = *(uws->next++);
      uws->bytes_left = 3;
    }
  else
    uws->bytes_left--;

  /* Extract the most significant byte.  */
  b = (uws->data >> 24) & 0xff;
  uws->data <<= 8;
  return b;
}

static _Unwind_Reason_Code
__gnu_unwind_execute (_Unwind_Context * context, __gnu_unwind_state * uws)
{
  _uw op;
  int set_pc;
  _uw reg;

  set_pc = 0;
  for (;;)
    {
      op = next_unwind_byte (uws);
      if (op == CODE_FINISH)
	{
	  /* If we haven't already set pc then copy it from lr.  */
	  if (!set_pc)
	    {
	      _Unwind_VRS_Get (context, _UVRSC_CORE, R_LR, _UVRSD_UINT32,
			       &reg);
	      _Unwind_VRS_Set (context, _UVRSC_CORE, R_PC, _UVRSD_UINT32,
			       &reg);
	      set_pc = 1;
	    }
	  /* Drop out of the loop.  */
	  break;
	}
      if ((op & 0x80) == 0)
	{
	  /* vsp = vsp +- (imm6 << 2 + 4).  */
	  _uw offset;

	  offset = ((op & 0x3f) << 2) + 4;
	  _Unwind_VRS_Get (context, _UVRSC_CORE, R_SP, _UVRSD_UINT32, &reg);
	  if (op & 0x40)
	    reg -= offset;
	  else
	    reg += offset;
	  _Unwind_VRS_Set (context, _UVRSC_CORE, R_SP, _UVRSD_UINT32, &reg);
	  continue;
	}
      
      if ((op & 0xf0) == 0x80)
	{
	  op = (op << 8) | next_unwind_byte (uws);
	  if (op == 0x8000)
	    {
	      /* Refuse to unwind.  */
	      return _URC_FAILURE;
	    }
	  /* Pop r4-r15 under mask.  */
	  op = (op << 4) & 0xfff0;
	  if (_Unwind_VRS_Pop (context, _UVRSC_CORE, op, _UVRSD_UINT32)
	      != _UVRSR_OK)
	    return _URC_FAILURE;
	  if (op & (1 << R_PC))
	    set_pc = 1;
	  continue;
	}
      if ((op & 0xf0) == 0x90)
	{
	  op &= 0xf;
	  if (op == 13 || op == 15)
	    /* Reserved.  */
	    return _URC_FAILURE;
	  /* vsp = r[nnnn].  */
	  _Unwind_VRS_Get (context, _UVRSC_CORE, op, _UVRSD_UINT32, &reg);
	  _Unwind_VRS_Set (context, _UVRSC_CORE, R_SP, _UVRSD_UINT32, &reg);
	  continue;
	}
      if ((op & 0xf0) == 0xa0)
	{
	  /* Pop r4-r[4+nnn], [lr].  */
	  _uw mask;
	  
	  mask = (0xff0 >> (7 - (op & 7))) & 0xff0;
	  if (op & 8)
	    mask |= (1 << R_LR);
	  if (_Unwind_VRS_Pop (context, _UVRSC_CORE, mask, _UVRSD_UINT32)
	      != _UVRSR_OK)
	    return _URC_FAILURE;
	  continue;
	}
      if ((op & 0xf0) == 0xb0)
	{
	  /* op == 0xb0 already handled.  */
	  if (op == 0xb1)
	    {
	      op = next_unwind_byte (uws);
	      if (op == 0 || ((op & 0xf0) != 0))
		/* Spare.  */
		return _URC_FAILURE;
	      /* Pop r0-r4 under mask.  */
	      if (_Unwind_VRS_Pop (context, _UVRSC_CORE, op, _UVRSD_UINT32)
		  != _UVRSR_OK)
		return _URC_FAILURE;
	      continue;
	    }
	  if (op == 0xb2)
	    {
	      /* vsp = vsp + 0x204 + (uleb128 << 2).  */
	      int shift;

	      _Unwind_VRS_Get (context, _UVRSC_CORE, R_SP, _UVRSD_UINT32,
			       &reg);
	      op = next_unwind_byte (uws);
	      shift = 2;
	      while (op & 0x80)
		{
		  reg += ((op & 0x7f) << shift);
		  shift += 7;
		  op = next_unwind_byte (uws);
		}
	      reg += ((op & 0x7f) << shift) + 0x204;
	      _Unwind_VRS_Set (context, _UVRSC_CORE, R_SP, _UVRSD_UINT32,
			       &reg);
	      continue;
	    }
	  if (op == 0xb3)
	    {
	      /* Pop VFP registers with fldmx.  */
	      op = next_unwind_byte (uws);
	      op = ((op & 0xf0) << 12) | (op & 0xf);
	      if (_Unwind_VRS_Pop (context, _UVRSC_VFP, op, _UVRSD_VFPX)
		  != _UVRSR_OK)
		return _URC_FAILURE;
	      continue;
	    }
	  if ((op & 0xfc) == 0xb4)
	    {
	      /* Pop FPA E[4]-E[4+nn].  */
	      op = 0x40000 | ((op & 3) + 1);
	      if (_Unwind_VRS_Pop (context, _UVRSC_FPA, op, _UVRSD_FPAX)
		  != _UVRSR_OK)
		return _URC_FAILURE;
	      continue;
	    }
	  /* op & 0xf8 == 0xb8.  */
	  /* Pop VFP D[8]-D[8+nnn] with fldmx.  */
	  op = 0x80000 | ((op & 7) + 1);
	  if (_Unwind_VRS_Pop (context, _UVRSC_VFP, op, _UVRSD_VFPX)
	      != _UVRSR_OK)
	    return _URC_FAILURE;
	  continue;
	}
      if ((op & 0xf0) == 0xc0)
	{
	  if (op == 0xc6)
	    {
	      /* Pop iWMMXt D registers.  */
	      op = next_unwind_byte (uws);
	      op = ((op & 0xf0) << 12) | (op & 0xf);
	      if (_Unwind_VRS_Pop (context, _UVRSC_WMMXD, op, _UVRSD_UINT64)
		  != _UVRSR_OK)
		return _URC_FAILURE;
	      continue;
	    }
	  if (op == 0xc7)
	    {
	      op = next_unwind_byte (uws);
	      if (op == 0 || (op & 0xf0) != 0)
		/* Spare.  */
		return _URC_FAILURE;
	      /* Pop iWMMXt wCGR{3,2,1,0} under mask.  */
	      if (_Unwind_VRS_Pop (context, _UVRSC_WMMXC, op, _UVRSD_UINT32)
		  != _UVRSR_OK)
		return _URC_FAILURE;
	      continue;
	    }
	  if ((op & 0xf8) == 0xc0)
	    {
	      /* Pop iWMMXt wR[10]-wR[10+nnn].  */
	      op = 0xa0000 | ((op & 0xf) + 1);
	      if (_Unwind_VRS_Pop (context, _UVRSC_WMMXD, op, _UVRSD_UINT64)
		  != _UVRSR_OK)
		return _URC_FAILURE;
	      continue;
	    }
	  if (op == 0xc8)
	    {
	      /* Pop FPA registers.  */
	      op = next_unwind_byte (uws);
	      op = ((op & 0xf0) << 12) | (op & 0xf);
	      if (_Unwind_VRS_Pop (context, _UVRSC_FPA, op, _UVRSD_FPAX)
		  != _UVRSR_OK)
		return _URC_FAILURE;
	      continue;
	    }
	  if (op == 0xc9)
	    {
	      /* Pop VFP registers with fldmd.  */
	      op = next_unwind_byte (uws);
	      op = ((op & 0xf0) << 12) | (op & 0xf);
	      if (_Unwind_VRS_Pop (context, _UVRSC_VFP, op, _UVRSD_DOUBLE)
		  != _UVRSR_OK)
		return _URC_FAILURE;
	      continue;
	    }
	  /* Spare.  */
	  return _URC_FAILURE;
	}
      if ((op & 0xf8) == 0xd0)
	{
	  /* Pop VFP D[8]-D[8+nnn] with fldmd.  */
	  op = 0x80000 | ((op & 7) + 1);
	  if (_Unwind_VRS_Pop (context, _UVRSC_VFP, op, _UVRSD_DOUBLE)
	      != _UVRSR_OK)
	    return _URC_FAILURE;
	  continue;
	}
      /* Spare.  */
      return _URC_FAILURE;
    }
  return _URC_OK;
}


/* Execute the unwinding instructions associated with a frame.  */

_Unwind_Reason_Code
__gnu_unwind_frame (_Unwind_Control_Block * ucbp, _Unwind_Context * context)
{
  _uw *ptr;
  __gnu_unwind_state uws;

  ptr = (_uw *) ucbp->pr_cache.ehtp;
  /* Skip over the personality routine address.  */
  ptr++;
  /* Setup the unwinder state.  */
  uws.data = (*ptr) << 8;
  uws.next = ptr + 1;
  uws.bytes_left = 3;
  uws.words_left = ((*ptr) >> 24) & 0xff;

  return __gnu_unwind_execute (context, &uws);
}


/* Get the _Unwind_Control_Block from an _Unwind_Context.  */

static inline _Unwind_Control_Block *
unwind_UCB_from_context (_Unwind_Context * context)
{
  return (_Unwind_Control_Block *) _Unwind_GetGR (context, R_IP);
}


/* Find the Language specific exception data.  */

void *
_Unwind_GetLanguageSpecificData (_Unwind_Context * context)
{
  _Unwind_Control_Block *ucbp;
  _uw *ptr;

  /* Get a pointer to the exception table entry.  */
  ucbp = unwind_UCB_from_context (context);
  ptr = (_uw *) ucbp->pr_cache.ehtp;
  /* Skip the prersonality routine address.  */
  ptr++;
  /* Skip the unwind opcodes.  */
  ptr += (((*ptr) >> 24) & 0xff) + 1;

  return ptr;
}


/* Get the start address of the function being unwound.  */

_Unwind_Ptr
_Unwind_GetRegionStart (_Unwind_Context * context)
{
  _Unwind_Control_Block *ucbp;
  
  ucbp = unwind_UCB_from_context (context);
  return (_Unwind_Ptr) ucbp->pr_cache.fnstart;
}


/* Free an exception.  */

void
_Unwind_DeleteException (_Unwind_Exception * exc)
{
  if (exc->exception_cleanup)
    (*exc->exception_cleanup) (_URC_FOREIGN_EXCEPTION_CAUGHT, exc);
}


/* Common implementation for ARM personality routines.  */

static _Unwind_Reason_Code
__gnu_unwind_pr_common (_Unwind_State state,
			_Unwind_Control_Block *ucbp,
			_Unwind_Context *context,
			int id)
{
  __gnu_unwind_state uws;
  _uw *data;
  _uw offset;
  _uw len;
  _uw rtti_count;
  int phase2_call_unexpected_after_unwind = 0;
  int in_range = 0;

  data = (_uw *) ucbp->pr_cache.ehtp;
  uws.data = *(data++);
  uws.next = data;
  if (id == 0)
    {
      uws.data <<= 8;
      uws.words_left = 0;
      uws.bytes_left = 3;
    }
  else
    {
      uws.words_left = (uws.data >> 16) & 0xff;
      uws.data <<= 16;
      uws.bytes_left = 2;
      data += uws.words_left;
    }

  /* Restore the saved pointer.  */
  if (state == _US_UNWIND_FRAME_RESUME)
    data = (_uw *) ucbp->cleanup_cache.bitpattern[0];

  if ((ucbp->pr_cache.additional & 1) == 0)
    {
      /* Process descriptors.  */
      while (*data)
	{
	  _uw addr;
	  _uw fnstart;

	  if (id == 2)
	    {
	      len = ((EHT32 *) data)->length;
	      offset = ((EHT32 *) data)->offset;
	      data += 2;
	    }
	  else
	    {
	      len = ((EHT16 *) data)->length;
	      offset = ((EHT16 *) data)->offset;
	      data++;
	    }

	  fnstart = ucbp->pr_cache.fnstart + (offset & ~1);
	  addr = _Unwind_GetGR (context, R_PC);
	  in_range = (fnstart <= addr && addr < fnstart + (len & ~1));

	  switch (((offset & 1) << 1) | (len & 1))
	    {
	    case 0:
	      /* Cleanup.  */
	      if (state != _US_VIRTUAL_UNWIND_FRAME
		  && in_range)
		{
		  /* Cleanup in range, and we are running cleanups.  */
		  _uw lp;

		  /* Landing pad address is 31-bit pc-relatvie offset.  */
		  lp = selfrel_offset31 (data);
		  data++;
		  /* Save the exception data pointer.  */
		  ucbp->cleanup_cache.bitpattern[0] = (_uw) data;
		  if (!__cxa_begin_cleanup (ucbp))
		    return _URC_FAILURE;
		  /* Setup the VRS to enter the landing pad.  */
		  _Unwind_SetGR (context, R_PC, lp);
		  return _URC_INSTALL_CONTEXT;
		}
	      /* Cleanup not in range, or we are in stage 1.  */
	      data++;
	      break;

	    case 1:
	      /* Catch handler.  */
	      if (state == _US_VIRTUAL_UNWIND_FRAME)
		{
		  if (in_range)
		    {
		      /* Check for a barrier.  */
		      _uw rtti;
		      void *matched;

		      /* Check for no-throw areas.  */
		      if (data[1] == (_uw) -2)
			return _URC_FAILURE;

		      /* The thrown object immediately folows the ECB.  */
		      matched = (void *)(ucbp + 1);
		      if (data[1] != (_uw) -1)
			{
			  /* Match a catch specification.  */
			  rtti = _Unwind_decode_target2 ((_uw) &data[1]);
			  if (!__cxa_type_match (ucbp, (type_info *) rtti,
						 &matched))
			    matched = (void *)0;
			}

		      if (matched)
			{
			  ucbp->barrier_cache.sp =
			    _Unwind_GetGR (context, R_SP);
			  ucbp->barrier_cache.bitpattern[0] = (_uw) matched;
			  ucbp->barrier_cache.bitpattern[1] = (_uw) data;
			  return _URC_HANDLER_FOUND;
			}
		    }
		  /* Handler out of range, or not matched.  */
		}
	      else if (ucbp->barrier_cache.sp == _Unwind_GetGR (context, R_SP)
		       && ucbp->barrier_cache.bitpattern[1] == (_uw) data)
		{
		  /* Matched a previous propagation barrier.  */
		  _uw lp;

		  /* Setup for entry to the handler.  */
		  lp = selfrel_offset31 (data);
		  _Unwind_SetGR (context, R_PC, lp);
		  _Unwind_SetGR (context, 0, (_uw) ucbp);
		  return _URC_INSTALL_CONTEXT;
		}
	      /* Catch handler not mached.  Advance to the next descriptor.  */
	      data += 2;
	      break;

	    case 2:
	      rtti_count = data[0] & 0x7fffffff;
	      /* Exception specification.  */
	      if (state == _US_VIRTUAL_UNWIND_FRAME)
		{
		  if (in_range)
		    {
		      /* Match against teh exception specification.  */
		      _uw i;
		      _uw rtti;
		      void *matched;

		      for (i = 0; i < rtti_count; i++)
			{
			  matched = (void *)(ucbp + 1);
			  rtti = _Unwind_decode_target2 ((_uw) &data[i + 1]);
			  if (__cxa_type_match (ucbp, (type_info *) rtti,
						&matched))
			    break;
			}

		      if (i == rtti_count)
			{
			  /* Exception does not match the spec.  */
			  ucbp->barrier_cache.sp =
			    _Unwind_GetGR (context, R_SP);
			  ucbp->barrier_cache.bitpattern[0] = (_uw) matched;
			  ucbp->barrier_cache.bitpattern[1] = (_uw) data;
			  return _URC_HANDLER_FOUND;
			}
		    }
		  /* Handler out of range, or exception is permitted.  */
		}
	      else if (ucbp->barrier_cache.sp == _Unwind_GetGR (context, R_SP)
		       && ucbp->barrier_cache.bitpattern[1] == (_uw) data)
		{
		  /* Matched a previous propagation barrier.  */
		  _uw lp;
		  /* Record the RTTI list for __cxa_call_unexpected.  */
		  ucbp->barrier_cache.bitpattern[1] = rtti_count;
		  ucbp->barrier_cache.bitpattern[2] = 0;
		  ucbp->barrier_cache.bitpattern[3] = 4;
		  ucbp->barrier_cache.bitpattern[4] = (_uw) &data[1];

		  if (data[0] & uint32_highbit)
		    phase2_call_unexpected_after_unwind = 1;
		  else
		    {
		      data += rtti_count + 1;
		      /* Setup for entry to the handler.  */
		      lp = selfrel_offset31 (data);
		      data++;
		      _Unwind_SetGR (context, R_PC, lp);
		      _Unwind_SetGR (context, 0, (_uw) ucbp);
		      return _URC_INSTALL_CONTEXT;
		    }
		}
	      if (data[0] & uint32_highbit)
		data++;
	      data += rtti_count + 1;
	      break;

	    default:
	      /* Should never happen.  */
	      return _URC_FAILURE;
	    }
	  /* Finished processing this descriptor.  */
	}
    }

  if (__gnu_unwind_execute (context, &uws) != _URC_OK)
    return _URC_FAILURE;

  if (phase2_call_unexpected_after_unwind)
    {
      /* Enter __cxa_unexpected as if called from the callsite.  */
      _Unwind_SetGR (context, R_LR, _Unwind_GetGR (context, R_PC));
      _Unwind_SetGR (context, R_PC, (_uw) &__cxa_call_unexpected);
      return _URC_INSTALL_CONTEXT;
    }

  return _URC_CONTINUE_UNWIND;
}


/* ABI defined personality routine entry points.  */

_Unwind_Reason_Code
__aeabi_unwind_cpp_pr0 (_Unwind_State state,
			_Unwind_Control_Block *ucbp,
			_Unwind_Context *context)
{
  return __gnu_unwind_pr_common (state, ucbp, context, 0);
}

_Unwind_Reason_Code
__aeabi_unwind_cpp_pr1 (_Unwind_State state,
			_Unwind_Control_Block *ucbp,
			_Unwind_Context *context)
{
  return __gnu_unwind_pr_common (state, ucbp, context, 1);
}

_Unwind_Reason_Code
__aeabi_unwind_cpp_pr2 (_Unwind_State state,
			_Unwind_Control_Block *ucbp,
			_Unwind_Context *context)
{
  return __gnu_unwind_pr_common (state, ucbp, context, 2);
}
