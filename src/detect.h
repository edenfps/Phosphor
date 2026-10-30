#pragma once
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Detects the resolution content was rendered at before being stretched to
// the panel, from 32 rows and 32 columns of the desktop (see Renderer::Probe).
//
// Every common scaler (nearest, bilinear, bicubic, Lanczos) builds the
// stretched image purely from one fixed kernel shape per source pixel, spaced
// on the source grid. So a line stretched from N pixels can be rebuilt
// almost exactly by a least-squares fit of N kernels, while at any slightly
// lower N the fit error jumps (the grid no longer lines up and detail is
// missing). Native content shows no such dip at all.
//
// Decision: a candidate counts when it fits better than the next higher
// candidate and the next lower one fits at least twice as badly; the lowest
// such candidate wins (multiples of the true size also dip).

struct DetectResult
{
    int sourceW = 0, sourceH = 0; // 0 = native / not stretched
    std::wstring detail;          // diagnostics
};

DetectResult AnalyzeLines(const float* lines, int w, int h);

// Runs AnalyzeLines off the render thread and only reports a resolution once
// it has been seen several times in a row.
class ResolutionDetector
{
public:
    ~ResolutionDetector() { Stop(); }
    void Start();
    void Stop();
    // Non-blocking; dropped if the worker is still busy.
    void Submit(std::vector<float>&& lines, int w, int h);
    // True when the stable decision changed; r receives it.
    bool Poll(DetectResult& r);
    bool Busy();
    bool debug = false; // log every analysis, not just changes

private:
    void Run();
    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    bool quit_ = false, busy_ = false, hasInput_ = false, changed_ = false;
    std::vector<float> input_;
    int w_ = 0, h_ = 0;
    DetectResult stable_, candidate_;
    int streak_ = 0;
};
