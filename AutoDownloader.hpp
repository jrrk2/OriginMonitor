#pragma once

#include <QObject>
#include <QQueue>
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QWebSocket>

/**
 * @brief Class for automatically downloading observations from the telescope
 * 
 * This class handles the automatic discovery of available observations
 * and downloads them to a specified directory.
 */
class AutoDownloader : public QObject {
    Q_OBJECT
    
public:
    /**
     * @brief Constructor
     * @param webSocket The WebSocket for sending commands
     * @param ipAddress The IP address of the telescope
     * @param downloadPath The path to download observations to
     * @param parent The parent QObject
     */
    AutoDownloader(QWebSocket *webSocket, const QString &ipAddress, 
                   const QString &downloadPath = "Downloads", QObject *parent = nullptr);
    
    /**
     * @brief Start the download process
     */
    void startDownload();
    
    /**
     * @brief Stop the download process
     */
    void stopDownload();
    /**
    * @brief Set the download path
    * @param path The new download path
    */
    void setDownloadPath(const QString &path);
  
signals:
    /**
     * @brief Signal emitted when a directory starts downloading
     * @param directory The name of the directory
     */
    void directoryDownloadStarted(const QString &directory);
    
    /**
     * @brief Signal emitted when a file starts downloading
     * @param fileName The name of the file
     */
    void fileDownloadStarted(const QString &fileName);
    
    /**
     * @brief Signal emitted when a file has been downloaded
     * @param fileName The name of the file
     * @param success Whether the download was successful
     */
    void fileDownloaded(const QString &fileName, bool success);
    
    /**
     * @brief Signal emitted when a directory has been downloaded
     * @param directory The name of the directory
     */
    void directoryDownloaded(const QString &directory);
    
    /**
     * @brief Signal emitted when all downloads are complete
     */
    void allDownloadsComplete();
    
    /**
     * @brief Signal emitted to report download progress
     * @param currentFile The current file being downloaded
     * @param filesCompleted The number of files completed
     * @param totalFiles The total number of files to download
     * @param bytesReceived The number of bytes received for the current file
     * @param bytesTotal The total number of bytes for the current file
     */
    void downloadProgress(const QString &currentFile, int filesCompleted, 
                          int totalFiles, qint64 bytesReceived, qint64 bytesTotal);
    
private slots:
    /**
     * @brief Process the list of available directories
     * @param message The JSON message containing the directory list
     */
    void processDirectoryList(const QString &message);
    
    /**
     * @brief Process the list of files in a directory
     * @param message The JSON message containing the file list
     */
    void processFileList(const QString &message);
    
    /**
     * @brief Slot called when a file download is complete
     */
    void onFileDownloaded();
    
    /**
     * @brief Slot called when a file download reports progress
     * @param bytesReceived The number of bytes received
     * @param bytesTotal The total number of bytes
     */
    void onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    
    /**
     * @brief Slot called when a message is received from the WebSocket
     * @param message The received message
     */
    void onTextMessageReceived(const QString &message);
    
private:
    /**
     * @brief Send a command to the telescope
     * @param command The command to send
     * @param destination The destination for the command
     * @param params Additional parameters for the command
     */
    void sendCommand(const QString &command, const QString &destination, 
                     const QJsonObject &params = QJsonObject());
    
    /**
     * @brief Download a file from the telescope
     * @param filePath The path to the file on the telescope
     */
    void downloadFile(const QString &filePath);
    
    /**
     * @brief Process the next directory in the queue
     */
    void processNextDirectory();
    
    /**
     * @brief Process the next file in the queue
     */
    void processNextFile();
    
    /** The WebSocket for sending commands */
    QWebSocket *webSocket;
    
    /** The IP address of the telescope */
    QString ipAddress;
    
    /** The base path to download observations to */
    QString downloadPath;
    
    /** The network access manager for downloading files */
    QNetworkAccessManager *networkManager;
    
    /** The queue of directories to process */
    QQueue<QString> directoryQueue;
    
    /** The queue of files to download */
    QQueue<QString> fileQueue;
    
    /** The current directory being processed */
    QString currentDirectory;
    
    /** The current file being downloaded */
    QString currentFile;
    
    /** The total number of files to download */
    int totalFiles;
    
    /** The number of files completed */
    int filesCompleted;
    
    /** Whether a download is in progress */
    bool downloadInProgress;
    
    /** The sequence ID for the next command */
    int nextSequenceId;
};
