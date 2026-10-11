// OriginHeadless: the Celestron Origin control server and its command-line client in one executable that links no Qt GUI or Widgets code.
//
//   OriginHeadless serve --connect 192.168.1.195 [--daemon --pidfile F --log-file F]    run the server (foreground, or as a daemon)
//   OriginHeadless <command> [args]                                                       talk to the running server, print ONE JSON object
//   OriginHeadless commands                                                               machine-readable catalogue of every command
//
// Every command prints exactly one JSON object on stdout (progress goes to stderr) and exits 0 when "ok" is true, 1 when it is false, 2 on a usage error.
#include "ControlServer.hpp"
#include "DataDir.hpp"
#include "FocusAnalyzer.hpp"
#include "OriginBackend.hpp"
#include "Planner.hpp"
#include "Weather.hpp"
#include "Mail.hpp"
#include "Ephemeris.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDir>
#include <QProcess>
#include <QSocketNotifier>
#include <QTcpSocket>
#include <QTimeZone>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <functional>

#ifdef Q_OS_UNIX
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

// ---------------------------------------------------------------- command table (drives parsing, help and the JSON catalogue)

struct Arg {
    QString name;          // long option name, or positional name
    QChar type;            // s string, i int, d double, b flag (true when present), n negated flag (--no-X gives key=false), o on|off, j JSON object text
    bool positional = false, required = false;
    QString def, help;
    QString key;           // JSON key sent to the server; defaults to name with '-' -> '_'
};
struct Cmd {
    QString name, group, help, server;   // server: control-server command that a plain pass-through sends; empty for orchestrated commands
    bool moves = false;                  // moves the mount, the focuser or the camera: only on the user's say-so
    bool offline = false;                // needs no running server
    QList<Arg> args;
};

Arg P(const QString& n, QChar t, const QString& help, bool req = true, const QString& key = QString()) { return {n, t, true, req, QString(), help, key}; }
Arg O(const QString& n, QChar t, const QString& help, const QString& def = QString(), const QString& key = QString()) { return {n, t, false, false, def, help, key}; }

QList<Arg> plannerArgs()
{
    return {O("date", 's', "local evening date YYYY-MM-DD (default: the night we are in)"), O("horizon", 's', "horizon JSON file (default: $ORIGIN_HORIZON or tools/horizon.json)"),
            O("min-alt", 'd', "flat horizon floor when there is no file, deg", "20"), O("sun-alt", 'd', "Sun altitude below which it is dark, deg", "-18"),
            O("moon-sep", 'd', "minimum Moon separation, deg", "30"), O("min-block", 'd', "shortest useful block, minutes", "45"), O("max-block", 'd', "longest time on one target, minutes", "150"),
            O("step", 'd', "time step, minutes", "5"), O("sky-map", 'b', "also use the obstruction bins learned in <data dir>/sky_map.json"),
            O("lat", 'd', "site latitude, deg north", "52.2054"), O("lon", 'd', "site longitude, deg east", "0.1218"), O("top", 'i', "targets to list", "25"), O("text", 'b', "human-readable table instead of JSON"),
            O("weather", 'o', "on|off drop hours the Open-Meteo forecast calls cloudy, wet or foggy (cached 1 h in the data dir; ignored if the request fails)", "on"),
            O("min-quality", 'd', "forecast quality 0..1 below which an hour is unobservable", "0.5")};
}

const QList<Cmd>& commands()
{
    static const QList<Cmd> c = [] {
        QList<Cmd> v;
        auto pass = [&](const QString& name, const QString& group, const QString& help, const QString& server, QList<Arg> args = {}, bool moves = false) { v.append({name, group, help, server, moves, false, args}); };
        // --- server lifecycle
        v.append({"serve", "server", "Run the control server (listens on 127.0.0.1 only). Foreground by default; --daemon detaches. SIGTERM/SIGINT save the live stack and exit cleanly.", "", false, true,
                  {O("control-port", 'i', "TCP port to listen on", "8765"), O("connect", 's', "telescope host to connect to at start-up"), O("telescope-port", 'i', "telescope port (default 80; other values are for simulators)", "80"), O("control-raw", 'b', "enable the 'send' raw-command passthrough"), O("no-cache", 'b', "disable the read-only response cache (default: status/mount/focuser/focus/analysis/sky-map replies are cached for 1-30 s)"),
                   O("daemon", 'b', "detach from the terminal (double fork; Unix)"), O("pidfile", 's', "write the PID here, refuse to start when it names a live process, remove on exit"),
                   O("log-file", 's', "append log lines here (default stderr; required in practice with --daemon)")}});
        v.append({"commands", "server", "Machine-readable catalogue of all commands, their arguments and which of them move the telescope.", "", false, true, {}});
        pass("ping", "server", "Check that the server answers.", "ping");
        pass("cache", "server", "Response-cache statistics (hits, misses, entries); --clear flushes it. Any query accepts fresh=true on the wire to bypass the cache.", "cache", {O("clear", 'b', "flush the cache")});
        pass("quit", "server", "Stop any sequence, save the live stack and shut the server down.", "quit");
        pass("connect", "server", "Connect the server to a telescope.", "connect", {P("host", 's', "telescope IP or host name"), O("telescope-port", 'i', "telescope port (default 80)", QString(), "port")});
        // --- state
        pass("status", "state", "Connection state, camera settings and the last TaskController/Focuser/Camera status messages.", "status");
        pass("mount", "state", "Mount status: RA/Dec, Alt/Az, tracking, goto state.", "mount_status");
        pass("focuser", "state", "Focuser position, travel limits, NeedAutoFocus.", "focuser_status");
        pass("focuser-settings", "state", "Focuser advanced settings.", "focuser_settings");
        pass("focus", "state", "Last focus score (HFR, lower is better), optionally the newest N.", "focus_status", {O("history", 'i', "also return the newest N scores", "0")});
        pass("analysis", "state", "Latest per-frame sky analysis (verdict, transparency, stars), optionally history.", "analysis", {O("history", 'i', "also return the newest N records", "0")});
        pass("events", "state", "Event log since a timestamp.", "events", {O("since", 'd', "epoch ms; 0 = from the start", "0"), O("limit", 'i', "maximum events", "200")});
        pass("sky-map", "state", "Learned sky obstruction map.", "sky_map");
        pass("preview", "state", "Render the live stack to <data dir>/previews/live_preview.png without saving the stack (read-only; used by OriginWatch).", "preview");
        pass("stack", "state", "Live-stack summary; saves state and preview.png and returns the preview path.", "stack");
        // --- configuration
        pass("manual", "config", "Camera to manual exposure mode.", "manual");
        pass("auto", "config", "Camera to automatic exposure mode.", "auto");
        pass("set", "config", "Set camera exposure (s, minimum 1 us), ISO and binning. The telescope reverts these after slews and captures, so re-send them.", "set",
             {O("exposure", 'd', "seconds"), O("iso", 'i', "ISO"), O("binning", 'i', "binning")});
        pass("monitor", "config", "Site, black level, auto-stacking and the transparency floor of the sky monitor.", "monitor",
             {O("lat", 'd', "site latitude", QString(), "lat_deg"), O("lon", 'd', "site longitude", QString(), "lon_deg"), O("black-level", 'd', "sensor black level, ADU"),
              O("auto-stack", 'o', "on|off: stack accepted frames automatically"), O("min-transparency", 'd', "reject frames below this transparency (0..1)")});
        pass("target", "config", "Choose or read the live-stack target; resumes a saved stack of the same name unless --reset.", "target",
             {P("name", 's', "target name, e.g. M31 (omit to read the current one)", false), O("ra-hours", 'd', "J2000 RA hours"), O("dec-deg", 'd', "J2000 Dec degrees"), O("reset", 'b', "discard the saved stack")});
        // --- actions
        pass("snapshot", "action", "Take one frame, download it and report the path.", "snapshot", {O("dir", 's', "output directory (default: data dir)"), O("timeout", 'i', "seconds", "30")}, true);
        pass("goto", "action", "Slew to J2000 coordinates and wait for the mount to finish (the firmware then centres and autofocuses for about 50 s).", "goto",
             {P("ra-hours", 'd', "J2000 RA, hours 0..24", true, "ra_hours"), P("dec-deg", 'd', "J2000 Dec, degrees", true, "dec_deg"), O("timeout", 'i', "seconds", "180")}, true);
        pass("initialize", "action", "RunInitialize: the mount homes and aligns. Waits and reports the outcome.", "initialize",
             {O("lat", 'd', "site latitude", "52.2054", "lat_deg"), O("lon", 'd', "site longitude", "0.1218", "lon_deg"), O("tz", 's', "IANA time zone", "Europe/London", "timezone"),
              O("fake", 'b', "FakeInitialize"), O("no-wait", 'n', "return as soon as the telescope accepts", QString(), "wait"), O("timeout", 'i', "seconds", "300")}, true);
        pass("focuser-move", "action", "Move the focuser to an absolute position (within travel limits and --max-step of the current one).", "focuser_move",
             {P("position", 'i', "absolute focuser position"), O("max-step", 'i', "largest allowed move", "3000"), O("timeout", 'i', "seconds", "60")}, true);
        pass("sequence", "action", "Unattended exposure loop inside the server: start, stop or status.", "sequence",
             {P("action", 's', "start | stop | status (default status)", false), O("exposure", 'd', "seconds"), O("iso", 'i', "ISO"), O("binning", 'i', "binning"), O("bad-limit", 'i', "bad frames before probing"),
              O("probe-s", 'd', "seconds between probes"), O("pipeline", 'o', "on|off overlap download with the next exposure"), O("early", 'o', "on|off request the next exposure early"),
              O("max-frames", 'i', "stop after N frames"), O("for-minutes", 'd', "stop after N minutes"), O("until-local", 's', "stop at local HH:MM"), O("reason", 's', "stop reason"),
              O("focus-pause", 'o', "on|off stop the sequence when the focus drifts, so that focus-sweep can be run (default off: only an event is recorded)"),
              O("focus-drift", 'd', "HFR ratio over the baseline that counts as drift", "1.15"), O("focus-baseline", 'd', "baseline HFR, full-resolution px (default: the first 8 good frames)"),
              O("focus-window", 'i', "frames in the rolling median", "8")}, true);
        pass("send", "action", "Raw telescope command (server must run with --control-raw).", "send",
             {P("command", 's', "command name"), P("destination", 's', "destination, e.g. Camera"), O("params", 'j', "JSON object of parameters", "{}"), O("timeout", 'i', "seconds", "10")}, true);
        // --- orchestrated
        v.append({"focus-sweep", "action", "Move the focuser through positions around the current one, take frames, score the star HFR in-process and report the best position (restores the start unless --apply).", "", true, false,
                  {O("span", 'i', "steps either side of the current position", "600"), O("step", 'i', "step between positions", "100"), O("frames", 'i', "snapshots per position (median)", "3"),
                   O("dir", 's', "snapshot directory (default: temp)"), O("apply", 'b', "leave the focuser at the best position")}});
        v.append({"now", "planning", "Time, darkness, current target, minutes each candidate stays observable, and the planner's suggestion. Read-only.", "", false, false, plannerArgs()});
        {
            QList<Arg> a = {P("name", 's', "target key or name, e.g. M31"), O("exposure", 'd', "seconds to set after the slew", "10"), O("iso", 'i', "ISO to set after the slew", "800"),
                            O("not-before", 's', "\"dusk\" (Sun below -12 deg at the site) or a local HH:MM; refuse to slew earlier", "dusk")};
            v.append({"go", "planning", "Slew to a target that is observable now: set site and target, goto, wait for centring/autofocus, re-send exposure and ISO.", "", true, false, a + plannerArgs()});
        }
        v.append({"run", "planning", "UNATTENDED night supervisor: follows the plan, goes to a backup target on obstruction or prolonged cloud, refocuses on drift, parks on a rain forecast or at dawn. Needs --unattended (it slews); --dry-run only prints the plan.", "", true, false,
                  [] { QList<Arg> a = {O("unattended", 'b', "confirm that the supervisor may slew and run all night"), O("dry-run", 'b', "print the plan and backups, do nothing"),
                                       O("iso", 'i', "ISO for the frames (falls back once to 200 if the first frames are all rejected on a clear forecast)", "800"),
                                       O("max-exposure", 'd', "longest exposure, s (further limited by field rotation for each block)", "20"),
                                       O("cloud-patience", 'd', "minutes of cloud before going to a backup target", "10"), O("give-up", 'd', "minutes of cloud on one block before moving to the next block", "60"),
                                       O("not-before", 's', "\"dusk\" (Sun below -12 deg at the site) or a local HH:MM; no slewing before this", "dusk"), O("connect", 's', "telescope host to reconnect to (default: the server's)"),
                                       O("mail", 'b', "e-mail a summary of the night when the run ends")};
                       QList<Arg> pa = plannerArgs();
                       for (Arg& x : pa) if (x.name == "min-quality") x.help += " (run default 0.2: the forecast is a soft prior, the sky monitor decides)";
                       return a + pa; }()});
        v.append({"slew-altaz", "action", "Move the mount in altitude and/or azimuth with the slow-slew rates the GUI arrows use, closed loop on the mount's own Alt/Azm, then stop. Works before the mount is aligned (e.g. to find clear sky on the other side of the meridian before 'initialize'). The rate sign is checked during the first seconds and flipped if the axis moves the wrong way.", "", true, false,
                  {O("az", 'd', "target azimuth, deg (0=N, 90=E, 180=S, 270=W)"), O("alt", 'd', "target altitude, deg"), O("tolerance", 'd', "stop within this many degrees", "3"), O("timeout", 'i', "seconds per axis", "120")}});
        v.append({"autorun", "planning", "Dusk launcher for cron (every 5 minutes): once per night, if the telescope is out and reachable, starts the daemon, connects, initializes the mount if needed and starts 'run --unattended --mail' in the background. Does nothing (quietly) when the telescope does not answer, when nothing is observable, when rain is forecast, or when ~/.config/origin/autorun.off exists. Slews the telescope: only schedule it if that is wanted.", "", true, false,
                  [] { QList<Arg> a = {O("telescope", 's', "telescope host", "192.168.1.195"), O("start-after", 's', "\"dusk\" (Sun below -12 deg at the site) or a local HH:MM; never act before this", "dusk"), O("stop-before-dawn", 'd', "minutes before the end of darkness after which no run is started", "60"),
                                       O("dry-run", 'b', "say what would be done, do nothing"), O("ignore-time", 'b', "skip the evening/dawn time gates (testing)"), O("no-mail", 'b', "send no e-mails (testing)"), O("iso", 'i', "ISO for the run", "800"), O("max-exposure", 'd', "longest exposure for the run, s", "20")};
                       return a + plannerArgs(); }()});
        v.append({"park", "planning", "Save the stack and slew to Polaris (safe park).", "", true, false, {}});
        v.append({"schedule", "planning", "Plan one night: observable windows against the local horizon, Sun and Moon, plus a greedy one-target-at-a-time plan.", "", false, true, plannerArgs()});
        v.append({"weather", "planning", "Hourly forecast for the site from Open-Meteo: cloud low/mid/high, rain chance, visibility, dew point, wind and a 0..1 imaging quality per hour (cached 1 h; --fresh bypasses).", "", false, true,
                  {O("hours", 'i', "hours to list from now", "24"), O("lat", 'd', "site latitude", "52.2054"), O("lon", 'd', "site longitude", "0.1218"), O("text", 'b', "one line per hour instead of JSON")}});
        v.append({"backup", "planning", "Backup targets when cloud covers the current one: observable now, far from it in the sky, with the longest remaining window and the field-rotation exposure limit. Read-only; the supervisor decides whether to goto.", "", false, true,
                  [] { QList<Arg> a = {O("name", 's', "current target key or name (default: the server's target, else the mount's pointing)"), O("min-sep", 'd', "minimum separation from the current pointing, deg", "40"),
                                       O("count", 'i', "backups to return", "3"), O("min-left", 'd', "minimum minutes the backup stays observable", "45"), O("avoid", 's', "comma-separated target keys already tried")};
                       return a + plannerArgs(); }()});
        v.append({"briefing", "planning", "The two e-mails: --kind dusk (worth putting out tonight?) or night (safe to leave out at 22:00?); --auto sends whichever is due now, once (for cron every 5 minutes); --send mails it (Microsoft Graph, see mail-login). Always saved to <data dir>/briefings/.", "", false, true,
                  [] { QList<Arg> a = {O("kind", 's', "dusk | night"), O("auto", 'b', "send the briefing that is due now, if not already sent tonight"), O("send", 'b', "e-mail it"), O("to", 's', "recipient (default: mail.conf)"),
                                       O("night-at", 's', "local time of the safety e-mail", "22:00"), O("dusk-offset", 'd', "minutes after sunset for the dusk e-mail", "0")};
                       return a + plannerArgs(); }()});
        v.append({"mail-login", "planning", "One-time Microsoft sign-in for the briefing e-mails (OAuth device-code flow: prints a code and a URL; needs ~/.config/origin/mail.conf).", "", false, true, {}});
        v.append({"mail-test", "planning", "Send a test e-mail through the same route.", "", false, true, {O("to", 's', "recipient (default: mail.conf)")}});
        v.append({"moon", "planning", "Moonrise, moonset, transit, illumination and phase (and sunrise/sunset) for each of the next days, from JPL DE440 via CSPICE when available (else a low-precision series; the reply says which).", "", false, true,
                  {O("date", 's', "first local date YYYY-MM-DD (default: today)"), O("days", 'i', "number of days", "7"), O("lat", 'd', "site latitude, deg north", "52.2054"), O("lon", 'd', "site longitude, deg east", "0.1218"),
                   O("time-zone", 's', "IANA time zone for the output", "Europe/London"), O("text", 'b', "one line per day instead of JSON")}});
        v.append({"fieldrot", "planning", "Field-rotation limit on exposure time for an alt-az mount, for a target over the next hours.", "", false, true,
                  {P("ra-hours", 'd', "J2000 RA, hours", true, "ra_hours"), P("dec-deg", 'd', "J2000 Dec, degrees", true, "dec_deg"), O("tol", 'd', "allowed corner trail, px", "2"), O("hours", 'd', "hours to cover", "8"),
                   O("lat", 'd', "site latitude", "52.2054"), O("lon", 'd', "site longitude", "0.1218")}});
        return v;
    }();
    return c;
}

