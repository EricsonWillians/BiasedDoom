# Dear ImGui (vendored core sources, MIT license - see LICENSE.txt)
# Built as a static library; the engine consumes it through the backend-
# agnostic F2DDrawer overlay layer in src/common/imgui/bd_imgui.cpp.

make_release_only()

add_library( imgui STATIC
	imgui.cpp
	imgui_draw.cpp
	imgui_tables.cpp
	imgui_widgets.cpp
	imgui_demo.cpp
)

target_include_directories( imgui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} )

# The executable is built as PIE on Linux, so the static library must be PIC.
set_target_properties( imgui PROPERTIES POSITION_INDEPENDENT_CODE ON )

if( MSVC )
	target_compile_options( imgui PRIVATE /W0 )
else()
	target_compile_options( imgui PRIVATE -w )
endif()
