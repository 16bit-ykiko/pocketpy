#include "pocketpy/interpreter/vm.h"
#include "pocketpy/common/memorypool.h"
#include "pocketpy/common/sstream.h"
#include "pocketpy/common/utils.h"
#include "pocketpy/objects/base.h"
#include "pocketpy/common/_generated.h"
#include "pocketpy/pocketpy.h"

static char* pk_default_import_file(const char* path) {
#if PK_ENABLE_OS
    FILE* f = fopen(path, "rb");
    if(f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buffer = malloc(size + 1);
    fread(buffer, 1, size, f);
    buffer[size] = 0;
    fclose(f);
    return buffer;
#else
    return NULL;
#endif
}

static void pk_default_print(const char* data) { printf("%s", data); }

static void py_TypeInfo__ctor(py_TypeInfo* self,
                              py_Name name,
                              py_Type index,
                              py_Type base,
                              py_TValue module) {
    memset(self, 0, sizeof(py_TypeInfo));

    self->name = name;
    self->base = base;

    // create type object with __dict__
    ManagedHeap* heap = &pk_current_vm->heap;
    PyObject* typeobj = ManagedHeap__new(heap, tp_type, -1, sizeof(py_Type));
    *(py_Type*)PyObject__userdata(typeobj) = index;
    self->self = (py_TValue){
        .type = typeobj->type,
        .is_ptr = true,
        ._obj = typeobj,
    };

    self->module = module;
    c11_vector__ctor(&self->annotated_fields, sizeof(py_Name));
}

static void py_TypeInfo__dtor(py_TypeInfo* self) { c11_vector__dtor(&self->annotated_fields); }

void VM__ctor(VM* self) {
    self->top_frame = NULL;

    NameDict__ctor(&self->modules);
    c11_vector__ctor(&self->types, sizeof(py_TypeInfo));

    self->builtins = *py_NIL;
    self->main = *py_NIL;

    self->ceval_on_step = NULL;
    self->import_file = pk_default_import_file;
    self->print = pk_default_print;

    self->last_retval = *py_NIL;
    self->curr_exception = *py_NIL;
    self->is_stopiteration = false;

    self->__curr_class = NULL;
    self->__dynamic_func_decl = NULL;

    ManagedHeap__ctor(&self->heap, self);
    ValueStack__ctor(&self->stack);

    /* Init Builtin Types */
    // 0: unused
    void* placeholder = c11_vector__emplace(&self->types);
    memset(placeholder, 0, sizeof(py_TypeInfo));

#define validate(t, expr)                                                                          \
    if(t != (expr)) abort()

    validate(tp_object, pk_newtype("object", 0, NULL, NULL, true, false));
    validate(tp_type, pk_newtype("type", 1, NULL, NULL, false, true));
    pk_object__register();

    validate(tp_int, pk_newtype("int", tp_object, NULL, NULL, false, true));
    validate(tp_float, pk_newtype("float", tp_object, NULL, NULL, false, true));
    validate(tp_bool, pk_newtype("bool", tp_object, NULL, NULL, false, true));
    pk_number__register();

    validate(tp_str, pk_str__register());
    validate(tp_str_iterator, pk_str_iterator__register());

    validate(tp_list, pk_list__register());
    validate(tp_tuple, pk_tuple__register());
    validate(tp_array_iterator, pk_array_iterator__register());

    validate(tp_slice, pk_slice__register());
    validate(tp_range, pk_range__register());
    validate(tp_range_iterator, pk_range_iterator__register());
    validate(tp_module, pk_newtype("module", tp_object, NULL, NULL, false, true));

    validate(tp_function, pk_function__register());
    validate(tp_nativefunc, pk_nativefunc__register());
    validate(tp_boundmethod, pk_newtype("boundmethod", tp_object, NULL, NULL, false, true));

    validate(tp_super, pk_super__register());
    validate(tp_BaseException, pk_BaseException__register());
    validate(tp_Exception, pk_Exception__register());
    validate(tp_bytes, pk_bytes__register());
    validate(tp_mappingproxy, pk_newtype("mappingproxy", tp_object, NULL, NULL, false, true));

    validate(tp_dict, pk_dict__register());
    validate(tp_dict_items, pk_dict_items__register());

    validate(tp_property, pk_newtype("property", tp_object, NULL, NULL, false, true));
    validate(tp_star_wrapper, pk_newtype("star_wrapper", tp_object, NULL, NULL, false, true));

    validate(tp_staticmethod, pk_newtype("staticmethod", tp_object, NULL, NULL, false, true));
    validate(tp_classmethod, pk_newtype("classmethod", tp_object, NULL, NULL, false, true));

    validate(tp_NoneType, pk_newtype("NoneType", tp_object, NULL, NULL, false, true));
    validate(tp_NotImplementedType,
             pk_newtype("NotImplementedType", tp_object, NULL, NULL, false, true));
    validate(tp_ellipsis, pk_newtype("ellipsis", tp_object, NULL, NULL, false, true));

    validate(tp_SyntaxError, pk_newtype("SyntaxError", tp_Exception, NULL, NULL, false, true));
    validate(tp_StopIteration, pk_newtype("StopIteration", tp_Exception, NULL, NULL, false, true));
#undef validate

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
                              tp_super,
                              tp_BaseException,
                              tp_Exception,
                              tp_StopIteration,
                              tp_SyntaxError};

    for(int i = 0; i < c11__count_array(public_types); i++) {
        py_Type t = public_types[i];
        py_TypeInfo* ti = c11__at(py_TypeInfo, &self->types, t);
        py_setdict(&self->builtins, ti->name, py_tpobject(t));
    }

    // inject some builtin expections
    const char** builtin_exceptions = (const char*[]){
        "StackOverflowError",
        "IOError",
        "OSError",
        "NotImplementedError",
        "TypeError",
        "IndexError",
        "ValueError",
        "RuntimeError",
        "ZeroDivisionError",
        "NameError",
        "UnboundLocalError",
        "AttributeError",
        "ImportError",
        "AssertionError",
        "KeyError",
        NULL,  // sentinel
    };
    const char** it = builtin_exceptions;
    while(*it) {
        py_Type type = pk_newtype(*it, tp_Exception, &self->builtins, NULL, false, true);
        py_setdict(&self->builtins, py_name(*it), py_tpobject(type));
        it++;
    }

    py_TValue tmp;
    py_newnotimplemented(&tmp);
    py_setdict(&self->builtins, py_name("NotImplemented"), &tmp);

    // add modules
    pk__add_module_pkpy();
    pk__add_module_os();
    pk__add_module_math();

    self->main = *py_newmodule("__main__");
}

