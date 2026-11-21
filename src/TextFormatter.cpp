#include "TextFormatter.h"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <numeric>
#include <sstream>
#include <unordered_set>

namespace ScreenTextify {

namespace {

struct VisualLine {
    std::vector<const OcrWordBox*> words;
    float minY = 0.0f;
    float maxY = 0.0f;
    float centerY = 0.0f;
    float leftX = 0.0f;
    float height = 0.0f;

    void recomputeMetrics() {
        if (words.empty()) return;
        std::sort(words.begin(), words.end(), [](const OcrWordBox* a, const OcrWordBox* b) {
            return a->bounds.x < b->bounds.x;
        });
        minY = 1e9f;
        maxY = -1e9f;
        float sumCy = 0.0f;
        for (const auto* w : words) {
            minY = std::min(minY, w->bounds.y);
            maxY = std::max(maxY, w->bounds.bottom());
            sumCy += w->bounds.centerY();
        }
        centerY = sumCy / static_cast<float>(words.size());
        leftX = words.front()->bounds.x;
        height = std::max(1.0f, maxY - minY);
    }
};

// Parses a pure positive editor line number (1..99999) with no punctuation/letters.
bool TryParseLineNumber(const std::wstring& text, int& outNum) {
    if (text.empty() || text.size() > 5) return false;
    int val = 0;
    for (wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') return false;
        val = val * 10 + (ch - L'0');
    }
    if (val <= 0) return false;
    outNum = val;
    return true;
}

bool IsMathOrListSecondToken(const std::wstring& tok) {
    if (tok.empty()) return false;
    wchar_t c = tok.front();
    return c == L'+' || c == L'-' || c == L'*' || c == L'/' || c == L'=' || c == L'<' || c == L'>' || c == L',' || c == L']';
}

// Identifies editor line-number gutter tokens across clustered visual lines.
// Returns the set of OcrWordBox pointers that belong to the left line-number gutter.
std::unordered_set<const OcrWordBox*> DetectLineNumberGutterWords(
    const std::vector<VisualLine>& visualLines,
    float medianCharWidth
) {
    std::unordered_set<const OcrWordBox*> gutterSet;
    if (visualLines.size() < 2) return gutterSet;

    struct Cand {
        size_t lineIdx;
        int num;
        const OcrWordBox* word;
        float leftX;
        float rightX;
        float gapToCode; // -1 if blank numbered line
        bool hasCodeToRight;
        bool secondIsMath;
    };

    std::vector<Cand> cands;
    cands.reserve(visualLines.size());

    for (size_t i = 0; i < visualLines.size(); ++i) {
        const auto& vl = visualLines[i];
        if (vl.words.empty()) continue;
        const OcrWordBox* w0 = vl.words.front();
        int num = 0;
        if (!TryParseLineNumber(w0->text, num)) continue;

        Cand c{};
        c.lineIdx = i;
        c.num = num;
        c.word = w0;
        c.leftX = w0->bounds.x;
        c.rightX = w0->bounds.right();
        c.hasCodeToRight = (vl.words.size() >= 2);
        c.gapToCode = c.hasCodeToRight ? (vl.words[1]->bounds.x - w0->bounds.right()) : -1.0f;
        c.secondIsMath = c.hasCodeToRight && IsMathOrListSecondToken(vl.words[1]->text);
        cands.push_back(c);
    }

    if (cands.size() < 2) return gutterSet;

    // 1. Check column alignment: right-aligned (VS Code / JetBrains / GitHub / Vim) OR left-aligned
    std::vector<float> rights, lefts;
    rights.reserve(cands.size());
    lefts.reserve(cands.size());
    for (const auto& c : cands) {
        rights.push_back(c.rightX);
        lefts.push_back(c.leftX);
    }
    std::sort(rights.begin(), rights.end());
    std::sort(lefts.begin(), lefts.end());
    float medRight = rights[rights.size() / 2];
    float medLeft = lefts[lefts.size() / 2];
    float alignTol = std::max(8.0f, medianCharWidth * 1.45f);

    std::vector<Cand> rightInliers, leftInliers;
    for (const auto& c : cands) {
        if (std::abs(c.rightX - medRight) <= alignTol) rightInliers.push_back(c);
        if (std::abs(c.leftX - medLeft) <= alignTol) leftInliers.push_back(c);
    }
    const auto& aligned = (rightInliers.size() >= leftInliers.size()) ? rightInliers : leftInliers;

    // Require >= 3 aligned line numbers, OR exactly 2 consecutive lines (e.g. 2-line crop) both with code to the right
    bool validCount = (aligned.size() >= 3 && aligned.size() * 10 >= visualLines.size() * 6) ||
                      (visualLines.size() == 2 && aligned.size() == 2 && aligned[0].hasCodeToRight && aligned[1].hasCodeToRight);
    if (!validCount) return gutterSet;

    // 2. Check strictly increasing near-consecutive sequence (1 <= delta <= 4, tolerates wrapped/blank lines)
    int goodStepPairs = 0;
    int totalPairs = static_cast<int>(aligned.size()) - 1;
    for (size_t i = 1; i < aligned.size(); ++i) {
        int delta = aligned[i].num - aligned[i - 1].num;
        int lineStep = static_cast<int>(aligned[i].lineIdx - aligned[i - 1].lineIdx);
        if (delta >= 1 && delta <= std::max(4, lineStep * 3)) {
            ++goodStepPairs;
        }
    }
    if (totalPairs == 1) {
        if (aligned[1].num - aligned[0].num != 1) return gutterSet;
    } else if (goodStepPairs * 100 < totalPairs * 80) {
        return gutterSet;
    }

    // 3. Verify clean vertical whitespace lane between gutter column and code column
    float gutterRightMax = -1e9f;
    std::vector<float> gaps;
    int codeLines = 0;
    int mathSecondLines = 0;
    for (const auto& c : aligned) {
        gutterRightMax = std::max(gutterRightMax, c.rightX);
        if (c.hasCodeToRight) {
            ++codeLines;
            gaps.push_back(c.gapToCode);
            if (c.secondIsMath) ++mathSecondLines;
        }
    }
    if (codeLines == 0) return gutterSet;
    // Reject arithmetic expressions like "1 + 2 / 2 + 3 / 3 + 4"
    if (mathSecondLines * 2 >= codeLines) return gutterSet;

    std::sort(gaps.begin(), gaps.end());
    float medGap = gaps[gaps.size() / 2];
    if (medGap < medianCharWidth * 1.35f) return gutterSet;

    // Ensure non-gutter code tokens across ALL lines lie cleanly to the right of the gutter column
    int cleanCodeLines = 0;
    int totalCodeLines = 0;
    for (size_t i = 0; i < visualLines.size(); ++i) {
        const auto& vl = visualLines[i];
        bool isAlignedGutterLine = false;
        for (const auto& c : aligned) {
            if (c.lineIdx == i) { isAlignedGutterLine = true; break; }
        }
        size_t startW = isAlignedGutterLine ? 1 : 0;
        if (startW < vl.words.size()) {
            ++totalCodeLines;
            if (vl.words[startW]->bounds.x >= gutterRightMax + medianCharWidth * 0.35f) {
                ++cleanCodeLines;
            }
        }
    }
    if (totalCodeLines == 0 || cleanCodeLines * 10 < totalCodeLines * 8) {
        return gutterSet;
    }

    // Confirmed! Mark all aligned gutter tokens
    for (const auto& c : aligned) {
        gutterSet.insert(c.word);
    }

    // Also catch any single OCR-corrupted gutter token (e.g. "l2" between "11" and "13") strictly inside the gutter X-lane
    for (size_t i = 0; i < visualLines.size(); ++i) {
        const auto& vl = visualLines[i];
        if (vl.words.empty()) continue;
        const OcrWordBox* w0 = vl.words.front();
        if (gutterSet.count(w0)) continue;
        if (w0->text.size() <= 5 &&
            w0->bounds.right() <= gutterRightMax + medianCharWidth * 0.55f &&
            (vl.words.size() == 1 || vl.words[1]->bounds.x >= gutterRightMax + medianCharWidth * 0.85f)) {
            gutterSet.insert(w0);
        }
    }

    return gutterSet;
}

void BuildVisualLines(
    const std::vector<const OcrWordBox*>& words,
    std::vector<VisualLine>& outLines,
    float& outMedianCharWidth,
    float& outMedianWordHeight
) {
    outLines.clear();
    outMedianCharWidth = 10.0f;
    outMedianWordHeight = 18.0f;
    if (words.empty()) return;

    std::vector<float> charWidths;
    std::vector<float> wordHeights;
    charWidths.reserve(words.size());
    wordHeights.reserve(words.size());

    for (const auto* w : words) {
        if (!w->text.empty() && w->bounds.width > 1.0f) {
            if (w->text.length() >= 2) {
                charWidths.push_back(w->bounds.width / static_cast<float>(w->text.length()));
            }
        }
        if (w->bounds.height > 1.0f) {
            wordHeights.push_back(w->bounds.height);
        }
    }
    if (charWidths.empty()) {
        for (const auto* w : words) {
            if (!w->text.empty() && w->bounds.width > 1.0f) {
                charWidths.push_back(w->bounds.width / static_cast<float>(w->text.length()));
            }
        }
    }

    if (!charWidths.empty()) {
        std::sort(charWidths.begin(), charWidths.end());
        outMedianCharWidth = std::max(3.0f, charWidths[charWidths.size() / 2]);
    }
    if (!wordHeights.empty()) {
        std::sort(wordHeights.begin(), wordHeights.end());
        outMedianWordHeight = std::max(6.0f, wordHeights[wordHeights.size() / 2]);
    }

    std::vector<const OcrWordBox*> sorted = words;
    std::sort(sorted.begin(), sorted.end(), [](const OcrWordBox* a, const OcrWordBox* b) {
        return a->bounds.centerY() < b->bounds.centerY();
    });

    float lineClusterThreshold = outMedianWordHeight * 0.58f;
    for (const auto* w : sorted) {
        bool placed = false;
        for (auto& vline : outLines) {
            float overlapY = std::min(w->bounds.bottom(), vline.maxY) - std::max(w->bounds.y, vline.minY);
            float minH = std::min(w->bounds.height, vline.height);
            if (std::abs(w->bounds.centerY() - vline.centerY) <= lineClusterThreshold ||
                (minH > 1.0f && overlapY >= minH * 0.50f)) {
                vline.words.push_back(w);
                vline.recomputeMetrics();
                placed = true;
                break;
            }
        }
        if (!placed) {
            VisualLine newLine;
            newLine.words.push_back(w);
            newLine.recomputeMetrics();
            outLines.push_back(std::move(newLine));
        }
    }

    for (size_t i = 0; i < outLines.size(); ++i) {
        for (size_t j = i + 1; j < outLines.size(); ) {
            float overlapY = std::min(outLines[i].maxY, outLines[j].maxY) - std::max(outLines[i].minY, outLines[j].minY);
            float minH = std::min(outLines[i].height, outLines[j].height);
            if (std::abs(outLines[i].centerY - outLines[j].centerY) <= lineClusterThreshold ||
                (minH > 1.0f && overlapY >= minH * 0.50f)) {
                for (const auto* w : outLines[j].words) {
                    outLines[i].words.push_back(w);
                }
                outLines[i].recomputeMetrics();
                outLines.erase(outLines.begin() + j);
            } else {
                ++j;
            }
        }
    }

    std::sort(outLines.begin(), outLines.end(), [](const VisualLine& a, const VisualLine& b) {
        return a.centerY < b.centerY;
    });
}

} // namespace

size_t TextFormatter::MarkLineNumberGutter(OcrDocument& doc) {
    std::vector<const OcrWordBox*> allWords;
    for (auto& line : doc.lines) {
        for (auto& word : line.words) {
            word.isLineNumber = false;
            allWords.push_back(&word);
        }
    }
    if (allWords.size() < 2) return 0;

    std::vector<VisualLine> visualLines;
    float medCharW = 10.0f, medWordH = 18.0f;
    BuildVisualLines(allWords, visualLines, medCharW, medWordH);

    auto gutterSet = DetectLineNumberGutterWords(visualLines, medCharW);
    if (gutterSet.empty()) return 0;

    size_t marked = 0;
    for (auto& line : doc.lines) {
        for (auto& word : line.words) {
            if (gutterSet.count(&word)) {
                word.isLineNumber = true;
                ++marked;
            }
        }
    }
    return marked;
}

std::wstring TextFormatter::FormatDocument(
    const OcrDocument& doc,
    bool onlySelected,
    bool preserveIndentation,
    size_t* outWordCount,
    bool stripLineNumbers
) {
    std::vector<const OcrWordBox*> activeWords;
    for (const auto& line : doc.lines) {
        for (const auto& word : line.words) {
            if (!onlySelected || word.selected) {
                activeWords.push_back(&word);
            }
        }
    }

    if (activeWords.empty()) {
        if (outWordCount) *outWordCount = 0;
        return L"";
    }

    std::vector<VisualLine> visualLines;
    float medianCharWidth = 10.0f;
    float medianWordHeight = 18.0f;
    BuildVisualLines(activeWords, visualLines, medianCharWidth, medianWordHeight);

    // Strip auto-detected editor line-number gutter tokens (both pre-marked and freshly detected)
    std::unordered_set<const OcrWordBox*> gutterSet;
    if (stripLineNumbers) {
        for (const auto* w : activeWords) {
            if (w->isLineNumber) gutterSet.insert(w);
        }
        if (gutterSet.empty()) {
            gutterSet = DetectLineNumberGutterWords(visualLines, medianCharWidth);
        }
        if (!gutterSet.empty()) {
            for (auto& vl : visualLines) {
                vl.words.erase(
                    std::remove_if(vl.words.begin(), vl.words.end(), [&](const OcrWordBox* w) {
                        return gutterSet.count(w) > 0;
                    }),
                    vl.words.end()
                );
                if (!vl.words.empty()) {
                    vl.recomputeMetrics();
                }
            }
            // Trim leading/trailing empty lines left by blank gutter numbers
            while (!visualLines.empty() && visualLines.front().words.empty()) visualLines.erase(visualLines.begin());
            while (!visualLines.empty() && visualLines.back().words.empty()) visualLines.pop_back();
        }
    }

    size_t finalWordCount = 0;
    for (const auto& vl : visualLines) finalWordCount += vl.words.size();
    if (outWordCount) *outWordCount = finalWordCount;
    if (finalWordCount == 0) return L"";

    // Find minimum left X coordinate across all remaining non-empty visual lines
    float minLeftX = 1e9f;
    size_t nonEmptyLineCount = 0;
    for (const auto& vline : visualLines) {
        if (!vline.words.empty()) {
            minLeftX = std::min(minLeftX, vline.leftX);
            ++nonEmptyLineCount;
        }
    }

    std::wstring output;
    for (size_t i = 0; i < visualLines.size(); ++i) {
        const auto& vline = visualLines[i];

        if (i > 0) {
            output += L"\r\n";
            // If preserveIndentation is enabled and there is a significant vertical gap (and previous line wasn't already an explicit blank gutter line)
            if (preserveIndentation && !vline.words.empty() && !visualLines[i - 1].words.empty()) {
                float prevBottom = visualLines[i - 1].maxY;
                float verticalGap = vline.minY - prevBottom;
                if (verticalGap > medianWordHeight * 1.65f) {
                    output += L"\r\n";
                }
            }
        }

        if (vline.words.empty()) {
            continue; // Blank numbered line in editor gutter
        }

        // Compute leading indentation spaces in Code Mode
        if (preserveIndentation && nonEmptyLineCount > 1) {
            float indentPx = std::max(0.0f, vline.leftX - minLeftX);
            int numSpaces = static_cast<int>(std::round(indentPx / medianCharWidth));
            if (numSpaces == 1 && indentPx < medianCharWidth * 1.35f) {
                numSpaces = 0;
            }
            numSpaces = std::clamp(numSpaces, 0, 64);
            if (numSpaces > 0) {
                output.append(static_cast<size_t>(numSpaces), L' ');
            }
        }

        // Join words on this visual line
        for (size_t wIdx = 0; wIdx < vline.words.size(); ++wIdx) {
            const auto* word = vline.words[wIdx];
            std::wstring token = word->text;
            if (preserveIndentation) {
                for (wchar_t& ch : token) {
                    if (ch == L'\x00F8' || ch == L'\x00D8') {
                        ch = L'0';
                    }
                }
            }

            if (wIdx > 0) {
                const auto* prevWord = vline.words[wIdx - 1];
                const std::wstring& prevText = prevWord->text;
                float gapPx = word->bounds.x - prevWord->bounds.right();

                bool shouldGlue = false;
                if (preserveIndentation && !prevText.empty() && !token.empty()) {
                    wchar_t lastCh = prevText.back();
                    wchar_t firstCh = token.front();

                    if (gapPx < medianCharWidth * 0.45f &&
                        !((lastCh == L')' || lastCh == L'>') && firstCh == L'{')) {
                        shouldGlue = true;
                    }
                    if (gapPx < medianCharWidth * 0.78f && (firstCh == L'(' || firstCh == L'[' || firstCh == L'?')) {
                        shouldGlue = true;
                    }
                    if (gapPx < medianCharWidth * 2.20f) {
                        if (lastCh == L'.' || lastCh == L'?' || lastCh == L'(' || lastCh == L'[' || lastCh == L'!') {
                            shouldGlue = true;
                        }
                        if (firstCh == L'.' || firstCh == L')' || firstCh == L']' || firstCh == L';' || firstCh == L',') {
                            shouldGlue = true;
                        }
                    }
                }

                if (!shouldGlue) {
                    if (preserveIndentation && gapPx > medianCharWidth * 3.0f) {
                        int gapSpaces = std::clamp(static_cast<int>(std::round(gapPx / medianCharWidth)), 1, 16);
                        output.append(static_cast<size_t>(gapSpaces), L' ');
                    } else {
                        output += L' ';
                    }
                }
            }
            output += token;
        }
    }

    return output;
}

namespace {

std::vector<std::wstring> SplitLines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    std::wstringstream ss(text);
    std::wstring line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

std::wstring TrimSpaces(const std::wstring& s) {
    size_t start = 0;
    while (start < s.size() && std::iswspace(s[start])) ++start;
    size_t end = s.size();
    while (end > start && std::iswspace(s[end - 1])) --end;
    return s.substr(start, end - start);
}

size_t LineEditDistance(const std::wstring& a, const std::wstring& b) {
    const size_t n = a.size(), m = b.size();
    std::vector<size_t> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = j;
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= m; ++j) {
            size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
        }
        prev.swap(cur);
    }
    return prev[m];
}

