#include "fpdcommunity.h"
#include <QDebug>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusInterface>
#include <QCoreApplication>
#include <QDir>
#include <QDataStream>
#include <QFile>

#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>

#define FINGERPRINT_PATH "/var/lib/sailfish-fpd-community/"
#define FINGERPRINT_FILE "fingerprints.db"

FPDCommunity::FPDCommunity()
{
    qDebug() << Q_FUNC_INFO;

    connect(&m_androidFP, &AndroidFP::enrollProgress, this, &FPDCommunity::slot_enrollProgress);
    connect(&m_androidFP, &AndroidFP::succeeded, this, &FPDCommunity::slot_succeeded);
    connect(&m_androidFP, &AndroidFP::failed, this, &FPDCommunity::slot_failed);
    connect(&m_androidFP, &AndroidFP::removed, this, &FPDCommunity::slot_removed);
    connect(&m_androidFP, &AndroidFP::acquired, this, &FPDCommunity::slot_acquired);
    connect(&m_androidFP, &AndroidFP::vendorAcquired, this, &FPDCommunity::slot_vendorAcquired);
    connect(&m_androidFP, &AndroidFP::authenticated, this, &FPDCommunity::slot_authenticated);
    connect(&m_androidFP, &AndroidFP::enumerated, this, &FPDCommunity::slot_enumerated);

    // configure cancel timer
    m_cancelTimer.setSingleShot(true);
    m_cancelTimer.setInterval(30000);
    connect(&m_cancelTimer, &QTimer::timeout, this, &FPDCommunity::slot_cancelIdentify);

    // a deferred abort is cancelled for real if no Identify takes over
    m_deferredCancelTimer.setSingleShot(true);
    m_deferredCancelTimer.setInterval(2000);
    connect(&m_deferredCancelTimer, &QTimer::timeout, this, &FPDCommunity::slot_deferredCancel);

    // set current user - nemo for now
    setUser(100000);
    registerDBus();
}

void FPDCommunity::registerDBus()
{
    qDebug() << Q_FUNC_INFO;
    if (!m_dbusRegistered)
    {
        // DBus
        QDBusConnection connection = QDBusConnection::systemBus();
        qDebug() << "Registering service on dbus" << SERVICE_NAME;
        if (!connection.registerService(SERVICE_NAME))
        {
            qCritical() << "Didnt register service";
            QCoreApplication::quit();
            return;
        }

        if (!connection.registerObject("/org/sailfishos/fingerprint1", this, QDBusConnection::ExportAllInvokables | QDBusConnection::ExportAllSignals | QDBusConnection::ExportAllProperties))
        {
            qCritical() << "Didnt register object"
                        "";
            QCoreApplication::quit();
            return;
        }
        m_dbusRegistered = true;

        qInfo() << "Sucessfully registered to dbus systemBus";
    }
}

// Code from https://stackoverflow.com/a/37489754/11848012
// with minor modifications
bool do_chown(const char *file_path, const char *user_name,
              const char *group_name)
{
    uid_t          uid;
    gid_t          gid;
    struct passwd *pwd;
    struct group  *grp;

    pwd = getpwnam(user_name);
    if (pwd == nullptr) {
        qWarning() << "Failed to get user id for" << user_name;
        return false;
    }
    uid = pwd->pw_uid;

    grp = getgrnam(group_name);
    if (grp == nullptr) {
        qWarning() << "Failed to get group id for" << group_name;
        return false;
    }
    gid = grp->gr_gid;

    if (chown(file_path, uid, gid) == -1) {
        qWarning() << "Failed to chown for" << file_path;
        return false;
    }
    return true;
}

static bool makePath(const char* path, const char* user = nullptr, const char* group = nullptr,
                     QFileDevice::Permissions permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)
{
    qDebug() << Q_FUNC_INFO << path << user << group << permissions;

    QFileInfo fi(path);
    if (fi.exists()) {
        return true;
    }

    QString leaf = fi.fileName();
    QDir dir = fi.absoluteDir();
    if (!makePath(dir.path().toLocal8Bit().data(), user, group, permissions)) {
        return false;
    }

    if (!dir.mkdir(leaf)) {
        qWarning() << "Failed to create directory" << path;
        return false;
    }

    if (!QFile::setPermissions(path, permissions))  {
        qWarning() << "Failed to set permissions" << path;
        return false;
    }

    if (user != nullptr && group != nullptr) {
      return do_chown(path, user, group);
    }

    return true;
}

