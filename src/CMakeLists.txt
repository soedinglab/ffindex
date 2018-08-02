cmake_minimum_required(VERSION 2.8)
set(CMAKE_C_FLAGS "-std=c99 ${CMAKE_C_FLAGS}")

# sets HAVE_FMEMOPEN
add_subdirectory(ext)

# Adapted from https://stackoverflow.com/questions/2152077/is-it-possible-to-get-cmake-to-build-both-a-static-and-shared-version-of-the-sam

# this is the "object library" target: compiles the sources only once
add_library(objlib OBJECT ffindex.c ffutil.c)
target_include_directories (objlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# shared libraries need PIC
set_property(TARGET objlib PROPERTY POSITION_INDEPENDENT_CODE 1)

# shared and static libraries built from the same object files
add_library(ffindex-static STATIC $<TARGET_OBJECTS:objlib>)
add_library(ffindex-shared SHARED $<TARGET_OBJECTS:objlib>)

set_target_properties(ffindex-static PROPERTIES OUTPUT_NAME ffindex)

set_target_properties(ffindex-shared PROPERTIES VERSION 0.9.9.7 
                                                SOVERSION 0
                                                OUTPUT_NAME ffindex)


if(NOT HAVE_FMEMOPEN)
#        target_link_libraries(ffindex ext)
        target_link_libraries(ffindex-shared ext)
endif()



add_executable(ffindex_apply
        ffindex_apply_mpi.c
)
target_link_libraries (ffindex_apply ffindex-shared)
set_property(TARGET ffindex_apply PROPERTY COMPILE_FLAGS "-UHAVE_MPI")

find_package(MPI)
if(MPI_C_FOUND AND HAVE_MPI)
    add_executable(ffindex_apply_mpi
      ffindex_apply_mpi.c
    )
    target_link_libraries (ffindex_apply_mpi ffindex-shared)
    add_subdirectory(mpq)

    set_property(TARGET ffindex_apply_mpi PROPERTY COMPILE_FLAGS "-DHAVE_MPI=1 ${MPI_C_COMPILE_FLAGS}")
    set_property(TARGET ffindex_apply_mpi PROPERTY LINK_FLAGS "${MPI_C_LINK_FLAGS}")

    target_include_directories(ffindex_apply_mpi PUBLIC ${MPI_C_INCLUDE_PATH})
    target_link_libraries (ffindex_apply_mpi mpq ${MPI_C_LIBRARIES})
    install(TARGETS
        ffindex_apply_mpi
        DESTINATION bin
    )
endif()


add_executable(ffindex_reduce
        ffindex_reduce.c
)
target_link_libraries (ffindex_reduce ffindex-shared)

add_executable(ffindex_build
  ffindex_build.c
)
target_link_libraries (ffindex_build ffindex-shared)


add_executable(ffindex_from_fasta
  ffindex_from_fasta.c
)
target_link_libraries (ffindex_from_fasta ffindex-shared)


add_executable(ffindex_get
  ffindex_get.c
)
target_link_libraries (ffindex_get ffindex-shared)


add_executable(ffindex_modify
  ffindex_modify.c
)
target_link_libraries (ffindex_modify ffindex-shared)


add_executable(ffindex_unpack
  ffindex_unpack.c
)
target_link_libraries (ffindex_unpack ffindex-shared)


add_executable(ffindex_order
  ffindex_order.c
)
target_link_libraries (ffindex_order ffindex-shared)


add_executable(ffindex_from_fasta_with_split
    ffindex_from_fasta_with_split.c
)
target_link_libraries (ffindex_from_fasta_with_split ffindex-shared)


install(PROGRAMS 
        ffindex.h 
        ffutil.h
        DESTINATION include
)

install(TARGETS ffindex-static
  LIBRARY DESTINATION lib COMPONENT libraries
        ARCHIVE DESTINATION lib COMPONENT libraries
)


install(TARGETS ffindex-shared
  LIBRARY DESTINATION lib COMPONENT libraries
        ARCHIVE DESTINATION lib COMPONENT libraries
)


INSTALL( 
  TARGETS ffindex_reduce
  ffindex_apply
  ffindex_build
  ffindex_from_fasta
  ffindex_get
  ffindex_modify
  ffindex_unpack
  ffindex_order
  ffindex_from_fasta_with_split
  DESTINATION bin
)
