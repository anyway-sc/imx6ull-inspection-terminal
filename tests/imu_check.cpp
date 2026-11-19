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
    auto *socket = window.findChild<QTcpSocket *>();
    auto *imu = window.findChild<QPushButton *>("imuButton");
    auto *automatic = window.findChild<QPushButton *>("imuAutoRefreshButton");
    auto *gyro = window.findChild<QLabel *>("gyroLabel");
    auto *gyroUnits = window.findChild<QLabel *>("gyroUnitsLabel");
    auto *accel = window.findChild<QLabel *>("accelLabel");
    auto *accelUnits = window.findChild<QLabel *>("accelUnitsLabel");
    auto *temperature = window.findChild<QLabel *>("temperatureLabel");
    auto *temperatureUnits = window.findChild<QLabel *>("temperatureUnitsLabel");
    auto *reply = window.findChild<QLabel *>("replyLabel");
    auto *home = window.findChild<QPushButton *>("homePageButton");
    auto *sensors = window.findChild<QPushButton *>("sensorPageButton");
    auto *camera = window.findChild<QPushButton *>("cameraPageButton");
    auto *preview = window.findChild<QPushButton *>("previewButton");
    QPushButton *ap = nullptr;
    QPushButton *apAutomatic = nullptr;
    QLabel *ir = nullptr;
    for (auto *button : window.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("读取 AP3216C"))
            ap = button;
        if (button->text() == QStringLiteral("开始自动刷新"))
            apAutomatic = button;
    }
    for (auto *label : window.findChildren<QLabel *>()) {
        if (label->text().startsWith("IR "))
            ir = label;
    }
    CHECK(socket && imu && automatic && gyro && gyroUnits && accel && accelUnits && temperature && temperatureUnits && reply && home && sensors &&
          camera && preview && ap && apAutomatic && ir, "找到六轴和原有控件");
    CHECK(!imu->isEnabled() && !automatic->isEnabled(), "断开时禁用 ICM");
    CHECK(gyroUnits->text().contains(QStringLiteral("—")) && accelUnits->text().contains(QStringLiteral("—")) &&
          temperatureUnits->text().contains(QStringLiteral("—")),
          "初始单位值为空");
    sensors->click();
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
    CHECK(server.listen(QHostAddress::LocalHost, 0), "监听随机本机端口");
    const auto connectMock = [&]() {
        socket->connectToHost(QHostAddress::LocalHost, server.serverPort());
        return waitUntil([&]() { return peer && imu->isEnabled(); });
    };
    const auto send = [&](const QByteArray &bytes) { peer->write(bytes); peer->flush(); };
    const auto id = [&]() { return commands.last().split(' ').value(1); };
    const QByteArray counts = "3 -15 16 -897 -4 1841 1753";
    const auto sample = [&](const QByteArray &number, const QByteArray &data) {
        return "ICM20608 " + number + ' ' + data + '\n';
    };
    const auto request = [&](QPushButton *button, const QByteArray &prefix) {
        const auto count = commands.size();
        button->click();
        return waitUntil([&]() { return commands.size() == count + 1; }) && commands.last().startsWith(prefix);
    };
    CHECK(connectMock(), "连接模拟服务");
    CHECK(request(ap, "READ_AP3216C "), "AP 单次读取仍可用");
    send("AP3216C " + id() + " 40 549 0\n");
    CHECK(waitUntil([&]() { return imu->isEnabled(); }) && ir->text().endsWith("40"), "记录 AP 成功值");
    CHECK(request(imu, "READ_ICM20608 ") && id() == "2", "两种传感器共用递增编号");
    CHECK(!ap->isEnabled() && !apAutomatic->isEnabled() && !preview->isEnabled(), "ICM 请求期间业务互斥");
    const QByteArray firstId = id();
    send(sample("1", "9 9 9 9 9 9 9") + "ICM20608 " + firstId + " 3 -");
    pump(30);
    CHECK(!imu->isEnabled() && gyro->text().contains(QStringLiteral("—")) &&
          gyroUnits->text().contains(QStringLiteral("—")) && accelUnits->text().contains(QStringLiteral("—")),
          "旧编号忽略，分段回复不提前显示原始值或单位值");
    send("15 16 -897 -4 1841 1753\n");
    CHECK(waitUntil([&]() { return imu->isEnabled(); }) && gyro->text().contains("gy=-15") &&
          accel->text().contains("ax=-897") && temperature->text().endsWith("1753") && ir->text().endsWith("40"),
          "有符号六轴与温度完整显示，不改 AP 值");
    CHECK(gyroUnits->text() == QStringLiteral("角速度（°/s）：x=0.18，y=-0.91，z=0.98") &&
          accelUnits->text() == QStringLiteral("加速度（g）：x=-0.4380，y=-0.0020，z=0.8989") &&
          temperatureUnits->text() == QStringLiteral("芯片温度（°C）：30.36"),
          "各轴顺序、负数和小数精度与原始值对应");
    CHECK(request(imu, "READ_ICM20608 "), "请求已知单位参考值");
    send(sample(id(), "164 -328 0 2048 -1024 1 1753"));
    CHECK(waitUntil([&]() { return imu->isEnabled(); }) &&
          gyroUnits->text() == QStringLiteral("角速度（°/s）：x=10.00，y=-20.00，z=0.00") &&
          accelUnits->text() == QStringLiteral("加速度（g）：x=1.0000，y=-0.5000，z=0.0005") &&
          gyro->text().contains("gx=164") && accel->text().contains("az=1"),
          "单位参考点正确，微小加速度保留小数，原始计数保留");
    send(sample(firstId, counts));
    pump(30);
    CHECK(gyroUnits->text().contains("x=10.00") && accelUnits->text().contains("z=0.0005") &&
          gyro->text().contains("gx=164") && temperatureUnits->text().endsWith("30.36"),
          "迟到旧样本不覆盖新单位值或原始值");
    const struct { const char *raw; const char *celsius; } temperaturePoints[] = {
        {"0", "25.00"}, {"3268", "35.00"}, {"-3268", "15.00"},
        {"-16340", "-25.00"}, {"1676", "30.13"},
    };
    for (const auto &point : temperaturePoints) {
        CHECK(request(imu, "READ_ICM20608 "), "请求温度参考值");
        send(sample(id(), QByteArray("0 0 0 0 0 2048 ") + point.raw));
        CHECK(waitUntil([&]() { return imu->isEnabled(); }) &&
              temperatureUnits->text() == QStringLiteral("芯片温度（°C）：") + point.celsius &&
              temperature->text().endsWith(point.raw),
              "温度零点、正负原始值、负摄氏温度及用户1676计数换算正确");
    }
    CHECK(window.grab().save("imu-device-page-mock.png"), "保存设备页模拟布局");
    qInfo("通过：ICM单位显示、参考值、小数精度、轴序和旧编号保护");

    for (const QByteArray &invalid : {QByteArray("3 +1 2 3 4 5 6"), QByteArray("3 - 2 3 4 5 6"),
             QByteArray("3 2147483648 2 3 4 5 6"), QByteArray("3 -2147483649 2 3 4 5 6"),
             QByteArray("3 1 2 3 4 5"), QByteArray("3 1 2 3 4 5 6 7"), QByteArray("3 1x 2 3 4 5 6")}) {
        CHECK(request(imu, "READ_ICM20608 "), "发送无效数据用例");
        send(sample(id(), invalid));
        CHECK(waitUntil([&]() { return imu->isEnabled(); }) && gyro->text().contains(QStringLiteral("—")) &&
              gyroUnits->text().contains(QStringLiteral("—")) && accelUnits->text().contains(QStringLiteral("—")) &&
              temperature->text().contains(QStringLiteral("—")) &&
              temperatureUnits->text().contains(QStringLiteral("—")) && ir->text().endsWith("40"),
              "非法字段结束等待，仅清理 ICM 数据");
    }
    CHECK(request(imu, "READ_ICM20608 "), "整数边界用例");
    send(sample(id(), "-2147483648 2147483647 0 -0 001 -002 3"));
    CHECK(waitUntil([&]() { return imu->isEnabled(); }) && gyro->text().contains("-2147483648") &&
          gyro->text().contains("2147483647"), "协议 int32 边界严格解析");
    qInfo("通过：负数、分段、编号、字段数量、符号、溢出和原 AP 结果保留");

    CHECK(request(automatic, "READ_ICM20608 "), "ICM 自动刷新立即读取");
    CHECK(automatic->isEnabled() && !apAutomatic->isEnabled() && !ap->isEnabled(), "自动刷新源互斥，停止可用");
    const QByteArray refreshId = id();
    const auto refreshCount = commands.size();
    pump(1150);
    CHECK(commands.size() == refreshCount, "慢回复不重叠请求");
    send(sample(refreshId, counts));
    CHECK(waitUntil([&]() { return temperature->text().endsWith("1753"); }) &&
          gyroUnits->text().contains("x=0.18") && accelUnits->text().contains("z=0.8989") &&
          temperatureUnits->text().endsWith("30.36"),
          "周期结果同步显示单位值");
    CHECK(waitUntil([&]() { return commands.size() == refreshCount + 1; }, 1300) &&
          commands.last().startsWith("READ_ICM20608 "), "下一周期继续读取 ICM");
    const QByteArray finalRefreshId = id();
    home->click();
    send(sample(finalRefreshId, "4 5 6 -10 -20 2000 1800"));
    CHECK(waitUntil([&]() { return imu->isEnabled(); }) && temperature->text().endsWith("1800") &&
          temperatureUnits->text().endsWith("30.51"),
          "离页停止未来请求，最后一条仍可完成");
    pump(1100);
    CHECK(commands.size() == refreshCount + 1, "隐藏设备页不再采样");
    sensors->click();
    CHECK(automatic->text() == QStringLiteral("开始 ICM 自动刷新") &&
          accelUnits->text().contains("z=0.9766") && temperatureUnits->text().endsWith("30.51"),
          "返回不自动恢复并保留最后单位值");
    CHECK(request(automatic, "READ_ICM20608 "), "ICM 自动刷新可重开");
    send("ERR " + id() + " ICM20608_READ\n");
    CHECK(waitUntil([&]() { return imu->isEnabled(); }) && automatic->text().startsWith(QStringLiteral("开始")) &&
          gyro->text().contains(QStringLiteral("—")) && gyroUnits->text().contains(QStringLiteral("—")) &&
          accelUnits->text().contains(QStringLiteral("—")) && temperatureUnits->text().contains(QStringLiteral("—")),
          "板端错误停止刷新并清除 ICM原始值和单位值");

    CHECK(request(automatic, "READ_ICM20608 "), "ICM 超时用例");
    const QByteArray timeoutId = id();
    CHECK(waitUntil([&]() { return imu->isEnabled(); }, 3800) && reply->text().contains(QStringLiteral("超时")) &&
          automatic->text().startsWith(QStringLiteral("开始")) && ir->text().endsWith("40") &&
          gyroUnits->text().contains(QStringLiteral("—")) && accelUnits->text().contains(QStringLiteral("—")) &&
          temperatureUnits->text().contains(QStringLiteral("—")),
          "超时停止刷新且保留 AP 结果");
    send(sample(timeoutId, counts));
    pump(30);
    CHECK(gyro->text().contains(QStringLiteral("—")) && gyroUnits->text().contains(QStringLiteral("—")) &&
          accelUnits->text().contains(QStringLiteral("—")), "迟到 ICM 回复不显示原始值或单位值");
    CHECK(request(imu, "READ_ICM20608 "), "旧服务用例");
    send("ERR UNKNOWN_COMMAND\n");
    CHECK(waitUntil([&]() { return imu->isEnabled(); }) && reply->text().contains(QStringLiteral("更新板端服务")),
          "旧服务给出明确提示，可继续用 AP");
    CHECK(request(ap, "READ_AP3216C "), "ICM 失败后 AP 可用");
    send("AP3216C " + id() + " 41 550 0\n");
    CHECK(waitUntil([&]() { return imu->isEnabled(); }), "AP 请求完成");
    qInfo("通过：ICM周期请求、互斥、离页停止、失败、超时、迟到和旧服务恢复");

    camera->click();
    CHECK(request(preview, "PREVIEW_START "), "摄像头仍可启动");
    send("OK " + id() + " PREVIEW_STARTED\n");
    CHECK(waitUntil([&]() { return commands.last().startsWith("PREVIEW_FRAME "); }) && !imu->isEnabled(),
          "预览期间禁止 ICM 采样");
    sensors->click();
    send("ERR " + id() + " CAMERA_FRAME\n");
    CHECK(waitUntil([&]() { return socket->state() == QAbstractSocket::ConnectedState && imu->isEnabled(); }) &&
          ir->text().endsWith("41"), "相机错误后保留连接和已有传感器结果");
    peer->abort();
    CHECK(waitUntil([&]() { return socket->state() == QAbstractSocket::UnconnectedState; }) &&
          !imu->isEnabled() && ir->text().contains(QStringLiteral("—")) && gyro->text().contains(QStringLiteral("—")) &&
          gyroUnits->text().contains(QStringLiteral("—")) && accelUnits->text().contains(QStringLiteral("—")) &&
          temperatureUnits->text().contains(QStringLiteral("—")),
          "断线统一清空两种传感器");
    CHECK(connectMock(), "手动重连后可重新使用");
    const auto reconnectedCount = commands.size();
    pump(1100);
    CHECK(commands.size() == reconnectedCount && automatic->text().startsWith(QStringLiteral("开始")),
          "重连不自动恢复 ICM 刷新");
    CHECK(request(imu, "READ_ICM20608 "), "重连后手动恢复");
    send(sample(id(), counts));
    CHECK(waitUntil([&]() { return imu->isEnabled(); }) && accel->text().contains("az=1841") &&
          accelUnits->text().contains("z=0.8989"), "重连后完整原始值和单位值可显示");
    qInfo("通过：全部 ICM Qt 检查，仅使用本机模拟服务");
    return EXIT_SUCCESS;
}
