#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QMainWindow>
#include <QPixmap>

QT_BEGIN_NAMESPACE
class QLabel;
class QFileDialog;
class QPushButton;
class QTcpSocket;
class QTimer;
class QResizeEvent;
class QStackedWidget;
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    enum class Page { Home, Sensors, Camera, Reverse, Sentinel };
    enum class SensorKind { Ap3216c, Icm20608, Dht11, Sr04, Sr501 };
    enum class CameraRequest { None, Photo, PreviewStart, PreviewFrame, PreviewStop };

    void switchPage(Page page);
    void connectToServer();
    void sendPing();
    void readSensor();
    void readImu();
    static const char *sensorName(SensorKind kind);
    void requestSensor(SensorKind kind);
    void toggleSensorRefresh(SensorKind kind);
    void capturePhoto();
    void savePhoto();
    void togglePreview();
    void toggleReverseMonitoring();
    void startPreview(bool reverse, bool sentinel = false);
    void toggleSentinelMonitoring();
    void stopSentinelMonitoring();
    void resetSentinel();
    void acceptSr501Level(unsigned int level);
    void clearSr501Values();
    void requestPreviewFrame();
    void requestPreviewStop();
    void resetPreview();
    void sendCameraRequest(CameraRequest request);
    void failCameraRequest(const QString &message);
    void toggleAutoRefresh();
    void stopAutoRefresh();
    void readSocketData();
    void handleReply(const QByteArray &line);
    void finishSensorRequest();
    void failSensorRequest(const QString &message, bool retryDeviceRead = false);
    void finishCameraRequest();
    void handleJpeg(quint32 id, const QByteArray &jpeg);
    void resetReceiveState();
    void clearPhoto();
    void updatePhotoDisplay();
    void clearSensorValues();
    void clearImuValues();
    void clearDhtValues();
    void clearSr04Values();
    void clearRequestedSensorValues();
    void updateButtons();

    QTcpSocket *socket = nullptr;
    QTimer *sensorTimer = nullptr;
    QTimer *refreshTimer = nullptr;
    QTimer *cameraTimer = nullptr;
    QTimer *previewTimer = nullptr;
    QTimer *sr04StaleTimer = nullptr;
    QTimer *sr501StaleTimer = nullptr;
    QByteArray receiveBuffer;
    quint32 nextRequestId = 0;
    quint32 pendingRequestId = 0;
    bool waitingSensorReply = false;
    SensorKind pendingSensor = SensorKind::Ap3216c;
    SensorKind refreshSensor = SensorKind::Ap3216c;
    bool waitingCameraReply = false;
    CameraRequest cameraRequest = CameraRequest::None;
    bool previewActive = false;
    bool previewStopRequested = false;
    bool reverseActive = false;
    bool sentinelActive = false;
    bool sentinelPreview = false;
    int sr501Level = -1;
    QElapsedTimer sentinelSampleClock;
    QElapsedTimer sentinelLowClock;
    QElapsedTimer reverseSampleClock;
    quint64 previewFrames = 0;
    quint32 pendingCameraRequestId = 0;
    quint32 incomingJpegId = 0;
    qsizetype incomingJpegBytes = 0;
    QPixmap photo;
    QByteArray photoJpeg;
    QFileDialog *saveDialog = nullptr;
    QStackedWidget *pageStack = nullptr;
    QPushButton *homePageButton = nullptr;
    QPushButton *sensorPageButton = nullptr;
    QPushButton *cameraPageButton = nullptr;
    QPushButton *reversePageButton = nullptr;
    QPushButton *sentinelPageButton = nullptr;
    QPushButton *sr501Button = nullptr;
    QPushButton *sentinelButton = nullptr;
    QPushButton *sentinelSaveButton = nullptr;
    QLabel *sr501LevelLabel = nullptr;
    QLabel *sr501StatusLabel = nullptr;
    QLabel *sentinelStatusLabel = nullptr;
    QLabel *sentinelImageLabel = nullptr;

    QPushButton *connectButton = nullptr;
    QPushButton *pingButton = nullptr;
    QPushButton *sensorButton = nullptr;
    QPushButton *autoRefreshButton = nullptr;
    QPushButton *imuButton = nullptr;
    QPushButton *imuAutoRefreshButton = nullptr;
    QPushButton *dhtButton = nullptr;
    QPushButton *dhtAutoRefreshButton = nullptr;
    QPushButton *sr04Button = nullptr;
    QPushButton *sr04AutoRefreshButton = nullptr;
    QPushButton *reverseButton = nullptr;
    QPushButton *reverseSaveButton = nullptr;
    QPushButton *captureButton = nullptr;
    QPushButton *previewButton = nullptr;
    QPushButton *saveButton = nullptr;
    QPushButton *disconnectButton = nullptr;
    QLabel *statusLabel = nullptr;
    QLabel *replyLabel = nullptr;
    QLabel *irLabel = nullptr;
    QLabel *alsLabel = nullptr;
    QLabel *psLabel = nullptr;
    QLabel *gyroLabel = nullptr;
    QLabel *gyroUnitsLabel = nullptr;
    QLabel *accelLabel = nullptr;
    QLabel *accelUnitsLabel = nullptr;
    QLabel *temperatureUnitsLabel = nullptr;
    QLabel *temperatureLabel = nullptr;
    QLabel *dhtHumidityLabel = nullptr;
    QLabel *dhtTemperatureLabel = nullptr;
    QLabel *dhtStatusLabel = nullptr;
    QLabel *sr04DistanceLabel = nullptr;
    QLabel *sr04PulseLabel = nullptr;
    QLabel *sr04StatusLabel = nullptr;
    QLabel *sr04HintLabel = nullptr;
    QLabel *reverseStatusLabel = nullptr;
    QLabel *reverseImageLabel = nullptr;
    QLabel *imageLabel = nullptr;
    QLabel *saveStatusLabel = nullptr;
};

#endif // MAINWINDOW_H
