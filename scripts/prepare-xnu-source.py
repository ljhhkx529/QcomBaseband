#!/usr/bin/env python3
"""Restore omitted DriverKit headers from Apple's earlier Darwin 23 source."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import urllib.error
import urllib.parse
import urllib.request

# Tags of the Apple sources that still carry the headers newer xnu omits; feeds.toml sets them for the unixnu driver.
TAG = os.environ.get('UNIXNU_DRIVERKIT_TAG', 'xnu-10063.121.3')
TRUST_TAG = os.environ.get('UNIXNU_TRUSTCACHE_TAG', 'xnu-8796.101.5')
DESTINATION = Path('.cache/xnu-work/iokit/DriverKit')
CACHE = Path(f'.cache/sources/xnu-driverkit-{TAG.removeprefix("xnu-")}.json')
LOCAL_STORE = Path('.cache/sources/driverkit-headers')
URL = f'https://api.github.com/repos/apple-oss-distributions/xnu/contents/iokit/DriverKit?ref={TAG}'
# Anonymous GitHub API calls share a small per-address quota that hosted CI runners exhaust (HTTP 403);
# a token in GITHUB_TOKEN or GH_TOKEN is sent to GitHub hosts only.
TOKEN = os.environ.get('GITHUB_TOKEN') or os.environ.get('GH_TOKEN')


def fetch(url):
    request = urllib.request.Request(url, headers={'User-Agent': 'unixnu-prepare-xnu-source'})
    authorize = TOKEN and urllib.parse.urlsplit(url).hostname in ('api.github.com', 'raw.githubusercontent.com')
    if authorize:
        request.add_header('Authorization', f'Bearer {TOKEN}')
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.read()
    except (urllib.error.URLError, OSError) as error:
        # Some hosts fail the TLS handshake from this Python build while curl reaches them fine
        # (SSL: UNEXPECTED_EOF_WHILE_READING), which aborted a kernel build with every patch
        # already applied. Fall back to curl rather than failing the build.
        command = ['curl', '-sSL', '--retry', '3', '--retry-delay', '1', '--max-time', '40',
                   '-A', 'unixnu-prepare-xnu-source']
        if authorize:
            command += ['-H', f'Authorization: Bearer {TOKEN}']
        completed = subprocess.run(command + [url], capture_output=True)
        if completed.returncode == 0 and completed.stdout:
            return completed.stdout
        raise SystemExit(f'fetch failed for {url}: {error}') from error


def restore(entry, destination=DESTINATION, tag=TAG):
    if entry['type'] != 'file' or not entry['name'].endswith('.h'):
        return None
    name = entry.get('relative', entry['name'])
    target = destination / name
    if target.exists():
        return None
    # A prepared tree is recreated whenever the recipe inputs change, which would otherwise repeat
    # every download. Keep a copy under .cache/sources so an offline or TLS-blocked build still works.
    stored = LOCAL_STORE / name
    if stored.is_file():
        data = stored.read_bytes()
        digest = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
        if digest == entry['sha']:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            return {'name': name, 'source_tag': tag, 'git_blob': digest,
                    'sha256': hashlib.sha256(data).hexdigest()}
        stored.unlink()
    data = fetch(entry['download_url'])
    digest = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
    if digest != entry['sha']:
        raise ValueError(f'Git blob hash mismatch: {entry["name"]}')
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    return {'name': name, 'source_tag': tag, 'git_blob': digest,
            'sha256': hashlib.sha256(data).hexdigest()}


if __name__ == '__main__':
    if not DESTINATION.is_dir():
        raise SystemExit('Create the local XNU work source before preparing headers')
    if not CACHE.exists():
        CACHE.parent.mkdir(parents=True, exist_ok=True)
        CACHE.write_bytes(fetch(URL))
    entries = json.loads(CACHE.read_text())
    crypto_cache = CACHE.with_name(f'xnu-driverkit-crypto-{TAG.removeprefix("xnu-")}.json')
    if not crypto_cache.exists():
        crypto_url = URL.replace('/DriverKit?', '/DriverKit/crypto?')
        crypto_cache.write_bytes(fetch(crypto_url))
    for entry in json.loads(crypto_cache.read_text()):
        entry['relative'] = 'crypto/' + entry['name']
        entries.append(entry)
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        restored = [entry for entry in executor.map(restore, entries) if entry]
        trust_tag = TRUST_TAG
        trust_cache = CACHE.with_name(f'xnu-trustcache-{TRUST_TAG.removeprefix("xnu-")}.json')
        if not trust_cache.exists():
            url = ('https://api.github.com/repos/apple-oss-distributions/xnu/'
                   f'contents/EXTERNAL_HEADERS/TrustCache?ref={trust_tag}')
            trust_cache.write_bytes(fetch(url))
        trust_destination = Path('.cache/xnu-work/EXTERNAL_HEADERS/TrustCache')
        def restore_trust(entry):
            return restore(entry, trust_destination, trust_tag)
        restored.extend(entry for entry in executor.map(
            restore_trust, json.loads(trust_cache.read_text())) if entry)
    previous = Path('.cache/sources/xnu-restored-headers.json')
    existing = json.loads(previous.read_text()) if previous.exists() else []
    by_name = {entry['name']: entry for entry in existing + restored}
    previous.write_text(json.dumps(list(by_name.values()), indent=2) + '\n')
    print(f'Restored {len(restored)} omitted public headers from Apple; blob hashes verified')
