plugins { alias(libs.plugins.agp.lib) }

val devSockName = rootProject.extra["devSockName"] as String

android {
    namespace = "org.matrix.vector.dex2oat"

    androidResources { enable = false }

    defaultConfig {
        externalNativeBuild { cmake { arguments += "-DDEV_SOCK_NAME=$devSockName" } }
    }

    externalNativeBuild { cmake { path("src/main/cpp/CMakeLists.txt") } }
}
