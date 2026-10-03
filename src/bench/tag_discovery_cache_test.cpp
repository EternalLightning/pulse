#include "../app/tag_discovery_cache.h"
#include <cstdio>

int main() {
    pulse::app::TagDiscoveryCache cache(3, 100);
    int failed = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failed;
    };
    cache.Record(L"a", 1, 10); cache.Record(L"b", 1, 10); cache.Record(L"c", 1, 10);
    check(cache.Contains(L"a", 1, 20), "cache hit touches entry");
    cache.Record(L"d", 1, 20);
    check(cache.Size() == 3 && !cache.Contains(L"b", 1, 20) && cache.Contains(L"a", 1, 20),
          "evict least recently used path at capacity");
    check(!cache.Contains(L"a", 2, 21), "modified version invalidates old lookup");
    cache.Record(L"a", 2, 21);
    check(!cache.Contains(L"a", 2, 121), "expire unchanged negative lookups");
    cache.Clear(); check(cache.Size() == 0, "directory notifications invalidate cache");
    for (uint64_t i = 0; i < 10000; ++i) cache.Record(std::to_wstring(i), i, i);
    check(cache.Size() == 3, "long browsing stays within entry budget");
    cache.Erase(L"9999"); check(cache.Size() == 2, "failed reads can be retried");
    return failed ? 1 : 0;
}
