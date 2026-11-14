#include "OcrService.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Globalization.h>

#include <future>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <mutex>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Storage::Streams;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Media::Ocr;
using namespace Windows::Globalization;

namespace ScreenTextify {

namespace {

OcrEngine GetSharedOcrEngine() {
    static std::mutex s_mutex;
    static OcrEngine s_engine{ nullptr };
    std::lock_guard<std::mutex> lock(s_mutex);

    if (!s_engine) {
        s_engine = OcrEngine::TryCreateFromUserProfileLanguages();
        if (!s_engine) {
            auto langs = OcrEngine::AvailableRecognizerLanguages();
            if (langs.Size() > 0) {
                s_engine = OcrEngine::TryCreateFromLanguage(langs.GetAt(0));
            }
        }
        if (!s_engine) {
            Language enUs(L"en-US");
            if (OcrEngine::IsLanguageSupported(enUs)) {
                s_engine = OcrEngine::TryCreateFromLanguage(enUs);
            }
        }
    }
    return s_engine;
}

OcrResult RunOcrOnGrayscaleBuffer(
    const OcrEngine& engine,
    const std::vector<uint8_t>& bgraPixels,
    int width,
    int height
) {
    DataWriter writer;
    writer.WriteBytes(winrt::array_view<const uint8_t>(bgraPixels));
    IBuffer buffer = writer.DetachBuffer();

    SoftwareBitmap softBmp = SoftwareBitmap::CreateCopyFromBuffer(
        buffer,
        BitmapPixelFormat::Bgra8,
        width,
        height,
        BitmapAlphaMode::Ignore
    );

    return engine.RecognizeAsync(softBmp).get();
}

struct InkComponent {
    int minX = 0, minY = 0, maxX = 0, maxY = 0;
    int pixelCount = 0;
    float topMeanX = 0.0f;
    float midMeanX = 0.0f;
    float botMeanX = 0.0f;