QJsonObject catalogue()
{
    QJsonArray cs;
    for (const Cmd& c : commands()) {
        QJsonArray as;
        for (const Arg& a : c.args) {
            static const QHash<QChar, QString> tn = {{'s', "string"}, {'i', "integer"}, {'d', "number"}, {'b', "flag"}, {'n', "flag"}, {'o', "on|off"}, {'j', "json"}};
            as.append(QJsonObject{{"name", a.name}, {"type", tn.value(a.type)}, {"positional", a.positional}, {"required", a.required}, {"default", a.def.isEmpty() ? QJsonValue() : QJsonValue(a.def)}, {"help", a.help}});
        }
        cs.append(QJsonObject{{"name", c.name}, {"group", c.group}, {"summary", c.help}, {"moves_hardware", c.moves}, {"needs_server", !c.offline}, {"args", as}});
    }
    return QJsonObject{{"ok", true}, {"program", "OriginHeadless"},
        {"usage", "OriginHeadless [--port N] [--pretty] <command> [positional args] [--option value ...]"},
        {"global_options", QJsonArray{"--port N (control port, default 8765 or $ORIGIN_CONTROL_PORT)", "--pretty (indented JSON)", "--fresh (bypass the server's response cache for this query)"}},
        {"conventions", QJsonArray{"stdout is exactly one JSON object", "progress and warnings go to stderr", "exit code 0 = ok:true, 1 = ok:false, 2 = usage error",
                                   "commands with moves_hardware=true move the mount, focuser or camera: run them only when the user has asked for it",
                                   "coordinates are J2000: RA in hours, Dec in degrees", "start the server first: OriginHeadless serve --connect <telescope host>"}},
        {"commands", cs}};
}

// ---------------------------------------------------------------- parsing

struct Parsed { QVariantMap v; QString error; };

Parsed parseArgs(const Cmd& c, const QStringList& rest)
{
    Parsed p; int pos = 0;
    QList<Arg> positionals; for (const Arg& a : c.args) if (a.positional) positionals.append(a);
    for (int i = 0; i < rest.size(); ++i) {
        QString t = rest[i];
        if (t.startsWith("--") && t.size() > 2) {
            QString name = t.mid(2), val; bool hasVal = false;
            const int eq = name.indexOf('=');
            if (eq > 0) { val = name.mid(eq + 1); name = name.left(eq); hasVal = true; }
            const Arg* a = nullptr; for (const Arg& x : c.args) if (!x.positional && x.name == name) a = &x;
            if (!a) { p.error = "unknown option --" + name + " for " + c.name; return p; }
            if (a->type == 'b' || a->type == 'n') { p.v[a->name] = true; continue; }
            if (!hasVal) { if (i + 1 >= rest.size()) { p.error = "--" + name + " needs a value"; return p; } val = rest[++i]; }
            p.v[a->name] = val;
        } else if (pos < positionals.size()) {
            p.v[positionals[pos++].name] = t;
        } else if (t.startsWith("-") && t.size() > 1 && !t[1].isDigit() && t[1] != '.') {
            p.error = "unknown option " + t; return p;     // a bare negative number is a value, not an option
        } else { p.error = "unexpected argument " + t; return p; }
    }
    for (const Arg& a : positionals) if (a.required && !p.v.contains(a.name)) { p.error = "missing <" + a.name + ">"; return p; }
    for (const Arg& a : c.args) {                       // validate types now, so the server never sees junk
        if (!p.v.contains(a.name)) continue;
        bool ok = true; const QString s = p.v[a.name].toString();
        if (a.type == 'i') s.toInt(&ok); else if (a.type == 'd') s.toDouble(&ok);
        else if (a.type == 'o') ok = (s == "on" || s == "off");
        else if (a.type == 'j') ok = QJsonDocument::fromJson(s.toUtf8()).isObject();
        if (!ok) { p.error = QString("bad value '%1' for %2 (expected %3)").arg(s, a.name, QString(a.type)); return p; }
    }
    return p;
}

QJsonValue typed(const Arg& a, const QVariant& v)
{
    const QString s = v.toString();
    switch (a.type.toLatin1()) {
    case 'i': return s.toInt(); case 'd': return s.toDouble(); case 'b': return true; case 'n': return false;
    case 'o': return s == "on"; case 'j': return QJsonDocument::fromJson(s.toUtf8()).object(); default: return s;
    }
}
QString keyOf(const Arg& a) { return a.key.isEmpty() ? QString(a.name).replace('-', '_') : a.key; }

double num(const Parsed& p, const Cmd& c, const QString& name)
{
    if (p.v.contains(name)) return p.v[name].toDouble();
    for (const Arg& a : c.args) if (a.name == name) return a.def.toDouble();
    return 0;
}
QString str(const Parsed& p, const QString& name, const QString& def = QString()) { return p.v.contains(name) ? p.v[name].toString() : def; }

