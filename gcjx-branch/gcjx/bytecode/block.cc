// Bytecode blocks.

// Copyright (C) 2004 Free Software Foundation, Inc.
//
// This file is part of GCC.
//
// gcjx is free software; you can redistribute it and/or
// modify it under the terms of the GNU Library General Public
// License as published by the Free Software Foundation; either
// version 2 of the License, or (at your option) any later version.
//
// gcjx is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Library General Public License for more details.
//
// You should have received a copy of the GNU Library General Public
// License along with gcjx; see the file COPYING.LIB.  If
// not, write to the Free Software Foundation, Inc.,
// 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

#include "typedefs.hh"
#include "bytecode/outpool.hh"
#include "bytecode/locals.hh"
#include "bytecode/block.hh"
#include "bytecode/attribute.hh"
#include "bytecode/generate.hh"

bool
bytecode_block::optimize ()
{
  if (relocations.empty ())
    return false;

  bool changed = false;

  // First, rewrite goto L; L: by deleting the block's contents.
  if (! relocations.empty ())
    {
      ref_relocation last = relocations.back ();
      if (last->get_kind () == reloc_goto
	  && last->get_target () == next_block)
	{
	  // Remove the relocation and the byte representing the
	  // instruction.
	  relocations.pop_back ();
	  bytecode.pop_back ();
	  assert (! fall_through);
	  fall_through = true;
	  changed = true;
	}
    }

  // Rewrite
  //   if_x L; goto T; L:
  // into
  //   if_!x T;
  // This lets the code generator remain a little dumb while still
  // getting better code than the old gcj generator.
  if (! relocations.empty ())
    {
      std::list<ref_relocation>::reverse_iterator last_it
	= relocations.rbegin ();
      ref_relocation last = *last_it;
      ++last_it;
      if (last_it != relocations.rend ())
	{
	  ref_relocation last_but_one = *last_it;
	  if (last_but_one->conditional_p ()
	      && last_but_one->get_target () == next_block
	      && last->get_kind () == reloc_goto)
	    {
	      // Remove the goto relocation and the byte representing
	      // the instruction.
	      relocations.pop_back ();
	      bytecode.pop_back ();
	      assert (! fall_through);
	      fall_through = true;
	      // Invert the condition and reset to point where the
	      // goto pointed.
	      last_but_one->set_target (last->get_target ());
	      last_but_one->invert_condition ();
	      // Rewrite the bytecode.  We know that the relocation
	      // type can be directly converted to an instruction.
	      bytecode[bytecode.size () - 1]
		= jbyte (last_but_one->get_kind ());
	      changed = true;
	    }
	}
    }

  // Second, we rewrite goto L; ... L: goto X by changing the first
  // goto to jump directly to X.
  for (std::list<ref_relocation>::iterator i = relocations.begin ();
       i != relocations.end ();
       ++i)
    {
      (*i)->update ();

      ref_relocation iter = *i;
      int count = 0;
      // Maximum of 10 goto operations, in case there is some infinite
      // loop.  The first branch can be conditional, but subsequent
      // ones must be plain 'goto's.  FIXME we should do real
      // detection here.
      while ((iter->get_kind () == reloc_goto
	      || (count == 0 && iter->conditional_p ()))
	     && ! iter->get_target ()->bytecode.empty ()
	     && iter->get_target ()->bytecode[0] == jbyte (op_goto)
	     && count < 10)
	{
	  // The first relocation in the target must be a 'goto'.
	  ref_relocation targ_reloc = iter->get_target ()->relocations.back ();
	  if (targ_reloc->get_kind () != reloc_goto)
	    break;
	  ++count;
	  iter = targ_reloc;
	}
      if (iter != *i)
	{
#if 0 // FIXME this causes crashes
	  // ITER is the new target.
	  (*i)->set_target (iter->get_target ());
	  changed = true;
#endif
	}
    }

  return changed;
}