void FPDCommunity::setUser(uint32_t uid)
{
    qDebug() << Q_FUNC_INFO << uid;

    // path for android store
    QString andrPath = AndroidFP::getDefaultGroupPath(uid);

    // create path if missing and set expected permissions / ownership
    if (!makePath(andrPath.toLocal8Bit().data(), "system", "system")) {
        qWarning() << "Unable to create Android store" << andrPath;
    }

    // path for storing string<->uint fp id map
    QDir fpDir(QStringLiteral(FINGERPRINT_PATH "/%1").arg(uid));
    if (!makePath(fpDir.absolutePath().toLocal8Bit().data())) {
        qWarning() << "Unable to create FPD store" << fpDir.absolutePath();
    }

    m_fingerDatabasePath = fpDir.absoluteFilePath(FINGERPRINT_FILE);

    // always setting group id to 0
    m_androidFP.setGroup(0, andrPath);

    // fingers are enumerated and loaded after that
    enumerate();
}

void FPDCommunity::saveFingers()
{
    qDebug() << Q_FUNC_INFO;

    QFile fingerprintFile(m_fingerDatabasePath);

    if (!fingerprintFile.open(QIODevice::WriteOnly)) {
        qWarning() << "Could not write " << m_fingerDatabasePath;
        return;
    }

    QDataStream out(&fingerprintFile);
    out.setVersion(QDataStream::Qt_5_6);
    out << m_fingerMap;
}

void FPDCommunity::loadFingers()
{
    qDebug() << Q_FUNC_INFO;

    QFile fingerprintFile(m_fingerDatabasePath);
    QDataStream in(&fingerprintFile);
    in.setVersion(QDataStream::Qt_5_6);

    if (!fingerprintFile.open(QIODevice::ReadOnly)) {
        qInfo() << "Could not read the file:" << m_fingerDatabasePath << "Error string:" << fingerprintFile.errorString();
        qInfo() << "Assuming empty fingerprint map";
        m_fingerMap.clear();
    } else {
        in >> m_fingerMap;
    }

    QList<uint32_t> enumeratedFingers = m_androidFP.fingerprints();

    //Check each of the loaded prints to ensure it was loaded from the store
    QList<uint32_t> keys = m_fingerMap.keys();
    for (int i = 0; i < keys.size(); ++i) {
        if (!enumeratedFingers.contains(keys[i])) {
            qWarning() << "Loaded finger " << keys[i] << m_fingerMap.value(keys[i]) << "not found in store, removing";
            m_fingerMap.remove(keys[i]);
        }
    }

    // Check whether all enumerated fingerprints are in the map
    QList<uint32_t> mapped = m_fingerMap.keys();
    for (uint32_t k: enumeratedFingers) {
        if (!mapped.contains(k)) {
            qWarning() << "Unknown fingerprint found, adding to the list:" << k;
            m_fingerMap[k] = QStringLiteral("Unknown %1").arg(k);
        }
    }

    //Save after load incase any were removed or added
    saveFingers();

    qDebug() << "Loaded finger map:" << m_fingerMap;
}

int FPDCommunity::Enroll(const QString &finger, const QDBusMessage &message)
{
    const QString caller = message.service();
    qDebug() << Q_FUNC_INFO << finger << caller;
    flushDeferredAbort();

    // check if busy by other client
    if (!m_dbusCaller.isEmpty() && m_dbusCaller != caller) {
        qWarning() << Q_FUNC_INFO << "called while busy. Caller:" << caller;
        return FPREPLY_ALREADY_BUSY;
    }

    // check if we have this ID already
    if (m_fingerMap.values().contains(finger)) {
        qWarning() << "Finger" << finger << "is in the database already";
        return FPREPLY_KEY_ALREADY_EXISTS;
    }

    if (m_state == FPSTATE_IDLE) {
        setState(FPSTATE_ENROLLING);
        m_addingFinger = finger;
        m_dbusCaller = caller;
        m_androidFP.enroll(100000); //nemo userID
        emit EnrollProgressChanged(0);
        return FPREPLY_STARTED;
    }
    return FPREPLY_ALREADY_BUSY;
}

