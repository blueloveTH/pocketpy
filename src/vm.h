#pragma once

#include "codeobject.h"
#include "iter.h"
#include "error.h"

#define __DEF_PY_AS_C(type, ctype, ptype)                       \
    inline ctype& Py##type##_AS_C(const PyVar& obj) {           \
        check_type(obj, ptype);                                \
        return UNION_GET(ctype, obj);                           \
    }

#define __DEF_PY(type, ctype, ptype)                            \
    inline PyVar Py##type(ctype value) {                        \
        return new_object(ptype, value);                         \
    }

#define DEF_NATIVE(type, ctype, ptype)                          \
    __DEF_PY(type, ctype, ptype)                                \
    __DEF_PY_AS_C(type, ctype, ptype)


class VM {
    std::atomic<bool> _stop_flag = false;
    std::vector<PyVar> _small_integers;             // [-5, 256]
    PyVarDict _modules;                             // loaded modules
    emhash8::HashMap<_Str, _Str> _lazy_modules;     // lazy loaded modules
protected:
    std::deque< std::unique_ptr<Frame> > callstack;
    PyVar __py2py_call_signal;
    
    inline void test_stop_flag(){
        if(_stop_flag){
            _stop_flag = false;
            _error("KeyboardInterrupt", "");
        }
    }

