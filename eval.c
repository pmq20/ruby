/************************************************

  eval.c -

  $Author$
  $Date$
  created at: Thu Jun 10 14:22:17 JST 1993

  Copyright (C) 1993-1998 Yukihiro Matsumoto

************************************************/

#include "ruby.h"
#include "node.h"
#include "env.h"
#include "rubysig.h"

#include <stdio.h>
#include <setjmp.h>
#include "st.h"
#include "dln.h"

#ifndef HAVE_STRING_H
char *strrchr _((char*,char));
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef __BEOS__
#include <net/socket.h>
#endif

#ifdef USE_CWGUSI
#include <sys/stat.h>
#include <sys/errno.h>
#include <compat.h>
#endif

#ifndef setjmp
#ifdef HAVE__SETJMP
#define setjmp(env) _setjmp(env)
#define longjmp(env,val) _longjmp(env,val)
#endif
#endif

#if defined(MSDOS) || defined(NT) || defined(__human68k__) || defined(__MACOS__)
#define RUBY_LIB_SEP ";"
#else
#define RUBY_LIB_SEP ":"
#endif

VALUE rb_cProc;
static VALUE rb_cBinding;
static VALUE proc_call _((VALUE,VALUE));
static VALUE rb_f_binding _((VALUE));
static void rb_f_END _((void));
static VALUE rb_f_iterator_p _((void));
static VALUE block_pass _((VALUE,NODE*));
static VALUE rb_cMethod;
static VALUE method_proc _((VALUE));

static int scope_vmode;
#define SCOPE_PUBLIC    0
#define SCOPE_PRIVATE   1
#define SCOPE_PROTECTED 2
#define SCOPE_MODFUNC   5
#define SCOPE_MASK      7
#define SCOPE_SET(f)  do {scope_vmode=(f);} while(0)
#define SCOPE_TEST(f) (scope_vmode&(f))

#define CACHE_SIZE 0x800
#define CACHE_MASK 0x7ff
#define EXPR1(c,m) ((((c)>>3)^(m))&CACHE_MASK)

struct cache_entry {		/* method hash table. */
    ID mid;			/* method's id */
    ID mid0;			/* method's original id */
    VALUE klass;		/* receiver's class */
    VALUE origin;		/* where method defined  */
    NODE *method;
    int noex;
};

static struct cache_entry cache[CACHE_SIZE];

void
rb_clear_cache()
{
    struct cache_entry *ent, *end;

    ent = cache; end = ent + CACHE_SIZE;
    while (ent < end) {
	ent->mid = 0;
	ent++;
    }
}

static int cache_conflict;
static int eval_called;

static int
count_cent()
{
    struct cache_entry *ent, *end;
    int n = 0, n2 = 0;
    int p = 0;

    fprintf(stderr, "\n|");
    ent = cache; end = ent + CACHE_SIZE;
    while (ent < end) {
	fprintf(stderr, (ent->klass != 0)?"*":" ");
	p++;
	if (p % 80 == 0) {
	    break;
	}
	ent++;
    }
    fprintf(stderr, "|\n|");
    p = 0; ent = cache; 
    while (ent < end) {
	if (ent->klass != 0) {n++; n2++;}
	p++;
	if (p % 24 == 0) {
	    fprintf(stderr, "%c", n2?(n2+'0'):' ');
	    n2 = 0;
	}
	ent++;
    }
    fprintf(stderr, "|\n");
    return n;
}

static void
rb_clear_cache_by_id(id)
    ID id;
{
    struct cache_entry *ent, *end;

    ent = cache; end = ent + CACHE_SIZE;
    while (ent < end) {
	if (ent->mid == id) {
	    ent->mid = 0;
	}
	ent++;
    }
}

void
rb_add_method(klass, mid, node, noex)
    VALUE klass;
    ID mid;
    NODE *node;
    int noex;
{
    NODE *body;

    if (NIL_P(klass)) klass = rb_cObject;
    if (klass == rb_cObject) {
	rb_secure(4);
    }
    body = NEW_METHOD(node, noex);
    st_insert(RCLASS(klass)->m_tbl, mid, body);
}

static NODE*
search_method(klass, id, origin)
    VALUE klass, *origin;
    ID id;
{
    NODE *body;

    while (!st_lookup(RCLASS(klass)->m_tbl, id, &body)) {
	klass = RCLASS(klass)->super;
	if (!klass) return 0;
    }

    if (origin) *origin = klass;
    return body;
}

static NODE*
rb_get_method_body(klassp, idp, noexp)
    VALUE *klassp;
    ID *idp;
    int *noexp;
{
    ID id = *idp;
    VALUE klass = *klassp;
    VALUE origin;
    NODE *body;
    struct cache_entry *ent;

    if ((body = search_method(klass, id, &origin)) == 0) {
	return 0;
    }
    if (!body->nd_body) return 0;

    /* store in cache */
    ent = cache + EXPR1(klass, id);
    ent->klass  = klass;
    ent->noex   = body->nd_noex;
    body = body->nd_body;
    if (nd_type(body) == NODE_FBODY) {
	ent->mid = id;
	*klassp = body->nd_orig;
	ent->origin = body->nd_orig;
	*idp = ent->mid0 = body->nd_mid;
	body = ent->method = body->nd_head;
    }
    else {
	*klassp = origin;
	ent->origin = origin;
	ent->mid = ent->mid0 = id;
	ent->method = body;
    }

    if (noexp) *noexp = ent->noex;
    return body;
}

void
rb_alias(klass, name, def)
    VALUE klass;
    ID name, def;
{
    VALUE origin;
    NODE *orig, *body;

    if (name == def) return;
    if (klass == rb_cObject) {
	rb_secure(4);
    }
    orig = search_method(klass, def, &origin);
    if (!orig || !orig->nd_body) {
	if (TYPE(klass) == T_MODULE) {
	    orig = search_method(rb_cObject, def, &origin);
	}
    }
    if (!orig || !orig->nd_body) {
	rb_raise(rb_eNameError, "undefined method `%s' for `%s'",
		 rb_id2name(def), rb_class2name(klass));
    }
    body = orig->nd_body;
    if (nd_type(body) == NODE_FBODY) { /* was alias */
	body = body->nd_head;
	def = body->nd_mid;
	origin = body->nd_orig;
    }

    st_insert(RCLASS(klass)->m_tbl, name,
	      NEW_METHOD(NEW_FBODY(body, def, origin), orig->nd_noex));
}

static void
remove_method(klass, mid)
    VALUE klass;
    ID mid;
{
    NODE *body;

    if (klass == rb_cObject) {
	rb_secure(4);
    }
    if (!st_delete(RCLASS(klass)->m_tbl, &mid, &body)) {
	rb_raise(rb_eNameError, "method `%s' not defined in %s",
		 rb_id2name(mid), rb_class2name(klass));
    }
    rb_clear_cache_by_id(mid);
}

void
rb_remove_method(klass, name)
    VALUE klass;
    char *name;
{
    remove_method(klass, rb_intern(name));
}

void
rb_disable_super(klass, name)
    VALUE klass;
    char *name;
{
    VALUE origin;
    NODE *body;
    ID mid = rb_intern(name);

    body = search_method(klass, mid, &origin);
    if (!body || !body->nd_body) {
	rb_raise(rb_eNameError, "undefined method `%s' for `%s'",
		 rb_id2name(mid), rb_class2name(klass));
    }
    if (origin == klass) {
	body->nd_noex |= NOEX_UNDEF;
    }
    else {
	rb_clear_cache_by_id(mid);
	rb_add_method(ruby_class, mid, 0, NOEX_UNDEF);
    }
}

void
rb_enable_super(klass, name)
    VALUE klass;
    char *name;
{
    VALUE origin;
    NODE *body;
    ID mid = rb_intern(name);

    body = search_method(klass, mid, &origin);
    if (!body || !body->nd_body || origin != klass) {
	rb_raise(rb_eNameError, "undefined method `%s' for `%s'",
		 rb_id2name(mid), rb_class2name(klass));
    }
    body->nd_noex &= ~NOEX_UNDEF;
}

static void
rb_export_method(klass, name, noex)
    VALUE klass;
    ID name;
    int noex;
{
    NODE *body;
    VALUE origin;

    if (klass == rb_cObject) {
	rb_secure(4);
    }
    body = search_method(klass, name, &origin);
    if (!body && TYPE(klass) == T_MODULE) {
	body = search_method(rb_cObject, name, &origin);
    }
    if (!body) {
	rb_raise(rb_eNameError, "undefined method `%s' for `%s'",
		 rb_id2name(name), rb_class2name(klass));
    }
    if (body->nd_noex != noex) {
	if (klass == origin) {
	    body->nd_noex = noex;
	}
	else {
	    rb_clear_cache_by_id(name);
	    rb_add_method(klass, name, NEW_ZSUPER(), noex);
	}
    }
}

int
rb_method_boundp(klass, id, ex)
    VALUE klass;
    ID id;
    int ex;
{
    int noex;

    if (rb_get_method_body(&klass, &id, &noex)) {
	if (ex && noex & NOEX_PRIVATE)
	    return Qfalse;
	return Qtrue;
    }
    return Qfalse;
}

void
rb_attr(klass, id, read, write, ex)
    VALUE klass;
    ID id;
    int read, write, ex;
{
    char *name;
    char *buf;
    ID attr, attriv;
    int noex;

    if (!ex) noex = NOEX_PUBLIC;
    else {
	if (SCOPE_TEST(SCOPE_PRIVATE)) {
	    noex = NOEX_PRIVATE;
	    rb_warning("private attribute?");
	}
	else if (SCOPE_TEST(SCOPE_PROTECTED)) {
	    noex = NOEX_PROTECTED;
	}
	else {
	    noex = NOEX_PUBLIC;
	}
    }

    name = rb_id2name(id);
    if (!name) {
	rb_raise(rb_eArgError, "argument needs to be symbol or string");
    }
    buf = ALLOCA_N(char,strlen(name)+2);
    sprintf(buf, "@%s", name);
    attriv = rb_intern(buf);
    if (read) {
	rb_add_method(klass, id, NEW_IVAR(attriv), noex);
    }
    sprintf(buf, "%s=", name);
    id = rb_intern(buf);
    if (write) {
	rb_add_method(klass, id, NEW_ATTRSET(attriv), noex);
    }
}

static ID init, eqq, each, aref, aset, match;
VALUE rb_errinfo = Qnil;
extern NODE *ruby_eval_tree_begin;
extern NODE *ruby_eval_tree;
extern int ruby_nerrs;

static VALUE rb_eLocalJumpError;
static VALUE rb_eSysStackError;

extern VALUE rb_top_self;

struct FRAME *ruby_frame;
struct SCOPE *ruby_scope;
static struct FRAME *top_frame;
static struct SCOPE *top_scope;

#define PUSH_FRAME() {			\
    struct FRAME _frame;		\
    _frame.prev = ruby_frame;		\
    _frame.file = ruby_sourcefile;	\
    _frame.line = ruby_sourceline;	\
    _frame.iter = ruby_iter->iter;	\
    _frame.cbase = ruby_frame->cbase;	\
    _frame.argc = 0;			\
    ruby_frame = &_frame;		\

#define POP_FRAME()  ruby_frame = _frame.prev; }

struct BLOCK {
    NODE *var;
    NODE *body;
    VALUE self;
    struct FRAME frame;
    struct SCOPE *scope;
    VALUE klass;
    struct tag *tag;
    int iter;
    int vmode;
    int used;
    struct RVarmap *d_vars;
#ifdef USE_THREAD
    VALUE orig_thread;
#endif
    struct BLOCK *prev;
};
static struct BLOCK *ruby_block;
static struct BLOCK *ruby_calling_block;

#define PUSH_BLOCK(v,b) {		\
    struct BLOCK _block;		\
    _block.tag = prot_tag;		\
    _block.var = v;			\
    _block.body = b;			\
    _block.self = self;			\
    _block.frame = *ruby_frame;		\
    _block.klass = ruby_class;		\
    _block.frame.file = ruby_sourcefile;\
    _block.frame.line = ruby_sourceline;\
    _block.scope = ruby_scope;		\
    _block.d_vars = ruby_dyna_vars;	\
    _block.prev = ruby_block;		\
    _block.iter = ruby_iter->iter;	\
    _block.vmode = scope_vmode;		\
    _block.used = 0;			\
    ruby_block = &_block;

#define POP_BLOCK() 			\
   ruby_block = _block.prev; 		\
}

#define PUSH_BLOCK2(b) {		\
    struct BLOCK * volatile _old;	\
    struct BLOCK * volatile _old_call;	\
    _old = ruby_block;			\
    _old_call = ruby_calling_block;	\
    ruby_calling_block = b;		\
    ruby_block = b;

#define POP_BLOCK2() 			\
   ruby_calling_block = _old_call;	\
   ruby_block = _old;	 		\
}

struct RVarmap *ruby_dyna_vars;
#define PUSH_VARS() {			\
    struct RVarmap * volatile _old;	\
    _old = ruby_dyna_vars;		\
    ruby_dyna_vars = 0;

#define POP_VARS()			\
    ruby_dyna_vars = _old;		\
}

static struct RVarmap*
new_dvar(id, value)
    ID id;
    VALUE value;
{
    NEWOBJ(vars, struct RVarmap);
    OBJSETUP(vars, 0, T_VARMAP);
    vars->id = id;
    vars->val = value;
    vars->next = ruby_dyna_vars;

    return vars;
}

VALUE
rb_dvar_defined(id)
    ID id;
{
    struct RVarmap *vars = ruby_dyna_vars;

    while (vars) {
	if (vars->id == id) return Qtrue;
	vars = vars->next;
    }
    return Qfalse;
}

static int
dvar_len()
{
    struct RVarmap *vars = ruby_dyna_vars;
    int n = 0;

    while (vars) {
	n++;
	vars = vars->next;
    }
    return n;
}

VALUE
rb_dvar_ref(id)
    ID id;
{
    struct RVarmap *vars = ruby_dyna_vars;

    while (vars) {
	if (vars->id == id) {
	    return vars->val;
	}
	vars = vars->next;
    }
    return Qnil;
}

void
rb_dvar_push(id, value)
    ID id;
    VALUE value;
{
    ruby_dyna_vars = new_dvar(id, value);
}

void
rb_dvar_asgn(id, value)
    ID id;
    VALUE value;
{
    struct RVarmap *vars = ruby_dyna_vars;

    while (vars) {
	if (vars->id == id) {
	    vars->val = value;
	    return;
	}
	vars = vars->next;
    }
    rb_dvar_push(id, value);
    return;
}

static void
dvar_asgn_push(id, value)
    ID id;
    VALUE value;
{
    rb_dvar_asgn(id, value);
    if (ruby_calling_block) {
	ruby_calling_block->d_vars = ruby_dyna_vars;
    }
}

struct iter {
    int iter;
    struct iter *prev;
};
static struct iter *ruby_iter;

#define ITER_NOT 0
#define ITER_PRE 1
#define ITER_CUR 2

#define PUSH_ITER(i) {			\
    struct iter _iter;			\
    _iter.prev = ruby_iter;		\
    _iter.iter = (i);			\
    ruby_iter = &_iter;			\

#define POP_ITER()			\
    ruby_iter = _iter.prev;		\
}

struct tag {
    jmp_buf buf;
    struct FRAME *frame;
    struct iter *iter;
    ID tag;
    VALUE retval;
    ID dst;
    struct tag *prev;
};
static struct tag *prot_tag;

#define PUSH_TAG(ptag) {		\
    struct tag _tag;			\
    _tag.retval = Qnil;			\
    _tag.frame = ruby_frame;		\
    _tag.iter = ruby_iter;		\
    _tag.prev = prot_tag;		\
    _tag.retval = Qnil;			\
    _tag.tag = ptag;			\
    _tag.dst = 0;			\
    prot_tag = &_tag;

#define PROT_NONE   0
#define PROT_FUNC   -1
#define PROT_THREAD -2

#define EXEC_TAG()    setjmp(prot_tag->buf)

#define JUMP_TAG(st) {			\
    ruby_frame = prot_tag->frame;	\
    ruby_iter = prot_tag->iter;		\
    longjmp(prot_tag->buf,(st));	\
}

#define POP_TAG()			\
    if (_tag.prev)			\
        _tag.prev->retval = _tag.retval;\
    prot_tag = _tag.prev;		\
}

#define TAG_RETURN	0x1
#define TAG_BREAK	0x2
#define TAG_NEXT	0x3
#define TAG_RETRY	0x4
#define TAG_REDO	0x5
#define TAG_RAISE	0x6
#define TAG_THROW	0x7
#define TAG_FATAL	0x8
#define TAG_MASK	0xf

VALUE ruby_class;

#define PUSH_CLASS() {		\
    VALUE _class = ruby_class;	\

#define POP_CLASS() ruby_class = _class; }

#define PUSH_SCOPE() {			\
    int volatile _vmode = scope_vmode;	\
    struct SCOPE * volatile _old;	\
    NEWOBJ(_scope, struct SCOPE);	\
    OBJSETUP(_scope, 0, T_SCOPE);	\
    _scope->local_tbl = 0;		\
    _scope->local_vars = 0;		\
    _scope->flag = 0;			\
    _old = ruby_scope;			\
    ruby_scope = _scope;		\
    scope_vmode = SCOPE_PUBLIC;

#define POP_SCOPE() \
    if (ruby_scope->flag == SCOPE_ALLOCA) {\
	ruby_scope->local_vars = 0;\
	ruby_scope->local_tbl  = 0;\
	if (ruby_scope != top_scope)\
            rb_gc_force_recycle((VALUE)ruby_scope);\
    }\
    else {\
        ruby_scope->flag |= SCOPE_NOSTACK;\
    }\
    ruby_scope = _old;\
    scope_vmode = _vmode;\
}

static VALUE rb_eval _((VALUE,NODE*));
static VALUE eval _((VALUE,VALUE,VALUE,char*,int));
static NODE *compile _((VALUE,char*));
static VALUE rb_yield_0 _((VALUE, VALUE, VALUE));

static VALUE rb_call _((VALUE,VALUE,ID,int,VALUE*,int));
static VALUE rb_module_setup _((VALUE,NODE*));

static VALUE massign _((VALUE,NODE*,VALUE));
static void assign _((VALUE,NODE*,VALUE));

static int safe_level = 0;
/* safe-level:
   0 - strings from streams/environment/ARGV are tainted (default)
   1 - no dangerous operation by tainted string
   2 - process/file operations prohibited
   3 - all genetated strings are tainted
   4 - no global (non-tainted) variable modification/no direct output
*/

int
rb_safe_level()
{
    return safe_level;
}

void
rb_set_safe_level(level)
    int level;
{
    if (level > safe_level) {
	safe_level = level;
    }
}

static VALUE
safe_getter()
{
    return INT2FIX(safe_level);
}

static void
safe_setter(val)
    VALUE val;
{
    int level = NUM2INT(val);

    if (level < safe_level) {
	rb_raise(rb_eSecurityError, "tried to downgrade safe level from %d to %d",
		 safe_level, level);
    }
    safe_level = level;
}

void
rb_check_safe_str(x)
    VALUE x;
{
    if (TYPE(x)!= T_STRING) {
	rb_raise(rb_eTypeError, "wrong argument type %s (expected String)",
		 rb_class2name(CLASS_OF(x)));
    }
    if (rb_obj_tainted(x)) {
	if (safe_level > 0){
	    rb_raise(rb_eSecurityError, "Insecure operation - %s",
		     rb_id2name(ruby_frame->last_func));
	}
    }
}

void
rb_secure(level)
    int level;
{
    if (level <= safe_level) {
	rb_raise(rb_eSecurityError, "Insecure operation `%s' for level %d",
		 rb_id2name(ruby_frame->last_func), safe_level);
    }
}

static VALUE trace_func = 0;
static void call_trace_func _((char*,char*,int,VALUE,ID,VALUE));

static void
error_pos()
{
    if (ruby_sourcefile) {
	if (ruby_frame->last_func) {
	    fprintf(stderr, "%s:%d:in `%s'", ruby_sourcefile, ruby_sourceline,
		    rb_id2name(ruby_frame->last_func));
	}
	else {
	    fprintf(stderr, "%s:%d", ruby_sourcefile, ruby_sourceline);
	}
    }
}

static VALUE
get_backtrace(info)
    VALUE info;
{
    if (NIL_P(info)) return Qnil;
    return rb_funcall(info, rb_intern("backtrace"), 0);
}

static void
set_backtrace(info, bt)
    VALUE info, bt;
{
    rb_funcall(info, rb_intern("set_backtrace"), 1, bt);
}

static void
error_print()
{
    VALUE errat;
    VALUE eclass;
    VALUE einfo;

    if (NIL_P(rb_errinfo)) return;

    errat = get_backtrace(rb_errinfo);
    if (!NIL_P(errat)) {
	VALUE mesg = RARRAY(errat)->ptr[0];

	if (NIL_P(mesg)) error_pos();
	else {
	    fwrite(RSTRING(mesg)->ptr, 1, RSTRING(mesg)->len, stderr);
	}
    }

    eclass = CLASS_OF(rb_errinfo);
    einfo = rb_obj_as_string(rb_errinfo);
    if (eclass == rb_eRuntimeError && RSTRING(einfo)->len == 0) {
	fprintf(stderr, ": unhandled exception\n");
    }
    else {
	VALUE epath;

	epath = rb_class_path(eclass);
	if (RSTRING(einfo)->len == 0) {
	    fprintf(stderr, ": ");
	    fwrite(RSTRING(epath)->ptr, 1, RSTRING(epath)->len, stderr);
	    putc('\n', stderr);
	}
	else {
	    char *tail  = 0;
	    int len = RSTRING(einfo)->len;

	    if (RSTRING(epath)->ptr[0] == '#') epath = 0;
	    if (tail = strchr(RSTRING(einfo)->ptr, '\n')) {
		len = tail - RSTRING(einfo)->ptr;
		tail++;		/* skip newline */
	    }
	    fprintf(stderr, ": ");
	    fwrite(RSTRING(einfo)->ptr, 1, len, stderr);
	    if (epath) {
		fprintf(stderr, " (");
		fwrite(RSTRING(epath)->ptr, 1, RSTRING(epath)->len, stderr);
		fprintf(stderr, ")\n");
	    }
	    if (tail) {
		fwrite(tail, 1, RSTRING(einfo)->len-len-1, stderr);
		putc('\n', stderr);
	    }
	}
    }

    if (!NIL_P(errat)) {
	int i;
	struct RArray *ep = RARRAY(errat);

#define TRACE_MAX (TRACE_HEAD+TRACE_TAIL+5)
#define TRACE_HEAD 8
#define TRACE_TAIL 5

	ep = RARRAY(errat);
	for (i=1; i<ep->len; i++) {
	    fprintf(stderr, "\tfrom %s\n", RSTRING(ep->ptr[i])->ptr);
	    if (i == TRACE_HEAD && ep->len > TRACE_MAX) {
		fprintf(stderr, "\t ... %d levels...\n",
			ep->len - TRACE_HEAD - TRACE_TAIL);
		i = ep->len - TRACE_TAIL;
	    }
	}
    }
}

#if !defined(NT) && !defined(__MACOS__)
extern char **environ;
#endif

void rb_call_inits _((void));
void Init_stack _((void));
void Init_heap _((void));
void Init_ext _((void));

