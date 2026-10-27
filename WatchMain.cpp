// OriginWatch: a window onto a running OriginHeadless daemon.
//   Now      what the telescope, the sequence and the unattended supervisor are doing
//   Log      recent telescope/daemon events and the supervisor's decisions
//   Live     the live stack, rendered on demand
//   Stacks   every stack in the data directory, finished or not
//   Forecast the blended multi-model weather forecast hour by hour (Open-Meteo, cached 1 h, same cache as OriginHeadless) and how accurate it has been
//   Tasks    (window only) override the target, change tonight's schedule, surveys, stop or park: see WatchTasks.hpp
// It talks to 127.0.0.1:<port> with a whitelist of read-only commands (see allowed()) and reads <data dir> files. Only the Tasks tab changes anything, by launching
// OriginHeadless or writing the run's control file, and only after asking; the phone view (web()) is read-only.
#include "DataDir.hpp"
#include "Weather.hpp"
#include "Astro.hpp"
#include "WatchWeb.hpp"
#include "WatchTasks.hpp"

#include <QApplication>
#include <QBuffer>
#include <QFormLayout>
#include <QCheckBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QWheelEvent>
#include <algorithm>
#include <QRegularExpression>
#include <QPixmap>
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
#include <QTextDocument>
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

// Image shown scaled to fit the widget (aspect kept). Mouse wheel zooms about the cursor, left-drag pans, double-click or "0" returns to fit.
class ImageView : public QWidget {
public:
    explicit ImageView(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(320, 240); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding); setFocusPolicy(Qt::ClickFocus);
        setToolTip("wheel: zoom   drag: pan   double-click: fit");
    }
    QImage image() const { return m_img; }
    void setImage(const QImage& im, const QString& caption)
    {
        if (im.size() != m_img.size()) resetView();                          // a refresh of the same size keeps the view
        m_img = im; m_caption = caption; update();
    }
    void resetView() { m_zoom = 1.0; m_cx = m_cy = 0.5; update(); }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this); p.fillRect(rect(), palette().window());
        if (m_img.isNull()) { p.setPen(palette().text().color()); p.drawText(rect(), Qt::AlignCenter, m_caption.isEmpty() ? "no image" : m_caption); return; }
        const double s = scale();
        const QPointF o = origin(s);                                         // widget position of image pixel (0,0)
        const QRectF src = QRectF(QPointF(-o.x() / s, -o.y() / s), QSizeF(width() / s, height() / s)).intersected(QRectF(QPointF(0, 0), QSizeF(m_img.size())));
        if (src.isEmpty()) return;
        const QRectF dst(o.x() + src.x() * s, o.y() + src.y() * s, src.width() * s, src.height() * s);
        p.setRenderHint(QPainter::SmoothPixmapTransform, s < 1.5);          // show real pixels once magnified
        p.drawImage(dst, m_img, src);
        p.setPen(Qt::white); p.drawText(8, height() - 8, QString("%1   %2%").arg(m_caption).arg(int(std::lround(s * 100))));
    }
    void wheelEvent(QWheelEvent* e) override
    {
        if (m_img.isNull()) return;
        const double s0 = scale();
        const QPointF pos = e->position(), ip = (pos - origin(s0)) / s0;     // image point under the cursor
        m_zoom = std::clamp(m_zoom * std::pow(1.0015, e->angleDelta().y()), 1.0, 40.0);
        const double s = scale();
        m_cx = ip.x() / m_img.width() + (width() / 2.0 - pos.x()) / (s * m_img.width());   // keep that point under the cursor
        m_cy = ip.y() / m_img.height() + (height() / 2.0 - pos.y()) / (s * m_img.height());
        clampCentre(); update(); e->accept();
    }
    void mousePressEvent(QMouseEvent* e) override { if (e->button() == Qt::LeftButton) { m_drag = e->position(); setCursor(Qt::ClosedHandCursor); } }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!(e->buttons() & Qt::LeftButton) || m_img.isNull()) return;
        const double s = scale(); const QPointF d = e->position() - m_drag; m_drag = e->position();
        m_cx -= d.x() / (s * m_img.width()); m_cy -= d.y() / (s * m_img.height()); clampCentre(); update();
    }
    void mouseReleaseEvent(QMouseEvent*) override { unsetCursor(); }
    void mouseDoubleClickEvent(QMouseEvent*) override { resetView(); }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_0) resetView();
        else if (e->key() == Qt::Key_Plus || e->key() == Qt::Key_Equal) { m_zoom = std::min(40.0, m_zoom * 1.5); clampCentre(); update(); }
        else if (e->key() == Qt::Key_Minus) { m_zoom = std::max(1.0, m_zoom / 1.5); clampCentre(); update(); }
        else QWidget::keyPressEvent(e);
    }
