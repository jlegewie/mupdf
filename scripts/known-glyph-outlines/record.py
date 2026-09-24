"""Record review decisions by candidate index: record.py candidates.jsonl review.json 'spec'
spec lines: '<idx> a' accept | '<idx> r <note>' reject | '<idx> = <char> <note>' relabel"""
import json, sys, os
cands = [json.loads(l) for l in open(sys.argv[1])]
path = sys.argv[2]; review = json.load(open(path)) if os.path.exists(path) else {}
for line in sys.argv[3].strip().splitlines():
    parts = line.split(None, 2); i = int(parts[0]); c = cands[i]
    e = {'idx': i, 'emit': c['emit'], 'proposed': c['label'], 'font': next(iter(c['fonts'])), 'wrong_chars': c['wrong_chars']}
    if parts[1] == 'a': e['decision'] = 'accept'
    elif parts[1] == 'r': e['decision'] = 'reject'; e['note'] = parts[2] if len(parts) > 2 else ''
    elif parts[1] == '=':
        rest = parts[2].split(None, 1); e['decision'] = 'relabel'; e['label'] = rest[0]
        e['note'] = rest[1] if len(rest) > 1 else ''
    review[c['h']] = e
json.dump(review, open(path, 'w'), ensure_ascii=False, indent=1)
print(len(review), 'decisions')
