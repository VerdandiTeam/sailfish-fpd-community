#include "kmhmac.h"

#include <QByteArray>
#include <QDebug>
#include <QFile>
#include <QThread>

#include <gbinder.h>

#include <cstring>

// Same flag the old droid-configs km-hmac-agree.py used, so the two never
// both run in one boot.
#define KM_HMAC_FLAG "/run/km-hmac-agreed"

#define KM_FQNAME "android.hardware.keymaster@4.0::IKeymasterDevice/default"
#define KM_IFACE  "android.hardware.keymaster@4.0::IKeymasterDevice"

// IKeymasterDevice@4.0 transaction codes (method order in the .hal)
#define KM_GET_HMAC_SHARING_PARAMETERS 2
#define KM_COMPUTE_SHARED_HMAC         3

#define KM_WAIT_SECONDS 30

// struct HmacSharingParameters { vec<uint8_t> seed; uint8_t[32] nonce; }
struct HmacSharingParameters {
    GBinderHidlVec seed;
    guint8 nonce[32];
};
static_assert(sizeof(HmacSharingParameters) == 48, "HIDL layout");

// Reads the HIDL status and the keymaster ErrorCode at the start of a reply.
static bool readStatus(GBinderReader *reader, const char *what)
{
    gint32 status = -1, error = -1;
    if (!gbinder_reader_read_int32(reader, &status) ||
        !gbinder_reader_read_int32(reader, &error) ||
        status != 0 || error != 0) {
        qWarning() << "km-hmac:" << what << "failed, status" << status << "error" << error;
        return false;
    }
    return true;
}

void agreeKeymasterHmac()
{
    if (QFile::exists(KM_HMAC_FLAG)) {
        qInfo() << "km-hmac: already agreed this boot";
        return;
    }

    GBinderServiceManager *sm = gbinder_servicemanager_new("/dev/hwbinder");
    if (!sm) {
        qWarning() << "km-hmac: no hwbinder service manager";
        return;
    }

    GBinderRemoteObject *remote = nullptr;
    for (int i = 0; i < KM_WAIT_SECONDS && !remote; i++) {
        int status = 0;
        remote = gbinder_servicemanager_get_service_sync(sm, KM_FQNAME, &status);
        if (!remote)
            QThread::sleep(1);
    }
    if (!remote) {
        qWarning() << "km-hmac: keymaster 4.0 not found, skipping";
        gbinder_servicemanager_unref(sm);
        return;
    }

    GBinderClient *km = gbinder_client_new(remote, KM_IFACE);
    QByteArray seed;
    guint8 nonce[32];
    bool ok = false;

    // getHmacSharingParameters() generates (ErrorCode, HmacSharingParameters)
    {
        GBinderLocalRequest *req = gbinder_client_new_request(km);
        int status = 0;
        GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(
            km, KM_GET_HMAC_SHARING_PARAMETERS, req, &status);
        gbinder_local_request_unref(req);
        if (reply) {
            GBinderReader reader;
            gbinder_remote_reply_init_reader(reply, &reader);
            if (readStatus(&reader, "getHmacSharingParameters")) {
                auto params = static_cast<const HmacSharingParameters *>(
                    gbinder_reader_read_hidl_struct1(&reader, sizeof(HmacSharingParameters)));
                if (params) {
                    if (params->seed.count)
                        seed = QByteArray(static_cast<const char *>(params->seed.data.ptr),
                                          int(params->seed.count));
                    memcpy(nonce, params->nonce, sizeof(nonce));
                    ok = true;
                }
            }
            gbinder_remote_reply_unref(reply);
        } else {
            qWarning() << "km-hmac: getHmacSharingParameters transaction failed" << status;
        }
    }

    // computeSharedHmac(vec<HmacSharingParameters>) with our single entry
    if (ok) {
        ok = false;
        HmacSharingParameters elem;
        memset(&elem, 0, sizeof(elem));
        elem.seed.data.ptr = seed.isEmpty() ? nullptr : seed.constData();
        elem.seed.count = guint32(seed.size());
        memcpy(elem.nonce, nonce, sizeof(nonce));

        GBinderHidlVec vec;
        memset(&vec, 0, sizeof(vec));
        vec.data.ptr = &elem;
        vec.count = 1;

        GBinderLocalRequest *req = gbinder_client_new_request(km);
        GBinderWriter writer;
        gbinder_local_request_init_writer(req, &writer);
        GBinderParent vecParent;
        vecParent.index = gbinder_writer_append_buffer_object(&writer, &vec, sizeof(vec));
        vecParent.offset = GBINDER_HIDL_VEC_BUFFER_OFFSET;
        GBinderParent elemParent;
        elemParent.index = gbinder_writer_append_buffer_object_with_parent(
            &writer, &elem, sizeof(elem), &vecParent);
        elemParent.offset = 0; // seed is the first field
        gbinder_writer_append_buffer_object_with_parent(
            &writer, elem.seed.data.ptr, seed.size(), &elemParent);

        int status = 0;
        GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(
            km, KM_COMPUTE_SHARED_HMAC, req, &status);
        gbinder_local_request_unref(req);
        if (reply) {
            GBinderReader reader;
            gbinder_remote_reply_init_reader(reply, &reader);
            ok = readStatus(&reader, "computeSharedHmac");
            gbinder_remote_reply_unref(reply);
        } else {
            qWarning() << "km-hmac: computeSharedHmac transaction failed" << status;
        }
    }

    gbinder_client_unref(km);
    gbinder_servicemanager_unref(sm);

    if (ok) {
        QFile flag(KM_HMAC_FLAG);
        if (!flag.open(QIODevice::WriteOnly))
            qWarning() << "km-hmac: cannot create" << KM_HMAC_FLAG;
        qInfo() << "km-hmac: shared HMAC key computed";
    }
}
