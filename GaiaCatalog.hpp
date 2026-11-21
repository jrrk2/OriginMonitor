// GaiaCatalog.hpp - cone search in the local Gaia DR3 file written by tools/gaia_pack.py, and the black-body colour model the live stacker's
// colour calibration predicts each star's camera signal with. Gaia data: ESA/Gaia/DPAC (https://www.cosmos.esa.int/gaia).
#ifndef GAIACATALOG_H
#define GAIACATALOG_H

#include "DataDir.hpp"

#include <QFile>
#include <QFileInfo>
#include <QString>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
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

/**
 * Predicted R, G, B camera signal of a star from its Gaia BP-RP: a black body at the colour temperature (Mucciarelli & Bellazzini 2020, dwarfs:
 * theta = 0.4929 + 0.5092 C - 0.0353 C^2, Teff = 5040 / theta) in photons per wavelength, through the tri-band filter and the colour sensor.
 * The filter windows are the "Nebula" tri-band's: 425-450 nm (blue), 484-503 nm (H-beta and O III) and 650-676 nm (H-alpha, N II, S II).
 * The Bayer channel curves are smooth approximations of a front-illuminated colour CMOS (no published curve for the Origin's sensor).
 * A filter other than the tri-band ("Clear", empty) uses an IR-cut 400-690 nm window instead.
 */
class StarColourModel {
public:
    explicit StarColourModel(const QString& filter = QString())
    {
        m_triband = filter.compare("Clear", Qt::CaseInsensitive) != 0 && !filter.isEmpty();
        for (int k = 0; k < kN; ++k) {
            const double l = lam(k);
            auto window = [&](double lo, double hi) { return 1 / (1 + std::exp(-(l - lo) / 1.5)) / (1 + std::exp((l - hi) / 1.5)); };
            auto gauss = [&](double mu, double s) { return std::exp(-0.5 * ((l - mu) / s) * ((l - mu) / s)); };
            const double filt = m_triband ? 0.92 * (window(425, 450) + window(484, 503) + window(650, 676)) : 0.95 * window(400, 690);
            const double qe = 0.80 * gauss(530, 120);
            const double cfa[3] = {0.95 / (1 + std::exp(-(l - 590) / 12)) * (0.6 + 0.4 * gauss(620, 80)),
                                   0.90 * gauss(530, 42) + 0.08 * gauss(660, 30),
                                   0.92 * gauss(455, 38) + 0.05 * gauss(660, 30)};
            for (int c = 0; c < 3; ++c) m_resp[c][k] = filt * qe * cfa[c];
        }
    }
    bool isTriband() const { return m_triband; }
    static double teffFromBpRp(double c) { return 5040.0 / (0.4929 + 0.5092 * c - 0.0353 * c * c); }
    std::array<double, 3> predict(double teff) const
    {
        constexpr double h = 6.626e-34, kb = 1.381e-23, cl = 2.998e8;
        std::array<double, 3> s{0, 0, 0};
        for (int k = 0; k < kN; ++k) {
            const double L = lam(k) * 1e-9, photons = (2 * cl / (L * L * L * L)) / std::expm1(h * cl / (L * kb * teff));
            for (int c = 0; c < 3; ++c) s[c] += photons * m_resp[c][k];
        }
        return s;
    }
private:
    static constexpr int kN = 680;                          // 380..720 nm in 0.5 nm steps
    static double lam(int k) { return 380.0 + 0.5 * k; }
    bool m_triband = true;
    double m_resp[3][kN];
};

#endif
