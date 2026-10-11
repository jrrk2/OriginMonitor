// OriginWatch: a read-only window onto a running OriginHeadless daemon.
//   Now      what the telescope, the sequence and the unattended supervisor are doing
//   Log      recent telescope/daemon events and the supervisor's decisions
//   Live     the live stack, rendered on demand
//   Stacks   every stack in the data directory, finished or not
//   Forecast the blended multi-model weather forecast hour by hour (Open-Meteo, cached 1 h, same cache as OriginHeadless) and how accurate it has been
// It talks to 127.0.0.1:<port> with a whitelist of read-only commands (see allowed()) and reads <data dir> files; it cannot move or change anything.
#include "DataDir.hpp"
#include "Weather.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QQueue>
#include <QFontDatabase>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QHeaderView>
#include <QTimeZone>
#include <QTabWidget>
#include <QTcpSocket>
#include <QTimer>
#include <QVBoxLayout>
#include <functional>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

// The only commands this program will ever send. Anything else is refused here, before it reaches the socket.
bool allowed(const QJsonObject& r)
{
    static const QSet<QString> ok = {"ping", "status", "mount_status", "focuser_status", "focus_status", "analysis", "events", "sky_map", "preview", "target"};
    const QString c = r["cmd"].toString();
    if (c == "sequence") return r["action"].toString("status") == "status";
    if (c == "target") return !r.contains("name") && !r.contains("reset");          // the read form only
    return ok.contains(c);
}

// One persistent connection, requests answered in order, never blocks the GUI.
class Client : public QObject {
    Q_OBJECT
public:
    using Callback = std::function<void(const QJsonObject&)>;
    explicit Client(int port, QObject* parent = nullptr) : QObject(parent), m_port(port)
    {
        connect(&m_sock, &QTcpSocket::readyRead, this, &Client::onRead);
        connect(&m_sock, &QTcpSocket::disconnected, this, &Client::onDown);
        connect(&m_sock, &QTcpSocket::connected, this, [this] { m_up = true; emit state(true); pump(); });
        connect(&m_sock, &QAbstractSocket::errorOccurred, this, [this] { onDown(); });
        m_timeout.setSingleShot(true);
        connect(&m_timeout, &QTimer::timeout, this, [this] { m_sock.abort(); });
    }
    ~Client() override { QObject::disconnect(&m_sock, nullptr, this, nullptr); m_timeout.stop(); }   // the socket's own destructor emits disconnected(); members are already gone by then
    bool up() const { return m_up; }
    void request(QJsonObject req, Callback cb)
    {
        if (!allowed(req)) { cb({{"ok", false}, {"error", "refused: OriginWatch is read-only"}}); return; }
        if (m_queue.size() > 40) return;                                      // the daemon is not answering; do not pile up
        m_queue.enqueue({req, cb});
        if (!m_up && m_sock.state() == QAbstractSocket::UnconnectedState) m_sock.connectToHost("127.0.0.1", quint16(m_port));
        else pump();
    }
signals:
    void state(bool up);
private:
    struct Item { QJsonObject req; Callback cb; };
    void pump()
    {
        if (!m_up || m_busy || m_queue.isEmpty()) return;
        m_busy = true; m_inflight = m_queue.head();
        m_sock.write(QJsonDocument(m_inflight.req).toJson(QJsonDocument::Compact) + "\n");
        m_timeout.start(20000);
    }
    void onRead()
    {
        m_buf += m_sock.readAll();
        int nl;
        while ((nl = m_buf.indexOf('\n')) >= 0) {
            const QByteArray line = m_buf.left(nl); m_buf.remove(0, nl + 1);
            if (!m_busy) continue;
            m_timeout.stop(); m_busy = false; m_queue.dequeue();
            const QJsonObject o = QJsonDocument::fromJson(line).object();
            if (m_inflight.cb) m_inflight.cb(o);
            pump();
        }
    }
    void onDown()
    {
        const bool was = m_up; m_up = false; m_busy = false; m_buf.clear(); m_timeout.stop();
        const QQueue<Item> pending = m_queue; m_queue.clear();
        for (const Item& i : pending) if (i.cb) i.cb({{"ok", false}, {"error", "daemon not reachable"}});
        if (was) emit state(false); else emit state(false);
    }
    int m_port; QTcpSocket m_sock; QByteArray m_buf; QQueue<Item> m_queue; Item m_inflight; bool m_up = false, m_busy = false; QTimer m_timeout;
};

