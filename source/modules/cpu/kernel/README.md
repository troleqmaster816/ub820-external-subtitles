This directory contains the byte-pinned UB820 CPU-wait kernel component used by
the optional CPU module. Loading it is inert; optimization starts only after an
explicit `ON` command. The helper's CPU module and optimization default to off.

The shipped `ub820_cpu_wait_v15.ko` is **101336 bytes**, SHA256
`13d23505ec9585aa4eacfdfcd592f4708814a4e897c2aa01e7bcad320a36be70`.
Normal helper builds embed this file without rebuilding a kernel component.

The component uses the stock FPD and antenna event queues and wake producers.
It checks exact supported kernel builds, immutable code, ABI offsets and worker
identity before changing two RAM instructions. It does not install a periodic
timer or worker. `OFF` sets bypass, wakes both queues and restores the original
instructions. After activation, code, contexts and task references remain pinned
until a real kernel reset; never force unload or alter reference counts.

The runtime interface is `/dev/ub820_cpu_wait`, mode `0600`, accepting `ON` and
`OFF`. Status distinguishes inert phase 0, active phase 1, and restored resident
phase 3. A disabled flag alone is not a complete restoration check. The helper
also validates expected code, build and worker state and does not blindly retry
an unknown operation.

The binary has been exercised on firmware 1.82. Exact-build guards for 1.85 are
included, but that firmware has not been verified on a player. POWER/standby,
CEC and unrestricted long-term operation are not covered by the completed
functional checks.

`manifest.json` records the artifact, ABI, interface and exact own-source pins.
Historical file names, `OFFLINE` comments and embedded version
`0.0-offline-v15` are retained to preserve the reviewed binary/source identity.
They describe the original development snapshot; they do not cause a different
runtime mode. Headers named `fixture` and `observer` contain required immutable
compatibility guards, not private reports or programs to run.

Verification needs Python 3.9 or later:

```sh
python3 rebuild.py --verify-package
```

An optional expert rebuild requires the matching Panasonic Linux 4.4.8 source
headers, the prepared ARM64 generated ABI headers, and an AArch64 toolchain.
The tested compiler was GCC 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1), with GNU
binutils 2.42. A generic Linux 4.4 tree or the vendor compiler alone does not
establish a matching ABI. The upstream source location is Panasonic's
[AWBPP18A source distribution](https://ospo.panasonic.com/oss/bdplayer/AWBPP18A.html).

`build-inputs.lock.json` lists only the exact external headers actually consumed,
their hashes, include order, flags, object hashes and four native continuation
symbols. These external headers, toolchain packages, firmware and kernel images
are not bundled here. Provide a prepared source/generated tree matching that
lock; the script rejects mismatches before compilation. Generated headers include
the target configuration, bounds, time constants and release string. Changing
their configuration can change structure layouts even with the same release.

```sh
python3 rebuild.py --kernel-source /path/to/linux4 \
  --kernel-generated /path/to/prepared-arm64-headers \
  --output /path/to/new-cpu-build-directory
```

The output directory must be new and outside this immutable package. The recipe
checks assembly, object and final hashes and creates a separate candidate; it
never replaces the shipped module, loads a module or contacts a player. Exact
reproduction with different external tools or headers is not claimed. The recipe
was transcribed from the tested build inputs; package verification is separate
from rebuilding and running a new candidate.

The own kernel code is offered under `MIT OR GPL-2.0-only`; the binary declares
`Dual MIT/GPL`. License texts are in `licenses/`. External Linux headers and
toolchain inputs retain their own licenses.
