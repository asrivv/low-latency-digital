add_executable(loadgen loadgen.cpp)
target_link_libraries(loadgen PRIVATE lle)
target_compile_options(loadgen PRIVATE ${LLE_WARNINGS})

add_executable(itch_listener itch_listener.cpp)
target_link_libraries(itch_listener PRIVATE lle)
target_compile_options(itch_listener PRIVATE ${LLE_WARNINGS})
