#!/usr/bin/env python3
"""Build a signed local release directory; never uploads or upgrades devices."""
import argparse,hashlib,json,os,re,subprocess,tempfile
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--image',type=Path,required=True)
p.add_argument('--key',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args();b=a.image.read_bytes()
if len(b)<1024 or len(b)>0x380000 or b[0]!=0xe9:raise SystemExit('Invalid application size/header')
if int.from_bytes(b[12:14],'little')!=5:raise SystemExit('Release must target ESP32-C3')
if int.from_bytes(b[32:36],'little')!=0xabcd5432 or b[80:112].split(b'\0')[0]!=b'passport-gpt-live':raise SystemExit('Wrong project image')
version=b[48:80].split(b'\0')[0].decode('ascii')
if not re.fullmatch(r'\d+\.\d+\.\d+',version):raise SystemExit('Release requires a numeric three-part version')
sha=hashlib.sha256(b).hexdigest()
canonical=f'passport-gpt-live\n{version}\n{len(b)}\n{sha}\n'.encode()
with tempfile.TemporaryDirectory() as td:
 data=Path(td)/'signed-data';signature=Path(td)/'signature';data.write_bytes(canonical)
 subprocess.run(['openssl','dgst','-sha256','-sign',str(a.key),'-out',str(signature),str(data)],check=True)
 sig=signature.read_bytes().hex()
a.out.mkdir(parents=True,exist_ok=True)
image=a.out/f'{sha}.bin'
if not image.exists():
 image.write_bytes(b);image.chmod(0o600)
elif image.read_bytes()!=b:raise SystemExit('Immutable release filename collision')
manifest={'project':'passport-gpt-live','version':version,'size':len(b),'sha256':sha,'signature':sig}
tmp=a.out/'latest.json.next';tmp.write_text(json.dumps(manifest,separators=(',',':'))+'\n');tmp.chmod(0o600);os.replace(tmp,a.out/'latest.json')
print(f'Signed release {version}: {len(b)} bytes, SHA-256 {sha}')