void VM__dtor(VM* self) {
    if(self->__dynamic_func_decl) { PK_DECREF(self->__dynamic_func_decl); }
    // destroy all objects
    ManagedHeap__dtor(&self->heap);
    // clear frames
    // ...
    NameDict__dtor(&self->modules);
    c11__foreach(py_TypeInfo, &self->types, ti) py_TypeInfo__dtor(ti);
    c11_vector__dtor(&self->types);
    ValueStack__clear(&self->stack);
}

void VM__push_frame(VM* self, Frame* frame) {
    frame->f_back = self->top_frame;
    self->top_frame = frame;
}

void VM__pop_frame(VM* self) {
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

bool pk__parse_int_slice(py_Ref slice, int length, int* start, int* stop, int* step) {
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

bool pk__normalize_index(int* index, int length) {
    if(*index < 0) *index += length;
    if(*index < 0 || *index >= length) { return IndexError("%d not in [0, %d)", *index, length); }
    return true;
}

py_Type pk_newtype(const char* name,
                   py_Type base,
                   const py_GlobalRef module,
                   void (*dtor)(void*),
                   bool is_python,
                   bool is_sealed) {
    c11_vector* types = &pk_current_vm->types;
    py_Type index = types->count;
    py_TypeInfo* ti = c11_vector__emplace(types);
    py_TypeInfo__ctor(ti, py_name(name), index, base, module ? *module : *py_NIL);
    if(!dtor && base) { dtor = c11__at(py_TypeInfo, types, base)->dtor; }
    ti->dtor = dtor;
    ti->is_python = is_python;
    ti->is_sealed = is_sealed;
    return index;
}

py_Type py_newtype(const char* name, py_Type base, const py_GlobalRef module, void (*dtor)(void*)) {
    return pk_newtype(name, base, module, dtor, false, false);
}

static bool
    prepare_py_call(py_TValue* buffer, py_Ref argv, py_Ref p1, int kwargc, const FuncDecl* decl) {
    const CodeObject* co = &decl->code;
    int decl_argc = decl->args.count;

    if(p1 - argv < decl_argc) {
        return TypeError("%s() takes %d positional arguments but %d were given",
                         co->name->data,
                         decl_argc,
                         p1 - argv);
    }

    py_TValue* t = argv;
    // prepare args
    memset(buffer, 0, co->nlocals * sizeof(py_TValue));
    c11__foreach(int, &decl->args, index) buffer[*index] = *t++;
    // prepare kwdefaults
    c11__foreach(FuncDeclKwArg, &decl->kwargs, kv) buffer[kv->index] = kv->value;

    // handle *args
    if(decl->starred_arg != -1) {
        int exceed_argc = p1 - t;
        py_Ref vargs = &buffer[decl->starred_arg];
        py_newtuple(vargs, exceed_argc);
        for(int j = 0; j < exceed_argc; j++) {
            py_tuple__setitem(vargs, j, t++);
        }
    } else {
        // kwdefaults override
        // def f(a, b, c=None)
        // f(1, 2, 3) -> c=3
        c11__foreach(FuncDeclKwArg, &decl->kwargs, kv) {
            if(t >= p1) break;
            buffer[kv->index] = *t++;
        }
        // not able to consume all args
        if(t < p1) return TypeError("too many arguments (%s)", co->name->data);
    }

    if(decl->starred_kwarg != -1) py_newdict(&buffer[decl->starred_kwarg]);

    for(int j = 0; j < kwargc; j++) {
        py_Name key = py_toint(&p1[2 * j]);
        int index = c11_smallmap_n2i__get(&decl->kw_to_index, key, -1);
        // if key is an explicit key, set as local variable
        if(index >= 0) {
            buffer[index] = p1[2 * j + 1];
        } else {
            // otherwise, set as **kwargs if possible
            if(decl->starred_kwarg == -1) {
                return TypeError("'%n' is an invalid keyword argument for %s()",
                                 key,
                                 co->name->data);
            } else {
                // add to **kwargs
                py_Ref tmp = py_pushtmp();
                c11_sv key_sv = py_name2sv(key);
                py_newstrn(tmp, key_sv.data, key_sv.size);
                py_dict__setitem(&buffer[decl->starred_kwarg], tmp, &p1[2 * j + 1]);
                py_pop();
                if(py_checkexc()) return false;
            }
        }
    }
    return true;
}

FrameResult VM__vectorcall(VM* self, uint16_t argc, uint16_t kwargc, bool opcall) {
    pk_print_stack(self, self->top_frame, (Bytecode){0});

    py_Ref p1 = self->stack.sp - kwargc * 2;
    py_Ref p0 = p1 - argc - 2;
    // [callable, <self>, args..., kwargs...]
    //      ^p0                    ^p1      ^_sp

#if 0
    // handle boundmethod, do a patch
    if(p0->type == tp_boundmethod) {
        assert(py_isnil(p0 + 1));  // self must be NULL
        // BoundMethod& bm = PK_OBJ_GET(BoundMethod, callable);
        // callable = bm.func;  // get unbound method
        // callable_t = _tp(callable);
        // p1[-(ARGC + 2)] = bm.func;
        // p1[-(ARGC + 1)] = bm.self;
        // [unbound, self, args..., kwargs...]
    }
#endif

    py_Ref argv = py_isnil(p0 + 1) ? p0 + 2 : p0 + 1;

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
            case FuncType_NORMAL: {
                bool ok = prepare_py_call(self->__vectorcall_buffer, argv, p1, kwargc, fn->decl);
                if(!ok) return RES_ERROR;
                // copy buffer back to stack
                self->stack.sp = argv + co->nlocals;
                memcpy(argv, self->__vectorcall_buffer, co->nlocals * sizeof(py_TValue));
                // submit the call
                if(!fn->cfunc) {
                    VM__push_frame(self, Frame__new(co, &fn->module, p0, p0, argv, co));
                    return opcall ? RES_CALL : VM__run_top_frame(self);
                } else {
                    bool ok = fn->cfunc(co->nlocals, argv);
                    self->stack.sp = p0;
                    return ok ? RES_RETURN : RES_ERROR;
                }
            }
            case FuncType_SIMPLE:
                if(p1 - argv != fn->decl->args.count) {
                    const char* fmt = "%s() takes %d positional arguments but %d were given";
                    TypeError(fmt, co->name->data, fn->decl->args.count, p1 - argv);
                    return RES_ERROR;
                }
                if(kwargc) {
                    TypeError("%s() takes no keyword arguments", co->name->data);
                    return RES_ERROR;
                }
                // [callable, <self>, args..., local_vars...]
                //      ^p0                    ^p1      ^_sp
                self->stack.sp = argv + co->nlocals;
                // initialize local variables to py_NIL
                memset(p1, 0, (char*)self->stack.sp - (char*)p1);
                // submit the call
                VM__push_frame(self, Frame__new(co, &fn->module, p0, p0, argv, co));
                return opcall ? RES_CALL : VM__run_top_frame(self);
            case FuncType_GENERATOR:
                assert(false);
                break;
                // prepare_py_call(__vectorcall_buffer, args, kwargs, fn.decl);
                // s_data.reset(p0);
                // callstack.emplace(nullptr, co, fn._module, callable.get(), nullptr);
                // return __py_generator(
                //     callstack.popx(),
                //     ArgsView(__vectorcall_buffer, __vectorcall_buffer + co->nlocals));
            default: c11__unreachedable();
        };

        c11__unreachedable();
        /*****************_py_call*****************/
    }

    if(p0->type == tp_nativefunc) {
        bool ok = p0->_cfunc(p1 - argv, argv);
        self->stack.sp = p0;
        return ok ? RES_RETURN : RES_ERROR;
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
        if(VM__vectorcall(self, argc, kwargc, false) == RES_ERROR) return RES_ERROR;
        // by recursively using vectorcall, args and kwargs are consumed

        // try __init__
        // NOTE: previously we use `get_unbound_method` but here we just use `tpfindmagic`
        // >> [cls, NULL, args..., kwargs...]
        // >> py_retval() is the new instance
        py_Ref init_f = py_tpfindmagic(py_totype(p0), __init__);
        if(init_f) {
            // do an inplace patch
            *p0 = *init_f;              // __init__
            p0[1] = self->last_retval;  // self
            // [__init__, self, args..., kwargs...]
            if(VM__vectorcall(self, argc, kwargc, false) == RES_ERROR) return RES_ERROR;
            *py_retval() = p0[1];  // restore the new instance
        }
        // reset the stack
        self->stack.sp = p0;
        return RES_RETURN;
    }

    // handle `__call__` overload
    if(pk_pushmethod(p0, __call__)) {
        // [__call__, self, args..., kwargs...]
        return VM__vectorcall(self, argc, kwargc, opcall);
    }

    TypeError("'%t' object is not callable", p0->type);
    return RES_ERROR;
}

