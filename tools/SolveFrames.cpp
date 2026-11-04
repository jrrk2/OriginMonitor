// Plate-solve many frames in one process with StellarSolver (the internal astrometry.net), after ~/stellina_processor/Qt_client/StellarSolverManager.h:
// the index files are found once, solvers run one after another (each already spreads its search over threads, MULTI_AUTO; 8 side by side solved only
// 73 of 136 Veil frames, in 76 s, against 136 of 136 in 24 s one at a time, 3 Nov 2026), each hinted by the frame's own RA/DEC header within --radius deg, at the scale of the
// half-resolution green plane (2.95"/px). One process instead of a solve-field per frame: no index loading, temporary files or process start per frame.
//
//   SolveFrames [--out-dir DIR] [--jobs 1] [--radius 0.5] [--scale-low 2.85] [--scale-high 3.05] FILE.fits... | @LIST
//
// For each frame writes DIR/<stem>.wcs (a header-only FITS file as solve-field's --wcs: TAN, CRVAL/CRPIX/CD of the half-resolution green plane, the image
// tools/canvas_stack.py maps) or DIR/<stem>.wcs.fail, and prints one JSON line {file, ok, seconds, ra, dec, pixscale, orientation}.
// The CD matrix is fitted to the solver's own pixel-to-sky mapping at 25 points over the frame, so no orientation or parity convention has to be guessed.
//   mkdir -p build/solveframes && cd build/solveframes && qmake6 ../../tools/SolveFrames.pro && make
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QQueue>
#include <QTextStream>
#include <cmath>
#include <cstdio>
#include <fitsio.h>
#include <stellarsolver.h>

static Parameters g_par; static const Parameters& par0() { return g_par; }
struct Job { QString file; double ra = 0, dec = 0; std::vector<float> g; int w = 0, h = 0; QElapsedTimer t; };

// the half-resolution green plane (mean of the two greens of each 2x2 cell) and the header position
static bool loadFrame(Job& j)
{
    fitsfile* f = nullptr; int st = 0;
    if (fits_open_file(&f, j.file.toLocal8Bit().constData(), READONLY, &st)) return false;
    long n[2] = {0, 0}; int naxis = 0;
    fits_get_img_dim(f, &naxis, &st); fits_get_img_size(f, 2, n, &st);
    char cm[FLEN_COMMENT]; int s2 = 0; if (fits_read_key(f, TDOUBLE, "RA", &j.ra, cm, &s2)) { s2 = 0; fits_read_key(f, TDOUBLE, "CRVAL1", &j.ra, cm, &s2); }
    s2 = 0; if (fits_read_key(f, TDOUBLE, "DEC", &j.dec, cm, &s2)) { s2 = 0; fits_read_key(f, TDOUBLE, "CRVAL2", &j.dec, cm, &s2); }
    if (st || naxis != 2) { fits_close_file(f, &st); return false; }
    std::vector<float> d(size_t(n[0]) * n[1]); long first[2] = {1, 1};
    fits_read_pix(f, TFLOAT, first, long(d.size()), nullptr, d.data(), nullptr, &st); fits_close_file(f, &st);
    if (st) return false;
    j.w = int(n[0] / 2); j.h = int(n[1] / 2); j.g.resize(size_t(j.w) * j.h);
    for (int y = 0; y < j.h; ++y) for (int x = 0; x < j.w; ++x)
        j.g[size_t(y) * j.w + x] = 0.5f * (d[size_t(2 * y) * n[0] + 2 * x] + d[size_t(2 * y + 1) * n[0] + 2 * x + 1]);
    return true;
}

