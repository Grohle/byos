package org.hibyos.cli

import org.hibyos.install.core.Json
import org.hibyos.install.core.JsonException
import org.hibyos.install.core.btp.BtpCrypto
import org.hibyos.install.core.btp.PairedDevice
import java.io.File
import java.io.IOException
import java.nio.file.Files
import java.nio.file.attribute.AclEntry
import java.nio.file.attribute.AclEntryPermission
import java.nio.file.attribute.AclEntryType
import java.nio.file.attribute.AclFileAttributeView
import java.nio.file.attribute.PosixFilePermission

/** One paired device as kept in devices.json. */
class DeviceRecord(
    val id: String, var name: String, val publicKey: String, val paired: Long, var lastAddress: String?, var lastSeen: Long,
) {
    fun toPaired() = PairedDevice(hex(id), hex(publicKey), name)
    fun toMap(): Map<String, Any?> = linkedMapOf("id" to id, "name" to name, "public_key" to publicKey, "paired" to paired,
        "last_address" to lastAddress, "last_seen" to lastSeen)

    companion object {
        fun hex(s: String) = ByteArray(s.length / 2) { s.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
    }
}

/** The configuration folder (docs/BYOS_CLI.md 3.1). */
class Config(val dir: File) {
    val keyFile = File(dir, "client.key")
    val devicesFile = File(dir, "devices.json")
    val cache = File(dir, "cache")

    companion object {
        fun defaultDir(env: Map<String, String>): File {
            val os = System.getProperty("os.name").lowercase()
            val home = System.getProperty("user.home")
            return when {
                os.contains("win") -> File(env["APPDATA"] ?: "$home\\AppData\\Roaming", "ByOS\\cli")
                os.contains("mac") -> File(home, "Library/Application Support/ByOS/cli")
                else -> env["XDG_CONFIG_HOME"]?.takeIf { it.isNotEmpty() }?.let { File(it, "byos-cli") } ?: File(home, ".config/byos-cli")
            }
        }

        fun hexOf(b: ByteArray) = b.joinToString("") { "%02x".format(it) }
    }

    private fun ensureDir() {
        if (!dir.isDirectory && !dir.mkdirs()) throw CliError("local_io", "cannot create the configuration folder ${dir.path}")
    }

    /** Makes a file readable by the current user only (POSIX 0600, or an owner-only ACL on Windows). */
    private fun restrict(f: File) {
        val p = f.toPath()
        try {
            val posix = Files.getFileAttributeView(p, java.nio.file.attribute.PosixFileAttributeView::class.java)
            if (posix != null) {
                Files.setPosixFilePermissions(p, setOf(PosixFilePermission.OWNER_READ, PosixFilePermission.OWNER_WRITE))
                return
            }
            val acl = Files.getFileAttributeView(p, AclFileAttributeView::class.java) ?: return
            val owner = Files.getOwner(p)
            val entry = AclEntry.newBuilder().setType(AclEntryType.ALLOW).setPrincipal(owner)
                .setPermissions(AclEntryPermission.values().toSet()).build()
            acl.acl = listOf(entry)
        } catch (e: Exception) {
            // best effort: some file systems have neither
        }
    }

    /** The client static X25519 secret, created on first use. Never printed. */
    fun clientSecret(): ByteArray {
        if (keyFile.isFile) {
            val b = keyFile.readBytes()
            if (b.size != 32) throw CliError("local_io", "the client key ${keyFile.path} is damaged; delete it and pair again")
            return b
        }
        ensureDir()
        val k = BtpCrypto.random(32)
        val tmp = File(dir, "client.key.tmp")
        tmp.delete()
        tmp.createNewFile()
        restrict(tmp)
        tmp.writeBytes(k)
        if (!tmp.renameTo(keyFile)) throw CliError("local_io", "cannot write ${keyFile.path}")
        restrict(keyFile)
        return k
    }

    fun devices(): MutableList<DeviceRecord> {
        if (!devicesFile.isFile) return ArrayList()
        return try {
            Json.arr(Json.parse(devicesFile.readText())).map { v ->
                val m = Json.obj(v)
                DeviceRecord(Json.str(m, "id"), Json.optStr(m, "name") ?: "HiBy", Json.str(m, "public_key"),
                    Json.optLong(m, "paired", 0), Json.optStr(m, "last_address"), Json.optLong(m, "last_seen", 0))
            }.toMutableList()
        } catch (e: JsonException) {
            throw CliError("local_io", "${devicesFile.path} is damaged: ${e.message}")
        }
    }

    fun saveDevices(list: List<DeviceRecord>) {
        ensureDir()
        val tmp = File(dir, "devices.json.tmp")
        try {
            tmp.writeText(JsonOut.encode(list.map { it.toMap() }) + "\n")
            Files.move(tmp.toPath(), devicesFile.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING,
                java.nio.file.StandardCopyOption.ATOMIC_MOVE)
        } catch (e: IOException) {
            throw CliError("local_io", "cannot write ${devicesFile.path}: ${e.message}")
        }
    }

    fun find(idOrPrefix: String): DeviceRecord? {
        val l = devices().filter { it.id.startsWith(idOrPrefix.lowercase()) }
        if (l.size > 1) throw CliError("usage", "\"$idOrPrefix\" matches more than one paired device")
        return l.firstOrNull()
    }
}
