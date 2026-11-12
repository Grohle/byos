package org.hibyos.install.core.library

import java.io.Closeable
import java.io.File
import java.io.IOException
import java.io.RandomAccessFile

/**
 * A small reader and writer for the SQLite 3 file format (https://www.sqlite.org/fileformat2.html),
 * enough to copy the stock player's library database and write a new one. The core module has
 * no dependencies and the JVM has no SQLite, so this works on the bytes directly.
 *
 * Supported: UTF-8 databases, rowid tables, indexes on plain columns (BINARY, NOCASE, RTRIM,
 * ASC/DESC), automatic PRIMARY KEY / UNIQUE indexes, overflow pages. Not supported (refused):
 * UTF-16, WITHOUT ROWID tables, partial or expression indexes. The writer always produces a
 * fresh, compact file without free pages.
 */
class SqliteException(message: String) : IOException(message)

/** A TEXT value, kept as its exact UTF-8 bytes (the player stores a trailing NUL inside them). */
class SqlText(val bytes: ByteArray) {
    constructor(s: String) : this(s.toByteArray(Charsets.UTF_8))
    val text: String get() = String(bytes, Charsets.UTF_8)
    override fun equals(other: Any?) = other is SqlText && other.bytes.contentEquals(bytes)
    override fun hashCode() = bytes.contentHashCode()
    override fun toString() = "'" + text.replace("\u0000", "\\0") + "'"
}

/** A row: its rowid and its values (null, Long, Double, SqlText or ByteArray for BLOB). */
class SqlRow(val rowid: Long, val values: List<Any?>)

class SchemaEntry(val type: String, val name: String, val tblName: String, val rootPage: Int, val sql: String?)

internal object Varint {
    fun read(b: ByteArray, p: Int): Pair<Long, Int> {
        var v = 0L
        for (i in 0 until 8) {
            val x = b[p + i].toInt() and 0xff
            v = (v shl 7) or (x and 0x7f).toLong()
            if (x and 0x80 == 0) return v to (i + 1)
        }
        v = (v shl 8) or (b[p + 8].toLong() and 0xff)
        return v to 9
    }

    fun len(v: Long): Int {
        if (v < 0 || v > 0x00ffffffffffffffL) return 9
        var n = 1
        var x = v ushr 7
        while (x != 0L) { n++; x = x ushr 7 }
        return n
    }

    fun write(out: ByteArray, p: Int, v: Long): Int {
        val n = len(v)
        if (n == 9) {
            out[p + 8] = v.toByte()
            var x = v ushr 8
            for (i in 7 downTo 0) { out[p + i] = ((x and 0x7f) or 0x80).toByte(); x = x ushr 7 }
            return 9
        }
        var x = v
        for (i in n - 1 downTo 0) {
            out[p + i] = ((x and 0x7f) or (if (i == n - 1) 0 else 0x80).toLong()).toByte()
            x = x ushr 7
        }
        return n
    }

    fun bytes(v: Long): ByteArray { val b = ByteArray(len(v)); write(b, 0, v); return b }
}

/** Record format (serial types) and value comparison with collations. */
object SqlRecord {
    private fun serialType(v: Any?): Long = when (v) {
        null -> 0
        is Long -> when {
            v == 0L -> 8
            v == 1L -> 9
            v in -128..127 -> 1
            v in -32768..32767 -> 2
            v in -8388608..8388607 -> 3
            v in Int.MIN_VALUE..Int.MAX_VALUE -> 4
            v in -140737488355328L..140737488355327L -> 5
            else -> 6
        }
        is Int -> serialType(v.toLong())
        is Double -> 7
        is SqlText -> 13L + 2 * v.bytes.size
        is ByteArray -> 12L + 2 * v.size
        is String -> 13L + 2 * v.toByteArray(Charsets.UTF_8).size
        else -> throw IllegalArgumentException("unsupported SQL value ${v.javaClass}")
    }

    private fun contentLen(t: Long): Int = when {
        t <= 0L -> 0
        t in 1..4 -> t.toInt()
        t == 5L -> 6
        t == 6L || t == 7L -> 8
        t == 8L || t == 9L -> 0
        t >= 12 -> ((t - 12) / 2).toInt()
        else -> throw SqliteException("reserved serial type $t")
    }

