// Offline driver for the live pipeline: runs archive FITS frames through SkyMonitor::process (the same call the daemon makes for each snapshot), so the verdicts, the registration and the
// stacking are exactly the live ones. Input: a TSV list, one frame per line: path, exposure_s, iso, utc (ISO), ra_deg, dec_deg. .fz files are unpacked with funpack.
//   StackFiles list.tsv --target "Messier 101" --ra-hours 14.0536 --dec-deg 54.349 [--lat 52.1489 --lon 0.1018] [--out records.jsonl] [--resume]
// --resume continues the stack already in $ORIGIN_DATA_DIR/stacks/<target> instead of starting a new one.
// A 7th column (target name, with columns 8 and 9 = its RA in hours and Dec in degrees) switches target like a supervisor block change: setTarget() with the stack of that target kept on disk.
// $ORIGIN_DATA_DIR decides where the stack (stacks/<target>), previews and sky map go: point it at a scratch directory.
#include "SkyMonitor.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <cmath>
#include <cstdio>

static QByteArray readFrame(const QString& path)
{
    if (!path.endsWith(".fz")) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
    QProcess p; p.start("funpack", {"-C", "-S", path}); p.waitForFinished(120000);
    return p.exitCode() == 0 ? p.readAllStandardOutput() : QByteArray();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QString list, target = "target", out = "records.jsonl"; double raH = -1, decD = 0, lat = 52.1489, lon = 0.1018; bool resume = false;
    for (int i = 1; i < argc; ++i) {
        const QString a = argv[i]; auto nx = [&] { return i + 1 < argc ? QString(argv[++i]) : QString(); };
        if (a == "--target") target = nx(); else if (a == "--ra-hours") raH = nx().toDouble(); else if (a == "--dec-deg") decD = nx().toDouble();
        else if (a == "--lat") lat = nx().toDouble(); else if (a == "--lon") lon = nx().toDouble(); else if (a == "--out") out = nx(); else if (a == "--resume") resume = true; else list = a;
    }
    QFile lf(list), of(out);
    if (!lf.open(QIODevice::ReadOnly) || !of.open(QIODevice::WriteOnly | QIODevice::Truncate)) { fprintf(stderr, "cannot open the list or the output\n"); return 1; }
    SkyMonitor mon; Astro::Site site; site.latDeg = lat; site.lonDeg = lon; mon.setSite(site); mon.setAutoStack(true);
    mon.setTarget(target, raH, decD, !resume);
    int n = 0, stacked = 0;
    while (!lf.atEnd()) {
        const QStringList c = QString::fromUtf8(lf.readLine()).trimmed().split('\t'); if (c.size() < 6) continue;
        if (c.size() >= 9 && c[6] != target) { mon.setTarget(c[6], c[7].toDouble(), c[8].toDouble(), false); target = c[6]; fprintf(stderr, "target -> %s\n", qPrintable(target)); }
        const QByteArray fits = readFrame(c[0]); ++n;
        QJsonObject r;
        if (fits.isEmpty()) r = QJsonObject{{"verdict", "unreadable"}};
        else r = mon.process(fits, c[1].toDouble(), c[2].toInt(), c[4].toDouble() * M_PI / 180.0, c[5].toDouble() * M_PI / 180.0, QDateTime::fromString(c[3], Qt::ISODate));
        r["file"] = c[0];
        if (r["registration"].toObject()["stacked"].toBool()) ++stacked;
        of.write(QJsonDocument(r).toJson(QJsonDocument::Compact) + "\n");
        if (n % 25 == 0) { fprintf(stderr, "%d frames, %d stacked, last verdict %s\n", n, stacked, qPrintable(r["verdict"].toString())); of.flush(); }
    }
    mon.saveStack();
    printf("%d frames, %d stacked\n%s\n", n, stacked, QJsonDocument(mon.stackSummary()).toJson(QJsonDocument::Compact).constData());
    return 0;
}