    PyVar run_frame(Frame* frame){
        while(frame->has_next_bytecode()){
            const Bytecode& byte = frame->next_bytecode();
            //printf("[%d] %s (%d)\n", frame->stack_size(), OP_NAMES[byte.op], byte.arg);
            //printf("%s\n", frame->code->src->getLine(byte.line).c_str());

            test_stop_flag();

            switch (byte.op)
            {
            case OP_NO_OP: break;       // do nothing
            case OP_LOAD_CONST: frame->push(frame->code->co_consts[byte.arg]); break;
            case OP_LOAD_LAMBDA: {
                PyVar obj = frame->code->co_consts[byte.arg];
                setattr(obj, __module__, frame->_module);
                frame->push(obj);
            } break;
            case OP_LOAD_NAME_REF: {
                frame->push(PyRef(NameRef(frame->code->co_names[byte.arg])));
            } break;
            case OP_LOAD_NAME: {
                frame->push(NameRef(frame->code->co_names[byte.arg]).get(this, frame));
            } break;
            case OP_STORE_NAME_REF: {
                const auto& p = frame->code->co_names[byte.arg];
                NameRef(p).set(this, frame, frame->pop_value(this));
            } break;
            case OP_BUILD_ATTR_REF: {
                const auto& attr = frame->code->co_names[byte.arg];
                PyVar obj = frame->pop_value(this);
                frame->push(PyRef(AttrRef(obj, NameRef(attr))));
            } break;
            case OP_BUILD_INDEX_REF: {
                PyVar index = frame->pop_value(this);
                PyVarRef obj = frame->pop_value(this);
                frame->push(PyRef(IndexRef(obj, index)));
            } break;
            case OP_STORE_REF: {
                PyVar obj = frame->pop_value(this);
                PyVarRef r = frame->pop();
                PyRef_AS_C(r)->set(this, frame, std::move(obj));
            } break;
            case OP_DELETE_REF: {
                PyVarRef r = frame->pop();
                PyRef_AS_C(r)->del(this, frame);
            } break;
            case OP_BUILD_SMART_TUPLE:
            {
                pkpy::ArgList items = frame->pop_n_reversed(byte.arg);
                bool done = false;
                for(int i=0; i<items.size(); i++){
                    if(!items[i]->is_type(_tp_ref)) {
                        done = true;
                        PyVarList values = items.toList();
                        for(int j=i; j<values.size(); j++) frame->try_deref(this, values[j]);
                        frame->push(PyTuple(values));
                        break;
                    }
                }
                if(done) break;
                frame->push(PyRef(TupleRef(items.toList())));
            } break;
            case OP_BUILD_STRING:
            {
                pkpy::ArgList items = frame->pop_n_values_reversed(this, byte.arg);
                _StrStream ss;
                for(int i=0; i<items.size(); i++) ss << PyStr_AS_C(asStr(items[i]));
                frame->push(PyStr(ss.str()));
            } break;
            case OP_LOAD_EVAL_FN: {
                frame->push(builtins->attribs[m_eval]);
            } break;
            case OP_LIST_APPEND: {
                pkpy::ArgList args(2);
                args[1] = frame->pop_value(this);            // obj
                args[0] = frame->top_value_offset(this, -2);     // list
                fast_call(m_append, std::move(args));
            } break;
            case OP_STORE_FUNCTION:
                {
                    PyVar obj = frame->pop_value(this);
                    const _Func& fn = PyFunction_AS_C(obj);
                    setattr(obj, __module__, frame->_module);
                    frame->f_globals()[fn->name] = obj;
                } break;
            case OP_BUILD_CLASS:
                {
                    const _Str& clsName = frame->code->co_names[byte.arg].first;
                    PyVar clsBase = frame->pop_value(this);
                    if(clsBase == None) clsBase = _tp_object;
                    check_type(clsBase, _tp_type);
                    PyVar cls = new_user_type_object(frame->_module, clsName, clsBase);
                    while(true){
                        PyVar fn = frame->pop_value(this);
                        if(fn == None) break;
                        const _Func& f = PyFunction_AS_C(fn);
                        setattr(fn, __module__, frame->_module);
                        setattr(cls, f->name, fn);
                    }
                } break;
            case OP_RETURN_VALUE: return frame->pop_value(this);
            case OP_PRINT_EXPR:
                {
                    const PyVar expr = frame->top_value(this);
                    if(expr == None) break;
                    *_stdout << PyStr_AS_C(asRepr(expr)) << '\n';
                } break;
            case OP_POP_TOP: frame->pop(); break;
            case OP_BINARY_OP:
                {
                    pkpy::ArgList args(2);
                    args._index(1) = frame->pop_value(this);
                    args._index(0) = frame->top_value(this);
                    frame->top() = fast_call(BINARY_SPECIAL_METHODS[byte.arg], std::move(args));
                } break;
            case OP_BITWISE_OP:
                {
                    frame->push(
                        fast_call(BITWISE_SPECIAL_METHODS[byte.arg],
                        frame->pop_n_values_reversed(this, 2))
                    );
                } break;
            case OP_COMPARE_OP:
                {
                    // for __ne__ we use the negation of __eq__
                    int op = byte.arg == 3 ? 2 : byte.arg;
                    PyVar res = fast_call(CMP_SPECIAL_METHODS[op], frame->pop_n_values_reversed(this, 2));
                    if(op != byte.arg) res = PyBool(!PyBool_AS_C(res));
                    frame->push(std::move(res));
                } break;
            case OP_IS_OP:
                {
                    bool ret_c = frame->pop_value(this) == frame->pop_value(this);
                    if(byte.arg == 1) ret_c = !ret_c;
                    frame->push(PyBool(ret_c));
                } break;
            case OP_CONTAINS_OP:
                {
                    PyVar rhs = frame->pop_value(this);
                    bool ret_c = PyBool_AS_C(call(rhs, __contains__, pkpy::oneArg(frame->pop_value(this))));
                    if(byte.arg == 1) ret_c = !ret_c;
                    frame->push(PyBool(ret_c));
                } break;
            case OP_UNARY_NEGATIVE:
                {
                    PyVar obj = frame->pop_value(this);
                    frame->push(num_negated(obj));
                } break;
            case OP_UNARY_NOT:
                {
                    PyVar obj = frame->pop_value(this);
                    const PyVar& obj_bool = asBool(obj);
                    frame->push(PyBool(!PyBool_AS_C(obj_bool)));
                } break;
            case OP_POP_JUMP_IF_FALSE:
                if(!PyBool_AS_C(asBool(frame->pop_value(this)))) frame->jump_abs(byte.arg);
                break;
            case OP_LOAD_NONE: frame->push(None); break;
            case OP_LOAD_TRUE: frame->push(True); break;
            case OP_LOAD_FALSE: frame->push(False); break;
            case OP_LOAD_ELLIPSIS: frame->push(Ellipsis); break;
            case OP_ASSERT:
                {
                    PyVar expr = frame->pop_value(this);
                    if(asBool(expr) != True) _error("AssertionError", "");
                } break;
            case OP_RAISE_ERROR:
                {
                    _Str msg = PyStr_AS_C(asRepr(frame->pop_value(this)));
                    _Str type = PyStr_AS_C(frame->pop_value(this));
                    _error(type, msg);
                } break;
            case OP_BUILD_LIST:
                {
                    frame->push(PyList(
                        frame->pop_n_values_reversed_unlimited(this, byte.arg)
                    ));
                } break;
            case OP_BUILD_MAP:
                {
                    PyVarList items = frame->pop_n_values_reversed_unlimited(this, byte.arg*2);
                    PyVar obj = call(builtins->attribs["dict"]);
                    for(int i=0; i<items.size(); i+=2){
                        call(obj, __setitem__, pkpy::twoArgs(items[i], items[i+1]));
                    }
                    frame->push(obj);
                } break;
            case OP_BUILD_SET:
                {
                    PyVar list = PyList(
                        frame->pop_n_values_reversed_unlimited(this, byte.arg)
                    );
                    PyVar obj = call(builtins->attribs["set"], pkpy::oneArg(list));
                    frame->push(obj);
                } break;
            case OP_DUP_TOP: frame->push(frame->top_value(this)); break;
            case OP_CALL:
                {
                    int ARGC = byte.arg & 0xFFFF;
                    int KWARGC = (byte.arg >> 16) & 0xFFFF;
                    pkpy::ArgList kwargs(0);
                    if(KWARGC > 0) kwargs = frame->pop_n_values_reversed(this, KWARGC*2);
                    pkpy::ArgList args = frame->pop_n_values_reversed(this, ARGC);
                    PyVar callable = frame->pop_value(this);
                    PyVar ret = call(callable, std::move(args), kwargs, true);
                    if(ret == __py2py_call_signal) return ret;
                    frame->push(std::move(ret));
                } break;
            case OP_JUMP_ABSOLUTE: frame->jump_abs(byte.arg); break;
            case OP_SAFE_JUMP_ABSOLUTE: frame->jump_abs_safe(byte.arg); break;
            case OP_GOTO: {
                PyVar obj = frame->pop_value(this);
                const _Str& label = PyStr_AS_C(obj);
                int* target = frame->code->co_labels.try_get(label);
                if(target == nullptr){
                    _error("KeyError", "label '" + label + "' not found");
                }
                frame->jump_abs_safe(*target);
            } break;
            case OP_GET_ITER:
                {
                    PyVar obj = frame->pop_value(this);
                    PyVarOrNull iter_fn = getattr(obj, __iter__, false);
                    if(iter_fn != nullptr){
                        PyVar tmp = call(iter_fn);
                        PyVarRef var = frame->pop();
                        check_type(var, _tp_ref);
                        PyIter_AS_C(tmp)->var = var;
                        frame->push(std::move(tmp));
                    }else{
                        typeError("'" + UNION_TP_NAME(obj) + "' object is not iterable");
                    }
                } break;
            case OP_FOR_ITER:
                {
                    // top() must be PyIter, so no need to try_deref()
                    auto& it = PyIter_AS_C(frame->top());
                    if(it->hasNext()){
                        PyRef_AS_C(it->var)->set(this, frame, it->next());
                    }else{
                        int blockEnd = frame->code->co_blocks[byte.block].end;
                        frame->jump_abs_safe(blockEnd);
                    }
                } break;
            case OP_LOOP_CONTINUE:
                {
                    int blockStart = frame->code->co_blocks[byte.block].start;
                    frame->jump_abs(blockStart);
                } break;
            case OP_LOOP_BREAK:
                {
                    int blockEnd = frame->code->co_blocks[byte.block].end;
                    frame->jump_abs_safe(blockEnd);
                } break;
            case OP_JUMP_IF_FALSE_OR_POP:
                {
                    const PyVar expr = frame->top_value(this);
                    if(asBool(expr)==False) frame->jump_abs(byte.arg);
                    else frame->pop_value(this);
                } break;
            case OP_JUMP_IF_TRUE_OR_POP:
                {
                    const PyVar expr = frame->top_value(this);
                    if(asBool(expr)==True) frame->jump_abs(byte.arg);
                    else frame->pop_value(this);
                } break;
            case OP_BUILD_SLICE:
                {
                    PyVar stop = frame->pop_value(this);
                    PyVar start = frame->pop_value(this);
                    _Slice s;
                    if(start != None) {check_type(start, _tp_int); s.start = (int)PyInt_AS_C(start);}
                    if(stop != None) {check_type(stop, _tp_int); s.stop = (int)PyInt_AS_C(stop);}
                    frame->push(PySlice(s));
                } break;
            case OP_IMPORT_NAME:
                {
                    const _Str& name = frame->code->co_names[byte.arg].first;
                    auto it = _modules.find(name);
                    if(it == _modules.end()){
                        auto it2 = _lazy_modules.find(name);
                        if(it2 == _lazy_modules.end()){
                            _error("ImportError", "module '" + name + "' not found");
                        }else{
                            const _Str& source = it2->second;
                            _Code code = compile(source, name, EXEC_MODE);
                            PyVar _m = newModule(name);
                            _exec(code, _m, {});
                            frame->push(_m);
                            _lazy_modules.erase(it2);
                        }
                    }else{
                        frame->push(it->second);
                    }
                } break;
            // TODO: using "goto" inside with block may cause __exit__ not called
            case OP_WITH_ENTER: call(frame->pop_value(this), __enter__); break;
            case OP_WITH_EXIT: call(frame->pop_value(this), __exit__); break;
            default:
                throw std::runtime_error(_Str("opcode ") + OP_NAMES[byte.op] + " is not implemented");
                break;
            }
        }

        if(frame->code->src->mode == EVAL_MODE || frame->code->src->mode == JSON_MODE){
            if(frame->stack_size() != 1) throw std::runtime_error("stack size is not 1 in EVAL_MODE/JSON_MODE");
            return frame->pop_value(this);
        }

        if(frame->stack_size() != 0) throw std::runtime_error("stack not empty in EXEC_MODE");
        return None;
    }

public:
    PyVarDict _types;
    PyVarDict _userTypes;
    PyVar None, True, False, Ellipsis;

