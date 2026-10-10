#include "LiveStacker.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

namespace {

double medianOf(std::vector<double> v)
{
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

// Solve a small dense system by Gaussian elimination with partial pivoting
bool solve(std::vector<double>& A, std::vector<double>& b, int n)
{
    for (int c = 0; c < n; ++c) {
        int p = c; for (int r = c + 1; r < n; ++r) if (std::fabs(A[r * n + c]) > std::fabs(A[p * n + c])) p = r;
        if (std::fabs(A[p * n + c]) < 1e-12) return false;
        if (p != c) { for (int k = 0; k < n; ++k) std::swap(A[c * n + k], A[p * n + k]); std::swap(b[c], b[p]); }
        for (int r = c + 1; r < n; ++r) {
            const double f = A[r * n + c] / A[c * n + c];
            for (int k = c; k < n; ++k) A[r * n + k] -= f * A[c * n + k];
            b[r] -= f * b[c];
        }
    }
    for (int c = n - 1; c >= 0; --c) { for (int k = c + 1; k < n; ++k) b[c] -= A[c * n + k] * b[k]; b[c] /= A[c * n + c]; }
    return true;
}

// Background model: 30th-percentile block values on a coarse grid, blocks lifted by the galaxy or a bright star rejected iteratively and
// the gaps filled from the surrounding sky (normalised convolution), then interpolated to full size and subtracted. The smoothing scale
// (about 100 px) is small enough to follow sensor vignetting and fixed patterns, and the rejection stops it eating a galaxy's halo.
// Returns the residual pixel noise.
double flattenPlane(std::vector<float>& img, int w, int h)
{
    const int B = 48, bx = std::max(3, w / B), by = std::max(3, h / B);
    std::vector<float> g(size_t(bx) * by), wt(size_t(bx) * by, 1.f), G(size_t(bx) * by);
    std::vector<float> v;
    for (int y = 0; y < by; ++y) for (int x = 0; x < bx; ++x) {
        v.clear();
        for (int yy = 0; yy < B; yy += 2) for (int xx = 0; xx < B; xx += 2) { const int Y = y * B + yy, X = x * B + xx; if (Y < h && X < w) v.push_back(img[size_t(Y) * w + X]); }
        std::nth_element(v.begin(), v.begin() + v.size() * 3 / 10, v.end()); g[size_t(y) * bx + x] = v[v.size() * 3 / 10];
    }
    const double sgm = 2.0; const int R = 5;
    auto blur = [&](const std::vector<float>& src, std::vector<float>& dst) {
        for (int y = 0; y < by; ++y) for (int x = 0; x < bx; ++x) {
            double s = 0, n = 0;
            for (int dy = -R; dy <= R; ++dy) for (int dx = -R; dx <= R; ++dx) { const int Y = y + dy, X = x + dx; if (Y < 0 || X < 0 || Y >= by || X >= bx) continue; const double k = std::exp(-(dx * dx + dy * dy) / (2 * sgm * sgm)); s += k * src[size_t(Y) * bx + X]; n += k; }
            dst[size_t(y) * bx + x] = float(s / n);
        }
    };
    std::vector<float> gw(g.size()), num(g.size()), den(g.size());
    for (int it = 0; it < 4; ++it) {
        for (size_t i = 0; i < g.size(); ++i) gw[i] = g[i] * wt[i];
        blur(gw, num); blur(wt, den);
        for (size_t i = 0; i < g.size(); ++i) G[i] = num[i] / std::max(den[i], 1e-3f);
        std::vector<float> r; for (size_t i = 0; i < g.size(); ++i) if (wt[i] > 0) r.push_back(g[i] - G[i]);
        std::nth_element(r.begin(), r.begin() + r.size() / 2, r.end()); const float md = r[r.size() / 2];
        for (auto& t : r) t = std::fabs(t - md); std::nth_element(r.begin(), r.begin() + r.size() / 2, r.end()); const float sd = 1.4826f * r[r.size() / 2] + 1e-6f;
        for (size_t i = 0; i < g.size(); ++i) wt[i] = (g[i] - G[i] - md) > 1.5f * sd ? 0.f : 1.f;
    }
    // bilinear interpolation of the block model (block centres at (x + 0.5) * B)
    for (int y = 0; y < h; ++y) {
        const double fy = std::clamp((y + 0.5) / B - 0.5, 0.0, by - 1.0001); const int y0 = int(fy); const double ty = fy - y0;
        for (int x = 0; x < w; ++x) {
            const double fx = std::clamp((x + 0.5) / B - 0.5, 0.0, bx - 1.0001); const int x0 = int(fx); const double tx = fx - x0;
            const size_t i = size_t(y0) * bx + x0;
            img[size_t(y) * w + x] -= float((G[i] * (1 - tx) + G[i + 1] * tx) * (1 - ty) + (G[i + bx] * (1 - tx) + G[i + bx + 1] * tx) * ty);
        }
    }
    std::vector<float> s; for (int y = 0; y < h; y += 4) for (int x = 0; x < w; x += 4) s.push_back(img[size_t(y) * w + x]);
    std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end()); const float md = s[s.size() / 2];
    for (auto& t : s) t = std::fabs(t - md); std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
    return 1.4826 * s[s.size() / 2] + 1e-6;
}

// Row and column offsets, measured after flattening from sky pixels only: values within 2 sigma of zero, so stars and the galaxy's bright
// parts are excluded (a faint halo still contributes a little, but only a fraction of a line's pixels). Wide stripes are removed as well
// as narrow ones. Two rounds, rows then columns.
void removeBanding(std::vector<float>& img, int w, int h, double sigma)
{
    // Truncated mean over [-2, +0.5] sigma: the cut is asymmetric so that a faint halo or galaxy arm (a few tenths of a sigma per pixel) cannot
    // lift a line's estimate and carve a dark cross through the object; the constant bias of the truncation is the same for every line.
    const double lo = -2.0 * sigma, hi = 0.5 * sigma;
    for (int it = 0; it < 2; ++it) {
        for (int y = 0; y < h; ++y) {
            double s = 0; int n = 0; float* r = &img[size_t(y) * w];
            for (int x = 0; x < w; ++x) if (r[x] > lo && r[x] < hi) { s += r[x]; ++n; }
            if (n > w / 4) { const float m = float(s / n); for (int x = 0; x < w; ++x) r[x] -= m; }
        }
        for (int x = 0; x < w; ++x) {
            double s = 0; int n = 0;
            for (int y = 0; y < h; ++y) { const float v = img[size_t(y) * w + x]; if (v > lo && v < hi) { s += v; ++n; } }
            if (n > h / 4) { const float m = float(s / n); for (int y = 0; y < h; ++y) img[size_t(y) * w + x] -= m; }
        }
    }
}

inline float sampleBilinear(const std::vector<float>& img, int w, int h, double x, double y, bool& inside)
{
    if (x < 0 || y < 0 || x > w - 1.001 || y > h - 1.001) { inside = false; return 0; }
    inside = true;
    const int x0 = int(x), y0 = int(y); const double fx = x - x0, fy = y - y0;
    const size_t i = size_t(y0) * w + x0;
    return float((img[i] * (1 - fx) + img[i + 1] * fx) * (1 - fy) + (img[i + w] * (1 - fx) + img[i + w + 1] * fx) * fy);
}

}   // namespace

