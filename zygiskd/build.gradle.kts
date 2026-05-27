plugins {
    alias(libs.plugins.agp.lib)
    alias(libs.plugins.rust.android)
}

val minKsuVersion: Int by rootProject.extra
val minMagiskVersion: Int by rootProject.extra
val verCode: Int by rootProject.extra
val verName: String by rootProject.extra
val commitHash: String by rootProject.extra

android.buildFeatures {
    androidResources = false
    buildConfig = false
}

cargo {
    module = "."
    libname = "r0zd"
    targetIncludes = arrayOf("r0zd")
    targets = listOf("arm64", "arm", "x86", "x86_64")
    targetDirectory = "build/intermediates/rust"
    val isDebug = gradle.startParameter.taskNames.any { it.toLowerCase().contains("debug") }
    profile = if (isDebug) "debug" else "release"
    exec = { spec, _ ->
        spec.environment("ANDROID_NDK_HOME", android.ndkDirectory.path)
        spec.environment("MIN_KSU_VERSION", minKsuVersion)
        spec.environment("MIN_MAGISK_VERSION", minMagiskVersion)
        spec.environment("ZKSU_VERSION", "$verName-$verCode-$commitHash-$profile")
    }
}

afterEvaluate {
    task<Task>("buildAndStrip") {
        dependsOn(":r0zd:cargoBuild")
        val isDebug = gradle.startParameter.taskNames.any { it.toLowerCase().contains("debug") }
        doLast {
            val dir = File(buildDir, "rustJniLibs/android")
            val prebuilt = File(android.ndkDirectory, "toolchains/llvm/prebuilt").listFiles()!!.first()
            val binDir = File(prebuilt, "bin")
            val symbolDir = File(buildDir, "symbols/${if (isDebug) "debug" else "release"}")
            symbolDir.mkdirs()
            val suffix = if (prebuilt.name.contains("windows")) ".exe" else ""
            val strip = File(binDir, "llvm-strip$suffix")
            val objcopy = File(binDir, "llvm-objcopy$suffix")
            dir.listFiles()!!.forEach {
                if (!it.isDirectory) return@forEach
                val symbolPath = File(symbolDir, "${it.name}/r0zd.debug")
                symbolPath.parentFile.mkdirs()
                exec {
                    workingDir = it
                    commandLine(objcopy, "--only-keep-debug", "r0zd", symbolPath)
                }
                exec {
                    workingDir = it
                    commandLine(strip, "--strip-all", "r0zd")
                }
                exec {
                    workingDir = it
                    commandLine(objcopy, "--add-gnu-debuglink", symbolPath, "r0zd")
                }
            }
        }
    }
}
