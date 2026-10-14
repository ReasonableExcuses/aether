/*---
description: WebAssembly JavaScript Host API (validate, Module, Instance, Memory, Table, Global)
features: [WebAssembly]
includes: [assert.js]
---*/

// Minimal valid WebAssembly binary module
var bytes = new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00]);

// 1. WebAssembly.validate
assert.sameValue(WebAssembly.validate(bytes), true, "validate valid module");
assert.sameValue(WebAssembly.validate(new Uint8Array([0, 0, 0, 0])), false, "validate invalid module");

// 2. WebAssembly.Module
var mod = new WebAssembly.Module(bytes);
assert.sameValue(mod instanceof WebAssembly.Module, true, "mod instanceof WebAssembly.Module");

var exportsList = WebAssembly.Module.exports(mod);
assert.sameValue(Array.isArray(exportsList), true, "Module.exports returns array");

var importsList = WebAssembly.Module.imports(mod);
assert.sameValue(Array.isArray(importsList), true, "Module.imports returns array");

// 3. WebAssembly.Instance
var inst = new WebAssembly.Instance(mod);
assert.sameValue(inst instanceof WebAssembly.Instance, true, "inst instanceof WebAssembly.Instance");

// 4. WebAssembly.Memory
var mem = new WebAssembly.Memory({ initial: 1, maximum: 5 });
assert.sameValue(mem instanceof WebAssembly.Memory, true, "mem instanceof WebAssembly.Memory");
assert.sameValue(mem.buffer instanceof ArrayBuffer, true, "mem.buffer instanceof ArrayBuffer");
assert.sameValue(mem.buffer.byteLength, 65536, "1 page = 64KB");

var oldBuf = mem.buffer;
var prevPages = mem.grow(1);
assert.sameValue(prevPages, 1, "mem.grow returned previous size");
assert.sameValue(mem.buffer.byteLength, 131072, "grown buffer is 128KB");
assert.sameValue(oldBuf.byteLength, 0, "previous buffer detached after grow");

// 5. WebAssembly.Global
var glob = new WebAssembly.Global({ value: "i32", mutable: true }, 42);
assert.sameValue(glob.value, 42, "glob.value initial");
glob.value = 100;
assert.sameValue(glob.value, 100, "glob.value mutated");
