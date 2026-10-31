// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.

#include "../index/content_result_store.h"
#include "../index/content_scope.h"
#include "../index/content_sort_sql.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <future>

using namespace pulse::index;
namespace {
int failures=0;
void Check(bool ok,const char* message) {
    printf("[%s] %s\n",ok ? "PASS" : "FAIL",message); fflush(stdout); if (!ok) ++failures;
}
ContentHit Hit(size_t i) {
    wchar_t name[48]{}; swprintf_s(name,L"f%06zu.txt",i);
    ContentHit hit{std::wstring(L"C:\\stream-fixture\\")+name,name,L"body",i,0,1};
    hit.file_id=i+1; return hit;
}
bool WaitRow(ContentResultStore& store,size_t index,ContentResultStore::Row& row) {
    const auto end=GetTickCount64()+5000;
    do { if (store.Get(index,row)) return true; Sleep(1); } while (GetTickCount64()<end);
    return false;
}
bool Wait(const std::function<bool()>& ready) {
    const auto end=GetTickCount64()+5000;
    do { if (ready()) return true; Sleep(1); } while (GetTickCount64()<end);
    return ready();
}
void Scaling(size_t count) {
    ContentResultStore store(nullptr,0);
    bool ok=store.StreamUpsert({Hit(0)},ContentResultSort::Name,true);
    sqlite3* probe=nullptr;
    ok=ok && sqlite3_open16(store.CachePath().c_str(),&probe)==SQLITE_OK;
    if (probe) sqlite3_create_collation(probe,"PULSE_ORDINAL",SQLITE_UTF16,nullptr,OrdinalCollation);
    ok=ok && sqlite3_exec(probe,
        "CREATE TABLE audit_writes(n INTEGER); INSERT INTO audit_writes VALUES(0);"
        "CREATE TRIGGER audit_visible_insert AFTER INSERT ON visible BEGIN UPDATE audit_writes SET n=n+1; END;",
        nullptr,nullptr,nullptr)==SQLITE_OK;
    ContentResultStore::Row row;
    Check(ok && WaitRow(store,0,row) && row.file_id==1,"first result is visible before remaining results");
    const auto begin=std::chrono::steady_clock::now();
    double slowest=0;
    bool middle_page=true;
    for (size_t offset=1;ok && offset<count;offset+=64) {
        if (offset>=count/2 && offset<count/2+64)
            middle_page=WaitRow(store,offset-1,row);
        std::vector<ContentHit> batch;
        for (size_t i=offset;i<(std::min)(offset+64,count);++i) batch.push_back(Hit(i));
        const auto start=std::chrono::steady_clock::now();
        ok=store.StreamUpsert(batch,ContentResultSort::Name,true);
        slowest=(std::max)(slowest,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
    }
    const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    sqlite3_stmt* statement=nullptr; sqlite3_int64 writes=-1;
    if (probe && sqlite3_prepare_v2(probe,"SELECT n FROM audit_writes",-1,&statement,nullptr)==SQLITE_OK &&
        sqlite3_step(statement)==SQLITE_ROW) writes=sqlite3_column_int64(statement,0);
    sqlite3_finalize(statement); sqlite3_close(probe);
    printf("rows=%zu elapsed_ms=%.3f max_batch_ms=%.3f visible_inserts=%lld\n",count,elapsed,slowest,writes);
    Check(ok && !store.Error() && store.Count()==count && store.RawCount()==count,"all streamed rows and counts are retained");
    Check(middle_page,"a distant cached page remains usable while later results arrive");
    Check(writes==static_cast<sqlite3_int64>(count-1),"display membership writes grow linearly with new hits");
    Check(WaitRow(store,0,row) && row.file_id==count && WaitRow(store,count-1,row) && row.file_id==1,
        "first and last pages follow exact descending global order");
    store.SetFilter([](const pulse::fs::DirEntry& entry){ return entry.size%2==0; });
    Check(Wait([&]{return !store.Filtering();}) && store.Count()==(count+1)/2,"filtering preserves exact streaming membership");
    store.SetSort(ContentResultSort::Size,false);
    Check(Wait([&]{return !store.Sorting();}) && WaitRow(store,0,row) && row.file_id==1,
        "sorting during streaming changes page order without rebuilding membership");
    auto update=Hit(0); update.size=3;
    auto removed=Hit(2); removed.removed=true;
    Check(store.StreamUpsert({update,removed},ContentResultSort::Name,true) && store.RawCount()==count-1 &&
        store.Count()==(count+1)/2-2 && WaitRow(store,0,row) && row.file_id==5,
        "updates and deletions refresh only affected filter membership");
    std::promise<int> located; auto result=located.get_future();
    store.FindIdentity(5,[&](int position){located.set_value(position);});
    Check(result.wait_for(std::chrono::seconds(5))==std::future_status::ready && result.get()==0,
        "identity lookup resolves the current dynamic display position");
    std::promise<ContentResultStore::Selection> selected; auto selection=selected.get_future();
    store.Resolve({0,1},false,2,[&](auto value){selected.set_value(std::move(value));});
    const bool resolved=selection.wait_for(std::chrono::seconds(5))==std::future_status::ready;
    auto rows=resolved ? selection.get() : ContentResultStore::Selection{};
    Check(resolved && !rows.error && rows.rows.size()==2 && rows.rows[0].second.file_id==5 && rows.rows[1].second.file_id==7,
        "selection uses the same order and filter as displayed pages");
    std::promise<uint64_t> summed; auto sum=summed.get_future();
    store.Sum({0,1},false,2,[&](uint64_t bytes,DWORD error){summed.set_value(error ? UINT64_MAX : bytes);});
    Check(sum.wait_for(std::chrono::seconds(5))==std::future_status::ready && sum.get()==10,
        "selection size uses dynamic display positions");
    Check(store.Append({Hit(count)}) && store.RawCount()==count && store.Count()==(count+1)/2-1 &&
        WaitRow(store,0,row) && row.file_id==5 && WaitRow(store,store.Count()-1,row) && row.file_id==count+1,
        "append after live deletions shares streaming identity and order semantics");
    Check(store.CachedRows()<=ContentResultStore::kCachePages*ContentResultStore::kPageSize,"display cache stays bounded");
}
}
int main() {
    ContentSearchRequest scope; scope.root=L"C:\\A"; scope.roots={L"C:\\A",L"D:\\B"};
    Check(ContentRequestIncludesPath(scope,L"D:\\B\\new.txt") &&
        !ContentRequestIncludesPath(scope,L"D:\\BC\\outside.txt"),"multiple roots override the compatibility root with path boundaries");
    Check(ContentRequestIntersectsPath(scope,L"D:\\") && !ContentRequestIntersectsPath(scope,L"D:\\other"),
        "ancestor imports intersect every requested root");
    scope.recursive=false;
    Check(ContentRequestIncludesPath(scope,L"D:\\B") && ContentRequestIncludesPath(scope,L"D:\\B\\new.txt") &&
        !ContentRequestIncludesPath(scope,L"D:\\B\\child\\new.txt"),"nonrecursive initial and live searches share their scope");
    for (size_t count:{1000u,10000u,100000u}) Scaling(count);
    return failures ? 1 : 0;
}
