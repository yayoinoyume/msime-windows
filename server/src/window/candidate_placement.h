#pragma once

#include "defines/base_structures.h"

#include <cmath>
#include <windows.h>

namespace FanyImeUi
{
// 候选卡片贴在光标下方时的落点：与 AdjustCandidateWindowPosition 共用同一套设计间距
// （3 DIP 间隙、2 DIP 边距），所以下面放不放得下的判定与实际定位不会各说一套。
inline int CandidateGapBelowCaretPx(FLOAT scale)
{
    return static_cast<int>(std::lround(3.0 * static_cast<double>(scale)));
}

inline int CandidateEdgePadPx(FLOAT scale)
{
    return static_cast<int>(std::lround(2.0 * static_cast<double>(scale)));
}

// `coordinates` 是光标所在显示器；`heightDip` 是判翻面用的卡片高度（竖排是见过的最高列表）。
// 全用物理像素比较，和 AdjustCandidateWindowPosition 的边界条件一字不差。
inline bool CandidateFitsBelowCaret(const POINT &caret, double heightDip, FLOAT scale,
                                    const MonitorCoordinates &coordinates)
{
    const int edgePadPx = CandidateEdgePadPx(scale);
    // POINT::y 是 LONG，与 int 混用 std::max 没有可推导的重载，统一收成 int 再比。
    const int caretY = static_cast<int>(caret.y);
    const int belowPx = caretY + CandidateGapBelowCaretPx(scale);
    const int topPx = belowPx > coordinates.top + edgePadPx ? belowPx : coordinates.top + edgePadPx;
    const int heightPx = static_cast<int>(std::ceil(heightDip * static_cast<double>(scale)));
    return topPx + heightPx <= coordinates.bottom - edgePadPx;
}
} // namespace FanyImeUi
