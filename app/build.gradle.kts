import java.util.zip.ZipFile

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

val githubClientId = providers.gradleProperty("ABK_GITHUB_CLIENT_ID")
    .orElse(providers.environmentVariable("ABK_GITHUB_CLIENT_ID"))
    .orElse("Ov23li8skGo6AFPBeSTh")
val appVersionCode = 10025
val appVersionName = "1.2.5"
val appUpdateMetadataUrl = providers.environmentVariable("ABK_APP_UPDATE_METADATA_URL")
    .orElse("https://raw.githubusercontent.com/xingguangcuican6666/ABK/dev/version.json")
val appBuildTimestamp = providers.environmentVariable("ABK_APP_BUILD_TIMESTAMP")
    .orElse("")
val appBuildTimestampEpochMillis = providers.environmentVariable("ABK_APP_BUILD_TIMESTAMP_EPOCH_MILLIS")
    .map { raw -> raw.toLongOrNull()?.let { "${it}L" } ?: "0L" }
    .orElse("0L")
val appBuildRunId = providers.environmentVariable("ABK_APP_BUILD_RUN_ID")
    .map { raw -> raw.toLongOrNull()?.let { "${it}L" } ?: "0L" }
    .orElse("0L")
val appBuildCommitSha = providers.environmentVariable("ABK_APP_BUILD_COMMIT_SHA")
    .orElse("")
val appBuildWorkflowName = providers.environmentVariable("ABK_APP_BUILD_WORKFLOW_NAME")
    .orElse("")

val releaseStoreFile = providers.gradleProperty("ABK_RELEASE_STORE_FILE")
    .orElse(providers.environmentVariable("ABK_RELEASE_STORE_FILE"))
val releaseStorePassword = providers.gradleProperty("ABK_RELEASE_STORE_PASSWORD")
    .orElse(providers.environmentVariable("ABK_RELEASE_STORE_PASSWORD"))
val releaseStoreType = providers.gradleProperty("ABK_RELEASE_STORE_TYPE")
    .orElse(providers.environmentVariable("ABK_RELEASE_STORE_TYPE"))
    .orElse("JKS")
val releaseKeyAlias = providers.gradleProperty("ABK_RELEASE_KEY_ALIAS")
    .orElse(providers.environmentVariable("ABK_RELEASE_KEY_ALIAS"))
val releaseKeyPassword = providers.gradleProperty("ABK_RELEASE_KEY_PASSWORD")
    .orElse(providers.environmentVariable("ABK_RELEASE_KEY_PASSWORD"))
val hasReleaseSigning = !releaseStoreFile.orNull.isNullOrBlank() &&
    !releaseStorePassword.orNull.isNullOrBlank() &&
    !releaseKeyAlias.orNull.isNullOrBlank() &&
    !releaseKeyPassword.orNull.isNullOrBlank()
val libsodiumAarCoordinate = "com.goterl:lazysodium-android:5.2.0@aar"

val extractLibsodium by tasks.registering {
    val outputDir = layout.buildDirectory.dir("generated/abk-jniLibs/main/libsodium")
    outputs.dir(outputDir)
    doLast {
        val jniRoot = outputDir.get().asFile
        jniRoot.deleteRecursively()
        jniRoot.mkdirs()

        val lazysodium = configurations.detachedConfiguration(
            dependencies.create(libsodiumAarCoordinate)
        ).singleFile
        ZipFile(lazysodium).use { zip ->
            val entries = zip.entries()
            while (entries.hasMoreElements()) {
                val entry = entries.nextElement()
                if (entry.name.startsWith("jni/") && entry.name.endsWith("/libsodium.so")) {
                    val relative = entry.name.removePrefix("jni/")
                    val target = jniRoot.resolve(relative)
                    target.parentFile?.mkdirs()
                    zip.getInputStream(entry).use { input ->
                        target.outputStream().use { output -> input.copyTo(output) }
                    }
                }
            }
        }
    }
}

