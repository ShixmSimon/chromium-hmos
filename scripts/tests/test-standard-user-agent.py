#!/usr/bin/env python3
"""Apply the production UA patch to both baselines and compile its generator."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
NAME = 'components/embedder_support/user_agent_utils.cc'


def postimage(patch):
    lines = patch.read_text().splitlines(True)
    start = lines.index('+++ b/' + NAME + '\n') + 1
    result = []
    for line in lines[start:]:
        if line.startswith('diff --git '):
            break
        if line.startswith(('+', ' ')):
            result.append(line[1:])
    return ''.join(result)


def function(source):
    start = source.index('std::string BuildOhosUserAgent(')
    end = source.index('\n}\n', start) + 3
    return source[start:end]


with tempfile.TemporaryDirectory(prefix='standard-ua-') as folder:
    target = Path(folder)
    path = target / NAME
    path.parent.mkdir(parents=True)
    for version, baseline in [
        (154, 'ohos-phone-ua-openharmony.patch'),
        (155, 'migration-155/chromium.patch'),
    ]:
        source = postimage(ROOT / 'patches' / baseline)
        if version == 154:
            # Reconstruct the touched regions from the checked-in base patch;
            # no Chromium checkout or network is needed for this regression.
            path.write_text(postimage(ROOT / 'patches/chromium-154-harmonyos.patch'))
            subprocess.run(['git', 'apply', '--include=' + NAME,
                            str(ROOT / 'patches' / baseline)], cwd=target, check=True)
        else:
            path.write_text(source)
        subprocess.run(['git', 'apply', str(ROOT / 'patches/ohos-standard-user-agent.patch')],
                       cwd=target, check=True)
        generator = function(path.read_text())
        for cpu in ['aarch64', 'x86_64']:
            unit = '''#include <cassert>
#include <string>
#include <string_view>
#include <initializer_list>
namespace base {
std::string StrCat(std::initializer_list<std::string_view> parts) {
 std::string result; for (auto p : parts) result += p; return result;
}
}
'''
            unit += f'constexpr char kOhosUserAgentCpu[] = "{cpu}";\n' + generator
            unit += f'''int main() {{
 const std::string product = "Chrome/{version}.0.0.0";
 const auto mobile = BuildOhosUserAgent(product, true);
 const auto desktop = BuildOhosUserAgent(product, false);
 assert(mobile == "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) " + product + " Mobile Safari/537.36");
 assert(desktop == "Mozilla/5.0 (X11; Linux {cpu}) AppleWebKit/537.36 (KHTML, like Gecko) " + product + " Safari/537.36");
 assert(mobile.find("OpenHarmony") == std::string::npos);
 assert(desktop.find("OpenHarmony") == std::string::npos);
}}
'''
            cpp = target / 'ua.cc'
            cpp.write_text(unit)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp),
                            '-o', str(target / 'ua')], check=True)
            subprocess.run([str(target / 'ua')], check=True)
        print(f'{version}: production patch applies; phone/desktop UA passed on ARM64/x86_64')
