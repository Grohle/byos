package org.hibyos.install.core.btp

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.CodingErrorAction
import java.text.Normalizer

/** Constants of BTP/1 (docs/SYNC_PROTOCOL.md). */
object Btp {
    const val VERSION = 1
    const val HEADER_LEN = 12
    const val MAX_PAYLOAD = 65544
    const val MAX_DATA = 65536
    const val TAG_LEN = 16
    const val MAX_RECORD = TAG_LEN + HEADER_LEN + MAX_PAYLOAD
    const val PORT = 7340
    const val ANNOUNCE_PORT = 7341
    const val F_END = 1
    const val MAX_NAME = 64

    // message types
    const val HELLO = 0x01; const val ERROR = 0x02; const val OK = 0x03; const val PING = 0x04
    const val PONG = 0x05; const val BYE = 0x06; const val CANCEL = 0x07
    const val PAIR_COMMIT = 0x10; const val PAIR_DEVICE = 0x11; const val PAIR_REVEAL = 0x12
    const val PAIR_CONFIRM = 0x13; const val PAIR_RESULT = 0x14
    const val AUTH_INIT = 0x18; const val AUTH_REPLY = 0x19; const val AUTH_FINISH = 0x1A; const val AUTH_OK = 0x1B
    const val STAT = 0x20; const val STAT_R = 0x21; const val LIST = 0x22; const val LIST_R = 0x23
    const val DF = 0x24; const val DF_R = 0x25; const val MKDIR = 0x26; const val MOVE = 0x27; const val DELETE = 0x28
    const val PUT_BEGIN = 0x30; const val PUT_READY = 0x31; const val DATA = 0x32; const val PUT_END = 0x33
    const val PUT_DONE = 0x34
    const val GET = 0x38; const val GET_R = 0x39; const val GET_END = 0x3A
    const val DB_SNAPSHOT = 0x40; const val DB_STAGE = 0x41; const val DB_STATUS = 0x42; const val DB_STATUS_R = 0x43
    const val DB_UNSTAGE = 0x44; const val LIB_RESCAN = 0x45
    const val UPDATE_STAGE = 0x50; const val UPDATE_STAGE_R = 0x51; const val UPDATE_INSTALL = 0x52
    const val UPDATE_STATUS = 0x53; const val UPDATE_STATUS_R = 0x54
    const val STATUS = 0x60; const val STATUS_R = 0x61

    val TYPE_NAMES: Map<Int, String> = linkedMapOf(
        HELLO to "HELLO", ERROR to "ERROR", OK to "OK", PING to "PING", PONG to "PONG", BYE to "BYE", CANCEL to "CANCEL",
        PAIR_COMMIT to "PAIR_COMMIT", PAIR_DEVICE to "PAIR_DEVICE", PAIR_REVEAL to "PAIR_REVEAL",
        PAIR_CONFIRM to "PAIR_CONFIRM", PAIR_RESULT to "PAIR_RESULT",
        AUTH_INIT to "AUTH_INIT", AUTH_REPLY to "AUTH_REPLY", AUTH_FINISH to "AUTH_FINISH", AUTH_OK to "AUTH_OK",
        STAT to "STAT", STAT_R to "STAT_R", LIST to "LIST", LIST_R to "LIST_R", DF to "DF", DF_R to "DF_R",
        MKDIR to "MKDIR", MOVE to "MOVE", DELETE to "DELETE",
        PUT_BEGIN to "PUT_BEGIN", PUT_READY to "PUT_READY", DATA to "DATA", PUT_END to "PUT_END", PUT_DONE to "PUT_DONE",
        GET to "GET", GET_R to "GET_R", GET_END to "GET_END",
        DB_SNAPSHOT to "DB_SNAPSHOT", DB_STAGE to "DB_STAGE", DB_STATUS to "DB_STATUS", DB_STATUS_R to "DB_STATUS_R",
        DB_UNSTAGE to "DB_UNSTAGE", LIB_RESCAN to "LIB_RESCAN",
        UPDATE_STAGE to "UPDATE_STAGE", UPDATE_STAGE_R to "UPDATE_STAGE_R", UPDATE_INSTALL to "UPDATE_INSTALL",
        UPDATE_STATUS to "UPDATE_STATUS", UPDATE_STATUS_R to "UPDATE_STATUS_R",
        STATUS to "STATUS", STATUS_R to "STATUS_R",
    )

