/* CPP Library.
   Copyright (C) 1986, 1987, 1989, 1992, 1993, 1994, 1995, 1996, 1997, 1998,
   1999, 2000 Free Software Foundation, Inc.
   Contributed by Per Bothner, 1994-95.
   Based on CCCP program by Paul Rubin, June 1986
   Adapted to ANSI C, Richard Stallman, Jan 1987

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 2, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

#include "config.h"
#include "system.h"

#include "cpplib.h"
#include "cpphash.h"
#include "intl.h"
#include "symcat.h"

/* `struct directive' defines one #-directive, including how to handle it.  */

struct directive
{
  directive_handler func;	/* Function to handle directive.  */
  const U_CHAR *name;		/* Name of directive.  */
  unsigned short length;	/* Length of name.  */
  unsigned short flags;	        /* Flags describing this directive.  */
};

/* Stack of conditionals currently in progress
   (including both successful and failing conditionals).  */

struct if_stack
{
  struct if_stack *next;
  int lineno;			/* line number where condition started */
  int if_succeeded;		/* truth of last condition in this group */
  const U_CHAR *control_macro;	/* macro name for #ifndef around entire file */
  int type;			/* type of last directive seen in this group */
};
typedef struct if_stack IF_STACK;

/* Forward declarations.  */

static void validate_else		PARAMS ((cpp_reader *, const U_CHAR *));
static int parse_ifdef			PARAMS ((cpp_reader *, const U_CHAR *));
static unsigned int parse_include	PARAMS ((cpp_reader *, const U_CHAR *));
static int conditional_skip		PARAMS ((cpp_reader *, int, int,
						 U_CHAR *));
static int skip_if_group		PARAMS ((cpp_reader *));
static void pass_thru_directive		PARAMS ((const U_CHAR *, size_t,
						 cpp_reader *, int));
static int read_line_number		PARAMS ((cpp_reader *, int *));
static U_CHAR *detect_if_not_defined	PARAMS ((cpp_reader *));
static int consider_directive_while_skipping
					PARAMS ((cpp_reader *, IF_STACK *));

/* Values for the flags field of the table below.  KANDR and COND
   directives come from traditional (K&R) C.  The difference is, if we
   care about it while skipping a failed conditional block, its origin
   is COND.  STDC89 directives come from the 1989 C standard.
   EXTENSION directives are extensions, with origins noted below.  */

#define KANDR       0
#define COND        1
#define STDC89      2
#define EXTENSION   3

#define ORIGIN_MASK 3
#define ORIGIN(f) ((f) & ORIGIN_MASK)
#define TRAD_DIRECT_P(f) (ORIGIN (f) == KANDR || ORIGIN (f) == COND)

/* This is the table of directive handlers.  It is ordered by
   frequency of occurrence; the numbers at the end are directive
   counts from all the source code I have lying around (egcs and libc
   CVS as of 1999-05-18, plus grub-0.5.91, linux-2.2.9, and
   pcmcia-cs-3.0.9).

   The entries with a dash and a name after the count are extensions,
   of which all but #warning and #include_next are deprecated.  The name
   is where the extension appears to have come from.  */

/* #sccs is not always recognized.  */
#ifdef SCCS_DIRECTIVE
# define SCCS_ENTRY D(sccs, T_SCCS, EXTENSION)		/*     0 - SVR2? */
#else
# define SCCS_ENTRY /* nothing */
#endif

#define DIRECTIVE_TABLE							 \
D(define,	T_DEFINE = 0,	KANDR)		           /* 270554 */ \
D(include,	T_INCLUDE,	KANDR | SYNTAX_INCLUDE)    /*  52262 */ \
D(endif,	T_ENDIF,	COND)		           /*  45855 */ \
D(ifdef,	T_IFDEF,	COND)			   /*  22000 */ \
D(if,		T_IF,		COND)			   /*  18162 */ \
D(else,		T_ELSE,		COND)			    /*  9863 */ \
D(ifndef,	T_IFNDEF,	COND)			    /*  9675 */ \
D(undef,	T_UNDEF,	KANDR)			    /*  4837 */ \
D(line,		T_LINE,		KANDR)			    /*  2465 */ \
D(elif,		T_ELIF,		COND)			    /*   610 */ \
D(error,	T_ERROR,	STDC89)			    /*   475 */ \
D(pragma,	T_PRAGMA,	STDC89)			    /*   195 */ \
D(warning,	T_WARNING,	EXTENSION)		    /*    22 GNU */ \
D(include_next,	T_INCLUDE_NEXT,	EXTENSION | SYNTAX_INCLUDE) /*    19 GNU */ \
D(ident,	T_IDENT,	EXTENSION)		    /*    11 SVR4 */ \
D(import,	T_IMPORT,	EXTENSION | SYNTAX_INCLUDE) /*     0 ObjC */ \
D(assert,	T_ASSERT,	EXTENSION)  		    /*     0 SVR4 */ \
D(unassert,	T_UNASSERT,	EXTENSION)  		    /*     0 SVR4 */ \
SCCS_ENTRY

/* Use the table to generate a series of prototypes, an enum for the
   directive names, and an array of directive handlers.  */

/* The directive-processing functions are declared to return int
   instead of void, because some old compilers have trouble with
   pointers to functions returning void.  */

/* Don't invoke CONCAT2 with any whitespace or K&R cc will fail. */
#define D(name, t, f) static int CONCAT2(do_,name) PARAMS ((cpp_reader *));
DIRECTIVE_TABLE
#undef D

#define D(n, tag, f) tag,
enum
{
  DIRECTIVE_TABLE
  N_DIRECTIVES
};
#undef D

/* Don't invoke CONCAT2 with any whitespace or K&R cc will fail. */
#define D(name, t, flags) \
{ CONCAT2(do_,name), (const U_CHAR *) STRINGX(name), \
  sizeof STRINGX(name) - 1, flags },
static const struct directive dtable[] =
{
DIRECTIVE_TABLE
};
#undef D
#undef DIRECTIVE_TABLE

/* Check if a token's name matches that of a known directive.  Put in
   this file to save exporting dtable and other unneeded information.  */
void
_cpp_check_directive (list, token)
     cpp_toklist *list;
     cpp_token *token;
{
  const U_CHAR *name = token->val.name.text;
  size_t len = token->val.name.len;
  unsigned int i;

  list->dirno = -1;
  list->flags &= ~SYNTAX_INCLUDE;

  for (i = 0; i < N_DIRECTIVES; i++)
    if (dtable[i].length == len && !ustrncmp (dtable[i].name, name, len)) 
      {
	list->dirno = i;
	if (dtable[i].flags & SYNTAX_INCLUDE)
	  list->flags |= SYNTAX_INCLUDE;
	break;
      }
}

/* Handle a possible # directive.
   '#' has already been read.  */

