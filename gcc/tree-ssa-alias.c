/* Alias analysis for trees.
   Copyright (C) 2004-2019 Free Software Foundation, Inc.
   Contributed by Diego Novillo <dnovillo@redhat.com>

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "target.h"
#include "rtl.h"
#include "tree.h"
#include "gimple.h"
#include "timevar.h"	/* for TV_ALIAS_STMT_WALK */
#include "ssa.h"
#include "cgraph.h"
#include "tree-pretty-print.h"
#include "alias.h"
#include "fold-const.h"
#include "langhooks.h"
#include "dumpfile.h"
#include "tree-eh.h"
#include "tree-dfa.h"
#include "ipa-reference.h"
#include "varasm.h"

/* Broad overview of how alias analysis on gimple works:

   Statements clobbering or using memory are linked through the
   virtual operand factored use-def chain.  The virtual operand
   is unique per function, its symbol is accessible via gimple_vop (cfun).
   Virtual operands are used for efficiently walking memory statements
   in the gimple IL and are useful for things like value-numbering as
   a generation count for memory references.

   SSA_NAME pointers may have associated points-to information
   accessible via the SSA_NAME_PTR_INFO macro.  Flow-insensitive
   points-to information is (re-)computed by the TODO_rebuild_alias
   pass manager todo.  Points-to information is also used for more
   precise tracking of call-clobbered and call-used variables and
   related disambiguations.

   This file contains functions for disambiguating memory references,
   the so called alias-oracle and tools for walking of the gimple IL.

   The main alias-oracle entry-points are

   bool stmt_may_clobber_ref_p (gimple *, tree)

     This function queries if a statement may invalidate (parts of)
     the memory designated by the reference tree argument.

   bool ref_maybe_used_by_stmt_p (gimple *, tree)

     This function queries if a statement may need (parts of) the
     memory designated by the reference tree argument.

   There are variants of these functions that only handle the call
   part of a statement, call_may_clobber_ref_p and ref_maybe_used_by_call_p.
   Note that these do not disambiguate against a possible call lhs.

   bool refs_may_alias_p (tree, tree)

     This function tries to disambiguate two reference trees.

   bool ptr_deref_may_alias_global_p (tree)

     This function queries if dereferencing a pointer variable may
     alias global memory.

   More low-level disambiguators are available and documented in
   this file.  Low-level disambiguators dealing with points-to
   information are in tree-ssa-structalias.c.  */

static int nonoverlapping_refs_since_match_p (tree, tree, tree, tree, bool);
static bool nonoverlapping_component_refs_p (const_tree, const_tree);

/* Query statistics for the different low-level disambiguators.
   A high-level query may trigger multiple of them.  */

static struct {
  unsigned HOST_WIDE_INT refs_may_alias_p_may_alias;
  unsigned HOST_WIDE_INT refs_may_alias_p_no_alias;
  unsigned HOST_WIDE_INT ref_maybe_used_by_call_p_may_alias;
  unsigned HOST_WIDE_INT ref_maybe_used_by_call_p_no_alias;
  unsigned HOST_WIDE_INT call_may_clobber_ref_p_may_alias;
  unsigned HOST_WIDE_INT call_may_clobber_ref_p_no_alias;
  unsigned HOST_WIDE_INT aliasing_component_refs_p_may_alias;
  unsigned HOST_WIDE_INT aliasing_component_refs_p_no_alias;
  unsigned HOST_WIDE_INT nonoverlapping_component_refs_p_may_alias;
  unsigned HOST_WIDE_INT nonoverlapping_component_refs_p_no_alias;
  unsigned HOST_WIDE_INT nonoverlapping_refs_since_match_p_may_alias;
  unsigned HOST_WIDE_INT nonoverlapping_refs_since_match_p_must_overlap;
  unsigned HOST_WIDE_INT nonoverlapping_refs_since_match_p_no_alias;
} alias_stats;

void
dump_alias_stats (FILE *s)
{
  fprintf (s, "\nAlias oracle query stats:\n");
  fprintf (s, "  refs_may_alias_p: "
	   HOST_WIDE_INT_PRINT_DEC" disambiguations, "
	   HOST_WIDE_INT_PRINT_DEC" queries\n",
	   alias_stats.refs_may_alias_p_no_alias,
	   alias_stats.refs_may_alias_p_no_alias
	   + alias_stats.refs_may_alias_p_may_alias);
  fprintf (s, "  ref_maybe_used_by_call_p: "
	   HOST_WIDE_INT_PRINT_DEC" disambiguations, "
	   HOST_WIDE_INT_PRINT_DEC" queries\n",
	   alias_stats.ref_maybe_used_by_call_p_no_alias,
	   alias_stats.refs_may_alias_p_no_alias
	   + alias_stats.ref_maybe_used_by_call_p_may_alias);
  fprintf (s, "  call_may_clobber_ref_p: "
	   HOST_WIDE_INT_PRINT_DEC" disambiguations, "
	   HOST_WIDE_INT_PRINT_DEC" queries\n",
	   alias_stats.call_may_clobber_ref_p_no_alias,
	   alias_stats.call_may_clobber_ref_p_no_alias
	   + alias_stats.call_may_clobber_ref_p_may_alias);
  fprintf (s, "  nonoverlapping_component_refs_p: "
	   HOST_WIDE_INT_PRINT_DEC" disambiguations, "
	   HOST_WIDE_INT_PRINT_DEC" queries\n",
	   alias_stats.nonoverlapping_component_refs_p_no_alias,
	   alias_stats.nonoverlapping_component_refs_p_no_alias
	   + alias_stats.nonoverlapping_component_refs_p_may_alias);
  fprintf (s, "  nonoverlapping_refs_since_match_p: "
	   HOST_WIDE_INT_PRINT_DEC" disambiguations, "
	   HOST_WIDE_INT_PRINT_DEC" must overlaps, "
	   HOST_WIDE_INT_PRINT_DEC" queries\n",
	   alias_stats.nonoverlapping_refs_since_match_p_no_alias,
	   alias_stats.nonoverlapping_refs_since_match_p_must_overlap,
	   alias_stats.nonoverlapping_refs_since_match_p_no_alias
	   + alias_stats.nonoverlapping_refs_since_match_p_may_alias
	   + alias_stats.nonoverlapping_refs_since_match_p_must_overlap);
  fprintf (s, "  aliasing_component_refs_p: "
	   HOST_WIDE_INT_PRINT_DEC" disambiguations, "
	   HOST_WIDE_INT_PRINT_DEC" queries\n",
	   alias_stats.aliasing_component_refs_p_no_alias,
	   alias_stats.aliasing_component_refs_p_no_alias
	   + alias_stats.aliasing_component_refs_p_may_alias);
  dump_alias_stats_in_alias_c (s);
}


/* Return true, if dereferencing PTR may alias with a global variable.  */

bool
ptr_deref_may_alias_global_p (tree ptr)
{
  struct ptr_info_def *pi;

  /* If we end up with a pointer constant here that may point
     to global memory.  */
  if (TREE_CODE (ptr) != SSA_NAME)
    return true;

  pi = SSA_NAME_PTR_INFO (ptr);

  /* If we do not have points-to information for this variable,
     we have to punt.  */
  if (!pi)
    return true;

  /* ???  This does not use TBAA to prune globals ptr may not access.  */
  return pt_solution_includes_global (&pi->pt);
}

/* Return true if dereferencing PTR may alias DECL.
   The caller is responsible for applying TBAA to see if PTR
   may access DECL at all.  */

static bool
ptr_deref_may_alias_decl_p (tree ptr, tree decl)
{
  struct ptr_info_def *pi;

  /* Conversions are irrelevant for points-to information and
     data-dependence analysis can feed us those.  */
  STRIP_NOPS (ptr);

  /* Anything we do not explicilty handle aliases.  */
  if ((TREE_CODE (ptr) != SSA_NAME
       && TREE_CODE (ptr) != ADDR_EXPR
       && TREE_CODE (ptr) != POINTER_PLUS_EXPR)
      || !POINTER_TYPE_P (TREE_TYPE (ptr))
      || (!VAR_P (decl)
	  && TREE_CODE (decl) != PARM_DECL
	  && TREE_CODE (decl) != RESULT_DECL))
    return true;

  /* Disregard pointer offsetting.  */
  if (TREE_CODE (ptr) == POINTER_PLUS_EXPR)
    {
      do
	{
	  ptr = TREE_OPERAND (ptr, 0);
	}
      while (TREE_CODE (ptr) == POINTER_PLUS_EXPR);
      return ptr_deref_may_alias_decl_p (ptr, decl);
    }

  /* ADDR_EXPR pointers either just offset another pointer or directly
     specify the pointed-to set.  */
  if (TREE_CODE (ptr) == ADDR_EXPR)
    {
      tree base = get_base_address (TREE_OPERAND (ptr, 0));
      if (base
	  && (TREE_CODE (base) == MEM_REF
	      || TREE_CODE (base) == TARGET_MEM_REF))
	ptr = TREE_OPERAND (base, 0);
      else if (base
	       && DECL_P (base))
	return compare_base_decls (base, decl) != 0;
      else if (base
	       && CONSTANT_CLASS_P (base))
	return false;
      else
	return true;
    }

  /* Non-aliased variables cannot be pointed to.  */
  if (!may_be_aliased (decl))
    return false;

  /* If we do not have useful points-to information for this pointer
     we cannot disambiguate anything else.  */
  pi = SSA_NAME_PTR_INFO (ptr);
  if (!pi)
    return true;

  return pt_solution_includes (&pi->pt, decl);
}

/* Return true if dereferenced PTR1 and PTR2 may alias.
   The caller is responsible for applying TBAA to see if accesses
   through PTR1 and PTR2 may conflict at all.  */

bool
ptr_derefs_may_alias_p (tree ptr1, tree ptr2)
{
  struct ptr_info_def *pi1, *pi2;

  /* Conversions are irrelevant for points-to information and
     data-dependence analysis can feed us those.  */
  STRIP_NOPS (ptr1);
  STRIP_NOPS (ptr2);

  /* Disregard pointer offsetting.  */
  if (TREE_CODE (ptr1) == POINTER_PLUS_EXPR)
    {
      do
	{
	  ptr1 = TREE_OPERAND (ptr1, 0);
	}
      while (TREE_CODE (ptr1) == POINTER_PLUS_EXPR);
      return ptr_derefs_may_alias_p (ptr1, ptr2);
    }
  if (TREE_CODE (ptr2) == POINTER_PLUS_EXPR)
    {
      do
	{
	  ptr2 = TREE_OPERAND (ptr2, 0);
	}
      while (TREE_CODE (ptr2) == POINTER_PLUS_EXPR);
      return ptr_derefs_may_alias_p (ptr1, ptr2);
    }

  /* ADDR_EXPR pointers either just offset another pointer or directly
     specify the pointed-to set.  */
  if (TREE_CODE (ptr1) == ADDR_EXPR)
    {
      tree base = get_base_address (TREE_OPERAND (ptr1, 0));
      if (base
	  && (TREE_CODE (base) == MEM_REF
	      || TREE_CODE (base) == TARGET_MEM_REF))
	return ptr_derefs_may_alias_p (TREE_OPERAND (base, 0), ptr2);
      else if (base
	       && DECL_P (base))
	return ptr_deref_may_alias_decl_p (ptr2, base);
      else
	return true;
    }
  if (TREE_CODE (ptr2) == ADDR_EXPR)
    {
      tree base = get_base_address (TREE_OPERAND (ptr2, 0));
      if (base
	  && (TREE_CODE (base) == MEM_REF
	      || TREE_CODE (base) == TARGET_MEM_REF))
	return ptr_derefs_may_alias_p (ptr1, TREE_OPERAND (base, 0));
      else if (base
	       && DECL_P (base))
	return ptr_deref_may_alias_decl_p (ptr1, base);
      else
	return true;
    }

  /* From here we require SSA name pointers.  Anything else aliases.  */
  if (TREE_CODE (ptr1) != SSA_NAME
      || TREE_CODE (ptr2) != SSA_NAME
      || !POINTER_TYPE_P (TREE_TYPE (ptr1))
      || !POINTER_TYPE_P (TREE_TYPE (ptr2)))
    return true;

  /* We may end up with two empty points-to solutions for two same pointers.
     In this case we still want to say both pointers alias, so shortcut
     that here.  */
  if (ptr1 == ptr2)
    return true;

  /* If we do not have useful points-to information for either pointer
     we cannot disambiguate anything else.  */
  pi1 = SSA_NAME_PTR_INFO (ptr1);
  pi2 = SSA_NAME_PTR_INFO (ptr2);
  if (!pi1 || !pi2)
    return true;

  /* ???  This does not use TBAA to prune decls from the intersection
     that not both pointers may access.  */
  return pt_solutions_intersect (&pi1->pt, &pi2->pt);
}

/* Return true if dereferencing PTR may alias *REF.
   The caller is responsible for applying TBAA to see if PTR
   may access *REF at all.  */

static bool
ptr_deref_may_alias_ref_p_1 (tree ptr, ao_ref *ref)
{
  tree base = ao_ref_base (ref);

  if (TREE_CODE (base) == MEM_REF
      || TREE_CODE (base) == TARGET_MEM_REF)
    return ptr_derefs_may_alias_p (ptr, TREE_OPERAND (base, 0));
  else if (DECL_P (base))
    return ptr_deref_may_alias_decl_p (ptr, base);

  return true;
}

/* Returns true if PTR1 and PTR2 compare unequal because of points-to.  */

bool
ptrs_compare_unequal (tree ptr1, tree ptr2)
{
  /* First resolve the pointers down to a SSA name pointer base or
     a VAR_DECL, PARM_DECL or RESULT_DECL.  This explicitely does
     not yet try to handle LABEL_DECLs, FUNCTION_DECLs, CONST_DECLs
     or STRING_CSTs which needs points-to adjustments to track them
     in the points-to sets.  */
  tree obj1 = NULL_TREE;
  tree obj2 = NULL_TREE;
  if (TREE_CODE (ptr1) == ADDR_EXPR)
    {
      tree tem = get_base_address (TREE_OPERAND (ptr1, 0));
      if (! tem)
	return false;
      if (VAR_P (tem)
	  || TREE_CODE (tem) == PARM_DECL
	  || TREE_CODE (tem) == RESULT_DECL)
	obj1 = tem;
      else if (TREE_CODE (tem) == MEM_REF)
	ptr1 = TREE_OPERAND (tem, 0);
    }
  if (TREE_CODE (ptr2) == ADDR_EXPR)
    {
      tree tem = get_base_address (TREE_OPERAND (ptr2, 0));
      if (! tem)
	return false;
      if (VAR_P (tem)
	  || TREE_CODE (tem) == PARM_DECL
	  || TREE_CODE (tem) == RESULT_DECL)
	obj2 = tem;
      else if (TREE_CODE (tem) == MEM_REF)
	ptr2 = TREE_OPERAND (tem, 0);
    }

  /* Canonicalize ptr vs. object.  */
  if (TREE_CODE (ptr1) == SSA_NAME && obj2)
    {
      std::swap (ptr1, ptr2);
      std::swap (obj1, obj2);
    }

  if (obj1 && obj2)
    /* Other code handles this correctly, no need to duplicate it here.  */;
  else if (obj1 && TREE_CODE (ptr2) == SSA_NAME)
    {
      struct ptr_info_def *pi = SSA_NAME_PTR_INFO (ptr2);
      /* We may not use restrict to optimize pointer comparisons.
         See PR71062.  So we have to assume that restrict-pointed-to
	 may be in fact obj1.  */
      if (!pi
	  || pi->pt.vars_contains_restrict
	  || pi->pt.vars_contains_interposable)
	return false;
      if (VAR_P (obj1)
	  && (TREE_STATIC (obj1) || DECL_EXTERNAL (obj1)))
	{
	  varpool_node *node = varpool_node::get (obj1);
	  /* If obj1 may bind to NULL give up (see below).  */
	  if (! node
	      || ! node->nonzero_address ()
	      || ! decl_binds_to_current_def_p (obj1))
	    return false;
	}
      return !pt_solution_includes (&pi->pt, obj1);
    }

  /* ???  We'd like to handle ptr1 != NULL and ptr1 != ptr2
     but those require pt.null to be conservatively correct.  */

  return false;
}

/* Returns whether reference REF to BASE may refer to global memory.  */

static bool
ref_may_alias_global_p_1 (tree base)
{
  if (DECL_P (base))
    return is_global_var (base);
  else if (TREE_CODE (base) == MEM_REF
	   || TREE_CODE (base) == TARGET_MEM_REF)
    return ptr_deref_may_alias_global_p (TREE_OPERAND (base, 0));
  return true;
}

bool
ref_may_alias_global_p (ao_ref *ref)
{
  tree base = ao_ref_base (ref);
  return ref_may_alias_global_p_1 (base);
}

bool
ref_may_alias_global_p (tree ref)
{
  tree base = get_base_address (ref);
  return ref_may_alias_global_p_1 (base);
}

/* Return true whether STMT may clobber global memory.  */

