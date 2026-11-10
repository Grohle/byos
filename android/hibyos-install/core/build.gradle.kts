plugins {
    id("org.jetbrains.kotlin.jvm")
}

kotlin {
    jvmToolchain(21)
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

dependencies {
    testImplementation("junit:junit:4.13.2")
}

tasks.test {
    useJUnit()
    maxHeapSize = "256m"   // the patcher must work in a phone-sized heap
    // optional inputs for the tests that need the real (not redistributable) firmware
    listOf("HIBYOS_FIRMWARE", "HIBYOS_REPO", "HIBYOS_PYTHON", "HIBYOS_EXPORT_PYTHON", "HIBYOS_SD", "HIBYOS_WORK", "HIBYOS_KEY_WORK").forEach { k ->
        System.getenv(k)?.let { environment(k, it) }
    }
    testLogging {
        events("passed", "skipped", "failed")
        showStandardStreams = true
        exceptionFormat = org.gradle.api.tasks.testing.logging.TestExceptionFormat.FULL
    }
    outputs.upToDateWhen { false }
}