    fun encode(values: List<Any?>): ByteArray {
        val types = values.map { serialType(it) }
        val body = types.sumOf { Varint.len(it) }
        var hdr = body + 1 // the header size includes its own varint
        while (body + Varint.len(hdr.toLong()) != hdr) hdr = body + Varint.len(hdr.toLong())
        val total = hdr + types.sumOf { contentLen(it) }
        val out = ByteArray(total)
        var p = Varint.write(out, 0, hdr.toLong())
        for (t in types) p += Varint.write(out, p, t)
        check(p == hdr)
        for ((i, v) in values.withIndex()) {
            val t = types[i]
            when (v) {
                null -> {}
                is Long, is Int -> {
                    val x = (v as Number).toLong()
                    val n = contentLen(t)
                    for (k in 0 until n) out[p + k] = (x ushr (8 * (n - 1 - k))).toByte()
                    p += n
                }
                is Double -> {
                    val x = java.lang.Double.doubleToRawLongBits(v)
                    for (k in 0 until 8) out[p + k] = (x ushr (8 * (7 - k))).toByte()
                    p += 8
                }
                is SqlText -> { System.arraycopy(v.bytes, 0, out, p, v.bytes.size); p += v.bytes.size }
                is String -> { val b = v.toByteArray(Charsets.UTF_8); System.arraycopy(b, 0, out, p, b.size); p += b.size }
                is ByteArray -> { System.arraycopy(v, 0, out, p, v.size); p += v.size }
            }
        }
        check(p == total)
        return out
    }

    fun decode(b: ByteArray): List<Any?> {
        val (hdr, n0) = Varint.read(b, 0)
        if (hdr > b.size || hdr < n0) throw SqliteException("bad record header")
        var hp = n0
        val types = ArrayList<Long>()
        while (hp < hdr) {
            val (t, n) = Varint.read(b, hp)
            types += t
            hp += n
        }
        var p = hdr.toInt()
        val out = ArrayList<Any?>(types.size)
        for (t in types) {
            val n = contentLen(t)
            if (p + n > b.size) throw SqliteException("record shorter than its header")
            out += when {
                t == 0L -> null
                t in 1..6 -> {
                    var x = b[p].toLong() // sign extend
                    for (k in 1 until n) x = (x shl 8) or (b[p + k].toLong() and 0xff)
                    x
                }
                t == 7L -> {
                    var x = 0L
                    for (k in 0 until 8) x = (x shl 8) or (b[p + k].toLong() and 0xff)
                    java.lang.Double.longBitsToDouble(x)
                }
                t == 8L -> 0L
                t == 9L -> 1L
                t >= 13 && t % 2 == 1L -> SqlText(b.copyOfRange(p, p + n))
                else -> b.copyOfRange(p, p + n)
            }
            p += n
        }
        return out
    }

    private fun cls(v: Any?): Int = when (v) {
        null -> 0
        is Long, is Int, is Double -> 1
        is SqlText, is String -> 2
        else -> 3
    }

    private fun textBytes(v: Any?): ByteArray = when (v) {
        is SqlText -> v.bytes
        is String -> v.toByteArray(Charsets.UTF_8)
        else -> throw IllegalStateException()
    }

    private fun lower(c: Int) = if (c in 'A'.code..'Z'.code) c + 32 else c

    /** sqlite3StrNICmp followed by the length rule of the NOCASE collation. */
    private fun nocase(a: ByteArray, b: ByteArray): Int {
        var n = minOf(a.size, b.size)
        var i = 0
        while (true) {
            if (n-- <= 0) return a.size - b.size
            val x = a[i].toInt() and 0xff
            val y = if (i < b.size) b[i].toInt() and 0xff else 0
            if (x == 0 || lower(x) != lower(y)) {
                val r = lower(x) - lower(y)
                return if (r != 0) r else a.size - b.size
            }
            i++
        }
    }

    private fun binary(a: ByteArray, b: ByteArray): Int {
        val n = minOf(a.size, b.size)
        for (i in 0 until n) {
            val d = (a[i].toInt() and 0xff) - (b[i].toInt() and 0xff)
            if (d != 0) return d
        }
        return a.size - b.size
    }

    private fun rtrim(a: ByteArray): ByteArray {
        var n = a.size
        while (n > 0 && a[n - 1] == ' '.code.toByte()) n--
        return if (n == a.size) a else a.copyOf(n)
    }

    fun compare(a: Any?, b: Any?, collation: String = "BINARY"): Int {
        val ca = cls(a); val cb = cls(b)
        if (ca != cb) return ca - cb
        return when (ca) {
            0 -> 0
            1 -> {
                if (a is Double || b is Double) (a as Number).toDouble().compareTo((b as Number).toDouble())
                else (a as Number).toLong().compareTo((b as Number).toLong())
            }
            2 -> when (collation.uppercase()) {
                "NOCASE" -> nocase(textBytes(a), textBytes(b))
                "RTRIM" -> binary(rtrim(textBytes(a)), rtrim(textBytes(b)))
                else -> binary(textBytes(a), textBytes(b))
            }
            else -> binary(a as ByteArray, b as ByteArray)
        }
    }
}

// ------------------------------------------------------------------------------- schema SQL

/** What the builder needs to know about a CREATE TABLE / CREATE INDEX statement. */
class TableDef(val name: String, val columns: List<ColumnDef>, val keyConstraints: List<List<IndexColumn>>,
               val rowidAlias: Int, val withoutRowid: Boolean) {
    fun col(name: String): Int = columns.indexOfFirst { it.name.equals(name, ignoreCase = true) }
}

