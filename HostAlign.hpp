#pragma once

#include "Astro.hpp"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <optional>

/**
 * Host alignment and host focus: the PI.N.S. algorithms (~/pins, NINA.Equipment/Equipment/Origin, MPL-2.0), reimplemented here in C++.
 *
 * The firmware is only asked to move and to follow a track. Its own initialisation needs clear sky at two places it chooses, and its frame assumes a
 * level base. Instead:
 *  1. A fake initialisation (RunInitialize FakeInitialize) makes the firmware take gotos and tracks. Its frame is then the encoders plus two constant
 *     offsets (2026-11-08 on the real Origin: altitude = Enc1 + 40.377, azimuth = Enc0 + 74.616), with RA/Dec in J2000.
 *  2. Points: a move under encoder feedback (Slew rates, timed), a raw frame and a plate solve. The encoders against the solved apparent alt/az.
 *  3. A pointing model (below) fitted to the points: base tilt in any direction, azimuth and altitude encoder zeros, tube flexure, cone error.
 *  4. Gotos by encoder feedback to the model's encoder readings, within the cord-wrap limit.
 *  5. Tracking as a TargetTrack: the model's encoder path for the target, written in the firmware's frame (the firmware's sidereal tracking in a fake frame
 *     drifts by several arcseconds a second).
 * Focus is the host's too: the gotos are rate moves, so the firmware's post-goto autofocus never runs; a V-curve of HFR against focuser position is
 * fitted with a hyperbola (NINA's model) and the focuser sent to its vertex.
 */
namespace HostAlign {

// ---- the pointing model -------------------------------------------------------------------------------------------------------------------------

/// A plate-solved point: encoders in degrees, and where the tube really pointed, as apparent (refracted) altitude and azimuth.
struct Point { double enc0 = 0, enc1 = 0, alt = 0, az = 0; QDateTime utc; };

/**
 * Where the tube points for given encoder readings, measured rather than assumed. The mount's frame: azimuth A = Enc0, elevation E = a - F cos(a) with
 * a = Enc1 + K (K the altitude encoder's zero, F the flexure at the horizon); the optical axis is C off square to the altitude axis (cone error), so it
 * points along (cos C cos E, sin C, cos C sin E) turned by A. A unit quaternion takes that frame onto the true horizon (x north, y east, z up); it absorbs
 * the base's tilt and the azimuth encoder's zero. The rotation for given K, F, C is Horn's closed-form least-squares one (J. Opt. Soc. Am. A 4, 629, 1987);
 * K, F and C are found by a grid and then a pattern search that follows the narrow K-F valley.
 */
struct Model {
    double w = 1, x = 0, y = 0, z = 0;     // unit quaternion, mount frame -> horizon
    double altitudeZero = 0, flexure = 0, cone = 0;
    double rmsArcmin = 0; int points = 0; QDateTime created;

    static constexpr int PointsForFlexure = 5, PointsForCone = 6;

    Astro::AltAz toSky(double enc0, double enc1) const;
    void toEncoders(double alt, double az, double* enc0, double* enc1) const;     // enc0 in (-180, 180]
    double residualArcmin(const Point& p) const;
    void baseTilt(double* degrees, double* towardAz) const;

    /// at least three points; flexure from PointsForFlexure, cone from PointsForCone (fitFlexure/fitCone: -1 auto, 0 no, 1 yes)
    static Model fit(const QList<Point>& pts, int fitFlexure = -1, int fitCone = -1);
    /// one or two points, level base: azimuth and altitude zeros only
    static Model level(const QList<Point>& pts);