void
ruby_init()
{
    static struct FRAME frame;
    static struct iter iter;
    int state;

    ruby_frame = top_frame = &frame;
    ruby_iter = &iter;

    Init_heap();
    PUSH_SCOPE();
    ruby_scope->local_vars = 0;
    ruby_scope->local_tbl  = 0;
    top_scope = ruby_scope;
    /* default visibility is private at toplevel */
    SCOPE_SET(SCOPE_PRIVATE);

    PUSH_TAG(PROT_NONE)
    if ((state = EXEC_TAG()) == 0) {
	rb_call_inits();
	ruby_class = rb_cObject;
	ruby_frame->self = rb_top_self;
	ruby_frame->cbase = (VALUE)rb_node_newnode(NODE_CREF,rb_cObject,0,0);
	rb_define_global_const("TOPLEVEL_BINDING", rb_f_binding(rb_top_self));
	ruby_prog_init();
    }
    POP_TAG();
    if (state) error_print();
    POP_SCOPE();
    ruby_scope = top_scope;
}

static int ext_init = 0;

void
ruby_options(argc, argv)
    int argc;
    char **argv;
{
    int state;

    PUSH_TAG(PROT_NONE)
    if ((state = EXEC_TAG()) == 0) {
	NODE *save;

	ruby_process_options(argc, argv);
	ext_init = 1;	/* Init_ext() called in ruby_process_options */
	save = ruby_eval_tree;
	ruby_require_modules();
	ruby_eval_tree = save;
    }
    POP_TAG();
    if (state) {
	trace_func = 0;
	error_print();
	exit(1);
    }
}

static VALUE
eval_node(self)
    VALUE self;
{
    VALUE result = Qnil;
    NODE *tree;

    if (ruby_eval_tree_begin) {
	tree = ruby_eval_tree_begin;
	ruby_eval_tree_begin = 0;
	rb_eval(self, tree);
    }

    if (!ruby_eval_tree) return Qnil;

    tree = ruby_eval_tree;
    ruby_eval_tree = 0;

    result = rb_eval(self, tree);
    return result;
}

int rb_in_eval;

#ifdef USE_THREAD
static void rb_thread_cleanup _((void));
static void rb_thread_wait_other_threads _((void));
static VALUE rb_thread_current _((void));
#endif

static int exit_status;

static void exec_end_proc _((void));

void
ruby_run()
{
    int state;
    static int ex;

    if (ruby_nerrs > 0) exit(ruby_nerrs);

    Init_stack();

    PUSH_TAG(PROT_NONE);
    PUSH_ITER(ITER_NOT);
    if ((state = EXEC_TAG()) == 0) {
	if (!ext_init) Init_ext();
	eval_node(rb_top_self);
    }
    POP_ITER();
    POP_TAG();

    if (state && !ex) ex = state;
    PUSH_TAG(PROT_NONE);
    PUSH_ITER(ITER_NOT);
    if ((state = EXEC_TAG()) == 0) {
	rb_trap_exit();
#ifdef USE_THREAD
	rb_thread_cleanup();
	rb_thread_wait_other_threads();
#endif
	exec_end_proc();
	rb_gc_call_finalizer_at_exit();
#if 1
	fprintf(stderr, "%d/%d(%d)\n", count_cent(), CACHE_SIZE, cache_conflict);
#else
	fprintf(stderr, "rb_eval() called %d times\n", eval_called);
#endif
    }
    else {
	ex = state;
    }
    POP_ITER();
    POP_TAG();

    switch (ex & 0xf) {
      case 0:
	exit(0);

      case TAG_RETURN:
	error_pos();
	fprintf(stderr, ": unexpected return\n");
	exit(1);
	break;
      case TAG_NEXT:
	error_pos();
	fprintf(stderr, ": unexpected next\n");
	exit(1);
	break;
      case TAG_BREAK:
	error_pos();
	fprintf(stderr, ": unexpected break\n");
	exit(1);
	break;
      case TAG_REDO:
	error_pos();
	fprintf(stderr, ": unexpected redo\n");
	exit(1);
	break;
      case TAG_RETRY:
	error_pos();
	fprintf(stderr, ": retry outside of rescue clause\n");
	exit(1);
	break;
      case TAG_RAISE:
      case TAG_FATAL:
	if (rb_obj_is_kind_of(rb_errinfo, rb_eSystemExit)) {
	    exit(exit_status);
	}
	error_print();
	exit(1);
	break;
      default:
	rb_bug("Unknown longjmp status %d", ex);
	break;
    }
}

static void
compile_error(at)
    char *at;
{
    VALUE str;
    char *mesg;
    int len;

    mesg = str2cstr(rb_errinfo, &len);
    ruby_nerrs = 0;
    str = rb_str_new2("compile error");
    if (at) {
	rb_str_cat(str, " in ", 4);
	rb_str_cat(str, at, strlen(at));
    }
    rb_str_cat(str, "\n", 1);
    rb_str_cat(str, mesg, len);
    rb_exc_raise(rb_exc_new3(rb_eSyntaxError, str));
}

VALUE
rb_eval_string(str)
    char *str;
{
    VALUE v;
    char *oldsrc = ruby_sourcefile;

    ruby_sourcefile = "(eval)";
    v = eval(rb_top_self, rb_str_new2(str), Qnil, 0, 0);
    ruby_sourcefile = oldsrc;

    return v;
}

VALUE
rb_eval_cmd(cmd, arg)
    VALUE cmd, arg;
{
    int state;
    VALUE val;
    struct SCOPE *saved_scope;
    volatile int safe = rb_safe_level();

    if (TYPE(cmd) != T_STRING) {
	return rb_funcall2(cmd, rb_intern("call"),
			   RARRAY(arg)->len, RARRAY(arg)->ptr);
    }

    PUSH_CLASS();
    PUSH_TAG(PROT_NONE);
    saved_scope = ruby_scope;
    ruby_scope = top_scope;

    ruby_class = rb_cObject;
    if (rb_obj_tainted(cmd)) {
	safe_level = 4;
    }

    if ((state = EXEC_TAG()) == 0) {
	val = eval(rb_top_self, cmd, Qnil, 0, 0);
    }

    ruby_scope = saved_scope;
    safe_level = safe;
    POP_TAG();
    POP_CLASS();

    switch (state) {
      case 0:
	break;
      case TAG_RETURN:
	rb_raise(rb_eLocalJumpError, "unexpected return");
	break;
      case TAG_NEXT:
	rb_raise(rb_eLocalJumpError, "unexpected next");
	break;
      case TAG_BREAK:
	rb_raise(rb_eLocalJumpError, "unexpected break");
	break;
      case TAG_REDO:
	rb_raise(rb_eLocalJumpError, "unexpected redo");
	break;
      case TAG_RETRY:
	rb_raise(rb_eLocalJumpError, "retry outside of rescue clause");
	break;
      default:
	JUMP_TAG(state);
	break;
    }
    return val;
}

VALUE
rb_trap_eval(cmd, sig)
    VALUE cmd;
    int sig;
{
    int state;
    VALUE val;			/* OK */

    PUSH_TAG(PROT_NONE);
    if ((state = EXEC_TAG()) == 0) {
	val = rb_eval_cmd(cmd, rb_ary_new3(1, INT2FIX(sig)));
    }
    POP_TAG();
    if (state) {
	rb_trap_immediate = 0;
	JUMP_TAG(state);
    }
    return val;
}

static VALUE
superclass(self, node)
    VALUE self;
    NODE *node;
{
    VALUE val;			/* OK */
    int state;

    PUSH_TAG(PROT_NONE);
    if ((state = EXEC_TAG()) == 0) {
	val = rb_eval(self, node);
    }
    POP_TAG();
    if (state) {
      superclass_error:
	switch (nd_type(node)) {
	  case NODE_COLON2:
	    rb_raise(rb_eTypeError, "undefined superclass `%s'",
		     rb_id2name(node->nd_mid));
	  case NODE_CVAR:
	    rb_raise(rb_eTypeError, "undefined superclass `%s'",
		     rb_id2name(node->nd_vid));
	  default:
	    rb_raise(rb_eTypeError, "superclass undefined");
	}
	JUMP_TAG(state);
    }
    if (TYPE(val) != T_CLASS) goto superclass_error;
    if (FL_TEST(val, FL_SINGLETON)) {
	rb_raise(rb_eTypeError, "can't make subclass of virtual class");
    }

    return val;
}

static VALUE
ev_const_defined(cref, id)
    NODE *cref;
    ID id;
{
    NODE *cbase = cref;

    while (cbase && cbase->nd_clss != rb_cObject) {
	struct RClass *klass = RCLASS(cbase->nd_clss);

	if (klass->iv_tbl &&
	    st_lookup(klass->iv_tbl, id, 0)) {
	    return Qtrue;
	}
	cbase = cbase->nd_next;
    }
    return rb_const_defined(cref->nd_clss, id);
}

static VALUE
ev_const_get(cref, id)
    NODE *cref;
    ID id;
{
    NODE *cbase = cref;
    VALUE result;

    while (cbase && cbase->nd_clss != rb_cObject) {
	struct RClass *klass = RCLASS(cbase->nd_clss);

	if (klass->iv_tbl &&
	    st_lookup(klass->iv_tbl, id, &result)) {
	    return result;
	}
	cbase = cbase->nd_next;
    }
    return rb_const_get(cref->nd_clss, id);
}

static VALUE
rb_mod_nesting()
{
    NODE *cbase = (NODE*)ruby_frame->cbase;
    VALUE ary = rb_ary_new();

    while (cbase && cbase->nd_clss != rb_cObject) {
	rb_ary_push(ary, cbase->nd_clss);
	cbase = cbase->nd_next;
    }
    return ary;
}

static VALUE
rb_mod_s_constants()
{
    NODE *cbase = (NODE*)ruby_frame->cbase;
    VALUE ary = rb_ary_new();

    while (cbase && cbase->nd_clss != rb_cObject) {
	rb_mod_const_at(cbase->nd_clss, ary);
	cbase = cbase->nd_next;
    }

    rb_mod_const_of(((NODE*)ruby_frame->cbase)->nd_clss, ary);
    return ary;
}

static VALUE
rb_mod_remove_method(mod, name)
    VALUE mod, name;
{
    remove_method(mod, rb_to_id(name));
    return mod;
}

static VALUE
rb_mod_undef_method(mod, name)
    VALUE mod, name;
{
    ID id = rb_to_id(name);

    rb_add_method(mod, id, 0, NOEX_PUBLIC);
    rb_clear_cache_by_id(id);
    return mod;
}

static VALUE
rb_mod_alias_method(mod, newname, oldname)
    VALUE mod, newname, oldname;
{
    ID id = rb_to_id(newname);

    rb_alias(mod, id, rb_to_id(oldname));
    rb_clear_cache_by_id(id);
    return mod;
}

#if defined(C_ALLOCA) && defined(USE_THREAD)
# define TMP_PROTECT NODE *__protect_tmp=0
# define TMP_ALLOC(type,n)						   \
    (__protect_tmp = rb_node_newnode(NODE_ALLOCA,				   \
			     rb_str_new(0,sizeof(type)*(n)),0,__protect_tmp), \
     (void*)RSTRING(__protect_tmp->nd_head)->ptr)
#else
# define TMP_PROTECT typedef int foobazzz
# define TMP_ALLOC(type,n) ALLOCA_N(type,n)
#endif

#define SETUP_ARGS(anode) {\
    NODE *n = anode;\
    if (!n) {\
	argc = 0;\
	argv = 0;\
    }\
    else if (nd_type(n) == NODE_ARRAY) {\
	argc=n->nd_alen;\
        if (argc > 0) {\
	    char *file = ruby_sourcefile;\
	    int line = ruby_sourceline;\
            int i;\
	    n = anode;\
	    argv = TMP_ALLOC(VALUE,argc);\
	    for (i=0;i<argc;i++) {\
		argv[i] = rb_eval(self,n->nd_head);\
		n=n->nd_next;\
	    }\
	    ruby_sourcefile = file;\
	    ruby_sourceline = line;\
        }\
        else {\
	    argc = 0;\
	    argv = 0;\
        }\
    }\
    else {\
        VALUE args = rb_eval(self,n);\
	char *file = ruby_sourcefile;\
	int line = ruby_sourceline;\
	if (TYPE(args) != T_ARRAY)\
	    args = rb_Array(args);\
        argc = RARRAY(args)->len;\
	argv = ALLOCA_N(VALUE, argc);\
	MEMCPY(argv, RARRAY(args)->ptr, VALUE, argc);\
	ruby_sourcefile = file;\
	ruby_sourceline = line;\
    }\
}

#define BEGIN_CALLARGS {\
    struct BLOCK *tmp_block = ruby_block;\
    if (ruby_iter->iter == ITER_PRE) {\
	ruby_block = ruby_block->prev;\
    }\
    PUSH_ITER(ITER_NOT);

#define END_CALLARGS \
    ruby_block = tmp_block;\
    POP_ITER();\
}

#define MATCH_DATA ruby_scope->local_vars[node->nd_cnt]

static char* is_defined _((VALUE, NODE*, char*));

static char*
arg_defined(self, node, buf, type)
    VALUE self;
    NODE *node;
    char *buf;
    char *type;
{
    int argc;
    int i;

    if (!node) return type;	/* no args */
    if (nd_type(node) == NODE_ARRAY) {
	argc=node->nd_alen;
        if (argc > 0) {
	    for (i=0;i<argc;i++) {
		if (!is_defined(self, node->nd_head, buf))
		    return 0;
		node = node->nd_next;
	    }
        }
    }
    else if (!is_defined(self, node, buf)) {
	return 0;
    }
    return type;
}
    
static char*
is_defined(self, node, buf)
    VALUE self;
    NODE *node;			/* OK */
    char *buf;
{
    VALUE val;			/* OK */
    int state;

    switch (nd_type(node)) {
      case NODE_SUPER:
      case NODE_ZSUPER:
	if (ruby_frame->last_func == 0) return 0;
	else if (rb_method_boundp(RCLASS(ruby_frame->last_class)->super,
				  ruby_frame->last_func, 1)) {
	    if (nd_type(node) == NODE_SUPER) {
		return arg_defined(self, node->nd_args, buf, "super");
	    }
	    return "super";
	}
	break;

      case NODE_VCALL:
      case NODE_FCALL:
	val = CLASS_OF(self);
	goto check_bound;

      case NODE_CALL:
	if (!is_defined(self, node->nd_recv, buf)) return 0;
	PUSH_TAG(PROT_NONE);
	if ((state = EXEC_TAG()) == 0) {
	    val = rb_eval(self, node->nd_recv);
	    val = CLASS_OF(val);
	}
	POP_TAG();
	if (state) return 0;
      check_bound:
	if (rb_method_boundp(val, node->nd_mid, nd_type(node)== NODE_CALL)) {
	    return arg_defined(self, node->nd_args, buf, "method");
	}
	break;

      case NODE_MATCH2:
      case NODE_MATCH3:
	return "method";

      case NODE_YIELD:
	if (rb_iterator_p()) {
	    return "yield";
	}
	break;

      case NODE_SELF:
	return "self";

      case NODE_NIL:
	return "nil";

      case NODE_TRUE:
	return "true";

      case NODE_FALSE:
	return "false";

      case NODE_ATTRSET:
      case NODE_OP_ASGN1:
      case NODE_OP_ASGN2:
      case NODE_MASGN:
      case NODE_LASGN:
      case NODE_DASGN:
      case NODE_DASGN_PUSH:
      case NODE_GASGN:
      case NODE_IASGN:
      case NODE_CASGN:
	return "assignment";

      case NODE_LVAR:
	return "local-variable";
      case NODE_DVAR:
	return "local-variable(in-block)";

      case NODE_GVAR:
	if (rb_gvar_defined(node->nd_entry)) {
	    return "global-variable";
	}
	break;

      case NODE_IVAR:
	if (rb_ivar_defined(self, node->nd_vid)) {
	    return "instance-variable";
	}
	break;

      case NODE_CVAR:
	if (ev_const_defined((NODE*)ruby_frame->cbase, node->nd_vid)) {
	    return "constant";
	}
	break;

      case NODE_COLON2:
	PUSH_TAG(PROT_NONE);
	if ((state = EXEC_TAG()) == 0) {
	    val = rb_eval(self, node->nd_head);
	}
	POP_TAG();
	if (state) return 0;
	else {
	    switch (TYPE(val)) {
	      case T_CLASS:
	      case T_MODULE:
		if (rb_const_defined_at(val, node->nd_mid))
		    return "constant";
	    }
	}
	break;

      case NODE_NTH_REF:
	if (rb_reg_nth_defined(node->nd_nth, MATCH_DATA)) {
	    sprintf(buf, "$%d", node->nd_nth);
	    return buf;
	}
	break;

      case NODE_BACK_REF:
	if (rb_reg_nth_defined(0, MATCH_DATA)) {
	    sprintf(buf, "$%c", node->nd_nth);
	    return buf;
	}
	break;

      default:
	PUSH_TAG(PROT_NONE);
	if ((state = EXEC_TAG()) == 0) {
	    rb_eval(self, node);
	}
	POP_TAG();
	if (!state) {
	    return "expression";
	}
	break;
    }
    return 0;
}

static int handle_rescue _((VALUE,NODE*));

static void blk_free();

static VALUE
rb_obj_is_block(block)
    VALUE block;
{
    if (TYPE(block) == T_DATA && RDATA(block)->dfree == blk_free) {
	return Qtrue;
    }
    return Qfalse;
}

static VALUE
rb_obj_is_proc(proc)
    VALUE proc;
{
    if (rb_obj_is_block(proc) && rb_obj_is_kind_of(proc, rb_cProc)) {
	return Qtrue;
    }
    return Qfalse;
}

static VALUE
set_trace_func(obj, trace)
    VALUE obj, trace;
{
    if (NIL_P(trace)) {
	trace_func = 0;
	return Qnil;
    }
    if (!rb_obj_is_proc(trace)) {
	rb_raise(rb_eTypeError, "trace_func needs to be Proc");
    }
    return trace_func = trace;
}

static void
call_trace_func(event, file, line, self, id, klass)
    char *event;
    char *file;
    int line;
    VALUE self;
    ID id;
    VALUE klass;
{
    int state;
    volatile VALUE trace;
    struct FRAME *prev;
    char *file_save = ruby_sourcefile;
    int line_save = ruby_sourceline;

    if (!trace_func) return;

    trace = trace_func;
    trace_func = 0;
#ifdef USE_THREAD
    rb_thread_critical++;
#endif

    prev = ruby_frame;
    PUSH_FRAME();
    *ruby_frame = *_frame.prev;
    ruby_frame->prev = prev;

    if (file) {
	ruby_frame->line = ruby_sourceline = line;
	ruby_frame->file = ruby_sourcefile = file;
    }
    if (klass) {
	if (TYPE(klass) == T_ICLASS || FL_TEST(klass, FL_SINGLETON)) {
	    klass = self;
	}
    }
    PUSH_TAG(PROT_NONE);
    if ((state = EXEC_TAG()) == 0) {
	proc_call(trace, rb_ary_new3(6, rb_str_new2(event),
				     rb_str_new2(ruby_sourcefile),
				     INT2FIX(ruby_sourceline),
				     INT2FIX(id),
				     self?rb_f_binding(self):Qnil,
				     klass));
    }
    POP_TAG();
    POP_FRAME();

#ifdef USE_THREAD
    rb_thread_critical--;
#endif
    if (!trace_func) trace_func = trace;
    ruby_sourceline = line_save;
    ruby_sourcefile = file_save;
    if (state) JUMP_TAG(state);
}

static void return_check _((void));
#define return_value(v) prot_tag->retval = (v)

