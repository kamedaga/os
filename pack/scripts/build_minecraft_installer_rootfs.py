#!/usr/bin/env python3
"""Compose the physical desktop installer payload from selected, verified layers."""
from __future__ import annotations
import argparse, copy, hashlib, importlib.util, io, json, os, posixpath
from pathlib import Path, PurePosixPath
import re, subprocess, tarfile

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / '.artifacts/minecraft-installer'
GAME = ROOT / '.artifacts/java25/minecraft-26.1.2'
PROTECTED = {'bin/sh', 'bin/ash', 'bin/busybox', 'etc/passwd', 'etc/group',
             'lib/ld-musl-x86_64.so.1', 'lib/libc.so', 'lib/libc.musl-x86_64.so.1',
             'lib/linux/ld-musl-x86_64.so.1', 'lib/linux/libc.so'}

def digest(path):
    with Path(path).open('rb') as f: return hashlib.file_digest(f, 'sha256').hexdigest()

def module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod); return mod

def clean(name):
    name = name.removeprefix('./').rstrip('/')
    if name in ('', '.'): return ''
    p = PurePosixPath(name)
    if p.is_absolute() or '..' in p.parts: raise ValueError('unsafe archive path: '+name)
    return p.as_posix()

class Payload:
    def __init__(self): self.entries = {}; self.opened = []; self.layers = []
    def archive(self, path, *, prefix='', strip=0, skip=frozenset(), package=False):
        path = Path(path).resolve(); arc = tarfile.open(path); self.opened.append(arc)
        self.layers.append({'source':str(path.relative_to(ROOT)), 'sha256':digest(path)})
        for item in arc:
            name = clean(item.name)
            if not name: continue
            parts = PurePosixPath(name).parts
            if len(parts) <= strip: continue
            name = clean(posixpath.join(prefix, *parts[strip:]))
            if name in skip: continue
            if not (item.isdir() or item.isfile() or item.issym() or item.islnk()):
                # APK signatures and package scripts are not runtime files.
                raise ValueError('unsupported archive entry: '+name)
            if name.split('/')[-1] in ('accounts.json', 'accounts.json.bak'):
                raise ValueError('account credentials must not enter installation media')
            if '/saves/' in name or '/logs/' in name: raise ValueError('private game data: '+name)
            info = copy.copy(item); info.name = name
            if info.islnk():
                target = PurePosixPath(clean(info.linkname)).parts
                if len(target) <= strip: raise ValueError('invalid hard link')
                info.linkname = clean(posixpath.join(prefix,*target[strip:]))
            if not package: info.uid = info.gid = 0
            self.entries[name] = (info, (arc,item) if item.isfile() else None)
    def file(self, source, name, mode=0o644):
        source=Path(source); info=tarfile.TarInfo(clean(name));info.size=source.stat().st_size;info.mode=mode
        self.entries[info.name]=(info,source)
    def text(self, name, content, mode=0o644):
        data=content.encode();info=tarfile.TarInfo(clean(name));info.size=len(data);info.mode=mode
        self.entries[info.name]=(info,data)
    def directory(self, name, mode=0o755):
        info=tarfile.TarInfo(clean(name));info.type=tarfile.DIRTYPE;info.mode=mode
        self.entries[info.name]=(info,None)
    def link(self,name,target):
        info=tarfile.TarInfo(clean(name));info.type=tarfile.SYMTYPE;info.linkname=target;info.mode=0o777
        self.entries[info.name]=(info,None)
    def tree(self,source,prefix='',skip=frozenset()):
        source=Path(source)
        for p in sorted(source.rglob('*')):
            if p.is_dir() and not p.is_symlink(): continue
            name=clean(posixpath.join(prefix,p.relative_to(source).as_posix()))
            if name in skip:continue
            if p.is_symlink():self.link(name,os.readlink(p))
            elif p.is_file():self.file(p,name,p.stat().st_mode&0o7777)
    def write(self, output):
        for name,(info,_) in self.entries.items():
            for parent in PurePosixPath(name).parents:
                old=self.entries.get(str(parent))
                if old and not old[0].isdir(): raise ValueError('non-directory parent: '+str(parent))
            if info.issym():
                target=posixpath.normpath(posixpath.join(posixpath.dirname(name),info.linkname)) if not info.linkname.startswith('/') else clean(info.linkname.lstrip('/'))
                if target=='..' or target.startswith('../'):raise ValueError('escaping link: '+name)
            if info.islnk() and info.linkname not in self.entries:raise ValueError('missing hard link target: '+name)
        inventory={};output=Path(output);output.parent.mkdir(parents=True,exist_ok=True)
        with tarfile.open(str(output)+'.tmp','w',format=tarfile.PAX_FORMAT) as arc:
            for name,(info,source) in sorted(self.entries.items(),key=lambda x:(x[1][0].islnk(),x[0])):
                info=copy.copy(info);info.mtime=0;info.uname=info.gname='';info.pax_headers={}
                if info.isfile():
                    stream=source[0].extractfile(source[1]) if isinstance(source,tuple) else io.BytesIO(source) if isinstance(source,bytes) else source.open('rb')
                    with stream:
                        reader=HashReader(stream);arc.addfile(info,reader)
                    inventory[name]={'sha256':reader.sha.hexdigest(),'bytes':info.size,'mode':info.mode,'uid':info.uid,'gid':info.gid}
                else:arc.addfile(info)
        Path(str(output)+'.tmp').replace(output)
        for arc in self.opened:arc.close()
        report={'layers':self.layers,'regular_files':inventory,'entries':len(self.entries),'bytes':output.stat().st_size,'sha256':digest(output)}
        output.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
        Path(str(output)+'.sha256').write_text(report['sha256']+'  '+output.name+'\n')
        return report
