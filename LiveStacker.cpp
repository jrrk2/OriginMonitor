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

// Remove a quadratic sky surface fitted to the unclipped pixels (stars and galaxies are iteratively excluded; the surface is too smooth
// to absorb faint extended light such as a tidal bridge). Returns the residual noise.
double flattenPlane(std::vector<float>& img, int w, int h)
{
    struct Pt { float u, v, val; };
    std::vector<Pt> pts;
    for (int y = 3; y < h; y += 6) for (int x = 3; x < w; x += 6) pts.push_back({2.f * x / w - 1.f, 2.f * y / h - 1.f, img[size_t(y) * w + x]});
    std::vector<char> use(pts.size(), 1);
    double c[6] = {0, 0, 0, 0, 0, 0}, sigma = 1;
    auto basis = [](double u, double v, double* t) { t[0] = 1; t[1] = u; t[2] = v; t[3] = u * u; t[4] = u * v; t[5] = v * v; };
    for (int it = 0; it < 5; ++it) {
        std::vector<double> A(36, 0.0), b(6, 0.0);
        for (size_t i = 0; i < pts.size(); ++i) if (use[i]) {
            double t[6]; basis(pts[i].u, pts[i].v, t);
            for (int r = 0; r < 6; ++r) { b[r] += t[r] * pts[i].val; for (int k = 0; k < 6; ++k) A[r * 6 + k] += t[r] * t[k]; }
        }
        if (!solve(A, b, 6)) break;
        for (int k = 0; k < 6; ++k) c[k] = b[k];
        std::vector<double> res;
        for (size_t i = 0; i < pts.size(); ++i) { double t[6]; basis(pts[i].u, pts[i].v, t); double m = 0; for (int k = 0; k < 6; ++k) m += c[k] * t[k]; res.push_back(pts[i].val - m); }
        std::vector<double> ar = res; for (auto& x : ar) x = std::fabs(x);
        sigma = 1.4826 * medianOf(ar) + 1e-6;
        for (size_t i = 0; i < pts.size(); ++i) use[i] = res[i] < 2.0 * sigma && res[i] > -3.0 * sigma;
    }
    for (int y = 0; y < h; ++y) {
        const double v = 2.0 * y / h - 1.0;
        for (int x = 0; x < w; ++x) {
            const double u = 2.0 * x / w - 1.0;
            img[size_t(y) * w + x] -= float(c[0] + c[1] * u + c[2] * v + c[3] * u * u + c[4] * u * v + c[5] * v * v);
        }
    }
    return sigma;
}

// Row/column offsets from the high-passed plane (a halo is smooth and cannot leak into the estimate)
void removeBanding(std::vector<float>& img, int w, int h)
{
    const int B = 24;
    auto boxBlur = [&](const std::vector<float>& src) {
        std::vector<double> integ(size_t(w + 1) * (h + 1), 0.0);
        for (int y = 0; y < h; ++y) { double row = 0; for (int x = 0; x < w; ++x) { row += src[size_t(y) * w + x]; integ[size_t(y + 1) * (w + 1) + x + 1] = integ[size_t(y) * (w + 1) + x + 1] + row; } }
        std::vector<float> out(src.size());
        for (int y = 0; y < h; ++y) { const int y0 = std::max(0, y - B), y1 = std::min(h, y + B + 1);
            for (int x = 0; x < w; ++x) { const int x0 = std::max(0, x - B), x1 = std::min(w, x + B + 1);
                const double s = integ[size_t(y1) * (w + 1) + x1] - integ[size_t(y0) * (w + 1) + x1] - integ[size_t(y1) * (w + 1) + x0] + integ[size_t(y0) * (w + 1) + x0];
                out[size_t(y) * w + x] = float(s / (double(y1 - y0) * (x1 - x0))); } }
        return out;
    };
    for (int it = 0; it < 2; ++it) {
        std::vector<float> low = boxBlur(img);
        std::vector<float> row(w);
        for (int y = 0; y < h; ++y) { for (int x = 0; x < w; ++x) row[x] = img[size_t(y) * w + x] - low[size_t(y) * w + x]; std::nth_element(row.begin(), row.begin() + w * 2 / 5, row.end()); const float m = row[w * 2 / 5]; for (int x = 0; x < w; ++x) img[size_t(y) * w + x] -= m; }
        low = boxBlur(img);
        std::vector<float> col(h);
        for (int x = 0; x < w; ++x) { for (int y = 0; y < h; ++y) col[y] = img[size_t(y) * w + x] - low[size_t(y) * w + x]; std::nth_element(col.begin(), col.begin() + h * 2 / 5, col.end()); const float m = col[h * 2 / 5]; for (int y = 0; y < h; ++y) img[size_t(y) * w + x] -= m; }
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
        removeBanding(img, w, h);
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
    std::vector<float> sub; for (int y = 0; y < h; y += 3) for (int x = 0; x < w; x += 3) sub.push_back(lum[size_t(y) * w + x]);
    std::vector<float> s2 = sub; std::nth_element(s2.begin(), s2.begin() + s2.size() / 2, s2.end()); const double sky = s2[s2.size() / 2];
    for (auto& v : s2) v = std::fabs(v - float(sky)); std::nth_element(s2.begin(), s2.begin() + s2.size() / 2, s2.end()); const double sg = 1.4826 * s2[s2.size() / 2] + 1e-6;
    std::nth_element(sub.begin(), sub.begin() + size_t(sub.size() * 0.9985), sub.end());
    const double lo = sky + 1.0 * sg, hi = std::max<double>(sub[size_t(sub.size() * 0.9985)], lo + 10 * sg);
    const double soft = 12.0, den = std::asinh(soft), sat = 1.35;
    QImage img(w, h, QImage::Format_RGB888);
    for (int y = 0; y < h; ++y) {
        uchar* line = img.scanLine(h - 1 - y);                         // FITS rows run bottom-up
        for (int x = 0; x < w; ++x) {
            const size_t o = size_t(y) * w + x;
            const double L = lum[o] - lo;
            double q[3] = {0, 0, 0};
            if (L > 0) { const double t = std::asinh(soft * L / (hi - lo)) / den; const double k = t / L; for (int c = 0; c < 3; ++c) q[c] = (col[c][o] - lo) * k; }
            const double m = (q[0] + q[1] + q[2]) / 3;
            for (int c = 0; c < 3; ++c) line[3 * x + c] = uchar(std::clamp(m + sat * (q[c] - m), 0.0, 1.0) * 255.0 + 0.5);
        }
    }
    return img;
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