android {
    namespace = "com.abk.kernel"
    compileSdk = 37

    defaultConfig {
        applicationId = "com.abk.kernel"
        minSdk = 26
        targetSdk = 35
        versionCode = 10025
        versionName = "1.2.5"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        buildConfigField("long", "APP_VERSION_CODE", "${appVersionCode}L")
        buildConfigField("String", "APP_VERSION_NAME", "\"$appVersionName\"")
        buildConfigField("String", "APP_UPDATE_METADATA_URL", "\"${appUpdateMetadataUrl.get()}\"")
        buildConfigField("String", "APP_BUILD_TIMESTAMP", "\"${appBuildTimestamp.get()}\"")
        buildConfigField("long", "APP_BUILD_TIMESTAMP_EPOCH_MILLIS", appBuildTimestampEpochMillis.get())
        buildConfigField("long", "APP_BUILD_RUN_ID", appBuildRunId.get())
        buildConfigField("String", "APP_BUILD_COMMIT_SHA", "\"${appBuildCommitSha.get()}\"")
        buildConfigField("String", "APP_BUILD_WORKFLOW_NAME", "\"${appBuildWorkflowName.get()}\"")
        buildConfigField("String", "GITHUB_CLIENT_ID", "\"${githubClientId.get()}\"")
        buildConfigField("String", "SOURCE_REPO_OWNER", "\"xingguangcuican6666\"")
        buildConfigField("String", "SOURCE_REPO_NAME", "\"ABK\"")
        buildConfigField("String", "SOURCE_REPO_DEFAULT_BRANCH", "\"dev\"")
        buildConfigField("String", "UPSTREAM_REPO_URL", "\"https://github.com/zzh20188/GKI_KernelSU_SUSFS\"")
        buildConfigField("String", "TOP_LEVEL_REPO_URL", "\"https://github.com/WildKernels/GKI_KernelSU_SUSFS\"")

        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
            }
        }
    }

    ndkVersion = "28.2.13676358"

    signingConfigs {
        create("release") {
            storeFile = releaseStoreFile.orNull?.let { file(it) }
            storePassword = releaseStorePassword.orNull
            storeType = releaseStoreType.get()
            keyAlias = releaseKeyAlias.orNull
            keyPassword = releaseKeyPassword.orNull
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            if (hasReleaseSigning) {
                signingConfig = signingConfigs.getByName("release")
            }
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures {
        compose = true
        buildConfig = true
    }
    sourceSets {
        getByName("main") {
            assets.srcDir("build/generated/abk-assets/main")
            jniLibs.srcDir("build/generated/abk-jniLibs/main")
        }
    }
    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }
}

tasks.named("preBuild").configure {
    dependsOn(extractLibsodium)
}

tasks.matching { it.name.startsWith("configureCMake") || it.name.startsWith("buildCMake") }.configureEach {
    dependsOn(extractLibsodium)
}

dependencies {
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.7")
    implementation("androidx.lifecycle:lifecycle-process:2.8.7")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation(platform("androidx.compose:compose-bom:2026.05.00"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3:1.5.0-alpha19")
    implementation("androidx.compose.material:material-icons-extended")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.navigation:navigation-compose:2.8.5")

    // Network
    implementation("com.squareup.retrofit2:retrofit:2.11.0")
    implementation("com.squareup.retrofit2:converter-gson:2.11.0")
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("com.squareup.okhttp3:logging-interceptor:4.12.0")
    implementation("com.google.code.gson:gson:2.11.0")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.7.3")

    // Root
    implementation("com.github.topjohnwu.libsu:core:5.2.2")
    implementation("com.github.topjohnwu.libsu:io:5.2.2")

    // Image
    implementation("io.coil-kt:coil-compose:2.7.0")

    // Background work & notifications
    implementation("androidx.work:work-runtime-ktx:2.10.0")

    // Preferences
    implementation("androidx.datastore:datastore-preferences:1.1.2")

    testImplementation("junit:junit:4.13.2")
    testImplementation("org.robolectric:robolectric:4.14.1")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.10.2")
    testImplementation("androidx.test:core:1.6.1")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("androidx.test.espresso:espresso-core:3.6.1")
    androidTestImplementation(platform("androidx.compose:compose-bom:2026.05.00"))
    androidTestImplementation("androidx.compose.ui:ui-test-junit4")
    debugImplementation("androidx.compose.ui:ui-tooling")
    debugImplementation("androidx.compose.ui:ui-test-manifest")
}
