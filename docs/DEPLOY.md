# Deploying zproject

This guide installs a release on one server host (Pop!_OS or Ubuntu 24.04,
amd64), Linux clients (24.04, amd64) and macOS clients (Apple Silicon). It
follows what the packages actually ship: the `zprojectd` postinst, the
systemd units and the default `/etc/zproject/zprojectd.conf`.

Placeholders:

- `<server-host>.<tailnet>.ts.net`: the server's MagicDNS name. It goes only
  in the server's local config and the clients' `cli.ini`, never in this
  repository.
- `<client-host>`: a client's name on the tailnet.
- `<machine>`: a token name, one per machine.
- `<ID>`: an id the CLI prints. Ids are not secrets; tokens are.
- `X.Y.Z`: the release version.

Steps marked **(sudo)** need root. Everything else runs as your own user.
Scratch files go in `~/agent-work/zproject-X.Y.Z/`.

## 1. What gets installed

| Asset | Contents |
|---|---|
| `zprojectd_X.Y.Z_amd64.deb` | `/usr/bin/zprojectd`, `/etc/zproject/zprojectd.conf` (conffile), `zprojectd.service`, `zprojectd-backup.service`, `zprojectd-backup.timer` |
| `zproject_X.Y.Z_amd64.deb` | `/usr/bin/zproject-cli`, `/usr/bin/zproject` (desktop placeholder), desktop file, icons |
| `zproject-X.Y.Z-Darwin.dmg` | `zproject.app` with `Contents/MacOS/zproject-cli`; ad-hoc signed, not notarized |
| `SHA256SUMS`, `SHA256SUMS.minisig` | checksums, and their minisign signature |

The `zprojectd` package:

- **postinst:** creates the `zproject` system user and group, creates
  `/var/lib/zproject` and `/var/lib/zproject/backups` (0700, owned by
  zproject), and sets the config to `root:zproject 0640`. It **enables and
  starts nothing**.
- **prerm:** stops and disables the units when the package is removed (not
  on upgrade).
- **postrm:** keeps `/var/lib/zproject` and the `zproject` user, even on
  purge, because they hold your project history.

The service runs as `zproject` with a sandboxed unit (`ProtectSystem=strict`,
writes only to `/var/lib/zproject`, `MemoryMax=512M`). It listens on
`127.0.0.1:8788` only. `tailscale serve` publishes it on the tailnet over
https. The backup timer runs `zprojectd --backup` nightly at 03:17.

## 2. Download and verify (every machine)

```sh
mkdir -p ~/agent-work/zproject-X.Y.Z && cd ~/agent-work/zproject-X.Y.Z
gh release download vX.Y.Z -R sbj-ee/zproject -p 'SHA256SUMS' -p 'SHA256SUMS.minisig' \
  -p '<the asset(s) for this machine>'
# or: curl -fLO https://github.com/sbj-ee/zproject/releases/download/vX.Y.Z/<asset>
sha256sum -c --ignore-missing SHA256SUMS          # macOS: shasum -a 256 -c --ignore-missing SHA256SUMS
minisign -Vm SHA256SUMS -P RWQfb7N3uguWMhJ7jsE/Z0hZ7pDWHQ9OEeWSBTlN/R5bxQ8HWSMxp/l9
```

Expect `OK` for each downloaded asset. minisign should print `Signature and
comment signature verified` with `Trusted comment: zproject vX.Y.Z SHA256SUMS`.
The key (ID `32960BBA77B36F1F`) is the one in `core/UpdateSigningKey.cpp` and
[RELEASING.md](RELEASING.md).

minisign comes from `apt install minisign` or `brew install minisign`. You
don't need it on every machine. If a host doesn't have it, verify
`SHA256SUMS` with minisign on another machine. Then check that the two
copies are identical (`sha256sum SHA256SUMS` matches on both) before you
trust `sha256sum -c` on the host.

## 3. Server host

### 3.1 Install (sudo)

