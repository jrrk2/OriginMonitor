#include "CoordinateUtils.hpp"
#include "CometObserver.hpp"
#include <QDebug>
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>
#include <cmath>

CometObserver::CometObserver(QObject *parent)
    : QObject(parent)
    , m_kernelsLoaded(false)
{
    // Default location (update with your coordinates)
    m_location.latitude = 52.0 + 11.0/60.0 + 24.0/3600.0;   // 52.19°
    m_location.longitude = 0.0 + 10.0/60.0 + 12.0/3600.0;   // 0.17°
    m_location.elevation = 20.0;
}

CometObserver::~CometObserver()
{
    if (m_kernelsLoaded) {
        // Unload SPICE kernels
        unload_c("ALL");
    }
}

bool CometObserver::loadKernels(const QString& spkFile, 
                                const QString& leapSecondFile,
                                const QString& planetaryEphemeris)
{
    // Check if files exist first
    if (!QFileInfo::exists(leapSecondFile)) {
        qDebug() << "ERROR: Leap second file not found:" << leapSecondFile;
        qDebug() << "Absolute path would be:" << QFileInfo(leapSecondFile).absoluteFilePath();
        return false;
    }
    
    if (!QFileInfo::exists(planetaryEphemeris)) {
        qDebug() << "ERROR: Planetary ephemeris not found:" << planetaryEphemeris;
        qDebug() << "Absolute path would be:" << QFileInfo(planetaryEphemeris).absoluteFilePath();
        qDebug() << "Download from: https://naif.jpl.nasa.gov/pub/naif/generic_kernels/spk/planets/";
        return false;
    }
    
    if (!QFileInfo::exists(spkFile)) {
        qDebug() << "ERROR: SPK file not found:" << spkFile;
        qDebug() << "Absolute path would be:" << QFileInfo(spkFile).absoluteFilePath();
        return false;
    }
    
    // Load leap second kernel (required for time conversions)
    qDebug() << "Loading leap seconds:" << leapSecondFile;
    furnsh_c(leapSecondFile.toStdString().c_str());
    
    if (failed_c()) {
        char msg[1841];
        getmsg_c("LONG", 1840, msg);
        qDebug() << "SPICE error loading TLS:" << msg;
        reset_c();
        return false;
    }
    
    // Load planetary ephemeris (for Earth position)
    qDebug() << "Loading planetary ephemeris:" << planetaryEphemeris;
    furnsh_c(planetaryEphemeris.toStdString().c_str());
    
    if (failed_c()) {
        char msg[1841];
        getmsg_c("LONG", 1840, msg);
        qDebug() << "SPICE error loading planetary ephemeris:" << msg;
        reset_c();
        return false;
    }
    
    // Load comet SPK file from Horizons
    qDebug() << "Loading comet SPK:" << spkFile;
    furnsh_c(spkFile.toStdString().c_str());
    
    if (failed_c()) {
        char msg[1841];
        getmsg_c("LONG", 1840, msg);
        qDebug() << "SPICE error loading SPK:" << msg;
        reset_c();
        return false;
    }
    
    m_kernelsLoaded = true;
    qDebug() << "SPICE kernels loaded successfully";
    
    // Print loaded kernels for verification
    SpiceInt count;
    ktotal_c("ALL", &count);
    qDebug() << "Total kernels loaded:" << count;
    
    return true;
}

void CometObserver::setLocation(const ObserverLocation& loc)
{
    m_location = loc;
}

double CometObserver::dateTimeToJD(const QDateTime& dt)
{
    QDateTime utc = dt.toUTC();
    
    ln_date lnDate;
    lnDate.years = utc.date().year();
    lnDate.months = utc.date().month();
    lnDate.days = utc.date().day();
    lnDate.hours = utc.time().hour();
    lnDate.minutes = utc.time().minute();
    lnDate.seconds = utc.time().second() + utc.time().msec() / 1000.0;
    
    return ln_get_julian_day(&lnDate);
}

