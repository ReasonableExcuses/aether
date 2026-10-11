#undef NDEBUG
#include <cassert>

#include "aether/runtime.h"
#include "aether/heap.h"

#include <iostream>
#include <string>

using namespace aether;

void TestProxyGetSetForwarding() {
    std::cout << "[Test 1] Verifying Proxy get/set traps and property forwarding...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let target = { a: 10, b: 20 };
        let handler = {
            get: function(t, prop, receiver) {
                if (prop === 'magic') return 999;
                return t[prop] * 2;
            },
            set: function(t, prop, val, receiver) {
                t[prop] = val + 5;
                return true;
            }
        };

        let p = new Proxy(target, handler);
        let valA = p.a;
        let valB = p.b;
        let valMagic = p.magic;

        p.c = 100;
        let valC = target.c;

        // Forwarding when traps are absent
        let passthrough = new Proxy(target, {});
        let ptA = passthrough.a;
        passthrough.d = 500;
        let ptD = target.d;

        valA === 20 && valB === 40 && valMagic === 999 && valC === 105 && ptA === 10 && ptD === 500;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());

    std::cout << "  -> Proxy get/set traps and forwarding verified.\n" << std::flush;
}

void TestProxyHasAndInOperator() {
    std::cout << "[Test 2] Verifying Proxy has trap and relational 'in' operator...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let obj = { x: 1, y: 2 };
        let hasX = 'x' in obj;
        let hasZ = 'z' in obj;

        let target = { secret: 123, publicProp: 456 };
        let handler = {
            has: function(t, prop) {
                if (prop === 'secret') return false;
                return prop in t;
            }
        };

        let p = new Proxy(target, handler);
        let proxyHasSecret = 'secret' in p;
        let proxyHasPublic = 'publicProp' in p;
        let proxyHasOther = 'other' in p;

        // Verify TypeError on non-object RHS
        let caughtTypeError = false;
        try {
            'foo' in 42;
        } catch (e) {
            if (e.name === 'TypeError') {
                caughtTypeError = true;
            }
        }

        hasX === true && hasZ === false &&
        proxyHasSecret === false && proxyHasPublic === true && proxyHasOther === false &&
        caughtTypeError === true;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());

    std::cout << "  -> Proxy has trap and 'in' operator verified.\n" << std::flush;
}

void TestProxyDeletePropertyAndDeleteOperator() {
    std::cout << "[Test 3] Verifying Proxy deleteProperty trap and unary 'delete' operator...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let obj = { foo: 1, bar: 2 };
        let delFoo = delete obj.foo;
        let fooAfter = 'foo' in obj;
        let delNonExistent = delete obj.baz;

        let arr = [10, 20, 30];
        let delArr = delete arr[1];

        let deletedKeys = [];
        let target = { alpha: 1, beta: 2 };
        let handler = {
            deleteProperty: function(t, prop) {
                deletedKeys.push(prop);
                delete t[prop];
                return true;
            }
        };

        let p = new Proxy(target, handler);
        let delAlpha = delete p.alpha;
        let delBeta = delete p['beta'];

        delFoo === true && fooAfter === false && delNonExistent === true && delArr === true &&
        delAlpha === true && delBeta === true &&
        deletedKeys.length === 2 && deletedKeys[0] === 'alpha' && deletedKeys[1] === 'beta' &&
        !('alpha' in target) && !('beta' in target);
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());

    std::cout << "  -> Proxy deleteProperty and 'delete' operator verified.\n" << std::flush;
}

void TestProxyApplyTrap() {
    std::cout << "[Test 4] Verifying Proxy apply trap on callable targets...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        function multiply(a, b) {
            return a * b;
        }

        let applyLog = [];
        let handler = {
            apply: function(targetFn, thisArg, argsList) {
                applyLog.push(argsList.length);
                return targetFn(argsList[0], argsList[1]) + 10;
            }
        };

        let proxyMul = new Proxy(multiply, handler);
        let resDirect = proxyMul(6, 7); // 6 * 7 + 10 = 52

        let container = {
            mul: proxyMul
        };
        let resMethod = container.mul(3, 4); // 3 * 4 + 10 = 22

        resDirect === 52 && resMethod === 22 && applyLog.length === 2 &&
        applyLog[0] === 2 && applyLog[1] === 2;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());

    std::cout << "  -> Proxy apply trap verified.\n" << std::flush;
}