static VALUE
rb_eval(self, node)
    VALUE self;
    NODE * volatile node;
{
    int state;
    volatile VALUE result = Qnil;
#ifdef NOBLOCK_RECUR
    NODE * volatile next = 0;
    NODE * volatile nstack = 0;
#endif

#define RETURN(v) { result = (v); goto finish; }

    eval_called++;

  again:
    if (!node) RETURN(Qnil);

    switch (nd_type(node)) {
      case NODE_BLOCK:
#ifndef NOBLOCK_RECUR
	if (!node->nd_next) {
	    node = node->nd_head;
	    goto again;
	}
	while (node) {
	    result = rb_eval(self, node->nd_head);
	    node = node->nd_next;
	}
	break;
#else
	if (next) {
	    nstack = rb_node_newnode(NODE_CREF,next,0,nstack);
	}
	next = node->nd_next;
	node = node->nd_head;
	goto again;
#endif
      case NODE_POSTEXE:
	rb_f_END();
	nd_set_type(node, NODE_NIL); /* exec just once */
	result = Qnil;
	break;

	/* begin .. end without clauses */
      case NODE_BEGIN:
	node = node->nd_body;
	goto again;

	/* nodes for speed-up(default match) */
      case NODE_MATCH:
	result = rb_reg_match2(node->nd_head->nd_lit);
	break;

	/* nodes for speed-up(literal match) */
      case NODE_MATCH2:
	result = rb_reg_match(rb_eval(self,node->nd_recv),
			   rb_eval(self,node->nd_value));
	break;

	/* nodes for speed-up(literal match) */
      case NODE_MATCH3:
        {
	    VALUE r = rb_eval(self,node->nd_recv);
	    VALUE l = rb_eval(self,node->nd_value);
	    if (TYPE(r) == T_STRING) {
		result = rb_reg_match(l, r);
	    }
	    else {
		result = rb_funcall(r, match, 1, l);
	    }
	}
	break;

	/* nodes for speed-up(top-level loop for -n/-p) */
      case NODE_OPT_N:
	while (!NIL_P(rb_gets())) {
	    rb_eval(self, node->nd_body);
	}
	RETURN(Qnil);

      case NODE_SELF:
	RETURN(self);

      case NODE_NIL:
	RETURN(Qnil);

      case NODE_TRUE:
	RETURN(Qtrue);

      case NODE_FALSE:
	RETURN(Qfalse);

      case NODE_IF:
	ruby_sourceline = nd_line(node);
#ifdef NOBLOCK_RECUR
	if (RTEST(result)){
#else
	if (RTEST(rb_eval(self, node->nd_cond))) {
#endif
	    node = node->nd_body;
	}
	else {
	    node = node->nd_else;
	}
	goto again;

      case NODE_CASE:
	{
	    VALUE val;

#ifdef NOBLOCK_RECUR
	    val = result;
#else
	    val = rb_eval(self, node->nd_head);
#endif
	    node = node->nd_body;
	    while (node) {
		NODE *tag;

		if (nd_type(node) != NODE_WHEN) {
		    goto again;
		}
		tag = node->nd_head;
		while (tag) {
		    if (trace_func) {
			call_trace_func("line", tag->nd_file, nd_line(tag),
					self, ruby_frame->last_func, 0);	
		    }
		    ruby_sourcefile = tag->nd_file;
		    ruby_sourceline = nd_line(tag);
		    if (nd_type(tag->nd_head) == NODE_WHEN) {
			VALUE v = rb_eval(self, tag->nd_head->nd_head);
			int i;

			if (TYPE(v) != T_ARRAY) v = rb_Array(v);
			for (i=0; i<RARRAY(v)->len; i++) {
			    if (RTEST(rb_funcall2(RARRAY(v)->ptr[i], eqq, 1, &val))){
				node = node->nd_body;
				goto again;
			    }
			}
			tag = tag->nd_next;
			continue;
		    }
		    if (RTEST(rb_funcall2(rb_eval(self, tag->nd_head), eqq, 1, &val))) {
			node = node->nd_body;
			goto again;
		    }
		    tag = tag->nd_next;
		}
		node = node->nd_next;
	    }
	}
	RETURN(Qnil);

      case NODE_WHILE:
	PUSH_TAG(PROT_NONE);
	switch (state = EXEC_TAG()) {
	  case 0:
	    ruby_sourceline = nd_line(node);
	    if (node->nd_state && !RTEST(rb_eval(self, node->nd_cond)))
		goto while_out;
	    do {
	      while_redo:
		rb_eval(self, node->nd_body);
	      while_next:
		;
	    } while (RTEST(rb_eval(self, node->nd_cond)));
	    break;

	  case TAG_REDO:
	    state = 0;
	    goto while_redo;
	  case TAG_NEXT:
	    state = 0;
	    goto while_next;
	  case TAG_BREAK:
	    state = 0;
	  default:
	    break;
	}
      while_out:
	POP_TAG();
	if (state) JUMP_TAG(state);
	RETURN(Qnil);

      case NODE_UNTIL:
	PUSH_TAG(PROT_NONE);
	switch (state = EXEC_TAG()) {
	  case 0:
	    if (node->nd_state && RTEST(rb_eval(self, node->nd_cond)))
		goto until_out;
	    do {
	      until_redo:
		rb_eval(self, node->nd_body);
	      until_next:
		;
	    } while (!RTEST(rb_eval(self, node->nd_cond)));
	    break;

	  case TAG_REDO:
	    state = 0;
	    goto until_redo;
	  case TAG_NEXT:
	    state = 0;
	    goto until_next;
	  case TAG_BREAK:
	    state = 0;
	  default:
	    break;
	}
      until_out:
	POP_TAG();
	if (state) JUMP_TAG(state);
	RETURN(Qnil);

      case NODE_BLOCK_PASS:
	result = block_pass(self, node);
	break;

      case NODE_ITER:
      case NODE_FOR:
	{
	  iter_retry:
	    PUSH_BLOCK(node->nd_var, node->nd_body);
	    PUSH_TAG(PROT_FUNC);

	    state = EXEC_TAG();
	    if (state == 0) {
		if (nd_type(node) == NODE_ITER) {
		    PUSH_ITER(ITER_PRE);
		    result = rb_eval(self, node->nd_iter);
		    POP_ITER();
		}
		else {
		    VALUE recv;
		    char *file = ruby_sourcefile;
		    int line = ruby_sourceline;

		    recv = rb_eval(self, node->nd_iter);
		    PUSH_ITER(ITER_PRE);
		    ruby_sourcefile = file;
		    ruby_sourceline = line;
		    result = rb_call(CLASS_OF(recv),recv,each,0,0,0);
		    POP_ITER();
		}
	    }
	    else if (_block.tag->dst == state) {
		state &= TAG_MASK;
		if (state == TAG_RETURN) {
		    result = prot_tag->retval;
		}
	    }
	    POP_TAG();
	    if (rb_verbose && !ruby_block->used) {
		ruby_sourcefile = node->nd_file;
		ruby_sourceline = nd_line(node);
		rb_warn("unused block");
	    }
	    POP_BLOCK();
	    switch (state) {
	      case 0:
		break;

	      case TAG_RETRY:
		goto iter_retry;

	      case TAG_BREAK:
		result = Qnil;
		break;
	      case TAG_RETURN:
		return_value(result);
		/* fall through */
	      default:
		JUMP_TAG(state);
	    }
	}
	break;

      case NODE_BREAK:
	JUMP_TAG(TAG_BREAK);
	break;

      case NODE_NEXT:
	JUMP_TAG(TAG_NEXT);
	break;

      case NODE_REDO:
	JUMP_TAG(TAG_REDO);
	break;

      case NODE_RETRY:
	JUMP_TAG(TAG_RETRY);
	break;

      case NODE_RESTARGS:
	result = rb_eval(self, node->nd_head);
	if (TYPE(result) != T_ARRAY) {
	    result = rb_Array(result);
	}
	break;

      case NODE_YIELD:
	result = rb_eval(self, node->nd_stts);
	if (nd_type(node->nd_stts) == NODE_RESTARGS && RARRAY(result)->len == 1) {
	    result = RARRAY(result)->ptr[0];
	}
	result = rb_yield_0(result, 0, 0);
	break;

      case NODE_RESCUE:
      retry_entry:
        {
	    volatile VALUE e_info = rb_errinfo;

	    PUSH_TAG(PROT_NONE);
	    if ((state = EXEC_TAG()) == 0) {
		result = rb_eval(self, node->nd_head);
	    }
	    POP_TAG();
	    if (state == TAG_RAISE) {
		NODE * volatile resq = node->nd_resq;

		while (resq) {
		    if (handle_rescue(self, resq)) {
			state = 0;
			PUSH_TAG(PROT_NONE);
			if ((state = EXEC_TAG()) == 0) {
			    result = rb_eval(self, resq->nd_body);
			}
			POP_TAG();
			if (state == 0) {
			    rb_errinfo = e_info;
			}
			else if (state == TAG_RETRY) {
			    state = 0;
			    goto retry_entry;
			}
			break;
		    }
		    resq = resq->nd_head; /* next rescue */
		}
	    }
	    if (state) JUMP_TAG(state);
	    if (node->nd_else) { /* no exception raised, else clause given */
		result = rb_eval(self, node->nd_else);
	    }
	}
        break;

      case NODE_ENSURE:
	PUSH_TAG(PROT_NONE);
	if ((state = EXEC_TAG()) == 0) {
	    result = rb_eval(self, node->nd_head);
	}
	POP_TAG();
	if (node->nd_ensr) {
	    VALUE retval = prot_tag->retval; /* save retval */

	    rb_eval(self, node->nd_ensr);
	    return_value(retval);
	}
	if (state) JUMP_TAG(state);
	break;

      case NODE_AND:
	result = rb_eval(self, node->nd_1st);
	if (!RTEST(result)) break;
	node = node->nd_2nd;
	goto again;

      case NODE_OR:
	result = rb_eval(self, node->nd_1st);
	if (RTEST(result)) break;
	node = node->nd_2nd;
	goto again;

      case NODE_NOT:
	if (RTEST(rb_eval(self, node->nd_body))) result = Qfalse;
	else result = Qtrue;
	break;

      case NODE_DOT2:
      case NODE_DOT3:
	result = rb_range_new(rb_eval(self, node->nd_beg), rb_eval(self, node->nd_end));
#if 0
	break;
#else
	result = rb_range_new(rb_eval(self, node->nd_beg), rb_eval(self, node->nd_end));
	if (node->nd_state) break;
	if (nd_type(node->nd_beg) == NODE_LIT && FIXNUM_P(node->nd_beg->nd_lit) &&
	    nd_type(node->nd_end) == NODE_LIT && FIXNUM_P(node->nd_end->nd_lit))
	{
	    nd_set_type(node, NODE_LIT);
	    node->nd_lit = result;
	}
	else {
	    node->nd_state = 1;
	}
#endif
	break;

      case NODE_FLIP2:		/* like AWK */
	if (ruby_scope->local_vars == 0) {
	    rb_bug("unexpected local variable");
	}
	if (!RTEST(ruby_scope->local_vars[node->nd_cnt])) {
	    if (RTEST(rb_eval(self, node->nd_beg))) {
		ruby_scope->local_vars[node->nd_cnt] = 
		    RTEST(rb_eval(self, node->nd_end))?Qfalse:Qtrue;
		result = Qtrue;
	    }
	    else {
		result = Qfalse;
	    }
	}
	else {
	    if (RTEST(rb_eval(self, node->nd_end))) {
		ruby_scope->local_vars[node->nd_cnt] = Qfalse;
	    }
	    result = Qtrue;
	}
	break;

      case NODE_FLIP3:		/* like SED */
	if (ruby_scope->local_vars == 0) {
	    rb_bug("unexpected local variable");
	}
	if (!RTEST(ruby_scope->local_vars[node->nd_cnt])) {
	    result = RTEST(rb_eval(self, node->nd_beg));
	    ruby_scope->local_vars[node->nd_cnt] = result;
	}
	else {
	    if (RTEST(rb_eval(self, node->nd_end))) {
		ruby_scope->local_vars[node->nd_cnt] = Qfalse;
	    }
	    result = Qtrue;
	}
	break;

      case NODE_RETURN:
	if (node->nd_stts) {
	    return_value(rb_eval(self, node->nd_stts));
	}
	return_check();
	JUMP_TAG(TAG_RETURN);
	break;

      case NODE_ARGSCAT:
	result = rb_ary_concat(rb_eval(self, node->nd_head),
			       rb_eval(self, node->nd_body));
	break;

      case NODE_CALL:
	{
	    VALUE recv;
	    int argc; VALUE *argv; /* used in SETUP_ARGS */
	    TMP_PROTECT;

	    BEGIN_CALLARGS;
#ifdef NOBLOCK_RECUR_incomplete
	    printf("mid %s recv: ", rb_id2name(node->nd_mid));
	    rb_p(result);
	    recv = result;
#else
	    recv = rb_eval(self, node->nd_recv);
#endif
	    SETUP_ARGS(node->nd_args);
	    END_CALLARGS;

	    result = rb_call(CLASS_OF(recv),recv,node->nd_mid,argc,argv,0);
	}
	break;

      case NODE_FCALL:
	{
	    int argc; VALUE *argv; /* used in SETUP_ARGS */
	    TMP_PROTECT;

	    BEGIN_CALLARGS;
	    SETUP_ARGS(node->nd_args);
	    END_CALLARGS;

	    result = rb_call(CLASS_OF(self),self,node->nd_mid,argc,argv,1);
	}
	break;

      case NODE_VCALL:
	result = rb_call(CLASS_OF(self),self,node->nd_mid,0,0,2);
	break;

      case NODE_SUPER:
      case NODE_ZSUPER:
	{
	    int argc; VALUE *argv; /* used in SETUP_ARGS */
	    TMP_PROTECT;

	    if (ruby_frame->last_class == 0) {	
		rb_raise(rb_eNameError, "superclass method `%s' disabled",
			 rb_id2name(ruby_frame->last_func));
	    }
	    if (nd_type(node) == NODE_ZSUPER) {
		argc = ruby_frame->argc;
		argv = ruby_frame->argv;
	    }
	    else {
		BEGIN_CALLARGS;
		SETUP_ARGS(node->nd_args);
		END_CALLARGS;
	    }

	    PUSH_ITER(ruby_iter->iter?ITER_PRE:ITER_NOT);
	    result = rb_call(RCLASS(ruby_frame->last_class)->super,
			     ruby_frame->self, ruby_frame->last_func,
			     argc, argv, 3);
	    POP_ITER();
	}
	break;

      case NODE_SCOPE:
	{
	    VALUE save = ruby_frame->cbase;

	    PUSH_SCOPE();
	    PUSH_TAG(PROT_NONE);
	    if (node->nd_rval) ruby_frame->cbase = node->nd_rval;
	    if (node->nd_tbl) {
		VALUE *vars = ALLOCA_N(VALUE, node->nd_tbl[0]+1);
		*vars++ = (VALUE)node;
		ruby_scope->local_vars = vars;
		rb_mem_clear(ruby_scope->local_vars, node->nd_tbl[0]);
		ruby_scope->local_tbl = node->nd_tbl;
	    }
	    else {
		ruby_scope->local_vars = 0;
		ruby_scope->local_tbl  = 0;
	    }
	    if ((state = EXEC_TAG()) == 0) {
		result = rb_eval(self, node->nd_next);
	    }
	    POP_TAG();
	    POP_SCOPE();
	    ruby_frame->cbase = save;
	    if (state) JUMP_TAG(state);
	}
	break;

      case NODE_OP_ASGN1:
	{
	    int argc; VALUE *argv; /* used in SETUP_ARGS */
	    VALUE recv, val;
	    NODE *rval;
	    TMP_PROTECT;

	    recv = rb_eval(self, node->nd_recv);
	    rval = node->nd_args->nd_head;
	    SETUP_ARGS(node->nd_args->nd_next);
	    val = rb_funcall2(recv, aref, argc-1, argv);
	    if (node->nd_mid == 0) {      /* OR */
		if (RTEST(val)) break;
		val = rb_eval(self, rval);
	    }
	    else if (node->nd_mid == 1) { /* AND */
		if (!RTEST(val)) break;
		val = rb_eval(self, rval);
	    }
	    else {
		val = rb_funcall(val, node->nd_mid, 1, rb_eval(self, rval));
	    }
	    argv[argc-1] = val;
	    val = rb_funcall2(recv, aset, argc, argv);
	    result = val;
	}
	break;

      case NODE_OP_ASGN2:
	{
	    ID id = node->nd_next->nd_vid;
	    VALUE recv, val;

	    recv = rb_eval(self, node->nd_recv);
	    val = rb_funcall(recv, id, 0);
	    if (node->nd_next->nd_mid == 0) {      /* OR */
		if (RTEST(val)) break;
		val = rb_eval(self, node->nd_value);
	    }
	    else if (node->nd_next->nd_mid == 1) { /* AND */
		if (!RTEST(val)) break;
		val = rb_eval(self, node->nd_value);
	    }
	    else {
		val = rb_funcall(val, node->nd_next->nd_mid, 1,
				 rb_eval(self, node->nd_value));
	    }

	    rb_funcall2(recv, node->nd_next->nd_aid, 1, &val);
	    result = val;
	}
	break;

      case NODE_OP_ASGN_AND:
	result = rb_eval(self, node->nd_head);
	if (RTEST(result)) {
	    result = rb_eval(self, node->nd_value);
	}
	break;

      case NODE_OP_ASGN_OR:
	result = rb_eval(self, node->nd_head);
	if (!RTEST(result)) {
	    result = rb_eval(self, node->nd_value);
	}
	break;

      case NODE_MASGN:
	result = massign(self, node, rb_eval(self, node->nd_value));
	break;

      case NODE_LASGN:
	if (ruby_scope->local_vars == 0)
	    rb_bug("unexpected local variable assignment");
	result = rb_eval(self, node->nd_value);
	ruby_scope->local_vars[node->nd_cnt] = result;
	break;

      case NODE_DASGN:
	result = rb_eval(self, node->nd_value);
	rb_dvar_asgn(node->nd_vid, result);
	break;

      case NODE_DASGN_PUSH:
	result = rb_eval(self, node->nd_value);
	dvar_asgn_push(node->nd_vid, result);
	break;

      case NODE_GASGN:
	result = rb_eval(self, node->nd_value);
	rb_gvar_set(node->nd_entry, result);
	break;

      case NODE_IASGN:
	result = rb_eval(self, node->nd_value);
	rb_ivar_set(self, node->nd_vid, result);
	break;

      case NODE_CASGN:
	if (NIL_P(ruby_class)) {
	    rb_raise(rb_eTypeError, "no class/module to define constant");
	}
	result = rb_eval(self, node->nd_value);
	/* check for static scope constants */
	if (RTEST(rb_verbose) &&
	    ev_const_defined((NODE*)ruby_frame->cbase, node->nd_vid)) {
	    if (rb_verbose) {
		rb_warning("already initialized constant %s",
			   rb_id2name(node->nd_vid));
	    }
	}
	rb_const_set(ruby_class, node->nd_vid, result);
	break;

      case NODE_LVAR:
	if (ruby_scope->local_vars == 0) {
	    rb_bug("unexpected local variable");
	}
	result = ruby_scope->local_vars[node->nd_cnt];
	break;

      case NODE_DVAR:
	result = rb_dvar_ref(node->nd_vid);
	break;

      case NODE_GVAR:
	result = rb_gvar_get(node->nd_entry);
	break;

      case NODE_IVAR:
	result = rb_ivar_get(self, node->nd_vid);
	break;

      case NODE_CVAR:
	result = ev_const_get((NODE*)ruby_frame->cbase, node->nd_vid);
	break;

      case NODE_BLOCK_ARG:
	if (ruby_scope->local_vars == 0)
	    rb_bug("unexpected block argument");
	if (rb_iterator_p()) {
	    result = rb_f_lambda();
	    ruby_scope->local_vars[node->nd_cnt] = result;
	}
	else {
	    result = Qnil;
	}
	break;

      case NODE_COLON2:
	{
	    VALUE klass;

	    klass = rb_eval(self, node->nd_head);
	    switch (TYPE(klass)) {
	      case T_CLASS:
	      case T_MODULE:
		break;
	      default:
		Check_Type(klass, T_CLASS);
		break;
	    }
	    result = rb_const_get_at(klass, node->nd_mid);
	}
	break;

      case NODE_COLON3:
	result = rb_const_get_at(rb_cObject, node->nd_mid);
	break;

      case NODE_NTH_REF:
	result = rb_reg_nth_match(node->nd_nth, MATCH_DATA);
	break;

      case NODE_BACK_REF:
	switch (node->nd_nth) {
	  case '&':
	    result = rb_reg_last_match(MATCH_DATA);
	    break;
	  case '`':
	    result = rb_reg_match_pre(MATCH_DATA);
	    break;
	  case '\'':
	    result = rb_reg_match_post(MATCH_DATA);
	    break;
	  case '+':
	    result = rb_reg_match_last(MATCH_DATA);
	    break;
	  default:
	    rb_bug("unexpected back-ref");
	}
	break;

      case NODE_HASH:
	{
	    NODE *list;
	    VALUE hash = rb_hash_new();
	    VALUE key, val;

	    list = node->nd_head;
	    while (list) {
		key = rb_eval(self, list->nd_head);
		list = list->nd_next;
		if (list == 0)
		    rb_bug("odd number list for Hash");
		val = rb_eval(self, list->nd_head);
		list = list->nd_next;
		rb_hash_aset(hash, key, val);
	    }
	    result = hash;
	}
	break;

      case NODE_ZARRAY:		/* zero length list */
	result = rb_ary_new();
	break;

      case NODE_ARRAY:
	{
	    VALUE ary;
	    int i;

	    i = node->nd_alen;
	    ary = rb_ary_new2(i);
	    for (i=0;node;node=node->nd_next) {
		RARRAY(ary)->ptr[i++] = rb_eval(self, node->nd_head);
		RARRAY(ary)->len = i;
	    }

	    result = ary;
	}
	break;

      case NODE_STR:
	result = rb_str_new3(node->nd_lit);
	break;

      case NODE_DSTR:
      case NODE_DXSTR:
      case NODE_DREGX:
      case NODE_DREGX_ONCE:
	{
	    VALUE str, str2;
	    NODE *list = node->nd_next;

	    str = rb_str_new3(node->nd_lit);
	    while (list) {
		if (nd_type(list->nd_head) == NODE_STR) {
		    str2 = list->nd_head->nd_lit;
		}
		else {
		    if (nd_type(list->nd_head) == NODE_EVSTR) {
			rb_in_eval++;
			list->nd_head = compile(list->nd_head->nd_lit,0);
			ruby_eval_tree = 0;
			rb_in_eval--;
			if (ruby_nerrs > 0) {
			    compile_error("string expansion");
			}
		    }
		    str2 = rb_eval(self, list->nd_head);
		    str2 = rb_obj_as_string(str2);
		}
		if (str2) {
		    rb_str_cat(str, RSTRING(str2)->ptr, RSTRING(str2)->len);
		    if (rb_obj_tainted(str2)) rb_obj_taint(str);
		}
		list = list->nd_next;
	    }
	    switch (nd_type(node)) {
	      case NODE_DREGX:
		result = rb_reg_new(RSTRING(str)->ptr, RSTRING(str)->len,
				 node->nd_cflag);
		break;
	      case NODE_DREGX_ONCE:	/* regexp expand once */
		result = rb_reg_new(RSTRING(str)->ptr, RSTRING(str)->len,
				 node->nd_cflag);
		nd_set_type(node, NODE_LIT);
		node->nd_lit = result;
		break;
	      case NODE_DXSTR:
		result = rb_funcall(self, '`', 1, str);
		break;
	      default:
		result = str;
		break;
	    }
	}
	break;

      case NODE_XSTR:
	result = rb_funcall(self, '`', 1, node->nd_lit);
	break;

      case NODE_LIT:
	result = node->nd_lit;
	break;

      case NODE_ATTRSET:
	if (ruby_frame->argc != 1)
	    rb_raise(rb_eArgError, "Wrong # of arguments(%d for 1)",
		     ruby_frame->argc);
	result = rb_ivar_set(self, node->nd_vid, ruby_frame->argv[0]);
	break;

      case NODE_DEFN:
	if (node->nd_defn) {
	    NODE *body;
	    VALUE origin;
	    int noex;

	    if (NIL_P(ruby_class)) {
		rb_raise(rb_eTypeError, "no class to add method");
	    }
	    if (ruby_class == rb_cObject && node->nd_mid == init) {
		rb_warn("re-defining Object#initialize may cause infinite loop");
	    }
	    body = search_method(ruby_class, node->nd_mid, &origin);
	    if (body) {
		if (origin == ruby_class) {
		    if (safe_level >= 3) {
			rb_raise(rb_eSecurityError, "re-defining method prohibited");
		    }
		    if (rb_verbose) {
			rb_warning("discarding old %s", rb_id2name(node->nd_mid));
		    }
		}
		rb_clear_cache_by_id(node->nd_mid);
	    }

	    if (SCOPE_TEST(SCOPE_PRIVATE) || node->nd_mid == init) {
		noex = NOEX_PRIVATE;
	    }
	    else if (SCOPE_TEST(SCOPE_PROTECTED)) {
		noex = NOEX_PROTECTED;
	    }
	    else {
		noex = NOEX_PUBLIC;
	    }
	    if (body && origin == ruby_class && body->nd_noex & NOEX_UNDEF) {
		noex |= NOEX_UNDEF;
	    }
	    rb_add_method(ruby_class, node->nd_mid, node->nd_defn, noex);
	    if (scope_vmode == SCOPE_MODFUNC) {
		rb_add_method(rb_singleton_class(ruby_class),
			      node->nd_mid, node->nd_defn, NOEX_PUBLIC);
		rb_funcall(ruby_class, rb_intern("singleton_method_added"),
			   1, INT2FIX(node->nd_mid));
	    }
	    if (FL_TEST(ruby_class, FL_SINGLETON)) {
		rb_funcall(rb_iv_get(ruby_class, "__attached__"),
			   rb_intern("singleton_method_added"),
			   1, INT2FIX(node->nd_mid));
	    }
	    else {
		rb_funcall(ruby_class, rb_intern("method_added"),
			   1, INT2FIX(node->nd_mid));
	    }
	    result = Qnil;
	}
	break;

      case NODE_DEFS:
	if (node->nd_defn) {
	    VALUE recv = rb_eval(self, node->nd_recv);
	    VALUE klass;
	    NODE *body = 0;

	    if (FIXNUM_P(recv)) {
		rb_raise(rb_eTypeError, "Can't define method \"%s\" for Fixnum",
			 rb_id2name(node->nd_mid));
	    }
	    if (NIL_P(recv)) {
		rb_raise(rb_eTypeError, "Can't define method \"%s\" for nil",
			 rb_id2name(node->nd_mid));
	    }
	    if (rb_special_const_p(recv)) {
		rb_raise(rb_eTypeError,
			 "Can't define method \"%s\" for special constants",
			 rb_id2name(node->nd_mid));
	    }

	    if (rb_safe_level() >= 4 && !FL_TEST(recv, FL_TAINT)) {
		rb_raise(rb_eSecurityError, "can't define singleton method");
	    }
	    klass = rb_singleton_class(recv);
	    if (st_lookup(RCLASS(klass)->m_tbl, node->nd_mid, &body)) {
		if (safe_level >= 3) {
		    rb_raise(rb_eSecurityError, "re-defining method prohibited");
		}
		if (rb_verbose) {
		    rb_warning("redefine %s", rb_id2name(node->nd_mid));
		}
	    }
	    rb_clear_cache_by_id(node->nd_mid);
	    rb_add_method(klass, node->nd_mid, node->nd_defn, 
			  NOEX_PUBLIC|(body?body->nd_noex&NOEX_UNDEF:0));
	    rb_funcall(recv, rb_intern("singleton_method_added"),
		       1, INT2FIX(node->nd_mid));
	    result = Qnil;
	}
	break;

      case NODE_UNDEF:
	{
	    VALUE origin;
	    NODE *body;

	    if (NIL_P(ruby_class)) {
		rb_raise(rb_eTypeError, "no class to undef method");
	    }
	    if (ruby_class == rb_cObject) {
		rb_secure(4);
	    }
	    body = search_method(ruby_class, node->nd_mid, &origin);
	    if (!body || !body->nd_body) {
		char *s0 = " class";
		VALUE klass = ruby_class;

		if (FL_TEST(ruby_class, FL_SINGLETON)) {
		    VALUE obj = rb_iv_get(ruby_class, "__attached__");
		    switch (TYPE(obj)) {
		      case T_MODULE:
		      case T_CLASS:
			klass = obj;
			s0 = "";
		    }
		}
		rb_raise(rb_eNameError, "undefined method `%s' for%s `%s'",
			 rb_id2name(node->nd_mid),s0,rb_class2name(klass));
	    }
	    rb_clear_cache_by_id(node->nd_mid);
	    rb_add_method(ruby_class, node->nd_mid, 0, NOEX_PUBLIC);
	    result = Qnil;
	}
	break;

      case NODE_ALIAS:
	if (NIL_P(ruby_class)) {
	    rb_raise(rb_eTypeError, "no class to make alias");
	}
	rb_alias(ruby_class, node->nd_new, node->nd_old);
	rb_funcall(ruby_class, rb_intern("method_added"),
		   1, INT2FIX(node->nd_mid));
	result = Qnil;
	break;

      case NODE_VALIAS:
	rb_alias_variable(node->nd_new, node->nd_old);
	result = Qnil;
	break;

      case NODE_CLASS:
	{
	    VALUE super, klass, tmp;

	    if (NIL_P(ruby_class)) {
		rb_raise(rb_eTypeError, "no outer class/module");
	    }
	    if (node->nd_super) {
		super = superclass(self, node->nd_super);
	    }
	    else {
		super = 0;
	    }

	    if (rb_const_defined_at(ruby_class,node->nd_cname) &&
		(ruby_class != rb_cObject ||
		 !rb_autoload_defined(node->nd_cname))) {

		klass = rb_const_get_at(ruby_class, node->nd_cname);
		if (TYPE(klass) != T_CLASS) {
		    rb_raise(rb_eTypeError, "%s is not a class",
			     rb_id2name(node->nd_cname));
		}
		if (super) {
		    tmp = RCLASS(klass)->super;
		    if (FL_TEST(tmp, FL_SINGLETON)) {
			tmp = RCLASS(tmp)->super;
		    }
		    while (TYPE(tmp) == T_ICLASS) {
			tmp = RCLASS(tmp)->super;
		    }
		    if (tmp != super) {
			rb_raise(rb_eTypeError, "superclass mismatch for %s",
				 rb_id2name(node->nd_cname));
		    }
		}
		if (safe_level >= 3) {
		    rb_raise(rb_eSecurityError, "extending class prohibited");
		}
		rb_clear_cache();
	    }
	    else {
		if (!super) super = rb_cObject;
		klass = rb_define_class_id(node->nd_cname, super);
		rb_const_set(ruby_class, node->nd_cname, klass);
		rb_set_class_path(klass,ruby_class,rb_id2name(node->nd_cname));
		rb_obj_call_init(klass);
	    }

	    result = rb_module_setup(klass, node->nd_body);
	}
	break;

      case NODE_MODULE:
	{
	    VALUE module;

	    if (NIL_P(ruby_class)) {
		rb_raise(rb_eTypeError, "no outer class/module");
	    }
	    if (rb_const_defined_at(ruby_class, node->nd_cname) &&
		(ruby_class != rb_cObject ||
		 !rb_autoload_defined(node->nd_cname))) {

		module = rb_const_get_at(ruby_class, node->nd_cname);
		if (TYPE(module) != T_MODULE) {
		    rb_raise(rb_eTypeError, "%s is not a module",
			     rb_id2name(node->nd_cname));
		}
		if (safe_level >= 3) {
		    rb_raise(rb_eSecurityError, "extending module prohibited");
		}
	    }
	    else {
		module = rb_define_module_id(node->nd_cname);
		rb_const_set(ruby_class, node->nd_cname, module);
		rb_set_class_path(module,ruby_class,rb_id2name(node->nd_cname));
		rb_obj_call_init(module);
	    }

	    result = rb_module_setup(module, node->nd_body);
	}
	break;

      case NODE_SCLASS:
	{
	    VALUE klass;

	    klass = rb_eval(self, node->nd_recv);
	    if (FIXNUM_P(klass)) {
		rb_raise(rb_eTypeError, "No virtual class for Fixnums");
	    }
	    if (NIL_P(klass)) {
		rb_raise(rb_eTypeError, "No virtual class for nil");
	    }
	    if (rb_special_const_p(klass)) {
		rb_raise(rb_eTypeError, "No virtual class for special constants");
	    }
	    if (FL_TEST(CLASS_OF(klass), FL_SINGLETON)) {
		rb_clear_cache();
	    }
	    klass = rb_singleton_class(klass);
	    
	    result = rb_module_setup(klass, node->nd_body);
	}
	break;

      case NODE_DEFINED:
	{
	    char buf[20];
	    char *desc = is_defined(self, node->nd_head, buf);

	    if (desc) result = rb_str_new2(desc);
	    else result = Qfalse;
	}
	break;

    case NODE_NEWLINE:
	ruby_sourcefile = node->nd_file;
	ruby_sourceline = node->nd_nth;
	if (trace_func) {
	    call_trace_func("line", ruby_sourcefile, ruby_sourceline,
			    self, ruby_frame->last_func, 0);	
	}
	node = node->nd_next;
	goto again;

      default:
	rb_bug("unknown node type %d", nd_type(node));
    }
  finish:
    CHECK_INTS;
#ifdef NOBLOCK_RECUR
    if (next) {
	node = next;
	next = 0;
	goto again;
    }
    if (nstack) {
	node = nstack->nd_head;
	nstack = nstack->nd_next;
	goto again;
    }
#endif
    return result;
}

static VALUE
rb_module_setup(module, node)
    VALUE module;
    NODE * volatile node;
{
    int state;
    VALUE save = ruby_frame->cbase;
    VALUE result;		/* OK */
    char *file = ruby_sourcefile;
    int line = ruby_sourceline;
    TMP_PROTECT;

    /* fill c-ref */
    node->nd_clss = module;
    node = node->nd_body;

    PUSH_CLASS();
    ruby_class = module;
    PUSH_SCOPE();

    if (node->nd_rval) ruby_frame->cbase = node->nd_rval;
    if (node->nd_tbl) {
	VALUE *vars = TMP_ALLOC(VALUE, node->nd_tbl[0]+1);
	*vars++ = (VALUE)node;
	ruby_scope->local_vars = vars;
	rb_mem_clear(ruby_scope->local_vars, node->nd_tbl[0]);
	ruby_scope->local_tbl = node->nd_tbl;
    }
    else {
	ruby_scope->local_vars = 0;
	ruby_scope->local_tbl  = 0;
    }

    PUSH_TAG(PROT_NONE);
    if ((state = EXEC_TAG()) == 0) {
	if (trace_func) {
	    call_trace_func("class", file, line,
			    ruby_class, ruby_frame->last_func, 0);
	}
	result = rb_eval(ruby_class, node->nd_next);
    }
    POP_TAG();
    POP_SCOPE();
    POP_CLASS();

    ruby_frame->cbase = save;
    if (trace_func) {
	call_trace_func("end", file, line, 0, ruby_frame->last_func, 0);
    }
    if (state) JUMP_TAG(state);

    return result;
}

int
rb_respond_to(obj, id)
    VALUE obj;
    ID id;
{
    if (rb_method_boundp(CLASS_OF(obj), id, 0)) {
	return Qtrue;
    }
    return Qfalse;
}

static VALUE
rb_obj_respond_to(argc, argv, obj)
    int argc;
    VALUE *argv;
    VALUE obj;
{
    VALUE mid, priv;
    ID id;

    rb_scan_args(argc, argv, "11", &mid, &priv);
    id = rb_to_id(mid);
    if (rb_method_boundp(CLASS_OF(obj), id, !RTEST(priv))) {
	return Qtrue;
    }
    return Qfalse;
}

static VALUE
rb_mod_method_defined(mod, mid)
    VALUE mod, mid;
{
    if (rb_method_boundp(mod, rb_to_id(mid), 1)) {
	return Qtrue;
    }
    return Qfalse;
}

void
rb_exit(status)
    int status;
{
    if (prot_tag) {
	exit_status = status;
	rb_exc_raise(rb_exc_new(rb_eSystemExit, 0, 0));
    }
    exit(status);
}

static VALUE
rb_f_exit(argc, argv, obj)
    int argc;
    VALUE *argv;
    VALUE obj;
{
    VALUE status;

    rb_secure(4);
    if (rb_scan_args(argc, argv, "01", &status) == 1) {
	status = NUM2INT(status);
    }
    else {
	status = 0;
    }
    rb_exit(status);
    /* not reached */
}

static void
rb_abort()
{
    if (rb_errinfo) {
	error_print();
    }
    rb_exit(1);
    /* not reached */
}

static VALUE
rb_f_abort()
{
    rb_secure(4);
    rb_abort();
}

void
rb_iter_break()
{
    JUMP_TAG(TAG_BREAK);
}

static void rb_longjmp _((int, VALUE)) NORETURN;
static VALUE make_backtrace _((void));

static void
rb_longjmp(tag, mesg)
    int tag;
    VALUE mesg;
{
    VALUE at;

    if (NIL_P(mesg)) mesg = rb_errinfo;
    if (NIL_P(mesg)) {
	mesg = rb_exc_new(rb_eRuntimeError, 0, 0);
    }

    at = get_backtrace(mesg);
    if (NIL_P(at) && ruby_sourcefile && !NIL_P(mesg)) {
	at = make_backtrace();
	set_backtrace(mesg, at);
    }
    if (!NIL_P(mesg)) {
	rb_errinfo = mesg;
    }

    if (rb_debug && !NIL_P(rb_errinfo)
	&& !rb_obj_is_kind_of(rb_errinfo, rb_eSystemExit)) {
	fprintf(stderr, "Exception `%s' at %s:%d\n",
		rb_class2name(CLASS_OF(rb_errinfo)),
		ruby_sourcefile, ruby_sourceline);
    }

    rb_trap_restore_mask();
    if (trace_func && tag != TAG_FATAL) {
	call_trace_func("raise", ruby_sourcefile, ruby_sourceline,
			ruby_frame->self, ruby_frame->last_func, 0);
    }
    if (!prot_tag) {
	error_print();
    }
    JUMP_TAG(tag);
}

void
rb_exc_raise(mesg)
    VALUE mesg;
{
    rb_longjmp(TAG_RAISE, mesg);
}

void
rb_exc_fatal(mesg)
    VALUE mesg;
{
    rb_longjmp(TAG_FATAL, mesg);
}

void
rb_interrupt()
{
    rb_raise(rb_eInterrupt, "");
}

static VALUE
rb_f_raise(argc, argv)
    int argc;
    VALUE *argv;
{
    VALUE arg1, arg2, arg3;
    VALUE etype, mesg;
    int n;

    etype = rb_eRuntimeError;
    mesg = Qnil;
    switch (n = rb_scan_args(argc, argv, "03", &arg1, &arg2, &arg3)) {
      case 1:
	mesg = arg1;
	break;
      case 3:
      case 2:
	etype = arg1;
	mesg = arg2;
	break;
    }

    if (!NIL_P(mesg)) {
	if (n >= 2) {
	    mesg = rb_funcall(etype, rb_intern("new"), 1, mesg);
	}
	else if (TYPE(mesg) == T_STRING) {
	    mesg = rb_exc_new3(rb_eRuntimeError, mesg);
	}
	if (!rb_obj_is_kind_of(mesg, rb_eException)) {
	    rb_raise(rb_eTypeError, "exception object expected");
	}
	set_backtrace(mesg, arg3);
    }

    PUSH_FRAME();		/* fake frame */
    *ruby_frame = *_frame.prev->prev;
    rb_longjmp(TAG_RAISE, mesg);
    POP_FRAME();
}

int
rb_iterator_p()
{
    if (ruby_frame->iter) return Qtrue;
    return Qfalse;
}

static VALUE
rb_f_iterator_p()
{
    if (ruby_frame->prev && ruby_frame->prev->iter) return Qtrue;
    return Qfalse;
}

static VALUE
rb_yield_0(val, self, klass)
    VALUE val, self, klass;
{
    NODE *node;
    volatile VALUE result = Qnil;
    struct BLOCK *block;
    struct SCOPE *old_scope;
    struct FRAME frame;
    int state;
    static unsigned serial = 1;

    if (!ruby_frame->iter || !ruby_block) {
	rb_raise(rb_eLocalJumpError, "yield called out of iterator");
    }

    ruby_block->used = 1;
    PUSH_VARS();
    PUSH_CLASS();
    block = ruby_block;
    frame = block->frame;
    frame.prev = ruby_frame;
    ruby_frame = &(frame);
    old_scope = ruby_scope;
    ruby_scope = block->scope;
    ruby_block = block->prev;
    ruby_dyna_vars = block->d_vars;
    ruby_class = klass?klass:block->klass;
    if (!self) self = block->self;
    node = block->body;
    if (block->var) {
	if (nd_type(block->var) == NODE_MASGN)
	    massign(self, block->var, val);
	else
	    assign(self, block->var, val);
    }
    PUSH_ITER(block->iter);
    PUSH_TAG(PROT_NONE);
    if ((state = EXEC_TAG()) == 0) {
      redo:
	if (!node) {
	    result = Qnil;
	}
	else if (nd_type(node) == NODE_CFUNC) {
	    result = (*node->nd_cfnc)(val, node->nd_tval, self);
	}
	else {
	    result = rb_eval(self, node);
	}
    }
    else {
	switch (state) {
	  case TAG_REDO:
	    state = 0;
	    goto redo;
	  case TAG_NEXT:
	    state = 0;
	    result = Qnil;
	    break;
	  case TAG_BREAK:
	  case TAG_RETURN:
	    state |= (serial++ << 8);
	    state |= 0x10;
	    block->tag->dst = state; 
	    break;
	  default:
	    break;
	}
    }
    POP_TAG();
    POP_ITER();
    POP_CLASS();
    POP_VARS();
    ruby_block = block;
    ruby_frame = ruby_frame->prev;
    ruby_scope = old_scope;
    if (state) JUMP_TAG(state);
    return result;
}

VALUE
rb_yield(val)
    VALUE val;
{
    return rb_yield_0(val, 0, 0);
}

static VALUE
rb_f_loop()
{
    for (;;) { rb_yield_0(Qnil, 0, 0); }
}

static VALUE
massign(self, node, val)
    VALUE self;
    NODE *node;
    VALUE val;
{
    NODE *list;
    int i, len;

    list = node->nd_head;

    if (val) {
	if (TYPE(val) != T_ARRAY) {
	    val = rb_Array(val);
	}
	len = RARRAY(val)->len;
	for (i=0; list && i<len; i++) {
	    assign(self, list->nd_head, RARRAY(val)->ptr[i]);
	    list = list->nd_next;
	}
	if (node->nd_args) {
	    if (!list && i<len) {
		assign(self, node->nd_args, rb_ary_new4(len-i, RARRAY(val)->ptr+i));
	    }
	    else {
		assign(self, node->nd_args, rb_ary_new2(0));
	    }
	}
    }
    else if (node->nd_args) {
	assign(self, node->nd_args, Qnil);
    }
    while (list) {
	assign(self, list->nd_head, Qnil);
	list = list->nd_next;
    }
    return val;
}

static void
assign(self, lhs, val)
    VALUE self;
    NODE *lhs;
    VALUE val;
{
    switch (nd_type(lhs)) {
      case NODE_GASGN:
	rb_gvar_set(lhs->nd_entry, val);
	break;

      case NODE_IASGN:
	rb_ivar_set(self, lhs->nd_vid, val);
	break;

      case NODE_LASGN:
	if (ruby_scope->local_vars == 0)
	    rb_bug("unexpected iterator variable assignment");
	ruby_scope->local_vars[lhs->nd_cnt] = val;
	break;

      case NODE_DASGN:
	rb_dvar_asgn(lhs->nd_vid, val);
	break;

      case NODE_DASGN_PUSH:
	dvar_asgn_push(lhs->nd_vid, val);
	break;

      case NODE_CASGN:
	rb_const_set(ruby_class, lhs->nd_vid, val);
	break;

      case NODE_MASGN:
	massign(self, lhs, val);
	break;

      case NODE_CALL:
	{
	    VALUE recv;
	    recv = rb_eval(self, lhs->nd_recv);
	    if (!lhs->nd_args->nd_head) {
		/* attr set */
		rb_call(CLASS_OF(recv), recv, lhs->nd_mid, 1, &val, 0);
	    }
	    else {
		/* array set */
		VALUE args;

		args = rb_eval(self, lhs->nd_args);
		RARRAY(args)->ptr[RARRAY(args)->len-1] = val;
		rb_call(CLASS_OF(recv), recv, lhs->nd_mid,
			RARRAY(args)->len, RARRAY(args)->ptr, 0);
	    }
	}
	break;

      default:
	rb_bug("bug in variable assignment");
	break;
    }
}

VALUE
rb_iterate(it_proc, data1, bl_proc, data2)
    VALUE (*it_proc)(), (*bl_proc)();
    VALUE data1, data2;
{
    int state;
    volatile VALUE retval = Qnil;
    NODE *node = NEW_CFUNC(bl_proc, data2);
    VALUE self = rb_top_self;

  iter_retry:
    PUSH_ITER(ITER_PRE);
    PUSH_BLOCK(0, node);
    PUSH_TAG(PROT_NONE);

    state = EXEC_TAG();
    if (state == 0) {
	retval = (*it_proc)(data1);
    }
    if (ruby_block->tag->dst == state) {
	state &= TAG_MASK;
	if (state == TAG_RETURN) {
	    retval = prot_tag->retval;
	}
    }
    POP_TAG();
    POP_BLOCK();
    POP_ITER();

    switch (state) {
      case 0:
	break;

      case TAG_RETRY:
	goto iter_retry;

      case TAG_BREAK:
	retval = Qnil;
	break;

      case TAG_RETURN:
	return_value(retval);
	/* fall through */
      default:
	JUMP_TAG(state);
    }
    return retval;
}

static int
handle_rescue(self, node)
    VALUE self;
    NODE *node;
{
    int argc; VALUE *argv; /* used in SETUP_ARGS */
    TMP_PROTECT;

    if (!node->nd_args) {
	return rb_obj_is_kind_of(rb_errinfo, rb_eStandardError);
    }

    BEGIN_CALLARGS;
    SETUP_ARGS(node->nd_args);
    END_CALLARGS;

    while (argc--) {
	if (!rb_obj_is_kind_of(argv[0], rb_cModule)) {
	    rb_raise(rb_eTypeError, "class or module required for rescue clause");
	}
	if (rb_obj_is_kind_of(rb_errinfo, argv[0])) return 1;
	argv++;
    }
    return 0;
}

VALUE
rb_rescue(b_proc, data1, r_proc, data2)
    VALUE (*b_proc)(), (*r_proc)();
    VALUE data1, data2;
{
    int state;
    volatile VALUE result;
    volatile VALUE e_info = rb_errinfo;

    PUSH_TAG(PROT_NONE);
    if ((state = EXEC_TAG()) == 0) {
      retry_entry:
	result = (*b_proc)(data1);
    }
    else if (state == TAG_RAISE && rb_obj_is_kind_of(rb_errinfo, rb_eStandardError)) {
	if (r_proc) {
	    PUSH_TAG(PROT_NONE);
	    if ((state = EXEC_TAG()) == 0) {
		result = (*r_proc)(data2, rb_errinfo);
	    }
	    POP_TAG();
	    if (state == TAG_RETRY) {
		state = 0;
		goto retry_entry;
	    }
	}
	else {
	    result = Qnil;
	    state = 0;
	}
	if (state == 0) {
	    rb_errinfo = e_info;
	}
    }
    POP_TAG();
    if (state) JUMP_TAG(state);

    return result;
}

VALUE
rb_ensure(b_proc, data1, e_proc, data2)
    VALUE (*b_proc)();
    VALUE (*e_proc)();
    VALUE data1, data2;
{
    int state;
    volatile VALUE result = Qnil;
    VALUE retval;

    PUSH_TAG(PROT_NONE);
    if ((state = EXEC_TAG()) == 0) {
	result = (*b_proc)(data1);
    }
    POP_TAG();
    retval = prot_tag->retval;	/* save retval */
    (*e_proc)(data2);
    return_value(retval);

    if (state) JUMP_TAG(state);
    return result;
}

static int last_call_status;

#define CSTAT_PRIV  1
#define CSTAT_PROT  2
#define CSTAT_VCALL 4

static VALUE
rb_f_missing(argc, argv, obj)
    int argc;
    VALUE *argv;
    VALUE obj;
{
    ID    id;
    VALUE desc = 0;
    char *format = 0;
    char *file = ruby_sourcefile;
    int   line = ruby_sourceline;

    id = FIX2INT(argv[0]);
    argc--; argv++;

    switch (TYPE(obj)) {
      case T_NIL:
	format = "undefined method `%s' for nil";
	break;
      case T_TRUE:
	format = "undefined method `%s' for Qtrue";
	break;
      case T_FALSE:
	format = "undefined method `%s' for Qfalse";
	break;
      case T_OBJECT:
	desc = rb_any_to_s(obj);
	break;
      default:
	desc = rb_inspect(obj);
	break;
    }
    if (desc) {
	if (last_call_status & CSTAT_PRIV) {
	    format = "private method `%s' called for %s";
	}
	if (last_call_status & CSTAT_PROT) {
	    format = "protected method `%s' called for %s";
	}
	else if (rb_iterator_p()) {
	    format = "undefined iterator `%s' for %s";
	}
	else if (last_call_status & CSTAT_VCALL) {
	    char *mname = rb_id2name(id);

	    if (('a' <= mname[0] && mname[0] <= 'z') || mname[0] == '_') {
		format = "undefined local variable or method `%s' for %s";
	    }
	}
	if (!format) {
	    format = "undefined method `%s' for %s";
	}
	if (RSTRING(desc)->len > 65) {
	    desc = rb_any_to_s(obj);
	}
    }

    ruby_sourcefile = file;
    ruby_sourceline = line;
    PUSH_FRAME();		/* fake frame */
    *ruby_frame = *_frame.prev->prev;

    rb_raise(rb_eNameError, format,
	     rb_id2name(id),
	     desc?(char*)RSTRING(desc)->ptr:"");
    POP_FRAME();

    return Qnil;		/* not reached */
}

static VALUE
rb_undefined(obj, id, argc, argv, call_status)
    VALUE obj;
    ID    id;
    int   argc;
    VALUE*argv;
    int   call_status;
{
    VALUE *nargv;

    nargv = ALLOCA_N(VALUE, argc+1);
    nargv[0] = INT2FIX(id);
    MEMCPY(nargv+1, argv, VALUE, argc);

    last_call_status = call_status;

    return rb_funcall2(obj, rb_intern("method_missing"), argc+1, nargv);
}

#ifdef DJGPP
# define STACK_LEVEL_MAX 65535
#else
#ifdef __human68k__
extern int _stacksize;
# define STACK_LEVEL_MAX (_stacksize - 4096)
#else
# define STACK_LEVEL_MAX 655300
#endif
#endif
extern VALUE *rb_gc_stack_start;
static int
stack_length()
{
    VALUE pos;

#ifdef sparc
    return rb_gc_stack_start - &pos + 0x80;
#else
    return (&pos < rb_gc_stack_start) ? rb_gc_stack_start - &pos
	                              : &pos - rb_gc_stack_start;
#endif
}

static VALUE
call_cfunc(func, recv, len, argc, argv)
    VALUE (*func)();
    VALUE recv;
    int len, argc;
    VALUE *argv;
{
    if (len >= 0 && argc != len) {
	rb_raise(rb_eArgError, "Wrong # of arguments(%d for %d)",
		 argc, len);
    }

    switch (len) {
      case -2:
	return (*func)(recv, rb_ary_new4(argc, argv));
	break;
      case -1:
	return (*func)(argc, argv, recv);
	break;
      case 0:
	return (*func)(recv);
	break;
      case 1:
	return (*func)(recv, argv[0]);
	break;
      case 2:
	return (*func)(recv, argv[0], argv[1]);
	break;
      case 3:
	return (*func)(recv, argv[0], argv[1], argv[2]);
	break;
      case 4:
	return (*func)(recv, argv[0], argv[1], argv[2], argv[3]);
	break;
      case 5:
	return (*func)(recv, argv[0], argv[1], argv[2], argv[3], argv[4]);
	break;
      case 6:
	return (*func)(recv, argv[0], argv[1], argv[2], argv[3], argv[4],
		       argv[5]);
	break;
      case 7:
	return (*func)(recv, argv[0], argv[1], argv[2], argv[3], argv[4],
		       argv[5], argv[6]);
	break;
      case 8:
	return (*func)(recv, argv[0], argv[1], argv[2], argv[3], argv[4],
		       argv[5], argv[6], argv[7]);
	break;
      case 9:
	return (*func)(recv, argv[0], argv[1], argv[2], argv[3], argv[4],
		       argv[5], argv[6], argv[7], argv[8]);
	break;
      case 10:
	return (*func)(recv, argv[0], argv[1], argv[2], argv[3], argv[4],
		       argv[5], argv[6], argv[7], argv[8], argv[9]);
	break;
      case 11:
	return (*func)(recv, argv[0], argv[1], argv[2], argv[3], argv[4],
		       argv[5], argv[6], argv[7], argv[8], argv[9], argv[10]);
	break;
      case 12:
	return (*func)(recv, argv[0], argv[1], argv[2], argv[3], argv[4],
		       argv[5], argv[6], argv[7], argv[8], argv[9],
		       argv[10], argv[11]);
	break;
      case 13:
	return (*func)(recv, argv[0], argv[1], argv[2], argv[3], argv[4],
		       argv[5], argv[6], argv[7], argv[8], argv[9], argv[10],
		       argv[11], argv[12]);
	break;
      case 14:
	return (*func)(recv, argv[0], argv[1], argv[2], argv[3], argv[4],
		       argv[5], argv[6], argv[7], argv[8], argv[9], argv[10],
		       argv[11], argv[12], argv[13]);
	break;
      case 15:
	return (*func)(recv, argv[0], argv[1], argv[2], argv[3], argv[4],
		       argv[5], argv[6], argv[7], argv[8], argv[9], argv[10],
		       argv[11], argv[12], argv[13], argv[14]);
	break;
      default:
	rb_raise(rb_eArgError, "too many arguments(%d)", len);
	break;
    }
}

static VALUE
rb_call0(klass, recv, id, argc, argv, body, nosuper)
    VALUE klass, recv;
    ID    id;
    int argc;			/* OK */
    VALUE *argv;		/* OK */
    NODE *body;
    int nosuper;
{
    NODE *b2;		/* OK */
    volatile VALUE result = Qnil;
    int itr;
    static int tick;
    TMP_PROTECT;

    switch (ruby_iter->iter) {
      case ITER_PRE:
	itr = ITER_CUR;
	break;
      case ITER_CUR:
      default:
	itr = ITER_NOT;
	break;
    }

    if ((++tick & 0x3ff) == 0 && stack_length() > STACK_LEVEL_MAX)
	rb_raise(rb_eSysStackError, "stack level too deep");

    PUSH_ITER(itr);
    PUSH_FRAME();

    ruby_frame->last_func = id;
    ruby_frame->last_class = nosuper?0:klass;
    ruby_frame->self = recv;
    ruby_frame->argc = argc;
    ruby_frame->argv = argv;

    switch (nd_type(body)) {
      case NODE_CFUNC:
	{
	    int len = body->nd_argc;

	    if (len < -2) {
		rb_bug("bad argc(%d) specified for `%s(%s)'",
		       len, rb_class2name(klass), rb_id2name(id));
	    }
	    if (trace_func) {
		int state;
		char *file = ruby_frame->prev->file;
		int line = ruby_frame->prev->line;
		if (!file) {
		    file = ruby_sourcefile;
		    line = ruby_sourceline;
		}

		call_trace_func("c-call", 0, 0, 0, id, 0);
		PUSH_TAG(PROT_FUNC);
		if ((state = EXEC_TAG()) == 0) {
		    result = call_cfunc(body->nd_cfnc, recv, len, argc, argv);
		}
		POP_TAG();
		call_trace_func("c-return", 0, 0, recv, id, klass);
		if (state) JUMP_TAG(state);
	    }
	    else {
		result = call_cfunc(body->nd_cfnc, recv, len, argc, argv);
	    }
	}
	break;

	/* for re-scoped/renamed method */
      case NODE_ZSUPER:
	/* for attr get/set */
      case NODE_ATTRSET:
      case NODE_IVAR:
	result = rb_eval(recv, body);
	break;

      default:
	{
	    int state;
	    VALUE *local_vars;	/* OK */

	    PUSH_SCOPE();

	    if (body->nd_rval) ruby_frame->cbase = body->nd_rval;
	    if (body->nd_tbl) {
		local_vars = TMP_ALLOC(VALUE, body->nd_tbl[0]+1);
		*local_vars++ = (VALUE)body;
		rb_mem_clear(local_vars, body->nd_tbl[0]);
		ruby_scope->local_tbl = body->nd_tbl;
		ruby_scope->local_vars = local_vars;
	    }
	    else {
		local_vars = ruby_scope->local_vars = 0;
		ruby_scope->local_tbl  = 0;
	    }
	    b2 = body = body->nd_next;

	    PUSH_VARS();
	    PUSH_TAG(PROT_FUNC);

	    if ((state = EXEC_TAG()) == 0) {
		NODE *node = 0;
		int i;

		if (nd_type(body) == NODE_ARGS) {
		    node = body;
		    body = 0;
		}
		else if (nd_type(body) == NODE_BLOCK) {
		    node = body->nd_head;
		    body = body->nd_next;
		}
		if (node) {
		    if (nd_type(node) != NODE_ARGS) {
			rb_bug("no argument-node");
		    }

		    i = node->nd_cnt;
		    if (i > argc) {
			rb_raise(rb_eArgError, "Wrong # of arguments(%d for %d)",
				 argc, i);
		    }
		    if (node->nd_rest == -1) {
			int opt = argc - i;
			NODE *optnode = node->nd_opt;

			while (optnode) {
			    opt--;
			    optnode = optnode->nd_next;
			}
			if (opt > 0) {
			    rb_raise(rb_eArgError, "Wrong # of arguments(%d for %d)",
				     argc, argc-opt);
			}
		    }

		    if (local_vars) {
			if (i > 0) {
			    /* +2 for $_ and $~ */
			    MEMCPY(local_vars+2, argv, VALUE, i);
			}
			argv += i; argc -= i;
			if (node->nd_opt) {
			    NODE *opt = node->nd_opt;

			    while (opt && argc) {
				assign(recv, opt->nd_head, *argv);
				argv++; argc--;
				opt = opt->nd_next;
			    }
			    rb_eval(recv, opt);
			}
			if (node->nd_rest >= 0) {
			    if (argc > 0)
				local_vars[node->nd_rest]=rb_ary_new4(argc,argv);
			    else
				local_vars[node->nd_rest]=rb_ary_new2(0);
			}
		    }
		}

		if (trace_func) {
		    call_trace_func("call", b2->nd_file, nd_line(b2),
				    recv, ruby_frame->last_func, 0);
		}
		result = rb_eval(recv, body);
	    }
	    else if (state == TAG_RETURN) {
		result = prot_tag->retval;
		state = 0;
	    }
	    POP_TAG();
	    POP_VARS();
	    POP_SCOPE();
	    if (trace_func) {
		char *file = ruby_frame->prev->file;
		int line = ruby_frame->prev->line;
		if (!file) {
		    file = ruby_sourcefile;
		    line = ruby_sourceline;
		}
		call_trace_func("return", file, line, recv,
				ruby_frame->last_func, klass);
	    }
	    switch (state) {
	      case 0:
		break;

	      case TAG_NEXT:
		rb_raise(rb_eLocalJumpError, "unexpected next");
		break;
	      case TAG_BREAK:
		rb_raise(rb_eLocalJumpError, "unexpected break");
		break;
	      case TAG_REDO:
		rb_raise(rb_eLocalJumpError, "unexpected redo");
		break;
	      case TAG_RETRY:
		if (!rb_iterator_p()) {
		    rb_raise(rb_eLocalJumpError, "retry outside of rescue clause");
		}
	      default:
		JUMP_TAG(state);
	    }
	}
    }
    POP_FRAME();
    POP_ITER();
    return result;
}

static VALUE
rb_call(klass, recv, mid, argc, argv, scope)
    VALUE klass, recv;
    ID    mid;
    int argc;			/* OK */
    VALUE *argv;		/* OK */
    int scope;
{
    NODE  *body;		/* OK */
    int    noex;
    ID     id = mid;
    struct cache_entry *ent;

    /* is it in the method cache? */
    ent = cache + EXPR1(klass, mid);
    if (ent->mid == mid && ent->klass == klass) {
	klass = ent->origin;
	id    = ent->mid0;
	noex  = ent->noex;
	body  = ent->method;
    }
    else {
    if (ent->mid) {
	fprintf(stderr, "%s#%s -> %s#%s (%d)\n", rb_class2name(ent->klass),
		                                 rb_id2name(ent->mid),
		                                 rb_class2name(klass),
		                                 rb_id2name(id), ent-cache);
	cache_conflict++;
    }
    if ((body = rb_get_method_body(&klass, &id, &noex)) == 0) {
	if (scope == 3) {
	    rb_raise(rb_eNameError, "super: no superclass method `%s'",
		     rb_id2name(mid));
	}
	return rb_undefined(recv, mid, argc, argv, scope==2?CSTAT_VCALL:0);
    }
    }

    /* receiver specified form for private method */
    if ((noex & NOEX_PRIVATE) && scope == 0)
	return rb_undefined(recv, mid, argc, argv, CSTAT_PRIV);

    /* self must be kind of a specified form for private method */
    if ((noex & NOEX_PROTECTED) && !rb_obj_is_kind_of(ruby_frame->self, klass))
	return rb_undefined(recv, mid, argc, argv, CSTAT_PROT);

    return rb_call0(klass, recv, id, argc, argv, body, noex & NOEX_UNDEF);
}

VALUE
rb_apply(recv, mid, args)
    VALUE recv;
    ID mid;
    VALUE args;
{
    int argc;
    VALUE *argv;

    argc = RARRAY(args)->len;
    argv = ALLOCA_N(VALUE, argc);
    MEMCPY(argv, RARRAY(args)->ptr, VALUE, argc);
    return rb_call(CLASS_OF(recv), recv, mid, argc, argv, 1);
}

static VALUE
rb_f_send(argc, argv, recv)
    int argc;
    VALUE *argv;
    VALUE recv;
{
    VALUE vid;

    if (argc == 0) rb_raise(rb_eArgError, "no method name given");

    vid = *argv++; argc--;
    PUSH_ITER(rb_iterator_p()?ITER_PRE:ITER_NOT);
    vid = rb_call(CLASS_OF(recv), recv, rb_to_id(vid), argc, argv, 1);
    POP_ITER();

    return vid;
}


#ifdef HAVE_STDARG_PROTOTYPES
#include <stdarg.h>
#define va_init_list(a,b) va_start(a,b)
#else
#include <varargs.h>
#define va_init_list(a,b) va_start(a)
#endif

VALUE
#ifdef HAVE_STDARG_PROTOTYPES
rb_funcall(VALUE recv, ID mid, int n, ...)
#else
rb_funcall(recv, mid, n, va_alist)
    VALUE recv;
    ID mid;
    int n;
    va_dcl
#endif
{
    va_list ar;
    VALUE *argv;

    if (n > 0) {
	int i;

	argv = ALLOCA_N(VALUE, n);

	va_init_list(ar, n);
	for (i=0;i<n;i++) {
	    argv[i] = va_arg(ar, VALUE);
	}
	va_end(ar);
    }
    else {
	argv = 0;
    }

    return rb_call(CLASS_OF(recv), recv, mid, n, argv, 1);
}

VALUE
rb_funcall2(recv, mid, argc, argv)
    VALUE recv;
    ID mid;
    int argc;
    VALUE *argv;
{
    return rb_call(CLASS_OF(recv), recv, mid, argc, argv, 1);
}

static VALUE
backtrace(lev)
    int lev;
{
    struct FRAME *frame = ruby_frame;
    char buf[BUFSIZ];
    VALUE ary;
    int slev = safe_level;

    safe_level = 0;
    ary = rb_ary_new();
    if (lev < 0) {
	if (frame->last_func) {
	    snprintf(buf, BUFSIZ, "%s:%d:in `%s'",
		     ruby_sourcefile, ruby_sourceline,
		     rb_id2name(frame->last_func));
	}
	else {
	    snprintf(buf, BUFSIZ, "%s:%d", ruby_sourcefile, ruby_sourceline);
	}
	rb_ary_push(ary, rb_str_new2(buf));
    }
    else {
	while (lev-- > 0) {
	    frame = frame->prev;
	    if (!frame) return Qnil;
	}
    }
    while (frame && frame->file) {
	if (frame->prev && frame->prev->last_func) {
	    snprintf(buf, BUFSIZ, "%s:%d:in `%s'",
		     frame->file, frame->line,
		     rb_id2name(frame->prev->last_func));
	}
	else {
	    snprintf(buf, BUFSIZ, "%s:%d", frame->file, frame->line);
	}
	rb_ary_push(ary, rb_str_new2(buf));
	frame = frame->prev;
    }
    safe_level = slev;
    return ary;
}

static VALUE
rb_f_caller(argc, argv)
    int argc;
    VALUE *argv;
{
    VALUE level;
    int lev;

    rb_scan_args(argc, argv, "01", &level);

    if (NIL_P(level)) lev = 1;
    else lev = NUM2INT(level);
    if (lev < 0) rb_raise(rb_eArgError, "negative level(%d)", lev);

    return backtrace(lev);
}

void
rb_backtrace()
{
    int i, lev;
    VALUE ary;

    lev = INT2FIX(0);
    ary = backtrace(-1);
    for (i=0; i<RARRAY(ary)->len; i++) {
	printf("\tfrom %s\n", RSTRING(RARRAY(ary)->ptr[i])->ptr);
    }
}

static VALUE
make_backtrace()
{
    VALUE lev;

    lev = INT2FIX(0);
    return backtrace(-1);
}

ID
rb_frame_last_func()
{
    return ruby_frame->last_func;
}

static NODE*
compile(src, place)
    VALUE src;
    char *place;
{
    NODE *node;

    Check_Type(src, T_STRING);
    if (place == 0) place = ruby_sourcefile;
    node = rb_compile_string(place, src);

    if (ruby_nerrs == 0) return node;
    return 0;
}

static VALUE
eval(self, src, scope, file, line)
    VALUE self, src, scope;
    char *file;
    int line;
{
    struct BLOCK *data;
    volatile VALUE result = Qnil;
    struct SCOPE * volatile old_scope;
    struct BLOCK * volatile old_block;
    struct BLOCK * volatile old_call_block;
    struct RVarmap * volatile old_d_vars;
    int volatile old_vmode;
    struct FRAME frame;
    char *filesave = ruby_sourcefile;
    int linesave = ruby_sourceline;
    volatile int iter = ruby_frame->iter;
    int state;

    if (file == 0) {
	file = ruby_sourcefile;
	line = ruby_sourceline;
    }
    if (!NIL_P(scope)) {
	if (!rb_obj_is_block(scope)) {
	    rb_raise(rb_eTypeError, "wrong argument type %s (expected Proc/Binding)",
		     rb_class2name(CLASS_OF(scope)));
	}

	Data_Get_Struct(scope, struct BLOCK, data);

	/* PUSH BLOCK from data */
	frame = data->frame;
	frame.prev = ruby_frame;
	ruby_frame = &(frame);
	old_scope = ruby_scope;
	ruby_scope = data->scope;
	old_call_block = ruby_calling_block;
	ruby_calling_block = data;
	old_block = ruby_block;
	ruby_block = data->prev;
	old_d_vars = ruby_dyna_vars;
	ruby_dyna_vars = data->d_vars;
	old_vmode = scope_vmode;
	scope_vmode = data->vmode;

	self = data->self;
	ruby_frame->iter = data->iter;
    }
    else {
	if (ruby_frame->prev) {
	    ruby_frame->iter = ruby_frame->prev->iter;
	}
    }
    PUSH_CLASS();
    ruby_class = ((NODE*)ruby_frame->cbase)->nd_clss;

    rb_in_eval++;
    if (TYPE(ruby_class) == T_ICLASS) {
	ruby_class = RBASIC(ruby_class)->klass;
    }
    PUSH_TAG(PROT_NONE);
    if ((state = EXEC_TAG()) == 0) {
	ruby_sourcefile = file;
	ruby_sourceline = line;
	compile(src, file);
	if (ruby_nerrs > 0) {
	    compile_error(0);
	}
	result = eval_node(self);
    }
    POP_TAG();
    POP_CLASS();
    rb_in_eval--;
    if (!NIL_P(scope)) {
	ruby_frame = ruby_frame->prev;
	ruby_scope = old_scope;
	ruby_block = old_block;
	ruby_calling_block = old_call_block;
	data->d_vars = ruby_dyna_vars;
	ruby_dyna_vars = old_d_vars;
	data->vmode = scope_vmode; /* write back visibility mode */
	scope_vmode = old_vmode;
    }
    else {
	ruby_frame->iter = iter;
    }
    ruby_sourcefile = filesave;
    ruby_sourceline = linesave;
    if (state) {
	if (state == TAG_RAISE) {
	    VALUE err;
	    VALUE errat;

	    errat = get_backtrace(rb_errinfo);
	    if (strcmp(file, "(eval)") == 0) {
		if (ruby_sourceline > 1) {
		    err = RARRAY(errat)->ptr[0];
		    rb_str_cat(err, ": ", 2);
		    rb_str_concat(err, rb_errinfo);
		}
		else {
		    err = rb_str_dup(rb_errinfo);
		}
		errat = Qnil;
		rb_exc_raise(rb_exc_new3(CLASS_OF(rb_errinfo), err));
	    }
	    rb_exc_raise(rb_errinfo);
	}
	JUMP_TAG(state);
    }

    return result;
}

static VALUE
rb_f_eval(argc, argv, self)
    int argc;
    VALUE *argv;
    VALUE self;
{
    VALUE src, scope, vfile, vline;
    char *file = "(eval)";
    int line = 0;

    rb_scan_args(argc, argv, "13", &src, &scope, &vfile, &vline);
    if (argc >= 3) {
	Check_Type(vfile, T_STRING);
	file = RSTRING(vfile)->ptr;
    }
    if (argc >= 4) {
	line = NUM2INT(vline);
    }

    Check_SafeStr(src);
    return eval(self, src, scope, file, line);
}

static VALUE
exec_under(func, under, args)
    VALUE (*func)();
    VALUE under;
    void *args;
{
    VALUE val;			/* OK */
    int state;
    int mode;
    VALUE cbase = ruby_frame->cbase;

    PUSH_CLASS();
    ruby_class = under;
    PUSH_FRAME();
    ruby_frame->last_func = _frame.prev->last_func;
    ruby_frame->last_class = _frame.prev->last_class;
    ruby_frame->argc = _frame.prev->argc;
    ruby_frame->argv = _frame.prev->argv;
    ruby_frame->cbase = (VALUE)rb_node_newnode(NODE_CREF,under,0,cbase);
    mode = scope_vmode;
    SCOPE_SET(SCOPE_PUBLIC);
    PUSH_TAG(PROT_NONE);
    if ((state = EXEC_TAG()) == 0) {
	val = (*func)(args);
    }
    POP_TAG();
    SCOPE_SET(mode);
    POP_FRAME();
    POP_CLASS();
    if (state) JUMP_TAG(state);

    return val;
}

static VALUE
eval_under_i(args)
    VALUE *args;
{
    return eval(args[0], args[1], Qnil, (char*)args[2], (int)args[3]);
}

static VALUE
eval_under(under, self, src, file, line)
    VALUE under, self, src;
    char *file;
    int line;
{
    VALUE args[4];

    Check_SafeStr(src);
    args[0] = self;
    args[1] = src;
    args[2] = (VALUE)file;
    args[3] = (VALUE)line;
    return exec_under(eval_under_i, under, args);
}

static VALUE
yield_under_i(self)
    VALUE self;
{
    return rb_yield_0(self, self, ruby_class);
}

static VALUE
yield_under(under, self)
    VALUE under, self;
{
    rb_secure(4);
    return exec_under(yield_under_i, under, self);
}

static VALUE
rb_obj_instance_eval(argc, argv, self)
    int argc;
    VALUE *argv;
    VALUE self;
{
    char *file = 0;
    int   line = 0;
    VALUE klass;

    if (argc == 0) {
	if (!rb_iterator_p()) {
	    rb_raise(rb_eArgError, "block not supplied");
	}
    }
    else if (argc < 4) {
	Check_SafeStr(argv[0]);
	if (argc > 1) file = STR2CSTR(argv[1]);
	if (argc > 2) line = NUM2INT(argv[2]);
    }
    else {
	rb_raise(rb_eArgError, "Wrong # of arguments: %s(src) or %s{..}",
		 rb_id2name(ruby_frame->last_func),
		 rb_id2name(ruby_frame->last_func));
    }

    if (rb_special_const_p(self)) {
	klass = Qnil;
    }
    else {
	klass = rb_singleton_class(self);
    }
    if (argc == 0) {
	return yield_under(klass, self);
    }
    else {
	return eval_under(klass, self, argv[0], file, line);
    }
}

static VALUE
rb_mod_module_eval(argc, argv, mod)
    int argc;
    VALUE *argv;
    VALUE mod;
{
    char *file = 0;
    int   line = 0;

    if (argc == 0) {
	if (!rb_iterator_p()) {
	    rb_raise(rb_eArgError, "block not supplied");
	}
    }
    else if (argc < 4) {
	Check_SafeStr(argv[0]);
	if (argc > 1) file = STR2CSTR(argv[1]);
	if (argc > 2) line = NUM2INT(argv[2]);
    }
    else {
	rb_raise(rb_eArgError, "Wrong # of arguments: %s(src) or %s{..}",
		 rb_id2name(ruby_frame->last_func),
		 rb_id2name(ruby_frame->last_func));
    }

    if (argc == 0) {
	return yield_under(mod, mod);
    }
    else {
	return eval_under(mod, mod, argv[0], file, line);
    }
}

VALUE rb_load_path;

static int
is_absolute_path(path)
    char *path;
{
    if (path[0] == '/') return 1;
# if defined(MSDOS) || defined(NT) || defined(__human68k__)
    if (path[0] == '\\') return 1;
    if (strlen(path) > 2 && path[1] == ':') return 1;
# endif
    return 0;
}

static char*
find_file(file)
    char *file;
{
    extern VALUE rb_load_path;
    VALUE vpath;
    char *path;

    if (is_absolute_path(file)) {
	FILE *f = fopen(file, "r");

	if (f == NULL) return 0;
	fclose(f);
	return file;
    }

    if (rb_load_path) {
	int i;

	Check_Type(rb_load_path, T_ARRAY);
	for (i=0;i<RARRAY(rb_load_path)->len;i++) {
	    Check_SafeStr(RARRAY(rb_load_path)->ptr[i]);
	}
	vpath = rb_ary_join(rb_load_path, rb_str_new2(RUBY_LIB_SEP));
	Check_SafeStr(vpath);
	path = RSTRING(vpath)->ptr;
	if (safe_level >= 2) {
	    rb_path_check(path);
	}
    }
    else {
	path = 0;
    }

    return dln_find_file(file, path);
}

void
rb_load(fname, priv)
    VALUE fname, priv;
{
    int state;
    char *file;
    volatile ID last_func;
    TMP_PROTECT;

    rb_secure(4);
    Check_SafeStr(fname);
#ifndef __MACOS__
    if (RSTRING(fname)->ptr[0] == '~') {
	fname = rb_file_s_expand_path(1, &fname);
    }
#endif
    file = find_file(RSTRING(fname)->ptr);
    if (!file) {
	rb_raise(rb_eLoadError, "No such file to load -- %s",
		 RSTRING(fname)->ptr);
    }

    PUSH_VARS();
    PUSH_TAG(PROT_NONE);
    PUSH_CLASS();
    if (priv == 0 || NIL_P(priv)) {
	rb_secure(4);		/* should alter global state */
	ruby_class = rb_cObject;
    }
    else {
	switch (TYPE(priv)) {
	  case T_MODULE:
	  case T_CLASS:
	    break;
	    rb_secure(4);	/* should alter global state */
	    ruby_class = priv;
	  default:
	    /* load in anonymous module as toplevel */
	    ruby_class = rb_module_new();
	    break;
	}
    }
    PUSH_FRAME();
    ruby_frame->last_func = 0;
    ruby_frame->self = rb_top_self;
    ruby_frame->cbase = (VALUE)rb_node_newnode(NODE_CREF,ruby_class,0,0);
    PUSH_SCOPE();
    if (ruby_class == rb_cObject && top_scope->local_tbl) {
	int len = top_scope->local_tbl[0]+1;
	ID *tbl = ALLOC_N(ID, len);
	VALUE *vars = TMP_ALLOC(VALUE, len);
	*vars++ = 0;
	MEMCPY(tbl, top_scope->local_tbl, ID, len);
	MEMCPY(vars, top_scope->local_vars, ID, len-1);
	ruby_scope->local_tbl = tbl;   /* copy toplevel scope */
	ruby_scope->local_vars = vars; /* will not alter toplevel variables */
    }
    /* default visibility is private at loading toplevel */
    SCOPE_SET(SCOPE_PRIVATE);

    state = EXEC_TAG();
    last_func = ruby_frame->last_func;
    if (state == 0) {
	rb_in_eval++;
	rb_load_file(file);
	rb_in_eval--;
	if (ruby_nerrs == 0) {
	    eval_node(rb_top_self);
	}
    }
    ruby_frame->last_func = last_func;
    if (ruby_scope->flag == SCOPE_ALLOCA && ruby_class == rb_cObject) {
	if (ruby_scope->local_tbl) /* toplevel was empty */
	    free(ruby_scope->local_tbl);
    }
    POP_SCOPE();
    POP_FRAME();
    POP_CLASS();
    POP_TAG();
    POP_VARS();
    if (ruby_nerrs > 0) {
	rb_exc_raise(rb_errinfo);
    }
    if (state) JUMP_TAG(state);
}

static VALUE
rb_f_load(argc, argv)
    int argc;
    VALUE *argv;
{
    VALUE fname, priv;

    rb_scan_args(argc, argv, "11", &fname, &priv);
    rb_load(fname, priv);
    return Qtrue;
}

static VALUE rb_features;

static int
rb_provided(feature)
    char *feature;
{
    VALUE *p, *pend;
    char *f;
    int len;

    p = RARRAY(rb_features)->ptr;
    pend = p + RARRAY(rb_features)->len;
    while (p < pend) {
	f = STR2CSTR(*p);
	if (strcmp(f, feature) == 0) return Qtrue;
	len = strlen(feature);
	if (strncmp(f, feature, len) == 0
	    && (strcmp(f+len, ".rb") == 0 ||strcmp(f+len, ".o") == 0)) {
	    return Qtrue;
	}
	p++;
    }
    return Qfalse;
}

#ifdef USE_THREAD
static int rb_thread_loading _((char*));
static void rb_thread_loading_done _((void));
#endif

void
rb_provide(feature)
    char *feature;
{
    char *buf, *ext;

    if (!rb_provided(feature)) {
	ext = strrchr(feature, '.');
	if (strcmp(DLEXT, ext) == 0) {
	    buf = ALLOCA_N(char, strlen(feature)+1);
	    strcpy(buf, feature);
	    ext = strrchr(buf, '.');
	    strcpy(ext, ".o");
	    feature = buf;
	}
	rb_ary_push(rb_features, rb_str_new2(feature));
    }
}

VALUE
rb_f_require(obj, fname)
    VALUE obj, fname;
{
    char *ext, *file, *feature, *buf; /* OK */
    VALUE load;

    rb_secure(4);
    Check_SafeStr(fname);
    if (rb_provided(RSTRING(fname)->ptr))
	return Qfalse;

    ext = strrchr(RSTRING(fname)->ptr, '.');
    if (ext) {
	if (strcmp(".rb", ext) == 0) {
	    feature = file = RSTRING(fname)->ptr;
	    file = find_file(file);
	    if (file) goto rb_load;
	}
	else if (strcmp(".o", ext) == 0) {
	    file = feature = RSTRING(fname)->ptr;
	    if (strcmp(".o", DLEXT) != 0) {
		buf = ALLOCA_N(char, strlen(file)+sizeof(DLEXT)+1);
		strcpy(buf, feature);
		ext = strrchr(buf, '.');
		strcpy(ext, DLEXT);
		file = find_file(buf);
	    }
	    if (file) goto dyna_load;
	}
	else if (strcmp(DLEXT, ext) == 0) {
	    feature = RSTRING(fname)->ptr;
	    file = find_file(feature);
	    if (file) goto dyna_load;
	}
    }
    buf = ALLOCA_N(char, strlen(RSTRING(fname)->ptr) + 5);
    strcpy(buf, RSTRING(fname)->ptr);
    strcat(buf, ".rb");
    file = find_file(buf);
    if (file) {
	fname = rb_str_new2(file);
	feature = buf;
	goto rb_load;
    }
    strcpy(buf, RSTRING(fname)->ptr);
    strcat(buf, DLEXT);
    file = find_file(buf);
    if (file) {
	feature = buf;
	goto dyna_load;
    }
    rb_raise(rb_eLoadError, "No such file to load -- %s",
	     RSTRING(fname)->ptr);

  dyna_load:
#ifdef USE_THREAD
    if (rb_thread_loading(feature)) return Qfalse;
    else {
	int state;
	PUSH_TAG(PROT_NONE);
	if ((state = EXEC_TAG()) == 0) {
#endif
	    load = rb_str_new2(file);
	    file = RSTRING(load)->ptr;
	    dln_load(file);
	    rb_provide(feature);
#ifdef USE_THREAD
	}
	POP_TAG();
	rb_thread_loading_done();
	if (state) JUMP_TAG(state);
    }
#endif
    return Qtrue;

  rb_load:
#ifdef USE_THREAD
    if (rb_thread_loading(feature)) return Qfalse;
    else {
	int state;
	PUSH_TAG(PROT_NONE);
	if ((state = EXEC_TAG()) == 0) {
#endif
	    rb_load(fname, 0);
	    rb_provide(feature);
#ifdef USE_THREAD
	}
	POP_TAG();
	rb_thread_loading_done();
	if (state) JUMP_TAG(state);
    }
#endif
    return Qtrue;
}

static void
set_method_visibility(self, argc, argv, ex)
    VALUE self;
    int argc;
    VALUE *argv;
    int ex;
{
    int i;

    for (i=0; i<argc; i++) {
	rb_export_method(self, rb_to_id(argv[i]), ex);
    }
}

static VALUE
rb_mod_public(argc, argv, module)
    int argc;
    VALUE *argv;
    VALUE module;
{
    if (argc == 0) {
	SCOPE_SET(SCOPE_PUBLIC);
    }
    else {
	set_method_visibility(module, argc, argv, NOEX_PUBLIC);
    }
    return module;
}

static VALUE
rb_mod_protected(argc, argv, module)
    int argc;
    VALUE *argv;
    VALUE module;
{
    if (argc == 0) {
	SCOPE_SET(SCOPE_PROTECTED);
    }
    else {
	set_method_visibility(module, argc, argv, NOEX_PROTECTED);
    }
    return module;
}

static VALUE
rb_mod_private(argc, argv, module)
    int argc;
    VALUE *argv;
    VALUE module;
{
    if (argc == 0) {
	SCOPE_SET(SCOPE_PRIVATE);
    }
    else {
	set_method_visibility(module, argc, argv, NOEX_PRIVATE);
    }
    return module;
}

static VALUE
rb_mod_public_method(argc, argv, obj)
    int argc;
    VALUE *argv;
    VALUE obj;
{
    set_method_visibility(CLASS_OF(obj), argc, argv, NOEX_PUBLIC);
    return obj;
}

static VALUE
rb_mod_private_method(argc, argv, obj)
    int argc;
    VALUE *argv;
    VALUE obj;
{
    set_method_visibility(CLASS_OF(obj), argc, argv, NOEX_PRIVATE);
    return obj;
}

static VALUE
top_public(argc, argv)
    int argc;
    VALUE *argv;
{
    return rb_mod_public(argc, argv, rb_cObject);
}

static VALUE
top_private(argc, argv)
    int argc;
    VALUE *argv;
{
    return rb_mod_private(argc, argv, rb_cObject);
}

static VALUE
rb_mod_modfunc(argc, argv, module)
    int argc;
    VALUE *argv;
    VALUE module;
{
    int i;
    ID id;
    NODE *body;

    if (argc == 0) {
	SCOPE_SET(SCOPE_MODFUNC);
	return module;
    }

    set_method_visibility(module, argc, argv, NOEX_PRIVATE);
    for (i=0; i<argc; i++) {
	id = rb_to_id(argv[i]);
	body = search_method(module, id, 0);
	if (body == 0 || body->nd_body == 0) {
	    rb_raise(rb_eNameError, "undefined method `%s' for module `%s'",
		     rb_id2name(id), rb_class2name(module));
	}
	rb_clear_cache_by_id(id);
	rb_add_method(rb_singleton_class(module), id, body->nd_body, NOEX_PUBLIC);
    }
    return module;
}

static VALUE
rb_mod_append_features(module, include)
    VALUE module, include;
{
    switch (TYPE(include)) {
      case T_CLASS:
      case T_MODULE:
	break;
      default:
	Check_Type(include, T_CLASS);
	break;
    }
    rb_include_module(include, module);

    return module;
}

static VALUE
rb_mod_include(argc, argv, module)
    int argc;
    VALUE *argv;
    VALUE module;
{
    int i;

    for (i=0; i<argc; i++) {
	Check_Type(argv[i], T_MODULE);
	rb_funcall(argv[i], rb_intern("append_features"), 1, module);
    }
    return module;
}

void
rb_obj_call_init(obj)
    VALUE obj;
{
    PUSH_ITER(rb_iterator_p()?ITER_PRE:ITER_NOT);
    rb_funcall2(obj, init, ruby_frame->argc, ruby_frame->argv);
    POP_ITER();
}

VALUE
rb_class_new_instance(argc, argv, klass)
    int argc;
    VALUE *argv;
    VALUE klass;
{
    VALUE obj;

    if (FL_TEST(klass, FL_SINGLETON)) {
	rb_raise(rb_eTypeError, "can't create instance of virtual class");
    }
    obj = rb_obj_alloc(klass);
    rb_obj_call_init(obj);

    return obj;
}

static VALUE
top_include(argc, argv)
    int argc;
    VALUE *argv;
{
    rb_secure(4);
    return rb_mod_include(argc, argv, rb_cObject);
}

void
rb_extend_object(obj, module)
    VALUE obj, module;
{
    rb_include_module(rb_singleton_class(obj), module);
}

static VALUE
rb_mod_extend_object(mod, obj)
    VALUE mod, obj;
{
    rb_extend_object(obj, mod);
    return obj;
}

static VALUE
rb_obj_extend(argc, argv, obj)
    int argc;
    VALUE *argv;
    VALUE obj;
{
    int i;

    for (i=0; i<argc; i++) Check_Type(argv[i], T_MODULE);
    for (i=0; i<argc; i++) {
	rb_funcall(argv[i], rb_intern("extend_object"), 1, obj);
    }
    return obj;
}

VALUE rb_f_trace_var();
VALUE rb_f_untrace_var();

static void
errinfo_setter(val, id, var)
    VALUE val;
    ID id;
    VALUE *var;
{
    if (!rb_obj_is_kind_of(val, rb_eException)) {
	rb_raise(rb_eTypeError, "assigning non-exception to $!");
    }
    *var = val;
}

static VALUE
errat_getter(id)
    ID id;
{
    return get_backtrace(rb_errinfo);
}

static void
errat_setter(val, id, var)
    VALUE val;
    ID id;
    VALUE *var;
{
    if (NIL_P(rb_errinfo)) {
	rb_raise(rb_eArgError, "$! not set");
    }
    set_backtrace(rb_errinfo, val);
}

VALUE rb_f_global_variables();
VALUE f_instance_variables();

static VALUE
rb_f_local_variables()
{
    ID *tbl;
    int n, i;
    VALUE ary = rb_ary_new();
    struct RVarmap *vars;

    tbl = ruby_scope->local_tbl;
    if (tbl) {
	n = *tbl++;
	for (i=2; i<n; i++) {	/* skip first 2 ($_ and $~) */
	    if (tbl[i] == 0) continue;  /* skip flip states */
	    rb_ary_push(ary, rb_str_new2(rb_id2name(tbl[i])));
	}
    }

    vars = ruby_dyna_vars;
    while (vars) {
	if (vars->id) {
	    rb_ary_push(ary, rb_str_new2(rb_id2name(vars->id)));
	}
	vars = vars->next;
    }

    return ary;
}

static VALUE rb_f_catch _((VALUE,VALUE));
static VALUE rb_f_throw _((int,VALUE*)) NORETURN;

struct end_proc_data {
    void (*func)();
    VALUE data;
    struct end_proc_data *next;
};
static struct end_proc_data *end_proc_data;

void
rb_set_end_proc(func, data)
    void (*func)();
    VALUE data;
{
    struct end_proc_data *link = ALLOC(struct end_proc_data);

    link->next = end_proc_data;
    link->func = func;
    link->data = data;
    rb_global_variable(&link->data);
    end_proc_data = link;
}

static void
call_end_proc(data)
    VALUE data;
{
    proc_call(data, Qnil);
}

static void
rb_f_END()
{
    PUSH_FRAME();
    ruby_frame->argc = 0;
    rb_set_end_proc(call_end_proc, rb_f_lambda());
    POP_FRAME();
}

static VALUE
rb_f_at_exit()
{
    VALUE proc;

    proc = rb_f_lambda();

    rb_set_end_proc(call_end_proc, proc);
    return proc;
}

static void
exec_end_proc()
{
    struct end_proc_data *link = end_proc_data;

    while (link) {
	(*link->func)(link->data);
	link = link->next;
    }
}

void
Init_eval()
{
    init = rb_intern("initialize");
    eqq = rb_intern("===");
    each = rb_intern("each");

    aref = rb_intern("[]");
    aset = rb_intern("[]=");
    match = rb_intern("=~");

    rb_global_variable((VALUE*)&top_scope);
    rb_global_variable((VALUE*)&ruby_eval_tree_begin);

    rb_global_variable((VALUE*)&ruby_eval_tree);
    rb_global_variable((VALUE*)&ruby_dyna_vars);

    rb_define_virtual_variable("$@", errat_getter, errat_setter);
    rb_define_hooked_variable("$!", &rb_errinfo, 0, errinfo_setter);

    rb_define_global_function("eval", rb_f_eval, -1);
    rb_define_global_function("iterator?", rb_f_iterator_p, 0);
    rb_define_global_function("method_missing", rb_f_missing, -1);
    rb_define_global_function("loop", rb_f_loop, 0);

    rb_define_method(rb_mKernel, "respond_to?", rb_obj_respond_to, -1);

    rb_define_global_function("raise", rb_f_raise, -1);
    rb_define_global_function("fail", rb_f_raise, -1);

    rb_define_global_function("caller", rb_f_caller, -1);

    rb_define_global_function("exit", rb_f_exit, -1);
    rb_define_global_function("abort", rb_f_abort, 0);

    rb_define_global_function("at_exit", rb_f_at_exit, 0);

    rb_define_global_function("catch", rb_f_catch, 1);
    rb_define_global_function("throw", rb_f_throw, -1);
    rb_define_global_function("global_variables", rb_f_global_variables, 0);
    rb_define_global_function("local_variables", rb_f_local_variables, 0);

    rb_define_method(rb_mKernel, "send", rb_f_send, -1);
    rb_define_method(rb_mKernel, "__send__", rb_f_send, -1);
    rb_define_method(rb_mKernel, "instance_eval", rb_obj_instance_eval, -1);

    rb_define_private_method(rb_cModule, "append_features", rb_mod_append_features, 1);
    rb_define_private_method(rb_cModule, "extend_object", rb_mod_extend_object, 1);
    rb_define_private_method(rb_cModule, "include", rb_mod_include, -1);
    rb_define_private_method(rb_cModule, "public", rb_mod_public, -1);
    rb_define_private_method(rb_cModule, "protected", rb_mod_protected, -1);
    rb_define_private_method(rb_cModule, "private", rb_mod_private, -1);
    rb_define_private_method(rb_cModule, "module_function", rb_mod_modfunc, -1);
    rb_define_method(rb_cModule, "method_defined?", rb_mod_method_defined, 1);
    rb_define_method(rb_cModule, "public_class_method", rb_mod_public_method, -1);
    rb_define_method(rb_cModule, "private_class_method", rb_mod_private_method, -1);
    rb_define_method(rb_cModule, "module_eval", rb_mod_module_eval, -1);
    rb_define_method(rb_cModule, "class_eval", rb_mod_module_eval, -1);

    rb_define_private_method(rb_cModule, "remove_method", rb_mod_remove_method, 1);
    rb_define_private_method(rb_cModule, "undef_method", rb_mod_undef_method, 1);
    rb_define_private_method(rb_cModule, "alias_method", rb_mod_alias_method, 2);

    rb_define_singleton_method(rb_cModule, "nesting", rb_mod_nesting, 0);
    rb_define_singleton_method(rb_cModule, "constants", rb_mod_s_constants, 0);

    rb_define_singleton_method(rb_top_self, "include", top_include, -1);
    rb_define_singleton_method(rb_top_self, "public", top_public, -1);
    rb_define_singleton_method(rb_top_self, "private", top_private, -1);

    rb_define_method(rb_mKernel, "extend", rb_obj_extend, -1);

    rb_define_global_function("trace_var", rb_f_trace_var, -1);
    rb_define_global_function("untrace_var", rb_f_untrace_var, -1);

    rb_define_global_function("set_trace_func", set_trace_func, 1);

    rb_define_virtual_variable("$SAFE", safe_getter, safe_setter);
}

VALUE rb_f_autoload();

void
Init_load()
{
    rb_load_path = rb_ary_new();
    rb_define_readonly_variable("$:", &rb_load_path);
    rb_define_readonly_variable("$-I", &rb_load_path);
    rb_define_readonly_variable("$LOAD_PATH", &rb_load_path);

    rb_features = rb_ary_new();
    rb_define_readonly_variable("$\"", &rb_features);

    rb_define_global_function("load", rb_f_load, -1);
    rb_define_global_function("require", rb_f_require, 1);
    rb_define_global_function("autoload", rb_f_autoload, 2);
}

static void
scope_dup(scope)
    struct SCOPE *scope;
{
    ID *tbl;
    VALUE *vars;

    if (scope->flag & SCOPE_MALLOC) return;

    if (scope->local_tbl) {
	tbl = scope->local_tbl;
	vars = ALLOC_N(VALUE, tbl[0]+1);
	*vars++ = scope->local_vars[-1];
	MEMCPY(vars, scope->local_vars, VALUE, tbl[0]);
	scope->local_vars = vars;
	scope->flag = SCOPE_MALLOC;
    }
    else {
        scope->flag = SCOPE_NOSTACK;
    }
}

static void
blk_mark(data)
    struct BLOCK *data;
{
    while (data) {
	rb_gc_mark_frame(&data->frame);
	rb_gc_mark(data->scope);
	rb_gc_mark(data->var);
	rb_gc_mark(data->body);
	rb_gc_mark(data->self);
	rb_gc_mark(data->d_vars);
	rb_gc_mark(data->klass);
	data = data->prev;
    }
}

static void
blk_free(data)
    struct BLOCK *data;
{
    struct BLOCK *tmp;

    while (data) {
	free(data->frame.argv);
	tmp = data;
	data = data->prev;
	free(tmp);
    }
}

static void
blk_copy_prev(block)
    struct BLOCK *block;
{
    struct BLOCK *tmp;

    while (block->prev) {
	tmp = ALLOC_N(struct BLOCK, 1);
	MEMCPY(tmp, block->prev, struct BLOCK, 1);
	tmp->frame.argv = ALLOC_N(VALUE, tmp->frame.argc);
	MEMCPY(tmp->frame.argv, block->frame.argv, VALUE, tmp->frame.argc);
	block->prev = tmp;
	block = tmp;
    }
}


static VALUE
bind_clone(self)
    VALUE self;
{
    struct BLOCK *orig, *data;
    VALUE bind;

    Data_Get_Struct(self, struct BLOCK, orig);
    bind = Data_Make_Struct(self,struct BLOCK,blk_mark,blk_free,data);
    MEMCPY(data, orig, struct BLOCK, 1);
    data->frame.argv = ALLOC_N(VALUE, orig->frame.argc);
    MEMCPY(data->frame.argv, orig->frame.argv, VALUE, orig->frame.argc);

    if (data->iter) {
	blk_copy_prev(data);
    }
    else {
	data->prev = 0;
    }

    return bind;
}

static VALUE
rb_f_binding(self)
    VALUE self;
{
    struct BLOCK *data;
    VALUE bind;

    PUSH_BLOCK(0,0);
    bind = Data_Make_Struct(rb_cBinding,struct BLOCK,blk_mark,blk_free,data);
    *data = *ruby_block;

#ifdef USE_THREAD
    data->orig_thread = rb_thread_current();
#endif
    data->iter = rb_f_iterator_p();
    if (ruby_frame->prev) {
	data->frame.last_func = ruby_frame->prev->last_func;
    }
    data->frame.argv = ALLOC_N(VALUE, data->frame.argc);
    MEMCPY(data->frame.argv, ruby_block->frame.argv, VALUE, data->frame.argc);

    if (data->iter) {
	blk_copy_prev(data);
    }
    else {
	data->prev = 0;
    }

    scope_dup(data->scope);
    POP_BLOCK();

    return bind;
}

#define PROC_T3    FL_USER1
#define PROC_T4    FL_USER2
#define PROC_T5    (FL_USER1|FL_USER2)
#define PROC_TMASK (FL_USER1|FL_USER2)

static void
proc_save_safe_level(data)
    VALUE data;
{
    if (FL_TEST(data, FL_TAINT)) {
	switch (safe_level) {
	  case 3:
	    FL_SET(data, PROC_T3);
	    break;
	  case 4:
	    FL_SET(data, PROC_T4);
	    break;
	  case 5:
	    FL_SET(data, PROC_T5);
	    break;
	}
    }
}

static void
proc_set_safe_level(data)
    VALUE data;
{
    if (FL_TEST(data, FL_TAINT)) {
	switch (RBASIC(data)->flags & PROC_TMASK) {
	  case PROC_T3:
	    safe_level = 3;
	    break;
	  case PROC_T4:
	    safe_level = 4;
	    break;
	  case PROC_T5:
	    safe_level = 5;
	    break;
	}
    }
}

static VALUE
proc_s_new(klass)
    VALUE klass;
{
    volatile VALUE proc;
    struct BLOCK *data;

    if (!rb_iterator_p() && !rb_f_iterator_p()) {
	rb_raise(rb_eArgError, "tried to create Procedure-Object out of iterator");
    }

    ruby_block->used = 1;
    proc = Data_Make_Struct(klass, struct BLOCK, blk_mark, blk_free, data);
    *data = *ruby_block;

#ifdef USE_THREAD
    data->orig_thread = rb_thread_current();
#endif
    data->iter = data->prev?Qtrue:Qfalse;
    data->frame.argv = ALLOC_N(VALUE, data->frame.argc);
    MEMCPY(data->frame.argv, ruby_block->frame.argv, VALUE, data->frame.argc);
    if (data->iter) {
	blk_copy_prev(data);
    }
    else {
	data->prev = 0;
    }

    scope_dup(data->scope);
    proc_save_safe_level(proc);
    rb_obj_call_init(proc);

    return proc;
}

VALUE
rb_f_lambda()
{
    return proc_s_new(rb_cProc);
}

static int
blk_orphan(data)
    struct BLOCK *data;
{
    if (data->scope && data->scope != top_scope &&
	(data->scope->flag & SCOPE_NOSTACK)) {
	return 1;
    }
#ifdef USE_THREAD
    if (data->orig_thread != rb_thread_current()) {
	return 1;
    }
#endif
    return 0;
}

static VALUE
proc_call(proc, args)
    VALUE proc, args;		/* OK */
{
    struct BLOCK * volatile old_block;
    struct BLOCK *data;
    volatile VALUE result = Qnil;
    int state;
    volatile int orphan;
    volatile int safe = safe_level;

    if (TYPE(args) == T_ARRAY) {
	switch (RARRAY(args)->len) {
	  case 0:
	    args = Qnil;
	    break;
	  case 1:
	    args = RARRAY(args)->ptr[0];
	    break;
	}
    }

    Data_Get_Struct(proc, struct BLOCK, data);
    orphan = blk_orphan(data);

    /* PUSH BLOCK from data */
    old_block = ruby_block;
    ruby_block = data;
    PUSH_ITER(ITER_CUR);
    ruby_frame->iter = ITER_CUR;

    if (orphan) {/* orphan procedure */
	if (rb_iterator_p()) {
	    ruby_block->frame.iter = ITER_CUR;
	}
	else {
	    ruby_block->frame.iter = ITER_NOT;
	}
    }

    PUSH_TAG(PROT_NONE);
    state = EXEC_TAG();
    if (state == 0) {
	proc_set_safe_level(proc);
	result = rb_yield_0(args, 0, 0);
    }
    POP_TAG();

    POP_ITER();
    if (ruby_block->tag->dst == state) {
	state &= TAG_MASK;
    }
    ruby_block = old_block;
    safe_level = safe;

    if (state) {
	if (orphan) {/* orphan procedure */
	    switch (state) {
	      case TAG_BREAK:
		rb_raise(rb_eLocalJumpError, "break from proc-closure");
		break;
	      case TAG_RETRY:
		rb_raise(rb_eLocalJumpError, "retry from proc-closure");
		break;
	      case TAG_RETURN:
		rb_raise(rb_eLocalJumpError, "return from proc-closure");
		break;
	    }
	}
	JUMP_TAG(state);
    }
    return result;
}

static VALUE
block_pass(self, node)
    VALUE self;
    NODE *node;
{
    VALUE block = rb_eval(self, node->nd_body);
    struct BLOCK * volatile old_block;
    struct BLOCK *data;
    volatile VALUE result = Qnil;
    int state;
    volatile int orphan;
    volatile int safe = safe_level;

    if (NIL_P(block)) {
	return rb_eval(self, node->nd_iter);
    }
    if (rb_obj_is_kind_of(block, rb_cMethod)) {
	block = method_proc(block);
    }
    else if (!rb_obj_is_proc(block)) {
	rb_raise(rb_eTypeError, "wrong argument type %s (expected Proc)",
		 rb_class2name(CLASS_OF(block)));
    }

    Data_Get_Struct(block, struct BLOCK, data);
    orphan = blk_orphan(data);

    /* PUSH BLOCK from data */
    old_block = ruby_block;
    ruby_block = data;
    PUSH_ITER(ITER_PRE);
    ruby_frame->iter = ITER_PRE;

    PUSH_TAG(PROT_NONE);
    state = EXEC_TAG();
    if (state == 0) {
	proc_set_safe_level(block);
	result = rb_eval(self, node->nd_iter);
    }
    POP_TAG();
    POP_ITER();
    if (ruby_block->tag->dst == state) {
	state &= TAG_MASK;
	orphan = 2;
    }
    ruby_block = old_block;
    safe_level = safe;

    if (state) {
	if (orphan == 2) {/* escape from orphan procedure */
	    switch (state) {
	      case TAG_BREAK:
		rb_raise(rb_eLocalJumpError, "break from proc-closure");
		break;
	      case TAG_RETRY:
		rb_raise(rb_eLocalJumpError, "retry from proc-closure");
		break;
	      case TAG_RETURN:
		rb_raise(rb_eLocalJumpError, "return from proc-closure");
		break;
	    }
	}
	JUMP_TAG(state);
    }
    return result;
}

struct METHOD {
    VALUE klass, oklass;
    VALUE recv;
    ID id, oid;
    NODE *body;
};

static void
bm_mark(data)
    struct METHOD *data;
{
    rb_gc_mark(data->oklass);
    rb_gc_mark(data->klass);
    rb_gc_mark(data->recv);
    rb_gc_mark(data->body);
}

static VALUE
rb_obj_method(obj, vid)
    VALUE obj;
    VALUE vid;
{
    VALUE method;
    VALUE klass = CLASS_OF(obj);
    ID id;
    NODE *body;
    int noex;
    struct METHOD *data;

    id = rb_to_id(vid);

  again:
    if ((body = rb_get_method_body(&klass, &id, &noex)) == 0) {
	return rb_undefined(obj, rb_to_id(vid), 0, 0, 0);
    }

    if (nd_type(body) == NODE_ZSUPER) {
	klass = RCLASS(klass)->super;
	goto again;
    }

    method = Data_Make_Struct(rb_cMethod, struct METHOD, bm_mark, free, data);
    data->klass = klass;
    data->recv = obj;
    data->id = id;
    data->body = body;
    data->oklass = CLASS_OF(obj);
    data->oid = rb_to_id(vid);
    if (FL_TEST(obj, FL_TAINT)) {
	FL_SET(method, FL_TAINT);
    }

    return method;
}

static VALUE
method_call(argc, argv, method)
    int argc;
    VALUE *argv;
    VALUE method;
{
    VALUE result;
    struct METHOD *data;
    int state;
    volatile int safe = safe_level;

    Data_Get_Struct(method, struct METHOD, data);
    PUSH_ITER(rb_iterator_p()?ITER_PRE:ITER_NOT);
    PUSH_TAG(PROT_NONE);
    if (FL_TEST(data->recv, FL_TAINT)) {
	FL_SET(method, FL_TAINT);
	if (safe_level < 4) safe_level = 4;
    }
    if ((state = EXEC_TAG()) == 0) {
	result = rb_call0(data->klass, data->recv, data->id,
			  argc, argv, data->body, 0);
    }
    POP_TAG();
    POP_ITER();
    safe_level = safe;
    if (state) JUMP_TAG(state);
    return result;
}

static VALUE
method_inspect(method)
    VALUE method;
{
    struct METHOD *data;
    VALUE str;
    char *s;

    Data_Get_Struct(method, struct METHOD, data);
    str = rb_str_new2("#<");
    s = rb_class2name(CLASS_OF(method));
    rb_str_cat(str, s, strlen(s));
    rb_str_cat(str, ": ", 2);
    s = rb_class2name(data->oklass);
    rb_str_cat(str, s, strlen(s));
    rb_str_cat(str, "#", 1);
    s = rb_id2name(data->oid);
    rb_str_cat(str, s, strlen(s));
    rb_str_cat(str, ">", 1);

    return str;
}

static VALUE
mproc()
{
    VALUE proc;

    /* emulate ruby's method call */
    PUSH_ITER(ITER_CUR);
    PUSH_FRAME();
    proc = rb_f_lambda();
    POP_FRAME();
    POP_ITER();

    return proc;
}

static VALUE
mcall(args, method)
    VALUE args, method;
{
    if (TYPE(args) == T_ARRAY) {
	return method_call(RARRAY(args)->len, RARRAY(args)->ptr, method);
    }
    return method_call(1, &args, method);
}

static VALUE
method_proc(method)
    VALUE method;
{
    return rb_iterate(mproc, 0, mcall, method);
}

void
Init_Proc()
{
    rb_eLocalJumpError = rb_define_class("LocalJumpError", rb_eStandardError);
    rb_eSysStackError = rb_define_class("SystemStackError", rb_eStandardError);

    rb_cProc = rb_define_class("Proc", rb_cObject);
    rb_define_singleton_method(rb_cProc, "new", proc_s_new, 0);

    rb_define_method(rb_cProc, "call", proc_call, -2);
    rb_define_global_function("proc", rb_f_lambda, 0);
    rb_define_global_function("lambda", rb_f_lambda, 0);
    rb_define_global_function("binding", rb_f_binding, 0);
    rb_cBinding = rb_define_class("Binding", rb_cObject);
    rb_undef_method(CLASS_OF(rb_cMethod), "new");
    rb_define_method(rb_cBinding, "clone", bind_clone, 0);

    rb_cMethod = rb_define_class("Method", rb_cObject);
    rb_undef_method(CLASS_OF(rb_cMethod), "new");
    rb_define_method(rb_cMethod, "call", method_call, -1);
    rb_define_method(rb_cMethod, "inspect", method_inspect, 0);
    rb_define_method(rb_cMethod, "to_s", method_inspect, 0);
    rb_define_method(rb_cMethod, "to_proc", method_proc, 0);
    rb_define_method(rb_mKernel, "method", rb_obj_method, 1);
}

#ifdef USE_THREAD

static VALUE rb_eThreadError;

int rb_thread_pending = 0;

VALUE rb_cThread;

#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#else
#ifndef NT
struct timeval {
        long    tv_sec;         /* seconds */
        long    tv_usec;        /* and microseconds */
};
#endif /* NT */
#endif
#include <signal.h>
#include <errno.h>

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

extern VALUE rb_last_status;

enum thread_status {
    THREAD_RUNNABLE,
    THREAD_STOPPED,
    THREAD_TO_KILL,
    THREAD_KILLED,
};

#define WAIT_FD		(1<<0)
#define WAIT_TIME	(1<<1)
#define WAIT_JOIN	(1<<2)

/* +infty, for this purpose */
#define DELAY_INFTY 1E30

typedef struct thread * thread_t;

struct thread {
    struct thread *next, *prev;
    jmp_buf context;

    VALUE result;

    int   stk_len;
    int   stk_max;
    VALUE*stk_ptr;
    VALUE*stk_pos;

    struct FRAME *frame;
    struct SCOPE *scope;
    struct RVarmap *dyna_vars;
    struct BLOCK *block;
    struct BLOCK *cblock;
    struct iter *iter;
    struct tag *tag;
    VALUE klass;

    VALUE trace;
    int misc;			/* misc. states (vmode/rb_trap_immediate) */

    char *file;
    int   line;

    VALUE rb_errinfo;
    VALUE last_status;
    VALUE last_line;
    VALUE last_match;

    int safe;

    enum thread_status status;
    int wait_for;
    int fd;
    double delay;
    thread_t join;

    int abort;

    VALUE thread;
};

static thread_t curr_thread;
static int num_waiting_on_fd;
static int num_waiting_on_timer;
static int num_waiting_on_join;

#define FOREACH_THREAD_FROM(f,x) x = f; do { x = x->next;
#define END_FOREACH_FROM(f,x) } while (x != f)

#define FOREACH_THREAD(x) FOREACH_THREAD_FROM(curr_thread,x)
#define END_FOREACH(x)    END_FOREACH_FROM(curr_thread,x)

/* Return the current time as a floating-point number */
static double
timeofday()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec * 1e-6;
}

