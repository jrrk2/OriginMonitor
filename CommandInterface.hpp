#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QListWidget>
#include <QGroupBox>
#include <QWebSocket>
#include <QJsonObject>
#include <QJsonDocument>
#include <QMessageBox>

/**
 * @brief Interface for sending commands to the telescope
 * 
 * This class provides a UI for sending commands to the telescope,
 * with a dropdown for selecting the command and destination,
 * and a text field for entering additional parameters.
 */
class CommandInterface : public QWidget {
    Q_OBJECT
    
public:
    /**
     * @brief Constructor
     * @param webSocket The WebSocket for sending commands
     * @param parent The parent widget
     */
    CommandInterface(QWebSocket *webSocket, QWidget *parent = nullptr);
    
private slots:
    /**
     * @brief Slot triggered when the send button is clicked
     * 
     * Gathers the command parameters and sends the command
     * to the telescope.
     */
    void sendCommand();
    
private:
    /**
     * @brief Set up the UI elements
     */
    void setupUI();
    
    /** The WebSocket for sending commands */
    QWebSocket *webSocket;
    
    /** Dropdown for selecting the command */
    QComboBox *commandComboBox;
    
    /** Dropdown for selecting the destination */
    QComboBox *destinationComboBox;
    
    /** Text field for entering additional parameters */
    QLineEdit *parametersEdit;
    
    /** Button for sending the command */
    QPushButton *sendButton;
    
    /** List for displaying the command history */
    QListWidget *commandHistoryList;
};