// ---- display stretch, ported from jrrk2/mkmosaic (VeraLux): background extraction, neutralisation, colour-preserving hyperbolic stretch ----
namespace {

float percentileOf(std::vector<float> v, double p)
{
    if (v.empty()) return 0;
    const size_t k = std::min(v.size() - 1, size_t(p / 100.0 * (v.size() - 1)));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

inline float lumaOf(const float* p) { return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]; }
inline float hyperbolicF(float v, float D, float b) { const float den = std::asinh(D + b) - std::asinh(b); return den < 1e-9f ? v : (std::asinh(D * v + b) - std::asinh(b)) / den; }
inline float mtfF(float v, float m) { if (v <= 0) return 0; if (v >= 1) return 1; const float den = (2 * m - 1) * v - m; return std::fabs(den) < 1e-9f ? v : std::clamp((m - 1) * v / den, 0.f, 1.f); }
inline float softClipF(float v) { const float thr = 0.98f; if (v <= thr) return v; const float t = std::clamp((v - thr) / (1 - thr), 0.f, 1.f); return thr + (1 - thr) * (1 - (1 - t) * (1 - t)); }

// Smooth sky model: cell medians, cells lifted by stars or the galaxy rejected iteratively, gaps filled by normalised Gaussian convolution
void extractBackground(std::vector<float>& plane, int w, int h, int cell, double sigmaCells)
{
    const int gx = w / cell, gy = h / cell; if (gx < 4 || gy < 4) return;
    std::vector<float> g(size_t(gx) * gy), wt(g.size(), 1.f), G(g.size()), v;
    for (int y = 0; y < gy; ++y) for (int x = 0; x < gx; ++x) {
        v.clear(); for (int yy = 0; yy < cell; yy += 2) for (int xx = 0; xx < cell; xx += 2) v.push_back(plane[size_t(y * cell + yy) * w + x * cell + xx]);
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); g[size_t(y) * gx + x] = v[v.size() / 2];
    }
    const int R = int(std::ceil(3 * sigmaCells));
    auto blur = [&](const std::vector<float>& s, std::vector<float>& d) {
        for (int y = 0; y < gy; ++y) for (int x = 0; x < gx; ++x) { double a = 0, n = 0;
            for (int dy = -R; dy <= R; ++dy) for (int dx = -R; dx <= R; ++dx) { const int Y = y + dy, X = x + dx; if (Y < 0 || X < 0 || Y >= gy || X >= gx) continue; const double k = std::exp(-(dx * dx + dy * dy) / (2 * sigmaCells * sigmaCells)); a += k * s[size_t(Y) * gx + X]; n += k; }
            d[size_t(y) * gx + x] = float(a / n); }
    };
    std::vector<float> gw(g.size()), num(g.size()), den(g.size());
    for (int it = 0; it < 4; ++it) {
        for (size_t i = 0; i < g.size(); ++i) gw[i] = g[i] * wt[i];
        blur(gw, num); blur(wt, den);
        for (size_t i = 0; i < g.size(); ++i) G[i] = num[i] / std::max(den[i], 1e-3f);
        std::vector<float> r; for (size_t i = 0; i < g.size(); ++i) if (wt[i] > 0) r.push_back(g[i] - G[i]);
        const float md = percentileOf(r, 50); for (auto& t : r) t = std::fabs(t - md); const float sd = 1.4826f * percentileOf(r, 50) + 1e-9f;
        for (size_t i = 0; i < g.size(); ++i) wt[i] = (g[i] - G[i] - md) > 1.2f * sd ? 0.f : 1.f;
    }
    const float keep = percentileOf(G, 50);
    for (int y = 0; y < h; ++y) {
        const double fy = std::clamp((y + 0.5) / cell - 0.5, 0.0, gy - 1.0001); const int y0 = int(fy); const double ty = fy - y0;
        for (int x = 0; x < w; ++x) {
            const double fx = std::clamp((x + 0.5) / cell - 0.5, 0.0, gx - 1.0001); const int x0 = int(fx); const double tx = fx - x0; const size_t i = size_t(y0) * gx + x0;
            const float m = float((G[i] * (1 - tx) + G[i + 1] * tx) * (1 - ty) + (G[i + gx] * (1 - tx) + G[i + gx + 1] * tx) * ty);
            plane[size_t(y) * w + x] = std::max(0.f, plane[size_t(y) * w + x] - m + keep);
        }
    }
}

}   // namespace

