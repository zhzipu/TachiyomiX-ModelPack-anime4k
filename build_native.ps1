# 编译 libmodelpack.so（仅 acnet / anime4k 需要）
# 前置：安装 Android SDK（含 CMake 与 NDK），并设置环境变量：
#   ANDROID_HOME      —— Android SDK 根目录
#   ANDROID_NDK_HOME  —— NDK 根目录
$ErrorActionPreference = "Stop"

$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { $env:ANDROID_SDK_ROOT }
$ndk = if ($env:ANDROID_NDK_HOME) { $env:ANDROID_NDK_HOME } else { $env:ANDROID_NDK_ROOT }
if (-not $sdk) { throw "未设置 ANDROID_HOME" }
if (-not $ndk) { throw "未设置 ANDROID_NDK_HOME" }

$cmake = Get-ChildItem "$sdk\cmake\*\bin\cmake.exe" | Sort-Object Name -Descending | Select-Object -First 1
$ninja = Get-ChildItem "$sdk\cmake\*\bin\ninja.exe" | Sort-Object Name -Descending | Select-Object -First 1
if (-not $cmake -or -not $ninja) { throw "未找到 SDK 中的 CMake/Ninja，请在 SDK Manager 安装" }
$toolchain = "$ndk\build\cmake\android.toolchain.cmake"

$src   = Join-Path $PSScriptRoot "src\main\cpp"
$build = Join-Path $PSScriptRoot "build\native\arm64-v8a"
$out   = Join-Path $PSScriptRoot "src\main\jniLibs\arm64-v8a"

New-Item -ItemType Directory -Force -Path $build, $out | Out-Null

& $cmake.FullName -S $src -B $build -G Ninja 