#include "CoordinateUtils.hpp"
#include "CometObserver.hpp"
#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QTextStream>
#include <QVector>

struct HorizonsData {
    QDateTime time;
    double ra;      // degrees
    double dec;     // degrees
    double az;      // degrees
    double alt;     // degrees
    double distance; // AU
};

struct CalculatedData {
    QDateTime time;
    double ra;      // degrees
    double dec;     // degrees
    double az;      // degrees
    double alt;     // degrees
    double distance; // AU
};

struct testcase {
  double lat, lon, jul_day, ra_J2000, dec_J2000, ra_now, dec_now, sid_time, alt, az, hour_angle;
};

#define testPosition(planet, lat, lon, date, jul_day, ra_J2000, dec_J2000, ra_now, dec_now, az, alt, sid_time, light_time, tdb_ut, hour_angle) {lat, lon, jul_day, ra_J2000, dec_J2000, ra_now, dec_now, sid_time, alt, az, hour_angle},

struct testcase testcases[] = {
#include "testdata.h"
};

QVector<HorizonsData> readHorizonsData(const QString& filename)
{
    QVector<HorizonsData> data;
    
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "ERROR: Cannot open reference file:" << filename;
        return data;
    }
    
    QTextStream in(&file);
    bool inData = false;
    int lineNum = 0;
    
    qDebug() << "\n=== Reading Horizons Data ===";
    
    while (!in.atEnd()) {
        QString line = in.readLine();
        lineNum++;
        
        if (line.contains("$$SOE")) {
            inData = true;
            qDebug() << "Found data start at line" << lineNum;
            continue;
        }
        if (line.contains("$$EOE")) {
            qDebug() << "Found data end at line" << lineNum;
            break;
        }
        
        if (!inData || line.trimmed().isEmpty()) continue;
        
        // Parse Horizons fixed-width format WITH JD column:
        // Columns (0-based):
        //  1-11: Date "2025-Oct-05"
        // 12-16: Time "02:20"
        // 18-36: JD "2460953.597222222"
        // 37: Solar marker
        // 38: Moon marker
        // 40-50: RA (degrees)
        // 51-61: DEC (degrees)
        // 63-74: Azimuth (degrees)
        // 75-85: Elevation (degrees)
        // 87-95: Distance (AU)
        
        if (line.length() < 95) {
            qDebug() << "Line" << lineNum << "too short (" << line.length() << "chars), skipping";
            continue;
        }
        
        QString dateStr = line.mid(1, 11).trimmed();
        QString timeStr = line.mid(12, 6).trimmed();
        QString raStr = line.mid(40, 11).trimmed();
        QString decStr = line.mid(51, 11).trimmed();
        QString azStr = line.mid(62, 12).trimmed();
        QString altStr = line.mid(74, 11).trimmed();
        QString distStr = line.mid(87, 9).trimmed();

	//	qDebug() << "date:" << dateStr << "time:" << timeStr;
	
        // Parse date
        QStringList dateParts = dateStr.split("-");
        if (dateParts.size() != 3) {
            qDebug() << "Line" << lineNum << "bad date format:" << dateStr;
            continue;
        }
        
        QStringList timeParts = timeStr.split(":");
        if (timeParts.size() != 2) {
            qDebug() << "Line" << lineNum << "bad time format:" << timeStr;
            continue;
        }
        
        // Month name to number
        QMap<QString, int> months;
        months["Jan"] = 1; months["Feb"] = 2; months["Mar"] = 3;
        months["Apr"] = 4; months["May"] = 5; months["Jun"] = 6;
        months["Jul"] = 7; months["Aug"] = 8; months["Sep"] = 9;
        months["Oct"] = 10; months["Nov"] = 11; months["Dec"] = 12;
        
        int year = dateParts[0].toInt();
        int month = months.value(dateParts[1], 0);
        int day = dateParts[2].toInt();
        int hour = timeParts[0].toInt();
        int minute = timeParts[1].toInt();
        
        if (month == 0) {
            qDebug() << "Line" << lineNum << "unknown month:" << dateParts[1];
            continue;
        }
        
        HorizonsData entry;
        entry.time = QDateTime(QDate(year, month, day), QTime(hour, minute), Qt::UTC);
        entry.ra = raStr.toDouble();
        entry.dec = decStr.toDouble();
        entry.az = azStr.toDouble();
        entry.alt = altStr.toDouble();
        entry.distance = distStr.toDouble();
        
        data.append(entry);
    }
    
    file.close();
    qDebug() << "Read" << data.size() << "data points from Horizons file\n";
    
    return data;
}

