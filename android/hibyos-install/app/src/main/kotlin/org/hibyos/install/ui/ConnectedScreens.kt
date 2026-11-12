package org.hibyos.install.ui

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.runtime.Composable
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.hibyos.install.Actions
import org.hibyos.install.PatchState
import org.hibyos.install.R
import org.hibyos.install.Screen
import org.hibyos.install.UiState
import org.hibyos.install.connected.Album
import org.hibyos.install.connected.Destination
import org.hibyos.install.connected.JobStatus
import org.hibyos.install.connected.LibraryNav
import org.hibyos.install.connected.LibraryPhase
import org.hibyos.install.connected.Link
import org.hibyos.install.connected.LibTarget
import org.hibyos.install.connected.Prompt
import org.hibyos.install.connected.Rescan
import org.hibyos.install.connected.SendJob
import org.hibyos.install.connected.SyncActions
import org.hibyos.install.connected.SyncDevice
import org.hibyos.install.connected.SyncEntry
import org.hibyos.install.connected.SyncPaths
import org.hibyos.install.connected.SyncState
import org.hibyos.install.connected.Track
import org.hibyos.install.connected.UpdatePhase
import org.hibyos.install.mb

/** The state and the actions of the connected mode, for the screens below. */
class SyncUi(val state: SyncState, val actions: SyncActions)

object NoSync : SyncActions {
    override fun findDevices() {}
    override fun connectTo(device: SyncDevice) {}
    override fun connectAddress(text: String) {}
    override fun setCode(text: String) {}
    override fun confirmCode() {}
    override fun cancelPairing() {}
    override fun forget(deviceId: String) {}
    override fun disconnect() {}
    override fun refreshSpace() {}
    override fun setDestination(d: Destination) {}
    override fun setReplace(on: Boolean) {}
    override fun pickFiles() {}
    override fun pickFolder() {}
    override fun startSend() {}
    override fun clearSend() {}
    override fun openFolder(path: String) {}
    override fun refreshFolder() {}
    override fun sendHere() {}
    override fun prompt(p: Prompt?) {}
    override fun applyPrompt() {}
    override fun startMove(entry: SyncEntry) {}
    override fun moveHere() {}
    override fun cancelMove() {}
    override fun loadLibrary() {}
    override fun libraryNav(nav: LibraryNav) {}
    override fun rescanLibrary() {}
    override fun libraryDelete(target: org.hibyos.install.connected.LibTarget?) {}
    override fun confirmLibraryDelete() {}
    override fun libraryMove(target: org.hibyos.install.connected.LibTarget) {}
    override fun revealInFiles(target: org.hibyos.install.connected.LibTarget) {}
    override fun sendByosUpdate() {}
    override fun pickUpdateFiles() {}
    override fun clearUpdateFiles() {}
    override fun installUpdate(allowDowngrade: Boolean) {}
    override fun cancelWork() {}
}

val LocalSync = compositionLocalOf { SyncUi(SyncState(), NoSync) }

fun syncTitleRes(screen: Screen): Int = when (screen) {
    Screen.SYNC_SEND -> R.string.sync_send_title
    Screen.SYNC_FILES -> R.string.sync_files_title
    Screen.SYNC_LIBRARY -> R.string.sync_library_title
    Screen.SYNC_UPDATE -> R.string.sync_update_title
    Screen.SYNC_INSTALL -> R.string.sync_install_title
    else -> R.string.sync_title
}

/** "59.5 GB", "12.3 MB", "640 KB", "17 B". */
fun size(bytes: Long): String = when {
    bytes >= 1L shl 30 -> {
        val tenths = (bytes * 10 + (1L shl 29)) / (1L shl 30)
        "${tenths / 10}.${tenths % 10} GB"
    }
    bytes >= 1L shl 20 -> mb(bytes)
    bytes >= 1024 -> "${(bytes + 512) / 1024} KB"
    else -> "$bytes B"
}

@Composable
internal fun SyncPage(s: UiState, a: Actions, screen: Screen, busy: Boolean, trailing: (@Composable () -> Unit)? = null,
                     bottom: (@Composable androidx.compose.foundation.layout.ColumnScope.() -> Unit)? = null,
                     content: @Composable androidx.compose.foundation.layout.ColumnScope.() -> Unit) {
    val prev = remember { s.stack.getOrNull(s.stack.indexOf(screen) - 1) }
    val back = when (prev) {
        null, Screen.HOME -> stringResource(R.string.back)
        Screen.SYNC -> stringResource(R.string.sync_back)
        else -> stringResource(R.string.back)
    }
    Page(title = AnnotatedString(stringResource(syncTitleRes(screen))), back = back, onBack = if (busy) null else a::back,
        trailing = trailing, bottom = bottom, content = content)
}

/** A sub-screen without a session: the way back to the connection screen. */
@Composable
internal fun NotConnected(a: Actions) {
    Section(footer = stringResource(R.string.sync_not_connected_footer)) {
        row {
            Item(title = stringResource(R.string.sync_not_connected), icon = { IconTile(Glyphs.Link, C.orange) }, chevron = true) { a.go(Screen.SYNC) }
        }
    }
}

// ---- the connection -------------------------------------------------------------------------