static thread_t main_thread;

#define ADJ(addr) (void*)(((VALUE*)(addr)-th->stk_pos)+th->stk_ptr)
#define STACK(addr) (th->stk_pos<(addr) && (addr)<th->stk_pos+th->stk_len)

static void
thread_mark(th)
    thread_t th;
{
    struct FRAME *frame;
    struct BLOCK *block;

    rb_gc_mark(th->result);
    rb_gc_mark(th->thread);
    if (th->join) rb_gc_mark(th->join->thread);

    rb_gc_mark(th->scope);
    rb_gc_mark(th->dyna_vars);
    rb_gc_mark(th->rb_errinfo);
    rb_gc_mark(th->last_line);
    rb_gc_mark(th->last_match);

    /* mark data in copied stack */
    if (th->status == THREAD_KILLED) return;
    if (th->stk_len == 0) return;  /* stack not active, no need to mark. */
    if (th->stk_ptr) {
	rb_gc_mark_locations(th->stk_ptr, th->stk_ptr+th->stk_len);
#if defined(THINK_C) || defined(__human68k__)
	rb_gc_mark_locations(th->stk_ptr+2, th->stk_ptr+th->stk_len+2);
#endif
    }
    frame = th->frame;
    while (frame && frame != top_frame) {
	frame = ADJ(frame);
	if (frame->argv && !STACK(frame->argv)) {
	    rb_gc_mark_frame(frame);
	}
	frame = frame->prev;
    }
    block = th->block;
    while (block) {
	block = ADJ(block);
	if (block->frame.argv && !STACK(block->frame.argv)) {
	    rb_gc_mark_frame(&block->frame);
	}
	block = block->prev;
    }
}

