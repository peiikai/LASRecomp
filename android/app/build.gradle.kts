// The app: SDL's activity running the runtime (libmain.so, src/main/cpp/CMakeLists.txt). Nothing of the game is in
// it: the player picks their own extracted game files on the device. From LASRecompAndroid by mamcjr (MIT).
plugins {
    id("com.android.application")
}

val repo: File = rootDir.resolve("..").canonicalFile
val deps: File = repo.resolve("android/deps/out")  // android/deps/build_deps.sh
val gen = layout.buildDirectory.dir("generated/lasrecomp")

android {
    namespace = "com.lasrecomp"
    compileSdk = 36
    ndkVersion = "30.0.16248370"

    defaultConfig {
        // (nightly builds: .github/workflows/nightly.yml installs them as their own app, beside the release one)
        applicationId = System.getenv("LARECOMP_APPLICATION_ID") ?: "com.lasrecomp.android"
        manifestPlaceholders["appLabel"] = System.getenv("LARECOMP_APP_LABEL") ?: "LAS Recomp"
        minSdk = 30
        targetSdk = 36
        // (release.yml and nightly.yml set the build's version and a code that grows)
        versionCode = System.getenv("LARECOMP_VERSION_CODE")?.toInt() ?: 1
        versionName = System.getenv("LARECOMP_VERSION_NAME") ?: "0.1-dev"
        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    // The published APKs' key (.github/workflows/release.yml and nightly.yml, from the repository's secrets): Android installs an
    // update only over an app signed with the same key. Without it, the release build is unsigned.
    signingConfigs {
        System.getenv("LARECOMP_KEYSTORE")?.let { ks ->
            create("release") {
                storeFile = file(ks)
                storePassword = System.getenv("LARECOMP_KEYSTORE_PASSWORD")
                keyAlias = "lasrecomp"
                keyPassword = System.getenv("LARECOMP_KEYSTORE_PASSWORD")
            }
        }
    }
    buildTypes {
        getByName("release") {
            signingConfig = signingConfigs.findByName("release")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.30.4"
        }
    }

    sourceSets {
        getByName("main") {
            java.srcDirs("src/main/java", deps.resolve("java"))  // (SDL's Java side, the same version as libSDL3.so)
            jniLibs.srcDirs(gen.get().asFile.resolve("jniLibs"))
            assets.srcDirs(gen.get().asFile.resolve("assets"))
            res.srcDirs("src/main/res", gen.get().asFile.resolve("res"))
        }
    }

    packaging {
        jniLibs {
            // (extracted to nativeLibraryDir at install: the game code builder's programs run from there)
            useLegacyPackaging = true
            pickFirsts += listOf("**/libSDL3.so", "**/libshaderc_shared.so")
        }
    }

    androidResources {
        noCompress += "zip"  // (gen.zip is compressed already)
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

// What the APK carries from the repository besides libmain.so: the prebuilt libraries and the shipped resources the
// desktop builds put next to the executable (the start screen's font, the fallback fonts, the icon; their licences).
val prepareLasRecomp by tasks.registering {
    val res = mapOf(
        "Baloo2-SemiBold.ttf" to "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf",
        "Baloo2-OFL.txt" to "third_party/fonts/Baloo2/OFL.txt",
        "BIZUDPGothic-Regular.ttf" to "third_party/fonts/BIZUDPGothic/BIZUDPGothic-Regular.ttf",
        "BIZUDPGothic-OFL.txt" to "third_party/fonts/BIZUDPGothic/OFL.txt",
        "DejaVuSans.ttf" to "third_party/fonts/DejaVu/DejaVuSans.ttf",
        "DejaVuSans-Bold.ttf" to "third_party/fonts/DejaVu/DejaVuSans-Bold.ttf",
        "DejaVu-LICENSE.txt" to "third_party/fonts/DejaVu/LICENSE",
        "icon.png" to "docs/icon/icon_1024.png",
        // (BSD 2-Clause: the licence text ships with the binaries)
        "libadrenotools-LICENSE.txt" to "android/deps/out/adrenotools/LICENSE",
        "liblinkernsbypass-LICENSE.txt" to "android/deps/out/adrenotools/lib/linkernsbypass/LICENSE",
        "volk-LICENSE.md" to "android/deps/out/volk/LICENSE.md").mapValues { repo.resolve(it.value) }
    val libs = listOf("libSDL3.so", "libshaderc_shared.so", "libavcodec.so", "libavformat.so", "libavutil.so",
                      "libswscale.so").map { deps.resolve("lib/$it") } +
        // the game code builder's compiler (android/deps/build_gen_deps.sh): zig as lib*.so, the only files Android
        // runs from an app (the generator itself is in libmain.so: runtime/gen/)
        listOf("zig/libzig.so").map { deps.resolve("gen/$it") }
    inputs.files(res.values, libs)
    outputs.dir(gen)
    doLast {
        val out = gen.get().asFile
        for (lib in libs) lib.copyTo(out.resolve("jniLibs/arm64-v8a/${lib.name}"), overwrite = true)
        for ((name, f) in res) f.copyTo(out.resolve("assets/res/$name"), overwrite = true)
    }
}
// The game code builder's files, unpacked by the activity into <internal files>/gen (runtime/gamebuild.c): zig's lib/,
// and the headers and entry file the generated code is compiled with. Nothing of the game: the player's own files are
// the input.
val genZip by tasks.registering(Zip::class) {
    val g = deps.resolve("gen")
    archiveFileName.set("gen.zip")
    destinationDirectory.set(gen.map { it.dir("assets") })
    from(g.resolve("zig/lib")) { into("zig/lib") }
    from(g.resolve("zig/LICENSE-zig.txt")) { into("zig") }
    from(repo.resolve("runtime")) { include("cpu.h", "simd.h", "simd_fp.h", "gamelib.h"); into("gamelib-src") }
    from(repo.resolve("tools/gamelib_entry.c")) { into("gamelib-src") }
    from(repo.resolve("tools")) { include("hot_functions*.txt"); into("gamelib-src") }
    includeEmptyDirs = false
}
tasks.named("preBuild") { dependsOn(prepareLasRecomp, genZip) }
