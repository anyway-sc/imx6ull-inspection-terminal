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
#include <cstdio>
#include <functional>

static unsigned int checks;
static void pump(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
static bool waitUntil(const std::function<bool()> &condition, int timeout = 1800)
{
    QElapsedTimer clock;
    clock.start();
    while (!condition() && clock.elapsed() < timeout)
        pump(10);
    return condition();
}
#define CHECK(condition, message) do { ++checks; if (!(condition)) { \
    qCritical().noquote() << "失败：" << message; return EXIT_FAILURE; \
} } while (false)

int main(int argc, char **argv)
{
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        const QByteArray bytes = message.toUtf8();
        std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stderr);
        std::fputc('\n', stderr);
    });
    QApplication app(argc, argv);
    const auto fonts = QFontDatabase::applicationFontFamilies(
        QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc"));
    if (!fonts.isEmpty())
        app.setFont(QFont(fonts.first(), 9));
    MainWindow window;
    window.setWindowTitle("SR501 哨兵页（本机模拟）");
    window.show();
    auto *socket = window.findChild<QTcpSocket *>();
    auto *pages = window.findChild<QStackedWidget *>("pageStack");
    auto *page = window.findChild<QPushButton *>("sentinelPageButton");
    auto *home = window.findChild<QPushButton *>("homePageButton");
    auto *manual = window.findChild<QPushButton *>("sr501Button");
    auto *monitor = window.findChild<QPushButton *>("sentinelButton");
    auto *save = window.findChild<QPushButton *>("sentinelSaveButton");
    auto *level = window.findChild<QLabel *>("sr501LevelLabel");
    auto *status = window.findChild<QLabel *>("sr501StatusLabel");
    auto *state = window.findChild<QLabel *>("sentinelStatusLabel");
    auto *image = window.findChild<QLabel *>("sentinelImage");
    auto *refresh = window.findChild<QTimer *>("refreshTimer");
    auto *timeout = window.findChild<QTimer *>("sensorTimer");
    auto *stale = window.findChild<QTimer *>("sr501StaleTimer");
    CHECK(socket && pages && page && home && manual && monitor && save && level && status && state && image && refresh && timeout && stale, "找到哨兵控件");
    CHECK(pages->count() == 5 && !manual->isEnabled() && level->text().contains("未知"), "初始状态与五页");
    page->click();
    CHECK(pages->currentWidget()->objectName() == "sentinelPage", "导航进入哨兵页");

    QImage mock(640, 480, QImage::Format_RGB32);
    mock.fill(QColor("#e1e9f3"));
    QPainter painter(&mock);
    painter.setPen(QColor("#253754"));
    painter.setFont(QFont("Arial", 21));
    painter.drawText(mock.rect(), Qt::AlignCenter, "LOCAL MOCK CAMERA\nSR501 HIGH");
    painter.end();
    QByteArray jpeg;
    QBuffer out(&jpeg);
    out.open(QIODevice::WriteOnly);
    CHECK(mock.save(&out, "JPEG"), "生成模拟JPEG");

    QTcpServer server;
    CHECK(server.listen(QHostAddress::LocalHost, 0), "模拟TCP服务监听");
    QPointer<QTcpSocket> peer;
    QByteArray buffer, heldSensor, heldFrame;
    QByteArray sample = "0";
    bool holdSensor = false, holdFrame = false;
    int reads = 0, starts = 0, stops = 0, frames = 0;
    auto writeJpeg = [&](const QByteArray &id) {
        peer->write("JPEG " + id + ' ' + QByteArray::number(jpeg.size()) + '\n' + jpeg);
    };
    QObject::connect(&server, &QTcpServer::newConnection, [&]() {
        peer = server.nextPendingConnection();
        QObject::connect(peer, &QTcpSocket::readyRead, [&]() {
            buffer += peer->readAll();
            while (buffer.contains('\n')) {
                const int end = buffer.indexOf('\n');
                const auto fields = buffer.left(end).split(' ');
                buffer.remove(0, end + 1);
                if (fields.size() != 2)
                    continue;
                const auto id = fields[1];
                if (fields[0] == "READ_SR501") {
                    ++reads;
                    if (holdSensor) { heldSensor = id; continue; }
                    if (sample == "error")
                        peer->write("ERR " + id + " SR501_READ\n");
                    else if (sample == "old")
                        peer->write("ERR UNKNOWN_COMMAND\n");
                    else
                        peer->write("SR501 " + id + ' ' + sample + '\n');
                } else if (fields[0] == "PREVIEW_START") {
                    ++starts;
                    peer->write("OK " + id + " PREVIEW_STARTED\n");
                } else if (fields[0] == "PREVIEW_FRAME") {
                    ++frames;
                    if (holdFrame) { heldFrame = id; continue; }
                    writeJpeg(id);
                } else if (fields[0] == "PREVIEW_STOP") {
                    ++stops;
                    peer->write("OK " + id + " PREVIEW_STOPPED\n");
                }
            }
        });
    });
    socket->connectToHost(QHostAddress::LocalHost, server.serverPort());
    CHECK(waitUntil([&]() { return peer && manual->isEnabled(); }), "共享连接可读取");
    holdSensor = true;
    manual->click();
    CHECK(waitUntil([&]() { return !heldSensor.isEmpty(); }), "单次请求发出");
    peer->write("SR501 4294967295 1\n");
    pump(50);
    CHECK(timeout->isActive() && level->text().contains("未知"), "旧编号不能更新信号");
    peer->write("SR501 " + heldSensor + " 1");
    pump(50);
    CHECK(timeout->isActive(), "半行等待结束符");
    peer->write("\n");
    CHECK(waitUntil([&]() { return manual->isEnabled(); }), "完整回复完成");
    CHECK(level->text().contains("OUT=1") && starts == 0, "手动读取不打开摄像头");
    heldSensor.clear();
    holdSensor = false;
    sample = "0";
    monitor->click();
    CHECK(waitUntil([&]() { return level->text().contains("OUT=0"); }), "监测立即采样");
    CHECK(refresh->isActive() && refresh->interval() == 500 && starts == 0, "低电平等待且500毫秒轮询");
    const int beforeHold = reads;
    holdSensor = true;
    CHECK(waitUntil([&]() { return !heldSensor.isEmpty(); }), "等待中的监测请求");
    pump(650);
    CHECK(reads == beforeHold + 1, "等待时不累积采样请求");
    monitor->click();
    peer->write("SR501 " + heldSensor + " 1\n");
    heldSensor.clear();
    CHECK(waitUntil([&]() { return manual->isEnabled(); }), "停止后迟到回复完成");
    CHECK(!refresh->isActive() && starts == 0, "迟到高电平不能重新打开摄像头");
    holdSensor = false;
    sample = "1";
    monitor->click();
    CHECK(waitUntil([&]() { return starts == 1 && frames >= 2; }), "高电平触发且持续取帧");
    CHECK(!image->pixmap().isNull() && save->isEnabled(), "触发画面可显示与保存");
    CHECK(window.grab().save("sr501-sentinel-mock.png"), "保存本机模拟布局");
    sample = "error";
    CHECK(waitUntil([&]() { return level->text().contains("未知"); }), "设备失败清空信号");
    const int errorFrames = frames;
    pump(600);
    CHECK(refresh->isActive() && frames > errorFrames && stops == 0, "读取失败继续监测和画面，不当成低电平");
    sample = "1";
    CHECK(waitUntil([&]() { return level->text().contains("OUT=1"); }), "下一次有效采样恢复");
    stale->start(20);
    CHECK(waitUntil([&]() { return level->text().contains("未知"); }), "过期信号标未知");
    CHECK(stops == 0 && refresh->isActive(), "过期不累计安静时间");
    stale->setInterval(2000); // 恢复真实过期周期，避免每个新样本在20ms后再次失效。
    sample = "0";
    CHECK(waitUntil([&]() { return level->text().contains("OUT=0"); }), "开始连续低电平");
    holdSensor = true;
    CHECK(waitUntil([&]() { return !heldSensor.isEmpty(); }), "暂缓下一条低电平回复");
    const int heldCountdownReads = reads;
    CHECK(waitUntil([&]() { return state->text().contains("约9秒"); }, 1400),
          "等待采样回复时倒计时文字仍按经过时间刷新");
    CHECK(reads == heldCountdownReads && stops == 0 && level->text().contains("OUT=0"),
          "刷新倒计时不增加请求，也不提前关闭画面");
    stale->start(20);
    CHECK(waitUntil([&]() { return level->text().contains("未知"); }), "倒计时期间过期信号标未知");
    pump(200);
    CHECK(state->text().contains("信号未知") && stops == 0,
          "过期后倒计时不能覆盖未知状态或关闭画面");
    stale->setInterval(2000);
    holdSensor = false;
    peer->write("SR501 " + heldSensor + " 0\n");
    heldSensor.clear();
    CHECK(waitUntil([&]() { return state->text().contains("约10秒"); }),
          "过期后有效低电平重新开始完整计时");
    pump(800);
    CHECK(stops == 0, "短暂低电平不关闭摄像头");
    CHECK(waitUntil([&]() { return stops == 1; }, 12000), "连续低电平10秒后释放摄像头");
    CHECK(refresh->isActive() && monitor->text().contains("停止"), "关闭画面仍保持监测");
    const int oldReads = reads;
    CHECK(waitUntil([&]() { return reads >= oldReads + 2; }), "关闭画面后继续读取");
    sample = "1";
    CHECK(waitUntil([&]() { return starts == 2 && frames > errorFrames + 3; }), "再次触发重新打开");
    holdFrame = true;
    CHECK(waitUntil([&]() { return !heldFrame.isEmpty(); }), "有一帧尚未收齐");
    monitor->click();
    pump(60);
    CHECK(stops == 1 && !refresh->isActive(), "停止时先等待在途帧，停止采样");
    writeJpeg(heldFrame);
    heldFrame.clear();
    holdFrame = false;
    CHECK(waitUntil([&]() { return stops == 2 && manual->isEnabled(); }), "帧收齐后STOP释放");
    sample = "2";
    monitor->click();
    CHECK(waitUntil([&]() { return !refresh->isActive() && manual->isEnabled(); }), "非法数值停止监测");
    CHECK(level->text().contains("未知") && starts == 2, "非法值不能触发相机");
    sample = "old";
    monitor->click();
    CHECK(waitUntil([&]() { return !refresh->isActive() && manual->isEnabled(); }), "旧服务提示并停止");
    sample = "1";
    holdSensor = true;
    monitor->click();
    CHECK(waitUntil([&]() { return !heldSensor.isEmpty(); }), "离页前在途请求");
    home->click();
    peer->write("SR501 " + heldSensor + " 1\n");
    heldSensor.clear();
    pump(100);
    CHECK(!refresh->isActive() && starts == 2, "离页后迟到高电平不启动画面");
    page->click();
    monitor->click();
    CHECK(waitUntil([&]() { return !heldSensor.isEmpty(); }), "超时前请求");
    timeout->start(20);
    CHECK(waitUntil([&]() { return !refresh->isActive() && manual->isEnabled(); }), "读取超时停止并标未知");
    CHECK(level->text().contains("未知"), "超时无旧有效值");
    heldSensor.clear();
    holdSensor = false;
    monitor->click();
    CHECK(waitUntil([&]() { return starts == 3; }), "超时后可手动重新监测");
    peer->abort();
    CHECK(waitUntil([&]() { return socket->state() == QAbstractSocket::UnconnectedState; }), "断线完成");
    CHECK(!refresh->isActive() && !monitor->isEnabled() && level->text().contains("未知") && image->pixmap().isNull(), "断线收尾，不保留活动与画面");
    qInfo() << "通过：" << checks << "项断言，哨兵采样、触发画面、低电平延时及异常收尾。";
    return EXIT_SUCCESS;
}
