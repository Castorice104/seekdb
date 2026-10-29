#!/usr/bin/env python3
"""Compare full token outputs and MATCH rows/scores across two server binaries."""
import argparse, concurrent.futures, hashlib, json, random
from pathlib import Path
import pymysql

p=argparse.ArgumentParser();p.add_argument('--port',type=int,default=2885)
p.add_argument('--snapshot',type=Path,required=True);p.add_argument('--compare',type=Path)
a=p.parse_args()
def connect():
    return pymysql.connect(host='127.0.0.1',port=a.port,user='root',charset='utf8mb4',autocommit=True)
conn=connect();cur=conn.cursor();cur.execute('SET ob_query_timeout=100000000')
rng=random.Random(20261028)
words=['数据库','全文索引','稳定','OceanBase','Tokenizer','DATABASE','performance','audit',
       'abc-def','a@b.com','１２３．４５公斤','ＡＢＣ','Café','ÄÖÜ','かな','한글','😀','𠀀','a','the','', '123.45']
corpus=['Résumé resume RESUME naïve naive café cafe', '',None,'a A a ALPHA alpha beta THE the','   \t\r\n!.,','😀𠀀',
        'Q'*10000+' HELLO '+'数据'*1600,'１２３．４５公斤 ＡＢＣ abc-def a@b.com',
        '南京市长江大桥 研究生命起源 二十三公里']
corpus += [' '.join(rng.choices(words,k=rng.randint(1,45))) for _ in range(96)]
configs=[('ik',None),('ik','[{"output":"all"}]'),
         ('ik','[{"output":"all"},{"additional_args":[{"ik_mode":"max_word"}]}]'),
         ('beng',None),('beng','[{"output":"all"}]'),('space','[{"output":"all"}]'),('ngram',None)]
result={}
def run(key,sql,args=None):
    try:
        if sql.startswith('SELECT tokenize('): sql += ' AS tokens'
        cur.execute(sql,args);result[key]=cur.fetchall()
    except pymysql.MySQLError as ex:result[key]={'error_code':ex.args[0]}
for i,text in enumerate(corpus):
    for j,(parser,options) in enumerate(configs):
        if options is None:run(f'token/{i}/{j}','SELECT tokenize(%s,%s)',(text,parser))
        else:run(f'token/{i}/{j}','SELECT tokenize(%s,%s,%s)',(text,parser,options))
for charset in ['utf8mb4','utf16','utf16le']:
    run('charset/'+charset,"SELECT tokenize(CONVERT(%s USING "+charset+"),'ik')",('ＡＢＣ 数据库 123 かな😀',))
for invalid in ['F0288CBC','E4B8','FF','EDA080']:
    run('invalid/'+invalid,"SELECT tokenize(CONVERT(UNHEX(%s) USING utf8mb4),'ik')",(invalid,))
cur.execute('DROP DATABASE IF EXISTS fts_hot_path_differential')
cur.execute('CREATE DATABASE fts_hot_path_differential');cur.execute('USE fts_hot_path_differential')
queries=['数据库','全文索引 分词器','database performance audit','OceanBase 稳定 database','+database -audit','"database performance"','abc-def','１２３']
for parser in ['ik','beng','space','ngram']:
    table='docs_'+parser
    cur.execute(f'CREATE TABLE {table}(id INT PRIMARY KEY,content TEXT)')
    docs=[(i,text) for i,text in enumerate(corpus) if text is not None]
    cur.executemany(f'INSERT INTO {table} VALUES(%s,%s)',docs)
    cur.execute(f'ALTER TABLE {table} ADD FULLTEXT INDEX f(content) WITH PARSER {parser}')
    def snapshot(stage):
        for i,query in enumerate(queries):
            for mode in ['NATURAL LANGUAGE MODE','BOOLEAN MODE']:
                run(f'{table}/{stage}/{i}/{mode}',f'SELECT id,ROUND(MATCH(content) AGAINST(%s IN {mode}),10) FROM {table} WHERE MATCH(content) AGAINST(%s IN {mode}) ORDER BY id',(query,query))
        run(f'{table}/{stage}/limit',f'SELECT id FROM {table} WHERE MATCH(content) AGAINST(%s) ORDER BY id LIMIT 7',('database 数据库',))
    snapshot('initial')
    cur.execute('BEGIN');cur.execute(f'UPDATE {table} SET content=%s WHERE id<8',('database performance audit 数据库 全文索引',));snapshot('updated')
    cur.execute(f'DELETE FROM {table} WHERE id<15');snapshot('deleted');cur.execute('ROLLBACK');snapshot('rollback')
    cur.execute(f'UPDATE {table} SET content=%s WHERE id<8',('database performance audit 数据库 全文索引',));snapshot('committed')
# Independent sessions exercise the local scratch paths concurrently.
def concurrent_case(i):
    c=connect()
    try:
        with c.cursor() as x:
            x.execute('SELECT tokenize(%s,%s,%s) AS tokens',('Alpha ALPHA 数据库 123.45 '*50,'ik' if i%2 else 'beng','[{"output":"all"}]'))
            return x.fetchall()
    finally:c.close()
with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
    for i,value in enumerate(pool.map(concurrent_case,range(36))):result[f'concurrent/{i}']=value
conn.close()
encoded=json.dumps(result,ensure_ascii=False,sort_keys=True,default=str,indent=2)
a.snapshot.write_text(encoded)
if a.compare:
    before=json.loads(a.compare.read_text());after=json.loads(encoded)
    different=[k for k in before.keys()|after.keys() if before.get(k)!=after.get(k)]
    if different:raise SystemExit('DIFFERENTIAL FAILED: '+str(different[:20]))
print('PASS',len(result),'full token/query/transaction/concurrent results; sha256='+hashlib.sha256(encoded.encode()).hexdigest())
