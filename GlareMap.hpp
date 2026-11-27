#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <algorithm>
// Glare from neighbours' lights, from the learned sky map's clear, moonless readings (ex_*: sky over the dark baseline, minus 1). The typical bin is the median of the
// bins' means (bins with 5+ readings, 4+ such bins needed); a bin reads bright on a night when its mean that night is at least 1.6x the typical and 0.4 above it.
// It is glare only when it read bright on 2+ nights and on at least as many nights as it read normal (one hazy night, town light on the haze, no longer marks it
// for good). Nights come from ex_nights {date: [sum, n]}; older readings with only ex_sum/ex_n/ex_dates count each date as one night at their overall mean.

namespace Glare {
struct Bin { QString key; int az = 0, alt = 0; double mean = 0; int frames = 0, brightNights = 0, normalNights = 0; bool glare = false; };
struct Map { double median = 0, threshold = 0; QList<Bin> bins; };

inline Map map(const QJsonObject& bins)
{
    Map gm; QList<double> means;
    for (auto it = bins.begin(); it != bins.end(); ++it) { const QJsonObject b = it.value().toObject(); if (b["ex_n"].toInt() >= 5) means << b["ex_sum"].toDouble() / b["ex_n"].toInt(); }
    if (means.size() < 4) return gm;
    std::sort(means.begin(), means.end()); gm.median = means[means.size() / 2]; gm.threshold = std::max(1.6 * gm.median, gm.median + 0.4);
    for (auto it = bins.begin(); it != bins.end(); ++it) {
        const QJsonObject b = it.value().toObject(); const int n = b["ex_n"].toInt();
        if (n < 5) continue;
        const QStringList p = it.key().split('_'); if (p.size() != 2) continue;
        Bin g; g.key = it.key(); g.az = p[0].toInt(); g.alt = p[1].toInt(); g.frames = n; g.mean = b["ex_sum"].toDouble() / n;
        const QJsonObject nights = b["ex_nights"].toObject(); double s = b["ex_sum"].toDouble(); int left = n;
        for (auto nt = nights.begin(); nt != nights.end(); ++nt) {
            const QJsonArray a = nt.value().toArray(); const double ns = a.at(0).toDouble(); const int nn = a.at(1).toInt();
            s -= ns; left -= nn;
            if (nn >= 2) ((ns / nn >= gm.threshold) ? g.brightNights : g.normalNights)++;
        }
        if (left >= 2) {                                                         // readings from before ex_nights: one night per date not listed there, at their mean
            int legacyDates = 0; for (const QJsonValue& d : b["ex_dates"].toArray()) if (!nights.contains(d.toString())) ++legacyDates;
            ((s / left >= gm.threshold) ? g.brightNights : g.normalNights) += std::max(1, legacyDates);
        }
        g.glare = g.brightNights >= 2 && g.brightNights >= g.normalNights;
        gm.bins.append(g);
    }
    return gm;
}
}   // namespace Glare
