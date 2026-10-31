plugins {
    alias(libs.plugins.agp.app)
}

android {
    namespace = "test.res"
    compileSdk = 37
    buildToolsVersion = rootProject.extra["androidBuildToolsVersion"] as String

    defaultConfig {
        applicationId = "test.res"
        minSdk = 27
        targetSdk = 37
        versionCode = 1
        versionName = "1.0"
    }

    // Two fixtures, one build: `legacy` packages assets/xposed_init (resources hook path),
    // `modern` packages META-INF/xposed/* (API 101+ loader path). Keeping them separate avoids
    // hand-editing packaging metadata between runs, which is exactly how the last verification
    // got confused about which loader it had exercised.
    flavorDimensions += "loader"
    productFlavors {
        create("legacy") { dimension = "loader" }
        create("modern") { dimension = "loader" }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }
}

dependencies {
    compileOnly(project(":legacy"))
    compileOnly(project(":hiddenapi:stubs"))
    compileOnly(project(":xposed"))
}
