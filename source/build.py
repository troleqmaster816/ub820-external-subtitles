from pathlib import Path
import hashlib,io,json,os,struct,subprocess,sys
from elftools.elf.elffile import ELFFile
from capstone import Cs,CS_ARCH_ARM,CS_MODE_THUMB,CS_GRP_JUMP,CS_GRP_CALL
from capstone.arm import ARM_OP_MEM,ARM_OP_REG,ARM_REG_PC
from keystone import Ks,KS_ARCH_ARM,KS_MODE_THUMB
ROOT=Path(__file__).resolve().parent
SRC=ROOT/'src';BUILD=ROOT/'build';BUILD.mkdir(exist_ok=True)
# Directory holding the stock 1.82 apl_dispsrv and apl_meda_bdplay executables.
FIRM=Path(os.environ.get('UB820_FIRMWARE_DIR','firmware'))
USB=ROOT.parent/'USB-CONTENTS'
def linux(p):
 s=str(p.resolve()).replace('\\','/');return '/mnt/'+s[0].lower()+s[2:]
def run(args,log):
 r=subprocess.run(['wsl.exe','-d','Ubuntu','--',*args],capture_output=True,text=True,encoding='utf-8',errors='replace')
 (BUILD/log).write_text(r.stdout+r.stderr)
 if r.returncode:raise RuntimeError((r.stdout+r.stderr)[-7000:])
 return r.stdout
def elf_bytes(path):
 data=path.read_bytes();return data,ELFFile(io.BytesIO(data))
def readva(elf,addr,n):
 for s in elf.iter_segments():
  if s['p_type']=='PT_LOAD' and s['p_vaddr']<=addr and addr+n<=s['p_vaddr']+s['p_filesz']:
   return s.data()[addr-s['p_vaddr']:addr-s['p_vaddr']+n]
 raise ValueError(hex(addr))
ks=Ks(KS_ARCH_ARM,KS_MODE_THUMB);cs=Cs(CS_ARCH_ARM,CS_MODE_THUMB);cs.detail=True
def asm(s):return bytes(ks.asm(s)[0])
def make_hook(elf,raw):
 addr=int(raw.get('target',raw.get('target_hex')),0)
 assert addr%4==0
 ins=[];size=0
 for x in cs.disasm(readva(elf,addr,32),addr):
  ins.append(x);size+=x.size
  if size>=4:break
 assert size<=32
 expected=readva(elf,addr,size);tramp=b'';rel=[]
 for x in ins:
  if x.group(CS_GRP_JUMP) or x.group(CS_GRP_CALL) or x.mnemonic.startswith('it'):raise ValueError('Unsupported hook prologue flow: '+str(x))
  mem=next((o for o in x.operands if o.type==ARM_OP_MEM and o.mem.base==ARM_REG_PC),None)
  if mem is not None:
   assert x.mnemonic.startswith('ldr') and x.operands[0].type==ARM_OP_REG
   reg=x.reg_name(x.operands[0].reg);assert reg!='pc'
   literal=((x.address+4)&~3)+mem.mem.disp
   value=struct.unpack('<I',readva(elf,literal,4))[0]
   tramp+=asm(f'movw {reg}, #{value&65535}; movt {reg}, #{value>>16}')
   rel.append({'address':hex(x.address),'literal':hex(literal),'value':hex(value),'register':reg})
  elif ARM_REG_PC in x.regs_access()[0]:raise ValueError('Unsupported PC-relative prefix '+str(x))
  else:tramp+=x.bytes
 if len(tramp)%4:tramp+=b'\x00\xbf'
 tramp+=b'\xdf\xf8\x00\xf0'+struct.pack('<I',addr+size+1)
 return {'target':addr,'length':size,'replacement':raw['replacement'],'original_pointer':raw['original_pointer'],
         'expected':expected,'trampoline':tramp,'relocated_literals':rel,
         'decoded':[{'address':hex(x.address),'mnemonic':x.mnemonic,'operands':x.op_str} for x in ins]}
def cblob(name,data):return 'static const unsigned char '+name+'[]={'+','.join('0x%02x'%b for b in data)+'};\n'
def compile_payload(name):
 args=['arm-linux-gnueabihf-gcc','-shared','-fPIC','-nostdlib','-fno-stack-protector','-mthumb','-march=armv7-a','-mfpu=neon','-mfloat-abi=hard','-Os','-Wall','-Wextra','-Werror','-Wl,--hash-style=sysv','-Wl,--build-id=none','-I'+linux(SRC),'-o',linux(BUILD/(name+'.so')),linux(SRC/(name+'.c'))]
 run(args,name+'-compile.log')
 return BUILD/(name+'.so')
