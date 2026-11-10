package org.hibyos.install.core

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.math.BigInteger
import java.security.MessageDigest

/** Signs like RFC 8032, for the tests only (the app itself never signs anything). */
internal object TestSigner {
    private fun clamp(h: ByteArray): BigInteger {
        val a = h.copyOf(32)
        a[0] = (a[0].toInt() and 248).toByte()
        a[31] = ((a[31].toInt() and 127) or 64).toByte()
        return Ed25519.le(a, 0, 32)
    }

    private fun le32(v: BigInteger): ByteArray {
        val out = ByteArray(32)
        val be = v.toByteArray()
        for (i in 0 until minOf(32, be.size)) out[i] = be[be.size - 1 - i]
        return out
    }

    fun publicKey(seed: ByteArray): ByteArray =
        Ed25519.encode(Ed25519.mul(clamp(MessageDigest.getInstance("SHA-512").digest(seed)), Ed25519.BASE))

    fun sign(seed: ByteArray, msg: ByteArray): ByteArray {
        val h = MessageDigest.getInstance("SHA-512").digest(seed)
        val a = clamp(h)
        val pub = Ed25519.encode(Ed25519.mul(a, Ed25519.BASE))
        val md = MessageDigest.getInstance("SHA-512")
        md.update(h, 32, 32)
        md.update(msg)
        val r = Ed25519.le(md.digest(), 0, 64).mod(Ed25519.L)
        val rEnc = Ed25519.encode(Ed25519.mul(r, Ed25519.BASE))
        md.update(rEnc)
        md.update(pub)
        md.update(msg)
        val k = Ed25519.le(md.digest(), 0, 64).mod(Ed25519.L)
        return rEnc + le32(r.add(k.multiply(a)).mod(Ed25519.L))
    }

    /** Fills in the signature of a JSON text that has the placeholder of 128 zeros. */
    fun signJson(seed: ByteArray, text: String): ByteArray {
        val zeros = "0".repeat(128)
        require(text.contains("\"ed25519:$zeros\""))
        val sig = Hex.of(sign(seed, text.toByteArray()))
        return text.replace("\"ed25519:$zeros\"", "\"ed25519:$sig\"").toByteArray()
    }
}

class Ed25519Test {
    private class Vec(val seed: String, val pub: String, val msg: String, val sig: String)

