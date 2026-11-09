#!/usr/bin/env python3
"""Apply the UB820 patches and localoptions.h to a dropbear-2022.83 tree.
Usage: patch_dropbear.py <dropbear_dir>"""
import sys, os
from pathlib import Path

d = sys.argv[1]

LOCALOPTIONS = r'''/* UB820 QoL: on-demand diagnostic SSH. The helper prepares this private
 * RAM environment; no /etc/passwd, system shell, profile or firmware file changes.
 * Authentication still uses the owner's key or password hash. */
#define UB820_AUTHKEYS_PATH "/tmp/.ub820_ssh/authorized_keys"
#define UB820_PWHASH_PATH   "/tmp/.ub820_ssh/pwhash"
#define UB820_HOME_PATH    "/tmp/.ub820_ssh/home"
#define UB820_SHELL_PATH   "/tmp/.ub820_ssh/bin/sh"
#define DEFAULT_ROOT_PATH "/tmp/.ub820_ssh/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/dtvrec/bin"
#define SFTPSERVER_PATH "/tmp/.ub820_ssh/bin/sftp-server"
#define DROPBEAR_PATH_SSH_PROGRAM "/tmp/.ub820_ssh/bin/ssh"

/* Server and an ordinary outbound client; keep authentication stock. */
#define DROPBEAR_SVR_PASSWORD_AUTH 1
#define DROPBEAR_SVR_PUBKEY_AUTH 1
#define DROPBEAR_SVR_MULTIUSER 1
/* No per-connection re-exec: the helper drops the binary in /tmp and the
 * player's quirks make the extra fexecve an avoidable risk. */
#define DROPBEAR_REEXEC 0
#define INETD_MODE 0
#define DO_MOTD 0
#define DROPBEAR_DSS 0
#define DROPBEAR_ECDSA 1
#define DROPBEAR_ED25519 1
#define DROPBEAR_RSA 1
'''

def patch(path, old, new):
    p = os.path.join(d, path)
    s = open(p, encoding="utf-8").read()
    c = s.count(old)
    assert c == 1, "%s: expected 1 occurrence, found %d" % (path, c)
    open(p, "w", encoding="utf-8", newline="").write(s.replace(old, new))
    print("patched", path)

open(os.path.join(d, "localoptions.h"), "w", encoding="utf-8", newline="").write(LOCALOPTIONS)
print("wrote localoptions.h")

# --- session identity: root's HOME and shell live in RAM, not in /etc ---
# Override only the authenticated account's copied passwd fields. UID/GID,
# password, root-login policy, key options and command execution stay stock.
# Do this before shell validation so authentication and session startup agree.
patch("svr-auth.c",
      "\t\tfill_passwd(username);\n",
      "\t\tfill_passwd(username);\n"
      "\t\tif (ses.authstate.pw_name && ses.authstate.pw_uid == 0 &&\n"
      "\t\t\t\tstrcmp(ses.authstate.pw_name, \"root\") == 0) {\n"
      "\t\t\tm_free(ses.authstate.pw_dir);\n"
      "\t\t\tm_free(ses.authstate.pw_shell);\n"
      "\t\t\tses.authstate.pw_dir = m_strdup(UB820_HOME_PATH);\n"
      "\t\t\tses.authstate.pw_shell = m_strdup(UB820_SHELL_PATH);\n"
      "\t\t}\n")
# The private shell cannot appear in the immutable /etc/shells. Recognize only
# this exact managed root shell, and require that the helper prepared it.
patch("svr-auth.c",
      "\tsetusershell();\n\twhile ((listshell = getusershell()) != NULL) {",
      "\tsetusershell();\n"
      "\tif (ses.authstate.pw_uid == 0 &&\n"
      "\t\t\tstrcmp(ses.authstate.pw_name, \"root\") == 0 &&\n"
      "\t\t\tstrcmp(usershell, UB820_SHELL_PATH) == 0 &&\n"
      "\t\t\taccess(usershell, X_OK) == 0) {\n"
      "\t\tgoto goodshell;\n"
      "\t}\n"
      "\twhile ((listshell = getusershell()) != NULL) {")

# An unknown host with no controlling TTY must fail cleanly when stdin closes,
# not spin forever trying to flush a newline that can never arrive. A lone
# newline is itself a rejected answer; do not consume another input line.
patch("cli-kex.c",
      "\t\tresponse = getc(stdin);\n\t\t/* flush stdin buffer */\n"
      "\t\twhile ((getchar()) != '\\n');",
      "\t\tint discard;\n\t\tresponse = getc(stdin);\n"
      "\t\tif (response != EOF && response != '\\n')\n"
      "\t\t\twhile ((discard = getchar()) != '\\n' && discard != EOF) {}")

