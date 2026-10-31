"""Package the accepted 0.2.1 runtime without rebuilding its DLLs."""
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import urllib.request
import urllib.parse
import zipfile

ROOT = Path(__file__).resolve().parent.parent
REPO = 'Aerox912/shroudtopia'
RUN = 39664973554
ARTIFACT = 12377537617
RUNTIME_SOURCE = '7203f907481ba7706341b2f6b29a5f8b438d70e1'
DOCS_SOURCE = '292df3c3ddb90788c270a29662db4886518ee660'
INPUT_SHA = 'b9551c44a2166d74312a2a122d5c7be7297eee90e57aeded625b382ba3f6ecb2'
NAME = 'Shroudweave-Shroudtopia-0.2.1-shroudweave.1.zip'
DLLS = {
    'shroudtopia.dll': '7a8fa4b339dfc20e6d800fbb01b9d298d57ee7f623e14c606367db206fd385c5',
    'winmm.dll': '75159793a140fa37d506d3096e863ee209bac6738d195a22153a444ce52da9c9',
}

def sha(data):
    return hashlib.sha256(data).hexdigest()

def require(condition, message):
    if not condition:
        raise RuntimeError(message)

def read_zip(data):
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        names = archive.namelist()
        require(len(names) == len(set(names)), 'Duplicate archive member')
        return {name: archive.read(name) for name in names if not name.endswith('/')}

def git(*args):
    return subprocess.check_output(['git', '-C', str(ROOT), *args]).decode().strip()

def request(url):
    # The token stays in the request header, never in subprocess arguments/output.
    req = urllib.request.Request(url, headers={
        'Authorization': 'Bearer ' + os.environ['GH_TOKEN'],
        'Accept': 'application/vnd.github+json',
        'X-GitHub-Api-Version': '2022-11-28',
    })
    class SafeRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, old, response, code, message, headers, new_url):
            redirected = super().redirect_request(old, response, code, message, headers, new_url)
            if urllib.parse.urlsplit(old.full_url).netloc != urllib.parse.urlsplit(new_url).netloc:
                redirected.remove_header('Authorization')
            return redirected
    with urllib.request.build_opener(SafeRedirect()).open(req, timeout=120) as response:
        return response.read()