    bool use_stdio;
    std::ostream* _stdout;
    std::ostream* _stderr;
    
    PyVar builtins;         // builtins module
    PyVar _main;            // __main__ module

    int maxRecursionDepth = 1000;

    VM(bool use_stdio){
        this->use_stdio = use_stdio;
        if(use_stdio){
            std::cout.setf(std::ios::unitbuf);
            std::cerr.setf(std::ios::unitbuf);
            this->_stdout = &std::cout;
            this->_stderr = &std::cerr;
        }else{
            this->_stdout = new _StrStream();
            this->_stderr = new _StrStream();
        }
        initializeBuiltinClasses();

        _small_integers.reserve(300);
        for(i64 i=-5; i<=256; i++) _small_integers.push_back(new_object(_tp_int, i));
    }

    void keyboardInterrupt(){
        _stop_flag = true;
    }

    void sleepForSecs(f64 sec){
        i64 ms = (i64)(sec * 1000);
        for(i64 i=0; i<ms; i+=20){
            test_stop_flag();
#ifdef __EMSCRIPTEN__
            emscripten_sleep(20);
#else
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
#endif
        }
    }

    PyVar asStr(const PyVar& obj){
        PyVarOrNull str_fn = getattr(obj, __str__, false);
        if(str_fn != nullptr) return call(str_fn);
        return asRepr(obj);
    }

    inline Frame* top_frame() const {
        if(callstack.empty()) UNREACHABLE();
        return callstack.back().get();
    }

    PyVar asRepr(const PyVar& obj){
        if(obj->is_type(_tp_type)) return PyStr("<class '" + UNION_GET(_Str, obj->attribs[__name__]) + "'>");
        return call(obj, __repr__);
    }

    PyVar asJson(const PyVar& obj){
        return call(obj, __json__);
    }

    const PyVar& asBool(const PyVar& obj){
        if(obj == None) return False;
        if(obj->is_type(_tp_bool)) return obj;
        if(obj->is_type(_tp_int)) return PyBool(PyInt_AS_C(obj) != 0);
        if(obj->is_type(_tp_float)) return PyBool(PyFloat_AS_C(obj) != 0.0);
        PyVarOrNull len_fn = getattr(obj, __len__, false);
        if(len_fn != nullptr){
            PyVar ret = call(len_fn);
            return PyBool(PyInt_AS_C(ret) > 0);
        }
        return True;
    }