// Image scaled to fit the widget, keeping the aspect ratio.
class ImageView : public QWidget {
public:
    explicit ImageView(QWidget* parent = nullptr) : QWidget(parent) { setMinimumSize(320, 240); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding); }
    void setImage(const QImage& im, const QString& caption) { m_img = im; m_caption = caption; update(); }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this); p.fillRect(rect(), palette().window());
        if (m_img.isNull()) { p.setPen(palette().text().color()); p.drawText(rect(), Qt::AlignCenter, m_caption.isEmpty() ? "no image" : m_caption); return; }
        const QImage s = m_img.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        p.drawImage((width() - s.width()) / 2, (height() - s.height()) / 2, s);
        p.setPen(Qt::white); p.drawText(8, height() - 8, m_caption);
    }
private:
    QImage m_img; QString m_caption;
};

QLabel* value() { auto* l = new QLabel("-"); l->setTextInteractionFlags(Qt::TextSelectableByMouse); return l; }
QString hms(double hours) { const int s = int(std::lround(hours * 3600)); return QString("%1h %2m %3s").arg(s / 3600).arg((s / 60) % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0')); }
QString dms(double deg) { const QString sign = deg < 0 ? "-" : "+"; deg = std::fabs(deg); const int d = int(deg), m = int((deg - d) * 60); return QString("%1%2° %3'").arg(sign).arg(d).arg(m, 2, 10, QChar('0')); }

class Window : public QMainWindow {
    Q_OBJECT
public:
    Window(int port, const QString& dataDir, double lat, double lon) : m_dir(dataDir), m_port(port), m_lat(lat), m_lon(lon)
    {
        m_client = new Client(port, this);
        setWindowTitle(QString("OriginWatch (read-only) - daemon port %1").arg(port));
        auto* tabs = new QTabWidget; setCentralWidget(tabs);
        tabs->addTab(buildNow(), "Now"); tabs->addTab(buildLog(), "Log"); tabs->addTab(buildLive(), "Live stack"); tabs->addTab(buildStacks(), "Stacks"); tabs->addTab(buildForecast(), "Forecast");
        m_night = new QCheckBox("Night (red) mode"); statusBar()->addPermanentWidget(m_night);
        connect(m_night, &QCheckBox::toggled, this, &Window::applyNight);
        m_conn = new QLabel("daemon: connecting..."); statusBar()->addWidget(m_conn);
        connect(m_client, &Client::state, this, [this](bool up) { m_conn->setText(up ? "daemon: connected" : QString("daemon: not reachable on port %1 (start it with OriginHeadless serve)").arg(m_port)); if (!up) clearNow(); });
        connect(&m_poll, &QTimer::timeout, this, &Window::poll); m_poll.start(2000);
        connect(&m_files, &QTimer::timeout, this, &Window::readRunLog); m_files.start(3000);
        connect(&m_fcTimer, &QTimer::timeout, this, &Window::reloadForecast); m_fcTimer.start(30 * 60 * 1000);
        resize(1100, 760); poll(); readRunLog(); reloadStacks(); QTimer::singleShot(200, this, &Window::reloadForecast);
    }
    void showTab(int i) { if (auto* t = findChild<QTabWidget*>()) t->setCurrentIndex(i); }
    void clickRender() { renderLive(); }
    void selectStack(int i) { if (i < m_list->count()) m_list->setCurrentRow(i); }
private:
    // ---------------------------------------------------------------- Forecast
    QWidget* buildForecast()
    {
        auto* w = new QWidget; auto* v = new QVBoxLayout(w);
        auto* top = new QHBoxLayout; auto* refresh = new QPushButton("Refresh now"); m_fcInfo = new QLabel("fetching..."); m_fcInfo->setWordWrap(true);
        top->addWidget(m_fcInfo, 1); top->addWidget(refresh); v->addLayout(top);
        m_fcTable = new QTableWidget(0, 15); m_fcTable->setEditTriggers(QAbstractItemView::NoEditTriggers); m_fcTable->setSelectionMode(QAbstractItemView::NoSelection);
        m_fcTable->setHorizontalHeaderLabels({"Local", "Quality", "Range", "Cloud %", "Low", "Mid", "High", "Rain %", "Met Office", "ECMWF", "AIFS", "ICON", "GFS", "M-France", "KNMI"});
        m_fcTable->verticalHeader()->hide(); m_fcTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        v->addWidget(m_fcTable, 1);
        m_fcAcc = new QLabel; m_fcAcc->setWordWrap(true); v->addWidget(m_fcAcc);
        auto* legend = new QLabel("Quality 0..1 = how much of the hour is usable (low and mid cloud block, high cloud counts half, cut for rain, fog, dew and wind). It is the mean over 7 models; the models often disagree by 50 points, so look at the range as well. Green >= 0.5, amber 0.3-0.5, red below.");
        legend->setWordWrap(true); v->addWidget(legend);
        connect(refresh, &QPushButton::clicked, this, [this] { m_fcFresh = true; reloadForecast(); });
        return w;
    }
    static QColor tint(double q) { return q >= 0.5 ? QColor(20, 110, 40) : q >= 0.3 ? QColor(150, 110, 20) : QColor(120, 30, 30); }
    void reloadForecast()
    {
        m_fcInfo->setText("fetching..."); qApp->processEvents();
        QVector<Weather::Hour> hrs; const QJsonObject r = Weather::fetch(m_lat, m_lon, 3, m_fcFresh ? 0 : 60, m_dir, &hrs); m_fcFresh = false;
        if (!r["ok"].toBool()) { m_fcInfo->setText("forecast unavailable: " + r["error"].toString()); return; }
        const QTimeZone zone("Europe/London"); const QDateTime now = QDateTime::currentDateTimeUtc();
        m_fcInfo->setText(QString("%1, fetched %2 (local). %3 Site %4 N %5 E.").arg(r["source"].toString(), QDateTime::fromString(r["fetched"].toString(), Qt::ISODate).toTimeZone(zone).toString("ddd HH:mm"),
                                                                                 r["note"].toString(), QString::number(m_lat, 'f', 2), QString::number(m_lon, 'f', 2)));
        static const QStringList ids = {"ukmo_seamless", "ecmwf_ifs025", "ecmwf_aifs025_single", "icon_seamless", "gfs_seamless", "meteofrance_seamless", "knmi_seamless"};
        m_fcTable->setRowCount(0);
        int row = 0;
        for (const Weather::Hour& h : hrs) {
            if (h.utc < now.addSecs(-3600) || h.utc > now.addSecs(48 * 3600)) continue;
            m_fcTable->insertRow(row);
            auto put = [&](int c, const QString& t, const QColor& bg = QColor()) { auto* it = new QTableWidgetItem(t); it->setTextAlignment(Qt::AlignCenter); if (bg.isValid()) { it->setBackground(bg); it->setForeground(Qt::white); } m_fcTable->setItem(row, c, it); };
            put(0, h.utc.toTimeZone(zone).toString("ddd HH:mm")); put(1, QString::number(h.quality, 'f', 2), tint(h.quality));
            put(2, h.models.isEmpty() ? "-" : QString("%1 - %2").arg(h.qMin, 0, 'f', 2).arg(h.qMax, 0, 'f', 2));
            put(3, QString::number(h.cloud, 'f', 0)); put(4, QString::number(h.low, 'f', 0)); put(5, QString::number(h.mid, 'f', 0)); put(6, QString::number(h.high, 'f', 0)); put(7, QString::number(h.precipProb, 'f', 0));
            for (int i = 0; i < ids.size(); ++i) { if (h.models.contains(ids[i])) put(8 + i, QString::number(h.models[ids[i]], 'f', 2), tint(h.models[ids[i]])); else put(8 + i, "-"); }
            ++row;
        }
        QFile af(m_dir + "/forecast_accuracy.json"); QString acc = "Forecast track record: none yet (the dusk forecast is archived each night and compared with the frames the telescope takes).";
        if (af.open(QIODevice::ReadOnly)) {
            const QJsonObject s = QJsonDocument::fromJson(af.readAll()).object()["summary"].toObject(); const int n = s["hours_compared"].toInt();
            if (n >= 8) {
                acc = QString("Forecast track record: %1 observed hours; the sky was on average %2 %3 than forecast (forecast %4, observed %5). Of %6 hours forecast poor, %7 were usable.")
                          .arg(n).arg(std::abs(s["bias_pessimism"].toDouble()), 0, 'f', 2).arg(s["bias_pessimism"].toDouble() >= 0 ? "better" : "worse").arg(s["mean_forecast"].toDouble(), 0, 'f', 2).arg(s["mean_observed"].toDouble(), 0, 'f', 2)
                          .arg(s["hours_forecast_poor"].toInt()).arg(s["poor_but_usable"].toInt());
                const QJsonArray bm = s["by_model_best_first"].toArray(); if (!bm.isEmpty()) acc += QString(" Closest model so far: %1.").arg(bm[0].toObject()["model"].toString());
            } else acc = QString("Forecast track record: only %1 observed hour(s) so far; too few to judge.").arg(n);
        }
        m_fcAcc->setText(acc);
    }

    // ---------------------------------------------------------------- Now
    QWidget* buildNow()
    {
        auto* w = new QWidget; auto* grid = new QGridLayout(w);
        auto group = [&](const QString& title, std::initializer_list<QPair<QString, QLabel**>> rows, int r, int c) {
            auto* g = new QGroupBox(title); auto* f = new QFormLayout(g);
            for (const auto& row : rows) { *row.second = value(); f->addRow(row.first, *row.second); }
            grid->addWidget(g, r, c);
        };
        group("Telescope", {{"link", &m_tLink}, {"state", &m_tState}, {"mount", &m_tMount}, {"battery", &m_tBatt}}, 0, 0);
        group("Pointing", {{"RA / Dec", &m_pRaDec}, {"Alt / Az", &m_pAltAz}, {"goto", &m_pGoto}, {"target", &m_pTarget}}, 0, 1);
        group("Focuser and camera", {{"focuser", &m_fPos}, {"exposure / ISO", &m_cExp}, {"sensor temp", &m_cTemp}}, 1, 0);
        group("Sequence", {{"state", &m_sState}, {"frames", &m_sFrames}, {"last frame", &m_sLast}, {"cause", &m_sCause}, {"focus watchdog", &m_sFocus}, {"timing", &m_sTiming}}, 1, 1);
        group("Sky (last analysis)", {{"verdict", &m_aVerdict}, {"why", &m_aWhy}, {"sky", &m_aSky}}, 2, 0);
        group("Supervisor", {{"last decision", &m_rLast}, {"decisions", &m_rCount}}, 2, 1);
        m_aWhy->setWordWrap(true); m_rLast->setWordWrap(true); m_sCause->setWordWrap(true);
        grid->setRowStretch(3, 1);
        return w;
    }
    // ---------------------------------------------------------------- Log
    QWidget* buildLog()
    {
        auto* w = new QWidget; auto* v = new QVBoxLayout(w);
        auto* bar = new QHBoxLayout; m_showEv = new QCheckBox("telescope / daemon events"); m_showEv->setChecked(true);
        m_showRun = new QCheckBox("supervisor decisions"); m_showRun->setChecked(true); m_follow = new QCheckBox("follow"); m_follow->setChecked(true);
        bar->addWidget(m_showEv); bar->addWidget(m_showRun); bar->addStretch(); bar->addWidget(m_follow); v->addLayout(bar);
        m_log = new QPlainTextEdit; m_log->setReadOnly(true); m_log->setMaximumBlockCount(5000); m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
        m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont)); v->addWidget(m_log);
        return w;
    }
    // ---------------------------------------------------------------- Live
    QWidget* buildLive()
    {
        auto* w = new QWidget; auto* v = new QVBoxLayout(w);
        auto* bar = new QHBoxLayout; auto* b = new QPushButton("Show current stack"); m_liveInfo = new QLabel("press the button to render the live stack"); m_auto = new QCheckBox("refresh every 60 s");
        bar->addWidget(b); bar->addWidget(m_auto); bar->addWidget(m_liveInfo, 1); v->addLayout(bar);
        m_live = new ImageView; v->addWidget(m_live, 1);
        connect(b, &QPushButton::clicked, this, &Window::renderLive);
        connect(&m_liveTimer, &QTimer::timeout, this, [this] { if (m_auto->isChecked()) renderLive(); }); m_liveTimer.start(60000);
        return w;
    }
    void renderLive()
    {
        m_liveInfo->setText("rendering...");
        m_client->request({{"cmd", "preview"}}, [this](const QJsonObject& r) {
            if (!r["ok"].toBool()) { m_liveInfo->setText("error: " + r["error"].toString()); return; }
            const QJsonObject s = r["stack"].toObject();
            if (!r.contains("preview")) { m_liveInfo->setText(QString("target %1: %2").arg(r["target"].toString(), r["note"].toString("no preview"))); m_live->setImage(QImage(), "the live stack is empty"); return; }
            const QString info = QString("%1: %2 frames, %3 s total, rendered %4Z").arg(r["target"].toString()).arg(s["frames"].toInt()).arg(s["total_exposure_s"].toDouble(), 0, 'f', 0).arg(r["rendered"].toString().mid(11, 8));
            m_liveInfo->setText(info); m_live->setImage(QImage(r["preview"].toString()), info);
        });
    }
    // ---------------------------------------------------------------- Stacks
    QWidget* buildStacks()
    {
        auto* sp = new QSplitter;
        auto* left = new QWidget; auto* lv = new QVBoxLayout(left); auto* reload = new QPushButton("Reload list");
        m_list = new QListWidget; lv->addWidget(reload); lv->addWidget(m_list); sp->addWidget(left);
        auto* right = new QWidget; auto* rv = new QVBoxLayout(right); m_stackInfo = new QLabel("select a stack"); m_stackInfo->setWordWrap(true); m_stackView = new ImageView;
        rv->addWidget(m_stackInfo); rv->addWidget(m_stackView, 1); sp->addWidget(right); sp->setStretchFactor(1, 1); sp->setSizes({320, 780});
        connect(reload, &QPushButton::clicked, this, &Window::reloadStacks);
        connect(m_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* it) { if (it) showStack(it->data(Qt::UserRole).toString()); });
        return sp;
    }
    struct StackInfo { int frames = 0, w = 0, h = 0; double exposure = 0; bool ok = false; };
    static StackInfo readHeader(const QString& file)      // state.bin: "LST1", w, h, frames (int32), total exposure (double)
    {
        StackInfo s; QFile f(file);
        if (!f.open(QIODevice::ReadOnly)) return s;
        const QByteArray b = f.read(32);
        if (b.size() < 24) return s;
        quint32 magic; qint32 w, h, n; double e;
        memcpy(&magic, b.constData(), 4); memcpy(&w, b.constData() + 4, 4); memcpy(&h, b.constData() + 8, 4); memcpy(&n, b.constData() + 12, 4); memcpy(&e, b.constData() + 16, 8);
        if (magic != 0x4C535431) return s;
        s.w = w; s.h = h; s.frames = n; s.exposure = e; s.ok = true; return s;
    }
    void reloadStacks()
    {
        const QString keep = m_list->currentItem() ? m_list->currentItem()->data(Qt::UserRole).toString() : QString();
        m_list->clear();
        const QFileInfoList dirs = QDir(m_dir + "/stacks").entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);   // newest first
        for (const QFileInfo& d : dirs) {
            const StackInfo s = readHeader(d.absoluteFilePath() + "/state.bin");
            const QString text = s.ok ? QString("%1\n  %2 frames, %3 s   %4").arg(d.fileName()).arg(s.frames).arg(s.exposure, 0, 'f', 0).arg(d.lastModified().toString("yyyy-MM-dd HH:mm"))
                                      : QString("%1\n  (no state)   %2").arg(d.fileName(), d.lastModified().toString("yyyy-MM-dd HH:mm"));
            auto* it = new QListWidgetItem(text); it->setData(Qt::UserRole, d.absoluteFilePath()); m_list->addItem(it);
            if (d.absoluteFilePath() == keep) m_list->setCurrentItem(it);
        }
        if (!m_list->count()) m_stackInfo->setText("no stacks under " + m_dir + "/stacks");
    }
    void showStack(const QString& dir)
    {
        const StackInfo s = readHeader(dir + "/state.bin");
        const QFileInfo prev(dir + "/preview.png");
        m_stackInfo->setText(QString("%1 - %2 frames, %3 s (%4 x %5 px); preview written %6").arg(QFileInfo(dir).fileName()).arg(s.frames).arg(s.exposure, 0, 'f', 0).arg(s.w).arg(s.h)
                                 .arg(prev.exists() ? prev.lastModified().toString("yyyy-MM-dd HH:mm") : "never"));
        m_stackView->setImage(prev.exists() ? QImage(prev.absoluteFilePath()) : QImage(), prev.exists() ? QFileInfo(dir).fileName() : QString("no preview.png in this stack"));
    }
    // ---------------------------------------------------------------- polling
    void poll()
    {
        m_client->request({{"cmd", "status"}}, [this](const QJsonObject& r) { if (r["ok"].toBool()) onStatus(r); });
        m_client->request({{"cmd", "mount_status"}}, [this](const QJsonObject& r) { if (r["ok"].toBool()) onMount(r["mount"].toObject()); });
        m_client->request({{"cmd", "focuser_status"}}, [this](const QJsonObject& r) { if (r["ok"].toBool()) m_fPos->setText(QString::number(r["focuser"].toObject()["Position"].toInt()) + (r["focuser"].toObject()["IsMoveToOver"].toBool(true) ? "" : "  (moving)")); });
        m_client->request({{"cmd", "sequence"}, {"action", "status"}}, [this](const QJsonObject& r) { if (r["ok"].toBool()) onSequence(r["status"].toObject()); });
        m_client->request({{"cmd", "analysis"}}, [this](const QJsonObject& r) { if (r["ok"].toBool()) onAnalysis(r["last"].toObject()); });
        m_client->request({{"cmd", "target"}}, [this](const QJsonObject& r) { if (r["ok"].toBool()) m_pTarget->setText(r["target"].toString() + (r["stack"].toObject().contains("frames") ? QString("  (stack %1 frames)").arg(r["stack"].toObject()["frames"].toInt()) : QString())); });
        m_client->request({{"cmd", "events"}, {"since", double(m_evSeq)}, {"limit", 200}}, [this](const QJsonObject& r) { if (r["ok"].toBool()) onEvents(r); });
    }
    void clearNow() { for (QLabel* l : {m_tLink, m_tState, m_tMount, m_tBatt, m_pRaDec, m_pAltAz, m_pGoto, m_pTarget, m_fPos, m_cExp, m_cTemp, m_sState, m_sFrames, m_sLast, m_sCause, m_sFocus, m_sTiming, m_aVerdict, m_aWhy, m_aSky}) l->setText("-"); }
    void onStatus(const QJsonObject& r)
    {
        m_tLink->setText(r["connected"].toBool() ? "connected to " + r["host"].toString() : "NOT connected to the telescope");
        const QJsonObject last = r["last"].toObject(), tc = last["TaskController/GetStatus"].toObject(), cam = last["Camera/GetCaptureParameters"].toObject();
        QString st = tc["State"].toString(); const QJsonObject ii = tc["InitializationInfo"].toObject();
        if (ii.contains("CurrentStep")) st += QString(" (%1, %2%)").arg(ii["CurrentStep"].toString()).arg(ii["PercentageComplete"].toInt());
        m_tState->setText(st.isEmpty() ? "-" : st);
        m_cExp->setText(QString("%1 s, ISO %2").arg(r["exposure"].toDouble()).arg(r["iso"].toInt()));
        if (cam.contains("Temperature")) m_cTemp->setText(QString("%1 °C").arg(cam["Temperature"].toDouble(), 0, 'f', 1));
    }
    void onMount(const QJsonObject& m)
    {
        m_tMount->setText(QString("%1, %2").arg(m["IsTracking"].toBool() ? "tracking" : "not tracking", m["IsAligned"].toBool() ? "aligned" : "not aligned"));
        m_tBatt->setText(QString("%1 %2 V %3").arg(m["BatteryLevel"].toString()).arg(m["BatteryVoltage"].toDouble(), 0, 'f', 1).arg(m["ChargerStatus"].toString()));
        m_pRaDec->setText(QString("%1  %2").arg(hms(m["Ra"].toDouble() * 12 / kPi), dms(m["Dec"].toDouble() * 180 / kPi)));
        m_pAltAz->setText(QString("%1°  %2°").arg(m["Alt"].toDouble() * 180 / kPi, 0, 'f', 1).arg(m["Azm"].toDouble() * 180 / kPi, 0, 'f', 1));
        m_pGoto->setText(m["IsGotoOver"].toBool(true) ? "idle" : "SLEWING");
    }
    void onSequence(const QJsonObject& s)
    {
        if (!s["active"].toBool()) { m_sState->setText("not running" + (s["stop_reason"].toString().isEmpty() ? QString() : " (" + s["stop_reason"].toString() + ")")); }
        else m_sState->setText(s["state"].toString() == "probing" ? "PROBING (frames rejected, waiting for the sky)" : "shooting");
        m_sFrames->setText(QString("%1 taken: %2 good, %3 bad;  %4 s x ISO %5").arg(s["frames"].toInt()).arg(s["good"].toInt()).arg(s["bad"].toInt()).arg(s["exposure"].toDouble()).arg(s["iso"].toInt()));
        const QJsonObject l = s["last"].toObject();
        if (!l.isEmpty()) {
            m_sLast->setText(QString("%1  %2 stars  HFR %3 px  transparency %4  stack %5 frames").arg(l["verdict"].toString()).arg(l["stars"].toInt()).arg(l["hfr_px"].toDouble(), 0, 'f', 2).arg(l["transparency"].toDouble(), 0, 'f', 2).arg(l["frames_in_stack"].toInt()));
            const QString c = l["cause"].toString(); m_sCause->setText(c.isEmpty() || c == "none" ? "-" : c);
        }
        const QJsonObject f = s["focus"].toObject();
        if (!f.isEmpty()) m_sFocus->setText(f["baseline_px"].toDouble() > 0 ? QString("median %1 px vs baseline %2 px (drift events: %3)").arg(f["median_px"].toDouble(), 0, 'f', 2).arg(f["baseline_px"].toDouble(), 0, 'f', 2).arg(f["drifts"].toInt()) : "collecting a baseline");
        if (s.contains("avg_cycle_ms")) m_sTiming->setText(QString("cycle %1 s, duty %2%").arg(s["avg_cycle_ms"].toDouble() / 1000, 0, 'f', 1).arg(int(s["duty_cycle"].toDouble() * 100)));
    }
    void onAnalysis(const QJsonObject& a)
    {
        if (a.isEmpty()) return;
        m_aVerdict->setText(a["verdict"].toString());
        QStringList why; for (const auto& v : a["reasons"].toArray()) why << v.toString(); for (const auto& v : a["cause_reasons"].toArray()) why << v.toString();
        m_aWhy->setText(why.isEmpty() ? "-" : why.join("; "));
        m_aSky->setText(QString("alt %1°, sky %2 ADU/s, moon %3%, sun %4°").arg(a["alt_deg"].toDouble(), 0, 'f', 0).arg(a["sky_rate"].toDouble(), 0, 'f', 0).arg(int(a["moon_illumination"].toDouble() * 100)).arg(a["sun_alt_deg"].toDouble(), 0, 'f', 0));
    }
    void onEvents(const QJsonObject& r)
    {
        const qint64 next = qint64(r["next"].toDouble());
        if (next < m_evSeq) { m_evSeq = 0; return; }                          // the daemon restarted: start over on the next poll
        m_evSeq = next;
        for (const QJsonValue& v : r["events"].toArray()) {
            const QJsonObject e = v.toObject();
            append("event", QString("%1  %2  %3").arg(e["time"].toString().mid(11, 8), e["kind"].toString(), e["text"].toString()));
        }
    }
    void readRunLog()
    {
        QFileInfoList runs = QDir(m_dir).entryInfoList({"run_*.jsonl"}, QDir::Files, QDir::Time);
        if (runs.isEmpty()) return;
        const QString path = runs.first().absoluteFilePath();
        if (path != m_runPath) { m_runPath = path; m_runPos = 0; m_runCount = 0; }
        QFile f(path); if (!f.open(QIODevice::ReadOnly)) return;
        if (f.size() < m_runPos) m_runPos = 0;
        f.seek(m_runPos);
        const QByteArray data = f.readAll(); m_runPos = f.pos();
        for (const QByteArray& line : data.split('\n')) {
            if (line.trimmed().isEmpty()) continue;
            const QJsonObject d = QJsonDocument::fromJson(line).object(); ++m_runCount;
            QStringList kv; for (auto it = d.begin(); it != d.end(); ++it) if (it.key() != "time" && it.key() != "decision") kv << QString("%1=%2").arg(it.key(), it.value().isString() ? it.value().toString() : QString::fromUtf8(QJsonDocument(QJsonArray{it.value()}).toJson(QJsonDocument::Compact)).mid(1).chopped(1));
            const QString text = QString("%1  %2  %3").arg(d["time"].toString().mid(11, 8), d["decision"].toString(), kv.join(" "));
            append("run", text); m_rLast->setText(text); 
        }
        m_rCount->setText(QString("%1 in %2").arg(m_runCount).arg(QFileInfo(path).fileName()));
    }
    void append(const QString& kind, const QString& text)
    {
        if ((kind == "event" && !m_showEv->isChecked()) || (kind == "run" && !m_showRun->isChecked())) return;
        m_log->appendPlainText((kind == "run" ? "[run]   " : "[event] ") + text);
        if (m_follow->isChecked()) m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
    }
    void applyNight(bool on)
    {
        qApp->setStyleSheet(on ? "QWidget { background: #100000; color: #ff4040; } QGroupBox { border: 1px solid #601010; margin-top: 8px; } QPushButton, QTabBar::tab { background: #200000; border: 1px solid #601010; padding: 4px; } QListWidget::item:selected, QTabBar::tab:selected { background: #500000; }" : QString());
    }

    Client* m_client = nullptr; QString m_dir; int m_port; double m_lat, m_lon; QTimer m_poll, m_files, m_liveTimer, m_fcTimer;
    QLabel *m_fcInfo = nullptr, *m_fcAcc = nullptr; QTableWidget* m_fcTable = nullptr; bool m_fcFresh = false;
    QLabel *m_conn, *m_tLink, *m_tState, *m_tMount, *m_tBatt, *m_pRaDec, *m_pAltAz, *m_pGoto, *m_pTarget, *m_fPos, *m_cExp, *m_cTemp, *m_sState, *m_sFrames, *m_sLast, *m_sCause, *m_sFocus, *m_sTiming,
           *m_aVerdict, *m_aWhy, *m_aSky, *m_rLast, *m_rCount, *m_liveInfo, *m_stackInfo;
    QCheckBox *m_night, *m_showEv, *m_showRun, *m_follow, *m_auto; QPlainTextEdit* m_log; ImageView *m_live, *m_stackView; QListWidget* m_list;
    qint64 m_evSeq = 0, m_runPos = 0; int m_runCount = 0; QString m_runPath;
};

}   // namespace

