#pragma once

#include "Registration.hpp"
#include "SkyFrame.hpp"

#include "Image16.hpp"
#include <QJsonObject>
#include <QString>
#include <vector>

/**
 * Incremental stack of one target. Every frame is registered to the first one (alt/az field rotation included), has its sky gradient and
 * row/column banding removed, is scaled to the reference transparency and exposure, and is added to a weighted running mean with a
 * running-variance outlier clip (satellites, planes, cosmic rays and the sensor's fixed hot pixels, which the rotating field spreads out).
 * The state can be saved and restored, so a target can be revisited after clouds or another target.
 */
class LiveStacker {
public:
    struct Report {
        bool accepted = false;
        QString reason;               // why a frame was refused, when it was
        int inliers = 0;              // stars matched to the reference
        double rms = 0;               // registration residual, pixels
        double rotationDeg = 0;       // field rotation relative to the reference
        double shiftPx = 0;           // translation relative to the reference
        double rawTransparency = 0;   // median star flux ratio to the reference (normalised by exposure and ISO gain)
        double transparency = 0;      // rawTransparency relative to the best seen so far for this target (1 = as clear as it gets)
        double matchedFraction = 0;   // fraction of the reference's brightest stars found in this frame
        int frames = 0;               // frames in the stack after this one
        static constexpr int kTilesX = 4, kTilesY = 3;
        int tileExpected[kTilesX * kTilesY] = {};   // reference stars per tile (reference frame, row-major from the top-left)
        int tileFound[kTilesX * kTilesY] = {};      // of those, found in this frame
    };

    /// Register only (no stacking): used to judge a frame against the stack's star field. iso <= 0: unknown, taken as the reference's.
    Report assess(const SkyFrame& frame, double exposureS, int iso = 0) const;
    /// Register and stack. Frames that cannot be registered, or that are far below the reference's clarity, are refused.
    Report add(const SkyFrame& frame, double exposureS, double minTransparency = 0.5, int iso = 0, const QString& filter = QString());

    bool hasReference() const { return m_frames > 0; }
    int frames() const { return m_frames; }
    double totalExposure() const { return m_totalExposure; }
    const std::vector<SkyStar>& referenceStars() const { return m_refStars; }
    int referenceFrameWidth() const { return m_w; }
    int referenceFrameHeight() const { return m_h; }
    int referenceIso() const { return m_refIso; }
    QString referenceFilter() const { return m_refFilter; }   // the FITS FILTER of the reference frame (empty: unknown)
    Image16 preview() const;                               // stretched (16 bits per channel), cropped to the well-covered area
    QJsonObject summary() const;

    bool save(const QString& dir) const;                   // state.bin + preview.png
    bool load(const QString& dir);
    void reset();

private:
    Report registerFrame(const SkyFrame& f, double exposureS, int iso, Similarity* T) const;
    double isoScale(int iso) const { return iso > 0 && m_refIso > 0 ? double(iso) / m_refIso : 1.0; }   // signal in ADU is proportional to ISO (2026-10-27 ladder: ISO 1600 = 2 x ISO 800; ISO 200 read 0.25 x)

    int m_w = 0, m_h = 0, m_frames = 0;
    double m_totalExposure = 0, m_refExposure = 0, m_bestRaw = 0;
    int m_refIso = 0;                                      // ISO of the reference frame (0 unknown); frames at another ISO are scaled by iso/m_refIso
    QString m_refFilter;                                   // filter of the reference frame: star fluxes through another filter are not comparable (SkyMonitor starts a new stack)
    std::vector<double> m_recentRaw;
    Similarity m_last;
    bool m_haveLast = false;
    std::vector<SkyStar> m_refStars;                       // brightest reference stars, reference-frame coordinates
    double m_wbR = 1, m_wbB = 1;                           // gains that make the median star neutral
    std::vector<double> m_sx[3], m_sx2[3], m_sw[3];        // per colour: sum w*v, sum w*v^2, sum w
};

/// Peak-to-peak variation (in sky-noise units) of the smooth quadratic sky surface fitted to the frame's luminance: large when a moon halo,
/// twilight, light dome or a cloud edge tilts the background.
double skyGradient(const SkyFrame& frame);
