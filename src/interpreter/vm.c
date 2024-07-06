#include "pocketpy/interpreter/vm.h"
#include "pocketpy/common/memorypool.h"
#include "pocketpy/common/sstream.h"
#include "pocketpy/common/utils.h"
#include "pocketpy/objects/base.h"
#include "pocketpy/pocketpy.h"

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>

static unsigned char* pk_default_import_file(const char* path) { return NULL; }

static void pk_default_stdout(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    fflush(stdout);
}

static void pk_default_stderr(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fflush(stderr);
}

void pk_TypeInfo__ctor(pk_TypeInfo* self,
                       py_Name name,
                       py_Type index,
                       py_Type base,
                       const py_TValue* module,
                       bool subclass_enabled) {
    memset(self, 0, sizeof(pk_TypeInfo));

    self->name = name;
    self->base = base;

    // create type object with __dict__
    pk_ManagedHeap* heap = &pk_current_vm->heap;
    PyObject* typeobj = pk_ManagedHeap__new(heap, tp_type, -1, sizeof(py_Type));
    *(py_Type*)PyObject__userdata(typeobj) = index;
    self->self = (py_TValue){
        .type = typeobj->type,
        .is_ptr = true,
        ._obj = typeobj,
    };

    self->module = module ? *module : PY_NIL;
    self->subclass_enabled = subclass_enabled;

    c11_vector__ctor(&self->annotated_fields, sizeof(py_Name));
}

void pk_TypeInfo__dtor(pk_TypeInfo* self) { c11_vector__dtor(&self->annotated_fields); }

void pk_VM__ctor(pk_VM* self) {
    self->top_frame = NULL;

    pk_NameDict__ctor(&self->modules);
    c11_vector__ctor(&self->types, sizeof(pk_TypeInfo));

    self->StopIteration = PY_NIL;
    self->builtins = PY_NIL;
    self->main = PY_NIL;

    self->_ceval_on_step = NULL;
    self->_import_file = pk_default_import_file;
    self->_stdout = pk_default_stdout;
    self->_stderr = pk_default_stderr;

    self->last_retval = PY_NIL;
    self->has_error = false;

    self->__curr_class = PY_NIL;
    self->__dynamic_func_decl = NULL;

    pk_ManagedHeap__ctor(&self->heap, self);
    ValueStack__ctor(&self->stack);

    /* Init Builtin Types */
    // 0: unused
    pk_TypeInfo__ctor(c11_vector__emplace(&self->types), 0, 0, 0, NULL, false);
#define validate(t, expr)                                                                          \
    if(t != (expr)) abort()

    validate(tp_object, pk_VM__new_type(self, "object", 0, NULL, true));
    validate(tp_type, pk_VM__new_type(self, "type", 1, NULL, false));
    pk_object__register();

    validate(tp_int, pk_VM__new_type(self, "int", tp_object, NULL, false));
    validate(tp_float, pk_VM__new_type(self, "float", tp_object, NULL, false));
    validate(tp_bool, pk_VM__new_type(self, "bool", tp_object, NULL, false));
    pk_number__register();

    validate(tp_str, pk_str__register());

    validate(tp_list, pk_list__register());
    validate(tp_tuple, pk_VM__new_type(self, "tuple", tp_object, NULL, false));

    validate(tp_slice, pk_VM__new_type(self, "slice", tp_object, NULL, false));
    validate(tp_range, pk_VM__new_type(self, "range", tp_object, NULL, false));
    validate(tp_module, pk_VM__new_type(self, "module", tp_object, NULL, false));

    validate(tp_function, pk_function__register());
    validate(tp_nativefunc, pk_nativefunc__register());
    validate(tp_bound_method, pk_VM__new_type(self, "bound_method", tp_object, NULL, false));

    validate(tp_super, pk_VM__new_type(self, "super", tp_object, NULL, false));
    validate(tp_exception, pk_VM__new_type(self, "Exception", tp_object, NULL, true));
    validate(tp_bytes, pk_bytes__register());
    validate(tp_mappingproxy, pk_VM__new_type(self, "mappingproxy", tp_object, NULL, false));

    validate(tp_dict, pk_VM__new_type(self, "dict", tp_object, NULL, true));
    validate(tp_property, pk_VM__new_type(self, "property", tp_object, NULL, false));
    validate(tp_star_wrapper, pk_VM__new_type(self, "star_wrapper", tp_object, NULL, false));

    validate(tp_staticmethod, pk_VM__new_type(self, "staticmethod", tp_object, NULL, false));
    validate(tp_classmethod, pk_VM__new_type(self, "classmethod", tp_object, NULL, false));

    validate(tp_none_type, pk_VM__new_type(self, "NoneType", tp_object, NULL, false));
    validate(tp_not_implemented_type,
             pk_VM__new_type(self, "NotImplementedType", tp_object, NULL, false));
    validate(tp_ellipsis, pk_VM__new_type(self, "ellipsis", tp_object, NULL, false));

    validate(tp_syntax_error, pk_VM__new_type(self, "SyntaxError", tp_exception, NULL, false));
    validate(tp_stop_iteration, pk_VM__new_type(self, "StopIteration", tp_exception, NULL, false));
#undef validate

    self->StopIteration = *py_tpobject(tp_stop_iteration);
    self->builtins = pk_builtins__register();

    /* Setup Public Builtin Types */
    py_Type public_types[] = {tp_object,
                              tp_type,
                              tp_int,
                              tp_float,
                              tp_bool,
                              tp_str,
                              tp_list,
                              tp_tuple,
                              tp_slice,
                              tp_range,
                              tp_bytes,
                              tp_dict,
                              tp_property,
                              tp_exception,
                              tp_stop_iteration,
                              tp_syntax_error};

    for(int i = 0; i < c11__count_array(public_types); i++) {
        py_Type t = public_types[i];
        pk_TypeInfo* ti = c11__at(pk_TypeInfo, &self->types, t);
        py_setdict(&self->builtins, ti->name, py_tpobject(t));
    }

    py_TValue tmp;
    py_newnotimplemented(&tmp);
    py_setdict(&self->builtins, py_name("NotImplemented"), &tmp);

    self->main = *py_newmodule("__main__", NULL);
}

