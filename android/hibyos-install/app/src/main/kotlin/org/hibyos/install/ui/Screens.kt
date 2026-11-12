package org.hibyos.install.ui

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.CubicBezierEasing
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import org.hibyos.install.Actions
import org.hibyos.install.BundleInfo
import org.hibyos.install.FirmwareState
import org.hibyos.install.ManualState
import org.hibyos.install.PatchState
import org.hibyos.install.R
import org.hibyos.install.Screen
import org.hibyos.install.UiState
import org.hibyos.install.UpdateState
import org.hibyos.install.WifiPhase
import org.hibyos.install.core.SendListener
import org.hibyos.install.core.Stage
import org.hibyos.install.mb

const val PROJECT_URL = "https://github.com/Grohle/hibyos"

/** The whole app: the screen on top of the stack, sliding like the screens of ByOS do. */
@Composable
fun App(state: UiState, actions: Actions) {
    AnimatedContent(
        targetState = state.stack.size to state.screen,
        modifier = Modifier.fillMaxSize().background(C.bg),
        transitionSpec = {
            val slide = tween<IntOffset>(340, easing = CubicBezierEasing(0.2f, 0.85f, 0.25f, 1f))
            if (targetState.first >= initialState.first) {
                slideInHorizontally(slide) { it } togetherWith slideOutHorizontally(slide) { -it / 4 } + fadeOut(tween(340))
            } else {
                (slideInHorizontally(slide) { -it / 4 } + fadeIn(tween(220)) togetherWith slideOutHorizontally(slide) { it })
                    .apply { targetContentZIndex = -1f }
            }
        },
        label = "screen",
    ) { target -> ScreenContent(target.second, state, actions) }
}

@Composable
fun ScreenContent(screen: Screen, s: UiState, a: Actions) {
    when (screen) {
        Screen.HOME -> Home(s, a)
        Screen.CHECK -> Check(s, a)
        Screen.FIRMWARE -> Firmware(s, a)
        Screen.PATCH -> Patch(s, a)
        Screen.SEND -> Send(s, a)
        Screen.WIFI -> Wifi(s, a)
        Screen.MANUAL -> Manual(s, a)
        Screen.INSTALL -> Install(s, a)
        Screen.ABOUT -> About(s, a)
        Screen.LICENSES -> Licenses(s, a)
        Screen.LICENSE_TEXT -> LicenseText(s, a)
        Screen.SYNC -> SyncHome(s, a)
        Screen.SYNC_SEND -> SyncSend(s, a)
        Screen.SYNC_FILES -> SyncFiles(s, a)
        Screen.SYNC_LIBRARY -> SyncLibrary(s, a)
        Screen.SYNC_UPDATE -> SyncUpdate(s, a)
        Screen.SYNC_INSTALL -> SyncInstall(s, a)
    }
}

private fun titleRes(screen: Screen): Int = when (screen) {
    Screen.HOME -> R.string.app_name
    Screen.CHECK -> R.string.check_title
    Screen.FIRMWARE -> R.string.fw_title
    Screen.PATCH -> R.string.patch_title
    Screen.SEND -> R.string.send_title
    Screen.WIFI -> R.string.wifi_title
    Screen.MANUAL -> R.string.manual_title
    Screen.INSTALL -> R.string.install_title
    Screen.ABOUT -> R.string.about_title
    Screen.LICENSES -> R.string.licenses_title
    Screen.LICENSE_TEXT -> R.string.licenses_title
    Screen.SYNC, Screen.SYNC_SEND, Screen.SYNC_FILES, Screen.SYNC_LIBRARY, Screen.SYNC_UPDATE,
    Screen.SYNC_INSTALL -> syncTitleRes(screen)
}

private fun stepScreen(n: Int): Screen = when (n) {
    1 -> Screen.FIRMWARE
    2 -> Screen.PATCH
    3 -> Screen.SEND
    else -> Screen.INSTALL
}

private fun buttons(content: @Composable ColumnScope.() -> Unit): @Composable ColumnScope.() -> Unit = content

/** The label of "back": the title of the screen it returns to when that is short, as ByOS does. */
@Composable
private fun backLabel(s: UiState, screen: Screen): String {
    val previous = remember { s.stack.getOrNull(s.stack.indexOf(screen) - 1) }
    val title = if (previous == null || previous == Screen.HOME) null else stringResource(titleRes(previous))
    return if (title != null && title.length <= 10) title else stringResource(R.string.back)
}

@Composable
private fun Sub(s: UiState, a: Actions, screen: Screen, bottom: (@Composable ColumnScope.() -> Unit)? = null,
                content: @Composable ColumnScope.() -> Unit) {
    Page(title = AnnotatedString(stringResource(titleRes(screen))), back = backLabel(s, screen),
        onBack = if (s.busy) null else a::back, bottom = bottom, content = content)
}

