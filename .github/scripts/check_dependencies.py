#!/usr/bin/env python3
"""Checks pinned dependencies against their upstream versions.

- JS runtimes (Deno, QuickJS-ng) pinned in src/extractor/binary_manager.cpp: when a newer
  release exists, rewrites the version constant, every download URL and every SHA-256 hash
  (computed from the downloaded asset and cross-checked with GitHub's asset digest).
- Platform SDKs pinned in cmake/sdk-pins.txt: moves each pin to the upstream HEAD commit and
  flags open.mp component ABI changes (OMP_VERSION_SUPPORTED).
- Libraries pinned in cmake/Dependencies.cmake: reports newer releases only (tags, or the
  version in the header the plugin uses), since they are audio decoders reviewed by hand.

Runtime and SDK changes go into one pull request, which also writes their changelog line in the
"### Unreleased" section of README.md. The version number is chosen later, by the Release workflow.

Outputs (GITHUB_OUTPUT when set): pr_needed, pr_branch, pr_title, needs_release,
base_version,
libraries_outdated.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
BINARY_MANAGER = os.path.join(ROOT, 'src', 'extractor', 'binary_manager.cpp')
DEPENDENCIES = os.path.join(ROOT, 'cmake', 'Dependencies.cmake')
SDK_PINS = os.path.join(ROOT, 'cmake', 'sdk-pins.txt')
README = os.path.join(ROOT, 'README.md')
INCLUDE = os.path.join(ROOT, 'SampAudioStream.inc')
USER_AGENT = 'SampAudioStream-dependency-check'

RUNTIMES = [
    # display name, GitHub repository, version constant
    ('Deno', 'denoland/deno', 'kPinnedDenoVersion'),
    ('QuickJS-ng', 'quickjs-ng/quickjs', 'kPinnedQuickJsVersion'),
]

SAFE_TAG = re.compile(r'^[A-Za-z0-9._-]{1,64}$')


def request(url):
    req = urllib.request.Request(url, headers={'User-Agent': USER_AGENT})
    if url.startswith('https://api.github.com/'):
        req.add_header('Accept', 'application/vnd.github+json')
        token = os.environ.get('GITHUB_TOKEN')
        if token:
            req.add_header('Authorization', 'Bearer ' + token)
    return urllib.request.urlopen(req, timeout=600)


def api(path):
    with request('https://api.github.com/' + path) as r:
        return json.load(r)


def sha256_of(url):
    h = hashlib.sha256()
    with request(url) as r:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def version_key(tag):
    return tuple(int(x) for x in re.findall(r'\d+', tag))


def git_ls_remote(url, options=(), patterns=()):
    out = subprocess.run(['git', 'ls-remote', *options, url, *patterns],
                         check=True, capture_output=True, text=True).stdout
    return [line.split('\t', 1) for line in out.splitlines() if '\t' in line]


# ---------------------------------------------------------------- JS runtimes

def update_runtimes(source, forced, check_only):
    """Returns (new_source, report_lines, changes) where changes = [(name, old, new)]."""
    lines = []
    changes = []
    for name, repo, constant in RUNTIMES:
        m = re.search(r'\b' + constant + r' = "([^"]+)"', source)
        if not m:
            sys.exit('%s not found in binary_manager.cpp' % constant)
        current = m.group(1)

        release = api('repos/%s/releases/%s' % (repo, ('tags/' + forced[name]) if name in forced else 'latest'))
        latest = release['tag_name']
        if not SAFE_TAG.match(latest):
            sys.exit('%s: unexpected tag name %r' % (name, latest))
        if name not in forced and version_key(latest) <= version_key(current):
            lines.append('- %s `%s` is up to date.' % (name, current))
            continue

        url_re = re.compile(
            r'"https://github\.com/' + re.escape(repo) + r'/releases/download/([^/"]+)/([^"]+)",(\s*)"([0-9a-f]{64})"')
        matches = list(url_re.finditer(source))
        if not matches:
            sys.exit('%s: no pinned download URLs found' % name)
        for u in matches:
            if u.group(1) != current:
                sys.exit('%s: URL pinned to %s but %s is %s' % (name, u.group(1), constant, current))

        if check_only:
            lines.append('- %s `%s` → `%s` available (check only).' % (name, current, latest))
            changes.append((name, current, latest))
            continue

        assets = {a['name']: a for a in release.get('assets', [])}
        hashes = {}
        for asset_name in sorted({u.group(2) for u in matches}):
            asset = assets.get(asset_name)
            if not asset:
                sys.exit('%s %s: release asset %s is missing (renamed upstream?)' % (name, latest, asset_name))
            digest = hashes[asset_name] = sha256_of(asset['browser_download_url'])
            upstream = (asset.get('digest') or '').lower()
            if upstream and upstream != 'sha256:' + digest:
                sys.exit('%s %s: SHA-256 of %s does not match GitHub digest' % (name, latest, asset_name))
            print('%s %s: %s %s' % (name, latest, asset_name, digest))

        def repl(u):
            return '"https://github.com/%s/releases/download/%s/%s",%s"%s"' % (
                repo, latest, u.group(2), u.group(3), hashes[u.group(2)])

        source = url_re.sub(repl, source)
        source = source.replace('%s = "%s"' % (constant, current), '%s = "%s"' % (constant, latest), 1)
        changes.append((name, current, latest))
        lines.append('- %s `%s` → `%s` ([release notes](https://github.com/%s/releases/tag/%s)); %d asset(s) re-hashed.'
                     % (name, current, latest, repo, latest, len(hashes)))
    return source, lines, changes


# ---------------------------------------------------------------- libraries

# Libraries pinned by commit (no GitHub releases): the headers the plugin uses carry their own
# version. dr_libs also tags each release per header (flac-x.y.z); stb bumps the header version.
HEADER_VERSIONS = {
    'mackron/dr_libs': [('dr_flac.h', 'flac-'), ('dr_wav.h', 'wav-'), ('dr_mp3.h', 'mp3-')],
    'nothings/stb': [('stb_vorbis.c', None)],
}


def header_version(repo, ref, path):
    """Returns (version, released) from the first lines of a header, e.g. 'dr_flac - v0.13.5 - TBD'."""
    with request('https://raw.githubusercontent.com/%s/%s/%s' % (repo, ref, path)) as r:
        head = r.read(2048).decode('utf-8', 'replace')
    m = re.search(r'\bv(\d+(?:\.\d+)+)(?:\s*-\s*(\S+))?', head)
    if not m:
        return None, False
    return m.group(1), (m.group(2) or '').upper() not in ('TBD', 'UNRELEASED')


def remote_tags(url):
    return [ref.rsplit('/', 1)[-1] for _, ref in git_ls_remote(url, options=('--tags', '--refs'))]


def check_tag_pins(text):
    lines = []
    outdated = False
    for name, url, tag in re.findall(
            r'FetchContent_Declare\(\s*(\S+)\s+GIT_REPOSITORY\s+(\S+)\s+GIT_TAG\s+(\S+)', text):
        m = re.match(r'^(\D*)\d+(\D?)', tag)
        if not m:
            lines.append('- **%s** `%s`: tag format not recognized, check by hand.' % (name, tag))
            continue
        prefix, sep = m.group(1), (m.group(2) or '.')
        pattern = re.compile('^' + re.escape(prefix) + r'\d+(?:' + re.escape(sep) + r'\d+)*$')
        newer = sorted((t for t in remote_tags(url) if pattern.match(t) and version_key(t) > version_key(tag)),
                       key=version_key)
        if newer:
            outdated = True
            repo = url.replace('https://github.com/', '').removesuffix('.git')
            lines.append('- **%s** `%s` → `%s` ([compare](https://github.com/%s/compare/%s...%s))'
                         % (name, tag, newer[-1], repo, tag, newer[-1]))
        else:
            lines.append('- %s `%s` is up to date.' % (name, tag))
    return lines, outdated


def check_commit_pins(text):
    lines = []
    outdated = False
    for repo, sha in re.findall(r'URL https://github\.com/([^/\s]+/[^/\s]+)/archive/([0-9a-f]{40})\.tar\.gz', text):
        url = 'https://github.com/%s.git' % repo
        headers = HEADER_VERSIONS.get(repo)
        if not headers:
            head = git_ls_remote(url, patterns=('HEAD',))
            if not head or head[0][0] == sha:
                lines.append('- %s `%s` matches upstream.' % (repo, sha[:10]))
            else:
                outdated = True
                lines.append('- **%s** `%s`: upstream has new commits ([compare](https://github.com/%s/compare/%s...%s)). '
                             'Update the commit in `URL` and recompute `URL_HASH`.'
                             % (repo, sha[:10], repo, sha[:10], head[0][0][:10]))
            continue
        tags = None
        for path, tag_prefix in headers:
            current, released = header_version(repo, sha, path)
            if not current:
                lines.append('- **%s** `%s`: version not found, check by hand.' % (repo, path))
                continue
            state = '' if released else ' (unreleased snapshot)'
            if tag_prefix:
                if tags is None:
                    tags = remote_tags(url)
                versions = sorted((t[len(tag_prefix):] for t in tags if t.startswith(tag_prefix)), key=version_key)
                latest = versions[-1] if versions else None
                newer = bool(latest) and (version_key(latest) > version_key(current) or
                                          (version_key(latest) == version_key(current) and not released))
                ref = (tag_prefix + latest) if latest else ''
            else:
                ref = git_ls_remote(url, patterns=('HEAD',))[0][0]
                latest, _ = header_version(repo, ref, path)
                newer = bool(latest) and version_key(latest) > version_key(current)
                ref = ref[:10]
            if newer:
                outdated = True
                lines.append('- **%s** `%s` v%s%s → **v%s** released ([compare](https://github.com/%s/compare/%s...%s)). '
                             'Update the commit in `URL` and recompute `URL_HASH`.'
                             % (repo, path, current, state, latest, repo, sha[:10], ref))
            else:
                lines.append('- %s `%s` v%s%s is up to date.' % (repo, path, current, state))
    return lines, outdated


# ---------------------------------------------------------------- SDKs

def omp_sdk_abi(sha):
    try:
        with request('https://raw.githubusercontent.com/openmultiplayer/open.mp-sdk/%s/include/component.hpp' % sha) as r:
            m = re.search(r'#define\s+OMP_VERSION_SUPPORTED\s+(\d+)', r.read().decode('utf-8', 'replace'))
            return m.group(1) if m else None
    except Exception:
        return None


SDK_NAMES = {'OMP_SDK': 'open.mp SDK', 'SAMP_SDK': 'SA-MP plugin SDK'}


def update_sdks(text, check_only):
    """SDK pins in cmake/sdk-pins.txt (NAME_REPO / NAME_COMMIT). The SDKs have no releases, so each
    upstream commit is a change to review. Returns (new_text, lines, changes, abi_changed) where
    changes = [(display name, old, new, abi note or '')]."""
    lines = []
    changes = []
    abi_changed = False
    for key, url in re.findall(r'^([A-Z_]+)_REPO=(\S+)$', text, re.M):
        m = re.search(r'^' + key + r'_COMMIT=([0-9a-f]{40})$', text, re.M)
        if not m:
            sys.exit('%s_COMMIT missing in sdk-pins.txt' % key)
        current = m.group(1)
        head = git_ls_remote(url, patterns=('HEAD',))[0][0]
        repo = url.replace('https://github.com/', '').removesuffix('.git')
        if head == current:
            lines.append('- %s `%s` matches upstream.' % (repo, current[:10]))
            continue
        note = changelog_note = ''
        if key == 'OMP_SDK':
            old_abi, new_abi = omp_sdk_abi(current), omp_sdk_abi(head)
            if old_abi != new_abi:
                abi_changed = True
                note = ' **`OMP_VERSION_SUPPORTED` changed from %s to %s: component ABI change.**' % (old_abi, new_abi)
                changelog_note = ('component ABI %s → %s; needed to load on open.mp servers built with it'
                                  % (old_abi, new_abi))
            else:
                note = ' `OMP_VERSION_SUPPORTED` unchanged (%s).' % old_abi
                changelog_note = 'no component ABI change'
        lines.append('- %s `%s` → `%s` ([compare](https://github.com/%s/compare/%s...%s)).%s'
                     % (repo, current[:10], head[:10], repo, current[:10], head[:10], note))
        changes.append((SDK_NAMES.get(key, repo), current[:10], head[:10], changelog_note))
        if not check_only:
            text = text.replace('%s_COMMIT=%s' % (key, current), '%s_COMMIT=%s' % (key, head))
    return text, lines, changes, abi_changed


# ---------------------------------------------------------------- changelog

UNRELEASED = '### Unreleased'


def read(path):
    with open(path, encoding='utf-8', newline='') as f:
        return f.read()


def write(path, text):
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)


def add_unreleased_changelog(entries):
    """Appends changelog entries to the "### Unreleased" section of README.md (created at the top of
    the Version history when missing). The version number is chosen later by the Release workflow."""
    text = read(README)
    nl = '\r\n' if '\r\n' in text else '\n'
    lines = text.split(nl)
    if UNRELEASED not in lines:
        first = next((i for i, line in enumerate(lines) if re.match(r'### \d+\.\d+\.\d+ ', line)), None)
        if first is None:
            sys.exit('README.md: no version history found')
        lines[first:first] = [UNRELEASED, '']
    start = lines.index(UNRELEASED)
    end = start + 1
    while end < len(lines) and not lines[end].startswith('### ') and not lines[end].startswith('</details>'):
        end += 1
    while end - 1 > start and not lines[end - 1].strip():
        end -= 1
    new_lines = [line for entry in entries for line in entry.split('\n')]
    lines[end:end] = new_lines
    after = end + len(new_lines)
    if after < len(lines) and lines[after].strip():
        lines.insert(after, '')
    write(README, nl.join(lines))


def changelog_entries(runtime_changes, sdk_changes):
    entries = []
    if runtime_changes:
        entries.append('- **Updated JS Runtimes** (used by `yt-dlp` for YouTube):\n' + '\n'.join(
            '  - %s %s → %s.' % (n, old, new) for n, old, new in runtime_changes))
    if sdk_changes:
        entries.append('- **Updated Platform SDKs**:\n' + '\n'.join(
            '  - %s `%s` → `%s`%s.' % (n, old, new, (' (%s)' % note) if note else '')
            for n, old, new, note in sdk_changes))
    return entries


# ---------------------------------------------------------------- main

RELEASE_HOW = ('run **Actions → Release** and choose **patch** (or minor/major if other unreleased '
               'changes call for it, or **auto** if a version was already written by hand in README.md); '
               'it sets the version and publishes the release.')


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser()
    ap.add_argument('--pr-report', default=None)
    ap.add_argument('--library-report', default=None)
    ap.add_argument('--commit-message', default=None)
    ap.add_argument('--check-only', action='store_true', help='report only; do not download or rewrite')
    ap.add_argument('--force', action='append', default=[], metavar='NAME=TAG',
                    help='use this release instead of the latest (testing), e.g. QuickJS-ng=v0.18.0')
    args = ap.parse_args()
    forced = dict(f.split('=', 1) for f in args.force)

    source = read(BINARY_MANAGER)
    new_source, runtime_lines, runtime_changes = update_runtimes(source, forced, args.check_only)
    if new_source != source:
        write(BINARY_MANAGER, new_source)

    pins = read(SDK_PINS)
    new_pins, sdk_lines, sdk_changes, abi_changed = update_sdks(pins, args.check_only)
    if new_pins != pins:
        write(SDK_PINS, new_pins)

    deps = read(DEPENDENCIES)
    tag_lines, tags_outdated = check_tag_pins(deps)
    commit_lines, commits_outdated = check_commit_pins(deps)
    outdated = tags_outdated or commits_outdated

    m = re.search(r'#define SAMPAUDIOSTREAM_VERSION_STRING "(\d+\.\d+\.\d+)"', read(INCLUDE))
    base = m.group(1) if m else 'unknown'
    pr_needed = bool(runtime_changes or sdk_changes)
    needs_release = bool(runtime_changes) or abi_changed
    if pr_needed and not args.check_only:
        add_unreleased_changelog(changelog_entries(runtime_changes, sdk_changes))

    notice = []
    if needs_release:
        notice += ['> [!IMPORTANT]',
                   '> **Needs a release to reach servers.** The changelog line is already in the `%s` section '
                   'of README.md. After merging, %s' % (UNRELEASED, RELEASE_HOW)]
        if runtime_changes:
            notice += ['>', '> **Urgency:** not urgent by itself. Servers keep working with the current runtimes. '
                       'It becomes urgent when the `yt-dlp` release notes say this runtime version (or newer) is '
                       'required: until a release ships, YouTube playback can fail on servers (direct links and '
                       'radios keep working).']
        if abi_changed:
            notice += ['>', '> **Urgency (open.mp ABI):** servers on their current open.mp version and SA-MP servers '
                       'keep working. Release before servers update to an open.mp version built with this SDK: '
                       'they will not load the component until the release ships.']
    elif pr_needed:
        notice += ['> [!NOTE]',
                   '> **No release needed.** Nothing changes for servers; the changelog line is in the `%s` '
                   'section of README.md and goes out with the next release.' % UNRELEASED]

    sections = []
    if runtime_changes:
        sections += ['### JS runtimes (`src/extractor/binary_manager.cpp`)', '', *runtime_lines, '',
                     'URLs and SHA-256 hashes were recomputed from the release assets. Before merging: check the '
                     'release notes for breaking changes, wait for the Build workflow started on this branch, and '
                     'test YouTube playback on a server.', '']
    if sdk_changes:
        sections += ['### Platform SDKs (`cmake/sdk-pins.txt`)', '', *sdk_lines, '',
                     'Before merging: read the compare link, wait for the Build workflow started on this branch, and '
                     'test the component on the latest open.mp server. For local builds, delete `deps/open.mp-sdk` '
                     '(or `deps/samp-plugin-sdk`) and clone it again at the new commit.', '']
    based = ('Generated from **SampAudioStream %s** (the version in the repository files, i.e. the latest '
             'release); the version number of the next release is chosen when it is published.' % base)
    pr_report = '\n'.join([*notice, '', based, '', *sections]).strip()

    library_report = '\n'.join([
        'Weekly check of the libraries pinned in `cmake/Dependencies.cmake`, by release (tags, or the '
        'version in the header the plugin uses). These decode audio downloaded from the internet, so '
        'update them by hand after reading the changes, giving priority to security fixes.',
        '',
        '**Urgency:** nothing breaks while they wait; prioritize security fixes. After updating, add a line '
        'to the `%s` section of README.md, then %s' % (UNRELEASED, RELEASE_HOW),
        '',
        *tag_lines,
        *commit_lines,
    ])
    print('Runtimes:', *runtime_lines, 'SDKs:', *sdk_lines, sep='\n')
    if pr_report:
        print('\n--- pull request ---\n' + pr_report)
    print('\n--- library issue ---\n' + library_report)
    for path, text in ((args.pr_report, pr_report), (args.library_report, library_report)):
        if path:
            write(path, text + '\n')

    names = ['%s-%s' % (n.lower().replace('-', ''), new) for n, _, new in runtime_changes] + \
            ['%s-%s' % (n.split()[0].lower().replace('.', ''), new) for n, _, new, _ in sdk_changes]
    items = ['%s to %s' % (n, new) for n, _, new in runtime_changes] + \
            ['%s to %s' % (n, new) for n, _, new, _ in sdk_changes]
    title = 'Update ' + (', '.join(items[:-1]) + ' and ' + items[-1] if len(items) > 1 else ''.join(items))
    message_lines = (['- %s %s → %s: version constant, download URLs and SHA-256 hashes in '
                     'src/extractor/binary_manager.cpp' % c for c in runtime_changes] +
                    ['- %s %s → %s in cmake/sdk-pins.txt%s' % (n, old, new, (' (%s)' % note) if note else '')
                     for n, old, new, note in sdk_changes] +
                    ['- Changelog line added under "### Unreleased" in README.md'])
    if pr_needed and args.commit_message:
        write(args.commit_message, '%s (based on v%s)\n\n%s\n' % (title, base, '\n'.join(message_lines)))
    out = os.environ.get('GITHUB_OUTPUT')
    if out:
        with open(out, 'a', encoding='utf-8') as f:
            f.write('pr_needed=%s\n' % ('true' if pr_needed else 'false'))
            f.write('pr_branch=deps/update-%s\n' % '_'.join(names))
            f.write('pr_title=[SampAudioStream %s] %s\n' % (base, title))
            f.write('base_version=%s\n' % base)
            f.write('needs_release=%s\n' % ('true' if needs_release else 'false'))
            f.write('libraries_outdated=%s\n' % ('true' if outdated else 'false'))


if __name__ == '__main__':
    main()
