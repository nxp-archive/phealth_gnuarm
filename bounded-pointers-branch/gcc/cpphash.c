/* Part of CPP library.  (Macro handling.)
   Copyright (C) 1986, 1987, 1989, 1992, 1993, 1994, 1995, 1996, 1998,
   1999, 2000 Free Software Foundation, Inc.
   Written by Per Bothner, 1994.
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
Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

 In other words, you are welcome to use, share and improve this program.
 You are forbidden to forbid anyone else to use, share and improve
 what you give them.   Help stamp out software-hoarding!  */

#include "config.h"
#include "system.h"
#include "cpplib.h"
#include "hashtab.h"
#include "cpphash.h"

#undef abort

static unsigned int hash_HASHNODE PARAMS ((const void *));
static int eq_HASHNODE		  PARAMS ((const void *, const void *));
static void del_HASHNODE	  PARAMS ((void *));
static int dump_hash_helper	  PARAMS ((void **, void *));

static void push_macro_expansion PARAMS ((cpp_reader *,
					  U_CHAR *, int, HASHNODE *));
static int unsafe_chars		 PARAMS ((cpp_reader *, int, int));
static int macro_cleanup	 PARAMS ((cpp_buffer *, cpp_reader *));
static enum cpp_ttype macarg	 PARAMS ((cpp_reader *, int));
static void special_symbol	 PARAMS ((HASHNODE *, cpp_reader *));

/* Initial hash table size.  (It can grow if necessary - see hashtab.c.)  */
#define HASHSIZE 500

/* The arglist structure is built by create_definition to tell
   collect_expansion where the argument names begin.  That
   is, for a define like "#define f(x,y,z) foo+x-bar*y", the arglist
   would contain pointers to the strings x, y, and z.
   collect_expansion would then build a DEFINITION node,
   with reflist nodes pointing to the places x, y, and z had
   appeared.  So the arglist is just convenience data passed
   between these two routines.  It is not kept around after
   the current #define has been processed and entered into the
   hash table.  */

struct arg
{
  const U_CHAR *name;
  unsigned int len;
  char rest_arg;
};

struct arglist
{
  U_CHAR *namebuf;
  const struct arg *argv;
  int argc;
};


static DEFINITION *collect_expansion PARAMS ((cpp_reader *, cpp_toklist *,
					      struct arglist *, unsigned int));
static unsigned int collect_params PARAMS ((cpp_reader *, cpp_toklist *,
					    struct arglist *));

static void warn_trad_stringify	PARAMS ((cpp_reader *, U_CHAR *, size_t,
					 unsigned int, const struct arg *));
static int duplicate_arg_p PARAMS ((U_CHAR *, U_CHAR *));

/* This structure represents one parsed argument in a macro call.
   `raw' points to the argument text as written (`raw_length' is its length).
   `expanded' points to the argument's macro-expansion
   (its length is `expand_length').
   `stringified_length' is the length the argument would have
   if stringified.  */

/* raw and expanded are relative to ARG_BASE */
#define ARG_BASE ((pfile)->token_buffer)

struct argdata
{
  /* Strings relative to pfile->token_buffer */
  long raw, expanded, stringified;
  int raw_length, expand_length;
  int stringified_length;
};

/* Calculate hash of a string of length LEN.  */
unsigned int
_cpp_calc_hash (str, len)
     const U_CHAR *str;
     size_t len;
{
  size_t n = len;
  unsigned int r = 0;

  do
    r = r * 67 + (*str++ - 113);
  while (--n);
  return r + len;
}

/* Calculate hash of a HASHNODE structure.  */
static unsigned int
hash_HASHNODE (x)
     const void *x;
{
  const HASHNODE *h = (const HASHNODE *)x;
  return h->hash;
}

/* Compare two HASHNODE structures.  */
static int
eq_HASHNODE (x, y)
     const void *x;
     const void *y;
{
  const HASHNODE *a = (const HASHNODE *)x;
  const HASHNODE *b = (const HASHNODE *)y;

  return (a->length == b->length
	  && !strncmp (a->name, b->name, a->length));
}

/* Destroy a HASHNODE.  */
static void
del_HASHNODE (x)
     void *x;
{
  HASHNODE *h = (HASHNODE *)x;
  
  if (h->type == T_MACRO)
    _cpp_free_definition (h->value.defn);
  else if (h->type == T_MCONST)
    free ((void *) h->value.cpval);
  free ((void *) h->name);
  free (h);
}

/* Allocate and initialize a HASHNODE structure.
   Caller must fill in the value field.  */

HASHNODE *
_cpp_make_hashnode (name, len, type, hash)
     const U_CHAR *name;
     size_t len;
     enum node_type type;
     unsigned long hash;
{
  HASHNODE *hp = (HASHNODE *) xmalloc (sizeof (HASHNODE));
  U_CHAR *p = xmalloc (len + 1);

  hp->type = type;
  hp->length = len;
  hp->name = p;
  hp->hash = hash;

  memcpy (p, name, len);
  p[len] = 0;

  return hp;
}

/* Find the hash node for name "name", which ends at the first
   non-identifier char.

   If LEN is >= 0, it is the length of the name.
   Otherwise, compute the length now.  */

HASHNODE *
_cpp_lookup (pfile, name, len)
     cpp_reader *pfile;
     const U_CHAR *name;
     int len;
{
  const U_CHAR *bp;
  HASHNODE dummy;

  if (len < 0)
    {
      for (bp = name; is_idchar (*bp); bp++);
      len = bp - name;
    }

  dummy.name = name;
  dummy.length = len;
  dummy.hash = _cpp_calc_hash (name, len);

  return (HASHNODE *) htab_find_with_hash (pfile->hashtab,
					   (void *)&dummy, dummy.hash);
}

/* Find the hashtable slot for name "name".  Used to insert or delete.  */

