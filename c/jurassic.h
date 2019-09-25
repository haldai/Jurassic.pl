#ifndef _JURASSIC_H
#define _JURASSIC_H

#include <SWI-Prolog.h>
#include <SWI-Stream.h>
#include <julia.h>

//#define JURASSIC_DEBUG

#define BUFFSIZE 1024

#define JURASSIC_SUCCESS 1
#define JURASSIC_FAIL 0
#define JURASSIC_TYPE_ERR -1
#define JURASSIC_CALL_ERR -2

int list_length(term_t list);
jl_expr_t *jl_dotname(const char *dotname);
int jl_set_args(jl_expr_t **ex, term_t expr, size_t arity, size_t start_jl, size_t start_pl);
jl_expr_t *jl_exprn(jl_sym_t *head, size_t n);
int checked_eval_string(const char *code, jl_value_t **ret);
jl_value_t * checked_send_command_str(const char *code);
void checked_jl_command(const char *code);
int jl_is_defined(const char *var);
int jl_access_var(const char *var, jl_value_t **ret);
int jl_assign_var(const char *var, jl_value_t *val);
int atom_to_jl(atom_t atom, jl_value_t **ret, int flag_sym); /* When an atom is a defined Julia variable,
                                                                the flag_sym determines whether to return
                                                                its symbol or its value */
int list_to_jl(term_t list, int length, jl_array_t **ret, int flag_sym); /* Requires "flag_sym" because it
                                                                            calls atom_to_jl */
jl_expr_t * compound_to_jl_expr(term_t expr); /* return the expression as an Julia Expr. TODO: GC issues? */
int pl2jl(term_t term, jl_value_t **ret, int flag_sym); /* Requires "flag_sym" because it calls atom_to_jl */
int jl_unify_pl(jl_value_t *val, term_t *ret);

install_t install_jurassic(void);
foreign_t jl_eval_str(term_t jl_expr, term_t pl_ret);
foreign_t jl_eval(term_t jl_expr, term_t pl_ret);
foreign_t jl_send_command_str(term_t jl_expr);
foreign_t jl_send_command(term_t jl_expr);
foreign_t jl_using(term_t term);
foreign_t jl_include(term_t term);

/* TODO: most of data should only be regarded as jl_value_t,
   which is just a pointer to julia stack. So GC is important. */
#endif /* _JURASSIC_H */
