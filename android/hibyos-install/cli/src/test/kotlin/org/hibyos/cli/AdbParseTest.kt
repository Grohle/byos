package org.hibyos.cli

import org.junit.Assert.assertEquals
import org.junit.Test

/** `adb devices -l` parsing: the HiBy R3 II 2025 reports a serial with a space in it. */
class AdbParseTest {
    @Test
    fun serialWithSpaceIsKeptWhole() {
        val out = "List of devices attached\n" +
            "HiBy R3II_2025         device product:occam model:Nexus_4 device:mako transport_id:2663\n" +
            "emulator-5554          offline transport_id:3\n" +
            "ABC123                 no permissions (user in plugdev group); see [http://developer.android.com/tools/device.html]\n\n"
        val d = Adb.parseDevices(out)
        assertEquals(3, d.size)
        assertEquals("HiBy R3II_2025", d[0].serial)
        assertEquals("device", d[0].state)
        assertEquals("Nexus_4", d[0].model)
        assertEquals("emulator-5554", d[1].serial)
        assertEquals("offline", d[1].state)
        assertEquals("ABC123", d[2].serial)
        assertEquals("no permissions", d[2].state)
    }
}
