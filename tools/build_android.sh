#!/usr/bin/env bash
# 编 Android 库：默认、候选、讲话、双档（prebuilt/dual/<ABI>）。
# 需要 NDK（r26 以上；本仓库的预编译包用 r28）：ANDROID_NDK=/path/to/ndk tools/build_android.sh
set -eu; cd "$(dirname "$0")/.."; NDK=${ANDROID_NDK:?请设 ANDROID_NDK}
# 可传 default / v62b / talk / dual，仅重建所选档；无参数仍构建全部。
if [ "$#" -eq 0 ]; then set -- default v62b talk dual; fi
for profile in "$@"; do
  case $profile in
    default) M=src/models.cpp; SUF=""; OUT=prebuilt ;;
    v62b) M=src/models_v62b.cpp; SUF=-v62b; OUT=prebuilt/candidate-v62b ;;
    talk) M=src/models_talk.cpp; SUF=-talk; OUT=prebuilt/talk ;;
    dual) M=src/models_dual.cpp; SUF=-dual; OUT=prebuilt/dual ;;
    *) echo "未知档：$profile" >&2; exit 2 ;;
  esac
  for abi in arm64-v8a armeabi-v7a; do
    cmake -S . -B build-android-$abi$SUF -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=$abi -DANDROID_PLATFORM=android-21 \
      -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release -DAEC48_BUILD_CHECK=ON -DAEC48_MODELS=$M > /dev/null
    cmake --build build-android-$abi$SUF -j${AEC48_BUILD_JOBS:-2} > /dev/null
    mkdir -p $OUT/$abi; cp build-android-$abi$SUF/libaec48.so $OUT/$abi/
    echo "$profile $abi：$OUT/$abi/libaec48.so（$(stat -c %s $OUT/$abi/libaec48.so) 字节）"
  done
done