class ColumnDef(val name: String, val type: String, val collation: String) {
    /** Column affinity (section 3.1 of the SQLite datatype documentation). */
    val affinity: String
        get() {
            val t = type.uppercase()
            return when {
                "INT" in t -> "INTEGER"
                "CHAR" in t || "CLOB" in t || "TEXT" in t -> "TEXT"
                t.isEmpty() || "BLOB" in t -> "BLOB"
                "REAL" in t || "FLOA" in t || "DOUB" in t -> "REAL"
                else -> "NUMERIC"
            }
        }
}

class IndexColumn(val column: String, val collation: String?, val desc: Boolean)

class IndexDef(val name: String, val table: String, val columns: List<IndexColumn>, val unique: Boolean)

object SchemaSql {
    private val KEYWORDS_COLUMN_END = setOf("CONSTRAINT", "PRIMARY", "NOT", "NULL", "UNIQUE", "CHECK", "DEFAULT",
        "COLLATE", "REFERENCES", "GENERATED", "AS")
    private val TABLE_CONSTRAINT = setOf("CONSTRAINT", "PRIMARY", "UNIQUE", "CHECK", "FOREIGN")

    /** Tokens: identifiers/keywords (quotes removed), strings kept with quotes, punctuation. */
    fun tokens(sql: String): List<String> {
        val out = ArrayList<String>()
        var i = 0
        while (i < sql.length) {
            val c = sql[i]
            when {
                c.isWhitespace() -> i++
                c == '-' && i + 1 < sql.length && sql[i + 1] == '-' -> { while (i < sql.length && sql[i] != '\n') i++ }
                c == '/' && i + 1 < sql.length && sql[i + 1] == '*' -> { val e = sql.indexOf("*/", i + 2); i = if (e < 0) sql.length else e + 2 }
                c == '"' || c == '`' || c == '[' -> {
                    val close = if (c == '[') ']' else c
                    val sb = StringBuilder()
                    i++
                    while (i < sql.length) {
                        if (sql[i] == close) {
                            if (close != ']' && i + 1 < sql.length && sql[i + 1] == close) { sb.append(close); i += 2; continue }
                            break
                        }
                        sb.append(sql[i++])
                    }
                    i++
                    out += "\u0001" + sb // marks a quoted identifier
                }
                c == '\'' -> {
                    val st = i
                    i++
                    while (i < sql.length) { if (sql[i] == '\'') { if (i + 1 < sql.length && sql[i + 1] == '\'') { i += 2; continue }; break }; i++ }
                    i++
                    out += sql.substring(st, minOf(i, sql.length))
                }
                c.isLetterOrDigit() || c == '_' || c == '$' -> {
                    val st = i
                    while (i < sql.length && (sql[i].isLetterOrDigit() || sql[i] == '_' || sql[i] == '$')) i++
                    out += sql.substring(st, i)
                }
                else -> { out += c.toString(); i++ }
            }
        }
        return out
    }

    private fun ident(t: String) = if (t.startsWith("\u0001")) t.substring(1) else t
    private fun kw(t: String) = if (t.startsWith("\u0001")) "" else t.uppercase()

    /** Splits the tokens inside the outer parentheses at top-level commas. */
    private fun groups(toks: List<String>, from: Int): Pair<List<List<String>>, Int> {
        if (toks.getOrNull(from) != "(") throw SqliteException("'(' expected in schema")
        val out = ArrayList<List<String>>()
        var cur = ArrayList<String>()
        var depth = 0
        var i = from + 1
        while (i < toks.size) {
            val t = toks[i]
            if (t == "(") depth++
            if (t == ")") {
                if (depth == 0) { out += cur; return out to i + 1 }
                depth--
            }
            if (t == "," && depth == 0) { out += cur; cur = ArrayList() } else cur += t
            i++
        }
        throw SqliteException("unbalanced parentheses in schema")
    }

    private fun indexedColumns(g: List<List<String>>): List<IndexColumn> = g.map { part ->
        if (part.isEmpty()) throw SqliteException("empty index column")
        val name = ident(part[0])
        var coll: String? = null
        var desc = false
        var k = 1
        while (k < part.size) {
            when (kw(part[k])) {
                "COLLATE" -> { coll = ident(part[k + 1]); k += 2 }
                "DESC" -> { desc = true; k++ }
                "ASC" -> k++
                else -> throw SqliteException("unsupported index column: ${part.joinToString(" ")}")
            }
        }
        IndexColumn(name, coll, desc)
    }

