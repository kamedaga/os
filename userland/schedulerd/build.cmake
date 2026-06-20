cmake_minimum_required(VERSION 3.16)

get_filename_component(SCHEDULERD_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(REPO_ROOT "${SCHEDULERD_DIR}/../.." ABSOLUTE)
set(BUILD_DIR "${REPO_ROOT}/.artifacts/cmake/schedulerd")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${SCHEDULERD_DIR}" -B "${BUILD_DIR}"
  WORKING_DIRECTORY "${REPO_ROOT}"
  RESULT_VARIABLE configure_status
)
if(NOT configure_status EQUAL 0)
  message(FATAL_ERROR "schedulerd configure failed: ${configure_status}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target schedulerd
  WORKING_DIRECTORY "${REPO_ROOT}"
  RESULT_VARIABLE build_status
)
if(NOT build_status EQUAL 0)
  message(FATAL_ERROR "schedulerd build failed: ${build_status}")
endif()