int
_cpp_handle_directive (pfile)
     cpp_reader *pfile;
{
  int i;
  int hash_at_bol;
  unsigned int len;
  U_CHAR *ident;
  long old_written = CPP_WRITTEN (pfile);
  enum cpp_ttype tok;

  if (CPP_IS_MACRO_BUFFER (CPP_BUFFER (pfile)))
    {
      cpp_ice (pfile, "handle_directive called on macro buffer");
      return 0;
    }

  /* -traditional directives are recognized only with the # in column 1.  */
  hash_at_bol = CPP_IN_COLUMN_1 (pfile);

  /* Scan the next token, then pretend we didn't.  */
  CPP_SET_MARK (pfile);
  pfile->no_macro_expand++;
  tok = _cpp_get_directive_token (pfile);
  pfile->no_macro_expand--;

  ident = pfile->token_buffer + old_written;
  len = CPP_PWRITTEN (pfile) - ident;
  CPP_SET_WRITTEN (pfile, old_written);
  CPP_GOTO_MARK (pfile);

  /* # followed by a number is equivalent to #line.  Do not recognize
     this form in assembly language source files.  Complain about this
     form if we're being pedantic, but not if this is regurgitated
     input (preprocessed or fed back in by the C++ frontend).  */
  if (tok == CPP_NUMBER)
    {
      if (CPP_OPTION (pfile, lang_asm))
	return 0;

      if (CPP_PEDANTIC (pfile)
	  && CPP_BUFFER (pfile)->ihash
	  && ! CPP_OPTION (pfile, preprocessed))
	cpp_pedwarn (pfile, "# followed by integer");
      i = T_LINE;
      goto process_directive;
    }

  /* If we are rescanning preprocessed input, don't obey any directives
     other than # nnn.  */
  else if (CPP_OPTION (pfile, preprocessed))
    return 0;

  /* A line of just # becomes blank.  */
  else if (tok == CPP_VSPACE)
    return 1;

  /* A NAME token might in fact be a directive!  */
  else if (tok == CPP_NAME)
    {
      for (i = 0; i < N_DIRECTIVES; i++)
	{
	  if (dtable[i].length == len
	      && !ustrncmp (dtable[i].name, ident, len)) 
	    goto real_directive;
	}
      /* Don't complain about invalid directives in assembly source,
	 we don't know where the comments are, and # may introduce
	 assembler pseudo-ops.  */
      if (!CPP_OPTION (pfile, lang_asm))
	cpp_error (pfile, "invalid preprocessing directive #%s", ident);
      return 0;
    }
  /* And anything else means the # wasn't a directive marker.   */
  else
    return 0;

 real_directive:

  /* In -traditional mode, a directive is ignored unless its # is in
     column 1.  */
  if (CPP_TRADITIONAL (pfile) && !hash_at_bol)
    {
      if (CPP_WTRADITIONAL (pfile))
	cpp_warning (pfile, "ignoring #%s because of its indented #",
		     dtable[i].name);
      return 0;
    }

  /* no_directives is set when we are parsing macro arguments.  Directives
     in macro arguments are undefined behavior (C99 6.10.3.11); this
     implementation chooses to make them hard errors.  */
  if (pfile->no_directives)
    {
      cpp_error (pfile, "#%s may not be used inside a macro argument",
		 dtable[i].name);
      _cpp_skip_rest_of_line (pfile);
      return 1;
    }

  /* Issue -pedantic warnings for extended directives.   */
  if (CPP_PEDANTIC (pfile) && ORIGIN (dtable[i].flags) == EXTENSION)
    cpp_pedwarn (pfile, "ISO C does not allow #%s", dtable[i].name);

  /* -Wtraditional gives warnings about directives with inappropriate
     indentation of #.  */
  if (CPP_WTRADITIONAL (pfile))
    {
      if (!hash_at_bol && TRAD_DIRECT_P (dtable[i].flags))
	cpp_warning (pfile, "traditional C ignores #%s with the # indented",
		     dtable[i].name);
      else if (hash_at_bol && ! TRAD_DIRECT_P (dtable[i].flags))
	cpp_warning (pfile,
		"suggest hiding #%s from traditional C with an indented #",
		     dtable[i].name);
    }

  /* Unfortunately, it's necessary to scan the directive name again,
     now we know we're going to consume it.  FIXME.  */

  pfile->no_macro_expand++;
  _cpp_get_directive_token (pfile);
  pfile->no_macro_expand--;
  CPP_SET_WRITTEN (pfile, old_written);

 process_directive:
  /* Some directives (e.g. #if) may return a request to execute
     another directive handler immediately.  No directive ever
     requests that #define be executed immediately, so it is safe for
     the loop to terminate when some function returns 0 (== T_DEFINE).  */
  while ((i = dtable[i].func (pfile)));
  return 1;
}

/* Pass a directive through to the output file.
   BUF points to the contents of the directive, as a contiguous string.
   LEN is the length of the string pointed to by BUF.
   KEYWORD is the keyword-table entry for the directive.  */

static void
pass_thru_directive (buf, len, pfile, keyword)
     const U_CHAR *buf;
     size_t len;
     cpp_reader *pfile;
     int keyword;
{
  const struct directive *kt = &dtable[keyword];
  register unsigned klen = kt->length;

  CPP_RESERVE (pfile, 1 + klen + len);
  CPP_PUTC_Q (pfile, '#');
  CPP_PUTS_Q (pfile, kt->name, klen);
  if (len != 0 && buf[0] != ' ')
    CPP_PUTC_Q (pfile, ' ');
  CPP_PUTS_Q (pfile, buf, len);
}

/* Process a #define command.  */

static int
do_define (pfile)
     cpp_reader *pfile;
{
  HASHNODE *node;
  int len;
  const U_CHAR *sym;
  cpp_toklist *list = &pfile->directbuf;

  pfile->no_macro_expand++;
  CPP_OPTION (pfile, discard_comments)++;

  _cpp_scan_until (pfile, list, CPP_VSPACE);

  /* First token on the line must be a NAME.  There may not be any
     tokens in the list (if we had #define all by itself on a line).  */
  if (list->tokens_used == 0
      || TOK_TYPE (list, 0) != CPP_NAME)
    {
      cpp_error_with_line (pfile, list->line, TOK_COL (list, 0),
			   "#define must be followed by an identifier");
      goto out;
    }

  sym = TOK_NAME (list, 0);
  len = TOK_LEN (list, 0);

  /* That NAME is not allowed to be "defined".  (Not clear if the
     standard requires this.)  */
  if (len == 7 && !ustrncmp (sym, U"defined", 7))
    {
      cpp_error_with_line (pfile, list->line, TOK_COL (list, 0),
			   "\"defined\" is not a legal macro name");
      goto out;
    }

  node = _cpp_lookup (pfile, sym, len);
  /* Check for poisoned identifiers now.  All other checks
     are done in cpphash.c.  */
  if (node->type == T_POISON)
    {
      cpp_error (pfile, "redefining poisoned `%.*s'", len, sym);
      goto out;
    }
    
  if (_cpp_create_definition (pfile, list, node) == 0)
    goto out;

  if (CPP_OPTION (pfile, debug_output)
      || CPP_OPTION (pfile, dump_macros) == dump_definitions)
    _cpp_dump_definition (pfile, node);
  else if (CPP_OPTION (pfile, dump_macros) == dump_names)
    pass_thru_directive (sym, len, pfile, T_DEFINE);

 out:
  pfile->no_macro_expand--;
  CPP_OPTION (pfile, discard_comments)--;
  return 0;
}

