#ifndef COORDINATEUTILS_H
#define COORDINATEUTILS_H

#include <QString>
#include <tuple>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

// Common astronomical constants
const double DEG_TO_RAD = M_PI / 180.0;
const double RAD_TO_DEG = 180.0 / M_PI;

class CoordinateUtils
{
public:
    // Julian day calculation
    static double computeJulianDay(int year, int month, int day, 
                                   int hour, int minute, int second);
    
    // Sidereal time calculation
    static double localSiderealTime(double longitude, double jd);
    
    // Coordinate transformations
    static std::tuple<double, double, double> raDecToAltAz(
        double ra, double dec, double latitude, double longitude, double lst);
    
    static std::tuple<double, double, double> altAzToRaDec(
        double alt, double az, double latitude, double longitude, double lst);
    
    // Epoch conversions
    static std::tuple<double, double> j2000ToJNow(double ra2000, double dec2000);
    static std::tuple<double, double> jNowToJ2000(double raNow, double decNow);
    
    // High-level calculation functions
    static std::tuple<double, double, double, double, double, double, double> 
    calculateAltAz(int year, int month, int day, int hour, int minute, int second,
                   double ra, double dec, double latitude, double longitude);
    
    static std::tuple<double, double, double, double, double, double, double>
    calculateRaDec(int year, int month, int day, int hour, int minute, int second,
                   double alt, double az, double latitude, double longitude);

    static void blindAltAzToEquatorial(double alt, double az, double latitude, double lst,
				double& ra, double& dec, double& ha);
  
    // Atmospheric refraction correction
    static double correctRefraction(double apparentAlt);
    static double calculateAtmosphericRefraction(double apparent_alt_degrees);
    
    // Formatting and parsing functions
    static QString formatRaAsHMS(double ra);
    static QString formatDecAsDMS(double dec);
    static double parseRaFromHMS(int hours, int minutes, double seconds);
    static double parseDecFromDMS(int degrees, int minutes, double seconds);
    
    // Utility functions
    static double normalizeHours(double hours);
    static double normalizeDegrees(double degrees);
    static double centuriesSinceJ2000(double jd);
};

#endif // COORDINATEUTILS_H
