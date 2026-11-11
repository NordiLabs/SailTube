# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# SailTune's account/session pieces, pinned verbatim under Shared/SailTune/ (same relative layout as
# SailTune, includes unchanged; Shared/SHARED_MANIFEST.json). Used by the library component for ACC-03/04/06/
# 08/10. Compiled, not wired: nothing calls them until library's account batch.
#   ACC-04 import parser, ACC-06 SAPISIDHASH signer (V1/V2/V3): Core/Accounts/Infrastructure/SessionImportParser,
#     SessionSigner, Sha1; types Core/Accounts/Domain/SessionTypes.h; ports Core/Accounts/Ports/*.
#   ACC-03 Sailfish Browser import (FULL): Core/Ytm/Sources/* (Gecko cookie jar reader, private scratch copy,
#     selection, enumeration, BrowserCookieJarSource). ACC-08 Set-Cookie: Core/Ytm/Transport/Domain/SetCookie.
#   ACC-10 vault: Core/Accounts/Infrastructure/SailfishCredentialVault needs Sailfish Secrets and the Secrets
#     permission, so it is built only with CONFIG+=sailtube_secrets; PrivateFile (0600 files) always.
# Do not edit the pinned files (docs/SHARING.md).
SHARED_ACCOUNTS = $$PWD/SailTune/Core
QT *= sql
HEADERS += $$SHARED_ACCOUNTS/Accounts/Domain/Account.h \
           $$SHARED_ACCOUNTS/Accounts/Domain/SessionTypes.h \
           $$SHARED_ACCOUNTS/Accounts/Infrastructure/PrivateFile.h \
           $$SHARED_ACCOUNTS/Accounts/Infrastructure/SessionBundleCodec.h \
           $$SHARED_ACCOUNTS/Accounts/Infrastructure/SessionExpiryDetector.h \
           $$SHARED_ACCOUNTS/Accounts/Infrastructure/SessionHolder.h \
           $$SHARED_ACCOUNTS/Accounts/Infrastructure/SessionImportParser.h \
           $$SHARED_ACCOUNTS/Accounts/Infrastructure/SessionSigner.h \
           $$SHARED_ACCOUNTS/Accounts/Infrastructure/Sha1.h \
           $$SHARED_ACCOUNTS/Accounts/Ports/AuthorizationPort.h \
           $$SHARED_ACCOUNTS/Accounts/Ports/CredentialVaultPort.h \
           $$SHARED_ACCOUNTS/Accounts/Ports/IdentityProbePort.h \
           $$SHARED_ACCOUNTS/Accounts/Ports/SessionCredentialsPort.h \
           $$SHARED_ACCOUNTS/Accounts/Ports/SessionMaterialSinkPort.h \
           $$SHARED_ACCOUNTS/Accounts/Ports/SessionSource.h \
           $$SHARED_ACCOUNTS/Accounts/Ports/WorkerPort.h \
           $$SHARED_ACCOUNTS/Ytm/Sources/Application/BrowserCookieJarSource.h \
           $$SHARED_ACCOUNTS/Ytm/Sources/Domain/AccountEnumeration.h \
           $$SHARED_ACCOUNTS/Ytm/Sources/Domain/BrowserImportTypes.h \
           $$SHARED_ACCOUNTS/Ytm/Sources/Domain/CookieJarSelection.h \
           $$SHARED_ACCOUNTS/Ytm/Sources/Infrastructure/GeckoCookieJarReader.h \
           $$SHARED_ACCOUNTS/Ytm/Sources/Infrastructure/PrivateScratch.h \
           $$SHARED_ACCOUNTS/Ytm/Sources/Ports/CookieJarReaderPort.h \
           $$SHARED_ACCOUNTS/Ytm/Sources/Ports/LoginProbePort.h \
           $$SHARED_ACCOUNTS/Ytm/Transport/Domain/SetCookie.h
SOURCES += $$SHARED_ACCOUNTS/Accounts/Infrastructure/PrivateFile.cpp \
           $$SHARED_ACCOUNTS/Accounts/Infrastructure/SessionBundleCodec.cpp \
           $$SHARED_ACCOUNTS/Accounts/Infrastructure/SessionExpiryDetector.cpp \
           $$SHARED_ACCOUNTS/Accounts/Infrastructure/SessionHolder.cpp \
           $$SHARED_ACCOUNTS/Accounts/Infrastructure/SessionImportParser.cpp \
           $$SHARED_ACCOUNTS/Accounts/Infrastructure/SessionSigner.cpp \
           $$SHARED_ACCOUNTS/Accounts/Infrastructure/Sha1.cpp \
           $$SHARED_ACCOUNTS/Ytm/Sources/Application/BrowserCookieJarSource.cpp \
           $$SHARED_ACCOUNTS/Ytm/Sources/Domain/AccountEnumeration.cpp \
           $$SHARED_ACCOUNTS/Ytm/Sources/Domain/CookieJarSelection.cpp \
           $$SHARED_ACCOUNTS/Ytm/Sources/Infrastructure/GeckoCookieJarReader.cpp \
           $$SHARED_ACCOUNTS/Ytm/Sources/Infrastructure/PrivateScratch.cpp \
           $$SHARED_ACCOUNTS/Ytm/Transport/Domain/SetCookie.cpp
sailtube_secrets {
    CONFIG += link_pkgconfig
    PKGCONFIG += sailfishsecrets
    DEFINES += SAILTUBE_SECRETS
    HEADERS += $$SHARED_ACCOUNTS/Accounts/Infrastructure/SailfishCredentialVault.h
    SOURCES += $$SHARED_ACCOUNTS/Accounts/Infrastructure/SailfishCredentialVault.cpp
}
