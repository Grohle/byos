package org.hibyos.cli

import java.io.PrintStream
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.system.exitProcess

/**
 * Entry point of byos-cli.jar. Ctrl+C (or Process.destroy()) cancels like {"cancel": true}
 * on stdin: the running request is cancelled, an `error` line with code `cancelled` is
 * printed, and the exit code is 7.
 */
fun main(args: Array<String>) {
    val stdout = PrintStream(java.io.FileOutputStream(java.io.FileDescriptor.out), false, "UTF-8")
    val cli = Cli(stdout = stdout)
    val done = CountDownLatch(1)
    Runtime.getRuntime().addShutdownHook(Thread {
        if (done.count == 0L) return@Thread
        cli.cancelled.set(true)
        done.await(2500, TimeUnit.MILLISECONDS)
        if (!cli.out.finished) {
            cli.out.event("error", linkedMapOf("ok" to false, "code" to "cancelled", "message" to "cancelled"))
        }
        stdout.flush()
        Runtime.getRuntime().halt(7)
    })
    val code = cli.run(args)
    done.countDown()
    stdout.flush()
    exitProcess(code)
}
