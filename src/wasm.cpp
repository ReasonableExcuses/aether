#include "aether/wasm.h"
#include "aether/runtime.h"
#include "aether/interpreter.h"
#include "aether/heap_object.h"
#include "aether/typed_array.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace aether {

// ============================================================================
// Internal Registries for WebAssembly Host JSObjects
// ============================================================================

static std::unordered_map<JSObject*, std::shared_ptr<WasmModule>> s_module_registry;
static std::unordered_map<JSObject*, std::shared_ptr<WasmInstance>> s_instance_registry;
static std::unordered_map<JSObject*, std::shared_ptr<WasmMemoryInstance>> s_memory_registry;
static std::unordered_map<JSObject*, std::shared_ptr<WasmTableInstance>> s_table_registry;
static std::unordered_map<JSObject*, std::shared_ptr<WasmGlobalInstance>> s_global_registry;

// Helper to extract binary buffer from ArrayBuffer or TypedArray
static bool ExtractBufferSource(TaggedValue val, const uint8_t*& out_data, size_t& out_size) {
    if (!val.IsHeapObject()) return false;
    HeapObject* ho = val.AsHeapObject();
    if (ho->GetType() == HeapObjectType::JSArrayBuffer) {
        auto* ab = static_cast<JSArrayBuffer*>(ho);
        if (ab->IsDetached()) return false;
        out_data = ab->Data();
        out_size = ab->ByteLength();
        return true;
    }
    if (ho->GetType() == HeapObjectType::JSTypedArray) {
        auto* ta = static_cast<JSTypedArray*>(ho);
        if (!ta->Buffer() || ta->Buffer()->IsDetached()) return false;
        out_data = ta->Buffer()->Data() + ta->ByteOffset();
        out_size = ta->ByteLength();
        return true;
    }
    return false;
}

// ============================================================================
// LEB128 & Binary Helper Functions
// ============================================================================

namespace {

bool ReadByte(const uint8_t*& p, const uint8_t* end, uint8_t& out) {
    if (p >= end) return false;
    out = *p++;
    return true;
}

bool DecodeU32(const uint8_t*& p, const uint8_t* end, uint32_t& out) {
    uint32_t result = 0;
    uint32_t shift = 0;
    while (p < end) {
        uint8_t byte = *p++;
        result |= (static_cast<uint32_t>(byte & 0x7F) << shift);
        if ((byte & 0x80) == 0) {
            out = result;
            return true;
        }
        shift += 7;
        if (shift >= 35) return false; // overflow
    }
    return false;
}

bool DecodeI32(const uint8_t*& p, const uint8_t* end, int32_t& out) {
    int32_t result = 0;
    uint32_t shift = 0;
    uint8_t byte = 0;
    while (p < end) {
        byte = *p++;
        result |= (static_cast<int32_t>(byte & 0x7F) << shift);
        shift += 7;
        if ((byte & 0x80) == 0) {
            if (shift < 32 && (byte & 0x40) != 0) {
                result |= (~0 << shift); // sign extend
            }
            out = result;
            return true;
        }
        if (shift >= 35) return false;
    }
    return false;
}

bool DecodeI64(const uint8_t*& p, const uint8_t* end, int64_t& out) {
    int64_t result = 0;
    uint32_t shift = 0;
    uint8_t byte = 0;
    while (p < end) {
        byte = *p++;
        result |= (static_cast<int64_t>(byte & 0x7F) << shift);
        shift += 7;
        if ((byte & 0x80) == 0) {
            if (shift < 64 && (byte & 0x40) != 0) {
                result |= (~0ULL << shift); // sign extend
            }
            out = result;
            return true;
        }
        if (shift >= 70) return false;
    }
    return false;
}

bool DecodeString(const uint8_t*& p, const uint8_t* end, std::string& out) {
    uint32_t len = 0;
    if (!DecodeU32(p, end, len)) return false;
    if (p + len > end) return false;
    out.assign(reinterpret_cast<const char*>(p), len);
    p += len;
    return true;
}

bool DecodeValType(const uint8_t*& p, const uint8_t* end, WasmValType& out) {
    uint8_t b = 0;
    if (!ReadByte(p, end, b)) return false;
    switch (b) {
        case 0x7F: out = WasmValType::I32; return true;
        case 0x7E: out = WasmValType::I64; return true;
        case 0x7D: out = WasmValType::F32; return true;
        case 0x7C: out = WasmValType::F64; return true;
        case 0x70: out = WasmValType::FuncRef; return true;
        case 0x40: out = WasmValType::Void; return true;
        default: return false;
    }
}

bool DecodeLimits(const uint8_t*& p, const uint8_t* end, WasmLimits& out) {
    uint8_t flag = 0;
    if (!ReadByte(p, end, flag)) return false;
    if (!DecodeU32(p, end, out.initial)) return false;
    if (flag & 0x01) {
        out.has_max = true;
        if (!DecodeU32(p, end, out.maximum)) return false;
    } else {
        out.has_max = false;
        out.maximum = 0;
    }
    return true;
}

bool SkipInitExpr(const uint8_t*& p, const uint8_t* end, std::vector<uint8_t>& expr_out) {
    const uint8_t* start = p;
    while (p < end) {
        uint8_t b = *p++;
        if (b == static_cast<uint8_t>(WasmOpcode::End)) {
            expr_out.assign(start, p);
            return true;
        }
    }
    return false;
}

} // namespace

// ============================================================================
// TaggedValue <-> WasmVal Conversions
// ============================================================================

TaggedValue WasmVal::ToJS(Realm& realm) const {
    (void)realm;
    switch (type) {
        case WasmValType::I32:
            return TaggedValue::FromSmi(i32);
        case WasmValType::I64:
            return TaggedValue::FromDouble(static_cast<double>(i64));
        case WasmValType::F32:
            return TaggedValue::FromDouble(static_cast<double>(f32));
        case WasmValType::F64:
            return TaggedValue::FromDouble(f64);
        case WasmValType::FuncRef:
        case WasmValType::Void:
        default:
            return TaggedValue::Undefined();
    }
}

WasmVal WasmVal::FromJS(TaggedValue val, WasmValType expected_type) {
    double num = 0.0;
    if (val.IsNumber()) {
        num = val.AsNumber();
    } else if (val.IsBoolean()) {
        num = val.ToBoolean() ? 1.0 : 0.0;
    }
    switch (expected_type) {
        case WasmValType::I32:
            return WasmVal::MakeI32(static_cast<int32_t>(num));
        case WasmValType::I64:
            return WasmVal::MakeI64(static_cast<int64_t>(num));
        case WasmValType::F32:
            return WasmVal::MakeF32(static_cast<float>(num));
        case WasmValType::F64:
            return WasmVal::MakeF64(num);
        default:
            return WasmVal::MakeI32(0);
    }
}

// ============================================================================
// WasmModule Binary Decoding & Validation
// ============================================================================

const WasmFuncType& WasmModule::GetFunctionSignature(uint32_t func_idx) const {
    static const WasmFuncType kEmptyType;
    if (func_idx < num_imported_funcs_) {
        uint32_t imp_idx = 0;
        for (const auto& imp : imports_) {
            if (imp.kind == WasmExternalKind::Function) {
                if (imp_idx == func_idx) {
                    if (imp.type_idx < types_.size()) return types_[imp.type_idx];
                    return kEmptyType;
                }
                imp_idx++;
            }
        }
    } else {
        uint32_t def_idx = func_idx - num_imported_funcs_;
        if (def_idx < function_type_indices_.size()) {
            uint32_t type_idx = function_type_indices_[def_idx];
            if (type_idx < types_.size()) return types_[type_idx];
        }
    }
    return kEmptyType;
}