/****************************************/
void PyObject__delete(PyObject* self) {
    py_TypeInfo* ti = c11__at(py_TypeInfo, &pk_current_vm->types, self->type);
    if(ti->dtor) ti->dtor(PyObject__userdata(self));
    if(self->slots == -1) NameDict__dtor(PyObject__dict(self));
    if(self->gc_is_large) {
        free(self);
    } else {
        PoolObject_dealloc(self);
    }
}

static void mark_object(PyObject* obj);

static void mark_value(py_TValue* val) {
    if(val->is_ptr) mark_object(val->_obj);
}

static void mark_object(PyObject* obj) {
    if(obj->gc_marked) return;
    obj->gc_marked = true;

    if(obj->slots > 0) {
        py_TValue* p = PyObject__slots(obj);
        for(int i = 0; i < obj->slots; i++)
            mark_value(p + i);
        return;
    }

    if(obj->slots == -1) {
        NameDict* dict = PyObject__dict(obj);
        for(int j = 0; j < dict->count; j++) {
            NameDict_KV* kv = c11__at(NameDict_KV, dict, j);
            mark_value(&kv->value);
        }
        return;
    }

    if(obj->type == tp_list) {
        pk_list__mark(PyObject__userdata(obj), mark_value);
    } else if(obj->type == tp_dict) {
        pk_dict__mark(PyObject__userdata(obj), mark_value);
    }
}

