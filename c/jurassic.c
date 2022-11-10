#include <stdio.h>
#include <string.h>
#include <dlfcn.h>

#include "jurassic.h"
#include <julia_gcext.h>

/* atoms for julia expressions */
static functor_t FUNCTOR_dot2; /* subfields */
static functor_t FUNCTOR_quote1; /* quotesymbol : */
static functor_t FUNCTOR_quotenode1; /* quotenode :(:) */
static functor_t FUNCTOR_cmd1; /* julia command string */
static functor_t FUNCTOR_inline2; /* inline functions */
static functor_t FUNCTOR_field2; /* var.field */
static functor_t FUNCTOR_macro1; /* macro */
static functor_t FUNCTOR_tuple1; /* use tuple/1 to represent julia tuple */
static functor_t FUNCTOR_equal2; /* assignment */
static functor_t FUNCTOR_plusequal2; /* += */
static functor_t FUNCTOR_minusqual2; /* -= */
static functor_t FUNCTOR_timesequal2; /* *= */
static functor_t FUNCTOR_dividesequal2; /* /= */
static functor_t FUNCTOR_powerequal2; /* ^= */
static functor_t FUNCTOR_expr2; /* jl_expr(head, args) make a julia expression for meta-programming*/
static atom_t ATOM_true;
static atom_t ATOM_false;
static atom_t ATOM_nan;
static atom_t ATOM_nothing;
static atom_t ATOM_missing;
static atom_t ATOM_inf;
static atom_t ATOM_ninf; /* negative infinity */

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
   static functions
   - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
/* Print Julia exceptions */
static void jl_throw_exception() {
  jl_call2(jl_get_function(jl_base_module, "showerror"),
           jl_stderr_obj(),
           jl_exception_occurred());
  jl_printf(jl_stderr_stream(), "\n");
}

/* Adapted from Julia source */
static jl_value_t *jl_eval_global_var(jl_module_t *m, jl_sym_t *e) {
  jl_value_t *v = jl_get_global(m, e);
  if (v == NULL)
    jl_undefined_var_error(e);
  return v;
}

/* Adapted from Julia source */
static jl_value_t *jl_eval_dot_expr(jl_module_t *m, jl_value_t *x, jl_value_t *f) {
  jl_task_t *ct = jl_current_task;
  jl_value_t **args;
  JL_GC_PUSHARGS(args, 3);
  args[1] = jl_toplevel_eval_in(m, x);
  args[2] = jl_toplevel_eval_in(m, f);
#ifdef JURASSIC_DEBUG
  jl_printf(jl_stdout_stream(), "        -- value: ");
  jl_static_show(jl_stdout_stream(), args[1]);
  jl_printf(jl_stdout_stream(), "\n");
  jl_printf(jl_stdout_stream(), "        -- field: ");
  jl_static_show(jl_stdout_stream(), args[2]);
  jl_printf(jl_stdout_stream(), "\n");
#endif
  if (jl_is_module(args[1])) {
    JL_TYPECHK(getfield, symbol, args[2]);
    args[0] = jl_eval_global_var((jl_module_t*)args[1], (jl_sym_t*)args[2]);
  }
  else {
    args[0] = jl_eval_global_var(jl_base_relative_to(m), jl_symbol("getproperty"));
    size_t last_age = ct->world_age;
    ct->world_age = jl_get_world_counter();
    args[0] = jl_apply(args, 3);
    ct->world_age = last_age;
  }
  JL_GC_POP();
  return args[0];
}

/* Adapted from Julia source */
jl_expr_t *jl_exprn(jl_sym_t *head, size_t n) {
  jl_task_t *ct = jl_current_task;
  jl_array_t *ar = jl_alloc_vec_any(n);
  JL_GC_PUSH1(&ar);
  jl_expr_t *ex = (jl_expr_t*)jl_gc_alloc_typed(ct->ptls, sizeof(jl_expr_t),
                                                jl_expr_type);
  ex->head = head;
  ex->args = ar;
  JL_GC_POP();
  return ex;
}

/* Borrowed from real */
static int list_length(term_t list) {
  term_t tail = PL_new_term_ref();
  size_t len = -1;
  switch(PL_skip_list(list, tail, &len)) {
  case PL_LIST:
    return len;
  case PL_PARTIAL_LIST:
    PL_instantiation_error(list);
    return -1;
  case PL_CYCLIC_TERM:
  case PL_NOT_A_LIST:
    PL_type_error("list", list);
    return -1;
  default:
    assert(0);
  }
}

/* test if quote symbol x and y are paird */
static int quote_pair(char x, char y) {
  if (x == '\'' && y == '\'')
    return 1;
  else if (x == '\"' && y == '\"')
    return 1;
  else if (x == '(' && y == ')')
    return 1;
  else
    return 0;
}

/* Evaluate Julia string (from julia/src/embedding.c) with checking,
   return to a pre-assigned address */
static int checked_eval_string(const char *code, jl_value_t **ret) {
  *ret = jl_eval_string(code);
  if (jl_exception_occurred()) {
    // none of these allocate, so a gc-root (JL_GC_PUSH) is not necessary
    jl_call2(jl_get_function(jl_base_module, "showerror"),
             jl_stderr_obj(),
             jl_exception_occurred());
    jl_printf(jl_stderr_stream(), "\n");
    *ret = NULL;
    return JURASSIC_FAIL;
  }
  assert(*ret && "Missing return value but no exception occurred!");
  return JURASSIC_SUCCESS;
}
/* Evaluate Julia string with return value */
static jl_value_t *checked_send_command_str(const char *code) {
  jl_value_t *ret = jl_eval_string(code);
  if (jl_exception_occurred()) {
    // none of these allocate, so a gc-root (JL_GC_PUSH) is not necessary
    jl_call2(jl_get_function(jl_base_module, "showerror"),
             jl_stderr_obj(),
             jl_exception_occurred());
    jl_printf(jl_stderr_stream(), "\n");
    return NULL;
  }
  assert(ret && "Missing return value but no exception occurred!");
  return ret;
}

/* Evaluate Julia code without return */
static int checked_jl_command(const char *code) {
  jl_eval_string(code);
  if (jl_exception_occurred()) {
    // none of these allocate, so a gc-root (JL_GC_PUSH) is not necessary
    jl_call2(jl_get_function(jl_base_module, "showerror"),
             jl_stderr_obj(),
             jl_exception_occurred());
    jl_printf(jl_stderr_stream(), "\n");
    return JURASSIC_FAIL;
  }
  return JURASSIC_SUCCESS;
}

/* Check if a variable (atom string) is defined in julia */
static int jl_is_defined(const char *var) {
  return jl_get_global(jl_main_module, jl_symbol_lookup(var)) != NULL ? TRUE : FALSE;
}

/* Get julia variable from string */
static int jl_access_var(const char *var, jl_value_t **ret) {
  if (jl_is_defined(var)) {
    JL_TRY {
      *ret = jl_get_global(jl_main_module, jl_symbol_lookup(var));
      jl_exception_clear();
    } JL_CATCH {
      jl_task_t *ct = jl_current_task;
      jl_current_task->ptls->previous_exception = jl_current_exception();
      jl_throw_exception();
      *ret = NULL;
      return JURASSIC_FAIL;
    }
  } else
    return JURASSIC_FAIL;
  return JURASSIC_SUCCESS;
}

/* Get julia variable from string */
static jl_value_t *jl_get_var(const char *var) {
  if (jl_is_defined(var)) {
    JL_TRY {
      return jl_get_global(jl_main_module, jl_symbol_lookup(var));
      jl_exception_clear();
    } JL_CATCH {
      jl_task_t *ct = jl_current_task;
      jl_current_task->ptls->previous_exception = jl_current_exception();
      jl_throw_exception();
      return NULL;
    }
  } else
    return NULL;
}


/* Variable assignment */
static int jl_assign_var(const char *var, jl_value_t *val) {
  JL_TRY {
    jl_set_global(jl_main_module, jl_symbol(var), val);
    jl_exception_clear();
  } JL_CATCH {
    jl_task_t *ct = jl_current_task;
    jl_current_task->ptls->previous_exception = jl_current_exception();
    jl_throw_exception();
    return JURASSIC_FAIL;
  }
  return JURASSIC_SUCCESS;
}

/* Assign Julia expression arguments with Prolog list */
static int list_to_expr_args(term_t list, jl_expr_t **ex, size_t start, size_t len, int quotenode) {
  term_t arg_term = PL_new_term_ref();
  term_t list_ = PL_copy_term_ref(list);
  size_t i = start;
  while (PL_get_list(list_, arg_term, list_) && i < start + len) {
    JL_TRY {
#ifdef JURASSIC_DEBUG
      printf("----    Argument %lu: ", i);
      char *str_arg;
      if (!PL_get_chars(arg_term, &str_arg,
                        CVT_WRITE|CVT_EXCEPTION|BUF_STACK|REP_UTF8))
        return JURASSIC_FAIL;
      printf("%s.\n", str_arg);
#endif
      jl_expr_t *a_i;
      if (!quotenode)
        a_i = compound_to_jl_expr(arg_term);
      else
        a_i = (jl_expr_t *) jl_new_struct(jl_quotenode_type, compound_to_jl_expr(arg_term));
      if (a_i == NULL) {
        printf("[ERR] Convert term argument %lu failed!\n", i);
        return JURASSIC_FAIL;
      }
      jl_exprargset(*ex, i, a_i);
      i++;
      jl_exception_clear();
    } JL_CATCH {
      jl_task_t *ct = jl_current_task;
      jl_current_task->ptls->previous_exception = jl_current_exception();
      jl_throw_exception();
      return JURASSIC_FAIL;
    }
  }
  return JURASSIC_SUCCESS;
}