bool
stmt_may_clobber_global_p (gimple *stmt)
{
  tree lhs;

  if (!gimple_vdef (stmt))
    return false;

  /* ???  We can ask the oracle whether an artificial pointer
     dereference with a pointer with points-to information covering
     all global memory (what about non-address taken memory?) maybe
     clobbered by this call.  As there is at the moment no convenient
     way of doing that without generating garbage do some manual
     checking instead.
     ???  We could make a NULL ao_ref argument to the various
     predicates special, meaning any global memory.  */

  switch (gimple_code (stmt))
    {
    case GIMPLE_ASSIGN:
      lhs = gimple_assign_lhs (stmt);
      return (TREE_CODE (lhs) != SSA_NAME
	      && ref_may_alias_global_p (lhs));
    case GIMPLE_CALL:
      return true;
    default:
      return true;
    }
}


/* Dump alias information on FILE.  */

void
dump_alias_info (FILE *file)
{
  unsigned i;
  tree ptr;
  const char *funcname
    = lang_hooks.decl_printable_name (current_function_decl, 2);
  tree var;

  fprintf (file, "\n\nAlias information for %s\n\n", funcname);

  fprintf (file, "Aliased symbols\n\n");

  FOR_EACH_LOCAL_DECL (cfun, i, var)
    {
      if (may_be_aliased (var))
	dump_variable (file, var);
    }

  fprintf (file, "\nCall clobber information\n");

  fprintf (file, "\nESCAPED");
  dump_points_to_solution (file, &cfun->gimple_df->escaped);

  fprintf (file, "\n\nFlow-insensitive points-to information\n\n");

  FOR_EACH_SSA_NAME (i, ptr, cfun)
    {
      struct ptr_info_def *pi;

      if (!POINTER_TYPE_P (TREE_TYPE (ptr))
	  || SSA_NAME_IN_FREE_LIST (ptr))
	continue;

      pi = SSA_NAME_PTR_INFO (ptr);
      if (pi)
	dump_points_to_info_for (file, ptr);
    }

  fprintf (file, "\n");
}


/* Dump alias information on stderr.  */

DEBUG_FUNCTION void
debug_alias_info (void)
{
  dump_alias_info (stderr);
}


/* Dump the points-to set *PT into FILE.  */

void
dump_points_to_solution (FILE *file, struct pt_solution *pt)
{
  if (pt->anything)
    fprintf (file, ", points-to anything");

  if (pt->nonlocal)
    fprintf (file, ", points-to non-local");

  if (pt->escaped)
    fprintf (file, ", points-to escaped");

  if (pt->ipa_escaped)
    fprintf (file, ", points-to unit escaped");

  if (pt->null)
    fprintf (file, ", points-to NULL");

  if (pt->vars)
    {
      fprintf (file, ", points-to vars: ");
      dump_decl_set (file, pt->vars);
      if (pt->vars_contains_nonlocal
	  || pt->vars_contains_escaped
	  || pt->vars_contains_escaped_heap
	  || pt->vars_contains_restrict)
	{
	  const char *comma = "";
	  fprintf (file, " (");
	  if (pt->vars_contains_nonlocal)
	    {
	      fprintf (file, "nonlocal");
	      comma = ", ";
	    }
	  if (pt->vars_contains_escaped)
	    {
	      fprintf (file, "%sescaped", comma);
	      comma = ", ";
	    }
	  if (pt->vars_contains_escaped_heap)
	    {
	      fprintf (file, "%sescaped heap", comma);
	      comma = ", ";
	    }
	  if (pt->vars_contains_restrict)
	    {
	      fprintf (file, "%srestrict", comma);
	      comma = ", ";
	    }
	  if (pt->vars_contains_interposable)
	    fprintf (file, "%sinterposable", comma);
	  fprintf (file, ")");
	}
    }
}


/* Unified dump function for pt_solution.  */

DEBUG_FUNCTION void
debug (pt_solution &ref)
{
  dump_points_to_solution (stderr, &ref);
}

DEBUG_FUNCTION void
debug (pt_solution *ptr)
{
  if (ptr)
    debug (*ptr);
  else
    fprintf (stderr, "<nil>\n");
}


/* Dump points-to information for SSA_NAME PTR into FILE.  */

void
dump_points_to_info_for (FILE *file, tree ptr)
{
  struct ptr_info_def *pi = SSA_NAME_PTR_INFO (ptr);

  print_generic_expr (file, ptr, dump_flags);

  if (pi)
    dump_points_to_solution (file, &pi->pt);
  else
    fprintf (file, ", points-to anything");

  fprintf (file, "\n");
}


/* Dump points-to information for VAR into stderr.  */

DEBUG_FUNCTION void
debug_points_to_info_for (tree var)
{
  dump_points_to_info_for (stderr, var);
}


/* Initializes the alias-oracle reference representation *R from REF.  */

void
ao_ref_init (ao_ref *r, tree ref)
{
  r->ref = ref;
  r->base = NULL_TREE;
  r->offset = 0;
  r->size = -1;
  r->max_size = -1;
  r->ref_alias_set = -1;
  r->base_alias_set = -1;
  r->volatile_p = ref ? TREE_THIS_VOLATILE (ref) : false;
}

/* Returns the base object of the memory reference *REF.  */

tree
ao_ref_base (ao_ref *ref)
{
  bool reverse;

  if (ref->base)
    return ref->base;
  ref->base = get_ref_base_and_extent (ref->ref, &ref->offset, &ref->size,
				       &ref->max_size, &reverse);
  return ref->base;
}

/* Returns the base object alias set of the memory reference *REF.  */

alias_set_type
ao_ref_base_alias_set (ao_ref *ref)
{
  tree base_ref;
  if (ref->base_alias_set != -1)
    return ref->base_alias_set;
  if (!ref->ref)
    return 0;
  base_ref = ref->ref;
  while (handled_component_p (base_ref))
    base_ref = TREE_OPERAND (base_ref, 0);
  ref->base_alias_set = get_alias_set (base_ref);
  return ref->base_alias_set;
}

/* Returns the reference alias set of the memory reference *REF.  */

alias_set_type
ao_ref_alias_set (ao_ref *ref)
{
  if (ref->ref_alias_set != -1)
    return ref->ref_alias_set;
  ref->ref_alias_set = get_alias_set (ref->ref);
  return ref->ref_alias_set;
}

/* Init an alias-oracle reference representation from a gimple pointer
   PTR and a gimple size SIZE in bytes.  If SIZE is NULL_TREE then the
   size is assumed to be unknown.  The access is assumed to be only
   to or after of the pointer target, not before it.  */

void
ao_ref_init_from_ptr_and_size (ao_ref *ref, tree ptr, tree size)
{
  poly_int64 t, size_hwi, extra_offset = 0;
  ref->ref = NULL_TREE;
  if (TREE_CODE (ptr) == SSA_NAME)
    {
      gimple *stmt = SSA_NAME_DEF_STMT (ptr);
      if (gimple_assign_single_p (stmt)
	  && gimple_assign_rhs_code (stmt) == ADDR_EXPR)
	ptr = gimple_assign_rhs1 (stmt);
      else if (is_gimple_assign (stmt)
	       && gimple_assign_rhs_code (stmt) == POINTER_PLUS_EXPR
	       && ptrdiff_tree_p (gimple_assign_rhs2 (stmt), &extra_offset))
	{
	  ptr = gimple_assign_rhs1 (stmt);
	  extra_offset *= BITS_PER_UNIT;
	}
    }

  if (TREE_CODE (ptr) == ADDR_EXPR)
    {
      ref->base = get_addr_base_and_unit_offset (TREE_OPERAND (ptr, 0), &t);
      if (ref->base)
	ref->offset = BITS_PER_UNIT * t;
      else
	{
	  size = NULL_TREE;
	  ref->offset = 0;
	  ref->base = get_base_address (TREE_OPERAND (ptr, 0));
	}
    }
  else
    {
      gcc_assert (POINTER_TYPE_P (TREE_TYPE (ptr)));
      ref->base = build2 (MEM_REF, char_type_node,
			  ptr, null_pointer_node);
      ref->offset = 0;
    }
  ref->offset += extra_offset;
  if (size
      && poly_int_tree_p (size, &size_hwi)
      && coeffs_in_range_p (size_hwi, 0, HOST_WIDE_INT_MAX / BITS_PER_UNIT))
    ref->max_size = ref->size = size_hwi * BITS_PER_UNIT;
  else
    ref->max_size = ref->size = -1;
  ref->ref_alias_set = 0;
  ref->base_alias_set = 0;
  ref->volatile_p = false;
}

/* S1 and S2 are TYPE_SIZE or DECL_SIZE.  Compare them:
   Return -1 if S1 < S2
   Return 1 if S1 > S2
   Return 0 if equal or incomparable.  */

static int
compare_sizes (tree s1, tree s2)
{
  if (!s1 || !s2)
    return 0;

  poly_uint64 size1;
  poly_uint64 size2;

  if (!poly_int_tree_p (s1, &size1) || !poly_int_tree_p (s2, &size2))
    return 0;
  if (known_lt (size1, size2))
    return -1;
  if (known_lt (size2, size1))
    return 1;
  return 0;
}

/* Compare TYPE1 and TYPE2 by its size.
   Return -1 if size of TYPE1 < size of TYPE2
   Return 1 if size of TYPE1 > size of TYPE2
   Return 0 if types are of equal sizes or we can not compare them.  */

static int
compare_type_sizes (tree type1, tree type2)
{
  /* Be conservative for arrays and vectors.  We want to support partial
     overlap on int[3] and int[3] as tested in gcc.dg/torture/alias-2.c.  */
  while (TREE_CODE (type1) == ARRAY_TYPE
	 || TREE_CODE (type1) == VECTOR_TYPE)
    type1 = TREE_TYPE (type1);
  while (TREE_CODE (type2) == ARRAY_TYPE
	 || TREE_CODE (type2) == VECTOR_TYPE)
    type2 = TREE_TYPE (type2);
  return compare_sizes (TYPE_SIZE (type1), TYPE_SIZE (type2));
}

/* Return 1 if TYPE1 and TYPE2 are to be considered equivalent for the
   purpose of TBAA.  Return 0 if they are distinct and -1 if we cannot
   decide.  */

static inline int
same_type_for_tbaa (tree type1, tree type2)
{
  type1 = TYPE_MAIN_VARIANT (type1);
  type2 = TYPE_MAIN_VARIANT (type2);

  /* Handle the most common case first.  */
  if (type1 == type2)
    return 1;

  /* If we would have to do structural comparison bail out.  */
  if (TYPE_STRUCTURAL_EQUALITY_P (type1)
      || TYPE_STRUCTURAL_EQUALITY_P (type2))
    return -1;

  /* Compare the canonical types.  */
  if (TYPE_CANONICAL (type1) == TYPE_CANONICAL (type2))
    return 1;

  /* ??? Array types are not properly unified in all cases as we have
     spurious changes in the index types for example.  Removing this
     causes all sorts of problems with the Fortran frontend.  */
  if (TREE_CODE (type1) == ARRAY_TYPE
      && TREE_CODE (type2) == ARRAY_TYPE)
    return -1;

  /* ??? In Ada, an lvalue of an unconstrained type can be used to access an
     object of one of its constrained subtypes, e.g. when a function with an
     unconstrained parameter passed by reference is called on an object and
     inlined.  But, even in the case of a fixed size, type and subtypes are
     not equivalent enough as to share the same TYPE_CANONICAL, since this
     would mean that conversions between them are useless, whereas they are
     not (e.g. type and subtypes can have different modes).  So, in the end,
     they are only guaranteed to have the same alias set.  */
  if (get_alias_set (type1) == get_alias_set (type2))
    return -1;

  /* The types are known to be not equal.  */
  return 0;
}

/* Return true if TYPE is a composite type (i.e. we may apply one of handled
   components on it).  */

static bool
type_has_components_p (tree type)
{
  return AGGREGATE_TYPE_P (type) || VECTOR_TYPE_P (type)
	 || TREE_CODE (type) == COMPLEX_TYPE;
}

/* MATCH1 and MATCH2 which are part of access path of REF1 and REF2
   respectively are either pointing to same address or are completely
   disjoint. If PARITAL_OVERLAP is true, assume that outermost arrays may
   just partly overlap.

   Try to disambiguate using the access path starting from the match
   and return false if there is no conflict.

   Helper for aliasing_component_refs_p.  */

static bool
aliasing_matching_component_refs_p (tree match1, tree ref1,
				    poly_int64 offset1, poly_int64 max_size1,
				    tree match2, tree ref2,
				    poly_int64 offset2, poly_int64 max_size2,
				    bool partial_overlap)
{
  poly_int64 offadj, sztmp, msztmp;
  bool reverse;

  if (!partial_overlap)
    {
      get_ref_base_and_extent (match2, &offadj, &sztmp, &msztmp, &reverse);
      offset2 -= offadj;
      get_ref_base_and_extent (match1, &offadj, &sztmp, &msztmp, &reverse);
      offset1 -= offadj;
      if (!ranges_maybe_overlap_p (offset1, max_size1, offset2, max_size2))
	{
	  ++alias_stats.aliasing_component_refs_p_no_alias;
	  return false;
	}
    }

  int cmp = nonoverlapping_refs_since_match_p (match1, ref1, match2, ref2,
					       partial_overlap);
  if (cmp == 1
      || (cmp == -1 && nonoverlapping_component_refs_p (ref1, ref2)))
    {
      ++alias_stats.aliasing_component_refs_p_no_alias;
      return false;
    }
  ++alias_stats.aliasing_component_refs_p_may_alias;
  return true;
}

/* Return true if REF is reference to zero sized trailing array. I.e.
   struct foo {int bar; int array[0];} *fooptr;
   fooptr->array.  */

static bool
component_ref_to_zero_sized_trailing_array_p (tree ref)
{
  return (TREE_CODE (ref) == COMPONENT_REF
	  && TREE_CODE (TREE_TYPE (TREE_OPERAND (ref, 1))) == ARRAY_TYPE
	  && (!TYPE_SIZE (TREE_TYPE (TREE_OPERAND (ref, 1)))
	      || integer_zerop (TYPE_SIZE (TREE_TYPE (TREE_OPERAND (ref, 1)))))
	  && array_at_struct_end_p (ref));
}

/* Worker for aliasing_component_refs_p. Most parameters match parameters of
   aliasing_component_refs_p.

   Walk access path REF2 and try to find type matching TYPE1
   (which is a start of possibly aliasing access path REF1).
   If match is found, try to disambiguate.

   Return 0 for sucessful disambiguation.
   Return 1 if match was found but disambiguation failed
   Return -1 if there is no match.
   In this case MAYBE_MATCH is set to 0 if there is no type matching TYPE1
   in access patch REF2 and -1 if we are not sure.  */

static int
aliasing_component_refs_walk (tree ref1, tree type1, tree base1,
			      poly_int64 offset1, poly_int64 max_size1,
			      tree end_struct_ref1,
			      tree ref2, tree base2,
			      poly_int64 offset2, poly_int64 max_size2,
			      bool *maybe_match)
{
  tree ref = ref2;
  int same_p = 0;

  while (true)
    {
      /* We walk from inner type to the outer types. If type we see is
	 already too large to be part of type1, terminate the search.  */
      int cmp = compare_type_sizes (type1, TREE_TYPE (ref));

      if (cmp < 0
	  && (!end_struct_ref1
	      || compare_type_sizes (TREE_TYPE (end_struct_ref1),
				     TREE_TYPE (ref)) < 0))
	break;
      /* If types may be of same size, see if we can decide about their
	 equality.  */
      if (cmp == 0)
	{
	  same_p = same_type_for_tbaa (TREE_TYPE (ref), type1);
	  if (same_p == 1)
	    break;
	  /* In case we can't decide whether types are same try to
	     continue looking for the exact match.
	     Remember however that we possibly saw a match
	     to bypass the access path continuations tests we do later.  */
	  if (same_p == -1)
	    *maybe_match = true;
	}
      if (!handled_component_p (ref))
	break;
      ref = TREE_OPERAND (ref, 0);
    }
  if (same_p == 1)
    {
      bool partial_overlap = false;

      /* We assume that arrays can overlap by multiple of their elements
	 size as tested in gcc.dg/torture/alias-2.c.
	 This partial overlap happen only when both arrays are bases of
	 the access and not contained within another component ref.
	 To be safe we also assume partial overlap for VLAs. */
      if (TREE_CODE (TREE_TYPE (base1)) == ARRAY_TYPE
	  && (!TYPE_SIZE (TREE_TYPE (base1))
	      || TREE_CODE (TYPE_SIZE (TREE_TYPE (base1))) != INTEGER_CST
	      || ref == base2))
	{
	  /* Setting maybe_match to true triggers
	     nonoverlapping_component_refs_p test later that still may do
	     useful disambiguation.  */
	  *maybe_match = true;
	  partial_overlap = true;
	}
      return aliasing_matching_component_refs_p (base1, ref1,
						 offset1, max_size1,
						 ref, ref2,
						 offset2, max_size2,
						 partial_overlap);
    }
  return -1;
}

/* Determine if the two component references REF1 and REF2 which are
   based on access types TYPE1 and TYPE2 and of which at least one is based
   on an indirect reference may alias.  
   REF1_ALIAS_SET, BASE1_ALIAS_SET, REF2_ALIAS_SET and BASE2_ALIAS_SET
   are the respective alias sets.  */

