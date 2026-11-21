#pragma once

#include "SkyFrame.hpp"
#include <vector>

/// Similarity transform mapping target-frame coordinates to reference coordinates: p' = c * p + t, with c = a + ib (complex multiply).
struct Similarity {
    double a = 1, b = 0, tx = 0, ty = 0;
    double scale() const;
    double rotationDeg() const;
    void apply(double x, double y, double& ox, double& oy) const { ox = a * x - b * y + tx; oy = b * x + a * y + ty; }
};

struct RegResult {
    bool ok = false;
    Similarity T;
    int inliers = 0;          // stars matched within 1.5 px
    int tried = 0;            // stars offered from the target
    double rms = 0;           // residual of the inliers, pixels
};

/**
 * Register a target star list onto a reference one (scale fixed near 1: same camera). With a guess, refinement from it is tried first,
 * which is nearly instant for consecutive frames; otherwise pair voting over the brightest stars finds the rotation and shift.
 */
/// maxK limits the pair voting to the brightest maxK stars of each list (40, 80, 160 and 300 are tried in turn up to it).
RegResult registerStars(const std::vector<SkyStar>& ref, const std::vector<SkyStar>& tgt, const Similarity* guess = nullptr, int minInliers = 10, int maxK = 300);
