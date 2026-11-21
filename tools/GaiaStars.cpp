// Gaia DR3 stars around a pointing, from the local catalogue (GaiaCatalog.hpp), each with the camera signal the live stacker's colour calibration
// predicts for it (StarColourModel: a Pickles spectrum at its BP-RP through the filter and the IMX178), for calibrating images made outside the live stacker
// (tools/montage_area.py's mosaics) the same way.
//   GaiaStars RA_DEG DEC_DEG RADIUS_DEG [--filter Nebula|Clear] [--gmax 14] [--epoch YEAR] [--catalog FILE]
// Prints CSV: a "#white,R,G,B,model" line (the G2V white point), then ra,dec,g,bp_rp,pred_r,pred_g,pred_b per star with a BP-RP.
#include "GaiaCatalog.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <cstdio>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QString filter = "Nebula", catalog; double gmax = 14, epoch = 0; std::vector<double> pos;
    for (int i = 1; i < argc; ++i) {
        const QString a = argv[i]; auto nx = [&] { return i + 1 < argc ? QString(argv[++i]) : QString(); };
        if (a == "--filter") filter = nx(); else if (a == "--gmax") gmax = nx().toDouble(); else if (a == "--epoch") epoch = nx().toDouble();
        else if (a == "--catalog") catalog = nx(); else pos.push_back(a.toDouble());
    }
    if (pos.size() != 3) { fprintf(stderr, "usage: GaiaStars RA_DEG DEC_DEG RADIUS_DEG [--filter Nebula|Clear] [--gmax 14] [--epoch YEAR] [--catalog FILE]\n"); return 1; }
    if (epoch == 0) { const QDate t = QDateTime::currentDateTimeUtc().date(); epoch = t.year() + (t.dayOfYear() - 0.5) / t.daysInYear(); }
    const QString path = catalog.isEmpty() ? gaiaCatalogPath() : catalog;
    std::vector<GaiaStar> cat;
    if (path.isEmpty() || !gaiaConeSearch(path, pos[0], pos[1], pos[2], gmax, epoch, cat)) { fprintf(stderr, "cannot read the Gaia catalogue %s\n", qPrintable(path)); return 1; }
    const StarColourModel model(filter);
    const auto w = model.white();
    printf("#white,%.6g,%.6g,%.6g,%s\nra,dec,g,bp_rp,pred_r,pred_g,pred_b\n", w[0], w[1], w[2], qPrintable(model.name()));
    for (const GaiaStar& s : cat) {
        if (!std::isfinite(s.bpRp)) continue;
        const auto p = model.predict(s.bpRp);
        printf("%.7f,%.7f,%.3f,%.4f,%.6g,%.6g,%.6g\n", s.raDeg, s.decDeg, s.g, s.bpRp, p[0], p[1], p[2]);
    }
    return 0;
}