static bool
aliasing_component_refs_p (tree ref1,
			   alias_set_type ref1_alias_set,
			   alias_set_type base1_alias_set,
			   poly_int64 offset1, poly_int64 max_size1,
			   tree ref2,
			   alias_set_type ref2_alias_set,
			   alias_set_type base2_alias_set,
			   poly_int64 offset2, poly_int64 max_size2)
{
  /* If one reference is a component references through pointers try to find a
     common base and apply offset based disambiguation.  This handles
     for example
       struct A { int i; int j; } *q;
       struct B { struct A a; int k; } *p;
     disambiguating q->i and p->a.j.  */
  tree base1, base2;
  tree type1, type2;
  bool maybe_match = false;
  tree end_struct_ref1 = NULL, end_struct_ref2 = NULL;

  /* Choose bases and base types to search for.  */
  base1 = ref1;
  while (handled_component_p (base1))
    {
      /* Generally access paths are monotous in the size of object. The
	 exception are trailing arrays of structures. I.e.
	   struct a {int array[0];};
	 or
	   struct a {int array1[0]; int array[];};
	 Such struct has size 0 but accesses to a.array may have non-zero size.
	 In this case the size of TREE_TYPE (base1) is smaller than
	 size of TREE_TYPE (TREE_OPERNAD (base1, 0)).

	 Because we compare sizes of arrays just by sizes of their elements,
	 we only need to care about zero sized array fields here.  */
      if (component_ref_to_zero_sized_trailing_array_p (base1))
	{
	  gcc_checking_assert (!end_struct_ref1);
          end_struct_ref1 = base1;
	}
      if (TREE_CODE (base1) == VIEW_CONVERT_EXPR
	  || TREE_CODE (base1) == BIT_FIELD_REF)
	ref1 = TREE_OPERAND (base1, 0);
      base1 = TREE_OPERAND (base1, 0);
    }
  type1 = TREE_TYPE (base1);
  base2 = ref2;
  while (handled_component_p (base2))
    {
      if (component_ref_to_zero_sized_trailing_array_p (base2))
	{
	  gcc_checking_assert (!end_struct_ref2);
	  end_struct_ref2 = base2;
	}
      if (TREE_CODE (base2) == VIEW_CONVERT_EXPR
	  || TREE_CODE (base2) == BIT_FIELD_REF)
	ref2 = TREE_OPERAND (base2, 0);
      base2 = TREE_OPERAND (base2, 0);
    }
  type2 = TREE_TYPE (base2);

  /* Now search for the type1 in the access path of ref2.  This
     would be a common base for doing offset based disambiguation on.
     This however only makes sense if type2 is big enough to hold type1.  */
  int cmp_outer = compare_type_sizes (type2, type1);

  /* If type2 is big enough to contain type1 walk its access path.
     We also need to care of arrays at the end of structs that may extend
     beyond the end of structure.  */
  if (cmp_outer >= 0
      || (end_struct_ref2
	  && compare_type_sizes (TREE_TYPE (end_struct_ref2), type1) >= 0))
    {
      int res = aliasing_component_refs_walk (ref1, type1, base1,
					      offset1, max_size1,
					      end_struct_ref1,
					      ref2, base2, offset2, max_size2,
					      &maybe_match);
      if (res != -1)
	return res;
    }

  /* If we didn't find a common base, try the other way around.  */
  if (cmp_outer <= 0 
      || (end_struct_ref1
	  && compare_type_sizes (TREE_TYPE (end_struct_ref1), type1) <= 0))
    {
      int res = aliasing_component_refs_walk (ref2, type2, base2,
					      offset2, max_size2,
					      end_struct_ref2,
					      ref1, base1, offset1, max_size1,
					      &maybe_match);
      if (res != -1)
	return res;
    }

  /* In the following code we make an assumption that the types in access
     paths do not overlap and thus accesses alias only if one path can be
     continuation of another.  If we was not able to decide about equivalence,
     we need to give up.  */
  if (maybe_match)
    {
      if (!nonoverlapping_component_refs_p (ref1, ref2))
	{
	  ++alias_stats.aliasing_component_refs_p_may_alias;
	  return true;
	}
      ++alias_stats.aliasing_component_refs_p_no_alias;
      return false;
    }

  /* If we have two type access paths B1.path1 and B2.path2 they may
     only alias if either B1 is in B2.path2 or B2 is in B1.path1.
     But we can still have a path that goes B1.path1...B2.path2 with
     a part that we do not see.  So we can only disambiguate now
     if there is no B2 in the tail of path1 and no B1 on the
     tail of path2.  */
  if (compare_type_sizes (TREE_TYPE (ref2), type1) >= 0
      && (!end_struct_ref1
	  || compare_type_sizes (TREE_TYPE (ref2),
		 		 TREE_TYPE (end_struct_ref1)) >= 0)
      && type_has_components_p (TREE_TYPE (ref2))
      && (base1_alias_set == ref2_alias_set
          || alias_set_subset_of (base1_alias_set, ref2_alias_set)))
    {
      ++alias_stats.aliasing_component_refs_p_may_alias;
      return true;
    }
  /* If this is ptr vs. decl then we know there is no ptr ... decl path.  */
  if (compare_type_sizes (TREE_TYPE (ref1), type2) >= 0
      && (!end_struct_ref2
	  || compare_type_sizes (TREE_TYPE (ref1),
		 		 TREE_TYPE (end_struct_ref2)) >= 0)
      && type_has_components_p (TREE_TYPE (ref1))
      && (base2_alias_set == ref1_alias_set
	  || alias_set_subset_of (base2_alias_set, ref1_alias_set)))
    {
      ++alias_stats.aliasing_component_refs_p_may_alias;
      return true;
    }
  ++alias_stats.aliasing_component_refs_p_no_alias;
  return false;
}

/* FIELD1 and FIELD2 are two fields of component refs.  We assume
   that bases of both component refs are either equivalent or nonoverlapping.
   We do not assume that the containers of FIELD1 and FIELD2 are of the
   same type or size.

   Return 0 in case the base address of component_refs are same then 
   FIELD1 and FIELD2 have same address. Note that FIELD1 and FIELD2
   may not be of same type or size.

   Return 1 if FIELD1 and FIELD2 are non-overlapping.

   Return -1 otherwise.

   Main difference between 0 and -1 is to let
   nonoverlapping_component_refs_since_match_p discover the semantically
   equivalent part of the access path.

   Note that this function is used even with -fno-strict-aliasing
   and makes use of no TBAA assumptions.  */

static int
nonoverlapping_component_refs_p_1 (const_tree field1, const_tree field2)
{
  /* If both fields are of the same type, we could save hard work of
     comparing offsets.  */
  tree type1 = DECL_CONTEXT (field1);
  tree type2 = DECL_CONTEXT (field2);

  if (TREE_CODE (type1) == RECORD_TYPE
      && DECL_BIT_FIELD_REPRESENTATIVE (field1))
    field1 = DECL_BIT_FIELD_REPRESENTATIVE (field1);
  if (TREE_CODE (type2) == RECORD_TYPE
      && DECL_BIT_FIELD_REPRESENTATIVE (field2))
    field2 = DECL_BIT_FIELD_REPRESENTATIVE (field2);

  /* ??? Bitfields can overlap at RTL level so punt on them.
     FIXME: RTL expansion should be fixed by adjusting the access path
     when producing MEM_ATTRs for MEMs which are wider than 
     the bitfields similarly as done in set_mem_attrs_minus_bitpos.  */
  if (DECL_BIT_FIELD (field1) && DECL_BIT_FIELD (field2))
    return -1;

  /* Assume that different FIELD_DECLs never overlap within a RECORD_TYPE.  */
  if (type1 == type2 && TREE_CODE (type1) == RECORD_TYPE)
    return field1 != field2;

  /* In common case the offsets and bit offsets will be the same.
     However if frontends do not agree on the alignment, they may be
     different even if they actually represent same address.
     Try the common case first and if that fails calcualte the
     actual bit offset.  */
  if (tree_int_cst_equal (DECL_FIELD_OFFSET (field1),
			  DECL_FIELD_OFFSET (field2))
      && tree_int_cst_equal (DECL_FIELD_BIT_OFFSET (field1),
			     DECL_FIELD_BIT_OFFSET (field2)))
    return 0;

  /* Note that it may be possible to use component_ref_field_offset
     which would provide offsets as trees. However constructing and folding
     trees is expensive and does not seem to be worth the compile time
     cost.  */

  poly_uint64 offset1, offset2;
  poly_uint64 bit_offset1, bit_offset2;

  if (poly_int_tree_p (DECL_FIELD_OFFSET (field1), &offset1)
      && poly_int_tree_p (DECL_FIELD_OFFSET (field2), &offset2)
      && poly_int_tree_p (DECL_FIELD_BIT_OFFSET (field1), &bit_offset1)
      && poly_int_tree_p (DECL_FIELD_BIT_OFFSET (field2), &bit_offset2))
    {
      offset1 = (offset1 << LOG2_BITS_PER_UNIT) + bit_offset1;
      offset2 = (offset2 << LOG2_BITS_PER_UNIT) + bit_offset2;

      if (known_eq (offset1, offset2))
	return 0;

      poly_uint64 size1, size2;

      if (poly_int_tree_p (DECL_SIZE (field1), &size1)
	  && poly_int_tree_p (DECL_SIZE (field2), &size2)
	  && !ranges_maybe_overlap_p (offset1, size1, offset2, size2))
	return 1;
    }
  /* Resort to slower overlap checking by looking for matching types in
     the middle of access path.  */
  return -1;
}

/* Return low bound of array. Do not produce new trees
   and thus do not care about particular type of integer constant
   and placeholder exprs.  */

static tree
cheap_array_ref_low_bound (tree ref)
{
  tree domain_type = TYPE_DOMAIN (TREE_TYPE (TREE_OPERAND (ref, 0)));

  /* Avoid expensive array_ref_low_bound.
     low bound is either stored in operand2, or it is TYPE_MIN_VALUE of domain
     type or it is zero.  */
  if (TREE_OPERAND (ref, 2))
    return TREE_OPERAND (ref, 2);
  else if (domain_type && TYPE_MIN_VALUE (domain_type))
    return TYPE_MIN_VALUE (domain_type);
  else
    return integer_zero_node;
}

/* REF1 and REF2 are ARRAY_REFs with either same base address or which are
   completely disjoint.

   Return 1 if the refs are non-overlapping.
   Return 0 if they are possibly overlapping but if so the overlap again
   starts on the same address.
   Return -1 otherwise.  */

int
nonoverlapping_array_refs_p (tree ref1, tree ref2)
{
  tree index1 = TREE_OPERAND (ref1, 1);
  tree index2 = TREE_OPERAND (ref2, 1);
  tree low_bound1 = cheap_array_ref_low_bound(ref1);
  tree low_bound2 = cheap_array_ref_low_bound(ref2);

  /* Handle zero offsets first: we do not need to match type size in this
     case.  */
  if (operand_equal_p (index1, low_bound1, 0)
      && operand_equal_p (index2, low_bound2, 0))
    return 0;

  /* If type sizes are different, give up.

     Avoid expensive array_ref_element_size.
     If operand 3 is present it denotes size in the alignmnet units.
     Otherwise size is TYPE_SIZE of the element type.
     Handle only common cases where types are of the same "kind".  */
  if ((TREE_OPERAND (ref1, 3) == NULL) != (TREE_OPERAND (ref2, 3) == NULL))
    return -1;

  tree elmt_type1 = TREE_TYPE (TREE_TYPE (TREE_OPERAND (ref1, 0)));
  tree elmt_type2 = TREE_TYPE (TREE_TYPE (TREE_OPERAND (ref2, 0)));

  if (TREE_OPERAND (ref1, 3))
    {
      if (TYPE_ALIGN (elmt_type1) != TYPE_ALIGN (elmt_type2)
	  || !operand_equal_p (TREE_OPERAND (ref1, 3),
			       TREE_OPERAND (ref2, 3), 0))
	return -1;
    }
  else
    {
      if (!operand_equal_p (TYPE_SIZE_UNIT (elmt_type1),
			    TYPE_SIZE_UNIT (elmt_type2), 0))
	return -1;
    }

  /* Since we know that type sizes are the same, there is no need to return
     -1 after this point. Partial overlap can not be introduced.  */

  /* We may need to fold trees in this case.
     TODO: Handle integer constant case at least.  */
  if (!operand_equal_p (low_bound1, low_bound2, 0))
    return 0;

  if (TREE_CODE (index1) == INTEGER_CST && TREE_CODE (index2) == INTEGER_CST)
    {
      if (tree_int_cst_equal (index1, index2))
	return 0;
      return 1;
    }
  /* TODO: We can use VRP to further disambiguate here. */
  return 0;
}

/* Try to disambiguate REF1 and REF2 under the assumption that MATCH1 and
   MATCH2 either point to the same address or are disjoint.
   MATCH1 and MATCH2 are assumed to be ref in the access path of REF1 and REF2
   respectively or NULL in the case we established equivalence of bases.
   If PARTIAL_OVERLAP is true assume that the toplevel arrays may actually
   overlap by exact multiply of their element size.

   This test works by matching the initial segment of the access path
   and does not rely on TBAA thus is safe for !flag_strict_aliasing if
   match was determined without use of TBAA oracle.

   Return 1 if we can determine that component references REF1 and REF2,
   that are within a common DECL, cannot overlap.

   Return 0 if paths are same and thus there is nothing to disambiguate more
   (i.e. there is must alias assuming there is must alias between MATCH1 and
   MATCH2)

   Return -1 if we can not determine 0 or 1 - this happens when we met
   non-matching types was met in the path.
   In this case it may make sense to continue by other disambiguation
   oracles.  */

