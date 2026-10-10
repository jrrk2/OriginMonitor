#include "LogReplayDialog.hpp"
#include <QMessageBox>
#include <QSplitter>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

LogReplayDialog::LogReplayDialog(TelescopeDataProcessor* processor, QWidget *parent)
    : QDialog(parent)
    , m_processor(processor)
    , m_currentIndex(-1)
    , m_isPlaying(false)
    , m_playbackSpeed(1000)
{
    setWindowTitle("Log Replay - WebSocket Message Analyzer");
    resize(1200, 800);
    
    setupUI();
    
    m_playbackTimer = new QTimer(this);
    connect(m_playbackTimer, &QTimer::timeout, this, &LogReplayDialog::onPlaybackTimer);
}

LogReplayDialog::~LogReplayDialog()
{
}

void LogReplayDialog::setupUI()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    
    // File loading section
    QGroupBox* fileGroup = new QGroupBox("Log File", this);
    QHBoxLayout* fileLayout = new QHBoxLayout(fileGroup);
    
    m_loadButton = new QPushButton("Load Log File...", fileGroup);
    connect(m_loadButton, &QPushButton::clicked, this, &LogReplayDialog::onLoadFile);
    fileLayout->addWidget(m_loadButton);
    
    m_statusLabel = new QLabel("No log file loaded", fileGroup);
    fileLayout->addWidget(m_statusLabel, 1);
    
    mainLayout->addWidget(fileGroup);
    
    // Position controls
    QGroupBox* controlGroup = new QGroupBox("Playback Controls", this);
    QVBoxLayout* controlLayout = new QVBoxLayout(controlGroup);
    
    // Slider
    QHBoxLayout* sliderLayout = new QHBoxLayout();
    m_positionSlider = new QSlider(Qt::Horizontal, controlGroup);
    m_positionSlider->setEnabled(false);
    connect(m_positionSlider, &QSlider::valueChanged, this, &LogReplayDialog::onSliderMoved);
    sliderLayout->addWidget(m_positionSlider);
    
    m_positionLabel = new QLabel("0/0", controlGroup);
    m_positionLabel->setMinimumWidth(80);
    sliderLayout->addWidget(m_positionLabel);
    
    controlLayout->addLayout(sliderLayout);
    
    // Timestamp
    m_timestampLabel = new QLabel("Timestamp: -", controlGroup);
    controlLayout->addWidget(m_timestampLabel);
    
    // Buttons
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    
    m_resetButton = new QPushButton("Reset", controlGroup);
    m_resetButton->setEnabled(false);
    connect(m_resetButton, &QPushButton::clicked, this, &LogReplayDialog::onReset);
    buttonLayout->addWidget(m_resetButton);
    
    m_stepBackButton = new QPushButton("◄ Step Back", controlGroup);
    m_stepBackButton->setEnabled(false);
    connect(m_stepBackButton, &QPushButton::clicked, this, &LogReplayDialog::onStepBackward);
    buttonLayout->addWidget(m_stepBackButton);
    
    m_playButton = new QPushButton("▶ Play", controlGroup);
    m_playButton->setEnabled(false);
    connect(m_playButton, &QPushButton::clicked, this, &LogReplayDialog::onPlay);
    buttonLayout->addWidget(m_playButton);
    
    m_stepForwardButton = new QPushButton("Step Forward ►", controlGroup);
    m_stepForwardButton->setEnabled(false);
    connect(m_stepForwardButton, &QPushButton::clicked, this, &LogReplayDialog::onStepForward);
    buttonLayout->addWidget(m_stepForwardButton);
    
    buttonLayout->addStretch();
    
    // Speed control
    buttonLayout->addWidget(new QLabel("Speed (ms):", controlGroup));
    m_speedSpinBox = new QSpinBox(controlGroup);
    m_speedSpinBox->setRange(100, 5000);
    m_speedSpinBox->setValue(1000);
    m_speedSpinBox->setSingleStep(100);
    connect(m_speedSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &LogReplayDialog::onSpeedChanged);
    buttonLayout->addWidget(m_speedSpinBox);
    
    controlLayout->addLayout(buttonLayout);
    
    mainLayout->addWidget(controlGroup);
    
    // Display area - split between raw log and processed data
    QSplitter* splitter = new QSplitter(Qt::Horizontal, this);
    
    // Left side - Raw log message
    QGroupBox* logGroup = new QGroupBox("Current Message", splitter);
    QVBoxLayout* logLayout = new QVBoxLayout(logGroup);
    
    m_logDisplay = new QTextEdit(logGroup);
    m_logDisplay->setReadOnly(true);
    m_logDisplay->setFont(QFont("Courier", 10));
    logLayout->addWidget(m_logDisplay);
    
    // Right side - Processed data
    QGroupBox* dataGroup = new QGroupBox("Processed Telescope Data", splitter);
    QVBoxLayout* dataLayout = new QVBoxLayout(dataGroup);
    
    m_dataDisplay = new QTextEdit(dataGroup);
    m_dataDisplay->setReadOnly(true);
    m_dataDisplay->setFont(QFont("Courier", 10));
    dataLayout->addWidget(m_dataDisplay);
    
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    
    mainLayout->addWidget(splitter, 1);
}

