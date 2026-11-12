package org.hibyos.install.connected

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import androidx.documentfile.provider.DocumentFile
import java.io.IOException

/** Turns what the system pickers return into files to send. Runs on the worker. */
object Picked {
    fun files(context: Context, uris: List<Uri>): List<LocalItem> {
        val out = ArrayList<LocalItem>()
        for (u in uris) {
            var name: String? = null
            var size = -1L
            context.contentResolver.query(u, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use { c ->
                if (c.moveToFirst()) {
                    if (!c.isNull(0)) name = c.getString(0)
                    if (!c.isNull(1)) size = c.getLong(1)
                }
            }
            val df = DocumentFile.fromSingleUri(context, u)
            if (size < 0) size = df?.length() ?: 0
            val mtime = (df?.lastModified() ?: 0L) / 1000
            out += LocalItem(listOf(name ?: u.lastPathSegment ?: "file"), size, mtime) { open(context, u) }
        }
        return out
    }

    /** Every file under the picked folder; the folder itself is kept as the first part of each path. */
    fun tree(context: Context, tree: Uri): List<LocalItem> {
        val root = DocumentFile.fromTreeUri(context, tree) ?: throw IOException("the folder cannot be read")
        val out = ArrayList<LocalItem>()
        fun walk(d: DocumentFile, parts: List<String>, depth: Int) {
            if (depth > 30) return
            for (f in d.listFiles()) {
                val n = f.name ?: continue
                if (f.isDirectory) walk(f, parts + n, depth + 1)
                else if (f.isFile) out += LocalItem(parts + n, f.length(), f.lastModified() / 1000) { open(context, f.uri) }
            }
        }
        walk(root, listOf(root.name ?: "Folder"), 0)
        return out
    }

    private fun open(context: Context, u: Uri) = context.contentResolver.openInputStream(u) ?: throw IOException("the file cannot be opened")
}
