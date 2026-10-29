"""Save public issue evidence for the follow-up audit; no GitHub writes."""
import concurrent.futures
import datetime
import json
import pathlib
import re
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent
PREVIOUS = ROOT.parent / "issue-bug-audit-2026-10-23" / "sources.json"
DESTINATION = ROOT / "sources"
DESTINATION.mkdir(exist_ok=True)


def fetch(number):
    path = DESTINATION / f"{number}.json"
    if path.exists() and path.stat().st_size:
        embedded = json.loads(path.read_text(encoding="utf-8-sig"))
    else:
        request = urllib.request.Request(
            f"https://github.com/jimmgreen/pulse/issues/{number}",
            headers={"User-Agent": "Pulse-local-bug-audit"},
        )
        html = urllib.request.urlopen(request, timeout=35).read().decode("utf-8")
        match = re.search(
            r'<script type="application/json" data-target="react-app.embeddedData">(.*?)</script>',
            html, re.S,
        )
        if not match:
            raise RuntimeError("issue payload missing")
        embedded = json.loads(match.group(1))
        path.write_text(json.dumps(embedded, ensure_ascii=False), encoding="utf-8")
    issue = embedded["payload"]["issueViewerRoute"]["data"]["repository"]["issue"]
    comments = [edge["node"] for edge in issue["timelineItems"]["edges"]
                if edge["node"].get("__typename") == "IssueComment"]
    excerpt = {
        "number": number, "title": issue["title"], "body": issue["body"],
        "url": f"https://github.com/jimmgreen/pulse/issues/{number}",
        "captured_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "updated_at": issue.get("updatedAt"),
        "comments": [{"body": comment.get("body"), "bodyHTML": comment.get("bodyHTML"),
                      "author": comment.get("author"), "url": comment.get("url")}
                     for comment in comments],
        "timeline_page_info": issue["timelineItems"].get("pageInfo"),
    }
    (DESTINATION / f"{number}-readable.json").write_text(
        json.dumps(excerpt, ensure_ascii=False, indent=2), encoding="utf-8")
    return {"number": number, "ok": True, "comments_read": len(comments),
            "timeline_has_more": issue["timelineItems"].get("pageInfo", {}).get("hasNextPage")}


if __name__ == "__main__":
    source = json.loads(PREVIOUS.read_text(encoding="utf-8-sig"))
    numbers = [entry["number"] for entry in source["issues"]
               if entry["classification"] == "未确认"]
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
        pending = {executor.submit(fetch, number): number for number in numbers}
        for future in concurrent.futures.as_completed(pending):
            number = pending[future]
            try:
                result = future.result()
            except Exception as error:
                result = {"number": number, "ok": False, "error": str(error)}
            results.append(result)
            print(json.dumps(result, ensure_ascii=False), flush=True)
    (ROOT / "source-refresh.json").write_text(
        json.dumps(sorted(results, key=lambda item: -item["number"]), indent=2), encoding="utf-8")
