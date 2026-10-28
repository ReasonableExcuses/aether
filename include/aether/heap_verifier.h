#pragma once

#include <string>

namespace aether {

class IsolateHeap;

class HeapVerifier {
public:
    // Full heap verification before garbage collection
    static void VerifyBeforeGC(IsolateHeap& heap);

    // Full heap verification after garbage collection
    static void VerifyAfterGC(IsolateHeap& heap);

    // General heap graph validity check
    static void VerifyHeap(IsolateHeap& heap, const std::string& phase = "General");
};

} // namespace aether
