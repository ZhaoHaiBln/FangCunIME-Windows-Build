#include "KeyHandler.hpp"
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

    // 8. Android 1.5.1 Theme tokens, GlassSurface math & TypingGlowEngine
    {
        auto wgMetrics = fangcun::skinMetrics(fangcun::SkinPreset::WarmGrid);
        KH_ASSERT(wgMetrics.cornerRadiusDp == 2.0f && wgMetrics.ledgeDepthDp == 1.8f,
                  "WarmGrid corner/ledge metrics mismatch");
        auto nlMetrics = fangcun::skinMetrics(fangcun::SkinPreset::NativeLight);
        KH_ASSERT(nlMetrics.cornerRadiusDp == 4.0f && nlMetrics.ledgeDepthDp == 2.1f,
                  "NativeLight corner/ledge metrics mismatch");

        auto nlLight = fangcun::skinColors(fangcun::SkinPreset::NativeLight, false);
        auto nlDark = fangcun::skinColors(fangcun::SkinPreset::NativeLight, true);
        KH_ASSERT(!nlLight.isDarkKeyboard() && nlDark.isDarkKeyboard(),
                  "NativeLight luminance detection mismatch");

        auto glassNormal = fangcun::computeGlassKeyColors(fangcun::SkinPreset::NativeLight, nlLight, 0.82f, false);
        KH_ASSERT(glassNormal.top.a == 209 && glassNormal.depressionDp == 0.0f,
                  "Glass key normal opacity/depression mismatch");
        auto glassPressed = fangcun::computeGlassKeyColors(fangcun::SkinPreset::NativeLight, nlLight, 0.82f, true);
        KH_ASSERT(glassPressed.depressionDp == 0.8f && glassPressed.shadowElevationDp == 0.25f,
                  "Glass key pressed depression/shadow mismatch");
        auto glassEmph = fangcun::computeGlassKeyColors(fangcun::SkinPreset::NativeLight, nlLight, 0.5f, false, false, true);
        KH_ASSERT(glassEmph.top.a == 255, "Emphasized key must remain opaque");

        auto wgBackdrop = fangcun::computeBackdropGradient(fangcun::SkinPreset::WarmGrid,
            fangcun::skinColors(fangcun::SkinPreset::WarmGrid, false));
        KH_ASSERT(wgBackdrop.isFlat, "WarmGrid backdrop must be flat");
        auto nlBackdrop = fangcun::computeBackdropGradient(fangcun::SkinPreset::NativeLight, nlLight);
        KH_ASSERT(!nlBackdrop.isFlat, "NativeLight backdrop must use 3-stop gradient");

        fangcun::TypingGlowEngine glow;
        glow.setEnabled(true);
        glow.setMotionAllowed(true);
        for (int i = 0; i < 8; ++i) glow.trigger(false, 0.0f, 0.0f);
        KH_ASSERT(glow.activeCount() == fangcun::TypingGlowEngine::kMaxPulses,
                  "TypingGlow must cap at 6 pulses");
        glow.advanceFrame(1'000'000'000LL);
        glow.advanceFrame(1'050'000'000LL);
        auto circles = glow.buildRenderCircles(360.0f, 68.0f, 1.0f, false, nlMetrics.glowColors);
        KH_ASSERT(circles.size() == 12, "Each pulse must emit primary + trailing glow circle");
        glow.advanceFrame(1'000'000'000LL + fangcun::TypingGlowEngine::kDurationNanos);
        KH_ASSERT(glow.activeCount() == 0, "Pulses must expire after 1100ms");

        auto gtMetrics = fangcun::skinMetrics(fangcun::SkinPreset::GreenTerminal);
        auto wtMetrics = fangcun::skinMetrics(fangcun::SkinPreset::WoodType);
        auto ctMetrics = fangcun::skinMetrics(fangcun::SkinPreset::ClassicTypewriter);
        KH_ASSERT(std::string(gtMetrics.id) == "green_terminal" && gtMetrics.defaultThemeMode == fangcun::ThemeMode::Dark,
                  "GreenTerminal metrics mismatch");
        KH_ASSERT(std::string(wtMetrics.id) == "wood_type" && wtMetrics.defaultThemeMode == fangcun::ThemeMode::Light,
                  "WoodType metrics mismatch");
        KH_ASSERT(std::string(ctMetrics.id) == "classic_typewriter" && ctMetrics.defaultThemeMode == fangcun::ThemeMode::Light,
                  "ClassicTypewriter metrics mismatch");

        auto gtColors = fangcun::skinColors(fangcun::SkinPreset::GreenTerminal, true);
        auto wtColors = fangcun::skinColors(fangcun::SkinPreset::WoodType, false);
        auto ctColors = fangcun::skinColors(fangcun::SkinPreset::ClassicTypewriter, false);
        KH_ASSERT(gtColors.isDarkKeyboard() && gtColors.accent.r == 0x78 && gtColors.accent.g == 0xF8,
                  "GreenTerminal phosphor palette mismatch");
        KH_ASSERT(!wtColors.isDarkKeyboard() && wtColors.accent.r == 0xB5 && wtColors.border.r == 0x68,
                  "WoodType walnut/vermilion palette mismatch");
        KH_ASSERT(!ctColors.isDarkKeyboard() && ctColors.accent.r == 0x2B && ctColors.keyboard.r == 0xF6,
                  "ClassicTypewriter paper/charcoal palette mismatch");
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
        KeyInput upperA{'A', 'A', false, false, false, false, true};
        KH_ASSERT(!handler.shouldEatKey(upperA), "CapsLock uppercase letter on empty composing should pass through");

        KeyInput shiftA{'A', 'A', false, false, false, true, false};
        KH_ASSERT(handler.shouldEatKey(shiftA), "Shift+A in mixed mode should still compose");

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