static QImage vlStretchToImage(std::vector<float> col[3], int w, int h)
{
    const size_t n = size_t(w) * h;
    // non-negative linear data: lift the noise floor, remove the sky model, normalise to the brightest pixels
    {
        std::vector<float> s; for (size_t i = 0; i < n; i += 7) s.push_back(col[1][i]);
        const float med = percentileOf(s, 50); for (auto& t : s) t = std::fabs(t - med); const float sg = 1.4826f * percentileOf(s, 50) + 1e-6f;
        for (int c = 0; c < 3; ++c) for (size_t i = 0; i < n; ++i) col[c][i] = std::max(0.f, col[c][i] + 4 * sg);
        for (int c = 0; c < 3; ++c) extractBackground(col[c], w, h, 48, 3.0);
    }
    std::vector<float> rgb(n * 3), lum(n);
    for (size_t i = 0; i < n; ++i) for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = col[c][i];
    { std::vector<float> s; for (size_t i = 0; i < n; i += 5) s.push_back(lumaOf(&rgb[i * 3])); const float top = std::max(percentileOf(s, 99.995), 1e-6f); for (auto& v : rgb) v = std::clamp(v / top, 0.f, 1.f); }
    // neutralise: R and B medians of the darkest 5% matched to G
    {
        std::vector<float> l; for (size_t i = 0; i < n; i += 4) l.push_back((rgb[i * 3] + rgb[i * 3 + 1] + rgb[i * 3 + 2]) / 3);
        const float thr = percentileOf(l, 5); std::vector<float> bg[3];
        for (size_t i = 0; i < n; i += 4) if ((rgb[i * 3] + rgb[i * 3 + 1] + rgb[i * 3 + 2]) / 3 <= thr) for (int c = 0; c < 3; ++c) bg[c].push_back(rgb[i * 3 + c]);
        if (bg[1].size() > 100) { const float mr = percentileOf(bg[0], 50), mg = percentileOf(bg[1], 50), mb = percentileOf(bg[2], 50); for (size_t i = 0; i < n; ++i) { rgb[i * 3] = std::clamp(rgb[i * 3] + mg - mr, 0.f, 1.f); rgb[i * 3 + 2] = std::clamp(rgb[i * 3 + 2] + mg - mb, 0.f, 1.f); } }
    }
    // hyperbolic stretch, strength solved so the median luminance lands on the target background
    const float targetBg = 0.10f, protectB = 5.f, conv = 3.5f;
    { std::vector<float> s; for (size_t i = 0; i < n; i += 5) s.push_back(lumaOf(&rgb[i * 3])); lum.assign(s.begin(), s.end()); }
    const float anchor = std::max(0.f, percentileOf(lum, 0.5) - 0.00025f);
    std::vector<float> ls; for (float v : lum) ls.push_back(std::max(0.f, v - anchor));
    const float med = percentileOf(ls, 50);
    double lo = 0, hi = 7, logD = 2;
    for (int it = 0; it < 40 && med > 1e-9f; ++it) { const double mid = 0.5 * (lo + hi); logD = mid; const float t = hyperbolicF(med, float(std::pow(10.0, mid)), protectB); if (std::fabs(t - targetBg) < 1e-4f) break; if (t < targetBg) lo = mid; else hi = mid; }
    const float D = float(std::pow(10.0, logD));
    for (size_t i = 0; i < n; ++i) {
        float* p = &rgb[i * 3];
        const float r = std::max(0.f, p[0] - anchor), g = std::max(0.f, p[1] - anchor), b = std::max(0.f, p[2] - anchor);
        const float L = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        const float Ls = std::clamp(hyperbolicF(L, D, protectB), 0.f, 1.f), k = std::pow(Ls, conv);
        const float in[3] = {r, g, b};
        for (int c = 0; c < 3; ++c) p[c] = std::clamp((Ls * ((in[c] / (L + 1e-9f)) * (1 - k) + k)) * 0.995f + 0.005f, 0.f, 1.f);
    }
    // adaptive floor/ceiling scaling and midtone transfer back to the target
    {
        std::vector<float> l; for (size_t i = 0; i < n; i += 5) l.push_back(lumaOf(&rgb[i * 3]));
        double mean = 0; for (float v : l) mean += v; mean /= l.size(); double var = 0; for (float v : l) var += (v - mean) * (v - mean); const float sd = float(std::sqrt(var / l.size()));
        const float mn = *std::min_element(l.begin(), l.end()), md = percentileOf(l, 50);
        const float floor_ = std::max(mn, md - 2.7f * sd); float soft = percentileOf(l, 99.0), hard = percentileOf(l, 99.99);
        if (soft <= floor_) soft = floor_ + 1e-6f; if (hard <= soft) hard = soft + 1e-6f;
        const float ped = 0.001f, scale = std::min((0.98f - ped) / (soft - floor_ + 1e-9f), (1.f - ped) / (hard - floor_ + 1e-9f));
        for (auto& v : rgb) v = std::clamp((v - floor_) * scale + ped, 0.f, 1.f);
        std::vector<float> l2; for (size_t i = 0; i < n; i += 5) l2.push_back(lumaOf(&rgb[i * 3]));
        const float cur = percentileOf(l2, 50);
        if (cur > 0 && cur < 1 && std::fabs(cur - targetBg) > 1e-3f) { const float m = cur * (targetBg - 1) / (cur * (2 * targetBg - 1) - targetBg); for (auto& v : rgb) v = mtfF(v, m); }
        for (auto& v : rgb) v = softClipF(v);
    }
    QImage img(w, h, QImage::Format_RGBX64);                           // 16 bits per channel
    for (int y = 0; y < h; ++y) {
        quint16* line = reinterpret_cast<quint16*>(img.scanLine(h - 1 - y));    // FITS rows run bottom-up
        for (int x = 0; x < w; ++x) { for (int c = 0; c < 3; ++c) line[4 * x + c] = quint16(rgb[(size_t(y) * w + x) * 3 + c] * 65535.f + 0.5f); line[4 * x + 3] = 65535; }
    }
    return img;
}

