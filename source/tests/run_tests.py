"""Host tests (x86, in WSL Ubuntu): python tests/run_tests.py

Optional: UB820_FIRMWARE_DIR=<v182>/dtvrec/bin also runs the English-text page
test against the real apl_lang.dat (<v182>/dtvrec/data/lang/apl_lang.dat).
"""
from pathlib import Path
import os,posixpath,re,struct,subprocess,sys,shlex
ROOT=Path(__file__).resolve().parent.parent
# UB820_TEST_TMP selects a parent in WSL and keeps that run's output.
PARENT=os.environ.get('UB820_TEST_TMP')
TMP=None
# Some application fixtures exercise the player's fixed paths in /tmp. Keep
# execution exclusive across runners, while their builds can run concurrently.
FIXTURE_LOCK='flock --exclusive --close /tmp/ub820-fixtures.lock'

def linux(p):
 s=str(Path(p).resolve()).replace('\\','/');return '/mnt/'+s[0].lower()+s[2:]
def sh(cmd):
 r=subprocess.run(['wsl.exe','-d','Ubuntu','--','bash','-lc',cmd],capture_output=True,text=True,encoding='utf-8',errors='replace')
 return r.returncode,r.stdout+r.stderr
def build(name,sources,defines):
 flags='-std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -DUB_HOST_TEST -pthread -fsanitize=alignment -fno-sanitize-recover=alignment'
 if name in ('subtitle_startup_test','ssh_runtime_test','settings_tile_test','cpu_settings_test','cpu_backend_test','hook_transfer_test','elf_alignment_test'):flags+=' -ffunction-sections -fdata-sections -Wl,--gc-sections'
 if name in ('settings_tile_test','power_settings_test'):flags+=' -no-pie'
 if name=='cpu_settings_test':flags+=' -fno-pie -no-pie'
 # stock ESL_Open reads 0x400 bytes of every path the controller hands it
 if name=='subtitle_engine_lifecycle_test':flags+=' -fsanitize=address -fno-omit-frame-pointer'
 inc=f'-I{shlex.quote(linux(ROOT/"core"))} -I{shlex.quote(linux(ROOT/"generated"))}'
 defs=' '.join(f"'-D{k}=\"{v}\"'" for k,v in defines.items())
 srcs=' '.join(shlex.quote(linux(ROOT/s)) for s in sources)
 return sh(f'mkdir -p {TMP} && gcc {flags} {inc} {defs} {srcs} -o {TMP}/{name}')

def decode_lang(dat,pages):
 D=open(dat,'rb').read();n=struct.unpack_from('<I',D,0)[0]//4;offs=struct.unpack_from('<%dI'%n,D,0)
 b=open(pages,'rb').read();old,new=b[:0x2000],b[0x2000:]
 assert old==D[0x408000:0x40a000]
 P=bytearray(D);P[0x408000:0x40a000]=new;P=bytes(P)
 def ent(buf,i):
  p=offs[i]+4*i;h=struct.unpack_from('<24H',buf,p)
  return [buf[p+2*k+x:buf.index(b'\0',p+2*k+x)] for k,x in enumerate(h)]
 changed=sum(1 for i in range(0x2000) if old[i]!=new[i])
 others=sum(1 for i in range(n) if not 0x12b5<=i<=0x12c0 and ent(P,i)!=ent(D,i))
 english=all(ent(P,i)[k]==ent(D,i)[16] for i in range(0x12b5,0x12c1) for k in range(16))
 return changed==192 and others==0 and english,f'{changed} bytes changed, {others} other entries changed, English in columns 0-15: {english}'