int FPDCommunity::Identify(const QDBusMessage &message)
{
    const QString caller = message.service();
    qDebug() << Q_FUNC_INFO << caller;

    // check if busy by other client
    if (!m_dbusCaller.isEmpty() && m_dbusCaller != caller) {
        qWarning() << Q_FUNC_INFO << "called while busy. Caller:" << caller;
        return FPREPLY_ALREADY_BUSY;
    }

    if (m_state != FPSTATE_IDLE) {
        return FPREPLY_ALREADY_BUSY;
    }

    if (m_fingerMap.size() == 0) {
        return FPREPLY_NO_KEYS_AVAILABLE;
    }

    setState(FPSTATE_IDENTIFYING);
    m_dbusCaller = caller;
    m_cancelTimer.start();

    if (m_abortDeferred) {
        // take over the HAL authentication kept running by abort()
        qInfo() << "udfps: Identify takes over running authentication";
        m_abortDeferred = false;
        m_deferredCancelTimer.stop();
        if (m_pendingFinger) {
            const uint32_t finger = m_pendingFinger;
            m_pendingFinger = 0;
            QTimer::singleShot(0, this, [this, finger]() { slot_authenticated(finger); });
            return FPREPLY_STARTED;
        }
        if (m_halAuthActive)
            return FPREPLY_STARTED;
    }

    m_halAuthActive = true;
    m_androidFP.authenticate();
    return FPREPLY_STARTED;
}

void FPDCommunity::Clear(const QDBusMessage &message)
{
    const QString caller = message.service();
    qDebug() << Q_FUNC_INFO << caller;
    flushDeferredAbort();

    // check if busy by other client
    if (!m_dbusCaller.isEmpty() && m_dbusCaller != caller) {
        qWarning() << Q_FUNC_INFO << "called while busy. Caller:" << caller;
        return;
    }

    if (m_state == FPSTATE_IDLE) {
        setState(FPSTATE_REMOVING);
        m_dbusCaller = caller;
        m_androidFP.clear();
    }
}

QString FPDCommunity::GetState()
{
    qDebug() << Q_FUNC_INFO;
    return QtEnumToString(m_state);
}

QStringList FPDCommunity::GetAll()
{
    qDebug() << Q_FUNC_INFO;
    return m_fingerMap.values();
}

int FPDCommunity::Abort(const QDBusMessage &message)
{
    const QString caller = message.service();
    qDebug() << Q_FUNC_INFO << caller;

    // check if the call is initiated by some other client
    if (!m_dbusCaller.isEmpty() && m_dbusCaller != caller) {
        qWarning() << Q_FUNC_INFO << "called while busy. Caller:" << caller;
        return FPREPLY_ALREADY_IDLE;
    }

    if (m_state == FPSTATE_IDLE) {
        return FPREPLY_ALREADY_IDLE;
    }

    abort();
    return FPREPLY_STARTED;
}

int FPDCommunity::Verify(const QDBusMessage &message)
{
    const QString caller = message.service();
    qDebug() << Q_FUNC_INFO << caller;
    flushDeferredAbort();

    // check if busy by other client
    if (!m_dbusCaller.isEmpty() && m_dbusCaller != caller) {
        qWarning() << Q_FUNC_INFO << "called while busy. Caller:" << caller;
        return FPREPLY_ALREADY_IDLE;
    }

    if (m_state != FPSTATE_IDLE) {
        return FPREPLY_ALREADY_BUSY;
    }

    if (m_state == FPSTATE_IDLE) {
        setState(FPSTATE_VERIFYING);
        m_androidFP.enroll(100000); //nemo userID
        m_dbusCaller = caller;
        emit EnrollProgressChanged(0);
        return FPREPLY_STARTED;
    }

    return FPREPLY_ALREADY_BUSY;
}