@Composable
private fun Notice(text: String, onClick: () -> Unit) {
    Section {
        row { Item(title = text, icon = { IconTile(Glyphs.Alert, C.orange) }, chevron = true, onClick = onClick) }
    }
}

private fun checksum(hex: String): String = hex.chunked(32).joinToString("\n") { it.chunked(8).joinToString(" ") }

// ---- home -----------------------------------------------------------------------------------

@Composable
private fun StepRow(n: Int, title: String, sub: String, state: Int, status: String?, onClick: () -> Unit) {
    Item(title = title, subtitle = sub, icon = { StepBadge(n, state) }, chevron = true,
        value = status, valueColor = C.green, onClick = onClick)
}

@Composable
private fun Home(s: UiState, a: Actions) {
    val b = s.bundle
    val v = b?.firmwareVersion ?: "1.3"
    val next = s.nextStep
    val bottom = if (b == null) null else buttons {
        PrimaryButton(stringResource(if (next == 1) R.string.home_start else R.string.go_on)) { a.go(stepScreen(next)) }
    }
    Page(title = wordmark(), trailing = { NavAction(stringResource(R.string.about_open)) { a.go(Screen.ABOUT) } }, bottom = bottom) {
        if (b != null) {
            Txt(stringResource(R.string.home_subtitle, b.hibyosVersion), T.lead, Modifier.padding(start = 20.dp, end = 20.dp, bottom = 22.dp))
        } else if (s.startError != null) {
            Txt(stringResource(R.string.home_start_error), T.lead, Modifier.padding(start = 20.dp, end = 20.dp, bottom = 22.dp))
            ErrorSection(s.startError)
        } else {
            Row(Modifier.padding(start = 20.dp, end = 20.dp, bottom = 22.dp), verticalAlignment = Alignment.CenterVertically) {
                Spinner(18.dp)
                Txt(stringResource(R.string.home_loading), T.lead, Modifier.padding(start = 10.dp))
            }
        }
        Section(header = stringResource(R.string.home_before), footer = stringResource(R.string.home_before_footer, v)) {
            row {
                Item(title = stringResource(R.string.home_req_player), subtitle = stringResource(R.string.home_req_player_sub, v),
                    icon = { IconTile(Glyphs.Player, C.purple) })
            }
            row(iconInset) {
                Item(title = stringResource(R.string.home_req_card), subtitle = stringResource(R.string.home_req_card_sub),
                    icon = { IconTile(Glyphs.Card, C.orange) })
            }
            row(iconInset) {
                Item(title = stringResource(R.string.home_req_battery), subtitle = stringResource(R.string.home_req_battery_sub),
                    icon = { IconTile(Glyphs.Battery, C.green) })
            }
            row(iconInset) {
                Item(title = stringResource(R.string.home_check), icon = { IconTile(Glyphs.Info, C.blue) }, chevron = true) { a.go(Screen.CHECK) }
            }
        }
        if (b != null) {
            val fwDone = s.firmware is FirmwareState.Ready || s.patch is PatchState.Done
            val patchDone = s.patch is PatchState.Done
            Section(header = stringResource(R.string.home_steps), footer = stringResource(R.string.home_footer)) {
                row {
                    StepRow(1, stringResource(R.string.step1_title), stringResource(R.string.step1_sub, v),
                        if (fwDone) 2 else if (next == 1) 1 else 0, if (fwDone) stringResource(R.string.status_ready) else null) { a.go(Screen.FIRMWARE) }
                }
                row(iconInset) {
                    StepRow(2, stringResource(R.string.step2_title), stringResource(R.string.step2_sub),
                        if (patchDone) 2 else if (next == 2) 1 else 0, if (patchDone) stringResource(R.string.status_ready) else null) { a.go(Screen.PATCH) }
                }
                row(iconInset) {
                    StepRow(3, stringResource(R.string.step3_title), stringResource(R.string.step3_sub),
                        if (s.delivered) 2 else if (next == 3) 1 else 0, if (s.delivered) stringResource(R.string.status_done) else null) { a.go(Screen.SEND) }
                }
                row(iconInset) {
                    StepRow(4, stringResource(R.string.step4_title), stringResource(R.string.step4_sub),
                        if (next == 4) 1 else 0, null) { a.go(Screen.INSTALL) }
                }
            }
        }
        Section(header = stringResource(R.string.sync_home_h), footer = stringResource(R.string.sync_home_footer)) {
            row {
                Item(title = stringResource(R.string.sync_title), subtitle = stringResource(R.string.sync_home_sub),
                    icon = { IconTile(Glyphs.Link, C.cyan) }, chevron = true) { a.go(Screen.SYNC) }
            }
        }
    }
}

