package org.hibyos.install

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.ui.Modifier
import com.github.takahirom.roborazzi.RoborazziOptions
import com.github.takahirom.roborazzi.RoborazziTaskType
import com.github.takahirom.roborazzi.captureRoboImage
import org.hibyos.install.core.SendListener
import org.hibyos.install.core.Stage
import org.hibyos.install.ui.C
import org.hibyos.install.ui.ScreenContent
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import java.io.File

/**
 * Draws every screen of the app in every state it can be in, on the desktop, and writes the
 * pictures to docs/screens (HIBYOS_SHOTS chooses another folder). The pictures of the README
 * come from here; looking at them is how the layout is reviewed without a phone.
 */
@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(sdk = [35], qualifiers = PHONE)
class ScreensTest {
    private val dir = File(System.getenv("HIBYOS_SHOTS") ?: System.getProperty("hibyos.shots") ?: "build/screens")
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

    private val bundle = BundleInfo(
        hibyosVersion = "0.6.1",
        generatedFrom = "f08b218",
        builtIn = true,
        firmwareLabel = "HiBy R3 II (2025), official firmware v1.3 of 2 March 2026",
        firmwareVersion = "1.3",
        firmwareFile = "r3ii_2025.upt",
        firmwareSize = 46_759_936,
        firmwarePage = "https://store.hiby.com/apps/help-center",
        firmwareBuild = "Mar 2 2026",
        canDownload = true,
        outName = "r3ii_2025.upt",
        overlayFiles = 145,
        sdFiles = 206,
        sdSize = 7_101_010,
        port = 4399,
        notices = listOf(
            NoticeItem("ByOS: licence (GPL-3.0-or-later)", "notices/LICENSE"),
            NoticeItem("Third-party notices", "notices/THIRD_PARTY_NOTICES.md"),
            NoticeItem("GNU General Public License 2.0", "notices/LICENSES/GPL-2.0.txt"),
            NoticeItem("GNU Lesser General Public License 2.1", "notices/LICENSES/LGPL-2.1.txt"),
            NoticeItem("Apache License 2.0", "notices/LICENSES/Apache-2.0.txt"),
            NoticeItem("MIT License", "notices/LICENSES/MIT.txt"),
            NoticeItem("SIL Open Font License 1.1", "notices/LICENSES/OFL-1.1.txt"),
        ),
        canUpdate = true,
    )

    private val sha = "764f0c6724675d4c0009c065576fd8a5d21564396f531a1440f261d46dd8b81a"
    private val base = UiState(bundle = bundle, appVersion = "1.0.0")
    private val ready = base.copy(firmware = FirmwareState.Ready(46_759_936, sha))
    private val patched = ready.copy(patch = PatchState.Done(47_972_352,
        "375238efbc682f7dd15919cf2cfda3610dff2d59327f94697ad301c9c1e61291", "0.6.1-f08b218", 145, "0.6.1"))
    private val player = FoundPlayer("192.168.1.20", 4399, "R3II_2025", true)
    private val total = 47_972_352L + 7_101_010L

    /** One picture at phone size; with [full] also one tall enough to show the whole page. */
    private fun shot(name: String, screen: Screen, state: UiState, full: Boolean = false) {
        val s = state.copy(stack = if (screen == Screen.HOME) listOf(Screen.HOME) else listOf(Screen.HOME, screen))
        RuntimeEnvironment.setQualifiers(PHONE)
        captureRoboImage(File(dir, "$name.png").path, options) {
            Box(Modifier.fillMaxSize().background(C.bg)) { ScreenContent(screen, s, nothing) }
        }
        if (full) {
            RuntimeEnvironment.setQualifiers(TALL)
            captureRoboImage(File(File(dir, "full"), "$name.png").path, options) {
                Box(Modifier.fillMaxSize().background(C.bg)) { ScreenContent(screen, s, nothing) }
            }
            RuntimeEnvironment.setQualifiers(PHONE)
        }
    }

    @Test
    fun home() {
        shot("home_1_loading", Screen.HOME, UiState(appVersion = "1.0.0"))
        shot("home_2_start", Screen.HOME, base, full = true)
        shot("home_3_firmware_ready", Screen.HOME, ready)
        shot("home_4_built", Screen.HOME, patched)
        shot("home_5_sent", Screen.HOME, patched.copy(delivered = true), full = true)
        shot("home_6_start_error", Screen.HOME, UiState(appVersion = "1.0.0",
            startError = ErrorInfo(R.string.err_patch_bundle, "bundle.json: unexpected end of the text", R.string.hint_patch_bundle)))
    }

    @Test
    fun check() {
        shot("check", Screen.CHECK, base, full = true)
    }

