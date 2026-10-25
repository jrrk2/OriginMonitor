#pragma once
// WatchTasks.hpp - OriginWatch's Tasks tab (desktop window only; the phone view stays read-only): the common things done to a night by hand.
//   Override the current target    with an unattended run in progress: a request in <data dir>/run_control.json, which the supervisor acts on within about 20 s
//                                  (it observes the target for the minutes given, then resumes its plan); without a run: OriginHeadless go/goto + sequence
//   Modify the schedule            boosts (~/.config/origin/target_boost.json, kept between nights) and tonight's skip list (~/.config/origin/target_skip.json),
//                                  then a replan request to a run in progress
//   Surveys                        OriginHeadless survey (obstruction, twilight) and sky-survey (sky brightness); refused while a run holds the telescope
//   Stop                           stop the run (parking or not), or without a run stop the sequence / park
// The socket client of OriginWatch keeps its read-only whitelist: everything here is done by launching OriginHeadless, one task at a time, or by writing the
// control file. Each action that moves the telescope asks first and says where it is pointing now.

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimeZone>
#include <QTimer>
#include <QVBoxLayout>
#include <functional>
#include <signal.h>

class TasksTab : public QWidget {
public:
    TasksTab(const QString& dataDir, int port, std::function<QString()> pointing, QWidget* parent = nullptr)
        : QWidget(parent), m_dir(dataDir), m_port(port), m_pointing(std::move(pointing))
    {
        auto* v = new QVBoxLayout(this);
        auto* top = new QHBoxLayout; v->addLayout(top, 1);
        auto* left = new QVBoxLayout; auto* right = new QVBoxLayout; top->addLayout(left, 1); top->addLayout(right, 1);

        // the run
        auto* gRun = new QGroupBox("Unattended run"); auto* vr = new QVBoxLayout(gRun);
        m_runLbl = new QLabel("-"); m_runLbl->setWordWrap(true); m_runLbl->setTextFormat(Qt::RichText); vr->addWidget(m_runLbl);
        m_plan = new QListWidget; m_plan->setMaximumHeight(110); vr->addWidget(m_plan);
        auto* hs = new QHBoxLayout; m_stopPark = new QPushButton("Stop run and park"); m_stopStay = new QPushButton("Stop run, stay pointed");
        m_stopSeq = new QPushButton("Stop sequence"); m_park = new QPushButton("Park");
        for (QPushButton* b : {m_stopPark, m_stopStay, m_stopSeq, m_park}) hs->addWidget(b);
        vr->addLayout(hs); left->addWidget(gRun);

        // override
        auto* gOv = new QGroupBox("Override the current target"); auto* fo = new QFormLayout(gOv);
        m_target = new QComboBox; m_target->setEditable(true); m_target->setInsertPolicy(QComboBox::NoInsert); m_target->setToolTip("a target observable tonight, or any name when RA/Dec are given");
        m_ra = new QLineEdit; m_ra->setPlaceholderText("RA hours (optional, J2000)"); m_dec = new QLineEdit; m_dec->setPlaceholderText("Dec degrees (optional, J2000)");
        auto* hc = new QHBoxLayout; hc->addWidget(m_ra); hc->addWidget(m_dec);
        m_minutes = new QSpinBox; m_minutes->setRange(5, 600); m_minutes->setValue(60); m_minutes->setSuffix(" min");
        m_exposure = new QDoubleSpinBox; m_exposure->setRange(1, 60); m_exposure->setValue(10); m_exposure->setSuffix(" s"); m_exposure->setToolTip("used without a run; a run chooses its own exposure");
        auto* ho = new QHBoxLayout; m_override = new QPushButton("Override now"); m_endOverride = new QPushButton("End override"); ho->addWidget(m_override); ho->addWidget(m_endOverride);
        fo->addRow("Target", m_target); fo->addRow("or coordinates", hc); fo->addRow("For", m_minutes); fo->addRow("Exposure (no run)", m_exposure); fo->addRow(ho);
        left->addWidget(gOv);

        // surveys
        auto* gSv = new QGroupBox("Surveys"); auto* vs = new QVBoxLayout(gSv);
        auto* hv = new QHBoxLayout; m_samples = new QSpinBox; m_samples->setRange(3, 30); m_samples->setValue(8); m_samples->setPrefix("samples ");
        m_obsSurvey = new QPushButton("Obstruction survey"); m_skySurvey = new QPushButton("Sky brightness survey");
        hv->addWidget(m_obsSurvey); hv->addWidget(m_samples); hv->addWidget(m_skySurvey); vs->addLayout(hv);
        m_sky = new QTableWidget(0, 6); m_sky->setHorizontalHeaderLabels({"Alt", "Az", "Delta mag", "mag/arcsec2", "Stars", "Moon sep"});
        m_sky->verticalHeader()->hide(); m_sky->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); m_sky->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_skyLbl = new QLabel; m_skyLbl->setWordWrap(true); vs->addWidget(m_skyLbl); vs->addWidget(m_sky, 1);
        left->addWidget(gSv, 1);