void ManagedHeap__mark(ManagedHeap* self) {
    VM* vm = self->vm;
    // mark heap objects
    for(int i = 0; i < self->no_gc.count; i++) {
        PyObject* obj = c11__getitem(PyObject*, &self->no_gc, i);
        mark_object(obj);
    }
    // mark value stack
    for(py_TValue* p = vm->stack.begin; p != vm->stack.end; p++) {
        mark_value(p);
    }
    // mark frame
    for(Frame* frame = vm->top_frame; frame; frame = frame->f_back) {
        mark_value(&frame->module);
        if(frame->function) mark_object(frame->function);
    }
    // mark vm's registers
    mark_value(&vm->last_retval);
    mark_value(&vm->curr_exception);
    for(int i = 0; i < c11__count_array(vm->reg); i++) {
        mark_value(&vm->reg[i]);
    }
}

void pk_print_stack(VM* self, Frame* frame, Bytecode byte) {
    return;
    if(frame == NULL) return;

    py_TValue* sp = self->stack.sp;
    c11_sbuf buf;
    c11_sbuf__ctor(&buf);
    for(py_Ref p = self->stack.begin; p != sp; p++) {
        switch(p->type) {
            case 0: c11_sbuf__write_cstr(&buf, "nil"); break;
            case tp_int: c11_sbuf__write_i64(&buf, p->_i64); break;
            case tp_float: c11_sbuf__write_f64(&buf, p->_f64, -1); break;
            case tp_bool: c11_sbuf__write_cstr(&buf, p->_bool ? "True" : "False"); break;
            case tp_NoneType: c11_sbuf__write_cstr(&buf, "None"); break;
            case tp_list: {
                pk_sprintf(&buf, "list(%d)", py_list__len(p));
                break;
            }
            case tp_tuple: {
                pk_sprintf(&buf, "tuple(%d)", py_tuple__len(p));
                break;
            }
            case tp_function: {
                Function* ud = py_touserdata(p);
                c11_sbuf__write_cstr(&buf, ud->decl->code.name->data);
                c11_sbuf__write_cstr(&buf, "()");
                break;
            }
            case tp_type: {
                pk_sprintf(&buf, "<class '%t'>", py_totype(p));
                break;
            }
            case tp_str: {
                pk_sprintf(&buf, "%q", py_tosv(p));
                break;
            }
            case tp_module: {
                py_Ref path = py_getdict(p, __path__);
                pk_sprintf(&buf, "<module '%v'>", py_tosv(path));
                break;
            }
            default: {
                pk_sprintf(&buf, "(%t)", p->type);
                break;
            }
        }
        if(p != &sp[-1]) c11_sbuf__write_cstr(&buf, ", ");
    }
    c11_string* stack_str = c11_sbuf__submit(&buf);

    printf("L%-3d: %-25s %-6d [%s]\n",
           Frame__lineno(frame),
           pk_opname(byte.op),
           byte.arg,
           stack_str->data);
    c11_string__delete(stack_str);
}