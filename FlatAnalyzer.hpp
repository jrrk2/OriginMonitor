#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

/**
 * Flat-field analysis from raw 16-bit Bayer FITS frames of a uniformly lit sky (an overcast deck, or twilight sky near the zenith).
 *
 * Every frame is reduced to half resolution (R, G, B planes, see SkyFrame), the black level removed and the frame divided by its own green
 * median, so exposure and slow brightness drift cancel; the frames are then averaged into a master flat. From the master:
 *   vignetting:  median level in 10 rings (centre = 1) and the corner-to-centre ratio
 *   gradient:    the best-fit plane over the green master, as the change across the frame width and height (an uneven sky, not the optics)
 *   dust:        green divided by its own large-scale smooth version; connected dark regions of at least 3% depth are dust shadows (reported
 *                with position, diameter and depth in full-resolution pixels)
 *   colour:      red/green and blue/green at the centre and in the corners (colour vignetting)
 *   noise:       per-pixel signal to noise of a single frame, from the frame-to-frame scatter
 * Outputs (in outDir): master_flat_<tag>.fits (three float planes R, G, B, each divided by its own centre level, ready to divide a light frame by),
 * flat_<tag>.png (the green master, stretched 0.80..1.10), flat_dust_<tag>.png (the dust map, stretched 0.90..1.05).
 */
QJsonObject analyseFlats(const QStringList& files, const QString& outDir, const QString& tag, double blackLevel = 4985.0);