// ---------------------------------------------------------------- client

int g_port = 8765;
bool g_fresh = false;

QJsonObject call(const QJsonObject& reqIn, int timeoutMs = 120000)
{
    QJsonObject req = reqIn; if (g_fresh) req["fresh"] = true;
    QTcpSocket s; s.connectToHost("127.0.0.1", quint16(g_port));
    if (!s.waitForConnected(3000))
        return {{"ok", false}, {"error", QString("cannot connect to the control server on 127.0.0.1:%1 (%2); start it with: OriginHeadless serve --connect <telescope host>").arg(g_port).arg(s.errorString())}};
    s.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n"); s.waitForBytesWritten(3000);
    QByteArray buf; QElapsedTimer t; t.start();
    while (!buf.endsWith('\n') && t.elapsed() < timeoutMs) { if (s.waitForReadyRead(500)) buf += s.readAll(); else if (s.state() != QAbstractSocket::ConnectedState) break; }
    if (!buf.endsWith('\n')) return {{"ok", false}, {"error", "no reply from the control server (timed out or connection closed)"}};
    return QJsonDocument::fromJson(buf).object();
}
QJsonObject call(const QString& cmd, QJsonObject extra = {}, int timeoutMs = 120000) { extra["cmd"] = cmd; return call(extra, timeoutMs); }
void note(const QJsonObject& o) { std::fprintf(stderr, "%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData()); std::fflush(stderr); }
void sleepMs(int ms) { QThread::msleep(ms); }
QJsonObject failure(const QString& m) { return {{"ok", false}, {"error", m}}; }

QJsonObject g_wx;                                   // forecast source/age/errors of the last plannerOptions() call
Planner::Options plannerOptions(const Parsed& p, const Cmd& c)
{
    Planner::Options o;
    if (p.v.contains("date")) o.date = QDate::fromString(str(p, "date"), Qt::ISODate);
    o.site.latDeg = num(p, c, "lat"); o.site.lonDeg = num(p, c, "lon");
    o.horizonFile = p.v.contains("horizon") ? str(p, "horizon") : Planner::defaultHorizonFile();
    if (p.v.contains("sky-map")) o.skyMapFile = originDataDir() + "/sky_map.json";
    o.minAlt = num(p, c, "min-alt"); o.sunAlt = num(p, c, "sun-alt"); o.moonSep = num(p, c, "moon-sep");
    o.minBlockMin = num(p, c, "min-block"); o.maxBlockMin = num(p, c, "max-block"); o.stepMin = num(p, c, "step"); o.top = int(num(p, c, "top"));
    g_wx = QJsonObject();
    if (str(p, "weather", "on") != "off") {
        o.minQuality = num(p, c, "min-quality");
        QJsonObject r = Weather::fetch(o.site.latDeg, o.site.lonDeg, 7, g_fresh ? 0 : 60, originDataDir(), &o.weather);
        r.remove("hours"); g_wx = r;                                           // keep the planner usable when the forecast cannot be had
        if (!r["ok"].toBool()) o.weather.clear();
    }
    return o;
}
QDateTime parseZ(const QString& s)
{
    QDateTime t = QDateTime::fromString(s, "yyyy-MM-ddTHH:mm:ssZ");
    if (t.isValid()) t.setTimeZone(QTimeZone::UTC);
    return t;
}

QJsonObject cmdSchedule(const Parsed& p, const Cmd& c)
{
    QJsonObject s = Planner::schedule(plannerOptions(p, c));
    if (!g_wx.isEmpty()) s["forecast"] = g_wx;
    if (p.v.contains("text")) s["text"] = Planner::scheduleText(s, int(num(p, c, "top")));
    else { QJsonArray t = s["targets"].toArray(), cut; for (int i = 0; i < t.size() && i < int(num(p, c, "top")); ++i) cut.append(t[i]); s["targets"] = cut; }
    return s;
}

QJsonObject cmdWeather(const Parsed& p, const Cmd& c)
{
    QVector<Weather::Hour> hrs;
    QJsonObject r = Weather::fetch(num(p, c, "lat"), num(p, c, "lon"), 7, g_fresh ? 0 : 60, originDataDir(), &hrs);
    if (!r["ok"].toBool()) return r;
    const QDateTime from = QDateTime::currentDateTimeUtc().addSecs(-1800), to = from.addSecs(3600 * int(num(p, c, "hours")));
    QJsonArray out; QString text;
    for (const Weather::Hour& h : hrs) {
        if (h.utc < from || h.utc > to) continue;
        out.append(Weather::toJson(h));
        text += QString("%1Z  cloud %2% (low %3 mid %4 high %5)  rain %6% %7mm  vis %8 km  T %9 dew %10  wind %11 m/s  quality %12\n").arg(h.utc.toString("MM-dd HH:mm")).arg(h.cloud, 3, 'f', 0)
                    .arg(h.low, 3, 'f', 0).arg(h.mid, 3, 'f', 0).arg(h.high, 3, 'f', 0).arg(h.precipProb, 3, 'f', 0).arg(h.precip, 0, 'f', 1).arg(h.visibilityM / 1000, 0, 'f', 0)
                    .arg(h.temp, 0, 'f', 1).arg(h.dew, 0, 'f', 1).arg(h.wind, 0, 'f', 1).arg(h.quality, 0, 'f', 2);
    }
    r["hours"] = out;
    if (p.v.contains("text")) r["text"] = text;
    return r;
}

QJsonObject cmdMoon(const Parsed& p, const Cmd& c)
{
    const QString zoneId = str(p, "time-zone", "Europe/London"); const QTimeZone zone(zoneId.toUtf8());
    if (!zone.isValid()) return failure("unknown time zone " + zoneId);
    const Astro::Site site{num(p, c, "lat"), num(p, c, "lon")};
    QDate d = p.v.contains("date") ? QDate::fromString(str(p, "date"), Qt::ISODate) : QDateTime::currentDateTimeUtc().toTimeZone(zone).date();
    if (!d.isValid()) return failure("date must be YYYY-MM-DD");
    auto local = [&](const QJsonValue& v) { if (v.isNull() || v.isUndefined()) return QString("--:--:--"); return parseZ(v.toString()).toTimeZone(zone).toString("HH:mm:ss"); };
    QJsonArray days; QString text, eph;
    for (int i = 0; i < int(num(p, c, "days")); ++i, d = d.addDays(1)) {
        const QJsonObject r = Planner::sunMoonDay(d, site, zoneId); eph = r["ephemeris"].toString();
        days.append(r);
        text += QString("%1  moonrise %2  moonset %3  transit %4 (alt %5)  %6% %7   sunrise %8 sunset %9\n").arg(d.toString("ddd dd MMM"), local(r["moonrise"]), local(r["moonset"]),
                    local(r["moon_transit"].toObject()["time"]), QString::number(r["moon_transit"].toObject()["alt_deg"].toDouble(), 'f', 0)).arg(int(r["moon_illumination"].toDouble() * 100 + 0.5)).arg(r["moon_phase"].toString(), local(r["sunrise"]), local(r["sunset"]));
    }
    QJsonObject out{{"ok", true}, {"ephemeris", eph}, {"time_zone", zoneId}, {"site", QJsonObject{{"lat_deg", site.latDeg}, {"lon_deg", site.lonDeg}}}, {"days", days}};
    if (p.v.contains("text")) out["text"] = text;
    return out;
}

QJsonObject cmdBackup(const Parsed& p, const Cmd& c)
{
    Planner::Options o = plannerOptions(p, c); o.date = Planner::currentNightDate(o.timeZone);
    double ra = 0, dec = 0; QString from;
    QString name = str(p, "name");
    if (name.isEmpty()) { const QJsonObject tc = call("target", {}, 5000); if (tc["ok"].toBool()) name = tc["target"].toString(); }
    if (!name.isEmpty()) {
        Planner::Options all = o; all.weather.clear();                      // look the target up whatever the forecast says
        const QJsonObject t = Planner::findTarget(all, name);
        if (t["ok"].toBool()) { ra = t["ra_hours"].toDouble(); dec = t["dec_deg"].toDouble(); from = t["key"].toString(); }
    }
    if (from.isEmpty()) {
        const QJsonObject m = call("mount_status", {}, 10000);
        if (!m["ok"].toBool()) return failure("current target unknown: pass --name, or start the server so that the mount's pointing can be read");
        ra = m["mount"].toObject()["Ra"].toDouble() * 12 / M_PI; dec = m["mount"].toObject()["Dec"].toDouble() * 180 / M_PI; from = "mount pointing";
    }
    const QStringList avoid = str(p, "avoid").split(',', Qt::SkipEmptyParts);
    QJsonObject r = Planner::backupTargets(o, ra, dec, QDateTime::currentDateTimeUtc(), num(p, c, "min-sep"), int(num(p, c, "count")), num(p, c, "min-left"), avoid);
    if (r["ok"].toBool()) r["current"] = from;
    if (!g_wx.isEmpty()) r["forecast"] = g_wx;
    return r;
}


// ---------------------------------------------------------------------------------------------------------------------------------------
// briefing: the two e-mails. "dusk" (at sunset): is it worth putting the telescope out tonight? "night" (22:00): is it safe to put it out for the night?
// Both always saved to <data dir>/briefings/; sent with --send (Microsoft Graph, see Mail.hpp). --auto decides which one is due now and whether it was already
// sent (<data dir>/briefings_sent.json), so a cron entry every 5 minutes is all the scheduling there is.
// ---------------------------------------------------------------------------------------------------------------------------------------
QJsonObject cmdBriefing(const Parsed& p, const Cmd& c)
{
    g_fresh = true;                                                                  // always a fresh forecast
    Planner::Options o = plannerOptions(p, c); if (!p.v.contains("date")) o.date = Planner::currentNightDate(o.timeZone);
    const QTimeZone zone(o.timeZone.toUtf8());
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString night = o.date.toString(Qt::ISODate);
    const QJsonObject sm = Planner::sunMoonDay(o.date, o.site, o.timeZone);
    const QJsonObject S = Planner::schedule(o);
    if (!S["ok"].toBool()) return S;
    auto loc = [&](const QDateTime& t) { return t.toTimeZone(zone).toString("HH:mm"); };
    auto locJ = [&](const QJsonValue& v) { return v.isNull() || v.isUndefined() ? QString("--:--") : loc(parseZ(v.toString())); };
    const QDateTime sunset = parseZ(sm["sunset"].toString()), darkStart = parseZ(S["dark_start"].toString()), darkEnd = parseZ(S["dark_end"].toString());

    // which briefing
    QString kind = str(p, "kind");
    QJsonObject sent; { QFile f(originDataDir() + "/briefings_sent.json"); if (f.open(QIODevice::ReadOnly)) sent = QJsonDocument::fromJson(f.readAll()).object(); }
    const QStringList nh = str(p, "night-at", "22:00").split(':');
    const QDateTime nightAt = QDateTime(now.toTimeZone(zone).date().addDays(now.toTimeZone(zone).time().hour() < 12 ? -1 : 0), QTime(nh.value(0).toInt(), nh.value(1).toInt()), zone).toUTC();
    if (p.v.contains("auto")) {
        const QDateTime duskAt = sunset.addSecs(int(num(p, c, "dusk-offset")) * 60);
        if (now >= duskAt && now < darkEnd && !sent.contains(night + "_dusk") && now < nightAt) kind = "dusk";
        else if (now >= nightAt && now < nightAt.addSecs(90 * 60) && now < darkEnd && !sent.contains(night + "_night")) kind = "night";
        else return {{"ok", true}, {"due", false}, {"night", night}, {"note", "no briefing due now"}};
    }
    if (kind != "dusk" && kind != "night") return failure("--kind dusk|night (or --auto)");

    // forecast hours with gusts, and the next hours of rain at 15-minute resolution
    QVector<Weather::Hour> hrs; const QJsonObject fr = Weather::fetch(o.site.latDeg, o.site.lonDeg, 3, 0, originDataDir(), &hrs);
    const QJsonObject wx = S["weather"].toObject();
    QString subject, body;
    const QString title = QDateTime(o.date, QTime(12, 0), zone).toString("dddd d MMMM");
    const QString moonLine = QString("Moon: %1, %2% lit; rises %3, sets %4 (%5)").arg(sm["moon_phase"].toString()).arg(int(sm["moon_illumination"].toDouble() * 100 + 0.5)).arg(locJ(sm["moonrise"]), locJ(sm["moonset"]),
                                  Ephemeris::description().startsWith("JPL") ? "JPL DE440" : "approximate");
    const QString footer = QString("\n--\nForecast: %1 (fetched %2Z). Quality is 0..1: low and mid cloud block, high cloud counts half, cut for rain, fog, dew risk and wind; windows need >= %3.\n%4\nSent by OriginHeadless briefing (%5).\n")
                               .arg(fr["source"].toString(), fr["fetched"].toString().mid(11, 5)).arg(o.minQuality).arg(moonLine, kind);

    if (kind == "dusk") {
        // usable windows = runs of forecast hours (inside the dark) with quality >= min
        struct Win { QDateTime a, b; double q = 0; int n = 0; };
        QList<Win> wins; Win cur; bool open = false;
        for (const QJsonValue& hv : wx["hours"].toArray()) {
            const QJsonObject h = hv.toObject(); const QDateTime a = parseZ(h["utc"].toString().left(19) + "Z");
            const QDateTime from = std::max(a, std::max(darkStart, now)), to = std::min(a.addSecs(3600), darkEnd);
            if (to <= from) continue;
            if (h["quality"].toDouble() >= o.minQuality) { if (!open) { cur = Win(); cur.a = from; open = true; } cur.b = to; cur.q += h["quality"].toDouble(); ++cur.n; }
            else if (open) { wins.append(cur); open = false; }
        }
        if (open) wins.append(cur);
        int best = 0, total = 0; for (const Win& w : wins) { const int m = int(w.a.secsTo(w.b) / 60); total += m; best = std::max(best, m); }
        const QString verdict = best >= 90 ? "WORTH PUTTING OUT" : best >= 40 ? "MARGINAL" : "NOT WORTH IT";
        QString when; for (const Win& w : wins) when += (when.isEmpty() ? "" : ", ") + loc(w.a) + "-" + loc(w.b);
        subject = QString("[Origin] Dusk: %1%2").arg(verdict, wins.isEmpty() ? " - no usable sky forecast" : " - clear " + when);
        body = QString("%1 (night of %2)\n\nVERDICT: %3\n").arg(verdict, title, best >= 90 ? "put the telescope out." : best >= 40 ? "only short gaps are forecast; your call." : "leave it in.");
        body += QString("Sunset %1, astronomical dark %2 to %3. %4 of the %5 dark minutes are forecast usable; longest clear stretch %6 min.\n").arg(loc(sunset), loc(darkStart), loc(darkEnd)).arg(total).arg(int(darkStart.secsTo(darkEnd) / 60)).arg(best);
        if (wx.contains("rain_from")) body += "Rain is forecast from " + locJ(wx["rain_from"]) + ".\n";
        body += "\nUsable windows (local): " + (when.isEmpty() ? "none" : when) + "\n\nHour by hour (local): cloud low/mid/high, rain chance, quality\n";
        for (const QJsonValue& hv : wx["hours"].toArray()) {
            const QJsonObject h = hv.toObject();
            body += QString("  %1  %2% (%3/%4/%5)  rain %6%  %7  %8\n").arg(loc(parseZ(h["utc"].toString().left(19) + "Z")), QString::number(h["cloud"].toDouble(), 'f', 0).rightJustified(3), QString::number(h["low"].toDouble(), 'f', 0),
                         QString::number(h["mid"].toDouble(), 'f', 0), QString::number(h["high"].toDouble(), 'f', 0), QString::number(h["precip_prob"].toDouble(), 'f', 0), QString::number(h["quality"].toDouble(), 'f', 2),
                         h["quality"].toDouble() >= o.minQuality ? "usable" : "");
        }
        if (!S["plan"].toArray().isEmpty()) {
            body += "\nPlan for the usable hours:\n";
            for (const QJsonValue& v : S["plan"].toArray()) {
                const QJsonObject e = v.toObject(); QStringList bs; for (const QJsonValue& bv : e["backups"].toArray()) bs << bv.toObject()["key"].toString();
                body += QString("  %1-%2  %3 (alt %4->%5)%6\n").arg(loc(parseZ(e["start"].toString())), loc(parseZ(e["end"].toString())), e["name"].toString()).arg(e["alt_start"].toInt()).arg(e["alt_end"].toInt())
                            .arg(bs.isEmpty() ? QString() : "   backups: " + bs.join(", "));
            }
        }
    } else {
        const QVector<QPair<QDateTime, double>> nc = Weather::nowcast(o.site.latDeg, o.site.lonDeg, 3);
        const QDateTime horizon = darkEnd.addSecs(3600);
        double maxProb = 0, sumMm = 0, maxGust = 0, minDew = 99, minVis = 1e9, maxHum = 0; QString rainFrom; int n = 0;
        for (const Weather::Hour& h : hrs) {
            if (h.utc.addSecs(3600) < now || h.utc > horizon) continue;
            ++n; maxProb = std::max(maxProb, h.precipProb); sumMm += h.precip; maxGust = std::max(maxGust, h.gust); minDew = std::min(minDew, h.temp - h.dew); minVis = std::min(minVis, h.visibilityM); maxHum = std::max(maxHum, h.humidity);
            if (rainFrom.isEmpty() && (h.precipProb >= 50 || h.precip >= 0.2)) rainFrom = loc(h.utc);
        }
        double ncMm = 0; QString ncFrom; for (const auto& q : nc) { ncMm += q.second; if (ncFrom.isEmpty() && q.second >= 0.1) ncFrom = loc(q.first); }
        const bool unsafe = !rainFrom.isEmpty() || ncMm >= 0.2, caution = !unsafe && (maxProb >= 25 || maxGust > 12 || minDew < 1.0);
        const QString verdict = unsafe ? "NOT SAFE" : caution ? "CAUTION" : "SAFE";
        int usableAfter = 0; for (const QJsonValue& hv : wx["hours"].toArray()) { const QJsonObject h = hv.toObject(); const QDateTime a = parseZ(h["utc"].toString().left(19) + "Z");
            if (h["quality"].toDouble() >= o.minQuality && a.addSecs(3600) > now) usableAfter += 60; }
        subject = QString("[Origin] %1: %2 to put out%3").arg(nightAt.toTimeZone(zone).toString("HH:mm"), verdict, unsafe ? (ncFrom.isEmpty() ? " - rain from " + rainFrom : " - rain within 3 h (" + ncFrom + ")") : caution ? "" : QString(" - dry until %1").arg(loc(darkEnd)));
        body = QString("%1 for leaving the telescope out tonight (%2).\n\n").arg(verdict, title);
        body += unsafe ? "VERDICT: keep it in (or take it in before the rain).\n" : caution ? "VERDICT: possible, but keep an eye on it.\n" : "VERDICT: safe to put out; no rain forecast before dawn.\n";
        body += QString("\nNext %1 forecast hours (to %2): rain chance up to %3%, %4 mm in total, wind gusts up to %5 m/s (%6 mph), lowest temperature-dew point margin %7 degC, humidity up to %8%, visibility down to %9 km.\n")
                    .arg(n).arg(loc(horizon)).arg(maxProb, 0, 'f', 0).arg(sumMm, 0, 'f', 1).arg(maxGust, 0, 'f', 0).arg(maxGust * 2.237, 0, 'f', 0).arg(minDew, 0, 'f', 1).arg(maxHum, 0, 'f', 0).arg(minVis / 1000, 0, 'f', 0);
        if (!rainFrom.isEmpty()) body += "Rain (>= 50% chance or >= 0.2 mm) first forecast at " + rainFrom + ".\n";
        if (!nc.isEmpty()) body += QString("Next 3 hours at 15-minute resolution: %1 mm%2.\n").arg(ncMm, 0, 'f', 1).arg(ncFrom.isEmpty() ? ", dry" : ", first rain at " + ncFrom);
        body += QString("Usable imaging sky still forecast: %1 min.\n").arg(usableAfter);
        if (minDew < 1.0) body += "Dew risk: the air is within 1 degC of the dew point at some point; use the dew heater.\n";
    }
    body += footer;

    QJsonObject out{{"ok", true}, {"kind", kind}, {"night", night}, {"subject", subject}, {"body", body}};
    const QString dir = originDataDir() + "/briefings"; QDir().mkpath(dir);
    QFile cf(dir + "/" + night + "_" + kind + ".txt"); if (cf.open(QIODevice::WriteOnly | QIODevice::Truncate)) { cf.write(("Subject: " + subject + "\n\n" + body).toUtf8()); out["saved"] = cf.fileName(); }
    if (p.v.contains("send") || p.v.contains("auto")) {
        const QJsonObject r = Mail::send(subject, body, str(p, "to"));
        out["sent"] = r["ok"].toBool(); if (!r["ok"].toBool()) out["send_error"] = r["error"];
        if (r["ok"].toBool() && p.v.contains("auto")) { sent[night + "_" + kind] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate); QFile f(originDataDir() + "/briefings_sent.json"); if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(sent).toJson()); }
        out["ok"] = r["ok"].toBool();
    }
    return out;
}

