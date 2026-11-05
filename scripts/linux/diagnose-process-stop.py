"""Build a private syscall/observer trace of test_process; fixed trial count.

Linux only. Never modifies the input source or signals a process itself. The
native test retains its original assertions, process ownership and deadlines.
Stdout/stderr are bytes because Linux comm may truncate a UTF-8 code point.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True,help='new private directory')
    parser.add_argument('--cxx',default='g++')
    parser.add_argument('--trials',type=int,default=100)
    args=parser.parse_args()
    if platform.system()!='Linux' or not 1<=args.trials<=1000:
        parser.error('Linux and 1..1000 predeclared trials required')
    source=args.source.resolve();output=args.output.resolve()
    output.mkdir()  # Refuse to overwrite any earlier run, including failures.
    identities={}
    for name in ('src/child_process.cpp','src/child_process.h','tests/test_process.cpp'):
        path=source/name;identities[name]=sha(path)
        shutil.copyfile(path,output/path.name)
    header=Path(__file__).with_name('process-stop-diagnostic.h')
    shutil.copyfile(header,output/'process-stop-diagnostic.h')
    cpp=(output/'child_process.cpp').read_text()
    changes={
        'pid=spawned;':'pid=spawned; process_diagnostic::record("spawn",pid,0,0);',
        'result=waitid(':'result=process_diagnostic::traced_waitid(',
        'owned=waitid(':'owned=process_diagnostic::traced_waitid(',
        '::kill(-pid,SIGKILL)':'process_diagnostic::traced_kill(-pid,SIGKILL)',
        'result=waitpid(':'result=process_diagnostic::traced_waitpid(',
    }
    for old,new in changes.items():
        assert cpp.count(old)==1,old
        cpp=cpp.replace(old,new)
    (output/'child_process.cpp').write_text('#include "process-stop-diagnostic.h"\n'+cpp)
    test=(output/'test_process.cpp').read_text()
    changes={
        'std::getline(stat,value);':'std::getline(stat,value);process_diagnostic::observation(pid,value);',
        'std::cerr<<e.what()':'process_diagnostic::dump();std::cerr<<e.what()',
        'std::cout<<"process: arguments':'process_diagnostic::dump();std::cout<<"process: arguments',
    }
    for old,new in changes.items():
        assert test.count(old)==1,old
        test=test.replace(old,new)
    (output/'test_process.cpp').write_text('#include "process-stop-diagnostic.h"\n'+test)
    metadata=dict(source=str(source),source_sha256=identities,header_sha256=sha(header),
        uname=list(platform.uname()),boot_id=Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
        namespaces={name:os.readlink('/proc/self/ns/'+name) for name in ('pid','user','mnt')},
        parent_status=Path('/proc/self/status').read_text(),trials=args.trials,
        original_assertions_and_deadlines=True)
    save(output/'metadata.json',metadata)
    exe=output/'process-stop-probe'
    command=[args.cxx,'-std=c++17','-O2','-pthread','-march=x86-64','-mtune=generic',
             str(output/'child_process.cpp'),str(output/'test_process.cpp'),'-o',str(exe)]
    compiled=subprocess.run(command,capture_output=True)
    (output/'compile.stdout').write_bytes(compiled.stdout);(output/'compile.stderr').write_bytes(compiled.stderr)
    save(output/'compile.json',dict(command=command,exit=compiled.returncode))
    if compiled.returncode:return compiled.returncode
    records=[]
    for index in range(args.trials):
        started=time.monotonic_ns();result=subprocess.run([str(exe)],capture_output=True)
        prefix=output/('%03d'%index)
        prefix.with_suffix('.stdout').write_bytes(result.stdout);prefix.with_suffix('.stderr').write_bytes(result.stderr)
        records.append(dict(index=index,exit=result.returncode,elapsed_ns=time.monotonic_ns()-started))
        save(output/'progress.json',records)
    assert all(sha(source/name)==value for name,value in identities.items())
    report=dict(exe_sha256=sha(exe),trials=args.trials,failed=[r for r in records if r['exit']],records=records,
                source_unchanged=True,instrumented_sha256={name:sha(output/name) for name in ('child_process.cpp','test_process.cpp','process-stop-diagnostic.h')})
    save(output/'report.json',report)
    print(json.dumps(dict(trials=args.trials,failures=len(report['failed']),exe_sha256=report['exe_sha256'])))
    return int(bool(report['failed']))


if __name__=='__main__':
    raise SystemExit(main())
