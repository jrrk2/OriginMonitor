// DataDir.hpp - where downloaded snapshots, live stacks, sky_map.json and focus_log.csv are written
#ifndef DATADIR_H
#define DATADIR_H

#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>
#include <QString>
#include <QtGlobal>

// $ORIGIN_DATA_DIR if set, else /mnt/fits-backup/claude. The default is only used while it is writable and on a different filesystem
// from / (the backup disk is mounted); otherwise ~/Pictures/CelestronOrigin, so frames never land on the root filesystem under an empty mount point.
inline QString originDataDir()
{
    const QString fallback = QDir::homePath() + "/Pictures/CelestronOrigin";
    const bool overridden = qEnvironmentVariableIsSet("ORIGIN_DATA_DIR");
    const QString dir = qEnvironmentVariable("ORIGIN_DATA_DIR", "/mnt/fits-backup/claude");
    if (!overridden && QStorageInfo(QFileInfo(dir).dir()).rootPath() == QStorageInfo::root().rootPath()) return fallback;
    QDir().mkpath(dir);
    return QFileInfo(dir).isDir() && QFileInfo(dir).isWritable() ? dir : fallback;
}

#endif
