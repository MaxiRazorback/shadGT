// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic comparison of sparse range misses and hash point lookups.
#include "../ryty_gpu/ryty_alias_cache.h"
#include <chrono>
#include <cstdio>
#include <unordered_map>
int main() {
    using clock = std::chrono::steady_clock;
    Ryty::AliasEntries<> cache; std::unordered_map<std::uint64_t, void*> old;
    for (std::uint64_t i=0;i<4096;++i) { cache.Insert(i*4, reinterpret_cast<void*>(1)); old.emplace(i*4,reinterpret_cast<void*>(1)); }
    std::vector<void*> retired; std::size_t hits=0; constexpr int iterations=200000;
    auto a=clock::now(); for(int i=0;i<iterations;++i) { auto first=(i%4096)*4+1; for(auto& e:old) if(e.first>=first && e.first<first+1) ++hits; } auto b=clock::now();
    for(int i=0;i<iterations;++i) { auto first=(i%4096)*4+1; cache.RemoveRange(first,first+1,retired); } auto c=clock::now();
    for(int i=0;i<iterations;++i) hits+=old.find((i%4096)*4)!=old.end(); auto d=clock::now();
    for(int i=0;i<iterations;++i) hits+=cache.Find((i%4096)*4).has_value(); auto e=clock::now();
    auto ns=[](auto x,auto y){return std::chrono::duration_cast<std::chrono::nanoseconds>(y-x).count();};
    std::printf("sparse old=%lldns indexed=%lldns; point old=%lldns indexed=%lldns; hits=%zu\n",(long long)ns(a,b),(long long)ns(b,c),(long long)ns(c,d),(long long)ns(d,e),hits);
}
