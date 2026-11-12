package org.hibyos.install.core.library

import java.io.File

internal object LibTestUtil {
    /** The stock R3 II 2025 schema (tables and indexes, no rows) in its sqlite_master order. */
    fun stockSchema(): List<SchemaEntry> {
        val text = LibTestUtil::class.java.getResource("/library/r3ii_2025_schema.sql")!!.readText()
        val lines = text.lines().filter { it.isNotEmpty() }
        val out = ArrayList<SchemaEntry>()
        var i = 0
        while (i < lines.size) {
            val h = lines[i].removePrefix("-- ").split(' ')
            val sql = lines[i + 1].removeSuffix(";").ifEmpty { null }
            out += SchemaEntry(h[0], h[1], h[2], 1, sql)
            i += 2
        }
        return out
    }

    fun tempDir(name: String): File {
        val d = File(System.getProperty("java.io.tmpdir"), "hibyos-library-test/$name")
        d.deleteRecursively()
        d.mkdirs()
        return d
    }

    /** A working Python 3 with sqlite3, or null (those checks are then skipped). */
    val python: String? by lazy {
        val cands = listOfNotNull(System.getenv("HIBYOS_PYTHON"), "python", "python3", "py")
        cands.firstOrNull { p ->
            try {
                val pr = ProcessBuilder(p, "-c", "import sqlite3,sys;sys.stdout.write('ok')").redirectErrorStream(true).start()
                val o = pr.inputStream.bufferedReader().readText()
                pr.waitFor() == 0 && o.trim() == "ok"
            } catch (e: Exception) {
                false
            }
        }
    }

    /** Runs a Python script with arguments and returns stdout (fails on a non-zero exit). */
    fun py(script: String, vararg args: String): String {
        val f = File.createTempFile("hibyos-lib", ".py")
        try {
            f.writeText(script)
            val pr = ProcessBuilder(listOf(python!!, "-I", f.path) + args).redirectErrorStream(true).start()
            val o = pr.inputStream.bufferedReader(Charsets.UTF_8).readText()
            if (pr.waitFor() != 0) throw AssertionError("python failed:\n$o")
            return o
        } finally {
            f.delete()
        }
    }

    const val INTEGRITY = """
import sqlite3, sys
c = sqlite3.connect('file:' + sys.argv[1] + '?mode=ro', uri=True)
r = [x[0] for x in c.execute('pragma integrity_check')]
sys.stdout.write('|'.join(r))
"""

    /** Dumps every table as lines "table|rowid|v1|v2..." with typed values. */
    const val DUMP = """
import sqlite3, sys
c = sqlite3.connect('file:' + sys.argv[1] + '?mode=ro', uri=True)
c.text_factory = bytes
def enc(v):
    if v is None: return 'N'
    if isinstance(v, int): return 'I%d' % v
    if isinstance(v, float): return 'R' + repr(v)
    if isinstance(v, bytes): return 'T' + v.hex()
    return '?'
names = [r[0].decode() for r in c.execute("select name from sqlite_master where type='table' order by name")]
out = []
for n in names:
    for row in c.execute('select rowid, * from "%s" order by rowid' % n):
        out.append(n + '|' + str(row[0]) + '|' + '|'.join(enc(v) for v in row[1:]))
sys.stdout.write('\n'.join(out))
"""

    fun enc(v: Any?): String = when (v) {
        null -> "N"
        is Long -> "I$v"
        is Double -> "R" + (if (v == Math.floor(v) && !v.isInfinite() && Math.abs(v) < 1e16) String.format(java.util.Locale.ROOT, "%.1f", v) else v.toString())
        is SqlText -> "T" + v.bytes.joinToString("") { "%02x".format(it) }
        is ByteArray -> "B" + v.joinToString("") { "%02x".format(it) }
        else -> "?"
    }

    /** Our reader's version of [DUMP] (blobs never appear in these tests). */
    fun dump(f: File): String = SqliteReader(f).use { r ->
        val names = r.schema().filter { it.type == "table" }.map { it.name }.sortedBy { it }
        val out = ArrayList<String>()
        for (n in names) {
            val schema = SchemaSql.parseTable(r.schema().first { it.name == n }.sql!!)
            for (row in r.table(n)) {
                // SQLite stores whole REAL values as integers and converts them back when read
                val vals = schema.columns.indices.map {
                    val v = row.values.getOrNull(it)
                    if (v is Long && schema.columns[it].affinity == "REAL") v.toDouble() else v
                }
                out += n + "|" + row.rowid + "|" + vals.joinToString("|") { enc(it) }
            }
        }
        out.joinToString("\n")
    }
}