    PyVar fast_call(const _Str& name, pkpy::ArgList&& args){
        PyObject* cls = args[0]->_type.get();
        while(cls != None.get()) {
            PyVar* val = cls->attribs.try_get(name);
            if(val != nullptr) return call(*val, std::move(args));
            cls = cls->attribs[__base__].get();
        }
        attributeError(args[0], name);
        return nullptr;
    }

    inline PyVar call(const PyVar& _callable){
        return call(_callable, pkpy::noArg(), pkpy::noArg(), false);
    }

    template<typename ArgT>
    inline std::enable_if_t<std::is_same_v<std::remove_const_t<std::remove_reference_t<ArgT>>, pkpy::ArgList>, PyVar>
    call(const PyVar& _callable, ArgT&& args){
        return call(_callable, std::forward<ArgT>(args), pkpy::noArg(), false);
    }

    template<typename ArgT>
    inline std::enable_if_t<std::is_same_v<std::remove_const_t<std::remove_reference_t<ArgT>>, pkpy::ArgList>, PyVar>
    call(const PyVar& obj, const _Str& func, ArgT&& args){
        return call(getattr(obj, func), std::forward<ArgT>(args), pkpy::noArg(), false);
    }

    inline PyVar call(const PyVar& obj, const _Str& func){
        return call(getattr(obj, func), pkpy::noArg(), pkpy::noArg(), false);
    }

    PyVar call(const PyVar& _callable, pkpy::ArgList args, const pkpy::ArgList& kwargs, bool opCall){
        if(_callable->is_type(_tp_type)){
            auto it = _callable->attribs.find(__new__);
            PyVar obj;
            if(it != _callable->attribs.end()){
                obj = call(it->second, args, kwargs, false);
            }else{
                obj = new_object(_callable, (i64)-1);
                PyVarOrNull init_fn = getattr(obj, __init__, false);
                if (init_fn != nullptr) call(init_fn, args, kwargs, false);
            }
            return obj;
        }

        const PyVar* callable = &_callable;
        if((*callable)->is_type(_tp_bounded_method)){
            auto& bm = PyBoundedMethod_AS_C((*callable));
            // TODO: avoid insertion here, bad performance
            pkpy::ArgList new_args(args.size()+1);
            new_args[0] = bm.obj;
            for(int i=0; i<args.size(); i++) new_args[i+1] = args[i];
            callable = &bm.method;
            args = std::move(new_args);
        }
        
        if((*callable)->is_type(_tp_native_function)){
            const auto& f = UNION_GET(_CppFunc, *callable);
            // _CppFunc do not support kwargs
            return f(this, args);
        } else if((*callable)->is_type(_tp_function)){
            const _Func& fn = PyFunction_AS_C((*callable));
            PyVarDict locals;
            int i = 0;
            for(const auto& name : fn->args){
                if(i < args.size()){
                    locals.emplace(name, args[i++]);
                    continue;
                }
                typeError("missing positional argument '" + name + "'");
            }

            locals.insert(fn->kwArgs.begin(), fn->kwArgs.end());

            std::vector<_Str> positional_overrided_keys;
            if(!fn->starredArg.empty()){
                // handle *args
                PyVarList vargs;
                while(i < args.size()) vargs.push_back(args[i++]);
                locals.emplace(fn->starredArg, PyTuple(std::move(vargs)));
            }else{
                for(const auto& key : fn->kwArgsOrder){
                    if(i < args.size()){
                        locals[key] = args[i++];
                        positional_overrided_keys.push_back(key);
                    }else{
                        break;
                    }
                }
                if(i < args.size()) typeError("too many arguments");
            }
            
            for(int i=0; i<kwargs.size(); i+=2){
                const _Str& key = PyStr_AS_C(kwargs[i]);
                if(!fn->kwArgs.contains(key)){
                    typeError(key.__escape(true) + " is an invalid keyword argument for " + fn->name + "()");
                }
                const PyVar& val = kwargs[i+1];
                if(!positional_overrided_keys.empty()){
                    auto it = std::find(positional_overrided_keys.begin(), positional_overrided_keys.end(), key);
                    if(it != positional_overrided_keys.end()){
                        typeError("multiple values for argument '" + key + "'");
                    }
                }
                locals[key] = val;
            }

            PyVar* it_m = (*callable)->attribs.try_get(__module__);
            PyVar _module = it_m != nullptr ? *it_m : top_frame()->_module;
            if(opCall){
                __pushNewFrame(fn->code, _module, std::move(locals));
                return __py2py_call_signal;
            }
            return _exec(fn->code, _module, std::move(locals));
        }
        typeError("'" + UNION_TP_NAME(*callable) + "' object is not callable");
        return None;
    }


    // repl mode is only for setting `frame->id` to 0
    virtual PyVarOrNull exec(_Str source, _Str filename, CompileMode mode, PyVar _module=nullptr){
        if(_module == nullptr) _module = _main;
        try {
            _Code code = compile(source, filename, mode);
            //if(filename != "<builtins>") std::cout << disassemble(code) << std::endl;
            return _exec(code, _module, {});
        }catch (const _Error& e){
            *_stderr << e.what() << '\n';
        }
        catch (const std::exception& e) {
            auto re = RuntimeError("UnexpectedError", e.what(), _cleanErrorAndGetSnapshots());
            *_stderr << re.what() << '\n';
        }
        return nullptr;
    }

    virtual void execAsync(_Str source, _Str filename, CompileMode mode) {
        exec(source, filename, mode);
    }

    Frame* __pushNewFrame(const _Code& code, PyVar _module, PyVarDict&& locals){
        if(code == nullptr) UNREACHABLE();
        if(callstack.size() > maxRecursionDepth){
            throw RuntimeError("RecursionError", "maximum recursion depth exceeded", _cleanErrorAndGetSnapshots());
        }
        Frame* frame = new Frame(code, _module, std::move(locals));
        callstack.emplace_back(frame);
        return frame;
    }

