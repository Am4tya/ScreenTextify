// PaddleOcr.cpp — PP-OCRv5 + PP-OCRv4 Dual-Model / Dual-View Code Engine on ONNX Runtime (CPU).
//
// Key capabilities for copying code from compressed YouTube videos & images:
//   1. Horizontal-Strip 25th-Percentile Background Estimator + Unsharp-Masked Contrast Equalizer (~2 ms)
//      plus a parallel polarity-normalized raw view for dual-view confidence verification.
//   2. 2D 8-Connected Component Row & Clause Detector (~0.3 ms) that captures 100% of standalone
//      braces/parens ('{', '}', '(', ')', '};') and edge punctuation without scene-photo DBNet drops.
//   3. ASCII-Constrained CTC Decoding across PP-OCRv5 (rec_en_v5.onnx) and PP-OCRv4 (rec_en.onnx),
//      automatically cross-checking low-confidence lines across both models and views.
//   4. Keyword-free 2-Bar Connected-Component '=' Verifier, 5-Zone Curvature Classifier, and
//      Pushdown Automaton (PDA) Delimiter Stack.

#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>

#include "PaddleOcr.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <numeric>
#include <thread>
#include <unordered_map>

namespace ScreenTextify {

namespace {

namespace fs = std::filesystem;

constexpr int kRecHeight   = 48;     // PP-OCRv4 / PP-OCRv5 recognizer input height
constexpr int kRecMinWidth = 48;
constexpr int kRecMaxWidth = 3200;

struct Recognizer {
    std::wstring name;
    std::vector<Ort::Session> sessions;  // 1 private session per worker -> lock-free BFCArena + MemPattern!
    std::string inName, outName;
    std::vector<std::wstring> alphabet;  // index 0 = CTC blank
    std::vector<int> allowed;            // class indices decodable to code ASCII (0 = blank included)
    std::vector<wchar_t> asciiOf;        // class index -> ASCII character (0 = disallowed)
};

struct Engine {
    Ort::Env env{ nullptr };
    Ort::SessionOptions opts{ nullptr };
    std::vector<std::unique_ptr<Recognizer>> recs;