@Composable
fun SyncHome(s: UiState, a: Actions) {
    val ui = LocalSync.current
    val st = ui.state
    val x = ui.actions
    val link = st.link
    var address by remember { mutableStateOf("") }
    val bottom: (@Composable androidx.compose.foundation.layout.ColumnScope.() -> Unit)? = when {
        !st.available -> null
        link is Link.Pairing -> {
            {
                if (link.waiting) GreyButton(stringResource(R.string.cancel)) { x.cancelPairing() }
                else {
                    PrimaryButton(stringResource(R.string.sync_pair), enabled = link.code.length == 6) { x.confirmCode() }
                    LinkButton(stringResource(R.string.cancel)) { x.cancelPairing() }
                }
            }
        }
        link is Link.Connected -> { { LinkButton(stringResource(R.string.sync_disconnect)) { x.disconnect() } } }
        link is Link.Searching -> { { GreyButton(stringResource(R.string.cancel)) { x.cancelWork() } } }
        else -> null
    }
    SyncPage(s, a, Screen.SYNC, st.busy, bottom = bottom) {
        if (!st.available) {
            Hero(Glyphs.Alert, C.orange, stringResource(R.string.sync_unavailable_title), stringResource(R.string.sync_unavailable_text))
            return@SyncPage
        }
        when (link) {
            is Link.Pairing -> PairingView(link, x)
            is Link.Connected -> {
                Hero(Glyphs.Player, C.green, link.device.name,
                    listOfNotNull(link.info.byosVersion?.let { "ByOS $it" },
                        stringResource(if (link.info.local) R.string.sync_link_usb else R.string.sync_link_wifi)).joinToString("  ·  "))
                val sp = link.space
                Section(header = stringResource(R.string.sync_card_h)) {
                    row {
                        if (sp == null || sp.sdState != 0) {
                            Item(title = stringResource(R.string.sync_card), value = stringResource(R.string.sync_card_unavailable), valueColor = C.orange,
                                icon = { IconTile(Glyphs.Card, C.orange) })
                        } else {
                            Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 11.dp)) {
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    Txt(stringResource(R.string.sync_card), T.body, Modifier.weight(1f))
                                    Txt(stringResource(R.string.sync_card_free, size(sp.sdFree), size(sp.sdTotal)), T.value)
                                }
                                ProgressBar(if (sp.sdTotal > 0) 1f - sp.sdFree.toFloat() / sp.sdTotal else 0f, Modifier.padding(top = 10.dp, bottom = 3.dp))
                            }
                        }
                    }
                }
                Section {
                    row {
                        Item(title = stringResource(R.string.sync_send_title), subtitle = stringResource(R.string.sync_send_sub),
                            icon = { IconTile(Glyphs.Upload, C.blue) }, chevron = true) { a.go(Screen.SYNC_SEND) }
                    }
                    row(iconInset) {
                        Item(title = stringResource(R.string.sync_files_title), subtitle = stringResource(R.string.sync_files_sub),
                            icon = { IconTile(Glyphs.Folder, C.cyan) }, chevron = true) { x.openFolder(""); a.go(Screen.SYNC_FILES) }
                    }
                    row(iconInset) {
                        Item(title = stringResource(R.string.sync_library_title), subtitle = stringResource(R.string.sync_library_sub),
                            icon = { IconTile(Glyphs.Music, C.pink) }, chevron = true) { a.go(Screen.SYNC_LIBRARY) }
                    }
                }
                Section(header = stringResource(R.string.sync_byos_h)) {
                    row {
                        Item(title = stringResource(R.string.sync_install_title), subtitle = stringResource(R.string.sync_install_sub),
                            icon = { IconTile(Glyphs.Download, C.green) }, chevron = true) { a.go(Screen.SYNC_INSTALL) }
                    }
                    row(iconInset) {
                        Item(title = stringResource(R.string.sync_update_title), subtitle = stringResource(R.string.sync_update_sub),
                            icon = { IconTile(Glyphs.Layers, C.grey) }, chevron = true) { a.go(Screen.SYNC_UPDATE) }
                    }
                }
            }
            else -> {
                st.error?.let { ErrorSection(it) }
                Section(header = stringResource(R.string.sync_player_h), footer = stringResource(R.string.menu_names_note)) {
                    row { StepText(1, stringResource(R.string.sync_p1)) }
                    row(stepInset) { StepText(2, stringResource(R.string.sync_p2)) }
                    row(stepInset) { StepText(3, stringResource(R.string.sync_p3)) }
                }
                Section(header = stringResource(R.string.sync_found_h), footer = stringResource(R.string.sync_found_footer)) {
                    row {
                        when (link) {
                            Link.Searching -> Item(title = stringResource(R.string.sync_searching), trailing = { Spinner() })
                            is Link.Connecting -> Item(title = stringResource(R.string.sync_connecting, link.device.name), trailing = { Spinner() })
                            else -> Item(title = stringResource(R.string.sync_find), titleColor = C.blue,
                                trailing = { Glyph(Glyphs.Search, C.blue, 20.dp) }) { x.findDevices() }
                        }
                    }
                    for (d in st.found) row(iconInset) {
                        val sub = d.host + "  ·  " + stringResource(when {
                            d.paired -> R.string.sync_dev_paired
                            d.pairingOpen -> R.string.sync_dev_pairing_open
                            else -> R.string.sync_dev_pairing_closed
                        })
                        Item(title = d.name, subtitle = sub, icon = { IconTile(Glyphs.Player, if (d.paired) C.green else C.purple) },
                            chevron = true, enabled = !st.busy) { x.connectTo(d) }
                    }
                }
                Section(header = stringResource(R.string.sync_address_h), footer = stringResource(R.string.sync_address_footer)) {
                    row {
                        Field(value = address, hint = stringResource(R.string.sync_address_hint), enabled = !st.busy,
                            onGo = { x.connectAddress(address) }, onChange = { address = it.take(64) })
                    }
                    row {
                        Item(title = stringResource(R.string.sync_connect), titleColor = C.blue, enabled = address.isNotBlank() && !st.busy) {
                            x.connectAddress(address)
                        }
                    }
                }
                val pairedOnly = st.paired.filter { p -> st.found.none { it.deviceId == p.deviceId } }
                if (pairedOnly.isNotEmpty()) {
                    Section(header = stringResource(R.string.sync_paired_h), footer = stringResource(R.string.sync_paired_footer)) {
                        pairedOnly.forEachIndexed { i, p ->
                            row(if (i == 0) 16.dp else iconInset) {
                                Item(title = p.name, subtitle = p.lastHost, icon = { IconTile(Glyphs.Player, C.grey) },
                                    trailing = { NavAction(stringResource(R.string.sync_forget)) { x.forget(p.deviceId) } },
                                    enabled = !st.busy, onClick = p.lastHost?.let { h ->
                                        { x.connectTo(SyncDevice(h, 7340, p.deviceId, p.name, false, true, true)) }
                                    })
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun PairingView(link: Link.Pairing, x: SyncActions) {
    Hero(Glyphs.Link, C.blue, stringResource(R.string.sync_pair_title, link.device.name), stringResource(R.string.sync_pair_text))
    Box(Modifier.fillMaxWidth().padding(bottom = 18.dp), contentAlignment = Alignment.Center) {
        CodeBoxes(link.code, enabled = !link.waiting, onChange = { x.setCode(it) }, onGo = { x.confirmCode() })
    }
    if (link.wrong) {
        Txt(stringResource(R.string.sync_pair_wrong), T.callout.copy(color = C.red, textAlign = TextAlign.Center),
            Modifier.fillMaxWidth().padding(start = 28.dp, end = 28.dp, bottom = 18.dp))
    }
    if (link.waiting) {
        Section {
            row { Item(title = stringResource(R.string.sync_pair_waiting), subtitle = stringResource(R.string.sync_pair_waiting_sub), trailing = { Spinner() }) }
        }
    }
    Txt(rich(stringResource(R.string.sync_pair_footer)), T.footer, Modifier.padding(start = 32.dp, end = 32.dp, bottom = 26.dp))
}

/** Six boxes for the code shown on the player, typed with the number keyboard. */
@Composable
private fun CodeBoxes(code: String, enabled: Boolean, onChange: (String) -> Unit, onGo: () -> Unit) {
    BasicTextField(
        value = code,
        onValueChange = { onChange(it) },
        enabled = enabled,
        singleLine = true,
        textStyle = T.body.copy(color = Color.Transparent),
        cursorBrush = SolidColor(Color.Transparent),
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.NumberPassword, imeAction = ImeAction.Done, autoCorrectEnabled = false),
        keyboardActions = KeyboardActions(onDone = { onGo() }),
        decorationBox = { inner ->
            Box {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                    for (i in 0 until 6) {
                        if (i == 3) Spacer(Modifier.width(8.dp))
                        val ch = code.getOrNull(i)?.toString() ?: ""
                        val active = enabled && i == code.length
                        Box(Modifier.size(width = 44.dp, height = 56.dp).clip(RoundedCornerShape(10.dp)).background(C.card)
                            .border(if (active) 2.dp else 0.5.dp, if (active) C.blue else C.sep, RoundedCornerShape(10.dp)),
                            contentAlignment = Alignment.Center) {
                            Txt(ch, T.title2.copy(fontSize = 28.sp, fontFeatureSettings = "tnum"))
                        }
                    }
                }
                Box(Modifier.size(1.dp).alpha(0f)) { inner() }
            }
        },
    )
}

// ---- send -----------------------------------------------------------------------------------

private fun destRes(d: Destination): Int = when (d) {
    Destination.MUSIC -> R.string.sync_dest_music
    Destination.BOOKS -> R.string.sync_dest_books
    Destination.PICTURES -> R.string.sync_dest_pictures
    Destination.APPS -> R.string.sync_dest_apps
    Destination.UPDATE -> R.string.sync_dest_update
}

private fun destGlyph(d: Destination): GlyphDraw = when (d) {
    Destination.MUSIC -> Glyphs.Music
    Destination.BOOKS -> Glyphs.Book
    Destination.PICTURES -> Glyphs.Picture
    Destination.APPS -> Glyphs.Grid
    Destination.UPDATE -> Glyphs.Card
}

private fun destColor(d: Destination): Color = when (d) {
    Destination.MUSIC -> C.pink
    Destination.BOOKS -> C.orange
    Destination.PICTURES -> C.purple
    Destination.APPS -> C.blue
    Destination.UPDATE -> C.grey
}

private fun jobRes(st: JobStatus): Int = when (st) {
    JobStatus.WAITING -> R.string.sync_job_waiting
    JobStatus.HASHING -> R.string.sync_job_hashing
    JobStatus.SENDING -> R.string.sync_job_sending
    JobStatus.SENT -> R.string.sync_job_sent
    JobStatus.SKIPPED -> R.string.sync_job_skipped
    JobStatus.EXISTS -> R.string.sync_job_exists
    JobStatus.TOO_BIG -> R.string.sync_job_too_big
    JobStatus.BAD_NAME -> R.string.sync_job_bad_name
    JobStatus.FAILED -> R.string.sync_job_failed
    JobStatus.CANCELLED -> R.string.sync_job_cancelled
}

@Composable
private fun JobMark(j: SendJob) {
    Box(Modifier.size(22.dp), contentAlignment = Alignment.Center) {
        when (j.status) {
            JobStatus.SENT, JobStatus.SKIPPED -> Glyph(Glyphs.Check, C.green, 18.dp)
            JobStatus.HASHING, JobStatus.SENDING -> Spinner(18.dp, C.blue)
            JobStatus.WAITING -> {}
            JobStatus.CANCELLED -> Glyph(Glyphs.Cross, C.secondary, 16.dp)
            else -> Glyph(Glyphs.Alert, C.red, 18.dp)
        }
    }
}

/** An on/off switch drawn like the ones of ByOS. */
@Composable
fun Toggle(on: Boolean, enabled: Boolean = true, onChange: (Boolean) -> Unit) {
    val f by animateFloatAsState(if (on) 1f else 0f, tween(180), label = "toggle")
    Canvas(Modifier.size(width = 51.dp, height = 31.dp).alpha(if (enabled) 1f else 0.4f)
        .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null, enabled = enabled) { onChange(!on) }) {
        val h = size.height
        val track = androidx.compose.ui.graphics.lerp(C.fill, C.green, f)
        drawRoundRect(track, cornerRadius = androidx.compose.ui.geometry.CornerRadius(h / 2, h / 2))
        val r = h / 2 - 2.dp.toPx()
        drawCircle(Color.White, r, Offset(h / 2 + (size.width - h) * f, h / 2))
    }
}

@Composable
fun SyncSend(s: UiState, a: Actions) {
    val ui = LocalSync.current
    val st = ui.state
    val x = ui.actions
    val t = st.transfer
    val connected = st.connected != null
    val todo = t.jobs.filter { it.retryable }
    val bottom: (@Composable androidx.compose.foundation.layout.ColumnScope.() -> Unit)? = when {
        t.running -> { { GreyButton(stringResource(R.string.cancel)) { x.cancelWork() } } }
        todo.isNotEmpty() && !connected -> { { PrimaryButton(stringResource(R.string.sync_reconnect)) { a.go(Screen.SYNC) } } }
        todo.isNotEmpty() -> {
            {
                val started = t.jobs.any { it.status != JobStatus.WAITING && it.status != JobStatus.BAD_NAME && it.status != JobStatus.TOO_BIG }
                PrimaryButton(if (started) stringResource(R.string.sync_resume, todo.size)
                else stringResource(R.string.sync_send_n, pluralStringResource(R.plurals.sync_files, todo.size, todo.size), size(todo.sumOf { it.size }))) { x.startSend() }
                if (!started) LinkButton(stringResource(R.string.sync_clear)) { x.clearSend() }
            }
        }
        t.finished -> { { PrimaryButton(stringResource(R.string.sync_done)) { x.clearSend(); a.back() } } }
        else -> null
    }
    SyncPage(s, a, Screen.SYNC_SEND, st.busy, bottom = bottom) {
        if (!connected && t.jobs.isEmpty()) {
            NotConnected(a)
            return@SyncPage
        }
        if (t.running) {
            ProgressCard(title = stringResource(R.string.sync_sending),
                fraction = if (t.total > 0) t.done.toFloat() / t.total else 0f,
                detail = stringResource(if (t.hashing) R.string.sync_job_hashing else R.string.sync_job_sending),
                detail2 = t.current.ifEmpty { " " }, note = stringResource(R.string.sync_sending_note))
        } else if (t.finished) {
            val ok = t.problems == 0
            Hero(if (ok) Glyphs.Check else Glyphs.Alert, if (ok) C.green else C.orange,
                stringResource(if (ok) R.string.sync_sent_title else R.string.sync_sent_some_title),
                stringResource(R.string.sync_sent_text, t.sent, t.skipped, t.problems))
            if (connected && (t.newMusic > 0 || st.rescan != Rescan.Idle)) RescanSection(st, x, t.newMusic)
        }
        t.error?.let { ErrorSection(it) }
        if (!t.running && !t.finished) {
            Section(header = stringResource(R.string.sync_dest_h), footer = stringResource(R.string.sync_dest_footer)) {
                val custom = st.destPath
                Destination.entries.forEachIndexed { i, d ->
                    row(if (i == 0) 16.dp else iconInset) {
                        val label = stringResource(destRes(d))
                        Item(title = label, subtitle = if (d.path.isEmpty()) stringResource(R.string.sync_dest_update_sub) else if (d.path == label) null else d.path,
                            icon = { IconTile(destGlyph(d), destColor(d)) },
                            trailing = { if (custom == null && st.dest == d) Glyph(Glyphs.Check, C.blue, 18.dp) }) { x.setDestination(d) }
                    }
                }
                if (custom != null) row(iconInset) {
                    Item(title = SyncPaths.name(custom).ifEmpty { stringResource(R.string.sync_card) }, subtitle = custom,
                        icon = { IconTile(Glyphs.Folder, C.cyan) }, trailing = { Glyph(Glyphs.Check, C.blue, 18.dp) })
                }
            }
            Section(header = stringResource(R.string.sync_choose_h), footer = stringResource(R.string.sync_choose_footer)) {
                row {
                    Item(title = stringResource(R.string.sync_choose_files), icon = { IconTile(Glyphs.Doc, C.blue) }, chevron = true) { x.pickFiles() }
                }
                row(iconInset) {
                    Item(title = stringResource(R.string.sync_choose_folder), icon = { IconTile(Glyphs.Folder, C.cyan) }, chevron = true) { x.pickFolder() }
                }
                row {
                    Item(title = stringResource(R.string.sync_replace), subtitle = stringResource(R.string.sync_replace_sub),
                        trailing = { Toggle(st.replace) { x.setReplace(it) } })
                }
            }
        }
        if (t.jobs.isNotEmpty()) {
            val shown = t.jobs.take(300)
            Section(header = pluralStringResource(R.plurals.sync_files, t.jobs.size, t.jobs.size),
                footer = if (t.jobs.size > shown.size) stringResource(R.string.sync_jobs_more, t.jobs.size - shown.size) else null) {
                for (j in shown) row {
                    val status = stringResource(jobRes(j.status))
                    val sub = if (j.status == JobStatus.SENDING && j.size > 0) "$status  ·  ${size(j.done)} / ${size(j.size)}"
                    else "$status  ·  ${size(j.size)}"
                    Item(title = SyncPaths.name(j.remote.ifEmpty { j.local }), subtitle = sub, trailing = { JobMark(j) })
                }
            }
        }
    }
}

// ---- files ----------------------------------------------------------------------------------

@Composable
fun SyncFiles(s: UiState, a: Actions) {
    val ui = LocalSync.current
    val st = ui.state
    val x = ui.actions
    val b = st.browser
    val connected = st.connected != null
    var sheet by remember { mutableStateOf<SyncEntry?>(null) }
    val bottom: (@Composable androidx.compose.foundation.layout.ColumnScope.() -> Unit)? = when {
        !connected -> null
        b.moving != null -> {
            {
                PrimaryButton(stringResource(R.string.sync_move_here), enabled = !b.working) { x.moveHere() }
                LinkButton(stringResource(R.string.cancel)) { x.cancelMove() }
            }
        }
        else -> { { LinkButton(stringResource(R.string.sync_send_here)) { x.sendHere(); a.go(Screen.SYNC_SEND) } } }
    }
    Box(Modifier.fillMaxSize()) {
        SyncPage(s, a, Screen.SYNC_FILES, st.busy, bottom = bottom,
            trailing = if (connected && b.moving == null) ({ NavAction(stringResource(R.string.sync_new_folder)) { x.prompt(Prompt.NewFolder()) } }) else null) {
            if (!connected) {
                NotConnected(a)
                return@SyncPage
            }
            Txt(listOf(stringResource(R.string.sync_card)).plus(if (b.path.isEmpty()) emptyList() else b.path.split('/')).joinToString("  ›  "),
                T.sub, Modifier.padding(start = 20.dp, end = 20.dp, bottom = 14.dp), maxLines = 2, overflow = TextOverflow.MiddleEllipsis)
            if (b.moving != null) {
                Section {
                    row {
                        Item(title = stringResource(R.string.sync_moving, SyncPaths.name(b.moving)), subtitle = stringResource(R.string.sync_moving_sub),
                            icon = { IconTile(Glyphs.Folder, C.orange) })
                    }
                }
            }
            b.error?.let { ErrorSection(it) }
            val list = b.entries
            when {
                list == null -> Section { row { Item(title = stringResource(R.string.sync_loading), trailing = { Spinner() }) } }
                list.isEmpty() -> Section { row { TextRow(stringResource(R.string.sync_empty_folder), T.sub) } }
                else -> Section {
                    list.forEachIndexed { i, e ->
                        row(if (i == 0) 16.dp else iconInset) {
                            val movingThis = b.moving != null && b.moving == SyncPaths.join(b.path, e.name)
                            Item(title = e.name, subtitle = if (e.isFolder) null else size(e.size),
                                icon = { IconTile(if (e.isFolder) Glyphs.Folder else Glyphs.Doc, if (e.isFolder) C.cyan else C.grey) },
                                chevron = e.isFolder,
                                enabled = !movingThis && (e.isFolder || b.moving == null),
                                trailing = if (b.moving == null) ({ MoreButton { sheet = e } }) else null) {
                                if (e.isFolder) x.openFolder(SyncPaths.join(b.path, e.name)) else sheet = e
                            }
                        }
                    }
                }
            }
        }
        val e = sheet
        if (e != null) {
            ActionSheet(e.name, onDismiss = { sheet = null }, actions = listOf(
                stringResource(R.string.sync_rename) to { sheet = null; x.prompt(Prompt.Rename(e, e.name)) },
                stringResource(R.string.sync_move) to { sheet = null; x.startMove(e) },
                stringResource(R.string.sync_delete) to { sheet = null; x.prompt(Prompt.Delete(e)) },
            ), destructive = 2)
        }
        when (val p = b.prompt) {
            is Prompt.NewFolder -> AlertBox(stringResource(R.string.sync_new_folder), null, p.name, stringResource(R.string.sync_create),
                working = b.working, onChange = { x.prompt(Prompt.NewFolder(it)) }, onOk = { x.applyPrompt() }, onCancel = { x.prompt(null) })
            is Prompt.Rename -> AlertBox(stringResource(R.string.sync_rename), null, p.name, stringResource(R.string.sync_rename),
                working = b.working, onChange = { x.prompt(p.copy(name = it)) }, onOk = { x.applyPrompt() }, onCancel = { x.prompt(null) })
            is Prompt.Delete -> AlertBox(stringResource(R.string.sync_delete_q, p.entry.name),
                stringResource(if (p.entry.isFolder) R.string.sync_delete_folder_text else R.string.sync_delete_text), null,
                stringResource(R.string.sync_delete), destructive = true, working = b.working,
                onChange = {}, onOk = { x.applyPrompt() }, onCancel = { x.prompt(null) })
            null -> {}
        }
    }
}

@Composable
internal fun MoreButton(onClick: () -> Unit) {
    Box(Modifier.size(32.dp).clip(CircleShape).clickable(onClick = onClick), contentAlignment = Alignment.Center) {
        Canvas(Modifier.size(18.dp)) {
            val r = 1.8.dp.toPx()
            for (k in -1..1) drawCircle(C.secondary, r, Offset(size.width / 2 + k * 6.dp.toPx(), size.height / 2))
        }
    }
}

@Composable
internal fun Scrim(onDismiss: () -> Unit, content: @Composable androidx.compose.foundation.layout.BoxScope.() -> Unit) {
    Box(Modifier.fillMaxSize().background(Color.Black.copy(alpha = 0.55f))
        .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) { onDismiss() }) {
        content()
    }
}

/** The list of actions on a file, from the bottom of the screen. */
@Composable
internal fun ActionSheet(title: String, onDismiss: () -> Unit, actions: List<Pair<String, () -> Unit>>, destructive: Int = -1) {
    Scrim(onDismiss) {
        Column(Modifier.align(Alignment.BottomCenter).widthIn(max = 560.dp).fillMaxWidth().padding(10.dp)) {
            Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(C.card)) {
                Txt(title, T.footer.copy(textAlign = TextAlign.Center), Modifier.fillMaxWidth().padding(14.dp), maxLines = 2,
                    overflow = TextOverflow.MiddleEllipsis)
                actions.forEachIndexed { i, (label, f) ->
                    Box(Modifier.fillMaxWidth().height(0.5.dp).background(C.sep))
                    Box(Modifier.fillMaxWidth().heightIn(min = 56.dp).clickable { f() }, contentAlignment = Alignment.Center) {
                        Txt(label, T.body.copy(color = if (i == destructive) C.red else C.blue, fontSize = 20.sp, textAlign = TextAlign.Center))
                    }
                }
            }
            Spacer(Modifier.height(8.dp))
            Box(Modifier.fillMaxWidth().heightIn(min = 56.dp).clip(RoundedCornerShape(14.dp)).background(C.card).clickable { onDismiss() },
                contentAlignment = Alignment.Center) {
                Txt(stringResource(R.string.cancel), T.button.copy(color = C.blue, fontSize = 20.sp))
            }
        }
    }
}