# --- password: read the helper's hash file instead of /etc/shadow ---
pw = "svr-authpasswd.c"
helper = (
    "\n/* UB820: password hash provided by the helper (see localoptions.h). Returns\n"
    " * the crypt hash string, or NULL when password login is not configured. */\n"
    "static char *ub820_pwhash(void) {\n"
    "\tstatic char buf[256];\n"
    "\tFILE *f = fopen(UB820_PWHASH_PATH, \"r\");\n"
    "\tsize_t n;\n"
    "\tif (!f) return NULL;\n"
    "\tif (!fgets(buf, sizeof buf, f)) { fclose(f); return NULL; }\n"
    "\tfclose(f);\n"
    "\tn = strlen(buf);\n"
    "\twhile (n > 0 && (buf[n-1] == '\\n' || buf[n-1] == '\\r')) buf[--n] = 0;\n"
    "\treturn n ? buf : NULL;\n"
    "}\n")
patch(pw, "void svr_auth_password(int valid_user) {",
      helper + "\nvoid svr_auth_password(int valid_user) {")
patch(pw,
      "\t\t/* the first bytes of passwdcrypt are the salt */\n"
      "\t\tpasswdcrypt = ses.authstate.pw_passwd;\n"
      "\t\ttestcrypt = crypt(password, passwdcrypt);",
      "\t\t/* the first bytes of passwdcrypt are the salt */\n"
      "\t\tpasswdcrypt = ub820_pwhash();\n"
      "\t\tif (passwdcrypt == NULL) passwdcrypt = ses.authstate.pw_passwd;\n"
      "\t\ttestcrypt = crypt(password, passwdcrypt);")

# --- pubkey: fixed private authorization file managed by the helper ---
pk = "svr-authpubkey.c"
old = (
    '\t/* check file permissions, also whether file exists */\n'
    '\tif (checkpubkeyperms() == DROPBEAR_FAILURE) {\n'
    '\t\tTRACE(("bad authorized_keys permissions, or file doesn\'t exist"))\n'
    '\t} else {\n'
    '\t\t/* we don\'t need to check pw and pw_dir for validity, since\n'
    '\t\t * its been done in checkpubkeyperms. */\n'
    '\t\tlen = strlen(ses.authstate.pw_dir);\n'
    '\t\t/* allocate max required pathname storage,\n'
    '\t\t * = path + "/.ssh/authorized_keys" + \'\\0\' = pathlen + 22 */\n'
    '\t\tfilename = m_malloc(len + 22);\n'
    '\t\tsnprintf(filename, len + 22, "%s/.ssh/authorized_keys",\n'
    '\t\t\t\t\tses.authstate.pw_dir);\n'
    '\n'
    '\t\tauthfile = fopen(filename, "r");\n'
    '\t\tif (!authfile) {\n'
    '\t\t\tTRACE(("checkpubkey: failed opening %s: %s", filename, strerror(errno)))\n'
    '\t\t}\n'
    '\t}')
new = (
    '\t/* UB820: the helper owns this private RAM authorization file;\n'
    '\t * a fixed path written by the helper; the home/.ssh checks do not apply. */\n'
    '\t(void)len; (void)checkpubkeyperms;\n'
    '\tfilename = m_strdup(UB820_AUTHKEYS_PATH);\n'
    '\tauthfile = fopen(filename, "r");\n'
    '\tif (!authfile) {\n'
    '\t\tTRACE(("checkpubkey: failed opening %s: %s", filename, strerror(errno)))\n'
    '\t}')
patch(pk, old, new)

# --- old-kernel ABI: one shared compatibility object in every tool ---
# Modern musl normally probes statx/time64 first. The player's kernel logs each
# missing syscall, so use the explicit Linux 4.4 ABI without repeated probes.
compat = Path(__file__).with_name("linux44_compat.c")
Path(d, "ub820_linux44_compat.c").write_bytes(compat.read_bytes())
Path(d, "ub820_ssh_shell_spawn.c").write_bytes(Path(__file__).with_name("ssh_shell_spawn.c").read_bytes())
patch("Makefile.in", "COMMONOBJS=dbutil.o buffer.o dbhelpers.o",
      "COMMONOBJS=ub820_linux44_compat.o ub820_ssh_shell_spawn.o dbutil.o buffer.o dbhelpers.o")
patch("Makefile.in", "SCPOBJS=scp.o progressmeter.o atomicio.o scpmisc.o compat.o",
      "SCPOBJS=ub820_linux44_compat.o ub820_ssh_shell_spawn.o scp.o progressmeter.o atomicio.o scpmisc.o compat.o")
patch("Makefile.in", "LDFLAGS=@LDFLAGS@", "LDFLAGS=@LDFLAGS@ -Wl,--wrap=posix_spawn")
print("wrote shared Linux 4.4 and implicit-shell objects for each SSH tool")
print("OK")
