#include "CoordinateUtils.hpp"
#include <QDateTime>
#include <cmath>
#include <algorithm>

// Additional constants for JavaScript-style implementation
const double ARCSEC_TO_RAD = DEG_TO_RAD / 3600.0;
const double ARCMIN_TO_RAD = DEG_TO_RAD / 60.0;
const double SEC_TO_RAD = DEG_TO_RAD / 240.0;  // 15 arcsec per second of time
const double TENTHS_ARCSEC_TO_RAD = ARCSEC_TO_RAD / 10.0;

// JavaScript-style coordinate utilities implementation
// This provides an alternative implementation following the MLB.coordLib patterns
class CoordinateUtilsJSStyle {
public:
    // Angle normalization utilities (from JavaScript validRev, validHalfRev)
    static double validRev(double rad) {
        double validatedRad = fmod(rad, 2 * M_PI);
        if (validatedRad < 0) {
            validatedRad += 2 * M_PI;
        }
        return validatedRad;
    }

    static double validHalfRev(double rad) {
        double validatedRad = validRev(rad);
        if (validatedRad > M_PI) {
            validatedRad -= 2 * M_PI;
        }
        return validatedRad;
    }

    // Declination validation (from JavaScript validDec)
    static double validDec(double dec) {
        double halfRevDec = validHalfRev(dec);
        
        if (halfRevDec > M_PI_2) {
            // > 90 degrees: reverse scale
            return M_PI - halfRevDec;
        } else if (halfRevDec >= -M_PI_2) {
            // between -90 and 90 degrees: don't change
            return halfRevDec;
        } else {
            // < -90 degrees: reverse negative scale
            return -M_PI - halfRevDec;
        }
    }

    // Julian Day calculation following JavaScript calcJD
    static double calcJD(int year, int month, int day, int hours, int minutes, int seconds, int milliseconds) {
        int a, b;
        double JD, fracJD;
        
        if (month < 3) {
            year--;
            month += 12;
        }
        
        a = year / 100;
        b = 2 - a + (a / 4);
        JD = floor(365.25 * (year + 4716)) + floor(30.6001 * (month + 1)) + day + b - 1524.5;
        
        fracJD = (hours + minutes / 60.0 + seconds / 3600.0 + milliseconds / 3600000.0) / 24.0;
        
        return JD + fracJD;
    }

    // Sidereal time calculation following JavaScript calcSidTFromJD
    static double calcSidTFromJD(double JD, double longitudeDeg) {
        double daysSinceJD2000 = JD - 2451545.0;  // J2000 epoch
        double GMSTHrs = fmod(18.697374558 + 24.06570982441908 * daysSinceJD2000, 24.0);
        return validRev(GMSTHrs * M_PI / 12.0 + longitudeDeg * DEG_TO_RAD);
    }