/* Handle #include and #import.  */

static unsigned int
parse_include (pfile, name)
     cpp_reader *pfile;
     const U_CHAR *name;
{
  long old_written = CPP_WRITTEN (pfile);
  enum cpp_ttype token;
  int len;

  pfile->parsing_include_directive++;
  token = _cpp_get_directive_token (pfile);
  pfile->parsing_include_directive--;

  len = CPP_WRITTEN (pfile) - old_written;

  if (token == CPP_STRING)
    ; /* No special treatment required.  */
#ifdef VMS
  else if (token == CPP_NAME)
    {
      /* Support '#include xyz' like VAX-C.  It is taken as
         '#include <xyz.h>' and generates a warning.  */
      cpp_warning (pfile, "#%s filename is obsolete, use #%s <filename.h>",
		   name, name);

      /* Rewrite the token to <xyz.h>.  */
      CPP_RESERVE (pfile, 4);
      len += 4;
      memmove (pfile->token_buffer + old_written + 1,
	       pfile->token_buffer + old_written,
	       CPP_WRITTEN (pfile) - old_written);
      pfile->token_buffer[old_written] = '<';
      CPP_PUTS_Q (pfile, ".h>", 2);
    }
#endif
  else
    {
      cpp_error (pfile, "`#%s' expects \"FILENAME\" or <FILENAME>", name);
      CPP_SET_WRITTEN (pfile, old_written);
      _cpp_skip_rest_of_line (pfile);
      return 0;
    }

  if (_cpp_get_directive_token (pfile) != CPP_VSPACE)
    {
      cpp_error (pfile, "junk at end of `#%s'", name);
      _cpp_skip_rest_of_line (pfile);
    }

  CPP_SET_WRITTEN (pfile, old_written);

  if (len == 0)
    cpp_error (pfile, "empty file name in `#%s'", name);

  return len;
}

static int
do_include (pfile)
     cpp_reader *pfile;
{
  unsigned int len;
  U_CHAR *token;

  len = parse_include (pfile, dtable[T_INCLUDE].name);
  if (len == 0)
    return 0;
  token = (U_CHAR *) alloca (len + 1);
  memcpy (token, CPP_PWRITTEN (pfile), len);
  token[len] = '\0';
  
  if (CPP_OPTION (pfile, dump_includes))
    pass_thru_directive (token, len, pfile, T_INCLUDE);

  _cpp_execute_include (pfile, token, len, 0, 0);
  return 0;
}

static int
do_import (pfile)
     cpp_reader *pfile;
{
  unsigned int len;
  U_CHAR *token;

  if (CPP_OPTION (pfile, warn_import)
      && !CPP_BUFFER (pfile)->system_header_p && !pfile->import_warning)
    {
      pfile->import_warning = 1;
      cpp_warning (pfile,
	   "#import is obsolete, use an #ifndef wrapper in the header file");
    }

  len = parse_include (pfile, dtable[T_IMPORT].name);
  if (len == 0)
    return 0;
  token = (U_CHAR *) alloca (len + 1);
  memcpy (token, CPP_PWRITTEN (pfile), len);
  token[len] = '\0';
  
  if (CPP_OPTION (pfile, dump_includes))
    pass_thru_directive (token, len, pfile, T_IMPORT);

  _cpp_execute_include (pfile, token, len, 1, 0);
  return 0;
}

static int
do_include_next (pfile)
     cpp_reader *pfile;
{
  unsigned int len;
  U_CHAR *token;
  struct file_name_list *search_start = 0;

  len = parse_include (pfile, dtable[T_INCLUDE_NEXT].name);
  if (len == 0)
    return 0;
  token = (U_CHAR *) alloca (len + 1);
  memcpy (token, CPP_PWRITTEN (pfile), len);
  token[len] = '\0';
  
  if (CPP_OPTION (pfile, dump_includes))
    pass_thru_directive (token, len, pfile, T_INCLUDE_NEXT);

  /* For #include_next, skip in the search path past the dir in which the
     containing file was found.  Treat files specified using an absolute path
     as if there are no more directories to search.  Treat the primary source
     file like any other included source, but generate a warning.  */
  if (CPP_PREV_BUFFER (CPP_BUFFER (pfile)))
    {
      if (CPP_BUFFER (pfile)->ihash->foundhere != ABSOLUTE_PATH)
	search_start = CPP_BUFFER (pfile)->ihash->foundhere->next;
    }
  else
    cpp_warning (pfile, "#include_next in primary source file");

  _cpp_execute_include (pfile, token, len, 0, search_start);
  return 0;
}

/* Subroutine of do_line.  Read next token from PFILE without adding it to
   the output buffer.  If it is a number between 1 and 4, store it in *NUM
   and return 1; otherwise, return 0 and complain if we aren't at the end
   of the directive.  */

static int
read_line_number (pfile, num)
     cpp_reader *pfile;
     int *num;
{
  long save_written = CPP_WRITTEN (pfile);
  U_CHAR *p;
  enum cpp_ttype token = _cpp_get_directive_token (pfile);
  p = pfile->token_buffer + save_written;

  if (token == CPP_NUMBER && p + 1 == CPP_PWRITTEN (pfile)
      && p[0] >= '1' && p[0] <= '4')
    {
      *num = p[0] - '0';
      CPP_SET_WRITTEN (pfile, save_written);
      return 1;
    }
  else
    {
      if (token != CPP_VSPACE && token != CPP_EOF)
	cpp_error (pfile, "invalid format `#line' command");
      CPP_SET_WRITTEN (pfile, save_written);
      return 0;
    }
}

/* Interpret #line command.
   Note that the filename string (if any) is treated as if it were an
   include filename.  That means no escape handling.  */

