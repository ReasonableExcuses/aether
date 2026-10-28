#include "aether/heap_verifier.h"
#include "aether/heap.h"
#include "aether/options.h"
#include <iostream>
#include <stdexcept>
#include <unordered_set>
#include <cstdint>

namespace aether {

void HeapVerifier::VerifyBeforeGC(IsolateHeap& heap) {
    if (!Options::Get().gc_verify) return;
    VerifyHeap(heap, "BeforeGC");
}

void HeapVerifier::VerifyAfterGC(IsolateHeap& heap) {
    if (!Options::Get().gc_verify) return;
    VerifyHeap(heap, "AfterGC");
}

void HeapVerifier::VerifyHeap(IsolateHeap& heap, const std::string& phase) {
    std::unordered_set<HeapObject*> visited;
    std::vector<HeapObject*> worklist;

    auto check_ptr = [&](HeapObject* obj, const std::string& source_desc) {
        if (!obj) return;
        uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
        if (addr < 0x10000) {
            throw std::runtime_error("HeapVerifier [" + phase + "]: Invalid object pointer " + std::to_string(addr) + " from " + source_desc);
        }

        // Check for 0xDD / 0xCD poison
        uint64_t head = *reinterpret_cast<const uint64_t*>(obj);
        if (head == 0xDDDDDDDDDDDDDDDDULL || head == 0xCDCDCDCDCDCDCDCDULL) {
            throw std::runtime_error("HeapVerifier [" + phase + "]: Reachable poisoned object (" +
                                     (head == 0xDDDDDDDDDDDDDDDDULL ? "0xDD" : "0xCD") + ") at " +
                                     std::to_string(addr) + " from " + source_desc);
        }

        // Validate type tag
        uint16_t type_val = static_cast<uint16_t>(obj->GetType());
        if (type_val < 0x01 || type_val > 0x18) {
            throw std::runtime_error("HeapVerifier [" + phase + "]: Invalid object type tag 0x" +
                                     std::to_string(type_val) + " at " + std::to_string(addr) +
                                     " from " + source_desc);
        }

        if (obj->IsJSObject()) {
            Shape* shape = obj->GetShape();
            if (shape) {
                uintptr_t saddr = reinterpret_cast<uintptr_t>(shape);
                if (saddr < 0x10000) {
                    throw std::runtime_error("HeapVerifier [" + phase + "]: Invalid shape pointer on object at " +
                                             std::to_string(addr));
                }
            }
        }

        if (visited.insert(obj).second) {
            worklist.push_back(obj);
        }
    };

    // 1. Check all roots
    heap.VisitRoots([&](TaggedValue& slot) {
        if (slot.IsHeapObject()) {
            check_ptr(slot.AsHeapObject(), "Root Slot");
        }
    });

    // 2. Traverse reachable object graph
    while (!worklist.empty()) {
        HeapObject* curr = worklist.back();
        worklist.pop_back();

        curr->IteratePointers([&](TaggedValue& child) {
            if (child.IsHeapObject()) {
                HeapObject* child_obj = child.AsHeapObject();
                check_ptr(child_obj, "Object " + std::to_string(reinterpret_cast<uintptr_t>(curr)));

                // Generational invariant check: if parent is old and child is young,
                // the card table or remembered set must cover the parent.
                if (curr->IsOld() && child_obj->IsYoung()) {
                    // Check if card table is marked
                    if (!heap.card_table_.IsCardMarked(curr)) {
                        // If card is not marked, report missing write barrier
                        throw std::runtime_error("HeapVerifier [" + phase + "]: Unremembered cross-generational pointer from old " +
                                                 std::to_string(reinterpret_cast<uintptr_t>(curr)) + " to young " +
                                                 std::to_string(reinterpret_cast<uintptr_t>(child_obj)));
                    }
                }
            }
        });
    }

    if (Options::Get().trace_gc) {
        std::cout << "[HeapVerifier] " << phase << " verified " << visited.size() << " reachable objects." << std::endl;
    }
}

} // namespace aether
