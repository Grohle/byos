package org.hibyos.install.core

import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket
import java.util.Collections
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

/**
 * "Find my player": looks on the phone's own Wi-Fi network for a device that answers on the
 * transfer port like the player does. It only runs when the user asks for it, it only tries
 * the one port, and it never leaves the local network.
 */
object Discovery {
    /**
     * The addresses to try: the phone's own network when it has up to 254 hosts, otherwise the
     * 254 neighbours that share the first three numbers. The phone itself is left out.
     */
    fun candidates(address: ByteArray, prefixLength: Int): List<String> {
        if (address.size != 4) return emptyList()
        val ip = ((address[0].toLong() and 0xff) shl 24) or ((address[1].toLong() and 0xff) shl 16) or
            ((address[2].toLong() and 0xff) shl 8) or (address[3].toLong() and 0xff)
        val prefix = prefixLength.coerceIn(24, 30)
        val mask = (0xffffffffL shl (32 - prefix)) and 0xffffffffL
        val net = ip and mask
        val last = net or (mask.inv() and 0xffffffffL)
        val out = ArrayList<String>()
        var a = net + 1
        while (a < last) {
            if (a != ip) out += "${(a shr 24) and 0xff}.${(a shr 16) and 0xff}.${(a shr 8) and 0xff}.${a and 0xff}"
            a++
        }
        return out
    }

    /**
     * Tries every address of [hosts]; [onFound] is called (from worker threads) for each player.
     * Returns the players found, in address order.
     */
    fun scan(hosts: List<String>, port: Int = PlayerClient.DEFAULT_PORT, home: String = PlayerClient.DEFAULT_HOME,
             connectTimeoutMs: Int = 900, threads: Int = 32, cancelled: () -> Boolean = { false },
             socketFactory: () -> Socket = { Socket() }, onProgress: ((Int, Int) -> Unit)? = null,
             onFound: ((PlayerInfo) -> Unit)? = null): List<PlayerInfo> {
        if (hosts.isEmpty()) return emptyList()
        val found = Collections.synchronizedList(ArrayList<PlayerInfo>())
        val done = AtomicInteger(0)
        val pool = Executors.newFixedThreadPool(minOf(threads, hosts.size)) { r -> Thread(r, "find-player").apply { isDaemon = true } }
        try {
            for (h in hosts) {
                pool.execute {
                    try {
                        if (!cancelled() && answers(h, port, connectTimeoutMs, socketFactory)) {
                            val info = PlayerClient(h, port, home, connectTimeoutMs = 2000, readTimeoutMs = 4000,
                                socketFactory = socketFactory).identify()
                            found += info
                            onFound?.invoke(info)
                        }
                    } catch (e: TransferException) {
                        // not a player
                    } catch (e: RuntimeException) {
                        // not a player
                    } finally {
                        onProgress?.invoke(done.incrementAndGet(), hosts.size)
                    }
                }
            }
            pool.shutdown()
            while (!pool.awaitTermination(100, TimeUnit.MILLISECONDS)) {
                if (cancelled()) {
                    pool.shutdownNow()
                    break
                }
            }
        } finally {
            pool.shutdownNow()
        }
        return found.sortedBy { it.host.split('.').joinToString(".") { p -> p.padStart(3, '0') } }
    }

    private fun answers(host: String, port: Int, timeoutMs: Int, socketFactory: () -> Socket): Boolean = try {
        socketFactory().use {
            it.connect(InetSocketAddress(host, port), timeoutMs)
            true
        }
    } catch (e: IOException) {
        false
    }
}
