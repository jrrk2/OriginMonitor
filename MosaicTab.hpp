#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QListWidget>
#include <QJsonObject>

class OriginBackend;

class MosaicTab : public QWidget {
    Q_OBJECT
public:
    explicit MosaicTab(OriginBackend *backend, QWidget *parent = nullptr);

private slots:
    void onRawJson(const QString &direction, const QJsonObject &json);
    void onStartCaptureClicked();
    void onComposeClicked();
    void onRefreshClicked();
    void onTargetChanged(int index);

private:
    void setupUI();
    void applyMosaicListResponse(const QJsonObject &json);
    void applyMosaicTilesResponse(const QJsonObject &json);

    OriginBackend *m_backend;

    // Target selection - shares the same Messier catalog list as the Slew & Image tab
    QComboBox *m_targetCombo;
    QLineEdit *m_customNameEdit;
    QLineEdit *m_customRaEdit;   // decimal hours
    QLineEdit *m_customDecEdit;  // decimal degrees

    // Capture parameters
    QDoubleSpinBox *m_widthSpin;       // degrees
    QDoubleSpinBox *m_heightSpin;      // degrees
    QDoubleSpinBox *m_orientationSpin; // degrees
    QDoubleSpinBox *m_exposureSpin;    // seconds
    QSpinBox *m_isoSpin;
    QSpinBox *m_totalDurationSpin;     // minutes
    QDoubleSpinBox *m_magnitudeSpin;
    QDoubleSpinBox *m_objectSizeSpin;  // degrees
    QLineEdit *m_startTimeEdit;        // "dd MM yyyy HH:mm:ss"
    QCheckBox *m_saveRawCheck;
    QCheckBox *m_autoFocusAfterSlewCheck;
    QCheckBox *m_powerDownCheck;
    QSpinBox *m_ditherDistanceSpin;
    QSpinBox *m_ditherIntervalSpin;
    QPushButton *m_startButton;

    // Status/browsing
    QListWidget *m_mosaicListWidget;
    QLabel *m_currentMosaicLabel;
    QLabel *m_tilesLabel;
    QProgressBar *m_tilesProgressBar;
    QPushButton *m_refreshButton;
    QPushButton *m_composeButton;

    // Tracks the most recently selected/active mosaic list Uuid for GetMosaicTiles/ComposeMosaic
    QString m_activeListUuid;
    int m_tilesTotal = 0;
    int m_tilesCompleted = 0;
};
