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
    }

    return 0;
}
