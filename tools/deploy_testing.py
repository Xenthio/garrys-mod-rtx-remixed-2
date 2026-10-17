"""Install a verified runtime bundle into a separate, persistent GMod test host.

Settings are local JSON: source, target, proxy, package, native_repo; optional mods.
The initial target may be absent or contain only RTXLauncher bootstrap files.
Updates preserve game content, user configuration, mods and bake caches.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from stage_game import prepare


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def child(root, relative):
    path = root / relative
    if Path(relative).is_absolute() or root not in path.resolve().parents:
        raise ValueError(f'Path leaves installation: {relative}')
    # Do not replace files through junctions, including ones pointing inward.
    for part in (path, *path.parents):
        if part == root:
            break
        if part.exists() and part.lstat().st_file_attributes & 0x400:
            raise ValueError(f'Reparse point in installation: {part}')
    return path


def idle(target):
    script = 'Get-Process | Where-Object {$_.Path} | Select-Object -ExpandProperty Path | ConvertTo-Json'
    result = subprocess.run(['powershell.exe', '-NoProfile', '-Command', script],
                            check=True, capture_output=True, text=True)
    processes = json.loads(result.stdout or '[]')
    if isinstance(processes, str):
        processes = [processes]
    for executable in processes:
        if target in Path(executable).resolve().parents:
            raise RuntimeError(f'Close the test installation before updating: {executable}')


def revision(repo):
    return subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()


def deploy(settings_path, verify=False):
    settings = json.loads(settings_path.read_text(encoding='utf-8-sig'))
    paths = {name: Path(settings[name]).resolve() for name in
             ('source', 'target', 'proxy', 'package', 'native_repo')}
    source, target = paths['source'], paths['target']
    proxy, package, native = paths['proxy'], paths['package'], paths['native_repo']
    wrapper = Path(__file__).resolve().parents[1]
    if target == source or source in target.parents or target in source.parents:
        raise ValueError('Test host must be separate from the source installation')
    original_target = Path(settings['target']).absolute()
    if original_target != target:
        raise ValueError('Test target must not redirect through a junction')
    manifest = json.loads((package / 'runtime-manifest.json').read_text())
    copies = [(proxy, Path('bin/win64/shaderapidx9.dll'))]
    for relative, entry in manifest['files'].items():
        asset = child(package, relative)
        if digest(asset) != entry['sha256']:
            raise ValueError(f'Package hash mismatch: {asset}')
        copies.append((asset, Path('bin/win64/openremix') / relative))
    copies.append((package / 'runtime-manifest.json', Path('bin/win64/openremix/runtime-manifest.json')))
    for asset, relative in copies:
        child(target, relative)
    if verify:
        installed = json.loads((target / 'openremix-install.json').read_text())
        for relative, expected in installed['files'].items():
            if digest(child(target, relative)) != expected:
                raise ValueError(f'Installed hash mismatch: {relative}')
        print(f'Verified {len(installed["files"])} installed files: {target}')
        return
    idle(target)
    native_revision, wrapper_revision = revision(native), revision(wrapper)
    if not (target / 'stage-manifest.json').exists():
        prepare(source, target, proxy, package / 'remix.dll', package / 'SDL3_shared.dll',
                launcher_only=True)
    stage = json.loads((target / 'stage-manifest.json').read_text())
    if Path(stage['contentRoot']).resolve() != target:
        raise ValueError('Stage manifest belongs to a different installation')
    # Only files owned by this deployment are updated. Keep rollback copies.
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    backup = child(target, Path('openremix-backups') / stamp)
    backup.mkdir(parents=True)
    generated = backup / 'generated'
    generated.mkdir()
    shutil.copy2(wrapper / 'tools/launch_testing.ps1', generated / 'Play OpenRemix.ps1')
    (generated / 'Play OpenRemix.cmd').write_text(
        '@echo off\r\npowershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Play OpenRemix.ps1" %*\r\n',
        encoding='ascii')
    # The updater always uses the same local settings, which can select a new package.
    update = f'@echo off\r\n"{sys.executable}" "{Path(__file__).resolve()}" --settings "{settings_path.resolve()}"\r\npause\r\n'
    (generated / 'Update OpenRemix.cmd').write_text(update, encoding='utf-8')
    copies += [(asset, Path(asset.name)) for asset in generated.iterdir()]
    # Seed a private copy once. Future generated replacements are user-owned.
    if settings.get('mods') and not (target / 'rtx-remix/mods').exists():
        mods = Path(settings['mods']).resolve(strict=True)
        for asset in mods.rglob('*'):
            if asset.is_file():
                copies.append((asset, Path('rtx-remix/mods') / asset.relative_to(mods)))
    changed = []
    hashes = {}
    try:
        for asset, relative in copies:
            destination = child(target, relative)
            hashes[relative.as_posix()] = digest(asset)
            if destination.exists() and digest(destination) == hashes[relative.as_posix()]:
                continue
            previous = backup / 'previous' / relative
            existed = destination.exists()
            if existed:
                previous.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(destination, previous)
            destination.parent.mkdir(parents=True, exist_ok=True)
            temporary = destination.with_name(destination.name + '.deploying')
            shutil.copy2(asset, temporary)
            changed.append((destination, previous, existed))
            os.replace(temporary, destination)
            if digest(destination) != hashes[relative.as_posix()]:
                raise ValueError(f'Copy verification failed: {destination}')
        # Generated materials may change while playing; they are no longer ours
        # after seeding. Only immutable deployment files belong in verification.
        owned_hashes = {name: value for name, value in hashes.items()
                        if not name.startswith('rtx-remix/mods/')}
        record = dict(installedAt=stamp, nativeSourceHead=native_revision,
                      wrapperSourceHead=wrapper_revision, package=str(package), files=owned_hashes)
        record_path = target / 'openremix-install.json'
        if record_path.exists():
            shutil.copy2(record_path, backup / record_path.name)
        temporary_record = record_path.with_suffix('.deploying')
        temporary_record.write_text(json.dumps(record, indent=2) + '\n')
        os.replace(temporary_record, record_path)
    except Exception:
        for destination, previous, existed in reversed(changed):
            if existed:
                shutil.copy2(previous, destination)
            else:
                destination.unlink(missing_ok=True)
        raise
    print(f'Installed {len(hashes)} verified files into {target}')
    print(f'Native {native_revision[:8]}, wrapper {wrapper_revision[:8]}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--settings', required=True, type=Path)
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    try:
        deploy(args.settings, args.verify)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Deployment failed: {error}\n')
