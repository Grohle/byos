plugins {
    id("org.jetbrains.kotlin.jvm")
}

// byos-cli: command-line client of the ByOS core (docs/BYOS_CLI.md). No dependencies beyond
// the core and the Kotlin standard library; `fatJar` bundles both into byos-cli.jar.

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
    implementation(project(":core"))
    testImplementation("junit:junit:4.13.2")
    testImplementation(testFixtures(project(":core")))
}

val fatJar by tasks.registering(Jar::class) {
    group = "build"
    description = "Builds cli/build/libs/byos-cli.jar with the core and the Kotlin runtime inside."
    archiveFileName.set("byos-cli.jar")
    manifest { attributes("Main-Class" to "org.hibyos.cli.MainKt") }
    duplicatesStrategy = DuplicatesStrategy.EXCLUDE
    from(sourceSets.main.get().output)
    dependsOn(configurations.runtimeClasspath)
    // the notices and licence texts that apply to what the jar contains (THIRD_PARTY_NOTICES.md section 7)
    val repo = rootProject.projectDir.parentFile.parentFile
    from(File(repo, "THIRD_PARTY_NOTICES.md")) { into("META-INF/byos-licenses") }
    from(File(repo, "LICENSE")) { into("META-INF/byos-licenses"); rename { "ByOS-LICENSE" } }
    from(File(repo, "LICENSES")) {
        include("Apache-2.0.txt", "TweetNaCl-public-domain.txt", "hiby-mods-LICENSE.txt")
        into("META-INF/byos-licenses")
    }
    from({ configurations.runtimeClasspath.get().filter { it.name.endsWith(".jar") }.map { zipTree(it) } }) {
        exclude("META-INF/*.SF", "META-INF/*.DSA", "META-INF/*.RSA", "META-INF/versions/9/module-info.class", "module-info.class")
    }
}

tasks.named("assemble") { dependsOn(fatJar) }

tasks.test {
    useJUnit()
    maxHeapSize = "512m"
    environment("HIBYOS_REPO_HINT", rootProject.projectDir.parentFile.parentFile.absolutePath)
    testLogging {
        events("passed", "skipped", "failed")
        exceptionFormat = org.gradle.api.tasks.testing.logging.TestExceptionFormat.FULL
    }
    outputs.upToDateWhen { false }
}

// The ByOS files the Android app ships, inside the jar too: build-info, firmware-fetch and build
// work without --bundle, from the very same file (docs/BYOS_CLI.md, "Build ByOS").
tasks.named<ProcessResources>("processResources") {
    // the same preparation as the app (exported from the Python definitions, or -PhibyosBundle=<file>)
    dependsOn(":app:prepareHibyosBundle")
    from(rootProject.file("app/src/main/assets/hibyos-bundle.zip")) { into("org/hibyos/cli") }
}
