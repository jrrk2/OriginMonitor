// Host alignment and host focus; see HostAlign.hpp. The pointing model, cord wrap, golden spiral and calibration logic follow PI.N.S.'s OriginPointingModel,
// OriginCordWrap, OriginModelBuilder and OriginCalibration (~/pins, MPL-2.0); the focus fit is NINA's hyperbola, solved in closed form.
#include "HostAlign.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <array>
#include <cmath>

namespace HostAlign {

namespace {
constexpr double PI = 3.14159265358979323846;
double rad(double d) { return d * PI / 180.0; }
double deg(double r) { return r * 180.0 / PI; }
using V3 = std::array<double, 3>;

V3 vec(double alt, double az) { const double a = rad(alt), z = rad(az); return {std::cos(a) * std::cos(z), std::cos(a) * std::sin(z), std::sin(a)}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double angleDeg(const V3& a, const V3& b)      // atan2 of |a x b| and a.b: exact for small angles too, where acos loses half its digits
{
    const V3 c{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    return deg(std::atan2(std::sqrt(dot(c, c)), dot(a, b)));
}

V3 mountVector(double enc0, double enc1, double k, double f, double cone)
{
    const double a = enc1 + k;
    const double e = rad(a - f * std::cos(rad(a))), az = rad(enc0), c = rad(cone);
    const double bx = std::cos(c) * std::cos(e), by = std::sin(c), bz = std::cos(c) * std::sin(e);
    return {bx * std::cos(az) - by * std::sin(az), bx * std::sin(az) + by * std::cos(az), bz};
}

V3 applyQ(double w, double x, double y, double z, const V3& v)
{
    return {(1 - 2 * (y * y + z * z)) * v[0] + 2 * (x * y - w * z) * v[1] + 2 * (x * z + w * y) * v[2],
            2 * (x * y + w * z) * v[0] + (1 - 2 * (x * x + z * z)) * v[1] + 2 * (y * z - w * x) * v[2],
            2 * (x * z - w * y) * v[0] + 2 * (y * z + w * x) * v[1] + (1 - 2 * (x * x + y * y)) * v[2]};
}

// cyclic Jacobi on a symmetric 4x4; the eigenvector of the largest eigenvalue
std::array<double, 4> largestEigenvector(double a[4][4])
{
    double v[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0;
        for (int p = 0; p < 4; ++p) for (int q = p + 1; q < 4; ++q) off += a[p][q] * a[p][q];
        if (off < 1e-24) break;
        for (int p = 0; p < 4; ++p) for (int q = p + 1; q < 4; ++q) {
            if (std::fabs(a[p][q]) < 1e-300) continue;
            const double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
            const double t = (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1));
            const double c = 1 / std::sqrt(t * t + 1), s = t * c;
            for (int k = 0; k < 4; ++k) { const double akp = a[k][p], akq = a[k][q]; a[k][p] = c * akp - s * akq; a[k][q] = s * akp + c * akq; }
            for (int k = 0; k < 4; ++k) { const double apk = a[p][k], aqk = a[q][k]; a[p][k] = c * apk - s * aqk; a[q][k] = s * apk + c * aqk; }
            for (int k = 0; k < 4; ++k) { const double vkp = v[k][p], vkq = v[k][q]; v[k][p] = c * vkp - s * vkq; v[k][q] = s * vkp + c * vkq; }
        }
    }
    int best = 0; for (int i = 1; i < 4; ++i) if (a[i][i] > a[best][best]) best = i;
    std::array<double, 4> e{v[0][best], v[1][best], v[2][best], v[3][best]};
    const double n = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2] + e[3] * e[3]);
    for (double& x : e) x /= n;
    return e;
}

std::array<double, 4> horn(const QList<V3>& from, const QList<V3>& to)
{
    double s[3][3] = {};
    for (int i = 0; i < from.size(); ++i) for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) s[r][c] += from[i][r] * to[i][c];
    const double sxx = s[0][0], sxy = s[0][1], sxz = s[0][2], syx = s[1][0], syy = s[1][1], syz = s[1][2], szx = s[2][0], szy = s[2][1], szz = s[2][2];
    double n[4][4] = {{sxx + syy + szz, syz - szy, szx - sxz, sxy - syx},
                      {syz - szy, sxx - syy - szz, sxy + syx, szx + sxz},
                      {szx - sxz, sxy + syx, -sxx + syy - szz, syz + szy},
                      {sxy - syx, szx + sxz, syz + szy, -sxx - syy + szz}};
    return largestEigenvector(n);
}

