#include "mainwindow.h"

#include <QBuffer>
#include <QButtonGroup>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QString>
#include <QTcpSocket>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <limits>

static constexpr quint32 maxJpegBytes = 4u * 1024u * 1024u;
static constexpr int cameraTimeoutMs = 8000;
static constexpr int sentinelQuietMs = 10000;
static constexpr double gyroCountsPerDps = 16.4;
static constexpr double accelCountsPerG = 2048.0;
// ICM-20608-G: DS-000081 第11页、RM-000030 第17页；采用标称零偏0。
static constexpr double temperatureCountsPerCelsius = 326.8;
static constexpr double temperatureReferenceCelsius = 25.0;

/* 协议中的数字只接受 ASCII 十进制数字，不接受符号或多余空白。 */
static bool parseUnsignedField(const QByteArray &field, quint32 *value)
{
    if (field.isEmpty())
        return false;
    for (char ch : field) {
        if (ch < '0' || ch > '9')
            return false;
    }

    bool ok = false;
    const auto parsed = field.toUInt(&ok, 10);
    if (!ok)
        return false;

    *value = parsed;
    return true;
}

static bool parseSignedField(const QByteArray &field, qint32 *value)
{
    if (field.isEmpty())
        return false;
    const qsizetype firstDigit = field[0] == '-' ? 1 : 0;
    if (firstDigit == field.size())
        return false;
    for (qsizetype i = firstDigit; i < field.size(); ++i) {
        if (field[i] < '0' || field[i] > '9')
            return false;
    }
    bool ok = false;
    const int parsed = field.toInt(&ok, 10);
    if (!ok)
        return false;
    *value = parsed;
    return true;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("传感器与摄像头上位机");
    resize(900, 700);

    socket = new QTcpSocket(this);
    sensorTimer = new QTimer(this);
    sensorTimer->setObjectName("sensorTimer");
    sensorTimer->setSingleShot(true);
    refreshTimer = new QTimer(this);
    refreshTimer->setObjectName("refreshTimer");
    refreshTimer->setTimerType(Qt::PreciseTimer);
    refreshTimer->setInterval(1000);
    cameraTimer = new QTimer(this);
    cameraTimer->setObjectName("cameraTimer");
    cameraTimer->setSingleShot(true);
    previewTimer = new QTimer(this);
    previewTimer->setObjectName("previewTimer");
    previewTimer->setSingleShot(true);
    previewTimer->setInterval(100);
    sr04StaleTimer = new QTimer(this);
    sr04StaleTimer->setObjectName("sr04StaleTimer");
    sr04StaleTimer->setSingleShot(true);
    sr04StaleTimer->setInterval(2000);
    sr501StaleTimer = new QTimer(this);
    sr501StaleTimer->setObjectName("sr501StaleTimer");
    sr501StaleTimer->setSingleShot(true);
    sr501StaleTimer->setInterval(2000);
    sentinelCountdownTimer = new QTimer(this);
    sentinelCountdownTimer->setTimerType(Qt::PreciseTimer);
    sentinelCountdownTimer->setInterval(100);
    socket->setReadBufferSize(maxJpegBytes + 128);

    auto *central = new QWidget(this);
    setCentralWidget(central);
    auto *layout = new QVBoxLayout(central);

    auto *targetLabel = new QLabel("教学目标地址：127.0.0.1:19000", central);
    connectButton = new QPushButton("连接", central);
    pingButton = new QPushButton("发送 PING", central);
    sensorButton = new QPushButton("读取 AP3216C", central);
    autoRefreshButton = new QPushButton("开始自动刷新", central);
    imuButton = new QPushButton("读取 ICM20608", central);
    imuButton->setObjectName("imuButton");
    imuAutoRefreshButton = new QPushButton("开始 ICM 自动刷新", central);
    imuAutoRefreshButton->setObjectName("imuAutoRefreshButton");
    dhtButton = new QPushButton("读取温湿度", central);
    dhtButton->setObjectName("dhtButton");
    dhtAutoRefreshButton = new QPushButton("开始温湿度自动刷新", central);
    dhtAutoRefreshButton->setObjectName("dhtAutoRefreshButton");
    sr04Button = new QPushButton("读取距离", central);
    sr04Button->setObjectName("sr04Button");
    sr04AutoRefreshButton = new QPushButton("开始测距刷新", central);
    sr04AutoRefreshButton->setObjectName("sr04AutoRefreshButton");
    reverseButton = new QPushButton("开始倒车监测", central);
    reverseButton->setObjectName("reverseButton");
    reverseSaveButton = new QPushButton("保存当前画面", central);
    reverseSaveButton->setObjectName("reverseSaveButton");
    sr501Button = new QPushButton("读取活动信号", central);
    sr501Button->setObjectName("sr501Button");
    sentinelButton = new QPushButton("开始哨兵监测", central);
    sentinelButton->setObjectName("sentinelButton");
    sentinelSaveButton = new QPushButton("保存当前画面", central);
    sentinelSaveButton->setObjectName("sentinelSaveButton");
    sr501LevelLabel = new QLabel(central);
    sr501LevelLabel->setObjectName("sr501LevelLabel");
    sr501StatusLabel = new QLabel(central);
    sr501StatusLabel->setObjectName("sr501StatusLabel");
    sentinelStatusLabel = new QLabel("监测已停止", central);
    sentinelStatusLabel->setObjectName("sentinelStatusLabel");
    sentinelStatusLabel->setWordWrap(true);
    sentinelImageLabel = new QLabel(central);
    sentinelImageLabel->setObjectName("sentinelImage");
    sentinelImageLabel->setAlignment(Qt::AlignCenter);
    sentinelImageLabel->setMinimumSize(320, 240);
    sentinelImageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    sentinelImageLabel->setFrameShape(QFrame::StyledPanel);
    captureButton = new QPushButton("拍照", central);
    captureButton->setObjectName("captureButton");
    previewButton = new QPushButton("开始预览", central);
    previewButton->setObjectName("previewButton");
    saveButton = new QPushButton("保存当前画面", central);
    saveButton->setObjectName("saveButton");
    disconnectButton = new QPushButton("断开", central);
    statusLabel = new QLabel("尚未连接", central);
    replyLabel = new QLabel("尚未收到回复", central);
    replyLabel->setObjectName("replyLabel");
    saveStatusLabel = new QLabel("尚未保存图片", central);
    saveStatusLabel->setObjectName("saveStatusLabel");
    saveStatusLabel->setTextFormat(Qt::PlainText);
    saveStatusLabel->setWordWrap(true);
    saveStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    irLabel = new QLabel(central);
    alsLabel = new QLabel(central);
    psLabel = new QLabel(central);
    gyroLabel = new QLabel(central);
    gyroLabel->setObjectName("gyroLabel");
    gyroUnitsLabel = new QLabel(central);
    gyroUnitsLabel->setObjectName("gyroUnitsLabel");
    accelLabel = new QLabel(central);
    accelLabel->setObjectName("accelLabel");
    accelUnitsLabel = new QLabel(central);
    accelUnitsLabel->setObjectName("accelUnitsLabel");
    temperatureUnitsLabel = new QLabel(central);
    temperatureUnitsLabel->setObjectName("temperatureUnitsLabel");
    temperatureUnitsLabel->setWordWrap(true);
    temperatureLabel = new QLabel(central);
    temperatureLabel->setObjectName("temperatureLabel");
    dhtHumidityLabel = new QLabel(central);
    dhtHumidityLabel->setObjectName("dhtHumidityLabel");
    dhtTemperatureLabel = new QLabel(central);
    dhtTemperatureLabel->setObjectName("dhtTemperatureLabel");
    dhtStatusLabel = new QLabel(central);
    dhtStatusLabel->setObjectName("dhtStatusLabel");
    sr04DistanceLabel = new QLabel(central);
    sr04DistanceLabel->setObjectName("sr04DistanceLabel");
    auto distanceFont = sr04DistanceLabel->font();
    distanceFont.setPointSize(distanceFont.pointSize() + 7);
    distanceFont.setBold(true);
    sr04DistanceLabel->setFont(distanceFont);
    sr04PulseLabel = new QLabel(central);
    sr04PulseLabel->setObjectName("sr04PulseLabel");
    sr04StatusLabel = new QLabel(central);
    sr04StatusLabel->setObjectName("sr04StatusLabel");
    sr04StatusLabel->setWordWrap(true);
    sr04HintLabel = new QLabel(central);
    sr04HintLabel->setObjectName("sr04HintLabel");
    reverseStatusLabel = new QLabel("监测已停止", central);
    reverseStatusLabel->setObjectName("reverseStatusLabel");
    reverseImageLabel = new QLabel(central);
    reverseImageLabel->setObjectName("reverseImage");
    reverseImageLabel->setAlignment(Qt::AlignCenter);
    reverseImageLabel->setMinimumSize(320, 240);
    reverseImageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    reverseImageLabel->setFrameShape(QFrame::StyledPanel);
    gyroLabel->setWordWrap(true);
    gyroUnitsLabel->setWordWrap(true);
    accelLabel->setWordWrap(true);
    accelUnitsLabel->setWordWrap(true);
    imageLabel = new QLabel(central);
    imageLabel->setObjectName("cameraImage");
    imageLabel->setAlignment(Qt::AlignCenter);
    imageLabel->setMinimumSize(320, 240);
    imageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    imageLabel->setFrameShape(QFrame::StyledPanel);
    statusLabel->setWordWrap(true);
    replyLabel->setWordWrap(true);
    clearSensorValues();
    clearPhoto();

    auto *connectionLayout = new QHBoxLayout;
    connectionLayout->addWidget(connectButton);
    connectionLayout->addWidget(pingButton);
    connectionLayout->addWidget(disconnectButton);
    connectionLayout->addStretch();
    layout->addWidget(targetLabel);
    layout->addLayout(connectionLayout);
    layout->addWidget(statusLabel);

    auto *navigationLayout = new QHBoxLayout;
    homePageButton = new QPushButton("首页", central);
    homePageButton->setObjectName("homePageButton");
    sensorPageButton = new QPushButton("板载设备", central);
    sensorPageButton->setObjectName("sensorPageButton");
    cameraPageButton = new QPushButton("摄像头", central);
    cameraPageButton->setObjectName("cameraPageButton");
    reversePageButton = new QPushButton("倒车测距", central);
    reversePageButton->setObjectName("reversePageButton");
    sentinelPageButton = new QPushButton("哨兵监测", central);
    sentinelPageButton->setObjectName("sentinelPageButton");
    auto *navigation = new QButtonGroup(this);
    for (auto *button : {homePageButton, sensorPageButton, cameraPageButton, reversePageButton, sentinelPageButton}) {
        button->setCheckable(true);
        navigation->addButton(button);
        navigationLayout->addWidget(button);
    }
    layout->addLayout(navigationLayout);
    pageStack = new QStackedWidget(central);
    pageStack->setObjectName("pageStack");

    auto *homePage = new QWidget(pageStack);
    homePage->setObjectName("homePage");
    auto *homeLayout = new QVBoxLayout(homePage);
    auto *title = new QLabel("车载系统控制台", homePage);
    auto titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 5);
    titleFont.setBold(true);
    title->setFont(titleFont);
    auto *clockLabel = new QLabel(homePage);
    clockLabel->setObjectName("clockLabel");
    const auto updateClock = [clockLabel]() {
        clockLabel->setText("电脑时间：" + QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    };
    auto *clockTimer = new QTimer(this);
    clockTimer->setObjectName("clockTimer");
    connect(clockTimer, &QTimer::timeout, this, updateClock);
    updateClock();
    clockTimer->start(1000);
    auto *dhtCard = new QGroupBox("环境温湿度（DHT11）", homePage);
    auto *dhtLayout = new QVBoxLayout(dhtCard);
    dhtLayout->addWidget(dhtTemperatureLabel);
    dhtLayout->addWidget(dhtHumidityLabel);
    dhtLayout->addWidget(dhtStatusLabel);
    auto *dhtActions = new QHBoxLayout;
    dhtActions->addWidget(dhtButton);
    dhtActions->addWidget(dhtAutoRefreshButton);
    dhtActions->addStretch();
    dhtLayout->addLayout(dhtActions);
    dhtLayout->addWidget(new QLabel("自动刷新间隔：3 秒", dhtCard));
    auto *sensorCard = new QGroupBox("板载设备", homePage);
    auto *sensorCardLayout = new QVBoxLayout(sensorCard);
    sensorCardLayout->addWidget(new QLabel("查看 AP3216C 的红外、光照和接近原始计数", sensorCard));
    auto *sensorShortcut = new QPushButton("查看设备", sensorCard);
    sensorShortcut->setObjectName("homeSensorButton");
    sensorCardLayout->addWidget(sensorShortcut);
    auto *cameraCard = new QGroupBox("摄像头", homePage);
    auto *cameraCardLayout = new QVBoxLayout(cameraCard);
    cameraCardLayout->addWidget(new QLabel("实时预览、拍照与保存当前画面", cameraCard));
    auto *cameraShortcut = new QPushButton("打开摄像头", cameraCard);
    cameraShortcut->setObjectName("homeCameraButton");
    cameraCardLayout->addWidget(cameraShortcut);
    auto *reverseShortcut = new QPushButton("打开倒车测距", homePage);
    reverseShortcut->setObjectName("homeReverseButton");
    auto *sentinelShortcut = new QPushButton("打开哨兵监测", homePage);
    sentinelShortcut->setObjectName("homeSentinelButton");
    homeLayout->addWidget(title);
    homeLayout->addWidget(clockLabel);
    homeLayout->addSpacing(16);
    homeLayout->addWidget(dhtCard);
    homeLayout->addWidget(sensorCard);
    homeLayout->addWidget(cameraCard);
    homeLayout->addWidget(reverseShortcut);
    homeLayout->addWidget(sentinelShortcut);
    homeLayout->addStretch();
    pageStack->addWidget(homePage);

    auto *sensorPage = new QWidget(pageStack);
    sensorPage->setObjectName("sensorPage");
    auto *sensorLayout = new QVBoxLayout(sensorPage);
    auto *sensorActions = new QHBoxLayout;
    sensorActions->addWidget(sensorButton);
    sensorActions->addWidget(autoRefreshButton);
    sensorActions->addStretch();
    auto *sampleGroup = new QGroupBox("AP3216C", sensorPage);
    auto *sampleLayout = new QVBoxLayout(sampleGroup);
    sampleLayout->addWidget(irLabel);
    sampleLayout->addWidget(alsLabel);
    sampleLayout->addWidget(psLabel);
    sensorLayout->addLayout(sensorActions);
    sensorLayout->addWidget(sampleGroup);
    sensorLayout->addWidget(new QLabel("IR、ALS、PS 为原始计数，用于观察变化。", sensorPage));
    auto *imuGroup = new QGroupBox("ICM20608", sensorPage);
    auto *imuLayout = new QVBoxLayout(imuGroup);
    auto *imuActions = new QHBoxLayout;
    imuActions->addWidget(imuButton);
    imuActions->addWidget(imuAutoRefreshButton);
    imuActions->addStretch();
    imuLayout->addLayout(imuActions);
    imuLayout->addWidget(gyroUnitsLabel);
    imuLayout->addWidget(gyroLabel);
    imuLayout->addWidget(accelUnitsLabel);
    imuLayout->addWidget(accelLabel);
    imuLayout->addWidget(temperatureUnitsLabel);
    imuLayout->addWidget(temperatureLabel);
    imuLayout->addWidget(new QLabel("量程：角速度 ±2000°/s，加速度 ±16g", imuGroup));
    sensorLayout->addWidget(imuGroup);
    sensorLayout->addStretch();
    pageStack->addWidget(sensorPage);

    auto *cameraPage = new QWidget(pageStack);
    cameraPage->setObjectName("cameraPage");
    auto *cameraLayout = new QVBoxLayout(cameraPage);
    auto *cameraActions = new QHBoxLayout;
    cameraActions->addWidget(captureButton);
    cameraActions->addWidget(previewButton);
    cameraActions->addWidget(saveButton);
    cameraActions->addStretch();
    cameraLayout->addLayout(cameraActions);
    cameraLayout->addWidget(saveStatusLabel);
    cameraLayout->addWidget(imageLabel, 1);
    pageStack->addWidget(cameraPage);

    auto *reversePage = new QWidget(pageStack);
    reversePage->setObjectName("reversePage");
    auto *reverseLayout = new QVBoxLayout(reversePage);
    auto *distanceGroup = new QGroupBox("超声波测距（HC-SR04-RC）", reversePage);
    auto *distanceLayout = new QVBoxLayout(distanceGroup);
    distanceLayout->addWidget(sr04DistanceLabel);
    distanceLayout->addWidget(sr04HintLabel);
    distanceLayout->addWidget(sr04PulseLabel);
    distanceLayout->addWidget(sr04StatusLabel);
    auto *distanceActions = new QHBoxLayout;
    distanceActions->addWidget(sr04Button);
    distanceActions->addWidget(sr04AutoRefreshButton);
    distanceActions->addStretch();
    distanceLayout->addLayout(distanceActions);
    distanceLayout->addWidget(new QLabel("距离提示：≤30 cm 较近；30～100 cm 注意；>100 cm 较远", distanceGroup));
    reverseLayout->addWidget(distanceGroup);
    auto *reverseActions = new QHBoxLayout;
    reverseActions->addWidget(reverseButton);
    reverseActions->addWidget(reverseSaveButton);
    reverseActions->addStretch();
    reverseLayout->addLayout(reverseActions);
    reverseLayout->addWidget(reverseStatusLabel);
    reverseLayout->addWidget(reverseImageLabel, 1);
    pageStack->addWidget(reversePage);

    auto *sentinelPage = new QWidget(pageStack);
    sentinelPage->setObjectName("sentinelPage");
    auto *sentinelLayout = new QVBoxLayout(sentinelPage);
    auto *activityGroup = new QGroupBox("人体活动信号（HC-SR501）", sentinelPage);
    auto *activityLayout = new QVBoxLayout(activityGroup);
    activityLayout->addWidget(sr501LevelLabel);
    activityLayout->addWidget(sr501StatusLabel);
    auto *sentinelActions = new QHBoxLayout;
    sentinelActions->addWidget(sr501Button);
    sentinelActions->addWidget(sentinelButton);
    sentinelActions->addWidget(sentinelSaveButton);
    sentinelActions->addStretch();
    activityLayout->addLayout(sentinelActions);
    auto *activityNote = new QLabel("上电预热约60秒。高电平触发画面，连续低电平10秒后关闭画面并继续监测。低电平不代表房间绝对无人。", activityGroup);
    activityNote->setWordWrap(true);
    activityLayout->addWidget(activityNote);
    sentinelLayout->addWidget(activityGroup);
    sentinelLayout->addWidget(sentinelStatusLabel);
    sentinelLayout->addWidget(sentinelImageLabel, 1);
    pageStack->addWidget(sentinelPage);
    layout->addWidget(pageStack, 1);
    layout->addWidget(replyLabel);

    connect(homePageButton, &QPushButton::clicked, this, [this]() { switchPage(Page::Home); });
    connect(sensorPageButton, &QPushButton::clicked, this, [this]() { switchPage(Page::Sensors); });
    connect(cameraPageButton, &QPushButton::clicked, this, [this]() { switchPage(Page::Camera); });
    connect(reversePageButton, &QPushButton::clicked, this, [this]() { switchPage(Page::Reverse); });
    connect(sentinelPageButton, &QPushButton::clicked, this, [this]() { switchPage(Page::Sentinel); });
    connect(sensorShortcut, &QPushButton::clicked, this, [this]() { switchPage(Page::Sensors); });
    connect(cameraShortcut, &QPushButton::clicked, this, [this]() { switchPage(Page::Camera); });
    connect(reverseShortcut, &QPushButton::clicked, this, [this]() { switchPage(Page::Reverse); });
    connect(sentinelShortcut, &QPushButton::clicked, this, [this]() { switchPage(Page::Sentinel); });
    connect(sr501Button, &QPushButton::clicked, this, [this]() { requestSensor(SensorKind::Sr501); });
    connect(sentinelButton, &QPushButton::clicked, this, &MainWindow::toggleSentinelMonitoring);
    connect(sentinelSaveButton, &QPushButton::clicked, this, &MainWindow::savePhoto);

    // 构造时登记回调。这里没有发起网络连接。
    connect(connectButton, &QPushButton::clicked,
            this, &MainWindow::connectToServer);
    connect(pingButton, &QPushButton::clicked,
            this, &MainWindow::sendPing);
    connect(sensorButton, &QPushButton::clicked,
            this, &MainWindow::readSensor);
    connect(imuButton, &QPushButton::clicked,
            this, &MainWindow::readImu);
    connect(imuAutoRefreshButton, &QPushButton::clicked, this, [this]() {
        toggleSensorRefresh(SensorKind::Icm20608);
    });
    connect(dhtButton, &QPushButton::clicked, this, [this]() {
        requestSensor(SensorKind::Dht11);
    });
    connect(dhtAutoRefreshButton, &QPushButton::clicked, this, [this]() {
        toggleSensorRefresh(SensorKind::Dht11);
    });
    connect(sr04Button, &QPushButton::clicked, this, [this]() {
        requestSensor(SensorKind::Sr04);
    });
    connect(sr04AutoRefreshButton, &QPushButton::clicked, this, [this]() {
        toggleSensorRefresh(SensorKind::Sr04);
    });
    connect(reverseButton, &QPushButton::clicked, this, &MainWindow::toggleReverseMonitoring);
    connect(reverseSaveButton, &QPushButton::clicked, this, &MainWindow::savePhoto);
    connect(captureButton, &QPushButton::clicked,
            this, &MainWindow::capturePhoto);
    connect(previewButton, &QPushButton::clicked,
            this, &MainWindow::togglePreview);
    connect(saveButton, &QPushButton::clicked,
            this, &MainWindow::savePhoto);
    connect(previewTimer, &QTimer::timeout,
            this, &MainWindow::requestPreviewFrame);
    connect(autoRefreshButton, &QPushButton::clicked,
            this, &MainWindow::toggleAutoRefresh);
    connect(refreshTimer, &QTimer::timeout, this, [this]() { requestSensor(refreshSensor); });
    connect(sentinelCountdownTimer, &QTimer::timeout,
            this, &MainWindow::updateSentinelCountdown);
    connect(disconnectButton, &QPushButton::clicked, this, [this]() {
        stopAutoRefresh();
        resetPreview();
        finishSensorRequest();
        finishCameraRequest();
        resetReceiveState();
        clearSensorValues();
        clearPhoto();
        replyLabel->setText("已结束本次等待");
        statusLabel->setText("正在断开……");
        socket->disconnectFromHost();
        updateButtons();
    });

    connect(socket, &QTcpSocket::connected, this, [this]() {
        statusLabel->setText("TCP 已连接，可以发送命令");
        updateButtons();
    });
    connect(socket, &QTcpSocket::readyRead,
            this, &MainWindow::readSocketData);
    connect(socket, &QTcpSocket::disconnected, this, [this]() {
        stopAutoRefresh();
        resetPreview();
        finishSensorRequest();
        finishCameraRequest();
        resetReceiveState();
        clearSensorValues();
        clearPhoto();
        statusLabel->setText("连接已断开");
        replyLabel->setText("当前没有有效采样结果");
        updateButtons();
    });
    connect(socket, &QTcpSocket::errorOccurred,
            this, [this](QAbstractSocket::SocketError) {
        stopAutoRefresh();
        resetPreview();
        finishSensorRequest();
        finishCameraRequest();
        resetReceiveState();
        clearSensorValues();
        clearPhoto();
        statusLabel->setText(socket->errorString());
        replyLabel->setText("通信失败，已结束等待");
        updateButtons();
    });
    connect(sensorTimer, &QTimer::timeout, this, [this]() {
        if (!waitingSensorReply)
            return;

        failSensorRequest("读取超时：3 秒内未收到有效回复，可重新读取");
    });
    connect(sr04StaleTimer, &QTimer::timeout, this, [this]() {
        if (reverseActive && !previewStopRequested) {
            clearSr04Values();
            sr04StatusLabel->setText("距离已过期，等待下一次有效测距");
        }
    });
    connect(sr501StaleTimer, &QTimer::timeout, this, [this]() {
        if (sentinelActive) {
            clearSr501Values();
            sr501StatusLabel->setText("信号已过期，等待下一次有效采样");
            sentinelStatusLabel->setText("信号未知，继续监测");
        }
    });
    connect(cameraTimer, &QTimer::timeout, this, [this]() {
        if (!waitingCameraReply && incomingJpegBytes == 0)
            return;
        // 图像可能只到一半；关闭连接后才能重新建立明确的消息边界。
        socket->abort();
        replyLabel->setText("摄像头请求或图像接收超时，连接已关闭，请重新连接");
    });

    updateButtons();
}

