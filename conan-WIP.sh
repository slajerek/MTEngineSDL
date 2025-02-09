#!/bin/sh
mkdir build
cd build
conan install .. --build=missing
