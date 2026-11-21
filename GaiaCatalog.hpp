// GaiaCatalog.hpp - cone search in the local Gaia DR3 file written by tools/gaia_pack.py, and the star colour model the live stacker's
// colour calibration predicts each star's camera signal with. Gaia data: ESA/Gaia/DPAC (https://www.cosmos.esa.int/gaia).
#ifndef GAIACATALOG_H
#define GAIACATALOG_H

#include "DataDir.hpp"
#include "StarSpectra.hpp"

#include <QFile>
#include <QFileInfo>
#include <QString>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <vector>

struct GaiaStar {
    double raDeg = 0, decDeg = 0;   // at the epoch asked for (proper motion applied)
    float g = 0;                    // G magnitude
    float bpRp = NAN;               // BP-RP colour, NaN when Gaia has none
};

/// $ORIGIN_GAIA_CATALOG, else gaia_dr3_g14.bin in the data directory, else the backup disk's copy. Empty when none exists.
inline QString gaiaCatalogPath()
{
    const QString env = qEnvironmentVariable("ORIGIN_GAIA_CATALOG");
    for (const QString& p : {env, originDataDir() + "/gaia_dr3_g14.bin", QString("/mnt/fits-backup/gaia/gaia_dr3_g14.bin")})
        if (!p.isEmpty() && QFileInfo(p).isFile()) return p;
    return QString();
}