    QJsonObject toJson() const;
    static Model fromJson(const QJsonObject& o);
};

// ---- the cables -----------------------------------------------------------------------------------------------------------------------------------

/**
 * The running total of azimuth from cable-neutral (where the cables hang straight), followed from one encoder reading to the next. Moves are planned in
 * this count, never across the far side; safe within 180 degrees either side. On 2026-11-08 moves driven the short way round added up to almost three
 * turns and the power cord snapped. The neutral is kept in a file ({"neutral_enc0_degrees": x}, the same as PI.N.S.'s cordwrap.json).
 */
class CordWrap {
public:
    explicit CordWrap(const QString& file = QString(), double limit = 180);
    static constexpr double TrackOverrun = 45;     // a track may carry on this far past the limit so an observation is not cut short
    double limit() const { return m_limit; }
    bool hasNeutral() const { return m_neutral.has_value(); }
    double turned() const { return m_turned; }
    void update(double enc0Deg);                    // first reading ever (no file): becomes neutral and is saved
    void setNeutralHere();
    /// the count of an encoder reading within the limit; false (and *why) if it lies beyond it on the far side
    bool turnedFor(double enc0Deg, double* out, QString* why = nullptr) const;
    double turnedNear(double enc0Deg, double nearTurned) const;
    bool canMove(int direction, double margin = 0) const { return direction == 0 || m_turned * (direction > 0 ? 1 : -1) < m_limit - margin; }
private:
    void save() const;
    QString m_file; double m_limit; std::optional<double> m_neutral, m_last; double m_turned = 0;
};

// ---- the sky ------------------------------------------------------------------------------------------------------------------------------------

/// Saemundsson refraction for a true altitude, degrees (pressure hPa, temperature C), as OriginSimulator refracts its stars; its exact inverse for an apparent one.
double refractionTrueToApparent(double trueAlt, double pressureHpa = 1010, double tempC = 10);
double refractionApparentToTrue(double apparentAlt, double pressureHpa = 1010, double tempC = 10);

/// J2000 -> horizon at the site (precessed to the date); with refract, the apparent altitude (where the tube points to see it). Without, the firmware's frame.
Astro::AltAz toHorizon(double raDeg, double decDeg, const QDateTime& utc, const Astro::Site& site, bool refract, double pressureHpa = 1010, double tempC = 10);
/// horizon -> J2000; refract: the altitude given is apparent
void toJ2000(double alt, double az, const QDateTime& utc, const Astro::Site& site, bool refract, double* raDeg, double* decDeg, double pressureHpa = 1010, double tempC = 10);

double separationDeg(double alt1, double az1, double alt2, double az2);
double norm360(double d);
double wrap180(double d);   // IEEE remainder by 360

/// Points spread evenly over a band of sky: sin(altitude) in equal steps, azimuth turned by the golden ratio of the span (a Fibonacci grid)
QList<Astro::AltAz> goldenSpiral(int count, double minAlt, double maxAlt, double minAz = 0, double maxAz = 360);

// ---- the calibration state ----------------------------------------------------------------------------------------------------------------------

/**
 * The points and the model kept between commands: from three points the fitted model, below that the level-base one from the newest point. A point more
 * than RejectDeg from a fitted model is a bad solve or a mount that slipped: refused. A point within SamePlaceDeg of an older one replaces it. Near the last
 * point added, a local correction (what the model still misses there) applies in full within 5 degrees, fading to nothing at 15.
 */
struct Calibration {
    QList<Point> pts; Model model; bool hasModel = false;
    bool hasLocal = false; double localAlt = 0, localAz = 0, localDAlt = 0, localDAz = 0;
    static constexpr double RejectDeg = 3, SamePlaceDeg = 2, LocalFull = 5, LocalFade = 15;

    /// adds a point and refits; returns the point's error against the model before (arcmin), -1 if there was none, -2 if refused
    double refine(const Point& p);
    void refit();
    void clear() { pts.clear(); hasModel = false; hasLocal = false; }
    void aim(double alt, double az, double* enc0, double* enc1) const;   // the model plus the local correction
    QJsonObject toJson() const;
    static Calibration fromJson(const QJsonObject& o);
};

// ---- focus --------------------------------------------------------------------------------------------------------------------------------------

/**
 * A focus V-curve: HFR(x) = sqrt(a^2 + b^2 (x - c)^2), the hyperbola NINA fits. HFR^2 is then a parabola in x, fitted by weighted least squares in closed
 * form; c is the best position, a the HFR there. ok is false when the curve does not open upward, or its vertex lies outside the positions measured
 * (then extend the sweep toward edge: -1 lower, +1 higher).
 */
struct FocusFit { bool ok = false; double best = 0, minHfr = 0, slope = 0, rms = 0, r2 = 0; int edge = 0; QString why; };
FocusFit fitHyperbola(const QList<double>& pos, const QList<double>& hfr);

} // namespace HostAlign
