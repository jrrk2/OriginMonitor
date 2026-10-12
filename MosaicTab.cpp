#include "MosaicTab.hpp"
#include "OriginBackend.hpp"
#include "MessierCatalog.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QDateTime>
#include <QJsonArray>
#include <QMessageBox>
#include <cmath>

namespace {
constexpr double kDegToRad = M_PI / 180.0;
}

MosaicTab::MosaicTab(OriginBackend *backend, QWidget *parent)
    : QWidget(parent), m_backend(backend)
{
    setupUI();
    connect(m_backend, &OriginBackend::rawJsonTraffic, this, &MosaicTab::onRawJson);
}

void MosaicTab::setupUI()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // --- Capture parameters ---
    QGroupBox *paramsGroup = new QGroupBox("New Mosaic Capture (RunCaptureMosaic)", this);
    QFormLayout *form = new QFormLayout(paramsGroup);

    m_targetCombo = new QComboBox(paramsGroup);
    m_targetCombo->addItem("Custom Coordinates");
    for (const QString &name : MessierCatalog::getObjectNames()) {
        m_targetCombo->addItem(name);
    }
    connect(m_targetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MosaicTab::onTargetChanged);
    form->addRow("Select Target:", m_targetCombo);

    m_customNameEdit = new QLineEdit(paramsGroup);
    m_customNameEdit->setPlaceholderText("Enter target name");
    form->addRow("Name:", m_customNameEdit);

    m_customRaEdit = new QLineEdit(paramsGroup);
    m_customRaEdit->setPlaceholderText("e.g. 0.712 (decimal hours)");
    form->addRow("RA (decimal hours):", m_customRaEdit);

    m_customDecEdit = new QLineEdit(paramsGroup);
    m_customDecEdit->setPlaceholderText("e.g. 41.269 (decimal degrees)");
    form->addRow("Dec (decimal degrees):", m_customDecEdit);

    onTargetChanged(m_targetCombo->currentIndex());

    m_widthSpin = new QDoubleSpinBox(paramsGroup);
    m_widthSpin->setRange(0.01, 45.0);
    m_widthSpin->setDecimals(4);
    m_widthSpin->setValue(3.0);
    m_widthSpin->setSuffix(" deg");
    form->addRow("Mosaic Width:", m_widthSpin);

    m_heightSpin = new QDoubleSpinBox(paramsGroup);
    m_heightSpin->setRange(0.01, 45.0);
    m_heightSpin->setDecimals(4);
    m_heightSpin->setValue(1.2);
    m_heightSpin->setSuffix(" deg");
    form->addRow("Mosaic Height:", m_heightSpin);

    m_orientationSpin = new QDoubleSpinBox(paramsGroup);
    m_orientationSpin->setRange(0.0, 360.0);
    m_orientationSpin->setDecimals(2);
    m_orientationSpin->setSuffix(" deg");
    form->addRow("Orientation:", m_orientationSpin);

    m_exposureSpin = new QDoubleSpinBox(paramsGroup);
    m_exposureSpin->setRange(0.1, 600.0);
    m_exposureSpin->setValue(15.0);
    m_exposureSpin->setSuffix(" s");
    form->addRow("Exposure Time:", m_exposureSpin);

    m_isoSpin = new QSpinBox(paramsGroup);
    m_isoSpin->setRange(100, 12800);
    m_isoSpin->setValue(1000);
    form->addRow("ISO:", m_isoSpin);

    m_totalDurationSpin = new QSpinBox(paramsGroup);
    m_totalDurationSpin->setRange(1, 600);
    m_totalDurationSpin->setValue(10);
    m_totalDurationSpin->setSuffix(" min per tile");
    form->addRow("Total Duration:", m_totalDurationSpin);

    m_magnitudeSpin = new QDoubleSpinBox(paramsGroup);
    m_magnitudeSpin->setRange(-30.0, 30.0);
    m_magnitudeSpin->setValue(3.4);
    form->addRow("Object Magnitude:", m_magnitudeSpin);

    m_objectSizeSpin = new QDoubleSpinBox(paramsGroup);
    m_objectSizeSpin->setRange(0.0, 45.0);
    m_objectSizeSpin->setDecimals(4);
    m_objectSizeSpin->setValue(5.9);
    m_objectSizeSpin->setSuffix(" deg");
    form->addRow("Object Size:", m_objectSizeSpin);

    m_startTimeEdit = new QLineEdit(paramsGroup);
    m_startTimeEdit->setText(QDateTime::currentDateTime().addSecs(60).toString("dd MM yyyy HH:mm:ss"));
    form->addRow("Minimum Start Time:", m_startTimeEdit);

    m_ditherDistanceSpin = new QSpinBox(paramsGroup);
    m_ditherDistanceSpin->setRange(0, 100);
    m_ditherDistanceSpin->setValue(16);
    form->addRow("Dither Distance:", m_ditherDistanceSpin);

    m_ditherIntervalSpin = new QSpinBox(paramsGroup);
    m_ditherIntervalSpin->setRange(0, 100);
    m_ditherIntervalSpin->setValue(5);
    form->addRow("Dither Interval:", m_ditherIntervalSpin);

    m_saveRawCheck = new QCheckBox("Save Raw Image", paramsGroup);
    m_saveRawCheck->setChecked(true);
    form->addRow(m_saveRawCheck);

    m_autoFocusAfterSlewCheck = new QCheckBox("Auto Focus After Slew", paramsGroup);
    m_autoFocusAfterSlewCheck->setChecked(true);
    form->addRow(m_autoFocusAfterSlewCheck);

    m_powerDownCheck = new QCheckBox("Power Down On Completion", paramsGroup);
    form->addRow(m_powerDownCheck);

    m_startButton = new QPushButton("Start Mosaic Capture", paramsGroup);
    connect(m_startButton, &QPushButton::clicked, this, &MosaicTab::onStartCaptureClicked);
    form->addRow(m_startButton);

    mainLayout->addWidget(paramsGroup);

    // --- Status / browsing ---
    QGroupBox *statusGroup = new QGroupBox("Mosaic Status", this);
    QVBoxLayout *statusLayout = new QVBoxLayout(statusGroup);

    m_mosaicListWidget = new QListWidget(statusGroup);
    statusLayout->addWidget(m_mosaicListWidget);

    m_currentMosaicLabel = new QLabel("-", statusGroup);
    statusLayout->addWidget(m_currentMosaicLabel);

    m_tilesLabel = new QLabel("Tiles: -", statusGroup);
    statusLayout->addWidget(m_tilesLabel);

    m_tilesProgressBar = new QProgressBar(statusGroup);
    m_tilesProgressBar->setRange(0, 1);
    m_tilesProgressBar->setValue(0);
    statusLayout->addWidget(m_tilesProgressBar);

    QHBoxLayout *statusButtonLayout = new QHBoxLayout();
    m_refreshButton = new QPushButton("Refresh Status", statusGroup);
    connect(m_refreshButton, &QPushButton::clicked, this, &MosaicTab::onRefreshClicked);
    statusButtonLayout->addWidget(m_refreshButton);

    m_composeButton = new QPushButton("Compose Mosaic", statusGroup);
    m_composeButton->setToolTip("Only valid once all tiles have been captured (TilesCompleted == TilesTotal)");
    connect(m_composeButton, &QPushButton::clicked, this, &MosaicTab::onComposeClicked);
    statusButtonLayout->addWidget(m_composeButton);

    statusLayout->addLayout(statusButtonLayout);
    mainLayout->addWidget(statusGroup);
}

