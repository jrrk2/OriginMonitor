#include "CommandInterface.hpp"

CommandInterface::CommandInterface(QWebSocket *webSocket, QWidget *parent) 
    : QWidget(parent), webSocket(webSocket) {
    setupUI();
}

void CommandInterface::sendCommand() {
    QString command = commandComboBox->currentText();
    QString destination = destinationComboBox->currentText();
    
    // Create a sequential ID for the command
    static int sequenceId = 1000;
    sequenceId++;
    
    // Create the JSON command
    QJsonObject jsonCommand;
    jsonCommand["Command"] = command;
    jsonCommand["Destination"] = destination;
    jsonCommand["SequenceID"] = sequenceId;
    jsonCommand["Source"] = "QtApp";
    jsonCommand["Type"] = "Command";
    
    // Add any parameters
    if (!parametersEdit->text().isEmpty()) {
        QJsonDocument paramsDoc = QJsonDocument::fromJson(parametersEdit->text().toUtf8());
        if (!paramsDoc.isNull() && paramsDoc.isObject()) {
            QJsonObject paramsObj = paramsDoc.object();
            
            // Add each parameter to the command
            for (auto it = paramsObj.begin(); it != paramsObj.end(); ++it) {
                jsonCommand[it.key()] = it.value();
            }
        } else {
            QMessageBox::warning(this, "Invalid Parameters", "Parameters must be a valid JSON object");
            return;
        }
    }
    
    // Convert to JSON and send
    QJsonDocument doc(jsonCommand);
    QString message = doc.toJson();
    
    if (webSocket->isValid() && webSocket->state() == QAbstractSocket::ConnectedState) {
        webSocket->sendTextMessage(message);
        
        // Add to command history
        QListWidgetItem *item = new QListWidgetItem(QString("Sent: %1 to %2").arg(command, destination));
        commandHistoryList->addItem(item);
        commandHistoryList->scrollToBottom();
    } else {
        QMessageBox::warning(this, "Connection Error", "Not connected to telescope");
    }
}

void CommandInterface::setupUI() {
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    
    // Command inputs
    QFormLayout *formLayout = new QFormLayout();
    
    commandComboBox = new QComboBox(this);
    commandComboBox->addItems({
        "GetStatus", 
        "StartTracking", 
        "StopTracking", 
        "StartAlignment", 
        "AddAlignmentPoint", 
        "FinishAlignment", 
        "MoveAxis", 
        "AbortAxisMovement",
        "GetCaptureParameters", 
        "SetCaptureParameters", 
        "CaptureImage",
        "MoveToPosition", 
        "AbortMoveTo"
    });
    formLayout->addRow("Command:", commandComboBox);
    
    destinationComboBox = new QComboBox(this);
    destinationComboBox->addItems({
        "Mount", 
        "Camera", 
        "Focuser", 
        "Environment", 
        "ImageServer", 
        "Disk", 
        "DewHeater", 
        "OrientationSensor", 
        "System", 
        "All"
    });
    formLayout->addRow("Destination:", destinationComboBox);
    
    parametersEdit = new QLineEdit(this);
    parametersEdit->setPlaceholderText("Optional JSON parameters: {\"param1\": value1, \"param2\": value2}");
    formLayout->addRow("Parameters:", parametersEdit);
    
    sendButton = new QPushButton("Send Command", this);
    connect(sendButton, &QPushButton::clicked, this, &CommandInterface::sendCommand);
    
    mainLayout->addLayout(formLayout);
    mainLayout->addWidget(sendButton);
    
    // Command history
    QGroupBox *historyGroup = new QGroupBox("Command History", this);
    QVBoxLayout *historyLayout = new QVBoxLayout(historyGroup);
    
    commandHistoryList = new QListWidget(this);
    historyLayout->addWidget(commandHistoryList);
    
    mainLayout->addWidget(historyGroup);
}