Install the client package on the server too. Creating and revoking tokens,
the audit log and backups all go through `zproject-cli` with the owner token,
and the owner token stays on this host.

```sh
cd ~/agent-work/zproject-X.Y.Z
sudo apt install ./zprojectd_X.Y.Z_amd64.deb ./zproject_X.Y.Z_amd64.deb
zprojectd --version && zproject-cli --version
ls -ld /var/lib/zproject /etc/zproject/zprojectd.conf    # zproject 0700; root:zproject 0640
```

### 3.2 Configure (sudo)

```sh
sudo sed -i 's|^allowed_hosts=.*|allowed_hosts=<server-host>.<tailnet>.ts.net|' /etc/zproject/zprojectd.conf
sudo grep -nE '^(listen|database|allowed_hosts|dir|notify_hub_url)=' /etc/zproject/zprojectd.conf
```

- **`listen=127.0.0.1:8788`:** keep it. zprojectd refuses a non-loopback bind.
- **`database=/var/lib/zproject/zproject.db`** and **`[backup] dir=/var/lib/zproject/backups`:** keep them.
- **`allowed_hosts`:** the server's MagicDNS name, the `Host` that
  `tailscale serve` forwards. Loopback names are always accepted. Any other
  `Host` gets a 400.
- **`[alerts]`:** leave the `<notify-hub-url>` placeholder to keep alerts
  off. To turn them on, set the URL and put the hub's token in
  `/etc/zproject/notify-hub.token` (`root:zproject`, 0640), never in the
  `.conf`.

**Why 0640 `root:zproject`, not 0600:** the service runs as `zproject` and
has to read the config. The file holds no secrets, only paths, the listen
address, host names and a URL. Root owns it, so the service can't change it.
At 0600 root, zprojectd couldn't start. The secrets on the host live
elsewhere:

- the database, in `/var/lib/zproject` (0700, owned by zproject);
- the optional `notify-hub.token` (0640);
- the CLI token files (0600, yours).

### 3.3 Owner token (sudo; run once; never displayed)

Do this before the service starts. It also creates the database as `zproject`.

```sh
install -d -m 700 ~/.config/zproject
( umask 077
  sudo -u zproject zprojectd --config /etc/zproject/zprojectd.conf \
       --create-owner-token --name owner > ~/.config/zproject/owner.token )
stat -c '%a %U %s' ~/.config/zproject/owner.token      # 600 <you> <size>
```

- The token goes to stdout, which the redirect sends straight into a 0600
  file. zprojectd's stderr message ("shown once") is all you see.
- Each run creates **another** admin token, so run this once. Revoke any
  extras with `zproject-cli token revoke`.

### 3.4 Start and publish (sudo)

```sh
sudo systemctl enable --now zprojectd.service zprojectd-backup.timer
systemctl is-active zprojectd.service && systemctl is-enabled zprojectd.service zprojectd-backup.timer
systemctl list-timers zprojectd-backup.timer --no-pager
curl -fsS http://127.0.0.1:8788/v1/health; echo
sudo tailscale serve --bg --https=443 http://127.0.0.1:8788
tailscale serve status
curl -fsS https://<server-host>.<tailnet>.ts.net/v1/health; echo
```

`tailscale serve` needs root unless your user is the tailscale operator
(`sudo tailscale set --operator=$USER`). Reading `tailscale serve status`
works either way.

### 3.5 Device tokens: one per machine, including the server

Run these as yourself. The owner token is read from its file and never shown.

```sh
cd ~/agent-work/zproject-X.Y.Z && install -d -m 700 tokens
export ZPROJECT_URL=http://127.0.0.1:8788
OWN="ZPROJECT_TOKEN_FILE=$HOME/.config/zproject/owner.token"
env $OWN zproject-cli status                                  # auth: ok
for m in <server-machine> <linux-client> <mac-client>; do
  env $OWN zproject-cli token create --kind device --name "$m" --expires 365d --out-file "tokens/$m.token"
done
env $OWN zproject-cli token list
```

