#!/usr/bin/env python3
"""Apply the SSH session's RAM home to standalone OpenSSH 10.6p2 SFTP.

The firmware passwd database is unchanged. Only the managed root environment
uses its private home for ~ expansion, including modern scp's SFTP extension.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])


def replace(name, before, after):
    path = root / name
    text = path.read_text()
    if after in text:
        return
    if text.count(before) != 1:
        raise SystemExit(f"{name}: source does not match the pinned OpenSSH version")
    path.write_text(text.replace(before, after), newline="\n")


replace("sftp-server-main.c", "#include <stdio.h>\n",
        "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n")
replace("sftp-server-main.c", "\treturn (sftp_server_main(argc, argv, user_pw));",
        '\t/* HOME was set by our authenticated Dropbear session. */\n'
        '\tif (user_pw->pw_uid == 0 && strcmp(user_pw->pw_name, "root") == 0 &&\n'
        '\t    getenv("HOME") != NULL &&\n'
        '\t    strcmp(getenv("HOME"), "/tmp/.ub820_ssh/home") == 0)\n'
        '\t\tuser_pw->pw_dir = getenv("HOME");\n'
        '\treturn (sftp_server_main(argc, argv, user_pw));')
replace("misc.c", "\t/* Make sure directory has a trailing '/' */\n"
        "\tslash = (len = strlen(pw->pw_dir)) == 0 || pw->pw_dir[len - 1] != '/';\n\n"
        '\tif ((r = xasprintf(&s, "%s%s%s", pw->pw_dir,',
        '\t/* Match the root HOME supplied by the managed SSH session, while\n'
        '\t * leaving the firmware password database and other users alone. */\n'
        '\tconst char *home = pw->pw_dir;\n'
        '\tif (pw->pw_uid == 0 && strcmp(pw->pw_name, "root") == 0 &&\n'
        '\t    getenv("HOME") != NULL &&\n'
        '\t    strcmp(getenv("HOME"), "/tmp/.ub820_ssh/home") == 0)\n'
        '\t\thome = getenv("HOME");\n'
        "\t/* Make sure directory has a trailing '/' */\n"
        "\tslash = (len = strlen(home)) == 0 || home[len - 1] != '/';\n\n"
        '\tif ((r = xasprintf(&s, "%s%s%s", home,')
