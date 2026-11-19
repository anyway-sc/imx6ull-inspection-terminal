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
#include <QStackedWidget>
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

static QByteArray makeJpeg()
{
    QImage image(640, 480, QImage::Format_RGB32);
    image.fill(QColor(45, 110, 160));
    QByteArray bytes;
    QBuffer output(&bytes);
    output.open(QIODevice::WriteOnly);
    return image.save(&output, "JPEG") ? bytes : QByteArray();
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
    auto *pages = window.findChild<QStackedWidget *>("pageStack");
    auto *home = window.findChild<QPushButton *>("homePageButton");
    auto *sensors = window.findChild<QPushButton *>("sensorPageButton");
    auto *camera = window.findChild<QPushButton *>("cameraPageButton");
    auto *homeSensor = window.findChild<QPushButton *>("homeSensorButton");
    auto *homeCamera = window.findChild<QPushButton *>("homeCameraButton");
    auto *clock = window.findChild<QLabel *>("clockLabel");
    auto *clockTimer = window.findChild<QTimer *>("clockTimer");
    auto *image = window.findChild<QLabel *>("cameraImage");
    auto *preview = window.findChild<QPushButton *>("previewButton");
    auto *capture = window.findChild<QPushButton *>("captureButton");
    auto *save = window.findChild<QPushButton *>("saveButton");
    auto *socket = window.findChild<QTcpSocket *>();
    QPushButton *automatic = nullptr;
    QPushButton *manual = nullptr;
    QLabel *ir = nullptr;
    for (auto *button : window.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("开始自动刷新"))
            automatic = button;
        if (button->text() == QStringLiteral("读取 AP3216C"))
            manual = button;
    }
    for (auto *label : window.findChildren<QLabel *>()) {
        if (label->text().startsWith("IR "))
            ir = label;
    }
    CHECK(pages && home && sensors && camera && homeSensor && homeCamera && clock && clockTimer &&
          image && preview && capture && save && socket && automatic && manual && ir, "找到页面和业务控件");
    const auto pageIs = [&](const QString &name) { return pages->currentWidget()->objectName() == name; };
    window.show();
    pump(20);
    CHECK(pages->count() == 5 && pageIs("homePage") && home->isChecked(), "默认显示首页和正确导航状态");
    CHECK(clockTimer->isActive() && clock->text().startsWith(QStringLiteral("电脑时间：")), "首页显示电脑时间");
    const QString previousTime = clock->text();
    CHECK(waitUntil([&]() { return clock->text() != previousTime; }, 1500), "时钟持续更新");
    CHECK(window.grab().save("home-page-mock.png"), "保存首页布局");
    homeSensor->click();
    CHECK(pageIs("sensorPage") && sensors->isChecked() && !home->isChecked() && manual->isVisible() &&
          !capture->isVisible(), "首页设备入口只显示设备内容");
    home->click();
    homeCamera->click();
    CHECK(pageIs("cameraPage") && camera->isChecked() && capture->isVisible() && !manual->isVisible(),
          "首页摄像头入口只显示摄像头内容");

    QTcpServer server;
    QPointer<QTcpSocket> peer;
    QByteArray incoming;
    QList<QByteArray> commands;
    QObject::connect(&server, &QTcpServer::newConnection, &window, [&]() {
        peer = server.nextPendingConnection();
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
    socket->connectToHost(QHostAddress::LocalHost, server.serverPort());
    CHECK(waitUntil([&]() { return peer && preview->isEnabled(); }), "页面共享一条连接");
    const auto send = [&](const QByteArray &bytes) { peer->write(bytes); peer->flush(); };
    const auto id = [&]() { return commands.last().split(' ').value(1); };
    const auto startPreview = [&]() {
        const auto count = commands.size();
        preview->click();
        return waitUntil([&]() { return commands.size() == count + 1; }) &&
               commands.last().startsWith("PREVIEW_START ");
    };
    const auto confirmStart = [&]() {
        const auto count = commands.size();
        send("OK " + id() + " PREVIEW_STARTED\n");
        return waitUntil([&]() { return commands.size() == count + 1; }) &&
               commands.last().startsWith("PREVIEW_FRAME ");
    };
    const QByteArray jpeg = makeJpeg();
    CHECK(!jpeg.isEmpty(), "JPEG 编码器可用");
    const auto frame = [&](const QByteArray &number) {
        return "JPEG " + number + ' ' + QByteArray::number(jpeg.size()) + '\n' + jpeg;
    };

    sensors->click();
    automatic->click();
    CHECK(waitUntil([&]() { return commands.size() == 1; }) && commands.last().startsWith("READ_AP3216C "),
          "设备页开启自动刷新");
    const QByteArray sensorId = id();
    sensors->click();
    CHECK(automatic->text() == QStringLiteral("停止自动刷新"), "重复点击当前页不停止业务");
    camera->click();
    CHECK(pageIs("cameraPage") && automatic->text() == QStringLiteral("开始自动刷新") &&
          !preview->isEnabled(), "离开设备页停止后续采样，当前采样仍等待");
    send("AP3216C " + sensorId + " 40 549 0\n");
    CHECK(waitUntil([&]() { return preview->isEnabled(); }) && ir->text().endsWith("40"),
          "已发采样可在后台收尾");
    pump(1150);
    CHECK(commands.size() == 1, "隐藏设备页不再周期请求");
    sensors->click();
    CHECK(automatic->text() == QStringLiteral("开始自动刷新") && ir->text().endsWith("40"),
          "返回设备页保留结果且不自动恢复");
    CHECK(window.grab().save("sensor-page-mock.png"), "保存设备页布局");
    camera->click();
    qInfo("通过：三页导航、时间更新、共享连接、离开设备页停止刷新");

    CHECK(startPreview() && confirmStart(), "摄像头页开启预览");
    const QByteArray firstFrame = id();
    camera->click();
    CHECK(preview->text() == QStringLiteral("停止预览"), "重复点击摄像头页不停止预览");
    const QByteArray first = frame(firstFrame);
    const auto firstHeaderEnd = first.indexOf('\n') + 1;
    send(first.left(firstHeaderEnd + 13));
    pump(30);
    const auto countBeforeLeave = commands.size();
    sensors->click();
    CHECK(pageIs("sensorPage") && !manual->isEnabled(), "接收半帧时可以切页，新业务等待摄像头释放");
    pump(150);
    CHECK(commands.size() == countBeforeLeave, "半帧未完成时不提前发送停止或新取帧");
    send(first.mid(firstHeaderEnd + 13));
    CHECK(waitUntil([&]() { return commands.size() == countBeforeLeave + 1; }) &&
          commands.last().startsWith("PREVIEW_STOP "), "离开后收完当前帧再请求停止");
    send("OK " + id() + " PREVIEW_STOPPED\n");
    CHECK(waitUntil([&]() { return manual->isEnabled(); }), "停止确认后设备页可使用");
    const auto stoppedCount = commands.size();
    pump(200);
    CHECK(commands.size() == stoppedCount, "隐藏摄像头页不请求后续帧");
    camera->click();
    CHECK(preview->text() == QStringLiteral("开始预览") && save->isEnabled() && !image->pixmap().isNull(),
          "返回摄像头页保留最后一帧，可保存但不自动预览");
    pump(30);
    CHECK(window.grab().save("camera-page-mock.png"), "保存摄像头页布局");

    CHECK(startPreview(), "再次启动预览");
    const QByteArray pendingStart = id();
    const auto pendingCount = commands.size();
    home->click();
    CHECK(pageIs("homePage"), "启动等待中可返回首页");
    send("OK " + pendingStart + " PREVIEW_STARTED\n");
    CHECK(waitUntil([&]() { return commands.size() == pendingCount + 1; }) &&
          commands.last().startsWith("PREVIEW_STOP "), "离开后启动成功直接停止，不取首帧");
    send("OK " + id() + " PREVIEW_STOPPED\n");
    CHECK(waitUntil([&]() { return capture->isEnabled(); }), "启动过程中切页收尾完成");
    camera->click();
    CHECK(startPreview() && confirmStart(), "开始帧间停止用例");
    send(frame(id()));
    CHECK(waitUntil([&]() { return !image->pixmap().isNull() &&
                                   window.findChild<QTimer *>("previewTimer")->isActive(); }),
          "显示一帧并安排下一次读取");
    const auto idleCount = commands.size();
    home->click();
    CHECK(waitUntil([&]() { return commands.size() == idleCount + 1; }) &&
          commands.last().startsWith("PREVIEW_STOP "), "帧间切页取消下次取帧并立即停止");
    send("OK " + id() + " PREVIEW_STOPPED\n");
    CHECK(waitUntil([&]() { return capture->isEnabled(); }), "帧间切页停止完成");
    qInfo("通过：摄像头启动中、接收中、帧间切页均停止预览，返回保留图像");

    for (int i = 0; i < 12; ++i) {
        sensors->click();
        camera->click();
        home->click();
    }
    pump(200);
    CHECK(pages->count() == 5 && window.findChildren<QTcpSocket *>().size() == 1 &&
          window.findChildren<QTimer *>("clockTimer").size() == 1 && commands.size() == idleCount + 1,
          "重复切页不新建连接、页面或定时器，不自动发请求");
    camera->click();
    CHECK(startPreview() && confirmStart(), "检查隐藏页面发生断线后的状态");
    home->click();
    peer->abort();
    CHECK(waitUntil([&]() { return socket->state() == QAbstractSocket::UnconnectedState; }) &&
          pageIs("homePage") && !save->isEnabled() && !preview->isEnabled(), "断线清理全部页面业务");
    qInfo("通过：重复导航与断线清理；全部检查使用本地模拟服务");
    return EXIT_SUCCESS;
}