    // error codes
    const val E_PROTO = 1; const val E_VERSION = 2; const val E_UNKNOWN_TYPE = 3; const val E_AUTH_REQUIRED = 4
    const val E_AUTH_FAILED = 5; const val E_PAIR_CLOSED = 6; const val E_PAIR_REJECTED = 7; const val E_BUSY = 8
    const val E_TOO_LARGE = 9; const val E_RATE_LIMIT = 10; const val E_TIMEOUT = 11; const val E_CANCELLED = 12
    const val E_UNSUPPORTED = 13; const val E_INTERNAL = 14
    const val E_BAD_PATH = 32; const val E_NOT_FOUND = 33; const val E_EXISTS = 34; const val E_NOT_DIR = 35
    const val E_IS_DIR = 36; const val E_NOT_EMPTY = 37; const val E_NO_SPACE = 38; const val E_IO = 39
    const val E_SD_UNAVAILABLE = 40; const val E_HASH_MISMATCH = 41; const val E_OFFSET = 42
    const val E_FILE_TOO_BIG = 43; const val E_NOT_ALLOWED = 44
    const val E_SIGNATURE = 64; const val E_UPDATE_INVALID = 65; const val E_DOWNGRADE = 66; const val E_BATTERY = 67
    const val E_NEEDS_FIRMWARE = 68; const val E_NOT_STAGED = 69
    const val E_DB_INVALID = 80; const val E_DB_STALE = 81

    val ERROR_NAMES: Map<Int, String> = linkedMapOf(
        E_PROTO to "E_PROTO", E_VERSION to "E_VERSION", E_UNKNOWN_TYPE to "E_UNKNOWN_TYPE",
        E_AUTH_REQUIRED to "E_AUTH_REQUIRED", E_AUTH_FAILED to "E_AUTH_FAILED", E_PAIR_CLOSED to "E_PAIR_CLOSED",
        E_PAIR_REJECTED to "E_PAIR_REJECTED", E_BUSY to "E_BUSY", E_TOO_LARGE to "E_TOO_LARGE",
        E_RATE_LIMIT to "E_RATE_LIMIT", E_TIMEOUT to "E_TIMEOUT", E_CANCELLED to "E_CANCELLED",
        E_UNSUPPORTED to "E_UNSUPPORTED", E_INTERNAL to "E_INTERNAL",
        E_BAD_PATH to "E_BAD_PATH", E_NOT_FOUND to "E_NOT_FOUND", E_EXISTS to "E_EXISTS", E_NOT_DIR to "E_NOT_DIR",
        E_IS_DIR to "E_IS_DIR", E_NOT_EMPTY to "E_NOT_EMPTY", E_NO_SPACE to "E_NO_SPACE", E_IO to "E_IO",
        E_SD_UNAVAILABLE to "E_SD_UNAVAILABLE", E_HASH_MISMATCH to "E_HASH_MISMATCH", E_OFFSET to "E_OFFSET",
        E_FILE_TOO_BIG to "E_FILE_TOO_BIG", E_NOT_ALLOWED to "E_NOT_ALLOWED",
        E_SIGNATURE to "E_SIGNATURE", E_UPDATE_INVALID to "E_UPDATE_INVALID", E_DOWNGRADE to "E_DOWNGRADE",
        E_BATTERY to "E_BATTERY", E_NEEDS_FIRMWARE to "E_NEEDS_FIRMWARE", E_NOT_STAGED to "E_NOT_STAGED",
        E_DB_INVALID to "E_DB_INVALID", E_DB_STALE to "E_DB_STALE",
    )