Model makeModel(double w, double x, double y, double z, double k, double f, double c)
{
    Model m; const double n = std::sqrt(w * w + x * x + y * y + z * z);
    m.w = w / n; m.x = x / n; m.y = y / n; m.z = z / n; m.altitudeZero = k; m.flexure = f; m.cone = c; m.created = QDateTime::currentDateTimeUtc();
    return m;
}
} // namespace

double norm360(double d) { return std::fmod(std::fmod(d, 360.0) + 360.0, 360.0); }
double wrap180(double d) { return std::remainder(d, 360.0); }

// ---- Model ----

Astro::AltAz Model::toSky(double enc0, double enc1) const
{
    const V3 t = applyQ(w, x, y, z, mountVector(enc0, enc1, altitudeZero, flexure, cone));
    return {deg(std::asin(std::clamp(t[2], -1.0, 1.0))), norm360(deg(std::atan2(t[1], t[0])))};
}

void Model::toEncoders(double alt, double az, double* enc0, double* enc1) const
{
    const V3 m = applyQ(w, -x, -y, -z, vec(alt, az));
    const double c = rad(cone);
    const double elevation = std::asin(std::clamp(m[2] / std::cos(c), -1.0, 1.0));
    const double azimuthAxis = std::atan2(m[1], m[0]) - std::atan2(std::sin(c), std::cos(c) * std::cos(elevation));
    const double mountAlt = deg(elevation);
    double a = mountAlt;
    for (int i = 0; i < 30; ++i) a = mountAlt + flexure * std::cos(rad(a));     // a - F cos a = mountAlt
    *enc0 = wrap180(deg(azimuthAxis)); *enc1 = a - altitudeZero;
}

double Model::residualArcmin(const Point& p) const
{
    const Astro::AltAz s = toSky(p.enc0, p.enc1);
    return angleDeg(vec(s.alt, s.az), vec(p.alt, p.az)) * 60.0;
}

void Model::baseTilt(double* degrees, double* towardAz) const
{
    const V3 up = applyQ(w, x, y, z, {0, 0, 1});
    *degrees = deg(std::acos(std::clamp(up[2], -1.0, 1.0))); *towardAz = norm360(deg(std::atan2(up[1], up[0])));
}

Model Model::fit(const QList<Point>& pts, int fitFlexure, int fitCone)
{
    const bool withF = fitFlexure < 0 ? pts.size() >= PointsForFlexure : fitFlexure > 0;
    const bool withC = fitCone < 0 ? pts.size() >= PointsForCone : fitCone > 0;
    QList<V3> truth; for (const Point& p : pts) truth.append(vec(p.alt, p.az));
    struct Trial { double rms = 1e300, k = 0, f = 0, c = 0; std::array<double, 4> q{1, 0, 0, 0}; };
    auto tryIt = [&](double k, double f, double c) {
        QList<V3> mount; for (const Point& p : pts) mount.append(mountVector(p.enc0, p.enc1, k, f, c));
        const std::array<double, 4> q = horn(mount, truth);
        double sum = 0;
        for (int i = 0; i < pts.size(); ++i) { const double e = angleDeg(applyQ(q[0], q[1], q[2], q[3], mount[i]), truth[i]) * 60; sum += e * e; }
        return Trial{std::sqrt(sum / pts.size()), k, f, c, q};
    };
    Trial best;
    for (double k = -90; k <= 90.0001; k += 0.5)
        for (double f = withF ? -2.0 : 0.0; f <= (withF ? 2.0001 : 0.0); f += 0.1) { const Trial t = tryIt(k, f, 0); if (t.rms < best.rms) best = t; }
    // K and F trade off along a narrow sloping valley: a pattern search re-centres on each better triple, then shrinks its steps
    for (double kStep = 0.5, fStep = withF ? 0.1 : 0, cStep = withC ? 0.1 : 0; kStep > 2e-4; kStep /= 5, fStep /= 5, cStep /= 5) {
        for (bool moved = true; moved;) {
            const Trial centre = best; Trial cand;
            for (int i = -2; i <= 2; ++i) for (int j = withF ? -2 : 0; j <= (withF ? 2 : 0); ++j) for (int l = withC ? -2 : 0; l <= (withC ? 2 : 0); ++l) {
                const Trial t = tryIt(centre.k + i * kStep, centre.f + j * fStep, centre.c + l * cStep);
                if (t.rms < cand.rms) cand = t;
            }
            moved = cand.rms < best.rms - 1e-9;
            if (moved) best = cand;
        }
    }
    Model m = makeModel(best.q[0], best.q[1], best.q[2], best.q[3], best.k, best.f, best.c);
    m.rmsArcmin = best.rms; m.points = int(pts.size());
    return m;
}

