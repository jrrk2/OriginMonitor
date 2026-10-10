#pragma once

#include <QDialog>
#include <QTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QGroupBox>
#include <QSpinBox>
#include "TelescopeDataProcessor.hpp"

struct LogEntry {
    QDateTime timestamp;
    QString direction;      // RECV, SEND, or SYSTEM
    QString jsonMessage;
};

/**
 * @brief Dialog for replaying WebSocket logs
 * 
 * This dialog allows you to:
 * - Load historical log files
 * - Step through messages one at a time
 * - Play back at various speeds
 * - View the processed data at each step
 * - Compare with expected values
 */
class LogReplayDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LogReplayDialog(TelescopeDataProcessor* processor, QWidget *parent = nullptr);
    ~LogReplayDialog();

    // Load a log file
    bool loadLogFile(const QString& filename);

signals:
    // Emitted when a message is processed
    void messageProcessed(const LogEntry& entry, bool success);
    
    // Emitted when playback position changes
    void positionChanged(int current, int total);

private slots:
    void onLoadFile();
    void onStepForward();
    void onStepBackward();
    void onPlay();
    void onPause();
    void onReset();
    void onSliderMoved(int position);
    void onPlaybackTimer();
    void onSpeedChanged(int value);

private:
    void setupUI();
    void updateDisplay();
    void processMessage(int index);
    void updateControls();
    QString formatTimestamp(const QDateTime& dt) const;
    
    // Data
    TelescopeDataProcessor* m_processor;
    QVector<LogEntry> m_logEntries;
    int m_currentIndex;
    bool m_isPlaying;
    int m_playbackSpeed;  // milliseconds between messages
    
    // UI Elements
    QTextEdit* m_logDisplay;
    QTextEdit* m_dataDisplay;
    QSlider* m_positionSlider;
    QLabel* m_positionLabel;
    QLabel* m_timestampLabel;
    QPushButton* m_loadButton;
    QPushButton* m_stepBackButton;
    QPushButton* m_stepForwardButton;
    QPushButton* m_playButton;
    QPushButton* m_resetButton;
    QSpinBox* m_speedSpinBox;
    QTimer* m_playbackTimer;
    
    // Status display
    QLabel* m_statusLabel;
};
