plugins {
    id("com.android.application") version "9.2.1"
}

android {
    namespace = "com.tachiyomix.modelpack.anime4k"
    compileSdk = 37

    defaultConfig {
        applicationId = "com.tachiyomix.modelpack.anime4k"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "1.0.0"
        manifestPlaceholders["modelpackId"] = "anime4k"
        manifestPlaceholders["modelpackName"] = "Anime4K 模型"
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }

    lint {
        abortOnError = false
        checkReleaseBuilds = false
    }
}
