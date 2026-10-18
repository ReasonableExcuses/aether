#include "aether/bytecode.h"
#include <iomanip>
#include <sstream>

namespace aether {

const char* OpcodeToString(Opcode op) {
    switch (op) {
        case Opcode::LdaConstant:        return "LdaConstant";
        case Opcode::LdaZero:            return "LdaZero";
        case Opcode::LdaSmi:             return "LdaSmi";
        case Opcode::LdaUndefined:       return "LdaUndefined";
        case Opcode::LdaNull:            return "LdaNull";
        case Opcode::LdaTrue:            return "LdaTrue";
        case Opcode::LdaFalse:           return "LdaFalse";
        case Opcode::Ldar:               return "Ldar";
        case Opcode::Star:               return "Star";
        case Opcode::Mov:                return "Mov";
        case Opcode::Add:                return "Add";
        case Opcode::Sub:                return "Sub";
        case Opcode::Mul:                return "Mul";
        case Opcode::Div:                return "Div";
        case Opcode::Mod:                return "Mod";
        case Opcode::Exp:                return "Exp";
        case Opcode::BitwiseAnd:         return "BitwiseAnd";
        case Opcode::BitwiseOr:          return "BitwiseOr";
        case Opcode::BitwiseXor:         return "BitwiseXor";
        case Opcode::ShiftLeft:          return "ShiftLeft";
        case Opcode::ShiftRight:         return "ShiftRight";
        case Opcode::ShiftRightUnsigned: return "ShiftRightUnsigned";
        case Opcode::TestEqual:          return "TestEqual";
        case Opcode::TestStrictEqual:    return "TestStrictEqual";
        case Opcode::TestLessThan:       return "TestLessThan";
        case Opcode::TestGreaterThan:    return "TestGreaterThan";
        case Opcode::TestLessThanOrEqual: return "TestLessThanOrEqual";
        case Opcode::TestGreaterThanOrEqual: return "TestGreaterThanOrEqual";
        case Opcode::TestInstanceOf:     return "TestInstanceOf";
        case Opcode::TestIn:             return "TestIn";
        case Opcode::TestInWith:         return "TestInWith";
        case Opcode::Jump:               return "Jump";
        case Opcode::JumpIfTrue:         return "JumpIfTrue";
        case Opcode::JumpIfFalse:        return "JumpIfFalse";
        case Opcode::JumpIfNotNullish:   return "JumpIfNotNullish";
        case Opcode::JumpIfNullish:      return "JumpIfNullish";
        case Opcode::LdaNamed:           return "LdaNamed";
        case Opcode::StaNamed:           return "StaNamed";
        case Opcode::LdaKeyed:           return "LdaKeyed";
        case Opcode::StaKeyed:           return "StaKeyed";
        case Opcode::DeleteNamed:        return "DeleteNamed";
        case Opcode::DeleteKeyed:        return "DeleteKeyed";
        case Opcode::LogicalNot:         return "LogicalNot";
        case Opcode::Negate:             return "Negate";
        case Opcode::BitwiseNot:         return "BitwiseNot";
        case Opcode::TypeOf:             return "TypeOf";
        case Opcode::TypeOfGlobal:       return "TypeOfGlobal";
        case Opcode::LdaGlobal:          return "LdaGlobal";
        case Opcode::StaGlobal:          return "StaGlobal";
        case Opcode::ToString:           return "ToString";
        case Opcode::ToNumber:           return "ToNumber";
        case Opcode::ToNumeric:          return "ToNumeric";
        case Opcode::ToPropertyKey:      return "ToPropertyKey";
        case Opcode::CheckObjectCoercible: return "CheckObjectCoercible";
        case Opcode::PushWith:           return "PushWith";
        case Opcode::PopWith:            return "PopWith";
        case Opcode::Inc:                return "Inc";
        case Opcode::Dec:                return "Dec";
        case Opcode::Call:               return "Call";
        case Opcode::CallMethod:         return "CallMethod";
        case Opcode::CreateObject:       return "CreateObject";
        case Opcode::CreateArray:        return "CreateArray";
        case Opcode::Construct:          return "Construct";
        case Opcode::LdaThis:            return "LdaThis";
        case Opcode::ArrayPush:          return "ArrayPush";
        case Opcode::ArraySpread:        return "ArraySpread";
        case Opcode::ObjectSpread:       return "ObjectSpread";
        case Opcode::CreateRestArray:    return "CreateRestArray";
        case Opcode::CreateClosure:      return "CreateClosure";
        case Opcode::CreateFunctionContext: return "CreateFunctionContext";
        case Opcode::LdaContextSlot:     return "LdaContextSlot";
        case Opcode::StaContextSlot:     return "StaContextSlot";
        case Opcode::PushExceptionHandler: return "PushExceptionHandler";
        case Opcode::PopExceptionHandler:  return "PopExceptionHandler";
        case Opcode::Throw:              return "Throw";
        case Opcode::Yield:              return "Yield";
        case Opcode::Await:              return "Await";
        case Opcode::Debugger:           return "Debugger";
        case Opcode::Return:             return "Return";
        case Opcode::Halt:               return "Halt";
        default:                         return "<UnknownOpcode>";
    }
}

void BytecodeArray::EmitU16(uint16_t u16) {
    bytes_.push_back(static_cast<uint8_t>(u16 & 0xFF));
    bytes_.push_back(static_cast<uint8_t>((u16 >> 8) & 0xFF));
}

void BytecodeArray::EmitI32(int32_t i32) {
    uint32_t u32 = static_cast<uint32_t>(i32);
    bytes_.push_back(static_cast<uint8_t>(u32 & 0xFF));
    bytes_.push_back(static_cast<uint8_t>((u32 >> 8) & 0xFF));
    bytes_.push_back(static_cast<uint8_t>((u32 >> 16) & 0xFF));
    bytes_.push_back(static_cast<uint8_t>((u32 >> 24) & 0xFF));
}

void BytecodeArray::PatchI32(size_t offset, int32_t val) {
    assert(offset + 4 <= bytes_.size());
    uint32_t u32 = static_cast<uint32_t>(val);
    bytes_[offset]     = static_cast<uint8_t>(u32 & 0xFF);
    bytes_[offset + 1] = static_cast<uint8_t>((u32 >> 8) & 0xFF);
    bytes_[offset + 2] = static_cast<uint8_t>((u32 >> 16) & 0xFF);
    bytes_[offset + 3] = static_cast<uint8_t>((u32 >> 24) & 0xFF);
}

uint16_t BytecodeArray::AddConstant(TaggedValue val) {
    for (size_t i = 0; i < constants_.size(); ++i) {
        if (constants_[i] == val) {
            return static_cast<uint16_t>(i);
        }
    }
    size_t idx = constants_.size();
    assert(idx <= UINT16_MAX && "Constant pool overflow");
    constants_.push_back(val);
    return static_cast<uint16_t>(idx);
}

std::string BytecodeArray::Disassemble() const {
    std::ostringstream ss;
    ss << "=== BytecodeArray (Registers: " << register_count_ 
       << ", Parameters: " << parameter_count_ << ", Size: " << bytes_.size() << " bytes) ===\n";

    size_t pc = 0;
    while (pc < bytes_.size()) {
        size_t current_pc = pc;
        Opcode op = static_cast<Opcode>(bytes_[pc++]);

        ss << "  [0x" << std::hex << std::right << std::setw(4) << std::setfill('0') << current_pc << std::dec << "] "
           << std::setfill(' ') << std::left << std::setw(18) << OpcodeToString(op) << " ";

        switch (op) {
            case Opcode::LdaConstant: {
                uint16_t idx = bytes_[pc] | (bytes_[pc + 1] << 8);
                pc += 2;
                ss << "const[" << idx << "] (" << constants_[idx].ToString() << ")";
                break;
            }
            case Opcode::LdaSmi: {
                int32_t imm = bytes_[pc] | (bytes_[pc + 1] << 8) | (bytes_[pc + 2] << 16) | (bytes_[pc + 3] << 24);
                pc += 4;
                ss << imm;
                break;
            }
            case Opcode::Ldar:
            case Opcode::Star:
            case Opcode::CheckObjectCoercible:
            case Opcode::PushWith: {
                uint8_t reg = bytes_[pc++];
                ss << "r" << static_cast<int>(reg);
                break;
            }
            case Opcode::Mov: {
                uint8_t src = bytes_[pc++];
                uint8_t dst = bytes_[pc++];
                ss << "r" << static_cast<int>(src) << " -> r" << static_cast<int>(dst);
                break;
            }
            case Opcode::Add:
            case Opcode::Sub:
            case Opcode::Mul:
            case Opcode::Div:
            case Opcode::Mod:
            case Opcode::BitwiseAnd:
            case Opcode::BitwiseOr:
            case Opcode::BitwiseXor:
            case Opcode::ShiftLeft:
            case Opcode::ShiftRight:
            case Opcode::TestEqual:
            case Opcode::TestStrictEqual:
            case Opcode::TestLessThan:
            case Opcode::TestGreaterThan:
            case Opcode::TestLessThanOrEqual:
            case Opcode::TestGreaterThanOrEqual:
            case Opcode::TestInstanceOf: {
                uint8_t reg = bytes_[pc++];
                ss << "r" << static_cast<int>(reg);
                if (op == Opcode::Add || op == Opcode::Sub || op == Opcode::Mul || op == Opcode::Div || op == Opcode::Mod) {
                    uint16_t slot = bytes_[pc] | (bytes_[pc + 1] << 8);
                    pc += 2;
                    ss << " [feedback_slot: " << slot << "]";
                }
                break;
            }
            case Opcode::Jump:
            case Opcode::JumpIfTrue:
            case Opcode::JumpIfFalse:
            case Opcode::JumpIfNotNullish:
            case Opcode::JumpIfNullish: {
                int32_t offset = bytes_[pc] | (bytes_[pc + 1] << 8) | (bytes_[pc + 2] << 16) | (bytes_[pc + 3] << 24);
                pc += 4;
                ss << "offset: " << offset << " -> [0x" << std::hex << (pc + offset) << std::dec << "]";
                break;
            }
            case Opcode::LdaNamed:
            case Opcode::StaNamed: {
                uint8_t obj = bytes_[pc++];
                uint16_t name_idx = bytes_[pc] | (bytes_[pc + 1] << 8);
                pc += 2;
                uint16_t slot = bytes_[pc] | (bytes_[pc + 1] << 8);
                pc += 2;
                ss << "r" << static_cast<int>(obj) << "." << constants_[name_idx].ToString() 
                   << " [feedback_slot: " << slot << "]";
                break;
            }
            case Opcode::LdaKeyed:
            case Opcode::StaKeyed: {
                uint8_t obj = bytes_[pc++];
                uint8_t key = bytes_[pc++];
                uint16_t slot = bytes_[pc] | (bytes_[pc + 1] << 8);
                pc += 2;
                ss << "r" << static_cast<int>(obj) << "[r" << static_cast<int>(key) << "]"
                   << " [feedback_slot: " << slot << "]";
                break;
            }
            case Opcode::ArrayPush: {
                uint8_t arr = bytes_[pc++];
                uint8_t val = bytes_[pc++];
                ss << "arr: r" << static_cast<int>(arr) << ", val: r" << static_cast<int>(val);
                break;
            }
            case Opcode::ArraySpread:
            case Opcode::ObjectSpread: {
                uint8_t target = bytes_[pc++];
                uint8_t src = bytes_[pc++];
                ss << "target: r" << static_cast<int>(target) << ", src: r" << static_cast<int>(src);
                break;
            }
            case Opcode::CreateRestArray: {
                uint8_t dest = bytes_[pc++];
                uint16_t start_idx = bytes_[pc] | (bytes_[pc + 1] << 8);
                pc += 2;
                ss << "dest: r" << static_cast<int>(dest) << ", start_idx: " << start_idx;
                break;
            }
            case Opcode::CreateArray: {
                uint8_t start = bytes_[pc++];
                uint16_t count = bytes_[pc] | (bytes_[pc + 1] << 8);
                pc += 2;
                ss << "start: r" << static_cast<int>(start) << ", count: " << count;
                break;
            }
            case Opcode::Call: {
                uint8_t callee = bytes_[pc++];
                uint8_t args = bytes_[pc++];
                uint8_t count = bytes_[pc++];
                uint16_t slot = bytes_[pc] | (bytes_[pc + 1] << 8);
                pc += 2;
                ss << "callee: r" << static_cast<int>(callee)
                   << ", args: r" << static_cast<int>(args) << ".." << static_cast<int>(args + count - 1)
                   << " (count: " << static_cast<int>(count) << ")"
                   << " [feedback_slot: " << slot << "]";
                break;
            }
            case Opcode::CallMethod: {
                uint8_t callee = bytes_[pc++];
                uint8_t this_reg = bytes_[pc++];
                uint8_t args = bytes_[pc++];
                uint8_t count = bytes_[pc++];
                uint16_t slot = bytes_[pc] | (bytes_[pc + 1] << 8);
                pc += 2;
                ss << "callee: r" << static_cast<int>(callee)
                   << ", this: r" << static_cast<int>(this_reg)
                   << ", args: r" << static_cast<int>(args) << ".." << static_cast<int>(args + count - 1)
                   << " (count: " << static_cast<int>(count) << ")"
                   << " [feedback_slot: " << slot << "]";
                break;
            }
            case Opcode::Construct: {
                uint8_t ctor = bytes_[pc++];
                uint8_t args = bytes_[pc++];
                uint8_t count = bytes_[pc++];
                uint16_t slot = bytes_[pc] | (bytes_[pc + 1] << 8);
                pc += 2;
                ss << "ctor: r" << static_cast<int>(ctor)
                   << ", args: r" << static_cast<int>(args) << ".." << static_cast<int>(args + count - 1)
                   << " (count: " << static_cast<int>(count) << ")"
                   << " [feedback_slot: " << slot << "]";
                break;
            }
            case Opcode::CreateClosure: {
                uint16_t idx = bytes_[pc] | (bytes_[pc + 1] << 8);
                pc += 2;
                ss << "const[" << idx << "]";
                break;
            }
            case Opcode::TypeOfGlobal:
            case Opcode::LdaGlobal:
            case Opcode::StaGlobal: {
                uint16_t name_idx = bytes_[pc] | (bytes_[pc + 1] << 8);
                pc += 2;
                ss << "const[" << name_idx << "]";
                break;
            }
            case Opcode::CreateFunctionContext: {
                uint16_t slots = bytes_[pc] | (bytes_[pc + 1] << 8);
                pc += 2;
                ss << "slots: " << slots;
                break;
            }
            case Opcode::LdaContextSlot:
            case Opcode::StaContextSlot: {
                uint8_t depth = bytes_[pc++];
                uint16_t slot = bytes_[pc] | (bytes_[pc + 1] << 8);
                pc += 2;
                ss << "depth: " << static_cast<int>(depth) << ", slot: " << slot;
                break;
            }
            case Opcode::PushExceptionHandler: {
                int32_t offset = bytes_[pc] | (bytes_[pc + 1] << 8) | (bytes_[pc + 2] << 16) | (bytes_[pc + 3] << 24);
                pc += 4;
                ss << "offset: " << offset << " -> [0x" << std::hex << (pc + offset) << std::dec << "]";
                break;
            }
            case Opcode::PopExceptionHandler:
            case Opcode::Throw:
            case Opcode::LdaThis:
            case Opcode::CreateObject:
            case Opcode::LogicalNot:
            case Opcode::Negate:
            case Opcode::BitwiseNot:
            case Opcode::LdaZero:
            case Opcode::LdaUndefined:
            case Opcode::LdaNull:
            case Opcode::LdaTrue:
            case Opcode::LdaFalse:
            case Opcode::Yield:
            case Opcode::Await:
            case Opcode::Return:
            case Opcode::Halt:
                break;
            default:
                break;
        }
        ss << "\n";
    }
    return ss.str();
}

} // namespace aether