/** A centred alert, with a text field when [value] is not null. */
@Composable
internal fun AlertBox(title: String, text: String?, value: String?, ok: String, destructive: Boolean = false, working: Boolean,
                     onChange: (String) -> Unit, onOk: () -> Unit, onCancel: () -> Unit) {
    Scrim(if (working) ({}) else onCancel) {
        Column(Modifier.align(Alignment.Center).width(290.dp).clip(RoundedCornerShape(14.dp)).background(C.card)
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {}) {
            Column(Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, top = 18.dp, bottom = 16.dp),
                horizontalAlignment = Alignment.CenterHorizontally) {
                Txt(title, T.headline.copy(textAlign = TextAlign.Center), maxLines = 3, overflow = TextOverflow.MiddleEllipsis)
                if (text != null) Txt(text, T.sub.copy(textAlign = TextAlign.Center, color = C.label), Modifier.padding(top = 4.dp))
                if (value != null) {
                    Box(Modifier.padding(top = 14.dp).fillMaxWidth().clip(RoundedCornerShape(8.dp)).background(C.bg)) {
                        Field(value = value, hint = "", enabled = !working, onGo = onOk, onChange = onChange)
                    }
                }
            }
            Box(Modifier.fillMaxWidth().height(0.5.dp).background(C.sep))
            Row(Modifier.fillMaxWidth().heightIn(min = 46.dp)) {
                Box(Modifier.weight(1f).heightIn(min = 46.dp).clickable(enabled = !working) { onCancel() }, contentAlignment = Alignment.Center) {
                    Txt(stringResource(R.string.cancel), T.nav)
                }
                Box(Modifier.width(0.5.dp).height(46.dp).background(C.sep))
                Box(Modifier.weight(1f).heightIn(min = 46.dp).clickable(enabled = !working && (value == null || value.isNotBlank())) { onOk() },
                    contentAlignment = Alignment.Center) {
                    if (working) Spinner(18.dp) else Txt(ok, T.nav.copy(color = if (destructive) C.red else C.blue, fontWeight = androidx.compose.ui.text.font.FontWeight.SemiBold))
                }
            }
        }
    }
}