MainWindow::~MainWindow()
{
    // 在成员和界面析构前停止异步来源，避免socket析构时回调已销毁的数据。
    sensorTimer->stop();
    refreshTimer->stop();
    cameraTimer->stop();
    previewTimer->stop();
    sr04StaleTimer->stop();
    sr501StaleTimer->stop();
    sentinelCountdownTimer->stop();
    disconnect(socket, nullptr, this, nullptr);
    socket->abort();
}

void MainWindow::switchPage(Page page)
{
    const int previous = pageStack->currentIndex();
    if (previous == static_cast<int>(page))
        return;
    if (previous == static_cast<int>(Page::Sentinel))
        stopSentinelMonitoring();
    if (previous == static_cast<int>(Page::Home) || previous == static_cast<int>(Page::Sensors) ||
        previous == static_cast<int>(Page::Reverse))
        stopAutoRefresh(); // 已发出的采样仍由原回复/超时逻辑收尾。
    if ((previous == static_cast<int>(Page::Camera) || previous == static_cast<int>(Page::Reverse)) &&
        previewActive && !previewStopRequested)
        togglePreview(); // 收完当前请求后 STOP；切页本身不阻塞界面。
    pageStack->setCurrentIndex(static_cast<int>(page));
    QTimer::singleShot(0, this, &MainWindow::updatePhotoDisplay);
    updateButtons();
}

