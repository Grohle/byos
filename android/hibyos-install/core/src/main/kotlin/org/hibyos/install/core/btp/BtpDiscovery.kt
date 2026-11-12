package org.hibyos.install.core.btp

import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.Socket
import java.net.SocketTimeoutException
import java.util.Collections
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/** A device that answered a discovery probe (section 16), or a HELLO during a sweep. */
class Found(val address: String, val announce: Announce) {
    val deviceIdHex: String get() = announce.deviceId.joinToString("") { "%02x".format(it) }
    val port: Int get() = announce.tcpPort
}

/**
 * Finds hibysync on the local networks.
 *
 * [probe]: UDP discovery (section 16). One 16-byte PROBE to every IPv4 broadcast address,
 * repeated a few times while waiting (a broadcast on Wi-Fi is easily lost, and a daemon that is
 * just starting misses the first one), then the ANNOUNCE answers that echo its nonce. Only
 * private and link-local senders are accepted, as the device itself does.
 *
 * [sweep]: the fallback for networks that drop broadcasts (guest networks, mesh systems, "AP
 * isolation" for broadcast): a TCP connection to the BTP port of every neighbour, and a HELLO
 * from the ones that answer. HELLO carries the same device id and name as an ANNOUNCE.
 */
object BtpDiscovery {
    /** When the probe is sent again during [probe]'s wait (ms after the start). */
    val RESEND_AT_MS = longArrayOf(0, 250, 700, 1500, 2500, 4000)

    fun broadcastAddresses(): List<InetAddress> {
        val out = LinkedHashSet<InetAddress>()
        try {
            for (ni in NetworkInterface.getNetworkInterfaces()?.toList() ?: emptyList()) {
                if (!ni.isUp || ni.isLoopback) continue
                for (ia in ni.interfaceAddresses) {
                    val b = ia.broadcast ?: continue
                    if (ia.address is Inet4Address) out += b
                }
            }
        } catch (e: IOException) {
            // no interfaces: only the limited broadcast below
        }
        out += InetAddress.getByName("255.255.255.255")
        return out.toList()
    }

    /** The directed broadcast address of [address]/[prefixLength] (IPv4), or null. */
    fun subnetBroadcast(address: ByteArray, prefixLength: Int): InetAddress? {
        if (address.size != 4 || prefixLength !in 1..30) return null
        val ip = ((address[0].toLong() and 0xff) shl 24) or ((address[1].toLong() and 0xff) shl 16) or
            ((address[2].toLong() and 0xff) shl 8) or (address[3].toLong() and 0xff)
        val mask = (0xffffffffL shl (32 - prefixLength)) and 0xffffffffL
        val b = (ip and mask) or (mask.inv() and 0xffffffffL)
        return InetAddress.getByAddress(byteArrayOf((b shr 24).toByte(), (b shr 16).toByte(), (b shr 8).toByte(), b.toByte()))
    }

    fun isPrivate(a: InetAddress): Boolean {
        val b = a.address
        if (b.size != 4) return false
        val x = b[0].toInt() and 0xff
        val y = b[1].toInt() and 0xff
        return x == 10 || (x == 172 && y in 16..31) || (x == 192 && y == 168) || (x == 169 && y == 254) || x == 127
    }

    /**
     * Checks one UDP answer: an ANNOUNCE from a private address that echoes [nonce]. Returns the
     * device, or null for anything else (a stray packet, an old answer, another program).
     */
    fun accept(sender: InetAddress, data: ByteArray, len: Int, nonce: ByteArray): Found? {
        if (!isPrivate(sender)) return null
        val a = Announce.parse(data, len) ?: return null
        if (!a.nonce.contentEquals(nonce)) return null
        if (a.tcpPort !in 1..65535) return null
        return Found(sender.hostAddress, a)
    }