Model Model::level(const QList<Point>& pts)
{
    double k = 0, s = 0, c = 0;
    for (const Point& p : pts) { k += p.alt - p.enc1; s += std::sin(rad(p.az - p.enc0)); c += std::cos(rad(p.az - p.enc0)); }
    k /= pts.size();
    const double half = std::atan2(s, c) / 2;
    Model m = makeModel(std::cos(half), 0, 0, std::sin(half), k, 0, 0);
    m.points = int(pts.size());
    double sum = 0; for (const Point& p : pts) sum += std::pow(m.residualArcmin(p), 2);
    m.rmsArcmin = std::sqrt(sum / pts.size());
    return m;
}

QJsonObject Model::toJson() const
{
    double t, ta; baseTilt(&t, &ta);
    return {{"w", w}, {"x", x}, {"y", y}, {"z", z}, {"altitude_zero", altitudeZero}, {"flexure", flexure}, {"cone", cone}, {"rms_arcmin", rmsArcmin},
            {"points", points}, {"created", created.toString(Qt::ISODate)}, {"base_tilt_deg", t}, {"base_tilt_toward_az", ta}};
}

Model Model::fromJson(const QJsonObject& o)
{
    Model m = makeModel(o["w"].toDouble(1), o["x"].toDouble(), o["y"].toDouble(), o["z"].toDouble(), o["altitude_zero"].toDouble(), o["flexure"].toDouble(), o["cone"].toDouble());
    m.rmsArcmin = o["rms_arcmin"].toDouble(); m.points = o["points"].toInt(); m.created = QDateTime::fromString(o["created"].toString(), Qt::ISODate);
    return m;
}

// ---- CordWrap ----

CordWrap::CordWrap(const QString& file, double limit) : m_file(file), m_limit(limit)
{
    QFile f(file);
    if (!file.isEmpty() && f.open(QIODevice::ReadOnly)) {
        const QJsonValue n = QJsonDocument::fromJson(f.readAll()).object()["neutral_enc0_degrees"];
        if (n.isDouble()) m_neutral = n.toDouble();
    }
}

void CordWrap::update(double enc0Deg)
{
    const double e = wrap180(enc0Deg);
    if (m_last) m_turned += wrap180(e - *m_last);
    else if (m_neutral) m_turned = wrap180(e - *m_neutral);
    else { m_neutral = e; m_turned = 0; save(); }
    m_last = e;
}

void CordWrap::setNeutralHere() { if (m_last) { m_neutral = *m_last; m_turned = 0; save(); } }

bool CordWrap::turnedFor(double enc0Deg, double* out, QString* why) const
{
    const double n = m_neutral ? *m_neutral : (m_last ? *m_last : 0);
    const double t = wrap180(enc0Deg - n);
    if (std::fabs(t) > m_limit) { if (why) *why = QString("an azimuth %1 degrees from cable-neutral is beyond the %2 degree limit").arg(t, 0, 'f', 0).arg(m_limit, 0, 'f', 0); return false; }
    *out = t; return true;
}

double CordWrap::turnedNear(double enc0Deg, double nearTurned) const
{
    const double n = m_neutral ? *m_neutral : (m_last ? *m_last : 0);
    return nearTurned + wrap180(enc0Deg - n - nearTurned);
}