HASHNODE **
_cpp_lookup_slot (pfile, name, len, insert, hash)
     cpp_reader *pfile;
     const U_CHAR *name;
     int len;
     enum insert_option insert;
     unsigned long *hash;
{
  const U_CHAR *bp;
  HASHNODE dummy;
  HASHNODE **slot;

  if (len < 0)
    {
      for (bp = name; is_idchar (*bp); bp++)
	;

      len = bp - name;
    }

  dummy.name = name;
  dummy.length = len;
  dummy.hash = _cpp_calc_hash (name, len);

  slot = (HASHNODE **) htab_find_slot_with_hash (pfile->hashtab,
						 (void *) &dummy,
						 dummy.hash, insert);
  if (insert)
    *hash = dummy.hash;
  return slot;
}

/* Init the hash table.  In here so it can see the hash and eq functions.  */
void
_cpp_init_macro_hash (pfile)
     cpp_reader *pfile;
{
  pfile->hashtab = htab_create (HASHSIZE, hash_HASHNODE,
				eq_HASHNODE, del_HASHNODE);
}

/* Free a DEFINITION structure.  Used by delete_macro, and by
   do_define when redefining macros.  */

void
_cpp_free_definition (d)
     DEFINITION *d;
{
  struct reflist *ap, *nextap;

  for (ap = d->pattern; ap != NULL; ap = nextap)
    {
      nextap = ap->next;
      free (ap);
    }
  if (d->argnames)
    free (d->argnames);
  free (d);
}

static int
macro_cleanup (pbuf, pfile)
     cpp_buffer *pbuf;
     cpp_reader *pfile ATTRIBUTE_UNUSED;
{
  HASHNODE *macro = pbuf->macro;
  if (macro->type == T_DISABLED)
    macro->type = T_MACRO;
  if (macro->type != T_MACRO || pbuf->buf != macro->value.defn->expansion)
    free ((PTR) pbuf->buf);
  return 0;
}

/* Issue warnings for macro argument names seen inside strings.  */
static void
warn_trad_stringify (pfile, p, len, argc, argv)
     cpp_reader *pfile;
     U_CHAR *p;
     size_t len;
     unsigned int argc;
     const struct arg *argv;
     
{
  U_CHAR *limit;
  unsigned int i;

  limit = p + len;
  for (;;)
    {
      while (p < limit && !is_idstart (*p)) p++;
      if (p >= limit)
	break;

      for (i = 0; i < argc; i++)
	if (!strncmp (p, argv[i].name, argv[i].len)
	    && ! is_idchar (p[argv[i].len]))
	  {
	    cpp_warning (pfile,
		"macro argument \"%s\" would be stringified in traditional C",
			 argv[i].name);
	    break;
	  }
      p++;
      while (p < limit && is_idchar (*p)) p++;
      if (p >= limit)
	break;
    }
}

/* Read a replacement list for a macro, and build the DEFINITION
   structure.  LIST contains the replacement list, beginning at
   REPLACEMENT.  ARGLIST specifies the formal parameters to look for
   in the text of the definition.  If ARGLIST is null, this is an
   object-like macro; if it points to an empty arglist, this is a
   function-like macro with no arguments.  */

static DEFINITION *
collect_expansion (pfile, list, arglist, replacement)
     cpp_reader *pfile;
     cpp_toklist *list;
     struct arglist *arglist;
     unsigned int replacement;
{
  DEFINITION *defn;
  struct reflist *pat = 0, *endpat = 0;
  enum cpp_ttype token;
  long start, last;
  unsigned int i;
  int j, argc;
  size_t len;
  const struct arg *argv;
  U_CHAR *tok, *exp;
  enum { START = 0, NORM, ARG, STRIZE, PASTE } last_token = START;

  if (arglist)
    {
      argv = arglist->argv;
      argc = arglist->argc;
    }
  else
    {
      argv = 0;
      argc = 0;
    }

  /* We copy the expansion text into the token_buffer, then out to
     its proper home.  */
  last = start = CPP_WRITTEN (pfile);
  CPP_PUTS (pfile, "\r ", 2);

  for (i = replacement; i < list->tokens_used; i++)
    {
      token = list->tokens[i].type;
      tok = list->tokens[i].val.name.offset + list->namebuf;
      len = list->tokens[i].val.name.len;
      switch (token)
	{
	case CPP_POP:
	case CPP_EOF:
	  cpp_ice (pfile, "EOF in collect_expansion");
	  /* fall through */
	case CPP_VSPACE:
	  goto done;

	case CPP_HASH:
	  /* # is not special in object-like macros.  It is special in
	     function-like macros with no args.  (6.10.3.2 para 1.)
	     However, it is not special after PASTE. (Implied by
	     6.10.3.3 para 4.)  */
	  if (arglist == NULL || last_token == PASTE)
	    goto norm;
	  last_token = STRIZE;
	  break;

	case CPP_PASTE:
	  if (last_token == PASTE)
	    /* ## ## - the second ## is ordinary.  */
	    goto norm;
	  else if (last_token == START)
	    cpp_error (pfile, "`##' at start of macro definition");
	    
	  else if (last_token == ARG)
	    /* If the last token was an argument, mark it raw_after.  */
	    endpat->raw_after = 1;
	  else if (last_token == STRIZE)
	    /* Oops - that wasn't a stringify operator.  */
	    CPP_PUTC (pfile, '#');

	  last_token = PASTE;
	  break;

	case CPP_STRING:
	case CPP_CHAR:
	  if (argc && CPP_WTRADITIONAL (pfile))
	    warn_trad_stringify (pfile, tok, len, argc, argv);
	  goto norm;
	  
	case CPP_NAME:
	  for (j = 0; j < argc; j++)
	    if (argv[j].len == len
		&& !strncmp (tok, argv[j].name, argv[j].len))
	      goto addref;

	  /* fall through */
	default:
	norm:
	  if (last_token == STRIZE)
	    cpp_error (pfile, "# is not followed by a macro argument name");
	  if (last_token != PASTE && last_token != START
	      && (list->tokens[i].flags & HSPACE_BEFORE))
	    CPP_PUTC (pfile, ' ');
	  CPP_PUTS (pfile, tok, len);
	  last_token = NORM;
	  break;
	}
      continue;

    addref:
      {
	struct reflist *tpat;
	if (last_token != PASTE && (list->tokens[i].flags & HSPACE_BEFORE))
	  CPP_PUTC (pfile, ' ');

	/* Make a pat node for this arg and add it to the pat list */
	tpat = (struct reflist *) xmalloc (sizeof (struct reflist));
	tpat->next = NULL;
	tpat->raw_before = (last_token == PASTE);
	tpat->raw_after = 0;
	tpat->stringify = (last_token == STRIZE);
	tpat->rest_args = argv[j].rest_arg;
	tpat->argno = j;
	tpat->nchars = CPP_WRITTEN (pfile) - last;

	if (endpat == NULL)
	  pat = tpat;
	else
	  endpat->next = tpat;
	endpat = tpat;
	last = CPP_WRITTEN (pfile);
      }
      last_token = ARG;
    }
 done:

  if (last_token == STRIZE)
    cpp_error (pfile, "`#' is not followed by a macro argument name");
  else if (last_token == PASTE)
    cpp_error (pfile, "`##' at end of macro definition");

    CPP_PUTS (pfile, "\r ", 2);
  len = CPP_WRITTEN (pfile) - start;
  CPP_SET_WRITTEN (pfile, start);

  exp = (U_CHAR *) xmalloc (len + 1);
  memcpy (exp, pfile->token_buffer + start, len);
  exp[len] = '\0';

  defn = (DEFINITION *) xmalloc (sizeof (DEFINITION));
  defn->length = len;
  defn->expansion = exp;
  defn->pattern = pat;
  defn->rest_args = argv && argv[argc - 1].rest_arg;
  if (arglist)
    {
      defn->nargs = argc;
      defn->argnames = arglist->namebuf;
      if (argv)
	free ((PTR) argv);
    }
  else
    {
      defn->nargs = -1;
      defn->argnames = 0;
    }
  return defn;
}

