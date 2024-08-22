#!/bin/bash

set -e

g++ --version

g++ -O2 -DNDEBUG -std=c++20 code.cc