void CordWrap::save() const
{
    if (m_file.isEmpty() || !m_neutral) return;
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    QFile f(m_file);
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(QJsonObject{{"neutral_enc0_degrees", *m_neutral}, {"set_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                                          {"note", "first encoder reading with no neutral kept: set by OriginHeadless hostalign"}}).toJson());
}

// ---- the sky ----

double refractionTrueToApparent(double h, double p, double t)
{
    if (h < -1) return 0;
    return 1.02 / std::tan(rad(h + 10.3 / (h + 5.11))) / 60.0 * (p / 1010.0) * (283.0 / (273.0 + t));
}

double refractionApparentToTrue(double h0, double p, double t)
{
    // the exact inverse of the forward formula, so that a round trip is exact: true h with h + R(h) = h0
    double h = h0;
    for (int i = 0; i < 20; ++i) h = h0 - refractionTrueToApparent(h, p, t);
    return h0 - h;
}

Astro::AltAz toHorizon(double raDeg, double decDeg, const QDateTime& utc, const Astro::Site& site, bool refract, double p, double t)
{
    Astro::AltAz a = Astro::altAz({raDeg / 15.0, decDeg}, site, utc);
    if (refract) a.alt += refractionTrueToApparent(a.alt, p, t);
    return a;
}

void toJ2000(double alt, double az, const QDateTime& utc, const Astro::Site& site, bool refract, double* raDeg, double* decDeg, double p, double t)
{
    if (refract) alt -= refractionApparentToTrue(alt, p, t);
    const double jd = Astro::julianDate(utc);
    const double a = rad(alt), z = rad(az), phi = rad(site.latDeg);
    const double sinDec = std::sin(a) * std::sin(phi) + std::cos(a) * std::cos(z) * std::cos(phi);
    const double ha = std::atan2(-std::cos(a) * std::sin(z), std::sin(a) * std::cos(phi) - std::cos(a) * std::cos(z) * std::sin(phi));
    const Astro::Equatorial ofDate{norm360(Astro::lstDegrees(jd, site.lonDeg) - deg(ha)) / 15.0, deg(std::asin(std::clamp(sinDec, -1.0, 1.0)))};
    const Astro::Equatorial j2000 = Astro::precess(ofDate, jd, 2451545.0);
    *raDeg = norm360(j2000.raHours * 15.0); *decDeg = j2000.decDeg;
}

double separationDeg(double alt1, double az1, double alt2, double az2) { return angleDeg(vec(alt1, az1), vec(alt2, az2)); }

QList<Astro::AltAz> goldenSpiral(int count, double minAlt, double maxAlt, double minAz, double maxAz)
{
    QList<Astro::AltAz> out;
    if (count < 1 || minAlt >= maxAlt) return out;
    const double low = std::sin(rad(minAlt)), high = std::sin(rad(maxAlt));
    double span = norm360(maxAz - minAz); if (span < 1e-9) span = 360;
    const double golden = (std::sqrt(5.0) - 1) / 2;
    for (int i = 0; i < count; ++i)
        out.append({deg(std::asin(low + (high - low) * (i + 0.5) / count)), norm360(minAz + span * std::fmod(i * golden, 1.0))});
    return out;
}

// ---- Calibration ----

double Calibration::refine(const Point& p)
{
    const double error = hasModel ? model.residualArcmin(p) : -1;
    if (pts.size() >= 3 && error > RejectDeg * 60) return -2;
    pts.erase(std::remove_if(pts.begin(), pts.end(), [&](const Point& q) { return separationDeg(q.alt, q.az, p.alt, p.az) < SamePlaceDeg; }), pts.end());
    pts.append(p);
    refit();
    // what the new model still misses at this point, for targets near it
    const Astro::AltAz s = model.toSky(p.enc0, p.enc1);
    const double dAlt = p.alt - s.alt, dAz = wrap180(p.az - s.az);
    if (std::fabs(dAlt) > 1e-6 || std::fabs(dAz) > 1e-6) { hasLocal = true; localAlt = p.alt; localAz = p.az; localDAlt = dAlt; localDAz = dAz; }
    return error;
}

void Calibration::refit()
{
    hasLocal = false;
    hasModel = !pts.isEmpty();
    if (pts.size() >= 3) model = Model::fit(pts);
    else if (!pts.isEmpty()) model = Model::level({pts.last()});     // a two-point average on a tilted base puts both out: the newest only, until three
}

void Calibration::aim(double alt, double az, double* enc0, double* enc1) const
{
    if (!hasLocal) { model.toEncoders(alt, az, enc0, enc1); return; }
    const double sep = separationDeg(alt, az, localAlt, localAz);
    const double wgt = sep <= LocalFull ? 1 : sep >= LocalFade ? 0 : (LocalFade - sep) / (LocalFade - LocalFull);
    model.toEncoders(alt - wgt * localDAlt, norm360(az - wgt * localDAz), enc0, enc1);
}

QJsonObject Calibration::toJson() const
{
    QJsonArray a;
    for (const Point& p : pts) a.append(QJsonObject{{"enc0", p.enc0}, {"enc1", p.enc1}, {"alt", p.alt}, {"az", p.az}, {"utc", p.utc.toString(Qt::ISODate)},
                                                    {"residual_arcmin", hasModel ? model.residualArcmin(p) : 0.0}});
    QJsonObject o{{"points", a}};
    if (hasModel) o["model"] = model.toJson();
    if (hasLocal) o["local"] = QJsonObject{{"alt", localAlt}, {"az", localAz}, {"d_alt", localDAlt}, {"d_az", localDAz}};
    return o;
}

Calibration Calibration::fromJson(const QJsonObject& o)
{
    Calibration c;
    for (const QJsonValue& v : o["points"].toArray()) {
        const QJsonObject p = v.toObject();
        c.pts.append({p["enc0"].toDouble(), p["enc1"].toDouble(), p["alt"].toDouble(), p["az"].toDouble(), QDateTime::fromString(p["utc"].toString(), Qt::ISODate)});
    }
    c.refit();
    if (o.contains("local")) {
        const QJsonObject l = o["local"].toObject();
        c.hasLocal = true; c.localAlt = l["alt"].toDouble(); c.localAz = l["az"].toDouble(); c.localDAlt = l["d_alt"].toDouble(); c.localDAz = l["d_az"].toDouble();
    }
    return c;
}

// ---- focus ----

FocusFit fitHyperbola(const QList<double>& pos, const QList<double>& hfr)
{
    FocusFit r;
    const int n = int(std::min(pos.size(), hfr.size()));
    if (n < 4) { r.why = "fewer than 4 measured positions"; return r; }
    // centre and scale x for conditioning
    double xm = 0; for (int i = 0; i < n; ++i) xm += pos[i]; xm /= n;
    double xs = 0; for (int i = 0; i < n; ++i) xs = std::max(xs, std::fabs(pos[i] - xm)); if (xs <= 0) xs = 1;
    // y = HFR^2 = p0 + p1 u + p2 u^2, u = (x - xm)/xs; weights 1/HFR^2 so that the sharp end counts as much as the blurred wings
    double S[3][3] = {}, B[3] = {};
    for (int i = 0; i < n; ++i) {
        const double u = (pos[i] - xm) / xs, y = hfr[i] * hfr[i], wgt = 1.0 / std::max(1e-6, y);
        const double b[3] = {1, u, u * u};
        for (int r0 = 0; r0 < 3; ++r0) { B[r0] += wgt * b[r0] * y; for (int c0 = 0; c0 < 3; ++c0) S[r0][c0] += wgt * b[r0] * b[c0]; }
    }
    // Cramer's rule
    auto det3 = [](double m[3][3]) { return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]); };
    const double D = det3(S);
    if (std::fabs(D) < 1e-18) { r.why = "singular fit"; return r; }
    double p[3];
    for (int k = 0; k < 3; ++k) { double M[3][3]; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) M[i][j] = j == k ? B[i] : S[i][j]; p[k] = det3(M) / D; }
    if (p[2] <= 0) { r.why = "the curve does not open upward (no V: too narrow a sweep, or no stars)"; return r; }
    const double uc = -p[1] / (2 * p[2]);
    const double a2 = p[0] - p[1] * p[1] / (4 * p[2]);
    r.best = xm + uc * xs; r.minHfr = std::sqrt(std::max(0.0, a2)); r.slope = std::sqrt(p[2]) / xs;
    double ss = 0, st = 0, mean = 0; for (int i = 0; i < n; ++i) mean += hfr[i]; mean /= n;
    for (int i = 0; i < n; ++i) {
        const double u = (pos[i] - xm) / xs, model = std::sqrt(std::max(0.0, p[0] + p[1] * u + p[2] * u * u));
        ss += (hfr[i] - model) * (hfr[i] - model); st += (hfr[i] - mean) * (hfr[i] - mean);
    }
    r.rms = std::sqrt(ss / n); r.r2 = st > 0 ? 1 - ss / st : 0;
    const double lo = *std::min_element(pos.begin(), pos.begin() + n), hi = *std::max_element(pos.begin(), pos.begin() + n);
    if (r.best < lo) { r.edge = -1; r.why = "the best focus lies below the positions measured"; return r; }
    if (r.best > hi) { r.edge = 1; r.why = "the best focus lies above the positions measured"; return r; }
    // NINA's check: both wings must rise: a vertex within one step of an edge is not trusted either
    const double stepGuess = (hi - lo) / std::max(1, n - 1);
    if (r.best - lo < 0.5 * stepGuess) { r.edge = -1; r.why = "the best focus is at the low edge of the sweep"; return r; }
    if (hi - r.best < 0.5 * stepGuess) { r.edge = 1; r.why = "the best focus is at the high edge of the sweep"; return r; }
    r.ok = true;
    return r;
}

} // namespace HostAlign