static int
nonoverlapping_refs_since_match_p (tree match1, tree ref1,
				   tree match2, tree ref2,
				   bool partial_overlap)
{
  /* Early return if there are no references to match, we do not need
     to walk the access paths.

     Do not consider this as may-alias for stats - it is more useful
     to have information how many disambiguations happened provided that
     the query was meaningful.  */

  if (match1 == ref1 || !handled_component_p (ref1)
      || match2 == ref2 || !handled_component_p (ref2))
    return -1;

  auto_vec<tree, 16> component_refs1;
  auto_vec<tree, 16> component_refs2;

  /* Create the stack of handled components for REF1.  */
  while (handled_component_p (ref1) && ref1 != match1)
    {
      if (TREE_CODE (ref1) == VIEW_CONVERT_EXPR
	  || TREE_CODE (ref1) == BIT_FIELD_REF)
	component_refs1.truncate (0);
      else
        component_refs1.safe_push (ref1);
      ref1 = TREE_OPERAND (ref1, 0);
    }

  /* Create the stack of handled components for REF2.  */
  while (handled_component_p (ref2) && ref2 != match2)
    {
      if (TREE_CODE (ref2) == VIEW_CONVERT_EXPR
	  || TREE_CODE (ref2) == BIT_FIELD_REF)
	component_refs2.truncate (0);
      else
        component_refs2.safe_push (ref2);
      ref2 = TREE_OPERAND (ref2, 0);
    }

  bool mem_ref1 = TREE_CODE (ref1) == MEM_REF && ref1 != match1;
  bool mem_ref2 = TREE_CODE (ref2) == MEM_REF && ref2 != match2;

  /* If only one of access path starts with MEM_REF check that offset is 0
     so the addresses stays the same after stripping it.
     TODO: In this case we may walk the other access path until we get same
     offset.

     If both starts with MEM_REF, offset has to be same.  */
  if ((mem_ref1 && !mem_ref2 && !integer_zerop (TREE_OPERAND (ref1, 1)))
      || (mem_ref2 && !mem_ref1 && !integer_zerop (TREE_OPERAND (ref2, 1)))
      || (mem_ref1 && mem_ref2
	  && !tree_int_cst_equal (TREE_OPERAND (ref1, 1),
				  TREE_OPERAND (ref2, 1))))
    {
      ++alias_stats.nonoverlapping_refs_since_match_p_may_alias;
      return -1;
    }

  /* TARGET_MEM_REF are never wrapped in handled components, so we do not need
     to handle them here at all.  */
  gcc_checking_assert (TREE_CODE (ref1) != TARGET_MEM_REF
		       && TREE_CODE (ref2) != TARGET_MEM_REF);

  /* Pop the stacks in parallel and examine the COMPONENT_REFs of the same
     rank.  This is sufficient because we start from the same DECL and you
     cannot reference several fields at a time with COMPONENT_REFs (unlike
     with ARRAY_RANGE_REFs for arrays) so you always need the same number
     of them to access a sub-component, unless you're in a union, in which
     case the return value will precisely be false.  */
  while (true)
    {
      /* Track if we seen unmatched ref with non-zero offset.  In this case
	 we must look for partial overlaps.  */
      bool seen_unmatched_ref_p = false;

      /* First match ARRAY_REFs an try to disambiguate.  */
      if (!component_refs1.is_empty ()
	  && !component_refs2.is_empty ())
	{
	  unsigned int narray_refs1=0, narray_refs2=0;

	  /* We generally assume that both access paths starts by same sequence
	     of refs.  However if number of array refs is not in sync, try
	     to recover and pop elts until number match.  This helps the case
	     where one access path starts by array and other by element.  */
	  for (narray_refs1 = 0; narray_refs1 < component_refs1.length ();
	       narray_refs1++)
	    if (TREE_CODE (component_refs1 [component_refs1.length()
					    - 1 - narray_refs1]) != ARRAY_REF)
	      break;

	  for (narray_refs2 = 0; narray_refs2 < component_refs2.length ();
	       narray_refs2++)
	    if (TREE_CODE (component_refs2 [component_refs2.length()
					    - 1 - narray_refs2]) != ARRAY_REF)
	      break;
	  for (; narray_refs1 > narray_refs2; narray_refs1--)
	    {
	      ref1 = component_refs1.pop ();

	      /* If index is non-zero we need to check whether the reference
		 does not break the main invariant that bases are either
		 disjoint or equal.  Consider the example:

		 unsigned char out[][1];
		 out[1]="a";
		 out[i][0];

		 Here bases out and out are same, but after removing the
		 [i] index, this invariant no longer holds, because
		 out[i] points to the middle of array out.

		 TODO: If size of type of the skipped reference is an integer
		 multiply of the size of type of the other reference this
		 invariant can be verified, but even then it is not completely
		 safe with !flag_strict_aliasing if the other reference contains
		 unbounded array accesses.
		 See   */

	      if (!operand_equal_p (TREE_OPERAND (ref1, 1),
				    cheap_array_ref_low_bound (ref1), 0))
		return 0;
	    }
	  for (; narray_refs2 > narray_refs1; narray_refs2--)
	    {
	      ref2 = component_refs2.pop ();
	      if (!operand_equal_p (TREE_OPERAND (ref2, 1),
				    cheap_array_ref_low_bound (ref2), 0))
		return 0;
	    }
	  /* Try to disambiguate matched arrays.  */
	  for (unsigned int i = 0; i < narray_refs1; i++)
	    {
	      int cmp = nonoverlapping_array_refs_p (component_refs1.pop (),
						     component_refs2.pop ());
	      if (cmp == 1 && !partial_overlap)
		{
		  ++alias_stats
		    .nonoverlapping_refs_since_match_p_no_alias;
		  return 1;
		}
	      partial_overlap = false;
	      if (cmp == -1)
		seen_unmatched_ref_p = true;
	    }
	}

      /* Next look for component_refs.  */
      do
	{
	  if (component_refs1.is_empty ())
	    {
	      ++alias_stats
		.nonoverlapping_refs_since_match_p_must_overlap;
	      return 0;
	    }
	  ref1 = component_refs1.pop ();
	  if (TREE_CODE (ref1) != COMPONENT_REF)
	    seen_unmatched_ref_p = true;
	}
      while (!RECORD_OR_UNION_TYPE_P (TREE_TYPE (TREE_OPERAND (ref1, 0))));

      do
	{
	  if (component_refs2.is_empty ())
	    {
	      ++alias_stats
		.nonoverlapping_refs_since_match_p_must_overlap;
	      return 0;
	    }
	  ref2 = component_refs2.pop ();
	  if (TREE_CODE (ref2) != COMPONENT_REF)
	    seen_unmatched_ref_p = true;
	}
      while (!RECORD_OR_UNION_TYPE_P (TREE_TYPE (TREE_OPERAND (ref2, 0))));

      /* BIT_FIELD_REF and VIEW_CONVERT_EXPR are taken off the vectors
	 earlier.  */
      gcc_checking_assert (TREE_CODE (ref1) == COMPONENT_REF
			   && TREE_CODE (ref2) == COMPONENT_REF);

      tree field1 = TREE_OPERAND (ref1, 1);
      tree field2 = TREE_OPERAND (ref2, 1);

      /* ??? We cannot simply use the type of operand #0 of the refs here
	 as the Fortran compiler smuggles type punning into COMPONENT_REFs
	 for common blocks instead of using unions like everyone else.  */
      tree type1 = DECL_CONTEXT (field1);
      tree type2 = DECL_CONTEXT (field2);

      partial_overlap = false;

      /* If we skipped array refs on type of different sizes, we can
	 no longer be sure that there are not partial overlaps.  */
      if (seen_unmatched_ref_p
	  && !operand_equal_p (TYPE_SIZE (type1), TYPE_SIZE (type2), 0))
	{
	  ++alias_stats
	    .nonoverlapping_refs_since_match_p_may_alias;
	  return -1;
	}

      int cmp = nonoverlapping_component_refs_p_1 (field1, field2);
      if (cmp == -1)
	{
	  ++alias_stats
	    .nonoverlapping_refs_since_match_p_may_alias;
	  return -1;
	}
      else if (cmp == 1)
	{
	  ++alias_stats
	    .nonoverlapping_refs_since_match_p_no_alias;
	  return 1;
	}
    }

  ++alias_stats.nonoverlapping_refs_since_match_p_must_overlap;
  return 0;
}

/* Return TYPE_UID which can be used to match record types we consider
   same for TBAA purposes.  */

static inline int
ncr_type_uid (const_tree field)
{
  /* ??? We cannot simply use the type of operand #0 of the refs here
     as the Fortran compiler smuggles type punning into COMPONENT_REFs
     for common blocks instead of using unions like everyone else.  */
  tree type = DECL_FIELD_CONTEXT (field);
  /* With LTO types considered same_type_for_tbaa_p 
     from different translation unit may not have same
     main variant.  They however have same TYPE_CANONICAL.  */
  if (TYPE_CANONICAL (type))
    return TYPE_UID (TYPE_CANONICAL (type));
  return TYPE_UID (type);
}

/* qsort compare function to sort FIELD_DECLs after their
   DECL_FIELD_CONTEXT TYPE_UID.  */

static inline int
ncr_compar (const void *field1_, const void *field2_)
{
  const_tree field1 = *(const_tree *) const_cast <void *>(field1_);
  const_tree field2 = *(const_tree *) const_cast <void *>(field2_);
  unsigned int uid1 = ncr_type_uid (field1);
  unsigned int uid2 = ncr_type_uid (field2);

  if (uid1 < uid2)
    return -1;
  else if (uid1 > uid2)
    return 1;
  return 0;
}

/* Return true if we can determine that the fields referenced cannot
   overlap for any pair of objects.  This relies on TBAA.  */

static bool
nonoverlapping_component_refs_p (const_tree x, const_tree y)
{
  /* Early return if we have nothing to do.

     Do not consider this as may-alias for stats - it is more useful
     to have information how many disambiguations happened provided that
     the query was meaningful.  */
  if (!flag_strict_aliasing
      || !x || !y
      || !handled_component_p (x)
      || !handled_component_p (y))
    return false;

  auto_vec<const_tree, 16> fieldsx;
  while (handled_component_p (x))
    {
      if (TREE_CODE (x) == COMPONENT_REF)
	{
	  tree field = TREE_OPERAND (x, 1);
	  tree type = DECL_FIELD_CONTEXT (field);
	  if (TREE_CODE (type) == RECORD_TYPE)
	    fieldsx.safe_push (field);
	}
      else if (TREE_CODE (x) == VIEW_CONVERT_EXPR
	       || TREE_CODE (x) == BIT_FIELD_REF)
	fieldsx.truncate (0);
      x = TREE_OPERAND (x, 0);
    }
  if (fieldsx.length () == 0)
    return false;
  auto_vec<const_tree, 16> fieldsy;
  while (handled_component_p (y))
    {
      if (TREE_CODE (y) == COMPONENT_REF)
	{
	  tree field = TREE_OPERAND (y, 1);
	  tree type = DECL_FIELD_CONTEXT (field);
	  if (TREE_CODE (type) == RECORD_TYPE)
	    fieldsy.safe_push (TREE_OPERAND (y, 1));
	}
      else if (TREE_CODE (y) == VIEW_CONVERT_EXPR
	       || TREE_CODE (y) == BIT_FIELD_REF)
	fieldsy.truncate (0);
      y = TREE_OPERAND (y, 0);
    }
  if (fieldsy.length () == 0)
    {
      ++alias_stats.nonoverlapping_component_refs_p_may_alias;
      return false;
    }

  /* Most common case first.  */
  if (fieldsx.length () == 1
      && fieldsy.length () == 1)
   {
     if (same_type_for_tbaa (DECL_FIELD_CONTEXT (fieldsx[0]),
			     DECL_FIELD_CONTEXT (fieldsy[0])) == 1
	 && nonoverlapping_component_refs_p_1 (fieldsx[0], fieldsy[0]) == 1)
      {
         ++alias_stats.nonoverlapping_component_refs_p_no_alias;
         return true;
      }
     else
      {
         ++alias_stats.nonoverlapping_component_refs_p_may_alias;
         return false;
      }
   }

  if (fieldsx.length () == 2)
    {
      if (ncr_compar (&fieldsx[0], &fieldsx[1]) == 1)
	std::swap (fieldsx[0], fieldsx[1]);
    }
  else
    fieldsx.qsort (ncr_compar);

  if (fieldsy.length () == 2)
    {
      if (ncr_compar (&fieldsy[0], &fieldsy[1]) == 1)
	std::swap (fieldsy[0], fieldsy[1]);
    }
  else
    fieldsy.qsort (ncr_compar);

  unsigned i = 0, j = 0;
  do
    {
      const_tree fieldx = fieldsx[i];
      const_tree fieldy = fieldsy[j];

      /* We're left with accessing different fields of a structure,
	 no possible overlap.  */
      if (same_type_for_tbaa (DECL_FIELD_CONTEXT (fieldx),
			      DECL_FIELD_CONTEXT (fieldy)) == 1
	  && nonoverlapping_component_refs_p_1 (fieldx, fieldy) == 1)
	{
	  ++alias_stats.nonoverlapping_component_refs_p_no_alias;
	  return true;
	}

      if (ncr_type_uid (fieldx) < ncr_type_uid (fieldy))
	{
	  i++;
	  if (i == fieldsx.length ())
	    break;
	}
      else
	{
	  j++;
	  if (j == fieldsy.length ())
	    break;
	}
    }
  while (1);

  ++alias_stats.nonoverlapping_component_refs_p_may_alias;
  return false;
}


/* Return true if two memory references based on the variables BASE1
   and BASE2 constrained to [OFFSET1, OFFSET1 + MAX_SIZE1) and
   [OFFSET2, OFFSET2 + MAX_SIZE2) may alias.  REF1 and REF2
   if non-NULL are the complete memory reference trees.  */

static bool
decl_refs_may_alias_p (tree ref1, tree base1,
		       poly_int64 offset1, poly_int64 max_size1,
		       poly_int64 size1,
		       tree ref2, tree base2,
		       poly_int64 offset2, poly_int64 max_size2,
		       poly_int64 size2)
{
  gcc_checking_assert (DECL_P (base1) && DECL_P (base2));

  /* If both references are based on different variables, they cannot alias.  */
  if (compare_base_decls (base1, base2) == 0)
    return false;

  /* If both references are based on the same variable, they cannot alias if
     the accesses do not overlap.  */
  if (!ranges_maybe_overlap_p (offset1, max_size1, offset2, max_size2))
    return false;

  /* If there is must alias, there is no use disambiguating further.  */
  if (known_eq (size1, max_size1) && known_eq (size2, max_size2))
    return true;

  /* For components with variable position, the above test isn't sufficient,
     so we disambiguate component references manually.  */
  if (ref1 && ref2
      && handled_component_p (ref1) && handled_component_p (ref2)
      && nonoverlapping_refs_since_match_p (NULL, ref1, NULL, ref2, false) == 1)
    return false;

  return true;     
}

/* Return true if an indirect reference based on *PTR1 constrained
   to [OFFSET1, OFFSET1 + MAX_SIZE1) may alias a variable based on BASE2
   constrained to [OFFSET2, OFFSET2 + MAX_SIZE2).  *PTR1 and BASE2 have
   the alias sets BASE1_ALIAS_SET and BASE2_ALIAS_SET which can be -1
   in which case they are computed on-demand.  REF1 and REF2
   if non-NULL are the complete memory reference trees.  */

static bool
indirect_ref_may_alias_decl_p (tree ref1 ATTRIBUTE_UNUSED, tree base1,
			       poly_int64 offset1, poly_int64 max_size1,
			       poly_int64 size1,
			       alias_set_type ref1_alias_set,
			       alias_set_type base1_alias_set,
			       tree ref2 ATTRIBUTE_UNUSED, tree base2,
			       poly_int64 offset2, poly_int64 max_size2,
			       poly_int64 size2,
			       alias_set_type ref2_alias_set,
			       alias_set_type base2_alias_set, bool tbaa_p)
{
  tree ptr1;
  tree ptrtype1, dbase2;

  gcc_checking_assert ((TREE_CODE (base1) == MEM_REF
			|| TREE_CODE (base1) == TARGET_MEM_REF)
		       && DECL_P (base2));

  ptr1 = TREE_OPERAND (base1, 0);
  poly_offset_int moff = mem_ref_offset (base1) << LOG2_BITS_PER_UNIT;

  /* If only one reference is based on a variable, they cannot alias if
     the pointer access is beyond the extent of the variable access.
     (the pointer base cannot validly point to an offset less than zero
     of the variable).
     ???  IVOPTs creates bases that do not honor this restriction,
     so do not apply this optimization for TARGET_MEM_REFs.  */
  if (TREE_CODE (base1) != TARGET_MEM_REF
      && !ranges_maybe_overlap_p (offset1 + moff, -1, offset2, max_size2))
    return false;
  /* They also cannot alias if the pointer may not point to the decl.  */
  if (!ptr_deref_may_alias_decl_p (ptr1, base2))
    return false;

  /* Disambiguations that rely on strict aliasing rules follow.  */
  if (!flag_strict_aliasing || !tbaa_p)
    return true;

  /* If the alias set for a pointer access is zero all bets are off.  */
  if (base1_alias_set == 0 || base2_alias_set == 0)
    return true;

  /* When we are trying to disambiguate an access with a pointer dereference
     as base versus one with a decl as base we can use both the size
     of the decl and its dynamic type for extra disambiguation.
     ???  We do not know anything about the dynamic type of the decl
     other than that its alias-set contains base2_alias_set as a subset
     which does not help us here.  */
  /* As we know nothing useful about the dynamic type of the decl just
     use the usual conflict check rather than a subset test.
     ???  We could introduce -fvery-strict-aliasing when the language
     does not allow decls to have a dynamic type that differs from their
     static type.  Then we can check 
     !alias_set_subset_of (base1_alias_set, base2_alias_set) instead.  */
  if (base1_alias_set != base2_alias_set
      && !alias_sets_conflict_p (base1_alias_set, base2_alias_set))
    return false;

  ptrtype1 = TREE_TYPE (TREE_OPERAND (base1, 1));

  /* If the size of the access relevant for TBAA through the pointer
     is bigger than the size of the decl we can't possibly access the
     decl via that pointer.  */
  if (/* ???  This in turn may run afoul when a decl of type T which is
	 a member of union type U is accessed through a pointer to
	 type U and sizeof T is smaller than sizeof U.  */
      TREE_CODE (TREE_TYPE (ptrtype1)) != UNION_TYPE
      && TREE_CODE (TREE_TYPE (ptrtype1)) != QUAL_UNION_TYPE
      && compare_sizes (DECL_SIZE (base2),
		        TYPE_SIZE (TREE_TYPE (ptrtype1))) < 0)
    return false;

  if (!ref2)
    return true;

  /* If the decl is accessed via a MEM_REF, reconstruct the base
     we can use for TBAA and an appropriately adjusted offset.  */
  dbase2 = ref2;
  while (handled_component_p (dbase2))
    dbase2 = TREE_OPERAND (dbase2, 0);
  poly_int64 doffset1 = offset1;
  poly_offset_int doffset2 = offset2;
  if (TREE_CODE (dbase2) == MEM_REF
      || TREE_CODE (dbase2) == TARGET_MEM_REF)
    {
      doffset2 -= mem_ref_offset (dbase2) << LOG2_BITS_PER_UNIT;
      tree ptrtype2 = TREE_TYPE (TREE_OPERAND (dbase2, 1));
      /* If second reference is view-converted, give up now.  */
      if (same_type_for_tbaa (TREE_TYPE (dbase2), TREE_TYPE (ptrtype2)) != 1)
	return true;
    }

  /* If first reference is view-converted, give up now.  */
  if (same_type_for_tbaa (TREE_TYPE (base1), TREE_TYPE (ptrtype1)) != 1)
    return true;

  /* If both references are through the same type, they do not alias
     if the accesses do not overlap.  This does extra disambiguation
     for mixed/pointer accesses but requires strict aliasing.
     For MEM_REFs we require that the component-ref offset we computed
     is relative to the start of the type which we ensure by
     comparing rvalue and access type and disregarding the constant
     pointer offset.

     But avoid treating variable length arrays as "objects", instead assume they
     can overlap by an exact multiple of their element size.
     See gcc.dg/torture/alias-2.c.  */
  if (((TREE_CODE (base1) != TARGET_MEM_REF
       || (!TMR_INDEX (base1) && !TMR_INDEX2 (base1)))
       && (TREE_CODE (dbase2) != TARGET_MEM_REF
	   || (!TMR_INDEX (dbase2) && !TMR_INDEX2 (dbase2))))
      && same_type_for_tbaa (TREE_TYPE (base1), TREE_TYPE (dbase2)) == 1)
    {
      bool partial_overlap = (TREE_CODE (TREE_TYPE (base1)) == ARRAY_TYPE
			      && (TYPE_SIZE (TREE_TYPE (base1))
			      && TREE_CODE (TYPE_SIZE (TREE_TYPE (base1)))
				 != INTEGER_CST));
      if (!partial_overlap
	  && !ranges_maybe_overlap_p (doffset1, max_size1, doffset2, max_size2))
	return false;
      if (!ref1 || !ref2
	  /* If there is must alias, there is no use disambiguating further.  */
	  || (!partial_overlap
	      && known_eq (size1, max_size1) && known_eq (size2, max_size2)))
	return true;
      int res = nonoverlapping_refs_since_match_p (base1, ref1, base2, ref2,
						   partial_overlap);
      if (res == -1)
	return !nonoverlapping_component_refs_p (ref1, ref2);
      return !res;
    }

  /* Do access-path based disambiguation.  */
  if (ref1 && ref2
      && (handled_component_p (ref1) || handled_component_p (ref2)))
    return aliasing_component_refs_p (ref1,
				      ref1_alias_set, base1_alias_set,
				      offset1, max_size1,
				      ref2,
				      ref2_alias_set, base2_alias_set,
				      offset2, max_size2);

  return true;
}

