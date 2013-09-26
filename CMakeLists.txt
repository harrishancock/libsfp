cmake_minimum_required(VERSION 2.8.10)
project(LIBSFP)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall --std=c++11")

add_library(sfp STATIC serial_framing_protocol.cc)