def main():
 for n in ['menu','backend']:compile_payload(n)
 definitions='#include "injector.h"\n';reports=[]
 for label,name,manifest_path,program in [('ui','menu',ROOT/'ui_hooks.json','apl_dispsrv'),('backend','backend',SRC/'backend-hooks.json','apl_meda_bdplay')]:
  manifest=json.loads(manifest_path.read_text());data,elf=elf_bytes(FIRM/program)
  sha=hashlib.sha256(data).hexdigest();assert sha==manifest.get('sha256',manifest.get('firmware_sha256'))
  blob,plugin=elf_bytes(BUILD/(name+'.so'));sym={s.name:s for s in plugin.get_section_by_name('.dynsym').iter_symbols()}
  relocs=[]
  for s in plugin.iter_sections():
   if s['sh_type']=='SHT_REL':
    for r in s.iter_relocations():assert r['r_info_type'] in (0,2,21,22,23);relocs.append(r['r_info_type'])
  hooks=[make_hook(elf,h) for h in manifest['hooks']]
  definitions+=cblob(label+'_elf',blob)
  for i,h in enumerate(hooks):
   assert h['replacement'] in sym and h['original_pointer'] in sym
   definitions+=cblob(f'{label}_expected_{i}',h['expected'])+cblob(f'{label}_trampoline_{i}',h['trampoline'])
  definitions+=f'static const struct ub_hook_spec {label}_hooks[]={{\n'
  for i,h in enumerate(hooks):definitions+='{0x%x,%d,"%s","%s",%s_expected_%d,%s_trampoline_%d,%d},\n'%(h['target'],h['length'],h['replacement'],h['original_pointer'],label,i,label,i,len(h['trampoline']))
  definitions+='};\n'
  bindings=manifest.get('bindings',[])
  definitions+=f'static const struct ub_binding {label}_bindings[]={{\n'
  for b in bindings:
   assert b['symbol'] in sym
   definitions+='{"%s",%su},\n'%(b['symbol'],b['value'])
  if not bindings:definitions+='{0,0},\n'
  definitions+='};\n'
  segments=sorted([(s['p_vaddr']&~4095,(s['p_vaddr']+s['p_memsz']+4095)&~4095) for s in elf.iter_segments() if s['p_type']=='PT_LOAD'])
  gateway_hint=next(end for (_,end),(start,_) in zip(segments,segments[1:]) if start-end>=4096)
  definitions+=f'static const struct ub_payload ub_{label}_payload={{"{program}","{sha}",{label}_elf,sizeof({label}_elf),{label}_hooks,{len(hooks)},{label}_bindings,{len(bindings)},0x{gateway_hint:x}}};\n'
  reports.append({'label':label,'program':program,'firmware_sha256':sha,'payload_sha256':hashlib.sha256(blob).hexdigest(),
                  'relocation_types':sorted(set(relocs)),'gateway_hint':hex(gateway_hint),
                  'undefined_imports':[n for n,s in sym.items() if s['st_shndx']=='SHN_UNDEF' and n],
                  'hooks':[{**h,'expected':h['expected'].hex(),'trampoline':h['trampoline'].hex()} for h in hooks]})
 (SRC/'embedded_payloads.h').write_text(definitions)
 args=['arm-linux-gnueabihf-gcc','-std=gnu11','-static','-Os','-s','-mthumb','-march=armv7-a','-mfpu=neon','-mfloat-abi=hard','-fno-strict-aliasing','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Wall','-Wextra','-Werror','-Wno-unused-function','-I'+linux(SRC),linux(SRC/'supervisor.c'),linux(SRC/'injector.c'),'-o',linux(BUILD/'engage')]
 run(args,'supervisor-compile.log')
 smoke=run(['qemu-arm',linux(BUILD/'engage'),'--selftest'],'supervisor-selftest.log')
 digest=run(['qemu-arm',linux(BUILD/'engage'),'--hash',linux(USB/'UB820-SUBTITLES.MKV')],'supervisor-hash-test.log').strip()
 assert digest==hashlib.sha256((USB/'UB820-SUBTITLES.MKV').read_bytes()).hexdigest()
 report={'target_firmware':'1.82','components':reports,'supervisor_sha256':hashlib.sha256((BUILD/'engage').read_bytes()).hexdigest(),
         'supervisor_bytes':(BUILD/'engage').stat().st_size,'selftest':smoke.strip(),'SHA256_implementation_known_file_pass':True,
         'USB_written':False,'hardware_executed':False}
 (BUILD/'build.json').write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps({k:v for k,v in report.items() if k!='components'},indent=2))
if __name__=='__main__':main()