void
rb_gc_mark_threads()
{
    thread_t th;

    if (!curr_thread) return;
    FOREACH_THREAD(th) {
	rb_gc_mark(th->thread);
    } END_FOREACH(th);
}

static void
thread_free(th)
    thread_t th;
{
    if (th->stk_ptr) free(th->stk_ptr);
    th->stk_ptr = 0;
    if (th != main_thread) free(th);
}

static thread_t
rb_thread_check(data)
    VALUE data;
{
    if (TYPE(data) != T_DATA || RDATA(data)->dfree != thread_free) {
	rb_raise(rb_eTypeError, "wrong argument type %s (expected Thread)",
		 rb_class2name(CLASS_OF(data)));
    }
    return (thread_t)RDATA(data)->data;
}

static void
rb_thread_save_context(th)
    thread_t th;
{
    VALUE v;

    int len = stack_length();
    th->stk_len = 0;
    th->stk_pos = (rb_gc_stack_start<(VALUE*)&v)?rb_gc_stack_start
				                :rb_gc_stack_start - len;
    if (len > th->stk_max)  {
	REALLOC_N(th->stk_ptr, VALUE, len);
	th->stk_max = len;
    }
    th->stk_len = len;
    FLUSH_REGISTER_WINDOWS; 
    MEMCPY(th->stk_ptr, th->stk_pos, VALUE, th->stk_len);

    th->frame = ruby_frame;
    th->scope = ruby_scope;
    th->klass = ruby_class;
    th->dyna_vars = ruby_dyna_vars;
    th->block = ruby_block;
    th->cblock = ruby_calling_block;
    th->misc = scope_vmode | (rb_trap_immediate<<8);
    th->iter = ruby_iter;
    th->tag = prot_tag;
    th->rb_errinfo = rb_errinfo;
    th->last_status = rb_last_status;
    th->last_line = rb_lastline_get();
    th->last_match = rb_backref_get();
    th->safe = safe_level;

    th->trace = trace_func;
    th->file = ruby_sourcefile;
    th->line = ruby_sourceline;
}

