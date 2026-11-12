package org.hibyos.install

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.Modifier
import com.github.takahirom.roborazzi.RoborazziOptions
import com.github.takahirom.roborazzi.RoborazziTaskType
import com.github.takahirom.roborazzi.captureRoboImage
import org.hibyos.install.connected.Browser
import org.hibyos.install.connected.Destination
import org.hibyos.install.connected.JobStatus
import org.hibyos.install.connected.Library
import org.hibyos.install.connected.LibraryNav
import org.hibyos.install.connected.LibraryPhase
import org.hibyos.install.connected.Link
import org.hibyos.install.connected.OtaPhase
import org.hibyos.install.connected.OtaPick
import org.hibyos.install.connected.PairedDevice
import org.hibyos.install.connected.LibTarget
import org.hibyos.install.connected.Prompt
import org.hibyos.install.connected.Rescan
import org.hibyos.install.connected.Route
import org.hibyos.install.connected.SendJob
import org.hibyos.install.connected.SyncDevice
import org.hibyos.install.connected.SyncDeviceInfo
import org.hibyos.install.connected.SyncEntry
import org.hibyos.install.connected.SyncSpace
import org.hibyos.install.connected.SyncState
import org.hibyos.install.connected.Track
import org.hibyos.install.connected.Transfer
import org.hibyos.install.connected.UpdatePhase
import org.hibyos.install.ui.C
import org.hibyos.install.ui.LocalStill
import org.hibyos.install.ui.LocalSync
import org.hibyos.install.ui.NoSync
import org.hibyos.install.ui.ScreenContent
import org.hibyos.install.ui.SyncUi
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import java.io.File

/**
 * Draws the "ByOS connected" screens in their states (to build/screens/connected, or
 * HIBYOS_SHOTS/connected) so that the layout can be reviewed without a phone.
 */
@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(sdk = [35], qualifiers = "w412dp-h892dp-xhdpi")
class ConnectedScreensTest {
    private val dir = File(File(System.getenv("HIBYOS_SHOTS") ?: "build/screens"), "connected")

    @OptIn(com.github.takahirom.roborazzi.ExperimentalRoborazziApi::class)
    private val options = RoborazziOptions(taskType = RoborazziTaskType.Record)

    private val nothing = object : Actions {
        override fun go(screen: Screen) {}
        override fun back() {}
        override fun cancel() {}
        override fun openUrl(url: String) {}
        override fun downloadFirmware() {}
        override fun pickFirmware() {}
        override fun forgetFirmware() {}
        override fun patch() {}
        override fun setAddress(text: String) {}
        override fun findPlayers() {}
        override fun connect(player: FoundPlayer?) {}
        override fun changePlayer() {}
        override fun send() {}
        override fun saveToFolder() {}
        override fun share() {}
        override fun checkUpdate() {}
        override fun downloadUpdate() {}
        override fun openNotice(item: NoticeItem?) {}
        override fun cleanUp() {}
    }

    private val bundle = BundleInfo("0.9.0", "f08b218", true, "HiBy R3 II (2025) v1.3", "1.3", "r3ii_2025.upt", 46_759_936,
        null, "Mar 2 2026", true, "r3ii_2025.upt", 145, 206, 7_101_010, 4399, emptyList(), true)
    private val base = UiState(bundle = bundle, appVersion = "1.1.0")
    private val patched = base.copy(firmware = FirmwareState.Ready(46_759_936, "00"),
        patch = PatchState.Done(47_972_352, "11", "0.9.0-f08b218", 145, "0.9.0"))

    private val dev = SyncDevice("192.168.1.20", 7340, "0123456789abcdef", "R3II_2025", true, true, false)
    private val info = SyncDeviceInfo(dev.deviceId, "R3II_2025", "0.9.0", "ByOS 0.9.0", false, true, true, 0x1ff)
    private val space = SyncSpace(0, 1, 63_864_569_856, 21_474_836_480, 1L shl 30, 300L shl 20)
    private val on = SyncState(available = true, link = Link.Connected(dev.copy(paired = true), info, space))