    fun parseTable(sql: String): TableDef {
        val t = tokens(sql)
        var i = 0
        fun at(k: String) = i < t.size && kw(t[i]) == k
        if (!at("CREATE")) throw SqliteException("not a CREATE TABLE")
        i++
        if (at("TEMP") || at("TEMPORARY")) i++
        if (!at("TABLE")) throw SqliteException("not a CREATE TABLE")
        i++
        if (at("IF")) i += 3
        var name = ident(t[i++])
        if (i < t.size && t[i] == ".") name = ident(t[++i]).also { i++ }
        val (gs, after) = groups(t, i)
        val withoutRowid = t.drop(after).map { kw(it) }.let { it.contains("WITHOUT") && it.contains("ROWID") }
        val cols = ArrayList<ColumnDef>()
        val keys = ArrayList<List<IndexColumn>>()
        var rowidAlias = -1
        val pendingColumnPk = ArrayList<Pair<Int, Boolean>>() // (column, isPk) in order
        for (g in gs) {
            if (g.isEmpty()) continue
            if (kw(g[0]) in TABLE_CONSTRAINT && !(g[0].startsWith("\u0001"))) {
                var k = 0
                if (kw(g[k]) == "CONSTRAINT") k += 2
                when (kw(g[k])) {
                    "PRIMARY", "UNIQUE" -> {
                        val pk = kw(g[k]) == "PRIMARY"
                        while (k < g.size && g[k] != "(") k++
                        val (cg, _) = groups(g, k)
                        val ic = indexedColumns(cg)
                        if (pk && ic.size == 1) {
                            val ci = cols.indexOfFirst { it.name.equals(ic[0].column, true) }
                            if (ci >= 0 && cols[ci].type.equals("INTEGER", true) && !ic[0].desc) { rowidAlias = ci; continue }
                        }
                        keys += ic
                    }
                    else -> {}
                }
                continue
            }
            val cname = ident(g[0])
            var k = 1
            val type = StringBuilder()
            while (k < g.size && kw(g[k]) !in KEYWORDS_COLUMN_END) {
                if (g[k] == "(") { // type arguments, e.g. VARCHAR(10)
                    while (k < g.size && g[k] != ")") { type.append(g[k]); k++ }
                    if (k < g.size) { type.append(")"); k++ }
                    continue
                }
                if (type.isNotEmpty() && g[k] != ")") type.append(' ')
                type.append(ident(g[k]))
                k++
            }
            var coll = "BINARY"
            val ci = cols.size
            while (k < g.size) {
                when (kw(g[k])) {
                    "COLLATE" -> { coll = ident(g[k + 1]); k += 2 }
                    "PRIMARY" -> { pendingColumnPk += ci to true; k++ }
                    "UNIQUE" -> { pendingColumnPk += ci to false; k++ }
                    else -> k++
                }
            }
            cols += ColumnDef(cname, type.toString().trim(), coll)
        }
        // column constraints come first in sqlite's numbering of automatic indexes
        val colKeys = ArrayList<List<IndexColumn>>()
        for ((ci, pk) in pendingColumnPk) {
            if (pk && cols[ci].type.equals("INTEGER", true)) { rowidAlias = ci; continue }
            colKeys += listOf(IndexColumn(cols[ci].name, null, false))
        }
        return TableDef(name, cols, colKeys + keys, rowidAlias, withoutRowid)
    }

    fun parseIndex(sql: String): IndexDef {
        val t = tokens(sql)
        var i = 0
        fun at(k: String) = i < t.size && kw(t[i]) == k
        if (!at("CREATE")) throw SqliteException("not a CREATE INDEX")
        i++
        val unique = at("UNIQUE")
        if (unique) i++
        if (!at("INDEX")) throw SqliteException("not a CREATE INDEX")
        i++
        if (at("IF")) i += 3
        var name = ident(t[i++])
        if (i < t.size && t[i] == ".") name = ident(t[++i]).also { i++ }
        if (!at("ON")) throw SqliteException("ON expected in CREATE INDEX")
        i++
        val table = ident(t[i++])
        val (gs, after) = groups(t, i)
        if (after < t.size) throw SqliteException("partial indexes are not supported ($name)")
        return IndexDef(name, table, indexedColumns(gs), unique)
    }
}

// ------------------------------------------------------------------------------- reader

/** Read-only access to a SQLite file. */
class SqliteReader(file: File) : Closeable {
    private val raf = RandomAccessFile(file, "r")
    val pageSize: Int
    private val usable: Int
    val pageCount: Int
    val userVersion: Int
    val applicationId: Int
    val schemaFormat: Int

    init {
        val h = ByteArray(100)
        if (raf.length() < 100) throw SqliteException("not a SQLite database (too short)")
        raf.readFully(h)
        if (!String(h, 0, 16, Charsets.ISO_8859_1).equals("SQLite format 3\u0000")) throw SqliteException("not a SQLite database")
        val ps = ((h[16].toInt() and 0xff) shl 8) or (h[17].toInt() and 0xff)
        pageSize = if (ps == 1) 65536 else ps
        if (pageSize < 512 || pageSize and (pageSize - 1) != 0) throw SqliteException("bad page size $pageSize")
        usable = pageSize - (h[20].toInt() and 0xff)
        val enc = be32(h, 56)
        if (enc != 0 && enc != 1) throw SqliteException("only UTF-8 databases are supported")
        userVersion = be32(h, 60)
        applicationId = be32(h, 68)
        schemaFormat = be32(h, 44)
        val counted = be32(h, 28)
        val bySize = (raf.length() / pageSize).toInt()
        pageCount = if (counted > 0 && be32(h, 24) == be32(h, 92)) counted else bySize
    }