    fun errorName(code: Int): String = ERROR_NAMES[code] ?: "E_$code"

    // capabilities
    const val CAP_FILES = 1 shl 0; const val CAP_RESUME = 1 shl 1; const val CAP_DB_SNAPSHOT = 1 shl 2
    const val CAP_DB_STAGE = 1 shl 3; const val CAP_LIB_RESCAN = 1 shl 4; const val CAP_UPDATE = 1 shl 5
    const val CAP_PAIRING = 1 shl 6; const val CAP_ENCRYPTION = 1 shl 7; const val CAP_STATUS = 1 shl 8
    const val ALL_CAPS = 0x1ff

    // device HELLO flags
    const val HF_LOCAL = 1; const val HF_PAIRING_OPEN = 2; const val HF_SD_MOUNTED = 4; const val HF_SHELL_ATTACHED = 8

    // HELLO TLV tags
    const val TLV_SOFTWARE = 1; const val TLV_NAME = 2; const val TLV_DEVICE_ID = 3; const val TLV_BYOS_VERSION = 4
    const val TLV_FIRMWARE = 5; const val TLV_MODEL = 6

    // namespaces
    const val NS_SD = 0; const val NS_UPDATE = 1; const val NS_DB = 2

    // PUT modes
    const val MODE_CREATE = 0; const val MODE_REPLACE = 1; const val MODE_SKIP_IDENTICAL = 2

    // record nonce directions
    const val DIR_C2D = 1; const val DIR_D2C = 2
}

/** A protocol error raised by this side (malformed input) or reported by the peer (ERROR frame). */
open class BtpException(val code: Int, message: String, val remote: Boolean = false) : Exception(message) {
    val codeName: String get() = Btp.errorName(code)
}

/** The connection broke or closed (I/O error, EOF, record that does not open). */
open class BtpConnectionException(message: String, cause: Throwable? = null) : java.io.IOException(message, cause)

/** An encrypted record did not open: wrong keys (another peer) or tampering. The session is over. */
class BtpRecordAuthException(message: String) : BtpConnectionException(message)

class Frame(val type: Int, val stream: Int, val payload: ByteArray, val flags: Int = 0) {
    val end: Boolean get() = (flags and Btp.F_END) != 0

    fun encode(): ByteArray {
        require(payload.size <= Btp.MAX_PAYLOAD)
        val b = ByteArray(Btp.HEADER_LEN + payload.size)
        b[0] = 0x42; b[1] = 0x54; b[2] = Btp.VERSION.toByte(); b[3] = type.toByte(); b[4] = flags.toByte(); b[5] = 0
        b[6] = (stream shr 8).toByte(); b[7] = stream.toByte()
        val n = payload.size
        b[8] = (n ushr 24).toByte(); b[9] = (n ushr 16).toByte(); b[10] = (n ushr 8).toByte(); b[11] = n.toByte()
        System.arraycopy(payload, 0, b, Btp.HEADER_LEN, n)
        return b
    }

    override fun toString(): String = "${Btp.TYPE_NAMES[type] ?: "0x%02x".format(type)}(stream=$stream, ${payload.size} B${if (end) ", END" else ""})"