void LiveStacker::reset()
{
    m_w = m_h = m_frames = 0; m_totalExposure = m_refExposure = m_bestRaw = 0; m_recentRaw.clear(); m_haveLast = false; m_refStars.clear();
    for (int c = 0; c < 3; ++c) { m_sx[c].clear(); m_sx2[c].clear(); m_sw[c].clear(); }
}

LiveStacker::Report LiveStacker::registerFrame(const SkyFrame& f, double exposureS, Similarity* Tout) const
{
    Report rep;
    if (!f.valid) { rep.reason = "unreadable frame"; return rep; }
    if (m_frames == 0) { rep.reason = "no reference"; return rep; }
    if (f.width != m_w || f.height != m_h) { rep.reason = "frame size changed"; return rep; }
    RegResult rr = registerStars(m_refStars, f.stars, m_haveLast ? &m_last : nullptr);
    rep.inliers = rr.inliers; rep.rms = rr.rms;
    if (!rr.ok) { rep.reason = "no star-pattern match"; return rep; }
    rep.rotationDeg = rr.T.rotationDeg(); rep.shiftPx = std::hypot(rr.T.tx, rr.T.ty);
    // Transparency: exposure-normalised flux of matched stars relative to the reference's
    std::vector<double> ratios; int found = 0, brightest = 0;
    for (const SkyStar& r : m_refStars) {
        if (++brightest > 60) break;
        const SkyStar* hit = nullptr; double bd = 2.5;
        for (const SkyStar& t : f.stars) { double x, y; rr.T.apply(t.x, t.y, x, y); const double d = std::hypot(r.x - x, r.y - y); if (d < bd) { bd = d; hit = &t; } }
        if (hit && r.flux > 0) { ++found; ratios.push_back((hit->flux / exposureS) / (r.flux / m_refExposure)); }
    }
    rep.matchedFraction = brightest ? double(found) / std::min<int>(60, int(m_refStars.size())) : 0;
    for (const SkyStar& r : m_refStars) {                    // per-tile expectation, all reference stars
        const int tx = std::clamp(int(r.x * Report::kTilesX / m_w), 0, Report::kTilesX - 1), ty = std::clamp(int(r.y * Report::kTilesY / m_h), 0, Report::kTilesY - 1);
        ++rep.tileExpected[ty * Report::kTilesX + tx];
        for (const SkyStar& t : f.stars) { double x, y; rr.T.apply(t.x, t.y, x, y); if (std::hypot(r.x - x, r.y - y) < 2.5) { ++rep.tileFound[ty * Report::kTilesX + tx]; break; } }
    }
    rep.rawTransparency = ratios.size() >= 5 ? medianOf(ratios) : 0;
    if (Tout) *Tout = rr.T;
    rep.accepted = true;
    return rep;
}

