// StarDump in.fits[.fz] out.csv : the live star list (x,y,flux,hfr,elong) of one frame, for offline registration experiments
#include "SkyFrame.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QProcess>
static QByteArray readFrame(const QString& path)
{
    if (!path.endsWith(".fz")) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
    QProcess p; p.start("funpack", {"-C", "-S", path}); p.waitForFinished(120000);
    return p.exitCode() == 0 ? p.readAllStandardOutput() : QByteArray();
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv); if (argc < 3) return 1;
    const SkyFrame F = analyseFrame(readFrame(argv[1])); QFile o(argv[2]); if (!F.valid || !o.open(QIODevice::WriteOnly)) return 2;
    o.write("x,y,flux,hfr,elong\n"); for (const SkyStar& s : F.stars) o.write(QString("%1,%2,%3,%4,%5\n").arg(s.x).arg(s.y).arg(s.flux).arg(s.hfr).arg(s.elong).toUtf8());
    return 0;
}
