#include "detect.h"
#include "common.h"
#include <algorithm>
#include <cmath>
#include <cwchar>

namespace
{
const int kRows = 32, kCols = 32;
const int kUseLines = 12;  // most detailed lines per axis
const int kSegment = 600;  // pixels per line used for the fit
const double kPi = 3.14159265358979;

const int kHeights[] = {1800, 1440, 1200, 1080, 1050, 900, 864, 800, 768, 720, 600, 576, 540, 480, 450, 400, 360, 288, 270, 240, 224};
const int kWidths[] = {3200, 2560, 2133, 1920, 1680, 1600, 1440, 1366, 1280, 1067, 1024, 960, 854, 800, 720, 640, 512, 427, 400, 320};

double KBox(double t) // nearest neighbour
{
    return t >= -0.5 && t < 0.5 ? 1 : 0;
}
double KLinear(double t)
{
    t = std::fabs(t);
    return t < 1 ? 1 - t : 0;
}
double KCubic(double t) // Catmull-Rom, a = -0.5
{
    const double a = -0.5;
    t = std::fabs(t);
    if (t < 1)
        return (a + 2) * t * t * t - (a + 3) * t * t + 1;
    if (t < 2)
        return a * t * t * t - 5 * a * t * t + 8 * a * t - 4 * a;
    return 0;
}
double KLanczos(double t)
{
    t = std::fabs(t);
    if (t < 1e-9)
        return 1;
    if (t >= 3)
        return 0;
    return 3 * std::sin(kPi * t) * std::sin(kPi * t / 3) / (kPi * kPi * t * t);
}
double KBoxCorner(double t) // nearest neighbour anchored at the pixel corner
{
    return t >= 0 && t < 1 ? 1 : 0;
}
struct Kernel
{
    double (*f)(double);
    int radius;
    bool cornerAligned; // source position x / r instead of the centre-aligned (x + 0.5) / r - 0.5
};
const Kernel kKernels[] = {
    {KBox, 1, false}, {KBoxCorner, 1, true}, {KLinear, 1, false}, {KCubic, 2, false}, {KLanczos, 3, false}};

double SourcePos(int x, double r, const Kernel& k)
{
    return k.cornerAligned ? x / r : (x + 0.5) / r - 0.5;
}

// RMS error (0-255 levels) of the best fit of y (output pixels s0..s0+n-1 of a
// line) by kernels on a source grid with spacing r, centre aligned like GPU
// and video scalers. Normal equations are banded: solved with banded Cholesky.
double FitError(const float* y, int s0, int n, double r, const Kernel& k)
{
    double u0 = SourcePos(s0, r, k), u1 = SourcePos(s0 + n - 1, r, k);
    int j0 = int(std::floor(u0)) - k.radius, j1 = int(std::ceil(u1)) + k.radius;
    int m = j1 - j0 + 1;
    int bw = int(std::ceil(2.0 * k.radius)) + 2; // half bandwidth of A^T A
    std::vector<double> N(size_t(m) * (bw + 1), 0.0), b(m, 0.0); // N[j][d] = (A^T A)(j, j + d)
    std::vector<double> wts(2 * k.radius + 3);
    std::vector<int> idx(2 * k.radius + 3);
    for (int i = 0; i < n; i++)
    {
        double u = SourcePos(s0 + i, r, k);
        int a = int(std::floor(u)) - k.radius + 1, cnt = 0;
        double sum = 0;
        for (int j = a; j <= int(std::floor(u)) + k.radius; j++)
        {
            double wv = k.f(u - j);
            if (wv != 0)
            {
                idx[cnt] = j - j0;
                wts[cnt] = wv;
                sum += wv;
                cnt++;
            }
        }
        if (sum == 0)
            continue;
        for (int p = 0; p < cnt; p++)
        {
            double wp = wts[p] / sum; // normalised, like real scalers
            b[idx[p]] += wp * y[i];
            for (int q = 0; q < cnt; q++)
            {
                int d = idx[q] - idx[p];
                if (d >= 0 && d <= bw)
                    N[size_t(idx[p]) * (bw + 1) + d] += wp * wts[q] / sum;
            }
        }
    }
    for (int j = 0; j < m; j++)
        N[size_t(j) * (bw + 1)] += 1e-9; // tiny ridge for unconstrained edge samples
    // Banded Cholesky: N = L L^T, stored in place (L(j+d, j) at N[j][d]).
    for (int j = 0; j < m; j++)
    {
        double* row = &N[size_t(j) * (bw + 1)];
        for (int k2 = std::max(0, j - bw); k2 < j; k2++)
        {
            double ljk = N[size_t(k2) * (bw + 1) + (j - k2)];
            for (int d = 0; d <= bw && j + d < m; d++)
            {
                int dd = j + d - k2;
                if (dd <= bw)
                    row[d] -= ljk * N[size_t(k2) * (bw + 1) + dd];
            }
        }
        double piv = row[0] > 1e-15 ? std::sqrt(row[0]) : 1e-7;
        row[0] = piv;
        for (int d = 1; d <= bw && j + d < m; d++)
            row[d] /= piv;
    }
    std::vector<double> z(m), s(m);
    for (int j = 0; j < m; j++)
    {
        double v = b[j];
        for (int k2 = std::max(0, j - bw); k2 < j; k2++)
            v -= N[size_t(k2) * (bw + 1) + (j - k2)] * z[k2];
        z[j] = v / N[size_t(j) * (bw + 1)];
    }
    for (int j = m - 1; j >= 0; j--)
    {
        double v = z[j];
        for (int d = 1; d <= bw && j + d < m; d++)
            v -= N[size_t(j) * (bw + 1) + d] * s[j + d];
        s[j] = v / N[size_t(j) * (bw + 1)];
    }
    double err = 0;
    for (int i = 0; i < n; i++)
    {
        double u = SourcePos(s0 + i, r, k), sum = 0, v = 0;
        for (int j = int(std::floor(u)) - k.radius + 1; j <= int(std::floor(u)) + k.radius; j++)
        {
            double wv = k.f(u - j);
            sum += wv;
            v += wv * s[j - j0];
        }
        double e = (sum != 0 ? v / sum : 0) - y[i];
        err += e * e;
    }
    return std::sqrt(err / n) * 255.0;
}

struct Line
{
    const float* v;
    int n, s0;
    double detail;
};

// Most detailed lines of one axis, each with its most detailed segment.
std::vector<Line> PickLines(const float* data, int w, int first, int count, int n)
{
    std::vector<Line> out;
    for (int i = 0; i < count; i++)
    {
        const float* v = data + size_t(first + i) * w;
        if (n <= kSegment + 2)
            continue;
        double run = 0, best = -1;
        int bestStart = 0;
        for (int x = 1; x < n; x++)
        {
            run += std::fabs(v[x] - v[x - 1]);
            if (x >= kSegment)
                run -= std::fabs(v[x - kSegment + 1] - v[x - kSegment]);
            if (x >= kSegment - 1 && run > best)
            {
                best = run;
                bestStart = x - kSegment + 1;
            }
        }
        if (best / kSegment > 2e-3)
            out.push_back({v, n, bestStart, best});
    }
    std::sort(out.begin(), out.end(), [](const Line& a, const Line& b) { return a.detail > b.detail; });
    if (out.size() > size_t(kUseLines))
        out.resize(kUseLines);
    return out;
}

// Fit error per candidate size: best kernel, median over lines.
std::vector<double> Errors(const std::vector<Line>& lines, int n, const int* sizes, int count)
{
    std::vector<double> out(count, 1e9);
    for (int c = 0; c < count; c++)
    {
        double r = double(n) / sizes[c];
        if (r < 1.05)
            continue;
        for (const Kernel& k : kKernels)
        {
            std::vector<double> e;
            for (const Line& l : lines)
                e.push_back(FitError(l.v + l.s0, l.s0, kSegment, r, k));
            std::nth_element(e.begin(), e.begin() + e.size() / 2, e.end());
            out[c] = std::min(out[c], e[e.size() / 2]);
        }
    }
    return out;
}

// A candidate dips when its error sits clearly below the trend of its two
// neighbours (native content rises smoothly, without dips) and the next lower
// candidate is clearly worse. Real video scalers dip less sharply than the
// textbook kernels, so the test is relative. The lowest dip wins (multiples
// of the true size dip too). -1 = none.
int PickDip(const std::vector<double>& e, const int* sizes, int count)
{
    int pick = -1;
    for (int c = 1; c + 1 < count; c++)
    {
        double hi = e[c - 1], lo = e[c + 1], v = e[c];
        if (v > 4.0 || hi >= 1e8 || lo >= 1e8)
            continue;
        double trend = std::sqrt(hi * lo);
        if (v < hi && lo >= 1.25 * v && v <= 0.85 * trend)
            pick = c;
    }
    return pick;
}
} // namespace

