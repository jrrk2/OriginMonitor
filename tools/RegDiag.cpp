// Diagnose registration failures: RegDiag ref.fits.fz target.fits.fz [more targets...]
// For each target: the star count and sharpness, then registerStars() against the reference and against the first target (a same-night frame), and the star list written to stars_<n>.csv
#include "Registration.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <algorithm>
#include <cstdio>

static QByteArray readFrame(const QString& path)
{
    if (!path.endsWith(".fz")) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
    QProcess p; p.start("funpack", {"-C", "-S", path}); p.waitForFinished(120000);
    return p.exitCode() == 0 ? p.readAllStandardOutput() : QByteArray();
}
static void describe(const char* tag, const SkyFrame& F)
{
    std::vector<float> h, e; for (size_t i = 0; i < F.stars.size() && i < 60; ++i) { h.push_back(F.stars[i].hfr); e.push_back(F.stars[i].elong); }
    std::sort(h.begin(), h.end()); std::sort(e.begin(), e.end());
    printf("%-10s stars %3zu  bg %.0f noise %.1f  hfr(med of 60 brightest) %.2f  elong(med) %.2f  brightest flux %.0f  40th flux %.0f\n", tag, F.stars.size(), F.background, F.noise,
           h.empty() ? 0 : h[h.size() / 2], e.empty() ? 0 : e[e.size() / 2], F.stars.empty() ? 0 : F.stars[0].flux, F.stars.size() > 40 ? F.stars[40].flux : 0);
}
static void report(const char* tag, const RegResult& r) { printf("   %-28s ok=%d inliers=%d rms=%.2f rot=%.2f shift=(%.0f,%.0f)\n", tag, r.ok, r.inliers, r.rms, r.T.rotationDeg(), r.T.tx, r.T.ty); }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) return 1;
    const SkyFrame ref = analyseFrame(readFrame(argv[1])); describe("reference", ref);
    std::vector<SkyStar> refStars(ref.stars.begin(), ref.stars.begin() + std::min<size_t>(ref.stars.size(), 150));
    SkyFrame first;
    for (int i = 2; i < argc; ++i) {
        const SkyFrame F = analyseFrame(readFrame(argv[i])); printf("%s\n", argv[i]); describe("target", F);
        report("vs reference", registerStars(refStars, F.stars));
        if (i == 2) first = F; else {
            std::vector<SkyStar> fs(first.stars.begin(), first.stars.begin() + std::min<size_t>(first.stars.size(), 150));
            report("vs first target", registerStars(fs, F.stars));
        }
        QFile o(QString("stars_%1.csv").arg(i - 2)); if (o.open(QIODevice::WriteOnly)) { o.write("x,y,flux,hfr,elong\n"); for (const SkyStar& s : F.stars) o.write(QString("%1,%2,%3,%4,%5\n").arg(s.x).arg(s.y).arg(s.flux).arg(s.hfr).arg(s.elong).toUtf8()); }
    }
    QFile o("stars_ref.csv"); if (o.open(QIODevice::WriteOnly)) { o.write("x,y,flux,hfr,elong\n"); for (const SkyStar& s : refStars) o.write(QString("%1,%2,%3,%4,%5\n").arg(s.x).arg(s.y).arg(s.flux).arg(s.hfr).arg(s.elong).toUtf8()); }
    return 0;
}
