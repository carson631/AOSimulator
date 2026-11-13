#include <QtTest>
#include <QTcpSocket>
#include <QSignalSpy>
#include <QRegularExpression>
#include <QPushButton>
#include <QSpinBox>

#include "simulator.h"
#include "mainwindow.h"

/*
 * 测试策略：不做 mock，全部走真实 TCP 环回（127.0.0.1）。
 * 理由是 SimulatorCore 的复杂度就在"字节流形状"上——半包/粘包/垃圾前缀
 * 只有在真实 socket 上才有意义，mock 掉 QTcpSocket 等于什么都没测。
 */
class TestSimulator : public QObject
{
    Q_OBJECT

private slots:
    // 纯函数（不需要网络）
    void makeFrameFormat();
    void nextSampleRanges();

    // 连接管理
    void serverAcceptsClients();
    void clientDisconnectUpdate();

    // 字节流形状
    void framesOnWireConstant();
    void halfPacketReassembles();
    void stickyBurstsOfThree();
    void garbagePrefix();
    void emptyValueFrame();
    void stopStopsStream();

    // UI 集成冒烟
    void uiSmokeStartAndStream();

private:
    static QTcpSocket *connectTo(quint16 port);
    static QByteArray drain(QTcpSocket *s);
};

QTcpSocket *TestSimulator::connectTo(quint16 port)
{
    auto *s = new QTcpSocket;
    s->connectToHost(QHostAddress::LocalHost, port);
    if (!s->waitForConnected(3000)) {
        delete s;
        return nullptr;
    }
    return s;
}

QByteArray TestSimulator::drain(QTcpSocket *s)
{
    QByteArray out;
    while (s->bytesAvailable() > 0)
        out += s->readAll();
    return out;
}

void TestSimulator::makeFrameFormat()
{
    // 必须与 STM32 9-2 工程逐字节一致：$DATA,<值>#\r\n
    QCOMPARE(SimulatorCore::makeFrame(2048), QByteArray("$DATA,2048#\r\n"));
    QCOMPARE(SimulatorCore::makeFrame(0), QByteArray("$DATA,0#\r\n"));
    QCOMPARE(SimulatorCore::makeFrame(4095), QByteArray("$DATA,4095#\r\n"));
}

void TestSimulator::nextSampleRanges()
{
    SimulatorCore core;

    core.setWaveform(WaveMode::Constant);
    QCOMPARE(core.nextSample(), qint64(2048));

    // 未启动时时钟无效，按 t=0 计算：正弦从 2048 起步、阶跃从 500 起步
    core.setWaveform(WaveMode::Sine);
    QCOMPARE(core.nextSample(), qint64(2048));
    core.setWaveform(WaveMode::Step);
    QCOMPARE(core.nextSample(), qint64(500));

    // 噪声必须落在 ADC 量程内
    core.setWaveform(WaveMode::Noise);
    for (int i = 0; i < 200; ++i) {
        const qint64 v = core.nextSample();
        QVERIFY2(v >= 0 && v <= 4095, qPrintable(QStringLiteral("越界值 %1").arg(v)));
    }
}

void TestSimulator::serverAcceptsClients()
{
    SimulatorCore core;
    core.start(9301, 100);
    QVERIFY(core.isRunning());

    QTcpSocket *c1 = connectTo(9301);
    QVERIFY(c1);
    QTRY_COMPARE_WITH_TIMEOUT(core.clientCount(), 1, 3000);

    QTcpSocket *c2 = connectTo(9301);
    QVERIFY(c2);
    QTRY_COMPARE_WITH_TIMEOUT(core.clientCount(), 2, 3000);

    c1->disconnectFromHost();
    QTRY_COMPARE_WITH_TIMEOUT(core.clientCount(), 1, 3000);

    c2->disconnectFromHost();
    QTRY_COMPARE_WITH_TIMEOUT(core.clientCount(), 0, 3000);

    core.stop();
    delete c1;
    delete c2;
}

