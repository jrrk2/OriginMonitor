#include "OriginBackend.hpp"
#include "TelescopeGUI.hpp"
#include "CommandInterface.hpp"

CommandInterface::CommandInterface(TelescopeGUI *telescopeGUI, QWidget *parent) 
    : QWidget(parent), telescopeGUI(telescopeGUI) {
    setupUI();
}

void CommandInterface::sendCommand() {
    // Check if we have a valid TelescopeGUI pointer
    if (!telescopeGUI) {
        QMessageBox::warning(this, "Error", "Cannot send command - not properly connected to main window");
        return;
    }
    
    QString command = commandComboBox->currentText();
    QString destination = destinationComboBox->currentText();
    
    // Create the JSON command
    QJsonObject jsonCommand;    
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
    
    // Send the command using the TelescopeGUI's method
    telescopeGUI->originBackend->sendCommand(command, destination, jsonCommand);
    
    // Add to command history - we'll assume it worked if we got here
    QListWidgetItem *item = new QListWidgetItem(QString("Sent: %1 to %2").arg(command, destination));
    commandHistoryList->addItem(item);
    commandHistoryList->scrollToBottom();
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
        "GetCaptureParameters", 
        "SetCaptureParameters", 
        "SetStretch", 
        "SetEnableAuto", 
        "SetEnableManual"
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
        "LiveStream", 
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
