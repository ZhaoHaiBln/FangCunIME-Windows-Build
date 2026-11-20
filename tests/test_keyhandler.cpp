#include "KeyHandler.hpp"
#include "fangcun/CandidateCellLayout.hpp"
#include "fangcun/ThemeConfig.hpp"

#include <iostream>
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

    // 7. English candidate Space appends trailing space in mixed mode
    KeyInput keyA{'A', 'A', false, false, false, true, false};
    KeyInput keyI{'I', 'I', false, false, false, true, false};
    handler.onKeyDown(keyA);
    handler.onKeyDown(keyI);
    handler.onAsyncCandidates(lastQuerySession, "AI", {"AI", "爱"});
    auto actEngSpace = handler.onKeyDown(space);
    KH_ASSERT(actEngSpace.commitText && actEngSpace.textToCommit == "AI ",
              "Space on English candidate should append trailing space");

    // 8. Reference materials, migrated settings, and bounded typing glow.
    {
        using fangcun::SkinPreset;
        const auto& wood = fangcun::sharpTheme(SkinPreset::WoodType);
        KH_ASSERT(wood.light.windowOuterBorder == 0xFF493629 && wood.light.candidateActiveText == 0xFF8B1A10,
                  "Wood type uses a walnut frame and red ink");
        const auto& typewriter = fangcun::sharpTheme(SkinPreset::Typewriter);
        KH_ASSERT(typewriter.metrics.candidateRadiusDp == 6 && typewriter.light.candidateActiveBg == 0xFF29251D,
                  "Typewriter selection is a dark round-edged stamp on paper");
        const auto& terminal = fangcun::sharpTheme(SkinPreset::Terminal);
        KH_ASSERT(terminal.metrics.useMonospaceCode && terminal.light.textPrimary == 0xFFA5F492,
                  "Terminal uses green screen text and monospaced code");
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
        KH_ASSERT(!handler.shouldEatKey(keyW), "Letters should pass through in English mode");

        handler.toggleEnglishMode();
        KH_ASSERT(!handler.englishMode() && handler.shouldEatKey(keyW),
                  "Toggling back to Chinese mode should resume composing");
        handler.resetContext();
    }

    return 0;
}
