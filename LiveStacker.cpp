#include "LiveStacker.hpp"
#include "GaiaCatalog.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <array>
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

// Smooth block model of a plane: frac-percentile block values on a coarse grid (B = 48 px), blocks lifted by the galaxy or a bright star rejected
// iteratively and the gaps filled from the surrounding sky (normalised convolution). G is the grid, block centres at (x + 0.5) * B.
struct BlockModel { static constexpr int B = 48; int bx = 0, by = 0; std::vector<float> G;
    float at(int x, int y) const {
        const double fy = std::clamp((y + 0.5) / B - 0.5, 0.0, by - 1.0001); const int y0 = int(fy); const double ty = fy - y0;
        const double fx = std::clamp((x + 0.5) / B - 0.5, 0.0, bx - 1.0001); const int x0 = int(fx); const double tx = fx - x0;
        const size_t i = size_t(y0) * bx + x0;
        return float((G[i] * (1 - tx) + G[i + 1] * tx) * (1 - ty) + (G[i + bx] * (1 - tx) + G[i + bx + 1] * tx) * ty);
    } };
BlockModel blockModel(const std::vector<float>& img, int w, int h, double frac)
{
    BlockModel m; const int B = BlockModel::B; m.bx = std::max(3, w / B); m.by = std::max(3, h / B);
    const int bx = m.bx, by = m.by;
    std::vector<float> g(size_t(bx) * by), wt(size_t(bx) * by, 1.f), G(size_t(bx) * by);
    std::vector<float> v;
    for (int y = 0; y < by; ++y) for (int x = 0; x < bx; ++x) {
        v.clear();
        for (int yy = 0; yy < B; yy += 2) for (int xx = 0; xx < B; xx += 2) { const int Y = y * B + yy, X = x * B + xx; if (Y < h && X < w) v.push_back(img[size_t(Y) * w + X]); }
        const size_t k = size_t(v.size() * frac); std::nth_element(v.begin(), v.begin() + k, v.end()); g[size_t(y) * bx + x] = v[k];
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
        for (auto& t : r) t = std::fabs(t - md);
        std::nth_element(r.begin(), r.begin() + r.size() / 2, r.end()); const float sd = 1.4826f * r[r.size() / 2] + 1e-6f;
        for (size_t i = 0; i < g.size(); ++i) wt[i] = (g[i] - G[i] - md) > 1.5f * sd ? 0.f : 1.f;
    }
    m.G = std::move(G);
    return m;
}

double robustSigma(const std::vector<float>& img, int w, int h)
{
    std::vector<float> s; for (int y = 0; y < h; y += 4) for (int x = 0; x < w; x += 4) s.push_back(img[size_t(y) * w + x]);
    std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end()); const float md = s[s.size() / 2];
    for (auto& t : s) t = std::fabs(t - md);
    std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
    return 1.4826 * s[s.size() / 2] + 1e-6;
}

// Background model: 30th-percentile block model, interpolated to full size and subtracted. The smoothing scale (about 100 px) is small enough to
// follow sensor vignetting and fixed patterns, and the rejection stops it eating a galaxy's halo. It also takes out anything wider than about
// 5 arcmin, and the frame's whole level: a nebula that fills the field is lost (see levelPlane). Returns the residual pixel noise.
// Trial flattening modes, compiled only with -DORIGIN_TRIAL_FLATTEN (then picked by $ORIGIN_FLATTEN=surface or =flat). Neither beat the plain block model
// (2026-11-24): kept for later experiments, out of the normal build.
#ifdef ORIGIN_TRIAL_FLATTEN
// A second-order surface through the frame's sky (64 px block 30th percentiles, clipped against the surface: stars, the target, a bright edge), removed before
// the block model. Alone, the block model sagged on the bright side of a steep gradient (it rejects blocks well above its own blurred fit, and on a slope those
// are the bright side's): a warm light low in the west put a red diagonal gradient into every M92 frame (2026-11-23) and its remainders added up to diagonal
// stripes in the stack. Trial, $ORIGIN_FLATTEN=surface (M92 2026-11-23: background structure G -10%, B -11%, R-B -9%; the stripes remain).
void subtractSurface(std::vector<float>& img, int w, int h)
{
    const int B = 64, bx = w / B, by = h / B; if (bx < 4 || by < 4) return;
    std::vector<double> X, Y, V; std::vector<float> v;
    for (int y = 0; y < by; ++y) for (int x = 0; x < bx; ++x) {
        v.clear();
        for (int yy = 0; yy < B; yy += 2) for (int xx = 0; xx < B; xx += 2) v.push_back(img[size_t(y * B + yy) * w + x * B + xx]);
        const size_t k = v.size() * 3 / 10; std::nth_element(v.begin(), v.begin() + k, v.end());
        X.push_back((x + 0.5) * B / w - 0.5); Y.push_back((y + 0.5) * B / h - 0.5); V.push_back(v[k]);
    }
    auto terms = [](double x, double y, double* t) { t[0] = 1; t[1] = x; t[2] = y; t[3] = x * x; t[4] = x * y; t[5] = y * y; };
    std::vector<char> use(V.size(), 1); double c[6] = {0, 0, 0, 0, 0, 0};
    for (int it = 0; it < 5; ++it) {
        std::vector<double> A(36, 0), b(6, 0); double t[6];
        for (size_t i = 0; i < V.size(); ++i) { if (!use[i]) continue; terms(X[i], Y[i], t); for (int r = 0; r < 6; ++r) { b[r] += t[r] * V[i]; for (int q = 0; q < 6; ++q) A[r * 6 + q] += t[r] * t[q]; } }
        if (!solve(A, b, 6)) return;
        for (int r = 0; r < 6; ++r) c[r] = b[r];
        std::vector<double> res(V.size()), a;
        for (size_t i = 0; i < V.size(); ++i) { terms(X[i], Y[i], t); double m = 0; for (int r = 0; r < 6; ++r) m += c[r] * t[r]; res[i] = V[i] - m; if (use[i]) a.push_back(std::fabs(res[i])); }
        if (a.size() < 12) return;
        const double sd = 1.4826 * medianOf(a) + 1e-6;
        for (size_t i = 0; i < V.size(); ++i) use[i] = res[i] < 2.5 * sd && res[i] > -4 * sd;      // stars and the target are bright: clip that side harder
    }
    for (int y = 0; y < h; ++y) { const double yy = (y + 0.5) / h - 0.5;
        for (int x = 0; x < w; ++x) { const double xx = (x + 0.5) / w - 0.5; img[size_t(y) * w + x] -= float(c[0] + c[1] * xx + c[2] * yy + c[3] * xx * xx + c[4] * xx * yy + c[5] * yy * yy); } }
}
#endif