/* Return true if two indirect references based on *PTR1
   and *PTR2 constrained to [OFFSET1, OFFSET1 + MAX_SIZE1) and
   [OFFSET2, OFFSET2 + MAX_SIZE2) may alias.  *PTR1 and *PTR2 have
   the alias sets BASE1_ALIAS_SET and BASE2_ALIAS_SET which can be -1
   in which case they are computed on-demand.  REF1 and REF2
   if non-NULL are the complete memory reference trees. */

static bool
indirect_refs_may_alias_p (tree ref1 ATTRIBUTE_UNUSED, tree base1,
			   poly_int64 offset1, poly_int64 max_size1,
			   poly_int64 size1,
			   alias_set_type ref1_alias_set,
			   alias_set_type base1_alias_set,
			   tree ref2 ATTRIBUTE_UNUSED, tree base2,
			   poly_int64 offset2, poly_int64 max_size2,
			   poly_int64 size2,
			   alias_set_type ref2_alias_set,
			   alias_set_type base2_alias_set, bool tbaa_p)
{
  tree ptr1;
  tree ptr2;
  tree ptrtype1, ptrtype2;

  gcc_checking_assert ((TREE_CODE (base1) == MEM_REF
			|| TREE_CODE (base1) == TARGET_MEM_REF)
		       && (TREE_CODE (base2) == MEM_REF
			   || TREE_CODE (base2) == TARGET_MEM_REF));

  ptr1 = TREE_OPERAND (base1, 0);
  ptr2 = TREE_OPERAND (base2, 0);

  /* If both bases are based on pointers they cannot alias if they may not
     point to the same memory object or if they point to the same object
     and the accesses do not overlap.  */
  if ((!cfun || gimple_in_ssa_p (cfun))
      && operand_equal_p (ptr1, ptr2, 0)
      && (((TREE_CODE (base1) != TARGET_MEM_REF
	    || (!TMR_INDEX (base1) && !TMR_INDEX2 (base1)))
	   && (TREE_CODE (base2) != TARGET_MEM_REF
	       || (!TMR_INDEX (base2) && !TMR_INDEX2 (base2))))
	  || (TREE_CODE (base1) == TARGET_MEM_REF
	      && TREE_CODE (base2) == TARGET_MEM_REF
	      && (TMR_STEP (base1) == TMR_STEP (base2)
		  || (TMR_STEP (base1) && TMR_STEP (base2)
		      && operand_equal_p (TMR_STEP (base1),
					  TMR_STEP (base2), 0)))
	      && (TMR_INDEX (base1) == TMR_INDEX (base2)
		  || (TMR_INDEX (base1) && TMR_INDEX (base2)
		      && operand_equal_p (TMR_INDEX (base1),
					  TMR_INDEX (base2), 0)))
	      && (TMR_INDEX2 (base1) == TMR_INDEX2 (base2)
		  || (TMR_INDEX2 (base1) && TMR_INDEX2 (base2)
		      && operand_equal_p (TMR_INDEX2 (base1),
					  TMR_INDEX2 (base2), 0))))))
    {
      poly_offset_int moff1 = mem_ref_offset (base1) << LOG2_BITS_PER_UNIT;
      poly_offset_int moff2 = mem_ref_offset (base2) << LOG2_BITS_PER_UNIT;
      if (!ranges_maybe_overlap_p (offset1 + moff1, max_size1,
				   offset2 + moff2, max_size2))
	return false;
      /* If there is must alias, there is no use disambiguating further.  */
      if (known_eq (size1, max_size1) && known_eq (size2, max_size2))
	return true;
      if (ref1 && ref2)
	{
	  int res = nonoverlapping_refs_since_match_p (NULL, ref1, NULL, ref2,
						       false);
	  if (res != -1)
	    return !res;
	}
    }
  if (!ptr_derefs_may_alias_p (ptr1, ptr2))
    return false;

  /* Disambiguations that rely on strict aliasing rules follow.  */
  if (!flag_strict_aliasing || !tbaa_p)
    return true;

  ptrtype1 = TREE_TYPE (TREE_OPERAND (base1, 1));
  ptrtype2 = TREE_TYPE (TREE_OPERAND (base2, 1));

  /* If the alias set for a pointer access is zero all bets are off.  */
  if (base1_alias_set == 0
      || base2_alias_set == 0)
    return true;

  /* Do type-based disambiguation.  */
  if (base1_alias_set != base2_alias_set
      && !alias_sets_conflict_p (base1_alias_set, base2_alias_set))
    return false;

  /* If either reference is view-converted, give up now.  */
  if (same_type_for_tbaa (TREE_TYPE (base1), TREE_TYPE (ptrtype1)) != 1
      || same_type_for_tbaa (TREE_TYPE (base2), TREE_TYPE (ptrtype2)) != 1)
    return true;

  /* If both references are through the same type, they do not alias
     if the accesses do not overlap.  This does extra disambiguation
     for mixed/pointer accesses but requires strict aliasing.  */
  if ((TREE_CODE (base1) != TARGET_MEM_REF
       || (!TMR_INDEX (base1) && !TMR_INDEX2 (base1)))
      && (TREE_CODE (base2) != TARGET_MEM_REF
	  || (!TMR_INDEX (base2) && !TMR_INDEX2 (base2)))
      && same_type_for_tbaa (TREE_TYPE (ptrtype1),
			     TREE_TYPE (ptrtype2)) == 1)
    {
      /* But avoid treating arrays as "objects", instead assume they
         can overlap by an exact multiple of their element size.
         See gcc.dg/torture/alias-2.c.  */
      bool partial_overlap = TREE_CODE (TREE_TYPE (ptrtype1)) == ARRAY_TYPE;

      if (!partial_overlap
	  && !ranges_maybe_overlap_p (offset1, max_size1, offset2, max_size2))
	return false;
      if (!ref1 || !ref2
	  || (!partial_overlap
	      && known_eq (size1, max_size1) && known_eq (size2, max_size2)))
	return true;
      int res = nonoverlapping_refs_since_match_p (base1, ref1, base2, ref2,
						   partial_overlap);
      if (res == -1)
	return !nonoverlapping_component_refs_p (ref1, ref2);
      return !res;
    }

  /* Do access-path based disambiguation.  */
  if (ref1 && ref2
      && (handled_component_p (ref1) || handled_component_p (ref2)))
    return aliasing_component_refs_p (ref1,
				      ref1_alias_set, base1_alias_set,
				      offset1, max_size1,
				      ref2,
				      ref2_alias_set, base2_alias_set,
				      offset2, max_size2);

  return true;
}

/* Return true, if the two memory references REF1 and REF2 may alias.  */

static bool
refs_may_alias_p_2 (ao_ref *ref1, ao_ref *ref2, bool tbaa_p)
{
  tree base1, base2;
  poly_int64 offset1 = 0, offset2 = 0;
  poly_int64 max_size1 = -1, max_size2 = -1;
  bool var1_p, var2_p, ind1_p, ind2_p;

  gcc_checking_assert ((!ref1->ref
			|| TREE_CODE (ref1->ref) == SSA_NAME
			|| DECL_P (ref1->ref)
			|| TREE_CODE (ref1->ref) == STRING_CST
			|| handled_component_p (ref1->ref)
			|| TREE_CODE (ref1->ref) == MEM_REF
			|| TREE_CODE (ref1->ref) == TARGET_MEM_REF)
		       && (!ref2->ref
			   || TREE_CODE (ref2->ref) == SSA_NAME
			   || DECL_P (ref2->ref)
			   || TREE_CODE (ref2->ref) == STRING_CST
			   || handled_component_p (ref2->ref)
			   || TREE_CODE (ref2->ref) == MEM_REF
			   || TREE_CODE (ref2->ref) == TARGET_MEM_REF));

  /* Decompose the references into their base objects and the access.  */
  base1 = ao_ref_base (ref1);
  offset1 = ref1->offset;
  max_size1 = ref1->max_size;
  base2 = ao_ref_base (ref2);
  offset2 = ref2->offset;
  max_size2 = ref2->max_size;

  /* We can end up with registers or constants as bases for example from
     *D.1663_44 = VIEW_CONVERT_EXPR<struct DB_LSN>(__tmp$B0F64_59);
     which is seen as a struct copy.  */
  if (TREE_CODE (base1) == SSA_NAME
      || TREE_CODE (base1) == CONST_DECL
      || TREE_CODE (base1) == CONSTRUCTOR
      || TREE_CODE (base1) == ADDR_EXPR
      || CONSTANT_CLASS_P (base1)
      || TREE_CODE (base2) == SSA_NAME
      || TREE_CODE (base2) == CONST_DECL
      || TREE_CODE (base2) == CONSTRUCTOR
      || TREE_CODE (base2) == ADDR_EXPR
      || CONSTANT_CLASS_P (base2))
    return false;

  /* We can end up referring to code via function and label decls.
     As we likely do not properly track code aliases conservatively
     bail out.  */
  if (TREE_CODE (base1) == FUNCTION_DECL
      || TREE_CODE (base1) == LABEL_DECL
      || TREE_CODE (base2) == FUNCTION_DECL
      || TREE_CODE (base2) == LABEL_DECL)
    return true;

  /* Two volatile accesses always conflict.  */
  if (ref1->volatile_p
      && ref2->volatile_p)
    return true;

  /* Defer to simple offset based disambiguation if we have
     references based on two decls.  Do this before defering to
     TBAA to handle must-alias cases in conformance with the
     GCC extension of allowing type-punning through unions.  */
  var1_p = DECL_P (base1);
  var2_p = DECL_P (base2);
  if (var1_p && var2_p)
    return decl_refs_may_alias_p (ref1->ref, base1, offset1, max_size1,
				  ref1->size,
				  ref2->ref, base2, offset2, max_size2,
				  ref2->size);

  /* Handle restrict based accesses.
     ???  ao_ref_base strips inner MEM_REF [&decl], recover from that
     here.  */
  tree rbase1 = base1;
  tree rbase2 = base2;
  if (var1_p)
    {
      rbase1 = ref1->ref;
      if (rbase1)
	while (handled_component_p (rbase1))
	  rbase1 = TREE_OPERAND (rbase1, 0);
    }
  if (var2_p)
    {
      rbase2 = ref2->ref;
      if (rbase2)
	while (handled_component_p (rbase2))
	  rbase2 = TREE_OPERAND (rbase2, 0);
    }
  if (rbase1 && rbase2
      && (TREE_CODE (base1) == MEM_REF || TREE_CODE (base1) == TARGET_MEM_REF)
      && (TREE_CODE (base2) == MEM_REF || TREE_CODE (base2) == TARGET_MEM_REF)
      /* If the accesses are in the same restrict clique... */
      && MR_DEPENDENCE_CLIQUE (base1) == MR_DEPENDENCE_CLIQUE (base2)
      /* But based on different pointers they do not alias.  */
      && MR_DEPENDENCE_BASE (base1) != MR_DEPENDENCE_BASE (base2))
    return false;

  ind1_p = (TREE_CODE (base1) == MEM_REF
	    || TREE_CODE (base1) == TARGET_MEM_REF);
  ind2_p = (TREE_CODE (base2) == MEM_REF
	    || TREE_CODE (base2) == TARGET_MEM_REF);

  /* Canonicalize the pointer-vs-decl case.  */
  if (ind1_p && var2_p)
    {
      std::swap (offset1, offset2);
      std::swap (max_size1, max_size2);
      std::swap (base1, base2);
      std::swap (ref1, ref2);
      var1_p = true;
      ind1_p = false;
      var2_p = false;
      ind2_p = true;
    }

  /* First defer to TBAA if possible.  */
  if (tbaa_p
      && flag_strict_aliasing
      && !alias_sets_conflict_p (ao_ref_alias_set (ref1),
				 ao_ref_alias_set (ref2)))
    return false;

  /* If the reference is based on a pointer that points to memory
     that may not be written to then the other reference cannot possibly
     clobber it.  */
  if ((TREE_CODE (TREE_OPERAND (base2, 0)) == SSA_NAME
       && SSA_NAME_POINTS_TO_READONLY_MEMORY (TREE_OPERAND (base2, 0)))
      || (ind1_p
	  && TREE_CODE (TREE_OPERAND (base1, 0)) == SSA_NAME
	  && SSA_NAME_POINTS_TO_READONLY_MEMORY (TREE_OPERAND (base1, 0))))
    return false;

  /* Dispatch to the pointer-vs-decl or pointer-vs-pointer disambiguators.  */
  if (var1_p && ind2_p)
    return indirect_ref_may_alias_decl_p (ref2->ref, base2,
					  offset2, max_size2, ref2->size,
					  ao_ref_alias_set (ref2),
					  ao_ref_base_alias_set (ref2),
					  ref1->ref, base1,
					  offset1, max_size1, ref1->size,
					  ao_ref_alias_set (ref1),
					  ao_ref_base_alias_set (ref1),
					  tbaa_p);
  else if (ind1_p && ind2_p)
    return indirect_refs_may_alias_p (ref1->ref, base1,
				      offset1, max_size1, ref1->size,
				      ao_ref_alias_set (ref1),
				      ao_ref_base_alias_set (ref1),
				      ref2->ref, base2,
				      offset2, max_size2, ref2->size,
				      ao_ref_alias_set (ref2),
				      ao_ref_base_alias_set (ref2),
				      tbaa_p);

  gcc_unreachable ();
}

/* Return true, if the two memory references REF1 and REF2 may alias
   and update statistics.  */

bool
refs_may_alias_p_1 (ao_ref *ref1, ao_ref *ref2, bool tbaa_p)
{
  bool res = refs_may_alias_p_2 (ref1, ref2, tbaa_p);
  if (res)
    ++alias_stats.refs_may_alias_p_may_alias;
  else
    ++alias_stats.refs_may_alias_p_no_alias;
  return res;
}

static bool
refs_may_alias_p (tree ref1, ao_ref *ref2, bool tbaa_p)
{
  ao_ref r1;
  ao_ref_init (&r1, ref1);
  return refs_may_alias_p_1 (&r1, ref2, tbaa_p);
}

bool
refs_may_alias_p (tree ref1, tree ref2, bool tbaa_p)
{
  ao_ref r1, r2;
  ao_ref_init (&r1, ref1);
  ao_ref_init (&r2, ref2);
  return refs_may_alias_p_1 (&r1, &r2, tbaa_p);
}

/* Returns true if there is a anti-dependence for the STORE that
   executes after the LOAD.  */

bool
refs_anti_dependent_p (tree load, tree store)
{
  ao_ref r1, r2;
  ao_ref_init (&r1, load);
  ao_ref_init (&r2, store);
  return refs_may_alias_p_1 (&r1, &r2, false);
}

/* Returns true if there is a output dependence for the stores
   STORE1 and STORE2.  */

bool
refs_output_dependent_p (tree store1, tree store2)
{
  ao_ref r1, r2;
  ao_ref_init (&r1, store1);
  ao_ref_init (&r2, store2);
  return refs_may_alias_p_1 (&r1, &r2, false);
}

/* If the call CALL may use the memory reference REF return true,
   otherwise return false.  */

