/* Process source files and output type information.
   Copyright (C) 2002 Free Software Foundation, Inc.

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

#include "hconfig.h"
#include "system.h"
#include <ctype.h>
#include "gengtype.h"

static int hit_error = 0;
void
error_at_line VPARAMS ((struct fileloc *pos, const char *msg, ...))
{
  VA_OPEN (ap, msg);
  VA_FIXEDARG (ap, struct fileloc *, pos);
  VA_FIXEDARG (ap, const char *, msg);

  fprintf (stderr, "%s:%d: ", pos->file, pos->line);
  vfprintf (stderr, msg, ap);
  fputc ('\n', stderr);
  hit_error = 1;

  VA_CLOSE (ap);
}

struct type string_type = {
  TYPE_STRING, NULL, NULL
  UNION_INIT_ZERO
}; 

static pair_p typedefs;
static type_p structures;
static type_p varrays;
static pair_p variables;

void
do_typedef (s, t, pos)
     const char *s;
     type_p t;
     struct fileloc *pos;
{
  pair_p p;

  for (p = typedefs; p != NULL; p = p->next)
    if (strcmp (p->name, s) == 0)
      {
	if (p->type != t)
	  {
	    error_at_line (pos, "type `%s' previously defined", s);
	    error_at_line (&p->line, "previously defined here");
	  }
	return;
      }

  p = xmalloc (sizeof (struct pair));
  p->next = typedefs;
  p->name = s;
  p->type = t;
  p->line = *pos;
  typedefs = p;
}

extern type_p
resolve_typedef (s, pos)
     const char *s;
     struct fileloc *pos;
{
  pair_p p;
  for (p = typedefs; p != NULL; p = p->next)
    if (strcmp (p->name, s) == 0)
      return p->type;
  error_at_line (pos, "unidentified type `%s'", s);
  return create_scalar_type ("char", 4);
}

type_p
find_structure (name, isunion)
     const char *name;
     int isunion;
{
  type_p s;
  for (s = structures; s != NULL; s = s->next)
    if (strcmp (name, s->u.s.tag) == 0 && (s->kind == TYPE_UNION) == isunion)
      return s;
  s = xcalloc (1, sizeof (struct type));
  s->kind = isunion ? TYPE_UNION : TYPE_STRUCT;
  s->next = structures;
  s->u.s.tag = name;
  s->u.s.line.file = NULL;
  s->u.s.fields = NULL;
  s->u.s.opt = NULL;
  structures = s;
  return s;
}

type_p
create_scalar_type (name, name_len)
     const char *name;
     size_t name_len;
{
  type_p r = xcalloc (1, sizeof (struct type));
  r->kind = TYPE_SCALAR;
  r->u.sc = xmemdup (name, name_len, name_len + 1);
  return r;
}

type_p
create_pointer (t)
     type_p t;
{
  if (! t->pointer_to)
    {
      type_p r = xcalloc (1, sizeof (struct type));
      r->kind = TYPE_POINTER;
      r->u.p = t;
      t->pointer_to = r;
    }
  return t->pointer_to;
}

type_p
create_varray (t)
     type_p t;
{
  type_p v;
  
  for (v = varrays; v; v = v->next)
    if (v->u.p == t)
      return v;
  v = xcalloc (1, sizeof (*v));
  v->kind = TYPE_VARRAY;
  v->next = varrays;
  v->u.p = t;
  varrays = v;
  return v;
}

type_p
create_array (t, len)
     type_p t;
     const char *len;
{
  type_p v;
  
  v = xcalloc (1, sizeof (*v));
  v->kind = TYPE_ARRAY;
  v->u.a.p = t;
  v->u.a.len = len;
  return v;
}

type_p
adjust_field_type (t, opt)
     type_p t;
     options_p opt;
{
  if (t->kind == TYPE_POINTER
      && t->u.p->kind == TYPE_SCALAR
      && (strcmp (t->u.p->u.sc, "char") == 0
	  || strcmp (t->u.p->u.sc, "unsigned char") == 0))
    {
      while (opt != NULL)
	if (strcmp (opt->name, "length") == 0)
	  return t;
	else
	  opt = opt->next;
      return &string_type;
    }
  return t;
}

void
note_variable (s, t, o, pos)
     const char *s;
     type_p t;
     options_p o;
     struct fileloc *pos;
{
  pair_p n;
  n = xmalloc (sizeof (*n));
  n->name = s;
  n->type = t;
  n->line = *pos;
  n->opt = o;
  n->next = variables;
  variables = n;
}

/* File mapping routines.  For each input file, there is one output .c file
   (but some output files have many input files), and there is one .h file
   for the whole build.  */