double flattenPlane(std::vector<float>& img, int w, int h)
{
#ifdef ORIGIN_TRIAL_FLATTEN
    static const bool surface = qEnvironmentVariable("ORIGIN_FLATTEN") == "surface";
    if (surface) subtractSurface(img, w, h);
#endif
    const BlockModel m = blockModel(img, w, h, 0.3);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) img[size_t(y) * w + x] -= m.at(x, y);
    return robustSigma(img, w, h);
}

// $ORIGIN_FLATTEN=level: for a frame already divided by the master flat, remove only a plane (the sky's slope) and the level of the frame's darkest
// part, so emission wider than the frame keeps its level and structure: the field-filling nebulae (IC 1396, the Heart and Soul, the Rosette) came
// out as a dim brown sky (2026-11-22). Block medians, not the 30th percentile: the gap between them is half the pixel noise, which differs from
// night to night and left steps in the stack where the nights' coverage changes. hp gets the high-pass copy (the 100 px model removed) that the
// banding is measured on. Returns the residual pixel noise.
double levelPlane(std::vector<float>& img, int w, int h, std::vector<float>& hp)
{
    const BlockModel m = blockModel(img, w, h, 0.5);
    hp.resize(img.size());
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) hp[size_t(y) * w + x] = img[size_t(y) * w + x] - m.at(x, y);
    // plane through the block model, both bright (nebula) and dark (globules) blocks rejected
    std::vector<char> use(m.G.size(), 1); double c[3] = {0, 0, 0};
    for (int it = 0; it < 4; ++it) {
        std::vector<double> A(9, 0), b(3, 0);
        for (int y = 0; y < m.by; ++y) for (int x = 0; x < m.bx; ++x) {
            const size_t i = size_t(y) * m.bx + x; if (!use[i]) continue;
            const double t[3] = {1, (x + 0.5) * BlockModel::B / w - 0.5, (y + 0.5) * BlockModel::B / h - 0.5};
            for (int r = 0; r < 3; ++r) { b[r] += t[r] * m.G[i]; for (int k = 0; k < 3; ++k) A[r * 3 + k] += t[r] * t[k]; }
        }
        if (!solve(A, b, 3)) break;
        c[0] = b[0]; c[1] = b[1]; c[2] = b[2];
        std::vector<double> r;
        for (int y = 0; y < m.by; ++y) for (int x = 0; x < m.bx; ++x) r.push_back(m.G[size_t(y) * m.bx + x] - c[0] - c[1] * ((x + 0.5) * BlockModel::B / w - 0.5) - c[2] * ((y + 0.5) * BlockModel::B / h - 0.5));
        std::vector<double> a(r); for (double& t : a) t = std::fabs(t); const double sd = 1.4826 * medianOf(a) + 1e-6;
        for (size_t i = 0; i < r.size(); ++i) use[i] = std::fabs(r[i]) < 2 * sd;
    }
    auto plane = [&](double x, double y) { return c[0] + c[1] * ((x + 0.5) / w - 0.5) + c[2] * ((y + 0.5) / h - 0.5); };
    // the darkest part: the 5th percentile of the block model above the plane (smooth already, so not a noise dip)
    std::vector<double> d;
    for (int y = 0; y < m.by; ++y) for (int x = 0; x < m.bx; ++x) d.push_back(m.G[size_t(y) * m.bx + x] - plane((x + 0.5) * BlockModel::B, (y + 0.5) * BlockModel::B));
    std::nth_element(d.begin(), d.begin() + d.size() / 20, d.end()); const double floorLevel = d[d.size() / 20];
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) img[size_t(y) * w + x] -= float(plane(x, y) + floorLevel);
    return robustSigma(hp, w, h);
}

bool levelMode() { static const bool on = qEnvironmentVariable("ORIGIN_FLATTEN") == "level"; return on; }

// $ORIGIN_MASTER_FLAT (with ORIGIN_FLATTEN=level or flat): a master flat from OriginHeadless flats, planes R, G, B at half resolution. Each plane's own plane
// fit is divided out first: a twilight flat carries the sky's slope (40% across the 15 Oct one), not just the optics' vignetting. Empty when absent.
const std::vector<float>& masterFlat(int c, int w, int h)
{
    static std::vector<float> planes[3]; static bool tried = false; static int fw = 0, fh = 0;
    static const std::vector<float> none;
    if (!tried) {
        tried = true;
        QFile f(qEnvironmentVariable("ORIGIN_MASTER_FLAT"));
        if (!f.fileName().isEmpty() && f.open(QIODevice::ReadOnly)) {
            const QByteArray b = f.readAll(); int n1 = 0, n2 = 0, n3 = 0, bitpix = 0; qsizetype pos = 0; bool end = false;
            for (; pos + 80 <= b.size() && !end; pos += 80) {
                const QByteArray card = b.mid(pos, 80), key = card.left(8).trimmed(), val = card.mid(10, 20).trimmed();
                if (key == "END") end = true; else if (key == "BITPIX") bitpix = val.toInt(); else if (key == "NAXIS1") n1 = val.toInt(); else if (key == "NAXIS2") n2 = val.toInt(); else if (key == "NAXIS3") n3 = val.toInt();
            }
            pos = (pos + 2879) / 2880 * 2880;
            if (end && bitpix == -32 && n3 == 3 && n1 > 0 && n2 > 0 && pos + qsizetype(n1) * n2 * 12 <= b.size()) {
                fw = n1; fh = n2;
                for (int p = 0; p < 3; ++p) {
                    planes[p].resize(size_t(n1) * n2);
                    for (size_t i = 0; i < planes[p].size(); ++i) {
                        const uchar* q = reinterpret_cast<const uchar*>(b.constData() + pos + (qsizetype(p) * n1 * n2 + qsizetype(i)) * 4);
                        const quint32 u = (quint32(q[0]) << 24) | (quint32(q[1]) << 16) | (quint32(q[2]) << 8) | q[3]; float v; memcpy(&v, &u, 4); planes[p][i] = v;
                    }
                    // least-squares plane over the plane's pixels (every 4th), divided out
                    std::vector<double> A(9, 0), r(3, 0);
                    for (int y = 0; y < n2; y += 4) for (int x = 0; x < n1; x += 4) {
                        const double t[3] = {1, (x + 0.5) / n1 - 0.5, (y + 0.5) / n2 - 0.5}, v = planes[p][size_t(y) * n1 + x];
                        for (int i = 0; i < 3; ++i) { r[i] += t[i] * v; for (int k = 0; k < 3; ++k) A[i * 3 + k] += t[i] * t[k]; }
                    }
                    if (!solve(A, r, 3)) { planes[p].clear(); continue; }
                    for (int y = 0; y < n2; ++y) for (int x = 0; x < n1; ++x) {
                        float& v = planes[p][size_t(y) * n1 + x];
                        v = float(v / (r[1] * ((x + 0.5) / n1 - 0.5) + r[2] * ((y + 0.5) / n2 - 0.5) + r[0]));
                        if (!(v > 0.2f)) v = 1.f;
                    }
                }
            }
        }
    }
    return fw == w && fh == h ? planes[c] : none;
}

