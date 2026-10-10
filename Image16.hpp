#pragma once

#include <QString>
#include <QtGlobal>
#include <vector>

/// 16-bit-per-channel RGB image with no Qt GUI dependency (the live-stack preview), plus a PNG writer built on QtCore only.
struct Image16 {
    int width = 0, height = 0;
    std::vector<quint16> rgb;                       // interleaved R,G,B; row 0 is the top row
    bool isNull() const { return width <= 0 || height <= 0; }
    quint16* row(int y) { return &rgb[size_t(y) * width * 3]; }
    const quint16* row(int y) const { return &rgb[size_t(y) * width * 3]; }
    static Image16 make(int w, int h) { Image16 i; i.width = w; i.height = h; i.rgb.assign(size_t(w) * h * 3, 0); return i; }
    bool savePng(const QString& path) const;        // 16-bit truecolour PNG; false when the image is null or the file cannot be written
};