    /** [tall]: a page tall enough to show everything, for the long lists. */
    private fun shot(name: String, screen: Screen, sync: SyncState, state: UiState = base, tall: Boolean = false) {
        val s = state.copy(stack = listOf(Screen.HOME, Screen.SYNC, screen).distinct())
        RuntimeEnvironment.setQualifiers(if (tall) "w412dp-h1700dp-xhdpi" else "w412dp-h892dp-xhdpi")
        captureRoboImage(File(dir, "$name.png").path, options) {
            CompositionLocalProvider(LocalSync provides SyncUi(sync, NoSync), LocalStill provides true) {
                Box(Modifier.fillMaxSize().background(C.bg)) { ScreenContent(screen, s, nothing) }
            }
        }
    }

    @Test
    fun connection() {
        shot("sync_0_unavailable", Screen.SYNC, SyncState())
        shot("sync_1_idle", Screen.SYNC, SyncState(available = true, paired = listOf(PairedDevice("aa", "Old R3", "192.168.1.30"))))
        shot("sync_2_found", Screen.SYNC, SyncState(available = true, found = listOf(dev, dev.copy(host = "192.168.1.21", name = "Kitchen", pairingOpen = false))))
        shot("sync_3_pairing", Screen.SYNC, SyncState(available = true, link = Link.Pairing(dev, "8153")))
        shot("sync_4_pairing_wrong", Screen.SYNC, SyncState(available = true, link = Link.Pairing(dev, "", wrong = true)))
        // states with a spinner (searching, connecting, waiting for Allow, a file being sent) are left out: an endless
        // animation keeps Robolectric from ever finishing the capture
        shot("sync_6_connected", Screen.SYNC, on)
        shot("sync_7_error", Screen.SYNC, SyncState(available = true,
            error = ErrorInfo(org.hibyos.install.R.string.sync_err_pair_closed, null, org.hibyos.install.R.string.sync_hint_pair_closed)))
    }

    @Test
    fun send() {
        val jobs = listOf(
            SendJob(1, "Album/01 Intro.flac", "Music/Album/01 Intro.flac", 31_000_000, JobStatus.SENT, 31_000_000),
            SendJob(2, "Album/02 Song.flac", "Music/Album/02 Song.flac", 28_000_000, JobStatus.WAITING, 0),
            SendJob(3, "Album/cover.jpg", "Music/Album/cover.jpg", 420_000),
            SendJob(4, "Album/.nomedia", "", 0, JobStatus.BAD_NAME),
        )
        shot("send_1_choose", Screen.SYNC_SEND, on.copy(dest = Destination.MUSIC))
        shot("send_2_list", Screen.SYNC_SEND, on.copy(transfer = Transfer(jobs = jobs.map { it.copy(status = if (it.id == 4) JobStatus.BAD_NAME else JobStatus.WAITING, done = 0) })))
        shot("send_3_running", Screen.SYNC_SEND, on.copy(transfer = Transfer(jobs, running = true, done = 99_000_000, total = 118_840_000,
            current = "Music/Album/02 Song.flac")))
        shot("send_4_done", Screen.SYNC_SEND, on.copy(transfer = Transfer(jobs.map { if (it.id == 4) it else it.copy(status = JobStatus.SENT, done = it.size) })))
        val mixed = listOf(
            SendJob(1, "Abba Gold/01 Dancing Queen.flac", "Music/Abba Gold/01 Dancing Queen.flac", 31_000_000, route = Route.MUSIC),
            SendJob(2, "Abba Gold/cover.jpg", "Music/Abba Gold/cover.jpg", 420_000, route = Route.MUSIC),
            SendJob(3, "Holiday/beach.jpg", "Pictures/Holiday/beach.jpg", 2_400_000, route = Route.PICTURES),
            SendJob(4, "novel.epub", "Books/novel.epub", 800_000, route = Route.BOOKS),
            SendJob(5, "r3ii_2025.upt", "r3ii_2025.upt", 46_759_936, route = Route.FIRMWARE),
            SendJob(6, "hibyos-update.json", "", 2_100, JobStatus.UPDATE_FILE, route = Route.BYOS_UPDATE),
        )
        shot("send_7_auto_groups", Screen.SYNC_SEND, on.copy(transfer = Transfer(jobs = mixed), routes = mapOf(Route.PICTURES to "Music")), tall = true)
        shot("send_8_no_space", Screen.SYNC_SEND, on.copy(link = Link.Connected(dev.copy(paired = true), info, space.copy(sdFree = 30_000_000)),
            transfer = Transfer(jobs = mixed)))
        shot("send_6_done_music", Screen.SYNC_SEND, on.copy(transfer = Transfer(jobs.map { if (it.id == 4) it else it.copy(status = JobStatus.SENT, done = it.size) },
            newMusic = 2)))
        shot("send_5_lost", Screen.SYNC_SEND, SyncState(available = true, transfer = Transfer(jobs.map { if (it.id == 2) it.copy(status = JobStatus.FAILED) else it },
            error = ErrorInfo(org.hibyos.install.R.string.sync_err_lost, "dropped", org.hibyos.install.R.string.sync_hint_lost))))
    }