double CometObserver::dateTimeToET(const QDateTime& dt)
{
    if (!m_kernelsLoaded) return 0.0;
    
    // Convert to UTC string in SPICE format
    QString utcStr = dt.toUTC().toString("yyyy-MM-dd HH:mm:ss.zzz");
    
    SpiceDouble et;
    str2et_c(utcStr.toStdString().c_str(), &et);
    
    return et;
}

CometObserver::RaDec CometObserver::getCometRaDec(const QString& cometName, double et)
{
    RaDec result = {0, 0, 0};
    
    if (!m_kernelsLoaded) {
        qDebug() << "Kernels not loaded";
        return result;
    }
    
    // Get state vector (position and velocity) of comet relative to Earth
    SpiceDouble state[6];
    SpiceDouble lt; // Light time
    
    // Target: comet, Observer: Earth (399), Reference frame: J2000
    spkezr_c(cometName.toStdString().c_str(), 
             et, 
             "J2000", 
             "LT+S",  // Light time + stellar aberration correction
             "EARTH", 
             state, 
             &lt);
    
    // Check for errors
    if (failed_c()) {
        char msg[1841];
        getmsg_c("SHORT", 1840, msg);
        qDebug() << "SPICE error:" << msg;
        reset_c();
        return result;
    }
    
    // Position vector (km)
    double x = state[0];
    double y = state[1];
    double z = state[2];
    
    // Convert to astronomical units
    const double KM_PER_AU = 149597870.7;
    double distance = sqrt(x*x + y*y + z*z) / KM_PER_AU;
    
    // Calculate RA and DEC in J2000
    double range, ra_rad_j2000, dec_rad_j2000;
    recrad_c(state, &range, &ra_rad_j2000, &dec_rad_j2000);
    //    qDebug() << "SPICE J2000:" << ra_rad_j2000 * 180/M_PI << dec_rad_j2000 * 180/M_PI;
    
    // Convert J2000 to JNow (apparent of-date) using libnova
    // Get Julian Date from ephemeris time
    SpiceDouble jd_tt;
    SpiceChar utc_str[256];
    et2utc_c(et, "ISOC", 6, 255, utc_str);
    
    ln_date date;
    // Parse UTC string (format: YYYY-MM-DDTHH:MM:SS.ssssss)
    sscanf(utc_str, "%d-%d-%dT%d:%d:%lf", 
           &date.years, &date.months, &date.days,
           &date.hours, &date.minutes, &date.seconds);
    double jd = ln_get_julian_day(&date);

    //    printf("JD: %.3f\n", jd);

    // Convert J2000 to apparent (of-date) coordinates using libnova
    ln_equ_posn j2000_pos;
    j2000_pos.ra = ra_rad_j2000 * 180.0 / M_PI;  // Convert to degrees
    j2000_pos.dec = dec_rad_j2000 * 180.0 / M_PI;
    
    // No proper motion for comets (proper motion is for stars)
    ln_equ_posn proper_motion;
    proper_motion.ra = 0.0;
    proper_motion.dec = 0.0;
    
    ln_equ_posn apparent_pos;
    ln_get_apparent_posn(&j2000_pos, &proper_motion, jd, &apparent_pos);
    //    qDebug() << "Apparent:" << apparent_pos.ra << apparent_pos.dec;
 
    // Convert back to hours and degrees
    result.ra = apparent_pos.ra;  // degrees to hours
    result.dec = apparent_pos.dec;
    result.distance = distance;
    
    return result;
}

CometObserver::AltAz CometObserver::raDecToAltAz(double ra, double dec, double jd)
{
    CometObserver::AltAz result;
    
    // Set up observer location
    double lst = CoordinateUtils::localSiderealTime(m_location.longitude, jd);
    //    qDebug() << "LST" << lst;
    
    // RA is in HOURS, DEC is in DEGREES (already in JNow from getCometRaDec)
    // Convert to radians for CoordinateUtils
    double ra_rad = ra * M_PI / 180.0;          // degrees → radians
    double dec_rad = dec * M_PI / 180.0;        // degrees → radians
    double lat_rad = m_location.latitude * M_PI / 180.0;
    double lon_rad = m_location.longitude * M_PI / 180.0;
    
    // NO precession here - coordinates are already apparent!
    auto [alt, az, ha] = CoordinateUtils::raDecToAltAz(
        ra_rad, dec_rad, lat_rad, lon_rad, lst);
    
    // Convert back to degrees
    result.alt = alt * 180.0 / M_PI;
    result.az = az * 180.0 / M_PI;
    //    qDebug() << "AZI/ALT" << result.az << result.alt;
    return result;
}

