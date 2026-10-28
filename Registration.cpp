#include "Registration.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Pair { int i, j; float d, ang; };

std::vector<Pair> pairsOf(const std::vector<SkyStar>& s, int K, float maxD = 1e9f)
{
    std::vector<Pair> v;
    const int n = std::min<int>(K, int(s.size()));
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            const double dx = s[j].x - s[i].x, dy = s[j].y - s[i].y, d = std::hypot(dx, dy);
            if (d <= maxD) v.push_back({i, j, float(d), float(std::atan2(dy, dx))});
        }
    return v;
}

// Least-squares similarity from matched points (complex form), optionally with the scale left free
bool fitSimilarity(const std::vector<std::pair<const SkyStar*, const SkyStar*>>& m, Similarity& T)
{
    if (m.size() < 3) return false;
    double pxm = 0, pym = 0, qxm = 0, qym = 0;
    for (auto& [p, q] : m) { pxm += p->x; pym += p->y; qxm += q->x; qym += q->y; }
    const double n = double(m.size()); pxm /= n; pym /= n; qxm /= n; qym /= n;
    double sr = 0, si = 0, sp = 0;
    for (auto& [p, q] : m) {
        const double px = p->x - pxm, py = p->y - pym, qx = q->x - qxm, qy = q->y - qym;
        sr += px * qx + py * qy;                 // Re(conj(p) q)
        si += px * qy - py * qx;                 // Im(conj(p) q)
        sp += px * px + py * py;
    }
    if (sp < 1e-6) return false;
    T.a = sr / sp; T.b = si / sp;
    T.tx = qxm - (T.a * pxm - T.b * pym);
    T.ty = qym - (T.b * pxm + T.a * pym);
    return true;
}

// Match every target star to the nearest reference star inside radius, refit; trims outliers on the way
int refine(const std::vector<SkyStar>& ref, const std::vector<SkyStar>& tgt, Similarity& T, double& rms)
{
    int count = 0;
    for (double radius : {8.0, 4.0, 2.5, 1.8}) {
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<std::pair<const SkyStar*, const SkyStar*>> m;
            std::vector<double> dist;
            for (const SkyStar& t : tgt) {
                double x, y; T.apply(t.x, t.y, x, y);
                const SkyStar* best = nullptr; double bd = radius;
                for (const SkyStar& r : ref) { const double dd = std::hypot(r.x - x, r.y - y); if (dd < bd) { bd = dd; best = &r; } }
                if (best) { m.push_back({&t, best}); dist.push_back(bd); }
            }
            if (m.size() < 4) return 0;
            if (pass == 1) {                         // drop the worst third before the final fit at this radius
                std::vector<double> s = dist; std::sort(s.begin(), s.end());
                const double cut = std::max(s[s.size() * 2 / 3], 0.5);
                std::vector<std::pair<const SkyStar*, const SkyStar*>> k;
                for (size_t i = 0; i < m.size(); ++i) if (dist[i] <= cut) k.push_back(m[i]);
                m.swap(k);
                if (m.size() < 4) return 0;
            }
            Similarity N; if (!fitSimilarity(m, N)) return 0;
            const double sc = std::hypot(N.a, N.b);
            if (sc < 0.97 || sc > 1.03) return 0;    // same camera: a scale far from 1 is a false match
            T = N;
        }
    }
    // final tally at 1.5 px
    double ss = 0; count = 0;
    for (const SkyStar& t : tgt) {
        double x, y; T.apply(t.x, t.y, x, y);
        double bd = 1.5;
        for (const SkyStar& r : ref) bd = std::min(bd, std::hypot(r.x - x, r.y - y));
        if (bd < 1.5) { ++count; ss += bd * bd; }
    }
    rms = count ? std::sqrt(ss / count) : 0;
    return count;
}

}   // namespace

double Similarity::scale() const { return std::hypot(a, b); }
double Similarity::rotationDeg() const { return std::atan2(b, a) * 180.0 / kPi; }