// Row and column offsets, measured after flattening from sky pixels only: values within 2 sigma of zero, so stars and the galaxy's bright
// parts are excluded (a faint halo still contributes a little, but only a fraction of a line's pixels). Wide stripes are removed as well
// as narrow ones. Two rounds, rows then columns.
void removeBanding(std::vector<float>& img, int w, int h, double sigma, std::vector<float>* measure = nullptr)
{
    // measure: measure the offsets on this copy instead (the high-pass one, whose sky is at zero) and take them out of both
    std::vector<float>& ms = measure ? *measure : img;
    // Truncated mean over [-2, +0.5] sigma: the cut is asymmetric so that a faint halo or galaxy arm (a few tenths of a sigma per pixel) cannot
    // lift a line's estimate and carve a dark cross through the object; the constant bias of the truncation is the same for every line.
    const double lo = -2.0 * sigma, hi = 0.5 * sigma;
    for (int it = 0; it < 2; ++it) {
        for (int y = 0; y < h; ++y) {
            double s = 0; int n = 0; const float* r = &ms[size_t(y) * w];
            for (int x = 0; x < w; ++x) if (r[x] > lo && r[x] < hi) { s += r[x]; ++n; }
            if (n > w / 4) { const float m = float(s / n); for (int x = 0; x < w; ++x) { img[size_t(y) * w + x] -= m; if (measure) ms[size_t(y) * w + x] -= m; } }
        }
        for (int x = 0; x < w; ++x) {
            double s = 0; int n = 0;
            for (int y = 0; y < h; ++y) { const float v = ms[size_t(y) * w + x]; if (v > lo && v < hi) { s += v; ++n; } }
            if (n > h / 4) { const float m = float(s / n); for (int y = 0; y < h; ++y) { img[size_t(y) * w + x] -= m; if (measure) ms[size_t(y) * w + x] -= m; } }
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

// ---- display stretch: background extraction, neutralisation, colour-preserving hyperbolic stretch. A port of VeraLux HyperMetric Stretch by Riccardo Paterniti
// (GPL-3.0-or-later), by way of jrrk2/mkmosaic; VeraLux's published algorithm only, no PixInsight/PCL code ----
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

// Sky levels R, G, B to neutralise on. Normally each channel's median over the frame, which is mostly sky once the model is removed (picking the
// darkest pixels by mean brightness instead selects the noise dips of the noisiest channel, R, and turned the background red). With ORIGIN_FLATTEN=level
// a nebula can fill the frame, and its median is the nebula: the mean of the darkest 5% of 32 px block medians instead (the dark globules), where
// the block medians are smooth enough not to be noise dips.
static std::array<float, 3> skyLevels(const std::vector<float>& rgb, int w, int h)
{
    std::array<float, 3> m{};
    if (!levelMode()) {
        std::vector<float> bg[3]; for (size_t i = 0; i < size_t(w) * h; i += 4) for (int c = 0; c < 3; ++c) bg[c].push_back(rgb[i * 3 + c]);
        for (int c = 0; c < 3; ++c) m[c] = percentileOf(bg[c], 50);
        return m;
    }
    const int B = 32, bx = w / B, by = h / B;
    struct Blk { float l, v[3]; }; std::vector<Blk> blk; std::vector<float> v[3];
    for (int y = 0; y < by; ++y) for (int x = 0; x < bx; ++x) {
        for (int c = 0; c < 3; ++c) v[c].clear();
        for (int yy = 0; yy < B; yy += 2) for (int xx = 0; xx < B; xx += 2) { const size_t i = size_t(y * B + yy) * w + x * B + xx; for (int c = 0; c < 3; ++c) v[c].push_back(rgb[i * 3 + c]); }
        Blk k; for (int c = 0; c < 3; ++c) k.v[c] = percentileOf(v[c], 50);
        k.l = 0.2126f * k.v[0] + 0.7152f * k.v[1] + 0.0722f * k.v[2]; blk.push_back(k);
    }
    if (blk.size() < 40) { m = {0, 0, 0}; return m; }
    std::sort(blk.begin(), blk.end(), [](const Blk& a, const Blk& b) { return a.l < b.l; });
    const size_t n = std::max<size_t>(4, blk.size() / 20);
    for (size_t i = 0; i < n; ++i) for (int c = 0; c < 3; ++c) m[c] += blk[i].v[c] / n;
    return m;
}

static Image16 vlStretchToImage(std::vector<float> col[3], int w, int h)
{
    const size_t n = size_t(w) * h;
    // non-negative linear data: lift the noise floor, remove the sky model, normalise to the brightest pixels
    {
        std::vector<float> s; for (size_t i = 0; i < n; i += 7) s.push_back(col[1][i]);
        const float med = percentileOf(s, 50); for (auto& t : s) t = std::fabs(t - med); const float sg = 1.4826f * percentileOf(s, 50) + 1e-6f;
        for (int c = 0; c < 3; ++c) for (size_t i = 0; i < n; ++i) col[c][i] = std::max(0.f, col[c][i] + 4 * sg);
        if (!levelMode()) for (int c = 0; c < 3; ++c) extractBackground(col[c], w, h, 48, 3.0);
    }
    std::vector<float> rgb(n * 3), lum(n);
    for (size_t i = 0; i < n; ++i) for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = col[c][i];
    { std::vector<float> s; for (size_t i = 0; i < n; i += 5) s.push_back(lumaOf(&rgb[i * 3])); const float top = std::max(percentileOf(s, 99.995), 1e-6f); for (auto& v : rgb) v = std::clamp(v / top, 0.f, 1.f); }
    // neutralise: R and B sky levels matched to G. Each channel's own median over the whole frame, which is mostly sky once the model is removed;
    // picking the darkest pixels by mean brightness instead selects the noise dips of the noisiest channel (R) and turned the background red
    {
        const std::array<float, 3> sky = skyLevels(rgb, w, h); const float mr = sky[0], mg = sky[1], mb = sky[2];
        for (size_t i = 0; i < n; ++i) { rgb[i * 3] = std::clamp(rgb[i * 3] + mg - mr, 0.f, 1.f); rgb[i * 3 + 2] = std::clamp(rgb[i * 3 + 2] + mg - mb, 0.f, 1.f); }
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
        if (soft <= floor_) soft = floor_ + 1e-6f;
        if (hard <= soft) hard = soft + 1e-6f;
        const float ped = 0.001f, scale = std::min((0.98f - ped) / (soft - floor_ + 1e-9f), (1.f - ped) / (hard - floor_ + 1e-9f));
        for (auto& v : rgb) v = std::clamp((v - floor_) * scale + ped, 0.f, 1.f);
        std::vector<float> l2; for (size_t i = 0; i < n; i += 5) l2.push_back(lumaOf(&rgb[i * 3]));
        const float cur = percentileOf(l2, 50);
        if (cur > 0 && cur < 1 && std::fabs(cur - targetBg) > 1e-3f) { const float m = cur * (targetBg - 1) / (cur * (2 * targetBg - 1) - targetBg); for (auto& v : rgb) v = mtfF(v, m); }
        // the stretch treats the noisier R and B planes differently from G, so match the sky medians once more where they are seen
        const std::array<float, 3> sky = skyLevels(rgb, w, h); const float mr = sky[0], mg = sky[1], mb = sky[2];
        for (size_t i = 0; i < n; ++i) { rgb[i * 3] = std::clamp(rgb[i * 3] + mg - mr, 0.f, 1.f); rgb[i * 3 + 2] = std::clamp(rgb[i * 3 + 2] + mg - mb, 0.f, 1.f); }
        for (auto& v : rgb) v = softClipF(v);
    }
    Image16 img = Image16::make(w, h);
    for (int y = 0; y < h; ++y) {
        quint16* line = img.row(h - 1 - y);                                     // FITS rows run bottom-up
        for (int x = 0; x < w; ++x) for (int c = 0; c < 3; ++c) line[3 * x + c] = quint16(rgb[(size_t(y) * w + x) * 3 + c] * 65535.f + 0.5f);
    }
    return img;
}

void LiveStacker::reset()
{
    m_w = m_h = m_frames = m_refIso = 0; m_refFilter.clear(); m_totalExposure = m_refExposure = m_bestRaw = 0; m_recentRaw.clear(); m_haveLast = false; m_refStars.clear();
    m_pointRa = 0; m_pointDec = -999; m_ccR = m_ccB = 1; m_ccStars = m_ccFrames = 0; m_ccReport = QJsonObject(); m_fixedLayout = false; m_firstCapture = m_lastCapture = QDateTime();
    for (int c = 0; c < 3; ++c) { m_sx[c].clear(); m_sx2[c].clear(); m_sw[c].clear(); }
}

LiveStacker::Report LiveStacker::registerFrame(const SkyFrame& f, double exposureS, int iso, Similarity* Tout) const
{
    Report rep;
    if (!f.valid) { rep.reason = "unreadable frame"; return rep; }
    if (m_frames == 0) { rep.reason = "no reference"; return rep; }
    if (f.width != m_w || f.height != m_h) { rep.reason = "frame size changed"; return rep; }
    RegResult rr = registerStars(m_refStars, f.stars, m_haveLast ? &m_last : nullptr);
    rep.inliers = rr.inliers; rep.rms = rr.rms;
    if (!rr.ok) { rep.reason = "no star-pattern match"; return rep; }
    rep.rotationDeg = rr.T.rotationDeg(); rep.shiftPx = std::hypot(rr.T.tx, rr.T.ty);
    // Transparency: flux of matched stars per second and per unit of ISO gain, relative to the reference's. Without the ISO term a frame at ISO 200 against an ISO 800 reference read
    // transparency 0.25 on a clear sky, and the supervisor's ISO fallback turned every frame "cloudy" (2026-10-27 21:00)
    const double expGain = exposureS * isoScale(iso);
    // ratios from the first 60 reference stars this frame also has (not the 60 brightest: a long frame drops the brightest as saturated, and 155 M31 frames read "too faint")
    std::vector<double> ratios; int found = 0, brightest = 0;
    for (const SkyStar& r : m_refStars) {
        if (ratios.size() >= 60) break;
        ++brightest;
        const SkyStar* hit = nullptr; double bd = 2.5;
        for (const SkyStar& t : f.stars) { double x, y; rr.T.apply(t.x, t.y, x, y); const double d = std::hypot(r.x - x, r.y - y); if (d < bd) { bd = d; hit = &t; } }
        if (hit && r.flux > 0) { if (brightest <= 60) ++found; ratios.push_back((hit->flux / expGain) / (r.flux / m_refExposure)); }
    }
    rep.matchedFraction = double(found) / std::max(1, std::min<int>(60, int(m_refStars.size())));   // of the 60 brightest reference stars
    // per-tile expectation: the reference stars that fall inside this frame. After a fresh goto the field can be shifted and rotated against the reference (2026-10-27: 346 px, 22 deg on the Heart);
    // stars outside the frame cannot be seen, and counting them made two corner tiles look empty, "patchy" on a clear sky, which set off the run's ISO fallback
    const double c2 = rr.T.a * rr.T.a + rr.T.b * rr.T.b;
    for (const SkyStar& r : m_refStars) {
        if (c2 > 0) {
            const double qx = r.x - rr.T.tx, qy = r.y - rr.T.ty, fx = (rr.T.a * qx + rr.T.b * qy) / c2, fy = (-rr.T.b * qx + rr.T.a * qy) / c2;
            if (fx < 8 || fy < 8 || fx >= f.width - 8 || fy >= f.height - 8) continue;
        }
        const int tx = std::clamp(int(r.x * Report::kTilesX / m_w), 0, Report::kTilesX - 1), ty = std::clamp(int(r.y * Report::kTilesY / m_h), 0, Report::kTilesY - 1);
        ++rep.tileExpected[ty * Report::kTilesX + tx];
        for (const SkyStar& t : f.stars) { double x, y; rr.T.apply(t.x, t.y, x, y); if (std::hypot(r.x - x, r.y - y) < 2.5) { ++rep.tileFound[ty * Report::kTilesX + tx]; break; } }
    }
    rep.rawTransparency = ratios.size() >= 5 ? medianOf(ratios) : 0;
    if (Tout) *Tout = rr.T;
    rep.accepted = true;
    return rep;
}

LiveStacker::Report LiveStacker::assess(const SkyFrame& f, double exposureS, int iso) const
{
    Report r = registerFrame(f, exposureS, iso, nullptr);
    if (r.accepted && m_bestRaw > 0) r.transparency = r.rawTransparency / std::max(m_bestRaw, r.rawTransparency);
    r.frames = m_frames;
    return r;
}

LiveStacker::Report LiveStacker::add(const SkyFrame& f, double exposureS, double minTransparency, int iso, const QString& filter, const QDateTime& captured)
{
    Report rep;
    if (!f.valid) { rep.reason = "unreadable frame"; return rep; }
    if (exposureS <= 0) exposureS = 1;
    const int w = f.width, h = f.height;
    Similarity T;
    double scaleFactor = 1.0;

    if (m_frames == 0) {                                           // first frame defines the grid and the reference stars
        if (f.stars.size() < 12) { rep.reason = "too few stars for a reference"; return rep; }
        m_w = w; m_h = h; m_refExposure = exposureS; m_refIso = std::max(0, iso); m_refFilter = filter;
        m_fixedLayout = qEnvironmentVariable("ORIGIN_BAYER_GREEN", "main").toLower() != "vote";
        // 300 (was 150): detection skips saturated stars, so frames of very different exposure share only the fainter part of their lists (M1, 4 s against 17 s, 2026-10-14)
        m_refStars.assign(f.stars.begin(), f.stars.begin() + std::min<size_t>(f.stars.size(), 300));
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
        rep = registerFrame(f, exposureS, iso, &T);
        if (!rep.accepted) { rep.frames = m_frames; return rep; }
        if (rep.rawTransparency <= 0) { rep.accepted = false; rep.reason = "stars too faint to measure transparency"; rep.frames = m_frames; return rep; }
        m_bestRaw = std::max(m_bestRaw, rep.rawTransparency);
        rep.transparency = rep.rawTransparency / m_bestRaw;
        if (rep.transparency < minTransparency) { rep.accepted = false; rep.reason = QString("transparency %1 below %2").arg(rep.transparency, 0, 'f', 2).arg(minTransparency); rep.frames = m_frames; return rep; }
        scaleFactor = m_refExposure / (exposureS * isoScale(iso) * rep.rawTransparency);
        m_last = T; m_haveLast = true;
    }

    // Preprocess each colour plane, then resample into the reference grid and accumulate
    const std::vector<float>* src[3] = {&f.r, &f.g, &f.b};
    const double c2 = T.a * T.a + T.b * T.b;
    for (int ch = 0; ch < 3; ++ch) {
        std::vector<float> img = *src[ch];
        double noise;
        if (levelMode()) {
            const std::vector<float>& flat = masterFlat(ch, w, h);
            // black level off first, or the division imprints the vignetting on it ($ORIGIN_BLACK_LEVEL, 4985 as SkyMonitor; uncertain by a few
            // hundred ADU without dark frames, which leaves a faint radial pattern of that much times the vignetting)
            static const double black = qEnvironmentVariableIsSet("ORIGIN_BLACK_LEVEL") ? qEnvironmentVariable("ORIGIN_BLACK_LEVEL").toDouble() : 4985.0;
            if (!flat.empty()) for (size_t i = 0; i < img.size(); ++i) img[i] = float((img[i] - black) / flat[i]);
            std::vector<float> hp;
            noise = levelPlane(img, w, h, hp);
            removeBanding(img, w, h, noise, &hp);
        } else {
#ifdef ORIGIN_TRIAL_FLATTEN
            // $ORIGIN_FLATTEN=flat (trial, 2026-11-24): the master flat's fixed pattern (vignetting, dust) divided out first, then the usual per-frame
            // background model, which still removes the sky's own gradients. Level mode alone left them in (M1 12 Nov: background 2x as uneven); this
            // was still worse than no flat with our flat or the factory one, black at 4985 or 4685 (corners 3.0-4.4 vs 1.3 noise units)
            static const bool flatFirst = qEnvironmentVariable("ORIGIN_FLATTEN") == "flat";
            if (flatFirst) {
                const std::vector<float>& flat = masterFlat(ch, w, h);
                static const double black = qEnvironmentVariableIsSet("ORIGIN_BLACK_LEVEL") ? qEnvironmentVariable("ORIGIN_BLACK_LEVEL").toDouble() : 4985.0;
                if (!flat.empty()) for (size_t i = 0; i < img.size(); ++i) img[i] = float((img[i] - black) / flat[i]);
            }
#endif
            noise = flattenPlane(img, w, h);
            removeBanding(img, w, h, noise);
        }
        const double gain = (ch == 0 ? m_wbR : ch == 2 ? m_wbB : 1.0) * scaleFactor;
        const double wt = 1.0 / std::max(noise * gain * noise * gain, 1e-9);
        // ORIGIN_FLATTEN=level keeps each frame's level, which differs from night to night (the Moon, the sky's brightness, which part of the field was
        // darkest): matched to the stack's over the overlap before it is added (the median difference on an 8 px grid), or every edge of a night's
        // coverage is a step (a straight diagonal across IC 1396, 2026-11-22). The first frame sets the level.
        double offset = 0;
        if (levelMode() && m_frames > 0) {
            std::vector<double> d;
            for (int y = 4; y < m_h; y += 8) for (int x = 4; x < m_w; x += 8) {
                const size_t i = size_t(y) * m_w + x; if (m_sw[ch][i] <= 0) continue;
                const double qx = x - T.tx, qy = y - T.ty;
                bool inside; const float v0 = sampleBilinear(img, w, h, (T.a * qx + T.b * qy) / c2, (-T.b * qx + T.a * qy) / c2, inside);
                if (inside) d.push_back(v0 * gain - m_sx[ch][i] / m_sw[ch][i]);
            }
            if (d.size() > 200) offset = medianOf(d);
        }
        for (int y = 0; y < m_h; ++y)
            for (int x = 0; x < m_w; ++x) {
                const double qx = x - T.tx, qy = y - T.ty;
                const double px = (T.a * qx + T.b * qy) / c2, py = (-T.b * qx + T.a * qy) / c2;
                bool inside; const float v0 = sampleBilinear(img, w, h, px, py, inside);
                if (!inside) continue;
                const double v = v0 * gain - offset; const size_t i = size_t(y) * m_w + x;
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
    if (captured.isValid()) {
        const QDateTime t = captured.toUTC();
        if (!m_firstCapture.isValid() || t < m_firstCapture) m_firstCapture = t;
        if (!m_lastCapture.isValid() || t > m_lastCapture) m_lastCapture = t;
    }
    m_recentRaw.push_back(rep.rawTransparency);
    rep.accepted = true; rep.frames = m_frames;
    return rep;
}

Image16 LiveStacker::preview() const
{
    if (m_frames == 0) return Image16();
    // Well-covered area: shrink the rectangle until every border row/column is at least 98% covered by 85% of the peak weight (60% until 2026-11-22
    // left wedges of the thinner coverage at the edges, 10-15% darker where fewer nights overlap)
    double mx = 0; for (double v : m_sw[1]) mx = std::max(mx, v);
    auto cov = [&](int x, int y) { return m_sw[1][size_t(y) * m_w + x] >= 0.85 * mx; };
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
        for (int c = 0; c < 3; ++c) col[c][o] = m_sw[c][i] > 0 ? float(m_sx[c][i] / m_sw[c][i] * (c == 0 ? m_ccR : c == 2 ? m_ccB : 1.0)) : 0.f;
        lum[o] = (col[0][o] + col[1][o] + col[2][o]) / 3.f;
    }
    return vlStretchToImage(col, w, h);
}

QJsonObject LiveStacker::calibrateColour(double raDeg, double decDeg, const QString& catalog, double arcsecPerPx, double marginDeg)
{
    QJsonObject o{{"ok", false}, {"frames", m_frames}};
    auto fail = [&](const QString& why) { o["reason"] = why; return o; };
    if (m_frames == 0) return fail("empty stack");
    const QString path = catalog.isEmpty() ? gaiaCatalogPath() : catalog;
    if (path.isEmpty()) return fail("no Gaia catalogue (tools/gaia_fetch.py, tools/gaia_pack.py)");
    const double s = arcsecPerPx / 3600.0, halfDiag = 0.5 * std::hypot(m_w, m_h) * s;
    const QDate today = QDateTime::currentDateTimeUtc().date();
    const double epoch = today.year() + (today.dayOfYear() - 0.5) / today.daysInYear();
    // The widest cone tried; narrower ones are subsets of it
    const double margins[] = {std::min(0.05, marginDeg), marginDeg, std::max(0.5, marginDeg)};
    std::vector<GaiaStar> cat;
    if (!gaiaConeSearch(path, raDeg, decDeg, halfDiag + margins[2], 14.0, epoch, cat)) return fail("cannot read " + path);
    if (cat.size() < 30) return fail(QString("only %1 Gaia stars around the pointing").arg(cat.size()));
    std::vector<double> xi(cat.size()), eta(cat.size()), sep(cat.size());
    for (size_t i = 0; i < cat.size(); ++i) { gnomonic(cat[i].raDeg, cat[i].decDeg, raDeg, decDeg, xi[i], eta[i]); sep[i] = std::hypot(xi[i], eta[i]); }

    // Match: Gaia's brightest stars on a tangent plane at the stack's scale, registered onto the reference stars like another frame. The sky is mirrored
    // or not depending on the optics, so both parities are tried. Which Gaia stars to offer matters: the reference list has no saturated stars (G brighter
    // than about 9 to 10.5, depending on the exposure), and a cone much wider than the pointing error buries the frame's stars among ones outside it. So a few
    // brightness floors and cone widths are tried, narrowest first, until a match has 25 inliers; the best match found is kept. The pair voting stops at the
    // 160 brightest of each list (the deeper passes cost seconds each in a rich field such as the Double Cluster): the floors stand in for depth.
    Similarity T; int parity = 0; RegResult best;
    std::vector<SkyStar> ref = m_refStars;                 // brightest first, as the Gaia list is (the saved list is not always in flux order)
    std::sort(ref.begin(), ref.end(), [](const SkyStar& a, const SkyStar& b) { return a.flux > b.flux; });
    auto attempt = [&](double margin, double gFloor, int par, const Similarity* guess) {
        std::vector<SkyStar> g;
        for (size_t i = 0; i < cat.size() && g.size() < 300; ++i) {
            if (cat[i].g < gFloor || sep[i] > halfDiag + margin) continue;
            SkyStar t; t.x = float(0.5 * m_w + par * xi[i] / s); t.y = float(0.5 * m_h + eta[i] / s); t.flux = float(std::pow(10.0, -0.4 * cat[i].g)); g.push_back(t);
        }
        const RegResult rr = registerStars(ref, g, guess, 10, 160);
        if (rr.ok && rr.inliers > best.inliers) { best = rr; parity = par; o["match"] = QJsonObject{{"ra_deg", raDeg}, {"dec_deg", decDeg}, {"g_floor", gFloor}, {"margin_deg", margin}, {"parity", par},
                                                                                                {"a", rr.T.a}, {"b", rr.T.b}, {"tx", rr.T.tx}, {"ty", rr.T.ty}}; }
    };
    // the previous calibration's match from the same pointing, as the starting guess: nearly instant
    const QJsonObject prev = m_ccReport["match"].toObject();
    if (!prev.isEmpty() && std::fabs(prev["ra_deg"].toDouble() - raDeg) < 1e-6 && std::fabs(prev["dec_deg"].toDouble() - decDeg) < 1e-6) {
        Similarity G; G.a = prev["a"].toDouble(); G.b = prev["b"].toDouble(); G.tx = prev["tx"].toDouble(); G.ty = prev["ty"].toDouble();
        attempt(prev["margin_deg"].toDouble(), prev["g_floor"].toDouble(), prev["parity"].toInt(), &G);
    }
    for (const double margin : margins) {
        if (best.inliers >= 25) break;
        for (double gFloor = 8.0; gFloor <= 12.6 && best.inliers < 25; gFloor += 0.5)
            for (const int par : {1, -1}) attempt(margin, gFloor, par, nullptr);
    }
    if (!best.ok) return fail(QString("the reference stars do not match Gaia around RA %1 Dec %2").arg(raDeg, 0, 'f', 3).arg(decDeg, 0, 'f', 3));
    T = best.T;
    o["match_inliers"] = best.inliers; o["match_rms_px"] = best.rms;
    o["scale_arcsec_px"] = arcsecPerPx / T.scale(); o["rotation_deg"] = T.rotationDeg();
    {   // where the stack's centre is on the sky
        double cx = 0.5 * m_w, cy = 0.5 * m_h;
        const double c2 = T.a * T.a + T.b * T.b, qx = cx - T.tx, qy = cy - T.ty;
        const double gx = (T.a * qx + T.b * qy) / c2, gy = (-T.b * qx + T.a * qy) / c2;
        const double x = parity * (gx - 0.5 * m_w) * s * M_PI / 180, y = (gy - 0.5 * m_h) * s * M_PI / 180, d0 = decDeg * M_PI / 180, rho = std::hypot(x, y), c = std::atan(rho);
        const double dec = rho < 1e-12 ? d0 : std::asin(std::cos(c) * std::sin(d0) + y * std::sin(c) * std::cos(d0) / rho);
        const double ra = raDeg * M_PI / 180 + std::atan2(x * std::sin(c), rho * std::cos(d0) * std::cos(c) - y * std::sin(d0) * std::sin(c));
        o["centre_ra_deg"] = std::fmod(ra * 180 / M_PI + 360, 360); o["centre_dec_deg"] = dec * 180 / M_PI;
    }

    // Photometry in the stacked mean: aperture 3 px, sky the median of the 6..9 px annulus, on the well-covered area. Stars kept: G 9..13.5, BP-RP -0.2..3,
    // no Gaia star brighter than G + 2.5 within 8 px, not near saturation, every aperture pixel stacked in every channel.
    const int n = m_w * m_h;
    std::vector<float> mean[3];
    double mx = 0; for (double v : m_sw[1]) mx = std::max(mx, v);
    for (int c = 0; c < 3; ++c) { mean[c].resize(n); for (int i = 0; i < n; ++i) mean[c][i] = m_sw[c][i] > 0 ? float(m_sx[c][i] / m_sw[c][i]) : 0.f; }
    float top = 0;
    { std::vector<float> l; for (int i = 0; i < n; i += 3) { const float v = (mean[0][i] + mean[1][i] + mean[2][i]) / 3; if (v > 0) l.push_back(v); } top = percentileOf(l, 99.99); }
    std::vector<double> px(cat.size()), py(cat.size());
    for (size_t i = 0; i < cat.size(); ++i) T.apply(0.5 * m_w + parity * xi[i] / s, 0.5 * m_h + eta[i] / s, px[i], py[i]);
    const StarColourModel model(m_refFilter);
    const std::array<double, 3> white = model.white();
    std::vector<double> rr, rb, col;
    int candidates = 0;
    // $ORIGIN_COLOUR_DUMP: each measured star (position, Gaia photometry, raw camera fluxes) as CSV, for checking the colour model offline
    QFile dump(qEnvironmentVariable("ORIGIN_COLOUR_DUMP"));
    if (!dump.fileName().isEmpty() && dump.open(QIODevice::WriteOnly | QIODevice::Text)) dump.write("ra,dec,g,bp_rp,raw_r,raw_g,raw_b\n");
    for (size_t i = 0; i < cat.size(); ++i) {
        const GaiaStar& g = cat[i];
        if (g.g < 9 || g.g > 13.5 || !(g.bpRp > -0.2 && g.bpRp < 3.0)) continue;
        const int xi0 = int(std::lround(px[i])), yi0 = int(std::lround(py[i]));
        if (xi0 < 12 || yi0 < 12 || xi0 >= m_w - 12 || yi0 >= m_h - 12 || m_sw[1][size_t(yi0) * m_w + xi0] < 0.6 * mx) continue;
        ++candidates;
        bool crowded = false;
        for (size_t j = 0; j < cat.size() && !crowded; ++j) {
            if (j == i || cat[j].g >= g.g + 2.5) continue;
            crowded = std::fabs(px[j] - px[i]) < 8 && std::fabs(py[j] - py[i]) < 8 && std::hypot(px[j] - px[i], py[j] - py[i]) < 8;
        }
        if (crowded) continue;
        double sum[3] = {0, 0, 0}; std::vector<float> ann[3]; int na = 0; bool bad = false;
        for (int dy = -9; dy <= 9 && !bad; ++dy) for (int dx = -9; dx <= 9; ++dx) {
            const double r = std::hypot(dx, dy); const size_t k = size_t(yi0 + dy) * m_w + xi0 + dx;
            if (r <= 3.0) {
                ++na;
                if ((mean[0][k] + mean[1][k] + mean[2][k]) / 3 > 0.85f * top) { bad = true; break; }
                for (int c = 0; c < 3; ++c) { if (m_sw[c][k] <= 0) bad = true; sum[c] += mean[c][k]; }
            } else if (r >= 6 && r <= 9) for (int c = 0; c < 3; ++c) ann[c].push_back(mean[c][k]);
        }
        if (bad) continue;
        double m[3];
        for (int c = 0; c < 3; ++c) m[c] = sum[c] - percentileOf(ann[c], 50) * na;
        if (m[0] <= 0 || m[1] <= 0 || m[2] <= 0) continue;
        const std::array<double, 3> p = model.predict(g.bpRp);
        if (dump.isOpen()) dump.write(QString::asprintf("%.7f,%.7f,%.3f,%.4f,%.6g,%.6g,%.6g\n", g.raDeg, g.decDeg, g.g, g.bpRp, m[0] / m_wbR, m[1], m[2] / m_wbB).toUtf8());
        rr.push_back((m[1] / p[1]) / (m[0] / p[0]));
        rb.push_back((m[1] / p[1]) / (m[2] / p[2]));
        col.push_back(g.bpRp);
    }
    o["candidates"] = candidates; o["stars"] = int(rr.size());
    if (rr.size() < 15) return fail(QString("only %1 isolated Gaia stars measured").arg(rr.size()));
    auto mad = [](std::vector<double> v, double med) { for (double& x : v) x = std::fabs(x - med); return 1.4826 * medianOf(v); };
    const double gR = medianOf(rr) * white[1] / white[0], gB = medianOf(rb) * white[1] / white[2];
    const double sR = mad(rr, medianOf(rr)) * white[1] / white[0] / std::sqrt(double(rr.size())), sB = mad(rb, medianOf(rb)) * white[1] / white[2] / std::sqrt(double(rb.size()));
    o["gain_r"] = gR; o["gain_b"] = gB; o["gain_r_err"] = sR; o["gain_b_err"] = sB;
    o["camera_gain_r"] = gR * m_wbR; o["camera_gain_b"] = gB * m_wbB;   // relative to the raw camera channels, comparable between stacks
    o["filter_model"] = model.name();
    // How the stars' gains drift with colour: the slope of log10(gain) against BP-RP, fitted to the stars within 3 MADs of the median. A model with the
    // right filter and sensor curves leaves no slope; a missing or extra band tilts it (red stars too bright in one channel, blue ones in another)
    auto trend = [&](const std::vector<double>& v) {
        const double med = medianOf(v), sd = mad(v, med);
        double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (size_t i = 0; i < v.size(); ++i) {
            if (std::fabs(v[i] - med) > 3 * sd) continue;
            const double x = col[i], y = std::log10(v[i]); n += 1; sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        const double den = n * sxx - sx * sx;
        return n >= 10 && den > 0 ? (n * sxy - sx * sy) / den : 0.0;
    };
    o["trend_r_per_bprp"] = trend(rr); o["trend_b_per_bprp"] = trend(rb);
    if (!(gR > 0.25 && gR < 4 && gB > 0.25 && gB < 4)) return fail(QString("implausible gains R %1 B %2").arg(gR, 0, 'f', 2).arg(gB, 0, 'f', 2));
    // stars that disagree this much point at a damaged channel rather than at colour (the Rosette stack's blue plane, 2026-11-20: 10x the green noise)
    if (sR > 0.05 * gR || sB > 0.05 * gB) return fail(QString("the stars disagree: gains R %1 +- %2, B %3 +- %4").arg(gR, 0, 'f', 2).arg(sR, 0, 'f', 2).arg(gB, 0, 'f', 2).arg(sB, 0, 'f', 2));
    o["ok"] = true; o["epoch"] = epoch; o["catalogue"] = "Gaia DR3 (ESA/Gaia/DPAC)";
    m_ccR = gR; m_ccB = gB; m_ccStars = int(rr.size()); m_ccFrames = m_frames; m_ccReport = o;
    return o;
}

QJsonObject LiveStacker::summary() const
{
    QJsonObject o{{"frames", m_frames}, {"total_exposure_s", m_totalExposure}, {"width", m_w}, {"height", m_h}, {"best_raw_transparency", m_bestRaw}, {"reference_iso", m_refIso}, {"reference_filter", m_refFilter}};
    if (m_ccFrames > 0) o["colour_calibration"] = m_ccReport;
    o["bayer_layout_fixed"] = m_fixedLayout;
    if (m_firstCapture.isValid()) { o["first_capture"] = m_firstCapture.toString(Qt::ISODate); o["last_capture"] = m_lastCapture.toString(Qt::ISODate); }
    return o;
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
    put(b, quint32(0x4C535433));                                     // "LST3": LST1 plus the reference ISO and filter
    put(b, qint32(m_w)); put(b, qint32(m_h)); put(b, qint32(m_frames)); put(b, qint32(m_refIso));
    const QByteArray flt = m_refFilter.toUtf8().left(64); put(b, qint32(flt.size())); b.append(flt);
    put(b, m_totalExposure); put(b, m_refExposure); put(b, m_bestRaw); put(b, m_wbR); put(b, m_wbB);
    put(b, qint32(m_refStars.size())); for (const SkyStar& s : m_refStars) { put(b, s.x); put(b, s.y); put(b, s.flux); put(b, s.peak); put(b, s.hfr); put(b, s.sigma); put(b, qint8(s.isolated)); }
    for (int c = 0; c < 3; ++c) for (size_t i = 0; i < m_sw[c].size(); ++i) { put(b, float(m_sx[c][i])); put(b, float(m_sx2[c][i])); put(b, float(m_sw[c][i])); }
    QFile f(dir + "/state.bin.tmp");
    if (!f.open(QIODevice::WriteOnly) || f.write(b) != b.size()) return false;
    f.close(); QFile::remove(dir + "/state.bin");
    if (!QFile::rename(dir + "/state.bin.tmp", dir + "/state.bin")) return false;
    saveColour(dir);
    preview().savePng(dir + "/preview.png");
    return true;
}

bool LiveStacker::load(const QString& dir)
{
    QFile f(dir + "/state.bin");
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray b = f.readAll(); qsizetype pos = 0;
    quint32 magic = 0; qint32 w = 0, h = 0, n = 0, ns = 0;
    LiveStacker t;
    if (!get(b, pos, magic) || (magic != 0x4C535431 && magic != 0x4C535432 && magic != 0x4C535433) || !get(b, pos, w) || !get(b, pos, h) || !get(b, pos, n)) return false;
    // LST1 (before 2026-10-28) had no ISO: those stacks were seeded at ISO 800, the run's ISO (frames from the ISO-200 fallback all read cloudy and were never stacked)
    qint32 refIso = 800;
    if (magic != 0x4C535431 && !get(b, pos, refIso)) return false;
    t.m_refIso = std::max(0, int(refIso));
    if (magic == 0x4C535433) {                                       // LST3 adds the reference filter; older stacks: unknown (tools/stack_filter.py migrates them)
        qint32 len = 0; if (!get(b, pos, len) || len < 0 || len > 64 || pos + len > b.size()) return false;
        t.m_refFilter = QString::fromUtf8(b.constData() + pos, len); pos += len;
    }
    if (w <= 0 || h <= 0 || w > 8000 || h > 8000) return false;
    if (!get(b, pos, t.m_totalExposure) || !get(b, pos, t.m_refExposure) || !get(b, pos, t.m_bestRaw) || !get(b, pos, t.m_wbR) || !get(b, pos, t.m_wbB) || !get(b, pos, ns) || ns < 0 || ns > 10000) return false;
    for (qint32 i = 0; i < ns; ++i) { SkyStar s; qint8 iso = 0; if (!get(b, pos, s.x) || !get(b, pos, s.y) || !get(b, pos, s.flux) || !get(b, pos, s.peak) || !get(b, pos, s.hfr) || !get(b, pos, s.sigma) || !get(b, pos, iso)) return false; s.isolated = iso; t.m_refStars.push_back(s); }
    t.m_w = w; t.m_h = h; t.m_frames = n;
    for (int c = 0; c < 3; ++c) {
        t.m_sx[c].resize(size_t(w) * h); t.m_sx2[c].resize(size_t(w) * h); t.m_sw[c].resize(size_t(w) * h);
        for (size_t i = 0; i < t.m_sw[c].size(); ++i) { float a, bb, cc; if (!get(b, pos, a) || !get(b, pos, bb) || !get(b, pos, cc)) return false; t.m_sx[c][i] = a; t.m_sx2[c][i] = bb; t.m_sw[c][i] = cc; }
    }
    *this = std::move(t);
    loadColour(dir);
    return true;
}

// colour.json beside state.bin (which several tools parse, so its layout stays as it is): the reference pointing and the Gaia colour calibration
bool LiveStacker::saveColour(const QString& dir) const
{
    QJsonObject o;
    if (hasPointing()) o["pointing"] = QJsonObject{{"ra_deg", m_pointRa}, {"dec_deg", m_pointDec}};
    if (m_ccFrames > 0) o["calibration"] = m_ccReport;
    if (m_fixedLayout) o["bayer_layout"] = "fixed";
    if (m_firstCapture.isValid()) o["capture"] = QJsonObject{{"first", m_firstCapture.toString(Qt::ISODate)}, {"last", m_lastCapture.toString(Qt::ISODate)}};
    if (o.isEmpty()) { QFile::remove(dir + "/colour.json"); return true; }
    QFile f(dir + "/colour.json");
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(QJsonDocument(o).toJson()) > 0;
}

void LiveStacker::loadColour(const QString& dir)
{
    QFile f(dir + "/colour.json");
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    m_fixedLayout = o["bayer_layout"].toString() == "fixed";
    const QJsonObject cap = o["capture"].toObject();
    m_firstCapture = QDateTime::fromString(cap["first"].toString(), Qt::ISODate); m_lastCapture = QDateTime::fromString(cap["last"].toString(), Qt::ISODate);
    const QJsonObject p = o["pointing"].toObject();
    if (p.contains("dec_deg")) { m_pointRa = p["ra_deg"].toDouble(); m_pointDec = p["dec_deg"].toDouble(); }
    const QJsonObject c = o["calibration"].toObject();
    if (c["ok"].toBool() && c["gain_r"].toDouble() > 0 && c["gain_b"].toDouble() > 0) {
        m_ccR = c["gain_r"].toDouble(); m_ccB = c["gain_b"].toDouble(); m_ccStars = c["stars"].toInt(); m_ccFrames = std::max(1, c["frames"].toInt()); m_ccReport = c;
    }
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
