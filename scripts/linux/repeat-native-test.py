"""Serial native CTest repetitions with best-effort owned PID/PGID/proc identity snapshots."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time


def proc_state(pid):
    root=Path('/proc')/str(pid)
    try:
        text=(root/'stat').read_text();end=text.rfind(')');fields=text[end+2:].split()
        result=dict(pid=pid,comm=text[text.find('(')+1:end],state=fields[0],ppid=int(fields[1]),pgid=int(fields[2]),session=int(fields[3]),start_ticks=int(fields[19]))
        try:
            result['exe']=os.readlink(root/'exe');info=(root/'exe').stat();result['exe_identity']=[info.st_dev,info.st_ino,info.st_size]
        except OSError:pass
        children=set()
        for path in (root/'task').glob('*/children'):
            try:children.update(map(int,path.read_text().split()))
            except OSError:pass
        result['children']=sorted(children)
        return result
    except (OSError,ValueError,IndexError):return None


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage',type=Path,required=True)
    parser.add_argument('--directory',type=Path,required=True)
    parser.add_argument('--test',default='scheduler_process')
    parser.add_argument('--repeats',type=int,default=20)
    args=parser.parse_args()
    if not 1<=args.repeats<=20:parser.error('repeats must be 1..20')
    suite=json.loads((args.stage/'tests/ctest-suite.json').read_text())['tests']
    test=next(item for item in suite if item['name']==args.test)
    command=[value.replace('@BIN@',str(args.stage/'bin')) for value in test['command']]
    assert len(command)==1 and Path(command[0]).is_file()
    sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
    original_sha=sha(command[0]);args.directory.mkdir(exist_ok=False)
    env=dict(os.environ,LD_LIBRARY_PATH=str(args.stage/'lib'))
    cases=[]
    for repeat in range(args.repeats):
        directory=args.directory/f'{repeat+1:02d}';directory.mkdir()
        (directory/'CTestTestfile.cmake').write_text(f'add_test({args.test} "{command[0]}")\nset_tests_properties({args.test} PROPERTIES TIMEOUT 30)\n')
        invocation=[str(args.stage/'tools/cmake/bin/ctest'),'--test-dir',str(directory),'-V','--output-on-failure']
        start=time.monotonic();process=subprocess.Popen(invocation,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=env)
        known={};snapshots=[]
        def snapshot():
            pending=[process.pid]+list(known);visited=set();rows=[]
            while pending:
                pid=pending.pop()
                if pid in visited:continue
                visited.add(pid);row=proc_state(pid)
                if row is None:continue
                if pid in known and known[pid]!=row['start_ticks']:
                    rows.append(dict(pid=pid,identity_changed=True));continue
                known[pid]=row['start_ticks'];rows.append(row);pending.extend(row['children'])
            snapshots.append(dict(elapsed_ms=(time.monotonic()-start)*1000,processes=rows))
        def stop_owned():
            for pid,ticks in reversed(list(known.items())):
                row=proc_state(pid)
                if row is not None and row['start_ticks']==ticks:
                    try:os.kill(pid,signal.SIGKILL)
                    except ProcessLookupError:pass
        while process.poll() is None:
            snapshot();time.sleep(0.002)
            if time.monotonic()-start>40:
                stop_owned();process.kill();break
        stdout,stderr=process.communicate(timeout=5);snapshot();wall_ms=(time.monotonic()-start)*1000
        (directory/'stdout.txt').write_text(stdout);(directory/'stderr.txt').write_text(stderr)
        (directory/'proc-snapshots.json').write_text(json.dumps(snapshots,indent=2)+'\n')
        case=dict(repeat=repeat+1,command=invocation,exit=process.returncode,wall_ms=wall_ms,
                  stdout=str(directory/'stdout.txt'),stderr=str(directory/'stderr.txt'),proc_snapshots=str(directory/'proc-snapshots.json'),observed_identities=len(known))
        cases.append(case)
        report=dict(test=args.test,requested_repeats=args.repeats,executed=len(cases),cases=cases,exe_sha256=original_sha,
                    all_passed=all(item['exit']==0 for item in cases),strace_available=shutil.which('strace'),
                    monitoring='2ms best-effort /proc snapshots of owned PID tree; snapshot overhead may affect timing; not syscall tracing',
                    first_failure_root_cause_resolved=False)
        (args.directory/'report.json').write_text(json.dumps(report,indent=2)+'\n')
        assert sha(command[0])==original_sha
        if process.returncode:
            print(json.dumps(case));raise SystemExit(process.returncode)
    print(json.dumps(dict(executed=len(cases),all_passed=True,root_cause_resolved=False)))


if __name__=='__main__':main()