static bool
ref_maybe_used_by_call_p_1 (gcall *call, ao_ref *ref, bool tbaa_p)
{
  tree base, callee;
  unsigned i;
  int flags = gimple_call_flags (call);

  /* Const functions without a static chain do not implicitly use memory.  */
  if (!gimple_call_chain (call)
      && (flags & (ECF_CONST|ECF_NOVOPS)))
    goto process_args;

  base = ao_ref_base (ref);
  if (!base)
    return true;

  /* A call that is not without side-effects might involve volatile
     accesses and thus conflicts with all other volatile accesses.  */
  if (ref->volatile_p)
    return true;

  /* If the reference is based on a decl that is not aliased the call
     cannot possibly use it.  */
  if (DECL_P (base)
      && !may_be_aliased (base)
      /* But local statics can be used through recursion.  */
      && !is_global_var (base))
    goto process_args;

  callee = gimple_call_fndecl (call);

  /* Handle those builtin functions explicitly that do not act as
     escape points.  See tree-ssa-structalias.c:find_func_aliases
     for the list of builtins we might need to handle here.  */
  if (callee != NULL_TREE
      && gimple_call_builtin_p (call, BUILT_IN_NORMAL))
    switch (DECL_FUNCTION_CODE (callee))
      {
	/* All the following functions read memory pointed to by
	   their second argument.  strcat/strncat additionally
	   reads memory pointed to by the first argument.  */
	case BUILT_IN_STRCAT:
	case BUILT_IN_STRNCAT:
	  {
	    ao_ref dref;
	    ao_ref_init_from_ptr_and_size (&dref,
					   gimple_call_arg (call, 0),
					   NULL_TREE);
	    if (refs_may_alias_p_1 (&dref, ref, false))
	      return true;
	  }
	  /* FALLTHRU */
	case BUILT_IN_STRCPY:
	case BUILT_IN_STRNCPY:
	case BUILT_IN_MEMCPY:
	case BUILT_IN_MEMMOVE:
	case BUILT_IN_MEMPCPY:
	case BUILT_IN_STPCPY:
	case BUILT_IN_STPNCPY:
	case BUILT_IN_TM_MEMCPY:
	case BUILT_IN_TM_MEMMOVE:
	  {
	    ao_ref dref;
	    tree size = NULL_TREE;
	    if (gimple_call_num_args (call) == 3)
	      size = gimple_call_arg (call, 2);
	    ao_ref_init_from_ptr_and_size (&dref,
					   gimple_call_arg (call, 1),
					   size);
	    return refs_may_alias_p_1 (&dref, ref, false);
	  }
	case BUILT_IN_STRCAT_CHK:
	case BUILT_IN_STRNCAT_CHK:
	  {
	    ao_ref dref;
	    ao_ref_init_from_ptr_and_size (&dref,
					   gimple_call_arg (call, 0),
					   NULL_TREE);
	    if (refs_may_alias_p_1 (&dref, ref, false))
	      return true;
	  }
	  /* FALLTHRU */
	case BUILT_IN_STRCPY_CHK:
	case BUILT_IN_STRNCPY_CHK:
	case BUILT_IN_MEMCPY_CHK:
	case BUILT_IN_MEMMOVE_CHK:
	case BUILT_IN_MEMPCPY_CHK:
	case BUILT_IN_STPCPY_CHK:
	case BUILT_IN_STPNCPY_CHK:
	  {
	    ao_ref dref;
	    tree size = NULL_TREE;
	    if (gimple_call_num_args (call) == 4)
	      size = gimple_call_arg (call, 2);
	    ao_ref_init_from_ptr_and_size (&dref,
					   gimple_call_arg (call, 1),
					   size);
	    return refs_may_alias_p_1 (&dref, ref, false);
	  }
	case BUILT_IN_BCOPY:
	  {
	    ao_ref dref;
	    tree size = gimple_call_arg (call, 2);
	    ao_ref_init_from_ptr_and_size (&dref,
					   gimple_call_arg (call, 0),
					   size);
	    return refs_may_alias_p_1 (&dref, ref, false);
	  }

	/* The following functions read memory pointed to by their
	   first argument.  */
	CASE_BUILT_IN_TM_LOAD (1):
	CASE_BUILT_IN_TM_LOAD (2):
	CASE_BUILT_IN_TM_LOAD (4):
	CASE_BUILT_IN_TM_LOAD (8):
	CASE_BUILT_IN_TM_LOAD (FLOAT):
	CASE_BUILT_IN_TM_LOAD (DOUBLE):
	CASE_BUILT_IN_TM_LOAD (LDOUBLE):
	CASE_BUILT_IN_TM_LOAD (M64):
	CASE_BUILT_IN_TM_LOAD (M128):
	CASE_BUILT_IN_TM_LOAD (M256):
	case BUILT_IN_TM_LOG:
	case BUILT_IN_TM_LOG_1:
	case BUILT_IN_TM_LOG_2:
	case BUILT_IN_TM_LOG_4:
	case BUILT_IN_TM_LOG_8:
	case BUILT_IN_TM_LOG_FLOAT:
	case BUILT_IN_TM_LOG_DOUBLE:
	case BUILT_IN_TM_LOG_LDOUBLE:
	case BUILT_IN_TM_LOG_M64:
	case BUILT_IN_TM_LOG_M128:
	case BUILT_IN_TM_LOG_M256:
	  return ptr_deref_may_alias_ref_p_1 (gimple_call_arg (call, 0), ref);

	/* These read memory pointed to by the first argument.  */
	case BUILT_IN_STRDUP:
	case BUILT_IN_STRNDUP:
	case BUILT_IN_REALLOC:
	  {
	    ao_ref dref;
	    tree size = NULL_TREE;
	    if (gimple_call_num_args (call) == 2)
	      size = gimple_call_arg (call, 1);
	    ao_ref_init_from_ptr_and_size (&dref,
					   gimple_call_arg (call, 0),
					   size);
	    return refs_may_alias_p_1 (&dref, ref, false);
	  }
	/* These read memory pointed to by the first argument.  */
	case BUILT_IN_INDEX:
	case BUILT_IN_STRCHR:
	case BUILT_IN_STRRCHR:
	  {
	    ao_ref dref;
	    ao_ref_init_from_ptr_and_size (&dref,
					   gimple_call_arg (call, 0),
					   NULL_TREE);
	    return refs_may_alias_p_1 (&dref, ref, false);
	  }
	/* These read memory pointed to by the first argument with size
	   in the third argument.  */
	case BUILT_IN_MEMCHR:
	  {
	    ao_ref dref;
	    ao_ref_init_from_ptr_and_size (&dref,
					   gimple_call_arg (call, 0),
					   gimple_call_arg (call, 2));
	    return refs_may_alias_p_1 (&dref, ref, false);
	  }
	/* These read memory pointed to by the first and second arguments.  */
	case BUILT_IN_STRSTR:
	case BUILT_IN_STRPBRK:
	  {
	    ao_ref dref;
	    ao_ref_init_from_ptr_and_size (&dref,
					   gimple_call_arg (call, 0),
					   NULL_TREE);
	    if (refs_may_alias_p_1 (&dref, ref, false))
	      return true;
	    ao_ref_init_from_ptr_and_size (&dref,
					   gimple_call_arg (call, 1),
					   NULL_TREE);
	    return refs_may_alias_p_1 (&dref, ref, false);
	  }

	/* The following builtins do not read from memory.  */
	case BUILT_IN_FREE:
	case BUILT_IN_MALLOC:
	case BUILT_IN_POSIX_MEMALIGN:
	case BUILT_IN_ALIGNED_ALLOC:
	case BUILT_IN_CALLOC:
	CASE_BUILT_IN_ALLOCA:
	case BUILT_IN_STACK_SAVE:
	case BUILT_IN_STACK_RESTORE:
	case BUILT_IN_MEMSET:
	case BUILT_IN_TM_MEMSET:
	case BUILT_IN_MEMSET_CHK:
	case BUILT_IN_FREXP:
	case BUILT_IN_FREXPF:
	case BUILT_IN_FREXPL:
	case BUILT_IN_GAMMA_R:
	case BUILT_IN_GAMMAF_R:
	case BUILT_IN_GAMMAL_R:
	case BUILT_IN_LGAMMA_R:
	case BUILT_IN_LGAMMAF_R:
	case BUILT_IN_LGAMMAL_R:
	case BUILT_IN_MODF:
	case BUILT_IN_MODFF:
	case BUILT_IN_MODFL:
	case BUILT_IN_REMQUO:
	case BUILT_IN_REMQUOF:
	case BUILT_IN_REMQUOL:
	case BUILT_IN_SINCOS:
	case BUILT_IN_SINCOSF:
	case BUILT_IN_SINCOSL:
	case BUILT_IN_ASSUME_ALIGNED:
	case BUILT_IN_VA_END:
	  return false;
	/* __sync_* builtins and some OpenMP builtins act as threading
	   barriers.  */
#undef DEF_SYNC_BUILTIN
#define DEF_SYNC_BUILTIN(ENUM, NAME, TYPE, ATTRS) case ENUM:
#include "sync-builtins.def"
#undef DEF_SYNC_BUILTIN
	case BUILT_IN_GOMP_ATOMIC_START:
	case BUILT_IN_GOMP_ATOMIC_END:
	case BUILT_IN_GOMP_BARRIER:
	case BUILT_IN_GOMP_BARRIER_CANCEL:
	case BUILT_IN_GOMP_TASKWAIT:
	case BUILT_IN_GOMP_TASKGROUP_END:
	case BUILT_IN_GOMP_CRITICAL_START:
	case BUILT_IN_GOMP_CRITICAL_END:
	case BUILT_IN_GOMP_CRITICAL_NAME_START:
	case BUILT_IN_GOMP_CRITICAL_NAME_END:
	case BUILT_IN_GOMP_LOOP_END:
	case BUILT_IN_GOMP_LOOP_END_CANCEL:
	case BUILT_IN_GOMP_ORDERED_START:
	case BUILT_IN_GOMP_ORDERED_END:
	case BUILT_IN_GOMP_SECTIONS_END:
	case BUILT_IN_GOMP_SECTIONS_END_CANCEL:
	case BUILT_IN_GOMP_SINGLE_COPY_START:
	case BUILT_IN_GOMP_SINGLE_COPY_END:
	  return true;

	default:
	  /* Fallthru to general call handling.  */;
      }

  /* Check if base is a global static variable that is not read
     by the function.  */
  if (callee != NULL_TREE && VAR_P (base) && TREE_STATIC (base))
    {
      struct cgraph_node *node = cgraph_node::get (callee);
      bitmap not_read;

      /* FIXME: Callee can be an OMP builtin that does not have a call graph
	 node yet.  We should enforce that there are nodes for all decls in the
	 IL and remove this check instead.  */
      if (node
	  && (not_read = ipa_reference_get_not_read_global (node))
	  && bitmap_bit_p (not_read, ipa_reference_var_uid (base)))
	goto process_args;
    }

  /* Check if the base variable is call-used.  */
  if (DECL_P (base))
    {
      if (pt_solution_includes (gimple_call_use_set (call), base))
	return true;
    }
  else if ((TREE_CODE (base) == MEM_REF
	    || TREE_CODE (base) == TARGET_MEM_REF)
	   && TREE_CODE (TREE_OPERAND (base, 0)) == SSA_NAME)
    {
      struct ptr_info_def *pi = SSA_NAME_PTR_INFO (TREE_OPERAND (base, 0));
      if (!pi)
	return true;

      if (pt_solutions_intersect (gimple_call_use_set (call), &pi->pt))
	return true;
    }
  else
    return true;

  /* Inspect call arguments for passed-by-value aliases.  */
process_args:
  for (i = 0; i < gimple_call_num_args (call); ++i)
    {
      tree op = gimple_call_arg (call, i);
      int flags = gimple_call_arg_flags (call, i);

      if (flags & EAF_UNUSED)
	continue;

      if (TREE_CODE (op) == WITH_SIZE_EXPR)
	op = TREE_OPERAND (op, 0);

      if (TREE_CODE (op) != SSA_NAME
	  && !is_gimple_min_invariant (op))
	{
	  ao_ref r;
	  ao_ref_init (&r, op);
	  if (refs_may_alias_p_1 (&r, ref, tbaa_p))
	    return true;
	}
    }

  return false;
}

static bool
ref_maybe_used_by_call_p (gcall *call, ao_ref *ref, bool tbaa_p)
{
  bool res;
  res = ref_maybe_used_by_call_p_1 (call, ref, tbaa_p);
  if (res)
    ++alias_stats.ref_maybe_used_by_call_p_may_alias;
  else
    ++alias_stats.ref_maybe_used_by_call_p_no_alias;
  return res;
}


/* If the statement STMT may use the memory reference REF return
   true, otherwise return false.  */

bool
ref_maybe_used_by_stmt_p (gimple *stmt, ao_ref *ref, bool tbaa_p)
{
  if (is_gimple_assign (stmt))
    {
      tree rhs;

      /* All memory assign statements are single.  */
      if (!gimple_assign_single_p (stmt))
	return false;

      rhs = gimple_assign_rhs1 (stmt);
      if (is_gimple_reg (rhs)
	  || is_gimple_min_invariant (rhs)
	  || gimple_assign_rhs_code (stmt) == CONSTRUCTOR)
	return false;

      return refs_may_alias_p (rhs, ref, tbaa_p);
    }
  else if (is_gimple_call (stmt))
    return ref_maybe_used_by_call_p (as_a <gcall *> (stmt), ref, tbaa_p);
  else if (greturn *return_stmt = dyn_cast <greturn *> (stmt))
    {
      tree retval = gimple_return_retval (return_stmt);
      if (retval
	  && TREE_CODE (retval) != SSA_NAME
	  && !is_gimple_min_invariant (retval)
	  && refs_may_alias_p (retval, ref, tbaa_p))
	return true;
      /* If ref escapes the function then the return acts as a use.  */
      tree base = ao_ref_base (ref);
      if (!base)
	;
      else if (DECL_P (base))
	return is_global_var (base);
      else if (TREE_CODE (base) == MEM_REF
	       || TREE_CODE (base) == TARGET_MEM_REF)
	return ptr_deref_may_alias_global_p (TREE_OPERAND (base, 0));
      return false;
    }

  return true;
}

bool
ref_maybe_used_by_stmt_p (gimple *stmt, tree ref, bool tbaa_p)
{
  ao_ref r;
  ao_ref_init (&r, ref);
  return ref_maybe_used_by_stmt_p (stmt, &r, tbaa_p);
}

/* If the call in statement CALL may clobber the memory reference REF
   return true, otherwise return false.  */

