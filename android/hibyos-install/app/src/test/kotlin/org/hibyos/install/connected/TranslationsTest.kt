package org.hibyos.install.connected

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory

/** Every text of the connected mode exists in English, French and Spanish, with the same placeholders. */
class TranslationsTest {
    private fun res(): File {
        var d: File? = File("").absoluteFile
        while (d != null) {
            for (c in listOf(File(d, "src/main/res"), File(d, "app/src/main/res"))) if (File(c, "values/strings_connected.xml").isFile) return c
            d = d.parentFile
        }
        throw AssertionError("res folder not found")
    }

    /** name -> text, for strings and for each plural quantity. */
    private fun read(f: File): Map<String, String> {
        val doc = DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(f)
        val out = LinkedHashMap<String, String>()
        val strings = doc.getElementsByTagName("string")
        for (i in 0 until strings.length) {
            val e = strings.item(i) as org.w3c.dom.Element
            out[e.getAttribute("name")] = e.textContent
        }
        val plurals = doc.getElementsByTagName("plurals")
        for (i in 0 until plurals.length) {
            val p = plurals.item(i) as org.w3c.dom.Element
            out["plural:" + p.getAttribute("name")] = p.getElementsByTagName("item").let { items ->
                (0 until items.length).joinToString("|") { (items.item(it) as org.w3c.dom.Element).textContent }
            }
        }
        return out
    }

    private fun placeholders(s: String): List<String> = Regex("%(\\d+\\$)?[sd]").findAll(s).map { it.value }.toSortedSet().toList()

    @Test
    fun frenchAndSpanishHaveEveryText() {
        val dir = res()
        val en = read(File(dir, "values/strings_connected.xml"))
        assertTrue(en.size > 250)
        for (lang in listOf("fr", "es")) {
            val other = read(File(dir, "values-$lang/strings_connected.xml"))
            assertEquals("$lang: missing", emptySet<String>(), en.keys - other.keys)
            assertEquals("$lang: unknown", emptySet<String>(), other.keys - en.keys)
            for ((k, v) in en) {
                if (k.startsWith("plural:")) continue
                assertEquals("$lang/$k placeholders", placeholders(v), placeholders(other[k]!!))
                assertTrue("$lang/$k is empty", other[k]!!.isNotBlank())
            }
        }
    }
}
