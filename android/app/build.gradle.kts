import java.io.File
import java.util.zip.ZipFile

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
    id("org.jetbrains.kotlin.plugin.serialization")
}

android {
    namespace = "ru.avgn8bit"
    compileSdk = 35
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "ru.avgn8bit"
        minSdk = 24
        targetSdk = 35
        versionCode = 1
        versionName = "1.0.0"

        ndk {
            // Real phones. (The emulator's x86_64 ABI can be enabled locally by adding it here.)
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=none")
                cFlags += listOf("-std=gnu99")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            isShrinkResources = false
            // Signed with the debug key so the APK installs straight from a browser download. Replace with your
            // own keystore (see android/README.md) if you publish builds.
            signingConfig = signingConfigs.getByName("debug")
        }
        debug {
            isMinifyEnabled = false
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../native/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
        freeCompilerArgs += listOf("-opt-in=kotlin.RequiresOptIn")
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    androidResources {
        // Game data must stay uncompressed inside the APK: the app reads it straight through AssetManager and a
        // 170 MB deflate stream inside the APK would be useless (no random access, slow first launch).
        noCompress += setOf("win", "mp4", "ttf", "otf", "bin", "dat", "zip")
    }

    // The optional bundled game (see the bundleGame task below) lands here.
    sourceSets["main"].assets.srcDir(layout.buildDirectory.dir("generated/bundledGame"))

    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
        resources {
            excludes += setOf("META-INF/*.kotlin_module", "META-INF/LICENSE*", "META-INF/*.txt")
        }
    }

    lint {
        abortOnError = false
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.documentfile:documentfile:1.0.1")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.7")

    implementation(platform("androidx.compose:compose-bom:2024.10.01"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")
    debugImplementation("androidx.compose.ui:ui-tooling")

    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.7.3")
}

// ---------------------------------------------------------------------------------------------------------------------
// Optional "everything in one APK" build.
//
//   ./gradlew assembleRelease -PbundleGame=/path/to/game.zip             (or a folder)
//   ./gradlew assembleRelease -PbundleGame=game.zip -PbundleVideos=true  (include the cutscenes)
//
// Only the files the port needs are copied: data.win (or an already patched data_vita.win), the .ttf/.otf fonts and
// the Lang, Attract and Cutscenes folders. The cutscenes are skipped by default: they are by far the biggest part of
// the install and the game simply skips the videos when they are missing.
//
// The files become assets and are unpacked into the app's private storage on the first launch (BundledGame.kt),
// because the runtime opens data.win as a regular file and writes its texture cache next to it.
// ---------------------------------------------------------------------------------------------------------------------
val bundleGameSource: Provider<String> = providers.gradleProperty("bundleGame")
val bundleVideos: Provider<Boolean> = providers.gradleProperty("bundleVideos").map(String::toBoolean).orElse(false)
val bundledAssetsDir: Provider<Directory> = layout.buildDirectory.dir("generated/bundledGame")

val bundleGame by tasks.registering {
    group = "build"
    description = "Copies the game given by -PbundleGame into assets/game (optional all-in-one APK)."
    val source = bundleGameSource
    val videos = bundleVideos
    val output = bundledAssetsDir

    inputs.property("bundleGame", source.map { it }.orElse(""))
    inputs.property("bundleVideos", videos)
    outputs.dir(output)

    doLast {
        val assets = output.get().asFile
        assets.deleteRecursively()
        assets.mkdirs()

        val raw = source.orNull?.trim()
        if (raw.isNullOrEmpty()) {
            logger.lifecycle("bundleGame is not set: the APK will not contain the game files.")
            return@doLast
        }
        val input = file(raw)
        if (!input.exists()) throw GradleException("bundleGame points to a missing path: $input")

        val rootDirs = listOf("Lang", "Attract", "Cutscenes")
        val fontExtensions = listOf(".ttf", ".otf")
        val includeVideos = videos.get()

        // relative path inside assets/game -> where the bytes come from
        val zipEntries = LinkedHashMap<String, String>()
        val fromDisk = LinkedHashMap<String, File>()

        fun wanted(relative: String): Boolean {
            if (relative.isEmpty() || relative.endsWith("/")) return false
            if (relative.split('/').any { it == ".." || it.isEmpty() || it.startsWith(".") }) return false
            if (relative.contains("__MACOSX")) return false
            val slash = relative.count { it == '/' }
            val keep = if (slash == 0) {
                val lower = relative.lowercase()
                lower == "data.win" || lower == "data_vita.win" || fontExtensions.any { lower.endsWith(it) }
            } else {
                rootDirs.contains(relative.substringBefore('/'))
            }
            if (!keep) return false
            if (!includeVideos && relative.lowercase().endsWith(".mp4")) return false
            return true
        }

        if (input.isDirectory) {
            fun hasData(dir: File) =
                File(dir, "data.win").isFile || File(dir, "data_vita.win").isFile

            var root = input
            if (!hasData(root)) {
                root = input.listFiles()?.firstOrNull { it.isDirectory && hasData(it) } ?: input
            }
            root.walkTopDown().filter { it.isFile }.forEach { file ->
                val relative = file.relativeTo(root).invariantSeparatorsPath
                if (wanted(relative)) fromDisk[relative] = file
            }
            logger.lifecycle("bundleGame: ${fromDisk.size} files from $root")
        } else {
            val zip = ZipFile(input)
            val names = zip.entries().toList()
                .filter { !it.isDirectory }
                .map { it.name.replace('\\', '/') }
            val root = names
                .filter { name ->
                    val lower = name.lowercase()
                    lower == "data.win" || lower == "data_vita.win" ||
                        lower.endsWith("/data.win") || lower.endsWith("/data_vita.win")
                }
                .minByOrNull { it.count { c -> c == '/' } }
                ?.substringBeforeLast('/', "")
                ?: ""
            val prefix = if (root.isEmpty()) "" else "$root/"
            names.filter { it.startsWith(prefix) }.forEach { name ->
                val relative = name.removePrefix(prefix)
                if (wanted(relative)) {
                    zipEntries[relative] = name
                }
            }
            if (zipEntries.isEmpty()) {
                zip.close()
                throw GradleException(
                    "bundleGame: no data.win / fonts / Lang / Attract / Cutscenes found in ${input.name}. " +
                        "Point it at the game folder itself or a zip of it."
                )
            }
            zipEntries.forEach { (relative, name) ->
                val target = File(assets, "game/$relative")
                target.parentFile?.mkdirs()
                zip.getInputStream(zip.getEntry(name)).use { stream ->
                    target.outputStream().use { stream.copyTo(it, 1 shl 16) }
                }
            }
            zip.close()
            logger.lifecycle("bundleGame: ${zipEntries.size} files from ${input.name}")
        }

        fromDisk.forEach { (relative, file) ->
            val target = File(assets, "game/$relative")
            target.parentFile?.mkdirs()
            file.copyTo(target, overwrite = true)
        }

        val total = File(assets, "game").walkTopDown().filter { it.isFile }.sumOf { it.length() }
        logger.lifecycle("bundleGame: assets/game is ${total / 1048576} MB")
    }
}

// The asset merge must run after the files are in place.
tasks.matching { it.name.startsWith("merge") && it.name.endsWith("Assets") }.configureEach {
    dependsOn(bundleGame)
}
