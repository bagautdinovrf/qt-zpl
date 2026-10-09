add_executable(qtzpl_example basic.cpp)
target_link_libraries(qtzpl_example PRIVATE QtZpl::QtZpl)

add_executable(qtzpl_inspect inspect.cpp)
target_link_libraries(qtzpl_inspect PRIVATE QtZpl::QtZpl)

add_executable(qtzpl_gallery gallery.cpp)
target_link_libraries(qtzpl_gallery PRIVATE QtZpl::QtZpl)

add_executable(qtzpl_render render.cpp)
target_link_libraries(qtzpl_render PRIVATE QtZpl::QtZpl)

add_executable(qtzpl_compare_golden compare_golden.cpp)
target_link_libraries(qtzpl_compare_golden PRIVATE QtZpl::QtZpl)

# Offline font research only; does not expose another library API or runtime.
add_executable(qtzpl_font0_probe ../tools/font0_probe.cpp)
target_link_libraries(qtzpl_font0_probe PRIVATE Qt6::Core Qt6::Gui ${QTZPL_FREETYPE_TARGETS})