- `token create` writes each token only to a **new 0600 file**. It prints
  only the id, scopes and expiry.
- Device tokens get the `write` scope. They can't manage tokens, which takes
  the owner's `admin` scope.
- 365 days is the longest sensible expiry. Without `--expires`, a device
  token never expires.

Log the server's own CLI in with its device token, over loopback:

```sh
zproject-cli login --url http://127.0.0.1:8788 --store file \
  --from-file ~/agent-work/zproject-X.Y.Z/tokens/<server-machine>.token
rm ~/agent-work/zproject-X.Y.Z/tokens/<server-machine>.token
unset ZPROJECT_URL
zproject-cli status
stat -c '%a' ~/.config/zproject/token ~/.config/zproject/cli.ini   # 600 600
```

For admin commands (`token`, `audit`, `backup`), put `env $OWN` in front so
they use the owner token file.

### 3.6 Send the client tokens with scp

```sh
cd ~/agent-work/zproject-X.Y.Z/tokens
scp -p <linux-client>.token <client-host>:agent-work/zproject-X.Y.Z/
scp -p <mac-client>.token   <client-host>:agent-work/zproject-X.Y.Z/
```

Create the target directory on each client first (section 2 does this).
`scp -p` keeps mode 0600. Delete the copies on the server once each client
has logged in.

## 4. Linux client

```sh
cd ~/agent-work/zproject-X.Y.Z            # zproject_X.Y.Z_amd64.deb verified as in section 2
sudo apt install ./zproject_X.Y.Z_amd64.deb                    # (sudo)
chmod 600 <linux-client>.token
zproject-cli login --url https://<server-host>.<tailnet>.ts.net --store file --from-file <linux-client>.token
rm <linux-client>.token
stat -c '%a' ~/.config/zproject/token ~/.config/zproject/cli.ini   # 600 600
zproject-cli status
```

## 5. macOS client

```sh
cd ~/agent-work/zproject-X.Y.Z            # the .dmg verified as in section 2
hdiutil attach -nobrowse -readonly zproject-X.Y.Z-Darwin.dmg
cp -R "/Volumes/zproject X.Y.Z/zproject.app" /Applications/
hdiutil detach "/Volumes/zproject X.Y.Z"
```

The app is ad-hoc signed and not notarized. On the first launch, right-click
`zproject.app` in Finder, choose **Open**, then **Open** again. Then link the
CLI (`/usr/local/bin` is usually user-writable on Apple Silicon; otherwise use
`~/bin`):

```sh
ln -s /Applications/zproject.app/Contents/MacOS/zproject-cli /usr/local/bin/zproject-cli
zproject-cli --version && zproject-cli doctor                  # tls: yes
chmod 600 <mac-client>.token
zproject-cli login --url https://<server-host>.<tailnet>.ts.net --store file --from-file <mac-client>.token
rm <mac-client>.token
stat -f '%Lp' ~/.config/zproject/token ~/.config/zproject/cli.ini  # 600 600
zproject-cli status
```

On macOS the default `--store` is the login Keychain (service
`ee.sbj.zproject`). `--store file` keeps the token in a 0600 file, the same
as on Linux.

## 6. Smoke tests

On the server (`$OWN` as in 3.5):

