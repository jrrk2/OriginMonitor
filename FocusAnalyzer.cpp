#include "FocusAnalyzer.hpp"

#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr double kArcsecPerPixel = 1.478;     // nominal full-resolution scale of the Origin
constexpr int kMaxStars = 150;                // brightest accepted stars that enter the median
constexpr int kMinStars = 6;
constexpr int kRadius = 7;                    // aperture radius in binned pixels
constexpr double kDetectSigma = 8.0;
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

QJsonObject FocusResult::toJson() const
{
    QJsonObject o{{"valid", valid}, {"stars", stars}, {"width", width}, {"height", height}, {"elapsed_ms", double(elapsedMs)}};
    if (!valid) { o["note"] = note; return o; }
    o["hfr_px"] = hfrPx; o["hfr_arcsec"] = hfrArcsec; o["hfr_spread_px"] = hfrSpreadPx;
    o["peak_fraction"] = peakFraction; o["background"] = background; o["noise"] = noise;
    return o;
}

FocusResult evaluateFocus(const QByteArray& data)
{
    QElapsedTimer timer; timer.start();
    FocusResult r;
    Fits f;
    if (!parseHeader(data, f, r.note)) { r.elapsedMs = timer.elapsed(); return r; }
    r.width = f.w; r.height = f.h;

    // Bin 2x2 (big-endian signed 16 bit + BZERO), mean of the four sensels
    const int w = f.w / 2, h = f.h / 2;
    const uchar* d = reinterpret_cast<const uchar*>(data.constData()) + f.dataStart;
    auto px = [&](int x, int y) { const qsizetype i = (qsizetype(y) * f.w + x) * 2; return double(qint16((d[i] << 8) | d[i + 1])) * f.bscale + f.bzero; };
    std::vector<float> img(size_t(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            img[size_t(y) * w + x] = float((px(2*x, 2*y) + px(2*x+1, 2*y) + px(2*x, 2*y+1) + px(2*x+1, 2*y+1)) * 0.25);

    // Sky: median and robust sigma from a subsample, then a smooth background from a star-clipped box mean
    std::vector<float> sub; sub.reserve(img.size() / 16 + 1);
    for (int y = 0; y < h; y += 4) for (int x = 0; x < w; x += 4) sub.push_back(img[size_t(y) * w + x]);
    const double med = medianOf(sub);
    for (auto& v : sub) v = std::fabs(v - float(med));
    const double sigma0 = 1.4826 * medianOf(sub) + 1e-3;
    const float clip = float(med + 3 * sigma0);

    const int B = 20;                                           // box half-size: 41 x 41 binned pixels
    std::vector<double> integ(size_t(w + 1) * (h + 1), 0.0);   // integral image of the clipped frame
    for (int y = 0; y < h; ++y) {
        double row = 0;
        for (int x = 0; x < w; ++x) {
            row += std::min(img[size_t(y) * w + x], clip);
            integ[size_t(y + 1) * (w + 1) + x + 1] = integ[size_t(y) * (w + 1) + x + 1] + row;
        }
    }
    std::vector<float> res(img.size());
    for (int y = 0; y < h; ++y) {
        const int y0 = std::max(0, y - B), y1 = std::min(h, y + B + 1);
        for (int x = 0; x < w; ++x) {
            const int x0 = std::max(0, x - B), x1 = std::min(w, x + B + 1);
            const double s = integ[size_t(y1) * (w + 1) + x1] - integ[size_t(y0) * (w + 1) + x1] - integ[size_t(y1) * (w + 1) + x0] + integ[size_t(y0) * (w + 1) + x0];
            res[size_t(y) * w + x] = img[size_t(y) * w + x] - float(s / (double(y1 - y0) * (x1 - x0)));
        }
    }
    for (int y = 0; y < h; y += 4) for (int x = 0; x < w; x += 4) sub[size_t(y / 4) * ((w + 3) / 4) + x / 4] = std::fabs(res[size_t(y) * w + x]);
    const double noise = 1.4826 * medianOf(sub) + 1e-3;
    r.background = med; r.noise = noise;

    // Candidate peaks: 5x5 local maxima well above the noise, away from the border
    struct Cand { int x, y; float peak; };
    std::vector<Cand> cands;
    const int m = kRadius + 2;
    for (int y = m; y < h - m; ++y)
        for (int x = m; x < w - m; ++x) {
            const float p = res[size_t(y) * w + x];
            if (p < kDetectSigma * noise) continue;
            bool top = true;
            for (int dy = -2; dy <= 2 && top; ++dy) for (int dx = -2; dx <= 2; ++dx) if ((dx || dy) && res[size_t(y + dy) * w + x + dx] > p) { top = false; break; }
            if (top) cands.push_back({x, y, p});
        }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.peak > b.peak; });

    std::vector<float> hfr, peakFrac;
    for (size_t ci = 0; ci < cands.size() && int(hfr.size()) < kMaxStars; ++ci) {
        const Cand& c = cands[ci];
        if (img[size_t(c.y) * w + c.x] > kSaturation) continue;
        bool crowded = false;                                   // a comparable star inside two apertures spoils the profile
        for (const Cand& o : cands) {
            if (&o == &c || o.peak < 0.3f * c.peak) continue;
            if (std::hypot(o.x - c.x, o.y - c.y) < 2 * kRadius) { crowded = true; break; }
        }
        if (crowded) continue;
        double cx = c.x, cy = c.y, flux = 0, rsum = 0;
        for (int pass = 0; pass < 3; ++pass) {                  // recentre on the flux-weighted centroid, then measure
            double sx = 0, sy = 0, sw = 0; rsum = 0; flux = 0;
            for (int dy = -kRadius; dy <= kRadius; ++dy)
                for (int dx = -kRadius; dx <= kRadius; ++dx) {
                    const int X = int(std::lround(cx)) + dx, Y = int(std::lround(cy)) + dy;
                    if (X < 0 || Y < 0 || X >= w || Y >= h) continue;
                    const double rr = std::hypot(X - cx, Y - cy);
                    if (rr > kRadius) continue;
                    const double v = std::max(0.0, double(res[size_t(Y) * w + X]) - 1.0 * noise);   // noise floor keeps the wings from biasing the radius
                    sx += v * X; sy += v * Y; sw += v; rsum += v * rr; flux += v;
                }
            if (sw <= 0) break;
            cx = sx / sw; cy = sy / sw;
        }
        if (flux <= 0) continue;
        const double radius = rsum / flux;
        double nb = 0; int n = 0;                               // wings: a real star lifts its neighbours, a hot sensel does not
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) if (dx || dy) { nb += res[size_t(c.y + dy) * w + c.x + dx]; ++n; }
        if (nb / n < 0.1 * c.peak || radius < 0.45) continue;
        hfr.push_back(float(radius));
        peakFrac.push_back(float(c.peak / flux));
    }

    r.stars = int(hfr.size());
    if (r.stars < kMinStars) { r.note = QString("only %1 usable stars").arg(r.stars); r.elapsedMs = timer.elapsed(); return r; }
    const double hm = medianOf(hfr);
    std::vector<float> dev; for (float v : hfr) dev.push_back(std::fabs(v - float(hm)));
    r.hfrPx = 2.0 * hm;                                          // binned -> full-resolution pixels
    r.hfrArcsec = r.hfrPx * kArcsecPerPixel;
    r.hfrSpreadPx = 2.0 * 1.4826 * medianOf(dev);
    r.peakFraction = medianOf(peakFrac);
    r.valid = true; r.elapsedMs = timer.elapsed();
    return r;
}