bool LinesFuzzyMatch(const std::wstring& a, const std::wstring& b) {
    std::wstring ta = TrimSpaces(a);
    std::wstring tb = TrimSpaces(b);
    if (ta == tb) return true;
    if (ta.empty() || tb.empty()) return false;
    size_t maxLen = std::max(ta.size(), tb.size());
    if (maxLen < 6) return false; // Short lines like "}" or ")" must match exactly
    size_t dist = LineEditDistance(ta, tb);
    return dist <= std::max<size_t>(1, maxLen / 6); // <= ~16% tolerance for minor video blur
}

} // namespace

std::wstring TextFormatter::StitchCodeBlocks(
    const std::wstring& existingText,
    const std::wstring& newText,
    int* outOverlapLines,
    int* outAppendedLines
) {
    auto aLines = SplitLines(existingText);
    auto bLines = SplitLines(newText);

    if (outOverlapLines) *outOverlapLines = 0;
    if (outAppendedLines) *outAppendedLines = static_cast<int>(bLines.size());

    if (aLines.empty()) return newText;
    if (bLines.empty()) {
        if (outAppendedLines) *outAppendedLines = 0;
        return existingText;
    }

    // Find the largest overlap k where the last k lines of aLines match the first k lines of bLines.
    size_t maxK = std::min({ aLines.size(), bLines.size(), static_cast<size_t>(20) });
    size_t bestK = 0;
    for (size_t k = maxK; k >= 1; --k) {
        bool allMatch = true;
        int nonEmptyCount = 0;
        size_t totalChars = 0;
        for (size_t i = 0; i < k; ++i) {
            const auto& la = aLines[aLines.size() - k + i];
            const auto& lb = bLines[i];
            if (!LinesFuzzyMatch(la, lb)) {
                allMatch = false;
                break;
            }
            std::wstring ta = TrimSpaces(la);
            if (!ta.empty()) {
                ++nonEmptyCount;
                totalChars += ta.size();
            }
        }
        // Require at least 1 meaningful line (or >= 4 chars) so a single "}" doesn't falsely merge unrelated blocks
        if (allMatch && (k >= 2 || (nonEmptyCount >= 1 && totalChars >= 4))) {
            bestK = k;
            break;
        }
    }

    int appended = static_cast<int>(bLines.size() - bestK);
    if (outOverlapLines) *outOverlapLines = static_cast<int>(bestK);
    if (outAppendedLines) *outAppendedLines = appended;

    std::wstring result;
    for (size_t i = 0; i < aLines.size(); ++i) {
        if (i > 0) result += L"\r\n";
        result += aLines[i];
    }
    for (size_t i = bestK; i < bLines.size(); ++i) {
        if (!result.empty()) result += L"\r\n";
        result += bLines[i];
    }
    return result;
}

} // namespace ScreenTextify
