/*
  AST
  interface to front-end, obtains and translates syntax trees
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <sys/types.h>
#include <limits.h>
#include <errno.h>
#include <math.h>
#include <setjmp.h>
#ifdef BOEHM_GC
#include <gc.h>
#endif
#include "llt.h"
#include "julia.h"

#include "flisp.h"

static htable_t gensym_table;

static char flisp_system_image[] = {
#include "julia_flisp.boot.inc"
};

extern fltype_t *iostreamtype;

void jl_lisp_prompt()
{
    fl_applyn(1, symbol_value(symbol("__start")), fl_cons(FL_NIL,FL_NIL));
}

value_t fl_defined_julia_global(value_t *args, uint32_t nargs)
{
    argcount("defined-julia-global", nargs, 1);
    (void)tosymbol(args[0], "defined-julia-global");
    char *name = symbol_name(args[0]);
    return jl_boundp(jl_system_module, jl_symbol(name)) ? FL_T : FL_F;
}

static builtinspec_t julia_flisp_ast_ext[] = {
    { "defined-julia-global", fl_defined_julia_global },
    { NULL, NULL }
};

static fltype_t *jvtype;

void jl_init_frontend()
{
    fl_init(2*512*1024);
    value_t img = cvalue(iostreamtype, sizeof(ios_t));
    ios_t *pi = value2c(ios_t*, img);
    ios_static_buffer(pi, flisp_system_image, sizeof(flisp_system_image));
    
    if (fl_load_system_image(img)) {
        ios_printf(ios_stderr, "fatal error loading system image\n");
        exit(1);
    }

    fl_applyn(0, symbol_value(symbol("__init_globals")));

    htable_new(&gensym_table, 0);

    jvtype = define_opaque_type(symbol("julia_value"), sizeof(void*),
                                NULL, NULL);

    assign_global_builtins(julia_flisp_ast_ext);
}

void jl_shutdown_frontend()
{
    //fl_applyn(0, symbol_value(symbol("show-profiles")));
}

static jl_value_t *scm_to_julia(value_t e);
static value_t julia_to_scm(jl_value_t *v);

static char boot_j[] = {
#include "boot.j.inc"
};

void jl_load_boot_j()
{
    value_t bootc = cvalue(iostreamtype, sizeof(ios_t));
    ios_t *pi = value2c(ios_t*, bootc);
    ios_static_buffer(pi, boot_j, sizeof(boot_j));
    value_t sexpr = fl_read_sexpr(bootc);
    jl_value_t *ast = scm_to_julia(sexpr);
    JL_GC_PUSH(&ast);
    jl_load_file_expr("boot.j", ast);
    JL_GC_POP();
}

static jl_sym_t *scmsym_to_julia(value_t s)
{
    assert(issymbol(s));
    if (fl_isgensym(s)) {
        uptrint_t n = ((gensym_t*)ptr(s))->id + 100;
        void **bp = ptrhash_bp(&gensym_table, (void*)n);
        if (*bp == HT_NOTFOUND) {
            *bp = jl_gensym();
        }
        return (jl_sym_t*)*bp;
    }
    return jl_symbol(symbol_name(s));
}

static char *scmsym_to_str(value_t s)
{
    assert(issymbol(s));
    return symbol_name(s);
}

static void syntax_error_check(value_t e)
{
    if (iscons(e)) {
        value_t hd = car_(e);
        if (issymbol(hd)) {
            char *s = scmsym_to_str(hd);
            if (!strcmp(s,"error")) {
                // TODO: line number
                jl_errorf("\nsyntax error: %s",
                          (char*)cvalue_data(car_(cdr_(e))));
            }
        }
    }
}

static size_t scm_list_length(value_t x)
{
    return llength(x);
}

static jl_value_t *scm_to_julia_(value_t e);

static jl_value_t *full_list(value_t e)
{
    jl_array_t *ar = jl_alloc_cell_1d(scm_list_length(e));
    size_t i=0;
    while (iscons(e)) {
        jl_cellset(ar, i, scm_to_julia_(car_(e)));
        e = cdr_(e);
        i++;
    }
    return (jl_value_t*)ar;
}

static jl_value_t *full_list_of_lists(value_t e)
{
    jl_array_t *ar = jl_alloc_cell_1d(scm_list_length(e));
    size_t i=0;
    while (iscons(e)) {
        jl_cellset(ar, i, full_list(car_(e)));
        e = cdr_(e);
        i++;
    }
    return (jl_value_t*)ar;
}

static jl_value_t *scm_to_julia(value_t e)
{
#ifdef JL_GC_MARKSWEEP
    int en = jl_gc_is_enabled();
    jl_gc_disable();
#endif
    jl_value_t *v = scm_to_julia_(e);
#ifdef JL_GC_MARKSWEEP
    if (en) jl_gc_enable();
#endif
    htable_reset(&gensym_table, gensym_table.size);
    return v;
}

static jl_value_t *scm_to_julia_(value_t e)
{
    if (fl_isnumber(e)) {
        if (iscprim(e) && cp_numtype((cprim_t*)ptr(e))==T_DOUBLE) {
            return (jl_value_t*)jl_box_float64(*(double*)cp_data((cprim_t*)ptr(e)));
        }
        if (iscprim(e) && cp_numtype((cprim_t*)ptr(e))==T_INT64) {
            return (jl_value_t*)jl_box_int64(*(int64_t*)cp_data((cprim_t*)ptr(e)));
        }
        if (iscprim(e) && cp_numtype((cprim_t*)ptr(e))==T_UINT64) {
            return (jl_value_t*)jl_box_uint64(*(uint64_t*)cp_data((cprim_t*)ptr(e)));
        }
        if (isfixnum(e)) {
            int64_t ne = numval(e);
            if (ne > S32_MAX || ne < S32_MIN)
                return (jl_value_t*)jl_box_int64(ne);
            return (jl_value_t*)jl_box_int32((int32_t)ne);
        }
        uint64_t n = toulong(e, "scm_to_julia");
        if (n > S32_MAX)
            return (jl_value_t*)jl_box_int64((int64_t)n);
        return (jl_value_t*)jl_box_int32((int32_t)n);
    }
    if (issymbol(e)) {
        jl_sym_t *sym = scmsym_to_julia(e);
        if (!strcmp(sym->name,"true"))
            return jl_true;
        else if (!strcmp(sym->name,"false"))
            return jl_false;
        return (jl_value_t*)sym;
    }
    if (fl_isstring(e)) {
        return jl_pchar_to_string(cvalue_data(e), cvalue_len(e));
    }
    if (e == FL_F) {
        return jl_false;
    }
    if (e == FL_T) {
        return jl_true;
    }
    if (e == FL_NIL) {
        return (jl_value_t*)jl_null;
    }
    if (iscons(e)) {
        value_t hd = car_(e);
        if (issymbol(hd)) {
            jl_sym_t *sym = scmsym_to_julia(hd);
            char *s = sym->name;
            /* tree node types:
               goto  gotoifnot  label  return
               lambda  call  =  quote
               null  top  unbound  box-unbound  closure-ref
               body  file
               line
            */
            size_t n = scm_list_length(e)-1;
            size_t i;
            if (sym == lambda_sym) {
                jl_expr_t *ex = jl_exprn(lambda_sym, n);
                value_t largs = car_(cdr_(e));
                jl_cellset(ex->args, 0, full_list(largs));
                e = cdr_(cdr_(e));
                for(i=1; i < n; i++) {
                    assert(iscons(e));
                    jl_cellset(ex->args, i, scm_to_julia_(car_(e)));
                    e = cdr_(e);
                }
                return
                    (jl_value_t*)jl_new_lambda_info((jl_value_t*)ex, jl_null);
            }
            if (!strcmp(s, "var-info")) {
                jl_expr_t *ex = jl_exprn(sym, n);
                e = cdr_(e);
                jl_cellset(ex->args, 0, scm_to_julia_(car_(e)));
                e = cdr_(e);
                jl_cellset(ex->args, 1, full_list_of_lists(car_(e)));
                e = cdr_(e);
                jl_cellset(ex->args, 2, full_list_of_lists(car_(e)));
                e = cdr_(e);
                for(i=3; i < n; i++) {
                    assert(iscons(e));
                    jl_cellset(ex->args, i, scm_to_julia_(car_(e)));
                    e = cdr_(e);
                }
                return (jl_value_t*)ex;
            }
            jl_expr_t *ex = jl_exprn(sym, n);
            e = cdr_(e);
            for(i=0; i < n; i++) {
                assert(iscons(e));
                jl_cellset(ex->args, i, scm_to_julia_(car_(e)));
                e = cdr_(e);
            }
            return (jl_value_t*)ex;
        }
        else {
            jl_error("malformed tree");
        }
    }
    if (iscprim(e) && cp_class((cprim_t*)ptr(e))==wchartype) {
        jl_value_t *wc =
            jl_box32(jl_char_type, *(int32_t*)cp_data((cprim_t*)ptr(e)));
        return wc;
    }
    if (iscvalue(e) && cv_class((cvalue_t*)ptr(e)) == jvtype) {
        return *(jl_value_t**)cv_data((cvalue_t*)ptr(e));
    }
    jl_error("malformed tree");
    
    return (jl_value_t*)jl_null;
}

