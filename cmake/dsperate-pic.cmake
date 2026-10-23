# Included right after DSperate's project() (CMAKE_PROJECT_INCLUDE, build.sh).
# A shared library needs PIC, and PIC alone costs DSperate's PGO profile ~270
# hot functions: GCC treats every function of PIC code as interposable and
# inlines differently, so their control flow no longer matches the profile.
# No function is interposed here - the core exports only retro_* - so
# -fno-semantic-interposition tells GCC as much and the profile applies whole.
# Set here rather than in CMAKE_CXX_FLAGS, which DSperate's profile
# fingerprint hashes.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-fno-semantic-interposition)