/* Is argument NEW, which has just been added to the argument list,
   a duplicate of a previous argument name?  */
static int
duplicate_arg_p (args, new)
     U_CHAR *args, *new;
{
  size_t newlen = strlen (new) + 1;
  size_t oldlen;

  while (args < new)
    {
      oldlen = strlen (args) + 1;
      if (!memcmp (args, new, MIN (oldlen, newlen)))
	return 1;
      args += oldlen;
    }
  return 0;
}

static unsigned int
collect_params (pfile, list, arglist)
     cpp_reader *pfile;
     cpp_toklist *list;
     struct arglist *arglist;
{
  struct arg *argv = 0;
  U_CHAR *namebuf, *p, *tok;
  unsigned int len, argslen;
  unsigned int argc, a, i, j;

  /* The formal parameters list starts at token 1.  */
  if (list->tokens[1].type != CPP_OPEN_PAREN)
    {
      cpp_ice (pfile, "first token = %d not %d in collect_formal_parameters",
	       list->tokens[1].type, CPP_OPEN_PAREN);
      return 0;
    }

  /* Scan once and count the number of parameters; also check for
     syntax errors here.  */
  argc = 0;
  argslen = 0;
  for (i = 2; i < list->tokens_used; i++)
    switch (list->tokens[i].type)
      {
      case CPP_NAME:
	argslen += list->tokens[i].val.name.len + 1;
	argc++;
	break;
      case CPP_COMMA:
	break;
      case CPP_CLOSE_PAREN:
	goto scanned;
      case CPP_VSPACE:
	cpp_error_with_line (pfile, list->line, list->tokens[i].col,
			     "missing right paren in macro argument list");
	return 0;

      default:
	cpp_error_with_line (pfile, list->line, list->tokens[i].col,
			     "syntax error in #define");
	return 0;

      case CPP_ELLIPSIS:
	if (list->tokens[i-1].type != CPP_NAME)
	  {
	    argslen += sizeof "__VA_ARGS__";
	    argc++;
	  }
	i++;
	if (list->tokens[i].type != CPP_CLOSE_PAREN)
	  {
	    cpp_error_with_line (pfile, list->line, list->tokens[i].col,
				 "another parameter follows \"...\"");
	    return 0;
	  }
	goto scanned;
      }

  cpp_ice (pfile, "collect_params: unreachable - i=%d, ntokens=%d, type=%d",
	   i, list->tokens_used, list->tokens[i-1].type);
  return 0;

 scanned:
  if (argc == 0)	/* function-like macro, no arguments */
    {
      arglist->argc = 0;
      arglist->argv = 0;
      arglist->namebuf = 0;
      return i + 1;
    }
  if (argslen == 0)
    {
      cpp_ice (pfile, "collect_params: argc=%d argslen=0", argc);
      return 0;
    }

  /* Now allocate space and copy the suckers.  */
  argv = (struct arg *) xmalloc (argc * sizeof (struct arg));
  namebuf = (U_CHAR *) xmalloc (argslen);
  p = namebuf;
  a = 0;
  for (j = 2; j < i; j++)
    switch (list->tokens[j].type)
      {
      case CPP_NAME:
	tok = list->tokens[j].val.name.offset + list->namebuf;
	len = list->tokens[j].val.name.len;
	memcpy (p, tok, len);
	p[len] = '\0';
	if (duplicate_arg_p (namebuf, p))
	  {
	    cpp_error (pfile, "duplicate macro argument name \"%s\"", tok);
	    a++;
	    break;
	  }
	if (CPP_PEDANTIC (pfile) && CPP_OPTION (pfile, c99)
	    && len == sizeof "__VA_ARGS__" - 1
	    && !strcmp (p, "__VA_ARGS__"))
	  cpp_pedwarn (pfile,
	"C99 does not permit use of __VA_ARGS__ as a macro argument name");
	argv[a].len = len;
	argv[a].name = p;
	argv[a].rest_arg = 0;
	p += len;
	a++;
	break;

      case CPP_COMMA:
	break;

      case CPP_ELLIPSIS:
	if (list->tokens[j-1].type != CPP_NAME)
	  {
	    if (CPP_PEDANTIC (pfile) && ! CPP_OPTION (pfile, c99))
	      cpp_pedwarn (pfile, "C89 does not permit varargs macros");

	    argv[a].len = sizeof "__VA_ARGS__" - 1;
	    argv[a].name = p;
	    argv[a].rest_arg = 1;
	    strcpy (p, "__VA_ARGS__");
	  }
	else
	  {
	    if (CPP_PEDANTIC (pfile))
	      cpp_pedwarn (pfile,
			   "ISO C does not permit named varargs macros");
	    argv[a-1].rest_arg = 1;
	  }
	break;

      default:
	cpp_ice (pfile, "collect_params: impossible token type %d",
		 list->tokens[j].type);
      }

