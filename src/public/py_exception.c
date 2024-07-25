#include "pocketpy/objects/codeobject.h"
#include "pocketpy/objects/error.h"
#include "pocketpy/pocketpy.h"

#include "pocketpy/common/utils.h"
#include "pocketpy/objects/object.h"
#include "pocketpy/interpreter/vm.h"
#include "pocketpy/common/sstream.h"

typedef struct BaseExceptionFrame {
    pk_SourceData_ src;
    int lineno;
    c11_string* name;
} BaseExceptionFrame;

typedef struct BaseException {
    const Bytecode* ip_backup;
    const CodeObject* code_backup;
    c11_vector /*T=BaseExceptionFrame*/ stacktrace;
} BaseException;

void py_BaseException__record(py_Ref self, const Bytecode* ip, const CodeObject* code) {
    BaseException* ud = py_touserdata(self);
    ud->ip_backup = ip;
    ud->code_backup = code;
}

void py_BaseException__stpush(py_Ref self, pk_SourceData_ src, int lineno, const char *func_name){
    BaseException* ud = py_touserdata(self);
    if(ud->stacktrace.count >= 7) return;
    BaseExceptionFrame* frame = c11_vector__emplace(&ud->stacktrace);
    PK_INCREF(src);
    frame->src = src;
    frame->lineno = lineno;
    frame->name = func_name ? c11_string__new(func_name) : NULL;
}

static void BaseException__dtor(void* ud) {
    BaseException* self = (BaseException*)ud;
    c11__foreach(BaseExceptionFrame, &self->stacktrace, it) {
        PK_DECREF(it->src);
        if(it->name) c11_string__delete(it->name);
    }
    c11_vector__dtor(&self->stacktrace);
}

static bool _py_BaseException__new__(int argc, py_Ref argv) {
    py_Type cls = py_totype(argv);
    BaseException* ud = py_newobject(py_retval(), cls, 1, sizeof(BaseException));
    c11_vector__ctor(&ud->stacktrace, sizeof(BaseExceptionFrame));
    ud->ip_backup = NULL;
    ud->code_backup = NULL;
    return true;
}

static bool _py_BaseException__init__(int argc, py_Ref argv) {
    if(argc == 1 + 0) { return true; }
    if(argc == 1 + 1) {
        py_setslot(py_arg(0), 0, py_arg(1));
        return true;
    }
    return TypeError("__init__() takes at most 2 arguments but %d were given", argc);
}

static bool _py_BaseException__repr__(int argc, py_Ref argv) {
    c11_sbuf ss;
    c11_sbuf__ctor(&ss);
    pk_sprintf(&ss, "%t(", argv->type);
    py_Ref arg = py_getslot(argv, 0);
    if(!py_isnil(arg)) {
        if(!py_repr(arg)) return false;
        c11_sbuf__write_sv(&ss, py_tosv(py_retval()));
    }
    c11_sbuf__write_char(&ss, ')');
    c11_string* res = c11_sbuf__submit(&ss);
    py_newstrn(py_retval(), res->data, res->size);
    c11_string__delete(res);
    return true;
}

static bool _py_BaseException__str__(int argc, py_Ref argv) {
    c11_sbuf ss;
    c11_sbuf__ctor(&ss);
    py_Ref arg = py_getslot(argv, 0);
    if(!py_isnil(arg)) {
        if(!py_str(arg)) return false;
        c11_sbuf__write_sv(&ss, py_tosv(py_retval()));
    }
    c11_string* res = c11_sbuf__submit(&ss);
    py_newstrn(py_retval(), res->data, res->size);
    c11_string__delete(res);
    return true;
}

py_Type pk_BaseException__register() {
    py_Type type = pk_newtype("BaseException", tp_object, NULL, BaseException__dtor, false, false);

    py_bindmagic(type, __new__, _py_BaseException__new__);
    py_bindmagic(type, __init__, _py_BaseException__init__);
    py_bindmagic(type, __repr__, _py_BaseException__repr__);
    py_bindmagic(type, __str__, _py_BaseException__str__);
    return type;
}

py_Type pk_Exception__register() {
    py_Type type = pk_newtype("Exception", tp_BaseException, NULL, NULL, false, true);
    return type;
}