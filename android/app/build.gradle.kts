plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}
android {
    namespace = "uk.kherve.kmusic"
    compileSdk = 34
    defaultConfig {
        applicationId = "uk.kherve.kmusic"
        minSdk = 26
        targetSdk = 34
        versionCode = 5
        versionName = "1.4"
    }
    buildTypes { release { isMinifyEnabled = false; signingConfig = signingConfigs.getByName("debug") } }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
    kotlinOptions { jvmTarget = "17" }
}