    companion object {
        class Header(val type: Int, val flags: Int, val stream: Int, val length: Int)

        /** Checks a 12-byte header (section 3.1). Throws [BtpException] with the error to answer. */
        fun parseHeader(b: ByteArray, off: Int = 0, maxPayload: Int = Btp.MAX_PAYLOAD): Header {
            if (b.size - off < Btp.HEADER_LEN) throw BtpException(Btp.E_PROTO, "short header")
            if (b[off] != 0x42.toByte() || b[off + 1] != 0x54.toByte()) throw BtpException(Btp.E_PROTO, "bad magic")
            if (b[off + 2].toInt() != Btp.VERSION) throw BtpException(Btp.E_VERSION, "unsupported version ${b[off + 2]}")
            val flags = b[off + 4].toInt() and 0xff
            if (flags and Btp.F_END.inv() != 0) throw BtpException(Btp.E_PROTO, "reserved flag bits set")
            if (b[off + 5].toInt() != 0) throw BtpException(Btp.E_PROTO, "reserved byte set")
            val len = ((b[off + 8].toLong() and 0xff) shl 24) or ((b[off + 9].toLong() and 0xff) shl 16) or
                ((b[off + 10].toLong() and 0xff) shl 8) or (b[off + 11].toLong() and 0xff)
            if (len > maxPayload) throw BtpException(Btp.E_TOO_LARGE, "frame of $len bytes")
            val stream = ((b[off + 6].toInt() and 0xff) shl 8) or (b[off + 7].toInt() and 0xff)
            return Header(b[off + 3].toInt() and 0xff, flags, stream, len.toInt())
        }

        /** Decodes one complete frame (header and exactly its payload). */
        fun decode(b: ByteArray): Frame {
            val h = parseHeader(b)
            if (b.size != Btp.HEADER_LEN + h.length) throw BtpException(Btp.E_PROTO, "frame length mismatch")
            return Frame(h.type, h.stream, b.copyOfRange(Btp.HEADER_LEN, b.size), h.flags)
        }
    }
}

/** Big-endian payload writer. */
class PayloadWriter {
    private val out = ByteArrayOutputStream(64)
    fun u8(v: Int) = apply { out.write(v and 0xff) }
    fun u16(v: Int) = apply { out.write((v shr 8) and 0xff); out.write(v and 0xff) }
    fun u32(v: Long) = apply { for (i in 3 downTo 0) out.write(((v ushr (8 * i)) and 0xff).toInt()) }
    fun u64(v: Long) = apply { for (i in 7 downTo 0) out.write(((v ushr (8 * i)) and 0xff).toInt()) }
    fun i64(v: Long) = u64(v)
    fun bytes(b: ByteArray) = apply { out.write(b, 0, b.size) }
    fun bytes(b: ByteArray, off: Int, len: Int) = apply { out.write(b, off, len) }
    fun str16(s: String) = str16(s.toByteArray(Charsets.UTF_8))
    fun str16(b: ByteArray) = apply { require(b.size <= 0xffff); u16(b.size); bytes(b) }
    fun str8(s: String) = apply { val b = s.toByteArray(Charsets.UTF_8); require(b.size <= 255); u8(b.size); bytes(b) }
    fun tlv(tag: Int, value: ByteArray) = apply { u8(tag); u16(value.size); bytes(value) }
    fun tlv(tag: Int, value: String) = tlv(tag, value.toByteArray(Charsets.UTF_8))
    fun build(): ByteArray = out.toByteArray()
}

/** Big-endian payload reader; any shortage or excess is E_PROTO. */
class PayloadReader(private val b: ByteArray, private var p: Int = 0, private val end: Int = b.size) {
    val remaining: Int get() = end - p
    private fun need(n: Int) { if (n < 0 || p + n > end) throw BtpException(Btp.E_PROTO, "payload too short") }
    fun u8(): Int { need(1); return b[p++].toInt() and 0xff }
    fun u16(): Int { need(2); val v = ((b[p].toInt() and 0xff) shl 8) or (b[p + 1].toInt() and 0xff); p += 2; return v }
    fun u32(): Long { need(4); var v = 0L; repeat(4) { v = (v shl 8) or (b[p++].toLong() and 0xff) }; return v }
    fun u64(): Long { need(8); var v = 0L; repeat(8) { v = (v shl 8) or (b[p++].toLong() and 0xff) }; return v }
    fun i64(): Long = u64()
    fun bytes(n: Int): ByteArray { need(n); val r = b.copyOfRange(p, p + n); p += n; return r }
    fun rest(): ByteArray = bytes(remaining)
    fun str16(): String = utf8(bytes(u16()))
    fun str8(): String = utf8(bytes(u8()))
    fun done() { if (p != end) throw BtpException(Btp.E_PROTO, "payload too long") }