void pk_VM__dtor(pk_VM* self) {
    if(self->__dynamic_func_decl) { PK_DECREF(self->__dynamic_func_decl); }
    // destroy all objects
    pk_ManagedHeap__dtor(&self->heap);
    // clear frames
    // ...
    pk_NameDict__dtor(&self->modules);
    c11_vector__dtor(&self->types);
    ValueStack__clear(&self->stack);
}

void pk_VM__push_frame(pk_VM* self, Frame* frame) {
    frame->f_back = self->top_frame;
    self->top_frame = frame;
}

void pk_VM__pop_frame(pk_VM* self) {
    assert(self->top_frame);
    Frame* frame = self->top_frame;
    // reset stack pointer
    self->stack.sp = frame->p0;
    // pop frame and delete
    self->top_frame = frame->f_back;
    Frame__delete(frame);
}

static void _clip_int(int* value, int min, int max) {
    if(*value < min) *value = min;
    if(*value > max) *value = max;
}

bool pk__parse_int_slice(const py_Ref slice, int length, int* start, int* stop, int* step) {
    py_Ref s_start = py_getslot(slice, 0);
    py_Ref s_stop = py_getslot(slice, 1);
    py_Ref s_step = py_getslot(slice, 2);

    if(py_isnone(s_step))
        *step = 1;
    else {
        if(!py_checkint(s_step)) return false;
        *step = py_toint(s_step);
    }
    if(*step == 0) return ValueError("slice step cannot be zero");

    if(*step > 0) {
        if(py_isnone(s_start))
            *start = 0;
        else {
            if(!py_checkint(s_start)) return false;
            *start = py_toint(s_start);
            if(*start < 0) *start += length;
            _clip_int(start, 0, length);
        }
        if(py_isnone(s_stop))
            *stop = length;
        else {
            if(!py_checkint(s_stop)) return false;
            *stop = py_toint(s_stop);
            if(*stop < 0) *stop += length;
            _clip_int(stop, 0, length);
        }
    } else {
        if(py_isnone(s_start))
            *start = length - 1;
        else {
            if(!py_checkint(s_start)) return false;
            *start = py_toint(s_start);
            if(*start < 0) *start += length;
            _clip_int(start, -1, length - 1);
        }
        if(py_isnone(s_stop))
            *stop = -1;
        else {
            if(!py_checkint(s_stop)) return false;
            *stop = py_toint(s_stop);
            if(*stop < 0) *stop += length;
            _clip_int(stop, -1, length - 1);
        }
    }
    return true;
}