// ---- before you start -----------------------------------------------------------------------

@Composable
private fun Check(s: UiState, a: Actions) {
    val b = s.bundle
    val v = b?.firmwareVersion ?: "1.3"
    val file = b?.firmwareFile ?: "r3ii_2025.upt"
    Sub(s, a, Screen.CHECK) {
        Section(header = stringResource(R.string.check_need_h)) {
            row { TextRow(stringResource(R.string.check_need, v)) }
        }
        Section(header = stringResource(R.string.check_how_h), footer = stringResource(R.string.menu_names_note)) {
            row { StepText(1, stringResource(R.string.check_how_1)) }
            row(stepInset) { StepText(2, stringResource(R.string.check_how_2)) }
            val build = b?.firmwareBuild
            row(stepInset) {
                StepText(3, if (build != null) stringResource(R.string.check_how_3_build, v, build) else stringResource(R.string.check_how_3, v))
            }
        }
        Section(header = stringResource(R.string.check_other_h), footer = stringResource(R.string.check_other_footer, v)) {
            row { StepText(1, stringResource(R.string.check_other_1)) }
            row(stepInset) { StepText(2, stringResource(R.string.check_other_2, v)) }
            row(stepInset) { StepText(3, stringResource(R.string.check_other_3, v, file)) }
            val page = b?.firmwarePage
            if (page != null) row {
                Item(title = stringResource(R.string.check_page), titleColor = C.blue,
                    trailing = { Glyph(Glyphs.External, C.blue, 18.dp) }) { a.openUrl(page) }
            }
        }
        Section(header = stringResource(R.string.check_card_h)) {
            row { TextRow(stringResource(R.string.check_card)) }
        }
    }
}

// ---- step 1 ---------------------------------------------------------------------------------

@Composable
private fun FirmwareChoices(b: BundleInfo, a: Actions) {
    Section(header = stringResource(R.string.fw_other_h), footer = stringResource(R.string.fw_get_footer, b.firmwareVersion)) {
        row {
            Item(title = stringResource(R.string.fw_pick), subtitle = stringResource(R.string.fw_pick_sub, b.firmwareFile),
                icon = { IconTile(Glyphs.Folder, C.blue) }, chevron = true) { a.pickFirmware() }
        }
        val page = b.firmwarePage
        if (page != null) row(iconInset) {
            Item(title = stringResource(R.string.fw_page), subtitle = stringResource(R.string.fw_page_sub),
                icon = { IconTile(Glyphs.External, C.purple) }, chevron = true) { a.openUrl(page) }
        }
    }
}

@Composable
private fun Firmware(s: UiState, a: Actions) {
    val b = s.bundle
    val fw = s.firmware
    val bottom = when {
        b == null -> null
        fw is FirmwareState.Downloading || fw is FirmwareState.Checking -> buttons { GreyButton(stringResource(R.string.cancel)) { a.cancel() } }
        fw is FirmwareState.Ready -> buttons { PrimaryButton(stringResource(R.string.go_on)) { a.go(Screen.PATCH) } }
        fw is FirmwareState.Failed -> buttons {
            PrimaryButton(stringResource(R.string.try_again)) { if (fw.wasDownload) a.downloadFirmware() else a.pickFirmware() }
        }
        b.canDownload -> buttons { PrimaryButton(stringResource(R.string.fw_download)) { a.downloadFirmware() } }
        else -> null
    }
    Sub(s, a, Screen.FIRMWARE, bottom) {
        if (b != null) when (fw) {
            is FirmwareState.Downloading -> ProgressCard(
                title = stringResource(R.string.fw_downloading),
                fraction = if (fw.total > 0) fw.done.toFloat() / fw.total else 0f,
                detail = stringResource(R.string.fw_progress, mb(fw.done), mb(fw.total)),
                note = stringResource(R.string.keep_open),
            )
            is FirmwareState.Checking -> ProgressCard(title = stringResource(R.string.fw_checking), fraction = fw.fraction,
                detail = b.firmwareFile)
            is FirmwareState.Ready -> {
                Hero(Glyphs.Check, C.green, stringResource(R.string.fw_ready_title, b.firmwareVersion), stringResource(R.string.fw_ready_sub))
                Section {
                    row { Item(title = stringResource(R.string.fw_file), value = b.firmwareFile) }
                    row { Item(title = stringResource(R.string.fw_size), value = mb(fw.size)) }
                    row { CodeItem(stringResource(R.string.fw_sha), checksum(fw.sha256)) }
                }
            }
            is FirmwareState.Wrong -> {
                Hero(Glyphs.Cross, C.red, stringResource(R.string.fw_wrong_title, b.firmwareVersion),
                    stringResource(R.string.fw_wrong_text, b.firmwareFile))
                Section(header = stringResource(R.string.fw_wrong_h)) {
                    if (fw.name != null) row { Item(title = stringResource(R.string.fw_wrong_name), value = fw.name) }
                    row { Item(title = stringResource(R.string.fw_size), value = mb(fw.size)) }
                    row { CodeItem(stringResource(R.string.fw_wrong_md5), fw.md5) }
                }
                FirmwareChoices(b, a)
            }
            is FirmwareState.Failed -> {
                ErrorSection(fw.error)
                FirmwareChoices(b, a)
            }
            FirmwareState.Idle -> {
                Section(header = stringResource(R.string.fw_h), footer = stringResource(R.string.fw_footer)) {
                    row { Item(title = stringResource(R.string.fw_file), value = b.firmwareFile) }
                    row { Item(title = stringResource(R.string.fw_version), value = b.firmwareVersion) }
                    if (b.firmwareSize > 0) row { Item(title = stringResource(R.string.fw_size), value = mb(b.firmwareSize)) }
                }
                FirmwareChoices(b, a)
            }
        }
    }
}

