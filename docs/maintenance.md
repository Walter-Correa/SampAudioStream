# Maintenance Guide

What keeps SampAudioStream working over time, split between **server owners** (who run the plugin) and **maintainers** (who build and release it), plus what is already automated and what can be.

Most of the plugin is pinned and reproducible, so it does not break on its own. The part that changes constantly is outside the codebase: media platforms (YouTube above all) change how their pages and streams work, and the extractor (`yt-dlp`) and its JavaScript runtime have to keep up.

---

## 1. Server Owners

| Task | Why | When |
| :--- | :--- | :--- |
| **Update the binary and `SampAudioStream.inc` together, then recompile your scripts** | A script compiled against a newer include calls natives an older binary does not have, and fails to load. | Every release. |
| **Keep `extractor.auto_update` enabled** | The plugin updates `yt-dlp` by itself, verifying the release's `SHA2-256SUMS` before replacing the binary. Platform fixes usually arrive this way, without a plugin release. | Always (default). |
| **Allow outbound HTTPS to GitHub** | Extractor binaries and the JS runtime (Deno / QuickJS-ng) are downloaded from GitHub releases on first boot and on updates. | Firewall setup. |
| **Refresh `cookies.txt` when asked to prove you are not a bot** | Cookies expire or get rotated, and datacenter IPs are flagged more often. See the [YouTube Anti-Bot & Cookies Guide](youtube-cookies.md). | Only when `SA_ERR_AUTH_REQUIRED` / bot checks appear. |
| **Linux: keep the 32-bit `libcurl.so.4` up to date** | HTTPS on Linux uses the system libcurl loaded at runtime, so TLS and security fixes come from your distribution (`apt upgrade`). | With regular system updates. |
| **Windows: nothing extra** | HTTPS uses the system WinHTTP, updated by Windows Update. | — |
| **Check the server log now and then** | Repeated extractor update failures or HTTP 403 errors point to an outdated extractor, expired cookies or a flagged IP. | Occasionally, or when tracks stop playing. |

> **sampctl users:** `pawn.json` publishes the Windows and Linux release archives, so `sampctl` fetches the matching binary and include together.

---

## 2. Maintainers

### 2.1 Pinned JS runtimes (highest priority)

`src/extractor/binary_manager.cpp` pins the JavaScript runtimes used by `yt-dlp` to solve YouTube challenges:

```cpp
constexpr const char* kPinnedQuickJsVersion = "v0.18.0";
constexpr const char* kPinnedDenoVersion = "v2.9.8";
```

`yt-dlp` updates itself on servers, but these pins only change with a plugin release. When a `yt-dlp` release raises the minimum runtime version, servers keep failing until a new plugin version ships.

