#!/usr/bin/env python3
"""Apply the UB820 RAM-only shell integration to pristine BusyBox 1.37.0."""
from pathlib import Path
import sys,shutil
root=Path(sys.argv[1])
def patch(name,old,new):
 p=root/name;s=p.read_text()
 if s.count(new)==1:return
 if s.count(old)!=1:raise SystemExit(f'{name}: expected exactly one old/new patch anchor')
 p.write_text(s.replace(old,new),newline='\n')
patch('shell/ash.c','read_profile("/etc/profile");','read_profile("/tmp/.ub820_ssh/profile");')
patch('include/libbb.h','#define LIBBB_DEFAULT_LOGIN_SHELL  "-/bin/sh"','#define LIBBB_DEFAULT_LOGIN_SHELL  "-/tmp/.ub820_ssh/bin/sh"')
patch('include/libbb.h','#define DEFAULT_SHELL_SHORT_NAME   (bb_default_login_shell+6)','#define DEFAULT_SHELL_SHORT_NAME   "sh"')
patch('include/libbb.h','#define BB_PATH_ROOT_PATH "PATH=/sbin:/usr/sbin:/bin:/usr/bin" BB_ADDITIONAL_PATH','#define BB_PATH_ROOT_PATH "PATH=/tmp/.ub820_ssh/bin:/sbin:/usr/sbin:/bin:/usr/bin" BB_ADDITIONAL_PATH')

patch('libbb/Kbuild.src','lib-y += appletlib.o','lib-y += appletlib.o\nlib-y += ub820_linux44.o')
shutil.copyfile(Path(__file__).with_name('linux44_compat.c'),root/'libbb/ub820_linux44.c')

patch('libbb/Kbuild.src','lib-y += ub820_linux44.o','lib-y += ub820_linux44.o\nobj-y += ub820_shell_spawn.o')
shutil.copyfile(Path(__file__).with_name('ssh_shell_spawn.c'),root/'libbb/ub820_shell_spawn.c')

patch('util-linux/setarch.c','(--argv)[0] = (char*)"/bin/sh";','(--argv)[0] = (char*)DEFAULT_SHELL;')

patch('shell/hush.c','hfopen("/etc/profile");','hfopen("/tmp/.ub820_ssh/profile");')