    @Test
    fun firmware() {
        shot("firmware_1_idle", Screen.FIRMWARE, base, full = true)
        shot("firmware_2_downloading", Screen.FIRMWARE, base.copy(firmware = FirmwareState.Downloading(19_922_944, 46_759_936)))
        shot("firmware_3_checking", Screen.FIRMWARE, base.copy(firmware = FirmwareState.Checking(0.62f)))
        shot("firmware_4_ready", Screen.FIRMWARE, ready, full = true)
        shot("firmware_5_wrong_file", Screen.FIRMWARE, base.copy(
            firmware = FirmwareState.Wrong("r3ii_2025 (1).upt", 46_211_072, "5d41402abc4b2a76b9719d911017c592")), full = true)
        shot("firmware_6_download_failed", Screen.FIRMWARE, base.copy(firmware = FirmwareState.Failed(
            ErrorInfo(R.string.err_offline, "Unable to resolve host \"drive.usercontent.google.com\"", R.string.hint_offline), true)), full = true)
        shot("firmware_7_file_failed", Screen.FIRMWARE, base.copy(firmware = FirmwareState.Failed(
            ErrorInfo(R.string.err_read, "Permission denied"), false)))
    }

    @Test
    fun patch() {
        shot("patch_1_no_firmware", Screen.PATCH, base)
        shot("patch_2_idle", Screen.PATCH, ready, full = true)
        shot("patch_3_running", Screen.PATCH, ready.copy(patch = PatchState.Running(Stage.BUILD, 0.47f)))
        shot("patch_4_running_end", Screen.PATCH, ready.copy(patch = PatchState.Running(Stage.VERIFY_UPT, 0.96f)))
        shot("patch_5_done", Screen.PATCH, patched, full = true)
        shot("patch_6_failed", Screen.PATCH, ready.copy(patch = PatchState.Failed(
            ErrorInfo(R.string.err_patch_verify, "usr/bin/hiby_player.sh: the file in the firmware is not the expected one", R.string.hint_patch_verify))), full = true)
    }

    @Test
    fun send() {
        shot("send_1_not_built", Screen.SEND, ready)
        shot("send_2_choose", Screen.SEND, patched, full = true)
    }

    @Test
    fun wifi() {
        shot("wifi_1_instructions", Screen.WIFI, patched, full = true)
        shot("wifi_2_address_typed", Screen.WIFI, patched.copy(wifi = WifiState(address = "192.168.1.20")))
        shot("wifi_3_searching", Screen.WIFI, patched.copy(wifi = WifiState(phase = WifiPhase.Searching(97, 253))), full = true)
        shot("wifi_4_found", Screen.WIFI, patched.copy(wifi = WifiState(found = listOf(player,
            FoundPlayer("192.168.1.37", 4399, null, false)))), full = true)
        shot("wifi_5_connecting", Screen.WIFI, patched.copy(wifi = WifiState(address = "192.168.1.20", phase = WifiPhase.Connecting)))
        shot("wifi_6_connected", Screen.WIFI, patched.copy(wifi = WifiState("192.168.1.20", player = player, phase = WifiPhase.Connected(player))), full = true)
        shot("wifi_7_sending", Screen.WIFI, patched.copy(wifi = WifiState("192.168.1.20", player = player,
            phase = WifiPhase.Sending(31_457_280, total, "r3ii_2025.upt", SendListener.Phase.SEND))))
        shot("wifi_8_checking", Screen.WIFI, patched.copy(wifi = WifiState("192.168.1.20", player = player,
            phase = WifiPhase.Sending(total - 900_000, total, "ByOS/payload/theme/layout/theme3/hiby_launcher_apps.view", SendListener.Phase.CHECK))))
        shot("wifi_9_done", Screen.WIFI, patched.copy(delivered = true, wifi = WifiState("192.168.1.20", player = player,
            phase = WifiPhase.Done(207, 0, 207, false))), full = true)
        shot("wifi_10_done_resumed", Screen.WIFI, patched.copy(delivered = true, wifi = WifiState("192.168.1.20", player = player,
            phase = WifiPhase.Done(207, 64, 0, true))))
        shot("wifi_11_wrong_address", Screen.WIFI, patched.copy(wifi = WifiState("192.168.1.99", phase = WifiPhase.Failed(
            ErrorInfo(R.string.err_unreachable, "192.168.1.99:4399: connect timed out", R.string.hint_unreachable), false))), full = true)
        shot("wifi_12_not_a_player", Screen.WIFI, patched.copy(wifi = WifiState("192.168.1.1", phase = WifiPhase.Failed(
            ErrorInfo(R.string.err_not_player, "192.168.1.1:4399 does not answer like the player", R.string.hint_not_player), false))))
        shot("wifi_13_none_found", Screen.WIFI, patched.copy(wifi = WifiState(phase = WifiPhase.Failed(
            ErrorInfo(R.string.err_none_found, null, R.string.hint_none_found), false))), full = true)
        shot("wifi_14_connection_lost", Screen.WIFI, patched.copy(wifi = WifiState("192.168.1.20", player = player, phase = WifiPhase.Failed(
            ErrorInfo(R.string.err_interrupted, "the connection to the player broke (POST /upload)", R.string.hint_interrupted), true))), full = true)
        shot("wifi_15_card_full", Screen.WIFI, patched.copy(wifi = WifiState("192.168.1.20", player = player, phase = WifiPhase.Failed(
            ErrorInfo(R.string.err_refused, "sending r3ii_2025.upt: the player answered 500 (Write to file fail)", R.string.hint_refused), true))))
        shot("wifi_16_no_card", Screen.WIFI, patched.copy(wifi = WifiState("192.168.1.20", phase = WifiPhase.Failed(
            ErrorInfo(R.string.err_no_sd, "reading /data/mnt/sd_0/: the player answered 404 (Home not found!)", R.string.hint_no_sd), false))))
        shot("wifi_17_not_built", Screen.WIFI, ready)
    }