        // schedule
        auto* gSc = new QGroupBox("Tonight's schedule"); auto* vc = new QVBoxLayout(gSc);
        auto* hl = new QHBoxLayout; m_schedLbl = new QLabel("targets observable tonight"); m_reload = new QPushButton("Reload"); hl->addWidget(m_schedLbl, 1); hl->addWidget(m_reload); vc->addLayout(hl);
        m_sched = new QTableWidget(0, 5); m_sched->setHorizontalHeaderLabels({"Target", "Window (local)", "Priority", "Boost", "Skip tonight"});
        m_sched->verticalHeader()->hide(); m_sched->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); m_sched->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_sched->setEditTriggers(QAbstractItemView::NoEditTriggers); vc->addWidget(m_sched, 1);
        m_save = new QPushButton("Save (and replan a run in progress)"); vc->addWidget(m_save);
        vc->addWidget(new QLabel("Boost: added to the planner's priority, kept for later nights (target_boost.json); a boosted target wins whenever it is observable. "
                                 "Skip: left out of tonight's plan and backups only."));
        right->addWidget(gSc, 1);

        // output of the tasks
        auto* ht = new QHBoxLayout; m_taskLbl = new QLabel("no task running"); m_cancel = new QPushButton("Cancel task"); ht->addWidget(m_taskLbl, 1); ht->addWidget(m_cancel); v->addLayout(ht);
        m_out = new QPlainTextEdit; m_out->setReadOnly(true); m_out->setMaximumBlockCount(2000); m_out->setMaximumHeight(170); v->addWidget(m_out);

        connect(m_override, &QPushButton::clicked, this, [this] { doOverride(); });
        connect(m_endOverride, &QPushButton::clicked, this, [this] { doEndOverride(); });
        connect(m_stopPark, &QPushButton::clicked, this, [this] { doStopRun(true); });
        connect(m_stopStay, &QPushButton::clicked, this, [this] { doStopRun(false); });
        connect(m_stopSeq, &QPushButton::clicked, this, [this] { if (confirm("Stop the sequence that is running?")) runChain({{"sequence", "--action", "stop", "--reason", "stopped from OriginWatch"}}, "stop sequence"); });
        connect(m_park, &QPushButton::clicked, this, [this] { if (confirm("Park the telescope (save the stack, goto Polaris)?\n\nNow: " + m_pointing())) runChain({{"park"}}, "park"); });
        connect(m_obsSurvey, &QPushButton::clicked, this, [this] { if (confirm(QString("Run the obstruction survey (%1 places of tonight's plan, short frames, slews between them)?\n\nNow: %2").arg(m_samples->value()).arg(m_pointing()))) runChain({{"survey", "--samples", QString::number(m_samples->value())}}, "obstruction survey"); });
        connect(m_skySurvey, &QPushButton::clicked, this, [this] { if (confirm("Run the sky brightness survey (up to 13 places across the sky, one frame each, slews between them)?\n\nNow: " + m_pointing())) runChain({{"sky-survey"}}, "sky brightness survey", [this] { loadSkySurvey(); }); });
        connect(m_cancel, &QPushButton::clicked, this, [this] { if (m_proc) { m_chain.clear(); m_proc->kill(); say("task cancelled"); } });
        connect(m_reload, &QPushButton::clicked, this, [this] { loadSchedule(); });
        connect(m_save, &QPushButton::clicked, this, [this] { saveSchedule(); });
        connect(&m_timer, &QTimer::timeout, this, [this] { refreshRun(); }); m_timer.start(5000);
        refreshRun(); loadSkySurvey(); QTimer::singleShot(500, this, [this] { loadSchedule(); });
    }

