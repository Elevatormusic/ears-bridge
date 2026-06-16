# EARS Bridge — Update Notification (design spec)

Date: 2026-06-16

## Purpose

Tell the user, non-intrusively, when a newer EARS Bridge release exists on GitHub, so they
can choose to update. On by default, user-disableable, and it never downloads or installs
anything itself — the user clicks through to get the new version.

## Settled decisions (from brainstorming)

- **Notice = a subtle title-bar link**: `Update available · vX.Y.Z →` shown next to the existing
  status line. Non-modal, accent-coloured, stays visible until the app is up to date. Never a popup.
- **On by default, with an off switch**: checks silently once per launch; an
  **"Automatically check for updates"** checkbox in the Advanced disclosure disables it.
- **Never auto-downloads / auto-installs**: clicking the link opens the release page in the
  default browser (changelog + the right installer). Getting/running the installer stays a user action.
- **At most one successful check per 24 h**, on a background thread, **fail-silent** (offline/error
  → nothing shown). A measurement app is launched only occasionally, so in practice this is ~once
  per launch; the 24 h floor just avoids hammering the API on rapid relaunches.

## Prior art

Mirrors the sibling **apple-hig** plugin's update notifier (a SessionStart hook, shipped 2026-06):
notify-only (never self-updates), throttled to ~1 check/24 h, silent offline, **numeric** semver
compare (`1.10.0 > 1.9.0`), opt-out, unit-tested. Same shape, different runtime (a JUCE desktop app
vs a Node hook). It also surfaced the **bootstrap caveat** below.

## Components

### `UpdateChecker` — new isolated unit (`src/net/UpdateChecker.{h,cpp}`)

Single responsibility: off-thread, ask GitHub for the latest release, compare to the running
version, and report the result back on the message thread.

```
struct Result { bool updateAvailable = false; juce::String latestVersion; juce::String releaseUrl; };
// Runs asynchronously; `onDone` is always invoked on the message thread (or never, if cancelled).
void start (juce::String currentVersion, std::function<void(Result)> onDone);
```

- Fetch: `GET https://api.github.com/repos/Elevatormusic/ears-bridge/releases/latest`
  - Headers: `User-Agent: EARS-Bridge/<currentVersion>` (GitHub requires a UA),
    `Accept: application/vnd.github+json`.
  - Connect/read timeout ≈ 3000 ms.
  - Use `juce::URL` + `WebInputStream`/`createInputStream` (in `juce_core`, already linked).
  - Why the Releases API (not the raw source version on `main`): we want the latest **published,
    downloadable** release; `main`'s `CMakeLists.txt` version can be ahead of it (a dev bump). The
    apple-hig sibling fetches raw `plugin.json` from `main` instead, because for a plugin `main` *is*
    the release.
- Parse with `juce::JSON`: read `tag_name` (e.g. `"v0.2.13"`) and `html_url`.
- Decide with the pure helper `isNewer(current, latest)`.
- Any failure (no network, non-200, HTTP 403 rate-limit, missing/garbage fields, JSON error)
  → `Result{ updateAvailable = false }`. Silent. No dialog, no log spam.
- Cancel-safety / lifetime: owns a background `juce::Thread` (or `Thread::launch`) + an atomic
  alive flag; the message-thread callback is guarded (`juce::Component::SafePointer` /
  `WeakReference`) so a destroyed owner is never called. The worker never touches the owner
  directly — only through the guarded `MessageManager::callAsync`.

### `isNewer` — pure version comparison (tested)

```
bool isNewer (juce::String currentVersion, juce::String latestTag);
```

- Strip a leading `v`/`V`; split on `.`; parse up to three integers (major, minor, patch;
  missing component → 0); ignore any pre-release/build suffix after the patch number.
- Return `true` iff the latest `(major, minor, patch)` tuple is strictly greater than current.
- Malformed/empty input → `false`.

## UI wiring (`src/gui/MainComponent.{h,cpp}`)

- New member: a small clickable label / `HyperlinkButton` `updateLink` (accent text), hidden by
  default, positioned in the title-bar row near `statusLine`.
- In the constructor, after settings load: if `settings.autoCheckUpdates()` is true, call
  `updateChecker.start (juce::ProjectInfo::versionString, callback)`.