private:
    double scale() const { return std::min(double(width()) / m_img.width(), double(height()) / m_img.height()) * m_zoom; }
    QPointF origin(double s) const { return QPointF(width() / 2.0 - m_cx * m_img.width() * s, height() / 2.0 - m_cy * m_img.height() * s); }
    void clampCentre()                                                       // never pan past the image edge; an image smaller than the widget stays centred
    {
        const double s = scale(), hx = std::max(0.0, 0.5 - width() / (2 * s * m_img.width())), hy = std::max(0.0, 0.5 - height() / (2 * s * m_img.height()));
        m_cx = std::clamp(m_cx, 0.5 - hx, 0.5 + hx); m_cy = std::clamp(m_cy, 0.5 - hy, 0.5 + hy);
    }
    QImage m_img; QString m_caption; double m_zoom = 1.0, m_cx = 0.5, m_cy = 0.5; QPointF m_drag;
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
        setWindowTitle(QString("OriginWatch - daemon port %1").arg(port));
        auto* tabs = new QTabWidget; setCentralWidget(tabs);
        tabs->addTab(buildNow(), "Now"); tabs->addTab(buildLog(), "Log"); tabs->addTab(buildLive(), "Live stack"); tabs->addTab(buildStacks(), "Stacks"); tabs->addTab(buildForecast(), "Forecast");
        tabs->addTab(new TasksTab(m_dir, port, [this] { return QString("pointing %1 (alt/az %2), target %3").arg(m_pRaDec->text(), m_pAltAz->text(), m_pTarget->text()); }), "Tasks");
        m_night = new QCheckBox("Night (red) mode"); statusBar()->addPermanentWidget(m_night);
        connect(m_night, &QCheckBox::toggled, this, &Window::applyNight);
        m_conn = new QLabel("daemon: connecting..."); statusBar()->addWidget(m_conn);
        connect(m_client, &Client::state, this, [this](bool up) { m_conn->setText(up ? "daemon: connected" : QString("daemon: not reachable on port %1 (start it with OriginHeadless serve)").arg(m_port)); if (!up) clearNow(); });
        connect(&m_poll, &QTimer::timeout, this, &Window::poll); m_poll.start(2000);
        connect(&m_files, &QTimer::timeout, this, &Window::readRunLog); m_files.start(3000);
        connect(&m_fcTimer, &QTimer::timeout, this, &Window::reloadForecast); m_fcTimer.start(5 * 60 * 1000);
        resize(1100, 760); poll(); readRunLog(); reloadStacks(); QTimer::singleShot(200, this, &Window::reloadForecast);
    }
    void showTab(int i) { if (auto* t = findChild<QTabWidget*>()) t->setCurrentIndex(i); }
    void clickRender() { renderLive(); }
    void selectStack(int i) { if (i < m_list->count()) m_list->setCurrentRow(i); }

    // ---------------------------------------------------------------- the web view (see WatchWeb.hpp): the same information as the window, read-only
    static QByteArray pngOf(const QImage& im) { QByteArray b; QBuffer buf(&b); buf.open(QIODevice::WriteOnly); im.save(&buf, "PNG"); return b; }
    static QByteArray jpegOf(const QImage& im)             // photographs for a phone: at most 1400 px wide, about 10 times smaller than the PNG
    {
        QByteArray b; QBuffer buf(&b); buf.open(QIODevice::WriteOnly);
        (im.width() > 1400 ? im.scaledToWidth(1400, Qt::SmoothTransformation) : im).convertToFormat(QImage::Format_RGB32).save(&buf, "JPEG", 85); return b;
    }
    static QString esc(const QString& t) { return t.toHtmlEscaped(); }
    QString webPage(const QString& body, int refreshS) const
    {
        return QString("<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>%1<title>OriginWatch</title><style>"
                       ":root{color-scheme:dark light}body{font:15px -apple-system,system-ui,sans-serif;margin:0 0 3em;background:#0c0c10;color:#e6e6ea}"
                       "nav{display:flex;gap:4px;padding:8px;background:#1a1a22;position:sticky;top:0}nav a{flex:1;text-align:center;padding:8px 2px;border-radius:6px;background:#26262f;color:#9ec1ff;text-decoration:none}"
                       "main{padding:8px 12px}h2{font-size:15px;margin:14px 0 4px;color:#9ec1ff}table{width:100%;border-collapse:collapse}td{padding:3px 6px 3px 0;vertical-align:top;border-bottom:1px solid #22222a}"
                       "td:first-child{color:#9a9aa6;width:38%}table.fc td,table.fc th{text-align:center;padding:3px 2px;font-size:12px;white-space:nowrap}table.fc td:first-child{color:inherit;width:auto}details{margin:10px 0}summary{color:#9ec1ff;padding:6px 0}img{max-width:100%;height:auto;border-radius:6px}pre{white-space:pre-wrap;font:12px ui-monospace,monospace}.dim{color:#9a9aa6}"
                       "@media (prefers-color-scheme:light){body{background:#f4f4f7;color:#111}nav{background:#e2e2ea}nav a{background:#fff;color:#1a56c4}td{border-color:#ddd}td:first-child,.dim{color:#666}h2{color:#1a56c4}}"
                       "</style></head><body><nav><a href=/>Now</a><a href=/log>Log</a><a href=/live>Live</a><a href=/stacks>Stacks</a><a href=/forecast>Forecast</a><a href=/planner>Planner</a></nav><main>%2</main></body></html>")
            .arg(refreshS > 0 ? QString("<meta http-equiv=refresh content=%1>").arg(refreshS) : QString(), body);
    }
    QString nowHtml() const
    {
        QString h = QString("<p class=dim>%1</p>").arg(esc(m_conn->text()));
        auto* tabs = findChild<QTabWidget*>(); if (!tabs) return h;
        for (QGroupBox* g : tabs->widget(0)->findChildren<QGroupBox*>()) {
            auto* f = qobject_cast<QFormLayout*>(g->layout()); if (!f) continue;
            h += "<h2>" + esc(g->title()) + "</h2><table>";
            for (int r = 0; r < f->rowCount(); ++r) {
                auto* lab = qobject_cast<QLabel*>(f->itemAt(r, QFormLayout::LabelRole) ? f->itemAt(r, QFormLayout::LabelRole)->widget() : nullptr);
                auto* val = qobject_cast<QLabel*>(f->itemAt(r, QFormLayout::FieldRole) ? f->itemAt(r, QFormLayout::FieldRole)->widget() : nullptr);
                if (lab && val) h += "<tr><td>" + esc(lab->text()) + "</td><td>" + esc(val->text()) + "</td></tr>";
            }
            h += "</table>";
            if (g->title().startsWith("Sky") && !m_aPic->pixmap().isNull()) h += QString("<p><img src='/img/lastframe.jpg?v=%1' alt='last frame'></p>").arg(m_picStamp.toSecsSinceEpoch());
        }
        return h;
    }
    WebReply web(const QString& path, const QUrlQuery& q)
    {
        WebReply r; auto html = [&](const QString& body, int refresh) { r.body = webPage(body, refresh).toUtf8(); return r; };
        {   // the pages are only shown while the telescope reports that it is on the charger (a battery night ended at 23:18 on 2026-10-16): CHARGING, or full
            const bool fresh = m_chargerAtMs && QDateTime::currentMSecsSinceEpoch() - m_chargerAtMs < 60000;
            const QString c = m_charger.toUpper();
            const bool onCharger = fresh && !c.contains("DISCHARG") && (c.contains("CHARG") || c.contains("FULL"));
            if (path == "/gate/dismiss") { m_gateDismissed = true; r.status = 303; r.type = "text/plain"; r.body = "dismissed\n"; r.location = "/"; return r; }
            if (!onCharger && !m_gateDismissed) {
                r.status = 503;
                const QString why = !fresh ? "The telescope is not reporting its battery (powered off, or not connected to the daemon)."
                                           : QString("Charger status: <b>%1</b>, battery level: <b>%2</b>.").arg(esc(m_charger.isEmpty() ? "unknown" : m_charger), esc(m_batteryLevel.isEmpty() ? "unknown" : m_batteryLevel));
                r.body = webPage("<h2>Charger not connected</h2><p>" + why + "</p><p>These pages stay hidden until the telescope is plugged in and reports CHARGING or full. Connect the charger; this page re-checks every 10 seconds.</p><p><a href=/gate/dismiss>Dismiss this alert</a> (it returns when the telescope next starts talking after being off or out of reach)</p>", 10).toUtf8();
                return r;
            }
        }
        auto png = [&](const QImage& im, bool photo = false) { if (im.isNull()) { r.status = 404; r.type = "text/plain"; r.body = "no image\n"; } else if (photo) { r.type = "image/jpeg"; r.body = jpegOf(im); } else { r.type = "image/png"; r.body = pngOf(im); } return r; };
        if (path == "/planner" || path == "/planner.json") {        // tools/planner_data.py writes both into the data dir; static files, read-only
            QFile f(m_dir + (path == "/planner" ? "/planner.html" : "/planner.json"));
            if (!f.open(QIODevice::ReadOnly)) { r.status = 404; r.type = "text/plain"; r.body = "no planner data yet: run tools/planner_data.py\n"; return r; }
            r.type = path == "/planner" ? "text/html; charset=utf-8" : "application/json"; r.body = f.readAll(); return r;
        }
        if (path == "/" || path == "/now") return html(nowHtml(), 10);
        if (path == "/log") { const QStringList lines = m_log->toPlainText().split('\n'); return html("<pre>" + esc(lines.mid(std::max(0, int(lines.size()) - 300)).join('\n')) + "</pre>", 15); }
        if (path == "/live") {
            if (!m_webLive.isValid() || m_webLive.elapsed() > 20000) { renderLive(); m_webLive.restart(); }
            return html(QString("<p class=dim>%1</p><p><img src='/img/live.jpg?v=%2' alt='live stack'></p>").arg(esc(m_liveInfo->text())).arg(QDateTime::currentSecsSinceEpoch()), 30);
        }
        if (path == "/stacks") {
            reloadStacks(); QString h = "<h2>Stacks</h2><table>";
            for (int i = 0; i < m_list->count(); ++i) h += QString("<tr><td><a href='/stack/%1'>%2</a></td></tr>").arg(i).arg(esc(m_list->item(i)->text()));
            return html(h + "</table>", 0);
        }
        if (path.startsWith("/stack/")) {
            bool ok = false; const int i = path.mid(7).toInt(&ok); if (!ok || i < 0 || i >= m_list->count()) { r.status = 404; r.type = "text/plain"; r.body = "no such stack\n"; return r; }
            return html(QString("<p>%1</p><p><img src='/img/stack/%2.jpg' alt='stack'></p>").arg(esc(m_list->item(i)->text())).arg(i), 0);
        }
        if (path == "/forecast") {
            QString h = "<h2>Forecast and nowcast</h2>";
            if (!m_fcSky->pixmap().isNull() && !m_fcSky->isHidden()) h += "<p><img src='/img/allsky.jpg' alt='all-sky camera' style='max-width:40%'></p>";
            for (QLabel* l : {m_fcInfo, m_fcNow, m_fcAcc}) if (l && !l->text().isEmpty()) { QTextDocument d; d.setHtml(l->text()); h += "<p>" + esc(d.toPlainText()).replace("\n", "<br>") + "</p>"; }   // the labels are rich text; show their words only
            // the tables as real HTML (the page scrolls as one; nothing is clipped inside a picture): the nights, the hourly columns, then the per-model columns in a fold-out
            const bool day = q.queryItemValue("daylight") == "1";                  // dry daylight hours are left out unless asked for (rain always shows)
            auto table = [&](QTableWidget* tw, QList<int> cols) {
                QString t = "<table class=fc><tr>";
                for (int c : cols) t += "<th>" + esc(tw->horizontalHeaderItem(c) ? tw->horizontalHeaderItem(c)->text() : QString()) + "</th>";
                t += "</tr>";
                for (int r = 0; r < tw->rowCount(); ++r) {
                    if (!day && tw->item(r, 0) && tw->item(r, 0)->data(Qt::UserRole).toBool()) continue;
                    t += "<tr>";
                    for (int c : cols) {
                        const QTableWidgetItem* it = tw->item(r, c); QString style;
                        if (it && it->background().style() != Qt::NoBrush) style = " style='background:" + it->background().color().name() + ";color:#fff'";
                        t += "<td" + style + ">" + esc(it ? it->text() : QString()) + "</td>";
                    }
                    t += "</tr>";
                }
                return t + "</table>";
            };
            h += "<h2>Nights ahead</h2><p>" + m_fcNext->text() + "</p>" + table(m_fcNights, {0, 1, 2, 3, 4, 5});
            h += "<p class=dim>An outlook of when the sky might be usable again, not a prediction: dark = Sun below -12 deg, quality = mean over the dark hours, clear run = longest stretch at 0.5 or more. "
                 "The colours fade with distance; fewer models reach that far and beyond about 5 days they have little skill.</p>";
            h += QString("<h2>Next 48 hours</h2><p><a href='/forecast%1'>%2</a></p>").arg(day ? "" : "?daylight=1", day ? "Hide daylight hours" : "Show daylight hours");
            h += table(m_fcTable, {0, 1, 2, 3, 4, 5, 6, 7});
            h += "<details><summary>Quality by model</summary>" + table(m_fcTable, {0, 8, 9, 10, 11, 12, 13, 14}) + "</details>";
            h += "<p class=dim>Quality 0..1 = how much of the hour is usable. Green 0.5 or more, amber 0.3-0.5, red below. A red rain cell is rain (chance 40% or more, or 0.1 mm); those hours show even in daylight.</p>";
            return html(h, 300);
        }
        if (path == "/img/allsky.jpg") return png(m_fcSky->pixmap().toImage(), true);
        if (path == "/img/lastframe.jpg") return png(m_aPic->pixmap().toImage(), true);
        if (path == "/img/live.jpg") return png(m_live->image(), true);
        if (path.startsWith("/img/stack/") && path.endsWith(".jpg")) {
            bool ok = false; const int i = path.mid(11, path.size() - 15).toInt(&ok); if (!ok || i < 0 || i >= m_list->count()) { r.status = 404; r.type = "text/plain"; r.body = "no such stack\n"; return r; }
            return png(QImage(m_list->item(i)->data(Qt::UserRole).toString() + "/preview.png"), true);
        }
        r.status = 404; r.type = "text/plain"; r.body = "not found\n"; return r;
    }
