"""Generate a diagnostic adapter from pinned stb loops, never edit the dependency."""
import argparse
import hashlib
from pathlib import Path

PIN = 'b04df6aa2fdd476ebcddcafd8e634014e34d29f89f6b648b137f2efa66f3bd47'

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--header', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    raw = a.header.read_bytes()
    if hashlib.sha256(raw).hexdigest() != PIN:
        raise RuntimeError('stb source changed: re-audit extraction')
    text = raw.decode()
    def extract(name):
        start = text.index('static void ' + name + '(') if 'static void ' + name + '(' in text else text.index('static void ' + name + ' (')
        body = text.index('{', start)
        depth = 1
        end = body + 1
        while depth:
            depth += (text[end] == '{') - (text[end] == '}')
            end += 1
        return text[start:end].replace('static void ' + name, 'static Task pause_' + name, 1)
    gather = extract('stbir__vertical_gather_loop')
    # Yield before changing ring state; resume executes the original operations.
    anchor = '      if ( stbir_info->vertical_first )'
    assert gather.count(anchor) == 1
    gather = gather.replace(anchor, '      co_yield split_info->ring_buffer_last_scanline + 1;\n' + anchor)
    scatter = extract('stbir__vertical_scatter_loop')
    anchor = '      stbir__decode_scanline( stbir_info, y,'
    assert scatter.count(anchor) == 1
    scatter = scatter.replace(anchor, '      co_yield y;\n' + anchor)
    a.output.mkdir(parents=True, exist_ok=True)
    # Original full dual license travels with the extracted source.
    license_text = text[text.rfind('/*'):] if 'ALTERNATIVE' in text[text.rfind('/*'):] else text[text.index('/*\n------------------------------------------------------------------------------'):]
    contents={'paused_loops.h':'// Generated from locked stb SHA256 '+PIN+'\n'+gather+'\n'+scatter+'\n'+license_text,
              'stb_path.h':'#include "'+a.header.resolve().as_posix()+'"\n'}
    for name,value in contents.items():
        destination=a.output/name
        if destination.exists():
            if destination.read_text(encoding='utf-8')!=value.replace('\r\n','\n'):
                raise RuntimeError('Existing generated file differs: '+name)
        else:destination.write_text(value,encoding='utf-8')

if __name__ == '__main__':
    main()
