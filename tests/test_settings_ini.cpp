#include "fangcun/SettingsIni.hpp"

#include <iostream>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " - " << #condition << "\n"; \
        return 1; \
    } \
} while (false)

int main() {
    using namespace fangcun::settings;
    const std::string original = "# keep exactly\r\nfuture = untouched = yes\r\n\r\n"
        "scheme = flypy\r\nclipboard_enabled=1\r\npage_size=7\r\n; trailing comment";
    const auto firstSnapshot = parse(original);
    const auto secondSnapshot = firstSnapshot;
    auto firstEdit = firstSnapshot;
    firstEdit["skin"] = "terminal";
    firstEdit["theme_mode"] = "dark";
    const auto firstCommit = merge(original, changed(firstSnapshot, firstEdit));
    auto secondEdit = secondSnapshot;
    secondEdit["clipboard_enabled"] = "0";
    const auto merged = merge(firstCommit, changed(secondSnapshot, secondEdit));
    const auto actual = parse(merged);
    CHECK(actual.at("skin") == "terminal");
    CHECK(actual.at("theme_mode") == "dark");
    CHECK(actual.at("clipboard_enabled") == "0");
    CHECK(actual.at("scheme") == "flypy" && actual.at("page_size") == "7");
    CHECK(merged.starts_with("# keep exactly\r\nfuture = untouched = yes\r\n\r\n"));
    CHECK(merged.find("; trailing comment\r\n") != std::string::npos);
    CHECK(merge(original, {}) == original);

    const auto duplicates = merge("page_size=5\n# between\npage_size=7\n", {{"page_size", "9"}});
    CHECK(duplicates == "page_size=9\n# between\npage_size=9\n");
    CHECK(parse(duplicates).at("page_size") == "9");
    CHECK(parse("\xEF\xBB\xBFscheme=flypy\n").at("scheme") == "flypy");
    CHECK(parse("  mixed_input = false \r\n").at("mixed_input") == "false");
    CHECK(merge("# no final newline", {{"scheme", "pinyin"}}) == "# no final newline\nscheme=pinyin\n");
    CHECK(changed(firstSnapshot, firstSnapshot).empty());
    for (const Values updates : {
        Values{{"skin", "terminal\nclipboard_enabled=1"}},
        Values{{"skin\rfoo", "terminal"}},
        Values{{"x=y", "z"}},
        Values{{"skin", std::string("term\0inal", 9)}},
    }) {
        bool rejected = false;
        try { (void)merge(original, updates); } catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    }
    std::cout << "[PASS] Native settings merge preserves concurrent edits, unknown lines and field boundaries\n";
}
