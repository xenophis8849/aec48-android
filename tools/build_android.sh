#!/usr/bin/env bash
# 编 Android 库：默认版 prebuilt/<ABI>/libaec48.so，候选版 prebuilt/candidate-v62b/<ABI>/libaec48.so（arm64-v8a、armeabi-v7a）；另编手机上跑的自检程序 build-android-<ABI>[-v62b]/aec48_check。
# 需要 NDK（r26 以上；本仓库的预编译包用 r28）：ANDROID_NDK=/path/to/ndk tools/build_android.sh
set -eu; cd "$(dirname "$0")/.."; NDK=${ANDROID_NDK:?请设 ANDROID_NDK}
for fl in default v62b; do
  if [ $fl = default ]; then M=src/models.cpp; SUF=""; OUT=prebuilt; else M=src/models_$fl.cpp; SUF=-$fl; OUT=prebuilt/candidate-$fl; fi
  for abi in arm64-v8a armeabi-v7a; do
    cmake -S . -B build-android-$abi$SUF -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=$abi -DANDROID_PLATFORM=android-21 \
      -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release -DAEC48_BUILD_CHECK=ON -DAEC48_MODELS=$M > /dev/null
    cmake --build build-android-$abi$SUF -j4 > /dev/null
    mkdir -p $OUT/$abi; cp build-android-$abi$SUF/libaec48.so $OUT/$abi/
    echo "$fl $abi：$OUT/$abi/libaec48.so（$(stat -c %s $OUT/$abi/libaec48.so) 字节）"
  done
done