// ---- step 2 ---------------------------------------------------------------------------------

private fun stageRes(stage: Stage): Int = when (stage) {
    Stage.CHECK -> R.string.stage_check
    Stage.READ -> R.string.stage_read
    Stage.APPLY -> R.string.stage_apply
    Stage.BUILD -> R.string.stage_build
    Stage.VERIFY_ROOTFS -> R.string.stage_verify_rootfs
    Stage.WRITE -> R.string.stage_write
    Stage.VERIFY_UPT -> R.string.stage_verify_upt
}

@Composable
private fun StageRow(text: String, state: Int) {
    Row(Modifier.fillMaxWidth().heightIn(min = 42.dp).padding(horizontal = 16.dp, vertical = 9.dp), verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(22.dp), contentAlignment = Alignment.Center) {
            when (state) {
                2 -> Glyph(Glyphs.Check, C.green, 18.dp)
                1 -> Spinner(18.dp, C.blue)
                else -> Canvas(Modifier.size(8.dp)) { drawCircle(C.tertiary, style = Stroke(1.5.dp.toPx())) }
            }
        }
        Txt(text, T.callout.copy(color = when (state) {
            2 -> C.secondary
            1 -> C.label
            else -> C.tertiary
        }), Modifier.padding(start = 12.dp))
    }
}

@Composable
private fun Patch(s: UiState, a: Actions) {
    val b = s.bundle
    val p = s.patch
    val hasFirmware = s.firmware is FirmwareState.Ready
    val bottom = when {
        b == null -> null
        p is PatchState.Running -> buttons { GreyButton(stringResource(R.string.cancel)) { a.cancel() } }
        p is PatchState.Done -> buttons {
            PrimaryButton(stringResource(R.string.go_on)) { a.go(Screen.SEND) }
            if (hasFirmware) LinkButton(stringResource(R.string.patch_again)) { a.patch() }
        }
        !hasFirmware -> buttons { PrimaryButton(stringResource(R.string.step1_sub, b.firmwareVersion)) { a.go(Screen.FIRMWARE) } }
        p is PatchState.Failed -> buttons { PrimaryButton(stringResource(R.string.try_again)) { a.patch() } }
        else -> buttons { PrimaryButton(stringResource(R.string.patch_start, b.hibyosVersion)) { a.patch() } }
    }
    Sub(s, a, Screen.PATCH, bottom) {
        if (b != null) when (p) {
            is PatchState.Running -> {
                ProgressCard(title = stringResource(R.string.patch_running, b.hibyosVersion), fraction = p.overall,
                    note = stringResource(R.string.keep_open))
                Section {
                    for (st in Stage.entries) row(50.dp) {
                        StageRow(stringResource(stageRes(st)), if (st.ordinal < p.stage.ordinal) 2 else if (st == p.stage) 1 else 0)
                    }
                }
            }
            is PatchState.Done -> {
                Hero(Glyphs.Check, C.green, stringResource(R.string.patch_done_title, p.version), stringResource(R.string.patch_done_sub, p.files))
                Section(header = stringResource(R.string.patch_out_h)) {
                    row { Item(title = stringResource(R.string.fw_file), value = b.outName) }
                    row { Item(title = stringResource(R.string.fw_size), value = mb(p.size)) }
                    row { Item(title = stringResource(R.string.patch_row_build), value = p.buildId) }
                    row { CodeItem(stringResource(R.string.fw_sha), checksum(p.sha256)) }
                }
            }
            else -> {
                if (p is PatchState.Failed) ErrorSection(p.error)
                if (!hasFirmware) {
                    Notice(stringResource(R.string.patch_need_fw)) { a.go(Screen.FIRMWARE) }
                } else {
                    Section(header = stringResource(R.string.patch_h)) {
                        row { TextRow(stringResource(R.string.patch_intro)) }
                    }
                }
                Section {
                    row { Item(title = stringResource(R.string.patch_row_version), value = b.hibyosVersion) }
                    row { Item(title = stringResource(R.string.patch_row_files), value = b.overlayFiles.toString()) }
                    row { Item(title = stringResource(R.string.patch_row_base), value = stringResource(R.string.patch_row_base_value, b.firmwareVersion)) }
                }
            }
        }
    }
}