/* Process 'A1.A2.A3' Atom */
static jl_value_t *jl_dot(const char *dotname) {
  char *dot = strrchr(dotname, '.');
  jl_value_t *re;
  if (dot == NULL) {
    /* change some operators */
    if (strcmp(dotname, "=<") == 0) {
      re = (jl_value_t *) jl_symbol("<=");
    } else if (strcmp(dotname, "\\=") == 0) {
      re = (jl_value_t *) jl_symbol("!=");
    } else
      return (jl_value_t *) jl_symbol(dotname);
  } else {
    /* if dotname is an operator, such as .+, .-, .*, etc. */
    if (jl_is_operator((char *) dotname))
      return (jl_value_t *) jl_symbol(dotname);
    /* if dotname is Mod.fn, translate to Expr(:Mod, QuoteNode(:fn)) */
    JL_TRY {
      /* Module name */
      size_t mod_len = (dot - dotname)/sizeof(char);
      char module[mod_len + 1];
      strncpy(module, dotname, mod_len);
      module[mod_len] = '\0';
      /* QuoteNode(function) */
      jl_value_t *mod = jl_dot(module);
      if (!mod)
        return NULL;
      re = jl_eval_dot_expr(jl_main_module,
                            mod,
                            jl_new_struct(jl_quotenode_type, jl_symbol(++dot)));
    } JL_CATCH {
      jl_task_t *ct = jl_current_task;
      jl_current_task->ptls->previous_exception = jl_current_exception();
      jl_throw_exception();
      return NULL;
    }
  }
  return re;
}

/* Julia arguments start from 0, Prolog term arguments start from 1 */
static int jl_set_arg(jl_expr_t **ex, size_t idx, term_t term) {
  JL_TRY {
    jl_expr_t *arg = compound_to_jl_expr(term);
    if (arg == NULL) {
      printf("[ERR] Convert Prolog term argument %lu failed!\n", idx);
      return JURASSIC_FAIL;
    }
    jl_exprargset(*ex, idx, arg);
    jl_gc_wb(*ex, arg); // for safety
    jl_exception_clear();
  } JL_CATCH {
    jl_task_t *ct = jl_current_task;
    jl_current_task->ptls->previous_exception = jl_current_exception();
    jl_throw_exception();
    return JURASSIC_FAIL;
  }
  return JURASSIC_SUCCESS;
}

/* Julia expression set a Julia argument */
static int jl_set_jl_arg(jl_expr_t **ex, size_t idx, jl_value_t *arg) {
  JL_TRY {
    if (arg == NULL) {
      printf("[ERR] Convert Prolog term argument %lu failed!\n", idx);
      return JURASSIC_FAIL;
    }
    jl_exprargset(*ex, idx, arg);
    jl_gc_wb(*ex, arg); // for safety
    jl_exception_clear();
  } JL_CATCH {
    jl_task_t *ct = jl_current_task;
    jl_current_task->ptls->previous_exception = jl_current_exception();
    jl_throw_exception();
    return JURASSIC_FAIL;
  }
  return JURASSIC_SUCCESS;
}

/* unify the idx-th element in tuple */
static int jl_tuple_ref_unify(term_t *pl_term, jl_value_t *val, size_t idx) {
  size_t n = jl_nfields(val);
  jl_value_t * v = jl_get_nth_field_checked(val, idx);
#ifdef JURASSIC_DEBUG
  jl_printf(jl_stdout_stream(), "[DEBUG] item: %lu/%lu\n", idx, n);
  jl_static_show(jl_stdout_stream(), (jl_value_t *) v);
  jl_printf(jl_stdout_stream(), "\n");
#endif
  if (PL_is_atom(*pl_term) && PL_is_ground(*pl_term)) {
    // assignment
      char *atom;
      if (!PL_get_chars(*pl_term, &atom,
                        CVT_ATOM|CVT_STRING|CVT_EXCEPTION|BUF_STACK|REP_UTF8))
        PL_fail;
      return jl_assign_var(atom, v);
  } else
    return jl_unify_pl(v, pl_term, 1);
  return JURASSIC_FAIL;
}

/* unify prolog tuple([A|B]) with julia functions that returns a tuple */
static int jl_tuple_unify_all(term_t *pl_tuple, jl_value_t *val) {
  if (PL_is_functor(*pl_tuple, FUNCTOR_tuple1)) {
    term_t list = PL_new_term_ref();
    if (!PL_unify_arg(1, *pl_tuple, list)) {
      printf("[ERR] Cannot access tuple arguments!\n");
      return JURASSIC_FAIL;
    }
    if (!jl_is_tuple(val)) {
#ifdef JURASSIC_DEBUG
      printf("        Julia expression doesn't return tuple.\n");
#endif
      return JURASSIC_FAIL;
    }
    size_t nargs = jl_nfields(val);
    if (nargs == 0) {
#ifdef JURASSIC_DEBUG
      printf("       Empty tuple: ()\n");
#endif
      if (!PL_unify_nil(list)) {
        return JURASSIC_FAIL;
      } else
        return JURASSIC_SUCCESS;
    }
#ifdef JURASSIC_DEBUG
    printf("        Unify tuple/%lu.\n", nargs);
    jl_printf(jl_stdout_stream(), "[DEBUG] Tuple value:\n");
    jl_static_show(jl_stdout_stream(), (jl_value_t *) val);
    jl_printf(jl_stdout_stream(), "\n");
#endif
    term_t head = PL_new_term_ref();
    term_t l = PL_copy_term_ref(list);
    for (size_t i = 0; i < nargs; i++) {
      if (!PL_unify_list(l, head, l) || !jl_tuple_ref_unify(&head, val, i))
        return JURASSIC_FAIL;
    }
    return PL_unify_nil(l);
  } else
    return JURASSIC_FAIL;
}

/* Julia linenumbernode with none in it */
static jl_value_t *jl_linenumbernode_none(size_t n) {
  return jl_new_struct(jl_linenumbernode_type, jl_box_long(n), jl_nothing);
}

/* Compile expression list into an array of Expr separated by LineNumberNode */
static int expr_list_to_func_lines(term_t list, jl_expr_t **ret) {
  jl_exprargset(*ret, 0, jl_linenumbernode_none(0)); // first line

  term_t head = PL_new_term_ref();
  term_t term = PL_copy_term_ref(list);

  size_t i = 1;
  size_t j = 1;
  // set arguments
  while (PL_get_list(term, head, term)) {
    jl_exprargset(*ret, i, jl_linenumbernode_none(j)); // linenumber node n
    jl_expr_t *arg_expr = compound_to_jl_expr(head);
#ifdef JURASSIC_DEBUG
    jl_static_show(jl_stdout_stream(), (jl_value_t *) arg_expr);
    jl_printf(jl_stdout_stream(), "\t");
    jl_static_show(jl_stdout_stream(), (jl_value_t *) jl_typeof(arg_expr));
    jl_printf(jl_stdout_stream(), "\n");
#endif
    if (PL_is_atomic(head)) {
      atom_t atom;
      if (!PL_get_atom(head, &atom))
        return JURASSIC_FAIL;
      const char *a = PL_atom_chars(atom);
      if (jl_is_defined(a)) {
        jl_exprargset(*ret, i+1, jl_get_var(a)); // code
      }
    } else
      jl_exprargset(*ret, i+1, (jl_value_t *) compound_to_jl_expr(head)); // code
    i+=2;
    j++;
  }
  if (jl_exception_occurred()) {
    // none of these allocate, so a gc-root (JL_GC_PUSH) is not necessary
    jl_call2(jl_get_function(jl_base_module, "showerror"),
             jl_stderr_obj(),
             jl_exception_occurred());
    jl_printf(jl_stderr_stream(), "\n");
    return JURASSIC_FAIL;
  }
  return JURASSIC_SUCCESS;
}

