#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
import pathlib
import sys
script = pathlib.Path(__file__).with_name('runtime.js').read_text()
assert ')LUMIE_TRANSLATE"' not in script
pathlib.Path(sys.argv[1]).write_text(
    'constexpr char kTranslationRuntime[] = R"LUMIE_TRANSLATE(' + script + ')LUMIE_TRANSLATE";\n')
