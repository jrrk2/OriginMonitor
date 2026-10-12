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
#include "FlatAnalyzer.hpp"
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
#include <random>
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
            O("boost", 's', "priority boosts, e.g. M1=5,M31=2 (added to ~/.config/origin/target_boost.json); a boosted target wins whenever it is observable"),
            O("novelty", 'o', "on|off prefer targets not imaged before: a target with a saved stack in <data dir>/stacks loses priority (-4, or -5 within 3 days), a new one gains +4", "on"),
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
                  {O("control-port", 'i', "TCP port to listen on (default 8765 or $ORIGIN_CONTROL_PORT)", "8765"), O("connect", 's', "telescope host to connect to at start-up"), O("telescope-port", 'i', "telescope port (default 80; other values are for simulators)", "80"), O("control-raw", 'b', "enable the 'send' raw-command passthrough"), O("no-cache", 'b', "disable the read-only response cache (default: status/mount/focuser/focus/analysis/sky-map replies are cached for 1-30 s)"),
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
        pass("snapshot", "action", "Take one frame, download it and report the path. With --exposure the values go straight into the capture request (without it the last values the camera confirmed are used, which lag a request behind a set).", "snapshot", {O("dir", 's', "output directory (default: data dir)"), O("timeout", 'i', "seconds", "30"), O("exposure", 'd', "seconds"), O("iso", 'i', "ISO", "800"), O("binning", 'i', "binning", "1")}, true);
        pass("goto", "action", "Slew to J2000 coordinates and wait for the mount to finish (the firmware then centres and autofocuses for about 50 s).", "goto",
             {P("ra-hours", 'd', "J2000 RA, hours 0..24", true, "ra_hours"), P("dec-deg", 'd', "J2000 Dec, degrees", true, "dec_deg"), O("timeout", 'i', "seconds", "180")}, true);
        pass("initialize", "action", "RunInitialize: the mount homes and aligns. Waits and reports the outcome.", "initialize",
             {O("lat", 'd', "site latitude", "52.2054", "lat_deg"), O("lon", 'd', "site longitude", "0.1218", "lon_deg"), O("tz", 's', "IANA time zone", "Europe/London", "timezone"),
              O("fake", 'b', "FakeInitialize"), O("no-wait", 'n', "return as soon as the telescope accepts", QString(), "wait"), O("timeout", 'i', "seconds", "300")}, true);
        pass("unpark", "action", "Raise the tube with the GUI's unpark (AltRate +9, no azimuth motion) until the telescope's tilt sensor reads --alt (default 60 deg), then stop. Does NOT initialise. Refuses to move without a fresh tilt reading; stops if the reading goes stale or after --timeout.", "unpark",
             {O("alt", 'd', "target tilt altitude, deg", "60", "alt_deg"), O("rate", 'i', "AltRate 1..9", "9"), O("timeout", 'i', "seconds", "90"), O("max-age", 'd', "oldest acceptable tilt reading, s", "5", "max_age")}, true);
        pass("solve", "action", "Plate solve (astrometry.net, -z2): take a snapshot (works before the mount has aligned) or use --file, and report RA/Dec, rotation, and the altitude/azimuth of the tube checked against the tilt sensor.", "solve",
             {O("file", 's', "solve this FITS file instead of taking a snapshot"), O("lat", 'd', "site latitude", "52.2054", "lat_deg"), O("lon", 'd', "site longitude", "0.1218", "lon_deg"),
              O("cpulimit", 'i', "seconds the solver may use", "60"), O("force", 'b', "run solve-field even if the frame statistics say it is hopeless"), O("exposure", 'd', "seconds for the snapshot (default: the camera's current setting)"), O("iso", 'i', "ISO for the snapshot"), O("binning", 'i', "binning for the snapshot"), O("index-dir", 's', "astrometry.net index files (default $ORIGIN_INDEX_DIR or ~/astrometry_indices)", QString(), "index_dir"), O("downsample", 'i', "solve-field -z", "2")}, true);
        pass("frame-stats", "analysis", "Statistics of one raw frame (a fresh snapshot, or --file): percentiles, histogram, saturated fraction, background, noise, star count, the sky rate in ADU/s above black (twilight brightness), a verdict (ok, too_bright, too_short, few_stars) and the exposure that would put the sky about 6000 ADU above black. Used by solve to skip hopeless frames.", "frame_stats",
             {O("file", 's', "analyse this FITS file instead of taking a snapshot"), O("exposure", 'd', "seconds for the snapshot"), O("iso", 'i', "ISO", "800"), O("binning", 'i', "binning", "1"), O("bins", 'i', "histogram bins (2-256)", "16"), O("max-exposure", 'd', "longest exposure to suggest, s", "6", "max_exposure"), O("target-adu", 'd', "sky level above black the suggested exposure should give, ADU", "6000", "target_adu")}, true);
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
                  {O("span", 'i', "steps either side of the current position", "600"), O("step", 'i', "step between positions", "100"), O("exposure", 'd', "seconds per frame", "4"), O("frames", 'i', "snapshots per position (median)", "3"),
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
        v.append({"sky-report", "planning", "Tell the forecast what the sky looks like now: percent of the sky covered by low/mid cloud (0 clear .. 100 overcast). Counts as an observation at the site (heavier than any airport METAR) for 90 minutes and steers the model blend; its influence fades over about 3 hours.", "", false, true,
                  {O("cover", 'd', "cloud cover now, percent")}});
        v.append({"survey", "action", "Twilight obstruction survey: while waiting for darkness, point at a sample of the places the night's plan will use (places the obstruction map has never seen first, then random) (block start, middle, end, backups), take a very short frame at each, and judge whether the view is blocked (trees, roofs) from the sky level and the spread of the frame. Needs an aligned mount and the Sun below --sun-limit. Records evidence in the learned obstruction map, writes survey_<night>.json and a picture per sample.",
                  "", true, false, [] { QList<Arg> a = {O("samples", 'i', "places to sample", "8"), O("target", 's', "instead of random places, sample this target's own track across its window tonight (e.g. M1)"), O("seed", 'i', "random seed for the order within equally-visited places (default: the night's date)"), O("sun-limit", 'd', "Sun altitude it must be below, deg", "-7"), O("max-minutes", 'd', "stop after this long", "35"),
                                                   O("dry-run", 'b', "list the sample points, do nothing"), O("iso", 'i', "ISO", "800")}; return a + plannerArgs(); }()});
        v.append({"flats", "action", "Take flat frames of a uniformly lit sky (an overcast deck, or twilight near the zenith) at the pointing the tube has now and analyse them: vignetting, dust shadows, colour, gradient, noise. Adapts the exposure to put the sky --target-adu above black, writes master_flat_<tag>.fits and two pictures to <data dir>/flats/. Does not move the telescope.",
                  "", true, false, {O("frames", 'i', "frames to take", "16"), O("iso", 'i', "ISO (use the ISO of the lights)", "800"), O("target-adu", 'd', "sky level above black to aim for, ADU", "20000"), O("max-exposure", 'd', "longest exposure, s", "2"), O("tag", 's', "name for the outputs (default: iso and time)"), O("no-keep", 'b', "delete the raw frames after the analysis")}});
        v.append({"flat-analyze", "analysis", "Analyse existing raw flat frames (see flats): --dir with snapshot_*.fits, or --files a.fits,b.fits.", "", false, true,
                  {O("dir", 's', "directory of raw FITS flat frames"), O("files", 's', "comma-separated FITS files"), O("tag", 's', "name for the outputs", "analysis")}});
        v.append({"goto-altaz", "action", "Slew to a horizontal position by converting it to J2000 RA/Dec for the current time and using the ordinary goto (bounded by the mount's own limits). Needs an aligned (or fake-initialized) mount.", "", true, false,
                  {O("az", 'd', "azimuth, deg (0=N, 90=E, 180=S, 270=W)"), O("alt", 'd', "altitude, deg", "50"), O("lat", 'd', "site latitude", "52.2054"), O("lon", 'd', "site longitude", "0.1218"), O("timeout", 'i', "seconds", "240")}});
        v.append({"forecast-accuracy", "planning", "How good the weather forecast has been: compares the archived dusk forecast of each night with the frames the sequencer took (usable fraction per hour). Writes forecast_accuracy.csv/.json in the data dir; reports bias (positive = the sky was better than forecast).", "", false, true, {}});
        v.append({"autorun", "planning", "Dusk launcher for cron (every 5 minutes): once per night, if the telescope is out and reachable, starts the daemon, connects, initializes the mount if needed and starts 'run --unattended --mail' in the background. Does nothing (quietly) when the telescope does not answer, when nothing is observable, when rain is forecast, or when ~/.config/origin/autorun.off exists. Slews the telescope: only schedule it if that is wanted.", "", true, false,
                  [] { QList<Arg> a = {O("telescope", 's', "telescope host", "192.168.1.195"), O("start-after", 's', "\"dusk\" (autorun begins when the Sun is below --probe-sun-alt; the run itself waits for -12 deg) or a local HH:MM; never act before this", "dusk"), O("probe-sun-alt", 'd', "Sun altitude below which autorun starts: raises the tube, probes with plate solves (exposure scaled to the sky brightness) and initializes; the run still waits for -12", "-7"), O("stop-before-dawn", 'd', "minutes before the end of darkness after which no run is started", "60"),
                                       O("dry-run", 'b', "say what would be done, do nothing"), O("init-fallback", 'o', "off | fake-init: after a failed initialize, fake-initialize, goto sky on the other side of the meridian and retry (UNTESTED on the real mount; off by default)", "off"), O("ignore-time", 'b', "skip the evening/dawn time gates (testing)"), O("no-mail", 'b', "send no e-mails (testing)"), O("iso", 'i', "ISO for the run", "800"), O("max-exposure", 'd', "longest exposure for the run, s", "20")};
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
    // the learned obstruction and glare bins are always used by the unattended commands (run, autorun, survey); for the others they are opt-in
    if (p.v.contains("sky-map") || c.name == "run" || c.name == "autorun" || c.name == "survey") o.skyMapFile = originDataDir() + "/sky_map.json";
    o.minAlt = num(p, c, "min-alt"); o.sunAlt = num(p, c, "sun-alt"); o.moonSep = num(p, c, "moon-sep");
    o.minBlockMin = num(p, c, "min-block"); o.maxBlockMin = num(p, c, "max-block"); o.stepMin = num(p, c, "step"); o.top = int(num(p, c, "top"));
    Planner::setStacksDir(str(p, "novelty", "on") == "off" ? QString() : originDataDir() + "/stacks");     // prefer targets not imaged before
    {   // wish list: ~/.config/origin/target_boost.json {"M1": 5} and/or --boost M1=5,M31=2
        QMap<QString, int> boosts;
        QFile bf(QDir::homePath() + "/.config/origin/target_boost.json");
        if (bf.open(QIODevice::ReadOnly)) { const QJsonObject b = QJsonDocument::fromJson(bf.readAll()).object(); for (auto it = b.begin(); it != b.end(); ++it) boosts[it.key()] = it.value().toInt(); }
        if (p.v.contains("boost")) for (const QString& kv : str(p, "boost").split(',', Qt::SkipEmptyParts)) { const QStringList a = kv.split('='); if (a.size() == 2) boosts[a[0].trimmed()] = a[1].toInt(); }
        Planner::setBoosts(boosts);
    }
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
// Forecast track record. The dusk forecast for each night is archived (<data dir>/forecasts/<night>.json, the first one saved between sunset-1h and
// 22:00, i.e. what the decision to put the telescope out is based on). It is compared hour by hour with what the sequencer measured that night
// (sequence_<date>.jsonl: 'clear'/'hazy' frames are usable, 'cloudy'/'patchy' are not; 'unreadable' and obstruction frames say nothing about the sky),
// weighted by the time each frame covers so sparse probing in cloud does not flatter the forecast. Results: forecast_accuracy.csv (one row per hour)
// and forecast_accuracy.json (summary). Positive bias = the forecast was too pessimistic.
// ---------------------------------------------------------------------------------------------------------------------------------------
void archiveForecast(const QString& night, const QJsonArray& hours)
{
    const QString dir = originDataDir() + "/forecasts"; QDir().mkpath(dir);
    QFile f(dir + "/" + night + ".json");
    if (f.exists() || hours.isEmpty()) return;
    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(QJsonObject{{"night", night}, {"saved", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}, {"hours", hours}}).toJson());
}

QJsonObject forecastAccuracy()
{
    const QString dir = originDataDir();
    QJsonArray rows; QStringList csv{"night,hour_utc,forecast_quality,cloud_low,cloud_mid,cloud_high,observed_usable,covered_min"};
    QJsonArray nights;
    for (const QFileInfo& fi : QDir(dir + "/forecasts").entryInfoList({"*.json"}, QDir::Files, QDir::Name)) {
        QFile ff(fi.absoluteFilePath()); if (!ff.open(QIODevice::ReadOnly)) continue;
        const QJsonObject fc = QJsonDocument::fromJson(ff.readAll()).object(); const QString night = fc["night"].toString();
        const QDate d0 = QDate::fromString(night, Qt::ISODate); if (!d0.isValid()) continue;
        struct Fr { QDateTime t; bool good; }; QList<Fr> frames;
        for (const QDate& d : {d0, d0.addDays(1)}) {
            QFile sf(dir + "/sequence_" + d.toString("yyyyMMdd") + ".jsonl"); if (!sf.open(QIODevice::ReadOnly)) continue;
            while (!sf.atEnd()) {
                const QJsonObject o = QJsonDocument::fromJson(sf.readLine()).object(); const QString v = o["verdict"].toString();
                if (v == "unreadable" || v.isEmpty() || o["cause"].toString() == "obstruction") continue;
                QDateTime t = QDateTime::fromString(o["time"].toString(), Qt::ISODateWithMs); if (!t.isValid()) continue;
                frames.append({t.toUTC(), v == "clear" || v == "hazy"});
            }
        }
        std::sort(frames.begin(), frames.end(), [](const Fr& a, const Fr& b) { return a.t < b.t; });
        QMap<qint64, QPair<double, double>> perHour;                                   // hour start (epoch s) -> {good s, bad s}
        for (int i = 0; i < frames.size(); ++i) {
            const double w = i + 1 < frames.size() ? std::min<double>(frames[i].t.secsTo(frames[i + 1].t), 600.0) : 30.0;
            const qint64 hs = frames[i].t.toSecsSinceEpoch() / 3600 * 3600;
            (frames[i].good ? perHour[hs].first : perHour[hs].second) += w;
        }
        int used = 0;
        for (const QJsonValue& hv : fc["hours"].toArray()) {
            const QJsonObject h = hv.toObject(); const QDateTime a = parseZ(h["utc"].toString().left(19) + "Z");
            const auto it = perHour.constFind(a.toSecsSinceEpoch()); if (it == perHour.constEnd()) continue;
            const double cov = it->first + it->second; if (cov < 900) continue;       // at least 15 minutes of evidence
            const double obs = it->first / cov;
            rows.append(QJsonObject{{"night", night}, {"hour_utc", a.toString("HH:mm")}, {"forecast", h.contains("q_model") ? h["q_model"] : h["quality"]}, {"observed", std::round(obs * 100) / 100}, {"covered_min", int(cov / 60)}, {"models", h["models"]}});
            csv << QString("%1,%2,%3,%4,%5,%6,%7,%8").arg(night, a.toString(Qt::ISODate)).arg((h.contains("q_model") ? h["q_model"] : h["quality"]).toDouble()).arg(h["low"].toDouble()).arg(h["mid"].toDouble()).arg(h["high"].toDouble()).arg(obs, 0, 'f', 2).arg(int(cov / 60));
            ++used;
        }
        nights.append(QJsonObject{{"night", night}, {"hours_compared", used}});
    }
    int n = 0, poor = 0, poorUsable = 0, good = 0, goodUsable = 0; double sp = 0, so = 0, ae = 0;
    QMap<QString, QVector<QPair<double, double>>> perModel;                          // model -> (forecast, observed)

    for (const QJsonValue& v : rows) {
        const QJsonObject r = v.toObject(); const double f = r["forecast"].toDouble(), o = r["observed"].toDouble();
        ++n; sp += f; so += o; ae += std::abs(o - f);
        const QJsonObject mo = r["models"].toObject(); for (auto it = mo.constBegin(); it != mo.constEnd(); ++it) perModel[it.key()].append({it.value().toDouble(), o});
        if (f < 0.5) { ++poor; if (o >= 0.5) ++poorUsable; } else { ++good; if (o >= 0.5) ++goodUsable; }
    }
    QFile cf(dir + "/forecast_accuracy.csv"); if (cf.open(QIODevice::WriteOnly | QIODevice::Truncate)) cf.write((csv.join("\n") + "\n").toUtf8());
    QJsonObject r{{"ok", true}, {"hours_compared", n}, {"nights", nights}, {"hours_forecast_poor", poor}, {"poor_but_usable", poorUsable}, {"hours_forecast_good", good}, {"good_and_usable", goodUsable}};
    if (n) { r["mean_forecast"] = std::round(sp / n * 100) / 100; r["mean_observed"] = std::round(so / n * 100) / 100; r["bias_pessimism"] = std::round((so - sp) / n * 100) / 100; r["mean_abs_error"] = std::round(ae / n * 100) / 100; }
    QList<QJsonObject> byModelL;
    for (auto it = perModel.constBegin(); it != perModel.constEnd(); ++it) {
        double mf = 0, mo2 = 0, mae = 0; for (const auto& pr : it.value()) { mf += pr.first; mo2 += pr.second; mae += std::abs(pr.second - pr.first); }
        const int k = it.value().size();
        byModelL.append(QJsonObject{{"model", it.key()}, {"hours", k}, {"mean_abs_error", std::round(mae / k * 100) / 100}, {"bias_pessimism", std::round((mo2 - mf) / k * 100) / 100}});
    }
    std::sort(byModelL.begin(), byModelL.end(), [](const QJsonObject& a, const QJsonObject& b) { return a["mean_abs_error"].toDouble() < b["mean_abs_error"].toDouble(); });
    QJsonArray byModel; for (const QJsonObject& m : byModelL) byModel.append(m);
    if (!byModel.isEmpty()) r["by_model_best_first"] = byModel;
    r["note"] = "only hours with at least 15 minutes of frames count, and those are mostly hours when we chose to shoot; positive bias_pessimism means the sky was better than forecast";
    QFile jf(dir + "/forecast_accuracy.json"); if (jf.open(QIODevice::WriteOnly | QIODevice::Truncate)) jf.write(QJsonDocument(QJsonObject{{"summary", r}, {"hours", rows}}).toJson());
    r["hours"] = rows;
    return r;
}

QString trackRecordLine(const QJsonObject& a)
{
    const int n = a["hours_compared"].toInt();
    if (n < 8) return QString("Forecast track record: only %1 observed hour(s) compared so far; too few to say how reliable the forecast is here (the dusk forecast is archived each night and compared with the frames the telescope takes).").arg(n);
    QString s = QString("Forecast track record (%1 observed hours over %2 night(s)): the sky was on average %3 %4 than forecast (mean forecast quality %5, observed usable fraction %6).")
                    .arg(n).arg(a["nights"].toArray().size()).arg(std::abs(a["bias_pessimism"].toDouble()), 0, 'f', 2).arg(a["bias_pessimism"].toDouble() >= 0 ? "better" : "worse").arg(a["mean_forecast"].toDouble(), 0, 'f', 2).arg(a["mean_observed"].toDouble(), 0, 'f', 2);
    if (a["hours_forecast_poor"].toInt() > 0) s += QString(" Of %1 hours forecast poor, %2 were actually usable.").arg(a["hours_forecast_poor"].toInt()).arg(a["poor_but_usable"].toInt());
    const QJsonArray bm = a["by_model_best_first"].toArray();
    if (!bm.isEmpty()) s += QString(" Closest model so far: %1 (mean error %2).").arg(bm[0].toObject()["model"].toString()).arg(bm[0].toObject()["mean_abs_error"].toDouble(), 0, 'f', 2);
    if (a["hours_forecast_good"].toInt() > 0) s += QString(" Of %1 hours forecast good, %2 were.").arg(a["hours_forecast_good"].toInt()).arg(a["good_and_usable"].toInt());
    return s;
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
    { const QStringList nh0 = str(p, "night-at", "22:00").split(':');
      const QDateTime cutoff = QDateTime(o.date, QTime(nh0.value(0).toInt(), nh0.value(1).toInt()), zone).toUTC();
      if (now >= sunset.addSecs(-3600) && now <= std::max(cutoff, darkStart)) archiveForecast(night, S["weather"].toObject()["hours"].toArray()); }

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
        const QString verdict = best >= 90 ? "WORTH PUTTING OUT" : best >= 40 ? "MARGINAL" : "POOR FORECAST";
        QString when; for (const Win& w : wins) when += (when.isEmpty() ? "" : ", ") + loc(w.a) + "-" + loc(w.b);
        subject = QString("[Origin] Dusk: %1%2").arg(verdict, wins.isEmpty() ? " - no usable hours forecast (but it has been wrong before)" : " - clear " + when);
        body = QString("%1 (night of %2)\n\nVERDICT: %3\n").arg(verdict, title, best >= 90 ? "put the telescope out." : best >= 40 ? "only short gaps are forecast; worth trying." : "the forecast is poor, but forecasts here have often been too pessimistic, so it is not a reason to stay in. Trying costs only the put-out; if it is out and answering, autorun will image whatever breaks.");
        body += QString("Sunset %1, astronomical dark %2 to %3. %4 of the %5 dark minutes are forecast usable; longest clear stretch %6 min.\n").arg(loc(sunset), loc(darkStart), loc(darkEnd)).arg(total).arg(int(darkStart.secsTo(darkEnd) / 60)).arg(best);
        if (wx.contains("rain_from")) body += "Rain is forecast from " + locJ(wx["rain_from"]) + ".\n";
        { const QJsonObject acc = forecastAccuracy(); body += "\n" + trackRecordLine(acc) + "\n"; }
        body += "\nUsable windows (local): " + (when.isEmpty() ? "none" : when) + "\n\nHour by hour (local): mean cloud (low/mid/high) of 7 models, rain chance, blended quality (range across models)\n";
        for (const QJsonValue& hv : wx["hours"].toArray()) {
            const QJsonObject h = hv.toObject();
            body += QString("  %1  %2% (%3/%4/%5)  rain %6%  %7 (models %8-%9)  %10\n").arg(loc(parseZ(h["utc"].toString().left(19) + "Z")), QString::number(h["cloud"].toDouble(), 'f', 0).rightJustified(3), QString::number(h["low"].toDouble(), 'f', 0),
                         QString::number(h["mid"].toDouble(), 'f', 0), QString::number(h["high"].toDouble(), 'f', 0), QString::number(h["precip_prob"].toDouble(), 'f', 0), QString::number(h["quality"].toDouble(), 'f', 2),
                         QString::number(h["q_min"].toDouble(), 'f', 2), QString::number(h["q_max"].toDouble(), 'f', 2), h["quality"].toDouble() >= o.minQuality ? "usable" : "");
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
            // explicit exposure: the snapshot then re-sends the capture parameters, otherwise the frame after a focuser move comes back as a PNG
            const QJsonObject sn = call("snapshot", {{"dir", dir}, {"exposure", num(p, c, "exposure")}, {"iso", 800}, {"binning", 1}}, 60000);
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
    const QJsonObject back = call("focuser_move", {{"position", final}, {"max_step", 2 * span + step + 200}}, 90000);          // the sweep ends at the far edge: the way back is up to 2 spans
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
    int trailedFrames = 0, regotos = 0; QDateTime cloudSince, blockCloudStart; double cloudMinTotal = 0; QDateTime lastTick = QDateTime::currentDateTimeUtc();
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
        // trailed stars are the mount's fault, not cloud: re-centre with a fresh goto (at most 3 times per block) instead of counting cloud time or leaving for a backup
        const QString lastV = s["last_verdict"].toString();
        // defocus is only worth a re-centre (which repeats the telescope's autofocus) while the sky is clear enough for that autofocus to see stars
        bool defocusFix = false;
        if (lastV == "defocused") { const QJsonObject nc = Weather::metarNowcast(g_run.o.site.latDeg, g_run.o.site.lonDeg, originDataDir(), 10, nullptr); defocusFix = !nc["ok"].toBool() || nc["cover_pct"].toDouble() < 60; }
        if (lastV == "trailed" || defocusFix) {
            if (++trailedFrames >= (lastV == "trailed" ? 2 : 3) && regotos < 3) {
                call("sequence", {{"action", "stop"}, {"reason", "trailed stars: re-centring"}});
                const QJsonObject g = call("goto", {{"ra_hours", t["ra_hours"]}, {"dec_deg", t["dec_deg"]}, {"timeout", 240}}, 300000);
                runLog("regoto", {{"target", t["key"]}, {"why", lastV == "trailed" ? "trailed stars (mount not tracking correctly)" : "defocused stars (the autofocus after the last goto ran without stars)"}, {"ok", g["ok"]}, {"count", ++regotos}});
                trailedFrames = 0; call("set", {{"exposure", exposure}, {"iso", iso}}); startSeq(exposure, iso, endUtc, true); lastTick = now; continue;
            }
        } else trailedFrames = 0;
        const bool probing = s["state"].toString() == "probing" && lastV != "trailed" && lastV != "defocused";
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
    body += "\n" + trackRecordLine(forecastAccuracy()) + "\n";
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
// goto-altaz: horizontal position -> J2000 RA/Dec for now -> the ordinary, bounded goto. (No rate slews: they cannot be bounded.)
// ---------------------------------------------------------------------------------------------------------------------------------------
bool readAltAz(double* altDeg, double* azDeg)
{
    const QJsonObject m = call("mount_status", {{"fresh", true}}, 15000);
    if (!m["ok"].toBool()) return false;
    *altDeg = m["mount"].toObject()["Alt"].toDouble() * 180.0 / M_PI; *azDeg = m["mount"].toObject()["Azm"].toDouble() * 180.0 / M_PI;
    return true;
}

Astro::Equatorial altAzToJ2000(double altDeg, double azDeg, const Astro::Site& site, const QDateTime& utc)
{
    const double r = M_PI / 180.0, a = altDeg * r, A = azDeg * r, phi = site.latDeg * r;
    const double sinDec = std::sin(a) * std::sin(phi) + std::cos(a) * std::cos(phi) * std::cos(A), dec = std::asin(std::clamp(sinDec, -1.0, 1.0));
    const double H = std::atan2(-std::sin(A) * std::cos(a), std::sin(a) * std::cos(phi) - std::cos(a) * std::sin(phi) * std::cos(A));
    const double jd = Astro::julianDate(utc);
    double ra = std::fmod(Astro::lstDegrees(jd, site.lonDeg) - H / r + 720.0, 360.0);
    return Astro::precess({ra / 15.0, dec / r}, jd, 2451545.0);
}

QJsonObject gotoAltAz(double alt, double az, const Astro::Site& site, int timeoutS)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const Astro::Equatorial eq = altAzToJ2000(alt, az, site, now);
    const Astro::AltAz chk = Astro::altAz(eq, site, now);               // round trip: must land on the requested position
    if (std::abs(chk.alt - alt) > 0.1 || std::abs(std::remainder(chk.az - az, 360.0)) > 0.1) return failure(QString("alt/az conversion check failed (%1,%2 -> %3,%4)").arg(alt).arg(az).arg(chk.alt).arg(chk.az));
    if (alt < 20) return failure("refusing a target below 20 deg altitude");
    QJsonObject r = call("goto", {{"ra_hours", eq.raHours}, {"dec_deg", eq.decDeg}, {"timeout", timeoutS}}, (timeoutS + 60) * 1000);
    r["requested_alt"] = alt; r["requested_az"] = az; r["ra_hours"] = eq.raHours; r["dec_deg"] = eq.decDeg;
    return r;
}

QJsonObject cmdGotoAltAz(const Parsed& p, const Cmd& c)
{
    if (!p.v.contains("az")) return failure("give --az (and optionally --alt)");
    return gotoAltAz(num(p, c, "alt"), num(p, c, "az"), Astro::Site{num(p, c, "lat"), num(p, c, "lon")}, int(num(p, c, "timeout")));
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// flats: frames of a uniformly lit sky at the current pointing, exposure adapted to a fixed signal level, then analysed (FlatAnalyzer). No motion.
// ---------------------------------------------------------------------------------------------------------------------------------------
QJsonObject cmdFlats(const Parsed& p, const Cmd& c)
{
    if (!call("status", {}, 10000)["connected"].toBool()) return failure("the daemon is not connected to the telescope");
    if (call("sequence", {{"action", "status"}}, 10000)["status"].toObject()["active"].toBool()) return failure("a sequence is running");
    const int n = std::max(2, int(num(p, c, "frames"))), iso = int(num(p, c, "iso")); const double target = num(p, c, "target-adu"), maxExp = num(p, c, "max-exposure");
    const QString tag = p.v.contains("tag") ? str(p, "tag") : QString("iso%1_%2").arg(iso).arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmm"));
    const QString outDir = originDataDir() + "/flats", rawDir = outDir + "/raw_" + tag; QDir().mkpath(rawDir);
    call("manual", {}, 15000);
    double expo = 0.001; QJsonObject fs;
    for (int k = 0; k < 8; ++k) {                                       // find the exposure that puts the sky about `target` ADU above black
        fs = call("frame_stats", {{"exposure", expo}, {"iso", iso}, {"binning", 1}, {"target_adu", target}, {"max_exposure", maxExp}, {"dir", rawDir}}, 90000);
        if (!fs["ok"].toBool()) return failure("frame statistics failed: " + fs["error"].toString());
        const double next = fs["suggested_exposure_s"].toDouble(expo);
        note({{"flats", "finding the exposure"}, {"exposure_s", expo}, {"median_adu", fs["median_adu"]}, {"next_s", next}});
        if (std::abs(std::log10(next / expo)) < 0.08) break;
        if (fs["median_adu"].toDouble() - 4985 < 200 && expo >= maxExp * 0.99) return failure(QString("too dark for flats even at %1 s (median %2 ADU)").arg(expo).arg(fs["median_adu"].toDouble()));
        expo = next;
    }
    QStringList files; QJsonArray log;
    for (int i = 0; i < n; ++i) {
        const QJsonObject s = call("snapshot", {{"dir", rawDir}, {"exposure", expo}, {"iso", iso}, {"binning", 1}, {"timeout", 40}}, 90000);
        if (!s["ok"].toBool()) { note({{"flats", "snapshot failed"}, {"error", s["error"]}}); continue; }
        files << s["path"].toString();
        const QJsonObject st = call("frame_stats", {{"file", s["path"]}, {"exposure", expo}, {"target_adu", target}, {"max_exposure", maxExp}}, 30000);
        log.append(QJsonObject{{"file", QFileInfo(s["path"].toString()).fileName()}, {"exposure_s", expo}, {"median_adu", st["median_adu"]}, {"saturated", st["saturated_fraction"]}});
        const double next = st["suggested_exposure_s"].toDouble(expo);
        if (std::abs(std::log10(next / expo)) > 0.1) expo = next;        // the cloud brightness drifts: follow it (the analysis normalises each frame)
    }
    QJsonObject r = analyseFlats(files, outDir, tag);
    r["tag"] = tag; r["raw_dir"] = rawDir; r["frame_log"] = log; r["iso"] = iso;
    if (p.v.contains("no-keep")) for (const QString& f : files) QFile::remove(f);
    QFile jf(outDir + "/flats_" + tag + ".json"); if (jf.open(QIODevice::WriteOnly | QIODevice::Truncate)) jf.write(QJsonDocument(r).toJson());
    return r;
}

QJsonObject cmdFlatAnalyze(const Parsed& p, const Cmd&)
{
    QStringList files;
    if (p.v.contains("files")) files = str(p, "files").split(',', Qt::SkipEmptyParts);
    else if (p.v.contains("dir")) for (const QString& f : QDir(str(p, "dir")).entryList({"*.fits"}, QDir::Files, QDir::Name)) files << str(p, "dir") + "/" + f;
    if (files.size() < 2) return failure("give --dir (with at least 2 FITS files) or --files");
    return analyseFlats(files, originDataDir() + "/flats", str(p, "tag", "analysis"));
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// survey: while waiting for darkness, sample the places the night's plan will use and judge each view. A tree line or roof in the frame shows as a dark silhouette against
// the bright twilight sky: either the whole frame is much darker than the other samples (blocked) or the frame is far from uniform (partly blocked). Evidence goes to the
// learned obstruction map (which the planner already uses once a bin has been bad on two nights), to survey_<night>.json, and as one picture per sample.
// ---------------------------------------------------------------------------------------------------------------------------------------
QJsonObject cmdSurvey(const Parsed& p, const Cmd& c)
{
    Planner::Options o = plannerOptions(p, c); o.date = Planner::currentNightDate(o.timeZone);
    o.minQuality = p.v.contains("min-quality") ? o.minQuality : 0.2;
    const QJsonObject S = Planner::schedule(o);
    if (!S["ok"].toBool()) return S;
    const QString night = S["night"].toString();
    // candidate places: block start, middle and end, and the first two backups of each block
    struct Pt { double az, alt; QString what; int visits = 0; };
    QList<Pt> cand;
    const QString arcTarget = str(p, "target", "");
    if (!arcTarget.isEmpty()) {
        // the target's own track: its alt/az at times spread across its window for tonight (dense at the start, where it is low and the house is), nothing random
        const QJsonObject tg = Planner::findTarget(o, arcTarget);
        if (!tg["ok"].toBool(true) || !tg.contains("ra_hours")) return failure("target not observable tonight: " + arcTarget);
        const QDateTime w0 = parseZ(tg["window_start"].toString()), w1 = parseZ(tg["window_end"].toString());
        const double span = w0.secsTo(w1) / 60.0;
        for (double m : {0.0, 8.0, 16.0, 30.0, 50.0, 80.0, 120.0, 180.0, 240.0, span}) {
            if (m > span) continue;
            const QDateTime t = w0.addSecs(qint64(m * 60)); const Astro::AltAz aa = Astro::altAz({tg["ra_hours"].toDouble(), tg["dec_deg"].toDouble()}, o.site, t);
            cand.append({aa.az, aa.alt, QString("%1 at %2 UTC").arg(tg["key"].toString(), t.toString("HH:mm"))});
            if (m == 0.0) { cand.append({aa.az, aa.alt - 5, "edge check: 5 deg below the track's start"}); cand.append({aa.az, aa.alt + 4, "edge check: 4 deg above the track's start"}); }
        }
    }
    else for (const QJsonValue& bv : S["plan"].toArray()) {
        const QJsonObject b = bv.toObject(); const QString name = b["name"].toString(b["key"].toString());
        const double a0 = b["az_start"].toDouble(), a1 = b["az_end"].toDouble(), h0 = b["alt_start"].toDouble(), h1 = b["alt_end"].toDouble();
        const double dAz = std::remainder(a1 - a0, 360.0);
        cand.append({a0, h0, name + " (start)"}); cand.append({std::fmod(a0 + dAz / 2 + 360.0, 360.0), (h0 + h1) / 2, name + " (middle)"}); cand.append({a1, h1, name + " (end)"});
        int nb = 0; for (const QJsonValue& kv : b["backups"].toArray()) { if (nb++ >= 2) break; const QJsonObject k = kv.toObject(); cand.append({k["az"].toDouble(), k["alt"].toDouble(), k["name"].toString() + " (backup)"}); }
    }
    QList<Pt> uniq;
    for (const Pt& q : cand) {
        if (q.alt < 22 || q.alt > 85) continue;
        bool dup = false; for (const Pt& u : uniq) if (std::abs(std::remainder(u.az - q.az, 360.0)) < (arcTarget.isEmpty() ? 8 : 2.5) && std::abs(u.alt - q.alt) < (arcTarget.isEmpty() ? 8 : 2.5)) dup = true;
        if (!dup) uniq.append(q);
    }
    if (uniq.isEmpty()) return failure("the plan has nothing to sample");
    // unvisited places first: how often has the learned obstruction map (10 deg x 5 deg bins) seen each place? Fewest visits first, random order within equal counts.
    QJsonObject bins; { QFile mf(originDataDir() + "/sky_map.json"); if (mf.open(QIODevice::ReadOnly)) bins = QJsonDocument::fromJson(mf.readAll()).object()["bins"].toObject(); }
    for (Pt& q : uniq) {
        const QJsonObject b = bins[QString("%1_%2").arg(int(std::floor(q.az / 10.0)) * 10).arg(int(std::floor(q.alt / 5.0)) * 5)].toObject();
        q.visits = b["good"].toInt() + b["bad"].toInt() + b["cloud"].toInt() + b["unk"].toInt();
    }
    if (arcTarget.isEmpty()) {
    std::mt19937 rng(p.v.contains("seed") ? uint32_t(str(p, "seed").toInt()) : uint32_t(QDate::fromString(night, Qt::ISODate).toJulianDay()));
    std::shuffle(uniq.begin(), uniq.end(), rng);
    std::stable_sort(uniq.begin(), uniq.end(), [](const Pt& a, const Pt& b) { return a.visits < b.visits; });
    uniq = uniq.mid(0, std::max(1, int(num(p, c, "samples"))));
    std::sort(uniq.begin(), uniq.end(), [](const Pt& a, const Pt& b) { return a.az < b.az; });       // sweep in azimuth: least slewing
    }
    QJsonArray plan; for (const Pt& q : uniq) plan.append(QJsonObject{{"az", std::round(q.az)}, {"alt", std::round(q.alt)}, {"for", q.what}, {"bin_visits", q.visits}});
    if (p.v.contains("dry-run")) return {{"ok", true}, {"dry_run", true}, {"night", night}, {"samples", plan}};

    const QDateTime t0 = QDateTime::currentDateTimeUtc();
    const double sunNow = Astro::altAz(Astro::sunPosition(t0), o.site, t0).alt;
    if (sunNow > num(p, c, "sun-limit")) return failure(QString("too early: the Sun is at %1 deg (the survey needs it below %2)").arg(sunNow, 0, 'f', 1).arg(num(p, c, "sun-limit"), 0, 'f', 0));
    { QProcess pg; pg.start("pgrep", {"-x", "CelestronOrigin"}); pg.waitForFinished(3000); if (pg.exitCode() == 0) return failure("the GUI app is running; it holds the telescope"); }
    if (!call("status", {}, 10000)["connected"].toBool()) return failure("the daemon is not connected to the telescope");
    if (!call("mount_status", {}, 15000)["mount"].toObject()["IsAligned"].toBool()) return failure("the mount is not aligned; initialize first");
    if (call("sequence", {{"action", "status"}}, 10000)["status"].toObject()["active"].toBool()) return failure("a sequence is running");

    const QString dir = originDataDir() + "/survey/" + night; QDir().mkpath(dir);
    QJsonArray results; QList<double> rates;
    call("manual", {}, 15000);
    for (const Pt& q : uniq) {
        if (t0.secsTo(QDateTime::currentDateTimeUtc()) > num(p, c, "max-minutes") * 60) { note({{"survey", "out of time; stopping"}}); break; }
        const QJsonObject g = gotoAltAz(q.alt, q.az, o.site, 240);
        QJsonObject r{{"az", std::round(q.az)}, {"alt", std::round(q.alt)}, {"for", q.what}};
        if (!g["ok"].toBool()) { r["error"] = "goto failed: " + g["error"].toString(); results.append(r); note({{"survey", "goto failed"}, {"at", r}}); continue; }
        const QDateTime now = QDateTime::currentDateTimeUtc(); const double depr = std::max(0.0, -Astro::altAz(Astro::sunPosition(now), o.site, now).alt);
        double expo = std::clamp(3.0 * std::pow(10.0, 0.4 * (depr - 12.0)), 1e-5, 6.0); QJsonObject fs;
        for (int k = 0; k < 4; ++k) {                                       // find an exposure that neither saturates nor starves the frame
            if (k == 3) sleepMs(16000);                                     // the picture of a frame is written at most every 15 s: make sure the last one is
            fs = call("frame_stats", {{"exposure", expo}, {"iso", int(num(p, c, "iso"))}, {"binning", 1}}, 90000);
            const QString v = fs["verdict"].toString();
            if ((v == "too_bright" || v == "too_short") && std::abs(std::log10(fs["suggested_exposure_s"].toDouble(expo) / expo)) > 0.15) { expo = fs["suggested_exposure_s"].toDouble(expo); continue; }
            if (k < 2) { sleepMs(16000); fs = call("frame_stats", {{"exposure", expo}, {"iso", int(num(p, c, "iso"))}, {"binning", 1}}, 90000); }
            break;
        }
        if (!fs["ok"].toBool()) { r["error"] = fs["error"]; results.append(r); continue; }
        const QJsonObject pc = fs["percentiles"].toObject(); const double black = fs["black_level"].toDouble(4985), p50 = std::max(1.0, pc["p50"].toDouble() - black);
        r["exposure_s"] = expo; r["median_adu"] = fs["median_adu"]; r["sky_rate"] = fs["sky_rate_adu_per_s"]; r["saturated"] = fs["saturated_fraction"];
        r["spread"] = std::round((pc["p95"].toDouble() - pc["p5"].toDouble()) / p50 * 100) / 100; r["stars"] = fs["stars"]; r["verdict_frame"] = fs["verdict"];
        rates.append(fs["sky_rate_adu_per_s"].toDouble());
        const QString pic = dir + QString("/az%1_alt%2.png").arg(int(std::round(q.az)), 3, 10, QChar('0')).arg(int(std::round(q.alt)), 2, 10, QChar('0'));
        QFile::remove(pic); if (QFile::copy(originDataDir() + "/previews/last_frame.png", pic)) r["picture"] = pic;
        results.append(r); note({{"survey", "sampled"}, {"at", r}});
    }
    // judge: much darker than the other samples, or far from uniform
    std::sort(rates.begin(), rates.end()); const double ref = rates.size() >= 4 ? rates[rates.size() / 2] : 0;
    int blocked = 0; QJsonArray out;
    for (const QJsonValue& rv : results) {
        QJsonObject r = rv.toObject();
        if (r.contains("sky_rate")) {
            const double ratio = ref > 0 ? r["sky_rate"].toDouble() / ref : 1.0; r["ratio_to_median"] = std::round(ratio * 100) / 100;
            QString why; if (ratio < 0.35) why = "much darker than the other samples"; else if (r["spread"].toDouble() > 0.6) why = "frame far from uniform (silhouette)";
            r["obstructed"] = !why.isEmpty(); if (!why.isEmpty()) { r["why"] = why; ++blocked; }
            call("sky_note", {{"az", r["az"]}, {"alt", r["alt"]}, {"obstructed", !why.isEmpty()}}, 10000);
        }
        out.append(r);
    }
    QJsonObject res{{"ok", true}, {"night", night}, {"sun_alt_start", sunNow}, {"sampled", int(rates.size())}, {"obstructed", blocked}, {"median_sky_rate", ref}, {"samples", out}, {"pictures", dir}};
    QFile f(originDataDir() + "/survey_" + QString(night).remove('-') + ".json"); if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(res).toJson());
    return res;
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
    const double sunNow = Astro::altAz(Astro::sunPosition(now), o.site, now).alt, probeAlt = num(p, c, "probe-sun-alt");
    {   // autorun starts at --probe-sun-alt (default -7): a plate solve of the twilight sky decides whether stars can be seen, instead of a fixed -12 deg cutoff
        const QString spec = str(p, "start-after", "dusk");
        const QString early = spec == "dusk" ? (sunNow > probeAlt ? QString("the Sun is at %1 deg (autorun starts below %2)").arg(sunNow, 0, 'f', 1).arg(probeAlt, 0, 'f', 0) : QString()) : tooEarlyToSlew(spec, o.site);
        if (!p.v.contains("ignore-time") && !early.isEmpty()) return idle("too early for tonight: " + early);
    }
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
    if (!mount["IsAligned"].toBool() || st["need_real_init"].toBool()) {
        if (st["init_attempts"].toInt() >= 3) return idle("mount will not align (3 attempts); needs a look");
        // Before the first init: raise the tube if the tilt sensor says it is low (the GUI's unpark, at most 3 times a night), then take a frame and plate-solve it.
        // The solve proves there are stars to align on and tells us where the tube points; it is advisory for now: after 2 failed solves the initialize is tried anyway.
        {
            const QJsonObject ms = call("mount_status", {}, 15000);
            if (ms.contains("tilt_alt_deg") && ms["tilt_alt_deg"].toDouble() < 45) {
                if (st["unparks"].toInt() >= 3) return idle("tube still low after 3 unpark attempts; needs a look");
                st["unparks"] = st["unparks"].toInt() + 1; save();
                note({{"autorun", "tube is low (tilt sensor); unparking"}, {"tilt", ms["tilt_alt_deg"]}});
                const QJsonObject u = call("unpark", {}, 150000);
                note({{"autorun", "unpark"}, {"result", u}});
                if (!u["ok"].toBool()) return idle("unpark: " + u["error"].toString());
            }
            // Rayleigh-scattered twilight brightens by roughly 1 magnitude per degree of Sun depression, so the first guess is 10^(0.4 (depression - 12)) * 3 s (3 s at -12 deg);
            // `solve` measures the frame first (frame-stats) and, when it is saturated or empty, suggests the exposure that puts the sky about 6000 ADU above black. Up to 5 quick
            // corrections per tick cost seconds each (no solve-field). Every probe is appended to solve_probes.jsonl so the real limit can be read off later.
            const double depr = std::max(0.0, -sunNow);
            double expo = std::clamp(3.0 * std::pow(10.0, 0.4 * (depr - 12.0)), 1e-5, 6.0);
            call("manual", {}, 15000);
            QJsonObject sv;
            for (int attempt = 0; attempt < 5; ++attempt) {
                sv = call("solve", {{"lat_deg", o.site.latDeg}, {"lon_deg", o.site.lonDeg}, {"cpulimit", 40}, {"exposure", expo}, {"iso", 800}, {"binning", 1}}, 200000);
                const QJsonObject fs = sv["stats"].toObject();
                note({{"autorun", "plate solve before init"}, {"attempt", attempt + 1}, {"sun_alt", sunNow}, {"exposure_s", expo}, {"verdict", fs["verdict"]}, {"stars", fs["stars"]}, {"sky_rate", fs["sky_rate_adu_per_s"]}, {"result", sv}});
                { QFile pf(originDataDir() + "/solve_probes.jsonl"); if (pf.open(QIODevice::Append)) pf.write(QJsonDocument(QJsonObject{{"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}, {"sun_alt", sunNow}, {"exposure_s", expo},
                    {"verdict", fs["verdict"]}, {"median_adu", fs["median_adu"]}, {"stars", fs["stars"]}, {"sky_rate_adu_per_s", fs["sky_rate_adu_per_s"]}, {"solved", sv["solved"].toBool()}, {"summary", sv["summary"]}, {"error", sv["error"]}}).toJson(QJsonDocument::Compact) + "\n"); }
                if (sv["solved"].toBool()) break;
                const QString verdict = sv["verdict"].toString();
                const double next = sv["suggested_exposure_s"].toDouble(0);
                if (sv["skipped"].toBool() && (verdict == "too_bright" || verdict == "too_short") && next > 0 && std::abs(std::log10(next / expo)) > 0.1) { expo = next; continue; }
                break;
            }
            if (sv["solved"].toBool()) { st["solve_fails"] = 0; st["last_solve"] = sv["summary"]; save(); }
            else {
                st["solve_fails"] = st["solve_fails"].toInt() + 1; st["last_solve_error"] = sv["error"]; save();
                // before nautical dark a failed solve just means the sky is still too bright (or cloudy): wait. From -12 deg on, two failures and the initialize is tried anyway.
                if (sunNow > -12.0 || st["solve_fails"].toInt() < 2) return idle(QString("no plate solution yet (%1; Sun %2 deg, %3 s); will retry next tick").arg(sv["error"].toString()).arg(sunNow, 0, 'f', 1).arg(expo, 0, 'g', 3));
                // ...unless the sky is overcast: an initialize needs stars as much as a solve does, and would only use up its 3 attempts on cloud
                const QJsonObject nc = Weather::metarNowcast(o.site.latDeg, o.site.lonDeg, originDataDir(), 15, nullptr);
                if (nc["ok"].toBool() && nc["cover_pct"].toDouble() >= 80) {
                    st["solve_fails"] = 1; save();
                    return idle(QString("overcast (%1% cloud now) and no plate solution; not spending an initialize on cloud, will retry next tick").arg(nc["cover_pct"].toDouble(), 0, 'f', 0));
                }
                note({{"autorun", "no plate solution twice in the dark; initializing anyway"}});
            }
        }
        st["init_attempts"] = st["init_attempts"].toInt() + 1; save();
        note({{"autorun", "initializing the mount"}});
        QJsonObject r = call("initialize", {{"lat_deg", o.site.latDeg}, {"lon_deg", o.site.lonDeg}, {"timezone", o.timeZone}, {"wait", true}, {"timeout", 300}}, 360000);
        if (!r["ok"].toBool() || r["outcome"].toString().toLower() == "failed") {
            if (r["error"].toString().contains("enough stars", Qt::CaseInsensitive) || r["summary"].toString().contains("enough stars", Qt::CaseInsensitive)) {
                // the sky is not clear enough yet: that is weather, not a mount fault, so it does not use up one of the 3 attempts (at most 12 such tries a night)
                st["init_attempts"] = st["init_attempts"].toInt() - 1; st["star_fails"] = st["star_fails"].toInt() + 1; save();
                if (st["star_fails"].toInt() > 12) return idle("the alignment found too few stars 12 times tonight; giving up until the sky clears (touch nothing; it will keep trying tomorrow)");
                return idle("initialize: not enough stars for alignment (cloud?); will retry next tick");
            }
            if (str(p, "init-fallback", "off") != "fake-init") return idle("initialize failed: " + r["error"].toString() + r["summary"].toString() + " (will retry; --init-fallback fake-init is off)");
            // Patchy cloud makes the star alignment fail. Fake-initialize (no alignment, just a coordinate frame), point at sky on the other side of the
            // meridian with an ordinary RA/Dec goto, and try the real initialize again on the next tick. The mount is NOT trusted until that succeeds.
            double alt = 0, az = 0; const bool have = readAltAz(&alt, &az);
            const double newAz = have ? std::fmod(360.0 - az + 360.0, 360.0) : 270.0, newAlt = have ? std::clamp(alt, 40.0, 65.0) : 55.0;
            note({{"autorun", "initialize failed; fake-init and move to the other side of the meridian"}, {"why", r["summary"]}, {"to_az", newAz}, {"to_alt", newAlt}});
            const QJsonObject fk = call("initialize", {{"lat_deg", o.site.latDeg}, {"lon_deg", o.site.lonDeg}, {"timezone", o.timeZone}, {"fake", true}, {"wait", true}, {"timeout", 120}}, 180000);
            QJsonObject g = fk["ok"].toBool() ? gotoAltAz(newAlt, newAz, o.site, 240) : fk;
            st["need_real_init"] = true; st["moves"] = st["moves"].toInt() + 1; st["last_init_error"] = r["summary"]; save();
            return idle(QString("initialize failed (%1); fake-init %2, moved to az %3 alt %4 (%5); will retry the real initialize").arg(r["summary"].toString(), fk["ok"].toBool() ? "ok" : "failed").arg(newAz, 0, 'f', 0).arg(newAlt, 0, 'f', 0).arg(g["ok"].toBool() ? "goto ok" : g["error"].toString()));
        }
        // never launch the run on a mount that is not aligned, whatever the initialize call reported
        if (!call("mount_status", {{"fresh", true}}, 15000)["mount"].toObject()["IsAligned"].toBool()) {
            st["need_real_init"] = false; save();
            return idle("initialize returned but the mount is not aligned; will retry next tick");
        }
        st["need_real_init"] = false; save();
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
    const quint16 port = quint16(p.v.contains("control-port") ? str(p, "control-port").toInt() : qEnvironmentVariableIsSet("ORIGIN_CONTROL_PORT") ? qEnvironmentVariable("ORIGIN_CONTROL_PORT").toInt() : 8765);
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
    else if (cmd->name == "survey") out = cmdSurvey(p, *cmd);
    else if (cmd->name == "flats") out = cmdFlats(p, *cmd);
    else if (cmd->name == "flat-analyze") out = cmdFlatAnalyze(p, *cmd);
    else if (cmd->name == "forecast-accuracy") out = forecastAccuracy();
    else if (cmd->name == "sky-report") {
        const double cv = std::max(0.0, std::min(100.0, num(p, *cmd, "cover")));
        QFile f(originDataDir() + "/sky_report.json");
        if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(QJsonObject{{"cover_pct", cv}, {"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}}).toJson());
        out = QJsonObject{{"ok", true}, {"cover_pct", cv}, {"note", "used for 90 minutes; forecast hours near now now follow the models that agree with it"}};
    }
    else if (cmd->name == "goto-altaz") out = cmdGotoAltAz(p, *cmd);
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
