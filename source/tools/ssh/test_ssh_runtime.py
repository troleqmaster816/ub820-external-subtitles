#!/usr/bin/env python3
"""Exercise the ARM SSH runtime locally, never on a player.

Linux requires root, unshare, mount, qemu-arm/binfmt, OpenSSH, openssl and
Paramiko. Root is used only to create a disposable mount/PID namespace and
chroot with its own /tmp, /proc and devpts. Windows dispatches through WSL
Ubuntu as root, using the default WSL user's existing Python packages.
All SSH connections go to loopback on an ephemeral, unprivileged port.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time


REPO = Path(__file__).resolve().parents[3]
RUNTIME = Path("/tmp/.ub820_ssh")


def run(args, **kwargs):
    return subprocess.run(args, check=True, capture_output=True, **kwargs)


def wsl_path(path):
    value = str(Path(path).resolve()).replace("\\", "/")
    return "/mnt/" + value[0].lower() + value[2:]


def windows_dispatch(args):
    site = run(["wsl.exe", "-d", "Ubuntu", "--", "python3", "-c",
                "import site; print(site.getusersitepackages())"], text=True).stdout.strip()
    command = ["wsl.exe", "-d", "Ubuntu", "-u", "root", "--", "env",
               "PYTHONPATH=" + site, "python3", wsl_path(__file__),
               "--bin-dir", wsl_path(args.bin_dir), "--report-dir", wsl_path(args.report_dir)]
    return subprocess.call(command)


def copy_file(root, source, destination=None):
    source = Path(source)
    target = root / str(destination or source).lstrip("/")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source.resolve(), target)


def prepare_fixture(root, binaries, key, password):
    for directory in ("dev/pts", "proc", "etc", "bin", "tmp/.ub820_ssh/home",
                      "tmp/.ub820_ssh/bin", "usr/libexec/qemu-binfmt"):
        (root / directory).mkdir(parents=True, exist_ok=True)
    (root / "tmp").chmod(0o1777)
    (root / "tmp/.ub820_ssh").chmod(0o700)
    (root / "tmp/.ub820_ssh/home").chmod(0o700)
    # Deliberately retain the firmware's missing home and stock shell entry.
    # The Dropbear patch must select the private runtime without rewriting it.
    (root / "etc/passwd").write_text("root:x:0:0:root:/root:/bin/sh\n")
    (root / "etc/group").write_text("root:x:0:\n")
    (root / "etc/shells").write_text("/bin/sh\n")
    stock = root / "bin/sh"
    stock.write_text("#!/tmp/.ub820_ssh/bin/sh\nprintf 'WRONG_STOCK_SHELL\\n' >&2\nexit 99\n")
    stock.chmod(0o755)

    qemu = Path(shutil.which("qemu-arm") or "/usr/bin/qemu-arm")
    copy_file(root, qemu, "/usr/bin/qemu-arm")
    # binfmt has no F flag on WSL, so the interpreter and its native ELF
    # dependencies must remain available after chroot and nested execs.
    registration = Path("/proc/sys/fs/binfmt_misc/qemu-arm").read_text()
    interpreter = re.search(r"^interpreter (.+)$", registration, re.M)
    if not interpreter:
        raise RuntimeError("qemu-arm binfmt registration is missing")
    copy_file(root, qemu, interpreter[1])
    for line in run(["ldd", str(qemu)], text=True).stdout.splitlines():
        match = re.search(r"(?:=>\s*)?(/\S+)", line)
        if match:
            copy_file(root, match[1])

    for name in ("dropbear", "dropbearkey", "busybox", "sftp-server", "scp", "dbclient"):
        source = binaries / name
        if not source.is_file():
            raise RuntimeError("Missing runtime component: " + str(source))
        target_name = "ssh" if name == "dbclient" else name
        copy_file(root, source, RUNTIME / "bin" / target_name)
        (root / str(RUNTIME / "bin" / target_name).lstrip("/")).chmod(0o700)
    listing = run([str(qemu), str(binaries / "busybox"), "--list"], text=True).stdout.splitlines()
    for name in listing:
        if "/" in name or not name or name in ("busybox", "scp", "sftp-server"):
            continue
        target = root / "tmp/.ub820_ssh/bin" / name
        if not target.exists():
            target.symlink_to("busybox")
    shell = root / "tmp/.ub820_ssh/bin/sh"
    if not shell.is_symlink():
        raise RuntimeError("BusyBox did not provide sh")
    public = key.with_suffix(".pub").read_bytes()
    client_home = root / "tmp/.ub820_ssh/home/.ssh"
    client_home.mkdir(mode=0o700)
    client_identity = client_home / "id_dropbear"
    run([str(qemu), str(binaries / "dropbearkey"), "-t", "ed25519", "-f", str(client_identity)])
    client_identity.chmod(0o600)
    generated = run([str(qemu), str(binaries / "dropbearkey"), "-y", "-f", str(client_identity)]).stdout
    client_public = next(line for line in generated.splitlines() if line.startswith(b"ssh-ed25519 "))
    (root / "tmp/.ub820_ssh/authorized_keys").write_bytes(public + client_public + b"\n")
    (root / "tmp/.ub820_ssh/authorized_keys").chmod(0o600)
    encrypted = run(["openssl", "passwd", "-6", "-salt", "ub820test", password]).stdout
    (root / "tmp/.ub820_ssh/pwhash").write_bytes(encrypted.strip())
    (root / "tmp/.ub820_ssh/pwhash").chmod(0o600)
    run([str(qemu), str(binaries / "dropbearkey"), "-t", "ed25519", "-f",
         str(root / "tmp/.ub820_ssh/hostkey")])
    return sorted(listing)


def namespace_child(root, port):
    root = root.resolve()
    if not str(root).startswith("/tmp/ub820-ssh-runtime-"):
        raise RuntimeError("Unexpected fixture root")
    run(["mount", "--make-rprivate", "/"])
    run(["mount", "-t", "proc", "proc", str(root / "proc")])
    for name in ("null", "zero", "random", "urandom", "tty"):
        target = root / "dev" / name
        target.touch()
        run(["mount", "--bind", "/dev/" + name, str(target)])
    run(["mount", "-t", "devpts", "devpts", str(root / "dev/pts"),
         "-o", "newinstance,ptmxmode=0666,mode=0620"])
    (root / "dev/ptmx").symlink_to("pts/ptmx")
    (root / "dev/fd").symlink_to("/proc/self/fd")
    os.chroot(root)
    os.chdir("/")
    os.environ.update(HOME="/root", SHELL="/bin/sh", PATH="/bin:/usr/bin")
    os.umask(0o022)
    for number in signal.valid_signals():
        if number not in (signal.SIGKILL, signal.SIGSTOP):
            try:
                signal.signal(number, signal.SIG_DFL)
            except (OSError, ValueError):
                pass
    signal.pthread_sigmask(signal.SIG_SETMASK, [])
    executable = str(RUNTIME / "bin/dropbear")
    os.execv(executable, [executable, "-F", "-r", str(RUNTIME / "hostkey"),
                         "-p", "127.0.0.1:" + str(port), "-P", str(RUNTIME / "pid")])


def main(args):
    if os.geteuid() != 0:
        raise RuntimeError("Run in WSL as root; the test server is confined to a disposable namespace/chroot")
    import paramiko

    binaries = args.bin_dir.resolve()
    report_dir = args.report_dir.resolve()
    report_dir.mkdir(parents=True, exist_ok=True)
    (report_dir / "results.json").unlink(missing_ok=True)
    (report_dir / "server.log").write_bytes(b"")
    results = []
    password = "ub820-fixture-password"
    with tempfile.TemporaryDirectory(prefix="ub820-ssh-runtime-", dir="/tmp") as temporary:
        directory = Path(temporary)
        root = directory / "root"
        root.mkdir()
        key = directory / "client_ed25519"
        run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C",
             "runtime-fixture-only", "-f", str(key)])
        applets = prepare_fixture(root, binaries, key, password)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        log_path = report_dir / "server.log"
        server_log = log_path.open("wb")
        server = subprocess.Popen(["unshare", "--mount", "--pid", "--fork", "--mount-proc",
                                   "--kill-child=SIGKILL", sys.executable, str(Path(__file__).resolve()),
                                   "--namespace-child", str(root), "--port", str(port)],
                                  stdin=subprocess.DEVNULL, stdout=server_log, stderr=server_log)

        def connect(auth="key"):
            client = paramiko.SSHClient()
            client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
            kwargs = dict(hostname="127.0.0.1", port=port, username="root", timeout=8,
                          banner_timeout=8, auth_timeout=8, allow_agent=False, look_for_keys=False)
            if auth == "key":
                kwargs["key_filename"] = str(key)
            else:
                kwargs["password"] = password
            client.connect(**kwargs)
            return client

        def execute(client, command, data=None, timeout=10):
            channel = client.get_transport().open_session(timeout=timeout)
            channel.settimeout(timeout)
            channel.exec_command(command)
            if data:
                channel.sendall(data)
            channel.shutdown_write()
            stdout, stderr = bytearray(), bytearray()
            deadline = time.monotonic() + timeout
            while True:
                while channel.recv_ready():
                    stdout.extend(channel.recv(65536))
                while channel.recv_stderr_ready():
                    stderr.extend(channel.recv_stderr(65536))
                if channel.exit_status_ready() and not channel.recv_ready() and not channel.recv_stderr_ready():
                    status = channel.recv_exit_status()
                    break
                if time.monotonic() >= deadline:
                    channel.close()
                    raise TimeoutError((command, bytes(stdout), bytes(stderr)))
                time.sleep(0.01)
            channel.close()
            if b"Failed chdir" in stderr or b"WRONG_STOCK_SHELL" in stderr:
                raise AssertionError((command, bytes(stderr)))
            return status, bytes(stdout), bytes(stderr)

        def check(name, callback):
            callback()
            results.append({"name": name, "passed": True})
            print(name + " PASS", flush=True)

        try:
            deadline = time.monotonic() + 15
            while True:
                if server.poll() is not None:
                    raise RuntimeError("SSH server exited: " + log_path.read_text(errors="replace"))
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.3):
                        break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.05)
            client = connect()
            check("public_key_auth", lambda: None)
            host_key = client.get_transport().get_remote_server_key()
            known_hosts = root / "tmp/.ub820_ssh/home/.ssh/known_hosts"
            # Dropbear 2022.83's native known_hosts format uses the literal
            # remote hostname, independent of the separately selected port.
            known_hosts.write_text(f"127.0.0.1 {host_key.get_name()} {host_key.get_base64()}\n")
            known_hosts.chmod(0o600)

            def environment():
                status, out, err = execute(client, 'printf "%s\\n" "$HOME" "$SHELL" "$PWD" "$PATH"')
                lines = out.decode().splitlines()
                assert status == 0 and not err, (status, out, err)
                assert lines[:3] == [str(RUNTIME / "home"), str(RUNTIME / "bin/sh"), str(RUNTIME / "home")], lines
                assert lines[3].split(":")[0] == str(RUNTIME / "bin"), lines
                assert not (root / "root").exists()
                assert (root / "etc/passwd").read_text() == "root:x:0:0:root:/root:/bin/sh\n"
            check("home_cwd_shell_path", environment)

            def normal_commands():
                status, out, err = execute(client,
                    "id -u; uname -s; printf 'one\\ntwo\\n' | grep two; "
                    "for i in 1 2 3; do sh -c 'printf nested'; done; printf '\\n'; "
                    "cat /proc/version | head -n 1; command -v sh; command -v cat")
                assert status == 0 and not err, (status, out, err)
                assert b"0\nLinux\ntwo\nnestednestednested\nLinux version " in out, out
                assert out.splitlines()[-2] in (b"sh", str(RUNTIME / "bin/sh").encode()), out
                assert out.splitlines()[-1] in (b"cat", str(RUNTIME / "bin/cat").encode()), out
            check("commands_pipes_loops_nested_sh", normal_commands)

            def implicit_tool_shell():
                # These paths exercise libc system() and popen() from inside
                # a bundled applet. They must not silently select factory sash
                # merely because the host libc defaults to /bin/sh.
                status, out, err = execute(client,
                    "awk 'BEGIN { "
                    "if (system(\"printf awk-system; echo; printf awk-pipeline | cat; echo\") != 0) exit 11; "
                    "command=\"printf awk-read | cat\"; "
                    "if ((command | getline line) != 1 || line != \"awk-read\") exit 12; "
                    "close(command); "
                    "print \"awk-write\" | \"cat > awk-pipe.txt\"; "
                    "if (close(\"cat > awk-pipe.txt\") != 0) exit 13; "
                    "}' && cat awk-pipe.txt && rm awk-pipe.txt")
                assert (status, out, err) == (0, b"awk-system\nawk-pipeline\nawk-write\n", b""), (status, out, err)
            check("bundled_awk_system_readpipe_writepipe_use_managed_shell", implicit_tool_shell)

            def outgoing_ssh():
                status, out, err = execute(client,
                    f"ssh -p {port} root@127.0.0.1 'printf outgoing-ok'; "
                    "result=$?; test -s \"$HOME/.ssh/known_hosts\" || exit 91; exit \"$result\"", timeout=15)
                assert status == 0 and out == b"outgoing-ok" and b"/root" not in err, (status, out, err)
            check("bundled_ssh_client_home_keys_known_hosts", outgoing_ssh)

            def quoting():
                status, out, err = execute(client, "printf '%s\\n' 'space ; $HOME *' \"quote'\\\"\"; printf error >&2; exit 37")
                assert (status, out, err) == (37, b'space ; $HOME *\nquote\'"\n', b"error"), (status, out, err)
            check("quoting_stderr_exit_status", quoting)

            def stdin_eof():
                body = b"stdin\n\x00binary\xff\n"
                status, out, err = execute(client, "cat", body)
                assert (status, out, err) == (0, body, b""), (status, out, err)
            check("binary_stdin_and_eof", stdin_eof)

            def writable_home_script():
                status, out, err = execute(client,
                    "cat > probe.sh <<'UB820_SCRIPT'\n#!/tmp/.ub820_ssh/bin/sh\nprintf '%s\\n' script-ok\nUB820_SCRIPT\n"
                    "sh probe.sh; chmod 700 probe.sh; ./probe.sh; "
                    "mkdir -p example/deep; printf persisted > example/deep/file; "
                    "cat example/deep/file; rm -r example probe.sh")
                assert (status, out, err) == (0, b"script-ok\nscript-ok\npersisted", b""), (status, out, err)
            check("writable_home_managed_shebang_and_sh_file", writable_home_script)

            def pty_login():
                shell = client.invoke_shell(term="xterm", width=100, height=30)
                shell.settimeout(10)
                shell.send("printf '__PTY__:%s:%s\\n' \"$HOME\" \"$SHELL\"\nexit\n")
                output = bytearray()
                until = time.monotonic() + 12
                while time.monotonic() < until:
                    if shell.recv_ready():
                        data = shell.recv(65536)
                        if not data:
                            break
                        output.extend(data)
                    elif shell.exit_status_ready():
                        break
                    else:
                        time.sleep(0.02)
                ended = shell.exit_status_ready()
                status = shell.recv_exit_status() if ended else None
                shell.close()
                marker = f"__PTY__:{RUNTIME / 'home'}:{RUNTIME / 'bin/sh'}".encode()
                assert ended and status == 0 and marker in output and b"Failed chdir" not in output and b"WRONG_STOCK_SHELL" not in output, (status, bytes(output))
                (report_dir / "pty.txt").write_bytes(output)
            check("interactive_pty_login", pty_login)

            def password_login():
                other = connect("password")
                try:
                    assert execute(other, "printf password-ok") == (0, b"password-ok", b"")
                finally:
                    other.close()
            check("password_auth", password_login)

            def tcp_forwarding():
                errors = []
                body = b"forwarded\x00ssh\xff"
                with socket.socket() as listener:
                    listener.bind(("127.0.0.1", 0))
                    listener.listen(1)
                    listener.settimeout(8)
                    def echo():
                        try:
                            stream, _ = listener.accept()
                            with stream:
                                stream.settimeout(8)
                                data = bytearray()
                                while len(data) < len(body):
                                    part = stream.recv(1024)
                                    if not part:
                                        break
                                    data.extend(part)
                                stream.sendall(data)
                        except Exception as error:
                            errors.append(str(error))
                    worker = threading.Thread(target=echo, daemon=True)
                    worker.start()
                    channel = client.get_transport().open_channel("direct-tcpip", listener.getsockname(),
                                                                  ("127.0.0.1", 0), timeout=8)
                    channel.settimeout(8)
                    channel.sendall(body)
                    reply = bytearray()
                    while len(reply) < len(body):
                        part = channel.recv(1024)
                        if not part:
                            break
                        reply.extend(part)
                    channel.close()
                    worker.join(8)
                    assert not worker.is_alive() and not errors and bytes(reply) == body, (reply, errors)
            check("ordinary_tcp_forwarding", tcp_forwarding)

            def rejected_password():
                wrong = paramiko.SSHClient()
                wrong.set_missing_host_key_policy(paramiko.AutoAddPolicy())
                try:
                    try:
                        wrong.connect("127.0.0.1", port, "root", password="wrong-fixture-password",
                                      allow_agent=False, look_for_keys=False, timeout=8, auth_timeout=8)
                    except paramiko.AuthenticationException:
                        return
                    raise AssertionError("Wrong password accepted")
                finally:
                    wrong.close()
            check("wrong_password_rejected", rejected_password)

            def sftp_roundtrip():
                ftp = client.open_sftp()
                try:
                    assert ftp.normalize(".") == str(RUNTIME / "home")
                    destination = str(RUNTIME / "home/sftp file.bin")
                    content = bytes(range(256)) * 17
                    with ftp.open(destination, "wb") as remote:
                        remote.write(content)
                    ftp.chmod(destination, 0o640)
                    assert ftp.stat(destination).st_mode & 0o777 == 0o640
                    renamed = destination + ".renamed"
                    ftp.rename(destination, renamed)
                    with ftp.open(renamed, "rb") as remote:
                        assert remote.read() == content
                    ftp.remove(renamed)
                finally:
                    ftp.close()
            check("sftp_binary_roundtrip_permissions_rename", sftp_roundtrip)

            def scp_roundtrip(legacy):
                original = directory / ("legacy.bin" if legacy else "modern.bin")
                original.write_bytes(bytes(range(256)) * 33)
                # Modern SCP uses SFTP expand-path@openssh.com for '~'; legacy
                # SCP lets the login shell expand it. Both must select RAM HOME.
                destination = "~/" + original.name
                command = ["scp", "-F", "/dev/null", "-P", str(port), "-i", str(key), "-o", "BatchMode=yes",
                           "-o", "IdentitiesOnly=yes", "-o", "StrictHostKeyChecking=accept-new",
                           "-o", "UserKnownHostsFile=" + str(directory / "known_hosts")]
                if legacy:
                    command.append("-O")
                run(command + [str(original), "root@127.0.0.1:" + destination], timeout=20)
                assert execute(client, 'test -f "$HOME/' + original.name + '"')[0] == 0
                downloaded = directory / (original.name + ".downloaded")
                run(command + ["root@127.0.0.1:" + destination, str(downloaded)], timeout=20)
                assert original.read_bytes() == downloaded.read_bytes()
                assert execute(client, 'rm "$HOME/' + original.name + '"')[0] == 0
            check("openssh_scp_default_sftp_roundtrip", lambda: scp_roundtrip(False))
            check("openssh_scp_legacy_roundtrip", lambda: scp_roundtrip(True))

            def repeated_exec():
                for i in range(12):
                    assert execute(client, "sh -c 'printf stable'") == (0, b"stable", b"")
                status, out, err = execute(client, "ps")
                assert status == 0 and not err and out, (status, out, err)
                (report_dir / "processes.txt").write_bytes(out)
            check("repeated_exec_without_shell_chain", repeated_exec)
            def path_override():
                status, out, err = execute(client,
                    "mkdir custom-bin; cat > custom-bin/uname <<'UB820_OVERRIDE'\n"
                    "#!/tmp/.ub820_ssh/bin/sh\nprintf '%s\\n' user-path-wins\nUB820_OVERRIDE\n"
                    "chmod 700 custom-bin/uname; PATH=\"$HOME/custom-bin:$PATH\" uname; rm -r custom-bin")
                assert (status, out, err) == (0, b"user-path-wins\n", b""), (status, out, err)
            check("user_path_can_override_bundled_applets", path_override)
            def unknown_host_eof():
                known_hosts.unlink()
                for input_data in (None, b"\n"):
                    status, out, err = execute(client, f"ssh -p {port} root@127.0.0.1 'printf must-not-execute'",
                                               data=input_data, timeout=5)
                    assert status != 0 and not out and b"not in the trusted hosts file" in err, (status, out, err)
            check("ssh_unknown_host_stdin_eof_fails_without_hang", unknown_host_eof)
            client.close()
        finally:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=5)
            server_log.close()
            report = {
                "scope": "loopback-only local ARM binaries in disposable mount/PID namespace/chroot",
                "player_accessed": False,
                "passed": len(results) == 18,
                "results": results,
                "applets": applets,
                "binaries": {name: hashlib.sha256((binaries / name).read_bytes()).hexdigest()
                             for name in ("dropbear", "dropbearkey", "busybox", "sftp-server", "scp", "dbclient")},
            }
            (report_dir / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print("ALL SSH RUNTIME TESTS PASSED", flush=True)
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, default=Path(__file__).resolve().parent / "bin")
    parser.add_argument("--report-dir", type=Path, default=REPO / "analysis/ssh-runtime/e2e")
    parser.add_argument("--namespace-child", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--port", type=int, help=argparse.SUPPRESS)
    arguments = parser.parse_args()
    if os.name == "nt":
        raise SystemExit(windows_dispatch(arguments))
    if arguments.namespace_child:
        namespace_child(arguments.namespace_child, arguments.port)
    else:
        raise SystemExit(main(arguments))