// ---- library --------------------------------------------------------------------------------

@Composable
fun SyncLibrary(s: UiState, a: Actions) {
    val ui = LocalSync.current
    val st = ui.state
    val x = ui.actions
    val lib = st.library
    if (lib is LibraryPhase.Ready && st.connected != null) {
        LibraryLists(s, a, lib, st.libraryNav, x)
        return
    }
    val bottom: (@Composable androidx.compose.foundation.layout.ColumnScope.() -> Unit)? = when {
        st.connected == null -> null
        lib is LibraryPhase.Loading -> { { GreyButton(stringResource(R.string.cancel)) { x.cancelWork() } } }
        else -> { { PrimaryButton(stringResource(if (lib is LibraryPhase.Failed) R.string.try_again else R.string.sync_library_load)) { x.loadLibrary() } } }
    }
    SyncPage(s, a, Screen.SYNC_LIBRARY, st.busy, bottom = bottom) {
        if (st.connected == null) {
            NotConnected(a)
            return@SyncPage
        }
        when (lib) {
            is LibraryPhase.Loading -> ProgressCard(title = stringResource(R.string.sync_library_loading),
                fraction = if (lib.total > 0) lib.done.toFloat() / lib.total else 0f,
                detail = if (lib.total > 0) stringResource(R.string.fw_progress, mb(lib.done), mb(lib.total)) else null)
            is LibraryPhase.Failed -> ErrorSection(lib.error)
            else -> {}
        }
        if (lib !is LibraryPhase.Loading) {
            Section(footer = stringResource(R.string.sync_library_footer)) {
                row { TextRow(stringResource(R.string.sync_library_text)) }
            }
            RescanSection(st, x, st.transfer.newMusic)
        }
    }
}

