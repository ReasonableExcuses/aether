#ifndef AETHER_WASM_H
#define AETHER_WASM_H

#include "aether/common.h"
#include "aether/value.h"
#include "aether/heap_object.h"
#include "aether/typed_array.h"
#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <optional>
#include <unordered_map>
#include <functional>

namespace aether {

class Realm;

// ============================================================================
// WebAssembly Binary Types, Limits, and Enums (Wasm 1.0 MVP)
// ============================================================================

enum class WasmValType : uint8_t {
    I32     = 0x7F,
    I64     = 0x7E,
    F32     = 0x7D,
    F64     = 0x7C,
    FuncRef = 0x70,
    Void    = 0x40
};

inline std::string_view WasmValTypeName(WasmValType t) noexcept {
    switch (t) {
        case WasmValType::I32:     return "i32";
        case WasmValType::I64:     return "i64";
        case WasmValType::F32:     return "f32";
        case WasmValType::F64:     return "f64";
        case WasmValType::FuncRef: return "funcref";
        case WasmValType::Void:    return "void";
    }
    return "unknown";
}

enum class WasmExternalKind : uint8_t {
    Function = 0x00,
    Table    = 0x01,
    Memory   = 0x02,
    Global   = 0x03
};

inline std::string_view WasmExternalKindName(WasmExternalKind k) noexcept {
    switch (k) {
        case WasmExternalKind::Function: return "function";
        case WasmExternalKind::Table:    return "table";
        case WasmExternalKind::Memory:   return "memory";
        case WasmExternalKind::Global:   return "global";
    }
    return "unknown";
}

enum class WasmOpcode : uint8_t {
    // Control flow
    Unreachable   = 0x00,
    Nop           = 0x01,
    Block         = 0x02,
    Loop          = 0x03,
    If            = 0x04,
    Else          = 0x05,
    End           = 0x0B,
    Br            = 0x0C,
    BrIf          = 0x0D,
    BrTable       = 0x0E,
    Return        = 0x0F,
    Call          = 0x10,
    CallIndirect  = 0x11,

    // Parametric
    Drop          = 0x1A,
    Select        = 0x1B,

    // Variables
    LocalGet      = 0x20,
    LocalSet      = 0x21,
    LocalTee      = 0x22,
    GlobalGet     = 0x23,
    GlobalSet     = 0x24,

    // Memory operations
    I32Load       = 0x28,
    I64Load       = 0x29,
    F32Load       = 0x2A,
    F64Load       = 0x2B,
    I32Load8S     = 0x2C,
    I32Load8U     = 0x2D,
    I32Load16S    = 0x2E,
    I32Load16U    = 0x2F,
    I64Load8S     = 0x30,
    I64Load8U     = 0x31,
    I64Load16S    = 0x32,
    I64Load16U    = 0x33,
    I64Load32S    = 0x34,
    I64Load32U    = 0x35,
    I32Store      = 0x36,
    I64Store      = 0x37,
    F32Store      = 0x38,
    F64Store      = 0x39,
    I32Store8     = 0x3A,
    I32Store16    = 0x3B,
    I64Store8     = 0x3C,
    I64Store16    = 0x3D,
    I64Store32    = 0x3E,
    MemorySize    = 0x3F,
    MemoryGrow    = 0x40,

    // Constants
    I32Const      = 0x41,
    I64Const      = 0x42,
    F32Const      = 0x43,
    F64Const      = 0x44,

    // Comparisons
    I32Eqz        = 0x45,
    I32Eq         = 0x46,
    I32Ne         = 0x47,
    I32LtS        = 0x48,
    I32LtU        = 0x49,
    I32GtS        = 0x4A,
    I32GtU        = 0x4B,
    I32LeS        = 0x4C,
    I32LeU        = 0x4D,
    I32GeS        = 0x4E,
    I32GeU        = 0x4F,

    I64Eqz        = 0x50,
    I64Eq         = 0x51,
    I64Ne         = 0x52,
    I64LtS        = 0x53,
    I64LtU        = 0x54,
    I64GtS        = 0x55,
    I64GtU        = 0x56,
    I64LeS        = 0x57,
    I64LeU        = 0x58,
    I64GeS        = 0x59,
    I64GeU        = 0x5A,

    F32Eq         = 0x5B,
    F32Ne         = 0x5C,
    F32Lt         = 0x5D,
    F32Gt         = 0x5E,
    F32Le         = 0x5F,
    F32Ge         = 0x60,

    F64Eq         = 0x61,
    F64Ne         = 0x62,
    F64Lt         = 0x63,
    F64Gt         = 0x64,
    F64Le         = 0x65,
    F64Ge         = 0x66,

