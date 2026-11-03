# Run only with real-video-harness.patch installed in a disposable debug build.
# Does not install, change screen settings, or enable Chromium overlays.
import subprocess,time,json,re,pathlib,argparse,os
parser=argparse.ArgumentParser()
parser.add_argument('--device',required=True)
parser.add_argument('--output',required=True)
parser.add_argument('--base-url',default='http://127.0.0.1:18880/video-overlay-hdr/media/')
parser.add_argument('--hdc',default=os.environ.get('HDC','/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc'))
args=parser.parse_args()
root=pathlib.Path(args.output);root.mkdir(parents=True,exist_ok=False)
hdc=args.hdc
serial=args.device
def shell(*args):
 return subprocess.check_output([hdc,'-t',serial,'shell',*args],text=True,timeout=20)
def logs(): return shell('hilog','-x','-T','LumieRealVideo,LumieHdrBuffer')
results=[]
for index,fixture in enumerate(['sdr-avc.mp4','sdr-hevc10.mp4','hdr10-hevc.mp4','sdr-avc.mp4','hdr10-hevc.mp4','hdr10-hevc.mp4','sdr-avc.mp4']):
 prior=logs().splitlines(); marker=prior[-1] if prior else ''
 start=time.monotonic(); result={'fixture':fixture,'index':index};prefix=root/f'{index}-{fixture}'
 print('START',index,fixture,flush=True)
 shell('aa','start','-b','com.miramira8295.lumiebrowser','-a','EntryAbility','--ps','lumieRealVideo',args.base_url.rstrip('/')+'/'+fixture)
 time.sleep(1.5)
 tree=shell('hidumper','-s','10','-a','RSTree')
 layers=[l.strip() for l in tree.splitlines() if 'Name [LumieWebLayer]' in l or 'Name [LumieVideoLayer]' in l]
 prefix.with_suffix(prefix.suffix+'.layers.txt').write_text('\n'.join(layers))
 result['active_layers']=len([l for l in layers if '| SURFACE_NODE' in l])
 if index==2:
  for layer in layers[:2]:
   found=re.search(r'SURFACE_NODE\[(\d+)\]',layer)
   if found:
    detail=shell('hidumper','-s','10','-a','surfacenode '+found[1])
    prefix.with_suffix(prefix.suffix+'.node-'+found[1]+'.txt').write_text(detail)
  subprocess.run(['devecocli','ui','screenshot','--device',serial,'--path',str(root/'hdr.png')],check=True,timeout=20)
 if index == 5: shell('aa','start','-b','com.miramira8295.lumiebrowser','-a','EntryAbility','--pb','lumieRealVideoStop','true')
 while time.monotonic()-start<30:
  text=logs();fresh=text.split(marker,1)[-1] if marker in text else text
  drain=re.search(r'drained submitted=(\d+) released=(\d+) outstanding=(\d+)',fresh)
  if drain: break
  time.sleep(.5)
 prefix.with_suffix(prefix.suffix+'.log').write_text(fresh)
 result['drain']=list(map(int,drain.groups())) if drain else None
 result['seconds']=round(time.monotonic()-start,2)
 result['errors']=re.findall(r'(?:codec_error|submit_rejected|host_unavailable|release_failed)[^\n]*',fresh)
 tree=shell('hidumper','-s','10','-a','RSTree')
 result['layers_after_close']=sum('Name [LumieWebLayer]' in l or 'Name [LumieVideoLayer]' in l for l in tree.splitlines())
 result['pass']=(result['drain'] is not None and result['drain'][0]==result['drain'][1] and result['drain'][2]==0 and (0<result['drain'][0]<180 if index==5 else result['drain'][0]==180)) and not result['errors'] and result['active_layers']==2 and not result['layers_after_close']
 results.append(result);(root/'results.json').write_text(json.dumps(results,indent=2))
 print(json.dumps(result),flush=True)
 if not result['pass']: break

raise SystemExit(0 if len(results)==7 and all(r['pass'] for r in results) else 1)