@Composable
private fun LibraryLists(s: UiState, a: Actions, lib: LibraryPhase.Ready, nav: LibraryNav, x: SyncActions) {
    val L = lib.library
    val list = rememberLazyListState()
    val collapsed by remember { derivedStateOf { list.firstVisibleItemIndex > 0 || list.firstVisibleItemScrollOffset > 80 } }
    val artist = nav.artist?.let { n -> L.artists.firstOrNull { it.name == n } }
    val album = nav.album?.let { (ar, al) -> L.albums.firstOrNull { it.artist == ar && it.title == al } }
    val unknown = stringResource(R.string.sync_unknown)
    val title = album?.title?.ifEmpty { unknown } ?: artist?.name?.ifEmpty { unknown } ?: stringResource(R.string.sync_library_title)
    val st = LocalSync.current.state
    val top = nav.artist == null && nav.album == null
    var sheet by remember { mutableStateOf<LibTarget?>(null) }
    val trailing: (@Composable () -> Unit)? = when {
        !top -> null
        st.rescan == Rescan.Running -> ({ Box(Modifier.padding(horizontal = 16.dp)) { Spinner(20.dp) } })
        else -> ({ NavAction(stringResource(R.string.sync_rescan_short)) { x.rescanLibrary() } })
    }
    Box(Modifier.fillMaxSize()) {
        PageFrame(AnnotatedString(title), stringResource(if (!top) R.string.back else R.string.sync_back), if (st.busy) null else a::back, collapsed, trailing) {
            LazyColumn(Modifier.fillMaxSize(), state = list, contentPadding = PaddingValues(bottom = 28.dp)) {
                item { Txt(title, T.largeTitle, Modifier.padding(start = 20.dp, end = 20.dp, top = 2.dp, bottom = 12.dp), maxLines = 2, overflow = TextOverflow.Ellipsis) }
                st.libError?.let { e -> item { ErrorSection(e) } }
                if (top && (st.rescan != Rescan.Idle || st.transfer.newMusic > 0 || st.libraryChanged)) item { RescanSection(st, x, st.transfer.newMusic) }
                when {
                    album != null -> {
                        item {
                            Txt(listOfNotNull(album.artist.ifEmpty { unknown }, album.year.takeIf { it > 0 }?.toString(),
                                songs(album.tracks.size)).joinToString("  ·  "),
                                T.sub, Modifier.padding(start = 20.dp, end = 20.dp, bottom = 14.dp))
                        }
                        cards(album.tracks.size) { i ->
                            val t = album.tracks[i]
                            LibRow(t.title, t.path.substringAfterLast('\\'), lead = "${i + 1}", onMore = { sheet = LibTarget.Song(t) })
                        }
                    }
                    artist != null -> {
                        item { Txt(albumsSongs(artist.albums.size, artist.trackCount), T.sub,
                            Modifier.padding(start = 20.dp, end = 20.dp, bottom = 14.dp)) }
                        cards(artist.albums.size) { i ->
                            AlbumRow(artist.albums[i], unknown, onMore = { sheet = LibTarget.AlbumOf(it) }) { x.libraryNav(nav.copy(album = it.artist to it.title)) }
                        }
                    }
                    else -> {
                        if (L.tracks.isNotEmpty()) item {
                            Segments(listOf(stringResource(R.string.sync_artists), stringResource(R.string.sync_albums), stringResource(R.string.sync_songs)),
                                nav.tab) { x.libraryNav(LibraryNav(tab = it)) }
                        }
                        if (L.tracks.isEmpty()) {
                            item { LibraryEmpty() }
                        } else when (nav.tab) {
                            0 -> cards(L.artists.size) { i ->
                                val ar = L.artists[i]
                                LibRow(ar.name.ifEmpty { unknown }, albumsSongs(ar.albums.size, ar.trackCount), chevron = true,
                                    onMore = { sheet = LibTarget.ArtistOf(ar) }) {
                                    x.libraryNav(nav.copy(artist = ar.name))
                                }
                            }
                            1 -> cards(L.albums.size) { i ->
                                AlbumRow(L.albums[i], unknown, showArtist = true, onMore = { sheet = LibTarget.AlbumOf(it) }) {
                                    x.libraryNav(nav.copy(album = it.artist to it.title))
                                }
                            }
                            else -> cards(L.tracks.size) { i ->
                                val t: Track = L.tracks[i]
                                LibRow(t.title, listOf(t.albumArtist.ifEmpty { t.artist }.ifEmpty { unknown }, t.album.ifEmpty { unknown }).joinToString("  ·  "),
                                    onMore = { sheet = LibTarget.Song(t) })
                            }
                        }
                    }
                }
            }
        }
        val t = sheet
        if (t != null) {
            val acts = ArrayList<Pair<String, () -> Unit>>()
            if (t !is LibTarget.ArtistOf) acts += stringResource(R.string.sync_lib_show) to { sheet = null; x.revealInFiles(t); a.go(Screen.SYNC_FILES) }
            if (t.cardPath != null) acts += stringResource(R.string.sync_move) to { sheet = null; x.libraryMove(t); a.go(Screen.SYNC_FILES) }
            acts += stringResource(R.string.sync_delete) to { sheet = null; x.libraryDelete(t) }
            ActionSheet(libTargetName(t, unknown), onDismiss = { sheet = null }, actions = acts, destructive = acts.size - 1)
        }
        val d = st.libDelete
        if (d != null) {
            val n = d.tracks.size
            AlertBox(stringResource(R.string.sync_delete_q, libTargetName(d, unknown)),
                when (d) {
                    is LibTarget.Song -> stringResource(R.string.sync_delete_text)
                    is LibTarget.AlbumOf -> stringResource(R.string.sync_lib_delete_album_text, songs(n))
                    is LibTarget.ArtistOf -> stringResource(R.string.sync_lib_delete_artist_text, songs(n))
                }, null, stringResource(R.string.sync_delete), destructive = true, working = st.libWorking,
                onChange = {}, onOk = { x.confirmLibraryDelete() }, onCancel = { x.libraryDelete(null) })
        }
    }
}

