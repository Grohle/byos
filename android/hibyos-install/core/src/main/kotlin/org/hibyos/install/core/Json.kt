package org.hibyos.install.core

/**
 * Very small JSON reader (objects become Map<String, Any?>, arrays List<Any?>, numbers Long
 * or Double). The core module has no dependencies, and org.json is not part of the JVM.
 */
class JsonException(message: String) : Exception(message)

object Json {
    fun parse(text: String): Any? {
        val p = Parser(text)
        p.ws()
        val v = p.value(0)
        p.ws()
        if (p.i != text.length) throw JsonException("unexpected text after the JSON value at ${p.i}")
        return v
    }

    @Suppress("UNCHECKED_CAST")
    fun obj(v: Any?): Map<String, Any?> = v as? Map<String, Any?> ?: throw JsonException("a JSON object was expected")

    fun arr(v: Any?): List<Any?> = v as? List<Any?> ?: throw JsonException("a JSON array was expected")

    fun str(m: Map<String, Any?>, key: String): String =
        m[key] as? String ?: throw JsonException("missing text field \"$key\"")

    fun optStr(m: Map<String, Any?>, key: String): String? = m[key] as? String

    fun long(m: Map<String, Any?>, key: String): Long = when (val v = m[key]) {
        is Long -> v
        is Double -> if (v == Math.floor(v) && Math.abs(v) < 9e15) v.toLong() else throw JsonException("field \"$key\" is not an integer")
        else -> throw JsonException("missing number field \"$key\"")
    }

    fun optLong(m: Map<String, Any?>, key: String, def: Long): Long = if (m[key] == null) def else long(m, key)

    /** Encodes a string as a JSON string literal. */
    fun quote(s: String): String {
        val sb = StringBuilder(s.length + 2).append('"')
        for (c in s) {
            when {
                c == '"' -> sb.append("\\\"")
                c == '\\' -> sb.append("\\\\")
                c == '\n' -> sb.append("\\n")
                c == '\r' -> sb.append("\\r")
                c == '\t' -> sb.append("\\t")
                c.code < 0x20 -> sb.append(String.format("\\u%04x", c.code))
                else -> sb.append(c)
            }
        }
        return sb.append('"').toString()
    }

    private class Parser(val s: String) {
        var i = 0

        fun ws() {
            while (i < s.length && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++
        }

        fun fail(what: String): Nothing = throw JsonException("$what at position $i")

        fun value(depth: Int): Any? {
            if (depth > 64) fail("JSON nested too deep")
            if (i >= s.length) fail("unexpected end of JSON")
            return when (val c = s[i]) {
                '{' -> {
                    i++
                    val m = LinkedHashMap<String, Any?>()
                    ws()
                    if (peek() == '}') { i++; return m }
                    while (true) {
                        ws()
                        if (peek() != '"') fail("a field name was expected")
                        val k = string()
                        ws()
                        if (peek() != ':') fail("':' was expected")
                        i++
                        ws()
                        m[k] = value(depth + 1)
                        ws()
                        when (peek()) {
                            ',' -> i++
                            '}' -> { i++; return m }
                            else -> fail("',' or '}' was expected")
                        }
                    }
                    @Suppress("UNREACHABLE_CODE")
                    m
                }
                '[' -> {
                    i++
                    val l = ArrayList<Any?>()
                    ws()
                    if (peek() == ']') { i++; return l }
                    while (true) {
                        ws()
                        l.add(value(depth + 1))
                        ws()
                        when (peek()) {
                            ',' -> i++
                            ']' -> { i++; return l }
                            else -> fail("',' or ']' was expected")
                        }
                    }
                    @Suppress("UNREACHABLE_CODE")
                    l
                }
                '"' -> string()
                't' -> lit("true", true)
                'f' -> lit("false", false)
                'n' -> lit("null", null)
                else -> if (c == '-' || c in '0'..'9') number() else fail("unexpected character '$c'")
            }
        }

        fun peek(): Char = if (i < s.length) s[i] else fail("unexpected end of JSON")

        fun lit(word: String, v: Any?): Any? {
            if (!s.startsWith(word, i)) fail("unexpected text")
            i += word.length
            return v
        }

        fun number(): Any {
            val st = i
            if (s[i] == '-') i++
            val digits = i
            while (i < s.length && s[i] in '0'..'9') i++
            if (i == digits || (s[digits] == '0' && i - digits > 1)) fail("bad number")
            var real = false
            if (i < s.length && s[i] == '.') { real = true; i++; while (i < s.length && s[i] in '0'..'9') i++ }
            if (i < s.length && (s[i] == 'e' || s[i] == 'E')) {
                real = true
                i++
                if (i < s.length && (s[i] == '+' || s[i] == '-')) i++
                while (i < s.length && s[i] in '0'..'9') i++
            }
            val t = s.substring(st, i)
            return try {
                if (real) t.toDouble() else t.toLong()
            } catch (e: NumberFormatException) {
                fail("bad number")
            }
        }

        fun string(): String {
            i++ // opening quote
            val sb = StringBuilder()
            while (true) {
                if (i >= s.length) fail("unterminated string")
                val c = s[i++]
                when (c) {
                    '"' -> return sb.toString()
                    '\\' -> {
                        if (i >= s.length) fail("unterminated string")
                        when (val e = s[i++]) {
                            '"', '\\', '/' -> sb.append(e)
                            'b' -> sb.append('\b')
                            'f' -> sb.append('\u000c')
                            'n' -> sb.append('\n')
                            'r' -> sb.append('\r')
                            't' -> sb.append('\t')
                            'u' -> {
                                if (i + 4 > s.length) fail("bad \\u escape")
                                val code = s.substring(i, i + 4).toIntOrNull(16) ?: fail("bad \\u escape")
                                sb.append(code.toChar())
                                i += 4
                            }
                            else -> fail("bad escape")
                        }
                    }
                    else -> sb.append(c)
                }
            }
        }
    }
}
