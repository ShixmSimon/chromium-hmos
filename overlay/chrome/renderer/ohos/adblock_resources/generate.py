#!/usr/bin/env python3
# Copyright 2026. BSD-style license in LICENSE.
import pathlib,sys
root=pathlib.Path(__file__).resolve().parent
script='(()=>{\n'+(root/'procedural_filters.js').read_text()+'\n'+(root/'controller.js').read_text()+'\n})()'
assert ')LUMIE_SCRIPT"' not in script
pathlib.Path(sys.argv[1]).write_text('// Generated from pinned Brave procedural evaluator and Lumie scheduler.\n'
    'constexpr char kAdblockRuntime[] = R"LUMIE_SCRIPT('+script+')LUMIE_SCRIPT";\n')
