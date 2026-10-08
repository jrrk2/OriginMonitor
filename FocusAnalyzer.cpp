#include "FocusAnalyzer.hpp"
#include "SkyFrame.hpp"

#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
constexpr double kArcsecPerPixel = 1.478;     // nominal full-resolution scale of the Origin
constexpr int kMaxStars = 150;                // brightest accepted stars that enter the median
constexpr int kMinStars = 6;

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

FocusResult focusFromFrame(const SkyFrame& F)
{
    FocusResult r;
    r.width = F.fullWidth; r.height = F.fullHeight;
    if (!F.valid) { r.note = F.note; return r; }
    r.background = F.background; r.noise = F.noise;
    std::vector<float> hfr, peakFrac;
    for (const SkyStar& s : F.stars) {                       // already brightest first
        if (!s.isolated) continue;
        hfr.push_back(s.hfr); peakFrac.push_back(s.peak / s.flux);
        if (int(hfr.size()) >= kMaxStars) break;
    }
    r.stars = int(hfr.size());
    if (r.stars < kMinStars) { r.note = QString("only %1 usable stars").arg(r.stars); return r; }
    const double hm = medianOf(hfr);
    std::vector<float> dev; for (float v : hfr) dev.push_back(std::fabs(v - float(hm)));
    r.hfrPx = 2.0 * hm;                                      // half-res -> full-resolution pixels
    r.hfrArcsec = r.hfrPx * kArcsecPerPixel;
    r.hfrSpreadPx = 2.0 * 1.4826 * medianOf(dev);
    r.peakFraction = medianOf(peakFrac);
    r.valid = true;
    return r;
}

FocusResult evaluateFocus(const QByteArray& fitsData)
{
    QElapsedTimer timer; timer.start();
    FocusResult r = focusFromFrame(analyseFrame(fitsData));
    r.elapsedMs = timer.elapsed();
    return r;
}
