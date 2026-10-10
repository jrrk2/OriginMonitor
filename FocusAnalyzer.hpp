#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

/**
 * Focus quality of one snapshot, measured from the star images in the raw 16-bit Bayer FITS the camera delivers.
 *
 * The frame is binned 2x2 (which also removes the Bayer pattern), the sky is subtracted, and every isolated, unsaturated star
 * gets a half-flux radius (flux-weighted mean distance from the centroid). The median over the brightest stars is the figure of
 * merit: LOWER IS BETTER, and it is largely independent of exposure, transparency and which stars happen to be in the field.
 * Single-sensel hot pixels (a point with no wings) and saturated or crowded stars are excluded.
 */
struct FocusResult {
    bool valid = false;
    QString note;                 // why the frame could not be scored, when valid is false
    int stars = 0;                // stars that passed the quality tests
    double hfrPx = 0;             // median half-flux radius, full-resolution sensor pixels
    double hfrArcsec = 0;         // same, using the nominal scale of the Origin (150 mm RASA, 335 mm, 2.4 um pixels)
    double hfrSpreadPx = 0;       // robust scatter of the per-star radii (large = tilt, coma or a poor sample)
    double peakFraction = 0;      // median (peak above sky) / (total flux) for the stars: rises as focus improves
    double background = 0;        // sky level, ADU per binned pixel
    double noise = 0;             // robust sky noise, ADU per binned pixel
    int width = 0, height = 0;    // sensor pixels
    qint64 elapsedMs = 0;         // analysis time

    QJsonObject toJson() const;
};

struct SkyFrame;
/// Score an already analysed frame (the stacker and the focus score share one star finder).
FocusResult focusFromFrame(const SkyFrame& frame);
/// Score a FITS file held in memory (BITPIX 16 only). Thread-safe, no Qt event loop needed.
FocusResult evaluateFocus(const QByteArray& fitsData);
