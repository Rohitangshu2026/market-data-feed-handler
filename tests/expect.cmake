# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Rohitangshu Bose
#
# Runs one program and checks how it ended: the end-to-end tests.
#
#   cmake -DSTATUS=<exit status> -DPATTERN=<regex> -DABSENT=<regex> [-DINPUT=<file>]
#         -P expect.cmake -- <program> <args>...
#
# Passes if the program exits with STATUS, its output (stdout and stderr
# together) matches PATTERN, and nothing in it matches ABSENT. With INPUT, the
# program reads that file as its standard input: the answers to its questions.

set(command "")
set(after_dashes FALSE)
math(EXPR last "${CMAKE_ARGC} - 1")
foreach(i RANGE ${last})
  if(after_dashes)
    list(APPEND command "${CMAKE_ARGV${i}}")
  elseif("${CMAKE_ARGV${i}}" STREQUAL "--")
    set(after_dashes TRUE)
  endif()
endforeach()

if(DEFINED INPUT)
  set(input INPUT_FILE "${INPUT}")
endif()
execute_process(COMMAND ${command} ${input} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE output)
string(REPLACE ";" " " shown "${command}")
if(NOT status STREQUAL STATUS)
  message(FATAL_ERROR "${shown}\nexited with ${status}, not ${STATUS}; it printed:\n${output}")
endif()
if(NOT output MATCHES "${PATTERN}")
  message(FATAL_ERROR "${shown}\nprinted nothing matching \"${PATTERN}\"; it printed:\n${output}")
endif()
if(NOT ABSENT STREQUAL "" AND output MATCHES "${ABSENT}")
  message(FATAL_ERROR "${shown}\nprinted something matching \"${ABSENT}\":\n${output}")
endif()