QJsonObject cmdMailTest(const Parsed& p, const Cmd&)
{
    return Mail::send("[Origin] test message", "If you can read this, the OriginHeadless briefing e-mails can reach you.\n" + QDateTime::currentDateTime().toString(Qt::ISODate), str(p, "to"));
}

QJsonObject cmdNow(const Parsed& p, const Cmd& c)
{
    Planner::Options o = plannerOptions(p, c); o.date = Planner::currentNightDate(o.timeZone);
    const QJsonObject S = Planner::schedule(o);
    if (!S["ok"].toBool()) return S;
    const QDateTime now = QDateTime::currentDateTimeUtc(), ds = parseZ(S["dark_start"].toString()), de = parseZ(S["dark_end"].toString());
    const QJsonObject tc = call("target", {}, 5000);
    const QString cur = tc["ok"].toBool() ? tc["target"].toString() : QString();
    QString planned;
    for (const QJsonValue& v : S["plan"].toArray()) { const QJsonObject e = v.toObject(); if (parseZ(e["start"].toString()) <= now && now <= parseZ(e["end"].toString()).addSecs(300)) planned = e["key"].toString(); }
    QJsonArray obs;
    QList<QJsonObject> ts; for (const QJsonValue& v : S["targets"].toArray()) ts.append(v.toObject());
    std::stable_sort(ts.begin(), ts.end(), [](const QJsonObject& a, const QJsonObject& b) { return a["priority"].toInt() > b["priority"].toInt(); });
    for (const QJsonObject& t : ts)
        if (parseZ(t["window_start"].toString()) <= now && now <= parseZ(t["window_end"].toString()))
            obs.append(QJsonObject{{"key", t["key"]}, {"minutes_left", std::round(now.secsTo(parseZ(t["window_end"].toString())) / 60.0)}, {"priority", t["priority"]}, {"peak_utc", t["peak"]},
                                   {"peak_alt", t["peak_alt"]}, {"planned", t["key"].toString() == planned}, {"current", !cur.isEmpty() && t["key"].toString() == cur}});
    QJsonObject r{{"ok", true}, {"now_utc", now.toString("HH:mm")}, {"dark", ds <= now && now <= de}, {"dark_start", S["dark_start"]}, {"dark_end", S["dark_end"]},
                  {"minutes_to_dark", now < ds ? std::round(now.secsTo(ds) / 60.0) : 0}, {"minutes_to_dawn", now < de ? std::round(now.secsTo(de) / 60.0) : 0},
                  {"moon_illumination", S["moon_illumination"]}, {"current_target", cur.isEmpty() ? QJsonValue() : QJsonValue(cur)}, {"observable_now", obs}};
    if (!tc["ok"].toBool()) r["server_note"] = tc["error"];
    if (!g_wx.isEmpty()) r["forecast"] = g_wx;
    const QJsonObject wx = S["weather"].toObject();
    if (!wx.isEmpty()) {
        QVector<Weather::Hour> hrs; QJsonObject w{{"usable_dark_minutes", wx["usable_minutes"]}, {"dark_minutes", wx["dark_minutes"]}};
        if (wx.contains("rain_from")) w["rain_from"] = wx["rain_from"];
        for (const QJsonValue& v : wx["hours"].toArray()) { const QJsonObject h = v.toObject(); const QDateTime t = parseZ(h["utc"].toString().left(19) + "Z"); if (t <= now && now < t.addSecs(3600)) { w["now"] = h; break; } }
        r["weather"] = w;
    }
    return r;
}