#include "WatchMain.moc"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    int port = qEnvironmentVariableIsSet("ORIGIN_CONTROL_PORT") ? qEnvironmentVariable("ORIGIN_CONTROL_PORT").toInt() : 8765;
    for (int i = 1; i + 1 < argc; ++i) if (QString(argv[i]) == "--port") port = QString(argv[i + 1]).toInt();
    double lat = 52.2054, lon = 0.1218;
    for (int i = 1; i + 1 < argc; ++i) { if (QString(argv[i]) == "--lat") lat = QString(argv[i + 1]).toDouble(); if (QString(argv[i]) == "--lon") lon = QString(argv[i + 1]).toDouble(); }
    Window w(port, originDataDir(), lat, lon);
    w.show();
    // --shot FILE [--tab N] [--stack N]: save a screenshot after a few seconds and exit (for documentation and testing)
    QString shot; int tab = 0, stack = 0;
    for (int i = 1; i + 1 < argc; ++i) { if (QString(argv[i]) == "--shot") shot = argv[i + 1]; if (QString(argv[i]) == "--tab") tab = QString(argv[i + 1]).toInt(); if (QString(argv[i]) == "--stack") stack = QString(argv[i + 1]).toInt(); }
    if (!shot.isEmpty()) {
        w.showTab(tab);
        QTimer::singleShot(1500, [&] { if (tab == 2) w.clickRender(); if (tab == 3) w.selectStack(stack); });
        QTimer::singleShot(5000, [&] { w.grab().save(shot); app.quit(); });
    }
    return app.exec();
}