    PyVar _exec(_Code code, PyVar _module, PyVarDict&& locals){
        Frame* frame = __pushNewFrame(code, _module, std::move(locals));
        Frame* frameBase = frame;
        PyVar ret = nullptr;

        while(true){
            ret = run_frame(frame);
            if(ret != __py2py_call_signal){
                if(frame == frameBase){         // [ frameBase<- ]
                    break;
                }else{
                    callstack.pop_back();
                    frame = callstack.back().get();
                    frame->push(ret);
                }
            }else{
                frame = callstack.back().get();  // [ frameBase, newFrame<- ]
            }
        }

        callstack.pop_back();
        return ret;
    }

    PyVar new_user_type_object(PyVar mod, _Str name, PyVar base){
        PyVar obj = pkpy::make_shared<PyObject, Py_<i64>>((i64)1, _tp_type);
        setattr(obj, __base__, base);
        _Str fullName = UNION_NAME(mod) + "." +name;
        setattr(obj, __name__, PyStr(fullName));
        _userTypes[fullName] = obj;
        setattr(mod, name, obj);
        return obj;
    }

    PyVar new_type_object(_Str name, PyVar base=nullptr) {
        if(base == nullptr) base = _tp_object;
        PyVar obj = pkpy::make_shared<PyObject, Py_<i64>>((i64)0, _tp_type);
        setattr(obj, __base__, base);
        _types[name] = obj;
        return obj;
    }

    template<typename T>
    inline PyVar new_object(PyVar type, T _value) {
        if(!type->is_type(_tp_type)) UNREACHABLE();
        return pkpy::make_shared<PyObject, Py_<T>>(_value, type);
    }

    PyVar newModule(_Str name) {
        PyVar obj = new_object(_tp_module, (i64)-2);
        setattr(obj, __name__, PyStr(name));
        _modules[name] = obj;
        return obj;
    }

    void addLazyModule(_Str name, _Str source){
        _lazy_modules[name] = source;
    }

    PyVarOrNull getattr(const PyVar& obj, const _Str& name, bool throw_err=true) {
        PyVarDict::iterator it;
        PyObject* cls;

        if(obj->is_type(_tp_super)){
            const PyVar* root = &obj;
            int depth = 1;
            while(true){
                root = &UNION_GET(PyVar, *root);
                if(!(*root)->is_type(_tp_super)) break;
                depth++;
            }
            cls = (*root)->_type.get();
            for(int i=0; i<depth; i++) cls = cls->attribs[__base__].get();

            it = (*root)->attribs.find(name);
            if(it != (*root)->attribs.end()) return it->second;        
        }else{
            it = obj->attribs.find(name);
            if(it != obj->attribs.end()) return it->second;
            cls = obj->_type.get();
        }

        while(cls != None.get()) {
            it = cls->attribs.find(name);
            if(it != cls->attribs.end()){
                PyVar valueFromCls = it->second;
                if(valueFromCls->is_type(_tp_function) || valueFromCls->is_type(_tp_native_function)){
                    return PyBoundedMethod({obj, std::move(valueFromCls)});
                }else{
                    return valueFromCls;
                }
            }
            cls = cls->attribs[__base__].get();
        }
        if(throw_err) attributeError(obj, name);
        return nullptr;
    }

    template<typename T>
    void setattr(PyObject* obj, const _Str& name, T&& value) {
        while(obj->is_type(_tp_super)) obj = ((Py_<PyVar>*)obj)->_valueT.get();
        obj->attribs[name] = value;
    }

    template<typename T>
    inline void setattr(PyVar& obj, const _Str& name, T&& value) {
        setattr(obj.get(), name, value);
    }

    void bindMethod(_Str typeName, _Str funcName, _CppFunc fn) {
        PyVar* type = _types.try_get(typeName);
        if(type == nullptr) type = _userTypes.try_get(typeName);
        if(type == nullptr) UNREACHABLE();
        PyVar func = PyNativeFunction(fn);
        setattr(*type, funcName, func);
    }

    void bindMethodMulti(std::vector<_Str> typeNames, _Str funcName, _CppFunc fn) {
        for(auto& typeName : typeNames){
            bindMethod(typeName, funcName, fn);
        }
    }

    void bindBuiltinFunc(_Str funcName, _CppFunc fn) {
        bindFunc(builtins, funcName, fn);
    }

    void bindFunc(PyVar module, _Str funcName, _CppFunc fn) {
        check_type(module, _tp_module);
        PyVar func = PyNativeFunction(fn);
        setattr(module, funcName, func);
    }

    bool isinstance(PyVar obj, PyVar type){
        check_type(type, _tp_type);
        PyObject* t = obj->_type.get();
        while (t != None.get()){
            if (t == type.get()) return true;
            t = t->attribs[__base__].get();
        }
        return false;
    }

    inline bool is_int_or_float(const PyVar& obj) const{
        return obj->is_type(_tp_int) || obj->is_type(_tp_float);
    }

    inline bool is_int_or_float(const PyVar& obj1, const PyVar& obj2) const{
        return is_int_or_float(obj1) && is_int_or_float(obj2);
    }

    inline f64 num_to_float(const PyVar& obj){
        if (obj->is_type(_tp_int)){
            return (f64)PyInt_AS_C(obj);
        }else if(obj->is_type(_tp_float)){
            return PyFloat_AS_C(obj);
        }
        UNREACHABLE();
    }