static value_t array_to_list(jl_array_t *a)
{
    long i;
    value_t lst=FL_NIL, temp=FL_NIL;
    fl_gc_handle(&lst);
    fl_gc_handle(&temp);
    for(i=a->length-1; i >= 0; i--) {
        temp = julia_to_scm(jl_cellref(a,i));
        lst = fl_cons(temp, lst);
    }
    fl_free_gc_handles(2);
    return lst;
}

static value_t julia_to_scm(jl_value_t *v)
{
    if (jl_is_symbol(v)) {
        return symbol(((jl_sym_t*)v)->name);
    }
    if (v == jl_true) {
        return symbol("true");
    }
    if (v == jl_false) {
        return symbol("false");
    }
    if (jl_is_expr(v)) {
        jl_expr_t *ex = (jl_expr_t*)v;
        value_t args = array_to_list(ex->args);
        fl_gc_handle(&args);
        value_t hd = julia_to_scm((jl_value_t*)ex->head);
        value_t scmv = fl_cons(hd, args);
        fl_free_gc_handles(1);
        return scmv;
    }
    if (jl_is_array(v)) {
        return array_to_list((jl_array_t*)v);
    }
    value_t opaque = cvalue(jvtype, sizeof(void*));
    *(jl_value_t**)cv_data((cvalue_t*)ptr(opaque)) = v;
    return opaque;
}