class HashReader:
    def __init__(self,source):self.source=source;self.sha=hashlib.sha256()
    def read(self,n):
        data=self.source.read(n);self.sha.update(data);return data

def graphics(p):
    lib=Path('/usr/lib/x86_64-linux-gnu'); existing=ROOT/'.artifacts/java25/openj9-guest/payload/opt/java25-openj9/glibc'
    roots=['dri/radeonsi_dri.so','libdrm_amdgpu.so.1']
    selected={}
    for name in roots:
        path=lib/name
        if not path.exists():raise FileNotFoundError(path)
        selected[name]=path.resolve()
        result=subprocess.run(['ldd',str(path)],check=True,text=True,capture_output=True).stdout
        if 'not found' in result:raise ValueError(result)
        for soname,source in re.findall(r'\s*(\S+) => (/\S+)',result):selected[soname]=Path(source).resolve()
    report=[]
    for name,source in sorted(selected.items()):
        if (existing/name).is_file():
            if digest(existing/name)!=digest(source):raise ValueError('glibc dependency drift: '+name)
            continue
        p.file(source,'opt/lwjgl/system/'+name,0o755)
        report.append({'name':name,'source':str(source),'sha256':digest(source)})
    (OUT/'radeonsi-libraries.json').write_text(json.dumps(report,indent=2)+'\n')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ssh-authorized-key-file',type=Path,required=True)
    parser.add_argument('--output',type=Path,default=OUT/'ROOTFS.TAR')
    parser.add_argument('--desktop-bundle',type=Path,default=ROOT/'.artifacts/live-desktop/desktop.tar.gz')
    args=parser.parse_args();OUT.mkdir(parents=True,exist_ok=True)
    key=args.ssh_authorized_key_file.read_text().strip()
    if not key.startswith('ssh-ed25519 ') or '\n' in key:parser.error('one Ed25519 public key required')
    base=OUT/'base-rootfs.tar'
    subprocess.run(['bash',str(ROOT/'pack/scripts/build_installer_rootfs.sh'),str(base)],check=True)
    p=Payload();p.archive(base)
    desktop=args.desktop_bundle
    desktop_manifest=json.loads(Path(str(desktop)+'.json').read_text())
    if digest(desktop)!=desktop_manifest['sha256']:raise ValueError('desktop archive digest mismatch')
    p.archive(desktop,package=True)
    # Select Epiphany's exact locked package closure; never copy a developer rootfs.
    desktop_api=module(ROOT/'pack/scripts/build_live_desktop_bundle.py','desktop_bundle')
    source=ROOT/'.artifacts/userland-fixtures/alpine-xfce-root'
    installed=desktop_api.records((source/'lib/apk/db/installed').read_bytes())
    browser=desktop_api.closure(installed,('epiphany','bash'),allow_browser=True)
    locked=desktop_api.locked_versions()
    for line in ((ROOT/'tools/manifests/alpine-epiphany-v3.22-x86_64.lock').read_text() + '\n' + (ROOT/'tools/manifests/alpine-libreoffice-v3.22-x86_64.lock').read_text()).splitlines():
        fields=line.split()
        if len(fields)==4 and fields[0] in ('main','community'):locked[fields[1]]=(fields[2],fields[3])
    shipped=set(json.loads(Path(str(desktop)+'.json').read_text())['packages'])
    for name in browser:
        if name in shipped:continue
        version=next(s[2:] for s in installed[name][0].decode().splitlines() if s.startswith('V:'))
        if name not in locked or locked[name][0]!=version:raise ValueError('unlocked browser package: '+name)
        archive=desktop_api.PACKAGE_CACHE/(name+'-'+version+'.apk')
        if digest(archive)!=locked[name][1]:raise ValueError('browser digest mismatch: '+name)
        # APK control members are not part of the runtime namespace.
        allow=set(installed[name][1]);t=tarfile.open(archive);p.opened.append(t)
        for item in t:
            if item.name in allow and item.name not in PROTECTED:
                info=copy.copy(item);info.name=clean(info.name);info.uid=info.gid=0
                p.entries[info.name]=(info,(t,item) if item.isfile() else None)
    # Reuse the already verified unmodified Void WebKit runtime and its launcher policy.
    webkit=OUT/'webkit-overlay';webkit.mkdir(exist_ok=True)
    (webkit/'usr/bin').mkdir(parents=True,exist_ok=True);(webkit/'usr/bin/epiphany').touch()
    subprocess.run(['python3',str(ROOT/'tools/stage_void_webkit.py'),str(webkit)],check=True)
    p.tree(webkit,skip={'usr/bin/epiphany'})
    for name in ['usr/share/glib-2.0/schemas/gschemas.compiled','usr/local/bin/epiphany']:
        p.file(source/name,name,0o755 if '/bin/' in name else 0o644)
    for path in (source/'usr/share/applications').glob('*Epiphany*.desktop'):p.file(path,path.relative_to(source).as_posix())
    for path in (source/'usr/share/dbus-1/services').glob('*Epiphany*.service'):p.file(path,path.relative_to(source).as_posix())
    for name in ['passwd','group']:p.file(ROOT/'userland/fixtures/base'/name,'etc/'+name)
    p.archive(ROOT/'.artifacts/java25/openj9-guest/jre.tar.gz',prefix='opt/java25-openj9',strip=1)
    for root in ['openj9-guest','lwjgl-path','lwjgl-window']:p.tree(ROOT/'.artifacts/java25'/root/'payload')
    graphics(p)
    for name in ['minecraft.tar.gz','prism.tar.gz','prism-runtime.tar.gz','prism-cache.tar.gz','prism-settings.tar.gz','fabric/fabric.tar.gz']:p.archive(GAME/name)
    # FileD reads this installed file for every future Linux exec. The base
    # archive must not silently keep an older compatibility runtime.
    p.file(ROOT/'.artifacts/lpr-linux-x86_64.so','lib/pacha/lpr-linux-x86_64.so',0o755)
    # The older runtime overlay contains its own OpenSSL pair. Restore the
    # prepared Prism policy after layering so it cannot replace Semeru's
    # matching libraries with the old text-embedded constant tables.
    for name,target in [('libssl.so.3','libssl-semeru.so'),
                        ('libcrypto.so.3','libcrypto-semeru.so'),
                        ('libcrypto-semeru.so','libcrypto-semeru.so')]:
        p.link('opt/prismlauncher/extra/'+name,'/opt/java25-openj9/lib/'+target)
    # An empty archive placeholder is not a resolver policy. Netd creates
    # this file exclusively from DHCP; leaving the placeholder blocks it.
    resolver=p.entries.get('etc/resolv.conf')
    if resolver and resolver[0].isfile() and resolver[0].size==0:
        del p.entries['etc/resolv.conf']
    # Installation configuration is fresh: no worlds, tokens or host class/shader caches.
    instance='root/.local/share/PrismLauncher/instances/CapabilityOS-26.1.2/'
    config=json.loads((GAME/'prism-settings-root'/instance/'mmc-pack.json').read_text())
    config['components']+=json.loads((GAME/'fabric/components.json').read_text())
    p.text(instance+'mmc-pack.json',json.dumps(config,indent=2)+'\n')
    p.text(instance+'minecraft/options.txt','renderDistance:16\nsimulationDistance:12\nenableVsync:true\nmaxFps:120\ngraphicsPreset:"fancy"\nrenderClouds:"true"\n')
    p.text('etc/pacha_boot_profile','installed-base physical-desktop\n')
    # Xauthority resolves the hostname returned by the Linux personality.
    p.text('etc/hosts','127.0.0.1 localhost localhost.localdomain\n127.0.1.1 pachaos\n::1 localhost localhost.localdomain\n')
    p.text('etc/shells','/bin/sh\n/bin/ash\n/bin/bash\n')
    p.text('etc/profile.d/lwjgl.sh',(ROOT/'tools/java25/lwjgl.sh').read_text())
    p.file(ROOT/'tools/java25/java','usr/local/bin/java',0o755)
    p.file(ROOT/'tools/java25/alsoft.conf','opt/lwjgl/alsoft.conf')
    p.link('lib64/ld-linux-x86-64.so.2','/opt/java25-openj9/glibc/ld-linux-x86-64.so.2')
    p.directory('root/.ssh',0o700)
    p.directory('etc/dropbear',0o700)
    p.text('root/.ssh/authorized_keys',key+'\n',0o600)
    p.text('etc/dropbear/README','SSH host keys are generated on the installed machine.\n')
    for command in ['dropbear','dropbearkey']:
        p.file(ROOT/'.artifacts/userland-fixtures/live-dropbear'/command,('usr/sbin/' if command=='dropbear' else 'usr/bin/')+command,0o755)
    # Canonical AMDGPU package only; no compiler/linker scratch directories.
    package=ROOT/'.artifacts/kobox2-amdgpu-package'
    for name in ['core.so','sandbox.elf','manifest.bin','firmware.bundle']:
        p.file(package/name,'srv/kobox2/amdgpu/'+name,0o755 if name.endswith(('.so','.elf')) else 0o644)
    p.tree(package/'modules','srv/kobox2/amdgpu/modules')
    p.file(package/'owner-policy.conf','etc/pacha/gpu-probe.conf')
    p.file(ROOT/'.artifacts/userland/gpud/GPUD.ELF','srv/gpud.elf',0o755)
    p.text('etc/inittab','::sysinit:/bin/busybox mkdir -p /run/user/0 /run/pacha /var/log\n::sysinit:/bin/busybox chmod 700 /run/user/0\n::sysinit:/bin/busybox touch /run/pacha/live-desktop.ready\n::respawn:-/bin/bash\n::ctrlaltdel:/sbin/reboot\n::shutdown:/bin/sync\n')
    launcher = ROOT/'pack/installer-rootfs/pacha-desktop'
    p.file(launcher, 'usr/bin/pacha-desktop', 0o755)
    p.link('usr/bin/pacha-live-desktop', 'pacha-desktop')
    report=p.write(args.output)
    if report['bytes']>=4<<30:raise ValueError('installer FAT32 payload exceeds 4 GiB')
    print(json.dumps({'entries':report['entries'],'bytes':report['bytes'],'sha256':report['sha256'],'output':str(args.output)}))
if __name__=='__main__':main()