    PyVar num_negated(const PyVar& obj){
        if (obj->is_type(_tp_int)){
            return PyInt(-PyInt_AS_C(obj));
        }else if(obj->is_type(_tp_float)){
            return PyFloat(-PyFloat_AS_C(obj));
        }
        typeError("unsupported operand type(s) for -");
        return nullptr;
    }

    int normalizedIndex(int index, int size){
        if(index < 0) index += size;
        if(index < 0 || index >= size){
            indexError("index out of range, " + std::to_string(index) + " not in [0, " + std::to_string(size) + ")");
        }
        return index;
    }

    _Str disassemble(_Code code){
        std::vector<int> jumpTargets;
        for(auto byte : code->co_code){
            if(byte.op == OP_JUMP_ABSOLUTE || byte.op == OP_SAFE_JUMP_ABSOLUTE || byte.op == OP_POP_JUMP_IF_FALSE){
                jumpTargets.push_back(byte.arg);
            }
        }
        _StrStream ss;
        ss << std::string(54, '-') << '\n';
        ss << code->name << ":\n";
        int prev_line = -1;
        for(int i=0; i<code->co_code.size(); i++){
            const Bytecode& byte = code->co_code[i];
            _Str line = std::to_string(byte.line);
            if(byte.line == prev_line) line = "";
            else{
                if(prev_line != -1) ss << "\n";
                prev_line = byte.line;
            }

            std::string pointer;
            if(std::find(jumpTargets.begin(), jumpTargets.end(), i) != jumpTargets.end()){
                pointer = "-> ";
            }else{
                pointer = "   ";
            }
            ss << pad(line, 8) << pointer << pad(std::to_string(i), 3);
            ss << " " << pad(OP_NAMES[byte.op], 20) << " ";
            // ss << pad(byte.arg == -1 ? "" : std::to_string(byte.arg), 5);
            std::string argStr = byte.arg == -1 ? "" : std::to_string(byte.arg);
            if(byte.op == OP_LOAD_CONST){
                argStr += " (" + PyStr_AS_C(asRepr(code->co_consts[byte.arg])) + ")";
            }
            if(byte.op == OP_LOAD_NAME_REF || byte.op == OP_LOAD_NAME){
                argStr += " (" + code->co_names[byte.arg].first.__escape(true) + ")";
            }
            ss << pad(argStr, 20);      // may overflow
            ss << code->co_blocks[byte.block].to_string();
            if(i != code->co_code.size() - 1) ss << '\n';
        }
        _StrStream consts;
        consts << "co_consts: ";
        consts << PyStr_AS_C(asRepr(PyList(code->co_consts)));

        _StrStream names;
        names << "co_names: ";
        PyVarList list;
        for(int i=0; i<code->co_names.size(); i++){
            list.push_back(PyStr(code->co_names[i].first));
        }
        names << PyStr_AS_C(asRepr(PyList(list)));
        ss << '\n' << consts.str() << '\n' << names.str() << '\n';

        for(int i=0; i<code->co_consts.size(); i++){
            PyVar obj = code->co_consts[i];
            if(obj->is_type(_tp_function)){
                const auto& f = PyFunction_AS_C(obj);
                ss << disassemble(f->code);
            }
        }
        return _Str(ss.str());
    }

    // for quick access
    PyVar _tp_object, _tp_type, _tp_int, _tp_float, _tp_bool, _tp_str;
    PyVar _tp_list, _tp_tuple;
    PyVar _tp_function, _tp_native_function, _tp_native_iterator, _tp_bounded_method;
    PyVar _tp_slice, _tp_range, _tp_module, _tp_ref;
    PyVar _tp_super;

    template<typename P>
    inline PyVarRef PyRef(P&& value) {
        static_assert(std::is_base_of<BaseRef, P>::value, "P should derive from BaseRef");
        return new_object(_tp_ref, std::forward<P>(value));
    }

    inline const BaseRef* PyRef_AS_C(const PyVar& obj)
    {
        if(!obj->is_type(_tp_ref)) typeError("expected an l-value");
        return (const BaseRef*)(obj->value());
    }

    __DEF_PY_AS_C(Int, i64, _tp_int)
    inline PyVar PyInt(i64 value) { 
        if(value >= -5 && value <= 256) return _small_integers[value + 5];
        return new_object(_tp_int, value);
    }

    DEF_NATIVE(Float, f64, _tp_float)
    DEF_NATIVE(Str, _Str, _tp_str)
    DEF_NATIVE(List, PyVarList, _tp_list)
    DEF_NATIVE(Tuple, PyVarList, _tp_tuple)
    DEF_NATIVE(Function, _Func, _tp_function)
    DEF_NATIVE(NativeFunction, _CppFunc, _tp_native_function)
    DEF_NATIVE(Iter, _Iterator, _tp_native_iterator)
    DEF_NATIVE(BoundedMethod, _BoundedMethod, _tp_bounded_method)
    DEF_NATIVE(Range, _Range, _tp_range)
    DEF_NATIVE(Slice, _Slice, _tp_slice)
    
    // there is only one True/False, so no need to copy them!
    inline bool PyBool_AS_C(const PyVar& obj){return obj == True;}
    inline const PyVar& PyBool(bool value){return value ? True : False;}