void TestProxyConstructTrap() {
    std::cout << "[Test 5] Verifying Proxy construct trap with 'new'...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        function Vector(x, y) {
            this.x = x;
            this.y = y;
        }

        let constructLog = [];
        let handler = {
            construct: function(targetFn, argsList, newTarget) {
                constructLog.push(argsList.length);
                return {
                    x: argsList[0] * 10,
                    y: argsList[1] * 10,
                    isProxyConstructed: true
                };
            }
        };

        let ProxyVector = new Proxy(Vector, handler);
        let v = new ProxyVector(3, 5);

        v.x === 30 && v.y === 50 && v.isProxyConstructed === true &&
        constructLog.length === 1 && constructLog[0] === 2;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());

    std::cout << "  -> Proxy construct trap verified.\n" << std::flush;
}

void TestProxyRevocable() {
    std::cout << "[Test 6] Verifying Proxy.revocable and operation errors on revoked proxy...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let target = { key: 'original' };
        let rev = Proxy.revocable(target, {
            get: function(t, p) { return t[p]; }
        });

        let preRevoke = rev.proxy.key;
        rev.revoke();

        let caughtGet = false;
        try {
            let x = rev.proxy.key;
        } catch (e) {
            if (e.name === 'TypeError') caughtGet = true;
        }

        let caughtSet = false;
        try {
            rev.proxy.key = 'modified';
        } catch (e) {
            if (e.name === 'TypeError') caughtSet = true;
        }

        let caughtHas = false;
        try {
            let h = 'key' in rev.proxy;
        } catch (e) {
            if (e.name === 'TypeError') caughtHas = true;
        }

        let caughtDelete = false;
        try {
            delete rev.proxy.key;
        } catch (e) {
            if (e.name === 'TypeError') caughtDelete = true;
        }

        preRevoke === 'original' && caughtGet === true && caughtSet === true &&
        caughtHas === true && caughtDelete === true;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());

    std::cout << "  -> Proxy.revocable and revocation enforcement verified.\n" << std::flush;
}

void TestReflectMOPMethods() {
    std::cout << "[Test 7] Verifying Reflect standard reflection methods...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let obj = { a: 10, b: 20 };

        let rGet = Reflect.get(obj, 'a');
        let rSet = Reflect.set(obj, 'c', 30);
        let rHas = Reflect.has(obj, 'c');
        let rDel = Reflect.deleteProperty(obj, 'b');
        let rHasB = Reflect.has(obj, 'b');

        function add(x, y) { return x + y; }
        let rApply = Reflect.apply(add, undefined, [15, 25]);

        function Point(x, y) { this.x = x; this.y = y; }
        let rConstruct = Reflect.construct(Point, [7, 9]);

        let rOwnKeys = Reflect.ownKeys(obj);
        let rIsExt = Reflect.isExtensible(obj);
        let rPrevExt = Reflect.preventExtensions(obj);

        let defSuccess = Reflect.defineProperty(obj, 'd', { value: 42 });
        let desc = Reflect.getOwnPropertyDescriptor(obj, 'd');

        rGet === 10 && rSet === true && obj.c === 30 && rHas === true &&
        rDel === true && rHasB === false &&
        rApply === 40 && rConstruct.x === 7 && rConstruct.y === 9 &&
        rOwnKeys.length >= 2 && rIsExt === true && rPrevExt === true &&
        defSuccess === true && desc.value === 42;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());

    std::cout << "  -> Reflect reflection methods verified.\n" << std::flush;
}

