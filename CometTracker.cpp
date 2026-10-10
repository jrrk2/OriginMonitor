// CometTracker.cpp
#include "CometTracker.hpp"
#include <QDebug>

extern CometObserver *global_observer;

CometTracker::CometTracker(OriginBackend* backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_updateTimer(new QTimer(this))
    , m_isTracking(false)
{
    connect(m_updateTimer, &QTimer::timeout, this, &CometTracker::updateTelescopePosition);
}

bool CometTracker::loadCometData(const QString& spkFile, 
                                 const QString& leapSecondFile,
                                 const QString& planetaryEphemeris)
{
    return global_observer->loadKernels(spkFile, leapSecondFile, planetaryEphemeris);
}

void CometTracker::setObserverLocation(const ObserverLocation& loc)
{
    global_observer->setLocation(loc);
}

void CometTracker::setCometName(const QString& name)
{
    m_cometName = name;
}

void CometTracker::startTracking(int updateIntervalSeconds)
{
    if (m_isTracking) {
        qDebug() << "Already tracking";
        return;
    }
    
    if (m_cometName.isEmpty()) {
        emit trackingError("No comet name specified");
        return;
    }
/*
    if (!m_backend->isConnected()) {
        emit trackingError("Telescope not connected");
        return;
    }
  */
    m_isTracking = true;
    m_updateTimer->start(updateIntervalSeconds * 1000);
    
    // Perform initial positioning
    updateTelescopePosition();
    
    emit trackingStarted();
    qDebug() << "Started tracking" << m_cometName;
}

void CometTracker::stopTracking()
{
    if (!m_isTracking) return;
    
    m_updateTimer->stop();
    m_isTracking = false;
    
    emit trackingStopped();
    qDebug() << "Stopped tracking";
}

bool CometTracker::isTracking() const
{
    return m_isTracking;
}

SkyPosition CometTracker::getCurrentCometPosition() const
{
    return m_lastPosition;
}

void CometTracker::updateTelescopePosition()
{
    // Calculate current comet position
    QDateTime now = QDateTime::currentDateTimeUtc();
    SkyPosition pos = global_observer->calculatePosition(m_cometName, now);
    
    qDebug() << "Comet position - RA:" << pos.ra << "Dec:" << pos.dec 
             << "Alt:" << pos.alt << "Az:" << pos.az;
    
    // Check if comet is observable
    if (!pos.isObservable) {
        qWarning() << "Comet not observable - Alt:" << pos.alt 
                   << "Sun alt:" << pos.sunAlt;
        emit trackingError("Comet not currently observable");
        return;
    }
    
    // Check if we need to update telescope position
    // (only if comet has moved significantly)
    if (m_lastPosition.ra != 0) {
        double raDiff = qAbs(pos.ra - m_lastPosition.ra);
        double decDiff = qAbs(pos.dec - m_lastPosition.dec);
        
        if (raDiff < m_positionToleranceDegrees && 
            decDiff < m_positionToleranceDegrees) {
            qDebug() << "Position change too small, skipping update";
            return;
        }
    }
    
    // Command telescope to new position
    qDebug() << "Commanding telescope to RA:" << pos.ra << "Dec:" << pos.dec;
    
    if (m_backend->gotoPosition(pos.ra, pos.dec)) {
        m_lastPosition = pos;
        emit positionUpdated(pos);
    } else {
        emit trackingError("Failed to command telescope");
    }
}
