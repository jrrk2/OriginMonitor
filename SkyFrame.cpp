#include "SkyFrame.hpp"

#include <QList>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kRadius = 7;                    // aperture radius in half-res pixels
constexpr double kSaturation = 0.9 * 65535.0;

struct Fits { int w = 0, h = 0; double bzero = 0, bscale = 1; qsizetype dataStart = 0; };

bool parseHeader(const QByteArray& d, Fits& f, QString& why)
{
    int bitpix = 0; bool end = false;
    for (qsizetype i = 0; i + 80 <= d.size() && !end; i += 80) {
        const QByteArray card = d.mid(i, 80);
        const QByteArray key = card.left(8).trimmed();
        const QByteArray val = card.mid(10, 20).split('/').first().trimmed();
        if (key == "NAXIS1") f.w = val.toInt();
        else if (key == "NAXIS2") f.h = val.toInt();
        else if (key == "BITPIX") bitpix = val.toInt();
        else if (key == "BZERO") f.bzero = val.toDouble();
        else if (key == "BSCALE") f.bscale = val.toDouble();
        else if (key == "END") { end = true; f.dataStart = ((i / 80 + 36) / 36) * 2880; }
    }
    if (!end || f.w < 64 || f.h < 64) { why = "not a FITS image"; return false; }
    if (bitpix != 16) { why = "unsupported BITPIX " + QString::number(bitpix); return false; }
    if (d.size() < f.dataStart + qint64(f.w) * f.h * 2) { why = "truncated data"; return false; }
    return true;
}

double medianOf(std::vector<float>& v)
{
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

}   // namespace