bool waitIdle(int timeoutS)
{
    QElapsedTimer t; t.start(); QString seen;
    while (t.elapsed() < timeoutS * 1000) {
        const QString st = call("status", {}, 10000)["last"].toObject()["TaskController/GetStatus"].toObject()["State"].toString();
        if (!st.isEmpty()) seen = st;
        if (seen.toUpper() == "IDLE" && t.elapsed() > 5000) return true;
        sleepMs(3000);
    }
    note({{"warning", "TaskController did not report IDLE"}, {"last_state", seen}});
    return false;
}

// The "not before" guard for slewing. "dusk" (the default) means the Sun is below --slew-sun-alt (-12 deg, nautical twilight) at the site, which follows the
// seasons; HH:MM keeps a fixed local clock time for the evening. Returns the reason when it is too early, else an empty string.
QString tooEarlyToSlew(const QString& spec, const Astro::Site& site)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (spec.contains(':')) {
        const QStringList hm = spec.split(':'); const QDateTime loc = QDateTime::currentDateTime(), nb(loc.date(), QTime(hm.value(0).toInt(), hm.value(1).toInt()));
        return (loc < nb && loc.time().hour() >= 6) ? QString("before %1 local").arg(spec) : QString();
    }
    const double alt = Astro::altAz(Astro::sunPosition(now), site, now).alt;
    return alt > -12.0 ? QString("the Sun is at %1 deg (slewing waits for -12 deg)").arg(alt, 0, 'f', 1) : QString();
}

QJsonObject cmdGo(const Parsed& p, const Cmd& c)
{
    { const QString early = tooEarlyToSlew(str(p, "not-before", "dusk"), Astro::Site{num(p, c, "lat"), num(p, c, "lon")}); if (!early.isEmpty()) return failure("too early: " + early + "; refusing to slew"); }
    Planner::Options o = plannerOptions(p, c); o.date = Planner::currentNightDate(o.timeZone);
    o.weather.clear();                              // the forecast guides planning; at the telescope the sky monitor decides
    const QJsonObject t = Planner::findTarget(o, str(p, "name"));
    if (!t["ok"].toBool()) return t;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (!(parseZ(t["window_start"].toString()) <= now && now <= parseZ(t["window_end"].toString())))
        return failure(QString("%1 is outside its observable window (%2 to %3); refusing to slew").arg(t["key"].toString(), t["window_start"].toString(), t["window_end"].toString()));
    QJsonObject before = call("mount_status", {}, 15000);
    if (!before["ok"].toBool()) return before;
    note({{"step", "pointing before"}, {"mount", before["mount"]}});
    note({{"step", "going to"}, {"target", t["name"]}, {"ra_hours", t["ra_hours"]}, {"dec_deg", t["dec_deg"]}});
    QJsonObject r;
    if (!(r = call("monitor", {{"lat_deg", o.site.latDeg}, {"lon_deg", o.site.lonDeg}, {"auto_stack", true}}))["ok"].toBool()) return r;
    if (!(r = call("target", {{"name", t["key"]}, {"ra_hours", t["ra_hours"]}, {"dec_deg", t["dec_deg"]}}))["ok"].toBool()) return r;
    if (!(r = call("goto", {{"ra_hours", t["ra_hours"]}, {"dec_deg", t["dec_deg"]}}, 240000))["ok"].toBool()) return r;
    const bool idle = waitIdle(180);
    r = call("set", {{"exposure", num(p, c, "exposure")}, {"iso", int(num(p, c, "iso"))}});        // the telescope reverts these after a slew
    return {{"ok", r["ok"].toBool()}, {"target", t["key"]}, {"idle_after_slew", idle}, {"exposure", num(p, c, "exposure")}, {"iso", int(num(p, c, "iso"))}};
}

QJsonObject cmdPark()
{
    const QJsonObject st = call("stack", {}, 60000);
    const QJsonObject r = call("goto", {{"ra_hours", 2.5303}, {"dec_deg", 89.264}}, 240000);   // Polaris
    return {{"ok", r["ok"].toBool()}, {"parked_at", "Polaris"}, {"stack", st}, {"goto", r}};
}

QJsonObject cmdFocusSweep(const Parsed& p, const Cmd& c)
{
    const int span = int(num(p, c, "span")), step = std::max(1, int(num(p, c, "step"))), frames = int(num(p, c, "frames"));
    const QString dir = p.v.contains("dir") ? str(p, "dir") : QDir::tempPath() + "/originctl_sweep";
    QDir().mkpath(dir);
    const QJsonObject st = call("focuser_status", {}, 15000);
    if (!st["ok"].toBool()) return st;
    const int start = st["focuser"].toObject()["Position"].toInt();
    QJsonArray results; QJsonObject best; double bestHfr = 1e9; bool good = false;
    for (int pos = start - span; pos <= start + span; pos += step) {
        const QJsonObject mv = call("focuser_move", {{"position", pos}, {"max_step", span + step + 100}}, 90000);
        if (!mv["ok"].toBool()) { results.append(QJsonObject{{"position", pos}, {"error", mv["error"]}}); break; }
        QList<double> h; int stars = 0;
        for (int f = 0; f < frames; ++f) {
            const QJsonObject sn = call("snapshot", {{"dir", dir}}, 60000);
            if (!sn["ok"].toBool()) continue;
            QFile file(sn["path"].toString());
            if (!file.open(QIODevice::ReadOnly)) continue;
            const FocusResult fr = evaluateFocus(file.readAll());
            if (fr.valid && fr.stars >= 5) { h.append(fr.hfrPx); stars += fr.stars; }
        }
        QJsonObject e{{"position", pos}};
        if (!h.isEmpty()) {
            std::sort(h.begin(), h.end()); const double med = h[h.size() / 2];
            e["hfr_px"] = std::round(med * 1000) / 1000; e["stars"] = stars / int(h.size()); e["frames"] = int(h.size());
            good = true; if (med < bestHfr) { bestHfr = med; best = e; }
        } else { e["hfr_px"] = QJsonValue(); e["note"] = "too few measurable stars (badly defocused, or the telescope returned a non-FITS frame)"; }
        results.append(e); note(e);
    }
    const int final = p.v.contains("apply") && good ? best["position"].toInt() : start;
    const QJsonObject back = call("focuser_move", {{"position", final}, {"max_step", span + step + 100}}, 90000);
    return {{"ok", good}, {"unit", "HFR in full-resolution sensor pixels, lower is better"}, {"start", start}, {"best", best}, {"applied", p.v.contains("apply") && good}, {"final", back}, {"results", results}};
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// run: the unattended supervisor. Follows the night plan block by block; reacts to what the server reports:
//   obstruction (tree line, building)  -> leave at once for a backup target, waiting would not help
//   cloud                               -> keep probing for --cloud-patience minutes, then go to a backup target far away in the sky, else keep waiting;
//                                          after --give-up minutes of cloud on one block, move on to the next block
//   focus drift                         -> narrow focus sweep (at most every 20 min), apply the best position, resume
//   rain forecast within the hour       -> stop, park, end the night
//   sequence/connection failure         -> reconnect and restart (3 tries), then park
// Every decision is logged to stderr and to <data dir>/run_<date>.jsonl. The night ends parked on Polaris.
// ---------------------------------------------------------------------------------------------------------------------------------------
struct RunState {
    Planner::Options o; QTimeZone zone; QDateTime darkEnd;
    double maxExposure = 20; int iso = 800; double patienceMin = 10, giveUpMin = 60; QString host;
    bool isoFallbackDone = false, rain = false;
    QDateTime lastRefocus, lastForecast; QStringList tried;
    int switches = 0, refocuses = 0, restarts = 0, blocksDone = 0;
    QString logPath; QJsonArray decisions;
};
RunState g_run;

void runLog(const QString& what, QJsonObject extra = {})
{
    extra["time"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate); extra["decision"] = what;
    note(extra); g_run.decisions.append(extra);
    QFile f(g_run.logPath);
    if (f.open(QIODevice::Append)) f.write(QJsonDocument(extra).toJson(QJsonDocument::Compact) + "\n");
}

bool rainSoon()
{
    QVector<Weather::Hour> hrs;
    if (!Weather::fetch(g_run.o.site.latDeg, g_run.o.site.lonDeg, 2, 20, originDataDir(), &hrs)["ok"].toBool()) return false;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const Weather::Hour& h : hrs)
        if (h.utc >= now.addSecs(-1800) && h.utc <= now.addSecs(3600) && (h.precipProb >= 60 || h.precip >= 0.2)) return true;
    return false;
}

double exposureFor(const QJsonObject& t, const QDateTime& from, const QDateTime& to)
{
    double lim = g_run.maxExposure;
    const QDateTime mid = from.addSecs(from.secsTo(to) / 2);
    for (const QDateTime& when : {from, mid, to}) {
        const Planner::RotLimit r = Planner::maxExposure(t["ra_hours"].toDouble(), t["dec_deg"].toDouble(), g_run.o.site, when, 2.0);
        if (std::isfinite(r.tMax)) lim = std::min(lim, r.tMax);
    }
    return std::max(1.0, std::floor(lim));
}

QJsonObject slewTo(const QJsonObject& t, double exposure, int iso)
{
    QJsonObject r;
    if (!(r = call("monitor", {{"lat_deg", g_run.o.site.latDeg}, {"lon_deg", g_run.o.site.lonDeg}, {"auto_stack", true}}))["ok"].toBool()) return r;
    if (!(r = call("target", {{"name", t["key"]}, {"ra_hours", t["ra_hours"]}, {"dec_deg", t["dec_deg"]}}))["ok"].toBool()) return r;     // resumes a saved stack of that name
    if (!(r = call("goto", {{"ra_hours", t["ra_hours"]}, {"dec_deg", t["dec_deg"]}}, 240000))["ok"].toBool()) return r;
    waitIdle(180);
    return call("set", {{"exposure", exposure}, {"iso", iso}});
}

