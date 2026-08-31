#include "sourcehost/source_protocol.h"
#include "sourcehost/sourcehost_client.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using listenfree::sourcehost::MessageType;
using listenfree::sourcehost::SourceHostClient;
using listenfree::sourcehost::SourceMessage;

namespace {

QString faultHostPath() {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/listenfree-sourcehost-fault-host.exe");
}

SourceMessage request(QString id, bool spawnTree = false) {
    SourceMessage message;
    message.type = MessageType::Search;
    message.requestId = std::move(id);
    if (spawnTree) message.payload.insert(QStringLiteral("spawnTree"), true);
    return message;
}

bool processExited(quint64 pid) {
#ifdef Q_OS_WIN
    if (pid == 0) return true;
    HANDLE handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) return true;
    const DWORD status = WaitForSingleObject(handle, 0);
    CloseHandle(handle);
    return status == WAIT_OBJECT_0;
#else
    Q_UNUSED(pid);
    return true;
#endif
}

void setFaultMode(const QString& value) { qputenv("LISTENFREE_FAULT_MODE", value.toUtf8()); }
void clearFaultMode() { qunsetenv("LISTENFREE_FAULT_MODE"); }

} // namespace

class SourceHostFaultTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY2(QFileInfo::exists(faultHostPath()), qPrintable(faultHostPath())); }
    void cleanup() { clearFaultMode(); qunsetenv("LISTENFREE_CHILD_PID_FILE"); }

    void startupHandshakeRequest();
    void failedAndMalformedStartup();
    void cancellationAndTimeoutAreExactlyOnce();
    void writeFailureIsTerminal();
    void crashAutoRestart();
    void repeatedLifecycleIsStable();
    void gracefulStopCleansProcessTree();
    void forcedTerminationCleansProcessTree();
};

void SourceHostFaultTests::startupHandshakeRequest() {
    setFaultMode(QStringLiteral("normal"));
    SourceHostClient client(faultHostPath());
    QSignalSpy ready(&client, &SourceHostClient::ready);
    QSignalSpy finished(&client, &SourceHostClient::requestFinished);
    QVERIFY(client.start());
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 2000);
    QVERIFY(client.request(request(QStringLiteral("request-1")), 1000));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1000);
    QCOMPARE(finished.at(0).at(1).value<SourceHostClient::RequestTerminal>(),
             SourceHostClient::RequestTerminal::Succeeded);
    client.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!client.running(), 2000);
}

void SourceHostFaultTests::failedAndMalformedStartup() {
    {
        SourceHostClient client(QCoreApplication::applicationDirPath() + QStringLiteral("/missing-sourcehost.exe"));
        QSignalSpy errors(&client, &SourceHostClient::protocolError);
        QVERIFY(client.start());
        QTRY_VERIFY_WITH_TIMEOUT(!client.running(), 2000);
        QVERIFY(errors.count() > 0);
    }
    for (const QString& fault : {QStringLiteral("bad-handshake"), QStringLiteral("delay-handshake")}) {
        setFaultMode(fault);
        SourceHostClient client(faultHostPath());
        QSignalSpy errors(&client, &SourceHostClient::protocolError);
        QVERIFY(client.start());
        QTRY_VERIFY_WITH_TIMEOUT(!client.running(), 2500);
        QVERIFY(errors.count() > 0);
        clearFaultMode();
    }
}

void SourceHostFaultTests::cancellationAndTimeoutAreExactlyOnce() {
    setFaultMode(QStringLiteral("hang"));
    SourceHostClient client(faultHostPath());
    QSignalSpy ready(&client, &SourceHostClient::ready);
    QSignalSpy timedOut(&client, &SourceHostClient::requestTimedOut);
    QSignalSpy finished(&client, &SourceHostClient::requestFinished);
    QVERIFY(client.start());
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 2000);
    QVERIFY(client.request(request(QStringLiteral("cancel-1")), 100));
    client.cancel("cancel-1");
    client.cancel("cancel-1");
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.at(0).at(1).value<SourceHostClient::RequestTerminal>(),
             SourceHostClient::RequestTerminal::Cancelled);
    QVERIFY(client.request(request(QStringLiteral("timeout-1")), 50));
    QTRY_COMPARE_WITH_TIMEOUT(timedOut.count(), 1, 1000);
    QCOMPARE(finished.count(), 2);
    QCOMPARE(finished.at(1).at(1).value<SourceHostClient::RequestTerminal>(),
             SourceHostClient::RequestTerminal::TimedOut);
    client.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!client.running(), 2000);
}