std::shared_ptr<WasmModule> WasmModule::Decode(const uint8_t* data, size_t length, std::string* error_msg) {
    if (!data || length < 8) {
        if (error_msg) *error_msg = "Wasm binary is truncated or null";
        return nullptr;
    }

    // Check magic: '\0asm'
    if (data[0] != 0x00 || data[1] != 0x61 || data[2] != 0x73 || data[3] != 0x6D) {
        if (error_msg) *error_msg = "Invalid WebAssembly magic number";
        return nullptr;
    }

    // Check version: 1
    uint32_t version = static_cast<uint32_t>(data[4]) |
                      (static_cast<uint32_t>(data[5]) << 8) |
                      (static_cast<uint32_t>(data[6]) << 16) |
                      (static_cast<uint32_t>(data[7]) << 24);
    if (version != 1) {
        if (error_msg) *error_msg = "Unsupported WebAssembly version: " + std::to_string(version);
        return nullptr;
    }

    auto module = std::make_shared<WasmModule>();
    const uint8_t* p = data + 8;
    const uint8_t* end = data + length;

    uint8_t prev_sec_id = 0;

    while (p < end) {
        uint8_t sec_id = 0;
        if (!ReadByte(p, end, sec_id)) break;

        uint32_t sec_len = 0;
        if (!DecodeU32(p, end, sec_len)) {
            if (error_msg) *error_msg = "Failed to decode section size";
            return nullptr;
        }

        if (p + sec_len > end) {
            if (error_msg) *error_msg = "Section payload exceeds module boundary";
            return nullptr;
        }

        const uint8_t* sec_start = p;
        const uint8_t* sec_end = p + sec_len;

        // Custom sections (ID 0) can appear anywhere. Non-custom sections must be ordered.
        if (sec_id != 0) {
            if (sec_id <= prev_sec_id) {
                if (error_msg) *error_msg = "Out-of-order section ID: " + std::to_string(sec_id);
                return nullptr;
            }
            prev_sec_id = sec_id;
        }

        switch (sec_id) {
            case 0: { // Custom section
                WasmCustomSection cs;
                if (!DecodeString(p, sec_end, cs.name)) {
                    if (error_msg) *error_msg = "Failed to decode custom section name";
                    return nullptr;
                }
                cs.data.assign(p, sec_end);
                module->custom_sections_.push_back(std::move(cs));
                break;
            }
            case 1: { // Type section
                uint32_t count = 0;
                if (!DecodeU32(p, sec_end, count)) return nullptr;
                for (uint32_t i = 0; i < count; ++i) {
                    uint8_t form = 0;
                    if (!ReadByte(p, sec_end, form) || form != 0x60) {
                        if (error_msg) *error_msg = "Invalid function type form";
                        return nullptr;
                    }
                    WasmFuncType ft;
                    uint32_t param_count = 0;
                    if (!DecodeU32(p, sec_end, param_count)) return nullptr;
                    for (uint32_t j = 0; j < param_count; ++j) {
                        WasmValType vt;
                        if (!DecodeValType(p, sec_end, vt)) return nullptr;
                        ft.params.push_back(vt);
                    }
                    uint32_t res_count = 0;
                    if (!DecodeU32(p, sec_end, res_count)) return nullptr;
                    for (uint32_t j = 0; j < res_count; ++j) {
                        WasmValType vt;
                        if (!DecodeValType(p, sec_end, vt)) return nullptr;
                        ft.results.push_back(vt);
                    }
                    module->types_.push_back(std::move(ft));
                }
                break;
            }
            case 2: { // Import section
                uint32_t count = 0;
                if (!DecodeU32(p, sec_end, count)) return nullptr;
                for (uint32_t i = 0; i < count; ++i) {
                    WasmImport imp;
                    if (!DecodeString(p, sec_end, imp.module_name)) return nullptr;
                    if (!DecodeString(p, sec_end, imp.field_name)) return nullptr;
                    uint8_t kind_byte = 0;
                    if (!ReadByte(p, sec_end, kind_byte)) return nullptr;
                    imp.kind = static_cast<WasmExternalKind>(kind_byte);
                    switch (imp.kind) {
                        case WasmExternalKind::Function:
                            if (!DecodeU32(p, sec_end, imp.type_idx)) return nullptr;
                            module->num_imported_funcs_++;
                            break;
                        case WasmExternalKind::Table: {
                            WasmValType vt;
                            if (!DecodeValType(p, sec_end, vt)) return nullptr;
                            imp.table_desc.elem_type = vt;
                            if (!DecodeLimits(p, sec_end, imp.table_desc.limits)) return nullptr;
                            module->num_imported_tables_++;
                            break;
                        }
                        case WasmExternalKind::Memory:
                            if (!DecodeLimits(p, sec_end, imp.mem_desc.limits)) return nullptr;
                            module->num_imported_memories_++;
                            break;
                        case WasmExternalKind::Global: {
                            WasmValType vt;
                            if (!DecodeValType(p, sec_end, vt)) return nullptr;
                            imp.global_desc.content_type = vt;
                            uint8_t mut = 0;
                            if (!ReadByte(p, sec_end, mut)) return nullptr;
                            imp.global_desc.is_mutable = (mut != 0);
                            module->num_imported_globals_++;
                            break;
                        }
                    }
                    module->imports_.push_back(std::move(imp));
                }
                break;
            }
            case 3: { // Function section
                uint32_t count = 0;
                if (!DecodeU32(p, sec_end, count)) return nullptr;
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t tidx = 0;
                    if (!DecodeU32(p, sec_end, tidx)) return nullptr;
                    module->function_type_indices_.push_back(tidx);
                }
                break;
            }
            case 4: { // Table section
                uint32_t count = 0;
                if (!DecodeU32(p, sec_end, count)) return nullptr;
                for (uint32_t i = 0; i < count; ++i) {
                    WasmTableType tt;
                    WasmValType vt;
                    if (!DecodeValType(p, sec_end, vt)) return nullptr;
                    tt.elem_type = vt;
                    if (!DecodeLimits(p, sec_end, tt.limits)) return nullptr;
                    module->tables_.push_back(tt);
                }
                break;
            }
            case 5: { // Memory section
                uint32_t count = 0;
                if (!DecodeU32(p, sec_end, count)) return nullptr;
                for (uint32_t i = 0; i < count; ++i) {
                    WasmMemoryType mt;
                    if (!DecodeLimits(p, sec_end, mt.limits)) return nullptr;
                    module->memories_.push_back(mt);
                }
                break;
            }
            case 6: { // Global section
                uint32_t count = 0;
                if (!DecodeU32(p, sec_end, count)) return nullptr;
                for (uint32_t i = 0; i < count; ++i) {
                    WasmGlobalDesc gd;
                    WasmValType vt;
                    if (!DecodeValType(p, sec_end, vt)) return nullptr;
                    gd.type.content_type = vt;
                    uint8_t mut = 0;
                    if (!ReadByte(p, sec_end, mut)) return nullptr;
                    gd.type.is_mutable = (mut != 0);
                    if (!SkipInitExpr(p, sec_end, gd.init_expr)) return nullptr;
                    module->globals_.push_back(std::move(gd));
                }
                break;
            }
            case 7: { // Export section
                uint32_t count = 0;
                if (!DecodeU32(p, sec_end, count)) return nullptr;
                for (uint32_t i = 0; i < count; ++i) {
                    WasmExport exp;
                    if (!DecodeString(p, sec_end, exp.name)) return nullptr;
                    uint8_t kind_byte = 0;
                    if (!ReadByte(p, sec_end, kind_byte)) return nullptr;
                    exp.kind = static_cast<WasmExternalKind>(kind_byte);
                    if (!DecodeU32(p, sec_end, exp.index)) return nullptr;
                    module->exports_.push_back(std::move(exp));
                }
                break;
            }
            case 8: { // Start section
                uint32_t sidx = 0;
                if (!DecodeU32(p, sec_end, sidx)) return nullptr;
                module->start_function_ = sidx;
                break;
            }
            case 9: { // Element section
                uint32_t count = 0;
                if (!DecodeU32(p, sec_end, count)) return nullptr;
                for (uint32_t i = 0; i < count; ++i) {
                    WasmElemSegment es;
                    if (!DecodeU32(p, sec_end, es.table_index)) return nullptr;
                    if (!SkipInitExpr(p, sec_end, es.offset_expr)) return nullptr;
                    uint32_t num_funcs = 0;
                    if (!DecodeU32(p, sec_end, num_funcs)) return nullptr;
                    for (uint32_t f = 0; f < num_funcs; ++f) {
                        uint32_t fidx = 0;
                        if (!DecodeU32(p, sec_end, fidx)) return nullptr;
                        es.func_indices.push_back(fidx);
                    }
                    module->elements_.push_back(std::move(es));
                }
                break;
            }
            case 10: { // Code section
                uint32_t count = 0;
                if (!DecodeU32(p, sec_end, count)) return nullptr;
                if (count != module->function_type_indices_.size()) {
                    if (error_msg) *error_msg = "Function and Code section counts mismatch";
                    return nullptr;
                }
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t body_size = 0;
                    if (!DecodeU32(p, sec_end, body_size)) return nullptr;
                    const uint8_t* body_end = p + body_size;
                    if (body_end > sec_end) return nullptr;

                    WasmFuncDesc fd;
                    fd.type_index = module->function_type_indices_[i];

                    uint32_t local_entries = 0;
                    if (!DecodeU32(p, body_end, local_entries)) return nullptr;
                    for (uint32_t l = 0; l < local_entries; ++l) {
                        uint32_t lcount = 0;
                        if (!DecodeU32(p, body_end, lcount)) return nullptr;
                        WasmValType ltype;
                        if (!DecodeValType(p, body_end, ltype)) return nullptr;
                        fd.locals.push_back({lcount, ltype});
                    }
                    fd.code.assign(p, body_end);
                    p = body_end;
                    module->functions_.push_back(std::move(fd));
                }
                break;
            }
            case 11: { // Data section
                uint32_t count = 0;
                if (!DecodeU32(p, sec_end, count)) return nullptr;
                for (uint32_t i = 0; i < count; ++i) {
                    WasmDataSegment ds;
                    if (!DecodeU32(p, sec_end, ds.memory_index)) return nullptr;
                    if (!SkipInitExpr(p, sec_end, ds.offset_expr)) return nullptr;
                    uint32_t data_len = 0;
                    if (!DecodeU32(p, sec_end, data_len)) return nullptr;
                    if (p + data_len > sec_end) return nullptr;
                    ds.data.assign(p, p + data_len);
                    p += data_len;
                    module->data_segments_.push_back(std::move(ds));
                }
                break;
            }
            default:
                // Unknown section ID
                if (error_msg) *error_msg = "Unknown section ID: " + std::to_string(sec_id);
                return nullptr;
        }

        p = sec_start + sec_len; // advance exactly to end of section
    }

    return module;
}

bool WasmModule::Validate(const uint8_t* data, size_t length) {
    std::string err;
    auto mod = Decode(data, length, &err);
    return (mod != nullptr);
}

// ============================================================================
// WasmMemoryInstance Implementation
// ============================================================================

WasmMemoryInstance::WasmMemoryInstance(WasmLimits limits)
    : limits_(limits),
      data_(limits.initial * kPageSize, 0) {}

int32_t WasmMemoryInstance::Grow(uint32_t delta_pages) {
    uint32_t current_pages = SizePages();
    uint64_t target_pages = static_cast<uint64_t>(current_pages) + delta_pages;
    if (target_pages > kMaxPages) return -1;
    if (limits_.has_max && target_pages > limits_.maximum) return -1;

    data_.resize(target_pages * kPageSize, 0);

    // If a JSArrayBuffer was already wrapping this memory, recreate it so buffer reflects new size
    js_buffer_ = nullptr;
    return static_cast<int32_t>(current_pages);
}

JSArrayBuffer* WasmMemoryInstance::GetOrCreateJSBuffer(Realm& realm) {
    if (!js_buffer_) {
        // Create an ArrayBuffer sharing the memory data pointer
        js_buffer_ = AllocateRuntime<JSArrayBuffer>(data_.data(), data_.size(), false, realm.GetRootShape());
    }
    return js_buffer_;
}

bool WasmMemoryInstance::LoadI32(uint32_t offset, int32_t& out_val) const {
    if (offset + sizeof(int32_t) > data_.size()) return false;
    std::memcpy(&out_val, data_.data() + offset, sizeof(int32_t));
    return true;
}

bool WasmMemoryInstance::LoadI64(uint32_t offset, int64_t& out_val) const {
    if (offset + sizeof(int64_t) > data_.size()) return false;
    std::memcpy(&out_val, data_.data() + offset, sizeof(int64_t));
    return true;
}

bool WasmMemoryInstance::LoadF32(uint32_t offset, float& out_val) const {
    if (offset + sizeof(float) > data_.size()) return false;
    std::memcpy(&out_val, data_.data() + offset, sizeof(float));
    return true;
}

bool WasmMemoryInstance::LoadF64(uint32_t offset, double& out_val) const {
    if (offset + sizeof(double) > data_.size()) return false;
    std::memcpy(&out_val, data_.data() + offset, sizeof(double));
    return true;
}

bool WasmMemoryInstance::Load8S(uint32_t offset, int32_t& out_val) const {
    if (offset >= data_.size()) return false;
    out_val = static_cast<int8_t>(data_[offset]);
    return true;
}

bool WasmMemoryInstance::Load8U(uint32_t offset, int32_t& out_val) const {
    if (offset >= data_.size()) return false;
    out_val = static_cast<uint8_t>(data_[offset]);
    return true;
}

bool WasmMemoryInstance::Load16S(uint32_t offset, int32_t& out_val) const {
    if (offset + 2 > data_.size()) return false;
    int16_t v = 0;
    std::memcpy(&v, data_.data() + offset, 2);
    out_val = v;
    return true;
}

bool WasmMemoryInstance::Load16U(uint32_t offset, int32_t& out_val) const {
    if (offset + 2 > data_.size()) return false;
    uint16_t v = 0;
    std::memcpy(&v, data_.data() + offset, 2);
    out_val = v;
    return true;
}

bool WasmMemoryInstance::StoreI32(uint32_t offset, int32_t val) {
    if (offset + sizeof(int32_t) > data_.size()) return false;
    std::memcpy(data_.data() + offset, &val, sizeof(int32_t));
    return true;
}

bool WasmMemoryInstance::StoreI64(uint32_t offset, int64_t val) {
    if (offset + sizeof(int64_t) > data_.size()) return false;
    std::memcpy(data_.data() + offset, &val, sizeof(int64_t));
    return true;
}

bool WasmMemoryInstance::StoreF32(uint32_t offset, float val) {
    if (offset + sizeof(float) > data_.size()) return false;
    std::memcpy(data_.data() + offset, &val, sizeof(float));
    return true;
}

bool WasmMemoryInstance::StoreF64(uint32_t offset, double val) {
    if (offset + sizeof(double) > data_.size()) return false;
    std::memcpy(data_.data() + offset, &val, sizeof(double));
    return true;
}

bool WasmMemoryInstance::Store8(uint32_t offset, uint8_t val) {
    if (offset >= data_.size()) return false;
    data_[offset] = val;
    return true;
}

bool WasmMemoryInstance::Store16(uint32_t offset, uint16_t val) {
    if (offset + 2 > data_.size()) return false;
    std::memcpy(data_.data() + offset, &val, 2);
    return true;
}

