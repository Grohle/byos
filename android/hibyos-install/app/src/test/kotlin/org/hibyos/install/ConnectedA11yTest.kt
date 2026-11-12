package org.hibyos.install

import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.SemanticsMatcher
import androidx.compose.ui.test.assert
import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.assertIsOff
import androidx.compose.ui.test.assertIsSelected
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onAllNodesWithContentDescription
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import org.hibyos.install.connected.Browser
import org.hibyos.install.connected.LibraryNav
import org.hibyos.install.connected.Link
import org.hibyos.install.connected.SyncDevice
import org.hibyos.install.connected.SyncDeviceInfo
import org.hibyos.install.connected.SyncEntry
import org.hibyos.install.connected.SyncSpace
import org.hibyos.install.connected.SyncState
import org.hibyos.install.ui.LocalStill
import org.hibyos.install.ui.LocalSync
import org.hibyos.install.ui.NoSync
import org.hibyos.install.ui.ScreenContent
import org.hibyos.install.ui.SyncUi
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/** What TalkBack finds on the connected screens: names for the icon buttons, roles, headings. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class ConnectedA11yTest {
    @get:Rule
    val rule = createComposeRule()

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

    private val dev = SyncDevice("192.168.1.20", 7340, "0123456789abcdef", "R3II_2025", true, true, true)
    private val on = SyncState(available = true, link = Link.Connected(dev, SyncDeviceInfo(dev.deviceId, "R3II_2025", "0.9.0", null, false, true, true, 0x1ff),
        SyncSpace(0, 1, 64L shl 30, 20L shl 30, 1L shl 30, 300L shl 20)))

    private fun show(screen: Screen, sync: SyncState) {
        rule.setContent {
            CompositionLocalProvider(LocalSync provides SyncUi(sync, NoSync), LocalStill provides true) {
                ScreenContent(screen, UiState(stack = listOf(Screen.HOME, Screen.SYNC, screen)), nothing)
            }
        }
    }

    @Test
    fun fileRowsHaveANamedMoreButton() {
        val entries = listOf(SyncEntry("ABBA", 1, 0, 0), SyncEntry("a.flac", 0, 10, 0), SyncEntry("b.flac", 0, 10, 0))
        show(Screen.SYNC_FILES, on.copy(browser = Browser("Music", entries)))
        rule.onAllNodesWithContentDescription("More actions").assertCountEquals(3)
        // the touch target is at least 44 dp
        val node = rule.onAllNodesWithContentDescription("More actions").fetchSemanticsNodes()[0]
        val dp = node.size.width / (node.layoutInfo.density.density)
        assert(dp >= 44f) { "more button is $dp dp" }
    }

    @Test
    fun replaceIsASwitch() {
        show(Screen.SYNC_SEND, on)
        rule.onNodeWithContentDescription("Replace files already there")
            .assert(SemanticsMatcher.expectValue(SemanticsProperties.Role, Role.Switch)).assertIsOff()
    }

    @Test
    fun libraryTabsAreSelectableTabs() {
        val lib = org.hibyos.install.connected.Library.build(listOf(org.hibyos.install.connected.Track("a:\\A\\B\\1.flac", "One", "A", "", "B")))
        show(Screen.SYNC_LIBRARY, on.copy(library = org.hibyos.install.connected.LibraryPhase.Ready(lib), libraryNav = LibraryNav(tab = 1)))
        rule.onNodeWithText("Albums").assertIsSelected()
        val tabs = rule.onAllNodes(SemanticsMatcher.expectValue(SemanticsProperties.Role, Role.Tab)).fetchSemanticsNodes()
        assertEquals(4, tabs.size)
        rule.onNodeWithContentDescription("Artists, albums and songs").performClick()
    }

    @Test
    fun pairingCodeIsDescribed() {
        show(Screen.SYNC, SyncState(available = true, link = Link.Pairing(dev, "815")))
        rule.onNodeWithContentDescription("Code shown on the player, 3 of 6 digits typed").performClick()
    }
}