void LogReplayDialog::onLoadFile()
{
    QString filename = QFileDialog::getOpenFileName(
        this,
        "Load WebSocket Log File",
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/CelestronOriginLogs",
        "Log Files (*.txt *.log);;All Files (*)"
    );
    
    if (filename.isEmpty()) {
        return;
    }
    
    if (loadLogFile(filename)) {
        m_statusLabel->setText(QString("Loaded: %1 (%2 messages)")
                              .arg(QFileInfo(filename).fileName())
                              .arg(m_logEntries.size()));
        
        // Enable controls
        m_positionSlider->setEnabled(true);
        m_positionSlider->setRange(0, m_logEntries.size() - 1);
        m_stepForwardButton->setEnabled(true);
        m_playButton->setEnabled(true);
        m_resetButton->setEnabled(true);
        
        // Reset to start
        onReset();
    }
}

bool LogReplayDialog::loadLogFile(const QString& filename)
{
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Error", "Cannot open log file: " + file.errorString());
        return false;
    }
    
    m_logEntries.clear();
    m_currentIndex = -1;
    
    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    
    // Parse log file
    // Format: [YYYY-MM-DD HH:MM:SS.zzz] DIRECTION: JSON
    QRegularExpression logRegex(R"(\[([\d\-]+\s[\d:\.]+)\]\s+(\w+):\s+(.+))");
    
    int lineNumber = 0;
    QString accumulatedJson;
    int jsonStartLine = 0;
    bool inMultilineJson = false;
    QDateTime multilineTimestamp;
    QString multilineDirection;
    int braceCount = 0;  // KEY ADDITION: Track brace balance
    
    while (!in.atEnd()) {
        QString line = in.readLine();
        lineNumber++;
        
        // Check if this is a new log entry or continuation of JSON
        QRegularExpressionMatch match = logRegex.match(line);
        
        if (match.hasMatch() && !inMultilineJson) {
            // This is a new log entry
            QString timestampStr = match.captured(1);
            QString direction = match.captured(2);
            QString jsonStr = match.captured(3).trimmed();
            
            // Parse timestamp
            QDateTime timestamp = QDateTime::fromString(timestampStr, "yyyy-MM-dd HH:mm:ss.zzz");
            if (!timestamp.isValid()) {
                qWarning() << "Invalid timestamp on line" << lineNumber << ":" << timestampStr;
                continue;
            }
            
            // Check if this is JSON that needs parsing
            if ((direction == "RECV" || direction == "SEND") && jsonStr.startsWith("{")) {
                // Count braces to determine if JSON is complete
                braceCount = jsonStr.count('{') - jsonStr.count('}');
                
                if (braceCount == 0) {
                    // Complete single-line JSON - validate and add
                    QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8());
                    if (doc.isNull()) {
                        qWarning() << "Invalid JSON on line" << lineNumber;
                        continue;
                    }
                    
                    LogEntry entry;
                    entry.timestamp = timestamp;
                    entry.direction = direction;
                    entry.jsonMessage = jsonStr;
                    m_logEntries.append(entry);
                } else {
                    // Start accumulating multiline JSON
                    inMultilineJson = true;
                    jsonStartLine = lineNumber;
                    accumulatedJson = jsonStr;
                    multilineTimestamp = timestamp;
                    multilineDirection = direction;
                }
            } else {
                // Non-JSON entry (SYSTEM, PING, PONG, ERROR, etc.)
                LogEntry entry;
                entry.timestamp = timestamp;
                entry.direction = direction;
                entry.jsonMessage = jsonStr;
                m_logEntries.append(entry);
            }
        }
        else if (inMultilineJson) {
            // Accumulate continuation lines
            accumulatedJson += "\n" + line;
            
            // Update brace count
            braceCount += line.count('{') - line.count('}');
            
            // Check if JSON is complete (braces balanced)
            if (braceCount == 0) {
                // Try to parse accumulated JSON
                QJsonDocument testDoc = QJsonDocument::fromJson(accumulatedJson.toUtf8());
                if (!testDoc.isNull()) {
                    // Successfully parsed - add entry
                    LogEntry entry;
                    entry.timestamp = multilineTimestamp;
                    entry.direction = multilineDirection;
                    entry.jsonMessage = accumulatedJson;
                    m_logEntries.append(entry);
                    
                    inMultilineJson = false;
                    accumulatedJson.clear();
                    braceCount = 0;
                } else {
                    qWarning() << "Multiline JSON invalid after balancing braces at line" << lineNumber;
                    qWarning() << "JSON started at line" << jsonStartLine;
                    // Reset multiline state
                    inMultilineJson = false;
                    accumulatedJson.clear();
                    braceCount = 0;
                }
            }
        }
        else if (!line.trimmed().isEmpty()) {
            qWarning() << "Could not parse line" << lineNumber << ":" << line;
        }
    }
    
    // Check if we ended in the middle of multiline JSON
    if (inMultilineJson) {
        qWarning() << "EOF reached while accumulating JSON starting at line" << jsonStartLine;
    }
    
    file.close();
    
    qDebug() << "Loaded" << m_logEntries.size() << "log entries from" << filename;
    
    return !m_logEntries.isEmpty();
}