    /** TLVs until the end: tag -> value (the last one wins). */
    fun tlvs(): Map<Int, ByteArray> {
        val m = LinkedHashMap<Int, ByteArray>()
        while (remaining > 0) {
            val t = u8()
            m[t] = bytes(u16())
        }
        return m
    }

    companion object {
        fun utf8(b: ByteArray): String = try {
            Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(b)).toString()
        } catch (e: CharacterCodingException) {
            throw BtpException(Btp.E_PROTO, "invalid UTF-8")
        }
    }
}

// ---------------------------------------------------------------------------------------- messages

class Hello(
    val protoMin: Int = 1, val protoMax: Int = 1, val role: Int, val flags: Int = 0,
    val maxPayload: Long = Btp.MAX_PAYLOAD.toLong(), val caps: Int,
    /** TLVs in the order they are sent. */
    val tlvs: List<Pair<Int, ByteArray>> = emptyList(),
) {
    fun tlv(tag: Int): ByteArray? = tlvs.lastOrNull { it.first == tag }?.second
    fun text(tag: Int): String? = tlv(tag)?.let { String(it, Charsets.UTF_8) }
    val software get() = text(Btp.TLV_SOFTWARE)
    val name get() = text(Btp.TLV_NAME)
    val deviceId get() = tlv(Btp.TLV_DEVICE_ID)
    val byosVersion get() = text(Btp.TLV_BYOS_VERSION)
    val firmware get() = text(Btp.TLV_FIRMWARE)
    val model get() = text(Btp.TLV_MODEL)

    fun encode(): ByteArray {
        val w = PayloadWriter().u8(protoMin).u8(protoMax).u8(role).u8(flags).u32(maxPayload).u32(caps.toLong())
        for ((t, v) in tlvs) w.tlv(t, v)
        return w.build()
    }

    companion object {
        fun decode(p: ByteArray): Hello {
            val r = PayloadReader(p)
            val h = Hello(r.u8(), r.u8(), r.u8(), r.u8(), r.u32(), r.u32().toInt(), run {
                val l = ArrayList<Pair<Int, ByteArray>>()
                while (r.remaining > 0) { val t = r.u8(); l += t to r.bytes(r.u16()) }
                l
            })
            return h
        }

        fun client(software: String, name: String, caps: Int = Btp.ALL_CAPS): Hello =
            Hello(role = 0, caps = caps, tlvs = listOf(
                Btp.TLV_SOFTWARE to software.toByteArray(Charsets.UTF_8),
                Btp.TLV_NAME to BtpPaths.clipUtf8(name, Btp.MAX_NAME).toByteArray(Charsets.UTF_8)))
    }
}

class ErrorMsg(val code: Int, val message: String) {
    fun encode() = PayloadWriter().u16(code).str16(BtpPaths.clipUtf8(message, 200)).build()
    companion object {
        fun decode(p: ByteArray): ErrorMsg { val r = PayloadReader(p); val e = ErrorMsg(r.u16(), r.str16()); r.done(); return e }
    }
}

class StatR(val kind: Int, val size: Long, val mtime: Long) {
    fun encode() = PayloadWriter().u8(kind).u64(size).i64(mtime).build()
    companion object {
        fun decode(p: ByteArray): StatR { val r = PayloadReader(p); val s = StatR(r.u8(), r.u64(), r.i64()); r.done(); return s }
    }
}

class ListEntry(val kind: Int, val size: Long, val mtime: Long, val name: String) {
    val isDir get() = kind == 1
}

object ListR {
    fun encode(entries: List<ListEntry>): ByteArray {
        val w = PayloadWriter().u16(entries.size)
        for (e in entries) w.u8(e.kind).u64(e.size).i64(e.mtime).str16(e.name)
        return w.build()
    }

