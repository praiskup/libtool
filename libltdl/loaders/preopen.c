/* loader-preopen.c -- emulate dynamic linking using preloaded_symbols
   Copyright (C) 1998, 1999, 2000, 2004 Free Software Foundation, Inc.
   Originally by Thomas Tanner <tanner@ffii.org>

   NOTE: The canonical source of this file is maintained with the
   GNU Libtool package.  Report bugs to bug-libtool@gnu.org.

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.

As a special exception to the GNU Lesser General Public License,
if you distribute this file as part of a program or library that
is built using GNU libtool, you may include it under the same
distribution terms that you use for the rest of that program.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
02111-1307  USA

*/

#include "lt_dlloader.h"
#include "lt__private.h"

/* Use the preprocessor to rename non-static symbols to avoid namespace
   collisions when the loader code is statically linked into libltdl.
   Use the "<module_name>_LTX_" prefix so that the symbol addresses can
   be fetched from the preloaded symbol list by lt_dlsym():  */
#define get_vtable	preopen_LTX_get_vtable

extern lt_dlloader *get_vtable (lt_user_data loader_data);


/* Boilerplate code to set up the vtable for hooking this loader into
   libltdl's loader list:  */
static int	 vl_init  (lt_user_data loader_data);
static int	 vl_exit  (lt_user_data loader_data);
static lt_module vm_open  (lt_user_data loader_data, const char *filename);
static int	 vm_close (lt_user_data loader_data, lt_module module);
static void *	 vm_sym   (lt_user_data loader_data, lt_module module,
			  const char *symbolname);

/* Return the vtable for this loader, only the name and sym_prefix
   attributes (plus the virtual function implementations, obviously)
   change between loaders.  */
lt_dlloader *
get_vtable (lt_user_data loader_data)
{
  static lt_dlloader *vtable = 0;

  if (!vtable)
    {
      vtable = lt__malloc (sizeof *vtable);
    }

  if (!vtable->name)
    {
      vtable->name		= "lt_preopen";
      vtable->sym_prefix	= 0;
      vtable->dlloader_init	= vl_init;
      vtable->dlloader_exit	= vl_exit;
      vtable->module_open	= vm_open;
      vtable->module_close	= vm_close;
      vtable->find_sym		= vm_sym;
      vtable->dlloader_data	= loader_data;
      vtable->priority		= LT_DLLOADER_PREPEND;
    }

  if (vtable->dlloader_data != loader_data)
    {
      LT__SETERROR (INIT_LOADER);
      return 0;
    }

  return vtable;
}



/* --- IMPLEMENTATION --- */


/* Wrapper type to chain together symbol lists of various origins.  */
typedef struct symlist_chain
{
  struct symlist_chain *next;
  const lt_dlsymlist   *symlist;
} symlist_chain;


static int add_symlist   (const lt_dlsymlist *symlist);
static int free_symlists (void);

/* The start of the symbol lists chain.  */
static symlist_chain	       *preloaded_symlists		= 0;

/* A symbol list preloaded before lt_init() was called.  */
static const	lt_dlsymlist   *default_preloaded_symbols	= 0;


/* A function called through the vtable to initialise this loader.  */
static int
vl_init (lt_user_data loader_data)
{
  int errors = 0;

  preloaded_symlists = 0;
  if (default_preloaded_symbols)
    {
      errors = lt_dlpreload (default_preloaded_symbols);
    }

  return errors;
}


/* A function called through the vtable when this loader is no
   longer needed by the application.  */
static int
vl_exit (lt_user_data loader_data)
{
  free_symlists ();
  return 0;
}


/* A function called through the vtable to open a module with this
   loader.  Returns an opaque representation of the newly opened
   module for processing with this loader's other vtable functions.  */
static lt_module
vm_open (lt_user_data loader_data, const char *filename)
{
  symlist_chain *lists;
  lt_module	 module = 0;

  if (!preloaded_symlists)
    {
      LT__SETERROR (NO_SYMBOLS);
      goto done;
    }

  /* Can't use NULL as the reflective symbol header, as NULL is
     used to mark the end of the entire symbol list.  Self-dlpreopened
     symbols follow this magic number, chosen to be an unlikely
     clash with a real module name.  */
  if (!filename)
    {
      filename = "@PROGRAM@";
    }

  for (lists = preloaded_symlists; lists; lists = lists->next)
    {
      const lt_dlsymbol *symbol;
      for (symbol= lists->symlist->symbols; symbol->name; ++symbol)
	{
	  if (!symbol->address && streq (symbol->name, filename))
	    {
	      module = (lt_module) lists->symlist;
	      goto done;
	    }
	}
    }

  LT__SETERROR (FILE_NOT_FOUND);

 done:
  return module;
}


