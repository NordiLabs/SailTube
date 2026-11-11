// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SailfishCredentialVault.h"
#include <algorithm>

#ifdef SAILTUNE_WITH_SAILFISH_SECRETS
#include <Secrets/deletesecretrequest.h>
#include <Secrets/secret.h>
#include <Secrets/secretmanager.h>
#include <Secrets/storedsecretrequest.h>
#include <Secrets/storesecretrequest.h>
#include <QByteArray>
#include <QString>
#endif

namespace nordtile { namespace music { namespace accounts { namespace infrastructure {
namespace {
domain::Result fail(domain::ErrorCode code, const char *message) {
    domain::Result result; result.code = code; result.message = message; return result;
}
bool validHandle(const std::string &handle) {
    return handle.size() >= 16 && handle.size() <= 128 &&
        std::all_of(handle.begin(), handle.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_';
        });
}
#ifdef SAILTUNE_WITH_SAILFISH_SECRETS
namespace Secrets = Sailfish::Secrets;
Secrets::Secret::Identifier identifier(const std::string &handle) {
    return Secrets::Secret::Identifier(QStringLiteral("harbour-sailtune.oauth.") + QString::fromStdString(handle),
        QString(), Secrets::SecretManager::DefaultEncryptedStoragePluginName);
}
domain::Result translated(const Secrets::Result &result) {
    if (result.code() == Secrets::Result::Succeeded) return domain::Result();
    return fail(domain::ErrorCode::SecureStorageFailure, "The system could not complete secure credential storage.");
}
#else
domain::Result unavailable() {
    return fail(domain::ErrorCode::SecureStorageUnavailable, "Sailfish Secrets support is not enabled in this build.");
}
#endif
}
domain::Result SailfishCredentialVault::store(const std::string &handle, const ports::SecretBytes &bytes) {
    if (!validHandle(handle) || bytes.bytes().empty() || bytes.bytes().size() > 65536)
        return fail(domain::ErrorCode::InvalidArgument, "Invalid credential storage request.");
#ifdef SAILTUNE_WITH_SAILFISH_SECRETS
    Secrets::SecretManager manager;
    Secrets::Secret secret(identifier(handle));
    secret.setData(QByteArray(reinterpret_cast<const char *>(bytes.bytes().data()), int(bytes.bytes().size())));
    secret.setType(Secrets::Secret::TypeBlob);
    Secrets::StoreSecretRequest request;
    request.setManager(&manager);
    request.setSecretStorageType(Secrets::StoreSecretRequest::StandaloneDeviceLockSecret);
    request.setDeviceLockUnlockSemantic(Secrets::SecretManager::DeviceLockVerifyLock);
    request.setAccessControlMode(Secrets::SecretManager::OwnerOnlyMode);
    request.setEncryptionPluginName(Secrets::SecretManager::DefaultEncryptionPluginName);
    request.setUserInteractionMode(Secrets::SecretManager::SystemInteraction);
    request.setSecret(secret);
    request.startRequest(); request.waitForFinished();
    return translated(request.result());
#else
    return unavailable();
#endif
}
domain::Result SailfishCredentialVault::load(const std::string &handle, ports::SecretBytes *out) {
    if (out) out->clear();
    if (!validHandle(handle) || !out) return fail(domain::ErrorCode::InvalidArgument, "Invalid credential retrieval request.");
#ifdef SAILTUNE_WITH_SAILFISH_SECRETS
    Secrets::SecretManager manager;
    Secrets::StoredSecretRequest request;
    request.setManager(&manager); request.setIdentifier(identifier(handle));
    request.setUserInteractionMode(Secrets::SecretManager::SystemInteraction);
    request.startRequest(); request.waitForFinished();
    const auto result = translated(request.result());
    if (!result.ok()) return result;
    QByteArray data = request.secret().data();
    if (data.isEmpty() || data.size() > 65536)
        return fail(domain::ErrorCode::SecureStorageFailure, "Stored credential data is invalid.");
    out->assign(reinterpret_cast<const unsigned char *>(data.constData()), std::size_t(data.size()));
    data.fill('\0');
    return domain::Result();
#else
    return unavailable();
#endif
}
domain::Result SailfishCredentialVault::remove(const std::string &handle) {
    if (!validHandle(handle)) return fail(domain::ErrorCode::InvalidArgument, "Invalid credential removal request.");
#ifdef SAILTUNE_WITH_SAILFISH_SECRETS
    Secrets::SecretManager manager;
    Secrets::DeleteSecretRequest request;
    request.setManager(&manager); request.setIdentifier(identifier(handle));
    request.setUserInteractionMode(Secrets::SecretManager::SystemInteraction);
    request.startRequest(); request.waitForFinished();
    return translated(request.result());
#else
    return unavailable();
#endif
}
}}}}