SkyPosition CometObserver::calculatePosition(const QString& cometName, 
                                             const QDateTime& dateTime)
{
    SkyPosition pos;
    
    // Get ephemeris time
    double et = dateTimeToET(dateTime);
    double jd = dateTimeToJD(dateTime);
    
    // Get RA/DEC from SPICE
    RaDec raDec = getCometRaDec(cometName, et);
    pos.ra = raDec.ra;
    pos.dec = raDec.dec;
    pos.distance = raDec.distance;
    
    // Convert to Alt/Az using libnova
    AltAz altAz = raDecToAltAz(raDec.ra, raDec.dec, jd);
    pos.alt = altAz.alt;
    pos.az = altAz.az;
    
    // Check if visible
    pos.isVisible = (pos.alt > 0.0);
    
    // Get Sun position
    ln_lnlat_posn observer;
    observer.lat = m_location.latitude;
    observer.lng = m_location.longitude;
    
    ln_equ_posn sunEqu;
    ln_get_solar_equ_coords(jd, &sunEqu);
    
    ln_hrz_posn sunHrz;
    ln_get_hrz_from_equ(&sunEqu, &observer, jd, &sunHrz);
    pos.sunAlt = sunHrz.alt;
    
    // Calculate solar elongation (angular separation)
    ln_equ_posn cometEqu;
    cometEqu.ra = pos.ra;
    cometEqu.dec = pos.dec;
    pos.solarElongation = ln_get_angular_separation(&sunEqu, &cometEqu);
    
    // Check if dark enough (astronomical twilight)
    pos.isDarkEnough = (pos.sunAlt < -18.0);
    
    // Truly observable = above horizon AND dark
    pos.isObservable = pos.isVisible && pos.isDarkEnough;
    
    return pos;
}

bool CometObserver::isObservable(const SkyPosition& pos, double minAltitude)
{
    return pos.alt >= minAltitude;
}

CometObserver::RiseSetTimes CometObserver::calculateRiseSet(
    const QString& cometName, 
    const QDate& date,
    double horizon)
{
    RiseSetTimes times;
    times.isCircumpolar = false;
    times.neverRises = false;
    
    // Get position at midnight to find approximate RA/DEC
    QDateTime midnight(date, QTime(0, 0), Qt::UTC);
    SkyPosition midnightPos = calculatePosition(cometName, midnight);
    
    // Set up observer and object for libnova
    ln_lnlat_posn observer;
    observer.lat = m_location.latitude;
    observer.lng = m_location.longitude;
    
    ln_equ_posn object;
    object.ra = midnightPos.ra;
    object.dec = midnightPos.dec;
    
    // Calculate rise/set times
    ln_rst_time rst;
    double jd = dateTimeToJD(midnight);
    
    int result = ln_get_object_rst(jd, &observer, &object, &rst);
    
    if (result == 0) {
        // Normal rise/set
        ln_date riseDate, transitDate, setDate;
        ln_get_date(rst.rise, &riseDate);
        ln_get_date(rst.transit, &transitDate);
        ln_get_date(rst.set, &setDate);
        
        times.rise = QDateTime(QDate(riseDate.years, riseDate.months, riseDate.days),
                               QTime(riseDate.hours, riseDate.minutes, 
                                     static_cast<int>(riseDate.seconds)), Qt::UTC);
        
        times.transit = QDateTime(QDate(transitDate.years, transitDate.months, transitDate.days),
                                  QTime(transitDate.hours, transitDate.minutes, 
                                        static_cast<int>(transitDate.seconds)), Qt::UTC);
        
        times.set = QDateTime(QDate(setDate.years, setDate.months, setDate.days),
                              QTime(setDate.hours, setDate.minutes, 
                                    static_cast<int>(setDate.seconds)), Qt::UTC);
        
    } else if (result == 1) {
        times.isCircumpolar = true;
        qDebug() << "Object is circumpolar (always above horizon)";
    } else {
        times.neverRises = true;
        qDebug() << "Object never rises above horizon";
    }
    
    return times;
}