static int
do_line (pfile)
     cpp_reader *pfile;
{
  cpp_buffer *ip = CPP_BUFFER (pfile);
  unsigned int new_lineno;
  long old_written = CPP_WRITTEN (pfile);
  enum cpp_ttype token;
  char *x;

  token = _cpp_get_directive_token (pfile);

  if (token != CPP_NUMBER)
    {
      cpp_error (pfile, "token after `#line' is not an integer");
      goto bad_line_directive;
    }

  CPP_PUTC (pfile, '\0');  /* not terminated for us */
  new_lineno = strtoul ((const char *) (pfile->token_buffer + old_written),
			&x, 10);
  if (x[0] != '\0')
    {
      cpp_error (pfile, "token after `#line' is not an integer");
      goto bad_line_directive;
    }      
  CPP_SET_WRITTEN (pfile, old_written);

  if (CPP_PEDANTIC (pfile) && (new_lineno <= 0 || new_lineno > 32767))
    cpp_pedwarn (pfile, "line number out of range in `#line' command");

  token = _cpp_get_directive_token (pfile);

  if (token == CPP_STRING)
    {
      U_CHAR *fname = pfile->token_buffer + old_written + 1;
      U_CHAR *end_name = CPP_PWRITTEN (pfile) - 1;
      int action_number = 0;

      if (read_line_number (pfile, &action_number))
	{
	  if (CPP_PEDANTIC (pfile))
	    cpp_pedwarn (pfile, "garbage at end of `#line' command");

	  /* This is somewhat questionable: change the buffer stack
	     depth so that output_line_command thinks we've stacked
	     another buffer. */
	  if (action_number == 1)
	    {
	      pfile->buffer_stack_depth++;
	      ip->system_header_p = 0;
	      read_line_number (pfile, &action_number);
	    }
	  else if (action_number == 2)
	    {
	      pfile->buffer_stack_depth--;
	      ip->system_header_p = 0;
	      read_line_number (pfile, &action_number);
	    }
	  if (action_number == 3)
	    {
	      ip->system_header_p = 1;
	      read_line_number (pfile, &action_number);
	    }
	  if (action_number == 4)
	    {
	      ip->system_header_p = 2;
	      read_line_number (pfile, &action_number);
	    }
	}
      
      *end_name = '\0';
      
      if (strcmp ((const char *)fname, ip->nominal_fname))
	{
	  if (!strcmp ((const char *)fname, ip->ihash->name))
	    ip->nominal_fname = ip->ihash->name;
	  else
	    ip->nominal_fname = _cpp_fake_ihash (pfile, (const char *)fname);
	}
    }
  else if (token != CPP_VSPACE && token != CPP_EOF)
    {
      cpp_error (pfile, "token after `#line %d' is not a string", new_lineno);
      goto bad_line_directive;
    }

  /* The Newline at the end of this line remains to be processed.
     To put the next line at the specified line number,
     we must store a line number now that is one less.  */
  ip->lineno = new_lineno - 1;
  CPP_SET_WRITTEN (pfile, old_written);
  return 0;

 bad_line_directive:
  _cpp_skip_rest_of_line (pfile);
  CPP_SET_WRITTEN (pfile, old_written);
  return 0;
}

/* Remove the definition of a symbol from the symbol table.
   According to the C standard, it is not an error to undef
   something that has no definitions. */
static int
do_undef (pfile)
     cpp_reader *pfile;
{
  int len;
  HASHNODE *hp;
  U_CHAR *name;
  long here = CPP_WRITTEN (pfile);
  enum cpp_ttype token;

  pfile->no_macro_expand++;
  token = _cpp_get_directive_token (pfile);
  pfile->no_macro_expand--;

  if (token != CPP_NAME)
    {
      cpp_error (pfile, "token after #undef is not an identifier");
      _cpp_skip_rest_of_line (pfile);
      return 0;
    }
  len = CPP_WRITTEN (pfile) - here;

  token = _cpp_get_directive_token (pfile);
  if (token != CPP_VSPACE)
  {
      cpp_pedwarn (pfile, "junk on line after #undef");
      _cpp_skip_rest_of_line (pfile);
  }

  name = pfile->token_buffer + here;
  CPP_SET_WRITTEN (pfile, here);

  hp = _cpp_lookup (pfile, name, len);
  if (hp->type == T_VOID)
    ; /* Not defined in the first place - do nothing.  */
  else if (hp->type == T_POISON)
    cpp_error (pfile, "cannot undefine poisoned \"%s\"", hp->name);
  else
    {
      /* If we are generating additional info for debugging (with -g) we
	 need to pass through all effective #undef commands.  */
      if (CPP_OPTION (pfile, debug_output))
	pass_thru_directive (hp->name, len, pfile, T_UNDEF);

      if (hp->type != T_MACRO && hp->type != T_FMACRO
	  && hp->type != T_EMPTY && hp->type != T_IDENTITY)
	cpp_warning (pfile, "undefining `%s'", hp->name);

      _cpp_free_definition (hp);
      hp->type = T_VOID;
    }

  return 0;
}

/*
 * Report an error detected by the program we are processing.
 * Use the text of the line in the error message.
 * (We use error because it prints the filename & line#.)
 */

static int
do_error (pfile)
     cpp_reader *pfile;
{
  const U_CHAR *text, *limit;

  _cpp_skip_hspace (pfile);
  text = CPP_BUFFER (pfile)->cur;
  _cpp_skip_rest_of_line (pfile);
  limit = CPP_BUFFER (pfile)->cur;

  cpp_error (pfile, "#error %.*s", (int)(limit - text), text);
  return 0;
}

/*
 * Report a warning detected by the program we are processing.
 * Use the text of the line in the warning message, then continue.
 */

static int
do_warning (pfile)
     cpp_reader *pfile;
{
  const U_CHAR *text, *limit;

  _cpp_skip_hspace (pfile);
  text = CPP_BUFFER (pfile)->cur;
  _cpp_skip_rest_of_line (pfile);
  limit = CPP_BUFFER (pfile)->cur;

  cpp_warning (pfile, "#warning %.*s", (int)(limit - text), text);
  return 0;
}

/* Report program identification.  */

static int
do_ident (pfile)
     cpp_reader *pfile;
{
  long old_written = CPP_WRITTEN (pfile);

  CPP_PUTS (pfile, "#ident ", 7);

  /* Next token should be a string constant.  */
  if (_cpp_get_directive_token (pfile) == CPP_STRING)
    /* And then a newline.  */
    if (_cpp_get_directive_token (pfile) == CPP_VSPACE)
      /* Good - ship it.  */
      return 0;

  cpp_error (pfile, "invalid #ident");
  _cpp_skip_rest_of_line (pfile);
  CPP_SET_WRITTEN (pfile, old_written);  /* discard directive */

  return 0;
}

/* Pragmata handling.  We handle some of these, and pass the rest on
   to the front end.  C99 defines three pragmas and says that no macro
   expansion is to be performed on them; whether or not macro
   expansion happens for other pragmas is implementation defined.
   This implementation never macro-expands the text after #pragma.

   We currently do not support the _Pragma operator.  Support for that
   has to be coordinated with the front end.  Proposed implementation:
   both #pragma blah blah and _Pragma("blah blah") become
   __builtin_pragma(blah blah) and we teach the parser about that.  */

/* Sub-handlers for the pragmas needing treatment here.
   They return 1 if the token buffer is to be popped, 0 if not. */
static int do_pragma_once		PARAMS ((cpp_reader *));
static int do_pragma_implementation	PARAMS ((cpp_reader *));
static int do_pragma_poison		PARAMS ((cpp_reader *));
static int do_pragma_system_header	PARAMS ((cpp_reader *));
static int do_pragma_default		PARAMS ((cpp_reader *));

static int
do_pragma (pfile)
     cpp_reader *pfile;
{
  long here, key;
  U_CHAR *buf;
  int pop;
  enum cpp_ttype token;

  here = CPP_WRITTEN (pfile);
  CPP_PUTS (pfile, "#pragma ", 8);

  key = CPP_WRITTEN (pfile);
  pfile->no_macro_expand++;
  token = _cpp_get_directive_token (pfile);
  if (token != CPP_NAME)
    {
      if (token == CPP_VSPACE)
	goto empty;
      else
	goto skip;
    }

  buf = pfile->token_buffer + key;
  CPP_PUTC (pfile, ' ');

#define tokis(x) !strncmp((char *) buf, x, sizeof(x) - 1)
  if (tokis ("once"))
    pop = do_pragma_once (pfile);
  else if (tokis ("implementation"))
    pop = do_pragma_implementation (pfile);
  else if (tokis ("poison"))
    pop = do_pragma_poison (pfile);
  else if (tokis ("system_header"))
    pop = do_pragma_system_header (pfile);
  else
    pop = do_pragma_default (pfile);
#undef tokis

  if (_cpp_get_directive_token (pfile) != CPP_VSPACE)
    goto skip;

  if (pop)
    CPP_SET_WRITTEN (pfile, here);
  pfile->no_macro_expand--;
  return 0;

 skip:
  cpp_error (pfile, "malformed #pragma directive");
  _cpp_skip_rest_of_line (pfile);
 empty:
  CPP_SET_WRITTEN (pfile, here);
  pfile->no_macro_expand--;
  return 0;
}

