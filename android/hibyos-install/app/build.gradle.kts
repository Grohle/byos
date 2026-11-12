import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
    id("io.github.takahirom.roborazzi")
}

// The release key lives outside the repository. Without it the release build is left unsigned.
val keystoreProps = Properties()
val keystorePropsFile = File(
    System.getenv("HIBYOS_INSTALL_KEYSTORE_PROPERTIES")
        ?: (System.getProperty("user.home") + "/.hibyos/keys/hibyos-install.properties"),
)
if (keystorePropsFile.isFile) keystorePropsFile.inputStream().use { keystoreProps.load(it) }

android {
    namespace = "org.hibyos.install"
    compileSdk = 36

    defaultConfig {
        applicationId = "org.hibyos.install"
        minSdk = 26
        targetSdk = 36
        versionCode = 2
        versionName = "1.1.0"
    }

    signingConfigs {
        if (keystoreProps.getProperty("storeFile") != null) {
            create("release") {
                storeFile = keystorePropsFile.parentFile.resolve(keystoreProps.getProperty("storeFile"))
                storePassword = keystoreProps.getProperty("storePassword")
                keyAlias = keystoreProps.getProperty("keyAlias")
                keyPassword = keystoreProps.getProperty("keyPassword") ?: keystoreProps.getProperty("storePassword")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.findByName("release")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    androidResources {
        // the bundle is a zip already
        noCompress += "zip"
    }

    packaging {
        resources.excludes += setOf("META-INF/{AL2.0,LGPL2.1}", "META-INF/*.kotlin_module", "kotlin/**", "DebugProbesKt.bin")
    }

    lint {
        // only the "ByOS connected" texts are translated (values-es, values-fr); the rest stays English
        warning += "MissingTranslation"
    }

    dependenciesInfo {
        // no encrypted dependency list for an app store in the APK: the file must be reproducible and readable
        includeInApk = false
        includeInBundle = false
    }

    testOptions {
        unitTests {
            isIncludeAndroidResources = true
            all { test ->
                test.maxHeapSize = "1536m"
                listOf("HIBYOS_FIRMWARE", "HIBYOS_WORK", "HIBYOS_SHOTS").forEach { k ->
                    System.getenv(k)?.let { test.environment(k, it) }
                }
                test.systemProperty("hibyos.shots", rootProject.file("docs/screens").path)
                test.testLogging {
                    events("passed", "skipped", "failed")
                    showStandardStreams = true
                    exceptionFormat = org.gradle.api.tasks.testing.logging.TestExceptionFormat.FULL
                }
            }
        }
    }
}

kotlin {
    jvmToolchain(21)
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

dependencies {
    implementation(project(":core"))
    implementation(platform("androidx.compose:compose-bom:2025.09.00"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.activity:activity-compose:1.11.0")
    implementation("androidx.documentfile:documentfile:1.1.0")

    testImplementation("junit:junit:4.13.2")
    testImplementation("org.robolectric:robolectric:4.16")
    testImplementation("io.github.takahirom.roborazzi:roborazzi:1.51.0")
    testImplementation("io.github.takahirom.roborazzi:roborazzi-compose:1.51.0")
    testImplementation("io.github.takahirom.roborazzi:roborazzi-junit-rule:1.51.0")
    testImplementation(platform("androidx.compose:compose-bom:2025.09.00"))
    testImplementation("androidx.compose.ui:ui-test-junit4")
    debugImplementation(platform("androidx.compose:compose-bom:2025.09.00"))
    debugImplementation("androidx.compose.ui:ui-test-manifest")
}

// ---- the ByOS files that go into the APK --------------------------------------------------
// app/src/main/assets/hibyos-bundle.zip holds our own files and the list of edits (never any
// HiBy firmware). It is made from the project's Python definitions by tools/export_bundle.py,
// or taken ready-made with -PhibyosBundle=<file> (for example the bundle of a release).
val bundleAsset = layout.projectDirectory.file("src/main/assets/hibyos-bundle.zip").asFile
val givenBundle = providers.gradleProperty("hibyosBundle").orNull
val exportPython = providers.gradleProperty("hibyosPython").orNull ?: System.getenv("HIBYOS_EXPORT_PYTHON") ?: "python"
val exporter = rootProject.file("tools/export_bundle.py")

val prepareHibyosBundle = tasks.register("prepareHibyosBundle") {
    description = "Puts the ByOS bundle in the assets (from -PhibyosBundle=<file>, or by running tools/export_bundle.py)."
    outputs.upToDateWhen { givenBundle == null && bundleAsset.isFile }
    doLast {
        bundleAsset.parentFile.mkdirs()
        if (givenBundle != null) {
            val src = File(givenBundle)
            if (!src.isFile) throw GradleException("hibyosBundle: $src not found")
            src.copyTo(bundleAsset, overwrite = true)
            logger.lifecycle("ByOS bundle taken from $src")
        } else if (!bundleAsset.isFile) {
            logger.lifecycle("No ByOS bundle in the assets: running $exporter")
            val p = ProcessBuilder(exportPython, exporter.path).directory(rootProject.projectDir).inheritIO().start()
            if (p.waitFor() != 0 || !bundleAsset.isFile) {
                throw GradleException(
                    "The ByOS bundle could not be made. Build the ByOS files first (see the README, \"Build from source\"), " +
                        "or pass a ready-made bundle with -PhibyosBundle=<path to hibyos-install-bundle-x.y.z.zip>.",
                )
            }
        }
    }
}
tasks.named("preBuild") { dependsOn(prepareHibyosBundle) }
