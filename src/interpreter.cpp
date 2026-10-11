#include "aether/interpreter.h"
#include "aether/baseline_jit.h"
#include "aether/optimizing_jit.h"
#include "aether/runtime.h"
#include "aether/typed_array.h"
#include <cmath>

namespace aether {

TaggedValue Interpreter::Execute(const BytecodeArray& bytecode, const std::vector<TaggedValue>& args, TaggedValue this_val, LexicalEnvironment* env) {
    struct InterpreterScope {
        Interpreter* prev;
        InterpreterScope(Interpreter* cur) : prev(Interpreter::s_current_interpreter_) {
            Interpreter::s_current_interpreter_ = cur;
        }
        ~InterpreterScope() {
            Interpreter::s_current_interpreter_ = prev;
        }
    };
    InterpreterScope current_scope(this);

    // 1. Direct dispatch to Tier 2 Optimizing JIT if already compiled
    if (const ExecutableMemory* t2 = bytecode.GetTier2Code()) {
        return BaselineJIT::Invoke(*t2, args.empty() ? nullptr : args.data(), static_cast<uint32_t>(args.size()));
    }

    // 2. Direct dispatch to Tier 1 Baseline JIT if already compiled; promote to Tier 2 when hot
    if (const ExecutableMemory* t1 = bytecode.GetTier1Code()) {
        bytecode.IncrementInvocationCount();
        if (bytecode.InvocationCount() >= OptimizingJIT::kInvocationThreshold && OptimizingJIT::CanCompile(bytecode)) {
            auto opt_code = OptimizingJIT::Compile(bytecode, &feedback_);
            if (opt_code) {
                bytecode.SetTier2Code(std::move(opt_code));
                return BaselineJIT::Invoke(*bytecode.GetTier2Code(), args.empty() ? nullptr : args.data(), static_cast<uint32_t>(args.size()));
            }
        }
        return BaselineJIT::Invoke(*t1, args.empty() ? nullptr : args.data(), static_cast<uint32_t>(args.size()));
    }

    // 3. Increment invocation profile counter; promote to Tier 2 if hot enough, or Tier 1
    bytecode.IncrementInvocationCount();
    if (bytecode.InvocationCount() >= OptimizingJIT::kInvocationThreshold && OptimizingJIT::CanCompile(bytecode)) {
        auto opt_code = OptimizingJIT::Compile(bytecode, &feedback_);
        if (opt_code) {
            bytecode.SetTier2Code(std::move(opt_code));
            return BaselineJIT::Invoke(*bytecode.GetTier2Code(), args.empty() ? nullptr : args.data(), static_cast<uint32_t>(args.size()));
        }
    }
    if (bytecode.InvocationCount() >= BaselineJIT::kInvocationThreshold && BaselineJIT::CanCompile(bytecode)) {
        auto jit_code = BaselineJIT::Compile(bytecode);
        if (jit_code) {
            bytecode.SetTier1Code(std::move(jit_code));
            return BaselineJIT::Invoke(*bytecode.GetTier1Code(), args.empty() ? nullptr : args.data(), static_cast<uint32_t>(args.size()));
        }
    }

    ExecutionFrame frame(bytecode, args, this_val, env);
    acc_ = TaggedValue::Undefined();
    return RunExecutionLoop(frame, bytecode.Code().data(), nullptr);
}

TaggedValue Interpreter::ResumeGenerator(JSGeneratorObject* gen, TaggedValue sent_val, bool is_rejection) {
    if (!gen) return TaggedValue::Undefined();
    if (gen->GetState() == JSGeneratorObject::State::Completed) {
        if (gen->IsAsync()) return TaggedValue::Undefined();
        return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::Undefined(), true));
    }
    if (gen->GetState() == JSGeneratorObject::State::Executing) {
        TaggedValue exc = TaggedValue::FromHeapObject(new JSError("TypeError", "Generator is already running"));
        has_pending_exception_ = true;
        pending_exception_ = exc;
        return exc;
    }

    gen->SetState(JSGeneratorObject::State::Executing);
    const BytecodeArray& bytecode = *gen->Function()->GetBytecode();
    ExecutionFrame frame(bytecode, gen->Args(), gen->ThisVal(), gen->Environment());
    frame.SetGenerator(gen);

    const uint8_t* code_start = bytecode.Code().data();
    const uint8_t* start_pc = code_start;
    if (gen->PCOffset() > 0) {
        frame.GetRegisters() = gen->SavedRegisters();
        for (const auto& sh : gen->SavedHandlers()) {
            frame.PushExceptionHandler(code_start + sh.target_offset, sh.env);
        }
        start_pc = code_start + gen->PCOffset();
        acc_ = sent_val;
    } else {
        acc_ = TaggedValue::Undefined();
    }

    if (is_rejection) {
        if (frame.HasExceptionHandler()) {
            auto h = frame.PopExceptionHandler();
            start_pc = h.target_pc;
            frame.SetEnvironment(h.env);
            acc_ = sent_val;
            has_pending_exception_ = false;
        } else {
            has_pending_exception_ = true;
            pending_exception_ = sent_val;
            gen->SetState(JSGeneratorObject::State::Completed);
            return pending_exception_;
        }
    }

    bool yielded = false;
    TaggedValue result = RunExecutionLoop(frame, start_pc, &yielded);

    if (has_pending_exception_) {
        gen->SetState(JSGeneratorObject::State::Completed);
        return pending_exception_;
    }

    if (gen->IsAsync()) {
        if (yielded) {
            return result;
        } else {
            gen->SetState(JSGeneratorObject::State::Completed);
            return result;
        }
    }

    if (yielded) {
        return TaggedValue::FromHeapObject(CreateIterResult(result, false));
    } else {
        gen->SetState(JSGeneratorObject::State::Completed);
        return TaggedValue::FromHeapObject(CreateIterResult(result, true));
    }
}