CometObserver::BestObservingTime CometObserver::findBestObservingTime(
    const QString& cometName, 
    const QDate& date)
{
    BestObservingTime best;
    best.altitude = -90.0;
    best.isDark = false;
    
    qDebug() << "Searching for best observing time on" << date.toString(Qt::ISODate);
    
    // Check every hour throughout the night
    int darkHours = 0;
    for (int hour = 0; hour < 24; hour++) {
        QDateTime time(date, QTime(hour, 0), Qt::UTC);
        SkyPosition pos = calculatePosition(cometName, time);
        
        if (pos.isDarkEnough) {
            darkHours++;
            qDebug() << "  Dark at" << time.toString("HH:mm") 
                     << "- Sun:" << pos.sunAlt << "° Comet alt:" << pos.alt << "°";
        }
        
        // Only consider times when it's dark
        if (pos.isDarkEnough && pos.alt > best.altitude) {
            best.altitude = pos.alt;
            best.azimuth = pos.az;
            best.time = time;
            best.isDark = true;
        }
    }
    
    qDebug() << "Found" << darkHours << "dark hours";
    
    // If we found a dark time, refine to 15-minute intervals around that hour
    if (best.isDark) {
        int bestHour = best.time.time().hour();
        double refinedAlt = best.altitude;
        
        for (int offset = -60; offset <= 60; offset += 15) {
            QDateTime refined = best.time.addSecs(offset * 60);
            if (refined.date() != date) continue;
            
            SkyPosition pos = calculatePosition(cometName, refined);
            if (pos.isDarkEnough && pos.alt > refinedAlt) {
                refinedAlt = pos.alt;
                best.altitude = pos.alt;
                best.azimuth = pos.az;
                best.time = refined;
            }
        }
        qDebug() << "Refined best time:" << best.time.toString(Qt::ISODate);
    } else {
        qDebug() << "No dark periods found - checking if Sun ever sets...";
        // Check midnight position
        QDateTime midnight(date, QTime(23, 59), Qt::UTC);
        SkyPosition midnightPos = calculatePosition(cometName, midnight);
        qDebug() << "At 23:59 UTC: Sun altitude =" << midnightPos.sunAlt << "degrees";
    }
    
    return best;
}

QVector<CometObserver::EveningOpportunity> CometObserver::findEveningOpportunities(
    const QString& cometName,
    const QDate& startDate,
    int numDays,
    double minAltitude)
{
    QVector<EveningOpportunity> opportunities;
    
    qDebug() << "\nSearching" << numDays << "days for evening opportunities (>=" << minAltitude << "° altitude)...";
    
    for (int day = 0; day < numDays; day++) {
        QDate checkDate = startDate.addDays(day);
        EveningOpportunity opp;
        opp.date = checkDate;
        opp.altitude = -90.0;
        opp.meetsMinAltitude = false;
        
        // Check evening hours: 18:00 to 23:59 local time
        // Convert to UTC (roughly - UK is UTC+0 or UTC+1)
        for (int hour = 18; hour < 24; hour++) {
            QDateTime checkTime(checkDate, QTime(hour, 0), Qt::LocalTime);
            QDateTime utcTime = checkTime.toUTC();
            
            SkyPosition pos = calculatePosition(cometName, utcTime);
            
            // Only consider if it's dark enough and above horizon
            if (pos.isDarkEnough && pos.isVisible && pos.alt > opp.altitude) {
                opp.altitude = pos.alt;
                opp.azimuth = pos.az;
                opp.bestTime = utcTime;
                
                if (pos.alt >= minAltitude) {
                    opp.meetsMinAltitude = true;
                }
            }
        }
        
        opportunities.append(opp);
        
        if (opp.meetsMinAltitude) {
            qDebug() << "  ✓" << checkDate.toString(Qt::ISODate) 
                     << "- Best:" << opp.bestTime.toString("HH:mm") << "UTC"
                     << "Alt:" << opp.altitude << "°";
        }
    }
    
    return opportunities;
}