    // Numerics (i32)
    I32Clz        = 0x67,
    I32Ctz        = 0x68,
    I32Popcnt     = 0x69,
    I32Add        = 0x6A,
    I32Sub        = 0x6B,
    I32Mul        = 0x6C,
    I32DivS       = 0x6D,
    I32DivU       = 0x6E,
    I32RemS       = 0x6F,
    I32RemU       = 0x70,
    I32And        = 0x71,
    I32Or         = 0x72,
    I32Xor        = 0x73,
    I32Shl        = 0x74,
    I32ShrS       = 0x75,
    I32ShrU       = 0x76,
    I32Rotl       = 0x77,
    I32Rotr       = 0x78,

    // Numerics (i64)
    I64Clz        = 0x79,
    I64Ctz        = 0x7A,
    I64Popcnt     = 0x7B,
    I64Add        = 0x7C,
    I64Sub        = 0x7D,
    I64Mul        = 0x7E,
    I64DivS       = 0x7F,
    I64DivU       = 0x80,
    I64RemS       = 0x81,
    I64RemU       = 0x82,
    I64And        = 0x83,
    I64Or         = 0x84,
    I64Xor        = 0x85,
    I64Shl        = 0x86,
    I64ShrS       = 0x87,
    I64ShrU       = 0x88,
    I64Rotl       = 0x89,
    I64Rotr       = 0x8A,

    // Float numerics (f32)
    F32Abs        = 0x8B,
    F32Neg        = 0x8C,
    F32Ceil       = 0x8D,
    F32Floor      = 0x8E,
    F32Trunc      = 0x8F,
    F32Nearest    = 0x90,
    F32Sqrt       = 0x91,
    F32Add        = 0x92,
    F32Sub        = 0x93,
    F32Mul        = 0x94,
    F32Div        = 0x95,
    F32Min        = 0x96,
    F32Max        = 0x97,
    F32Copysign   = 0x98,

    // Float numerics (f64)
    F64Abs        = 0x99,
    F64Neg        = 0x9A,
    F64Ceil       = 0x9B,
    F64Floor      = 0x9C,
    F64Trunc      = 0x9D,
    F64Nearest    = 0x9E,
    F64Sqrt       = 0x9F,
    F64Add        = 0xA0,
    F64Sub        = 0xA1,
    F64Mul        = 0xA2,
    F64Div        = 0xA3,
    F64Min        = 0xA4,
    F64Max        = 0xA5,
    F64Copysign   = 0xA6,

    // Conversions
    I32WrapI64    = 0xA7,
    I32TruncF32S  = 0xA8,
    I32TruncF32U  = 0xA9,
    I32TruncF64S  = 0xAA,
    I32TruncF64U  = 0xAB,
    I64ExtendI32S = 0xAC,
    I64ExtendI32U = 0xAD,
    I64TruncF32S  = 0xAE,
    I64TruncF32U  = 0xAF,
    I64TruncF64S  = 0xB0,
    I64TruncF64U  = 0xB1,
    F32ConvertI32S= 0xB2,
    F32ConvertI32U= 0xB3,
    F32ConvertI64S= 0xB4,
    F32ConvertI64U= 0xB5,
    F32DemoteF64  = 0xB6,
    F64ConvertI32S= 0xB7,
    F64ConvertI32U= 0xB8,
    F64ConvertI64S= 0xB9,
    F64ConvertI64U= 0xBA,
    F64PromoteF32 = 0xBB,
    I32ReinterpretF32 = 0xBC,
    I64ReinterpretF64 = 0xBD,
    F32ReinterpretI32 = 0xBE,
    F64ReinterpretI64 = 0xBF
};

// ============================================================================
// Core WebAssembly Runtime Value Representation
// ============================================================================

struct WasmVal {
    WasmValType type{WasmValType::I32};
    union {
        int32_t i32;
        int64_t i64;
        float f32;
        double f64;
    };

    WasmVal() : type(WasmValType::I32), i32(0) {}

    static WasmVal MakeI32(int32_t v) noexcept {
        WasmVal val; val.type = WasmValType::I32; val.i32 = v; return val;
    }
    static WasmVal MakeI64(int64_t v) noexcept {
        WasmVal val; val.type = WasmValType::I64; val.i64 = v; return val;
    }
    static WasmVal MakeF32(float v) noexcept {
        WasmVal val; val.type = WasmValType::F32; val.f32 = v; return val;
    }
    static WasmVal MakeF64(double v) noexcept {
        WasmVal val; val.type = WasmValType::F64; val.f64 = v; return val;
    }