LiveStacker::Report LiveStacker::assess(const SkyFrame& f, double exposureS) const
{
    Report r = registerFrame(f, exposureS, nullptr);
    if (r.accepted && m_bestRaw > 0) r.transparency = r.rawTransparency / std::max(m_bestRaw, r.rawTransparency);
    r.frames = m_frames;
    return r;
}

LiveStacker::Report LiveStacker::add(const SkyFrame& f, double exposureS, double minTransparency)
{
    Report rep;
    if (!f.valid) { rep.reason = "unreadable frame"; return rep; }
    if (exposureS <= 0) exposureS = 1;
    const int w = f.width, h = f.height;
    Similarity T;
    double scaleFactor = 1.0;

    if (m_frames == 0) {                                           // first frame defines the grid and the reference stars
        if (f.stars.size() < 12) { rep.reason = "too few stars for a reference"; return rep; }
        m_w = w; m_h = h; m_refExposure = exposureS;
        m_refStars.assign(f.stars.begin(), f.stars.begin() + std::min<size_t>(f.stars.size(), 150));
        for (int c = 0; c < 3; ++c) { m_sx[c].assign(size_t(w) * h, 0.0); m_sx2[c].assign(size_t(w) * h, 0.0); m_sw[c].assign(size_t(w) * h, 0.0); }
        T = Similarity();
        rep.accepted = true; rep.rawTransparency = 1.0; rep.transparency = 1.0; rep.inliers = int(m_refStars.size());
        m_bestRaw = 1.0;
        // white balance from the bright isolated stars: gains that make the median star neutral
        std::vector<double> gr, gb;
        for (const SkyStar& s : m_refStars) {
            if (!s.isolated) continue;
            double fr = 0, fg = 0, fb = 0, br = 0, bg = 0, bb = 0; int nb = 0;
            const int cx = int(std::lround(s.x)), cy = int(std::lround(s.y));
            for (int dy = -9; dy <= 9; ++dy) for (int dx = -9; dx <= 9; ++dx) {
                const int x = cx + dx, y = cy + dy; if (x < 0 || y < 0 || x >= w || y >= h) continue;
                const size_t i = size_t(y) * w + x; const double rr = std::hypot(dx, dy);
                if (rr <= 4) { fr += f.r[i]; fg += f.g[i]; fb += f.b[i]; } else if (rr >= 7) { br += f.r[i]; bg += f.g[i]; bb += f.b[i]; ++nb; }
            }
            if (nb < 20) continue;
            const double na = 0;  (void)na;
            int na2 = 0; for (int dy = -9; dy <= 9; ++dy) for (int dx = -9; dx <= 9; ++dx) if (std::hypot(dx, dy) <= 4) ++na2;
            fr -= br / nb * na2; fg -= bg / nb * na2; fb -= bb / nb * na2;
            if (fr > 0 && fg > 0 && fb > 0) { gr.push_back(fg / fr); gb.push_back(fg / fb); }
        }
        if (gr.size() >= 8) { m_wbR = medianOf(gr); m_wbB = medianOf(gb); }
    } else {
        rep = registerFrame(f, exposureS, &T);
        if (!rep.accepted) { rep.frames = m_frames; return rep; }
        if (rep.rawTransparency <= 0) { rep.accepted = false; rep.reason = "stars too faint to measure transparency"; rep.frames = m_frames; return rep; }
        m_bestRaw = std::max(m_bestRaw, rep.rawTransparency);
        rep.transparency = rep.rawTransparency / m_bestRaw;
        if (rep.transparency < minTransparency) { rep.accepted = false; rep.reason = QString("transparency %1 below %2").arg(rep.transparency, 0, 'f', 2).arg(minTransparency); rep.frames = m_frames; return rep; }
        scaleFactor = m_refExposure / (exposureS * rep.rawTransparency);
        m_last = T; m_haveLast = true;
    }

    // Preprocess each colour plane, then resample into the reference grid and accumulate
    const std::vector<float>* src[3] = {&f.r, &f.g, &f.b};
    const double c2 = T.a * T.a + T.b * T.b;
    for (int ch = 0; ch < 3; ++ch) {
        std::vector<float> img = *src[ch];
        const double noise = flattenPlane(img, w, h);
        removeBanding(img, w, h, noise);
        const double gain = (ch == 0 ? m_wbR : ch == 2 ? m_wbB : 1.0) * scaleFactor;
        const double wt = 1.0 / std::max(noise * gain * noise * gain, 1e-9);
        for (int y = 0; y < m_h; ++y)
            for (int x = 0; x < m_w; ++x) {
                const double qx = x - T.tx, qy = y - T.ty;
                const double px = (T.a * qx + T.b * qy) / c2, py = (-T.b * qx + T.a * qy) / c2;
                bool inside; const float v0 = sampleBilinear(img, w, h, px, py, inside);
                if (!inside) continue;
                const double v = v0 * gain; const size_t i = size_t(y) * m_w + x;
                if (m_frames >= 8 && m_sw[ch][i] > 0) {                       // running-variance clip
                    const double mean = m_sx[ch][i] / m_sw[ch][i];
                    const double var = std::max(0.0, m_sx2[ch][i] / m_sw[ch][i] - mean * mean);
                    const double sd = std::max(std::sqrt(var), 1.0 / std::sqrt(wt));
                    if (std::fabs(v - mean) > 5.0 * sd) continue;
                }
                m_sx[ch][i] += wt * v; m_sx2[ch][i] += wt * v * v; m_sw[ch][i] += wt;
            }
    }
    ++m_frames; m_totalExposure += exposureS;
    m_recentRaw.push_back(rep.rawTransparency);
    rep.accepted = true; rep.frames = m_frames;
    return rep;
}