bytecode_block *
bytecode_block::update ()
{
  // If this block is empty, move to the next one.  Also skip dead
  // blocks.
  bytecode_block *iter = this;
  while (iter && (iter->empty_p () || ! iter->live_p ()))
    iter = iter->next ();
  return iter;
}

void
bytecode_block::copy (bytecode_stream *out, int from_offset, int to_offset)
{
  for (int i = from_offset; i < to_offset; ++i)
    out->put (bytecode[i]);
}

bool
bytecode_block::relocate (int &local_pc, bytecode_stream *out)
{
  bool changed = pc != local_pc;
  pc = local_pc;

  // Update 'next' and also all relocation targets.
  next_block = next_block->update ();
  assert (! fall_through || next_block);
  for (std::list<ref_relocation>::const_iterator i = relocations.begin ();
       i != relocations.end ();
       ++i)
    (*i)->update ();

  if (optimize ())
    changed = true;

  int cumulative_delta = 0;
  int previous_offset = 0;
  for (std::list<ref_relocation>::const_iterator i = relocations.begin ();
       i != relocations.end ();
       ++i)
    {
      int this_offset = (*i)->get_offset ();
      if (out)
	copy (out, previous_offset, this_offset);
      previous_offset = this_offset;

      if ((*i)->perform (local_pc + cumulative_delta + this_offset, pc, out))
	changed = true;
      // Add in the use length of the relocation.
      cumulative_delta += (*i)->get_use_length ();
    }
  if (out)
    copy (out, previous_offset, bytecode.size ());

  // If we're emitting bytecode, we better not have changed anything.
  assert (! out || ! changed);

  // The size of the bytecode doesn't include any use lengths.
  local_pc += cumulative_delta + bytecode.size ();
  return changed;
}

bytecode_block *
bytecode_block::clone (bytecode_generator *gen,
		       const bytecode_block *old_finish,
		       bytecode_block *new_finish) const
{
  bytecode_block *result = NULL;
  bytecode_block *prev = NULL;
  const bytecode_block *iter = this;

  std::map<const bytecode_block *, bytecode_block *> remap;
  remap[old_finish] = new_finish;

  // fixme could also == old_finish ?
  while (iter != NULL)
    {
      bytecode_block *newval = gen->new_bytecode_block ();
      newval->line = iter->line;
      newval->relocations = iter->relocations;
      newval->bytecode = iter->bytecode;
      newval->fall_through = iter->fall_through;

      if (prev)
	prev->next_block = newval;
      prev = newval;
      if (! result)
	result = newval;

      remap[iter] = newval;
      iter = iter->next_block;
    }

  // Now walk over the result and rewrite all the relocations.
  for (bytecode_block *riter = result;
       riter != NULL;
       riter = riter->next_block)
    {
      for (std::list<ref_relocation>::iterator i = riter->relocations.begin ();
	   i != riter->relocations.end ();
	   ++i)
	{
	  ref_relocation &reloc = *i;
	  bytecode_block *targ = reloc->get_target ();
	  std::map<const bytecode_block *, bytecode_block *>::const_iterator
	    found = remap.find (targ);
	  if (found != remap.end ())
	    reloc = new relocation (reloc.get (), remap[targ]);
	}
    }

  return result;
}

bool
bytecode_block::emit_relocation (relocation_kind what,
				 bytecode_block *dest)
{
  assert (what >= 0);
  if (what != switch_align && what != switch_table)
    emit (java_opcode (what));
  relocations.push_back (new relocation (what, bytecode.size (), dest));
  return ! (what == reloc_goto || what == goto_w);
}

void
bytecode_block::mark (std::set<bytecode_block *> &work_list)
{
  live = true;

  if (fall_through && next_block && ! next_block->live_p ())
    work_list.insert (next_block);

  for (std::list<ref_relocation>::const_iterator i = relocations.begin ();
       i != relocations.end ();
       ++i)
    {
      bytecode_block *targ = (*i)->get_target ();
      if (targ && ! targ->live_p ())
	work_list.insert (targ);
    }
}