bool
call_may_clobber_ref_p_1 (gcall *call, ao_ref *ref)
{
  tree base;
  tree callee;

  /* If the call is pure or const it cannot clobber anything.  */
  if (gimple_call_flags (call)
      & (ECF_PURE|ECF_CONST|ECF_LOOPING_CONST_OR_PURE|ECF_NOVOPS))
    return false;
  if (gimple_call_internal_p (call))
    switch (gimple_call_internal_fn (call))
      {
	/* Treat these internal calls like ECF_PURE for aliasing,
	   they don't write to any memory the program should care about.
	   They have important other side-effects, and read memory,
	   so can't be ECF_NOVOPS.  */
      case IFN_UBSAN_NULL:
      case IFN_UBSAN_BOUNDS:
      case IFN_UBSAN_VPTR:
      case IFN_UBSAN_OBJECT_SIZE:
      case IFN_UBSAN_PTR:
      case IFN_ASAN_CHECK:
	return false;
      default:
	break;
      }

  base = ao_ref_base (ref);
  if (!base)
    return true;

  if (TREE_CODE (base) == SSA_NAME
      || CONSTANT_CLASS_P (base))
    return false;

  /* A call that is not without side-effects might involve volatile
     accesses and thus conflicts with all other volatile accesses.  */
  if (ref->volatile_p)
    return true;

  /* If the reference is based on a decl that is not aliased the call
     cannot possibly clobber it.  */
  if (DECL_P (base)
      && !may_be_aliased (base)
      /* But local non-readonly statics can be modified through recursion
         or the call may implement a threading barrier which we must
	 treat as may-def.  */
      && (TREE_READONLY (base)
	  || !is_global_var (base)))
    return false;

  /* If the reference is based on a pointer that points to memory
     that may not be written to then the call cannot possibly clobber it.  */
  if ((TREE_CODE (base) == MEM_REF
       || TREE_CODE (base) == TARGET_MEM_REF)
      && TREE_CODE (TREE_OPERAND (base, 0)) == SSA_NAME
      && SSA_NAME_POINTS_TO_READONLY_MEMORY (TREE_OPERAND (base, 0)))
    return false;

  callee = gimple_call_fndecl (call);

  /* Handle those builtin functions explicitly that do not act as
     escape points.  See tree-ssa-structalias.c:find_func_aliases
     for the list of builtins we might need to handle here.  */
  if (callee != NULL_TREE
      && gimple_call_builtin_p (call, BUILT_IN_NORMAL))
    switch (DECL_FUNCTION_CODE (callee))
      {
	/* All the following functions clobber memory pointed to by
	   their first argument.  */
	case BUILT_IN_STRCPY:
	case BUILT_IN_STRNCPY:
	case BUILT_IN_MEMCPY:
	case BUILT_IN_MEMMOVE:
	case BUILT_IN_MEMPCPY:
	case BUILT_IN_STPCPY:
	case BUILT_IN_STPNCPY:
	case BUILT_IN_STRCAT:
	case BUILT_IN_STRNCAT:
	case BUILT_IN_MEMSET:
	case BUILT_IN_TM_MEMSET:
	CASE_BUILT_IN_TM_STORE (1):
	CASE_BUILT_IN_TM_STORE (2):
	CASE_BUILT_IN_TM_STORE (4):
	CASE_BUILT_IN_TM_STORE (8):
	CASE_BUILT_IN_TM_STORE (FLOAT):
	CASE_BUILT_IN_TM_STORE (DOUBLE):
	CASE_BUILT_IN_TM_STORE (LDOUBLE):
	CASE_BUILT_IN_TM_STORE (M64):
	CASE_BUILT_IN_TM_STORE (M128):
	CASE_BUILT_IN_TM_STORE (M256):
	case BUILT_IN_TM_MEMCPY:
	case BUILT_IN_TM_MEMMOVE:
	  {
	    ao_ref dref;
	    tree size = NULL_TREE;
	    /* Don't pass in size for strncat, as the maximum size
	       is strlen (dest) + n + 1 instead of n, resp.
	       n + 1 at dest + strlen (dest), but strlen (dest) isn't
	       known.  */
	    if (gimple_call_num_args (call) == 3
		&& DECL_FUNCTION_CODE (callee) != BUILT_IN_STRNCAT)
	      size = gimple_call_arg (call, 2);
	    ao_ref_init_from_ptr_and_size (&dref,
					   gimple_call_arg (call, 0),
					   size);
	    return refs_may_alias_p_1 (&dref, ref, false);
	  }
	case BUILT_IN_STRCPY_CHK:
	case BUILT_IN_STRNCPY_CHK:
	case BUILT_IN_MEMCPY_CHK:
	case BUILT_IN_MEMMOVE_CHK:
	case BUILT_IN_MEMPCPY_CHK:
	case BUILT_IN_STPCPY_CHK:
	case BUILT_IN_STPNCPY_CHK:
	case BUILT_IN_STRCAT_CHK:
	case BUILT_IN_STRNCAT_CHK:
	case BUILT_IN_MEMSET_CHK:
	  {
	    ao_ref dref;
	    tree size = NULL_TREE;
	    /* Don't pass in size for __strncat_chk, as the maximum size
	       is strlen (dest) + n + 1 instead of n, resp.
	       n + 1 at dest + strlen (dest), but strlen (dest) isn't
	       known.  */
	    if (gimple_call_num_args (call) == 4
		&& DECL_FUNCTION_CODE (callee) != BUILT_IN_STRNCAT_CHK)
	      size = gimple_call_arg (call, 2);
	    ao_ref_init_from_ptr_and_size (&dref,
					   gimple_call_arg (call, 0),
					   size);
	    return refs_may_alias_p_1 (&dref, ref, false);
	  }
	case BUILT_IN_BCOPY:
	  {
	    ao_ref dref;
	    tree size = gimple_call_arg (call, 2);
	    ao_ref_init_from_ptr_and_size (&dref,
					   gimple_call_arg (call, 1),
					   size);
	    return refs_may_alias_p_1 (&dref, ref, false);
	  }
	/* Allocating memory does not have any side-effects apart from
	   being the definition point for the pointer.  */
	case BUILT_IN_MALLOC:
	case BUILT_IN_ALIGNED_ALLOC:
	case BUILT_IN_CALLOC:
	case BUILT_IN_STRDUP:
	case BUILT_IN_STRNDUP:
	  /* Unix98 specifies that errno is set on allocation failure.  */
	  if (flag_errno_math
	      && targetm.ref_may_alias_errno (ref))
	    return true;
	  return false;
	case BUILT_IN_STACK_SAVE:
	CASE_BUILT_IN_ALLOCA:
	case BUILT_IN_ASSUME_ALIGNED:
	  return false;
	/* But posix_memalign stores a pointer into the memory pointed to
	   by its first argument.  */
	case BUILT_IN_POSIX_MEMALIGN:
	  {
	    tree ptrptr = gimple_call_arg (call, 0);
	    ao_ref dref;
	    ao_ref_init_from_ptr_and_size (&dref, ptrptr,
					   TYPE_SIZE_UNIT (ptr_type_node));
	    return (refs_may_alias_p_1 (&dref, ref, false)
		    || (flag_errno_math
			&& targetm.ref_may_alias_errno (ref)));
	  }
	/* Freeing memory kills the pointed-to memory.  More importantly
	   the call has to serve as a barrier for moving loads and stores
	   across it.  */
	case BUILT_IN_FREE:
	case BUILT_IN_VA_END:
	  {
	    tree ptr = gimple_call_arg (call, 0);
	    return ptr_deref_may_alias_ref_p_1 (ptr, ref);
	  }
	/* Realloc serves both as allocation point and deallocation point.  */
	case BUILT_IN_REALLOC:
	  {
	    tree ptr = gimple_call_arg (call, 0);
	    /* Unix98 specifies that errno is set on allocation failure.  */
	    return ((flag_errno_math
		     && targetm.ref_may_alias_errno (ref))
		    || ptr_deref_may_alias_ref_p_1 (ptr, ref));
	  }
	case BUILT_IN_GAMMA_R:
	case BUILT_IN_GAMMAF_R:
	case BUILT_IN_GAMMAL_R:
	case BUILT_IN_LGAMMA_R:
	case BUILT_IN_LGAMMAF_R:
	case BUILT_IN_LGAMMAL_R:
	  {
	    tree out = gimple_call_arg (call, 1);
	    if (ptr_deref_may_alias_ref_p_1 (out, ref))
	      return true;
	    if (flag_errno_math)
	      break;
	    return false;
	  }
	case BUILT_IN_FREXP:
	case BUILT_IN_FREXPF:
	case BUILT_IN_FREXPL:
	case BUILT_IN_MODF:
	case BUILT_IN_MODFF:
	case BUILT_IN_MODFL:
	  {
	    tree out = gimple_call_arg (call, 1);
	    return ptr_deref_may_alias_ref_p_1 (out, ref);
	  }
	case BUILT_IN_REMQUO:
	case BUILT_IN_REMQUOF:
	case BUILT_IN_REMQUOL:
	  {
	    tree out = gimple_call_arg (call, 2);
	    if (ptr_deref_may_alias_ref_p_1 (out, ref))
	      return true;
	    if (flag_errno_math)
	      break;
	    return false;
	  }
	case BUILT_IN_SINCOS:
	case BUILT_IN_SINCOSF:
	case BUILT_IN_SINCOSL:
	  {
	    tree sin = gimple_call_arg (call, 1);
	    tree cos = gimple_call_arg (call, 2);
	    return (ptr_deref_may_alias_ref_p_1 (sin, ref)
		    || ptr_deref_may_alias_ref_p_1 (cos, ref));
	  }
	/* __sync_* builtins and some OpenMP builtins act as threading
	   barriers.  */
#undef DEF_SYNC_BUILTIN
#define DEF_SYNC_BUILTIN(ENUM, NAME, TYPE, ATTRS) case ENUM:
#include "sync-builtins.def"
#undef DEF_SYNC_BUILTIN
	case BUILT_IN_GOMP_ATOMIC_START:
	case BUILT_IN_GOMP_ATOMIC_END:
	case BUILT_IN_GOMP_BARRIER:
	case BUILT_IN_GOMP_BARRIER_CANCEL:
	case BUILT_IN_GOMP_TASKWAIT:
	case BUILT_IN_GOMP_TASKGROUP_END:
	case BUILT_IN_GOMP_CRITICAL_START:
	case BUILT_IN_GOMP_CRITICAL_END:
	case BUILT_IN_GOMP_CRITICAL_NAME_START:
	case BUILT_IN_GOMP_CRITICAL_NAME_END:
	case BUILT_IN_GOMP_LOOP_END:
	case BUILT_IN_GOMP_LOOP_END_CANCEL:
	case BUILT_IN_GOMP_ORDERED_START:
	case BUILT_IN_GOMP_ORDERED_END:
	case BUILT_IN_GOMP_SECTIONS_END:
	case BUILT_IN_GOMP_SECTIONS_END_CANCEL:
	case BUILT_IN_GOMP_SINGLE_COPY_START:
	case BUILT_IN_GOMP_SINGLE_COPY_END:
	  return true;
	default:
	  /* Fallthru to general call handling.  */;
      }

  /* Check if base is a global static variable that is not written
     by the function.  */
  if (callee != NULL_TREE && VAR_P (base) && TREE_STATIC (base))
    {
      struct cgraph_node *node = cgraph_node::get (callee);
      bitmap not_written;

      if (node
	  && (not_written = ipa_reference_get_not_written_global (node))
	  && bitmap_bit_p (not_written, ipa_reference_var_uid (base)))
	return false;
    }

  /* Check if the base variable is call-clobbered.  */
  if (DECL_P (base))
    return pt_solution_includes (gimple_call_clobber_set (call), base);
  else if ((TREE_CODE (base) == MEM_REF
	    || TREE_CODE (base) == TARGET_MEM_REF)
	   && TREE_CODE (TREE_OPERAND (base, 0)) == SSA_NAME)
    {
      struct ptr_info_def *pi = SSA_NAME_PTR_INFO (TREE_OPERAND (base, 0));
      if (!pi)
	return true;

      return pt_solutions_intersect (gimple_call_clobber_set (call), &pi->pt);
    }

  return true;
}

/* If the call in statement CALL may clobber the memory reference REF
   return true, otherwise return false.  */

bool
call_may_clobber_ref_p (gcall *call, tree ref)
{
  bool res;
  ao_ref r;
  ao_ref_init (&r, ref);
  res = call_may_clobber_ref_p_1 (call, &r);
  if (res)
    ++alias_stats.call_may_clobber_ref_p_may_alias;
  else
    ++alias_stats.call_may_clobber_ref_p_no_alias;
  return res;
}


/* If the statement STMT may clobber the memory reference REF return true,
   otherwise return false.  */

bool
stmt_may_clobber_ref_p_1 (gimple *stmt, ao_ref *ref, bool tbaa_p)
{
  if (is_gimple_call (stmt))
    {
      tree lhs = gimple_call_lhs (stmt);
      if (lhs
	  && TREE_CODE (lhs) != SSA_NAME)
	{
	  ao_ref r;
	  ao_ref_init (&r, lhs);
	  if (refs_may_alias_p_1 (ref, &r, tbaa_p))
	    return true;
	}

      return call_may_clobber_ref_p_1 (as_a <gcall *> (stmt), ref);
    }
  else if (gimple_assign_single_p (stmt))
    {
      tree lhs = gimple_assign_lhs (stmt);
      if (TREE_CODE (lhs) != SSA_NAME)
	{
	  ao_ref r;
	  ao_ref_init (&r, lhs);
	  return refs_may_alias_p_1 (ref, &r, tbaa_p);
	}
    }
  else if (gimple_code (stmt) == GIMPLE_ASM)
    return true;

  return false;
}

bool
stmt_may_clobber_ref_p (gimple *stmt, tree ref, bool tbaa_p)
{
  ao_ref r;
  ao_ref_init (&r, ref);
  return stmt_may_clobber_ref_p_1 (stmt, &r, tbaa_p);
}

/* Return true if store1 and store2 described by corresponding tuples
   <BASE, OFFSET, SIZE, MAX_SIZE> have the same size and store to the same
   address.  */

static bool
same_addr_size_stores_p (tree base1, poly_int64 offset1, poly_int64 size1,
			 poly_int64 max_size1,
			 tree base2, poly_int64 offset2, poly_int64 size2,
			 poly_int64 max_size2)
{
  /* Offsets need to be 0.  */
  if (maybe_ne (offset1, 0)
      || maybe_ne (offset2, 0))
    return false;

  bool base1_obj_p = SSA_VAR_P (base1);
  bool base2_obj_p = SSA_VAR_P (base2);

  /* We need one object.  */
  if (base1_obj_p == base2_obj_p)
    return false;
  tree obj = base1_obj_p ? base1 : base2;

  /* And we need one MEM_REF.  */
  bool base1_memref_p = TREE_CODE (base1) == MEM_REF;
  bool base2_memref_p = TREE_CODE (base2) == MEM_REF;
  if (base1_memref_p == base2_memref_p)
    return false;
  tree memref = base1_memref_p ? base1 : base2;

  /* Sizes need to be valid.  */
  if (!known_size_p (max_size1)
      || !known_size_p (max_size2)
      || !known_size_p (size1)
      || !known_size_p (size2))
    return false;

  /* Max_size needs to match size.  */
  if (maybe_ne (max_size1, size1)
      || maybe_ne (max_size2, size2))
    return false;

  /* Sizes need to match.  */
  if (maybe_ne (size1, size2))
    return false;


  /* Check that memref is a store to pointer with singleton points-to info.  */
  if (!integer_zerop (TREE_OPERAND (memref, 1)))
    return false;
  tree ptr = TREE_OPERAND (memref, 0);
  if (TREE_CODE (ptr) != SSA_NAME)
    return false;
  struct ptr_info_def *pi = SSA_NAME_PTR_INFO (ptr);
  unsigned int pt_uid;
  if (pi == NULL
      || !pt_solution_singleton_or_null_p (&pi->pt, &pt_uid))
    return false;

  /* Be conservative with non-call exceptions when the address might
     be NULL.  */
  if (cfun->can_throw_non_call_exceptions && pi->pt.null)
    return false;

  /* Check that ptr points relative to obj.  */
  unsigned int obj_uid = DECL_PT_UID (obj);
  if (obj_uid != pt_uid)
    return false;

  /* Check that the object size is the same as the store size.  That ensures us
     that ptr points to the start of obj.  */
  return (DECL_SIZE (obj)
	  && poly_int_tree_p (DECL_SIZE (obj))
	  && known_eq (wi::to_poly_offset (DECL_SIZE (obj)), size1));
}

/* If STMT kills the memory reference REF return true, otherwise
   return false.  */

