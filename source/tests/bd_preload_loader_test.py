"""Real ARM loader/constructor test using the player's glibc 2.20 under QEMU.

python source/tests/bd_preload_loader_test.py
No preloader test-mode macros, syscall wrappers or fake /proc files are used.
Evidence (including failure/unsupported behavior) is saved under analysis/.
"""
from pathlib import Path
import hashlib,json,os,re,shutil,subprocess,sys

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'analysis'/'next-pass'/'bd-loader'

def linux(path):
    s=str(Path(path).resolve()).replace('\\','/')
    return '/mnt/'+s[0].lower()+s[2:] if os.name=='nt' else s

def run(args,check=True,env=None):
    command=['wsl.exe','-d','Ubuntu','--exec',*args] if os.name=='nt' else args
    p=subprocess.run(command,capture_output=True,text=True,encoding='utf-8',errors='replace',env=env,timeout=60)
    if check and p.returncode:raise RuntimeError(p.stdout+p.stderr)
    return p

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    (OUT/'results.json').write_text(json.dumps({'status':'building','ld_bind_now':True},indent=2)+'\n')
    lib=OUT/'sysroot'/'lib';lib.mkdir(parents=True,exist_ok=True)
    fwlib=ROOT/'analysis'/'v182'/'rootfs'/'lib'
    for src,dst in [('ld-2.20-2014.11.so','ld-linux-armhf.so.3'),('libc-2.20-2014.11.so','libc.so.6')]:
        shutil.copyfile(fwlib/src,lib/dst)
    exe=OUT/'bd-loader-fixture';preload=OUT/'bd-preload-fixture.so'
    flags=['-mthumb','-march=armv7-a','-mfpu=neon','-mfloat-abi=hard','-Os','-fno-stack-protector','-Wall','-Wextra','-Werror']
    fixture=ROOT/'source'/'tests'/'fixtures'
    run(['arm-linux-gnueabihf-gcc',*flags,'-no-pie','-nostdlib','-nostartfiles','-Wl,--hash-style=sysv','-Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3',
         linux(fixture/'bd_loader_start.S'),linux(fixture/'bd_loader_main.c'),linux(lib/'libc.so.6'),'-lgcc','-o',linux(exe)])
    nm=run(['arm-linux-gnueabihf-nm','-n',linux(exe)]).stdout
    symbols={name:int(address,16)&~1 for address,name in re.findall(r'^([0-9a-f]+)\s+\w\s+(region_source[012])$',nm,re.M)}
    assert len(symbols)==3,nm
    digest=hashlib.sha256(exe.read_bytes()).hexdigest()
    defines=['-DUB_BD_EXE_HASH_182="'+digest+'"','-DUB_BD_EXE_HASH_185="'+digest+'"']
    defines += ['-DUB_BD_SOURCE%d=0x%xu'%(i,symbols['region_source%d'%i]) for i in range(3)]
    run(['arm-linux-gnueabihf-gcc',*flags,'-shared','-fPIC','-nostdlib','-nostartfiles','-Wl,--hash-style=sysv','-Wl,-z,defs',*defines,
         linux(ROOT/'source'/'modules'/'region'/'bd_preload.c'),linux(lib/'libc.so.6'),'-lgcc','-o',linux(preload)])
    inherited=OUT/'bd-inherited-fixture.so'
    run(['arm-linux-gnueabihf-gcc',*flags,'-shared','-fPIC','-nostdlib','-nostartfiles','-Wl,--hash-style=sysv',
         linux(fixture/'bd_loader_inherited.c'),linux(lib/'libc.so.6'),'-lgcc','-o',linux(inherited)])
    versions={}
    for item in [exe,preload,inherited]:
        report=run(['arm-linux-gnueabihf-readelf','-V',linux(item)]).stdout
        (OUT/(item.name+'.versions.txt')).write_text(report)
        versions[item.name]=sorted(set(re.findall(r'Name: (GLIBC_[0-9.]+)',report)))
        assert all(tuple(map(int,v.removeprefix('GLIBC_').split('.'))) <= (2,20)
                   for v in versions[item.name]),versions
    results=[]
    for policy,old in [(p,old) for p in [1,2,4,7] for old in ['',inherited.name]]:
        # LD_PRELOAD splits on spaces as well as colons; a basename searched
        # through LD_LIBRARY_PATH supports a workspace containing spaces.
        p=run(['qemu-arm','-L',linux(lib.parent),'-U','FIXTURE_OLD_CTOR','-E','LD_BIND_NOW=1','-E','LD_LIBRARY_PATH='+linux(lib)+':'+linux(OUT),'-E','LD_PRELOAD='+preload.name+(':'+old if old else ''),
               '-E','UB820_BD_POLICY=v1:'+str(policy),'-E','UB820_BD_PRELOAD_OLD='+old,'-E','FIXTURE_OLD='+old,'-E','FIXTURE_EXPECT='+str(policy),linux(exe)],check=False)
        result={'policy':policy,'inherited_preload':old,'returncode':p.returncode,'stdout':p.stdout,'stderr':p.stderr,'passed':not p.returncode and 'bd_preload_loader_test passed:' in p.stdout}
        results.append(result)
        print('policy',policy,'inherited',bool(old),'PASS' if result['passed'] else 'FAIL',p.stdout.strip(),p.stderr.strip())
    production=ROOT/'source'/'build'/'region_bd_preload.so'
    production_result={'present':production.is_file()}
    if production.is_file():
        # This is the unmodified production DSO, with its firmware hash pins.
        # It must load eagerly, remove private environment and retain stock
        # instructions because this executable has an unrecognized identity.
        versions_text=run(['arm-linux-gnueabihf-readelf','-V',linux(production)]).stdout
        production_result['version_requirements']=sorted(set(re.findall(r'Name: (GLIBC_[0-9.]+)',versions_text)))
        production_result['sha256']=hashlib.sha256(production.read_bytes()).hexdigest()
        p=run(['qemu-arm','-L',linux(lib.parent),'-E','LD_BIND_NOW=1','-E','LD_LIBRARY_PATH='+linux(lib)+':'+linux(production.parent),
               '-E','LD_PRELOAD='+production.name,'-E','UB820_BD_POLICY=v1:7','-E','UB820_BD_PRELOAD_OLD=',
               '-E','FIXTURE_OLD=','-E','FIXTURE_EXPECT=stock',linux(exe)],check=False)
        production_result.update(returncode=p.returncode,stdout=p.stdout,stderr=p.stderr,passed=not p.returncode and 'bd_preload_loader_test passed:' in p.stdout)
        print('production eager binding and unknown identity','PASS' if production_result['passed'] else 'FAIL',p.stdout.strip(),p.stderr.strip())
    report={'ld_bind_now':True,'production_asset':production_result,'executable_sha256':digest,'source_addresses':symbols,'libc_sha256':hashlib.sha256((lib/'libc.so.6').read_bytes()).hexdigest(),
            'loader_sha256':hashlib.sha256((lib/'ld-linux-armhf.so.3').read_bytes()).hexdigest(),'preload_sha256':hashlib.sha256(preload.read_bytes()).hexdigest(),
            'version_requirements':versions,'results':results}
    (OUT/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    return 0 if all(r['passed'] for r in results) and production_result.get('passed',True) else 1

if __name__=='__main__':
    try:
        sys.exit(main())
    except Exception as error:
        OUT.mkdir(parents=True,exist_ok=True)
        (OUT/'results.json').write_text(json.dumps({'status':'failed','ld_bind_now':True,'error':str(error)},indent=2)+'\n')
        raise