jl_value_t *jl_parse_input_line(const char *str)
{
    value_t e = fl_applyn(1, symbol_value(symbol("jl-parse-string")),
                          cvalue_static_cstring(str));
    if (e == FL_T || e == FL_F || e == FL_EOF)
        return NULL;
    syntax_error_check(e);
    
    return scm_to_julia(e);
}

jl_value_t *jl_parse_file(const char *fname)
{
    value_t e = fl_applyn(1, symbol_value(symbol("jl-parse-file")),
                          cvalue_static_cstring(fname));
    syntax_error_check(e);
    if (!iscons(e))
        return (jl_value_t*)jl_null;
    return scm_to_julia(e);
}

jl_lambda_info_t *jl_expand(jl_value_t *expr)
{
    value_t e = fl_applyn(1, symbol_value(symbol("jl-expand-to-thunk")),
                          julia_to_scm(expr));
    if (e == FL_T || e == FL_F || e == FL_EOF)
        return NULL;
    syntax_error_check(e);

    jl_lambda_info_t *li = (jl_lambda_info_t*)scm_to_julia(e);
    assert(jl_is_lambda_info(li));
    return li;
}

// syntax tree accessors

// get array of formal argument expressions
jl_array_t *jl_lam_args(jl_expr_t *l)
{
    assert(l->head == lambda_sym);
    jl_value_t *ae = jl_exprarg(l,0);
    assert(jl_is_array(ae));
    return (jl_array_t*)ae;
}

// get array of local var symbols
jl_array_t *jl_lam_locals(jl_expr_t *l)
{
    jl_value_t *le = jl_exprarg(l, 1);
    assert(jl_is_expr(le));
    jl_expr_t *lle = (jl_expr_t*)jl_exprarg(le,0);
    assert(jl_is_expr(lle));
    assert(lle->head == locals_sym);
    return lle->args;
}

// get array of var info records
jl_array_t *jl_lam_vinfo(jl_expr_t *l)
{
    jl_value_t *le = jl_exprarg(l, 1);
    assert(jl_is_expr(le));
    jl_array_t *vil = (jl_array_t*)jl_exprarg(le,1);
    assert(jl_is_array(vil));
    return vil;
}