void MainWindow::connectToServer()
{
    if (socket->state() != QAbstractSocket::UnconnectedState)
        return;

    stopAutoRefresh();
    resetPreview();
    finishSensorRequest();
    finishCameraRequest();
    resetReceiveState();
    clearSensorValues();
    clearPhoto();
    replyLabel->setText("尚未收到本次连接的回复");
    statusLabel->setText("正在连接……");

    // 默认通过 USB/ADB 转发连接；Wi-Fi 直连时修改目标地址及上方显示。
    socket->connectToHost("127.0.0.1", 19000);
    updateButtons();
}

void MainWindow::sendPing()
{
    if (socket->state() != QAbstractSocket::ConnectedState || waitingSensorReply ||
        waitingCameraReply || incomingJpegBytes > 0 || previewActive || sentinelActive)
        return;

    replyLabel->setText("等待 PONG……");
    if (socket->write("PING\n", 5) != 5) {
        socket->abort();
        replyLabel->setText("PING 提交失败，连接已关闭");
    }
}

void MainWindow::readSensor()
{
    requestSensor(SensorKind::Ap3216c);
}

void MainWindow::readImu()
{
    requestSensor(SensorKind::Icm20608);
}

const char *MainWindow::sensorName(SensorKind kind)
{
    switch (kind) {
    case SensorKind::Ap3216c: return "AP3216C";
    case SensorKind::Icm20608: return "ICM20608";
    case SensorKind::Dht11: return "DHT11";
    case SensorKind::Sr04: return "SR04";
    case SensorKind::Sr501: return "SR501";
    }
    return "";
}

