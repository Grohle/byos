package org.hibyos.install

private val listItem = Regex("^(?:[-*+•]|[0-9]{1,2}[.)]|\\(?[a-z]\\))\\s")

/**
 * Licence texts come wrapped for an 80-column screen. This joins the lines of each paragraph
 * again so that a phone can wrap them itself, keeps list items and table rows on their own
 * lines, and marks headings with a leading "# ".
 */
fun noticeParagraphs(text: String): List<String> {
    val out = ArrayList<String>()
    val cur = StringBuilder()
    fun flush() {
        if (cur.isNotBlank()) out += cur.toString().trimEnd()
        cur.setLength(0)
    }
    for (raw in text.replace("\r\n", "\n").replace('\u000c', '\n').split('\n')) {
        val t = raw.trim()
        when {
            t.isEmpty() -> flush()
            t.startsWith("#") -> {
                flush()
                out += "# " + t.trimStart('#').trim().replace("`", "")
            }
            t.length >= 3 && t.all { it == '=' || it == '-' || it == '~' || it == '*' || it == '_' } -> {
                // a line of dashes under a title, or a rule between two parts
                if (cur.isNotBlank() && !cur.contains('\n') && cur.length < 80) {
                    out += "# " + cur.toString().trim()
                    cur.setLength(0)
                } else {
                    flush()
                }
            }
            t.startsWith("|") -> {
                flush()
                val cells = t.trim('|').split('|').map { it.trim().replace("`", "").replace("**", "") }
                if (!cells.all { c -> c.all { ch -> ch == '-' || ch == ':' } }) {
                    out += cells.filter { it.isNotEmpty() }.joinToString("  ·  ")
                }
            }
            listItem.containsMatchIn(t) -> {
                if (cur.isNotEmpty()) cur.append('\n')
                cur.append(t)
            }
            else -> {
                if (cur.isNotEmpty()) cur.append(' ')
                cur.append(t)
            }
        }
    }
    flush()
    return out
}
