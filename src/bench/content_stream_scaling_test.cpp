// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.

#include "../index/content_result_store.h"
#include "../index/content_scope.h"
#include "../index/content_sort_sql.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <future>
#include <string_view>

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
void ResultStoreChecks() {
    constexpr size_t count=1200;
    std::vector<ContentHit> hits;
    for (size_t i=0;i<count;++i) {
        auto hit=Hit(i);
        hit.name=L"name"+std::to_wstring(i%4);
        hit.path+=i%3 ? L".TXT" : L".log";
        hit.size=i%5; hit.modified=i%7;
        hits.push_back(std::move(hit));
    }
    ContentResultStore store(nullptr,0);
    Check(store.StreamUpsert(hits,ContentResultSort::Name,false),"seed result-store seek fixture");
    ContentResultStore::Row row;
    const auto locate=[&](const ContentHit& hit,int expected) {
        auto path=std::make_shared<std::promise<int>>(), identity=std::make_shared<std::promise<int>>();
        auto by_path=path->get_future(), by_identity=identity->get_future();
        store.FindPath(hit.path,[path](int index){path->set_value(index);});
        store.FindIdentity(hit.file_id,[identity](int index){identity->set_value(index);});
        return by_path.wait_for(std::chrono::seconds(5))==std::future_status::ready &&
            by_identity.wait_for(std::chrono::seconds(5))==std::future_status::ready &&
            by_path.get()==expected && by_identity.get()==expected;
    };
    for (auto sort:{ContentResultSort::Index,ContentResultSort::Name,ContentResultSort::Type,
                   ContentResultSort::Path,ContentResultSort::Size,ContentResultSort::Mtime}) {
        for (bool descending:{false,true}) {
            auto expected=hits;
            std::stable_sort(expected.begin(),expected.end(),[&](const auto& a,const auto& b) {
                int compare=0;
                if (sort==ContentResultSort::Index) compare=a.file_id<b.file_id ? -1 : a.file_id>b.file_id ? 1 : 0;
                else if (sort==ContentResultSort::Size || sort==ContentResultSort::Mtime) {
                    const auto left=sort==ContentResultSort::Size ? a.size : a.modified;
                    const auto right=sort==ContentResultSort::Size ? b.size : b.modified;
                    compare=left<right ? -1 : left>right ? 1 : 0;
                } else {
                    const auto key=[&](const auto& hit) {
                        return sort==ContentResultSort::Name ? hit.name : sort==ContentResultSort::Path ? hit.path : hit.path.substr(hit.path.find_last_of(L'.')+1);
                    };
                    const auto left=key(a),right=key(b);
                    compare=CompareStringOrdinal(left.c_str(),-1,right.c_str(),-1,TRUE)-CSTR_EQUAL;
                }
                return descending ? compare>0 : compare<0;
            });
            store.SetSort(sort,descending);
            bool exact=Wait([&]{return !store.Sorting();});
            for (size_t index : {768u,256u,0u,1024u,512u})
                exact=exact && WaitRow(store,index,row) && row.file_id==expected[index].file_id;
            for (size_t i=0;exact && i<count;++i)
                exact=WaitRow(store,i,row) && row.file_id==expected[i].file_id;
            Check(exact && !store.Error() && locate(expected[768],768) && locate(expected.back(),static_cast<int>(count)-1),
                "all sort keys and directions preserve stable ties across cold pages, seeks and rank lookups");
        }
    }
    store.SetSort(ContentResultSort::Index,false);
    Check(Wait([&]{return !store.Sorting();}),"restore insertion order for selection checks");
    const auto resolve=[&](std::vector<int> indices) {
        auto promise=std::make_shared<std::promise<ContentResultStore::Selection>>();
        auto future=promise->get_future();
        store.Resolve(std::move(indices),false,count,[promise](auto value){promise->set_value(std::move(value));});
        if (future.wait_for(std::chrono::seconds(5))==std::future_status::ready) return future.get();
        ContentResultStore::Selection failed; failed.error=ERROR_TIMEOUT; return failed;
    };
    const auto sum=[&](std::vector<int> indices) {
        auto promise=std::make_shared<std::promise<std::pair<uint64_t,DWORD>>>();
        auto future=promise->get_future();
        store.Sum(std::move(indices),false,count,[promise](uint64_t bytes,DWORD error){promise->set_value({bytes,error});});
        if (future.wait_for(std::chrono::seconds(5))==std::future_status::ready) return future.get();
        return std::pair<uint64_t,DWORD>{0,ERROR_TIMEOUT};
    };
    const std::vector<int> indices={1199,900,900,2,1190};
    const auto selection=resolve(indices);
    bool exact=!selection.error && selection.rows.size()==indices.size();
    uint64_t expected_bytes=0;
    for (size_t i=0;i<indices.size();++i) {
        expected_bytes+=hits[indices[i]].size;
        if (exact) exact=selection.rows[i].first==static_cast<size_t>(indices[i]) && selection.rows[i].second.file_id==hits[indices[i]].file_id;
    }
    Check(exact,"selection ranges preserve caller order and duplicate indices");
    Check(sum(indices)==std::pair<uint64_t,DWORD>{expected_bytes,0},"selection range size sum retains duplicate selections");
    const auto sparse=resolve({0,static_cast<int>(count)-1});
    Check(!sparse.error && sparse.rows.size()==2 && sparse.rows[0].second.file_id==1 &&
        sparse.rows[1].second.file_id==count && sum({0,static_cast<int>(count)-1})==std::pair<uint64_t,DWORD>{4,0},
        "sparse endpoint selections retain exact rows and sizes");
    const auto missing=resolve({5,static_cast<int>(count),0});
    const auto cancelled=resolve({5,-1,0});
    Check(missing.error==ERROR_DATABASE_FAILURE && missing.rows.size()==1 &&
        cancelled.error==ERROR_CANCELLED && cancelled.rows.size()==1,
        "selection preserves prefix results and errors for missing and negative indices");
    Check(sum({7,static_cast<int>(count),8})==std::pair<uint64_t,DWORD>{5,ERROR_DATABASE_FAILURE} &&
        sum({7,-1,8})==std::pair<uint64_t,DWORD>{2,ERROR_CANCELLED},
        "size sum retains missing-index and negative-index behavior");
    Check(resolve({}).rows.empty() && sum({})==std::pair<uint64_t,DWORD>{0,0},"empty selections complete without scanning");
    auto removed=hits[0]; removed.removed=true;
    exact=store.StreamUpsert({removed},ContentResultSort::Name,true) && store.Count()==count-1;
    for (size_t i=0;exact && i<count-1;++i) exact=WaitRow(store,i,row) && row.file_id==i+2;
    Check(exact,"live refresh ignores old cached position anchors and retains the requested order");
    store.SetFilter([](const pulse::fs::DirEntry& entry){return entry.size%2==0;});
    exact=Wait([&]{return !store.Filtering();});
    const auto last=std::find_if(hits.rbegin(),hits.rend(),[](const auto& hit){return hit.size%2==0;});
    const int last_visible=static_cast<int>(store.Count())-1;
    Check(exact && last_visible>=0 && WaitRow(store,last_visible,row) && row.file_id==last->file_id &&
        locate(*last,last_visible) && locate(hits[1],-1),"cold tail page and rank lookups preserve filtered membership");
    size_t visible=0;
    for (size_t i=1;exact && i<count;++i) if (hits[i].size%2==0)
        exact=WaitRow(store,visible++,row) && row.file_id==i+1;
    Check(exact && visible==store.Count(),"sequential seeks retain filtered membership across page boundaries");
    Check(store.CachedRows()<=ContentResultStore::kCachePages*ContentResultStore::kPageSize,"seek pages retain the bounded display cache");
}
void ResultStoreBenchmark(size_t count) {
    ContentResultStore store(nullptr,0);
    std::vector<ContentHit> hits; hits.reserve(count);
    for (size_t i=0;i<count;++i) hits.push_back(Hit(i));
    Check(store.StreamUpsert(hits,ContentResultSort::Name,false),"seed deep-page and multi-selection benchmark");
    ContentResultStore::Row row;
    const auto wait_row=[&](size_t index) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        store.Prefetch(index);
        while (!store.Ready(index)) {
            if (std::chrono::steady_clock::now()>=deadline) return false;
            SwitchToThread();
        }
        return store.Get(index,row);
    };
    const size_t last_page=(count-1)/ContentResultStore::kPageSize;
    auto begin=std::chrono::steady_clock::now();
    const auto random_index=(last_page-3)*ContentResultStore::kPageSize;
    bool ok=wait_row(random_index) && row.file_id==random_index+1;
    const auto random_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    begin=std::chrono::steady_clock::now();
    for (size_t page=last_page-2;ok && page<=last_page;++page)
        ok=wait_row(page*ContentResultStore::kPageSize) && row.file_id==page*ContentResultStore::kPageSize+1;
    const auto seek_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    std::vector<int> indices;
    for (size_t i=count-1;i>=count-100;--i) indices.push_back(static_cast<int>(i));
    auto selected=std::make_shared<std::promise<ContentResultStore::Selection>>(); auto selection=selected->get_future();
    begin=std::chrono::steady_clock::now();
    store.Resolve(indices,false,count,[selected](auto value){selected->set_value(std::move(value));});
    const bool resolved=selection.wait_for(std::chrono::seconds(30))==std::future_status::ready;
    auto rows=resolved ? selection.get() : ContentResultStore::Selection{};
    const auto resolve_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    auto summed=std::make_shared<std::promise<std::pair<uint64_t,DWORD>>>(); auto sum=summed->get_future();
    begin=std::chrono::steady_clock::now();
    store.Sum(indices,false,count,[summed](uint64_t bytes,DWORD error){summed->set_value({bytes,error});});
    const bool summed_ready=sum.wait_for(std::chrono::seconds(30))==std::future_status::ready;
    const auto total=summed_ready ? sum.get() : std::pair<uint64_t,DWORD>{0,ERROR_TIMEOUT};
    const auto sum_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    auto sparse_selected=std::make_shared<std::promise<ContentResultStore::Selection>>(); auto sparse_selection=sparse_selected->get_future();
    begin=std::chrono::steady_clock::now();
    store.Resolve({0,static_cast<int>(count)-1},false,count,[sparse_selected](auto value){sparse_selected->set_value(std::move(value));});
    const bool sparse_ready=sparse_selection.wait_for(std::chrono::seconds(30))==std::future_status::ready;
    auto sparse=sparse_ready ? sparse_selection.get() : ContentResultStore::Selection{};
    const auto sparse_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    printf("rows=%zu random_deep_page_ms=%.3f next_three_pages_ms=%.3f tail_100_resolve_ms=%.3f tail_100_sum_ms=%.3f sparse_endpoints_resolve_ms=%.3f\n",count,random_ms,seek_ms,resolve_ms,sum_ms,sparse_ms);
    Check(ok && resolved && !rows.error && rows.rows.size()==100 && rows.rows.front().second.file_id==count &&
        rows.rows.back().second.file_id==count-99 && !total.second && total.first==100*count-5050,
        "deep sequential pages and tail selections retain exact contents");
    Check(sparse_ready && !sparse.error && sparse.rows.size()==2 && sparse.rows.front().second.file_id==1 &&
        sparse.rows.back().second.file_id==count,"sparse endpoint benchmark retains both selected rows");
    for (int i=0;i<12;++i) {
        store.SetSort(ContentResultSort::Path,i%2==0); Sleep(1);
        store.SetSort(ContentResultSort::Size,i%2!=0); Sleep(1);
    }
    store.SetSort(ContentResultSort::Name,false);
    Check(Wait([&]{return !store.Sorting();}) && !store.Error() && wait_row(0) && row.file_id==1 &&
        store.Append({Hit(count)}) && wait_row(count) && row.file_id==count+1,
        "superseded index builds recover for the latest sort, page reads and later appends");
}
void ScopeChecks() {
    ContentSearchRequest scope; scope.root=L"C:\\A"; scope.roots={L"C:\\A",L"D:\\B"};
    Check(ContentRequestIncludesPath(scope,L"D:\\B\\new.txt") &&
        !ContentRequestIncludesPath(scope,L"D:\\BC\\outside.txt"),"multiple roots override the compatibility root with path boundaries");
    Check(ContentRequestIntersectsPath(scope,L"D:\\") && !ContentRequestIntersectsPath(scope,L"D:\\other"),
        "ancestor imports intersect every requested root");
    scope.recursive=false;
    Check(ContentRequestIncludesPath(scope,L"D:\\B") && ContentRequestIncludesPath(scope,L"D:\\B\\new.txt") &&
        !ContentRequestIncludesPath(scope,L"D:\\B\\child\\new.txt"),"nonrecursive initial and live searches share their scope");
    Check(ContentPathUnder(L"c:\\", L"c:\\") && ContentPathUnder(L"c:\\file.txt", L"c:\\") &&
        !ContentPathUnder(L"d:\\file.txt", L"c:\\"), "shared path containment handles drive roots");
    Check(ContentPathUnder(L"\\\\server\\share\\child", L"\\\\server\\share") &&
        !ContentPathUnder(L"\\\\server\\share-extra\\child", L"\\\\server\\share"),
        "shared path containment respects UNC share boundaries");
    ContentIndexConfig config;
    config.excluded_directories = {L".GIT", L"Node_Modules"};
    config.excluded_paths = {L"C:/Work/Private/"};
    Check(ContentPathExcluded(config, L"c:/work/.git/config") &&
        ContentPathExcluded(config, L"C:\\Work\\NODE_MODULES\\package.json") &&
        !ContentPathExcluded(config, L"C:\\Work\\node_modules_backup\\keep.txt"),
        "directory exclusions preserve case folding, separator normalization and component boundaries");
    Check(ContentPathExcluded(config, L"c:\\work\\private\\file.txt") &&
        !ContentPathExcluded(config, L"c:\\work\\private-copy\\file.txt"),
        "path exclusions match complete directory prefixes");
    config.excluded_directories.clear();
    config.excluded_paths.clear();
    Check(!ContentPathExcluded(config, L"C:\\Work\\keep.txt"), "empty exclusions retain every ordinary path");
}
}
int main(int argc, char** argv) {
    if (argc==2 && std::string_view(argv[1])=="--result-store-only") { ResultStoreChecks(); return failures ? 1 : 0; }
    if (argc==2 && std::string_view(argv[1])=="--result-store-bench") {
        for (size_t count:{10000u,100000u}) ResultStoreBenchmark(count);
        return failures ? 1 : 0;
    }
    ScopeChecks();
    if (argc == 2 && std::string_view(argv[1]) == "--scope-only") return failures ? 1 : 0;
    for (size_t count:{1000u,10000u,100000u}) Scaling(count);
    return failures ? 1 : 0;
}