    // Rigorous precession calculation following JavaScript calcPrecessionRigorous
    static void calcPrecessionRigorous(double RA, double Dec, double startJulianYear, double deltaJulianYear,
                                     double& deltaRA, double& deltaDec) {
        // Convert to centuries
        double t1 = (startJulianYear - 2000.0) / 100.0;
        double t2 = deltaJulianYear / 100.0;
        double t1Sqr = t1 * t1;
        double t2Sqr = t2 * t2;
        double t2Cube = t2Sqr * t2;
        
        // Calculate precession angles in arcseconds
        double eta, zeta, theta;
        
        if (fabs(t1) < 1e-10) {
            eta = (2306.2181 * t2 + 0.30188 * t2Sqr + 0.017998 * t2Cube) * ARCSEC_TO_RAD;
            zeta = (2306.2181 * t2 + 1.09468 * t2Sqr + 0.018203 * t2Cube) * ARCSEC_TO_RAD;
            theta = (2004.3109 * t2 - 0.42665 * t2Sqr + 0.041883 * t2Cube) * ARCSEC_TO_RAD;
        } else {
            eta = ((2306.2181 + 1.39656 * t1 - 0.000139 * t1Sqr) * t2 + 
                   (0.30188 - 0.000344 * t1) * t2Sqr + 0.017998 * t2Cube) * ARCSEC_TO_RAD;
            zeta = ((2306.2181 + 1.39656 * t1 - 0.000139 * t1Sqr) * t2 + 
                    (1.09468 + 0.000066 * t1) * t2Sqr + 0.018203 * t2Cube) * ARCSEC_TO_RAD;
            theta = ((2004.3109 - 0.8533 * t1 - 0.000217 * t1Sqr) * t2 - 
                     (0.42665 + 0.000217 * t1) * t2Sqr + 0.041883 * t2Cube) * ARCSEC_TO_RAD;
        }
        
        double sinTheta = sin(theta);
        double cosTheta = cos(theta);
        
        // Handle flipped declination
        double unflippedDec = Dec;
        double unflippedRa = RA;
        bool isFlipped = (Dec > M_PI_2 || Dec < -M_PI_2);
        
        if (isFlipped) {
            unflippedDec = M_PI - Dec;
            unflippedRa = validRev(RA + M_PI);
        }
        
        double sinDec = sin(unflippedDec);
        double cosDec = cos(unflippedDec);
        double RaPlusEta = RA + eta;
        double cosRaPlusEta = cos(RaPlusEta);
        
        double a = cosDec * sin(RaPlusEta);
        double b = cosTheta * cosDec * cosRaPlusEta - sinTheta * sinDec;
        double c = sinTheta * cosDec * cosRaPlusEta + cosTheta * sinDec;
        
        // Calculate precessed coordinates
        double precessedRA = atan2(a, b) + zeta;
        deltaRA = validHalfRev(precessedRA - unflippedRa);
        
        double precessedDec;
        if (fabs(unflippedDec) > (M_PI_2 - ARCMIN_TO_RAD)) {
            // Near pole - use alternative formula
            double acosParm = sqrt(a * a + b * b);
            precessedDec = acos(acosParm);
        } else {
            precessedDec = asin(c);
        }
        
        deltaDec = validDec(precessedDec - unflippedDec);
    }

    // Coordinate transformation using trigonometry (from JavaScript getAltazTrig)
    static void getAltazTrig(double RA, double Dec, double SidT, double latitude, 
                           double& alt, double& az, double& HA) {
        double absLatitude = fabs(latitude);
        double adjustedDec = Dec;
        
        // Southern hemisphere adjustment
        if (latitude < 0) {
            adjustedDec = -adjustedDec;
        }
        
        // Calculate Hour Angle
        HA = SidT - RA;
        
        // Convert to altitude using spherical trigonometry
        double sinAlt = sin(adjustedDec) * sin(absLatitude) + 
                       cos(adjustedDec) * cos(absLatitude) * cos(HA);
        alt = asin(sinAlt);
        
        // Calculate azimuth
        double cosAz = (sin(adjustedDec) - sin(absLatitude) * sin(alt)) / 
                      (cos(absLatitude) * cos(alt));
        cosAz = std::max(-1.0, std::min(1.0, cosAz));  // Clamp to valid range
        
        az = acos(cosAz);
        
        // Determine azimuth quadrant
        if (sin(HA) > 0) {
            az = 2 * M_PI - az;
        }
        
        // Southern hemisphere azimuth adjustment
        if (latitude < 0) {
            az = 2 * M_PI - az;
        }
        
        az = validRev(az);
    }

    // Convert altitude/azimuth to RA/Dec (from JavaScript getEquatTrig)
    static void getEquatTrig(double alt, double az, double SidT, double latitude,
                           double& RA, double& Dec, double& HA) {
        double absLatitude = fabs(latitude);
        double adjustedAz = az;
        
        // Southern hemisphere adjustment
        if (latitude < 0) {
            adjustedAz = 2 * M_PI - adjustedAz;
        }
        
        // Calculate declination
        double sinDec = sin(alt) * sin(absLatitude) + 
                       cos(alt) * cos(absLatitude) * cos(adjustedAz);
        Dec = asin(sinDec);
        
        // Calculate hour angle
        double cosHA = (sin(alt) - sin(absLatitude) * sin(Dec)) / 
                      (cos(absLatitude) * cos(Dec));
        cosHA = std::max(-1.0, std::min(1.0, cosHA));  // Clamp to valid range
        
        HA = acos(cosHA);
        
        // Determine hour angle quadrant
        if (sin(adjustedAz) > 0) {
            HA = 2 * M_PI - HA;
        }
        
        // Calculate Right Ascension
        RA = validRev(SidT - HA);
        
        // Southern hemisphere declination adjustment
        if (latitude < 0) {
            Dec = -Dec;
        }
    }

