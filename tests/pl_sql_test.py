# Reproduce en el PC las sentencias de pl_build (hibymod_disp.c v6) sobre una COPIA de la base.
# Uso (WSL): python3 -I tests/pl_sql_test.py ~/hiby/big.db /tmp/copia.db
import sqlite3, shutil, sys, time, re
src, dst = sys.argv[1], sys.argv[2]
shutil.copy(src, dst)
c = sqlite3.connect(dst, isolation_level=None)
code = open('/mnt/c/Users/alber/Downloads/Hiby/hibymod/src/hibymod_disp.c', encoding='utf-8').read()
def cdef(name):
    m = re.search(r'#define %s ((?:"[^"]*"\s*\\?\s*)+)' % name, code)
    return ''.join(re.findall(r'"([^"]*)"', m.group(1)))
COLS, CREATE = cdef('PL_COLS'), cdef('PL_CREATE')
ins = re.search(r'"INSERT OR IGNORE INTO list_tb_%d\(" PL_COLS "\) SELECT "(.*?)tb, unscanned', code, re.S).group(1)
ins = 'INSERT OR IGNORE INTO list_tb_%d(' + COLS + ') SELECT ' + ''.join(re.findall(r'"([^"]*)"', ins))
paths = [r[0] for r in c.execute("select path from MEDIA_TABLE where album like 'Mamma Mia%' order by rowid desc limit 6")]
paths.insert(2, 'a:\\no\\existe.flac\x00')
paths.append(paths[0])                         # duplicado
want = 3
def run(tb, lpath, unscanned):
    t = time.time()
    c.execute('BEGIN')
    c.execute('CREATE TEMP TABLE IF NOT EXISTS hm_req(pos INTEGER PRIMARY KEY, path TEXT COLLATE NOCASE, cue INT)')
    c.execute('DELETE FROM hm_req')
    for i, p in enumerate(paths): c.execute('INSERT INTO hm_req(pos, path, cue) VALUES (%d, ?1, %d)' % (i, -1), (p,))
    c.execute('DROP TABLE IF EXISTS list_tb_%d' % tb)
    c.execute('CREATE TABLE IF NOT EXISTS list_tb_%d' % tb + CREATE)
    q = ins % (tb, 1 if unscanned else 0)
    for r in c.execute('EXPLAIN QUERY PLAN ' + q.split(') SELECT ', 1)[1].join(['SELECT ', ''])): print('  plan:', r)
    c.execute(q)
    rows = c.execute('SELECT count(*) FROM list_tb_%d' % tb).fetchone()[0]
    idx = c.execute('SELECT count(*) FROM list_tb_%d WHERE rowid < (SELECT min(rowid) FROM list_tb_%d WHERE path = ?1)' % (tb, tb), (paths[want],)).fetchone()[0]
    c.execute('DELETE FROM MANAGER_TABLE WHERE id = ?3 OR path = ?1', (lpath, None, tb))
    c.execute('INSERT INTO MANAGER_TABLE(id, path, count, music_count, dir_count, m3u_count) VALUES (?3, ?1, %d, %d, 0, 0)' % (rows, rows), (lpath, None, tb))
    c.execute('COMMIT')
    print('tb', tb, 'unscanned', unscanned, 'rows', rows, 'index', idx, 'ms', int((time.time() - t) * 1000))
    for r in c.execute('SELECT rowid, id, name, cue_id, has_child_file FROM list_tb_%d' % tb): print('   ', r)
    # lo que leeria el reproductor (0x71ecd4)
    print('   leer[idx]:', c.execute('SELECT name FROM list_tb_%d limit ? offset ?' % tb, (1, idx)).fetchone())
run(7701, 'hibymod\\q1\x00', 0)
run(7702, 'hibymod\\q2\x00', 1)
print(c.execute('SELECT * FROM MANAGER_TABLE').fetchall())
print('integrity', c.execute('pragma integrity_check').fetchone())
# rendimiento: 1000 pistas
paths = [r[0] for r in c.execute('select path from MEDIA_TABLE order by random() limit 1000')]
want = 500

t = time.time()
c.execute('BEGIN'); c.execute('DELETE FROM hm_req')
for i, p in enumerate(paths): c.execute('INSERT INTO hm_req(pos, path, cue) VALUES (%d, ?1, -1)' % i, (p,))
c.execute('DROP TABLE IF EXISTS list_tb_7701'); c.execute('CREATE TABLE list_tb_7701' + CREATE); c.execute(ins % (7701, 0)); c.execute('COMMIT')
print('1000 pistas: rows', c.execute('select count(*) from list_tb_7701').fetchone()[0], 'ms en PC', int((time.time() - t) * 1000))