// ---- step 3 ---------------------------------------------------------------------------------

@Composable
private fun WhatGoes(b: BundleInfo, size: Long, footer: String?) {
    Section(header = stringResource(R.string.send_what_h), footer = footer) {
        row {
            Item(title = b.outName, subtitle = stringResource(R.string.send_item_fw_sub, mb(size)), icon = { IconTile(Glyphs.Doc, C.blue) })
        }
        row(iconInset) {
            Item(title = stringResource(R.string.send_item_folder), subtitle = stringResource(R.string.send_item_folder_sub, b.sdFiles, mb(b.sdSize)),
                icon = { IconTile(Glyphs.Folder, C.cyan) })
        }
    }
}

@Composable
private fun Send(s: UiState, a: Actions) {
    val b = s.bundle
    val p = s.patch
    Sub(s, a, Screen.SEND) {
        if (b != null) {
            if (p !is PatchState.Done) {
                Notice(stringResource(R.string.send_need_patch)) { a.go(Screen.PATCH) }
            } else {
                WhatGoes(b, p.size, stringResource(R.string.send_what_footer))
                Section(header = stringResource(R.string.send_how_h), footer = stringResource(R.string.send_how_footer)) {
                    row {
                        Item(title = stringResource(R.string.send_wifi), subtitle = stringResource(R.string.send_wifi_sub),
                            icon = { IconTile(Glyphs.Wifi, C.blue) }, chevron = true) { a.go(Screen.WIFI) }
                    }
                    row(iconInset) {
                        Item(title = stringResource(R.string.send_manual), subtitle = stringResource(R.string.send_manual_sub),
                            icon = { IconTile(Glyphs.Card, C.orange) }, chevron = true) { a.go(Screen.MANUAL) }
                    }
                }
            }
        }
    }
}

private fun phaseRes(phase: SendListener.Phase): Int = when (phase) {
    SendListener.Phase.PREPARE -> R.string.phase_prepare
    SendListener.Phase.SEND -> R.string.phase_send
    SendListener.Phase.CHECK -> R.string.phase_check
}