/* A function called through the vtable when a particular module
   should be unloaded.  */
static int
vm_close (lt_user_data loader_data, lt_module module)
{
  /* Just to silence gcc -Wall */
  module = 0;
  return 0;
}


/* A function called through the vtable to get the address of
   a symbol loaded from a particular module.  */
static void *
vm_sym (lt_user_data loader_data, lt_module module, const char *name)
{
  lt_dlsymlist	       *symlist = (lt_dlsymlist*) module;
  const lt_dlsymbol    *symbol  = symlist->symbols;

  ++symbol;			/* Skip header. */

  while (symbol->name)
    {
      if (streq (symbol->name, name))
	{
	  return symbol->address;
	}

    ++symbol;
  }

  LT__SETERROR (SYMBOL_NOT_FOUND);

  return 0;
}



/* --- HELPER FUNCTIONS --- */


/* The symbol lists themselves are not allocated from the heap, but
   we can unhook them and free up the chain of links between them.  */
static int
free_symlists (void)
{
  symlist_chain *lists;

  lists = preloaded_symlists;
  while (lists)
    {
      symlist_chain *next = lists->next;
      FREE (lists);
      lists = next;
    }
  preloaded_symlists = 0;

  return 0;
}

/* Add a new symbol list to the global chain.  */
static int
add_symlist (const lt_dlsymlist *symlist)
{
  symlist_chain *lists;
  int		 errors   = 0;

  /* Search for duplicate entries:  */
  for (lists = preloaded_symlists;
       lists && lists->symlist != symlist; lists = lists->next)
    /*NOWORK*/;

  /* Don't add the same list twice:  */
  if (!lists)
    {
      symlist_chain *tmp = lt__zalloc (sizeof *tmp);

      if (tmp)
	{
	  tmp->symlist = symlist;
	  tmp->next = preloaded_symlists;
	  preloaded_symlists = tmp;
	}
      else
	{
	  ++errors;
	}
    }

  return errors;
}



/* --- PRELOADING API CALL IMPLEMENTATIONS --- */


/* Save a default symbol list for later.  */
int
lt_dlpreload_default (const lt_dlsymlist *preloaded)
{
  default_preloaded_symbols = preloaded;
  return 0;
}


/* Add a symbol list to the global chain, or with a NULL argument,
   revert to just the default list.  */
int
lt_dlpreload (const lt_dlsymlist *preloaded)
{
  int errors = 0;

  if (preloaded)
    {
      errors = add_symlist (preloaded);
    }
  else
    {
      free_symlists();

      if (default_preloaded_symbols)
	{
	  errors = lt_dlpreload (default_preloaded_symbols);
	}
    }

  return errors;
}


/* Open all the preloaded modules from the named originator, executing
   a callback for each one.  */
int
lt_dlpreload_open (const char *originator, lt_dlpreload_callback_func *func)
{
  symlist_chain *list;
  int		 errors = 0;
  int		 found  = 0;

  /* For each symlist in the chain...  */
  for (list = preloaded_symlists; list; list = list->next)
    {
      /* ...that was preloaded by the requesting ORIGINATOR... */
      if (streq (list->symlist->originator, originator))
	{
	  const lt_dlsymbol *symbol;
	  unsigned int idx = 0;

	  ++found;

	  /* ...load the symbols per source compilation unit:  */
	  while ((symbol = &list->symlist->symbols[idx++])->name != 0)
	    {
	      if ((symbol->address == 0)
		  && (strneq (symbol->name, "@PROGRAM@")))
		{
		  lt_dlhandle handle = lt_dlopen (symbol->name);
		  if (handle == 0)
		    {
		      ++errors;
		    }
		  else
		    {
		      errors += (*func) (handle);
		    }
		}
	    }
	}

      if (!found)
	{
	  LT__SETERROR(CANNOT_OPEN);
	  ++errors;
	}
    }

  return errors;
}