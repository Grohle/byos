package org.hibyos.install.connected

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import org.hibyos.install.MainActivity
import org.hibyos.install.R

/**
 * Keeps the app running while files go to the player with the screen off or another app in
 * front: a foreground service of type dataSync with one notification that shows the progress.
 * The work itself stays on the connected mode's worker; this service only holds the process
 * and draws [note]. It is started when work begins and stopped when it ends ([update]).
 */
class TransferService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val n = note
        if (n == null) {
            stopSelf()
            return START_NOT_STICKY
        }
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) startForeground(ID, build(this, n), ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
            else startForeground(ID, build(this, n))
        } catch (e: Exception) {
            // not allowed now (the app went to the background first): the work goes on without the notification
            stopSelf()
        }
        return START_NOT_STICKY
    }

    /** Android 15 ends a dataSync service after six hours a day. */
    override fun onTimeout(startId: Int, fgsType: Int) {
        stopSelf()
    }

    companion object {
        private const val ID = 7340
        private const val CHANNEL = "transfers"

        @Volatile
        private var note: WorkNote? = null
        private var running = false
        private var shownAt = 0L
        private var shown: WorkNote? = null

        private fun channel(ctx: Context) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
            val nm = ctx.getSystemService(NotificationManager::class.java) ?: return
            if (nm.getNotificationChannel(CHANNEL) != null) return
            nm.createNotificationChannel(NotificationChannel(CHANNEL, ctx.getString(R.string.sync_note_channel), NotificationManager.IMPORTANCE_LOW).apply {
                description = ctx.getString(R.string.sync_note_channel_text)
                setShowBadge(false)
            })
        }

        fun build(ctx: Context, n: WorkNote): Notification {
            channel(ctx)
            val open = PendingIntent.getActivity(ctx, 0, Intent(ctx, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
            val title = ctx.getString(when {
                n.reconnecting > 0 -> R.string.sync_note_reconnecting
                n.kind == WorkNote.Kind.SEND -> R.string.sync_note_sending
                n.kind == WorkNote.Kind.UPDATE -> R.string.sync_note_update
                else -> R.string.sync_note_install
            })
            val pm = n.permille
            @Suppress("DEPRECATION")
            val b = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) Notification.Builder(ctx, CHANNEL) else Notification.Builder(ctx)
            return b.setSmallIcon(R.drawable.ic_launcher_monochrome)
                .setContentTitle(title)
                .setContentText(if (pm >= 0) ctx.getString(R.string.percent, pm / 10) + "  ·  " + n.detail.substringAfterLast('/') else n.detail.substringAfterLast('/'))
                .setProgress(1000, maxOf(pm, 0), pm < 0 || n.reconnecting > 0)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setCategory(Notification.CATEGORY_PROGRESS)
                .setContentIntent(open)
                .build()
        }

        /**
         * Called on the UI thread after each change of the connected mode: starts the service
         * when work begins, refreshes the notification (at most twice a second) and stops it
         * when the work ends.
         */
        fun update(ctx: Context, n: WorkNote?, now: Long) {
            note = n
            val app = ctx.applicationContext
            if (n == null) {
                if (running) {
                    running = false
                    shown = null
                    app.stopService(Intent(app, TransferService::class.java))
                }
                return
            }
            if (!running) {
                running = true
                try {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) app.startForegroundService(Intent(app, TransferService::class.java))
                    else app.startService(Intent(app, TransferService::class.java))
                } catch (e: Exception) {
                    // the system refused (background start limits): the work goes on while the app is open
                    running = false
                }
                shownAt = now
                shown = n
                return
            }
            val s = shown
            if (s != null && s.kind == n.kind && s.reconnecting == n.reconnecting && now - shownAt < 500) return
            shownAt = now
            shown = n
            try {
                app.getSystemService(NotificationManager::class.java)?.notify(ID, build(app, n))
            } catch (e: SecurityException) {
                // notifications are off for the app
            }
        }
    }
}