    TaggedValue ToJS(Realm& realm) const;
    static WasmVal FromJS(TaggedValue val, WasmValType expected_type);
};

// ============================================================================
// Module Type Descriptors & Structure Definitions
// ============================================================================

struct WasmLimits {
    uint32_t initial{0};
    bool has_max{false};
    uint32_t maximum{0};
};

struct WasmFuncType {
    std::vector<WasmValType> params;
    std::vector<WasmValType> results;

    bool operator==(const WasmFuncType& other) const noexcept {
        return params == other.params && results == other.results;
    }
};

struct WasmGlobalType {
    WasmValType content_type{WasmValType::I32};
    bool is_mutable{false};
};

struct WasmTableType {
    WasmValType elem_type{WasmValType::FuncRef};
    WasmLimits limits;
};

struct WasmMemoryType {
    WasmLimits limits;
};

struct WasmImport {
    std::string module_name;
    std::string field_name;
    WasmExternalKind kind{WasmExternalKind::Function};
    uint32_t type_idx{0};          // if Function
    WasmTableType table_desc;       // if Table
    WasmMemoryType mem_desc;        // if Memory
    WasmGlobalType global_desc;     // if Global
};

struct WasmExport {
    std::string name;
    WasmExternalKind kind{WasmExternalKind::Function};
    uint32_t index{0};
};

struct WasmGlobalDesc {
    WasmGlobalType type;
    std::vector<uint8_t> init_expr;
};

struct WasmFuncDesc {
    uint32_t type_index{0};
    std::vector<std::pair<uint32_t, WasmValType>> locals; // (count, type)
    std::vector<uint8_t> code;
};

struct WasmElemSegment {
    uint32_t table_index{0};
    std::vector<uint8_t> offset_expr;
    std::vector<uint32_t> func_indices;
};

struct WasmDataSegment {
    uint32_t memory_index{0};
    std::vector<uint8_t> offset_expr;
    std::vector<uint8_t> data;
};

struct WasmCustomSection {
    std::string name;
    std::vector<uint8_t> data;
};

// ============================================================================
// WasmModule: Decoded Binary Representation
// ============================================================================

class WasmModule {
public:
    static std::shared_ptr<WasmModule> Decode(const uint8_t* data, size_t length, std::string* error_msg = nullptr);
    static bool Validate(const uint8_t* data, size_t length);

    const std::vector<WasmFuncType>& Types() const noexcept { return types_; }
    const std::vector<WasmImport>& Imports() const noexcept { return imports_; }
    const std::vector<uint32_t>& FunctionTypeIndices() const noexcept { return function_type_indices_; }
    const std::vector<WasmTableType>& Tables() const noexcept { return tables_; }
    const std::vector<WasmMemoryType>& Memories() const noexcept { return memories_; }
    const std::vector<WasmGlobalDesc>& Globals() const noexcept { return globals_; }
    const std::vector<WasmExport>& Exports() const noexcept { return exports_; }
    const std::optional<uint32_t>& StartFunction() const noexcept { return start_function_; }
    const std::vector<WasmElemSegment>& Elements() const noexcept { return elements_; }
    const std::vector<WasmFuncDesc>& Functions() const noexcept { return functions_; }
    const std::vector<WasmDataSegment>& DataSegments() const noexcept { return data_segments_; }
    const std::vector<WasmCustomSection>& CustomSections() const noexcept { return custom_sections_; }

    uint32_t NumImportedFunctions() const noexcept { return num_imported_funcs_; }
    uint32_t NumImportedTables() const noexcept { return num_imported_tables_; }
    uint32_t NumImportedMemories() const noexcept { return num_imported_memories_; }
    uint32_t NumImportedGlobals() const noexcept { return num_imported_globals_; }

    const WasmFuncType& GetFunctionSignature(uint32_t func_idx) const;

private:
    std::vector<WasmFuncType> types_;
    std::vector<WasmImport> imports_;
    std::vector<uint32_t> function_type_indices_;
    std::vector<WasmTableType> tables_;
    std::vector<WasmMemoryType> memories_;
    std::vector<WasmGlobalDesc> globals_;
    std::vector<WasmExport> exports_;
    std::optional<uint32_t> start_function_;
    std::vector<WasmElemSegment> elements_;
    std::vector<WasmFuncDesc> functions_;
    std::vector<WasmDataSegment> data_segments_;
    std::vector<WasmCustomSection> custom_sections_;

    uint32_t num_imported_funcs_{0};
    uint32_t num_imported_tables_{0};
    uint32_t num_imported_memories_{0};
    uint32_t num_imported_globals_{0};
};

// ============================================================================
// WebAssembly Runtime Instances (Memory, Table, Global, Instance)
// ============================================================================

class WasmMemoryInstance {
public:
    static constexpr size_t kPageSize = 65536; // 64 KiB
    static constexpr size_t kMaxPages = 65536; // 4 GiB max