void writeHorizonsDataCheck(const QVector<HorizonsData>& data, const QString& filename)
{
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qDebug() << "Cannot write check file:" << filename;
        return;
    }
    
    QTextStream out(&file);
    out << "# Horizons data readback check\n";
    out << "Time,RA_deg,DEC_deg,Az_deg,Alt_deg,Distance_AU\n";
    
    for (const auto& entry : data) {
        out << entry.time.toString("yyyy-MM-dd HH:mm") << ","
            << entry.ra << ","
            << entry.dec << ","
            << entry.az << ","
            << entry.alt << ","
            << entry.distance << "\n";
    }
    
    file.close();
    qDebug() << "Wrote readback check to:" << filename;
}

QVector<CalculatedData> generateCalculatedData(CometObserver& observer, 
                                               const QString& cometName,
                                               const QVector<HorizonsData>& horizonsData)
{
    QVector<CalculatedData> data;
    
    qDebug() << "\n=== Calculating positions for" << horizonsData.size() << "time points ===";
    
    for (const auto& horizons : horizonsData) {
      //        qDebug() << "time:" << horizons.time;
        SkyPosition pos = observer.calculatePosition(cometName, horizons.time);
        
        CalculatedData entry;
        entry.time = horizons.time;
        entry.ra = pos.ra;
        entry.dec = pos.dec;
        entry.az = pos.az;
        entry.alt = pos.alt;
        entry.distance = pos.distance;
        
        data.append(entry);
    }
    
    qDebug() << "Calculated" << data.size() << "positions\n";
    
    return data;
}