// get array of var info records for captured vars
jl_array_t *jl_lam_capt(jl_expr_t *l)
{
    jl_value_t *le = jl_exprarg(l, 1);
    assert(jl_is_expr(le));
    jl_array_t *vil = (jl_array_t*)jl_exprarg(le,2);
    assert(jl_is_array(vil));
    return vil;
}

// get array of body forms
jl_array_t *jl_lam_body(jl_expr_t *l)
{
    jl_value_t *be = jl_exprarg(l, 2);
    assert(jl_is_expr(be));
    assert(((jl_expr_t*)be)->head == body_sym);
    return ((jl_expr_t*)be)->args;
}

jl_sym_t *jl_decl_var(jl_value_t *ex)
{
    if (jl_is_symbol(ex)) return (jl_sym_t*)ex;
    assert(jl_is_expr(ex));
    return (jl_sym_t*)jl_exprarg(ex, 0);
}

int jl_is_rest_arg(jl_value_t *ex)
{
    if (!jl_is_expr(ex)) return 0;
    if (((jl_expr_t*)ex)->head != colons_sym) return 0;
    jl_expr_t *atype = (jl_expr_t*)jl_exprarg(ex,1);
    if (!jl_is_expr(atype)) return 0;
    if (atype->head != call_sym ||
        atype->args->length != 3)
        return 0;
    if ((jl_sym_t*)jl_exprarg(atype,1) != dots_sym)
        return 0;
    return 1;
}

static jl_value_t *copy_ast(jl_value_t *expr, jl_tuple_t *sp)
{
    if (jl_is_lambda_info(expr)) {
        return (jl_value_t*)jl_add_static_parameters((jl_lambda_info_t*)expr,
                                                     sp);
    }
    if (jl_typeis(expr,jl_array_any_type)) {
        jl_array_t *a = (jl_array_t*)expr;
        jl_array_t *na = jl_alloc_cell_1d(a->length);
        JL_GC_PUSH(&na);
        size_t i;
        for(i=0; i < a->length; i++)
            jl_cellset(na, i, copy_ast(jl_cellref(a,i), sp));
        JL_GC_POP();
        return (jl_value_t*)na;
    }
    if (jl_is_expr(expr)) {
        jl_expr_t *e = (jl_expr_t*)expr;
        jl_expr_t *ne = jl_exprn(e->head, e->args->length);
        JL_GC_PUSH(&ne);
        size_t i;
        for(i=0; i < e->args->length; i++)
            jl_exprarg(ne, i) = copy_ast(jl_exprarg(e,i), sp);
        JL_GC_POP();
        return (jl_value_t*)ne;
    }
    return expr;
}

static void eval_decl_types(jl_array_t *vi, jl_tuple_t *spenv)
{
    size_t i;
    for(i=0; i < vi->length; i++) {
        jl_array_t *v = (jl_array_t*)jl_cellref(vi, i);
        jl_value_t *ty =
            jl_interpret_toplevel_expr_with(jl_cellref(v,1),
                                            &jl_tupleref(spenv,0),
                                            spenv->length/2);
        jl_cellref(v, 1) = ty;
    }
}

jl_tuple_t *jl_tuple_tvars_to_symbols(jl_tuple_t *t)
{
    jl_tuple_t *s = jl_alloc_tuple_uninit(t->length);
    size_t i;
    for(i=0; i < s->length; i+=2) {
        jl_tupleset(s, i,
                    (jl_value_t*)((jl_tvar_t*)jl_tupleref(t,i))->name);
        jl_tupleset(s, i+1, jl_tupleref(t,i+1));
    }
    return s;
}

// given a new lambda_info with static parameter values, make a copy
// of the tree with declared types evaluated and static parameters passed
// on to all enclosed functions.
// this tree can then be further mutated by optimization passes.
void jl_specialize_ast(jl_lambda_info_t *li)
{
    if (li->ast == NULL) return;
    jl_tuple_t *spenv = jl_tuple_tvars_to_symbols(li->sparams);
    jl_value_t *ast = copy_ast(li->ast, li->sparams);
    li->ast = ast;
    JL_GC_PUSH(&spenv);
    eval_decl_types(jl_lam_vinfo((jl_expr_t*)ast), spenv);
    eval_decl_types(jl_lam_capt((jl_expr_t*)ast), spenv);
    JL_GC_POP();
}