private:
    // ---- helpers
    QString headless() const
    {
        const QString env = qEnvironmentVariable("ORIGIN_HEADLESS");
        if (!env.isEmpty()) return env;
        const QString near = QCoreApplication::applicationDirPath() + "/../../headless/build/OriginHeadless";       // build/watch/build -> build/headless/build
        return QFileInfo::exists(near) ? QFileInfo(near).canonicalFilePath() : QString("OriginHeadless");
    }
    QProcessEnvironment env() const
    {
        QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
        e.insert("ORIGIN_CONTROL_PORT", QString::number(m_port)); e.insert("ORIGIN_DATA_DIR", m_dir); return e;
    }
    void say(const QString& t) { m_out->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss  ") + t); }
    bool confirm(const QString& q) { return QMessageBox::question(this, "OriginWatch", q, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes; }
    static QJsonObject readJson(const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject(); }
    static bool writeJson(const QString& path, const QJsonObject& o) { QSaveFile f(path); if (!f.open(QIODevice::WriteOnly)) return false; f.write(QJsonDocument(o).toJson()); return f.commit(); }
    static QString cfg(const QString& name) { QDir().mkpath(QDir::homePath() + "/.config/origin"); return QDir::homePath() + "/.config/origin/" + name; }

    // OriginHeadless commands one after another; the chain stops at the first that fails. One task at a time.
    void runChain(QList<QStringList> cmds, const QString& what, std::function<void()> done = nullptr)
    {
        if (m_proc) { say("busy: " + m_taskLbl->text() + "; cancel it first"); return; }
        m_chain = cmds; m_what = what; m_done = std::move(done); next();
    }
    void next()
    {
        if (m_chain.isEmpty()) { m_taskLbl->setText("no task running"); say(m_what + ": finished"); if (m_done) m_done(); m_done = nullptr; refreshRun(); return; }
        const QStringList args = m_chain.takeFirst();
        auto* p = new QProcess(this); m_proc = p; p->setProcessEnvironment(env()); p->setProcessChannelMode(QProcess::SeparateChannels);
        m_taskLbl->setText(m_what + ": OriginHeadless " + args.join(' ')); say("OriginHeadless " + args.join(' '));
        connect(p, &QProcess::readyReadStandardError, this, [this, p] { for (const QByteArray& l : p->readAllStandardError().split('\n')) if (!l.trimmed().isEmpty()) say("  " + QString::fromUtf8(l).left(400)); });
        connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
            const QJsonObject r = QJsonDocument::fromJson(p->readAllStandardOutput()).object();
            const bool ok = code == 0 && (r.isEmpty() || r["ok"].toBool(true));
            say(QString("  -> %1%2").arg(ok ? "ok" : "FAILED").arg(r.contains("error") ? ": " + r["error"].toString() : QString()));
            p->deleteLater(); m_proc = nullptr;
            if (!ok) { m_chain.clear(); m_done = nullptr; m_taskLbl->setText("no task running"); say(m_what + ": stopped"); refreshRun(); return; }
            next();
        });
        connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart) return;
            say("cannot start " + headless() + " (set ORIGIN_HEADLESS)"); p->deleteLater(); m_proc = nullptr; m_chain.clear(); m_done = nullptr; m_taskLbl->setText("no task running"); refreshRun();
        });
        p->start(headless(), args);
    }

    // ---- the run in progress: <data dir>/run_state.json, written by the supervisor every tick
    bool runActive() const
    {
        if (m_state["phase"].toString() == "finished") return false;
        const QDateTime upd = QDateTime::fromString(m_state["updated"].toString(), Qt::ISODate);
        const qint64 pid = qint64(m_state["pid"].toDouble());
        return upd.isValid() && upd.secsTo(QDateTime::currentDateTimeUtc()) < 180 && pid > 0 && ::kill(pid_t(pid), 0) == 0;
    }
    void refreshRun()
    {
        m_state = readJson(m_dir + "/run_state.json");
        const bool active = runActive(), busy = m_proc != nullptr; const QTimeZone zone("Europe/London");
        auto local = [&](const QString& iso) { return QDateTime::fromString(iso, Qt::ISODate).toTimeZone(zone).toString("HH:mm"); };
        if (active) {
            const QJsonObject c = m_state["current"].toObject();
            QString now = c.contains("key") ? QString("observing <b>%1</b> until %2%3").arg(c["key"].toString().toHtmlEscaped(), local(c["until"].toString()), c["override"].toBool() ? " (override)" : c["backup"].toBool() ? " (backup target)" : "")
                                            : c.contains("waiting") ? "waiting: " + c["waiting"].toString().toHtmlEscaped() : QString("starting");
            const qint64 sent = qint64(readJson(m_dir + "/run_control.json")["id"].toDouble()), done = qint64(m_state["control_done"].toDouble());
            m_runLbl->setText(QString("<b>Run in progress</b> (night %1, pid %2): %3.%4").arg(m_state["night"].toString()).arg(qint64(m_state["pid"].toDouble())).arg(now)
                                  .arg(sent > done ? " <i>A request is waiting for the supervisor (it checks every 20 s).</i>" : ""));
        } else m_runLbl->setText(m_state.isEmpty() ? "No unattended run (none has reported)." : QString("No unattended run in progress (the last one: night %1, %2).").arg(m_state["night"].toString(),
                                     m_state["phase"].toString() == "finished" ? m_state["current"].toObject()["finished"].toString("finished") : QString("ended without reporting: its process is gone")));
        m_plan->clear();
        if (active) {
            const QJsonArray plan = m_state["plan"].toArray(); const int cur = m_state["block"].toInt(-1);
            for (int i = 0; i < plan.size(); ++i) {
                const QJsonObject b = plan[i].toObject();
                auto* it = new QListWidgetItem(QString("%1  %2 - %3  %4").arg(i == cur ? ">" : " ", local(b["start"].toString()), local(b["end"].toString()), b["name"].toString(b["key"].toString())));
                if (i == cur) { QFont f = it->font(); f.setBold(true); it->setFont(f); }
                m_plan->addItem(it);
            }
        }
        // what can be done now: with a run, through it; without one, directly; never two tasks at once
        m_stopPark->setEnabled(active); m_stopStay->setEnabled(active);
        m_stopSeq->setEnabled(!active && !busy); m_park->setEnabled(!active && !busy);
        m_obsSurvey->setEnabled(!active && !busy); m_skySurvey->setEnabled(!active && !busy);
        m_override->setEnabled(active || !busy); m_endOverride->setEnabled(active ? m_state["current"].toObject()["override"].toBool() : !busy);
        m_cancel->setEnabled(busy);
        const QString why = active ? "an unattended run holds the telescope: stop it first" : busy ? "a task is running" : QString();
        for (QPushButton* b : {m_obsSurvey, m_skySurvey, m_stopSeq, m_park}) b->setToolTip(why);
    }
    void control(QJsonObject req)
    {
        const qint64 id = std::max(qint64(readJson(m_dir + "/run_control.json")["id"].toDouble()), qint64(m_state["control_done"].toDouble())) + 1;
        req["id"] = double(id); req["sent"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate); req["from"] = "OriginWatch";
        if (!writeJson(m_dir + "/run_control.json", req)) { say("cannot write " + m_dir + "/run_control.json"); return; }
        say(QString("request %1 to the run: %2 (it acts within about 20 s; see the Log tab)").arg(id).arg(QString::fromUtf8(QJsonDocument(req).toJson(QJsonDocument::Compact))));
        refreshRun();
    }

    // ---- actions
    void doOverride()
    {
        const QString name = m_target->currentText().section("  ", 0, 0).trimmed();
        bool okRa = false, okDec = false; const double ra = m_ra->text().toDouble(&okRa), dec = m_dec->text().toDouble(&okDec);
        const bool coords = !m_ra->text().trimmed().isEmpty() || !m_dec->text().trimmed().isEmpty();
        if (coords && (!okRa || !okDec || ra < 0 || ra >= 24 || dec < -90 || dec > 90)) { say("override: RA must be 0..24 h and Dec -90..90 deg"); return; }
        if (!coords && name.isEmpty()) { say("override: choose a target"); return; }
        const QString what = coords ? QString("%1 (RA %2 h, Dec %3 deg)").arg(name.isEmpty() ? "coordinates" : name).arg(ra, 0, 'f', 3).arg(dec, 0, 'f', 2) : name;
        const bool active = runActive();
        if (!confirm(QString("Send the telescope to %1 for %2 minutes%3?\n\nNow: %4").arg(what).arg(m_minutes->value()).arg(active ? ", then let the run resume its plan" : "", m_pointing()))) return;
        if (active) {
            QJsonObject req{{"action", "override"}, {"minutes", m_minutes->value()}};
            if (!name.isEmpty()) req["name"] = name;
            if (coords) { req["ra_hours"] = ra; req["dec_deg"] = dec; }
            control(req); return;
        }
        const QString until = QDateTime::currentDateTime().addSecs(m_minutes->value() * 60).toString("HH:mm"), exp = QString::number(m_exposure->value());
        QList<QStringList> chain;
        if (coords) { const QString label = name.isEmpty() ? QString("RA%1_Dec%2").arg(ra, 0, 'f', 2).arg(dec, 0, 'f', 1) : name;
                      chain << QStringList{"target", label, "--ra-hours", QString::number(ra, 'f', 5), "--dec-deg", QString::number(dec, 'f', 5)} << QStringList{"goto", QString::number(ra, 'f', 5), QString::number(dec, 'f', 5)}; }
        else chain << QStringList{"go", name, "--exposure", exp};
        chain << QStringList{"sequence", "--action", "start", "--exposure", exp, "--until-local", until};
        runChain(chain, "override " + what);
    }
    void doEndOverride()
    {
        if (runActive()) { control({{"action", "end_override"}}); return; }
        if (confirm("No run is in progress: stop the sequence that is running?")) runChain({{"sequence", "--action", "stop", "--reason", "override ended from OriginWatch"}}, "end override");
    }
    void doStopRun(bool park)
    {
        if (!confirm(park ? "Stop the unattended run and park the telescope?" : "Stop the unattended run and leave the telescope where it is pointing?\n\nNow: " + m_pointing())) return;
        control({{"action", "stop"}, {"park", park}});
    }

    // ---- schedule: tonight's observable targets with boosts and skips
    void loadSchedule()
    {
        auto* p = new QProcess(this); p->setProcessEnvironment(env());
        m_schedLbl->setText("loading tonight's targets...");
        connect(p, &QProcess::finished, this, [this, p](int, QProcess::ExitStatus) {
            const QJsonObject s = QJsonDocument::fromJson(p->readAllStandardOutput()).object(); p->deleteLater();
            if (!s["ok"].toBool()) { m_schedLbl->setText("could not plan tonight: " + s["error"].toString("no answer from " + headless())); return; }
            m_night = s["night"].toString();
            const QJsonObject boosts = readJson(cfg("target_boost.json")), skipF = readJson(cfg("target_skip.json"));
            QStringList skip; if (skipF["night"].toString() == m_night) for (const QJsonValue& v : skipF["skip"].toArray()) skip << v.toString();
            const QTimeZone zone("Europe/London"); auto local = [&](const QString& iso) { return QDateTime::fromString(iso, Qt::ISODate).toTimeZone(zone).toString("HH:mm"); };
            const QString keep = m_target->currentText();
            m_target->clear(); m_sched->setRowCount(0);
            for (const QJsonValue& tv : s["targets"].toArray()) {
                const QJsonObject t = tv.toObject(); const QString key = t["key"].toString(), name = t["name"].toString();
                m_target->addItem(key + "  " + (name != key ? name : QString()) + "  " + local(t["window_start"].toString()) + "-" + local(t["window_end"].toString()));
                const int r = m_sched->rowCount(); m_sched->insertRow(r);
                auto* k = new QTableWidgetItem(name.isEmpty() ? key : name); k->setData(Qt::UserRole, key); m_sched->setItem(r, 0, k);
                m_sched->setItem(r, 1, new QTableWidgetItem(local(t["window_start"].toString()) + " - " + local(t["window_end"].toString())));
                m_sched->setItem(r, 2, new QTableWidgetItem(QString::number(t["priority"].toDouble(), 'f', 1)));
                auto* b = new QSpinBox; b->setRange(-10, 10); b->setValue(boosts.contains(key) ? boosts[key].toInt() : 0); m_sched->setCellWidget(r, 3, b);
                auto* c = new QCheckBox; c->setChecked(skip.contains(key)); m_sched->setCellWidget(r, 4, c);
            }
            if (!keep.isEmpty()) m_target->setCurrentText(keep);
            m_schedLbl->setText(QString("%1 targets observable on the night of %2 (weather ignored)").arg(m_sched->rowCount()).arg(m_night));
        });
        p->start(headless(), {"schedule", "--weather", "off"});
    }
    void saveSchedule()
    {
        if (m_night.isEmpty()) { say("schedule not loaded yet"); return; }
        QJsonObject boosts = readJson(cfg("target_boost.json")); QJsonArray skip;
        for (int r = 0; r < m_sched->rowCount(); ++r) {
            const QString key = m_sched->item(r, 0)->data(Qt::UserRole).toString();
            const int b = static_cast<QSpinBox*>(m_sched->cellWidget(r, 3))->value();
            if (b != 0) boosts[key] = b; else boosts.remove(key);
            if (static_cast<QCheckBox*>(m_sched->cellWidget(r, 4))->isChecked()) skip.append(key);
        }
        const bool ok = writeJson(cfg("target_boost.json"), boosts) && writeJson(cfg("target_skip.json"), {{"night", m_night}, {"skip", skip}});
        say(ok ? QString("saved: %1 boost(s), %2 skipped tonight").arg(boosts.size()).arg(skip.size()) : "could not write ~/.config/origin/target_boost.json or target_skip.json");
        if (ok && runActive()) control({{"action", "replan"}});
    }

    // ---- the last sky brightness survey
    void loadSkySurvey()
    {
        QStringList files = QDir(m_dir).entryList({"sky_survey_*.json"}, QDir::Files, QDir::Name);
        if (files.isEmpty()) { m_skyLbl->setText("No sky brightness survey yet."); return; }
        const QJsonObject s = readJson(m_dir + "/" + files.last());
        m_skyLbl->setText(QString("Night %1, %2 places. %3").arg(s["night"].toString()).arg(s["sampled"].toInt()).arg(s["note"].toString()));
        m_sky->setRowCount(0);
        for (const QJsonValue& v : s["samples"].toArray()) {
            const QJsonObject r = v.toObject(); const int i = m_sky->rowCount(); m_sky->insertRow(i);
            auto put = [&](int c, const QString& t) { auto* it = new QTableWidgetItem(t); it->setTextAlignment(Qt::AlignCenter); m_sky->setItem(i, c, it); };
            put(0, QString::number(r["alt"].toDouble(), 'f', 0)); put(1, QString::number(r["az"].toDouble(), 'f', 0));
            put(2, r.contains("delta_mag") ? QString::number(r["delta_mag"].toDouble(), 'f', 2) : r["error"].toString("-"));
            put(3, r.contains("mag_per_arcsec2") ? QString::number(r["mag_per_arcsec2"].toDouble(), 'f', 2) : "-");
            put(4, r.contains("stars") ? QString::number(r["stars"].toInt()) : "-"); put(5, r.contains("moon_sep_deg") ? QString::number(r["moon_sep_deg"].toDouble(), 'f', 0) : "-");
        }
    }

    QString m_dir; int m_port; std::function<QString()> m_pointing;
    QJsonObject m_state; QString m_night;
    QProcess* m_proc = nullptr; QList<QStringList> m_chain; QString m_what; std::function<void()> m_done;
    QTimer m_timer;
    QLabel *m_runLbl, *m_skyLbl, *m_schedLbl, *m_taskLbl; QListWidget* m_plan;
    QComboBox* m_target; QLineEdit *m_ra, *m_dec; QSpinBox *m_minutes, *m_samples; QDoubleSpinBox* m_exposure;
    QPushButton *m_override, *m_endOverride, *m_stopPark, *m_stopStay, *m_stopSeq, *m_park, *m_obsSurvey, *m_skySurvey, *m_reload, *m_save, *m_cancel;
    QTableWidget *m_sky, *m_sched; QPlainTextEdit* m_out;
};