    void initializeBuiltinClasses(){
        _tp_object = pkpy::make_shared<PyObject, Py_<i64>>((i64)0, nullptr);
        _tp_type = pkpy::make_shared<PyObject, Py_<i64>>((i64)0, nullptr);

        _types["object"] = _tp_object;
        _types["type"] = _tp_type;

        _tp_bool = new_type_object("bool");
        _tp_int = new_type_object("int");
        _tp_float = new_type_object("float");
        _tp_str = new_type_object("str");
        _tp_list = new_type_object("list");
        _tp_tuple = new_type_object("tuple");
        _tp_slice = new_type_object("slice");
        _tp_range = new_type_object("range");
        _tp_module = new_type_object("module");
        _tp_ref = new_type_object("_ref");

        new_type_object("NoneType");
        new_type_object("ellipsis");
        
        _tp_function = new_type_object("function");
        _tp_native_function = new_type_object("_native_function");
        _tp_native_iterator = new_type_object("_native_iterator");
        _tp_bounded_method = new_type_object("_bounded_method");
        _tp_super = new_type_object("super");

        this->None = new_object(_types["NoneType"], (i64)0);
        this->Ellipsis = new_object(_types["ellipsis"], (i64)0);
        this->True = new_object(_tp_bool, true);
        this->False = new_object(_tp_bool, false);
        this->builtins = newModule("builtins");
        this->_main = newModule("__main__");

        setattr(_tp_type, __base__, _tp_object);
        _tp_type->_type = _tp_type;
        setattr(_tp_object, __base__, None);
        _tp_object->_type = _tp_type;
        
        for (auto& [name, type] : _types) {
            setattr(type, __name__, PyStr(name));
        }

        this->__py2py_call_signal = new_object(_tp_object, (i64)7);

        std::vector<_Str> publicTypes = {"type", "object", "bool", "int", "float", "str", "list", "tuple", "range"};
        for (auto& name : publicTypes) {
            setattr(builtins, name, _types[name]);
        }
    }

    i64 hash(const PyVar& obj){
        if (obj->is_type(_tp_int)) return PyInt_AS_C(obj);
        if (obj->is_type(_tp_bool)) return PyBool_AS_C(obj) ? 1 : 0;
        if (obj->is_type(_tp_float)){
            f64 val = PyFloat_AS_C(obj);
            return (i64)std::hash<f64>()(val);
        }
        if (obj->is_type(_tp_str)) return PyStr_AS_C(obj).hash();
        if (obj->is_type(_tp_type)) return (i64)obj.get();
        if (obj->is_type(_tp_tuple)) {
            i64 x = 1000003;
            for (const auto& item : PyTuple_AS_C(obj)) {
                i64 y = hash(item);
                // this is recommended by Github Copilot
                // i am not sure whether it is a good idea
                x = x ^ (y + 0x9e3779b9 + (x << 6) + (x >> 2));
            }
            return x;
        }
        typeError("unhashable type: " +  UNION_TP_NAME(obj));
        return 0;
    }

    /***** Error Reporter *****/
private:
    void _error(const _Str& name, const _Str& msg){
        throw RuntimeError(name, msg, _cleanErrorAndGetSnapshots());
    }

    std::stack<_Str> _cleanErrorAndGetSnapshots(){
        std::stack<_Str> snapshots;
        while (!callstack.empty()){
            if(snapshots.size() < 8){
                snapshots.push(callstack.back()->curr_snapshot());
            }
            callstack.pop_back();
        }
        return snapshots;
    }

public:
    void typeError(const _Str& msg){ _error("TypeError", msg); }
    void zeroDivisionError(){ _error("ZeroDivisionError", "division by zero"); }
    void indexError(const _Str& msg){ _error("IndexError", msg); }
    void valueError(const _Str& msg){ _error("ValueError", msg); }
    void nameError(const _Str& name){ _error("NameError", "name '" + name + "' is not defined"); }

    void attributeError(PyVar obj, const _Str& name){
        _error("AttributeError", "type '" +  UNION_TP_NAME(obj) + "' has no attribute '" + name + "'");
    }

    inline void check_type(const PyVar& obj, const PyVar& type){
        if(!obj->is_type(type)) typeError("expected '" + UNION_NAME(type) + "', but got '" + UNION_TP_NAME(obj) + "'");
    }

    inline void check_args_size(const pkpy::ArgList& args, int size, bool method=false){
        if(args.size() == size) return;
        if(method) typeError(args.size()>size ? "too many arguments" : "too few arguments");
        else typeError("expected " + std::to_string(size) + " arguments, but got " + std::to_string(args.size()));
    }

    virtual ~VM() {
        if(!use_stdio){
            delete _stdout;
            delete _stderr;
        }
    }

    _Code compile(_Str source, _Str filename, CompileMode mode);
};

/***** Pointers' Impl *****/

PyVar NameRef::get(VM* vm, Frame* frame) const{
    PyVar* val;
    val = frame->f_locals.try_get(pair->first);
    if(val) return *val;
    val = frame->f_globals().try_get(pair->first);
    if(val) return *val;
    val = vm->builtins->attribs.try_get(pair->first);
    if(val) return *val;
    vm->nameError(pair->first);
    return nullptr;
}

void NameRef::set(VM* vm, Frame* frame, PyVar val) const{
    switch(pair->second) {
        case NAME_LOCAL: frame->f_locals[pair->first] = std::move(val); break;
        case NAME_GLOBAL:
        {
            if(frame->f_locals.contains(pair->first)){
                frame->f_locals[pair->first] = std::move(val);
            }else{
                frame->f_globals()[pair->first] = std::move(val);
            }
        } break;
        default: UNREACHABLE();
    }
}

void NameRef::del(VM* vm, Frame* frame) const{
    switch(pair->second) {
        case NAME_LOCAL: {
            if(frame->f_locals.count(pair->first) > 0){
                frame->f_locals.erase(pair->first);
            }else{
                vm->nameError(pair->first);
            }
        } break;
        case NAME_GLOBAL:
        {
            if(frame->f_locals.count(pair->first) > 0){
                frame->f_locals.erase(pair->first);
            }else{
                if(frame->f_globals().count(pair->first) > 0){
                    frame->f_globals().erase(pair->first);
                }else{
                    vm->nameError(pair->first);
                }
            }
        } break;
        default: UNREACHABLE();
    }
}