QImage LiveStacker::preview() const
{
    if (m_frames == 0) return QImage();
    // Well-covered area: shrink the rectangle until every border row/column is at least 98% covered by 60% of the peak weight
    double mx = 0; for (double v : m_sw[1]) mx = std::max(mx, v);
    auto cov = [&](int x, int y) { return m_sw[1][size_t(y) * m_w + x] >= 0.6 * mx; };
    int x0 = 0, y0 = 0, x1 = m_w, y1 = m_h;
    for (int guard = 0; guard < 4000 && x1 - x0 > 32 && y1 - y0 > 32; ++guard) {
        auto frac = [&](int a, int b, bool horizontal, int fixed) { int n = 0; for (int k = a; k < b; ++k) n += horizontal ? cov(k, fixed) : cov(fixed, k); return double(n) / std::max(1, b - a); };
        const double t = frac(x0, x1, true, y0), bt = frac(x0, x1, true, y1 - 1), l = frac(y0, y1, false, x0), r = frac(y0, y1, false, x1 - 1);
        const double worst = std::min({t, bt, l, r});
        if (worst >= 0.98) break;
        if (worst == t) ++y0; else if (worst == bt) --y1; else if (worst == l) ++x0; else --x1;
    }
    const int w = x1 - x0, h = y1 - y0;
    std::vector<float> lum(size_t(w) * h), col[3];
    for (int c = 0; c < 3; ++c) col[c].resize(size_t(w) * h);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const size_t i = size_t(y + y0) * m_w + x + x0, o = size_t(y) * w + x;
        for (int c = 0; c < 3; ++c) col[c][o] = m_sw[c][i] > 0 ? float(m_sx[c][i] / m_sw[c][i]) : 0.f;
        lum[o] = (col[0][o] + col[1][o] + col[2][o]) / 3.f;
    }
    return vlStretchToImage(col, w, h);
}


