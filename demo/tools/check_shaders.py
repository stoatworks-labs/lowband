"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py [--dump DIR]

Called from `tools/verify.sh`, with `--dump` pointing at `lbtest --dump-shaders`
output. Exit code 1 means the two copies have drifted. Galvo's check, by way
of colourunder.

------------------------------------------------------------------- why

`demo/plugin.js` holds the version line, the `kCommon` block and the three
GLSL bodies of `source/Shaders.cpp`. Two copies drift quietly: a demo that
draws a *plausible* tape looks exactly like one that draws the right one. The
page's claim is that the passes running in your browser are the plugin's, so
the claim needs something enforcing it. `lbtest` drives the real plugin class
and has never heard of this page.

------------------------------------------------------------------- what it does

1. Pulls each `R"( ... )"` literal out of Shaders.cpp and each matching
   backtick literal out of plugin.js, and compares them exactly -- no
   whitespace normalisation, no comment stripping.

   The one transformation is a decode, not a normalisation. A template literal
   cannot hold a raw backtick, backslash or `${`, so `sync_shaders.py` escapes
   those three; this undoes exactly those three and REJECTS any other backslash
   on the JS side, which could only be somebody hiding a difference.

2. Checks that the page's stage list (which bodies get `kCommon` in front) is
   the one Shaders.cpp's stage functions use, and regenerates the whole
   generated block and compares it with plugin.js, line for line.

3. With `--dump DIR`: assembles every stage from plugin.js's strings exactly as
   the page does (the version line, then kCommon for the stages that take it,
   then the body) and compares the whole text with the file the PLUGIN
   compiled, as `lbtest --dump-shaders DIR` wrote it.

------------------------------------------------------------------- what it cannot

Nothing here checks the PORT. `demo/model.js`, `dsp.js` and `engine.js` are
a hand translation of Model.cpp, Controls.cpp, the Clock, Dsp.cpp, Engine.cpp
and the CPU half of ProcessOpenGL; `demo/tools/check_port.sh` compares them
with the plugin's own C++. The GL calls in plugin.js (which texture on which
unit, which buffer is drawn into, the read-back) only a reader and the
page-against-`lbtest --pipe` comparison check.

If this fails because a shader changed in the plugin: `python3 demo/tools/sync_shaders.py`,
never an edit of plugin.js by hand.
"""
import os
import re
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sync_shaders  # noqa: E402

REPO = sync_shaders.REPO


def from_js(source, name):
    match = re.search(r"^const " + name + r" = `(.*?)`;$", source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?![`\\$])", body)
    if stray is not None:
        upto = body[: stray.start()]
        return None, f"backslash that is not one of the three escapes, at line {upto.count(chr(10)) + 1}"
    decoded = re.sub(r"\\([`\\$])", r"\1", body)
    return decoded, None


def first_difference(a_text, b_text, a_label, b_label):
    a_lines = a_text.splitlines()
    b_lines = b_text.splitlines()
    for i in range(max(len(a_lines), len(b_lines))):
        a = a_lines[i] if i < len(a_lines) else "<missing>"
        b = b_lines[i] if i < len(b_lines) else "<missing>"
        if a != b:
            print(f"        first difference at line {i + 1}")
            print(f"          {a_label}: {a}")
            print(f"          {b_label}: {b}")
            return


def main():
    dump = None
    if "--dump" in sys.argv:
        dump = sys.argv[sys.argv.index("--dump") + 1]

    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()
    shaders_cpp = sync_shaders.read("source/Shaders.cpp")

    problems = 0
    pieces = 0

    pieces += 1
    want = sync_shaders.cpp_version(shaders_cpp)
    got = re.search(r'^const K_VERSION = "(.*?)";$', js, re.M)
    if got is None or got.group(1) != want:
        print("FAIL  K_VERSION is not Shaders.cpp's kVersion")
        problems += 1
    else:
        print(f"ok    {'K_VERSION':<14} matches kVersion")

    texts = {}
    for name, symbol in sync_shaders.SHADERS:
        pieces += 1
        cpp_text = sync_shaders.cpp_shader(shaders_cpp, symbol)
        js_text, complaint = from_js(js, name)
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue
        texts[name] = js_text
        if cpp_text == js_text:
            print(f"ok    {name:<14} matches {symbol} ({len(cpp_text)} chars)")
            continue
        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in source/Shaders.cpp")
        first_difference(cpp_text, js_text, "C++", "js ")

    # Which stages take kCommon: the page's list against Shaders.cpp's functions.
    symbol_of = dict(sync_shaders.SHADERS)
    cpp_common = sync_shaders.cpp_stages(shaders_cpp)
    for filename, body_name, common in sync_shaders.STAGES:
        symbol = symbol_of[body_name]
        if symbol not in cpp_common:
            print(f"FAIL  no stage function in Shaders.cpp assembles {symbol}")
            problems += 1
        elif cpp_common[symbol] != common:
            print(f"FAIL  {filename}: Shaders.cpp assembles {symbol} {'with' if cpp_common[symbol] else 'without'} kCommon, the page {'with' if common else 'without'}")
            problems += 1
    page_common = re.search(r"^const STAGE_COMMON = \{(.*?)\};$", js, re.M | re.S)
    if page_common is None:
        print("FAIL  demo/plugin.js has no STAGE_COMMON table")
        problems += 1
    else:
        table = dict(re.findall(r"(\w+): (true|false)", page_common.group(1)))
        want_table = {body: ("true" if common else "false") for _, body, common in sync_shaders.STAGES}
        if table != want_table:
            print(f"FAIL  plugin.js's STAGE_COMMON {table} is not Shaders.cpp's {want_table}")
            problems += 1
        else:
            print(f"ok    {'STAGE_COMMON':<14} kCommon goes in front of the same {sum(1 for v in table.values() if v == 'true')} stages as in Shaders.cpp")

    where = sync_shaders.region(js)
    if where is None:
        print("FAIL  demo/plugin.js has no generated block")
        problems += 1
    elif js[where[0]: where[1]] != sync_shaders.block():
        print("FAIL  the generated block differs from what sync_shaders.py writes")
        problems += 1
    else:
        print("ok    the generated block is exactly what sync_shaders.py writes")

    stages = 0
    if dump is not None:
        version = re.search(r'^const K_VERSION = "(.*?)";$', js, re.M)
        common = texts.get("COMMON")
        if version is None or common is None:
            print("FAIL  cannot assemble the page's shaders (K_VERSION or COMMON missing)")
            problems += 1
        else:
            for filename, body_name, with_common in sync_shaders.STAGES:
                path = os.path.join(dump, filename)
                if not os.path.exists(path):
                    print(f"FAIL  {filename} is not in {dump}: did lbtest --dump-shaders run?")
                    problems += 1
                    continue
                with open(path) as handle:
                    compiled = handle.read()
                body = texts.get(body_name)
                if body is None:
                    problems += 1
                    continue
                page = version.group(1) + "\n" + (common if with_common else "") + body
                if page == compiled:
                    stages += 1
                    print(f"ok    {filename:<14} the page assembles exactly what the plugin compiles ({len(page)} chars)")
                else:
                    problems += 1
                    print(f"FAIL  {filename}: the page's assembled shader is not the plugin's")
                    first_difference(compiled, page, "plugin", "page  ")

    print()
    if problems:
        print(f"{problems} piece(s) differ -- run python3 demo/tools/sync_shaders.py, do not edit plugin.js by hand")
        return 1
    tail = f", and all {stages} assembled stages equal lbtest --dump-shaders" if dump is not None else ""
    print(f"all {pieces} shader pieces are identical to the plugin's{tail}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
