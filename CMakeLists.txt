cmake_minimum_required(VERSION 3.19)
project(s265065_lab3_spo C)

set(CMAKE_C_STANDARD 11)

set(CMAKE_EXE_LINKER_FLAGS -lpthread)

add_executable(s265065_lab3_spo main.c server.c main.h terminal.c terminal.h client.c)