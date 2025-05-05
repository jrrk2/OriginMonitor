#include "AutoDownloader.hpp"
#include <QDebug>
#include <QEventLoop>

AutoDownloader::AutoDownloader(QWebSocket *webSocket, const QString &ipAddress, 
                               const QString &downloadPath, QObject *parent)
    : QObject(parent),
      webSocket(webSocket),
      ipAddress(ipAddress),
      downloadPath(downloadPath),
      networkManager(new QNetworkAccessManager(this)),
      totalFiles(0),
      filesCompleted(0),
      downloadInProgress(false),
      nextSequenceId(1000) {
    
    // Create download directory if it doesn't exist
    QDir dir(downloadPath);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    
    // Connect WebSocket message received signal
    connect(webSocket, &QWebSocket::textMessageReceived, 
            this, &AutoDownloader::onTextMessageReceived);
}

void AutoDownloader::startDownload() {
    qDebug() << "Starting automatic download of observations";
    
    // Reset counters and queues
    totalFiles = 0;
    filesCompleted = 0;
    directoryQueue.clear();
    fileQueue.clear();
    
    // Request the list of available directories
    sendCommand("GetListOfAvailableDirectories", "ImageServer");
}

void AutoDownloader::stopDownload() {
    qDebug() << "Stopping automatic download";
    
    // Clear the queues
    directoryQueue.clear();
    fileQueue.clear();
    
    // Cancel any ongoing download
    if (downloadInProgress) {
        QNetworkReply *reply = networkManager->findChild<QNetworkReply*>();
        if (reply) {
            reply->abort();
            reply->deleteLater();
        }
        downloadInProgress = false;
    }
}

void AutoDownloader::processDirectoryList(const QString &message) {
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) {
        qDebug() << "Failed to parse directory list JSON";
        return;
    }
    
    QJsonObject obj = doc.object();
    
    // Check if this is a response to GetListOfAvailableDirectories
    if (obj["Command"].toString() != "GetListOfAvailableDirectories" || 
        obj["Type"].toString() != "Response") {
        return;
    }
    
    // Get the directory list
    QJsonArray dirList = obj["DirectoryList"].toArray();
    
    if (dirList.isEmpty()) {
        qDebug() << "No directories found";
        emit allDownloadsComplete();
        return;
    }
    
    qDebug() << "Found" << dirList.size() << "directories";
    
    // Add each directory to the queue
    for (const auto &dir : dirList) {
        directoryQueue.enqueue(dir.toString());
    }
    
    // Start processing the first directory
    processNextDirectory();
}

void AutoDownloader::processFileList(const QString &message) {
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) {
        qDebug() << "Failed to parse file list JSON";
        return;
    }
    
    QJsonObject obj = doc.object();
    
    // Check if this is a response to GetDirectoryContents
    if (obj["Command"].toString() != "GetDirectoryContents" || 
        obj["Type"].toString() != "Response") {
        return;
    }
    
    // Get the file list
    QJsonArray fileList = obj["FileList"].toArray();
    
    if (fileList.isEmpty()) {
        qDebug() << "No files found in directory" << currentDirectory;
        // Move on to the next directory
        emit directoryDownloaded(currentDirectory);
        processNextDirectory();
        return;
    }
    
    qDebug() << "Found" << fileList.size() << "files in directory" << currentDirectory;
    
    // Create subdirectory for this observation
    QDir dir(downloadPath);
    dir.mkpath(currentDirectory);
    
    // Add each file to the queue
    for (const auto &file : fileList) {
        QString filePath = currentDirectory + "/" + file.toString();
        fileQueue.enqueue(filePath);
        totalFiles++;
    }
    
    // Start downloading the first file
    processNextFile();
}

void AutoDownloader::onFileDownloaded() {
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    
    bool success = false;
    
    if (reply->error() == QNetworkReply::NoError) {
        // Get the file data
        QByteArray fileData = reply->readAll();
        
        // Save the file to disk
        QString localPath = downloadPath + "/" + currentFile;
        QFile file(localPath);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(fileData);
            file.close();
            success = true;
            qDebug() << "Downloaded file:" << currentFile << "(" << fileData.size() << "bytes)";
        } else {
            qDebug() << "Failed to save file:" << currentFile;
        }
    } else {
        qDebug() << "Error downloading file:" << currentFile << "-" << reply->errorString();
    }
    
    // Clean up
    reply->deleteLater();
    
    // Update counters
    filesCompleted++;
    downloadInProgress = false;
    
    // Emit signal
    emit fileDownloaded(currentFile, success);
    
    // Process the next file or directory
    if (!fileQueue.isEmpty()) {
        processNextFile();
    } else {
        emit directoryDownloaded(currentDirectory);
        processNextDirectory();
    }
    
    // Check if all downloads are complete
    if (directoryQueue.isEmpty() && fileQueue.isEmpty() && !downloadInProgress) {
        emit allDownloadsComplete();
    }
}