    private fun be32(b: ByteArray, p: Int) = ((b[p].toInt() and 0xff) shl 24) or ((b[p + 1].toInt() and 0xff) shl 16) or
        ((b[p + 2].toInt() and 0xff) shl 8) or (b[p + 3].toInt() and 0xff)

    private fun be16(b: ByteArray, p: Int) = ((b[p].toInt() and 0xff) shl 8) or (b[p + 1].toInt() and 0xff)

    fun page(n: Int): ByteArray {
        if (n < 1 || n > pageCount) throw SqliteException("page $n out of range")
        val b = ByteArray(pageSize)
        raf.seek((n - 1).toLong() * pageSize)
        raf.readFully(b)
        return b
    }

    private fun payload(page: ByteArray, p: Int, total: Long, maxLocal: Int, minLocal: Int): ByteArray {
        if (total > Int.MAX_VALUE) throw SqliteException("record too large")
        val out = ByteArray(total.toInt())
        val local = localSize(total, usable, maxLocal, minLocal)
        System.arraycopy(page, p, out, 0, local)
        if (local < total) {
            var next = be32(page, p + local)
            var pos = local
            var guard = 0
            while (pos < total) {
                if (next == 0 || ++guard > pageCount) throw SqliteException("broken overflow chain")
                val ov = page(next)
                val n = minOf(usable - 4, out.size - pos)
                System.arraycopy(ov, 4, out, pos, n)
                pos += n
                next = be32(ov, 0)
            }
        }
        return out
    }

    /** Every row of the table b-tree rooted at [root], in rowid order. */
    fun rows(root: Int): List<SqlRow> {
        val out = ArrayList<SqlRow>()
        val maxLocal = usable - 35
        val minLocal = (usable - 12) * 32 / 255 - 23
        fun walk(pg: Int, depth: Int) {
            if (depth > 40) throw SqliteException("b-tree too deep")
            val b = page(pg)
            val h = if (pg == 1) 100 else 0
            val type = b[h].toInt() and 0xff
            val n = be16(b, h + 3)
            when (type) {
                0x0d -> for (k in 0 until n) {
                    var p = be16(b, h + 8 + 2 * k)
                    val (len, a) = Varint.read(b, p); p += a
                    val (rowid, c) = Varint.read(b, p); p += c
                    out += SqlRow(rowid, SqlRecord.decode(payload(b, p, len, maxLocal, minLocal)))
                }
                0x05 -> {
                    for (k in 0 until n) walk(be32(b, be16(b, h + 12 + 2 * k)), depth + 1)
                    walk(be32(b, h + 8), depth + 1)
                }
                else -> throw SqliteException("page $pg is not a table b-tree page (type $type)")
            }
        }
        walk(root, 0)
        return out
    }

    /** Every key of the index b-tree rooted at [root], in index order. */
    fun indexKeys(root: Int): List<List<Any?>> {
        val out = ArrayList<List<Any?>>()
        val maxLocal = (usable - 12) * 64 / 255 - 23
        val minLocal = (usable - 12) * 32 / 255 - 23
        fun walk(pg: Int, depth: Int) {
            if (depth > 40) throw SqliteException("b-tree too deep")
            val b = page(pg)
            val type = b[0].toInt() and 0xff
            val n = be16(b, 3)
            when (type) {
                0x0a -> for (k in 0 until n) {
                    var p = be16(b, 8 + 2 * k)
                    val (len, a) = Varint.read(b, p); p += a
                    out += SqlRecord.decode(payload(b, p, len, maxLocal, minLocal))
                }
                0x02 -> {
                    for (k in 0 until n) {
                        var p = be16(b, 12 + 2 * k)
                        walk(be32(b, p), depth + 1)
                        p += 4
                        val (len, a) = Varint.read(b, p); p += a
                        out += SqlRecord.decode(payload(b, p, len, maxLocal, minLocal))
                    }
                    walk(be32(b, 8), depth + 1)
                }
                else -> throw SqliteException("page $pg is not an index b-tree page (type $type)")
            }
        }
        walk(root, 0)
        return out
    }

    fun schema(): List<SchemaEntry> = rows(1).map { r ->
        val v = r.values
        SchemaEntry((v[0] as SqlText).text, (v[1] as SqlText).text, (v[2] as SqlText).text,
            (v[3] as? Long)?.toInt() ?: 0, (v.getOrNull(4) as? SqlText)?.text)
    }

