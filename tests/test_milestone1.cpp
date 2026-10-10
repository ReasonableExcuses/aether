#include "aether/common.h"
#include "aether/value.h"
#include "aether/shape.h"
#include "aether/heap_object.h"
#include "aether/bytecode.h"
#include "aether/interpreter.h"
#include "aether/ast.h"
#include "aether/bytecode_emitter.h"

#include <iostream>
#include <cassert>

using namespace aether;

void TestTaggedValues() {
    std::cout << "[Test 1] Verifying TaggedValue NaN-Boxing Representation...\n";

    // 1. Smis
    TaggedValue smi1 = TaggedValue::FromSmi(42);
    TaggedValue smi2 = TaggedValue::FromSmi(-100);
    assert(smi1.IsSmi());
    assert(smi1.AsSmi() == 42);
    assert(smi2.IsSmi());
    assert(smi2.AsSmi() == -100);
    assert(smi1.ToBoolean() == true);
    assert(TaggedValue::FromSmi(0).ToBoolean() == false);

    // 2. Doubles
    TaggedValue d1 = TaggedValue::FromDouble(3.1415926535);
    TaggedValue d2 = TaggedValue::FromDouble(-0.0078125);
    TaggedValue d_nan = TaggedValue::FromDouble(std::nan(""));
    assert(d1.IsDouble());
    assert(!d1.IsSmi());
    assert(d1.AsDouble() == 3.1415926535);
    assert(d2.IsDouble());
    assert(d2.AsDouble() == -0.0078125);
    assert(d_nan.IsDouble());
    assert(std::isnan(d_nan.AsDouble()));

    // 3. Booleans
    TaggedValue b_true = TaggedValue::True();
    TaggedValue b_false = TaggedValue::False();
    assert(b_true.IsBoolean());
    assert(b_true.AsBoolean() == true);
    assert(b_false.IsBoolean());
    assert(b_false.AsBoolean() == false);

    // 4. Undefined & Null
    TaggedValue undef = TaggedValue::Undefined();
    TaggedValue null_val = TaggedValue::Null();
    assert(undef.IsUndefined());
    assert(undef.IsNullOrUndefined());
    assert(!undef.ToBoolean());
    assert(null_val.IsNull());
    assert(null_val.IsNullOrUndefined());
    assert(!null_val.ToBoolean());

    std::cout << "  -> TaggedValue layout and bitwise tagging validated successfully.\n";
}

void TestShapesAndObjects() {
    std::cout << "[Test 2] Verifying Shape Transitions and JSObject Layout...\n";

    uint32_t next_shape_id = 1;
    auto root_shape = std::make_unique<Shape>(0, nullptr, "", TaggedValue::Null());

    JSObject obj(root_shape.get());
    assert(obj.GetShape()->Id() == 0);
    assert(obj.GetShape()->PropertyCount() == 0);

    // Add property 'x' = 100
    obj.SetNamedProperty("x", TaggedValue::FromSmi(100), next_shape_id);
    assert(obj.GetShape()->Id() == 1);
    assert(obj.GetShape()->PropertyCount() == 1);
    assert(obj.GetNamedProperty("x").AsSmi() == 100);

    // Add property 'y' = 200
    obj.SetNamedProperty("y", TaggedValue::FromSmi(200), next_shape_id);
    assert(obj.GetShape()->Id() == 2);
    assert(obj.GetShape()->PropertyCount() == 2);
    assert(obj.GetNamedProperty("y").AsSmi() == 200);
    assert(obj.GetNamedProperty("x").AsSmi() == 100);

    // Update existing property 'x' = 999 (No shape transition should occur)
    Shape* current_shape = obj.GetShape();
    obj.SetNamedProperty("x", TaggedValue::FromSmi(999), next_shape_id);
    assert(obj.GetShape() == current_shape);
    (void)current_shape;
    assert(obj.GetNamedProperty("x").AsSmi() == 999);

    std::cout << "  -> Shapes and dynamic property transition tree validated successfully.\n";
}

