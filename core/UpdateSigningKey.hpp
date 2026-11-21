#pragma once

namespace zproject {

// The minisign public key that signs SHA256SUMS of every zproject release
// (SPEC.md §20; key ID 32960BBA77B36F1F, shared with zterminal and ztodo).
// The release workflow refuses to publish unless the signature verifies
// against this key at the tagged commit; the in-app updater (SPEC.md §21)
// will use it too. Public by design.
extern const char kUpdateSigningPublicKey[];

}  // namespace zproject
