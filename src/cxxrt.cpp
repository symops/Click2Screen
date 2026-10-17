// The little of the C++ runtime the programs use (new / delete), so they link without libstdc++
// (built with -fno-exceptions -fno-rtti, linked by gcc).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>

void* operator new(size_t n)
{
    void* p = HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
    if (!p) abort();
    return p;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { if (p) HeapFree(GetProcessHeap(), 0, p); }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, size_t) noexcept { operator delete(p); }
void operator delete[](void* p, size_t) noexcept { operator delete(p); }
extern "C" void __cxa_pure_virtual() { abort(); }
