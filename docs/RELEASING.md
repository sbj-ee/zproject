# Releasing zproject

1. Make sure `project(zproject VERSION x.y.z)` in `CMakeLists.txt` has the
   version to release. Every PR already bumps it, adds its dated
   `CHANGELOG.md` entry and updates the version lines in `README.md`,
   `SPEC.md` and `api/openapi.yaml` (`info.version`, then
   `scripts/openapi-json.py`), so change nothing else.
2. Merge to `main`, then tag and push: `git tag vX.Y.Z && git push origin vX.Y.Z`.
3. `.github/workflows/release.yml` runs `scripts/check-version.sh --tag`. It
   then builds and tests on Ubuntu 24.04 and macOS 15 and packages
   `zproject_<v>_amd64.deb`, `zprojectd_<v>_amd64.deb` and
   `zproject-<v>-Darwin.dmg`, attesting each with build provenance. Next it
   writes `SHA256SUMS` and waits for approval of the **Publish** job (the
   protected `release` environment; reviewer sbj-ee). Once approved, it
   **signs** `SHA256SUMS` with minisign (`SHA256SUMS.minisig`, trusted comment
   `zproject vX.Y.Z SHA256SUMS`), checks the signature against the public key
   in `core/UpdateSigningKey.cpp` at the tagged commit, and creates the GitHub
   Release. If the signing secrets or the public key are missing, the publish
   job stops and nothing is released.

## Signing key (one-time setup)

zproject uses **the same minisign key as zterminal and ztodo** (key ID
`32960BBA77B36F1F`). Signatures can't be moved from one app to another,
because the signed comment names the app and the tag. The release job needs
two secrets in the `release` environment of `sbj-ee/zproject`, and they are
never committed:

```sh
gh secret set MINISIGN_SECRET_KEY -R sbj-ee/zproject --env release < zterminal-release.key
gh secret set MINISIGN_PASSWORD   -R sbj-ee/zproject --env release   # paste the password when asked
```

The environment must require sbj-ee's review and allow deployments only from
`v*` tags. Keep no repository-wide copy of these secrets. On each release,
approve the waiting **Publish** job (Actions → the run → Review deployments).

To give zproject its own key instead:

1. Run `minisign -G -p zproject-release.pub -s zproject-release.key` on your
   own machine, never in CI or on a shared box.
2. Store the key and its password in your password manager, with an offline
   backup.
3. Set the two secrets as shown above.
4. Copy the second line of the `.pub` file into `kUpdateSigningPublicKey` in
   `core/UpdateSigningKey.cpp`, and replace the key in the README and in this
   file.

## Rotating the key

Generate a new key pair, update the secrets and `core/UpdateSigningKey.cpp`,
then release. Once the in-app updater exists, clients that still hold the old
key can't verify releases signed with the new one. Users then have to install
that release by hand.

## Current release key

Key ID `32960BBA77B36F1F`:

```
RWQfb7N3uguWMhJ7jsE/Z0hZ7pDWHQ9OEeWSBTlN/R5bxQ8HWSMxp/l9
```