void TestManualBytecodeInterpreter() {
    std::cout << "[Test 3] Verifying Bytecode ISA & Interpreter Execution...\n";

    // Manually construct bytecode for:
    // r0 = 15;
    // r1 = 30;
    // acc = r0 + r1; (15 + 30 = 45)
    // Return acc;
    BytecodeArray bc(2, 0);
    bc.EmitOpcode(Opcode::LdaSmi);
    bc.EmitI32(15);
    bc.EmitOpcode(Opcode::Star);
    bc.EmitU8(0); // r0 = 15

    bc.EmitOpcode(Opcode::LdaSmi);
    bc.EmitI32(30);
    bc.EmitOpcode(Opcode::Star);
    bc.EmitU8(1); // r1 = 30

    bc.EmitOpcode(Opcode::Ldar);
    bc.EmitU8(0); // acc = r0 (15)

    bc.EmitOpcode(Opcode::Add);
    bc.EmitU8(1); // acc = acc + r1 (45)
    bc.EmitU16(0); // feedback slot 0

    bc.EmitOpcode(Opcode::Return);

    std::cout << bc.Disassemble() << "\n";

    Interpreter vm;
    TaggedValue result = vm.Execute(bc);

    assert(result.IsSmi());
    assert(result.AsSmi() == 45);
    std::cout << "  -> Interpreter executed bytecode correctly. Result: " << result.ToString() << "\n";
}

void TestASTToBytecodeCompilation() {
    std::cout << "[Test 4] Verifying AST-to-Bytecode Emitter & Complex Loop Execution...\n";

    // Construct AST for:
    // var count = 10;
    // var total = 0;
    // while (count > 0) {
    //     total = total + count;
    //     count = count - 1;
    // }
    // return total;
    // Expected result: 10 + 9 + 8 + 7 + 6 + 5 + 4 + 3 + 2 + 1 = 55

    auto program = std::make_unique<BlockStmt>();

    // var count = 10;
    program->AddStatement(std::make_unique<VarDeclStmt>(
        "count", std::make_unique<LiteralExpr>(TaggedValue::FromSmi(10))
    ));

    // var total = 0;
    program->AddStatement(std::make_unique<VarDeclStmt>(
        "total", std::make_unique<LiteralExpr>(TaggedValue::FromSmi(0))
    ));

    // while (count > 0)
    auto loop_cond = std::make_unique<BinaryExpr>(
        BinaryOp::GreaterThan,
        std::make_unique<IdentifierExpr>("count"),
        std::make_unique<LiteralExpr>(TaggedValue::FromSmi(0))
    );

    auto loop_body = std::make_unique<BlockStmt>();
    // total = total + count;
    loop_body->AddStatement(std::make_unique<ExprStmt>(
        std::make_unique<AssignExpr>(
            "total",
            std::make_unique<BinaryExpr>(
                BinaryOp::Add,
                std::make_unique<IdentifierExpr>("total"),
                std::make_unique<IdentifierExpr>("count")
            )
        )
    ));

    // count = count - 1;
    loop_body->AddStatement(std::make_unique<ExprStmt>(
        std::make_unique<AssignExpr>(
            "count",
            std::make_unique<BinaryExpr>(
                BinaryOp::Sub,
                std::make_unique<IdentifierExpr>("count"),
                std::make_unique<LiteralExpr>(TaggedValue::FromSmi(1))
            )
        )
    ));

    program->AddStatement(std::make_unique<WhileStmt>(
        std::move(loop_cond), std::move(loop_body)
    ));

    // return total;
    program->AddStatement(std::make_unique<ReturnStmt>(
        std::make_unique<IdentifierExpr>("total")
    ));

    // Emit bytecode
    BytecodeEmitter emitter;
    std::unique_ptr<BytecodeArray> compiled_bc = emitter.Emit(program.get());

    std::cout << compiled_bc->Disassemble() << "\n";

    // Execute in VM
    Interpreter vm;
    TaggedValue final_val = vm.Execute(*compiled_bc);

    std::cout << "Execution result of sum(1..10): " << final_val.ToString() << "\n";
    assert(final_val.IsSmi());
    assert(final_val.AsSmi() == 55);

    std::cout << "  -> AST emission, backward loop branching, and execution verified perfectly.\n";
}

int main() {
    std::cout << "========================================================\n";
    std::cout << "   AetherVM Milestone 1 Verification Test Suite          \n";
    std::cout << "========================================================\n\n";

    TestTaggedValues();
    std::cout << "\n";
    TestShapesAndObjects();
    std::cout << "\n";
    TestManualBytecodeInterpreter();
    std::cout << "\n";
    TestASTToBytecodeCompilation();

    std::cout << "\n========================================================\n";
    std::cout << "   ALL MILESTONE 1 ARCHITECTURAL TESTS PASSED (4/4)     \n";
    std::cout << "========================================================\n";
    return 0;
}
