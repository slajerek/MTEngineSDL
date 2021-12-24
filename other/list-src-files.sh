#!/bin/sh
find . -type d > folders.inc
find . -type f -name "*.c" > c-files.inc
find . -type f -name "*.cpp" > cpp-files.inc