    fun entrySize(e: ListEntry) = 1 + 8 + 8 + 2 + e.name.toByteArray(Charsets.UTF_8).size

    fun decode(p: ByteArray): List<ListEntry> {
        val r = PayloadReader(p)
        val n = r.u16()
        val l = ArrayList<ListEntry>(n)
        repeat(n) {
            val e = ListEntry(r.u8(), r.u64(), r.i64(), r.str16())
            // a name is one path component: never "..", a separator or a drive (a client turns it into a local file name)
            BtpPaths.checkName(e.name)?.let { throw BtpException(Btp.E_PROTO, "bad name in LIST_R ($it)") }
            l += e
        }
        r.done()
        return l
    }
}

class DfR(val sdState: Int, val sdFs: Int, val sdTotal: Long, val sdFree: Long, val dataTotal: Long, val dataFree: Long) {
    fun encode() = PayloadWriter().u8(sdState).u8(sdFs).u64(sdTotal).u64(sdFree).u64(dataTotal).u64(dataFree).build()
    companion object {
        fun decode(p: ByteArray): DfR {
            val r = PayloadReader(p)
            val d = DfR(r.u8(), r.u8(), r.u64(), r.u64(), r.u64(), r.u64()); r.done(); return d
        }
        val SD_STATES = listOf("mounted", "no_card", "usb_storage", "read_only")
        val SD_FS = listOf("unknown", "vfat", "exfat")
    }
}

class PutBegin(val ns: Int, val mode: Int, val size: Long, val mtime: Long, val sha256: ByteArray, val path: String) {
    fun encode() = PayloadWriter().u8(ns).u8(mode).u64(size).i64(mtime).bytes(sha256).str16(path).build()
    companion object {
        fun decode(p: ByteArray): PutBegin {
            val r = PayloadReader(p)
            val v = PutBegin(r.u8(), r.u8(), r.u64(), r.i64(), r.bytes(32), r.str16()); r.done(); return v
        }
    }
}

class DbStatusR(val staged: Int, val stagedSha256: ByteArray, val liveSha256: ByteArray, val liveSize: Long,
                val lastApply: Int, val dataFree: Long) {
    fun encode() = PayloadWriter().u8(staged).bytes(stagedSha256).bytes(liveSha256).u64(liveSize).u8(lastApply).u64(dataFree).build()
    companion object {
        fun decode(p: ByteArray): DbStatusR {
            val r = PayloadReader(p)
            val v = DbStatusR(r.u8(), r.bytes(32), r.bytes(32), r.u64(), r.u8(), r.u64()); r.done(); return v
        }
        val LAST_APPLY = listOf("never", "applied", "rejected_integrity", "rejected_space", "rejected_other")
    }
}

class UpdateStageR(val version: String, val restart: Int, val needsFirmware: Int, val size: Long) {
    fun encode() = PayloadWriter().str16(version).u8(restart).u8(needsFirmware).u64(size).build()
    companion object {
        fun decode(p: ByteArray): UpdateStageR {
            val r = PayloadReader(p)
            val v = UpdateStageR(r.str16(), r.u8(), r.u8(), r.u64()); r.done(); return v
        }
        val RESTART = listOf("none", "shell", "reboot")
    }
}

class UpdateStatusR(val state: Int, val permille: Int, val error: Int, val version: String) {
    fun encode() = PayloadWriter().u8(state).u16(permille).u16(error).str16(version).build()
    companion object {
        fun decode(p: ByteArray): UpdateStatusR {
            val r = PayloadReader(p)
            val v = UpdateStatusR(r.u8(), r.u16(), r.u16(), r.str16()); r.done(); return v
        }
        val STATES = listOf("idle", "staged", "waiting_user", "installing", "restart_pending", "failed", "refused")
    }
}