SkyFrame analyseFrame(const QByteArray& data, double detectSigma, int maxStars)
{
    SkyFrame F;
    Fits f;
    if (!parseHeader(data, f, F.note)) return F;
    F.fullWidth = f.w; F.fullHeight = f.h;
    const int w = f.w / 2, h = f.h / 2;
    F.width = w; F.height = h;
    const uchar* d = reinterpret_cast<const uchar*>(data.constData()) + f.dataStart;
    auto px = [&](int x, int y) { const qsizetype i = (qsizetype(y) * f.w + x) * 2; return float(double(qint16((d[i] << 8) | d[i + 1])) * f.bscale + f.bzero); };

    // Four sensel planes -> luminance, then work out which diagonal carries the greens (the FITS header's BAYERPAT is not reliable)
    std::vector<float> p[4];
    for (auto& v : p) v.resize(size_t(w) * h);
    F.lum.resize(size_t(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * w + x;
            p[0][i] = px(2*x, 2*y); p[1][i] = px(2*x+1, 2*y); p[2][i] = px(2*x, 2*y+1); p[3][i] = px(2*x+1, 2*y+1);
            F.lum[i] = 0.25f * (p[0][i] + p[1][i] + p[2][i] + p[3][i]);
        }

    // Sky: median and robust sigma of a subsample, then a smooth background from a star-clipped box mean
    std::vector<float> sub; sub.reserve(F.lum.size() / 16 + 1);
    for (int y = 0; y < h; y += 4) for (int x = 0; x < w; x += 4) sub.push_back(F.lum[size_t(y) * w + x]);
    const double med = medianOf(sub);
    for (auto& v : sub) v = std::fabs(v - float(med));
    const double sigma0 = 1.4826 * medianOf(sub) + 1e-3;
    const float clip = float(med + 3 * sigma0);

    const int B = 20;                                          // box half-size: 41 x 41 pixels
    std::vector<double> integ(size_t(w + 1) * (h + 1), 0.0);
    for (int y = 0; y < h; ++y) {
        double row = 0;
        for (int x = 0; x < w; ++x) {
            row += std::min(F.lum[size_t(y) * w + x], clip);
            integ[size_t(y + 1) * (w + 1) + x + 1] = integ[size_t(y) * (w + 1) + x + 1] + row;
        }
    }
    std::vector<float> res(F.lum.size());
    for (int y = 0; y < h; ++y) {
        const int y0 = std::max(0, y - B), y1 = std::min(h, y + B + 1);
        for (int x = 0; x < w; ++x) {
            const int x0 = std::max(0, x - B), x1 = std::min(w, x + B + 1);
            const double s = integ[size_t(y1) * (w + 1) + x1] - integ[size_t(y0) * (w + 1) + x1] - integ[size_t(y1) * (w + 1) + x0] + integ[size_t(y0) * (w + 1) + x0];
            res[size_t(y) * w + x] = F.lum[size_t(y) * w + x] - float(s / (double(y1 - y0) * (x1 - x0)));
        }
    }
    std::vector<float> ar; ar.reserve(sub.size());
    for (int y = 0; y < h; y += 4) for (int x = 0; x < w; x += 4) ar.push_back(std::fabs(res[size_t(y) * w + x]));
    const double noise = 1.4826 * medianOf(ar) + 1e-3;
    F.background = med; F.noise = noise;

    // Candidate peaks: 5x5 local maxima above the detection threshold, away from the border
    struct Cand { int x, y; float peak; };
    std::vector<Cand> cands;
    const int m = kRadius + 2;
    for (int y = m; y < h - m; ++y)
        for (int x = m; x < w - m; ++x) {
            const float pk = res[size_t(y) * w + x];
            if (pk < detectSigma * noise) continue;
            bool top = true;
            for (int dy = -2; dy <= 2 && top; ++dy) for (int dx = -2; dx <= 2; ++dx) if ((dx || dy) && res[size_t(y + dy) * w + x + dx] > pk) { top = false; break; }
            if (top) cands.push_back({x, y, pk});
        }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.peak > b.peak; });

    // Green diagonal from the brightest candidates: the two green sensels carry more star light than red or blue
    {
        double main = 0, anti = 0;
        for (size_t i = 0; i < cands.size() && i < 200; ++i) {
            const size_t k = size_t(cands[i].y) * w + cands[i].x;
            main += p[0][k] + p[3][k] - 2 * med; anti += p[1][k] + p[2][k] - 2 * med;
        }
        F.greenOnMainDiagonal = main >= anti;                  // GRBG-like (red at (0,1)) when true, RGGB-like (red at (0,0)) otherwise
    }
    const std::vector<float>& R = F.greenOnMainDiagonal ? p[1] : p[0];
    const std::vector<float>& Bl = F.greenOnMainDiagonal ? p[2] : p[3];
    F.r = R; F.b = Bl; F.g.resize(F.lum.size());
    const std::vector<float>& G1 = F.greenOnMainDiagonal ? p[0] : p[1];
    const std::vector<float>& G2 = F.greenOnMainDiagonal ? p[3] : p[2];
    for (size_t i = 0; i < F.g.size(); ++i) F.g[i] = 0.5f * (G1[i] + G2[i]);

    for (size_t ci = 0; ci < cands.size() && int(F.stars.size()) < maxStars; ++ci) {
        const Cand& c = cands[ci];
        if (F.lum[size_t(c.y) * w + c.x] > kSaturation) continue;
        bool crowded = false;
        for (const Cand& o : cands) {
            if (&o == &c || o.peak < 0.3f * c.peak) continue;
            if (std::hypot(o.x - c.x, o.y - c.y) < 2 * kRadius) { crowded = true; break; }
        }
        double cx = c.x, cy = c.y, flux = 0, rsum = 0;
        for (int pass = 0; pass < 3; ++pass) {                 // recentre on the flux-weighted centroid, then measure
            double sx = 0, sy = 0, sw = 0; rsum = 0; flux = 0;
            for (int dy = -kRadius; dy <= kRadius; ++dy)
                for (int dx = -kRadius; dx <= kRadius; ++dx) {
                    const int X = int(std::lround(cx)) + dx, Y = int(std::lround(cy)) + dy;
                    if (X < 0 || Y < 0 || X >= w || Y >= h) continue;
                    const double rr = std::hypot(X - cx, Y - cy);
                    if (rr > kRadius) continue;
                    const double v = std::max(0.0, double(res[size_t(Y) * w + X]) - noise);   // noise floor keeps the wings from biasing the radius
                    sx += v * X; sy += v * Y; sw += v; rsum += v * rr; flux += v;
                }
            if (sw <= 0) break;
            cx = sx / sw; cy = sy / sw;
        }
        double mxx = 0, myy = 0, mxy = 0, mw = 0;                // second moments about the final centroid
        for (int dy = -kRadius; dy <= kRadius; ++dy)
            for (int dx = -kRadius; dx <= kRadius; ++dx) {
                const int X = int(std::lround(cx)) + dx, Y = int(std::lround(cy)) + dy;
                if (X < 0 || Y < 0 || X >= w || Y >= h || std::hypot(X - cx, Y - cy) > kRadius) continue;
                const double v = std::max(0.0, double(res[size_t(Y) * w + X]) - noise);
                mxx += v * (X - cx) * (X - cx); myy += v * (Y - cy) * (Y - cy); mxy += v * (X - cx) * (Y - cy); mw += v;
            }
        if (flux <= 0) continue;
        const double radius = rsum / flux;
        double nb = 0; int n = 0;                              // a real star lifts its neighbours, a hot sensel does not
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) if (dx || dy) { nb += res[size_t(c.y + dy) * w + c.x + dx]; ++n; }
        if (nb / n < 0.1 * c.peak || radius < 0.45) continue;
        SkyStar s; s.x = float(cx); s.y = float(cy); s.flux = float(flux); s.peak = c.peak; s.hfr = float(radius); s.sigma = float(c.peak / noise);
        if (mw > 0) {
            mxx /= mw; myy /= mw; mxy /= mw;
            const double tr = mxx + myy, det = mxx * myy - mxy * mxy, disc = std::sqrt(std::max(0.0, tr * tr / 4 - det)), l1 = tr / 2 + disc, l2 = std::max(1e-6, tr / 2 - disc);
            s.elong = float(std::sqrt(std::max(1.0, l1 / l2))); s.angle = float(0.5 * std::atan2(2 * mxy, mxx - myy));
        }
        s.isolated = !crowded && c.peak >= 8 * noise;
        F.stars.push_back(s);
    }
    F.valid = true;
    return F;
}

StreakInfo streakInfo(const SkyFrame& f)
{
    StreakInfo r; double sc = 0, ss = 0; int n = 0;
    for (size_t i = 0; i < f.stars.size() && i < 100; ++i) if (f.stars[i].elong >= 2.0f) { sc += std::cos(2.0 * f.stars[i].angle); ss += std::sin(2.0 * f.stars[i].angle); ++n; }
    r.elongated = n;
    if (n > 0) { r.coherence = std::hypot(sc, ss) / n; r.angleDeg = 0.5 * std::atan2(ss, sc) * 180.0 / 3.14159265358979323846; }
    r.trailed = n >= 25 && r.coherence >= 0.8;
    return r;
}