def main():
    require(os.environ.get('GITHUB_REPOSITORY') == REPO, 'Unexpected repository')
    checkout = git('rev-parse', 'HEAD')
    head = os.environ['PACKAGING_SOURCE_COMMIT']
    require(git('rev-parse', head) == head, 'Invalid packaging source commit')
    for path in ('docs/releases/0.2.1-shroudweave.1.md',
                 'ci/package-shroudtopia-release.ps1', 'release/default-configs/shroudtopia.json'):
        require(git('rev-parse', f'{DOCS_SOURCE}:{path}') == git('rev-parse', f'HEAD:{path}'),
                'Reviewed packaging input changed: ' + path)
    changed = git('diff', '--name-only', RUNTIME_SOURCE, 'HEAD').splitlines()
    allowed = {'docs/releases/0.2.1-shroudweave.1.md', 'ci/package-shroudtopia-release.ps1',
               'ci/test-shroudtopia-package.ps1', 'ci/package-tested-shroudtopia.py',
               '.github/workflows/package-tested-shroudtopia.yml'}
    require(set(changed) <= allowed, 'Runtime source differs from the tested source')
    metadata = json.loads(request(f'https://api.github.com/repos/{REPO}/actions/artifacts/{ARTIFACT}'))
    require(metadata['id'] == ARTIFACT and not metadata['expired'], 'Invalid/expired input artifact')
    require(metadata['name'] == 'shroudtopia-release', 'Unexpected input artifact name')
    require(metadata['workflow_run']['id'] == RUN and
            metadata['workflow_run']['head_sha'] == RUNTIME_SOURCE, 'Input run/source mismatch')
    outer = read_zip(request(f'https://api.github.com/repos/{REPO}/actions/artifacts/{ARTIFACT}/zip'))
    require(NAME in outer and sha(outer[NAME]) == INPUT_SHA, 'Input package hash mismatch')
    old = read_zip(outer[NAME])
    original_info = json.loads(old['build-info.json'].decode('utf-8-sig'))
    require(original_info['sourceCommit'] == RUNTIME_SOURCE and
            str(original_info['workflowRunId']) == str(RUN), 'Input provenance mismatch')
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    for name, digest in DLLS.items():
        require(sha(old[name]) == digest, 'Tested DLL hash mismatch: ' + name)
        target = build / name
        require(not target.exists(), 'Refusing to replace an existing build DLL')
        target.write_bytes(old[name])
    subprocess.run(['pwsh', '-NoProfile', '-File', str(ROOT / 'ci/package-shroudtopia-release.ps1')],
                   cwd=ROOT, check=True)
    archive = ROOT / 'dist' / NAME
    members = read_zip(archive.read_bytes())
    for name, digest in DLLS.items():
        require(sha(members[name]) == digest, 'Output DLL changed: ' + name)
    info = json.loads(members['build-info.json'].decode('utf-8-sig'))
    info['sourceCommit'] = RUNTIME_SOURCE
    info['workflowRunId'] = str(RUN)
    info['runtime'] = {'sourceCommit': RUNTIME_SOURCE, 'workflowRunId': RUN,
                       'artifactId': ARTIFACT, 'packageSha256': INPUT_SHA, 'dlls': DLLS}
    info['packaging'] = {'sourceCommit': head, 'checkoutCommit': checkout,
                         'reviewedDocsCommit': DOCS_SOURCE,
                         'workflowRunId': os.environ['GITHUB_RUN_ID'],
                         'policy': 'reuse-exact-user-tested-runtime'}
    members['build-info.json'] = (json.dumps(info, indent=2) + '\n').encode()
    members['SHA256SUMS.txt'] = ''.join(
        f'{sha(data)}  {name}\n' for name, data in sorted(members.items())
        if name != 'SHA256SUMS.txt').encode('ascii')
    added = sorted(members.keys() - old.keys())
    removed = sorted(old.keys() - members.keys())
    modified = sorted(n for n in old.keys() & members.keys() if old[n] != members[n])
    require(added == ['examples/shroudtopia.default.json'] and removed == ['shroudtopia.json'],
            'Unexpected package layout difference')
    require(modified == ['INSTALL.txt', 'RELEASE-NOTES.md', 'SHA256SUMS.txt', 'build-info.json'],
            'Unexpected changed members')
    require(members['examples/shroudtopia.default.json'] == old['shroudtopia.json'], 'Default changed')
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as output:
        for name, data in sorted(members.items()):
            output.writestr(name, data)
    actual = read_zip(archive.read_bytes())
    require(actual == members, 'Output archive content mismatch')
    for line in actual['SHA256SUMS.txt'].decode().splitlines():
        digest, name = line.split('  ', 1)
        require(sha(actual[name]) == digest, 'Member checksum mismatch: ' + name)
    # Exercise actual extract-all against a configured upgrade target.
    upgrade = ROOT / 'upgrade-fixture'
    upgrade.mkdir()
    existing = b'{"mods":{"minimap_mod":{"enabled":true}},"bootDelay":4321}'
    (upgrade / 'shroudtopia.json').write_bytes(existing)
    with zipfile.ZipFile(archive) as output:
        output.extractall(upgrade)
    require((upgrade / 'shroudtopia.json').read_bytes() == existing, 'Upgrade config overwritten')
    digest = sha(archive.read_bytes())
    archive.with_suffix('.zip.sha256').write_text(f'{digest}  {NAME}\n', encoding='ascii')
    receipt = {'schema': 'shroudtopia-tested-runtime-actions-package-v1',
               'runtime': info['runtime'], 'packaging': info['packaging'],
               'packageSha256': digest, 'packageBytes': archive.stat().st_size,
               'members': {name: sha(data) for name, data in sorted(actual.items())},
               'memberDiff': {'added': added, 'removed': removed, 'changed': modified},
               'checks': {'exactTestedDlls': True, 'allMemberChecksums': True,
                          'extractAllPreservesExistingConfig': True, 'noRootDefaultConfig': True},
               'runtimeRebuilt': False, 'publicationAccepted': False}
    (ROOT / 'dist/TESTED-PACKAGE-RECEIPT.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2))

if __name__ == '__main__':
    main()