void CometObserver::generateEphemeris(const QString& cometName,
                                      const QDateTime& startTime,
                                      const QDateTime& endTime,
                                      int stepMinutes,
                                      const QString& outputFile)
{
    QVector<EphemerisEntry> entries;
    
    QDateTime current = startTime;
    while (current <= endTime) {
        SkyPosition pos = calculatePosition(cometName, current);
        
        EphemerisEntry entry;
        entry.datetime = current;
        entry.ra = pos.ra;
        entry.dec = pos.dec;
        entry.az = pos.az;
        entry.alt = pos.alt;
        entry.distance = pos.distance;
        entry.sunAlt = pos.sunAlt;
        
        // Determine solar presence code (like Horizons)
        if (pos.sunAlt > 0) {
            entry.solarPresence = '*';  // Daylight
        } else if (pos.sunAlt > -6) {
            entry.solarPresence = 'C';  // Civil twilight
        } else if (pos.sunAlt > -12) {
            entry.solarPresence = 'N';  // Nautical twilight
        } else if (pos.sunAlt > -18) {
            entry.solarPresence = 'A';  // Astronomical twilight
        } else {
            entry.solarPresence = ' ';  // Night
        }
        
        entries.append(entry);
        current = current.addSecs(stepMinutes * 60);
    }
    
    // Print to console (and optionally to file)
    QString output;
    QTextStream stream(&output);
    
    stream << "****************************************************************************************************\n";
    stream << QString(" Date__(UT)__HR:MN     R.A._(a-appar)_DEC.  Azi____(a-app)___Elev   Distance\n");
    stream << "****************************************************************************************************\n";
    stream << "$SOE\n";
    
    for (const auto& entry : entries) {
        stream << QString(" %1 %2%3  %4  %5  %6  %7  %8\n")
            .arg(entry.datetime.toString("yyyy-MMM-dd HH:mm"))
            .arg(entry.solarPresence)
            .arg('m')  // Moon marker - simplified, always show 'm' for now
            .arg(entry.ra, 10, 'f', 5)
            .arg(entry.dec, 10, 'f', 5)
            .arg(entry.az, 11, 'f', 6)
            .arg(entry.alt, 10, 'f', 6)
            .arg(entry.distance, 8, 'f', 6);
    }
    
    stream << "$EOE\n";
    stream << "****************************************************************************************************\n";
    
    // Print to console
    qDebug().noquote() << output;
    
    // Optionally write to file
    if (!outputFile.isEmpty()) {
        QFile file(outputFile);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream fileStream(&file);
            fileStream << output;
            file.close();
            qDebug() << "\nEphemeris written to:" << outputFile;
        } else {
            qDebug() << "Failed to write to file:" << outputFile;
        }
    }
}