static void rb_thread_restore_context _((thread_t,int));

static void
stack_extend(th, exit)
    thread_t th;
    int exit;
{
    VALUE space[1024];

    memset(space, 0, 1);	/* prevent array from optimization */
    rb_thread_restore_context(th, exit);
}

static int   th_raise_argc;
static VALUE th_raise_argv[2];
static char *th_raise_file;
static int   th_raise_line;
static VALUE th_cmd;
static int   th_sig;

static void
rb_thread_restore_context(th, exit)
    thread_t th;
    int exit;
{
    VALUE v;
    static thread_t tmp;
    static int ex;

    if (!th->stk_ptr) rb_bug("unsaved context");

    if (&v < rb_gc_stack_start) {
	/* Stack grows downward */
	if (&v > th->stk_pos) stack_extend(th, exit);
    }
    else {
	/* Stack grows upward */
	if (&v < th->stk_pos + th->stk_len) stack_extend(th, exit);
    }

    ruby_frame = th->frame;
    ruby_scope = th->scope;
    ruby_class = th->klass;
    ruby_dyna_vars = th->dyna_vars;
    ruby_block = th->block;
    ruby_calling_block = th->cblock;
    scope_vmode = th->misc&SCOPE_MASK;
    rb_trap_immediate = th->misc>>8;
    ruby_iter = th->iter;
    prot_tag = th->tag;
    rb_errinfo = th->rb_errinfo;
    rb_last_status = th->last_status;
    safe_level = th->safe;

    trace_func = th->trace;
    ruby_sourcefile = th->file;
    ruby_sourceline = th->line;

    tmp = th;
    ex = exit;
    FLUSH_REGISTER_WINDOWS;
    MEMCPY(tmp->stk_pos, tmp->stk_ptr, VALUE, tmp->stk_len);

    rb_lastline_set(tmp->last_line);
    rb_backref_set(tmp->last_match);

    switch (ex) {
      case 1:
	JUMP_TAG(TAG_FATAL);
	break;

      case 2:
	rb_interrupt();
	break;

      case 3:
	rb_trap_eval(th_cmd, th_sig);
	errno = EINTR;
	break;

      case 4:
	ruby_frame->last_func = 0;
	ruby_sourcefile = th_raise_file;
	ruby_sourceline = th_raise_line;
	rb_f_raise(th_raise_argc, th_raise_argv);
	break;

      default:
	longjmp(tmp->context, 1);
    }
}

