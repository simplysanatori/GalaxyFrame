# Privacy audit

What `tools/privacy-check.sh` and the checks below look for before anything is published: every tracked file, the history, the release notes and the release tarball.

## What was searched

- The owner's names and former account names, and the old commit email. These were searched with a list kept outside the repository, so that the list itself does not publish them.
- Network identifiers: IPv4 addresses, MAC addresses, PC and client hostnames, Wi-Fi network names, including the Frame's own access point name pattern.
- IDs: UUIDs, Steam user IDs and `userdata/` paths, headset and controller serial numbers, OpenXR system IDs.
- Secrets: GitHub tokens (`ghp_`, `gho_`, `github_pat_`), other API key prefixes (`sk-`, `xox?-`, `AKIA`), PEM private keys and certificates, email addresses.
- Home folder paths of users other than the generic `steamos`.

## What it finds

- Tracked files: no personal data found. No file needed a placeholder.
- Paths under `/home/steamos` and `127.0.0.1` (Steam's local debugging port, used by `install.sh` and `steam-cef.py`) are generic and stay.
- The sha256 in `steam-frame/prepare-game.sh` is the checksum of the public nodtool download.
- Published history: no tokens, keys or network identifiers. Commits and tags on this branch use the GitHub no-reply address.
- The release tarball holds only the launcher, the stripped game library, the Vulkan shader library, the headless runner, the launch wrapper, `vrtiming.py` and, if given, a shader cache seed (`spirv.bin`). `steam-frame/build.sh` packs those files by name, so configuration, saves, logs and keys cannot end up in a tarball. No personal strings were found in the binaries.
- The game's own logs (`petari_log.txt`, `launch.log`) hold file paths, the Steam shortcut's app id and settings. They hold no account names, network addresses or serial numbers. SteamVR's own logs can hold device serial numbers. The README says to check those before attaching them to a report.
- There is no bug-report or log-collection script in the repository.

## Verification

Run these from the repository root. Each must print `0`.

```sh
# IPv4 addresses other than 127.0.0.1 and 0.0.0.0 (two SDK version strings in decomp/src/RVL_SDK look like addresses and are excluded)
git grep -IPn '(?<![\d.])(?:\d{1,3}\.){3}\d{1,3}(?![\d.])' -- . ':!decomp/src/RVL_SDK' | grep -vE '127\.0\.0\.1|0\.0\.0\.0' | wc -l

# MAC addresses
git grep -IPni '(?<![0-9a-f:])(?:[0-9a-f]{2}:){5}[0-9a-f]{2}(?![0-9a-f:])' | wc -l

# UUIDs
git grep -IPni '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' | wc -l

# Email addresses other than GitHub no-reply ones
git grep -IPn '[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+\.[A-Za-z]{2,}' | grep -v 'users.noreply.github.com' | wc -l

# Tokens, API keys, private keys and certificates
git grep -IPn 'ghp_[A-Za-z0-9]{20}|gho_[A-Za-z0-9]{20}|github_pat_[A-Za-z0-9_]{20}|sk-[A-Za-z0-9]{20}|xox[abp]-|AKIA[0-9A-Z]{16}|-----BEGIN [A-Z ]*(PRIVATE KEY|CERTIFICATE)' | wc -l

# Windows PC names, Steam Frame access point names, Steam user IDs, other users' home folders
git grep -IPn 'DESKTOP-[A-Z0-9]{5,}' | wc -l
git grep -IPni 'frame_[0-9a-f]{8}\b|userdata/[0-9]{5,}|/home/(?!steamos\b)[a-z][a-z0-9_-]+/|C:\\\\Users\\\\' | wc -l

# Secrets anywhere in the history
git log -p --all --tags | grep -cE 'ghp_[A-Za-z0-9]{20}|github_pat_[A-Za-z0-9_]{20}|-----BEGIN [A-Z ]*PRIVATE KEY'

# Commit and tag identities other than no-reply addresses, in the history
git log --format='%ae%n%ce' --all --tags | grep -vc 'users.noreply.github.com'
```

Before publishing new docs, logs or probe output, run these again. Replace anything they find with a placeholder such as `<ip-address>`, `<pc-hostname>`, `<ssid>` or `<serial>`.
