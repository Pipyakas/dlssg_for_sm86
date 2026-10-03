import collections
import json
import pathlib
import sys

pid = int(sys.argv[1])
path = pathlib.Path(__file__).resolve().parents[4] / 'logs' / 'sm86' / f'backend_{pid}.jsonl'
counts = collections.Counter()
statuses = collections.Counter()
maximum_sequence = 0
failed_launches = 0
for line in path.read_text().splitlines():
    try:
        item = json.loads(line)
    except json.JSONDecodeError:
        continue
    if item.get('event') == 'evaluate':
        counts[item.get('multi_frame_count')] += 1
        statuses[item.get('status')] += 1
        maximum_sequence = max(maximum_sequence, item.get('sequence', 0))
        failed_launches = max(failed_launches, item.get('failed_launches_total', 0))
print(json.dumps({'pid': pid, 'sampled_generated_counts': dict(counts), 'sampled_evaluate_statuses': dict(statuses), 'highest_evaluate_sequence': maximum_sequence, 'failed_kernel_launches_total': failed_launches}, indent=2))
