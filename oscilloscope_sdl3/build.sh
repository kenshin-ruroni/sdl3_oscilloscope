#!/bin/bash

current_date_time=`date +"%Y-%m-%d %H:%M:%S"`;
echo $current_date_time " starting building application in " $(pwd)


if  test -d build; then
  cd build

  rm -rf CMakesFiles
  if [ -f "cmake_install.cmake" ]; then
    rm cmake_install.cmake
  fi
  if [ -f "CMakeCache.txt" ]; then
    rm CMakeCache.txt
  fi
  cd ../
fi


cmake -DCMAKE_BUILD_TYPE=Debug  -S . -B build



cmake --build build  --verbose
current_date_time=`date +"%Y-%m-%d %H:%M:%S"`;
echo $current_date_time "building application ended " $(pwd)