    // Atmospheric refraction calculation (from JavaScript refraction tables)
    static double calcRefractionFromTrue(double trueElevationDeg) {
        // Refraction table: elevation in degrees, refraction in arcminutes
        static const double refractionTable[][2] = {
            {90, 0}, {60, 0.55}, {30, 1.7}, {20, 2.6}, {15, 3.5}, {10, 5.2},
            {8, 6.4}, {6, 8.3}, {4, 11.5}, {2, 18}, {0, 34.5}, {-1, 42.75}
        };
        
        const int tableSize = sizeof(refractionTable) / sizeof(refractionTable[0]);
        
        // Find interpolation points
        int ix = 0;
        while (ix < tableSize - 1 && trueElevationDeg <= refractionTable[ix][0]) {
            ix++;
        }
        
        if (ix == 0) ix = 1;
        
        // Linear interpolation
        double bp = refractionTable[ix - 1][0];
        double ep = refractionTable[ix][0];
        double br = refractionTable[ix - 1][1];
        double er = refractionTable[ix][1];
        
        double refractionArcmin = br + (trueElevationDeg - bp) * (er - br) / (ep - bp);
        
        return refractionArcmin * ARCMIN_TO_RAD;
    }

    // Main coordinate transformation engine
    static void transformCoordinates(double inputRA, double inputDec, double jd, 
                                   double latitude, double longitude,
                                   double& outputAlt, double& outputAz, double& outputHA) {
        // Calculate local sidereal time
        double lst = calcSidTFromJD(jd, longitude);
        
        // Apply precession from J2000 to current epoch
        double julianYear = 2000.0 + (jd - 2451545.0) / 365.25;
        double deltaRA, deltaDec;
        calcPrecessionRigorous(inputRA, inputDec, 2000.0, julianYear - 2000.0, deltaRA, deltaDec);
        
        double currentRA = inputRA + deltaRA;
        double currentDec = inputDec + deltaDec;
        
        // Convert to horizontal coordinates
        getAltazTrig(currentRA, currentDec, lst, latitude, outputAlt, outputAz, outputHA);
    }
};

// Wrapper functions to maintain the same interface
double CoordinateUtils::computeJulianDay(int year, int month, int day, 
                                           int hour, int minute, int second)
{
    return CoordinateUtilsJSStyle::calcJD(year, month, day, hour, minute, second, 0);
}

double CoordinateUtils::localSiderealTime(double longitude, double jd)
{
    return CoordinateUtilsJSStyle::calcSidTFromJD(jd, longitude);
}

std::tuple<double, double, double> CoordinateUtils::raDecToAltAz(
    double ra, double dec, double latitude, double longitude, double lst)
{
    double alt, az, ha;
    CoordinateUtilsJSStyle::getAltazTrig(ra, dec, lst, latitude, alt, az, ha);
    return std::make_tuple(alt, az, ha);
}


double CoordinateUtils::calculateAtmosphericRefraction(double apparent_alt_degrees) {
    // Bennett's refraction formula (1982) - more accurate than simple formulas
    // Returns refraction in degrees
    
    if (apparent_alt_degrees < -1.0) {
        return 0.0;  // Below horizon
    }
    
    double refraction_arcsec;
    
    if (apparent_alt_degrees > 15.0) {
        // For elevations above 15°
        double alt_rad = apparent_alt_degrees * M_PI / 180.0;
        double tan_alt = tan(alt_rad);
        refraction_arcsec = 58.1 / tan_alt - 0.07 / (tan_alt * tan_alt * tan_alt) + 
                           0.000086 / (tan_alt * tan_alt * tan_alt * tan_alt * tan_alt);
    } else {
        // For low elevations (more complex formula needed)
        double h = apparent_alt_degrees;
        refraction_arcsec = 1735.0 + h * (-518.2 + h * (103.4 + h * (-12.79 + h * 0.711)));
        refraction_arcsec = refraction_arcsec / 3600.0;  // Convert to degrees
        return refraction_arcsec;
    }
    
    return refraction_arcsec / 3600.0;  // Convert arcseconds to degrees
}