void LogReplayDialog::onStepForward()
{
    if (m_currentIndex < m_logEntries.size() - 1) {
        m_currentIndex++;
        processMessage(m_currentIndex);
        updateDisplay();
        updateControls();
    }
}

void LogReplayDialog::onStepBackward()
{
    if (m_currentIndex > 0) {
        // Reset processor and replay up to previous position
        m_processor->reset();
        
        m_currentIndex--;
        
        // Replay all messages up to current position
        for (int i = 0; i <= m_currentIndex; i++) {
            processMessage(i);
        }
        
        updateDisplay();
        updateControls();
    }
}

void LogReplayDialog::onPlay()
{
    if (m_isPlaying) {
        onPause();
        return;
    }
    
    m_isPlaying = true;
    m_playButton->setText("⏸ Pause");
    m_stepForwardButton->setEnabled(false);
    m_stepBackButton->setEnabled(false);
    m_positionSlider->setEnabled(false);
    
    m_playbackTimer->start(m_playbackSpeed);
}

void LogReplayDialog::onPause()
{
    m_isPlaying = false;
    m_playButton->setText("▶ Play");
    m_stepForwardButton->setEnabled(m_currentIndex < m_logEntries.size() - 1);
    m_stepBackButton->setEnabled(m_currentIndex > 0);
    m_positionSlider->setEnabled(true);
    
    m_playbackTimer->stop();
}