static int
do_pragma_default (pfile)
     cpp_reader *pfile;
{
  while (_cpp_get_directive_token (pfile) != CPP_VSPACE)
    CPP_PUTC (pfile, ' ');
  return 0;
}

static int
do_pragma_once (pfile)
     cpp_reader *pfile;
{
  cpp_buffer *ip = CPP_BUFFER (pfile);

  /* Allow #pragma once in system headers, since that's not the user's
     fault.  */
  if (!ip->system_header_p)
    cpp_warning (pfile, "`#pragma once' is obsolete");
      
  if (CPP_PREV_BUFFER (ip) == NULL)
    cpp_warning (pfile, "`#pragma once' outside include file");
  else
    ip->ihash->control_macro = U"";  /* never repeat */

  return 1;
}

static int
do_pragma_implementation (pfile)
     cpp_reader *pfile;
{
  /* Be quiet about `#pragma implementation' for a file only if it hasn't
     been included yet.  */
  enum cpp_ttype token;
  long written = CPP_WRITTEN (pfile);
  U_CHAR *name;
  char *copy;
  size_t len;

  token = _cpp_get_directive_token (pfile);
  if (token == CPP_VSPACE)
    return 0;
  else if (token != CPP_STRING)
    {
      cpp_error (pfile, "malformed #pragma implementation");
      return 1;
    }

  /* Trim the leading and trailing quote marks from the string.  */
  name = pfile->token_buffer + written + 1;
  len = CPP_PWRITTEN (pfile) - name;
  copy = alloca (len);
  memcpy (copy, name, len - 1);
  copy[len - 1] = '\0';
  
  if (cpp_included (pfile, copy))
    cpp_warning (pfile,
	 "`#pragma implementation' for `%s' appears after file is included",
		 copy);
  return 0;
}

static int
do_pragma_poison (pfile)
     cpp_reader *pfile;
{
  /* Poison these symbols so that all subsequent usage produces an
     error message.  */
  U_CHAR *p;
  HASHNODE *hp;
  long written;
  size_t len;
  enum cpp_ttype token;
  int writeit;

  /* As a rule, don't include #pragma poison commands in output,  
     unless the user asks for them.  */
  writeit = (CPP_OPTION (pfile, debug_output)
	     || CPP_OPTION (pfile, dump_macros) == dump_definitions
	     || CPP_OPTION (pfile, dump_macros) == dump_names);

  for (;;)
    {
      written = CPP_WRITTEN (pfile);
      token = _cpp_get_directive_token (pfile);
      if (token == CPP_VSPACE)
	break;
      if (token != CPP_NAME)
	{
	  cpp_error (pfile, "invalid #pragma poison directive");
	  _cpp_skip_rest_of_line (pfile);
	  return 1;
	}

      p = pfile->token_buffer + written;
      len = CPP_PWRITTEN (pfile) - p;
      hp = _cpp_lookup (pfile, p, len);
      if (hp->type == T_POISON)
	;  /* It is allowed to poison the same identifier twice.  */
      else
	{
	  if (hp->type != T_VOID)
	    cpp_warning (pfile, "poisoning existing macro `%s'", hp->name);
	  _cpp_free_definition (hp);
	  hp->type = T_POISON;
	}
      if (writeit)
	CPP_PUTC (pfile, ' ');
    }
  return !writeit;
}

/* Mark the current header as a system header.  This will suppress
   some categories of warnings (notably those from -pedantic).  It is
   intended for use in system libraries that cannot be implemented in
   conforming C, but cannot be certain that their headers appear in a
   system include directory.  To prevent abuse, it is rejected in the
   primary source file.  */
static int
do_pragma_system_header (pfile)
     cpp_reader *pfile;
{
  cpp_buffer *ip = cpp_file_buffer (pfile);
  if (CPP_PREV_BUFFER (ip) == NULL)
    cpp_warning (pfile, "#pragma system_header outside include file");
  else
    ip->system_header_p = 1;

  return 1;
}
 
/* Just ignore #sccs, on systems where we define it at all.  */
#ifdef SCCS_DIRECTIVE
static int
do_sccs (pfile)
     cpp_reader *pfile;
{
  _cpp_skip_rest_of_line (pfile);
  return 0;
}
#endif

/* We've found an `#if' directive.  If the only thing before it in
   this file is white space, and if it is of the form
   `#if ! defined SYMBOL', then SYMBOL is a possible controlling macro
   for inclusion of this file.  (See redundant_include_p in cppfiles.c
   for an explanation of controlling macros.)  If so, return a
   malloced copy of SYMBOL.  Otherwise, return NULL.  */

static U_CHAR *
detect_if_not_defined (pfile)
     cpp_reader *pfile;
{
  U_CHAR *control_macro = 0;
  enum cpp_ttype token;
  unsigned int base_offset;
  unsigned int token_offset;
  unsigned int need_rparen = 0;
  unsigned int token_len;

  if (pfile->only_seen_white != 2)
    return NULL;

  /* Save state required for restore.  */
  pfile->no_macro_expand++;
  CPP_SET_MARK (pfile);
  base_offset = CPP_WRITTEN (pfile);

  /* Look for `!', */
  if (_cpp_get_directive_token (pfile) != CPP_OTHER
      || CPP_WRITTEN (pfile) != (size_t) base_offset + 1
      || CPP_PWRITTEN (pfile)[-1] != '!')
    goto restore;

  /* ...then `defined', */
  token_offset = CPP_WRITTEN (pfile);
  token = _cpp_get_directive_token (pfile);
  if (token != CPP_NAME)
    goto restore;
  if (ustrncmp (pfile->token_buffer + token_offset, U"defined", 7))
    goto restore;

  /* ...then an optional '(' and the name, */
  token_offset = CPP_WRITTEN (pfile);
  token = _cpp_get_directive_token (pfile);
  if (token == CPP_OPEN_PAREN)
    {
      token_offset = CPP_WRITTEN (pfile);
      need_rparen = 1;
      token = _cpp_get_directive_token (pfile);
    }
  if (token != CPP_NAME)
    goto restore;

  token_len = CPP_WRITTEN (pfile) - token_offset;

  /* ...then the ')', if necessary, */
  if (need_rparen && _cpp_get_directive_token (pfile) != CPP_CLOSE_PAREN)
    goto restore;

  /* ...and make sure there's nothing else on the line.  */
  if (_cpp_get_directive_token (pfile) != CPP_VSPACE)
    goto restore;

  /* We have a legitimate controlling macro for this header.  */
  control_macro = (U_CHAR *) xmalloc (token_len + 1);
  memcpy (control_macro, pfile->token_buffer + token_offset, token_len);
  control_macro[token_len] = '\0';

 restore:
  CPP_SET_WRITTEN (pfile, base_offset);
  pfile->no_macro_expand--;
  CPP_GOTO_MARK (pfile);

  return control_macro;
}

