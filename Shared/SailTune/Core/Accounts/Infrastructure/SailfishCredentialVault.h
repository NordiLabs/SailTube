// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "../Ports/CredentialVaultPort.h"

namespace nordtile { namespace music { namespace accounts { namespace infrastructure {
// Optional platform adapter. Define SAILTUNE_WITH_SAILFISH_SECRETS and link
// sailfishsecrets only after SDK/runtime verification. Without it, all calls
// fail closed; there is no fallback to a plaintext settings file.
// Construct/use on a worker thread. Calls may wait for system interaction.
class SailfishCredentialVault final : public ports::CredentialVaultPort {
public:
    domain::Result store(const std::string &, const ports::SecretBytes &) override;
    domain::Result load(const std::string &, ports::SecretBytes *) override;
    domain::Result remove(const std::string &) override;
};
}}}}
