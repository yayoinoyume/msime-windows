#include "tests/includes/test_framework.h"
#include "window/candidate_placement.h"

#include <cmath>

// 光标下方放不放得下候选卡片，决定候选窗往下开还是翻到上方并倒序排列。这个判定与
// AdjustCandidateWindowPosition 共用同一份实现，所以边界必须和它一致：文字底边下方
// 3 DIP 间隙、与屏幕底边 2 DIP 边距。
TEST_CASE(candidate_card_fits_below_until_it_touches_the_bottom_edge_pad)
{
    for (const FLOAT scale : {1.0f, 1.25f, 1.5f, 2.0f})
    {
        const MonitorCoordinates monitor{0, 0, 1920, 1080};
        const POINT caret{400, 900};
        const int edgePadPx = static_cast<int>(std::lround(2.0 * scale));
        const int roomPx = monitor.bottom - edgePadPx - (caret.y + FanyImeUi::CandidateGapBelowCaretPx(scale));
        const auto heightDip = [scale](int px) { return static_cast<double>(px) / scale; };
        // 留 2px 余量避开浮点取整，仍然是对着同一条边界在测。
        REQUIRE(FanyImeUi::CandidateFitsBelowCaret(caret, heightDip(roomPx - 2), scale, monitor));
        REQUIRE(!FanyImeUi::CandidateFitsBelowCaret(caret, heightDip(roomPx + 2), scale, monitor));
    }
}

TEST_CASE(candidate_card_flips_up_only_when_it_would_cross_the_bottom_edge)
{
    const MonitorCoordinates monitor{0, 0, 1920, 1080};
    // 屏幕中段放得下很高的列表；贴近底边时只有很矮的卡片放得下。
    REQUIRE(FanyImeUi::CandidateFitsBelowCaret({400, 500}, 500.0, 1.0f, monitor));
    REQUIRE(!FanyImeUi::CandidateFitsBelowCaret({400, 1000}, 500.0, 1.0f, monitor));
    REQUIRE(FanyImeUi::CandidateFitsBelowCaret({400, 1000}, 20.0, 1.0f, monitor));
}