/*
 * #if is straightforward; just call _cpp_parse_expr, then conditional_skip.
 * Also, check for a reinclude preventer of the form #if !defined (MACRO).
 */

static int
do_if (pfile)
     cpp_reader *pfile;
{
  U_CHAR *control_macro;
  int value;
  int save_only_seen_white = pfile->only_seen_white;

  control_macro = detect_if_not_defined (pfile);  

  pfile->only_seen_white = 0;
  value = _cpp_parse_expr (pfile);
  pfile->only_seen_white = save_only_seen_white;

  return conditional_skip (pfile, value == 0, T_IF, control_macro);
}

/*
 * handle a #elif directive by not changing  if_stack  either.
 * see the comment above do_else.
 */

static int
do_elif (pfile)
     cpp_reader *pfile;
{
  if (pfile->if_stack == CPP_BUFFER (pfile)->if_stack)
    {
      cpp_error (pfile, "`#elif' not within a conditional");
      return 0;
    }
  else
    {
      if (pfile->if_stack->type == T_ELSE)
	{
	  cpp_error (pfile, "`#elif' after `#else'");
	  cpp_error_with_line (pfile, pfile->if_stack->lineno, 0,
			       "the conditional began here");
	}
      pfile->if_stack->type = T_ELIF;
    }

  if (pfile->if_stack->if_succeeded)
    {
      _cpp_skip_rest_of_line (pfile);
      return skip_if_group (pfile);
    }
  if (_cpp_parse_expr (pfile) == 0)
    return skip_if_group (pfile);

  ++pfile->if_stack->if_succeeded;	/* continue processing input */
  return 0;
}

/* Parse an #ifdef or #ifndef directive.  Returns 1 for defined, 0 for
   not defined; the macro tested is left in the token buffer (but
   popped).  */

static int
parse_ifdef (pfile, name)
     cpp_reader *pfile;
     const U_CHAR *name;
{
  U_CHAR *ident;
  unsigned int len;
  enum cpp_ttype token;
  long old_written = CPP_WRITTEN (pfile);
  int defined;

  pfile->no_macro_expand++;
  token = _cpp_get_directive_token (pfile);
  pfile->no_macro_expand--;

  ident = pfile->token_buffer + old_written;
  len = CPP_WRITTEN (pfile) - old_written;

  if (token == CPP_VSPACE)
    {
      if (! CPP_TRADITIONAL (pfile))
	cpp_pedwarn (pfile, "`#%s' with no argument", name);
      defined = 0;
      goto done;
    }
  else if (token == CPP_NAME)
    {
      defined = cpp_defined (pfile, ident, len);
      CPP_PUTC (pfile, '\0');  /* so it can be copied with xstrdup */
    }
  else
    {
      defined = 0;
      if (! CPP_TRADITIONAL (pfile))
	cpp_error (pfile, "`#%s' with invalid argument", name);
    }

  if (!CPP_TRADITIONAL (pfile))
    {
      if (_cpp_get_directive_token (pfile) == CPP_VSPACE)
	goto done;
      
      cpp_pedwarn (pfile, "garbage at end of `#%s' argument", name);
    }
  _cpp_skip_rest_of_line (pfile);
  
 done:
  CPP_SET_WRITTEN (pfile, old_written); /* Pop */
  return defined;
}

/* #ifdef is dead simple.  */

static int
do_ifdef (pfile)
     cpp_reader *pfile;
{
  int skip = ! parse_ifdef (pfile, dtable[T_IFDEF].name);
  return conditional_skip (pfile, skip, T_IFDEF, 0);
}

/* #ifndef is a tad more complex, because we need to check for a
   no-reinclusion wrapper.  */

static int
do_ifndef (pfile)
     cpp_reader *pfile;
{
  int start_of_file, skip;
  U_CHAR *control_macro = 0;

  start_of_file = pfile->only_seen_white == 2;
  skip = parse_ifdef (pfile, dtable[T_IFNDEF].name);

  if (start_of_file && !skip)
    control_macro = uxstrdup (CPP_PWRITTEN (pfile));

  return conditional_skip (pfile, skip, T_IFNDEF, control_macro);
}

/* Push TYPE on stack; then, if SKIP is nonzero, skip ahead.
   If this is a #ifndef starting at the beginning of a file,
   CONTROL_MACRO is the macro name tested by the #ifndef.
   Otherwise, CONTROL_MACRO is 0.  */

static int
conditional_skip (pfile, skip, type, control_macro)
     cpp_reader *pfile;
     int skip;
     int type;
     U_CHAR *control_macro;
{
  IF_STACK *temp;

  temp = (IF_STACK *) xcalloc (1, sizeof (IF_STACK));
  temp->lineno = CPP_BUFFER (pfile)->lineno;
  temp->next = pfile->if_stack;
  temp->control_macro = control_macro;
  pfile->if_stack = temp;

  pfile->if_stack->type = type;

  if (skip != 0)
    return skip_if_group (pfile);

  ++pfile->if_stack->if_succeeded;
  return 0;
}

/* Subroutine of skip_if_group.  Examine one preprocessing directive
   and return 0 if skipping should continue, or the directive number
   of the directive that ends the block if it should halt.

   Also adjusts the if_stack as appropriate.  The `#' has been read,
   but not the identifier. */

static int
consider_directive_while_skipping (pfile, stack)
    cpp_reader *pfile;
    IF_STACK *stack; 
{
  long ident;
  int i, hash_at_bol;
  unsigned int len;
  IF_STACK *temp;

  /* -traditional directives are recognized only with the # in column 1.  */
  hash_at_bol = CPP_IN_COLUMN_1 (pfile);

  ident = CPP_WRITTEN (pfile);
  if (_cpp_get_directive_token (pfile) != CPP_NAME)
    return 0;
  len = CPP_WRITTEN (pfile) - ident;

  for (i = 0; i < N_DIRECTIVES; i++)
    {
      if (dtable[i].length == len
	  && !ustrncmp (dtable[i].name, pfile->token_buffer + ident, len)) 
	goto real_directive;
    }
  return 0;

 real_directive:

  /* If it's not a directive of interest to us, return now.  */
  if (ORIGIN (dtable[i].flags) != COND)
    return 0;

  /* First, deal with -traditional and -Wtraditional.
     All COND directives are from K+R.  */

  if (! hash_at_bol)
    {
      if (CPP_TRADITIONAL (pfile))
	{
	  if (CPP_WTRADITIONAL (pfile))
	    cpp_warning (pfile, "ignoring #%s because of its indented #",
			 dtable[i].name);
	  return 0;
	}
      if (CPP_WTRADITIONAL (pfile))
	cpp_warning (pfile, "traditional C ignores %s with the # indented",
		     dtable[i].name);
    }
  
  switch (i)
    {
    default:
      cpp_ice (pfile, "non COND directive in switch in c_d_w_s");
      return 0;

    case T_IF:
    case T_IFDEF:
    case T_IFNDEF:
      temp = (IF_STACK *) xcalloc (1, sizeof (IF_STACK));
      temp->lineno = CPP_BUFFER (pfile)->lineno;
      temp->next = pfile->if_stack;
      temp->type = i;
      pfile->if_stack = temp;
      return 0;

    case T_ELSE:
      if (pfile->if_stack != stack)
	validate_else (pfile, dtable[i].name);
      /* fall through */
    case T_ELIF:
      if (pfile->if_stack == stack)
	return i;

      pfile->if_stack->type = i;
      return 0;

    case T_ENDIF:
      if (pfile->if_stack != stack)
	validate_else (pfile, dtable[i].name);

      if (pfile->if_stack == stack)
	return i;
		    
      temp = pfile->if_stack;
      pfile->if_stack = temp->next;
      free (temp);
      return 0;
    }
}