void LogReplayDialog::onReset()
{
    if (m_isPlaying) {
        onPause();
    }
    
    m_processor->reset();
    m_currentIndex = -1;
    
    m_logDisplay->clear();
    m_dataDisplay->clear();
    m_positionSlider->setValue(0);
    
    updateDisplay();
    updateControls();
}

void LogReplayDialog::onSliderMoved(int position)
{
    if (position == m_currentIndex) {
        return;
    }
    
    // Reset and replay up to this position
    m_processor->reset();
    
    for (int i = 0; i < position && i < m_logEntries.size(); i++) {
        processMessage(i);
    }
    
    m_currentIndex = position - 1;
    if (m_currentIndex >= 0) {
        updateDisplay();
    }
    
    updateControls();
}

void LogReplayDialog::onPlaybackTimer()
{
    if (m_currentIndex < m_logEntries.size() - 1) {
        onStepForward();
    } else {
        onPause();
    }
}

void LogReplayDialog::onSpeedChanged(int value)
{
    m_playbackSpeed = value;
    if (m_isPlaying) {
        m_playbackTimer->setInterval(m_playbackSpeed);
    }
}

void LogReplayDialog::processMessage(int index)
{
    if (index < 0 || index >= m_logEntries.size()) {
        return;
    }
    
    const LogEntry& entry = m_logEntries[index];
    
    // Only process RECV messages (responses from telescope)
    if (entry.direction == "RECV") {
        bool success = m_processor->processJsonPacket(entry.jsonMessage.toUtf8());
        emit messageProcessed(entry, success);
        
        if (!success) {
            qWarning() << "Failed to process message at index" << index;
        }
    }
}