void AutoDownloader::onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal) {
    emit downloadProgress(currentFile, filesCompleted, totalFiles, bytesReceived, bytesTotal);
}

void AutoDownloader::onTextMessageReceived(const QString &message) {
    // Process messages from the telescope
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) return;
    
    QJsonObject obj = doc.object();
    
    // Check the type of message
    QString type = obj["Type"].toString();
    QString command = obj["Command"].toString();
    
    if (type == "Response") {
        if (command == "GetListOfAvailableDirectories") {
            processDirectoryList(message);
        } else if (command == "GetDirectoryContents") {
            processFileList(message);
        }
    }
}

void AutoDownloader::sendCommand(const QString &command, const QString &destination, 
                               const QJsonObject &params) {
    nextSequenceId++;
    
    // Create the JSON command
    QJsonObject jsonCommand;
    jsonCommand["Command"] = command;
    jsonCommand["Destination"] = destination;
    jsonCommand["SequenceID"] = nextSequenceId;
    jsonCommand["Source"] = "AutoDownloader";
    jsonCommand["Type"] = "Command";
    
    // Add any parameters
    for (auto it = params.begin(); it != params.end(); ++it) {
        jsonCommand[it.key()] = it.value();
    }
    
    // Send via WebSocket
    QJsonDocument doc(jsonCommand);
    QString message = doc.toJson();
    
    if (webSocket->isValid() && webSocket->state() == QAbstractSocket::ConnectedState) {
        webSocket->sendTextMessage(message);
    }
}

void AutoDownloader::downloadFile(const QString &filePath) {
    if (ipAddress.isEmpty()) return;
    
    // Construct the proper URL path
    QString fullPath = QString("http://%1/SmartScope-1.0/dev2/%2").arg(ipAddress, filePath);
    QUrl url(fullPath);
    QNetworkRequest request(url);
    
    // Set appropriate headers for the request
    request.setRawHeader("Cache-Control", "no-cache");
    request.setRawHeader("Accept", "*/*");
    request.setRawHeader("User-Agent", "CelestronOriginMonitor Qt Application");
    request.setRawHeader("Connection", "keep-alive");
    
    qDebug() << "Downloading file from:" << fullPath;
    
    QNetworkReply *reply = networkManager->get(request);
    
    connect(reply, &QNetworkReply::finished, this, &AutoDownloader::onFileDownloaded);
    connect(reply, &QNetworkReply::downloadProgress, this, &AutoDownloader::onDownloadProgress);
    
    // Update state
    downloadInProgress = true;
    currentFile = filePath;
    
    // Emit signal
    emit fileDownloadStarted(filePath);
}

void AutoDownloader::processNextDirectory() {
    if (directoryQueue.isEmpty()) {
        qDebug() << "All directories processed";
        if (!downloadInProgress) {
            emit allDownloadsComplete();
        }
        return;
    }
    
    // Get the next directory
    currentDirectory = directoryQueue.dequeue();
    qDebug() << "Processing directory:" << currentDirectory;
    
    // Emit signal
    emit directoryDownloadStarted(currentDirectory);
    
    // Request the list of files in this directory
    QJsonObject params;
    params["Directory"] = currentDirectory;
    sendCommand("GetDirectoryContents", "ImageServer", params);
}

void AutoDownloader::processNextFile() {
    if (fileQueue.isEmpty()) {
        qDebug() << "All files processed in current directory";
        return;
    }
    
    // Get the next file
    QString filePath = fileQueue.dequeue();
    qDebug() << "Processing file:" << filePath;
    
    // Download the file
    downloadFile(filePath);
}

void AutoDownloader::setDownloadPath(const QString &path) {
    downloadPath = path;
    
    // Create the directory if it doesn't exist
    QDir dir(downloadPath);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
}