void Interpreter::RunAsyncStep(JSGeneratorObject* gen, TaggedValue sent_val, bool is_rejection) {
    if (!gen) return;
    JSPromise* outer_promise = gen->AsyncPromise();
    ClearPendingException();

    TaggedValue step_result = ResumeGenerator(gen, sent_val, is_rejection);

    if (HasPendingException()) {
        TaggedValue reason = PendingException();
        ClearPendingException();
        if (outer_promise) {
            outer_promise->Reject(reason, CurrentMicrotaskQueue());
        }
        return;
    }

    if (gen->GetState() == JSGeneratorObject::State::Completed) {
        if (outer_promise) {
            outer_promise->Fulfill(step_result, CurrentMicrotaskQueue());
        }
        return;
    }

    JSPromise* awaited_promise = nullptr;
    if (step_result.IsHeapObject() && step_result.AsHeapObject()->IsPromise()) {
        awaited_promise = static_cast<JSPromise*>(step_result.AsHeapObject());
    } else {
        awaited_promise = AllocateRuntime<JSPromise>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
        if (Realm::Current() && Realm::Current()->GetPromisePrototype()) {
            awaited_promise->SetPrototype(Realm::Current()->GetPromisePrototype());
        }
        awaited_promise->Fulfill(step_result, CurrentMicrotaskQueue());
    }

    auto* on_fulfilled = AllocateRuntime<JSNativeFunction>("", [this, gen](const std::vector<TaggedValue>& args) {
        TaggedValue v = args.empty() ? TaggedValue::Undefined() : args[0];
        this->RunAsyncStep(gen, v, false);
        return TaggedValue::Undefined();
    });

    auto* on_rejected = AllocateRuntime<JSNativeFunction>("", [this, gen](const std::vector<TaggedValue>& args) {
        TaggedValue r = args.empty() ? TaggedValue::Undefined() : args[0];
        this->RunAsyncStep(gen, r, true);
        return TaggedValue::Undefined();
    });

    awaited_promise->AddReaction(TaggedValue::FromHeapObject(on_fulfilled), TaggedValue::FromHeapObject(on_rejected), nullptr, CurrentMicrotaskQueue());
}

