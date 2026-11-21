// Gaia colour calibration of saved live stacks (LiveStacker::calibrateColour, the call the daemon makes as a stack grows), for stacks saved before it existed
// or to redo one. Writes colour.json and a new preview.png into the stack directory; state.bin is only read. The first time, the old preview is kept as
// preview_starwb.png (the first frame's star-median white balance).
//   StackColour STACKDIR [--ra DEG --dec DEG] [--catalog FILE] [--margin DEG] [--dry-run]
// Without --ra/--dec the pointing in colour.json is used. Prints the calibration report as one JSON line.
#include "LiveStacker.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <cstdio>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QString dir, catalog; double ra = -1, dec = -999, margin = 0.2; bool dry = false;
    for (int i = 1; i < argc; ++i) {
        const QString a = argv[i]; auto nx = [&] { return i + 1 < argc ? QString(argv[++i]) : QString(); };
        if (a == "--ra") ra = nx().toDouble(); else if (a == "--dec") dec = nx().toDouble(); else if (a == "--catalog") catalog = nx();
        else if (a == "--margin") margin = nx().toDouble(); else if (a == "--dry-run") dry = true; else dir = a;
    }
    LiveStacker st;
    if (dir.isEmpty() || !st.load(dir)) { fprintf(stderr, "cannot load a stack from %s\n", qPrintable(dir)); return 1; }
    if (dec < -90) { if (!st.hasPointing()) { fprintf(stderr, "no --ra/--dec and no pointing in colour.json\n"); return 1; } ra = st.pointingRa(); dec = st.pointingDec(); }
    st.setPointing(ra, dec);
    QJsonObject r = st.calibrateColour(ra, dec, catalog, LiveStacker::kArcsecPerPx, margin);
    r["directory"] = dir; r["filter"] = st.referenceFilter();
    if (r["ok"].toBool() && !dry) {
        if (QFile::exists(dir + "/preview.png") && !QFile::exists(dir + "/preview_starwb.png")) QFile::copy(dir + "/preview.png", dir + "/preview_starwb.png");
        r["written"] = st.saveColourAndPreview(dir);
    }
    printf("%s\n", QJsonDocument(r).toJson(QJsonDocument::Compact).constData());
    return r["ok"].toBool() ? 0 : 2;
}
