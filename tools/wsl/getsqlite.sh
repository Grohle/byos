set -e
. "$(dirname "$0")/fetch.sh"
cd ~/hiby/third
if [ ! -f sqlite3.c ]; then
  # SQLite 3.50.4 amalgamation, pinned by SHA-256
  fetch_verify https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip 1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032 sq.zip
  python3 -c "import zipfile;z=zipfile.ZipFile('sq.zip');[open(n.split('/')[-1],'wb').write(z.read(n)) for n in z.namelist() if n.endswith(('.c','.h'))]"
fi
# the two files that are compiled in must be the ones of SQLite 3.50.4, also when they were already there
verify_sha256 sqlite3.c e3f5d6901e7492af4a1fc8c4d745cae84c264942524c3fbfc02b82a5ca8818c8
verify_sha256 sqlite3.h abd1514e0351f79393d1be882830afdb40a8099e8257f311f0bfdf8486f11bea
ls -la sqlite3.c sqlite3.h
grep -m1 '#define SQLITE_VERSION ' sqlite3.h