@Composable
private fun Wifi(s: UiState, a: Actions) {
    val b = s.bundle
    val p = s.patch
    val w = s.wifi
    val ph = w.phase
    val total = if (b != null && p is PatchState.Done) p.size + b.sdSize else 0L
    val bottom = when {
        b == null || p !is PatchState.Done -> null
        ph is WifiPhase.Searching || ph is WifiPhase.Sending -> buttons { GreyButton(stringResource(R.string.cancel)) { a.cancel() } }
        ph is WifiPhase.Connecting -> null
        ph is WifiPhase.Connected -> buttons {
            PrimaryButton(stringResource(R.string.wifi_send, mb(total))) { a.send() }
            LinkButton(stringResource(R.string.wifi_change)) { a.changePlayer() }
        }
        ph is WifiPhase.Done -> buttons { PrimaryButton(stringResource(R.string.go_on)) { a.go(Screen.INSTALL) } }
        ph is WifiPhase.Failed && ph.canRetrySend -> buttons {
            PrimaryButton(stringResource(R.string.try_again)) { a.send() }
            LinkButton(stringResource(R.string.wifi_use_manual)) { a.go(Screen.MANUAL) }
        }
        else -> buttons { PrimaryButton(stringResource(R.string.wifi_connect), enabled = w.address.isNotBlank()) { a.connect() } }
    }
    Sub(s, a, Screen.WIFI, bottom) {
        if (b == null) return@Sub
        if (p !is PatchState.Done) {
            Notice(stringResource(R.string.send_need_patch)) { a.go(Screen.PATCH) }
            return@Sub
        }
        when (ph) {
            is WifiPhase.Sending -> ProgressCard(
                title = stringResource(R.string.wifi_sending),
                fraction = if (ph.total > 0) ph.done.toFloat() / ph.total else 0f,
                detail = stringResource(phaseRes(ph.phase)),
                detail2 = ph.path.ifEmpty { " " },
                note = stringResource(R.string.wifi_sending_note),
            )
            is WifiPhase.Done -> {
                val text = (if (ph.sizesOnly) stringResource(R.string.wifi_done_sizes, ph.files)
                else stringResource(R.string.wifi_done_verified, ph.files, ph.verified)) +
                    (if (ph.skipped > 0) " " + stringResource(R.string.wifi_done_skipped, ph.skipped) else "")
                Hero(Glyphs.Check, C.green, stringResource(R.string.wifi_done_title), text)
                WhatGoes(b, p.size, null)
            }
            is WifiPhase.Connected -> {
                val name = ph.player.name ?: stringResource(R.string.wifi_player_default)
                Hero(Glyphs.Wifi, C.blue, stringResource(R.string.wifi_connected_title, name),
                    ph.player.host + ":" + ph.player.port + "  ·  " + stringResource(R.string.wifi_card_ok))
                WhatGoes(b, p.size, stringResource(R.string.send_what_footer))
            }
            is WifiPhase.Connecting -> Section {
                row { Item(title = stringResource(R.string.wifi_connecting), subtitle = w.address, trailing = { Spinner() }) }
            }
            else -> {
                val failed = ph as? WifiPhase.Failed
                if (failed != null) ErrorSection(failed.error)
                if (failed != null && failed.canRetrySend) {
                    Section {
                        row {
                            Item(title = stringResource(R.string.wifi_change), titleColor = C.blue) { a.changePlayer() }
                        }
                    }
                    return@Sub
                }
                val searching = ph as? WifiPhase.Searching
                Section(header = stringResource(R.string.wifi_player_h), footer = stringResource(R.string.wifi_player_footer)) {
                    row { StepText(1, stringResource(R.string.wifi_p1)) }
                    row(stepInset) { StepText(2, stringResource(R.string.wifi_p2)) }
                    row(stepInset) { StepText(3, stringResource(R.string.wifi_p3, b.port)) }
                }
                Section(header = stringResource(R.string.wifi_phone_h), footer = stringResource(R.string.wifi_phone_footer, b.port)) {
                    row {
                        Field(value = w.address, hint = stringResource(R.string.wifi_address_hint), enabled = searching == null,
                            onGo = { a.connect() }, onChange = { a.setAddress(it) })
                    }
                    row {
                        if (searching != null) {
                            Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 11.dp)) {
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    Txt(stringResource(R.string.wifi_searching), T.body, Modifier.weight(1f))
                                    Txt(stringResource(R.string.percent, if (searching.total > 0) searching.done * 100 / searching.total else 0), T.value)
                                }
                                ProgressBar(if (searching.total > 0) searching.done.toFloat() / searching.total else 0f, Modifier.padding(top = 10.dp, bottom = 3.dp))
                            }
                        } else {
                            Item(title = stringResource(R.string.wifi_find), titleColor = C.blue,
                                trailing = { Glyph(Glyphs.Search, C.blue, 20.dp) }) { a.findPlayers() }
                        }
                    }
                }
                if (w.found.isNotEmpty()) {
                    Section(header = stringResource(R.string.wifi_found_h)) {
                        w.found.forEachIndexed { i, player ->
                            row(if (i == 0) 16.dp else iconInset) {
                                Item(title = player.name ?: player.host,
                                    subtitle = player.host + "  ·  " + stringResource(if (player.sdCard) R.string.wifi_card_ok else R.string.wifi_card_none),
                                    icon = { IconTile(Glyphs.Player, C.purple) }, chevron = true, enabled = searching == null) { a.connect(player) }
                            }
                        }
                    }
                }
                if (failed != null) {
                    Section {
                        row { Item(title = stringResource(R.string.wifi_use_manual), titleColor = C.blue, chevron = true) { a.go(Screen.MANUAL) } }
                    }
                }
            }
        }
    }
}

