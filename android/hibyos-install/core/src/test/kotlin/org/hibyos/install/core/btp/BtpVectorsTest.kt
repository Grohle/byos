package org.hibyos.install.core.btp

import org.hibyos.install.core.Json
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.File
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.atomic.AtomicReference

/**
 * Checks the Kotlin client against tests/btp/vectors.json (written by
 * tools/btp_test_vectors.py): primitives, every frame, paths, part files, discovery, and the
 * whole pairing and AUTH exchanges byte for byte against a scripted device.
 */
class BtpVectorsTest {
    companion object {
        fun repoFile(rel: String): File {
            System.getenv("HIBYOS_REPO")?.let { val f = File(it, rel); if (f.exists()) return f }
            var d: File? = File(System.getProperty("user.dir")).absoluteFile
            while (d != null) {
                val f = File(d, rel)
                if (f.exists()) return f
                d = d.parentFile
            }
            throw AssertionError("$rel not found (set HIBYOS_REPO)")
        }

        val V: Map<String, Any?> by lazy { Json.obj(Json.parse(repoFile("tests/btp/vectors.json").readText())) }

        fun hex(s: String): ByteArray = ByteArray(s.length / 2) { s.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
        fun hx(b: ByteArray) = b.joinToString("") { "%02x".format(it) }
        fun o(v: Any?) = Json.obj(v)
        fun a(v: Any?) = Json.arr(v)
        fun s(m: Map<String, Any?>, k: String) = Json.str(m, k)
        fun l(m: Map<String, Any?>, k: String) = Json.long(m, k)
        fun i(m: Map<String, Any?>, k: String) = Json.long(m, k).toInt()
    }

    @Test
    fun primitives() {
        val p = o(V["primitives"])
        for (x in a(p["x25519"]).map(::o)) {
            assertEquals(s(x, "out"), hx(BtpCrypto.x25519(hex(s(x, "scalar")), hex(s(x, "u")))))
        }
        for (x in a(p["hsalsa20"]).map(::o)) {
            assertEquals(s(x, "out"), hx(BtpCrypto.hsalsa20(hex(s(x, "key")), hex(s(x, "in")))))
        }
        for (x in a(p["poly1305"]).map(::o)) {
            assertEquals(s(x, "tag"), hx(BtpCrypto.poly1305(hex(s(x, "key")), hex(s(x, "msg")))))
        }
        for (x in a(p["secretbox"]).map(::o)) {
            val key = hex(s(x, "key")); val n = hex(s(x, "nonce")); val m = hex(s(x, "msg"))
            val box = BtpCrypto.secretbox(m, n, key)
            assertEquals(s(x, "box"), hx(box))
            assertArrayEquals(m, BtpCrypto.secretboxOpen(box, n, key))
            val bad = box.copyOf(); bad[bad.size - 1] = (bad[bad.size - 1].toInt() xor 1).toByte()
            assertNull(BtpCrypto.secretboxOpen(bad, n, key))
        }
        for (x in a(p["hkdf_sha256"]).map(::o)) {
            val prk = BtpCrypto.hkdfExtract(hex(s(x, "salt")), hex(s(x, "ikm")))
            assertEquals(s(x, "okm"), hx(BtpCrypto.hkdfExpand(prk, hex(s(x, "info")), i(x, "len"))))
        }
        // RFC 7748 6.1 Diffie-Hellman example, as an extra check of the base point
        val alice = hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
        assertEquals("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", hx(BtpCrypto.x25519Base(alice)))
    }

    @Test
    fun poly1305AgreesWithBigIntegerReference() {
        // random lengths and keys against a straightforward BigInteger implementation
        val r = java.util.Random(7)
        val p = java.math.BigInteger.ONE.shiftLeft(130).subtract(java.math.BigInteger.valueOf(5))
        repeat(200) {
            val key = ByteArray(32).also(r::nextBytes)
            val msg = ByteArray(r.nextInt(300)).also(r::nextBytes)
            fun le(b: ByteArray) = java.math.BigInteger(1, b.reversedArray())
            val rr = le(key.copyOf(16)).and(java.math.BigInteger("0ffffffc0ffffffc0ffffffc0fffffff", 16))
            val ss = le(key.copyOfRange(16, 32))
            var acc = java.math.BigInteger.ZERO
            var q = 0
            while (q < msg.size) {
                val blk = msg.copyOfRange(q, minOf(q + 16, msg.size)) + byteArrayOf(1)
                acc = acc.add(le(blk)).multiply(rr).mod(p)
                q += 16
            }
            val tag = acc.add(ss).mod(java.math.BigInteger.ONE.shiftLeft(128)).toByteArray().reversedArray().copyOf(16)
            assertEquals(hx(tag), hx(BtpCrypto.poly1305(key, msg)))
        }
    }

    @Test
    fun x25519SharedSecretsAgree() {
        val r = java.util.Random(11)
        repeat(20) {
            val a = ByteArray(32).also(r::nextBytes); val b = ByteArray(32).also(r::nextBytes)
            assertArrayEquals(BtpCrypto.x25519(a, BtpCrypto.x25519Base(b)), BtpCrypto.x25519(b, BtpCrypto.x25519Base(a)))
        }
    }

    private fun payloadFromFields(type: Int, f: Map<String, Any?>, payload: ByteArray): ByteArray? = when (type) {
        Btp.ERROR -> ErrorMsg(i(f, "code"), s(f, "message")).encode()
        Btp.STAT -> Payloads.nsPath(i(f, "ns"), s(f, "path"))
        Btp.STAT_R -> StatR(i(f, "kind"), l(f, "size"), l(f, "mtime")).encode()
        Btp.LIST -> Payloads.nsFlagsPath(i(f, "ns"), i(f, "flags"), s(f, "path"))
        Btp.LIST_R -> ListR.encode(a(f["entries"]).map(::o).map { ListEntry(i(it, "kind"), l(it, "size"), l(it, "mtime"), s(it, "name")) })
        Btp.DF_R -> DfR(i(f, "sd_state"), i(f, "sd_fs"), l(f, "sd_total"), l(f, "sd_free"), l(f, "data_total"), l(f, "data_free")).encode()
        Btp.MOVE -> Payloads.move(i(f, "ns"), i(f, "flags"), s(f, "src"), s(f, "dst"))
        Btp.PUT_BEGIN -> PutBegin(i(f, "ns"), i(f, "mode"), l(f, "size"), l(f, "mtime"), hex(s(f, "sha256")), s(f, "path")).encode()
        Btp.PUT_READY -> Payloads.u64(l(f, "offset"))
        Btp.PUT_DONE -> Payloads.putDone(i(f, "result"), l(f, "size"))
        Btp.GET_R -> Payloads.getR(l(f, "size"), l(f, "mtime"), l(f, "offset"))
        Btp.GET_END -> hex(s(f, "sha256"))
        Btp.DB_STAGE -> Payloads.dbStage(i(f, "flags"), hex(s(f, "base_sha256")))
        Btp.DB_STATUS_R -> DbStatusR(i(f, "staged"), hex(s(f, "staged_sha256")), hex(s(f, "live_sha256")), l(f, "live_size"),
            i(f, "last_apply"), l(f, "data_free")).encode()
        Btp.LIB_RESCAN -> Payloads.libRescan(i(f, "mode"), a(f["paths"]).map { it as String })
        Btp.UPDATE_STAGE, Btp.UPDATE_INSTALL -> Payloads.flags(i(f, "flags"))
        Btp.UPDATE_STAGE_R -> UpdateStageR(s(f, "version"), i(f, "restart"), i(f, "needs_firmware"), l(f, "size")).encode()
        Btp.UPDATE_STATUS_R -> UpdateStatusR(i(f, "state"), i(f, "permille"), i(f, "error"), s(f, "version")).encode()
        Btp.STATUS_R -> StatusR(i(f, "battery"), i(f, "charging"), i(f, "sd_state"), listOf(
            Btp.TLV_BYOS_VERSION to s(f, "byos_version").toByteArray(), Btp.TLV_FIRMWARE to s(f, "firmware").toByteArray())).encode()
        Btp.DATA -> payload.copyOf(8).also { assertEquals(l(f, "offset"), PayloadReader(it).u64()) }.let {
            assertEquals(i(f, "len"), payload.size - 8); null
        }
        Btp.MKDIR, Btp.DELETE -> { val r = PayloadReader(payload); assertEquals(i(f, "ns"), r.u8()); assertEquals(i(f, "flags"), r.u8()); r.str16(); r.done(); null }
        Btp.GET -> { val r = PayloadReader(payload); assertEquals(i(f, "ns"), r.u8()); assertEquals(i(f, "flags"), r.u8()); assertEquals(l(f, "offset"), r.u64()); r.str16(); r.done(); null }
        else -> null
    }

    @Test
    fun frames() {
        val types = o(o(V["constants"])["types"])
        for ((name, code) in types) assertEquals(name, Btp.TYPE_NAMES[(code as Long).toInt()])
        for ((name, code) in o(o(V["constants"])["errors"])) assertEquals(name, Btp.errorName((code as Long).toInt()))
        var checked = 0
        for (fv in a(V["frames"]).map(::o)) {
            val type = i(fv, "type_code")
            val fields = o(fv["fields"])
            if (fv["frame"] == null) {
                // the full 64 KiB DATA frame is given by its hash
                val content = ByteArray(65536) { ((it * 7 + 3) and 0xff).toByte() }
                val fr = Frame(type, i(fv, "stream"), Payloads.data(0, content, 0, content.size), i(fv, "flags")).encode()
                assertEquals(l(fv, "frame_len").toInt(), fr.size)
                assertEquals(s(fv, "frame_sha256"), hx(BtpCrypto.sha256(fr)))
                assertTrue(hx(fr).startsWith(s(fv, "frame_head")))
                val whole = ByteArray(70000) { ((it * 7 + 3) and 0xff).toByte() }
                assertEquals(s(fields, "file_sha256"), hx(BtpCrypto.sha256(whole)))
                checked++
                continue
            }
            val bytes = hex(s(fv, "frame"))
            val f = Frame.decode(bytes)
            assertEquals(s(fv, "name"), type, f.type)
            assertEquals(i(fv, "stream"), f.stream)
            assertEquals(i(fv, "flags"), f.flags)
            assertEquals(s(fv, "payload"), hx(f.payload))
            assertArrayEquals(bytes, Frame(type, f.stream, f.payload, f.flags).encode())
            if (type == Btp.HELLO) {
                val h = Hello.decode(f.payload)
                if (h.role == 0) {
                    assertEquals(s(fv, "payload"), hx(Hello.client(s(fields, "software"), s(fields, "name")).encode()))
                } else {
                    assertEquals(i(fields, "flags"), h.flags)
                    assertEquals(s(fields, "device_id"), hx(h.deviceId!!))
                    assertEquals(f.payload.toList(), h.encode().toList())
                }
            } else if (fields.isNotEmpty()) {
                val built = payloadFromFields(type, fields, f.payload)
                if (built != null) assertEquals(s(fv, "name"), s(fv, "payload"), hx(built))
            }
            checked++
        }
        assertTrue(checked >= 38)
    }

    @Test
    fun invalidFrames() {
        for (x in a(V["invalid_frames"]).map(::o)) {
            val b = hex(Json.optStr(x, "frame") ?: s(x, "header"))
            val expected = s(x, "error")
            val got = try {
                val h = Frame.parseHeader(b)
                if (Btp.TYPE_NAMES.containsKey(h.type)) "OK" else "E_UNKNOWN_TYPE"
            } catch (e: BtpException) {
                e.codeName
            }
            assertEquals(s(x, "name"), expected, got)
        }
    }

    @Test
    fun paths() {
        val p = o(V["paths"])
        for (x in a(p["valid"]).map(::o)) assertNull(s(x, "path_hex"), BtpPaths.check(hex(s(x, "path_hex"))))
        for (x in a(p["invalid"]).map(::o)) assertEquals(s(x, "path_hex"), s(x, "reason"), BtpPaths.check(hex(s(x, "path_hex"))))
        assertNull(BtpPaths.check(ByteArray(0), allowRoot = true))
        for (x in a(V["part_files"]).map(::o)) {
            assertEquals(s(x, "part"), BtpPaths.partFile(s(x, "path"), hex(s(x, "sha256")), l(x, "size")))
        }
        for (x in a(V["resume_offsets"]).map(::o)) {
            assertEquals(l(x, "offset"), BtpPaths.resumeOffset(l(x, "part_size"), l(x, "size")))
        }
    }

    @Test
    fun discovery() {
        val d = a(V["discovery"]).map(::o)
        val probe = d[0]
        val nonce = hex(s(o(probe["fields"]), "nonce"))
        assertEquals(s(probe, "packet"), hx(Announce.probe(nonce)))
        assertArrayEquals(nonce, Announce.parseProbe(hex(s(probe, "packet"))))
        for (x in d.drop(1)) {
            val pk = hex(s(x, "packet"))
            val an = Announce.parse(pk)
            assertNotNull(an)
            assertEquals(i(o(x["fields"]), "flags"), an!!.flags)
            assertEquals(s(x, "packet"), hx(an.encode()))
        }
        val ann = Announce.parse(hex(s(d[1], "packet")))!!
        assertEquals("HiBy R3 II", ann.name)
        assertEquals(7340, ann.tcpPort)
        assertTrue(ann.pairingOpen && ann.sdMounted && !ann.hasPeers)
    }

    @Test
    fun sasExamples() {
        for (x in a(V["sas_examples"]).map(::o)) assertEquals(s(x, "sas"), BtpSession.sasFrom(hex(s(x, "prk"))))
    }

    /**
     * Plays the device side of [section] from the vectors over a real socket: every byte the
     * client sends must be the expected one; the device bytes are sent verbatim.
     * [steps] is the ordered list of (dir, bytes).
     */
    private fun scriptedDevice(steps: List<Pair<String, ByteArray>>): Pair<ServerSocket, AtomicReference<Throwable?>> {
        val server = ServerSocket(0)
        val err = AtomicReference<Throwable?>(null)
        Thread {
            try {
                server.accept().use { s ->
                    val inp = DataInputStream(s.getInputStream())
                    val out = s.getOutputStream()
                    for ((idx, st) in steps.withIndex()) {
                        if (st.first == "c2d") {
                            val got = ByteArray(st.second.size)
                            inp.readFully(got)
                            if (!got.contentEquals(st.second)) throw AssertionError("step $idx: client sent ${hx(got)}, expected ${hx(st.second)}")
                        } else {
                            out.write(st.second); out.flush()
                        }
                    }
                    // the client ends with BYE (encrypted); just drain it
                    s.soTimeout = 2000
                    try { while (inp.read() >= 0) {} } catch (e: Exception) {}
                }
            } catch (t: Throwable) {
                err.set(t)
            } finally {
                server.close()
            }
        }.apply { isDaemon = true; start() }
        return server to err
    }

    private fun steps(sec: Map<String, Any?>): List<Pair<String, ByteArray>> {
        val l = ArrayList<Pair<String, ByteArray>>()
        for (f in a(sec["clear_frames"]).map(::o)) l += s(f, "dir") to hex(s(f, "frame"))
        // encrypted records in protocol order: c2d and d2c interleave by counter as the exchange runs
        for (f in a(sec["encrypted"]).map(::o)) l += s(f, "dir") to hex(s(f, "record"))
        return l
    }

    @Test
    fun pairingTranscript() {
        val p = o(V["pairing"])
        val (server, err) = scriptedDevice(steps(p))
        val rnd = ArrayDeque(listOf(hex(s(p, "client_ephemeral_secret")), hex(s(p, "client_nonce"))))
        var shownTo: String? = null
        val sess = BtpSession.connect("127.0.0.1", server.localPort, Hello.client("byos-cli 0.1.0", "Desktop"), 5000) { n ->
            val b = rnd.removeFirst(); assertEquals(n, b.size); b
        }
        assertTrue(sess.pairingOpen)
        assertEquals("0acef168ac63fe0f", hx(sess.device.deviceId!!))
        val dev = sess.pair(hex(s(p, "client_static_secret")), "Desktop", { name -> shownTo = name; s(p, "sas") })
        assertEquals("HiBy R3 II", shownTo)
        assertEquals(s(p, "device_id"), hx(dev.deviceId))
        assertEquals(s(p, "device_static_public"), hx(dev.publicKey))
        assertEquals("HiBy R3 II", dev.name)
        val df = sess.df()
        assertEquals(127865454592L, df.sdTotal)
        assertEquals(2, df.sdFs)
        sess.close()
        Thread.sleep(200)
        err.get()?.let { throw it }
    }

    @Test
    fun pairingWrongCodeStops() {
        val p = o(V["pairing"])
        val clear = steps(p).take(5) // up to PAIR_REVEAL
        val (server, err) = scriptedDevice(clear)
        val rnd = ArrayDeque(listOf(hex(s(p, "client_ephemeral_secret")), hex(s(p, "client_nonce"))))
        val sess = BtpSession.connect("127.0.0.1", server.localPort, Hello.client("byos-cli 0.1.0", "Desktop"), 5000) { rnd.removeFirst() }
        try {
            sess.pair(hex(s(p, "client_static_secret")), "Desktop", { "123456" })
            fail("a wrong code must stop pairing")
        } catch (e: BtpAuthException) {
            assertEquals(BtpAuthException.Reason.CODE_MISMATCH, e.reason)
        }
        sess.close()
        Thread.sleep(100)
        err.get()?.let { throw it }
    }

    @Test
    fun authTranscriptAndEncryptedPut() {
        val p = o(V["auth"])
        val (server, err) = scriptedDevice(steps(p))
        val rnd = ArrayDeque(listOf(hex(s(p, "client_ephemeral_secret"))))
        val sess = BtpSession.connect("127.0.0.1", server.localPort, Hello.client("byos-cli 0.1.0", "Desktop"), 5000) { rnd.removeFirst() }
        val ds = hex(s(p, "device_static_public"))
        sess.auth(hex(s(p, "client_static_secret")), PairedDevice(BtpCrypto.sha256(ds).copyOf(8), ds, "HiBy R3 II"))
        assertTrue(sess.encrypted)
        val content = "hello world\n".toByteArray()
        val out = sess.put("Docs/hello.txt", content.size.toLong(), BtpCrypto.sha256(content), Btp.MODE_CREATE, 1767225600) {
            ByteArrayInputStream(content, it.toInt(), content.size - it.toInt())
        }
        assertEquals(0, out.result)
        assertEquals(12L, out.size)
        sess.close()
        Thread.sleep(200)
        err.get()?.let { throw it }
    }

    @Test
    fun authRefusesAnotherDevice() {
        val p = o(V["auth"])
        val (server, _) = scriptedDevice(steps(p).take(2))
        val sess = BtpSession.connect("127.0.0.1", server.localPort, Hello.client("byos-cli 0.1.0", "Desktop"), 5000)
        try {
            sess.auth(hex(s(p, "client_static_secret")), PairedDevice(ByteArray(8), ByteArray(32), "x"))
            fail()
        } catch (e: BtpAuthException) {
            assertEquals(BtpAuthException.Reason.DIFFERENT_DEVICE, e.reason)
        }
        sess.close()
    }

    @Test
    fun tamperedRecordIsRejected() {
        val p = o(V["pairing"])
        val enc = a(p["encrypted"]).map(::o)
        val first = hex(s(enc[0], "record")) // c2d #0
        val tampered = hex(s(o(p["tampered_record"]), "record")) // c2d #1, last byte flipped
        val good = hex(s(enc[2], "record")) // c2d #1 untouched
        val k = hex(s(p, "k_c2d")); val kd = hex(s(p, "k_d2c"))
        // the device side receives c2d records
        val dev = FrameChannel(ByteArrayInputStream(first + tampered), ByteArrayOutputStream(), Btp.DIR_D2C)
        dev.secure(kd, k)
        assertEquals(Btp.PAIR_CONFIRM, dev.receive().type)
        try {
            dev.receive(); fail("tampered record accepted")
        } catch (e: BtpConnectionException) {
            // expected
        }
        val dev2 = FrameChannel(ByteArrayInputStream(first + good), ByteArrayOutputStream(), Btp.DIR_D2C)
        dev2.secure(kd, k)
        dev2.receive()
        assertEquals(Btp.DF, dev2.receive().type)
        // a replayed record (counter mismatch) does not open either
        val dev3 = FrameChannel(ByteArrayInputStream(first + first), ByteArrayOutputStream(), Btp.DIR_D2C)
        dev3.secure(kd, k)
        dev3.receive()
        try { dev3.receive(); fail("replay accepted") } catch (e: BtpConnectionException) {}
    }

    @Test
    fun encryptedRecordsMatch() {
        for (sec in listOf("pairing", "auth")) {
            val p = o(V[sec])
            val c2d = hex(s(p, "k_c2d")); val d2c = hex(s(p, "k_d2c"))
            for (r in a(p["encrypted"]).map(::o)) {
                val dir = if (s(r, "dir") == "c2d") Btp.DIR_C2D else Btp.DIR_D2C
                val key = if (dir == Btp.DIR_C2D) c2d else d2c
                assertEquals(s(r, "nonce"), hx(FrameChannel.nonce(dir, l(r, "counter"))))
                val box = BtpCrypto.secretbox(hex(s(r, "inner_frame")), FrameChannel.nonce(dir, l(r, "counter")), key)
                val rec = PayloadWriter().u32(box.size.toLong()).bytes(box).build()
                assertEquals(s(r, "record"), hx(rec))
            }
        }
    }
}