    Recognizer* Find(const std::wstring& n) const {
        for (const auto& r : recs) {
            if (r->name == n) return r.get();
        }
        return nullptr;
    }
};

Engine* g_engine = nullptr;
std::once_flag g_initOnce;
std::atomic<bool> g_ready{ false };
std::wstring g_initError;
PaddleOcr::Options g_options;
std::mutex g_optMutex;

fs::path ExeDir() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return fs::path(buf).parent_path();
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::vector<std::wstring> ReadDictLines(const fs::path& dictPath) {
    std::ifstream f(dictPath, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::wstring> lines;
    size_t start = 0;
    while (start <= content.size()) {
        size_t nl = content.find('\n', start);
        std::string line = content.substr(start, (nl == std::string::npos ? content.size() : nl) - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (nl == std::string::npos) {
            if (!line.empty()) lines.push_back(Utf8ToWide(line));
            break;
        }
        lines.push_back(Utf8ToWide(line));
        start = nl + 1;
    }
    return lines;
}

// Maps typographic look-alikes that recognizers emit for code glyphs back to standard ASCII.
wchar_t ToCodeAscii(wchar_t c) {
    if (c >= 0x20 && c <= 0x7E) return c;
    if (c >= 0xFF01 && c <= 0xFF5E) return static_cast<wchar_t>(c - 0xFEE0);
    switch (c) {
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: case 0x00B4: return L'\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: return L'"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2212: return L'-';
    case 0x2024: case 0x00B7: case 0x2219: case 0x22C5: return L'.';
    case 0x2236: return L':';
    case 0x2215: return L'/';
    case 0x2217: return L'*';
    case 0x223C: return L'~';
    default: return 0;
    }
}

// Determine optimal physical-core worker count (avoids SMT AVX2 FMA unit contention on Zen/Core CPUs)
size_t OptimalWorkerCount(size_t taskCount) {
    if (taskCount <= 1) return 1;
    unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    size_t physEst = (hw >= 8) ? (hw / 2) : hw;
    size_t maxWorkers = std::clamp<size_t>(physEst, 2, 6);
    return std::min(taskCount, maxWorkers);
}

std::unique_ptr<Recognizer> LoadRecognizer(Engine& e, const std::wstring& name, const fs::path& model, const fs::path& dict) {
    auto r = std::make_unique<Recognizer>();
    r->name = name;

    // Read model file once into memory, then build 1 private Ort::Session per physical worker core
    std::ifstream mf(model, std::ios::binary | std::ios::ate);
    std::streamsize sz = mf.tellg();
    mf.seekg(0, std::ios::beg);
    std::vector<char> modelBytes(static_cast<size_t>(std::max<std::streamsize>(0, sz)));
    if (sz > 0) mf.read(modelBytes.data(), sz);

    const size_t numSlots = OptimalWorkerCount(6);
    std::vector<std::unique_ptr<Ort::Session>> loaded(numSlots);
    // Construct private per-worker Ort::Sessions in parallel from the in-memory model buffer
    std::vector<std::thread> initThreads;
    initThreads.reserve(numSlots > 1 ? numSlots - 1 : 0);
    auto buildSlot = [&](size_t s) {
        loaded[s] = std::make_unique<Ort::Session>(e.env, modelBytes.data(), modelBytes.size(), e.opts);
    };
    for (size_t s = 1; s < numSlots; ++s) initThreads.emplace_back(buildSlot, s);
    buildSlot(0);
    for (auto& t : initThreads) t.join();

    r->sessions.reserve(numSlots);
    for (auto& up : loaded) r->sessions.push_back(std::move(*up));

    Ort::AllocatorWithDefaultOptions alloc;
    r->inName = r->sessions[0].GetInputNameAllocated(0, alloc).get();
    r->outName = r->sessions[0].GetOutputNameAllocated(0, alloc).get();

    auto outShape = r->sessions[0].GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    int64_t numClasses = outShape.empty() ? -1 : outShape.back();

    r->alphabet.push_back(L"");  // CTC blank at index 0
    for (auto& l : ReadDictLines(dict)) r->alphabet.push_back(l);
    if (numClasses > 0) {
        while (static_cast<int64_t>(r->alphabet.size()) < numClasses) r->alphabet.push_back(L" ");
        if (static_cast<int64_t>(r->alphabet.size()) > numClasses) r->alphabet.resize(static_cast<size_t>(numClasses));
    } else {
        r->alphabet.push_back(L" ");
    }

    r->asciiOf.assign(r->alphabet.size(), 0);
    r->allowed.push_back(0);
    for (size_t i = 1; i < r->alphabet.size(); ++i) {
        const std::wstring& s = r->alphabet[i];
        if (s.size() != 1) continue;
        wchar_t a = ToCodeAscii(s[0]);
        if (a == 0) continue;
        r->asciiOf[i] = a;
        r->allowed.push_back(static_cast<int>(i));
    }
    return r;
}

void DoInitialize() {
    try {
        const fs::path dir = ExeDir();
        const fs::path dll = dir / L"onnxruntime.dll";
        if (!fs::exists(dll)) {
            g_initError = L"Missing OCR component: " + dll.wstring();
            return;
        }

        HMODULE hOrt = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!hOrt) {
            g_initError = L"Failed to load onnxruntime.dll (error " + std::to_wstring(GetLastError()) + L")";
            return;
        }
        using GetApiBaseFn = const OrtApiBase* (ORT_API_CALL*)();
        auto getApiBase = reinterpret_cast<GetApiBaseFn>(GetProcAddress(hOrt, "OrtGetApiBase"));
        if (!getApiBase) {
            g_initError = L"onnxruntime.dll does not export OrtGetApiBase";
            return;
        }
        const OrtApi* api = getApiBase()->GetApi(ORT_API_VERSION);
        if (!api) {
            g_initError = L"onnxruntime.dll is older than the headers this app was built with";
            return;
        }
        Ort::InitApi(api);

        auto engine = std::make_unique<Engine>();
        engine->env = Ort::Env(ORT_LOGGING_LEVEL_ERROR, "ScreenTextify");

        engine->opts = Ort::SessionOptions();
        // Each worker thread owns its own private Ort::Session with intra_op=1 and full BFCArena +
        // MemPattern enabled — zero cross-thread arena lock contention and zero heap churn.
        engine->opts.SetIntraOpNumThreads(1);
        engine->opts.SetInterOpNumThreads(1);
        engine->opts.EnableCpuMemArena();
        engine->opts.EnableMemPattern();
        engine->opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        engine->opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);

        struct Cand { const wchar_t* name; const wchar_t* model; const wchar_t* dict; };
        const Cand cands[] = {
            { L"v5", L"rec_en_v5.onnx", L"rec_en_v5_dict.txt" },
            { L"v4", L"rec_en.onnx",    L"en_dict.txt" },
        };
        for (const auto& c : cands) {
            fs::path m = dir / L"models" / c.model;
            fs::path d = dir / L"models" / c.dict;
            if (fs::exists(m) && fs::exists(d)) {
                engine->recs.push_back(LoadRecognizer(*engine, c.name, m, d));
            }
        }
        if (engine->recs.empty()) {
            g_initError = L"No recognition model found in " + (dir / L"models").wstring();
            return;
        }

        g_engine = engine.release();
        g_ready = true;
    } catch (const Ort::Exception& ex) {
        g_initError = L"ONNX Runtime error: " + Utf8ToWide(ex.what());
    } catch (const std::exception& ex) {
        g_initError = L"OCR init error: " + Utf8ToWide(ex.what());
    }
}

// Lightweight work-stealing parallel-for helper (calling thread participates as worker #0)
template <typename Fn>
void ParallelFor(size_t count, Fn&& fn) {
    if (count == 0) return;
    size_t numWorkers = OptimalWorkerCount(count);
    if (numWorkers <= 1) {
        for (size_t i = 0; i < count; ++i) fn(i);
        return;
    }

    std::atomic<size_t> nextIdx{ 0 };
    std::vector<std::thread> workers;
    workers.reserve(numWorkers - 1);
    for (size_t t = 0; t + 1 < numWorkers; ++t) {
        workers.emplace_back([&]() {
            while (true) {
                size_t idx = nextIdx.fetch_add(1, std::memory_order_relaxed);
                if (idx >= count) break;
                fn(idx);
            }
        });
    }
    while (true) {
        size_t idx = nextIdx.fetch_add(1, std::memory_order_relaxed);
        if (idx >= count) break;
        fn(idx);
    }
    for (auto& w : workers) w.join();
}

// ------------------------------------------------------------------------------------------------
// Step 1: Dual-View Preprocessing + 8-Connected Component Ink Mask
// ------------------------------------------------------------------------------------------------

struct InkComponent {
    int minX = 0, minY = 0, maxX = 0, maxY = 0;
    int pixelCount = 0;
    float topMeanX = 0.0f;    // relY in [0.00, 0.22]
    float upStemX  = 0.0f;    // relY in [0.22, 0.40]
    float midMeanX = 0.0f;    // relY in [0.40, 0.60]
    float loStemX  = 0.0f;    // relY in [0.60, 0.78]
    float botMeanX = 0.0f;    // relY in [0.78, 1.00]
    int width() const { return maxX - minX + 1; }
    int height() const { return maxY - minY + 1; }
    float centerX() const { return 0.5f * (minX + maxX); }
    float centerY() const { return 0.5f * (minY + maxY); }
};

struct PreprocessedFrame {
    RawBitmap cleanBmp;              // Contrast-equalized + sharpened black-on-white BGRA8 bitmap
    RawBitmap rawNormBmp;            // Polarity-normalized direct BGR view (for dual-view verification)
    std::vector<uint8_t> inkMask;    // 1 where foreground glyph ink is present
    std::vector<uint8_t> maxDeltaMap;// Per-pixel max RGB contrast delta from local background (for faint-line rescue)
    std::vector<InkComponent> comps; // Filtered glyph connected components
    float medGlyphH = 18.0f;
    float p90GlyphH = 22.0f;
    float scaleFactor = 1.0f;
};

void ExtractInkComponents(
    const std::vector<uint8_t>& mask,
    int srcW, int srcH,
    int yStart, int yEnd,
    std::vector<InkComponent>& outComps
) {
    outComps.clear();
    yStart = std::clamp(yStart, 0, srcH);
    yEnd = std::clamp(yEnd, yStart, srcH);
    int bandH = yEnd - yStart;
    if (bandH <= 0 || srcW <= 0) return;

    std::vector<uint8_t> visited(static_cast<size_t>(srcW) * bandH, 0);
    struct Pt { int x, y; };
    std::vector<Pt> pts;
    pts.reserve(256);

    for (int y = yStart; y < yEnd; ++y) {
        for (int x = 0; x < srcW; ++x) {
            size_t mIdx = static_cast<size_t>(y) * srcW + x;
            size_t vIdx = static_cast<size_t>(y - yStart) * srcW + x;
            if (!mask[mIdx] || visited[vIdx]) continue;
            visited[vIdx] = 1;
            pts.clear();
            pts.push_back({ x, y });

            InkComponent comp;
            comp.minX = comp.maxX = x;
            comp.minY = comp.maxY = y;

            size_t head = 0;
            while (head < pts.size()) {
                Pt cur = pts[head++];
                comp.minX = std::min(comp.minX, cur.x);
                comp.maxX = std::max(comp.maxX, cur.x);
                comp.minY = std::min(comp.minY, cur.y);
                comp.maxY = std::max(comp.maxY, cur.y);

                for (int dy = -1; dy <= 1; ++dy) {
                    int ny = cur.y + dy;
                    if (ny < yStart || ny >= yEnd) continue;
                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = cur.x + dx;
                        if (nx < 0 || nx >= srcW) continue;
                        size_t nmIdx = static_cast<size_t>(ny) * srcW + nx;
                        size_t nvIdx = static_cast<size_t>(ny - yStart) * srcW + nx;
                        if (!visited[nvIdx] && mask[nmIdx]) {
                            visited[nvIdx] = 1;
                            pts.push_back({ nx, ny });
                        }
                    }
                }
            }

            comp.pixelCount = static_cast<int>(pts.size());
            if (comp.pixelCount >= 2) {
                int h = std::max(1, comp.height());
                float sum[5] = {};
                int cnt[5] = {};
                for (const auto& p : pts) {
                    float relY = static_cast<float>(p.y - comp.minY) / static_cast<float>(h);
                    int zone = 2;
                    if (relY <= 0.22f) zone = 0;
                    else if (relY <= 0.40f) zone = 1;
                    else if (relY <= 0.60f) zone = 2;
                    else if (relY <= 0.78f) zone = 3;
                    else zone = 4;
                    sum[zone] += p.x;
                    ++cnt[zone];
                }
                float cx = comp.centerX();
                comp.topMeanX = cnt[0] > 0 ? (sum[0] / cnt[0]) : cx;
                comp.upStemX  = cnt[1] > 0 ? (sum[1] / cnt[1]) : cx;
                comp.midMeanX = cnt[2] > 0 ? (sum[2] / cnt[2]) : cx;
                comp.loStemX  = cnt[3] > 0 ? (sum[3] / cnt[3]) : cx;
                comp.botMeanX = cnt[4] > 0 ? (sum[4] / cnt[4]) : cx;
                outComps.push_back(comp);
            }
        }
    }
}

// Suppresses thin red/yellow wavy ESLint/TypeScript error squiggles sitting beneath text baselines
void SuppressEditorSquiggles(const RawBitmap& src, PreprocessedFrame& pf) {
    const int srcW = src.width;
    const int srcH = src.height;
    const float sc = std::max(1.0f, src.scaleFactor);
    const size_t total = static_cast<size_t>(srcW) * srcH;

    std::vector<uint8_t> sqMask(total, 0);
    bool anyCand = false;
    for (int y = 0; y < srcH; ++y) {
        const uint8_t* sRow = src.pixels.data() + static_cast<size_t>(y) * src.stride;
        uint8_t* qRow = sqMask.data() + static_cast<size_t>(y) * srcW;
        const uint8_t* mRow = pf.inkMask.data() + static_cast<size_t>(y) * srcW;
        for (int x = 0; x < srcW; ++x) {
            if (!mRow[x]) continue;
            int b = sRow[x * 4 + 0];
            int g = sRow[x * 4 + 1];
            int r = sRow[x * 4 + 2];
            // High-saturation error red (#F14C4C / #E51400) or warning yellow (#CCA700 / #EAB308)
            bool isErrRed = (r >= 145) && (r - std::max(g, b) >= 72);
            bool isWarnYel = (r >= 150) && (g >= 115) && (std::min(r, g) - b >= 80) && (r - g <= 65);
            if (isErrRed || isWarnYel) {
                qRow[x] = 1;
                anyCand = true;
            }
        }
    }
    if (!anyCand) return;

    const int maxSqH = std::max(4, static_cast<int>(std::ceil(3.8f * sc)));
    const int minSqW = std::max(8, static_cast<int>(10.0f * sc));
    const int lookUpMin = std::max(2, static_cast<int>(2.0f * sc));
    const int lookUpMax = std::max(10, static_cast<int>(14.0f * sc));

    std::vector<uint8_t> visited(total, 0);
    struct Pt { int x, y; };
    std::vector<Pt> pts;
    pts.reserve(256);

    for (int y = 0; y < srcH; ++y) {
        for (int x = 0; x < srcW; ++x) {
            size_t idx0 = static_cast<size_t>(y) * srcW + x;
            if (!sqMask[idx0] || visited[idx0]) continue;
            visited[idx0] = 1;
            pts.clear();
            pts.push_back({ x, y });
            int minX = x, maxX = x, minY = y, maxY = y;

            size_t head = 0;
            while (head < pts.size()) {
                Pt cur = pts[head++];
                minX = std::min(minX, cur.x);
                maxX = std::max(maxX, cur.x);
                minY = std::min(minY, cur.y);
                maxY = std::max(maxY, cur.y);
                for (int dy = -1; dy <= 1; ++dy) {
                    int ny = cur.y + dy;
                    if (ny < 0 || ny >= srcH) continue;
                    for (int dx = -2; dx <= 2; ++dx) { // 2px horiz bridge links wavy peaks
                        int nx = cur.x + dx;
                        if (nx < 0 || nx >= srcW) continue;
                        size_t nIdx = static_cast<size_t>(ny) * srcW + nx;
                        if (!visited[nIdx] && sqMask[nIdx]) {
                            visited[nIdx] = 1;
                            pts.push_back({ nx, ny });
                        }
                    }
                }
            }

            int h = maxY - minY + 1;
            int w = maxX - minX + 1;
            if (h > maxSqH || w < minSqW) continue;

            // Verify there is normal non-squiggle text ink directly above this underline band
            int aboveCols = 0;
            int yA0 = std::max(0, minY - lookUpMax);
            int yA1 = std::max(0, minY - lookUpMin);
            if (yA1 <= yA0) continue;
            for (int ax = minX; ax <= maxX; ++ax) {
                bool colHasText = false;
                for (int ay = yA0; ay < yA1; ++ay) {
                    size_t aIdx = static_cast<size_t>(ay) * srcW + ax;
                    if (pf.inkMask[aIdx] && !sqMask[aIdx]) {
                        colHasText = true;
                        break;
                    }
                }
                if (colHasText) ++aboveCols;
            }

            if (aboveCols * 10 >= w * 3) {
                // Erase squiggle pixels + immediate 1px colored anti-alias fringe from inkMask & clean/raw bitmaps
                int ey0 = std::max(0, minY - 1);
                int ey1 = std::min(srcH - 1, maxY + 1);
                int ex0 = std::max(0, minX - 1);
                int ex1 = std::min(srcW - 1, maxX + 1);
                for (int ey = ey0; ey <= ey1; ++ey) {
                    const uint8_t* sRow = src.pixels.data() + static_cast<size_t>(ey) * src.stride;
                    uint8_t* dRow = pf.cleanBmp.pixels.data() + static_cast<size_t>(ey) * pf.cleanBmp.stride;
                    uint8_t* nRow = pf.rawNormBmp.pixels.data() + static_cast<size_t>(ey) * pf.rawNormBmp.stride;
                    uint8_t* mRow = pf.inkMask.data() + static_cast<size_t>(ey) * srcW;
                    uint8_t* delRow = pf.maxDeltaMap.data() + static_cast<size_t>(ey) * srcW;
                    uint8_t* qRow = sqMask.data() + static_cast<size_t>(ey) * srcW;
                    for (int ex = ex0; ex <= ex1; ++ex) {
                        int b = sRow[ex * 4 + 0], g = sRow[ex * 4 + 1], r = sRow[ex * 4 + 2];
                        bool chromaFringe = qRow[ex] || (r - std::max(g, b) >= 38) || (std::min(r, g) - b >= 45 && r - g <= 65);
                        if (chromaFringe) {
                            mRow[ex] = 0;
                            delRow[ex] = 0;
                            dRow[ex * 4 + 0] = dRow[ex * 4 + 1] = dRow[ex * 4 + 2] = 255;
                            nRow[ex * 4 + 0] = nRow[ex * 4 + 1] = nRow[ex * 4 + 2] = 255;
                        }
                    }
                }
            }
        }
    }
}

// Detects and erases tall, ultra-thin blinking editor text cursor carets ('|') that extend beyond normal glyph height
bool SuppressEditorCursorCarets(PreprocessedFrame& pf, float p90GlyphH, float scaleFactor) {
    const int srcW = pf.cleanBmp.width;
    const int srcH = pf.cleanBmp.height;
    const int minCaretH = std::max(14, static_cast<int>(std::ceil(p90GlyphH * 1.20f)));
    const int maxCaretH = std::max(minCaretH + 8, static_cast<int>(std::ceil(p90GlyphH * 2.10f)));
    const int maxCaretW = std::max(2, static_cast<int>(std::ceil(2.4f * scaleFactor)));
    bool erasedAny = false;

    std::vector<uint8_t> caretMask(static_cast<size_t>(srcW) * srcH, 0);

    for (int x = 0; x < srcW; ++x) {
        int y = 0;
        while (y < srcH) {
            if (!pf.inkMask[static_cast<size_t>(y) * srcW + x]) {
                ++y;
                continue;
            }
            int y0 = y;
            while (y < srcH && pf.inkMask[static_cast<size_t>(y) * srcW + x]) ++y;
            int runH = y - y0;
            if (runH < minCaretH || runH > maxCaretH) continue;

            // Check horizontal extent of this vertical bar at top/bottom tips (carets are thin at both ends)
            int xL = x, xR = x;
            while (xL > 0 && (x - xL) < maxCaretW &&
                   pf.inkMask[static_cast<size_t>(y0 + 1) * srcW + (xL - 1)] &&
                   pf.inkMask[static_cast<size_t>(y - 2) * srcW + (xL - 1)]) {
                --xL;
            }
            while (xR + 1 < srcW && (xR - xL + 1) < maxCaretW &&
                   pf.inkMask[static_cast<size_t>(y0 + 1) * srcW + (xR + 1)] &&
                   pf.inkMask[static_cast<size_t>(y - 2) * srcW + (xR + 1)]) {
                ++xR;
            }
            int barW = xR - xL + 1;
            if (barW > maxCaretW) continue;

            // Verify at least one side of the vertical bar is mostly clear (protects solid graphic boxes)
            int leftClear = 0, rightClear = 0;
            for (int cy = y0; cy < y; ++cy) {
                if (xL == 0 || !pf.inkMask[static_cast<size_t>(cy) * srcW + (xL - 1)]) ++leftClear;
                if (xR + 1 >= srcW || !pf.inkMask[static_cast<size_t>(cy) * srcW + (xR + 1)]) ++rightClear;
            }
            if (std::max(leftClear, rightClear) * 100 >= runH * 75 &&
                std::min(leftClear, rightClear) * 100 >= runH * 35) {
                for (int cy = y0; cy < y; ++cy) {
                    for (int cx = xL; cx <= xR; ++cx) {
                        caretMask[static_cast<size_t>(cy) * srcW + cx] = 1;
                    }
                }
                erasedAny = true;
            }
        }
    }

    if (erasedAny) {
        for (int y = 0; y < srcH; ++y) {
            uint8_t* dRow = pf.cleanBmp.pixels.data() + static_cast<size_t>(y) * pf.cleanBmp.stride;
            uint8_t* nRow = pf.rawNormBmp.pixels.data() + static_cast<size_t>(y) * pf.rawNormBmp.stride;
            uint8_t* mRow = pf.inkMask.data() + static_cast<size_t>(y) * srcW;
            uint8_t* delRow = pf.maxDeltaMap.data() + static_cast<size_t>(y) * srcW;
            const uint8_t* cRow = caretMask.data() + static_cast<size_t>(y) * srcW;
            for (int x = 0; x < srcW; ++x) {
                if (cRow[x]) {
                    mRow[x] = 0;
                    delRow[x] = 0;
                    dRow[x * 4 + 0] = dRow[x * 4 + 1] = dRow[x * 4 + 2] = 255;
                    nRow[x * 4 + 0] = nRow[x * 4 + 1] = nRow[x * 4 + 2] = 255;
                }
            }
        }
    }
    return erasedAny;
}

PreprocessedFrame PreprocessCodeBitmap(const RawBitmap& src) {
    PreprocessedFrame pf;
    const int srcW = src.width;
    const int srcH = src.height;
    const size_t totalPixels = static_cast<size_t>(srcW) * srcH;
    const uint8_t* srcPx = src.pixels.data();

    pf.cleanBmp.width = srcW;
    pf.cleanBmp.height = srcH;
    pf.cleanBmp.stride = srcW * 4;
    pf.cleanBmp.scaleFactor = src.scaleFactor;
    pf.cleanBmp.screenOriginX = src.screenOriginX;
    pf.cleanBmp.screenOriginY = src.screenOriginY;
    pf.cleanBmp.pixels.resize(totalPixels * 4, 255);

    pf.rawNormBmp = pf.cleanBmp;
    pf.inkMask.assign(totalPixels, 0);
    pf.maxDeltaMap.assign(totalPixels, 0);
    pf.scaleFactor = std::max(1.0f, src.scaleFactor);

    uint64_t lumSum = 0;
    for (size_t i = 0; i < totalPixels; i += 4) {
        uint32_t b = srcPx[i * 4 + 0];
        uint32_t g = srcPx[i * 4 + 1];
        uint32_t r = srcPx[i * 4 + 2];
        lumSum += (r * 77 + g * 150 + b * 29) >> 8;
    }
    uint32_t avgLum = (totalPixels > 0) ? static_cast<uint32_t>(lumSum / ((totalPixels + 3) / 4)) : 128;
    bool isDarkTheme = (avgLum < 140);

    const int cellW = std::max(36, static_cast<int>(36.0f * src.scaleFactor));
    const int cellH = 2;
    const int gridW = (srcW + cellW - 1) / cellW;
    const int gridH = (srcH + cellH - 1) / cellH;

    struct RgbBg { int r, g, b; };
    std::vector<RgbBg> bgGrid(static_cast<size_t>(gridW) * gridH);

    struct SamplePx { int lum, r, g, b; };

    const int bgChunkRows = 16;
    const size_t bgChunks = static_cast<size_t>((gridH + bgChunkRows - 1) / bgChunkRows);
    ParallelFor(bgChunks, [&](size_t chunk) {
        int gyStart = static_cast<int>(chunk) * bgChunkRows;
        int gyEnd = std::min(gridH, gyStart + bgChunkRows);
        std::vector<SamplePx> cellSamples;
        cellSamples.reserve(128);

        for (int gy = gyStart; gy < gyEnd; ++gy) {
            int y0 = gy * cellH;
            int y1 = std::min(srcH, y0 + cellH);
            for (int gx = 0; gx < gridW; ++gx) {
                int x0 = std::max(0, gx * cellW - cellW / 4);
                int x1 = std::min(srcW, (gx + 1) * cellW + cellW / 4);

                cellSamples.clear();
                for (int y = y0; y < y1; ++y) {
                    const uint8_t* row = srcPx + static_cast<size_t>(y) * src.stride;
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
    });

    const int pxChunkRows = 32;
    const size_t pxChunks = static_cast<size_t>((srcH + pxChunkRows - 1) / pxChunkRows);
    ParallelFor(pxChunks, [&](size_t chunk) {
        int yStart = static_cast<int>(chunk) * pxChunkRows;
        int yEnd = std::min(srcH, yStart + pxChunkRows);
        for (int y = yStart; y < yEnd; ++y) {
            int gy = std::clamp(y / cellH, 0, gridH - 1);
            const uint8_t* sRow = srcPx + static_cast<size_t>(y) * src.stride;
            uint8_t* dRow = pf.cleanBmp.pixels.data() + static_cast<size_t>(y) * pf.cleanBmp.stride;
            uint8_t* nRow = pf.rawNormBmp.pixels.data() + static_cast<size_t>(y) * pf.rawNormBmp.stride;
            uint8_t* mRow = pf.inkMask.data() + static_cast<size_t>(y) * srcW;
            uint8_t* delRow = pf.maxDeltaMap.data() + static_cast<size_t>(y) * srcW;

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
                uint8_t gentleInk = 255;
                int maxD = 0;
                if (isDarkTheme) {
                    int dR = std::max(0, r - bgR);
                    int dG = std::max(0, g - bgG);
                    int dB = std::max(0, b - bgB);
                    maxD = std::max({ dR, dG, dB });
                    int lumD = (dR * 77 + dG * 150 + dB * 29) >> 8;
                    int delta = (maxD * 2 + lumD) / 3;

                    if (delta > 19) {
                        float t = std::clamp(static_cast<float>(delta - 19) / 100.0f, 0.0f, 1.0f);
                        float smooth = t * t * (3.0f - 2.0f * t);
                        ink = static_cast<uint8_t>(std::clamp(255.0f * (1.0f - smooth), 0.0f, 255.0f));
                    }
                    if (delta > 12) {
                        float tg = std::clamp(static_cast<float>(delta - 12) / 145.0f, 0.0f, 1.0f);
                        gentleInk = static_cast<uint8_t>(std::clamp(255.0f * (1.0f - tg), 0.0f, 255.0f));
                    }
                } else {
                    int dR = std::max(0, bgR - r);
                    int dG = std::max(0, bgG - g);
                    int dB = std::max(0, bgB - b);
                    maxD = std::max({ dR, dG, dB });

                    if (maxD > 17) {
                        float t = std::clamp(static_cast<float>(maxD - 17) / 104.0f, 0.0f, 1.0f);
                        float smooth = t * t * (3.0f - 2.0f * t);
                        ink = static_cast<uint8_t>(std::clamp(255.0f * (1.0f - smooth), 0.0f, 255.0f));
                    }
                    if (maxD > 10) {
                        float tg = std::clamp(static_cast<float>(maxD - 10) / 145.0f, 0.0f, 1.0f);
                        gentleInk = static_cast<uint8_t>(std::clamp(255.0f * (1.0f - tg), 0.0f, 255.0f));
                    }
                }

                dRow[x * 4 + 0] = ink;
                dRow[x * 4 + 1] = ink;
                dRow[x * 4 + 2] = ink;
                dRow[x * 4 + 3] = 255;

                nRow[x * 4 + 0] = gentleInk;
                nRow[x * 4 + 1] = gentleInk;
                nRow[x * 4 + 2] = gentleInk;
                nRow[x * 4 + 3] = 255;

                delRow[x] = static_cast<uint8_t>(std::clamp(maxD, 0, 255));
                if (maxD >= 36 || ink < 190) {
                    mRow[x] = 1;
                }
            }
        }
    });

    // Suppress thin red/yellow wavy ESLint/TypeScript squiggles before 8-CC segmentation
    SuppressEditorSquiggles(src, pf);

    // Extract 8-connected components from inkMask with 5-zone vertical curvature profiles
    std::vector<InkComponent> rawComps;
    ExtractInkComponents(pf.inkMask, srcW, srcH, 0, srcH, rawComps);

    std::vector<int> heights;
    heights.reserve(rawComps.size());
    int minNormalH = std::max(6, static_cast<int>(7.0f * src.scaleFactor));
    int maxNormalH = std::max(36, static_cast<int>(38.0f * src.scaleFactor));
    for (const auto& c : rawComps) {
        if (c.height() >= minNormalH && c.height() <= maxNormalH && c.width() <= maxNormalH * 2) {
            heights.push_back(c.height());
        }
    }
    if (!heights.empty()) {
        std::sort(heights.begin(), heights.end());
        pf.medGlyphH = static_cast<float>(heights[heights.size() / 2]);
        pf.p90GlyphH = static_cast<float>(heights[(heights.size() * 9) / 10]);
    } else {
        pf.medGlyphH = 15.0f * src.scaleFactor;
        pf.p90GlyphH = 19.0f * src.scaleFactor;
    }

    // Remove tall blinking vertical editor cursor carets ('|') and re-extract components if any were found
    if (SuppressEditorCursorCarets(pf, pf.p90GlyphH, pf.scaleFactor)) {
        ExtractInkComponents(pf.inkMask, srcW, srcH, 0, srcH, rawComps);
    }

    pf.comps.reserve(rawComps.size());
    for (const auto& c : rawComps) {
        if (c.height() > static_cast<int>(pf.medGlyphH * 1.85f) && c.width() <= static_cast<int>(4.5f * src.scaleFactor)) {
            continue;
        }
        if (c.width() > static_cast<int>(pf.medGlyphH * 10.0f) && c.height() <= static_cast<int>(3.5f * src.scaleFactor)) {
            continue;
        }
        if (c.height() > static_cast<int>(pf.medGlyphH * 3.0f)) {
            continue;
        }
        pf.comps.push_back(c);
    }

    return pf;
}

// ------------------------------------------------------------------------------------------------
// Image resampling into CHW float tensor (with out-of-bounds white padding support)
// ------------------------------------------------------------------------------------------------

void ResampleToTensor(const RawBitmap& src, float rx, float ry, float rw, float rh,
                      int dstW, int dstH, int strideW, size_t planeSize, float* dst,
                      const float mean[3], const float stdv[3], bool invert) {
    const float sx = rw / static_cast<float>(dstW);
    const float sy = rh / static_cast<float>(dstH);

    float scale[3], bias[3];
    for (int c = 0; c < 3; ++c) {
        scale[c] = 1.0f / (255.0f * stdv[c]);
        bias[c] = -mean[c] / stdv[c];
    }

    for (int y = 0; y < dstH; ++y) {
        float fy = ry + (static_cast<float>(y) + 0.5f) * sy - 0.5f;
        float* d0 = dst + static_cast<size_t>(y) * strideW;

        if (fy < 0.0f || fy > static_cast<float>(src.height - 1)) {
            for (int c = 0; c < 3; ++c) {
                float whiteVal = 255.0f * scale[c] + bias[c];
                std::fill(d0 + static_cast<size_t>(c) * planeSize,
                          d0 + static_cast<size_t>(c) * planeSize + dstW, whiteVal);
            }
            continue;
        }

        int iy0 = std::clamp(static_cast<int>(std::floor(fy)), 0, src.height - 1);
        int iy1 = std::min(iy0 + 1, src.height - 1);
        float wy = fy - static_cast<float>(iy0);
        const uint8_t* r0 = src.pixels.data() + static_cast<size_t>(iy0) * src.stride;
        const uint8_t* r1 = src.pixels.data() + static_cast<size_t>(iy1) * src.stride;

        for (int x = 0; x < dstW; ++x) {
            float fx = rx + (static_cast<float>(x) + 0.5f) * sx - 0.5f;
            if (fx < 0.0f || fx > static_cast<float>(src.width - 1)) {
                for (int c = 0; c < 3; ++c) {
                    d0[static_cast<size_t>(c) * planeSize + x] = 255.0f * scale[c] + bias[c];
                }
                continue;
            }
            int ix0 = std::clamp(static_cast<int>(std::floor(fx)), 0, src.width - 1);
            int ix1 = std::min(ix0 + 1, src.width - 1);
            float wx = fx - static_cast<float>(ix0);

            const uint8_t* p00 = r0 + ix0 * 4;
            const uint8_t* p01 = r0 + ix1 * 4;
            const uint8_t* p10 = r1 + ix0 * 4;
            const uint8_t* p11 = r1 + ix1 * 4;
            for (int c = 0; c < 3; ++c) {
                float top = p00[c] + (p01[c] - p00[c]) * wx;
                float bot = p10[c] + (p11[c] - p10[c]) * wx;
                float v = top + (bot - top) * wy;
                if (invert) v = 255.0f - v;
                d0[static_cast<size_t>(c) * planeSize + x] = v * scale[c] + bias[c];
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Step 2: 2D Connected-Component Line & Clause Detector
// ------------------------------------------------------------------------------------------------

struct DetBox {
    float x0, y0, x1, y1;  // tight ink bounds in bitmap pixels
    std::vector<InkComponent> comps;
};

std::vector<DetBox> BuildComponentLineBoxes(PreprocessedFrame& pf) {
    float refH = std::max(10.0f, pf.medGlyphH);

    std::vector<const InkComponent*> seedComps;
    std::vector<const InkComponent*> smallComps;
    for (const auto& c : pf.comps) {
        if (c.height() >= static_cast<int>(refH * 0.42f)) {
            seedComps.push_back(&c);
        } else {
            smallComps.push_back(&c);
        }
    }

    std::sort(seedComps.begin(), seedComps.end(), [](const InkComponent* a, const InkComponent* b) {
        return a->centerY() < b->centerY();
    });

    struct RowBand {
        int minY = 100000, maxY = -1;
        float sumCy = 0.0f;
        int seedCount = 0;
        std::vector<const InkComponent*> comps;
        float meanCy() const { return seedCount > 0 ? (sumCy / seedCount) : (0.5f * (minY + maxY)); }
    };
    std::vector<RowBand> rows;

    for (const auto* sc : seedComps) {
        RowBand* bestRow = nullptr;
        float bestDist = 1e9f;
        for (auto& r : rows) {
            float cyDist = std::abs(sc->centerY() - r.meanCy());
            int overlapY = std::min(sc->maxY, r.maxY) - std::max(sc->minY, r.minY) + 1;
            int minH = std::min(sc->height(), r.maxY - r.minY + 1);
            if ((cyDist <= refH * 0.55f || overlapY >= static_cast<int>(minH * 0.45f)) && cyDist < bestDist) {
                bestDist = cyDist;
                bestRow = &r;
            }
        }
        if (!bestRow) {
            RowBand nr;
            nr.minY = sc->minY;
            nr.maxY = sc->maxY;
            nr.sumCy = sc->centerY();
            nr.seedCount = 1;
            nr.comps.push_back(sc);
            rows.push_back(std::move(nr));
        } else {
            bestRow->minY = std::min(bestRow->minY, sc->minY);
            bestRow->maxY = std::max(bestRow->maxY, sc->maxY);
            bestRow->sumCy += sc->centerY();
            bestRow->seedCount += 1;
            bestRow->comps.push_back(sc);
        }
    }

    for (const auto* sm : smallComps) {
        RowBand* targetRow = nullptr;
        float bestDist = 1e9f;
        for (auto& r : rows) {
            if (sm->minY >= r.minY - static_cast<int>(refH * 0.25f) &&
                sm->maxY <= r.maxY + static_cast<int>(refH * 0.35f)) {
                float d = std::abs(sm->centerY() - r.meanCy());
                if (d < bestDist) {
                    bestDist = d;
                    targetRow = &r;
                }
            }
        }
        if (targetRow) {
            targetRow->minY = std::min(targetRow->minY, sm->minY);
            targetRow->maxY = std::max(targetRow->maxY, sm->maxY);
            targetRow->comps.push_back(sm);
        }
    }

    std::sort(rows.begin(), rows.end(), [](const RowBand& a, const RowBand& b) {
        return a.meanCy() < b.meanCy();
    });

    std::vector<DetBox> boxes;
    float maxIntraLineGap = refH * 3.2f;

    for (auto& r : rows) {
        if (r.comps.empty()) continue;
        std::sort(r.comps.begin(), r.comps.end(), [](const InkComponent* a, const InkComponent* b) {
            if (std::abs(a->minX - b->minX) > 3) return a->minX < b->minX;
            return a->minY < b->minY;
        });

        DetBox cur;
        cur.x0 = static_cast<float>(r.comps[0]->minX);
        cur.y0 = static_cast<float>(r.comps[0]->minY);
        cur.x1 = static_cast<float>(r.comps[0]->maxX + 1);
        cur.y1 = static_cast<float>(r.comps[0]->maxY + 1);
        cur.comps.push_back(*r.comps[0]);

        for (size_t i = 1; i < r.comps.size(); ++i) {
            const auto* c = r.comps[i];
            float gap = static_cast<float>(c->minX) - cur.x1;
            if (gap <= maxIntraLineGap) {
                cur.x0 = std::min(cur.x0, static_cast<float>(c->minX));
                cur.y0 = std::min(cur.y0, static_cast<float>(c->minY));
                cur.x1 = std::max(cur.x1, static_cast<float>(c->maxX + 1));
                cur.y1 = std::max(cur.y1, static_cast<float>(c->maxY + 1));
                cur.comps.push_back(*c);
            } else {
                if (cur.y1 - cur.y0 >= refH * 0.40f) {
                    boxes.push_back(std::move(cur));
                }
                cur = DetBox{};
                cur.x0 = static_cast<float>(c->minX);
                cur.y0 = static_cast<float>(c->minY);
                cur.x1 = static_cast<float>(c->maxX + 1);
                cur.y1 = static_cast<float>(c->maxY + 1);
                cur.comps.push_back(*c);
            }
        }
        if (cur.y1 - cur.y0 >= refH * 0.40f) {
            boxes.push_back(std::move(cur));
        }
    }

    // Empty-Band Adaptive Faint-Line Rescue:
    // Inspect vertical gaps between detected rows using a lower contrast threshold (maxDelta >= 17)
    // so dimmed VS Code comments (#545862) and faded unused imports are never silently dropped.
    const int srcW = pf.cleanBmp.width;
    const int srcH = pf.cleanBmp.height;
    if (!pf.maxDeltaMap.empty() && srcW > 0 && srcH > 0) {
        std::vector<std::pair<int, int>> occupied;
        occupied.reserve(rows.size());
        for (const auto& r : rows) {
            occupied.push_back({ std::max(0, r.minY - 2), std::min(srcH, r.maxY + 3) });
        }
        std::sort(occupied.begin(), occupied.end());

        std::vector<std::pair<int, int>> gaps;
        int curTop = 0;
        for (const auto& occ : occupied) {
            if (occ.first - curTop >= static_cast<int>(refH * 1.15f)) {
                gaps.push_back({ curTop, occ.first });
            }
            curTop = std::max(curTop, occ.second);
        }
        if (srcH - curTop >= static_cast<int>(refH * 1.15f)) {
            gaps.push_back({ curTop, srcH });
        }

        if (!gaps.empty()) {
            std::vector<uint8_t> faintMask(static_cast<size_t>(srcW) * srcH, 0);
            const int minFaintPx = std::max(3, static_cast<int>(3.0f * pf.scaleFactor));
            bool addedAny = false;

            for (const auto& gp : gaps) {
                int gy0 = std::clamp(gp.first + 2, 0, srcH);
                int gy1 = std::clamp(gp.second - 2, gy0, srcH);
                if (gy1 - gy0 < static_cast<int>(refH * 0.65f)) continue;

                int faintPixels = 0;
                for (int y = gy0; y < gy1; ++y) {
                    const uint8_t* delRow = pf.maxDeltaMap.data() + static_cast<size_t>(y) * srcW;
                    uint8_t* fmRow = faintMask.data() + static_cast<size_t>(y) * srcW;
                    for (int x = 0; x < srcW; ++x) {
                        if (delRow[x] >= 17) {
                            fmRow[x] = 1;
                            ++faintPixels;
                        }
                    }
                }
                if (faintPixels < minFaintPx * 4) continue;

                std::vector<InkComponent> rawFaint;
                ExtractInkComponents(faintMask, srcW, srcH, gy0, gy1, rawFaint);

                std::vector<InkComponent> validFaint;
                for (const auto& c : rawFaint) {
                    if (c.pixelCount < minFaintPx) continue;
                    if (c.height() > static_cast<int>(refH * 2.2f)) continue;
                    if (c.width() > static_cast<int>(refH * 8.0f) && c.height() <= static_cast<int>(3.5f * pf.scaleFactor)) continue;
                    validFaint.push_back(c);
                }
                if (validFaint.size() < 4) continue;

                std::vector<RowBand> faintRows;
                std::sort(validFaint.begin(), validFaint.end(), [](const InkComponent& a, const InkComponent& b) {
                    return a.centerY() < b.centerY();
                });
                for (const auto& fc : validFaint) {
                    if (fc.height() < static_cast<int>(refH * 0.35f)) continue;
                    RowBand* best = nullptr;
                    float bestD = 1e9f;
                    for (auto& fr : faintRows) {
                        float d = std::abs(fc.centerY() - fr.meanCy());
                        if (d <= refH * 0.55f && d < bestD) {
                            bestD = d;
                            best = &fr;
                        }
                    }
                    if (!best) {
                        RowBand nr;
                        nr.minY = fc.minY; nr.maxY = fc.maxY;
                        nr.sumCy = fc.centerY(); nr.seedCount = 1;
                        nr.comps.push_back(&fc);
                        faintRows.push_back(std::move(nr));
                    } else {
                        best->minY = std::min(best->minY, fc.minY);
                        best->maxY = std::max(best->maxY, fc.maxY);
                        best->sumCy += fc.centerY();
                        best->seedCount += 1;
                        best->comps.push_back(&fc);
                    }
                }
                // Attach small punctuation in the faint band (dots, slashes, underscores)
                for (const auto& fc : validFaint) {
                    if (fc.height() >= static_cast<int>(refH * 0.35f)) continue;
                    for (auto& fr : faintRows) {
                        if (fc.minY >= fr.minY - static_cast<int>(refH * 0.25f) &&
                            fc.maxY <= fr.maxY + static_cast<int>(refH * 0.35f)) {
                            fr.minY = std::min(fr.minY, fc.minY);
                            fr.maxY = std::max(fr.maxY, fc.maxY);
                            fr.comps.push_back(&fc);
                            break;
                        }
                    }
                }

                for (auto& fr : faintRows) {
                    if (fr.comps.size() < 4) continue;
                    std::sort(fr.comps.begin(), fr.comps.end(), [](const InkComponent* a, const InkComponent* b) {
                        return a->minX < b->minX;
                    });
                    DetBox fb;
                    fb.x0 = static_cast<float>(fr.comps.front()->minX);
                    fb.x1 = static_cast<float>(fr.comps.front()->maxX + 1);
                    fb.y0 = static_cast<float>(fr.minY);
                    fb.y1 = static_cast<float>(fr.maxY + 1);
                    for (const auto* c : fr.comps) {
                        fb.x0 = std::min(fb.x0, static_cast<float>(c->minX));
                        fb.x1 = std::max(fb.x1, static_cast<float>(c->maxX + 1));
                        fb.y0 = std::min(fb.y0, static_cast<float>(c->minY));
                        fb.y1 = std::max(fb.y1, static_cast<float>(c->maxY + 1));
                        fb.comps.push_back(*c);
                    }
                    if (fb.x1 - fb.x0 < refH * 1.8f || fb.y1 - fb.y0 < refH * 0.45f) continue;

                    // Locally stretch faint-line contrast in cleanBmp & rawNormBmp to crisp black-on-white
                    int bx0 = std::clamp(static_cast<int>(fb.x0) - 4, 0, srcW - 1);
                    int bx1 = std::clamp(static_cast<int>(fb.x1) + 4, bx0 + 1, srcW);
                    int by0 = std::clamp(static_cast<int>(fb.y0) - 3, 0, srcH - 1);
                    int by1 = std::clamp(static_cast<int>(fb.y1) + 3, by0 + 1, srcH);

                    int peakDelta = 38;
                    for (int y = by0; y < by1; ++y) {
                        const uint8_t* delRow = pf.maxDeltaMap.data() + static_cast<size_t>(y) * srcW;
                        for (int x = bx0; x < bx1; ++x) peakDelta = std::max<int>(peakDelta, delRow[x]);
                    }
                    float span = static_cast<float>(std::max(24, peakDelta - 10));

                    for (int y = by0; y < by1; ++y) {
                        const uint8_t* delRow = pf.maxDeltaMap.data() + static_cast<size_t>(y) * srcW;
                        uint8_t* dRow = pf.cleanBmp.pixels.data() + static_cast<size_t>(y) * pf.cleanBmp.stride;
                        uint8_t* nRow = pf.rawNormBmp.pixels.data() + static_cast<size_t>(y) * pf.rawNormBmp.stride;
                        for (int x = bx0; x < bx1; ++x) {
                            int d = delRow[x];
                            if (d > 10) {
                                float t = std::clamp(static_cast<float>(d - 10) / span, 0.0f, 1.0f);
                                float smooth = t * t * (3.0f - 2.0f * t);
                                uint8_t ink = static_cast<uint8_t>(std::clamp(255.0f * (1.0f - smooth), 0.0f, 255.0f));
                                dRow[x * 4 + 0] = dRow[x * 4 + 1] = dRow[x * 4 + 2] = ink;
                                nRow[x * 4 + 0] = nRow[x * 4 + 1] = nRow[x * 4 + 2] = ink;
                            }
                        }
                    }

                    boxes.push_back(std::move(fb));
                    addedAny = true;
                }
            }

            if (addedAny) {
                std::sort(boxes.begin(), boxes.end(), [](const DetBox& a, const DetBox& b) {
                    float cyA = 0.5f * (a.y0 + a.y1);
                    float cyB = 0.5f * (b.y0 + b.y1);
                    if (std::abs(cyA - cyB) > 6.0f) return cyA < cyB;
                    return a.x0 < b.x0;
                });
            }
        }
    }

    return boxes;
}

// ------------------------------------------------------------------------------------------------
// Step 3: Recognition (ASCII-Constrained CTC + Adaptive Dual-Model/Dual-View Ensemble + Topology)
// ------------------------------------------------------------------------------------------------

struct RecChar {
    wchar_t ch;
    float x;     // bitmap pixels
    float prob;
};

struct RecResult {
    std::vector<RecChar> chars;
    float meanProb = 0.0f;
    float minProb = 1.0f;
};

int GetVerticalBracketCurvature(const InkComponent& c, float refH) {
    if (c.height() < static_cast<int>(refH * 0.55f) || c.width() > static_cast<int>(refH * 0.85f)) {
        return 0;
    }
    float endsX = 0.5f * (c.topMeanX + c.botMeanX);
    float diff = c.midMeanX - endsX;
    if (diff > 0.28f) return +1;   // curves right: '}' or ')'
    if (diff < -0.28f) return -1;  // curves left:  '{' or '('
    return 0;
}

// Distinguishes curly brace '{'/'}' from smooth parenthesis '('/')' using 5-zone stem vs tip profile:
// In '{' (curv < 0), the middle point juts left of the upper/lower stems (midMeanX < stemsX), while
// the top/bottom hooks curve right of the stems (endsX > stemsX).
bool IsCurlyBraceShape(const InkComponent& c, int curv) {
    float stemsX = 0.5f * (c.upStemX + c.loStemX);
    float endsX = 0.5f * (c.topMeanX + c.botMeanX);
    if (curv < 0) {
        return (c.midMeanX < stemsX - 0.12f) && (endsX > stemsX + 0.12f);
    }
    if (curv > 0) {
        return (c.midMeanX > stemsX + 0.12f) && (endsX < stemsX - 0.12f);
    }
    return false;
}

// Checks whether a horizontal region around xCenter contains two vertically stacked horizontal bars ('=')
bool HasTwoHorizontalBarsAt(const DetBox& bx, const PreprocessedFrame& pf, float xCenter) {
    // 1. Check 8-connected components in bx.comps for two vertically stacked bars at xCenter
    float tol = std::max(5.0f, pf.medGlyphH * 0.42f);
    std::vector<const InkComponent*> localComps;
    for (const auto& c : bx.comps) {
        if (std::abs(c.centerX() - xCenter) <= tol &&
            c.height() <= static_cast<int>(pf.medGlyphH * 0.55f) &&
            c.width() >= static_cast<int>(pf.medGlyphH * 0.18f)) {
            localComps.push_back(&c);
        }
    }
    for (size_t a = 0; a < localComps.size(); ++a) {
        for (size_t b = a + 1; b < localComps.size(); ++b) {
            const auto* c1 = localComps[a];
            const auto* c2 = localComps[b];
            int overlapX = std::min(c1->maxX, c2->maxX) - std::max(c1->minX, c2->minX) + 1;
            int minW = std::min(c1->width(), c2->width());
            if (overlapX >= std::max(1, minW / 2) && std::abs(c1->centerY() - c2->centerY()) >= 2.0f) {
                return true;
            }
        }
    }

    // 2. Also check grayscale vertical profile in cleanBmp for two dark horizontal bars separated by a lighter valley
    int cx = static_cast<int>(std::round(xCenter));
    int rx0 = std::clamp(cx - static_cast<int>(pf.medGlyphH * 0.28f), 0, pf.cleanBmp.width - 1);
    int rx1 = std::clamp(cx + static_cast<int>(pf.medGlyphH * 0.28f), rx0 + 1, pf.cleanBmp.width);
    int ry0 = std::clamp(static_cast<int>(bx.y0), 0, pf.cleanBmp.height - 1);
    int ry1 = std::clamp(static_cast<int>(bx.y1), ry0 + 1, pf.cleanBmp.height);

    std::vector<int> rowMinInk(ry1 - ry0, 255);
    int runs = 0;
    bool inRun = false;
    for (int y = ry0; y < ry1; ++y) {
        const uint8_t* cRow = pf.cleanBmp.pixels.data() + static_cast<size_t>(y) * pf.cleanBmp.stride;
        int darkCount = 0;
        int minVal = 255;
        for (int x = rx0; x < rx1; ++x) {
            int v = cRow[x * 4];
            minVal = std::min(minVal, v);
            if (v < 155) ++darkCount;
        }
        rowMinInk[y - ry0] = minVal;
        if (darkCount >= 2) {
            if (!inRun) { ++runs; inRun = true; }
        } else {
            inRun = false;
        }
    }
    if (runs >= 2) return true;

    // Check for a lighter valley row between two darker bar rows even if anti-aliasing bridged them
    for (int r = 1; r + 1 < static_cast<int>(rowMinInk.size()); ++r) {
        int topDark = 255, botDark = 255;
        for (int t = 0; t < r; ++t) topDark = std::min(topDark, rowMinInk[t]);
        for (int b = r + 1; b < static_cast<int>(rowMinInk.size()); ++b) botDark = std::min(botDark, rowMinInk[b]);
        if (topDark < 110 && botDark < 110 && rowMinInk[r] >= std::max(topDark, botDark) + 45) {
            return true;
        }
    }
    return false;
}

RecResult DecodeBoxWithRecognizer(Recognizer& rec, size_t sessionSlot, const RawBitmap& viewBmp, const DetBox& bx,
                                  bool constrainAscii, Ort::MemoryInfo& memInfo,
                                  std::vector<float>& inputBuf) {
    RecResult res;
    float tightW = bx.x1 - bx.x0;
    float tightH = bx.y1 - bx.y0;

    float padY = std::max(3.0f, tightH * 0.22f);
    float padX = std::max(6.0f, tightH * 0.45f);
    float cropX = bx.x0 - padX;
    float cropY = bx.y0 - padY;
    float cropW = tightW + padX * 2.0f;
    float cropH = tightH + padY * 2.0f;

    int rw = std::clamp(static_cast<int>(std::ceil(kRecHeight * cropW / std::max(1.0f, cropH))), 24, kRecMaxWidth);
    int batchW = std::max(kRecMinWidth, ((rw + 7) / 8) * 8);

    const size_t plane = static_cast<size_t>(batchW) * kRecHeight;
    inputBuf.assign(3 * plane, 1.0f);  // +1.0f = pure white background

    const float mean[3] = { 0.5f, 0.5f, 0.5f };
    const float stdv[3] = { 0.5f, 0.5f, 0.5f };
    ResampleToTensor(viewBmp, cropX, cropY, cropW, cropH, rw, kRecHeight, batchW, plane,
                     inputBuf.data(), mean, stdv, false);

    std::array<int64_t, 4> shape{ 1, 3, kRecHeight, batchW };
    Ort::Value inTensor = Ort::Value::CreateTensor<float>(memInfo, inputBuf.data(), inputBuf.size(), shape.data(), shape.size());
    const char* inNames[] = { rec.inName.c_str() };
    const char* outNames[] = { rec.outName.c_str() };
    auto& sess = rec.sessions[sessionSlot % rec.sessions.size()];
    auto outputs = sess.Run(Ort::RunOptions{ nullptr }, inNames, &inTensor, 1, outNames, 1);

    auto outShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
    const int T = static_cast<int>(outShape[1]);
    const int C = static_cast<int>(outShape[2]);
    const float* out = outputs[0].GetTensorData<float>();
    const float stepPx = static_cast<float>(batchW) / static_cast<float>(T);
    const float toBitmap = cropW / static_cast<float>(rw);

    int prev = 0;
    double probSum = 0.0;
    float minP = 1.0f;

    for (int t = 0; t < T; ++t) {
        const float* row = out + static_cast<size_t>(t) * C;
        int best = 0;
        float bestVal = row[0];
        if (constrainAscii && !rec.allowed.empty()) {
            for (int idx : rec.allowed) {
                if (idx < C && row[idx] > bestVal) {
                    bestVal = row[idx];
                    best = idx;
                }
            }
        } else {
            best = static_cast<int>(std::max_element(row, row + C) - row);
            bestVal = row[best];
        }

        if (best != 0 && best != prev && best < static_cast<int>(rec.alphabet.size())) {
            wchar_t ch = constrainAscii ? rec.asciiOf[static_cast<size_t>(best)] :
                         (rec.alphabet[static_cast<size_t>(best)].empty() ? 0 : rec.alphabet[static_cast<size_t>(best)][0]);
            if (ch != 0) {
                float xIn = (static_cast<float>(t) + 0.5f) * stepPx;
                float bmpX = std::clamp(cropX + xIn * toBitmap, bx.x0, bx.x1);
                res.chars.push_back({ ch, bmpX, bestVal });
                probSum += bestVal;
                if (ch != L' ') minP = std::min(minP, bestVal);
            }
        }
        prev = best;
    }
    res.meanProb = res.chars.empty() ? 0.0f : static_cast<float>(probSum / res.chars.size());
    res.minProb = res.chars.empty() ? 0.0f : minP;
    return res;
}

// Word-level confidence fusion between v4 (strong camelCase & leading-dot retention) and v5 (strong digit/symbol head)
RecResult FuseRecResultsByWord(const RecResult& v4Res, const RecResult& v5Res) {
    if (v4Res.chars.empty()) return v5Res;
    if (v5Res.chars.empty()) return v4Res;

    struct WordTok {
        std::vector<RecChar> chars;
        float meanProb = 0.0f;
        float minProb = 1.0f;
        std::wstring text() const {
            std::wstring s;
            for (const auto& c : chars) s += c.ch;
            return s;
        }
    };

    auto splitWords = [](const RecResult& rr) {
        std::vector<WordTok> out;
        size_t i = 0;
        while (i < rr.chars.size()) {
            while (i < rr.chars.size() && rr.chars[i].ch == L' ') ++i;
            if (i >= rr.chars.size()) break;
            WordTok w;
            double sum = 0.0;
            while (i < rr.chars.size() && rr.chars[i].ch != L' ') {
                w.chars.push_back(rr.chars[i]);
                sum += rr.chars[i].prob;
                w.minProb = std::min(w.minProb, rr.chars[i].prob);
                ++i;
            }
            w.meanProb = static_cast<float>(sum / w.chars.size());
            out.push_back(std::move(w));
        }
        return out;
    };

    auto w4 = splitWords(v4Res);
    auto w5 = splitWords(v5Res);
    if (w4.size() != w5.size() || w4.empty()) {
        RecResult out = (v5Res.meanProb > v4Res.meanProb + 0.035f) ? v5Res : v4Res;
        if (!v4Res.chars.empty() && v4Res.chars.front().ch == L'.' &&
            !out.chars.empty() && out.chars.front().ch != L'.') {
            out.chars.insert(out.chars.begin(), v4Res.chars.front());
        }
        return out;
    }

    RecResult fused;
    double totalProb = 0.0;
    float minP = 1.0f;

    for (size_t k = 0; k < w4.size(); ++k) {
        if (k > 0) {
            float spX = 0.5f * (fused.chars.back().x + w4[k].chars.front().x);
            fused.chars.push_back({ L' ', spX, 0.99f });
            totalProb += 0.99;
        }

        std::wstring s4 = w4[k].text();
        std::wstring s5 = w5[k].text();
        const WordTok* chosen = &w4[k];

        if (s4 != s5) {
            bool sameCaseInsensitive = (s4.size() == s5.size());
            if (sameCaseInsensitive) {
                for (size_t c = 0; c < s4.size(); ++c) {
                    if (std::towlower(s4[c]) != std::towlower(s5[c])) {
                        sameCaseInsensitive = false;
                        break;
                    }
                }
            }

            if (sameCaseInsensitive) {
                // Check if s4 has genuine interior camelCase (e.g. 'useState', 'useLoginStore') that s5 lowercased
                bool s4HasInteriorCamel = false;
                for (size_t c = 1; c + 1 < s4.size(); ++c) {
                    if (std::iswlower(s4[c - 1]) && std::iswupper(s4[c]) && std::iswlower(s4[c + 1])) {
                        s4HasInteriorCamel = true;
                        break;
                    }
                }
                if (s4HasInteriorCamel) chosen = &w4[k];
                else chosen = &w5[k];
            } else {
                // Keep leading '.' if v4 detected it and v5 dropped it (e.g. '.populate' vs 'populate')
                if (s4.size() == s5.size() + 1 && s4.front() == L'.' && s4.substr(1) == s5) {
                    chosen = &w4[k];
                } else if (w4[k].minProb > w5[k].minProb + 0.08f && w4[k].meanProb > w5[k].meanProb + 0.04f &&
                           !(s5.find(L'0') != std::wstring::npos && s4.find(L'e') != std::wstring::npos)) {
                    chosen = &w4[k];
                } else {
                    chosen = &w5[k];
                }
            }
        }

        for (const auto& rc : chosen->chars) {
            fused.chars.push_back(rc);
            totalProb += rc.prob;
            minP = std::min(minP, rc.prob);
        }
    }

    fused.meanProb = fused.chars.empty() ? 0.0f : static_cast<float>(totalProb / fused.chars.size());
    fused.minProb = fused.chars.empty() ? 0.0f : minP;
    return fused;
}

std::vector<RecResult> RunRecognition(Engine& e, const PreprocessedFrame& pf, const std::vector<DetBox>& boxes) {
    std::vector<RecResult> results(boxes.size());
    if (boxes.empty() || e.recs.empty()) return results;

    PaddleOcr::Options opts = PaddleOcr::GetOptions();
    Recognizer* recV4 = e.Find(L"v4");
    Recognizer* recV5 = e.Find(L"v5");

    // Stage A: Parallel Multi-Threaded Neural Inference with LPT (Longest-Processing-Time-First) Work-Stealing
    // across private per-worker Ort::Sessions (zero BFCArena contention).
    std::vector<size_t> order(boxes.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return (boxes[a].x1 - boxes[a].x0) > (boxes[b].x1 - boxes[b].x0);
    });

    auto inferOneBox = [&](size_t boxIdx, size_t workerId, Ort::MemoryInfo& memInfo, std::vector<float>& inputBuf) {
        const DetBox& bx = boxes[boxIdx];
        RecResult res;
        if (opts.model == L"v5" && recV5) {
            res = DecodeBoxWithRecognizer(*recV5, workerId, pf.cleanBmp, bx, opts.constrainToCodeAscii, memInfo, inputBuf);
        } else if (opts.model == L"v4" && recV4) {
            res = DecodeBoxWithRecognizer(*recV4, workerId, pf.cleanBmp, bx, opts.constrainToCodeAscii, memInfo, inputBuf);
        } else if (recV4 && recV5) {
            RecResult r4 = DecodeBoxWithRecognizer(*recV4, workerId, pf.cleanBmp, bx, opts.constrainToCodeAscii, memInfo, inputBuf);
            if (r4.minProb < 0.93f || r4.meanProb < 0.97f) {
                RecResult r5 = DecodeBoxWithRecognizer(*recV5, workerId, pf.cleanBmp, bx, opts.constrainToCodeAscii, memInfo, inputBuf);
                res = FuseRecResultsByWord(r4, r5);
            } else {
                res = std::move(r4);
            }
            if (opts.dualViewVerify && (res.meanProb < 0.88f || res.minProb < 0.58f)) {
                RecResult rawCand = DecodeBoxWithRecognizer(*recV5, workerId, pf.rawNormBmp, bx, opts.constrainToCodeAscii, memInfo, inputBuf);
                if (rawCand.meanProb > res.meanProb + 0.03f && rawCand.chars.size() >= res.chars.size()) {
                    res = std::move(rawCand);
                }
            }
        } else {
            res = DecodeBoxWithRecognizer(*e.recs.front(), workerId, pf.cleanBmp, bx, opts.constrainToCodeAscii, memInfo, inputBuf);
        }
        results[boxIdx] = std::move(res);
    };

    size_t numWorkers = OptimalWorkerCount(boxes.size());
    if (numWorkers <= 1) {
        auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<float> inputBuf;
        for (size_t idx : order) inferOneBox(idx, 0, memInfo, inputBuf);
    } else {
        std::atomic<size_t> nextWork{ 0 };
        std::vector<std::thread> workers;
        workers.reserve(numWorkers - 1);
        auto workerLoop = [&](size_t workerId) {
            auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            std::vector<float> inputBuf;
            inputBuf.reserve(3 * kRecHeight * 640);
            while (true) {
                size_t k = nextWork.fetch_add(1, std::memory_order_relaxed);
                if (k >= order.size()) break;
                inferOneBox(order[k], workerId, memInfo, inputBuf);
            }
        };
        for (size_t t = 1; t < numWorkers; ++t) {
            workers.emplace_back(workerLoop, t);
        }
        workerLoop(0);
        for (auto& w : workers) w.join();
    }

    // Stage B: Deterministic Top-to-Bottom Topological Verification & PDA Delimiter Stack (< 0.1 ms)
    std::vector<wchar_t> openStack;
    for (size_t i = 0; i < boxes.size(); ++i) {
        const DetBox& bx = boxes[i];
        float tightW = bx.x1 - bx.x0;
        RecResult& res = results[i];

        // Case A: Standalone 1-3 component symbol box ('}', '{', '};', ')')
        // Guard against overwriting multi-digit line numbers (e.g. "45" where '4' has left-leaning curvature):
        // any trailing components after bx.comps[0] must be small punctuation (';', ','), never full-height glyphs.
        bool isStandaloneBracketBox = !bx.comps.empty() && bx.comps.size() <= 3 && tightW <= pf.medGlyphH * 1.65f;
        if (isStandaloneBracketBox && bx.comps.size() >= 2) {
            for (size_t ci = 1; ci < bx.comps.size(); ++ci) {
                if (bx.comps[ci].height() > static_cast<int>(pf.medGlyphH * 0.72f) ||
                    bx.comps[ci].width() > static_cast<int>(pf.medGlyphH * 0.50f)) {
                    isStandaloneBracketBox = false;
                    break;
                }
            }
        }
        if (isStandaloneBracketBox && res.chars.size() >= 2 && std::iswalnum(res.chars[0].ch) &&
            std::iswalnum(res.chars[1].ch) && res.meanProb >= 0.85f) {
            isStandaloneBracketBox = false;
        }
        if (isStandaloneBracketBox) {
            int curv = GetVerticalBracketCurvature(bx.comps[0], pf.medGlyphH);
            if (curv != 0) {
                wchar_t sym = (curv > 0) ? L'}' : L'{';
                if (curv > 0 && !openStack.empty()) {
                    sym = (openStack.back() == L'(') ? L')' : L'}';
                } else if (curv > 0 && !IsCurlyBraceShape(bx.comps[0], curv) &&
                           !res.chars.empty() && res.chars.front().ch == L')') {
                    sym = L')';
                }
                bool hasSemi = false;
                if (bx.comps.size() >= 2) {
                    const auto& cLast = bx.comps.back();
                    if (cLast.minX > bx.comps[0].maxX - 2 && cLast.width() <= static_cast<int>(pf.medGlyphH * 0.45f)) {
                        hasSemi = true;
                    }
                }
                res.chars.clear();
                res.chars.push_back({ sym, 0.5f * (bx.comps[0].minX + bx.comps[0].maxX), 0.95f });
                if (hasSemi) {
                    res.chars.push_back({ L';', 0.5f * (bx.comps.back().minX + bx.comps.back().maxX), 0.95f });
                }
                res.meanProb = 0.95f;
            }
        }
        // Case B: Multi-token line structural & topological verification (keyword-free!)
        else if (!bx.comps.empty() && !res.chars.empty()) {
            // 1. Leading closing brace '}' or member-access dot '.' at the start of a line
            const InkComponent& firstC = bx.comps.front();
            if (GetVerticalBracketCurvature(firstC, pf.medGlyphH) > 0 && IsCurlyBraceShape(firstC, +1)) {
                if (res.chars.front().ch != L'}') {
                    if (res.chars.front().ch == L'-' || res.chars.front().ch == L'1' || res.chars.front().ch == L')') {
                        res.chars.front().ch = L'}';
                    } else if (firstC.maxX + 2 < res.chars.front().x) {
                        res.chars.insert(res.chars.begin(), { L'}', firstC.centerX(), 0.95f });
                        res.chars.insert(res.chars.begin() + 1, { L' ', static_cast<float>(firstC.maxX + 2), 0.95f });
                    }
                }
            } else if (firstC.width() <= static_cast<int>(pf.medGlyphH * 0.48f) &&
                       firstC.height() <= static_cast<int>(pf.medGlyphH * 0.48f) &&
                       firstC.minY >= static_cast<int>(bx.y0 + (bx.y1 - bx.y0) * 0.30f) &&
                       res.chars.front().ch != L'.' && std::iswlower(res.chars.front().ch)) {
                res.chars.insert(res.chars.begin(), { L'.', firstC.centerX(), 0.95f });
            }

            // 2. Trailing opening bracket '{' or '(' at the end of a line
            const InkComponent& lastC = bx.comps.back();
            if (GetVerticalBracketCurvature(lastC, pf.medGlyphH) < 0) {
                wchar_t prevNonSpace = 0;
                for (size_t k = 0; k + 1 < res.chars.size(); ++k) {
                    if (res.chars[k].ch != L' ') prevNonSpace = res.chars[k].ch;
                }
                bool afterBlockHeader = (prevNonSpace == L')' || prevNonSpace == L'>');

                wchar_t lastCh = res.chars.back().ch;
                if (lastCh == L'-' || lastCh == L'0' || lastCh == L'1') {
                    res.chars.back().ch = afterBlockHeader ? L'{' : L'(';
                } else if (afterBlockHeader && lastCh == L'(') {
                    res.chars.back().ch = L'{';
                } else if (lastCh == L'>' || lastCh == L')') {
                    res.chars.push_back({ L' ', static_cast<float>(lastC.minX - 2), 0.95f });
                    res.chars.push_back({ L'{', lastC.centerX(), 0.95f });
                } else if (std::iswalnum(lastCh) && lastC.minX > res.chars.back().x + pf.medGlyphH * 0.25f) {
                    res.chars.push_back({ L' ', static_cast<float>(lastC.minX - 2), 0.95f });
                    res.chars.push_back({ L'(', lastC.centerX(), 0.95f });
                }
            }

            // Ensure standard space between ')' or '>' and trailing '{' in block headers
            if (res.chars.size() >= 2 && res.chars.back().ch == L'{' &&
                (res.chars[res.chars.size() - 2].ch == L')' || res.chars[res.chars.size() - 2].ch == L'>')) {
                float spX = res.chars.back().x - 2.0f;
                res.chars.insert(res.chars.end() - 1, { L' ', spX, 0.95f });
            }

            // Normalize 2-char logical OR (' || ') where '|' + ('l'|'1'|'I') was decoded between spaces
            auto isPipeLookalike = [](wchar_t c) { return c == L'|' || c == L'l' || c == L'1' || c == L'I'; };
            for (size_t k = 1; k + 2 < res.chars.size(); ++k) {
                if (res.chars[k - 1].ch == L' ' && res.chars[k + 2].ch == L' ' &&
                    isPipeLookalike(res.chars[k].ch) && isPipeLookalike(res.chars[k + 1].ch) &&
                    !(res.chars[k].ch == L'1' && res.chars[k + 1].ch == L'1')) {
                    res.chars[k].ch = L'|';
                    res.chars[k + 1].ch = L'|';
                }
            }

            // 3. Keyword-free 2-Bar Connected-Component '=' vs '-' Verification
            for (size_t k = 0; k < res.chars.size(); ++k) {
                if (res.chars[k].ch == L'-') {
                    if (HasTwoHorizontalBarsAt(bx, pf, res.chars[k].x)) {
                        res.chars[k].ch = L'=';
                        res.chars[k].prob = std::max(res.chars[k].prob, 0.92f);
                    }
                }
            }

            // 4. Recover CTC-collapsed empty string literals '""' when '",' or '"]' or '")' appears outside a string
            int qRun = 0;
            for (size_t k = 0; k < res.chars.size(); ++k) {
                if (res.chars[k].ch == L'"') {
                    if ((qRun % 2 == 0) && k + 1 < res.chars.size()) {
                        wchar_t nextCh = res.chars[k + 1].ch;
                        if (nextCh == L',' || nextCh == L']' || nextCh == L')' || nextCh == L';') {
                            res.chars.insert(res.chars.begin() + static_cast<std::ptrdiff_t>(k + 1),
                                             { L'"', res.chars[k].x + 2.0f, 0.95f });
                            qRun += 2;
                            ++k;
                            continue;
                        }
                    }
                    ++qRun;
                }
            }

            // 5. Fix '[a-z]l"' or '[a-z]1"' -> '[a-z]("' (opening paren before string literal misread as 'l'/'1')
            bool hasOpenParen = false;
            for (size_t k = 0; k < res.chars.size(); ++k) {
                if (res.chars[k].ch == L'(') hasOpenParen = true;
                if (!hasOpenParen && k >= 1 && k + 1 < res.chars.size() &&
                    (res.chars[k].ch == L'l' || res.chars[k].ch == L'1') &&
                    std::iswlower(res.chars[k - 1].ch) && res.chars[k + 1].ch == L'"') {
                    res.chars[k].ch = L'(';
                    hasOpenParen = true;
                }
            }

            // 6. Strip spurious space immediately after an opening '"' inside a string argument (e.g. '" sender",' -> '"sender",')
            int qParity = 0;
            for (size_t k = 0; k < res.chars.size(); ++k) {
                if (res.chars[k].ch == L'"') {
                    ++qParity;
                    if ((qParity % 2 == 1) && k + 2 < res.chars.size() &&
                        res.chars[k + 1].ch == L' ' && std::iswalnum(res.chars[k + 2].ch)) {
                        res.chars.erase(res.chars.begin() + static_cast<std::ptrdiff_t>(k + 1));
                    }
                }
            }

            // 7. Balance leading '(' on single-expression crops ending with ')' (e.g. '(message?._id)')
            int pBal = 0;
            for (const auto& rc : res.chars) {
                if (rc.ch == L'(') ++pBal;
                else if (rc.ch == L')') --pBal;
            }
            if (pBal == -1 && !res.chars.empty() && res.chars.front().ch != L'(' && res.chars.back().ch == L')') {
                res.chars.insert(res.chars.begin(), { L'(', bx.x0, 0.95f });
            }

            // 8. Repair single-letter OCR case flips on all-caps acronyms (e.g. "HTTp", "HttP", "posT")
            size_t aStart = 0;
            while (aStart < res.chars.size()) {
                while (aStart < res.chars.size() && !std::iswalpha(res.chars[aStart].ch)) ++aStart;
                if (aStart >= res.chars.size()) break;
                size_t aEnd = aStart;
                int nUp = 0, nLo = 0;
                while (aEnd < res.chars.size() && std::iswalpha(res.chars[aEnd].ch)) {
                    if (std::iswupper(res.chars[aEnd].ch)) ++nUp;
                    else ++nLo;
                    ++aEnd;
                }
                size_t len = aEnd - aStart;
                if (len >= 3 && nUp >= 1 && nLo >= 1) {
                    // Case 1: All-caps acronym where 1-2 interior/trailing letters flipped to lowercase ("HTTp", "HttP")
                    if ((nUp == static_cast<int>(len) - 1 && std::iswupper(res.chars[aStart].ch) && std::iswlower(res.chars[aEnd - 1].ch)) ||
                        (std::iswupper(res.chars[aStart].ch) && std::iswupper(res.chars[aEnd - 1].ch))) {
                        for (size_t k = aStart; k < aEnd; ++k) res.chars[k].ch = static_cast<wchar_t>(std::towupper(res.chars[k].ch));
                    }
                    // Case 2: Word inside quotes where only the very last letter flipped to uppercase ("posT" -> "POST")
                    else if (nUp == 1 && std::iswupper(res.chars[aEnd - 1].ch) &&
                             aStart > 0 && res.chars[aStart - 1].ch == L'"' &&
                             aEnd < res.chars.size() && res.chars[aEnd].ch == L'"') {
                        for (size_t k = aStart; k < aEnd; ++k) res.chars[k].ch = static_cast<wchar_t>(std::towupper(res.chars[k].ch));
                    }
                }
                aStart = aEnd;
            }
        }

        // Update Pushdown Automaton delimiter stack with all brackets on this line
        for (const auto& rc : res.chars) {
            if (rc.ch == L'{' || rc.ch == L'(') {
                openStack.push_back(rc.ch);
            } else if (rc.ch == L'}') {
                for (auto it = openStack.rbegin(); it != openStack.rend(); ++it) {
                    if (*it == L'{') { openStack.erase(std::next(it).base()); break; }
                }
            } else if (rc.ch == L')') {
                for (auto it = openStack.rbegin(); it != openStack.rend(); ++it) {
                    if (*it == L'(') { openStack.erase(std::next(it).base()); break; }
                }
            }
        }
    }
    return results;
}

// ------------------------------------------------------------------------------------------------
// Step 4: Assembly into OcrDocument
// ------------------------------------------------------------------------------------------------

struct Segment {
    RectF bounds;
    std::vector<OcrWordBox> words;
};

Segment BuildSegment(const DetBox& bx, const RecResult& rr) {
    Segment seg;
    seg.bounds = { bx.x0, bx.y0, bx.x1 - bx.x0, bx.y1 - bx.y0 };

    std::vector<float> diffs;
    for (size_t i = 1; i < rr.chars.size(); ++i) {
        if (rr.chars[i].ch != L' ' && rr.chars[i - 1].ch != L' ') {
            float d = rr.chars[i].x - rr.chars[i - 1].x;
            if (d > 1.0f) diffs.push_back(d);
        }
    }
    float pitch = (bx.y1 - bx.y0) * 0.48f;
    if (diffs.size() >= 2) {
        std::nth_element(diffs.begin(), diffs.begin() + diffs.size() / 2, diffs.end());
        pitch = std::clamp(diffs[diffs.size() / 2], 2.0f, (bx.y1 - bx.y0) * 1.2f);
    }

    float h = bx.y1 - bx.y0;
    float wy0 = bx.y0;
    float wh = h;

    size_t i = 0;
    while (i < rr.chars.size()) {
        while (i < rr.chars.size() && rr.chars[i].ch == L' ') ++i;
        if (i >= rr.chars.size()) break;
        size_t j = i;
        std::wstring text;
        double probSum = 0.0;
        float minP = 1.0f;
        while (j < rr.chars.size() && rr.chars[j].ch != L' ') {
            text += rr.chars[j].ch;
            probSum += rr.chars[j].prob;
            minP = std::min(minP, rr.chars[j].prob);
            ++j;
        }
        float left = (i == 0) ? bx.x0 : std::max(bx.x0, rr.chars[i].x - pitch * 0.45f);
        float right = (j == rr.chars.size()) ? bx.x1 : std::min(bx.x1, rr.chars[j - 1].x + pitch * 0.45f);
        if (right <= left) right = left + pitch;
        OcrWordBox w;
        w.text = std::move(text);
        w.bounds = { left, wy0, right - left, wh };
        w.confidence = (j > i) ? static_cast<float>(probSum / static_cast<double>(j - i)) : 1.0f;
        w.minCharConf = (j > i) ? minP : 1.0f;
        seg.words.push_back(std::move(w));
        i = j;
    }
    return seg;
}

} // namespace

void PaddleOcr::SetOptions(const Options& opts) {
    std::lock_guard<std::mutex> lock(g_optMutex);
    g_options = opts;
}

PaddleOcr::Options PaddleOcr::GetOptions() {
    std::lock_guard<std::mutex> lock(g_optMutex);
    return g_options;
}

bool PaddleOcr::EnsureInitialized(std::wstring* outError) {
    std::call_once(g_initOnce, DoInitialize);
    if (!g_ready && outError) *outError = g_initError;
    return g_ready;
}

bool PaddleOcr::IsAvailable() {
    return g_ready;
}

std::wstring PaddleOcr::GetActiveEngineDescription() {
    if (!EnsureInitialized(nullptr) || !g_engine) return L"Unavailable";
    if (g_engine->recs.size() >= 2) return L"PP-OCRv5 + v4 Dual-View Code Engine";
    return L"PP-OCR " + g_engine->recs.front()->name + L" Code Engine";
}

void PaddleOcr::WarmUpAsync() {
    std::thread([] { EnsureInitialized(nullptr); }).detach();
}

bool PaddleOcr::Recognize(const RawBitmap& bitmap, OcrDocument& outDoc, std::wstring* outError) {
    if (!EnsureInitialized(outError)) return false;
    if (!bitmap.valid()) {
        if (outError) *outError = L"Invalid bitmap";
        return false;
    }

    try {
        Engine& e = *g_engine;
        PreprocessedFrame pf = PreprocessCodeBitmap(bitmap);
        std::vector<DetBox> boxes = BuildComponentLineBoxes(pf);
        std::vector<RecResult> recs = RunRecognition(e, pf, boxes);

        std::vector<Segment> segments;
        segments.reserve(boxes.size());
        for (size_t i = 0; i < boxes.size(); ++i) {
            if (recs[i].chars.empty() || recs[i].meanProb < 0.28f) continue;
            Segment s = BuildSegment(boxes[i], recs[i]);
            if (!s.words.empty()) segments.push_back(std::move(s));
        }

        std::sort(segments.begin(), segments.end(), [](const Segment& a, const Segment& b) {
            return a.bounds.centerY() < b.bounds.centerY();
        });
        struct Row { float y0, y1; std::vector<Segment*> segs; };
        std::vector<Row> rows;
        for (auto& s : segments) {
            Row* target = nullptr;
            for (auto& r : rows) {
                float overlap = std::min(r.y1, s.bounds.bottom()) - std::max(r.y0, s.bounds.y);
                float minH = std::min(r.y1 - r.y0, s.bounds.height);
                if (overlap >= minH * 0.45f ||
                    std::abs(0.5f * (r.y0 + r.y1) - s.bounds.centerY()) <= pf.medGlyphH * 0.55f) {
                    target = &r;
                    break;
                }
            }
            if (!target) {
                rows.push_back({ s.bounds.y, s.bounds.bottom(), {} });
                target = &rows.back();
            }
            target->y0 = std::min(target->y0, s.bounds.y);
            target->y1 = std::max(target->y1, s.bounds.bottom());
            target->segs.push_back(&s);
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.y0 + a.y1 < b.y0 + b.y1; });

        const float invScale = bitmap.scaleFactor > 0.0f ? 1.0f / bitmap.scaleFactor : 1.0f;
        const float ox = static_cast<float>(bitmap.screenOriginX);
        const float oy = static_cast<float>(bitmap.screenOriginY);
        auto toOverlay = [&](const RectF& r) {
            return RectF{ ox + r.x * invScale, oy + r.y * invScale, r.width * invScale, r.height * invScale };
        };

        outDoc.lines.clear();
        outDoc.scanRegion = { ox, oy, bitmap.width * invScale, bitmap.height * invScale };
        for (auto& r : rows) {
            std::sort(r.segs.begin(), r.segs.end(), [](const Segment* a, const Segment* b) { return a->bounds.x < b->bounds.x; });
            OcrLineBox line;
            float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
            for (Segment* s : r.segs) {
                for (auto& w : s->words) {
                    w.bounds = toOverlay(w.bounds);
                    w.lineIndex = static_cast<int>(outDoc.lines.size());
                    w.wordIndex = static_cast<int>(line.words.size());
                    minX = std::min(minX, w.bounds.x);
                    minY = std::min(minY, w.bounds.y);
                    maxX = std::max(maxX, w.bounds.right());
                    maxY = std::max(maxY, w.bounds.bottom());
                    if (!line.text.empty()) line.text += L' ';
                    line.text += w.text;
                    line.words.push_back(std::move(w));
                }
            }
            if (line.words.empty()) continue;
            line.bounds = { minX, minY, maxX - minX, maxY - minY };
            outDoc.lines.push_back(std::move(line));
        }

        // Document-level cross-line identifier voting (repairs uncertain smudges using high-confidence occurrences)
        RepairCrossLineIdentifiers(outDoc);
        return true;
    } catch (const Ort::Exception& ex) {
        if (outError) *outError = L"ONNX Runtime error: " + Utf8ToWide(ex.what());
    } catch (const std::exception& ex) {
        if (outError) *outError = L"OCR error: " + Utf8ToWide(ex.what());
    }
    return false;
}

namespace {

size_t IdentEditDistance(const std::wstring& a, const std::wstring& b) {
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

bool IsIdentChar(wchar_t c) {
    return std::iswalnum(c) || c == L'_';
}

bool DiffersOnlyByDigitOrPluralS(const std::wstring& a, const std::wstring& b) {
    // Never merge singular/plural identifiers (e.g. "item" vs "items", "user" vs "users")
    if (a.size() + 1 == b.size() && b.back() == L's' && b.compare(0, a.size(), a) == 0) return true;
    if (b.size() + 1 == a.size() && a.back() == L's' && a.compare(0, b.size(), b) == 0) return true;
    // Never merge numbered identifiers (e.g. "node1" vs "node2", "x1" vs "x2")
    if (a.size() == b.size()) {
        size_t diffIdx = std::wstring::npos;
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i] != b[i]) {
                if (diffIdx != std::wstring::npos) return false;
                diffIdx = i;
            }
        }
        if (diffIdx != std::wstring::npos && std::iswdigit(a[diffIdx]) && std::iswdigit(b[diffIdx])) {
            return true;
        }
    }
    return false;
}

} // namespace

size_t PaddleOcr::RepairCrossLineIdentifiers(OcrDocument& doc) {
    struct AnchorStats {
        int count = 0;
        float bestMeanConf = 0.0f;
        float bestMinConf = 0.0f;
    };
    std::unordered_map<std::wstring, AnchorStats> stats;

    // Pass 1: Collect identifier statistics across the entire snippet
    for (const auto& line : doc.lines) {
        for (const auto& w : line.words) {
            if (w.isLineNumber) continue;
            const std::wstring& s = w.text;
            size_t i = 0;
            while (i < s.size()) {
                if (std::iswalpha(s[i]) || s[i] == L'_') {
                    size_t j = i + 1;
                    while (j < s.size() && IsIdentChar(s[j])) ++j;
                    if (j - i >= 5) {
                        std::wstring id = s.substr(i, j - i);
                        auto& st = stats[id];
                        st.count += 1;
                        st.bestMeanConf = std::max(st.bestMeanConf, w.confidence);
                        st.bestMinConf = std::max(st.bestMinConf, w.minCharConf);
                    }
                    i = j;
                } else {
                    ++i;
                }
            }
        }
    }

    // Build set of trusted high-confidence anchor identifiers
    std::vector<std::wstring> anchors;
    for (const auto& kv : stats) {
        const auto& st = kv.second;
        if ((st.bestMeanConf >= 0.93f && st.bestMinConf >= 0.85f) || (st.count >= 2 && st.bestMeanConf >= 0.88f)) {
            anchors.push_back(kv.first);
        }
    }
    if (anchors.empty()) return 0;

    // Pass 2: Repair low-confidence single-occurrence identifiers that uniquely match a trusted anchor at edit distance 1
    size_t repaired = 0;
    for (auto& line : doc.lines) {
        bool lineModified = false;
        for (auto& w : line.words) {
            if (w.isLineNumber) continue;
            if (!w.isUncertain() && w.minCharConf >= 0.82f) continue;

            std::wstring updated;
            const std::wstring& s = w.text;
            size_t i = 0;
            bool wordModified = false;

            while (i < s.size()) {
                if (std::iswalpha(s[i]) || s[i] == L'_') {
                    size_t j = i + 1;
                    while (j < s.size() && IsIdentChar(s[j])) ++j;
                    std::wstring id = s.substr(i, j - i);

                    auto itSt = stats.find(id);
                    bool isRareWeak = (id.size() >= 5) && (itSt != stats.end() && itSt->second.count == 1 && itSt->second.bestMinConf < 0.84f);
                    if (isRareWeak) {
                        const std::wstring* uniqueMatch = nullptr;
                        int matchCount = 0;
                        for (const auto& anc : anchors) {
                            if (anc == id) { matchCount = 2; break; }
                            if (std::iswupper(anc[0]) != std::iswupper(id[0])) continue;
                            if (std::abs(static_cast<int>(anc.size()) - static_cast<int>(id.size())) > 1) continue;
                            if (DiffersOnlyByDigitOrPluralS(id, anc)) continue;

                            size_t dist = IdentEditDistance(id, anc);
                            if (dist == 1) {
                                uniqueMatch = &anc;
                                ++matchCount;
                                if (matchCount > 1) break;
                            }
                        }
                        if (matchCount == 1 && uniqueMatch) {
                            updated += *uniqueMatch;
                            wordModified = true;
                            ++repaired;
                        } else {
                            updated += id;
                        }
                    } else {
                        updated += id;
                    }
                    i = j;
                } else {
                    updated += s[i++];
                }
            }

            if (wordModified) {
                w.text = std::move(updated);
                w.confidence = std::max(w.confidence, 0.90f);
                w.minCharConf = std::max(w.minCharConf, 0.80f);
                lineModified = true;
            }
        }
        if (lineModified) {
            line.text.clear();
            for (const auto& w : line.words) {
                if (!line.text.empty()) line.text += L' ';
                line.text += w.text;
            }
        }
    }
    return repaired;
}

} // namespace ScreenTextify