private fun libTargetName(t: LibTarget, unknown: String): String = when (t) {
    is LibTarget.Song -> t.track.title.ifEmpty { unknown }
    is LibTarget.AlbumOf -> t.album.title.ifEmpty { unknown }
    is LibTarget.ArtistOf -> t.artist.name.ifEmpty { unknown }
}

/** No song in the player's library (a new card, or the player has not scanned it yet). */
@Composable
private fun LibraryEmpty() {
    Hero(Glyphs.Music, C.pink, stringResource(R.string.sync_library_empty_title), stringResource(R.string.sync_library_empty_text))
}

/** Rows of a grouped card inside a lazy list. */
internal fun LazyListScope.cards(n: Int, row: @Composable (Int) -> Unit) {
    if (n == 0) {
        item { Box(Modifier.padding(horizontal = 16.dp).fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(C.card)) { TextRow("—", T.sub) } }
        return
    }
    items(n) { i ->
        val top = if (i == 0) 12.dp else 0.dp
        val bottom = if (i == n - 1) 12.dp else 0.dp
        Box(Modifier.padding(horizontal = 16.dp).fillMaxWidth()
            .clip(RoundedCornerShape(topStart = top, topEnd = top, bottomStart = bottom, bottomEnd = bottom)).background(C.card)) {
            Column {
                if (i > 0) Box(Modifier.padding(start = 16.dp).fillMaxWidth().height(0.5.dp).background(C.sep))
                row(i)
            }
        }
    }
}

