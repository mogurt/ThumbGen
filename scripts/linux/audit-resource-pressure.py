"""Audit completed archived Linux pressure reports without rewriting raw evidence."""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import statistics

from jsonschema import Draft202012Validator

parser=argparse.ArgumentParser()
parser.add_argument('--stage',type=Path,required=True)
parser.add_argument('--schema',type=Path,required=True)
parser.add_argument('--windows-report',type=Path,required=True)
args=parser.parse_args()
validator=Draft202012Validator(json.loads(args.schema.read_text()))
windows=json.loads(args.windows_report.read_text())
binding=json.loads((args.stage/'measurement-binding.json').read_text())
summaries={}
source_hashes=None
for platform in ('CentOS-7.9','rocky'):
    directory=args.stage/'results'/platform
    report=json.loads((directory/'report.json').read_text())
    storage=json.loads((directory/'storage.json').read_text())
    assert hashlib.sha256((directory/'report.json').read_bytes()).hexdigest()==storage['report_sha256']
    assert report['executable_sha256']==binding['product_sha256']
    assert report['native_image_validator']['sha256']==binding['tools']['thumbgen_validate_image']['measured_sha256']
    assert report['cycles']==2 and report['guard_mib']==4096
    assert report['outputs_complete'] and report['sampled_calibration_pass'] and report['complete']
    assert report['source_sha256_unchanged'] and report['input_sha256']==windows['input_sha256']
    assert len(report['profiles'])==12 and len(report['batches'])==4
    if source_hashes is None:source_hashes=report['input_sha256']
    else:assert source_hashes==report['input_sha256']
    units=[item['unit'] for item in report['profiles']+report['batches']]
    tasks=sum(len(unit['tasks']) for unit in units)
    previews=sum(len(task['validation']) for unit in units for task in unit['tasks'])
    records=[record for unit in units for record in unit['records']]
    assert (tasks,previews,len(records))==(108,264,124)
    for record in records:
        validator.validate(record)
        if record['type']=='result':assert record['execution']['threads']==1
    for unit in units:
        assert unit['valid'] and not unit['timeout'] and unit['stop_reason'] is None and not unit['capture_faults']
        assert unit['sample_interval_ms']==10
        for task in unit['tasks']:
            assert all(row['readable'] and row['matches_reported_size'] and row['fits_box'] for row in task['validation'])
            assert all(row['all_black'] is False and row['rgb_mean'] and row['rgb_max'] for row in task['validation'])
    profiles=[]
    for item in report['profiles']:
        assert item['layout_validation']['valid'] and not item['worker_peak_over_estimate']
        unit=item['unit']
        profiles.append(dict(id=item['id'],layout=item['layout_validation']['actual'],
            sampled_worker_peak_mib=unit['sampled_peak_child_rss_mib'],estimated_task_mib=item['estimate_mib'],
            peak_over_estimate_ratio=unit['sampled_peak_child_rss_mib']/item['estimate_mib']))
    batches=[]
    assert [(b['workers'],b['budget_mib'],b['layout']) for b in report['batches']]==[(1,0,'mixed'),(4,4096,'mixed'),(4,1280,'mixed'),(4,1280,'large-first')]
    for item in report['batches']:
        unit=item['unit']
        assert not item['output_sha256_mismatches'] and not item['sampled_peak_exceeds_budget']
        samples=[sample['tree_rss_mib'] for sample in unit['samples']]
        batches.append(dict(workers=item['workers'],budget_mib=item['budget_mib'],layout=item['layout'],tasks=len(unit['tasks']),
            wall_seconds=unit['wall_ms']/1000,tree_peak_mib=unit['sampled_peak_tree_rss_mib'],
            child_peak_mib=unit['sampled_peak_child_rss_mib'],parent_peak_mib=unit['sampled_peak_parent_rss_mib'],
            time_quarter_medians_mib=item['rss_quarter_medians_mib'],
            rss_distribution_quartiles_mib=statistics.quantiles(samples,n=4,method='inclusive'),
            queue_max_ms=item['queue_max_ms'],jpeg_sha_mismatches=len(item['output_sha256_mismatches'])))
    summaries[platform]=dict(tasks=tasks,previews=previews,schema_records=len(records),
        product_identity=report['identity'],executable_sha256=report['executable_sha256'],
        native_image_validator=report['native_image_validator'],oiio_simd=report['capabilities']['cpu']['oiio_simd'],
        library_sha256=report['runtime_library_sha256'],host=report['host'],profiles=profiles,batches=batches,
        created_utc=report['created_utc'],storage=json.loads((directory/'storage.json').read_text()))
    copy=json.loads((directory/'source-copy.json').read_text())
    assert len(copy['inputs'])==12
    assert all(row['bytes_identical'] and row['sha256']==report['input_sha256'][row['id']] for row in copy['inputs'])
centos_end=summaries['CentOS-7.9']['storage']['report_mtime_ns']/1e9
rocky_start=datetime.fromisoformat(summaries['rocky']['created_utc']).timestamp()
assert centos_end<rocky_start
summary=dict(platforms=summaries,linux_runs_sequential=True,centos_report_completed_before_rocky_start_seconds=rocky_start-centos_end,
    inputs_byte_identical_to_windows=True,windows_input_storage='D:/github/ThumbGen/.deps/pressure-fixtures-v2',
    linux_input_storage='separate per-distribution ext4 copies; paths and filesystems differ from Windows',
    quality_scope=dict(preset_requested='balanced',quality_requested=90,actual_encoder_quality_verified=False,
        known_mainline_finding='Legacy source compression metadata can override requested output encoding; do not treat this as actual Q90 or quality-equivalent timing after the fix'),
    measurement_source_commit='5709348f6ee5461c01754b093b5d2c06759f47cb',
    product_source_commit='0b6d5fd9369e88186be4b3d49d216fcf272a58d0',
    timeout_seconds_per_cli=300,watchdog_tree_mib=4096,sample_interval_ms=10,
    limits=['Sampled RSS can miss peaks; summed RSS may double-count shared pages.',
        'Native read-back runs after measured CLI exit and is outside the CLI RSS/time samples.',
        'The two owned Linux runs are sequential; the physical host is not a dedicated isolated benchmark host.',
        'No shared-storage, actual-Q90, release-default or formal QA gate acceptance.'])
(args.stage/'pressure-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps({key:dict(tasks=value['tasks'],previews=value['previews'],schema_records=value['schema_records'],
    oiio_simd=value['oiio_simd'],max_profile_ratio=max(p['peak_over_estimate_ratio'] for p in value['profiles']),batches=value['batches'])
    for key,value in summaries.items()},indent=2))