/// Stars brighter than gmax within radiusDeg of (raDeg, decDeg), positions moved by proper motion to epochYear. False when the file is unreadable.
inline bool gaiaConeSearch(const QString& path, double raDeg, double decDeg, double radiusDeg, double gmax, double epochYear, std::vector<GaiaStar>& out)
{
    out.clear();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    char hdr[64];
    if (f.read(hdr, 64) != 64 || memcmp(hdr, "GAIACAT1", 8) != 0) return false;
    quint32 zones, bins; float epoch;
    memcpy(&zones, hdr + 8, 4); memcpy(&bins, hdr + 12, 4); memcpy(&epoch, hdr + 16, 4);
    if (zones != 180 || bins != 360) return false;
    const qint64 indexPos = 64, starsPos = indexPos + 8 * (qint64(zones) * bins + 1);
    constexpr double d2r = M_PI / 180.0;
    const double dt = epochYear - epoch, cd0 = std::cos(decDeg * d2r), sd0 = std::sin(decDeg * d2r), cosR = std::cos(radiusDeg * d2r);
    struct Rec { quint32 ra; qint32 dec; qint16 pmra, pmdec, g, bprp; };
    static_assert(sizeof(Rec) == 16, "packed record");
    const int z0 = std::clamp(int(std::floor(decDeg - radiusDeg + 90)), 0, int(zones) - 1), z1 = std::clamp(int(std::floor(decDeg + radiusDeg + 90)), 0, int(zones) - 1);
    std::vector<Rec> buf;
    for (int z = z0; z <= z1; ++z) {
        // RA half-width of the cone at the zone's edge nearest the pole; the whole zone near a pole
        const double edge = std::max(std::fabs(z - 90.0), std::fabs(z + 1 - 90.0)), cosEdge = std::cos(std::min(89.9, edge) * d2r);
        const double half = (edge >= 89.0 || radiusDeg / cosEdge >= 179.0) ? 180.0 : radiusDeg / cosEdge + 0.01;
        const int b0 = half >= 180 ? 0 : int(std::floor(raDeg - half)), b1 = half >= 180 ? int(bins) - 1 : int(std::floor(raDeg + half));
        // the bins b0..b1, wrapped into 0..359, as at most two contiguous runs
        std::vector<std::pair<int, int>> runs;
        if (b1 - b0 + 1 >= int(bins)) runs.push_back({0, int(bins) - 1});
        else {
            const int a = ((b0 % int(bins)) + int(bins)) % int(bins), b = ((b1 % int(bins)) + int(bins)) % int(bins);
            if (a <= b) runs.push_back({a, b}); else { runs.push_back({a, int(bins) - 1}); runs.push_back({0, b}); }
        }
        for (auto [ba, bb] : runs) {
            quint64 lo = 0, hi = 0;
            if (!f.seek(indexPos + 8 * (qint64(z) * bins + ba)) || f.read(reinterpret_cast<char*>(&lo), 8) != 8) return false;
            if (!f.seek(indexPos + 8 * (qint64(z) * bins + bb + 1)) || f.read(reinterpret_cast<char*>(&hi), 8) != 8) return false;
            if (hi <= lo) continue;
            buf.resize(size_t(hi - lo));
            if (!f.seek(starsPos + qint64(lo) * 16) || f.read(reinterpret_cast<char*>(buf.data()), qint64(buf.size()) * 16) != qint64(buf.size()) * 16) return false;
            for (const Rec& r : buf) {
                if (r.g * 1e-3 >= gmax) continue;
                const double dec = r.dec * 1e-7, cd = std::cos(dec * d2r);
                const double ra = r.ra * 1e-7 + (cd > 1e-6 ? r.pmra * dt / 3.6e6 / cd : 0.0), de = dec + r.pmdec * dt / 3.6e6;
                const double c = sd0 * std::sin(de * d2r) + cd0 * std::cos(de * d2r) * std::cos((ra - raDeg) * d2r);
                if (c < cosR) continue;
                GaiaStar s; s.raDeg = std::fmod(ra + 360.0, 360.0); s.decDeg = de; s.g = float(r.g * 1e-3); s.bpRp = r.bprp == -32768 ? NAN : float(r.bprp * 1e-3);
                out.push_back(s);
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const GaiaStar& a, const GaiaStar& b) { return a.g < b.g; });
    return true;
}

/// Gnomonic projection about (ra0, dec0): standard coordinates xi (east), eta (north) in degrees. False behind the tangent point.
inline bool gnomonic(double raDeg, double decDeg, double ra0Deg, double dec0Deg, double& xi, double& eta)
{
    constexpr double d2r = M_PI / 180.0;
    const double dra = (raDeg - ra0Deg) * d2r, d = decDeg * d2r, d0 = dec0Deg * d2r;
    const double den = std::sin(d0) * std::sin(d) + std::cos(d0) * std::cos(d) * std::cos(dra);
    if (den <= 0) return false;
    xi = std::cos(d) * std::sin(dra) / den / d2r;
    eta = (std::cos(d0) * std::sin(d) - std::sin(d0) * std::cos(d) * std::cos(dra)) / den / d2r;
    return true;
}

// Antlia Triband RGB Ultra II transmission, 380..720 nm in 2 nm steps: Antlia's 2025 spectrum curve for the filter (antliafilter.com), digitised;
// off-band below 0.5% set to 0 (the filter blocks to OD4 from 300 to 1050 nm)
static constexpr float kAntliaTriband2[171] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0.068, 0.841, 0.947, 0.956, 0.971, 0.979, 0.967, 0.959, 0.953, 0.949, 0.949, 0.95, 0.948, 0.901, 0.341, 0.021, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0.024, 0.294, 0.952, 0.973, 0.983, 0.985, 0.984, 0.99, 0.986, 0.975, 0.978,
    0.985, 0.965, 0.959, 0.939, 0.621, 0.158, 0.022, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.013, 0.085, 0.41, 0.851, 0.919, 0.938, 0.962, 0.975,
    0.98, 0.985, 0.978, 0.968, 0.954, 0.956, 0.962, 0.971, 0.979, 0.939, 0.526, 0.135, 0.032, 0.009, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
// Sony IMX178 colour response, relative (R, G, B), 400..700 nm in 5 nm steps:
// Sony's curves as published in The Imaging Source's Spectral Sensitivity whitepaper 1.30, p. 25 (read from the PDF's vector paths)
static constexpr float kImx178R[61] = {
    0.093, 0.087, 0.071, 0.06, 0.048, 0.039, 0.031, 0.027, 0.023, 0.019, 0.017, 0.015, 0.015, 0.016, 0.018, 0.02, 0.024, 0.025, 0.027, 0.028,
    0.031, 0.038, 0.046, 0.057, 0.066, 0.07, 0.07, 0.064, 0.056, 0.046, 0.042, 0.044, 0.053, 0.096, 0.205, 0.355, 0.532, 0.707, 0.84, 0.913,
    0.935, 0.936, 0.924, 0.91, 0.895, 0.879, 0.865, 0.851, 0.835, 0.813, 0.794, 0.77, 0.742, 0.716, 0.693, 0.672, 0.651, 0.636, 0.631, 0.633,
    0.635};
static constexpr float kImx178G[61] = {
    0.068, 0.064, 0.055, 0.048, 0.041, 0.035, 0.034, 0.038, 0.039, 0.041, 0.047, 0.06, 0.091, 0.158, 0.259, 0.392, 0.532, 0.679, 0.776, 0.855,
    0.909, 0.944, 0.966, 0.983, 0.992, 0.995, 0.994, 0.993, 0.986, 0.971, 0.952, 0.93, 0.902, 0.868, 0.835, 0.795, 0.75, 0.698, 0.63, 0.556,
    0.481, 0.409, 0.341, 0.288, 0.245, 0.216, 0.194, 0.18, 0.167, 0.156, 0.15, 0.146, 0.148, 0.153, 0.167, 0.185, 0.203, 0.222, 0.245, 0.269,
    0.281};
static constexpr float kImx178B[61] = {
    0.458, 0.476, 0.52, 0.559, 0.594, 0.626, 0.658, 0.692, 0.726, 0.756, 0.778, 0.794, 0.801, 0.796, 0.783, 0.759, 0.723, 0.683, 0.625, 0.56,
    0.495, 0.432, 0.364, 0.305, 0.247, 0.201, 0.169, 0.141, 0.121, 0.101, 0.083, 0.068, 0.056, 0.045, 0.039, 0.035, 0.035, 0.033, 0.03, 0.027,
    0.025, 0.024, 0.023, 0.022, 0.02, 0.021, 0.024, 0.029, 0.033, 0.038, 0.042, 0.049, 0.054, 0.058, 0.063, 0.066, 0.069, 0.071, 0.073, 0.075,
    0.075};
/**
 * Predicted R, G, B camera signal of a star from its Gaia BP-RP: the Pickles library spectrum of that colour (StarSpectra.hpp, interpolated between the two
 * templates either side of it) in photons per nm, through the filter and the IMX178's colour response, 380..720 nm.
 *  - "Clear" or no filter: the Origin's window, taken as flat to 700 nm.
 *  - any other filter ("Nebula"): the Antlia Triband RGB Ultra II's measured curve, whose bands are 422-447, 481-508 and 649-680 nm (half maximum).
 *  - the sensor: Sony's R, G, B curves 400..700 nm; below 400 nm each is extended at its 400-410 nm slope; nothing past 700 nm. The camera has an IR-cut:
 *    on 12 stacks a model that let the sensor see to 1000 nm left 2 to 3 times the colour trend (LiveStacker::calibrateColour's trend_r/b_per_bprp), 2026-11-21.
 *  - the stars: the mean of the dwarf and giant spectra at the same BP-RP by default ($ORIGIN_STAR_TEMPLATES = dwarf, giant or mix). A field star of G 9..13.5 redder
 *    than about BP-RP 1 is as likely to be a giant as a dwarf, and their spectra differ (gravity-sensitive lines and molecular bands) inside narrow bands.
 *    On 12 stacks (2026-11-21) the mean residual colour slope was dwarf 0.016/0.008 (R/B, tri-band) and 0.030/0.019 (Clear), giant 0.020/0.018 and
 *    0.012/0.009, mix 0.015/0.011 and 0.020/0.017; the old black body through approximate curves about 0.02/0.06 and 0.03/0.09.
 * Real spectra rather than a black body matter with the tri-band filter: its bands hold the Balmer lines and, in cool stars, metal and molecular absorption.
 */
class StarColourModel {
public:
    explicit StarColourModel(const QString& filter = QString(), QString templates = QString())
    {
        m_tri = filter.compare("Clear", Qt::CaseInsensitive) != 0 && !filter.isEmpty();
        if (templates.isEmpty()) templates = qEnvironmentVariable("ORIGIN_STAR_TEMPLATES", kDefaultTemplates);
        m_templates = templates == "giant" || templates == "mix" ? templates : QString("dwarf");
        double resp[3][kN];
        for (int k = 0; k < kN; ++k) {
            const double l = lam(k), filt = m_tri ? table(kAntliaTriband2, 171, 380, 2, l) : (l <= 700 ? 0.95 : 0.0);
            for (int c = 0; c < 3; ++c) resp[c][k] = filt * sensor(c, l);
        }
        auto fold = [&](const StarSpectrum* t, int n, std::vector<Point>& out) {
            for (int i = 0; i < n; ++i) {
                Point p; p.bpRp = t[i].bpRp;
                for (int c = 0; c < 3; ++c) { p.s[c] = 0; for (int k = 0; k < kN; ++k) p.s[c] += t[i].photons[k] * resp[c][k]; }
                out.push_back(p);
            }
        };
        fold(kDwarfSpectra, int(std::size(kDwarfSpectra)), m_dwarf);
        fold(kGiantSpectra, int(std::size(kGiantSpectra)), m_giant);
    }
    bool isTriband() const { return m_tri; }
    QString name() const { return QString(m_tri ? "Antlia Triband RGB Ultra II" : "clear") + " x IMX178, Pickles " + m_templates + " spectra"; }
    /// the white point: a solar-type star, G2V (BP-RP 0.82), whatever the template choice
    std::array<double, 3> white() const { return at(m_dwarf, 0.82); }
    std::array<double, 3> predict(double bpRp) const
    {
        if (m_templates == "dwarf") return at(m_dwarf, bpRp);
        if (m_templates == "giant") return at(m_giant, bpRp);
        const auto d = at(m_dwarf, bpRp), g = at(m_giant, bpRp);
        return {0.5 * (d[0] + g[0]), 0.5 * (d[1] + g[1]), 0.5 * (d[2] + g[2])};
    }
private:
    static constexpr const char* kDefaultTemplates = "mix";
    static constexpr int kN = 171;                          // 380..720 nm in 2 nm bins, as StarSpectra.hpp
    static double lam(int k) { return 380.0 + 2 * k; }
    struct Point { double bpRp; double s[3]; };
    static std::array<double, 3> at(const std::vector<Point>& seq, double c)
    {
        size_t i = 1;
        while (i + 1 < seq.size() && seq[i].bpRp < c) ++i;
        const Point& a = seq[i - 1]; const Point& b = seq[i];
        const double t = std::clamp((c - a.bpRp) / std::max(1e-6, double(b.bpRp - a.bpRp)), 0.0, 1.0);
        return {a.s[0] + t * (b.s[0] - a.s[0]), a.s[1] + t * (b.s[1] - a.s[1]), a.s[2] + t * (b.s[2] - a.s[2])};
    }
    static double table(const float* t, int n, double l0, double step, double l)
    {
        const double f = (l - l0) / step; if (f <= 0) return t[0]; if (f >= n - 1) return t[n - 1];
        const int i = int(f); return t[i] + (f - i) * (t[i + 1] - t[i]);
    }
    static double sensor(int c, double l)
    {
        const float* rgb = c == 0 ? kImx178R : c == 1 ? kImx178G : kImx178B;
        if (l < 400) return std::max(0.0, rgb[0] + (rgb[0] - rgb[2]) / 10.0 * (400 - l));
        return l <= 700 ? table(rgb, 61, 400, 5, l) : 0.0;
    }
    bool m_tri = true;
    QString m_templates;
    std::vector<Point> m_dwarf, m_giant;
};

#endif
