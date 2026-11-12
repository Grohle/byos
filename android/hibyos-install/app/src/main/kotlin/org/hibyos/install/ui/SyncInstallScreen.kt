package org.hibyos.install.ui

import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import org.hibyos.install.Actions
import org.hibyos.install.R
import org.hibyos.install.Screen
import org.hibyos.install.UiState
import org.hibyos.install.connected.OtaPhase
import org.hibyos.install.connected.SyncDeviceInfo

/**
 * Installs a signed ByOS update (hibyos-update.json and its payload tar) from the phone: the
 * files go to the player's update area, the player checks them, asks the user and installs.
 */
@Composable
fun SyncInstall(s: UiState, a: Actions) {
    val ui = LocalSync.current
    val st = ui.state
    val x = ui.actions
    val pick = st.otaPick
    val ota = st.ota
    val link = st.connected
    val supported = link == null || link.info.can(SyncDeviceInfo.CAP_UPDATE)
    val bottom: (@Composable ColumnScope.() -> Unit)? = when (ota) {
        is OtaPhase.Sending -> { { GreyButton(stringResource(R.string.cancel)) { x.cancelWork() } } }
        is OtaPhase.Checking, is OtaPhase.Installing -> null
        is OtaPhase.WaitingUser -> { { GreyButton(stringResource(R.string.sync_ota_stop_waiting)) { x.cancelWork() } } }
        is OtaPhase.Done -> { { PrimaryButton(stringResource(R.string.sync_done)) { x.clearUpdateFiles(); a.go(Screen.SYNC) } } }
        is OtaPhase.Refused -> { { PrimaryButton(stringResource(R.string.sync_ota_install_again), enabled = link != null) { x.installUpdate(false) } } }
        is OtaPhase.StagedOnly -> { { PrimaryButton(stringResource(R.string.sync_done)) { a.go(Screen.SYNC) } } }
        is OtaPhase.Failed -> when {
            link == null -> { { PrimaryButton(stringResource(R.string.sync_reconnect)) { a.go(Screen.SYNC) } } }
            ota.downgrade -> {
                {
                    PrimaryButton(stringResource(R.string.sync_ota_downgrade_anyway, pick.version ?: ""), color = C.orange) { x.installUpdate(true) }
                    LinkButton(stringResource(R.string.cancel)) { x.clearUpdateFiles() }
                }
            }
            else -> { { PrimaryButton(stringResource(R.string.try_again), enabled = pick.ready) { x.installUpdate(false) } } }
        }
        OtaPhase.Idle -> when {
            link == null || !supported -> null
            pick.ready -> {
                {
                    PrimaryButton(stringResource(R.string.sync_ota_send_install, size(pick.payloadSize))) { x.installUpdate(false) }
                    LinkButton(stringResource(R.string.sync_clear)) { x.clearUpdateFiles() }
                }
            }
            else -> {
                {
                    PrimaryButton(stringResource(if (pick.version != null || pick.tarName != null) R.string.sync_ota_choose_other else R.string.sync_ota_choose)) {
                        x.pickUpdateFiles()
                    }
                }
            }
        }
    }
    SyncPage(s, a, Screen.SYNC_INSTALL, st.busy, bottom = bottom) {
        when (ota) {
            is OtaPhase.Sending -> {
                ProgressCard(title = stringResource(R.string.sync_ota_sending, pick.version ?: ""),
                    fraction = if (ota.total > 0) ota.done.toFloat() / ota.total else 0f,
                    detail = if (ota.reconnecting > 0) stringResource(R.string.sync_reconnecting, ota.reconnecting,
                        org.hibyos.install.connected.ConnectedModel.reconnectOf(st)) else null,
                    detail2 = ota.path.ifEmpty { " " },
                    note = stringResource(R.string.sync_ota_sending_note))
                return@SyncPage
            }
            OtaPhase.Checking -> {
                Hero(Glyphs.Info, C.blue, stringResource(R.string.sync_ota_checking), stringResource(R.string.sync_ota_checking_sub))
                Section { row { Item(title = stringResource(R.string.sync_ota_checking), trailing = { Spinner() }) } }
                return@SyncPage
            }
            is OtaPhase.WaitingUser -> {
                Hero(Glyphs.Player, C.blue, stringResource(R.string.sync_ota_confirm_title), stringResource(R.string.sync_ota_confirm_text, ota.version))
                Section { row { Item(title = stringResource(R.string.sync_ota_confirm_wait), trailing = { Spinner() }) } }
                return@SyncPage
            }
            is OtaPhase.Installing -> {
                ProgressCard(title = stringResource(R.string.sync_ota_installing, ota.version), fraction = ota.permille / 1000f,
                    note = stringResource(R.string.sync_ota_installing_note))
                return@SyncPage
            }
            is OtaPhase.Done -> {
                Hero(Glyphs.Check, C.green, stringResource(R.string.sync_ota_done_title, ota.version), stringResource(when (ota.restart) {
                    0 -> R.string.sync_ota_done_none
                    1 -> R.string.sync_ota_done_shell
                    else -> R.string.sync_ota_done_reboot
                }))
                return@SyncPage
            }
            is OtaPhase.Refused -> {
                Hero(Glyphs.Alert, C.orange, stringResource(R.string.sync_ota_refused_title), stringResource(R.string.sync_ota_refused_text, ota.version))
                return@SyncPage
            }
            is OtaPhase.StagedOnly -> {
                Hero(Glyphs.Check, C.blue, stringResource(R.string.sync_ota_staged_title), stringResource(R.string.sync_ota_staged_text, ota.version))
                return@SyncPage
            }
            is OtaPhase.Failed -> ErrorSection(ota.error)
            OtaPhase.Idle -> {}
        }
        if (link == null) {
            NotConnected(a)
        } else if (!supported) {
            Section(footer = stringResource(R.string.sync_ota_unsupported_sub)) {
                row { Item(title = stringResource(R.string.sync_ota_unsupported), icon = { IconTile(Glyphs.Alert, C.orange) }) }
            }
        }
        pick.error?.let { ErrorSection(it) }
        if (pick.version != null) {
            Section(header = stringResource(R.string.sync_ota_version_h, pick.version),
                footer = if (ota == OtaPhase.Idle && pick.ready) stringResource(R.string.sync_ota_install_footer) else null) {
                row { Item(title = stringResource(R.string.sync_ota_signed), icon = { IconTile(Glyphs.Check, C.green) }) }
                val installed = link?.info?.byosVersion
                if (installed != null) row(iconInset) {
                    Item(title = stringResource(R.string.sync_ota_installed), value = "ByOS $installed", icon = { IconTile(Glyphs.Player, C.grey) })
                }
                row(iconInset) { Item(title = stringResource(R.string.sync_ota_size), value = size(pick.payloadSize), icon = { IconTile(Glyphs.Doc, C.blue) }) }
                row(iconInset) {
                    Item(title = stringResource(R.string.sync_ota_after), value = stringResource(when (pick.restart) {
                        "none" -> R.string.sync_ota_restart_none
                        "shell" -> R.string.sync_ota_restart_shell
                        else -> R.string.sync_ota_restart_reboot
                    }), icon = { IconTile(Glyphs.Refresh, C.purple) })
                }
                if (pick.needsFirmware) row(iconInset) {
                    Item(title = stringResource(R.string.sync_ota_needs_fw), subtitle = stringResource(R.string.sync_ota_needs_fw_sub),
                        icon = { IconTile(Glyphs.Alert, C.orange) })
                }
            }
        }
        Section(header = stringResource(R.string.sync_ota_files_h), footer = stringResource(R.string.sync_ota_files_footer)) {
            val busy = ota.running
            row {
                Item(title = stringResource(R.string.sync_ota_json), subtitle = if (pick.version != null) "hibyos-update.json" else null,
                    value = if (pick.version != null) null else stringResource(R.string.sync_ota_not_chosen),
                    icon = { IconTile(Glyphs.Doc, if (pick.version != null) C.green else C.grey) },
                    trailing = if (pick.version != null) ({ Glyph(Glyphs.Check, C.green, 18.dp) }) else null,
                    enabled = !busy, onClick = { x.pickUpdateFiles() })
            }
            row(iconInset) {
                val tar = pick.tarName
                Item(title = stringResource(R.string.sync_ota_payload),
                    subtitle = when {
                        tar == null -> null
                        pick.tarMismatch -> "$tar  ·  **${stringResource(R.string.sync_ota_tar_wrong)}**"
                        else -> "$tar  ·  ${size(pick.tarSize)}"
                    },
                    value = if (tar == null) stringResource(R.string.sync_ota_not_chosen) else null,
                    icon = { IconTile(Glyphs.Folder, if (tar == null) C.grey else if (pick.tarMismatch) C.red else C.green) },
                    trailing = when {
                        tar == null -> null
                        pick.tarMismatch -> ({ Glyph(Glyphs.Alert, C.red, 18.dp) })
                        pick.version != null -> ({ Glyph(Glyphs.Check, C.green, 18.dp) })
                        else -> null
                    },
                    enabled = !busy, onClick = { x.pickUpdateFiles() })
            }
        }
    }
}
