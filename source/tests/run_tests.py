"""Host tests (x86, in WSL Ubuntu): python tests/run_tests.py

Optional: UB820_FIRMWARE_DIR=<v182>/dtvrec/bin also runs the English-text page
test against the real apl_lang.dat (<v182>/dtvrec/data/lang/apl_lang.dat).
"""
from pathlib import Path
import os,re,struct,subprocess,sys
ROOT=Path(__file__).resolve().parent.parent
TMP='/tmp/ub820-tests'

def linux(p):
 s=str(Path(p).resolve()).replace('\\','/');return '/mnt/'+s[0].lower()+s[2:]
def sh(cmd):
 r=subprocess.run(['wsl.exe','-d','Ubuntu','--','bash','-lc',cmd],capture_output=True,text=True,encoding='utf-8',errors='replace')
 return r.returncode,r.stdout+r.stderr
def build(name,sources,defines):
 flags='-std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -DUB_HOST_TEST'
 inc=f'-I{linux(ROOT/"core")} -I{linux(ROOT/"generated")}'
 defs=' '.join(f"'-D{k}=\"{v}\"'" for k,v in defines.items())
 srcs=' '.join(linux(ROOT/s) for s in sources)
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
 inc=f'-I{linux(ROOT/"core")} -I{linux(ROOT/"generated")}'
 defs=f"'-DUB_CONFIG_DIR=\"{TMP}/syscfg\"' '-DUB_LOG_FILE=\"{TMP}/sys.log\"'"
 rc,out=sh(f'mkdir -p {TMP}/syscfg && arm-linux-gnueabihf-gcc {flags} {inc} {defs} {" ".join(linux(s) for s in srcs)} -o {TMP}/sys_check')
 if rc:print('sys_check BUILD FAILED'+chr(10)+out);return False
 # the helper sends stderr, and with it the trace, to its log
 rc,out=sh(f'cd {TMP} && rm -f sys.log && qemu-arm -strace ./sys_check 2>&1; s=$?; cat sys.log; exit $s')
 bad=sorted({m.group(0) for m in FORBIDDEN.finditer(out)})
 result=[l for l in out.splitlines() if 'sys_check' in l and ('passed' in l or 'FAILED' in l)]
 good=not rc and not bad and bool(result) and 'passed' in result[-1]
 print('sys_check',('passed: ' if good else 'FAILED: ')+(result[-1] if result else 'no result')+(f'; forbidden syscalls: {bad}' if bad else '; no syscall missing from Linux 4.4'))
 if not good:print(chr(10).join(l for l in out.splitlines() if 'FAIL' in l or FORBIDDEN.search(l))[:3000])
 return good

def main():
 ok=True
 tests=[
  ('core_test',['tests/core_test.c','core/core.c','core/config.c','core/log.c','core/sys.c','core/procmem.c','core/injector.c'],
   {'UB_CONFIG_DIR':TMP+'/cfg','UB_LOG_FILE':TMP+'/core.log'}),
  ('region_test',['tests/region_test.c','core/log.c','core/sys.c','core/procmem.c','core/injector.c','core/config.c'],
   {'UB_TEST_DIR':TMP+'/disc','UB_LOG_FILE':TMP+'/region.log','UB_CONFIG_DIR':TMP+'/regioncfg'}),
  ('remote_test',['tests/remote_test.c','core/log.c','core/sys.c','core/procmem.c','core/injector.c','core/config.c'],
   {'UB_LOG_FILE':TMP+'/remote.log','UB_CONFIG_DIR':TMP+'/remotecfg'}),
  ('log_test',['tests/log_test.c','core/log.c','core/sys.c','core/procmem.c'],
   {'UB_LOG_FILE':TMP+'/log.log'}),
  ('ssh_test',['tests/ssh_test.c','core/log.c','core/sys.c',*sorted(str(p.relative_to(ROOT)) for p in (ROOT/'generated').glob('tools.c'))],{'UB_LOG_FILE':TMP+'/ssh.log'}),
  ('config_test',['tests/config_test.c','core/core.c','core/config.c','core/log.c','core/sys.c','core/procmem.c','core/injector.c','core/storage.c',
                  *sorted(str(p.relative_to(ROOT)) for p in (ROOT/'modules').glob('*/module.c')),
                  *sorted(str(p.relative_to(ROOT)) for p in (ROOT/'generated').glob('*.c'))],
   {'UB_CONFIG_DIR':linux(ROOT.parent/'USB-CONTENTS'/'sub'),'UB_LOG_FILE':TMP+'/config.log'}),
 ]
 sh(f'rm -rf {TMP} && mkdir -p {TMP}/cfg {TMP}/disc {TMP}/usb {TMP}/regioncfg {TMP}/remotecfg')
 for name,sources,defines in tests:
  rc,out=build(name,sources,defines)
  if rc:print(name,'BUILD FAILED\n'+out);ok=False;continue
  rc,out=sh(f'{TMP}/{name}');print(out.strip().splitlines()[-1] if out.strip() else name+': no output')
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
 ok&=sys_check()
 print('ALL PASSED' if ok else 'FAILURES')
 return 0 if ok else 1
if __name__=='__main__':sys.exit(main())
