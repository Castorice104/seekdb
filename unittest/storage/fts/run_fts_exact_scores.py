#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Compare complete binary-protocol DOUBLE scores and ordered IDs on a private fixture."""
import argparse
import hashlib
import json
from pathlib import Path
import pymysql
from fts_binary_protocol import BinaryScoreClient

p = argparse.ArgumentParser()
p.add_argument('--port', type=int, default=2885)
p.add_argument('--snapshot', type=Path, required=True)
p.add_argument('--compare', type=Path)
a = p.parse_args()
a.snapshot.parent.mkdir(parents=True, exist_ok=True)
connection = pymysql.connect(host='127.0.0.1', port=a.port, user='root', charset='utf8mb4', autocommit=True, read_timeout=30, write_timeout=30)
x = connection.cursor()
x.execute('SET ob_query_timeout=100000000')
x.execute('DROP DATABASE IF EXISTS fts_exact_scores_regression')
x.execute('CREATE DATABASE fts_exact_scores_regression')
x.execute('USE fts_exact_scores_regression')
documents = [(0, 0, 'alpha database performance 数据库 全文索引'),
             (1, 1, 'alpha database performance 数据库 全文索引'),
             (2, 2, 'audit alpha alpha 数据库'), (3, 3, 'beta audit'),
             (4, 0, ''), (5, 1, None)]
for i in range(6, 70):
    documents.append((i, i % 4, ('alpha ' * (i % 7 + 1)) +
                      ('database performance 数据库 ' if i % 3 else 'audit beta 全文索引 ')))
long_terms = ' '.join('term' + str(i).zfill(4) for i in range(400))
documents[0] = (0, 0, documents[0][2] + ' ' + long_terms)
lines = ['!SET ob_query_timeout=100000000', '!USE fts_exact_scores_regression', 'protocol_sanity\tSELECT 1,1.25e0']
for parser in ['ik', 'beng']:
    table = 'docs_' + parser
    x.execute(f'CREATE TABLE {table}(id BIGINT PRIMARY KEY, category INT, content TEXT, FULLTEXT INDEX f(content) WITH PARSER {parser})')
    x.executemany(f'INSERT INTO {table} VALUES(%s,%s,%s)', documents)
    # An independent small, known corpus checks exact ID membership as well as A/B equivalence.
    x.execute(f'SELECT id FROM {table} WHERE MATCH(content) AGAINST(\'alpha\') ORDER BY id')
    ids = [row[0] for row in x.fetchall()]
    expected = [0, 1, 2] + list(range(6, 70))
    if ids != expected:
        raise AssertionError((parser, ids, expected))
    for rowset in [0, 64]:
        hint = f"/*+ OPT_PARAM('rowsets_max_rows',{rowset}) */"
        prefix = f'{parser}/{rowset}/'
        match = "MATCH(content) AGAINST('database 数据库')"
        shapes = {
            'project': f'SELECT {hint} id,{match} FROM {table} WHERE {match} ORDER BY id',
            'numeric': f'SELECT {hint} id,{match} FROM {table} WHERE {match}>0.001 ORDER BY id',
            'residual': f'SELECT {hint} id,{match} FROM {table} WHERE {match} AND category=1 ORDER BY id',
            'reuse': f'SELECT {hint} id,{match} FROM {table} WHERE {match} AND (NOT {match} OR category=1) ORDER BY id',
            'topk': f'SELECT {hint} id,{match} s FROM {table} WHERE {match} ORDER BY s DESC,id LIMIT 13',
            'offset': f'SELECT {hint} id,{match} s FROM {table} WHERE {match} ORDER BY s DESC,id LIMIT 5 OFFSET 3',
            'implicit_limit_ids': f'SELECT {hint} id,0e0 FROM {table} WHERE {match} LIMIT 20',
            'derived_limit_ids': f'SELECT {hint} id,0e0 FROM (SELECT id FROM {table} WHERE {match} LIMIT 20)t',
            'aggregate': f'SELECT {hint} category,SUM({match}) s FROM {table} WHERE {match} GROUP BY category ORDER BY category',
            'boolean': f"SELECT {hint} id,MATCH(content) AGAINST('+alpha -audit' IN BOOLEAN MODE) s FROM {table} WHERE MATCH(content) AGAINST('+alpha -audit' IN BOOLEAN MODE) ORDER BY id",
            'many_terms': f"SELECT {hint} id,MATCH(content) AGAINST('{long_terms}') FROM {table} WHERE MATCH(content) AGAINST('{long_terms}') ORDER BY id",
        }
        for name, sql in shapes.items():
            lines.append(prefix + name + '\t' + sql)
        # Consecutive identical SQL strings deliberately reuse the same prepared statement with changed parameters.
        sql = f'SELECT {hint} id,MATCH(content) AGAINST(?) s FROM {table} WHERE MATCH(content) AGAINST(?) ORDER BY id'
        for index, query in enumerate(['alpha', 'database 数据库', 'no_such_term_987654321', 'audit', 'alpha', long_terms]):
            lines.append(prefix + 'prepared/' + str(index) + '\t' + sql + '\t' + query)
queries = a.snapshot.with_suffix('.queries.tsv')
queries.write_text('\n'.join(lines) + '\n')
binary = BinaryScoreClient(connection)
output = []
try:
    for line in lines:
        if line.startswith('!'):
            binary.close()
            x.execute(line[1:])
            continue
        parts = line.split('\t', 2)
        label, sql = parts[:2]
        rows = binary.execute(sql, parts[2] if len(parts) == 3 else None)
        output.append(label + ''.join('\t' + str(id_) + ':' + bits for id_, bits in rows))
finally:
    binary.close()
    connection.close()
a.snapshot.write_text('\n'.join(output) + '\n')
print('PASS', len(output), 'unrounded binary-protocol observations')
result = a.snapshot.read_bytes()
if a.compare and result != a.compare.read_bytes():
    before = dict(line.split(b'\t', 1) if b'\t' in line else (line, b'') for line in a.compare.read_bytes().splitlines())
    after = dict(line.split(b'\t', 1) if b'\t' in line else (line, b'') for line in result.splitlines())
    different = [key.decode() for key in before.keys() | after.keys() if before.get(key) != after.get(key)]
    raise SystemExit('EXACT SCORE/ORDER MISMATCH: ' + str(different[:20]))
print('PASS exact binary scores and ordered IDs; sha256=' + hashlib.sha256(result).hexdigest())
