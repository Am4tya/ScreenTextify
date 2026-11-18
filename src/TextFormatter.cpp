#include "TextFormatter.h"
#include <algorithm>
#include <cmath>
#include <numeric>

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

} // namespace

std::wstring TextFormatter::FormatDocument(
    const OcrDocument& doc,
    bool onlySelected,
    bool preserveIndentation,
    size_t* outWordCount
) {
    std::vector<const OcrWordBox*> activeWords;
    for (const auto& line : doc.lines) {
        for (const auto& word : line.words) {
            if (!onlySelected || word.selected) {
                activeWords.push_back(&word);
            }
        }
    }

    if (outWordCount) {
        *outWordCount = activeWords.size();
    }

    if (activeWords.empty()) {
        return L"";
    }

    // Estimate median character width & word height
    std::vector<float> charWidths;
    std::vector<float> wordHeights;
    charWidths.reserve(activeWords.size());
    wordHeights.reserve(activeWords.size());

    for (const auto* w : activeWords) {
        if (!w->text.empty() && w->bounds.width > 1.0f) {
            // Prefer multi-character words for stable monospace/proportional width estimation
            if (w->text.length() >= 2) {
                charWidths.push_back(w->bounds.width / static_cast<float>(w->text.length()));
            }
        }
        if (w->bounds.height > 1.0f) {
            wordHeights.push_back(w->bounds.height);
        }
    }

    if (charWidths.empty()) {
        for (const auto* w : activeWords) {
            if (!w->text.empty() && w->bounds.width > 1.0f) {
                charWidths.push_back(w->bounds.width / static_cast<float>(w->text.length()));
            }
        }
    }

    float medianCharWidth = 10.0f;
    if (!charWidths.empty()) {
        std::sort(charWidths.begin(), charWidths.end());
        medianCharWidth = charWidths[charWidths.size() / 2];
        medianCharWidth = std::max(3.0f, medianCharWidth);
    }

    float medianWordHeight = 18.0f;
    if (!wordHeights.empty()) {
        std::sort(wordHeights.begin(), wordHeights.end());
        medianWordHeight = wordHeights[wordHeights.size() / 2];
        medianWordHeight = std::max(6.0f, medianWordHeight);
    }

    // Sort words primarily by vertical center to cluster into visual lines
    std::sort(activeWords.begin(), activeWords.end(), [](const OcrWordBox* a, const OcrWordBox* b) {
        return a->bounds.centerY() < b->bounds.centerY();
    });

    std::vector<VisualLine> visualLines;
    float lineClusterThreshold = medianWordHeight * 0.58f;

    for (const auto* w : activeWords) {
        bool placed = false;
        for (auto& vline : visualLines) {
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
            visualLines.push_back(std::move(newLine));
        }
    }

    // Merge any visualLines that overlap vertically after word additions
    for (size_t i = 0; i < visualLines.size(); ++i) {
        for (size_t j = i + 1; j < visualLines.size(); ) {
            float overlapY = std::min(visualLines[i].maxY, visualLines[j].maxY) - std::max(visualLines[i].minY, visualLines[j].minY);
            float minH = std::min(visualLines[i].height, visualLines[j].height);
            if (std::abs(visualLines[i].centerY - visualLines[j].centerY) <= lineClusterThreshold ||
                (minH > 1.0f && overlapY >= minH * 0.50f)) {
                for (const auto* w : visualLines[j].words) {
                    visualLines[i].words.push_back(w);
                }
                visualLines[i].recomputeMetrics();
                visualLines.erase(visualLines.begin() + j);
            } else {
                ++j;
            }
        }
    }

    // Sort visual lines top-to-bottom
    std::sort(visualLines.begin(), visualLines.end(), [](const VisualLine& a, const VisualLine& b) {
        return a.centerY < b.centerY;
    });

    // Find minimum left X coordinate across all visual lines
    float minLeftX = 1e9f;
    for (const auto& vline : visualLines) {
        minLeftX = std::min(minLeftX, vline.leftX);
    }

    std::wstring output;
    for (size_t i = 0; i < visualLines.size(); ++i) {
        const auto& vline = visualLines[i];

        if (i > 0) {
            output += L"\r\n";
            // If preserveIndentation is enabled and there is a significant vertical gap, insert a blank line
            if (preserveIndentation) {
                float prevBottom = visualLines[i - 1].maxY;
                float verticalGap = vline.minY - prevBottom;
                if (verticalGap > medianWordHeight * 1.65f) {
                    output += L"\r\n";
                }
            }
        }

        // Compute leading indentation spaces in Code Mode
        if (preserveIndentation && visualLines.size() > 1) {
            float indentPx = std::max(0.0f, vline.leftX - minLeftX);
            int numSpaces = static_cast<int>(std::round(indentPx / medianCharWidth));
            // Ignore tiny 1-space jitter caused by font kerning
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
                // Monospace coding fonts (Consolas, JetBrains Mono, Cascadia) use slashed zeros,
                // which Windows OCR frequently decodes as 'ø' (U+00F8) or 'Ø' (U+00D8).
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

                    // If bounding boxes are visually touching (less than 45% of a character width),
                    // they belong to the same programming token (e.g. "findOne(message?" + "._id)"),
                    // except ') {' or '> {' which are always space-separated in block headers.
                    if (gapPx < medianCharWidth * 0.45f &&
                        !((lastCh == L')' || lastCh == L'>') && firstCh == L'{')) {
                        shouldGlue = true;
                    }
                    // Member access / method chaining / optional chaining / parens where OCR split tokens
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

} // namespace ScreenTextify
