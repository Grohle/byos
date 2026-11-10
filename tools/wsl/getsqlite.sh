set -e
cd ~/hiby/third
if [ ! -f sqlite3.c ]; then
  for v in 2025/sqlite-amalgamation-3500400 2025/sqlite-amalgamation-3490100 2024/sqlite-amalgamation-3470200; do
    if curl -sSfL -o sq.zip "https://www.sqlite.org/$v.zip"; then echo "ok $v"; break; fi
  done
  python3 -c "import zipfile;z=zipfile.ZipFile('sq.zip');[open(n.split('/')[-1],'wb').write(z.read(n)) for n in z.namelist() if n.endswith(('.c','.h'))]"
fi
ls -la sqlite3.c sqlite3.h
grep -m1 '#define SQLITE_VERSION ' sqlite3.h