void CoordinateUtils::blindAltAzToEquatorial(double alt, double az, double latitude, double lst,
                                           double& ra, double& dec, double& ha) {
    // Apply atmospheric refraction correction
    // Assuming 'alt' is apparent altitude, convert to true altitude
    double true_alt = alt - calculateAtmosphericRefraction(alt);
    
    // Convert to radians
    double alt_rad = true_alt * M_PI / 180.0;
    double az_rad = az * M_PI / 180.0;  // Ensure azimuth convention is correct
    double lat_rad = latitude * M_PI / 180.0;
    
    // Calculate declination
    double sin_dec = sin(alt_rad) * sin(lat_rad) + 
                     cos(alt_rad) * cos(lat_rad) * cos(az_rad);
    sin_dec = std::max(-1.0, std::min(1.0, sin_dec));
    dec = asin(sin_dec);
    
    // Calculate hour angle
    double cos_ha = (sin(alt_rad) - sin(lat_rad) * sin(dec)) / 
                    (cos(lat_rad) * cos(dec));
    cos_ha = std::max(-1.0, std::min(1.0, cos_ha));
    double ha_rad = acos(cos_ha);
    
    // CORRECTED: Determine hour angle sign from azimuth
    // sin(az) > 0 means 0° < az < 180° (Eastern sky, rising) → Positive HA
    // sin(az) < 0 means 180° < az < 360° (Western sky, setting) → Negative HA
    if (sin(az_rad) < 0) {  // Western sky (setting)
        ha_rad = -ha_rad;
    }
    // Eastern sky keeps positive HA (no change needed)
    
    // Convert Hour Angle to hours
    ha = ha_rad * 12.0 / M_PI;
    
    // Calculate RA: RA = LST - HA
    double ra_hours = lst - ha;
    
    // Normalize RA to [0, 24) hours
    while (ra_hours < 0) ra_hours += 24.0;
    while (ra_hours >= 24.0) ra_hours -= 24.0;
    
    // Convert to degrees
    ra = ra_hours * 15.0;
    dec = dec * 180.0 / M_PI;
}

std::tuple<double, double, double> CoordinateUtils::altAzToRaDec(
    double alt, double az, double latitude, double longitude, double lst)
{
    // Apply refraction correction
    double trueAlt = alt - CoordinateUtilsJSStyle::calcRefractionFromTrue(alt * RAD_TO_DEG);
    
    double ra, dec, ha;
    //    CoordinateUtilsJSStyle::getEquatTrig(trueAlt, az, lst, latitude, ra, dec, ha);
    
    blindAltAzToEquatorial(trueAlt, az, latitude, lst, ra, dec, ha);
      
    // qDebug() << "blind eq" << ra << " " << dec << " " << ha;

    return std::make_tuple(ra, dec, ha);
}

std::tuple<double, double> CoordinateUtils::j2000ToJNow(double ra2000, double dec2000)
{
    QDateTime now = QDateTime::currentDateTimeUtc();
    double currentJD = computeJulianDay(now.date().year(), now.date().month(), now.date().day(),
                                       now.time().hour(), now.time().minute(), now.time().second());
    
    double julianYear = 2000.0 + (currentJD - 2451545.0) / 365.25;
    double deltaRA, deltaDec;
    CoordinateUtilsJSStyle::calcPrecessionRigorous(ra2000, dec2000, 2000.0, julianYear - 2000.0, 
                                                  deltaRA, deltaDec);
    
    double raNow = CoordinateUtilsJSStyle::validRev(ra2000 + deltaRA);
    double decNow = CoordinateUtilsJSStyle::validDec(dec2000 + deltaDec);
    
    return std::make_tuple(raNow, decNow);
}

