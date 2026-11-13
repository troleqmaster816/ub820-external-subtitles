"""Verify the shipped CPU component or rebuild a SEPARATE candidate.

Normal helper builds embed the byte-pinned prebuilt module; no kernel/toolchain
download is needed. Rebuilding requires exact external headers from the lock.
This script never loads a module, accesses a player, or overwrites the asset.
"""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import json
import struct
import subprocess

HERE = Path(__file__).resolve().parent


def require(value, message):
    if not value: raise RuntimeError(message)


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def named(root, relative):
    parts = PurePosixPath(relative)
    require(not parts.is_absolute() and '..' not in parts.parts and parts.suffix.lower() != '.mkv', 'Unsafe input name')
    return root.joinpath(*parts.parts)


def verify_package():
    manifest = json.loads((HERE / 'manifest.json').read_text(encoding='utf-8'))
    asset = manifest['artifact']
    require(asset['file'] == 'ub820_cpu_wait_v15.ko', 'Unexpected runtime asset name')
    binary = HERE / asset['file']
    require(binary.stat().st_size == asset['bytes'] and sha(binary) == asset['sha256'], 'Runtime asset hash/size differs')
    for name, expected in manifest['source_sha256'].items():
        require(PurePosixPath(name).name == name and Path(name).suffix in ('.c', '.h', '.S'), 'Unexpected own source name')
        require(sha(HERE / name) == expected, 'Frozen source differs: ' + name)
    data = binary.read_bytes()
    require(data[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<HH', data, 16) == (1, 183), 'Expected AArch64 ELF64 relocatable module')
    section_offset = struct.unpack_from('<Q', data, 40)[0]
    width, count, strings_index = struct.unpack_from('<HHH', data, 58)
    require(width == 64 and strings_index < count and section_offset + count * width <= len(data), 'Invalid ELF section table')
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, section_offset + index * width) for index in range(count)]
    strings = sections[strings_index]
    names = data[strings[4]:strings[4] + strings[5]]
    by_name = {}
    for section in sections:
        name = names[section[0]:].split(b'\0', 1)[0].decode('ascii')
        by_name[name] = section
    descriptor = by_name['.gnu.linkonce.this_module']
    require(descriptor[5] == 768 and descriptor[8] == 128, 'Native module descriptor differs')
    payload = data[descriptor[4]:descriptor[4] + descriptor[5]]
    require(payload[24:24 + len(asset['module_name']) + 1] == asset['module_name'].encode() + b'\0', 'Native module name differs')
    info = by_name['.modinfo']
    entries = data[info[4]:info[4] + info[5]].split(b'\0')
    for key in ('vermagic', 'license'):
        require((key + '=' + asset[key]).encode() in entries, 'Module ' + key + ' differs')
    require(('version=' + asset['embedded_version']).encode() in entries, 'Frozen embedded version differs')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify-package', action='store_true')
    parser.add_argument('--kernel-source', type=Path)
    parser.add_argument('--kernel-generated', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--cc', default='aarch64-linux-gnu-gcc-13')
    parser.add_argument('--assembler', default='aarch64-linux-gnu-as')
    parser.add_argument('--linker', default='aarch64-linux-gnu-ld.bfd')
    parser.add_argument('--gcc-include', type=Path)
    args = parser.parse_args()
    manifest = verify_package()
    if args.verify_package:
        print('PASS: unchanged tested CPU module and all frozen own sources')
        return 0
    require(args.kernel_source is not None and args.kernel_generated is not None and args.output is not None,
            'A rebuild needs --kernel-source, --kernel-generated and --output; use --verify-package otherwise')
    output = args.output.resolve()
    require(not output.is_relative_to(HERE), 'Rebuild output must be outside the immutable kernel package')
    output.mkdir(parents=True, exist_ok=False)
    lock = json.loads((HERE / 'build-inputs.lock.json').read_text(encoding='utf-8'))
    gcc_include = args.gcc_include
    if gcc_include is None:
        gcc_include = Path(subprocess.check_output([args.cc, '-print-file-name=include'], text=True).strip())
    roots = dict(kernel=args.kernel_source.resolve(), generated=args.kernel_generated.resolve(), gcc_builtin=gcc_include.resolve())
    for item in lock['external_headers']:
        require(sha(named(roots[item['root']], item['path'])) == item['sha256'],
                'External ABI header differs: ' + item['root'] + '/' + item['path'])
    # Version text is recorded for visibility. Exact header/assembly/object/final
    # hashes provide the reproducibility checks; a generic Linux4.4 tree is not enough.
    versions = {label: subprocess.check_output([tool, '--version'], text=True).splitlines()[0]
                for label, tool in (('compiler', args.cc), ('assembler', args.assembler), ('linker', args.linker))}
    flags = list(lock['flags']) + ['-isystem', str(roots['gcc_builtin'])]
    for root, path in lock['include_order']: flags += ['-I', str(named(roots[root], path))]
    records, objects = [], []

    def run(argv):
        subprocess.run(argv, check=True)
        records.append(argv)

    for item in lock['compiles']:
        assembly, obj = output / item['assembly'], output / item['object']
        run([args.cc, *flags, str(HERE / item['source']), '-o', str(assembly)])
        require(sha(assembly) == item['expected_assembly_sha256'], 'Assembly differs from tested compiler output: ' + item['source'])
        run([args.assembler, '-o', str(obj), str(assembly)])
        objects.append(obj)
    bridge = output / 'native-cpu-wait-bridge-v15.o'
    run([args.assembler, '-o', str(bridge), str(HERE / lock['bridge'])]); objects.append(bridge)
    for item in lock['objects']:
        require(sha(output / item['name']) == item['sha256'], 'Object differs from tested link input: ' + item['name'])
    candidate = output / 'ub820_cpu_wait_v15.rebuilt.ko'
    run([args.linker, '-r', *['--defsym=' + key + '=' + value for key, value in lock['absolute_link_symbols'].items()],
         '-o', str(candidate), *map(str, objects)])
    reproduced = sha(candidate) == manifest['artifact']['sha256'] and candidate.stat().st_size == manifest['artifact']['bytes']
    (output / 'rebuild-result.json').write_text(json.dumps(dict(versions=versions, commands=records,
        candidate=candidate.name, sha256=sha(candidate), exact_reproduction=reproduced,
        runtime_asset_unchanged=True), indent=2) + '\n', encoding='utf-8')
    require(reproduced, 'Rebuilt candidate differs; the tested runtime asset was not replaced')
    print('PASS: separate candidate matches tested module byte for byte; runtime asset unchanged')
    return 0


if __name__ == '__main__': raise SystemExit(main())
