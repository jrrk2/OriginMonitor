#ifndef COMETOBSERVER_H
#define COMETOBSERVER_H

#include <QObject>
#include <QDateTime>
#include <libnova/libnova.h>

extern "C" {
    #include "SpiceUsr.h"  // CSPICE headers
}

// Observer location
struct ObserverLocation {
    double latitude;    // degrees (+ North)
    double longitude;   // degrees (+ East)
    double elevation;   // meters
};

// Sky position result
struct SkyPosition {
    double ra;          // Right Ascension (hours)
    double dec;         // Declination (degrees)
    double az;          // Azimuth (degrees, N=0, E=90)
    double alt;         // Altitude (degrees)
    double distance;    // Distance from Earth (AU)
    bool isVisible;     // Above horizon
    double sunAlt;      // Sun altitude (degrees)
    double solarElongation; // Angular separation from Sun (degrees)
    bool isDarkEnough;  // True if Sun < -18° (astronomical twilight)
    bool isObservable;  // Above horizon AND dark enough
};

class CometObserver : public QObject
{
    Q_OBJECT
    
public:
    explicit CometObserver(QObject *parent = nullptr);
    ~CometObserver();
    
    // Initialize SPICE kernels
    bool loadKernels(const QString& spkFile, 
                     const QString& leapSecondFile,
                     const QString& planetaryEphemeris);
    
    // Set observer location
    void setLocation(const ObserverLocation& loc);
    
    // Calculate comet position for given date/time
    SkyPosition calculatePosition(const QString& cometName, const QDateTime& dateTime);
    
    // Check if comet is observable (above minimum altitude)
    bool isObservable(const SkyPosition& pos, double minAltitude = 0.0);
    
    // Calculate rise, transit, and set times
    struct RiseSetTimes {
        QDateTime rise;
        QDateTime transit;
        QDateTime set;
        bool isCircumpolar;
        bool neverRises;
    };
    
    RiseSetTimes calculateRiseSet(const QString& cometName, 
                                  const QDate& date,
                                  double horizon = 0.0);
    
    // Find best observing time for tonight (highest altitude during darkness)
    struct BestObservingTime {
        QDateTime time;
        double altitude;
        double azimuth;
        bool isDark;
    };
    BestObservingTime findBestObservingTime(const QString& cometName, const QDate& date);
    
    // Find evening observing opportunities (18:00-midnight local time)
    struct EveningOpportunity {
        QDate date;
        QDateTime bestTime;
        double altitude;
        double azimuth;
        bool meetsMinAltitude;
    };
    QVector<EveningOpportunity> findEveningOpportunities(const QString& cometName, 
                                                          const QDate& startDate,
                                                          int numDays,
                                                          double minAltitude = 30.0);
    
    // Generate Horizons-style ephemeris
    struct EphemerisEntry {
        QDateTime datetime;
        double ra;          // degrees
        double dec;         // degrees
        double az;          // degrees
        double alt;         // degrees
        double distance;    // AU
        double sunAlt;      // degrees
        char solarPresence; // *, C, N, A, or ' '
    };
    
    void generateEphemeris(const QString& cometName,
                          const QDateTime& startTime,
                          const QDateTime& endTime,
                          int stepMinutes,
                          const QString& outputFile = "");
    
    // Generate error analysis comparing with reference data
    struct ErrorData {
        QDateTime time;
        double raError;      // degrees
        double decError;     // degrees
        double azError;      // degrees
        double altError;     // degrees
    };
    
    QVector<ErrorData> compareWithReference(const QString& cometName,
                                            const QString& referenceFile);
    
    // List all objects in loaded SPK files
    void listAvailableObjects();
    
private:
    ObserverLocation m_location;
    bool m_kernelsLoaded;
    
    // Convert QDateTime to Julian Date
    double dateTimeToJD(const QDateTime& dt);
    
    // Convert QDateTime to Ephemeris Time (for SPICE)
    double dateTimeToET(const QDateTime& dt);
    
    // Get comet RA/DEC using SPICE
    struct RaDec {
        double ra;      // hours
        double dec;     // degrees
        double distance; // AU
    };
    RaDec getCometRaDec(const QString& cometName, double et);
    
    // Convert RA/DEC to Alt/Az using libnova
    struct AltAz {
        double alt;     // degrees
        double az;      // degrees
    };
    AltAz raDecToAltAz(double ra, double dec, double jd);
};

#endif // COMETOBSERVER_H
