cmake_minimum_required(VERSION 3.16)

project(kobox_storage C)

# koboxd is no longer a boot/service executable. The storage runtime sources in
# this directory are linked directly into filed.
