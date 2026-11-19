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
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <cstdlib>
#include <functional>

static unsigned int checks;

static void pump(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

static bool waitUntil(const std::function<bool()> &condition, int milliseconds = 1200)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < milliseconds)
        pump(10);
    return condition();
}

static QByteArray makeJpeg()
{
    QImage image(640, 480, QImage::Format_RGB32);
    image.fill(QColor(220, 231, 241));
    QPainter painter(&image);
    painter.fillRect(0, 220, 640, 260, QColor(74, 89, 101));
    painter.setPen(QPen(QColor(246, 216, 110), 8));
    painter.drawLine(110, 479, 240, 250);
    painter.drawLine(530, 479, 400, 250);
    painter.setPen(Qt::black);
    painter.drawText(30, 60, "LOCAL MOCK CAMERA");
    painter.end();
    QByteArray bytes;
    QBuffer output(&bytes);
    output.open(QIODevice::WriteOnly);
    return image.save(&output, "JPEG") ? bytes : QByteArray();
}

#define CHECK(condition, message) do { \
    ++checks; \
    if (!(condition)) { qCritical().noquote() << "失败：" << message; return EXIT_FAILURE; } \
} while (false)

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    const auto families = QFontDatabase::applicationFontFamilies(
        QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc"));
    if (!families.isEmpty())
        application.setFont(QFont(families.first(), 9));
    MainWindow window;
    window.setWindowTitle("SR04倒车页检查（本机模拟数据）");
    auto *socket = window.findChild<QTcpSocket *>();
    auto *pages = window.findChild<QStackedWidget *>("pageStack");
    auto *home = window.findChild<QPushButton *>("homePageButton");
    auto *reverse = window.findChild<QPushButton *>("reversePageButton");
    auto *shortcut = window.findChild<QPushButton *>("homeReverseButton");
    auto *camera = window.findChild<QPushButton *>("cameraPageButton");
    auto *manual = window.findChild<QPushButton *>("sr04Button");
    auto *automatic = window.findChild<QPushButton *>("sr04AutoRefreshButton");
    auto *monitor = window.findChild<QPushButton *>("reverseButton");
    auto *save = window.findChild<QPushButton *>("reverseSaveButton");
    auto *preview = window.findChild<QPushButton *>("previewButton");
    auto *distance = window.findChild<QLabel *>("sr04DistanceLabel");
    auto *pulse = window.findChild<QLabel *>("sr04PulseLabel");
    auto *status = window.findChild<QLabel *>("sr04StatusLabel");
    auto *hint = window.findChild<QLabel *>("sr04HintLabel");
    auto *image = window.findChild<QLabel *>("reverseImage");
    auto *reply = window.findChild<QLabel *>("replyLabel");
    auto *refresh = window.findChild<QTimer *>("refreshTimer");
    auto *sensorTimeout = window.findChild<QTimer *>("sensorTimer");
    auto *previewTimer = window.findChild<QTimer *>("previewTimer");
    auto *stale = window.findChild<QTimer *>("sr04StaleTimer");
    CHECK(socket && pages && home && reverse && shortcut && camera && manual && automatic && monitor &&
          save && preview && distance && pulse && status && hint && image && reply && refresh &&
          sensorTimeout && previewTimer && stale, "找到四页导航、测距、图像与定时器");
    CHECK(pages->count() == 5 && !manual->isEnabled() && !monitor->isEnabled() &&
          distance->text().contains("—") && stale->interval() == 2000, "初始状态与过期时间");
    window.show();
    shortcut->click();
    CHECK(pages->currentWidget()->objectName() == "reversePage" && reverse->isChecked(), "首页进入倒车页");

    QTcpServer server;
    QPointer<QTcpSocket> peer;
    QByteArray incoming;
    QList<QByteArray> commands;
    std::function<void(const QByteArray &)> responder;
    QObject::connect(&server, &QTcpServer::newConnection, &window, [&]() {
        peer = server.nextPendingConnection();
        incoming.clear();
        const QPointer<QTcpSocket> accepted = peer;
        QObject::connect(peer, &QTcpSocket::readyRead, &window, [&, accepted]() {
            if (!accepted || accepted != peer) return;
            incoming += accepted->readAll();
            while (incoming.contains('\n')) {
                const auto end = incoming.indexOf('\n');
                const QByteArray line = incoming.left(end);
                incoming.remove(0, end + 1);
                commands += line;
                if (responder) responder(line);
            }
        });
    });
    CHECK(server.listen(QHostAddress::LocalHost, 0), "监听随机本地端口");
    socket->connectToHost(QHostAddress::LocalHost, server.serverPort());
    CHECK(waitUntil([&]() { return peer && manual->isEnabled(); }), "连接本机模拟服务");
    const auto send = [&](const QByteArray &bytes) { peer->write(bytes); peer->flush(); };
    const auto id = [&]() { return commands.last().split(' ').value(1); };
    const auto request = [&](QPushButton *button, const QByteArray &prefix) {
        const auto count = commands.size();
        button->click();
        return waitUntil([&]() { return commands.size() == count + 1; }) && commands.last().startsWith(prefix);
    };
    const auto sample = [](const QByteArray &number, const QByteArray &values) {
        return "SR04 " + number + ' ' + values + '\n';
    };
    const QByteArray jpeg = makeJpeg();
    const auto frame = [&](const QByteArray &number) {
        return "JPEG " + number + ' ' + QByteArray::number(jpeg.size()) + '\n' + jpeg;
    };
    CHECK(!jpeg.isEmpty(), "模拟JPEG可编码");

    CHECK(request(manual, "READ_SR04 ") && id() == "1", "单次测距使用共享编号");
    CHECK(sensorTimeout->interval() == 3000 && !monitor->isEnabled() && !preview->isEnabled(), "在途请求互斥");
    const QByteArray firstId = id();
    send("SR04 " + firstId + " 91 ");
    pump(30);
    CHECK(distance->text().contains("—") && !manual->isEnabled(), "半行不交付部分值");
    send("529\n");
    CHECK(waitUntil([&]() { return manual->isEnabled(); }) && distance->text().contains("9.1 cm") &&
          pulse->text().contains("529") && hint->text().contains("较近"), "91毫米显示9.1厘米与回波");
    send("SR04 999999 4000 23324\n");
    pump(20);
    CHECK(distance->text().contains("9.1 cm"), "旧编号不改变有效距离");

    for (const auto &values : {QByteArray("300 1749"), QByteArray("1000 5831"), QByteArray("1001 5837")}) {
        CHECK(request(manual, "READ_SR04 "), "读取阈值附近距离");
        send(sample(id(), values));
        CHECK(waitUntil([&]() { return manual->isEnabled(); }), "阈值回复完成");
        CHECK(hint->text().contains(values.startsWith("300 ") ? "较近" : values.startsWith("1000 ") ? "注意" : "较远"),
              "距离阈值边界提示正确");
    }
    for (const auto &values : {QByteArray("19 111"), QByteArray("4001 23330"), QByteArray("-1 529"),
                               QByteArray("91 2000"), QByteArray("91 529 extra")}) {
        CHECK(request(manual, "READ_SR04 "), "开始无效回复用例");
        send(sample(id(), values));
        CHECK(waitUntil([&]() { return manual->isEnabled(); }) && distance->text().contains("—") &&
              hint->text().contains("未知"), "范围、符号、不一致和额外字段均失败清空");
    }
    CHECK(request(automatic, "READ_SR04 ") && refresh->interval() == 500, "测距500ms周期");
    send(sample(id(), "200 1166"));
    const auto autoCount = commands.size();
    CHECK(waitUntil([&]() { return commands.size() == autoCount + 1; }), "到周期再次读取");
    pump(650);
    CHECK(commands.size() == autoCount + 1, "等待期间不堆积采样请求");
    automatic->click();
    send(sample(id(), "343 2000"));
    CHECK(waitUntil([&]() { return manual->isEnabled(); }) && distance->text().contains("34.3 cm"), "停止保留最后在途结果");
    const auto stoppedCount = commands.size();
    pump(600);
    CHECK(commands.size() == stoppedCount, "停止后不再采样");
    CHECK(request(automatic, "READ_SR04 "), "开启测距刷新错误恢复用例");
    const auto refreshErrorCount = commands.size();
    send("ERR " + id() + " SR04_READ\n");
    CHECK(waitUntil([&]() { return !sensorTimeout->isActive(); }) && refresh->isActive() &&
          automatic->text().contains("停止") && distance->text().contains("—") &&
          hint->text().contains("未知") && status->text().contains("采样失败"),
          "单次设备采样失败不停止周期刷新，距离显示未知");
    CHECK(waitUntil([&]() { return commands.size() == refreshErrorCount + 1; }) &&
          commands.last().startsWith("READ_SR04 "), "按原周期再次测距");
    send(sample(id(), "343 2000"));
    CHECK(waitUntil([&]() { return !sensorTimeout->isActive(); }) &&
          distance->text().contains("34.3 cm") && status->text().contains("最近测距"),
          "下一次有效采样恢复距离和时间");
    automatic->click();
    CHECK(request(manual, "READ_SR04 "), "单次读取仍报告设备错误");
    send("ERR " + id() + " SR04_READ\n");
    CHECK(waitUntil([&]() { return manual->isEnabled(); }) && !refresh->isActive() &&
          distance->text().contains("—"), "手动采样失败不自行开启自动刷新");
    CHECK(request(automatic, "READ_SR04 "), "开始离页刷新用例");
    const QByteArray leavingId = id();
    home->click();
    send(sample(leavingId, "200 1166"));
    CHECK(waitUntil([&]() { return manual->isEnabled(); }) && !refresh->isActive(), "离页停止且允许在途完成");
    reverse->click();
    CHECK(automatic->text().contains("开始") && distance->text().contains("20.0 cm"), "返回不自动恢复");

    const auto startMonitor = [&]() {
        if (!request(monitor, "PREVIEW_START ")) return false;
        const auto count = commands.size();
        send("OK " + id() + " PREVIEW_STARTED\n");
        return waitUntil([&]() { return commands.size() == count + 1; }) && commands.last().startsWith("READ_SR04 ");
    };
    const auto acknowledgeStop = [&]() {
        if (!commands.last().startsWith("PREVIEW_STOP ")) return false;
        send("OK " + id() + " PREVIEW_STOPPED\n");
        return waitUntil([&]() { return monitor->isEnabled() && monitor->text().contains("开始"); });
    };
    CHECK(startMonitor() && !manual->isEnabled() && !preview->isEnabled(), "联合监测先启动相机再测距");
    const auto waitingCount = commands.size();
    monitor->click();
    pump(30);
    CHECK(commands.size() == waitingCount, "停止等待中的测距，不越过请求发STOP");
    send(sample(id(), "1250 7290"));
    CHECK(waitUntil([&]() { return commands.size() == waitingCount + 1; }) && acknowledgeStop(), "测距完成后停止相机");

    CHECK(startMonitor(), "启动联合循环");
    responder = [&](const QByteArray &line) {
        const QByteArray number = line.split(' ').value(1);
        if (line.startsWith("READ_SR04 ")) send(sample(number, "1250 7290"));
        if (line.startsWith("PREVIEW_FRAME ")) send(frame(number));
    };
    const auto cycleStart = commands.size();
    send(sample(id(), "1250 7290"));
    pump(1300);
    int samples = 0, frames = 0;
    for (qsizetype i = cycleStart; i < commands.size(); ++i) {
        samples += commands[i].startsWith("READ_SR04 ");
        frames += commands[i].startsWith("PREVIEW_FRAME ");
    }
    CHECK(samples >= 2 && frames >= 3 && !image->pixmap().isNull() && distance->text().contains("125.0 cm"),
          "同连接交替测距与连续取帧");
    CHECK(save->isEnabled() && window.grab().save("sr04-reverse-mock.png"), "最后一帧可保存，生成模拟布局图");
    CHECK(waitUntil([&]() { return previewTimer->isActive(); }), "等待帧间空闲");
    responder = {};
    CHECK(request(monitor, "PREVIEW_STOP ") && acknowledgeStop(), "帧间正常停止");
    CHECK(!stale->isActive() && distance->text().contains("125.0 cm"), "停止保留最后距离与画面");

    CHECK(startMonitor(), "开始半帧离页和距离过期用例");
    send(sample(id(), "343 2000"));
    CHECK(waitUntil([&]() { return commands.last().startsWith("PREVIEW_FRAME "); }), "测距后请求图像");
    const QByteArray payload = frame(id());
    const auto split = payload.indexOf('\n') + 18;
    send(payload.left(split));
    pump(20);
    stale->start(30);
    CHECK(waitUntil([&]() { return status->text().contains("过期"); }) && distance->text().contains("—") &&
          hint->text().contains("未知"), "图像阻塞时旧距离及时失效");
    stale->setInterval(2000);
    const auto halfCount = commands.size();
    camera->click();
    CHECK(!preview->isEnabled(), "切到相机页等待原监测释放");
    pump(50);
    CHECK(commands.size() == halfCount, "半帧期间不交错STOP或新测距");
    send(payload.mid(split));
    CHECK(waitUntil([&]() { return commands.size() == halfCount + 1; }) && acknowledgeStop(), "收齐半帧后STOP");
    CHECK(preview->isEnabled(), "停止确认后相机页可接管");
    CHECK(request(preview, "PREVIEW_START "), "旧摄像头模式可用");
    const QByteArray cameraStart = id();
    reverse->click();
    CHECK(!monitor->isEnabled(), "相机启动在途时倒车页等待");
    const auto cameraCount = commands.size();
    send("OK " + cameraStart + " PREVIEW_STARTED\n");
    CHECK(waitUntil([&]() { return commands.size() == cameraCount + 1; }) && acknowledgeStop(), "切页后启动成功直接停止");

    CHECK(startMonitor(), "开始采样错误用例");
    const auto errorCount = commands.size();
    send("ERR " + id() + " SR04_READ\n");
    CHECK(waitUntil([&]() { return commands.size() == errorCount + 1; }) &&
          commands.last().startsWith("PREVIEW_FRAME ") && monitor->text().contains("停止倒车监测") &&
          status->text().contains("采样失败") && distance->text().contains("—") && hint->text().contains("未知"),
          "测距失败清空距离并继续取帧，不停止倒车监测");
    responder = [&](const QByteArray &line) {
        if (line.startsWith("PREVIEW_FRAME ")) send(frame(line.split(' ').value(1)));
    };
    send(frame(id()));
    for (int i = 0; i < 3; ++i) {
        CHECK(waitUntil([&]() { return commands.last().startsWith("READ_SR04 "); }), "画面继续更新并到期重试测距");
        const auto readCount = commands.size();
        send("ERR " + id() + " SR04_READ\n");
        CHECK(waitUntil([&]() { return commands.size() > readCount; }) &&
              commands.last().startsWith("PREVIEW_FRAME "), "连续设备采样错误仍继续预览");
    }
    CHECK(waitUntil([&]() { return commands.last().startsWith("READ_SR04 "); }), "等待错误后的恢复采样");
    send(sample(id(), "343 2000"));
    CHECK(waitUntil([&]() { return distance->text().contains("34.3 cm") && previewTimer->isActive(); }) &&
          hint->text().contains("注意") && status->text().contains("最近测距"), "连续失败后有效结果恢复正常显示");
    responder = {};
    CHECK(request(monitor, "PREVIEW_STOP ") && acknowledgeStop(), "错误恢复后仍可手动停止");

    CHECK(startMonitor(), "开始采样失败时停止用例");
    monitor->click();
    const auto stoppingCount = commands.size();
    send("ERR " + id() + " SR04_READ\n");
    CHECK(waitUntil([&]() { return commands.size() == stoppingCount + 1; }) && acknowledgeStop(),
          "用户已请求停止时，错误回复收尾发送STOP，不继续取帧");
    CHECK(startMonitor(), "开始无效测距回复用例");
    const auto invalidCount = commands.size();
    send(sample(id(), "19 111"));
    CHECK(waitUntil([&]() { return commands.size() == invalidCount + 1; }) && acknowledgeStop(),
          "协议数值无效仍停止监测，不能作为设备暂时错误重试");
    CHECK(startMonitor(), "开始采样超时用例");
    const QByteArray lateId = id();
    sensorTimeout->start(30);
    const auto timeoutCount = commands.size();
    CHECK(waitUntil([&]() { return commands.size() == timeoutCount + 1; }) && acknowledgeStop(), "超时后停止相机");
    CHECK(request(manual, "READ_SR04 "), "超时后可重新读取");
    send(sample(lateId, "91 529"));
    pump(20);
    CHECK(!manual->isEnabled() && distance->text().contains("—"), "旧超时回复不结束新请求");
    send(sample(id(), "343 2000"));
    CHECK(waitUntil([&]() { return manual->isEnabled(); }), "新编号仍可成功");
    CHECK(request(manual, "READ_SR04 "), "检查旧服务兼容提示");
    send("ERR UNKNOWN_COMMAND\n");
    CHECK(waitUntil([&]() { return manual->isEnabled(); }) && reply->text().contains("尚不支持 SR04"), "旧服务提示升级");

    CHECK(startMonitor(), "断线清理用例");
    peer->abort();
    CHECK(waitUntil([&]() { return socket->state() == QAbstractSocket::UnconnectedState; }) &&
          !monitor->isEnabled() && !refresh->isActive() && !stale->isActive() &&
          distance->text().contains("—") && image->pixmap().isNull(), "断线清空距离、画面和所有业务");
    qInfo("通过：%u条断言；SR04协议、单位、刷新、过期、相机互斥、停止/离页与错误收尾。", checks);
    return EXIT_SUCCESS;
}