int FPDCommunity::Remove(const QString &finger, const QDBusMessage &message)
{
    const QString caller = message.service();
    qDebug() << Q_FUNC_INFO << finger << caller;
    flushDeferredAbort();

    // check if busy by other client
    if (!m_dbusCaller.isEmpty() && m_dbusCaller != caller) {
        qWarning() << Q_FUNC_INFO << "called while busy. Caller:" << caller;
        return FPREPLY_ALREADY_BUSY;
    }

    if (m_state != FPSTATE_IDLE) {
        return FPREPLY_ALREADY_BUSY;
    }

    uint32_t key = 0;
    for (auto i = m_fingerMap.constBegin(); i != m_fingerMap.constEnd(); ++i) {
        if (i.value() == finger) {
            key = i.key();
            break;
        }
    }

    if (!key) {
        return FPREPLY_KEY_DOES_NOT_EXIST;
    }

    setState(FPSTATE_REMOVING);
    m_androidFP.remove(key);
    m_dbusCaller = caller;
    return FPREPLY_STARTED;
}

void FPDCommunity::abort()
{
    m_cancelTimer.stop();

    if (m_state == FPSTATE_IDENTIFYING && m_halAuthActive && m_udfps.pressed()) {
        // mce aborts its identify as soon as the display starts to power
        // up and the lockscreen then starts a new one. Cancelling the HAL
        // here would block until its capture times out and waste the
        // press, so keep the authentication running for the next Identify.
        qInfo() << "udfps: finger on sensor, deferring cancel for hand-over";
        m_abortDeferred = true;
        m_pendingFinger = 0;
        m_deferredCancelTimer.start();
        emit Aborted();
        setState(FPSTATE_IDLE);
        return;
    }

    m_halAuthActive = false;
    m_androidFP.cancel();
    emit Aborted();
    setState(FPSTATE_IDLE);
}

void FPDCommunity::slot_deferredCancel()
{
    if (!m_abortDeferred)
        return;
    qInfo() << "udfps: no Identify took over, cancelling";
    m_abortDeferred = false;
    m_pendingFinger = 0;
    if (m_halAuthActive) {
        m_halAuthActive = false;
        m_androidFP.cancel();
    }
    m_udfps.fingerUp();
}

void FPDCommunity::flushDeferredAbort()
{
    m_deferredCancelTimer.stop();
    slot_deferredCancel();
}

void FPDCommunity::enumerate()
{
    qDebug() << Q_FUNC_INFO;
    if (m_state == FPSTATE_IDLE) {
        setState(FPSTATE_ENUMERATING);
        m_androidFP.enumerate();
    }
}

void FPDCommunity::slot_enrollProgress(float pc)
{
    qDebug() << Q_FUNC_INFO << pc;
    emit EnrollProgressChanged((int)(pc * 100));

    if (m_state == FPSTATE_VERIFYING) {
        //Cancel the operation if veryfying
        emit Verified();
        abort();
    }
}

void FPDCommunity::slot_succeeded(uint32_t finger)
{
    qDebug() << Q_FUNC_INFO << finger;
    m_fingerMap[finger] = m_addingFinger;
    emit Added(m_addingFinger);
    m_addingFinger = "";
    saveFingers();
    emit ListChanged();
    setState(FPSTATE_IDLE);
}

void FPDCommunity::slot_failed(const QString &message)
{
    qDebug() << Q_FUNC_INFO << message;

    emit ErrorInfo(message); // always report error via signal
    if (message == "FINGER_NOT_RECOGNIZED") {
        if (m_state == FPSTATE_IDENTIFYING || m_abortDeferred)
            return; // giving a chance to try another finger, HAL keeps going
    } else {
        m_halAuthActive = false;
        if (m_abortDeferred) {
            // the kept authentication ended; the next Identify starts anew
            m_abortDeferred = false;
            m_deferredCancelTimer.stop();
            m_pendingFinger = 0;
            m_udfps.fingerUp();
        }
    }

    if (m_state != FPSTATE_IDLE) {
        emit Failed();
        setState(FPSTATE_IDLE);
    }
}