  arglist->argc = argc;
  arglist->argv = argv;
  arglist->namebuf = namebuf;
  return i + 1;
}

/* Create a DEFINITION node for a macro.  The replacement text
   (including formal parameters if present) is in LIST.  If FUNLIKE is
   true, this is a function-like macro.  */

DEFINITION *
_cpp_create_definition (pfile, list, funlike)
     cpp_reader *pfile;
     cpp_toklist *list;
     int funlike;
{
  struct arglist args;
  DEFINITION *defn;
  int replacement = 1;  /* replacement begins at this token */

  if (funlike)
    {
      replacement = collect_params (pfile, list, &args);
      if (replacement == 0)
	return 0;
    }

  defn = collect_expansion (pfile, list, funlike ? &args : 0, replacement);
  if (defn == 0)
    return 0;

  defn->file = CPP_BUFFER (pfile)->nominal_fname;
  defn->line = list->line;
  defn->col  = list->tokens[0].col;
  return defn;
}

/*
 * Parse a macro argument and append the info on PFILE's token_buffer.
 * REST_ARGS means to absorb the rest of the args.
 * Return nonzero to indicate a syntax error.
 */

static enum cpp_ttype
macarg (pfile, rest_args)
     cpp_reader *pfile;
     int rest_args;
{
  int paren = 0;
  enum cpp_ttype token;

  /* Try to parse as much of the argument as exists at this
     input stack level.  */
  for (;;)
    {
      token = cpp_get_token (pfile);
      switch (token)
	{
	case CPP_EOF:
	  return token;
	case CPP_POP:
	  /* If we've hit end of file, it's an error (reported by caller).
	     Ditto if it's the end of cpp_expand_to_buffer text.
	     If we've hit end of macro, just continue.  */
	  if (!CPP_IS_MACRO_BUFFER (CPP_BUFFER (pfile)))
	    return token;
	  break;
	case CPP_OPEN_PAREN:
	  paren++;
	  break;
	case CPP_CLOSE_PAREN:
	  if (--paren < 0)
	    goto found;
	  break;
	case CPP_COMMA:
	  /* if we've returned to lowest level and
	     we aren't absorbing all args */
	  if (paren == 0 && rest_args == 0)
	    goto found;
	  break;
	found:
	  /* Remove ',' or ')' from argument buffer.  */
	  CPP_ADJUST_WRITTEN (pfile, -1);
	  return token;
	default:;
	}
    }
}