# Syscalls glibc 2.39 tries first on 32-bit ARM that the player's Linux 4.4
# lacks (core/sys.h). 398 (rseq) is made once by glibc at start-up.
FORBIDDEN=re.compile(r'\b(statx|clock_gettime64|clock_nanosleep\w*|clock_settime64|\w+_time64)\(|Unknown syscall (39[79]|4[0-4]\d)')
def sys_check():
 """The ARM helper code under qemu-arm -strace: no syscall Linux 4.4 lacks."""
 srcs=[ROOT/'tests'/'sys_check.c',*sorted((ROOT/'core').glob('*.c')),*sorted((ROOT/'modules').glob('*/module.c')),*sorted((ROOT/'generated').glob('*.c'))]
 flags='-std=gnu11 -static -mthumb -march=armv7-a -mfpu=neon -mfloat-abi=hard -Os -Wall -Wextra -Werror -Wno-unused-function -fno-strict-aliasing -DUB_HOST_TEST'
 inc=f'-I{shlex.quote(linux(ROOT/"core"))} -I{shlex.quote(linux(ROOT/"generated"))}'
 defs=f"'-DUB_CONFIG_DIR=\"{TMP}/syscfg\"' '-DUB_LOG_FILE=\"{TMP}/sys.log\"'"
 rc,out=sh(f'mkdir -p {TMP}/syscfg && arm-linux-gnueabihf-gcc {flags} {inc} {defs} {" ".join(shlex.quote(linux(s)) for s in srcs)} -o {TMP}/sys_check')
 if rc:print('sys_check BUILD FAILED'+chr(10)+out);return False
 # the helper sends stderr, and with it the trace, to its log
 rc,out=sh(f'cd {TMP} && rm -f sys.log && {FIXTURE_LOCK} qemu-arm -strace ./sys_check 2>&1; s=$?; cat sys.log; exit $s')
 bad=sorted({m.group(0) for m in FORBIDDEN.finditer(out)})
 result=[l for l in out.splitlines() if 'sys_check' in l and ('passed' in l or 'FAILED' in l)]
 good=not rc and not bad and bool(result) and 'passed' in result[-1]
 print('sys_check',('passed: ' if good else 'FAILED: ')+(result[-1] if result else 'no result')+(f'; forbidden syscalls: {bad}' if bad else '; no syscall missing from Linux 4.4'))
 if not good:print(chr(10).join(l for l in out.splitlines() if 'FAIL' in l or FORBIDDEN.search(l))[:3000])
 return good

def elf_arm_check():
 """Run the generated-image parser with the helper's ARM compiler settings."""
 sources=['tests/elf_alignment_test.c','generated/payloads.c']
 flags='-std=gnu11 -static -mthumb -march=armv7-a -mfpu=neon -mfloat-abi=hard -Os -Wall -Wextra -Werror -Wno-unused-function -fno-strict-aliasing -DUB_HOST_TEST -ffunction-sections -fdata-sections -Wl,--gc-sections'
 inc=f'-I{shlex.quote(linux(ROOT/"core"))} -I{shlex.quote(linux(ROOT/"generated"))}'
 srcs=' '.join(shlex.quote(linux(ROOT/s)) for s in sources)
 rc,out=sh(f'arm-linux-gnueabihf-gcc {flags} {inc} {srcs} -o {TMP}/elf_alignment_arm')
 if not rc:rc,out=sh(f'qemu-arm -cpu cortex-a9 {TMP}/elf_alignment_arm')
 print('elf_alignment_arm '+('FAILED' if rc else 'passed'))
 if rc:print(out)
 return rc==0

def main():
 global TMP
 parent=PARENT or '/tmp'
 if not re.fullmatch(r'/[A-Za-z0-9._/-]*',parent) or '..' in parent.split('/'):
  print('UB820_TEST_TMP must be an absolute WSL path of letters, digits, . _ - /');return 2
 parent='/'+posixpath.normpath(parent).lstrip('/')
 prefix=posixpath.join(parent,'ub820-tests.')
 rc,out=sh(f'mkdir -p -- {parent} && mktemp -d -- {prefix}XXXXXX')
 path=out.strip()
 if rc or not re.fullmatch(re.escape(prefix)+r'[A-Za-z0-9]{6}',path):
  print('cannot create a run directory under',parent,out);return 2
 TMP=path
 print('test directory',TMP)
 try:
  return run_suite()
 finally:
  if not PARENT:
   subprocess.run(['wsl.exe','-d','Ubuntu','--','rm','-rf','--',path],check=True)