void FPDCommunity::slot_acquired(int info)
{
    qDebug() << Q_FUNC_INFO << info;
    AcquiredState newState;

    switch(info) {
    case ACQUIRED_GOOD:
        newState = FPACQUIRED_GOOD;
        break;
    case ACQUIRED_PARTIAL:
        newState = FPACQUIRED_PARTIAL;
        break;
    case ACQUIRED_INSUFFICIENT:
        newState = FPACQUIRED_INSUFFICIENT;
        break;
    case ACQUIRED_IMAGER_DIRTY:
        newState = FPACQUIRED_IMAGER_DIRTY;
        break;
    case ACQUIRED_TOO_SLOW:
        newState = FPACQUIRED_TOO_SLOW;
        break;
    case ACQUIRED_TOO_FAST:
        newState = FPACQUIRED_TOO_FAST;
        break;
    case ACQUIRED_VENDOR:
        newState = FPACQUIRED_UNRECOGNIZED;
        break;
    default:
        newState = FPACQUIRED_GOOD; //Should we default to good?
        break;
    }

    if (newState != m_acquired) {
        m_acquired = newState;
        emit AcquisitionInfo(QtEnumToString(m_acquired));
    }
}

void FPDCommunity::slot_vendorAcquired(int vendorCode)
{
    // OnePlus/Goodix FOD HAL: vendor code 0 = finger down on the sensor,
    // 1 = finger up (same as LineageOS FingerprintInscreen::handleAcquired).
    qDebug() << Q_FUNC_INFO << vendorCode;
    if (vendorCode == 0)
        m_udfps.fingerDown();
    else if (vendorCode == 1)
        m_udfps.fingerUp();
}

void FPDCommunity::slot_removed(uint32_t finger)
{
    qDebug() << Q_FUNC_INFO << finger;
    if (finger != 0) {
        QString f = m_fingerMap[finger];
        emit Removed(f);
        m_fingerMap.remove(finger);
    } else {
        QStringList values = m_fingerMap.values();
        for (auto f: values)
            emit Removed(f);
        m_fingerMap.clear();
    }
    saveFingers();
    emit ListChanged();
    setState(FPSTATE_IDLE);
}

void FPDCommunity::slot_authenticated(uint32_t finger)
{
    qDebug() << Q_FUNC_INFO << finger;
    m_halAuthActive = false;
    if (m_abortDeferred && m_state == FPSTATE_IDLE) {
        // matched during the hand-over gap: deliver to the next Identify
        qInfo() << "udfps: match during hand-over, holding it for the next Identify";
        m_pendingFinger = finger;
        return;
    }
    m_cancelTimer.stop();
    if (m_fingerMap.contains(finger)){
        emit Identified(m_fingerMap[finger]);
    } else {
        qWarning() << "Authenticated finger was not found in the map.  Assuming name";
        emit Identified(QStringLiteral("Unknown %1").arg(finger));
    }
    setState(FPSTATE_IDLE);
}

void FPDCommunity::slot_cancelIdentify()
{
    qDebug() << Q_FUNC_INFO;
    m_halAuthActive = false;
    m_androidFP.cancel();
    emit Failed();
    setState(FPSTATE_IDLE);
}

void FPDCommunity::slot_enumerated()
{
    qDebug() << Q_FUNC_INFO;
    loadFingers();
    emit ListChanged();
    setState(FPSTATE_IDLE);
}

void FPDCommunity::setState(FPDCommunity::State newState)
{
    qDebug() << Q_FUNC_INFO << newState;

    if (newState == FPSTATE_IDLE) {
        m_dbusCaller.clear();
        // The HAL may not report finger up after a match/abort; don't leave
        // HBM on. Not during a hand-over: the finger is still being read.
        if (!m_abortDeferred)
            m_udfps.fingerUp();
    }

    if (newState != m_state) {
        m_state = newState;
        emit StateChanged(QtEnumToString(m_state));
    }
}

