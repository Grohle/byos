package org.hibyos.install

import android.content.ActivityNotFoundException
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
import androidx.compose.runtime.LaunchedEffect
import androidx.core.content.FileProvider
import org.hibyos.install.ui.App
import java.io.File

class MainActivity : ComponentActivity(), Host {
    private val vm: InstallViewModel by viewModels()

    private val openFirmware = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) vm.importFirmware(uri)
    }

    private val openFolder = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) vm.saveTo(uri)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge(SystemBarStyle.dark(Color.TRANSPARENT), SystemBarStyle.dark(Color.TRANSPARENT))
        super.onCreate(savedInstanceState)
        vm.host = this
        setContent {
            val s = vm.state
            // while work is running "back" is held: the screen offers Cancel instead
            BackHandler(enabled = s.stack.size > 1 || s.busy) { vm.back() }
            LaunchedEffect(s.busy) {
                if (s.busy) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            }
            App(s, vm)
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