    explicit WasmMemoryInstance(WasmLimits limits);
    ~WasmMemoryInstance() = default;

    uint32_t SizePages() const noexcept { return static_cast<uint32_t>(data_.size() / kPageSize); }
    size_t SizeBytes() const noexcept { return data_.size(); }
    const uint8_t* Data() const noexcept { return data_.data(); }
    uint8_t* Data() noexcept { return data_.data(); }

    int32_t Grow(uint32_t delta_pages);

    JSArrayBuffer* GetOrCreateJSBuffer(Realm& realm);

    // Memory load/store helpers (little-endian)
    bool LoadI32(uint32_t offset, int32_t& out_val) const;
    bool LoadI64(uint32_t offset, int64_t& out_val) const;
    bool LoadF32(uint32_t offset, float& out_val) const;
    bool LoadF64(uint32_t offset, double& out_val) const;
    bool Load8S(uint32_t offset, int32_t& out_val) const;
    bool Load8U(uint32_t offset, int32_t& out_val) const;
    bool Load16S(uint32_t offset, int32_t& out_val) const;
    bool Load16U(uint32_t offset, int32_t& out_val) const;

    bool StoreI32(uint32_t offset, int32_t val);
    bool StoreI64(uint32_t offset, int64_t val);
    bool StoreF32(uint32_t offset, float val);
    bool StoreF64(uint32_t offset, double val);
    bool Store8(uint32_t offset, uint8_t val);
    bool Store16(uint32_t offset, uint16_t val);

    const WasmLimits& Limits() const noexcept { return limits_; }

private:
    WasmLimits limits_;
    std::vector<uint8_t> data_;
    JSArrayBuffer* js_buffer_{nullptr};
};

class WasmTableInstance {
public:
    explicit WasmTableInstance(WasmTableType type);

    uint32_t Length() const noexcept { return static_cast<uint32_t>(elements_.size()); }
    uint32_t Get(uint32_t index) const;
    bool Set(uint32_t index, uint32_t func_idx);
    int32_t Grow(uint32_t delta, uint32_t init_val = 0xFFFFFFFF);

    const WasmTableType& Type() const noexcept { return type_; }

private:
    WasmTableType type_;
    std::vector<uint32_t> elements_; // 0xFFFFFFFF represents null/uninitialized
};

class WasmGlobalInstance {
public:
    WasmGlobalInstance(WasmGlobalType type, WasmVal initial_val)
        : type_(type), value_(initial_val) {}

    const WasmGlobalType& Type() const noexcept { return type_; }
    WasmVal Get() const noexcept { return value_; }
    void Set(WasmVal val) noexcept { if (type_.is_mutable) value_ = val; }

private:
    WasmGlobalType type_;
    WasmVal value_;
};

// ============================================================================
// WasmInstance: Instantiated Module Bound to Imports and State
// ============================================================================

class WasmInstance {
public:
    using HostFunctionCallback = std::function<std::vector<WasmVal>(const std::vector<WasmVal>&)>;

    WasmInstance(std::shared_ptr<WasmModule> module, Realm& realm)
        : module_(module), realm_(realm) {}

    static std::shared_ptr<WasmInstance> Instantiate(
        std::shared_ptr<WasmModule> module,
        TaggedValue import_object,
        Realm& realm,
        std::string* error_msg = nullptr
    );

    std::shared_ptr<WasmModule> Module() const noexcept { return module_; }
    JSObject* GetExportsObject() const noexcept { return exports_object_; }

    // Execution entry points
    std::vector<WasmVal> Execute(uint32_t func_index, const std::vector<WasmVal>& args);

    const std::vector<std::shared_ptr<WasmMemoryInstance>>& Memories() const noexcept { return memories_; }
    const std::vector<std::shared_ptr<WasmTableInstance>>& Tables() const noexcept { return tables_; }
    const std::vector<std::shared_ptr<WasmGlobalInstance>>& Globals() const noexcept { return globals_; }

private:
    std::shared_ptr<WasmModule> module_;
    Realm& realm_;
    JSObject* exports_object_{nullptr};

    std::vector<HostFunctionCallback> imported_functions_;
    std::vector<std::shared_ptr<WasmMemoryInstance>> memories_;
    std::vector<std::shared_ptr<WasmTableInstance>> tables_;
    std::vector<std::shared_ptr<WasmGlobalInstance>> globals_;

    WasmVal EvaluateInitExpr(const std::vector<uint8_t>& expr);
};

// ============================================================================
// WebAssembly Realm Intrinsics Initialization
// ============================================================================

void InitializeWebAssembly(Realm& realm);

} // namespace aether

#endif // AETHER_WASM_H