void MainWindow::requestSensor(SensorKind kind)
{
    if (kind == SensorKind::Dht11 &&
        pageStack->currentIndex() != static_cast<int>(Page::Home))
        return;
    if (kind == SensorKind::Sr04 &&
        pageStack->currentIndex() != static_cast<int>(Page::Reverse))
        return;
    if (kind == SensorKind::Sr501 &&
        pageStack->currentIndex() != static_cast<int>(Page::Sentinel))
        return;
    const bool reverseSample = reverseActive && !previewStopRequested && kind == SensorKind::Sr04;
    const bool sentinelSample = sentinelActive && kind == SensorKind::Sr501;
    if (socket->state() != QAbstractSocket::ConnectedState || waitingSensorReply ||
        waitingCameraReply || incomingJpegBytes > 0 || (previewActive && !reverseSample && !sentinelSample) ||
        (refreshTimer->isActive() && refreshSensor != kind))
        return;

    // unsigned 递增允许回绕；0 留作“没有等待中的请求”。
    ++nextRequestId;
    if (nextRequestId == 0)
        ++nextRequestId;
    pendingRequestId = nextRequestId;
    pendingSensor = kind;
    waitingSensorReply = true;
    // 等待新结果时保留最近一次成功数值；失败或断开时再清空。
    replyLabel->setText(QString("等待请求 %1 的采样结果……").arg(pendingRequestId));
    if (kind == SensorKind::Dht11)
        dhtStatusLabel->setText("正在读取……");
    if (reverseSample)
        reverseSampleClock.start();
    if (sentinelSample)
        sentinelSampleClock.start();
    sensorTimer->start(3000);
    updateButtons();

    const QByteArray command = QByteArray("READ_") + sensorName(kind) + ' '
        + QByteArray::number(pendingRequestId) + '\n';
    // write 成功表示进入发送缓冲；收到匹配编号的回复才算请求完成。
    if (socket->write(command) != command.size()) {
        stopAutoRefresh();
        finishSensorRequest();
        socket->abort();
        replyLabel->setText("读取命令提交失败，连接已关闭");
    }
}

void MainWindow::capturePhoto()
{
    if (socket->state() != QAbstractSocket::ConnectedState || waitingSensorReply ||
        waitingCameraReply || incomingJpegBytes > 0 || refreshTimer->isActive() || previewActive || sentinelActive)
        return;

    sendCameraRequest(CameraRequest::Photo);
}

void MainWindow::savePhoto()
{
    if (photoJpeg.isEmpty() || saveDialog != nullptr)
        return;

    // QByteArray 隐式共享；后续预览更新不会改变点击时保留的这一帧。
    const QByteArray snapshot = photoJpeg;
    QString directory = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (directory.isEmpty())
        directory = QDir::homePath();
    const QString name = "vehicle_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz")
                         + ".jpg";
    auto *dialog = new QFileDialog(this, "保存当前画面");
    dialog->setObjectName("savePhotoDialog");
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setFileMode(QFileDialog::AnyFile);
    dialog->setNameFilter("JPEG 图片 (*.jpg *.jpeg)");
    dialog->setDefaultSuffix("jpg");
    dialog->selectFile(QDir(directory).filePath(name));
    dialog->setWindowModality(Qt::WindowModal);
    saveDialog = dialog;
    connect(dialog, &QFileDialog::fileSelected, this, [this, snapshot](const QString &path) {
        const QString suffix = QFileInfo(path).suffix();
        if (suffix.compare("jpg", Qt::CaseInsensitive) != 0 &&
            suffix.compare("jpeg", Qt::CaseInsensitive) != 0) {
            saveStatusLabel->setText("保存失败：请使用 .jpg 或 .jpeg 文件名");
            return;
        }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(snapshot) != snapshot.size() ||
            !file.commit()) {
            saveStatusLabel->setText("保存失败：" + file.errorString());
            return;
        }
        saveStatusLabel->setText("已保存：" + QDir::toNativeSeparators(path));
    });
    connect(dialog, &QDialog::finished, this, [this, dialog](int) {
        saveDialog = nullptr;
        dialog->deleteLater();
        updateButtons();
    });
    // 异步打开选择框，Qt 事件循环继续接收和显示预览。
    dialog->open();
    updateButtons();
}

