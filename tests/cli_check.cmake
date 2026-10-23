# Runs kestrel_app and checks how it ends. Variables (pass with -D):
#   APP         path to kestrel_app
#   ARGS        arguments, ';'-separated
#   EXPECT_RC   exit code it must return
#   EXPECT_MSG  text that the output (stdout or stderr) must contain; usage
#               errors are printed by the parser to stderr, the rest are logged
#               to stdout
execute_process(
  COMMAND ${APP} ${ARGS}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)
if(NOT rc STREQUAL "${EXPECT_RC}")
  message(FATAL_ERROR "exit code ${rc}, expected ${EXPECT_RC}\nstdout: ${out}\nstderr: ${err}")
endif()
string(FIND "${out}${err}" "${EXPECT_MSG}" found)
if(found EQUAL -1)
  message(FATAL_ERROR "output does not contain '${EXPECT_MSG}'\nstdout: ${out}\nstderr: ${err}")
endif()