// TAN CD matrix through the solver's mapping: sky at a 5x5 grid of pixels, projected on the tangent plane at the centre, least squares x,y -> xi,eta
static bool writeWcs(StellarSolver* s, const Job& j, const QString& path, QJsonObject* out)
{
    WCSData wd = s->getWCSData();
    // The solver's pixels sit 1.5 px from ours (measured against solve-field on 40 Veil frames: +1.50, +1.60 px, scatter 0.07; rotation and scale agree to
    // 0.004 deg and 0.02%): 1-based coordinates plus half a pixel from the 2x downsample. Our 0-based pixel x is its x + 1.5.
    const double k0 = 1.0 + (par0().downsample - 1) / 2.0;
    auto sky = [&](double x, double y, FITSImage::wcs_point& p) { return wd.pixelToWCS(QPointF(x + k0, y + k0), p); };
    const double cx = (j.w - 1) / 2.0, cy = (j.h - 1) / 2.0;                       // 0-based pixel centres
    FITSImage::wcs_point c; if (!sky(cx, cy, c)) return false;
    const double a0 = c.ra * M_PI / 180, d0 = c.dec * M_PI / 180;
    double sxx = 0, sxy = 0, syy = 0, bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
    for (int iy = 0; iy < 5; ++iy) for (int ix = 0; ix < 5; ++ix) {
        const double x = (j.w - 1) * ix / 4.0, y = (j.h - 1) * iy / 4.0; FITSImage::wcs_point p;
        if (!sky(x, y, p)) return false;
        const double a = p.ra * M_PI / 180, d = p.dec * M_PI / 180, cosc = std::sin(d0) * std::sin(d) + std::cos(d0) * std::cos(d) * std::cos(a - a0);
        const double xi = std::cos(d) * std::sin(a - a0) / cosc * 180 / M_PI, eta = (std::cos(d0) * std::sin(d) - std::sin(d0) * std::cos(d) * std::cos(a - a0)) / cosc * 180 / M_PI;
        const double dx = x - cx, dy = y - cy;
        sxx += dx * dx; sxy += dx * dy; syy += dy * dy; bx1 += dx * xi; by1 += dy * xi; bx2 += dx * eta; by2 += dy * eta;
    }
    const double det = sxx * syy - sxy * sxy; if (std::fabs(det) < 1e-12) return false;
    const double cd11 = (bx1 * syy - by1 * sxy) / det, cd12 = (by1 * sxx - bx1 * sxy) / det, cd21 = (bx2 * syy - by2 * sxy) / det, cd22 = (by2 * sxx - bx2 * sxy) / det;
    fitsfile* f = nullptr; int st = 0; QFile::remove(path);
    if (fits_create_file(&f, path.toLocal8Bit().constData(), &st)) return false;
    fits_create_img(f, BYTE_IMG, 0, nullptr, &st);
    const char* t1 = "RA---TAN"; const char* t2 = "DEC--TAN"; double crval1 = c.ra, crval2 = c.dec, crpix1 = cx + 1, crpix2 = cy + 1; int wi = j.w, hi = j.h;
    fits_write_key(f, TSTRING, "CTYPE1", (void*)t1, "TAN (gnomonic)", &st); fits_write_key(f, TSTRING, "CTYPE2", (void*)t2, "TAN (gnomonic)", &st);
    fits_write_key(f, TDOUBLE, "CRVAL1", &crval1, "RA at CRPIX, deg", &st); fits_write_key(f, TDOUBLE, "CRVAL2", &crval2, "Dec at CRPIX, deg", &st);
    fits_write_key(f, TDOUBLE, "CRPIX1", &crpix1, "frame centre, 1-based", &st); fits_write_key(f, TDOUBLE, "CRPIX2", &crpix2, "", &st);
    double v[4] = {cd11, cd12, cd21, cd22}; const char* k[4] = {"CD1_1", "CD1_2", "CD2_1", "CD2_2"};
    for (int i = 0; i < 4; ++i) fits_write_key(f, TDOUBLE, k[i], &v[i], "", &st);
    fits_write_key(f, TINT, "IMAGEW", &wi, "solved image (half-resolution green plane)", &st); fits_write_key(f, TINT, "IMAGEH", &hi, "", &st);
    fits_write_comment(f, "SolveFrames (StellarSolver)", &st);
    fits_close_file(f, &st);
    const FITSImage::Solution& so = s->getSolution();
    (*out)["ra"] = c.ra; (*out)["dec"] = c.dec; (*out)["pixscale"] = so.pixscale; (*out)["orientation"] = so.orientation;
    return st == 0;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QString outDir = "."; int jobs = 1; bool verbose = false; double radius = 0.5, lo = 2.85, hi = 3.05; QStringList files;
    const QStringList a = app.arguments();
    for (int i = 1; i < a.size(); ++i) {
        if (a[i] == "--out-dir" && i + 1 < a.size()) outDir = a[++i];
        else if (a[i] == "--jobs" && i + 1 < a.size()) jobs = a[++i].toInt();
        else if (a[i] == "--radius" && i + 1 < a.size()) radius = a[++i].toDouble();
        else if (a[i] == "--scale-low" && i + 1 < a.size()) lo = a[++i].toDouble();
        else if (a[i] == "--scale-high" && i + 1 < a.size()) hi = a[++i].toDouble();
        else if (a[i] == "--verbose") verbose = true;
        else if (a[i].startsWith("@")) { QFile f(a[i].mid(1)); if (f.open(QIODevice::ReadOnly)) for (const QByteArray& l : f.readAll().split('\n')) if (!l.trimmed().isEmpty()) files << QString::fromLocal8Bit(l.trimmed()); }
        else files << a[i];
    }
    if (files.isEmpty()) { std::fprintf(stderr, "usage: SolveFrames [--out-dir DIR] [--jobs N] [--radius DEG] [--scale-low A] [--scale-high B] FILE... | @LIST\n"); return 2; }
    QDir().mkpath(outDir);
    QStringList indexPaths; for (const QString& p : {QString("/usr/share/astrometry"), QString("/usr/local/share/astrometry"), QString("/usr/local/astrometry/data")}) if (QDir(p).exists()) indexPaths << p;
    QList<Parameters> profiles = StellarSolver::getBuiltInProfiles();
    Parameters par = profiles.isEmpty() ? Parameters() : profiles.at(0);
    par.multiAlgorithm = SSolver::MULTI_AUTO; par.search_radius = radius; par.autoDownsample = false; par.downsample = 2;   // -z2
    par.inParallel = false; par.solverTimeLimit = 60; par.resort = true;
    par.initialKeep = 1000; par.keepNum = 300; par.saturationLimit = 64000;
    g_par = par;

    QQueue<QString> todo; for (const QString& f : files) todo.enqueue(f);
    QMap<StellarSolver*, Job*> active; int done = 0, solved = 0; QElapsedTimer all; all.start();
    QTextStream out(stdout);
    std::function<void()> next = [&]() {
        while (active.size() < jobs && !todo.isEmpty()) {
            Job* j = new Job; j->file = todo.dequeue();
            const QString stem = QFileInfo(j->file).completeBaseName();
            if (QFile::exists(outDir + "/" + stem + ".wcs")) { ++done; ++solved; delete j; continue; }       // cached
            if (!loadFrame(*j)) { out << QJsonDocument(QJsonObject{{"file", j->file}, {"ok", false}, {"error", "unreadable"}}).toJson(QJsonDocument::Compact) << "\n"; out.flush(); ++done; delete j; continue; }
            FITSImage::Statistic stat; stat.width = uint16_t(j->w); stat.height = uint16_t(j->h); stat.channels = 1; stat.dataType = TFLOAT; stat.bytesPerPixel = 4;
            stat.samples_per_channel = uint32_t(j->w) * j->h; stat.size = int64_t(stat.samples_per_channel) * 4;
            float mn = 1e30f, mx = -1e30f; double sum = 0; for (float v : j->g) { mn = std::min(mn, v); mx = std::max(mx, v); sum += v; }
            stat.min[0] = mn; stat.max[0] = mx; stat.mean[0] = sum / j->g.size(); stat.median[0] = stat.mean[0];
            StellarSolver* s = new StellarSolver(SSolver::SOLVE, stat, reinterpret_cast<const uint8_t*>(j->g.data()), nullptr);
            s->setProperty("ExtractorType", SSolver::EXTRACTOR_INTERNAL); s->setProperty("SolverType", SSolver::SOLVER_STELLARSOLVER);
            s->setIndexFolderPaths(indexPaths); s->setParameters(par);
            s->setSearchPositionInDegrees(j->ra, j->dec); s->setProperty("UsePosition", true);
            s->setSearchScale(lo, hi, SSolver::ARCSEC_PER_PIX); s->setProperty("UseScale", true);
            s->setSSLogLevel(verbose ? SSolver::LOG_VERBOSE : SSolver::LOG_OFF);
            if (verbose) { s->setLogLevel(SSolver::LOG_MSG); QObject::connect(s, &StellarSolver::logOutput, &app, [](const QString& m) { std::fprintf(stderr, "%s\n", m.toLocal8Bit().constData()); }); }
            QObject::connect(s, &StellarSolver::finished, &app, [&, s]() {
                Job* j = active.take(s); const QString stem = QFileInfo(j->file).completeBaseName();
                QJsonObject r{{"file", j->file}, {"seconds", std::round(j->t.elapsed() / 100.0) / 10}};
                const bool ok = s->solvingDone() && s->hasWCSData() && writeWcs(s, *j, outDir + "/" + stem + ".wcs", &r);
                if (!ok) { QFile ff(outDir + "/" + stem + ".wcs.fail"); if (ff.open(QIODevice::WriteOnly)) ff.close(); } else ++solved;
                r["ok"] = ok; out << QJsonDocument(r).toJson(QJsonDocument::Compact) << "\n"; out.flush();
                ++done; s->deleteLater(); delete j;
                if (todo.isEmpty() && active.isEmpty()) {
                    std::fprintf(stderr, "solved %d of %d frames in %.1f s (%.2f s per frame, %d at a time)\n", solved, done, all.elapsed() / 1000.0, all.elapsed() / 1000.0 / std::max(done, 1), jobs);
                    app.quit();
                } else next();
            });
            j->t.start(); active[s] = j; s->start();
        }
        if (todo.isEmpty() && active.isEmpty()) { std::fprintf(stderr, "solved %d of %d frames (all cached or unreadable)\n", solved, done); QMetaObject::invokeMethod(&app, "quit", Qt::QueuedConnection); }
    };
    next();
    return app.exec();
}