private:
    // ---------------------------------------------------------------- Forecast
    QWidget* buildForecast()
    {
        auto* w = new QWidget; auto* v = new QVBoxLayout(w);
        auto* top = new QHBoxLayout; auto* refresh = new QPushButton("Refresh now"); m_fcInfo = new QLabel("fetching..."); m_fcInfo->setWordWrap(true);
        top->addWidget(m_fcInfo, 1); top->addWidget(refresh); v->addLayout(top);
        auto* nowBox = new QHBoxLayout; m_fcSky = new QLabel; m_fcSky->setFixedSize(130, 160); m_fcSky->setScaledContents(true); m_fcSky->hide();
        m_fcNow = new QLabel("nowcast..."); m_fcNow->setWordWrap(true); m_fcNow->setTextFormat(Qt::RichText); m_fcNow->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        nowBox->addWidget(m_fcSky); nowBox->addWidget(m_fcNow, 1); v->addLayout(nowBox);
        m_fcNext = new QLabel; m_fcNext->setWordWrap(true); m_fcNext->setTextFormat(Qt::RichText); v->addWidget(m_fcNext);
        m_fcNights = new QTableWidget(0, 6); m_fcNights->setEditTriggers(QAbstractItemView::NoEditTriggers); m_fcNights->setSelectionMode(QAbstractItemView::NoSelection);
        m_fcNights->setHorizontalHeaderLabels({"Night", "Dark (local)", "Quality", "Clear run", "Range", "Models"});
        m_fcNights->verticalHeader()->hide(); m_fcNights->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        v->addWidget(m_fcNights, 1);
        m_fcDay = new QCheckBox("Show daylight hours"); v->addWidget(m_fcDay);
        connect(m_fcDay, &QCheckBox::toggled, this, [this] { applyDaylight(); });
        m_fcTable = new QTableWidget(0, 15); m_fcTable->setEditTriggers(QAbstractItemView::NoEditTriggers); m_fcTable->setSelectionMode(QAbstractItemView::NoSelection);
        m_fcTable->setHorizontalHeaderLabels({"Local", "Quality", "Range", "Cloud %", "Low", "Mid", "High", "Rain %", "Met Office", "ECMWF", "AIFS", "ICON", "GFS", "M-France", "KNMI"});
        m_fcTable->verticalHeader()->hide(); m_fcTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        m_fcTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents); m_fcTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);   // "Mon 00:00" and "0.00 - 0.86" must not be elided
        m_fcTable->horizontalHeader()->setMinimumSectionSize(40); m_fcTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        v->addWidget(m_fcTable, 1);
        m_fcAcc = new QLabel; m_fcAcc->setWordWrap(true); v->addWidget(m_fcAcc);
        auto* legend = new QLabel("Quality 0..1 = how much of the hour is usable (low and mid cloud block, high cloud counts half, cut for rain, fog, dew and wind). It is the mean over 7 models; the models often disagree by 50 points, so look at the range as well. Green >= 0.5, amber 0.3-0.5, red below. "
                                  "Nights: dark = Sun below -12 deg; quality = mean over the dark hours; clear run = longest stretch of dark hours at 0.5 or more. A rough idea of when the sky might be usable again, not a prediction: "
                                  "the colours fade with distance, fewer models reach that far (7 to 4.5 days, 6 to 7.5 days, 4 to 15 days) and beyond about 5 days they have little skill.");
        legend->setWordWrap(true); v->addWidget(legend);
        connect(refresh, &QPushButton::clicked, this, [this] { m_fcFresh = true; reloadForecast(); });
        return w;
    }
    static QColor tint(double q) { return q >= 0.5 ? QColor(20, 110, 40) : q >= 0.3 ? QColor(150, 110, 20) : QColor(120, 30, 30); }
    static QColor faded(const QColor& c, int leadDays)                // towards grey with distance: the far nights are an outlook only
    {
        const double f = std::clamp((leadDays - 2) / 12.0, 0.0, 0.65);
        return QColor(int(c.red() * (1 - f) + 85 * f), int(c.green() * (1 - f) + 85 * f), int(c.blue() * (1 - f) + 85 * f));
    }
    void applyDaylight()
    {
        for (int r = 0; r < m_fcTable->rowCount(); ++r) { const QTableWidgetItem* it = m_fcTable->item(r, 0); m_fcTable->setRowHidden(r, !m_fcDay->isChecked() && it && it->data(Qt::UserRole).toBool()); }
    }
    void showNowcast(const QJsonObject& r, const QVector<Weather::Hour>& hrs, const QTimeZone& zone)
    {
        const QJsonObject mt = r["metar"].toObject();
        if (!mt["ok"].toBool()) { m_fcNow->setText("<b>Nowcast:</b> no observations available (all-sky camera and airport reports unreachable); the forecast below is the models alone."); m_fcSky->hide(); return; }
        QString sky = "<i>none</i>", obs = "<i>none</i>", ap;
        for (const QJsonValue& v : mt["stations"].toArray()) {
            const QJsonObject o = v.toObject(); const QString id = o["id"].toString();
            if (id == "allsky") sky = o.contains("error") ? "<i>unavailable: " + o["error"].toString() + "</i>" : QString("<b>%1%</b> cloud (%2)").arg(o["cover"].toDouble() * 100, 0, 'f', 0).arg(o["raw"].toString().section(", ", 1).left(19));
            else if (id == "observer") obs = QString("<b>%1%</b>").arg(o["cover"].toDouble() * 100, 0, 'f', 0);
            else { QString raw = o["raw"].toString(); const QRegularExpressionMatch m = QRegularExpression("(NCD|CLR|SKC|(?:FEW|SCT|BKN|OVC)\\d+)").match(raw);
                   ap += QString("%1 %2 km %3; ").arg(id).arg(o["km"].toInt()).arg(m.hasMatch() ? m.captured(1) : o["cover"].toDouble() == 0 ? QString("clear") : QString("?")); }
        }
        if (QFile::exists(m_dir + "/allsky_latest.jpg") && mt["stations"].toArray().size() > 0) { QPixmap pm(m_dir + "/allsky_latest.jpg"); if (!pm.isNull()) { m_fcSky->setPixmap(pm); m_fcSky->show(); } }
        const Weather::Hour* cur = Weather::at(hrs, QDateTime::currentDateTimeUtc());
        QString eff = cur && cur->qModel >= 0 ? QString("Current hour: models alone %1 quality, with the nowcast <b>%2</b> (observation weight %3%).").arg(cur->qModel, 0, 'f', 2).arg(cur->quality, 0, 'f', 2).arg(cur->obsWeight * 100, 0, 'f', 0) : QString();
        m_fcNow->setText(QString("<b>Nowcast: observations now</b> (low/mid cloud fraction; steers the models, fading over about 3 hours)<br>"
                                 "All-sky camera, Institute of Astronomy: %1<br>Your sky-report: %2 (OriginHeadless sky-report --cover N)<br>Airports: %3<br>"
                                 "<b>Combined observed cover: %4%</b> at %5 local. %6")
                             .arg(sky, obs, ap.isEmpty() ? "<i>none</i>" : ap.left(ap.size() - 2)).arg(mt["cover_pct"].toDouble(), 0, 'f', 0)
                             .arg(QDateTime::fromString(mt["obs_time"].toString(), Qt::ISODate).toTimeZone(zone).toString("HH:mm"), eff));
    }
    // one row per night to the end of the forecast: when might the sky be usable again (an outlook, the forecast is not trusted for more)
    template <typename SunAlt> void showNights(const QVector<Weather::Hour>& hrs, const QTimeZone& zone, const QDateTime& now, SunAlt sunAlt)
    {
        m_fcNights->setRowCount(0); QString next;
        const QDate first = now.toTimeZone(zone).addSecs(-12 * 3600).date();          // the night we are in (until local noon)
        for (int d = 0; d < 16; ++d) {
            const QDate date = first.addDays(d);
            const QDateTime noon = QDateTime(date, QTime(12, 0), zone).toUTC();
            QDateTime darkFrom, darkTo;
            int dark = 0, covered = 0, run = 0, best = 0, nm = 99; double sq = 0, smin = 0, smax = 0;
            for (int k = 0; k < 25; ++k) {
                const QDateTime t = noon.addSecs(k * 3600);
                if (sunAlt(t.addSecs(1800)) >= -12) { run = 0; continue; }
                if (!darkFrom.isValid()) darkFrom = t;
                darkTo = t.addSecs(3600);
                if (t.addSecs(3600) <= now) continue;                                   // hours already gone tonight
                ++dark; const Weather::Hour* h = Weather::at(hrs, t);
                if (!h || std::abs(h->utc.secsTo(t)) > 1800 || h->models.size() < 3) { run = 0; continue; }   // past ECMWF's 15 days only GFS is left: one model is no outlook
                ++covered; sq += h->quality; smin += h->qMin; smax += h->qMax; nm = std::min(nm, int(h->models.size()));
                run = h->quality >= 0.5 ? run + 1 : 0; best = std::max(best, run);
            }
            if (dark == 0 || covered * 2 < dark) { if (d == 0) continue; break; }          // night over, or past the end of the forecast
            const double q = sq / covered; const int r = m_fcNights->rowCount(); m_fcNights->insertRow(r);
            auto put = [&](int c, const QString& txt, const QColor& bg = QColor()) { auto* it = new QTableWidgetItem(txt); it->setTextAlignment(Qt::AlignCenter); if (bg.isValid()) { it->setBackground(faded(bg, d)); it->setForeground(Qt::white); } m_fcNights->setItem(r, c, it); };
            put(0, d == 0 ? "Tonight" : date.toString("ddd d MMM"));
            put(1, darkFrom.toTimeZone(zone).toString("HH:mm") + " - " + darkTo.toTimeZone(zone).toString("HH:mm"));
            put(2, QString::number(q, 'f', 2), tint(q)); put(3, QString("%1 h").arg(best), best >= 2 ? tint(1) : best == 1 ? tint(0.4) : tint(0));
            put(4, QString("%1 - %2").arg(smin / covered, 0, 'f', 2).arg(smax / covered, 0, 'f', 2)); put(5, QString::number(nm));
            if (next.isEmpty() && best >= 2)
                next = QString("<b>%1</b>: about %2 h of dark sky at quality 0.5 or more (%3 models%4).")
                           .arg(d == 0 ? QString("Tonight might be usable") : "Next night that might be usable: " + date.toString("ddd d MMM"))
                           .arg(best).arg(nm).arg(d >= 5 ? QString("; little skill this far ahead") : QString());
        }
        m_fcNext->setText(next.isEmpty() ? QString("<b>No night in the forecast reaches 2 hours of quality 0.5</b> (%1 nights ahead).").arg(m_fcNights->rowCount()) : next);
    }
    void reloadForecast()
    {
        m_fcInfo->setText("fetching..."); qApp->processEvents();
        QVector<Weather::Hour> hrs; const QJsonObject r = Weather::fetch(m_lat, m_lon, 16, m_fcFresh ? 0 : 60, m_dir, &hrs); m_fcFresh = false;
        if (!r["ok"].toBool()) { m_fcInfo->setText("forecast unavailable: " + r["error"].toString()); return; }
        const QTimeZone zone("Europe/London"); const QDateTime now = QDateTime::currentDateTimeUtc();
        m_fcInfo->setText(QString("%1, fetched %2 (local). %3 Site %4 N %5 E.").arg(r["source"].toString(), QDateTime::fromString(r["fetched"].toString(), Qt::ISODate).toTimeZone(zone).toString("ddd HH:mm"),
                                                                                 r["note"].toString(), QString::number(m_lat, 'f', 2), QString::number(m_lon, 'f', 2)));
        showNowcast(r, hrs, zone);
        static const QStringList ids = {"ukmo_seamless", "ecmwf_ifs025", "ecmwf_aifs025_single", "icon_seamless", "gfs_seamless", "meteofrance_seamless", "knmi_seamless"};
        const Astro::Site site{m_lat, m_lon};
        auto sunAlt = [&](const QDateTime& t) { return Astro::altAz(Astro::sunPosition(t), site, t).alt; };
        m_fcTable->setRowCount(0);
        int row = 0;
        for (const Weather::Hour& h : hrs) {
            if (h.utc < now.addSecs(-3600) || h.utc > now.addSecs(48 * 3600)) continue;
            m_fcTable->insertRow(row);
            auto put = [&](int c, const QString& t, const QColor& bg = QColor()) { auto* it = new QTableWidgetItem(t); it->setTextAlignment(Qt::AlignCenter); if (bg.isValid()) { it->setBackground(bg); it->setForeground(Qt::white); } m_fcTable->setItem(row, c, it); };
            put(0, h.utc.toTimeZone(zone).toString("ddd HH:mm")); put(1, QString::number(h.quality, 'f', 2), tint(h.quality));
            const bool wet = h.precipProb >= 40 || h.precip >= 0.1;                                // rain as Weather::quality counts it
            m_fcTable->item(row, 0)->setData(Qt::UserRole, sunAlt(h.utc.addSecs(1800)) > 0 && !wet);  // dry daylight: hidden unless asked for; rain always shows
            put(2, h.models.isEmpty() ? "-" : QString("%1 - %2").arg(h.qMin, 0, 'f', 2).arg(h.qMax, 0, 'f', 2));
            put(3, QString::number(h.cloud, 'f', 0)); put(4, QString::number(h.low, 'f', 0)); put(5, QString::number(h.mid, 'f', 0)); put(6, QString::number(h.high, 'f', 0)); put(7, QString::number(h.precipProb, 'f', 0), wet ? tint(0) : QColor());
            for (int i = 0; i < ids.size(); ++i) { if (h.models.contains(ids[i])) put(8 + i, QString::number(h.models[ids[i]], 'f', 2), tint(h.models[ids[i]])); else put(8 + i, "-"); }
            ++row;
        }
        applyDaylight();
        showNights(hrs, zone, now, sunAlt);
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
        group("Sky (last analysis)", {{"verdict", &m_aVerdict}, {"why", &m_aWhy}, {"sky", &m_aSky}, {"pointing", &m_aPoint}, {"plate solve", &m_aSolve}}, 2, 0);
        {   // the last frame as a picture, to check for trees, roofs and cloud
            m_aPic = new QLabel("no frame yet"); m_aPic->setAlignment(Qt::AlignCenter); m_aPic->setMinimumSize(360, 240); m_aPic->setStyleSheet("background:#000; color:#888");
            static_cast<QFormLayout*>(grid->itemAtPosition(2, 0)->widget()->layout())->addRow(m_aPic);
        }
        group("Supervisor", {{"last decision", &m_rLast}, {"decisions", &m_rCount}}, 2, 1);
        m_aWhy->setWordWrap(true); m_aSolve->setWordWrap(true); m_aPoint->setWordWrap(true); m_rLast->setWordWrap(true); m_sCause->setWordWrap(true);
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
        m_client->request({{"cmd", "analysis"}}, [this](const QJsonObject& r) { if (r["ok"].toBool()) onAnalysis(r["last"].toObject(), r["solve"].toObject()); });
        m_client->request({{"cmd", "target"}}, [this](const QJsonObject& r) { if (r["ok"].toBool()) m_pTarget->setText(r["target"].toString() + (r["stack"].toObject().contains("frames") ? QString("  (stack %1 frames)").arg(r["stack"].toObject()["frames"].toInt()) : QString())); });
        m_client->request({{"cmd", "events"}, {"since", double(m_evSeq)}, {"limit", 200}}, [this](const QJsonObject& r) { if (r["ok"].toBool()) onEvents(r); });
    }
    void clearNow() { for (QLabel* l : {m_tLink, m_tState, m_tMount, m_tBatt, m_pRaDec, m_pAltAz, m_pGoto, m_pTarget, m_fPos, m_cExp, m_cTemp, m_sState, m_sFrames, m_sLast, m_sCause, m_sFocus, m_sTiming, m_aVerdict, m_aWhy, m_aSky, m_aPoint, m_aSolve}) l->setText("-"); }
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
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (!m_chargerAtMs || nowMs - m_chargerAtMs >= 60000) m_gateDismissed = false;   // the telescope talks again after a silence (the gate's 60 s): a dismissed alert re-arms
        m_charger = m["ChargerStatus"].toString(); m_batteryLevel = m["BatteryLevel"].toString(); m_chargerAtMs = nowMs;
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
    void onAnalysis(const QJsonObject& a, const QJsonObject& solve)
    {
        const QTimeZone zone("Europe/London");
        // the latest plate-solve attempt (solved, skipped as hopeless by the frame statistics, or failed)
        if (solve.isEmpty()) m_aSolve->setText("not attempted yet");
        else {
            const QDateTime t = QDateTime::fromString(solve["time"].toString(), Qt::ISODate); const int ago = int(t.secsTo(QDateTime::currentDateTimeUtc()) / 60);
            const QString when = QString("%1 (%2 min ago)").arg(t.toTimeZone(zone).toString("HH:mm:ss")).arg(ago);
            QString what;
            if (solve["solved"].toBool()) what = QString("<b>solved</b>: %1").arg(solve["summary"].toString());
            else if (solve["skipped"].toBool()) what = QString("skipped (%1): exposure %2 s, %3 stars, sky %4 ADU/s; next try %5 s").arg(solve["verdict"].toString()).arg(solve["exposure_s"].toDouble(), 0, 'g', 3).arg(solve["stars"].toInt())
                                                           .arg(solve["sky_rate"].toDouble(), 0, 'g', 3).arg(solve["suggested_exposure_s"].toDouble(), 0, 'g', 3);
            else what = "no solution: " + solve["error"].toString();
            m_aSolve->setTextFormat(Qt::RichText); m_aSolve->setText(when + "  " + what);
        }
        if (a.isEmpty()) return;
        const QString src = a["pointing_source"].toString("mount");
        m_aPoint->setText(src == "unknown" ? "unknown (mount not aligned, no plate solve): geometry, stack and obstruction checks are skipped"
                                          : QString("%1: RA %2 h, Dec %3°, alt %4°, az %5°").arg(src == "plate_solve" ? "plate solve" : "mount").arg(a["ra_hours"].toDouble(), 0, 'f', 2).arg(a["dec_deg"].toDouble(), 0, 'f', 1)
                                                .arg(a["alt_deg"].toDouble(), 0, 'f', 0).arg(a["az_deg"].toDouble(), 0, 'f', 0));
        m_aVerdict->setText(a["verdict"].toString());
        QStringList why; for (const auto& v : a["reasons"].toArray()) why << v.toString(); for (const auto& v : a["cause_reasons"].toArray()) why << v.toString();
        m_aWhy->setText(why.isEmpty() ? "-" : why.join("; "));
        m_aSky->setText(QString("%1sky %2 ADU/s, moon %3%, sun %4°").arg(a.contains("alt_deg") ? QString("alt %1°, ").arg(a["alt_deg"].toDouble(), 0, 'f', 0) : QString()).arg(a["sky_rate"].toDouble(), 0, 'f', 0).arg(int(a["moon_illumination"].toDouble() * 100)).arg(a["sun_alt_deg"].toDouble(), 0, 'f', 0));
        // the picture the daemon renders for the eye (at most every 15 s)
        const QString pic = m_dir + "/previews/last_frame.png"; const QFileInfo fi(pic);
        if (fi.exists() && fi.lastModified() != m_picStamp) {
            QPixmap pm(pic); if (!pm.isNull()) { m_aPic->setPixmap(pm.scaledToWidth(std::max(360, m_aPic->width() - 4), Qt::SmoothTransformation)); m_picStamp = fi.lastModified();
                m_aPic->setToolTip(QString("last frame, %1 (stretched; check for trees, roofs, cloud)").arg(fi.lastModified().toString("HH:mm:ss"))); }
        }
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

    QString m_charger, m_batteryLevel; qint64 m_chargerAtMs = 0; bool m_gateDismissed = false;       // latest ChargerStatus / BatteryLevel from the mount, and when
    Client* m_client = nullptr; QString m_dir; int m_port; double m_lat, m_lon; QTimer m_poll, m_files, m_liveTimer, m_fcTimer;
    QDateTime m_picStamp; QElapsedTimer m_webLive;
    QLabel *m_fcInfo = nullptr, *m_fcAcc = nullptr, *m_fcNow = nullptr, *m_fcSky = nullptr; QTableWidget* m_fcTable = nullptr; bool m_fcFresh = false;
    QLabel* m_fcNext = nullptr; QTableWidget* m_fcNights = nullptr; QCheckBox* m_fcDay = nullptr;
    QLabel *m_conn, *m_tLink, *m_tState, *m_tMount, *m_tBatt, *m_pRaDec, *m_pAltAz, *m_pGoto, *m_pTarget, *m_fPos, *m_cExp, *m_cTemp, *m_sState, *m_sFrames, *m_sLast, *m_sCause, *m_sFocus, *m_sTiming,
           *m_aVerdict, *m_aWhy, *m_aSky, *m_aPoint, *m_aSolve, *m_aPic, *m_rLast, *m_rCount, *m_liveInfo, *m_stackInfo;
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
    // --web-port N [--web-cert F --web-key F] [--web-bind ADDR] [--web-token-file F]: serve the window's information read-only over HTTP (HTTPS with a certificate) to private-network
    // peers only. Run headless with: OriginWatch -platform offscreen --web-port 8443 ...
    int webPort = 0; QString cert, key, bind, tokenFile;
    for (int i = 1; i + 1 < argc; ++i) { const QString a = argv[i], v = argv[i + 1]; if (a == "--web-port") webPort = v.toInt(); if (a == "--web-cert") cert = v; if (a == "--web-key") key = v; if (a == "--web-bind") bind = v; if (a == "--web-token-file") tokenFile = v; }
    WebServer* web = nullptr;
    if (webPort > 0) {
        QString token; if (!tokenFile.isEmpty()) { QFile tf(tokenFile); if (tf.open(QIODevice::ReadOnly)) token = QString::fromUtf8(tf.readAll()).trimmed(); else { fprintf(stderr, "cannot read %s\n", qPrintable(tokenFile)); return 2; } }
        web = new WebServer([&w](const QString& p, const QUrlQuery& q) { return w.web(p, q); }, token, &app);
        QString err;
        if (!web->start(quint16(webPort), cert, key, bind.isEmpty() ? QHostAddress(QHostAddress::Any) : QHostAddress(bind), &err)) { fprintf(stderr, "web server: %s\n", qPrintable(err)); return 2; }
        fprintf(stderr, "OriginWatch web view on %s://*:%d/ (private networks only%s)\n", web->tls() ? "https" : "http", webPort, token.isEmpty() ? "" : ", token required");
    }
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