bool pk__normalize_index(int *index, int length){
    if(*index < 0) *index += length;
    if(*index < 0 || *index >= length){
        return IndexError("index out of range");
    }
    return true;
}

py_Type pk_VM__new_type(pk_VM* self,
                        const char* name,
                        py_Type base,
                        const py_TValue* module,
                        bool subclass_enabled) {
    py_Type index = self->types.count;
    pk_TypeInfo* ti = c11_vector__emplace(&self->types);
    pk_TypeInfo__ctor(ti, py_name(name), index, base, module, subclass_enabled);
    return index;
}

pk_FrameResult pk_VM__vectorcall(pk_VM* self, uint16_t argc, uint16_t kwargc, bool opcall) {
    py_Ref p1 = self->stack.sp - kwargc * 2;
    py_Ref p0 = p1 - argc - 2;
    // [callable, <self>, args..., kwargs...]
    //      ^p0                    ^p1      ^_sp

    // handle boundmethod, do a patch
    if(p0->type == tp_bound_method) {
        assert(false);
        assert(py_isnil(p0 + 1));  // self must be NULL
        // BoundMethod& bm = PK_OBJ_GET(BoundMethod, callable);
        // callable = bm.func;  // get unbound method
        // callable_t = _tp(callable);
        // p1[-(ARGC + 2)] = bm.func;
        // p1[-(ARGC + 1)] = bm.self;
        // [unbound, self, args..., kwargs...]
    }

    py_Ref argv = py_isnil(p0 + 1) ? p0 + 2 : p0 + 1;
    int argc2 = argv - p0;

    if(p0->type == tp_function) {
        /*****************_py_call*****************/
        // check stack overflow
        if(self->stack.sp > self->stack.end) {
            py_exception("StackOverflowError", "");
            return RES_ERROR;
        }

        Function* fn = py_touserdata(p0);
        const CodeObject* co = &fn->decl->code;

        switch(fn->decl->type) {
            case FuncType_NORMAL:
                assert(false);
                // __prepare_py_call(__vectorcall_buffer, args, kwargs, fn.decl);
                // // copy buffer back to stack
                // self->stack.sp = argv + co->nlocals;
                // for(int j = 0; j < co->nlocals; j++)
                //     argv[j] = self->__vectorcall_buffer[j];
                break;
            case FuncType_SIMPLE:
                if(argc2 != fn->decl->args.count) {
                    const char* fmt = "%s() takes %d positional arguments but %d were given";
                    TypeError(fmt, co->name, fn->decl->args.count, argc2);
                    return RES_ERROR;
                }
                if(kwargc) {
                    TypeError("%s() takes no keyword arguments", co->name->data);
                    return RES_ERROR;
                }
                // [callable, <self>, args..., local_vars...]
                //      ^p0                    ^p1      ^_sp
                self->stack.sp = argv + co->nlocals;
                // initialize local variables to PY_NIL
                memset(p1, 0, (char*)self->stack.sp - (char*)p1);
                break;
            case FuncType_GENERATOR:
                assert(false);
                break;
                // __prepare_py_call(__vectorcall_buffer, args, kwargs, fn.decl);
                // s_data.reset(p0);
                // callstack.emplace(nullptr, co, fn._module, callable.get(), nullptr);
                // return __py_generator(
                //     callstack.popx(),
                //     ArgsView(__vectorcall_buffer, __vectorcall_buffer + co->nlocals));
            default: c11__unreachedable();
        };

        // simple or normal
        Frame* frame = Frame__new(co, fn->module, p0, p0, argv, co);
        pk_VM__push_frame(self, frame);
        if(opcall) return RES_CALL;
        return pk_VM__run_top_frame(self);
        /*****************_py_call*****************/
    }

    if(p0->type == tp_nativefunc) {
        // const auto& f = PK_OBJ_GET(NativeFunc, callable);
        // PyVar ret;
        // if(f.decl != nullptr) {
        //     int co_nlocals = f.decl->code->nlocals;
        //     __prepare_py_call(__vectorcall_buffer, args, kwargs, f.decl);
        //     // copy buffer back to stack
        //     s_data.reset(_base + co_nlocals);
        //     for(int j = 0; j < co_nlocals; j++)
        //         _base[j] = __vectorcall_buffer[j];
        //     ret = f.call(vm, ArgsView(s_data._sp - co_nlocals, s_data._sp));
        // } else {
        //     if(f.argc != -1) {
        //         if(KWARGC != 0)
        //             TypeError(
        //                 "old-style native_func does not accept keyword arguments. If you want to
        //                 skip this check, specify `argc` to -1");
        //         if(args.size() != f.argc) {
        //             vm->TypeError(_S("expected ", f.argc, " arguments, got ", args.size()));
        //         }
        //     }
        //     ret = f.call(this, args);
        // }

        // `argc` passed to _cfunc must include self if exists
        if(!p0->_cfunc(p1 - argv, argv)) return RES_ERROR;
        self->stack.sp = p0;
        return RES_RETURN;
    }

    if(p0->type == tp_type) {
        // [cls, NULL, args..., kwargs...]
        py_Ref new_f = py_tpfindmagic(py_totype(p0), __new__);
        assert(new_f && py_isnil(p0 + 1));

        // prepare a copy of args and kwargs
        int span = self->stack.sp - argv;
        *self->stack.sp++ = *new_f;  // push __new__
        *self->stack.sp++ = *p0;     // push cls
        memcpy(self->stack.sp, argv, span * sizeof(py_TValue));
        self->stack.sp += span;

        // [new_f, cls, args..., kwargs...]
        pk_FrameResult res = pk_VM__vectorcall(self, argc, kwargc, false);
        if(res == RES_ERROR) return RES_ERROR;
        assert(res == RES_RETURN);
        // by recursively using vectorcall, args and kwargs are consumed
        // [cls, NULL, args..., kwargs...]

        // try __init__
        // NOTE: previous we use `get_unbound_method` but here we just use `tpfindmagic`
        py_Ref init_f = py_tpfindmagic(py_totype(p0), __init__);
        if(init_f) {
            // do an inplace patch
            *p0 = *init_f;              // __init__
            p0[1] = self->last_retval;  // self
            // [__init__, self, args..., kwargs...]
            pk_FrameResult res = pk_VM__vectorcall(self, argc, kwargc, false);
            if(res == RES_ERROR) return RES_ERROR;
            assert(res == RES_RETURN);
        } else {
            // manually reset the stack
            self->stack.sp = p0;
        }
        return RES_RETURN;
    }

    // handle `__call__` overload
    if(py_getunboundmethod(p0, __call__, p0, p0 + 1)) {
        // [__call__, self, args..., kwargs...]
        pk_FrameResult res = pk_VM__vectorcall(self, argc, kwargc, false);
        if(res == RES_ERROR) return RES_ERROR;
        assert(res == RES_RETURN);
    }

    TypeError("'%t' object is not callable", p0->type);
    c11__unreachedable();
}

/****************************************/
void PyObject__delete(PyObject* self) {
    pk_TypeInfo* ti = c11__at(pk_TypeInfo, &pk_current_vm->types, self->type);
    if(ti->dtor) ti->dtor(PyObject__userdata(self));
    if(self->slots == -1) pk_NameDict__dtor(PyObject__dict(self));
    if(self->gc_is_large) {
        free(self);
    } else {
        PoolObject_dealloc(self);
    }
}

void pk_ManagedHeap__mark(pk_ManagedHeap* self) {
    // for(int i=0; i<self->no_gc.count; i++){
    //     PyObject* obj = c11__getitem(PyObject*, &self->no_gc, i);
    //     vm->__obj_gc_mark(obj);
    // }
    // vm->callstack.apply([vm](Frame& frame) {
    //     frame._gc_mark(vm);
    // });
    // vm->obj_gc_mark(vm->__last_exception);
    // vm->obj_gc_mark(vm->__curr_class);
    // vm->obj_gc_mark(vm->__c.error);
    // vm->__stack_gc_mark(vm->s_data.begin(), vm->s_data.end());
    // if(self->_gc_marker_ex) self->_gc_marker_ex((pkpy_VM*)vm);
}
