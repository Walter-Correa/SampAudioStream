# YouTube Cookies Guide (`Sign in to confirm you're not a bot`)

## When Is This Needed?

You only need YouTube cookies when YouTube challenges your server's IP address with `"Sign in to confirm you're not a bot"` (or age-restricted playback requires an adult-verified account). This is common on cloud VPS and datacenter IPs and rare on residential connections. Internet radios, direct audio URLs, and other platforms are unaffected.

---

## What SampAudioStream Already Does Automatically

While YouTube does not challenge the server, **SampAudioStream never touches browser files**. Only *after* YouTube rejects a request with an auth/bot challenge does the plugin fall back through cookie sources in this order:

1. **Configured file (`cookies_file`):** sent on every call when set and existing on disk (if missing, a single `Warn` is logged on startup).
2. **Default file (`scriptfiles/SampAudioStream/cookies.txt`):** if present when `cookies_file` is empty, it is loaded automatically from startup on every request (and also detected automatically if placed while the server is running).
3. **Installed browsers (`cookies_from_browser: "auto"`):** scans local profile directories on disk and passes up to 4 candidates to `yt-dlp` in priority order:
   1. **Firefox** (newest cookie database first)
   2. **Chromium-based browsers** (Chrome, Edge, Brave, Chromium, Vivaldi, Opera — newest cookie database first per browser)
4. **If none succeed:** fires `SA_ERR_AUTH_REQUIRED` (`10`) to Pawn callbacks and logs a rate-limited `Warn` (at most once every 10 minutes, even with `logging.debug` off) explaining what to do.

---

## Recommended Browser: Firefox (and Account Safety)

* **Why Firefox:** Firefox works identically on both Windows and Linux, allows `yt-dlp` to read `cookies.sqlite` even while Firefox is open, and does **not** rely on an OS graphical keyring (Linux) or DPAPI / App-Bound encryption (Windows) that typically prevents `yt-dlp` from reading Chrome, Edge, or Brave cookies.
* **Use a secondary Google account:** Always sign in with a throwaway/secondary Google account—never your personal account. YouTube can restrict accounts used for automated downloads.

---

## Path A — Machine With a Desktop / GUI (Windows or Linux GUI VPS)

1. Install **Firefox** on the server machine.
2. Open `https://www.youtube.com`, sign in with your **secondary Google account**, and play one video so session cookies are written cleanly.
3. Leave the session signed in (do **not** click *Sign out*).
4. Make sure your SA-MP or open.mp server runs under the **same OS user** that owns that Firefox profile (and **not** as `root` on Linux):
   * Check the running server user: `ps -eo user,comm | grep -Ei "omp-server|samp03svr"`
   * If it runs as `root` while your Firefox profile is under a regular user (e.g. `admindks`), change ownership of the server folder (`sudo chown -R <user>:<user> <server_dir>`) and start the server directly as `<user>` without `sudo` (remove `sudo` from any startup script, or set `User=<user>` and `Group=<user>` in your `systemd` service unit).
5. Done — on the next YouTube bot challenge, **SampAudioStream** automatically finds the Firefox profile, pins it for the rest of the session, and logs an `Info` message naming the browser and profile.

---

## Path B — Headless Server / Terminal-Only VPS

1. On your personal PC, open a **Private / Incognito** browser window.
2. Go to `https://www.youtube.com`, sign in with your **secondary Google account**, and open one video.
3. Export cookies in Netscape format using the **"Get cookies.txt LOCALLY"** browser extension (enable the extension for Private/Incognito windows).
4. Close the private window **without clicking Sign out** (closing instead of signing out keeps the exported session active longer).
5. Upload the file to `scriptfiles/SampAudioStream/cookies.txt` on your server. No `config.json` change is needed (and even on an already-running server, the next blocked request picks it up automatically).

---

## Verifying, Maintaining & Troubleshooting

* **How to confirm it worked:** Look for `[SampAudioStream]` lines in your server log:
  * Startup/hot-drop file: `Using YouTube cookies file: scriptfiles/SampAudioStream/cookies.txt` (or `Detected new YouTube cookies file...`)
  * Browser fallback: `YouTube challenged this server's IP; using cookies from Firefox (profile: ...). The account logged in there is used for YouTube requests; a secondary account is recommended.`
* **Maintenance:**
  * *Path B (`cookies.txt`):* Exported cookies expire after a few weeks; when the `Warn` returns, repeat the private-window export and overwrite `scriptfiles/SampAudioStream/cookies.txt`.
  * *Path A (Firefox):* Cookies renew naturally as long as the profile stays signed in.
* **Security:** Both `cookies.txt` and browser profile directories grant access to that Google account. **Never share them publicly or commit them to Git.**
* **Disable automatic browser cookie discovery:** Set `extractor.cookies_from_browser` to `"off"` (`extractor.cookies_from_browser` in `scriptfiles/SampAudioStream/config.json` for SA-MP, or `sampaudiostream.extractor.cookies_from_browser` in `config.json` for open.mp).
* **If nothing works:** Datacenter IPs can sometimes be hard-blocked even with valid cookies; if challenges persist with a fresh `cookies.txt` and Firefox session, you may need a different IP or VPS provider.
