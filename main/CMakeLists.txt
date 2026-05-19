idf_component_register(SRCS "kryos_consensus_nodes.c"
                       PRIV_REQUIRES spi_flash esp_wifi nvs_flash esp_event esp_netif esp_timer mbedtls
                       INCLUDE_DIRS "")

if(DEFINED ENV{KRYOS_NODE_ID})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KRYOS_NODE_ID=$ENV{KRYOS_NODE_ID})
endif()

if(DEFINED ENV{KRYOS_NODE_ROLE_ROOT})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KRYOS_NODE_ROLE_ROOT=$ENV{KRYOS_NODE_ROLE_ROOT})
endif()

if(DEFINED ENV{KRYOS_NODE_ROLE_LEADER})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KRYOS_NODE_ROLE_LEADER=$ENV{KRYOS_NODE_ROLE_LEADER})
endif()

if(DEFINED ENV{KRYOS_ENABLE_ROOT_UPLINK})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KRYOS_ENABLE_ROOT_UPLINK=$ENV{KRYOS_ENABLE_ROOT_UPLINK})
endif()
