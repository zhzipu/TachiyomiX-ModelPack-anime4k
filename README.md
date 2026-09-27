# TachiyomiX Model Pack — Anime4K 模型

[TachiyomiX](https://github.com/zhzipu/TachiyomiX) 的图像增强模型包插件（单模型，独立分发）。

## 信息

- 模型 ID：`anime4k`
- 包名：`com.tachiyomix.modelpack.anime4k`
- 说明：Anime4K 亮度边缘定向 2x 超分，自带原生推理库

## 构建

1. 编译原生库 `libmodelpack.so`（需安装 Android SDK + NDK + CMake，并设置 `ANDROID_HOME` / `ANDROID_NDK_HOME`）：

   ```powershell
   ./build_native.ps1
   ```

   生成 `src/main/jniLibs/arm64-v8a/libmodelpack.so`。

2. 打包 APK：

   ```bash
   ./gradlew assembleRelease
   ```

产物位于 `build/outputs/apk/release/`。

## 安装与使用

将构建出的 APK 安装到设备后，在 TachiyomiX 的「图像增强」设置中选择该模型即可。

## 许可证

模型文件与原生代码版权归其原作者所有，仅用于学习与研究，请遵循各模型自身的许可证。
