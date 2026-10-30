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