// ============================================================================
// WasmTableInstance Implementation
// ============================================================================

WasmTableInstance::WasmTableInstance(WasmTableType type)
    : type_(type),
      elements_(type.limits.initial, 0xFFFFFFFF) {}

uint32_t WasmTableInstance::Get(uint32_t index) const {
    if (index >= elements_.size()) return 0xFFFFFFFF;
    return elements_[index];
}

bool WasmTableInstance::Set(uint32_t index, uint32_t func_idx) {
    if (index >= elements_.size()) return false;
    elements_[index] = func_idx;
    return true;
}

int32_t WasmTableInstance::Grow(uint32_t delta, uint32_t init_val) {
    uint32_t old_size = static_cast<uint32_t>(elements_.size());
    uint64_t new_size = static_cast<uint64_t>(old_size) + delta;
    if (type_.limits.has_max && new_size > type_.limits.maximum) return -1;
    elements_.resize(new_size, init_val);
    return static_cast<int32_t>(old_size);
}

// ============================================================================
// WasmExecutionEngine: Full Stack Machine Interpreter
// ============================================================================

namespace {

struct BlockMetadata {
    size_t else_pc{0};
    size_t end_pc{0};
};

struct ControlFrame {
    WasmOpcode opcode{WasmOpcode::Block};
    size_t start_pc{0};
    size_t end_pc{0};
    size_t stack_depth{0};
    WasmValType result_type{WasmValType::Void};
};

// Pre-scan block nesting structure to compute jump targets in O(1)
std::unordered_map<size_t, BlockMetadata> AnalyzeBlocks(const uint8_t* code, size_t code_len) {
    std::unordered_map<size_t, BlockMetadata> map;
    std::vector<size_t> stack;

    size_t pc = 0;
    while (pc < code_len) {
        size_t cur = pc;
        uint8_t op = code[pc++];
        switch (static_cast<WasmOpcode>(op)) {
            case WasmOpcode::Block:
            case WasmOpcode::Loop:
            case WasmOpcode::If: {
                stack.push_back(cur);
                // Skip block signature byte
                if (pc < code_len) pc++;
                break;
            }
            case WasmOpcode::Else: {
                if (!stack.empty()) {
                    map[stack.back()].else_pc = cur;
                }
                break;
            }
            case WasmOpcode::End: {
                if (!stack.empty()) {
                    size_t opener = stack.back();
                    stack.pop_back();
                    map[opener].end_pc = cur;
                }
                break;
            }
            // Skip immediate arguments for other instructions
            case WasmOpcode::Br:
            case WasmOpcode::BrIf: {
                uint32_t dummy = 0;
                const uint8_t* p = code + pc;
                DecodeU32(p, code + code_len, dummy);
                pc = p - code;
                break;
            }
            case WasmOpcode::BrTable: {
                const uint8_t* p = code + pc;
                uint32_t count = 0;
                DecodeU32(p, code + code_len, count);
                for (uint32_t i = 0; i <= count; ++i) {
                    uint32_t target = 0;
                    DecodeU32(p, code + code_len, target);
                }
                pc = p - code;
                break;
            }
            case WasmOpcode::Call:
            case WasmOpcode::CallIndirect:
            case WasmOpcode::LocalGet:
            case WasmOpcode::LocalSet:
            case WasmOpcode::LocalTee:
            case WasmOpcode::GlobalGet:
            case WasmOpcode::GlobalSet: {
                const uint8_t* p = code + pc;
                uint32_t dummy = 0;
                DecodeU32(p, code + code_len, dummy);
                if (static_cast<WasmOpcode>(op) == WasmOpcode::CallIndirect) {
                    if (p < code + code_len) p++; // 0x00 reserved
                }
                pc = p - code;
                break;
            }
            case WasmOpcode::I32Const: {
                const uint8_t* p = code + pc;
                int32_t dummy = 0;
                DecodeI32(p, code + code_len, dummy);
                pc = p - code;
                break;
            }
            case WasmOpcode::I64Const: {
                const uint8_t* p = code + pc;
                int64_t dummy = 0;
                DecodeI64(p, code + code_len, dummy);
                pc = p - code;
                break;
            }
            case WasmOpcode::F32Const: {
                pc += 4;
                break;
            }
            case WasmOpcode::F64Const: {
                pc += 8;
                break;
            }
            case WasmOpcode::I32Load:
            case WasmOpcode::I64Load:
            case WasmOpcode::F32Load:
            case WasmOpcode::F64Load:
            case WasmOpcode::I32Load8S:
            case WasmOpcode::I32Load8U:
            case WasmOpcode::I32Load16S:
            case WasmOpcode::I32Load16U:
            case WasmOpcode::I64Load8S:
            case WasmOpcode::I64Load8U:
            case WasmOpcode::I64Load16S:
            case WasmOpcode::I64Load16U:
            case WasmOpcode::I64Load32S:
            case WasmOpcode::I64Load32U:
            case WasmOpcode::I32Store:
            case WasmOpcode::I64Store:
            case WasmOpcode::F32Store:
            case WasmOpcode::F64Store:
            case WasmOpcode::I32Store8:
            case WasmOpcode::I32Store16:
            case WasmOpcode::I64Store8:
            case WasmOpcode::I64Store16:
            case WasmOpcode::I64Store32: {
                const uint8_t* p = code + pc;
                uint32_t align = 0, offset = 0;
                DecodeU32(p, code + code_len, align);
                DecodeU32(p, code + code_len, offset);
                pc = p - code;
                break;
            }
            case WasmOpcode::MemorySize:
            case WasmOpcode::MemoryGrow: {
                if (pc < code_len) pc++; // 0x00 reserved
                break;
            }
            default:
                break;
        }
    }
    return map;
}

} // namespace

// ============================================================================
// WasmInstance Implementation
// ============================================================================

WasmVal WasmInstance::EvaluateInitExpr(const std::vector<uint8_t>& expr) {
    if (expr.empty()) return WasmVal::MakeI32(0);
    const uint8_t* p = expr.data();
    const uint8_t* end = expr.data() + expr.size();
    uint8_t op = *p++;
    switch (static_cast<WasmOpcode>(op)) {
        case WasmOpcode::I32Const: {
            int32_t v = 0;
            DecodeI32(p, end, v);
            return WasmVal::MakeI32(v);
        }
        case WasmOpcode::I64Const: {
            int64_t v = 0;
            DecodeI64(p, end, v);
            return WasmVal::MakeI64(v);
        }
        case WasmOpcode::F32Const: {
            float v = 0.0f;
            if (p + 4 <= end) std::memcpy(&v, p, 4);
            return WasmVal::MakeF32(v);
        }
        case WasmOpcode::F64Const: {
            double v = 0.0;
            if (p + 8 <= end) std::memcpy(&v, p, 8);
            return WasmVal::MakeF64(v);
        }
        case WasmOpcode::GlobalGet: {
            uint32_t gidx = 0;
            DecodeU32(p, end, gidx);
            if (gidx < globals_.size()) return globals_[gidx]->Get();
            return WasmVal::MakeI32(0);
        }
        default:
            return WasmVal::MakeI32(0);
    }
}