void MainWindow::togglePreview()
{
    if (previewActive) {
        // 先停止后续取帧；正在传输的帧收完后再发送停止命令。
        previewStopRequested = true;
        previewTimer->stop();
        sentinelCountdownTimer->stop();
        sr04StaleTimer->stop();
        if (reverseActive)
            reverseStatusLabel->setText("正在停止倒车监测……");
        replyLabel->setText("正在停止预览，等待当前请求结束……");
        updateButtons();
        requestPreviewStop();
        return;
    }
    startPreview(false);
}

void MainWindow::toggleReverseMonitoring()
{
    if (reverseActive) {
        togglePreview();
        return;
    }
    if (pageStack->currentIndex() == static_cast<int>(Page::Reverse))
        startPreview(true);
}

void MainWindow::toggleSentinelMonitoring()
{
    if (sentinelActive) {
        stopSentinelMonitoring();
        return;
    }
    if (pageStack->currentIndex() != static_cast<int>(Page::Sentinel) ||
        socket->state() != QAbstractSocket::ConnectedState || waitingSensorReply ||
        waitingCameraReply || incomingJpegBytes > 0 || previewActive || refreshTimer->isActive())
        return;
    clearSr501Values();
    sentinelActive = true;
    sentinelSampleClock.invalidate();
    sentinelStatusLabel->setText("监测中，等待活动信号");
    refreshSensor = SensorKind::Sr501;
    refreshTimer->start(500);
    requestSensor(SensorKind::Sr501);
    updateButtons();
}

void MainWindow::stopSentinelMonitoring()
{
    sentinelActive = false;
    if (refreshTimer->isActive() && refreshSensor == SensorKind::Sr501)
        refreshTimer->stop();
    sr501StaleTimer->stop();
    sentinelCountdownTimer->stop();
    sentinelLowClock.invalidate();
    sentinelStatusLabel->setText("监测已停止");
    if (sentinelPreview && previewActive && !previewStopRequested) {
        togglePreview();
        sentinelStatusLabel->setText("正在关闭哨兵画面……");
    }
    updateButtons();
}

void MainWindow::resetSentinel()
{
    sentinelActive = false;
    sentinelSampleClock.invalidate();
    sentinelLowClock.invalidate();
    sentinelCountdownTimer->stop();
    sr501StaleTimer->stop();
    sentinelStatusLabel->setText("监测已停止");
}

void MainWindow::acceptSr501Level(unsigned int level)
{
    sr501Level = static_cast<int>(level);
    sr501LevelLabel->setText(level ? "活动信号：高电平（OUT=1）" : "活动信号：低电平（OUT=0）");
    sr501StatusLabel->setText("最近读取：" + QDateTime::currentDateTime().toString("HH:mm:ss.zzz"));
    if (!sentinelActive) {
        if (sentinelPreview && previewStopRequested)
            requestPreviewStop();
        return; // 离页或停止后收尾的旧请求不能重新打开摄像头。
    }
    sr501StaleTimer->start();
    if (level) {
        sentinelLowClock.invalidate();
        sentinelCountdownTimer->stop();
        if (!previewActive)
            startPreview(false, true);
        else
            sentinelStatusLabel->setText("活动信号为高，画面开启中");
    } else if (sentinelPreview && !previewStopRequested) {
        if (!sentinelLowClock.isValid()) {
            sentinelLowClock.start();
            sentinelCountdownTimer->start();
        }
        if (sentinelLowClock.elapsed() >= sentinelQuietMs) {
            togglePreview();
            sentinelStatusLabel->setText("连续低电平10秒，正在关闭画面；监测继续");
        } else {
            updateSentinelCountdown();
        }
    } else if (!previewActive) {
        sentinelStatusLabel->setText("监测中，等待活动信号");
    }
    if (sentinelPreview && previewActive) {
        if (previewStopRequested)
            requestPreviewStop();
        else if (!waitingCameraReply && !previewTimer->isActive())
            previewTimer->start();
    }
}

void MainWindow::updateSentinelCountdown()
{
    if (!sentinelActive || !sentinelPreview || !previewActive ||
        previewStopRequested || sr501Level != 0 || !sentinelLowClock.isValid()) {
        sentinelCountdownTimer->stop();
        return;
    }
    // 显示按经过时间刷新；关闭画面仍由有效低电平采样确认。
    const qint64 remainingMs = qMax<qint64>(0, sentinelQuietMs - sentinelLowClock.elapsed());
    const qint64 remaining = (remainingMs + 999) / 1000;
    sentinelStatusLabel->setText(QString("活动信号为低，继续保持画面约%1秒").arg(remaining));
}

void MainWindow::startPreview(bool reverse, bool sentinel)
{
    if (socket->state() != QAbstractSocket::ConnectedState || waitingSensorReply ||
        waitingCameraReply || incomingJpegBytes > 0 ||
        (refreshTimer->isActive() && !sentinel) || previewActive ||
        (sentinelActive && !sentinel))
        return;

    reverseActive = reverse;
    sentinelPreview = sentinel;
    reverseSampleClock.invalidate();
    if (reverse)
        reverseStatusLabel->setText("正在启动倒车监测……");
    if (sentinel)
        sentinelStatusLabel->setText("活动信号触发，正在打开画面……");
    previewActive = true;
    previewStopRequested = false;
    previewFrames = 0;
    sendCameraRequest(CameraRequest::PreviewStart);
}

void MainWindow::requestPreviewFrame()
{
    if (!previewActive || previewStopRequested)
        return;
    if (sentinelPreview && sentinelActive &&
        (!sentinelSampleClock.isValid() || sentinelSampleClock.elapsed() >= 500)) {
        requestSensor(SensorKind::Sr501);
        return;
    }
    if (reverseActive && (!reverseSampleClock.isValid() || reverseSampleClock.elapsed() >= 500)) {
        requestSensor(SensorKind::Sr04);
        return;
    }
    sendCameraRequest(CameraRequest::PreviewFrame);
}

void MainWindow::requestPreviewStop()
{
    if (!previewActive || !previewStopRequested)
        return;
    sendCameraRequest(CameraRequest::PreviewStop);
}

void MainWindow::resetPreview()
{
    previewTimer->stop();
    sr04StaleTimer->stop();
    if (reverseActive)
        reverseStatusLabel->setText("监测已停止");
    if (sentinelPreview)
        sentinelStatusLabel->setText(sentinelActive ? "监测中，等待活动信号" : "监测已停止");
    reverseActive = false;
    sentinelPreview = false;
    sentinelLowClock.invalidate();
    sentinelCountdownTimer->stop();
    reverseSampleClock.invalidate();
    previewActive = false;
    previewStopRequested = false;
    previewFrames = 0;
}

void MainWindow::sendCameraRequest(CameraRequest request)
{
    if (request == CameraRequest::None ||
        socket->state() != QAbstractSocket::ConnectedState || waitingSensorReply ||
        waitingCameraReply || incomingJpegBytes > 0)
        return;

    QByteArray name;
    switch (request) {
    case CameraRequest::Photo: name = "CAPTURE_JPEG"; break;
    case CameraRequest::PreviewStart: name = "PREVIEW_START"; break;
    case CameraRequest::PreviewFrame: name = "PREVIEW_FRAME"; break;
    case CameraRequest::PreviewStop: name = "PREVIEW_STOP"; break;
    case CameraRequest::None: return;
    }
    ++nextRequestId;
    if (nextRequestId == 0)
        ++nextRequestId;
    pendingCameraRequestId = nextRequestId;
    waitingCameraReply = true;
    cameraRequest = request;
    cameraTimer->start(cameraTimeoutMs);
    if (request == CameraRequest::Photo)
        replyLabel->setText(QString("等待拍照请求 %1……").arg(pendingCameraRequestId));
    else if (request == CameraRequest::PreviewStart)
        replyLabel->setText("正在启动预览……");
    else if (request == CameraRequest::PreviewStop)
        replyLabel->setText("正在停止预览，等待板端释放摄像头……");
    updateButtons();

    const QByteArray command = name + ' '
        + QByteArray::number(pendingCameraRequestId) + '\n';
    if (socket->write(command) != command.size()) {
        socket->abort();
        replyLabel->setText("摄像头命令提交失败，连接已关闭");
    }
}

