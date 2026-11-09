// SPDX-License-Identifier: GPL-2.0-or-later
#include "../ryty_gpu/ryty_alias_cache.h"
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <new>
#include <random>
#include <cstdio>

static long fail_after = -1;
void* operator new(std::size_t n) {
    if (fail_after == 0) throw std::bad_alloc();
    if (fail_after > 0) --fail_after;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
static void* ptr(std::uintptr_t n) { return reinterpret_cast<void*>(n); }
static void require(bool v) { if (!v) std::abort(); }

static void failures() {
    for (int fail = 0; fail < 8; ++fail) {
        Ryty::AliasEntries<2> c;
        c.Insert(1, ptr(1)); c.Insert(2, ptr(2));
        fail_after = fail;
        bool threw = false;
        try { c.Insert(3, ptr(3)); } catch (const std::bad_alloc&) { threw = true; }
        fail_after = -1;
        if (threw) {
            require(c.Size() == 2);
            require(c.Insert(4, ptr(4)) == ptr(1)); // original LRU survives rollback
            require(c.Find(2).value() == ptr(2) && !c.Find(3));
        } else require(c.Size() == 2 && !c.Find(1) && c.Find(3).value() == ptr(3));
    }
    Ryty::AliasEntries<4> c; c.Insert(1, ptr(1)); c.Insert(2, nullptr); c.Insert(3, ptr(3));
    std::vector<void*> retired{ptr(99)}; retired.shrink_to_fit();
    fail_after = 0; bool threw = false;
    try { c.RemoveRange(1, 4, retired); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1;
    require(threw && c.Size() == 3 && retired.size() == 1 && retired[0] == ptr(99));
    require(c.Find(1).value() == ptr(1) && c.Find(2).has_value() && c.Find(2).value() == nullptr);
    c.RemoveRange(1, 4, retired); require(c.Size() == 0 && retired.size() == 3);
}

static void randomized() {
    struct Entry { std::uint64_t key; void* value; };
    Ryty::AliasEntries<7> c; std::vector<Entry> model; std::mt19937_64 gen(7783);
    for (int step = 0; step < 40000; ++step) {
        auto key = gen() % 25; auto it = std::find_if(model.begin(), model.end(), [=](auto e){return e.key == key;});
        switch (gen() % 3) {
        case 0: {
            auto got = c.Find(key); require(got.has_value() == (it != model.end()));
            if (it != model.end()) { require(*got == it->value); auto e = *it; model.erase(it); model.insert(model.begin(), e); }
            break;
        }
        case 1: if (it == model.end()) {
            void* value = (gen() % 3 == 0) ? nullptr : ptr(key + 1); void* victim = nullptr;
            if (model.size() == 7) { victim = model.back().value; model.pop_back(); }
            require(c.Insert(key, value) == victim); model.insert(model.begin(), {key, value});
        } break;
        default: {
            auto end = gen() % 27; std::vector<void*> got{ptr(99)}, expected{ptr(99)};
            for (auto e : model) if (key <= e.key && e.key < end && e.value) expected.push_back(e.value);
            c.RemoveRange(key, end, got);
            std::erase_if(model, [=](auto e){return key <= e.key && e.key < end;});
            std::sort(got.begin() + 1, got.end()); std::sort(expected.begin() + 1, expected.end()); require(got == expected);
        }}
        require(c.Size() == model.size());
    }
    Ryty::AliasEntries<1> one; auto max = std::numeric_limits<std::uint64_t>::max();
    one.Insert(max, nullptr); std::vector<void*> out; one.RemoveRange(max, 0, out); require(one.Find(max).has_value());
    require(one.Insert(0, ptr(1)) == nullptr); one.RemoveRange(0, max, out); require(one.Size() == 0 && out.size() == 1);
}
int main() { failures(); randomized(); std::puts("alias cache tests passed"); }