static void
rb_thread_ready(th)
    thread_t th;
{
    /* The thread is no longer waiting on anything */
    if (th->wait_for & WAIT_FD) {
	num_waiting_on_fd--;
    }
    if (th->wait_for & WAIT_TIME) {
	num_waiting_on_timer--;
    }
    if (th->wait_for & WAIT_JOIN) {
	num_waiting_on_join--;
    }
    th->wait_for = 0;
    th->status = THREAD_RUNNABLE;
}

static void
rb_thread_remove()
{
    rb_thread_ready(curr_thread);
    curr_thread->status = THREAD_KILLED;
    curr_thread->prev->next = curr_thread->next;
    curr_thread->next->prev = curr_thread->prev;
}

static int
rb_thread_dead(th)
    thread_t th;
{
    return th->status == THREAD_KILLED;
}

static void
rb_thread_deadlock()
{
    curr_thread = main_thread;
    th_raise_argc = 1;
    th_raise_argv[0] = rb_exc_new2(rb_eFatal, "Thread: deadlock");
    th_raise_file = ruby_sourcefile;
    th_raise_line = ruby_sourceline;
    rb_abort();
}

void
rb_thread_schedule()
{
    thread_t next;		/* OK */
    thread_t th;
    thread_t curr;

  select_err:
    rb_thread_pending = 0;
    if (curr_thread == curr_thread->next) return;

    next = 0;
    curr = curr_thread;		/* starting thread */

    while (curr->status == THREAD_KILLED) {
	curr = curr->prev;
    }

    FOREACH_THREAD_FROM(curr, th) {
       if (th->status != THREAD_STOPPED && th->status != THREAD_KILLED) {
           next = th;
           break;
       }
    }
    END_FOREACH_FROM(curr, th); 

    if (num_waiting_on_join) {
	FOREACH_THREAD_FROM(curr, th) {
	    if ((th->wait_for&WAIT_JOIN) && rb_thread_dead(th->join)) {
		th->join = 0;
		th->wait_for &= ~WAIT_JOIN;
		th->status = THREAD_RUNNABLE;
		num_waiting_on_join--;
		if (!next) next = th;
	    }
	}
	END_FOREACH_FROM(curr, th);
    }

    if (num_waiting_on_fd > 0 || num_waiting_on_timer > 0) {
	fd_set readfds;
	struct timeval delay_tv, *delay_ptr;
	double delay, now;	/* OK */

	int n, max;

	do {
	    max = 0;
	    FD_ZERO(&readfds);
	    if (num_waiting_on_fd > 0) {
		FOREACH_THREAD_FROM(curr, th) {
		    if (th->wait_for & WAIT_FD) {
			FD_SET(th->fd, &readfds);
			if (th->fd > max) max = th->fd;
		    }
		}
		END_FOREACH_FROM(curr, th);
	    }

	    delay = DELAY_INFTY;
	    if (num_waiting_on_timer > 0) {
		now = timeofday();
		FOREACH_THREAD_FROM(curr, th) {
		    if (th->wait_for & WAIT_TIME) {
			if (th->delay <= now) {
			    th->delay = 0.0;
			    th->wait_for &= ~WAIT_TIME;
			    th->status = THREAD_RUNNABLE;
			    num_waiting_on_timer--;
			    next = th;
			} else if (th->delay < delay) {
			    delay = th->delay;
			}
		    }
		}
		END_FOREACH_FROM(curr, th);
	    }
	    /* Do the select if needed */
	    if (num_waiting_on_fd > 0 || !next) {
		/* Convert delay to a timeval */
		/* If a thread is runnable, just poll */
		if (next) {
		    delay_tv.tv_sec = 0;
		    delay_tv.tv_usec = 0;
		    delay_ptr = &delay_tv;
		}
		else if (delay == DELAY_INFTY) {
		    delay_ptr = 0;
		}
		else {
		    delay -= now;
		    delay_tv.tv_sec = (unsigned int)delay;
		    delay_tv.tv_usec = (delay - (double)delay_tv.tv_sec) * 1e6;
		    delay_ptr = &delay_tv;
		}

		n = select(max+1, &readfds, 0, 0, delay_ptr);
		if (n < 0) {
		    if (rb_trap_pending) rb_trap_exec();
		    goto select_err;
		}
		if (n > 0) {
		    /* Some descriptors are ready. 
		       Make the corresponding threads runnable. */
		    FOREACH_THREAD_FROM(curr, th) {
			if ((th->wait_for&WAIT_FD)
			    && FD_ISSET(th->fd, &readfds)) {
			    /* Wake up only one thread per fd. */
			    FD_CLR(th->fd, &readfds);
			    th->status = THREAD_RUNNABLE;
			    th->fd = 0;
			    th->wait_for &= ~WAIT_FD;
			    num_waiting_on_fd--;
			    if (!next) next = th; /* Found one. */
			}
		    }
		    END_FOREACH_FROM(curr, th);
		}
	    }
	    /* The delays for some of the threads should have expired.
	       Go through the loop once more, to check the delays. */
	} while (!next && delay != DELAY_INFTY);
    }

    if (!next) {
	curr_thread->file = ruby_sourcefile;
	curr_thread->line = ruby_sourceline;
	FOREACH_THREAD_FROM(curr, th) {
	    fprintf(stderr, "%s:%d:deadlock 0x%x: %d:%d %s\n", 
		    th->file, th->line, th->thread, th->status,
		    th->wait_for, th==main_thread?"(main)":"");
	}
	END_FOREACH_FROM(curr, th);
	/* raise fatal error to main thread */
	rb_thread_deadlock();
    }
    if (next->status == THREAD_RUNNABLE && next == curr_thread) {
	return;
    }

    /* context switch */
    if (curr == curr_thread) {
	rb_thread_save_context(curr);
	if (setjmp(curr->context)) {
	    return;
	}
    }

    curr_thread = next;
    if (next->status == THREAD_TO_KILL) {
	/* execute ensure-clause if any */
	rb_thread_restore_context(next, 1);
    }
    rb_thread_restore_context(next, 0);
}