bool startSeq(double exposure, int iso, const QDateTime& endUtc, bool focusPause)
{
    const QJsonObject r = call("sequence", {{"action", "start"}, {"exposure", exposure}, {"iso", iso}, {"pipeline", false}, {"focus_pause", focusPause},
                                            {"until_local", endUtc.toTimeZone(g_run.zone).toString("HH:mm")}});
    return r["ok"].toBool() && r["status"].toObject()["active"].toBool();
}

enum class RunResult { Done, Failed, Rain };

RunResult runTarget(const QJsonObject& t, const QDateTime& endUtc, int depth)
{
    const QDateTime t0 = QDateTime::currentDateTimeUtc();
    const double exposure = exposureFor(t, t0, endUtc);
    int iso = g_run.iso;
    runLog("start", {{"target", t["key"]}, {"exposure_s", exposure}, {"iso", iso}, {"until", endUtc.toTimeZone(g_run.zone).toString("HH:mm")}, {"backup", depth > 0}});
    QJsonObject r = slewTo(t, exposure, iso);
    if (!r["ok"].toBool()) { runLog("slew_failed", {{"target", t["key"]}, {"error", r["error"]}}); return RunResult::Failed; }
    if (!startSeq(exposure, iso, endUtc, true)) { runLog("sequence_start_failed", {{"target", t["key"]}}); return RunResult::Failed; }
    QDateTime cloudSince, blockCloudStart; double cloudMinTotal = 0; QDateTime lastTick = QDateTime::currentDateTimeUtc();
    qint64 seq = call("events", {{"since", 1e12}})["next"].toDouble();            // only events from now on
    bool obstructed = false; int failures = 0;
    while (true) {
        sleepMs(20000);
        const QDateTime now = QDateTime::currentDateTimeUtc();
        if (now >= endUtc.addSecs(300) || now >= g_run.darkEnd) { call("sequence", {{"action", "stop"}, {"reason", "block end"}}); runLog("block_end", {{"target", t["key"]}}); return RunResult::Done; }
        if (!g_run.lastForecast.isValid() || g_run.lastForecast.secsTo(now) > 1200) {
            g_run.lastForecast = now;
            if (rainSoon()) { call("sequence", {{"action", "stop"}, {"reason", "rain forecast"}}); runLog("rain_forecast", {{"target", t["key"]}}); g_run.rain = true; return RunResult::Rain; }
        }
        const QJsonObject st = call("sequence", {{"action", "status"}}, 20000);
        if (!st["ok"].toBool()) { if (++failures >= 3) { runLog("server_lost", {{"error", st["error"]}}); return RunResult::Failed; } continue; }
        failures = 0;
        const QJsonObject s = st["status"].toObject();
        const QJsonObject ev = call("events", {{"since", double(seq)}}, 20000);
        if (ev["ok"].toBool()) { seq = qint64(ev["next"].toDouble()); for (const QJsonValue& v : ev["events"].toArray()) if (v.toObject()["kind"].toString() == "sequence_obstructed") obstructed = true; }
        if (!s["active"].toBool()) {
            const QString why = s["stop_reason"].toString();
            if (why == "end time reached" || why == "block end") return RunResult::Done;
            if (why == "focus drift" && (!g_run.lastRefocus.isValid() || g_run.lastRefocus.secsTo(now) > 1200)) {
                runLog("refocus", {{"target", t["key"]}, {"median_hfr_px", s["focus"].toObject()["median_px"]}, {"baseline_px", s["focus"].toObject()["baseline_px"]}});
                const Cmd* fs = nullptr; for (const Cmd& c : commands()) if (c.name == "focus-sweep") fs = &c;
                Parsed fp; fp.v["span"] = 300; fp.v["step"] = 60; fp.v["frames"] = 2; fp.v["apply"] = true;
                const QJsonObject sw = cmdFocusSweep(fp, *fs);
                runLog("refocus_done", {{"ok", sw["ok"]}, {"best", sw["best"]}}); ++g_run.refocuses; g_run.lastRefocus = QDateTime::currentDateTimeUtc();
                r = call("set", {{"exposure", exposure}, {"iso", iso}});
            } else {
                runLog("sequence_stopped", {{"target", t["key"]}, {"reason", why}});
                if (++g_run.restarts > 6) return RunResult::Failed;
                const QJsonObject stt = call("status", {}, 20000);
                if (!stt["connected"].toBool() && !g_run.host.isEmpty()) { call("connect", {{"host", g_run.host}}, 60000); sleepMs(30000); runLog("reconnect", {{"host", g_run.host}}); }
            }
            if (!startSeq(exposure, iso, endUtc, !(why == "focus drift"))) { runLog("sequence_restart_failed", {{"target", t["key"]}}); return RunResult::Failed; }
            continue;
        }
        const bool probing = s["state"].toString() == "probing";
        if (probing) { if (!cloudSince.isValid()) cloudSince = now; cloudMinTotal += lastTick.secsTo(now) / 60.0; } else cloudSince = QDateTime();
        lastTick = now;
        // a high ISO whose dark baseline was never learned reads as cloud on a clear forecast: fall back once to ISO 200
        if (!g_run.isoFallbackDone && iso > 200 && s["frames"].toInt() >= 6 && s["good"].toInt() == 0 && !obstructed) {
            QVector<Weather::Hour> hrs; Weather::fetch(g_run.o.site.latDeg, g_run.o.site.lonDeg, 2, 60, originDataDir(), &hrs);
            const Weather::Hour* h = Weather::at(hrs, now);
            if (!h || h->quality >= 0.8) {
                g_run.isoFallbackDone = true; iso = 200;
                call("sequence", {{"action", "stop"}, {"reason", "iso fallback"}});
                runLog("iso_fallback", {{"from", g_run.iso}, {"to", 200}, {"why", "6 bad frames at the start while the forecast is clear"}});
                call("set", {{"exposure", exposure}, {"iso", iso}});
                startSeq(exposure, iso, endUtc, true); continue;
            }
        }
        QString reason;
        if (obstructed) reason = "obstruction";
        else if (cloudSince.isValid() && cloudSince.secsTo(now) / 60.0 >= g_run.patienceMin) reason = QString("cloud for %1 min").arg(int(cloudSince.secsTo(now) / 60));
        if (cloudMinTotal >= g_run.giveUpMin && reason.isEmpty()) { call("sequence", {{"action", "stop"}, {"reason", "cloud give-up"}}); runLog("give_up", {{"target", t["key"]}, {"cloud_min", cloudMinTotal}}); return RunResult::Done; }
        if (reason.isEmpty()) continue;

        // switch: pick a backup far from here in the sky
        const QJsonObject mount = call("mount_status", {}, 15000)["mount"].toObject();
        g_run.tried.append(t["key"].toString());
        const QJsonObject bk = Planner::backupTargets(g_run.o, mount["Ra"].toDouble() * 12 / M_PI, mount["Dec"].toDouble() * 180 / M_PI, now, 40, 3, 45, g_run.tried);
        const QJsonArray list = bk["backups"].toArray();
        if (list.isEmpty()) {
            runLog("no_backup", {{"target", t["key"]}, {"reason", reason}});
            if (obstructed) { call("sequence", {{"action", "stop"}, {"reason", "obstructed, no backup"}}); return RunResult::Done; }
            cloudSince = now; continue;                                                  // cloud: keep waiting
        }
        const QJsonObject next = list.first().toObject();
        call("sequence", {{"action", "stop"}, {"reason", "switching target: " + reason}}); call("stack", {}, 60000);
        runLog("switch", {{"from", t["key"]}, {"to", next["key"]}, {"reason", reason}, {"separation_deg", next["separation_deg"]}}); ++g_run.switches;
        return runTarget(next, endUtc, depth + 1);
    }
}

// the morning summary of an unattended night, from the decision log and the stacks the server reports
void mailRunSummary(const QString& outcome, const QString& night)
{
    QString body = QString("Unattended night %1: %2.\n\nBlocks run: %3, target switches: %4, refocuses: %5, restarts: %6.\n\nWhat it did (local time):\n")
                       .arg(night, outcome).arg(g_run.blocksDone).arg(g_run.switches).arg(g_run.refocuses).arg(g_run.restarts);
    for (const QJsonValue& v : g_run.decisions) {
        const QJsonObject d = v.toObject(); QStringList bits;
        for (auto it = d.constBegin(); it != d.constEnd(); ++it)
            if (it.key() != "time" && it.key() != "decision" && !it.value().isObject() && !it.value().isArray() && !it.value().isNull()) bits << it.key() + "=" + (it.value().isString() ? it.value().toString() : QString::number(it.value().toDouble(), 'g', 6));
        body += QString("  %1  %2  %3\n").arg(parseZ(d["time"].toString().left(19) + "Z").toTimeZone(g_run.zone).toString("HH:mm"), d["decision"].toString(), bits.join("  "));
    }
    const QJsonObject st = call("sequence", {{"action", "status"}}, 20000)["status"].toObject();
    if (!st.isEmpty()) body += QString("\nLast sequence: %1 frames, %2 good.\n").arg(st["frames"].toInt()).arg(st["good"].toInt());
    body += "\nFull log: " + g_run.logPath + "\nStacks: " + originDataDir() + "/stacks\nThe telescope was parked on Polaris. Put it away when convenient.\n";
    const QJsonObject r = Mail::send(QString("[Origin] night %1: %2").arg(night, outcome), body);
    note({{"summary_mail", r["ok"]}, {"error", r["error"]}});
}