/* Skip to #endif, #else, or #elif.  Consumes the directive that
   causes it to stop, but not its argument.  Returns the number of
   that directive, which must be passed back up to
   _cpp_handle_directive, which will execute it.  */
static int
skip_if_group (pfile)
    cpp_reader *pfile;
{
  enum cpp_ttype token;
  IF_STACK *save_if_stack = pfile->if_stack; /* don't pop past here */
  long old_written;
  int ret = 0;

  /* We are no longer at the start of the file.  */
  pfile->only_seen_white = 0;

  old_written = CPP_WRITTEN (pfile);
  pfile->no_macro_expand++;
  for (;;)
    {
      /* We are at the end of a line.
	 XXX Serious layering violation here.  */
      int c = CPP_BUF_PEEK (CPP_BUFFER (pfile));
      if (c == EOF)
	break;  /* Caller will issue error.  */
      else if (c != '\n')
	cpp_ice (pfile, "character %c at end of line in skip_if_group", c);
      CPP_BUFFER (pfile)->cur++;
      CPP_BUMP_LINE (pfile);
      CPP_SET_WRITTEN (pfile, old_written);
      pfile->only_seen_white = 1;

      token = _cpp_get_directive_token (pfile);

      if (token == CPP_HASH)
	{
	  ret = consider_directive_while_skipping (pfile, save_if_stack);
	  if (ret)
	    break;
	}

      if (token != CPP_VSPACE)
	_cpp_skip_rest_of_line (pfile);
    }
  CPP_SET_WRITTEN (pfile, old_written);
  pfile->no_macro_expand--;
  return ret;
}

/*
 * handle a #else directive.  Do this by just continuing processing
 * without changing  if_stack ;  this is so that the error message
 * for missing #endif's etc. will point to the original #if.  It
 * is possible that something different would be better.
 */

static int
do_else (pfile)
     cpp_reader *pfile;
{
  validate_else (pfile, dtable[T_ELSE].name);
  _cpp_skip_rest_of_line (pfile);

  if (pfile->if_stack == CPP_BUFFER (pfile)->if_stack)
    {
      cpp_error (pfile, "`#else' not within a conditional");
      return 0;
    }
  else
    {
      /* #ifndef can't have its special treatment for containing the whole file
	 if it has a #else clause.  */
      pfile->if_stack->control_macro = 0;

      if (pfile->if_stack->type == T_ELSE)
	{
	  cpp_error (pfile, "`#else' after `#else'");
	  cpp_error_with_line (pfile, pfile->if_stack->lineno, 0,
			       "the conditional began here");
	}
      pfile->if_stack->type = T_ELSE;
    }

  if (pfile->if_stack->if_succeeded)
    return skip_if_group (pfile);
  
  ++pfile->if_stack->if_succeeded;	/* continue processing input */
  return 0;
}

/*
 * unstack after #endif command
 */

static int
do_endif (pfile)
     cpp_reader *pfile;
{
  validate_else (pfile, dtable[T_ENDIF].name);
  _cpp_skip_rest_of_line (pfile);

  if (pfile->if_stack == CPP_BUFFER (pfile)->if_stack)
    cpp_error (pfile, "`#endif' not within a conditional");
  else
    {
      IF_STACK *temp = pfile->if_stack;
      pfile->if_stack = temp->next;
      if (temp->control_macro != 0)
	pfile->potential_control_macro = temp->control_macro;
      free (temp);
    }
  return 0;
}

/* Issue -pedantic warning for text which is not a comment following
   an #else or #endif.  Do not warn in system headers, as this is harmless
   and very common on old systems.  */

static void
validate_else (pfile, directive)
     cpp_reader *pfile;
     const U_CHAR *directive;
{
  long old_written;
  if (! CPP_PEDANTIC (pfile))
    return;

  old_written = CPP_WRITTEN (pfile);
  pfile->no_macro_expand++;
  if (_cpp_get_directive_token (pfile) != CPP_VSPACE)
    cpp_pedwarn (pfile,
		 "text following `#%s' violates ANSI standard", directive);
  CPP_SET_WRITTEN (pfile, old_written);
  pfile->no_macro_expand--;
}

/* Called when we reach the end of a macro buffer.  Walk back up the
   conditional stack till we reach its level at entry to this file,
   issuing error messages.  */
void
_cpp_unwind_if_stack (pfile, pbuf)
     cpp_reader *pfile;
     cpp_buffer *pbuf;
{
  struct if_stack *ifs, *nifs;

  for (ifs = pfile->if_stack;
       ifs != pbuf->if_stack;
       ifs = nifs)
    {
      cpp_error_with_line (pfile, ifs->lineno, 0,
			   "unterminated `#%s' conditional",
			   dtable[ifs->type].name);

      nifs = ifs->next;
      free (ifs);
    }
  pfile->if_stack = ifs;
}