    /**
     * Probes for [waitMs] and returns the devices that answered (the most recent address per
     * device id). [targets] defaults to every broadcast address; tests pass 127.0.0.1.
     * [socket] makes the UDP socket (Android binds it to the Wi-Fi network). [onFound] sees each
     * device the first time it answers; [stopWhen] ends the wait early (for example once the
     * wanted device answered).
     */
    fun probe(waitMs: Int = 1500, targets: List<InetAddress> = broadcastAddresses(), port: Int = Btp.PORT,
              nonce: ByteArray = BtpCrypto.random(8), socket: () -> DatagramSocket = { DatagramSocket() },
              cancelled: () -> Boolean = { false }, onFound: (Found) -> Unit = {},
              stopWhen: (Found) -> Boolean = { false }): List<Found> {
        val found = LinkedHashMap<String, Found>()
        socket().use { sock ->
            sock.broadcast = true
            val pkt = Announce.probe(nonce)
            var sentAny = false
            var lastError: IOException? = null
            fun send() {
                for (t in targets) {
                    try {
                        sock.send(DatagramPacket(pkt, pkt.size, t, port))
                        sentAny = true
                    } catch (e: IOException) {
                        // unreachable network: try the others
                        lastError = e
                    }
                }
            }
            val start = System.currentTimeMillis()
            val deadline = start + waitMs
            var nextSend = 0
            val buf = ByteArray(512)
            while (!cancelled()) {
                val now = System.currentTimeMillis()
                if (now >= deadline) break
                while (nextSend < RESEND_AT_MS.size && now - start >= RESEND_AT_MS[nextSend]) {
                    send()
                    nextSend++
                }
                if (!sentAny && nextSend >= 1) {
                    // not one probe could leave (no network at all): say so instead of "nothing found"
                    throw lastError ?: IOException("the discovery probe could not be sent")
                }
                val untilSend = if (nextSend < RESEND_AT_MS.size) start + RESEND_AT_MS[nextSend] - now else Long.MAX_VALUE
                val wait = minOf(deadline - now, untilSend, 250L).coerceAtLeast(1L)
                sock.soTimeout = wait.toInt()
                val p = DatagramPacket(buf, buf.size)
                try {
                    sock.receive(p)
                } catch (e: SocketTimeoutException) {
                    continue
                }
                val f = accept(p.address, p.data, p.length, nonce) ?: continue
                val first = !found.containsKey(f.deviceIdHex)
                found[f.deviceIdHex] = f
                if (first) onFound(f)
                if (stopWhen(f)) break
            }
        }
        return found.values.toList()
    }

    /**
     * The fallback when broadcasts do not get through: connects to [port] on each of [hosts]
     * (at most [threads] at a time) and reads the HELLO of whatever answers as hibysync. The
     * session is closed right after HELLO (BYE), so the device only sees a short connection.
     */
    fun sweep(hosts: List<String>, hello: Hello, port: Int = Btp.PORT, connectTimeoutMs: Int = 700, helloTimeoutMs: Int = 3000,
              threads: Int = 32, socketFactory: () -> Socket = { Socket() }, cancelled: () -> Boolean = { false },
              onFound: (Found) -> Unit = {}, stopWhen: (Found) -> Boolean = { false }): List<Found> {
        if (hosts.isEmpty()) return emptyList()
        val found = Collections.synchronizedList(ArrayList<Found>())
        val done = java.util.concurrent.atomic.AtomicBoolean(false)
        val pool = Executors.newFixedThreadPool(minOf(threads, hosts.size)) { r -> Thread(r, "btp-sweep").apply { isDaemon = true } }
        try {
            for (h in hosts) {
                pool.execute {
                    if (done.get() || cancelled()) return@execute
                    val f = helloAt(h, port, hello, connectTimeoutMs, helloTimeoutMs, socketFactory) ?: return@execute
                    synchronized(found) {
                        if (found.any { it.deviceIdHex == f.deviceIdHex }) return@execute
                        found += f
                    }
                    onFound(f)
                    if (stopWhen(f)) done.set(true)
                }
            }
            pool.shutdown()
            while (!pool.awaitTermination(100, TimeUnit.MILLISECONDS)) {
                if (done.get() || cancelled()) {
                    pool.shutdownNow()
                    break
                }
            }
        } finally {
            pool.shutdownNow()
        }
        return synchronized(found) { found.toList() }
    }

    /** HELLO at [host]:[port], as an ANNOUNCE would describe it; null when no hibysync answers there. */
    fun helloAt(host: String, port: Int, hello: Hello, connectTimeoutMs: Int, helloTimeoutMs: Int,
                socketFactory: () -> Socket = { Socket() }): Found? {
        val s = try {
            socketFactory().also {
                it.connect(InetSocketAddress(host, port), connectTimeoutMs)
                it.soTimeout = helloTimeoutMs
            }
        } catch (e: IOException) {
            return null
        } catch (e: RuntimeException) {
            return null
        }
        return try {
            val session = BtpSession.open(s.getInputStream(), s.getOutputStream(), hello, null, s)
            session.use {
                val id = it.device.deviceId ?: return null
                if (id.size != 8) return null
                val flags = (if (it.pairingOpen) 1 else 0) or (if (it.sdMounted) 2 else 0)
                Found(host, Announce(flags, ByteArray(8), port, id, it.device.name ?: ""))
            }
        } catch (e: Exception) {
            try { s.close() } catch (x: IOException) {}
            null
        }
    }
}