QVector<CometObserver::ErrorData> CometObserver::compareWithReference(
    const QString& cometName,
    const QString& referenceFile)
{
    QVector<ErrorData> errors;
    
    QFile file(referenceFile);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "Cannot open reference file:" << referenceFile;
        return errors;
    }
    
    QTextStream in(&file);
    bool inData = false;
    
    while (!in.atEnd()) {
        QString line = in.readLine();
        
        if (line.contains("$$SOE")) {
            inData = true;
            continue;
        }
        if (line.contains("$$EOE")) {
            break;
        }
        
        if (!inData || line.trimmed().isEmpty()) continue;
        
        // Parse Horizons fixed-width format:
        // Columns (0-based character positions):
        // 0-10: Date "2025-Oct-05"
        // 12-16: Time "00:00"
        // 18: Solar presence marker
        // 19: Moon/event marker
        // 23-33: RA (degrees)
        // 35-45: DEC (degrees)
        // 47-58: Azimuth (degrees)
        // 60-70: Elevation (degrees)
        
        if (line.length() < 70) continue;
        
        QString dateStr = line.mid(1, 11).trimmed();  // Skip leading space
        QString timeStr = line.mid(12, 5).trimmed();
        QString raStr = line.mid(23, 11).trimmed();
        QString decStr = line.mid(35, 11).trimmed();
        QString azStr = line.mid(47, 12).trimmed();
        QString altStr = line.mid(60, 11).trimmed();
        
        // Parse date
        QStringList dateParts = dateStr.split("-");
        if (dateParts.size() != 3) continue;
        
        QStringList timeParts = timeStr.split(":");
        if (timeParts.size() != 2) continue;
        
        // Convert month name to number
        QMap<QString, int> months;
        months["Jan"] = 1; months["Feb"] = 2; months["Mar"] = 3;
        months["Apr"] = 4; months["May"] = 5; months["Jun"] = 6;
        months["Jul"] = 7; months["Aug"] = 8; months["Sep"] = 9;
        months["Oct"] = 10; months["Nov"] = 11; months["Dec"] = 12;
        
        int year = dateParts[0].toInt();
        int month = months[dateParts[1]];
        int day = dateParts[2].toInt();
        int hour = timeParts[0].toInt();
        int minute = timeParts[1].toInt();
        
        QDateTime refTime(QDate(year, month, day), QTime(hour, minute), Qt::UTC);
        
        // Parse reference values
        double refRA = raStr.toDouble();
        double refDEC = decStr.toDouble();
        double refAz = azStr.toDouble();
        double refAlt = altStr.toDouble();
        
        // Calculate our position
        SkyPosition calcPos = calculatePosition(cometName, refTime);
        
        // Calculate errors
        ErrorData error;
        error.time = refTime;
        error.raError = calcPos.ra - refRA;
        error.decError = calcPos.dec - refDEC;
        error.azError = calcPos.az - refAz;
        error.altError = calcPos.alt - refAlt;
        
        // Normalize azimuth error to [-180, 180]
        if (error.azError > 180.0) error.azError -= 360.0;
        if (error.azError < -180.0) error.azError += 360.0;
        
        errors.append(error);
    }
    
    file.close();
    return errors;
}

void CometObserver::listAvailableObjects()
{
    if (!m_kernelsLoaded) {
        qDebug() << "No kernels loaded";
        return;
    }
    
    qDebug() << "\n=== Objects in loaded SPK files ===";
    
    // Get list of loaded SPK files
    SpiceInt count;
    ktotal_c("SPK", &count);
    qDebug() << "Number of SPK files loaded:" << count;
    
    for (SpiceInt i = 0; i < count; i++) {
        SpiceChar file[256];
        SpiceChar type[32];
        SpiceChar source[256];
        SpiceInt handle;
        SpiceBoolean found;
        
        kdata_c(i, "SPK", 255, 31, 255, file, type, source, &handle, &found);
        
        if (found) {
            qDebug() << "\nSPK file:" << file;
            
            // Get coverage for this file
            SPICEINT_CELL(ids, 1000);
            spkobj_c(file, &ids);
            
            SpiceInt nobj = card_c(&ids);
            qDebug() << "Number of objects:" << nobj;
            
            for (SpiceInt j = 0; j < nobj; j++) {
                SpiceInt id = SPICE_CELL_ELEM_I(&ids, j);
                
                // Try to get the name for this ID
                SpiceChar name[256];
                SpiceBoolean nameFound;
                bodc2n_c(id, 255, name, &nameFound);
                
                if (nameFound) {
                    qDebug() << "  ID:" << id << "Name:" << name;
                } else {
                    qDebug() << "  ID:" << id << "(no name found)";
                }
            }
        }
    }
    
    qDebug() << "\n=== Try using one of these IDs or names ===\n";
}