DetectResult AnalyzeLines(const float* data, int w, int h)
{
    DetectResult res;
    std::vector<Line> rows = PickLines(data, w, 0, kRows, w);
    std::vector<Line> cols = PickLines(data, w, kRows, kCols, h);
    if (cols.size() < 4)
    {
        res.detail = L"too little detail";
        return res;
    }
    const int nh = int(std::size(kHeights)), nw = int(std::size(kWidths));
    std::vector<double> ev = Errors(cols, h, kHeights, nh);
    int vi = PickDip(ev, kHeights, nh);

    wchar_t buf[64];
    res.detail = L"V";
    for (int c = 0; c < nh; c++)
        if (ev[c] < 1e8)
        {
            swprintf_s(buf, L" %d:%.2f", kHeights[c], ev[c]);
            res.detail += buf;
        }
    if (vi < 0)
    {
        res.detail = L"native | " + res.detail;
        return res;
    }
    res.sourceH = kHeights[vi];
    // Width: assume square pixels unless the rows clearly dip elsewhere.
    res.sourceW = int(std::lround(double(w) * res.sourceH / h));
    if (rows.size() >= 4)
    {
        std::vector<double> eh = Errors(rows, w, kWidths, nw);
        int hi = PickDip(eh, kWidths, nw);
        if (hi >= 0 && std::fabs(double(kWidths[hi]) / res.sourceW - 1.0) < 0.35)
            res.sourceW = kWidths[hi];
    }
    swprintf_s(buf, L"%dx%d | ", res.sourceW, res.sourceH);
    res.detail = buf + res.detail;
    return res;
}

