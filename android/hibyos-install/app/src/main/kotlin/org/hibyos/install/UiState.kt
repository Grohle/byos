package org.hibyos.install

import org.hibyos.install.core.SendListener
import org.hibyos.install.core.Stage

enum class Screen { HOME, CHECK, FIRMWARE, PATCH, SEND, WIFI, MANUAL, INSTALL, ABOUT, LICENSES, LICENSE_TEXT }

/** An error ready to show: a headline and an advice from the resources, and the technical detail. */
data class ErrorInfo(val title: Int, val detail: String? = null, val hint: Int = 0, val hintArg: String? = null)

data class NoticeItem(val title: String, val entry: String)

/** What the screens need to know about the HiByOS files in use. */
data class BundleInfo(
    val hibyosVersion: String,
    val generatedFrom: String,
    val builtIn: Boolean,
    val firmwareLabel: String,
    val firmwareVersion: String,
    val firmwareFile: String,
    val firmwareSize: Long,
    val firmwarePage: String?,
    val firmwareBuild: String? = null,
    val canDownload: Boolean,
    val outName: String,
    val overlayFiles: Int,
    val sdFiles: Int,
    val sdSize: Long,
    val port: Int,
    val notices: List<NoticeItem>,
    val canUpdate: Boolean,
)

sealed interface FirmwareState {
    data object Idle : FirmwareState
    data class Downloading(val done: Long, val total: Long) : FirmwareState
    data class Checking(val fraction: Float) : FirmwareState
    data class Ready(val size: Long, val sha256: String) : FirmwareState
    data class Wrong(val name: String?, val size: Long, val md5: String) : FirmwareState
    data class Failed(val error: ErrorInfo, val wasDownload: Boolean) : FirmwareState
}

sealed interface PatchState {
    data object Idle : PatchState
    data class Running(val stage: Stage, val overall: Float) : PatchState
    data class Done(val size: Long, val sha256: String, val buildId: String, val files: Int, val version: String) : PatchState
    data class Failed(val error: ErrorInfo) : PatchState
}

data class FoundPlayer(val host: String, val port: Int, val name: String?, val sdCard: Boolean)

sealed interface WifiPhase {
    data object Idle : WifiPhase
    data class Searching(val done: Int, val total: Int) : WifiPhase
    data object Connecting : WifiPhase
    data class Connected(val player: FoundPlayer) : WifiPhase
    data class Sending(val done: Long, val total: Long, val path: String, val phase: SendListener.Phase) : WifiPhase
    data class Done(val files: Int, val skipped: Int, val verified: Int, val sizesOnly: Boolean) : WifiPhase
    data class Failed(val error: ErrorInfo, val canRetrySend: Boolean) : WifiPhase
}

data class WifiState(
    val address: String = "",
    val found: List<FoundPlayer> = emptyList(),
    val player: FoundPlayer? = null,
    val phase: WifiPhase = WifiPhase.Idle,
)

sealed interface ManualState {
    data object Idle : ManualState
    data class Saving(val done: Long, val total: Long, val path: String) : ManualState
    data class Preparing(val fraction: Float) : ManualState
    data class Saved(val folder: String, val files: Int) : ManualState
    data class Failed(val error: ErrorInfo) : ManualState
}

sealed interface UpdateState {
    data object Idle : UpdateState
    data object Checking : UpdateState
    data class UpToDate(val version: String) : UpdateState
    data class Available(val version: String, val size: Long) : UpdateState
    data class Downloading(val version: String, val done: Long, val total: Long) : UpdateState
    data class Installed(val version: String) : UpdateState
    data class Failed(val error: ErrorInfo) : UpdateState
}

data class UiState(
    /** Where the user is: the last one is on screen, "back" drops it. */
    val stack: List<Screen> = listOf(Screen.HOME),
    val bundle: BundleInfo? = null,
    val startError: ErrorInfo? = null,
    val appVersion: String = "",
    val firmware: FirmwareState = FirmwareState.Idle,
    val patch: PatchState = PatchState.Idle,
    val wifi: WifiState = WifiState(),
    val manual: ManualState = ManualState.Idle,
    val update: UpdateState = UpdateState.Idle,
    /** The files were sent or saved in this session. */
    val delivered: Boolean = false,
    val storageUsed: Long = 0,
    val licenseTitle: String = "",
    val licenseText: List<String> = emptyList(),
) {
    val screen: Screen get() = stack.last()
    val previous: Screen? get() = stack.getOrNull(stack.size - 2)

    /** Work is running: leaving the screen is held back until it ends or is cancelled. */
    val busy: Boolean
        get() = firmware is FirmwareState.Downloading || firmware is FirmwareState.Checking ||
            patch is PatchState.Running ||
            wifi.phase is WifiPhase.Searching || wifi.phase is WifiPhase.Connecting || wifi.phase is WifiPhase.Sending ||
            manual is ManualState.Saving || manual is ManualState.Preparing ||
            update is UpdateState.Checking || update is UpdateState.Downloading

    /** The step the user should do next: 1..4, or 5 when everything was done here. */
    val nextStep: Int
        get() = when {
            firmware !is FirmwareState.Ready && patch !is PatchState.Done -> 1
            patch !is PatchState.Done -> 2
            !delivered -> 3
            else -> 4
        }
}

/** Everything the screens can ask for. */
interface Actions {
    fun go(screen: Screen)
    fun back()
    fun cancel()
    fun openUrl(url: String)

    fun downloadFirmware()
    fun pickFirmware()
    fun forgetFirmware()

    fun patch()

    fun setAddress(text: String)
    fun findPlayers()
    fun connect(player: FoundPlayer? = null)
    fun changePlayer()
    fun send()

    fun saveToFolder()
    fun share()

    fun checkUpdate()
    fun downloadUpdate()
    fun openNotice(item: NoticeItem?)
    fun cleanUp()
}
