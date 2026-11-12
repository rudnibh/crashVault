# CMake generated Testfile for 
# Source directory: /home/rudnibh/Projects/crashvault
# Build directory: /home/rudnibh/Projects/crashvault/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(unit_phase1 "/home/rudnibh/Projects/crashvault/build/test_phase1")
set_tests_properties(unit_phase1 PROPERTIES  _BACKTRACE_TRIPLES "/home/rudnibh/Projects/crashvault/CMakeLists.txt;173;add_test;/home/rudnibh/Projects/crashvault/CMakeLists.txt;0;")
add_test(integration_watcher_phase1 "/usr/bin/python3" "/home/rudnibh/Projects/crashvault/tests/integration/test_watcher_phase1.py" "/home/rudnibh/Projects/crashvault/build/crashvaultd")
set_tests_properties(integration_watcher_phase1 PROPERTIES  TIMEOUT "60" WORKING_DIRECTORY "/home/rudnibh/Projects/crashvault/build" _BACKTRACE_TRIPLES "/home/rudnibh/Projects/crashvault/CMakeLists.txt;178;add_test;/home/rudnibh/Projects/crashvault/CMakeLists.txt;0;")
subdirs("_deps/doctest-build")
subdirs("_deps/nlohmann_json-build")