    @Test
    fun files() {
        val entries = listOf(SyncEntry("ABBA", 1, 0, 0), SyncEntry("Free", 1, 0, 0), SyncEntry("01 Waterloo.flac", 0, 24_000_000, 0),
            SyncEntry("notes.txt", 0, 1200, 0))
        shot("files_1_list", Screen.SYNC_FILES, on.copy(browser = Browser("Music", entries)))
        shot("files_2_moving", Screen.SYNC_FILES, on.copy(browser = Browser("Music", entries, moving = "Books/Old")))
        shot("files_3_new_folder", Screen.SYNC_FILES, on.copy(browser = Browser("Music", entries, prompt = Prompt.NewFolder("Live"))))
        shot("files_4_delete", Screen.SYNC_FILES, on.copy(browser = Browser("Music", entries, prompt = Prompt.Delete(entries[0]))))
        shot("files_5_empty", Screen.SYNC_FILES, on.copy(browser = Browser("Books", emptyList())))
    }

    @Test
    fun library() {
        val lib = Library.build(listOf(
            Track("a:\\ABBA\\Gold\\01 Dancing Queen.flac", "Dancing Queen", "ABBA", "", "Gold", year = 1992),
            Track("a:\\ABBA\\Gold\\02 Knowing Me.flac", "Knowing Me, Knowing You", "ABBA", "", "Gold", year = 1992),
            Track("a:\\The Beatles\\Abbey Road\\01 Come Together.flac", "Come Together", "The Beatles", "", "Abbey Road", year = 1969),
            Track("a:\\Free\\x.mp3", "Untitled", "", "", ""),
        ))
        shot("library_1_idle", Screen.SYNC_LIBRARY, on)
        shot("library_2_loading", Screen.SYNC_LIBRARY, on.copy(library = LibraryPhase.Loading(1_200_000, 3_400_000)))
        shot("library_3_artists", Screen.SYNC_LIBRARY, on.copy(library = LibraryPhase.Ready(lib)))
        shot("library_4_albums", Screen.SYNC_LIBRARY, on.copy(library = LibraryPhase.Ready(lib), libraryNav = LibraryNav(tab = 1)))
        shot("library_5_album", Screen.SYNC_LIBRARY, on.copy(library = LibraryPhase.Ready(lib), libraryNav = LibraryNav(artist = "ABBA", album = "ABBA" to "Gold")))
        shot("library_6_rescanning", Screen.SYNC_LIBRARY, on.copy(library = LibraryPhase.Ready(lib), rescan = Rescan.Running))
        shot("library_7_rescan_unsupported", Screen.SYNC_LIBRARY, on.copy(library = LibraryPhase.Ready(lib), rescan = Rescan.Unsupported))
        shot("library_8_new_music", Screen.SYNC_LIBRARY, on.copy(library = LibraryPhase.Ready(lib), transfer = Transfer(newMusic = 12)))
        shot("library_9_idle_rescan_done", Screen.SYNC_LIBRARY, on.copy(rescan = Rescan.Done))
        val gold = lib.albums.first { it.title == "Gold" }
        shot("library_10_delete_album", Screen.SYNC_LIBRARY, on.copy(library = LibraryPhase.Ready(lib), libraryNav = LibraryNav(tab = 1),
            libDelete = LibTarget.AlbumOf(gold)))
        shot("library_11_delete_song", Screen.SYNC_LIBRARY, on.copy(library = LibraryPhase.Ready(lib), libraryNav = LibraryNav(tab = 2),
            libDelete = LibTarget.Song(lib.tracks[0])))
        shot("library_12_empty", Screen.SYNC_LIBRARY, on.copy(library = LibraryPhase.Ready(Library.build(emptyList()))))
        shot("library_13_changed", Screen.SYNC_LIBRARY, on.copy(library = LibraryPhase.Ready(lib), libraryNav = LibraryNav(tab = 2), libraryChanged = true))
    }