void TestSimulator::clientDisconnectUpdate()
{
    SimulatorCore core;
    core.setWaveform(WaveMode::Constant);
    core.start(9307, 80);

    QTcpSocket *c1 = connectTo(9307);
    QTcpSocket *c2 = connectTo(9307);
    QVERIFY(c1 && c2);
    QTRY_COMPARE_WITH_TIMEOUT(core.clientCount(), 2, 3000);

    c1->disconnectFromHost();
    QTRY_COMPARE_WITH_TIMEOUT(core.clientCount(), 1, 3000);

    // 剩下的客户端必须继续收到数据（断开不能影响其他连接）
    QTRY_VERIFY_WITH_TIMEOUT(c2->bytesAvailable() > 0, 3000);
    QVERIFY(!drain(c2).isEmpty());

    core.stop();
    delete c1;
    delete c2;
}

void TestSimulator::framesOnWireConstant()
{
    SimulatorCore core;
    core.setWaveform(WaveMode::Constant);
    core.start(9302, 50);

    QTcpSocket *c = connectTo(9302);
    QVERIFY(c);

    QTRY_VERIFY_WITH_TIMEOUT(c->bytesAvailable() > 0, 3000);
    QTest::qWait(400);

    const QByteArray data = drain(c);
    const int framesOnWire = data.count("$DATA,");
    QVERIFY2(framesOnWire >= 5, qPrintable(QStringLiteral("只收到 %1 帧").arg(framesOnWire)));

    // 恒值 2048 → 每帧固定 13 字节，整条流必须是它整除
    QCOMPARE(data.size(), framesOnWire * 13);
    QCOMPARE(data.left(13), QByteArray("$DATA,2048#\r\n"));
    // 收发计数允许在边界差 1-2 帧（最后几个 tick 可能还在缓冲区）
    QVERIFY(qAbs(framesOnWire - int(core.framesSent())) <= 2);

    core.stop();
    delete c;
}

void TestSimulator::halfPacketReassembles()
{
    SimulatorCore core;
    core.setWaveform(WaveMode::Constant);
    core.setFaultMode(FaultMode::HalfPacket);

    QSignalSpy spy(&core, &SimulatorCore::frameSent);
    core.start(9303, 50);

    QTcpSocket *c = connectTo(9303);
    QVERIFY(c);

    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 4, 4000);
    QTest::qWait(150);      // 等最后一个半包的尾巴（50ms 延迟）到达

    const QByteArray data = drain(c);

    // 期望流 = 按发出顺序拼接的完整帧
    QByteArray expected;
    for (int i = 0; i < spy.count(); ++i)
        expected += spy.at(i).at(0).toByteArray();

    // 半包只拆不改：重组出的字节流必须与发送序列逐字节一致（前缀比对，容忍尾部未到）
    const int n = qMin(data.size(), expected.size());
    QVERIFY2(n > 0, "没收到任何数据");
    QCOMPARE(data.left(n), expected.left(n));

    core.stop();
    delete c;
}

void TestSimulator::stickyBurstsOfThree()
{
    SimulatorCore core;
    core.setWaveform(WaveMode::Constant);
    core.setFaultMode(FaultMode::Sticky);
    core.start(9304, 40);

    QTcpSocket *c = connectTo(9304);
    QVERIFY(c);

    QList<int> chunks;      // 每次 readyRead 收到的字节数
    connect(c, &QTcpSocket::readyRead, c, [&chunks, c]() {
        const QByteArray chunk = c->readAll();
        if (!chunk.isEmpty())
            chunks << chunk.size();
    });

    QTRY_VERIFY_WITH_TIMEOUT(core.framesSent() >= 9, 4000);   // 至少 3 个包
    QTest::qWait(120);

    QVERIFY2(chunks.size() >= 2, qPrintable(QStringLiteral("只收到 %1 次读取").arg(chunks.size())));
    for (int size : chunks) {
        QCOMPARE(size % 13, 0);       // 整帧的整数倍
        QVERIFY2(size >= 39, qPrintable(QStringLiteral("出现非粘包的碎包: %1 字节").arg(size)));
    }

    core.stop();
    delete c;
}