@Composable
private fun LibRow(title: String, sub: String?, lead: String? = null, chevron: Boolean = false, onMore: (() -> Unit)? = null,
                   onClick: (() -> Unit)? = null) {
    Item(title = title, subtitle = sub, chevron = chevron, onClick = onClick ?: onMore,
        trailing = onMore?.let { { MoreButton(it) } },
        icon = lead?.let { { Txt(it, T.sub.copy(textAlign = TextAlign.End, fontFeatureSettings = "tnum"), Modifier.width(26.dp)) } })
}

@Composable
private fun AlbumRow(al: Album, unknown: String, showArtist: Boolean = false, onMore: ((Album) -> Unit)? = null, onClick: (Album) -> Unit) {
    val sub = listOfNotNull(if (showArtist) al.artist.ifEmpty { unknown } else null, al.year.takeIf { it > 0 }?.toString(),
        songs(al.tracks.size)).joinToString("  ·  ")
    Item(title = al.title.ifEmpty { unknown }, subtitle = sub, icon = { IconTile(Glyphs.Music, C.pink) }, chevron = true,
        trailing = onMore?.let { { MoreButton { it(al) } } }) { onClick(al) }
}

@Composable
internal fun songs(n: Int): String = pluralStringResource(R.plurals.sync_songs, n, n)