    int width() const { return maxX - minX + 1; }
    int height() const { return maxY - minY + 1; }
};

std::vector<InkComponent> ExtractConnectedComponents(
    const std::vector<uint8_t>& inkMask,
    int maskW,
    int maskH,
    int x0, int y0, int x1, int y1
) {
    x0 = std::clamp(x0, 0, maskW);
    y0 = std::clamp(y0, 0, maskH);
    x1 = std::clamp(x1, x0, maskW);
    y1 = std::clamp(y1, y0, maskH);

    int rw = x1 - x0;
    int rh = y1 - y0;
    if (rw <= 0 || rh <= 0) return {};

    std::vector<uint8_t> visited(static_cast<size_t>(rw) * rh, 0);
    std::vector<InkComponent> comps;
    std::vector<POINT> pts;
    pts.reserve(256);

    const int dx[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
    const int dy[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (!inkMask[static_cast<size_t>(y) * maskW + x]) continue;
            if (visited[static_cast<size_t>(y - y0) * rw + (x - x0)]) continue;

            pts.clear();
            visited[static_cast<size_t>(y - y0) * rw + (x - x0)] = 1;
            pts.push_back({ x, y });

            InkComponent comp;
            comp.minX = comp.maxX = x;
            comp.minY = comp.maxY = y;

            size_t head = 0;
            while (head < pts.size()) {
                POINT cur = pts[head++];
                comp.minX = std::min<int>(comp.minX, cur.x);
                comp.maxX = std::max<int>(comp.maxX, cur.x);
                comp.minY = std::min<int>(comp.minY, cur.y);
                comp.maxY = std::max<int>(comp.maxY, cur.y);

                for (int d = 0; d < 8; ++d) {
                    int nx = cur.x + dx[d];
                    int ny = cur.y + dy[d];
                    if (nx >= x0 && nx < x1 && ny >= y0 && ny < y1) {
                        size_t vIdx = static_cast<size_t>(ny - y0) * rw + (nx - x0);
                        if (!visited[vIdx] && inkMask[static_cast<size_t>(ny) * maskW + nx]) {
                            visited[vIdx] = 1;
                            pts.push_back({ nx, ny });
                        }
                    }
                }
            }

            comp.pixelCount = static_cast<int>(pts.size());
            if (comp.pixelCount >= 4) {
                int h = std::max(1, comp.height());
                float topSum = 0.0f, midSum = 0.0f, botSum = 0.0f;
                int topCnt = 0, midCnt = 0, botCnt = 0;
                for (const auto& p : pts) {
                    float relY = static_cast<float>(p.y - comp.minY) / static_cast<float>(h);
                    if (relY <= 0.25f) { topSum += p.x; ++topCnt; }
                    else if (relY >= 0.38f && relY <= 0.62f) { midSum += p.x; ++midCnt; }
                    else if (relY >= 0.75f) { botSum += p.x; ++botCnt; }
                }
                comp.topMeanX = topCnt > 0 ? (topSum / topCnt) : ((comp.minX + comp.maxX) * 0.5f);
                comp.midMeanX = midCnt > 0 ? (midSum / midCnt) : ((comp.minX + comp.maxX) * 0.5f);
                comp.botMeanX = botCnt > 0 ? (botSum / botCnt) : ((comp.minX + comp.maxX) * 0.5f);
                comps.push_back(comp);
            }
        }
    }

    std::sort(comps.begin(), comps.end(), [](const InkComponent& a, const InkComponent& b) {
        if (std::abs(a.minX - b.minX) > 4) return a.minX < b.minX;
        return a.minY < b.minY;
    });
    return comps;
}

bool IsSecondCharEqualsSign(
    const std::vector<uint8_t>& inkMask,
    int maskW,
    int maskH,
    int x0, int y0, int x1, int y1
) {
    int w = x1 - x0;
    int h = y1 - y0;
    if (w < 6 || h < 6) return false;

    int rx0 = std::clamp(x0 + (w * 50) / 100, 0, maskW - 1);
    int rx1 = std::clamp(x1, rx0 + 1, maskW);
    int ry0 = std::clamp(y0, 0, maskH - 1);
    int ry1 = std::clamp(y1, ry0 + 1, maskH);

    int firstInkY = -1, lastInkY = -1;
    std::vector<int> rowCounts(ry1 - ry0, 0);
    for (int y = ry0; y < ry1; ++y) {
        int c = 0;
        for (int x = rx0; x < rx1; ++x) {
            c += inkMask[static_cast<size_t>(y) * maskW + x];
        }
        rowCounts[y - ry0] = c;
        if (c >= 2) {
            if (firstInkY < 0) firstInkY = y - ry0;
            lastInkY = y - ry0;
        }
    }

    if (firstInkY < 0 || lastInkY - firstInkY < 4) return false;

    int midStart = firstInkY + (lastInkY - firstInkY) / 4;
    int midEnd = lastInkY - (lastInkY - firstInkY) / 4;
    for (int r = midStart; r <= midEnd; ++r) {
        if (rowCounts[r] == 0) {
            return true;
        }
    }
    return false;
}

bool HasHorizontalOverlapWithExistingWords(const OcrLineBox& line, const RectF& candidate) {
    for (const auto& w : line.words) {
        float overlapX = std::min(w.bounds.right(), candidate.right()) - std::max(w.bounds.x, candidate.x);
        float minW = std::min(w.bounds.width, candidate.width);
        if (minW > 0.5f && overlapX > minW * 0.30f) {
            return true;
        }
    }
    return false;
}

} // namespace

std::wstring OcrService::GetActiveLanguageName() {
    static std::wstring s_cachedLang;
    if (!s_cachedLang.empty()) return s_cachedLang;

    try {
        auto future = std::async(std::launch::async, []() -> std::wstring {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            OcrEngine engine = GetSharedOcrEngine();
            if (!engine) return L"Unavailable";
            return std::wstring(engine.RecognizerLanguage().DisplayName().c_str());
        });
        s_cachedLang = future.get();
        return s_cachedLang;
    } catch (...) {
        return L"Default System OCR";
    }
}

OcrDocument OcrService::RecognizeBitmap(const RawBitmap& bitmap, std::wstring* outError) {
    OcrDocument doc;
    if (!bitmap.valid()) {
        if (outError) *outError = L"Invalid or empty bitmap passed to OCR.";
        return doc;
    }

    auto startTime = std::chrono::steady_clock::now();

    auto task = std::async(std::launch::async, [&bitmap, outError]() -> OcrDocument {
        OcrDocument resultDoc;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);

            OcrEngine engine = GetSharedOcrEngine();
            if (!engine) {
                if (outError) {
                    *outError = L"Windows OCR Engine could not be initialized. Ensure an OCR language pack is installed in Windows Settings.";
                }
                return resultDoc;
            }

            uint32_t maxDim = OcrEngine::MaxImageDimension();
            const int pad = 28;
            const int srcW = bitmap.width;
            const int srcH = bitmap.height;
            const int paddedW = srcW + pad * 2;
            const int paddedH = srcH + pad * 2;

            if (static_cast<uint32_t>(paddedW) > maxDim || static_cast<uint32_t>(paddedH) > maxDim) {
                if (outError) {
                    *outError = L"Selected region exceeds maximum OCR image dimension. Please select a smaller region.";
                }
                return resultDoc;
            }

            const size_t totalSrcPixels = static_cast<size_t>(srcW) * srcH;
            const uint8_t* srcPx = bitmap.pixels.data();

            // 1. Horizontal-Strip 25th-Percentile Background Estimator
            // Editor selection highlights (#264F78), diff lines, and current-line bars have sharp horizontal boundaries.
            // By using thin horizontal strips (cellH = 2px, cellW = 36 * scale), there is ZERO vertical bleed across
            // selection box borders while spanning enough characters horizontally that the 25th percentile is pure background!
            uint64_t lumSum = 0;
            for (size_t i = 0; i < totalSrcPixels; i += 4) {
                uint32_t b = srcPx[i * 4 + 0];
                uint32_t g = srcPx[i * 4 + 1];
                uint32_t r = srcPx[i * 4 + 2];
                lumSum += (r * 77 + g * 150 + b * 29) >> 8;
            }
            uint32_t avgLum = (totalSrcPixels > 0) ? static_cast<uint32_t>(lumSum / ((totalSrcPixels + 3) / 4)) : 128;
            bool isDarkTheme = (avgLum < 140);

            const int cellW = std::max(36, static_cast<int>(36.0f * bitmap.scaleFactor));
            const int cellH = 2;
            const int gridW = (srcW + cellW - 1) / cellW;
            const int gridH = (srcH + cellH - 1) / cellH;

            struct RgbBg { int r, g, b; };
            std::vector<RgbBg> bgGrid(static_cast<size_t>(gridW) * gridH);

            struct SamplePx { int lum, r, g, b; };
            std::vector<SamplePx> cellSamples;
            cellSamples.reserve(128);

            for (int gy = 0; gy < gridH; ++gy) {
                int y0 = gy * cellH;
                int y1 = std::min(srcH, y0 + cellH);
                for (int gx = 0; gx < gridW; ++gx) {
                    int x0 = std::max(0, gx * cellW - cellW / 4);
                    int x1 = std::min(srcW, (gx + 1) * cellW + cellW / 4);

                    cellSamples.clear();
                    for (int y = y0; y < y1; ++y) {
                        const uint8_t* row = srcPx + static_cast<size_t>(y) * bitmap.stride;
                        for (int x = x0; x < x1; x += 2) {
                            int b = row[x * 4 + 0];
                            int g = row[x * 4 + 1];
                            int r = row[x * 4 + 2];
                            int lum = (r * 77 + g * 150 + b * 29) >> 8;
                            cellSamples.push_back({ lum, r, g, b });
                        }
                    }

                    if (cellSamples.empty()) {
                        bgGrid[static_cast<size_t>(gy) * gridW + gx] = isDarkTheme ? RgbBg{ 24, 24, 28 } : RgbBg{ 245, 245, 245 };
                    } else {
                        size_t targetIdx = isDarkTheme ? (cellSamples.size() / 4) : ((cellSamples.size() * 3) / 4);
                        std::nth_element(cellSamples.begin(), cellSamples.begin() + targetIdx, cellSamples.end(),
                            [](const SamplePx& a, const SamplePx& b) { return a.lum < b.lum; });
                        const auto& bg = cellSamples[targetIdx];
                        bgGrid[static_cast<size_t>(gy) * gridW + gx] = { bg.r, bg.g, bg.b };
                    }
                }
            }

            // 2. Build contrast-equalized grayscale BGRA8 buffer & binary inkMask
            std::vector<uint8_t> ocrPixels(static_cast<size_t>(paddedW) * paddedH * 4, 255);
            std::vector<uint8_t> inkMask(totalSrcPixels, 0);

            for (int y = 0; y < srcH; ++y) {
                int gy = std::clamp(y / cellH, 0, gridH - 1);

                const uint8_t* sRow = srcPx + static_cast<size_t>(y) * bitmap.stride;
                uint8_t* dRow = ocrPixels.data() + static_cast<size_t>(y + pad) * (paddedW * 4) + static_cast<size_t>(pad) * 4;
                uint8_t* mRow = inkMask.data() + static_cast<size_t>(y) * srcW;

                for (int x = 0; x < srcW; ++x) {
                    float fx = (static_cast<float>(x) + 0.5f) / cellW - 0.5f;
                    int gx0 = std::clamp(static_cast<int>(std::floor(fx)), 0, gridW - 1);
                    int gx1 = std::min(gx0 + 1, gridW - 1);
                    float wx = std::clamp(fx - gx0, 0.0f, 1.0f);

                    const auto& c0 = bgGrid[static_cast<size_t>(gy) * gridW + gx0];
                    const auto& c1 = bgGrid[static_cast<size_t>(gy) * gridW + gx1];

                    int bgR = static_cast<int>(c0.r * (1.0f - wx) + c1.r * wx);
                    int bgG = static_cast<int>(c0.g * (1.0f - wx) + c1.g * wx);
                    int bgB = static_cast<int>(c0.b * (1.0f - wx) + c1.b * wx);

                    int b = sRow[x * 4 + 0];
                    int g = sRow[x * 4 + 1];
                    int r = sRow[x * 4 + 2];

                    uint8_t ink = 255;
                    int maxD = 0;
                    if (isDarkTheme) {
                        int dR = std::max(0, r - bgR);
                        int dG = std::max(0, g - bgG);
                        int dB = std::max(0, b - bgB);
                        maxD = std::max({ dR, dG, dB });
                        int lumD = (dR * 77 + dG * 150 + dB * 29) >> 8;
                        int delta = (maxD * 2 + lumD) / 3;

                        if (delta <= 22) {
                            ink = 255;
                        } else {
                            float t = std::clamp(static_cast<float>(delta - 22) / 100.0f, 0.0f, 1.0f);
                            float smooth = t * t * (3.0f - 2.0f * t);
                            ink = static_cast<uint8_t>(std::clamp(255.0f * (1.0f - smooth), 0.0f, 255.0f));
                        }
                    } else {
                        int dR = std::max(0, bgR - r);
                        int dG = std::max(0, bgG - g);
                        int dB = std::max(0, bgB - b);
                        maxD = std::max({ dR, dG, dB });

                        if (maxD <= 18) {
                            ink = 255;
                        } else {
                            float t = std::clamp(static_cast<float>(maxD - 18) / 105.0f, 0.0f, 1.0f);
                            float smooth = t * t * (3.0f - 2.0f * t);
                            ink = static_cast<uint8_t>(std::clamp(255.0f * (1.0f - smooth), 0.0f, 255.0f));
                        }
                    }

                    dRow[x * 4 + 0] = ink;
                    dRow[x * 4 + 1] = ink;
                    dRow[x * 4 + 2] = ink;
                    dRow[x * 4 + 3] = 255;

                    // High-sensitivity binary mask captures 1px ClearType strokes ('_', '(', ')', '"', ';')
                    // while ignoring faint editor indentation guide lines (maxD ~ 35..40)
                    if (maxD >= 42 || ink < 185) {
                        mRow[x] = 1;
                    }
                }
            }

            // 3. Pass 1: Primary Full-Region OCR
            OcrResult ocrResult = RunOcrOnGrayscaleBuffer(engine, ocrPixels, paddedW, paddedH);
            if (!ocrResult) {
                return resultDoc;
            }

            float invScale = (bitmap.scaleFactor > 0.0f) ? (1.0f / bitmap.scaleFactor) : 1.0f;
            float offsetX = static_cast<float>(bitmap.screenOriginX);
            float offsetY = static_cast<float>(bitmap.screenOriginY);
            float padF = static_cast<float>(pad);

            resultDoc.scanRegion = {
                offsetX,
                offsetY,
                static_cast<float>(bitmap.width) * invScale,
                static_cast<float>(bitmap.height) * invScale
            };

            struct SrcIntRect {
                int x0, y0, x1, y1;
            };
            std::vector<SrcIntRect> coveredRects;

            auto lines = ocrResult.Lines();
            int lineIdx = 0;
            for (const auto& ocrLine : lines) {
                OcrLineBox lineBox;
                lineBox.text = ocrLine.Text().c_str();

                float minX = 1e9f, minY = 1e9f;
                float maxX = -1e9f, maxY = -1e9f;

                auto words = ocrLine.Words();
                int wordIdx = 0;
                for (const auto& ocrWord : words) {
                    Rect r = ocrWord.BoundingRect();
                    int sx0 = std::clamp(static_cast<int>(r.X - padF), 0, srcW);
                    int sy0 = std::clamp(static_cast<int>(r.Y - padF), 0, srcH);
                    int sx1 = std::clamp(static_cast<int>(r.X + r.Width - padF), 0, srcW);
                    int sy1 = std::clamp(static_cast<int>(r.Y + r.Height - padF), 0, srcH);
                    coveredRects.push_back({ sx0, sy0, sx1, sy1 });

                    std::wstring wText = ocrWord.Text().c_str();
                    if (wText == L"+2" || wText == L"-2" || wText == L"=2" || wText == L"!2") {
                        if (IsSecondCharEqualsSign(inkMask, srcW, srcH, sx0, sy0, sx1, sy1)) {
                            wText[1] = L'=';
                        }
                    }

                    OcrWordBox wordBox;
                    wordBox.text = std::move(wText);
                    wordBox.bounds.x = offsetX + (r.X - padF) * invScale;
                    wordBox.bounds.y = offsetY + (r.Y - padF) * invScale;
                    wordBox.bounds.width = r.Width * invScale;
                    wordBox.bounds.height = r.Height * invScale;
                    wordBox.lineIndex = lineIdx;
                    wordBox.wordIndex = wordIdx++;

                    minX = std::min(minX, wordBox.bounds.x);
                    minY = std::min(minY, wordBox.bounds.y);
                    maxX = std::max(maxX, wordBox.bounds.right());
                    maxY = std::max(maxY, wordBox.bounds.bottom());

                    lineBox.words.push_back(std::move(wordBox));
                }

                if (!lineBox.words.empty()) {
                    lineBox.bounds = { minX, minY, maxX - minX, maxY - minY };
                    resultDoc.lines.push_back(std::move(lineBox));
                    ++lineIdx;
                }
            }

            // Merge any OcrLineBoxes that vertically overlap (e.g. when Windows OCR puts '.' and ',' in a separate line from words)
            for (size_t i = 0; i < resultDoc.lines.size(); ++i) {
                for (size_t j = i + 1; j < resultDoc.lines.size(); ) {
                    auto& l1 = resultDoc.lines[i];
                    auto& l2 = resultDoc.lines[j];
                    float overlapY = std::min(l1.bounds.bottom(), l2.bounds.bottom()) - std::max(l1.bounds.y, l2.bounds.y);
                    float minH = std::min(l1.bounds.height, l2.bounds.height);
                    if ((minH > 0.5f && overlapY >= minH * 0.45f) ||
                        std::abs(l1.bounds.centerY() - l2.bounds.centerY()) <= std::max(l1.bounds.height, l2.bounds.height) * 0.45f) {
                        for (auto& w : l2.words) {
                            l1.words.push_back(std::move(w));
                        }
                        float nMinX = std::min(l1.bounds.x, l2.bounds.x);
                        float nMinY = std::min(l1.bounds.y, l2.bounds.y);
                        float nMaxX = std::max(l1.bounds.right(), l2.bounds.right());
                        float nMaxY = std::max(l1.bounds.bottom(), l2.bounds.bottom());
                        l1.bounds = { nMinX, nMinY, nMaxX - nMinX, nMaxY - nMinY };
                        resultDoc.lines.erase(resultDoc.lines.begin() + j);
                    } else {
                        ++j;
                    }
                }
            }

            float medWordH = 15.0f * bitmap.scaleFactor;
            if (!coveredRects.empty()) {
                std::vector<int> heights;
                heights.reserve(coveredRects.size());
                for (const auto& cr : coveredRects) heights.push_back(cr.y1 - cr.y0);
                std::sort(heights.begin(), heights.end());
                medWordH = static_cast<float>(std::max(10, heights[heights.size() / 2]));
            }

            // Clear covered word rectangles in uncoveredMask (with vertical margin for ascenders/descenders,
            // but only +1px on the right so trailing ';' right after ')' is preserved!)
            std::vector<uint8_t> uncoveredMask = inkMask;
            for (const auto& cr : coveredRects) {
                int cy0 = std::max(0, cr.y0 - 4);
                int cy1 = std::min(srcH, cr.y1 + 5);
                int cx0 = std::max(0, cr.x0 - 2);
                int cx1 = std::min(srcW, cr.x1 + 1);
                for (int y = cy0; y < cy1; ++y) {
                    uint8_t* mRow = uncoveredMask.data() + static_cast<size_t>(y) * srcW;
                    std::fill(mRow + cx0, mRow + cx1, uint8_t(0));
                }
            }

            // 4. Pass 2: Intra-Line Uncovered Token & Symbol Recovery
            int recoveredPatches = 0;
            for (size_t lIdx = 0; lIdx < resultDoc.lines.size(); ++lIdx) {
                auto& lineBox = resultDoc.lines[lIdx];
                int bandY0 = std::clamp(static_cast<int>((lineBox.bounds.y - offsetY) * bitmap.scaleFactor) - 4, 0, srcH - 1);
                int bandY1 = std::clamp(static_cast<int>((lineBox.bounds.bottom() - offsetY) * bitmap.scaleFactor) + 4, bandY0 + 1, srcH);

                int segStart = -1;
                int blankRun = 0;
                int segInkCount = 0;
                int maxGap = static_cast<int>(12.0f * bitmap.scaleFactor);

                for (int x = 0; x <= srcW && recoveredPatches < 10; ++x) {
                    int colInk = 0;
                    if (x < srcW) {
                        for (int y = bandY0; y < bandY1; ++y) {
                            colInk += uncoveredMask[static_cast<size_t>(y) * srcW + x];
                        }
                    }

                    if (colInk >= 2) {
                        if (segStart < 0) segStart = x;
                        segInkCount += colInk;
                        blankRun = 0;
                    } else if (segStart >= 0) {
                        ++blankRun;
                        if (blankRun >= maxGap || x == srcW) {
                            int segEnd = x - blankRun + 1;
                            int segW = segEnd - segStart;
                            if (segW >= static_cast<int>(10.0f * bitmap.scaleFactor) && segInkCount >= 22) {
                                int cropX0 = std::max(0, segStart - 6);
                                int cropY0 = std::max(0, bandY0 - 4);
                                int cropX1 = std::min(srcW, segEnd + 6);
                                int cropY1 = std::min(srcH, bandY1 + 4);
                                int cw = cropX1 - cropX0;
                                int ch = cropY1 - cropY0;

                                const int subPad = 28;
                                int subW = cw + subPad * 2;
                                int subH = ch + subPad * 2;
                                std::vector<uint8_t> subPixels(static_cast<size_t>(subW) * subH * 4, 255);

                                for (int sy = 0; sy < ch; ++sy) {
                                    const uint8_t* srcRow = ocrPixels.data() +
                                        static_cast<size_t>(cropY0 + sy + pad) * (paddedW * 4) +
                                        static_cast<size_t>(cropX0 + pad) * 4;
                                    uint8_t* dstRow = subPixels.data() +
                                        static_cast<size_t>(sy + subPad) * (subW * 4) +
                                        static_cast<size_t>(subPad) * 4;
                                    memcpy(dstRow, srcRow, static_cast<size_t>(cw) * 4);
                                }

                                OcrResult subRes = RunOcrOnGrayscaleBuffer(engine, subPixels, subW, subH);
                                if (subRes && subRes.Lines().Size() > 0) {
                                    for (const auto& sLine : subRes.Lines()) {
                                        for (const auto& sWord : sLine.Words()) {
                                            Rect sr = sWord.BoundingRect();
                                            OcrWordBox wb;
                                            wb.text = sWord.Text().c_str();
                                            wb.bounds.x = offsetX + (cropX0 + sr.X - subPad) * invScale;
                                            wb.bounds.y = offsetY + (cropY0 + sr.Y - subPad) * invScale;
                                            wb.bounds.width = sr.Width * invScale;
                                            wb.bounds.height = sr.Height * invScale;
                                            wb.lineIndex = static_cast<int>(lIdx);

                                            if (!HasHorizontalOverlapWithExistingWords(lineBox, wb.bounds)) {
                                                lineBox.words.push_back(std::move(wb));
                                            }
                                        }
                                    }
                                }
                                for (int cy = cropY0; cy < cropY1; ++cy) {
                                    uint8_t* mRow = uncoveredMask.data() + static_cast<size_t>(cy) * srcW;
                                    std::fill(mRow + cropX0, mRow + cropX1, uint8_t(0));
                                }
                                ++recoveredPatches;
                            }
                            segStart = -1;
                            segInkCount = 0;
                            blankRun = 0;
                        }
                    }
                }

                std::sort(lineBox.words.begin(), lineBox.words.end(), [](const OcrWordBox& a, const OcrWordBox& b) {
                    return a.bounds.x < b.bounds.x;
                });

                // 4b. Topological Uncovered Symbol Recovery ('_', '(', '"', ',', ')', ';') + Quote/Paren Parity State Machine
                for (size_t wIdx = 0; wIdx + 1 < lineBox.words.size(); ++wIdx) {
                    auto& prevW = lineBox.words[wIdx];
                    auto& nextW = lineBox.words[wIdx + 1];
                    int gx0 = std::clamp(static_cast<int>((prevW.bounds.right() - offsetX) * bitmap.scaleFactor), 0, srcW);
                    int gx1 = std::clamp(static_cast<int>((nextW.bounds.x - offsetX) * bitmap.scaleFactor), gx0, srcW);

                    if (gx1 - gx0 >= static_cast<int>(2.0f * bitmap.scaleFactor) &&
                        gx1 - gx0 <= static_cast<int>(28.0f * bitmap.scaleFactor)) {
                        auto gapComps = ExtractConnectedComponents(uncoveredMask, srcW, srcH, gx0, bandY0, gx1, bandY1);
                        for (const auto& gc : gapComps) {
                            if (gc.width() >= static_cast<int>(medWordH * 0.20f) &&
                                gc.width() <= static_cast<int>(medWordH * 0.85f) &&
                                gc.height() <= static_cast<int>(medWordH * 0.35f) &&
                                gc.minY >= (bandY0 + bandY1) / 2 - 1) {
                                if (!nextW.text.empty() && nextW.text.front() != L'_') {
                                    nextW.text.insert(nextW.text.begin(), L'_');
                                    float newLeft = prevW.bounds.right();
                                    nextW.bounds.width += (nextW.bounds.x - newLeft);
                                    nextW.bounds.x = newLeft;
                                }
                                break;
                            }
                        }
                    }
                }

                // Quote-Parity & Parenthesis Balance State Machine across the line
                int totalQuotesOnLine = 0;
                int parenBalance = 0;
                for (const auto& w : lineBox.words) {
                    for (wchar_t ch : w.text) {
                        if (ch == L'"') ++totalQuotesOnLine;
                        else if (ch == L'(') ++parenBalance;
                        else if (ch == L')') --parenBalance;
                    }
                }

                // If total '"' on the line is odd and a word starts with single quote '\'' (misread opening '"'), normalize '\'' -> '"'
                if (totalQuotesOnLine % 2 == 1) {
                    for (auto& w : lineBox.words) {
                        if (!w.text.empty() && w.text.front() == L'\'') {
                            w.text.front() = L'"';
                            ++totalQuotesOnLine;
                            break;
                        }
                    }
                }

                int runningQuotes = 0;
                for (size_t wIdx = 0; wIdx < lineBox.words.size(); ++wIdx) {
                    auto& curW = lineBox.words[wIdx];
                    int wordQuotes = 0;
                    for (wchar_t ch : curW.text) {
                        if (ch == L'"') ++wordQuotes;
                    }

                    // If outside a string before curW (runningQuotes is even), and curW has a closing '"' without an opening '"'
                    if ((runningQuotes % 2 == 0) && (wordQuotes % 2 == 1) && !curW.text.empty() && curW.text.front() != L'"') {
                        curW.text.insert(curW.text.begin(), L'"');
                        ++wordQuotes;
                        // If preceded by a method/function identifier and line is missing '(', restore '('
                        if (wIdx > 0 && parenBalance < 0) {
                            auto& prevW = lineBox.words[wIdx - 1];
                            if (!prevW.text.empty() && std::iswalnum(prevW.text.back())) {
                                prevW.text += L'(';
                                prevW.bounds.width = std::max(prevW.bounds.width, curW.bounds.x - prevW.bounds.x);
                                ++parenBalance;
                            }
                        }
                    }

                    runningQuotes += wordQuotes;

                    // If curW just closed a string literal (ends with '"') and the next word opens a string literal (starts with '"'),
                    // restore dropped comma ',' between adjacent string arguments
                    if ((runningQuotes % 2 == 0) && !curW.text.empty() && curW.text.back() == L'"' &&
                        wIdx + 1 < lineBox.words.size() && !lineBox.words[wIdx + 1].text.empty() &&
                        lineBox.words[wIdx + 1].text.front() == L'"') {
                        curW.text += L',';
                    }
                }

                // Check right BEFORE the first word and AFTER the last word for uncovered '.', '(', ')', ',', ';'
                if (!lineBox.words.empty()) {
                    auto& firstW = lineBox.words.front();
                    auto& lastW = lineBox.words.back();

                    int lx1 = std::clamp(static_cast<int>((firstW.bounds.x - offsetX) * bitmap.scaleFactor), 0, srcW);
                    int lx0 = std::clamp(lx1 - static_cast<int>(20.0f * bitmap.scaleFactor), 0, lx1);
                    auto leadComps = ExtractConnectedComponents(uncoveredMask, srcW, srcH, lx0, bandY0, lx1, bandY1);

                    int tx0 = std::clamp(static_cast<int>((lastW.bounds.right() - offsetX) * bitmap.scaleFactor), 0, srcW);
                    int tx1 = std::clamp(tx0 + static_cast<int>(22.0f * bitmap.scaleFactor), tx0, srcW);
                    auto trailComps = ExtractConnectedComponents(uncoveredMask, srcW, srcH, tx0, bandY0, tx1, bandY1);

                    const InkComponent* leadParen = nullptr;
                    const InkComponent* leadDot = nullptr;
                    for (const auto& lc : leadComps) {
                        if (lc.height() >= static_cast<int>(medWordH * 0.42f) && lc.width() <= static_cast<int>(medWordH * 0.65f)) {
                            leadParen = &lc;
                        } else if (lc.width() <= static_cast<int>(6.5f * bitmap.scaleFactor) &&
                                   lc.height() <= static_cast<int>(6.5f * bitmap.scaleFactor) &&
                                   lc.minY >= (bandY0 + bandY1) / 2 - 2) {
                            leadDot = &lc;
                        }
                    }

                    if (leadDot && !firstW.text.empty() && std::iswlower(firstW.text.front())) {
                        firstW.text.insert(firstW.text.begin(), L'.');
                        float newLeft = offsetX + leadDot->minX * invScale;
                        firstW.bounds.width += std::max(0.0f, firstW.bounds.x - newLeft);
                        firstW.bounds.x = std::min(firstW.bounds.x, newLeft);
                    }

                    const InkComponent* trailParen = nullptr;
                    const InkComponent* trailComma = nullptr;
                    for (const auto& tc : trailComps) {
                        if (tc.height() >= static_cast<int>(medWordH * 0.50f) &&
                            tc.width() <= static_cast<int>(medWordH * 0.65f) &&
                            tc.minY <= (bandY0 + bandY1) / 2 - static_cast<int>(medWordH * 0.15f)) {
                            trailParen = &tc;
                            break;
                        } else if (tc.minY > (bandY0 + bandY1) / 2 - static_cast<int>(medWordH * 0.18f) &&
                                   tc.height() <= static_cast<int>(medWordH * 0.65f) &&
                                   tc.width() <= static_cast<int>(medWordH * 0.65f)) {
                            trailComma = &tc;
                        }
                    }

                    if (trailComma && !lastW.text.empty() && lastW.text.back() == L'"') {
                        lastW.text += L',';
                        lastW.bounds.width = std::max(lastW.bounds.width, (trailComma->maxX * invScale) + offsetX - lastW.bounds.x);
                    }

                    if (parenBalance < 0 && !firstW.text.empty() && firstW.text.front() != L'(' &&
                        (leadParen != nullptr || lineBox.words.size() == 1)) {
                        firstW.text.insert(firstW.text.begin(), L'(');
                        if (leadParen) {
                            float newLeft = offsetX + leadParen->minX * invScale;
                            firstW.bounds.width += std::max(0.0f, firstW.bounds.x - newLeft);
                            firstW.bounds.x = std::min(firstW.bounds.x, newLeft);
                        }
                        ++parenBalance;
                    } else if (parenBalance > 0 && trailParen && !lastW.text.empty() &&
                               lastW.text.back() != L')' && lastW.text.back() != L';') {
                        lastW.text += L')';
                        lastW.bounds.width = std::max(lastW.bounds.width, (trailParen->maxX * invScale) + offsetX - lastW.bounds.x);
                        --parenBalance;
                    } else if (parenBalance == 0 && leadParen && trailParen &&
                               !firstW.text.empty() && firstW.text.front() != L'(' &&
                               !lastW.text.empty() && lastW.text.back() != L')' && lastW.text.back() != L';') {
                        firstW.text.insert(firstW.text.begin(), L'(');
                        float newLeft = offsetX + leadParen->minX * invScale;
                        firstW.bounds.width += std::max(0.0f, firstW.bounds.x - newLeft);
                        firstW.bounds.x = std::min(firstW.bounds.x, newLeft);

                        lastW.text += L')';
                        lastW.bounds.width = std::max(lastW.bounds.width, (trailParen->maxX * invScale) + offsetX - lastW.bounds.x);
                    }

                    if (!lastW.text.empty() && lastW.text.back() != L';' && lastW.text.back() != L'{' && lastW.text.back() != L',') {
                        for (size_t cIdx = 0; cIdx < trailComps.size(); ++cIdx) {
                            const auto& tc = trailComps[cIdx];
                            for (size_t cIdx2 = cIdx + 1; cIdx2 < trailComps.size(); ++cIdx2) {
                                const auto& tc2 = trailComps[cIdx2];
                                int dx = std::abs((tc.minX + tc.maxX) / 2 - (tc2.minX + tc2.maxX) / 2);
                                if (dx <= static_cast<int>(4.5f * bitmap.scaleFactor) &&
                                    tc.width() <= static_cast<int>(7.0f * bitmap.scaleFactor) &&
                                    tc2.width() <= static_cast<int>(7.0f * bitmap.scaleFactor)) {
                                    const auto& topC = (tc.minY < tc2.minY) ? tc : tc2;
                                    const auto& botC = (tc.minY < tc2.minY) ? tc2 : tc;
                                    if (botC.minY >= topC.maxY - 2 && botC.maxY > (bandY0 + bandY1) / 2) {
                                        lastW.text += L';';
                                        int maxRight = std::max(topC.maxX, botC.maxX);
                                        lastW.bounds.width = std::max(lastW.bounds.width, (maxRight * invScale) + offsetX - lastW.bounds.x);
                                        break;
                                    }
                                }
                            }
                            if (!lastW.text.empty() && lastW.text.back() == L';') break;
                        }
                    }
                }

                float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
                for (size_t wIdx = 0; wIdx < lineBox.words.size(); ++wIdx) {
                    lineBox.words[wIdx].wordIndex = static_cast<int>(wIdx);
                    minX = std::min(minX, lineBox.words[wIdx].bounds.x);
                    minY = std::min(minY, lineBox.words[wIdx].bounds.y);
                    maxX = std::max(maxX, lineBox.words[wIdx].bounds.right());
                    maxY = std::max(maxY, lineBox.words[wIdx].bounds.bottom());
                }
                lineBox.bounds = { minX, minY, maxX - minX, maxY - minY };

                // Clear entire processed line band in uncoveredMask so Pass 3 only looks at completely missed lines!
                for (int y = bandY0; y < bandY1; ++y) {
                    std::fill(uncoveredMask.data() + static_cast<size_t>(y) * srcW,
                              uncoveredMask.data() + static_cast<size_t>(y + 1) * srcW, uint8_t(0));
                }
            }

            // 5. Pass 3: 2D Connected-Component Y-Clustering for Missed Standalone Symbol Lines ('{', '}', '};')
            auto allUncoveredComps = ExtractConnectedComponents(uncoveredMask, srcW, srcH, 0, 0, srcW, srcH);
            std::vector<InkComponent> glyphComps;
            glyphComps.reserve(allUncoveredComps.size());
            for (const auto& c : allUncoveredComps) {
                // Filter out tall vertical editor indentation guide lines & long horizontal borders
                if (c.height() > static_cast<int>(medWordH * 1.65f) && c.width() <= static_cast<int>(5.0f * bitmap.scaleFactor)) {
                    continue;
                }
                if (c.width() > static_cast<int>(medWordH * 1.50f) && c.height() <= static_cast<int>(4.0f * bitmap.scaleFactor)) {
                    continue;
                }
                glyphComps.push_back(c);
            }

            std::sort(glyphComps.begin(), glyphComps.end(), [](const InkComponent& a, const InkComponent& b) {
                return (a.minY + a.maxY) < (b.minY + b.maxY);
            });

            struct CompRowCluster {
                int minX = 100000, minY = 100000, maxX = -1, maxY = -1;
                float sumCenterY = 0.0f;
                std::vector<InkComponent> comps;
                float meanCenterY() const { return comps.empty() ? 0.0f : (sumCenterY / comps.size()); }
            };
            std::vector<CompRowCluster> rowClusters;

            for (const auto& gc : glyphComps) {
                float cy = 0.5f * (gc.minY + gc.maxY);
                if (!rowClusters.empty() && std::abs(cy - rowClusters.back().meanCenterY()) <= medWordH * 0.60f) {
                    auto& cl = rowClusters.back();
                    cl.minX = std::min(cl.minX, gc.minX);
                    cl.minY = std::min(cl.minY, gc.minY);
                    cl.maxX = std::max(cl.maxX, gc.maxX);
                    cl.maxY = std::max(cl.maxY, gc.maxY);
                    cl.sumCenterY += cy;
                    cl.comps.push_back(gc);
                } else {
                    CompRowCluster cl;
                    cl.minX = gc.minX;
                    cl.minY = gc.minY;
                    cl.maxX = gc.maxX;
                    cl.maxY = gc.maxY;
                    cl.sumCenterY = cy;
                    cl.comps.push_back(gc);
                    rowClusters.push_back(std::move(cl));
                }
            }

            for (auto& cl : rowClusters) {
                if (recoveredPatches >= 18) break;
                int bandH = cl.maxY - cl.minY + 1;
                if (bandH < static_cast<int>(8.0f * bitmap.scaleFactor)) continue;

                std::sort(cl.comps.begin(), cl.comps.end(), [](const InkComponent& a, const InkComponent& b) {
                    if (std::abs(a.minX - b.minX) > 4) return a.minX < b.minX;
                    return a.minY < b.minY;
                });

                bool addedViaOcr = false;
                int cropX0 = std::max(0, cl.minX - 8);
                int cropY0 = std::max(0, cl.minY - 6);
                int cropX1 = std::min(srcW, cl.maxX + 8);
                int cropY1 = std::min(srcH, cl.maxY + 6);
                int cw = cropX1 - cropX0;
                int ch = cropY1 - cropY0;

                if (cw >= static_cast<int>(18.0f * bitmap.scaleFactor) && cl.comps.size() > 3) {
                    const int subPad = 28;
                    int subW = cw + subPad * 2;
                    int subH = ch + subPad * 2;
                    std::vector<uint8_t> subPixels(static_cast<size_t>(subW) * subH * 4, 255);
                    for (int sy = 0; sy < ch; ++sy) {
                        const uint8_t* srcRow = ocrPixels.data() +
                            static_cast<size_t>(cropY0 + sy + pad) * (paddedW * 4) +
                            static_cast<size_t>(cropX0 + pad) * 4;
                        uint8_t* dstRow = subPixels.data() +
                            static_cast<size_t>(sy + subPad) * (subW * 4) +
                            static_cast<size_t>(subPad) * 4;
                        memcpy(dstRow, srcRow, static_cast<size_t>(cw) * 4);
                    }
                    OcrResult subRes = RunOcrOnGrayscaleBuffer(engine, subPixels, subW, subH);
                    if (subRes && subRes.Lines().Size() > 0) {
                        for (const auto& sLine : subRes.Lines()) {
                            OcrLineBox newLine;
                            newLine.text = sLine.Text().c_str();
                            float lMinX = 1e9f, lMinY = 1e9f, lMaxX = -1e9f, lMaxY = -1e9f;
                            int wIdx = 0;
                            for (const auto& sWord : sLine.Words()) {
                                Rect sr = sWord.BoundingRect();
                                OcrWordBox wb;
                                wb.text = sWord.Text().c_str();
                                wb.bounds.x = offsetX + (cropX0 + sr.X - subPad) * invScale;
                                wb.bounds.y = offsetY + (cropY0 + sr.Y - subPad) * invScale;
                                wb.bounds.width = sr.Width * invScale;
                                wb.bounds.height = sr.Height * invScale;
                                wb.lineIndex = static_cast<int>(resultDoc.lines.size());
                                wb.wordIndex = wIdx++;
                                lMinX = std::min(lMinX, wb.bounds.x);
                                lMinY = std::min(lMinY, wb.bounds.y);
                                lMaxX = std::max(lMaxX, wb.bounds.right());
                                lMaxY = std::max(lMaxY, wb.bounds.bottom());
                                newLine.words.push_back(std::move(wb));
                            }
                            if (!newLine.words.empty()) {
                                newLine.bounds = { lMinX, lMinY, lMaxX - lMinX, lMaxY - lMinY };
                                resultDoc.lines.push_back(std::move(newLine));
                                addedViaOcr = true;
                            }
                        }
                    }
                }

                if (!addedViaOcr && cl.comps.size() <= 4) {
                    std::wstring symText;
                    int symMinX = 100000, symMinY = 100000, symMaxX = -1, symMaxY = -1;

                    for (size_t i = 0; i < cl.comps.size(); ++i) {
                        const auto& c = cl.comps[i];
                        if (c.height() >= static_cast<int>(8.5f * bitmap.scaleFactor) &&
                            c.height() <= static_cast<int>(medWordH * 1.65f) &&
                            c.width() <= static_cast<int>(medWordH * 0.80f)) {
                            float endsX = 0.5f * (c.topMeanX + c.botMeanX);
                            if (c.midMeanX > endsX + 0.30f) {
                                symText += L'}';
                            } else if (c.midMeanX < endsX - 0.30f) {
                                symText += L'{';
                            }
                            symMinX = std::min(symMinX, c.minX);
                            symMinY = std::min(symMinY, c.minY);
                            symMaxX = std::max(symMaxX, c.maxX);
                            symMaxY = std::max(symMaxY, c.maxY);
                        } else if (i + 1 < cl.comps.size()) {
                            const auto& c2 = cl.comps[i + 1];
                            int dx = std::abs((c.minX + c.maxX) / 2 - (c2.minX + c2.maxX) / 2);
                            if (dx <= static_cast<int>(5.0f * bitmap.scaleFactor) &&
                                c.width() <= static_cast<int>(7.5f * bitmap.scaleFactor) &&
                                c2.width() <= static_cast<int>(7.5f * bitmap.scaleFactor)) {
                                symText += L';';
                                symMinX = std::min({ symMinX, c.minX, c2.minX });
                                symMinY = std::min({ symMinY, c.minY, c2.minY });
                                symMaxX = std::max({ symMaxX, c.maxX, c2.maxX });
                                symMaxY = std::max({ symMaxY, c.maxY, c2.maxY });
                                ++i;
                            }
                        }
                    }

                    if (!symText.empty() && symMaxX > symMinX) {
                        OcrWordBox wb;
                        wb.text = symText;
                        wb.bounds.x = offsetX + symMinX * invScale;
                        wb.bounds.y = offsetY + symMinY * invScale;
                        wb.bounds.width = std::max(8.0f, (symMaxX - symMinX + 1) * invScale);
                        wb.bounds.height = std::max(10.0f, (symMaxY - symMinY + 1) * invScale);
                        wb.lineIndex = static_cast<int>(resultDoc.lines.size());
                        wb.wordIndex = 0;

                        OcrLineBox newLine;
                        newLine.text = symText;
                        newLine.bounds = wb.bounds;
                        newLine.words.push_back(std::move(wb));
                        resultDoc.lines.push_back(std::move(newLine));
                    }
                }
                ++recoveredPatches;
            }

            std::sort(resultDoc.lines.begin(), resultDoc.lines.end(), [](const OcrLineBox& a, const OcrLineBox& b) {
                return a.bounds.centerY() < b.bounds.centerY();
            });
            for (size_t i = 0; i < resultDoc.lines.size(); ++i) {
                for (auto& w : resultDoc.lines[i].words) {
                    w.lineIndex = static_cast<int>(i);
                }
            }

        } catch (const winrt::hresult_error& ex) {
            if (outError) {
                *outError = std::wstring(L"WinRT OCR error: ") + ex.message().c_str();
            }
        } catch (const std::exception& ex) {
            if (outError) {
                std::string msg = ex.what();
                *outError = std::wstring(msg.begin(), msg.end());
            }
        }
        return resultDoc;
    });

    doc = task.get();
    auto endTime = std::chrono::steady_clock::now();
    doc.recognitionTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    return doc;
}

} // namespace ScreenTextify
