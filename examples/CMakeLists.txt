add_executable(qtzpl_example basic.cpp)
target_link_libraries(qtzpl_example PRIVATE QtZpl::QtZpl)

add_executable(qtzpl_gallery gallery.cpp)
target_link_libraries(qtzpl_gallery PRIVATE QtZpl::QtZpl)

add_executable(qtzpl_render render.cpp)
target_link_libraries(qtzpl_render PRIVATE QtZpl::QtZpl)
