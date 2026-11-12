// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ops/ops_manager.h"
#include "../common/json_utils.h"
#include <iostream>

int RunJsonUndoParserRegressions() {
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n'; failures += !ok;
    };
    pulse::ops::OpsManager manager;
    const std::wstring type = std::to_wstring(static_cast<int>(pulse::ops::OpType::Rename));
    const std::wstring old = L"[{\"type\":" + type +
        LR"(,"sup":true,"dest":"","name":"next","src":["C:\\fixture\\old"]}])";
    check(manager.UndoFromJson(old), "actual old undo schema without dst or produced remains readable");
    const std::wstring unicode = L"[{\"type\":" + type +
        LR"(,"sup":true,"dest":"","name":"\u6C49\uD83D\uDE00\b\f","src":["C:\\fixture\\brace}]"]}])";
    check(manager.UndoFromJson(unicode), "undo reads full escapes and braces inside strings");
    const auto serialized = manager.UndoToJson();
    const pulse::json::Document document(serialized);
    check(document.Is(pulse::json::Kind::Array) && document.Root().children.size() == 1 &&
          pulse::json::String(document.Root().children.front().Find(L"name")) == L"汉\U0001F600\b\f",
          "undo writer and production reader preserve decoded Unicode and controls");
    for (const auto* bad : {LR"([{"type":999999999999,"sup":true,"dest":"","name":"x","src":[]}])",
                            LR"([{"type":2,"sup":truebad,"dest":"","name":"x","src":[]}])",
                            LR"([{"type":2,"sup":true,"dest":"","name":"\uD800","src":[]}])",
                            LR"([{"type":2,"sup":true,"dest":"","name":"x","src":["a" "b"]}])"})
        check(!manager.UndoFromJson(bad) && manager.UndoToJson() == serialized,
              "bad undo token overflow or escape rejected without replacing prior history");
    check(manager.UndoFromJson(LR"([{"type":0,"sup":true,"dest":"C:\\dest","name":"","src":["C:\\src"]}])") &&
          !manager.CanUndo(), "old copy lacking produced identities keeps existing disabled-undo behavior");
    const std::wstring arrays = LR"([{"type":1,"sup":false,"dest":"C:\\dest\u6C49","name":"quoted\"","src":["C:\\one","C:\\two"],"dst":["D:\\one","D:\\two"],"cleanup":["D:\\two"],"undo_group":"42","undo_label":"line\nnext"}])";
    check(manager.UndoFromJson(arrays), "undo accepts source destination and cleanup arrays for serialization");
    const auto array_json = manager.UndoToJson();
    const pulse::json::Document array_doc(array_json);
    const auto& entries = array_doc.Root().children;
    check(array_doc.Is(pulse::json::Kind::Array) && entries.size() == 1 &&
          pulse::json::Strings(entries[0].Find(L"src")) == std::vector<std::wstring>{L"C:\\one", L"C:\\two"} &&
          pulse::json::Strings(entries[0].Find(L"dst")) == std::vector<std::wstring>{L"D:\\one", L"D:\\two"} &&
          pulse::json::Strings(entries[0].Find(L"cleanup")) == std::vector<std::wstring>{L"D:\\two"} &&
          pulse::json::String(entries[0].Find(L"dest")) == L"C:\\dest汉" &&
          pulse::json::String(entries[0].Find(L"name")) == L"quoted\"" &&
          pulse::json::String(entries[0].Find(L"undo_label")) == L"line\nnext" &&
          pulse::json::String(entries[0].Find(L"undo_group")) == L"42",
          "direct undo serialization preserves all arrays and escaped string fields");
    return failures ? 1 : 0;
}
