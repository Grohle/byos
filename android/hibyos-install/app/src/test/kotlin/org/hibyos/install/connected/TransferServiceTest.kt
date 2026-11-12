package org.hibyos.install.connected

import android.app.Notification
import android.app.NotificationManager
import android.content.Context
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config

/** The foreground service that keeps transfers going in the background, and its notification. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class TransferServiceTest {
    private val ctx: Context get() = RuntimeEnvironment.getApplication()

    @Test
    fun startsWithWorkAndStopsAfter() {
        val app = shadowOf(RuntimeEnvironment.getApplication())
        TransferService.update(ctx, WorkNote(WorkNote.Kind.SEND, 25, 100, "Music/Album/01 Song.flac"), 0)
        val started = app.nextStartedService
        assertNotNull(started)
        assertEquals(TransferService::class.java.name, started.component?.className)
        // the service shows the notification of the current work
        val svc = Robolectric.buildService(TransferService::class.java, started).create().get()
        svc.onStartCommand(started, 0, 1)
        val n: Notification = shadowOf(svc).lastForegroundNotification
        assertEquals(ctx.getString(org.hibyos.install.R.string.sync_note_sending), n.extras.getString(Notification.EXTRA_TITLE))
        assertTrue(n.extras.getCharSequence(Notification.EXTRA_TEXT).toString().endsWith("01 Song.flac"))
        assertEquals(250, n.extras.getInt(Notification.EXTRA_PROGRESS))
        assertTrue(n.flags and Notification.FLAG_ONGOING_EVENT != 0)
        // a reconnection shows at once; later progress is refreshed
        TransferService.update(ctx, WorkNote(WorkNote.Kind.SEND, 30, 100, "Music/Album/01 Song.flac", reconnecting = 1), 100)
        val nm = shadowOf(ctx.getSystemService(NotificationManager::class.java))
        assertEquals(ctx.getString(org.hibyos.install.R.string.sync_note_reconnecting),
            nm.allNotifications.last().extras.getString(Notification.EXTRA_TITLE))
        // no work: the service leaves the foreground and stops itself
        TransferService.update(ctx, null, 200)
        assertTrue(shadowOf(svc).isStoppedBySelf)
        assertTrue(shadowOf(svc).isForegroundStopped)
        assertNull(app.nextStartedService)
    }

    /**
     * Review: work that ends before the service gets its start (a file skipped in a few ms) must
     * not stop the service before startForeground(), or Android ends the app.
     */
    @Test
    fun workEndingBeforeTheStartStillGoesToTheForeground() {
        val app = shadowOf(RuntimeEnvironment.getApplication())
        TransferService.update(ctx, WorkNote(WorkNote.Kind.SEND, 0, 10, "Music/cover.jpg"), 0)
        val started = app.nextStartedService
        assertNotNull(started)
        TransferService.update(ctx, null, 5)
        // nothing stops it from outside while its start is pending
        assertNull(app.nextStoppedService)
        val svc = Robolectric.buildService(TransferService::class.java, started).create().get()
        svc.onStartCommand(started, 0, 1)
        assertTrue(shadowOf(svc).lastForegroundNotificationId != 0)
        assertTrue(shadowOf(svc).notificationShouldRemoved)
        assertTrue(shadowOf(svc).isForegroundStopped)
        assertTrue(shadowOf(svc).isStoppedBySelf)
    }

    @Test
    fun installWithoutProgressIsIndeterminate() {
        val n = TransferService.build(ctx, WorkNote(WorkNote.Kind.INSTALL, 0, 0, ""))
        assertTrue(n.extras.getBoolean(Notification.EXTRA_PROGRESS_INDETERMINATE))
        assertEquals(ctx.getString(org.hibyos.install.R.string.sync_note_install), n.extras.getString(Notification.EXTRA_TITLE))
    }
}