void MainWindow::toggleAutoRefresh()
{
    toggleSensorRefresh(SensorKind::Ap3216c);
}

void MainWindow::toggleSensorRefresh(SensorKind kind)
{
    if (kind == SensorKind::Dht11 &&
        pageStack->currentIndex() != static_cast<int>(Page::Home))
        return;
    if (kind == SensorKind::Sr04 &&
        pageStack->currentIndex() != static_cast<int>(Page::Reverse))
        return;
    if (refreshTimer->isActive()) {
        if (refreshSensor == kind)
            stopAutoRefresh();
        return;
    }

    if (socket->state() != QAbstractSocket::ConnectedState || waitingSensorReply ||
        waitingCameraReply || incomingJpegBytes > 0 || previewActive)
        return;

    refreshSensor = kind;
    refreshTimer->start(kind == SensorKind::Dht11 ? 3000 : kind == SensorKind::Sr04 ? 500 : 1000);
    updateButtons();
    requestSensor(kind); // 立即读取一次，之后按该传感器的间隔触发。
}

void MainWindow::stopAutoRefresh()
{
    refreshTimer->stop();
    // 只停止未来的读取；已发出的请求继续由原来的超时/回复逻辑处理。
    updateButtons();
}

void MainWindow::readSocketData()
{
    receiveBuffer.append(socket->readAll());

    for (;;) {
        if (incomingJpegBytes > 0) {
            if (receiveBuffer.size() < incomingJpegBytes)
                return;
            const QByteArray jpeg = receiveBuffer.left(incomingJpegBytes);
            receiveBuffer.remove(0, incomingJpegBytes);
            const quint32 id = incomingJpegId;
            incomingJpegBytes = 0;
            incomingJpegId = 0;
            handleJpeg(id, jpeg);
            continue;
        }

        const auto pos = receiveBuffer.indexOf('\n');
        if (pos > 127 || (pos < 0 && receiveBuffer.size() > 127)) {
            socket->abort();
            replyLabel->setText("回复超过协议长度限制，连接已关闭");
            return;
        }
        if (pos < 0)
            return;

        const QByteArray line = receiveBuffer.left(pos);
        receiveBuffer.remove(0, pos + 1);
        handleReply(line);
        if (socket->state() != QAbstractSocket::ConnectedState)
            return;
    }
}

void MainWindow::handleReply(const QByteArray &line)
{
    if (line == "PONG") {
        replyLabel->setText("收到 PONG：命令已得到应答");
        return;
    }
    if (waitingCameraReply && line == "ERR UNKNOWN_COMMAND") {
        failCameraRequest(previewActive ? "板端服务尚不支持连续预览，请更新板端服务"
                                        : "板端服务尚不支持拍照，请更新板端服务");
        return;
    }
    if (waitingSensorReply && line == "ERR UNKNOWN_COMMAND") {
        failSensorRequest(QString("板端服务尚不支持 %1，请更新板端服务")
                              .arg(QString::fromLatin1(sensorName(pendingSensor))));
        return;
    }

    const auto fields = line.split(' ');
    if (fields[0] == "JPEG") {
        quint32 id = 0, size = 0;
        if (fields.size() != 3 || !parseUnsignedField(fields[1], &id) || id == 0 ||
            !parseUnsignedField(fields[2], &size) || size == 0 || size > maxJpegBytes) {
            socket->abort();
            replyLabel->setText("图像回复头或长度无效，连接已关闭");
            return;
        }
        // 即使编号过期，也先按长度消费载荷，才能找到下一条消息。
        incomingJpegId = id;
        incomingJpegBytes = size;
        if (!cameraTimer->isActive())
            cameraTimer->start(cameraTimeoutMs);
        updateButtons();
        return;
    }
    if (fields[0] != "AP3216C" && fields[0] != "ICM20608" && fields[0] != "DHT11" && fields[0] != "SR04" &&
        fields[0] != "SR501" &&
        fields[0] != "ERR" && fields[0] != "OK") {
        replyLabel->setText("收到无法识别的回复");
        return;
    }

    quint32 id = 0;
    if (fields.size() < 2 || !parseUnsignedField(fields[1], &id) || id == 0) {
        replyLabel->setText("收到缺少有效请求编号的回复");
        return;
    }

    if (waitingCameraReply && id == pendingCameraRequestId) {
        if (cameraRequest == CameraRequest::PreviewStart && fields.size() == 3 &&
            fields[0] == "OK" && fields[2] == "PREVIEW_STARTED") {
            finishCameraRequest();
            if (reverseActive && !previewStopRequested)
                reverseStatusLabel->setText("倒车监测中");
            if (sentinelPreview && !previewStopRequested)
                sentinelStatusLabel->setText("哨兵画面已开启");
            if (previewStopRequested)
                requestPreviewStop();
            else
                requestPreviewFrame();
            return;
        }
        if (cameraRequest == CameraRequest::PreviewStop && fields.size() == 3 &&
            fields[0] == "OK" && fields[2] == "PREVIEW_STOPPED") {
            const quint64 frames = previewFrames;
            resetPreview();
            finishCameraRequest();
            replyLabel->setText(QString("预览已停止，共显示 %1 帧").arg(frames));
            return;
        }
        if (previewActive) {
            failCameraRequest("预览请求失败或回复无效，连接已关闭，可检查板端日志后重连");
            return;
        }
        const bool captureFailed = fields[0] == "ERR" && fields.size() == 3 &&
                                   fields[2] == "CAMERA_CAPTURE";
        failCameraRequest(captureFailed ? "板端拍照失败，可检查摄像头后重试"
                                        : "拍照回复无效，可重新拍照");
        return;
    }

    // 超时后的旧回复，以及与当前请求无关的回复，都不更新采样结果。
    if (!waitingSensorReply || id != pendingRequestId)
        return;

    if (pendingSensor == SensorKind::Icm20608 && fields[0] == "ICM20608" && fields.size() == 9) {
        qint32 values[7];
        bool valid = true;
        for (int i = 0; i < 7; ++i)
            valid = parseSignedField(fields[i + 2], &values[i]) && valid;
        if (valid) {
            finishSensorRequest();
            gyroUnitsLabel->setText(QString("角速度（°/s）：x=%1，y=%2，z=%3")
                .arg(values[0] / gyroCountsPerDps, 0, 'f', 2)
                .arg(values[1] / gyroCountsPerDps, 0, 'f', 2)
                .arg(values[2] / gyroCountsPerDps, 0, 'f', 2));
            gyroLabel->setText(QString("角速度原始计数：gx=%1，gy=%2，gz=%3")
                                   .arg(values[0]).arg(values[1]).arg(values[2]));
            accelUnitsLabel->setText(QString("加速度（g）：x=%1，y=%2，z=%3")
                .arg(values[3] / accelCountsPerG, 0, 'f', 4)
                .arg(values[4] / accelCountsPerG, 0, 'f', 4)
                .arg(values[5] / accelCountsPerG, 0, 'f', 4));
            accelLabel->setText(QString("加速度原始计数：ax=%1，ay=%2，az=%3")
                                    .arg(values[3]).arg(values[4]).arg(values[5]));
            temperatureUnitsLabel->setText(QString("芯片温度（°C）：%1")
                .arg(values[6] / temperatureCountsPerCelsius + temperatureReferenceCelsius, 0, 'f', 2));
            temperatureLabel->setText(QString("温度原始计数：%1").arg(values[6]));
            replyLabel->setText(QString("ICM20608 请求 %1 完成").arg(id));
            return;
        }
    } else if (pendingSensor == SensorKind::Ap3216c && fields[0] == "AP3216C" && fields.size() == 5) {
        quint32 ir = 0, als = 0, ps = 0;
        const quint32 maxCount = std::numeric_limits<qint32>::max();
        if (parseUnsignedField(fields[2], &ir) && ir <= maxCount &&
            parseUnsignedField(fields[3], &als) && als <= maxCount &&
            parseUnsignedField(fields[4], &ps) && ps <= maxCount) {
            finishSensorRequest();
            irLabel->setText(QString("IR 原始计数：%1").arg(ir));
            alsLabel->setText(QString("ALS 原始计数：%1").arg(als));
            psLabel->setText(QString("PS 原始计数：%1").arg(ps));
            replyLabel->setText(QString("请求 %1 完成").arg(id));
            return;
        }
    } else if (pendingSensor == SensorKind::Dht11 && fields[0] == "DHT11" && fields.size() == 4) {
        quint32 humidity = 0, temperature = 0;
        if (parseUnsignedField(fields[2], &humidity) && humidity <= 1000 &&
            parseUnsignedField(fields[3], &temperature) && temperature <= 500) {
            finishSensorRequest();
            dhtHumidityLabel->setText(QString("相对湿度（%RH）：%1").arg(humidity / 10.0, 0, 'f', 1));
            dhtTemperatureLabel->setText(QString("环境温度（°C）：%1").arg(temperature / 10.0, 0, 'f', 1));
            dhtStatusLabel->setText("最近更新：" + QDateTime::currentDateTime().toString("HH:mm:ss"));
            replyLabel->setText(QString("DHT11 请求 %1 完成").arg(id));
            return;
        }
    } else if (pendingSensor == SensorKind::Sr04 && fields[0] == "SR04" && fields.size() == 4) {
        quint32 distance = 0, pulse = 0;
        if (parseUnsignedField(fields[2], &distance) && distance >= 20 && distance <= 4000 &&
            parseUnsignedField(fields[3], &pulse) && pulse > 0 && pulse <= 40000) {
            const quint32 expected = (pulse * 343u + 1000u) / 2000u;
            if (distance + 1 < expected || expected + 1 < distance) {
                failSensorRequest("SR04 距离与回波脉宽不一致");
                return;
            }
            finishSensorRequest();
            sr04DistanceLabel->setText(QString("障碍物距离：%1 cm（%2 mm）")
                                           .arg(distance / 10.0, 0, 'f', 1).arg(distance));
            sr04PulseLabel->setText(QString("回波高电平：%1 μs").arg(pulse));
            sr04StatusLabel->setText("最近测距：" + QDateTime::currentDateTime().toString("HH:mm:ss.zzz"));
            sr04HintLabel->setText(distance <= 300 ? "距离提示：较近" : distance <= 1000 ? "距离提示：注意" : "距离提示：较远");
            sr04HintLabel->setStyleSheet(distance <= 300 ? "color: #b42318; font-weight: bold;"
                                         : distance <= 1000 ? "color: #8a5700; font-weight: bold;"
                                                            : "color: #21633d; font-weight: bold;");
            replyLabel->setText(QString("SR04 请求 %1 完成").arg(id));
            if (reverseActive) {
                if (previewStopRequested) {
                    requestPreviewStop();
                } else {
                    sr04StaleTimer->start();
                    sendCameraRequest(CameraRequest::PreviewFrame);
                }
            }
            return;
        }
    } else if (pendingSensor == SensorKind::Sr501 && fields[0] == "SR501" && fields.size() == 3) {
        quint32 level = 0;
        if (parseUnsignedField(fields[2], &level) && level <= 1) {
            finishSensorRequest();
            acceptSr501Level(level);
            replyLabel->setText(QString("SR501 请求 %1 完成").arg(id));
            return;
        }
    } else if (fields[0] == "ERR" && fields.size() == 3 &&
               fields[2] == QByteArray(sensorName(pendingSensor)) + "_READ") {
        failSensorRequest(QString("请求 %1：板端采样失败，可重新读取").arg(id),
                          pendingSensor == SensorKind::Sr04 || pendingSensor == SensorKind::Sr501);
        return;
    }

    failSensorRequest(QString("请求 %1：回复字段或数值无效").arg(id));
}

