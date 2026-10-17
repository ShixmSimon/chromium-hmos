"""Verify Dawn enum/table index agreement in an explicitly supplied source tree.

Usage: python3 tools/verify-dawn-toggle-order.py PATH_TO_EFFECTIVE_DAWN
Reads source only; does not compile or modify CI.
"""
from pathlib import Path
import re,sys,json
root=Path(sys.argv[1]);h=(root/'src/dawn/native/Toggles.h').read_text();cpp=(root/'src/dawn/native/Toggles.cpp').read_text();body=h.split('enum class Toggle')[1].split('{',1)[1].split('EnumCount,',1)[0];body=re.sub(r'/\*.*?\*/|//[^\n]*','',body,flags=re.S);names=re.findall(r'^\s*(\w+)\s*,',body,re.M);table=cpp.split('kToggleNameAndInfoList = {{',1)[1].split('}};',1)[0];entries=re.findall(r'\{Toggle::(\w+)\s*,',table);mismatch=[{'index':i,'enum':a,'table':b} for i,(a,b) in enumerate(zip(names,entries)) if a!=b];r={'enumCount':len(names),'tableCount':len(entries),'mismatches':mismatch,'pass':bool(names) and len(names)==len(entries) and not mismatch};print(json.dumps(r));sys.exit(0 if r['pass'] else 1)