QJsonObject cmdRun(const Parsed& p, const Cmd& c)
{
    if (!p.v.contains("dry-run") && !p.v.contains("unattended")) return failure("run takes over the night and slews the telescope; add --unattended to confirm (or --dry-run to see the plan)");
    Planner::Options o = plannerOptions(p, c); o.date = Planner::currentNightDate(o.timeZone);
    if (!p.v.contains("min-quality")) o.minQuality = 0.2;      // the forecast is a soft prior here: only write off hours that are clearly hopeless, the sky monitor and backups handle the rest
    const QJsonObject S = Planner::schedule(o);
    if (!S["ok"].toBool()) return S;
    if (p.v.contains("dry-run")) { QJsonObject d{{"ok", true}, {"dry_run", true}, {"plan", S["plan"]}, {"weather", S["weather"]}}; if (!g_wx.isEmpty()) d["forecast"] = g_wx; return d; }
    if (S["plan"].toArray().isEmpty()) return failure("nothing observable tonight (see schedule); stay parked");
    const QJsonObject pg = call("ping", {}, 5000);
    if (!pg["ok"].toBool()) return pg;

    g_run = RunState();
    g_run.o = o; g_run.zone = QTimeZone(o.timeZone.toUtf8()); g_run.darkEnd = parseZ(S["dark_end"].toString());
    g_run.maxExposure = num(p, c, "max-exposure"); g_run.iso = int(num(p, c, "iso")); g_run.patienceMin = num(p, c, "cloud-patience"); g_run.giveUpMin = num(p, c, "give-up");
    g_run.host = p.v.contains("connect") ? str(p, "connect") : call("status", {}, 10000)["host"].toString();
    g_run.logPath = originDataDir() + "/run_" + S["night"].toString().remove('-') + ".jsonl";
    runLog("run_start", {{"night", S["night"]}, {"blocks", S["plan"].toArray().size()}, {"iso", g_run.iso}, {"max_exposure", g_run.maxExposure}});

    RunResult res = RunResult::Done;
    for (const QJsonValue& bv : S["plan"].toArray()) {
        const QJsonObject b = bv.toObject();
        const QDateTime start = parseZ(b["start"].toString()), end = parseZ(b["end"].toString()).addSecs(300);
        QDateTime now = QDateTime::currentDateTimeUtc();
        if (now >= end) continue;
        while (now < start && now < g_run.darkEnd) { sleepMs(30000); now = QDateTime::currentDateTimeUtc(); }
        { const QString early = tooEarlyToSlew(str(p, "not-before", "dusk"), g_run.o.site);
          if (!early.isEmpty()) { runLog("too_early", {{"why", early}}); while (!tooEarlyToSlew(str(p, "not-before", "dusk"), g_run.o.site).isEmpty() && QDateTime::currentDateTimeUtc() < g_run.darkEnd) sleepMs(30000); now = QDateTime::currentDateTimeUtc(); } }
        if (now >= g_run.darkEnd) break;
        g_run.tried = QStringList{b["key"].toString()};
        res = runTarget(b, end, 0);
        ++g_run.blocksDone;
        if (res == RunResult::Rain || res == RunResult::Failed) break;
        g_run.tried.clear();
    }
    const QJsonObject park = cmdPark();
    runLog("parked", {{"ok", park["ok"]}, {"outcome", res == RunResult::Done ? "complete" : res == RunResult::Rain ? "rain" : "failed"}});
    if (p.v.contains("mail")) mailRunSummary(res == RunResult::Done ? "complete" : res == RunResult::Rain ? "stopped for rain" : "stopped after a failure", S["night"].toString());
    return {{"ok", res != RunResult::Failed}, {"outcome", res == RunResult::Done ? "complete" : res == RunResult::Rain ? "stopped: rain forecast" : "stopped: failure"}, {"blocks_run", g_run.blocksDone},
            {"switches", g_run.switches}, {"refocuses", g_run.refocuses}, {"log", g_run.logPath}, {"parked", park["ok"]}};
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// slew-altaz: closed-loop move of one axis with the GUI's rate slews. The mount reports Alt/Azm in radians; the rate unit is the GUI's (-9..9) and its
// sign convention is not documented, so the first seconds of motion are used to find the sign. Always ends by sending 0/0.
// ---------------------------------------------------------------------------------------------------------------------------------------
bool readAltAz(double* altDeg, double* azDeg)
{
    const QJsonObject m = call("mount_status", {{"fresh", true}}, 15000);
    if (!m["ok"].toBool()) return false;
    *altDeg = m["mount"].toObject()["Alt"].toDouble() * 180.0 / M_PI; *azDeg = m["mount"].toObject()["Azm"].toDouble() * 180.0 / M_PI;
    return true;
}
double azDiff(double target, double cur) { double d = std::fmod(target - cur, 360.0); if (d > 180) d -= 360; if (d < -180) d += 360; return d; }

QJsonObject moveAxis(bool isAlt, double target, double tol, int timeoutS)
{
    double alt, az;
    if (!readAltAz(&alt, &az)) return failure("cannot read the mount's Alt/Azm");
    auto err = [&](double a, double z) { return isAlt ? target - a : azDiff(target, z); };
    const double start = err(alt, az);
    if (std::abs(start) <= tol) return {{"ok", true}, {"moved", false}};
    int dir = start > 0 ? 1 : -1; bool flipped = false, checked = false;
    QElapsedTimer t; t.start(); QJsonObject last;
    auto rates = [&](int r) { return call("slew_rate", {{"alt_rate", isAlt ? r : 0}, {"azm_rate", isAlt ? 0 : r}}, 10000); };
    QJsonObject r = rates(dir * 5);
    if (!r["ok"].toBool()) return r;
    while (t.elapsed() < timeoutS * 1000) {
        sleepMs(1000);
        if (!readAltAz(&alt, &az)) continue;
        const double now = err(alt, az);
        if (std::abs(now) <= tol || (now * start < 0 && std::abs(now) < 20)) { break; }       // arrived, or passed it
        if (!checked && t.elapsed() > 4000) {
            checked = true;
            if (std::abs(now) > std::abs(start) + 0.5) { dir = -dir; flipped = true; rates(dir * 5); note({{"slew_altaz", "wrong direction; reversed"}}); }
            else if (std::abs(start) - std::abs(now) < 0.2) { rates(0); return {{"ok", false}, {"error", "the axis did not move at rate 5"}, {"alt_deg", alt}, {"az_deg", az}}; }
        }
        const int want = std::abs(now) > 25 ? 9 : std::abs(now) > 8 ? 5 : 2;                              // slow down on approach
        if (checked) rates(dir * want);
    }
    rates(0);
    readAltAz(&alt, &az);
    return {{"ok", std::abs(err(alt, az)) <= tol * 2}, {"axis", isAlt ? "alt" : "az"}, {"target_deg", target}, {"alt_deg", alt}, {"az_deg", az}, {"error_deg", err(alt, az)}, {"reversed", flipped}, {"seconds", int(t.elapsed() / 1000)}};
}

QJsonObject cmdSlewAltAz(const Parsed& p, const Cmd& c)
{
    if (!p.v.contains("az") && !p.v.contains("alt")) return failure("give --az and/or --alt");
    QJsonObject out{{"ok", true}};
    if (p.v.contains("alt")) { out["alt"] = moveAxis(true, num(p, c, "alt"), num(p, c, "tolerance"), int(num(p, c, "timeout"))); if (!out["alt"].toObject()["ok"].toBool()) out["ok"] = false; }
    if (out["ok"].toBool() && p.v.contains("az")) { out["az"] = moveAxis(false, num(p, c, "az"), num(p, c, "tolerance"), int(num(p, c, "timeout"))); if (!out["az"].toObject()["ok"].toBool()) out["ok"] = false; }
    double a, z; if (readAltAz(&a, &z)) { out["alt_deg"] = a; out["az_deg"] = z; }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// autorun: the dusk launcher, meant for cron every 5 minutes. Each tick does the next missing step and returns; once 'run' is started for a night
// it is left alone (it logs, reconnects and e-mails itself), and is relaunched only if its process died before parking, at most 3 times.
// ---------------------------------------------------------------------------------------------------------------------------------------
bool pidAlive(qint64 pid) {
#ifdef Q_OS_UNIX
    return pid > 0 && kill(pid_t(pid), 0) == 0;
#else
    Q_UNUSED(pid); return false;
#endif
}

QJsonObject cmdAutorun(const Parsed& p, const Cmd& c)
{
    const QString off = QDir::homePath() + "/.config/origin/autorun.off";
    if (QFile::exists(off)) return {{"ok", true}, {"acted", false}, {"note", "disabled: " + off + " exists"}};
    Planner::Options o = plannerOptions(p, c); o.date = Planner::currentNightDate(o.timeZone);
    o.minQuality = p.v.contains("min-quality") ? o.minQuality : 0.2;
    const QJsonObject S = Planner::schedule(o);
    if (!S["ok"].toBool()) return S;
    const QString night = S["night"].toString();
    const QDateTime now = QDateTime::currentDateTimeUtc(), darkStart = parseZ(S["dark_start"].toString()), darkEnd = parseZ(S["dark_end"].toString());
    auto idle = [&](const QString& why) { return QJsonObject{{"ok", true}, {"acted", false}, {"night", night}, {"note", why}}; };
    if (!p.v.contains("ignore-time") && !tooEarlyToSlew(str(p, "start-after", "dusk"), o.site).isEmpty()) return idle("too early for tonight: " + tooEarlyToSlew(str(p, "start-after", "dusk"), o.site));
    if (!p.v.contains("ignore-time") && now >= darkEnd.addSecs(-int(num(p, c, "stop-before-dawn")) * 60)) return idle("too late to start tonight");

    const QString statePath = originDataDir() + "/autorun_state.json";
    QJsonObject all; { QFile f(statePath); if (f.open(QIODevice::ReadOnly)) all = QJsonDocument::fromJson(f.readAll()).object(); }
    QJsonObject st = all[night].toObject();
    auto save = [&] { all[night] = st; QFile f(statePath); if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(all).toJson()); };
    const QString runLogPath = originDataDir() + "/run_" + QString(night).remove('-') + ".jsonl";
    if (st["launches"].toInt() > 0) {
        if (pidAlive(qint64(st["pid"].toDouble()))) return idle("run is in progress");
        QFile f(runLogPath); const bool parked = f.open(QIODevice::ReadOnly) && f.readAll().contains("\"decision\":\"parked\"");
        if (parked) return idle("tonight's run has finished");
        if (st["launches"].toInt() >= 3) return idle("run died 3 times tonight; giving up");
        note({{"autorun", "run process died before parking; relaunching"}});
    }
    if (S["plan"].toArray().isEmpty()) return idle("nothing observable tonight");
    if (p.v.contains("dry-run")) return {{"ok", true}, {"acted", false}, {"dry_run", true}, {"night", night}, {"would", "start the daemon if needed, connect, initialize if needed, launch run --unattended --mail"}, {"blocks", S["plan"].toArray().size()}};
    g_run = RunState(); g_run.o = o;
    if (rainSoon()) return idle("rain forecast within the hour");

    // daemon: one instance only. The old GUI app would hold the telescope's single client slot.
    if (!call("ping", {}, 3000)["ok"].toBool()) {
        QProcess pg; pg.start("pgrep", {"-x", "CelestronOrigin"}); pg.waitForFinished(3000);
        if (pg.exitCode() == 0) return idle("CelestronOrigin (the GUI app) is running; not starting a second instance");
        const QString dir = QDir::homePath() + "/.cache"; QDir().mkpath(dir);
        QProcess d; d.setProgram(QCoreApplication::applicationFilePath());
        d.setArguments({"serve", "--daemon", "--control-port", QString::number(g_port), "--connect", str(p, "telescope", "192.168.1.195"), "--pidfile", dir + "/origin-daemon.pid", "--log-file", dir + "/origin-daemon.log"});
        d.start(); d.waitForFinished(10000);
        sleepMs(4000);
        if (!call("ping", {}, 3000)["ok"].toBool()) return idle("could not start the daemon (see ~/.cache/origin-daemon.log)");
        note({{"autorun", "daemon started"}});
    }
    if (!call("status", {}, 10000)["connected"].toBool()) {
        call("connect", {{"host", str(p, "telescope", "192.168.1.195")}}, 20000);
        sleepMs(3000);
        if (!call("status", {}, 10000)["connected"].toBool()) return idle("telescope does not answer (not put out, or off)");
    }
    const QJsonObject mount = call("mount_status", {}, 15000)["mount"].toObject();
    if (!mount["IsAligned"].toBool()) {
        if (st["init_attempts"].toInt() >= 3) return idle("mount will not align (3 attempts); needs a look");
        st["init_attempts"] = st["init_attempts"].toInt() + 1; save();
        note({{"autorun", "initializing the mount"}});
        QJsonObject r = call("initialize", {{"lat_deg", o.site.latDeg}, {"lon_deg", o.site.lonDeg}, {"timezone", o.timeZone}, {"wait", true}, {"timeout", 300}}, 360000);
        if (!r["ok"].toBool() || r["outcome"].toString().toLower() == "failed") {
            // patchy cloud makes the star alignment fail: look for sky on the other side of the meridian and try again on the next tick
            double alt = 0, az = 0;
            if (readAltAz(&alt, &az)) {
                const double newAz = std::fmod(360.0 - az + 360.0, 360.0), newAlt = std::clamp(alt, 40.0, 65.0);
                note({{"autorun", "initialize failed; moving to the other side of the meridian"}, {"from_az", az}, {"to_az", newAz}, {"to_alt", newAlt}, {"why", r["summary"]}});
                const QJsonObject m1 = moveAxis(true, newAlt, 3, 90), m2 = moveAxis(false, newAz, 3, 150);
                st["moves"] = st["moves"].toInt() + 1; st["last_init_error"] = r["summary"]; save();
                return idle(QString("initialize failed (%1); moved to az %2 alt %3 (%4) to find clearer sky, will retry").arg(r["summary"].toString()).arg(newAz, 0, 'f', 0).arg(newAlt, 0, 'f', 0).arg(m1["ok"].toBool() && m2["ok"].toBool() ? "ok" : "move incomplete"));
            }
            return idle("initialize failed: " + r["error"].toString() + r["summary"].toString());
        }
    }

    // launch the supervisor, detached, logging to its own file
    QProcess run; run.setProgram(QCoreApplication::applicationFilePath());
    run.setArguments({"run", "--unattended", "--iso", str(p, "iso", "800"), "--max-exposure", str(p, "max-exposure", "20"), "--not-before", str(p, "start-after", "dusk"), "--port", QString::number(g_port)});
    if (!p.v.contains("no-mail")) run.setArguments(run.arguments() << "--mail");
    run.setStandardOutputFile(originDataDir() + "/autorun_" + QString(night).remove('-') + ".log", QIODevice::Append);
    run.setStandardErrorFile(originDataDir() + "/autorun_" + QString(night).remove('-') + ".log", QIODevice::Append);
    qint64 pid = 0;
    if (!run.startDetached(&pid)) return failure("could not launch run");
    st["launches"] = st["launches"].toInt() + 1; st["pid"] = double(pid); st["launched"] = now.toString(Qt::ISODate); save();
    const QJsonObject fc = g_wx;
    if (!p.v.contains("no-mail")) Mail::send(QString("[Origin] unattended run started (%1)").arg(night),
               QString("The telescope answered and the mount is aligned, so the unattended run has started for night %1 (launch %2, pid %3).\n\nBlocks planned: %4. Dark until %5 local.\nIt will e-mail a summary when it ends. To stop it: touch %6 and kill %3, or run: OriginHeadless park.\nLog: %7\n")
                   .arg(night).arg(st["launches"].toInt()).arg(pid).arg(S["plan"].toArray().size()).arg(darkEnd.toTimeZone(QTimeZone(o.timeZone.toUtf8())).toString("HH:mm"), off, originDataDir() + "/autorun_" + QString(night).remove('-') + ".log"));
    return {{"ok", true}, {"acted", true}, {"night", night}, {"launched_pid", double(pid)}, {"launches", st["launches"]}};
}

// ---------------------------------------------------------------- server

#ifdef Q_OS_UNIX
int g_sigPipe[2] = {-1, -1};
void onSignal(int) { const char b = 1; if (g_sigPipe[1] >= 0) { ssize_t r = write(g_sigPipe[1], &b, 1); (void)r; } }

void daemonize()
{
    if (fork() > 0) _exit(0);
    setsid();
    if (fork() > 0) _exit(0);
    umask(027);
    const int nul = open("/dev/null", O_RDWR);
    dup2(nul, 0); dup2(nul, 1); if (nul > 2) close(nul);       // stderr stays: it becomes the log when --log-file is not given and the caller redirected it
}
#endif

QFile* g_log = nullptr;
void logHandler(QtMsgType, const QMessageLogContext&, const QString& msg)
{
    const QByteArray line = QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toUtf8() + " " + msg.toUtf8() + "\n";
    if (g_log) { g_log->write(line); g_log->flush(); } else { std::fwrite(line.constData(), 1, size_t(line.size()), stderr); std::fflush(stderr); }
}

int serve(const Parsed& p, int& argc, char** argv)
{
    const QString pidfile = str(p, "pidfile");
    if (!pidfile.isEmpty()) {                                    // refuse a second instance: the telescope accepts one client and the port has one owner
        QFile f(pidfile);
        if (f.open(QIODevice::ReadOnly)) {
            const qint64 old = f.readAll().trimmed().toLongLong();
#ifdef Q_OS_UNIX
            if (old > 0 && kill(pid_t(old), 0) == 0) { std::puts(QJsonDocument(failure(QString("already running (pid %1, %2)").arg(old).arg(pidfile))).toJson(QJsonDocument::Compact).constData()); return 1; }
#endif
        }
    }
#ifdef Q_OS_UNIX
    if (p.v.contains("daemon")) daemonize();                     // before QCoreApplication exists: no threads to lose across the fork
#endif
    QCoreApplication app(argc, argv);
    QFile logFile;
    if (p.v.contains("log-file")) { logFile.setFileName(str(p, "log-file")); if (logFile.open(QIODevice::WriteOnly | QIODevice::Append)) g_log = &logFile; }
    qInstallMessageHandler(logHandler);
    const quint16 port = quint16(p.v.contains("control-port") ? str(p, "control-port").toInt() : 8765);
    OriginBackend backend;
    ControlServer server(&backend, p.v.contains("control-raw"));
    server.setCacheEnabled(!p.v.contains("no-cache"));
    if (!server.listen(port)) { qCritical() << "cannot listen on 127.0.0.1:" << port; std::puts(QJsonDocument(failure(QString("cannot listen on 127.0.0.1:%1 (is another instance running?)").arg(port))).toJson(QJsonDocument::Compact).constData()); return 1; }
    if (!pidfile.isEmpty()) { QFile f(pidfile); if (f.open(QIODevice::WriteOnly)) f.write(QByteArray::number(qint64(QCoreApplication::applicationPid())) + "\n"); }
#ifdef Q_OS_UNIX
    if (pipe(g_sigPipe) == 0) {
        QSocketNotifier notifier(g_sigPipe[0], QSocketNotifier::Read);
        QObject::connect(&notifier, &QSocketNotifier::activated, &app, [&]() { char b; ssize_t r = read(g_sigPipe[0], &b, 1); (void)r; qInfo() << "signal received: saving the stack and shutting down"; server.shutdown(); });
        struct sigaction sa; sa.sa_handler = onSignal; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_RESTART;
        sigaction(SIGTERM, &sa, nullptr); sigaction(SIGINT, &sa, nullptr); sigaction(SIGHUP, &sa, nullptr);
        signal(SIGPIPE, SIG_IGN);
#endif
        qInfo() << "OriginHeadless control server on 127.0.0.1:" << port << (p.v.contains("control-raw") ? "(raw commands enabled)" : "");
        const QString host = str(p, "connect");
        if (!host.isEmpty()) QTimer::singleShot(0, &app, [&]() { qInfo() << "connect" << host << backend.connectToTelescope(host, p.v.contains("telescope-port") ? str(p, "telescope-port").toInt() : 80); });
        const int rc = app.exec();
        if (!pidfile.isEmpty()) QFile::remove(pidfile);
        qInfo() << "stopped";
        return rc;
#ifdef Q_OS_UNIX
    }
    return 1;
#endif
}

void print(const QJsonObject& o, bool pretty)
{
    const QByteArray b = QJsonDocument(o).toJson(pretty ? QJsonDocument::Indented : QJsonDocument::Compact);
    std::fwrite(b.constData(), 1, size_t(b.size()), stdout); if (!pretty) std::fputc('\n', stdout); std::fflush(stdout);
}

}   // namespace