static const char * const monthnames[] =
{
  "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

/* Place into PFILE a quoted string representing the string SRC.
   Caller must reserve enough space in pfile->token_buffer.  */

void
_cpp_quote_string (pfile, src)
     cpp_reader *pfile;
     const char *src;
{
  U_CHAR c;

  CPP_PUTC_Q (pfile, '\"');
  for (;;)
    switch ((c = *src++))
      {
      default:
        if (ISPRINT (c))
	  CPP_PUTC_Q (pfile, c);
	else
	  {
	    sprintf ((char *)CPP_PWRITTEN (pfile), "\\%03o", c);
	    CPP_ADJUST_WRITTEN (pfile, 4);
	  }
	break;

      case '\"':
      case '\\':
	CPP_PUTC_Q (pfile, '\\');
	CPP_PUTC_Q (pfile, c);
	break;
      
      case '\0':
	CPP_PUTC_Q (pfile, '\"');
	return;
      }
}

/*
 * expand things like __FILE__.  Place the expansion into the output
 * buffer *without* rescanning.
 */

#define DSC(str) (const U_CHAR *)str, sizeof str - 1
static void
special_symbol (hp, pfile)
     HASHNODE *hp;
     cpp_reader *pfile;
{
  const char *buf;
  cpp_buffer *ip;

  switch (hp->type)
    {
    case T_FILE:
    case T_BASE_FILE:
      ip = cpp_file_buffer (pfile);
      if (ip == NULL)
	{
	  CPP_PUTS (pfile, "\"\"", 2);
	  return;
	}
      if (hp->type == T_BASE_FILE)
	while (CPP_PREV_BUFFER (ip) != NULL)
	  ip = CPP_PREV_BUFFER (ip);

      buf = ip->nominal_fname;
      CPP_RESERVE (pfile, 3 + 4 * strlen (buf));
      _cpp_quote_string (pfile, buf);
      return;

    case T_INCLUDE_LEVEL:
      {
	int true_indepth = 0;
	ip = cpp_file_buffer (pfile);
	while (ip)
	  {
	    true_indepth++;
	    ip = CPP_PREV_BUFFER (ip);
	  }

	CPP_RESERVE (pfile, 10);
	sprintf (CPP_PWRITTEN (pfile), "%d", true_indepth);
	CPP_ADJUST_WRITTEN (pfile, strlen (CPP_PWRITTEN (pfile)));
	return;
      }

    case T_STDC:
#ifdef STDC_0_IN_SYSTEM_HEADERS
      ip = cpp_file_buffer (pfile);
      if (ip && ip->system_header_p
	  && !cpp_defined (pfile, DSC("__STRICT_ANSI__")))
	{
	  CPP_PUTC (pfile, '0');
	  return;
	}
#endif
      /* else fall through */
    case T_CONST:
    case T_MCONST:
    constant:
      buf = hp->value.cpval;
      if (!buf)
	return;
      if (*buf == '\0')
	buf = "\r \r ";

      CPP_PUTS (pfile, buf, strlen (buf));
      return;

    case T_SPECLINE:
      ip = cpp_file_buffer (pfile);
      if (ip == NULL)
	{
	  CPP_PUTC (pfile, '0');
	  return;
	}
      CPP_RESERVE (pfile, 10);
      sprintf (CPP_PWRITTEN (pfile), "%u", CPP_BUF_LINE (ip));
      CPP_ADJUST_WRITTEN (pfile, strlen (CPP_PWRITTEN (pfile)));
      return;

    case T_DATE:
    case T_TIME:
      /* Generate both __DATE__ and __TIME__, stuff them into their
	 respective hash nodes, and mark the nodes T_MCONST so we
	 don't have to do this again.  We don't generate these strings
	 at init time because time() and localtime() are very slow on
	 some systems.  */
      {
	time_t tt = time (NULL);
	struct tm *tb = localtime (&tt);
	HASHNODE *d, *t;

	if (hp->type == T_DATE)
	  d = hp, t = _cpp_lookup (pfile, DSC("__TIME__"));
	else
	  t = hp, d = _cpp_lookup (pfile, DSC("__DATE__"));

	d->value.cpval = xmalloc (sizeof "'Oct 11 1347'");
	sprintf ((char *)d->value.cpval, "\"%s %2d %4d\"",
		 monthnames[tb->tm_mon], tb->tm_mday, tb->tm_year + 1900);
	d->type = T_MCONST;

	t->value.cpval = xmalloc (sizeof "'12:34:56'");
	sprintf ((char *)t->value.cpval, "\"%02d:%02d:%02d\"",
		 tb->tm_hour, tb->tm_min, tb->tm_sec);
	t->type = T_MCONST;
	goto constant;
      }

    case T_POISON:
      cpp_error (pfile, "attempt to use poisoned `%s'.", hp->name);
      CPP_PUTC (pfile, '0');
      break;

    default:
      cpp_ice (pfile, "invalid special hash type");
      return;
    }
}
#undef DSC

/* Expand a macro call.
   HP points to the symbol that is the macro being called.
   Put the result of expansion onto the input stack
   so that subsequent input by our caller will use it.

   If macro wants arguments, caller has already verified that
   an argument list follows; arguments come from the input stack.  */

void
_cpp_macroexpand (pfile, hp)
     cpp_reader *pfile;
     HASHNODE *hp;
{
  int nargs;
  DEFINITION *defn;
  register U_CHAR *xbuf;
  unsigned int start_line, start_column;
  cpp_buffer *ip;
  int xbuf_len;
  struct argdata *args = 0;
  long old_written = CPP_WRITTEN (pfile);
  int rest_args, rest_zero = 0;
  register int i;

  ip = cpp_file_buffer (pfile);
  if (ip)
    {
      start_line = CPP_BUF_LINE (ip);
      start_column = CPP_BUF_COL (ip);
    }
  else
    start_line = start_column = 0;

  /* Check for and handle special symbols. */
  if (hp->type != T_MACRO)
    {
      special_symbol (hp, pfile);
      xbuf_len = CPP_WRITTEN (pfile) - old_written;
      xbuf = (U_CHAR *) xmalloc (xbuf_len + 1);
      CPP_SET_WRITTEN (pfile, old_written);
      memcpy (xbuf, CPP_PWRITTEN (pfile), xbuf_len + 1);
      push_macro_expansion (pfile, xbuf, xbuf_len, hp);
      CPP_BUFFER (pfile)->has_escapes = 1;
      return;
    }

  defn = hp->value.defn;
  nargs = defn->nargs;
  pfile->output_escapes++;

  if (nargs >= 0)
    {
      enum cpp_ttype token;

      args = (struct argdata *) alloca ((nargs + 1) * sizeof (struct argdata));

      for (i = 0; i < nargs; i++)
	{
	  args[i].raw = args[i].expanded = 0;
	  args[i].raw_length = 0;
	  args[i].expand_length = args[i].stringified_length = -1;
	}

      /* Parse all the macro args that are supplied.  I counts them.
         The first NARGS args are stored in ARGS.
         The rest are discarded.  If rest_args is set then we assume
         macarg absorbed the rest of the args.  */
      i = 0;
      rest_args = 0;

      /* Skip over the opening parenthesis.  */
      CPP_OPTION (pfile, discard_comments)++;
      pfile->no_macro_expand++;
      pfile->no_directives++;

      token = cpp_get_non_space_token (pfile);
      if (token != CPP_OPEN_PAREN)
	cpp_ice (pfile, "macroexpand: unexpected token %d (wanted LPAREN)",
		 token);
      CPP_ADJUST_WRITTEN (pfile, -1);

      token = CPP_EOF;
      do
	{
	  if (rest_args)
	    continue;
	  if (i < nargs || (nargs == 0 && i == 0))
	    {
	      /* if we are working on last arg which absorbs rest of args... */
	      if (i == nargs - 1 && defn->rest_args)
		rest_args = 1;
	      args[i].raw = CPP_WRITTEN (pfile);
	      token = macarg (pfile, rest_args);
	      args[i].raw_length = CPP_WRITTEN (pfile) - args[i].raw;
	    }
	  else
	    token = macarg (pfile, 0);
	  if (token == CPP_EOF || token == CPP_POP)
	    cpp_error_with_line (pfile, start_line, start_column,
				 "unterminated macro call");
	  i++;
	}
      while (token == CPP_COMMA);
      CPP_OPTION (pfile, discard_comments)--;
      pfile->no_macro_expand--;
      pfile->no_directives--;
      if (token != CPP_CLOSE_PAREN)
	return;

      /* foo ( ) is equivalent to foo () unless foo takes exactly one
	 argument, in which case the former is allowed and the latter
	 is not.  XXX C99 is silent on this rule, but it seems
	 inconsistent to me.  */
      if (i == 1 && nargs != 1)
	{
	  register U_CHAR *bp = ARG_BASE + args[0].raw;
	  register U_CHAR *lim = bp + args[0].raw_length;
	    while (bp != lim && is_space(*bp))
	      bp++;
	  if (bp == lim)
	    i = 0;
	}

      /* Don't output an error message if we have already output one for
         a parse error above.  */
      rest_zero = 0;
      if (nargs == 0 && i > 0)
	{
	  cpp_error (pfile, "arguments given to macro `%s'", hp->name);
	}
      else if (i < nargs)
	{
	  /* the rest args token is allowed to absorb 0 tokens */
	  if (i == nargs - 1 && defn->rest_args)
	    rest_zero = 1;
	  else if (i == 0)
	    cpp_error (pfile, "macro `%s' used without args", hp->name);
	  else if (i == 1)
	    cpp_error (pfile, "macro `%s' used with just one arg", hp->name);
	  else
	    cpp_error (pfile, "macro `%s' used with only %d args",
		       hp->name, i);
	}
      else if (i > nargs)
	{
	  cpp_error (pfile,
		     "macro `%s' used with too many (%d) args", hp->name, i);
	}
    }

  /* If macro wants zero args, we parsed the arglist for checking only.
     Read directly from the macro definition.  */
  if (nargs <= 0)
    {
      xbuf = defn->expansion;
      xbuf_len = defn->length;
    }
  else
    {
      register U_CHAR *exp = defn->expansion;
      register int offset;	/* offset in expansion,
				   copied a piece at a time */
      register int totlen;	/* total amount of exp buffer filled so far */

      register struct reflist *ap, *last_ap;

      /* Macro really takes args.  Compute the expansion of this call.  */

      /* Compute length in characters of the macro's expansion.
         Also count number of times each arg is used.  */
      xbuf_len = defn->length;
      for (ap = defn->pattern; ap != NULL; ap = ap->next)
	{
	  if (ap->stringify)
	    {
	      register struct argdata *arg = &args[ap->argno];
	      /* Stringify if it hasn't already been */
	      if (arg->stringified_length < 0)
		{
		  int arglen = arg->raw_length;
		  int escaped = 0;
		  int in_string = 0;
		  int c;
		  /* Initially need_space is -1.  Otherwise, 1 means the
		     previous character was a space, but we suppressed it;
		     0 means the previous character was a non-space.  */
		  int need_space = -1;
		  i = 0;
		  arg->stringified = CPP_WRITTEN (pfile);
		  CPP_PUTC (pfile, '\"');	/* insert beginning quote */
		  for (; i < arglen; i++)
		    {
		      c = (ARG_BASE + arg->raw)[i];

		      if (!in_string)
			{
			  /* Delete "\r " and "\r-" escapes.  */
			  if (c == '\r')
			    {
			      i++;
			      continue;
			    }
			  /* Internal sequences of whitespace are
			     replaced by one space except within
			     a string or char token. */
			  else if (is_space(c))
			    {
			      if (need_space == 0)
				need_space = 1;
			      continue;
			    }
			  else if (need_space > 0)
			    CPP_PUTC (pfile, ' ');
			  need_space = 0;
			}

		      if (escaped)
			escaped = 0;
		      else
			{
			  if (c == '\\')
			    escaped = 1;
			  if (in_string)
			    {
			      if (c == in_string)
				in_string = 0;
			    }
			  else if (c == '\"' || c == '\'')
			    in_string = c;
			}

		      /* Escape these chars */
		      if (c == '\"' || (in_string && c == '\\'))
			CPP_PUTC (pfile, '\\');
		      if (ISPRINT (c))
			CPP_PUTC (pfile, c);
		      else
			{
			  CPP_RESERVE (pfile, 4);
			  sprintf ((char *) CPP_PWRITTEN (pfile), "\\%03o",
				   (unsigned int) c);
			  CPP_ADJUST_WRITTEN (pfile, 4);
			}
		    }
		  CPP_PUTC (pfile, '\"');	/* insert ending quote */
		  arg->stringified_length
		    = CPP_WRITTEN (pfile) - arg->stringified;
		}
	      xbuf_len += args[ap->argno].stringified_length;
	    }
	  else if (ap->raw_before || ap->raw_after)
	    /* Add 4 for two \r-space markers to prevent
	       token concatenation.  */
	    xbuf_len += args[ap->argno].raw_length + 4;
	  else
	    {
	      /* We have an ordinary (expanded) occurrence of the arg.
	         So compute its expansion, if we have not already.  */
	      if (args[ap->argno].expand_length < 0)
		{
		  args[ap->argno].expanded = CPP_WRITTEN (pfile);
		  _cpp_expand_to_buffer (pfile,
					 ARG_BASE + args[ap->argno].raw,
					 args[ap->argno].raw_length);

		  args[ap->argno].expand_length
		    = CPP_WRITTEN (pfile) - args[ap->argno].expanded;
		}

	      /* Add 4 for two \r-space markers to prevent
	         token concatenation.  */
	      xbuf_len += args[ap->argno].expand_length + 4;
	    }
	}

      xbuf = (U_CHAR *) xmalloc (xbuf_len + 1);

      /* Generate in XBUF the complete expansion
         with arguments substituted in.
         TOTLEN is the total size generated so far.
         OFFSET is the index in the definition
         of where we are copying from.  */
      offset = totlen = 0;
      for (last_ap = NULL, ap = defn->pattern; ap != NULL;
	   last_ap = ap, ap = ap->next)
	{
	  register struct argdata *arg = &args[ap->argno];
	  int count_before = totlen;

	  /* Add chars to XBUF.  */
	  i = ap->nchars;
	  memcpy (&xbuf[totlen], &exp[offset], i);
	  totlen += i;
	  offset += i;

	  /* If followed by an empty rest arg with concatenation,
	     delete the last run of nonwhite chars.  */
	  if (rest_zero && totlen > count_before
	      && ((ap->rest_args && ap->raw_before)
		  || (last_ap != NULL && last_ap->rest_args
		      && last_ap->raw_after)))
	    {
	      /* Delete final whitespace.  */
	      while (totlen > count_before && is_space(xbuf[totlen - 1]))
		totlen--;

	      /* Delete the nonwhites before them.  */
	      while (totlen > count_before && !is_space(xbuf[totlen - 1]))
		totlen--;
	    }

	  if (ap->stringify != 0)
	    {
	      memcpy (xbuf + totlen, ARG_BASE + arg->stringified,
		      arg->stringified_length);
	      totlen += arg->stringified_length;
	    }
	  else if (ap->raw_before || ap->raw_after)
	    {
	      U_CHAR *p1 = ARG_BASE + arg->raw;
	      U_CHAR *l1 = p1 + arg->raw_length;
	      if (ap->raw_before)
		{
		  /* Arg is concatenated before: delete leading whitespace,
		     whitespace markers, and no-reexpansion markers.  */
		  while (p1 != l1)
		    {
		      if (is_space(p1[0]))
			p1++;
		      else if (p1[0] == '\r')
			p1 += 2;
		      else
			break;
		    }
		}
	      if (ap->raw_after)
		{
		  /* Arg is concatenated after: delete trailing whitespace,
		     whitespace markers, and no-reexpansion markers.  */
		  while (p1 != l1)
		    {
		      if (is_space(l1[-1]))
			l1--;
		      else if (l1[-1] == '\r')
			l1--;
		      else if (l1[-1] == '-')
			{
			  if (l1 != p1 + 1 && l1[-2] == '\r')
			    l1 -= 2;
			  else
			    break;
			}
		      else
			break;
		    }
		}

	      /* Delete any no-reexpansion marker that precedes
	         an identifier at the beginning of the argument. */
	      if (p1[0] == '\r' && p1[1] == '-')
		p1 += 2;

	      memcpy (xbuf + totlen, p1, l1 - p1);
	      totlen += l1 - p1;
	    }
	  else
	    {
	      U_CHAR *expanded = ARG_BASE + arg->expanded;
	      if (!ap->raw_before && totlen > 0 && arg->expand_length
		  && unsafe_chars (pfile, xbuf[totlen - 1], expanded[0]))
		{
		  xbuf[totlen++] = '\r';
		  xbuf[totlen++] = ' ';
		}

	      memcpy (xbuf + totlen, expanded, arg->expand_length);
	      totlen += arg->expand_length;

	      if (!ap->raw_after && totlen > 0 && offset < defn->length
		  && unsafe_chars (pfile, xbuf[totlen - 1], exp[offset]))
		{
		  xbuf[totlen++] = '\r';
		  xbuf[totlen++] = ' ';
		}
	    }

	  if (totlen > xbuf_len)
	    {
	      cpp_ice (pfile, "buffer overrun in macroexpand");
	      return;
	    }
	}

      /* if there is anything left of the definition
         after handling the arg list, copy that in too.  */

      for (i = offset; i < defn->length; i++)
	{
	  /* if we've reached the end of the macro */
	  if (exp[i] == ')')
	    rest_zero = 0;
	  if (!(rest_zero && last_ap != NULL && last_ap->rest_args
		&& last_ap->raw_after))
	    xbuf[totlen++] = exp[i];
	}

      xbuf[totlen] = 0;
      xbuf_len = totlen;

    }

  pfile->output_escapes--;

  /* Now put the expansion on the input stack
     so our caller will commence reading from it.  */
  push_macro_expansion (pfile, xbuf, xbuf_len, hp);
  CPP_BUFFER (pfile)->has_escapes = 1;

  /* Pop the space we've used in the token_buffer for argument expansion.  */
  CPP_SET_WRITTEN (pfile, old_written);

  /* Per C89, a macro cannot be expanded recursively.  */
  hp->type = T_DISABLED;
}

/* Return 1 iff a token ending in C1 followed directly by a token C2
   could cause mis-tokenization.  */

static int
unsafe_chars (pfile, c1, c2)
     cpp_reader *pfile;
     int c1, c2;
{
  /* If c2 is EOF, that's always safe.  */
  if (c2 == EOF)
    return 0;

  switch (c1)
    {
    case EOF:
      /* We don't know what the previous character was.  We do know
	 that it can't have been an idchar (or else it would have been
	 pasted with the idchars of the macro name), and there are a
	 number of second characters for which it doesn't matter what
	 the first was.  */
      if (is_idchar (c2) || c2 == '\'' || c2 == '\"'
	  || c2 == '(' || c2 == '[' || c2 == '{'
	  || c2 == ')' || c2 == ']' || c2 == '}')
	return 0;
      return 1;

    case '+':  case '-':
      if (c2 == c1 || c2 == '=')
	return 1;
      goto letter;

    case 'e':  case 'E':  case 'p':  case 'P':
      if (c2 == '-' || c2 == '+')
	return 1;		/* could extend a pre-processing number */
      goto letter;

    case '$':
      if (CPP_OPTION (pfile, dollars_in_ident))
	goto letter;
      return 0;

    case 'L':
      if (c2 == '\'' || c2 == '\"')
	return 1;		/* Could turn into L"xxx" or L'xxx'.  */
      goto letter;

    case '.':  case '0':  case '1':  case '2':  case '3':
    case '4':  case '5':  case '6':  case '7':  case '8':  case '9':
    case '_':  case 'a':  case 'b':  case 'c':  case 'd':  case 'f':
    case 'g':  case 'h':  case 'i':  case 'j':  case 'k':  case 'l':
    case 'm':  case 'n':  case 'o':  case 'q':  case 'r':  case 's':
    case 't':  case 'u':  case 'v':  case 'w':  case 'x':  case 'y':
    case 'z':  case 'A':  case 'B':  case 'C':  case 'D':  case 'F':
    case 'G':  case 'H':  case 'I':  case 'J':  case 'K':  case 'M':
    case 'N':  case 'O':  case 'Q':  case 'R':  case 'S':  case 'T':
    case 'U':  case 'V':  case 'W':  case 'X':  case 'Y':  case 'Z':
    letter:
    /* We're in the middle of either a name or a pre-processing number.  */
      return (is_idchar(c2) || c2 == '.');

    case '<':  case '>':  case '!':  case '%':  case '#':  case ':':
    case '^':  case '&':  case '|':  case '*':  case '/':  case '=':
      return (c2 == c1 || c2 == '=');
    }
  return 0;
}

static void
push_macro_expansion (pfile, xbuf, len, hp)
     cpp_reader *pfile;
     register U_CHAR *xbuf;
     int len;
     HASHNODE *hp;
{
  cpp_buffer *mbuf;
  int advance_cur = 0;

  /* The first chars of the expansion should be a "\r " added by
     collect_expansion.  This is to prevent accidental token-pasting
     between the text preceding the macro invocation, and the macro
     expansion text.

     We would like to avoid adding unneeded spaces (for the sake of
     tools that use cpp, such as imake).  In some common cases we can
     tell that it is safe to omit the space.  */

  if (xbuf[0] == '\r' && xbuf[1] == ' '
      && !unsafe_chars (pfile, EOF, xbuf[2]))
    advance_cur = 1;

  /* Likewise, avoid the extra space at the end of the macro expansion
     if this is safe.  We can do a better job here since we can know
     what the next char will be.  */
  if (len >= 3 && xbuf[len-2] == '\r' && xbuf[len-1] == ' '
      && !unsafe_chars (pfile, xbuf[len-3], CPP_BUF_PEEK (CPP_BUFFER (pfile))))
    len -= 2;

  /* If the total expansion is "\r \r", we must not trim both escapes.  */
  if (len == 2 && advance_cur)
    advance_cur = 0;

  mbuf = cpp_push_buffer (pfile, xbuf, len);
  if (mbuf == NULL)
    return;
  if (advance_cur)
    mbuf->cur += 2;
  mbuf->cleanup = macro_cleanup;
  mbuf->macro = hp;
}

/* Return zero if two DEFINITIONs are isomorphic.  */

int
_cpp_compare_defs (pfile, d1, d2)
     cpp_reader *pfile;
     DEFINITION *d1, *d2;
{
  struct reflist *a1, *a2;

  if (d1->nargs != d2->nargs)
    return 1;
  if (strcmp (d1->expansion, d2->expansion))
    return 1;
  if (CPP_PEDANTIC (pfile)
      && d1->argnames && d2->argnames)
    {
      U_CHAR *arg1 = d1->argnames;
      U_CHAR *arg2 = d2->argnames;
      size_t len;
      int i = d1->nargs;
      while (i--)
	{
	  len = strlen (arg1) + 1;
	  if (strcmp (arg1, arg2))
	    return 1;
	  arg1 += len;
	  arg2 += len;
	}
    }
  for (a1 = d1->pattern, a2 = d2->pattern; a1 && a2;
       a1 = a1->next, a2 = a2->next)
    {
      if (a1->nchars != a2->nchars
	  || a1->argno != a2->argno
	  || a1->stringify != a2->stringify
	  || a1->raw_before != a2->raw_before
	  || a1->raw_after != a2->raw_after)
	return 1;
    }
  if (a1 != a2)
    return 1;

  return 0;
}

/* Dump the definition of macro MACRO on stdout.  The format is suitable
   to be read back in again. */

void
_cpp_dump_definition (pfile, sym, len, defn)
     cpp_reader *pfile;
     const U_CHAR *sym;
     long len;
     DEFINITION *defn;
{
  CPP_RESERVE (pfile, len + sizeof "#define ");
  CPP_PUTS_Q (pfile, "#define ", sizeof "#define " -1);
  CPP_PUTS_Q (pfile, sym, len);

  if (defn->nargs == -1)
    {
      CPP_PUTC_Q (pfile, ' ');

      /* The first and last two characters of a macro expansion are
	 always "\r "; this needs to be trimmed out.
	 So we need length-4 chars of space, plus one for the NUL.  */
      CPP_RESERVE (pfile, defn->length - 4 + 1);
      CPP_PUTS_Q (pfile, defn->expansion + 2, defn->length - 4);
    }
  else
    {
      struct reflist *r;
      unsigned char **argv = (unsigned char **) alloca (defn->nargs *
							sizeof(char *));
      int *argl = (int *) alloca (defn->nargs * sizeof(int));
      unsigned char *x;
      int i;

      /* First extract the argument list. */
      x = defn->argnames;
      for (i = 0; i < defn->nargs; i++)
	{
	  argv[i] = x;
	  argl[i] = strlen (x);
	  x += argl[i] + 1;
	}
      
      /* Now print out the argument list. */
      CPP_PUTC_Q (pfile, '(');
      for (i = 0; i < defn->nargs; i++)
	{
	  CPP_RESERVE (pfile, argl[i] + 2);
	  if (!(i == defn->nargs-1 && defn->rest_args
		&& !strcmp (argv[i], "__VA_ARGS__")))
	    CPP_PUTS_Q (pfile, argv[i], argl[i]);
	  if (i < defn->nargs-1)
	    CPP_PUTS_Q (pfile, ", ", 2);
	}
      if (defn->rest_args)
	CPP_PUTS (pfile, "...", 3);
      CPP_PUTS (pfile, ") ", 2);

      /* Now the definition. */
      x = defn->expansion;
      for (r = defn->pattern; r; r = r->next)
      {
	i = r->nchars;
	if (*x == '\r') x += 2, i -= 2;
	/* i chars for macro text, plus the length of the macro
	   argument name, plus one for a stringify marker, plus two for
	   each concatenation marker. */
	CPP_RESERVE (pfile,
		     i + argl[r->argno] + r->stringify
		     + (r->raw_before + r->raw_after) * 2);

	if (i > 0) CPP_PUTS_Q (pfile, x, i);
	if (r->raw_before)
	  CPP_PUTS_Q (pfile, "##", 2);
	if (r->stringify)
	  CPP_PUTC_Q (pfile, '#');
	CPP_PUTS_Q (pfile, argv[r->argno], argl[r->argno]);
	if (r->raw_after && !(r->next && r->next->nchars == 0
			      && r->next->raw_before))
	  CPP_PUTS_Q (pfile, "##", 2);

	x += i;
      }

      i = defn->length - (x - defn->expansion) - 2;
      if (*x == '\r') x += 2, i -= 2;
      if (i > 0) CPP_PUTS (pfile, x, i);
    }
  if (CPP_BUFFER (pfile) == 0 || ! pfile->done_initializing)
    CPP_PUTC (pfile, '\n');
}

/* Dump out the hash table.  */
static int
dump_hash_helper (h, p)
     void **h;
     void *p;
{
  HASHNODE *hp = (HASHNODE *)*h;
  cpp_reader *pfile = (cpp_reader *)p;

  if (hp->type == T_MACRO)
    _cpp_dump_definition (pfile, hp->name, hp->length, hp->value.defn);
  return 1;
}

void
_cpp_dump_macro_hash (pfile)
     cpp_reader *pfile;
{
  htab_traverse (pfile->hashtab, dump_hash_helper, pfile);
}