class StatusR(val battery: Int, val charging: Int, val sdState: Int, val tlvs: List<Pair<Int, ByteArray>>) {
    fun text(tag: Int): String? = tlvs.lastOrNull { it.first == tag }?.second?.let { String(it, Charsets.UTF_8) }
    fun encode(): ByteArray {
        val w = PayloadWriter().u8(battery).u8(charging).u8(sdState)
        for ((t, v) in tlvs) w.tlv(t, v)
        return w.build()
    }
    companion object {
        fun decode(p: ByteArray): StatusR {
            val r = PayloadReader(p)
            val b = r.u8(); val c = r.u8(); val s = r.u8()
            val l = ArrayList<Pair<Int, ByteArray>>()
            while (r.remaining > 0) { val t = r.u8(); l += t to r.bytes(r.u16()) }
            return StatusR(b, c, s, l)
        }
    }
}

/** Simple request payloads. */
object Payloads {
    fun nsPath(ns: Int, path: String) = PayloadWriter().u8(ns).str16(path).build()
    fun nsFlagsPath(ns: Int, flags: Int, path: String) = PayloadWriter().u8(ns).u8(flags).str16(path).build()
    fun move(ns: Int, flags: Int, src: String, dst: String) = PayloadWriter().u8(ns).u8(flags).str16(src).str16(dst).build()
    fun u64(v: Long) = PayloadWriter().u64(v).build()
    fun data(offset: Long, b: ByteArray, off: Int, len: Int) = PayloadWriter().u64(offset).bytes(b, off, len).build()
    fun putDone(result: Int, size: Long) = PayloadWriter().u8(result).u64(size).build()
    fun get(ns: Int, flags: Int, offset: Long, path: String) = PayloadWriter().u8(ns).u8(flags).u64(offset).str16(path).build()
    fun getR(size: Long, mtime: Long, offset: Long) = PayloadWriter().u64(size).i64(mtime).u64(offset).build()
    fun dbStage(flags: Int, base: ByteArray) = PayloadWriter().u8(flags).bytes(base).build()
    fun libRescan(mode: Int, paths: List<String>) =
        PayloadWriter().u8(mode).u16(paths.size).apply { paths.forEach { str16(it) } }.build()
    fun flags(f: Int) = PayloadWriter().u8(f).build()
}

// ---------------------------------------------------------------------------------------- paths

/** Path rules of section 9.2, the PUT part-file name (11.2) and the resume rule. */
object BtpPaths {
    const val MAX_PATH = 1024
    const val MAX_COMPONENT = 255
    const val MAX_DEPTH = 32
    private const val FORBIDDEN = "\\:*?\"<>|"

    /** null when [p] is valid, else the reason name used by the spec and the vectors. */
    fun check(p: ByteArray, allowRoot: Boolean = false): String? {
        if (p.isEmpty()) return if (allowRoot) null else "empty"
        if (p.size > MAX_PATH) return "too_long"
        val s = try {
            PayloadReader.utf8(p)
        } catch (e: BtpException) {
            return "bad_utf8"
        }
        if (s.startsWith("/")) return "absolute"
        val comps = s.split('/')
        if (comps.size > MAX_DEPTH) return "too_deep"
        for (c in comps) {
            if (c.isEmpty()) return "empty_component"
            if (c == "." || c == "..") return "dot_component"
            if (c.startsWith(".")) return "hidden"
            for (ch in c) {
                if (ch.code < 0x20 || ch.code == 0x7f) return "control_char"
                if (FORBIDDEN.indexOf(ch) >= 0) return "forbidden_char"
            }
            if (c.endsWith(" ") || c.endsWith(".")) return "trailing_dot_space"
            if (c.toByteArray(Charsets.UTF_8).size > MAX_COMPONENT) return "component_too_long"
        }
        return null
    }

    fun check(p: String, allowRoot: Boolean = false): String? = check(p.toByteArray(Charsets.UTF_8), allowRoot)