TaggedValue Interpreter::RunExecutionLoop(ExecutionFrame& frame, const uint8_t* start_pc, bool* out_yielded) {
    const BytecodeArray& bytecode = frame.GetBytecode();
    struct RootGuard {
        TaggedValue* acc_ptr;
        ExecutionFrame& frame;
        IsolateHeap* heap;
        RootGuard(TaggedValue* a, ExecutionFrame& f) : acc_ptr(a), frame(f), heap(IsolateHeap::Current()) {
            if (heap) {
                heap->RegisterExtraRoot(acc_ptr);
                for (auto& r : frame.GetRegisters()) {
                    heap->RegisterExtraRoot(&r);
                }
            }
        }
        ~RootGuard() {
            if (heap) {
                heap->UnregisterExtraRoot(acc_ptr);
                for (auto& r : frame.GetRegisters()) {
                    heap->UnregisterExtraRoot(&r);
                }
            }
        }
    } root_guard(&acc_, frame);

    struct EnvGuard {
        Interpreter& interp;
        ExecutionFrame& frame;
        ~EnvGuard() {
            interp.last_environment_ = frame.GetEnvironment();
        }
    } env_guard{*this, frame};

    const uint8_t* code_start = bytecode.Code().data();
    const uint8_t* pc = start_pc;
    const uint8_t* code_end = code_start + bytecode.Code().size();

    while (pc < code_end) {
        Opcode op = static_cast<Opcode>(*pc++);

        switch (op) {
            case Opcode::LdaConstant: {
                uint16_t idx = pc[0] | (pc[1] << 8);
                pc += 2;
                acc_ = bytecode.Constants()[idx];
                break;
            }
            case Opcode::LdaZero: {
                acc_ = TaggedValue::FromSmi(0);
                break;
            }
            case Opcode::LdaSmi: {
                int32_t imm = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                acc_ = TaggedValue::FromSmi(imm);
                break;
            }
            case Opcode::LdaUndefined: {
                acc_ = TaggedValue::Undefined();
                break;
            }
            case Opcode::LdaNull: {
                acc_ = TaggedValue::Null();
                break;
            }
            case Opcode::LdaTrue: {
                acc_ = TaggedValue::True();
                break;
            }
            case Opcode::LdaFalse: {
                acc_ = TaggedValue::False();
                break;
            }
            case Opcode::Ldar: {
                uint8_t reg = *pc++;
                acc_ = frame.GetRegister(reg);
                break;
            }
            case Opcode::Star: {
                uint8_t reg = *pc++;
                frame.SetRegister(reg, acc_);
                break;
            }
            case Opcode::Mov: {
                uint8_t src = *pc++;
                uint8_t dst = *pc++;
                frame.SetRegister(dst, frame.GetRegister(src));
                break;
            }
            case Opcode::Add: {
                uint8_t reg = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;

                TaggedValue rhs = frame.GetRegister(reg);
                if (slot_id != 0) {
                    FeedbackSlot& slot = bytecode.GetFeedbackVector().GetSlot(slot_id);
                    slot.RecordType(acc_);
                    slot.RecordType(rhs);
                }

                // Fast path: Both are Smi
                if (AETHER_LIKELY(acc_.IsSmi() && rhs.IsSmi())) {
                    int64_t sum = static_cast<int64_t>(acc_.AsSmi()) + rhs.AsSmi();
                    if (AETHER_LIKELY(sum <= INT32_MAX && sum >= INT32_MIN)) {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(sum));
                    } else {
                        acc_ = TaggedValue::FromDouble(static_cast<double>(sum));
                    }
                } else if (acc_.IsNumber() && rhs.IsNumber()) {
                    acc_ = TaggedValue::FromDouble(acc_.AsNumber() + rhs.AsNumber());
                } else {
                    // String concatenation
                    std::string s1 = acc_.ToString();
                    std::string s2 = rhs.ToString();
                    acc_ = TaggedValue::FromHeapObject(new JSString(s1 + s2));
                }
                break;
            }
            case Opcode::Sub: {
                uint8_t reg = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;

                TaggedValue rhs = frame.GetRegister(reg);
                if (slot_id != 0) {
                    FeedbackSlot& slot = bytecode.GetFeedbackVector().GetSlot(slot_id);
                    slot.RecordType(acc_);
                    slot.RecordType(rhs);
                }

                if (AETHER_LIKELY(acc_.IsSmi() && rhs.IsSmi())) {
                    int64_t diff = static_cast<int64_t>(acc_.AsSmi()) - rhs.AsSmi();
                    if (AETHER_LIKELY(diff <= INT32_MAX && diff >= INT32_MIN)) {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(diff));
                    } else {
                        acc_ = TaggedValue::FromDouble(static_cast<double>(diff));
                    }
                } else if (acc_.IsNumber() && rhs.IsNumber()) {
                    acc_ = TaggedValue::FromDouble(acc_.AsNumber() - rhs.AsNumber());
                }
                break;
            }
            case Opcode::Mul: {
                uint8_t reg = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;

                TaggedValue rhs = frame.GetRegister(reg);
                if (slot_id != 0) {
                    FeedbackSlot& slot = bytecode.GetFeedbackVector().GetSlot(slot_id);
                    slot.RecordType(acc_);
                    slot.RecordType(rhs);
                }

                if (AETHER_LIKELY(acc_.IsSmi() && rhs.IsSmi())) {
                    int64_t prod = static_cast<int64_t>(acc_.AsSmi()) * rhs.AsSmi();
                    if (AETHER_LIKELY(prod <= INT32_MAX && prod >= INT32_MIN && !(prod == 0 && (acc_.AsSmi() < 0 || rhs.AsSmi() < 0)))) {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(prod));
                    } else {
                        acc_ = TaggedValue::FromDouble(static_cast<double>(prod));
                    }
                } else if (acc_.IsNumber() && rhs.IsNumber()) {
                    acc_ = TaggedValue::FromDouble(acc_.AsNumber() * rhs.AsNumber());
                }
                break;
            }
            case Opcode::Div: {
                uint8_t reg = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;

                TaggedValue rhs = frame.GetRegister(reg);
                if (AETHER_LIKELY(acc_.IsSmi() && rhs.IsSmi() && rhs.AsSmi() != 0)) {
                    int32_t n = acc_.AsSmi();
                    int32_t d = rhs.AsSmi();
                    if (n % d == 0 && !(n == 0 && d < 0)) {
                        acc_ = TaggedValue::FromSmi(n / d);
                    } else {
                        acc_ = TaggedValue::FromDouble(static_cast<double>(n) / static_cast<double>(d));
                    }
                } else if (acc_.IsNumber() && rhs.IsNumber()) {
                    acc_ = TaggedValue::FromDouble(acc_.AsNumber() / rhs.AsNumber());
                }
                break;
            }
            case Opcode::Mod: {
                uint8_t reg = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;

                TaggedValue rhs = frame.GetRegister(reg);
                if (AETHER_LIKELY(acc_.IsSmi() && rhs.IsSmi() && rhs.AsSmi() != 0)) {
                    acc_ = TaggedValue::FromSmi(acc_.AsSmi() % rhs.AsSmi());
                } else if (acc_.IsNumber() && rhs.IsNumber()) {
                    acc_ = TaggedValue::FromDouble(std::fmod(acc_.AsNumber(), rhs.AsNumber()));
                }
                break;
            }
            case Opcode::BitwiseAnd: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                int32_t a = acc_.IsSmi() ? acc_.AsSmi() : static_cast<int32_t>(acc_.AsNumber());
                int32_t b = rhs.IsSmi() ? rhs.AsSmi() : static_cast<int32_t>(rhs.AsNumber());
                acc_ = TaggedValue::FromSmi(a & b);
                break;
            }
            case Opcode::BitwiseOr: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                int32_t a = acc_.IsSmi() ? acc_.AsSmi() : static_cast<int32_t>(acc_.AsNumber());
                int32_t b = rhs.IsSmi() ? rhs.AsSmi() : static_cast<int32_t>(rhs.AsNumber());
                acc_ = TaggedValue::FromSmi(a | b);
                break;
            }
            case Opcode::BitwiseXor: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                int32_t a = acc_.IsSmi() ? acc_.AsSmi() : static_cast<int32_t>(acc_.AsNumber());
                int32_t b = rhs.IsSmi() ? rhs.AsSmi() : static_cast<int32_t>(rhs.AsNumber());
                acc_ = TaggedValue::FromSmi(a ^ b);
                break;
            }
            case Opcode::ShiftLeft: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                int32_t a = acc_.IsSmi() ? acc_.AsSmi() : static_cast<int32_t>(acc_.AsNumber());
                uint32_t shift = (rhs.IsSmi() ? rhs.AsSmi() : static_cast<int32_t>(rhs.AsNumber())) & 0x1F;
                acc_ = TaggedValue::FromSmi(a << shift);
                break;
            }
            case Opcode::ShiftRight: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                int32_t a = acc_.IsSmi() ? acc_.AsSmi() : static_cast<int32_t>(acc_.AsNumber());
                uint32_t shift = (rhs.IsSmi() ? rhs.AsSmi() : static_cast<int32_t>(rhs.AsNumber())) & 0x1F;
                acc_ = TaggedValue::FromSmi(a >> shift);
                break;
            }
            case Opcode::TestEqual:
            case Opcode::TestStrictEqual: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                if (acc_.IsSmi() && rhs.IsSmi()) {
                    acc_ = TaggedValue::FromBoolean(acc_.AsSmi() == rhs.AsSmi());
                } else if (acc_.IsNumber() && rhs.IsNumber()) {
                    acc_ = TaggedValue::FromBoolean(acc_.AsNumber() == rhs.AsNumber());
                } else if (acc_.IsHeapObject() && rhs.IsHeapObject() &&
                           acc_.AsHeapObject()->IsString() && rhs.AsHeapObject()->IsString()) {
                    auto* s1 = static_cast<JSString*>(acc_.AsHeapObject());
                    auto* s2 = static_cast<JSString*>(rhs.AsHeapObject());
                    acc_ = TaggedValue::FromBoolean(s1->View() == s2->View());
                } else {
                    acc_ = TaggedValue::FromBoolean(acc_ == rhs);
                }
                break;
            }
            case Opcode::TestLessThan: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                if (AETHER_LIKELY(acc_.IsSmi() && rhs.IsSmi())) {
                    acc_ = TaggedValue::FromBoolean(acc_.AsSmi() < rhs.AsSmi());
                } else if (acc_.IsNumber() && rhs.IsNumber()) {
                    acc_ = TaggedValue::FromBoolean(acc_.AsNumber() < rhs.AsNumber());
                }
                break;
            }
            case Opcode::TestGreaterThan: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                if (AETHER_LIKELY(acc_.IsSmi() && rhs.IsSmi())) {
                    acc_ = TaggedValue::FromBoolean(acc_.AsSmi() > rhs.AsSmi());
                } else if (acc_.IsNumber() && rhs.IsNumber()) {
                    acc_ = TaggedValue::FromBoolean(acc_.AsNumber() > rhs.AsNumber());
                }
                break;
            }
            case Opcode::TestLessThanOrEqual: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                if (AETHER_LIKELY(acc_.IsSmi() && rhs.IsSmi())) {
                    acc_ = TaggedValue::FromBoolean(acc_.AsSmi() <= rhs.AsSmi());
                } else if (acc_.IsNumber() && rhs.IsNumber()) {
                    acc_ = TaggedValue::FromBoolean(acc_.AsNumber() <= rhs.AsNumber());
                }
                break;
            }
            case Opcode::TestGreaterThanOrEqual: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                if (AETHER_LIKELY(acc_.IsSmi() && rhs.IsSmi())) {
                    acc_ = TaggedValue::FromBoolean(acc_.AsSmi() >= rhs.AsSmi());
                } else if (acc_.IsNumber() && rhs.IsNumber()) {
                    acc_ = TaggedValue::FromBoolean(acc_.AsNumber() >= rhs.AsNumber());
                }
                break;
            }
            case Opcode::TestInstanceOf: {
                uint8_t reg = *pc++;
                TaggedValue rhs = frame.GetRegister(reg);
                if (!acc_.IsHeapObject() || !rhs.IsHeapObject()) {
                    acc_ = TaggedValue::FromBoolean(false);
                    break;
                }
                TaggedValue target_proto = TaggedValue::Undefined();
                HeapObject* rhs_obj = rhs.AsHeapObject();
                if (rhs_obj->IsJSObject()) {
                    target_proto = static_cast<JSObject*>(rhs_obj)->GetNamedProperty("prototype");
                } else if (rhs_obj->IsFunction()) {
                    auto* fn = static_cast<JSFunction*>(rhs_obj);
                    if (!fn->GetPrototype()) {
                        fn->SetPrototype(new JSObject(GetRootShape()));
                    }
                    target_proto = TaggedValue::FromHeapObject(fn->GetPrototype());
                } else if (rhs_obj->IsNativeFunction()) {
                    target_proto = static_cast<JSNativeFunction*>(rhs_obj)->GetNamedProperty("prototype");
                }

                JSObject* curr = nullptr;
                if (acc_.AsHeapObject()->IsJSObject()) {
                    curr = static_cast<JSObject*>(acc_.AsHeapObject())->GetPrototype();
                }
                bool found = false;
                while (curr) {
                    if (TaggedValue::FromHeapObject(curr) == target_proto) {
                        found = true;
                        break;
                    }
                    curr = curr->GetPrototype();
                }
                acc_ = TaggedValue::FromBoolean(found);
                break;
            }
            case Opcode::Jump: {
                int32_t offset = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                pc += offset;
                break;
            }
            case Opcode::JumpIfTrue: {
                int32_t offset = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                if (acc_.ToBoolean()) {
                    pc += offset;
                }
                break;
            }
            case Opcode::JumpIfFalse: {
                int32_t offset = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                if (!acc_.ToBoolean()) {
                    pc += offset;
                }
                break;
            }
            case Opcode::JumpIfNotNullish: {
                int32_t offset = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                if (!acc_.IsNull() && !acc_.IsUndefined()) {
                    pc += offset;
                }
                break;
            }
            case Opcode::JumpIfNullish: {
                int32_t offset = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                if (acc_.IsNull() || acc_.IsUndefined()) {
                    pc += offset;
                }
                break;
            }
            case Opcode::LdaNamed: {
                uint8_t obj_reg = *pc++;
                uint16_t name_idx = pc[0] | (pc[1] << 8);
                pc += 2;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;

                TaggedValue obj_val = frame.GetRegister(obj_reg);
                assert(obj_val.IsHeapObject());
                TaggedValue prop_name_val = bytecode.Constants()[name_idx];
                assert(prop_name_val.IsHeapObject() && prop_name_val.AsHeapObject()->IsString());
                std::string_view prop_key = static_cast<JSString*>(prop_name_val.AsHeapObject())->View();

                if (obj_val.AsHeapObject()->GetType() == HeapObjectType::JSFunction) {
                    auto* fn = static_cast<JSFunction*>(obj_val.AsHeapObject());
                    if (prop_key == "prototype") {
                        if (!fn->GetPrototype()) {
                            fn->SetPrototype(new JSObject(GetRootShape()));
                        }
                        acc_ = TaggedValue::FromHeapObject(fn->GetPrototype());
                        break;
                    }
                }

                if (obj_val.AsHeapObject()->IsString()) {
                    auto* str_obj = static_cast<JSString*>(obj_val.AsHeapObject());
                    if (prop_key == "length") {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(str_obj->Length()));
                        break;
                    }
                    if (Realm::Current() && Realm::Current()->GetStringPrototype()) {
                        acc_ = Realm::Current()->GetStringPrototype()->GetNamedProperty(prop_key);
                        break;
                    }
                    acc_ = TaggedValue::Undefined();
                    break;
                }

                if (obj_val.AsHeapObject()->IsTypedArray()) {
                    auto* t_arr = static_cast<JSTypedArray*>(obj_val.AsHeapObject());
                    if (prop_key == "length") {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(t_arr->Length()));
                        break;
                    }
                    if (prop_key == "byteLength") {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(t_arr->ByteLength()));
                        break;
                    }
                    if (prop_key == "byteOffset") {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(t_arr->ByteOffset()));
                        break;
                    }
                    if (prop_key == "buffer") {
                        acc_ = t_arr->Buffer() ? TaggedValue::FromHeapObject(t_arr->Buffer()) : TaggedValue::Undefined();
                        break;
                    }
                    if (prop_key == "BYTES_PER_ELEMENT") {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(t_arr->BytesPerElement()));
                        break;
                    }
                }

                if (obj_val.AsHeapObject()->IsArrayBuffer()) {
                    auto* ab = static_cast<JSArrayBuffer*>(obj_val.AsHeapObject());
                    if (prop_key == "byteLength") {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(ab->ByteLength()));
                        break;
                    }
                }

                if (obj_val.AsHeapObject()->IsDataView()) {
                    auto* dv = static_cast<JSDataView*>(obj_val.AsHeapObject());
                    if (prop_key == "byteLength") {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(dv->ByteLength()));
                        break;
                    }
                    if (prop_key == "byteOffset") {
                        acc_ = TaggedValue::FromSmi(static_cast<int32_t>(dv->ByteOffset()));
                        break;
                    }
                    if (prop_key == "buffer") {
                        acc_ = dv->Buffer() ? TaggedValue::FromHeapObject(dv->Buffer()) : TaggedValue::Undefined();
                        break;
                    }
                }

                assert(obj_val.AsHeapObject()->IsJSObject());
                JSObject* js_obj = static_cast<JSObject*>(obj_val.AsHeapObject());

                // Inline Cache check
                FeedbackSlot& ic_slot = bytecode.GetFeedbackVector().GetSlot(slot_id);
                Shape* obj_shape = js_obj->GetShape();

                if (AETHER_LIKELY(ic_slot.state == FeedbackSlot::State::Monomorphic && ic_slot.cached_shape == obj_shape)) {
                    // Monomorphic IC Hit: Direct O(1) slot read
                    acc_ = js_obj->GetFastSlot(ic_slot.cached_slot, ic_slot.cached_in_object);
                } else {
                    // IC Miss: Slow path lookup and IC transition
                    acc_ = js_obj->GetNamedProperty(prop_key);

                    // Update IC to Monomorphic
                    if (const PropertyDescriptor* desc = obj_shape->LookupProperty(prop_key)) {
                        ic_slot.state = FeedbackSlot::State::Monomorphic;
                        ic_slot.cached_shape = obj_shape;
                        ic_slot.cached_slot = desc->slot_index;
                        ic_slot.cached_in_object = desc->is_in_object;
                    }
                }
                break;
            }
            case Opcode::StaNamed: {
                uint8_t obj_reg = *pc++;
                uint16_t name_idx = pc[0] | (pc[1] << 8);
                pc += 2;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;

                TaggedValue obj_val = frame.GetRegister(obj_reg);
                assert(obj_val.IsHeapObject());
                TaggedValue prop_name_val = bytecode.Constants()[name_idx];
                assert(prop_name_val.IsHeapObject() && prop_name_val.AsHeapObject()->IsString());
                std::string_view prop_key = static_cast<JSString*>(prop_name_val.AsHeapObject())->View();

                if (obj_val.AsHeapObject()->GetType() == HeapObjectType::JSFunction) {
                    auto* fn = static_cast<JSFunction*>(obj_val.AsHeapObject());
                    if (prop_key == "prototype" && acc_.IsHeapObject() && acc_.AsHeapObject()->IsJSObject()) {
                        fn->SetPrototype(static_cast<JSObject*>(acc_.AsHeapObject()));
                        break;
                    }
                }

                if (obj_val.AsHeapObject()->IsString()) {
                    // String primitives are immutable in ECMAScript
                    break;
                }

                assert(obj_val.AsHeapObject()->IsJSObject());
                JSObject* js_obj = static_cast<JSObject*>(obj_val.AsHeapObject());

                // Inline Cache check
                FeedbackSlot& ic_slot = bytecode.GetFeedbackVector().GetSlot(slot_id);
                Shape* obj_shape = js_obj->GetShape();

                if (AETHER_LIKELY(ic_slot.state == FeedbackSlot::State::Monomorphic && ic_slot.cached_shape == obj_shape)) {
                    // Monomorphic IC Hit: Direct O(1) slot write
                    js_obj->SetFastSlot(ic_slot.cached_slot, ic_slot.cached_in_object, acc_);
                } else {
                    // IC Miss: Set property and update Shape / IC
                    js_obj->SetNamedProperty(prop_key, acc_, next_shape_id_);

                    Shape* new_shape = js_obj->GetShape();
                    if (const PropertyDescriptor* desc = new_shape->LookupProperty(prop_key)) {
                        ic_slot.state = FeedbackSlot::State::Monomorphic;
                        ic_slot.cached_shape = new_shape;
                        ic_slot.cached_slot = desc->slot_index;
                        ic_slot.cached_in_object = desc->is_in_object;
                    }
                }
                break;
            }
            case Opcode::LogicalNot: {
                acc_ = TaggedValue::FromBoolean(!acc_.ToBoolean());
                break;
            }
            case Opcode::Negate: {
                if (AETHER_LIKELY(acc_.IsSmi())) {
                    int32_t val = acc_.AsSmi();
                    if (AETHER_LIKELY(val != INT32_MIN)) {
                        acc_ = TaggedValue::FromSmi(-val);
                    } else {
                        acc_ = TaggedValue::FromDouble(-static_cast<double>(val));
                    }
                } else if (acc_.IsDouble()) {
                    acc_ = TaggedValue::FromDouble(-acc_.AsDouble());
                }
                break;
            }
            case Opcode::BitwiseNot: {
                int32_t val = acc_.IsSmi() ? acc_.AsSmi() : static_cast<int32_t>(acc_.AsNumber());
                acc_ = TaggedValue::FromSmi(~val);
                break;
            }
            case Opcode::TypeOf: {
                if (acc_.IsSmi() || acc_.IsDouble()) {
                    acc_ = TaggedValue::FromHeapObject(new JSString("number"));
                } else if (acc_.IsBoolean()) {
                    acc_ = TaggedValue::FromHeapObject(new JSString("boolean"));
                } else if (acc_.IsUndefined()) {
                    acc_ = TaggedValue::FromHeapObject(new JSString("undefined"));
                } else if (acc_.IsNull()) {
                    acc_ = TaggedValue::FromHeapObject(new JSString("object"));
                } else if (acc_.IsSymbol()) {
                    acc_ = TaggedValue::FromHeapObject(new JSString("symbol"));
                } else if (acc_.IsHeapObject()) {
                    auto* h = acc_.AsHeapObject();
                    if (h->IsString()) {
                        acc_ = TaggedValue::FromHeapObject(new JSString("string"));
                    } else if (h->IsFunction() || h->IsNativeFunction()) {
                        acc_ = TaggedValue::FromHeapObject(new JSString("function"));
                    } else {
                        acc_ = TaggedValue::FromHeapObject(new JSString("object"));
                    }
                } else {
                    acc_ = TaggedValue::FromHeapObject(new JSString("undefined"));
                }
                break;
            }
            case Opcode::CreateObject: {
                auto* obj = new JSObject(GetRootShape());
                acc_ = TaggedValue::FromHeapObject(obj);
                break;
            }
            case Opcode::CreateArray: {
                uint8_t start_reg = *pc++;
                uint16_t count = pc[0] | (pc[1] << 8);
                pc += 2;
                auto* arr = new JSArray(GetRootShape(), static_cast<size_t>(count));
                if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
                    arr->SetPrototype(Realm::Current()->GetArrayPrototype());
                }
                for (uint16_t i = 0; i < count; ++i) {
                    arr->SetElement(i, frame.GetRegister(static_cast<uint8_t>(start_reg + i)));
                }
                acc_ = TaggedValue::FromHeapObject(arr);
                break;
            }
            case Opcode::ArrayPush: {
                uint8_t arr_reg = *pc++;
                uint8_t val_reg = *pc++;
                TaggedValue arr_val = frame.GetRegister(arr_reg);
                TaggedValue val = frame.GetRegister(val_reg);
                if (arr_val.IsHeapObject() && arr_val.AsHeapObject()->IsArray()) {
                    static_cast<JSArray*>(arr_val.AsHeapObject())->Push(val);
                }
                break;
            }
            case Opcode::ArraySpread: {
                uint8_t target_arr_reg = *pc++;
                uint8_t src_reg = *pc++;
                TaggedValue target_val = frame.GetRegister(target_arr_reg);
                TaggedValue src_val = frame.GetRegister(src_reg);
                if (target_val.IsHeapObject() && target_val.AsHeapObject()->IsArray()) {
                    auto* target_arr = static_cast<JSArray*>(target_val.AsHeapObject());
                    if (src_val.IsHeapObject()) {
                        HeapObject* src_obj = src_val.AsHeapObject();
                        if (src_obj->IsArray()) {
                            auto* src_arr = static_cast<JSArray*>(src_obj);
                            for (size_t i = 0; i < src_arr->Length(); ++i) {
                                target_arr->Push(src_arr->GetElement(i));
                            }
                        } else if (src_obj->IsTypedArray()) {
                            auto* src_ta = static_cast<JSTypedArray*>(src_obj);
                            for (size_t i = 0; i < src_ta->Length(); ++i) {
                                target_arr->Push(src_ta->GetElement(i));
                            }
                        } else if (src_obj->IsString()) {
                            auto* src_str = static_cast<JSString*>(src_obj);
                            for (size_t i = 0; i < src_str->Length(); ++i) {
                                target_arr->Push(TaggedValue::FromHeapObject(new JSString(src_str->View().substr(i, 1))));
                            }
                        }
                    }
                }
                break;
            }
            case Opcode::ObjectSpread: {
                uint8_t target_reg = *pc++;
                uint8_t src_reg = *pc++;
                TaggedValue target_val = frame.GetRegister(target_reg);
                TaggedValue src_val = frame.GetRegister(src_reg);
                if (target_val.IsHeapObject() && target_val.AsHeapObject()->IsJSObject() &&
                    src_val.IsHeapObject() && src_val.AsHeapObject()->IsJSObject()) {
                    auto* target_obj = static_cast<JSObject*>(target_val.AsHeapObject());
                    auto* source_obj = static_cast<JSObject*>(src_val.AsHeapObject());
                    const Shape* s = source_obj->GetShape();
                    if (s) {
                        for (const auto& [prop_name, desc] : s->GetPropertyMap()) {
                            TaggedValue val = source_obj->GetNamedProperty(prop_name);
                            target_obj->SetNamedProperty(prop_name, val, next_shape_id_);
                        }
                    }
                }
                break;
            }
            case Opcode::CreateRestArray: {
                uint8_t dest_reg = *pc++;
                uint16_t start_idx = pc[0] | (pc[1] << 8);
                pc += 2;
                size_t count = (frame.Args().size() > start_idx) ? (frame.Args().size() - start_idx) : 0;
                auto* arr = new JSArray(GetRootShape(), count);
                if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
                    arr->SetPrototype(Realm::Current()->GetArrayPrototype());
                }
                for (size_t i = 0; i < count; ++i) {
                    arr->SetElement(i, frame.Args()[start_idx + i]);
                }
                frame.SetRegister(dest_reg, TaggedValue::FromHeapObject(arr));
                break;
            }
            case Opcode::LdaKeyed: {
                uint8_t obj_reg = *pc++;
                uint8_t key_reg = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;

                TaggedValue obj_val = frame.GetRegister(obj_reg);
                TaggedValue key_val = frame.GetRegister(key_reg);

                if (!obj_val.IsHeapObject()) {
                    acc_ = TaggedValue::Undefined();
                    break;
                }

                if (key_val.IsSymbol()) {
                    auto* sym = static_cast<JSSymbol*>(key_val.AsHeapObject());
                    HeapObject* ho = obj_val.AsHeapObject();
                    if (ho->IsJSObject()) {
                        acc_ = static_cast<JSObject*>(ho)->GetNamedProperty(sym->Key());
                    } else if (ho->IsString()) {
                        if (Realm::Current() && Realm::Current()->GetStringPrototype()) {
                            acc_ = Realm::Current()->GetStringPrototype()->GetNamedProperty(sym->Key());
                        } else {
                            acc_ = TaggedValue::Undefined();
                        }
                    } else {
                        acc_ = TaggedValue::Undefined();
                    }
                    break;
                }

                HeapObject* h_obj = obj_val.AsHeapObject();
                if (h_obj->IsArray()) {
                    auto* arr = static_cast<JSArray*>(h_obj);
                    if (key_val.IsSmi()) {
                        int32_t idx = key_val.AsSmi();
                        acc_ = idx >= 0 ? arr->GetElement(static_cast<size_t>(idx)) : TaggedValue::Undefined();
                    } else if (key_val.IsNumber()) {
                        double d = key_val.AsNumber();
                        acc_ = (d >= 0.0 && std::floor(d) == d) ? arr->GetElement(static_cast<size_t>(d)) : TaggedValue::Undefined();
                    } else if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
                        acc_ = arr->GetNamedProperty(static_cast<JSString*>(key_val.AsHeapObject())->View());
                    } else {
                        acc_ = TaggedValue::Undefined();
                    }
                } else if (h_obj->IsString()) {
                    auto* str_obj = static_cast<JSString*>(h_obj);
                    int32_t idx = -1;
                    if (key_val.IsSmi()) idx = key_val.AsSmi();
                    else if (key_val.IsNumber()) {
                        double d = key_val.AsNumber();
                        if (d >= 0.0 && std::floor(d) == d) idx = static_cast<int32_t>(d);
                    }
                    if (idx >= 0 && static_cast<size_t>(idx) < str_obj->Length()) {
                        acc_ = TaggedValue::FromHeapObject(new JSString(str_obj->View().substr(idx, 1)));
                    } else if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
                        std::string_view k = static_cast<JSString*>(key_val.AsHeapObject())->View();
                        if (k == "length") {
                            acc_ = TaggedValue::FromSmi(static_cast<int32_t>(str_obj->Length()));
                        } else if (Realm::Current() && Realm::Current()->GetStringPrototype()) {
                            acc_ = Realm::Current()->GetStringPrototype()->GetNamedProperty(k);
                        } else {
                            acc_ = TaggedValue::Undefined();
                        }
                    } else {
                        acc_ = TaggedValue::Undefined();
                    }
                } else if (h_obj->IsTypedArray()) {
                    auto* t_arr = static_cast<JSTypedArray*>(h_obj);
                    if (key_val.IsSmi()) {
                        int32_t idx = key_val.AsSmi();
                        acc_ = idx >= 0 ? t_arr->GetElement(static_cast<size_t>(idx)) : TaggedValue::Undefined();
                    } else if (key_val.IsNumber()) {
                        double d = key_val.AsNumber();
                        acc_ = (d >= 0.0 && std::floor(d) == d) ? t_arr->GetElement(static_cast<size_t>(d)) : TaggedValue::Undefined();
                    } else if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
                        std::string_view k = static_cast<JSString*>(key_val.AsHeapObject())->View();
                        if (k == "length") {
                            acc_ = TaggedValue::FromSmi(static_cast<int32_t>(t_arr->Length()));
                        } else if (k == "byteLength") {
                            acc_ = TaggedValue::FromSmi(static_cast<int32_t>(t_arr->ByteLength()));
                        } else if (k == "byteOffset") {
                            acc_ = TaggedValue::FromSmi(static_cast<int32_t>(t_arr->ByteOffset()));
                        } else if (k == "buffer") {
                            acc_ = t_arr->Buffer() ? TaggedValue::FromHeapObject(t_arr->Buffer()) : TaggedValue::Undefined();
                        } else if (k == "BYTES_PER_ELEMENT") {
                            acc_ = TaggedValue::FromSmi(static_cast<int32_t>(t_arr->BytesPerElement()));
                        } else {
                            acc_ = t_arr->GetNamedProperty(k);
                        }
                    } else {
                        acc_ = TaggedValue::Undefined();
                    }
                } else if (h_obj->IsJSObject()) {
                    auto* js_obj = static_cast<JSObject*>(h_obj);
                    if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
                        acc_ = js_obj->GetNamedProperty(static_cast<JSString*>(key_val.AsHeapObject())->View());
                    } else if (key_val.IsSmi()) {
                        acc_ = js_obj->GetNamedProperty(std::to_string(key_val.AsSmi()));
                    } else {
                        acc_ = js_obj->GetNamedProperty(key_val.ToString());
                    }
                } else {
                    acc_ = TaggedValue::Undefined();
                }
                break;
            }
            case Opcode::StaKeyed: {
                uint8_t obj_reg = *pc++;
                uint8_t key_reg = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;

                TaggedValue obj_val = frame.GetRegister(obj_reg);
                TaggedValue key_val = frame.GetRegister(key_reg);

                if (obj_val.IsHeapObject()) {
                    if (key_val.IsSymbol()) {
                        auto* sym = static_cast<JSSymbol*>(key_val.AsHeapObject());
                        HeapObject* ho = obj_val.AsHeapObject();
                        if (ho->IsJSObject()) {
                            static_cast<JSObject*>(ho)->SetNamedProperty(sym->Key(), acc_, next_shape_id_);
                        }
                        break;
                    }
                    HeapObject* h_obj = obj_val.AsHeapObject();
                    if (h_obj->IsArray()) {
                        auto* arr = static_cast<JSArray*>(h_obj);
                        if (key_val.IsSmi()) {
                            int32_t idx = key_val.AsSmi();
                            if (idx >= 0) arr->SetElement(static_cast<size_t>(idx), acc_);
                        } else if (key_val.IsNumber()) {
                            double d = key_val.AsNumber();
                            if (d >= 0.0 && std::floor(d) == d) arr->SetElement(static_cast<size_t>(d), acc_);
                        } else if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
                            arr->SetNamedProperty(static_cast<JSString*>(key_val.AsHeapObject())->View(), acc_, next_shape_id_);
                        }
                    } else if (h_obj->IsTypedArray()) {
                        auto* t_arr = static_cast<JSTypedArray*>(h_obj);
                        if (key_val.IsSmi()) {
                            int32_t idx = key_val.AsSmi();
                            if (idx >= 0) t_arr->SetElement(static_cast<size_t>(idx), acc_);
                        } else if (key_val.IsNumber()) {
                            double d = key_val.AsNumber();
                            if (d >= 0.0 && std::floor(d) == d) t_arr->SetElement(static_cast<size_t>(d), acc_);
                        } else if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
                            t_arr->SetNamedProperty(static_cast<JSString*>(key_val.AsHeapObject())->View(), acc_, next_shape_id_);
                        }
                    } else if (h_obj->IsJSObject()) {
                        auto* js_obj = static_cast<JSObject*>(h_obj);
                        if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
                            js_obj->SetNamedProperty(static_cast<JSString*>(key_val.AsHeapObject())->View(), acc_, next_shape_id_);
                        } else if (key_val.IsSmi()) {
                            js_obj->SetNamedProperty(std::to_string(key_val.AsSmi()), acc_, next_shape_id_);
                        } else {
                            js_obj->SetNamedProperty(key_val.ToString(), acc_, next_shape_id_);
                        }
                    }
                }
                break;
            }
            case Opcode::LdaThis: {
                acc_ = frame.GetThis();
                break;
            }
            case Opcode::CreateClosure: {
                uint16_t const_idx = pc[0] | (pc[1] << 8);
                pc += 2;
                TaggedValue proto_val = bytecode.Constants()[const_idx];
                assert(proto_val.IsHeapObject() && proto_val.AsHeapObject()->IsFunction());
                auto* proto_fn = static_cast<JSFunction*>(proto_val.AsHeapObject());
                auto* closure = new JSFunction(proto_fn->Name(), 
                                               proto_fn->GetSharedBytecode() ? proto_fn->GetSharedBytecode() : std::shared_ptr<const BytecodeArray>(proto_fn->GetBytecode(), [](const BytecodeArray*){}), 
                                               frame.GetEnvironment());
                closure->SetPrototype(proto_fn->GetPrototype());
                closure->SetGenerator(proto_fn->IsGenerator());
                closure->SetAsync(proto_fn->IsAsync());
                acc_ = TaggedValue::FromHeapObject(closure);
                break;
            }
            case Opcode::CreateFunctionContext: {
                uint16_t slot_count = pc[0] | (pc[1] << 8);
                pc += 2;
                auto* new_env = new LexicalEnvironment(frame.GetEnvironment(), slot_count);
                frame.SetEnvironment(new_env);
                last_environment_ = new_env;
                break;
            }
            case Opcode::LdaContextSlot: {
                uint8_t depth = *pc++;
                uint16_t slot = pc[0] | (pc[1] << 8);
                pc += 2;
                LexicalEnvironment* cur_env = frame.GetEnvironment();
                acc_ = cur_env ? cur_env->Get(depth, slot) : TaggedValue::Undefined();
                break;
            }
            case Opcode::StaContextSlot: {
                uint8_t depth = *pc++;
                uint16_t slot = pc[0] | (pc[1] << 8);
                pc += 2;
                LexicalEnvironment* cur_env = frame.GetEnvironment();
                if (cur_env) {
                    cur_env->Set(depth, slot, acc_);
                }
                break;
            }
            case Opcode::Construct: {
                uint8_t ctor_reg = *pc++;
                uint8_t args_start = *pc++;
                uint8_t arg_count = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;

                TaggedValue ctor_val = frame.GetRegister(ctor_reg);
                if (!ctor_val.IsHeapObject() || !ctor_val.AsHeapObject()->IsFunction()) {
                    TaggedValue exc = TaggedValue::FromHeapObject(new JSError("TypeError", "value is not a constructor"));
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = exc;
                    } else {
                        has_pending_exception_ = true;
                        pending_exception_ = exc;
                        return exc;
                    }
                    break;
                }

                std::vector<TaggedValue> call_args;
                call_args.reserve(arg_count);
                for (uint8_t i = 0; i < arg_count; ++i) {
                    call_args.push_back(frame.GetRegister(static_cast<uint8_t>(args_start + i)));
                }

                if (ctor_val.AsHeapObject()->IsNativeFunction()) {
                    auto* native_fn = static_cast<JSNativeFunction*>(ctor_val.AsHeapObject());
                    acc_ = native_fn->Invoke(TaggedValue::Undefined(), call_args);
                    break;
                }

                auto* ctor_fn = static_cast<JSFunction*>(ctor_val.AsHeapObject());
                if (!ctor_fn->GetPrototype()) {
                    ctor_fn->SetPrototype(new JSObject(GetRootShape()));
                }
                auto* instance = new JSObject(GetRootShape());
                instance->SetPrototype(ctor_fn->GetPrototype());
                TaggedValue instance_val = TaggedValue::FromHeapObject(instance);

                TaggedValue ret = Execute(*ctor_fn->GetBytecode(), call_args, instance_val, ctor_fn->GetEnvironment());
                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                } else {
                    if (ret.IsHeapObject() && ret.AsHeapObject()->IsJSObject()) {
                        acc_ = ret;
                    } else {
                        acc_ = instance_val;
                    }
                }
                break;
            }
            case Opcode::CallMethod: {
                uint8_t callee_reg = *pc++;
                uint8_t this_reg = *pc++;
                uint8_t args_start = *pc++;
                uint8_t arg_count = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;

                TaggedValue callee_val = frame.GetRegister(callee_reg);
                TaggedValue rec_this = frame.GetRegister(this_reg);
                assert(callee_val.IsHeapObject() && callee_val.AsHeapObject()->IsFunction() && "Callee must be a Function");

                std::vector<TaggedValue> call_args;
                call_args.reserve(arg_count);
                for (uint8_t i = 0; i < arg_count; ++i) {
                    call_args.push_back(frame.GetRegister(static_cast<uint8_t>(args_start + i)));
                }

                if (callee_val.AsHeapObject()->IsNativeFunction()) {
                    auto* native_fn = static_cast<JSNativeFunction*>(callee_val.AsHeapObject());
                    acc_ = native_fn->Invoke(rec_this, call_args);
                } else {
                    auto* func = static_cast<JSFunction*>(callee_val.AsHeapObject());
                    if (func->IsGenerator()) {
                        auto* gen_obj = new JSGeneratorObject(func, call_args, rec_this, func->GetEnvironment(), GetRootShape());
                        if (Realm::Current() && Realm::Current()->GetGeneratorPrototype()) {
                            gen_obj->SetPrototype(Realm::Current()->GetGeneratorPrototype());
                        }
                        acc_ = TaggedValue::FromHeapObject(gen_obj);
                    } else if (func->IsAsync()) {
                        auto* gen_obj = new JSGeneratorObject(func, call_args, rec_this, func->GetEnvironment(), GetRootShape());
                        auto* promise = new JSPromise(GetRootShape());
                        if (Realm::Current() && Realm::Current()->GetPromisePrototype()) {
                            promise->SetPrototype(Realm::Current()->GetPromisePrototype());
                        }
                        gen_obj->SetAsyncPromise(promise);
                        RunAsyncStep(gen_obj, TaggedValue::Undefined(), false);
                        acc_ = TaggedValue::FromHeapObject(promise);
                    } else {
                        acc_ = Execute(*func->GetBytecode(), call_args, rec_this, func->GetEnvironment());
                    }
                }

                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                }
                break;
            }
            case Opcode::Call: {
                uint8_t callee_reg = *pc++;
                uint8_t args_start = *pc++;
                uint8_t arg_count = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;
                (void)slot_id;

                TaggedValue callee_val = frame.GetRegister(callee_reg);
                assert(callee_val.IsHeapObject() && callee_val.AsHeapObject()->IsFunction() && "Callee must be a Function");

                std::vector<TaggedValue> call_args;
                call_args.reserve(arg_count);
                for (uint8_t i = 0; i < arg_count; ++i) {
                    call_args.push_back(frame.GetRegister(static_cast<uint8_t>(args_start + i)));
                }

                if (callee_val.AsHeapObject()->IsNativeFunction()) {
                    auto* native_fn = static_cast<JSNativeFunction*>(callee_val.AsHeapObject());
                    acc_ = native_fn->Invoke(TaggedValue::Undefined(), call_args);
                } else {
                    auto* func = static_cast<JSFunction*>(callee_val.AsHeapObject());
                    if (func->IsGenerator()) {
                        auto* gen_obj = new JSGeneratorObject(func, call_args, TaggedValue::Undefined(), func->GetEnvironment(), GetRootShape());
                        if (Realm::Current() && Realm::Current()->GetGeneratorPrototype()) {
                            gen_obj->SetPrototype(Realm::Current()->GetGeneratorPrototype());
                        }
                        acc_ = TaggedValue::FromHeapObject(gen_obj);
                    } else if (func->IsAsync()) {
                        auto* gen_obj = new JSGeneratorObject(func, call_args, TaggedValue::Undefined(), func->GetEnvironment(), GetRootShape());
                        auto* promise = new JSPromise(GetRootShape());
                        if (Realm::Current() && Realm::Current()->GetPromisePrototype()) {
                            promise->SetPrototype(Realm::Current()->GetPromisePrototype());
                        }
                        gen_obj->SetAsyncPromise(promise);
                        RunAsyncStep(gen_obj, TaggedValue::Undefined(), false);
                        acc_ = TaggedValue::FromHeapObject(promise);
                    } else {
                        acc_ = Execute(*func->GetBytecode(), call_args, TaggedValue::Undefined(), func->GetEnvironment());
                    }
                }

                if (has_pending_exception_) {
                    if (frame.HasExceptionHandler()) {
                        auto h = frame.PopExceptionHandler();
                        pc = h.target_pc;
                        frame.SetEnvironment(h.env);
                        acc_ = pending_exception_;
                        has_pending_exception_ = false;
                    } else {
                        return pending_exception_;
                    }
                }
                break;
            }
            case Opcode::PushExceptionHandler: {
                int32_t offset = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4;
                const uint8_t* target = pc + offset;
                frame.PushExceptionHandler(target, frame.GetEnvironment());
                break;
            }
            case Opcode::PopExceptionHandler: {
                frame.PopExceptionHandler();
                break;
            }
            case Opcode::Throw: {
                TaggedValue exc = acc_;
                if (frame.HasExceptionHandler()) {
                    auto h = frame.PopExceptionHandler();
                    pc = h.target_pc;
                    frame.SetEnvironment(h.env);
                    acc_ = exc;
                } else {
                    has_pending_exception_ = true;
                    pending_exception_ = exc;
                    if (frame.GetGenerator()) {
                        frame.GetGenerator()->SetState(JSGeneratorObject::State::Completed);
                    }
                    return exc;
                }
                break;
            }
            case Opcode::Await:
            case Opcode::Yield: {
                if (frame.GetGenerator()) {
                    auto* gen = frame.GetGenerator();
                    TaggedValue yielded_val = acc_;
                    gen->SetPCOffset(static_cast<uint32_t>(pc - code_start));
                    gen->SavedRegisters() = frame.GetRegisters();
                    gen->SetEnvironment(frame.GetEnvironment());
                    gen->SavedHandlers().clear();
                    for (const auto& h : frame.Handlers()) {
                        gen->SavedHandlers().push_back({static_cast<uint32_t>(h.target_pc - code_start), h.env});
                    }
                    gen->SetState(JSGeneratorObject::State::SuspendedYield);
                    if (out_yielded) *out_yielded = true;
                    return yielded_val;
                }
                acc_ = TaggedValue::Undefined();
                break;
            }
            case Opcode::Return: {
                if (frame.GetGenerator()) {
                    frame.GetGenerator()->SetState(JSGeneratorObject::State::Completed);
                }
                return acc_;
            }
            case Opcode::Halt: {
                if (frame.GetGenerator()) {
                    frame.GetGenerator()->SetState(JSGeneratorObject::State::Completed);
                }
                return acc_;
            }
            default:
                assert(false && "Unhandled bytecode opcode");
                return TaggedValue::Undefined();
        }
    }

    if (frame.GetGenerator()) {
        frame.GetGenerator()->SetState(JSGeneratorObject::State::Completed);
    }
    return acc_;
}

} // namespace aether
