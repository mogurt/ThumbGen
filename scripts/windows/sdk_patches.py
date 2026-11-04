"""Small, exact patches to the fixed upstream SDK sources."""
from pathlib import Path
import subprocess


OLD = '''            # MSVC makes every intrinsic through AVX2 available regardless of
            # the /arch: setting, and it never predefines the __SSE*__ macros
            # that simd.h keys off. Since all x86-64 hardware is SSE4.2 or
            # better, just say so.
            list (APPEND SIMD_COMPILE_FLAGS "/D__SSE2__" "/D__SSE4_1__" "/D__SSE4_2__")'''
NEW = '''            # MSVC exposes intrinsics regardless of /arch. Advertise only
            # requested ISA levels; x86-64 guarantees SSE2, not SSE4.2.
            list (APPEND SIMD_COMPILE_FLAGS "/D__SSE2__")
            if ("sse4.1" IN_LIST SIMD_FEATURE_LIST OR "sse4.2" IN_LIST SIMD_FEATURE_LIST
                OR _highest_msvc_arch GREATER 0)
                list (APPEND SIMD_COMPILE_FLAGS "/D__SSE4_1__")
            endif ()
            if ("sse4.2" IN_LIST SIMD_FEATURE_LIST OR _highest_msvc_arch GREATER 0)
                list (APPEND SIMD_COMPILE_FLAGS "/D__SSE4_2__")
            endif ()'''


def expected_oiio_patch(source):
    original = subprocess.check_output(['git', '-C', str(source), 'show', 'HEAD:src/cmake/compiler.cmake'])
    old, new = OLD.encode(), NEW.encode()
    if original.count(old) != 1:
        raise RuntimeError('Pinned OIIO compiler patch context differs')
    return original.replace(old, new)


def expected_oiio_patches(source):
    header = 'src/include/OpenImageIO/simd.h'
    original = subprocess.check_output(['git', '-C', str(source), 'show', 'HEAD:' + header])
    start = b'#  elif defined(_MSC_VER) && !defined(__clang__) && !defined(__INTEL_LLVM_COMPILER)'
    end = b'#  elif defined(__SSSE3__)'
    if original.count(start) != 1 or original.count(end) != 1:
        raise RuntimeError('Pinned OIIO SIMD header patch context differs')
    first, last = original.index(start), original.index(end)
    return {'src/cmake/compiler.cmake': expected_oiio_patch(source), header: original[:first] + original[last:]}


def approved_changes(source, status):
    changes = status.splitlines()
    expected = expected_oiio_patches(source)
    return bool(changes) and all(line.startswith(' M ') and line[3:] in expected and
                                (source / line[3:]).read_bytes() == expected[line[3:]] for line in changes)


def apply(source):
    for relative, patched in expected_oiio_patches(source).items():
        target = source / relative
        original = subprocess.check_output(['git', '-C', str(source), 'show', 'HEAD:' + relative])
        if target.read_bytes() not in (original, patched):
            raise RuntimeError('Unrecognized OIIO source changes: ' + relative)
        if target.read_bytes() != patched:
            target.write_bytes(patched)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    apply(parser.parse_args().source)