typedef struct filemap *filemap_p;

struct filemap {
  filemap_p next;
  const char *input_name;
  const char *output_name;
  FILE *output;
};

static filemap_p files;
FILE * header_file;
static FILE * create_file PARAMS ((const char *));

static FILE *
create_file (name)
     const char *name;
{
  static const char *const hdr[] = {
    "   Copyright (C) 2002 Free Software Foundation, Inc.\n",
    "\n",
    "This file is part of GCC.\n",
    "\n",
    "GCC is free software; you can redistribute it and/or modify it under\n",
    "the terms of the GNU General Public License as published by the Free\n",
    "Software Foundation; either version 2, or (at your option) any later\n",
    "version.\n",
    "\n",
    "GCC is distributed in the hope that it will be useful, but WITHOUT ANY\n",
    "WARRANTY; without even the implied warranty of MERCHANTABILITY or\n",
    "FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License\n",
    "for more details.\n",
    "\n",
    "You should have received a copy of the GNU General Public License\n",
    "along with GCC; see the file COPYING.  If not, write to the Free\n",
    "Software Foundation, 59 Temple Place - Suite 330, Boston, MA\n",
    "02111-1307, USA.  */\n",
    "\n",
    "/* This file is machine generated.  Do not edit.  */\n",
    "\n"
  };
  FILE *f;
  size_t i;
  
  f = tmpfile();
  if (f == NULL)
    {
      perror ("couldn't create temporary file");
      exit (1);
    }
  fprintf (f, "/* Type information for %s.\n", name);
  for (i = 0; i < sizeof(hdr)/sizeof(hdr[0]); i++)
    fputs (hdr[i], f);
  return f;
}

static void
open_base_files (void)
{
  header_file = create_file ("GCC");
}

FILE *
get_output_file (input_file)
     const char *input_file;
{
  filemap_p fm, fmo;
  size_t len;
  const char *basename, *langname;

  /* Do we already know the file?  */
  for (fm = files; fm; fm = fm->next)
    if (input_file == fm->input_name)
      return fm->output;

  /* No, we'll be creating a new filemap.  */
  fm = xmalloc (sizeof (*fm));
  fm->next = files;
  files = fm;
  fm->input_name = input_file;
  
  /* Determine the output file name.  */
  len = strlen (input_file);
  basename = strrchr (input_file, '/');
  if (basename == NULL)
    basename = input_file;
  else
    basename++;
  langname = basename;
  if (basename - input_file >= 2 && memcmp (basename-2, "f/", 2) == 0)
    basename -= 2;
  else if (basename - input_file >= 3 && memcmp (basename-3, "cp/", 3) == 0)
    basename -= 3;
  else if (basename - input_file >= 4 && memcmp (basename-4, "ada/", 4) == 0)
    basename -= 4;
  else if (basename - input_file >= 5 && memcmp (basename-5, "java/", 5) == 0)
    basename -= 5;

  if (len > 2 && input_file[len-1] == 'c' && input_file[len-2] == '.')
    {
      int l;
      char *s;
      
      for (l = len-3; l >= 0; l--)
	if (! isalnum (input_file[l]) && input_file[l] != '-')
	  break;
      fm->output_name = s = xmalloc (sizeof ("gt-") + len - l 
				     + (langname - basename));
      sprintf (s, "%.*sgt-%.*s.h", 
	       langname - basename, basename,
	       len - l - 3, input_file + l + 1);
      fm->output = create_file (input_file);
      return fm->output;
    }
  else if (strcmp (basename, "c-common.h") == 0)
    fm->output_name = "gtype-c.c";
  else
    fm->output_name = "gtype-desc.c";

  /* Look through to see if we've ever seen this output filename before.  */
  for (fmo = fm->next; fmo; fmo = fmo->next)
    if (strcmp (fmo->output_name, fm->output_name) == 0)
      {
	fm->output = fmo->output;
	break;
      }

  /* If not, create it.  */
  if (fmo == NULL)
    {
      fm->output = create_file ("GCC");
      fputs ("#include \"config.h\"\n", fm->output);
      fputs ("#include \"system.h\"\n", fm->output);
      fputs ("#include \"varray.h\"\n", fm->output);
      fputs ("#include \"tree.h\"\n", fm->output);
      fputs ("#include \"rtl.h\"\n", fm->output);
      fputs ("#include \"function.h\"\n", fm->output);
      fputs ("#include \"insn-config.h\"\n", fm->output);
      fputs ("#include \"expr.h\"\n", fm->output);
      fputs ("#include \"optabs.h\"\n", fm->output);
      fputs ("#include \"libfuncs.h\"\n", fm->output);
      fputs ("#include \"ggc.h\"\n", fm->output);
    }

  return fm->output;
}

