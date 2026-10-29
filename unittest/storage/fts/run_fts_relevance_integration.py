#!/usr/bin/env python3
"""Relevance elision must preserve full SQL results and keep numeric score consumers."""
import argparse, json
from pathlib import Path
import pymysql
p=argparse.ArgumentParser();p.add_argument('--port',type=int,default=2885)
p.add_argument('--snapshot',type=Path,required=True);p.add_argument('--compare',type=Path)
p.add_argument('--require-fastpath',action='store_true')
p.add_argument('--rowset-size',type=int,choices=[0,64]);a=p.parse_args()
c=pymysql.connect(host='127.0.0.1',port=a.port,user='root',charset='utf8mb4',autocommit=True)
x=c.cursor();x.execute('SET ob_query_timeout=100000000')
x.execute('DROP DATABASE IF EXISTS fts_relevance_regression');x.execute('CREATE DATABASE fts_relevance_regression');x.execute('USE fts_relevance_regression')
results={};plans={}
def hinted(sql):
    if a.rowset_size is not None:
        sql=sql.replace('SELECT ',"SELECT /*+ OPT_PARAM('rowsets_max_rows',"+str(a.rowset_size)+") */ ",1)
    return sql
def query(key,sql,args=None):
    try:
        x.execute(hinted(sql),args);results[key]=x.fetchall()
    except pymysql.MySQLError as ex:
        results[key]={'error_code':ex.args[0]}
        print('BASELINE/RESULT ERROR',key,ex.args[0])
for parser in ['ik','beng']:
    x.execute(f'CREATE TABLE docs_{parser}(id INT PRIMARY KEY, category INT, content TEXT, FULLTEXT INDEX f(content) WITH PARSER {parser})')
    long_query=' '.join('term'+str(i).zfill(4) for i in range(400))
    texts=['database performance audit 数据库 全文索引','database database 数据库','performance audit',None,'','a the','数据库 支持全文检索','unrelated document','audit database','database performance performance']
    texts[0] += ' '+long_query
    x.executemany(f'INSERT INTO docs_{parser} VALUES(%s,%s,%s)',[(i,i%3,text) for i,text in enumerate(texts)])
    table='docs_'+parser;match="MATCH(content) AGAINST('database 数据库')"
    shapes={
      'count':f'SELECT COUNT(*) FROM {table} WHERE {match}',
      'ids':f'SELECT id FROM {table} WHERE {match} ORDER BY id',
      'residual_reuse':f'SELECT id FROM {table} WHERE {match} AND (NOT {match} OR category=1) ORDER BY id',
      'residual_or':f'SELECT id FROM {table} WHERE {match} AND ({match} OR category=1) ORDER BY id',
      'conjunction':f'SELECT id FROM {table} WHERE {match} AND category=1 ORDER BY id',
      'or':f'SELECT id FROM {table} WHERE {match} OR category=1 ORDER BY id',
      'not':f'SELECT id FROM {table} WHERE NOT {match} ORDER BY id',
      'project':f'SELECT id,ROUND({match},12) FROM {table} WHERE {match} ORDER BY id',
      'numeric':f'SELECT id FROM {table} WHERE {match}>0.07 ORDER BY id',
      'arithmetic':f'SELECT id FROM {table} WHERE {match}+0.1>0.2 ORDER BY id',
      'score_order':f'SELECT id,ROUND({match},12) s FROM {table} WHERE {match} ORDER BY s DESC,id LIMIT 4',
      'score_aggregate':f'SELECT category,ROUND(SUM({match}),12) FROM {table} WHERE {match} GROUP BY category ORDER BY category',
      'having':f'SELECT id,ROUND({match},12) s FROM {table} HAVING s>0.01 ORDER BY id',
      'distinct':f'SELECT DISTINCT category FROM {table} WHERE {match} ORDER BY category',
      'derived':f'SELECT COUNT(*) FROM (SELECT id FROM {table} WHERE {match} LIMIT 3) t',
      'correlated':f'SELECT id FROM {table} d WHERE EXISTS(SELECT 1 FROM {table} e WHERE e.id=d.id AND MATCH(d.content) AGAINST(\'database 数据库\')) ORDER BY id',
      'join':f'SELECT d.id,e.id FROM {table} d LEFT JOIN {table} e ON e.id=d.id WHERE MATCH(d.content) AGAINST(\'database 数据库\') ORDER BY d.id,e.id',
      'boolean':f'SELECT id,ROUND(MATCH(content) AGAINST(\'+database -audit\' IN BOOLEAN MODE),12) FROM {table} WHERE MATCH(content) AGAINST(\'+database -audit\' IN BOOLEAN MODE) ORDER BY id'}
    for key,sql in shapes.items():
        query(parser+'/'+key,sql)
        if key in ['count','project','numeric','score_order','score_aggregate','boolean'] and not isinstance(results[parser+'/'+key],dict):
            x.execute('EXPLAIN EXTENDED '+hinted(sql));plans[parser+'/'+key]='\n'.join(str(row[0]) for row in x.fetchall())
    query(parser+'/long',f'SELECT COUNT(*) FROM {table} WHERE MATCH(content) AGAINST(%s)',(long_query,))
    query(parser+'/long_score',f'SELECT id,ROUND(MATCH(content) AGAINST(%s),12) FROM {table} WHERE MATCH(content) AGAINST(%s) ORDER BY id',(long_query,long_query))
    for threshold in [0,0.07,1,0.00001]:
        query(parser+'/threshold/'+str(threshold),f'SELECT id FROM {table} WHERE {match}>%s ORDER BY id',(threshold,))
    x.execute('BEGIN');x.execute(f'UPDATE {table} SET content=\'database audit 数据库\' WHERE id IN (3,4,7)')
    query(parser+'/update',shapes['ids']);x.execute(f'DELETE FROM {table} WHERE id<3');query(parser+'/delete',shapes['ids'])
    x.execute('ROLLBACK');query(parser+'/rollback',shapes['ids'])
    for term in ['', 'the', 'audit', 'database', '数据库', 'database audit', 'notpresent']:
        query(parser+'/term/'+term,f'SELECT id FROM {table} WHERE MATCH(content) AGAINST(%s) ORDER BY id',(term,))
c.close()
encoded=json.dumps(results,ensure_ascii=False,sort_keys=True,default=str,indent=2);a.snapshot.write_text(encoded)
a.snapshot.with_suffix('.plans.json').write_text(json.dumps(plans,ensure_ascii=False,indent=2))
if a.compare:
    b=json.loads(a.compare.read_text());v=json.loads(encoded);different=[k for k in b.keys()|v.keys() if b.get(k)!=v.get(k)]
    assert not different,'SQL result changed: '+str(different)
if a.require_fastpath:
    for parser in ['ik','beng']:
        count_plan=plans[parser+'/count']
        assert 'calc_relevance=false' in count_plan,'Missing predicate-only fast plan for '+parser
        if a.rowset_size is not None:
            assert "OPT_PARAM('ROWSETS_MAX_ROWS' "+str(a.rowset_size)+')' in count_plan,'Rowset hint not used'
            assert ('rowset='+str(a.rowset_size) in count_plan) == (a.rowset_size>0),'Wrong execution mode'
        for key in ['project','numeric','score_order','score_aggregate','boolean']:
            plan=plans[parser+'/'+key]
            assert 'calc_relevance=false' not in plan and ('calc_relevance=true' in plan or 'has_functional_lookup=true' in plan),'Lost numeric score plan: '+parser+'/'+key
print('PASS',len(results),'full SQL results; plan checks',a.require_fastpath)
