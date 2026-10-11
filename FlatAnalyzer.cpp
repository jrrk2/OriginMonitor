#include "FlatAnalyzer.hpp"
#include "Image16.hpp"
#include "SkyFrame.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

float medianOf(std::vector<float> v)
{
    if (v.empty()) return 0.f;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

// median of a plane inside the box [x0,x1) x [y0,y1), sampling every `step` pixels
float boxMedian(const std::vector<float>& p, int w, int x0, int x1, int y0, int y1, int step = 3)
{
    std::vector<float> v;
    for (int y = y0; y < y1; y += step) for (int x = x0; x < x1; x += step) v.push_back(p[size_t(y) * w + x]);
    return medianOf(v);
}

// box blur with edge clamping, separable running sums
std::vector<float> boxBlur(const std::vector<float>& in, int w, int h, int r)
{
    std::vector<float> tmp(in.size()), out(in.size());
    for (int y = 0; y < h; ++y) {
        const float* row = &in[size_t(y) * w]; double acc = 0;
        for (int x = -r; x <= r; ++x) acc += row[std::clamp(x, 0, w - 1)];
        for (int x = 0; x < w; ++x) { tmp[size_t(y) * w + x] = float(acc / (2 * r + 1)); acc += row[std::clamp(x + r + 1, 0, w - 1)] - row[std::clamp(x - r, 0, w - 1)]; }
    }
    for (int x = 0; x < w; ++x) {
        double acc = 0;
        for (int y = -r; y <= r; ++y) acc += tmp[size_t(std::clamp(y, 0, h - 1)) * w + x];
        for (int y = 0; y < h; ++y) { out[size_t(y) * w + x] = float(acc / (2 * r + 1)); acc += tmp[size_t(std::clamp(y + r + 1, 0, h - 1)) * w + x] - tmp[size_t(std::clamp(y - r, 0, h - 1)) * w + x]; }
    }
    return out;
}

void putCard(QByteArray& h, const QString& key, const QString& value)
{
    QByteArray c = (key.leftJustified(8) + "= " + value).toLatin1(); c = c.leftJustified(80, ' ', true); h += c;
}

// three float planes as a FITS image (BITPIX -32, NAXIS 3), big endian
bool writeFits(const QString& path, const std::vector<float> planes[3], int w, int h)
{
    QByteArray hd;
    putCard(hd, "SIMPLE", "T"); putCard(hd, "BITPIX", "-32"); putCard(hd, "NAXIS", "3"); putCard(hd, "NAXIS1", QString::number(w)); putCard(hd, "NAXIS2", QString::number(h)); putCard(hd, "NAXIS3", "3");
    putCard(hd, "COMMENT", "master flat: planes R, G, B at half resolution, each divided by its own centre level; divide a light frame by it");
    hd += QByteArray("END").leftJustified(80, ' ');
    while (hd.size() % 2880) hd += ' ';
    QByteArray data; data.reserve(qsizetype(w) * h * 3 * 4);
    for (int c = 0; c < 3; ++c) for (size_t i = 0; i < planes[c].size(); ++i) {
        float f = planes[c][i]; quint32 u; static_assert(sizeof(u) == sizeof(f), "float size"); memcpy(&u, &f, 4);
        data.append(char(u >> 24)); data.append(char(u >> 16)); data.append(char(u >> 8)); data.append(char(u));
    }
    while (data.size() % 2880) data.append(char(0));
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(hd) == hd.size() && f.write(data) == data.size();
}

}   // namespace