void MainWindow::handleJpeg(quint32 id, const QByteArray &jpeg)
{
    if (!waitingCameraReply || id != pendingCameraRequestId) {
        if (!waitingCameraReply)
            cameraTimer->stop();
        updateButtons();
        if (previewActive && !waitingCameraReply) {
            if (previewStopRequested)
                requestPreviewStop();
            else if (!previewTimer->isActive())
                previewTimer->start();
        }
        return;
    }
    if (cameraRequest != CameraRequest::Photo && cameraRequest != CameraRequest::PreviewFrame) {
        failCameraRequest("摄像头回复类型错误，连接已关闭，请重新连接");
        return;
    }

    QBuffer buffer;
    buffer.setData(jpeg);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "JPEG");
    const QSize size = reader.size();
    // 先检查尺寸，避免异常文件要求分配过大的解码图像。
    if (!size.isValid() || size.width() > 4096 || size.height() > 4096) {
        failCameraRequest("JPEG 图像无效或尺寸过大，请检查摄像头后重试");
        return;
    }

    const QImage image = reader.read();
    if (image.isNull()) {
        failCameraRequest("JPEG 解码失败，请检查摄像头后重试");
        return;
    }
    const bool previewFrame = cameraRequest == CameraRequest::PreviewFrame;
    finishCameraRequest();
    photo = QPixmap::fromImage(image);
    photoJpeg = jpeg;
    updatePhotoDisplay();
    updateButtons();
    if (previewFrame) {
        ++previewFrames;
        replyLabel->setText(QString("预览第 %1 帧：%2×%3，%4 字节")
                               .arg(previewFrames).arg(image.width()).arg(image.height()).arg(jpeg.size()));
        if (previewStopRequested)
            requestPreviewStop();
        else
            previewTimer->start(); // 收齐且显示完成后才安排下一帧，不累积请求。
    } else {
        replyLabel->setText(QString("拍照请求 %1 完成：%2×%3，%4 字节")
                               .arg(id).arg(image.width()).arg(image.height()).arg(jpeg.size()));
    }
}

void MainWindow::failCameraRequest(const QString &message)
{
    if (previewActive) {
        // 断开连接也通知服务释放流，避免错误后仍占用摄像头。
        socket->abort();
    } else {
        finishCameraRequest();
        clearPhoto();
    }
    replyLabel->setText(message);
}

void MainWindow::finishCameraRequest()
{
    cameraTimer->stop();
    waitingCameraReply = false;
    cameraRequest = CameraRequest::None;
    pendingCameraRequestId = 0;
    updateButtons();
}

void MainWindow::resetReceiveState()
{
    receiveBuffer.clear();
    incomingJpegId = 0;
    incomingJpegBytes = 0;
}

void MainWindow::clearPhoto()
{
    photo = QPixmap();
    photoJpeg.clear();
    imageLabel->clear();
    imageLabel->setText("尚未拍照");
    reverseImageLabel->clear();
    reverseImageLabel->setText("尚未开始倒车监测");
    sentinelImageLabel->clear();
    sentinelImageLabel->setText("活动信号触发后显示画面");
    updateButtons();
}

void MainWindow::updatePhotoDisplay()
{
    if (imageLabel && !photo.isNull())
        imageLabel->setPixmap(photo.scaled(imageLabel->size(), Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation));
    if (reverseImageLabel && !photo.isNull())
        reverseImageLabel->setPixmap(photo.scaled(reverseImageLabel->size(), Qt::KeepAspectRatio,
                                                 Qt::SmoothTransformation));
    if (sentinelImageLabel && !photo.isNull())
        sentinelImageLabel->setPixmap(photo.scaled(sentinelImageLabel->size(), Qt::KeepAspectRatio,
                                                  Qt::SmoothTransformation));
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    updatePhotoDisplay();
}

void MainWindow::finishSensorRequest()
{
    sensorTimer->stop();
    waitingSensorReply = false;
    pendingRequestId = 0;
    updateButtons();
}