    @Test
    fun home() {
        // the entry on the home screen (ScreensTest draws the rest of it)
        RuntimeEnvironment.setQualifiers("w412dp-h1900dp-xhdpi")
        captureRoboImage(File(dir, "home_entry.png").path, options) {
            Box(Modifier.fillMaxSize().background(C.bg)) { ScreenContent(Screen.HOME, base, nothing) }
        }
    }

    @Test
    fun update() {
        shot("update_1_not_built", Screen.SYNC_UPDATE, on)
        shot("update_2_ready", Screen.SYNC_UPDATE, on, patched)
        shot("update_3_sending", Screen.SYNC_UPDATE, on.copy(update = UpdatePhase.Sending(20_000_000, 55_073_362, "r3ii_2025.upt")), patched)
        shot("update_4_sent", Screen.SYNC_UPDATE, on.copy(update = UpdatePhase.Sent(207, 180)), patched)
    }

    @Test
    fun install() {
        val pick = OtaPick("0.9.1", "hibyos-payload-0.9.1.tar", 9_437_184, false, "shell", "hibyos-payload-0.9.1.tar", 9_437_184)
        shot("install_1_empty", Screen.SYNC_INSTALL, on)
        shot("install_2_json_only", Screen.SYNC_INSTALL, on.copy(otaPick = pick.copy(tarName = null, tarSize = 0)))
        shot("install_3_ready", Screen.SYNC_INSTALL, on.copy(otaPick = pick))
        shot("install_4_tar_wrong", Screen.SYNC_INSTALL, on.copy(otaPick = pick.copy(tarName = "hibyos-payload-0.8.0.tar", tarSize = 8_000_000, needsFirmware = true)))
        shot("install_5_signature", Screen.SYNC_INSTALL, on.copy(otaPick = OtaPick(error = ErrorInfo(org.hibyos.install.R.string.sync_err_signature,
            "the update is not signed by a ByOS release key", org.hibyos.install.R.string.sync_hint_signature))))
        shot("install_6_sending", Screen.SYNC_INSTALL, on.copy(otaPick = pick, ota = OtaPhase.Sending(6_000_000, 18_900_000, "hibyos-payload-0.9.1.tar")))
        shot("install_7_checking", Screen.SYNC_INSTALL, on.copy(otaPick = pick, ota = OtaPhase.Checking))
        shot("install_8_confirm", Screen.SYNC_INSTALL, on.copy(otaPick = pick, ota = OtaPhase.WaitingUser("0.9.1")))
        shot("install_9_installing", Screen.SYNC_INSTALL, on.copy(otaPick = pick, ota = OtaPhase.Installing("0.9.1", 640)))
        shot("install_10_done", Screen.SYNC_INSTALL, SyncState(available = true, otaPick = pick, ota = OtaPhase.Done("0.9.1", 1)))
        shot("install_11_downgrade", Screen.SYNC_INSTALL, on.copy(otaPick = pick.copy(version = "0.8.0"), ota = OtaPhase.Failed(
            ErrorInfo(org.hibyos.install.R.string.sync_err_downgrade, null, org.hibyos.install.R.string.sync_hint_downgrade), downgrade = true)))
        shot("install_12_battery", Screen.SYNC_INSTALL, on.copy(otaPick = pick, ota = OtaPhase.Failed(
            ErrorInfo(org.hibyos.install.R.string.sync_err_battery, null, org.hibyos.install.R.string.sync_hint_battery))))
        shot("install_13_refused", Screen.SYNC_INSTALL, on.copy(otaPick = pick, ota = OtaPhase.Refused("0.9.1")))
        shot("install_14_staged", Screen.SYNC_INSTALL, on.copy(otaPick = pick, ota = OtaPhase.StagedOnly("0.9.1")))
        shot("install_15_unsupported", Screen.SYNC_INSTALL, SyncState(available = true,
            link = Link.Connected(dev.copy(paired = true), info.copy(caps = 0x1df), space)))
    }
}