@Composable
internal fun albumsSongs(albums: Int, songs: Int): String =
    pluralStringResource(R.plurals.sync_albums, albums, albums) + ", " + songs(songs)

/** A segmented control. */
@Composable
internal fun Segments(labels: List<String>, selected: Int, onSelect: (Int) -> Unit) {
    Row(Modifier.padding(start = 16.dp, end = 16.dp, bottom = 16.dp).fillMaxWidth().clip(RoundedCornerShape(9.dp)).background(C.card).padding(2.dp)) {
        labels.forEachIndexed { i, l ->
            Box(Modifier.weight(1f).heightIn(min = 32.dp).clip(RoundedCornerShape(7.dp)).background(if (i == selected) C.fill else Color.Transparent)
                .clickable { onSelect(i) }, contentAlignment = Alignment.Center) {
                Txt(l, T.sub.copy(color = C.label, fontWeight = if (i == selected) androidx.compose.ui.text.font.FontWeight.SemiBold else null), maxLines = 1)
            }
        }
    }
}

/** "Update the library" and how it went (LIB_RESCAN mode 0). */
@Composable
internal fun RescanSection(st: SyncState, x: SyncActions, newMusic: Int) {
    when (val r = st.rescan) {
        Rescan.Running -> Section(footer = stringResource(R.string.sync_rescan_running_footer)) {
            row { Item(title = stringResource(R.string.sync_rescan_running), icon = { IconTile(Glyphs.Refresh, C.blue) }, trailing = { Spinner() }) }
        }
        Rescan.Done -> Section {
            row { Item(title = stringResource(R.string.sync_rescan_done), icon = { IconTile(Glyphs.Check, C.green) }) }
        }
        Rescan.Unsupported -> Section(footer = stringResource(R.string.sync_rescan_unsupported_text)) {
            row { Item(title = stringResource(R.string.sync_rescan_unsupported), icon = { IconTile(Glyphs.Info, C.orange) }) }
        }
        is Rescan.Failed, Rescan.Idle -> {
            if (r is Rescan.Failed) ErrorSection(r.error)
            Section(footer = when {
                newMusic > 0 -> stringResource(R.string.sync_rescan_new, songs(newMusic))
                st.libraryChanged -> stringResource(R.string.sync_lib_changed)
                else -> stringResource(R.string.sync_rescan_footer)
            }) {
                row {
                    Item(title = stringResource(R.string.sync_rescan), titleColor = C.blue, icon = { IconTile(Glyphs.Refresh, C.blue) },
                        enabled = !st.busy) { x.rescanLibrary() }
                }
            }
        }
    }
}

// ---- ByOS update ----------------------------------------------------------------------------

@Composable
fun SyncUpdate(s: UiState, a: Actions) {
    val ui = LocalSync.current
    val st = ui.state
    val x = ui.actions
    val b = s.bundle
    val p = s.patch
    val u = st.update
    val connected = st.connected != null
    val total = if (b != null && p is PatchState.Done) p.size + b.sdSize else 0L
    val bottom: (@Composable androidx.compose.foundation.layout.ColumnScope.() -> Unit)? = when {
        u is UpdatePhase.Sending -> { { GreyButton(stringResource(R.string.cancel)) { x.cancelWork() } } }
        u is UpdatePhase.Sent -> { { PrimaryButton(stringResource(R.string.go_on)) { a.go(Screen.INSTALL) } } }
        b == null -> null
        p !is PatchState.Done -> { { PrimaryButton(stringResource(R.string.sync_update_build, b.hibyosVersion)) { a.go(Screen.FIRMWARE) } } }
        !connected -> null
        else -> { { PrimaryButton(stringResource(if (u is UpdatePhase.Failed) R.string.try_again else R.string.wifi_send, mb(total))) { x.sendByosUpdate() } } }
    }
    SyncPage(s, a, Screen.SYNC_UPDATE, st.busy, bottom = bottom) {
        if (b == null) return@SyncPage
        when (u) {
            is UpdatePhase.Sending -> {
                ProgressCard(title = stringResource(R.string.sync_update_sending, b.hibyosVersion),
                    fraction = if (u.total > 0) u.done.toFloat() / u.total else 0f, detail2 = u.path.ifEmpty { " " },
                    note = stringResource(R.string.sync_sending_note))
                return@SyncPage
            }
            is UpdatePhase.Sent -> {
                Hero(Glyphs.Check, C.green, stringResource(R.string.sync_update_sent_title),
                    stringResource(R.string.sync_update_sent_text, u.files, u.skipped, b.outName))
                return@SyncPage
            }
            is UpdatePhase.Failed -> ErrorSection(u.error)
            UpdatePhase.Idle -> {}
        }
        if (!connected) NotConnected(a)
        Section(header = stringResource(R.string.sync_update_h), footer = stringResource(R.string.sync_update_footer)) {
            row { TextRow(stringResource(R.string.sync_update_text, b.hibyosVersion)) }
        }
        if (p is PatchState.Done) {
            Section(header = stringResource(R.string.send_what_h)) {
                row { Item(title = b.outName, subtitle = stringResource(R.string.send_item_fw_sub, mb(p.size)), icon = { IconTile(Glyphs.Doc, C.blue) }) }
                row(iconInset) {
                    Item(title = stringResource(R.string.send_item_folder), subtitle = stringResource(R.string.send_item_folder_sub, b.sdFiles, mb(b.sdSize)),
                        icon = { IconTile(Glyphs.Folder, C.cyan) })
                }
            }
        } else {
            Section {
                row { Item(title = stringResource(R.string.sync_update_need_build), icon = { IconTile(Glyphs.Alert, C.orange) }, chevron = true) { a.go(Screen.FIRMWARE) } }
            }
        }
    }
}