def run_suite():
 ok=True
 prefix=subprocess.run([sys.executable,str(ROOT/'tests'/'hook_codegen_test.py')],capture_output=True,text=True)
 print('hook_codegen_test '+('passed (8 cases)' if not prefix.returncode else 'FAILED\n'+prefix.stdout+prefix.stderr))
 ok&=not prefix.returncode
 tests=[
  ('elf_alignment_test',['tests/elf_alignment_test.c','generated/payloads.c'],{}),
  ('poke_test',['tests/poke_test.c'],{}),
  ('power_ui_test',['tests/power_ui_test.c','core/sys.c'],{}),
  ('power_test',['tests/power_test.c','core/sys.c'],{}),
  ('power_settings_test',['tests/power_settings_test.c','core/sys.c'],{}),
  ('cpu_ui_test',['tests/cpu_ui_test.c','core/sys.c'],{}),
  ('cpu_settings_test',['tests/cpu_settings_test.c','core/sys.c'],{}),
  ('cpu_backend_test',['tests/cpu_backend_test.c'],{}),
  ('native_popup_test',['tests/native_popup_test.c','core/sys.c'],{}),
  ('hook_transfer_test',['tests/hook_transfer_test.c'],{}),
  ('payload_thread_test',['tests/payload_thread_test.c'],{}),
  ('precision_clock_test',['tests/precision_clock_test.c'],{}),
  ('precision_wake_test',['tests/precision_wake_test.c'],{}),
  ('precision_idle_integration_test',['tests/precision_idle_integration_test.c'],{}),
  ('precision_transport_test',['tests/precision_transport_test.c'],{}),
  ('subtitle_precision_render_test',['tests/subtitle_precision_render_test.c'],{}),
  ('subtitle_precision_source_test',['tests/subtitle_precision_source_test.c'],{}),
  ('subtitle_precision_backend_test',['tests/subtitle_precision_backend_test.c'],{}),
  ('subtitle_luminance_test',['tests/subtitle_luminance_test.c'],{}),
  ('subtitle_luminance_backend_test',['tests/subtitle_luminance_backend_test.c'],{}),
  ('subtitle_brightness_ui_test',['tests/subtitle_brightness_ui_test.c','core/sys.c'],{}),
  ('subtitle_brightness_test',['tests/subtitle_brightness_test.c'],{}),
  ('subtitle_brightness_supervisor_test',['tests/subtitle_brightness_supervisor_test.c'],{}),
  ('subtitle_memory_test',['tests/subtitle_memory_test.c'],{}),
  ('disc_id_test',['tests/disc_id_test.c','core/sys.c'],{}),
  ('settings_tile_test',['tests/settings_tile_test.c'],{}),
  ('bd_launch_test',['tests/bd_launch_test.c'],{}),
  ('native_wait_test',['tests/native_wait_test.c'],{}),
  ('native_wait_api_test',['tests/native_wait_api_test.c'],{}),
  ('native_medm_wait_test',['tests/native_medm_wait_test.c'],{}),
  ('mgr_launch_test',['tests/mgr_launch_test.c'],{}),
  ('bd_preload_test',['tests/bd_preload_test.c'],{}),
  ('subtitle_ui_wait_test',['tests/subtitle_ui_wait_test.c'],{}),
  ('ssh_runtime_test',['tests/ssh_runtime_test.c','core/sys.c'],{}),
  ('subtitle_startup_test',['tests/subtitle_startup_test.c','core/sys.c'],{}),
  ('native_text_test',['tests/native_text_test.c'],{}),
  ('native_text_supervisor_test',['tests/native_text_supervisor_test.c'],{}),
  ('injector_test',['tests/injector_test.c','core/sys.c','core/log.c'],{'UB_LOG_FILE':TMP+'/injector.log'}),
  ('action_ipc_test',['tests/action_ipc_test.c','core/sys.c'],{}),
  ('keys_delivery_test',['tests/keys_delivery_test.c'],{}),
  ('subtitle_backend_test',['tests/subtitle_backend_test.c'],{}),
  ('subtitle_style_test',['tests/subtitle_style_test.c','core/sys.c'],{}),
  ('core_test',['tests/core_test.c','core/core.c','core/config.c','core/log.c','core/sys.c','core/procmem.c','core/injector.c'],
   {'UB_CONFIG_DIR':TMP+'/cfg','UB_LOG_FILE':TMP+'/core.log'}),
  ('region_test',['tests/region_test.c','modules/native_events/module.c','core/log.c','core/sys.c','core/procmem.c','core/injector.c','core/config.c'],
   {'UB_TEST_DIR':TMP+'/disc','UB_BD_RAM_DIR':TMP+'/disc/region-ram','UB_NATIVE_WAIT_DIR':TMP+'/disc/native-ready','UB_NATIVE_WAIT_LIBRARY':TMP+'/disc/region-ram/libbd_preload.so','UB_LOG_FILE':TMP+'/region.log','UB_CONFIG_DIR':TMP+'/regioncfg'}),
  ('remote_test',['tests/remote_test.c','core/log.c','core/sys.c','core/procmem.c','core/injector.c','core/config.c'],
   {'UB_LOG_FILE':TMP+'/remote.log','UB_CONFIG_DIR':TMP+'/remotecfg'}),
  ('log_test',['tests/log_test.c','core/log.c','core/sys.c','core/procmem.c'],
   {'UB_LOG_FILE':TMP+'/log.log'}),
  ('ssh_test',['tests/ssh_test.c','core/log.c','core/sys.c',*sorted(str(p.relative_to(ROOT)) for p in (ROOT/'generated').glob('tools.c'))],{'UB_LOG_FILE':TMP+'/ssh.log'}),
  ('ufs_test',['tests/ufs_test.c','tools/ufs/ufs.c'],{}),
  ('config_test',['tests/config_test.c','core/core.c','core/config.c','core/log.c','core/sys.c','core/procmem.c','core/injector.c','core/storage.c','core/disc_id.c',
                  *sorted(str(p.relative_to(ROOT)) for p in (ROOT/'modules').glob('*/module.c')),
                  *sorted(str(p.relative_to(ROOT)) for p in (ROOT/'generated').glob('*.c'))],
   {'UB_CONFIG_DIR':linux(ROOT.parent/'USB-CONTENTS'/'sub'),'UB_LOG_FILE':TMP+'/config.log'}),
  ('subtitle_engine_lifecycle_test',['tests/subtitle_engine_lifecycle_test.c'],{}),
  ('subtitle_catalogue_utf8_test',['tests/subtitle_catalogue_utf8_test.c'],{}),
  ('subtitle_request_resync_test',['tests/subtitle_request_resync_test.c'],{}),
  ('subtitle_precision_option_test',['tests/subtitle_precision_option_test.c'],{}),
  ('precision_seek_race_test',['tests/precision_seek_race_test.c'],{}),
  ('ui_fixes_test',['tests/ui_fixes_test.c','core/sys.c'],{}),
  ('sys_dir_test',['tests/sys_dir_test.c','core/sys.c'],{}),
  ('ufs_replace_test',['tests/ufs_replace_test.c','tools/ufs/ufs.c'],{}),
  ('storage_test',['tests/storage_test.c','core/sys.c'],{}),
  ('config_stick_test',['tests/config_stick_test.c'],{'UB_CONFIG_DIR':TMP+'/stickcfg'}),
 ]
 rc,out=sh(f'mkdir -p {TMP}/cfg {TMP}/disc {TMP}/usb {TMP}/regioncfg {TMP}/remotecfg')
 if rc:print(out);return 2
 for name,sources,defines in tests:
  rc,out=build(name,sources,defines)
  if rc:print(name,'BUILD FAILED\n'+out);ok=False;continue
  rc,out=sh(f'{FIXTURE_LOCK} {TMP}/{name}');summary=[line for line in out.splitlines() if line.startswith(name+' passed') or line.startswith(name+' FAILED')];print(summary[-1] if summary else (out.strip().splitlines()[-1] if out.strip() else name+': no output'))
  if rc:print(out);ok=False
 fw=os.environ.get('UB820_FIRMWARE_DIR')
 dat=Path(fw).parent/'data'/'lang'/'apl_lang.dat' if fw else None
 if dat and dat.exists():
  rc,out=build('lang_test',['tests/lang_test.c','core/core.c','core/config.c','core/log.c','core/sys.c','core/procmem.c','core/injector.c'],{'UB_LOG_FILE':TMP+'/lang.log'})
  if rc:print('lang_test BUILD FAILED\n'+out);ok=False
  else:
   rc,out=sh(f"{TMP}/lang_test '{linux(dat)}' {TMP}/pages.bin")
   if rc:print(out);ok=False
   else:
    pages=Path(sh(f'wslpath -w {TMP}/pages.bin')[1].strip())
    good,detail=decode_lang(dat,pages);print('lang_test',('passed: ' if good else 'FAILED: ')+detail);ok&=good
 else:print('lang_test skipped (set UB820_FIRMWARE_DIR)')
 ok&=elf_arm_check()
 ok&=sys_check()
 print('ALL PASSED' if ok else 'FAILURES')
 return 0 if ok else 1
if __name__=='__main__':sys.exit(main())
