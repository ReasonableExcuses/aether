#include "aether/interpreter.h"
#include "aether/baseline_jit.h"
#include "aether/runtime.h"
#include <cmath>

namespace aether {

TaggedValue Interpreter::Execute(const BytecodeArray& bytecode, const std::vector<TaggedValue>& args) {
    // 1. Direct dispatch to Tier 2 Optimizing JIT if already compiled
    if (const ExecutableMemory* t2 = bytecode.GetTier2Code()) {
        return BaselineJIT::Invoke(*t2, args.empty() ? nullptr : args.data(), static_cast<uint32_t>(args.size()));
    }

    // 2. Direct dispatch to Tier 1 Baseline JIT if already compiled
    if (const ExecutableMemory* t1 = bytecode.GetTier1Code()) {
        bytecode.IncrementInvocationCount();
        return BaselineJIT::Invoke(*t1, args.empty() ? nullptr : args.data(), static_cast<uint32_t>(args.size()));
    }

    // 3. Increment invocation profile counter; promote to Tier 1 when hot
    bytecode.IncrementInvocationCount();
    if (bytecode.InvocationCount() >= BaselineJIT::kInvocationThreshold && BaselineJIT::CanCompile(bytecode)) {
        auto jit_code = BaselineJIT::Compile(bytecode);
        if (jit_code) {
            bytecode.SetTier1Code(std::move(jit_code));
            return BaselineJIT::Invoke(*bytecode.GetTier1Code(), args.empty() ? nullptr : args.data(), static_cast<uint32_t>(args.size()));
        }
    }

    ExecutionFrame frame(bytecode, args);
    const uint8_t* code_start = bytecode.Code().data();
    const uint8_t* pc = code_start;
    const uint8_t* code_end = code_start + bytecode.Code().size();

    acc_ = TaggedValue::Undefined();

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
                    FeedbackSlot& slot = feedback_.GetSlot(slot_id);
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
                    // String concatenation fallback
                    std::string s1 = acc_.ToString();
                    std::string s2 = rhs.ToString();
                    // Heap string allocation in future GC milestone
                    acc_ = TaggedValue::Undefined();
                }
                break;
            }
            case Opcode::Sub: {
                uint8_t reg = *pc++;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;

                TaggedValue rhs = frame.GetRegister(reg);
                if (slot_id != 0) {
                    FeedbackSlot& slot = feedback_.GetSlot(slot_id);
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
                    FeedbackSlot& slot = feedback_.GetSlot(slot_id);
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
            case Opcode::LdaNamed: {
                uint8_t obj_reg = *pc++;
                uint16_t name_idx = pc[0] | (pc[1] << 8);
                pc += 2;
                uint16_t slot_id = pc[0] | (pc[1] << 8);
                pc += 2;

                TaggedValue obj_val = frame.GetRegister(obj_reg);
                assert(obj_val.IsHeapObject() && obj_val.AsHeapObject()->IsJSObject());
                JSObject* js_obj = static_cast<JSObject*>(obj_val.AsHeapObject());

                // Inline Cache check
                FeedbackSlot& ic_slot = feedback_.GetSlot(slot_id);
                Shape* obj_shape = js_obj->GetShape();

                if (AETHER_LIKELY(ic_slot.state == FeedbackSlot::State::Monomorphic && ic_slot.cached_shape == obj_shape)) {
                    // Monomorphic IC Hit: Direct O(1) slot read
                    acc_ = js_obj->GetFastSlot(ic_slot.cached_slot, ic_slot.cached_in_object);
                } else {
                    // IC Miss: Slow path lookup and IC transition
                    TaggedValue prop_name_val = bytecode.Constants()[name_idx];
                    assert(prop_name_val.IsHeapObject() && prop_name_val.AsHeapObject()->IsString());
                    std::string_view prop_key = static_cast<JSString*>(prop_name_val.AsHeapObject())->View();

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
                assert(obj_val.IsHeapObject() && obj_val.AsHeapObject()->IsJSObject());
                JSObject* js_obj = static_cast<JSObject*>(obj_val.AsHeapObject());

                // Inline Cache check
                FeedbackSlot& ic_slot = feedback_.GetSlot(slot_id);
                Shape* obj_shape = js_obj->GetShape();

                if (AETHER_LIKELY(ic_slot.state == FeedbackSlot::State::Monomorphic && ic_slot.cached_shape == obj_shape)) {
                    // Monomorphic IC Hit: Direct O(1) slot write
                    js_obj->SetFastSlot(ic_slot.cached_slot, ic_slot.cached_in_object, acc_);
                } else {
                    // IC Miss: Set property and update Shape / IC
                    TaggedValue prop_name_val = bytecode.Constants()[name_idx];
                    assert(prop_name_val.IsHeapObject() && prop_name_val.AsHeapObject()->IsString());
                    std::string_view prop_key = static_cast<JSString*>(prop_name_val.AsHeapObject())->View();

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
            case Opcode::CreateObject: {
                auto* obj = new JSObject(GetRootShape());
                acc_ = TaggedValue::FromHeapObject(obj);
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
                    acc_ = native_fn->Callback()(call_args);
                } else {
                    auto* func = static_cast<JSFunction*>(callee_val.AsHeapObject());
                    acc_ = Execute(*func->GetBytecode(), call_args);
                }
                break;
            }
            case Opcode::Return: {
                return acc_;
            }
            case Opcode::Halt: {
                return acc_;
            }
            default:
                assert(false && "Unhandled bytecode opcode");
                return TaggedValue::Undefined();
        }
    }

    return acc_;
}

} // namespace aether
