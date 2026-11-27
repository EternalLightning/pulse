// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "content_sort_sql.h"
#include "content_result_store.h"
#include "../common/current_user_security.h"
#include "../../third_party/sqlite/sqlite3.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <thread>
#include <objbase.h>

namespace pulse::index {
namespace {
struct Statement {
    sqlite3_stmt* p = nullptr;
    Statement(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &p, nullptr); }
    ~Statement() { sqlite3_finalize(p); }
    bool Step() { return p && sqlite3_step(p) == SQLITE_DONE; }
    void Reset() { sqlite3_reset(p); sqlite3_clear_bindings(p); }
    void Int(int i, uint64_t n) { sqlite3_bind_int64(p, i, static_cast<sqlite3_int64>(n)); }
    void Text(int i, const std::wstring& s) { sqlite3_bind_text16(p, i, s.data(), static_cast<int>(s.size()*2), SQLITE_TRANSIENT); }
};
std::wstring Column(sqlite3_stmt* p, int i) {
    const auto* text = static_cast<const wchar_t*>(sqlite3_column_text16(p,i));
    return text ? std::wstring(text, sqlite3_column_bytes16(p,i)/2) : std::wstring{};
}
ContentResultStore::Row ReadRow(sqlite3_stmt* p) {
    ContentResultStore::Row row;
    row.entry.name = Column(p,0); row.entry.full_path = Column(p,1);
    row.entry.size = static_cast<uint64_t>(sqlite3_column_int64(p,2));
    const auto stamp = static_cast<uint64_t>(sqlite3_column_int64(p,3));
    row.entry.mtime = {static_cast<DWORD>(stamp), static_cast<DWORD>(stamp >> 32)};
    row.entry.attrs = FILE_ATTRIBUTE_NORMAL;
    row.snippet = Column(p,5);
    row.file_id = static_cast<uint64_t>(sqlite3_column_int64(p,6));
    row.spool_sequence = static_cast<uint64_t>(sqlite3_column_int64(p,7));
    const auto line = sqlite3_column_int(p,4);
    if (line) row.snippet = L"L" + std::to_wstring(line) + L"  " + row.snippet;
    return row;
}
constexpr const char* kRows = "SELECT h.name,h.path,h.size,h.mtime,h.line,h.snippet,h.file_id,h.seq FROM visible v JOIN hits h ON h.seq=v.seq WHERE v.pos>=?1 AND v.pos<?2 ORDER BY v.pos";
constexpr const char* kHitColumns = "name,path,size,mtime,line,snippet,file_id,seq";
constexpr const char* kMembership = "EXISTS(SELECT 1 FROM visible WHERE visible.seq=hits.seq)";
std::string SortOrder(ContentResultSort sort, bool descending, bool stable_ties = false) {
    std::string order = stable_ties ? "seq" : "file_id";
    if (sort == ContentResultSort::Name) order = "name COLLATE PULSE_ORDINAL";
    else if (sort == ContentResultSort::Type) order = "pulse_extension(path) COLLATE PULSE_ORDINAL";
    else if (sort == ContentResultSort::Path) order = "path COLLATE PULSE_ORDINAL";
    else if (sort == ContentResultSort::Size) order = "size";
    else if (sort == ContentResultSort::Mtime) order = "mtime";
    order += descending ? " DESC" : " ASC";
    return order + (stable_ties ? ",seq ASC" : ",file_id ASC,seq ASC");
}
}
struct ContentResultStore::Impl {
    HWND notify; UINT message;
    sqlite3* db = nullptr;
    std::wstring path;
    std::mutex io, mu;
    std::condition_variable wake;
    std::deque<std::function<void()>> tasks;
    std::map<size_t,std::vector<Row>> pages;
    std::deque<size_t> lru;
    std::set<size_t> pending;
    std::atomic<size_t> count{0}, raw{0};
    std::atomic<uint64_t> revision{0}, filter_epoch{0}, page_epoch{0};
    std::atomic<DWORD> error{0};
    std::atomic<bool> stopping{false}, filtering{false};
    Filter filter;
    std::string order = "seq";
    std::string requested_order;
    std::string indexed_order;
    std::atomic<uint64_t> sort_epoch{0};
    std::atomic<bool> sorting{false};
    std::atomic<bool> stream_order{false};
    Impl(HWND n, UINT m):notify(n),message(m) {}
    ~Impl() {
        if (db) sqlite3_close(db);
        if (!path.empty()) DeleteFileW(path.c_str());
    }
    void Notify() { ++revision; if(notify && message) PostMessageW(notify,message,0,0); }
    void FinishSort(uint64_t epoch) {
        std::lock_guard lock(mu);
        if (epoch==sort_epoch) sorting=false;
    }
    bool Exec(const char* sql) { return sqlite3_exec(db,sql,nullptr,nullptr,nullptr)==SQLITE_OK; }
    struct SortProgress { Impl* state; uint64_t epoch; };
    static int CancelSort(void* context) {
        const auto& current=*static_cast<SortProgress*>(context);
        return current.state->stopping || current.epoch!=current.state->sort_epoch ? 1 : 0;
    }
    bool EnsureOrderIndex(const std::string& next_order, uint64_t epoch=0) {
        if (next_order.empty() || indexed_order==next_order) return true;
        if (!Exec("SAVEPOINT display_order")) return false;
        SortProgress progress{this,epoch};
        if (epoch) sqlite3_progress_handler(db,4096,CancelSort,&progress);
        bool ok=Exec("DROP INDEX IF EXISTS hits_display_order") &&
            Exec(("CREATE INDEX hits_display_order ON hits("+next_order+")").c_str());
        if (epoch) sqlite3_progress_handler(db,0,nullptr,nullptr);
        ok=ok && !stopping && (!epoch || epoch==sort_epoch);
        if (!ok || !Exec("RELEASE display_order")) {
            Exec("ROLLBACK TO display_order; RELEASE display_order");
            return false;
        }
        indexed_order=next_order;
        return true;
    }
    auto PrimaryOrder() const {
        const auto primary=std::string_view(order).substr(0,order.find(','));
        return std::pair{std::string(primary.substr(0,primary.find_last_of(' '))),primary.ends_with(" DESC")};
    }
    bool HasFileIdTie() const { return order.ends_with(",file_id ASC,seq ASC"); }
    std::string StreamRangeSql(const char* columns, bool reverse) const {
        std::string scan_order=order;
        if (reverse) {
            const auto [key,descending]=PrimaryOrder();
            scan_order=key+(descending ? " ASC" : " DESC")+(HasFileIdTie() ? ",file_id DESC,seq DESC" : ",seq DESC");
        }
        return std::string("SELECT ")+columns+" FROM hits INDEXED BY hits_display_order"+
            (filter ? std::string(" WHERE ")+kMembership : "")+" ORDER BY "+scan_order+" LIMIT ?2-?1 OFFSET ?1";
    }
    std::string RowsSql() const { return stream_order ? StreamRangeSql(kHitColumns,false) : kRows; }
    std::string SeekPageSql() const {
        const std::string rows=std::string("SELECT ")+kHitColumns+" FROM hits INDEXED BY hits_display_order WHERE ";
        const std::string membership=filter ? std::string(" AND ")+kMembership : "";
        const auto [key,descending]=PrimaryOrder();
        const auto anchor="(SELECT "+key+" FROM hits WHERE seq=?3)";
        // Keep equal keys in seq ASC order even when the primary key descends.
        // Each branch seeks the index and reads only the page and bounded gap.
        return "SELECT * FROM (SELECT * FROM ("+rows+key+"="+anchor+" AND seq>?3"+membership+
            " ORDER BY "+order+" LIMIT ?2-?1+?4) UNION ALL SELECT * FROM ("+rows+key+
            (descending ? "<" : ">")+anchor+membership+" ORDER BY "+order+
            " LIMIT ?2-?1+?4)) ORDER BY "+order+" LIMIT ?2-?1 OFFSET ?4";
    }
    template<class Read>
    DWORD ScanSelection(const std::vector<int>& indices, Read selected) {
        const size_t total=count;
        std::vector<int> ordered;
        for (const int index : indices) {
            if (index<0) break;
            if (static_cast<size_t>(index)<total) ordered.push_back(index);
        }
        std::sort(ordered.begin(),ordered.end());
        ordered.erase(std::unique(ordered.begin(),ordered.end()),ordered.end());

        // Traverse each side once. Read only sequence numbers through sparse gaps.
        const auto scan=[&](std::span<const int> positions, bool reverse) -> DWORD {
            if (positions.empty()) return 0;
            const bool dense=positions.size()==static_cast<size_t>(positions.back()-positions.front())+1;
            const auto offset=[&](int index) { return reverse ? total-1-index : static_cast<size_t>(index); };
            const size_t first=offset(reverse ? positions.back() : positions.front());
            const size_t end=offset(reverse ? positions.front() : positions.back())+1;
            Statement read(db,StreamRangeSql(dense ? kHitColumns : "seq",reverse).c_str());
            std::optional<Statement> body;
            if (!dense) body.emplace(db,(std::string("SELECT ")+kHitColumns+" FROM hits WHERE seq=?1").c_str());
            if (!read.p || (body && !body->p)) return ERROR_DATABASE_FAILURE;
            read.Int(1,first); read.Int(2,end);
            size_t current=0;
            for (size_t pos=first;pos<end && !stopping;++pos) {
                if (sqlite3_step(read.p)!=SQLITE_ROW) return ERROR_DATABASE_FAILURE;
                const int index=positions[reverse ? positions.size()-1-current : current];
                if (pos!=offset(index)) continue;
                if (body) {
                    body->Int(1,static_cast<uint64_t>(sqlite3_column_int64(read.p,0)));
                    if (sqlite3_step(body->p)!=SQLITE_ROW) return ERROR_DATABASE_FAILURE;
                    selected(index,body->p); body->Reset();
                } else selected(index,read.p);
                ++current;
            }
            return stopping ? ERROR_CANCELLED : 0;
        };
        const std::span<const int> positions=ordered;
        const auto midpoint=std::lower_bound(ordered.begin(),ordered.end(),total/2);
        const auto split=static_cast<size_t>(midpoint-ordered.begin());
        const auto result=scan(positions.first(split),false);
        return result ? result : scan(positions.subspan(split),true);
    }
    std::string PositionSql(const char* predicate) const {
        if (!stream_order) return std::string("SELECT v.pos FROM visible v JOIN hits ON hits.seq=v.seq WHERE ")+predicate+" LIMIT 1";
        const auto [key,descending]=PrimaryOrder();
        const std::string membership=filter ? std::string(" AND ")+kMembership : "";
        const std::string count_before="(SELECT count(*) FROM hits INDEXED BY hits_display_order WHERE "+key;
        const auto ties=HasFileIdTie() ? " AND (file_id,seq)<(target.file_id,target.seq)" : " AND seq<target.seq";
        return "WITH target AS MATERIALIZED (SELECT "+key+" AS sort_key,file_id,seq FROM hits WHERE "+predicate+membership+
            " LIMIT 1) SELECT "+count_before+(descending ? ">" : "<")+"target.sort_key"+membership+
            ")+"+count_before+"=target.sort_key"+ties+membership+") FROM target";
    }
    bool Open() {
        if(db) return true;
        wchar_t directory[32768]{};
        if(!GetTempPathW(ARRAYSIZE(directory),directory)) return false;
        GUID id{}; if(FAILED(CoCreateGuid(&id))) return false;
        wchar_t token[40]{}; StringFromGUID2(id,token,40);
        path=std::wstring(directory)+L"Pulse-results-"+token+L".sqlite";
        CurrentUserSecurityAttributes security;
        HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,security ? security.get() : nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,nullptr);
        if(file==INVALID_HANDLE_VALUE) return false;
        CloseHandle(file);
        if(sqlite3_open16(path.c_str(),&db)!=SQLITE_OK) return false;
        sqlite3_create_collation(db,"PULSE_ORDINAL",SQLITE_UTF16,nullptr,OrdinalCollation);
        sqlite3_create_function_v2(db,"pulse_extension",1,SQLITE_UTF16|SQLITE_DETERMINISTIC,nullptr,SqlExtension,nullptr,nullptr,nullptr);
        return Exec("PRAGMA journal_mode=MEMORY; PRAGMA synchronous=OFF; PRAGMA cache_size=-2048; PRAGMA temp_store=FILE; CREATE TABLE hits(seq INTEGER PRIMARY KEY,name TEXT,path TEXT,size INTEGER,mtime INTEGER,line INTEGER,snippet TEXT,file_id INTEGER NOT NULL DEFAULT 0); CREATE UNIQUE INDEX hits_identity ON hits(file_id) WHERE file_id<>0; CREATE TABLE visible(pos INTEGER PRIMARY KEY,seq INTEGER);");
    }
    void Enqueue(std::function<void()> task) {
        { std::lock_guard lock(mu); if(stopping) return; tasks.push_back(std::move(task)); }
        wake.notify_one();
    }
    void Run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mu); wake.wait(lock,[&]{return stopping || !tasks.empty();});
                if(stopping) { tasks.clear(); return; }
                task=std::move(tasks.front()); tasks.pop_front();
            }
            task();
        }
    }
    struct PageAnchor { size_t next_page; uint64_t sequence; };
    static std::optional<PageAnchor> CachedAnchor(const std::map<size_t,std::vector<Row>>& cached, size_t page) {
        const auto at=cached.lower_bound(page);
        if (at==cached.begin()) return std::nullopt;
        const auto previous=std::prev(at);
        if (previous->second.size()!=kPageSize) return std::nullopt;
        return PageAnchor{previous->first+1,previous->second.back().spool_sequence};
    }
    std::vector<Row> ReadPage(size_t page, size_t total, std::optional<PageAnchor> anchor) {
        std::vector<Row> rows;
        const size_t first=page*kPageSize;
        const size_t end=(std::min)(total,first+kPageSize);
        if (first>=end) return rows;
        const size_t gap=anchor ? (page-anchor->next_page)*kPageSize : 0;
        const size_t edge_distance=(std::min)(first,total-end);
        const bool seek=stream_order && anchor && !HasFileIdTie() && gap<=2*kPageSize && edge_distance>2*(gap+kPageSize);
        // An index scan backwards reverses the tie order too. Restore it only for the returned page.
        const bool reverse=stream_order && !seek && total-end<first;
        Statement read(db,(seek ? SeekPageSql() : (stream_order ? StreamRangeSql(kHitColumns,reverse) : kRows)).c_str());
        if (read.p) {
            read.Int(1,reverse ? total-end : first); read.Int(2,reverse ? total-first : end);
            if (seek) { read.Int(3,anchor->sequence); read.Int(4,gap); }
            int rc=SQLITE_DONE;
            while (!stopping && (rc=sqlite3_step(read.p))==SQLITE_ROW) rows.push_back(ReadRow(read.p));
            if (!stopping && rc!=SQLITE_DONE) error=ERROR_DATABASE_FAILURE;
        } else error=ERROR_DATABASE_FAILURE;
        if (reverse) std::reverse(rows.begin(),rows.end());
        return rows;
    }
    void Load(size_t page, uint64_t epoch) {
        std::vector<Row> rows;
        {
            std::lock_guard lock(io);
            if(!stopping && epoch==page_epoch && Open()) {
                std::optional<PageAnchor> anchor;
                if (page && stream_order) {
                    std::lock_guard cache(mu);
                    anchor=CachedAnchor(pages,page);
                }
                rows=ReadPage(page,count,anchor);
            }
        }
        {
            std::lock_guard lock(mu);
            if(epoch!=page_epoch || stopping) return;
            pending.erase(page);
            std::erase(lru,page);
            while(pages.size()>=kCachePages && !lru.empty()) { pages.erase(lru.front()); lru.pop_front(); }
            pages[page]=std::move(rows); lru.push_back(page);
        }
        Notify();
    }
    // Caller holds io. Read replacements before publishing so paints retain
    // complete old pages throughout an order or live-result update.
    void RefreshPages(size_t new_count) {
        std::map<size_t,std::vector<Row>> replacement;
        std::deque<size_t> recent;
        {
            std::lock_guard lock(mu);
            recent=lru;
        }
        // Old page positions are invalid after a mutation. Use only refreshed
        // anchors, while retaining the original LRU.
        const std::set<size_t> ascending(recent.begin(),recent.end());
        for (size_t page : ascending) {
            if (page*kPageSize>=new_count) continue;
            replacement[page]=ReadPage(page,new_count,CachedAnchor(replacement,page));
        }
        std::lock_guard lock(mu);
        ++page_epoch; ++revision; pending.clear(); pages.swap(replacement); lru.clear();
        for (size_t page : recent) if (pages.contains(page)) lru.push_back(page);
        count=new_count;
    }
};
ContentResultStore::ContentResultStore(HWND notify,UINT message):impl_(std::make_shared<Impl>(notify,message)) {
    std::thread([state=impl_]{state->Run();}).detach();
}
ContentResultStore::~ContentResultStore() {
    // Publish shutdown under the wait mutex so notification cannot fall
    // between the worker's predicate check and its transition to sleep.
    { std::lock_guard lock(impl_->mu); impl_->stopping=true; }
    impl_->wake.notify_all();
}
bool ContentResultStore::Append(const std::vector<ContentHit>& hits) {
    if(hits.empty()) return true;
    auto& p=*impl_; std::unique_lock lock(p.io);
    if (p.stream_order) {
        lock.unlock();
        return UpsertChanges(hits,ContentResultSort::Index,false,true,true);
    }
    std::string order;
    {
        std::lock_guard cache(p.mu);
        order=p.requested_order;
    }
    if(p.stopping || !p.Open() || !p.EnsureOrderIndex(order) || !p.Exec("BEGIN")) { p.error=ERROR_DATABASE_FAILURE; p.Notify(); return false; }
    Statement insert(p.db,"INSERT INTO hits VALUES(?1,?2,?3,?4,?5,?6,?7,?8)");
    Statement visible(p.db,"INSERT INTO visible VALUES(?1,?2)");
    size_t raw=p.raw, count=p.count;
    bool ok=insert.p && visible.p;
    for(const auto& hit:hits) {
        if(!ok || p.stopping) { ok=false; break; }
        insert.Int(1,raw); insert.Text(2,hit.name); insert.Text(3,hit.path);
        insert.Int(4,hit.size); insert.Int(5,hit.modified); insert.Int(6,hit.line); insert.Text(7,hit.snippet); insert.Int(8,hit.file_id);
        ok=insert.Step(); insert.Reset();
        fs::DirEntry entry; entry.name=hit.name; entry.full_path=hit.path; entry.size=hit.size; entry.attrs=FILE_ATTRIBUTE_NORMAL;
        entry.mtime={static_cast<DWORD>(hit.modified),static_cast<DWORD>(hit.modified >> 32)};
        if(ok && (!p.filter || p.filter(entry))) {
            visible.Int(1,count++); visible.Int(2,raw); ok=visible.Step(); visible.Reset();
        }
        ++raw;
    }
    if (ok && !order.empty()) {
        // New hits have already passed the active filter. Merge their display
        // positions using the reusable disk index. Swapping one completed table
        // avoids sorting and writing every prior position twice per small batch.
        const std::string membership=p.filter ? " WHERE seq IN (SELECT seq FROM visible)" : "";
        ok=p.Exec("CREATE TABLE appended_order(pos INTEGER PRIMARY KEY,seq INTEGER)") &&
           p.Exec(("INSERT INTO appended_order SELECT row_number() OVER (ORDER BY "+order+
                   ")-1,seq FROM hits INDEXED BY hits_display_order"+membership).c_str()) &&
           p.Exec("DROP TABLE visible; ALTER TABLE appended_order RENAME TO visible;");
    }
    if(!ok || !p.Exec("COMMIT")) { p.Exec("ROLLBACK"); p.error=ERROR_DATABASE_FAILURE; p.Notify(); return false; }
    if (!order.empty()) p.order=std::move(order);
    p.raw=raw;
    p.RefreshPages(count);
    return true;
}
size_t ContentResultStore::Count() const { return impl_->count; }
bool ContentResultStore::ApplyChanges(const std::vector<ContentHit>& hits, ContentResultSort sort, bool descending) {
    return UpsertChanges(hits,sort,descending,false);
}
bool ContentResultStore::StreamUpsert(const std::vector<ContentHit>& hits, ContentResultSort sort, bool descending) {
    return UpsertChanges(hits,sort,descending,true);
}
bool ContentResultStore::UpsertChanges(const std::vector<ContentHit>& hits, ContentResultSort sort, bool descending,
                                     bool streaming, bool retain_order) {
    if (hits.empty()) return true;
    auto& p = *impl_; std::lock_guard lock(p.io);
    if (streaming) p.stream_order=true;
    std::string order = retain_order ? p.order : SortOrder(sort,descending,p.stream_order);
    {
        std::lock_guard cache(p.mu);
        if (!p.requested_order.empty()) order=p.requested_order;
    }
    if (p.stopping || !p.Open() || (streaming && !p.EnsureOrderIndex(order)) || !p.Exec("BEGIN")) { p.error=ERROR_DATABASE_FAILURE; p.Notify(); return false; }
    bool ok = p.Exec("CREATE UNIQUE INDEX IF NOT EXISTS hits_path ON hits(path COLLATE PULSE_ORDINAL)");
    if (streaming) ok = ok && p.Exec("CREATE UNIQUE INDEX IF NOT EXISTS visible_identity ON visible(seq)");
    size_t raw = p.raw;
    std::set<uint64_t> affected;
    Statement affected_rows(p.db,"SELECT seq FROM hits WHERE path=?1 COLLATE PULSE_ORDINAL OR (file_id=?2 AND file_id<>0)");
    Statement existing(p.db,"SELECT seq FROM hits WHERE path=?1 COLLATE PULSE_ORDINAL");
    Statement remove(p.db, "DELETE FROM hits WHERE path=?1 COLLATE PULSE_ORDINAL AND (?2=0 OR file_id=?2)");
    // A temporary task result gaining its index identity is still the same
    // displayed row. Only a genuine overwrite/rename conflict removes it.
    Statement conflict(p.db, "DELETE FROM hits WHERE path=?1 COLLATE PULSE_ORDINAL AND file_id<>?2 AND (file_id<>0 OR EXISTS(SELECT 1 FROM hits WHERE file_id=?2))");
    Statement identity(p.db, "UPDATE hits SET path=?1 WHERE file_id=?2 AND file_id<>0");
    Statement put(p.db, "INSERT INTO hits(seq,name,path,size,mtime,line,snippet,file_id) VALUES((SELECT coalesce(max(seq),-1)+1 FROM hits),?1,?2,?3,?4,?5,?6,?7) ON CONFLICT(path) DO UPDATE SET name=excluded.name,size=excluded.size,mtime=excluded.mtime,line=excluded.line,snippet=excluded.snippet,file_id=CASE WHEN excluded.file_id=0 THEN hits.file_id ELSE excluded.file_id END");
    for (const auto& hit : hits) {
        if (!ok || p.stopping) { ok = false; break; }
        if (streaming) {
            if (!affected_rows.p || !existing.p) { ok=false; break; }
            affected_rows.Text(1,hit.path); affected_rows.Int(2,hit.file_id);
            int rc;
            while ((rc=sqlite3_step(affected_rows.p))==SQLITE_ROW)
                affected.insert(static_cast<uint64_t>(sqlite3_column_int64(affected_rows.p,0)));
            ok=rc==SQLITE_DONE; affected_rows.Reset();
            if (!ok) break;
        }
        if (hit.removed) {
            remove.Text(1,hit.path); remove.Int(2,hit.file_id); ok=remove.Step();
            if (ok && streaming) raw-=static_cast<size_t>(sqlite3_changes(p.db));
            remove.Reset();
        }
        else {
            if (hit.file_id) {
                conflict.Text(1,hit.path); conflict.Int(2,hit.file_id); ok=conflict.Step();
                if (ok && streaming) raw-=static_cast<size_t>(sqlite3_changes(p.db));
                conflict.Reset();
                identity.Text(1,hit.path); identity.Int(2,hit.file_id); ok=ok && identity.Step(); identity.Reset();
                if (!ok) break;
            }
            bool inserted=false;
            if (streaming) {
                existing.Text(1,hit.path);
                const auto rc=sqlite3_step(existing.p);
                inserted=rc==SQLITE_DONE; ok=rc==SQLITE_ROW || inserted;
                existing.Reset(); if (!ok) break;
            }
            put.Text(1,hit.name); put.Text(2,hit.path); put.Int(3,hit.size); put.Int(4,hit.modified);
            put.Int(5,hit.line); put.Text(6,hit.snippet); put.Int(7,hit.file_id); ok=put.Step(); put.Reset();
            if (ok && streaming) {
                if (inserted) ++raw;
                existing.Text(1,hit.path);
                ok=sqlite3_step(existing.p)==SQLITE_ROW;
                if (ok) affected.insert(static_cast<uint64_t>(sqlite3_column_int64(existing.p,0)));
                existing.Reset();
            }
        }
    }
    if (streaming) {
        // Membership changes only for affected identities. Display positions
        // come directly from the reusable order index when a page is read.
        Statement erase(p.db,"DELETE FROM visible WHERE seq=?1");
        Statement row(p.db,"SELECT name,path,size,mtime,line,snippet,file_id,seq FROM hits WHERE seq=?1");
        Statement admit(p.db,"INSERT INTO visible VALUES((SELECT coalesce(max(pos),-1)+1 FROM visible),?1)");
        size_t count=p.count;
        ok=ok && erase.p && row.p && admit.p;
        for (const auto seq:affected) {
            if (!ok || p.stopping) { ok=false; break; }
            erase.Int(1,seq); ok=erase.Step();
            if (ok) count-=static_cast<size_t>(sqlite3_changes(p.db));
            erase.Reset(); if (!ok) break;
            row.Int(1,seq); const auto rc=sqlite3_step(row.p);
            if (rc==SQLITE_ROW && (!p.filter || p.filter(ReadRow(row.p).entry))) {
                admit.Int(1,seq); ok=admit.Step(); admit.Reset(); if (ok) ++count;
            } else if (rc!=SQLITE_ROW && rc!=SQLITE_DONE) ok=false;
            row.Reset();
        }
        if (!ok || !p.Exec("COMMIT")) { p.Exec("ROLLBACK"); p.error=ERROR_DATABASE_FAILURE; p.Notify(); return false; }
        p.order=std::move(order); p.raw=raw; p.RefreshPages(count); p.Notify(); return true;
    }
    // Rebuild only the compact display order; unchanged bodies and hit rows
    // stay in the disk spool. Page caches remain bounded after every mutation.
    ok = ok && p.Exec("DELETE FROM visible");
    Statement rows(p.db, ("SELECT name,path,size,mtime,line,snippet,file_id,seq FROM hits ORDER BY " + order).c_str());
    Statement visible(p.db,"INSERT INTO visible(pos,seq) VALUES(?1,?2)");
    size_t count = 0; raw = 0;
    int rc = SQLITE_DONE;
    while (ok && rows.p && (rc=sqlite3_step(rows.p)) == SQLITE_ROW) {
        const auto row = ReadRow(rows.p); ++raw;
        if (!p.filter || p.filter(row.entry)) {
            visible.Int(1,count++); visible.Int(2,static_cast<uint64_t>(sqlite3_column_int64(rows.p,7)));
            ok=visible.Step(); visible.Reset();
        }
    }
    ok = ok && rows.p && rc == SQLITE_DONE;
    if (!ok || !p.Exec("COMMIT")) { p.Exec("ROLLBACK"); p.error=ERROR_DATABASE_FAILURE; return false; }
    p.order=std::move(order); p.raw=raw;
    p.RefreshPages(count);
    p.Notify(); return true;
}
size_t ContentResultStore::RawCount() const { return impl_->raw; }
bool ContentResultStore::SameContents(const ContentResultStore& other, HANDLE cancel) const {
    if (this == &other) return true;
    auto& a = *impl_; auto& b = *other.impl_;
    std::scoped_lock lock(a.io, b.io);
    if (a.error || b.error || a.raw != b.raw) return false;
    if (!a.raw) return true;
    if (!a.Open() || !b.Open()) return false;
    constexpr auto sql = "SELECT name,path,size,mtime,line,snippet,file_id FROM hits ORDER BY seq";
    Statement left(a.db, sql), right(b.db, sql);
    if (!left.p || !right.p) return false;
    while (!cancel || WaitForSingleObject(cancel, 0) != WAIT_OBJECT_0) {
        const int l = sqlite3_step(left.p), r = sqlite3_step(right.p);
        if (l != r) return false;
        if (l == SQLITE_DONE) return true;
        if (l != SQLITE_ROW) return false;
        for (int i : {0, 1, 5}) if (Column(left.p, i) != Column(right.p, i)) return false;
        for (int i : {2, 3, 4, 6})
            if (sqlite3_column_int64(left.p, i) != sqlite3_column_int64(right.p, i)) return false;
    }
    return false;
}
uint64_t ContentResultStore::Revision() const { return impl_->revision; }
DWORD ContentResultStore::Error() const { return impl_->error; }
bool ContentResultStore::Filtering() const { return impl_->filtering; }
bool ContentResultStore::Sorting() const { return impl_->sorting; }
bool ContentResultStore::Get(size_t index,Row& row) const {
    {
        std::lock_guard lock(impl_->mu);
        const auto found=impl_->pages.find(index/kPageSize);
        if(found!=impl_->pages.end() && index%kPageSize<found->second.size()) {
            row=found->second[index%kPageSize]; return true;
        }
    }
    Prefetch(index); return false;
}
bool ContentResultStore::Ready(size_t index) const {
    std::lock_guard lock(impl_->mu);
    const auto found=impl_->pages.find(index/kPageSize);
    return found!=impl_->pages.end() && index%kPageSize<found->second.size();
}
void ContentResultStore::Prefetch(size_t index) const {
    if(index>=Count() || Filtering()) return;
    const auto page=index/kPageSize;
    uint64_t epoch;
    {
        std::lock_guard lock(impl_->mu);
        const auto found=impl_->pages.find(page);
        if((found!=impl_->pages.end() && index%kPageSize<found->second.size()) || impl_->pending.contains(page) || impl_->pending.size()>=kCachePages) return;
        // Register against the same generation that owns pending. Otherwise a
        // refresh between capture and insertion can strand a stale request.
        epoch=impl_->page_epoch.load();
        impl_->pending.insert(page);
    }
    impl_->Enqueue([state=impl_,page,epoch]{state->Load(page,epoch);});
}
void ContentResultStore::SetFilter(Filter filter) {
    const auto epoch=++impl_->filter_epoch;
    { std::lock_guard lock(impl_->mu); ++impl_->page_epoch; impl_->pending.clear(); }
    impl_->filtering=true;
    impl_->Enqueue([p=impl_,epoch,filter=std::move(filter)]() mutable {
        {
            std::lock_guard lock(p->io);
            if(epoch!=p->filter_epoch || p->stopping) return;
            if(!p->Open() || !p->Exec("BEGIN; DELETE FROM visible;")) { p->error=ERROR_DATABASE_FAILURE; p->filtering=false; p->Notify(); return; }
            Statement read(p->db,("SELECT name,path,size,mtime,line,snippet,file_id,seq FROM hits ORDER BY " + p->order).c_str());
            Statement write(p->db,"INSERT INTO visible VALUES(?1,?2)");
            size_t count=0; int rc=SQLITE_DONE; bool ok=read.p && write.p;
            while(ok && !p->stopping && epoch==p->filter_epoch && (rc=sqlite3_step(read.p))==SQLITE_ROW) {
                const auto row=ReadRow(read.p);
                if(!filter || filter(row.entry)) {
                    write.Int(1,count++); write.Int(2,static_cast<uint64_t>(sqlite3_column_int64(read.p,7)));
                    ok=write.Step(); write.Reset();
                }
            }
            if(!ok || rc!=SQLITE_DONE || epoch!=p->filter_epoch || p->stopping) {
                p->Exec("ROLLBACK");
                if(epoch==p->filter_epoch && !p->stopping) p->error=ERROR_DATABASE_FAILURE;
            }
            else if(p->Exec("COMMIT")) {
                p->filter=std::move(filter);
                std::lock_guard cache(p->mu); ++p->page_epoch; p->pages.clear(); p->lru.clear(); p->pending.clear(); p->count=count;
            } else {p->Exec("ROLLBACK");p->error=ERROR_DATABASE_FAILURE;}
            if(epoch==p->filter_epoch) p->filtering=false;
        }
        p->Notify();
    });
    impl_->Notify();
}
void ContentResultStore::SetSort(ContentResultSort sort, bool descending) {
    const auto order=SortOrder(sort,descending,impl_->stream_order);
    uint64_t epoch;
    {
        std::lock_guard lock(impl_->mu);
        impl_->requested_order=order;
        epoch=++impl_->sort_epoch;
        impl_->sorting=true;
    }
    impl_->Enqueue([p=impl_,order,epoch] {
        std::lock_guard lock(p->io);
        if (p->stopping || epoch!=p->sort_epoch) return;
        if (p->stream_order) {
            const bool ok=p->Open() && p->EnsureOrderIndex(order,epoch);
            if (p->stopping || epoch!=p->sort_epoch) return;
            if (!ok) p->error=ERROR_DATABASE_FAILURE;
            else { p->order=order; p->RefreshPages(p->count); }
            p->FinishSort(epoch); p->Notify(); return;
        }
        // Sort only identities already admitted by the current filter. No body
        // matching or filter callback is needed for a direction change.
        if (!p->Open() || !p->Exec("BEGIN; CREATE TEMP TABLE sorted(pos INTEGER PRIMARY KEY,seq INTEGER);")) {
            p->Exec("ROLLBACK"); p->error=ERROR_DATABASE_FAILURE;
            p->FinishSort(epoch);
            p->Notify(); return;
        }
        Statement read(p->db,("SELECT seq FROM hits WHERE seq IN (SELECT seq FROM visible) ORDER BY "+order).c_str());
        Statement write(p->db,"INSERT INTO sorted VALUES(?1,?2)");
        Impl::SortProgress progress{p.get(),epoch};
        sqlite3_progress_handler(p->db,4096,Impl::CancelSort,&progress);
        size_t count=0; int rc=SQLITE_DONE; bool ok=read.p && write.p;
        while (ok && !p->stopping && epoch==p->sort_epoch && (rc=sqlite3_step(read.p))==SQLITE_ROW) {
            write.Int(1,count++); write.Int(2,static_cast<uint64_t>(sqlite3_column_int64(read.p,0)));
            ok=write.Step(); write.Reset();
        }
        sqlite3_progress_handler(p->db,0,nullptr,nullptr);
        ok=ok && rc==SQLITE_DONE && p->Exec("DELETE FROM visible; INSERT INTO visible SELECT pos,seq FROM sorted; DROP TABLE sorted;");
        {
            std::lock_guard cache(p->mu);
            if (p->stopping || epoch!=p->sort_epoch) { p->Exec("ROLLBACK"); return; }
            ok=ok && p->Exec("COMMIT");
        }
        if (!ok) {
            p->Exec("ROLLBACK"); p->error=ERROR_DATABASE_FAILURE;
            p->FinishSort(epoch);
            p->Notify(); return;
        }
        p->order=order;
        p->RefreshPages(count);
        p->FinishSort(epoch);
        p->Notify();
    });
}
void ContentResultStore::Resolve(std::vector<int> indices,bool all,size_t count,std::function<void(Selection)> complete) {
    const auto epoch=impl_->page_epoch.load();
    impl_->Enqueue([p=impl_,indices=std::move(indices),all,count,epoch,complete=std::move(complete)]() mutable {
        Selection result;
        {
            std::lock_guard lock(p->io);
            if(p->stopping || epoch!=p->page_epoch || !p->Open()) result.error=ERROR_CANCELLED;
            else if (p->stream_order && !all) {
                std::map<int,Row> found;
                result.error=p->ScanSelection(indices,[&](int index,sqlite3_stmt* row){found.emplace(index,ReadRow(row));});
                for (int index:indices) {
                    if (index<0 || p->stopping) { result.error=ERROR_CANCELLED; break; }
                    const auto row=found.find(index);
                    if (row==found.end()) { if (!result.error) result.error=ERROR_DATABASE_FAILURE; break; }
                    result.rows.emplace_back(static_cast<size_t>(index),row->second);
                }
            }
            else {
                Statement read(p->db,p->RowsSql().c_str());
                if(!read.p) result.error=ERROR_DATABASE_FAILURE;
                else if(all) {
                    read.Int(1,0); read.Int(2,count); size_t index=0; int rc=SQLITE_DONE;
                    while(!p->stopping && (rc=sqlite3_step(read.p))==SQLITE_ROW) result.rows.emplace_back(index++,ReadRow(read.p));
                    if(rc!=SQLITE_DONE || index!=count) result.error=ERROR_DATABASE_FAILURE;
                } else for(int index:indices) {
                    if(index<0 || p->stopping) { result.error=ERROR_CANCELLED; break; }
                    read.Int(1,static_cast<size_t>(index)); read.Int(2,static_cast<size_t>(index)+1);
                    if(sqlite3_step(read.p)!=SQLITE_ROW) { result.error=ERROR_DATABASE_FAILURE; break; }
                    result.rows.emplace_back(static_cast<size_t>(index),ReadRow(read.p)); read.Reset();
                }
            }
        }
        complete(std::move(result));
    });
}
void ContentResultStore::FindPath(std::wstring path,std::function<void(int)> complete) {
    const auto epoch=impl_->page_epoch.load();
    impl_->Enqueue([p=impl_,path=std::move(path),epoch,complete=std::move(complete)] {
        int index=-1;
        {
            std::lock_guard lock(p->io);
            if(!p->stopping && epoch==p->page_epoch && p->Open()) {
                const auto predicate=p->stream_order ? "path=?1 COLLATE PULSE_ORDINAL AND path=?1" : "path=?1";
                Statement read(p->db,p->PositionSql(predicate).c_str());
                if(read.p) {read.Text(1,path);if(sqlite3_step(read.p)==SQLITE_ROW) index=sqlite3_column_int(read.p,0);}
            }
        }
        complete(index);p->Notify();
    });
}
void ContentResultStore::FindIdentity(uint64_t file_id,std::function<void(int)> complete) {
    const auto epoch=impl_->page_epoch.load();
    impl_->Enqueue([p=impl_,file_id,epoch,complete=std::move(complete)] {
        int index=-1;
        {
            std::lock_guard lock(p->io);
            if(!p->stopping && epoch==p->page_epoch && p->Open()) {
                Statement read(p->db,p->PositionSql("file_id=?1 AND file_id<>0").c_str());
                if(read.p) {read.Int(1,file_id);if(sqlite3_step(read.p)==SQLITE_ROW) index=sqlite3_column_int(read.p,0);}
            }
        }
        complete(index);p->Notify();
    });
}
void ContentResultStore::Match(Filter filter,std::function<void(Selection)> complete) {
    impl_->Enqueue([p=impl_,filter=std::move(filter),complete=std::move(complete)] {
        Selection result;
        {
            std::lock_guard lock(p->io);
            result.page_epoch=p->page_epoch.load();
            if(p->stopping) result.error=ERROR_CANCELLED;
            else if(!p->Open()) result.error=ERROR_DATABASE_FAILURE;
            else {
                Statement read(p->db,p->RowsSql().c_str());
                read.Int(1,0); read.Int(2,p->count);
                int rc=SQLITE_DONE;
                if(!read.p) result.error=ERROR_DATABASE_FAILURE;
                else {
                    int index=0;
                    while(!p->stopping && (rc=sqlite3_step(read.p))==SQLITE_ROW) {
                        if(!filter || filter(ReadRow(read.p).entry)) result.matches.push_back(index);
                        ++index;
                    }
                    if(rc!=SQLITE_DONE) result.error=ERROR_CANCELLED;
                }
            }
        }
        complete(std::move(result));
    });
}
bool ContentResultStore::ApplyMatches(const Selection& selection,
        const std::function<void(const std::vector<int>&)>& apply) const {
    std::lock_guard lock(impl_->mu);
    if(selection.error || impl_->stopping || selection.page_epoch!=impl_->page_epoch) return false;
    apply(selection.matches);
    return true;
}
void ContentResultStore::Sum(std::vector<int> indices,bool all,size_t count,std::function<void(uint64_t,DWORD)> complete) {
    const auto epoch=impl_->page_epoch.load();
    impl_->Enqueue([p=impl_,indices=std::move(indices),all,count,epoch,complete=std::move(complete)] {
        uint64_t bytes=0;DWORD error=0;
        {
            std::lock_guard lock(p->io);
            if(epoch!=p->page_epoch || !p->Open()) error=ERROR_CANCELLED;
            else {
                if (p->stream_order) {
                    if (!all) {
                        std::map<int,uint64_t> found;
                        error=p->ScanSelection(indices,[&](int index,sqlite3_stmt* row) {
                            found.emplace(index,static_cast<uint64_t>(sqlite3_column_int64(row,2)));
                        });
                        for (int index:indices) {
                            if (index<0 || p->stopping) { error=ERROR_CANCELLED; break; }
                            const auto value=found.find(index);
                            if (value!=found.end()) bytes+=value->second;
                            else if (!error) error=ERROR_DATABASE_FAILURE;
                        }
                    } else {
                        Statement read(p->db,p->RowsSql().c_str());
                        read.Int(1,0); read.Int(2,count);
                        size_t rows=0; int rc=SQLITE_DONE;
                        while (read.p && !p->stopping && (rc=sqlite3_step(read.p))==SQLITE_ROW) {
                            bytes+=static_cast<uint64_t>(sqlite3_column_int64(read.p,2)); ++rows;
                        }
                        if (!read.p || p->stopping || rc!=SQLITE_DONE || rows!=count) error=ERROR_DATABASE_FAILURE;
                    }
                } else {
                Statement read(p->db,all ? "SELECT sum(h.size) FROM visible v JOIN hits h ON h.seq=v.seq WHERE v.pos<?1" : "SELECT h.size FROM visible v JOIN hits h ON h.seq=v.seq WHERE v.pos=?1");
                if(!read.p) error=ERROR_DATABASE_FAILURE;
                else if(all) { read.Int(1,count);if(sqlite3_step(read.p)==SQLITE_ROW) bytes=static_cast<uint64_t>(sqlite3_column_int64(read.p,0));else error=ERROR_DATABASE_FAILURE; }
                else for(int index:indices) {
                    if(p->stopping) {error=ERROR_CANCELLED;break;}
                    read.Int(1,static_cast<size_t>(index));
                    if(sqlite3_step(read.p)==SQLITE_ROW) bytes+=static_cast<uint64_t>(sqlite3_column_int64(read.p,0));else error=ERROR_DATABASE_FAILURE;
                    read.Reset();
                }
                }
            }
        }
        complete(bytes,error);p->Notify();
    });
}
size_t ContentResultStore::CachedRows() const {
    std::lock_guard lock(impl_->mu); size_t count=0;
    for(const auto& pair:impl_->pages) count+=pair.second.size();
    return count;
}
std::wstring ContentResultStore::CachePath() const { std::lock_guard lock(impl_->io); return impl_->path; }
} // namespace pulse::index
