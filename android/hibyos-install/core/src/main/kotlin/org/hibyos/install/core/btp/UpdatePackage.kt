package org.hibyos.install.core.btp

import org.hibyos.install.core.Ed25519
import org.hibyos.install.core.Json
import org.hibyos.install.core.JsonException
import java.io.File
import java.security.MessageDigest

class UpdateException(val kind: Kind, message: String) : Exception(message) {
    enum class Kind { SIGNATURE, INVALID, PAYLOAD }
}

/**
 * A ByOS release (`hibyos-update.json` plus its payload tar), checked on the computer with
 * the same rules as the device (docs/OTA.md, `ota_sig_check_keys` and `ota_parse` in
 * src/ota_net.inc.c) before anything is sent. The device checks everything again.
 */
class UpdatePackage(
    val json: ByteArray, val version: String, val payloadName: String, val payloadUrl: String, val payloadSize: Long,
    val payloadSha256: String, val restart: String, val needsFirmware: Boolean, val minVersion: String?, val minFirmware: String?,
) {
    /** Checks the tar against the signed size and SHA-256. */
    fun checkPayload(tar: File) {
        if (!tar.isFile) throw UpdateException(UpdateException.Kind.PAYLOAD, "payload file not found: ${tar.path}")
        if (tar.length() != payloadSize) throw UpdateException(UpdateException.Kind.PAYLOAD, "the payload is ${tar.length()} bytes, the release says $payloadSize")
        val md = MessageDigest.getInstance("SHA-256")
        tar.inputStream().use { s -> val b = ByteArray(1 shl 16); while (true) { val n = s.read(b); if (n < 0) break; md.update(b, 0, n) } }
        val got = md.digest().joinToString("") { "%02x".format(it) }
        if (got != payloadSha256) throw UpdateException(UpdateException.Kind.PAYLOAD, "the payload's SHA-256 does not match the signed release")
    }

    companion object {
        /** Release keys compiled into hibyshell (OTA_PUBKEY_HEX, OTA_PUBKEY2_HEX in src/ota_net.inc.c). */
        val RELEASE_KEYS = listOf("6d87851ece77b81547cfbde491d9c46494dc99e70922c48115bfe5d9e854e06e")
        const val PAYLOAD_MAX = 64L shl 20
        const val JSON_MAX = 64 shl 10
        const val LATEST_URL = "https://github.com/Grohle/hibyos/releases/latest/download/hibyos-update.json"

        fun versionOk(v: String): Boolean = v.length in 1..23 && Regex("[0-9]+(\\.[0-9]+)+(-[0-9A-Za-z.]+)?").matches(v)
        fun nameOk(n: String): Boolean = n.length in 5..90 && n[0] != '.' && n.all { it.isLetterOrDigit() && it.code < 128 || it in "._-" } && n.endsWith(".tar")
        fun urlOk(u: String): Boolean = (u.startsWith("https://") || u.startsWith("http://")) && u.length > 12 && u.none { it in " \t\r\n\"'\\<>" }

        /** Verifies the signature first, then parses; nothing in the JSON is used before that. */
        fun parse(json: ByteArray, keys: List<String> = RELEASE_KEYS): UpdatePackage {
            if (json.size > JSON_MAX) throw UpdateException(UpdateException.Kind.INVALID, "the update description is too large")
            if (!Ed25519.verifySignedJson(json, keys)) throw UpdateException(UpdateException.Kind.SIGNATURE, "the update is not signed by a ByOS release key")
            fun bad(why: String): Nothing = throw UpdateException(UpdateException.Kind.INVALID, "invalid update description: $why")
            try {
                val m = Json.obj(Json.parse(String(json, Charsets.UTF_8)))
                if (Json.optStr(m, "product") != "hibyos") bad("product")
                if (Json.optLong(m, "format", -1) != 1L) bad("format")
                val version = Json.str(m, "version").also { if (!versionOk(it)) bad("version") }
                val minV = Json.optStr(m, "min_version")?.also { if (!versionOk(it)) bad("min_version") }
                val minF = Json.optStr(m, "min_firmware")?.also { if (!versionOk(it)) bad("min_firmware") }
                val needs = when (val v = m["needs_firmware"]) { null -> false; is Boolean -> v; else -> bad("needs_firmware") }
                val restart = when (val r = m["restart"]) { null -> "reboot"; "none" -> "none"; "shell" -> "shell"; is String -> "reboot"; else -> bad("restart") }
                val pl = m["payload"] as? Map<*, *> ?: bad("payload")
                @Suppress("UNCHECKED_CAST") val p = pl as Map<String, Any?>
                val url = Json.str(p, "url").also { if (!urlOk(it)) bad("payload url") }
                val name = Json.str(p, "name").also { if (!nameOk(it)) bad("payload name") }
                val sha = Json.str(p, "sha256").lowercase().also { if (!Regex("[0-9a-f]{64}").matches(it)) bad("payload sha256") }
                val size = Json.long(p, "size").also { if (it <= 0 || it > PAYLOAD_MAX) bad("payload size") }
                Json.optStr(p, "compression")?.let { if (it != "none") bad("compression") }
                return UpdatePackage(json, version, name, url, size, sha, restart, needs, minV, minF)
            } catch (e: JsonException) {
                bad(e.message ?: "JSON")
            }
        }
    }
}