- Callback (message thread): if `result.updateAvailable`, set the link text to
  `"Update available · " + result.latestVersion + " →"`, store `result.releaseUrl`,
  `setVisible(true)`, re-layout; otherwise leave it hidden.
- `updateLink` click → `juce::URL(releaseUrl).launchInDefaultBrowser()` (fall back to the
  `/releases` page URL if `releaseUrl` is empty).
- `resized()`: place `updateLink` in the title-bar row without overlapping the Start/Stop button
  or `statusLine` — it shares the status area and only appears when an update exists.
- It is a passive link; it coexists with `statusLine` and never blocks or interrupts a running
  measurement.

## Settings (`src/state/Settings.{h,cpp}`)

- Add `bool autoCheckUpdates()` (default `true`) + `setAutoCheckUpdates(bool)`, backed by an
  `"autoCheckUpdates"` key in the existing `PropertiesFile`.
- Advanced disclosure: add an **"Automatically check for updates"** checkbox bound to it.
  Turning it off prevents the check on the next launch (and hides an already-shown link immediately).
- Add `juce::int64 lastUpdateCheck()` / `setLastUpdateCheck(juce::int64)` (unix seconds, default 0),
  key `"lastUpdateCheck"`. The check runs on launch only if `autoCheckUpdates()` **and**
  `now - lastUpdateCheck() >= 24 h`; the timestamp is written after a successful (HTTP 200) check.

## Error handling / edge cases

- Offline / DNS failure / timeout → silent, no link.
- HTTP 403 (rate limit) or any non-200 → silent.
- `tag_name` missing or unparseable → silent.
- `current >= latest` (including a local/dev build newer than the published release) → no link.
- `/releases/latest` excludes drafts and pre-releases (GitHub behaviour) → only stable releases
  ever trigger the notice.
- **Bootstrap caveat:** the checker ships *inside* a release, so a user must first update to (or
  install) the version that introduces it before any future "update available" notice can appear.

## Build / platform

- `juce::URL` / `WebInputStream` / `JSON` are in `juce_core`, already a dependency — **no new JUCE
  module**. CMake change is **additive**: add `src/net/UpdateChecker.cpp` to the app target and the
  `isNewer` unit to the test target.
- Windows uses the built-in WinHTTP/WinINet backend. On macOS, outbound HTTPS to `api.github.com`
  is allowed by default (no App Transport Security plist exception needed). If a macOS/Linux build
  ever fails to resolve TLS, confirm `JUCE_USE_CURL`. UI + networking are 100% cross-platform
  (no `#if JUCE_*`), so the macOS `.dmg` ships the identical behaviour.

## Testing

Catch2 unit tests for `isNewer()` (the pure logic; the live network fetch is not unit-tested):

- `isNewer("0.2.12", "v0.2.13") == true`
- `isNewer("0.2.12", "v0.2.12") == false`
- `isNewer("0.2.9",  "v0.2.10") == true`  *(numeric, not lexical)*
- `isNewer("0.2.12", "v0.2.11") == false`
- `isNewer("0.2.12", "v0.3.0")  == true`
- `isNewer("0.2.12", "1.0.0")   == true`  *(no `v` prefix)*
- `isNewer("0.2.12", "v0.2.13-beta") == true`  *(suffix ignored)*
- `isNewer("0.2.12", "garbage") == false`
- `isNewer("0.2.12", "")        == false`

## Privacy

The only outbound contact is GitHub's public API, once per launch — a standard HTTPS request
(User-Agent + IP, like any web fetch); no user or measurement data is sent. The Advanced toggle
disables it entirely. Note this in the README.

## Out of scope (YAGNI)

- No auto-download / auto-install.
- No per-version "skip/dismiss" (the persistent link + the toggle are sufficient).
- No polling while running — one check per launch.
- No changelog / "what's new" panel (a separate future feature).
- No menu bar / About box / Help menu (the separate "app-shell" bundle, earmarked, not part of this spec).

## Files touched

- **New:** `src/net/UpdateChecker.h`, `src/net/UpdateChecker.cpp`, `tests/test_updatecheck.cpp`
  (or the repo's existing test-file convention).
- **Modified:** `src/gui/MainComponent.{h,cpp}`, `src/state/Settings.{h,cpp}`, `CMakeLists.txt`
  (additive source + test entries), README (privacy note + a line in the feature list).
