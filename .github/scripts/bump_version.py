#!/usr/bin/env python3
"""Prepares a release: sets the plugin version in every file that carries it and turns the pending
changelog of README.md into "### X.Y.Z — DD/MM/YYYY" (the release day).

Usage: python -I .github/scripts/bump_version.py auto|patch|minor|major|X.Y.Z

  auto                 Use the version already written by hand at the top of the Version history
                       ("### X.Y.Z — ..." not released yet). A "### Unreleased" section, if any, is
                       merged into it. Fails when there is no such version (choose a type instead).
  patch|minor|major    Next version after the latest release, for the "### Unreleased" changes.
  X.Y.Z                That exact version, for the "### Unreleased" changes.

The Release workflow runs it (Actions → Release); run it by hand only to prepare a release locally.
Prints "version=X.Y.Z" on the last line.
"""
import datetime
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
UNRELEASED = '### Unreleased'
VERSION_HEADING = re.compile(r'^### (\d+\.\d+\.\d+) — ')

# Files that carry the version (README.md, the 7th, carries the changelog heading).
VERSION_FILES = {
    ('CMakeLists.txt',): [(r'(project\(SampAudioStream VERSION )\d+\.\d+\.\d+', r'\g<1>{v}')],
    ('pawn.json',): [(r'("version": ")\d+\.\d+\.\d+"', r'\g<1>{v}"')],
    ('SampAudioStream.inc',): [
        (r'( \* Version: )\d+\.\d+\.\d+', r'\g<1>{v}'),
        (r'(#define SAMPAUDIOSTREAM_VERSION_MAJOR  \()\d+', r'\g<1>{major}'),
        (r'(#define SAMPAUDIOSTREAM_VERSION_MINOR  \()\d+', r'\g<1>{minor}'),
        (r'(#define SAMPAUDIOSTREAM_VERSION_PATCH  \()\d+', r'\g<1>{patch}'),
        (r'(#define SAMPAUDIOSTREAM_VERSION_STRING ")\d+\.\d+\.\d+"', r'\g<1>{v}"'),
    ],
    ('src', 'main.cpp'): [(r'(SampAudioStream v)\d+\.\d+\.\d+ ', r'\g<1>{v} ')],
    ('src', 'runtime', 'omp_host.cpp'): [(r'(SampAudioStream v)\d+\.\d+\.\d+ ', r'\g<1>{v} ')],
    ('include', 'sampaudiostream', 'runtime', 'omp_host.hpp'): [
        (r'(componentVersion\(\) const override \{ return SemanticVersion\()\d+, \d+, \d+, 0\)',
         r'\g<1>{major}, {minor}, {patch}, 0)')],
}


def path(*parts):
    return os.path.join(ROOT, *parts)


def read(p):
    with open(p, encoding='utf-8', newline='') as f:
        return f.read()


def write(p, text):
    with open(p, 'w', encoding='utf-8', newline='') as f:
        f.write(text)


def vkey(v):
    return tuple(int(x) for x in v.split('.'))


def files_version():
    m = re.search(r'#define SAMPAUDIOSTREAM_VERSION_STRING "(\d+\.\d+\.\d+)"', read(path('SampAudioStream.inc')))
    if not m:
        sys.exit('SAMPAUDIOSTREAM_VERSION_STRING not found in SampAudioStream.inc')
    return m.group(1)


def released_versions():
    try:
        out = subprocess.run(['git', 'tag', '-l', 'v*'], cwd=ROOT, check=True, capture_output=True, text=True).stdout
    except Exception:
        return []
    return [t[1:] for t in out.split() if re.fullmatch(r'v\d+\.\d+\.\d+', t)]


def latest_release():
    released = released_versions()
    return max(released, key=vkey) if released else files_version()


def section(lines, start):
    """Index range (start, end) of the section whose heading is lines[start]."""
    end = start + 1
    while end < len(lines) and not lines[end].startswith('### ') and not lines[end].startswith('</details>'):
        end += 1
    return start, end