QJsonObject LiveStacker::summary() const
{
    return QJsonObject{{"frames", m_frames}, {"total_exposure_s", m_totalExposure}, {"width", m_w}, {"height", m_h}, {"best_raw_transparency", m_bestRaw}};
}

namespace {
template <class T> void put(QByteArray& b, const T& v) { b.append(reinterpret_cast<const char*>(&v), sizeof(T)); }
template <class T> bool get(const QByteArray& b, qsizetype& pos, T& v) { if (pos + qsizetype(sizeof(T)) > b.size()) return false; memcpy(&v, b.constData() + pos, sizeof(T)); pos += sizeof(T); return true; }
}

bool LiveStacker::save(const QString& dir) const
{
    if (m_frames == 0) return false;
    QDir().mkpath(dir);
    QByteArray b; b.reserve(qsizetype(m_w) * m_h * 3 * 3 * 4 + 4096);
    put(b, quint32(0x4C535431));                                     // "LST1"
    put(b, qint32(m_w)); put(b, qint32(m_h)); put(b, qint32(m_frames));
    put(b, m_totalExposure); put(b, m_refExposure); put(b, m_bestRaw); put(b, m_wbR); put(b, m_wbB);
    put(b, qint32(m_refStars.size())); for (const SkyStar& s : m_refStars) { put(b, s.x); put(b, s.y); put(b, s.flux); put(b, s.peak); put(b, s.hfr); put(b, s.sigma); put(b, qint8(s.isolated)); }
    for (int c = 0; c < 3; ++c) for (size_t i = 0; i < m_sw[c].size(); ++i) { put(b, float(m_sx[c][i])); put(b, float(m_sx2[c][i])); put(b, float(m_sw[c][i])); }
    QFile f(dir + "/state.bin.tmp");
    if (!f.open(QIODevice::WriteOnly) || f.write(b) != b.size()) return false;
    f.close(); QFile::remove(dir + "/state.bin");
    if (!QFile::rename(dir + "/state.bin.tmp", dir + "/state.bin")) return false;
    preview().save(dir + "/preview.png");
    return true;
}

