#include "KeyHandler.hpp"
#include "fangcun/CandidateCellLayout.hpp"
#include "fangcun/ThemeConfig.hpp"

#include <iostream>
#include <limits>
#include <string>
#include <vector>

#define KH_ASSERT(cond, msg)                                                      \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " - " << msg \
                      << std::endl;                                               \
            return 1;                                                             \
        }                                                                         \
    } while (0)

namespace {
struct DeferredInputFixture {
    struct Query { uint64_t generation; std::string input; };
    std::vector<Query> queries;
    std::vector<std::string> learned;
    fangcun::tsf::KeyHandler handler;

    DeferredInputFixture() : handler(
        [this](uint64_t generation, std::string input, fangcun::InputScheme, bool) {
            queries.push_back({generation, std::move(input)});
            return generation;
        },
        [this](std::string_view word) { learned.emplace_back(word); }, [] {}) {}

    fangcun::tsf::KeyAction type(char character) {
        uint32_t vk = character >= 'a' && character <= 'z' ? character - 'a' + 'A' : character;
        bool shift = false;
        // Win32 supplies physical VK_* codes, not ASCII punctuation codes
        // (ASCII '!' is VK_PRIOR and would incorrectly exercise PageUp).
        switch (character) {
            case ',': vk = 0xBC; break;
            case '.': vk = 0xBE; break;
            case '-': vk = 0xBD; break;
            case '=': vk = 0xBB; break;
            case '!': vk = '1'; shift = true; break;
            case '@': vk = '2'; shift = true; break;
            case '?': vk = 0xBF; shift = true; break;
            default: break;
        }
        return handler.onKeyDown({vk, character, false, false, false, shift});
    }
    fangcun::tsf::KeyAction press(uint32_t vk) { return handler.onKeyDown({vk}); }
    fangcun::tsf::KeyAction reply(size_t index, std::vector<std::string> words) {
        // A response may enqueue the next segment and reallocate queries.
        const auto query = queries.at(index);
        return handler.onAsyncCandidates(query.generation, query.input, std::move(words));
    }
    fangcun::tsf::KeyAction accept(const fangcun::tsf::KeyAction& action, bool followup = true) {
        return handler.acknowledgeCommit(action.commitReceipt, true, followup);
    }
};
} // namespace

