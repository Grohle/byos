package org.hibyos.install.connected

import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.net.wifi.WifiManager
import org.hibyos.install.Net
import java.io.File

/**
 * Binds the screens to the BTP/1 client of the shared core (org.hibyos.install.core.btp) through
 * [CoreSync]. The client keys live in [keyDir] (the app's private storage). A Wi-Fi multicast
 * lock is held while discovery waits for the UDP answers (Android drops broadcasts otherwise).
 * Every socket is tied to the Wi-Fi network, and when no ANNOUNCE comes back the phone's Wi-Fi
 * neighbours are tried directly on the BTP port.
 */
object SyncBackend {
    fun connector(context: Context, keyDir: File, version: String): SyncConnector? {
        val app = context.applicationContext
        val wifi = app.getSystemService(WifiManager::class.java)
        val lock = try {
            wifi?.createMulticastLock("hibyos-sync")?.apply { setReferenceCounted(false) }
        } catch (e: Exception) {
            null
        }
        val net = SyncNetwork(
            // looked up for each socket: the Wi-Fi network changes when the phone roams or reconnects
            socket = { Net.socketFactory(app)() },
            datagram = { Net.datagramFactory(app)() },
            broadcasts = { Net.broadcasts(app) },
            neighbours = { Net.neighbours(app) },
        )
        return CoreSync(keyDir, "ByOS Install $version",
            beforeDiscovery = { try { lock?.acquire() } catch (e: Exception) {} },
            afterDiscovery = { try { lock?.release() } catch (e: Exception) {} },
            net = net)
    }
}
/** Reads the tracks of a DB snapshot with Android's own SQLite, read-only. */
object LibraryDb {
    fun read(file: File): List<Track> {
        val db = SQLiteDatabase.openDatabase(file.path, null, SQLiteDatabase.OPEN_READONLY or SQLiteDatabase.NO_LOCALIZED_COLLATORS)
        db.use {
            val hasAlbumArtist = it.rawQuery("PRAGMA table_info(MEDIA_TABLE)", null).use { c ->
                var found = false
                val ni = c.getColumnIndex("name")
                while (c.moveToNext()) if (c.getString(ni).equals("album_artist", ignoreCase = true)) found = true
                found
            }
            val cols = "path, name, artist, album, genre, year" + if (hasAlbumArtist) ", album_artist" else ""
            val out = ArrayList<Track>()
            it.rawQuery("SELECT $cols FROM MEDIA_TABLE WHERE has_child_file = 0 OR has_child_file IS NULL", null).use { c ->
                while (c.moveToNext()) {
                    out += Track(
                        path = c.getString(0) ?: "",
                        title = c.getString(1) ?: "",
                        artist = c.getString(2) ?: "",
                        album = c.getString(3) ?: "",
                        genre = c.getString(4) ?: "",
                        year = if (c.isNull(5)) 0 else c.getInt(5),
                        albumArtist = if (hasAlbumArtist) c.getString(6) ?: "" else "",
                    )
                }
            }
            return out
        }
    }
}