static int expr_array_to_func_lines(jl_array_t *array, int len, jl_expr_t **ret) {
  jl_exprargset(*ret, 0, jl_linenumbernode_none(0)); // first line
  size_t i = 1;
  size_t j = 1;
  while (j <= len) {
    jl_exprargset(*ret, i, jl_linenumbernode_none(j)); // linenumber node n
    jl_exprargset(*ret, i+1, (jl_value_t *) jl_arrayref(array, j-1)); // code
    i+=2;
    j++;
  }
  if (jl_exception_occurred()) {
    // none of these allocate, so a gc-root (JL_GC_PUSH) is not necessary
    jl_call2(jl_get_function(jl_base_module, "showerror"),
             jl_stderr_obj(),
             jl_exception_occurred());
    jl_printf(jl_stderr_stream(), "\n");
    return JURASSIC_FAIL;
  }
  return JURASSIC_SUCCESS;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
   Dynamic functions
   - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
/* Prolog atoms to Julia values */
int atom_to_jl(atom_t atom, jl_value_t **ret, int flag_sym) {
  const char *a = PL_atom_chars(atom);
  if (a == NULL) {
#ifdef JURASSIC_DEBUG
    printf("Reading string from atom failed!\n");
#endif
    *ret = NULL;
    return JURASSIC_FAIL;
  }
  JL_TRY {
    /* if the atom is julia keywords*/
    if (atom == ATOM_true) {
#ifdef JURASSIC_DEBUG
      printf("boolean: true.\n");
#endif
      *ret = jl_true;
    } else if (atom == ATOM_false) {
#ifdef JURASSIC_DEBUG
      printf("boolean: false.\n");
#endif
      *ret = jl_false;
    } else if (atom == ATOM_nothing) {
#ifdef JURASSIC_DEBUG
      printf("Nothing.\n");
      jl_static_show(jl_stdout_stream(), jl_get_global(jl_main_module, jl_symbol_lookup(a)));
      jl_printf(jl_stdout_stream(), "\n");
#endif
      *ret = jl_nothing;
    } else if (atom == ATOM_missing) {
#ifdef JURASSIC_DEBUG
      printf("Missing.\n");
      jl_static_show(jl_stdout_stream(), jl_get_global(jl_main_module, jl_symbol_lookup(a)));
      jl_printf(jl_stdout_stream(), "\n");
#endif
      *ret = jl_eval_string("missing");
    }  else if (atom == ATOM_nan) {
#ifdef JURASSIC_DEBUG
      printf("NaN.\n");
      jl_static_show(jl_stdout_stream(), jl_get_global(jl_main_module, jl_symbol_lookup(a)));
      jl_printf(jl_stdout_stream(), "\n");
#endif
      *ret = jl_box_float64(D_PNAN);
    } else if (atom == ATOM_inf) {
#ifdef JURASSIC_DEBUG
      printf("Inf.\n");
      jl_static_show(jl_stdout_stream(), jl_get_global(jl_main_module, jl_symbol_lookup(a)));
      jl_printf(jl_stdout_stream(), "\n");
#endif
      *ret = jl_box_float64(D_PINF);
    } else if (atom == ATOM_ninf) {
#ifdef JURASSIC_DEBUG
      printf("negative Inf.\n");
      jl_static_show(jl_stdout_stream(), jl_get_global(jl_main_module, jl_symbol_lookup(a)));
      jl_printf(jl_stdout_stream(), "\n");
#endif
      *ret = jl_box_float64(D_NINF);
    } else if (jl_is_defined(a) && !flag_sym) {
      /* get the variable assignment according to name */
#ifdef JURASSIC_DEBUG
      printf("defined Julia variable.\n");
      jl_static_show(jl_stdout_stream(), jl_get_global(jl_main_module, jl_symbol_lookup(a)));
      jl_printf(jl_stdout_stream(), "\n");
#endif
      return jl_access_var(a, ret);
    } else if (strchr(a, '.') != NULL){
      /* Expression A1.A2 */
#ifdef JURASSIC_DEBUG
      printf("dot symbol.\n");
#endif
      *ret = jl_dot(a);
      if (!ret)
        return JURASSIC_FAIL;
    } else { /* default as Symbol */
#ifdef JURASSIC_DEBUG
      printf("Fallback to Symbol.\n");
#endif
      *ret = (jl_value_t *) jl_symbol(a);
    }
    jl_exception_clear();
  } JL_CATCH {
    jl_task_t *ct = jl_current_task;
    jl_current_task->ptls->previous_exception = jl_current_exception();
    *ret = NULL;
    return JURASSIC_FAIL;
  }
  return JURASSIC_SUCCESS;
}

/* convert prolog term to julia expression */
jl_expr_t *compound_to_jl_expr(term_t expr) {
#ifdef JURASSIC_DEBUG
  char *str_expr;
  if (!PL_get_chars(expr, &str_expr,
                    CVT_WRITE|CVT_EXCEPTION|BUF_STACK|REP_UTF8))
    return NULL;
  printf("[DEBUG] Parse expression: %s\n", str_expr);
#endif
  /* Treat everthing as Symbols */
  atom_t functor;
  size_t arity;
  if (PL_is_list(expr)) {
    /* is list */
    size_t len = list_length(expr);
#ifdef JURASSIC_DEBUG
    printf("        Functor: vect/%lu.\n", len);
#endif
    /* use :vect as head, list members as arguments */
    jl_expr_t *ex = jl_exprn(jl_symbol("vect"), len);
    if (!list_to_expr_args(expr, &ex, 0, len, 0))
      return NULL;
#ifdef JURASSIC_DEBUG
    jl_static_show(jl_stdout_stream(), (jl_value_t *) ex);
    jl_printf(jl_stdout_stream(), "\n");
#endif
    return ex;
  } else if (!PL_is_compound(expr)) {
    jl_value_t *ret;
    if (!pl_to_jl(expr, &ret, TRUE))
      return NULL;
    else
      return (jl_expr_t *) ret;
  } else if (!PL_get_compound_name_arity_sz(expr, &functor, &arity)) {
#ifdef JURASSIC_DEBUG
    printf("[DEBUG] Cannot analyse compound!\n");
#endif
    return NULL;
  }
  const char *fname = PL_atom_chars(functor);
  if (fname == NULL || strlen(fname) == 0) {
    printf("[ERR] Read functor name failed!\n");
    return NULL;
  } else if (PL_is_functor(expr, FUNCTOR_field2) && arity == 2) {
#ifdef JURASSIC_DEBUG
    printf("        Field of variable:\n");
#endif
    // field symbol should be quotenode!!
    jl_expr_t *ex = jl_exprn((jl_sym_t *) jl_symbol("."), arity);
    JL_GC_PUSH1(&ex);
    /* assign the two arguments */

    term_t arg1_term = PL_new_term_ref();
    term_t arg2_term = PL_new_term_ref();
    if (!PL_get_arg(1, expr, arg1_term) || !PL_get_arg(2, expr, arg2_term)) {
      printf("[ERR] Get field term arguments failed!\n");
      JL_GC_POP();
      return NULL;
    }
#ifdef JURASSIC_DEBUG
    char *str_arg1, *str_arg2;
    if (!PL_get_chars(arg1_term, &str_arg1,
                      CVT_WRITE|CVT_EXCEPTION|BUF_STACK|REP_UTF8) ||
        !PL_get_chars(arg2_term, &str_arg2,
                      CVT_WRITE|CVT_EXCEPTION|BUF_STACK|REP_UTF8))
      return JURASSIC_FAIL;
    printf("------- %s.%s\n", str_arg1, str_arg2);
#endif
    jl_exprargset(ex, 0, compound_to_jl_expr(arg1_term));
    jl_exprargset(ex, 1, (jl_expr_t *) jl_new_struct(jl_quotenode_type, compound_to_jl_expr(arg2_term)));
#ifdef JURASSIC_DEBUG
    jl_static_show(jl_stdout_stream(), (jl_value_t *) ex);
    jl_printf(jl_stdout_stream(), "\n");
#endif
    JL_GC_POP();
    return ex;
  } else if (PL_is_functor(expr, FUNCTOR_cmd1) && arity < 2) {
    term_t cmd = PL_new_term_ref();
    if (!PL_get_arg(1, expr, cmd)) {
      printf("[ERR] Cannot access command term!\n");
      return NULL;
    }
    if (!PL_is_string(cmd)) {
      printf("[ERR] Command should be a string!\n");
      return NULL;
    }
    char *cmd_str;
    if (!PL_get_chars(cmd, &cmd_str,
                      CVT_ATOM|CVT_STRING|BUF_STACK|CVT_EXCEPTION|REP_UTF8)) {
      printf("[ERR] Reading command string failed!\n");
      return NULL;
    }
#ifdef JURASSIC_DEBUG
    printf("        Command string: %s.\n", cmd_str);
#endif
    return (jl_expr_t *) checked_send_command_str(cmd_str);
  } else if (PL_is_functor(expr, FUNCTOR_quote1) && arity < 2) {
    term_t expr_arg = PL_new_term_ref();
    if (!PL_get_arg(1, expr, expr_arg)) {
      printf("[ERR] Reading quoted symbol failed!\n");
      return NULL;
    }
    if (PL_is_compound(expr_arg)) {
      // QuoteNode?
      return (jl_expr_t *) jl_new_struct(jl_quotenode_type, compound_to_jl_expr(expr_arg));
    } else if (PL_is_atom(expr_arg))
      return (jl_expr_t *) compound_to_sym(expr);
  } else if (PL_is_functor(expr, FUNCTOR_quotenode1) && arity < 2) {
    // fetch the argument
    term_t arg_term = PL_new_term_ref();
    if (!PL_get_arg(1, expr, arg_term)) {
      printf("[ERR] Get field term argument of quotenode failed!\n");
      return NULL;
    }
#ifdef JURASSIC_DEBUG
    char *str_arg;
    if (!PL_get_chars(arg_term, &str_arg,
                      CVT_WRITE|CVT_EXCEPTION|BUF_STACK|REP_UTF8)) {
      printf(" [ERR] QuoteNode: Wrong argument!\n");
      return NULL;
    }
    printf("        QuoteNode: %s.\n", str_arg);
#endif
    return (jl_expr_t *) jl_new_struct(jl_quotenode_type, compound_to_jl_expr(arg_term));
  } else if (PL_is_functor(expr, FUNCTOR_expr2) && arity == 2) {
    // make an expression (:call, :Expr, QuoteNode_pred, QuoteNode_args)
    jl_expr_t *ex;

    // expr pred
    term_t expr_pred = PL_new_term_ref();
    if (!PL_get_arg(1, expr, expr_pred)) {
      printf("[ERR] Cannot access the first argument!\n");
      return NULL;
    }
    // expr args
    term_t expr_args = PL_new_term_ref();
    if (!PL_get_arg(2, expr, expr_args)) {
      printf("[ERR] Cannot access the second argument as a list!\n");
      return NULL;
    }
    size_t len = list_length(expr_args);
#ifdef JURASSIC_DEBUG
    printf("        Functor: Expression with %lu args.\n", len);
#endif
    // head Symbol
    jl_sym_t *head = compound_to_sym(expr_pred);
    JL_GC_PUSH1(&head);
    if (!head) {
      printf("[ERR] head of Expr is not a symbol!\n");
      JL_GC_POP();
      return NULL;
    }
    {
      ex = jl_exprn(head, len);
      JL_GC_PUSH1(&ex);
      if (!list_to_expr_args(expr_args, &ex, 0, len, 0)) {
        JL_GC_POP();
        JL_GC_POP();
        return NULL;
      }
      JL_GC_POP();
    }
    JL_GC_POP();
    // return (jl_expr_t *) jl_new_struct(jl_quotenode_type, ex);
    return ex;
  } else if (PL_is_functor(expr, FUNCTOR_macro1) && arity == 1) {
    /* macro calls */
    term_t macro_call = PL_new_term_ref();
    if (!PL_get_arg(1, expr, macro_call)) {
      printf("[ERR] Cannot access macro calling term!\n");
      return NULL;
    }
    atom_t macro;
    size_t macro_arity;
    if (!PL_get_compound_name_arity_sz(macro_call, &macro, &macro_arity)) {
#ifdef JURASSIC_DEBUG
      printf("[DEBUG] Cannot analyse macro call!\n");
#endif
      return NULL;
    }
    const char *macro_name_ = PL_atom_chars(macro);
    char *macro_name = (char *) calloc(1 + strlen(macro_name_) + 1, sizeof(char));
    strcpy(macro_name, "@");
    strcat(macro_name, macro_name_);
#ifdef JURASSIC_DEBUG
    printf("        Macro: %s/%lu.\n", macro_name, macro_arity);
#endif
    term_t arg = PL_new_term_ref();
    if (!PL_get_arg(1, macro_call, arg)) {
      printf("[ERR] Cannot access macro argument!\n");
      free(macro_name);
      return NULL;
    }
    jl_expr_t *ex = jl_exprn(jl_symbol("macrocall"), arity + 2);
    JL_GC_PUSH1(&ex);
    jl_value_t *func = jl_dot(macro_name);
    free(macro_name); // free calloc
    if (!func)
      return NULL;
    jl_exprargset(ex, 0, func);
    jl_exprargset(ex, 1, jl_linenumbernode_none(0));
    if (!jl_set_args(&ex, macro_call, arity, 2, 1)) {
      JL_GC_POP();
      return NULL;
    }
#ifdef JURASSIC_DEBUG
    jl_static_show(jl_stdout_stream(), (jl_value_t *) ex);
    jl_printf(jl_stdout_stream(), "\n");
#endif
    JL_GC_POP();
    return ex;
  } else if (strcmp(fname, "[]") == 0) {
    /* reference in array */
    /* term like a[i,j] =.. [[], [i,j], a]. use :ref function */
    term_t collection = PL_new_term_ref();
    term_t list = PL_new_term_ref();
    if (arity == 1) {
#ifdef JURASSIC_DEBUG
      jl_printf(jl_stdout_stream(), "Array initialisation.");
#endif
      if (!PL_get_arg(1, expr, collection)) {
        printf("[ERR] Cannot access reference arguments!\n");
        return NULL;
      } else {
        jl_expr_t *ex = jl_exprn(jl_symbol("ref"), 1);
        jl_exprargset(ex, 0, compound_to_jl_expr(collection));
#ifdef JURASSIC_DEBUG
        jl_static_show(jl_stdout_stream(), (jl_value_t *) ex);
        jl_printf(jl_stdout_stream(), "\n");
#endif
        return ex;
      }
    } else {
      if (!PL_get_arg(1, expr, list) || !PL_get_arg(2, expr, collection)) {
        printf("[ERR] Cannot access reference arguments!\n");
        return NULL;
      }
      size_t len = list_length(list);
#ifdef JURASSIC_DEBUG
      printf("        Functor: ref/%lu.\n", len);
#endif
      /* use :ref as head, list members as arguments */
      /* a[1,2,3] =.. (:ref, :a, 1, 2, 3)*/
      jl_expr_t *ex = strcmp(fname, "[]") == 0 ?
        jl_exprn(jl_symbol("ref"), len+1) : jl_exprn(jl_symbol("curly"), len+1);
      JL_GC_PUSH1(&ex);
      jl_exprargset(ex, 0, compound_to_jl_expr(collection)); /* first argument is the collection */
      /* following arguments are references */
      if (!list_to_expr_args(list, &ex, 1, len, 0)) {
        JL_GC_POP();
        return NULL;
      }
#ifdef JURASSIC_DEBUG
      jl_static_show(jl_stdout_stream(), (jl_value_t *) ex);
      jl_printf(jl_stdout_stream(), "\n");
#endif
      JL_GC_POP();
      return ex;
    }
  } else if (PL_is_functor(expr, FUNCTOR_tuple1) && arity == 1) {
    term_t list = PL_new_term_ref();
    if (!PL_get_arg(1, expr, list)) {
      printf("[ERR] Cannot access tuple arguments!\n");
      return NULL;
    }
    size_t len = list_length(list);
#ifdef JURASSIC_DEBUG
    printf("        Functor: tuple/%lu.\n", len);
#endif
    /* use :tuple as head, list members as arguments */
    jl_expr_t *ex = jl_exprn(jl_symbol("tuple"), len);
    JL_GC_PUSH1(&ex);
    if (!list_to_expr_args(list, &ex, 0, len, 0)) {
      JL_GC_POP();
      return NULL;
    }
#ifdef JURASSIC_DEBUG
    jl_static_show(jl_stdout_stream(), (jl_value_t *) ex);
    jl_printf(jl_stdout_stream(), "\n");
#endif
    JL_GC_POP();
    return ex;
  } else if (PL_is_functor(expr, FUNCTOR_inline2) && arity == 2) {
#ifdef JURASSIC_DEBUG
    printf("        Functor: ->/2.\n");
#endif
    jl_expr_t *ex = jl_exprn(jl_symbol("->"), 2);
    JL_GC_PUSH1(&ex);
    /* assign first argument */
    term_t arg = PL_new_term_ref();
    if (!PL_get_arg(1, expr, arg) || !jl_set_arg(&ex, 0, arg)) {
#ifdef JURASSIC_DEBUG
      printf("        Cannot get argument 1.\n");
#endif
      JL_GC_POP(); // POP ex
      return NULL;
    } else {
      /* assign second argument (:block, :quotenode, expr)*/
      jl_expr_t *ex2 = jl_exprn(jl_symbol("block"), 2);
      JL_GC_PUSH1(&ex2);
      if (!PL_get_arg(2, expr, arg)) {
#ifdef JURASSIC_DEBUG
        printf("        Cannot get argument 2.\n");
#endif
        JL_GC_POP(); // POP ex2
        JL_GC_POP(); // POP ex
        return NULL;
      } else {
        jl_set_jl_arg(&ex2, 0, jl_linenumbernode_none(0)); // linenumbernode
        if (!jl_set_arg(&ex2, 1, arg) || !jl_set_jl_arg(&ex, 1, (jl_value_t *) ex2)) {
          JL_GC_POP(); // POP ex2
          JL_GC_POP(); // POP ex
          return NULL;
        }
        JL_GC_POP();
      }
    }
#ifdef JURASSIC_DEBUG
    jl_static_show(jl_stdout_stream(), (jl_value_t *) ex);
    jl_printf(jl_stdout_stream(), "\n");
#endif
    JL_GC_POP(); // POP ex
    return ex;
  } else {
    /* functor to julia function symbol */
    if (arity == 0) {
      /* 0-argument function */
#ifdef JURASSIC_DEBUG
      printf("[DEBUG] 0-argument function: %s().\n", fname);
#endif
      jl_expr_t *ex = jl_exprn(jl_symbol("call"), 1);
      JL_GC_PUSH1(&ex);
      /* "XX.xx" has to be processed as Expr(XX, :(xx))*/
      jl_value_t *func = jl_dot(fname);
      if (!func) {
        JL_GC_POP();
        return NULL;
      }
      jl_exprargset(ex, 0, func);
      JL_GC_POP();
      return ex;
    } else {
      /* initialise an expression without using :call */
      if ( strcmp(fname, "=") == 0 ||
           strcmp(fname, "+=") == 0 ||
           strcmp(fname, "-=") == 0 ||
           strcmp(fname, "*=") == 0 ||
           strcmp(fname, "/=") == 0 ||
           strcmp(fname, "^=") == 0 ||
           strcmp(fname, "call") == 0 ||
           strcmp(fname, "kw") == 0 ||
           strcmp(fname, "...") == 0 ||
           strcmp(fname, "curly") == 0) {
        /* for these meta predicates, no need to add "call" as Expr.head */
#ifdef JURASSIC_DEBUG
        printf("        Functor (no call): %s/%lu.\n", fname, arity);
#endif
        jl_value_t *func = jl_dot(fname);
        if (!func)
          return NULL;
        jl_expr_t *ex = jl_exprn((jl_sym_t *) func, arity);
        JL_GC_PUSH1(&ex);
        /* assign arguments */
        if (!jl_set_args(&ex, expr, arity, 0, 1)) {
          JL_GC_POP();
          return NULL;
        }
#ifdef JURASSIC_DEBUG
        jl_static_show(jl_stdout_stream(), (jl_value_t *) ex);
        jl_printf(jl_stdout_stream(), "\n");
#endif
        JL_GC_POP();
        return ex;
      } else {
#ifdef JURASSIC_DEBUG
        printf("        Functor: call/%lu.\n", arity+1);
#endif
        /* use :call as Expr head */
        jl_expr_t *ex = jl_exprn(jl_symbol("call"), arity + 1);
        JL_GC_PUSH1(&ex);
        /* set fname as the first argument */
        jl_value_t *func = jl_dot(fname);
        if (!func)
          return NULL;
        jl_exprargset(ex, 0, func);
#ifdef JURASSIC_DEBUG
        printf("----    Argument 0: %s.\n", fname);
#endif
        /* other arguments */
        if (!jl_set_args(&ex, expr, arity, 1, 1)) {
          JL_GC_POP();
          return NULL;
        }
#ifdef JURASSIC_DEBUG
        jl_static_show(jl_stdout_stream(), (jl_value_t *) ex);
        jl_printf(jl_stdout_stream(), "\n");
#endif
        JL_GC_POP();
        return ex;
      }
    }
  }
  return NULL;
}

/* declare a julia function */
jl_expr_t *jl_function(term_t fname_pl, term_t fargs_pl, term_t fexprs_pl) {
#ifdef JURASSIC_DEBUG
  printf("[DEBUG] Constructing function:\n");
#endif
  /* First argument: function declaration (:call, fname, fargs...) */

  // parse fname_pl into Julia Symbol
  jl_sym_t *fname = PL_is_compound(fname_pl) ?
    compound_to_sym(fname_pl) : atomic_to_sym(fname_pl);
  if (!fname) {
#ifdef JURASSIC_DEBUG
    printf("[DEBUG] Function name:\n");
    jl_static_show(jl_stdout_stream(), (jl_value_t *) fname);
    jl_printf(jl_stdout_stream(), "\n");
#endif
    printf("[ERR] Function name type error (1)!\n");
    return NULL;
  }

  // parse the list of arguments
  int arg_len = list_length(fargs_pl);
  if (arg_len < 0) {
    printf("[ERR] Argument is not a list!\n");
    return NULL;
  }

  jl_expr_t *fdec = jl_exprn(jl_symbol("call"), arg_len+1);

  JL_GC_PUSH1(&fname); // push fname
  jl_exprargset(fdec, 0, fname); // first argument is the function name
  if (!list_to_expr_args(fargs_pl, &fdec, 1, arg_len, 0)) {
    printf("[ERR] Read argument list failed!\n");
    JL_GC_POP(); // pop fname
    return NULL;
  }
#ifdef JURASSIC_DEBUG
  jl_static_show(jl_stdout_stream(), (jl_value_t *) fdec);
  jl_printf(jl_stdout_stream(), "\n");
#endif
  /* Second argument: expressions with line number nodes */
  // if it is an atom (Julia variable of Array{Expr})
  int num_lines = 0;
  jl_expr_t *fcodes;
  int is_julia_var = 0;
  if (PL_is_atomic(fexprs_pl)) {
    jl_value_t *array_expr;
    if (!pl_to_jl(fexprs_pl, &array_expr, FALSE)) {
      printf("[ERR] Function lines error, cannot access the variable!\n");
      return NULL;
    } else {
      if (!jl_is_array(array_expr) || !jl_is_expr(jl_arrayref((jl_array_t *) array_expr, 0))) {
        printf("[ERR] Function lines error, variable is not an array of Expr!\n");
        return NULL;
      } else {
        is_julia_var = 1;
        JL_GC_PUSH1(&array_expr); // push array_expr
        num_lines = jl_array_len(array_expr);
        fcodes = jl_exprn(jl_symbol("block"), num_lines*2+1);
        if (!expr_array_to_func_lines((jl_array_t *) array_expr, num_lines, &fcodes)) {
          JL_GC_POP(); // pop fname
          return NULL;
        }
      }
    }
  } else if (PL_is_list(fexprs_pl)) {
    // if it is a list term
    // make the second argument for Julia operator function/2
    num_lines = list_length(fexprs_pl);
    fcodes = jl_exprn(jl_symbol("block"), num_lines*2+1);
    if (!expr_list_to_func_lines(fexprs_pl, &fcodes)) {
      JL_GC_POP(); // pop fname
      return NULL;
    }
  }

#ifdef JURASSIC_DEBUG
  jl_static_show(jl_stdout_stream(), (jl_value_t *) fcodes);
  jl_printf(jl_stdout_stream(), "\n");
#endif

  /* Construct the expression for function */
  jl_expr_t *f_expr = jl_exprn(jl_symbol("function"), 2);
  jl_set_jl_arg(&f_expr, 0, (jl_value_t *) fdec);
  jl_set_jl_arg(&f_expr, 1, (jl_value_t *) fcodes);
  if (is_julia_var) {
    JL_GC_POP(); // pop array_expr
  }
  JL_GC_POP(); // pop fname
  return f_expr;
}

int jl_set_args(jl_expr_t **ex, term_t expr, size_t arity, size_t start_jl, size_t start_pl) {
  term_t arg_term = PL_new_term_ref();
  for (size_t i = 0; i < arity; i++) { // Prolog argument index starts from 1
#ifdef JURASSIC_DEBUG
    printf("----    Argument %lu: ", i + start_jl);
#endif
    if (!PL_get_arg(i + start_pl, expr, arg_term)) {
      printf("[ERR] Get term argument %lu failed!\n", i + start_pl);
      return JURASSIC_FAIL;
    }
#ifdef JURASSIC_DEBUG
    char *str_arg;
    if (!PL_get_chars(arg_term, &str_arg,
                      CVT_WRITE|CVT_EXCEPTION|BUF_STACK|REP_UTF8))
      return JURASSIC_FAIL;
    printf("%s.\n", str_arg);
#endif
    if (!jl_set_arg(ex, i + start_jl, arg_term))
      return JURASSIC_FAIL;
  }
  return JURASSIC_SUCCESS;
}

int list_to_jl(term_t list, jl_array_t **ret, int flag_sym) {
  jl_value_t **arr_vals = (jl_value_t **) jl_array_data(*ret);

  term_t head = PL_new_term_ref();
  term_t term = PL_copy_term_ref(list);

  size_t i = 0;
  while (PL_get_list(term, head, term)) {
    if (!pl_to_jl(head, &arr_vals[i], flag_sym)) {
      *ret = NULL;
      return JURASSIC_FAIL;
    }
    jl_gc_wb(*ret, arr_vals[i]); // for safety
    i++;
  }
  if (jl_exception_occurred()) {
    // none of these allocate, so a gc-root (JL_GC_PUSH) is not necessary
    jl_call2(jl_get_function(jl_base_module, "showerror"),
             jl_stderr_obj(),
             jl_exception_occurred());
    jl_printf(jl_stderr_stream(), "\n");
    return JURASSIC_FAIL;
  }
  return JURASSIC_SUCCESS;
}

int pl_to_jl(term_t term, jl_value_t **ret, int flag_sym) {
#ifdef JURASSIC_DEBUG
  char *show;
  /* string, to string*/
  if (!PL_get_chars(term, &show,
                    CVT_WRITE|CVT_EXCEPTION|BUF_STACK|REP_UTF8))
    return JURASSIC_FAIL;
  printf("[DEBUG] term = %s\n", show);
#endif
  if (PL_is_variable(term))
#ifdef JURASSIC_DEBUG
    printf("        Unbound Variable!\n");
#endif
  *ret = NULL;
  switch (PL_term_type(term)) {
  case PL_ATOM: {
#ifdef JURASSIC_DEBUG
    printf("        Atom: ");
#endif
    atom_t atom;
    if (!PL_get_atom(term, &atom)) {
#ifdef JURASSIC_DEBUG
      printf("FAILED!\n");
#endif
      *ret = NULL;
      return JURASSIC_FAIL;
    }
    return atom_to_jl(atom, ret, flag_sym);
    break;
  }
  case PL_NIL:
    return checked_eval_string("[]", ret);
    break;
  case PL_STRING: {
    char *str;
#ifdef JURASSIC_DEBUG
    printf("        String: ");
#endif
    /* string, to string*/
    if (!PL_get_chars(term, &str,
                      CVT_ATOM|CVT_STRING|CVT_EXCEPTION|BUF_STACK|REP_UTF8)) {
      *ret = NULL;
#ifdef JURASSIC_DEBUG
      printf("FAILED!\n");
#endif
      return JURASSIC_FAIL;
    } else {
#ifdef JURASSIC_DEBUG
      printf("%s\n", str);
#endif
      *ret = jl_cstr_to_string(str);
    }
    break;
  }
  case PL_INTEGER: {
#ifdef JURASSIC_DEBUG
    printf("        Integer: ");
#endif
    int64_t num_int;
    /* integer, to int64 */
    if (!PL_get_int64(term, &num_int)) {
      *ret = NULL;
#ifdef JURASSIC_DEBUG
      printf("FAILED!\n");
#endif
      return JURASSIC_FAIL;
    } else {
#ifdef JURASSIC_DEBUG
      printf("%ld\n", num_int);
#endif
      *ret = jl_box_int64(num_int);
    }
    break;
  }
  case PL_FLOAT: {
#ifdef JURASSIC_DEBUG
    printf("        Float: ");
#endif
    double num_float;
    /* float, to double */
    if (!PL_get_float(term, &num_float)) {
#ifdef JURASSIC_DEBUG
      printf("FAILD!\n");
#endif
      *ret = NULL;
      return JURASSIC_FAIL;
    } else {
#ifdef JURASSIC_DEBUG
      printf("%f\n", num_float);
#endif
      *ret = jl_box_float64(num_float);
    }
    break;
  }
  case PL_RATIONAL: {
#ifdef JURASSIC_DEBUG
    printf("        Rational: ");
#endif
    mpq_t mpq; // use GMP rational numbers
    mpq_init(mpq);

    if (!PL_get_mpq(term, mpq)) {
#ifdef JURASSIC_DEBUG
      printf("FAILD!\n");
#endif
      *ret = NULL;
      mpq_clear(mpq);
      return JURASSIC_FAIL;
    } else {
      mpz_t numerator;
      mpz_t denominator;

      mpz_init(numerator);
      mpz_init(denominator);

      mpq_get_num(numerator, mpq);
      mpq_get_den(denominator, mpq);

      if (mpz_fits_slong_p(numerator) && mpz_fits_slong_p(denominator)) {
        const long num = mpz_get_si(numerator);
        const long denom = mpz_get_si(denominator);

#ifdef JURASSIC_DEBUG
        printf("        %d//%d\n", num, denom);
#endif
        /* convert to sexpr(:call, //, nom, denom) */
        jl_expr_t *expr = jl_exprn(jl_symbol("call"), 3);
        jl_exprargset(expr, 0, jl_symbol("//"));
        jl_exprargset(expr, 1, jl_box_int64(num));
        jl_exprargset(expr, 2, jl_box_int64(denom));

        /* convert to return value */
        JL_GC_PUSH1(&expr);
        *ret = jl_toplevel_eval_in(jl_main_module, (jl_value_t *) expr);
        JL_GC_POP();
        jl_exception_clear();

        // free
        mpq_clear(mpq);
        mpz_clear(numerator);
        mpz_clear(denominator);
      } else {
        // free
        mpq_clear(mpq);
        mpz_clear(numerator);
        mpz_clear(denominator);

        return JURASSIC_FAIL;
      }
    }
    break;
  }
  case PL_LIST_PAIR: {
    int len = list_length(term);
#ifdef JURASSIC_DEBUG
    printf("        This is a list, length = %d\n", len);
#endif
    jl_array_t *arr = jl_alloc_array_1d(jl_apply_array_type((jl_value_t*)jl_any_type, 1), len);

    if (!list_to_jl(term, &arr, flag_sym)) {
      *ret = NULL;
      return JURASSIC_FAIL;
    } else
      *ret = (jl_value_t *) arr;
    break;
  }
  case PL_TERM: {
    JL_TRY {
      jl_expr_t *expr = compound_to_jl_expr(term);
      if (expr == NULL) {
        *ret = NULL;
        return JURASSIC_FAIL;
      }
      JL_GC_PUSH1(&expr);
#ifdef JURASSIC_DEBUG
      jl_printf(jl_stdout_stream(), "[DEBUG] Parsed expression:\n");
      jl_static_show(jl_stdout_stream(), (jl_value_t *)expr);
      jl_printf(jl_stdout_stream(), "\n");
#endif
      if (jl_is_quotenode(expr))
        *ret = (jl_value_t *) expr;
      else
        *ret = jl_toplevel_eval_in(jl_main_module, (jl_value_t *) expr);
      JL_GC_POP();
      jl_exception_clear();
    } JL_CATCH {
      jl_task_t *ct = jl_current_task;
      jl_current_task->ptls->previous_exception = jl_current_exception();
      jl_throw_exception();
      *ret = NULL;
      return JURASSIC_FAIL;
    }
    break;
  }
  default:
    return JURASSIC_FAIL;
  }
  if (jl_exception_occurred()) {
    // none of these allocate, so a gc-root (JL_GC_PUSH) is not necessary
    jl_call2(jl_get_function(jl_base_module, "showerror"),
             jl_stderr_obj(),
             jl_exception_occurred());
    jl_printf(jl_stderr_stream(), "\n");
    return JURASSIC_FAIL;
  }
  return JURASSIC_SUCCESS;
}

/* convert atomic term to symbol */
jl_sym_t *atomic_to_sym(term_t atomic) {
  atom_t atom;
  if (!PL_get_atom(atomic, &atom)) {
#ifdef JURASSIC_DEBUG
    printf("[DEBUG] Term is not atomic!\n");
#endif
    return NULL;
  }

  const char *a = PL_atom_chars(atom);
  if (a == NULL) {
#ifdef JURASSIC_DEBUG
    printf("[DEBUG] Reading string from atom failed!\n");
#endif
    return NULL;
  } JL_TRY {
#ifdef JURASSIC_DEBUG
    printf("[DEBUG] Convert to symbol %s.\n", a);
#endif
    jl_exception_clear();
    return jl_symbol(a);
  } JL_CATCH {
    jl_task_t *ct = jl_current_task;
    jl_current_task->ptls->previous_exception = jl_current_exception();
    return NULL;
  }
}

/* convert quoted term to Julia symbol */
jl_sym_t * compound_to_sym(term_t term) {
  atom_t functor;
  size_t arity;
  if (!PL_get_compound_name_arity_sz(term, &functor, &arity)) {
#ifdef JURASSIC_DEBUG
    printf("[DEBUG] Cannot analyse compound!\n");
#endif
    return NULL;
  }
  const char *fname = PL_atom_chars(functor);
  if (fname[0] == ':' && arity < 2) {
    char *sym_str;
    if (!PL_get_chars(term, &sym_str,
                      CVT_WRITE|CVT_EXCEPTION|BUF_STACK|REP_UTF8))
      return NULL;

    int sym_len = strlen(sym_str);
    int ks = 0, ke = 0; // the start and end positions of symbol in string
    char quote;
    while ((sym_str[ks] == '$' || sym_str[ks] == ':' || sym_str[ks] == '\'' || sym_str[ks] == '\"'
            || sym_str[ks] == '(' || sym_str[ks] == ' ')
           && ks <= sym_len-1) {
      quote = sym_str[ks];
      sym_str[ks] = '\0';
      ks++;
    }
    for (ke = sym_len-1;  ke > ks; --ke) {
      if (quote_pair(quote, sym_str[ke])) {
        sym_str[ke] = '\0';
        break;
      }
    }
#ifdef JURASSIC_DEBUG
    printf("        Symbol: \"%s\"\n", sym_str + ks);
#endif
    return jl_symbol(sym_str + ks);
  } else {
    printf("[ERR] Term is not a symbol!\n");
    return NULL;
  }
}

/* Unify julia term with prolog term */
int jl_unify_pl(jl_value_t *val, term_t *ret, int flag_sym) {
  jl_sym_t *val_type_name_sym = ((jl_datatype_t*)(jl_typeof(val)))->name->name;
#ifdef JURASSIC_DEBUG
  printf("[Debug] Julia value:\n");
  jl_static_show(jl_stdout_stream(), val);
  jl_printf(jl_stdout_stream(), "\n");
  printf("[Debug] Julia type:\n");
  jl_static_show(jl_stdout_stream(), jl_typeof(val));
  jl_printf(jl_stdout_stream(), "/");
  jl_static_show(jl_stdout_stream(), (jl_value_t *) val_type_name_sym);
  jl_printf(jl_stdout_stream(), "\n");
#endif
  term_t tmp_term = PL_copy_term_ref(*ret);
  if (jl_is_nothing(val)) {
#ifdef JURASSIC_DEBUG
    printf("        Nothing.\n");
#endif
    return PL_unify_atom(tmp_term, ATOM_nothing);
  } else if (val == jl_eval_string("missing")) {
#ifdef JURASSIC_DEBUG
    printf("        Missing.\n");
#endif
    return PL_unify_atom(tmp_term, ATOM_missing);
  } else if (jl_is_bool(val)) {
#ifdef JURASSIC_DEBUG
    printf("        Bool: ");
#endif
    int retval = jl_unbox_bool(val);
#ifdef JURASSIC_DEBUG
    printf("%d.\n", retval);
#endif
    return PL_unify_bool(tmp_term, retval);
  } else if (jl_is_int8(val)) {
#ifdef JURASSIC_DEBUG
    printf("        Int8: ");
#endif
    int8_t retval = jl_unbox_int8(val);
    #ifdef JURASSIC_DEBUG
    printf("%ld.\n", retval);
#endif
    return PL_unify_integer(tmp_term, retval);
  } else if (jl_is_int16(val)) {
#ifdef JURASSIC_DEBUG
    printf("        Int16: ");
#endif
    int16_t retval = jl_unbox_int16(val);
    #ifdef JURASSIC_DEBUG
    printf("%ld.\n", retval);
#endif
    return PL_unify_integer(tmp_term, retval);
  } else if (jl_is_int32(val)) {
#ifdef JURASSIC_DEBUG
    printf("        Int32: ");
#endif
    int32_t retval = jl_unbox_int32(val);
    #ifdef JURASSIC_DEBUG
    printf("%ld.\n", retval);
#endif
    return PL_unify_integer(tmp_term, retval);
  } else if (jl_is_int64(val)) {
#ifdef JURASSIC_DEBUG
    printf("        Int64: ");
#endif
    int64_t retval = jl_unbox_int64(val);
    #ifdef JURASSIC_DEBUG
    printf("%ld.\n", retval);
#endif
    return PL_unify_int64(tmp_term, retval);
  } else if (jl_is_uint8(val) || jl_is_uint16(val) || jl_is_uint32(val)
             || jl_is_uint64(val)) {
#ifdef JURASSIC_DEBUG
    printf("        Unsigned Integer: ");
#endif
    uint64_t retval = jl_unbox_uint64(val);
    #ifdef JURASSIC_DEBUG
    printf("%ud.\n", retval);
#endif
    return PL_unify_uint64(tmp_term, retval);
  } else if (jl_typeis(val, jl_float16_type) ||
             jl_typeis(val, jl_float32_type) ||
             jl_typeis(val, jl_float64_type)) {
#ifdef JURASSIC_DEBUG
    printf("        Float: ");
#endif
    double retval;
    if (jl_typeis(val, jl_float64_type)) {
      retval = jl_unbox_float64(val);
    } else {
      retval = jl_unbox_float32(val);
    }
#ifdef JURASSIC_DEBUG
    printf("%f.\n", retval);
#endif
    if (retval == D_PINF) {
#ifdef JURASSIC_DEBUG
      printf("        Inf.\n");
#endif
      return PL_unify_atom(tmp_term, ATOM_inf);
    } else if (retval == D_NINF) {
#ifdef JURASSIC_DEBUG
      printf("        -Inf.\n");
#endif
      return PL_unify_atom(tmp_term, ATOM_ninf);
    } else if (isnan(retval)) {
#ifdef JURASSIC_DEBUG
      printf("        NaN.\n");
#endif
      return PL_unify_atom(tmp_term, ATOM_nan);
    } else
      return PL_unify_float(tmp_term, retval);
  } else if (jl_is_string(val)) {
#ifdef JURASSIC_DEBUG
    printf("        String: ");
#endif
    const char *retval = jl_string_ptr(val);
#ifdef JURASSIC_DEBUG
    printf("%s.\n", retval);
#endif
    return PL_unify_string_chars(tmp_term, retval);
  } else if (jl_is_quotenode(val)) {
    jl_value_t *quotedval = jl_quotenode_value(val);
#ifdef JURASSIC_DEBUG
    printf("        QuoteNode of:\n");
    jl_static_show(jl_stdout_stream(), quotedval);
    printf("\n");
#endif
    term_t qval = PL_new_term_ref();
    return jl_unify_pl(quotedval, &qval, 1)
      && PL_unify_functor(tmp_term, FUNCTOR_quotenode1)
      && PL_unify_arg(1, tmp_term, qval);
  } else if (strcmp(jl_symbol_name(val_type_name_sym), "Rational") == 0) {
        // Rational number
#ifdef JURASSIC_DEBUG
    printf("        Rational: ");
    jl_static_show(jl_stdout_stream(), jl_get_nth_field(val, 0));
    jl_printf(jl_stdout_stream(), "//");
    jl_static_show(jl_stdout_stream(), jl_get_nth_field(val, 1));
    jl_printf(jl_stdout_stream(), "\n");
#endif
    mpq_t retval;
    mpz_t num;
    mpz_t denom;

    int64_t numerator = jl_unbox_int64(jl_get_nth_field(val, 0));
    int64_t denominator = jl_unbox_int64(jl_get_nth_field(val, 1));

    mpz_init(num);
    mpz_init(denom);

    mpz_set_si(num, numerator);
    mpz_set_si(denom, denominator);

    mpq_init(retval);

    mpq_set_num(retval, num);
    mpq_set_den(retval, denom);

    int ret = PL_unify_mpq(tmp_term, retval);

    mpz_clear(num);
    mpz_clear(denom);
    mpq_clear(retval);

    return ret;
  } else if (jl_is_symbol(val)) {
#ifdef JURASSIC_DEBUG
    printf("        Symbol (Atom): ");
#endif
    const char *retval = jl_symbol_name((jl_sym_t *)val);
#ifdef JURASSIC_DEBUG
    printf("%s.\n", retval);
#endif
    if (strchr(retval, '.') != NULL) {
      return jl_unify_pl(jl_dot(retval), &tmp_term, flag_sym);
    } else if (jl_is_defined(retval) && !jl_is_operator((char *)retval)
               && !flag_sym) {
#ifdef JURASSIC_DEBUG
      printf("--- is defined.\n");
#endif
      jl_value_t *var_val;
      if (!jl_access_var(retval, &var_val))
        return JURASSIC_FAIL;
      return jl_unify_pl(var_val, &tmp_term, flag_sym);
    } else {
      /* unify with :/1 */
      term_t symname = PL_new_term_ref();
      return PL_put_atom(symname, PL_new_atom(retval))
        && PL_unify_functor(tmp_term, FUNCTOR_quote1)
        && PL_unify_arg(1, tmp_term, symname);
    }
  } else if (jl_is_array(val)) {
    if (jl_array_ndims(val) == 1) {
#ifdef JURASSIC_DEBUG
      printf("[DEBUG] 1D Array:\n");
#endif
      /* Construct a list */
      size_t len = jl_array_len(val);
      if (len == 0) {
#ifdef JURASSIC_DEBUG
        printf("        Empty Array: []\n");
#endif
        return PL_unify_nil(tmp_term);
      } else {
        term_t head = PL_new_term_ref();
        for (size_t i = 0; i < len; i++) {
#ifdef JURASSIC_DEBUG
          printf("---- #%lu:\n", i);
#endif
          if (!PL_unify_list(tmp_term, head, tmp_term) ||
              !jl_unify_pl(jl_arrayref((jl_array_t *)val, i), &head, flag_sym))
            return JURASSIC_FAIL;
        }
        return PL_unify_nil(tmp_term);
      }
    } else {
      // TODO: Unify with Multi-dimensional arrays with list of list of list...
      printf("[ERR] Cannot unify list with matrices and tensors!\n");
      return JURASSIC_FAIL;
    }
  } else if (jl_is_tuple(val)) {
#ifdef JURASSIC_DEBUG
    printf("[DEBUG] Tuple:\n");
#endif
    /* Tuple */
    return PL_unify_functor(tmp_term, FUNCTOR_tuple1) && jl_tuple_unify_all(&tmp_term, val);
  } else if (jl_is_expr(val)) {
    /* Expr */
#ifdef JURASSIC_DEBUG
    printf("[DEBUG] Expr:\n");
#endif
    jl_sym_t *head = ((jl_expr_t *)val)->head;
    jl_array_t *args = ((jl_expr_t *)val)->args;

    term_t head_term = PL_new_term_ref();
    if (!jl_unify_pl((jl_value_t *)head, &head_term, 1))
      return JURASSIC_FAIL;

    term_t args_term = PL_new_term_ref();
    if (!jl_unify_pl((jl_value_t *)args, &args_term, 1))
      return JURASSIC_FAIL;

    return PL_unify_functor(tmp_term, FUNCTOR_expr2)
      && PL_unify_arg(1, tmp_term, head_term)
      && PL_unify_arg(2, tmp_term, args_term);
  }
  return JURASSIC_FAIL;
}

/*******************************
 *          registers          *
 *******************************/
install_t install_jurassic(void) {
  ATOM_true = PL_new_atom("true");
  ATOM_false = PL_new_atom("false");
  ATOM_nan = PL_new_atom("nan");
  ATOM_missing = PL_new_atom("missing");
  ATOM_nothing = PL_new_atom("nothing");
  ATOM_inf = PL_new_atom("inf");
  ATOM_ninf = PL_new_atom("ninf");
  FUNCTOR_dot2 = PL_new_functor(ATOM_dot, 2);
  FUNCTOR_quote1 = PL_new_functor(PL_new_atom(":"), 1);
  FUNCTOR_quotenode1 = PL_new_functor(PL_new_atom("$"), 1);
  FUNCTOR_cmd1 = PL_new_functor(PL_new_atom("cmd"), 1);
  FUNCTOR_field2 = PL_new_functor(PL_new_atom("jl_field"), 2);
  FUNCTOR_inline2 = PL_new_functor(PL_new_atom("jl_inline"), 2);
  FUNCTOR_tuple1 = PL_new_functor(PL_new_atom("tuple"), 1);
  FUNCTOR_macro1 = PL_new_functor(PL_new_atom("jl_macro"), 1);
  FUNCTOR_equal2 = PL_new_functor(PL_new_atom("="), 2);
  FUNCTOR_plusequal2 = PL_new_functor(PL_new_atom("+="), 2);
  FUNCTOR_minusqual2 = PL_new_functor(PL_new_atom("-="), 2);
  FUNCTOR_timesequal2 = PL_new_functor(PL_new_atom("*="), 2);
  FUNCTOR_dividesequal2 = PL_new_functor(PL_new_atom("/="), 2);
  FUNCTOR_powerequal2 = PL_new_functor(PL_new_atom("^="), 2);
  FUNCTOR_expr2 = PL_new_functor(PL_new_atom("jl_expr"), 2);

  /* Registration */
  PL_register_foreign("jl_eval_str", 2, jl_eval_str, 0);
  PL_register_foreign("jl_eval", 2, jl_eval, 0);
  PL_register_foreign("jl_tuple_unify_str", 2, jl_tuple_unify_str, 0);
  PL_register_foreign("jl_tuple_unify", 2, jl_tuple_unify, 0);
  PL_register_foreign("jl_send_command_str", 1, jl_send_command_str, 0);
  PL_register_foreign("jl_send_command", 1, jl_send_command, 0);
  PL_register_foreign("jl_isdefined", 1, jl_isdefined, 0);
  PL_register_foreign("jl_using", 1, jl_using, 0);
  PL_register_foreign("jl_include", 1, jl_include, 0);
  PL_register_foreign("jl_declare_function", 3, jl_declare_function, 0);
  PL_register_foreign("jl_declare_macro_function", 4, jl_declare_function, 0);
  PL_register_foreign("jl_type_name", 2, jl_type_name, 0);
  PL_register_foreign("jl_embed_halt", 0, jl_embed_halt, 0);

  printf("Initialise Embedded Julia ...");

  /* Loading julia library */
  void *handle;
  handle = dlopen("libjulia.so", RTLD_NOW | RTLD_GLOBAL);
  if (!handle) {
    fprintf (stderr, "%s\n", dlerror());
    exit(1);
  }

  /* initialisation */
  jl_init();

  // jl_load(jl_main_module, "julia/arrays.jl");

  checked_send_command_str("println(\" Done.\")");
}

/* Allow returning value and unifying with Prolog variable */
foreign_t jl_eval(term_t jl_expr, term_t pl_ret) {
  jl_value_t *ret;
  JL_TRY {
    if (!pl_to_jl(jl_expr, &ret, TRUE))
      PL_fail;
    JL_GC_PUSH1(&ret);
#ifdef JURASSIC_DEBUG
    printf("[DEBUG] Evaluated result:\n");
    jl_static_show(jl_stdout_stream(), ret);
    jl_printf(jl_stdout_stream(), "\n");
#endif
    if (!jl_unify_pl(ret, &pl_ret, 0)) {
      JL_GC_POP();
      PL_fail;
    }
    JL_GC_POP();
    jl_exception_clear();
  } JL_CATCH {
    jl_task_t *ct = jl_current_task;
    jl_current_task->ptls->previous_exception = jl_current_exception();
    jl_throw_exception();
    PL_fail;
  }
  PL_succeed;
}

/* evaluate a string expression */
foreign_t jl_eval_str(term_t jl_expr, term_t pl_ret) {
  char *expression;
  if (!PL_get_chars(jl_expr, &expression,
                    CVT_ATOM|CVT_STRING|CVT_EXCEPTION|BUF_STACK|REP_UTF8))
    PL_fail;
  jl_value_t *ret;
  if (!checked_eval_string(expression, &ret))
    PL_fail;
  JL_GC_PUSH1(&ret);
  if (!jl_unify_pl(ret, &pl_ret, 1)) {
    JL_GC_POP();
    PL_fail;
  } else {
    JL_GC_POP();
    PL_succeed;
  }
}

/* unify prolog tuple([A|B]) with julia functions that returns a tuple */
foreign_t jl_tuple_unify(term_t pl_tuple, term_t jl_expr) {
  jl_value_t *val;
  if (!pl_to_jl(jl_expr, &val, TRUE))
    PL_fail;
  JL_GC_PUSH1(&val);
  if (!jl_tuple_unify_all(&pl_tuple, val)) {
    JL_GC_POP();
    PL_fail;
  } else {
    JL_GC_POP();
    PL_succeed;
  }
}

foreign_t jl_tuple_unify_str(term_t pl_tuple, term_t jl_expr_str) {
  char *expression;
  if (!PL_get_chars(jl_expr_str, &expression,
                    CVT_ATOM|CVT_STRING|CVT_EXCEPTION|BUF_STACK|REP_UTF8))
    PL_fail;
  jl_value_t *val;
  if (!checked_eval_string(expression, &val))
    PL_fail;
  JL_GC_PUSH1(&val);
  if (!jl_tuple_unify_all(&pl_tuple, val)) {
    JL_GC_POP();
    PL_fail;
  } else {
    JL_GC_POP();
    PL_succeed;
  }
}

/* evaluate string without returning value */
foreign_t jl_send_command_str(term_t jl_expr) {
  char *expression;
  if (!PL_get_chars(jl_expr, &expression,
                    CVT_ATOM|CVT_STRING|CVT_EXCEPTION|BUF_STACK|REP_UTF8))
    PL_fail;
  if (!checked_jl_command(expression))
    PL_fail;
  PL_succeed;
}

foreign_t jl_send_command(term_t jl_expr) {
  jl_value_t *ret;
  if (!pl_to_jl(jl_expr, &ret, TRUE) || ret == NULL) {
    if (jl_exception_occurred()) {
      jl_call2(jl_get_function(jl_base_module, "showerror"),
               jl_stderr_obj(),
               jl_exception_occurred());
      jl_printf(jl_stderr_stream(), "\n");
    }
    PL_fail;
  }
#ifdef JURASSIC_DEBUG
  jl_static_show(jl_stdout_stream(), ret);
  jl_printf(jl_stdout_stream(), "\n");
#endif
  JL_GC_PUSH1(&ret);
  if (jl_is_bool(ret)) {
    if (jl_unbox_bool(ret)) {
      JL_GC_POP();
      PL_succeed;
    } else {
      JL_GC_POP();
      PL_fail;
    }
  } else {
    JL_GC_POP();
    PL_succeed;
  }
}

/* test if an atom is defined as julia variable */
foreign_t jl_isdefined(term_t jl_expr) {
  char *expression;
  if (!PL_get_chars(jl_expr, &expression,
                    CVT_ATOM|CVT_STRING|CVT_EXCEPTION|BUF_STACK|REP_UTF8))
    PL_fail;
  if (!jl_is_defined(expression))
    PL_fail;
  PL_succeed;
}

/* using a julia module */
foreign_t jl_using(term_t term) {
  char *module;
  if (!PL_get_chars(term, &module,
                    CVT_ATOM|CVT_STRING|CVT_EXCEPTION|BUF_STACK|REP_UTF8))
    PL_fail;
  char cmd[BUFFSIZE];
  sprintf(cmd, "using %s", module);
  if (!checked_jl_command(cmd))
    PL_fail;
  PL_succeed;
}

/* load a julia file */
foreign_t jl_include(term_t term) {
  char *file;
  if (!PL_get_chars(term, &file,
                    CVT_ATOM|CVT_STRING|CVT_EXCEPTION|BUF_STACK|REP_UTF8))
    PL_fail;
  JL_TRY {
    jl_load(jl_main_module, file);
    jl_exception_clear();
  } JL_CATCH {
    jl_task_t *ct = jl_current_task;
    jl_current_task->ptls->previous_exception = jl_current_exception();
    jl_throw_exception();
    PL_fail;
  }
  PL_succeed;
}

/* declare a julia function */
foreign_t jl_declare_function(term_t fname_pl, term_t fargs_pl, term_t fexprs_pl) {
  JL_TRY {
    jl_expr_t *func = jl_function(fname_pl, fargs_pl, fexprs_pl);
    if (!func)
      PL_fail;
    JL_GC_PUSH1(&func);
#ifdef JURASSIC_DEBUG
    printf("[DEBUG] Function expression:\n");
    jl_static_show(jl_stdout_stream(), (jl_value_t *) func);
    jl_printf(jl_stdout_stream(), "\n");
#endif
    if (!jl_toplevel_eval_in(jl_main_module, (jl_value_t *) func)) {
      JL_GC_POP();
      PL_fail;
    }
    JL_GC_POP();
    jl_exception_clear();
  } JL_CATCH {
    jl_task_t *ct = jl_current_task;
    jl_current_task->ptls->previous_exception = jl_current_exception();
    jl_throw_exception();
    PL_fail;
  }
  PL_succeed;
}

/* declare a julia macro function */
foreign_t jl_declare_macro_function(term_t mname_pl, term_t fname_pl, term_t fargs_pl, term_t fexprs_pl) {
  // handle macro name
  atom_t m_atom;
  if (PL_is_atomic(mname_pl)) {
    // mname_pl is just an atom of name
    if (!PL_get_atom(mname_pl, &m_atom))
      PL_fail;
  } else {
    // mname_pl is a compound @macro
    term_t mname_term;
    if (!PL_is_functor(mname_pl, FUNCTOR_macro1))
      PL_fail;
    if (!PL_get_arg(1, mname_pl, mname_term))
      PL_fail;
    if (!PL_get_atom(mname_term, &m_atom))
      PL_fail;
  }
  const char *macro_name_ = PL_atom_chars(m_atom);
  JL_TRY {
    // macro name
    char *macro_name = (char *) calloc(1 + strlen(macro_name_) + 1, sizeof(char));
    strcpy(macro_name, "@");
    strcat(macro_name, macro_name_);

    // macro func
    jl_expr_t *ex = jl_exprn(jl_symbol("macrocall"), 3);

    // function definition
    jl_expr_t *func = jl_function(fname_pl, fargs_pl, fexprs_pl);
    if (!func)
      PL_fail;

    jl_exprargset(ex, 0, jl_dot(macro_name));
    jl_exprargset(ex, 1, func);
    JL_GC_PUSH1(&ex);

    free(macro_name); // free calloc
#ifdef JURASSIC_DEBUG
    printf("[DEBUG] Macro function expression:\n");
    jl_static_show(jl_stdout_stream(), (jl_value_t *) ex);
    jl_printf(jl_stdout_stream(), "\n");
#endif
    if (!jl_toplevel_eval_in(jl_main_module, (jl_value_t *) ex)) {
      JL_GC_POP(); // ex
      PL_fail;
    }
    JL_GC_POP();
    jl_exception_clear();
  } JL_CATCH {
    jl_task_t *ct = jl_current_task;
    jl_current_task->ptls->previous_exception = jl_current_exception();
    jl_throw_exception();
    JL_GC_POP();
    PL_fail;
  }
  PL_succeed;
}

/* return julia term type name */
foreign_t jl_type_name(term_t jl_expr, term_t type_name_term) {
  jl_value_t *tmp_val;
  if (!pl_to_jl(jl_expr, &tmp_val, FALSE) || tmp_val == NULL) {
    if (jl_exception_occurred()) {
      jl_call2(jl_get_function(jl_base_module, "showerror"),
               jl_stderr_obj(),
               jl_exception_occurred());
      jl_printf(jl_stderr_stream(), "\n");
    }
    PL_fail;
  }
#ifdef JURASSIC_DEBUG
  jl_static_show(jl_stdout_stream(), tmp_val);
  jl_printf(jl_stdout_stream(), "\n");
#endif
  JL_GC_PUSH1(&tmp_val);

  jl_sym_t *val_type_name_sym = ((jl_datatype_t*)(jl_typeof(tmp_val)))->name->name;
  const char *type_name_str = jl_symbol_name(val_type_name_sym);
#ifdef JURASSIC_DEBUG
  printf("Type name: %s.\n", type_name_str);
#endif
  if (!PL_unify_string_chars(type_name_term, type_name_str)) {
    JL_GC_POP();
    PL_fail;
  } else {
    JL_GC_POP();
    PL_succeed;
  }
}

/* halt embedding julia */
foreign_t jl_embed_halt(void) {
  jl_atexit_hook(0);
  PL_succeed;
}