const char *
get_output_file_name (input_file)
     const char *input_file;
{
  filemap_p fm;

  for (fm = files; fm; fm = fm->next)
    if (input_file == fm->input_name)
      return fm->output_name;
  (void) get_output_file (input_file);
  return get_output_file_name (input_file);
}

static void
close_output_files PARAMS ((void))
{
  filemap_p fm;
  struct filemap header;
  header.next = files;
  header.output_name = "gtype-desc.h";
  header.output = header_file;
  
  for (fm = &header; fm; fm = fm->next)
    {
      int no_write_p;
      filemap_p ofm;
      FILE *newfile;
      
      /* Handle each output file once.  */
      if (fm->output == NULL)
	continue;
      
      for (ofm = fm->next; ofm; ofm = ofm->next)
	if (fm->output == ofm->output)
	  ofm->output = NULL;
      
      /* Compare the output file with the file to be created, avoiding
	 unnecessarily changing timestamps.  */
      newfile = fopen (fm->output_name, "r");
      if (newfile != NULL)
	{
	  int ch1, ch2;
	  
	  rewind (fm->output);
	  do {
	    ch1 = fgetc (fm->output);
	    ch2 = fgetc (newfile);
	  } while (ch1 != EOF && ch1 == ch2);

	  fclose (newfile);
	  
	  no_write_p = ch1 == ch2;
	}
      else
	no_write_p = 0;
     
      /* Nothing interesting to do.  Close the output file.  */
      if (no_write_p)
	{
	  fclose (fm->output);
	  continue;
	}

      newfile = fopen (fm->output_name, "w");
      if (newfile == NULL)
	{
	  perror ("opening output file");
	  exit (1);
	}
      {
	int ch;
	rewind (fm->output);
	while ((ch = fgetc (fm->output)) != EOF)
	  fputc (ch, newfile);
      }
      fclose (newfile);
      fclose (fm->output);
    }
}

static void write_gc_structure_fields 
  PARAMS ((FILE *, type_p, const char *, const char *, options_p, 
	   int, struct fileloc *));
static void write_gc_types PARAMS ((type_p structures));
static void put_mangled_filename PARAMS ((FILE *, const char *));
static void write_gc_roots PARAMS ((pair_p));

static int counter = 0;

static void
write_gc_structure_fields (of, s, val, prev_val, opts, indent, line)
     FILE *of;
     type_p s;
     const char *val;
     const char *prev_val;
     options_p opts;
     int indent;
     struct fileloc *line;
{
  pair_p f;
  int tagcounter = -1;