    // RFC 8032, section 7.1: tests 1, 2, 3 and SHA(abc)
    private val vectors = listOf(
        Vec("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
            "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
        Vec("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
            "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
            "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
        Vec("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
            "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
            "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"),
        Vec("833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42",
            "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
            "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
            "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704"),
    )

    @Test
    fun rfc8032Vectors() {
        for (v in vectors) {
            val pub = Hex.parse(v.pub)
            val msg = Hex.parse(v.msg)
            val sig = Hex.parse(v.sig)
            assertTrue(Ed25519.verify(pub, msg, sig))
            assertArrayEquals(pub, TestSigner.publicKey(Hex.parse(v.seed)))
            assertArrayEquals(sig, TestSigner.sign(Hex.parse(v.seed), msg))
            // any change of message, signature or key must be refused
            assertFalse(Ed25519.verify(pub, msg + 0, sig))
            for (i in sig.indices step 7) {
                val bad = sig.clone().also { it[i] = (it[i].toInt() xor 1).toByte() }
                assertFalse("signature byte $i", Ed25519.verify(pub, msg, bad))
            }
            for (i in pub.indices step 5) {
                val bad = pub.clone().also { it[i] = (it[i].toInt() xor 4).toByte() }
                assertFalse("key byte $i", Ed25519.verify(bad, msg, sig))
            }
        }
    }

    @Test
    fun refusesMalformedInput() {
        val v = vectors[1]
        val pub = Hex.parse(v.pub)
        val msg = Hex.parse(v.msg)
        val sig = Hex.parse(v.sig)
        assertFalse(Ed25519.verify(pub.copyOf(31), msg, sig))
        assertFalse(Ed25519.verify(pub, msg, sig.copyOf(63)))
        // S + L is the same number modulo the group order and must not be accepted
        val s = Ed25519.le(sig, 32, 32).add(Ed25519.L)
        val be = s.toByteArray()
        val forged = sig.clone()
        for (i in 0 until 32) forged[32 + i] = if (i < be.size) be[be.size - 1 - i] else 0
        assertFalse(Ed25519.verify(pub, msg, forged))
        // a "key" that is not a point of the curve
        assertFalse(Ed25519.verify(ByteArray(32) { 0xff.toByte() }, msg, sig))
        assertFalse(Ed25519.verify(ByteArray(32).also { it[0] = 2 }, msg, sig))
    }

    private val pythonKey = "daa8ce1e8f5979fb65ac11f533ceba853998d6e89c23b36344b704989f942015"

    @Test
    fun acceptsAFileSignedByTheProjectsTool() {
        // signed_index.json was signed by tools/ota_sign.py with a key made only for this test
        val data = TestData.resource("signed_index.json")
        assertTrue(Ed25519.verifySignedJson(data, listOf(pythonKey)))
        assertTrue(Ed25519.verifySignedJson(data, listOf("", "0".repeat(64), "junk", pythonKey.uppercase())))
        assertFalse(Ed25519.verifySignedJson(data, emptyList()))
        assertFalse(Ed25519.verifySignedJson(data, listOf(vectors[0].pub)))
        assertFalse(Ed25519.verifySignedJson(data, listOf("0".repeat(64))))
        val text = String(data)
        // every field is covered by the signature
        for ((a, b) in listOf("12345" to "12346", "9.9.9\"" to "9.9.8\"", "example.invalid" to "example.invalif", "abab" to "abac",
            "\"format\": 1" to "\"format\": 2", "{" to "{ ")) {
            assertTrue(a, text.contains(a))
            assertFalse(a, Ed25519.verifySignedJson(text.replaceFirst(a, b).toByteArray(), listOf(pythonKey)))
        }
        // a second "signature" key, a missing one, a short one
        assertFalse(Ed25519.verifySignedJson(text.replaceFirst("{", "{\"signature\": \"x\",").toByteArray(), listOf(pythonKey)))
        assertFalse(Ed25519.verifySignedJson(text.replace("signature", "signatur").toByteArray(), listOf(pythonKey)))
        assertFalse(Ed25519.verifySignedJson(text.replace("ed25519:3", "ed25519:").toByteArray(), listOf(pythonKey)))
        assertFalse(Ed25519.verifySignedJson(ByteArray(0), listOf(pythonKey)))
    }

    @Test
    fun testSignerAndVerifierAgreeOnJson() {
        val seed = ByteArray(32) { (it * 7 + 1).toByte() }
        val pub = Hex.of(TestSigner.publicKey(seed))
        val signed = TestSigner.signJson(seed, "{\"a\": 1, \"signature\": \"ed25519:${"0".repeat(128)}\"}\n")
        assertTrue(Ed25519.verifySignedJson(signed, listOf(pub)))
        assertFalse(Ed25519.verifySignedJson(signed, listOf(pythonKey)))
    }

    /**
     * Live check against the project's own signing tool: a new throwaway key every run, signed by
     * Python, verified here; and signed here, verified by Python. Needs HIBYOS_REPO and Python.
     */
    @Test
    fun agreesWithTheProjectsSigningTool() {
        val repo = System.getenv("HIBYOS_REPO")
        assumeTrue("HIBYOS_REPO not set", !repo.isNullOrEmpty())
        val tool = File(repo, "tools/ota_sign.py")
        assumeTrue("no tools/ota_sign.py", tool.isFile)
        val python = System.getenv("HIBYOS_EXPORT_PYTHON") ?: "python"
        val dir = TestUtil.tempDir("sig")
        // the tool refuses to write a key inside the repository, and it is right: the throwaway
        // key goes to HIBYOS_KEY_WORK or the system's temporary folder, and is deleted afterwards
        val keyDir = File(System.getenv("HIBYOS_KEY_WORK") ?: System.getProperty("java.io.tmpdir"), "hibyos-install-test-key")
        keyDir.deleteRecursively()
        keyDir.mkdirs()
        val key = File(keyDir, "throwaway.key")
        TestUtil.run(listOf(python, tool.path, "genkey", "--key", key.path))
        val pub = TestUtil.run(listOf(python, tool.path, "pubkey", "--key", key.path)).trim().lines().last().trim()
        assertEquals(64, pub.length)
        val zeros = "0".repeat(128)
        var count = 0
        for (n in listOf(0, 1, 63, 64, 65, 500, 4000)) {
            val f = File(dir, "index$n.json")
            f.writeText("{\n \"format\": 1,\n \"pad\": \"${"x".repeat(n)}\",\n \"signature\": \"ed25519:$zeros\"\n}\n")
            TestUtil.run(listOf(python, tool.path, "sign", "--key", key.path, f.path))
            val data = f.readBytes()
            assertTrue("python-signed, $n", Ed25519.verifySignedJson(data, listOf(pub)))
            assertFalse(Ed25519.verifySignedJson(String(data).replace("\"format\": 1", "\"format\": 3").toByteArray(), listOf(pub)))
            count++
        }
        // the other direction: signed by the test signer, checked by the tool
        val seed = ByteArray(32) { (it * 11 + 3).toByte() }
        val mine = File(dir, "mine.json")
        mine.writeBytes(TestSigner.signJson(seed, "{\"format\": 1, \"signature\": \"ed25519:$zeros\"}\n"))
        val out = TestUtil.run(listOf(python, tool.path, "verify", mine.path, "--pub", Hex.of(TestSigner.publicKey(seed))))
        assertTrue(out, out.contains("OK"))
        keyDir.deleteRecursively()
        println("ED25519 cross-check with tools/ota_sign.py: $count files signed by Python verified here, 1 signed here verified by Python")
    }
}