void MosaicTab::onTargetChanged(int index)
{
    bool isCustom = (index == 0);
    m_customNameEdit->setEnabled(isCustom);
    m_customRaEdit->setEnabled(isCustom);
    m_customDecEdit->setEnabled(isCustom);
    if (isCustom) return;

    // Auto-fill from the Messier catalog, mirroring the Slew & Image tab's target selector
    auto objects = MessierCatalog::getAllObjects();
    const MessierObject &obj = objects[index - 1];
    m_customNameEdit->setText(obj.name);
    m_customRaEdit->setText(QString::number(obj.sky_position.ra_deg / 15.0, 'f', 6));
    m_customDecEdit->setText(QString::number(obj.sky_position.dec_deg, 'f', 6));
    m_magnitudeSpin->setValue(obj.magnitude);
    double sizeDeg = qMax(obj.size_arcmin.width(), obj.size_arcmin.height()) / 60.0;
    if (sizeDeg > 0.0) m_objectSizeSpin->setValue(sizeDeg);
}

void MosaicTab::onStartCaptureClicked()
{
    if (!m_backend || !m_backend->isConnected()) {
        QMessageBox::warning(this, "Not Connected", "Please connect to the telescope first");
        return;
    }

    QString targetName = m_customNameEdit->text().trimmed();
    bool raOk = false, decOk = false;
    double raHours = m_customRaEdit->text().toDouble(&raOk);
    double decDegrees = m_customDecEdit->text().toDouble(&decOk);
    if (targetName.isEmpty() || !raOk || !decOk || raHours < 0 || raHours >= 24 ||
        decDegrees < -90 || decDegrees > 90) {
        QMessageBox::warning(this, "Invalid Target",
                              "Please select a target or enter a valid name, RA (0-24h), and Dec (-90 to +90 deg)");
        return;
    }
    double raRadians = raHours * M_PI / 12.0;
    double decRadians = decDegrees * kDegToRad;

    m_backend->runCaptureMosaic(
        m_widthSpin->value() * kDegToRad,
        m_heightSpin->value() * kDegToRad,
        m_orientationSpin->value() * kDegToRad,
        raRadians,
        decRadians,
        targetName,
        m_exposureSpin->value(),
        m_isoSpin->value(),
        m_totalDurationSpin->value() * 60,
        m_magnitudeSpin->value(),
        m_objectSizeSpin->value() * kDegToRad,
        m_saveRawCheck->isChecked(),
        m_startTimeEdit->text(),
        m_autoFocusAfterSlewCheck->isChecked(),
        m_powerDownCheck->isChecked(),
        m_ditherDistanceSpin->value(),
        m_ditherIntervalSpin->value());
}