void LogReplayDialog::updateDisplay()
{
    if (m_currentIndex < 0 || m_currentIndex >= m_logEntries.size()) {
        return;
    }
    
    const LogEntry& entry = m_logEntries[m_currentIndex];
    
    // Update log display
    QString logText;
    logText += QString("Index: %1/%2\n").arg(m_currentIndex + 1).arg(m_logEntries.size());
    logText += QString("Timestamp: %1\n").arg(formatTimestamp(entry.timestamp));
    logText += QString("Direction: %1\n").arg(entry.direction);
    logText += "\n--- JSON Message ---\n";
    
    // Pretty-print JSON
    QJsonDocument doc = QJsonDocument::fromJson(entry.jsonMessage.toUtf8());
    if (!doc.isNull()) {
        logText += doc.toJson(QJsonDocument::Indented);
    } else {
        logText += entry.jsonMessage;
    }
    
    m_logDisplay->setPlainText(logText);
    
    // Update processed data display
    const TelescopeData& data = m_processor->getData();
    
    QString dataText;
    dataText += "=== MOUNT STATUS ===\n";
    dataText += QString("Battery: %1 (%2V, %3A)\n")
        .arg(data.mount.batteryLevel)
        .arg(data.mount.batteryVoltage, 0, 'f', 2)
        .arg(data.mount.batteryCurrent, 0, 'f', 2);
    dataText += QString("Time: %1 %2 %3\n").arg(data.mount.date, data.mount.time, data.mount.timeZone);
    dataText += QString("Position: Lat=%1° Lon=%2°\n")
        .arg(data.mount.latitude * 180.0 / M_PI, 0, 'f', 4)
        .arg(data.mount.longitude * 180.0 / M_PI, 0, 'f', 4);
    dataText += QString("Aligned: %1  Tracking: %2  Goto Over: %3\n")
        .arg(data.mount.isAligned ? "Yes" : "No")
        .arg(data.mount.isTracking ? "Yes" : "No")
        .arg(data.mount.isGotoOver ? "Yes" : "No");
    dataText += QString("Altitude: %1°  Azimuth: %2°\n")
        .arg(data.mount.altitude, 0, 'f', 2)
        .arg(data.mount.azimuth, 0, 'f', 2);
    dataText += QString("Align Refs: %1\n").arg(data.mount.numAlignRefs);
    
    dataText += "\n=== CAMERA STATUS ===\n";
    dataText += QString("Exposure: %1s  ISO: %2  Binning: %3  Bit Depth: %4\n")
        .arg(data.camera.exposure, 0, 'f', 3)
        .arg(data.camera.iso)
        .arg(data.camera.binning)
        .arg(data.camera.bitDepth);
    dataText += QString("Color Balance: R=%1 G=%2 B=%3\n")
        .arg(data.camera.colorRBalance, 0, 'f', 2)
        .arg(data.camera.colorGBalance, 0, 'f', 2)
        .arg(data.camera.colorBBalance, 0, 'f', 2);
    
    dataText += "\n=== FOCUSER STATUS ===\n";
    dataText += QString("Position: %1  Range: [%2-%3]\n")
        .arg(data.focuser.position)
        .arg(data.focuser.calibrationLowerLimit)
        .arg(data.focuser.calibrationUpperLimit);
    dataText += QString("Calibrated: %1  Progress: %2%\n")
        .arg(data.focuser.isCalibrationComplete ? "Yes" : "No")
        .arg(data.focuser.percentageCalibrationComplete);
    
    dataText += "\n=== ENVIRONMENT ===\n";
    dataText += QString("Ambient: %1°C  Camera: %2°C  CPU: %3°C  Cell: %4°C\n")
        .arg(data.environment.ambientTemperature, 0, 'f', 1)
        .arg(data.environment.cameraTemperature, 0, 'f', 1)
        .arg(data.environment.cpuTemperature, 0, 'f', 1)
        .arg(data.environment.frontCellTemperature, 0, 'f', 1);
    dataText += QString("Humidity: %1%  Dew Point: %2°C\n")
        .arg(data.environment.humidity, 0, 'f', 0)
        .arg(data.environment.dewPoint, 0, 'f', 1);
    dataText += QString("CPU Fan: %1  OTA Fan: %2\n")
        .arg(data.environment.cpuFanOn ? "On" : "Off")
        .arg(data.environment.otaFanOn ? "On" : "Off");
    
    dataText += "\n=== LAST IMAGE ===\n";
    dataText += QString("File: %1\n").arg(data.lastImage.fileLocation);
    dataText += QString("Type: %1\n").arg(data.lastImage.imageType);
    dataText += QString("RA: %1° (%2h)  Dec: %3°\n")
        .arg(data.lastImage.ra * 180.0 / M_PI, 0, 'f', 4)
        .arg(data.lastImage.ra * 12.0 / M_PI, 0, 'f', 4)
        .arg(data.lastImage.dec * 180.0 / M_PI, 0, 'f', 4);
    
    dataText += "\n=== DISK ===\n";
    double totalGB = data.disk.capacity / (1024.0 * 1024.0 * 1024.0);
    double freeGB = data.disk.freeBytes / (1024.0 * 1024.0 * 1024.0);
    dataText += QString("Total: %1 GB  Free: %2 GB  Level: %3\n")
        .arg(totalGB, 0, 'f', 2)
        .arg(freeGB, 0, 'f', 2)
        .arg(data.disk.level);
    
    m_dataDisplay->setPlainText(dataText);
    
    // Update position displays
    m_positionSlider->setValue(m_currentIndex);
    m_positionLabel->setText(QString("%1/%2").arg(m_currentIndex + 1).arg(m_logEntries.size()));
    m_timestampLabel->setText(QString("Timestamp: %1").arg(formatTimestamp(entry.timestamp)));
    
    emit positionChanged(m_currentIndex + 1, m_logEntries.size());
}

void LogReplayDialog::updateControls()
{
    if (!m_isPlaying) {
        m_stepBackButton->setEnabled(m_currentIndex > 0);
        m_stepForwardButton->setEnabled(m_currentIndex < m_logEntries.size() - 1);
        m_playButton->setEnabled(!m_logEntries.isEmpty());
    }
}

QString LogReplayDialog::formatTimestamp(const QDateTime& dt) const
{
    return dt.toString("yyyy-MM-dd HH:mm:ss.zzz");
}
