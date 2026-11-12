package org.hibyos.install.core.btp

import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface
import java.net.SocketTimeoutException

/** A device that answered a discovery probe (section 16). */
class Found(val address: String, val announce: Announce) {
    val deviceIdHex: String get() = announce.deviceId.joinToString("") { "%02x".format(it) }
}

/**
 * UDP discovery of hibysync on the local networks: one 16-byte PROBE to every IPv4
 * broadcast address, then the ANNOUNCE answers that echo its nonce. Only private and
 * link-local senders are accepted, as the device itself does.
 */
object BtpDiscovery {
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

    fun isPrivate(a: InetAddress): Boolean {
        val b = a.address
        if (b.size != 4) return false
        val x = b[0].toInt() and 0xff
        val y = b[1].toInt() and 0xff
        return x == 10 || (x == 172 && y in 16..31) || (x == 192 && y == 168) || (x == 169 && y == 254) || x == 127
    }

    /**
     * Probes for [waitMs] and returns the devices that answered (the most recent address per
     * device id). [targets] defaults to every broadcast address; tests pass 127.0.0.1.
     */
    fun probe(waitMs: Int = 1500, targets: List<InetAddress> = broadcastAddresses(), port: Int = Btp.PORT,
              nonce: ByteArray = BtpCrypto.random(8)): List<Found> {
        val found = LinkedHashMap<String, Found>()
        DatagramSocket().use { sock ->
            sock.broadcast = true
            val pkt = Announce.probe(nonce)
            for (t in targets) {
                try {
                    sock.send(DatagramPacket(pkt, pkt.size, t, port))
                } catch (e: IOException) {
                    // unreachable network: try the others
                }
            }
            val deadline = System.currentTimeMillis() + waitMs
            val buf = ByteArray(512)
            while (true) {
                val left = deadline - System.currentTimeMillis()
                if (left <= 0) break
                sock.soTimeout = left.toInt().coerceAtLeast(1)
                val p = DatagramPacket(buf, buf.size)
                try {
                    sock.receive(p)
                } catch (e: SocketTimeoutException) {
                    break
                }
                if (!isPrivate(p.address)) continue
                val a = Announce.parse(p.data.copyOf(p.length)) ?: continue
                if (!a.nonce.contentEquals(nonce)) continue
                val f = Found(p.address.hostAddress, a)
                found[f.deviceIdHex] = f
            }
        }
        return found.values.toList()
    }
}
