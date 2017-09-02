#include "DaemonManager.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <QUrl>
#include <QtConcurrent/QtConcurrent>
#include <QApplication>
#include <QProcess>
#include <QTime>

namespace {
    static const int DAEMON_START_TIMEOUT_SECONDS = 30;
}

DaemonManager * DaemonManager::m_instance = nullptr;
QStringList DaemonManager::m_clArgs;

DaemonManager *DaemonManager::instance(const QStringList *args)
{
    if (!m_instance) {
        m_instance = new DaemonManager;
        // store command line arguments for later use
        m_clArgs = *args;
        m_clArgs.removeFirst();
    }

    return m_instance;
}

bool DaemonManager::start(const QString &flags, bool testnet)
{
    // prepare command line arguments and pass to swapcoinzd
    QStringList arguments;

    // Start daemon with --detach flag on non-windows platforms
#ifndef Q_OS_WIN
    arguments << "--detach";
#endif

    if(testnet)
        arguments << "--testnet";

    foreach (const QString &str, m_clArgs) {
          qDebug() << QString(" [%1] ").arg(str);
          if (!str.isEmpty())
            arguments << str;
    }

    // Custom startup flags for daemon
    foreach (const QString &str, flags.split(" ")) {
          qDebug() << QString(" [%1] ").arg(str);
          if (!str.isEmpty())
            arguments << str;
    }

    arguments << "--check-updates" << "disabled";

    qDebug() << "starting swapcoinzd " + m_monerod;
    qDebug() << "With command line arguments " << arguments;

    m_daemon = new QProcess();
    initialized = true;

    // Connect output slots
    connect (m_daemon, SIGNAL(readyReadStandardOutput()), this, SLOT(printOutput()));
    connect (m_daemon, SIGNAL(readyReadStandardError()), this, SLOT(printError()));

    // Start swapcoinzd
    bool started = m_daemon->startDetached(m_monerod, arguments);

    // add state changed listener
    connect(m_daemon,SIGNAL(stateChanged(QProcess::ProcessState)),this,SLOT(stateChanged(QProcess::ProcessState)));

    if (!started) {
        qDebug() << "Daemon start error: " + m_daemon->errorString();
        emit daemonStartFailure();
        return false;
    }

    // Start start watcher
    QFuture<bool> future = QtConcurrent::run(this, &DaemonManager::startWatcher, testnet);
    QFutureWatcher<bool> * watcher = new QFutureWatcher<bool>();
    connect(watcher, &QFutureWatcher<bool>::finished,
            this, [this, watcher]() {
        QFuture<bool> future = watcher->future();
        watcher->deleteLater();
        if(future.result())
            emit daemonStarted();
        else
            emit daemonStartFailure();
    });
    watcher->setFuture(future);


    return true;
}

bool DaemonManager::stop(bool testnet)
{
    QString message;
    sendCommand("exit",testnet,message);
    qDebug() << message;

    // Start stop watcher - Will kill if not shutting down
    QFuture<bool> future = QtConcurrent::run(this, &DaemonManager::stopWatcher, testnet);
    QFutureWatcher<bool> * watcher = new QFutureWatcher<bool>();
    connect(watcher, &QFutureWatcher<bool>::finished,
            this, [this, watcher]() {
        QFuture<bool> future = watcher->future();
        watcher->deleteLater();
        if(future.result()) {
            emit daemonStopped();
        }
    });
    watcher->setFuture(future);

    return true;
}

bool DaemonManager::startWatcher(bool testnet) const
{
    // Check if daemon is started every 2 seconds
    QTime timer;
    timer.restart();
    while(true && !m_app_exit && timer.elapsed() / 1000 < DAEMON_START_TIMEOUT_SECONDS  ) {
        QThread::sleep(2);
        if(!running(testnet)) {
            qDebug() << "daemon not running. checking again in 2 seconds.";
        } else {
            qDebug() << "daemon is started. Waiting 5 seconds to let daemon catch up";
            QThread::sleep(5);
            return true;
        }
    }
    return false;
}

bool DaemonManager::stopWatcher(bool testnet) const
{
    // Check if daemon is running every 2 seconds. Kill if still running after 10 seconds
    int counter = 0;
    while(true && !m_app_exit) {
        QThread::sleep(2);
        counter++;
        if(running(testnet)) {
            qDebug() << "Daemon still running.  " << counter;
            if(counter >= 5) {
                qDebug() << "Killing it! ";
#ifdef Q_OS_WIN
                QProcess::execute("taskkill /F /IM swapcoinzd.exe");
#else
                QProcess::execute("pkill swapcoinzd");
#endif
            }

        } else
            return true;
    }
    return false;
}


void DaemonManager::stateChanged(QProcess::ProcessState state)
{
    qDebug() << "STATE CHANGED: " << state;
    if (state == QProcess::NotRunning) {
        emit daemonStopped();
    }
}

void DaemonManager::printOutput()
{
    QByteArray byteArray = m_daemon->readAllStandardOutput();
    QStringList strLines = QString(byteArray).split("\n");

    foreach (QString line, strLines) {
        emit daemonConsoleUpdated(line);
        qDebug() << "Daemon: " + line;
    }
}

void DaemonManager::printError()
{
    QByteArray byteArray = m_daemon->readAllStandardError();
    QStringList strLines = QString(byteArray).split("\n");

    foreach (QString line, strLines) {
        emit daemonConsoleUpdated(line);
        qDebug() << "Daemon ERROR: " + line;
    }
}

bool DaemonManager::running(bool testnet) const
{ 
    QString status;
    sendCommand("status",testnet, status);
    qDebug() << status;
    // `./swapcoinzd status` returns BUSY when syncing.
    // Treat busy as connected, until fixed upstream.
    if (status.contains("Height:") || status.contains("BUSY") ) {
        return true;
    }
    return false;
}
bool DaemonManager::sendCommand(const QString &cmd,bool testnet) const
{
    QString message;
    return sendCommand(cmd, testnet, message);
}

bool DaemonManager::sendCommand(const QString &cmd,bool testnet, QString &message) const
{
    QProcess p;
    QString external_cmd = m_monerod + " " + cmd;
    qDebug() << "sending external cmd: " << external_cmd;

    // Add testnet flag if needed
    if (testnet)
        external_cmd += " --testnet";
    external_cmd += "\n";

    p.setWorkingDirectory(QApplication::applicationDirPath());
    p.start(external_cmd);

    bool started = p.waitForFinished(-1);
    message = p.readAllStandardOutput();
    emit daemonConsoleUpdated(message);
    return started;
}

void DaemonManager::exit()
{
    qDebug("DaemonManager: exit()");
    m_app_exit = true;
}

DaemonManager::DaemonManager(QObject *parent)
    : QObject(parent)
{

    // Platform depetent path to swapcoinzd
#ifdef Q_OS_WIN
    m_monerod = "./swapcoinzd.exe";
#elif defined(Q_OS_UNIX)
    m_monerod = "./swapcoinzd";
#endif

    if (m_monerod.length() == 0) {
        qCritical() << "no daemon binary defined for current platform";
        m_has_daemon = false;
    }
}
