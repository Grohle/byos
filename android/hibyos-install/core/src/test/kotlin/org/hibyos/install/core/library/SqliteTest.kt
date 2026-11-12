package org.hibyos.install.core.library

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File

class SqliteTest {
    private fun mediaRow(i: Int, rnd: java.util.Random): List<Any?> {
        val long = rnd.nextInt(40) == 0
        val name = (if (long) "Very long title ".repeat(300) else "Title $i ") + listOf("a", "B", "ñ", "Ábc", "zz").random(kotlin.random.Random(i))
        return listOf(
            i.toLong(), SqlText("a:\\Artist ${i % 37}\\Album ${i % 11}\\$name.flac\u0000"), SqlText("$name\u0000"),
            SqlText("Album ${i % 11}\u0000"), SqlText(if (i % 2 == 0) "ARTIST ${i % 37}\u0000" else "artist ${i % 37}\u0000"),
            SqlText("Unknown\u0000"), 2000L + i % 25, 1L, (i % 20).toLong(), 0L, 0L, -1L, -1L, SqlText(name.take(1).uppercase()),
            rnd.nextInt(Int.MAX_VALUE).toLong() * 3, 44100L, 1703936L, 24L, 2L, 61868L, SqlText("2"), SqlText("\u0000"),
            SqlText("\u0000"), if (i % 3 == 0) null else -6.5 + i, null, 1764543060L + i, 1764030528L - i,
            SqlText(name.uppercase() + "\u0000"), SqlText("Artist ${i % 37}\u0000"),
        )
    }

    private fun build(f: File, n: Int): Map<String, List<SqlRow>> {
        val rnd = java.util.Random(42)
        val data = HashMap<String, List<SqlRow>>()
        data["MEDIA_TABLE"] = (1..n).map { SqlRow(it.toLong(), mediaRow(it, rnd)) }
        data["HISTORY_TABLE"] = (1..50).map { SqlRow((it * 3 + 100).toLong(), mediaRow(it, rnd)) }
        data["VERSION_TABLE"] = listOf(SqlRow(1, listOf(20181015L)))
        data["COUNT_TABLE"] = (1..5).map { SqlRow(it.toLong(), listOf((it * 10).toLong())) }
        data["CTIME_TABLE"] = (1..n).map { SqlRow(it.toLong(), listOf(((it * 7919) % n + 1).toLong())) }
        data["ALBUM_TABLE"] = (0 until 11).map { SqlRow((it + 1).toLong(), listOf(it.toLong(), SqlText("Album $it\u0000"), SqlText("A"), 3L, 1L, 2L, 0L, SqlText("ALBUM $it\u0000"))) }
        SqliteWriter(f, userVersion = 0).write(LibTestUtil.stockSchema()) { data[it] ?: emptyList() }
        return data
    }

    @Test
    fun recordRoundTrip() {
        val vals = listOf(null, 0L, 1L, -1L, 127L, 128L, -129L, 32768L, 8388608L, -8388609L, 2147483648L, 140737488355328L,
            Long.MIN_VALUE, Long.MAX_VALUE, 3.25, SqlText("x\u0000"), SqlText(""), byteArrayOf(1, 2, 3))
        val back = SqlRecord.decode(SqlRecord.encode(vals))
        assertEquals(vals.size, back.size)
        for (k in vals.indices) assertEquals(LibTestUtil.enc(vals[k]), LibTestUtil.enc(back[k]))
        for (v in listOf(0L, 127L, 128L, 16383L, 16384L, 0x00ffffffffffffffL, -1L, Long.MAX_VALUE)) {
            val b = Varint.bytes(v)
            assertEquals(v, Varint.read(b + ByteArray(9), 0).first)
        }
        // a header that needs a 2-byte size varint
        val many = List(200) { SqlText("v") }
        assertEquals(200, SqlRecord.decode(SqlRecord.encode(many)).size)
    }

    @Test
    fun nocaseCollation() {
        assertEquals(0, SqlRecord.compare(SqlText("ABC\u0000"), SqlText("abc\u0000"), "NOCASE"))
        assertTrue(SqlRecord.compare(SqlText("abc"), SqlText("ABD"), "NOCASE") < 0)
        assertTrue(SqlRecord.compare(SqlText("abc"), SqlText("ABD"), "BINARY") > 0)
        assertTrue(SqlRecord.compare(null, 0L) < 0)
        assertTrue(SqlRecord.compare(5L, SqlText("1")) < 0)
        assertTrue(SqlRecord.compare(2L, 1.5) > 0)
    }