std::tuple<double, double> CoordinateUtils::jNowToJ2000(double raNow, double decNow)
{
    QDateTime now = QDateTime::currentDateTimeUtc();
    double currentJD = computeJulianDay(now.date().year(), now.date().month(), now.date().day(),
                                       now.time().hour(), now.time().minute(), now.time().second());
    
    double julianYear = 2000.0 + (currentJD - 2451545.0) / 365.25;
    double deltaRA, deltaDec;
    CoordinateUtilsJSStyle::calcPrecessionRigorous(raNow, decNow, julianYear, 2000.0 - julianYear,
                                                  deltaRA, deltaDec);
    
    // qDebug() << "delta J2000" << deltaRA << " " << deltaDec;
    
    double ra2000 = CoordinateUtilsJSStyle::validRev(raNow + deltaRA);
    double dec2000 = CoordinateUtilsJSStyle::validDec(decNow + deltaDec);
    
    return std::make_tuple(ra2000, dec2000);
}

std::tuple<double, double, double, double, double, double, double> 
CoordinateUtils::calculateAltAz(
    int year, int month, int day, int hour, int minute, int second,
    double ra, double dec, double latitude, double longitude)
{
    double jd = computeJulianDay(year, month, day, hour, minute, second);
    double lst = localSiderealTime(longitude, jd);
    
    auto [raNow, decNow] = j2000ToJNow(ra, dec);
    auto [alt, az, ha] = raDecToAltAz(raNow, decNow, latitude, longitude, lst);
    
    return std::make_tuple(jd, raNow, decNow, alt, az, lst, ha);
}

std::tuple<double, double, double, double, double, double, double>
CoordinateUtils::calculateRaDec(
    int year, int month, int day, int hour, int minute, int second,
    double alt, double az, double latitude, double longitude)
{
    double jd = computeJulianDay(year, month, day, hour, minute, second);
    double lst = localSiderealTime(longitude, jd);
    
    auto [raNow, decNow, ha] = altAzToRaDec(alt, az, latitude, longitude, lst);
    auto [ra2000, dec2000] = jNowToJ2000(raNow, decNow);
    
    return std::make_tuple(jd, ra2000, dec2000, raNow, decNow, lst, ha);
}

double CoordinateUtils::correctRefraction(double apparentAlt)
{
    double apparentAltDeg = apparentAlt * RAD_TO_DEG;
    double refractionRad = CoordinateUtilsJSStyle::calcRefractionFromTrue(apparentAltDeg);
    return apparentAlt - refractionRad;
}

// Formatting functions remain the same as in the original implementation
QString CoordinateUtils::formatRaAsHMS(double ra)
{
    double raHours = ra / 15.0;
    int hours = static_cast<int>(floor(raHours));
    double remainingMinutes = (raHours - hours) * 60.0;
    int minutes = static_cast<int>(floor(remainingMinutes));
    double seconds = (remainingMinutes - minutes) * 60.0;
    
    return QString("%1h %2m %3s")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 4, 'f', 1, QChar('0'));
}

QString CoordinateUtils::formatDecAsDMS(double dec)
{
    QString sign = (dec < 0) ? "-" : "+";
    dec = fabs(dec);
    
    int degrees = static_cast<int>(floor(dec));
    double remainingMinutes = (dec - degrees) * 60.0;
    int minutes = static_cast<int>(floor(remainingMinutes));
    double seconds = (remainingMinutes - minutes) * 60.0;
    
    return QString("%1%2° %3' %4\"")
        .arg(sign)
        .arg(degrees, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 4, 'f', 1, QChar('0'));
}

double CoordinateUtils::parseRaFromHMS(int hours, int minutes, double seconds)
{
    double raHours = hours + minutes / 60.0 + seconds / 3600.0;
    return raHours * 15.0;
}

double CoordinateUtils::parseDecFromDMS(int degrees, int minutes, double seconds)
{
    double decDegrees = abs(degrees) + minutes / 60.0 + seconds / 3600.0;
    if (degrees < 0) {
        decDegrees = -decDegrees;
    }
    return decDegrees;
}

double CoordinateUtils::normalizeHours(double hours)
{
    hours = fmod(hours, 24.0);
    if (hours < 0) hours += 24.0;
    return hours;
}

double CoordinateUtils::normalizeDegrees(double degrees)
{
    degrees = fmod(degrees, 360.0);
    if (degrees < 0) degrees += 360.0;
    return degrees;
}

double CoordinateUtils::centuriesSinceJ2000(double jd)
{
    return (jd - 2451545.0) / 36525.0;
}