int runKeyHandlerTests() {
    using namespace fangcun;
    using namespace fangcun::tsf;

    {
        // Enlarged two-character first candidate, six normal candidates and a separate pager.
        std::vector<float> widths{86, 60, 52, 60, 60, 60, 60};
        const auto measured = widths;
        fitHorizontalCandidateWidths(widths, 498.0f - 40.0f);
        KH_ASSERT(widths == measured, "A wide first word must use its measured width when the page fits");

        widths = {86, 200, 200, 60};
        fitHorizontalCandidateWidths(widths, 300.0f);
        KH_ASSERT((widths == std::vector<float>{86, 77, 77, 60}),
                  "Constrained horizontal pages preserve short words and shrink long words");

        widths = {400, 80, 130};
        fitHorizontalCandidateWidths(widths, 300.0f);
        KH_ASSERT((widths == std::vector<float>{120, 80, 100}),
                  "An unusually long first word must leave room for later candidates");

        widths = {300, 40};
        fitHorizontalCandidateWidths(widths, 300.0f);
        KH_ASSERT((widths == std::vector<float>{260, 40}),
                  "Return unused width to the first word when the other words fit");
    }

    {
        const auto measure = [](const std::wstring& value) { return static_cast<float>(value.size()) * 10.0f; };
        KH_ASSERT(fitPreeditTail(L"abcdefghij", 100.0f, measure) == L"abcdefghij",
                  "Preedit that fits exactly must retain its full spelling");
        KH_ASSERT(fitPreeditTail(L"abcdefghij", 40.0f, measure) == L"\u2026hij",
                  "A constrained preedit keeps the typing end and a measured leading ellipsis");
        KH_ASSERT(fitPreeditTail(L"abcdefghij", 10.0f, measure) == L"\u2026" &&
                      fitPreeditTail(L"abcdefghij", 5.0f, measure).empty(),
                  "A very narrow slot may show only the ellipsis or no text, never an overflowing glyph");
        KH_ASSERT(fitPreeditTail(L"", 100.0f, measure).empty() &&
                      fitPreeditTail(L"abc", 0.0f, measure).empty() &&
                      fitPreeditTail(L"abc", -10.0f, measure).empty() &&
                      fitPreeditTail(L"abc", std::numeric_limits<float>::quiet_NaN(), measure).empty(),
                  "Empty text and invalid or nonpositive display budgets must remain empty");
        const auto proportional = [](const std::wstring& value) {
            float width = 0.0f;
            for (const auto character : value) width += character == L'W' ? 12.0f : character == L'i' ? 3.0f : 7.0f;
            return width;
        };
        KH_ASSERT(fitPreeditTail(L"WWWiii", 19.0f, proportional) == L"\u2026iii",
                  "Preedit fitting must use actual font measurement rather than a character-count cutoff");

        bool splitPairMeasured = false;
        const auto measureUnicode = [&](const std::wstring& value) {
            float width = 0.0f;
            for (std::size_t i = 0; i < value.size(); ++i) {
                const auto character = static_cast<unsigned int>(value[i]);
                if (character >= 0xD800 && character <= 0xDBFF) {
                    if (i + 1 < value.size() && static_cast<unsigned int>(value[i + 1]) >= 0xDC00 &&
                        static_cast<unsigned int>(value[i + 1]) <= 0xDFFF) {
                        ++i; width += 14.0f; continue;
                    }
                    splitPairMeasured = true;
                } else if (character >= 0xDC00 && character <= 0xDFFF) splitPairMeasured = true;
                width += character == 0x2026 ? 7.0f : character > 0xFFFF ? 14.0f : 8.0f;
            }
            return width;
        };
        // Explicit surrogate units exercise Windows UTF-16 boundaries even on
        // Linux, whose wchar_t can represent a supplementary scalar directly.
        const std::wstring pair{L'a', static_cast<wchar_t>(0xD83D), static_cast<wchar_t>(0xDE00), L'b'};
        KH_ASSERT(fitPreeditTail(pair, 29.0f, measureUnicode) == std::wstring(L"\u2026") + pair.substr(1) &&
                      fitPreeditTail(pair, 20.0f, measureUnicode) == L"\u2026b" && !splitPairMeasured,
                  "All measured and displayed suffixes must keep a UTF-16 surrogate pair intact");
        for (int budget = 1; budget <= 40; ++budget) {
            const auto result = fitPreeditTail(pair, static_cast<float>(budget), measureUnicode);
            KH_ASSERT(measureUnicode(result) <= static_cast<float>(budget) && !splitPairMeasured,
                      "Every narrow Unicode budget must produce a bounded suffix without orphan surrogate units");
        }
        KH_ASSERT(fitPreeditTail(L"\U0001F600b", 22.0f, measureUnicode) == L"\U0001F600b" &&
                      fitPreeditTail(L"\U0001F600b", 21.0f, measureUnicode) == L"\u2026b",
                  "Native supplementary characters must work with both UTF-16 and scalar wchar_t");
        int measurements = 0;
        const auto counted = [&](const std::wstring& value) { ++measurements; return measure(value); };
        KH_ASSERT(fitPreeditTail(std::wstring(128, L'x'), 80.0f, counted) == L"\u2026xxxxxxx" && measurements <= 10,
                  "Long input must keep its tail with logarithmic rather than per-character font measurements");
    }

    uint64_t lastQuerySession = 0;
    std::string lastQueryInput;
    std::vector<std::string> learned;
    int cancelCount = 0;

    KeyHandler handler(
        [&](uint64_t sessionGen, std::string input, InputScheme, bool) -> uint64_t {
            lastQuerySession = sessionGen;
            lastQueryInput = std::move(input);
            return sessionGen;
        },
        [&](std::string_view text) {
            learned.emplace_back(text);
        },
        [&]() {
            ++cancelCount;
        });

    // 1. Ctrl/Alt/Win pass through
    KeyInput ctrlC{'C', 'c', true, false, false, false, false};
    KH_ASSERT(!handler.shouldEatKey(ctrlC), "Ctrl+C should pass through");
    KH_ASSERT(!handler.onKeyDown(ctrlC).handled, "Ctrl+C should not be handled");

    KeyInput winSpace{' ', ' ', false, false, true, false, false};
    KH_ASSERT(!handler.shouldEatKey(winSpace), "Win+Space should pass through");

    // 2. Backspace when empty passes through
    KeyInput bs{0x08, 0, false, false, false, false, false};
    KH_ASSERT(!handler.shouldEatKey(bs), "Backspace when empty should pass through");

    // 3. Typing 'w', 'o' -> composing "wo", Space waits if pending, then commits candidate[0]
    KeyInput keyW{'W', 'w', false, false, false, false, false};
    KeyInput keyO{'O', 'o', false, false, false, false, false};
    auto actW = handler.onKeyDown(keyW);
    KH_ASSERT(actW.handled && actW.compositionText == "w" && actW.candidatesPending, "w composing failed");
    auto actO = handler.onKeyDown(keyO);
    KH_ASSERT(actO.handled && actO.compositionText == "wo" && lastQueryInput == "wo", "wo composing failed");

    // Simulate Space pressed BEFORE async query finishes -> queued boundary!
    KeyInput space{0x20, ' ', false, false, false, false, false};
    auto actSpacePending = handler.onKeyDown(space);
    KH_ASSERT(actSpacePending.handled && !actSpacePending.commitText, "Pending space should wait for async query");

    // Async query returns for "wo" -> immediately commits "我"
    auto actAsync = handler.onAsyncCandidates(lastQuerySession, "wo", {"我", "窝", "wo"});
    KH_ASSERT(actAsync.commitText && actAsync.textToCommit == "我", "Queued space did not commit candidate[0]");
    KH_ASSERT(handler.composing().empty(), "Composing should be empty after commit");
    KH_ASSERT(learned.empty() && actAsync.commitReceipt != 0, "Proposed text must not be learned before host acknowledgement");
    handler.acknowledgeCommit(actAsync.commitReceipt, true);
    KH_ASSERT(learned.size() == 1 && learned[0] == "我", "Learning callback not invoked");

    // 4. Stale async query after focus switch (resetContext) must be discarded
    handler.onKeyDown(keyW);
    uint64_t staleSession = lastQuerySession;
    auto actReset = handler.resetContext();
    KH_ASSERT(actReset.hideCandidates && handler.composing().empty(), "resetContext failed");
    auto actStale = handler.onAsyncCandidates(staleSession, "w", {"我"});
    KH_ASSERT(!actStale.showCandidates && !actStale.commitText, "Stale async candidates were not discarded");

    // 5. Esc cancels composing without committing
    handler.onKeyDown(keyW);
    KeyInput esc{0x1B, 0, false, false, false, false, false};
    auto actEsc = handler.onKeyDown(esc);
    KH_ASSERT(actEsc.handled && !actEsc.commitText && actEsc.hideCandidates && handler.composing().empty(),
              "Esc should cancel composing without committing");

    // 6. Digit selection 2 -> commits candidate[1]
    handler.onKeyDown(keyW);
    handler.onKeyDown(keyO);
    handler.onAsyncCandidates(lastQuerySession, "wo", {"我", "窝", "wo"});
    KeyInput digit2{'2', '2', false, false, false, false, false};
    auto actDigit = handler.onKeyDown(digit2);
    KH_ASSERT(actDigit.handled && actDigit.commitText && actDigit.textToCommit == "窝",
              "Digit 2 should commit second candidate");
    handler.acknowledgeCommit(actDigit.commitReceipt, true);

    // 7. English candidate Space appends trailing space in mixed mode
    KeyInput keyA{'A', 'A', false, false, false, true, false};
    KeyInput keyI{'I', 'I', false, false, false, true, false};
    handler.onKeyDown(keyA);
    handler.onKeyDown(keyI);
    handler.onAsyncCandidates(lastQuerySession, "AI", {"AI", "爱"});
    auto actEngSpace = handler.onKeyDown(space);
    KH_ASSERT(actEngSpace.commitText && actEngSpace.textToCommit == "AI ",
              "Space on English candidate should append trailing space");
    handler.acknowledgeCommit(actEngSpace.commitReceipt, true);

    // 8. Reference materials, migrated settings, and bounded typing glow.
    {
        using fangcun::SkinPreset;
        const auto& wood = fangcun::sharpTheme(SkinPreset::WoodType);
        KH_ASSERT(wood.light.windowOuterBorder == 0xFF472A16 && wood.light.candidateActiveText == 0xFF9E2F1E,
                  "Wood type uses a walnut frame and red ink");
        const auto& typewriter = fangcun::sharpTheme(SkinPreset::Typewriter);
        KH_ASSERT(typewriter.metrics.candidateRadiusDp == 4.0f && typewriter.light.candidateActiveBg == 0xFF2B2723 &&
                      typewriter.light.accent == 0xFF237218,
                  "Typewriter selection is a dark round-edged stamp on paper with emerald caret");
        const auto& terminal = fangcun::sharpTheme(SkinPreset::Terminal);
        KH_ASSERT(terminal.metrics.useMonospaceCode && terminal.light.accent == 0xFFF4E667 &&
                      terminal.light.candidateActiveBg == 0xFF78F867 && terminal.light.candidateActiveText == 0xFF0B1A0D,
                  "Terminal uses amber monospaced code and a solid green selection");
        KH_ASSERT(wood.metrics.outerBorderWidthPx == 6 && typewriter.metrics.outerBorderWidthPx == 6 &&
                      terminal.metrics.outerBorderWidthPx == 6,
                  "The three reference candidate frames have the same thickness");
        const auto& brutal = fangcun::sharpTheme(SkinPreset::Brutal);
        KH_ASSERT(brutal.metrics.useMonospaceCode && brutal.metrics.hardShadowOffsetPx == 4,
                  "Brutal keeps its monospace code and four pixel hard shadow");

        const std::array presets{SkinPreset::WoodType, SkinPreset::Typewriter, SkinPreset::Terminal, SkinPreset::Brutal};
        const std::array<std::string_view, 4> ids{"wood_type", "typewriter", "terminal", "brutal"};
        const std::array<std::string_view, 4> oldIds{"swiss", "eink", "metro", "deep_blue"};
        const auto contrast = [](fangcun::ColorArgb a, fangcun::ColorArgb b) {
            auto la = a.relativeLuminance();
            auto lb = b.relativeLuminance();
            return (std::max(la, lb) + .05f) / (std::min(la, lb) + .05f);
        };
        for (size_t i = 0; i < presets.size(); ++i) {
            const auto preset = presets[i];
            const auto metrics = fangcun::skinMetrics(preset);
            KH_ASSERT(fangcun::parseSkinPreset(ids[i]) == preset && fangcun::parseSkinPreset(oldIds[i]) == preset,
                      "Stored IDs and their replacements must agree");
            KH_ASSERT(std::string_view(metrics.id) == ids[i], "Skin serialization must preserve the chosen material");
            KH_ASSERT(fangcun::resolveEffectiveDarkMode(fangcun::ThemeMode::Light, preset, true) == false &&
                      fangcun::resolveEffectiveDarkMode(fangcun::ThemeMode::Dark, preset, false) == true,
                      "Explicit OS window modes must work for all four skins");
            KH_ASSERT(fangcun::sharpPalette(preset, false).windowBg != fangcun::sharpPalette(preset, true).windowBg,
                      "Light and dark candidates use different surfaces in every skin");
            for (const auto& glow : metrics.glowColors) {
                KH_ASSERT(glow.a == 255 && (glow.r != glow.g || glow.g != glow.b), "Every skin supplies colorful glow");
            }
            for (bool dark : {false, true}) {
                const auto& screen = fangcun::sharpPalette(preset, dark);
                const auto colors = fangcun::skinColors(preset, dark);
                using fangcun::ColorArgb;
                KH_ASSERT(contrast(ColorArgb::fromHex(screen.candidateActiveText),
                                   ColorArgb::fromHex(screen.candidateActiveBg)) >= 4.5f,
                          "Selected candidates must have readable text");
                KH_ASSERT(contrast(colors.text, colors.keyboard) >= 4.5f &&
                          contrast(colors.onCandidate, colors.pressed) >= 4.5f &&
                          contrast(colors.onAccent, colors.accent) >= 4.5f,
                          "Desktop controls must have readable text");
                const auto label = fangcun::readableSettingsAccent(colors);
                const auto settingsCard = ColorArgb::lerp(colors.keyboard, colors.key,
                    colors.isDarkKeyboard() ? .35f : .55f);
                KH_ASSERT(contrast(label, colors.keyboard) >= 4.5f && contrast(label, settingsCard) >= 4.5f,
                          "Small native-settings labels must be readable on both the plate and translucent cards");
                if (!dark && (preset == SkinPreset::Terminal || preset == SkinPreset::Brutal)) {
                    KH_ASSERT(label.toColorRef() != colors.accent.toColorRef(),
                              "A bright accent fill must not also be used as illegible text on a light settings plate");
                }
                KH_ASSERT(fangcun::accentForeground(colors).toColorRef() == colors.onAccent.toColorRef(),
                          "Making small labels readable must not change emphasized-button foreground colors");
                const auto preview = fangcun::computeGlassKeyColors(preset, colors, .4f, true, true, true);
                KH_ASSERT(preview.cornerRadiusDp == metrics.cornerRadiusDp && preview.top.a == 255 &&
                          preview.ledgeDepthDp == (preset == SkinPreset::Brutal ? 2.0f : 0.0f),
                          "Preview geometry must preserve each material's cap shape");
            }
        }
        KH_ASSERT(fangcun::parseSkinPreset("unknown") == SkinPreset::WoodType,
                  "An invalid skin falls back to wood type");

        fangcun::TypingGlowEngine glow;
        glow.setEnabled(true);
        glow.setMotionAllowed(true);
        for (int i = 0; i < 8; ++i) glow.trigger(false, 0.0f, 0.0f);
        KH_ASSERT(glow.activeCount() == fangcun::TypingGlowEngine::kMaxPulses,
                  "TypingGlow must cap at 6 pulses");
        glow.advanceFrame(1'000'000'000LL);
        glow.advanceFrame(1'050'000'000LL);
        auto circles = glow.buildRenderCircles(360.0f, 68.0f, 1.0f, false, fangcun::skinMetrics(fangcun::SkinPreset::Brutal).glowColors);
        KH_ASSERT(circles.size() == 12, "Each pulse must emit primary + trailing glow circle");
        glow.advanceFrame(1'000'000'000LL + fangcun::TypingGlowEngine::kDurationNanos);
        KH_ASSERT(glow.activeCount() == 0, "Pulses must expire after 1100ms");
    }

    // 9. Candidate pagination (> 9 candidates): =/-/PageDown/PageUp, arrow wrap, and page-relative digit selection
    {
        std::vector<std::string> manyCands;
        for (int i = 0; i < 12; ++i) {
            manyCands.push_back("词" + std::to_string(i));
        }

        handler.onKeyDown(keyW);
        handler.onAsyncCandidates(lastQuerySession, "w", manyCands);
        KH_ASSERT(handler.totalPages() == 2, "12 candidates should have 2 pages");
        KH_ASSERT(handler.currentPage() == 0 && handler.pageStart() == 0, "Initial page should be 0");

        KeyInput pageNext{'=', '=', false, false, false, false, false};
        KeyInput pagePrev{'-', '-', false, false, false, false, false};
        KeyInput vkNext{0x22, 0, false, false, false, false, false};
        KeyInput vkPrior{0x21, 0, false, false, false, false, false};
        KeyInput vkRight{0x27, 0, false, false, false, false, false};
        KeyInput vkLeft{0x25, 0, false, false, false, false, false};

        // '=' flips to page 1 (selectedIndex = 9)
        auto actPage1 = handler.onKeyDown(pageNext);
        KH_ASSERT(actPage1.handled && actPage1.selectedIndex == 9, "= should flip to page 1");
        KH_ASSERT(handler.currentPage() == 1 && handler.pageStart() == 9, "Should be on page 1");

        // '=' on last page stays on page 1 and still consumes key
        auto actPage1Stay = handler.onKeyDown(vkNext);
        KH_ASSERT(actPage1Stay.handled && handler.currentPage() == 1 && handler.selectedIndex() == 9,
                  "PageDown on last page should stay on last page");

        // On page 1 (items 9, 10, 11), '3' is valid and '4' is out of range
        KeyInput digit3{'3', '3', false, false, false, false, false};
        KeyInput digit4{'4', '4', false, false, false, false, false};
        KH_ASSERT(handler.shouldEatKey(digit3), "Digit 3 on page 1 should be eaten");
        KH_ASSERT(!handler.shouldEatKey(digit4), "Digit 4 on page 1 (only 3 items) should pass through");

        // '-' flips back to page 0
        auto actPage0 = handler.onKeyDown(pagePrev);
        KH_ASSERT(actPage0.handled && actPage0.selectedIndex == 0 && handler.currentPage() == 0,
                  "- should flip back to page 0");

        // Arrow right from 8 crosses into page 1 (index 9), arrow left crosses back to 8
        for (int i = 0; i < 8; ++i) handler.onKeyDown(vkRight);
        KH_ASSERT(handler.selectedIndex() == 8 && handler.currentPage() == 0, "Should be at index 8 on page 0");
        handler.onKeyDown(vkRight);
        KH_ASSERT(handler.selectedIndex() == 9 && handler.currentPage() == 1, "Right from 8 should enter page 1");
        handler.onKeyDown(vkLeft);
        KH_ASSERT(handler.selectedIndex() == 8 && handler.currentPage() == 0, "Left from 9 should return to page 0");

        // PageDown then press '2' -> commits manyCands[10] ("词10")
        handler.onKeyDown(vkNext);
        KH_ASSERT(handler.currentPage() == 1, "PageDown should move to page 1");
        auto actCommitP2 = handler.onKeyDown(digit2);
        KH_ASSERT(actCommitP2.commitText && actCommitP2.textToCommit == "词10",
                  "Digit 2 on page 1 should commit candidate[10]");
        handler.acknowledgeCommit(actCommitP2.commitReceipt, true);

        // PageUp on empty composing does nothing; test PageUp from page 1
        handler.onKeyDown(keyW);
        handler.onAsyncCandidates(lastQuerySession, "w", manyCands);
        handler.onKeyDown(vkNext);
        auto actPrior = handler.onKeyDown(vkPrior);
        KH_ASSERT(actPrior.handled && handler.currentPage() == 0 && handler.selectedIndex() == 0,
                  "PageUp should return to page 0");
        handler.resetContext();
    }

    // 7. English mode toggle and CapsLock uppercase passthrough
    {
        ShiftTapTracker tap;
        tap.keyDown(ShiftSide::Left, true, false);
        tap.keyDown(ShiftSide::Left, true, true);
        KH_ASSERT(tap.keyUp(ShiftSide::Left) && !tap.keyUp(ShiftSide::Left),
                  "A held or duplicated TSF callback toggles at most once when released");
        tap.keyDown(ShiftSide::Right, true, false);
        KH_ASSERT(tap.keyUp(ShiftSide::Right), "A solo right Shift can toggle too");

        for (const auto key : {ShiftSide::None, ShiftSide::Right}) {
            tap.keyDown(ShiftSide::Left, true, false);
            tap.keyDown(key, true, false);
            KH_ASSERT(!tap.keyUp(ShiftSide::Left) && !tap.keyUp(key), "A Shift chord must pass through without a toggle");
        }
        tap.keyDown(ShiftSide::Left, false, false);
        KH_ASSERT(!tap.keyUp(ShiftSide::Left), "Disabled or modified Shift must not toggle");
        tap.keyDown(ShiftSide::Left, true, false);
        tap.reset();
        KH_ASSERT(!tap.keyUp(ShiftSide::Left), "A shift release after losing focus must not change modes");

        KeyInput upperA{'A', 'A', false, false, false, false, true};
        KH_ASSERT(!handler.shouldEatKey(upperA), "CapsLock uppercase letter on empty composing should pass through");

        KeyInput shiftA{'A', 'A', false, false, false, true, false};
        KH_ASSERT(handler.shouldEatKey(shiftA), "Shift+A in mixed mode should still compose");
        KH_ASSERT(handler.onKeyDown(shiftA).compositionText == "A", "The Shift+A chord must preserve case");
        handler.resetContext();

        handler.onKeyDown(keyW);
        KH_ASSERT(handler.composing() == "w", "Composing should be 'w'");
        auto toggleAct = handler.toggleEnglishMode();
        KH_ASSERT(handler.englishMode(), "Should now be in English mode");
        KH_ASSERT(toggleAct.commitText && toggleAct.textToCommit == "w",
                  "Toggling to English while composing should commit raw text");
        handler.acknowledgeCommit(toggleAct.commitReceipt, true);
        KH_ASSERT(!handler.shouldEatKey(keyW), "Letters should pass through in English mode");

        handler.toggleEnglishMode();
        KH_ASSERT(!handler.englishMode() && handler.shouldEatKey(keyW),
                  "Toggling back to Chinese mode should resume composing");
        handler.resetContext();
    }

    // Fast typing keeps each accepted boundary ahead of later segments. The
    // fixture controls response order instead of relying on thread timing.
    {
        DeferredInputFixture f;
        f.type('w'); f.type('o'); f.press(0x20);
        f.type('n'); f.type('i'); f.press(0x20);
        f.type('h'); f.type('a'); f.type('o');
        KH_ASSERT(f.queries.size() == 2 && f.handler.displayComposing() == "wo ni hao",
                  "Keys after pending Space must stay in a bounded tail, not replace the old code");
        const auto first = f.reply(1, {"我", "窝"});
        KH_ASSERT(first.textToCommit == "我" && first.compositionText == "ni hao" &&
                      first.updateComposition && f.queries.size() == 3 && f.queries.back().input == "ni",
                  "One transaction must commit the old word and keep both later segments");
        f.accept(first);
        const auto second = f.reply(2, {"你"});
        KH_ASSERT(second.textToCommit == "你" && second.compositionText == "hao" &&
                      f.queries.size() == 4 && f.queries.back().input == "hao",
                  "A second queued boundary must commit in order and query only its following segment");
        f.accept(second);
        f.reply(3, {"好"});
        const auto third = f.press(0x20);
        f.accept(third);
        KH_ASSERT(first.textToCommit + second.textToCommit + third.textToCommit == "我你好" &&
                      (f.learned == std::vector<std::string>{"我", "你", "好"}) && f.handler.composing().empty(),
                  "Several rapid boundaries must produce the complete sentence and learn only accepted words");
    }
    {
        DeferredInputFixture f;
        f.type('w'); f.type('o'); f.type(','); f.type('n'); f.type('i'); f.type('!');
        const auto first = f.reply(1, {"我"});
        KH_ASSERT(first.textToCommit == "我，" && first.compositionText == "ni！",
                  "Pending punctuation must follow its own word and leave the next punctuation with its segment");
        f.accept(first);
        const auto second = f.reply(2, {"你"});
        KH_ASSERT(second.textToCommit == "你！" && second.hideCandidates,
                  "Punctuation may not replace the word already in the editor composition");
        f.accept(second);

        f.handler.setChinesePunctuation(false);
        f.type('w'); f.type('o'); f.type(',');
        const auto ascii = f.reply(f.queries.size() - 1, {"我"});
        KH_ASSERT(ascii.textToCommit == "我,", "ASCII punctuation mode must still commit the preceding word");
        f.accept(ascii);
        f.type('w'); f.type('o');
        f.reply(f.queries.size() - 1, {"我"});
        const auto at = f.type('@');
        KH_ASSERT(at.textToCommit == "我@", "An unmapped printable symbol must follow rather than overwrite the word");
        f.accept(at);
    }
    {
        DeferredInputFixture f;
        f.type('w'); f.type('o'); f.press(0x20); f.type('@'); f.type('0'); f.type('n'); f.type('i');
        const auto action = f.reply(1, {"我"});
        KH_ASSERT(action.textToCommit == "我@0" && action.compositionText == "ni",
                  "Literal symbols and zero after a queued boundary must retain their order");
        f.accept(action);
    }
    {
        DeferredInputFixture f;
        f.type('w'); f.type('o'); f.press(0x20); f.type('n'); f.type('i'); f.press(0x0D);
        f.type('h'); f.type('a'); f.type('o'); f.press(0x20);
        const auto action = f.reply(1, {"我"});
        KH_ASSERT(action.textToCommit == "我ni" && action.compositionText == "hao " &&
                      f.queries.size() == 3 && f.queries.back().input == "hao",
                  "Queued Enter must commit its raw segment and avoid submitting a superseded intermediate query");
        f.accept(action);
        const auto next = f.reply(2, {"好"});
        KH_ASSERT(next.textToCommit == "好", "A later Space must still resolve after queued raw Enter");
        f.accept(next);
        KH_ASSERT((f.learned == std::vector<std::string>{"我", "好"}), "Raw code must never be learned as a Chinese candidate");
    }
    {
        DeferredInputFixture f;
        f.type('w'); f.type('o'); f.press(0x20); f.type('n'); f.type('i');
        KH_ASSERT(f.press(0x08).compositionText == "wo n" && f.press(0x08).compositionText == "wo ",
                  "Backspace must edit the deferred tail immediately");
        KH_ASSERT(f.press(0x08).compositionText == "w" && f.handler.composing() == "w",
                  "Backspace crossing an unresolved boundary must edit the old code without accepting it");
        const auto stale = f.reply(1, {"我"});
        KH_ASSERT(!stale.commitText && !stale.showCandidates, "A response for the erased old code must be ignored");
        const auto cancel = f.press(0x1B);
        KH_ASSERT(cancel.hideCandidates && f.handler.composing().empty(), "Escape must cancel the old segment and entire tail");
        const auto afterCancel = f.reply(f.queries.size() - 1, {"文"});
        KH_ASSERT(!afterCancel.commitText && !afterCancel.showCandidates, "Focus/cancel generation must invalidate deferred results");
    }
    {
        DeferredInputFixture f;
        f.type('w'); f.type('o'); f.press(0x20);
        for (int i = 0; i < 128; ++i) {
            KH_ASSERT(!f.type('a').inputLimitReached, "All 128 deferred keys must fit");
        }
        const auto overflow = f.type('b');
        KH_ASSERT(overflow.handled && overflow.inputLimitReached && f.handler.displayComposing().size() == 131,
                  "A full tail must explicitly report its limit while preserving all earlier keys");
        f.press(0x08);
        KH_ASSERT(!f.type('b').inputLimitReached && f.handler.displayComposing().back() == 'b',
                  "Backspace must free a deferred slot");
        f.handler.resetContext();
        for (int i = 0; i < 128; ++i) f.type('a');
        const auto codeOverflow = f.type('b');
        KH_ASSERT(codeOverflow.handled && codeOverflow.inputLimitReached && f.handler.composing().size() == 128,
                  "The existing 128-code engine limit must remain bounded and visible to the adapter");
    }
    {
        DeferredInputFixture f;
        f.type('w'); f.type('o'); f.press(0x20); f.type('n'); f.type('i');
        const auto proposed = f.reply(1, {"我", "窝"});
        const auto refused = f.handler.acknowledgeCommit(proposed.commitReceipt, false);
        KH_ASSERT(refused.compositionText == "wo ni" && !refused.commitText && f.learned.empty() &&
                      f.handler.composing() == "wo" && f.queries.size() == 3,
                  "A refused commit must restore the exact old code and tail without learning or automatic retry");
        const auto oldTailResult = f.reply(2, {"你"});
        KH_ASSERT(!oldTailResult.showCandidates && !oldTailResult.commitText,
                  "A canceled tail query must not overwrite a restored rejected prefix");
        const auto retry = f.press(0x20);
        KH_ASSERT(retry.textToCommit == "我" && retry.compositionText == "ni" &&
                      retry.commitReceipt != proposed.commitReceipt,
                  "Explicit Space must retry the rejected boundary while keeping later input");
        f.accept(retry);
        f.accept(retry);
        const auto staleAck = f.handler.acknowledgeCommit(proposed.commitReceipt, false);
        KH_ASSERT(!staleAck.handled && f.learned.size() == 1 && f.handler.composing() == "ni",
                  "Duplicate/stale receipts must neither learn twice nor restore over later input");
        f.reply(f.queries.size() - 1, {"你"});
        const auto next = f.press(0x20);
        f.accept(next);
        KH_ASSERT((f.learned == std::vector<std::string>{"我", "你"}), "Accepted retry and tail must each learn once");
    }
    {
        DeferredInputFixture f;
        f.type('w'); f.type('o'); f.press(0x20); f.type('n'); f.type('i'); f.press(0x20);
        const auto proposed = f.reply(1, {"我"});
        const auto partial = f.accept(proposed, false);
        KH_ASSERT(partial.updateComposition && !partial.commitText && partial.compositionText == "ni " &&
                      f.handler.composing() == "ni" && (f.learned == std::vector<std::string>{"我"}),
                  "If prefix commit succeeds but followup fails, recover only the tail and never resend the prefix");
        const auto tail = f.reply(2, {"你"});
        KH_ASSERT(tail.textToCommit == "你", "Only the remaining tail may be committed after a partial editor failure");
        f.accept(tail);
        KH_ASSERT((f.learned == std::vector<std::string>{"我", "你"}), "Partial success must not lose accepted-prefix learning");
    }
    {
        DeferredInputFixture f;
        std::vector<std::string> words;
        for (int i = 0; i < 12; ++i) words.push_back("词" + std::to_string(i));
        f.type('w'); f.reply(0, words); f.handler.movePage(1);
        f.type('o'); f.handler.movePage(1); f.type('2');
        const auto pendingDigit = f.reply(1, words);
        KH_ASSERT(pendingDigit.textToCommit == "词10", "Queued digit selection must remember its requested page");
        const auto rejected = f.handler.acknowledgeCommit(pendingDigit.commitReceipt, false);
        KH_ASSERT(rejected.compositionText == "wo" && rejected.selectedIndex == 0 && f.learned.empty(),
                  "Rejected pending digit must retain its original draft");
        const auto clickedRetry = f.handler.selectCandidateIndex(0);
        KH_ASSERT(clickedRetry.textToCommit == "词0", "Clicking a different retry candidate must replace the failed digit selection");
        f.accept(clickedRetry);

        f.type('w'); f.reply(f.queries.size() - 1, words); f.handler.movePage(1);
        const auto directDigit = f.type('2');
        f.handler.acknowledgeCommit(directDigit.commitReceipt, false);
        KH_ASSERT(f.handler.selectedIndex() == 10, "A direct commit refusal must preserve the chosen candidate and page");
        const auto directRetry = f.press(0x20);
        KH_ASSERT(directRetry.textToCommit == "词10", "A refused direct selection must not silently change the chosen word");
        f.accept(directRetry);
    }
    {
        DeferredInputFixture f;
        f.type('w');
        const auto toggle = f.handler.toggleEnglishMode();
        const auto restored = f.handler.acknowledgeCommit(toggle.commitReceipt, false);
        KH_ASSERT(!f.handler.englishMode() && restored.compositionText == "w" && f.learned.empty(),
                  "A rejected Shift raw commit must restore both the draft and Chinese mode");
        const auto retry = f.handler.toggleEnglishMode();
        f.accept(retry);
        KH_ASSERT(f.handler.englishMode() && f.handler.composing().empty(), "Accepted Shift retry must switch mode once");
        f.handler.setEnglishMode(false);
        f.type('w'); f.reply(f.queries.size() - 1, {"我"});
        const auto commit = f.press(0x20);
        f.handler.resetContext();
        const auto ignored = f.handler.acknowledgeCommit(commit.commitReceipt, false);
        KH_ASSERT(!ignored.handled && f.handler.composing().empty() && f.learned.empty(),
                  "A receipt after losing focus must not restore or learn into another editor");
        f.type('a');
        f.accept(commit);
        KH_ASSERT(f.handler.composing() == "a" && f.learned.empty(), "Late success must not affect a newer input session");
    }
    {
        DeferredInputFixture f;
        const auto punctuation = f.type('.');
        const auto rejected = f.handler.acknowledgeCommit(punctuation.commitReceipt, false);
        KH_ASSERT(rejected.compositionText == "。", "Failed direct punctuation must remain available as a recoverable draft");
        const auto erased = f.press(0x08);
        KH_ASSERT(erased.compositionText.empty() && erased.hideCandidates,
                  "Backspace must remove one complete UTF-8 code point from a recovered literal");
        f.handler.setPersonalizedLearning(false);
        f.type('w'); f.reply(f.queries.size() - 1, {"我"});
        f.accept(f.press(0x20));
        KH_ASSERT(f.learned.empty(), "Host acknowledgement must still honor disabled personalization");
    }

    return 0;
}
