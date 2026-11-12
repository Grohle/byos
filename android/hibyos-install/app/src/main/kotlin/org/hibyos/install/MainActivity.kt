package org.hibyos.install

import android.Manifest
import android.content.ActivityNotFoundException
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.IntentCompat
import android.content.ClipData
import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.core.content.FileProvider
import org.hibyos.install.ui.App
import org.hibyos.install.ui.LocalSync
import org.hibyos.install.ui.SyncUi
import java.io.File

class MainActivity : ComponentActivity(), Host {
    private val vm: InstallViewModel by viewModels()

    private val openFirmware = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) vm.importFirmware(uri)
    }

    private val openFolder = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) vm.saveTo(uri)
    }

    private val openSyncFiles = registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isNotEmpty()) vm.syncFilesPicked(uris)
    }

    private val openSyncFolder = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) vm.syncFolderPicked(uri)
    }

    private val openSyncUpdate = registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isNotEmpty()) vm.syncUpdatePicked(uris)
    }

    private val askNotifications = registerForActivityResult(ActivityResultContracts.RequestPermission()) {
        // either way the transfer goes on; without the permission its notification is not shown
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge(SystemBarStyle.dark(Color.TRANSPARENT), SystemBarStyle.dark(Color.TRANSPARENT))
        super.onCreate(savedInstanceState)
        vm.host = this
        if (savedInstanceState == null) shared(intent)
        setContent {
            val s = vm.state
            val sync = vm.connected.state
            val busy = s.busy || sync.busy
            // while work is running "back" is held: the screen offers Cancel instead
            BackHandler(enabled = s.stack.size > 1 || busy) { vm.back() }
            LaunchedEffect(busy) {
                if (busy) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            }
            CompositionLocalProvider(LocalSync provides SyncUi(sync, vm.connected)) { App(s, vm) }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        shared(intent)
    }

    /** Files shared from another app ("Share" › ByOS Install): they go to Send Files. */
    private fun shared(intent: Intent?) {
        if (intent == null) return
        val uris = ArrayList<Uri>()
        when (intent.action) {
            Intent.ACTION_SEND -> IntentCompat.getParcelableExtra(intent, Intent.EXTRA_STREAM, Uri::class.java)?.let { uris += it }
            Intent.ACTION_SEND_MULTIPLE -> IntentCompat.getParcelableArrayListExtra(intent, Intent.EXTRA_STREAM, Uri::class.java)?.let { uris += it }
            else -> return
        }
        if (uris.isEmpty()) {
            val clip = intent.clipData
            if (clip != null) for (i in 0 until clip.itemCount) clip.getItemAt(i).uri?.let { uris += it }
        }
        if (uris.isEmpty()) {
            Toast.makeText(this, R.string.sync_share_nothing, Toast.LENGTH_LONG).show()
            return
        }
        vm.syncShared(uris.distinct())
    }

    override fun askNotifications() {
        if (Build.VERSION.SDK_INT < 33) return
        if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) return
        try {
            askNotifications.launch(Manifest.permission.POST_NOTIFICATIONS)
        } catch (e: Exception) {
            // no way to ask: the transfer goes on without its notification
        }
    }

    override fun onDestroy() {
        if (vm.host === this) vm.host = null
        super.onDestroy()
    }

    private fun noApp() {
        Toast.makeText(this, R.string.err_no_app, Toast.LENGTH_LONG).show()
    }

    override fun pickFirmware() {
        try {
            openFirmware.launch(arrayOf("*/*"))
        } catch (e: ActivityNotFoundException) {
            noApp()
        }
    }

    override fun pickFolder() {
        try {
            openFolder.launch(null)
        } catch (e: ActivityNotFoundException) {
            noApp()
        }
    }

    override fun pickSyncFiles() {
        try {
            openSyncFiles.launch(arrayOf("*/*"))
        } catch (e: ActivityNotFoundException) {
            noApp()
        }
    }

    override fun pickSyncFolder() {
        try {
            openSyncFolder.launch(null)
        } catch (e: ActivityNotFoundException) {
            noApp()
        }
    }

    override fun pickSyncUpdate() {
        try {
            openSyncUpdate.launch(arrayOf("*/*"))
        } catch (e: ActivityNotFoundException) {
            noApp()
        }
    }

    override fun open(url: String): Boolean = try {
        startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
        true
    } catch (e: ActivityNotFoundException) {
        noApp()
        false
    }

    override fun share(files: List<File>) {
        try {
            val uris = ArrayList<Uri>()
            for (f in files) uris += FileProvider.getUriForFile(this, "$packageName.files", f)
            val send = Intent(Intent.ACTION_SEND_MULTIPLE).apply {
                type = "application/octet-stream"
                putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                // the grant travels with the clip data, also through the chooser
                clipData = ClipData.newRawUri(null, uris[0]).also { c ->
                    for (i in 1 until uris.size) c.addItem(ClipData.Item(uris[i]))
                }
            }
            startActivity(Intent.createChooser(send, null))
        } catch (e: ActivityNotFoundException) {
            noApp()
        } catch (e: IllegalArgumentException) {
            noApp()
        }
    }
}