  if (s->kind == TYPE_UNION)
    {
      const char *tagexpr = NULL;
      const char *p;
      options_p oo;
      
      tagcounter = ++counter;
      for (oo = opts; oo; oo = oo->next)
	if (strcmp (oo->name, "desc") == 0)
	  tagexpr = (const char *)oo->info;

      fprintf (of, "%*s{\n", indent, "");
      indent += 2;
      fprintf (of, "%*sunsigned int tag%d = (", indent, "", tagcounter);
      for (p = tagexpr; *p; p++)
	if (*p != '%')
	  fputc (*p, of);
	else if (*++p == 'h')
	  fprintf (of, "(%s)", val);
	else if (*p == '0')
	  fputs ("(*x)", of);
	else if (*p == '1')
	  fprintf (of, "(%s)", prev_val);
	else
	  error_at_line (line, "`desc' option contains bad escape %c%c",
			 '%', *p);
      fputs (");\n", of);
    }
  
  for (f = s->u.s.fields; f; f = f->next)
    {
      const char *tagid = NULL;
      const char *length = NULL;
      const char *really = NULL;
      int skip_p = 0;
      int always_p = 0;
      options_p oo;
      
      if (f->type->kind == TYPE_SCALAR)
	continue;
      
      for (oo = f->opt; oo; oo = oo->next)
	if (strcmp (oo->name, "length") == 0)
	  length = (const char *)oo->info;
	else if (strcmp (oo->name, "really") == 0)
	  really = (const char *)oo->info;
	else if (strcmp (oo->name, "tag") == 0)
	  tagid = (const char *)oo->info;
	else if (strcmp (oo->name, "skip") == 0)
	  skip_p = 1;
	else if (strcmp (oo->name, "always") == 0)
	  always_p = 1;
	else if (strcmp (oo->name, "desc") == 0 && f->type->kind == TYPE_UNION)
	  ;
	else if (strcmp (oo->name, "descbits") == 0 
		 && f->type->kind == TYPE_UNION)
	  ;
	else
	  error_at_line (&f->line, "unknown option `%s'\n", oo->name);

      if (skip_p)
	continue;

      if (really && (length
		     || f->type->kind != TYPE_POINTER
		     || f->type->u.p->kind != TYPE_STRUCT))
	  error_at_line (&f->line, "field `%s' has invalid option `really'\n",
			 f->name);
      
      if (s->kind == TYPE_UNION && ! always_p )
	{
	  if (! tagid)
	    {
	      error_at_line (&f->line, "field `%s' has no tag", f->name);
	      continue;
	    }
	  fprintf (of, "%*sif (tag%d == (%s)) {\n", indent, "", 
		   tagcounter, tagid);
	  indent += 2;
	}
      
      switch (f->type->kind)
	{
	case TYPE_STRING:
	  /* Do nothing; strings go in the string pool.  */
	  break;

	case TYPE_STRUCT:
	case TYPE_UNION:
	  {
	    char *newval;

	    newval = xmalloc (strlen (val) + sizeof (".") + strlen (f->name));
	    sprintf (newval, "%s.%s", val, f->name);
	    write_gc_structure_fields (of, f->type, newval, val,
				       f->opt, indent, &f->line);
	    free (newval);
	    break;
	  }

	case TYPE_POINTER:
	  if (! length)
	    {
	      if (really)
		fprintf (of, "%*sgt_ggc_mr_%s (%s.%s);\n", indent, "", 
			 really, val, f->name);
	      else if (f->type->u.p->kind == TYPE_STRUCT
		       || f->type->u.p->kind == TYPE_UNION)
		fprintf (of, "%*sgt_ggc_m_%s (%s.%s);\n", indent, "", 
			 f->type->u.p->u.s.tag, val, f->name);
	      else
		error_at_line (&f->line, "field `%s' is pointer to scalar",
			       f->name);
	      break;
	    }
	  else if (f->type->u.p->kind == TYPE_SCALAR)
	    fprintf (of, "%*sggc_mark (%s.%s);\n", indent, "", 
		     val, f->name);
	  else
	    {
	      const char *p;
	      int loopcounter = ++counter;
	      
	      fprintf (of, "%*sif (%s.%s != NULL) {\n", indent, "",
		       val, f->name);
	      indent += 2;
	      fprintf (of, "%*ssize_t i%d;\n", indent, "", loopcounter);
	      fprintf (of, "%*sggc_set_mark (%s.%s);\n", indent, "", 
		       val, f->name);
	      fprintf (of, "%*sfor (i%d = 0; i%d < (", indent, "", 
		       loopcounter, loopcounter);
	      for (p = length; *p; p++)
		if (*p != '%')
		  fputc (*p, of);
		else
		  fprintf (of, "(%s)", val);
	      fprintf (of, "); i%d++) {\n", loopcounter);
	      indent += 2;
	      switch (f->type->u.p->kind)
		{
		case TYPE_STRUCT:
		case TYPE_UNION:
		  {
		    char *newval;
		    
		    newval = xmalloc (strlen (val) + 8 + strlen (f->name));
		    sprintf (newval, "%s.%s[i%d]", val, f->name, loopcounter);
		    write_gc_structure_fields (of, f->type->u.p, newval, val,
					       f->opt, indent, &f->line);
		    free (newval);
		    break;
		  }
		case TYPE_POINTER:
		  if (f->type->u.p->u.p->kind == TYPE_STRUCT
		      || f->type->u.p->u.p->kind == TYPE_UNION)
		    fprintf (of, "%*sgt_ggc_m_%s (%s.%s[i%d]);\n", indent, "", 
			     f->type->u.p->u.p->u.s.tag, val, f->name,
			     loopcounter);
		  else
		    error_at_line (&f->line, 
				   "field `%s' is array of pointer to scalar",
				   f->name);
		  break;
		default:
		  error_at_line (&f->line, 
				 "field `%s' is array of unimplemented type",
				 f->name);
		  break;
		}
	      indent -= 2;
	      fprintf (of, "%*s}\n", indent, "");
	      indent -= 2;
	      fprintf (of, "%*s}\n", indent, "");
	    }
	  break;

	case TYPE_VARRAY:
	  if (f->type->u.p->kind == TYPE_SCALAR)
	    ;
	  else if (f->type->u.p->kind == TYPE_POINTER
		   && (f->type->u.p->u.p->kind == TYPE_STRUCT
		       || f->type->u.p->u.p->kind == TYPE_UNION))
	    {
	      const char *name = f->type->u.p->u.p->u.s.tag;
	      if (strcmp (name, "rtx_def") == 0)
		fprintf (of, "%*sggc_mark_rtx_varray (%s.%s);\n",
			 indent, "", val, f->name);
	      else if (strcmp (name, "tree_node") == 0)
		fprintf (of, "%*sggc_mark_tree_varray (%s.%s);\n",
			 indent, "", val, f->name);
	      else
		error_at_line (&f->line, 
			       "field `%s' is unimplemented varray type",
			       f->name);
	    }
	  else
	    error_at_line (&f->line, 
			   "field `%s' is complicated varray type",
			   f->name);
	  break;

	case TYPE_ARRAY:
	  {
	    int loopcounter = ++counter;
	    type_p t;
	    int i;

	    if (strcmp (f->type->u.a.len, "0") == 0
		|| strcmp (f->type->u.a.len, "1") == 0)
	      error_at_line (&f->line, 
			     "field `%s' is array of size %s",
			     f->name, f->type->u.a.len);
	    
	    fprintf (of, "%*s{\n", indent, "");
	    indent += 2;
	    for (t = f->type, i=0; t->kind == TYPE_ARRAY; t = t->u.a.p, i++)
	      fprintf (of, "%*ssize_t i%d_%d;\n", indent, "", loopcounter, i);
	    for (t = f->type, i=0; t->kind == TYPE_ARRAY; t = t->u.a.p, i++)
	      {
		fprintf (of, 
			 "%*sfor (i%d_%d = 0; i%d_%d < (%s); i%d_%d++) {\n",
			 indent, "", loopcounter, i, loopcounter, i,
			 t->u.a.len, loopcounter, i);
		indent += 2;
	      }

	      if (t->kind == TYPE_POINTER
		  && (t->u.p->kind == TYPE_STRUCT
		      || t->u.p->kind == TYPE_UNION))
		{
		  fprintf (of, "%*sgt_ggc_m_%s (%s.%s", 
			   indent, "", t->u.p->u.s.tag, val, f->name);
		  for (t = f->type, i=0; 
		       t->kind == TYPE_ARRAY; 
		       t = t->u.a.p, i++)
		    fprintf (of, "[i%d_%d]", loopcounter, i);
		  fputs (");\n", of);
		}
	      else if (t->kind == TYPE_STRUCT || t->kind == TYPE_UNION)
		{
		  char *newval;
		  int len;
		  
		  len = strlen (val) + strlen (f->name) + 2;
		  for (t = f->type; t->kind == TYPE_ARRAY; t = t->u.a.p)
		    len += sizeof ("[i_]") + 2*6;
		  
		  newval = xmalloc (len);
		  sprintf (newval, "%s.%s", val, f->name);
		  for (t = f->type, i=0; 
		       t->kind == TYPE_ARRAY; 
		       t = t->u.a.p, i++)
		    sprintf (newval + strlen (newval), "[i%d_%d]", 
			     loopcounter, i);
		  write_gc_structure_fields (of, f->type->u.p, newval, val,
					     f->opt, indent, &f->line);
		  free (newval);
		}
	      else
		error_at_line (&f->line, 
			       "field `%s' is array of unimplemented type",
			       f->name);
	    for (t = f->type, i=0; t->kind == TYPE_ARRAY; t = t->u.a.p, i++)
	      {
		indent -= 2;
		fprintf (of, "%*s}\n", indent, "");
	      }
	    indent -= 2;
	    fprintf (of, "%*s}\n", indent, "");
	    break;
	  }

	default:
	  error_at_line (&f->line, 
			 "field `%s' is unimplemented type",
			 f->name);
	  break;
	}
      
      if (s->kind == TYPE_UNION && ! always_p )
	{
	  indent -= 2;
	  fprintf (of, "%*s}\n", indent, "");
	}
    }
  if (s->kind == TYPE_UNION)
    {
      indent -= 2;
      fprintf (of, "%*s}\n", indent, "");
    }
}

