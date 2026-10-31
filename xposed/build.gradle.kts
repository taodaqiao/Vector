@Suppress("UNCHECKED_CAST")
val versionCodeProvider = rootProject.extra["versionCodeProvider"] as Provider<String>
@Suppress("UNCHECKED_CAST")
val versionNameProvider = rootProject.extra["versionNameProvider"] as Provider<String>

/**
 * Whoever is building this framework, which is who a module is loaded by.
 *
 * Included in another build -- a patcher that ships this code inside an app rather than a daemon --
 * the framework a module answers to is that build's, so its name is the one reported. The version
 * already resolves that way, being read from the repository the build was invoked in.
 */
// Reported to modules through `XposedInterface.getFrameworkName()`, which means it is a string
// living in the framework dex and readable from any process the framework is injected into. It used
// to be the Gradle project name, so a renamed checkout leaked its new name straight into the app
// process. A neutral constant, on purpose: nothing in the loader keys off this value, and modules
// that want to know the flavour read VERSION_NAME instead.
val frameworkName: String = "framework"

plugins {
    alias(libs.plugins.agp.lib)
    alias(libs.plugins.ktfmt)
}

ktfmt { kotlinLangStyle() }

android {
    namespace = "org.matrix.vector.impl"

    androidResources { enable = false }

    defaultConfig {
        consumerProguardFiles("consumer-rules.pro")
        buildConfigField("String", "FRAMEWORK_NAME", """"${frameworkName}"""")
        buildConfigField("String", "VERSION_NAME", """"${versionNameProvider.get()}"""")
        buildConfigField("long", "VERSION_CODE", versionCodeProvider.get())
    }

    sourceSets {
        named("main") {
            java.directories.addAll(listOf("src/main/kotlin", "libxposed/api/src/main/java"))
        }
    }
}

dependencies {
    implementation(projects.external.axml)
    implementation(projects.hiddenapi.bridge)
    implementation(projects.services.daemonService)
    compileOnly(libs.androidx.annotation)
    compileOnly(libs.libxposed.annotation)
    compileOnly(projects.hiddenapi.stubs)
}
