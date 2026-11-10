package org.hibyos.install.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.fail
import org.junit.Test

class JsonTest {
    @Test
    fun parsesValues() {
        val v = Json.obj(Json.parse("""
            {"a": 1, "b": -2.5e1, "c": "x\n\u00e9\ud83d\ude00\"\\/", "d": [true, false, null, [], {}], "e": {"f": 9007199254740991}}
        """))
        assertEquals(1L, v["a"])
        assertEquals(-25.0, v["b"])
        assertEquals("x\n\u00e9\ud83d\ude00\"\\/", v["c"])
        assertEquals(listOf(true, false, null, emptyList<Any?>(), emptyMap<String, Any?>()), v["d"])
        assertEquals(9007199254740991L, Json.long(Json.obj(v["e"]), "f"))
        assertEquals(-25L, Json.long(v, "b"))
        assertEquals(7L, Json.optLong(v, "zz", 7))
        assertNull(Json.optStr(v, "a"))
    }

    @Test
    fun quoteRoundTrips() {
        val s = "line\nbreak \"quoted\" back\\slash \t tab \u0001 é 中"
        assertEquals(s, Json.parse(Json.quote(s)))
    }

    @Test
    fun rejectsBrokenText() {
        for (bad in listOf("", "{", "[1,]", "{\"a\":}", "{\"a\" 1}", "nul", "\"abc", "{\"a\":1} x", "[1 2]", "01", "\"\\u12\"",
            "[".repeat(5000))) {
            try {
                Json.parse(bad)
                fail("accepted: ${bad.take(20)}")
            } catch (e: JsonException) {
                // expected
            }
        }
    }
}