void MainWindow::failSensorRequest(const QString &message, bool retryDeviceRead)
{
    if (pendingSensor == SensorKind::Sr501) {
        const bool retry = retryDeviceRead && sentinelActive;
        if (!retry)
            stopSentinelMonitoring();
        finishSensorRequest();
        clearSr501Values();
        const QString status = retry ? "本次读取失败，信号未知；继续监测并重试" : message;
        sr501StatusLabel->setText(status);
        replyLabel->setText(status);
        if (retry)
            sentinelStatusLabel->setText("信号未知，继续监测");
        if (sentinelPreview && previewActive) {
            if (previewStopRequested)
                requestPreviewStop();
            else if (!previewTimer->isActive())
                previewTimer->start();
        }
        return;
    }
    const bool reverseSample = reverseActive && pendingSensor == SensorKind::Sr04;
    // 仅设备明确回复的SR04_READ允许继续；协议错误、超时和用户停止仍按原流程收尾。
    const bool retry = retryDeviceRead && pendingSensor == SensorKind::Sr04 &&
                       ((reverseSample && !previewStopRequested) ||
                        (refreshTimer->isActive() && refreshSensor == SensorKind::Sr04));
    if (!retry)
        stopAutoRefresh();
    finishSensorRequest();
    clearRequestedSensorValues();
    const QString status = retry ? QString("本次采样失败，距离未知；将自动重试") : message;
    if (pendingSensor == SensorKind::Sr04)
        sr04StatusLabel->setText(status);
    replyLabel->setText(status);
    if (reverseSample) {
        if (previewStopRequested)
            requestPreviewStop();
        else if (retry)
            sendCameraRequest(CameraRequest::PreviewFrame); // 先继续画面，再按原间隔重试测距。
        else
            togglePreview();
    }
}

void MainWindow::clearSensorValues()
{
    irLabel->setText("IR 原始计数：—");
    alsLabel->setText("ALS 原始计数：—");
    psLabel->setText("PS 原始计数：—");
    clearImuValues();
    clearDhtValues();
    clearSr04Values();
    resetSentinel();
    clearSr501Values();
}

void MainWindow::clearImuValues()
{
    gyroUnitsLabel->setText("角速度（°/s）：x=—，y=—，z=—");
    gyroLabel->setText("角速度原始计数：gx=—，gy=—，gz=—");
    accelUnitsLabel->setText("加速度（g）：x=—，y=—，z=—");
    accelLabel->setText("加速度原始计数：ax=—，ay=—，az=—");
    temperatureUnitsLabel->setText("芯片温度（°C）：—");
    temperatureLabel->setText("温度原始计数：—");
}

void MainWindow::clearDhtValues()
{
    dhtHumidityLabel->setText("相对湿度（%RH）：—");
    dhtTemperatureLabel->setText("环境温度（°C）：—");
    dhtStatusLabel->setText("暂无有效读数");
}

void MainWindow::clearRequestedSensorValues()
{
    if (pendingSensor == SensorKind::Icm20608) {
        clearImuValues();
    } else if (pendingSensor == SensorKind::Dht11) {
        clearDhtValues();
    } else if (pendingSensor == SensorKind::Sr04) {
        clearSr04Values();
    } else if (pendingSensor == SensorKind::Sr501) {
        clearSr501Values();
    } else {
        irLabel->setText("IR 原始计数：—");
        alsLabel->setText("ALS 原始计数：—");
        psLabel->setText("PS 原始计数：—");
    }
}

void MainWindow::clearSr04Values()
{
    sr04StaleTimer->stop();
    sr04DistanceLabel->setText("障碍物距离：— cm");
    sr04PulseLabel->setText("回波高电平：— μs");
    sr04StatusLabel->setText("暂无有效距离");
    sr04HintLabel->setText("距离提示：未知");
    sr04HintLabel->setStyleSheet(QString());
}

void MainWindow::clearSr501Values()
{
    sr501StaleTimer->stop();
    sr501Level = -1;
    sentinelLowClock.invalidate(); // 未知和丢失的样本不能累计为连续低电平。
    sentinelCountdownTimer->stop();
    sr501LevelLabel->setText("活动信号：未知");
    sr501StatusLabel->setText("暂无有效采样");
}

void MainWindow::updateButtons()
{
    if (pageStack) {
        const int current = pageStack->currentIndex();
        homePageButton->setChecked(current == static_cast<int>(Page::Home));
        sensorPageButton->setChecked(current == static_cast<int>(Page::Sensors));
        cameraPageButton->setChecked(current == static_cast<int>(Page::Camera));
        reversePageButton->setChecked(current == static_cast<int>(Page::Reverse));
        sentinelPageButton->setChecked(current == static_cast<int>(Page::Sentinel));
    }
    const auto state = socket->state();
    const bool canSend = state == QAbstractSocket::ConnectedState &&
                         !waitingSensorReply && !waitingCameraReply && incomingJpegBytes == 0 &&
                         !previewActive && !sentinelActive;
    const bool refreshing = refreshTimer->isActive();
    connectButton->setEnabled(state == QAbstractSocket::UnconnectedState);
    pingButton->setEnabled(canSend && !refreshing);
    sensorButton->setEnabled(canSend && !refreshing);
    imuButton->setEnabled(canSend && !refreshing);
    dhtButton->setEnabled(canSend && !refreshing);
    sr04Button->setEnabled(canSend && !refreshing);
    sr501Button->setEnabled(canSend && !refreshing);
    sentinelButton->setEnabled(state == QAbstractSocket::ConnectedState &&
                              (sentinelActive || (canSend && !refreshing)));
    sentinelButton->setText(sentinelActive ? "停止哨兵监测" : "开始哨兵监测");
    sentinelSaveButton->setEnabled(!photoJpeg.isEmpty() && saveDialog == nullptr);
    captureButton->setEnabled(canSend && !refreshing);
    saveButton->setEnabled(!photoJpeg.isEmpty() && saveDialog == nullptr);
    reverseSaveButton->setEnabled(!photoJpeg.isEmpty() && saveDialog == nullptr);
    previewButton->setEnabled(state == QAbstractSocket::ConnectedState &&
                              !reverseActive && !sentinelActive && !sentinelPreview &&
                              ((previewActive && !previewStopRequested) || (canSend && !refreshing)));
    previewButton->setText(previewStopRequested ? "正在停止预览……"
                                               : previewActive ? "停止预览" : "开始预览");
    autoRefreshButton->setEnabled(state == QAbstractSocket::ConnectedState &&
                                  ((refreshing && refreshSensor == SensorKind::Ap3216c) || (canSend && !refreshing)));
    autoRefreshButton->setText(refreshing && refreshSensor == SensorKind::Ap3216c
                                  ? "停止自动刷新" : "开始自动刷新");
    imuAutoRefreshButton->setEnabled(state == QAbstractSocket::ConnectedState &&
                                     ((refreshing && refreshSensor == SensorKind::Icm20608) || (canSend && !refreshing)));
    imuAutoRefreshButton->setText(refreshing && refreshSensor == SensorKind::Icm20608
                                     ? "停止 ICM 自动刷新" : "开始 ICM 自动刷新");
    dhtAutoRefreshButton->setEnabled(state == QAbstractSocket::ConnectedState &&
                                     ((refreshing && refreshSensor == SensorKind::Dht11) || (canSend && !refreshing)));
    dhtAutoRefreshButton->setText(refreshing && refreshSensor == SensorKind::Dht11
                                     ? "停止温湿度自动刷新" : "开始温湿度自动刷新");
    sr04AutoRefreshButton->setEnabled(state == QAbstractSocket::ConnectedState &&
                                      ((refreshing && refreshSensor == SensorKind::Sr04) || (canSend && !refreshing)));
    sr04AutoRefreshButton->setText(refreshing && refreshSensor == SensorKind::Sr04
                                      ? "停止测距刷新" : "开始测距刷新");
    reverseButton->setEnabled(state == QAbstractSocket::ConnectedState &&
                              ((reverseActive && !previewStopRequested) || (canSend && !refreshing)));
    reverseButton->setText(reverseActive ? (previewStopRequested ? "正在停止倒车监测……" : "停止倒车监测")
                                         : "开始倒车监测");
    disconnectButton->setEnabled(state == QAbstractSocket::ConnectedState);
}