void compareAndAnalyze(const QVector<HorizonsData>& horizons,
                      const QVector<CalculatedData>& calculated,
                      const QString& csvFile)
{
    if (horizons.size() != calculated.size()) {
        qDebug() << "ERROR: Array size mismatch!" 
                 << horizons.size() << "vs" << calculated.size();
        return;
    }
    
    qDebug() << "=== Comparing" << horizons.size() << "data points ===\n";
    
    // Calculate statistics
    double raSum = 0, decSum = 0, azSum = 0, altSum = 0;
    double raMax = -999, decMax = -999, azMax = -999, altMax = -999;
    double raMin = 999, decMin = 999, azMin = 999, altMin = 999;
    
    QFile file(csvFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qDebug() << "Cannot write CSV file:" << csvFile;
        return;
    }
    
    QTextStream out(&file);
    out << "Time,Horizons_RA,Calc_RA,Horizons_DEC,Calc_DEC,Horizons_Az,Calc_Az,Horizons_Alt,Calc_Alt,";
    out << "RA_Error_deg,DEC_Error_deg,Az_Error_deg,Alt_Error_deg,RA_Error_arcmin,DEC_Error_arcmin\n";
    
    for (int i = 0; i < horizons.size(); i++) {
        const auto& h = horizons[i];
        const auto& c = calculated[i];
	/*
	qDebug() << "compare RA: " << c.ra << h.ra;
	qDebug() << "compare DEC: " << c.dec << h.dec;
	qDebug() << "compare AZ: " << c.az << h.az;
	qDebug() << "compare ALT: " << c.alt << h.alt;
	*/
        // Calculate errors
        double raError = c.ra - h.ra;
        double decError = c.dec - h.dec;
        double azError = c.az - h.az;
        double altError = c.alt - h.alt;
        
        // Normalize azimuth error to [-180, 180]
        while (azError > 180.0) azError -= 360.0;
        while (azError < -180.0) azError += 360.0;
	/*
	qDebug() << "raError: " << raError;
	qDebug() << "decError: " << decError;
	qDebug() << "azError: " << azError;
	qDebug() << "altError: " << altError;
	*/
        // Update statistics
        raSum += qAbs(raError);
        decSum += qAbs(decError);
        azSum += qAbs(azError);
        altSum += qAbs(altError);
        
        raMax = qMax(raMax, raError);
        raMin = qMin(raMin, raError);
        decMax = qMax(decMax, decError);
        decMin = qMin(decMin, decError);
        azMax = qMax(azMax, azError);
        azMin = qMin(azMin, azError);
        altMax = qMax(altMax, altError);
        altMin = qMin(altMin, altError);
        
        // Write to CSV
        out << h.time.toString("yyyy-MM-dd HH:mm") << ","
            << h.ra << "," << c.ra << ","
            << h.dec << "," << c.dec << ","
            << h.az << "," << c.az << ","
            << h.alt << "," << c.alt << ","
            << raError << ","
            << decError << ","
            << azError << ","
            << altError << ","
            << raError * 60.0 << ","
            << decError * 60.0 << "\n";
    }
    
    file.close();
    
    int n = horizons.size();
    
    qDebug() << "=== Error Statistics (degrees) ===\n";
    qDebug() << "RA Errors:";
    qDebug() << "  Mean absolute:" << raSum / n;
    qDebug() << "  Range:" << raMin << "to" << raMax;
    qDebug() << "  (in arcmin):" << raMin * 60 << "to" << raMax * 60;
    
    qDebug() << "\nDEC Errors:";
    qDebug() << "  Mean absolute:" << decSum / n;
    qDebug() << "  Range:" << decMin << "to" << decMax;
    qDebug() << "  (in arcmin):" << decMin * 60 << "to" << decMax * 60;
    
    qDebug() << "\nAzimuth Errors:";
    qDebug() << "  Mean absolute:" << azSum / n;
    qDebug() << "  Range:" << azMin << "to" << azMax;
    qDebug() << "  (in arcmin):" << azMin * 60 << "to" << azMax * 60;
    
    qDebug() << "\nAltitude Errors:";
    qDebug() << "  Mean absolute:" << altSum / n;
    qDebug() << "  Range:" << altMin << "to" << altMax;
    qDebug() << "  (in arcmin):" << altMin * 60 << "to" << altMax * 60;
    
    qDebug() << "\n=== CSV file written to:" << csvFile << "===";
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    
    // Configuration
    QString kernelDir = "/Users/jonathan/spice_kernels/";
    QString outputDir = kernelDir; // Write output to same directory
    QString horizonsFile = kernelDir + "horizons_reference.txt";
    QString cometName = "1004054";

    // Step 0: Validate AltAz conversion
    double err = 0.0;
    for (int i = 0; i < sizeof(testcases)/sizeof(struct testcase); i++)
      {
	double lat = testcases[i].lat;
	double lon = testcases[i].lon;
	double jd = testcases[i].jul_day;
	double ra = testcases[i].ra_now;
	double dec = testcases[i].dec_now;
	double sid_time = testcases[i].sid_time * 15.0 * M_PI / 180.0;
	double hour_angle = testcases[i].hour_angle;
	double test_alt = testcases[i].alt * M_PI / 180.0;
	double test_az = testcases[i].az * M_PI / 180.0;
	// Set up local siderial time for observer location
	double lat_rad = lat * M_PI / 180.0;
	double lon_rad = lon * M_PI / 180.0;
	double lst = CoordinateUtils::localSiderealTime(lon, jd);
	//        qDebug() << "JD/lst" << jd << lst << sid_time;
	// RA is in HOURS, DEC is in DEGREES (already in JNow from getCometRaDec)
	// Convert to radians for CoordinateUtils
	double ra_rad = ra * M_PI / 180.0;  // degrees → radians
	double dec_rad = dec * M_PI / 180.0;        // degrees → radians
	//	qDebug() << "RA" << ra_rad << "DEC" << dec_rad;

	// NO precession here - coordinates are already apparent!
	auto [alt, az, ha_rad] = CoordinateUtils::raDecToAltAz(
	    ra_rad, dec_rad, lat_rad, lon_rad, lst);
	// Convert HA to hours for comparison
	double ha_hours = ha_rad * 12.0 / M_PI;
	// Normalize to [-12, +12]
	while (ha_hours > 12.0) ha_hours -= 24.0;
	while (ha_hours < -12.0) ha_hours += 24.0;

	//	qDebug() << "ALT" << alt << test_alt;
	//	qDebug() << "AZ" << az << test_az;
	//	qDebug() << "HA" << ha_hours << hour_angle;
	err += fabs(alt - test_alt) + fabs(az - test_az);
      }

    qDebug() << "Average error: " << err/(sizeof(testcases)/sizeof(struct testcase));
    // Step 1: Read Horizons data
    QVector<HorizonsData> horizonsData = readHorizonsData(horizonsFile);
    
    if (horizonsData.isEmpty()) {
        qDebug() << "\nNo data read. Check that" << horizonsFile << "exists and is valid.";
        return 1;
    }
    
    // Step 2: Write it back out as a check
    writeHorizonsDataCheck(horizonsData, outputDir + "horizons_readback.csv");
    
    // Step 3: Set up observer and load kernels
    CometObserver observer;
    
    ObserverLocation myLocation;
    myLocation.latitude = 52.2053;
    myLocation.longitude = 0.1218;
    myLocation.elevation = 20.0;
    observer.setLocation(myLocation);
    
    if (!observer.loadKernels(
        kernelDir + "c2025a6.bsp",
        kernelDir + "naif0012.tls",
        kernelDir + "de440s.bsp"))
    {
        qDebug() << "Failed to load kernels";
        return 1;
    }
    
    // Step 4: Generate calculated data for same time points
    QVector<CalculatedData> calculatedData = generateCalculatedData(observer, cometName, horizonsData);
    
    // Step 5: Compare and analyze
    compareAndAnalyze(horizonsData, calculatedData, outputDir + "error_analysis.csv");
    
    qDebug() << "\n=== Analysis Complete ===";
    qDebug() << "Files created in:" << outputDir;
    qDebug() << "  horizons_readback.csv - Check that Horizons data was read correctly";
    qDebug() << "  error_analysis.csv - Full comparison with errors";
    
    return 0;
}