void ResolutionDetector::Start()
{
    if (thread_.joinable())
        return;
    quit_ = false;
    thread_ = std::thread([this] { Run(); });
}

void ResolutionDetector::Stop()
{
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

bool ResolutionDetector::Busy()
{
    std::lock_guard<std::mutex> lk(m_);
    return busy_ || hasInput_;
}

void ResolutionDetector::Submit(std::vector<float>&& lines, int w, int h)
{
    {
        std::lock_guard<std::mutex> lk(m_);
        if (busy_ || hasInput_)
            return;
        input_ = std::move(lines);
        w_ = w;
        h_ = h;
        hasInput_ = true;
    }
    cv_.notify_all();
}

bool ResolutionDetector::Poll(DetectResult& r)
{
    std::lock_guard<std::mutex> lk(m_);
    if (!changed_)
        return false;
    changed_ = false;
    r = stable_;
    return true;
}

void ResolutionDetector::Run()
{
    std::unique_lock<std::mutex> lk(m_);
    while (true)
    {
        cv_.wait(lk, [this] { return quit_ || hasInput_; });
        if (quit_)
            return;
        std::vector<float> data = std::move(input_);
        int w = w_, h = h_;
        hasInput_ = false;
        busy_ = true;
        lk.unlock();
        DetectResult r = AnalyzeLines(data.data(), w, h);
        if (debug)
            Log(L"detect: %s", r.detail.c_str());
        lk.lock();
        busy_ = false;
        // "too little detail" frames (dark, flat) keep the current decision.
        if (r.sourceH == 0 && r.detail == L"too little detail")
            continue;
        // Switch only after the same answer three times in a row.
        if (r.sourceH == candidate_.sourceH && r.sourceW == candidate_.sourceW)
            streak_++;
        else
        {
            candidate_ = r;
            streak_ = 1;
        }
        if (streak_ >= 3 && (candidate_.sourceH != stable_.sourceH || candidate_.sourceW != stable_.sourceW))
        {
            stable_ = candidate_;
            changed_ = true;
        }
    }
}
