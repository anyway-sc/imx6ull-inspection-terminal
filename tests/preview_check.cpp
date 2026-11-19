#include "mainwindow.h"

#include <QApplication>
#include <QBuffer>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFontDatabase>
#include <QHostAddress>
#include <QImage>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <cstdlib>
#include <functional>

static void pump(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

static bool waitUntil(const std::function<bool()> &condition, int milliseconds = 1000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < milliseconds)
        pump(10);
    return condition();
}

static QByteArray makeJpeg(const QColor &color)
{
    QImage image(640, 480, QImage::Format_RGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer output(&bytes);
    output.open(QIODevice::WriteOnly);
    if (!image.save(&output, "JPEG"))
        return {};
    return bytes;
}

#define CHECK(condition, message) do { \
    if (!(condition)) { qCritical().noquote() << "失败：" << message; return EXIT_FAILURE; } \
} while (false)

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    const int fontId = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    const auto families = QFontDatabase::applicationFontFamilies(fontId);
    if (!families.isEmpty())
        application.setFont(QFont(families.first(), 9));

    QTcpServer server;
    MainWindow window;
    QPointer<QTcpSocket> peer;
    QByteArray incoming;
    QList<QByteArray> commands;
    int connections = 0;
    auto *socket = window.findChild<QTcpSocket *>();
    auto *preview = window.findChild<QPushButton *>("previewButton");
    auto *photo = window.findChild<QPushButton *>("captureButton");
    auto *image = window.findChild<QLabel *>("cameraImage");
    auto *reply = window.findChild<QLabel *>("replyLabel");
    auto *cameraTimer = window.findChild<QTimer *>("cameraTimer");
    QPushButton *automatic = nullptr;
    QPushButton *manual = nullptr;
    QPushButton *disconnect = nullptr;
    for (auto *button : window.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("开始自动刷新"))
            automatic = button;
        if (button->text().contains("AP3216C"))
            manual = button;
        if (button->text() == QStringLiteral("断开"))
            disconnect = button;
    }
    CHECK(socket && preview && photo && image && reply && cameraTimer && automatic && manual && disconnect,
          "找到界面控件");
    CHECK(!preview->isEnabled(), "未连接时不能开启预览");

    QObject::connect(&server, &QTcpServer::newConnection, &window, [&]() {
        peer = server.nextPendingConnection();
        incoming.clear();
        ++connections;
        const QPointer<QTcpSocket> accepted = peer;
        QObject::connect(peer, &QTcpSocket::readyRead, &window, [&, accepted]() {
            if (!accepted || accepted != peer)
                return;
            incoming += accepted->readAll();
            for (;;) {
                const auto pos = incoming.indexOf('\n');
                if (pos < 0)
                    return;
                commands += incoming.left(pos);
                incoming.remove(0, pos + 1);
            }
        });
    });
    CHECK(server.listen(QHostAddress::LocalHost, 0), "监听随机本地端口");
    const auto connectMock = [&]() {
        const int expected = connections + 1;
        socket->connectToHost(QHostAddress::LocalHost, server.serverPort());
        return waitUntil([&]() { return connections == expected && preview->isEnabled(); });
    };
    const auto send = [&](const QByteArray &bytes) {
        peer->write(bytes);
        peer->flush();
    };
    const auto id = [&]() { return commands.last().split(' ').value(1); };
    const auto header = [](const QByteArray &number, qsizetype length) {
        return "JPEG " + number + ' ' + QByteArray::number(length) + '\n';
    };
    const auto start = [&]() {
        const auto count = commands.size();
        preview->click();
        return waitUntil([&]() { return commands.size() == count + 1; }) &&
               commands.last().startsWith("PREVIEW_START ");
    };
    const auto acknowledgeStart = [&]() {
        const auto count = commands.size();
        send("OK " + id() + " PREVIEW_STARTED\n");
        return waitUntil([&]() { return commands.size() == count + 1; }) &&
               commands.last().startsWith("PREVIEW_FRAME ");
    };
    const auto disconnected = [&]() {
        return waitUntil([&]() { return socket->state() == QAbstractSocket::UnconnectedState; }) &&
               !preview->isEnabled() && preview->text() == QStringLiteral("开始预览") &&
               image->pixmap().isNull();
    };
    const auto center = [&]() {
        const QImage shown = image->pixmap().toImage();
        return shown.isNull() ? QColor() : shown.pixelColor(shown.width() / 2, shown.height() / 2);
    };
    const QByteArray red = makeJpeg(Qt::red);
    const QByteArray green = makeJpeg(Qt::green);
    CHECK(!red.isEmpty() && !green.isEmpty(), "JPEG 编码器可用");
    window.show(); // offscreen，仅用于本地布局检查。
    window.findChild<QPushButton *>("cameraPageButton")->click();
    CHECK(connectMock() && start(), "连接后发送启动命令");
    CHECK(cameraTimer->interval() == 8000, "摄像头请求采用 8 秒超时");
    CHECK(preview->isEnabled() && preview->text() == QStringLiteral("停止预览") &&
          !automatic->isEnabled() && !manual->isEnabled() && !photo->isEnabled(),
          "启动等待中可停止，其他业务禁用");
    const auto beforeStart = commands.size();
    pump(250);
    CHECK(commands.size() == beforeStart, "启动确认前不请求图像");
    send("OK 4294967295 PREVIEW_STARTED\n");
    pump(30);
    CHECK(commands.size() == beforeStart, "忽略其他编号的启动确认");
    CHECK(acknowledgeStart(), "启动确认后立即请求首帧");
    const QByteArray firstFrame = id();
    const auto beforeFrame = commands.size();
    bool responsive = false;
    QTimer::singleShot(0, &window, [&]() { responsive = true; });
    pump(250);
    CHECK(responsive && commands.size() == beforeFrame, "慢帧不重复排队，界面仍响应");
    send(header(firstFrame, red.size()) + red.left(31));
    pump(150);
    CHECK(commands.size() == beforeFrame && image->pixmap().isNull(), "半帧不解码也不请求下一帧");
    send(red.mid(31));
    CHECK(waitUntil([&]() { return center().red() > 200; }), "首帧收齐后显示");
    CHECK(commands.size() == beforeFrame, "显示后保留取帧间隔");
    CHECK(window.grab().save("preview-layout-mock.png"), "保存预览模拟布局");
    CHECK(waitUntil([&]() { return commands.size() == beforeFrame + 1; }), "显示完成后请求下一帧");
    CHECK(commands.last().startsWith("PREVIEW_FRAME ") && id() != firstFrame, "下一帧使用新编号");

    const QByteArray secondFrame = id();
    send(header(firstFrame, red.size()) + red + header(secondFrame, green.size()) + green.left(19));
    pump(40);
    preview->click();
    const auto beforeStop = commands.size();
    pump(200);
    CHECK(commands.size() == beforeStop && !preview->isEnabled(), "等待当前帧期间不提前发送停止命令");
    send(green.mid(19));
    CHECK(waitUntil([&]() { return commands.size() == beforeStop + 1; }) &&
          commands.last().startsWith("PREVIEW_STOP ") && center().green() > 200,
          "收齐当前帧后请求停止");
    CHECK(!photo->isEnabled(), "停止确认前不能重新占用摄像头");
    send("OK " + id() + " PREVIEW_STOPPED\n");
    CHECK(waitUntil([&]() { return preview->isEnabled() && photo->isEnabled(); }) &&
          reply->text().contains("2 帧"), "停止确认后恢复按钮并保留最后一帧");
    pump(250);
    CHECK(commands.size() == beforeStop + 1, "停止后不再取帧");
    qInfo("通过：逐帧请求、慢帧不积压、旧编号过滤、接收途中停止");

    CHECK(start(), "可重新启动预览");
    const QByteArray pendingStart = id();
    const auto earlyStopCount = commands.size();
    preview->click();
    pump(50);
    CHECK(commands.size() == earlyStopCount, "启动过程中停止需要等待启动结果");
    send("OK " + pendingStart + " PREVIEW_STARTED\n");
    CHECK(waitUntil([&]() { return commands.size() == earlyStopCount + 1; }) &&
          commands.last().startsWith("PREVIEW_STOP "), "启动确认后直接停止，不请求首帧");
    send("OK " + id() + " PREVIEW_STOPPED\n");
    CHECK(waitUntil([&]() { return photo->isEnabled(); }), "启动中取消后回到空闲");

    CHECK(start() && acknowledgeStart(), "检查帧间停止");
    send(header(id(), red.size()) + red);
    CHECK(waitUntil([&]() { return center().red() > 200; }), "帧间有完整图像");
    const auto idleStopCount = commands.size();
    preview->click();
    CHECK(waitUntil([&]() { return commands.size() == idleStopCount + 1; }) &&
          commands.last().startsWith("PREVIEW_STOP "), "帧间停止取消下次取帧");
    send("OK " + id() + " PREVIEW_STOPPED\n");
    CHECK(waitUntil([&]() { return photo->isEnabled(); }), "帧间停止完成");
    pump(200);
    CHECK(commands.size() == idleStopCount + 1, "取消的定时器不发送新帧请求");

    window.findChild<QPushButton *>("sensorPageButton")->click();
    automatic->click();
    CHECK(waitUntil([&]() { return commands.last().startsWith("READ_AP3216C "); }) &&
          !preview->isEnabled(), "自动刷新与预览互斥");
    automatic->click();
    send("AP3216C " + id() + " 40 549 0\n");
    CHECK(waitUntil([&]() { return preview->isEnabled(); }), "最后一次采样完成后预览恢复可用");
    window.findChild<QPushButton *>("cameraPageButton")->click();
    qInfo("通过：重复启停、启动中取消、帧间停止、自动刷新互斥");

    CHECK(start(), "模拟旧服务");
    send("ERR UNKNOWN_COMMAND\n");
    CHECK(disconnected() && reply->text().contains("更新板端服务"), "旧服务明确提示更新并清理连接");
    CHECK(connectMock() && start(), "重连后启动失败用例");
    send("ERR " + id() + " CAMERA_START\n");
    CHECK(disconnected(), "启动失败后释放连接状态");
    CHECK(connectMock() && start() && acknowledgeStart(), "取帧失败用例");
    send("ERR " + id() + " CAMERA_FRAME\n");
    CHECK(disconnected(), "取帧失败时断开以释放板端流");
    CHECK(connectMock() && start() && acknowledgeStart(), "非法 JPEG 用例");
    send(header(id(), 3) + "bad");
    CHECK(disconnected(), "预览解码失败时停止并清理");

    CHECK(connectMock() && start() && acknowledgeStart(), "停止失败用例");
    preview->click();
    const auto stopFailureCount = commands.size();
    send(header(id(), red.size()) + red);
    CHECK(waitUntil([&]() { return commands.size() == stopFailureCount + 1; }) &&
          commands.last().startsWith("PREVIEW_STOP "), "等待停止回复");
    send("ERR " + id() + " CAMERA_STOP\n");
    CHECK(disconnected(), "停止失败仍清理连接");

    CHECK(connectMock() && start() && acknowledgeStart(), "预览接收超时用例");
    send(header(id(), red.size()) + red.left(20));
    CHECK(waitUntil([&]() { return socket->state() == QAbstractSocket::UnconnectedState; }, 9500) &&
          image->pixmap().isNull() && reply->text().contains("超时"), "半帧超时后关闭连接");
    CHECK(connectMock(), "超时后手动重连");
    const auto reconnectedCount = commands.size();
    pump(250);
    CHECK(commands.size() == reconnectedCount && preview->text() == QStringLiteral("开始预览"),
          "重连后不自动恢复预览");
    CHECK(start() && acknowledgeStart(), "重连后可手动重开预览");
    send(header(id(), red.size()) + red.left(10));
    pump(30);
    disconnect->click();
    CHECK(disconnected(), "用户可在接收途中断开");
    qInfo("通过：旧服务、各阶段错误、非法图像、半帧超时、断开与重连恢复");
    qInfo("通过：全部连续预览模拟检查；未访问 ADB 或摄像头");
    return EXIT_SUCCESS;
}