static void
write_gc_types PARAMS ((type_p structures))
{
  type_p s;
  
  fputs ("/* GC marker procedures.  */\n", header_file);
  for (s = structures; s; s = s->next)
    if (s->u.s.line.file
	&& (s->kind == TYPE_STRUCT || s->u.s.opt))
      {
	FILE *f;
	
	/* Declare the marker procedure.  */
	fprintf (header_file, 
		 "extern void gt_ggc_m_%s PARAMS ((void *));\n",
		 s->u.s.tag);

	/* Output it.  */
	f = get_output_file (s->u.s.line.file);
	
	fputc ('\n', f);
	fputs ("void\n", f);
	fprintf (f, "gt_ggc_m_%s (x_p)\n", s->u.s.tag);
	fputs ("      void *x_p;\n", f);
	fputs ("{\n", f);
	fprintf (f, "  %s %s * const x = (%s %s *)x_p;\n",
		 s->kind == TYPE_UNION ? "union" : "struct", s->u.s.tag,
		 s->kind == TYPE_UNION ? "union" : "struct", s->u.s.tag);
	fputs ("  if (! ggc_test_and_set_mark (x))\n", f);
	fputs ("    return;\n", f);
	
	write_gc_structure_fields (f, s, "(*x)", "not valid postage",
				   s->u.s.opt, 2, &s->u.s.line);
	
	fputs ("}\n", f);
      }
}