RegResult registerStars(const std::vector<SkyStar>& ref, const std::vector<SkyStar>& tgt, const Similarity* guess, int minInliers)
{
    RegResult R;
    R.tried = int(tgt.size());
    if (ref.size() < 6 || tgt.size() < 6) return R;
    const int need = std::max(minInliers, int(0.2 * std::min(ref.size(), tgt.size()) * 0.5));

    auto attempt = [&](Similarity T) {
        double rms = 0; const int n = refine(ref, tgt, T, rms);
        if (n >= need && n > R.inliers) { R.ok = true; R.T = T; R.inliers = n; R.rms = rms; }
    };
    if (guess) { attempt(*guess); if (R.ok) return R; }

    // Pair voting over the brightest K stars of each list: 40 first (fast), then 80 when that finds nothing. Bright-star ranks are scrambled when the frames differ in ISO or saturate
    // (21 Apr M101, ISO 1000 against an ISO 200 reference: 98 of 599 frames matched with K=40, 582 with K=80; PixInsight StarAlignment matched 588).
    // 160 and 300 for frames of very different exposure: detection skips saturated stars, so a 17 s frame's list starts where a 4 s frame's ends (M1, 2026-10-14: of the 4 s frame's
    // 40 brightest only 1 was in the 17 s frame's list at all, of its 300 stars 113 were). Those passes vote with pairs closer than 350 px only, which keeps them to about a second.
    for (const int K : {40, 80, 160, 300}) {
        // Equal separations in both lists constrain rotation and shift (scale is ~1)
        const float maxD = K > 80 ? 350.f : 1e9f;
        const std::vector<Pair> rp = pairsOf(ref, K, maxD);
        std::vector<Pair> tp = pairsOf(tgt, K, maxD);
        std::sort(tp.begin(), tp.end(), [](const Pair& a, const Pair& b) { return a.d < b.d; });
        struct Acc { int n = 0; double th = 0, tx = 0, ty = 0; };
        std::unordered_map<long long, Acc> votes;
        auto key = [](int a, int b, int c) { return ((long long)(a + 4096) << 40) | ((long long)(b + 4096) << 20) | (long long)(c + 4096); };
        for (const Pair& r : rp) {
            if (r.d < 40) continue;
            const float tol = 0.012f * r.d + 0.8f;
            auto lo = std::lower_bound(tp.begin(), tp.end(), r.d - tol, [](const Pair& p, float v) { return p.d < v; });
            for (auto it = lo; it != tp.end() && it->d <= r.d + tol; ++it) {
                for (int flip = 0; flip < 2; ++flip) {
                    const SkyStar& tk = tgt[flip ? it->j : it->i];
                    const double th = r.ang - (flip ? it->ang + kPi : it->ang);
                    const double c = std::cos(th), s = std::sin(th);
                    const SkyStar& ri = ref[r.i];
                    const double tx = ri.x - (c * tk.x - s * tk.y), ty = ri.y - (s * tk.x + c * tk.y);
                    double tn = std::remainder(th, 2 * kPi);
                    Acc& a = votes[key(int(std::lround(tn * 180 / kPi)), int(std::lround(tx / 8)), int(std::lround(ty / 8)))];
                    a.n++; a.th += tn; a.tx += tx; a.ty += ty;
                }
            }
        }
        // Rank bins by their own votes plus the neighbours', try the best few
        std::vector<std::pair<int, Acc>> top;
        for (auto& [k, a] : votes) if (a.n >= 3) top.push_back({a.n, a});
        std::sort(top.begin(), top.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        for (size_t i = 0; i < top.size() && i < 6 && !R.ok; ++i) {
            const Acc& a = top[i].second;
            const double th = a.th / a.n;
            Similarity T; T.a = std::cos(th); T.b = std::sin(th); T.tx = a.tx / a.n; T.ty = a.ty / a.n;
            attempt(T);
        }
        if (R.ok) break;
    }
    return R;
}
