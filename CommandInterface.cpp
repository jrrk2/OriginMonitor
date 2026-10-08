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
    // NOTE: only a subset of these (GotoRaDec, RunImaging, RunInitialize, Reboot, PowerDownOrigin,
    // SetFans/GetFans, GetSensors, SetFilter/GetFilter, EnableAuto/EnableManual, GetPosition/MoveTo,
    // AutoFocusAfterGoto/AutoFocusOnTemperatureChange, SetMountConfig, Connect/Disconnect/TurnOnLight/
    // TurnOffLight/SetLightLevel (ElPanel), RunObservingList, RunInfinityAutoFocus, PolarAlign,
    // RunGenerateNewDarks/RunGenerateNewFlat, RunSampleCapture, RunAddReference,
    // GetCaptureParameters/SetCaptureParameters, GetLogs) have been confirmed against OLV OriginLiveView's
    // TelescopeControl.cs. The rest of this list is speculative/reverse-engineered and unconfirmed against
    // any known-working client or real hardware - treat as experimental when sending manually.
    //
    // Live-probed 2026-09-17 against a real Origin at 192.168.1.195 (demo mode, read-only "Get"/"Is" commands
    // only): GetCameraInfo, GetFilter, GetSensors, GetFans, GetPosition, GetPositionLimits, GetVelocity,
    // GetEnableManual, GetDisableLiveStream, GetMountConfig, IsAligned, IsGotoOver, IsTracking, IsMoveToOver,
    // GetLogs, GetVersion, GetModel, GetAutomaticUpdates, GetUpdateChannel, HasUpdateAvailable, GetVisibleWiFi,
    // GetWiFiNetworks, HasInternetConnection, GetBrightnessLevel, GetAccelerometer, GetAvailableImagingLists,
    // GetListOfAvailableDirectories, GetAvailableMosaics, and GetLightLevel/IsLightOn (Destination: ElPanel,
    // which really is a recognized destination - it only errors with "EL Panel not available" because no
    // panel is attached) all returned real data/ErrorCode 0. CONFIRMED INVALID on real firmware (ErrorCode -2
    // "Unknown command"): IsCalibrationComplete. GetCooling/SetCooling and GetVersionInfo were also confirmed
    // invalid and removed entirely (Origin has no cooled camera; use GetVersion/GetModel).
    //
    // Cross-checked 2026-09-17 against genuine packet captures of the real Celestron Origin iOS app
    // (~/origin_anal/origin1.txt, dither.txt - actual client command traffic, not guessed). Confirmed real:
    // CancelImaging/TaskController, RunCenterTarget/TaskController, GetObservingListInfo/TaskController,
    // HaltTasks/TaskController, GetAutomaticUpdates+SetAutomaticUpdates+GetUpdateChannel+SetUpdateChannel+
    // Update+HasUpdateAvailable/System, GetWiFiNetworks+GetDirectConnectPassword+GetForceDirectConnect+
    // HasInternetConnection+SetRegulatoryDomain/Network, GetBrightnessLevel/LedRing and /LampController,
    // GetFocuserAdvancedSettings+GetPositionLimits+MoveTo/Focuser, GetMountConfig+GotoRaDec+Slew/Mount,
    // GetAvailableStackedMasters+GetStretch+SetStretch/ImageServer, GetStatus/Autoguider, GetStatus/ElPanel,
    // GetStatus/DewHeater. IMPORTANT correction: GetSerialNumber is real, but its Destination is
    // "FactoryCalibrationController", not "System" - select that destination when using it from this tab.
    //
    // Live-captured 2026-09-17 during a real in-progress mosaic (official app, current firmware):
    // the correct mosaic-start command is RunCaptureMosaic/TaskController with MosaicWidth/MosaicHeight
    // as angular radians (not tile counts) plus DitherDistance/DitherInterval - NOT CaptureMosaic, which
    // was an OLV-ported command name that is now confirmed stale/obsolete (OLV's mosaic code predates the
    // telescope's current mosaic implementation). Also confirmed real: GetAvailableMosaics/Mosaic,
    // GetMosaicTiles/Mosaic, GetMaximumTiles/Mosaic. RunComposeMosaic was not seen in our capture, but
    // per user confirmation it can only be called after all tiles are captured (TilesCompleted ==
    // TilesTotal) - our capture window ended while the mosaic was still IN_PROGRESS with 0 tiles
    // complete, so this is inconclusive rather than confirmed unused.
    commandComboBox->addItems({
        // Mount commands
        "AddAlignRef",
        "DeleteAllAlignRefs",
        "EnableCustomRate9",
        "EnablePec",
        "EnableTracking",
        "GetMountConfig",
        "GotoAltAzm",
        "GotoEnc",
        "GotoRaDec",
        "IsAligned",
        "IsGotoOver",
        "IsTracking",
        "MoveRa",
        "MoveDec",
        "SetMountConfig",
        "Slew",
        "StartTracking",
        "StopAll",
        "StopTracking",
        "TrackMovingTarget",
        // Camera commands
        "GetCameraInfo",
        "GetCaptureParameters",
        "GetFilter",
        "GetSavePlateSolves",
        "SetCaptureParameters",
        "SetFilter",
        "SetSavePlateSolves",
        // Focuser commands
        "AutoFocusAfterGoto",
        "AutoFocusOnTemperatureChange",
        "GetFocuserAdvancedSettings",
        "GetPosition",
        "GetPositionLimits",
        "GetVelocity",
        "HaltCalibration",
        "IsCalibrationComplete",
        "IsMoveToOver",
        "MoveTo",
        "RunCalibration",
        // LiveStream commands
        "CloseLiveStream",
        "GetDisableLiveStream",
        "GetEnableManual",
        "SetDisableLiveStream",
        "SetEnableAuto",
        "SetEnableManual",
        // TaskController commands
        "CalibrateFocuser",
        "CancelAutoCapture",
        "CancelAutoFocus",
        "CancelImaging",
        "CaptureVideo",
        "CopyAllDirectoriesToFlashDrive",
        "CopyDirectoryToFlashDrive",
        "GetObservingListInfo",
        "ImageNextTarget",
        "MeasureDarkCurrent",
        "PolarAlign",
        "RecaptureImagingListObject",
        "RecaptureMosaicTile",
        "RestoreDarksToFactory",
        "RestoreFlatsToFactory",
        "RunAddReference",
        "RunCaptureMosaic",
        "RunCenterTarget",
        "RunComposeMosaic",
        "RunGenerateAmplitudeVsGain",
        "RunGenerateNewDarks",
        "RunGenerateNewFlat",
        "RunImaging",
        "RunInfinityAutoFocus",
        "RunInitialize",
        "RunManualFocus",
        "RunObservingList",
        "RunSampleCapture",
        "RunTemperatureShiftAutoFocus",
        "RunTerrestrialAutoFocus",
        "SafeToRemoveFlashDrive",
        // Environment commands
        "GetFans",
        "GetSensors",
        "GetStatus",
        "RecalibrateEnvironmentSensor",
        "SetFans",
        // DewHeater commands
        "EnableAuto",
        "EnableManual",
        // Disk commands
        "DeleteAllImageDirectories",
        "DeleteImageDirectory",
        // ImageServer commands
        "DeleteAllImagingLists",
        "DeleteImagingList",
        "GetAvailableImagingLists",
        "GetAvailableStackedMasters",
        "GetFinalStackedMaster",
        "GetListOfAvailableDirectories",
        "GetStackedMasterFromDirectory",
        "GetStretch",
        "GetThumbnail",
        "SetStretch",
        // Mosaic commands
        "DeleteAllMosaics",
        "DeleteMosaic",
        "GetAvailableMosaics",
        "GetMaximumTiles",
        "GetMosaicMetadata",
        "GetMosaicTiles",
        // LedRing commands
        "GetBrightnessLevel",
        "SetBrightnessLevel",
        // ElPanel commands
        "GetLightLevel",
        "IsLightOn",
        "SetLightLevel",
        "TurnOffLight",
        "TurnOnLight",
        // LampController commands (same names as LedRing, use Destination to distinguish)
        // OrientationSensor commands
        "GetAccelerometer",
        "ZeroCalibrate",
        // Network commands
        "5GHzAccessPoint",
        "AddWiFiNetwork",
        "ConnectToSpecificNetwork",
        "DeleteWiFiNetwork",
        "ForceDirectConnect",
        "Get5GHzAccessPoint",
        "GetDirectConnectPassword",
        "GetForceDirectConnect",
        "GetVisibleWiFi",
        "GetWiFiNetworks",
        "HasInternetConnection",
        "ReconnectDirectConnect",
        "ReconnectWiFi",
        "RemoveAllKnownNetworks",
        "SetDeviceIsHost",
        "SetDirectConnectPassword",
        "SetRegulatoryDomain",
        // HostController commands
        "GetDeviceIsHost",
        "GetPin",
        "ResetPin",
        "SetPin",
        // Optics commands
        "GetConfiguration",
        "GetSupportedModels",
        "SetConfiguration",
        // System commands
        "CriticalError",
        "EnableCustomerMode",
        "EnableFTPAccess",
        "GetAutomaticUpdates",
        "GetLogs",
        "GetModel",
        "GetSerialNumber",
        "GetUpdateChannel",
        "GetVersion",
        "HasUpdateAvailable",
        "PowerDownOrigin",
        "Reboot",
        "RunFactoryReset",
        "SetAutomaticUpdates",
        "SetUpdateChannel",
        "ShutDownCore",
        "Update",
        // Autoguider commands
        // (GetStatus - use Destination: Autoguider)
        // FactoryCalibrationController commands
        "BeginFactoryCalibration",
        "CheckCameraOrientation",
        "ClearFAImages",
        "GetCertification",
        "GetCurrentLocation",
        "GetMountId",
        "GetRtcTime",
        "GetSKUNumber",
        "HotSpotCentered",
        "SetHotSpotAnalysisParameters",
        "SetSKUNumber",
        "SetSerialNumber",
        "TransferFAImagesToFlashdrive",
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
        "TaskController",
        "System",
        "Network",
        "LedRing",
        "LampController",
        "ElPanel",
        "Autoguider",
        "HostController",
        "Optics",
        "Mosaic",
        "FactoryCalibrationController",
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