void TestObjectReflectionBuiltins() {
    std::cout << "[Test 8] Verifying Object reflection builtins (keys, assign, defineProperty, etc.)...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let proto = { inherited: 99 };
        let child = Object.create ? Object.create(proto) : {};
        if (!Object.create) Object.setPrototypeOf(child, proto);
        child.own1 = 1;
        child.own2 = 2;

        let keys = Object.keys(child);
        let names = Object.getOwnPropertyNames(child);
        let getProto = Object.getPrototypeOf(child);

        let merged = Object.assign({ x: 1 }, { y: 2 }, { z: 3 });

        Object.defineProperty(merged, 'extra', { value: 777 });
        let desc = Object.getOwnPropertyDescriptor(merged, 'extra');

        keys.length === 2 && names.length === 2 &&
        getProto.inherited === 99 &&
        merged.x === 1 && merged.y === 2 && merged.z === 3 &&
        desc.value === 777;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());

    std::cout << "  -> Object reflection builtins verified.\n" << std::flush;
}

void TestProxyPrototypeTraps() {
    std::cout << "[Test 9] Verifying Proxy getPrototypeOf and setPrototypeOf traps...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let fakeProto = { marker: 'fake_proto' };
        let newProto = { marker: 'new_proto' };
        let protoSetLog = [];

        let target = {};
        let handler = {
            getPrototypeOf: function(t) {
                return fakeProto;
            },
            setPrototypeOf: function(t, p) {
                protoSetLog.push(p.marker);
                return true;
            }
        };

        let p = new Proxy(target, handler);

        let pProto = Object.getPrototypeOf(p);
        let refProto = Reflect.getPrototypeOf(p);

        let setResObj = Object.setPrototypeOf(p, newProto);
        let setResRef = Reflect.setPrototypeOf(p, newProto);

        pProto.marker === 'fake_proto' && refProto.marker === 'fake_proto' &&
        protoSetLog.length === 2 && protoSetLog[0] === 'new_proto' && protoSetLog[1] === 'new_proto' &&
        setResRef === true;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());

    std::cout << "  -> Proxy prototype traps verified.\n" << std::flush;
}

void TestForInLoopsAndObjectSpread() {
    std::cout << "[Test 10] Verifying for-in loop lowering and object spread with Proxy/Object keys...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let regularObj = { a: 1, b: 2, c: 3 };
        let keysFound = [];
        for (let k in regularObj) {
            keysFound.push(k);
        }

        let target = { x: 10, y: 20 };
        let handler = {
            ownKeys: function(t) {
                return ['custom1', 'custom2'];
            },
            get: function(t, prop) {
                if (prop === 'custom1') return 111;
                if (prop === 'custom2') return 222;
                return t[prop];
            }
        };

        let p = new Proxy(target, handler);
        let proxyKeysFound = [];
        for (let pk in p) {
            proxyKeysFound.push(pk);
        }

        let spreadObj = { ...p };

        keysFound.length === 3 &&
        keysFound[0] === 'a' && keysFound[1] === 'b' && keysFound[2] === 'c' &&
        proxyKeysFound.length === 2 &&
        proxyKeysFound[0] === 'custom1' && proxyKeysFound[1] === 'custom2' &&
        spreadObj.custom1 === 111 && spreadObj.custom2 === 222;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());

    std::cout << "  -> for-in loops and object spread verified.\n" << std::flush;
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << " Running AetherVM Milestone 15: Proxy & Reflect MOP Suite        \n";
    std::cout << "=================================================================\n" << std::flush;

    TestProxyGetSetForwarding();
    TestProxyHasAndInOperator();
    TestProxyDeletePropertyAndDeleteOperator();
    TestProxyApplyTrap();
    TestProxyConstructTrap();
    TestProxyRevocable();
    TestReflectMOPMethods();
    TestObjectReflectionBuiltins();
    TestProxyPrototypeTraps();
    TestForInLoopsAndObjectSpread();

    std::cout << "=================================================================\n";
    std::cout << " All 10 Milestone 15 Proxy & Reflect Tests Passed Successfully!  \n";
    std::cout << "=================================================================\n" << std::flush;

    return 0;
}