PyVar AttrRef::get(VM* vm, Frame* frame) const{
    return vm->getattr(obj, attr.pair->first);
}

void AttrRef::set(VM* vm, Frame* frame, PyVar val) const{
    vm->setattr(obj, attr.pair->first, val);
}

void AttrRef::del(VM* vm, Frame* frame) const{
    vm->typeError("cannot delete attribute");
}

PyVar IndexRef::get(VM* vm, Frame* frame) const{
    return vm->call(obj, __getitem__, pkpy::oneArg(index));
}

void IndexRef::set(VM* vm, Frame* frame, PyVar val) const{
    vm->call(obj, __setitem__, pkpy::twoArgs(index, val));
}

void IndexRef::del(VM* vm, Frame* frame) const{
    vm->call(obj, __delitem__, pkpy::oneArg(index));
}

PyVar TupleRef::get(VM* vm, Frame* frame) const{
    PyVarList args(varRefs.size());
    for (int i = 0; i < varRefs.size(); i++) {
        args[i] = vm->PyRef_AS_C(varRefs[i])->get(vm, frame);
    }
    return vm->PyTuple(args);
}

void TupleRef::set(VM* vm, Frame* frame, PyVar val) const{
    if(!val->is_type(vm->_tp_tuple) && !val->is_type(vm->_tp_list)){
        vm->typeError("only tuple or list can be unpacked");
    }
    const PyVarList& args = UNION_GET(PyVarList, val);
    if(args.size() > varRefs.size()) vm->valueError("too many values to unpack");
    if(args.size() < varRefs.size()) vm->valueError("not enough values to unpack");
    for (int i = 0; i < varRefs.size(); i++) {
        vm->PyRef_AS_C(varRefs[i])->set(vm, frame, args[i]);
    }
}

void TupleRef::del(VM* vm, Frame* frame) const{
    for (auto& r : varRefs) vm->PyRef_AS_C(r)->del(vm, frame);
}

/***** Frame's Impl *****/
inline void Frame::try_deref(VM* vm, PyVar& v){
    if(v->is_type(vm->_tp_ref)) v = vm->PyRef_AS_C(v)->get(vm, this);
}

/***** Iterators' Impl *****/
PyVar RangeIterator::next(){
    PyVar val = vm->PyInt(current);
    current += r.step;
    return val;
}

PyVar StringIterator::next(){
    return vm->PyStr(str.u8_getitem(index++));
}

enum ThreadState {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_SUSPENDED,
    THREAD_FINISHED
};

class ThreadedVM : public VM {
    std::atomic<ThreadState> _state = THREAD_READY;
    _Str _sharedStr = "";

#ifndef __EMSCRIPTEN__
    std::thread* _thread = nullptr;
    void __deleteThread(){
        if(_thread != nullptr){
            terminate();
            _thread->join();
            delete _thread;
            _thread = nullptr;
        }
    }
#else
    void __deleteThread(){
        terminate();
    }
#endif

public:
    ThreadedVM(bool use_stdio) : VM(use_stdio) {
        bindBuiltinFunc("__string_channel_call", [](VM* vm, const pkpy::ArgList& args){
            vm->check_args_size(args, 1);
            _Str data = vm->PyStr_AS_C(args[0]);

            ThreadedVM* tvm = (ThreadedVM*)vm;
            tvm->_sharedStr = data;
            tvm->suspend();
            return tvm->PyStr(tvm->readJsonRpcRequest());
        });
    }

    void terminate(){
        if(_state == THREAD_RUNNING || _state == THREAD_SUSPENDED){
            keyboardInterrupt();
#ifdef __EMSCRIPTEN__
            // no way to terminate safely
#else
            while(_state != THREAD_FINISHED);
#endif
        }
    }

    void suspend(){
        if(_state != THREAD_RUNNING) UNREACHABLE();
        _state = THREAD_SUSPENDED;
        while(_state == THREAD_SUSPENDED){
            test_stop_flag();
#ifdef __EMSCRIPTEN__
            emscripten_sleep(20);
#else
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
#endif
        }
    }

    _Str readJsonRpcRequest(){
        _Str copy = _sharedStr;
        _sharedStr = "";
        return copy;
    }

    /***** For outer use *****/

    ThreadState getState(){
        return _state;
    }

    void writeJsonrpcResponse(const char* value){
        if(_state != THREAD_SUSPENDED) UNREACHABLE();
        _sharedStr = _Str(value);
        _state = THREAD_RUNNING;
    }

    void execAsync(_Str source, _Str filename, CompileMode mode) override {
        if(_state != THREAD_READY) UNREACHABLE();

#ifdef __EMSCRIPTEN__
        this->_state = THREAD_RUNNING;
        VM::exec(source, filename, mode);
        this->_state = THREAD_FINISHED;
#else
        __deleteThread();
        _thread = new std::thread([=](){
            this->_state = THREAD_RUNNING;
            VM::exec(source, filename, mode);
            this->_state = THREAD_FINISHED;
        });
#endif
    }

    PyVarOrNull exec(_Str source, _Str filename, CompileMode mode, PyVar _module=nullptr) override {
        if(_state == THREAD_READY) return VM::exec(source, filename, mode, _module);
        auto callstackBackup = std::move(callstack);
        callstack.clear();
        PyVarOrNull ret = VM::exec(source, filename, mode, _module);
        callstack = std::move(callstackBackup);
        return ret;
    }

    void resetState(){
        if(this->_state != THREAD_FINISHED) return;
        this->_state = THREAD_READY;
    }

    ~ThreadedVM(){
        __deleteThread();
    }
};