std::shared_ptr<WasmInstance> WasmInstance::Instantiate(
    std::shared_ptr<WasmModule> module,
    TaggedValue import_object,
    Realm& realm,
    std::string* error_msg
) {
    if (!module) {
        if (error_msg) *error_msg = "Module is null";
        return nullptr;
    }

    auto instance = std::make_shared<WasmInstance>(module, realm);

    // 1. Resolve imports
    for (const auto& imp : module->Imports()) {
        TaggedValue mod_val = TaggedValue::Undefined();
        if (import_object.IsHeapObject() && import_object.AsHeapObject()->IsJSObject()) {
            auto* obj = static_cast<JSObject*>(import_object.AsHeapObject());
            mod_val = obj->GetNamedProperty(imp.module_name);
        }

        TaggedValue field_val = TaggedValue::Undefined();
        if (mod_val.IsHeapObject() && mod_val.AsHeapObject()->IsJSObject()) {
            auto* obj = static_cast<JSObject*>(mod_val.AsHeapObject());
            field_val = obj->GetNamedProperty(imp.field_name);
        }

        switch (imp.kind) {
            case WasmExternalKind::Function: {
                if (!field_val.IsHeapObject() || !field_val.AsHeapObject()->IsJSObject()) {
                    if (error_msg) *error_msg = "Imported function not provided: " + imp.module_name + "." + imp.field_name;
                    return nullptr;
                }
                const WasmFuncType& sig = module->Types()[imp.type_idx];
                instance->imported_functions_.push_back([field_val, &realm, sig](const std::vector<WasmVal>& args) -> std::vector<WasmVal> {
                    std::vector<TaggedValue> js_args;
                    for (const auto& a : args) js_args.push_back(a.ToJS(realm));
                    TaggedValue ret = CallFunction(field_val, TaggedValue::Undefined(), js_args);
                    std::vector<WasmVal> out_results;
                    if (!sig.results.empty()) {
                        out_results.push_back(WasmVal::FromJS(ret, sig.results[0]));
                    }
                    return out_results;
                });
                break;
            }
            case WasmExternalKind::Table: {
                if (field_val.IsHeapObject() && s_table_registry.contains(static_cast<JSObject*>(field_val.AsHeapObject()))) {
                    instance->tables_.push_back(s_table_registry[static_cast<JSObject*>(field_val.AsHeapObject())]);
                } else {
                    instance->tables_.push_back(std::make_shared<WasmTableInstance>(imp.table_desc));
                }
                break;
            }
            case WasmExternalKind::Memory: {
                if (field_val.IsHeapObject() && s_memory_registry.contains(static_cast<JSObject*>(field_val.AsHeapObject()))) {
                    instance->memories_.push_back(s_memory_registry[static_cast<JSObject*>(field_val.AsHeapObject())]);
                } else {
                    instance->memories_.push_back(std::make_shared<WasmMemoryInstance>(imp.mem_desc.limits));
                }
                break;
            }
            case WasmExternalKind::Global: {
                if (field_val.IsHeapObject() && s_global_registry.contains(static_cast<JSObject*>(field_val.AsHeapObject()))) {
                    instance->globals_.push_back(s_global_registry[static_cast<JSObject*>(field_val.AsHeapObject())]);
                } else {
                    WasmVal init_val = WasmVal::FromJS(field_val, imp.global_desc.content_type);
                    instance->globals_.push_back(std::make_shared<WasmGlobalInstance>(imp.global_desc, init_val));
                }
                break;
            }
        }
    }

    // 2. Instantiate declared tables, memories, and globals
    for (const auto& tt : module->Tables()) {
        instance->tables_.push_back(std::make_shared<WasmTableInstance>(tt));
    }
    for (const auto& mt : module->Memories()) {
        instance->memories_.push_back(std::make_shared<WasmMemoryInstance>(mt.limits));
    }
    for (const auto& gd : module->Globals()) {
        WasmVal init_val = instance->EvaluateInitExpr(gd.init_expr);
        instance->globals_.push_back(std::make_shared<WasmGlobalInstance>(gd.type, init_val));
    }

    // 3. Populate Element segments
    for (const auto& elem : module->Elements()) {
        if (elem.table_index >= instance->tables_.size()) continue;
        WasmVal offset_val = instance->EvaluateInitExpr(elem.offset_expr);
        uint32_t offset = static_cast<uint32_t>(offset_val.i32);
        auto& tbl = instance->tables_[elem.table_index];
        for (size_t i = 0; i < elem.func_indices.size(); ++i) {
            tbl->Set(offset + static_cast<uint32_t>(i), elem.func_indices[i]);
        }
    }

    // 4. Populate Data segments
    for (const auto& data : module->DataSegments()) {
        if (data.memory_index >= instance->memories_.size()) continue;
        WasmVal offset_val = instance->EvaluateInitExpr(data.offset_expr);
        uint32_t offset = static_cast<uint32_t>(offset_val.i32);
        auto& mem = instance->memories_[data.memory_index];
        for (size_t i = 0; i < data.data.size(); ++i) {
            mem->Store8(offset + static_cast<uint32_t>(i), data.data[i]);
        }
    }

    // 5. Construct `exports` JSObject
    Shape* root = realm.GetRootShape();
    uint32_t& next_sid = realm.NextShapeId();
    instance->exports_object_ = AllocateRuntime<JSObject>(root);

    for (const auto& exp : module->Exports()) {
        TaggedValue exported_val = TaggedValue::Undefined();
        switch (exp.kind) {
            case WasmExternalKind::Function: {
                uint32_t fidx = exp.index;
                auto host_fn = AllocateRuntime<JSNativeFunction>(exp.name, [instance, fidx, &realm](const std::vector<TaggedValue>& args) -> TaggedValue {
                    const auto& sig = instance->module_->GetFunctionSignature(fidx);
                    std::vector<WasmVal> wasm_args;
                    for (size_t i = 0; i < sig.params.size(); ++i) {
                        TaggedValue jarg = (i < args.size()) ? args[i] : TaggedValue::FromSmi(0);
                        wasm_args.push_back(WasmVal::FromJS(jarg, sig.params[i]));
                    }
                    auto res = instance->Execute(fidx, wasm_args);
                    if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) {
                        return TaggedValue::Undefined();
                    }
                    if (res.empty()) return TaggedValue::Undefined();
                    return res[0].ToJS(realm);
                }, root);
                exported_val = TaggedValue::FromHeapObject(host_fn);
                break;
            }
            case WasmExternalKind::Memory: {
                if (exp.index < instance->memories_.size()) {
                    auto mem_inst = instance->memories_[exp.index];
                    auto* mem_obj = AllocateRuntime<JSObject>(root);
                    s_memory_registry[mem_obj] = mem_inst;

                    // buffer getter
                    mem_obj->SetNamedProperty("buffer", TaggedValue::FromHeapObject(mem_inst->GetOrCreateJSBuffer(realm)), next_sid);
                    // grow method
                    mem_obj->SetNamedProperty("grow", TaggedValue::FromHeapObject(
                        AllocateRuntime<JSNativeFunction>("grow", [mem_inst, &realm, mem_obj, &next_sid](const std::vector<TaggedValue>& args) -> TaggedValue {
                            uint32_t delta = args.empty() ? 0 : static_cast<uint32_t>(args[0].AsNumber());
                            int32_t prev = mem_inst->Grow(delta);
                            if (prev < 0) {
                                if (Interpreter::Current()) {
                                    Interpreter::Current()->SetPendingException(
                                        TaggedValue::FromHeapObject(new JSError("RangeError", "WebAssembly.Memory.grow failed")));
                                }
                                return TaggedValue::Undefined();
                            }
                            // Refresh buffer property
                            mem_obj->SetNamedProperty("buffer", TaggedValue::FromHeapObject(mem_inst->GetOrCreateJSBuffer(realm)), next_sid);
                            return TaggedValue::FromSmi(prev);
                        }, root)), next_sid);
                    exported_val = TaggedValue::FromHeapObject(mem_obj);
                }
                break;
            }
            case WasmExternalKind::Table: {
                if (exp.index < instance->tables_.size()) {
                    auto tbl_inst = instance->tables_[exp.index];
                    auto* tbl_obj = AllocateRuntime<JSObject>(root);
                    s_table_registry[tbl_obj] = tbl_inst;
                    tbl_obj->SetNamedProperty("length", TaggedValue::FromSmi(tbl_inst->Length()), next_sid);
                    tbl_obj->SetNamedProperty("get", TaggedValue::FromHeapObject(
                        AllocateRuntime<JSNativeFunction>("get", [tbl_inst](const std::vector<TaggedValue>& args) -> TaggedValue {
                            uint32_t idx = args.empty() ? 0 : static_cast<uint32_t>(args[0].AsNumber());
                            uint32_t f = tbl_inst->Get(idx);
                            if (f == 0xFFFFFFFF) return TaggedValue::Null();
                            return TaggedValue::FromSmi(f);
                        }, root)), next_sid);
                    tbl_obj->SetNamedProperty("set", TaggedValue::FromHeapObject(
                        AllocateRuntime<JSNativeFunction>("set", [tbl_inst](const std::vector<TaggedValue>& args) -> TaggedValue {
                            uint32_t idx = args.empty() ? 0 : static_cast<uint32_t>(args[0].AsNumber());
                            uint32_t f = args.size() > 1 ? static_cast<uint32_t>(args[1].AsNumber()) : 0xFFFFFFFF;
                            tbl_inst->Set(idx, f);
                            return TaggedValue::Undefined();
                        }, root)), next_sid);
                    exported_val = TaggedValue::FromHeapObject(tbl_obj);
                }
                break;
            }
            case WasmExternalKind::Global: {
                if (exp.index < instance->globals_.size()) {
                    auto g_inst = instance->globals_[exp.index];
                    auto* g_obj = AllocateRuntime<JSObject>(root);
                    s_global_registry[g_obj] = g_inst;
                    g_obj->SetNamedProperty("value", g_inst->Get().ToJS(realm), next_sid);
                    exported_val = TaggedValue::FromHeapObject(g_obj);
                }
                break;
            }
        }
        instance->exports_object_->SetNamedProperty(exp.name, exported_val, next_sid);
    }

    // 6. Execute Start function if present
    if (module->StartFunction().has_value()) {
        instance->Execute(module->StartFunction().value(), {});
    }

    return instance;
}

// ============================================================================
// WasmExecutionEngine Execution Loop
// ============================================================================