static void
put_mangled_filename (f, fn)
     FILE *f;
     const char *fn;
{
  const char *name = get_output_file_name (fn);
  for (; *name != 0; name++)
    if (isalnum (*name))
      fputc (*name, f);
    else
      fputc ('_', f);
}

static void
write_gc_roots (variables)
     pair_p variables;
{
  pair_p v;
  struct flist {
    struct flist *next;
    int started_p;
    const char *name;
    FILE *f;
  } *flp = NULL;
  struct flist *fli2;
  FILE *topf;

  for (v = variables; v; v = v->next)
    {
      FILE *f = get_output_file (v->line.file);
      struct flist *fli;
      const char *length = NULL;
      int deletable_p = 0;
      options_p o;

      for (o = v->opt; o; o = o->next)
	if (strcmp (o->name, "length") == 0)
	  length = (const char *)o->info;
	else if (strcmp (o->name, "deletable") == 0)
	  deletable_p = 1;
	else
	  error_at_line (&v->line, 
			 "global `%s' has unknown option `%s'",
			 v->name, o->name);

      for (fli = flp; fli; fli = fli->next)
	if (fli->f == f)
	  break;
      if (fli == NULL)
	{
	  fli = xmalloc (sizeof (*fli));
	  fli->f = f;
	  fli->next = flp;
	  fli->started_p = 0;
	  fli->name = v->line.file;
	  flp = fli;

	  fputs ("\n/* GC roots.  */\n\n", f);
	}

      if (! deletable_p
	  && length
	  && v->type->kind == TYPE_POINTER
	  && v->type->u.p->kind == TYPE_POINTER)
	{
	  type_p s = v->type->u.p->u.p;
	  
	  fprintf (f, "static void gt_ggc_ma_%s PARAMS((void *));\n",
		   v->name);
	  fprintf (f, "static void\ngt_ggc_ma_%s (x_p);\n      void *x_p;\n",
		   v->name);
	  fputs ("{\n", f);
	  if (s->kind != TYPE_STRUCT && s->kind != TYPE_UNION)
	    {
	      error_at_line (&v->line, 
			     "global `%s' has unsupported ** type",
			     v->name);
	      continue;
	    }

	  fprintf (f, "  %s %s * const x = (%s %s *)x_p;\n",
		   s->kind == TYPE_UNION ? "union" : "struct", s->u.s.tag,
		   s->kind == TYPE_UNION ? "union" : "struct", s->u.s.tag);
	  fputs ("  size_t i;\n", f);
	  fprintf (f, "  for (i = 0; i < (%s); i++)\n", length);
	  fprintf (f, "    gt_ggc_m_%s (x[i])", s->u.s.tag);
	  fputs ("}\n\n", f);
	}
    }

  for (v = variables; v; v = v->next)
    {
      FILE *f = get_output_file (v->line.file);
      struct flist *fli;
      const char *length = NULL;
      int deletable_p = 0;
      options_p o;
      type_p tp;
      type_p ap;
      
      for (o = v->opt; o; o = o->next)
	if (strcmp (o->name, "length") == 0)
	  length = (const char *)o->info;
	else if (strcmp (o->name, "deletable") == 0)
	  deletable_p = 1;
	else
	  error_at_line (&v->line, 
			 "global `%s' has unknown option `%s'",
			 v->name, o->name);

      if (deletable_p)
	continue;

      for (fli = flp; fli; fli = fli->next)
	if (fli->f == f)
	  break;
      if (! fli->started_p)
	{
	  fli->started_p = 1;

	  fputs ("const struct ggc_root_tab gt_ggc_r_", f);
	  put_mangled_filename (f, v->line.file);
	  fputs ("[] = {\n", f);
	}


      fputs ("  {\n", f);
      fprintf (f, "    &%s,\n", v->name);
      fputs ("    1", f);

      for (ap = v->type; ap->kind == TYPE_ARRAY; ap = ap->u.a.p)
	fprintf (f, " * (%s)", ap->u.a.len);
      fputs (",\n", f);

      if (ap->kind != TYPE_POINTER)
	error_at_line (&v->line, 
		       "global `%s' is unimplemented type",
		       v->name);

      tp = ap->u.p;
      
      
      if (! length
	  && (tp->kind == TYPE_UNION || tp->kind == TYPE_STRUCT))
	{
	  fprintf (f, "    sizeof (%s %s *),\n    &gt_ggc_m_%s",
		   tp->kind == TYPE_UNION ? "union" : "struct", 
		   tp->u.s.tag, tp->u.s.tag);
	}
      else if (tp->kind == TYPE_POINTER
	       && length
	       && (tp->u.p->kind == TYPE_UNION
		   || tp->u.p->kind == TYPE_STRUCT))
	{
	  fprintf (f, "    sizeof (%s %s **),\n    &gt_ggc_mp_%s",
		   tp->kind == TYPE_UNION ? "union" : "struct", 
		   tp->u.s.tag, v->name);
	}
      else
	{
	  error_at_line (&v->line, 
			 "global `%s' is pointer to unimplemented type",
			 v->name);
	}
      fputs ("\n  },\n", f);
    }

  for (fli2 = flp; fli2; fli2 = fli2->next)
    if (fli2->started_p)
      {
	fputs ("  LAST_GGC_ROOT_TAB\n", fli2->f);
	fputs ("};\n\n", fli2->f);
      }

  topf = get_output_file ("ggc.h");
  for (fli2 = flp; fli2; fli2 = fli2->next)
    if (fli2->started_p)
      {
	fputs ("extern const struct ggc_root_tab gt_ggc_r_", topf);
	put_mangled_filename (topf, fli2->name);
	fputs ("[];\n", topf);
      }

  fputs ("const struct ggc_root_tab * const gt_ggc_rtab[] = {\n", topf);
  for (fli2 = flp; fli2; fli2 = fli2->next)
    if (fli2->started_p)
      {
	fli2->started_p = 0;
	
	fputs ("  gt_ggc_r_", topf);
	put_mangled_filename (topf, fli2->name);
	fputs (",\n", topf);
      }
  fputs ("  NULL\n", topf);
  fputs ("};\n\n", topf);

  for (v = variables; v; v = v->next)
    {
      FILE *f = get_output_file (v->line.file);
      struct flist *fli;
      const char *length = NULL;
      int deletable_p = 0;
      options_p o;

      for (o = v->opt; o; o = o->next)
	if (strcmp (o->name, "length") == 0)
	  length = (const char *)o->info;
	else if (strcmp (o->name, "deletable") == 0)
	  deletable_p = 1;
	else
	  error_at_line (&v->line, 
			 "global `%s' has unknown option `%s'",
			 v->name, o->name);

      if (! deletable_p)
	continue;

      for (fli = flp; fli; fli = fli->next)
	if (fli->f == f)
	  break;
      if (! fli->started_p)
	{
	  fli->started_p = 1;

	  fputs ("const struct ggc_root_tab gt_ggc_rd_", f);
	  put_mangled_filename (f, v->line.file);
	  fputs ("[] = {\n", f);
	}
      
      fprintf (f, "  { &%s, 1, sizeof (%s), NULL },\n",
	       v->name, v->name);
    }
  
  for (fli2 = flp; fli2; fli2 = fli2->next)
    if (fli2->started_p)
      {
	fputs ("  LAST_GGC_ROOT_TAB\n", fli2->f);
	fputs ("};\n\n", fli2->f);
      }

  for (fli2 = flp; fli2; fli2 = fli2->next)
    if (fli2->started_p)
      {
	fputs ("extern const struct ggc_root_tab gt_ggc_rd_", topf);
	put_mangled_filename (topf, fli2->name);
	fputs ("[];\n", topf);
      }

  fputs ("const struct ggc_root_tab * const gt_ggc_deletable_rtab[] = {\n", 
	 topf);
  for (fli2 = flp; fli2; fli2 = fli2->next)
    if (fli2->started_p)
      {
	fli2->started_p = 0;
	
	fputs ("  gt_ggc_rd_", topf);
	put_mangled_filename (topf, fli2->name);
	fputs (",\n", topf);
      }
  fputs ("  NULL\n", topf);
  fputs ("};\n\n", topf);
}


extern int main PARAMS ((int argc, char **argv));
int 
main(argc, argv)
     int argc;
     char **argv;
{
  int i;
  static struct fileloc pos = { __FILE__, __LINE__ };

  do_typedef ("CUMULATIVE_ARGS",
	      create_scalar_type ("CUMULATIVE_ARGS", 
				  strlen ("CUMULATIVE_ARGS")),
	      &pos);
  do_typedef ("REAL_VALUE_TYPE",
	      create_scalar_type ("REAL_VALUE_TYPE", 
				  strlen ("REAL_VALUE_TYPE")),
	      &pos);

  for (i = 1; i < argc; i++)
    parse_file (argv[i]);

  if (hit_error != 0)
    exit (1);

  open_base_files ();
  write_gc_types (structures);
  write_gc_roots (variables);
  close_output_files ();

  return (hit_error != 0);
}