void MosaicTab::onComposeClicked()
{
    if (!m_backend || !m_backend->isConnected()) {
        QMessageBox::warning(this, "Not Connected", "Please connect to the telescope first");
        return;
    }
    if (m_tilesTotal > 0 && m_tilesCompleted < m_tilesTotal) {
        auto reply = QMessageBox::question(this, "Mosaic Not Complete",
            QString("Only %1 of %2 tiles are complete. RunComposeMosaic is only valid once all tiles "
                    "are captured. Send it anyway?").arg(m_tilesCompleted).arg(m_tilesTotal));
        if (reply != QMessageBox::Yes) return;
    }
    m_backend->runComposeMosaic();
}

void MosaicTab::onRefreshClicked()
{
    if (!m_backend || !m_backend->isConnected()) {
        QMessageBox::warning(this, "Not Connected", "Please connect to the telescope first");
        return;
    }
    m_backend->getAvailableMosaics();
    if (!m_activeListUuid.isEmpty()) {
        m_backend->getMosaicTiles(m_activeListUuid);
    }
}

void MosaicTab::onRawJson(const QString &direction, const QJsonObject &json)
{
    Q_UNUSED(direction);
    const QString command = json.value("Command").toString();
    if (command == "GetAvailableMosaics") {
        applyMosaicListResponse(json);
    } else if (command == "GetMosaicTiles") {
        applyMosaicTilesResponse(json);
    }
}

void MosaicTab::applyMosaicListResponse(const QJsonObject &json)
{
    m_mosaicListWidget->clear();

    const QJsonArray mosaics = json.value("AvailableMosaics").toArray();
    for (const QJsonValue &v : mosaics) {
        QJsonObject m = v.toObject();
        QString status = m.value("Status").toString();
        QString name = m.value("ObjectName").toString();
        int completed = m.value("TilesCompleted").toInt();
        int total = m.value("TilesTotal").toInt();
        QString uuid = m.value("ListUuid").toString();

        m_mosaicListWidget->addItem(QString("%1 - %2 (%3/%4 tiles) [%5]")
                                         .arg(name, status).arg(completed).arg(total).arg(uuid));

        // Track the most recent in-progress mosaic as the active one to monitor
        if (status == "IN_PROGRESS") {
            m_activeListUuid = uuid;
            m_currentMosaicLabel->setText(QString("Active: %1").arg(name));
            m_tilesTotal = total;
            m_tilesCompleted = completed;
            m_tilesLabel->setText(QString("Tiles: %1 / %2").arg(completed).arg(total));
            m_tilesProgressBar->setRange(0, total > 0 ? total : 1);
            m_tilesProgressBar->setValue(completed);
        }
    }
}

void MosaicTab::applyMosaicTilesResponse(const QJsonObject &json)
{
    QJsonObject metadata = json.value("MosaicMetadata").toObject();
    if (metadata.isEmpty()) return;

    int completed = metadata.value("TilesCompleted").toInt();
    int total = metadata.value("TilesTotal").toInt();
    QString status = metadata.value("Status").toString();
    QString name = metadata.value("ObjectName").toString();

    m_currentMosaicLabel->setText(QString("Active: %1 (%2)").arg(name, status));
    m_tilesTotal = total;
    m_tilesCompleted = completed;
    m_tilesLabel->setText(QString("Tiles: %1 / %2").arg(completed).arg(total));
    m_tilesProgressBar->setRange(0, total > 0 ? total : 1);
    m_tilesProgressBar->setValue(completed);
}