int main(int argc, char* argv[])
{
    QStringList args; for (int i = 1; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);
    g_port = qEnvironmentVariableIsSet("ORIGIN_CONTROL_PORT") ? qEnvironmentVariable("ORIGIN_CONTROL_PORT").toInt() : 8765;
    bool pretty = false;
    for (int i = 0; i < args.size();) {                    // global options may appear anywhere
        if (args[i] == "--pretty") { pretty = true; args.removeAt(i); }
        else if (args[i] == "--fresh") { g_fresh = true; args.removeAt(i); }
        else if (args[i] == "--port" && i + 1 < args.size()) { g_port = args[i + 1].toInt(); args.remove(i, 2); }
        else ++i;
    }
    if (args.removeAll("--headless")) { if (args.isEmpty() || args[0].startsWith("--")) args.prepend("serve"); }     // compatibility with CelestronOriginMonitor --headless
    if (args.isEmpty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") { print(catalogue(), true); return args.isEmpty() ? 2 : 0; }
    if (args[0] == "serve" && args.contains("--control-port")) { /* parsed below */ }

    const Cmd* cmd = nullptr; for (const Cmd& c : commands()) if (c.name == args[0]) cmd = &c;
    if (!cmd) { print({{"ok", false}, {"error", "unknown command: " + args[0]}, {"hint", "run: OriginHeadless commands"}}, pretty); return 2; }
    const QStringList rest = args.mid(1);
    if (rest.contains("--help")) { QJsonObject one; for (const QJsonValue& v : catalogue()["commands"].toArray()) if (v.toObject()["name"] == cmd->name) one = v.toObject(); one["ok"] = true; print(one, true); return 0; }
    const Parsed p = parseArgs(*cmd, rest);
    if (!p.error.isEmpty()) { print({{"ok", false}, {"error", p.error}, {"hint", "run: OriginHeadless " + cmd->name + " --help"}}, pretty); return 2; }

    if (cmd->name == "serve") return serve(p, argc, argv);
    QCoreApplication app(argc, argv);
    QJsonObject out;
    if (cmd->name == "commands") out = catalogue();
    else if (cmd->name == "schedule") out = cmdSchedule(p, *cmd);
    else if (cmd->name == "fieldrot") { Astro::Site s{num(p, *cmd, "lat"), num(p, *cmd, "lon")}; out = Planner::fieldRotation(num(p, *cmd, "ra-hours"), num(p, *cmd, "dec-deg"), s, num(p, *cmd, "tol"), num(p, *cmd, "hours")); }
    else if (cmd->name == "moon") out = cmdMoon(p, *cmd);
    else if (cmd->name == "briefing") out = cmdBriefing(p, *cmd);
    else if (cmd->name == "mail-login") out = Mail::login();
    else if (cmd->name == "mail-test") out = cmdMailTest(p, *cmd);
    else if (cmd->name == "backup") out = cmdBackup(p, *cmd);
    else if (cmd->name == "run") out = cmdRun(p, *cmd);
    else if (cmd->name == "autorun") out = cmdAutorun(p, *cmd);
    else if (cmd->name == "slew-altaz") out = cmdSlewAltAz(p, *cmd);
    else if (cmd->name == "now") out = cmdNow(p, *cmd);
    else if (cmd->name == "weather") out = cmdWeather(p, *cmd);
    else if (cmd->name == "go") out = cmdGo(p, *cmd);
    else if (cmd->name == "park") out = cmdPark();
    else if (cmd->name == "focus-sweep") out = cmdFocusSweep(p, *cmd);
    else {
        QJsonObject req{{"cmd", cmd->server}};
        for (const Arg& a : cmd->args) {
            if (p.v.contains(a.name)) req[keyOf(a)] = typed(a, p.v[a.name]);
            else if (!a.def.isEmpty() && a.type != 'n' && a.type != 'b' && (cmd->name == "initialize" || cmd->name == "send" || cmd->name == "snapshot" || cmd->name == "goto" || cmd->name == "focuser-move" || cmd->name == "events" || cmd->name == "focus" || cmd->name == "analysis"))
                req[keyOf(a)] = typed(a, a.def);
        }
        if (cmd->name == "sequence" && !req.contains("action")) req["action"] = "status";
        if (cmd->name == "target" && !req.contains("reset")) req.remove("reset");
        const int timeout = std::max(120, req["timeout"].toInt() + 60) * 1000;
        out = call(req, timeout);
        if (cmd->name == "initialize" && out.contains("summary")) std::fprintf(stderr, "%s: %s\n", qPrintable(out["outcome"].toString().toUpper()), qPrintable(out["summary"].toString()));
    }
    const bool text = cmd->name == "schedule" && p.v.contains("text") && out["ok"].toBool();
    if (text) { std::fputs(qPrintable(out["text"].toString()), stdout); return 0; }
    print(out, pretty);
    return out["ok"].toBool() ? 0 : 1;
}