bool LiveStacker::load(const QString& dir)
{
    QFile f(dir + "/state.bin");
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray b = f.readAll(); qsizetype pos = 0;
    quint32 magic = 0; qint32 w = 0, h = 0, n = 0, ns = 0;
    LiveStacker t;
    if (!get(b, pos, magic) || magic != 0x4C535431 || !get(b, pos, w) || !get(b, pos, h) || !get(b, pos, n)) return false;
    if (w <= 0 || h <= 0 || w > 8000 || h > 8000) return false;
    if (!get(b, pos, t.m_totalExposure) || !get(b, pos, t.m_refExposure) || !get(b, pos, t.m_bestRaw) || !get(b, pos, t.m_wbR) || !get(b, pos, t.m_wbB) || !get(b, pos, ns) || ns < 0 || ns > 10000) return false;
    for (qint32 i = 0; i < ns; ++i) { SkyStar s; qint8 iso = 0; if (!get(b, pos, s.x) || !get(b, pos, s.y) || !get(b, pos, s.flux) || !get(b, pos, s.peak) || !get(b, pos, s.hfr) || !get(b, pos, s.sigma) || !get(b, pos, iso)) return false; s.isolated = iso; t.m_refStars.push_back(s); }
    t.m_w = w; t.m_h = h; t.m_frames = n;
    for (int c = 0; c < 3; ++c) {
        t.m_sx[c].resize(size_t(w) * h); t.m_sx2[c].resize(size_t(w) * h); t.m_sw[c].resize(size_t(w) * h);
        for (size_t i = 0; i < t.m_sw[c].size(); ++i) { float a, bb, cc; if (!get(b, pos, a) || !get(b, pos, bb) || !get(b, pos, cc)) return false; t.m_sx[c][i] = a; t.m_sx2[c][i] = bb; t.m_sw[c][i] = cc; }
    }
    *this = std::move(t);
    return true;
}

double skyGradient(const SkyFrame& f)
{
    if (!f.valid || f.width < 64) return 0;
    struct Pt { double u, v, val; };
    std::vector<Pt> pts;
    for (int y = 3; y < f.height; y += 6) for (int x = 3; x < f.width; x += 6) pts.push_back({2.0 * x / f.width - 1.0, 2.0 * y / f.height - 1.0, f.lum[size_t(y) * f.width + x]});
    std::vector<char> use(pts.size(), 1); double c[6] = {0, 0, 0, 0, 0, 0}, sigma = 1;
    auto basis = [](double u, double v, double* t) { t[0] = 1; t[1] = u; t[2] = v; t[3] = u * u; t[4] = u * v; t[5] = v * v; };
    for (int it = 0; it < 5; ++it) {
        std::vector<double> A(36, 0.0), b(6, 0.0);
        for (size_t i = 0; i < pts.size(); ++i) if (use[i]) { double t[6]; basis(pts[i].u, pts[i].v, t); for (int r = 0; r < 6; ++r) { b[r] += t[r] * pts[i].val; for (int k = 0; k < 6; ++k) A[r * 6 + k] += t[r] * t[k]; } }
        if (!solve(A, b, 6)) return 0;
        for (int k = 0; k < 6; ++k) c[k] = b[k];
        std::vector<double> res, ar;
        for (auto& p : pts) { double t[6]; basis(p.u, p.v, t); double m = 0; for (int k = 0; k < 6; ++k) m += c[k] * t[k]; res.push_back(p.val - m); }
        ar = res; for (auto& x : ar) x = std::fabs(x);
        sigma = 1.4826 * medianOf(ar) + 1e-6;
        for (size_t i = 0; i < pts.size(); ++i) use[i] = res[i] < 2.0 * sigma && res[i] > -3.0 * sigma;
    }
    double lo = 1e18, hi = -1e18;                               // extremes of the fitted surface over the frame
    for (double v = -1; v <= 1.001; v += 0.25) for (double u = -1; u <= 1.001; u += 0.25) { double t[6]; basis(u, v, t); double m = 0; for (int k = 0; k < 6; ++k) m += c[k] * t[k]; lo = std::min(lo, m); hi = std::max(hi, m); }
    return (hi - lo) / std::max(f.noise, 1e-6);
}
