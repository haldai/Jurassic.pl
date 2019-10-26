#ifndef _JURASSIC_H
#define _JURASSIC_H

#include <SWI-Prolog.h>
#include <SWI-Stream.h>
#include <julia.h>

#define JURASSIC_DEBUG

#define BUFFSIZE 1024

#define JURASSIC_SUCCESS 1
#define JURASSIC_FAIL 0

/* Convert Prolog atoms to Julia values. When an atom is a defined Julia variable,
   the "flag_sym" argument determines whether to return its symbol or its value. */
int atom_to_jl(atom_t atom, jl_value_t **ret, int flag_sym);
/* Convert Prolog lists to Julia arrays.
   TODO: multi-dimension arrays */
int list_to_jl(term_t list, int length, jl_array_t **ret, int flag_sym);
/* Convert prolog compounds to Julia expressions.
   FIXME: GC issues? */
jl_expr_t * compound_to_jl_expr(term_t expr);
/* High-level function to convert Prolog term to Julia value */
int pl2jl(term_t term, jl_value_t **ret, int flag_sym);
/* Convert (unify Julia value with Prolog term) */
int jl_unify_pl(jl_value_t *val, term_t *ret);
/* Set Julia expression "*ex"'s arguments with Prolog term "expr"'s arguments,
   in total of "arity" arguments, starting from start_jl and start_pl respectively.
   FIXME: GC issue? */
int jl_set_args(jl_expr_t **ex, term_t expr, size_t arity, size_t start_jl, size_t start_pl);

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
   Prolog foreign predicates
   - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
install_t install_jurassic(void);
foreign_t jl_eval_str(term_t jl_expr, term_t pl_ret);
foreign_t jl_eval(term_t jl_expr, term_t pl_ret);
foreign_t jl_tuple_unify(term_t pl_tuple, term_t jl_expr);
foreign_t jl_tuple_unify_str(term_t pl_tuple, term_t jl_expr_str);
foreign_t jl_send_command_str(term_t jl_expr);
foreign_t jl_send_command(term_t jl_expr);
foreign_t jl_isdefined(term_t jl_expr);
foreign_t jl_using(term_t term);
foreign_t jl_include(term_t term);

#endif /* _JURASSIC_H */
