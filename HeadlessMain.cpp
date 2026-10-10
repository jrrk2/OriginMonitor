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

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
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
            O("lat", 'd', "site latitude, deg north", "52.2054"), O("lon", 'd', "site longitude, deg east", "0.1218"), O("top", 'i', "targets to list", "25"), O("text", 'b', "human-readable table instead of JSON")};
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
              O("max-frames", 'i', "stop after N frames"), O("for-minutes", 'd', "stop after N minutes"), O("until-local", 's', "stop at local HH:MM"), O("reason", 's', "stop reason")}, true);
        pass("send", "action", "Raw telescope command (server must run with --control-raw).", "send",
             {P("command", 's', "command name"), P("destination", 's', "destination, e.g. Camera"), O("params", 'j', "JSON object of parameters", "{}"), O("timeout", 'i', "seconds", "10")}, true);
        // --- orchestrated
        v.append({"focus-sweep", "action", "Move the focuser through positions around the current one, take frames, score the star HFR in-process and report the best position (restores the start unless --apply).", "", true, false,
                  {O("span", 'i', "steps either side of the current position", "600"), O("step", 'i', "step between positions", "100"), O("frames", 'i', "snapshots per position (median)", "3"),
                   O("dir", 's', "snapshot directory (default: temp)"), O("apply", 'b', "leave the focuser at the best position")}});
        v.append({"now", "planning", "Time, darkness, current target, minutes each candidate stays observable, and the planner's suggestion. Read-only.", "", false, false, plannerArgs()});
        {
            QList<Arg> a = {P("name", 's', "target key or name, e.g. M31"), O("exposure", 'd', "seconds to set after the slew", "10"), O("iso", 'i', "ISO to set after the slew", "800"),
                            O("not-before", 's', "local HH:MM; refuse to slew earlier (the telescope is put out at 20:00)", "20:00")};
            v.append({"go", "planning", "Slew to a target that is observable now: set site and target, goto, wait for centring/autofocus, re-send exposure and ISO.", "", true, false, a + plannerArgs()});
        }
        v.append({"park", "planning", "Save the stack and slew to Polaris (safe park).", "", true, false, {}});
        v.append({"schedule", "planning", "Plan one night: observable windows against the local horizon, Sun and Moon, plus a greedy one-target-at-a-time plan.", "", false, true, plannerArgs()});
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

Planner::Options plannerOptions(const Parsed& p, const Cmd& c)
{
    Planner::Options o;
    if (p.v.contains("date")) o.date = QDate::fromString(str(p, "date"), Qt::ISODate);
    o.site.latDeg = num(p, c, "lat"); o.site.lonDeg = num(p, c, "lon");
    o.horizonFile = p.v.contains("horizon") ? str(p, "horizon") : Planner::defaultHorizonFile();
    if (p.v.contains("sky-map")) o.skyMapFile = originDataDir() + "/sky_map.json";
    o.minAlt = num(p, c, "min-alt"); o.sunAlt = num(p, c, "sun-alt"); o.moonSep = num(p, c, "moon-sep");
    o.minBlockMin = num(p, c, "min-block"); o.maxBlockMin = num(p, c, "max-block"); o.stepMin = num(p, c, "step"); o.top = int(num(p, c, "top"));
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
    if (p.v.contains("text")) s["text"] = Planner::scheduleText(s, int(num(p, c, "top")));
    else { QJsonArray t = s["targets"].toArray(), cut; for (int i = 0; i < t.size() && i < int(num(p, c, "top")); ++i) cut.append(t[i]); s["targets"] = cut; }
    return s;
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

QJsonObject cmdGo(const Parsed& p, const Cmd& c)
{
    const QStringList hm = str(p, "not-before", "20:00").split(':');
    const QDateTime loc = QDateTime::currentDateTime();
    const QDateTime start(loc.date(), QTime(hm.value(0).toInt(), hm.value(1).toInt()));
    if (loc < start && loc.time().hour() >= 6) return failure(QString("too early: not before %1 local (%2 min to go); refusing to slew").arg(str(p, "not-before", "20:00")).arg(loc.secsTo(start) / 60));
    Planner::Options o = plannerOptions(p, c); o.date = Planner::currentNightDate(o.timeZone);
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
    else if (cmd->name == "now") out = cmdNow(p, *cmd);
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