- Follow the [`yt-dlp` release notes](https://github.com/yt-dlp/yt-dlp/releases) for JS runtime (EJS) requirements.
- `dependencies.yml` opens a pull request with the new pins and the changelog line (see section 3); review it, test, merge and release.

### 2.2 Platform behaviour handled in our own code

Some platform behaviour is handled by the plugin itself rather than by `yt-dlp`: IP-bound signed media URLs, HTTP 403 retries and URL refresh before a queued track starts, resolver cache TTL (`performance.cache_ttl_seconds`), and placeholder titles on radio streams. A platform change here cannot be fixed by an extractor update. It usually shows up as "tracks stopped playing" reports; `scriptfiles/SampAudioStream/debug/` keeps the last rejected media request for analysis.

> The debug files contain signed, IP-bound URLs and the server's public IP. Never paste them publicly without masking.

### 2.3 C/C++ libraries (`cmake/Dependencies.cmake`)

| Library | Pin |
| :--- | :--- |
| nlohmann/json | tag (`GIT_TAG`) |
| asio | tag (`GIT_TAG`) |
| xiph/ogg | tag (`GIT_TAG`) |
| xiph/opus | tag (`GIT_TAG`) |
| fdk-aac | tag (`GIT_TAG`); the unused `fdk-xhe-aac` target is excluded from the build |
| libmp3lame (CMake port) | commit + SHA256 (`URL`) |
| dr_libs (`dr_flac.h`, `dr_wav.h`, `dr_mp3.h`) | commit + SHA256 (`URL`) |
| stb (`stb_vorbis.c`) | commit + SHA256 (`URL`) |

Builds are reproducible: these do not change unless the pins are edited, so there is no need to update them on a schedule. The exception is **security**: `dr_libs`, `stb_vorbis`, `fdk-aac` and `opus` decode data downloaded from the internet, and `stb_vorbis` and `dr_libs` have had CVEs in the past.

- The weekly `dependencies.yml` run lists newer **releases** in the "Pinned library updates available" issue (see section 3): new tags for `GIT_TAG` pins; for `dr_libs`, a new per-header release tag (`flac-*`, `wav-*`, `mp3-*`) beyond the header version we use; for `stb`, a new `stb_vorbis.c` version. Development commits alone are not reported. Review the list once or twice a year, or right away when an advisory is published.
- For `URL` pins, update both the commit in the URL and `URL_HASH` (`cmake -E sha256sum <archive>`).

### 2.4 Platform SDKs (`cmake/sdk-pins.txt`)

The SDKs are cloned into `deps/` (ignored by Git) at the commits pinned in `cmake/sdk-pins.txt`, which the Build and Release workflows read.

- **`open.mp-sdk`**: a new open.mp server release can change the component ABI (`OMP_VERSION_SUPPORTED` in `include/component.hpp`). The SDK has no releases, so every upstream commit is checked; the weekly run opens a pull request with the new pin and says whether the ABI changed.
- **`samp-plugin-sdk`**: SA-MP is frozen; checked the same way, but no updates are expected.
- **Local builds**: after changing a pin, delete the folder in `deps/` and clone it again at the new commit.

### 2.5 CI (`.github/workflows/`)

- **Linux builds** run inside a `debian:bullseye` container to keep the glibc requirement low (Debian 11+, Ubuntu 20.04+). Debian 11 is past its LTS, so the workflows already point APT at `archive.debian.org`. This keeps working as long as the archive and the Docker image exist; moving to `bookworm` raises the minimum glibc to 2.36 and drops older distributions, which is a **major**-level compatibility decision for server owners.
- **Runner images** (`windows-2022`, `ubuntu-24.04`) are retired by GitHub every few years; the workflow logs show deprecation notices well in advance.
- **Actions** (`actions/checkout`, `upload-artifact`, `download-artifact`) occasionally deprecate major versions; bump them when warned.

### 2.6 Versioning

Follow semantic versioning so server owners know what an update implies:

- **Patch**: fixes only (including runtime pin bumps and library security updates).
- **Minor**: new natives, parameters, callbacks, states or behaviour, without breaking existing scripts.
- **Major**: anything that breaks existing scripts or raises system requirements.

Files that carry the version: `CMakeLists.txt` (`project(... VERSION ...)`), the load messages in `src/main.cpp` and `src/runtime/omp_host.cpp`, `componentVersion()` in `include/sampaudiostream/runtime/omp_host.hpp` (the version open.mp reports), `SampAudioStream.inc` (header and `SAMPAUDIOSTREAM_VERSION_*`) and `pawn.json`, plus the changelog heading in `README.md`. **No need to edit them by hand**: the Release workflow sets all of them (2.7).

### 2.7 Changelog and releases

Changes waiting for a release are listed in a `### Unreleased` section at the top of the Version history in `README.md`, so no change has to guess the next version number:

- Dependency pull requests (section 3) write their own line there. Their title shows the version they were generated from, e.g. `[SampAudioStream 2.4.0] Update QuickJS-ng to v0.19.0`, and their commit follows the project style: `Update QuickJS-ng to v0.19.0 (based on v2.4.0)` plus `- ` bullets.
- For anything else (plugin code, library updates), add the line yourself in the same commit. You may instead write the section with its version by hand (`### 2.5.0 — DD/MM/YYYY`).

**To release:** **Actions → Release → Run workflow** and pick the version:

| Option | Use it when |
| :--- | :--- |
| **auto** (default) | The top of the Version history already has a version written by hand (`### X.Y.Z — ...`) that is not released yet. That version is used, and any `### Unreleased` lines are moved into it. |
| **patch** | Only `### Unreleased`, with fixes and dependency updates. |
| **minor** | Only `### Unreleased`, with at least one new native, parameter, callback, state or compatible behaviour change. |
| **major** | Only `### Unreleased`, with anything that breaks existing scripts or raises system requirements. |

GitHub cannot show different options depending on the README, so the workflow checks instead and stops with a clear message, changing nothing, when the choice does not fit:

- **auto** with only `### Unreleased`: choose patch, minor or major.
- **patch/minor/major** while a version written by hand is pending: choose auto.
- Nothing pending (no `### Unreleased` lines and no unreleased version): nothing to release.

Then the workflow (`.github/scripts/bump_version.py`):

1. Sets the version in the 6 files that carry it (`CMakeLists.txt`, `pawn.json`, `SampAudioStream.inc`, `src/main.cpp`, `src/runtime/omp_host.cpp`, `include/sampaudiostream/runtime/omp_host.hpp`) and dates the changelog section with the release day in `README.md`.
2. Pushes a `Prepare release (vX.Y.Z)` commit (with `- ` bullets), builds Windows and Linux from it, and publishes the GitHub release with that changelog section.

If a release run fails after that commit, run it again with **auto**: it publishes the same version instead of bumping again.

**What needs a release** (a change reaches servers only through a release; merging into `main` is not enough):

| Change | Release? |
| :--- | :--- |
| Deno / QuickJS-ng pull request | **Yes**. Labelled **needs release**. Not urgent unless `yt-dlp` requires that runtime version. |
| open.mp SDK pull request with `OMP_VERSION_SUPPORTED` changed | **Yes**, before servers update to an open.mp version built with it. Labelled **needs release**. |
| open.mp / SA-MP SDK pull request without ABI change | No. It goes out with the next release. |
| Library update (from the issue) | **Yes**. Nothing breaks while it waits; prioritize security fixes. |
| Plugin code change | **Yes**. |
| Dependabot (GitHub Actions) pull request, documentation or workflow changes | No. |

Several changes can wait in `### Unreleased` and go out in one release.

---

## 3. Automation

### On servers

- `yt-dlp` download and periodic updates, with checksum verification.
- JS runtime (Deno / QuickJS-ng) download on first boot, with SHA-256 verification, a health check and fallback.

### In the repository

| What | How | Your part |
| :--- | :--- | :--- |
| Build, tests and release packaging (Windows and Linux) | `build.yml` on every push and pull request; `release.yml` started by hand: sets the version, turns `### Unreleased` into its changelog, builds and publishes. | Run the release and pick auto, patch, minor or major (2.7). |
| GitHub Actions versions | Dependabot (`.github/dependabot.yml`), monthly, all actions grouped in one pull request. | Merge when the build passes. |
| Deno / QuickJS-ng pins and platform SDKs (`cmake/sdk-pins.txt`) | `dependencies.yml` (weekly, Monday 06:00 UTC, or by hand from the Actions tab) runs `.github/scripts/check_dependencies.py`. Runtime updates rewrite the version constant, every download URL and every SHA-256 hash (computed from the downloaded asset and cross-checked with GitHub's asset digest); SDK updates move the pin to the new upstream commit and check the open.mp component ABI. Everything found goes into one `deps/update-*` pull request that also writes its changelog line under `### Unreleased`, is labelled **needs release** when servers need it (with how urgent it is), and starts the Build workflow on its branch. Only one such pull request is open at a time. | Read the release notes / compare link, wait for the build, test YouTube playback (or the latest open.mp server), merge; release when labelled. |
| Libraries in `cmake/Dependencies.cmake` | The same weekly run keeps one open issue, **"Pinned library updates available"**, listing newer upstream releases (section 2.3) and which version to release them in; it closes the issue when everything matches. | Review the changes (security first), update by hand and release. |

**One-time setup:** Settings → Actions → General → Workflow permissions → enable **"Allow GitHub Actions to create and approve pull requests"**. Without it the update pull request cannot be opened (the library issue still works).

Run the check locally without changing anything:

```bash
python -I .github/scripts/check_dependencies.py --check-only
```

> Renovate was not used for the runtimes because it can bump the version but not recompute the per-asset SHA-256 hashes, which would produce pull requests that fail verification on every server. The SDK pins live in `cmake/sdk-pins.txt` rather than in the workflow files because the workflow token is not allowed to push changes to `.github/workflows/`.

### Stays manual

- Refreshing cookies (needs a signed-in browser; `extractor.cookies_from_browser` helps on desktop hosts).
- Fixing platform behaviour handled in our own code (2.2).
- Choosing a new Linux base image or raising system requirements.
- Updating the libraries themselves (decoders are reviewed before any bump; `URL` pins also need `URL_HASH` recomputed).
- Updating binaries on each server (owners decide when to update).