    @Test
    fun manual() {
        shot("manual_1_idle", Screen.MANUAL, patched, full = true)
        shot("manual_2_saving", Screen.MANUAL, patched.copy(manual = ManualState.Saving(21_495_808, total, "ByOS/payload/bin/hibyshell")))
        shot("manual_3_preparing", Screen.MANUAL, patched.copy(manual = ManualState.Preparing(0.4f)))
        shot("manual_4_saved", Screen.MANUAL, patched.copy(delivered = true, manual = ManualState.Saved("SD card", 207)), full = true)
        shot("manual_5_failed", Screen.MANUAL, patched.copy(manual = ManualState.Failed(
            ErrorInfo(R.string.err_save, "ByOS/payload/bin/hibyshell: No space left on device", R.string.hint_save))), full = true)
        shot("manual_6_not_built", Screen.MANUAL, ready)
    }

    @Test
    fun install() {
        shot("install", Screen.INSTALL, patched.copy(delivered = true), full = true)
        shot("install_before_sending", Screen.INSTALL, base)
    }

    @Test
    fun about() {
        shot("about_1", Screen.ABOUT, patched.copy(storageUsed = 101_711_872), full = true)
        shot("about_2_checking", Screen.ABOUT, base.copy(update = UpdateState.Checking))
        shot("about_3_up_to_date", Screen.ABOUT, base.copy(update = UpdateState.UpToDate("0.6.1")))
        shot("about_4_update_available", Screen.ABOUT, base.copy(update = UpdateState.Available("0.6.2", 4_935_265)))
        shot("about_5_update_downloading", Screen.ABOUT, base.copy(update = UpdateState.Downloading("0.6.2", 1_900_000, 4_935_265)))
        shot("about_6_update_installed", Screen.ABOUT, base.copy(
            bundle = bundle.copy(hibyosVersion = "0.6.2", builtIn = false), update = UpdateState.Installed("0.6.2")))
        shot("about_7_update_failed", Screen.ABOUT, base.copy(update = UpdateState.Failed(
            ErrorInfo(R.string.err_untrusted, "the signature of hibyos-install-bundle.json is not from a release key", R.string.hint_untrusted))), full = true)
    }

    @Test
    fun licences() {
        shot("licences_1_list", Screen.LICENSES, base, full = true)
        val text = noticeParagraphs(
            """
            # Third-party notices

            ByOS is free software under the GNU General Public License, version 3 or later.
            The files it adds to the player include, or are built from, the works below. Each of
            them stays under its own licence.

            dropbear
            --------

            A small SSH server and client. Copyright (c) 2002-2020 Matt Johnston and others.
            Licence: MIT.

            - it is started only when you turn it on in Settings
            - its source code is downloaded when ByOS is built

            | Component | Licence |
            |-----------|---------|
            | BlueZ | GPL-2.0-or-later |
            | Mbed TLS | Apache-2.0 |
            """.trimIndent(),
        )
        shot("licences_2_text", Screen.LICENSE_TEXT, base.copy(licenseTitle = "Third-party notices", licenseText = text))
        shot("licences_3_loading", Screen.LICENSE_TEXT, base.copy(licenseTitle = "GNU General Public License 2.0"))
    }
}

private const val PHONE = "w412dp-h892dp-xhdpi"
private const val TALL = "w412dp-h1900dp-xhdpi"
