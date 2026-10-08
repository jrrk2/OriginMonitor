#include "AlignmentController.hpp"
#include "OriginBackend.hpp"
#include "TelescopeDataProcessor.hpp"
#include "CoordinateUtils.hpp"
#include <QDebug>
#include <cmath>
#include <algorithm>

static constexpr double DEG2RAD = M_PI / 180.0;
static constexpr double RAD2DEG = 180.0 / M_PI;

// ---------------------------------------------------------------------------
// Bright star catalog (J2000 coordinates, ~50 navigation stars)
// ---------------------------------------------------------------------------

QList<AlignmentStar> AlignmentController::brightStarCatalog()
{
    return {
        {"Sirius",       101.287,  -16.716, -1.46},
        {"Canopus",       95.988,  -52.696, -0.74},
        {"Arcturus",     213.915,   19.182, -0.05},
        {"Vega",         279.235,   38.784,  0.03},
        {"Capella",       79.172,   45.998,  0.08},
        {"Rigel",         78.634,   -8.202,  0.13},
        {"Procyon",      114.826,    5.225,  0.34},
        {"Betelgeuse",    88.793,    7.407,  0.42},
        {"Altair",       297.696,    8.868,  0.77},
        {"Aldebaran",     68.980,   16.509,  0.85},
        {"Antares",      247.352,  -26.432,  0.96},
        {"Spica",        201.298,  -11.161,  0.97},
        {"Pollux",       116.329,   28.026,  1.14},
        {"Fomalhaut",    344.413,  -29.622,  1.16},
        {"Deneb",        310.358,   45.280,  1.25},
        {"Regulus",      152.093,   11.967,  1.35},
        {"Castor",       113.650,   31.889,  1.58},
        {"Bellatrix",     81.283,    6.350,  1.64},
        {"Elnath",        81.573,   28.608,  1.65},
        {"Alnilam",       84.053,   -1.202,  1.69},
        {"Alioth",       193.507,   55.960,  1.77},
        {"Dubhe",        165.932,   61.751,  1.79},
        {"Mirfak",        51.081,   49.861,  1.80},
        {"Kaus Australis",276.043, -34.384,  1.85},
        {"Alkaid",       206.885,   49.313,  1.86},
        {"Sargas",       264.330,  -43.000,  1.87},
        {"Avior",        125.629,  -59.510,  1.86},
        {"Menkalinan",    89.882,   44.948,  1.90},
        {"Atria",        252.166,  -69.028,  1.92},
        {"Alhena",        99.428,   16.399,  1.93},
        {"Peacock",      306.412,  -56.735,  1.94},
        {"Mirzam",        95.675,  -17.956,  1.98},
        {"Alphard",      141.897,   -8.659,  1.98},
        {"Hamal",         31.793,   23.462,  2.00},
        {"Polaris",       37.954,   89.264,  2.02},
        {"Diphda",        10.897,  -17.987,  2.02},
        {"Miaplacidus",  138.300,  -69.717,  1.68},
        {"Nunki",        283.816,  -26.297,  2.05},
        {"Alpheratz",      2.097,   29.091,  2.06},
        {"Mirach",        17.433,   35.621,  2.05},
        {"Saiph",         86.939,   -9.670,  2.09},
        {"Rasalhague",   263.734,   12.560,  2.08},
        {"Algol",         47.042,   40.956,  2.12},
        {"Denebola",     177.265,   14.572,  2.13},
        {"Tiaki",        340.667,  -46.885,  2.14},
        {"Muhlifain",    190.379,  -48.960,  2.17},
        {"Wezen",        107.098,  -26.393,  1.84},
        {"Naos",         120.896,  -40.003,  2.25},
        {"Schedar",       10.127,   56.537,  2.23},
        {"Merak",        165.460,   56.382,  2.37},
    };
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

AlignmentController::AlignmentController(OriginBackend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_dataProcessor(backend->getDataProcessor())
{
    connect(m_backend, &OriginBackend::tiffImageDownloaded,
            this, &AlignmentController::onTiffImageDownloaded);
    connect(m_dataProcessor, &TelescopeDataProcessor::mountStatusUpdated,
            this, &AlignmentController::onMountStatusUpdated);

    connect(&m_plateSolver, &PlateSolver::solveCompleted,
            this, &AlignmentController::onPlateSolveCompleted);
    connect(&m_plateSolver, &PlateSolver::solveFailed,
            this, &AlignmentController::onPlateSolveFailed);
    connect(&m_plateSolver, &PlateSolver::logMessage,
            this, &AlignmentController::logMessage);

    connect(&m_mountModel, &MountModel::logMessage,
            this, &AlignmentController::logMessage);
    connect(&m_mountModel, &MountModel::modelUpdated,
            this, &AlignmentController::modelReady);

    m_settleTimer.setSingleShot(true);
    connect(&m_settleTimer, &QTimer::timeout, this, &AlignmentController::onSettleTimeout);

    m_timeoutTimer.setSingleShot(true);
    connect(&m_timeoutTimer, &QTimer::timeout, this, [this]() {
        log("Operation timed out");
        if (m_state == WaitingForImage) {
            // Image fetch may involve a slow network download (e.g. survey tile)
            // Retry once with the same timeout before giving up
            if (m_retryCount < 1) {
                m_retryCount++;
                takeAlignmentSnapshot();
            } else {
                log("Image acquisition failed after retries");
                setState(Error);
            }
        } else if (m_state == WaitingForCorrection) {
            setState(Error);
        }
    });

    m_correctionTimer.setSingleShot(true);
    connect(&m_correctionTimer, &QTimer::timeout, this, &AlignmentController::onCorrectionComplete);
}

// ---------------------------------------------------------------------------
// State management
// ---------------------------------------------------------------------------

void AlignmentController::setState(AlignState s)
{
    if (m_state == s) return;
    m_state = s;
    emit stateChanged(s);
}

QString AlignmentController::stateString() const
{
    switch (m_state) {
    case Idle:                 return "Idle";
    case Starting:             return "Starting";
    case InitialCapture:       return "Initial Capture";
    case InitialSolve:         return "Initial Solve";
    case SelectingStar:        return "Selecting Star";
    case SlewingToStar:        return "Slewing (relative)";
    case WaitingForCorrection: return "Slewing...";
    case SettlingAfterSlew:    return "Settling";
    case TakingSnapshot:       return "Taking Snapshot";
    case WaitingForImage:      return "Waiting for Image";
    case PlateSolving:         return "Plate Solving";
    case EvaluatingResult:     return "Evaluating";
    case SyncingMount:         return "Syncing Mount";
    case AdvancingToNext:      return "Advancing";
    case BuildingModel:        return "Building Model";
    case Complete:             return "Complete";
    case Error:                return "Error";
    }
    return "Unknown";
}

void AlignmentController::log(const QString &msg)
{
    emit logMessage(QString("[Align] %1").arg(msg));
}

// ---------------------------------------------------------------------------
// Coordinate helpers
// ---------------------------------------------------------------------------

void AlignmentController::raDecToAltAz(double raDeg, double decDeg,
                                         double &altDeg, double &azDeg) const
{
    QDateTime now = QDateTime::currentDateTimeUtc();
    double jd = CoordinateUtils::computeJulianDay(
        now.date().year(), now.date().month(), now.date().day(),
        now.time().hour(), now.time().minute(), now.time().second());

    // m_latitude/m_longitude are in radians (Origin protocol).
    // localSiderealTime expects longitude in degrees.
    double latDeg = m_latitude * RAD2DEG;
    double lonDeg = m_longitude * RAD2DEG;
    double lst = CoordinateUtils::localSiderealTime(lonDeg, jd);

    // raDecToAltAz expects all angles in radians
    auto [alt, az, ha] = CoordinateUtils::raDecToAltAz(
        raDeg * DEG2RAD, decDeg * DEG2RAD,
        latDeg * DEG2RAD, lonDeg * DEG2RAD, lst);

    altDeg = alt * RAD2DEG;
    azDeg = az * RAD2DEG;
}

double AlignmentController::angularSeparation(double ra1, double dec1,
                                               double ra2, double dec2) const
{
    double r1 = ra1 * DEG2RAD, d1 = dec1 * DEG2RAD;
    double r2 = ra2 * DEG2RAD, d2 = dec2 * DEG2RAD;
    double cosD = std::sin(d1)*std::sin(d2) + std::cos(d1)*std::cos(d2)*std::cos(r1 - r2);
    cosD = std::max(-1.0, std::min(1.0, cosD));
    return std::acos(cosD) * RAD2DEG;
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

void AlignmentController::startAlignment(int numStars)
{
    if (!m_backend->isConnected()) {
        log("Cannot start alignment - telescope not connected");
        return;
    }

    if (m_state != Idle && m_state != Complete && m_state != Error) {
        log("Alignment already in progress");
        return;
    }

    m_numStarsRequested = numStars;
    m_completedStars = 0;
    m_currentStarIndex = -1;
    m_retryCount = 0;
    m_correctionIteration = 0;
    m_oneShotMode = false;
    m_positionKnown = false;
    m_starStatuses.clear();
    m_selectedIndices.clear();
    m_mountModel.clear();

    setState(Starting);
    log(QString("Starting %1-star alignment (relative slew mode)").arg(numStars));

    // Get observer location from mount data
    const TelescopeData &data = m_dataProcessor->getData();
    m_latitude = data.mount.latitude;
    m_longitude = data.mount.longitude;
    m_hasLocation = (m_latitude != 0 || m_longitude != 0);

    if (!m_hasLocation) {
        log("Warning: no observer location from mount");
    }

    // Ensure the camera is connected and in manual mode for snapshots
    if (!m_backend->isCameraLogicallyConnected()) {
        m_backend->setCameraConnected(true);
    }
    m_backend->setCameraManualMode();

    // Step 1: take a snapshot to determine where we're currently pointing
    setState(InitialCapture);
    log("Taking initial snapshot to determine current position...");
    m_backend->takeSnapshot(5.0, 800, 1);
    setState(WaitingForImage);
    m_timeoutTimer.start(120000);
}

void AlignmentController::abort()
{
    m_settleTimer.stop();
    m_timeoutTimer.stop();
    m_correctionTimer.stop();
    m_plateSolver.abort();

    // Stop any slew in progress
    m_backend->stopAll();

    if (m_state != Idle) {
        log("Alignment aborted");
        setState(Idle);
    }
}

void AlignmentController::oneShotSolve()
{
    if (!m_backend->isConnected()) {
        log("Cannot solve - telescope not connected");
        return;
    }

    if (m_plateSolver.isSolving()) {
        log("Plate solve already in progress");
        return;
    }

    // Ensure the camera is connected and in manual mode
    if (!m_backend->isCameraLogicallyConnected()) {
        m_backend->setCameraConnected(true);
    }
    m_backend->setCameraManualMode();

    m_oneShotMode = true;
    log("One-shot solve: taking snapshot");

    m_backend->takeSnapshot(5.0, 800, 1);
    m_timeoutTimer.start(120000);
    setState(WaitingForImage);
}

// ---------------------------------------------------------------------------
// Star selection
// ---------------------------------------------------------------------------

void AlignmentController::buildVisibleStarList()
{
    m_visibleStars.clear();
    QList<AlignmentStar> catalog = brightStarCatalog();

    for (const AlignmentStar &star : catalog) {
        double altDeg, azDeg;
        raDecToAltAz(star.raDeg, star.decDeg, altDeg, azDeg);

        if (altDeg > 20.0 && altDeg < 85.0) {
            m_visibleStars.append(star);
        }
    }

    // Sort by magnitude (brightest first)
    std::sort(m_visibleStars.begin(), m_visibleStars.end(),
              [](const AlignmentStar &a, const AlignmentStar &b) {
                  return a.magnitude < b.magnitude;
              });

    log(QString("%1 stars visible above 20 deg altitude").arg(m_visibleStars.size()));
}

void AlignmentController::selectNextStar()
{
    if (m_completedStars >= m_numStarsRequested) {
        setState(BuildingModel);
        buildFinalModel();
        return;
    }

    // All navigation uses relative slew, so always prefer nearby stars
    // to minimize slew time. First star: pick nearest to current position.
    // Subsequent stars: balance proximity with sky coverage.

    double bestScore = -1e9;
    int bestIdx = -1;

    for (int i = 0; i < m_visibleStars.size(); i++) {
        if (m_selectedIndices.contains(i)) continue;

        double score;

        if (m_completedStars == 0 && m_positionKnown) {
            // First star: pick nearest to current position
            double sep = angularSeparation(
                m_visibleStars[i].raDeg, m_visibleStars[i].decDeg,
                m_currentRaDeg, m_currentDecDeg);
            score = -sep;
            score += (3.0 - m_visibleStars[i].magnitude) * 2.0;
        } else {
            // Subsequent stars: maximize sky coverage but penalise long slews
            double minSep = 180.0;
            for (int j : m_selectedIndices) {
                double sep = angularSeparation(
                    m_visibleStars[i].raDeg, m_visibleStars[i].decDeg,
                    m_visibleStars[j].raDeg, m_visibleStars[j].decDeg);
                minSep = std::min(minSep, sep);
            }
            score = minSep;
            // Penalise distance from current position (long relative slews)
            if (m_positionKnown) {
                double dist = angularSeparation(
                    m_visibleStars[i].raDeg, m_visibleStars[i].decDeg,
                    m_currentRaDeg, m_currentDecDeg);
                score -= dist * 0.3;
            }
            score += (3.0 - m_visibleStars[i].magnitude) * 0.1;
        }

        if (score > bestScore) {
            bestScore = score;
            bestIdx = i;
        }
    }

    if (bestIdx < 0) {
        log("No more suitable alignment stars");
        setState(BuildingModel);
        buildFinalModel();
        return;
    }

    m_selectedIndices.append(bestIdx);
    m_currentStarIndex = bestIdx;
    m_correctionIteration = 0;

    StarStatus ss;
    ss.star = m_visibleStars[bestIdx];
    ss.status = StarStatus::Slewing;
    m_starStatuses.append(ss);
    emit starStatusesUpdated();

    const AlignmentStar &star = m_visibleStars[bestIdx];
    double sep = m_positionKnown ?
        angularSeparation(star.raDeg, star.decDeg, m_currentRaDeg, m_currentDecDeg) : -1;

    log(QString("Selected star #%1: %2 (mag %3, %4 deg away)")
        .arg(m_completedStars + 1)
        .arg(star.name)
        .arg(star.magnitude, 0, 'f', 2)
        .arg(sep, 0, 'f', 1));

    emit alignmentProgress(m_completedStars, m_numStarsRequested, star.name);

    navigateToStar();
}

// ---------------------------------------------------------------------------
// Navigation: compute offset and apply relative slew
// ---------------------------------------------------------------------------

void AlignmentController::navigateToStar()
{
    const AlignmentStar &target = m_visibleStars[m_currentStarIndex];

    // Always use relative slew based on plate-solved position
    // (SyncToRaDec / GotoRaDec / MoveAxis are not available on Origin)
    if (!m_positionKnown) {
        log("Error: position unknown, cannot navigate");
        setState(Error);
        return;
    }

    // Compute alt/az of current position and target
    double curAlt, curAz, tgtAlt, tgtAz;
    raDecToAltAz(m_currentRaDeg, m_currentDecDeg, curAlt, curAz);
    raDecToAltAz(target.raDeg, target.decDeg, tgtAlt, tgtAz);

    double dAlt = tgtAlt - curAlt;  // degrees
    double dAz = tgtAz - curAz;    // degrees

    // Normalize azimuth difference to [-180, +180]
    if (dAz > 180.0) dAz -= 360.0;
    if (dAz < -180.0) dAz += 360.0;

    double totalSep = std::sqrt(dAlt * dAlt + dAz * dAz);

    log(QString("Relative slew: dAlt=%1 deg, dAz=%2 deg (total %3 deg)")
        .arg(dAlt, 0, 'f', 2).arg(dAz, 0, 'f', 2).arg(totalSep, 0, 'f', 2));

    setState(SlewingToStar);

    log(QString("GotoAltAzm: target Alt=%1 Az=%2 deg")
        .arg(tgtAlt, 0, 'f', 3).arg(tgtAz, 0, 'f', 3));

    // Use absolute slew to the target position
    m_backend->gotoAltAzm(tgtAlt * M_PI / 180.0, tgtAz * M_PI / 180.0);

    // Wait for the goto to complete, then settle.
    // Estimate a generous timeout based on separation.
    int timeoutMs = static_cast<int>(std::max(totalSep / 2.0, 5.0) * 1000);
    setState(WaitingForCorrection);
    m_correctionTimer.start(timeoutMs);
}

void AlignmentController::onCorrectionComplete()
{
    // GotoAltAzm stops automatically, but ensure a clean stop
    m_backend->stopAll();

    log("Goto complete, settling...");
    setState(SettlingAfterSlew);
    m_settleTimer.start(2000);  // 2 seconds settle time
}

void AlignmentController::onSettleTimeout()
{
    takeAlignmentSnapshot();
}

void AlignmentController::onMountStatusUpdated()
{
    // All navigation uses relative slew with timers; no GotoRaDec to monitor
}

// ---------------------------------------------------------------------------
// Snapshot and plate solving
// ---------------------------------------------------------------------------

void AlignmentController::takeAlignmentSnapshot()
{
    setState(TakingSnapshot);
    log("Taking snapshot");
    m_backend->takeSnapshot(5.0, 800, 1);
    setState(WaitingForImage);
    m_timeoutTimer.start(120000);  // 120s — survey tile downloads can be slow
}

void AlignmentController::onTiffImageDownloaded(const QString &filePath,
                                                 const QByteArray &imageData,
                                                 double ra, double dec,
                                                 double exposure)
{
    Q_UNUSED(filePath);
    Q_UNUSED(exposure);

    if (m_state != WaitingForImage) return;

    m_timeoutTimer.stop();

    // Store image metadata (Origin reports RA/Dec in radians)
    m_lastImageRaDeg = ra * RAD2DEG;
    m_lastImageDecDeg = dec * RAD2DEG;

    setState(PlateSolving);

    // Update star status
    if (!m_oneShotMode && m_currentStarIndex >= 0) {
        int statusIdx = m_starStatuses.size() - 1;
        if (statusIdx >= 0)
            m_starStatuses[statusIdx].status = StarStatus::Solving;
        emit starStatusesUpdated();
    }

    // When we don't yet know our position, use the inclinometer to
    // constrain the plate solver's declination search range.
    if (!m_positionKnown && m_hasLocation) {
        const TelescopeData &td = m_dataProcessor->getData();
        double incAlt = td.orientation.altitude;  // degrees from inclinometer
        // m_latitude/m_longitude are in radians (Origin protocol), convert to degrees
        double latDeg = m_latitude * RAD2DEG;
        double lonDeg = m_longitude * RAD2DEG;
        if (incAlt > 0 && incAlt < 90) {
            log(QString("Using inclinometer hint: alt=%1° lat=%2° lon=%3°")
                .arg(incAlt, 0, 'f', 1).arg(latDeg, 0, 'f', 1).arg(lonDeg, 0, 'f', 1));
            m_plateSolver.setInclinometerHint(incAlt, latDeg, lonDeg);
        }
    }

    m_plateSolver.solveImage(imageData, 0, 0);
}

void AlignmentController::onPlateSolveCompleted(const PlateSolveResult &result)
{
    // Always update our known position
    m_currentRaDeg = result.ra;
    m_currentDecDeg = result.dec;
    m_positionKnown = true;

    emit plateSolveCompleted(result);

    // One-shot mode
    if (m_oneShotMode) {
        log(QString("One-shot solve: RA=%1 Dec=%2 (%3 stars)")
            .arg(CoordinateUtils::formatRaAsHMS(result.ra))
            .arg(CoordinateUtils::formatDecAsDMS(result.dec))
            .arg(result.numStarsDetected));
        m_oneShotMode = false;
        setState(Idle);
        return;
    }

    // Initial solve: now we know where we are, select first star
    if (m_state == PlateSolving && m_currentStarIndex < 0) {
        log(QString("Initial position: RA=%1 Dec=%2")
            .arg(CoordinateUtils::formatRaAsHMS(result.ra))
            .arg(CoordinateUtils::formatDecAsDMS(result.dec)));
        buildVisibleStarList();

        if (m_visibleStars.size() < 3) {
            log("Error: fewer than 3 visible stars");
            setState(Error);
            return;
        }
        m_numStarsRequested = std::min(m_numStarsRequested, static_cast<int>(m_visibleStars.size()));

        setState(SelectingStar);
        selectNextStar();
        return;
    }

    // Regular alignment solve: evaluate how close we are to target
    evaluateSolveResult(result);
}

void AlignmentController::onPlateSolveFailed(const QString &reason)
{
    if (m_oneShotMode) {
        log(QString("One-shot solve failed: %1").arg(reason));
        m_oneShotMode = false;
        setState(Idle);
        return;
    }

    log(QString("Plate solve failed: %1").arg(reason));

    // Retry a couple of times
    if (m_retryCount < 2) {
        m_retryCount++;
        log(QString("Retrying (%1/2)...").arg(m_retryCount));
        takeAlignmentSnapshot();
    } else {
        // Skip this star
        int statusIdx = m_starStatuses.size() - 1;
        if (statusIdx >= 0)
            m_starStatuses[statusIdx].status = StarStatus::Failed;
        emit starStatusesUpdated();
        m_retryCount = 0;
        advanceToNextStar();
    }
}

// ---------------------------------------------------------------------------
// Evaluate: are we close enough to the target star?
// ---------------------------------------------------------------------------

void AlignmentController::evaluateSolveResult(const PlateSolveResult &result)
{
    setState(EvaluatingResult);

    const AlignmentStar &target = m_visibleStars[m_currentStarIndex];
    double errorDeg = angularSeparation(result.ra, result.dec,
                                         target.raDeg, target.decDeg);
    double errorArcsec = errorDeg * 3600.0;

    log(QString("Position error: %1\" (%2 deg) from %3 [iteration %4/%5]")
        .arg(errorArcsec, 0, 'f', 1)
        .arg(errorDeg, 0, 'f', 3)
        .arg(target.name)
        .arg(m_correctionIteration + 1)
        .arg(MAX_CORRECTION_ITERATIONS));

    m_correctionIteration++;

    if (errorArcsec < CORRECTION_THRESHOLD_ARCSEC) {
        // Close enough -- sync and record
        log(QString("Within threshold (%1\"), syncing mount").arg(errorArcsec, 0, 'f', 1));
        syncMountToSolution(result);
    } else if (m_correctionIteration >= MAX_CORRECTION_ITERATIONS) {
        // Ran out of iterations -- sync with what we have
        log(QString("Max iterations reached (error=%1\"), syncing anyway").arg(errorArcsec, 0, 'f', 1));
        syncMountToSolution(result);
    } else {
        // Need another correction slew
        log(QString("Applying correction slew iteration %1").arg(m_correctionIteration));
        navigateToStar();  // re-computes offset from current solved position to target
    }
}

// ---------------------------------------------------------------------------
// Sync mount and record observation
// ---------------------------------------------------------------------------

void AlignmentController::syncMountToSolution(const PlateSolveResult &result)
{
    setState(SyncingMount);

    const AlignmentStar &target = m_visibleStars[m_currentStarIndex];
    double errorArcsec = angularSeparation(result.ra, result.dec,
                                            target.raDeg, target.decDeg) * 3600.0;

    // Update star status
    int statusIdx = m_starStatuses.size() - 1;
    if (statusIdx >= 0) {
        m_starStatuses[statusIdx].status = StarStatus::Solved;
        m_starStatuses[statusIdx].errorArcsec = errorArcsec;
        m_starStatuses[statusIdx].solvedRaDeg = result.ra;
        m_starStatuses[statusIdx].solvedDecDeg = result.dec;
    }
    emit starStatusesUpdated();

    log(QString("Solved %1: RA=%2 Dec=%3 (error=%4\")")
        .arg(target.name)
        .arg(result.ra, 0, 'f', 5)
        .arg(result.dec, 0, 'f', 5)
        .arg(errorArcsec, 0, 'f', 1));

    // Record observation for mount model
    double altDeg, azDeg;
    raDecToAltAz(target.raDeg, target.decDeg, altDeg, azDeg);

    PointingObservation obs;
    obs.timestamp = QDateTime::currentDateTimeUtc();
    obs.commandedRaDeg = target.raDeg;
    obs.commandedDecDeg = target.decDeg;
    obs.actualRaDeg = result.ra;
    obs.actualDecDeg = result.dec;
    obs.altDeg = altDeg;
    obs.azDeg = azDeg;
    obs.temperature = m_dataProcessor->getData().environment.ambientTemperature;

    m_mountModel.addObservation(obs);

    m_completedStars++;
    m_retryCount = 0;
    m_correctionIteration = 0;

    advanceToNextStar();
}

void AlignmentController::advanceToNextStar()
{
    setState(AdvancingToNext);

    if (m_completedStars >= m_numStarsRequested) {
        buildFinalModel();
    } else {
        setState(SelectingStar);
        selectNextStar();
    }
}

void AlignmentController::buildFinalModel()
{
    setState(BuildingModel);
    log(QString("Building mount model from %1 observations").arg(m_mountModel.observationCount()));

    if (m_mountModel.observationCount() >= 4) {
        m_mountModel.fitModel();
    }
    if (m_mountModel.observationCount() >= 3) {
        m_mountModel.computeLevelingFromObservations();
    }

    ModelCoefficients coeffs = m_mountModel.coefficients();
    Quaternion q = m_mountModel.levelingQuaternion();

    log(QString("Alignment complete: %1 stars, RMS=%2\", tilt=%3 deg")
        .arg(m_completedStars)
        .arg(coeffs.rmsTotal, 0, 'f', 1)
        .arg(q.angle() * RAD2DEG, 0, 'f', 3));

    setState(Complete);
}