void SourceHostFaultTests::writeFailureIsTerminal() {
    setFaultMode(QStringLiteral("writefail"));
    SourceHostClient client(faultHostPath());
    QSignalSpy ready(&client, &SourceHostClient::ready);
    QSignalSpy finished(&client, &SourceHostClient::requestFinished);
    QVERIFY(client.start());
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 2000);
    QTest::qWait(100);
    const bool accepted = client.request(request(QStringLiteral("write-fail")), 200);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1000);
    const auto terminal = finished.at(0).at(1).value<SourceHostClient::RequestTerminal>();
    QVERIFY(terminal != SourceHostClient::RequestTerminal::Succeeded);
    QVERIFY(accepted || terminal == SourceHostClient::RequestTerminal::WriteFailed);
    client.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!client.running(), 2000);
}

void SourceHostFaultTests::crashAutoRestart() {
    setFaultMode(QStringLiteral("crash"));
    SourceHostClient client(faultHostPath());
    QSignalSpy ready(&client, &SourceHostClient::ready);
    QSignalSpy crashed(&client, &SourceHostClient::crashed);
    QSignalSpy restarted(&client, &SourceHostClient::restarted);
    QSignalSpy finished(&client, &SourceHostClient::requestFinished);
    QVERIFY(client.start());
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 2000);
    QVERIFY(client.request(request(QStringLiteral("crash-1")), 1000));
    QTRY_COMPARE_WITH_TIMEOUT(crashed.count(), 1, 2000);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.at(0).at(1).value<SourceHostClient::RequestTerminal>(),
             SourceHostClient::RequestTerminal::HostCrashed);
    QTRY_COMPARE_WITH_TIMEOUT(restarted.count(), 1, 3000);
    QVERIFY(client.running());
    client.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!client.running(), 2000);
}

void SourceHostFaultTests::repeatedLifecycleIsStable() {
    setFaultMode(QStringLiteral("normal"));
    SourceHostClient client(faultHostPath());
    for (int i = 0; i < 4; ++i) {
        QSignalSpy ready(&client, &SourceHostClient::ready);
        QVERIFY(client.start());
        QVERIFY(!client.start());
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 2000);
        client.stop();
        client.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!client.running(), 2000);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(client.findChildren<QTimer*>().size(), 0);
    }
}

void SourceHostFaultTests::gracefulStopCleansProcessTree() {
    setFaultMode(QStringLiteral("hang"));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pidPath = dir.filePath(QStringLiteral("child.pid"));
    qputenv("LISTENFREE_CHILD_PID_FILE", pidPath.toUtf8());
    SourceHostClient client(faultHostPath());
    QSignalSpy ready(&client, &SourceHostClient::ready);
    QVERIFY(client.start());
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 2000);
    QVERIFY(client.request(request(QStringLiteral("tree-1"), true), 1000));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(pidPath), 1000);
    QFile file(pidPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const quint64 pid = file.readAll().trimmed().toULongLong();
    QVERIFY(pid != 0);
    client.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!client.running(), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(processExited(pid), 3000);
    QVERIFY(processExited(pid));
}

void SourceHostFaultTests::forcedTerminationCleansProcessTree() {
    setFaultMode(QStringLiteral("ignore-stop"));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pidPath = dir.filePath(QStringLiteral("child.pid"));
    qputenv("LISTENFREE_CHILD_PID_FILE", pidPath.toUtf8());
    SourceHostClient client(faultHostPath());
    QSignalSpy ready(&client, &SourceHostClient::ready);
    QVERIFY(client.start());
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 2000);
    QVERIFY(client.request(request(QStringLiteral("tree-force"), true), 1000));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(pidPath), 1000);
    QFile file(pidPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const quint64 pid = file.readAll().trimmed().toULongLong();
    QVERIFY(pid != 0);
    client.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!client.running(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(processExited(pid), 3000);
    QVERIFY(processExited(pid));
}

QTEST_MAIN(SourceHostFaultTests)
#include "sourcehost_fault_tests.moc"