QJsonObject analyseFlats(const QStringList& files, const QString& outDir, const QString& tag, double black)
{
    QDir().mkpath(outDir);
    int w = 0, h = 0, used = 0, rejected = 0; std::vector<double> sum[3]; std::vector<float> g0, g1; std::vector<double> sq;   // sq: sum of squares of the green plane for the scatter
    std::vector<float> levels;
    for (const QString& path : files) {
        QFile f(path); if (!f.open(QIODevice::ReadOnly)) { ++rejected; continue; }
        const SkyFrame F = analyseFrame(f.readAll(), 6.0, 0);
        if (!F.valid || F.g.empty()) { ++rejected; continue; }
        if (w == 0) { w = F.width; h = F.height; for (int c = 0; c < 3; ++c) sum[c].assign(size_t(w) * h, 0.0); sq.assign(size_t(w) * h, 0.0); }
        if (F.width != w || F.height != h) { ++rejected; continue; }
        const float gmed = medianOf(F.g) - float(black);
        if (gmed < 3000 || medianOf(F.g) > 60000) { ++rejected; continue; }       // too dark to beat the read noise, or close to full scale
        const std::vector<float>* src[3] = {&F.r, &F.g, &F.b};
        for (int c = 0; c < 3; ++c) for (size_t i = 0; i < sum[c].size(); ++i) sum[c][i] += ((*src[c])[i] - black) / gmed;
        for (size_t i = 0; i < sq.size(); ++i) { const double v = (F.g[i] - black) / gmed; sq[i] += v * v; }
        levels.push_back(gmed + float(black)); ++used;
    }
    if (used < 2) return QJsonObject{{"ok", false}, {"error", QString("only %1 usable frame(s) (%2 rejected as unreadable, too dark or too bright); need at least 2").arg(used).arg(rejected)}};

    std::vector<float> M[3];
    for (int c = 0; c < 3; ++c) { M[c].resize(sum[c].size()); for (size_t i = 0; i < M[c].size(); ++i) M[c][i] = float(sum[c][i] / used); }
    // per-pixel scatter of a single frame (relative), from the frame-to-frame variance of the green plane
    std::vector<float> sd; for (size_t i = 0; i < sq.size(); i += 17) { const double m = sum[1][i] / used, v = std::max(0.0, sq[i] / used - m * m) * used / (used - 1); sd.push_back(float(std::sqrt(v))); }
    const double singleFrameSnr = 1.0 / std::max(1e-6f, medianOf(sd));

    // centre levels and the normalised masters
    const int cx0 = w * 2 / 5, cx1 = w * 3 / 5, cy0 = h * 2 / 5, cy1 = h * 3 / 5;
    double centre[3];
    for (int c = 0; c < 3; ++c) { centre[c] = boxMedian(M[c], w, cx0, cx1, cy0, cy1, 2); if (centre[c] <= 0) centre[c] = 1; for (float& v : M[c]) v = float(v / centre[c]); }
    const std::vector<float>& G = M[1];

    // vignetting: median in 10 rings of normalised radius (0 = centre, 1 = corner)
    const double rmax = std::hypot(w / 2.0, h / 2.0); std::vector<std::vector<float>> ring(10);
    for (int y = 0; y < h; y += 3) for (int x = 0; x < w; x += 3) { const int k = std::min(9, int(std::hypot(x - w / 2.0, y - h / 2.0) / rmax * 10)); ring[k].push_back(G[size_t(y) * w + x]); }
    QJsonArray profile; for (auto& r : ring) profile.append(std::round(medianOf(r) * 1000) / 1000);

    // gradient: least-squares plane over the green master (sampled), as the % change across the width and the height
    double sxx = 0, syy = 0, sxz = 0, syz = 0, n = 0, mz = 0;
    for (int y = 0; y < h; y += 8) for (int x = 0; x < w; x += 8) { mz += G[size_t(y) * w + x]; ++n; }
    mz /= n;
    for (int y = 0; y < h; y += 8) for (int x = 0; x < w; x += 8) { const double dx = x - w / 2.0, dy = y - h / 2.0, z = G[size_t(y) * w + x] - mz; sxx += dx * dx; syy += dy * dy; sxz += dx * z; syz += dy * z; }
    const double gx = (sxx > 0 ? sxz / sxx : 0) * w * 100, gy = (syy > 0 ? syz / syy : 0) * h * 100;

    // dust: green over its own large-scale smooth version
    const std::vector<float> smooth = boxBlur(boxBlur(G, w, h, 40), w, h, 40);
    std::vector<float> ratio(G.size()); for (size_t i = 0; i < G.size(); ++i) ratio[i] = smooth[i] > 0.05f ? G[i] / smooth[i] : 1.f;
    std::vector<int> label(G.size(), 0); struct Blob { double sx = 0, sy = 0; int area = 0; float depth = 0; }; std::vector<Blob> blobs;
    std::vector<int> stack;
    for (int y = 2; y < h - 2; ++y) for (int x = 2; x < w - 2; ++x) {
        const size_t i = size_t(y) * w + x; if (label[i] || ratio[i] >= 0.97f) continue;
        Blob b; const int id = int(blobs.size()) + 1; stack.assign(1, int(i)); label[i] = id;
        while (!stack.empty()) {
            const int j = stack.back(); stack.pop_back(); const int jx = j % w, jy = j / w;
            b.sx += jx; b.sy += jy; ++b.area; b.depth = std::max(b.depth, 1.f - ratio[size_t(j)]);
            const int nb[4] = {j - 1, j + 1, j - w, j + w}; const bool ok[4] = {jx > 1, jx < w - 2, jy > 1, jy < h - 2};
            for (int k = 0; k < 4; ++k) if (ok[k] && !label[size_t(nb[k])] && ratio[size_t(nb[k])] < 0.97f) { label[size_t(nb[k])] = id; stack.push_back(nb[k]); }
        }
        if (b.area >= 6) blobs.push_back(b);
    }
    std::sort(blobs.begin(), blobs.end(), [](const Blob& a, const Blob& b) { return a.depth * std::sqrt(double(a.area)) > b.depth * std::sqrt(double(b.area)); });
    int ge3 = 0, ge5 = 0; QJsonArray top;
    for (const Blob& b : blobs) { if (b.depth >= 0.03f) ++ge3; if (b.depth >= 0.05f) ++ge5; }
    for (size_t k = 0; k < blobs.size() && k < 15; ++k) {
        const Blob& b = blobs[k];
        top.append(QJsonObject{{"x", int(std::round(b.sx / b.area * 2))}, {"y", int(std::round(b.sy / b.area * 2))}, {"diameter_px", std::round(2 * std::sqrt(b.area / M_PI) * 2 * 10) / 10}, {"depth_percent", std::round(b.depth * 1000) / 10}});
    }

    // colour: R/G and B/G (each plane is normalised to its own centre, so the ratio is relative to the centre colour) at the corners
    auto cornerRatio = [&](int c) {
        std::vector<float> v; const int bw = w / 8, bh = h / 8;
        for (const auto& o : {std::pair<int, int>{0, 0}, {w - bw, 0}, {0, h - bh}, {w - bw, h - bh}}) { const float a = boxMedian(M[c], w, o.first, o.first + bw, o.second, o.second + bh), g = boxMedian(M[1], w, o.first, o.first + bw, o.second, o.second + bh); if (g > 0) v.push_back(a / g); }
        return medianOf(v);
    };

    // pictures and the master
    std::vector<float> dustMap(ratio);
    Image16 pic = Image16::make(w, h), dp = Image16::make(w, h);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const size_t i = size_t(y) * w + x;
        const quint16 a = quint16(std::clamp((G[i] - 0.80f) / 0.30f, 0.f, 1.f) * 65535.f), b = quint16(std::clamp((ratio[i] - 0.90f) / 0.15f, 0.f, 1.f) * 65535.f);
        for (int c = 0; c < 3; ++c) { pic.row(y)[x * 3 + c] = a; dp.row(y)[x * 3 + c] = b; }
    }
    const QString picPath = outDir + "/flat_" + tag + ".png", dustPath = outDir + "/flat_dust_" + tag + ".png", fitsPath = outDir + "/master_flat_" + tag + ".fits";
    const bool okPic = pic.savePng(picPath), okDust = dp.savePng(dustPath), okFits = writeFits(fitsPath, M, w, h);

    return QJsonObject{{"ok", true}, {"frames_used", used}, {"frames_rejected", rejected}, {"median_adu_per_frame", std::round(medianOf(levels))},
                       {"single_frame_snr_per_pixel", std::round(singleFrameSnr * 10) / 10},
                       {"vignetting", QJsonObject{{"rings_centre_to_corner", profile}, {"corner_over_centre", profile[9]}}},
                       {"gradient_percent_across", QJsonObject{{"width", std::round(gx * 10) / 10}, {"height", std::round(gy * 10) / 10}, {"note", "change of the green master across the frame: an uneven sky, not the optics"}}},
                       {"dust", QJsonObject{{"regions_ge_3pct", ge3}, {"regions_ge_5pct", ge5}, {"deepest", top}, {"coordinates", "full-resolution pixels, origin top-left"}}},
                       {"colour_at_corners", QJsonObject{{"red_over_green", std::round(cornerRatio(0) * 1000) / 1000}, {"blue_over_green", std::round(cornerRatio(2) * 1000) / 1000}}},
                       {"centre_levels_relative", QJsonObject{{"r", std::round(centre[0] / centre[1] * 1000) / 1000}, {"b", std::round(centre[2] / centre[1] * 1000) / 1000}}},
                       {"files", QJsonObject{{"flat_png", okPic ? picPath : QString()}, {"dust_png", okDust ? dustPath : QString()}, {"master_fits", okFits ? fitsPath : QString()}}},
                       {"half_resolution", QJsonArray{w, h}}};
}
