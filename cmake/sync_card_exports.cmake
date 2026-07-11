# Build-time sync of card models exported by sandbox/card_proto.py.
# Globs at build time (not configure time), so a fresh export is picked up
# by the very next compile without re-running cmake. No-op when the sandbox
# export dir is absent (CI).
if(NOT IS_DIRECTORY "${EXPORT_DIR}")
    return()
endif()

file(GLOB glbs "${EXPORT_DIR}/*_card.glb" "${EXPORT_DIR}/*_hex.glb")
foreach(glb IN LISTS glbs)
    get_filename_component(name "${glb}" NAME)
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${glb}" "${MODELS_DIR}/${name}")
endforeach()
