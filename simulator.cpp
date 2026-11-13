#include "simulator.h"

#include <QTcpSocket>
#include <QRandomGenerator>
#include <QtMath>

static constexpr double kPi = 3.14159265358979323846;

SimulatorCore::SimulatorCore(QObject *parent)
    : QObject(parent)
{
    connect(&m_timer, &QTimer::timeout, this, &SimulatorCore::broadcast);
}

void SimulatorCore::start(quint16 port, int intervalMs, const QHostAddress &host)
{
    if (m_running)
        stop();

    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &SimulatorCore::onNewConnection);
    if (!m_server->listen(host, port)) {
        emit statusMessage(QStringLiteral("监听失败: %1").arg(m_server->errorString()));
        m_server->deleteLater();
        m_server = nullptr;
        return;
    }

    m_stickyBuf.clear();
    m_clock.start();
    m_timer.start(qMax(10, intervalMs));
    m_running = true;
    emit statusMessage(QStringLiteral("已监听 %1:%2，%3ms/帧")
                           .arg(host.toString(), QString::number(port),
                                QString::number(intervalMs)));
}

void SimulatorCore::stop()
{
    m_timer.stop();
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    // 先通知各客户端断开；disconnected 信号随后触发 onClientDisconnected 清理
    for (QTcpSocket *c : std::as_const(m_clients))
        c->disconnectFromHost();
    m_clients.clear();
    m_stickyBuf.clear();
    m_running = false;
    emit statusMessage(QStringLiteral("已停止"));
}

void SimulatorCore::onNewConnection()
{
    while (QTcpSocket *s = m_server->nextPendingConnection()) {
        m_clients.append(s);
        connect(s, &QTcpSocket::disconnected, this, &SimulatorCore::onClientDisconnected);
        emit statusMessage(QStringLiteral("客户端接入 %1:%2")
                               .arg(s->peerAddress().toString(),
                                    QString::number(s->peerPort())));
    }
    emit clientCountChanged(m_clients.size());
}

void SimulatorCore::onClientDisconnected()
{
    if (auto *s = qobject_cast<QTcpSocket *>(sender())) {
        m_clients.removeOne(s);
        s->deleteLater();       // 必须先移除再删，否则广播会写到野指针
    }
    emit clientCountChanged(m_clients.size());
}

QByteArray SimulatorCore::makeFrame(qint64 value)
{
    // 与 STM32 9-2 工程完全一致：$DATA,<数值>#\r\n
    return "$DATA," + QByteArray::number(value) + "#\r\n";
}

qint64 SimulatorCore::nextSample()
{
    const qint64 t = m_clock.isValid() ? m_clock.elapsed() : 0;  // 未启动时时钟无效
    switch (m_wave) {
    case WaveMode::Constant:
        return 2048;
    case WaveMode::Sine:
        // 10 秒完整周期，模拟昼夜光照
        return qRound64(2048.0 + 2047.0 * qSin(2.0 * kPi * (t % 10000) / 10000.0));
    case WaveMode::Step:
        return ((t / 5000) % 2 == 0) ? 500 : 3500;
    case WaveMode::Noise: {
        double base = 2048.0 + 1500.0 * qSin(2.0 * kPi * (t % 10000) / 10000.0);
        base += QRandomGenerator::global()->bounded(-100, 101);
        return qBound<qint64>(qint64(0), qRound64(base), qint64(4095));
    }
    }
    return 2048;
}

void SimulatorCore::broadcast()
{
    QByteArray payload;
    switch (m_fault) {
    case FaultMode::None:
        payload = makeFrame(nextSample());
        sendRaw(payload);
        ++m_framesSent;
        break;

    case FaultMode::HalfPacket: {
        // 半包：前 6 字节（"$DATA,"）先走，余下 50ms 后到
        payload = makeFrame(nextSample());
        const QByteArray tail = payload.mid(6);
        sendRaw(payload.left(6));
        QTimer::singleShot(50, this, [this, tail]() { sendRaw(tail); });
        ++m_framesSent;
        break;
    }

    case FaultMode::Sticky: {
        // 粘包：攒满 3 帧一次性写出，模拟上位机 read 到"一包多帧"
        payload = makeFrame(nextSample());
        m_stickyBuf += payload;
        ++m_framesSent;
        if (m_stickyBuf.count("\r\n") >= 3) {
            sendRaw(m_stickyBuf);
            m_stickyBuf.clear();
        }
        break;
    }

    case FaultMode::Garbage: {
        // 每帧前混 5 字节垃圾（字符池保证不会碰巧凑出帧头）
        static const char pool[] = "abcdefABCDEF0123456789xyz";
        QByteArray junk;
        for (int i = 0; i < 5; ++i)
            junk += pool[QRandomGenerator::global()->bounded(0, 24)];
        payload = makeFrame(nextSample());
        sendRaw(junk);
        sendRaw(payload);
        ++m_framesSent;
        ++m_badFramesSent;
        break;
    }

    case FaultMode::EmptyValue:
        payload = "$DATA,#\r\n";
        sendRaw(payload);
        ++m_badFramesSent;
        break;
    }

    emit frameSent(payload);
}

void SimulatorCore::sendRaw(const QByteArray &data)
{
    for (QTcpSocket *c : std::as_const(m_clients)) {
        if (c->state() == QAbstractSocket::ConnectedState)
            c->write(data);
    }
}