std::vector<WasmVal> WasmInstance::Execute(uint32_t func_index, const std::vector<WasmVal>& args) {
    // 1. If imported function, forward to host callback
    if (func_index < module_->NumImportedFunctions()) {
        return imported_functions_[func_index](args);
    }

    // 2. Otherwise defined in module
    uint32_t def_idx = func_index - module_->NumImportedFunctions();
    if (def_idx >= module_->Functions().size()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(
                TaggedValue::FromHeapObject(new JSError("WebAssembly.RuntimeError", "Function index out of bounds: " + std::to_string(func_index))));
        }
        return {};
    }

    const auto& fn_desc = module_->Functions()[def_idx];
    const auto& sig = module_->Types()[fn_desc.type_index];

    // Setup local variables: params followed by declared locals
    std::vector<WasmVal> locals;
    locals.reserve(sig.params.size() + 16);
    for (size_t i = 0; i < sig.params.size(); ++i) {
        if (i < args.size()) locals.push_back(args[i]);
        else locals.push_back(WasmVal::MakeI32(0));
    }
    for (const auto& [count, type] : fn_desc.locals) {
        for (uint32_t c = 0; c < count; ++c) {
            switch (type) {
                case WasmValType::I32: locals.push_back(WasmVal::MakeI32(0)); break;
                case WasmValType::I64: locals.push_back(WasmVal::MakeI64(0)); break;
                case WasmValType::F32: locals.push_back(WasmVal::MakeF32(0.0f)); break;
                case WasmValType::F64: locals.push_back(WasmVal::MakeF64(0.0)); break;
                default: locals.push_back(WasmVal::MakeI32(0)); break;
            }
        }
    }

    const uint8_t* code = fn_desc.code.data();
    size_t code_len = fn_desc.code.size();
    auto block_map = AnalyzeBlocks(code, code_len);

    std::vector<WasmVal> stack;
    std::vector<ControlFrame> control_stack;

    auto Trap = [this](std::string_view msg) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(
                TaggedValue::FromHeapObject(new JSError("WebAssembly.RuntimeError", msg)));
        }
    };

    size_t pc = 0;
    while (pc < code_len) {
        uint8_t op_byte = code[pc++];
        auto op = static_cast<WasmOpcode>(op_byte);

        switch (op) {
            case WasmOpcode::Unreachable: {
                Trap("unreachable executed");
                return {};
            }
            case WasmOpcode::Nop:
                break;

            case WasmOpcode::Block:
            case WasmOpcode::Loop: {
                uint8_t btype_byte = code[pc++];
                WasmValType btype = WasmValType::Void;
                if (btype_byte != 0x40) btype = static_cast<WasmValType>(btype_byte);

                size_t opener = pc - 2;
                control_stack.push_back({
                    op,
                    (op == WasmOpcode::Loop ? pc : block_map[opener].end_pc),
                    block_map[opener].end_pc,
                    stack.size(),
                    btype
                });
                break;
            }

            case WasmOpcode::If: {
                uint8_t btype_byte = code[pc++];
                WasmValType btype = WasmValType::Void;
                if (btype_byte != 0x40) btype = static_cast<WasmValType>(btype_byte);

                if (stack.empty()) { Trap("Stack underflow on if"); return {}; }
                int32_t cond = stack.back().i32;
                stack.pop_back();

                size_t opener = pc - 2;
                auto meta = block_map[opener];

                if (cond != 0) {
                    control_stack.push_back({
                        WasmOpcode::If,
                        meta.end_pc,
                        meta.end_pc,
                        stack.size(),
                        btype
                    });
                } else {
                    if (meta.else_pc != 0) {
                        pc = meta.else_pc + 1; // jump to code inside else
                        control_stack.push_back({
                            WasmOpcode::If,
                            meta.end_pc,
                            meta.end_pc,
                            stack.size(),
                            btype
                        });
                    } else {
                        pc = meta.end_pc + 1; // skip if completely
                    }
                }
                break;
            }

            case WasmOpcode::Else: {
                if (!control_stack.empty()) {
                    pc = control_stack.back().end_pc; // jump to matching End
                }
                break;
            }

            case WasmOpcode::End: {
                if (control_stack.empty()) {
                    // Function body finished
                    pc = code_len;
                    break;
                }
                auto frame = control_stack.back();
                control_stack.pop_back();
                if (frame.result_type != WasmValType::Void && !stack.empty()) {
                    WasmVal res = stack.back();
                    while (stack.size() > frame.stack_depth) stack.pop_back();
                    stack.push_back(res);
                }
                break;
            }

            case WasmOpcode::Br: {
                uint32_t depth = 0;
                const uint8_t* p = code + pc;
                DecodeU32(p, code + code_len, depth);
                pc = p - code;

                if (depth >= control_stack.size()) {
                    return stack.empty() ? std::vector<WasmVal>{} : std::vector<WasmVal>{stack.back()};
                }

                size_t target_idx = control_stack.size() - 1 - depth;
                auto target = control_stack[target_idx];
                WasmVal res;
                bool has_res = (target.result_type != WasmValType::Void && !stack.empty());
                if (has_res) res = stack.back();

                if (target.opcode == WasmOpcode::Loop) {
                    while (control_stack.size() > target_idx + 1) {
                        control_stack.pop_back();
                    }
                    while (stack.size() > target.stack_depth) stack.pop_back();
                    pc = target.start_pc;
                } else {
                    while (control_stack.size() > target_idx) {
                        control_stack.pop_back();
                    }
                    while (stack.size() > target.stack_depth) stack.pop_back();
                    if (has_res) stack.push_back(res);
                    pc = target.end_pc + 1;
                }
                break;
            }

            case WasmOpcode::BrIf: {
                uint32_t depth = 0;
                const uint8_t* p = code + pc;
                DecodeU32(p, code + code_len, depth);
                pc = p - code;

                if (stack.empty()) { Trap("Stack underflow on br_if"); return {}; }
                int32_t cond = stack.back().i32;
                stack.pop_back();

                if (cond != 0) {
                    if (depth >= control_stack.size()) {
                        return stack.empty() ? std::vector<WasmVal>{} : std::vector<WasmVal>{stack.back()};
                    }

                    size_t target_idx = control_stack.size() - 1 - depth;
                    auto target = control_stack[target_idx];
                    WasmVal res;
                    bool has_res = (target.result_type != WasmValType::Void && !stack.empty());
                    if (has_res) res = stack.back();

                    if (target.opcode == WasmOpcode::Loop) {
                        while (control_stack.size() > target_idx + 1) {
                            control_stack.pop_back();
                        }
                        while (stack.size() > target.stack_depth) stack.pop_back();
                        pc = target.start_pc;
                    } else {
                        while (control_stack.size() > target_idx) {
                            control_stack.pop_back();
                        }
                        while (stack.size() > target.stack_depth) stack.pop_back();
                        if (has_res) stack.push_back(res);
                        pc = target.end_pc + 1;
                    }
                }
                break;
            }

            case WasmOpcode::BrTable: {
                const uint8_t* p = code + pc;
                uint32_t count = 0;
                DecodeU32(p, code + code_len, count);
                std::vector<uint32_t> table;
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t t = 0;
                    DecodeU32(p, code + code_len, t);
                    table.push_back(t);
                }
                uint32_t def_target = 0;
                DecodeU32(p, code + code_len, def_target);
                pc = p - code;

                if (stack.empty()) { Trap("Stack underflow on br_table"); return {}; }
                int32_t idx = stack.back().i32;
                stack.pop_back();

                uint32_t depth = (idx >= 0 && static_cast<uint32_t>(idx) < count) ? table[idx] : def_target;
                if (depth >= control_stack.size()) {
                    return stack.empty() ? std::vector<WasmVal>{} : std::vector<WasmVal>{stack.back()};
                }

                size_t target_idx = control_stack.size() - 1 - depth;
                auto target = control_stack[target_idx];
                WasmVal res;
                bool has_res = (target.result_type != WasmValType::Void && !stack.empty());
                if (has_res) res = stack.back();

                if (target.opcode == WasmOpcode::Loop) {
                    while (control_stack.size() > target_idx + 1) {
                        control_stack.pop_back();
                    }
                    while (stack.size() > target.stack_depth) stack.pop_back();
                    pc = target.start_pc;
                } else {
                    while (control_stack.size() > target_idx) {
                        control_stack.pop_back();
                    }
                    while (stack.size() > target.stack_depth) stack.pop_back();
                    if (has_res) stack.push_back(res);
                    pc = target.end_pc + 1;
                }
                break;
            }

            case WasmOpcode::Return: {
                return stack.empty() ? std::vector<WasmVal>{} : std::vector<WasmVal>{stack.back()};
            }

            case WasmOpcode::Call: {
                uint32_t target_fidx = 0;
                const uint8_t* p = code + pc;
                DecodeU32(p, code + code_len, target_fidx);
                pc = p - code;

                const auto& target_sig = module_->GetFunctionSignature(target_fidx);
                std::vector<WasmVal> call_args(target_sig.params.size());
                for (size_t i = target_sig.params.size(); i > 0; --i) {
                    if (stack.empty()) { Trap("Stack underflow on call args"); return {}; }
                    call_args[i - 1] = stack.back();
                    stack.pop_back();
                }

                auto res = Execute(target_fidx, call_args);
                if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return {};
                for (const auto& r : res) stack.push_back(r);
                break;
            }

            case WasmOpcode::CallIndirect: {
                uint32_t target_tidx = 0;
                const uint8_t* p = code + pc;
                DecodeU32(p, code + code_len, target_tidx);
                if (p < code + code_len) p++; // 0x00 reserved
                pc = p - code;

                if (tables_.empty()) { Trap("No table present for call_indirect"); return {}; }
                if (stack.empty()) { Trap("Stack underflow on call_indirect index"); return {}; }
                uint32_t elem_idx = static_cast<uint32_t>(stack.back().i32);
                stack.pop_back();

                const auto& tbl = tables_[0];
                if (elem_idx >= tbl->Length()) { Trap("undefined element in table"); return {}; }
                uint32_t fidx = tbl->Get(elem_idx);
                if (fidx == 0xFFFFFFFF) { Trap("uninitialized element in table"); return {}; }

                const auto& actual_sig = module_->GetFunctionSignature(fidx);
                const auto& expected_sig = module_->Types()[target_tidx];
                if (actual_sig != expected_sig) { Trap("indirect call type mismatch"); return {}; }

                std::vector<WasmVal> call_args(expected_sig.params.size());
                for (size_t i = expected_sig.params.size(); i > 0; --i) {
                    if (stack.empty()) { Trap("Stack underflow on call_indirect args"); return {}; }
                    call_args[i - 1] = stack.back();
                    stack.pop_back();
                }

                auto res = Execute(fidx, call_args);
                if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return {};
                for (const auto& r : res) stack.push_back(r);
                break;
            }

            case WasmOpcode::Drop: {
                if (!stack.empty()) stack.pop_back();
                break;
            }

            case WasmOpcode::Select: {
                if (stack.size() < 3) { Trap("Stack underflow on select"); return {}; }
                int32_t cond = stack.back().i32; stack.pop_back();
                WasmVal v2 = stack.back(); stack.pop_back();
                WasmVal v1 = stack.back(); stack.pop_back();
                stack.push_back(cond != 0 ? v1 : v2);
                break;
            }

            case WasmOpcode::LocalGet: {
                uint32_t lidx = 0;
                const uint8_t* p = code + pc;
                DecodeU32(p, code + code_len, lidx);
                pc = p - code;
                if (lidx >= locals.size()) { Trap("Local index out of range"); return {}; }
                stack.push_back(locals[lidx]);
                break;
            }

            case WasmOpcode::LocalSet: {
                uint32_t lidx = 0;
                const uint8_t* p = code + pc;
                DecodeU32(p, code + code_len, lidx);
                pc = p - code;
                if (stack.empty() || lidx >= locals.size()) { Trap("Stack underflow on local.set"); return {}; }
                locals[lidx] = stack.back();
                stack.pop_back();
                break;
            }

            case WasmOpcode::LocalTee: {
                uint32_t lidx = 0;
                const uint8_t* p = code + pc;
                DecodeU32(p, code + code_len, lidx);
                pc = p - code;
                if (stack.empty() || lidx >= locals.size()) { Trap("Stack underflow on local.tee"); return {}; }
                locals[lidx] = stack.back();
                break;
            }

            case WasmOpcode::GlobalGet: {
                uint32_t gidx = 0;
                const uint8_t* p = code + pc;
                DecodeU32(p, code + code_len, gidx);
                pc = p - code;
                if (gidx >= globals_.size()) { Trap("Global index out of range"); return {}; }
                stack.push_back(globals_[gidx]->Get());
                break;
            }

            case WasmOpcode::GlobalSet: {
                uint32_t gidx = 0;
                const uint8_t* p = code + pc;
                DecodeU32(p, code + code_len, gidx);
                pc = p - code;
                if (stack.empty() || gidx >= globals_.size()) { Trap("Stack underflow on global.set"); return {}; }
                globals_[gidx]->Set(stack.back());
                stack.pop_back();
                break;
            }

            case WasmOpcode::I32Const: {
                int32_t v = 0;
                const uint8_t* p = code + pc;
                DecodeI32(p, code + code_len, v);
                pc = p - code;
                stack.push_back(WasmVal::MakeI32(v));
                break;
            }

            case WasmOpcode::I64Const: {
                int64_t v = 0;
                const uint8_t* p = code + pc;
                DecodeI64(p, code + code_len, v);
                pc = p - code;
                stack.push_back(WasmVal::MakeI64(v));
                break;
            }

            case WasmOpcode::F32Const: {
                float v = 0.0f;
                if (pc + 4 <= code_len) std::memcpy(&v, code + pc, 4);
                pc += 4;
                stack.push_back(WasmVal::MakeF32(v));
                break;
            }

            case WasmOpcode::F64Const: {
                double v = 0.0;
                if (pc + 8 <= code_len) std::memcpy(&v, code + pc, 8);
                pc += 8;
                stack.push_back(WasmVal::MakeF64(v));
                break;
            }

            // ================================================================
            // Memory Load / Store Operations
            // ================================================================
            case WasmOpcode::I32Load:
            case WasmOpcode::I64Load:
            case WasmOpcode::F32Load:
            case WasmOpcode::F64Load:
            case WasmOpcode::I32Load8S:
            case WasmOpcode::I32Load8U:
            case WasmOpcode::I32Load16S:
            case WasmOpcode::I32Load16U: {
                uint32_t align = 0, offset = 0;
                const uint8_t* p = code + pc;
                DecodeU32(p, code + code_len, align);
                DecodeU32(p, code + code_len, offset);
                pc = p - code;

                if (memories_.empty()) { Trap("No linear memory available"); return {}; }
                if (stack.empty()) { Trap("Stack underflow on memory load"); return {}; }
                uint32_t base = static_cast<uint32_t>(stack.back().i32);
                stack.pop_back();

                uint64_t eff = static_cast<uint64_t>(base) + offset;
                if (eff > 0xFFFFFFFF) { Trap("out of bounds memory access"); return {}; }
                uint32_t addr = static_cast<uint32_t>(eff);

                const auto& mem = memories_[0];
                if (op == WasmOpcode::I32Load) {
                    int32_t val = 0;
                    if (!mem->LoadI32(addr, val)) { Trap("out of bounds memory access"); return {}; }
                    stack.push_back(WasmVal::MakeI32(val));
                } else if (op == WasmOpcode::I64Load) {
                    int64_t val = 0;
                    if (!mem->LoadI64(addr, val)) { Trap("out of bounds memory access"); return {}; }
                    stack.push_back(WasmVal::MakeI64(val));
                } else if (op == WasmOpcode::F32Load) {
                    float val = 0.0f;
                    if (!mem->LoadF32(addr, val)) { Trap("out of bounds memory access"); return {}; }
                    stack.push_back(WasmVal::MakeF32(val));
                } else if (op == WasmOpcode::F64Load) {
                    double val = 0.0;
                    if (!mem->LoadF64(addr, val)) { Trap("out of bounds memory access"); return {}; }
                    stack.push_back(WasmVal::MakeF64(val));
                } else if (op == WasmOpcode::I32Load8S) {
                    int32_t val = 0;
                    if (!mem->Load8S(addr, val)) { Trap("out of bounds memory access"); return {}; }
                    stack.push_back(WasmVal::MakeI32(val));
                } else if (op == WasmOpcode::I32Load8U) {
                    int32_t val = 0;
                    if (!mem->Load8U(addr, val)) { Trap("out of bounds memory access"); return {}; }
                    stack.push_back(WasmVal::MakeI32(val));
                } else if (op == WasmOpcode::I32Load16S) {
                    int32_t val = 0;
                    if (!mem->Load16S(addr, val)) { Trap("out of bounds memory access"); return {}; }
                    stack.push_back(WasmVal::MakeI32(val));
                } else if (op == WasmOpcode::I32Load16U) {
                    int32_t val = 0;
                    if (!mem->Load16U(addr, val)) { Trap("out of bounds memory access"); return {}; }
                    stack.push_back(WasmVal::MakeI32(val));
                }
                break;
            }

            case WasmOpcode::I32Store:
            case WasmOpcode::I64Store:
            case WasmOpcode::F32Store:
            case WasmOpcode::F64Store:
            case WasmOpcode::I32Store8:
            case WasmOpcode::I32Store16: {
                uint32_t align = 0, offset = 0;
                const uint8_t* p = code + pc;
                DecodeU32(p, code + code_len, align);
                DecodeU32(p, code + code_len, offset);
                pc = p - code;

                if (memories_.empty()) { Trap("No linear memory available"); return {}; }
                if (stack.size() < 2) { Trap("Stack underflow on memory store"); return {}; }
                WasmVal val = stack.back(); stack.pop_back();
                uint32_t base = static_cast<uint32_t>(stack.back().i32); stack.pop_back();

                uint64_t eff = static_cast<uint64_t>(base) + offset;
                if (eff > 0xFFFFFFFF) { Trap("out of bounds memory access"); return {}; }
                uint32_t addr = static_cast<uint32_t>(eff);

                const auto& mem = memories_[0];
                bool ok = false;
                if (op == WasmOpcode::I32Store) ok = mem->StoreI32(addr, val.i32);
                else if (op == WasmOpcode::I64Store) ok = mem->StoreI64(addr, val.i64);
                else if (op == WasmOpcode::F32Store) ok = mem->StoreF32(addr, val.f32);
                else if (op == WasmOpcode::F64Store) ok = mem->StoreF64(addr, val.f64);
                else if (op == WasmOpcode::I32Store8) ok = mem->Store8(addr, static_cast<uint8_t>(val.i32));
                else if (op == WasmOpcode::I32Store16) ok = mem->Store16(addr, static_cast<uint16_t>(val.i32));

                if (!ok) { Trap("out of bounds memory access"); return {}; }
                break;
            }

            case WasmOpcode::MemorySize: {
                if (pc < code_len) pc++; // 0x00 reserved
                uint32_t sz = memories_.empty() ? 0 : memories_[0]->SizePages();
                stack.push_back(WasmVal::MakeI32(static_cast<int32_t>(sz)));
                break;
            }

            case WasmOpcode::MemoryGrow: {
                if (pc < code_len) pc++; // 0x00 reserved
                if (stack.empty() || memories_.empty()) {
                    stack.push_back(WasmVal::MakeI32(-1));
                    break;
                }
                uint32_t delta = static_cast<uint32_t>(stack.back().i32);
                stack.pop_back();
                int32_t prev = memories_[0]->Grow(delta);
                stack.push_back(WasmVal::MakeI32(prev));
                break;
            }

            // ================================================================
            // Numeric Operations: i32
            // ================================================================
            case WasmOpcode::I32Eqz: {
                if (stack.empty()) return {};
                stack.back().i32 = (stack.back().i32 == 0) ? 1 : 0;
                break;
            }
            case WasmOpcode::I32Eq:
            case WasmOpcode::I32Ne:
            case WasmOpcode::I32LtS:
            case WasmOpcode::I32LtU:
            case WasmOpcode::I32GtS:
            case WasmOpcode::I32GtU:
            case WasmOpcode::I32LeS:
            case WasmOpcode::I32LeU:
            case WasmOpcode::I32GeS:
            case WasmOpcode::I32GeU: {
                if (stack.size() < 2) return {};
                int32_t b = stack.back().i32; stack.pop_back();
                int32_t a = stack.back().i32;
                int32_t res = 0;
                if (op == WasmOpcode::I32Eq) res = (a == b);
                else if (op == WasmOpcode::I32Ne) res = (a != b);
                else if (op == WasmOpcode::I32LtS) res = (a < b);
                else if (op == WasmOpcode::I32LtU) res = (static_cast<uint32_t>(a) < static_cast<uint32_t>(b));
                else if (op == WasmOpcode::I32GtS) res = (a > b);
                else if (op == WasmOpcode::I32GtU) res = (static_cast<uint32_t>(a) > static_cast<uint32_t>(b));
                else if (op == WasmOpcode::I32LeS) res = (a <= b);
                else if (op == WasmOpcode::I32LeU) res = (static_cast<uint32_t>(a) <= static_cast<uint32_t>(b));
                else if (op == WasmOpcode::I32GeS) res = (a >= b);
                else if (op == WasmOpcode::I32GeU) res = (static_cast<uint32_t>(a) >= static_cast<uint32_t>(b));
                stack.back().i32 = res;
                break;
            }

            case WasmOpcode::I32Clz: {
                if (stack.empty()) return {};
                stack.back().i32 = std::countl_zero(static_cast<uint32_t>(stack.back().i32));
                break;
            }
            case WasmOpcode::I32Ctz: {
                if (stack.empty()) return {};
                stack.back().i32 = std::countr_zero(static_cast<uint32_t>(stack.back().i32));
                break;
            }
            case WasmOpcode::I32Popcnt: {
                if (stack.empty()) return {};
                stack.back().i32 = std::popcount(static_cast<uint32_t>(stack.back().i32));
                break;
            }

            case WasmOpcode::I32Add: {
                if (stack.size() < 2) return {};
                int32_t b = stack.back().i32; stack.pop_back();
                stack.back().i32 += b;
                break;
            }
            case WasmOpcode::I32Sub: {
                if (stack.size() < 2) return {};
                int32_t b = stack.back().i32; stack.pop_back();
                stack.back().i32 -= b;
                break;
            }
            case WasmOpcode::I32Mul: {
                if (stack.size() < 2) return {};
                int32_t b = stack.back().i32; stack.pop_back();
                stack.back().i32 *= b;
                break;
            }
            case WasmOpcode::I32DivS: {
                if (stack.size() < 2) return {};
                int32_t b = stack.back().i32; stack.pop_back();
                int32_t a = stack.back().i32;
                if (b == 0) { Trap("integer divide by zero"); return {}; }
                if (a == std::numeric_limits<int32_t>::min() && b == -1) { Trap("integer overflow"); return {}; }
                stack.back().i32 = a / b;
                break;
            }
            case WasmOpcode::I32DivU: {
                if (stack.size() < 2) return {};
                uint32_t b = static_cast<uint32_t>(stack.back().i32); stack.pop_back();
                uint32_t a = static_cast<uint32_t>(stack.back().i32);
                if (b == 0) { Trap("integer divide by zero"); return {}; }
                stack.back().i32 = static_cast<int32_t>(a / b);
                break;
            }
            case WasmOpcode::I32RemS: {
                if (stack.size() < 2) return {};
                int32_t b = stack.back().i32; stack.pop_back();
                int32_t a = stack.back().i32;
                if (b == 0) { Trap("integer divide by zero"); return {}; }
                if (a == std::numeric_limits<int32_t>::min() && b == -1) {
                    stack.back().i32 = 0;
                } else {
                    stack.back().i32 = a % b;
                }
                break;
            }
            case WasmOpcode::I32RemU: {
                if (stack.size() < 2) return {};
                uint32_t b = static_cast<uint32_t>(stack.back().i32); stack.pop_back();
                uint32_t a = static_cast<uint32_t>(stack.back().i32);
                if (b == 0) { Trap("integer divide by zero"); return {}; }
                stack.back().i32 = static_cast<int32_t>(a % b);
                break;
            }
            case WasmOpcode::I32And: {
                if (stack.size() < 2) return {};
                int32_t b = stack.back().i32; stack.pop_back();
                stack.back().i32 &= b;
                break;
            }
            case WasmOpcode::I32Or: {
                if (stack.size() < 2) return {};
                int32_t b = stack.back().i32; stack.pop_back();
                stack.back().i32 |= b;
                break;
            }
            case WasmOpcode::I32Xor: {
                if (stack.size() < 2) return {};
                int32_t b = stack.back().i32; stack.pop_back();
                stack.back().i32 ^= b;
                break;
            }
            case WasmOpcode::I32Shl: {
                if (stack.size() < 2) return {};
                uint32_t b = static_cast<uint32_t>(stack.back().i32) % 32; stack.pop_back();
                stack.back().i32 = static_cast<int32_t>(static_cast<uint32_t>(stack.back().i32) << b);
                break;
            }
            case WasmOpcode::I32ShrS: {
                if (stack.size() < 2) return {};
                uint32_t b = static_cast<uint32_t>(stack.back().i32) % 32; stack.pop_back();
                stack.back().i32 >>= b;
                break;
            }
            case WasmOpcode::I32ShrU: {
                if (stack.size() < 2) return {};
                uint32_t b = static_cast<uint32_t>(stack.back().i32) % 32; stack.pop_back();
                stack.back().i32 = static_cast<int32_t>(static_cast<uint32_t>(stack.back().i32) >> b);
                break;
            }
            case WasmOpcode::I32Rotl: {
                if (stack.size() < 2) return {};
                uint32_t b = static_cast<uint32_t>(stack.back().i32); stack.pop_back();
                uint32_t a = static_cast<uint32_t>(stack.back().i32);
                stack.back().i32 = static_cast<int32_t>(std::rotl(a, b));
                break;
            }
            case WasmOpcode::I32Rotr: {
                if (stack.size() < 2) return {};
                uint32_t b = static_cast<uint32_t>(stack.back().i32); stack.pop_back();
                uint32_t a = static_cast<uint32_t>(stack.back().i32);
                stack.back().i32 = static_cast<int32_t>(std::rotr(a, b));
                break;
            }

            // ================================================================
            // Floating Point Operations: f64 & Conversions
            // ================================================================
            case WasmOpcode::F64Add: {
                if (stack.size() < 2) return {};
                double b = stack.back().f64; stack.pop_back();
                stack.back().f64 += b;
                break;
            }
            case WasmOpcode::F64Sub: {
                if (stack.size() < 2) return {};
                double b = stack.back().f64; stack.pop_back();
                stack.back().f64 -= b;
                break;
            }
            case WasmOpcode::F64Mul: {
                if (stack.size() < 2) return {};
                double b = stack.back().f64; stack.pop_back();
                stack.back().f64 *= b;
                break;
            }
            case WasmOpcode::F64Div: {
                if (stack.size() < 2) return {};
                double b = stack.back().f64; stack.pop_back();
                stack.back().f64 /= b;
                break;
            }
            case WasmOpcode::F64Sqrt: {
                if (stack.empty()) return {};
                stack.back().f64 = std::sqrt(stack.back().f64);
                break;
            }
            case WasmOpcode::F64Abs: {
                if (stack.empty()) return {};
                stack.back().f64 = std::fabs(stack.back().f64);
                break;
            }
            case WasmOpcode::F64Neg: {
                if (stack.empty()) return {};
                stack.back().f64 = -stack.back().f64;
                break;
            }
            case WasmOpcode::F64ConvertI32S: {
                if (stack.empty()) return {};
                int32_t val = stack.back().i32;
                stack.back().type = WasmValType::F64;
                stack.back().f64 = static_cast<double>(val);
                break;
            }
            case WasmOpcode::I32TruncF64S: {
                if (stack.empty()) return {};
                double val = stack.back().f64;
                if (std::isnan(val) || val > 2147483647.0 || val < -2147483648.0) {
                    Trap("invalid conversion to integer");
                    return {};
                }
                stack.back().type = WasmValType::I32;
                stack.back().i32 = static_cast<int32_t>(val);
                break;
            }

            default:
                break;
        }
    }

    return stack.empty() ? std::vector<WasmVal>{} : std::vector<WasmVal>{stack.back()};
}

