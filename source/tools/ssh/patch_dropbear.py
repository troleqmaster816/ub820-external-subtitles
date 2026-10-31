#!/usr/bin/env python3
"""Apply the UB820 patches and localoptions.h to a dropbear-2022.83 tree.
Usage: patch_dropbear.py <dropbear_dir>"""
import sys, os

d = sys.argv[1]

LOCALOPTIONS = r'''/* UB820 QoL: on-demand diagnostic SSH. The user home (/root) is read-only and
 * there is no /etc/shadow, so the helper writes these two files and the auth
 * code below reads them instead of ~/.ssh and the shadow password. */
#define UB820_AUTHKEYS_PATH "/tmp/.ub820_ssh/authorized_keys"
#define UB820_PWHASH_PATH   "/tmp/.ub820_ssh/pwhash"

/* Server only; keep it small but compatible with current clients. */
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

# --- pubkey: fixed authorized_keys path (home dir is read-only) ---
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
    '\t/* UB820: /root is read-only on this device, so authorized_keys is kept at\n'
    '\t * a fixed path written by the helper; the home/.ssh checks do not apply. */\n'
    '\t(void)len; (void)checkpubkeyperms;\n'
    '\tfilename = m_strdup(UB820_AUTHKEYS_PATH);\n'
    '\tauthfile = fopen(filename, "r");\n'
    '\tif (!authfile) {\n'
    '\t\tTRACE(("checkpubkey: failed opening %s: %s", filename, strerror(errno)))\n'
    '\t}')
patch(pk, old, new)

# --- time: the player's Linux 4.4 has no clock_gettime64 ---
# musl 1.2 tries syscall 403 (clock_gettime64) first on every call and falls
# back on ENOSYS; the 4.4 kernel logs each failure with a register dump.
# These take the place of musl's own __clock_gettime (used inside musl:
# time(), timed waits) and __clock_gettime64 (what clock_gettime and
# gettimeofday resolve to), so musl's clock_gettime.o is not linked at all.
TIME32 = r'''/* UB820: clock_gettime through the 32-bit syscall that Linux 4.4 has
 * (included at the end of dbutil.c, linked into dropbear and dropbearkey).
 * time_t stays 64-bit in musl; the kernel value is 32-bit until 2038. */
#include <time.h>
#include <unistd.h>
#define UB820_NR_CLOCK_GETTIME32 263
int __clock_gettime(clockid_t clk, struct timespec *ts) {
	long t[2];
	if (syscall(UB820_NR_CLOCK_GETTIME32, clk, t)) return -1;
	ts->tv_sec = t[0];
	ts->tv_nsec = t[1];
	return 0;
}
int __clock_gettime64(clockid_t clk, struct timespec *ts) __attribute__((alias("__clock_gettime")));
'''
open(os.path.join(d, "ub820_time32.c"), "w", encoding="utf-8", newline="").write(TIME32)
print("wrote ub820_time32.c")
with open(os.path.join(d, "dbutil.c"), "a", encoding="utf-8", newline="") as f:
    f.write('\n#include "ub820_time32.c"\n')
print("patched dbutil.c")
print("OK")