void
rb_thread_wait_fd(fd)
    int fd;
{
    if (curr_thread == curr_thread->next) return;

    curr_thread->status = THREAD_STOPPED;
    curr_thread->fd = fd;
    num_waiting_on_fd++;
    curr_thread->wait_for |= WAIT_FD;
    rb_thread_schedule();
}

void
rb_thread_fd_writable(fd)
    int fd;
{
    struct timeval zero;
    fd_set fds;

    if (curr_thread == curr_thread->next) return;

    zero.tv_sec = zero.tv_usec = 0;
    for (;;) {
	FD_ZERO(&fds);
	FD_SET(fd, &fds);
	if (select(fd+1, 0, &fds, 0, &zero) == 1) break;
	rb_thread_schedule();
    }
}

void
rb_thread_wait_for(time)
    struct timeval time;
{
    double date;

    if (curr_thread == curr_thread->next) {
	int n;
#ifndef linux
	double d, limit;
	limit = timeofday()+(double)time.tv_sec+(double)time.tv_usec*1e-6;
#endif
	for (;;) {
	    TRAP_BEG;
	    n = select(0, 0, 0, 0, &time);
	    TRAP_END;
	    if (n == 0) return;

#ifndef linux
	    d = limit - timeofday();

	    time.tv_sec = (int)d;
	    time.tv_usec = (int)((d - (int)d)*1e6);
	    if (time.tv_usec < 0) {
		time.tv_usec += 1e6;
		time.tv_sec -= 1;
	    }
	    if (time.tv_sec < 0) return;
#endif
	}
    }

    date = timeofday() + (double)time.tv_sec + (double)time.tv_usec*1e-6;
    curr_thread->status = THREAD_STOPPED;
    curr_thread->delay = date;
    num_waiting_on_timer++;
    curr_thread->wait_for |= WAIT_TIME;
    rb_thread_schedule();
}

void rb_thread_sleep_forever _((void));

int
rb_thread_alone()
{
    return curr_thread == curr_thread->next;
}

int
rb_thread_select(max, read, write, except, timeout)
    int max;
    fd_set *read, *write, *except;
    struct timeval *timeout;
{
    double limit;
    struct timeval zero;
    fd_set r, *rp, w, *wp, x, *xp;
    int n;

    if (!read && !write && !except) {
	if (!timeout) {
	    rb_thread_sleep_forever();
	    return 0;
	}
	rb_thread_wait_for(*timeout);
	return 0;
    }

    if (timeout) {
	limit = timeofday()+
	    (double)timeout->tv_sec+(double)timeout->tv_usec*1e-6;
    }

    if (curr_thread == curr_thread->next) { /* no other thread */
#ifndef linux
	struct timeval tv, *tvp = timeout;

	if (timeout) {
	    tv = *timeout;
	    tvp = &tv;
	}
	for (;;) {
	    TRAP_BEG;
	    n = select(max, read, write, except, tvp);
	    TRAP_END;
	    if (n < 0 && errno == EINTR) {
		if (timeout) {
		    double d = timeofday() - limit;

		    tv.tv_sec = (unsigned int)d;
		    tv.tv_usec = (d - (double)tv.tv_sec) * 1e6;
		}
		continue;
	    }
	    return n;
	}
#else
	for (;;) {
	    TRAP_BEG;
	    n = select(max, read, write, except, timeout);
	    TRAP_END;
	    if (n < 0 && errno == EINTR) {
		continue;
	    }
	    return n;
	}
#endif

    }

    for (;;) {
	zero.tv_sec = zero.tv_usec = 0;
	if (read) {rp = &r; r = *read;} else {rp = 0;}
	if (write) {wp = &w; w = *write;} else {wp = 0;}
	if (except) {xp = &x; x = *except;} else {xp = 0;}
	n = select(max, rp, wp, xp, &zero);
	if (n > 0) {
	    /* write back fds */
	    if (read) {*read = r;}
	    if (write) {*write = w;}
	    if (except) {*except = x;}
	    return n;
	}
	if (n < 0 && errno != EINTR) {
	    return n;
	}
	if (timeout) {
	    if (timeout->tv_sec == 0 && timeout->tv_usec == 0) return 0;
	    if (limit <= timeofday()) return 0;
	}

        rb_thread_schedule();
	CHECK_INTS;
    }
}

static VALUE
rb_thread_join(thread)
    VALUE thread;
{
    thread_t th = rb_thread_check(thread);

    if (rb_thread_dead(th)) return thread;
    if ((th->wait_for & WAIT_JOIN) && th->join == curr_thread)
	rb_raise(rb_eThreadError, "Thread.join: deadlock");
    curr_thread->status = THREAD_STOPPED;
    curr_thread->join = th;
    num_waiting_on_join++;
    curr_thread->wait_for |= WAIT_JOIN;
    rb_thread_schedule();

    return thread;
}

static VALUE
rb_thread_s_join(dmy, thread)	/* will be removed in 1.2 */
    VALUE dmy;
    VALUE thread;
{
    rb_warn("Thread.join is obsolete; use Thread#join instead");
    return rb_thread_join(thread);
}

static VALUE
rb_thread_current()
{
    return curr_thread->thread;
}

static VALUE
rb_thread_main()
{
    return main_thread->thread;
}

static VALUE
rb_thread_wakeup(thread)
    VALUE thread;
{
    thread_t th = rb_thread_check(thread);

    if (th->status == THREAD_KILLED)
	rb_raise(rb_eThreadError, "killed thread");
    rb_thread_ready(th);

    return thread;
}

static VALUE
rb_thread_run(thread)
    VALUE thread;
{
    rb_thread_wakeup(thread);
    if (!rb_thread_critical) rb_thread_schedule();

    return thread;
}

static VALUE
rb_thread_kill(thread)
    VALUE thread;
{
    thread_t th = rb_thread_check(thread);

    if (th->status == THREAD_TO_KILL || th->status == THREAD_KILLED)
	return thread; 
    if (th == th->next || th == main_thread) rb_exit(0);

    rb_thread_ready(th);
    th->status = THREAD_TO_KILL;
    rb_thread_schedule();
    return Qnil;		/* not reached */
}

static VALUE
rb_thread_s_kill(obj, th)
    VALUE obj, th;
{
    return rb_thread_kill(th);
}

static VALUE
rb_thread_exit()
{
    return rb_thread_kill(curr_thread->thread);
}

static VALUE
rb_thread_pass()
{
    rb_thread_schedule();
    return Qnil;
}

static VALUE
rb_thread_stop()
{
    rb_thread_critical = 0;
    curr_thread->status = THREAD_STOPPED;
    if (curr_thread == curr_thread->next) {
	rb_raise(rb_eThreadError, "stopping only thread");
    }
    rb_thread_schedule();

    return Qnil;
}

struct timeval rb_time_timeval();

void
rb_thread_sleep(sec)
    int sec;
{
    if (curr_thread == curr_thread->next) {
	TRAP_BEG;
	sleep(sec);
	TRAP_END;
	return;
    }
    rb_thread_wait_for(rb_time_timeval(INT2FIX(sec)));
}

void
rb_thread_sleep_forever()
{
    if (curr_thread == curr_thread->next) {
	TRAP_BEG;
	sleep((32767L<<16)+32767);
	TRAP_END;
	return;
    }

    num_waiting_on_timer++;
    curr_thread->delay = DELAY_INFTY;
    curr_thread->wait_for |= WAIT_TIME;
    curr_thread->status = THREAD_STOPPED;
    rb_thread_schedule();
}

static int rb_thread_abort;

static VALUE
rb_thread_s_abort_exc()
{
    return rb_thread_abort?Qtrue:Qfalse;
}

static VALUE
rb_thread_s_abort_exc_set(self, val)
    VALUE self, val;
{
    rb_thread_abort = RTEST(val);
    return val;
}

static VALUE
rb_thread_abort_exc(thread)
    VALUE thread;
{
    thread_t th = rb_thread_check(thread);

    return th->abort?Qtrue:Qfalse;
}

static VALUE
rb_thread_abort_exc_set(thread, val)
    VALUE thread, val;
{
    thread_t th = rb_thread_check(thread);

    th->abort = RTEST(val);
    return val;
}

static thread_t
rb_thread_alloc(klass)
    VALUE klass;
{
    thread_t th;

    th = ALLOC(struct thread);
    th->status = THREAD_RUNNABLE;

    th->status = 0;
    th->result = 0;
    th->rb_errinfo = Qnil;

    th->stk_ptr = 0;
    th->stk_len = 0;
    th->stk_max = 0;
    th->wait_for = 0;
    th->fd = 0;
    th->delay = 0.0;
    th->join = 0;

    th->frame = 0;
    th->scope = 0;
    th->klass = 0;
    th->dyna_vars = 0;
    th->block = 0;
    th->iter = 0;
    th->tag = 0;
    th->rb_errinfo = 0;
    th->last_status = 0;
    th->last_line = 0;
    th->last_match = 0;
    th->abort = 0;

    th->thread = Data_Wrap_Struct(klass, thread_mark, thread_free, th);

    if (curr_thread) {
	th->prev = curr_thread;
	curr_thread->next->prev = th;
	th->next = curr_thread->next;
	curr_thread->next = th;
    }
    else {
	curr_thread = th->prev = th->next = th;
	th->status = THREAD_RUNNABLE;
    }

    return th;
}

#if defined(HAVE_SETITIMER) && !defined(__BOW__)
static void
catch_timer(sig)
    int sig;
{
#if !defined(POSIX_SIGNAL) && !defined(BSD_SIGNAL)
    signal(sig, catch_timer);
#endif
    if (!rb_thread_critical) {
	if (rb_trap_immediate) {
	    rb_thread_schedule();
	}
	else rb_thread_pending = 1;
    }
}
#else
int thread_tick = rb_THREAD_TICK;
#endif

static VALUE rb_thread_raise _((int, VALUE*, VALUE));

#define SCOPE_SHARED  FL_USER1

static VALUE
rb_thread_create_0(fn, arg, klass)
    VALUE (*fn)();
    void *arg;
    VALUE klass;
{
    thread_t th = rb_thread_alloc(klass);
    enum thread_status status;
    int state;

#if defined(HAVE_SETITIMER) && !defined(__BOW__)
    static init = 0;

    if (!init) {
	struct itimerval tval;

#ifdef POSIX_SIGNAL
	posix_signal(SIGVTALRM, catch_timer);
	posix_signal(SIGALRM, catch_timer);
#else
	signal(SIGVTALRM, catch_timer);
	signal(SIGALRM, catch_timer);
#endif

	tval.it_interval.tv_sec = 0;
	tval.it_interval.tv_usec = 100000;
	tval.it_value = tval.it_interval;
	setitimer(ITIMER_VIRTUAL, &tval, NULL);
	init = 1;
    }
#endif

    FL_SET(ruby_scope, SCOPE_SHARED);
    rb_thread_save_context(curr_thread);
    if (setjmp(curr_thread->context)) {
	return th->thread;
    }

    PUSH_TAG(PROT_THREAD);
    if ((state = EXEC_TAG()) == 0) {
	rb_thread_save_context(th);
	if (setjmp(th->context) == 0) {
	    curr_thread = th;
	    th->result = (*fn)(arg, th);
	}
    }
    POP_TAG();
    status = th->status;
    rb_thread_remove();
    if (state && status != THREAD_TO_KILL && !NIL_P(rb_errinfo)) {
	if (state == TAG_FATAL) { 
	    /* fatal error within this thread, need to stop whole script */
	    main_thread->rb_errinfo = rb_errinfo;
	    rb_thread_cleanup();
	}
	else if (rb_obj_is_kind_of(rb_errinfo, rb_eSystemExit)) {
	    /* delegate exception to main_thread */
	    rb_thread_raise(1, &rb_errinfo, main_thread->thread);
	}
	else if (rb_thread_abort || curr_thread->abort || RTEST(rb_debug)) {
	    VALUE err = rb_exc_new(rb_eSystemExit, 0, 0);
	    error_print();
	    /* exit on main_thread */
	    rb_thread_raise(1, &err, main_thread->thread);
	}
	else {
	    curr_thread->rb_errinfo = rb_errinfo;
	}
    }
    rb_thread_schedule();
    return 0;			/* not reached */
}

VALUE
rb_thread_create(fn, arg)
    VALUE (*fn)();
    void *arg;
{
    return rb_thread_create_0(fn, arg, rb_cThread);
}

int
rb_thread_scope_shared_p()
{
    return FL_TEST(ruby_scope, SCOPE_SHARED);
}

static VALUE
rb_thread_yield(arg, th) 
    int arg;
    thread_t th;
{
    scope_dup(ruby_block->scope);
    return rb_yield_0(th->thread, 0, 0);
}

static VALUE
rb_thread_start(klass)
    VALUE klass;
{
    if (!rb_iterator_p()) {
	rb_raise(rb_eThreadError, "must be called as iterator");
    }
    return rb_thread_create_0(rb_thread_yield, 0, klass);
}

static VALUE
rb_thread_value(thread)
    VALUE thread;
{
    thread_t th = rb_thread_check(thread);

    rb_thread_join(thread);
    if (!NIL_P(th->rb_errinfo)) {
	VALUE oldbt = get_backtrace(th->rb_errinfo);
	VALUE errat = make_backtrace();

	rb_ary_unshift(errat, rb_ary_entry(oldbt, 0));
	set_backtrace(th->rb_errinfo, errat);
	rb_exc_raise(th->rb_errinfo);
    }

    return th->result;
}

static VALUE
rb_thread_status(thread)
    VALUE thread;
{
    thread_t th = rb_thread_check(thread);

    if (rb_thread_dead(th)) {
	if (NIL_P(th->rb_errinfo)) return Qfalse;
	return Qnil;
    }

    return Qtrue;
}

static VALUE
rb_thread_stop_p(thread)
    VALUE thread;
{
    thread_t th = rb_thread_check(thread);

    if (rb_thread_dead(th)) return Qtrue;
    if (th->status == THREAD_STOPPED) return Qtrue;
    return Qfalse;
}

static void
rb_thread_wait_other_threads()
{
    /* wait other threads to terminate */
    while (curr_thread != curr_thread->next) {
	rb_thread_schedule();
    }
}

static void
rb_thread_cleanup()
{
    thread_t th;

    if (curr_thread != curr_thread->next->prev) {
	curr_thread = curr_thread->prev;
    }

    FOREACH_THREAD(th) {
	if (th != curr_thread && th->status != THREAD_KILLED) {
	    th->status = THREAD_TO_KILL;
	    th->wait_for = 0;
	}
    }
    END_FOREACH(th);
}

int rb_thread_critical;

static VALUE
rb_thread_get_critical()
{
    return rb_thread_critical?Qtrue:Qfalse;
}

static VALUE
rb_thread_set_critical(obj, val)
    VALUE obj, val;
{
    rb_thread_critical = RTEST(val);
    return val;
}

void
rb_thread_interrupt()
{
    rb_thread_critical = 0;
    rb_thread_ready(main_thread);
    if (curr_thread == main_thread) {
	rb_interrupt();
    }
    rb_thread_save_context(curr_thread);
    if (setjmp(curr_thread->context)) {
	return;
    }
    curr_thread = main_thread;
    rb_thread_restore_context(curr_thread, 2);
}

void
rb_thread_trap_eval(cmd, sig)
    VALUE cmd;
    int sig;
{
    rb_thread_critical = 0;
    if (!rb_thread_dead(curr_thread)) {
	rb_thread_ready(curr_thread);
	rb_trap_eval(cmd, sig);
	return;
    }
    rb_thread_ready(main_thread);
    rb_thread_save_context(curr_thread);
    if (setjmp(curr_thread->context)) {
	return;
    }
    th_cmd = cmd;
    th_sig = sig;
    curr_thread = main_thread;
    rb_thread_restore_context(curr_thread, 3);
}

static VALUE
rb_thread_raise(argc, argv, thread)
    int argc;
    VALUE *argv;
    VALUE thread;
{
    thread_t th = rb_thread_check(thread);

    if (rb_thread_dead(th)) return thread;
    if (curr_thread == th) {
	rb_f_raise(argc, argv);
    }

    if (curr_thread->status != THREAD_KILLED)
	rb_thread_save_context(curr_thread);
    if (setjmp(curr_thread->context)) {
	return thread;
    }

    rb_scan_args(argc, argv, "11", &th_raise_argv[0], &th_raise_argv[1]);
    rb_thread_ready(th);
    curr_thread = th;

    th_raise_argc = argc;
    th_raise_file = ruby_sourcefile;
    th_raise_line = ruby_sourceline;
    rb_thread_restore_context(curr_thread, 4);
    return Qnil;		/* not reached */
}

static thread_t loading_thread;
static int loading_nest;

static int
rb_thread_loading(feature)
    char *feature;
{
    if (curr_thread != curr_thread->next && loading_thread) {
	while (loading_thread != curr_thread) {
	    rb_thread_schedule();
	    CHECK_INTS;
	}
	if (rb_provided(feature)) return Qtrue; /* no need to load */
    }

    loading_thread = curr_thread;
    loading_nest++;

    return Qfalse;
}

static void
rb_thread_loading_done()
{
    if (--loading_nest == 0) {
	loading_thread = 0;
    }
}

void
Init_Thread()
{
    rb_eThreadError = rb_define_class("ThreadError", rb_eStandardError);
    rb_cThread = rb_define_class("Thread", rb_cObject);

    rb_define_singleton_method(rb_cThread, "new", rb_thread_start, 0);
    rb_define_singleton_method(rb_cThread, "start", rb_thread_start, 0);
    rb_define_singleton_method(rb_cThread, "fork", rb_thread_start, 0);

    rb_define_singleton_method(rb_cThread, "stop", rb_thread_stop, 0);
    rb_define_singleton_method(rb_cThread, "kill", rb_thread_s_kill, 1);
    rb_define_singleton_method(rb_cThread, "exit", rb_thread_exit, 0);
    rb_define_singleton_method(rb_cThread, "pass", rb_thread_pass, 0);
    rb_define_singleton_method(rb_cThread, "join", rb_thread_s_join, 1);
    rb_define_singleton_method(rb_cThread, "current", rb_thread_current, 0);
    rb_define_singleton_method(rb_cThread, "main", rb_thread_main, 0);

    rb_define_singleton_method(rb_cThread, "critical", rb_thread_get_critical, 0);
    rb_define_singleton_method(rb_cThread, "critical=", rb_thread_set_critical, 1);

    rb_define_singleton_method(rb_cThread, "abort_on_exception", rb_thread_s_abort_exc, 0);
    rb_define_singleton_method(rb_cThread, "abort_on_exception=", rb_thread_s_abort_exc_set, 1);

    rb_define_method(rb_cThread, "run", rb_thread_run, 0);
    rb_define_method(rb_cThread, "wakeup", rb_thread_wakeup, 0);
    rb_define_method(rb_cThread, "exit", rb_thread_kill, 0);
    rb_define_method(rb_cThread, "value", rb_thread_value, 0);
    rb_define_method(rb_cThread, "status", rb_thread_status, 0);
    rb_define_method(rb_cThread, "join", rb_thread_join, 0);
    rb_define_method(rb_cThread, "alive?", rb_thread_status, 0);
    rb_define_method(rb_cThread, "stop?", rb_thread_stop_p, 0);
    rb_define_method(rb_cThread, "raise", rb_thread_raise, -1);

    rb_define_method(rb_cThread, "abort_on_exception", rb_thread_abort_exc, 0);
    rb_define_method(rb_cThread, "abort_on_exception=", rb_thread_abort_exc_set, 1);

    /* allocate main thread */
    main_thread = rb_thread_alloc(rb_cThread);
}
#endif

static VALUE
rb_f_catch(dmy, tag)
    VALUE dmy, tag;
{
    int state;
    ID t;
    VALUE val;			/* OK */

    t = rb_to_id(tag);
    PUSH_TAG(t);
    if ((state = EXEC_TAG()) == 0) {
	val = rb_yield_0(tag, 0, 0);
    }
    else if (state == TAG_THROW && t == prot_tag->dst) {
	val = prot_tag->retval;
	state = 0;
    }
    POP_TAG();
    if (state) JUMP_TAG(state);

    return val;
}

static VALUE
catch_i(tag)
    ID tag;
{
    return rb_f_catch(0, FIX2INT(tag));
}

VALUE
rb_catch(tag, proc, data)
    char *tag;
    VALUE (*proc)();
    VALUE data;
{
    return rb_iterate(catch_i, rb_intern(tag), proc, data);
}

static VALUE
rb_f_throw(argc, argv)
    int argc;
    VALUE *argv;
{
    VALUE tag, value;
    ID t;
    struct tag *tt = prot_tag;

    rb_scan_args(argc, argv, "11", &tag, &value);
    t = rb_to_id(tag);

    while (tt) {
	if (tt->tag == t) {
	    tt->dst = t;
	    break;
	}
#ifdef USE_THREAD
	if (tt->tag == PROT_THREAD) {
	    rb_raise(rb_eThreadError, "uncaught throw `%s' in thread 0x%x",
		     rb_id2name(t),
		     curr_thread);
	}
#endif
	tt = tt->prev;
    }
    if (!tt) {
	rb_raise(rb_eNameError, "uncaught throw `%s'", rb_id2name(t));
    }
    return_value(value);
    rb_trap_restore_mask();
    JUMP_TAG(TAG_THROW);
    /* not reached */
}

void
rb_throw(tag, val)
    char *tag;
    VALUE val;
{
    VALUE argv[2];
    ID t = rb_intern(tag);

    argv[0] = FIX2INT(t);
    argv[1] = val;
    rb_f_throw(2, argv);
}

static void
return_check()
{
#ifdef USE_THREAD
    struct tag *tt = prot_tag;

    while (tt) {
	if (tt->tag == PROT_FUNC) {
	    break;
	}
	if (tt->tag == PROT_THREAD) {
	    rb_raise(rb_eThreadError, "return from within thread 0x%x",
		     curr_thread);
	}
	tt = tt->prev;
    }
#endif
}

