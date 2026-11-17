#include "mainwindow.h"

#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFontDatabase>
#include <QHostAddress>
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
    MainWindow window;
    window.setWindowTitle("DHT11首页检查（本机模拟数据）");
    auto *socket = window.findChild<QTcpSocket *>();
    auto *manual = window.findChild<QPushButton *>("dhtButton");
    auto *automatic = window.findChild<QPushButton *>("dhtAutoRefreshButton");
    auto *humidity = window.findChild<QLabel *>("dhtHumidityLabel");
    auto *temperature = window.findChild<QLabel *>("dhtTemperatureLabel");
    auto *status = window.findChild<QLabel *>("dhtStatusLabel");
    auto *reply = window.findChild<QLabel *>("replyLabel");
    auto *home = window.findChild<QPushButton *>("homePageButton");
    auto *sensors = window.findChild<QPushButton *>("sensorPageButton");
    auto *camera = window.findChild<QPushButton *>("cameraPageButton");
    auto *preview = window.findChild<QPushButton *>("previewButton");
    auto *refresh = window.findChild<QTimer *>("refreshTimer");
    QPushButton *ap = nullptr;
    QPushButton *apAutomatic = nullptr;
    QLabel *ir = nullptr;
    for (auto *button : window.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("读取 AP3216C")) ap = button;
        if (button->text() == QStringLiteral("开始自动刷新")) apAutomatic = button;
    }
    for (auto *label : window.findChildren<QLabel *>())
        if (label->text().startsWith("IR ")) ir = label;
    CHECK(socket && manual && automatic && humidity && temperature && status && reply && home &&
          sensors && camera && preview && refresh && ap && apAutomatic && ir, "找到控件和定时器");
    CHECK(!manual->isEnabled() && !automatic->isEnabled() && humidity->text().endsWith("—") &&
          temperature->text().endsWith("—"), "初始无读数，断开时禁用读取");
    window.show();

    QTcpServer server;
    QPointer<QTcpSocket> peer;
    QByteArray incoming;
    QList<QByteArray> commands;
    QObject::connect(&server, &QTcpServer::newConnection, &window, [&]() {
        peer = server.nextPendingConnection();
        incoming.clear();
        const QPointer<QTcpSocket> accepted = peer;
        QObject::connect(peer, &QTcpSocket::readyRead, &window, [&, accepted]() {
            if (!accepted || accepted != peer) return;
            incoming += accepted->readAll();
            for (;;) {
                const auto pos = incoming.indexOf('\n');
                if (pos < 0) return;
                commands += incoming.left(pos);
                incoming.remove(0, pos + 1);
            }
        });
    });
    CHECK(server.listen(QHostAddress::LocalHost, 0), "监听随机本机端口");
    const auto connectMock = [&]() {
        socket->connectToHost(QHostAddress::LocalHost, server.serverPort());
        return waitUntil([&]() { return peer && manual->isEnabled(); });
    };
    const auto send = [&](const QByteArray &bytes) { peer->write(bytes); peer->flush(); };
    const auto id = [&]() { return commands.last().split(' ').value(1); };
    const auto sample = [&](const QByteArray &number, const QByteArray &data) {
        return "DHT11 " + number + ' ' + data + '\n';
    };
    const auto request = [&](QPushButton *button, const QByteArray &prefix = "READ_DHT11 ") {
        const auto count = commands.size();
        button->click();
        return waitUntil([&]() { return commands.size() == count + 1; }) && commands.last().startsWith(prefix);
    };
    CHECK(connectMock(), "连接模拟服务");
    CHECK(request(manual) && id() == "1", "首页发送DHT11及请求编号");
    const QByteArray firstId = id();
    CHECK(!ap->isEnabled() && !preview->isEnabled() && !manual->isEnabled(), "DHT请求与其他设备互斥");
    send("DHT11 " + firstId + " 600 ");
    pump(40);
    CHECK(!manual->isEnabled() && humidity->text().endsWith("—"), "分段回复不提前显示");
    send("260\n");
    CHECK(waitUntil([&]() { return manual->isEnabled(); }) &&
          humidity->text() == QStringLiteral("相对湿度（%RH）：60.0") &&
          temperature->text() == QStringLiteral("环境温度（°C）：26.0") &&
          status->text().startsWith(QStringLiteral("最近更新：")), "十分之一单位和顺序正确");
    CHECK(window.grab().save("dht11-home-mock.png"), "保存模拟布局");
    CHECK(request(manual), "第二次读取");
    const QByteArray secondId = id();
    send(sample(firstId, "1000 500"));
    pump(40);
    CHECK(!manual->isEnabled() && humidity->text().endsWith("60.0"), "旧编号不清空、不更新、不结束新请求");
    send(sample(secondId, "607 264"));
    CHECK(waitUntil([&]() { return manual->isEnabled(); }) && humidity->text().endsWith("60.7") &&
          temperature->text().endsWith("26.4"), "新编号更新小数");

    sensors->click();
    CHECK(request(ap, "READ_AP3216C "), "共享编号支持AP");
    send("AP3216C " + id() + " 40 549 0\n");
    CHECK(waitUntil([&]() { return ap->isEnabled(); }) && ir->text().endsWith("40"), "记录AP值供隔离检查");
    home->click();
    for (const QByteArray &invalid : {QByteArray("1001 260"), QByteArray("600 501"), QByteArray("-1 260"),
             QByteArray("+600 260"), QByteArray("60.0 260"), QByteArray("nan 260"),
             QByteArray("4294967296 260"), QByteArray("600"), QByteArray("600 260 1"), QByteArray(" 600 260")}) {
        CHECK(request(manual), "非法回复用例发送");
        send(sample(id(), invalid));
        CHECK(waitUntil([&]() { return manual->isEnabled(); }) && humidity->text().endsWith("—") &&
              temperature->text().endsWith("—") && ir->text().endsWith("40") &&
              reply->text().contains(QStringLiteral("无效")), "拒绝非法字段，只清空DHT");
    }
    for (const QByteArray &boundary : {QByteArray("0 0"), QByteArray("1000 500")}) {
        CHECK(request(manual), "边界回复用例发送");
        send(sample(id(), boundary));
        CHECK(waitUntil([&]() { return manual->isEnabled(); }) && !humidity->text().endsWith("—"), "接受合法边界");
    }
    qInfo("通过：DHT11命令、分帧、单位、范围和旧编号隔离");

    CHECK(request(automatic) && refresh->interval() == 3000, "刷新开始立即读取，周期3秒");
    CHECK(automatic->isEnabled() && !apAutomatic->isEnabled(), "停止可用且刷新源互斥");
    send(sample(id(), "600 260"));
    CHECK(waitUntil([&]() { return humidity->text().endsWith("60.0"); }), "刷新首帧完成");
    const auto firstRefreshCount = commands.size();
    pump(1150);
    CHECK(commands.size() == firstRefreshCount, "DHT不沿用一秒周期");
    CHECK(waitUntil([&]() { return commands.size() == firstRefreshCount + 1; }, 2300), "三秒后继续读取");
    const QByteArray leavingId = id();
    sensors->click();
    CHECK(!refresh->isActive(), "离开首页停止未来刷新");
    send(sample(leavingId, "610 270"));
    CHECK(waitUntil([&]() { return ap->isEnabled(); }) && humidity->text().endsWith("61.0"), "已发请求仍可完成");
    pump(3150);
    CHECK(commands.size() == firstRefreshCount + 1, "隐藏首页不继续采样");
    CHECK(request(apAutomatic, "READ_AP3216C ") && refresh->interval() == 1000, "切回AP恢复一秒周期");
    apAutomatic->click();
    send("AP3216C " + id() + " 41 550 0\n");
    CHECK(waitUntil([&]() { return ap->isEnabled(); }), "AP停止后的在途请求收尾");
    home->click();
    CHECK(!refresh->isActive() && humidity->text().endsWith("61.0"), "返回首页保留值，不自动重开");

    CHECK(request(automatic), "DHT刷新可重开");
    automatic->click();
    CHECK(!refresh->isActive() && !manual->isEnabled(), "停止不取消在途请求");
    send(sample(id(), "620 280"));
    CHECK(waitUntil([&]() { return manual->isEnabled(); }) && humidity->text().endsWith("62.0"), "停止后最后结果仍可更新");
    CHECK(request(automatic), "板端错误用例");
    send("ERR " + id() + " DHT11_READ\n");
    CHECK(waitUntil([&]() { return manual->isEnabled(); }) && !refresh->isActive() &&
          humidity->text().endsWith("—") && temperature->text().endsWith("—") && ir->text().endsWith("41"),
          "板端错误停止刷新，仅清除DHT值");
    qInfo("通过：3秒周期、互斥、停止、离页、AP周期恢复和错误清理");

    CHECK(request(manual), "超时用例");
    const QByteArray expiredId = id();
    CHECK(waitUntil([&]() { return manual->isEnabled(); }, 3800) &&
          reply->text().contains(QStringLiteral("超时")) && humidity->text().endsWith("—"), "三秒超时清空");
    CHECK(request(manual), "超时后新请求");
    const QByteArray currentId = id();
    send(sample(expiredId, "999 499"));
    pump(40);
    CHECK(!manual->isEnabled() && humidity->text().endsWith("—"), "超时旧回复不影响新请求");
    send(sample(currentId, "630 290"));
    CHECK(waitUntil([&]() { return manual->isEnabled(); }) && humidity->text().endsWith("63.0"), "超时后可以恢复");
    CHECK(request(automatic), "旧板端服务用例");
    send("ERR UNKNOWN_COMMAND\n");
    CHECK(waitUntil([&]() { return manual->isEnabled(); }) && !refresh->isActive() &&
          humidity->text().endsWith("—") && reply->text().contains(QStringLiteral("尚不支持 DHT11")), "旧服务给出升级提示");
    CHECK(request(automatic), "断线用例");
    send(sample(id(), "600 260"));
    CHECK(waitUntil([&]() { return humidity->text().endsWith("60.0"); }), "断线前有有效值");
    peer->disconnectFromHost();
    CHECK(waitUntil([&]() { return socket->state() == QAbstractSocket::UnconnectedState; }) &&
          !refresh->isActive() && !manual->isEnabled() && humidity->text().endsWith("—") &&
          temperature->text().endsWith("—"), "断线清空并停止");
    CHECK(connectMock() && !refresh->isActive(), "重连不自动恢复刷新");
    CHECK(request(manual), "重连后读取");
    const QByteArray cameraWaitId = id();
    camera->click();
    CHECK(!preview->isEnabled(), "在途DHT请求阻止摄像头抢占");
    send(sample(cameraWaitId, "600 260"));
    CHECK(waitUntil([&]() { return preview->isEnabled(); }), "DHT完成后摄像头可使用");
    socket->abort();
    pump(20);
    qInfo("通过：超时、迟到旧回复、旧服务、断线重连及摄像头互斥；全部数据为本机模拟");
    return EXIT_SUCCESS;
}