@Composable
private fun Manual(s: UiState, a: Actions) {
    val b = s.bundle
    val p = s.patch
    val m = s.manual
    val bottom = when {
        b == null || p !is PatchState.Done -> null
        m is ManualState.Saving || m is ManualState.Preparing -> buttons { GreyButton(stringResource(R.string.cancel)) { a.cancel() } }
        else -> buttons {
            PrimaryButton(stringResource(if (m is ManualState.Saved) R.string.go_on else R.string.manual_next)) { a.go(Screen.INSTALL) }
        }
    }
    Sub(s, a, Screen.MANUAL, bottom) {
        if (b == null) return@Sub
        if (p !is PatchState.Done) {
            Notice(stringResource(R.string.send_need_patch)) { a.go(Screen.PATCH) }
            return@Sub
        }
        when (m) {
            is ManualState.Saving -> ProgressCard(title = stringResource(R.string.manual_saving),
                fraction = if (m.total > 0) m.done.toFloat() / m.total else 0f, detail2 = m.path.ifEmpty { " " },
                note = stringResource(R.string.keep_open))
            is ManualState.Preparing -> ProgressCard(title = stringResource(R.string.manual_preparing), fraction = m.fraction,
                note = stringResource(R.string.keep_open))
            else -> {
                if (m is ManualState.Saved) {
                    Hero(Glyphs.Check, C.green, stringResource(R.string.manual_saved_title, m.folder),
                        stringResource(R.string.manual_saved_sub, b.outName, b.sdFiles))
                }
                if (m is ManualState.Failed) ErrorSection(m.error)
                Section(header = stringResource(R.string.manual_save_h), footer = stringResource(R.string.manual_save_footer)) {
                    row {
                        Item(title = stringResource(R.string.manual_save), subtitle = stringResource(R.string.manual_save_sub),
                            icon = { IconTile(Glyphs.Folder, C.blue) }, chevron = true) { a.saveToFolder() }
                    }
                    row(iconInset) {
                        Item(title = stringResource(R.string.manual_share), subtitle = stringResource(R.string.manual_share_sub, b.outName),
                            icon = { IconTile(Glyphs.Share, C.green) }, chevron = true) { a.share() }
                    }
                }
                Section(header = stringResource(R.string.manual_card_h)) {
                    row { StepText(1, stringResource(R.string.manual_c1, b.outName)) }
                    row(stepInset) { StepText(2, stringResource(R.string.manual_c2)) }
                    row(stepInset) { StepText(3, stringResource(R.string.manual_c3, b.outName)) }
                    row(stepInset) { StepText(4, stringResource(R.string.manual_c4)) }
                }
            }
        }
    }
}

// ---- step 4 ---------------------------------------------------------------------------------

@Composable
private fun Install(s: UiState, a: Actions) {
    val b = s.bundle
    val v = b?.firmwareVersion ?: "1.3"
    val out = b?.outName ?: "r3ii_2025.upt"
    val file = b?.firmwareFile ?: "r3ii_2025.upt"
    Sub(s, a, Screen.INSTALL, buttons { PrimaryButton(stringResource(R.string.install_done)) { a.go(Screen.HOME) } }) {
        Section(header = stringResource(R.string.install_check_h)) {
            row { CheckText(stringResource(R.string.install_k1, out)) }
            row(stepInset) { CheckText(stringResource(R.string.install_k2)) }
            row(stepInset) { CheckText(stringResource(R.string.install_k3, v)) }
        }
        Section(header = stringResource(R.string.install_h),
            footer = stringResource(R.string.install_footer, out) + " " + stringResource(R.string.menu_names_note)) {
            row { StepText(1, stringResource(R.string.install_1)) }
            row(stepInset) { StepText(2, stringResource(R.string.install_2)) }
            row(stepInset) { StepText(3, stringResource(R.string.install_3)) }
            row(stepInset) { StepText(4, stringResource(R.string.install_4)) }
            row(stepInset) { StepText(5, stringResource(R.string.install_5)) }
        }
        Section(header = stringResource(R.string.install_back_h)) {
            row { TextRow(stringResource(R.string.install_back, file)) }
        }
        if (s.storageUsed > 0 && !s.busy) {
            Section {
                row {
                    Item(title = stringResource(R.string.install_cleanup, mb(s.storageUsed)), subtitle = stringResource(R.string.install_cleanup_sub),
                        icon = { IconTile(Glyphs.Trash, C.grey) }) { a.cleanUp() }
                }
            }
        }
    }
}

// ---- about ----------------------------------------------------------------------------------

