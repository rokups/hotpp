# ------------------------------------------------------------ Precompiled scripts
spp_add_scripts(TARGET pre_gen1 SCRIPTS player_v1.spp
                INCLUDES ${PROJECT_SOURCE_DIR}/tests/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS})
spp_add_scripts(TARGET pre_gen2 SPPC_ARGS --gen 2 SCRIPTS player_v2.spp AFTER pre_gen1
                INCLUDES ${PROJECT_SOURCE_DIR}/tests/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS})
spp_add_scripts(TARGET pre_native1 SCRIPTS native_std_v1.spp AFTER pre_gen2
                INCLUDES ${PROJECT_SOURCE_DIR}/tests/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS} ${PROJECT_SOURCE_DIR}/tests/engine/Util.h)
spp_add_scripts(TARGET pre_native2 SPPC_ARGS --gen 2 SCRIPTS native_std_v2.spp AFTER pre_native1
                INCLUDES ${PROJECT_SOURCE_DIR}/tests/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS} ${PROJECT_SOURCE_DIR}/tests/engine/Util.h)
spp_add_scripts(TARGET pre_handle1 SCRIPTS handle_v1.spp AFTER pre_native2
                INCLUDES ${PROJECT_SOURCE_DIR}/tests/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS} ${PROJECT_SOURCE_DIR}/tests/engine/Weapon.h)
spp_add_scripts(TARGET pre_handle2 SPPC_ARGS --gen 2 SCRIPTS handle_v2.spp AFTER pre_handle1
                INCLUDES ${PROJECT_SOURCE_DIR}/tests/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS} ${PROJECT_SOURCE_DIR}/tests/engine/Weapon.h)
spp_add_scripts(TARGET pre_handle3 SPPC_ARGS --gen 3 SCRIPTS handle_v1.spp AFTER pre_handle2
                INCLUDES ${PROJECT_SOURCE_DIR}/tests/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS} ${PROJECT_SOURCE_DIR}/tests/engine/Weapon.h)
spp_add_scripts(TARGET pre_cpp1 SCRIPTS cpp_player_v1.cpp AFTER pre_handle3
                INCLUDES ${PROJECT_SOURCE_DIR}/tests/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS})

# The .cpp scripts are standard C++. A normal build compiles them without sppc.
# Each generation has its own library, because both define Player and the module variable total.
# Generation 1 has no hot-reload aids. It needs no spp include directory.
add_library(cpp_scripts_plain1 OBJECT cpp_player_v1.cpp)
target_include_directories(cpp_scripts_plain1 PRIVATE ${PROJECT_SOURCE_DIR}/tests/engine)
target_compile_features(cpp_scripts_plain1 PRIVATE cxx_std_17)

# Generation 2 still has the aids SPP_ALIAS and onReload. A normal build needs the header for them.
# The option -include works in GCC and in Clang. SHELL: keeps the two words together.
add_library(cpp_scripts_plain2 OBJECT cpp_player_v2.cpp)
target_include_directories(cpp_scripts_plain2 PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/engine)
target_compile_options(cpp_scripts_plain2 PRIVATE "SHELL:-include spp/script.h")
target_compile_features(cpp_scripts_plain2 PRIVATE cxx_std_17)