    @Test
    fun schemaParsing() {
        val t = SchemaSql.parseTable("CREATE TABLE MEDIA_TABLE(id INT,path TEXT COLLATE NOCASE,name TEXT, ctime INT, PRIMARY KEY(id, path, cue_id))")
        assertEquals(4, t.columns.size)
        assertEquals("NOCASE", t.columns[1].collation)
        assertEquals(listOf("id", "path", "cue_id"), t.keyConstraints[0].map { it.column })
        val r = SchemaSql.parseTable("CREATE TABLE \"x y\"(a INTEGER PRIMARY KEY, b VARCHAR(10) UNIQUE, c)")
        assertEquals(0, r.rowidAlias)
        assertEquals("x y", r.name)
        assertEquals(1, r.keyConstraints.size)
        val i = SchemaSql.parseIndex("CREATE INDEX charater_index ON MEDIA_TABLE(character)")
        assertEquals("MEDIA_TABLE", i.table)
        assertEquals("character", i.columns[0].column)
    }

    @Test
    fun writeAndReadBack() {
        val dir = LibTestUtil.tempDir("rw")
        val f = File(dir, "lib.db")
        val data = build(f, 3000)
        SqliteReader(f).use { r ->
            assertEquals(4096, r.pageSize)
            val media = r.table("MEDIA_TABLE")
            assertEquals(3000, media.size)
            for ((k, row) in media.withIndex()) {
                val exp = data["MEDIA_TABLE"]!![k]
                assertEquals(exp.rowid, row.rowid)
                assertEquals(exp.values.map(LibTestUtil::enc), row.values.map(LibTestUtil::enc))
            }
            assertEquals(50, r.table("HISTORY_TABLE").size)
            // the automatic index of MEDIA_TABLE holds one key per row, in NOCASE order of (id, path, cue_id)
            val auto = r.schema().first { it.name == "sqlite_autoindex_MEDIA_TABLE_1" }
            val keys = r.indexKeys(auto.rootPage)
            assertEquals(3000, keys.size)
            for (k in 1 until keys.size) assertTrue(SqlRecord.compare(keys[k - 1][0], keys[k][0]) <= 0)
            val ch = r.schema().first { it.name == "charater_index" }
            val ck = r.indexKeys(ch.rootPage)
            assertEquals(3000, ck.size)
            for (k in 1 until ck.size) {
                val c = SqlRecord.compare(ck[k - 1][0], ck[k][0], "NOCASE")
                assertTrue(c < 0 || (c == 0 && (ck[k - 1][1] as Long) < (ck[k][1] as Long)))
            }
        }
    }

    @Test
    fun pythonAgreesWithWriterAndReader() {
        assumeTrue("python with sqlite3 not found", LibTestUtil.python != null)
        val dir = LibTestUtil.tempDir("py")
        val f = File(dir, "ours.db")
        build(f, 3000)
        assertEquals("ok", LibTestUtil.py(LibTestUtil.INTEGRITY, f.path).trim())
        assertEquals(LibTestUtil.dump(f), LibTestUtil.py(LibTestUtil.DUMP, f.path).replace("\r\n", "\n").trim())
        // a database written by SQLite itself, with deletes (free pages) and big rows, read by our reader
        val g = File(dir, "theirs.db")
        LibTestUtil.py("""
import sqlite3, sys
c = sqlite3.connect(sys.argv[1])
c.execute('create table t(id INT, path TEXT COLLATE NOCASE, v REAL, PRIMARY KEY(id, path))')
c.execute('create index ti on t(path)')
for i in range(5000):
    c.execute('insert into t values (?,?,?)', (i, ('p%05d' % i) * (1 + (i % 97 == 0) * 400) + '\x00', i / 3.0))
c.execute('delete from t where id % 7 = 0')
c.commit()
""", g.path)
        val theirs = LibTestUtil.py(LibTestUtil.DUMP, g.path).replace("\r\n", "\n").trim()
        assertEquals(theirs, LibTestUtil.dump(g))
        // copy it with our writer and let SQLite check the copy
        val h = File(dir, "copy.db")
        SqliteReader(g).use { r ->
            val schema = r.schema()
            SqliteWriter(h).write(schema) { n -> r.table(n) }
        }
        assertEquals("ok", LibTestUtil.py(LibTestUtil.INTEGRITY, h.path).trim())
        assertEquals(theirs, LibTestUtil.py(LibTestUtil.DUMP, h.path).replace("\r\n", "\n").trim())
    }

    /** Optional: HIBYOS_LIBRARY_DB names a real usrlocal_media.db; it is copied and checked. */
    @Test
    fun realDatabaseCopy() {
        val src = System.getenv("HIBYOS_LIBRARY_DB")
        assumeTrue("HIBYOS_LIBRARY_DB not set", !src.isNullOrEmpty())
        val out = File(LibTestUtil.tempDir("real"), "copy.db")
        SqliteReader(File(src!!)).use { r -> SqliteWriter(out, r.pageSize, r.userVersion, r.applicationId).write(r.schema()) { r.table(it) } }
        assumeTrue(LibTestUtil.python != null)
        assertEquals("ok", LibTestUtil.py(LibTestUtil.INTEGRITY, out.path).trim())
        assertEquals(LibTestUtil.py(LibTestUtil.DUMP, src).trim(), LibTestUtil.py(LibTestUtil.DUMP, out.path).trim())
    }
}
