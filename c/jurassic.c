#include <stdio.h>
#include <string.h>
#include <dlfcn.h>

#include "jurassic.h"

/* atoms for julia expressions */
static functor_t FUNCTOR_dot2; /* subfields */
static functor_t FUNCTOR_at1; /* macro */
static functor_t FUNCTOR_colon1; /* for julia variable (and symbols) */
static functor_t FUNCTOR_tuple1; /* use tuple/1 to represent julia tuple */
static functor_t FUNCTOR_equal2; /* assignment */
static atom_t ATOM_true;
static atom_t ATOM_false;
static atom_t ATOM_nan;
static atom_t ATOM_nothing;
static atom_t ATOM_missing;
static atom_t ATOM_inf;
static atom_t ATOM_ninf; /* negative infinity */

static int halt_julia(int rc, void *p) {
  jl_atexit_hook(rc);
  return 0;
}

/*******************************
 *      static functions       *
 *******************************/
/* borrowed from real */
int list_length(term_t list) {
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

/* Have to adapt the built-in function to initialise an expression */
jl_expr_t *jl_exprn(jl_sym_t *head, size_t n) {
  jl_expr_t *ex;
  JL_TRY {
      jl_array_t *ar = jl_alloc_vec_any(n);
      JL_GC_PUSH1(&ar);
      ex = (jl_expr_t*)jl_gc_allocobj(sizeof(jl_expr_t));
      jl_set_typeof((jl_value_t *)ex, jl_expr_type);
      ex->head = head;
      ex->args = ar;
      JL_GC_POP();
      jl_exception_clear();
  } JL_CATCH {
    jl_get_ptls_states()->previous_exception = jl_current_exception();
    return NULL;
  }
  return ex;
}

/* eval julia string (from julia/src/embedding.c) with checking */
int checked_eval_string(const char *code, jl_value_t **ret) {
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

jl_value_t * checked_send_command_str(const char *code) {
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
/* eval julia code without return */
void checked_jl_command(const char *code) {
  jl_eval_string(code);
  if (jl_exception_occurred()) {
    // none of these allocate, so a gc-root (JL_GC_PUSH) is not necessary
    jl_call2(jl_get_function(jl_base_module, "showerror"),
                 jl_stderr_obj(),
                 jl_exception_occurred());
        jl_printf(jl_stderr_stream(), "\n");
  }
}

/* check if a variable is defined in julia */
int jl_is_defined(const char *var) {
  return jl_get_global(jl_main_module, jl_symbol_lookup(var))!= NULL ? TRUE : FALSE;
}

/* get julia variable from string */
int jl_access_var(const char *var, jl_value_t **ret) {
  if (jl_is_defined(var)) {
    JL_TRY {
      *ret = jl_get_global(jl_main_module, jl_symbol_lookup(var));
      jl_exception_clear();
    } JL_CATCH {
      jl_get_ptls_states()->previous_exception = jl_current_exception();
      *ret = NULL;
      return JURASSIC_FAIL;
    }
  }
  else
    return JURASSIC_FAIL;
  return JURASSIC_SUCCESS;
}

/* variable assignment */
int jl_assign_var(const char *var, jl_value_t *val) {
  JL_TRY {
      jl_set_global(jl_main_module, jl_symbol_lookup(var), val);
      jl_exception_clear();
  } JL_CATCH {
    jl_get_ptls_states()->previous_exception = jl_current_exception();
    return JURASSIC_FAIL;
  }
  return JURASSIC_SUCCESS;
}

/* prolog expression to string */
int atom_to_jl(atom_t atom, jl_value_t **ret, int flag_sym) {
  const char *a = PL_atom_chars(atom);
  if (a == NULL) {
#ifdef JURASSIC_DEBUG
    printf("Reading string from atom failed!\n");
#endif
    *ret = NULL;
    return JURASSIC_FAIL;
  }
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
    jl_static_show(JL_STDOUT, jl_get_global(jl_main_module, jl_symbol_lookup(a)));
    jl_printf(JL_STDOUT, "\n");
#endif
    *ret = jl_nothing;
  } else if (atom == ATOM_missing) {
#ifdef JURASSIC_DEBUG
    printf("Missing.\n");
    jl_static_show(JL_STDOUT, jl_get_global(jl_main_module, jl_symbol_lookup(a)));
    jl_printf(JL_STDOUT, "\n");
#endif
    *ret = jl_eval_string("missing");
  }  else if (atom == ATOM_nan) {
#ifdef JURASSIC_DEBUG
    printf("NaN.\n");
    jl_static_show(JL_STDOUT, jl_get_global(jl_main_module, jl_symbol_lookup(a)));
    jl_printf(JL_STDOUT, "\n");
#endif
    *ret = jl_box_float64(D_PNAN);
  } else if (atom == ATOM_inf) {
#ifdef JURASSIC_DEBUG
    printf("Inf.\n");
    jl_static_show(JL_STDOUT, jl_get_global(jl_main_module, jl_symbol_lookup(a)));
    jl_printf(JL_STDOUT, "\n");
#endif
    *ret = jl_box_float64(D_PINF);
  } else if (atom == ATOM_ninf) {
#ifdef JURASSIC_DEBUG
    printf("negative Inf.\n");
    jl_static_show(JL_STDOUT, jl_get_global(jl_main_module, jl_symbol_lookup(a)));
    jl_printf(JL_STDOUT, "\n");
#endif
    *ret = jl_box_float64(D_NINF);
  } else if (jl_is_defined(a) && !flag_sym) {
    /* get the variable assignment according to name */
#ifdef JURASSIC_DEBUG
    printf("defined Julia variable.\n");
    jl_static_show(JL_STDOUT, jl_get_global(jl_main_module, jl_symbol_lookup(a)));
    jl_printf(JL_STDOUT, "\n");
#endif
    return jl_access_var(a, ret);
  } else { /* default as Symbol */
#ifdef JURASSIC_DEBUG
    printf("Fallback to Symbol.\n");
#endif
    *ret = (jl_value_t *)jl_symbol(a);
  }
  if (jl_exception_occurred()) {
    jl_call2(jl_get_function(jl_base_module, "showerror"),
             jl_stderr_obj(),
             jl_exception_occurred());
    jl_printf(jl_stderr_stream(), "\n");
    return JURASSIC_FAIL;
  }
  return JURASSIC_SUCCESS;
}

/* assign Julia expression arguments with Prolog list */
static int list_to_expr_args(term_t list, jl_expr_t **ex, size_t start, size_t len) {
  term_t arg_term = PL_new_term_ref();
  term_t list_ = PL_copy_term_ref(list);
  size_t i = start;
  while (PL_get_list(list_, arg_term, list_) && i < start + len) {
#ifdef JURASSIC_DEBUG
    printf("----    Argument %lu: ", i);
    char *str_arg;
    if (!PL_get_chars(arg_term, &str_arg,
                      CVT_WRITE | CVT_EXCEPTION | BUF_DISCARDABLE | REP_UTF8))
      return JURASSIC_FAIL;
    printf("%s.\n", str_arg);
#endif
    jl_expr_t *a_i = compound_to_jl_expr(arg_term);
    if (a_i == NULL) {
      printf("[ERR] Convert term argument %lu failed!\n", i);
      return JURASSIC_FAIL;
    }
    jl_exprargset(*ex, i, a_i);
    i++;
  }
  return JURASSIC_SUCCESS;
}

/* Process function name */
static jl_sym_t *jl_fname(const char *fname) {
  char *dot = strchr(fname, '.');
  jl_expr_t *ex;
  if (dot == NULL)
    return jl_symbol(fname);
  else {
    /* if fname is Mod.fn, translate to Expr(:Mod, QuoteNode(:fn)) */
    JL_TRY {
      /* Module name */
      char *tok = strtok((char *) fname, ".");
      ex = jl_exprn(jl_symbol("."), 2);
      /* QuoteNode(function) */
      jl_exprargset(ex, 0, jl_symbol(tok));
      jl_exprargset(ex, 1, jl_new_struct(jl_quotenode_type, jl_symbol(++dot)));
    } JL_CATCH {
      jl_get_ptls_states()->previous_exception = jl_current_exception();
      return NULL;
    }
    return (jl_sym_t *) ex;
  }
}

/* convert prolog term to julia expression */
jl_expr_t *compound_to_jl_expr(term_t expr) {
#ifdef JURASSIC_DEBUG
  char *str_expr;
  if (!PL_get_chars(expr, &str_expr,
                    CVT_WRITE | CVT_EXCEPTION | BUF_DISCARDABLE | REP_UTF8))
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
    if (!list_to_expr_args(expr, &ex, 0, len))
      return NULL;
#ifdef JURASSIC_DEBUG
    jl_static_show(JL_STDOUT, (jl_value_t *) ex);
    jl_printf(JL_STDOUT, "\n");
#endif
    return ex;
  } else if (!PL_is_compound(expr)) {
    jl_value_t *ret;
    if (!pl2jl(expr, &ret, TRUE))
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
  } else if (fname[0] == '@') {
    /* macro calls */
#ifdef JURASSIC_DEBUG
    printf("        Macro: %s/%lu.\n", fname, arity);
#endif
    term_t arg = PL_new_term_ref();
    if (!PL_get_arg(1, expr, arg)) {
      printf("[ERR] Cannot access macro argument!\n");
      return NULL;
    }
    jl_expr_t *ex = jl_exprn(jl_symbol("macrocall"), arity + 2);
    jl_expr_t *ex_arg = compound_to_jl_expr(arg);
    JL_GC_PUSH1(&ex_arg);
    jl_exprargset(ex, 0, jl_fname(fname));
    jl_exprargset(ex, 1, (jl_value_t *)jl_linenode_line(ex_arg));
    jl_exprargset(ex, 2, ex_arg);
#ifdef JURASSIC_DEBUG
    jl_static_show(JL_STDOUT, (jl_value_t *) ex);
    jl_printf(JL_STDOUT, "\n");
#endif
    JL_GC_POP();
    return ex;
  } else if (strcmp(fname, "[]") == 0) {
    /* reference in array */
    /* term like a[i,j] =.. [[], [i,j], a]. use :ref function */
    term_t collection = PL_new_term_ref();
    term_t list = PL_new_term_ref();
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
    jl_expr_t *ex = jl_exprn(jl_symbol("ref"), len+1);
    jl_exprargset(ex, 0, compound_to_jl_expr(collection)); /* first argument is the collection */
    if (!list_to_expr_args(list, &ex, 1, len)) /* following arguments are references */
      return NULL;
#ifdef JURASSIC_DEBUG
    jl_static_show(JL_STDOUT, (jl_value_t *) ex);
    jl_printf(JL_STDOUT, "\n");
#endif
    return ex;
  } else if (PL_is_functor(expr, FUNCTOR_tuple1) && arity == 1) {
    term_t list = PL_new_term_ref();;
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
    if (!list_to_expr_args(list, &ex, 0, len))
      return NULL;
#ifdef JURASSIC_DEBUG
    jl_static_show(JL_STDOUT, (jl_value_t *) ex);
    jl_printf(JL_STDOUT, "\n");
#endif
    return ex;
  } else {
    /* functor to julia function symbol */
    if (arity == 0) {
      /* 0-argument function */
#ifdef JURASSIC_DEBUG
      printf("[DEBUG] 0-argument function: %s().\n", fname);
#endif
      jl_expr_t *ex = jl_exprn(jl_symbol("call"), 1);
      /* FIXME: "XX.xx" has to be processed as Expr(XX, :(xx))*/
      jl_exprargset(ex, 0, jl_fname(fname));
      return ex;
    } else {
      /* initialise an expression */
      if (strcmp(fname, "=") == 0 || strcmp(fname, "call") == 0) {
        /* for these meta predicates, no need to add "call" as Expr.head */
#ifdef JURASSIC_DEBUG
        printf("        Functor: %s/%lu.\n", fname, arity);
#endif
        jl_expr_t *ex = jl_exprn(jl_fname(fname), arity);

        /* assign arguments */
        term_t arg_term = PL_new_term_ref();
        for (size_t i = 0; i < arity; i++) { // Prolog argument index starts from 1
#ifdef JURASSIC_DEBUG
          printf("----    Argument %lu: ", i);
#endif
          if (!PL_get_arg(i+1, expr, arg_term)) {
            printf("[ERR] Get term argument %lu failed!\n", i);
            return NULL;
          }
#ifdef JURASSIC_DEBUG
          char *str_arg;
          if (!PL_get_chars(arg_term, &str_arg,
                            CVT_WRITE | CVT_EXCEPTION | BUF_DISCARDABLE | REP_UTF8))
            return NULL;
          printf("%s.\n", str_arg);
#endif
          jl_expr_t *a_i = compound_to_jl_expr(arg_term);
          if (a_i == NULL) {
            printf("[ERR] Convert term argument %lu failed!\n", i);
            return NULL;
          }
          jl_exprargset(ex, i, a_i);
        }
#ifdef JURASSIC_DEBUG
        jl_static_show(JL_STDOUT, (jl_value_t *) ex);
        jl_printf(JL_STDOUT, "\n");
#endif
        return ex;
      } else {
#ifdef JURASSIC_DEBUG
        printf("        Functor: call/%lu.\n", arity+1);
#endif
        /* use :call as Expr head */
        jl_expr_t *ex = jl_exprn(jl_symbol("call"), arity+1);

        /* set fname as the first argument */
        jl_exprargset(ex, 0, jl_fname(fname));
#ifdef JURASSIC_DEBUG
        printf("----    Argument 0: %s.\n", fname);
#endif
        /* other arguments */
        term_t arg_term = PL_new_term_ref();
        for (size_t i = 0; i < arity; i++) { // Prolog argument index starts from 1
#ifdef JURASSIC_DEBUG
          printf("----    Argument %lu: ", i+1);
#endif
          if (!PL_get_arg(i+1, expr, arg_term)) {
            printf("[ERR] Get term argument %lu failed!\n", i);
            return NULL;
          }
#ifdef JURASSIC_DEBUG
          char *str_arg;
          if (!PL_get_chars(arg_term, &str_arg,
                            CVT_WRITE | CVT_EXCEPTION | BUF_DISCARDABLE | REP_UTF8))
            return NULL;
          printf("%s.\n", str_arg);
#endif
          jl_expr_t *a_i = compound_to_jl_expr(arg_term);
          if (a_i == NULL) {
            printf("[ERR] Convert term argument %lu failed!\n", i);
            return NULL;
          }
          jl_exprargset(ex, i+1, a_i);
        }
#ifdef JURASSIC_DEBUG
        jl_static_show(JL_STDOUT, (jl_value_t *) ex);
        jl_printf(JL_STDOUT, "\n");
#endif
        return ex;
      }
    }
  }
  return NULL;
}

int list_to_jl(term_t list, int length, jl_array_t **ret, int flag_sym) {
  jl_value_t **arr_vals = (jl_value_t **) jl_array_data(*ret);

  term_t head = PL_new_term_ref();
  term_t term = PL_copy_term_ref(list);

  size_t i = 0;
  while (PL_get_list(term, head, term)) {
    if (!pl2jl(head, &arr_vals[i], flag_sym)) {
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

int pl2jl(term_t term, jl_value_t **ret, int flag_sym) {
#ifdef JURASSIC_DEBUG
  char * show;
  /* string, to string*/
  if (!PL_get_chars(term, &show,
                CVT_WRITE|CVT_EXCEPTION|BUF_DISCARDABLE|REP_UTF8))
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
                      CVT_ATOM|CVT_STRING|CVT_EXCEPTION|BUF_DISCARDABLE|REP_UTF8)) {
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
  case PL_LIST_PAIR: {
    int len = list_length(term);
#ifdef JURASSIC_DEBUG
    printf("        This is a list, length = %d\n", len);
#endif
    jl_array_t *arr = jl_alloc_array_1d(jl_apply_array_type((jl_value_t*)jl_any_type, 1), len);

    if (!list_to_jl(term, len, &arr, flag_sym)) {
      *ret = NULL;
      return JURASSIC_FAIL;
    } else
      *ret = (jl_value_t *) arr;
    break;
  }
  case PL_TERM: {
    // return compound_to_jl(term, ret);
    JL_TRY {
      jl_expr_t *expr = compound_to_jl_expr(term);
      JL_GC_PUSH1(&expr);
#ifdef JURASSIC_DEBUG
      jl_printf(JL_STDOUT, "[DEBUG] Parsed expression:\n");
      jl_static_show(JL_STDOUT, (jl_value_t *)expr);
      jl_printf(JL_STDOUT, "\n");
#endif
      *ret = jl_toplevel_eval(jl_main_module, (jl_value_t *)expr);
      JL_GC_POP();
      jl_exception_clear();
    } JL_CATCH {
      jl_get_ptls_states()->previous_exception = jl_current_exception();
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

/* TODO: unify julia term with prolog term */
int jl_unify_pl(jl_value_t *val, term_t *ret) {
#ifdef JURASSIC_DEBUG
  printf("[Debug] Julia value:\n");
  jl_static_show(JL_STDOUT, val);
  jl_printf(JL_STDOUT, "\n");
#endif
  term_t tmp_term = PL_new_term_ref();
  if (jl_is_nothing(val)) {
#ifdef JURASSIC_DEBUG
    printf("        Nothing.\n");
#endif
    PL_put_atom(tmp_term, ATOM_nothing);
  } else if (val == jl_eval_string("missing")) {
#ifdef JURASSIC_DEBUG
    printf("        Missing.\n");
#endif
    PL_put_atom(tmp_term, ATOM_missing);
  } else if (jl_is_bool(val)) {
#ifdef JURASSIC_DEBUG
    printf("        Bool: ");
#endif
    int retval = jl_unbox_bool(val);
#ifdef JURASSIC_DEBUG
    printf("%d.\n", retval);
#endif
    if (!PL_put_bool(tmp_term, retval))
      return JURASSIC_FAIL;
  } else if (jl_is_int8(val) || jl_is_int16(val) || jl_is_int32(val) ||
             jl_is_int64(val) || jl_is_uint8(val) || jl_is_uint16(val) ||
             jl_is_uint32(val) || jl_is_uint64(val)) {
#ifdef JURASSIC_DEBUG
    printf("        Integer: ");
#endif
    int64_t retval =jl_unbox_int64(val);
#ifdef JURASSIC_DEBUG
    printf("%ld.\n", retval);
#endif
    if (!PL_put_int64(tmp_term, retval))
      return JURASSIC_FAIL;
  } else if (jl_typeis(val, jl_float16_type) ||
             jl_typeis(val, jl_float32_type) ||
             jl_typeis(val, jl_float64_type)) {
#ifdef JURASSIC_DEBUG
    printf("        Float: ");
#endif
    double retval = jl_unbox_float64(val);
#ifdef JURASSIC_DEBUG
    printf("%f.\n", retval);
#endif
    if (retval == D_PINF) {
#ifdef JURASSIC_DEBUG
      printf("        Inf.\n");
#endif
      PL_put_atom(tmp_term, ATOM_inf);
    } else if (retval == D_NINF) {
#ifdef JURASSIC_DEBUG
      printf("        -Inf.\n");
#endif
      PL_put_atom(tmp_term, ATOM_ninf);
    } else if (isnan(retval)) {
#ifdef JURASSIC_DEBUG
    printf("        NaN.\n");
#endif
    PL_put_atom(tmp_term, ATOM_nan);
    } else if (!PL_put_float(tmp_term, retval))
      return JURASSIC_FAIL;
  } else if (jl_is_string(val)) {
#ifdef JURASSIC_DEBUG
    printf("        String: ");
#endif
    const char* retval = jl_string_ptr(val);
#ifdef JURASSIC_DEBUG
    printf("%s.\n", retval);
#endif
    if (!PL_put_string_chars(tmp_term, retval))
      return JURASSIC_FAIL;
  } else if (jl_is_symbol(val)) {
#ifdef JURASSIC_DEBUG
    printf("        Symbol (Atom): ");
#endif
    const char *retval = jl_symbol_name((jl_sym_t *)val);
#ifdef JURASSIC_DEBUG
    printf("%s.\n", retval);
#endif
    if (jl_is_defined(retval)) {
      jl_value_t *var_val;
      if (!jl_access_var(retval, &var_val))
        return JURASSIC_FAIL;
      return jl_unify_pl(var_val, &tmp_term);
    } else if (!PL_put_atom_chars(tmp_term, retval))
      return JURASSIC_FAIL;
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
        tmp_term = ATOM_nil;
      } else {
        PL_put_nil(tmp_term);
        int i = len;
        while (--i >= 0) {
#ifdef JURASSIC_DEBUG
          printf("---- #%d:\n", i);
#endif
          term_t head = PL_new_term_ref();
          if (!jl_unify_pl(jl_arrayref((jl_array_t *)val, i), &head)) // Use jl_arrayref to retrieve boxed jl_value_t *,
                                                                      // Don't use jl_array_data, which retrieves unboxed c array.
            return JURASSIC_FAIL;
          if (!PL_cons_list(tmp_term, head, tmp_term))
            return JURASSIC_FAIL;
        }
      }
    } else {
      printf("[ERR] Cannot unify list with matrices and tensors!\n");
      return JURASSIC_FAIL;
    }
  } else if (jl_is_tuple(val)) {
#ifdef JURASSIC_DEBUG
    printf("[DEBUG] Tuple:\n");
#endif
    /* Tuple */
    size_t nargs = jl_nfields(val);
    term_t list = PL_new_term_ref();
    if (nargs == 0) {
#ifdef JURASSIC_DEBUG
      printf("       Empty tuple: ()\n");
      PL_put_atom(list, ATOM_nil);
#endif
    } else {
      /* Construct list */
      PL_put_nil(list);
      int i = nargs;
      while (--i >= 0) {
#ifdef JURASSIC_DEBUG
        printf("---- #%d:\n", i);
#endif
        term_t head = PL_new_term_ref();
        jl_value_t *element = jl_fieldref(val, i);
        JL_GC_PUSH1(&element);
        if (!jl_unify_pl(element, &head)) {
          JL_GC_POP();
          return JURASSIC_FAIL;
        }
        if (!PL_cons_list(list, head, list)) {
          JL_GC_POP();
          return JURASSIC_FAIL;
        }
        JL_GC_POP();
      }
      /* Construct tuple/1 compound */
      if (!PL_cons_functor(tmp_term, FUNCTOR_tuple1, list))
        return JURASSIC_FAIL;
    }
  }
  /* unify with the converted term */
  if (!PL_unify(*ret, tmp_term)) {
    printf("[ERR] Unifying converted term with target term failed!\n");
    return JURASSIC_FAIL;
  }
  return JURASSIC_SUCCESS;
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
  FUNCTOR_colon1 = PL_new_functor(PL_new_atom(":"), 1);
  FUNCTOR_tuple1 = PL_new_functor(PL_new_atom("tuple"), 1);
  FUNCTOR_equal2 = PL_new_functor(PL_new_atom("="), 2);

  /* Registration */
  PL_register_foreign("jl_eval_str", 2, jl_eval_str, 0);
  PL_register_foreign("jl_eval", 2, jl_eval, 0);
  PL_register_foreign("jl_send_command_str", 1, jl_send_command_str, 0);
  PL_register_foreign("jl_send_command", 1, jl_send_command, 0);
  PL_register_foreign("jl_using", 1, jl_using, 0);
  PL_register_foreign("jl_include", 1, jl_include, 0);

  printf("Starting Julia ...");

  /* Loading julia library */
  void *handle;
  handle = dlopen("libjulia.so", RTLD_NOW | RTLD_GLOBAL);
  if (!handle) {
    fprintf (stderr, "%s\n", dlerror());
    exit(1);
  }

  /* initialisation */
  jl_init();
  PL_on_halt(halt_julia, 0);
  checked_send_command_str("println(\"Done.\")");
}

/* TODO: Allow returning value and unifying with Prolog variable */
foreign_t jl_eval(term_t jl_expr, term_t pl_ret) {
  jl_value_t * ret;
  JL_TRY {
    if (!pl2jl(jl_expr, &ret, TRUE))
      PL_fail;
#ifdef JURASSIC_DEBUG
    printf("[DEBUG] Evaluated result:\n");
    jl_static_show(JL_STDOUT, ret);
    jl_printf(JL_STDOUT, "\n");
#endif
    JL_GC_PUSH1(&ret);
    if (!jl_unify_pl(ret, &pl_ret)) {
      JL_GC_POP();
      PL_fail;
    }
    JL_GC_POP();
  } JL_CATCH {
    jl_get_ptls_states()->previous_exception = jl_current_exception();
    PL_fail;
  }
  PL_succeed;
}

/* evaluate a string expression */
foreign_t jl_eval_str(term_t jl_expr, term_t pl_ret) {
  char * expression;
  if (!PL_get_chars(jl_expr, &expression,
                    CVT_ATOM|CVT_STRING|CVT_EXCEPTION|BUF_DISCARDABLE|REP_UTF8))
    PL_fail;
  jl_value_t *ret = jl_eval_string(expression);
  if (jl_exception_occurred()) {
    jl_call2(jl_get_function(jl_base_module, "showerror"),
             jl_stderr_obj(),
             jl_exception_occurred());
    jl_printf(jl_stderr_stream(), "\n");
    PL_fail;
  }
  JL_GC_PUSH1(&ret);
  if (!jl_unify_pl(ret, &pl_ret)) {
    JL_GC_POP();
    PL_fail;
  }
  JL_GC_POP();
  PL_succeed;
}

/* evaluate string without returning value */
foreign_t jl_send_command_str(term_t jl_expr) {
  char * expression;
  if (!PL_get_chars(jl_expr, &expression,
                    CVT_ATOM|CVT_STRING|CVT_EXCEPTION|BUF_DISCARDABLE|REP_UTF8))
    PL_fail;
  jl_eval_string(expression);
  if (jl_exception_occurred()) {
    jl_call2(jl_get_function(jl_base_module, "showerror"),
             jl_stderr_obj(),
             jl_exception_occurred());
    jl_printf(jl_stderr_stream(), "\n");
    PL_fail;
  }
  PL_succeed;
}

foreign_t jl_send_command(term_t jl_expr) {
  jl_value_t *ret;
  if (!pl2jl(jl_expr, &ret, TRUE) || ret == NULL) {
    if (jl_exception_occurred()) {
        jl_call2(jl_get_function(jl_base_module, "showerror"),
                 jl_stderr_obj(),
                 jl_exception_occurred());
        jl_printf(jl_stderr_stream(), "\n");
    }
    PL_fail;
  }
#ifdef JURASSIC_DEBUG
  jl_static_show(JL_STDOUT, ret);
  jl_printf(JL_STDOUT, "\n");
#endif
  PL_succeed;
}

/* using a julia module */
foreign_t jl_using(term_t term) {
  char *module;
  if (!PL_get_chars(term, &module,
                    CVT_ATOM|CVT_STRING|CVT_EXCEPTION|BUF_DISCARDABLE|REP_UTF8))
    PL_fail;
  char cmd[BUFFSIZE];
  sprintf(cmd, "using %s", module);
  checked_jl_command(cmd);
  if (jl_exception_occurred()) {
    jl_call2(jl_get_function(jl_base_module, "showerror"),
             jl_stderr_obj(),
             jl_exception_occurred());
    jl_printf(jl_stderr_stream(), "\n");
    PL_fail;
  }
  PL_succeed;
}

/* load a julia file */
foreign_t jl_include(term_t term) {
  char *file;
  if (!PL_get_chars(term, &file,
                    CVT_ATOM|CVT_STRING|CVT_EXCEPTION|BUF_DISCARDABLE|REP_UTF8))
    PL_fail;
  char cmd[BUFFSIZE];
  sprintf(cmd, "include(%s)", file);
  if (jl_exception_occurred()) {
    jl_call2(jl_get_function(jl_base_module, "showerror"),
             jl_stderr_obj(),
             jl_exception_occurred());
    jl_printf(jl_stderr_stream(), "\n");
    PL_fail;
  }
  PL_succeed;
}