def body_text(lines, start):
    s, e = section(lines, start)
    return '\n'.join(lines[s + 1:e]).strip()


def unreleased_body(readme=None):
    """Text of the "### Unreleased" section ('' when missing or empty)."""
    lines = (read(path('README.md')) if readme is None else readme).replace('\r\n', '\n').split('\n')
    return body_text(lines, lines.index(UNRELEASED)) if UNRELEASED in lines else ''


def pending_version(lines):
    """Version written by hand at the top of the history and not released yet (or None)."""
    released = set(released_versions())
    last = latest_release()
    for line in lines:
        m = VERSION_HEADING.match(line)
        if m:
            v = m.group(1)
            return v if v not in released and vkey(v) > vkey(last) else None
    return None


def next_version(base, kind):
    major, minor, patch = vkey(base)
    return {'patch': (major, minor, patch + 1), 'minor': (major, minor + 1, 0), 'major': (major + 1, 0, 0)}[kind]


def set_version_files(version):
    major, minor, patch = vkey(version)
    for parts, pairs in VERSION_FILES.items():
        p = path(*parts)
        text = read(p)
        for pattern, repl in pairs:
            text, n = re.subn(pattern, repl.format(v=version, major=major, minor=minor, patch=patch), text)
            if n != 1:
                sys.exit('%s: expected 1 match for %s, found %d' % ('/'.join(parts), pattern, n))
        write(p, text)


def prepare(kind, date=None):
    text = read(path('README.md'))
    nl = '\r\n' if '\r\n' in text else '\n'
    lines = text.split(nl)
    heading_date = (date or datetime.date.today()).strftime('%d/%m/%Y')
    has_unreleased = UNRELEASED in lines and bool(body_text(lines, lines.index(UNRELEASED)))
    pending = pending_version(lines)

    if kind == 'auto':
        if not pending:
            if has_unreleased:
                sys.exit('README.md has "%s" changes but no version written by hand: run the release '
                         'again choosing patch, minor or major.' % UNRELEASED)
            sys.exit('Nothing to release: README.md has no "%s" changes and no unreleased version section.'
                     % UNRELEASED)
        version = pending
        idx = next(i for i, line in enumerate(lines) if line.startswith('### %s — ' % version))
        if UNRELEASED in lines:
            u_start, u_end = section(lines, lines.index(UNRELEASED))
            moved = lines[u_start + 1:u_end]
            while moved and not moved[0].strip():
                moved.pop(0)
            while moved and not moved[-1].strip():
                moved.pop()
            del lines[u_start:u_end]
            idx = next(i for i, line in enumerate(lines) if line.startswith('### %s — ' % version))
            _, v_end = section(lines, idx)
            while v_end - 1 > idx and not lines[v_end - 1].strip():
                v_end -= 1
            lines[v_end:v_end] = moved
        lines[idx] = '### %s — %s' % (version, heading_date)
    else:
        if pending:
            sys.exit('README.md already has version %s written by hand and not released: run the release '
                     'again choosing auto (or remove that section).' % pending)
        if not has_unreleased:
            sys.exit('Nothing to release: README.md has no "%s" changes.' % UNRELEASED)
        if kind in ('patch', 'minor', 'major'):
            version = '%d.%d.%d' % next_version(latest_release(), kind)
        elif re.fullmatch(r'\d+\.\d+\.\d+', kind):
            version = kind
        else:
            sys.exit(__doc__)
        if version in released_versions() or vkey(version) <= vkey(latest_release()):
            sys.exit('version %s is not above the latest release %s' % (version, latest_release()))
        lines[lines.index(UNRELEASED)] = '### %s — %s' % (version, heading_date)

    if not body_text(lines, next(i for i, line in enumerate(lines) if line.startswith('### %s — ' % version))):
        sys.exit('README.md: the changelog of %s is empty' % version)
    set_version_files(version)
    write(path('README.md'), nl.join(lines))
    return version


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    version = prepare(sys.argv[1])
    print('version=%s' % version)


if __name__ == '__main__':
    main()