@Composable
private fun About(s: UiState, a: Actions) {
    val b = s.bundle
    Sub(s, a, Screen.ABOUT) {
        Section {
            row { Item(title = stringResource(R.string.about_app_version), value = s.appVersion) }
            if (b != null) {
                row {
                    Item(title = stringResource(R.string.about_hibyos_version), value = b.hibyosVersion,
                        subtitle = stringResource(if (b.builtIn) R.string.about_builtin else R.string.about_updated))
                }
                row { Item(title = stringResource(R.string.about_source), value = b.generatedFrom) }
            }
        }
        if (b != null && b.canUpdate) {
            val u = s.update
            Section(footer = stringResource(R.string.about_update_footer)) {
                when (u) {
                    UpdateState.Checking -> row { Item(title = stringResource(R.string.update_checking), trailing = { Spinner() }) }
                    is UpdateState.Available -> {
                        row { Item(title = stringResource(R.string.update_available, u.version), icon = { IconTile(Glyphs.Download, C.green) }) }
                        row(iconInset) {
                            Item(title = stringResource(R.string.update_download, u.version, mb(u.size)), titleColor = C.blue) { a.downloadUpdate() }
                        }
                    }
                    is UpdateState.Downloading -> row {
                        Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 11.dp)) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Txt(stringResource(R.string.update_downloading, u.version), T.body, Modifier.weight(1f))
                                Txt(stringResource(R.string.percent, if (u.total > 0) (u.done * 100 / u.total).toInt() else 0), T.value)
                            }
                            ProgressBar(if (u.total > 0) u.done.toFloat() / u.total else 0f, Modifier.padding(top = 10.dp, bottom = 3.dp))
                        }
                    }
                    else -> {
                        row {
                            Item(title = stringResource(R.string.about_update), titleColor = C.blue,
                                icon = { IconTile(Glyphs.Refresh, C.blue) }) { a.checkUpdate() }
                        }
                        if (u is UpdateState.UpToDate) row(iconInset) { TextRow(stringResource(R.string.update_uptodate, u.version), T.sub) }
                        if (u is UpdateState.Installed) row(iconInset) { TextRow(stringResource(R.string.update_installed, u.version), T.sub.copy(color = C.green)) }
                    }
                }
            }
            if (u is UpdateState.Failed) ErrorSection(u.error)
        }
        Section(header = stringResource(R.string.about_net_h), footer = stringResource(R.string.about_net_footer)) {
            row { TextRow(stringResource(R.string.about_net_1), T.sub.copy(fontSize = T.callout.fontSize, lineHeight = T.callout.lineHeight)) }
            row { TextRow(stringResource(R.string.about_net_2), T.sub.copy(fontSize = T.callout.fontSize, lineHeight = T.callout.lineHeight)) }
            row { TextRow(stringResource(R.string.about_net_3), T.sub.copy(fontSize = T.callout.fontSize, lineHeight = T.callout.lineHeight)) }
        }
        Section {
            row {
                Item(title = stringResource(R.string.about_licenses), icon = { IconTile(Glyphs.Book, C.grey) }, chevron = true) { a.go(Screen.LICENSES) }
            }
            row(iconInset) {
                Item(title = stringResource(R.string.about_project), subtitle = stringResource(R.string.about_project_value),
                    icon = { IconTile(Glyphs.External, C.purple) }, chevron = true) { a.openUrl(PROJECT_URL) }
            }
        }
        if (s.storageUsed > 0 && !s.busy) {
            Section {
                row {
                    Item(title = stringResource(R.string.about_storage), subtitle = stringResource(R.string.about_storage_sub, mb(s.storageUsed)),
                        icon = { IconTile(Glyphs.Trash, C.red) }) { a.cleanUp() }
                }
            }
        }
        Txt(stringResource(R.string.about_legal), T.footer.copy(textAlign = TextAlign.Center),
            Modifier.fillMaxWidth().padding(start = 32.dp, end = 32.dp, bottom = 16.dp))
    }
}

@Composable
private fun Licenses(s: UiState, a: Actions) {
    val notices = s.bundle?.notices ?: emptyList()
    Sub(s, a, Screen.LICENSES) {
        Section(header = stringResource(R.string.licenses_hibyos_h), footer = stringResource(R.string.licenses_hibyos_footer)) {
            if (notices.isEmpty()) row { TextRow(stringResource(R.string.licenses_none), T.sub) }
            for (n in notices) row { Item(title = n.title, chevron = true) { a.openNotice(n) } }
        }
        Section(header = stringResource(R.string.licenses_app_h)) {
            row {
                Item(title = stringResource(R.string.licenses_android), subtitle = stringResource(R.string.licenses_android_sub), chevron = true) { a.openNotice(null) }
            }
        }
    }
}

@Composable
private fun LicenseText(s: UiState, a: Actions) {
    val list = rememberLazyListState()
    val collapsed by remember { derivedStateOf { list.firstVisibleItemIndex > 0 || list.firstVisibleItemScrollOffset > 80 } }
    PageFrame(AnnotatedString(s.licenseTitle), backLabel(s, Screen.LICENSE_TEXT), a::back, collapsed) {
        LazyColumn(Modifier.fillMaxSize(), state = list, contentPadding = PaddingValues(start = 20.dp, end = 20.dp, bottom = 28.dp)) {
            item { Txt(s.licenseTitle, T.largeTitle, Modifier.padding(top = 2.dp, bottom = 14.dp)) }
            items(s.licenseText.size) { i ->
                val p = s.licenseText[i]
                if (p.startsWith("# ")) Txt(p.substring(2), T.headline, Modifier.padding(top = 12.dp, bottom = 8.dp))
                else Txt(p, T.license, Modifier.padding(bottom = 11.dp))
            }
        }
    }
}

@Suppress("unused")
private val unusedSpacer: @Composable () -> Unit = { Spacer(Modifier.width(0.dp)) }