    fun table(name: String): List<SqlRow> {
        val e = schema().firstOrNull { it.type == "table" && it.name.equals(name, ignoreCase = true) }
            ?: throw SqliteException("no table $name")
        return rows(e.rootPage)
    }

    override fun close() = raf.close()

    companion object {
        internal fun localSize(total: Long, usable: Int, maxLocal: Int, minLocal: Int): Int {
            if (total <= maxLocal) return total.toInt()
            val k = minLocal + ((total - minLocal) % (usable - 4)).toInt()
            return if (k <= maxLocal) k else minLocal
        }
    }
}

// ------------------------------------------------------------------------------- writer

/**
 * Writes a complete new database: the schema entries in their order, each table's rows, and
 * every index rebuilt from those rows. Page 1 holds the header and the schema root.
 */
class SqliteWriter(private val file: File, private val pageSize: Int = 4096, private val userVersion: Int = 0,
                   private val applicationId: Int = 0) {
    private val usable = pageSize
    private var nextPage = 2
    private lateinit var raf: RandomAccessFile

    private val tableMaxLocal = usable - 35
    private val indexMaxLocal = (usable - 12) * 64 / 255 - 23
    private val minLocal = (usable - 12) * 32 / 255 - 23

    private fun alloc(): Int = nextPage++

    private fun writePage(n: Int, b: ByteArray) {
        raf.seek((n - 1).toLong() * pageSize)
        raf.write(b)
    }

    /** varint(len) [prefix] local part [overflow page]; writes the overflow chain. */
    private fun cellPayload(record: ByteArray, maxLocal: Int, between: ByteArray?): ByteArray {
        val local = SqliteReader.localSize(record.size.toLong(), usable, maxLocal, minLocal)
        val lenV = Varint.bytes(record.size.toLong())
        val pre = between ?: ByteArray(0)
        val cell = ByteArray(lenV.size + pre.size + local + (if (local < record.size) 4 else 0))
        System.arraycopy(lenV, 0, cell, 0, lenV.size)
        System.arraycopy(pre, 0, cell, lenV.size, pre.size)
        System.arraycopy(record, 0, cell, lenV.size + pre.size, local)
        if (local < record.size) {
            // overflow chain: allocate all pages first so each can point to the next
            val per = usable - 4
            val count = (record.size - local + per - 1) / per
            val pages = IntArray(count) { alloc() }
            var pos = local
            for (i in 0 until count) {
                val pg = ByteArray(pageSize)
                val nxt = if (i + 1 < count) pages[i + 1] else 0
                be32(pg, 0, nxt)
                val n = minOf(per, record.size - pos)
                System.arraycopy(record, pos, pg, 4, n)
                pos += n
                writePage(pages[i], pg)
            }
            be32(cell, cell.size - 4, pages[0])
        }
        return cell
    }

    private fun be32(b: ByteArray, p: Int, v: Int) {
        b[p] = (v ushr 24).toByte(); b[p + 1] = (v ushr 16).toByte(); b[p + 2] = (v ushr 8).toByte(); b[p + 3] = v.toByte()
    }

    private fun be16(b: ByteArray, p: Int, v: Int) { b[p] = (v ushr 8).toByte(); b[p + 1] = v.toByte() }

    /** A page under construction. [capacity] is the bytes available from the b-tree header on. */
    private inner class PageBuf(val type: Int, val capacity: Int) {
        val cells = ArrayList<ByteArray>()
        var used = if (type == 0x05 || type == 0x02) 12 else 8
        fun fits(c: ByteArray) = used + 2 + c.size <= capacity
        fun add(c: ByteArray) { cells += c; used += 2 + c.size }
        fun removeLast(): ByteArray { val c = cells.removeAt(cells.size - 1); used -= 2 + c.size; return c }

        fun render(hdrOff: Int, rightChild: Int = 0, into: ByteArray = ByteArray(pageSize)): ByteArray {
            val b = into
            val hs = if (type == 0x05 || type == 0x02) 12 else 8
            b[hdrOff] = type.toByte()
            be16(b, hdrOff + 1, 0)
            be16(b, hdrOff + 3, cells.size)
            var top = usable
            for ((i, c) in cells.withIndex()) {
                top -= c.size
                System.arraycopy(c, 0, b, top, c.size)
                be16(b, hdrOff + hs + 2 * i, top)
            }
            check(hdrOff + hs + 2 * cells.size <= top) { "page overflow" }
            be16(b, hdrOff + 5, if (top == 65536) 0 else top)
            b[hdrOff + 7] = 0
            if (hs == 12) be32(b, hdrOff + 8, rightChild)
            return b
        }
    }

    private fun capacityFor(rootOnPage1: Boolean) = if (rootOnPage1) usable - 100 else usable

    /** Builds a table b-tree from rows in rowid order; returns the root page. */
    private fun writeTable(rows: Iterator<SqlRow>, rootOnPage1: Boolean = false, page1: ByteArray? = null): Int {
        val cap = capacityFor(rootOnPage1)
        val leaves = ArrayList<Pair<Int, Long>>() // page, max rowid
        val pendingLeaves = ArrayList<Pair<PageBuf, Long>>()
        var cur = PageBuf(0x0d, cap)
        var curMax = Long.MIN_VALUE
        var last = Long.MIN_VALUE
        fun flushLeaf() {
            if (rootOnPage1) { pendingLeaves += cur to curMax; return }
            val pg = alloc()
            writePage(pg, cur.render(0))
            leaves += pg to curMax
        }
        for (r in rows) {
            if (r.rowid <= last) throw SqliteException("rows must be in increasing rowid order")
            last = r.rowid
            val rec = SqlRecord.encode(r.values)
            val cell = cellPayload(rec, tableMaxLocal, Varint.bytes(r.rowid))
            if (!cur.fits(cell)) {
                flushLeaf()
                cur = PageBuf(0x0d, cap)
            }
            cur.add(cell)
            curMax = r.rowid
        }
        if (rootOnPage1) {
            pendingLeaves += cur to curMax
            if (pendingLeaves.size == 1) {
                cur.render(100, 0, page1!!)
                return 1
            }
            for ((pb, mx) in pendingLeaves) {
                val pg = alloc()
                writePage(pg, pb.render(0))
                leaves += pg to mx
            }
        } else {
            if (leaves.isEmpty() || cur.cells.isNotEmpty()) flushLeaf()
            if (leaves.size == 1) return leaves[0].first
        }
        var level: List<Pair<Int, Long>> = leaves
        while (true) {
            // group children: a page holds k cells and one right child
            val groups = ArrayList<MutableList<Pair<Int, Long>>>()
            var g = ArrayList<Pair<Int, Long>>()
            var used = 12
            for (ch in level) {
                if (g.isEmpty()) { g += ch; continue }
                // adding [ch] turns the current right child into a cell
                val cost = 2 + 4 + Varint.len(g.last().second)
                if (used + cost > cap) {
                    groups += g; g = arrayListOf(ch); used = 12
                } else {
                    used += cost; g += ch
                }
            }
            groups += g
            if (groups.size > 1 && groups.last().size == 1) groups.last().add(0, groups[groups.size - 2].removeAt(groups[groups.size - 2].size - 1))
            val next = ArrayList<Pair<Int, Long>>()
            val single = groups.size == 1
            for (gr in groups) {
                val pb = PageBuf(0x05, cap)
                for (k in 0 until gr.size - 1) {
                    val c = ByteArray(4 + Varint.len(gr[k].second))
                    be32(c, 0, gr[k].first)
                    Varint.write(c, 4, gr[k].second)
                    pb.add(c)
                }
                if (single && rootOnPage1) {
                    pb.render(100, gr.last().first, page1!!)
                    return 1
                }
                val pg = alloc()
                writePage(pg, pb.render(0, gr.last().first))
                next += pg to gr.last().second
            }
            if (single) return next[0].first
            level = next
        }
    }

    /** Builds an index b-tree from records already in index order; returns the root page. */
    private fun writeIndex(records: List<ByteArray>): Int {
        // leaf level: entries, with dividers promoted to the parent
        val payloads = records.map { cellPayload(it, indexMaxLocal, null) }
        val children = ArrayList<Int>()
        val dividers = ArrayList<ByteArray>()
        var cur = PageBuf(0x0a, usable)
        val leafBufs = ArrayList<PageBuf>()
        for (p in payloads) {
            if (cur.cells.isEmpty() || cur.fits(p)) cur.add(p)
            else { leafBufs += cur; dividers += p; cur = PageBuf(0x0a, usable) }
        }
        if (cur.cells.isEmpty() && leafBufs.isNotEmpty()) cur.add(dividers.removeAt(dividers.size - 1))
        leafBufs += cur
        for (lb in leafBufs) {
            val pg = alloc()
            writePage(pg, lb.render(0))
            children += pg
        }
        var ch: List<Int> = children
        var dv: List<ByteArray> = dividers
        while (ch.size > 1) {
            val nch = ArrayList<Int>()
            val ndv = ArrayList<ByteArray>()
            var j = 0
            val n = ch.size - 1 // number of dividers
            var pb = PageBuf(0x02, usable)
            while (true) {
                if (j == n) {
                    val pg = alloc(); writePage(pg, pb.render(0, ch[n])); nch += pg
                    break
                }
                val cell = ByteArray(4) + dv[j]
                be32(cell, 0, ch[j])
                if (pb.cells.isEmpty() || pb.fits(cell)) { pb.add(cell); j++; continue }
                var right = ch[j]
                var promote = dv[j]
                var restart = j + 1
                if (j + 1 == n) {
                    // only the last child would be left alone: hand one cell back
                    pb.removeLast()
                    right = ch[j - 1]
                    promote = dv[j - 1]
                    restart = j
                }
                val pg = alloc(); writePage(pg, pb.render(0, right)); nch += pg
                ndv += promote
                pb = PageBuf(0x02, usable)
                j = restart
            }
            ch = nch
            dv = ndv
        }
        return ch[0]
    }

    /**
     * Writes the database. [rowsOf] returns the rows of each table named in [schema] (rowid
     * order); entries without a b-tree (views, triggers) are copied as they are.
     */
    fun write(schema: List<SchemaEntry>, rowsOf: (String) -> List<SqlRow>) {
        file.delete()
        raf = RandomAccessFile(file, "rw")
        try {
            val tables = HashMap<String, TableDef>()
            val roots = LinkedHashMap<SchemaEntry, Int>()
            val dataOf = HashMap<String, List<SqlRow>>()
            for (e in schema) {
                if (e.type == "table" && e.rootPage > 0) {
                    val def = if (e.sql != null) SchemaSql.parseTable(e.sql) else null
                    if (def != null) {
                        if (def.withoutRowid) throw SqliteException("WITHOUT ROWID tables are not supported (${e.name})")
                        tables[e.name.lowercase()] = def
                    }
                    val rows = rowsOf(e.name)
                    dataOf[e.name.lowercase()] = rows
                    roots[e] = writeTable(rows.iterator())
                }
            }
            for (e in schema) {
                if (e.type != "index" || e.rootPage <= 0) continue
                val tdef = tables[e.tblName.lowercase()] ?: throw SqliteException("index ${e.name} on unknown table ${e.tblName}")
                val cols: List<IndexColumn> = if (e.sql != null) {
                    SchemaSql.parseIndex(e.sql).columns
                } else {
                    val m = Regex("sqlite_autoindex_(.+)_(\\d+)").matchEntire(e.name)
                        ?: throw SqliteException("unknown automatic index ${e.name}")
                    tdef.keyConstraints.getOrNull(m.groupValues[2].toInt() - 1)
                        ?: throw SqliteException("cannot tell the columns of ${e.name}")
                }
                val idx = cols.map { c ->
                    val ci = tdef.col(c.column)
                    if (ci < 0) throw SqliteException("index ${e.name}: unknown column ${c.column}")
                    Triple(ci, c.collation ?: tdef.columns[ci].collation, c.desc)
                }
                val rows = dataOf[e.tblName.lowercase()]!!
                val keys = rows.map { r -> idx.map { (ci, _, _) -> if (ci == tdef.rowidAlias) r.rowid else r.values.getOrNull(ci) } + r.rowid }
                val sorted = keys.sortedWith { x, y ->
                    var d = 0
                    for ((k, spec) in idx.withIndex()) {
                        d = SqlRecord.compare(x[k], y[k], spec.second)
                        if (spec.third) d = -d
                        if (d != 0) break
                    }
                    if (d == 0) d = (x.last() as Long).compareTo(y.last() as Long)
                    d
                }
                roots[e] = writeIndex(sorted.map { SqlRecord.encode(it) })
            }
            // schema table on page 1
            val master = schema.mapIndexed { i, e ->
                SqlRow((i + 1).toLong(), listOf(SqlText(e.type), SqlText(e.name), SqlText(e.tblName),
                    (roots[e] ?: e.rootPage).toLong(), e.sql?.let { SqlText(it) }))
            }
            val page1 = ByteArray(pageSize)
            writeTable(master.iterator(), rootOnPage1 = true, page1 = page1)
            val pages = nextPage - 1
            writeHeader(page1, pages)
            writePage(1, page1)
            raf.setLength(pages.toLong() * pageSize)
            raf.fd.sync()
        } finally {
            raf.close()
        }
    }

    private fun writeHeader(b: ByteArray, pages: Int) {
        val magic = "SQLite format 3\u0000".toByteArray(Charsets.ISO_8859_1)
        System.arraycopy(magic, 0, b, 0, 16)
        be16(b, 16, if (pageSize == 65536) 1 else pageSize)
        b[18] = 1; b[19] = 1; b[20] = 0; b[21] = 64; b[22] = 32; b[23] = 32
        be32(b, 24, 1)          // file change counter
        be32(b, 28, pages)      // database size in pages
        be32(b, 32, 0); be32(b, 36, 0) // no free pages
        be32(b, 40, 1)          // schema cookie
        be32(b, 44, 4)          // schema format
        be32(b, 48, 0)          // default cache size
        be32(b, 52, 0)          // no auto-vacuum
        be32(b, 56, 1)          // UTF-8
        be32(b, 60, userVersion)
        be32(b, 64, 0)
        be32(b, 68, applicationId)
        be32(b, 92, 1)          // version-valid-for = change counter
        be32(b, 96, 3045001)    // SQLITE_VERSION_NUMBER of the format written
    }
}
