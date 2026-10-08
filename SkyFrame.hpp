#pragma once

#include <QByteArray>
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