    /**
     * null when [name] is a valid single entry name as LIST_R returns it, else the reason. Same
     * rules as one component of [check], except that hidden names (a leading dot) are allowed,
     * because LIST shows them on request.
     */
    fun checkName(name: String): String? {
        if (name.isEmpty()) return "empty_component"
        if (name.contains('/') || name.contains('\\')) return "separator"
        val rest = name.trimStart('.')
        if (rest.isEmpty()) return "dot_component"
        return check(rest)
    }

    /** Unicode NFC, as the device and the stock database expect. */
    fun nfc(s: String): String = Normalizer.normalize(s, Normalizer.Form.NFC)

    /** Joins device path parts with "/", ignoring empty parts. */
    fun join(vararg parts: String): String = parts.flatMap { it.split('/') }.filter { it.isNotEmpty() }.joinToString("/")

    /** `<parent>/.btp-<id>.part` for a PUT of [path] with this content. */
    fun partFile(path: String, sha256: ByteArray, size: Long): String {
        val pb = path.toByteArray(Charsets.UTF_8)
        val id = BtpCrypto.sha256(pb, byteArrayOf(0), sha256, PayloadWriter().u64(size).build()).copyOf(8)
        val name = ".btp-" + id.joinToString("") { "%02x".format(it) } + ".part"
        val slash = path.lastIndexOf('/')
        return if (slash < 0) name else path.substring(0, slash + 1) + name
    }

    /** The durable resume offset: whole 64 KiB blocks of the part, capped at the file size. */
    fun resumeOffset(partSize: Long, size: Long): Long = minOf((partSize / 65536) * 65536, size)

    /** Cuts a string to at most [max] UTF-8 bytes without splitting a character. */
    fun clipUtf8(s: String, max: Int): String {
        if (s.toByteArray(Charsets.UTF_8).size <= max) return s
        val sb = StringBuilder()
        var n = 0
        var i = 0
        while (i < s.length) {
            val cp = s.codePointAt(i)
            val len = String(Character.toChars(cp)).toByteArray(Charsets.UTF_8).size
            if (n + len > max) break
            sb.appendCodePoint(cp)
            n += len
            i += Character.charCount(cp)
        }
        return sb.toString()
    }
}

// ---------------------------------------------------------------------------------------- discovery packets

class Announce(val flags: Int, val nonce: ByteArray, val tcpPort: Int, val deviceId: ByteArray, val name: String) {
    val pairingOpen get() = flags and 1 != 0
    val sdMounted get() = flags and 2 != 0
    val hasPeers get() = flags and 4 != 0

    fun encode(): ByteArray = PayloadWriter().bytes(MAGIC).u8(1).u8(2).u16(flags).bytes(nonce).u16(tcpPort)
        .bytes(deviceId).str8(BtpPaths.clipUtf8(name, 32)).build()

    companion object {
        val MAGIC = "BTPD".toByteArray(Charsets.US_ASCII)

        fun probe(nonce: ByteArray): ByteArray {
            require(nonce.size == 8)
            return PayloadWriter().bytes(MAGIC).u8(1).u8(1).u16(0).bytes(nonce).build()
        }

        /** The probe nonce, or null when [b] is not a valid probe. */
        fun parseProbe(b: ByteArray, len: Int = b.size): ByteArray? {
            if (len != 16) return null
            for (i in 0 until 4) if (b[i] != MAGIC[i]) return null
            if (b[4].toInt() != 1 || b[5].toInt() != 1) return null
            return b.copyOfRange(8, 16)
        }

        /** Parses an ANNOUNCE, or returns null. */
        fun parse(b: ByteArray, len: Int = b.size): Announce? = try {
            val r = PayloadReader(b, 0, len)
            if (!r.bytes(4).contentEquals(MAGIC) || r.u8() != 1 || r.u8() != 2) null
            else {
                val a = Announce(r.u16(), r.bytes(8), r.u16(), r.bytes(8), r.str8())
                r.done()
                a
            }
        } catch (e: BtpException) {
            null
        }
    }
}