// ============================================================================
// Realm Intrinsics Initialization: WebAssembly Namespace
// ============================================================================

void InitializeWebAssembly(Realm& realm) {
    Shape* root = realm.GetRootShape();
    uint32_t& next_sid = realm.NextShapeId();

    auto make_native = [&realm, root](std::string_view name, auto cb) -> JSNativeFunction* {
        return AllocateRuntime<JSNativeFunction>(name, cb, root);
    };

    auto* wasm_obj = AllocateRuntime<JSObject>(root);

    // ========================================================================
    // Error Constructors: CompileError, RuntimeError, LinkError
    // ========================================================================
    auto make_error_ctor = [&realm, root, &next_sid](std::string_view err_name) -> JSNativeFunction* {
        auto* proto = AllocateRuntime<JSObject>(root);
        auto* ctor = AllocateRuntime<JSNativeFunction>(err_name, [err_name, root](const std::vector<TaggedValue>& args) -> TaggedValue {
            std::string msg = args.empty() ? "" : args[0].ToString();
            return TaggedValue::FromHeapObject(new JSError(err_name, msg, root));
        }, root);
        ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(proto), next_sid);
        proto->SetNamedProperty("constructor", TaggedValue::FromHeapObject(ctor), next_sid);
        proto->SetNamedProperty("name", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(err_name)), next_sid);
        return ctor;
    };

    auto* compile_error_ctor = make_error_ctor("WebAssembly.CompileError");
    auto* runtime_error_ctor = make_error_ctor("WebAssembly.RuntimeError");
    auto* link_error_ctor = make_error_ctor("WebAssembly.LinkError");

    wasm_obj->SetNamedProperty("CompileError", TaggedValue::FromHeapObject(compile_error_ctor), next_sid);
    wasm_obj->SetNamedProperty("RuntimeError", TaggedValue::FromHeapObject(runtime_error_ctor), next_sid);
    wasm_obj->SetNamedProperty("LinkError", TaggedValue::FromHeapObject(link_error_ctor), next_sid);

    // ========================================================================
    // WebAssembly.validate(bufferSource)
    // ========================================================================
    wasm_obj->SetNamedProperty("validate", TaggedValue::FromHeapObject(
        make_native("validate", [](const std::vector<TaggedValue>& args) -> TaggedValue {
            if (args.empty()) return TaggedValue::FromBoolean(false);
            const uint8_t* data = nullptr;
            size_t size = 0;
            if (!ExtractBufferSource(args[0], data, size)) return TaggedValue::FromBoolean(false);
            return TaggedValue::FromBoolean(WasmModule::Validate(data, size));
        })), next_sid);

    // ========================================================================
    // WebAssembly.Module Constructor and Statics
    // ========================================================================
    auto* module_proto = AllocateRuntime<JSObject>(root);
    auto* module_ctor = make_native("Module", [&realm, root, module_proto, &next_sid](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(
                    TaggedValue::FromHeapObject(new JSError("TypeError", "WebAssembly.Module expects buffer source argument")));
            }
            return TaggedValue::Undefined();
        }
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!ExtractBufferSource(args[0], data, size)) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(
                    TaggedValue::FromHeapObject(new JSError("TypeError", "Argument 0 must be an ArrayBuffer or TypedArray")));
            }
            return TaggedValue::Undefined();
        }

        std::string err;
        auto mod = WasmModule::Decode(data, size, &err);
        if (!mod) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(
                    TaggedValue::FromHeapObject(new JSError("WebAssembly.CompileError", err)));
            }
            return TaggedValue::Undefined();
        }

        auto* mod_obj = AllocateRuntime<JSObject>(root);
        mod_obj->SetNamedProperty("__proto__", TaggedValue::FromHeapObject(module_proto), next_sid);
        s_module_registry[mod_obj] = mod;
        return TaggedValue::FromHeapObject(mod_obj);
    });

    module_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(module_proto), next_sid);
    module_proto->SetNamedProperty("constructor", TaggedValue::FromHeapObject(module_ctor), next_sid);

    module_ctor->SetNamedProperty("exports", TaggedValue::FromHeapObject(
        make_native("exports", [&realm, root, &next_sid](const std::vector<TaggedValue>& args) -> TaggedValue {
            if (args.empty() || !args[0].IsHeapObject() || !s_module_registry.contains(static_cast<JSObject*>(args[0].AsHeapObject()))) {
                if (Interpreter::Current()) {
                    Interpreter::Current()->SetPendingException(
                        TaggedValue::FromHeapObject(new JSError("TypeError", "Expected WebAssembly.Module")));
                }
                return TaggedValue::Undefined();
            }
            auto mod = s_module_registry[static_cast<JSObject*>(args[0].AsHeapObject())];
            auto* arr = AllocateRuntime<JSArray>(root);
            for (const auto& exp : mod->Exports()) {
                auto* item = AllocateRuntime<JSObject>(root);
                item->SetNamedProperty("name", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(exp.name)), next_sid);
                item->SetNamedProperty("kind", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(std::string(WasmExternalKindName(exp.kind)))), next_sid);
                arr->Push(TaggedValue::FromHeapObject(item));
            }
            return TaggedValue::FromHeapObject(arr);
        })), next_sid);

    module_ctor->SetNamedProperty("imports", TaggedValue::FromHeapObject(
        make_native("imports", [&realm, root, &next_sid](const std::vector<TaggedValue>& args) -> TaggedValue {
            if (args.empty() || !args[0].IsHeapObject() || !s_module_registry.contains(static_cast<JSObject*>(args[0].AsHeapObject()))) {
                if (Interpreter::Current()) {
                    Interpreter::Current()->SetPendingException(
                        TaggedValue::FromHeapObject(new JSError("TypeError", "Expected WebAssembly.Module")));
                }
                return TaggedValue::Undefined();
            }
            auto mod = s_module_registry[static_cast<JSObject*>(args[0].AsHeapObject())];
            auto* arr = AllocateRuntime<JSArray>(root);
            for (const auto& imp : mod->Imports()) {
                auto* item = AllocateRuntime<JSObject>(root);
                item->SetNamedProperty("module", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(imp.module_name)), next_sid);
                item->SetNamedProperty("name", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(imp.field_name)), next_sid);
                item->SetNamedProperty("kind", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(std::string(WasmExternalKindName(imp.kind)))), next_sid);
                arr->Push(TaggedValue::FromHeapObject(item));
            }
            return TaggedValue::FromHeapObject(arr);
        })), next_sid);

    module_ctor->SetNamedProperty("customSections", TaggedValue::FromHeapObject(
        make_native("customSections", [&realm, root](const std::vector<TaggedValue>& args) -> TaggedValue {
            if (args.size() < 2 || !args[0].IsHeapObject() || !s_module_registry.contains(static_cast<JSObject*>(args[0].AsHeapObject()))) {
                return TaggedValue::FromHeapObject(AllocateRuntime<JSArray>(root));
            }
            auto mod = s_module_registry[static_cast<JSObject*>(args[0].AsHeapObject())];
            std::string sec_name = args[1].ToString();
            auto* arr = AllocateRuntime<JSArray>(root);
            for (const auto& cs : mod->CustomSections()) {
                if (cs.name == sec_name) {
                    auto* ab = AllocateRuntime<JSArrayBuffer>(cs.data.size(), root);
                    if (!cs.data.empty()) std::memcpy(ab->Data(), cs.data.data(), cs.data.size());
                    arr->Push(TaggedValue::FromHeapObject(ab));
                }
            }
            return TaggedValue::FromHeapObject(arr);
        })), next_sid);

    wasm_obj->SetNamedProperty("Module", TaggedValue::FromHeapObject(module_ctor), next_sid);

    // ========================================================================
    // WebAssembly.Instance Constructor
    // ========================================================================
    auto* instance_proto = AllocateRuntime<JSObject>(root);
    auto* instance_ctor = make_native("Instance", [&realm, root, instance_proto, &next_sid](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || !s_module_registry.contains(static_cast<JSObject*>(args[0].AsHeapObject()))) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(
                    TaggedValue::FromHeapObject(new JSError("TypeError", "WebAssembly.Instance expects WebAssembly.Module as argument 0")));
            }
            return TaggedValue::Undefined();
        }
        auto mod = s_module_registry[static_cast<JSObject*>(args[0].AsHeapObject())];
        TaggedValue import_obj = args.size() > 1 ? args[1] : TaggedValue::Undefined();

        std::string err;
        auto inst = WasmInstance::Instantiate(mod, import_obj, realm, &err);
        if (!inst) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(
                    TaggedValue::FromHeapObject(new JSError("WebAssembly.LinkError", err)));
            }
            return TaggedValue::Undefined();
        }

        auto* inst_obj = AllocateRuntime<JSObject>(root);
        inst_obj->SetNamedProperty("__proto__", TaggedValue::FromHeapObject(instance_proto), next_sid);
        inst_obj->SetNamedProperty("exports", TaggedValue::FromHeapObject(inst->GetExportsObject()), next_sid);
        s_instance_registry[inst_obj] = inst;
        return TaggedValue::FromHeapObject(inst_obj);
    });

    instance_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(instance_proto), next_sid);
    instance_proto->SetNamedProperty("constructor", TaggedValue::FromHeapObject(instance_ctor), next_sid);
    wasm_obj->SetNamedProperty("Instance", TaggedValue::FromHeapObject(instance_ctor), next_sid);

    // ========================================================================
    // WebAssembly.Memory Constructor
    // ========================================================================
    auto* memory_proto = AllocateRuntime<JSObject>(root);
    auto* memory_ctor = make_native("Memory", [&realm, root, memory_proto, &next_sid](const std::vector<TaggedValue>& args) -> TaggedValue {
        WasmLimits limits;
        limits.initial = 1;
        if (!args.empty() && args[0].IsHeapObject() && args[0].AsHeapObject()->IsJSObject()) {
            auto* desc = static_cast<JSObject*>(args[0].AsHeapObject());
            if (desc->HasNamedProperty("initial")) {
                limits.initial = static_cast<uint32_t>(desc->GetNamedProperty("initial").AsNumber());
            }
            if (desc->HasNamedProperty("maximum")) {
                limits.has_max = true;
                limits.maximum = static_cast<uint32_t>(desc->GetNamedProperty("maximum").AsNumber());
            }
        }
        auto mem_inst = std::make_shared<WasmMemoryInstance>(limits);
        auto* mem_obj = AllocateRuntime<JSObject>(root);
        mem_obj->SetNamedProperty("__proto__", TaggedValue::FromHeapObject(memory_proto), next_sid);
        mem_obj->SetNamedProperty("buffer", TaggedValue::FromHeapObject(mem_inst->GetOrCreateJSBuffer(realm)), next_sid);
        s_memory_registry[mem_obj] = mem_inst;

        mem_obj->SetNamedProperty("grow", TaggedValue::FromHeapObject(
            AllocateRuntime<JSNativeFunction>("grow", [mem_inst, &realm, mem_obj, &next_sid](const std::vector<TaggedValue>& args) -> TaggedValue {
                uint32_t delta = args.empty() ? 0 : static_cast<uint32_t>(args[0].AsNumber());
                int32_t prev = mem_inst->Grow(delta);
                if (prev < 0) {
                    if (Interpreter::Current()) {
                        Interpreter::Current()->SetPendingException(
                            TaggedValue::FromHeapObject(new JSError("RangeError", "WebAssembly.Memory.grow failed")));
                    }
                    return TaggedValue::Undefined();
                }
                mem_obj->SetNamedProperty("buffer", TaggedValue::FromHeapObject(mem_inst->GetOrCreateJSBuffer(realm)), next_sid);
                return TaggedValue::FromSmi(prev);
            }, root)), next_sid);

        return TaggedValue::FromHeapObject(mem_obj);
    });

    memory_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(memory_proto), next_sid);
    memory_proto->SetNamedProperty("constructor", TaggedValue::FromHeapObject(memory_ctor), next_sid);
    wasm_obj->SetNamedProperty("Memory", TaggedValue::FromHeapObject(memory_ctor), next_sid);

    // ========================================================================
    // WebAssembly.Table Constructor
    // ========================================================================
    auto* table_proto = AllocateRuntime<JSObject>(root);
    auto* table_ctor = make_native("Table", [&realm, root, table_proto, &next_sid](const std::vector<TaggedValue>& args) -> TaggedValue {
        WasmTableType tt;
        tt.elem_type = WasmValType::FuncRef;
        tt.limits.initial = 0;
        if (!args.empty() && args[0].IsHeapObject() && args[0].AsHeapObject()->IsJSObject()) {
            auto* desc = static_cast<JSObject*>(args[0].AsHeapObject());
            if (desc->HasNamedProperty("initial")) {
                tt.limits.initial = static_cast<uint32_t>(desc->GetNamedProperty("initial").AsNumber());
            }
            if (desc->HasNamedProperty("maximum")) {
                tt.limits.has_max = true;
                tt.limits.maximum = static_cast<uint32_t>(desc->GetNamedProperty("maximum").AsNumber());
            }
        }
        auto tbl_inst = std::make_shared<WasmTableInstance>(tt);
        auto* tbl_obj = AllocateRuntime<JSObject>(root);
        tbl_obj->SetNamedProperty("__proto__", TaggedValue::FromHeapObject(table_proto), next_sid);
        tbl_obj->SetNamedProperty("length", TaggedValue::FromSmi(tbl_inst->Length()), next_sid);
        s_table_registry[tbl_obj] = tbl_inst;

        tbl_obj->SetNamedProperty("get", TaggedValue::FromHeapObject(
            AllocateRuntime<JSNativeFunction>("get", [tbl_inst](const std::vector<TaggedValue>& args) -> TaggedValue {
                uint32_t idx = args.empty() ? 0 : static_cast<uint32_t>(args[0].AsNumber());
                uint32_t f = tbl_inst->Get(idx);
                if (f == 0xFFFFFFFF) return TaggedValue::Null();
                return TaggedValue::FromSmi(f);
            }, root)), next_sid);

        tbl_obj->SetNamedProperty("set", TaggedValue::FromHeapObject(
            AllocateRuntime<JSNativeFunction>("set", [tbl_inst](const std::vector<TaggedValue>& args) -> TaggedValue {
                uint32_t idx = args.empty() ? 0 : static_cast<uint32_t>(args[0].AsNumber());
                uint32_t f = args.size() > 1 ? static_cast<uint32_t>(args[1].AsNumber()) : 0xFFFFFFFF;
                tbl_inst->Set(idx, f);
                return TaggedValue::Undefined();
            }, root)), next_sid);

        return TaggedValue::FromHeapObject(tbl_obj);
    });

    table_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(table_proto), next_sid);
    table_proto->SetNamedProperty("constructor", TaggedValue::FromHeapObject(table_ctor), next_sid);
    wasm_obj->SetNamedProperty("Table", TaggedValue::FromHeapObject(table_ctor), next_sid);

    // ========================================================================
    // WebAssembly.Global Constructor
    // ========================================================================
    auto* global_proto = AllocateRuntime<JSObject>(root);
    auto* global_ctor = make_native("Global", [&realm, root, global_proto, &next_sid](const std::vector<TaggedValue>& args) -> TaggedValue {
        WasmGlobalType gt;
        gt.content_type = WasmValType::I32;
        gt.is_mutable = false;
        if (!args.empty() && args[0].IsHeapObject() && args[0].AsHeapObject()->IsJSObject()) {
            auto* desc = static_cast<JSObject*>(args[0].AsHeapObject());
            if (desc->HasNamedProperty("value")) {
                std::string vtype = desc->GetNamedProperty("value").ToString();
                if (vtype == "i64") gt.content_type = WasmValType::I64;
                else if (vtype == "f32") gt.content_type = WasmValType::F32;
                else if (vtype == "f64") gt.content_type = WasmValType::F64;
                else gt.content_type = WasmValType::I32;
            }
            if (desc->HasNamedProperty("mutable")) {
                gt.is_mutable = desc->GetNamedProperty("mutable").ToBoolean();
            }
        }
        TaggedValue init_val = args.size() > 1 ? args[1] : TaggedValue::FromSmi(0);
        auto g_inst = std::make_shared<WasmGlobalInstance>(gt, WasmVal::FromJS(init_val, gt.content_type));

        auto* g_obj = AllocateRuntime<JSObject>(root);
        g_obj->SetNamedProperty("__proto__", TaggedValue::FromHeapObject(global_proto), next_sid);
        g_obj->SetNamedProperty("value", g_inst->Get().ToJS(realm), next_sid);
        s_global_registry[g_obj] = g_inst;

        g_obj->SetNamedProperty("valueOf", TaggedValue::FromHeapObject(
            AllocateRuntime<JSNativeFunction>("valueOf", [g_inst, &realm](const std::vector<TaggedValue>&) -> TaggedValue {
                return g_inst->Get().ToJS(realm);
            }, root)), next_sid);

        return TaggedValue::FromHeapObject(g_obj);
    });

    global_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(global_proto), next_sid);
    global_proto->SetNamedProperty("constructor", TaggedValue::FromHeapObject(global_ctor), next_sid);
    wasm_obj->SetNamedProperty("Global", TaggedValue::FromHeapObject(global_ctor), next_sid);

    // ========================================================================
    // WebAssembly.compile(bufferSource) -> Promise<WebAssembly.Module>
    // ========================================================================
    wasm_obj->SetNamedProperty("compile", TaggedValue::FromHeapObject(
        make_native("compile", [&realm, root, module_proto, &next_sid](const std::vector<TaggedValue>& args) -> TaggedValue {
            auto* promise = AllocateRuntime<JSPromise>(root);
            if (args.empty()) {
                promise->Reject(TaggedValue::FromHeapObject(new JSError("TypeError", "Missing buffer source")), CurrentMicrotaskQueue());
                return TaggedValue::FromHeapObject(promise);
            }
            const uint8_t* data = nullptr;
            size_t size = 0;
            if (!ExtractBufferSource(args[0], data, size)) {
                promise->Reject(TaggedValue::FromHeapObject(new JSError("TypeError", "Argument 0 must be an ArrayBuffer or TypedArray")), CurrentMicrotaskQueue());
                return TaggedValue::FromHeapObject(promise);
            }

            std::string err;
            auto mod = WasmModule::Decode(data, size, &err);
            if (!mod) {
                promise->Reject(TaggedValue::FromHeapObject(new JSError("WebAssembly.CompileError", err)), CurrentMicrotaskQueue());
                return TaggedValue::FromHeapObject(promise);
            }

            auto* mod_obj = AllocateRuntime<JSObject>(root);
            mod_obj->SetNamedProperty("__proto__", TaggedValue::FromHeapObject(module_proto), next_sid);
            s_module_registry[mod_obj] = mod;
            promise->Fulfill(TaggedValue::FromHeapObject(mod_obj), CurrentMicrotaskQueue());
            return TaggedValue::FromHeapObject(promise);
        })), next_sid);

    // ========================================================================
    // WebAssembly.instantiate(bufferSourceOrModule, importObject) -> Promise
    // ========================================================================
    wasm_obj->SetNamedProperty("instantiate", TaggedValue::FromHeapObject(
        make_native("instantiate", [&realm, root, module_proto, instance_proto, &next_sid](const std::vector<TaggedValue>& args) -> TaggedValue {
            auto* promise = AllocateRuntime<JSPromise>(root);
            if (args.empty()) {
                promise->Reject(TaggedValue::FromHeapObject(new JSError("TypeError", "Missing argument 0")), CurrentMicrotaskQueue());
                return TaggedValue::FromHeapObject(promise);
            }

            TaggedValue import_obj = args.size() > 1 ? args[1] : TaggedValue::Undefined();

            // Case A: Passed an existing WebAssembly.Module
            if (args[0].IsHeapObject() && s_module_registry.contains(static_cast<JSObject*>(args[0].AsHeapObject()))) {
                auto mod = s_module_registry[static_cast<JSObject*>(args[0].AsHeapObject())];
                std::string err;
                auto inst = WasmInstance::Instantiate(mod, import_obj, realm, &err);
                if (!inst) {
                    promise->Reject(TaggedValue::FromHeapObject(new JSError("WebAssembly.LinkError", err)), CurrentMicrotaskQueue());
                    return TaggedValue::FromHeapObject(promise);
                }
                auto* inst_obj = AllocateRuntime<JSObject>(root);
                inst_obj->SetNamedProperty("__proto__", TaggedValue::FromHeapObject(instance_proto), next_sid);
                inst_obj->SetNamedProperty("exports", TaggedValue::FromHeapObject(inst->GetExportsObject()), next_sid);
                s_instance_registry[inst_obj] = inst;
                promise->Fulfill(TaggedValue::FromHeapObject(inst_obj), CurrentMicrotaskQueue());
                return TaggedValue::FromHeapObject(promise);
            }

            // Case B: Passed raw buffer source (compiles and instantiates)
            const uint8_t* data = nullptr;
            size_t size = 0;
            if (!ExtractBufferSource(args[0], data, size)) {
                promise->Reject(TaggedValue::FromHeapObject(new JSError("TypeError", "Argument 0 must be WebAssembly.Module or buffer source")), CurrentMicrotaskQueue());
                return TaggedValue::FromHeapObject(promise);
            }

            std::string err;
            auto mod = WasmModule::Decode(data, size, &err);
            if (!mod) {
                promise->Reject(TaggedValue::FromHeapObject(new JSError("WebAssembly.CompileError", err)), CurrentMicrotaskQueue());
                return TaggedValue::FromHeapObject(promise);
            }

            auto* mod_obj = AllocateRuntime<JSObject>(root);
            mod_obj->SetNamedProperty("__proto__", TaggedValue::FromHeapObject(module_proto), next_sid);
            s_module_registry[mod_obj] = mod;

            auto inst = WasmInstance::Instantiate(mod, import_obj, realm, &err);
            if (!inst) {
                promise->Reject(TaggedValue::FromHeapObject(new JSError("WebAssembly.LinkError", err)), CurrentMicrotaskQueue());
                return TaggedValue::FromHeapObject(promise);
            }

            auto* inst_obj = AllocateRuntime<JSObject>(root);
            inst_obj->SetNamedProperty("__proto__", TaggedValue::FromHeapObject(instance_proto), next_sid);
            inst_obj->SetNamedProperty("exports", TaggedValue::FromHeapObject(inst->GetExportsObject()), next_sid);
            s_instance_registry[inst_obj] = inst;

            auto* result_pair = AllocateRuntime<JSObject>(root);
            result_pair->SetNamedProperty("module", TaggedValue::FromHeapObject(mod_obj), next_sid);
            result_pair->SetNamedProperty("instance", TaggedValue::FromHeapObject(inst_obj), next_sid);
            promise->Fulfill(TaggedValue::FromHeapObject(result_pair), CurrentMicrotaskQueue());
            return TaggedValue::FromHeapObject(promise);
        })), next_sid);

    realm.GetGlobalObject()->SetNamedProperty("WebAssembly", TaggedValue::FromHeapObject(wasm_obj), next_sid);
}

} // namespace aether
