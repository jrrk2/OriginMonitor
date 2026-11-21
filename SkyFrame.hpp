#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <vector>

/**
 * One raw 16-bit Bayer FITS snapshot, reduced to what live stacking and sky-quality decisions need.
 * The frame is binned to half resolution (each 2x2 Bayer cell becomes one pixel with R, G and B values), so all coordinates below are
 * half-resolution pixels. Stars are found on the luminance (mean of the four sensels).
 */
struct SkyStar {
    float x = 0, y = 0;      // centroid, half-res pixels
    float flux = 0;          // background-subtracted flux within the aperture, ADU
    float peak = 0;          // peak above the local sky, ADU
    float hfr = 0;           // half-flux radius, half-res pixels
    float sigma = 0;         // peak / sky noise
    bool isolated = false;   // no comparable neighbour inside two apertures: usable for focus measurement
    float elong = 1;         // axis ratio of the second moments inside the aperture (1 = round)
    float angle = 0;         // direction of the long axis, radians modulo pi
};

struct SkyFrame {
    bool valid = false;
    QString note;
    int fullWidth = 0, fullHeight = 0;
    int width = 0, height = 0;                       // half-res
    std::vector<float> r, g, b;                      // half-res colour planes, raw ADU (black level still in)
    std::vector<float> lum;                          // mean of the four sensels
    std::vector<SkyStar> stars;                      // brightest first
    double background = 0;                           // median luminance
    double noise = 0;                                // robust sky noise of the high-passed luminance
    bool greenOnMainDiagonal = false;                // detected Bayer layout (see analyseFrame)
};

SkyFrame analyseFrame(const QByteArray& fitsData, double detectSigma = 6.0, int maxStars = 300);

/// The filter a frame was taken through, from its stars, not from the FILTER keyword (which only says what the telescope was told is fitted). The raw blue/green
/// ratio of the isolated stars separates the two filters by a wide margin (survey of 924 archive frames, 2026-11-21, per-night medians): "Clear" 0.56-0.68,
/// the "Nebula" tri-band 1.01-1.20, whose blue window lets through more than its green one. Empty name when fewer than kFilterMinStars stars can be measured
/// (cloud, twilight: a dawn frame with 12 stars read 0.10).
constexpr int kFilterMinStars = 15;
constexpr double kTribandMinBG = 0.85;
struct FilterGuess { QString name; double starBG = 0; int stars = 0; };
FilterGuess detectFilter(const SkyFrame& f);

/// FITS header cards: one 80-character card, and a copy of a FITS file with cards added before END (the header grows by a block when it must)
QByteArray fitsCard(const char* key, const QString& value, const QString& comment, bool quoted);
QByteArray fitsWithCards(const QByteArray& fits, const QList<QByteArray>& cards);
/// FILTDTCT (the detected filter, or UNKNOWN), FILTDBG (median raw star blue/green), FILTDN (stars measured)
QList<QByteArray> filterCards(const FilterGuess& g);

/// Are the sources streaks? Among the 100 brightest sources, those with an axis ratio of at least 2: when there are at least 25 and their long axes agree (circular
/// coherence of the doubled angle 0.8 or better) the frame is trailed (the mount moved during the exposure), not cloud and not defocus. Noise in bright cloud gives many
/// "sources" but no common direction.
struct StreakInfo { int elongated = 0; double coherence = 0, angleDeg = 0; bool trailed = false; };
StreakInfo streakInfo(const SkyFrame& f);