bool
stmt_kills_ref_p (gimple *stmt, ao_ref *ref)
{
  if (!ao_ref_base (ref))
    return false;

  if (gimple_has_lhs (stmt)
      && TREE_CODE (gimple_get_lhs (stmt)) != SSA_NAME
      /* The assignment is not necessarily carried out if it can throw
	 and we can catch it in the current function where we could inspect
	 the previous value.
	 ???  We only need to care about the RHS throwing.  For aggregate
	 assignments or similar calls and non-call exceptions the LHS
	 might throw as well.  */
      && !stmt_can_throw_internal (cfun, stmt))
    {
      tree lhs = gimple_get_lhs (stmt);
      /* If LHS is literally a base of the access we are done.  */
      if (ref->ref)
	{
	  tree base = ref->ref;
	  tree innermost_dropped_array_ref = NULL_TREE;
	  if (handled_component_p (base))
	    {
	      tree saved_lhs0 = NULL_TREE;
	      if (handled_component_p (lhs))
		{
		  saved_lhs0 = TREE_OPERAND (lhs, 0);
		  TREE_OPERAND (lhs, 0) = integer_zero_node;
		}
	      do
		{
		  /* Just compare the outermost handled component, if
		     they are equal we have found a possible common
		     base.  */
		  tree saved_base0 = TREE_OPERAND (base, 0);
		  TREE_OPERAND (base, 0) = integer_zero_node;
		  bool res = operand_equal_p (lhs, base, 0);
		  TREE_OPERAND (base, 0) = saved_base0;
		  if (res)
		    break;
		  /* Remember if we drop an array-ref that we need to
		     double-check not being at struct end.  */ 
		  if (TREE_CODE (base) == ARRAY_REF
		      || TREE_CODE (base) == ARRAY_RANGE_REF)
		    innermost_dropped_array_ref = base;
		  /* Otherwise drop handled components of the access.  */
		  base = saved_base0;
		}
	      while (handled_component_p (base));
	      if (saved_lhs0)
		TREE_OPERAND (lhs, 0) = saved_lhs0;
	    }
	  /* Finally check if the lhs has the same address and size as the
	     base candidate of the access.  Watch out if we have dropped
	     an array-ref that was at struct end, this means ref->ref may
	     be outside of the TYPE_SIZE of its base.  */
	  if ((! innermost_dropped_array_ref
	       || ! array_at_struct_end_p (innermost_dropped_array_ref))
	      && (lhs == base
		  || (((TYPE_SIZE (TREE_TYPE (lhs))
			== TYPE_SIZE (TREE_TYPE (base)))
		       || (TYPE_SIZE (TREE_TYPE (lhs))
			   && TYPE_SIZE (TREE_TYPE (base))
			   && operand_equal_p (TYPE_SIZE (TREE_TYPE (lhs)),
					       TYPE_SIZE (TREE_TYPE (base)),
					       0)))
		      && operand_equal_p (lhs, base,
					  OEP_ADDRESS_OF
					  | OEP_MATCH_SIDE_EFFECTS))))
	    return true;
	}

      /* Now look for non-literal equal bases with the restriction of
         handling constant offset and size.  */
      /* For a must-alias check we need to be able to constrain
	 the access properly.  */
      if (!ref->max_size_known_p ())
	return false;
      poly_int64 size, offset, max_size, ref_offset = ref->offset;
      bool reverse;
      tree base = get_ref_base_and_extent (lhs, &offset, &size, &max_size,
					   &reverse);
      /* We can get MEM[symbol: sZ, index: D.8862_1] here,
	 so base == ref->base does not always hold.  */
      if (base != ref->base)
	{
	  /* Try using points-to info.  */
	  if (same_addr_size_stores_p (base, offset, size, max_size, ref->base,
				       ref->offset, ref->size, ref->max_size))
	    return true;

	  /* If both base and ref->base are MEM_REFs, only compare the
	     first operand, and if the second operand isn't equal constant,
	     try to add the offsets into offset and ref_offset.  */
	  if (TREE_CODE (base) == MEM_REF && TREE_CODE (ref->base) == MEM_REF
	      && TREE_OPERAND (base, 0) == TREE_OPERAND (ref->base, 0))
	    {
	      if (!tree_int_cst_equal (TREE_OPERAND (base, 1),
				       TREE_OPERAND (ref->base, 1)))
		{
		  poly_offset_int off1 = mem_ref_offset (base);
		  off1 <<= LOG2_BITS_PER_UNIT;
		  off1 += offset;
		  poly_offset_int off2 = mem_ref_offset (ref->base);
		  off2 <<= LOG2_BITS_PER_UNIT;
		  off2 += ref_offset;
		  if (!off1.to_shwi (&offset) || !off2.to_shwi (&ref_offset))
		    size = -1;
		}
	    }
	  else
	    size = -1;
	}
      /* For a must-alias check we need to be able to constrain
	 the access properly.  */
      if (known_eq (size, max_size)
	  && known_subrange_p (ref_offset, ref->max_size, offset, size))
	return true;
    }

  if (is_gimple_call (stmt))
    {
      tree callee = gimple_call_fndecl (stmt);
      if (callee != NULL_TREE
	  && gimple_call_builtin_p (stmt, BUILT_IN_NORMAL))
	switch (DECL_FUNCTION_CODE (callee))
	  {
	  case BUILT_IN_FREE:
	    {
	      tree ptr = gimple_call_arg (stmt, 0);
	      tree base = ao_ref_base (ref);
	      if (base && TREE_CODE (base) == MEM_REF
		  && TREE_OPERAND (base, 0) == ptr)
		return true;
	      break;
	    }

	  case BUILT_IN_MEMCPY:
	  case BUILT_IN_MEMPCPY:
	  case BUILT_IN_MEMMOVE:
	  case BUILT_IN_MEMSET:
	  case BUILT_IN_MEMCPY_CHK:
	  case BUILT_IN_MEMPCPY_CHK:
	  case BUILT_IN_MEMMOVE_CHK:
	  case BUILT_IN_MEMSET_CHK:
	  case BUILT_IN_STRNCPY:
	  case BUILT_IN_STPNCPY:
	  case BUILT_IN_CALLOC:
	    {
	      /* For a must-alias check we need to be able to constrain
		 the access properly.  */
	      if (!ref->max_size_known_p ())
		return false;
	      tree dest;
	      tree len;

	      /* In execution order a calloc call will never kill
		 anything.  However, DSE will (ab)use this interface
		 to ask if a calloc call writes the same memory locations
		 as a later assignment, memset, etc.  So handle calloc
		 in the expected way.  */
	      if (DECL_FUNCTION_CODE (callee) == BUILT_IN_CALLOC)
		{
		  tree arg0 = gimple_call_arg (stmt, 0);
		  tree arg1 = gimple_call_arg (stmt, 1);
		  if (TREE_CODE (arg0) != INTEGER_CST
		      || TREE_CODE (arg1) != INTEGER_CST)
		    return false;

		  dest = gimple_call_lhs (stmt);
		  len = fold_build2 (MULT_EXPR, TREE_TYPE (arg0), arg0, arg1);
		}
	      else
		{
		  dest = gimple_call_arg (stmt, 0);
		  len = gimple_call_arg (stmt, 2);
		}
	      if (!poly_int_tree_p (len))
		return false;
	      tree rbase = ref->base;
	      poly_offset_int roffset = ref->offset;
	      ao_ref dref;
	      ao_ref_init_from_ptr_and_size (&dref, dest, len);
	      tree base = ao_ref_base (&dref);
	      poly_offset_int offset = dref.offset;
	      if (!base || !known_size_p (dref.size))
		return false;
	      if (TREE_CODE (base) == MEM_REF)
		{
		  if (TREE_CODE (rbase) != MEM_REF)
		    return false;
		  // Compare pointers.
		  offset += mem_ref_offset (base) << LOG2_BITS_PER_UNIT;
		  roffset += mem_ref_offset (rbase) << LOG2_BITS_PER_UNIT;
		  base = TREE_OPERAND (base, 0);
		  rbase = TREE_OPERAND (rbase, 0);
		}
	      if (base == rbase
		  && known_subrange_p (roffset, ref->max_size, offset,
				       wi::to_poly_offset (len)
				       << LOG2_BITS_PER_UNIT))
		return true;
	      break;
	    }

	  case BUILT_IN_VA_END:
	    {
	      tree ptr = gimple_call_arg (stmt, 0);
	      if (TREE_CODE (ptr) == ADDR_EXPR)
		{
		  tree base = ao_ref_base (ref);
		  if (TREE_OPERAND (ptr, 0) == base)
		    return true;
		}
	      break;
	    }

	  default:;
	  }
    }
  return false;
}

bool
stmt_kills_ref_p (gimple *stmt, tree ref)
{
  ao_ref r;
  ao_ref_init (&r, ref);
  return stmt_kills_ref_p (stmt, &r);
}


/* Walk the virtual use-def chain of VUSE until hitting the virtual operand
   TARGET or a statement clobbering the memory reference REF in which
   case false is returned.  The walk starts with VUSE, one argument of PHI.  */

static bool
maybe_skip_until (gimple *phi, tree &target, basic_block target_bb,
		  ao_ref *ref, tree vuse, bool tbaa_p, unsigned int &limit,
		  bitmap *visited, bool abort_on_visited,
		  void *(*translate)(ao_ref *, tree, void *, translate_flags *),
		  translate_flags disambiguate_only,
		  void *data)
{
  basic_block bb = gimple_bb (phi);

  if (!*visited)
    *visited = BITMAP_ALLOC (NULL);

  bitmap_set_bit (*visited, SSA_NAME_VERSION (PHI_RESULT (phi)));

  /* Walk until we hit the target.  */
  while (vuse != target)
    {
      gimple *def_stmt = SSA_NAME_DEF_STMT (vuse);
      /* If we are searching for the target VUSE by walking up to
         TARGET_BB dominating the original PHI we are finished once
	 we reach a default def or a definition in a block dominating
	 that block.  Update TARGET and return.  */
      if (!target
	  && (gimple_nop_p (def_stmt)
	      || dominated_by_p (CDI_DOMINATORS,
				 target_bb, gimple_bb (def_stmt))))
	{
	  target = vuse;
	  return true;
	}

      /* Recurse for PHI nodes.  */
      if (gimple_code (def_stmt) == GIMPLE_PHI)
	{
	  /* An already visited PHI node ends the walk successfully.  */
	  if (bitmap_bit_p (*visited, SSA_NAME_VERSION (PHI_RESULT (def_stmt))))
	    return !abort_on_visited;
	  vuse = get_continuation_for_phi (def_stmt, ref, tbaa_p, limit,
					   visited, abort_on_visited,
					   translate, data, disambiguate_only);
	  if (!vuse)
	    return false;
	  continue;
	}
      else if (gimple_nop_p (def_stmt))
	return false;
      else
	{
	  /* A clobbering statement or the end of the IL ends it failing.  */
	  if ((int)limit <= 0)
	    return false;
	  --limit;
	  if (stmt_may_clobber_ref_p_1 (def_stmt, ref, tbaa_p))
	    {
	      translate_flags tf = disambiguate_only;
	      if (translate
		  && (*translate) (ref, vuse, data, &tf) == NULL)
		;
	      else
		return false;
	    }
	}
      /* If we reach a new basic-block see if we already skipped it
         in a previous walk that ended successfully.  */
      if (gimple_bb (def_stmt) != bb)
	{
	  if (!bitmap_set_bit (*visited, SSA_NAME_VERSION (vuse)))
	    return !abort_on_visited;
	  bb = gimple_bb (def_stmt);
	}
      vuse = gimple_vuse (def_stmt);
    }
  return true;
}


/* Starting from a PHI node for the virtual operand of the memory reference
   REF find a continuation virtual operand that allows to continue walking
   statements dominating PHI skipping only statements that cannot possibly
   clobber REF.  Decrements LIMIT for each alias disambiguation done
   and aborts the walk, returning NULL_TREE if it reaches zero.
   Returns NULL_TREE if no suitable virtual operand can be found.  */

tree
get_continuation_for_phi (gimple *phi, ao_ref *ref, bool tbaa_p,
			  unsigned int &limit, bitmap *visited,
			  bool abort_on_visited,
			  void *(*translate)(ao_ref *, tree, void *,
					     translate_flags *),
			  void *data,
			  translate_flags disambiguate_only)
{
  unsigned nargs = gimple_phi_num_args (phi);

  /* Through a single-argument PHI we can simply look through.  */
  if (nargs == 1)
    return PHI_ARG_DEF (phi, 0);

  /* For two or more arguments try to pairwise skip non-aliasing code
     until we hit the phi argument definition that dominates the other one.  */
  basic_block phi_bb = gimple_bb (phi);
  tree arg0, arg1;
  unsigned i;

  /* Find a candidate for the virtual operand which definition
     dominates those of all others.  */
  /* First look if any of the args themselves satisfy this.  */
  for (i = 0; i < nargs; ++i)
    {
      arg0 = PHI_ARG_DEF (phi, i);
      if (SSA_NAME_IS_DEFAULT_DEF (arg0))
	break;
      basic_block def_bb = gimple_bb (SSA_NAME_DEF_STMT (arg0));
      if (def_bb != phi_bb
	  && dominated_by_p (CDI_DOMINATORS, phi_bb, def_bb))
	break;
      arg0 = NULL_TREE;
    }
  /* If not, look if we can reach such candidate by walking defs
     until we hit the immediate dominator.  maybe_skip_until will
     do that for us.  */
  basic_block dom = get_immediate_dominator (CDI_DOMINATORS, phi_bb);

  /* Then check against the (to be) found candidate.  */
  for (i = 0; i < nargs; ++i)
    {
      arg1 = PHI_ARG_DEF (phi, i);
      if (arg1 == arg0)
	;
      else if (! maybe_skip_until (phi, arg0, dom, ref, arg1, tbaa_p,
				   limit, visited,
				   abort_on_visited,
				   translate,
				   /* Do not valueize when walking over
				      backedges.  */
				   dominated_by_p
				     (CDI_DOMINATORS,
				      gimple_bb (SSA_NAME_DEF_STMT (arg1)),
				      phi_bb)
				   ? TR_DISAMBIGUATE
				   : disambiguate_only, data))
	return NULL_TREE;
    }

  return arg0;
}

/* Based on the memory reference REF and its virtual use VUSE call
   WALKER for each virtual use that is equivalent to VUSE, including VUSE
   itself.  That is, for each virtual use for which its defining statement
   does not clobber REF.

   WALKER is called with REF, the current virtual use and DATA.  If
   WALKER returns non-NULL the walk stops and its result is returned.
   At the end of a non-successful walk NULL is returned.

   TRANSLATE if non-NULL is called with a pointer to REF, the virtual
   use which definition is a statement that may clobber REF and DATA.
   If TRANSLATE returns (void *)-1 the walk stops and NULL is returned.
   If TRANSLATE returns non-NULL the walk stops and its result is returned.
   If TRANSLATE returns NULL the walk continues and TRANSLATE is supposed
   to adjust REF and *DATA to make that valid.

   VALUEIZE if non-NULL is called with the next VUSE that is considered
   and return value is substituted for that.  This can be used to
   implement optimistic value-numbering for example.  Note that the
   VUSE argument is assumed to be valueized already.

   LIMIT specifies the number of alias queries we are allowed to do,
   the walk stops when it reaches zero and NULL is returned.  LIMIT
   is decremented by the number of alias queries (plus adjustments
   done by the callbacks) upon return.

   TODO: Cache the vector of equivalent vuses per ref, vuse pair.  */

void *
walk_non_aliased_vuses (ao_ref *ref, tree vuse, bool tbaa_p,
			void *(*walker)(ao_ref *, tree, void *),
			void *(*translate)(ao_ref *, tree, void *,
					   translate_flags *),
			tree (*valueize)(tree),
			unsigned &limit, void *data)
{
  bitmap visited = NULL;
  void *res;
  bool translated = false;

  timevar_push (TV_ALIAS_STMT_WALK);

  do
    {
      gimple *def_stmt;

      /* ???  Do we want to account this to TV_ALIAS_STMT_WALK?  */
      res = (*walker) (ref, vuse, data);
      /* Abort walk.  */
      if (res == (void *)-1)
	{
	  res = NULL;
	  break;
	}
      /* Lookup succeeded.  */
      else if (res != NULL)
	break;

      if (valueize)
	{
	  vuse = valueize (vuse);
	  if (!vuse)
	    {
	      res = NULL;
	      break;
	    }
	}
      def_stmt = SSA_NAME_DEF_STMT (vuse);
      if (gimple_nop_p (def_stmt))
	break;
      else if (gimple_code (def_stmt) == GIMPLE_PHI)
	vuse = get_continuation_for_phi (def_stmt, ref, tbaa_p, limit,
					 &visited, translated, translate, data);
      else
	{
	  if ((int)limit <= 0)
	    {
	      res = NULL;
	      break;
	    }
	  --limit;
	  if (stmt_may_clobber_ref_p_1 (def_stmt, ref, tbaa_p))
	    {
	      if (!translate)
		break;
	      translate_flags disambiguate_only = TR_TRANSLATE;
	      res = (*translate) (ref, vuse, data, &disambiguate_only);
	      /* Failed lookup and translation.  */
	      if (res == (void *)-1)
		{
		  res = NULL;
		  break;
		}
	      /* Lookup succeeded.  */
	      else if (res != NULL)
		break;
	      /* Translation succeeded, continue walking.  */
	      translated = translated || disambiguate_only == TR_TRANSLATE;
	    }
	  vuse = gimple_vuse (def_stmt);
	}
    }
  while (vuse);

  if (visited)
    BITMAP_FREE (visited);

  timevar_pop (TV_ALIAS_STMT_WALK);

  return res;
}


/* Based on the memory reference REF call WALKER for each vdef which
   defining statement may clobber REF, starting with VDEF.  If REF
   is NULL_TREE, each defining statement is visited.

   WALKER is called with REF, the current vdef and DATA.  If WALKER
   returns true the walk is stopped, otherwise it continues.

   If function entry is reached, FUNCTION_ENTRY_REACHED is set to true.
   The pointer may be NULL and then we do not track this information.

   At PHI nodes walk_aliased_vdefs forks into one walk for reach
   PHI argument (but only one walk continues on merge points), the
   return value is true if any of the walks was successful.

   The function returns the number of statements walked or -1 if
   LIMIT stmts were walked and the walk was aborted at this point.
   If LIMIT is zero the walk is not aborted.  */

static int
walk_aliased_vdefs_1 (ao_ref *ref, tree vdef,
		      bool (*walker)(ao_ref *, tree, void *), void *data,
		      bitmap *visited, unsigned int cnt,
		      bool *function_entry_reached, unsigned limit)
{
  do
    {
      gimple *def_stmt = SSA_NAME_DEF_STMT (vdef);

      if (*visited
	  && !bitmap_set_bit (*visited, SSA_NAME_VERSION (vdef)))
	return cnt;

      if (gimple_nop_p (def_stmt))
	{
	  if (function_entry_reached)
	    *function_entry_reached = true;
	  return cnt;
	}
      else if (gimple_code (def_stmt) == GIMPLE_PHI)
	{
	  unsigned i;
	  if (!*visited)
	    *visited = BITMAP_ALLOC (NULL);
	  for (i = 0; i < gimple_phi_num_args (def_stmt); ++i)
	    {
	      int res = walk_aliased_vdefs_1 (ref,
					      gimple_phi_arg_def (def_stmt, i),
					      walker, data, visited, cnt,
					      function_entry_reached, limit);
	      if (res == -1)
		return -1;
	      cnt = res;
	    }
	  return cnt;
	}

      /* ???  Do we want to account this to TV_ALIAS_STMT_WALK?  */
      cnt++;
      if (cnt == limit)
	return -1;
      if ((!ref
	   || stmt_may_clobber_ref_p_1 (def_stmt, ref))
	  && (*walker) (ref, vdef, data))
	return cnt;

      vdef = gimple_vuse (def_stmt);
    }
  while (1);
}

int
walk_aliased_vdefs (ao_ref *ref, tree vdef,
		    bool (*walker)(ao_ref *, tree, void *), void *data,
		    bitmap *visited,
		    bool *function_entry_reached, unsigned int limit)
{
  bitmap local_visited = NULL;
  int ret;

  timevar_push (TV_ALIAS_STMT_WALK);

  if (function_entry_reached)
    *function_entry_reached = false;

  ret = walk_aliased_vdefs_1 (ref, vdef, walker, data,
			      visited ? visited : &local_visited, 0,
			      function_entry_reached, limit);
  if (local_visited)
    BITMAP_FREE (local_visited);

  timevar_pop (TV_ALIAS_STMT_WALK);

  return ret;
}