```sh
curl -fsS http://127.0.0.1:8788/v1/health; echo
test "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:8788/v1/projects)" = 401 && echo "no token: 401"
test "$(curl -s -o /dev/null -w '%{http_code}' -H 'Host: evil.example' http://127.0.0.1:8788/v1/health)" = 400 && echo "Host allowlist: 400"
zproject-cli --version && zproject-cli doctor && zproject-cli projects list

zproject-cli projects create "zproject smoke"                    # note <PROJECT_ID>
zproject-cli tasks add <PROJECT_ID> "smoke task" --duration 1    # note <TASK_ID>
zproject-cli tasks show <TASK_ID> && zproject-cli tasks list <PROJECT_ID>
zproject-cli tasks delete <TASK_ID>          # if it prints a dry run, rerun with --confirm <CONFIRM_ID>
zproject-cli projects delete <PROJECT_ID>    # same

# A read-only token can't write (expect exit 4), then revoke it.
env $OWN zproject-cli token create --kind device --name scope-test --scopes read --expires 1d \
  --out-file ~/agent-work/zproject-X.Y.Z/tokens/readonly.token
ZPROJECT_TOKEN_FILE=~/agent-work/zproject-X.Y.Z/tokens/readonly.token zproject-cli projects list
ZPROJECT_TOKEN_FILE=~/agent-work/zproject-X.Y.Z/tokens/readonly.token zproject-cli projects create nope; echo "exit=$?"
env $OWN zproject-cli token revoke <SCOPE_TEST_ID>
rm ~/agent-work/zproject-X.Y.Z/tokens/readonly.token

# MCP: the bearer header is built from the 0600 file and never echoed.
curl -fsS -H @<(printf 'Authorization: Bearer %s\n' "$(cat ~/.config/zproject/token)") \
  -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' \
  http://127.0.0.1:8788/mcp | head -c 400; echo

env $OWN zproject-cli audit --limit 20
env $OWN zproject-cli backup
sudo -u zproject zprojectd --config /etc/zproject/zprojectd.conf --verify-audit   # (sudo)
journalctl -u zprojectd --since today --no-pager | grep -c zpt_                   # 0: no tokens in the log
```

On each client:

```sh
curl -fsS https://<server-host>.<tailnet>.ts.net/v1/health; echo
zproject-cli --version && zproject-cli doctor && zproject-cli status && zproject-cli projects list
zproject-cli projects create "<client> smoke"; zproject-cli projects show <ID>; zproject-cli projects delete <ID>
```

## 7. Tokens at a glance

| Token | Where | Scope / expiry |
|---|---|---|
| owner | server `~/.config/zproject/owner.token` (0600), used through `ZPROJECT_TOKEN_FILE` | admin, no expiry |
| each device | that machine's `~/.config/zproject/token` (0600, `--store file`) | write, 365 days |

- Never echo a token, put it on a command line, or paste it into chat, an
  issue or a commit.
- Renew a device token before it expires: `token create`, then
  `login --from-file` on the client, then `token revoke` on the old one.
- For a lost machine, run `env $OWN zproject-cli token revoke <ID>`.

## 8. Upgrade, rollback, uninstall

- **Upgrade:** verify the new assets (section 2), then
  `sudo apt install ./zprojectd_<new>_amd64.deb ./zproject_<new>_amd64.deb`
  and `sudo systemctl restart zprojectd.service`. The config is a conffile,
  so your edits are kept. On macOS, replace `zproject.app`.
- **Restore data:**
  ```sh
  sudo systemctl stop zprojectd.service
  sudo -u zproject zprojectd --config /etc/zproject/zprojectd.conf --restore <backup-file>
  sudo systemctl start zprojectd.service
  ```
- **Linux client:** `zproject-cli logout`, then `sudo apt remove zproject`,
  then `rm -rf ~/.config/zproject`.
- **macOS client:**
  1. `zproject-cli logout`
  2. `rm /usr/local/bin/zproject-cli`
  3. Move `zproject.app` to the Trash.
  4. `rm -rf ~/.config/zproject`
- **Server:**
  ```sh
  sudo tailscale serve --https=443 off
  sudo systemctl disable --now zprojectd.service zprojectd-backup.timer
  sudo apt remove zprojectd             # apt purge also removes /etc/zproject/zprojectd.conf
  rm ~/.config/zproject/owner.token
  ```
  `/var/lib/zproject` (the database and backups) and the `zproject` user
  survive even a purge. Removing them destroys your data, so copy a backup off
  the host first:
  `sudo rm -rf /var/lib/zproject && sudo deluser zproject && sudo delgroup zproject`.