void TestSimulator::garbagePrefix()
{
    SimulatorCore core;
    core.setWaveform(WaveMode::Constant);
    core.setFaultMode(FaultMode::Garbage);
    core.start(9305, 80);

    QTcpSocket *c = connectTo(9305);
    QVERIFY(c);
    QTest::qWait(500);

    const QByteArray data = drain(c);
    static const QRegularExpression frameRe(QStringLiteral("\\$DATA,\\d+#\\r\\n"));

    int frames = 0;
    auto it = frameRe.globalMatch(QString::fromLatin1(data));
    while (it.hasNext()) {
        it.next();
        ++frames;
    }
    QVERIFY2(frames >= 3, qPrintable(QStringLiteral("只收到 %1 帧").arg(frames)));

    // 把完整帧全部剔除，剩下的应该正好是 5 字节 × 帧数 的垃圾
    // 注意：QByteArray 没有 replace(QRegularExpression) 重载（那是 QString 的），
    // 只能按匹配区间手工裁剪。
    QByteArray rest;
    int last = 0;
    auto it2 = frameRe.globalMatch(QString::fromLatin1(data));
    while (it2.hasNext()) {
        const auto m = it2.next();
        rest += data.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
    }
    rest += data.mid(last);
    QCOMPARE(rest.size(), frames * 5);

    static const QByteArray pool("abcdefABCDEF0123456789xyz");
    for (char ch : rest)
        QVERIFY2(pool.contains(ch), "垃圾字节超出了预设字符池");

    core.stop();
    delete c;
}

void TestSimulator::emptyValueFrame()
{
    SimulatorCore core;
    core.setWaveform(WaveMode::Constant);
    core.setFaultMode(FaultMode::EmptyValue);
    core.start(9306, 60);

    QTcpSocket *c = connectTo(9306);
    QVERIFY(c);
    QTest::qWait(400);

    const QByteArray data = drain(c);
    QVERIFY2(data.count("$DATA,#\r\n") >= 3,
             qPrintable(QStringLiteral("坏帧数不足: %1").arg(data.count("$DATA,#\r\n"))));
    QCOMPARE(core.framesSent(), qint64(0));       // 没有产生任何有效帧
    QVERIFY(core.badFramesSent() >= 3);

    core.stop();
    delete c;
}

void TestSimulator::stopStopsStream()
{
    SimulatorCore core;
    core.setWaveform(WaveMode::Constant);
    core.start(9308, 50);

    QTcpSocket *c = connectTo(9308);
    QVERIFY(c);
    QTRY_VERIFY_WITH_TIMEOUT(c->bytesAvailable() > 0, 3000);

    core.stop();
    QVERIFY(!core.isRunning());
    QCOMPARE(core.clientCount(), 0);

    QTest::qWait(200);
    drain(c);                                   // 清掉停机前在途的字节
    QTest::qWait(300);
    QCOMPARE(c->bytesAvailable(), 0);           // 之后不应再有新数据

    delete c;
}

void TestSimulator::uiSmokeStartAndStream()
{
    MainWindow w;
    w.show();

    auto *btnStart = w.findChild<QPushButton *>(QStringLiteral("btnStart"));
    auto *btnStop = w.findChild<QPushButton *>(QStringLiteral("btnStop"));
    auto *spinInterval = w.findChild<QSpinBox *>(QStringLiteral("spinInterval"));
    auto *spinPort = w.findChild<QSpinBox *>(QStringLiteral("spinPort"));
    QVERIFY(btnStart && btnStop && spinInterval && spinPort);
    QVERIFY(!btnStop->isEnabled());

    spinInterval->setValue(50);
    btnStart->click();

    QVERIFY(w.core()->isRunning());
    QVERIFY(!spinPort->isEnabled());            // 运行中禁止改端口
    QVERIFY(btnStop->isEnabled());

    QTcpSocket *c = connectTo(static_cast<quint16>(spinPort->value()));
    QVERIFY(c);
    QTRY_COMPARE_WITH_TIMEOUT(w.core()->clientCount(), 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(c->bytesAvailable() > 0, 3000);

    btnStop->click();
    QVERIFY(!w.core()->isRunning());
    QVERIFY(spinPort->isEnabled());
    QVERIFY(btnStart->isEnabled());

    delete c;
}

QTEST_MAIN(TestSimulator)
#include "test_simulator.moc"
