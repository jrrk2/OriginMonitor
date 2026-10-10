// CometTracker.hpp
#pragma once

#include <QObject>
#include <QTimer>
#include "CometObserver.hpp"
#include "OriginBackend.hpp"

class CometTracker : public QObject
{
    Q_OBJECT
    
public:
    explicit CometTracker(OriginBackend* backend, QObject *parent = nullptr);
    
    // Configuration
    bool loadCometData(const QString& spkFile, 
                      const QString& leapSecondFile,
                      const QString& planetaryEphemeris);
    void setObserverLocation(const ObserverLocation& loc);
    void setCometName(const QString& name);
    
    // Tracking control
    void startTracking(int updateIntervalSeconds = 30);
    void stopTracking();
    bool isTracking() const;
    
    // Status
    SkyPosition getCurrentCometPosition() const;
    
signals:
    void trackingStarted();
    void trackingStopped();
    void positionUpdated(const SkyPosition& pos);
    void trackingError(const QString& error);
    
private slots:
    void updateTelescopePosition();
    
private:
    OriginBackend* m_backend;
    QTimer* m_updateTimer;
    QString m_cometName;
    bool m_isTracking;
    SkyPosition m_lastPosition;
    
    // Tracking parameters
    double m_positionToleranceDegrees = 0.1; // Only update if movement exceeds this
};