#define WARNING(msgid) do { cpp_warning(pfile, msgid); goto error; } while (0)
#define ERROR(msgid) do { cpp_error(pfile, msgid); goto error; } while (0)
#define ICE(msgid) do { cpp_ice(pfile, msgid); goto error; } while (0)
static int
do_assert (pfile)
     cpp_reader *pfile;
{
  long old_written;
  U_CHAR *sym;
  size_t len;
  HASHNODE *hp;
  struct predicate *pred = 0;
  enum cpp_ttype type;

  old_written = CPP_WRITTEN (pfile);
  pfile->no_macro_expand++;

  CPP_PUTC (pfile, '#');	/* force token out of macro namespace */
  type = _cpp_get_directive_token (pfile);
  if (type == CPP_VSPACE)
    ERROR ("#assert without predicate");
  else if (type != CPP_NAME)
    ERROR ("assertion predicate is not an identifier");

  sym = pfile->token_buffer + old_written;
  len = CPP_WRITTEN (pfile) - old_written;
  hp = _cpp_lookup (pfile, sym, len);

  if (_cpp_get_directive_token (pfile) != CPP_OPEN_PAREN)
    ERROR ("missing token-sequence in #assert");

  pred = (struct predicate *) xmalloc (sizeof (struct predicate));
  _cpp_init_toklist (&pred->answer);

  if (_cpp_scan_until (pfile, &pred->answer, CPP_CLOSE_PAREN)
      != CPP_CLOSE_PAREN)
    ERROR ("missing close paren in #assert");

  if (_cpp_get_directive_token (pfile) != CPP_CLOSE_PAREN)
    ICE ("impossible token, expecting ) in do_assert");

  if (_cpp_get_directive_token (pfile) != CPP_VSPACE)
    ERROR ("junk at end of #assert");

  if (hp->type == T_ASSERTION)
    {
      /* Check for reassertion.  */
      const struct predicate *old;

      for (old = hp->value.pred; old; old = old->next)
	if (_cpp_equiv_toklists (&pred->answer, &old->answer))
	  /* We used to warn about this, but SVR4 cc doesn't, so let's
	     match that (also consistent with #define).  goto error will
	     clean up.  */
	  goto error;
      pred->next = hp->value.pred;
    }
  else
    {
      hp->type = T_ASSERTION;
      pred->next = 0;
    }
  
  _cpp_squeeze_toklist (&pred->answer);
  hp->value.pred = pred;
  pfile->no_macro_expand--;
  CPP_SET_WRITTEN (pfile, old_written);
  return 0;

 error:
  _cpp_skip_rest_of_line (pfile);
  pfile->no_macro_expand--;
  CPP_SET_WRITTEN (pfile, old_written);
  if (pred)
    {
      _cpp_free_toklist (&pred->answer);
      free (pred);
    }
  return 0;
}

static int
do_unassert (pfile)
     cpp_reader *pfile;
{
  long old_written;
  U_CHAR *sym;
  size_t len;
  HASHNODE *hp;
  struct predicate *pred = 0;
  enum cpp_ttype type;

  old_written = CPP_WRITTEN (pfile);
  pfile->no_macro_expand++;

  CPP_PUTC (pfile, '#');	/* force token out of macro namespace */
  if (_cpp_get_directive_token (pfile) != CPP_NAME)
    ERROR ("#unassert must be followed by an identifier");

  sym = pfile->token_buffer + old_written;
  len = CPP_WRITTEN (pfile) - old_written;
  hp = _cpp_lookup (pfile, sym, len);

  type = _cpp_get_directive_token (pfile);
  if (type == CPP_OPEN_PAREN)
    {
      pred = (struct predicate *) xmalloc (sizeof (struct predicate));
      _cpp_init_toklist (&pred->answer);

      if (_cpp_scan_until (pfile, &pred->answer, CPP_CLOSE_PAREN)
	  != CPP_CLOSE_PAREN)
	ERROR ("missing close paren in #unassert");

      if (_cpp_get_directive_token (pfile) != CPP_CLOSE_PAREN)
	ICE ("impossible token, expecting ) in do_unassert");

      type = _cpp_get_directive_token (pfile);
    }

  if (type != CPP_VSPACE)
    ERROR ("junk at end of #unassert");

  if (hp->type != T_ASSERTION)
    /* Not an error to #unassert something that isn't asserted.
       goto error to clean up.  */
    goto error;

  if (pred)
    {
      /* Find this specific answer and remove it.  */
      struct predicate *o, *p;

      for (p = NULL, o = hp->value.pred; o; p = o, o = o->next)
	if (_cpp_equiv_toklists (&pred->answer, &o->answer))
	  {
	    if (p)
	      p->next = o->next;
	    else
	      hp->value.pred = o->next;

	    _cpp_free_toklist (&o->answer);
	    free (o);
	    break;
	  }
    }
  else
    {
      struct predicate *o, *p;
      for (o = hp->value.pred; o; o = p)
	{
	  p = o->next;
	  _cpp_free_toklist ((cpp_toklist *) &o->answer);
	  free (o);
	}
      hp->value.pred = NULL;
    }

  if (hp->value.pred == NULL)
    hp->type = T_VOID;  /* Last answer for this predicate deleted.  */

 error:
  _cpp_skip_rest_of_line (pfile);
  pfile->no_macro_expand--;
  CPP_SET_WRITTEN (pfile, old_written);
  if (pred)
    {
      _cpp_free_toklist (&pred->answer);
      free (pred);
    }
  return 0;
}

/* These are for -D, -U, -A.  */

/* Process the string STR as if it appeared as the body of a #define.
   If STR is just an identifier, define it with value 1.
   If STR has anything after the identifier, then it should
   be identifier=definition. */

void
cpp_define (pfile, str)
     cpp_reader *pfile;
     const char *str;
{
  char *buf, *p;
  size_t count;

  p = strchr (str, '=');
  /* Copy the entire option so we can modify it. 
     Change the first "=" in the string to a space.  If there is none,
     tack " 1" on the end.  Then add a newline and a NUL.  */
  
  if (p)
    {
      count = strlen (str) + 2;
      buf = (char *) alloca (count);
      memcpy (buf, str, count - 2);
      buf[p - str] = ' ';
      buf[count - 2] = '\n';
      buf[count - 1] = '\0';
    }
  else
    {
      count = strlen (str) + 4;
      buf = (char *) alloca (count);
      memcpy (buf, str, count - 4);
      strcpy (&buf[count-4], " 1\n");
    }

  if (cpp_push_buffer (pfile, (U_CHAR *)buf, count - 1) != NULL)
    {
      do_define (pfile);
      cpp_pop_buffer (pfile);
    }
}

/* Process MACRO as if it appeared as the body of an #undef.  */
void
cpp_undef (pfile, macro)
     cpp_reader *pfile;
     const char *macro;
{
  /* Copy the string so we can append a newline.  */
  size_t len = strlen (macro);
  char *buf = (char *) alloca (len + 2);
  memcpy (buf, macro, len);
  buf[len]     = '\n';
  buf[len + 1] = '\0';
  if (cpp_push_buffer (pfile, (U_CHAR *)buf, len + 1) != NULL)
    {
      do_undef (pfile);
      cpp_pop_buffer (pfile);
    }
}

/* Process the string STR as if it appeared as the body of a #assert. */
void
cpp_assert (pfile, str)
     cpp_reader *pfile;
     const char *str;
{
  if (cpp_push_buffer (pfile, (const U_CHAR *)str, strlen (str)) != NULL)
    {
      do_assert (pfile);
      cpp_pop_buffer (pfile);
    }
}

/* Process STR as if it appeared as the body of an #unassert. */
void
cpp_unassert (pfile, str)
     cpp_reader *pfile;
     const char *str;
{
  if (cpp_push_buffer (pfile, (const U_CHAR *)str, strlen (str)) != NULL)
    {
      do_unassert (pfile);
      cpp_pop_buffer (pfile);
    }
}  

/* Determine whether the identifier ID, of length LEN, is a defined macro.  */
int
cpp_defined (pfile, id, len)
     cpp_reader *pfile;
     const U_CHAR *id;
     int len;
{
  HASHNODE *hp = _cpp_lookup (pfile, id, len);
  if (hp->type == T_POISON)
    {
      cpp_error (pfile, "attempt to use poisoned `%s'", hp->name);
      return 0;
    }
  return (hp->type != T_VOID);
}
