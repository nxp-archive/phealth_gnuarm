/* Declarations for shared server code
   Copyright (C) 2007, 2008 Free Software Foundation, Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#ifndef GCC_SERVER_H
#define GCC_SERVER_H

/* Functions for the server to use.  */
extern void server_start (char *, int);
extern int server_main_loop (const char *progname, int, int);
extern bool server_start_back_end (bool *);
extern bool server_interrupted_p (void);

/* Functions for the client to use.  */
extern bool client_connect (const char *);
extern bool client_send_directory (void);
extern bool client_send_command (const char **);
extern bool client_wait (void);
extern void client_kill_server (const char *);

/* The main loop calls this when a command is read.  Return true on
   success, false on failure.  */
extern bool server_callback (int, char *, char **, char **);

/* Ensure that we are either running standalone, or in the
   code-generation subprocess.  */
extern void server_assert_code_generation (void);

#endif /* GCC_SERVER_H */
